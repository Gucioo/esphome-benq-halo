import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from . import BENQ_HALO_CHILD_SCHEMA, CONF_BENQ_HALO_ID, benq_halo_ns

DEPENDENCIES = ["benq_halo"]

BenqHaloLampSelect = benq_halo_ns.class_("BenqHaloLampSelect", select.Select)

OPTIONS = ["Front", "Back", "Both"]  # order matters: matches the C++ index

CONFIG_SCHEMA = select.select_schema(BenqHaloLampSelect, icon="mdi:lamps").extend(BENQ_HALO_CHILD_SCHEMA)


async def to_code(config):
    var = await select.new_select(config, options=OPTIONS)
    parent = await cg.get_variable(config[CONF_BENQ_HALO_ID])
    cg.add(var.set_parent(parent))
