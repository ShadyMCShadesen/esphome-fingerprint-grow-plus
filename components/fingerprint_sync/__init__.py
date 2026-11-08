"""Synchronise GROW fingerprint templates directly between ESPHome nodes."""

import base64
import re

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, fingerprint_grow, sensor, socket, uart
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import (
    CONF_DURATION,
    CONF_ID,
    CONF_KEY,
    CONF_NUM_SCANS,
    CONF_PORT,
    CONF_TIME_ID,
)
from esphome.core import CORE

CODEOWNERS = ["@ShadyMCShadesen"]
DEPENDENCIES = ["uart", "fingerprint_grow", "network"]
AUTO_LOAD = ["socket", "sha256", "hmac_sha256", "sensor", "binary_sensor"]

CONF_FINGERPRINT_SYNC_ID = "fingerprint_sync_id"
CONF_HOST = "host"
CONF_SLOT = "slot"
CONF_CODE = "code"
CONF_FINGERPRINT_GROW_ID = "fingerprint_grow_id"
CONF_PEERS = "peers"
CONF_ALLOWED_NETWORKS = "allowed_networks"
CONF_INCLUDE_SLOTS = "include_slots"
CONF_EXCLUDE_SLOTS = "exclude_slots"
CONF_ENROLL_SLOTS = "enroll_slots"
CONF_SYNC_INTERVAL = "sync_interval"
CONF_STARTUP_DELAY = "startup_delay"
CONF_DRY_RUN = "dry_run"
CONF_METADATA_STORAGE = "metadata_storage"
CONF_DELETE_ALL_CODE = "delete_all_code"
CONF_FINGER_SENSOR = "finger_sensor"
CONF_FINGERPRINT_COUNT_SENSOR = "fingerprint_count_sensor"
CONF_SENSOR_ADDRESS = "sensor_address"
CONF_ADMIN = "admin"
CONF_TOUCHES = "touches"
CONF_WINDOW = "window"
CONF_CONFIRM_CLUSTER_WIDE = "confirm_cluster_wide"
CONF_KEEP_LOCAL = "keep_local"
CONF_COMPARE_SLOT = "compare_slot"
CONF_ON_SYNC_STARTED = "on_sync_started"
CONF_ON_SYNC_DONE = "on_sync_done"
CONF_ON_SYNC_FAILED = "on_sync_failed"
CONF_ON_CONFLICT = "on_conflict"
CONF_ON_SLOT_UPDATED = "on_slot_updated"
CONF_ON_SLOT_DELETED = "on_slot_deleted"
CONF_ON_ENROLL_STARTED = "on_enroll_started"
CONF_ON_ADMIN_TOUCH = "on_admin_touch"

DEFAULT_PORT = 18530

fingerprint_sync_ns = cg.esphome_ns.namespace("fingerprint_sync")
FingerprintSyncComponent = fingerprint_sync_ns.class_(
    "FingerprintSyncComponent", cg.Component, uart.UARTDevice
)
MetaBackend = fingerprint_sync_ns.enum("MetaBackend", is_class=True)
METADATA_STORAGE = {
    "preferences": MetaBackend.PREFERENCES,
    "sensor_notepad": MetaBackend.NOTEPAD,
}


def validate_key(value):
    value = cv.string_strict(value)
    try:
        raw = base64.b64decode(value, validate=True)
    except ValueError as err:
        raise cv.Invalid("key must be base64 encoded") from err
    if len(raw) != 32:
        raise cv.Invalid(
            "key must be exactly 32 bytes (base64 of 32 random bytes, "
            "e.g. `openssl rand -base64 32`)"
        )
    return value


_RANGE_RE = re.compile(r"^\s*(\d+)\s*(?:-\s*(\d+)\s*)?$")


def validate_slot_range(value):
    if isinstance(value, int):
        value = str(value)
    value = cv.string_strict(value)
    match = _RANGE_RE.match(value)
    if match is None:
        raise cv.Invalid(f"'{value}' is not a slot or slot range like '10-19'")
    low = int(match.group(1))
    high = int(match.group(2)) if match.group(2) is not None else low
    if high < low:
        raise cv.Invalid(f"slot range '{value}' is reversed")
    if high > 65534:
        raise cv.Invalid("slot numbers must be below 65535")
    return (low, high)


def validate_peer(value):
    if isinstance(value, str):
        host, _, port = value.partition(":")
        value = {CONF_HOST: host} if not port else {CONF_HOST: host, CONF_PORT: port}
    return PEER_SCHEMA(value)


PEER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_HOST): socket.ipv4_host,
        cv.Optional(CONF_PORT): cv.port,
    }
)

ADMIN_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SLOT, default=0): cv.uint16_t,
        cv.Optional(CONF_TOUCHES, default=2): cv.int_range(min=1, max=10),
        cv.Optional(
            CONF_WINDOW, default="5s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_NUM_SCANS, default=3): cv.int_range(min=1, max=6),
    }
)


