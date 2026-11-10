#!/usr/bin/env python3
"""L2 contract verification: schemas, firmware payloads, live mock, mobile tool, constants, and routes."""

import base64
import http.server
import json
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import urllib.error
import urllib.request

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(THIS_DIR, ".."))
REPO_ROOT = os.path.abspath(os.path.join(THIS_DIR, "..", ".."))

for _p in (THIS_DIR, TOOLS_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import mobile_alarm_sim  # noqa: E402
import mock_device  # noqa: E402

SCHEMA_FILES = (
    "POWER_OFF.schema.json",
    "POWER_ON.schema.json",
    "ALARM_STOP.schema.json",
    "TEST_ALARM.schema.json",
    "api_status.schema.json",
    "api_config.schema.json",
)


def find_pio():
    """Locate PlatformIO CLI portably without machine-specific paths."""
    env_pio = os.environ.get("PIO", "").strip()
    if env_pio:
        if os.path.isfile(env_pio) or shutil.which(env_pio):
            return env_pio, ""
        return None, f"PIO environment variable points to missing executable: {env_pio}"

    which_pio = shutil.which("pio")
    if which_pio:
        return which_pio, ""

    if os.name == "nt":
        penv_pio = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "pio.exe")
    else:
        penv_pio = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "bin", "pio")

    if os.path.isfile(penv_pio):
        return penv_pio, ""

    return None, "pio not found; install PlatformIO, add it to PATH, or set PIO"


def _load_jsonschema():
    try:
        import jsonschema
        from jsonschema import Draft202012Validator
        return jsonschema, Draft202012Validator, ""
    except ImportError as exc:
        return None, None, f"jsonschema package unavailable ({exc}); install tools/sim/requirements.txt"


def _validate_against_schema(instance, schema_path, validator_cls, jsonschema_mod):
    try:
        with open(schema_path, "r", encoding="utf-8") as sf:
            schema = json.load(sf)
    except Exception as exc:
        return [f"failed to load schema {os.path.basename(schema_path)}: {exc}"]

    try:
        validator_cls.check_schema(schema)
    except jsonschema_mod.SchemaError as exc:
        return [f"invalid Draft 2020-12 schema {os.path.basename(schema_path)}: {exc.message}"]

    validator = validator_cls(schema)
    errs = []
    for err in sorted(validator.iter_errors(instance), key=lambda e: list(e.path)):
        path = "$" + "".join(f"[{p!r}]" if isinstance(p, int) else f".{p}" for p in err.path)
        errs.append(f"{path}: {err.message}")
    return errs


def check_schemas_metaschema():
    jsonschema_mod, validator_cls, imp_err = _load_jsonschema()
    if imp_err:
        return [imp_err]

    errors = []
    schemas_dir = os.path.join(REPO_ROOT, "docs", "schemas")
    for fname in SCHEMA_FILES:
        sp = os.path.join(schemas_dir, fname)
        if not os.path.isfile(sp):
            errors.append(f"missing schema file: docs/schemas/{fname}")
            continue
        try:
            with open(sp, "r", encoding="utf-8") as sf:
                schema = json.load(sf)
            validator_cls.check_schema(schema)
        except jsonschema_mod.SchemaError as exc:
            errors.append(f"{fname}: Draft 2020-12 metaschema error: {exc.message}")
        except Exception as exc:
            errors.append(f"{fname}: failed to parse JSON: {exc}")
    return errors


