#include <unity.h>

#include "fakes.h"
#include "setup_ap.h"
#include "wifi_connector.h"

namespace {

void stepMs(FakeClock& clk, SetupApController& ap, uint32_t totalMs, uint32_t step = 100) {
    uint32_t elapsed = 0;
    while (elapsed < totalMs) {
        uint32_t dt = (totalMs - elapsed < step) ? (totalMs - elapsed) : step;
        clk.advance(dt);
        ap.update();
        elapsed += dt;
    }
}

DeviceConfig makeSingleWifiCfg(const char* ssid, const char* pass, uint16_t setupWindowS = 0) {
    DeviceConfig cfg = makeDefaultTestConfig();
    cfg.setup_window_s = setupWindowS;
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "%s", ssid);
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "%s", pass);
    return cfg;
}

bool advanceUntilBackoff(FakeClock& clk, SetupApController& ap, WifiConnector& conn, uint32_t maxMs) {
    uint32_t elapsed = 0;
    while (elapsed < maxMs) {
        if (conn.getState() == WIFI_CONN_BACKOFF) return true;
        clk.advance(50);
        ap.update();
        elapsed += 50;
    }
    return conn.getState() == WIFI_CONN_BACKOFF;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_W1_first_boot_no_networks_permanent_ap_no_sta(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    TEST_ASSERT_TRUE(ap.isApMode());
    TEST_ASSERT_FALSE(ap.hasSavedNetworks());
    TEST_ASSERT_EQUAL_UINT32(0, ap.getApClosesInSeconds());
    TEST_ASSERT_EQUAL_INT(WIFI_AP, wifi.getMode());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("begin"));
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStart"));

    stepMs(clk, ap, 600000, 1000);
    TEST_ASSERT_TRUE(ap.isApMode());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("begin"));
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStart"));
}

void test_W2_fast_path_last_good_succeeds_without_scan(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Slot1Ssid", "pass1111", 100, 6, "192.168.1.42");
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);

    DeviceConfig cfg = makeDefaultTestConfig();
    cfg.setup_window_s = 0;
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "Slot0Ssid");
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "pass0000");
    snprintf(cfg.wifi[1].ssid, sizeof(cfg.wifi[1].ssid), "Slot1Ssid");
    snprintf(cfg.wifi[1].pass, sizeof(cfg.wifi[1].pass), "pass1111");

    conn.init(cfg.wifi, 1, cfg.device_name);
    ap.init(cfg, 0xAABBCCDDEEFFULL);

    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStart"));
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("begin"));
    bool foundSlot1 = false;
    for (size_t i = 0; i < wifi.calls().size(); i++) {
        if (wifi.calls()[i].fn == "begin") {
            TEST_ASSERT_EQUAL_STRING("Slot1Ssid", wifi.calls()[i].arg1.c_str());
            foundSlot1 = true;
        }
    }
    TEST_ASSERT_TRUE(foundSlot1);

    stepMs(clk, ap, 200, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT8(1, conn.getConnectedSlot());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStart"));
}

void test_W3_fast_path_missing_scans_picks_strongest_and_tie_breaks_lower_slot(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    // List NetSlot3 before NetSlot1 at the same RSSI so scan-order selection cannot pass
    wifi.addScanAp("NetSlot2", -65, false, 1);
    wifi.addScanAp("NetSlot3", -45, false, 11);
    wifi.addScanAp("NetSlot1", -45, false, 6);
    wifi.addAcceptCred("NetSlot1", "p1111111", 50, 6);
    wifi.addAcceptCred("NetSlot2", "p2222222", 50, 1);
    wifi.addAcceptCred("NetSlot3", "p3333333", 50, 11);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();
    cfg.setup_window_s = 0;
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "MissingSlot0");
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "p0000000");
    snprintf(cfg.wifi[1].ssid, sizeof(cfg.wifi[1].ssid), "NetSlot1");
    snprintf(cfg.wifi[1].pass, sizeof(cfg.wifi[1].pass), "p1111111");
    snprintf(cfg.wifi[2].ssid, sizeof(cfg.wifi[2].ssid), "NetSlot2");
    snprintf(cfg.wifi[2].pass, sizeof(cfg.wifi[2].pass), "p2222222");
    snprintf(cfg.wifi[3].ssid, sizeof(cfg.wifi[3].ssid), "NetSlot3");
    snprintf(cfg.wifi[3].pass, sizeof(cfg.wifi[3].pass), "p3333333");

    wifi.setScanDurationMs(200);
    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    // Wait out fast-path timeout on MissingSlot0
    stepMs(clk, ap, WIFI_CONNECT_TIMEOUT_MS + 100, 200);
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("scanStart"));

    // Wait for scan completion + candidate connect
    stepMs(clk, ap, 400, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT8(1, conn.getConnectedSlot());
    uint8_t newLast = 99;
    TEST_ASSERT_TRUE(conn.consumeLastGoodSlotChanged(newLast));
    TEST_ASSERT_EQUAL_UINT8(1, newLast);
}

void test_W4_hidden_ssid_tried_after_visible_candidates(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addScanAp("VisibleWrongPass", -50, false, 1);
    wifi.addAcceptCred("HiddenNet", "hiddenpass", 50, 11);
    wifi.setScanDurationMs(100);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();
    cfg.setup_window_s = 0;
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "HiddenNet");
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "hiddenpass");
    snprintf(cfg.wifi[1].ssid, sizeof(cfg.wifi[1].ssid), "VisibleWrongPass");
    snprintf(cfg.wifi[1].pass, sizeof(cfg.wifi[1].pass), "wrongpass");
    snprintf(cfg.wifi[2].ssid, sizeof(cfg.wifi[2].ssid), "GoneFastSlot");
    snprintf(cfg.wifi[2].pass, sizeof(cfg.wifi[2].pass), "gone1234");

    // Set lastGood to missing slot 2 so fast-path fails first; the scan must
    // then try visible slot 1 before hidden/unseen slot 0.
    conn.init(cfg.wifi, 2, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    stepMs(clk, ap, WIFI_CONNECT_TIMEOUT_MS + 300, 100);

    std::vector<std::string> begins;
    for (size_t i = 0; i < wifi.calls().size(); i++) {
        if (wifi.calls()[i].fn == "begin") begins.push_back(wifi.calls()[i].arg1);
    }
    TEST_ASSERT_TRUE(begins.size() >= 2);
    TEST_ASSERT_EQUAL_STRING("GoneFastSlot", begins[0].c_str());
    TEST_ASSERT_EQUAL_STRING("VisibleWrongPass", begins[1].c_str());

    // Wait out timeout on VisibleWrongPass -> hidden slot 0 is tried and succeeds
    stepMs(clk, ap, WIFI_CONNECT_TIMEOUT_MS + 200, 100);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT8(0, conn.getConnectedSlot());
}

