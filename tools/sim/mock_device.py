#!/usr/bin/env python3
"""Mock ESP32 Power Outage Monitor device (stdlib only) for L2/L3 verification and LAN phone tests."""

import argparse
import base64
import copy
import email.policy
from email.parser import BytesParser
import http.server
import json
import os
import re
import socket
import socketserver
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(THIS_DIR, ".."))
REPO_ROOT = os.path.abspath(os.path.join(THIS_DIR, "..", ".."))
WEB_UI_PATH = os.path.join(REPO_ROOT, "firmware", "src", "web_ui.h")

if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import mobile_alarm_sim  # noqa: E402

FW_VERSION = "1.0.0-mock"
WIFI_SLOTS = 5
CONFIG_BODY_MAX_BYTES = 2048
SNOOZE_MIN_MINUTES = 1
SNOOZE_MAX_MINUTES = 1440

DEFAULT_AP_SSID_PREFIX = "PowerMonitor-"
DEFAULT_AP_SSID = "PowerMonitor-A1B2"
DEFAULT_AP_PASS = "12345678"
DEFAULT_DEVICE_NAME = mobile_alarm_sim.DEFAULT_DEVICE_NAME
DEFAULT_DEBOUNCE_MS = 500
DEFAULT_COOLDOWN_MS = 5000
DEFAULT_SNOOZE_MIN = 30
DEFAULT_SETUP_WINDOW_S = 60
DEFAULT_ALERT_MODE = 0
DEFAULT_CLOUD_ENABLED = False

CONFIG_FIELD_BOUNDS = {
    "setup_window_s": (0, 600),
    "debounce_ms": (50, 10000),
    "restore_cooldown_ms": (0, 60000),
    "snooze_default_min": (SNOOZE_MIN_MINUTES, SNOOZE_MAX_MINUTES),
    "alert_mode": (0, 1),
}

CONFIG_STRING_LIMITS = {
    "ssid": (0, 32),
    "ap_ssid": (1, 32),
    "pass": (8, 63),
    "device_name": (1, 32),
    "url": (0, 256),
}

KNOWN_CONFIG_KEYS = {
    "wifi",
    "device_name",
    "ap_ssid",
    "ap_pass",
    "setup_window_s",
    "admin_pass",
    "clear_admin_pass",
    "debounce_ms",
    "restore_cooldown_ms",
    "snooze_default_min",
    "alert_mode",
    "cloud_enabled",
    "webhook_url",
    "local_url",
}

# Single authoritative route table: (method, path, guard, handler_method_name)
ROUTE_TABLE = (
    ("GET", "/", "open", "_handle_root"),
    ("GET", "/api/status", "open", "_handle_get_status"),
    ("POST", "/api/arm", "open", "_handle_arm"),
    ("POST", "/api/disarm", "open", "_handle_disarm"),
    ("POST", "/api/toggle", "open", "_handle_toggle"),
    ("POST", "/api/snooze", "open", "_handle_snooze"),
    ("POST", "/api/test", "open", "_handle_test"),
    ("GET", "/api/config", "admin", "_handle_get_config"),
    ("POST", "/api/config", "admin", "_handle_post_config"),
    ("GET", "/api/scan", "admin", "_handle_scan"),
    ("POST", "/api/setup-ap", "admin", "_handle_setup_ap"),
    ("POST", "/api/setup-ap/close", "admin", "_handle_setup_ap_close"),
    ("POST", "/api/reboot", "admin", "_handle_reboot"),
    ("POST", "/api/factory-reset", "admin", "_handle_factory_reset"),
    ("POST", "/api/update", "admin", "_handle_update"),
)

_ROUTE_MAP = {(m, p): (g, h) for m, p, g, h in ROUTE_TABLE}

DEVICE_NAME_RE = re.compile(r"^[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?$")
VALID_GRID_STATES = ("BOOTING", "ONLINE", "OFFLINE")
VALID_MODES = ("ARMED", "DISARMED", "SNOOZED")

_STATE_ALLOWED_KEYS = {
    "config",
    "mode",
    "snooze_remaining_s",
    "grid",
    "outage_duration_s",
    "wifi",
    "setup_ap",
    "scan_networks",
    "scan_pending_polls",
    "scan_hold",
    "force_error",
    "clear_requests",
    "update_in_progress",
    "ntp_synced",
    "notify",
}


def extract_web_ui_html(path=WEB_UI_PATH):
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    m = re.search(r'R"rawliteral\((.*)\)rawliteral"', raw, re.DOTALL)
    if not m:
        raise RuntimeError(f'Could not extract R"rawliteral(...)rawliteral" from {path}')
    return m.group(1)


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _parse_strtol_int(raw):
    """Parse decimal integer with C strtol(trimmed, &endp, 10) semantics."""
    if not isinstance(raw, str):
        return None
    s = raw.strip()
    if not re.fullmatch(r"[+-]?[0-9]+", s):
        return None
    return int(s, 10)


def default_config():
    return {
        "wifi": [{"slot": i, "ssid": "", "pass": ""} for i in range(WIFI_SLOTS)],
        "device_name": DEFAULT_DEVICE_NAME,
        "ap_ssid": DEFAULT_AP_SSID,
        "ap_pass": DEFAULT_AP_PASS,
        "setup_window_s": DEFAULT_SETUP_WINDOW_S,
        "admin_pass": "",
        "debounce_ms": DEFAULT_DEBOUNCE_MS,
        "restore_cooldown_ms": DEFAULT_COOLDOWN_MS,
        "snooze_default_min": DEFAULT_SNOOZE_MIN,
        "alert_mode": DEFAULT_ALERT_MODE,
        "cloud_enabled": DEFAULT_CLOUD_ENABLED,
        "webhook_url": "",
        "local_url": "",
    }


def _validate_url_field(field_name, u):
    _, max_url = CONFIG_STRING_LIMITS["url"]
    if not isinstance(u, str):
        return False, field_name, "must be a string"
    ulen = len(u.encode("utf-8"))
    if ulen > max_url:
        return False, field_name, f"must be <= {max_url} bytes"
    if ulen > 0 and not (u.startswith("http://") or u.startswith("https://")):
        return False, field_name, "must be empty or start with http:// or https://"
    return True, "", ""


