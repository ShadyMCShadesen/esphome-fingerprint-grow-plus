"""Optional status text sensor of fingerprint_grow_plus."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_STATUS, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FINGERPRINT_GROW_PLUS_ID, FingerprintGrowPlusComponent

DEPENDENCIES = ["fingerprint_grow_plus"]

CONF_OCCUPIED_SLOTS = "occupied_slots"

CONF_UNREACHABLE_PEERS = "unreachable_peers"
CONF_LAST_NAME = "last_name"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_GROW_PLUS_ID): cv.use_id(FingerprintGrowPlusComponent),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:sync", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # Occupied slots of this sensor as ranges, e.g. "0, 2-5, 17, 150-151".
        cv.Optional(CONF_OCCUPIED_SLOTS): text_sensor.text_sensor_schema(
            icon="mdi:fingerprint"
        ),
        # Peers that failed in the last sync round with the reason, "-" when all were reached.
        cv.Optional(CONF_UNREACHABLE_PEERS): text_sensor.text_sensor_schema(
            icon="mdi:lan-disconnect", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # Name of the last recognised finger ("#<slot>" if it has none).
        cv.Optional(CONF_LAST_NAME): text_sensor.text_sensor_schema(icon="mdi:account"),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_GROW_PLUS_ID])
    for key, setter in (
        (CONF_STATUS, parent.set_status_text_sensor),
        (CONF_OCCUPIED_SLOTS, parent.set_occupied_text_sensor),
        (CONF_UNREACHABLE_PEERS, parent.set_unreachable_text_sensor),
        (CONF_LAST_NAME, parent.set_last_name_text_sensor),
    ):
        if key in config:
            cg.add(setter(await text_sensor.new_text_sensor(config[key])))
