"""Optional binary sensors of fingerprint_grow_plus."""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_RUNNING, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FINGERPRINT_GROW_PLUS_ID, FingerprintGrowPlusComponent

DEPENDENCIES = ["fingerprint_grow_plus"]

CONF_SYNCING = "syncing"
CONF_LOCKED = "locked"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_GROW_PLUS_ID): cv.use_id(FingerprintGrowPlusComponent),
        cv.Optional(CONF_SYNCING): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # On while a lockout is active (lockout:). No device class: HA's "lock" class means
        # on = unlocked, which would read the wrong way round here.
        cv.Optional(CONF_LOCKED): binary_sensor.binary_sensor_schema(
            icon="mdi:lock-alert",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_GROW_PLUS_ID])
    for key, setter in (
        (CONF_SYNCING, parent.set_syncing_binary_sensor),
        (CONF_LOCKED, parent.set_locked_binary_sensor),
    ):
        if key in config:
            cg.add(setter(await binary_sensor.new_binary_sensor(config[key])))