void test_W5_all_fail_backoff_15_30_60_60_cycle_repeats(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.setScanDurationMs(100);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Unreachable", "12345678", 0);

    conn.init(cfg.wifi, WIFI_SLOT_NONE, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    // 1. Enter backoff[0] = 15s
    TEST_ASSERT_TRUE(advanceUntilBackoff(clk, ap, conn, 15000));
    int scansBefore15 = wifi.countCalls("scanStart");
    stepMs(clk, ap, 14800, 200);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_BACKOFF, conn.getState());
    TEST_ASSERT_EQUAL_INT(scansBefore15, wifi.countCalls("scanStart"));
    stepMs(clk, ap, 300, 50);
    TEST_ASSERT_TRUE(wifi.countCalls("scanStart") > scansBefore15);

    // 2. Fail cycle -> backoff[1] = 30s
    TEST_ASSERT_TRUE(advanceUntilBackoff(clk, ap, conn, 15000));
    int scansBefore30 = wifi.countCalls("scanStart");
    stepMs(clk, ap, 29800, 200);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_BACKOFF, conn.getState());
    TEST_ASSERT_EQUAL_INT(scansBefore30, wifi.countCalls("scanStart"));
    stepMs(clk, ap, 300, 50);
    TEST_ASSERT_TRUE(wifi.countCalls("scanStart") > scansBefore30);

    // 3. Fail cycle -> backoff[2] = 60s
    TEST_ASSERT_TRUE(advanceUntilBackoff(clk, ap, conn, 15000));
    int scansBefore60a = wifi.countCalls("scanStart");
    stepMs(clk, ap, 59800, 500);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_BACKOFF, conn.getState());
    TEST_ASSERT_EQUAL_INT(scansBefore60a, wifi.countCalls("scanStart"));
    stepMs(clk, ap, 300, 50);
    TEST_ASSERT_TRUE(wifi.countCalls("scanStart") > scansBefore60a);

    // 4. Fail cycle -> backoff[3] = 60s (repeats cap)
    TEST_ASSERT_TRUE(advanceUntilBackoff(clk, ap, conn, 15000));
    int scansBefore60b = wifi.countCalls("scanStart");
    stepMs(clk, ap, 59800, 500);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_BACKOFF, conn.getState());
    TEST_ASSERT_EQUAL_INT(scansBefore60b, wifi.countCalls("scanStart"));
    stepMs(clk, ap, 300, 50);
    TEST_ASSERT_TRUE(wifi.countCalls("scanStart") > scansBefore60b);
}

void test_W6_drop_restarts_with_lost_slot_first_and_no_double_try_on_fail(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Slot0", "pass0000", 50, 1);
    wifi.addScanAp("Slot0", -50, false, 1);
    wifi.addScanAp("Slot1", -60, false, 6);
    wifi.setScanDurationMs(100);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();
    cfg.setup_window_s = 0;
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "Slot0");
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "pass0000");
    snprintf(cfg.wifi[1].ssid, sizeof(cfg.wifi[1].ssid), "Slot1");
    snprintf(cfg.wifi[1].pass, sizeof(cfg.wifi[1].pass), "pass1111");

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());

    // Drop connection and remove Slot0 acceptance so fast path fails on Slot0
    wifi.clearAcceptCreds();
    wifi.clearCalls();
    wifi.injectDrop();
    stepMs(clk, ap, 50, 50);

    // Lost slot (Slot0) must be tried immediately via fast path
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("begin"));
    bool foundSlot0Begin = false;
    for (size_t i = 0; i < wifi.calls().size(); i++) {
        if (wifi.calls()[i].fn == "begin") {
            TEST_ASSERT_EQUAL_STRING("Slot0", wifi.calls()[i].arg1.c_str());
            foundSlot0Begin = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(foundSlot0Begin);

    // Wait for fast-path Slot0 timeout + scan
    stepMs(clk, ap, WIFI_CONNECT_TIMEOUT_MS + 300, 100);

    // After scan, Slot0 must NOT be tried a second time; next candidate is Slot1
    int slot0Count = 0;
    int slot1Count = 0;
    for (size_t i = 0; i < wifi.calls().size(); i++) {
        if (wifi.calls()[i].fn == "begin") {
            if (wifi.calls()[i].arg1 == "Slot0") slot0Count++;
            if (wifi.calls()[i].arg1 == "Slot1") slot1Count++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, slot0Count);
    TEST_ASSERT_EQUAL_INT(1, slot1Count);
}

void test_W7_ap_station_pauses_connector_stops_scan_and_resumes_after_leave(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.setScanDurationMs(5000);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 60);

    conn.init(cfg.wifi, 1, cfg.device_name); // lastGood=1 empty -> starts scan
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_TRUE(conn.isScanning());

    wifi.clearCalls();
    wifi.setApClients(1);
    stepMs(clk, ap, 50, 50);

    TEST_ASSERT_EQUAL_INT(WIFI_CONN_PAUSED_AP_CLIENT, conn.getState());
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("scanStop"));
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("scanDelete"));

    int scansWhilePaused = wifi.countCalls("scanStart");
    int beginsWhilePaused = wifi.countCalls("begin");
    stepMs(clk, ap, 5000, 200);
    TEST_ASSERT_EQUAL_INT(scansWhilePaused, wifi.countCalls("scanStart"));
    TEST_ASSERT_EQUAL_INT(beginsWhilePaused, wifi.countCalls("begin"));

    // Station leaves -> connector resumes
    wifi.setApClients(0);
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_TRUE(conn.getState() != WIFI_CONN_PAUSED_AP_CLIENT);
    TEST_ASSERT_TRUE(wifi.countCalls("scanStart") + wifi.countCalls("begin") > scansWhilePaused + beginsWhilePaused);
}

