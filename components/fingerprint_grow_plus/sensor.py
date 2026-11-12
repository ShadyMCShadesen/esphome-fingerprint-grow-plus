"""Optional diagnostic sensors of fingerprint_grow_plus."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_TIMESTAMP,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

from . import CONF_FINGERPRINT_GROW_PLUS_ID, FingerprintGrowPlusComponent

DEPENDENCIES = ["fingerprint_grow_plus"]

CONF_PEERS_REACHABLE = "peers_reachable"
CONF_ERROR_COUNT = "error_count"
CONF_CONFLICT_COUNT = "conflict_count"
CONF_LAST_SYNC = "last_sync"
CONF_VERSIONS_LEFT = "versions_left"
CONF_FAILED_ATTEMPTS = "failed_attempts"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FINGERPRINT_GROW_PLUS_ID): cv.use_id(FingerprintGrowPlusComponent),
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
        # Synced changes left before the version counter must be reset (max 16383).
        cv.Optional(CONF_VERSIONS_LEFT): sensor.sensor_schema(
            icon="mdi:counter",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Failed attempts counted by lockout: (RAM only).
        cv.Optional(CONF_FAILED_ATTEMPTS): sensor.sensor_schema(
            icon="mdi:fingerprint-off",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_FINGERPRINT_GROW_PLUS_ID])
    for key, setter in (
        (CONF_PEERS_REACHABLE, parent.set_peers_reachable_sensor),
        (CONF_ERROR_COUNT, parent.set_error_count_sensor),
        (CONF_CONFLICT_COUNT, parent.set_conflict_count_sensor),
        (CONF_LAST_SYNC, parent.set_last_sync_sensor),
        (CONF_VERSIONS_LEFT, parent.set_versions_left_sensor),
        (CONF_FAILED_ATTEMPTS, parent.set_failed_attempts_sensor),
    ):
        if key in config:
            cg.add(setter(await sensor.new_sensor(config[key])))
