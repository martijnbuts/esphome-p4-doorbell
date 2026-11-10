"""Talk back through a camera with go2rtc: the microphone goes to the camera's speaker.

When started, the component
  1. serves the microphone (16-bit mono PCM at the microphone's sample rate) as an endless WAV
     over HTTP on its own port (path /talk.wav), only to the allowed IP addresses, and
  2. asks go2rtc to play that WAV on the stream's two-way audio channel:
     POST <url>/api/ffmpeg?dst=<stream>&live=http://<this device>:<port>/talk.wav
     go2rtc's ffmpeg fetches the WAV and converts it to the codec the camera asks for
     (go2rtc adds #audio=auto). Use a go2rtc stream of its own for this, straight to the camera:
     go2rtc opens it only while talking. An always-on stream with a backchannel makes go2rtc
     reconnect the camera when talking starts, and can keep a Reolink in two-way mode.
Stop closes the WAV stream and asks go2rtc to stop (POST <url>/api/streams?dst=<stream>&src=).

Serving and the go2rtc calls run in their own tasks: go2rtc only answers the POST after ffmpeg
has opened the WAV, which would block the main loop (and the display) otherwise.
"""

import ipaddress

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, microphone
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_ON_ERROR,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_URL,
    CONF_USERNAME,
)
from esphome.types import ConfigType

CODEOWNERS = ["@martijnbuts"]
DEPENDENCIES = ["esp32", "network", "microphone"]
AUTO_LOAD = ["ring_buffer"]
DOMAIN = "go2rtc_talkback"

CONF_ALLOWED_IPS = "allowed_ips"
CONF_MAX_DURATION = "max_duration"
CONF_ON_STOP = "on_stop"
CONF_ON_STREAMING = "on_streaming"
CONF_STREAM = "stream"

go2rtc_talkback_ns = cg.esphome_ns.namespace("go2rtc_talkback")
Go2rtcTalkback = go2rtc_talkback_ns.class_("Go2rtcTalkback", cg.Component)


def _validate_base_url(value):
    value = cv.url(value)
    if not value.startswith("http://"):
        raise cv.Invalid("url must start with http:// (go2rtc's API on the local network)")
    return value.rstrip("/")


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Go2rtcTalkback),
            cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                min_channels=1,
                max_channels=1,
            ),
            # base URL of the go2rtc API, e.g. http://192.168.1.10:1984
            cv.Required(CONF_URL): _validate_base_url,
            cv.Optional(CONF_USERNAME): cv.string_strict,
            cv.Optional(CONF_PASSWORD): cv.string_strict,
            # the go2rtc stream with the camera's backchannel
            cv.Required(CONF_STREAM): cv.string_strict,
            # port on this device where go2rtc's ffmpeg fetches the microphone
            cv.Optional(CONF_PORT, default=8555): cv.port,
            # who may fetch the microphone (normally only the go2rtc host); empty = everyone
            cv.Optional(CONF_ALLOWED_IPS, default=[]): cv.ensure_list(cv.ipv4address),
            # stop by itself after this time, in case nobody taps stop
            cv.Optional(
                CONF_MAX_DURATION, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_STREAMING): automation.validate_automation({}),
            cv.Optional(CONF_ON_STOP): automation.validate_automation({}),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation({}),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)

FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
            DOMAIN
        ),
    },
    extra=cv.ALLOW_EXTRA,
)

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_STREAMING, "add_on_streaming_callback"),
    automation.CallbackAutomation(CONF_ON_STOP, "add_on_stop_callback"),
    automation.CallbackAutomation(CONF_ON_ERROR, "add_on_error_callback"),
)

ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(Go2rtcTalkback),
    }
)

automation.register_apply_action(
    "go2rtc_talkback.start", ACTION_SCHEMA, automation.ApplyCall("start()")
)
automation.register_apply_action(
    "go2rtc_talkback.stop", ACTION_SCHEMA, automation.ApplyCall("stop()")
)
automation.register_apply_condition(
    "go2rtc_talkback.is_active", ACTION_SCHEMA, "is_active()"
)
automation.register_apply_condition(
    "go2rtc_talkback.is_streaming", ACTION_SCHEMA, "is_streaming()"
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_stream(config[CONF_STREAM]))
    if CONF_USERNAME in config:
        cg.add(
            var.set_credentials(config[CONF_USERNAME], config.get(CONF_PASSWORD, ""))
        )
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_duration(config[CONF_MAX_DURATION]))
    for ip in config[CONF_ALLOWED_IPS]:
        # network byte order, as in sockaddr_in.sin_addr.s_addr on the little endian ESP32
        addr = int.from_bytes(ipaddress.IPv4Address(str(ip)).packed, "little")
        cg.add(var.add_allowed_ip(addr))
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)

    # esp_http_client for the go2rtc API
    esp32.request_http_client()
