"""LVGL screenshot as a Home Assistant camera entity.

Renders the active LVGL screen with lv_snapshot (LVGL 9) straight into a DMA buffer,
encodes it to JPEG with the ESP32-P4's hardware JPEG encoder (esp_driver_jpeg, the same
engine jpeg_stream uses for decoding) and publishes it through ESPHome's generic camera
interface, so Home Assistant shows the display as a normal (API-encrypted) camera.
A screenshot is only made when Home Assistant asks for an image.

Until 23-10-2026 the JPEG was made in software with Espressif's esp_new_jpeg encoder.
"""

import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32.const import VARIANT_ESP32P4
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core.entity_helpers import setup_entity

AUTO_LOAD = ["camera"]
DEPENDENCIES = ["esp32", "lvgl", "psram"]

CONF_JPEG_QUALITY = "jpeg_quality"
CONF_STREAM_INTERVAL = "stream_interval"

lvgl_screenshot_ns = cg.esphome_ns.namespace("lvgl_screenshot")
LvglScreenshot = lvgl_screenshot_ns.class_("LvglScreenshot", cg.Component, cg.EntityBase)

CONFIG_SCHEMA = cv.All(
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(LvglScreenshot),
            cv.Optional(CONF_JPEG_QUALITY, default=85): cv.int_range(min=10, max=100),
            cv.Optional(
                CONF_STREAM_INTERVAL, default="1s"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # The hardware JPEG encoder only exists on the ESP32-P4
    esp32.only_on_variant(supported=[VARIANT_ESP32P4]),
)


async def to_code(config):
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    cg.add(var.set_stream_interval(config[CONF_STREAM_INTERVAL]))
    # lv_snapshot is off in ESPHome's generated lv_conf.h; enable it for the LVGL build
    cg.add_build_flag("-DLV_USE_SNAPSHOT=1")
    esp32.include_builtin_idf_component("esp_driver_jpeg")
