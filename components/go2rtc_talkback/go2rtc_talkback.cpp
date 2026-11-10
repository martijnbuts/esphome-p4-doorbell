#include "go2rtc_talkback.h"

#ifdef USE_ESP32

#include <cinttypes>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <esp_http_client.h>
#include <lwip/sockets.h>

#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::go2rtc_talkback {

static const char *const TAG = "go2rtc_talkback";

static constexpr const char *PATH = "/talk.wav";
static constexpr size_t RING_BUFFER_SIZE = 32768;  // 1 s at 16 kHz, 16-bit mono
static constexpr size_t SEND_CHUNK = 1024;         // 32 ms at 16 kHz
static constexpr uint32_t SERVER_STACK = 4096;
static constexpr uint32_t CONTROL_STACK = 6144;  // esp_http_client
static constexpr UBaseType_t TASK_PRIORITY = 2;  // above the main loop: short bursts, low latency
static constexpr int HTTP_TIMEOUT_MS = 15000;    // go2rtc answers once ffmpeg has opened the WAV

void Go2rtcTalkback::setup() {
  this->ring_buffer_ = ring_buffer::RingBuffer::create(RING_BUFFER_SIZE);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the ring buffer");
    this->mark_failed();
    return;
  }
  this->sample_rate_ = this->mic_source_->get_audio_stream_info().get_sample_rate();

  // Called in the microphone's task. Only keep audio while go2rtc is fetching it, so the
  // first words are not delayed by audio from before the connection.
  this->mic_source_->add_data_callback([this](const microphone::AudioCapture &capture) {
    if (!this->streaming_.load())
      return;
    const size_t written = this->ring_buffer_->write_without_replacement(capture.data.data(), capture.data.size(), 0);
    if (written < capture.data.size())
      this->bytes_dropped_ += capture.data.size() - written;
  });

  if (xTaskCreate(Go2rtcTalkback::server_task_, "talkback_srv", SERVER_STACK, this, TASK_PRIORITY,
                  &this->server_task_handle_) != pdPASS ||
      xTaskCreate(Go2rtcTalkback::control_task_, "talkback_api", CONTROL_STACK, this, TASK_PRIORITY,
                  &this->control_task_handle_) != pdPASS) {
    ESP_LOGE(TAG, "Could not create the tasks");
    this->mark_failed();
  }
}

void Go2rtcTalkback::dump_config() {
  ESP_LOGCONFIG(TAG,
                "go2rtc talkback:\n"
                "  go2rtc: %s, stream: %s%s\n"
                "  Microphone: http://<this device>:%u%s (%" PRIu32 " Hz, 16-bit mono WAV)\n"
                "  Max duration: %" PRIu32 " ms",
                this->url_.c_str(), this->stream_.c_str(), this->username_.empty() ? "" : " (with password)",
                this->port_, PATH, this->sample_rate_, this->max_duration_);
  for (uint32_t addr : this->allowed_ips_) {
    ESP_LOGCONFIG(TAG, "  Allowed: %u.%u.%u.%u", (unsigned) (addr & 0xFF), (unsigned) ((addr >> 8) & 0xFF),
                  (unsigned) ((addr >> 16) & 0xFF), (unsigned) (addr >> 24));
  }
}

void Go2rtcTalkback::start() {
  if (this->active_.load() || this->is_failed())
    return;

  // The address go2rtc's ffmpeg fetches the microphone from: the first IPv4 address of this device
  std::string ip;
  for (auto &addr : network::get_ip_addresses()) {
    if (addr.is_ip4()) {
      char buf[network::IP_ADDRESS_BUFFER_SIZE];
      ip = addr.str_to(buf);
      break;
    }
  }
  if (ip.empty()) {
    ESP_LOGW(TAG, "No IPv4 address, cannot talk back");
    this->error_callback_.call();
    return;
  }
  this->own_url_ = "http://" + ip + ":" + std::to_string(this->port_) + PATH;

  this->bytes_sent_ = 0;
  this->bytes_dropped_ = 0;
  this->play_status_ = 0;
  this->play_error_ = ESP_OK;
  this->closed_by_go2rtc_ = false;
  this->start_ms_ = millis();
  this->active_ = true;
  this->mic_source_->start();
  ESP_LOGI(TAG, "Talking: go2rtc plays %s on stream %s", this->own_url_.c_str(), this->stream_.c_str());
  xTaskNotify(this->control_task_handle_, CMD_PLAY, eSetValueWithOverwrite);
}

