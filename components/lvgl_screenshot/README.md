# lvgl_screenshot

An ESPHome component that shows what an LVGL display shows as a camera entity in Home Assistant.
A screenshot is only made when Home Assistant asks for one, so a display nobody looks at costs
no CPU time.

It is handy to check a wall display from a distance, to see what a remote device is showing
when someone reports a problem, and for documentation (the picture is the LVGL screen itself,
pixel for pixel).

Status: an external component, used daily on two displays (see *Tested on*). The same code is
proposed for ESPHome itself (branch
[`lvgl-screenshot`](https://github.com/martijnbuts/esphome_new/tree/lvgl-screenshot) of
martijnbuts/esphome_new); see *ESPHome* at the end.

## Configuration

The same configuration works on every ESP32 with LVGL:

```yaml
external_components:
  - source: github://martijnbuts/esphome-p4-doorbell@main
    components: [lvgl_screenshot]

lvgl_screenshot:
  name: "Screen"            # becomes camera.<device>_screen in Home Assistant
```

The JPEG is made by ESPHome's [`camera_encoder`](https://esphome.io/components/camera/camera_encoder/).
Without a `camera_encoder:` block one with default settings (software, quality 80) is added. Add the
block to change its settings; it is picked up without further configuration:

```yaml
camera_encoder:
  quality: 85               # 1–100, default 80

lvgl_screenshot:
  name: "Screen"
```

On an ESP32-P4 the chip's JPEG codec can do the encoding. That needs the `hardware` type, which is
in the [`camera_encoder`](../camera_encoder) copy in this repository until ESPHome has it:

```yaml
external_components:
  - source: github://martijnbuts/esphome-p4-doorbell@main
    components: [lvgl_screenshot, camera_encoder]

camera_encoder:
  type: hardware
  quality: 85
  subsampling: "4:4:4"      # keeps small coloured text sharp

lvgl_screenshot:
  name: "Screen"
```

| Option | Default | What it does |
| --- | --- | --- |
| `name`, `id`, `icon`, `internal`, … | | The usual entity options |
| `stream_interval` | `1s` | Time between pictures while the live view is open in Home Assistant |
| `encoder` | | ID of the `camera_encoder` to use; only needed when there is more than one |

Coming from the version before 28 November 2026: `jpeg_quality` and `task_priority` are gone. On
the P4, replace `jpeg_quality: 85` with the `camera_encoder` block above (`type: hardware`,
`quality: 85`); elsewhere nothing changes.

## How it works

1. Home Assistant asks the camera for a picture over the native API.
2. In the main loop, the component renders the active LVGL screen once more with
   `lv_snapshot_take_to_draw_buf()` into an RGB565 buffer (LVGL is not thread-safe, so this
   step has to run there).
3. A FreeRTOS task of the component hands the buffer to the `camera_encoder`, so the main loop
   and the display keep running while it encodes. LVGL renders RGB565 low byte first and camera
   encoders expect the high byte first (as a camera sends it), so the task swaps the bytes first.
   When the encoder asks for a retry (after it grew its output buffer), the task tries again; the
   encoder stops asking once its buffer cannot grow.
4. The JPEG is published through `camera::Camera` like any ESPHome camera, so it travels over
   the encrypted API connection.

Memory, all reserved in `setup()` and then kept:

- the screen buffer: width × height × 2 bytes, allocated by LVGL (aligned for its 2D accelerator,
  the PPA on the ESP32-P4); in practice in PSRAM;
- room in the encoder's output buffer for 2 bits per pixel (width × height / 4 bytes), unless its
  `buffer_size` is larger already. Without this, the default encoder's buffer would start at 4 kB
  and grow by 1 kB per retry, and every retry encodes the whole screen again;
- a 6 kB task stack in internal RAM.

The published image points straight into the encoder's output buffer: no copy per screenshot.
That is safe because the next screenshot is only made once every API connection has released the
previous one.

## Measured

ESPHome 2026.12.0-dev, ESP-IDF 6.1.1, the dashboards shown on these displays:

| | ESP32-P4 1024×600, software encoder (default) | ESP32-P4 1024×600, hardware encoder (`type: hardware`) | ESP32-S3 800×480, software encoder |
| --- | --- | --- | --- |
| Render (`lv_snapshot`, main loop) | 48–49 ms | 48–52 ms | 127–128 ms |
| Encode (own task, including the byte swap) | 341–348 ms | 85–103 ms | 407–419 ms |
| JPEG size | 82 kB (quality 80) | 109 kB (quality 85, 4:4:4) | 67 kB (quality 85) |

Measured on 27 and 28 November 2026. The times are in the log at VERBOSE:
`Screenshot 1024x600, 82072 bytes (render 48 ms, encode 348 ms)`.

The render step is the weak point. ESPHome warns when a component holds up the main loop for more
than 50 ms; on the P4 that happened only for the first screenshot after boot (71 ms), on the S3 the
render takes 127 ms. Free internal RAM on the S3 stayed at 163 kB while screenshots were made.

The previous version (until 27 November 2026) encoded on the P4 in the main loop and held it up
for 177 ms per screenshot.

## Tests

[`tests/components/lvgl_screenshot`](../../tests/components/lvgl_screenshot) has test
configurations in the layout of ESPHome's own tests: ESP32-S3 and ESP32-P4 with the default
encoder, and the ESP32-P4 with the hardware encoder; see [`tests/README.md`](../../tests/README.md).

## Tested on

- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (1024×600, MIPI DSI), ESP-IDF 6.1.1, LVGL 9.6,
  ESPHome 2026.12.0-dev: both encoders (27 and 28 November 2026), also while `jpeg_stream`
  decodes a doorbell stream with the same JPEG codec
- ESP32-S3 (N16R8) with an 800×480 RGB panel, ESP-IDF 6.1.1, LVGL 9.6, ESPHome 2026.12.0-dev: software
  encoder (28 November 2026)

## Limitations

- **Only the active screen.** Widgets on LVGL's top and system layers (for example a message
  box on `lv_layer_top`) are not in the picture.
