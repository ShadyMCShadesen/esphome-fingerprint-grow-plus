"""Optional diagnostic sensors of fingerprint_sync."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_TIMESTAMP,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

from . import CONF_FINGERPRINT_SYNC_ID, FingerprintSyncComponent

DEPENDENCIES = ["fingerprint_sync"]

CONF_PEERS_REACHABLE = "peers_reachable"
CONF_ERROR_COUNT = "error_count"
CONF_CONFLICT_COUNT = "conflict_count"
CONF_LAST_SYNC = "last_sync"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_SYNC_ID): cv.use_id(FingerprintSyncComponent),
        cv.Optional(CONF_PEERS_REACHABLE): sensor.sensor_schema(
            icon="mdi:lan-connect",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_ERROR_COUNT): sensor.sensor_schema(
            icon="mdi:alert-circle-outline",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CONFLICT_COUNT): sensor.sensor_schema(
            icon="mdi:fingerprint-off",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LAST_SYNC): sensor.sensor_schema(
            device_class=DEVICE_CLASS_TIMESTAMP,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_SYNC_ID])
    for key, setter in (
        (CONF_PEERS_REACHABLE, parent.set_peers_reachable_sensor),
        (CONF_ERROR_COUNT, parent.set_error_count_sensor),
        (CONF_CONFLICT_COUNT, parent.set_conflict_count_sensor),
        (CONF_LAST_SYNC, parent.set_last_sync_sensor),
    ):
        if key in config:
            cg.add(setter(await sensor.new_sensor(config[key])))
