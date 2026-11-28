#include "lvgl_screenshot.h"

#if defined(USE_ESP32) && defined(USE_LVGL_SCREENSHOT)

#include <cinttypes>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::lvgl_screenshot {

ESPHOME_LOG_TAG(TAG, "lvgl_screenshot");

/// The esp32-camera software encoder uses about 2.3 kB of it.
static constexpr uint32_t ENCODE_TASK_STACK_SIZE = 6144;
/// The same as the main loop; on a dual-core chip the task mostly runs on the other core.
static constexpr UBaseType_t ENCODE_TASK_PRIORITY = 1;

void LvglScreenshot::setup() {
  lv_display_t *display = lv_display_get_default();
  if (display == nullptr) {
    this->mark_failed(LOG_STR("No LVGL display"));
    return;
  }
  this->width_ = lv_display_get_horizontal_resolution(display);
  this->height_ = lv_display_get_vertical_resolution(display);
  // LVGL's own allocation is aligned to LV_DRAW_BUF_ALIGN, which keeps the CPU cache in step when
  // LVGL fills parts of the screenshot with a 2D accelerator (the PPA on the ESP32-P4).
  this->draw_buf_ = lv_draw_buf_create(this->width_, this->height_, LV_COLOR_FORMAT_RGB565, this->width_ * 2);
  if (this->draw_buf_ == nullptr) {
    this->mark_failed(LOG_STR("Failed to allocate the screenshot buffer"));
    return;
  }
  // The encoder's output buffer starts small (4 kB for the default encoder) and grows a little per retry, and every
  // retry encodes the whole screen again. A JPEG of a screen rarely needs more than 2 bits per pixel.
  camera::EncoderBuffer *output = this->encoder_->get_output_buffer();
  const size_t expected_jpeg_size = static_cast<size_t>(this->width_) * this->height_ / 4;
  if (output->get_max_size() < expected_jpeg_size && !output->set_buffer_size(expected_jpeg_size)) {
    ESP_LOGW(TAG, "Could not reserve %zu bytes for the JPEG", expected_jpeg_size);
  }
  this->reusable_image_ = std::make_shared<ScreenshotImage>();
  if (xTaskCreate(LvglScreenshot::encode_task, "lvgl_screenshot", ENCODE_TASK_STACK_SIZE, this, ENCODE_TASK_PRIORITY,
                  &this->task_) != pdPASS) {
    this->mark_failed(LOG_STR("Failed to start the encoding task"));
    return;
  }
  this->disable_loop();
}

void LvglScreenshot::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LVGL Screenshot:\n"
                "  Size: %ux%u\n"
                "  Stream interval: %" PRIu32 " ms",
                this->width_, this->height_, this->stream_interval_);
  // The encoder is not a component, so nothing else logs its settings.
  this->encoder_->dump_config();
}

void LvglScreenshot::loop() {
  const CaptureState state = this->state_.load(std::memory_order_acquire);
  if (state == CaptureState::CAPTURE_STATE_BUSY)
    return;
  if (state == CaptureState::CAPTURE_STATE_DONE) {
    this->publish_();
    this->state_.store(CaptureState::CAPTURE_STATE_IDLE, std::memory_order_release);
  } else if (state == CaptureState::CAPTURE_STATE_FAILED) {
    this->state_.store(CaptureState::CAPTURE_STATE_IDLE, std::memory_order_release);
    ESP_LOGW(TAG, "Encoding failed: %s", camera::to_string(this->encode_result_));
    if (this->encode_result_ == camera::ENCODER_ERROR_CONFIGURATION) {
      // Stops further screenshots; see the check below.
      this->status_set_error(LOG_STR("The encoder cannot encode the screen"));
    } else {
      this->status_set_warning(LOG_STR("Encoding failed"));
    }
  }

  // Release the previous screenshot once every client has finished reading it, which frees the
  // encoder's output buffer for the next one.
  if (this->current_image_ && this->current_image_.use_count() == 2)
    this->current_image_.reset();

  const uint32_t now = App.get_loop_component_start_time();
  const bool stream_due = this->stream_requesters_ != 0 && now - this->last_stream_frame_ >= this->stream_interval_;
  const uint8_t requesters = this->single_requesters_ | (stream_due ? this->stream_requesters_ : 0);
  if (requesters == 0 || this->status_has_error()) {
    if (!this->current_image_ && this->stream_requesters_ == 0)
      this->disable_loop();
    return;
  }
  if (this->current_image_)
    return;

  this->single_requesters_ = 0;
  if (stream_due)
    this->last_stream_frame_ = now;
  if (!this->render_())
    return;
  this->pending_requesters_ = requesters;
  this->state_.store(CaptureState::CAPTURE_STATE_BUSY, std::memory_order_release);
  xTaskNotifyGive(this->task_);
}

