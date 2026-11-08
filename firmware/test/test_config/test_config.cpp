#include <unity.h>

#include "config_codec.h"
#include "fakes.h"
#include "protocol_helpers.h"

namespace {

bool tryValidate(const DeviceConfig& cur, const char* jsonStr, DeviceConfig& out,
                 char* errField, size_t errFieldLen,
                 char* errReason, size_t errReasonLen,
                 bool& wifiChanged) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err || !doc.is<JsonObject>()) {
        snprintf(errField, errFieldLen, "body");
        snprintf(errReason, errReasonLen, "invalid json");
        return false;
    }
    char applied[16][24];
    uint8_t appliedCount = 0;
    return validateAndMergeConfig(cur, doc.as<JsonObjectConst>(), out,
                                  errField, errFieldLen, errReason, errReasonLen,
                                  applied, appliedCount, wifiChanged);
}

void assertReject(const DeviceConfig& cur, const char* jsonStr, const char* expectedField = nullptr) {
    DeviceConfig out;
    char ef[64] = {0};
    char er[128] = {0};
    bool wchg = false;
    bool ok = tryValidate(cur, jsonStr, out, ef, sizeof(ef), er, sizeof(er), wchg);
    TEST_ASSERT_FALSE(ok);
    if (expectedField) {
        TEST_ASSERT_EQUAL_STRING(expectedField, ef);
    }
}