def validate_and_merge_config(cur, obj):
    """Validate and merge a partial config update atomically."""
    if not isinstance(obj, dict):
        return False, None, "body", "invalid JSON object", []

    for key in obj.keys():
        if key not in KNOWN_CONFIG_KEYS:
            return False, None, key, "unknown field", []

    out = copy.deepcopy(cur)
    applied = []

    _, max_ssid = CONFIG_STRING_LIMITS["ssid"]
    min_ap_ssid, max_ap_ssid = CONFIG_STRING_LIMITS["ap_ssid"]
    min_pass, max_pass = CONFIG_STRING_LIMITS["pass"]

    if "wifi" in obj:
        arr = obj["wifi"]
        if not isinstance(arr, list):
            return False, None, "wifi", "must be an array", []
        for idx, entry in enumerate(arr):
            if not isinstance(entry, dict):
                return False, None, "wifi", "each entry must be an object", []
            for ek in entry.keys():
                if ek not in ("slot", "ssid", "pass", "clear_pass"):
                    return False, None, f"wifi.{ek}", "unknown field", []
            slot = idx
            if "slot" in entry:
                if not _is_int(entry["slot"]):
                    return False, None, "wifi.slot", f"must be integer 0-{WIFI_SLOTS - 1}", []
                slot = entry["slot"]
            if slot < 0 or slot >= WIFI_SLOTS:
                return False, None, "wifi.slot", f"out of range 0-{WIFI_SLOTS - 1}", []

            ssid_changed_to_non_empty = False
            if "ssid" in entry:
                s = entry["ssid"]
                if not isinstance(s, str):
                    return False, None, f"wifi[{slot}].ssid", "must be a string", []
                if len(s.encode("utf-8")) > max_ssid:
                    return False, None, f"wifi[{slot}].ssid", f"must be 0-{max_ssid} bytes (empty clears slot)", []
                if out["wifi"][slot]["ssid"] != s:
                    if len(s) > 0:
                        ssid_changed_to_non_empty = True
                    out["wifi"][slot]["ssid"] = s
                if len(s) == 0 and out["wifi"][slot]["pass"] != "":
                    out["wifi"][slot]["pass"] = ""

            clear_pass = False
            if "clear_pass" in entry:
                if not isinstance(entry["clear_pass"], bool):
                    return False, None, f"wifi[{slot}].clear_pass", "must be a boolean", []
                clear_pass = entry["clear_pass"]

            has_non_empty_pass = False
            if "pass" in entry:
                p = entry["pass"]
                if p is None:
                    clear_pass = True
                elif isinstance(p, str):
                    plen = len(p.encode("utf-8"))
                    if plen > 0:
                        if plen < min_pass or plen > max_pass:
                            return False, None, f"wifi[{slot}].pass", f"must be {min_pass}-{max_pass} bytes (null or clear_pass for an open network)", []
                        has_non_empty_pass = True
                        out["wifi"][slot]["pass"] = p
                else:
                    return False, None, f"wifi[{slot}].pass", "must be string or null", []

            if ssid_changed_to_non_empty and not has_non_empty_pass and not clear_pass:
                return False, None, f"wifi[{slot}].pass", "required when ssid changes", []

            if clear_pass:
                out["wifi"][slot]["pass"] = ""
            if out["wifi"][slot]["ssid"] == "":
                out["wifi"][slot]["pass"] = ""

        applied.append("wifi")

    if "device_name" in obj:
        s = obj["device_name"]
        if not isinstance(s, str):
            return False, None, "device_name", "must be a string", []
        if not DEVICE_NAME_RE.fullmatch(s):
            return False, None, "device_name", "must be 1-32 chars [a-z0-9-], not starting/ending with '-'", []
        out["device_name"] = s
        applied.append("device_name")

    if "ap_ssid" in obj:
        s = obj["ap_ssid"]
        if not isinstance(s, str):
            return False, None, "ap_ssid", "must be a string", []
        slen = len(s.encode("utf-8"))
        if slen < min_ap_ssid or slen > max_ap_ssid:
            return False, None, "ap_ssid", f"must be {min_ap_ssid}-{max_ap_ssid} bytes", []
        out["ap_ssid"] = s
        applied.append("ap_ssid")

    def _check_int_field(num_field):
        min_v, max_v = CONFIG_FIELD_BOUNDS[num_field]
        v = obj[num_field]
        if not _is_int(v):
            if num_field == "alert_mode":
                return False, num_field, "must be 0 or 1", None
            return False, num_field, f"must be an integer {min_v}-{max_v}", None
        if v < min_v or v > max_v:
            if num_field == "alert_mode":
                return False, num_field, "must be 0 or 1", None
            return False, num_field, f"must be in range {min_v}-{max_v}", None
        return True, "", "", v

    if "ap_pass" in obj:
        p = obj["ap_pass"]
        if not isinstance(p, str):
            return False, None, "ap_pass", "must be a string", []
        plen = len(p.encode("utf-8"))
        if plen > 0:
            if plen < min_pass or plen > max_pass:
                return False, None, "ap_pass", f"must be empty to keep or {min_pass}-{max_pass} bytes", []
            out["ap_pass"] = p
            applied.append("ap_pass")

    if "setup_window_s" in obj:
        ok, ef, er, val = _check_int_field("setup_window_s")
        if not ok:
            return False, None, ef, er, []
        out["setup_window_s"] = val
        applied.append("setup_window_s")

    clear_adm = False
    if "clear_admin_pass" in obj:
        if not isinstance(obj["clear_admin_pass"], bool):
            return False, None, "clear_admin_pass", "must be a boolean", []
        clear_adm = obj["clear_admin_pass"]

    if clear_adm and "admin_pass" in obj:
        return False, None, "admin_pass", "cannot be sent together with clear_admin_pass", []

    if "admin_pass" in obj:
        p = obj["admin_pass"]
        if p is None:
            clear_adm = True
        elif isinstance(p, str):
            plen = len(p.encode("utf-8"))
            if plen > 0:
                if plen < min_pass or plen > max_pass:
                    return False, None, "admin_pass", f"must be empty or {min_pass}-{max_pass} bytes", []
                out["admin_pass"] = p
                applied.append("admin_pass")
        else:
            return False, None, "admin_pass", "must be string or null", []

    if clear_adm:
        out["admin_pass"] = ""
        applied.append("admin_pass")

    for num_field in CONFIG_FIELD_BOUNDS:
        if num_field == "setup_window_s":
            continue
        if num_field in obj:
            ok, ef, er, val = _check_int_field(num_field)
            if not ok:
                return False, None, ef, er, []
            out[num_field] = val
            applied.append(num_field)

    if "cloud_enabled" in obj:
        v = obj["cloud_enabled"]
        if not isinstance(v, bool):
            return False, None, "cloud_enabled", "must be a boolean", []
        out["cloud_enabled"] = v
        applied.append("cloud_enabled")

    for url_field in ("webhook_url", "local_url"):
        if url_field in obj:
            ok, err_f, err_r = _validate_url_field(url_field, obj[url_field])
            if not ok:
                return False, None, err_f, err_r, []
            out[url_field] = obj[url_field]
            applied.append(url_field)

    return True, out, "", "", applied


