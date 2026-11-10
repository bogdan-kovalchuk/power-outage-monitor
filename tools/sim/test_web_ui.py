#!/usr/bin/env python3
"""L3 Playwright web-UI tests against tools/sim/mock_device.py driving real web_ui.h."""

import http.server
import json
import os
import sys
import threading
import urllib.request

import pytest
from playwright.sync_api import sync_playwright

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if THIS_DIR not in sys.path:
    sys.path.insert(0, THIS_DIR)

import mock_device  # noqa: E402


def _http_json(url, method="GET", payload=None):
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


@pytest.fixture(scope="session")
def mock_server():
    srv = mock_device.MockDeviceServer(host="127.0.0.1", port=0, lan=False)
    srv.start()
    yield srv
    srv.stop()


@pytest.fixture(scope="session")
def browser():
    channel = os.environ.get("POM_BROWSER_CHANNEL", "").strip() or None
    launch_kwargs = {
        "headless": True,
        "args": ["--host-resolver-rules=MAP 192.168.4.1 127.0.0.1"],
    }
    if channel:
        launch_kwargs["channel"] = channel
    with sync_playwright() as p:
        b = p.chromium.launch(**launch_kwargs)
        yield b
        b.close()


@pytest.fixture()
def dev_ctrl(mock_server):
    base = mock_server.base_url
    _http_json(f"{base}/__reset", method="POST", payload={})

    class Ctrl:
        base_url = base
        port = mock_server.port
        server = mock_server

        def set_state(self, data):
            return _http_json(f"{base}/__state", method="POST", payload=data)

        def get_requests(self):
            return _http_json(f"{base}/__requests")["requests"]

        def clear_requests(self):
            return self.set_state({"clear_requests": True})

    return Ctrl()


def _open_page(browser, url, http_credentials=None, install_clock=False):
    ctx = browser.new_context(http_credentials=http_credentials) if http_credentials else browser.new_context()
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda err: errors.append(str(err)))
    if install_clock:
        page.clock.install()
    page.goto(url, wait_until="domcontentloaded")
    page.wait_for_function("() => document.getElementById('diagIp').textContent !== '-'")
    return ctx, page, errors