void assertAccept(const DeviceConfig& cur, const char* jsonStr, DeviceConfig& out) {
    char ef[64] = {0};
    char er[128] = {0};
    bool wchg = false;
    bool ok = tryValidate(cur, jsonStr, out, ef, sizeof(ef), er, sizeof(er), wchg);
    if (!ok) {
        fprintf(stderr, "  unexpected reject on %s -> %s: %s\n", jsonStr, ef, er);
    }
    TEST_ASSERT_TRUE(ok);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_C1_every_d1_field_bounds_types_and_missing(void) {
    DeviceConfig cur = makeDefaultTestConfig();
    DeviceConfig out;

    // Empty object (all fields missing -> valid no-op)
    assertAccept(cur, "{}", out);

    // setup_window_s: 0..600
    assertAccept(cur, "{\"setup_window_s\":0}", out);
    TEST_ASSERT_EQUAL_UINT16(0, out.setup_window_s);
    assertAccept(cur, "{\"setup_window_s\":600}", out);
    TEST_ASSERT_EQUAL_UINT16(600, out.setup_window_s);
    assertReject(cur, "{\"setup_window_s\":-1}", "setup_window_s");
    assertReject(cur, "{\"setup_window_s\":601}", "setup_window_s");
    assertReject(cur, "{\"setup_window_s\":60.5}", "setup_window_s");
    assertReject(cur, "{\"setup_window_s\":\"60\"}", "setup_window_s");

    // debounce_ms: 50..10000
    assertAccept(cur, "{\"debounce_ms\":50}", out);
    assertAccept(cur, "{\"debounce_ms\":10000}", out);
    assertReject(cur, "{\"debounce_ms\":49}", "debounce_ms");
    assertReject(cur, "{\"debounce_ms\":10001}", "debounce_ms");
    assertReject(cur, "{\"debounce_ms\":-100}", "debounce_ms");
    assertReject(cur, "{\"debounce_ms\":500.1}", "debounce_ms");
    assertReject(cur, "{\"debounce_ms\":\"500\"}", "debounce_ms");

    // restore_cooldown_ms: 0..60000
    assertAccept(cur, "{\"restore_cooldown_ms\":0}", out);
    assertAccept(cur, "{\"restore_cooldown_ms\":60000}", out);
    assertReject(cur, "{\"restore_cooldown_ms\":-1}", "restore_cooldown_ms");
    assertReject(cur, "{\"restore_cooldown_ms\":60001}", "restore_cooldown_ms");
    assertReject(cur, "{\"restore_cooldown_ms\":10.2}", "restore_cooldown_ms");
    assertReject(cur, "{\"restore_cooldown_ms\":\"0\"}", "restore_cooldown_ms");

    // snooze_default_min: 1..1440
    assertAccept(cur, "{\"snooze_default_min\":1}", out);
    assertAccept(cur, "{\"snooze_default_min\":1440}", out);
    assertReject(cur, "{\"snooze_default_min\":0}", "snooze_default_min");
    assertReject(cur, "{\"snooze_default_min\":1441}", "snooze_default_min");
    assertReject(cur, "{\"snooze_default_min\":15.5}", "snooze_default_min");
    assertReject(cur, "{\"snooze_default_min\":\"30\"}", "snooze_default_min");

    // alert_mode: 0 or 1
    assertAccept(cur, "{\"alert_mode\":0}", out);
    assertAccept(cur, "{\"alert_mode\":1}", out);
    assertReject(cur, "{\"alert_mode\":-1}", "alert_mode");
    assertReject(cur, "{\"alert_mode\":2}", "alert_mode");
    assertReject(cur, "{\"alert_mode\":1.5}", "alert_mode");
    assertReject(cur, "{\"alert_mode\":\"1\"}", "alert_mode");

    // cloud_enabled: boolean only (default false)
    TEST_ASSERT_FALSE(cur.cloud_enabled);
    assertAccept(cur, "{\"cloud_enabled\":true}", out);
    TEST_ASSERT_TRUE(out.cloud_enabled);
    assertAccept(cur, "{\"cloud_enabled\":false}", out);
    TEST_ASSERT_FALSE(out.cloud_enabled);
    assertReject(cur, "{\"cloud_enabled\":1}", "cloud_enabled");
    assertReject(cur, "{\"cloud_enabled\":\"true\"}", "cloud_enabled");
    assertReject(cur, "{\"cloud_enabled\":null}", "cloud_enabled");

    // device_name: 1-32 [a-z0-9-], no leading/trailing '-'
    assertAccept(cur, "{\"device_name\":\"a\"}", out);
    assertAccept(cur, "{\"device_name\":\"power-mon-01\"}", out);
    assertAccept(cur, "{\"device_name\":\"01234567890123456789012345678901\"}", out);
    assertReject(cur, "{\"device_name\":\"\"}", "device_name");
    assertReject(cur, "{\"device_name\":\"-abc\"}", "device_name");
    assertReject(cur, "{\"device_name\":\"abc-\"}", "device_name");
    assertReject(cur, "{\"device_name\":\"PowerMon\"}", "device_name");
    assertReject(cur, "{\"device_name\":\"012345678901234567890123456789012\"}", "device_name");
    assertReject(cur, "{\"device_name\":123}", "device_name");

    // ap_ssid: 1-32 bytes string
    assertAccept(cur, "{\"ap_ssid\":\"M\"}", out);
    assertReject(cur, "{\"ap_ssid\":\"\"}", "ap_ssid");
    assertReject(cur, "{\"ap_ssid\":\"012345678901234567890123456789012\"}", "ap_ssid");
    assertReject(cur, "{\"ap_ssid\":99}", "ap_ssid");

    // ap_pass: empty keeps, 8..63 sets, 1..7 or >=64 rejects
    snprintf(cur.ap_pass, sizeof(cur.ap_pass), "existingpass");
    assertAccept(cur, "{\"ap_pass\":\"\"}", out);
    TEST_ASSERT_EQUAL_STRING("existingpass", out.ap_pass);
    assertAccept(cur, "{\"ap_pass\":\"12345678\"}", out);
    TEST_ASSERT_EQUAL_STRING("12345678", out.ap_pass);
    assertReject(cur, "{\"ap_pass\":\"1234567\"}", "ap_pass");
    assertReject(cur, "{\"ap_pass\":\"0123456789012345678901234567890123456789012345678901234567890123\"}", "ap_pass");
    assertReject(cur, "{\"ap_pass\":12345678}", "ap_pass");

    // webhook_url & local_url: empty or http:// / https:// <= 256
    assertAccept(cur, "{\"webhook_url\":\"\"}", out);
    assertAccept(cur, "{\"webhook_url\":\"https://ntfy.sh/test\"}", out);
    assertReject(cur, "{\"webhook_url\":\"ftp://example.com\"}", "webhook_url");
    assertReject(cur, "{\"webhook_url\":123}", "webhook_url");

    assertAccept(cur, "{\"local_url\":\"\"}", out);
    assertAccept(cur, "{\"local_url\":\"http://192.168.1.60:8080/{event}\"}", out);
    assertAccept(cur, "{\"local_url\":\"https://phone.lan/{event}\"}", out);
    assertReject(cur, "{\"local_url\":\"ftp://192.168.1.60/{event}\"}", "local_url");
    assertReject(cur, "{\"local_url\":123}", "local_url");
    {
        char overUrlJson[320];
        char longPath[260];
        memset(longPath, 'a', 250);
        longPath[250] = '\0';
        snprintf(overUrlJson, sizeof(overUrlJson), "{\"local_url\":\"http://h.local/%s\"}", longPath);
        assertReject(cur, overUrlJson, "local_url");
    }

    // wifi[].slot & wifi[].ssid length
    assertReject(cur, "{\"wifi\":{}}", "wifi");
    assertReject(cur, "{\"wifi\":[123]}", "wifi");
    assertReject(cur, "{\"wifi\":[{\"slot\":5,\"ssid\":\"x\",\"pass\":\"12345678\"}]}", "wifi.slot");
    assertReject(cur, "{\"wifi\":[{\"slot\":-1,\"ssid\":\"x\",\"pass\":\"12345678\"}]}", "wifi.slot");
    assertReject(cur, "{\"wifi\":[{\"slot\":0.5,\"ssid\":\"x\",\"pass\":\"12345678\"}]}", "wifi.slot");
    assertReject(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"012345678901234567890123456789012\",\"pass\":\"12345678\"}]}", "wifi[0].ssid");
    assertReject(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"Home\",\"pass\":\"short\"}]}", "wifi[0].pass");
}

void test_C2_rejected_multifield_body_leaves_candidate_unchanged(void) {
    DeviceConfig cur = makeDefaultTestConfig();
    cur.debounce_ms = 500;
    cur.snooze_default_min = 30;

    DeviceConfig candidate = cur;
    char ef[64] = {0};
    char er[128] = {0};
    bool wchg = false;
    bool ok = tryValidate(
        cur,
        "{\"debounce_ms\":1200,\"snooze_default_min\":45,\"device_name\":\"BAD_NAME!\"}",
        candidate, ef, sizeof(ef), er, sizeof(er), wchg);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("device_name", ef);

    // Output candidate must remain unchanged across the rejected multi-field body
    TEST_ASSERT_EQUAL_UINT32(500, candidate.debounce_ms);
    TEST_ASSERT_EQUAL_UINT16(30, candidate.snooze_default_min);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_DEVICE_NAME, candidate.device_name);
}

