# Power Outage Monitor Protocol & API Reference

HTTP REST API, notification target rules and payloads, NVS storage layout, and JSON schemas for Power Outage Monitor.

## Grid and mode state machine

- **Grid states:**
  - `BOOTING` – initial debounce window after power-up while the pin level settles (state unknown).
  - `ONLINE` – mains sense (`GPIO 4`) confirmed continuously HIGH for `debounce_ms` at boot, or for `max(debounce_ms, restore_cooldown_ms)` when recovering from `OFFLINE` (any LOW restarts the window).
  - `OFFLINE` – mains sense confirmed LOW for `debounce_ms`.
- **Operating modes:** `ARMED`, `DISARMED`, `SNOOZED`.
- **Entering `ARMED` while `OFFLINE`:** Arming, toggling to `ARMED`, or snooze expiry while `grid` is `OFFLINE` triggers a `POWER_OFF` alert.
- **Snooze rules:** `SNOOZE` is accepted only in `ARMED` or `SNOOZED` (restarts the timer); in `DISARMED` it is rejected (`409` on HTTP). Valid duration is `1..1440` minutes; values above `1440` clamp to `1440`, while `0`, negative, or non-numeric values return an error.

## HTTP REST API (port `80`)

### Guards and error format

- **`open`**: No header or authentication required.
- **`admin`**: Requires header `X-Requested-With: pm-ui` (missing returns `403` `{"error":"csrf: missing X-Requested-With header"}`) and, when an admin password is configured, HTTP Basic authentication for user `admin` (missing or wrong credentials return `401`).
- **Errors**: JSON object `{"error":"<field>: <reason>"}`.

### Routes

| Route | Method | Guard | Status codes | Notes |
|---|---|---|---|---|
| `/` | `GET` | `open` | `200`, `302` | Embedded web UI; on Setup AP, a foreign `Host` header redirects `302` to `http://192.168.4.1/` |
| `/api/status` | `GET` | `open` | `200`, `500` | Status JSON (see below); `500` `{"error":"status: json overflow"}` |
| `/api/arm` | `POST` | `open` | `200` | `{"status":"ok","mode":"ARMED","message":...}` |
| `/api/disarm` | `POST` | `open` | `200` | `{"status":"ok","mode":"DISARMED","message":...}` |
| `/api/toggle` | `POST` | `open` | `200` | `{"status":"ok","mode":"ARMED"|"DISARMED","message":...}` |
| `/api/test` | `POST` | `open` | `200` | Dispatches `TEST_ALARM`; `{"status":"ok","message":...}` |
| `/api/snooze` | `POST` | `open` | `200`, `400`, `409` | Accepts `?min=N`, `?duration=S`, `{"duration_min":N}`, `{"duration":S}`, or empty body (uses default); `400` on invalid value or `{"error":"body: expected duration_min or duration"}`; `409` `{"error":"snooze: system is disarmed"}` |
| `/api/config` | `GET` | `admin` | `200`, `401`, `403`, `500` | Config JSON (secrets omitted; `has_*` flags returned); `500` `{"error":"config: json overflow"}` |
| `/api/config` | `POST` | `admin` | `200`, `400`, `401`, `403`, `413`, `500` | Partial update, validated atomically; `413` `{"error":"body: too large"}` above `2048` bytes; `500` `{"error":"nvs: write failed"}` |
| `/api/scan` | `GET` | `admin` | `200`, `401`, `403` | Returns `{"status":"scanning"}` or `{"networks":[{ssid,rssi,secure,channel}]}` sorted strongest-first (max `20`) |
| `/api/setup-ap` | `POST` | `admin` | `200`, `401`, `403` | Opens Setup AP for `300 s` |
| `/api/setup-ap/close` | `POST` | `admin` | `200`, `400`, `401`, `403` | Closes Setup AP (`400` if no saved Wi-Fi networks) |
| `/api/reboot` | `POST` | `admin` | `200`, `401`, `403` | Schedules clean restart |
| `/api/factory-reset` | `POST` | `admin` | `200`, `401`, `403` | Clears NVS namespace `pwr_mon` and restarts |
| `/api/update` | `POST` | `admin` + password set | `200`, `400`, `401`, `403`, `409`, `500` | `multipart/form-data`, file field `firmware`; the image becomes the boot image only after the whole request has been received and verified, so a failed or interrupted upload keeps the running firmware; `200` `{"status":"ok","message":"Firmware written, rebooting"}`; `400` `{"error":"update: missing firmware file"}`; `403` `{"error":"update: set an admin password first"}`; `409` `{"error":"update: already in progress"}`; `500` `{"error":"update: <reason>"}` |

