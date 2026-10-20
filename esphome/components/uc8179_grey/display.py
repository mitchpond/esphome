from esphome import pins
import esphome.codegen as cg
from esphome.components import display, http_request, spi
import esphome.config_validation as cv
from esphome.const import CONF_BUSY_PIN, CONF_DC_PIN, CONF_ID, CONF_LAMBDA, CONF_RESET_PIN

DEPENDENCIES = ["spi", "http_request"]

uc8179_grey_ns = cg.esphome_ns.namespace("uc8179_grey")
UC8179Grey = uc8179_grey_ns.class_("UC8179Grey", display.DisplayBuffer, spi.SPIDevice)

CONFIG_SCHEMA = (
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(UC8179Grey),
            cv.Required(CONF_DC_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_BUSY_PIN): pins.gpio_input_pin_schema,
            cv.GenerateID(http_request.CONF_HTTP_REQUEST_ID): cv.use_id(http_request.HttpRequestComponent),
        }
    )
    .extend(cv.polling_component_schema("never"))
    .extend(spi.spi_device_schema())
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    await spi.register_spi_device(var, config, write_only=True)
    cg.add(var.set_dc_pin(await cg.gpio_pin_expression(config[CONF_DC_PIN])))
    cg.add(var.set_reset_pin(await cg.gpio_pin_expression(config[CONF_RESET_PIN])))
    cg.add(var.set_busy_pin(await cg.gpio_pin_expression(config[CONF_BUSY_PIN])))
    cg.add(var.set_http(await cg.get_variable(config[http_request.CONF_HTTP_REQUEST_ID])))
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void)
        cg.add(var.set_writer(lambda_))
