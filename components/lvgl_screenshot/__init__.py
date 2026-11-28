"""Screenshots of the active LVGL screen as a camera entity.

The screen is rendered with LVGL's snapshot API in the main loop and encoded to JPEG by a
`camera_encoder` in a task of its own. A screenshot is only taken when a client asks for one.
"""

import esphome.codegen as cg
from esphome.components.camera_encoder import Encoder
from esphome.components.lvgl import defines as lvgl_defines
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core.entity_helpers import setup_entity
from esphome.types import ConfigType

CODEOWNERS = ["@martijnbuts"]
DOMAIN = "lvgl_screenshot"
DEPENDENCIES = ["esp32", "lvgl"]
AUTO_LOAD = ["camera", "camera_encoder"]

CONF_ENCODER = "encoder"
CONF_STREAM_INTERVAL = "stream_interval"

camera_ns = cg.esphome_ns.namespace("camera")
Camera = camera_ns.class_("Camera", cg.EntityBase, cg.Component)

lvgl_screenshot_ns = cg.esphome_ns.namespace("lvgl_screenshot")
LvglScreenshot = lvgl_screenshot_ns.class_("LvglScreenshot", Camera)

CONFIG_SCHEMA = cv.ENTITY_BASE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(LvglScreenshot),
        # Without an id this resolves to the only camera_encoder, which AUTO_LOAD provides
        # when the configuration has none.
        cv.GenerateID(CONF_ENCODER): cv.use_id(Encoder),
        cv.Optional(
            CONF_STREAM_INTERVAL, default="1s"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config: ConfigType) -> ConfigType:
    # Turns on LV_USE_SNAPSHOT in the lv_conf.h that lvgl writes.
    lvgl_defines.add_lv_use("snapshot")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_CAMERA")
    cg.add_define("USE_LVGL_SCREENSHOT")
    encoder = await cg.get_variable(config[CONF_ENCODER])
    var = cg.new_Pvariable(config[CONF_ID], encoder, config[CONF_STREAM_INTERVAL])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)
