#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_LVGL_SCREENSHOT)

#include <atomic>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "esphome/components/camera/camera.h"
#include "esphome/components/camera/encoder.h"
#include "esphome/core/component.h"

namespace esphome::lvgl_screenshot {

/// A JPEG screenshot in the encoder's output buffer. The component takes the next screenshot only
/// once every client has released this one.
class ScreenshotImage final : public camera::CameraImage {
 public:
  void set_frame(uint8_t *data, size_t length, uint8_t requesters) {
    this->data_ = data;
    this->length_ = length;
    this->requesters_ = requesters;
  }
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1U << requester)) != 0;
  }

 protected:
  uint8_t *data_{nullptr};
  size_t length_{0};
  uint8_t requesters_{0};
};

/// Tracks how far a client has read into a screenshot.
class ScreenshotImageReader final : public camera::CameraImageReader {
 public:
  void set_image(std::shared_ptr<camera::CameraImage> image) override;
  size_t available() const override;
  uint8_t *peek_data_buffer() override;
  void consume_data(size_t consumed) override;
  void return_image() override;

 protected:
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

/// The raw RGB565 screenshot, handed to the encoder.
class RawScreenshot final : public camera::Buffer {
 public:
  RawScreenshot(uint8_t *data, size_t length) : data_(data), length_(length) {}
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }

 protected:
  uint8_t *data_;
  size_t length_;
};

/// Camera entity that shows the active LVGL screen.
class LvglScreenshot final : public camera::Camera {
 public:
  LvglScreenshot(camera::Encoder *encoder, uint32_t stream_interval)
      : encoder_(encoder), stream_interval_(stream_interval) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After lvgl, which creates the display.
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Called at setup by the clients; a pointer array grown by one, without std::vector's code
  void add_listener(camera::CameraListener *listener) override;
  camera::CameraImageReader *create_image_reader() override { return new ScreenshotImageReader(); }  // NOLINT
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  /// Hand-over between the main loop and the encoding task. While CAPTURE_STATE_BUSY the task owns
  /// the draw buffer and the encoder; the main loop reads the result after DONE or FAILED.
  enum class CaptureState : uint8_t {
    CAPTURE_STATE_IDLE,
    CAPTURE_STATE_BUSY,
    CAPTURE_STATE_DONE,
    CAPTURE_STATE_FAILED,
  };

  bool render_();
  void publish_();
  void encode_();
  static void encode_task(void *param);

  camera::Encoder *encoder_;
  uint32_t stream_interval_;
  uint32_t last_stream_frame_{0};
  uint32_t render_ms_{0};
  uint32_t encode_ms_{0};
  TaskHandle_t task_{nullptr};
  std::shared_ptr<ScreenshotImage> reusable_image_;
  std::shared_ptr<ScreenshotImage> current_image_;
  camera::CameraListener **listeners_{nullptr};
  lv_draw_buf_t *draw_buf_{nullptr};
  std::atomic<CaptureState> state_{CaptureState::CAPTURE_STATE_IDLE};
  camera::EncoderError encode_result_{camera::ENCODER_ERROR_SUCCESS};
  uint16_t width_{0};
  uint16_t height_{0};
  uint8_t listener_count_{0};
  uint8_t single_requesters_{0};
  uint8_t stream_requesters_{0};
  /// Requesters of the screenshot being encoded.
  uint8_t pending_requesters_{0};
};

}  // namespace esphome::lvgl_screenshot

#endif
