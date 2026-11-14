# jpeg_stream

An ESPHome component that shows a live MJPEG stream (or a JPEG image fetched again and again) on
an LVGL canvas. Networking and decoding run in a background task, so the main loop only swaps the
canvas buffer when a new frame is ready.

It was built to show a doorbell camera on a wall display: go2rtc turns the camera's RTSP stream
into MJPEG, and the display shows it at the camera's 10 fps with under a second of delay.

Status: an external component, used daily on an ESP32-P4 display. It is meant as a starting point
for discussing native MJPEG support in ESPHome; see *Proposal for ESPHome* at the end.

## Configuration

```yaml
external_components:
  - source: github://martijnbuts/esphome-p4-doorbell@main
    components: [jpeg_stream]

lvgl:
  pages:
    - id: camera_page
      widgets:
        - canvas:
            id: camera_canvas
            width: 640
            height: 480

jpeg_stream:
  - id: camera_stream
    canvas: camera_canvas
    max_width: 640
    max_height: 480

# for example on a button:
#   - jpeg_stream.start:
#       id: camera_stream
#       url: "http://user:password@192.168.1.10:1984/api/stream.mjpeg?src=doorbell"
#   - jpeg_stream.stop: camera_stream
```

| Option | Default | What it does |
| --- | --- | --- |
| `canvas` | (required) | The LVGL canvas to draw on; the component sizes it to the picture |
| `url` | | The stream or image URL; can also be given (templated) in `jpeg_stream.start` and `jpeg_stream.set_url` |
| `decoder` | `hardware` on the ESP32-P4, `software` elsewhere | `hardware`: the ESP32-P4 JPEG decoder. `software`: JPEGDEC, the decoder ESPHome's `runtime_image` (`online_image`) uses |
| `max_width`, `max_height` | `640`, `480` | Largest picture; sets the size of the two frame buffers |
| `max_jpeg_size` | `256 kB` | Largest compressed frame |
| `timeout` | `5s` | Network timeout |
| `min_interval` | `0ms` | Least time between two frames (lower CPU use) |
| `stats_interval` | `0s` (off) | Log receive, decode, hand-off and LVGL refresh times every interval while the stream runs |
| `on_frame`, `on_error` | | Automations after each shown frame or failed frame |

Actions: `jpeg_stream.start` (optionally with `url`), `jpeg_stream.stop`, `jpeg_stream.set_url`.
Condition: `jpeg_stream.is_running`.

## How it works

- **Source:** the response's Content-Type decides the mode. `multipart/x-mixed-replace` (for
  example go2rtc's `/api/stream.mjpeg`) is read as one long stream; every part needs a
  `Content-Length` header. Anything else is treated as a single JPEG and fetched again over a
  kept-alive connection (for example Home Assistant's camera proxy). Credentials in the URL are
  sent as HTTP Basic authentication; `https` uses the certificate bundle.
- **Background task:** `esp_http_client` receives the frames and decodes them into one of two
  RGB565 frame buffers in PSRAM. The main loop then points the canvas at that buffer
  (`lv_canvas_set_draw_buf`), so a frame is never copied after decoding.
- **Lowest latency:** a frame that arrives while the previous one is not on the screen yet is
  skipped rather than queued. The task waits on a semaphore that the main loop gives as soon as
  the previous frame is on the canvas (and on `stop()` or a new URL), so there is no polling.
- **Decoders:**
  - `hardware` (ESP32-P4): `esp_driver_jpeg`. After a failed decode the engine is recreated
    once (following the approach of the transcoder and picture_viewer components in
    [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome)).
  - `software`: [JPEGDEC](https://github.com/esphome-libs/JPEGDEC) (the same build as
    `runtime_image`) in framebuffer mode, so it writes the rows straight into the frame
    buffer. On the ESP32-S3 it uses the chip's SIMD colour conversion.
- **Memory:** nothing is allocated until the first `start()`. Then: two frame buffers
  (`max_width` × `max_height` × 2, 600 kB each at 640×480), the JPEG buffer (`max_jpeg_size`), a 4 kB
  receive buffer and an 8 kB task stack in internal RAM; for `software` also JPEGDEC's state.

## Measured

Doorbell: Reolink D340P sub-stream (640×480, 10 fps) as MJPEG from go2rtc, 13–22 kB per frame.

| | `hardware`: ESP32-P4, 1024×600 MIPI DSI | `software`: ESP32-S3 (JPEGDEC), 800×480 RGB panel |
| --- | --- | --- |
| Frames shown | 10.0 fps (every frame) | 7.5 fps (about 1 in 4 skipped) |
| Receive per frame (task) | 3.5 ms | 15 ms |
| Decode per frame (task) | 3.5 ms | 70 ms |
| LVGL refresh per frame (main loop) | 21–51 ms (see PERFORMANCE.md) | 109 ms, of which 39 ms copying to the panel |
| First frame after `start()` | 119 ms | 117 ms |
| Memory while streaming | about 1.5 MB PSRAM | about 1.7 MB PSRAM, internal RAM 160 → 153 kB free |

The ESP32-S3 numbers are averages over three 10 s windows on 14 November 2026 (no errors). On the
S3 the limit is not the decoder but LVGL drawing the 640×480 canvas in the main loop: about 9
refreshes per second at most, and the main loop's longest pass rose to 145 ms while the stream ran.
A smaller picture (for example go2rtc scaling to 480×360) or `min_interval` lowers that load.

The P4 numbers are from the more detailed measurements in [docs/PERFORMANCE.md](../../docs/PERFORMANCE.md)
(12 October 2026; synthetic 30 and 60 fps sources, start-up time, CPU per task); the first-frame
time was measured again on 14 November 2026 with this version.

## Tests

[`tests/components/jpeg_stream`](../../tests/components/jpeg_stream) has test configurations in
the layout of ESPHome's own tests: ESP32-S3 (software), ESP32-P4 (hardware) and ESP32-P4 with
`decoder: software`. They exercise every option, action and the condition; see
[`tests/README.md`](../../tests/README.md).

## Tested on

- ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-7B), ESP-IDF 6.1, LVGL 9.6: `hardware`, in daily
  use since October 2026
