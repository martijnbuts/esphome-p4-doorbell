#include "jpeg_stream.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::jpeg_stream {

static const char *const TAG = "jpeg_stream";

static constexpr uint32_t TASK_STACK_SIZE = 8192;  // TLS handshakes run in this task
static constexpr UBaseType_t TASK_PRIORITY = 1;     // same as the main loop task
static constexpr uint32_t RETRY_DELAY_MS = 1000;
static constexpr size_t MAX_URL_LENGTH = 512;
static constexpr size_t RX_BUFFER_SIZE = 4096;
static constexpr size_t MAX_PART_HEADER = 512;
static constexpr int DECODER_TIMEOUT_MS = 200;
// open_(), read_image_(), read_stream_(), decode_() and present_() return 0, an esp_err_t, or
// HTTP_STATUS_BASE + the HTTP status code.
static constexpr int HTTP_STATUS_BASE = 1000000;

static uint32_t align_up(uint32_t value, uint32_t align) { return (value + align - 1) / align * align; }

void JpegStream::setup() {
  // Buffers, decoder and task are only created on the first start(), so a page that is never
  // opened costs no memory.
  this->disable_loop();
}

void JpegStream::dump_config() {
  ESP_LOGCONFIG(TAG,
                "JPEG stream:\n"
                "  Max size: %ux%u\n"
                "  Max JPEG size: %" PRIu32 " bytes\n"
                "  Timeout: %" PRIu32 " ms\n"
                "  Min interval: %" PRIu32 " ms\n"
                "  Stats interval: %" PRIu32 " ms",
                this->max_width_, this->max_height_, this->max_jpeg_size_, this->timeout_, this->min_interval_,
                this->stats_interval_);
}

void JpegStream::set_url(const std::string &url) {
  LockGuard guard(this->url_mutex_);
  if (url != this->url_) {
    this->url_ = url;
    this->url_changed_ = true;
  }
}

