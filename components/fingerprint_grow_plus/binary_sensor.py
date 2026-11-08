"""Optional "syncing" binary sensor of fingerprint_grow_plus."""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_RUNNING, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FINGERPRINT_SYNC_ID, FingerprintGrowPlusComponent

DEPENDENCIES = ["fingerprint_grow_plus"]

CONF_SYNCING = "syncing"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_SYNC_ID): cv.use_id(FingerprintGrowPlusComponent),
        cv.Optional(CONF_SYNCING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_SYNC_ID])
    if CONF_SYNCING in config:
        cg.add(
            parent.set_syncing_binary_sensor(
                await binary_sensor.new_binary_sensor(config[CONF_SYNCING])
            )
        )
