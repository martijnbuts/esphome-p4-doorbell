#include "lvgl_screenshot.h"

#ifdef USE_ESP32

#include <esp_heap_caps.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::lvgl_screenshot {

static const char *const TAG = "lvgl_screenshot";

#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
// Far longer than the hardware needs for 1024x600 (a few ms). This only bounds the encode itself:
// the driver first takes the codec mutex without a time limit, so when a hardware JPEG decode
// (same engine) is running, the encoder waits for it to finish.
static constexpr int ENCODER_TIMEOUT_MS = 500;
#endif

#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
// The esp32-camera encoder keeps about 1.2 kB on the stack; the log reports what is left.
static constexpr uint32_t TASK_STACK_SIZE = 6144;
// Encoding attempts per screenshot; each failed one grows the encoder's output buffer by half.
static constexpr int MAX_ENCODE_ATTEMPTS = 6;
#endif

void LvglScreenshot::setup() {
  // Buffers, the hardware encoder and the software encoding task are all created on the first
  // request, so a display nobody looks at costs no memory.
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  if (this->encoder_ == nullptr) {
    ESP_LOGE(TAG, "No JPEG encoder");
    this->mark_failed();
  }
#endif
}

void LvglScreenshot::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LVGL screenshot camera:\n"
                "  Stream interval: %" PRIu32 " ms",
                this->stream_interval_);
#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  ESP_LOGCONFIG(TAG,
                "  Encoder: hardware JPEG (ESP32-P4)\n"
                "  JPEG quality: %u",
                this->quality_);
#endif
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  ESP_LOGCONFIG(TAG,
                "  Encoder: camera_encoder, in its own task\n"
                "  Task priority: %u",
                this->task_priority_);
  // camera_encoder is not a Component, so nobody else logs its settings
  if (this->encoder_ != nullptr)
    this->encoder_->dump_config();
#endif
}

void LvglScreenshot::request_image(camera::CameraRequester requester) { this->single_requesters_ |= (1U << requester); }

void LvglScreenshot::start_stream(camera::CameraRequester requester) {
  this->stream_requesters_ |= (1U << requester);
  for (auto *listener : this->listeners_)
    listener->on_stream_start();
}

void LvglScreenshot::stop_stream(camera::CameraRequester requester) {
  this->stream_requesters_ &= ~(1U << requester);
  for (auto *listener : this->listeners_)
    listener->on_stream_stop();
}

void LvglScreenshot::loop() {
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  // Collect a finished software encode first.
  const uint8_t state = this->sw_state_.load(std::memory_order_acquire);
  if (state == SW_BUSY)
    return;
  if (state == SW_DONE) {
    camera::EncoderBuffer *out = this->encoder_->get_output_buffer();
    this->publish_(out->get_data(), out->get_size(), this->sw_requesters_, this->sw_render_ms_, this->sw_encode_ms_);
    this->sw_state_.store(SW_IDLE, std::memory_order_release);
  } else if (state == SW_FAILED) {
    ESP_LOGE(TAG, "JPEG encoding failed: %s", this->sw_error_ != nullptr ? this->sw_error_ : "unknown");
    this->sw_state_.store(SW_IDLE, std::memory_order_release);
  }
#endif

  uint8_t requesters = this->single_requesters_;
  const uint32_t now = millis();
  const bool stream_due = this->stream_requesters_ != 0 && now - this->last_stream_frame_ >= this->stream_interval_;
  if (stream_due)
    requesters |= this->stream_requesters_;
  if (requesters == 0)
    return;
  // Wait until every client has released the previous screenshot: it points into the output
  // buffer that the next one is written to.
  if (this->current_image_ && this->current_image_.use_count() > 1)
    return;
  this->current_image_.reset();
  this->single_requesters_ = 0;
  if (stream_due)
    this->last_stream_frame_ = now;
  this->capture_(requesters);
}

bool LvglScreenshot::prepare_(uint32_t width, uint32_t height) {
#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  if (this->hw_encoder_ == nullptr) {
    jpeg_encode_engine_cfg_t engine_cfg = {};
    engine_cfg.intr_priority = 0;
    engine_cfg.timeout_ms = ENCODER_TIMEOUT_MS;
    if (jpeg_new_encoder_engine(&engine_cfg, &this->hw_encoder_) != ESP_OK) {
      this->hw_encoder_ = nullptr;
      ESP_LOGE(TAG, "Could not create the JPEG encoder");
      return false;
    }
  }
#endif
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  if (!this->start_task_())
    return false;
#endif
  if (this->raw_buf_ != nullptr && width == this->width_ && height == this->height_)
    return true;

  // New (or first) screen size: (re)allocate the picture buffer(s).
  const uint32_t stride = width * 2;
  heap_caps_free(this->raw_buf_);
  this->raw_buf_ = nullptr;
  this->raw_size_ = 0;