void test_W8_web_scan_abandoned_recovers_within_15s(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 1);
    wifi.setScanDurationMs(100);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);

    // Connect STA first via initial scan so connector is idle when web scan starts
    conn.init(cfg.wifi, 1, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 200, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());

    // Start a web scan that never completes, then stop polling
    const int scansBefore = wifi.countCalls("scanStart");
    const int stopsBefore = wifi.countCalls("scanStop");
    wifi.setScanStuck(true);
    const char* json = ap.pollWifiScanJsonPtr();
    TEST_ASSERT_NOT_NULL(strstr(json, "\"scanning\""));
    TEST_ASSERT_EQUAL_INT(scansBefore + 1, wifi.countCalls("scanStart"));
    TEST_ASSERT_EQUAL_INT(stopsBefore, wifi.countCalls("scanStop"));

    // Still pending just before WIFI_SCAN_TIMEOUT_MS
    stepMs(clk, ap, WIFI_SCAN_TIMEOUT_MS - 1000, 500);
    TEST_ASSERT_EQUAL_INT(stopsBefore, wifi.countCalls("scanStop"));

    // Past WIFI_SCAN_TIMEOUT_MS without polling: scan is force-stopped and cleared
    stepMs(clk, ap, 1500, 250);
    TEST_ASSERT_TRUE(wifi.countCalls("scanStop") > stopsBefore);
    wifi.setScanStuck(false);

    // Next poll returns cached empty networks without re-triggering a new scan
    json = ap.pollWifiScanJsonPtr();
    TEST_ASSERT_EQUAL_STRING("{\"networks\":[]}", json);
    TEST_ASSERT_EQUAL_INT(scansBefore + 1, wifi.countCalls("scanStart"));
}

void test_W9_disconnected_timer_accurate_no_uptime_value(void) {
    FakeClock clk(5000000UL);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 1);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_UINT32(0, conn.getDisconnectedDurationMs());

    // While staying connected for 100s, disconnected duration remains 0
    stepMs(clk, ap, 100000, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, conn.getDisconnectedDurationMs());

    // Inject drop, disable creds -> advance 12300ms
    wifi.clearAcceptCreds();
    wifi.injectDrop();
    stepMs(clk, ap, 50, 50);
    TEST_ASSERT_TRUE(conn.getDisconnectedDurationMs() <= 50);
    stepMs(clk, ap, 12300, 100);
    uint32_t d = conn.getDisconnectedDurationMs();
    TEST_ASSERT_TRUE(d >= 12300 && d <= 12400);
}

void test_W10_5min_offline_fallback_ap_once_rearms_5min_after_close(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Missing", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_FALSE(ap.isApMode());

    // Before NO_STA_AP_FALLBACK_MS -> AP still off
    stepMs(clk, ap, NO_STA_AP_FALLBACK_MS - 1000, 1000);
    TEST_ASSERT_FALSE(ap.isApMode());

    // Cross fallback threshold -> fallback AP opens for SETUP_FALLBACK_WINDOW_S
    stepMs(clk, ap, 2000, 200);
    TEST_ASSERT_TRUE(ap.isApMode());

    // Nobody connects -> closes after SETUP_FALLBACK_WINDOW_S, starts cooldown
    stepMs(clk, ap, (uint32_t)SETUP_FALLBACK_WINDOW_S * 1000UL, 200);
    TEST_ASSERT_FALSE(ap.isApMode());

    // Before NO_STA_AP_FALLBACK_MS after close -> still off
    stepMs(clk, ap, NO_STA_AP_FALLBACK_MS - 2000, 1000);
    TEST_ASSERT_FALSE(ap.isApMode());

    // Cross NO_STA_AP_FALLBACK_MS after close -> fallback AP reopens
    stepMs(clk, ap, 3000, 200);
    TEST_ASSERT_TRUE(ap.isApMode());
}

void test_W11_adding_slot3_while_connected_slot0_no_disconnect(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Slot0", "pass0000", 50, 1);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Slot0", "pass0000", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());

    wifi.clearCalls();
    snprintf(cfg.wifi[3].ssid, sizeof(cfg.wifi[3].ssid), "BackupNet");
    snprintf(cfg.wifi[3].pass, sizeof(cfg.wifi[3].pass), "backup123");
    conn.setNetworks(cfg.wifi, cfg.device_name, false);

    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("disconnect"));
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("begin"));
}

void test_W12_changing_connected_slot_password_reconnects_after_reply(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Slot0", "oldpass00", 50, 1);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Slot0", "oldpass00", 60);
    FakeSystemActions actions(&ap, &cfg);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL, &actions);
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());

    wifi.clearCalls();
    wifi.clearAcceptCreds();
    wifi.addAcceptCred("Slot0", "newpass11", 50, 1);

    DeviceConfig newCfg = cfg;
    snprintf(newCfg.wifi[0].pass, sizeof(newCfg.wifi[0].pass), "newpass11");
    ap.requestApplyConfig(newCfg, true);

    // Immediately (0ms, HTTP reply phase): no disconnect yet
    ap.update();
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("disconnect"));

    // After deferred delay -> disconnects and reconnects with new password
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS + 50, 50);
    TEST_ASSERT_EQUAL_INT(1, actions.applyCount);
    TEST_ASSERT_TRUE(actions.lastTriggerReconnect);
    TEST_ASSERT_TRUE(wifi.countCalls("disconnect") >= 1);

    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    int beginWithNewPass = 0;
    for (size_t i = 0; i < wifi.calls().size(); i++) {
        if (wifi.calls()[i].fn == "begin" && wifi.calls()[i].arg2 == "newpass11") {
            beginWithNewPass++;
        }
    }
    TEST_ASSERT_TRUE(beginWithNewPass >= 1);
}