class MockDeviceState:
    def __init__(self, html):
        self.lock = threading.RLock()
        self.html = html
        self.start_mono = time.monotonic()
        self._notify_threads = []
        self.notify_gen = {"local": 0, "cloud": 0}
        self.reset()

    def reset(self, overrides=None):
        with self.lock:
            self.config = default_config()
            self.mode = "ARMED"
            self.snooze_until_mono = 0.0
            self.grid_state = "ONLINE"
            self.outage_started_mono = None
            self.outage_duration_s = 0
            self.ntp_synced = True
            self.update_in_progress = False
            self.last_update_bytes = 0
            self.wifi_runtime = {
                "connected": True,
                "ssid": "MockHomeWiFi",
                "slot": 0,
                "ip": "192.168.1.105",
                "rssi": -58,
            }
            self.setup_ap = {
                "active": False,
                "ssid": DEFAULT_AP_SSID,
                "ip": "",
                "clients": 0,
                "closes_in_s": 0,
            }
            self.scan_networks = [
                {"ssid": "MockHomeWiFi", "rssi": -54, "secure": True, "channel": 6},
                {"ssid": "GuestOpen", "rssi": -72, "secure": False, "channel": 1},
            ]
            self.scan_pending_polls = 0
            self.scan_hold = False
            self.force_error = None
            self.notify_status = {"local": None, "cloud": None}
            self.notify_gen["local"] += 1
            self.notify_gen["cloud"] += 1
            self.requests = []
            if overrides is not None:
                self.apply_state_locked(overrides)

    def apply_state_locked(self, data):
        if not isinstance(data, dict):
            raise ValueError("state: expected JSON object")

        unknown = set(data.keys()) - _STATE_ALLOWED_KEYS
        if unknown:
            raise ValueError(f"state: unknown key(s): {', '.join(sorted(unknown))}")

        if "config" in data:
            prev_local = self.config["local_url"]
            prev_cloud = self.config["webhook_url"]
            prev_cloud_en = bool(self.config.get("cloud_enabled", False))
            c = data["config"]
            if not isinstance(c, dict):
                raise ValueError("state.config: must be an object")
            unknown_cfg = set(c.keys()) - KNOWN_CONFIG_KEYS
            if unknown_cfg:
                raise ValueError(f"state.config: unknown key(s): {', '.join(sorted(unknown_cfg))}")
            if "wifi" in c:
                if not isinstance(c["wifi"], list):
                    raise ValueError("state.config.wifi: must be a list")
                for item in c["wifi"]:
                    if not isinstance(item, dict):
                        raise ValueError("state.config.wifi: entries must be objects")
                    slot = item.get("slot", 0)
                    if _is_int(slot) and 0 <= slot < WIFI_SLOTS:
                        self.config["wifi"][slot] = {
                            "slot": slot,
                            "ssid": item.get("ssid", ""),
                            "pass": item.get("pass", ""),
                        }
                    else:
                        raise ValueError(f"state.config.wifi.slot out of range: {slot!r}")
            for k in KNOWN_CONFIG_KEYS - {"wifi", "clear_admin_pass"}:
                if k in c:
                    self.config[k] = c[k]
            if c.get("clear_admin_pass"):
                self.config["admin_pass"] = ""
            if "ap_ssid" in c:
                self.setup_ap["ssid"] = c["ap_ssid"]
            if self.config["local_url"] != prev_local or not self.config["local_url"]:
                self.notify_status["local"] = None
                self.notify_gen["local"] += 1
            if (
                self.config["webhook_url"] != prev_cloud
                or not self.config["webhook_url"]
                or bool(self.config.get("cloud_enabled", False)) != prev_cloud_en
                or not self.config.get("cloud_enabled", False)
            ):
                self.notify_status["cloud"] = None
                self.notify_gen["cloud"] += 1

        if "mode" in data:
            m = data["mode"]
            if m not in VALID_MODES:
                raise ValueError(f"state.mode: invalid mode {m!r}")
            self.mode = m
            if self.mode == "SNOOZED":
                rem = int(data.get("snooze_remaining_s", self.config["snooze_default_min"] * 60))
                self.snooze_until_mono = time.monotonic() + max(0, rem)
            else:
                self.snooze_until_mono = 0.0
        elif "snooze_remaining_s" in data and self.mode == "SNOOZED":
            self.snooze_until_mono = time.monotonic() + max(0, int(data["snooze_remaining_s"]))

        if "grid" in data:
            g = data["grid"]
            if not isinstance(g, str) or g.upper() not in VALID_GRID_STATES:
                raise ValueError(f"state.grid: must be one of {VALID_GRID_STATES}")
            self.set_grid_locked(g.upper())

        if "outage_duration_s" in data:
            self.outage_duration_s = max(0, int(data["outage_duration_s"]))

        if "wifi" in data:
            if not isinstance(data["wifi"], dict):
                raise ValueError("state.wifi: must be an object")
            self.wifi_runtime.update(data["wifi"])

        if "setup_ap" in data:
            if not isinstance(data["setup_ap"], dict):
                raise ValueError("state.setup_ap: must be an object")
            self.setup_ap.update(data["setup_ap"])
            if self.setup_ap.get("active") and not self.setup_ap.get("ip"):
                self.setup_ap["ip"] = "192.168.4.1"
            elif not self.setup_ap.get("active"):
                self.setup_ap["ip"] = ""

        if "scan_networks" in data:
            if not isinstance(data["scan_networks"], list):
                raise ValueError("state.scan_networks: must be a list")
            self.scan_networks = list(data["scan_networks"])

        if "scan_pending_polls" in data:
            self.scan_pending_polls = max(0, int(data["scan_pending_polls"]))

        if "scan_hold" in data:
            if not isinstance(data["scan_hold"], bool):
                raise ValueError("state.scan_hold: must be a boolean")
            self.scan_hold = data["scan_hold"]

        if "force_error" in data:
            self.force_error = data["force_error"]

        if "update_in_progress" in data:
            if not isinstance(data["update_in_progress"], bool):
                raise ValueError("state.update_in_progress: must be a boolean")
            self.update_in_progress = data["update_in_progress"]

        if "ntp_synced" in data:
            if not isinstance(data["ntp_synced"], bool):
                raise ValueError("state.ntp_synced: must be a boolean")
            self.ntp_synced = data["ntp_synced"]

        if "notify" in data:
            nd = data["notify"]
            if not isinstance(nd, dict):
                raise ValueError("state.notify: must be an object")
            now_mono = time.monotonic()
            for slot_key in ("local", "cloud"):
                if slot_key not in nd:
                    continue
                val = nd[slot_key]
                if val is None:
                    self.notify_status[slot_key] = None
                elif isinstance(val, dict):
                    age_s = max(0, int(val.get("age_s", 0)))
                    self.notify_status[slot_key] = {
                        "event": str(val.get("event", "TEST_ALARM"))[:15],
                        "ok": bool(val.get("ok", True)),
                        "code": int(val.get("code", 200)),
                        "reason": str(val.get("reason", ""))[:63],
                        "mono": now_mono - age_s,
                    }
                else:
                    raise ValueError(f"state.notify.{slot_key}: must be object or null")

        if data.get("clear_requests"):
            self.requests.clear()

    def set_grid_locked(self, next_state):
        if next_state not in VALID_GRID_STATES:
            raise ValueError(f"invalid grid state: {next_state!r}")
        now = time.monotonic()
        prev = self.grid_state
        self.grid_state = next_state
        if next_state == "OFFLINE":
            if prev != "OFFLINE":
                self.outage_started_mono = now
                self.outage_duration_s = 0
        elif next_state == "ONLINE":
            if prev == "OFFLINE" and self.outage_started_mono is not None:
                self.outage_duration_s = max(0, int(now - self.outage_started_mono))
            self.outage_started_mono = None
        else:
            self.outage_started_mono = None
            self.outage_duration_s = 0
        return prev, next_state

    def _refresh_timers_locked(self):
        now = time.monotonic()
        expired_while_offline = False
        if self.mode == "SNOOZED" and now >= self.snooze_until_mono:
            self.mode = "ARMED"
            self.snooze_until_mono = 0.0
            if self.grid_state == "OFFLINE":
                expired_while_offline = True
        if self.grid_state == "OFFLINE" and self.outage_started_mono is not None:
            self.outage_duration_s = max(0, int(now - self.outage_started_mono))
        return expired_while_offline

    def has_saved_networks_locked(self):
        return any(w["ssid"] != "" for w in self.config["wifi"])

    def _current_ts_locked(self):
        return int(time.time()) if self.ntp_synced else 0

    def wait_for_notifications(self, timeout=2.0):
        with self.lock:
            threads = list(self._notify_threads)
        deadline = time.monotonic() + timeout
        for t in threads:
            rem = max(0.0, deadline - time.monotonic())
            t.join(timeout=rem)

    def build_status_dict(self):
        with self.lock:
            self._refresh_timers_locked()
            snooze_rem = 0
            if self.mode == "SNOOZED" and self.snooze_until_mono > time.monotonic():
                snooze_rem = int(self.snooze_until_mono - time.monotonic())
            ap_active = bool(self.setup_ap.get("active", False))
            now_mono = time.monotonic()
            def _notify_slot_dict(slot_key, url_key, enabled=True):
                if not enabled or not self.config.get(url_key):
                    return None
                rec = self.notify_status.get(slot_key)
                if not rec:
                    return None
                return {
                    "event": rec["event"],
                    "ok": bool(rec["ok"]),
                    "code": int(rec["code"]),
                    "reason": rec["reason"],
                    "age_s": max(0, int(now_mono - rec["mono"])),
                }

            return {
                "device": self.config["device_name"],
                "fw_version": FW_VERSION,
                "grid": self.grid_state,
                "mode": self.mode,
                "power": (self.grid_state == "ONLINE"),
                "armed": (self.mode == "ARMED"),
                "outage_duration_s": int(self.outage_duration_s),
                "snooze_remaining_s": snooze_rem,
                "uptime_s": max(0, int(now_mono - self.start_mono)),
                "wifi": {
                    "connected": bool(self.wifi_runtime["connected"]),
                    "ssid": self.wifi_runtime["ssid"] if self.wifi_runtime["connected"] else "",
                    "slot": int(self.wifi_runtime["slot"]) if self.wifi_runtime["connected"] else -1,
                    "ip": self.wifi_runtime["ip"] if self.wifi_runtime["connected"] else "",
                    "rssi": int(self.wifi_runtime["rssi"]) if self.wifi_runtime["connected"] else -100,
                },
                "setup_ap": {
                    "active": ap_active,
                    "ssid": self.setup_ap.get("ssid", self.config["ap_ssid"]),
                    "ip": "192.168.4.1" if ap_active else "",
                    "clients": int(self.setup_ap.get("clients", 0)),
                    "closes_in_s": int(self.setup_ap.get("closes_in_s", 0)) if ap_active else 0,
                },
                "admin_protected": bool(self.config["admin_pass"]),
                "snooze_default_min": int(self.config["snooze_default_min"]),
                "free_heap": 184520,
                "ts": self._current_ts_locked(),
                "notify": {
                    "local": _notify_slot_dict("local", "local_url"),
                    "cloud": _notify_slot_dict(
                        "cloud",
                        "webhook_url",
                        enabled=bool(self.config.get("cloud_enabled", False)),
                    ),
                },
            }

    def build_config_dict(self):
        with self.lock:
            return {
                "wifi": [
                    {
                        "slot": w["slot"],
                        "ssid": w["ssid"],
                        "has_pass": bool(w["pass"]),
                    }
                    for w in self.config["wifi"]
                ],
                "device_name": self.config["device_name"],
                "ap_ssid": self.config["ap_ssid"],
                "has_ap_pass": bool(self.config["ap_pass"]),
                "setup_window_s": self.config["setup_window_s"],
                "has_admin_pass": bool(self.config["admin_pass"]),
                "debounce_ms": self.config["debounce_ms"],
                "restore_cooldown_ms": self.config["restore_cooldown_ms"],
                "snooze_default_min": self.config["snooze_default_min"],
                "alert_mode": self.config["alert_mode"],
                "cloud_enabled": bool(self.config["cloud_enabled"]),
                "webhook_url": self.config["webhook_url"],
                "local_url": self.config["local_url"],
            }

    def build_power_off_payload(self):
        with self.lock:
            return {
                "event": "POWER_OFF",
                "device": self.config["device_name"],
                "state": "ARMED",
                "grid": "OFFLINE",
                "power": False,
                "armed": True,
                "duration_s": 0,
                "uptime_s": max(0, int(time.monotonic() - self.start_mono)),
                "ts": self._current_ts_locked(),
            }

    def build_power_on_payload(self):
        with self.lock:
            self._refresh_timers_locked()
            am = self.config["alert_mode"]
            return {
                "event": "POWER_ON",
                "device": self.config["device_name"],
                "state": self.mode,
                "grid": "ONLINE",
                "power": True,
                "armed": (self.mode == "ARMED"),
                "duration_s": int(self.outage_duration_s),
                "uptime_s": max(0, int(time.monotonic() - self.start_mono)),
                "alert_mode": "OUTAGE_ONLY" if am == 0 else "OUTAGE_AND_RESTORE",
                "ts": self._current_ts_locked(),
            }

    def build_test_alarm_payload(self):
        with self.lock:
            self._refresh_timers_locked()
            return {
                "event": "TEST_ALARM",
                "device": self.config["device_name"],
                "state": self.mode,
                "grid": "TEST",
                "power": False,
                "armed": (self.mode == "ARMED"),
                "duration_s": 0,
                "uptime_s": max(0, int(time.monotonic() - self.start_mono)),
                "ts": self._current_ts_locked(),
            }

    def build_alarm_stop_payload(self):
        with self.lock:
            self._refresh_timers_locked()
            snooze_rem = 0
            if self.mode == "SNOOZED" and self.snooze_until_mono > time.monotonic():
                snooze_rem = int(self.snooze_until_mono - time.monotonic())
            return {
                "event": "ALARM_STOP",
                "device": self.config["device_name"],
                "state": self.mode,
                "snooze_remaining_s": snooze_rem,
                "uptime_s": max(0, int(time.monotonic() - self.start_mono)),
                "ts": self._current_ts_locked(),
            }


