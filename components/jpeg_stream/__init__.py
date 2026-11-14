"""Show JPEG frames from an HTTP(S) URL on an LVGL canvas.

The URL can point to
- an MJPEG stream (multipart/x-mixed-replace, e.g. go2rtc /api/stream.mjpeg): one connection,
  every part is a frame. Each part needs a Content-Length header (go2rtc sends one).
- a single JPEG image (e.g. Home Assistant's camera proxy): fetched again and again over one
  kept-alive connection.
The kind is taken from the Content-Type of the response. Credentials in the URL
(http://user:password@host/...) are sent as HTTP Basic authentication.

A background task receives the images and decodes them, so the main loop is never blocked by
networking or decoding. Decoder: the ESP32-P4 hardware JPEG decoder (default on the P4), or the
JPEGDEC software decoder that ESPHome's runtime_image also uses (default on other ESP32s, and
`decoder: software` on the P4). On the ESP32-S3 JPEGDEC uses the chip's SIMD instructions.
Each frame is shown as soon as it is decoded; a frame that arrives while the previous one is not
shown yet is skipped (lowest latency).

With stats_interval set, it logs where the time goes (receive, decode, hand-off to the main loop,
LVGL refresh) every interval while the stream runs; see docs/PERFORMANCE.md.

The hardware JPEG decoding follows the approach of the transcoder and picture_viewer components
in https://github.com/p1ngb4ck/esphome (engine setup, buffer allocation, recreate after an error).
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32.const import VARIANT_ESP32P4
from esphome.components.lvgl.widgets.canvas import lv_canvas_t
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_ERROR, CONF_TIMEOUT, CONF_URL
from esphome.types import ConfigType

CODEOWNERS = ["@martijnbuts"]
DEPENDENCIES = ["esp32", "lvgl", "psram"]
DOMAIN = "jpeg_stream"
MULTI_CONF = True

CONF_CANVAS = "canvas"
CONF_DECODER = "decoder"
CONF_MAX_HEIGHT = "max_height"
CONF_MAX_JPEG_SIZE = "max_jpeg_size"
CONF_MAX_WIDTH = "max_width"
CONF_MIN_INTERVAL = "min_interval"
CONF_ON_FRAME = "on_frame"
CONF_STATS_INTERVAL = "stats_interval"

DECODER_HARDWARE = "hardware"
DECODER_SOFTWARE = "software"

jpeg_stream_ns = cg.esphome_ns.namespace("jpeg_stream")
JpegStream = jpeg_stream_ns.class_("JpegStream", cg.Component)


def _validate_decoder(config: ConfigType) -> ConfigType:
    is_p4 = esp32.get_esp32_variant() == VARIANT_ESP32P4
    if CONF_DECODER not in config:
        config[CONF_DECODER] = DECODER_HARDWARE if is_p4 else DECODER_SOFTWARE
    elif config[CONF_DECODER] == DECODER_HARDWARE and not is_p4:
        raise cv.Invalid(
            "The hardware JPEG decoder only exists on the ESP32-P4; use 'software'",
            path=[CONF_DECODER],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(JpegStream),
            cv.Required(CONF_CANVAS): cv.use_id(lv_canvas_t),
            cv.Optional(CONF_URL): cv.url,
            # Default: hardware on the ESP32-P4, software elsewhere
            cv.Optional(CONF_DECODER): cv.one_of(
                DECODER_HARDWARE, DECODER_SOFTWARE, lower=True
            ),
            cv.Optional(CONF_MAX_WIDTH, default=640): cv.int_range(min=16, max=1920),
            cv.Optional(CONF_MAX_HEIGHT, default=480): cv.int_range(min=16, max=1920),
            cv.Optional(CONF_MAX_JPEG_SIZE, default=262144): cv.int_range(
                min=4096, max=4194304
            ),
            cv.Optional(
                CONF_TIMEOUT, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_MIN_INTERVAL, default="0ms"
            ): cv.positive_time_period_milliseconds,
            # log receive/decode/hand-off/LVGL refresh times every interval while running (0s = off)
            cv.Optional(
                CONF_STATS_INTERVAL, default="0s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_FRAME): automation.validate_automation({}),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation({}),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_decoder,
)

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_FRAME, "add_on_frame_callback"),
    automation.CallbackAutomation(CONF_ON_ERROR, "add_on_error_callback"),
)

ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(JpegStream),
    }
)
URL_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(JpegStream),
        cv.Optional(CONF_URL): cv.templatable(cv.string),
    }
)

# start: optionally set the URL first, then start fetching frames
automation.register_apply_action(
    "jpeg_stream.start",
    URL_ACTION_SCHEMA,
    automation.ApplyField(CONF_URL, "set_url", cg.std_string),
    automation.ApplyCall("start()"),
)
automation.register_apply_action(
    "jpeg_stream.stop", ACTION_SCHEMA, automation.ApplyCall("stop()")
)
automation.register_apply_action(
    "jpeg_stream.set_url",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(JpegStream),
            cv.Required(CONF_URL): cv.templatable(cv.string),
        }
    ),
    automation.ApplyField(CONF_URL, "set_url", cg.std_string),
)
automation.register_apply_condition(
    "jpeg_stream.is_running", ACTION_SCHEMA, "is_running()"
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_MAX_WIDTH],
        config[CONF_MAX_HEIGHT],
        config[CONF_MAX_JPEG_SIZE],
    )
    await cg.register_component(var, config)
    canvas = await cg.get_variable(config[CONF_CANVAS])
    cg.add(var.set_canvas(canvas))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))
    cg.add(var.set_min_interval(config[CONF_MIN_INTERVAL]))
    cg.add(var.set_stats_interval(config[CONF_STATS_INTERVAL]))
    if (url := config.get(CONF_URL)) is not None:
        cg.add(var.set_url(url))
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)

    # esp_http_client (plus esp-tls) and the root certificate bundle for https URLs
    esp32.request_http_client()
    esp32.require_certificate_bundle()

    if config[CONF_DECODER] == DECODER_HARDWARE:
        cg.add_define("USE_JPEG_STREAM_HW_DECODER")
        cg.add(var.set_hardware_decoder(True))
        esp32.include_builtin_idf_component("esp_driver_jpeg")
    else:
        cg.add_define("USE_JPEG_STREAM_SW_DECODER")
        # The same JPEGDEC build as runtime_image (online_image), so both can be used together.
        # It uses the ESP32-S3 SIMD colour conversion, which needs the esp-dsp headers.
        cg.add_library(
            "JPEGDEC",
            "1.8.4",
            "https://github.com/esphome-libs/JPEGDEC#1.8.4-esphome.4",
        )
        esp32.add_idf_component(name="espressif/esp-dsp", ref="1.8.2")
