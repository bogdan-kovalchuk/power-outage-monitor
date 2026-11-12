# Power Outage Monitor

Alerts you within seconds on an Android phone when mains power drops, so you can act on whatever depends on it. An ESP32 monitors mains via a 12 V wall adapter stepped down through an LM2596 module to `3.1 V` on `GPIO 4` while the ESP32 and Wi-Fi router stay on a Mini-UPS, sending local Wi-Fi HTTP alerts to MacroDroid with optional `ntfy.sh` cloud backup, PlatformIO firmware, a host verification suite, and a 3D-printable enclosure.

## Quick start

1. **Wire and calibrate:** Power the LM2596 from the `12 V` mains adapter and adjust its trimmer with a multimeter to `3.0–3.2 V` **before** connecting `OUT+` through a `1–4.7 kOhm` resistor to `GPIO 4` (`OUT-` to `GND`). Plug both the ESP32 and your Wi-Fi router into the Mini-UPS.
2. **Reserve fixed IPs:** In your router's DHCP settings, reserve static LAN IP addresses for both the ESP32 and the phone (`power-monitor.local` mDNS does not resolve reliably on Android).
3. **Set up the phone:** Follow [`docs/phone-setup.md`](docs/phone-setup.md) to configure the `Power Outage Alarm` macro in MacroDroid Pro (`power_off`, `power_on`, `alarm_stop`, `test_alarm` on port `8080`, full-screen `"Power is OFF"` stop screen, repeating `Alarm`-stream siren, `Override Do Not Disturb`), optionally subscribe to a random `ntfy.sh` topic for away-from-home insistent alerts, and set battery usage to **Unrestricted** for both apps.
4. **Flash and join Wi-Fi:** Build and upload `firmware/` (hold `BOOT` while `esptool` connects on first USB flash before closing the case), connect to `PowerMonitor-XXXX` (password `12345678`, `http://192.168.4.1/`), and save your home Wi-Fi credentials.
5. **Link and test:** Open `http://<esp32-ip>/`, expand **Device Settings**, set **Local Notify URL (Phone)** to `http://<phone-ip>:8080/{event}` and (for away-from-home backup) **Cloud Webhook URL** to `https://ntfy.sh/<your-topic>`, keep **Internet alerts (ntfy)** **Off** while at home, save, and press **Test Alarm** – confirm the page shows `Phone: OK`, `Cloud: off`, and the `"Power monitor test received"` test notification arrives without the siren (normal notification sound only).

## Features

- Debounced mains sensing on `GPIO 4` with continuous-HIGH restore cooldown (`max(debounce_ms, restore_cooldown_ms)`) against grid flapping
- Three modes (`ARMED`, `DISARMED`, `SNOOZED`) via web dashboard, HTTP API, or `BOOT` button (when case is open)
- Local HTTP alerts (`power_off`, `power_on`, `test_alarm`, `alarm_stop`) plus `ntfy.sh` / HTTPS webhook backup (`alert_mode`: `0` outage-only or `1` outage-and-restore)
- Web dashboard with live mains state, `Phone` / `Cloud` last delivery status, primary Wi-Fi setup, **Setup Access & Maintenance**, password-protected OTA **Firmware Update**, and a collapsed **Advanced** section for tuning knobs and backup Wi-Fi slots 2–5
- Parametric 3-part 3D-printed enclosure (`126.0 x 48.0 x 28.0 mm`) with cable strain-relief clamp

## Project structure

| Path | Purpose |
|---|---|
| `firmware/` | PlatformIO ESP32 firmware (`src/`, `test/`, `host/`, `scripts/`, `certs/`, `data/cert/`, `platformio.ini`) |
| [`hardware/enclosure/`](hardware/enclosure/README.md) | SolidWorks CAD (`cad/`), `step/`, print-ready `stl/`, and mesh/build `tools/` |
| [`tools/`](tools/sim/README.md) | Bench simulator (`mobile_alarm_sim.py`) and host verification suite (`sim/`) |
| `docs/` | [Protocol reference](docs/protocol.md), [phone setup guide](docs/phone-setup.md), JSON `schemas/`, and renders (`img/`) |
| `.github/workflows/ci.yml` | CI workflow running native unit tests and the `esp32dev` firmware build |
| [`LICENSE`](LICENSE) | MIT license |