void test_W13_connector_scan_stuck_15s_guard(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.setScanStuck(true);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("AnyNet", "pass1234", 0);

    conn.init(cfg.wifi, 1, cfg.device_name); // starts scan immediately
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_TRUE(conn.isScanning());

    stepMs(clk, ap, WIFI_SCAN_TIMEOUT_MS - 500, 500);
    TEST_ASSERT_TRUE(conn.isScanning());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStop"));

    stepMs(clk, ap, 1000, 250);
    TEST_ASSERT_FALSE(conn.isScanning());
    TEST_ASSERT_TRUE(wifi.countCalls("scanStop") >= 1);
    TEST_ASSERT_TRUE(wifi.countCalls("scanDelete") >= 1);
}

void test_W14_connect_timeout_across_millis_wrap(void) {
    FakeClock clk(0xFFFFFFFFUL - 3000UL);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("MissingNet", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_FAST_PATH, conn.getState());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStart"));

    // Advance across 0xFFFFFFFF wrap to 100 ms before WIFI_CONNECT_TIMEOUT_MS
    stepMs(clk, ap, WIFI_CONNECT_TIMEOUT_MS - 100UL, 500);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_FAST_PATH, conn.getState());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("scanStart"));

    // Cross WIFI_CONNECT_TIMEOUT_MS -> transitions to scan
    stepMs(clk, ap, 200UL, 50);
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("scanStart"));
    TEST_ASSERT_TRUE(conn.isScanning());
}

void test_A1_boot_window_60s_nobody_joins_closes_sta_untouched(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 6);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", DEFAULT_SETUP_WINDOW_S);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_TRUE(ap.isApMode());

    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());

    wifi.clearCalls();
    stepMs(clk, ap, (uint32_t)DEFAULT_SETUP_WINDOW_S * 1000UL - 500UL, 500);
    TEST_ASSERT_TRUE(ap.isApMode());

    stepMs(clk, ap, 1000, 200);
    TEST_ASSERT_FALSE(ap.isApMode());
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("disconnect"));
}

void test_A2_client_joins_stays_10min_idle_120s_and_30min_hard_cap(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", DEFAULT_SETUP_WINDOW_S);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_TRUE(ap.isApMode());

    // Client joins at 50s and stays 10 minutes
    stepMs(clk, ap, 50000, 1000);
    wifi.setApClients(1);
    stepMs(clk, ap, 600000, 5000);
    TEST_ASSERT_TRUE(ap.isApMode());

    // Client leaves -> closes after SETUP_IDLE_TIMEOUT_S
    wifi.setApClients(0);
    stepMs(clk, ap, (uint32_t)SETUP_IDLE_TIMEOUT_S * 1000UL - 2000UL, 1000);
    TEST_ASSERT_TRUE(ap.isApMode());
    stepMs(clk, ap, 3000, 500);
    TEST_ASSERT_FALSE(ap.isApMode());

    // Re-open AP on demand with a permanently attached client -> closes at SETUP_HARD_CAP_S
    ap.openSetupAp(SETUP_ONDEMAND_WINDOW_S);
    wifi.setApClients(1);
    stepMs(clk, ap, (uint32_t)SETUP_HARD_CAP_S * 1000UL - 2000UL, 5000);
    TEST_ASSERT_TRUE(ap.isApMode());
    stepMs(clk, ap, 5000, 1000);
    TEST_ASSERT_FALSE(ap.isApMode());
}

void test_A3_first_save_via_permanent_ap_converts_to_timed_window_and_closes(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_TRUE(ap.isApMode());
    TEST_ASSERT_EQUAL_UINT32(0, ap.getApClosesInSeconds());

    DeviceConfig saved = makeSingleWifiCfg("MyHome", "mypassword", DEFAULT_SETUP_WINDOW_S);
    cfg = saved;
    ap.onConfigApplied(cfg, true);

    TEST_ASSERT_TRUE(ap.isApMode());
    uint32_t rem = ap.getApClosesInSeconds();
    TEST_ASSERT_TRUE(rem >= SETUP_FALLBACK_WINDOW_S - 1 && rem <= SETUP_FALLBACK_WINDOW_S);

    stepMs(clk, ap, ((uint32_t)SETUP_FALLBACK_WINDOW_S + 1UL) * 1000UL, 1000);
    TEST_ASSERT_FALSE(ap.isApMode());
}

void test_A4_window_zero_no_boot_ap_when_networks_exist(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    TEST_ASSERT_FALSE(ap.isApMode());
    TEST_ASSERT_EQUAL_INT(WIFI_STA, wifi.getMode());
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("softApStart"));
}

void test_A5_ap_opened_while_sta_on_channel_6_uses_channel_6(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 6);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT(6, wifi.channel());

    ap.openSetupAp(SETUP_ONDEMAND_WINDOW_S);
    TEST_ASSERT_TRUE(ap.isApMode());
    TEST_ASSERT_EQUAL_INT(6, wifi.softApChannel());
}