def should_send_alarm_stop(prev_mode, next_mode):
    if next_mode == "DISARMED":
        return prev_mode == "ARMED"
    if next_mode == "SNOOZED":
        return prev_mode in ("ARMED", "SNOOZED")
    return False


def should_use_ntfy_format(url, event_name):
    parsed = urllib.parse.urlparse(url or "")
    return parsed.hostname == "ntfy.sh" and event_name in mobile_alarm_sim.NTFY_TEMPLATES


def _dispatch_notifications(state: MockDeviceState, event_name: str, payload_obj: dict):
    """Send one HTTP POST attempt per matching notification target and log in state.requests."""
    with state.lock:
        local_url = state.config.get("local_url", "")
        local_gen = state.notify_gen["local"]
        webhook_url = state.config.get("webhook_url", "")
        cloud_enabled = bool(state.config.get("cloud_enabled", False))
        cloud_gen = state.notify_gen["cloud"]
        alert_mode = state.config.get("alert_mode", 0)

    targets = []
    if local_url and (event_name in mobile_alarm_sim.ALERT_EVENTS or event_name == "ALARM_STOP"):
        targets.append(("local_url", "local", local_url, local_gen))
    if cloud_enabled and webhook_url and (
        event_name in ("POWER_OFF", "TEST_ALARM")
        or (event_name == "POWER_ON" and alert_mode == 1)
    ):
        targets.append(("webhook_url", "cloud", webhook_url, cloud_gen))

    if not targets:
        return

    def _send_one(target_key, slot_key, url_template, expected_gen):
        expanded_url = mobile_alarm_sim.expand_target_url(url_template, event_name)
        parsed = urllib.parse.urlparse(expanded_url)
        if should_use_ntfy_format(expanded_url, event_name):
            headers = mobile_alarm_sim.build_ntfy_headers(event_name)
            body_str = mobile_alarm_sim.build_ntfy_body(
                event_name,
                device=payload_obj["device"],
                duration_s=payload_obj.get("duration_s", 0),
            )
            body_bytes = body_str.encode("utf-8")
            parsed_json = None
        else:
            headers = {"Content-Type": "application/json"}
            body_str = json.dumps(payload_obj)
            body_bytes = body_str.encode("utf-8")
            parsed_json = copy.deepcopy(payload_obj)

        status_code = None
        err_msg = None
        req = urllib.request.Request(expanded_url, data=body_bytes, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=2.0) as resp:
                status_code = int(resp.status)
        except urllib.error.HTTPError as exc:
            status_code = int(exc.code)
            err_msg = str(exc)
        except Exception as exc:
            err_msg = str(exc)

        entry = {
            "outbound": True,
            "target": target_key,
            "method": "POST",
            "url": expanded_url,
            "path": parsed.path or "/",
            "headers": {k.lower(): v for k, v in headers.items()},
            "body": body_str,
            "json": parsed_json,
            "status": status_code,
            "error": err_msg,
        }
        if status_code is not None and 200 <= status_code < 300:
            rec = {
                "event": event_name[:15],
                "ok": True,
                "code": status_code,
                "reason": "",
                "mono": time.monotonic(),
            }
        elif status_code is not None:
            rec = {
                "event": event_name[:15],
                "ok": False,
                "code": status_code,
                "reason": f"HTTP {status_code}",
                "mono": time.monotonic(),
            }
        else:
            rec = {
                "event": event_name[:15],
                "ok": False,
                "code": -1,
                "reason": (err_msg or "Connect failed")[:63],
                "mono": time.monotonic(),
            }
        with state.lock:
            state.requests.append(entry)
            if state.notify_gen[slot_key] == expected_gen and state.config[target_key]:
                if slot_key != "cloud" or state.config.get("cloud_enabled", False):
                    state.notify_status[slot_key] = rec

    for target_key, slot_key, url_tmpl, expected_gen in targets:
        t = threading.Thread(
            target=_send_one,
            args=(target_key, slot_key, url_tmpl, expected_gen),
            daemon=True,
        )
        with state.lock:
            state._notify_threads = [th for th in state._notify_threads if th.is_alive()]
            state._notify_threads.append(t)
        t.start()