#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  // The encoder reads the raw picture and writes the JPEG with 2D-DMA, so both come from
  // jpeg_alloc_encoder_mem (cache-line aligned PSRAM).
  heap_caps_free(this->out_buf_);
  this->out_buf_ = nullptr;
  jpeg_encode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
  jpeg_encode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  // A JPEG of a dashboard is a fraction of the raw size; half of it leaves ample room even for
  // a photo, and the encoder reports an error rather than overflowing.
  this->raw_buf_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(stride * height, &in_cfg, &this->raw_size_));
  this->out_buf_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(stride * height / 2, &out_cfg, &this->out_size_));
  if (this->raw_buf_ == nullptr || this->out_buf_ == nullptr) {
    heap_caps_free(this->out_buf_);
    this->out_buf_ = nullptr;
    heap_caps_free(this->raw_buf_);
    this->raw_buf_ = nullptr;
  }
#else
  // PSRAM when there is any, internal RAM otherwise (small screens on chips without PSRAM)
  this->raw_buf_ = static_cast<uint8_t *>(heap_caps_malloc(stride * height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->raw_buf_ == nullptr)
    this->raw_buf_ = static_cast<uint8_t *>(heap_caps_malloc(stride * height, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (this->raw_buf_ != nullptr)
    this->raw_size_ = stride * height;
#endif
  if (this->raw_buf_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the screenshot buffers (%" PRIu32 "x%" PRIu32 ")", width, height);
    return false;
  }
  lv_draw_buf_init(&this->snapshot_buf_, width, height, LV_COLOR_FORMAT_RGB565, stride, this->raw_buf_,
                   this->raw_size_);
  this->width_ = width;
  this->height_ = height;
  return true;
}

/// Render the active LVGL screen into raw_buf_ (RGB565, little endian). Main loop only:
/// LVGL is not thread-safe.
bool LvglScreenshot::render_() {
  lv_obj_t *screen = lv_screen_active();
  if (screen == nullptr) {
    ESP_LOGW(TAG, "No active LVGL screen");
    return false;
  }
  // Size of the screen. lv_snapshot also adds the extra draw size (shadow/outline), which a
  // screen does not have; if it ever does, the snapshot below fails instead of overflowing.
  lv_obj_update_layout(screen);
  const int32_t w = lv_obj_get_width(screen);
  const int32_t h = lv_obj_get_height(screen);
  if (w <= 0 || h <= 0) {
    ESP_LOGE(TAG, "Invalid screen size %" PRId32 "x%" PRId32, w, h);
    return false;
  }
#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  // 4:4:4 encodes in 8x8 blocks
  if (w % 8 != 0 || h % 8 != 0) {
    ESP_LOGE(TAG, "Screen size %" PRId32 "x%" PRId32 " is not a multiple of 8", w, h);
    return false;
  }
#endif
  if (!this->prepare_(w, h))
    return false;
  if (lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, &this->snapshot_buf_) != LV_RESULT_OK) {
    ESP_LOGE(TAG, "lv_snapshot failed");
    return false;
  }
  if (this->snapshot_buf_.header.stride != this->width_ * 2) {
    ESP_LOGE(TAG, "Unexpected stride %" PRIu32 " for width %" PRIu32, (uint32_t) this->snapshot_buf_.header.stride,
             this->width_);
    return false;
  }
  return true;
}

bool LvglScreenshot::capture_(uint8_t requesters) {
  const uint32_t start = millis();
  if (!this->render_())
    return false;
  const uint32_t rendered = millis();

#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  // Encode in hardware. 4:4:4 keeps small coloured text sharp. The driver writes the cache
  // back to PSRAM before the DMA reads the picture.
  jpeg_encode_cfg_t cfg = {};
  cfg.width = this->width_;
  cfg.height = this->height_;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV444;
  cfg.image_quality = this->quality_;
  uint32_t jpeg_len = 0;
  const esp_err_t err = jpeg_encoder_process(this->hw_encoder_, &cfg, this->raw_buf_, this->width_ * this->height_ * 2,
                                             this->out_buf_, this->out_size_, &jpeg_len);
  if (err != ESP_OK || jpeg_len == 0) {
    ESP_LOGE(TAG, "JPEG encoding failed: %s", esp_err_to_name(err));
    return false;
  }
  this->publish_(this->out_buf_, jpeg_len, requesters, rendered - start, millis() - rendered);
  return true;
#endif

#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  // Hand the picture to the encoding task; loop() publishes the result.
  this->sw_requesters_ = requesters;
  this->sw_render_ms_ = rendered - start;
  this->sw_error_ = nullptr;
  this->sw_state_.store(SW_BUSY, std::memory_order_release);
  xTaskNotifyGive(this->task_);
  return true;
#endif
}

void LvglScreenshot::publish_(uint8_t *jpeg, size_t jpeg_len, uint8_t requesters, uint32_t render_ms,
                              uint32_t encode_ms) {
  this->current_image_ = std::make_shared<ScreenshotImage>(jpeg, jpeg_len, requesters);
  // The first screenshot after boot at INFO, the rest at DEBUG
  if (this->first_done_) {
    ESP_LOGD(TAG, "Screenshot %" PRIu32 "x%" PRIu32 ", %zu bytes (render %" PRIu32 " ms, jpeg %" PRIu32 " ms)",
             this->width_, this->height_, jpeg_len, render_ms, encode_ms);
  } else {
    ESP_LOGI(TAG, "First screenshot %" PRIu32 "x%" PRIu32 ", %zu bytes (render %" PRIu32 " ms, jpeg %" PRIu32 " ms)",
             this->width_, this->height_, jpeg_len, render_ms, encode_ms);
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
    ESP_LOGI(TAG, "Encoding task: %" PRIu32 " of %" PRIu32 " stack bytes never used", this->sw_stack_free_,
             TASK_STACK_SIZE);
#endif
    this->first_done_ = true;
  }
  for (auto *listener : this->listeners_)
    listener->on_camera_image(this->current_image_);
}

#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
bool LvglScreenshot::start_task_() {
  if (this->task_ != nullptr)
    return true;
  if (xTaskCreate(encode_task_, "screenshot_jpg", TASK_STACK_SIZE, this, this->task_priority_, &this->task_) !=
      pdPASS) {
    this->task_ = nullptr;
    ESP_LOGE(TAG, "Could not start the encoding task");
    return false;
  }
  return true;
}

void LvglScreenshot::encode_task_(void *arg) {
  auto *self = static_cast<LvglScreenshot *>(arg);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    self->encode_sw_();
  }
}

