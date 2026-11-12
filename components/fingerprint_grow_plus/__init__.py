"""fingerprint_grow_plus: admin fingers, guest fingers and optional sync for fingerprint_grow."""

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

CONF_FINGERPRINT_GROW_PLUS_ID = "fingerprint_grow_plus_id"
CONF_FINGERPRINT_GROW_ID = "fingerprint_grow_id"
CONF_HOST = "host"
CONF_SLOT = "slot"
CONF_SLOTS = "slots"
CONF_CODE = "code"
CONF_KEEP_LOCAL = "keep_local"

# general
CONF_ENROLL_SLOTS = "enroll_slots"
CONF_STARTUP_DELAY = "startup_delay"
CONF_METADATA_STORAGE = "metadata_storage"
CONF_DELETE_ALL_CODE = "delete_all_code"
CONF_FINGER_SENSOR = "finger_sensor"
CONF_FINGERPRINT_COUNT_SENSOR = "fingerprint_count_sensor"
CONF_FINGERPRINT_CAPACITY_SENSOR = "fingerprint_capacity_sensor"
CONF_ON_SLOT_UPDATED = "on_slot_updated"
CONF_ON_SLOT_DELETED = "on_slot_deleted"
CONF_ON_ENROLL_STARTED = "on_enroll_started"

# sync:
CONF_SYNC = "sync"
CONF_PEERS = "peers"
CONF_ALLOWED_NETWORKS = "allowed_networks"
CONF_INCLUDE_SLOTS = "include_slots"
CONF_EXCLUDE_SLOTS = "exclude_slots"
CONF_INTERVAL = "interval"
CONF_DRY_RUN = "dry_run"
CONF_AT_BOOT = "at_boot"
CONF_AFTER_CHANGE = "after_change"
CONF_ON_SYNC_STARTED = "on_started"
CONF_ON_SYNC_DONE = "on_done"
CONF_ON_SYNC_FAILED = "on_failed"
CONF_ON_CONFLICT = "on_conflict"

# admin:
CONF_ADMIN = "admin"
CONF_ENROLL_TOUCHES = "enroll_touches"
CONF_DELETE_TOUCHES = "delete_touches"
CONF_REENROLL_TOUCHES = "reenroll_touches"
CONF_GUEST_TOUCHES = "guest_touches"
CONF_WINDOW = "window"
CONF_HOLD_TIME = "hold_time"
CONF_TARGET_TIMEOUT = "target_timeout"
CONF_ACCESS = "access"
ADMIN_ACCESS = {"off": 0, "immediate": 1, "delayed": 2}
CONF_ON_ADMIN_TOUCH = "on_touch"
CONF_ON_ADMIN_ACCESS = "on_access"
CONF_ON_ADMIN_GESTURE = "on_gesture"
CONF_ON_ADMIN_HOLD = "on_hold"
CONF_ON_ADMIN_WAITING = "on_waiting"
CONF_ON_ADMIN_CANCELLED = "on_cancelled"
CONF_ON_ADMIN_DONE = "on_done"

# led_feedback:
CONF_LED_FEEDBACK = "led_feedback"
CONF_ON_IDLE = "on_idle"

# lockout: / duress:
CONF_LOCKOUT = "lockout"
CONF_MAX_ATTEMPTS = "max_attempts"
CONF_RESET_ON_MATCH = "reset_on_match"
CONF_SHARE_WITH = "share_with"
CONF_ON_LOCKED = "on_locked"
CONF_ON_UNLOCKED = "on_unlocked"
CONF_DURESS = "duress"
CONF_ON_DURESS = "on_duress"

# schedule:
CONF_SCHEDULE = "schedule"
CONF_WITHOUT_TIME = "without_time"
CONF_ON_DENIED = "on_denied"
CONF_RANGES = "ranges"
CONF_TIMES = "times"
CONF_DAYS = "days"
CONF_FROM = "from"
CONF_TO = "to"
WEEKDAYS = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]