def test_U1_grid_states_warnings_and_controls(browser, dev_ctrl):
    dev_ctrl.set_state({"grid": "online"})
    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        # ONLINE state + warning banner + LAN-open note
        page.wait_for_function("() => document.getElementById('statusTitle').textContent === 'GRID ONLINE'")
        assert "online" in page.locator("#statusCard").get_attribute("class").split()
        assert page.locator("#statusDesc").text_content() == "Mains power present \u2013 Normal"
        assert page.locator("#timerBadge").text_content() == "Active & Protected"
        assert page.locator("#controlsNote").text_content() == (
            "Arm, disarm, snooze and test are open to anyone on the local network by design."
        )
        assert page.locator("#adminWarn").is_visible()
        page.wait_for_function(
            "() => document.getElementById('phoneNotifyStatus').textContent === 'Phone: not configured'"
        )
        page.wait_for_function(
            "() => document.getElementById('cloudNotifyStatus').textContent === 'Cloud: off'"
        )
        assert page.locator("#cloudNotifyStatus").is_visible()

        # Notify summary pills: local OK -> local failed + cloud OK
        dev_ctrl.set_state({
            "config": {"local_url": "http://192.168.1.60:8080/{event}"},
            "notify": {"local": {"event": "TEST_ALARM", "ok": True, "code": 200, "reason": "", "age_s": 120}},
        })
        page.wait_for_function(
            "() => document.getElementById('phoneNotifyStatus').textContent === 'Phone: OK (test 2 min ago)'"
        )
        assert "ok" in page.locator("#phoneNotifyStatus").get_attribute("class").split()
        assert page.locator("#cloudNotifyStatus").text_content() == "Cloud: off"

        dev_ctrl.set_state({
            "config": {"cloud_enabled": True, "webhook_url": "https://ntfy.sh/test-topic"},
            "notify": {
                "local": {"event": "POWER_OFF", "ok": False, "code": -11, "reason": "Read timeout", "age_s": 30},
                "cloud": {"event": "POWER_OFF", "ok": True, "code": 200, "reason": "", "age_s": 12},
            },
        })
        page.wait_for_function(
            "() => document.getElementById('phoneNotifyStatus').textContent.startsWith("
            "'Phone: last delivery failed (power_off, Read timeout, ') && "
            "document.getElementById('phoneNotifyStatus').textContent.endsWith('s ago)')"
        )
        assert "fail" in page.locator("#phoneNotifyStatus").get_attribute("class").split()
        page.wait_for_function(
            "() => document.getElementById('cloudNotifyStatus').textContent.startsWith('Cloud: OK (power_off ') && "
            "document.getElementById('cloudNotifyStatus').textContent.endsWith('s ago)')"
        )
        assert page.locator("#cloudNotifyStatus").is_visible()
        assert "ok" in page.locator("#cloudNotifyStatus").get_attribute("class").split()

        # Reset URLs to blank before control flow checks
        dev_ctrl.set_state({"config": {"local_url": "", "webhook_url": "", "cloud_enabled": False}})
        page.wait_for_function(
            "() => document.getElementById('phoneNotifyStatus').textContent === 'Phone: not configured'"
        )
        page.wait_for_function(
            "() => document.getElementById('cloudNotifyStatus').textContent === 'Cloud: off'"
        )

        # Transition ONLINE -> BOOTING (not vacuous because page was on ONLINE)
        dev_ctrl.set_state({"grid": "booting"})
        page.wait_for_function("() => document.getElementById('statusTitle').textContent === 'CHECKING POWER'")
        boot_cls = page.locator("#statusCard").get_attribute("class").split()
        assert "online" not in boot_cls and "offline" not in boot_cls
        assert page.locator("#statusDesc").text_content() == "Checking mains power..."
        assert page.locator("#timerBadge").text_content() == "Starting up..."

        # Transition BOOTING -> OFFLINE
        dev_ctrl.set_state({"grid": "offline"})
        page.wait_for_function("() => document.getElementById('statusTitle').textContent === 'POWER OUTAGE DETECTED'")
        assert "offline" in page.locator("#statusCard").get_attribute("class").split()
        assert page.locator("#statusDesc").text_content() == "Mains power lost"
        assert page.locator("#timerBadge").text_content().startswith("Outage: 00:00:")

        # Admin protection hides #adminWarn
        dev_ctrl.set_state({"config": {"admin_pass": "SecretPass1"}})
        page.wait_for_function("() => document.getElementById('adminWarn').style.display === 'none'")

        # Disarm -> 409 snooze rejected with error toast -> Arm -> Snooze -> Test Alarm
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/disarm") and r.request.method == "POST") as resp_dis:
            page.locator("button.btn-disarm").click()
        assert resp_dis.value.status == 200
        page.wait_for_function("() => document.getElementById('modePill').textContent === 'DISARMED'")

        with page.expect_response(lambda r: "/api/snooze" in r.url and r.request.method == "POST") as resp_snz_409:
            page.locator("#btnSnooze").click()
        assert resp_snz_409.value.status == 409
        page.wait_for_function("() => document.getElementById('toast').classList.contains('show')")
        assert "disarmed" in page.locator("#toast").text_content().lower()
        assert page.locator("#modePill").text_content() == "DISARMED"

        with page.expect_response(lambda r: r.url.endswith("/api/arm") and r.request.method == "POST") as resp_arm:
            page.locator("button.btn-arm").click()
        assert resp_arm.value.status == 200
        page.wait_for_function("() => document.getElementById('modePill').textContent === 'ARMED'")

        with page.expect_response(lambda r: "/api/snooze?min=30" in r.url and r.request.method == "POST") as resp_snz:
            page.locator("#btnSnooze").click()
        assert resp_snz.value.status == 200
        page.wait_for_function("() => document.getElementById('modePill').textContent.startsWith('SNOOZE (')")

        class _OkHook(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                if n > 0:
                    self.rfile.read(n)
                self.send_response(200)
                self.end_headers()

            def log_message(self, fmt, *args):
                pass

        hook_srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _OkHook)
        hook_thread = threading.Thread(target=hook_srv.serve_forever, daemon=True)
        hook_thread.start()
        try:
            hook_port = hook_srv.server_address[1]
            dev_ctrl.set_state({"config": {"local_url": f"http://127.0.0.1:{hook_port}/{{event}}"}})
            with page.expect_response(lambda r: r.url.endswith("/api/test") and r.request.method == "POST") as resp_tst:
                page.locator("button.btn-test").click()
            assert resp_tst.value.status == 200
            page.wait_for_function("() => document.getElementById('toast').textContent === 'Test alert dispatched'")
            page.wait_for_function(
                "() => document.getElementById('phoneNotifyStatus').textContent.startsWith('Phone: OK (test ')"
            )
            assert "ok" in page.locator("#phoneNotifyStatus").get_attribute("class").split()
        finally:
            hook_srv.shutdown()
            hook_srv.server_close()

        assert errors == []
    finally:
        ctx.close()