## Hardware

### Bill of materials

| Component | Qty | Notes |
|---|---:|---|
| ESP32 38-pin DevKit V1 (USB-C, CP2102) | 1 | Powered continuously from the router Mini-UPS USB port (`5 V`) |
| LM2596 DC-DC step-down module | 1 | Calibrated to `3.0–3.2 V` on `OUT+` / `OUT-` |
| 12 V DC wall power adapter | 1 | Certified adapter plugged into the monitored non-UPS mains outlet |
| Series resistor, `1–4.7 kOhm` (`0.25 W`) | 1 | Current-limiting protection between LM2596 `OUT+` and `GPIO 4` |
| 2-wire DC cable (`4.0 x 2.0 mm` flat) | 1 | Feeds `12 V` from the adapter to LM2596 `IN+` / `IN-` |
| M3 self-tapping screws | 8 | 4x M3 x 8–12 mm (lid), 2x M3 x 6 mm (clamp), 2x M3 x 5–6 mm (LM2596) |
| Router Mini-UPS | 1 | Powers both the ESP32 and the Wi-Fi router during outages |

### Wiring & safety

| From | To | Function |
|---|---|---|
| Router Mini-UPS USB (`5 V`) | ESP32 USB-C port | Uninterruptible controller power |
| 12 V wall adapter (`+` / `-`) | LM2596 `IN+` / `IN-` | Monitored mains indicator input / return |
| LM2596 `OUT-` | ESP32 `GND` | Common ground reference |
| LM2596 `OUT+` (`3.0–3.2 V`) | `1–4.7 kOhm` resistor -> ESP32 `GPIO 4` | Digital mains sense (`INPUT_PULLDOWN`) |

Only the `12 V` DC secondary side enters the enclosure; never route AC mains inside the case. Always calibrate LM2596 `OUT+` to `3.0–3.2 V` (never above `3.3 V`) before wiring `GPIO 4`. Print files and assembly steps are in [`hardware/enclosure/README.md`](hardware/enclosure/README.md).

![Power Outage Monitor enclosure](docs/img/assembly_closed_isometric.png)

## Firmware

### Build and first flash

Requires [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html) (or VS Code extension), CP2102 USB-UART drivers, and Python 3.11+ for host tools:

```bash
pio run -d firmware -e esp32dev
pio run -d firmware -e esp32dev -t upload
pio device monitor -d firmware -b 115200
```

On the 38-pin CP2102 USB-C DevKit, auto-reset may not pull `GPIO 0` low in time on some hosts; hold `BOOT` while `esptool` connects on the first USB flash, then release once upload starts. Inside a cloud-synced folder (such as Google Drive), set `PLATFORMIO_WORKSPACE_DIR` to a local directory outside the synced tree.

### Web UI, security, and OTA updates

After saving home Wi-Fi via `PowerMonitor-XXXX` (`12345678`, `http://192.168.4.1/`), open **Setup Access & Maintenance** to set an **Admin Password** (HTTP Basic auth user `admin`) and change the public default **Setup AP Password**. Once an admin password is set, subsequent firmware `.bin` updates can be uploaded over Wi-Fi from the **Firmware Update** card (`POST /api/update`). The new image boots only after the whole upload has been received and verified; a failed or interrupted upload keeps the running firmware.

## Configuration

Core settings live under **Wi-Fi Network**, **Device Settings**, and **Setup Access & Maintenance**; optional tuning knobs and backup Wi-Fi slots 2–5 are collapsed under **Advanced**.