def _run_payload_dump_to_temp():
    pio_exe, pio_err = find_pio()
    if not pio_exe:
        return None, pio_err
    payload_dir = tempfile.mkdtemp(prefix="pom_l2_")
    cases_path = os.path.join(THIS_DIR, "config_cases.json")
    try:
        proc = subprocess.run(
            [pio_exe, "run", "-d", os.path.join(REPO_ROOT, "firmware"), "-e", "payloads", "-t", "exec", "-a", payload_dir, "-a", cases_path],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        shutil.rmtree(payload_dir, ignore_errors=True)
        return None, f"failed to run pio payloads exec: {exc}"
    if proc.returncode != 0:
        shutil.rmtree(payload_dir, ignore_errors=True)
        tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
        return None, f"pio run -e payloads -t exec failed ({proc.returncode}): {' | '.join(tail)}"
    return payload_dir, None


def check_firmware_payloads(payload_dir=None, payload_exec_error=None):
    if payload_exec_error:
        return [payload_exec_error]

    jsonschema_mod, validator_cls, imp_err = _load_jsonschema()
    if imp_err:
        return [imp_err]

    errors = []
    created_tmp = False
    if payload_dir is None:
        payload_dir, exec_err = _run_payload_dump_to_temp()
        if exec_err:
            return [exec_err]
        created_tmp = True

    try:
        schemas_dir = os.path.join(REPO_ROOT, "docs", "schemas")
        pairs = (
            ("POWER_OFF.json", "POWER_OFF.schema.json"),
            ("POWER_ON.json", "POWER_ON.schema.json"),
            ("ALARM_STOP.json", "ALARM_STOP.schema.json"),
            ("TEST_ALARM.json", "TEST_ALARM.schema.json"),
            ("status.json", "api_status.schema.json"),
            ("status_booting.json", "api_status.schema.json"),
            ("config.json", "api_config.schema.json"),
        )
        for json_name, schema_name in pairs:
            jp = os.path.join(payload_dir, json_name)
            sp = os.path.join(schemas_dir, schema_name)
            if not os.path.isfile(jp):
                errors.append(f"missing dumped payload {json_name}")
                continue
            try:
                with open(jp, "r", encoding="utf-8") as jf:
                    instance = json.load(jf)
            except Exception as exc:
                errors.append(f"{json_name}: invalid JSON: {exc}")
                continue
            for e in _validate_against_schema(instance, sp, validator_cls, jsonschema_mod):
                errors.append(f"{json_name}: {e}")

        for name in ("status.json", "config.json"):
            jp = os.path.join(payload_dir, name)
            if os.path.isfile(jp):
                raw = open(jp, "r", encoding="utf-8").read()
                for secret in ("secretpass", "admsecret", "12345678"):
                    if secret in raw:
                        errors.append(f"{name}: leaked secret substring {secret!r}")
                if name == "status.json" and ("webhook" in raw or "local_url" in raw):
                    errors.append("status.json: must not contain notification URLs")
    finally:
        if created_tmp:
            shutil.rmtree(payload_dir, ignore_errors=True)
    return errors


def check_mobile_alarm_payloads():
    jsonschema_mod, validator_cls, imp_err = _load_jsonschema()
    if imp_err:
        return [imp_err]

    errors = []
    schemas_dir = os.path.join(REPO_ROOT, "docs", "schemas")
    cases = (
        ("POWER_OFF.schema.json", mobile_alarm_sim.build_payload("POWER_OFF", now=1700000100)),
        ("POWER_ON.schema.json", mobile_alarm_sim.build_payload("POWER_ON", alert_mode="OUTAGE_AND_RESTORE", now=1700000200)),
        ("POWER_ON.schema.json", mobile_alarm_sim.build_payload("POWER_ON", alert_mode="OUTAGE_ONLY", now=1700000201)),
        ("TEST_ALARM.schema.json", mobile_alarm_sim.build_payload("TEST_ALARM", state="ARMED", now=1700000500)),
        ("TEST_ALARM.schema.json", mobile_alarm_sim.build_payload("TEST_ALARM", state="DISARMED", now=1700000501)),
    )
    for schema_name, payload in cases:
        sp = os.path.join(schemas_dir, schema_name)
        for e in _validate_against_schema(payload, sp, validator_cls, jsonschema_mod):
            errors.append(f"mobile_alarm_sim({payload['event']}): {e}")
    return errors


def _http_request(url, method="GET", headers=None, body_bytes=None):
    req = urllib.request.Request(url, data=body_bytes, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read()


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _http_request_no_redirect(url, headers=None):
    opener = urllib.request.build_opener(_NoRedirectHandler())
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    try:
        with opener.open(req, timeout=3.0) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read()


def check_live_mock_contracts():
    """Start in-process mock on ephemeral ports; probe routes/guards, HTTP payloads, and targets."""
    jsonschema_mod, validator_cls, imp_err = _load_jsonschema()
    if imp_err:
        return [imp_err]

    errors = []
    schemas_dir = os.path.join(REPO_ROOT, "docs", "schemas")

    received_hooks = []
    hook_lock = threading.Lock()

    class _HookHandler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def do_POST(self):
            clen = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(clen).decode("utf-8", errors="replace") if clen > 0 else ""
            with hook_lock:
                received_hooks.append({"path": self.path, "body": body})
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")

    class _HookServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    hook_srv = _HookServer(("127.0.0.1", 0), _HookHandler)
    hook_port = hook_srv.server_address[1]
    hook_thread = threading.Thread(target=hook_srv.serve_forever, daemon=True)
    hook_thread.start()

    srv = mock_device.MockDeviceServer(host="127.0.0.1", port=0, lan=False)
    srv.start()

    try:
        base = srv.base_url

        # 1. Live HTTP responses against schemas (BOOTING and ONLINE status + config)
        st, _, body = _http_request(f"{base}/__state", "POST", {"Content-Type": "application/json"}, b'{"grid":"booting"}')
        if st != 200:
            errors.append(f"mock /__state booting returned {st}")
        st, _, body = _http_request(f"{base}/api/status")
        if st != 200:
            errors.append(f"mock GET /api/status (BOOTING) returned {st}")
        else:
            status_obj = json.loads(body.decode("utf-8"))
            for e in _validate_against_schema(status_obj, os.path.join(schemas_dir, "api_status.schema.json"), validator_cls, jsonschema_mod):
                errors.append(f"mock GET /api/status (BOOTING): {e}")

        _http_request(f"{base}/__state", "POST", {"Content-Type": "application/json"}, b'{"grid":"online"}')
        st, _, body = _http_request(f"{base}/api/status")
        if st == 200:
            status_obj = json.loads(body.decode("utf-8"))
            for e in _validate_against_schema(status_obj, os.path.join(schemas_dir, "api_status.schema.json"), validator_cls, jsonschema_mod):
                errors.append(f"mock GET /api/status (ONLINE): {e}")
            if status_obj.get("notify") != {"local": None, "cloud": None}:
                errors.append(f"mock initial notify mismatch: {status_obj.get('notify')!r}")

        st, _, body = _http_request(f"{base}/api/config", "GET", {"X-Requested-With": "pm-ui"})
        if st != 200:
            errors.append(f"mock GET /api/config returned {st}")
        else:
            cfg_obj = json.loads(body.decode("utf-8"))
            for e in _validate_against_schema(cfg_obj, os.path.join(schemas_dir, "api_config.schema.json"), validator_cls, jsonschema_mod):
                errors.append(f"mock GET /api/config: {e}")
            if cfg_obj.get("cloud_enabled") is not False:
                errors.append(f"mock default cloud_enabled={cfg_obj.get('cloud_enabled')!r}, expected False")

        def _drain_hooks():
            srv.state.wait_for_notifications(timeout=2.0)
            with hook_lock:
                items = list(received_hooks)
                received_hooks.clear()
            return items

        # 2a. Cloud off by default: webhook_url configured, cloud_enabled=False -> local only, cloud skipped & null
        _http_request(
            f"{base}/__state",
            "POST",
            {"Content-Type": "application/json"},
            json.dumps({
                "config": {
                    "alert_mode": 1,
                    "cloud_enabled": False,
                    "local_url": f"http://127.0.0.1:{hook_port}/local/{{event}}",
                    "webhook_url": f"http://127.0.0.1:{hook_port}/cloud/{{event}}",
                }
            }).encode("utf-8"),
        )
        _http_request(f"{base}/api/test", "POST")
        hooks_off = _drain_hooks()
        if [h["path"] for h in hooks_off] != ["/local/test_alarm"]:
            errors.append(f"mock cloud_enabled=False TEST_ALARM hooks={[h['path'] for h in hooks_off]}, expected ['/local/test_alarm']")
        _, _, st_off_body = _http_request(f"{base}/api/status")
        st_off = json.loads(st_off_body.decode("utf-8"))
        if (st_off.get("notify") or {}).get("cloud") is not None:
            errors.append(f"mock notify.cloud while cloud_enabled=False must be None: {st_off.get('notify')!r}")

        # 2b. Live notification targets with cloud_enabled=True
        _http_request(
            f"{base}/__state",
            "POST",
            {"Content-Type": "application/json"},
            json.dumps({
                "config": {
                    "alert_mode": 0,
                    "cloud_enabled": True,
                    "local_url": f"http://127.0.0.1:{hook_port}/local/{{event}}",
                    "webhook_url": f"http://127.0.0.1:{hook_port}/cloud/{{event}}",
                }
            }).encode("utf-8"),
        )

        def _validate_hook_bodies(hooks, expected_path, schema_name, label):
            matched = [h for h in hooks if h["path"] == expected_path]
            if not matched:
                errors.append(f"mock {label} missing hook {expected_path}: got {[h['path'] for h in hooks]}")
                return
            for idx, h in enumerate(matched):
                try:
                    obj = json.loads(h["body"])
                except json.JSONDecodeError:
                    errors.append(f"mock {label}[{idx}] body is not JSON: {h['body']!r}")
                    continue
                for e in _validate_against_schema(obj, os.path.join(schemas_dir, schema_name), validator_cls, jsonschema_mod):
                    errors.append(f"mock {label}[{idx}]: {e}")

        # POWER_OFF via /__grid offline
        st, _, _ = _http_request(f"{base}/__grid", "POST", {"Content-Type": "application/json"}, b'{"grid":"offline"}')
        if st != 200:
            errors.append(f"mock /__grid offline returned {st}")
        hooks = _drain_hooks()
        _validate_hook_bodies(hooks, "/local/power_off", "POWER_OFF.schema.json", "HTTP local POWER_OFF")
        _validate_hook_bodies(hooks, "/cloud/power_off", "POWER_OFF.schema.json", "HTTP cloud POWER_OFF")

        # POWER_ON via /__grid online (alert_mode=0 -> local_url gets it, webhook_url does not)
        _http_request(f"{base}/__grid", "POST", {"Content-Type": "application/json"}, b'{"grid":"online"}')
        hooks = _drain_hooks()
        _validate_hook_bodies(hooks, "/local/power_on", "POWER_ON.schema.json", "HTTP local POWER_ON")
        if any(h["path"] == "/cloud/power_on" for h in hooks):
            errors.append(f"mock POWER_ON alert_mode=0 leaked to cloud: got {[h['path'] for h in hooks]}")

        # TEST_ALARM via /api/test
        _http_request(f"{base}/api/test", "POST")
        hooks = _drain_hooks()
        _validate_hook_bodies(hooks, "/local/test_alarm", "TEST_ALARM.schema.json", "HTTP local TEST_ALARM")
        _validate_hook_bodies(hooks, "/cloud/test_alarm", "TEST_ALARM.schema.json", "HTTP cloud TEST_ALARM")

        _, _, st_body = _http_request(f"{base}/api/status")
        st_after_test = json.loads(st_body.decode("utf-8"))
        for e in _validate_against_schema(st_after_test, os.path.join(schemas_dir, "api_status.schema.json"), validator_cls, jsonschema_mod):
            errors.append(f"mock GET /api/status after TEST_ALARM: {e}")
        n_local = (st_after_test.get("notify") or {}).get("local")
        n_cloud = (st_after_test.get("notify") or {}).get("cloud")
        if not n_local or n_local.get("event") != "TEST_ALARM" or n_local.get("ok") is not True or n_local.get("code") != 200:
            errors.append(f"mock notify.local after TEST_ALARM unexpected: {n_local!r}")
        if not n_cloud or n_cloud.get("event") != "TEST_ALARM" or n_cloud.get("ok") is not True or n_cloud.get("code") != 200:
            errors.append(f"mock notify.cloud after TEST_ALARM unexpected: {n_cloud!r}")

        def _expect_alarm_stop_hook(label, expected_state, expect_snooze_nonzero=False):
            hooks = _drain_hooks()
            paths = [h["path"] for h in hooks]
            if paths != ["/local/alarm_stop"]:
                errors.append(f"mock {label} ALARM_STOP hooks={paths}, expected ['/local/alarm_stop']")
                return
            try:
                body_obj = json.loads(hooks[0]["body"])
            except json.JSONDecodeError:
                errors.append(f"mock {label} ALARM_STOP body is not JSON: {hooks[0]['body']!r}")
                return
            for e in _validate_against_schema(body_obj, os.path.join(schemas_dir, "ALARM_STOP.schema.json"), validator_cls, jsonschema_mod):
                errors.append(f"mock {label} ALARM_STOP: {e}")
            if body_obj.get("state") != expected_state:
                errors.append(f"mock {label} ALARM_STOP state={body_obj.get('state')!r}, expected {expected_state!r}")
            rem = body_obj.get("snooze_remaining_s", 0)
            if expect_snooze_nonzero and rem <= 0:
                errors.append(f"mock {label} ALARM_STOP snooze_remaining_s={rem}, expected > 0")
            if not expect_snooze_nonzero and rem != 0:
                errors.append(f"mock {label} ALARM_STOP snooze_remaining_s={rem}, expected 0")

        def _expect_no_hooks(label):
            hooks = _drain_hooks()
            if hooks:
                errors.append(f"mock {label} unexpected hooks={[h['path'] for h in hooks]}")

        # ALARM_STOP (ARMED -> DISARMED) via /api/disarm
        _http_request(f"{base}/api/disarm", "POST")
        _expect_alarm_stop_hook("POST /api/disarm (ARMED->DISARMED)", "DISARMED", expect_snooze_nonzero=False)

        # DISARMED -> DISARMED sends no ALARM_STOP
        _http_request(f"{base}/api/disarm", "POST")
        _expect_no_hooks("POST /api/disarm (DISARMED->DISARMED)")

        st, _, _ = _http_request(f"{base}/api/snooze", "POST")
        if st != 409:
            errors.append(f"mock POST /api/snooze while DISARMED returned {st}, expected 409")
        _expect_no_hooks("POST /api/snooze 409")

        # DISARMED -> ARMED via /api/arm does not send ALARM_STOP
        _http_request(f"{base}/api/arm", "POST")
        _expect_no_hooks("POST /api/arm (DISARMED->ARMED)")

        # ARMED -> SNOOZED via /api/snooze sends ALARM_STOP (local only)
        _http_request(f"{base}/api/snooze?min=5", "POST")
        _expect_alarm_stop_hook("POST /api/snooze (ARMED->SNOOZED)", "SNOOZED", expect_snooze_nonzero=True)

        # SNOOZED -> SNOOZED restart sends ALARM_STOP (local only)
        _http_request(f"{base}/api/snooze?min=10", "POST")
        _expect_alarm_stop_hook("POST /api/snooze (SNOOZED->SNOOZED)", "SNOOZED", expect_snooze_nonzero=True)

        # SNOOZED -> DISARMED does not send ALARM_STOP
        _http_request(f"{base}/api/disarm", "POST")
        _expect_no_hooks("POST /api/disarm (SNOOZED->DISARMED)")

        # DISARMED -> ARMED via /api/toggle does not send ALARM_STOP; ARMED -> DISARMED via /api/toggle does
        _http_request(f"{base}/api/toggle", "POST")
        _expect_no_hooks("POST /api/toggle (DISARMED->ARMED)")

        _http_request(f"{base}/api/toggle", "POST")
        _expect_alarm_stop_hook("POST /api/toggle (ARMED->DISARMED)", "DISARMED", expect_snooze_nonzero=False)

        _, _, st_body = _http_request(f"{base}/api/status")
        st_after_stop = json.loads(st_body.decode("utf-8"))
        if (st_after_stop.get("notify") or {}).get("local", {}).get("event") != "ALARM_STOP":
            errors.append(f"mock notify.local after ALARM_STOP unexpected: {st_after_stop.get('notify')!r}")

        _http_request(f"{base}/api/arm", "POST")
        _expect_no_hooks("POST /api/arm (restore ARMED)")

        # Turning cloud_enabled off clears notify.cloud to null while preserving webhook_url
        _http_request(
            f"{base}/api/config",
            "POST",
            {"Content-Type": "application/json", "X-Requested-With": "pm-ui"},
            json.dumps({"cloud_enabled": False}).encode("utf-8"),
        )
        _, _, st_body = _http_request(f"{base}/api/status")
        st_cloud_off = json.loads(st_body.decode("utf-8"))
        if (st_cloud_off.get("notify") or {}).get("cloud") is not None:
            errors.append(f"mock notify.cloud not cleared when cloud_enabled set false: {st_cloud_off.get('notify')!r}")
        _, _, cfg_after_off = _http_request(f"{base}/api/config", "GET", {"X-Requested-With": "pm-ui"})
        cfg_off_obj = json.loads(cfg_after_off.decode("utf-8"))
        if not cfg_off_obj.get("webhook_url"):
            errors.append("mock webhook_url cleared when cloud_enabled set false; expected preserved")

        # Clearing local_url via POST /api/config clears notify.local to null
        _http_request(
            f"{base}/api/config",
            "POST",
            {"Content-Type": "application/json", "X-Requested-With": "pm-ui"},
            json.dumps({"local_url": "", "webhook_url": ""}).encode("utf-8"),
        )
        _, _, st_body = _http_request(f"{base}/api/status")
        st_cleared = json.loads(st_body.decode("utf-8"))
        if (st_cleared.get("notify") or {}).get("local") is not None or (st_cleared.get("notify") or {}).get("cloud") is not None:
            errors.append(f"mock notify not cleared when URLs emptied: {st_cleared.get('notify')!r}")

        # Failed delivery records ok=False with non-empty reason
        dead_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        dead_sock.bind(("127.0.0.1", 0))
        dead_port = dead_sock.getsockname()[1]
        dead_sock.close()
        _http_request(
            f"{base}/api/config",
            "POST",
            {"Content-Type": "application/json", "X-Requested-With": "pm-ui"},
            json.dumps({"local_url": f"http://127.0.0.1:{dead_port}/{{event}}"}).encode("utf-8"),
        )
        _http_request(f"{base}/api/test", "POST")
        srv.state.wait_for_notifications(timeout=3.0)
        _, _, st_body = _http_request(f"{base}/api/status")
        st_failed = json.loads(st_body.decode("utf-8"))
        n_fail = (st_failed.get("notify") or {}).get("local")
        if not n_fail or n_fail.get("ok") is not False or not n_fail.get("reason"):
            errors.append(f"mock notify.local failure not recorded cleanly: {n_fail!r}")

        # 3. Live route guard probe driven directly by mock_device.ROUTE_TABLE
        auth_tok = base64.b64encode(b"admin:SecretPass1").decode("ascii")
        for method, path, guard, _ in mock_device.ROUTE_TABLE:
            _http_request(
                f"{base}/__reset",
                "POST",
                {"Content-Type": "application/json"},
                json.dumps({"config": {"admin_pass": "SecretPass1"}}).encode("utf-8"),
            )
            if guard == "admin":
                st_no_csrf, _, _ = _http_request(f"{base}{path}", method)
                if st_no_csrf != 403:
                    errors.append(f"live guard: {method} {path} without X-Requested-With returned {st_no_csrf}, expected 403")

                st_no_auth, _, _ = _http_request(f"{base}{path}", method, {"X-Requested-With": "pm-ui"})
                if st_no_auth != 401:
                    errors.append(f"live guard: {method} {path} without Basic auth returned {st_no_auth}, expected 401")

                body_arg = b"{}" if (method == "POST" and path == "/api/config") else None
                hdrs = {"X-Requested-With": "pm-ui", "Authorization": f"Basic {auth_tok}"}
                if body_arg is not None:
                    hdrs["Content-Type"] = "application/json"
                st_ok, _, _ = _http_request(f"{base}{path}", method, hdrs, body_arg)
                if st_ok in (401, 403):
                    errors.append(f"live guard: {method} {path} with valid CSRF+auth returned {st_ok}")
            elif guard == "open":
                st_open, _, _ = _http_request(f"{base}{path}", method)
                if st_open in (401, 403):
                    errors.append(f"live guard: open route {method} {path} returned {st_open}")
            else:
                errors.append(f"ROUTE_TABLE unknown guard {guard!r} on {method} {path}")

        # 4. /api/update requires admin_pass set, 0-byte upload -> 500, missing -> 400; /api/config > 2048 -> 413; captive 302
        _http_request(f"{base}/__reset", "POST", {"Content-Type": "application/json"}, b"{}")
        st_upd, _, _ = _http_request(f"{base}/api/update", "POST", {"X-Requested-With": "pm-ui"})
        if st_upd != 403:
            errors.append(f"live /api/update without admin_pass returned {st_upd}, expected 403")

        _http_request(
            f"{base}/__reset",
            "POST",
            {"Content-Type": "application/json"},
            json.dumps({"config": {"admin_pass": "SecretPass1"}}).encode("utf-8"),
        )
        boundary = "----pomBoundary7MA4YWxkTrZu0gW"
        empty_mp = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="firmware"; filename="empty.bin"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
            f"\r\n--{boundary}--\r\n"
        ).encode("utf-8")
        st_empty, _, body_empty = _http_request(
            f"{base}/api/update",
            "POST",
            {
                "X-Requested-With": "pm-ui",
                "Authorization": f"Basic {auth_tok}",
                "Content-Type": f"multipart/form-data; boundary={boundary}",
            },
            empty_mp,
        )
        try:
            empty_json = json.loads(body_empty.decode("utf-8"))
        except Exception:
            empty_json = None
        if st_empty != 500 or empty_json != {"error": "update: empty firmware file"}:
            errors.append(
                f"live /api/update empty upload returned ({st_empty}, {empty_json!r}), "
                "expected (500, {'error': 'update: empty firmware file'})"
            )

        missing_mp = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="other"; filename="other.bin"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
            f"abc\r\n--{boundary}--\r\n"
        ).encode("utf-8")
        st_miss, _, body_miss = _http_request(
            f"{base}/api/update",
            "POST",
            {
                "X-Requested-With": "pm-ui",
                "Authorization": f"Basic {auth_tok}",
                "Content-Type": f"multipart/form-data; boundary={boundary}",
            },
            missing_mp,
        )
        try:
            miss_json = json.loads(body_miss.decode("utf-8"))
        except Exception:
            miss_json = None
        if st_miss != 400 or miss_json != {"error": "update: missing firmware file"}:
            errors.append(
                f"live /api/update missing firmware returned ({st_miss}, {miss_json!r}), "
                "expected (400, {'error': 'update: missing firmware file'})"
            )
        _http_request(f"{base}/__reset", "POST", {"Content-Type": "application/json"}, b"{}")

        oversize = b'{"device_name":"' + (b"a" * 2100) + b'"}'
        st_413, _, _ = _http_request(
            f"{base}/api/config",
            "POST",
            {"X-Requested-With": "pm-ui", "Content-Type": "application/json"},
            oversize,
        )
        if st_413 != 413:
            errors.append(f"live POST /api/config > 2048 B returned {st_413}, expected 413")

        _http_request(
            f"{base}/__state",
            "POST",
            {"Content-Type": "application/json"},
            b'{"setup_ap":{"active":true,"ip":"192.168.4.1"}}',
        )
        st_cap, hdrs_cap, _ = _http_request_no_redirect(f"{base}/", {"Host": "captive.example.com"})
        loc_cap = hdrs_cap.get("Location") or hdrs_cap.get("location")
        if st_cap != 302 or loc_cap != "http://192.168.4.1/":
            errors.append(f"live captive portal redirect returned ({st_cap}, {loc_cap!r}), expected (302, 'http://192.168.4.1/')")
    finally:
        srv.stop()
        hook_srv.shutdown()
        hook_srv.server_close()

    return errors


