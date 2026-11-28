import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_TYPE
from esphome.types import ConfigType

CODEOWNERS = ["@DT-art1"]
DOMAIN = "camera_encoder"

AUTO_LOAD = ["camera"]

CONF_BUFFER_EXPAND_SIZE = "buffer_expand_size"
CONF_ENCODER_BUFFER_ID = "encoder_buffer_id"
CONF_QUALITY = "quality"
CONF_SUBSAMPLING = "subsampling"

ESP32_CAMERA_ENCODER = "esp32_camera"
HARDWARE_ENCODER = "hardware"

camera_ns = cg.esphome_ns.namespace("camera")
camera_encoder_ns = cg.esphome_ns.namespace("camera_encoder")

Encoder = camera_ns.class_("Encoder")
EncoderBufferImpl = camera_encoder_ns.class_("EncoderBufferImpl")

ESP32CameraJPEGEncoder = camera_encoder_ns.class_("ESP32CameraJPEGEncoder", Encoder)
ESP32HardwareJPEGEncoder = camera_encoder_ns.class_("ESP32HardwareJPEGEncoder", Encoder)
ESP32HardwareJPEGEncoderBuffer = camera_encoder_ns.class_(
    "ESP32HardwareJPEGEncoderBuffer"
)

JpegDownSampling = cg.global_ns.enum("jpeg_down_sampling_type_t")
SUBSAMPLING = {
    "4:4:4": JpegDownSampling.JPEG_DOWN_SAMPLING_YUV444,
    "4:2:2": JpegDownSampling.JPEG_DOWN_SAMPLING_YUV422,
    "4:2:0": JpegDownSampling.JPEG_DOWN_SAMPLING_YUV420,
}

MAX_JPEG_BUFFER_SIZE_2MB = 2 * 1024 * 1024


def _encoder_schema(
    encoder_class: cg.MockObjClass,
    buffer_class: cg.MockObjClass,
    buffer_size: int,
    buffer_expand_size: int,
) -> cv.Schema:
    return cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(encoder_class),
            cv.Optional(CONF_QUALITY, default=80): cv.int_range(1, 100),
            cv.Optional(CONF_BUFFER_SIZE, default=buffer_size): cv.int_range(
                1024, MAX_JPEG_BUFFER_SIZE_2MB
            ),
            cv.Optional(
                CONF_BUFFER_EXPAND_SIZE, default=buffer_expand_size
            ): cv.int_range(0, MAX_JPEG_BUFFER_SIZE_2MB),
            cv.GenerateID(CONF_ENCODER_BUFFER_ID): cv.declare_id(buffer_class),
        }
    )


ESP32_CAMERA_ENCODER_SCHEMA = _encoder_schema(
    ESP32CameraJPEGEncoder, EncoderBufferImpl, 4096, 1024
)

# The JPEG codec of the ESP32-P4. A screenshot or camera frame is usually tens of kilobytes,
# and each retry with a larger buffer encodes the whole frame again, so it starts larger.
HARDWARE_ENCODER_SCHEMA = cv.All(
    _encoder_schema(
        ESP32HardwareJPEGEncoder, ESP32HardwareJPEGEncoderBuffer, 65536, 32768
    ).extend({cv.Optional(CONF_SUBSAMPLING, default="4:2:0"): cv.enum(SUBSAMPLING)}),
    esp32.only_on_variant(
        supported=[esp32.VARIANT_ESP32P4], msg_prefix="The hardware JPEG encoder"
    ),
)

CONFIG_SCHEMA = cv.typed_schema(
    {
        ESP32_CAMERA_ENCODER: ESP32_CAMERA_ENCODER_SCHEMA,
        HARDWARE_ENCODER: HARDWARE_ENCODER_SCHEMA,
    },
    default_type=ESP32_CAMERA_ENCODER,
)


async def to_code(config: ConfigType) -> None:
    buffer = cg.new_Pvariable(config[CONF_ENCODER_BUFFER_ID])
    cg.add(buffer.set_buffer_size(config[CONF_BUFFER_SIZE]))
    args = [config[CONF_QUALITY]]
    if config[CONF_TYPE] == ESP32_CAMERA_ENCODER:
        esp32.add_idf_component(name="espressif/esp32-camera", ref="2.1.7")
        cg.add_define("USE_ESP32_CAMERA_JPEG_ENCODER")
    else:
        esp32.include_builtin_idf_component("esp_driver_jpeg")
        cg.add_define("USE_ESP32_HARDWARE_JPEG_ENCODER")
        args.append(config[CONF_SUBSAMPLING])
    var = cg.new_Pvariable(config[CONF_ID], *args, buffer)
    cg.add(var.set_buffer_expand_size(config[CONF_BUFFER_EXPAND_SIZE]))