### `GET /api/status`

```json
{
  "device": "power-monitor",
  "fw_version": "1.0.0",
  "grid": "ONLINE",
  "mode": "ARMED",
  "power": true,
  "armed": true,
  "outage_duration_s": 0,
  "snooze_remaining_s": 0,
  "uptime_s": 1420,
  "wifi": {
    "connected": true,
    "ssid": "HomeWiFi",
    "slot": 0,
    "ip": "192.168.1.50",
    "rssi": -54
  },
  "setup_ap": {
    "active": false,
    "ssid": "PowerMonitor-A1B2",
    "ip": "192.168.4.1",
    "clients": 0,
    "closes_in_s": 0
  },
  "admin_protected": true,
  "snooze_default_min": 30,
  "free_heap": 198432,
  "ts": 1731668620,
  "notify": {
    "local": {
      "event": "TEST_ALARM",
      "ok": true,
      "code": 200,
      "reason": "",
      "age_s": 15
    },
    "cloud": null
  }
}
```

Each `notify.local` / `notify.cloud` slot is `null` when that target is not configured (or `cloud_enabled` is `false` for `notify.cloud`) or has not sent an event since boot/URL/enable change, otherwise `{event, ok, code, reason, age_s}`.

### `GET /api/config` and `POST /api/config`

`GET /api/config` returns the current settings without plaintext passwords:

```json
{
  "wifi": [
    {"slot": 0, "ssid": "HomeWiFi", "has_pass": true},
    {"slot": 1, "ssid": "", "has_pass": false},
    {"slot": 2, "ssid": "", "has_pass": false},
    {"slot": 3, "ssid": "", "has_pass": false},
    {"slot": 4, "ssid": "", "has_pass": false}
  ],
  "device_name": "power-monitor",
  "ap_ssid": "PowerMonitor-A1B2",
  "has_ap_pass": true,
  "setup_window_s": 60,
  "has_admin_pass": true,
  "debounce_ms": 500,
  "restore_cooldown_ms": 5000,
  "snooze_default_min": 30,
  "alert_mode": 0,
  "cloud_enabled": false,
  "webhook_url": "https://ntfy.sh/example-topic",
  "local_url": "http://192.168.1.60:8080/{event}"
}
```

Validation limits for `POST /api/config`:

| Key | Limits / rules |
|---|---|
| `wifi` | Array of objects `{slot, ssid, pass, clear_pass}`; `slot` `0–4`, `ssid` `0–32` B (empty clears the slot and its password); `pass` `8–63` B, empty or omitted keeps the stored password, `null` or `clear_pass: true` clears it (open network); changing `ssid` to a different non-empty value requires a non-empty `pass`, `pass: null`, or `clear_pass: true` (otherwise `400` `wifi[<slot>].pass: required when ssid changes`) |
| `device_name` | `1–32` chars matching `[a-z0-9-]`, no leading or trailing `-` |
| `ap_ssid` | `1–32` bytes |
| `ap_pass` | `8–63` bytes (empty or omitted keeps existing password) |
| `setup_window_s` | Integer `0–600` s |
| `admin_pass` / `clear_admin_pass` | `admin_pass` `8–63` bytes (empty/omitted keeps existing; `null` or `clear_admin_pass: true` clears; cannot send both together) |
| `debounce_ms` | Integer `50–10000` ms |
| `restore_cooldown_ms` | Integer `0–60000` ms |
| `snooze_default_min` | Integer `1–1440` min |
| `alert_mode` | `0` (`OUTAGE_ONLY`) or `1` (`OUTAGE_AND_RESTORE`) |
| `cloud_enabled` | Boolean (`false` by default); enables/disables `webhook_url` delivery while keeping the stored URL |
| `webhook_url` | Empty string, or `http://` / `https://` URL up to `256` bytes |
| `local_url` | Empty string, or `http://` / `https://` URL up to `256` bytes |

## Notification targets