# guests:
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
            "e.g. `python3 tools/growplus.py genkey`)"
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


PEER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_HOST): socket.ipv4_host,
        cv.Optional(CONF_PORT): cv.port,
    }
)


def validate_peer(value):
    # "host" or "host:port"
    if isinstance(value, str):
        host, _, port = value.partition(":")
        value = {CONF_HOST: host} if not port else {CONF_HOST: host, CONF_PORT: port}
    return PEER_SCHEMA(value)


def _empty_dict_if_none(value):
    # `admin:` without options enables the feature with all defaults.
    return {} if value is None or value is True else value


# ---------------------------------------------------------------------------------------
# sync:
# ---------------------------------------------------------------------------------------

SYNC_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_KEY): validate_key,
        # All nodes of the cluster; a node skips itself (name or name.local). Without peers the
        # node only accepts the backup tool (backup access for a single node).
        cv.Optional(CONF_PEERS, default=[]): cv.ensure_list(validate_peer),
        cv.Optional(CONF_PORT, default=DEFAULT_PORT): cv.port,
        cv.Optional(CONF_ALLOWED_NETWORKS): socket.IPV4_ALLOW_SCHEMA,
        # Synced slots (default: all) minus excluded ones. Local features use all slots.
        cv.Optional(CONF_INCLUDE_SLOTS, default=[]): cv.ensure_list(validate_slot_range),
        cv.Optional(CONF_EXCLUDE_SLOTS, default=[]): cv.ensure_list(validate_slot_range),
        # Periodic full comparison with all peers, or "never" (manual sync only).
        cv.Optional(CONF_INTERVAL, default="15min"): cv.Any(
            cv.one_of("never", lower=True), cv.positive_time_period_milliseconds
        ),
        # Sync shortly after boot / ~2 s after a local change (enroll, delete, restore, ...).
        cv.Optional(CONF_AT_BOOT, default=True): cv.boolean,
        cv.Optional(CONF_AFTER_CHANGE, default=True): cv.boolean,
        cv.Optional(CONF_DRY_RUN, default=False): cv.boolean,
        cv.Optional(CONF_ON_SYNC_STARTED): automation.validate_automation({}),
        cv.Optional(CONF_ON_SYNC_DONE): automation.validate_automation({}),
        cv.Optional(CONF_ON_SYNC_FAILED): automation.validate_automation({}),
        cv.Optional(CONF_ON_CONFLICT): automation.validate_automation({}),
    }
)

# ---------------------------------------------------------------------------------------
# admin:
# ---------------------------------------------------------------------------------------

_TOUCHES = cv.int_range(min=0, max=10)


def _validate_admin(config):
    config = dict(config)
    config[CONF_SLOTS] = sorted(set(config[CONF_SLOTS]))
    used = [
        config.get(k, 0)
        for k in (CONF_ENROLL_TOUCHES, CONF_DELETE_TOUCHES, CONF_REENROLL_TOUCHES, CONF_GUEST_TOUCHES)
        if config.get(k, 0) > 0
    ]
    if len(used) != len(set(used)):
        raise cv.Invalid("every admin gesture needs its own number of touches (0 = disabled)")
    if config[CONF_ACCESS] != "off" and 1 in used:
        raise cv.Invalid("with access, a single touch is the access action; no gesture may use 1 touch")
    return config


