# Performance

Measured on 12 October 2026. On this board the network and the hardware JPEG decoder are cheap
(about 7 ms per frame together). **The limit is the LVGL refresh of the 640×480 picture: about
41 ms per frame on the main loop with LVGL `rotation: 180`, so at most about 21 frames per second
reach the screen. Without the rotation it is 28 ms, and with ESP-IDF 6.0 and LVGL drawing the
picture with the PPA 21 ms: measured, a 30 fps source is shown completely and a 60 fps source at
39 fps.** The camera itself delivers 10 fps, so the doorbell page shows every frame.

**The start-up was slower than the frames:** 7.7 s from opening the page to the first frame,
because go2rtc started ffmpeg only then. With a go2rtc `preload` it is 0.13 s
(see [Start-up time](#start-up-time)).

This page lists what was measured, how, and where the time could still be won.

## Hardware acceleration at a glance

The ESP32-P4 has three blocks that do the heavy work here: the JPEG codec, the PPA (pixel
processing accelerator) and the 2D-DMA controller (DMA2D) that moves the data for both. Which one
does what, and when it is on (checked against ESPHome dev, LVGL 9.6.0 and ESP-IDF 6.1 on
10 November 2026):

| Work | Hardware | When it is on | Who turns it on |
| --- | --- | --- | --- |
| Decoding the doorbell JPEG into the canvas buffer (RGB565) | JPEG codec, decoder | Always, every frame, in the `jpeg_stream` background task | `jpeg_stream` |
| Encoding a screenshot (RGB565 → JPEG, 4:4:4) | JPEG codec, encoder | Only when Home Assistant asks for a picture, in the main loop | `lvgl_screenshot` (optional) |
| Turning every LVGL refresh by 90/180/270° | PPA, scale-rotate-mirror | Only with an LVGL `rotation`; any ESP-IDF version | ESPHome (its own flush code) |
| LVGL fills (backgrounds, rectangles) | PPA, fill | ESP-IDF 6.0 or newer | ESPHome sets `LV_USE_PPA` (since LVGL 9.6) |
| LVGL drawing images, such as the doorbell canvas | PPA, blend | ESP-IDF 6.0 or newer **and** the build flag `-DLV_USE_PPA_IMG=1`; only unscaled, unrotated, fully opaque images without masks | You (off by default in LVGL) |
| Moving the data for all of the above | DMA2D | Whenever one of the above runs | The ESP-IDF drivers |

What this means in practice:

- **With ESPHome's default ESP-IDF (5.5.6)** only the JPEG codec and, with a rotation, the PPA
  rotation are used; LVGL draws everything else in software. That is enough for the 10 fps of the
  camera.
- **The biggest gain is not hardware but leaving work out**: no `rotation` (13 ms per frame) and
  no `clip_corner` on the card around the picture (10 ms). `LV_USE_PPA_IMG` then saves another 8 ms.
- **The JPEG codec is one engine.** The ESP-IDF driver takes a lock for each decode or encode, so
  a screenshot and a doorbell frame never run at the same time; one waits a few ms for the other.
- **DMA2D is shared without such a lock** between the JPEG codec and the PPA. That is a known risk
  on the ESP32-P4; see [DMA2D: JPEG decoder and PPA](#dma2d-jpeg-decoder-and-ppa).

## Test setup

| Part | Value |
| --- | --- |
| Board | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B: ESP32-P4 rev 1.3 at 360 MHz, 32 MB flash, PSRAM hex mode 200 MHz |
| Wi-Fi | ESP32-C6 co-processor over SDIO (`esp32_hosted`) |
| Firmware | ESPHome 2026.11.0-dev, ESP-IDF 5.5.5, LVGL 9.5.0, `compiler_optimization: PERF` (-O2), `execute_from_psram: true` |
| Display | 1024×600 MIPI-DSI; LVGL `rotation: 180` (software rotation, PPA accelerated; removed for the later measurements, see below), `buffer_size: 100%` (one 1.2 MB draw buffer in PSRAM, partial render mode) |
| Page | 640×480 canvas inside a card; also running: touchscreen, voice assistant with micro_wake_word, media player (the doorbell audio plays as an announcement) |
| Source | go2rtc 1.9.14: Reolink D340P sub-stream (640×480, 10 fps) turned into MJPEG by ffmpeg, 13–22 KB per JPEG. Synthetic load: ffmpeg `testsrc2` 640×480 at 30 or 60 fps, `-q:v 8` (about 17 KB per JPEG) |

## Results

Averages over 10-second windows. Windows in which `runtime_stats` printed its report are left
out (see *Measurement side effects*).

| Input | Shown | Skipped | Receive | Decode | Hand-off | LVGL refresh (of which flush) |
| --- | --- | --- | --- | --- | --- | --- |
| Camera, 10 fps | 10.0 fps | 0 % | 3.5 ms | 3.5 ms | 2.2 ms | 51.5 ms (17.4 ms) |
| Camera, 10 fps, no `clip_corner` | 10.0 fps | 0 % | 3.5 ms | 3.5 ms | 2.0 ms | 41.3 ms (17.4 ms) |
| Synthetic, 30 fps | 17.2 fps | 43 % | 3.5 ms | 4.2 ms | 26 ms | 57 ms (18.7 ms) |
| Synthetic, 30 fps, no `clip_corner` | 21.5 fps | 29 % | – | 4.0 ms | 16 ms | 45.7 ms (19.5 ms) |
| Synthetic, 60 fps | 16.4 fps | 73 % | 4.5 ms | 6.3–8.0 ms | 24 ms | 60 ms (19.5 ms) |

*Receive* is the time from the part header to the last byte of the JPEG. *Decode* is
`jpeg_decoder_process()`. *Hand-off* is the time from "frame ready" in the stream task until the
main loop puts it on the canvas. *LVGL refresh* is one refresh cycle that rendered something
(`LV_EVENT_REFR_START` to `LV_EVENT_REFR_READY`: layout, render and flush); *flush* is the time
spent in the flush callback (rotation plus the copy into the display frame buffer).

### Without the 180° rotation

The display was mounted upside down and LVGL turned every refresh back with `rotation: 180`
(software rotation, PPA accelerated). After removing it (and turning the display around), with the
camera at 10 fps, no `clip_corner`, averages over three 30-second windows:

| | With `rotation: 180` | Without rotation |
| --- | --- | --- |
| LVGL refresh | 41.3 ms | 28.0 ms |
| of which flush | 17.4 ms | 4.6 ms |
| Hand-off | 2.0 ms | 0.3–0.4 ms |
| Estimated ceiling (1000 ms / refresh) | ~21 fps (measured 21.5) | ~35 fps (not measured yet) |

The rotation cost 13 ms per refresh: the PPA rotation into a separate 1.2 MB buffer before the copy
into the display frame buffer. Without rotation LVGL also does not allocate that buffer. Touch
needs no change: ESPHome only turns the touch coordinates when LVGL has a rotation.

### ESP-IDF 6.0 and LVGL drawing with the PPA

ESPHome turns the LVGL PPA draw unit (`LV_USE_PPA`) on only with ESP-IDF 6.0 or newer, since LVGL
9.6 ([esphome#20257](https://github.com/esphome/esphome/pull/20257)): the LVGL PPA driver needs a
cache function that IDF 5.x lacks. That unit does fills by default; drawing images (such as the
canvas with the doorbell picture) is a separate LVGL option, `LV_USE_PPA_IMG`, that is off by
default. ESPHome keeps a `-D` build flag that sets an LVGL option:

```yaml
esphome:
  build_flags:
    - -DLV_USE_PPA_IMG=1   # LVGL draws simple images (unscaled, unrotated, opaque) with the PPA

esp32:
  framework:
    type: esp-idf
    version: 6.0.1
```

Same display, no rotation, camera at 10 fps, two 30-second windows each:

| Firmware | LVGL refresh | of which flush |
| --- | --- | --- |
| ESP-IDF 5.5.5, LVGL 9.5 (PPA off) | 28.0 ms | 4.6 ms |
| ESP-IDF 6.0.1, LVGL 9.6, `LV_USE_PPA` (fills) | 28.6 ms | 4.6 ms |
| ESP-IDF 6.0.1, LVGL 9.6, `LV_USE_PPA` + `LV_USE_PPA_IMG` | 20.8 ms | 4.2 ms |

Fills on the PPA change nothing for this page; drawing the picture with the PPA saves about 8 ms
per frame. Receive (1.7 ms) and decode (3.5 ms) did not change. The rest of the page and the home
page looked right in screenshots (made on the display with
[`lvgl_screenshot`](../components/lvgl_screenshot), see the manual). In the windows with IDF 6.0 the longest gap between two frames
was 230–275 ms (150–165 ms before); with 0 skipped frames this is probably the camera or Wi-Fi, but
it has not been looked into yet.

**Ceiling, measured** with a synthetic source on the last firmware (ESP-IDF 6.0.1, `LV_USE_PPA_IMG`,
no rotation, no `clip_corner`), two 30-second windows each:

| Input | Shown | Skipped | JPEG size | Decode | Hand-off | LVGL refresh (of which flush) |
| --- | --- | --- | --- | --- | --- | --- |
| Synthetic, 30 fps | 29.9 fps | 0.1 % | 8.6 KB | 4.7 ms | 2.0 ms | 22.2 ms (4.8 ms) |
| Synthetic, 60 fps | 38.8 fps | 35 % | 8.6 KB | 5.5 ms | 8.4 ms | 23.2 ms (4.6 ms) |

So the display now shows up to about **39 fps**, against 21.5 fps with `rotation: 180` on IDF 5.5
(same page, *Results* above). That is less than 1000 / 23 ms: between two refreshes the main loop
also runs the other components (touch, API, clock), and a frame waits for that before it is put on
the canvas (hand-off 8 ms at 60 fps). No errors, and the 60 fps input (0.5 MB/s) arrived completely.

This test source came from go2rtc's built-in ffmpeg test picture, set through its API without a
restart (see *How to measure*). It encodes with ffmpeg's default MJPEG quality, so the JPEGs are
8.6 KB instead of about 17 KB in the earlier synthetic tests (`-q:v 8`). The LVGL refresh does not
depend on the JPEG size; decoding is slightly cheaper with smaller files.

The estimated ~35 fps without rotation on IDF 5.5 (*Without the 180° rotation*) was not measured.

With LVGL 9.6 the compiler warns that `lv_obj_add_flag()`, `lv_obj_remove_flag()` and
`lv_obj_has_flag()` are deprecated (`lv_obj_set_hidden()` and so on). Since 19 October 2026 the
doorbell package no longer calls them: it shows and hides the picture with ESPHome's own
`lvgl.widget.show` and `lvgl.widget.hide`, which use the right call for the LVGL version ESPHome
builds with. ESPHome dev builds every configuration with LVGL 9.6 since 12 October 2026 (also with
ESP-IDF 5.5, where the PPA stays off); ESPHome 2026.10 still uses LVGL 9.5.

#### ESP-IDF 6.1

The author's display runs ESP-IDF 6.1.0 since 14 October 2026, with the same settings
(`LV_USE_PPA_IMG`, no rotation); the frame rate has not been measured again. Two changes in
ESP-IDF 6.1 matter for this board
([release notes](https://github.com/espressif/esp-idf/releases/tag/v6.1)):

- It builds for ESP32-P4 chip revision v3.0 by default, and older chips need a separate build.
  ESPHome takes care of this from the board: `esp32-p4-evboard` builds for revisions below v3.0
  (the tested board has v1.3), `esp32-p4_r3-evboard` for v3.0 and newer. The boot log shows the
  revision (`chip revision: vX.Y`).
- The JPEG decoder checks images more strictly against malformed input. go2rtc's MJPEG is normal
  ffmpeg output; a frame that is refused counts as an error in the statistics.

Without a `version`, ESPHome uses its recommended ESP-IDF: 5.5.6 since 17 October 2026
([esphome#20987](https://github.com/esphome/esphome/pull/20987)).

### Start-up time

The time from opening the page to the first frame on the screen. The display logs it as
`First frame ... ms after start` (`jpeg_stream`, INFO) and the doorbell package as
`First frame ... ms after opening the page`.

| go2rtc | First frame |
| --- | --- |
| On demand (default): ffmpeg starts when the display connects | 7.7 s |
| `preload: doorbell_mjpeg: video` in `go2rtc.yaml` | 0.13 s |
| Preload, and the stream already started by the presence sensor (`example/prewarm.yaml`) | at once (last frame 0.11 s old) |

Most of the 7.7 s is ffmpeg: starting, probing the RTSP stream and waiting for the next key frame of
the camera. Home Assistant's own go2rtc already had the camera stream loaded. With `preload`, go2rtc
keeps the ffmpeg conversion running all the time; on this Home Assistant host the go2rtc add-on then
used 0.5 % CPU with the page closed and 2 % with the page open (Supervisor add-on statistics).

`example/prewarm.yaml` starts the stream on the display as soon as a presence sensor turns on. On
this doorbell the person detection came only 0.2–3 s (median about 1 s) before the ring
(21 rings, 4–12 October 2026), so on its own it cannot hide 7.7 s; with `preload` it saves the last
0.13 s.

### CPU load per task

In % of one core over 30 s (the ESP32-P4 has two cores, so everything adds up to 200 %).
With `clip_corner` on.

| Task | Page closed | Camera 10 fps | Synthetic 30 fps | Synthetic 60 fps |
| --- | --- | --- | --- | --- |
| `loopTask` (ESPHome main loop, LVGL) | 13 % | 38 % | 68 % | 69 % |
| `jpeg_stream` (receive + decode) | – | 3.2 % | 11 % | 15 % |
| `tiT` (lwIP) | 0.2 % | 2.6 % | 10 % | 19 % |
| `sdio_*` (Wi-Fi via the C6), together | 0.5 % | 5.7 % | 20 % | 37 % |
| Audio (`ann_*`, `speaker_task`, `mixer`) | – | 7.0 % | 9.0 % | about 8 % |
| `mww` (wake word; paused while the audio plays) | 9.1 % | – | – | – |
| `IDLE0` / `IDLE1` | 90 % / 86 % | 86 % / 57 % | 54 % / 26 % | 28 % / 22 % |

### Main loop

`runtime_stats`, time spent in the `lvgl` component per 30 s: 2.4 s with the page closed, 15.6 s
with the camera at 10 fps (52 % of the main loop) and 28.8 s with 30 or 60 fps input (96 %).

## What this shows

1. **Receiving and decoding are not the problem.** About 7 ms per frame, in a background task. The
   display received 60 fps (1 MB/s) over Wi-Fi without errors.
2. **The LVGL refresh is the bottleneck.** Each new frame costs one refresh of the 640×480 canvas:
   about 24 ms rendering (LVGL software-blends the RGB565 picture from PSRAM into the PSRAM draw
   buffer) and about 17.5 ms flushing (PPA rotation by 180° and the copy into the MIPI-DSI frame
   buffer). This runs on the main loop, so at most about 21 fps reach the screen.
3. **`clip_corner` on the card around the picture cost about 10 ms per frame** (render 34 → 24 ms).
   The example and the author's display no longer use it.
4. **`loopTask` uses less CPU (38 %) than LVGL takes wall time (52 %).** Part of each refresh waits
   for hardware (PPA, DMA) or memory rather than computing.
5. **Decoding slows down under load** (3.5 → 6–8 ms at 60 fps input): the decoder shares the PSRAM
   bandwidth with LVGL.
6. **Latency.** The clock in the camera picture was less than 1 s behind a clock next to the display
   (1-second resolution). On the display itself a frame takes about 7 ms from first byte to decoded,
   plus the hand-off and one refresh.

## Ideas to make it faster

Not tried yet, roughly in order of expected gain.

| Idea | Why it could help |
| --- | --- |
| Draw the video rectangle outside LVGL: rotate and copy the decoded frame with the PPA straight into the display frame buffer, and let LVGL only draw the rest of the page | Removes the rest of the LVGL render per frame (about 16 ms with the PPA image path, 24 ms without); the PPA copy would replace the flush |
| ~~Use the LVGL PPA draw unit for image blits~~ | Done with ESP-IDF 6.0 and `LV_USE_PPA_IMG`: refresh 28 → 21 ms, see *ESP-IDF 6.0 and LVGL drawing with the PPA* |
| ~~Avoid the 180° rotation~~ | Done: refresh 41 → 28 ms, see *Without the 180° rotation* |
| Double draw buffers or LVGL direct mode | Lets rendering of the next area overlap with the flush of the previous one |
| Keep the JPEG decoder and the PPA apart | Not for speed but for stability: see *DMA2D: JPEG decoder and PPA* |
| Keep parents of the canvas plain | No `clip_corner`, opacity, transforms or shadows on the card: each makes LVGL take a slower blend path (`clip_corner` alone cost 10 ms) |
| Pause other image widgets while the page is open | Another project measured `online_image` blocking the main loop 300–650 ms per picture (software decoder); album covers or camera thumbnails elsewhere on the display stall the doorbell picture meanwhile |
| Doorbell audio on the display's own speaker from the stream, not as a Home Assistant announcement | The announcement route (go2rtc AAC → Home Assistant → FLAC → media player) is 1–2 s behind the picture; an RTSP client with G.711 sound on the same connection ([esp_rtsp_stream](https://github.com/chickymonkey/Esp-rtsp-stream)) avoids two hops |
| Echo cancellation for talking back | Lets you listen and talk at the same time instead of muting the doorbell audio (ESP-SR AFE, as the [VoIP stack](https://github.com/n-IA-hane/esphome-intercom) uses) |

## Other projects

Projects that solve parts of the same problem, read from their source or README on
10 November 2026 (none measured on this board). What this repo takes from them is in the last
column; the rest is for comparison.

| Project | What it does | Difference with this repo | Taken from it |
| --- | --- | --- | --- |
| [Olivex24/esphome-p4-mjpeg-video](https://github.com/Olivex24/esphome-p4-mjpeg-video) | MJPEG over HTTP, hardware JPEG decoder (13 ms at 1280×720), PPA scales and rotates (~30 ms) straight into the MIPI-DSI frame buffer, LVGL never touches the video; 16–27 fps at 1280×720 (ESPHome 2026.10, ESP-IDF 5.5.6, LVGL 9.5) | Full-size video outside LVGL; the rest of the page is hidden while it plays. Through LVGL a full 1280×720 frame took ~150 ms there (~6 fps) | Confirms the first idea in *Ideas to make it faster*. Its notes: `online_image` widgets elsewhere blocked the main loop 300–650 ms per picture, so pause them while video plays; with one frame buffer fast motion can tear. Its widths must be a multiple of 32; `jpeg_stream` handles the decoder's row alignment itself (stride rounded to the JPEG block size) |
| [chickymonkey/Esp-rtsp-stream](https://github.com/chickymonkey/Esp-rtsp-stream) | RTSP client on the display: MJPEG over RTP/TCP (no H.264), hardware decoder + PPA on the P4; G.711 or L16 sound on the same connection to an ESPHome `speaker` | Reads the camera (or an RTSP restream) itself instead of go2rtc's HTTP MJPEG; sound without Home Assistant's media player | An idea for lower audio delay (see *Ideas*). A Reolink sub-stream is H.264, so it would still need go2rtc to make MJPEG |
| [p1ngb4ck/esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video) and the fork's `dev` branch | Video for the P4: MIPI-CSI cameras, H.264/JPEG encoders, RTSP server, an LVGL `video_output` (hardware JPEG straight into one or two canvas buffers, or full screen), `avi` player, `stream_receiver` (a desktop or browser rendered by Home Assistant, pushed with sound and touch through the SVP Relay integration), `screen_capture` (to SD or the relay), `sip_phone` | Built on the fork's own ESPHome `dev` (core components changed: audio, camera, display, LVGL, storage); not usable as external components on stock ESPHome. No pull requests to ESPHome open (one, a per-request `http_request` timeout, merged in October). Replaces `simple_video_player`, which this page compared in October (branch `testing_dev`, gone since 1 November 2026) | Two October findings are outdated: the video output now decodes in LVGL's colour depth (RGB565 on these displays, was RGB888), and the 1024×600 check ("not a multiple of 16") is no longer in the source. Still true: sound arrives in sync with the picture, but Home Assistant runs ffmpeg per display and connects to the display (firewall the other way round) |
| [dcgrove/esphome-lvgl-screenshot](https://github.com/dcgrove/esphome-lvgl-screenshot) | LVGL screenshot as JPEG on its own HTTP port (8080), software encoder (stb), ~1.8 MB PSRAM for 800×480 | `lvgl_screenshot` here: a camera entity over the encrypted API, hardware encoder, buffers only after the first request | Nothing; a software alternative for chips without the JPEG encoder |
| [n-IA-hane/esphome-intercom](https://github.com/n-IA-hane/esphome-intercom) (VoIP stack) | SIP/RTP phone system for Home Assistant and ESPHome: registrar, softphone card, ESP32-P4 video calls, echo cancellation (ESP-SR AFE) shared with the wake word | For SIP door stations and displays calling each other; a Reolink speaks no SIP | Echo cancellation is the way to real two-way talk (see *Ideas*); `talkback.yaml` mutes the doorbell audio instead |
| [AmexHusky/reolink-intercom-Home-Assistant](https://github.com/AmexHusky/reolink-intercom-Home-Assistant), [go2rtc#1860](https://github.com/AlexxIT/go2rtc/issues/1860), [go2rtc#331](https://github.com/AlexxIT/go2rtc/issues/331) | Two-way audio with Reolink doorbells through go2rtc in the browser | Browser instead of a display | Used in `talkback.yaml`: a Reolink backchannel takes only G.711 (PCMU or PCMA; firmware v3.0.0.4662 often PCMU; go2rtc repacks PCMA for Reolink doorbells), 1–2 s delay is normal, and an open backchannel keeps the doorbell in two-way mode (blue ring, no chime), so talking gets a stream of its own |
| [esphome#16902](https://github.com/esphome/esphome/pull/16902) | `esp32_jpeg`: a shared helper for the P4's hardware JPEG encoder and decoder, with ESP-IDF JPEG/DMA2D patches | Would have replaced the own wrappers in `jpeg_stream` and `lvgl_screenshot` | Closed without merging on 23 September 2026; the components keep their own wrappers |

## DMA2D: JPEG decoder and PPA

On the ESP32-P4 the hardware JPEG decoder and the PPA both move their data with the 2D-DMA
controller (DMA2D). Two reports describe DMA2D getting stuck under load:

| Report | What | Status on 10 November 2026 |
| --- | --- | --- |
| [esp-idf#18999](https://github.com/espressif/esp-idf/issues/18999) | Interrupt watchdog: `dma2d_connect()` waits forever for a channel reset, under continuous PPA load (ESP32-P4 rev 1.3; IDF 6.0, 6.1-rc1, master) | Closed as a duplicate of #19108, without a fix |
| [esp-idf#19108](https://github.com/espressif/esp-idf/issues/19108) | Channel reset never completes; from then on every DMA2D transfer hangs, without a crash or log. The reporter's pipeline runs `jpeg_decoder_process()` and the PPA in separate tasks | Open, no fix linked; Espressif could not reproduce it yet (last activity 8 October 2026). The reporter's workaround: one mutex around both calls |

That is the situation of this component with `LV_USE_PPA_IMG`: the stream task decodes while LVGL
draws the canvas with the PPA in the main loop. Two ways to keep them apart, neither built yet:

1. Run the stream task on the main loop's core (core 1) with its priority, as `simple_video_player`
   did in October 2026. Cheap, but receiving then also competes with LVGL for that core.
2. Take a lock in the stream task around `jpeg_decoder_process()`, and the same lock in the main
   loop from `LV_EVENT_REFR_START` to `LV_EVENT_REFR_READY` (the events the statistics already use).
   A decode (3.5–8 ms) then never overlaps an LVGL refresh that may use the PPA.

If a display hangs without a log while the stream runs, this is the first suspect.

`lvgl_screenshot` adds a third DMA2D user: its hardware JPEG encoder shares the JPEG engine with
the decoder (the ESP-IDF driver lets one wait for the other) and moves its data with DMA2D too.
It runs only when Home Assistant asks for a picture, in the main loop, so it never overlaps an
LVGL refresh, but it can overlap a decode in the stream task. Don't leave the screenshot camera
open in Home Assistant while you test the stream for stability.

## How to measure

### 1. Statistics from the component

Set `stats_interval` on `jpeg_stream`:

```yaml
jpeg_stream:
  - id: doorbell_stream
    canvas: doorbell_video
    stats_interval: 10s
```

While the stream runs, the log shows every 10 s (INFO, tag `jpeg_stream`):

```
Stats over 10.0 s: 10.0 fps shown (100 shown, 100 received, 0 skipped, 0 errors)
  JPEG size: avg 21.7 KB, 217 KB/s
  Receive:   avg 3.4 ms, max 10.0 ms
  Decode:    avg 3.6 ms, max 6.0 ms
  Hand-off:  avg 3.1 ms, max 30.4 ms (task -> canvas in the main loop)
  Frame gap: max 155.4 ms
  LVGL refresh: 102 x, avg 51.6 ms, max 58.2 ms (layout + render + flush)
  LVGL flush:   avg 17.4 ms per refresh (flush_cb: copy to the display)
```

*Skipped* counts frames that arrived while the previous one was not on the screen yet. *Frame gap*
is the longest time between two frames on the canvas.

### 2. Main loop and CPU per task

Include [`example/measure.yaml`](../example/measure.yaml) next to the doorbell package. It turns on
FreeRTOS run time statistics and logs every 30 s:

- `runtime_stats`: time per component in the main loop;
- `cpu`: CPU load per FreeRTOS task over the last 30 s.

It also adds a switch *Measure: clip_corner on the video card* for an A/B test while the page is
open. Remove the package after measuring: the run time statistics cost some CPU and RAM.

### 3. Synthetic load

To find the ceiling, replace the camera by a test picture in `/config/go2rtc.yaml` and restart the
add-on:

```yaml
streams:
  doorbell_mjpeg: "exec:ffmpeg -hide_banner -v error -re -f lavfi -i testsrc2=size=640x480:rate=30 -c:v mjpeg -q:v 8 -f mjpeg -"
```

Change `rate=30` to test other frame rates. Put the original line back afterwards.

Without editing the file or restarting: go2rtc's API can change the source of a stream until the
next restart (`PATCH` does not write the config). go2rtc refuses `exec:` sources through the API,
but its ffmpeg test picture works:

```
curl -u <user>:<password> -X PATCH "http://<ha-ip>:1984/api/streams?name=doorbell_mjpeg&src=ffmpeg%3Avirtual%3Fvideo%3Dtestsrc2%26size%3D640x480%26rate%3D60%23video%3Dmjpeg"
```

The new source is used when the stream starts again, so close the page first (and remove a
`preload` of that stream with `curl -X DELETE ".../api/preload?src=doorbell_mjpeg"`; add it back
afterwards with `curl -X PUT ".../api/preload?src=doorbell_mjpeg&video"`). Put the original source
back the same way, with `src=ffmpeg%3Adoorbell%23video%3Dmjpeg`.

### 4. Latency

Show a clock with milliseconds (for example a phone stopwatch) to the camera and film the display
and the stopwatch together with a second phone in slow motion. The difference between the two
clocks in one video frame is the glass-to-glass latency.

### Measurement side effects

- When `runtime_stats` prints its report (about 60 log lines), the main loop stalls for 0.4–0.55 s.
  In that 10-second window the component reports a hand-off max above 300 ms and a few skipped
  frames. Leave those windows out.
- The CPU lambda in `measure.yaml` takes about 46 ms of main loop time per run.

## Sharing results

Open an issue with the statistics block, the CPU block, your board, your display settings
(`rotation`, `buffer_size`) and the ESPHome version.
