#pragma once

#ifdef USE_ESP32

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "esp_http_client.h"

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_JPEG_STREAM_HW_DECODER
#include "driver/jpeg_decode.h"
#endif
#ifdef USE_JPEG_STREAM_SW_DECODER
#include <JPEGDEC.h>
#endif

namespace esphome::jpeg_stream {

/// Receives JPEG frames from an URL in a background task (an MJPEG stream, or a single image that is
/// fetched again and again), decodes them in that task (ESP32-P4 hardware JPEG decoder, or JPEGDEC in
/// software) and shows each frame on an LVGL canvas. The main loop only swaps the canvas buffer.
class JpegStream : public Component {
 public:
  JpegStream(uint16_t max_width, uint16_t max_height, uint32_t max_jpeg_size)
      : max_width_(max_width), max_height_(max_height), max_jpeg_size_(max_jpeg_size) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  void set_timeout(uint32_t timeout_ms) { this->timeout_ = timeout_ms; }
  void set_min_interval(uint32_t interval_ms) { this->min_interval_ = interval_ms; }
  /// Log performance statistics every interval while the stream runs (0 = off).
  void set_stats_interval(uint32_t interval_ms) { this->stats_interval_ = interval_ms; }
  /// Decode with the ESP32-P4 hardware JPEG decoder instead of JPEGDEC.
  void set_hardware_decoder(bool hardware) { this->hardware_decoder_ = hardware; }

  /// Thread-safe; the new URL is used for the next frame (an open MJPEG stream is reconnected).
  void set_url(const std::string &url);
  void start();
  void stop();
  bool is_running() const { return this->running_; }
  /// millis() of the last frame put on the canvas (0: none since boot).
  uint32_t get_last_shown() const { return this->last_shown_ms_; }

  /// Number of frames shown since boot.
  uint32_t get_frame_count() const { return this->frame_count_; }
  /// Single images: time from the request until the frame was decoded. MJPEG: time from the start of
  /// the part until it was decoded. In ms.
  uint32_t get_last_frame_time() const { return this->last_frame_time_; }
  /// Number of MJPEG frames skipped because the previous frame was not shown yet.
  uint32_t get_dropped_count() const { return this->dropped_count_; }