void test_C3_secret_semantics_wifi_and_admin_pass(void) {
    DeviceConfig cur = makeDefaultTestConfig();
    snprintf(cur.wifi[0].ssid, sizeof(cur.wifi[0].ssid), "HomeNet");
    snprintf(cur.wifi[0].pass, sizeof(cur.wifi[0].pass), "secret123");
    snprintf(cur.admin_pass, sizeof(cur.admin_pass), "admsecret");

    DeviceConfig out;

    // 1. Empty pass + empty admin_pass keeps existing secrets
    assertAccept(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"HomeNet\",\"pass\":\"\"}],\"admin_pass\":\"\"}", out);
    TEST_ASSERT_EQUAL_STRING("secret123", out.wifi[0].pass);
    TEST_ASSERT_EQUAL_STRING("admsecret", out.admin_pass);

    // 2. null or clear_pass=true clears wifi pass; null or clear_admin_pass=true clears admin_pass
    assertAccept(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"HomeNet\",\"pass\":null}],\"admin_pass\":null}", out);
    TEST_ASSERT_EQUAL_STRING("", out.wifi[0].pass);
    TEST_ASSERT_EQUAL_STRING("", out.admin_pass);

    assertAccept(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"HomeNet\",\"clear_pass\":true}],\"clear_admin_pass\":true}", out);
    TEST_ASSERT_EQUAL_STRING("", out.wifi[0].pass);
    TEST_ASSERT_EQUAL_STRING("", out.admin_pass);

    // 3. Empty ssid removes network slot and its password
    assertAccept(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"\"}]}", out);
    TEST_ASSERT_EQUAL_STRING("", out.wifi[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", out.wifi[0].pass);

    // 4. Changing SSID to non-empty without pass or clear_pass/null -> reject
    assertReject(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"OtherNet\"}]}", "wifi[0].pass");
    assertReject(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"OtherNet\",\"pass\":\"\"}]}", "wifi[0].pass");

    // 5. Changing SSID with clear_pass=true (open network) -> accepted
    assertAccept(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"OpenCafe\",\"clear_pass\":true}]}", out);
    TEST_ASSERT_EQUAL_STRING("OpenCafe", out.wifi[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", out.wifi[0].pass);
}

void test_C4_only_changed_nvs_keys_written(void) {
    FakeKvStore kv;
    DeviceConfig cfg = makeDefaultTestConfig();
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg));
    kv.clearWriteLog();

    // Unchanged save writes 0 keys
    char keys[16][16];
    uint8_t nUnchanged = 0;
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg, &nUnchanged, keys, 16));
    TEST_ASSERT_EQUAL_UINT8(0, nUnchanged);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned int)kv.writes().size());

    // Change only debounce_ms and snooze_default_min -> only "debounce" and "snooze" written
    cfg.debounce_ms = 1200;
    cfg.snooze_default_min = 45;
    uint8_t nChanged = 0;
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg, &nChanged, keys, 16));
    TEST_ASSERT_EQUAL_UINT8(2, nChanged);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned int)kv.writes().size());
    TEST_ASSERT_TRUE(kv.hasWriteForKey("debounce"));
    TEST_ASSERT_TRUE(kv.hasWriteForKey("snooze"));
    TEST_ASSERT_FALSE(kv.hasWriteForKey("cooldown"));
    TEST_ASSERT_FALSE(kv.hasWriteForKey("dev_name"));
}

