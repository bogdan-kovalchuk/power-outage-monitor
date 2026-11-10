# Hardware-Free Simulation & Verification Suite

This directory implements verification layers L0 through L3 for Power Outage Monitor, plus a mock device (`mock_device.py`) for browser and LAN phone testing.

## Verification Layers

| Layer | Scope | Entry point | Requirement |
| --- | --- | --- | --- |
| L0 | Clean `esp32dev` build (0 `src/` warnings), `web_ui.h` single `)rawliteral"` + `node --check`, ES2017 JS ceiling check, zero `U+2014` em dashes across repository text files | `python tools/sim/run_all.py --layers L0` | PlatformIO CLI (`pio` or `PIO`), Node.js |
| L1 | Host C++ Unity tests (`test_config`, `test_power_proto`, `test_wifi_ap`) | `python tools/sim/run_all.py --layers L1` | PlatformIO + GCC/MinGW (`g++`) |
| L2 | Draft 2020-12 JSON schema checks, firmware host payload dump (`payload_dump`), `tools/mobile_alarm_sim.py` payloads, live mock HTTP/target/guard probes, `config.h` constants parity, `main.cpp` + `ota_update.cpp` route parity, `network_mgr.cpp` ntfy literal parity, and `config_cases.json` validation parity against `validateAndMergeConfig` | `python tools/sim/run_all.py --layers L2` | Python 3 + `jsonschema` + PlatformIO |
| L3 | Browser UI tests (`test_web_ui.py`) against `mock_device.py` serving `firmware/src/web_ui.h` (status states, Phone/Cloud notify status line, controls/409 snooze, card saves including collapsed `Advanced` card, Wi-Fi CRUD/UTF-8 check, scan & timeout, OTA gate/upload, AP controls, confirm dialogs, captive-portal AP host) | `python tools/sim/run_all.py --layers L3` | `pytest`, `playwright`, and Playwright Chromium (or `POM_BROWSER_CHANNEL`) |

## Setup & Running

Install the Python test dependencies into a virtual environment outside synced folders:

```bash
python -m pip install -r tools/sim/requirements.txt
```

Run all layers (or select specific layers with `--layers`):

```bash
python tools/sim/run_all.py
python tools/sim/run_all.py --layers L0,L2
```

Environment variables:

- `PIO` – optional path to the PlatformIO CLI executable (otherwise resolved from `PATH` or the standard PlatformIO virtualenv).
- `PLATFORMIO_WORKSPACE_DIR` – respected by PlatformIO to keep `.pio` build outputs outside synced project folders.
- `POM_BROWSER_CHANNEL` – Playwright browser channel for L3 (defaults to Playwright Chromium when unset; set e.g. `msedge` or `chrome` to use an installed system browser).

> **Windows note:** Host test binaries built by PlatformIO (`L1` and the `L2` `[env:payloads]` executable) require an environment that allows running locally compiled unsigned executables. On Windows machines with Smart App Control enabled, run `L1` and `L2` inside WSL.

## Mock Device (`mock_device.py`)

`tools/sim/mock_device.py` uses Python stdlib only and extracts `INDEX_HTML` directly from `firmware/src/web_ui.h` at start-up.

Start in LAN mode (binds `0.0.0.0`):

```bash
python tools/sim/mock_device.py --lan --port 8080
```

Test-control endpoints exposed by the mock device:

- `GET /__requests` – returns logged inbound HTTP requests and outbound `local_url` / `webhook_url` notification attempts (`outbound`, `method`, `path`/`url`, `headers`, `body`, `json`, `status`).
- `POST /__reset` – clears the request log and restores default state; optional JSON body applies `/__state` overrides atomically (400 on malformed JSON or unknown keys).
- `POST /__state` – merges partial state overrides (`config`, `mode`, `snooze_remaining_s`, `grid` (`booting`|`online`|`offline`), `outage_duration_s`, `wifi`, `setup_ap`, `scan_networks`, `scan_pending_polls`, `scan_hold`, `force_error`, `clear_requests`, `update_in_progress`, `ntp_synced`, `notify`). Returns 400 on malformed JSON or unknown keys.
- `POST /__grid` – sets grid state (`{"grid": "booting"|"online"|"offline"}`). Triggers `POWER_OFF` (when `ARMED`) or `POWER_ON` (when recovering from `OFFLINE`) notification target requests.

## Mobile Alarm Bench Utility (`tools/mobile_alarm_sim.py`)

`tools/mobile_alarm_sim.py` simulates local HTTP target webhooks and `ntfy.sh` pushes:

```bash
python tools/mobile_alarm_sim.py http power_off --target "http://192.168.1.60:8080/{event}"
python tools/mobile_alarm_sim.py ntfy test_alarm --topic my-topic
```
