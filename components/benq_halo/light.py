import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_DEFAULT_TRANSITION_LENGTH, CONF_OUTPUT_ID

from . import BENQ_HALO_CHILD_SCHEMA, CONF_BENQ_HALO_ID, benq_halo_ns

DEPENDENCIES = ["benq_halo"]

BenqHaloLight = benq_halo_ns.class_("BenqHaloLight", light.LightOutput)

CONFIG_SCHEMA = (
    light.light_schema(BenqHaloLight, light.LightType.BRIGHTNESS_ONLY)
    .extend(
        {
            # The lamp fades by itself; ESPHome transitions would only spam the radio.
            cv.Optional(CONF_DEFAULT_TRANSITION_LENGTH, default="0s"): cv.positive_time_period_milliseconds,
        }
    )
    .extend(BENQ_HALO_CHILD_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    parent = await cg.get_variable(config[CONF_BENQ_HALO_ID])
    cg.add(var.set_parent(parent))