def _extract_c_scalar_constant(text, name):
    m = re.search(rf'\b{re.escape(name)}\s*=\s*([^;]+);', text)
    if m:
        return m.group(1).strip()
    m_def = re.search(rf'^\s*#define\s+{re.escape(name)}\s+([^\r\n/]+)', text, re.MULTILINE)
    if m_def:
        return m_def.group(1).strip()
    return None


def _parse_c_int(raw):
    if raw is None:
        return None
    cleaned = re.sub(r'[uUlL]+$', '', raw.strip())
    if re.fullmatch(r'-?\d+', cleaned):
        return int(cleaned, 10)
    return None


def _parse_c_str(raw):
    if raw is None:
        return None
    m = re.fullmatch(r'"([^"]*)"', raw.strip())
    return m.group(1) if m else None


def _parse_c_bool(raw):
    if raw is None:
        return None
    cleaned = raw.strip()
    if cleaned == "true":
        return True
    if cleaned == "false":
        return False
    return None


def _parse_js_object(src, idx):
    """Parse a simple JS object literal (keys, strings, ints, nested objects)."""
    n = len(src)

    def skip_ws(pos):
        while pos < n and src[pos] in " \t\r\n":
            pos += 1
        return pos

    pos = skip_ws(idx)
    if pos >= n or src[pos] != "{":
        raise ValueError(f"expected '{{' at offset {pos}")
    pos += 1
    obj = {}

    while True:
        pos = skip_ws(pos)
        if pos >= n:
            raise ValueError("unterminated object literal")
        if src[pos] == "}":
            return obj, pos + 1

        if src[pos] in ("'", '"'):
            quote = src[pos]
            pos += 1
            buf = []
            while pos < n and src[pos] != quote:
                if src[pos] == "\\" and pos + 1 < n:
                    pos += 1
                buf.append(src[pos])
                pos += 1
            if pos >= n:
                raise ValueError("unterminated string key")
            pos += 1
            key = "".join(buf)
        else:
            m_key = re.match(r"[A-Za-z_][A-Za-z0-9_]*", src[pos:])
            if not m_key:
                raise ValueError(f"invalid object key at offset {pos}")
            key = m_key.group(0)
            pos += len(key)

        pos = skip_ws(pos)
        if pos >= n or src[pos] != ":":
            raise ValueError(f"expected ':' after key {key!r}")
        pos = skip_ws(pos + 1)
        if pos >= n:
            raise ValueError(f"missing value for key {key!r}")

        if src[pos] == "{":
            val, pos = _parse_js_object(src, pos)
        elif src[pos] in ("'", '"'):
            quote = src[pos]
            pos += 1
            buf = []
            while pos < n and src[pos] != quote:
                if src[pos] == "\\" and pos + 1 < n:
                    pos += 1
                buf.append(src[pos])
                pos += 1
            if pos >= n:
                raise ValueError(f"unterminated string value for key {key!r}")
            pos += 1
            val = "".join(buf)
        else:
            m_int = re.match(r"-?\d+", src[pos:])
            if not m_int:
                raise ValueError(f"unsupported value for key {key!r} at offset {pos}")
            val = int(m_int.group(0), 10)
            pos += len(m_int.group(0))

        obj[key] = val
        pos = skip_ws(pos)
        if pos < n and src[pos] == ",":
            pos += 1
        elif pos < n and src[pos] == "}":
            continue
        else:
            raise ValueError(f"expected ',' or '}}' after key {key!r} at offset {pos}")


