"""RS-485 link between the PROKOPOV alarm board and the 2.8" touch panel.

Controller (alarm board):
    prokopov_panel_link:
      uart_id: rs485
      alarm_id: alarm_core
      key: !secret panel_link_key         # 64 hex chars, same on both ends

Panel (ESP32-2432S028):
    prokopov_panel_link:
      id: link
      uart_id: rs485
      key: !secret panel_link_key
      on_status:
        - lambda: 'id(ui_state) = x.state;'
    # send: id(link).send_pin(4 /*DISARM*/, "1234");
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import uart
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["uart"]
CODEOWNERS = []

CONF_ALARM_ID = "alarm_id"
CONF_KEY = "key"
CONF_STATUS_INTERVAL = "status_interval"
CONF_ON_STATUS = "on_status"

ns = cg.esphome_ns.namespace("prokopov_panel_link")
PanelLink = ns.class_("PanelLink", cg.Component, uart.UARTDevice)
PanelStatus = ns.struct("PanelStatus")
StatusTrigger = ns.class_("StatusTrigger", automation.Trigger.template(PanelStatus))


def _key(value):
    value = cv.string_strict(value)
    if len(value) != 64:
        raise cv.Invalid("key must be exactly 64 hexadecimal characters (256 bit)")
    try:
        int(value, 16)
    except ValueError as err:
        raise cv.Invalid("key must be hexadecimal") from err
    return value.lower()


def _alarm_id(value):
    from esphome.components.prokopov_alarm import ProkopovAlarm  # noqa: WPS433 (optional dependency)
    return cv.use_id(ProkopovAlarm)(value)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PanelLink),
            cv.Optional(CONF_ALARM_ID): _alarm_id,
            cv.Required(CONF_KEY): _key,
            cv.Optional(CONF_STATUS_INTERVAL, default="500ms"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(milliseconds=200))),
            cv.Optional(CONF_ON_STATUS): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StatusTrigger)}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    include_builtin_idf_component("mbedtls")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_key(config[CONF_KEY]))
    cg.add(var.set_status_interval(config[CONF_STATUS_INTERVAL].total_milliseconds))
    if CONF_ALARM_ID in config:
        cg.add_define("USE_PROKOPOV_PANEL_CONTROLLER")
        alarm = await cg.get_variable(config[CONF_ALARM_ID])
        cg.add(var.set_controller(alarm))
    for conf in config.get(CONF_ON_STATUS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(PanelStatus, "x")], conf)
