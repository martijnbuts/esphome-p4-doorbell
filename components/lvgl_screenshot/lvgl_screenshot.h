#pragma once

#ifdef USE_ESP32

#include <memory>
#include <vector>

#include <lvgl.h>

#include "driver/jpeg_encode.h"
#include "esphome/components/camera/camera.h"
#include "esphome/core/component.h"

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

class LvglScreenshot : public camera::Camera {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_jpeg_quality(uint8_t quality) { this->quality_ = quality; }
  void set_stream_interval(uint32_t interval_ms) { this->stream_interval_ = interval_ms; }

  // camera::Camera
  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new ScreenshotImageReader(); }
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  bool prepare_(uint32_t width, uint32_t height);
  bool capture_(uint8_t requesters);

  std::vector<camera::CameraListener *> listeners_;
  std::shared_ptr<ScreenshotImage> current_image_;
  // Hardware JPEG encoder (shares the codec with jpeg_stream's decoder) and its DMA buffers,
  // created on the first screenshot and kept.
  jpeg_encoder_handle_t encoder_{nullptr};
  uint8_t *raw_buf_{nullptr};  // RGB565 snapshot, rendered by LVGL straight into it
  uint8_t *out_buf_{nullptr};  // JPEG output
  size_t raw_size_{0};
  size_t out_size_{0};
  lv_draw_buf_t snapshot_buf_{};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t stream_interval_{1000};
  uint32_t last_stream_frame_{0};
  uint8_t quality_{85};
  uint8_t single_requesters_{0};
  uint8_t stream_requesters_{0};
  bool first_done_{false};
};

}  // namespace esphome::lvgl_screenshot

#endif  // USE_ESP32
