#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <memory>
#include <vector>

#include <lvgl.h>

#include "esphome/components/camera/camera.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
#include "driver/jpeg_encode.h"
#endif
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/components/camera/encoder.h"
#endif

namespace esphome::lvgl_screenshot {

/// One JPEG screenshot, shared by all API connections until the last one is done with it.
class ScreenshotImage : public camera::CameraImage {
 public:
  ScreenshotImage(uint8_t *data, size_t length, uint8_t requesters)
      : data_(data), length_(length), requesters_(requesters) {}
  ~ScreenshotImage() override;
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1 << requester)) != 0;
  }

 protected:
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
};

class ScreenshotImageReader : public camera::CameraImageReader {
 public:
  void set_image(std::shared_ptr<camera::CameraImage> image) override {
    this->image_ = std::move(image);
    this->offset_ = 0;
  }
  size_t available() const override {
    return this->image_ ? this->image_->get_data_length() - this->offset_ : 0;
  }
  uint8_t *peek_data_buffer() override { return this->image_->get_data_buffer() + this->offset_; }
  void consume_data(size_t consumed) override { this->offset_ += consumed; }
  void return_image() override { this->image_.reset(); }

 protected:
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
/// The raw RGB565 picture, handed to a camera::Encoder.
class RawPixels : public camera::Buffer {
 public:
  RawPixels(uint8_t *data, size_t length) : data_(data), length_(length) {}
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }

 protected:
  uint8_t *data_;
  size_t length_;
};
#endif

class LvglScreenshot : public camera::Camera {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_stream_interval(uint32_t interval_ms) { this->stream_interval_ = interval_ms; }
#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  void set_jpeg_quality(uint8_t quality) { this->quality_ = quality; }
#endif
#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  void set_encoder(camera::Encoder *encoder) { this->encoder_ = encoder; }
  void set_task_priority(uint8_t priority) { this->task_priority_ = priority; }
#endif

  // camera::Camera
  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new ScreenshotImageReader(); }
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  bool prepare_(uint32_t width, uint32_t height);
  bool render_(uint32_t &width, uint32_t &height);
  bool capture_(uint8_t requesters);
  void publish_(uint8_t *jpeg, size_t jpeg_len, uint8_t requesters, uint32_t render_ms, uint32_t encode_ms);

  std::vector<camera::CameraListener *> listeners_;
  std::shared_ptr<ScreenshotImage> current_image_;
  uint8_t *raw_buf_{nullptr};  // RGB565 snapshot, rendered by LVGL straight into it
  size_t raw_size_{0};
  lv_draw_buf_t snapshot_buf_{};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t stream_interval_{1000};
  uint32_t last_stream_frame_{0};
  uint8_t single_requesters_{0};
  uint8_t stream_requesters_{0};
  bool first_done_{false};

#ifdef USE_LVGL_SCREENSHOT_HW_JPEG
  // Hardware JPEG encoder (shares the codec with jpeg_stream's decoder) and its DMA output
  // buffer, created on the first screenshot and kept.
  jpeg_encoder_handle_t hw_encoder_{nullptr};
  uint8_t *out_buf_{nullptr};
  size_t out_size_{0};
  uint8_t quality_{85};
#endif

#ifdef USE_LVGL_SCREENSHOT_SW_JPEG
  enum SwState : uint8_t { SW_IDLE = 0, SW_BUSY, SW_DONE, SW_FAILED };

  static void encode_task_(void *arg);
  void encode_sw_();

  camera::Encoder *encoder_{nullptr};
  TaskHandle_t task_{nullptr};
  uint8_t task_priority_{1};
  // Hand-over between the main loop and the encoding task. While SW_BUSY the task owns
  // raw_buf_ and the encoder; the main loop only reads the results after SW_DONE/SW_FAILED.
  std::atomic<uint8_t> sw_state_{SW_IDLE};
  uint8_t sw_requesters_{0};
  uint32_t sw_render_ms_{0};
  uint32_t sw_encode_ms_{0};
  uint8_t *sw_jpeg_{nullptr};
  size_t sw_jpeg_len_{0};
  const char *sw_error_{nullptr};
#endif
};

}  // namespace esphome::lvgl_screenshot

#endif  // USE_ESP32