def _emit_alert(state: MockDeviceState, event_name: str):
    if event_name == "POWER_OFF":
        payload = state.build_power_off_payload()
    elif event_name == "POWER_ON":
        payload = state.build_power_on_payload()
    elif event_name == "TEST_ALARM":
        payload = state.build_test_alarm_payload()
    else:
        raise ValueError(f"Unknown alert event: {event_name}")

    _dispatch_notifications(state, event_name, payload)


def _emit_alarm_stop(state: MockDeviceState):
    payload = state.build_alarm_stop_payload()
    _dispatch_notifications(state, "ALARM_STOP", payload)


def _parse_snooze_request(query_params, raw_body, default_min):
    dur_min = default_min if SNOOZE_MIN_MINUTES <= default_min <= SNOOZE_MAX_MINUTES else DEFAULT_SNOOZE_MIN

    if "min" in query_params:
        val = _parse_strtol_int(query_params["min"][0])
        if val is None or val < 1:
            return False, 0, "min", "must be a positive integer >= 1"
        dur_min = min(SNOOZE_MAX_MINUTES, val)
        return True, dur_min, "", ""

    if "duration" in query_params:
        sec = _parse_strtol_int(query_params["duration"][0])
        if sec is None or sec < 1:
            return False, 0, "duration", "must be a positive integer >= 1 seconds"
        m = (sec // 60) if sec >= 60 else 1
        dur_min = min(SNOOZE_MAX_MINUTES, m)
        return True, dur_min, "", ""

    if raw_body.strip():
        try:
            obj = json.loads(raw_body)
        except json.JSONDecodeError:
            return False, 0, "body", "invalid JSON object"
        if not isinstance(obj, dict):
            return False, 0, "body", "invalid JSON object"
        if "duration_min" in obj:
            v = obj["duration_min"]
            if not _is_int(v):
                return False, 0, "duration_min", "must be a positive integer >= 1"
            if v < 1:
                return False, 0, "duration_min", "must be >= 1"
            dur_min = min(SNOOZE_MAX_MINUTES, v)
        elif "duration" in obj:
            v = obj["duration"]
            if not _is_int(v):
                return False, 0, "duration", "must be a positive integer >= 1 seconds"
            if v < 1:
                return False, 0, "duration", "must be >= 1 seconds"
            m = (v // 60) if v >= 60 else 1
            dur_min = min(SNOOZE_MAX_MINUTES, m)
        else:
            return False, 0, "body", "expected duration_min or duration"

    return True, dur_min, "", ""


def _extract_host_only(host_header):
    h = (host_header or "").strip().lower()
    if not h:
        return ""
    if h.startswith("["):
        end = h.find("]")
        return h[1:end] if end != -1 else h
    return h.split(":", 1)[0]


def _make_handler(state: MockDeviceState):
    class MockHandler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args):
            pass

        def _send_bytes(self, status, content_type, body_bytes, extra_headers=None):
            rec = getattr(self, "_recorded_entry", None)
            if rec is not None:
                with state.lock:
                    rec["status"] = status
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body_bytes)))
            self.send_header("Cache-Control", "no-store")
            if extra_headers:
                for k, v in extra_headers.items():
                    self.send_header(k, v)
            self.end_headers()
            if body_bytes:
                self.wfile.write(body_bytes)

        def _send_json(self, status, obj, extra_headers=None):
            body = json.dumps(obj).encode("utf-8")
            self._send_bytes(status, "application/json", body, extra_headers=extra_headers)

        def _read_body_bytes(self):
            raw_len = self.headers.get("Content-Length", "0")
            try:
                clen = int(raw_len or "0")
            except ValueError as exc:
                raise ValueError("invalid Content-Length") from exc
            if clen < 0:
                raise ValueError("negative Content-Length")
            if clen == 0:
                return b""
            return self.rfile.read(clen)

        def _record_request(self, path, query, raw_bytes):
            ct = self.headers.get("Content-Type", "")
            is_multipart = ct.lower().startswith("multipart/form-data")
            raw_body = "" if is_multipart else raw_bytes.decode("utf-8", errors="replace")
            parsed_json = None
            if raw_body and not is_multipart:
                try:
                    parsed_json = json.loads(raw_body)
                except json.JSONDecodeError:
                    parsed_json = None
            hdrs = {k.lower(): v for k, v in self.headers.items()}
            entry = {
                "outbound": False,
                "method": self.command,
                "path": path,
                "query": query,
                "headers": hdrs,
                "body": raw_body,
                "body_bytes": len(raw_bytes),
                "json": parsed_json,
                "status": None,
            }
            self._recorded_entry = entry
            with state.lock:
                state.requests.append(entry)

        def _check_forced_error(self, path):
            with state.lock:
                fe = copy.deepcopy(state.force_error) if state.force_error else None
            if not fe:
                return False
            methods = fe.get("methods")
            if methods and self.command not in methods:
                return False
            match_path = fe.get("path")
            match_prefix = fe.get("path_prefix")
            if match_path and path != match_path:
                return False
            if match_prefix and not path.startswith(match_prefix):
                return False
            if fe.get("network_error"):
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    self.connection.close()
                except OSError:
                    pass
                return True
            status = int(fe.get("status", 500))
            msg = fe.get("error", f"Injected HTTP {status} error")
            self._send_json(status, {"error": msg})
            return True

        def _ensure_csrf_and_auth(self):
            if self.headers.get("X-Requested-With") != "pm-ui":
                self._send_json(403, {"error": "csrf: missing X-Requested-With header"})
                return False
            with state.lock:
                adm_pass = state.config["admin_pass"]
            if not adm_pass:
                return True
            auth = self.headers.get("Authorization", "")
            if auth.startswith("Basic "):
                try:
                    decoded = base64.b64decode(auth[6:].strip()).decode("utf-8", errors="replace")
                    user, pw = decoded.split(":", 1)
                    if user == "admin" and pw == adm_pass:
                        return True
                except Exception:
                    pass
            self._send_bytes(
                401,
                "text/plain",
                b"Unauthorized",
                extra_headers={"WWW-Authenticate": 'Basic realm="Login Required"'},
            )
            return False

        def _is_foreign_ap_host(self):
            with state.lock:
                ap_active = bool(state.setup_ap.get("active", False))
            if not ap_active:
                return False
            host_only = _extract_host_only(self.headers.get("Host", ""))
            if not host_only:
                return False
            return host_only not in ("192.168.4.1", "127.0.0.1", "localhost", "::1")

        def _dispatch(self, raw_bytes):
            parsed = urllib.parse.urlparse(self.path)
            path = parsed.path

            self._record_request(path, parsed.query, raw_bytes)
            if self._check_forced_error(path):
                return

            route = _ROUTE_MAP.get((self.command, path))
            if route is None:
                if self.command == "GET" and self._is_foreign_ap_host():
                    self._send_bytes(
                        302,
                        "text/plain",
                        b"",
                        extra_headers={"Location": "http://192.168.4.1/"},
                    )
                    return
                self._send_bytes(404, "text/plain", b"404 Not Found")
                return

            guard, handler_name = route
            if guard == "admin" and not self._ensure_csrf_and_auth():
                return
            getattr(self, handler_name)(parsed, raw_bytes)

        # ---- Route handlers ----

        def _handle_root(self, parsed, raw_bytes):
            if self._is_foreign_ap_host():
                self._send_bytes(
                    302,
                    "text/plain",
                    b"",
                    extra_headers={"Location": "http://192.168.4.1/"},
                )
                return
            self._send_bytes(200, "text/html; charset=utf-8", state.html.encode("utf-8"))

        def _handle_get_status(self, parsed, raw_bytes):
            with state.lock:
                expired_offline = state._refresh_timers_locked()
            if expired_offline:
                _emit_alert(state, "POWER_OFF")
            self._send_json(200, state.build_status_dict())

        def _handle_arm(self, parsed, raw_bytes):
            alert_offline = False
            with state.lock:
                changed = (state.mode != "ARMED")
                state.mode = "ARMED"
                state.snooze_until_mono = 0.0
                alert_offline = changed and (state.grid_state == "OFFLINE")
            if alert_offline:
                _emit_alert(state, "POWER_OFF")
            self._send_json(200, {"status": "ok", "mode": "ARMED", "message": "System armed"})

        def _handle_disarm(self, parsed, raw_bytes):
            with state.lock:
                prev_mode = state.mode
                state.mode = "DISARMED"
                state.snooze_until_mono = 0.0
                stop_alarm = should_send_alarm_stop(prev_mode, "DISARMED")
            if stop_alarm:
                _emit_alarm_stop(state)
            self._send_json(200, {"status": "ok", "mode": "DISARMED", "message": "System disarmed"})

        def _handle_toggle(self, parsed, raw_bytes):
            with state.lock:
                prev_mode = state.mode
                state.mode = "DISARMED" if prev_mode == "ARMED" else "ARMED"
                state.snooze_until_mono = 0.0
                m = state.mode
                alert_offline = (m == "ARMED" and state.grid_state == "OFFLINE")
                stop_alarm = should_send_alarm_stop(prev_mode, m)
            if alert_offline:
                _emit_alert(state, "POWER_OFF")
            if stop_alarm:
                _emit_alarm_stop(state)
            self._send_json(200, {"status": "ok", "mode": m, "message": "State toggled"})

        def _handle_snooze(self, parsed, raw_bytes):
            with state.lock:
                if state.mode == "DISARMED":
                    self._send_json(409, {"error": "snooze: system is disarmed"})
                    return
                def_min = state.config["snooze_default_min"]
            query_params = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
            raw_body = raw_bytes.decode("utf-8", errors="replace")
            ok, dur_min, err_field, err_reason = _parse_snooze_request(query_params, raw_body, def_min)
            if not ok:
                self._send_json(400, {"error": f"{err_field}: {err_reason}"})
                return
            with state.lock:
                prev_mode = state.mode
                state.mode = "SNOOZED"
                state.snooze_until_mono = time.monotonic() + dur_min * 60
                stop_alarm = should_send_alarm_stop(prev_mode, "SNOOZED")
            if stop_alarm:
                _emit_alarm_stop(state)
            self._send_json(200, {
                "status": "ok",
                "mode": "SNOOZED",
                "remaining_s": dur_min * 60,
                "message": f"Alarm snoozed for {dur_min} min",
            })

        def _handle_test(self, parsed, raw_bytes):
            _emit_alert(state, "TEST_ALARM")
            self._send_json(200, {"status": "ok", "message": "Test alert dispatched"})

        def _handle_get_config(self, parsed, raw_bytes):
            self._send_json(200, state.build_config_dict())

        def _handle_post_config(self, parsed, raw_bytes):
            if len(raw_bytes) > CONFIG_BODY_MAX_BYTES:
                self._send_json(413, {"error": "body: too large"})
                return
            raw_body = raw_bytes.decode("utf-8", errors="replace")
            if not raw_body:
                self._send_json(400, {"error": "body: missing request body"})
                return
            try:
                obj = json.loads(raw_body)
            except json.JSONDecodeError:
                self._send_json(400, {"error": "body: invalid JSON object"})
                return
            with state.lock:
                prev_local = state.config["local_url"]
                prev_cloud = state.config["webhook_url"]
                prev_cloud_en = bool(state.config.get("cloud_enabled", False))
                ok, next_cfg, err_field, err_reason, applied = validate_and_merge_config(state.config, obj)
                if not ok:
                    self._send_json(400, {"error": f"{err_field}: {err_reason}"})
                    return
                state.config = next_cfg
                state.setup_ap["ssid"] = next_cfg["ap_ssid"]
                if next_cfg["local_url"] != prev_local or not next_cfg["local_url"]:
                    state.notify_status["local"] = None
                    state.notify_gen["local"] += 1
                if (
                    next_cfg["webhook_url"] != prev_cloud
                    or not next_cfg["webhook_url"]
                    or bool(next_cfg.get("cloud_enabled", False)) != prev_cloud_en
                    or not next_cfg.get("cloud_enabled", False)
                ):
                    state.notify_status["cloud"] = None
                    state.notify_gen["cloud"] += 1
            self._send_json(200, {
                "status": "ok",
                "applied": applied,
                "message": "Configuration applied",
            })

        def _handle_scan(self, parsed, raw_bytes):
            with state.lock:
                if state.scan_hold:
                    self._send_json(200, {"status": "scanning"})
                    return
                if state.scan_pending_polls > 0:
                    state.scan_pending_polls -= 1
                    self._send_json(200, {"status": "scanning"})
                    return
                nets = sorted(
                    copy.deepcopy(state.scan_networks),
                    key=lambda item: item.get("rssi", -120),
                    reverse=True,
                )[:20]
            self._send_json(200, {"networks": nets})

        def _handle_setup_ap(self, parsed, raw_bytes):
            with state.lock:
                has_nets = state.has_saved_networks_locked()
                state.setup_ap["active"] = True
                state.setup_ap["ssid"] = state.config["ap_ssid"]
                state.setup_ap["ip"] = "192.168.4.1"
                state.setup_ap["closes_in_s"] = 300 if has_nets else 0
            self._send_json(200, {"status": "ok", "message": "Setup AP opened for 5 minutes"})

        def _handle_setup_ap_close(self, parsed, raw_bytes):
            with state.lock:
                if not state.has_saved_networks_locked():
                    self._send_json(400, {"error": "setup_ap: cannot close without saved Wi-Fi networks"})
                    return
                state.setup_ap["active"] = False
                state.setup_ap["ip"] = ""
                state.setup_ap["closes_in_s"] = 0
            self._send_json(200, {"status": "ok", "message": "Setup AP closing"})

        def _handle_reboot(self, parsed, raw_bytes):
            self._send_json(200, {"status": "ok", "message": "Rebooting device..."})

        def _handle_factory_reset(self, parsed, raw_bytes):
            with state.lock:
                state.config = default_config()
                state.mode = "ARMED"
                state.snooze_until_mono = 0.0
                state.setup_ap = {
                    "active": True,
                    "ssid": state.config["ap_ssid"],
                    "ip": "192.168.4.1",
                    "clients": 0,
                    "closes_in_s": 0,
                }
                state.wifi_runtime = {
                    "connected": False,
                    "ssid": "",
                    "slot": -1,
                    "ip": "",
                    "rssi": -100,
                }
                state.notify_status = {"local": None, "cloud": None}
                state.notify_gen["local"] += 1
                state.notify_gen["cloud"] += 1
            self._send_json(200, {"status": "ok", "message": "Factory reset complete. Rebooting..."})

        def _handle_update(self, parsed, raw_bytes):
            with state.lock:
                if not state.config["admin_pass"]:
                    self._send_json(403, {"error": "update: set an admin password first"})
                    return
                if state.update_in_progress:
                    self._send_json(409, {"error": "update: already in progress"})
                    return

            ct = self.headers.get("Content-Type", "")
            if not ct.lower().startswith("multipart/form-data"):
                self._send_json(400, {"error": "update: expected multipart/form-data"})
                return

            header_block = f"Content-Type: {ct}\r\nMIME-Version: 1.0\r\n\r\n".encode("latin-1")
            msg = BytesParser(policy=email.policy.default).parsebytes(header_block + raw_bytes)
            if not msg.is_multipart():
                self._send_json(400, {"error": "update: invalid multipart body"})
                return

            fw_bytes = None
            for part in msg.iter_parts():
                if part.get_param("name", header="content-disposition") == "firmware":
                    fw_bytes = part.get_payload(decode=True)
                    break

            if fw_bytes is None:
                self._send_json(400, {"error": "update: missing firmware file"})
                return
            if len(fw_bytes) == 0:
                self._send_json(500, {"error": "update: empty firmware file"})
                return

            with state.lock:
                state.last_update_bytes = len(fw_bytes)
                if self._recorded_entry is not None:
                    self._recorded_entry["firmware_bytes"] = len(fw_bytes)

            self._send_json(200, {"status": "ok", "message": "Firmware written, rebooting"})

        # ---- HTTP method entry points ----

        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path == "/__requests":
                with state.lock:
                    reqs = copy.deepcopy(state.requests)
                self._send_json(200, {"requests": reqs})
                return
            self._dispatch(b"")

        def do_POST(self):
            parsed = urllib.parse.urlparse(self.path)
            path = parsed.path
            try:
                raw_bytes = self._read_body_bytes()
            except ValueError as exc:
                self._send_json(400, {"error": f"body: {exc}"})
                return

            if path == "/__reset":
                raw_body = raw_bytes.decode("utf-8", errors="replace")
                overrides = None
                if raw_body.strip():
                    try:
                        overrides = json.loads(raw_body)
                    except json.JSONDecodeError:
                        self._send_json(400, {"error": "reset: invalid JSON"})
                        return
                try:
                    with state.lock:
                        state.reset(overrides=overrides)
                except ValueError as exc:
                    self._send_json(400, {"error": str(exc)})
                    return
                self._send_json(200, {"status": "ok"})
                return

            if path == "/__state":
                raw_body = raw_bytes.decode("utf-8", errors="replace")
                if raw_body.strip():
                    try:
                        data = json.loads(raw_body)
                    except json.JSONDecodeError:
                        self._send_json(400, {"error": "state: invalid JSON"})
                        return
                else:
                    data = {}
                try:
                    with state.lock:
                        state.apply_state_locked(data)
                except ValueError as exc:
                    self._send_json(400, {"error": str(exc)})
                    return
                self._send_json(200, {"status": "ok"})
                return

            if path == "/__grid":
                raw_body = raw_bytes.decode("utf-8", errors="replace")
                if not raw_body.strip():
                    self._send_json(400, {"error": "grid: missing JSON body"})
                    return
                try:
                    data = json.loads(raw_body)
                except json.JSONDecodeError:
                    self._send_json(400, {"error": "grid: invalid JSON"})
                    return
                if not isinstance(data, dict):
                    self._send_json(400, {"error": "grid: expected JSON object"})
                    return
                unknown = set(data.keys()) - {"grid"}
                if unknown:
                    self._send_json(400, {"error": f"grid: unknown key(s): {', '.join(sorted(unknown))}"})
                    return
                if not isinstance(data.get("grid"), str) or data["grid"].upper() not in VALID_GRID_STATES:
                    self._send_json(400, {"error": f"grid: must be one of {VALID_GRID_STATES}"})
                    return
                target_state = data["grid"].upper()

                with state.lock:
                    prev, curr = state.set_grid_locked(target_state)
                    mode = state.mode
                    dur = state.outage_duration_s
                if prev != curr:
                    if curr == "OFFLINE" and mode == "ARMED":
                        _emit_alert(state, "POWER_OFF")
                    elif curr == "ONLINE" and prev == "OFFLINE":
                        _emit_alert(state, "POWER_ON")
                self._send_json(200, {
                    "status": "ok",
                    "grid": curr,
                    "outage_duration_s": dur,
                })
                return

            self._dispatch(raw_bytes)

    return MockHandler


