# Component tests

The files in `components/<name>/` follow the layout of ESPHome's own
[`tests/components`](https://github.com/esphome/esphome/tree/dev/tests/components), so they can
move into an ESPHome pull request unchanged:

- `common.yaml` – configuration shared by all targets
- `test.<target>.yaml` – one per build target (`esp32-s3-idf`, `esp32-p4-idf`), with the board
  specific parts (PSRAM, display) and `<<: !include common.yaml`
- `test-software.esp32-p4-idf.yaml` – the P4 with the software encoder/decoder instead of the
  hardware one

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
packages:
  under_test: !include tests/components/jpeg_stream/test.esp32-s3-idf.yaml
external_components:
  - source:
      type: local
      path: components
    components: [jpeg_stream]
```

Then `esphome config <file>` validates it and `esphome compile <file>` builds it.

Checked on 14 November 2026 with ESPHome 2026.12.0-dev: all six files validate and generate the
expected code (hardware or software path).
