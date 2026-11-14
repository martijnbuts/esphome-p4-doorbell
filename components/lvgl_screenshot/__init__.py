"""LVGL screenshot as a Home Assistant camera entity.

Renders the active LVGL screen with lv_snapshot (LVGL 9), encodes it to JPEG and publishes it
through ESPHome's generic camera interface, so Home Assistant shows the display as a normal
(API-encrypted) camera. A screenshot is only made when Home Assistant asks for an image.

Two JPEG encoders:
- ESP32-P4: the hardware JPEG encoder (esp_driver_jpeg, the same engine jpeg_stream uses for
  decoding). A few milliseconds per screenshot, in the main loop.
- Every other ESP32 (for example the S3), or any chip when `encoder:` is set: a
  `camera_encoder` (ESPHome's software JPEG from esp32-camera). It runs in its own FreeRTOS
  task, so a slow encode does not hold up the main loop. Without `encoder:` a default
  `camera_encoder` is loaded automatically; add a `camera_encoder:` block to set its quality.

Until 23-10-2026 the JPEG was made in software with Espressif's esp_new_jpeg encoder.
Software fallback via camera_encoder added on 14-11-2026.
"""

import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32.const import VARIANT_ESP32P4
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.core.entity_helpers import setup_entity
from esphome.types import ConfigType

DEPENDENCIES = ["esp32", "lvgl"]

CONF_ENCODER = "encoder"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_STREAM_INTERVAL = "stream_interval"
CONF_TASK_PRIORITY = "task_priority"

DEFAULT_JPEG_QUALITY = 85

camera_ns = cg.esphome_ns.namespace("camera")
Encoder = camera_ns.class_("Encoder")

lvgl_screenshot_ns = cg.esphome_ns.namespace("lvgl_screenshot")
LvglScreenshot = lvgl_screenshot_ns.class_("LvglScreenshot", cg.Component, cg.EntityBase)


def _is_p4() -> bool:
    return esp32.get_esp32_variant() == VARIANT_ESP32P4


def _use_hardware(config: ConfigType) -> bool:
    return CONF_ENCODER not in config and _is_p4()


def AUTO_LOAD(config: ConfigType) -> list[str]:
    # Runs after schema validation, so the chip variant is known. Without hardware JPEG and
    # without an explicit encoder, load a default camera_encoder (software JPEG).
    if _use_hardware(config) or CONF_ENCODER in config:
        return ["camera"]
    return ["camera", "camera_encoder"]


def _validate(config: ConfigType) -> ConfigType:
    if not _use_hardware(config) and CONF_JPEG_QUALITY in config:
        raise cv.Invalid(
            f"'{CONF_JPEG_QUALITY}' only applies to the ESP32-P4 hardware encoder. "
            "For the software encoder set 'quality' under 'camera_encoder:'.",
            path=[CONF_JPEG_QUALITY],
        )
    if _use_hardware(config) and CONF_TASK_PRIORITY in config:
        raise cv.Invalid(
            f"'{CONF_TASK_PRIORITY}' only applies to the software encoder.",
            path=[CONF_TASK_PRIORITY],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(LvglScreenshot),
            # Hardware encoder only (ESP32-P4); the software encoder has its own quality.
            cv.Optional(CONF_JPEG_QUALITY): cv.int_range(min=10, max=100),
            cv.Optional(
                CONF_STREAM_INTERVAL, default="1s"
            ): cv.positive_time_period_milliseconds,
            # A camera_encoder to use instead of the ESP32-P4 hardware encoder.
            cv.Optional(CONF_ENCODER): cv.use_id(Encoder),
            # FreeRTOS priority of the software encoding task. Default 1, the same as the
            # main loop: on a dual-core chip it then mostly runs on the other core.
            cv.Optional(CONF_TASK_PRIORITY): cv.int_range(min=0, max=24),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)
    cg.add(var.set_stream_interval(config[CONF_STREAM_INTERVAL]))
    # lv_snapshot is off in ESPHome's generated lv_conf.h; enable it for the LVGL build
    cg.add_build_flag("-DLV_USE_SNAPSHOT=1")

    if _use_hardware(config):
        cg.add_define("USE_LVGL_SCREENSHOT_HW_JPEG")
        cg.add(var.set_jpeg_quality(config.get(CONF_JPEG_QUALITY, DEFAULT_JPEG_QUALITY)))
        esp32.include_builtin_idf_component("esp_driver_jpeg")
        return

    cg.add_define("USE_LVGL_SCREENSHOT_SW_JPEG")
    if CONF_ENCODER in config:
        encoder = await cg.get_variable(config[CONF_ENCODER])
    else:
        # The camera_encoder loaded by AUTO_LOAD (or one the user added without linking it)
        encoder = await cg.get_variable(CORE.config["camera_encoder"][CONF_ID])
    cg.add(var.set_encoder(encoder))
    if CONF_TASK_PRIORITY in config:
        cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
