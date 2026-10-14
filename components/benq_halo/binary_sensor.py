import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import BENQ_HALO_CHILD_SCHEMA, CONF_BENQ_HALO_ID

DEPENDENCIES = ["benq_halo"]

CONF_LINK = "link"
CONF_PC_ONLINE = "pc_online"

CONFIG_SCHEMA = BENQ_HALO_CHILD_SCHEMA.extend(
    {
        # ON after the lamp acknowledged a frame, OFF after a request was given up.
        cv.Optional(CONF_LINK): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # Needs pc_presence in benq_halo.
        cv.Optional(CONF_PC_ONLINE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY, icon="mdi:desktop-tower-monitor"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BENQ_HALO_ID])
    if conf := config.get(CONF_LINK):
        var = await binary_sensor.new_binary_sensor(conf)
        cg.add(parent.add_on_link_callback(cg.RawExpression(f"[](bool ok) {{ {var}->publish_state(ok); }}")))
    if conf := config.get(CONF_PC_ONLINE):
        var = await binary_sensor.new_binary_sensor(conf)
        cg.add(parent.add_on_pc_callback(cg.RawExpression(f"[](bool on) {{ {var}->publish_state(on); }}")))