def _extract_web_ui_limits(web_ui_text):
    m = re.search(r"\bconst\s+LIMITS\s*=\s*\{", web_ui_text)
    if not m:
        raise ValueError("const LIMITS = { ... } not found in web_ui.h")
    obj, _ = _parse_js_object(web_ui_text, m.end() - 1)
    return obj


def check_constants_parity():
    errors = []
    cfg_h_path = os.path.join(REPO_ROOT, "firmware", "src", "config.h")
    codec_path = os.path.join(REPO_ROOT, "firmware", "src", "config_codec.cpp")
    if not os.path.isfile(cfg_h_path):
        return ["missing firmware/src/config.h"]

    cfg_text = open(cfg_h_path, "r", encoding="utf-8").read()

    # Default scalars
    default_int_map = {
        "DEFAULT_DEBOUNCE_MS": mock_device.DEFAULT_DEBOUNCE_MS,
        "DEFAULT_COOLDOWN_MS": mock_device.DEFAULT_COOLDOWN_MS,
        "DEFAULT_SNOOZE_MIN": mock_device.DEFAULT_SNOOZE_MIN,
        "DEFAULT_SETUP_WINDOW_S": mock_device.DEFAULT_SETUP_WINDOW_S,
        "DEFAULT_ALERT_MODE": mock_device.DEFAULT_ALERT_MODE,
        "WIFI_SLOTS": mock_device.WIFI_SLOTS,
    }
    for cname, py_val in default_int_map.items():
        val = _parse_c_int(_extract_c_scalar_constant(cfg_text, cname))
        if val is None:
            errors.append(f"config.h missing integer constant {cname}")
        elif val != py_val:
            errors.append(f"{cname} mismatch: config.h={val}, mock={py_val}")

    default_str_map = {
        "DEFAULT_AP_SSID_PREFIX": mock_device.DEFAULT_AP_SSID_PREFIX,
        "DEFAULT_AP_PASS": mock_device.DEFAULT_AP_PASS,
        "DEFAULT_DEVICE_NAME": mock_device.DEFAULT_DEVICE_NAME,
    }
    for cname, py_val in default_str_map.items():
        val = _parse_c_str(_extract_c_scalar_constant(cfg_text, cname))
        if val is None:
            errors.append(f"config.h missing string constant {cname}")
        elif val != py_val:
            errors.append(f"{cname} mismatch: config.h={val!r}, mock={py_val!r}")

    cloud_en_def = _parse_c_bool(_extract_c_scalar_constant(cfg_text, "DEFAULT_CLOUD_ENABLED"))
    if cloud_en_def is None:
        errors.append("config.h missing boolean constant DEFAULT_CLOUD_ENABLED")
    elif cloud_en_def != mock_device.DEFAULT_CLOUD_ENABLED:
        errors.append(f"DEFAULT_CLOUD_ENABLED mismatch: config.h={cloud_en_def!r}, mock={mock_device.DEFAULT_CLOUD_ENABLED!r}")

    if mock_device.DEFAULT_DEVICE_NAME != mobile_alarm_sim.DEFAULT_DEVICE_NAME:
        errors.append(f"DEFAULT_DEVICE_NAME mismatch: mock={mock_device.DEFAULT_DEVICE_NAME!r}, tool={mobile_alarm_sim.DEFAULT_DEVICE_NAME!r}")

    # Numeric limits, snooze limits, string limits, and body size limit by name in config.h
    limit_int_map = {
        "MIN_SETUP_WINDOW_S": mock_device.CONFIG_FIELD_BOUNDS["setup_window_s"][0],
        "MAX_SETUP_WINDOW_S": mock_device.CONFIG_FIELD_BOUNDS["setup_window_s"][1],
        "MIN_DEBOUNCE_MS": mock_device.CONFIG_FIELD_BOUNDS["debounce_ms"][0],
        "MAX_DEBOUNCE_MS": mock_device.CONFIG_FIELD_BOUNDS["debounce_ms"][1],
        "MIN_COOLDOWN_MS": mock_device.CONFIG_FIELD_BOUNDS["restore_cooldown_ms"][0],
        "MAX_COOLDOWN_MS": mock_device.CONFIG_FIELD_BOUNDS["restore_cooldown_ms"][1],
        "MIN_SNOOZE_MIN": mock_device.SNOOZE_MIN_MINUTES,
        "MAX_SNOOZE_MIN": mock_device.SNOOZE_MAX_MINUTES,
        "MAX_WIFI_SSID_LEN": mock_device.CONFIG_STRING_LIMITS["ssid"][1],
        "MIN_PASS_LEN": mock_device.CONFIG_STRING_LIMITS["pass"][0],
        "MAX_PASS_LEN": mock_device.CONFIG_STRING_LIMITS["pass"][1],
        "MAX_DEVICE_NAME_LEN": mock_device.CONFIG_STRING_LIMITS["device_name"][1],
        "MAX_WEBHOOK_URL_LEN": mock_device.CONFIG_STRING_LIMITS["url"][1],
        "HTTP_MAX_BODY_BYTES": mock_device.CONFIG_BODY_MAX_BYTES,
    }
    for cname, py_val in limit_int_map.items():
        val = _parse_c_int(_extract_c_scalar_constant(cfg_text, cname))
        if val is None:
            errors.append(f"config.h missing limit constant {cname}")
        elif val != py_val:
            errors.append(f"{cname} mismatch: config.h={val}, mock={py_val}")

    # Config codec KNOWN_KEYS and schema bounds parity
    if os.path.isfile(codec_path):
        codec_text = open(codec_path, "r", encoding="utf-8").read()
        m_keys = re.search(r"KNOWN_KEYS\[\]\s*=\s*\{(.*?)\};", codec_text, re.DOTALL)
        if not m_keys:
            errors.append("config_codec.cpp missing KNOWN_KEYS[]")
        else:
            codec_keys = set(re.findall(r'"([^"]+)"', m_keys.group(1)))
            if mock_device.KNOWN_CONFIG_KEYS != codec_keys:
                errors.append(
                    f"KNOWN_CONFIG_KEYS mismatch vs config_codec.cpp: "
                    f"mock_only={sorted(mock_device.KNOWN_CONFIG_KEYS - codec_keys)}, "
                    f"codec_only={sorted(codec_keys - mock_device.KNOWN_CONFIG_KEYS)}"
                )

    schema_path = os.path.join(REPO_ROOT, "docs", "schemas", "api_config.schema.json")
    if os.path.isfile(schema_path):
        with open(schema_path, "r", encoding="utf-8") as sf:
            cfg_schema = json.load(sf)
        props = cfg_schema.get("properties", {})
        for field, (lo, hi) in mock_device.CONFIG_FIELD_BOUNDS.items():
            sp = props.get(field)
            if not sp:
                errors.append(f"api_config.schema.json missing property {field}")
                continue
            if "enum" in sp:
                s_lo, s_hi = min(sp["enum"]), max(sp["enum"])
            else:
                s_lo, s_hi = sp.get("minimum"), sp.get("maximum")
            if (s_lo, s_hi) != (lo, hi):
                errors.append(f"schema bounds mismatch for {field}: schema=({s_lo},{s_hi}) mock=({lo},{hi})")

    # Embedded web UI LIMITS object vs config.h by name
    web_ui_path = os.path.join(REPO_ROOT, "firmware", "src", "web_ui.h")
    if not os.path.isfile(web_ui_path):
        errors.append("missing firmware/src/web_ui.h")
    else:
        web_ui_text = open(web_ui_path, "r", encoding="utf-8").read()
        try:
            ui_limits = _extract_web_ui_limits(web_ui_text)
        except ValueError as exc:
            errors.append(f"web_ui.h LIMITS parse error: {exc}")
            ui_limits = None

        if ui_limits is not None:
            ui_int_map = {
                "ssidMaxBytes": "MAX_WIFI_SSID_LEN",
                "apSsidMaxBytes": "MAX_WIFI_SSID_LEN",
                "passMinBytes": "MIN_PASS_LEN",
                "passMaxBytes": "MAX_PASS_LEN",
                "deviceNameMax": "MAX_DEVICE_NAME_LEN",
                "urlMaxBytes": "MAX_WEBHOOK_URL_LEN",
                "alertModeDef": "DEFAULT_ALERT_MODE",
            }
            for js_key, c_name in ui_int_map.items():
                c_val = _parse_c_int(_extract_c_scalar_constant(cfg_text, c_name))
                if c_val is None:
                    errors.append(f"config.h missing integer constant {c_name} for web_ui.h LIMITS.{js_key}")
                elif js_key not in ui_limits:
                    errors.append(f"web_ui.h LIMITS missing {js_key}")
                elif ui_limits[js_key] != c_val:
                    errors.append(f"web_ui.h LIMITS.{js_key}={ui_limits[js_key]!r} != config.h {c_name}={c_val}")

            ui_min_map = {
                "apSsidMinBytes": mock_device.CONFIG_STRING_LIMITS["ap_ssid"][0],
                "deviceNameMin": mock_device.CONFIG_STRING_LIMITS["device_name"][0],
            }
            for js_key, expected_min in ui_min_map.items():
                if js_key not in ui_limits:
                    errors.append(f"web_ui.h LIMITS missing {js_key}")
                elif ui_limits[js_key] != expected_min:
                    errors.append(f"web_ui.h LIMITS.{js_key}={ui_limits[js_key]!r} != expected {expected_min}")

            ui_str_map = {
                "deviceNameDefault": "DEFAULT_DEVICE_NAME",
            }
            for js_key, c_name in ui_str_map.items():
                c_val = _parse_c_str(_extract_c_scalar_constant(cfg_text, c_name))
                if c_val is None:
                    errors.append(f"config.h missing string constant {c_name} for web_ui.h LIMITS.{js_key}")
                elif js_key not in ui_limits:
                    errors.append(f"web_ui.h LIMITS missing {js_key}")
                elif ui_limits[js_key] != c_val:
                    errors.append(f"web_ui.h LIMITS.{js_key}={ui_limits[js_key]!r} != config.h {c_name}={c_val!r}")

            ui_range_map = {
                "debounceMs": ("MIN_DEBOUNCE_MS", "MAX_DEBOUNCE_MS", "DEFAULT_DEBOUNCE_MS"),
                "restoreCooldownMs": ("MIN_COOLDOWN_MS", "MAX_COOLDOWN_MS", "DEFAULT_COOLDOWN_MS"),
                "snoozeDefaultMin": ("MIN_SNOOZE_MIN", "MAX_SNOOZE_MIN", "DEFAULT_SNOOZE_MIN"),
                "setupWindowS": ("MIN_SETUP_WINDOW_S", "MAX_SETUP_WINDOW_S", "DEFAULT_SETUP_WINDOW_S"),
            }
            for js_key, (c_min_name, c_max_name, c_def_name) in ui_range_map.items():
                spec = ui_limits.get(js_key)
                if not isinstance(spec, dict):
                    errors.append(f"web_ui.h LIMITS missing object {js_key}")
                    continue
                for sub_key, c_name in (("min", c_min_name), ("max", c_max_name), ("def", c_def_name)):
                    c_val = _parse_c_int(_extract_c_scalar_constant(cfg_text, c_name))
                    if c_val is None:
                        errors.append(f"config.h missing integer constant {c_name} for web_ui.h LIMITS.{js_key}.{sub_key}")
                    elif sub_key not in spec:
                        errors.append(f"web_ui.h LIMITS.{js_key} missing {sub_key}")
                    elif spec[sub_key] != c_val:
                        errors.append(
                            f"web_ui.h LIMITS.{js_key}.{sub_key}={spec[sub_key]!r} != config.h {c_name}={c_val}"
                        )

    return errors