void Go2rtcTalkback::stop() {
  if (!this->active_.load())
    return;
  this->active_ = false;  // the server task closes the WAV, so ffmpeg ends
  this->mic_source_->stop();
  xTaskNotify(this->control_task_handle_, CMD_STOP, eSetValueWithOverwrite);
  ESP_LOGI(TAG, "Stopped after %" PRIu32 " ms: %" PRIu32 " bytes sent, %" PRIu32 " dropped", millis() - this->start_ms_,
           this->bytes_sent_.load(), this->bytes_dropped_.load());
  this->stop_callback_.call();
}

void Go2rtcTalkback::loop() {
  if (this->streaming_started_.exchange(false)) {
    ESP_LOGI(TAG, "go2rtc is fetching the microphone (%" PRIu32 " ms after start)", millis() - this->start_ms_);
    this->streaming_callback_.call();
  }

  const int status = this->play_status_.exchange(0);
  if (status != 0 && status != 200 && this->active_.load()) {
    if (status < 0) {
      ESP_LOGW(TAG, "go2rtc not reachable: %s", esp_err_to_name(this->play_error_.load()));
    } else {
      ESP_LOGW(TAG, "go2rtc refused to play (HTTP %d): does stream '%s' exist and have a backchannel?", status,
               this->stream_.c_str());
    }
    this->stop();
    this->error_callback_.call();  // after on_stop, so an error message stays on screen
    return;
  }

  if (this->closed_by_go2rtc_.exchange(false) && this->active_.load()) {
    ESP_LOGW(TAG, "go2rtc closed the microphone stream: does the camera accept two-way audio?");
    this->stop();
    this->error_callback_.call();
    return;
  }

  if (this->active_.load() && this->max_duration_ != 0 && millis() - this->start_ms_ > this->max_duration_) {
    ESP_LOGI(TAG, "Maximum duration reached");
    this->stop();
  }
}

bool Go2rtcTalkback::ip_allowed_(uint32_t addr) const {
  if (this->allowed_ips_.empty())
    return true;
  for (uint32_t allowed : this->allowed_ips_) {
    if (allowed == addr)
      return true;
  }
  return false;
}

// ---- Server task: hands the microphone to go2rtc's ffmpeg as an endless WAV ----------------------

void Go2rtcTalkback::server_task_(void *arg) { static_cast<Go2rtcTalkback *>(arg)->serve_(); }