void test_A6_closes_in_s_hard_cap_and_distinct_ips_in_status(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 6, "10.0.0.77");
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", DEFAULT_SETUP_WINDOW_S);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 100, 50);

    // Client attached -> closes_in_s reflects hard cap, not 0
    wifi.setApClients(1);
    ap.update();
    uint32_t closesWhileAttached = ap.getApClosesInSeconds();
    TEST_ASSERT_TRUE(closesWhileAttached >= SETUP_HARD_CAP_S - 5 && closesWhileAttached <= SETUP_HARD_CAP_S);

    char staIp[20];
    char apIp[20];
    wifi.localIpStr(staIp, sizeof(staIp));
    ap.getApIpAddress(apIp, sizeof(apIp));

    StatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.deviceName = cfg.device_name;
    snap.gridState = GRID_ONLINE;
    snap.mode = MODE_ARMED;
    snap.wifiConnected = true;
    snap.wifiSsid = "Home";
    snap.wifiSlot = 0;
    snap.wifiIp = staIp;
    snap.wifiRssi = -55;
    snap.apActive = ap.isApMode();
    snap.apSsid = ap.getApSsid();
    snap.apIp = apIp;
    snap.apClients = ap.getApClientCount();
    snap.apClosesInS = closesWhileAttached;

    char json[1024];
    buildStatusJsonBuf(snap, json, sizeof(json));

    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, json).code());
    TEST_ASSERT_EQUAL_STRING("10.0.0.77", doc["wifi"]["ip"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING(SETUP_AP_IP_STR, doc["setup_ap"]["ip"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(closesWhileAttached, doc["setup_ap"]["closes_in_s"].as<uint32_t>());
}

void test_A7_ondemand_open_300s_and_deferred_close_via_api(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 1);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    stepMs(clk, ap, 100, 50);

    // Request deferred open (as POST /api/setup-ap does) -> not open until after reply delay
    ap.requestOpenSetupAp(SETUP_ONDEMAND_WINDOW_S);
    ap.update();
    TEST_ASSERT_FALSE(ap.isApMode());

    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS + 50, 50);
    TEST_ASSERT_TRUE(ap.isApMode());
    uint32_t rem = ap.getApClosesInSeconds();
    TEST_ASSERT_TRUE(rem >= SETUP_ONDEMAND_WINDOW_S - 2 && rem <= SETUP_ONDEMAND_WINDOW_S);

    // Also verify reconnectWiFi() closes the AP if needed and initiates a STA reconnect
    ap.requestCloseSetupAp();
    ap.update();
    TEST_ASSERT_TRUE(ap.isApMode()); // still open during deferred flush delay
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS + 50, 50);
    TEST_ASSERT_FALSE(ap.isApMode());

    int beginBefore = wifi.countCalls("begin");
    ap.reconnectWiFi();
    TEST_ASSERT_FALSE(ap.isApMode());
    TEST_ASSERT_TRUE(wifi.countCalls("begin") > beginBefore);
}

void test_A8_boot_window_closes_across_millis_wrap(void) {
    FakeClock clk(0xFFFFFFFFUL - 5000UL);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 1);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", DEFAULT_SETUP_WINDOW_S);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    TEST_ASSERT_TRUE(ap.isApMode());

    // Advance across 0xFFFFFFFF wrap to 500 ms before setup window expires
    stepMs(clk, ap, (uint32_t)DEFAULT_SETUP_WINDOW_S * 1000UL - 500UL, 500);
    TEST_ASSERT_TRUE(ap.isApMode());

    // Cross window boundary -> AP closes
    stepMs(clk, ap, 1000UL, 200);
    TEST_ASSERT_FALSE(ap.isApMode());
}

void test_A9_deferred_reboot_and_factory_reset_reach_hal_once(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);
    FakeSystemActions actions(&ap, &cfg);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL, &actions);

    ap.requestReboot();
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS - 1, DEFERRED_APPLY_DELAY_MS - 1);
    TEST_ASSERT_EQUAL_INT(0, actions.rebootCount);
    stepMs(clk, ap, 1, 1);
    TEST_ASSERT_EQUAL_INT(1, actions.rebootCount);
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS, 50);
    TEST_ASSERT_EQUAL_INT(1, actions.rebootCount);

    ap.requestFactoryReset();
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS - 1, DEFERRED_APPLY_DELAY_MS - 1);
    TEST_ASSERT_EQUAL_INT(0, actions.factoryResetCount);
    stepMs(clk, ap, 1, 1);
    TEST_ASSERT_EQUAL_INT(1, actions.factoryResetCount);
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS, 50);
    TEST_ASSERT_EQUAL_INT(1, actions.factoryResetCount);
}