- **Rendering runs in the main loop**, so a screenshot holds it up for about 50 ms on the P4
  (see *Measured*). Keep `stream_interval` at 1 s or more.
- **One camera per device:** ESPHome supports one camera, so this cannot be combined with
  `esp32_camera` or `mipi_csi`.
- **ESP-IDF only** (esp32-camera's encoder and the P4 codec both need it).
- **Privacy:** everyone who can see the camera entity in Home Assistant sees the screen.

## ESPHome

The component is split for ESPHome into two pull requests that do not depend on each other:

1. **camera_encoder:** `type: hardware` for the ESP32-P4 JPEG codec (branch
   [`camera-encoder-hardware`](https://github.com/martijnbuts/esphome_new/tree/camera-encoder-hardware)).
   Other cameras on the P4 could use it as well.
2. **lvgl_screenshot:** this component, which works with any `camera_encoder` (branch
   [`lvgl-screenshot`](https://github.com/martijnbuts/esphome_new/tree/lvgl-screenshot)). It turns
   on `LV_USE_SNAPSHOT` with `lvgl`'s existing `add_lv_use()`, so `lvgl` itself needs no change.

Related work: [n-IA-hane/esphome-screenshot](https://github.com/n-IA-hane/esphome-screenshot)
captures LVGL during a normal refresh (or the P4 MIPI DSI framebuffer), encodes in software and
serves the JPEG over HTTP. This component instead renders on request and uses ESPHome's camera
entity, so the picture goes over the native API without the web server.

In ESPHome itself:

- [esphome#19421](https://github.com/esphome/esphome/pull/19421) (merged, 2026.10) added the
  `snapshot` component: `snapshot.take` writes what a display shows to a BMP or animated GIF
  file. It only runs on the `host` platform.
- [esphome#22789](https://github.com/esphome/esphome/pull/22789) (open) lets other components
  read back the finished picture of a `mipi_dsi` panel, including video drawn outside LVGL. On
  the P4 that would replace the second render with a copy of the frame buffer, and avoid the
  render time in the main loop.

## License

MIT, like the rest of this repository.
