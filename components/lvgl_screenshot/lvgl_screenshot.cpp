#include "lvgl_screenshot.h"

#ifdef USE_ESP32

#include <cstring>

#include <esp_heap_caps.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::lvgl_screenshot {

static const char *const TAG = "lvgl_screenshot";
static constexpr uint32_t PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
// Far longer than the hardware needs for 1024x600 (a few ms); the codec may first be busy
// with a doorbell frame (jpeg_stream shares the same JPEG engine).
static constexpr int ENCODER_TIMEOUT_MS = 500;

ScreenshotImage::~ScreenshotImage() { heap_caps_free(this->data_); }

void LvglScreenshot::setup() {
  // Nothing to allocate up front: the encoder and buffers are created on the first request,
  // so a display nobody looks at costs no memory.
}

void LvglScreenshot::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LVGL screenshot camera:\n"
                "  Encoder: hardware JPEG (ESP32-P4)\n"
                "  JPEG quality: %u\n"
                "  Stream interval: %" PRIu32 " ms",
                this->quality_, this->stream_interval_);
}

void LvglScreenshot::request_image(camera::CameraRequester requester) {
  this->single_requesters_ |= (1U << requester);
}

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
  uint8_t requesters = this->single_requesters_;
  const uint32_t now = millis();
  const bool stream_due = this->stream_requesters_ != 0 && now - this->last_stream_frame_ >= this->stream_interval_;
  if (stream_due)
    requesters |= this->stream_requesters_;
  if (requesters == 0)
    return;
  // Wait until every client has finished sending the previous screenshot.
  if (this->current_image_ && this->current_image_.use_count() > 1)
    return;
  this->current_image_.reset();
  this->single_requesters_ = 0;
  if (stream_due)
    this->last_stream_frame_ = now;
  this->capture_(requesters);
}

bool LvglScreenshot::prepare_(uint32_t width, uint32_t height) {
  if (this->encoder_ == nullptr) {
    jpeg_encode_engine_cfg_t engine_cfg = {};
    engine_cfg.intr_priority = 0;
    engine_cfg.timeout_ms = ENCODER_TIMEOUT_MS;
    if (jpeg_new_encoder_engine(&engine_cfg, &this->encoder_) != ESP_OK) {
      this->encoder_ = nullptr;
      ESP_LOGE(TAG, "Could not create the JPEG encoder");
      return false;
    }
  }
  if (this->raw_buf_ != nullptr && width == this->width_ && height == this->height_)
    return true;

  // New (or first) screen size: (re)allocate both DMA buffers. The encoder reads the raw
  // picture and writes the JPEG with 2D-DMA, so both come from jpeg_alloc_encoder_mem
  // (cache-line aligned PSRAM).
  heap_caps_free(this->raw_buf_);
  heap_caps_free(this->out_buf_);
  this->raw_buf_ = nullptr;
  this->out_buf_ = nullptr;
  const uint32_t stride = width * 2;
  jpeg_encode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
  jpeg_encode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  // A JPEG of a dashboard is a fraction of the raw size; half of it leaves ample room even for
  // a doorbell photo, and the encoder reports an error rather than overflowing.
  this->raw_buf_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(stride * height, &in_cfg, &this->raw_size_));
  this->out_buf_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(stride * height / 2, &out_cfg, &this->out_size_));
  if (this->raw_buf_ == nullptr || this->out_buf_ == nullptr) {
    heap_caps_free(this->raw_buf_);
    heap_caps_free(this->out_buf_);
    this->raw_buf_ = nullptr;
    this->out_buf_ = nullptr;
    ESP_LOGE(TAG, "Could not allocate the screenshot buffers (%" PRIu32 "x%" PRIu32 ")", width, height);
    return false;
  }
  lv_draw_buf_init(&this->snapshot_buf_, width, height, LV_COLOR_FORMAT_RGB565, stride, this->raw_buf_,
                   this->raw_size_);
  this->width_ = width;
  this->height_ = height;
  return true;
}