ADMIN_SCHEMA = cv.All(
    _empty_dict_if_none,
    cv.Schema(
        {
            cv.Optional(CONF_SLOTS, default=[0]): cv.All(
                cv.ensure_list(cv.uint16_t), cv.Length(min=1, max=10)
            ),
            cv.Optional(CONF_ENROLL_TOUCHES, default=2): _TOUCHES,
            cv.Optional(CONF_DELETE_TOUCHES, default=3): _TOUCHES,
            cv.Optional(CONF_REENROLL_TOUCHES, default=4): _TOUCHES,
            # Default: 5 when guests are configured, otherwise off.
            cv.Optional(CONF_GUEST_TOUCHES): _TOUCHES,
            # No finger on the sensor for this long completes a gesture.
            cv.Optional(CONF_WINDOW, default="2s"): cv.positive_time_period_milliseconds,
            # Keep the admin finger on the sensor this long for on_hold (needs finger_sensor).
            cv.Optional(CONF_HOLD_TIME, default="3s"): cv.positive_time_period_milliseconds,
            # Time to present the finger to delete / re-enroll.
            cv.Optional(CONF_TARGET_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_NUM_SCANS, default=3): cv.int_range(min=1, max=6),
            # A single touch fires on_access: at once (immediate) or once the gesture is
            # complete and it stayed a single touch (delayed).
            cv.Optional(CONF_ACCESS, default="off"): cv.one_of(*ADMIN_ACCESS, lower=True),
            cv.Optional(CONF_ON_ADMIN_TOUCH): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_ACCESS): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_GESTURE): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_HOLD): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_WAITING): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_CANCELLED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ADMIN_DONE): automation.validate_automation({}),
        }
    ),
    _validate_admin,
)

# ---------------------------------------------------------------------------------------
# led_feedback:
# ---------------------------------------------------------------------------------------

LED_FEEDBACK_SCHEMA = cv.All(
    _empty_dict_if_none,
    cv.Schema(
        {
            # Called when a feedback ends; without it the LED is switched off.
            cv.Optional(CONF_ON_IDLE): automation.validate_automation({}),
        }
    ),
)

# ---------------------------------------------------------------------------------------
# lockout: / duress:
# ---------------------------------------------------------------------------------------

LOCKOUT_SCHEMA = cv.All(
    _empty_dict_if_none,
    cv.Schema(
        {
            # Unrecognised fingers in a row that lock this node.
            cv.Optional(CONF_MAX_ATTEMPTS, default=5): cv.int_range(min=1, max=100),
            cv.Optional(CONF_DURATION, default="15min"): cv.positive_time_period_milliseconds,
            # A recognised finger resets the counter.
            cv.Optional(CONF_RESET_ON_MATCH, default=True): cv.boolean,
            # Lock these peers too: host (or host:port) as written in sync: peers.
            cv.Optional(CONF_SHARE_WITH, default=[]): cv.ensure_list(validate_peer),
            cv.Optional(CONF_ON_LOCKED): automation.validate_automation({}),
            cv.Optional(CONF_ON_UNLOCKED): automation.validate_automation({}),
        }
    ),
)

DURESS_SCHEMA = cv.maybe_simple_value(
    {
        cv.Required(CONF_SLOTS): validate_slot_range,
        cv.Optional(CONF_ON_DURESS): automation.validate_automation({}),
    },
    key=CONF_SLOTS,
)

# ---------------------------------------------------------------------------------------
# schedule:
# ---------------------------------------------------------------------------------------


def validate_days(value):
    """[mon, wed], "mon-fri", "daily" or "weekend" -> bit mask (bit 0 = Sunday)."""
    if isinstance(value, str):
        value = value.strip().lower()
        if value == "daily":
            return 0x7F
        if value == "weekend":
            return 0x41
        if "-" in value:
            first, _, last = value.partition("-")
            if first not in WEEKDAYS or last not in WEEKDAYS:
                raise cv.Invalid(f"'{value}': use day names like mon-fri")
            i, j = WEEKDAYS.index(first), WEEKDAYS.index(last)
            mask = 0
            while True:
                mask |= 1 << i
                if i == j:
                    return mask
                i = (i + 1) % 7
        value = [value]
    mask = 0
    for day in cv.ensure_list(cv.string_strict)(value):
        day = day.strip().lower()[:3]
        if day not in WEEKDAYS:
            raise cv.Invalid(f"'{day}' is not a day (mon, tue, wed, thu, fri, sat, sun)")
        mask |= 1 << WEEKDAYS.index(day)
    return mask