| Setting | Default | Limits / format | UI section |
|---|---|---|---|
| `wifi[0..4]` | Empty | 5 slots; SSID `0–32` B (empty clears), pass `8–63` B or open | Wi-Fi / Advanced |
| `local_url` | Empty | Empty or `http://` / `https://`, max `256` B | Device Settings |
| `cloud_enabled` | `false` | Boolean (`true` = send to `webhook_url`, `false` = skip cloud) | Device Settings |
| `webhook_url` | Empty | Empty or `http://` / `https://`, max `256` B | Device Settings |
| `alert_mode` | `0` | `0` = `OUTAGE_ONLY`, `1` = `OUTAGE_AND_RESTORE` (cloud) | Device Settings |
| `admin_pass` | Empty | `8–63` B; enables HTTP Basic auth (`admin`) and OTA | Setup Access |
| `ap_ssid` / `ap_pass` | `PowerMonitor-XXXX` / `12345678` | SSID `1–32` B; pass `8–63` B | Setup Access |
| `setup_window_s` | `60` | `0–600` s (`0` disables boot AP window) | Setup Access |
| `device_name` | `power-monitor` | `1–32` chars `[a-z0-9-]`, no leading/trailing `-` | Advanced (optional) |
| `debounce_ms` / `restore_cooldown_ms` | `500` / `5000` | `50–10000` ms / `0–60000` ms | Advanced (optional) |
| `snooze_default_min` | `30` | `1–1440` min | Advanced (optional) |

## Alert channels

- **Local notify URL (`local_url`):** Sends HTTP `POST` to a LAN listener (MacroDroid at `http://<phone-ip>:8080/{event}`) for `POWER_OFF`, `POWER_ON`, `TEST_ALARM`, and `ALARM_STOP` (on disarm/snooze from `ARMED` or snooze restart), replacing `{event}` with lowercase event names. Needs same Wi-Fi and fixed DHCP IPs.
- **Cloud webhook (`webhook_url`, gated by `cloud_enabled`):** Off by default at home so only the LAN phone alert runs; switch **Internet alerts (ntfy)** (`cloud_enabled`) **On** only when away from home. When enabled and `webhook_url` is set, sends HTTPS/HTTP `POST` for `POWER_OFF` and `TEST_ALARM` always, and `POWER_ON` (informational) only when `alert_mode` is `1`. When `cloud_enabled` is `false`, cloud delivery is skipped completely (preserving the saved URL). Host `ntfy.sh` uses ntfy headers and plain text, alerting independently through the ntfy app's insistent highest-priority notification (not MacroDroid); other hosts receive JSON.
- **Delivery:** Each target gets up to 3 attempts per event. If Wi-Fi is down, the latest undelivered `POWER_OFF`, `POWER_ON` or `ALARM_STOP` is sent as soon as Wi-Fi reconnects (a `POWER_OFF` only while the outage lasts and the unit is `ARMED`); older ones are dropped (latest wins).

See [`docs/phone-setup.md`](docs/phone-setup.md) and [`docs/protocol.md`](docs/protocol.md) for details.

## Before you sleep

1. **ESP32 LED is solid ON**: `ARMED`, grid `ONLINE`, Setup AP off (the LED does not show Wi-Fi; item 3 does). Router and ESP32 on the Mini-UPS, `12 V` adapter in a non-UPS outlet.
2. **Phone is on home Wi-Fi and charging**, with `Power Outage Alarm` enabled (and **Internet alerts (ntfy)** **Off** on the device page while at home; turn it **On** only when away).
3. **Device page shows `Phone: OK`**: the ESP32 is on Wi-Fi and its last alert reached the phone (if unsure, tap **Test Alarm** on the page for a test notification without the siren – normal notification sound only).
4. **Know where the Stop button is:** when the siren fires, tap **`Stop alarm`** on the full-screen `"Power is OFF"` screen or on the standalone alarm notification (or tap **Disarm** / **Snooze** on the device page; swiping the notification away does **not** stop the siren on Android 14+).

## Button and LED