bool JpegStream::allocate_() {
  if (this->jpeg_buf_ != nullptr)
    return true;
  this->jpeg_buf_ = static_cast<uint8_t *>(heap_caps_malloc(this->max_jpeg_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->rx_buf_ = static_cast<uint8_t *>(heap_caps_malloc(RX_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  // The decoder writes whole MCU blocks (up to 16x16 pixels), so round the frame size up.
  const size_t frame_size = align_up(this->max_width_, 16) * align_up(this->max_height_, 16) * 2;
  jpeg_decode_memory_alloc_cfg_t mem_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  for (auto &buf : this->frame_buf_)
    buf = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(frame_size, &mem_cfg, &this->frame_buf_size_));
  if (this->jpeg_buf_ == nullptr || this->rx_buf_ == nullptr || this->frame_buf_[0] == nullptr ||
      this->frame_buf_[1] == nullptr) {
    ESP_LOGE(TAG, "Could not allocate buffers (%u + 2 x %u bytes)", (unsigned) this->max_jpeg_size_,
             (unsigned) frame_size);
    heap_caps_free(this->jpeg_buf_);
    heap_caps_free(this->rx_buf_);
    heap_caps_free(this->frame_buf_[0]);
    heap_caps_free(this->frame_buf_[1]);
    this->jpeg_buf_ = this->rx_buf_ = this->frame_buf_[0] = this->frame_buf_[1] = nullptr;
    return false;
  }
  return true;
}

bool JpegStream::create_decoder_() {
  jpeg_decode_engine_cfg_t engine_cfg = {.intr_priority = 0, .timeout_ms = DECODER_TIMEOUT_MS};
  const esp_err_t err = jpeg_new_decoder_engine(&engine_cfg, &this->decoder_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not create the hardware JPEG decoder: %s", esp_err_to_name(err));
    this->decoder_ = nullptr;
    return false;
  }
  return true;
}

void JpegStream::start() {
  if (this->canvas_ == nullptr || !this->allocate_())
    return;
  if (this->decoder_ == nullptr && !this->create_decoder_())
    return;
  if (this->task_ == nullptr) {
    if (xTaskCreate(JpegStream::task_entry_, "jpeg_stream", TASK_STACK_SIZE, this, TASK_PRIORITY, &this->task_) !=
        pdPASS) {
      ESP_LOGE(TAG, "Could not start the stream task");
      this->task_ = nullptr;
      return;
    }
  }
  if (this->stats_interval_ > 0 && !this->display_events_added_) {
    // Time every LVGL refresh that renders something, to see what drawing the frames costs.
    lv_display_t *disp = lv_obj_get_display(this->canvas_);
    if (disp != nullptr) {
      lv_display_add_event_cb(disp, JpegStream::display_event_, LV_EVENT_REFR_START, this);
      lv_display_add_event_cb(disp, JpegStream::display_event_, LV_EVENT_RENDER_START, this);
      lv_display_add_event_cb(disp, JpegStream::display_event_, LV_EVENT_REFR_READY, this);
      lv_display_add_event_cb(disp, JpegStream::display_event_, LV_EVENT_FLUSH_START, this);
      lv_display_add_event_cb(disp, JpegStream::display_event_, LV_EVENT_FLUSH_FINISH, this);
      this->display_events_added_ = true;
    }
  }
  this->enable_loop();
  if (!this->running_.exchange(true)) {
    this->reset_stats_();
    xTaskNotifyGive(this->task_);
  }
}

void JpegStream::stop() { this->running_ = false; }

void JpegStream::loop() {
  const int8_t index = this->ready_.load();
  if (index >= 0) {
    lv_draw_buf_t *draw_buf = &this->draw_buf_[index];
    lv_draw_buf_init(draw_buf, this->ready_width_, this->ready_height_, LV_COLOR_FORMAT_RGB565, this->ready_stride_,
                     this->frame_buf_[index], this->frame_buf_size_);
    lv_canvas_set_draw_buf(this->canvas_, draw_buf);
    if (this->ready_width_ != this->canvas_width_ || this->ready_height_ != this->canvas_height_) {
      this->canvas_width_ = this->ready_width_;
      this->canvas_height_ = this->ready_height_;
      lv_obj_set_size(this->canvas_, this->canvas_width_, this->canvas_height_);
    }
    lv_obj_invalidate(this->canvas_);
    const uint32_t now_us = micros();
    const uint32_t handoff = now_us - this->ready_at_us_;
    this->handoff_us_ += handoff;
    this->handoff_us_max_ = std::max(this->handoff_us_max_, handoff);
    if (this->stats_shown_ > 0)
      this->gap_us_max_ = std::max(this->gap_us_max_, now_us - this->last_shown_us_);
    this->last_shown_us_ = now_us;
    this->stats_shown_++;
    this->shown_ = index;
    this->ready_ = -1;
    this->frame_count_++;
    this->frame_callback_.call();
  }

  const int error = this->error_.exchange(0);
  if (error != 0) {
    if (error > HTTP_STATUS_BASE) {
      ESP_LOGW(TAG, "HTTP status %d", error - HTTP_STATUS_BASE);
    } else {
      ESP_LOGW(TAG, "Frame failed: %s", esp_err_to_name(error));
    }
    this->stats_errors_++;
    this->error_callback_.call();
  }

  if (this->stats_interval_ > 0 && this->running_ && millis() - this->stats_start_ >= this->stats_interval_)
    this->log_stats_();

  if (!this->running_ && this->ready_ < 0)
    this->disable_loop();
}

void JpegStream::display_event_(lv_event_t *event) {
  auto *self = static_cast<JpegStream *>(lv_event_get_user_data(event));
  if (!self->running_)
    return;
  switch (lv_event_get_code(event)) {
    case LV_EVENT_REFR_START:
      self->refresh_start_us_ = micros();
      self->refresh_rendered_ = false;
      self->refresh_flush_us_ = 0;
      break;
    case LV_EVENT_FLUSH_START:
      self->flush_start_us_ = micros();
      break;
    case LV_EVENT_FLUSH_FINISH:
      if (self->flush_start_us_ != 0)
        self->refresh_flush_us_ += micros() - self->flush_start_us_;
      break;
    case LV_EVENT_RENDER_START:
      self->refresh_rendered_ = true;
      break;
    case LV_EVENT_REFR_READY:
      if (self->refresh_rendered_ && self->refresh_start_us_ != 0) {
        const uint32_t took = micros() - self->refresh_start_us_;
        self->refresh_us_ += took;
        self->refresh_us_max_ = std::max(self->refresh_us_max_, took);
        self->flush_us_ += self->refresh_flush_us_;
        self->refreshes_++;
      }
      break;
    default:
      break;
  }
}

void JpegStream::reset_stats_() {
  {
    LockGuard guard(this->stats_mutex_);
    this->task_stats_ = TaskStats{};
  }
  this->stats_start_ = millis();
  this->stats_dropped_start_ = this->dropped_count_;
  this->stats_shown_ = this->stats_errors_ = 0;
  this->handoff_us_ = this->handoff_us_max_ = this->gap_us_max_ = 0;
  this->refreshes_ = this->refresh_us_ = this->refresh_us_max_ = this->flush_us_ = 0;
}

void JpegStream::log_stats_() {
  TaskStats task;
  {
    LockGuard guard(this->stats_mutex_);
    task = this->task_stats_;
  }
  const uint32_t now = millis();
  const uint32_t dropped = this->dropped_count_;
  if (now > this->stats_start_) {
    const float seconds = (now - this->stats_start_) / 1000.0f;
    const auto avg_ms = [](uint32_t sum_us, uint32_t count) { return count ? sum_us / 1000.0f / count : 0.0f; };
    ESP_LOGI(TAG,
             "Stats over %.1f s: %.1f fps shown (%" PRIu32 " shown, %" PRIu32 " received, %" PRIu32
             " skipped, %" PRIu32 " errors)\n"
             "  JPEG size: avg %.1f KB, %.0f KB/s\n"
             "  Receive:   avg %.1f ms, max %.1f ms\n"
             "  Decode:    avg %.1f ms, max %.1f ms\n"
             "  Hand-off:  avg %.1f ms, max %.1f ms (task -> canvas in the main loop)\n"
             "  Frame gap: max %.1f ms\n"
             "  LVGL refresh: %" PRIu32 " x, avg %.1f ms, max %.1f ms (layout + render + flush)\n"
             "  LVGL flush:   avg %.1f ms per refresh (flush_cb: copy to the display)",
             seconds, this->stats_shown_ / seconds, this->stats_shown_, task.received,
             dropped - this->stats_dropped_start_, this->stats_errors_,
             task.received ? task.bytes / 1024.0f / task.received : 0.0f, task.bytes / 1024.0f / seconds,
             avg_ms(task.receive_us, task.received), task.receive_us_max / 1000.0f,
             avg_ms(task.decode_us, task.decoded), task.decode_us_max / 1000.0f,
             avg_ms(this->handoff_us_, this->stats_shown_), this->handoff_us_max_ / 1000.0f,
             this->gap_us_max_ / 1000.0f, this->refreshes_, avg_ms(this->refresh_us_, this->refreshes_),
             this->refresh_us_max_ / 1000.0f, avg_ms(this->flush_us_, this->refreshes_));
  }
  this->reset_stats_();
}

void JpegStream::task_entry_(void *arg) { static_cast<JpegStream *>(arg)->run_task_(); }

void JpegStream::wait_while_running_(uint32_t ms) {
  for (uint32_t waited = 0; waited < ms && this->running_; waited += 50)
    vTaskDelay(pdMS_TO_TICKS(50));
}

void JpegStream::run_task_() {
  char url[MAX_URL_LENGTH];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (this->running_) {
      const uint32_t begin = millis();
      const uint32_t begin_us = micros();
      {
        LockGuard guard(this->url_mutex_);
        strncpy(url, this->url_.c_str(), sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        this->url_changed_ = false;
      }
      if (url[0] == '\0') {
        this->wait_while_running_(200);
        continue;
      }

      int result = this->open_(url);
      if (result == 0) {
        if (this->multipart_) {
          // Runs until stop(), a new URL or an error; the stream connection cannot be reused.
          result = this->read_stream_();
          this->close_();
        } else {
          result = this->read_image_();
          if (result == 0) {
            this->add_receive_stats_(micros() - begin_us, this->jpeg_len_);
            result = this->present_(begin);
          }
        }
      }
      if (result != 0) {
        if (this->running_) {
          this->error_ = result;
          App.wake_loop_threadsafe();
        }
        // Start the next attempt on a fresh connection.
        this->close_();
        this->wait_while_running_(RETRY_DELAY_MS);
        continue;
      }
      if (!this->multipart_) {
        const uint32_t elapsed = millis() - begin;
        if (elapsed < this->min_interval_)
          this->wait_while_running_(this->min_interval_ - elapsed);
      }
    }
    // Stopped: drop the (kept-alive) connection until the next start().
    this->close_();
  }
}

void JpegStream::close_() {
  if (this->client_ != nullptr)
    esp_http_client_close(this->client_);
  this->connected_ = false;
}

esp_err_t JpegStream::http_event_(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || strcasecmp(event->header_key, "Content-Type") != 0)
    return ESP_OK;
  auto *self = static_cast<JpegStream *>(event->user_data);
  strncpy(self->content_type_, event->header_value, sizeof(self->content_type_) - 1);
  self->content_type_[sizeof(self->content_type_) - 1] = '\0';
  return ESP_OK;
}

int JpegStream::open_(const char *url) {
  // One client per server (and credentials): esp_http_client_set_url() keeps the old credentials when
  // the new URL has none, and a kept-alive connection only works for the same server.
  const char *host = strstr(url, "://");
  const char *path = host != nullptr ? strchr(host + 3, '/') : nullptr;
  const std::string origin = path != nullptr ? std::string(url, path - url) : std::string(url);
  if (this->client_ != nullptr && origin != this->origin_) {
    esp_http_client_cleanup(this->client_);
    this->client_ = nullptr;
    this->connected_ = false;
  }

  if (this->client_ == nullptr) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = (int) this->timeout_;
    config.buffer_size = RX_BUFFER_SIZE;
    config.buffer_size_tx = 1024;  // the camera proxy URL carries a long access token
    config.auth_type = HTTP_AUTH_TYPE_BASIC;  // only used when the URL has user:password@
    config.event_handler = JpegStream::http_event_;
    config.user_data = this;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    this->client_ = esp_http_client_init(&config);
    if (this->client_ == nullptr)
      return ESP_ERR_NO_MEM;
    this->origin_ = origin;
  } else if (esp_http_client_set_url(this->client_, url) != ESP_OK) {
    return ESP_ERR_INVALID_ARG;
  }

  // A kept-alive connection may have been closed by the server in the meantime: then retry once on
  // a new connection.
  for (int attempt = this->connected_ ? 2 : 1; attempt > 0; attempt--) {
    this->content_type_[0] = '\0';
    esp_err_t err = esp_http_client_open(this->client_, 0);
    int64_t length = err == ESP_OK ? esp_http_client_fetch_headers(this->client_) : 0;
    if (err == ESP_OK && length >= 0)
      break;
    this->close_();
    if (attempt == 1) {
      if (err != ESP_OK)
        return err;
      return length == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_ERR_HTTP_FETCH_HEADER;
    }
  }
  this->connected_ = true;

  const int status = esp_http_client_get_status_code(this->client_);
  if (status != 200)
    return HTTP_STATUS_BASE + status;
  this->multipart_ = strncasecmp(this->content_type_, "multipart/", 10) == 0;
  return 0;
}

int JpegStream::read_image_() {
  this->jpeg_len_ = 0;
  bool overflow = false;
  while (true) {
    uint8_t *target = this->jpeg_buf_ + this->jpeg_len_;
    int space = (int) (this->max_jpeg_size_ - this->jpeg_len_);
    if (space == 0) {
      // Too big: read on into the spare buffer so the connection stays usable.
      overflow = true;
      target = this->rx_buf_;
      space = RX_BUFFER_SIZE;
    }
    const int len =
        esp_http_client_read(this->client_, reinterpret_cast<char *>(target), std::min<int>(space, RX_BUFFER_SIZE));
    if (len < 0)
      return len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    if (len == 0)
      break;
    if (!overflow)
      this->jpeg_len_ += len;
  }
  if (!esp_http_client_is_complete_data_received(this->client_)) {
    // The server closed the connection early.
    this->close_();
    return ESP_ERR_HTTP_CONNECTION_CLOSED;
  }
  if (overflow)
    return ESP_ERR_INVALID_SIZE;
  if (this->jpeg_len_ == 0)
    return ESP_ERR_INVALID_RESPONSE;
  return 0;
}

// Content-Length of a multipart part header, or -1.
static int64_t part_content_length(const char *header) {
  for (const char *p = header; *p != '\0'; p++) {
    if ((p == header || p[-1] == '\n') && strncasecmp(p, "Content-Length:", 15) == 0) {
      char *end = nullptr;
      const unsigned long value = strtoul(p + 15, &end, 10);
      return end != p + 15 ? (int64_t) value : -1;
    }
  }
  return -1;
}

int JpegStream::read_stream_() {
  // Each part: [\r\n]--boundary\r\n<headers>\r\n\r\n<Content-Length bytes of JPEG>
  char header[MAX_PART_HEADER];
  size_t header_len = 0;
  bool in_body = false;
  bool skip = false;
  size_t remaining = 0;
  uint32_t part_begin = 0;
  uint32_t part_begin_us = 0;

  while (this->running_ && !this->url_changed_) {
    // esp_http_client_read() only returns when the buffer is full (or on a timeout), so in a part
    // ask for no more than its remaining bytes: then the frame is decoded as soon as it is complete.
    const int want = in_body ? (int) std::min<size_t>(remaining, RX_BUFFER_SIZE) : (int) RX_BUFFER_SIZE;
    const int len = esp_http_client_read(this->client_, reinterpret_cast<char *>(this->rx_buf_), want);
    if (len < 0)
      return len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    if (len == 0)
      return ESP_ERR_HTTP_CONNECTION_CLOSED;  // the server ended the stream

    int pos = 0;
    while (pos < len) {
      if (!in_body) {
        // Collect the part header up to the empty line.
        while (pos < len && !in_body) {
          if (header_len == sizeof(header) - 1)
            return ESP_ERR_INVALID_RESPONSE;
          header[header_len++] = (char) this->rx_buf_[pos++];
          if (header_len >= 4 && memcmp(header + header_len - 4, "\r\n\r\n", 4) == 0) {
            header[header_len] = '\0';
            if (header_len == 4) {  // only the line end after the previous part
              header_len = 0;
              continue;
            }
            const int64_t length = part_content_length(header);
            if (length <= 0)
              return ESP_ERR_NOT_SUPPORTED;  // parts without Content-Length
            remaining = (size_t) length;
            skip = remaining > this->max_jpeg_size_;
            this->jpeg_len_ = 0;
            part_begin = millis();
            part_begin_us = micros();
            header_len = 0;
            in_body = true;
          }
        }
        continue;
      }

      const size_t take = std::min<size_t>(remaining, len - pos);
      if (!skip)
        memcpy(this->jpeg_buf_ + this->jpeg_len_, this->rx_buf_ + pos, take);
      this->jpeg_len_ += take;
      pos += take;
      remaining -= take;
      if (remaining > 0)
        continue;

      in_body = false;
      if (!skip)
        this->add_receive_stats_(micros() - part_begin_us, this->jpeg_len_);
      if (skip) {
        this->error_ = ESP_ERR_INVALID_SIZE;
        App.wake_loop_threadsafe();
      } else if (this->ready_ >= 0 ||
                 (this->min_interval_ > 0 && millis() - this->last_present_ < this->min_interval_)) {
        // The previous frame is not on the screen yet (or too soon): skip this one, the next is better.
        this->dropped_count_++;
      } else {
        const int result = this->present_(part_begin);
        if (result != 0) {
          // A broken frame: report it and carry on with the next one.
          this->error_ = result;
          App.wake_loop_threadsafe();
        }
      }
    }
  }
  return 0;
}

int JpegStream::present_(uint32_t begin) {
  // Wait until the main loop has shown the previous frame before reusing its buffer.
  while (this->running_ && this->ready_ >= 0)
    vTaskDelay(pdMS_TO_TICKS(5));
  if (!this->running_)
    return 0;
  const uint8_t target = this->shown_ == 0 ? 1 : 0;
  const int result = this->decode_(target);
  if (result != 0)
    return result;
  this->last_present_ = millis();
  this->last_frame_time_ = this->last_present_ - begin;
  this->ready_at_us_ = micros();
  this->ready_ = target;
  App.wake_loop_threadsafe();
  return 0;
}

void JpegStream::add_receive_stats_(uint32_t took_us, size_t bytes) {
  LockGuard guard(this->stats_mutex_);
  this->task_stats_.received++;
  this->task_stats_.bytes += bytes;
  this->task_stats_.receive_us += took_us;
  this->task_stats_.receive_us_max = std::max(this->task_stats_.receive_us_max, took_us);
}

int JpegStream::decode_(uint8_t target) {
  jpeg_decode_picture_info_t info = {};
  esp_err_t err = jpeg_decoder_get_info(this->jpeg_buf_, this->jpeg_len_, &info);
  if (err != ESP_OK)
    return err;
  if (info.width > this->max_width_ || info.height > this->max_height_)
    return ESP_ERR_INVALID_SIZE;

  jpeg_decode_cfg_t cfg = {
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,  // little endian, as LVGL expects
      .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
  };
  uint32_t out_size = 0;
  const uint32_t decode_start = micros();
  err = jpeg_decoder_process(this->decoder_, &cfg, this->jpeg_buf_, this->jpeg_len_, this->frame_buf_[target],
                             this->frame_buf_size_, &out_size);
  if (err != ESP_OK) {
    // The decoder engine can get stuck after a bad image; recreate it and try once more.
    jpeg_del_decoder_engine(this->decoder_);
    this->decoder_ = nullptr;
    if (!this->create_decoder_())
      return ESP_FAIL;
    err = jpeg_decoder_process(this->decoder_, &cfg, this->jpeg_buf_, this->jpeg_len_, this->frame_buf_[target],
                               this->frame_buf_size_, &out_size);
    if (err != ESP_OK)
      return err;
  }
  {
    const uint32_t took = micros() - decode_start;
    LockGuard guard(this->stats_mutex_);
    this->task_stats_.decoded++;
    this->task_stats_.decode_us += took;
    this->task_stats_.decode_us_max = std::max(this->task_stats_.decode_us_max, took);
  }

  // Rows are padded to whole MCU blocks: 16 pixels wide for 4:2:0 and 4:2:2, otherwise 8.
  const bool wide_mcu =
      info.sample_method == JPEG_DOWN_SAMPLING_YUV420 || info.sample_method == JPEG_DOWN_SAMPLING_YUV422;
  const uint32_t row_pixels = align_up(info.width, wide_mcu ? 16 : 8);
  this->ready_width_ = info.width;
  this->ready_height_ = info.height;
  this->ready_stride_ = row_pixels * 2;
  return 0;
}

}  // namespace esphome::jpeg_stream

#endif  // USE_ESP32