def validate_clock(value):
    """"HH:MM" -> minutes since midnight ("24:00" allowed as end of day)."""
    match = re.match(r"^\s*(\d{1,2}):(\d{2})\s*$", cv.string_strict(value))
    if not match:
        raise cv.Invalid(f"'{value}' is not a time like 08:30")
    hours, minutes = int(match.group(1)), int(match.group(2))
    if minutes > 59 or hours > 24 or (hours == 24 and minutes):
        raise cv.Invalid(f"'{value}' is not a valid time")
    return hours * 60 + minutes


def _validate_window(config):
    if config[CONF_FROM] == config[CONF_TO]:
        raise cv.Invalid("from and to must differ (use 00:00 to 24:00 for the whole day)")
    return config


SCHEDULE_SCHEMA = cv.Schema(
    {
        # Slots with a time window while the time is not known yet: deny (default) or allow.
        cv.Optional(CONF_WITHOUT_TIME, default="deny"): cv.one_of("deny", "allow", lower=True),
        cv.Optional(CONF_ON_DENIED): automation.validate_automation({}),
        cv.Required(CONF_RANGES): cv.ensure_list(
            cv.Schema(
                {
                    cv.Required(CONF_SLOTS): validate_slot_range,
                    cv.Required(CONF_TIMES): cv.All(
                        cv.ensure_list(
                            cv.All(
                                cv.Schema(
                                    {
                                        cv.Optional(CONF_DAYS, default="daily"): validate_days,
                                        cv.Required(CONF_FROM): validate_clock,
                                        cv.Required(CONF_TO): validate_clock,
                                    }
                                ),
                                _validate_window,
                            )
                        ),
                        cv.Length(min=1),
                    ),
                }
            )
        ),
    }
)

# ---------------------------------------------------------------------------------------
# guests:
# ---------------------------------------------------------------------------------------

GUESTS_SCHEMA = cv.maybe_simple_value(
    {
        cv.Required(CONF_SLOTS): validate_slot_range,
        cv.Optional(CONF_VALID_FOR, default="7d"): cv.positive_time_period_seconds,
    },
    key=CONF_SLOTS,
)

# ---------------------------------------------------------------------------------------
# Component
# ---------------------------------------------------------------------------------------


def _default_metadata_storage(config):
    if CONF_METADATA_STORAGE not in config:
        config = dict(config)
        config[CONF_METADATA_STORAGE] = (
            "sensor_notepad" if CORE.is_esp8266 else "preferences"
        )
    return config


def _validate(config):
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
    if lockout := config.get(CONF_LOCKOUT):
        sync = config.get(CONF_SYNC, {})
        default_port = sync.get(CONF_PORT, DEFAULT_PORT)
        peers = {(p[CONF_HOST], p.get(CONF_PORT, default_port)) for p in sync.get(CONF_PEERS, [])}
        for target in lockout[CONF_SHARE_WITH]:
            host, port = target[CONF_HOST], target.get(CONF_PORT)
            if not any(h == host and (port is None or p == port) for h, p in peers):
                raise cv.Invalid(
                    f"share_with: '{host}' is not in sync: peers (write it as there)",
                    path=[CONF_LOCKOUT, CONF_SHARE_WITH],
                )
    if schedule := config.get(CONF_SCHEDULE):
        if CONF_TIME_ID not in config:
            raise cv.Invalid("schedule needs time_id", path=[CONF_SCHEDULE])
        admin_slots = config.get(CONF_ADMIN, {}).get(CONF_SLOTS, [])
        for rng in schedule[CONF_RANGES]:
            low, high = rng[CONF_SLOTS]
            if any(low <= a <= high for a in admin_slots):
                raise cv.Invalid(
                    "admin slots must not be inside a schedule range", path=[CONF_SCHEDULE]
                )
    if duress := config.get(CONF_DURESS):
        low, high = duress[CONF_SLOTS]
        admin_slots = config.get(CONF_ADMIN, {}).get(CONF_SLOTS, [])
        if any(low <= a <= high for a in admin_slots):
            raise cv.Invalid("admin slots must not be inside the duress range", path=[CONF_DURESS])
    if CORE.is_esp8266 and config[CONF_METADATA_STORAGE] == "preferences":
        raise cv.Invalid(
            "ESP8266 has only 512 bytes of flash preferences; use "
            "metadata_storage: sensor_notepad",
            path=[CONF_METADATA_STORAGE],
        )
    return config


