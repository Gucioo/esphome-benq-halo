import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import BENQ_HALO_CHILD_SCHEMA, CONF_BENQ_HALO_ID

DEPENDENCIES = ["benq_halo"]

CONF_LAST_FRAME = "last_frame"

CONFIG_SCHEMA = BENQ_HALO_CHILD_SCHEMA.extend(
    {
        # Last valid frame heard from the remote/lamp (or pairing beacon when sniffing).
        cv.Optional(CONF_LAST_FRAME): text_sensor.text_sensor_schema(
            icon="mdi:remote", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BENQ_HALO_ID])
    if conf := config.get(CONF_LAST_FRAME):
        var = await text_sensor.new_text_sensor(conf)
        cg.add(
            parent.add_on_frame_callback(
                cg.RawExpression(f"[](const std::string &s) {{ {var}->publish_state(s); }}")
            )
        )