def _strip_cpp_comments(src):
    out = []
    i = 0
    n = len(src)
    while i < n:
        ch = src[i]
        if ch == "/" and i + 1 < n and src[i + 1] == "/":
            i = src.find("\n", i)
            if i == -1:
                break
            out.append("\n")
            i += 1
        elif ch == "/" and i + 1 < n and src[i + 1] == "*":
            end = src.find("*/", i + 2)
            if end == -1:
                break
            i = end + 2
        elif ch in ('"', "'"):
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\" and i + 1 < n:
                    i += 1
                    out.append(src[i])
                elif src[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _delimit_cpp_handler_body(text, search_start):
    """Find the opening '{' of a C++ lambda/handler after search_start and return its matched body."""
    i = search_start
    n = len(text)
    in_brace = False
    depth = 0
    body_start = -1

    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            nl = text.find("\n", i)
            i = n if nl == -1 else nl + 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            if end == -1:
                return None
            i = end + 2
            continue
        if ch in ('"', "'"):
            quote = ch
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue

        if not in_brace:
            if ch == "{":
                in_brace = True
                depth = 1
                body_start = i
            elif ch == ";":
                return None
        else:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[body_start:i + 1]
        i += 1
    return None


def check_firmware_route_parity():
    errors = []
    main_path = os.path.join(REPO_ROOT, "firmware", "src", "main.cpp")
    ota_path = os.path.join(REPO_ROOT, "firmware", "src", "ota_update.cpp")
    if not os.path.isfile(main_path):
        return ["missing firmware/src/main.cpp"]
    if not os.path.isfile(ota_path):
        return ["missing firmware/src/ota_update.cpp"]

    text = open(main_path, "r", encoding="utf-8").read()
    pattern = re.compile(r'webServer\.on\(\s*"([^"]+)"\s*,\s*HTTP_(GET|POST)\s*,')
    fw_routes = {}

    for m in pattern.finditer(text):
        path, method = m.group(1), m.group(2)
        body = _delimit_cpp_handler_body(text, m.end())
        if body is None:
            errors.append(f"could not delimit handler body for {method} {path} in main.cpp")
            continue
        code_only = _strip_cpp_comments(body)
        has_csrf_auth = "ensureCsrfAndAuth()" in code_only
        fw_routes[(method, path)] = "admin" if has_csrf_auth else "open"

    if "registerOtaUpdateRoute(" not in _strip_cpp_comments(text):
        errors.append("main.cpp does not call registerOtaUpdateRoute()")

    ota_text = open(ota_path, "r", encoding="utf-8").read()
    ota_pattern = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*HTTP_(GET|POST)\s*,')
    for m in ota_pattern.finditer(ota_text):
        path, method = m.group(1), m.group(2)
        body = _delimit_cpp_handler_body(ota_text, m.end())
        if body is None:
            errors.append(f"could not delimit handler body for {method} {path} in ota_update.cpp")
            continue
        code_only = _strip_cpp_comments(body)
        has_guard = "evaluateServerOtaGuard(" in code_only or "evaluateOtaGuard(" in code_only
        fw_routes[(method, path)] = "admin" if has_guard else "open"

    if not fw_routes:
        errors.append("no routes extracted from firmware/src/main.cpp or ota_update.cpp")
        return errors

    expected_routes = {(m, p): g for m, p, g, _ in mock_device.ROUTE_TABLE}
    if len(fw_routes) != len(expected_routes) or set(fw_routes.keys()) != set(expected_routes.keys()):
        missing = sorted(set(expected_routes.keys()) - set(fw_routes.keys()))
        extra = sorted(set(fw_routes.keys()) - set(expected_routes.keys()))
        errors.append(
            f"firmware route table mismatch (fw={len(fw_routes)}, mock={len(expected_routes)}): "
            f"missing={missing}, extra={extra}"
        )

    for key in sorted(set(fw_routes.keys()) & set(expected_routes.keys())):
        if fw_routes[key] != expected_routes[key]:
            errors.append(
                f"guard mismatch on {key[0]} {key[1]}: firmware={fw_routes[key]}, mock={expected_routes[key]}"
            )

    return errors


def check_ntfy_literals_parity():
    errors = []
    net_path = os.path.join(REPO_ROOT, "firmware", "src", "network_mgr.cpp")
    if not os.path.isfile(net_path):
        return ["missing firmware/src/network_mgr.cpp"]

    text = open(net_path, "r", encoding="utf-8").read()
    m_block = re.search(
        r'if\s*\(\s*strcmp\s*\(\s*payload\.event\s*,\s*"POWER_OFF"\s*\)\s*==\s*0\s*\)\s*\{(.*?)'
        r'\}\s*else\s+if\s*\(\s*strcmp\s*\(\s*payload\.event\s*,\s*"POWER_ON"\s*\)\s*==\s*0\s*\)\s*\{(.*?)'
        r'\}\s*else\s*\{(.*?)\}',
        text,
        re.DOTALL,
    )
    if not m_block:
        return ["could not locate ntfy header branches in firmware/src/network_mgr.cpp"]

    hdr_re = re.compile(r'http\.addHeader\(\s*"(Title|Priority|Tags)"\s*,\s*"([^"]+)"\s*\)')
    branch_map = {
        "POWER_OFF": m_block.group(1),
        "POWER_ON": m_block.group(2),
        "TEST_ALARM": m_block.group(3),
    }
    for ev_name, block_src in branch_map.items():
        fw_hdrs = dict(hdr_re.findall(block_src))
        expected = mobile_alarm_sim.NTFY_TEMPLATES[ev_name]
        if set(fw_hdrs.keys()) != set(expected.keys()):
            errors.append(f"ntfy headers missing in network_mgr.cpp for {ev_name}: got {sorted(fw_hdrs.keys())}")
        elif fw_hdrs != expected:
            errors.append(f"ntfy headers mismatch for {ev_name}: fw={fw_hdrs}, tool={expected}")

    if re.search(r'http\.addHeader\(\s*"Actions"\b', text):
        errors.append("network_mgr.cpp must not send ntfy Actions header")
    if hasattr(mobile_alarm_sim, "NTFY_ACTION_TEMPLATE"):
        errors.append("mobile_alarm_sim.py must not define NTFY_ACTION_TEMPLATE")
    if "Actions" in mobile_alarm_sim.build_ntfy_headers("POWER_OFF"):
        errors.append("mobile_alarm_sim.build_ntfy_headers must not include Actions")

    m_body = re.search(
        r'String\s+messageBody\s*=\s*String\(\s*"([^"]+)"\s*\)\s*\+\s*payload\.event\s*\+\s*"([^"]+)"\s*\+\s*payload\.device\s*\+\s*"([^"]+)"\s*\+\s*payload\.duration_s\s*\+\s*"([^"]+)"',
        text,
    )
    if not m_body:
        errors.append("could not extract ntfy messageBody template from network_mgr.cpp")
    else:
        fw_body_tmpl = (
            m_body.group(1).replace("\\n", "\n")
            + "{event}"
            + m_body.group(2).replace("\\n", "\n")
            + "{device}"
            + m_body.group(3).replace("\\n", "\n")
            + "{duration_s}"
            + m_body.group(4).replace("\\n", "\n")
        )
        if fw_body_tmpl != mobile_alarm_sim.NTFY_BODY_TEMPLATE:
            errors.append(f"ntfy body template mismatch: fw={fw_body_tmpl!r}, tool={mobile_alarm_sim.NTFY_BODY_TEMPLATE!r}")

    return errors


def check_config_validation_parity(payload_dir=None, payload_exec_error=None):
    if payload_exec_error:
        return [payload_exec_error]

    cases_path = os.path.join(THIS_DIR, "config_cases.json")
    if not os.path.isfile(cases_path):
        return ["missing tools/sim/config_cases.json"]
    try:
        with open(cases_path, "r", encoding="utf-8") as cf:
            cases_doc = json.load(cf)
    except Exception as exc:
        return [f"invalid config_cases.json: {exc}"]

    cases = cases_doc.get("cases") if isinstance(cases_doc, dict) else None
    if not isinstance(cases, list) or not cases:
        return ["config_cases.json must contain a non-empty 'cases' list"]

    errors = []
    created_tmp = False
    if payload_dir is None:
        payload_dir, exec_err = _run_payload_dump_to_temp()
        if exec_err:
            return [exec_err]
        created_tmp = True

    try:
        res_path = os.path.join(payload_dir, "config_cases_result.json")
        if not os.path.isfile(res_path):
            return ["missing config_cases_result.json from payload_dump"]
        with open(res_path, "r", encoding="utf-8") as rf:
            fw_results = json.load(rf)
        if not isinstance(fw_results, list):
            return ["config_cases_result.json must be a JSON array"]

        fw_by_name = {entry.get("name"): entry for entry in fw_results if isinstance(entry, dict)}
        case_names = [c.get("name") for c in cases]
        if len(fw_results) != len(cases) or set(fw_by_name.keys()) != set(case_names):
            missing = sorted(set(case_names) - set(fw_by_name.keys()))
            extra = sorted(set(fw_by_name.keys()) - set(case_names))
            errors.append(
                f"config_cases_result.json case mismatch (fw={len(fw_results)}, cases={len(cases)}): "
                f"missing={missing}, extra={extra}"
            )
            return errors

        for case in cases:
            cname = case["name"]
            cur = mock_device.default_config()
            if "initial" in case and case["initial"] is not None:
                init_ok, cur_merged, init_f, init_r, _ = mock_device.validate_and_merge_config(cur, case["initial"])
                if not init_ok:
                    errors.append(f"{cname}: mock rejected initial config: {init_f} ({init_r})")
                    continue
                cur = cur_merged

            m_ok, _, m_field, m_reason, m_applied = mock_device.validate_and_merge_config(cur, case["body"])
            fw = fw_by_name[cname]
            fw_ok = bool(fw.get("ok"))
            fw_field = fw.get("field", "")
            fw_reason = fw.get("reason", "")
            fw_applied = list(fw.get("applied", []))

            if m_ok != fw_ok:
                errors.append(
                    f"{cname}: ok mismatch fw={fw_ok} ({fw_field}: {fw_reason}) vs mock={m_ok} ({m_field}: {m_reason})"
                )
                continue
            if m_field != fw_field or m_reason != fw_reason:
                errors.append(
                    f"{cname}: error mismatch fw=({fw_field!r}, {fw_reason!r}) vs mock=({m_field!r}, {m_reason!r})"
                )
            if set(m_applied) != set(fw_applied) or m_applied != fw_applied:
                errors.append(
                    f"{cname}: applied mismatch fw={fw_applied!r} vs mock={m_applied!r}"
                )
    finally:
        if created_tmp:
            shutil.rmtree(payload_dir, ignore_errors=True)

    return errors


def run_all_checks(payload_dir=None, payload_exec_error=None):
    created_tmp = False
    if payload_dir is None and payload_exec_error is None:
        payload_dir, exec_err = _run_payload_dump_to_temp()
        if exec_err:
            payload_exec_error = exec_err
        else:
            created_tmp = True

    try:
        return [
            ("L2_schemas_draft2020_12", check_schemas_metaschema()),
            ("L2_firmware_payload_schemas", check_firmware_payloads(payload_dir=payload_dir, payload_exec_error=payload_exec_error)),
            ("L2_mobile_alarm_schemas", check_mobile_alarm_payloads()),
            ("L2_live_mock_contracts", check_live_mock_contracts()),
            ("L2_constants_parity", check_constants_parity()),
            ("L2_firmware_route_parity", check_firmware_route_parity()),
            ("L2_ntfy_literals_parity", check_ntfy_literals_parity()),
            ("L2_config_validation_parity", check_config_validation_parity(payload_dir=payload_dir, payload_exec_error=payload_exec_error)),
        ]
    finally:
        if created_tmp:
            shutil.rmtree(payload_dir, ignore_errors=True)


if __name__ == "__main__":
    pdir = sys.argv[1] if len(sys.argv) > 1 else None
    results = run_all_checks(payload_dir=pdir)
    failed = 0
    for name, errs in results:
        if errs:
            failed += 1
            print(f"FAIL {name}")
            for e in errs:
                print(f"  - {e}")
        else:
            print(f"PASS {name}")
    sys.exit(1 if failed else 0)
