#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::go2rtc_talkback {

/// The microphone as an endless WAV over HTTP, played by go2rtc on a camera's two-way audio channel.
class Go2rtcTalkback : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone_source(microphone::MicrophoneSource *source) { this->mic_source_ = source; }
  void set_url(const std::string &url) { this->url_ = url; }
  void set_stream(const std::string &stream) { this->stream_ = stream; }
  void set_credentials(const std::string &username, const std::string &password) {
    this->username_ = username;
    this->password_ = password;
  }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_max_duration(uint32_t ms) { this->max_duration_ = ms; }
  /// Network byte order. Without any, every peer may fetch the microphone.
  void add_allowed_ip(uint32_t addr) { this->allowed_ips_.push_back(addr); }

  /// Start the microphone and ask go2rtc to play it on the camera.
  void start();
  /// Stop the microphone and ask go2rtc to stop.
  void stop();
  /// Between start() and stop().
  bool is_active() const { return this->active_.load(); }
  /// go2rtc's ffmpeg is fetching the microphone.
  bool is_streaming() const { return this->streaming_.load(); }

  template<typename F> void add_on_streaming_callback(F &&callback) {
    this->streaming_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_stop_callback(F &&callback) { this->stop_callback_.add(std::forward<F>(callback)); }
  template<typename F> void add_on_error_callback(F &&callback) {
    this->error_callback_.add(std::forward<F>(callback));
  }

 protected:
  static constexpr uint32_t CMD_PLAY = 1;
  static constexpr uint32_t CMD_STOP = 2;

  static void server_task_(void *arg);
  static void control_task_(void *arg);
  void serve_();
  void serve_client_(int fd);
  void control_();
  /// POST to the go2rtc API; returns the HTTP status, or -1 with err set when the request failed.
  int post_(const std::string &path_and_query, esp_err_t &err);
  bool ip_allowed_(uint32_t addr) const;

  microphone::MicrophoneSource *mic_source_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  TaskHandle_t server_task_handle_{nullptr};
  TaskHandle_t control_task_handle_{nullptr};

  std::string url_;
  std::string stream_;
  std::string username_;
  std::string password_;
  std::string own_url_;  // http://<this device>:<port>/talk.wav, set by start()
  std::vector<uint32_t> allowed_ips_;
  uint32_t sample_rate_{16000};
  uint32_t max_duration_{60000};
  uint32_t start_ms_{0};
  uint16_t port_{8555};

  std::atomic<bool> active_{false};
  std::atomic<bool> streaming_{false};
  // set by the tasks, handled in loop()
  std::atomic<bool> streaming_started_{false};
  std::atomic<int> play_status_{0};  // HTTP status of a failed play request, -1: see play_error_
  std::atomic<esp_err_t> play_error_{ESP_OK};
  std::atomic<bool> closed_by_go2rtc_{false};  // ffmpeg closed the WAV while still talking
  std::atomic<uint32_t> bytes_sent_{0};
  std::atomic<uint32_t> bytes_dropped_{0};

  CallbackManager<void()> streaming_callback_;
  CallbackManager<void()> stop_callback_;
  CallbackManager<void()> error_callback_;
};

}  // namespace esphome::go2rtc_talkback

#endif  // USE_ESP32
