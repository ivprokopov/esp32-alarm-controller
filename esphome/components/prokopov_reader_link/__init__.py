import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID

DEPENDENCIES = ["wifi"]
AUTO_LOAD = ["prokopov_espnow"]
CODEOWNERS = []

CONF_PMK = "pmk"
CONF_LMK = "lmk"
CONF_AUTH_KEY = "auth_key"
CONF_HELLO_INTERVAL_MS = "hello_interval_ms"
CONF_RETRY_INTERVAL_MS = "retry_interval_ms"
CONF_MAX_ATTEMPTS = "max_attempts"
CONF_RESPONSE_TIMEOUT_MS = "response_timeout_ms"

reader_link_ns = cg.esphome_ns.namespace("prokopov_reader_link")
ProkopovReaderLink = reader_link_ns.class_("ProkopovReaderLink", cg.Component)


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
        cv.GenerateID(): cv.declare_id(ProkopovReaderLink),
        cv.Required(CONF_PMK): _hex_len(32),
        cv.Required(CONF_LMK): _hex_len(32),
        cv.Required(CONF_AUTH_KEY): _hex_len(64),
        cv.Optional(CONF_HELLO_INTERVAL_MS, default=3000): cv.int_range(min=500, max=60000),
        cv.Optional(CONF_RETRY_INTERVAL_MS, default=250): cv.int_range(min=100, max=5000),
        cv.Optional(CONF_MAX_ATTEMPTS, default=4): cv.int_range(min=1, max=10),
        cv.Optional(CONF_RESPONSE_TIMEOUT_MS, default=1400): cv.int_range(min=500, max=10000),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    include_builtin_idf_component("esp_wifi")
    include_builtin_idf_component("mbedtls")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_crypto(config[CONF_PMK], config[CONF_LMK], config[CONF_AUTH_KEY]))
    cg.add(var.set_timings(
        config[CONF_HELLO_INTERVAL_MS],
        config[CONF_RETRY_INTERVAL_MS],
        config[CONF_MAX_ATTEMPTS],
        config[CONF_RESPONSE_TIMEOUT_MS],
    ))