- **Routing by target, `cloud_enabled`, and `alert_mode`:**
  - `local_url` receives `POWER_OFF`, `POWER_ON`, and `TEST_ALARM` regardless of `alert_mode` or `cloud_enabled`, plus `ALARM_STOP` when the mode transitions from `ARMED` to `DISARMED` or `SNOOZED` (from `BOOT` button, web page, or HTTP API) or when `SNOOZE` restarts while already `SNOOZED` (not sent when arming, and not sent on `DISARMED -> DISARMED` or `SNOOZED -> DISARMED`).
  - `webhook_url` receives events only when `cloud_enabled` is `true` and `webhook_url` is non-empty (otherwise skipped with no queue or attempt): `POWER_OFF` and `TEST_ALARM` always, `POWER_ON` only when `alert_mode` is `1` (`OUTAGE_AND_RESTORE`), and never `ALARM_STOP`.
- **Placeholder expansion:** `{event}` in either URL is replaced with lowercase `power_off`, `power_on`, `test_alarm`, or `alarm_stop`.
- **Standard HTTP/HTTPS POST:** Sends `Content-Type: application/json` with the event's JSON payload as the body (schemas below).
- **Timestamp (`ts`):** UNIX epoch seconds once NTP (`pool.ntp.org`, `time.nist.gov`) has set the clock, otherwise `0`.

### Event payloads (JSON)

#### `POWER_OFF`

```json
{
  "event": "POWER_OFF",
  "device": "power-monitor",
  "state": "ARMED",
  "grid": "OFFLINE",
  "power": false,
  "armed": true,
  "duration_s": 0,
  "uptime_s": 120,
  "ts": 1731667200
}
```

| Field | Type | Value / constraint |
|---|---|---|
| `event` | string | `"POWER_OFF"` |
| `device` | string | Configured `device_name` (`1–32` chars) |
| `state` | string | `"ARMED"` |
| `grid` | string | `"OFFLINE"` |
| `power` | boolean | `false` |
| `armed` | boolean | `true` |
| `duration_s` | integer | Elapsed outage duration in seconds (`>= 0`) |
| `uptime_s` | integer | Device uptime in seconds (`>= 0`) |
| `ts` | integer | UNIX epoch seconds or `0` |

#### `POWER_ON`

```json
{
  "event": "POWER_ON",
  "device": "power-monitor",
  "state": "ARMED",
  "grid": "ONLINE",
  "power": true,
  "armed": true,
  "duration_s": 185,
  "uptime_s": 310,
  "alert_mode": "OUTAGE_AND_RESTORE",
  "ts": 1731667385
}
```

| Field | Type | Value / constraint |
|---|---|---|
| `event` | string | `"POWER_ON"` |
| `device` | string | Configured `device_name` |
| `state` | string | `"ARMED"`, `"DISARMED"`, or `"SNOOZED"` |
| `grid` | string | `"ONLINE"` |
| `power` | boolean | `true` |
| `armed` | boolean | `true` when `state == "ARMED"`, else `false` |
| `duration_s` | integer | Completed outage duration in seconds (`>= 0`) |
| `uptime_s` | integer | Device uptime in seconds (`>= 0`) |
| `alert_mode` | string | `"OUTAGE_ONLY"` or `"OUTAGE_AND_RESTORE"` |
| `ts` | integer | UNIX epoch seconds or `0` |

#### `TEST_ALARM`

```json
{
  "event": "TEST_ALARM",
  "device": "power-monitor",
  "state": "ARMED",
  "grid": "TEST",
  "power": false,
  "armed": true,
  "duration_s": 0,
  "uptime_s": 600,
  "ts": 1731667800
}
```

| Field | Type | Value / constraint |
|---|---|---|
| `event` | string | `"TEST_ALARM"` |
| `device` | string | Configured `device_name` |
| `state` | string | `"ARMED"`, `"DISARMED"`, or `"SNOOZED"` |
| `grid` | string | `"TEST"` |
| `power` | boolean | `false` |
| `armed` | boolean | Follows current mode (`true` if `ARMED`) |
| `duration_s` | integer | `0` |
| `uptime_s` | integer | Device uptime in seconds (`>= 0`) |
| `ts` | integer | UNIX epoch seconds or `0` |

#### `ALARM_STOP` (`local_url` only)

```json
{
  "event": "ALARM_STOP",
  "device": "power-monitor",
  "state": "DISARMED",
  "snooze_remaining_s": 0,
  "uptime_s": 650,
  "ts": 1731667850
}
```