  template<typename F> void add_on_frame_callback(F &&callback) {
    this->frame_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_error_callback(F &&callback) {
    this->error_callback_.add(std::forward<F>(callback));
  }

 protected:
  static void task_entry_(void *arg);
  static esp_err_t http_event_(esp_http_client_event_t *event);
  void run_task_();
  bool allocate_();
  // These return 0, an esp_err_t, or a HTTP status code offset by HTTP_STATUS_BASE (see the .cpp).
  int open_(const char *url);
  int read_image_();
  int read_stream_();
  int decode_(uint8_t target);
#ifdef USE_JPEG_STREAM_HW_DECODER
  int decode_hardware_(uint8_t target);
  bool create_decoder_();
#endif
#ifdef USE_JPEG_STREAM_SW_DECODER
  int decode_software_(uint8_t target);
  static int draw_software_(JPEGDRAW *draw);
#endif
  /// Decodes jpeg_buf_ into the free frame buffer and hands it to the main loop.
  int present_(uint32_t begin);
  void close_();
  void log_stats_();
  void reset_stats_();
  void add_receive_stats_(uint32_t took_us, size_t bytes);
  static void display_event_(lv_event_t *event);
  void wait_while_running_(uint32_t ms);
  /// Wakes the stream task when it waits for a frame to be shown, a retry delay, or a stop.
  void wake_task_();

  lv_obj_t *canvas_{nullptr};
  uint16_t max_width_;
  uint16_t max_height_;
  uint32_t max_jpeg_size_;
  uint32_t timeout_{5000};
  uint32_t min_interval_{0};
  uint32_t stats_interval_{0};
  bool hardware_decoder_{false};

  Mutex url_mutex_;
  std::string url_;
  std::atomic<bool> url_changed_{false};

  TaskHandle_t task_{nullptr};
  SemaphoreHandle_t wake_{nullptr};      // given by the main loop: frame shown, stop(), new URL
  std::atomic<uint32_t> stack_free_{0};  // bytes of the task's stack never used (high-water mark)
  esp_http_client_handle_t client_{nullptr};
  std::string origin_;        // scheme://[user:password@]host[:port] the client was created for
  bool connected_{false};     // the client holds an open connection that can be reused
  bool multipart_{false};     // the current response is an MJPEG stream
  char content_type_[64]{};   // Content-Type of the current response
  uint8_t *rx_buf_{nullptr};  // receive buffer for MJPEG streams
#ifdef USE_JPEG_STREAM_HW_DECODER
  jpeg_decoder_handle_t decoder_{nullptr};
#endif
#ifdef USE_JPEG_STREAM_SW_DECODER
  // JPEGDEC state (some 20 kB, PSRAM when there is any), created on the first start()
  RAMUniquePtr<JPEGDEC> jpegdec_;
#endif

  // Compressed image (PSRAM) and two decoded RGB565 frames (PSRAM, aligned for the decoder)
  uint8_t *jpeg_buf_{nullptr};
  size_t jpeg_len_{0};
  uint8_t *frame_buf_[2]{nullptr, nullptr};
  size_t frame_buf_size_{0};
  lv_draw_buf_t draw_buf_[2]{};

  // Hand-off between the task and the main loop: the task decodes into the buffer that is not shown,
  // sets ready_ and waits until loop() has put it on the canvas (ready_ back to -1).
  std::atomic<bool> running_{false};
  std::atomic<int8_t> ready_{-1};
  std::atomic<int8_t> shown_{-1};
  uint16_t ready_width_{0};
  uint16_t ready_height_{0};
  uint32_t ready_stride_{0};
  std::atomic<int> error_{0};
  std::atomic<uint32_t> last_frame_time_{0};
  std::atomic<uint32_t> dropped_count_{0};
  uint32_t last_present_{0};
  uint32_t frame_count_{0};
  uint32_t last_shown_ms_{0};
  uint32_t start_ms_{0};             // millis() of the last start() that started the stream
  bool first_frame_pending_{false};  // the first frame since that start() has not been shown yet
  uint16_t canvas_width_{0};
  uint16_t canvas_height_{0};

  // Statistics. TaskStats is written by the stream task under stats_mutex_; the rest is only used
  // in the main loop (loop() and the LVGL display events, which run from the LVGL timer handler).
  struct TaskStats {
    uint32_t received{0};    // complete JPEGs received
    uint32_t bytes{0};       // their total size
    uint32_t receive_us{0};  // receive time: request (single image) or part start (MJPEG) -> last byte
    uint32_t receive_us_max{0};
    uint32_t decoded{0};    // JPEGs decoded
    uint32_t decode_us{0};  // decode time
    uint32_t decode_us_max{0};
  };
  Mutex stats_mutex_;
  TaskStats task_stats_{};
  std::atomic<uint32_t> ready_at_us_{0};  // micros() when the task handed a frame to the main loop
  uint32_t stats_start_{0};
  uint32_t stats_dropped_start_{0};
  uint32_t stats_shown_{0};
  uint32_t stats_errors_{0};
  uint32_t handoff_us_{0};  // ready_ set by the task -> frame put on the canvas in loop()
  uint32_t handoff_us_max_{0};
  uint32_t last_shown_us_{0};
  uint32_t gap_us_max_{0};  // longest time between two frames on the canvas
  uint32_t refresh_start_us_{0};
  bool refresh_rendered_{false};
  uint32_t refreshes_{0};   // LVGL refresh cycles that rendered something
  uint32_t refresh_us_{0};  // LVGL refresh time: layout + render + flush to the display
  uint32_t refresh_us_max_{0};
  uint32_t flush_start_us_{0};
  uint32_t refresh_flush_us_{0};  // flush_cb time within the current refresh
  uint32_t flush_us_{0};          // flush_cb time (copy to the display) summed over the window
  bool display_events_added_{false};

  CallbackManager<void()> frame_callback_;
  CallbackManager<void()> error_callback_;
};

}  // namespace esphome::jpeg_stream

#endif  // USE_ESP32
