# Performance

Measured on 12 October 2026. On this board the network and the hardware JPEG decoder are cheap
(about 7 ms per frame together). **The limit is the LVGL refresh of the 640×480 picture: about
41 ms per frame on the main loop, so at most about 21 frames per second reach the screen.** The
camera itself delivers 10 fps, so the doorbell page shows every frame today.

This page lists what was measured, how, and where the time could still be won.

## Test setup

| Part | Value |
| --- | --- |
| Board | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B: ESP32-P4 rev 1.3 at 360 MHz, 32 MB flash, PSRAM hex mode 200 MHz |
| Wi-Fi | ESP32-C6 co-processor over SDIO (`esp32_hosted`) |
| Firmware | ESPHome 2026.11.0-dev, ESP-IDF 5.5.5, LVGL 9.5.0, `compiler_optimization: PERF` (-O2), `execute_from_psram: true` |
| Display | 1024×600 MIPI-DSI; LVGL `rotation: 180` (software rotation, PPA accelerated), `buffer_size: 100%` (one 1.2 MB draw buffer in PSRAM, partial render mode) |
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
| Draw the video rectangle outside LVGL: rotate and copy the decoded frame with the PPA straight into the display frame buffer, and let LVGL only draw the rest of the page | Removes the ~24 ms LVGL render per frame; the PPA copy would replace the flush |
| Use the LVGL PPA draw unit (`LV_USE_PPA`) for image blits | ESPHome turns `LV_USE_PPA` off ("disable use of PPA for fills until upstream bugs fixed"); enabling it only for image blits could take the render off the CPU |
| Avoid the 180° rotation | Saves the rotation step in every flush, if the panel can mirror in hardware or the board can be mounted the other way round |
| Double draw buffers or LVGL direct mode | Lets rendering of the next area overlap with the flush of the previous one |
| Keep parents of the canvas plain | No `clip_corner`, opacity, transforms or shadows on the card: each makes LVGL take a slower blend path (`clip_corner` alone cost 10 ms) |

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