class _ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class MockDeviceServer:
    def __init__(
        self,
        host="127.0.0.1",
        port=0,
        lan=False,
    ):
        self.html = extract_web_ui_html()
        self.state = MockDeviceState(self.html)
        self.host = "0.0.0.0" if lan else host
        handler_cls = _make_handler(self.state)
        self.httpd = _ThreadingHTTPServer((self.host, port), handler_cls)
        self.port = self.httpd.server_address[1]
        self._http_thread = None

    @property
    def base_url(self):
        connect_host = "127.0.0.1" if self.host == "0.0.0.0" else self.host
        return f"http://{connect_host}:{self.port}"

    def start(self):
        self._http_thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self._http_thread.start()
        return self

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def main():
    parser = argparse.ArgumentParser(description="Mock ESP32 Power Outage Monitor device")
    parser.add_argument("--host", default="127.0.0.1", help="Bind address (ignored if --lan is set)")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (0 for ephemeral)")
    parser.add_argument("--lan", action="store_true", help="Bind 0.0.0.0 for LAN access")
    args = parser.parse_args()

    srv = MockDeviceServer(host=args.host, port=args.port, lan=args.lan)
    srv.start()
    print(f"[mock_device] Serving exact web_ui.h on http://{srv.host}:{srv.port}/ (lan={args.lan})")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[mock_device] Shutting down...")
    finally:
        srv.stop()


if __name__ == "__main__":
    main()