void test_A10_web_scan_json_sorted_by_rssi_and_escapes_ssid(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addScanAp("WeakNet", -80, true, 1);
    wifi.addScanAp("Quote\"And\\Slash", -40, false, 6);
    wifi.addScanAp("MidNet", -60, false, 11);
    wifi.setScanDurationMs(100);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    const char* first = ap.pollWifiScanJsonPtr();
    TEST_ASSERT_NOT_NULL(strstr(first, "\"status\":\"scanning\""));

    stepMs(clk, ap, 150, 50);
    const char* jsonStr = ap.pollWifiScanJsonPtr();
    TEST_ASSERT_NOT_NULL(strstr(jsonStr, "Quote\\\"And\\\\Slash"));

    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, jsonStr).code());
    JsonArrayConst nets = doc["networks"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(3, (unsigned int)nets.size());
    TEST_ASSERT_EQUAL_STRING("Quote\"And\\Slash", nets[0]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_INT32(-40, nets[0]["rssi"].as<int32_t>());
    TEST_ASSERT_TRUE(nets[0]["secure"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("MidNet", nets[1]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_INT32(-60, nets[1]["rssi"].as<int32_t>());
    TEST_ASSERT_EQUAL_STRING("WeakNet", nets[2]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_INT32(-80, nets[2]["rssi"].as<int32_t>());
    TEST_ASSERT_FALSE(nets[2]["secure"].as<bool>());
}

void test_W15_force_reconnect_while_ap_client_runs_on_leave(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("Home", "pass1234", 50, 1);
    WifiConnector conn(wifi, clk);
    DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    conn.update(false, false);
    clk.advance(60);
    conn.update(false, false);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_CONNECTED, conn.getState());

    wifi.setApClients(1);
    int discBefore = wifi.countCalls("disconnect");
    conn.forceReconnect();
    // Deferred while AP client is present
    TEST_ASSERT_EQUAL_INT(discBefore, wifi.countCalls("disconnect"));
    conn.update(true, false);
    TEST_ASSERT_EQUAL_INT(discBefore, wifi.countCalls("disconnect"));

    // AP client leaves -> deferred forceReconnect runs
    wifi.setApClients(0);
    conn.update(false, false);
    TEST_ASSERT_TRUE(wifi.countCalls("disconnect") > discBefore);
}

void test_W16_external_connect_without_trying_slot_matches_or_unknown(void) {
    // 1. Observed SSID matches slot 1 when _tryingSlot == -1 -> _connectedSlot == 1 and w_last updated
    {
        FakeClock clk(1000);
        FakeWifiHal wifi(clk);
        wifi.setApClients(1); // keeps conn in WIFI_CONN_PAUSED_AP_CLIENT with _tryingSlot == -1
        WifiConnector conn(wifi, clk);
        DeviceConfig cfg = makeDefaultTestConfig();
        snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "NetA");
        snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "passA123");
        snprintf(cfg.wifi[1].ssid, sizeof(cfg.wifi[1].ssid), "NetB");
        snprintf(cfg.wifi[1].pass, sizeof(cfg.wifi[1].pass), "passB123");

        conn.init(cfg.wifi, 0, cfg.device_name);
        wifi.setApClients(0);
        wifi.simulateExternalConnect("NetB");
        conn.update(false, false);
        TEST_ASSERT_EQUAL_INT8(1, conn.getConnectedSlot());
        uint8_t slotOut = 255;
        TEST_ASSERT_TRUE(conn.consumeLastGoodSlotChanged(slotOut));
        TEST_ASSERT_EQUAL_UINT8(1, slotOut);
    }

    // 2. Observed SSID matches no slot when _tryingSlot == -1 -> _connectedSlot == -1 and no w_last write
    {
        FakeClock clk(1000);
        FakeWifiHal wifi(clk);
        wifi.setApClients(1);
        WifiConnector conn(wifi, clk);
        DeviceConfig cfg = makeSingleWifiCfg("NetA", "passA123", 0);

        conn.init(cfg.wifi, 0, cfg.device_name);
        wifi.setApClients(0);
        wifi.simulateExternalConnect("AlienNet");
        conn.update(false, false);
        TEST_ASSERT_EQUAL_INT8(-1, conn.getConnectedSlot());
        uint8_t slotOut = 255;
        TEST_ASSERT_FALSE(conn.consumeLastGoodSlotChanged(slotOut));
    }
}

void test_A11_softap_start_failure_restores_mode_and_retries(void) {
    // 1. Saved networks: initial boot window fails -> restores previous mode, retries and succeeds
    {
        FakeClock clk(1000);
        FakeWifiHal wifi(clk);
        wifi.setMode(WIFI_STA);
        wifi.setSoftApStartFailCount(1);
        WifiConnector conn(wifi, clk);
        SetupApController ap(wifi, clk, conn);
        DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 60);

        conn.init(cfg.wifi, 0, cfg.device_name);
        ap.init(cfg, 0x112233445566ULL);
        TEST_ASSERT_FALSE(ap.isApMode());
        TEST_ASSERT_FALSE(wifi.isSoftApActive());
        TEST_ASSERT_EQUAL_INT(WIFI_STA, wifi.getMode());

        stepMs(clk, ap, SETUP_AP_RETRY_MS - 100, 100);
        TEST_ASSERT_FALSE(ap.isApMode());
        stepMs(clk, ap, 200, 50);
        TEST_ASSERT_TRUE(ap.isApMode());
        TEST_ASSERT_TRUE(wifi.isSoftApActive());
    }

    // 2. No saved networks: keeps retrying across multiple failures until SoftAP starts
    {
        FakeClock clk(1000);
        FakeWifiHal wifi(clk);
        wifi.setSoftApStartFailCount(2);
        WifiConnector conn(wifi, clk);
        SetupApController ap(wifi, clk, conn);
        DeviceConfig cfg = makeDefaultTestConfig();

        conn.init(cfg.wifi, 0, cfg.device_name);
        ap.init(cfg, 0x112233445566ULL);
        TEST_ASSERT_FALSE(ap.isApMode());

        stepMs(clk, ap, SETUP_AP_RETRY_MS + 100, 100);
        TEST_ASSERT_FALSE(ap.isApMode());
        stepMs(clk, ap, SETUP_AP_RETRY_MS + 100, 100);
        TEST_ASSERT_TRUE(ap.isApMode());
        TEST_ASSERT_EQUAL_INT(3, wifi.countCalls("softApStart"));
    }
}

void test_A12_web_scan_json_overflow_drops_weakest_networks(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    for (int i = 0; i < 20; ++i) {
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "Network_%02d_With_Long_Name_SSID", i);
        wifi.addScanAp(ssid, -30 - i * 3, (i % 2) == 0, 1);
    }
    wifi.setScanDurationMs(100);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();
    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    // Drive an actual overflow against a tight 512-byte output buffer and verify weakest entries drop
    wifi.scanStart(true, false);
    clk.advance(150);
    int16_t sc = wifi.scanComplete();
    TEST_ASSERT_EQUAL_INT16(20, sc);

    char smallBuf[512];
    size_t written = ap.buildWebScanJson(sc, smallBuf, sizeof(smallBuf));
    TEST_ASSERT_TRUE(written > 0 && written < sizeof(smallBuf));

    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, smallBuf).code());
    JsonArrayConst nets = doc["networks"].as<JsonArrayConst>();
    TEST_ASSERT_TRUE(nets.size() > 0 && nets.size() < 20);
    TEST_ASSERT_EQUAL_INT32(-30, nets[0]["rssi"].as<int32_t>());
    for (size_t i = 1; i < nets.size(); ++i) {
        TEST_ASSERT_TRUE(nets[i - 1]["rssi"].as<int32_t>() >= nets[i]["rssi"].as<int32_t>());
    }
    wifi.scanDelete();
}