void test_C5_invalid_and_oversized_dev_name_on_load(void) {
    // 1. Invalid dev_name stored in NVS -> sanitized fallback to DEFAULT_DEVICE_NAME
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_TRUE(kv.putUChar("cfg_ver", CFG_VERSION) > 0);
        TEST_ASSERT_TRUE(kv.putString("dev_name", "INVALID_NAME!") > 0);
        kv.end();

        DeviceConfig cfg;
        loadConfigFromKv(kv, cfg, 0x112233445566ULL);
        TEST_ASSERT_EQUAL_STRING(DEFAULT_DEVICE_NAME, cfg.device_name);
    }

    // 2. Oversized (>32 bytes) dev_name stored in NVS -> fallback to DEFAULT_DEVICE_NAME (no silent truncation)
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_TRUE(kv.putUChar("cfg_ver", CFG_VERSION) > 0);
        TEST_ASSERT_TRUE(kv.putString("dev_name", "0123456789012345678901234567890123456789") > 0);
        kv.end();

        DeviceConfig cfg;
        loadConfigFromKv(kv, cfg, 0x112233445566ULL);
        TEST_ASSERT_EQUAL_STRING(DEFAULT_DEVICE_NAME, cfg.device_name);
    }
}

void test_C6_unknown_key_and_admin_pass_conflict_and_bad_clear_pass_type(void) {
    DeviceConfig cur = makeDefaultTestConfig();

    assertReject(cur, "{\"foo_bar\":1}", "foo_bar");
    assertReject(cur, "{\"admin_pass\":\"12345678\",\"clear_admin_pass\":true}", "admin_pass");
    assertReject(cur, "{\"clear_admin_pass\":\"yes\"}", "clear_admin_pass");
    assertReject(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"Home\",\"extra\":1}]}", "wifi.extra");
    assertReject(cur, "{\"wifi\":[{\"slot\":0,\"ssid\":\"Home\",\"clear_pass\":\"yes\"}]}", "wifi[0].clear_pass");
}

