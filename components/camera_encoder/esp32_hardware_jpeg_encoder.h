#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_HARDWARE_JPEG_ENCODER) && defined(USE_ESP32_VARIANT_ESP32P4)

#include <driver/jpeg_encode.h>

#include "esphome/components/camera/encoder.h"

namespace esphome::camera_encoder {

/// Output buffer for the hardware encoder. The codec writes it with DMA, so it comes from
/// jpeg_alloc_encoder_mem(). Growing it does not keep the old contents.
class ESP32HardwareJPEGEncoderBuffer final : public camera::EncoderBuffer {
 public:
  bool set_buffer_size(size_t size) override;
  uint8_t *get_data() const override { return this->data_; }
  size_t get_size() const override { return this->size_; }
  size_t get_max_size() const override { return this->capacity_; }

 protected:
  uint8_t *data_{nullptr};
  size_t capacity_{0};
  size_t size_{0};
};

/// Encoder that uses the JPEG codec of the ESP32-P4.
class ESP32HardwareJPEGEncoder final : public camera::Encoder {
 public:
  /// @param quality Quality of the encoded image (1-100).
  /// @param sub_sample Chroma subsampling for colour images.
  /// @param output Output buffer, allocated with jpeg_alloc_encoder_mem().
  ESP32HardwareJPEGEncoder(uint8_t quality, jpeg_down_sampling_type_t sub_sample, camera::EncoderBuffer *output)
      : output_(output), sub_sample_(sub_sample), quality_(quality) {}
  /// Sets the number of bytes to grow the output buffer by when an image does not fit.
  void set_buffer_expand_size(size_t buffer_expand_size) { this->buffer_expand_size_ = buffer_expand_size; }
  camera::EncoderError encode_pixels(camera::CameraImageSpec *spec, camera::Buffer *pixels) override;
  camera::EncoderBuffer *get_output_buffer() override { return this->output_; }
  void dump_config() override;

 protected:
  bool copy_input_(camera::CameraImageSpec *spec, camera::Buffer *pixels);

  jpeg_encoder_handle_t engine_{nullptr};
  camera::EncoderBuffer *output_;
  uint8_t *input_{nullptr};
  size_t input_capacity_{0};
  size_t buffer_expand_size_{0};
  jpeg_down_sampling_type_t sub_sample_;
  uint8_t quality_;
};

}  // namespace esphome::camera_encoder

#endif