def _consume_sockets(config):
    # Only a node with sync: talks to the network: one inbound, one outbound, one listener.
    if CONF_SYNC in config:
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
            cv.Optional(CONF_FINGER_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_ENROLL_SLOTS): validate_slot_range,
            cv.Optional(CONF_DELETE_ALL_CODE): cv.All(cv.string_strict, cv.Length(min=4)),
            cv.Optional(CONF_FINGERPRINT_COUNT_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_FINGERPRINT_CAPACITY_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_STARTUP_DELAY, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_METADATA_STORAGE): cv.enum(METADATA_STORAGE, lower=True),
            cv.Optional(CONF_ADMIN): ADMIN_SCHEMA,
            cv.Optional(CONF_GUESTS): GUESTS_SCHEMA,
            cv.Optional(CONF_LED_FEEDBACK): LED_FEEDBACK_SCHEMA,
            cv.Optional(CONF_LOCKOUT): LOCKOUT_SCHEMA,
            cv.Optional(CONF_DURESS): DURESS_SCHEMA,
            cv.Optional(CONF_SCHEDULE): SCHEDULE_SCHEMA,
            cv.Optional(CONF_SYNC): SYNC_SCHEMA,
            cv.Optional(CONF_ON_SLOT_UPDATED): automation.validate_automation({}),
            cv.Optional(CONF_ON_SLOT_DELETED): automation.validate_automation({}),
            cv.Optional(CONF_ON_ENROLL_STARTED): automation.validate_automation({}),
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

_AUTOMATIONS = (
    automation.CallbackAutomation(
        CONF_ON_SLOT_UPDATED, "add_on_slot_updated_callback", [(cg.uint16, "slot")]
    ),
    automation.CallbackAutomation(
        CONF_ON_SLOT_DELETED, "add_on_slot_deleted_callback", [(cg.uint16, "slot")]
    ),
    automation.CallbackAutomation(
        CONF_ON_ENROLL_STARTED, "add_on_enroll_started_callback", [(cg.uint16, "slot")]
    ),
)

_SYNC_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_SYNC_STARTED, "add_on_sync_started_callback"),
    automation.CallbackAutomation(CONF_ON_SYNC_DONE, "add_on_sync_done_callback"),
    automation.CallbackAutomation(
        CONF_ON_SYNC_FAILED, "add_on_sync_failed_callback", [(cg.std_string, "reason")]
    ),
    automation.CallbackAutomation(
        CONF_ON_CONFLICT, "add_on_conflict_callback", [(cg.uint16, "slot")]
    ),
)