- ESP32-S3 (N16R8, octal PSRAM) with an 800×480 RGB panel, ESP-IDF 5.5, LVGL 9.6: `software`,
  live doorbell stream on 14 November 2026
- ESPHome 2026.12.0-dev (14 November 2026)

## Limitations

- **MJPEG parts need a `Content-Length` header.** go2rtc sends one; a stream that only uses the
  boundary is refused (`ESP_ERR_NOT_SUPPORTED`).
- **The canvas is drawn by LVGL in the main loop.** At 640×480 that takes 21–51 ms per frame on
  the P4 (depending on rotation, rounded corners and PPA use, see PERFORMANCE.md) and about
  110 ms on the S3, above the 50 ms ESPHome allows a component per loop.
- **Own HTTP client.** It uses `esp_http_client` directly, not ESPHome's `http_request` component,
  because it needs a long-running stream in its own task.
- **ESP-IDF and PSRAM only.**
- **ESP32-P4: DMA2D is shared** by the JPEG decoder and the PPA; see *DMA2D: JPEG decoder and PPA*
  in PERFORMANCE.md for a known ESP-IDF issue.

## Proposal for ESPHome

ESPHome already has most of the parts: `online_image` downloads an image and `runtime_image`
decodes it with JPEGDEC (decoding while it downloads since 2026.11). What is missing for a live
camera view is a stream, a background task, and the P4's hardware decoder. I think it fits
upstream better as pieces of what exists than as a new component:

1. **runtime_image: use the ESP32-P4 hardware JPEG decoder automatically**, with JPEGDEC as the
   fallback (for example for progressive JPEGs). That matches the review comments on
   [esphome#16902](https://github.com/esphome/esphome/pull/16902): no extra YAML, at most an
   option to turn it off.
2. **online_image: MJPEG streams.** A `multipart/x-mixed-replace` source with continuous updates,
   received and decoded in a background task, double-buffered so LVGL shows complete frames
   (runtime_image already has `double_buffer`).
3. Optionally, **skip frames instead of queueing**, and the `stats_interval` diagnostics.

Open questions for the maintainers:

- On chips without a 2D accelerator, LVGL drawing the frame costs more than decoding it (S3:
  109 ms against 70 ms). Should a stream write straight into the display's frame buffer when
  the picture is not covered by other widgets, or is drawing through LVGL the expected way?
- Is a background task acceptable for `online_image`, or should streaming stay in the main loop
  with a time budget per loop (like the current decoding)?
- Should the stream use `http_request`, which would need a streaming/long-running mode, or is a
  direct `esp_http_client` in a task fine for ESP32-only streaming?
- Hardware decoding outputs whole MCU blocks (row stride rounded up to 8 or 16 pixels). Can
  `runtime_image` buffers take a stride, or should the decoder write into a padded buffer and copy?

## License

MIT, like the rest of this repository.