void test_C7_get_config_and_status_redact_passwords_and_return_webhook(void) {
    DeviceConfig cfg = makeDefaultTestConfig();
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "Home");
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "WifiSuperSecret");
    snprintf(cfg.ap_pass, sizeof(cfg.ap_pass), "ApSuperSecret");
    snprintf(cfg.admin_pass, sizeof(cfg.admin_pass), "AdmSuperSecret");
    snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "https://user:token123@ntfy.sh/mysecret");
    snprintf(cfg.local_url, sizeof(cfg.local_url), "http://192.168.1.60:8080/{event}");

    char cfgJson[1536];
    buildConfigJsonBuf(cfg, cfgJson, sizeof(cfgJson));
    TEST_ASSERT_NULL(strstr(cfgJson, "WifiSuperSecret"));
    TEST_ASSERT_NULL(strstr(cfgJson, "ApSuperSecret"));
    TEST_ASSERT_NULL(strstr(cfgJson, "AdmSuperSecret"));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"has_pass\":true"));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"has_ap_pass\":true"));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"has_admin_pass\":true"));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"cloud_enabled\":false"));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"webhook_url\":\"https://user:token123@ntfy.sh/mysecret\""));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"local_url\":\"http://192.168.1.60:8080/{event}\""));

    StatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.deviceName = cfg.device_name;
    snap.gridState = GRID_ONLINE;
    snap.mode = MODE_ARMED;
    snap.wifiConnected = true;
    snap.wifiSsid = "Home";
    snap.wifiSlot = 0;
    snap.wifiIp = "192.168.1.50";
    snap.wifiRssi = -55;
    snap.apActive = false;
    snap.apSsid = "PowerMonitor-5566";
    snap.apIp = "";
    snap.adminProtected = true;
    snap.snoozeDefaultMin = 30;
    snap.freeHeap = 180000;
    snap.ts = 1700000000;

    char stJson[1024];
    buildStatusJsonBuf(snap, stJson, sizeof(stJson));
    TEST_ASSERT_NULL(strstr(stJson, "WifiSuperSecret"));
    TEST_ASSERT_NULL(strstr(stJson, "ApSuperSecret"));
    TEST_ASSERT_NULL(strstr(stJson, "AdmSuperSecret"));
    TEST_ASSERT_NULL(strstr(stJson, "webhook"));
    TEST_ASSERT_NULL(strstr(stJson, "local_url"));
}

void test_C8_fake_kv_enforces_nvs_limits_and_saved_keys_within_15_chars(void) {
    // 1. FakeKvStore enforces open state, read-only mode, and 15-char key limit
    {
        FakeKvStore kv;
        char buf[32];

        // Unopened store fails writes and returns defaults on reads
        TEST_ASSERT_FALSE(kv.begin(NVS_NAMESPACE, true));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)kv.putString("k", "v"));
        TEST_ASSERT_FALSE(kv.isKey("k"));
        kv.getString("k", buf, sizeof(buf), "def");
        TEST_ASSERT_EQUAL_STRING("def", buf);

        // Key > 15 chars rejected on RW handle
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)kv.putString("0123456789abcdef", "val"));
        TEST_ASSERT_FALSE(kv.isKey("0123456789abcdef"));
        TEST_ASSERT_TRUE(kv.putString("0123456789abcde", "ok") > 0);
        kv.end();

        // Read-only handle rejects writes, permits reads
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, true));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)kv.putString("k2", "v2"));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)kv.putUShort("u", 1));
        TEST_ASSERT_FALSE(kv.clear());
        kv.getString("0123456789abcde", buf, sizeof(buf), "");
        TEST_ASSERT_EQUAL_STRING("ok", buf);
        kv.end();
    }

    // 2. Every key written by saveConfigDiffToKv is 1..15 characters
    {
        FakeKvStore kv;
        DeviceConfig cfg = makeDefaultTestConfig();
        for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
            snprintf(cfg.wifi[i].ssid, sizeof(cfg.wifi[i].ssid), "Net%u", i);
            snprintf(cfg.wifi[i].pass, sizeof(cfg.wifi[i].pass), "pass%u1234", i);
        }
        snprintf(cfg.ap_ssid, sizeof(cfg.ap_ssid), "CustomAP");
        snprintf(cfg.admin_pass, sizeof(cfg.admin_pass), "admin1234");
        snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "https://ntfy.sh/topic");
        snprintf(cfg.local_url, sizeof(cfg.local_url), "http://192.168.1.60:8080/{event}");

        uint8_t written = 0;
        TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg, &written));
        TEST_ASSERT_TRUE(written > 0);
        TEST_ASSERT_EQUAL_UINT(written, (unsigned int)kv.writes().size());
        for (size_t i = 0; i < kv.writes().size(); ++i) {
            size_t len = kv.writes()[i].key.length();
            TEST_ASSERT_TRUE(len >= 1 && len <= FakeKvStore::MAX_NVS_KEY_LEN);
        }
    }
}

