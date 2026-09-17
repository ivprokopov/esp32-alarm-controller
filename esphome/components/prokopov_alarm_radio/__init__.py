import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID

from esphome.components.prokopov_alarm import ProkopovAlarm

DEPENDENCIES = ["wifi", "prokopov_alarm"]
CODEOWNERS = []

CONF_ALARM_ID = "alarm_id"
CONF_PMK = "pmk"
CONF_LMK = "lmk"
CONF_AUTH_KEY = "auth_key"
CONF_HELLO_INTERVAL_MS = "hello_interval_ms"
CONF_STATE_INTERVAL_MS = "state_interval_ms"

prokopov_radio_ns = cg.esphome_ns.namespace("prokopov_alarm_radio")
ProkopovAlarmRadio = prokopov_radio_ns.class_("ProkopovAlarmRadio", cg.Component)


def _hex_len(length):
    def validator(value):
        value = cv.string_strict(value)
        if len(value) != length:
            raise cv.Invalid(f"must contain exactly {length} hexadecimal characters")
        try:
            int(value, 16)
        except ValueError as err:
            raise cv.Invalid("must contain hexadecimal characters only") from err
        return value.lower()
    return validator


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ProkopovAlarmRadio),
        cv.Required(CONF_ALARM_ID): cv.use_id(ProkopovAlarm),
        cv.Required(CONF_PMK): _hex_len(32),
        cv.Required(CONF_LMK): _hex_len(32),
        cv.Required(CONF_AUTH_KEY): _hex_len(64),
        cv.Optional(CONF_HELLO_INTERVAL_MS, default=3000): cv.int_range(min=500, max=60000),
        cv.Optional(CONF_STATE_INTERVAL_MS, default=1000): cv.int_range(min=250, max=60000),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    include_builtin_idf_component("esp_wifi")
    include_builtin_idf_component("mbedtls")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    alarm = await cg.get_variable(config[CONF_ALARM_ID])
    cg.add(var.set_alarm(alarm))
    cg.add(var.set_crypto(config[CONF_PMK], config[CONF_LMK], config[CONF_AUTH_KEY]))
    cg.add(var.set_intervals(config[CONF_HELLO_INTERVAL_MS], config[CONF_STATE_INTERVAL_MS]))
