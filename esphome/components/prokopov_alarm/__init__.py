import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID

DEPENDENCIES = ["wifi"]
AUTO_LOAD = ["json"]
CODEOWNERS = []

CONF_D0_PIN = "d0_pin"
CONF_D1_PIN = "d1_pin"
CONF_DOOR_RELAY_PIN = "door_relay_pin"
CONF_SIREN_RELAY_PIN = "siren_relay_pin"
CONF_READER_CONTROL_PIN = "reader_control_pin"
CONF_STATUS_LIGHT = "status_light"
CONF_MANAGEMENT_PORT = "management_port"
CONF_SHELLY_PORT = "shelly_port"
CONF_CONTROL_KEY = "control_key"
CONF_SHELLY_KEY = "shelly_key"
CONF_ARM_CONFIRM_WINDOW_MS = "arm_confirm_window_ms"
CONF_READER_VALID_PULSE_MS = "reader_valid_pulse_ms"
CONF_READER_ARM_PULSE_MS = "reader_arm_pulse_ms"
CONF_WIEGAND_FRAME_GAP_MS = "wiegand_frame_gap_ms"
CONF_WIEGAND_GLITCH_US = "wiegand_glitch_us"
CONF_BOOTSTRAP_CARDS = "bootstrap_cards"

prokopov_ns = cg.esphome_ns.namespace("prokopov_alarm")
ProkopovAlarm = prokopov_ns.class_("ProkopovAlarm", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ProkopovAlarm),
        cv.Required(CONF_D0_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_D1_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_DOOR_RELAY_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_SIREN_RELAY_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_READER_CONTROL_PIN): cv.int_range(min=0, max=39),
        cv.Required(CONF_STATUS_LIGHT): cv.use_id(light.LightState),
        cv.Optional(CONF_MANAGEMENT_PORT, default=8088): cv.port,
        cv.Optional(CONF_SHELLY_PORT, default=8081): cv.port,
        cv.Required(CONF_CONTROL_KEY): cv.string_strict,
        cv.Required(CONF_SHELLY_KEY): cv.string_strict,
        cv.Optional(CONF_ARM_CONFIRM_WINDOW_MS, default=5000): cv.int_range(min=500, max=60000),
        cv.Optional(CONF_READER_VALID_PULSE_MS, default=120): cv.int_range(min=20, max=5000),
        cv.Optional(CONF_READER_ARM_PULSE_MS, default=800): cv.int_range(min=20, max=5000),
        cv.Optional(CONF_WIEGAND_FRAME_GAP_MS, default=50): cv.int_range(min=10, max=500),
        cv.Optional(CONF_WIEGAND_GLITCH_US, default=250): cv.int_range(min=0, max=10000),
        cv.Optional(CONF_BOOTSTRAP_CARDS, default=[]): cv.ensure_list(cv.uint32_t),
    }
).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    # ESPHome 2026.x excludes several IDF components by default. This component
    # uses them directly for the SD/FAT filesystem and the two local HTTP APIs.
    include_builtin_idf_component("fatfs")
    include_builtin_idf_component("driver")
    include_builtin_idf_component("sdmmc")
    include_builtin_idf_component("esp_http_server")
    include_builtin_idf_component("json")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    status_light = await cg.get_variable(config[CONF_STATUS_LIGHT])

    cg.add(var.set_pins(
        config[CONF_D0_PIN],
        config[CONF_D1_PIN],
        config[CONF_DOOR_RELAY_PIN],
        config[CONF_SIREN_RELAY_PIN],
        config[CONF_READER_CONTROL_PIN],
    ))
    cg.add(var.set_status_light(status_light))
    cg.add(var.set_ports(config[CONF_MANAGEMENT_PORT], config[CONF_SHELLY_PORT]))
    cg.add(var.set_keys(config[CONF_CONTROL_KEY], config[CONF_SHELLY_KEY]))
    cg.add(var.set_timings(
        config[CONF_ARM_CONFIRM_WINDOW_MS],
        config[CONF_READER_VALID_PULSE_MS],
        config[CONF_READER_ARM_PULSE_MS],
        config[CONF_WIEGAND_FRAME_GAP_MS],
        config[CONF_WIEGAND_GLITCH_US],
    ))
    for uid in config[CONF_BOOTSTRAP_CARDS]:
        cg.add(var.add_bootstrap_card(uid))
