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

CONF_FINGERPRINT_SYNC_ID = "fingerprint_grow_plus_id"
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
CONF_FINGERPRINT_CAPACITY_SENSOR = "fingerprint_capacity_sensor"
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
CONF_ON_ADMIN_GESTURE = "on_admin_gesture"
CONF_ON_ADMIN_HOLD = "on_admin_hold"
CONF_ON_ADMIN_WAITING = "on_admin_waiting"
CONF_ON_ADMIN_CANCELLED = "on_admin_cancelled"
CONF_ON_ADMIN_DONE = "on_admin_done"
CONF_SLOTS = "slots"
CONF_ENROLL_TOUCHES = "enroll_touches"
CONF_DELETE_TOUCHES = "delete_touches"
CONF_REENROLL_TOUCHES = "reenroll_touches"
CONF_GUEST_TOUCHES = "guest_touches"
CONF_HOLD_TIME = "hold_time"
CONF_TARGET_TIMEOUT = "target_timeout"
CONF_GUESTS = "guests"
CONF_VALID_FOR = "valid_for"

DEFAULT_PORT = 18530

fingerprint_grow_plus_ns = cg.esphome_ns.namespace("fingerprint_grow_plus")
FingerprintGrowPlusComponent = fingerprint_grow_plus_ns.class_(
    "FingerprintGrowPlusComponent", cg.Component, uart.UARTDevice
)
MetaBackend = fingerprint_grow_plus_ns.enum("MetaBackend", is_class=True)
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

_TOUCHES = cv.int_range(min=0, max=10)


def _validate_admin(config):
    config = dict(config)
    slots = list(config.pop(CONF_SLOTS, []))
    if CONF_SLOT in config:
        slots.append(config.pop(CONF_SLOT))
    config[CONF_SLOTS] = sorted(set(slots)) or [0]
    if CONF_TOUCHES in config:  # older name of enroll_touches
        config[CONF_ENROLL_TOUCHES] = config.pop(CONF_TOUCHES)
    config.setdefault(CONF_ENROLL_TOUCHES, 2)
    used = [
        config.get(k, 0)
        for k in (CONF_ENROLL_TOUCHES, CONF_DELETE_TOUCHES, CONF_REENROLL_TOUCHES, CONF_GUEST_TOUCHES)
        if config.get(k, 0) > 0
    ]
    if len(used) != len(set(used)):
        raise cv.Invalid("every admin gesture needs its own number of touches (0 = disabled)")
    return config


def _empty_dict_if_none(value):
    # `admin:` without options (or `admin: true`) enables the admin finger with all defaults.
    return {} if value is None or value is True else value


ADMIN_SCHEMA = cv.All(
    _empty_dict_if_none,
    cv.Schema(
        {
            cv.Optional(CONF_SLOT): cv.uint16_t,
            cv.Optional(CONF_SLOTS): cv.All(cv.ensure_list(cv.uint16_t), cv.Length(min=1, max=10)),
            cv.Optional(CONF_TOUCHES): _TOUCHES,
            cv.Optional(CONF_ENROLL_TOUCHES): _TOUCHES,
            cv.Optional(CONF_DELETE_TOUCHES, default=3): _TOUCHES,
            cv.Optional(CONF_REENROLL_TOUCHES, default=4): _TOUCHES,
            # Default: 5 when guests are configured, otherwise off.
            cv.Optional(CONF_GUEST_TOUCHES): _TOUCHES,
            # Pause after the last touch that completes a gesture.
            cv.Optional(CONF_WINDOW, default="2s"): cv.positive_time_period_milliseconds,
            # Keep the admin finger on the sensor this long for on_admin_hold (needs finger_sensor).
            cv.Optional(CONF_HOLD_TIME, default="3s"): cv.positive_time_period_milliseconds,
            # Time to present the target finger for delete / re-enroll.
            cv.Optional(CONF_TARGET_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_NUM_SCANS, default=3): cv.int_range(min=1, max=6),
        }
    ),
    _validate_admin,
)

GUESTS_SCHEMA = cv.maybe_simple_value(
    {
        cv.Required(CONF_SLOTS): validate_slot_range,
        cv.Optional(CONF_VALID_FOR, default="7d"): cv.positive_time_period_seconds,
    },
    key=CONF_SLOTS,
)


def _default_metadata_storage(config):
    if CONF_METADATA_STORAGE not in config:
        config = dict(config)
        config[CONF_METADATA_STORAGE] = (
            "sensor_notepad" if CORE.is_esp8266 else "preferences"
        )
    return config


