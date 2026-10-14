import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG, ENTITY_CATEGORY_DIAGNOSTIC

from . import BENQ_HALO_CHILD_SCHEMA, CONF_BENQ_HALO_ID, benq_halo_ns

DEPENDENCIES = ["benq_halo"]

BenqHaloButton = benq_halo_ns.class_("BenqHaloButton", button.Button)
ButtonAction = benq_halo_ns.enum("ButtonAction", is_class=True)

CONF_AUTO_MODE = "auto_mode"
CONF_RESYNC = "resync"
CONF_PAIR_LAMP = "pair_lamp"
CONF_SNIFF_PAIRING = "sniff_pairing"
CONF_SCAN_ADDRESSES = "scan_addresses"

ACTIONS = {
    CONF_AUTO_MODE: ButtonAction.AUTO_MODE,
    CONF_RESYNC: ButtonAction.RESYNC,
    CONF_PAIR_LAMP: ButtonAction.PAIR_LAMP,
    CONF_SNIFF_PAIRING: ButtonAction.SNIFF_PAIRING,
    CONF_SCAN_ADDRESSES: ButtonAction.SCAN_ADDRESSES,
}

CONFIG_SCHEMA = BENQ_HALO_CHILD_SCHEMA.extend(
    {
        # Remote's "A" button (auto-dimming / sensor mode). Ignored while off.
        cv.Optional(CONF_AUTO_MODE): button.button_schema(BenqHaloButton, icon="mdi:brightness-auto"),
        # Re-send the full believed state (e.g. after the lamp lost power).
        cv.Optional(CONF_RESYNC): button.button_schema(
            BenqHaloButton, icon="mdi:sync", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        # EXPERIMENTAL: replay a pairing beacon so the lamp learns this ESP.
        cv.Optional(CONF_PAIR_LAMP): button.button_schema(
            BenqHaloButton, icon="mdi:link-variant", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        # Listen 60 s on the pairing address and log your remote's beacon.
        cv.Optional(CONF_SNIFF_PAIRING): button.button_schema(
            BenqHaloButton, icon="mdi:radio-tower", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # Try link addresses derived from the remote's beacon ID while you turn the dial.
        cv.Optional(CONF_SCAN_ADDRESSES): button.button_schema(
            BenqHaloButton, icon="mdi:magnify-scan", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BENQ_HALO_ID])
    for key, action in ACTIONS.items():
        if conf := config.get(key):
            var = await button.new_button(conf)
            cg.add(var.set_parent(parent))
            cg.add(var.set_action(action))