void Go2rtcTalkback::serve_() {
  int listen_fd = -1;
  for (;;) {
    if (listen_fd < 0) {
      listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listen_fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      int one = 1;
      setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      struct sockaddr_in addr {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(this->port_);
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
      if (::bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || ::listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "Could not listen on port %u (errno %d)", this->port_, errno);
        ::close(listen_fd);
        listen_fd = -1;
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      // accept() returns at least once a second
      struct timeval tv {
        .tv_sec = 1, .tv_usec = 0
      };
      setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in peer {};
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept(listen_fd, (struct sockaddr *) &peer, &peer_len);
    if (fd < 0)
      continue;
    const uint32_t peer_ip = peer.sin_addr.s_addr;
    if (!this->ip_allowed_(peer_ip)) {
      ESP_LOGW(TAG, "Refused %u.%u.%u.%u: not in allowed_ips", (unsigned) (peer_ip & 0xFF),
               (unsigned) ((peer_ip >> 8) & 0xFF), (unsigned) ((peer_ip >> 16) & 0xFF), (unsigned) (peer_ip >> 24));
    } else if (!this->active_.load()) {
      static const char NOT_TALKING[] = "HTTP/1.0 503 Service Unavailable\r\nConnection: close\r\n\r\n";
      ::send(fd, NOT_TALKING, sizeof(NOT_TALKING) - 1, 0);
    } else {
      this->serve_client_(fd);
    }
    ::close(fd);
  }
}

static bool send_all(int fd, const uint8_t *data, size_t len) {
  while (len > 0) {
    const int n = ::send(fd, data, len, 0);
    if (n <= 0)
      return false;
    data += n;
    len -= n;
  }
  return true;
}

static void put_le(uint8_t *p, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++)
    p[i] = (value >> (8 * i)) & 0xFF;
}

void Go2rtcTalkback::serve_client_(int fd) {
  struct timeval tv {
    .tv_sec = 2, .tv_usec = 0
  };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Read the request head: "GET /talk.wav HTTP/1.1 ... \r\n\r\n"
  char req[512];
  size_t got = 0;
  while (got < sizeof(req) - 1) {
    const int n = ::recv(fd, req + got, sizeof(req) - 1 - got, 0);
    if (n <= 0)
      return;
    got += n;
    req[got] = '\0';
    if (strstr(req, "\r\n\r\n") != nullptr)
      break;
  }
  const size_t path_len = strlen(PATH);
  if (strncmp(req, "GET ", 4) != 0 || strncmp(req + 4, PATH, path_len) != 0 ||
      (req[4 + path_len] != ' ' && req[4 + path_len] != '?')) {
    static const char NOT_FOUND[] = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
    ::send(fd, NOT_FOUND, sizeof(NOT_FOUND) - 1, 0);
    return;
  }

  // HTTP/1.0 without Content-Length: the body ends when the connection closes (on stop)
  static const char HEAD[] =
      "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
  // WAV header for 16-bit mono PCM with an unknown (maximum) length
  uint8_t wav[44];
  memcpy(wav, "RIFF", 4);
  put_le(wav + 4, 0xFFFFFFFF, 4);
  memcpy(wav + 8, "WAVEfmt ", 8);
  put_le(wav + 16, 16, 4);                      // fmt chunk size
  put_le(wav + 20, 1, 2);                       // PCM
  put_le(wav + 22, 1, 2);                       // mono
  put_le(wav + 24, this->sample_rate_, 4);      // sample rate
  put_le(wav + 28, this->sample_rate_ * 2, 4);  // byte rate
  put_le(wav + 32, 2, 2);                       // block align
  put_le(wav + 34, 16, 2);                      // bits per sample
  memcpy(wav + 36, "data", 4);
  put_le(wav + 40, 0xFFFFFFFF, 4);
  if (!send_all(fd, (const uint8_t *) HEAD, sizeof(HEAD) - 1) || !send_all(fd, wav, sizeof(wav)))
    return;

  // From now on the microphone fills the ring buffer; start without old audio
  this->ring_buffer_->reset();
  this->streaming_ = true;
  this->streaming_started_ = true;

  uint8_t buf[SEND_CHUNK];
  while (this->active_.load()) {
    const size_t n = this->ring_buffer_->read(buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (n == 0)
      continue;
    if (!send_all(fd, buf, n)) {
      this->closed_by_go2rtc_ = true;
      break;
    }
    this->bytes_sent_ += n;
  }
  this->streaming_ = false;
}

// ---- Control task: the go2rtc API calls ----------------------------------------------------------

void Go2rtcTalkback::control_task_(void *arg) { static_cast<Go2rtcTalkback *>(arg)->control_(); }

static std::string url_encode(const std::string &in) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char) c;
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 0x0F];
    }
  }
  return out;
}

void Go2rtcTalkback::control_() {
  for (;;) {
    uint32_t cmd = 0;
    xTaskNotifyWait(0, UINT32_MAX, &cmd, portMAX_DELAY);
    if (cmd == CMD_PLAY) {
      // go2rtc runs ffmpeg on our WAV with #audio=auto (the camera's codec) and answers once it plays
      esp_err_t err = ESP_OK;
      const int status =
          this->post_("/api/ffmpeg?dst=" + url_encode(this->stream_) + "&live=" + url_encode(this->own_url_), err);
      if (status != 200) {
        this->play_error_ = err;
        this->play_status_ = err != ESP_OK ? -1 : status;
      }
    } else if (cmd == CMD_STOP) {
      // closing the WAV already ends ffmpeg; this also stops anything else still playing
      esp_err_t err = ESP_OK;
      const int status = this->post_("/api/streams?dst=" + url_encode(this->stream_) + "&src=", err);
      if (status != 200) {
        ESP_LOGD(TAG, "go2rtc stop request: HTTP %d (%s)", status, esp_err_to_name(err));
      }
    }
  }
}

int Go2rtcTalkback::post_(const std::string &path_and_query, esp_err_t &err) {
  const std::string url = this->url_ + path_and_query;
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.disable_auto_redirect = true;
  if (!this->username_.empty()) {
    config.username = this->username_.c_str();
    config.password = this->password_.c_str();
    config.auth_type = HTTP_AUTH_TYPE_BASIC;
  }
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    err = ESP_ERR_NO_MEM;
    return -1;
  }
  err = esp_http_client_perform(client);
  const int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
  esp_http_client_cleanup(client);
  return status;
}

}  // namespace esphome::go2rtc_talkback

#endif  // USE_ESP32