void test_S3_snooze_clamp_1_1440_http(void) {
    uint16_t outMin = 0;
    char ef[32] = {0};
    char er[64] = {0};

    // Query ?min=
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(true, "5000", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(true, "1", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1, outMin);
    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(true, "0", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));

    // Query ?duration= (seconds -> ceil/min 1, clamped 1440)
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", true, "30", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", true, "200000", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", true, "0", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));

    // JSON {"duration_min": ...} and {"duration": ...}
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":2000}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration\":200000}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":0}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":10.5}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));

    // Default snooze_default_min=0 fallback safety via HTTP snooze parser
    {
        uint16_t outMinFallback = 0;
        char ef2[32] = {0};
        char er2[64] = {0};
        TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "", 0,
                                                outMinFallback, ef2, sizeof(ef2), er2, sizeof(er2)));
        TEST_ASSERT_EQUAL_UINT16(DEFAULT_SNOOZE_MIN, outMinFallback);
    }
}

void test_C9_out_of_range_nvs_and_unknown_cfg_ver_on_load(void) {
    // 1. Out-of-range numeric fields and invalid alert_mode fall back to defaults
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_TRUE(kv.putUChar("cfg_ver", CFG_VERSION) > 0);
        TEST_ASSERT_TRUE(kv.putUShort("ap_win", 900) > 0);
        TEST_ASSERT_TRUE(kv.putULong("debounce", 20000) > 0);
        TEST_ASSERT_TRUE(kv.putULong("cooldown", 90000) > 0);
        TEST_ASSERT_TRUE(kv.putUShort("snooze", 5000) > 0);
        TEST_ASSERT_TRUE(kv.putUChar("alert_mode", 7) > 0);
        TEST_ASSERT_TRUE(kv.putString("ap_pass", "123") > 0);
        TEST_ASSERT_TRUE(kv.putString("webhook", "ftp://bad.example") > 0);
        TEST_ASSERT_TRUE(kv.putString("local_url", "ftp://bad.local") > 0);
        kv.end();

        DeviceConfig cfg;
        loadConfigFromKv(kv, cfg, 0x112233445566ULL);
        TEST_ASSERT_EQUAL_UINT16(DEFAULT_SETUP_WINDOW_S, cfg.setup_window_s);
        TEST_ASSERT_EQUAL_UINT32(DEFAULT_DEBOUNCE_MS, cfg.debounce_ms);
        TEST_ASSERT_EQUAL_UINT32(DEFAULT_COOLDOWN_MS, cfg.restore_cooldown_ms);
        TEST_ASSERT_EQUAL_UINT16(DEFAULT_SNOOZE_MIN, cfg.snooze_default_min);
        TEST_ASSERT_EQUAL_INT((int)ALERT_OUTAGE_ONLY, (int)cfg.alert_mode);
        TEST_ASSERT_EQUAL_STRING(DEFAULT_AP_PASS, cfg.ap_pass);
        TEST_ASSERT_EQUAL_STRING("", cfg.webhook_url);
        TEST_ASSERT_EQUAL_STRING("", cfg.local_url);
    }

    // 2. Unknown future cfg_ver (e.g. 3) leaves NVS untouched and loads defaults
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_TRUE(kv.putUChar("cfg_ver", 3) > 0);
        TEST_ASSERT_TRUE(kv.putULong("debounce", 1234) > 0);
        kv.end();
        kv.clearWriteLog();

        DeviceConfig cfg;
        loadConfigFromKv(kv, cfg, 0x112233445566ULL);
        TEST_ASSERT_EQUAL_UINT32(DEFAULT_DEBOUNCE_MS, cfg.debounce_ms);
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)kv.writes().size());
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, true));
        TEST_ASSERT_EQUAL_UINT8(3, kv.getUChar("cfg_ver", 0));
        TEST_ASSERT_EQUAL_UINT32(1234, kv.getULong("debounce", 0));
        kv.end();
    }
}

