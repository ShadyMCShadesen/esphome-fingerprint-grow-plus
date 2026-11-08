"""Optional status text sensor of fingerprint_grow_plus."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_STATUS, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FINGERPRINT_SYNC_ID, FingerprintGrowPlusComponent

DEPENDENCIES = ["fingerprint_grow_plus"]

CONF_OCCUPIED_SLOTS = "occupied_slots"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_SYNC_ID): cv.use_id(FingerprintGrowPlusComponent),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:sync", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
        # Occupied slots of this sensor as ranges, e.g. "0, 2-5, 99, 150-151".
        cv.Optional(CONF_OCCUPIED_SLOTS): text_sensor.text_sensor_schema(
            icon="mdi:fingerprint"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_SYNC_ID])
    if CONF_STATUS in config:
        cg.add(parent.set_status_text_sensor(await text_sensor.new_text_sensor(config[CONF_STATUS])))
    if CONF_OCCUPIED_SLOTS in config:
        cg.add(
            parent.set_occupied_text_sensor(
                await text_sensor.new_text_sensor(config[CONF_OCCUPIED_SLOTS])
            )
        )
