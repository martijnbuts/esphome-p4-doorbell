# camera_encoder (with the ESP32-P4 hardware encoder)

A copy of ESPHome's own [`camera_encoder`](https://esphome.io/components/camera/camera_encoder/)
with one addition: `type: hardware`, which encodes JPEG with the JPEG codec of the ESP32-P4 instead
of the esp32-camera software encoder. It is the code proposed for ESPHome itself (branch
[`camera-encoder-hardware`](https://github.com/martijnbuts/esphome_new/tree/camera-encoder-hardware)
of martijnbuts/esphome_new). Once ESPHome has it, remove `camera_encoder` from
`external_components` and ESPHome's own copy is used again.

Listing it under `external_components` replaces ESPHome's copy for the whole configuration. The
default type `esp32_camera` works as before.

```yaml
external_components:
  - source: github://martijnbuts/esphome-p4-doorbell@main
    components: [lvgl_screenshot, camera_encoder]

camera_encoder:
  type: hardware
  quality: 85               # 1–100, default 80
  subsampling: "4:4:4"      # 4:4:4, 4:2:2 or 4:2:0 (default); 4:4:4 keeps coloured text sharp
```

| Option | Default | What it does |
| --- | --- | --- |
| `quality` | `80` | JPEG quality, 1–100 |
| `subsampling` | `4:2:0` | How much colour detail is kept: `4:4:4` (all), `4:2:2` or `4:2:0` (smallest images). Grayscale input is always encoded as grayscale |
| `buffer_size` | `65536` | Initial size of the output buffer in bytes |
| `buffer_expand_size` | `32768` | Bytes added to the output buffer when an image does not fit (`0`: no growth) |

Only accepted on the ESP32-P4. The encoder copies each frame into a buffer of its own that the
codec can read with DMA, which takes as much memory as one raw frame. When an image does not fit
the output buffer, ESP-IDF logs an error line, and the encoder grows the buffer and encodes the
frame again.

Tested on 27 November 2026 on a Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (ESP-IDF 6.1.1) with
[`lvgl_screenshot`](../lvgl_screenshot): 1024×600 screenshots of about 109 kB at quality 85 with
`4:4:4`, also while `jpeg_stream` decoded a doorbell stream with the same codec (254 frames in
30 s, 0 errors).
