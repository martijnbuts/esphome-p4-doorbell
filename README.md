# esphome-p4-doorbell

Live Reolink doorbell video and audio on an ESP32-P4 LVGL display with ESPHome.

**Manual:** [docs/MANUAL.md](docs/MANUAL.md) · [PDF](docs/Manual-P4-doorbell.pdf)

When someone rings, the display opens a page with the live picture of the doorbell at
10 frames per second with under 1 s delay, and plays the doorbell audio on its speaker.
A video button shows the picture without a ring.

![Setup](docs/setup.png)

## What is in here

| Path | What it is |
| --- | --- |
| [`components/jpeg_stream`](components/jpeg_stream) | ESPHome external component: receives an MJPEG stream (or repeated JPEG snapshots) in a background task and decodes every frame with the ESP32-P4 hardware JPEG decoder onto an LVGL canvas |
| [`example/doorbell.yaml`](example/doorbell.yaml) | Package with the doorbell page, the scripts, the audio and the fallback to Home Assistant snapshots |
| [`example/waveshare-p4-7b.yaml`](example/waveshare-p4-7b.yaml) | Complete device configuration for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B |
| [`example/go2rtc.yaml`](example/go2rtc.yaml) | Configuration for the go2rtc add-on (MJPEG + AAC from the camera's RTSP stream) |
| [`docs/`](docs) | The manual as Markdown and PDF, and the setup diagram |

## Quick start

```yaml
external_components:
  - source: github://martijnbuts/esphome-p4-doorbell@main
    components: [jpeg_stream]

jpeg_stream:
  - id: doorbell_stream
    canvas: doorbell_video        # an LVGL canvas widget
    timeout: 10s

# then, for example in an on_click:
#   - jpeg_stream.start:
#       id: doorbell_stream
#       url: "http://user:password@192.168.1.10:1984/api/stream.mjpeg?src=doorbell_mjpeg"
#   - jpeg_stream.stop: doorbell_stream
```

The manual explains the whole setup: go2rtc, the firewall rule, the doorbell page and the audio.

## Measured

On a Waveshare ESP32-P4-WIFI6-Touch-LCD-7B with a Reolink D340P (sub-stream 640×480):

| | Before (`online_image`) | Snapshots via HA | MJPEG via go2rtc |
| --- | --- | --- | --- |
| Frames per second | 0.4 | 3.6 | 10 (the camera's frame rate) |
| Decode time per frame | ~2.7 s in the main loop | 5–10 ms, background task | 5–10 ms, background task |

## Requirements

ESP32-P4 with PSRAM, ESPHome 2026.11.0-dev or newer (ESP-IDF 5.5, LVGL 9),
Home Assistant with the go2rtc add-on for the MJPEG stream and the audio.

## Credits

The hardware JPEG decoding follows the approach of the transcoder and picture_viewer
components in [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome).

## License

[MIT](LICENSE)
