# Component tests

The files in `components/<name>/` follow the layout of ESPHome's own
[`tests/components`](https://github.com/esphome/esphome/tree/dev/tests/components), so they can
move into an ESPHome pull request unchanged:

- `common.yaml` – configuration shared by all targets
- `test.<target>.yaml` – one per build target (`esp32-s3-idf`, `esp32-p4-idf`), with the board
  specific parts (PSRAM, display) and `<<: !include common.yaml`
- `test-software.esp32-p4-idf.yaml` (`jpeg_stream`) – the P4 with the software decoder instead of
  the hardware one
- `test-hardware.esp32-p4-idf.yaml` (`lvgl_screenshot`, `camera_encoder`) – the P4 with the
  `hardware` type of `camera_encoder` instead of the default software encoder

They have no `esphome:` or `esp32:` block: ESPHome's test runner adds those from
`tests/test_build_components/build_components_base.<target>.yaml`.

## Running them in this repository

Put a test file in a package next to a base configuration and point `external_components` at
this repository:

```yaml
esphome:
  name: test-jpeg-stream
esp32:
  variant: esp32s3          # or esp32p4, matching the test file
  framework:
    type: esp-idf
logger:
packages:
  under_test: !include tests/components/jpeg_stream/test.esp32-s3-idf.yaml
external_components:
  - source:
      type: local
      path: components
    components: [jpeg_stream]   # add camera_encoder for the test-hardware files
```

Then `esphome config <file>` validates it and `esphome compile <file>` builds it.

Checked on 28 November 2026 with ESPHome 2026.12.0-dev: all seven files validate.