void test_C10_kv_failures_written_cap_last_good_and_json_overflow(void) {
    // 1. begin() failure -> saveConfigDiffToKv returns false and writes 0
    {
        FakeKvStore kv;
        DeviceConfig cfg = makeDefaultTestConfig();
        kv.setFailBegin(true);
        uint8_t written = 99;
        TEST_ASSERT_FALSE(saveConfigDiffToKv(kv, cfg, &written));
        TEST_ASSERT_EQUAL_UINT8(0, written);
        TEST_ASSERT_FALSE(saveLastGoodWifiSlot(kv, 2));
        TEST_ASSERT_FALSE(wipeConfigKv(kv));
    }

    // 2. Mid-save put failure -> returns false and reports actual puts succeeded
    {
        FakeKvStore kv;
        DeviceConfig cfg = makeDefaultTestConfig();
        kv.setFailPutAfter(3);
        uint8_t written = 0;
        TEST_ASSERT_FALSE(saveConfigDiffToKv(kv, cfg, &written));
        TEST_ASSERT_EQUAL_UINT8(3, written);
        TEST_ASSERT_EQUAL_UINT(3, (unsigned int)kv.writes().size());
    }

    // 3. maxKeys cap on writtenKeys does not overflow caller buffer or counter
    {
        FakeKvStore kv;
        DeviceConfig cfg = makeDefaultTestConfig();
        char keys[8][16];
        memset(keys, 0x5A, sizeof(keys));
        uint8_t written = 0;
        TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg, &written, keys, 8));
        TEST_ASSERT_EQUAL_UINT8(8, written);
        TEST_ASSERT_TRUE(kv.writes().size() > 8);
        DeviceConfig loaded;
        loadConfigFromKv(kv, loaded, 0x112233445566ULL);
        TEST_ASSERT_EQUAL_UINT32(cfg.debounce_ms, loaded.debounce_ms);
    }

    // 4. w_last round-trip & wipeConfigKv
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(saveLastGoodWifiSlot(kv, 3));
        TEST_ASSERT_EQUAL_UINT8(3, loadLastGoodWifiSlot(kv));
        TEST_ASSERT_FALSE(saveLastGoodWifiSlot(kv, WIFI_SLOTS));
        TEST_ASSERT_TRUE(wipeConfigKv(kv));
        TEST_ASSERT_EQUAL_UINT8(WIFI_SLOT_NONE, loadLastGoodWifiSlot(kv));
    }

    // 5. buildConfigJsonBuf overflow returns 0 and empties buffer
    {
        DeviceConfig cfg = makeDefaultTestConfig();
        char tiny[64];
        memset(tiny, 'X', sizeof(tiny));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)buildConfigJsonBuf(cfg, tiny, sizeof(tiny)));
        TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
    }
}

void test_C11_local_url_nvs_roundtrip_and_diff_write(void) {
    FakeKvStore kv;
    DeviceConfig cfg;
    loadConfigFromKv(kv, cfg, 0x112233445566ULL);
    snprintf(cfg.local_url, sizeof(cfg.local_url), "http://192.168.1.60:8080/{event}");

    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg));
    DeviceConfig loaded;
    loadConfigFromKv(kv, loaded, 0x112233445566ULL);
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/{event}", loaded.local_url);

    // Unchanged save writes 0 keys
    uint8_t written = 0;
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, loaded, &written));
    TEST_ASSERT_EQUAL_UINT8(0, written);

    // Changing only local_url writes 1 key: "local_url"
    char keys[4][16];
    memset(keys, 0, sizeof(keys));
    snprintf(loaded.local_url, sizeof(loaded.local_url), "https://phone.lan/hook/{event}");
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, loaded, &written, keys, 4));
    TEST_ASSERT_EQUAL_UINT8(1, written);
    TEST_ASSERT_EQUAL_STRING("local_url", keys[0]);

    // Clearing local_url writes 1 key and persists as empty
    loaded.local_url[0] = '\0';
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, loaded, &written, keys, 4));
    TEST_ASSERT_EQUAL_UINT8(1, written);
    TEST_ASSERT_EQUAL_STRING("local_url", keys[0]);
    DeviceConfig cleared;
    loadConfigFromKv(kv, cleared, 0x112233445566ULL);
    TEST_ASSERT_EQUAL_STRING("", cleared.local_url);
}