void test_A13_ap_cred_change_restarts_and_refresh_keeps_hard_cap(void) {
    // 1. AP credential change while AP active restarts SoftAP with new creds
    {
        FakeClock clk(1000);
        FakeWifiHal wifi(clk);
        WifiConnector conn(wifi, clk);
        SetupApController ap(wifi, clk, conn);
        DeviceConfig cfg = makeDefaultTestConfig();
        FakeSystemActions actions(&ap, &cfg);

        conn.init(cfg.wifi, 0, cfg.device_name);
        ap.init(cfg, 0x112233445566ULL, &actions);
        TEST_ASSERT_TRUE(ap.isApMode());
        int startsBefore = wifi.countCalls("softApStart");

        DeviceConfig next = cfg;
        snprintf(next.ap_ssid, sizeof(next.ap_ssid), "NewMonitorAP");
        snprintf(next.ap_pass, sizeof(next.ap_pass), "newsecret12");
        ap.requestApplyConfig(next, false);
        stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS + 50, 50);

        TEST_ASSERT_TRUE(ap.isApMode());
        TEST_ASSERT_TRUE(wifi.countCalls("softApStart") > startsBefore);
        TEST_ASSERT_EQUAL_STRING("NewMonitorAP", wifi.softApSsid().c_str());
        TEST_ASSERT_EQUAL_STRING("newsecret12", wifi.softApPass().c_str());
    }

    // 2. Refreshing an open AP does not reset the 30-min hard cap; close+reopen resets it
    {
        FakeClock clk(1000);
        FakeWifiHal wifi(clk);
        WifiConnector conn(wifi, clk);
        SetupApController ap(wifi, clk, conn);
        DeviceConfig cfg = makeSingleWifiCfg("Home", "pass1234", 60);

        conn.init(cfg.wifi, 0, cfg.device_name);
        ap.init(cfg, 0x112233445566ULL);
        wifi.setApClients(1);

        // Advance 20 min, then refresh the open AP
        stepMs(clk, ap, 1200000UL, 60000UL);
        TEST_ASSERT_TRUE(ap.isApMode());
        ap.openSetupAp(SETUP_ONDEMAND_WINDOW_S);

        // At 29m50s total from initial open, AP is still open; at 30m00s, hard cap closes it
        stepMs(clk, ap, 590000UL, 60000UL);
        TEST_ASSERT_TRUE(ap.isApMode());
        stepMs(clk, ap, 11000UL, 1000UL);
        TEST_ASSERT_FALSE(ap.isApMode());

        // Reopening after close starts a fresh 30-min cap
        ap.openSetupAp(SETUP_ONDEMAND_WINDOW_S);
        TEST_ASSERT_TRUE(ap.isApMode());
        TEST_ASSERT_TRUE(ap.getApClosesInSeconds() > 1700UL);
    }
}

void test_W17_first_network_saved_with_ap_client_attempts_immediately(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addAcceptCred("HomeNet", "secret123", 100, 6, "192.168.1.42");
    wifi.addScanAp("HomeNet", -55, false, 6);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);

    DeviceConfig cfg = makeDefaultTestConfig();
    conn.init(cfg.wifi, WIFI_SLOT_NONE, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);
    wifi.setApClients(1);

    DeviceConfig next = makeSingleWifiCfg("HomeNet", "secret123", 60);
    ap.onConfigApplied(next, true);
    clk.advance(50);
    conn.update(true, false);

    // Config save triggers an immediate attempt within one update even with an AP client present
    TEST_ASSERT_NOT_EQUAL_INT(WIFI_CONN_PAUSED_AP_CLIENT, conn.getState());
    TEST_ASSERT_TRUE((wifi.countCalls("scanStart") + wifi.countCalls("begin")) > 0);

    // Finish scan + connect while the AP client remains associated
    stepMs(clk, ap, 500, 50);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_CONNECTED, conn.getState());
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
}

void test_W18_background_retry_with_ap_client_stays_paused(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.setApClients(1);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);

    DeviceConfig cfg = makeDefaultTestConfig();
    conn.init(cfg.wifi, WIFI_SLOT_NONE, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    // First config-triggered attempt runs even with AP client, then fails all candidates
    DeviceConfig next = makeSingleWifiCfg("MissingNet", "secret123", 60);
    ap.onConfigApplied(next, true);
    uint32_t elapsed = 0;
    while (elapsed < 20000UL && conn.getState() != WIFI_CONN_PAUSED_AP_CLIENT) {
        clk.advance(100);
        ap.update();
        elapsed += 100;
    }
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_PAUSED_AP_CLIENT, conn.getState());

    int scansAfterFail = wifi.countCalls("scanStart");
    int beginsAfterFail = wifi.countCalls("begin");

    // Subsequent background retries remain paused while the AP client stays connected
    stepMs(clk, ap, 120000UL, 1000UL);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_PAUSED_AP_CLIENT, conn.getState());
    TEST_ASSERT_EQUAL_INT(scansAfterFail, wifi.countCalls("scanStart"));
    TEST_ASSERT_EQUAL_INT(beginsAfterFail, wifi.countCalls("begin"));

    // Leaving resumes background retries
    wifi.setApClients(0);
    clk.advance(50);
    ap.update();
    TEST_ASSERT_NOT_EQUAL_INT(WIFI_CONN_PAUSED_AP_CLIENT, conn.getState());
    TEST_ASSERT_TRUE((wifi.countCalls("scanStart") + wifi.countCalls("begin")) >
                     (scansAfterFail + beginsAfterFail));
}

void test_W19_no_last_good_slot_skips_fast_path_and_persists_connected_slot(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.addScanAp("HomeSlot0", -52, false, 6);
    wifi.addAcceptCred("HomeSlot0", "pass0000", 50, 6);
    wifi.setScanDurationMs(100);

    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("HomeSlot0", "pass0000", 0);

    conn.init(cfg.wifi, WIFI_SLOT_NONE, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    // Must scan first instead of entering fast-path on slot 0
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_SCANNING, conn.getState());
    TEST_ASSERT_EQUAL_INT(1, wifi.countCalls("scanStart"));
    TEST_ASSERT_EQUAL_INT(0, wifi.countCalls("begin"));

    stepMs(clk, ap, 300, 50);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT8(0, conn.getConnectedSlot());
    uint8_t slotOut = WIFI_SLOT_NONE;
    TEST_ASSERT_TRUE(conn.consumeLastGoodSlotChanged(slotOut));
    TEST_ASSERT_EQUAL_UINT8(0, slotOut);
}