bool LvglScreenshot::render_() {
  // LVGL is not thread-safe, so this stays in the main loop.
  const uint32_t start = millis();
#if LV_USE_SNAPSHOT  // Set in lv_conf.h for this component; static analysis builds LVGL without it.
  const lv_result_t result = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, this->draw_buf_);
#else
  const lv_result_t result = LV_RESULT_INVALID;
#endif
  this->render_ms_ = millis() - start;
  // The encoder takes the buffer as one block of rows, so the screen must fill it exactly.
  const lv_image_header_t &header = this->draw_buf_->header;
  if (result != LV_RESULT_OK || header.w != this->width_ || header.h != this->height_ ||
      header.stride != this->width_ * 2) {
    ESP_LOGW(TAG, "Failed to render the screen");
    this->status_set_warning(LOG_STR("Failed to render the screen"));
    return false;
  }
  return true;
}

void LvglScreenshot::encode_task(void *param) {
  auto *self = static_cast<LvglScreenshot *>(param);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    self->encode_();
    App.wake_loop_threadsafe();
  }
}

void LvglScreenshot::encode_() {
  const uint32_t start = millis();
  // LVGL renders RGB565 with the low byte first; camera encoders take the high byte first, as a
  // camera sends it.
  const size_t count = static_cast<size_t>(this->width_) * this->height_;
  auto *pixels = reinterpret_cast<uint16_t *>(this->draw_buf_->data);
  for (size_t i = 0; i < count; i++)
    pixels[i] = byteswap(pixels[i]);

  camera::CameraImageSpec spec{this->width_, this->height_, camera::PIXEL_FORMAT_RGB565};
  RawScreenshot raw(this->draw_buf_->data, count * 2);
  // The encoder asks for a retry after it grew its output buffer, or while it encodes in steps. It stops asking by
  // itself when the buffer cannot grow any further.
  camera::EncoderError result;
  while ((result = this->encoder_->encode_pixels(&spec, &raw)) == camera::ENCODER_ERROR_RETRY_FRAME)
    vTaskDelay(1);  // Lets the idle task run, which the task watchdog checks.
  this->encode_ms_ = millis() - start;
  this->encode_result_ = result;
  this->state_.store(
      result == camera::ENCODER_ERROR_SUCCESS ? CaptureState::CAPTURE_STATE_DONE : CaptureState::CAPTURE_STATE_FAILED,
      std::memory_order_release);
}

void LvglScreenshot::publish_() {
  camera::EncoderBuffer *jpeg = this->encoder_->get_output_buffer();
  this->reusable_image_->set_frame(jpeg->get_data(), jpeg->get_size(), this->pending_requesters_);
  this->current_image_ = this->reusable_image_;
  this->status_clear_warning();
  ESP_LOGV(TAG, "Screenshot %ux%u, %zu bytes (render %" PRIu32 " ms, encode %" PRIu32 " ms)", this->width_,
           this->height_, jpeg->get_size(), this->render_ms_, this->encode_ms_);
  for (uint8_t i = 0; i < this->listener_count_; i++)
    this->listeners_[i]->on_camera_image(this->current_image_);
}

void LvglScreenshot::add_listener(camera::CameraListener *listener) {
  uint16_t capacity;
  this->listeners_ = static_cast<camera::CameraListener **>(
      callback_manager_grow(this->listeners_, this->listener_count_, capacity, sizeof(camera::CameraListener *)));
  this->listeners_[this->listener_count_++] = listener;
}

void LvglScreenshot::request_image(camera::CameraRequester requester) {
  this->single_requesters_ |= (1U << requester);
  this->enable_loop();
}

void LvglScreenshot::start_stream(camera::CameraRequester requester) {
  this->stream_requesters_ |= (1U << requester);
  this->enable_loop();
  this->defer([this]() {
    for (uint8_t i = 0; i < this->listener_count_; i++)
      this->listeners_[i]->on_stream_start();
  });
}

void LvglScreenshot::stop_stream(camera::CameraRequester requester) {
  this->stream_requesters_ &= ~(1U << requester);
  this->defer([this]() {
    for (uint8_t i = 0; i < this->listener_count_; i++)
      this->listeners_[i]->on_stream_stop();
  });
}

void ScreenshotImageReader::set_image(std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::move(image);
  this->offset_ = 0;
}

size_t ScreenshotImageReader::available() const {
  if (!this->image_)
    return 0;
  return this->image_->get_data_length() - this->offset_;
}

uint8_t *ScreenshotImageReader::peek_data_buffer() { return this->image_->get_data_buffer() + this->offset_; }

void ScreenshotImageReader::consume_data(size_t consumed) { this->offset_ += consumed; }

void ScreenshotImageReader::return_image() { this->image_.reset(); }

}  // namespace esphome::lvgl_screenshot

#endif
