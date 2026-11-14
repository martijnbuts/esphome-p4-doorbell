# lvgl_screenshot

An ESPHome component that shows what an LVGL display shows as a camera entity in Home Assistant.
A screenshot is only made when Home Assistant asks for one, so a display nobody looks at costs
no CPU and no memory.

It is handy to check a wall display from a distance, to see what a remote device is showing
when someone reports a problem, and for documentation (the picture is the LVGL screen itself,
pixel for pixel).

Status: an external component, used daily on two displays (see *Tested on*). It is meant as a
starting point for discussing a native ESPHome feature; see *Proposal for ESPHome* at the end.

## Configuration

The same configuration works on every ESP32 with LVGL:

```yaml
external_components:
  - source: github://martijnbuts/esphome-p4-doorbell@main
    components: [lvgl_screenshot]

lvgl_screenshot:
  name: "Screen"            # becomes camera.<device>_screen in Home Assistant
```

On an ESP32-P4 the picture is encoded by the chip's hardware JPEG encoder. On other ESP32s
(for example the S3) ESPHome's [`camera_encoder`](https://esphome.io/components/camera/camera_encoder/)
is loaded automatically and encodes it in software. Add a `camera_encoder:` block to change its
settings; it is picked up without further configuration:

```yaml
camera_encoder:
  quality: 85               # 1–100, default 80

lvgl_screenshot:
  name: "Screen"
```

| Option | Default | What it does |
| --- | --- | --- |
| `name`, `id`, `icon`, `internal`, … | | The usual entity options |
| `stream_interval` | `1s` | Time between pictures while the live view is open in Home Assistant |
| `jpeg_quality` | `85` | ESP32-P4 hardware encoder only: quality 10–100 (4:4:4, so small coloured text stays sharp). Elsewhere set `quality` under `camera_encoder:`; using `jpeg_quality` there is a validation error |
| `encoder` | | ID of a `camera_encoder` to use; also forces the software encoder on a P4. Without it, a P4 uses its hardware encoder and other chips use the only `camera_encoder` |
| `task_priority` | `1` | Software encoder only: FreeRTOS priority of the encoding task |

## How it works

1. Home Assistant asks the camera for a picture over the native API.
2. In the main loop, the component renders the active LVGL screen once more with
   `lv_snapshot_take_to_draw_buf()` into an RGB565 buffer (LVGL is not thread-safe, so this
   step has to run there).
3. The buffer is encoded to JPEG:
   - **ESP32-P4:** `esp_driver_jpeg` (hardware), directly in the main loop; it takes a few ms.
   - **Other chips:** a FreeRTOS task hands the buffer to the `camera_encoder` (esp32-camera's
     `fmt2jpg`). LVGL renders RGB565 little endian and esp32-camera expects big endian, so the
     task swaps the bytes in place first. Calling esp32-camera's global
     `jpg_set_rgb565_big_endian(false)` instead would also change real cameras.
4. The JPEG is published through `camera::Camera` like any ESPHome camera, so it travels over
   the encrypted API connection.

Memory: all buffers are created on the first request and then kept.

- the raw screen buffer: width × height × 2 bytes (PSRAM when available, otherwise internal RAM);
- the JPEG output buffer: on the P4 half the raw size, on other chips the `camera_encoder`
  buffer, which starts at a quarter of the raw size and grows when needed;
- on other chips, a 6 kB task stack in internal RAM.

The published image points straight into the output buffer: no copy per screenshot. That is
safe because the next screenshot is only made once every API connection has released the
previous one.

## Measured

| | ESP32-P4, 1024×600 (hardware) | ESP32-S3, 800×480 (software) |
| --- | --- | --- |
| Render (`lv_snapshot`, main loop) | MEASURE_P4_RENDER | MEASURE_S3_RENDER |
| JPEG encode | MEASURE_P4_JPEG (main loop) | MEASURE_S3_JPEG (own task) |
| JPEG size, dashboard | MEASURE_P4_SIZE | MEASURE_S3_SIZE |
| Encoding task stack never used | – | MEASURE_S3_STACK of 6144 bytes |

The first screenshot after boot logs these numbers at INFO, later ones at DEBUG:
`First screenshot 800x480, … bytes (render … ms, jpeg … ms)`.

## Tested on

- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (1024×600, MIPI DSI), ESP-IDF 6.1, LVGL 9.6
- ESP32-S3 (N16R8) with an 800×480 RGB panel, ESP-IDF 5.5, LVGL 9.6
- ESPHome 2026.12.0-dev (14 November 2026); compiles and validates for both chips

## Limitations

- **Only the active screen.** Widgets on LVGL's top and system layers (for example a message
  box on `lv_layer_top`) are not in the picture.
- **Rendering runs in the main loop**, so a screenshot briefly holds it up. The time grows with
  the screen size and how much is on the screen (see *Measured*).
- **ESP32-P4: the screen size must be a multiple of 8** in both directions (4:4:4 JPEG blocks).
  Otherwise no picture is made and the log says so. The software encoder has no such limit.
- **ESP32-P4: the JPEG engine is shared** with the hardware JPEG decoder. The driver serialises
  them, so a screenshot waits for a running decode (a few ms).
- **ESP-IDF only** (the P4 hardware encoder and esp32-camera both need it).
- **Privacy:** everyone who can see the camera entity in Home Assistant sees the screen.

## Proposal for ESPHome

This works as one external component, but upstream I think it fits better as three small
pieces:

1. **lvgl:** an option to enable LVGL's snapshot API (`LV_USE_SNAPSHOT`, off in the generated
   `lv_conf.h`). This component now sets it with a build flag.
2. **camera_encoder:** a new encoder type for the ESP32-P4 hardware JPEG encoder, next to the
   existing software type `esp32_camera`. Other cameras on the P4 could use it as well.
3. **An LVGL screenshot camera** that renders the active screen and hands it to any
   `camera_encoder`, always encoding outside the main loop. That removes the
   `jpeg_quality`/`quality` split between the P4 and other chips.

Open questions for the maintainers:

- Should the screenshot camera be its own component, or part of `lvgl` (for example a
  `camera` option on the `lvgl:` block)?
- Should the top and system layers be included in the picture?
- Is creating the buffers on the first request acceptable (they are large, and most displays
  are never looked at), or should they be allocated in `setup()`?

## License

MIT, like the rest of this repository.