def test_U2_config_load_and_isolated_card_saves(browser, dev_ctrl):
    dev_ctrl.set_state({
        "config": {
            "wifi": [{"slot": 0, "ssid": "InitialNet", "pass": "initialpass1"}],
            "device_name": "initial-mon",
            "alert_mode": 1,
            "webhook_url": "https://ntfy.sh/init-topic",
            "local_url": "http://192.168.1.61:8080/{event}",
            "setup_window_s": 90,
            "snooze_default_min": 45,
        }
    })
    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        page.wait_for_function("() => document.getElementById('cfg_name').value === 'initial-mon'")
        assert page.locator("#w_ssid_0").input_value() == "InitialNet"
        assert page.locator("#w_pass_0").get_attribute("placeholder") == "unchanged"
        assert page.locator("#cfg_ap_pass").get_attribute("placeholder") == "unchanged"
        assert page.locator("#cfg_alert_mode").input_value() == "1"
        assert (
            page.locator("#cfg_alert_mode option[value='0']").text_content()
            == "Outage only (no internet alert on power return)"
        )
        assert not page.locator("#cfg_cloud_enabled").is_checked()
        assert page.locator("#cfg_webhook").input_value() == "https://ntfy.sh/init-topic"
        assert page.locator("#cfg_local_url").input_value() == "http://192.168.1.61:8080/{event}"
        assert page.locator("#btnSnooze").text_content() == "Snooze 45m"

        # Open config cards including collapsed #advCard; edit multiple cards, save ONLY Device Settings
        assert "open" not in (page.locator("#body_adv").get_attribute("class") or "").split()
        assert page.locator("#body_adv #cfg_name").count() == 1
        assert page.locator("#body_adv #cfg_debounce").count() == 1
        assert page.locator("#body_adv #cfg_cooldown").count() == 1
        assert page.locator("#body_adv #cfg_snooze").count() == 1
        assert page.locator("#advWifiRows #w_ssid_1").count() == 1

        page.locator("#wifiCard .collapsible-trigger").click()
        page.locator("#devCard .collapsible-trigger").click()
        page.locator("#secCard .collapsible-trigger").click()
        page.locator("#advCard .collapsible-trigger").click()
        assert "open" in page.locator("#body_wifi").get_attribute("class").split()
        assert "open" in page.locator("#body_dev").get_attribute("class").split()
        assert "open" in page.locator("#body_sec").get_attribute("class").split()
        assert "open" in page.locator("#body_adv").get_attribute("class").split()

        page.locator("#w_ssid_1").fill("DraftUnsavedNet")
        page.locator("#cfg_ap_win").fill("120")
        page.locator("#cfg_name").fill("kitchen-mon")
        page.locator("#cfg_alert_mode").select_option("0")
        page.locator("#cfg_webhook").fill("https://ntfy.sh/updated-topic")
        page.locator("#cfg_local_url").fill("http://192.168.1.60:8080/{event}")

        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as resp:
            page.locator("#body_dev button[type='submit']").click()
        assert resp.value.status == 200
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Device settings saved'")

        post_reqs = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(post_reqs) == 1
        assert post_reqs[0]["headers"].get("x-requested-with") == "pm-ui"
        assert post_reqs[0]["json"] == {
            "alert_mode": 0,
            "webhook_url": "https://ntfy.sh/updated-topic",
            "local_url": "http://192.168.1.60:8080/{event}",
        }
        # Unsaved edits in the other cards were preserved
        assert page.locator("#w_ssid_1").input_value() == "DraftUnsavedNet"
        assert page.locator("#cfg_ap_win").input_value() == "120"
        assert page.locator("#cfg_name").input_value() == "kitchen-mon"

        # Save Advanced Settings card independently
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as resp_adv:
            page.locator("#form_adv button[type='submit']").click()
        assert resp_adv.value.status == 200
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Advanced settings saved'")
        adv_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(adv_posts) == 1
        assert adv_posts[0]["json"] == {"device_name": "kitchen-mon"}

        # Verify webhook_url, local_url & device_name round-trip after full reload
        page.reload(wait_until="domcontentloaded")
        page.wait_for_function("() => document.getElementById('cfg_name').value === 'kitchen-mon'")
        assert page.locator("#cfg_webhook").input_value() == "https://ntfy.sh/updated-topic"
        assert page.locator("#cfg_local_url").input_value() == "http://192.168.1.60:8080/{event}"

        # Empty Setup Access submit must not send secret fields or any POST
        page.locator("#secCard .collapsible-trigger").click()
        dev_ctrl.clear_requests()
        page.locator("#body_sec button[type='submit']").click()
        page.wait_for_function("() => document.getElementById('toast').textContent === 'No access changes to save'")
        assert [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"] == []

        # Set setup_window_s + admin_pass on Setup Access card
        page.locator("#cfg_ap_win").fill("180")
        page.locator("#cfg_adm_pass").fill("AdminPass99")
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as resp_sec:
            page.locator("#body_sec button[type='submit']").click()
        assert resp_sec.value.status == 200
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Setup access saved'")
        sec_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(sec_posts) == 1
        assert sec_posts[0]["json"] == {"setup_window_s": 180, "admin_pass": "AdminPass99"}
        page.wait_for_function("() => document.getElementById('adminWarn').style.display === 'none'")
        assert errors == []
    finally:
        ctx.close()

    # Open fresh authenticated context to verify admin_pass placeholder & clear_admin_pass
    ctx2, page2, errors2 = _open_page(
        browser,
        f"{dev_ctrl.base_url}/",
        http_credentials={"username": "admin", "password": "AdminPass99"},
    )
    try:
        page2.locator("#secCard .collapsible-trigger").click()
        page2.wait_for_function("() => document.getElementById('cfg_adm_pass').placeholder === 'unchanged'")
        assert page2.locator("#clearAdmWrap").is_visible()
        page2.locator("#cfg_clear_adm").check()
        assert page2.locator("#cfg_adm_pass").is_disabled()
        dev_ctrl.clear_requests()
        with page2.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as resp_clr:
            page2.locator("#body_sec button[type='submit']").click()
        assert resp_clr.value.status == 200
        clr_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(clr_posts) == 1
        assert clr_posts[0]["json"] == {"clear_admin_pass": True}
        page2.wait_for_function("() => document.getElementById('adminWarn').style.display === 'block'")
        assert errors2 == []
    finally:
        ctx2.close()


def test_U3_wifi_card_add_change_clear_open_and_utf8_reject(browser, dev_ctrl):
    dev_ctrl.set_state({
        "config": {
            "wifi": [{"slot": 0, "ssid": "InitialNet", "pass": "initialpass1"}],
        }
    })
    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        page.wait_for_function(
            "() => { const el = document.getElementById('w_ssid_0'); return el && el.value === 'InitialNet'; }"
        )
        page.locator("#wifiCard .collapsible-trigger").click()
        page.locator("#advCard .collapsible-trigger").click()

        # 1. Client-side UTF-8 > 32 bytes rejection in backup slot 2 (17 Cyrillic chars = 34 bytes)
        page.locator("#w_ssid_1").fill("АБВГДЕЖЗИЙКЛМНОПР")
        page.locator("#w_pass_1").fill("validpass1")
        dev_ctrl.clear_requests()
        page.locator("#body_adv > button.btn-save").click()
        page.wait_for_function("() => document.getElementById('advWifiErr').style.display === 'block'")
        assert page.locator("#advWifiErr").text_content() == "Slot 2: SSID must be at most 32 bytes"
        assert [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"] == []

        # 2. Add backup slot 1 with password
        page.locator("#w_ssid_1").fill("SecondNet")
        page.locator("#w_pass_1").fill("secondpass1")
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "GET") as r_reload_2:
            with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_add:
                page.locator("#body_adv > button.btn-save").click()
        assert r_add.value.status == 200
        assert r_reload_2.value.status == 200
        r_reload_2.value.finished()
        page.wait_for_function("() => document.getElementById('w_pass_1').placeholder === 'unchanged'")
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Backup Wi-Fi settings saved'")
        add_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(add_posts) == 1
        assert add_posts[0]["json"] == {
            "wifi": [{"slot": 1, "ssid": "SecondNet", "pass": "secondpass1"}]
        }

        # 3. Change primary slot 0 to open network (sends clear_pass: true)
        page.locator("#w_ssid_0").fill("OpenSpot")
        page.locator("#w_open_0").check()
        assert page.locator("#w_pass_0").is_disabled()
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "GET") as r_reload_3:
            with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_open:
                page.locator("#body_wifi button.btn-save").click()
        assert r_open.value.status == 200
        assert r_reload_3.value.status == 200
        r_reload_3.value.finished()
        page.wait_for_function("() => !!(cfg && cfg.wifi && cfg.wifi[0] && cfg.wifi[0].ssid === 'OpenSpot' && !dirtyWifi)")
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Wi-Fi settings saved'")
        page.wait_for_function("() => document.getElementById('w_pass_0').placeholder === 'Open network (no password)'")
        open_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(open_posts) == 1
        assert open_posts[0]["json"] == {
            "wifi": [{"slot": 0, "ssid": "OpenSpot", "clear_pass": True}]
        }

        # 4. Clear backup slot 1 via row Delete button -> exact POST {"wifi": [{"slot": 1, "ssid": ""}]}
        page.locator("#advWifiRows .wifi-slot").nth(0).get_by_role("button", name="Delete").click()
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "GET") as r_reload_4:
            with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_clr:
                page.locator("#body_adv > button.btn-save").click()
        assert r_clr.value.status == 200
        assert r_reload_4.value.status == 200
        r_reload_4.value.finished()
        page.wait_for_function("() => !!(cfg && cfg.wifi && cfg.wifi[1] && cfg.wifi[1].ssid === '' && !dirtyAdvWifi)")
        page.wait_for_function("() => document.getElementById('w_ssid_1').value === ''")
        clr_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert len(clr_posts) == 1
        assert clr_posts[0]["json"] == {"wifi": [{"slot": 1, "ssid": ""}]}
        assert errors == []
    finally:
        ctx.close()