/// Runs in the encoding task while sw_state_ == SW_BUSY. No logging here: the main loop
/// reports the result.
void LvglScreenshot::encode_sw_() {
  const uint32_t start = millis();
  const size_t pixels = static_cast<size_t>(this->width_) * this->height_;

  // LVGL renders RGB565 little endian; the esp32-camera encoder reads it big endian (as a
  // camera sends it). Swap in place: the buffer is rendered anew for every screenshot.
  // (esp32-camera's global jpg_set_rgb565_big_endian() would also change real cameras.)
  auto *px = reinterpret_cast<uint16_t *>(this->raw_buf_);
  for (size_t i = 0; i < pixels; i++)
    px[i] = static_cast<uint16_t>((px[i] << 8) | (px[i] >> 8));

  // Start the output buffer at a quarter of the raw size, which fits a typical dashboard,
  // instead of growing it 1 kB per failed attempt. It is never shrunk.
  camera::EncoderBuffer *out = this->encoder_->get_output_buffer();
  const size_t initial = pixels / 2;
  if (out->get_max_size() < initial)
    out->set_buffer_size(initial);

  camera::CameraImageSpec spec{static_cast<uint16_t>(this->width_), static_cast<uint16_t>(this->height_),
                               camera::PIXEL_FORMAT_RGB565};
  RawPixels raw(this->raw_buf_, pixels * 2);
  camera::EncoderError result = camera::ENCODER_ERROR_SKIP_FRAME;
  for (int attempt = 0; attempt < MAX_ENCODE_ATTEMPTS; attempt++) {
    result = this->encoder_->encode_pixels(&spec, &raw);
    if (result == camera::ENCODER_ERROR_SUCCESS || result == camera::ENCODER_ERROR_CONFIGURATION)
      break;
    // Output buffer too small (RETRY, or SKIP when buffer_expand_size is 0): grow by half.
    if (!out->set_buffer_size(out->get_max_size() + out->get_max_size() / 2)) {
      this->sw_error_ = "out of memory for the JPEG";
      this->sw_state_.store(SW_FAILED, std::memory_order_release);
      return;
    }
  }
  if (result != camera::ENCODER_ERROR_SUCCESS) {
    this->sw_error_ = camera::to_string(result);
    this->sw_state_.store(SW_FAILED, std::memory_order_release);
    return;
  }
  this->sw_encode_ms_ = millis() - start;
  // ESP-IDF reports the high-water mark in bytes
  this->sw_stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
  // The JPEG stays in the encoder's output buffer; loop() publishes it from there.
  this->sw_state_.store(SW_DONE, std::memory_order_release);
}
#endif

}  // namespace esphome::lvgl_screenshot

#endif  // USE_ESP32