| Field | Type | Value / constraint |
|---|---|---|
| `event` | string | `"ALARM_STOP"` |
| `device` | string | Configured `device_name` (`1–32` chars) |
| `state` | string | `"DISARMED"` or `"SNOOZED"` |
| `snooze_remaining_s` | integer | Remaining snooze time in seconds (`0` when `DISARMED`) |
| `uptime_s` | integer | Device uptime in seconds (`>= 0`) |
| `ts` | integer | UNIX epoch seconds or `0` |

- **`ntfy.sh` format:** When the URL hostname is exactly `ntfy.sh`, the request sends plain-text body `Event: <EVENT>\nDevice: <name>\nDuration: <n>s` with ntfy headers:

| Event | `Title` | `Priority` | `Tags` |
|---|---|---|---|
| `POWER_OFF` | `Power Outage Alert` | `urgent` | `warning,rotating_light,zap` |
| `POWER_ON` | `Power Restored` | `default` | `white_check_mark,electric_plug` |
| `TEST_ALARM` | `Power Monitor Test` | `default` | `bell,test_tube` |

- **Retries:** Each configured target is delivered independently, from its own queue of `4` events, with up to 3 attempts (retry gaps `2 s`, then `8 s`); before each attempt it waits up to `10 s` for Wi-Fi and wakes the Wi-Fi connector from its backoff. Any HTTP `2xx` status counts as success.
- **State events (`POWER_OFF`, `POWER_ON`, `ALARM_STOP`), latest wins:** A queued or retrying state event is dropped once a newer state event exists for the same target. If a state event cannot be sent because Wi-Fi is down (when it occurs, after the `10 s` wait, or when the last attempt fails with Wi-Fi down), the target keeps it as pending and sends it when Wi-Fi reconnects, following the routing rules above; a pending `POWER_OFF` is sent only if the grid is still `OFFLINE` and the mode is `ARMED`, with the current outage duration. Events whose attempts all fail while Wi-Fi is up are not sent again.
- **Full queue:** A new state event discards the oldest queued event; a new `TEST_ALARM` is dropped and reported as `"reason": "Queue full"` in `notify` of `GET /api/status`.

## NVS storage keys

Settings are persisted in ESP32 NVS namespace `"pwr_mon"` (`CFG_VERSION = 2` in [`firmware/src/config.h`](../firmware/src/config.h)).

| Key | Type | Default | Description |
|---|---|---|---|
| `cfg_ver` | `uint8` | `2` | NVS configuration schema version (`0` if absent until first save) |
| `armed` | `bool` | `true` | Persisted arming state |
| `w0s` .. `w4s` | `string` | `""` | Saved Wi-Fi SSIDs for slots 0–4 |
| `w0p` .. `w4p` | `string` | `""` | Saved Wi-Fi passwords for slots 0–4 |
| `w_last` | `uint8` | `255` (`WIFI_SLOT_NONE`) | Last successfully connected Wi-Fi slot (`0–4`, or `255` when none/invalid) |
| `dev_name` | `string` | `"power-monitor"` | Device name and mDNS hostname |
| `ap_ssid` | `string` | `PowerMonitor-XXXX` | Setup AP SSID (`XXXX` = last 2 eFuse MAC bytes) |
| `ap_pass` | `string` | `"12345678"` | Setup AP password |
| `ap_win` | `uint16` | `60` | Boot Setup AP window (`0–600` s) |
| `adm_p` | `string` | `""` | Admin Basic Auth password |
| `cloud_en` | `bool` | `false` | Enable cloud webhook (`webhook_url`) delivery |
| `webhook` | `string` | `""` | Cloud webhook URL |
| `local_url` | `string` | `""` | Local HTTP notify URL template |
| `debounce` | `uint32` | `500` | Grid sense debounce (`ms`) |
| `cooldown` | `uint32` | `5000` | Grid restore cooldown (`ms`) |
| `snooze` | `uint16` | `30` | Default snooze duration (`min`) |
| `alert_mode` | `uint8` | `0` | Alert tracking mode (`0` or `1`) |

## JSON schemas

Draft 2020-12 schemas live under `docs/schemas/`:

- [`POWER_OFF.schema.json`](schemas/POWER_OFF.schema.json)
- [`POWER_ON.schema.json`](schemas/POWER_ON.schema.json)
- [`ALARM_STOP.schema.json`](schemas/ALARM_STOP.schema.json)
- [`TEST_ALARM.schema.json`](schemas/TEST_ALARM.schema.json)
- [`api_status.schema.json`](schemas/api_status.schema.json)
- [`api_config.schema.json`](schemas/api_config.schema.json)