def _validate(config):
    if CONF_KEY not in config:
        for opt in (CONF_PEERS, CONF_ALLOWED_NETWORKS):
            if config.get(opt):
                raise cv.Invalid(
                    f"{opt} needs a key (without a key the node runs stand-alone)",
                    path=[opt],
                )
    if CONF_ADMIN in config and CONF_GUEST_TOUCHES not in config[CONF_ADMIN]:
        config = dict(config)
        admin = dict(config[CONF_ADMIN])
        taken = {admin[k] for k in (CONF_ENROLL_TOUCHES, CONF_DELETE_TOUCHES, CONF_REENROLL_TOUCHES)}
        admin[CONF_GUEST_TOUCHES] = 5 if CONF_GUESTS in config and 5 not in taken else 0
        config[CONF_ADMIN] = admin
    if guests := config.get(CONF_GUESTS):
        low, high = guests[CONF_SLOTS]
        if CONF_TIME_ID not in config:
            raise cv.Invalid("guests need time_id (expiry dates)", path=[CONF_GUESTS])
        if high - low >= 64:
            raise cv.Invalid("at most 64 guest slots", path=[CONF_GUESTS, CONF_SLOTS])
        admin_slots = config.get(CONF_ADMIN, {}).get(CONF_SLOTS, [])
        if any(low <= a <= high for a in admin_slots):
            raise cv.Invalid("admin slots must not be inside the guest range", path=[CONF_GUESTS])
    if CORE.is_esp8266 and config[CONF_METADATA_STORAGE] == "preferences":
        raise cv.Invalid(
            "ESP8266 has only 512 bytes of flash preferences; use "
            "metadata_storage: sensor_notepad",
            path=[CONF_METADATA_STORAGE],
        )
    return config


def _consume_sockets(config):
    # Only a node with a key syncs over the network: one inbound, one outbound, one listener.
    if CONF_KEY in config:
        config = socket.consume_sockets(2, "fingerprint_grow_plus")(config)
        config = socket.consume_sockets(
            1, "fingerprint_grow_plus", socket.SocketType.TCP_LISTEN
        )(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FingerprintGrowPlusComponent),
            cv.GenerateID(CONF_FINGERPRINT_GROW_ID): cv.use_id(
                fingerprint_grow.FingerprintGrowComponent
            ),
            cv.Optional(CONF_KEY): validate_key,
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
            cv.Optional(CONF_FINGERPRINT_CAPACITY_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_SENSOR_ADDRESS, default=0xFFFFFFFF): cv.hex_uint32_t,
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_ADMIN): ADMIN_SCHEMA,
            cv.Optional(CONF_GUESTS): GUESTS_SCHEMA,
            cv.Optional(CONF_ON_SYNC_STARTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_SYNC_DONE): automation.validate_automation({}),
            cv.Optional(CONF_ON_SYNC_FAILED): automation.validate_automation({}),
            cv.Optional(CONF_ON_CONFLICT): automation.validate_automation({}),
            cv.Optional(CONF_ON_SLOT_UPDATED): automation.validate_automation({}),
            cv.Optional(CONF_ON_SLOT_DELETED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ENROLL_STARTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_TOUCH): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_GESTURE): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_HOLD): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_WAITING): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_CANCELLED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_DONE): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _default_metadata_storage,
    _validate,
    _consume_sockets,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "fingerprint_grow_plus", baud_rate=None, require_tx=True, require_rx=True
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
    automation.CallbackAutomation(
        CONF_ON_ADMIN_GESTURE, "add_on_admin_gesture_callback", [(cg.uint8, "touches")]
    ),
    automation.CallbackAutomation(CONF_ON_ADMIN_HOLD, "add_on_admin_hold_callback"),
    automation.CallbackAutomation(
        CONF_ON_ADMIN_WAITING, "add_on_admin_waiting_callback", [(cg.std_string, "action")]
    ),
    automation.CallbackAutomation(CONF_ON_ADMIN_CANCELLED, "add_on_admin_cancelled_callback"),
    automation.CallbackAutomation(
        CONF_ON_ADMIN_DONE,
        "add_on_admin_done_callback",
        [(cg.std_string, "action"), (cg.uint16, "slot")],
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    grow = await cg.get_variable(config[CONF_FINGERPRINT_GROW_ID])
    cg.add(var.set_fingerprint_grow(grow))

    if CONF_KEY in config:
        key = base64.b64decode(config[CONF_KEY])
        cg.add(
            var.set_key(
                cg.RawExpression(
                    "std::array<uint8_t, 32>{"
                    + ", ".join(f"0x{b:02X}" for b in key)
                    + "}"
                )
            )
        )
    # The transport code is always compiled (defines only); without a key it is never started.
    socket.require_tcp_listener()
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
    if CONF_FINGERPRINT_CAPACITY_SENSOR in config:
        cg.add(
            var.set_fingerprint_capacity_sensor(
                await cg.get_variable(config[CONF_FINGERPRINT_CAPACITY_SENSOR])
            )
        )
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    if admin := config.get(CONF_ADMIN):
        for slot in admin[CONF_SLOTS]:
            cg.add(var.add_admin_slot(slot))
        cg.add(
            var.set_admin_gestures(
                admin[CONF_ENROLL_TOUCHES],
                admin[CONF_DELETE_TOUCHES],
                admin[CONF_REENROLL_TOUCHES],
                admin[CONF_GUEST_TOUCHES],
                admin[CONF_WINDOW],
                admin[CONF_HOLD_TIME],
                admin[CONF_TARGET_TIMEOUT],
                admin[CONF_NUM_SCANS],
            )
        )
    if guests := config.get(CONF_GUESTS):
        low, high = guests[CONF_SLOTS]
        cg.add(var.set_guests(low, high, guests[CONF_VALID_FOR]))
    if networks := config.get(CONF_ALLOWED_NETWORKS):
        socket.add_ipv4_allow(var.set_allowed_networks, networks, config[CONF_ID])

    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)