def test_U4_scan_xss_selection_and_timeout(browser, dev_ctrl):
    # Part A: pending -> networks rendered safely (XSS escaped) -> click selection
    xss_ssid = '<img src=x onerror="window.__xss=1">BadNet'
    dev_ctrl.set_state({
        "scan_hold": True,
        "scan_networks": [
            {"ssid": xss_ssid, "rssi": -49, "secure": True, "channel": 6},
            {"ssid": "GuestOpen", "rssi": -71, "secure": False, "channel": 1},
        ],
    })
    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        page.locator("#wifiCard .collapsible-trigger").click()
        with page.expect_response(lambda r: "/api/scan" in r.url and r.status == 200) as scan_hold_resp:
            page.locator("#btnScan").click()
        scan_hold_resp.value.finished()
        assert page.locator("#btnScan").is_disabled()
        assert page.locator("#btnScan").text_content() == "Scanning..."

        dev_ctrl.set_state({"scan_hold": False})
        page.wait_for_function("() => document.getElementById('scanResults').classList.contains('show')")
        assert not page.locator("#btnScan").is_disabled()
        assert page.locator("#btnScan").text_content() == "Scan"
        assert page.evaluate("() => window.__xss === undefined")
        assert page.locator("#scanResults .scan-item img").count() == 0
        assert xss_ssid in page.locator("#scanResults .scan-item").nth(0).text_content()

        page.locator("#scanResults .scan-item").nth(0).click()
        assert page.locator("#w_ssid_0").input_value() == xss_ssid
        assert not page.locator("#w_open_0").is_checked()

        page.locator("#scanResults .scan-item").nth(1).click()
        assert page.locator("#w_ssid_1").input_value() == "GuestOpen"
        assert page.locator("#w_open_1").is_checked()
        assert page.locator("#w_pass_1").is_disabled()
        assert errors == []
    finally:
        ctx.close()

    # Part B: scan timeout path driven deterministically by poll responses + Playwright clock
    dev_ctrl.set_state({"scan_hold": True})
    ctx_t, page_t, errors_t = _open_page(browser, f"{dev_ctrl.base_url}/", install_clock=True)
    try:
        page_t.clock.pause_at(page_t.evaluate("() => Date.now()"))
        limits = page_t.evaluate("() => ({timeout: LIMITS.scanTimeoutMs, interval: LIMITS.scanPollIntervalMs})")
        max_attempts = -(-limits["timeout"] // limits["interval"])
        page_t.evaluate("""() => {
            const orig = window.setTimeout;
            window.__scanTimerCount = 0;
            window.setTimeout = function(fn, ms) {
                if (ms === LIMITS.scanPollIntervalMs) window.__scanTimerCount++;
                return orig.apply(this, arguments);
            };
        }""")
        page_t.locator("#wifiCard .collapsible-trigger").click()
        with page_t.expect_response(lambda r: "/api/scan" in r.url and r.status == 200) as r0:
            page_t.locator("#btnScan").click()
        r0.value.finished()
        assert page_t.locator("#btnScan").is_disabled()

        for poll_idx in range(1, max_attempts):
            page_t.wait_for_function("n => window.__scanTimerCount >= n", arg=poll_idx)
            with page_t.expect_response(lambda r: "/api/scan" in r.url and r.status == 200) as r_next:
                page_t.clock.run_for(limits["interval"])
            r_next.value.finished()

        page_t.wait_for_function("() => !document.getElementById('btnScan').disabled")
        assert "Scan timed out, try again" in page_t.locator("#scanResults").text_content()
        assert page_t.locator("#btnScan").text_content() == "Scan"
        assert errors_t == []
    finally:
        ctx_t.close()


def test_U5_firmware_ota_gate_and_upload(browser, dev_ctrl):
    # Without admin password: OTA inputs and hint disabled
    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        page.locator("#fwCard .collapsible-trigger").click()
        assert page.locator("#fwCurrentVer").text_content() == mock_device.FW_VERSION
        assert page.locator("#fwDisabledHint").is_visible()
        assert page.locator("#fwDisabledHint").text_content() == "Set an admin password to enable firmware updates"
        assert page.locator("#fw_file").is_disabled()
        assert page.locator("#btnFwUpload").is_disabled()
        assert errors == []
    finally:
        ctx.close()

    # With admin password: OTA enabled and multipart upload records byte count + reboot message
    dev_ctrl.set_state({"config": {"admin_pass": "OtaPass123"}})
    ctx2, page2, errors2 = _open_page(
        browser,
        f"{dev_ctrl.base_url}/",
        http_credentials={"username": "admin", "password": "OtaPass123"},
    )
    try:
        page2.locator("#fwCard .collapsible-trigger").click()
        assert not page2.locator("#fwDisabledHint").is_visible()
        assert not page2.locator("#fw_file").is_disabled()
        assert not page2.locator("#btnFwUpload").is_disabled()

        fw_bytes = b"\xE9" + b"POM-TEST-FW" * 100
        page2.locator("#fw_file").set_input_files({
            "name": "firmware.bin",
            "mimeType": "application/octet-stream",
            "buffer": fw_bytes,
        })
        dev_ctrl.clear_requests()
        with page2.expect_response(
            lambda r: r.url.endswith("/api/update") and r.request.method == "POST" and r.status == 200
        ) as resp_up:
            page2.locator("#btnFwUpload").click()
        assert resp_up.value.status == 200
        page2.wait_for_function(
            "() => document.getElementById('fwStatusMsg').textContent === 'Firmware written, device is rebooting'"
        )
        assert dev_ctrl.server.state.last_update_bytes == len(fw_bytes)
        up_reqs = [
            r for r in dev_ctrl.get_requests()
            if r["method"] == "POST" and r["path"] == "/api/update" and r["status"] == 200
        ]
        assert len(up_reqs) == 1
        assert up_reqs[0]["firmware_bytes"] == len(fw_bytes)
        assert errors2 == []
    finally:
        ctx2.close()


def test_U6_setup_ap_reboot_and_factory_reset(browser, dev_ctrl):
    # Closing Setup AP requires at least one saved Wi-Fi network
    dev_ctrl.set_state({"config": {"wifi": [{"slot": 0, "ssid": "HomeNet", "pass": "homepass123"}]}})
    # Setup AP open/close + Reboot confirm dismiss & accept
    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        page.locator("#secCard .collapsible-trigger").click()
        assert page.locator("#diagAp").text_content() == "Off"
        assert not page.locator("#btnCloseAp").is_visible()

        with page.expect_response(lambda r: r.url.endswith("/api/setup-ap") and r.request.method == "POST") as r_ap_open:
            page.get_by_role("button", name="Open setup AP for 5 min").click()
        assert r_ap_open.value.status == 200
        page.wait_for_function("() => document.getElementById('btnCloseAp').style.display === 'inline-flex'")
        assert page.locator("#diagAp").text_content() != "Off"

        with page.expect_response(lambda r: r.url.endswith("/api/setup-ap/close") and r.request.method == "POST") as r_ap_cls:
            page.locator("#btnCloseAp").click()
        assert r_ap_cls.value.status == 200
        page.wait_for_function("() => document.getElementById('diagAp').textContent === 'Off'")
        assert not page.locator("#btnCloseAp").is_visible()

        # Reboot dismiss
        dialogs = []

        def on_dismiss(dlg):
            dialogs.append(dlg.message)
            dlg.dismiss()

        page.once("dialog", on_dismiss)
        dev_ctrl.clear_requests()
        page.get_by_role("button", name="Reboot").click()
        assert dialogs == ["Reboot the Power Outage Monitor?"]
        assert [r for r in dev_ctrl.get_requests() if r["path"] == "/api/reboot"] == []

        # Reboot accept
        page.once("dialog", lambda d: d.accept())
        with page.expect_response(lambda r: r.url.endswith("/api/reboot") and r.request.method == "POST") as r_reb:
            page.get_by_role("button", name="Reboot").click()
        assert r_reb.value.status == 200
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Device rebooting...'")
        assert errors == []
    finally:
        ctx.close()

    # Factory reset (double confirm: dismiss 2nd, then accept both)
    ctx2, page2, errors2 = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        page2.locator("#secCard .collapsible-trigger").click()

        seen_msgs = []
        actions = ["accept", "dismiss"]

        def handle_two(dlg):
            seen_msgs.append(dlg.message)
            act = actions.pop(0)
            if act == "accept":
                dlg.accept()
            else:
                dlg.dismiss()

        page2.on("dialog", handle_two)
        dev_ctrl.clear_requests()
        page2.get_by_role("button", name="Factory reset").click()
        assert len(seen_msgs) == 2
        assert [r for r in dev_ctrl.get_requests() if r["path"] == "/api/factory-reset"] == []
        page2.remove_listener("dialog", handle_two)

        accept_msgs = []

        def accept_all(dlg):
            accept_msgs.append(dlg.message)
            dlg.accept()

        page2.on("dialog", accept_all)
        with page2.expect_response(lambda r: r.url.endswith("/api/factory-reset") and r.request.method == "POST") as r_fr:
            page2.get_by_role("button", name="Factory reset").click()
        assert r_fr.value.status == 200
        assert len(accept_msgs) == 2
        page2.wait_for_function(
            "() => document.getElementById('toast').textContent === 'Factory reset complete. Rebooting...'"
        )
        assert errors2 == []
    finally:
        ctx2.close()


def test_U7_captive_portal_ap_host_vs_lan_host(browser, dev_ctrl):
    dev_ctrl.set_state({
        "setup_ap": {
            "active": True,
            "ssid": "PowerMonitor-A1B2",
            "ip": "192.168.4.1",
            "clients": 1,
            "closes_in_s": 120,
        },
        "config": {"wifi": [{"slot": 0, "ssid": "", "pass": ""}]},
    })

    # AP host 192.168.4.1 prioritizes Wi-Fi setup and reveals apSavedBanner on save
    ctx_ap, page_ap, errors_ap = _open_page(browser, f"http://192.168.4.1:{dev_ctrl.port}/")
    try:
        page_ap.wait_for_function("() => document.getElementById('wifiCard').classList.contains('ap-top')")
        assert "open" in page_ap.locator("#body_wifi").get_attribute("class").split()
        assert page_ap.locator("#apHintBanner").is_visible()

        page_ap.locator("#w_ssid_0").fill("MyHomeWiFi")
        page_ap.locator("#w_pass_0").fill("homepass123")
        with page_ap.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_save:
            page_ap.locator("#body_wifi button.btn-save").click()
        assert r_save.value.status == 200
        page_ap.wait_for_function("() => document.getElementById('apSavedBanner').style.display === 'block'")
        assert "http://power-monitor.local/" in page_ap.locator("#apSavedBanner").text_content()
        assert errors_ap == []
    finally:
        ctx_ap.close()

    # Non-AP LAN host (127.0.0.1) keeps dashboard order even while setup_ap.active is true
    ctx_lan, page_lan, errors_lan = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        assert "ap-top" not in page_lan.locator("#wifiCard").get_attribute("class").split()
        assert not page_lan.locator("#apHintBanner").is_visible()
        assert errors_lan == []
    finally:
        ctx_lan.close()


def test_U8_cloud_switch_toggle_and_url_preserved(browser, dev_ctrl):
    hits = []
    hits_lock = threading.Lock()

    class _CloudHook(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length", "0"))
            if n > 0:
                self.rfile.read(n)
            with hits_lock:
                hits.append(self.path)
            self.send_response(200)
            self.end_headers()

        def log_message(self, fmt, *args):
            pass

    hook_srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _CloudHook)
    hook_thread = threading.Thread(target=hook_srv.serve_forever, daemon=True)
    hook_thread.start()

    ctx, page, errors = _open_page(browser, f"{dev_ctrl.base_url}/")
    try:
        hook_port = hook_srv.server_address[1]
        cloud_url = f"http://127.0.0.1:{hook_port}/cloud/{{event}}"

        page.wait_for_function(
            "() => !!cfg && document.getElementById('cloudNotifyStatus').style.display !== 'none' && "
            "document.getElementById('cloudNotifyStatus').textContent === 'Cloud: off'"
        )
        page.locator("#devCard .collapsible-trigger").click()
        assert not page.locator("#cfg_cloud_enabled").is_checked()
        assert page.locator("#cloudSwitchState").text_content() == "Off"
        assert (
            "Turn on when you are away from home; at home the phone on Wi-Fi is alerted directly."
            in page.locator("#body_dev").text_content()
        )

        # Save cloud URL while switch remains off -> Cloud stays off and Test Alarm skips cloud
        page.locator("#cfg_webhook").fill(cloud_url)
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_url:
            page.locator("#body_dev button[type='submit']").click()
        assert r_url.value.status == 200
        page.wait_for_function("() => document.getElementById('toast').textContent === 'Device settings saved'")
        assert page.locator("#cloudNotifyStatus").text_content() == "Cloud: off"

        with page.expect_response(lambda r: r.url.endswith("/api/test") and r.request.method == "POST"):
            page.locator("button.btn-test").click()
        dev_ctrl.server.state.wait_for_notifications(timeout=2.0)
        with hits_lock:
            assert hits == []
        assert page.locator("#cloudNotifyStatus").text_content() == "Cloud: off"

        # Turn switch On and save -> Test Alarm hits cloud and updates Cloud pill
        page.locator("label[for='cfg_cloud_enabled']").click()
        assert page.locator("#cfg_cloud_enabled").is_checked()
        assert page.locator("#cloudSwitchState").text_content() == "On"
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_on:
            page.locator("#body_dev button[type='submit']").click()
        assert r_on.value.status == 200
        on_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert on_posts[0]["json"] == {"cloud_enabled": True}
        page.wait_for_function("() => document.getElementById('cloudNotifyStatus').textContent === 'Cloud: not tested yet'")

        with page.expect_response(lambda r: r.url.endswith("/api/test") and r.request.method == "POST"):
            page.locator("button.btn-test").click()
        dev_ctrl.server.state.wait_for_notifications(timeout=2.0)
        page.wait_for_function("() => document.getElementById('cloudNotifyStatus').textContent.startsWith('Cloud: OK (test ')")
        with hits_lock:
            assert hits == ["/cloud/test_alarm"]
            hits.clear()

        # Turn switch Off and save -> URL preserved, Cloud pill returns to 'Cloud: off', reload persists, cloud skipped
        page.locator("label[for='cfg_cloud_enabled']").click()
        assert not page.locator("#cfg_cloud_enabled").is_checked()
        assert page.locator("#cloudSwitchState").text_content() == "Off"
        dev_ctrl.clear_requests()
        with page.expect_response(lambda r: r.url.endswith("/api/config") and r.request.method == "POST") as r_off:
            page.locator("#body_dev button[type='submit']").click()
        assert r_off.value.status == 200
        off_posts = [r for r in dev_ctrl.get_requests() if r["method"] == "POST" and r["path"] == "/api/config"]
        assert off_posts[0]["json"] == {"cloud_enabled": False}
        assert page.locator("#cfg_webhook").input_value() == cloud_url
        page.wait_for_function("() => document.getElementById('cloudNotifyStatus').textContent === 'Cloud: off'")

        page.reload(wait_until="domcontentloaded")
        page.wait_for_function(
            "() => !!cfg && document.getElementById('cloudNotifyStatus').style.display !== 'none' && "
            "document.getElementById('cloudNotifyStatus').textContent === 'Cloud: off'"
        )
        page.locator("#devCard .collapsible-trigger").click()
        assert not page.locator("#cfg_cloud_enabled").is_checked()
        assert page.locator("#cfg_webhook").input_value() == cloud_url

        with page.expect_response(lambda r: r.url.endswith("/api/test") and r.request.method == "POST"):
            page.locator("button.btn-test").click()
        dev_ctrl.server.state.wait_for_notifications(timeout=2.0)
        with hits_lock:
            assert hits == []

        assert errors == []
    finally:
        ctx.close()
        hook_srv.shutdown()
        hook_srv.server_close()
