#include "esp32_hardware_jpeg_encoder.h"

#if defined(USE_ESP32_HARDWARE_JPEG_ENCODER) && defined(USE_ESP32_VARIANT_ESP32P4)

#include <cstdlib>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome::camera_encoder {

ESPHOME_LOG_TAG(TAG, "camera_encoder");

/// Far longer than the codec needs for any frame; only stops a stuck transfer.
static constexpr int ENCODE_TIMEOUT_MS = 5000;

static uint8_t *alloc_codec_buffer(jpeg_enc_buffer_alloc_direction_t direction, size_t size, size_t *allocated) {
  jpeg_encode_memory_alloc_cfg_t config{};
  config.buffer_direction = direction;
  return static_cast<uint8_t *>(jpeg_alloc_encoder_mem(size, &config, allocated));
}

static const LogString *sub_sample_to_str(jpeg_down_sampling_type_t sub_sample) {
  switch (sub_sample) {
    case JPEG_DOWN_SAMPLING_YUV444:
      return LOG_STR("4:4:4");
    case JPEG_DOWN_SAMPLING_YUV422:
      return LOG_STR("4:2:2");
    default:
      return LOG_STR("4:2:0");
  }
}

bool ESP32HardwareJPEGEncoderBuffer::set_buffer_size(size_t size) {
  if (size > this->capacity_) {
    size_t allocated = 0;
    uint8_t *data = alloc_codec_buffer(JPEG_ENC_ALLOC_OUTPUT_BUFFER, size, &allocated);
    if (data == nullptr)
      return false;
    free(this->data_);  // NOLINT(cppcoreguidelines-no-malloc)
    this->data_ = data;
    this->capacity_ = allocated;
  }
  this->size_ = size;
  return true;
}

bool ESP32HardwareJPEGEncoder::copy_input_(camera::CameraImageSpec *spec, camera::Buffer *pixels) {
  const size_t length = pixels->get_data_length();
  // The codec reads the frame with DMA, so it needs an aligned buffer of its own. It is made on
  // the first frame, when the frame size is known, and only grows for a larger frame.
  if (length > this->input_capacity_) {
    size_t allocated = 0;
    uint8_t *input = alloc_codec_buffer(JPEG_ENC_ALLOC_INPUT_BUFFER, length, &allocated);
    if (input == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate %zu bytes for the input frame", length);
      return false;
    }
    free(this->input_);  // NOLINT(cppcoreguidelines-no-malloc)
    this->input_ = input;
    this->input_capacity_ = allocated;
  }
  const uint8_t *src = pixels->get_data_buffer();
  if (spec->format == camera::PIXEL_FORMAT_RGB565) {
    // RGB565 frames have the high byte first, as the esp32_camera encoder reads them; the
    // codec reads the low byte first.
    for (size_t i = 0; i + 1 < length; i += 2) {
      this->input_[i] = src[i + 1];
      this->input_[i + 1] = src[i];
    }
  } else {
    std::memcpy(this->input_, src, length);
  }
  return true;
}

camera::EncoderError ESP32HardwareJPEGEncoder::encode_pixels(camera::CameraImageSpec *spec, camera::Buffer *pixels) {
  jpeg_encode_cfg_t config{};
  config.width = spec->width;
  config.height = spec->height;
  config.image_quality = this->quality_;
  switch (spec->format) {
    case camera::PIXEL_FORMAT_GRAYSCALE:
      config.src_type = JPEG_ENCODE_IN_FORMAT_GRAY;
      config.sub_sample = JPEG_DOWN_SAMPLING_GRAY;
      break;
    case camera::PIXEL_FORMAT_RGB565:
      config.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
      config.sub_sample = this->sub_sample_;
      break;
    case camera::PIXEL_FORMAT_BGR888:
      // The codec reads 24-bit pixels in B, G, R order, the same as PIXEL_FORMAT_BGR888.
      config.src_type = JPEG_ENCODE_IN_FORMAT_RGB888;
      config.sub_sample = this->sub_sample_;
      break;
    default:
      ESP_LOGE(TAG, "Unsupported pixel format %u", static_cast<unsigned>(spec->format));
      return camera::ENCODER_ERROR_CONFIGURATION;
  }

  if (this->engine_ == nullptr) {
    jpeg_encode_engine_cfg_t engine_config{};
    engine_config.timeout_ms = ENCODE_TIMEOUT_MS;
    esp_err_t err = jpeg_new_encoder_engine(&engine_config, &this->engine_);
    if (err != ESP_OK) {
      this->engine_ = nullptr;
      ESP_LOGE(TAG, "Encoder engine setup failed: %s", esp_err_to_name(err));
      return camera::ENCODER_ERROR_CONFIGURATION;
    }
  }
  if (!this->copy_input_(spec, pixels))
    return camera::ENCODER_ERROR_SKIP_FRAME;

  uint32_t encoded = 0;
  // Without an output buffer (its allocation at boot failed) this takes the same path as a full one.
  esp_err_t err = ESP_ERR_INVALID_STATE;
  if (this->output_->get_data() != nullptr) {
    err = jpeg_encoder_process(this->engine_, &config, this->input_, pixels->get_data_length(),
                               this->output_->get_data(), this->output_->get_max_size(), &encoded);
  }
  if (err == ESP_OK) {
    this->output_->set_buffer_size(encoded);
    return camera::ENCODER_ERROR_SUCCESS;
  }
  // The codec returns ESP_ERR_INVALID_STATE when the image does not fit the output buffer.
  if (err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "Encoding failed: %s", esp_err_to_name(err));
    return camera::ENCODER_ERROR_SKIP_FRAME;
  }
  if (this->buffer_expand_size_ == 0) {
    ESP_LOGW(TAG, "Image does not fit the %zu byte output buffer", this->output_->get_max_size());
    return camera::ENCODER_ERROR_SKIP_FRAME;
  }
  const size_t new_size = this->output_->get_max_size() + this->buffer_expand_size_;
  if (!this->output_->set_buffer_size(new_size)) {
    ESP_LOGE(TAG, "Failed to expand the output buffer to %zu bytes", new_size);
    this->buffer_expand_size_ = 0;
    return camera::ENCODER_ERROR_SKIP_FRAME;
  }
  ESP_LOGD(TAG, "Output buffer expanded to %zu bytes", this->output_->get_max_size());
  return camera::ENCODER_ERROR_RETRY_FRAME;
}

void ESP32HardwareJPEGEncoder::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP32 Hardware JPEG Encoder:\n"
                "  Quality: %u\n"
                "  Subsampling: %s\n"
                "  Size: %zu\n"
                "  Expand: %zu",
                this->quality_, LOG_STR_ARG(sub_sample_to_str(this->sub_sample_)), this->output_->get_max_size(),
                this->buffer_expand_size_);
}

}  // namespace esphome::camera_encoder

#endif