bool LvglScreenshot::capture_(uint8_t requesters) {
  lv_obj_t *screen = lv_screen_active();
  if (screen == nullptr) {
    ESP_LOGW(TAG, "No active LVGL screen");
    return false;
  }
  const uint32_t start = millis();

  // Size of the screen. lv_snapshot also adds the extra draw size (shadow/outline), which a
  // screen does not have; if it ever does, the snapshot below fails instead of overflowing.
  lv_obj_update_layout(screen);
  const int32_t w = lv_obj_get_width(screen);
  const int32_t h = lv_obj_get_height(screen);
  // 4:4:4 encodes in 8x8 blocks
  if (w <= 0 || h <= 0 || w % 8 != 0 || h % 8 != 0) {
    ESP_LOGE(TAG, "Screen size %" PRId32 "x%" PRId32 " is not a multiple of 8", w, h);
    return false;
  }
  if (!this->prepare_(w, h))
    return false;

  // Render the active screen into the encoder's input buffer (RGB565, little endian).
  if (lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, &this->snapshot_buf_) != LV_RESULT_OK) {
    ESP_LOGE(TAG, "lv_snapshot failed");
    return false;
  }
  if (this->snapshot_buf_.header.stride != this->width_ * 2) {
    ESP_LOGE(TAG, "Unexpected stride %" PRIu32 " for width %" PRIu32, (uint32_t) this->snapshot_buf_.header.stride,
             this->width_);
    return false;
  }
  const uint32_t rendered = millis();

  // Encode in hardware. 4:4:4 keeps small coloured text sharp. The driver writes the cache
  // back to PSRAM before the DMA reads the picture.
  jpeg_encode_cfg_t cfg = {};
  cfg.width = this->width_;
  cfg.height = this->height_;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV444;
  cfg.image_quality = this->quality_;
  uint32_t jpeg_len = 0;
  const esp_err_t err = jpeg_encoder_process(this->encoder_, &cfg, this->raw_buf_, this->width_ * this->height_ * 2,
                                             this->out_buf_, this->out_size_, &jpeg_len);
  if (err != ESP_OK || jpeg_len == 0) {
    ESP_LOGE(TAG, "JPEG encoding failed: %s", esp_err_to_name(err));
    return false;
  }
  const uint32_t encoded = millis();

  // Copy the JPEG into an exact-size buffer, so the output buffer is free for the next
  // screenshot while the API connections are still sending this one.
  auto *jpeg = static_cast<uint8_t *>(heap_caps_malloc(jpeg_len, PSRAM_CAPS));
  if (jpeg == nullptr) {
    ESP_LOGE(TAG, "Could not allocate %" PRIu32 " bytes for the JPEG", jpeg_len);
    return false;
  }
  std::memcpy(jpeg, this->out_buf_, jpeg_len);

  this->current_image_ = std::make_shared<ScreenshotImage>(jpeg, (size_t) jpeg_len, requesters);
  // The first screenshot after boot at INFO (the screens log at INFO), the rest at DEBUG
  if (this->first_done_) {
    ESP_LOGD(TAG, "Screenshot %" PRIu32 "x%" PRIu32 ", %" PRIu32 " bytes (render %" PRIu32 " ms, jpeg %" PRIu32 " ms)",
             this->width_, this->height_, jpeg_len, rendered - start, encoded - rendered);
  } else {
    ESP_LOGI(TAG, "First screenshot %" PRIu32 "x%" PRIu32 ", %" PRIu32 " bytes (render %" PRIu32 " ms, jpeg %" PRIu32
             " ms)", this->width_, this->height_, jpeg_len, rendered - start, encoded - rendered);
    this->first_done_ = true;
  }
  for (auto *listener : this->listeners_)
    listener->on_camera_image(this->current_image_);
  return true;
}

}  // namespace esphome::lvgl_screenshot

#endif  // USE_ESP32