void test_A14_deferred_apply_preserves_pre_save_ap_state_during_delay(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeDefaultTestConfig();
    FakeSystemActions actions(&ap, &cfg);

    conn.init(cfg.wifi, WIFI_SLOT_NONE, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL, &actions);
    TEST_ASSERT_TRUE(ap.isApMode());
    TEST_ASSERT_EQUAL_UINT32(0, ap.getApClosesInSeconds());
    int startsBefore = wifi.countCalls("softApStart");

    DeviceConfig next = makeSingleWifiCfg("HomeNet", "secret123", DEFAULT_SETUP_WINDOW_S);
    snprintf(next.ap_ssid, sizeof(next.ap_ssid), "RenamedAP");
    snprintf(next.ap_pass, sizeof(next.ap_pass), "newpass123");
    ap.requestApplyConfig(next, true);

    // SetupApController keeps its pre-save snapshot until the deferred delay fires.
    stepMs(clk, ap, DEFERRED_APPLY_DELAY_MS - 50, 50);
    TEST_ASSERT_EQUAL_INT(0, actions.applyCount);
    TEST_ASSERT_EQUAL_INT(startsBefore, wifi.countCalls("softApStart"));

    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_EQUAL_INT(1, actions.applyCount);
    TEST_ASSERT_TRUE(actions.lastTriggerReconnect);
    TEST_ASSERT_TRUE(ap.getApClosesInSeconds() > 0);
    TEST_ASSERT_TRUE(wifi.countCalls("softApStart") > startsBefore);
    TEST_ASSERT_EQUAL_STRING("RenamedAP", wifi.softApSsid().c_str());
    TEST_ASSERT_EQUAL_STRING("newpass123", wifi.softApPass().c_str());
}

void test_W20_wake_from_backoff_starts_cycle_immediately(void) {
    FakeClock clk(1000);
    FakeWifiHal wifi(clk);
    wifi.setScanDurationMs(100);
    WifiConnector conn(wifi, clk);
    SetupApController ap(wifi, clk, conn);
    DeviceConfig cfg = makeSingleWifiCfg("HomeRouter", "pass1234", 0);

    conn.init(cfg.wifi, 0, cfg.device_name);
    ap.init(cfg, 0x112233445566ULL);

    // Fail first cycle -> enters 15s backoff
    TEST_ASSERT_TRUE(advanceUntilBackoff(clk, ap, conn, 15000));
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_BACKOFF, conn.getState());

    // Router comes back online only 1s into the 15s backoff window
    stepMs(clk, ap, 1000, 100);
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_BACKOFF, conn.getState());
    wifi.addAcceptCred("HomeRouter", "pass1234", 50, 6);

    // Waking from backoff starts a new cycle on the next update without waiting the remaining 14s
    int beginsBefore = wifi.countCalls("begin");
    conn.wakeFromBackoff();
    stepMs(clk, ap, 100, 50);
    TEST_ASSERT_TRUE(wifi.countCalls("begin") > beginsBefore);
    TEST_ASSERT_EQUAL_INT(WL_CONNECTED, wifi.status());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_CONNECTED, conn.getState());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_W1_first_boot_no_networks_permanent_ap_no_sta);
    RUN_TEST(test_W2_fast_path_last_good_succeeds_without_scan);
    RUN_TEST(test_W3_fast_path_missing_scans_picks_strongest_and_tie_breaks_lower_slot);
    RUN_TEST(test_W4_hidden_ssid_tried_after_visible_candidates);
    RUN_TEST(test_W5_all_fail_backoff_15_30_60_60_cycle_repeats);
    RUN_TEST(test_W6_drop_restarts_with_lost_slot_first_and_no_double_try_on_fail);
    RUN_TEST(test_W7_ap_station_pauses_connector_stops_scan_and_resumes_after_leave);
    RUN_TEST(test_W8_web_scan_abandoned_recovers_within_15s);
    RUN_TEST(test_W9_disconnected_timer_accurate_no_uptime_value);
    RUN_TEST(test_W10_5min_offline_fallback_ap_once_rearms_5min_after_close);
    RUN_TEST(test_W11_adding_slot3_while_connected_slot0_no_disconnect);
    RUN_TEST(test_W12_changing_connected_slot_password_reconnects_after_reply);
    RUN_TEST(test_W13_connector_scan_stuck_15s_guard);
    RUN_TEST(test_W14_connect_timeout_across_millis_wrap);
    RUN_TEST(test_W15_force_reconnect_while_ap_client_runs_on_leave);
    RUN_TEST(test_W16_external_connect_without_trying_slot_matches_or_unknown);
    RUN_TEST(test_A1_boot_window_60s_nobody_joins_closes_sta_untouched);
    RUN_TEST(test_A2_client_joins_stays_10min_idle_120s_and_30min_hard_cap);
    RUN_TEST(test_A3_first_save_via_permanent_ap_converts_to_timed_window_and_closes);
    RUN_TEST(test_A4_window_zero_no_boot_ap_when_networks_exist);
    RUN_TEST(test_A5_ap_opened_while_sta_on_channel_6_uses_channel_6);
    RUN_TEST(test_A6_closes_in_s_hard_cap_and_distinct_ips_in_status);
    RUN_TEST(test_A7_ondemand_open_300s_and_deferred_close_via_api);
    RUN_TEST(test_A8_boot_window_closes_across_millis_wrap);
    RUN_TEST(test_A9_deferred_reboot_and_factory_reset_reach_hal_once);
    RUN_TEST(test_A10_web_scan_json_sorted_by_rssi_and_escapes_ssid);
    RUN_TEST(test_A11_softap_start_failure_restores_mode_and_retries);
    RUN_TEST(test_A12_web_scan_json_overflow_drops_weakest_networks);
    RUN_TEST(test_A13_ap_cred_change_restarts_and_refresh_keeps_hard_cap);
    RUN_TEST(test_W17_first_network_saved_with_ap_client_attempts_immediately);
    RUN_TEST(test_W18_background_retry_with_ap_client_stays_paused);
    RUN_TEST(test_W19_no_last_good_slot_skips_fast_path_and_persists_connected_slot);
    RUN_TEST(test_A14_deferred_apply_preserves_pre_save_ap_state_during_delay);
    RUN_TEST(test_W20_wake_from_backoff_starts_cycle_immediately);
    return UNITY_END();
}