def _default_metadata_storage(config):
    if CONF_METADATA_STORAGE not in config:
        config = dict(config)
        config[CONF_METADATA_STORAGE] = (
            "sensor_notepad" if CORE.is_esp8266 else "preferences"
        )
    return config


def _validate(config):
    if CORE.is_esp8266 and config[CONF_METADATA_STORAGE] == "preferences":
        raise cv.Invalid(
            "ESP8266 has only 512 bytes of flash preferences; use "
            "metadata_storage: sensor_notepad",
            path=[CONF_METADATA_STORAGE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FingerprintSyncComponent),
            cv.GenerateID(CONF_FINGERPRINT_GROW_ID): cv.use_id(
                fingerprint_grow.FingerprintGrowComponent
            ),
            cv.Required(CONF_KEY): validate_key,
            cv.Optional(CONF_PORT, default=DEFAULT_PORT): cv.port,
            cv.Optional(CONF_PEERS, default=[]): cv.ensure_list(validate_peer),
            cv.Optional(CONF_ALLOWED_NETWORKS): socket.IPV4_ALLOW_SCHEMA,
            cv.Optional(CONF_INCLUDE_SLOTS, default=[]): cv.ensure_list(
                validate_slot_range
            ),
            cv.Optional(CONF_EXCLUDE_SLOTS, default=[]): cv.ensure_list(
                validate_slot_range
            ),
            cv.Optional(CONF_ENROLL_SLOTS): validate_slot_range,
            cv.Optional(
                CONF_SYNC_INTERVAL, default="15min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_STARTUP_DELAY, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_DRY_RUN, default=False): cv.boolean,
            cv.Optional(CONF_METADATA_STORAGE): cv.enum(METADATA_STORAGE, lower=True),
            cv.Optional(CONF_DELETE_ALL_CODE): cv.All(
                cv.string_strict, cv.Length(min=4)
            ),
            cv.Optional(CONF_FINGER_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_FINGERPRINT_COUNT_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_SENSOR_ADDRESS, default=0xFFFFFFFF): cv.hex_uint32_t,
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_ADMIN): ADMIN_SCHEMA,
            cv.Optional(CONF_ON_SYNC_STARTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_SYNC_DONE): automation.validate_automation({}),
            cv.Optional(CONF_ON_SYNC_FAILED): automation.validate_automation({}),
            cv.Optional(CONF_ON_CONFLICT): automation.validate_automation({}),
            cv.Optional(CONF_ON_SLOT_UPDATED): automation.validate_automation({}),
            cv.Optional(CONF_ON_SLOT_DELETED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ENROLL_STARTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_TOUCH): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _default_metadata_storage,
    _validate,
    socket.consume_sockets(2, "fingerprint_sync"),
    socket.consume_sockets(1, "fingerprint_sync", socket.SocketType.TCP_LISTEN),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "fingerprint_sync", baud_rate=None, require_tx=True, require_rx=True
)

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_SYNC_STARTED, "add_on_sync_started_callback"),
    automation.CallbackAutomation(CONF_ON_SYNC_DONE, "add_on_sync_done_callback"),
    automation.CallbackAutomation(
        CONF_ON_SYNC_FAILED,
        "add_on_sync_failed_callback",
        [(cg.std_string, "reason")],
    ),
    automation.CallbackAutomation(
        CONF_ON_CONFLICT, "add_on_conflict_callback", [(cg.uint16, "slot")]
    ),
    automation.CallbackAutomation(
        CONF_ON_SLOT_UPDATED, "add_on_slot_updated_callback", [(cg.uint16, "slot")]
    ),
    automation.CallbackAutomation(
        CONF_ON_SLOT_DELETED, "add_on_slot_deleted_callback", [(cg.uint16, "slot")]
    ),
    automation.CallbackAutomation(
        CONF_ON_ENROLL_STARTED, "add_on_enroll_started_callback", [(cg.uint16, "slot")]
    ),
    automation.CallbackAutomation(
        CONF_ON_ADMIN_TOUCH, "add_on_admin_touch_callback", [(cg.uint8, "touches")]
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    grow = await cg.get_variable(config[CONF_FINGERPRINT_GROW_ID])
    cg.add(var.set_fingerprint_grow(grow))

    key = base64.b64decode(config[CONF_KEY])
    cg.add(
        var.set_key(
            cg.RawExpression(
                "std::array<uint8_t, 32>{" + ", ".join(f"0x{b:02X}" for b in key) + "}"
            )
        )
    )
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_sensor_address(config[CONF_SENSOR_ADDRESS]))
    for peer in config[CONF_PEERS]:
        cg.add(var.add_peer(peer[CONF_HOST], peer.get(CONF_PORT, config[CONF_PORT])))
    for low, high in config[CONF_INCLUDE_SLOTS]:
        cg.add(var.add_include_range(low, high))
    for low, high in config[CONF_EXCLUDE_SLOTS]:
        cg.add(var.add_exclude_range(low, high))
    if CONF_ENROLL_SLOTS in config:
        low, high = config[CONF_ENROLL_SLOTS]
        cg.add(var.set_enroll_range(low, high))
    cg.add(var.set_sync_interval(config[CONF_SYNC_INTERVAL]))
    cg.add(var.set_startup_delay(config[CONF_STARTUP_DELAY]))
    cg.add(var.set_dry_run(config[CONF_DRY_RUN]))
    cg.add(var.set_meta_backend(METADATA_STORAGE[str(config[CONF_METADATA_STORAGE])]))
    if CONF_DELETE_ALL_CODE in config:
        cg.add(var.set_delete_all_code(config[CONF_DELETE_ALL_CODE]))
    if CONF_FINGER_SENSOR in config:
        cg.add(var.set_finger_sensor(await cg.get_variable(config[CONF_FINGER_SENSOR])))
    if CONF_FINGERPRINT_COUNT_SENSOR in config:
        cg.add(
            var.set_fingerprint_count_sensor(
                await cg.get_variable(config[CONF_FINGERPRINT_COUNT_SENSOR])
            )
        )
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    if admin := config.get(CONF_ADMIN):
        cg.add(
            var.set_admin(
                admin[CONF_SLOT],
                admin[CONF_TOUCHES],
                admin[CONF_WINDOW],
                admin[CONF_NUM_SCANS],
            )
        )
    if networks := config.get(CONF_ALLOWED_NETWORKS):
        socket.add_ipv4_allow(var.set_allowed_networks, networks, config[CONF_ID])

    socket.require_tcp_listener()
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)