_ADMIN_AUTOMATIONS = (
    automation.CallbackAutomation(
        CONF_ON_ADMIN_TOUCH, "add_on_admin_touch_callback", [(cg.uint8, "touches")]
    ),
    automation.CallbackAutomation(
        CONF_ON_ADMIN_ACCESS,
        "add_on_admin_access_callback",
        [(cg.uint16, "finger_id"), (cg.uint16, "confidence")],
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

    cg.add(var.set_fingerprint_grow(await cg.get_variable(config[CONF_FINGERPRINT_GROW_ID])))
    cg.add(var.set_startup_delay(config[CONF_STARTUP_DELAY]))
    cg.add(var.set_meta_backend(METADATA_STORAGE[str(config[CONF_METADATA_STORAGE])]))
    if CONF_ENROLL_SLOTS in config:
        low, high = config[CONF_ENROLL_SLOTS]
        cg.add(var.set_enroll_range(low, high))
    if CONF_DELETE_ALL_CODE in config:
        cg.add(var.set_delete_all_code(config[CONF_DELETE_ALL_CODE]))
    if CONF_FINGER_SENSOR in config:
        cg.add(var.set_finger_sensor(await cg.get_variable(config[CONF_FINGER_SENSOR])))
    if CONF_FINGERPRINT_COUNT_SENSOR in config:
        sens = await cg.get_variable(config[CONF_FINGERPRINT_COUNT_SENSOR])
        cg.add(var.set_fingerprint_count_sensor(sens))
    if CONF_FINGERPRINT_CAPACITY_SENSOR in config:
        sens = await cg.get_variable(config[CONF_FINGERPRINT_CAPACITY_SENSOR])
        cg.add(var.set_fingerprint_capacity_sensor(sens))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    await automation.build_callback_automations(var, config, _AUTOMATIONS)

    # The transport code is always compiled (defines only); without sync: it is never started.
    socket.require_tcp_listener()
    if sync := config.get(CONF_SYNC):
        key = base64.b64decode(sync[CONF_KEY])
        cg.add(
            var.set_key(
                cg.RawExpression(
                    "std::array<uint8_t, 32>{" + ", ".join(f"0x{b:02X}" for b in key) + "}"
                )
            )
        )
        cg.add(var.set_port(sync[CONF_PORT]))
        for peer in sync[CONF_PEERS]:
            cg.add(var.add_peer(peer[CONF_HOST], peer.get(CONF_PORT, sync[CONF_PORT])))
        if networks := sync.get(CONF_ALLOWED_NETWORKS):
            socket.add_ipv4_allow(var.set_allowed_networks, networks, config[CONF_ID])
        for low, high in sync[CONF_INCLUDE_SLOTS]:
            cg.add(var.add_include_range(low, high))
        for low, high in sync[CONF_EXCLUDE_SLOTS]:
            cg.add(var.add_exclude_range(low, high))
        interval = sync[CONF_INTERVAL]
        cg.add(var.set_sync_interval(0 if interval == "never" else interval))
        cg.add(var.set_sync_at_boot(sync[CONF_AT_BOOT]))
        cg.add(var.set_sync_after_change(sync[CONF_AFTER_CHANGE]))
        cg.add(var.set_dry_run(sync[CONF_DRY_RUN]))
        await automation.build_callback_automations(var, sync, _SYNC_AUTOMATIONS)

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
        cg.add(var.set_admin_access(ADMIN_ACCESS[admin[CONF_ACCESS]]))
        await automation.build_callback_automations(var, admin, _ADMIN_AUTOMATIONS)

    if guests := config.get(CONF_GUESTS):
        low, high = guests[CONF_SLOTS]
        cg.add(var.set_guests(low, high, guests[CONF_VALID_FOR]))

    if lockout := config.get(CONF_LOCKOUT):
        cg.add(
            var.set_lockout(
                lockout[CONF_MAX_ATTEMPTS], lockout[CONF_DURATION], lockout[CONF_RESET_ON_MATCH]
            )
        )
        for target in lockout[CONF_SHARE_WITH]:
            cg.add(var.add_lock_target(target[CONF_HOST], target.get(CONF_PORT, 0)))
        await automation.build_callback_automations(
            var,
            lockout,
            (
                automation.CallbackAutomation(
                    CONF_ON_LOCKED, "add_on_locked_callback", [(cg.std_string, "source")]
                ),
                automation.CallbackAutomation(CONF_ON_UNLOCKED, "add_on_unlocked_callback"),
            ),
        )

    if duress := config.get(CONF_DURESS):
        low, high = duress[CONF_SLOTS]
        cg.add(var.set_duress(low, high))
        await automation.build_callback_automations(
            var,
            duress,
            (
                automation.CallbackAutomation(
                    CONF_ON_DURESS,
                    "add_on_duress_callback",
                    [(cg.uint16, "finger_id"), (cg.uint16, "confidence")],
                ),
            ),
        )

    if schedule := config.get(CONF_SCHEDULE):
        cg.add(var.set_schedule_allow_without_time(schedule[CONF_WITHOUT_TIME] == "allow"))
        for rng in schedule[CONF_RANGES]:
            low, high = rng[CONF_SLOTS]
            cg.add(var.add_schedule(low, high))
            for win in rng[CONF_TIMES]:
                cg.add(var.add_schedule_window(win[CONF_DAYS], win[CONF_FROM], win[CONF_TO]))
        await automation.build_callback_automations(
            var,
            schedule,
            (
                automation.CallbackAutomation(
                    CONF_ON_DENIED, "add_on_schedule_denied_callback", [(cg.uint16, "finger_id")]
                ),
            ),
        )

    if CONF_LED_FEEDBACK in config:
        led = config[CONF_LED_FEEDBACK]
        cg.add(var.set_led_feedback(CONF_ON_IDLE in led))
        await automation.build_callback_automations(
            var, led, (automation.CallbackAutomation(CONF_ON_IDLE, "add_on_led_idle_callback"),)
        )


# ---------------------------------------------------------------------------------------
# Actions and conditions
# ---------------------------------------------------------------------------------------

_PARENT = cv.GenerateID()
_PARENT_ID = cv.use_id(FingerprintGrowPlusComponent)
_PARENT_SCHEMA = cv.Schema({_PARENT: _PARENT_ID})
_NUM_SCANS = cv.templatable(cv.int_range(min=1, max=6))
_SLOT = cv.templatable(cv.uint16_t)
_VALID_FOR = cv.templatable(cv.positive_time_period_seconds)


def _action(name, schema, apply):
    automation.register_apply_action(f"fingerprint_grow_plus.{name}", schema, apply)


def _simple(name, call):
    _action(name, _PARENT_SCHEMA, automation.ApplyCall(call))


# --- general ---------------------------------------------------------------------------

_action(
    "enroll_next_free",
    cv.maybe_simple_value(
        {_PARENT: _PARENT_ID, cv.Optional(CONF_NUM_SCANS, default=3): _NUM_SCANS},
        key=CONF_NUM_SCANS,
    ),
    automation.ApplyField(CONF_NUM_SCANS, "enroll_next_free", cg.uint8),
)
_action(
    "delete",
    cv.maybe_simple_value({_PARENT: _PARENT_ID, cv.Required(CONF_SLOT): _SLOT}, key=CONF_SLOT),
    automation.ApplyField(CONF_SLOT, "delete_slot", cg.uint16),
)
_action(
    "delete_all",
    cv.maybe_simple_value(
        {_PARENT: _PARENT_ID, cv.Required(CONF_CODE): cv.templatable(cv.string)},
        key=CONF_CODE,
    ),
    automation.ApplyField(CONF_CODE, "delete_all", cg.std_string),
)

# --- admin -----------------------------------------------------------------------------

_simple("delete_next_finger", "delete_next_finger()")
_simple("reenroll_next_finger", "reenroll_next_finger()")
_simple("cancel_admin", "cancel_admin()")
_action(
    "delete_admin",
    cv.Schema(
        {
            _PARENT: _PARENT_ID,
            cv.Required(CONF_SLOT): _SLOT,
            cv.Required(CONF_CODE): cv.templatable(cv.string),
        }
    ),
    automation.ApplyCall(
        "delete_admin({}, {})", ((CONF_SLOT, cg.uint16), (CONF_CODE, cg.std_string))
    ),
)

# --- guests ----------------------------------------------------------------------------

_action(
    "enroll_guest",
    cv.Schema(
        {
            _PARENT: _PARENT_ID,
            cv.Optional(CONF_NUM_SCANS, default=3): _NUM_SCANS,
            cv.Optional(CONF_VALID_FOR, default="7d"): _VALID_FOR,
        }
    ),
    automation.ApplyCall(
        "enroll_guest({}, {})", ((CONF_NUM_SCANS, cg.uint8), (CONF_VALID_FOR, cg.uint32))
    ),
)
_action(
    "extend_guest",
    cv.Schema(
        {
            _PARENT: _PARENT_ID,
            cv.Required(CONF_SLOT): _SLOT,
            cv.Optional(CONF_VALID_FOR, default="7d"): _VALID_FOR,
        }
    ),
    automation.ApplyCall(
        "extend_guest({}, {})", ((CONF_SLOT, cg.uint16), (CONF_VALID_FOR, cg.uint32))
    ),
)

# --- lockout ---------------------------------------------------------------------------

_simple("register_failed_attempt", "register_failed_attempt()")
_simple("reset_lockout", "reset_lockout()")

# --- sync ------------------------------------------------------------------------------

_simple("sync", "sync_now()")
_simple("claim_all_slots", "claim_all_slots()")
_action(
    "resolve_conflict",
    cv.Schema(
        {
            _PARENT: _PARENT_ID,
            cv.Required(CONF_SLOT): _SLOT,
            cv.Required(CONF_KEEP_LOCAL): cv.templatable(cv.boolean),
        }
    ),
    automation.ApplyCall(
        "resolve_conflict({}, {})", ((CONF_SLOT, cg.uint16), (CONF_KEEP_LOCAL, cg.bool_))
    ),
)
_action(
    "resolve_all_conflicts",
    cv.Schema({_PARENT: _PARENT_ID, cv.Required(CONF_KEEP_LOCAL): cv.templatable(cv.boolean)}),
    automation.ApplyField(CONF_KEEP_LOCAL, "resolve_all_conflicts", cg.bool_),
)
_action(
    "allow_restore",
    cv.maybe_simple_value(
        {
            _PARENT: _PARENT_ID,
            cv.Optional(CONF_DURATION, default="5min"): cv.templatable(
                cv.positive_time_period_seconds
            ),
        },
        key=CONF_DURATION,
    ),
    automation.ApplyField(CONF_DURATION, "allow_restore", cg.uint32),
)
_action(
    "allow_full_backup",
    cv.maybe_simple_value(
        {
            _PARENT: _PARENT_ID,
            cv.Optional(CONF_DURATION, default="5min"): cv.templatable(
                cv.positive_time_period_seconds
            ),
        },
        key=CONF_DURATION,
    ),
    automation.ApplyField(CONF_DURATION, "allow_full_backup", cg.uint32),
)

# --- conditions ------------------------------------------------------------------------

automation.register_apply_condition(
    "fingerprint_grow_plus.is_ready", _PARENT_SCHEMA, "is_ready()"
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_syncing", _PARENT_SCHEMA, "is_syncing()"
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_admin_busy", _PARENT_SCHEMA, "is_admin_busy()"
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_locked", _PARENT_SCHEMA, "is_locked()"
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_allowed",
    cv.maybe_simple_value({_PARENT: _PARENT_ID, cv.Required(CONF_SLOT): _SLOT}, key=CONF_SLOT),
    automation.ApplyCall("is_allowed({})", ((CONF_SLOT, cg.uint16),)),
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_duress_slot",
    cv.maybe_simple_value({_PARENT: _PARENT_ID, cv.Required(CONF_SLOT): _SLOT}, key=CONF_SLOT),
    automation.ApplyCall("is_duress_slot({})", ((CONF_SLOT, cg.uint16),)),
)
automation.register_apply_condition(
    "fingerprint_grow_plus.is_admin_slot",
    cv.maybe_simple_value({_PARENT: _PARENT_ID, cv.Required(CONF_SLOT): _SLOT}, key=CONF_SLOT),
    automation.ApplyCall("is_admin_slot({})", ((CONF_SLOT, cg.uint16),)),
)