void test_C12_last_good_wifi_slot_none_and_bounds(void) {
    // 1. Missing w_last key returns WIFI_SLOT_NONE
    {
        FakeKvStore kv;
        TEST_ASSERT_EQUAL_UINT8(WIFI_SLOT_NONE, loadLastGoodWifiSlot(kv));
    }

    // 2. Unreadable NVS namespace returns WIFI_SLOT_NONE
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(saveLastGoodWifiSlot(kv, 2));
        kv.setFailBegin(true);
        TEST_ASSERT_EQUAL_UINT8(WIFI_SLOT_NONE, loadLastGoodWifiSlot(kv));
    }

    // 3. Stored valid slots round-trip cleanly
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(saveLastGoodWifiSlot(kv, 0));
        TEST_ASSERT_EQUAL_UINT8(0, loadLastGoodWifiSlot(kv));
        TEST_ASSERT_TRUE(saveLastGoodWifiSlot(kv, WIFI_SLOTS - 1));
        TEST_ASSERT_EQUAL_UINT8(WIFI_SLOTS - 1, loadLastGoodWifiSlot(kv));
    }

    // 4. Out-of-range stored values return WIFI_SLOT_NONE, and saving WIFI_SLOT_NONE is rejected
    {
        FakeKvStore kv;
        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)kv.putUChar("w_last", WIFI_SLOTS));
        kv.end();
        TEST_ASSERT_EQUAL_UINT8(WIFI_SLOT_NONE, loadLastGoodWifiSlot(kv));

        TEST_ASSERT_TRUE(kv.begin(NVS_NAMESPACE, false));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)kv.putUChar("w_last", 200));
        kv.end();
        TEST_ASSERT_EQUAL_UINT8(WIFI_SLOT_NONE, loadLastGoodWifiSlot(kv));

        TEST_ASSERT_FALSE(saveLastGoodWifiSlot(kv, WIFI_SLOT_NONE));
    }
}

void test_C13_cloud_enabled_default_nvs_roundtrip_and_diff_write(void) {
    FakeKvStore kv;
    DeviceConfig cfg;
    loadConfigFromKv(kv, cfg, 0x112233445566ULL);
    TEST_ASSERT_FALSE(cfg.cloud_enabled);

    snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "https://ntfy.sh/my-secret-topic");
    cfg.cloud_enabled = true;
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, cfg));

    DeviceConfig loaded;
    loadConfigFromKv(kv, loaded, 0x112233445566ULL);
    TEST_ASSERT_TRUE(loaded.cloud_enabled);
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/my-secret-topic", loaded.webhook_url);

    // Unchanged save writes 0 keys
    uint8_t written = 0;
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, loaded, &written));
    TEST_ASSERT_EQUAL_UINT8(0, written);

    // Turning cloud_enabled off writes only "cloud_en" and preserves webhook_url
    char keys[4][16];
    memset(keys, 0, sizeof(keys));
    loaded.cloud_enabled = false;
    TEST_ASSERT_TRUE(saveConfigDiffToKv(kv, loaded, &written, keys, 4));
    TEST_ASSERT_EQUAL_UINT8(1, written);
    TEST_ASSERT_EQUAL_STRING("cloud_en", keys[0]);

    DeviceConfig afterOff;
    loadConfigFromKv(kv, afterOff, 0x112233445566ULL);
    TEST_ASSERT_FALSE(afterOff.cloud_enabled);
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/my-secret-topic", afterOff.webhook_url);

    char cfgJson[1536];
    buildConfigJsonBuf(afterOff, cfgJson, sizeof(cfgJson));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"cloud_enabled\":false"));
    TEST_ASSERT_NOT_NULL(strstr(cfgJson, "\"webhook_url\":\"https://ntfy.sh/my-secret-topic\""));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_C1_every_d1_field_bounds_types_and_missing);
    RUN_TEST(test_C2_rejected_multifield_body_leaves_candidate_unchanged);
    RUN_TEST(test_C3_secret_semantics_wifi_and_admin_pass);
    RUN_TEST(test_C4_only_changed_nvs_keys_written);
    RUN_TEST(test_C5_invalid_and_oversized_dev_name_on_load);
    RUN_TEST(test_C6_unknown_key_and_admin_pass_conflict_and_bad_clear_pass_type);
    RUN_TEST(test_C7_get_config_and_status_redact_passwords_and_return_webhook);
    RUN_TEST(test_C8_fake_kv_enforces_nvs_limits_and_saved_keys_within_15_chars);
    RUN_TEST(test_S3_snooze_clamp_1_1440_http);
    RUN_TEST(test_C9_out_of_range_nvs_and_unknown_cfg_ver_on_load);
    RUN_TEST(test_C10_kv_failures_written_cap_last_good_and_json_overflow);
    RUN_TEST(test_C11_local_url_nvs_roundtrip_and_diff_write);
    RUN_TEST(test_C12_last_good_wifi_slot_none_and_bounds);
    RUN_TEST(test_C13_cloud_enabled_default_nvs_roundtrip_and_diff_write);
    return UNITY_END();
}
