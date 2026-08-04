import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import spi
from esphome import automation
from esphome.const import CONF_ID, CONF_CS_PIN, CONF_TRIGGER_ID

# Define the C++ namespace and class for the component
desfire_pn532_ns = cg.esphome_ns.namespace("desfire_pn532")
DesfirePN532 = desfire_pn532_ns.class_("DesfirePN532", cg.Component, spi.SPIDevice)

# Define the custom trigger class
DesfireAuthenticatedTrigger = desfire_pn532_ns.class_(
    "DesfireAuthenticatedTrigger", automation.Trigger.template(cg.std_string)
)

CONF_NASC_KEY = "nasc_key"
CONF_IRQ_PIN = "irq_pin"
CONF_ON_AUTHENTICATED = "on_authenticated"

# Validation schema for the component configuration
CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DesfirePN532),
            cv.Required(CONF_CS_PIN): cv.All(cv.pins.internal_gpio_output_pin_schema),
            cv.Required(CONF_IRQ_PIN): cv.All(cv.pins.internal_gpio_input_pin_schema),
            cv.Required(CONF_NASC_KEY): cv.string_strict,
            cv.Optional(CONF_ON_AUTHENTICATED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        DesfireAuthenticatedTrigger
                    ),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema())
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    irq_pin = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
    cg.add(var.set_irq_pin(irq_pin))
    cg.add(var.set_nasc_key(config[CONF_NASC_KEY]))

    for conf in config.get(CONF_ON_AUTHENTICATED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "user_id")], conf)