# ---------------------------------------------------------------------------------------
# Actions and conditions
# ---------------------------------------------------------------------------------------

_PARENT_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(FingerprintSyncComponent)})

automation.register_apply_action(
    "fingerprint_sync.sync",
    _PARENT_SCHEMA,
    automation.ApplyCall("sync_now()"),
)

automation.register_apply_action(
    "fingerprint_sync.delete",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
        },
        key=CONF_SLOT,
    ),
    automation.ApplyField(CONF_SLOT, "delete_slot", cg.uint16),
)


def _must_be_true(value):
    if cv.boolean(value) is not True:
        raise cv.Invalid(
            "delete_all removes the slots on ALL nodes; set confirm_cluster_wide: true"
        )
    return True


automation.register_apply_action(
    "fingerprint_sync.delete_all",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Required(CONF_CODE): cv.templatable(cv.string),
            cv.Required(CONF_CONFIRM_CLUSTER_WIDE): _must_be_true,
        }
    ),
    automation.ApplyField(CONF_CODE, "delete_all", cg.std_string),
)

automation.register_apply_action(
    "fingerprint_sync.enroll_next_free",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Optional(CONF_NUM_SCANS, default=3): cv.templatable(
                cv.int_range(min=1, max=6)
            ),
        },
        key=CONF_NUM_SCANS,
    ),
    automation.ApplyField(CONF_NUM_SCANS, "enroll_next_free", cg.uint8),
)

automation.register_apply_action(
    "fingerprint_sync.resolve_conflict",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
            cv.Required(CONF_KEEP_LOCAL): cv.templatable(cv.boolean),
        }
    ),
    automation.ApplyCall(
        "resolve_conflict({}, {})",
        ((CONF_SLOT, cg.uint16), (CONF_KEEP_LOCAL, cg.bool_)),
    ),
)

automation.register_apply_action(
    "fingerprint_sync.allow_restore",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Optional(CONF_DURATION, default="5min"): cv.templatable(
                cv.positive_time_period_seconds
            ),
        },
        key=CONF_DURATION,
    ),
    automation.ApplyField(CONF_DURATION, "allow_restore", cg.uint32),
)

automation.register_apply_action(
    "fingerprint_sync.self_test",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Optional(CONF_SLOT, default=0): cv.templatable(cv.uint16_t),
            # 65535 = none; otherwise also score this slot against compare_slot
            cv.Optional(CONF_COMPARE_SLOT, default=65535): cv.templatable(cv.uint16_t),
        },
        key=CONF_SLOT,
    ),
    automation.ApplyCall(
        "self_test({}, {})", ((CONF_SLOT, cg.uint16), (CONF_COMPARE_SLOT, cg.uint16))
    ),
)

automation.register_apply_action(
    "fingerprint_sync.resolve_all_conflicts",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintSyncComponent),
            cv.Required(CONF_KEEP_LOCAL): cv.templatable(cv.boolean),
        }
    ),
    automation.ApplyField(CONF_KEEP_LOCAL, "resolve_all_conflicts", cg.bool_),
)

automation.register_apply_condition(
    "fingerprint_sync.is_syncing", _PARENT_SCHEMA, "is_syncing()"
)
automation.register_apply_condition(
    "fingerprint_sync.is_ready", _PARENT_SCHEMA, "is_ready()"
)