### Physical `BOOT` button (`GPIO 0`, active LOW, 50 ms debounce, action on release)

The `BOOT` button sits inside the closed enclosure and is **not reachable** in normal use. Use the web dashboard (**Arm**, **Disarm**, **Snooze**, **Test Alarm**, **Open setup AP for 5 min**, **Factory reset**) unless the lid is open.

| Hold duration | Action |
|---|---|
| `50 ms – 2 s` | Toggle `ARMED` / `DISARMED` |
| `2 – 5 s` | Send `TEST_ALARM` and reconnect Wi-Fi if offline |
| `5 – 10 s` | Open Setup AP for 5 min (`300 s`) |
| `>= 10 s` | Factory reset (`pwr_mon` NVS wipe) and reboot |

### Status LED (`GPIO 2`, priority: button hold `>= 5 s` > grid `OFFLINE` > Setup AP > mode)

| Pattern | Timing | Meaning |
|---|---|---|
| Solid ON | Continuous HIGH | `ARMED`, grid `ONLINE`, Setup AP off |
| Slow blink | `1 Hz` (`500 ms` ON / `500 ms` OFF) | `DISARMED`, grid `ONLINE`, Setup AP off |
| Double blink | 2x `100 ms` pulses in `1.0 s` cycle | `SNOOZED`, grid `ONLINE`, Setup AP off |
| Rapid strobe | `5 Hz` (`100 ms` ON / `100 ms` OFF) | Grid `OFFLINE` (power outage active) |
| Setup AP | 3x `100 ms` flashes in `1.2 s` cycle | Setup AP active (or `BOOT` held `5–10 s`) |
| Factory wipe | `10 Hz` (`50 ms` ON / `50 ms` OFF) | `BOOT` held `>= 10 s` |

## Tests

```bash
pio test -d firmware -e native
python tools/sim/run_all.py
```

On Windows with Smart App Control enabled, locally compiled unsigned native binaries are blocked; run `pio test -d firmware -e native` inside WSL Ubuntu (CI runs on Linux). See [`tools/sim/README.md`](tools/sim/README.md) for verification layers L0–L3 and the LAN mock device.

## Limitations

- Control endpoints (`/api/arm`, `/api/disarm`, `/api/toggle`, `/api/snooze`, `/api/test`) are unauthenticated on the LAN by design so phone actions and widgets work without credentials.
- The `2048`-byte request limit on `POST /api/config` is checked after the web server has read the body, so it protects JSON parsing rather than memory used while reading the request.
- The initial Setup AP password (`12345678`) is public; change it and set an admin password during first setup.
- HTTPS notification targets verify server certificates against an embedded Mozilla root CA bundle (`firmware/data/cert/x509_crt_bundle.bin`); refresh the bundle from an updated `certifi` package using [`firmware/certs/README.md`](firmware/certs/README.md).
- On Android 14+, swiping away the MacroDroid alarm notification does not stop the siren; use the **`Stop alarm`** button on the full-screen `"Power is OFF"` screen or notification, or **Disarm** / **Snooze** on the device page.
- Local HTTP phone alerts require both devices on the home Wi-Fi with static DHCP IPs (`power-monitor.local` mDNS did not resolve on the tested Android phone); remote `ntfy` alerts require **Internet alerts (ntfy)** switched on and are silenced independently by dismissing the `ntfy` notification.
- Outage reporting depends on the router Mini-UPS keeping both the ESP32 and the Wi-Fi router powered during a mains cut.
- There is no remote dead-man alert if the ESP32 itself loses UPS power.
- When both notification URLs use host names (rather than IP addresses) or both use `https://`, their delivery attempts run one at a time, so an unreachable host can delay the other target by up to `39 s` per attempt.

## License

[MIT](LICENSE), except `firmware/data/cert/x509_crt_bundle.bin` (generated from the Mozilla CA certificate list, MPL-2.0; see [`firmware/certs/README.md`](firmware/certs/README.md)).
