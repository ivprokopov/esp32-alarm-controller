"""PROKOPOV alarm core v3 — main board Rev A (ESP32-S3 DevKit).

The I2C address of the component is the FRAM (MB85RC256V, default 0x50).
Board inputs (zones, door contact, tamper, mains, siren line, Uni commands) are ordinary
ESPHome binary_sensors that call the public methods from on_state lambdas, see
examples/prokopov-alarm-v3.yaml.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, light
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID

DEPENDENCIES = ["wifi", "i2c"]
AUTO_LOAD = ["json"]
CODEOWNERS = []

CONF_READERS = "readers"
CONF_D0_PIN = "d0_pin"
CONF_D1_PIN = "d1_pin"
CONF_LOCK_PIN = "lock_pin"
CONF_LOCK_TYPE = "lock_type"
CONF_SIREN_PIN = "siren_pin"
CONF_READER_LED_PIN = "reader_led_pin"
CONF_READER_BEEP_PIN = "reader_beep_pin"
CONF_ARMED_OUTPUT_PIN = "armed_output_pin"
CONF_ALARM_OUTPUT_PIN = "alarm_output_pin"
CONF_WATCHDOG_PIN = "watchdog_pin"
CONF_USE_FRAM = "use_fram"
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
CONF_LOCKOUT = "lockout"
CONF_ATTEMPTS = "attempts"
CONF_WINDOW = "window"
CONF_DURATION = "duration"
CONF_BOOTSTRAP_CARDS = "bootstrap_cards"

prokopov_ns = cg.esphome_ns.namespace("prokopov_alarm")
ProkopovAlarm = prokopov_ns.class_("ProkopovAlarm", cg.Component, i2c.I2CDevice)

LOCK_TYPES = {"maglock": 0, "strike": 1}
GPIO = cv.int_range(min=0, max=48)

READER_SCHEMA = cv.Schema({cv.Required(CONF_D0_PIN): GPIO, cv.Required(CONF_D1_PIN): GPIO})

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ProkopovAlarm),
            cv.Required(CONF_READERS): cv.All(cv.ensure_list(READER_SCHEMA), cv.Length(min=1, max=2)),
            cv.Optional(CONF_LOCK_PIN, default=40): GPIO,
            cv.Optional(CONF_LOCK_TYPE, default="maglock"): cv.enum(LOCK_TYPES, lower=True),
            cv.Optional(CONF_SIREN_PIN, default=41): GPIO,
            cv.Optional(CONF_READER_LED_PIN, default=14): GPIO,
            cv.Optional(CONF_READER_BEEP_PIN, default=21): GPIO,
            cv.Optional(CONF_ARMED_OUTPUT_PIN, default=39): GPIO,
            cv.Optional(CONF_ALARM_OUTPUT_PIN, default=48): GPIO,
            cv.Optional(CONF_WATCHDOG_PIN, default=10): cv.Any(GPIO, cv.one_of("none", lower=True)),
            cv.Optional(CONF_USE_FRAM, default=True): cv.boolean,
            cv.Optional(CONF_STATUS_LIGHT): cv.use_id(light.LightState),
            cv.Optional(CONF_MANAGEMENT_PORT, default=8088): cv.port,
            cv.Optional(CONF_SHELLY_PORT, default=8081): cv.port,
            cv.Required(CONF_CONTROL_KEY): cv.All(cv.string_strict, cv.Length(min=16)),
            cv.Required(CONF_SHELLY_KEY): cv.All(cv.string_strict, cv.Length(min=12)),
            cv.Optional(CONF_ARM_CONFIRM_WINDOW_MS, default=5000): cv.int_range(min=500, max=60000),
            cv.Optional(CONF_READER_VALID_PULSE_MS, default=120): cv.int_range(min=20, max=5000),
            cv.Optional(CONF_READER_ARM_PULSE_MS, default=800): cv.int_range(min=20, max=5000),
            cv.Optional(CONF_WIEGAND_FRAME_GAP_MS, default=50): cv.int_range(min=10, max=500),
            cv.Optional(CONF_WIEGAND_GLITCH_US, default=250): cv.int_range(min=0, max=10000),
            cv.Optional(CONF_LOCKOUT, default={}): cv.Schema(
                {
                    cv.Optional(CONF_ATTEMPTS, default=5): cv.int_range(min=0, max=20),
                    cv.Optional(CONF_WINDOW, default="5min"): cv.positive_time_period_seconds,
                    cv.Optional(CONF_DURATION, default="5min"): cv.positive_time_period_seconds,
                }
            ),
            cv.Optional(CONF_BOOTSTRAP_CARDS, default=[]): cv.ensure_list(cv.uint32_t),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x50))
)


async def to_code(config):
    # flash FAT for cards/config, HTTP APIs, SHA-256 for PIN hashes
    include_builtin_idf_component("fatfs")
    include_builtin_idf_component("wear_levelling")
    include_builtin_idf_component("driver")
    include_builtin_idf_component("esp_http_server")
    include_builtin_idf_component("json")
    include_builtin_idf_component("mbedtls")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    for i, r in enumerate(config[CONF_READERS]):
        cg.add(var.set_reader_pins(i, r[CONF_D0_PIN], r[CONF_D1_PIN]))
    wdt = config[CONF_WATCHDOG_PIN]
    cg.add(var.set_output_pins(
        config[CONF_LOCK_PIN], config[CONF_SIREN_PIN], config[CONF_READER_LED_PIN], config[CONF_READER_BEEP_PIN],
        config[CONF_ARMED_OUTPUT_PIN], config[CONF_ALARM_OUTPUT_PIN], -1 if wdt == "none" else wdt))
    cg.add(var.set_lock_type(config[CONF_LOCK_TYPE]))
    cg.add(var.set_use_fram(config[CONF_USE_FRAM]))
    if CONF_STATUS_LIGHT in config:
        cg.add(var.set_status_light(await cg.get_variable(config[CONF_STATUS_LIGHT])))
    cg.add(var.set_ports(config[CONF_MANAGEMENT_PORT], config[CONF_SHELLY_PORT]))
    cg.add(var.set_keys(config[CONF_CONTROL_KEY], config[CONF_SHELLY_KEY]))
    cg.add(var.set_timings(
        config[CONF_ARM_CONFIRM_WINDOW_MS], config[CONF_READER_VALID_PULSE_MS], config[CONF_READER_ARM_PULSE_MS],
        config[CONF_WIEGAND_FRAME_GAP_MS], config[CONF_WIEGAND_GLITCH_US]))
    lo = config[CONF_LOCKOUT]
    cg.add(var.set_lockout(lo[CONF_ATTEMPTS], lo[CONF_WINDOW].total_seconds, lo[CONF_DURATION].total_seconds))
    for uid in config[CONF_BOOTSTRAP_CARDS]:
        cg.add(var.add_bootstrap_card(uid))