# ---------------------------------------------------------------------------------------
# Actions and conditions
# ---------------------------------------------------------------------------------------

_PARENT_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent)})

automation.register_apply_action(
    "fingerprint_grow_plus.sync",
    _PARENT_SCHEMA,
    automation.ApplyCall("sync_now()"),
)

automation.register_apply_action(
    "fingerprint_grow_plus.delete",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
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
    "fingerprint_grow_plus.delete_all",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Required(CONF_CODE): cv.templatable(cv.string),
            cv.Required(CONF_CONFIRM_CLUSTER_WIDE): _must_be_true,
        }
    ),
    automation.ApplyField(CONF_CODE, "delete_all", cg.std_string),
)

automation.register_apply_action(
    "fingerprint_grow_plus.enroll_next_free",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Optional(CONF_NUM_SCANS, default=3): cv.templatable(
                cv.int_range(min=1, max=6)
            ),
        },
        key=CONF_NUM_SCANS,
    ),
    automation.ApplyField(CONF_NUM_SCANS, "enroll_next_free", cg.uint8),
)

automation.register_apply_action(
    "fingerprint_grow_plus.resolve_conflict",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
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
    "fingerprint_grow_plus.allow_restore",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Optional(CONF_DURATION, default="5min"): cv.templatable(
                cv.positive_time_period_seconds
            ),
        },
        key=CONF_DURATION,
    ),
    automation.ApplyField(CONF_DURATION, "allow_restore", cg.uint32),
)

automation.register_apply_action(
    "fingerprint_grow_plus.self_test",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
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
    "fingerprint_grow_plus.claim_all_slots",
    _PARENT_SCHEMA,
    automation.ApplyCall("claim_all_slots()"),
)

automation.register_apply_action(
    "fingerprint_grow_plus.resolve_all_conflicts",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Required(CONF_KEEP_LOCAL): cv.templatable(cv.boolean),
        }
    ),
    automation.ApplyField(CONF_KEEP_LOCAL, "resolve_all_conflicts", cg.bool_),
)

automation.register_apply_action(
    "fingerprint_grow_plus.delete_next_finger", _PARENT_SCHEMA, automation.ApplyCall("delete_next_finger()")
)
automation.register_apply_action(
    "fingerprint_grow_plus.reenroll_next_finger",
    _PARENT_SCHEMA,
    automation.ApplyCall("reenroll_next_finger()"),
)
automation.register_apply_action(
    "fingerprint_grow_plus.cancel_admin", _PARENT_SCHEMA, automation.ApplyCall("cancel_admin()")
)
automation.register_apply_action(
    "fingerprint_grow_plus.delete_admin",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
            cv.Required(CONF_CODE): cv.templatable(cv.string),
        }
    ),
    automation.ApplyCall(
        "delete_admin({}, {})", ((CONF_SLOT, cg.uint16), (CONF_CODE, cg.std_string))
    ),
)
automation.register_apply_action(
    "fingerprint_grow_plus.enroll_guest",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Optional(CONF_NUM_SCANS, default=3): cv.templatable(cv.int_range(min=1, max=6)),
            cv.Optional(CONF_VALID_FOR, default="7d"): cv.templatable(
                cv.positive_time_period_seconds
            ),
        }
    ),
    automation.ApplyCall(
        "enroll_guest({}, {})", ((CONF_NUM_SCANS, cg.uint8), (CONF_VALID_FOR, cg.uint32))
    ),
)
automation.register_apply_action(
    "fingerprint_grow_plus.extend_guest",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
            cv.Optional(CONF_VALID_FOR, default="7d"): cv.templatable(
                cv.positive_time_period_seconds
            ),
        }
    ),
    automation.ApplyCall(
        "extend_guest({}, {})", ((CONF_SLOT, cg.uint16), (CONF_VALID_FOR, cg.uint32))
    ),
)

automation.register_apply_condition(
    "fingerprint_grow_plus.is_admin_busy", _PARENT_SCHEMA, "is_admin_busy()"
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_admin_slot",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(FingerprintGrowPlusComponent),
            cv.Required(CONF_SLOT): cv.templatable(cv.uint16_t),
        },
        key=CONF_SLOT,
    ),
    automation.ApplyCall("is_admin_slot({})", ((CONF_SLOT, cg.uint16),)),
)

automation.register_apply_condition(
    "fingerprint_grow_plus.is_syncing", _PARENT_SCHEMA, "is_syncing()"
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_ready", _PARENT_SCHEMA, "is_ready()"
)
