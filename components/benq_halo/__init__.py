"""BenQ ScreenBar Halo (1st generation) over a Holtek BM5602-60-1 (BC5602) module.

Protocol and BC5602 sequences from github.com/Djoko-cli/benq-screenbar-halo-matter,
built on github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration.
"""

import esphome.codegen as cg
from esphome.components import socket, spi
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["spi"]
MULTI_CONF = True

CONF_BENQ_HALO_ID = "benq_halo_id"
CONF_LISTEN = "listen"
CONF_BRIGHTNESS_GAMMA = "brightness_gamma"
CONF_REPEATS = "repeats"
CONF_MIN_ACKS = "min_acks"
CONF_PAIRING_PAYLOADS = "pairing_payloads"
CONF_REMOTE_BEACON = "remote_beacon"
CONF_PC_PRESENCE = "pc_presence"
CONF_HOST = "host"
CONF_INTERVAL = "interval"
CONF_OFF_DELAY = "off_delay"
CONF_PORT = "port"
CONF_CONTROL_LAMP = "control_lamp"

benq_halo_ns = cg.esphome_ns.namespace("benq_halo")
BenqHaloComponent = benq_halo_ns.class_("BenqHaloComponent", cg.Component, spi.SPIDevice)

# Register (write) order, like `lampe adresse` upstream. The on-air order is reversed.
_FORBIDDEN = {"00000000", "FFFFFFFF", "B0000159", "590100B0", "B00008E2", "E20800B0"}


def _hex_bytes(n):
    def validator(value):
        value = cv.string_strict(value).upper().replace(" ", "").replace(":", "")
        if value.startswith("0X"):
            value = value[2:]
        if len(value) != 2 * n:
            raise cv.Invalid(f"expected {2 * n} hex digits, got '{value}'")
        try:
            return [int(value[i : i + 2], 16) for i in range(0, 2 * n, 2)]
        except ValueError as e:
            raise cv.Invalid(f"invalid hex '{value}'") from e

    return validator


def validate_address(value):
    raw = _hex_bytes(4)(value)
    if "".join(f"{b:02X}" for b in raw) in _FORBIDDEN:
        raise cv.Invalid("address 0/FFFFFFFF or a pairing address is not allowed as link address")
    return raw


PC_PRESENCE_SCHEMA = cv.All(
    cv.Schema(
        {
            # "DESKTOP-ABC123", "DESKTOP-ABC123.local", a DNS name, or an IP.
            cv.Required(CONF_HOST): cv.string_strict,
            cv.Optional(CONF_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
            # PC unreachable this long -> lamp off (a reboot or a missed ping won't flick it).
            cv.Optional(CONF_OFF_DELAY, default="2min"): cv.positive_time_period_milliseconds,
            # 0 = ICMP ping. >0 = TCP connect to that port (e.g. 445 or 3389) if ping is blocked.
            cv.Optional(CONF_PORT, default=0): cv.int_range(min=0, max=65535),
            # false = only report PC online/offline (binary_sensor), don't switch the lamp.
            cv.Optional(CONF_CONTROL_LAMP, default=True): cv.boolean,
        }
    ),
    cv.only_on_esp32,
)


def _consume_pc_socket(config):
    if CONF_PC_PRESENCE in config:
        socket.consume_sockets(1, "benq_halo", socket.SocketType.TCP)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BenqHaloComponent),
            # Link address of YOUR remote/lamp pair, register order. Default is the
            # upstream author's (on air 63 FD F0 4F); the experimental pairing
            # button pairs the lamp to it.
            cv.Optional(CONF_ADDRESS, default="4FF0FD63"): validate_address,
            cv.Optional(CONF_LISTEN, default=True): cv.boolean,
            cv.Optional(CONF_BRIGHTNESS_GAMMA, default=2.0): cv.float_range(min=0.3, max=4.0),
            cv.Optional(CONF_REPEATS, default=3): cv.int_range(min=1, max=10),
            cv.Optional(CONF_MIN_ACKS, default=2): cv.int_range(min=1, max=10),
            # Your remote's beacon ID (the 4 bytes after 5A5A), from "Sniff remote pairing".
            cv.Optional(CONF_REMOTE_BEACON): _hex_bytes(4),
            # Lamp follows a PC: on when it answers, off after off_delay without answers.
            cv.Optional(CONF_PC_PRESENCE): PC_PRESENCE_SCHEMA,
            cv.Optional(CONF_PAIRING_PAYLOADS, default=["5A5A", "F5C3", "CF49"]): cv.All(
                cv.ensure_list(_hex_bytes(2)), cv.Length(min=1, max=16)
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(cs_pin_required=True, default_data_rate="1MHz", default_mode="MODE0")),
    _consume_pc_socket,
)

BENQ_HALO_CHILD_SCHEMA = cv.Schema({cv.GenerateID(CONF_BENQ_HALO_ID): cv.use_id(BenqHaloComponent)})


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    cg.add(var.set_address(*config[CONF_ADDRESS]))
    cg.add(var.set_listen(config[CONF_LISTEN]))
    cg.add(var.set_brightness_gamma(config[CONF_BRIGHTNESS_GAMMA]))
    cg.add(var.set_repeats(config[CONF_REPEATS]))
    cg.add(var.set_min_acks(config[CONF_MIN_ACKS]))
    if beacon := config.get(CONF_REMOTE_BEACON):
        cg.add(var.set_remote_beacon(*beacon))
    for a, b in config[CONF_PAIRING_PAYLOADS]:
        cg.add(var.add_pairing_payload(a, b))
    if pc := config.get(CONF_PC_PRESENCE):
        cg.add_define("USE_BENQ_HALO_PC_PRESENCE")
        cg.add(
            var.set_pc_presence(
                pc[CONF_HOST],
                pc[CONF_INTERVAL].total_milliseconds,
                pc[CONF_OFF_DELAY].total_milliseconds,
                pc[CONF_PORT],
                pc[CONF_CONTROL_LAMP],
            )
        )
