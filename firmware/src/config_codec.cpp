#include "config_codec.h"
#include <string.h>
#include <stdio.h>

static void setStrErr(char* errField, size_t errFieldLen,
                      char* errReason, size_t errReasonLen,
                      const char* field, const char* reason) {
    if (errField && errFieldLen > 0) {
        strncpy(errField, field ? field : "", errFieldLen - 1);
        errField[errFieldLen - 1] = '\0';
    }
    if (errReason && errReasonLen > 0) {
        strncpy(errReason, reason ? reason : "", errReasonLen - 1);
        errReason[errReasonLen - 1] = '\0';
    }
}

static bool hasJsonKey(JsonObjectConst obj, const char* key) {
    for (JsonPairConst kv : obj) {
        if (strcmp(kv.key().c_str(), key) == 0) return true;
    }
    return false;
}

bool isKnownConfigKey(const char* key) {
    static const char* const KNOWN_KEYS[] = {
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
        "local_url"
    };
    for (size_t i = 0; i < sizeof(KNOWN_KEYS) / sizeof(KNOWN_KEYS[0]); ++i) {
        if (strcmp(key, KNOWN_KEYS[i]) == 0) return true;
    }
    return false;
}

static bool parseIntRangeField(JsonObjectConst obj, const char* key,
                               int64_t minVal, int64_t maxVal,
                               int64_t& outVal,
                               char* errField, size_t errFieldLen,
                               char* errReason, size_t errReasonLen) {
    if (!obj[key].is<int64_t>()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "must be an integer %lld-%lld",
                 (long long)minVal, (long long)maxVal);
        setStrErr(errField, errFieldLen, errReason, errReasonLen, key, msg);
        return false;
    }
    int64_t v = obj[key].as<int64_t>();
    if (v < minVal || v > maxVal) {
        char msg[64];
        snprintf(msg, sizeof(msg), "must be in range %lld-%lld",
                 (long long)minVal, (long long)maxVal);
        setStrErr(errField, errFieldLen, errReason, errReasonLen, key, msg);
        return false;
    }
    outVal = v;
    return true;
}

static int64_t clampOrDefaultNvs(int64_t val, int64_t minVal, int64_t maxVal,
                                 int64_t defVal, const char* label) {
    (void)label;
    if (val < minVal || val > maxVal) {
        HAL_LOG_PRINTF("[NVS] Out-of-range %s (%lld); using default %lld.\n",
                       label, (long long)val, (long long)defVal);
        return defVal;
    }
    return val;
}

static bool isValidUrlOrEmpty(const char* url) {
    if (!url) return false;
    size_t ulen = strlen(url);
    if (ulen == 0) return true;
    if (ulen > MAX_WEBHOOK_URL_LEN) return false;
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static bool parseUrlField(JsonObjectConst obj, const char* key,
                          char* dest, size_t destSize,
                          bool& applied,
                          char* errField, size_t errFieldLen,
                          char* errReason, size_t errReasonLen) {
    applied = false;
    if (!hasJsonKey(obj, key)) return true;
    if (!obj[key].is<const char*>()) {
        setStrErr(errField, errFieldLen, errReason, errReasonLen, key, "must be a string");
        return false;
    }
    const char* u = obj[key].as<const char*>();
    size_t ulen = strlen(u);
    if (ulen > MAX_WEBHOOK_URL_LEN) {
        char msg[32];
        snprintf(msg, sizeof(msg), "must be <= %u bytes", (unsigned)MAX_WEBHOOK_URL_LEN);
        setStrErr(errField, errFieldLen, errReason, errReasonLen, key, msg);
        return false;
    }
    if (ulen > 0 && strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) {
        setStrErr(errField, errFieldLen, errReason, errReasonLen, key, "must be empty or start with http:// or https://");
        return false;
    }
    memset(dest, 0, destSize);
    strncpy(dest, u, destSize - 1);
    applied = true;
    return true;
}

static void sanitizeStoredUrl(char* buf, size_t bufSize, const char* nvsKey) {
    (void)nvsKey;
    if (!isValidUrlOrEmpty(buf)) {
        HAL_LOG_PRINTF("[NVS] Invalid stored %s -> clearing.\n", nvsKey);
        memset(buf, 0, bufSize);
    }
}

static void initDefaultDeviceConfig(DeviceConfig& cfg, uint64_t efuseMac) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.armed = DEFAULT_ARMED;
    strncpy(cfg.device_name, DEFAULT_DEVICE_NAME, sizeof(cfg.device_name) - 1);
    formatDefaultApSsid(efuseMac, cfg.ap_ssid, sizeof(cfg.ap_ssid));
    strncpy(cfg.ap_pass, DEFAULT_AP_PASS, sizeof(cfg.ap_pass) - 1);
    cfg.setup_window_s      = DEFAULT_SETUP_WINDOW_S;
    cfg.debounce_ms         = DEFAULT_DEBOUNCE_MS;
    cfg.restore_cooldown_ms = DEFAULT_COOLDOWN_MS;
    cfg.snooze_default_min  = DEFAULT_SNOOZE_MIN;
    cfg.alert_mode          = (AlertTrackingMode)DEFAULT_ALERT_MODE;
    cfg.cloud_enabled       = DEFAULT_CLOUD_ENABLED;
}

bool isValidDeviceName(const char* s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len < 1 || len > MAX_DEVICE_NAME_LEN) return false;
    if (s[0] == '-' || s[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-');
        if (!ok) return false;
    }
    return true;
}

bool validateAndMergeConfig(
    const DeviceConfig& cur,
    JsonObjectConst obj,
    DeviceConfig& out,
    char* errField,
    size_t errFieldLen,
    char* errReason,
    size_t errReasonLen,
    char applied[][24],
    uint8_t& appliedCount,
    bool& wifiChanged
) {
    appliedCount = 0;
    wifiChanged = false;
    out = cur;

    for (JsonPairConst kv : obj) {
        if (!isKnownConfigKey(kv.key().c_str())) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, kv.key().c_str(), "unknown field");
            return false;
        }
    }

    auto addApplied = [&](const char* name) {
        if (applied && appliedCount < 16) {
            strncpy(applied[appliedCount], name, 23);
            applied[appliedCount][23] = '\0';
            appliedCount++;
        }
    };

    // 1. Validate & stage wifi[0..4]
    if (hasJsonKey(obj, "wifi")) {
        if (!obj["wifi"].is<JsonArrayConst>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "wifi", "must be an array");
            return false;
        }
        JsonArrayConst arr = obj["wifi"].as<JsonArrayConst>();
        uint8_t idx = 0;
        for (JsonVariantConst v : arr) {
            if (!v.is<JsonObjectConst>()) {
                setStrErr(errField, errFieldLen, errReason, errReasonLen, "wifi", "each entry must be an object");
                return false;
            }
            JsonObjectConst entry = v.as<JsonObjectConst>();
            for (JsonPairConst ekv : entry) {
                const char* ek = ekv.key().c_str();
                if (strcmp(ek, "slot") != 0 && strcmp(ek, "ssid") != 0 &&
                    strcmp(ek, "pass") != 0 && strcmp(ek, "clear_pass") != 0) {
                    char errKey[48];
                    snprintf(errKey, sizeof(errKey), "wifi.%s", ek);
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, errKey, "unknown field");
                    return false;
                }
            }
            int slot = idx;
            if (hasJsonKey(entry, "slot")) {
                if (!entry["slot"].is<int64_t>()) {
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, "wifi.slot", "must be integer 0-4");
                    return false;
                }
                slot = (int)entry["slot"].as<int64_t>();
            }
            if (slot < 0 || slot >= WIFI_SLOTS) {
                setStrErr(errField, errFieldLen, errReason, errReasonLen, "wifi.slot", "out of range 0-4");
                return false;
            }

            char fieldName[32];
            bool ssidChangedToNonEmpty = false;
            if (hasJsonKey(entry, "ssid")) {
                if (!entry["ssid"].is<const char*>()) {
                    snprintf(fieldName, sizeof(fieldName), "wifi[%d].ssid", slot);
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, fieldName, "must be a string");
                    return false;
                }
                const char* s = entry["ssid"].as<const char*>();
                size_t slen = strlen(s);
                if (slen > MAX_WIFI_SSID_LEN) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "must be 0-%u bytes (empty clears slot)", (unsigned)MAX_WIFI_SSID_LEN);
                    snprintf(fieldName, sizeof(fieldName), "wifi[%d].ssid", slot);
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, fieldName, msg);
                    return false;
                }
                if (strcmp(out.wifi[slot].ssid, s) != 0) {
                    if (slen > 0) {
                        ssidChangedToNonEmpty = true;
                    }
                    memset(out.wifi[slot].ssid, 0, sizeof(out.wifi[slot].ssid));
                    strncpy(out.wifi[slot].ssid, s, sizeof(out.wifi[slot].ssid) - 1);
                    wifiChanged = true;
                }
                if (slen == 0 && out.wifi[slot].pass[0] != '\0') {
                    memset(out.wifi[slot].pass, 0, sizeof(out.wifi[slot].pass));
                    wifiChanged = true;
                }
            }

            bool clearPass = false;
            if (hasJsonKey(entry, "clear_pass")) {
                if (!entry["clear_pass"].is<bool>()) {
                    snprintf(fieldName, sizeof(fieldName), "wifi[%d].clear_pass", slot);
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, fieldName, "must be a boolean");
                    return false;
                }
                clearPass = entry["clear_pass"].as<bool>();
            }
            bool hasNonEmptyPass = false;
            if (hasJsonKey(entry, "pass")) {
                if (entry["pass"].isNull()) {
                    clearPass = true;
                } else if (entry["pass"].is<const char*>()) {
                    const char* p = entry["pass"].as<const char*>();
                    size_t plen = strlen(p);
                    if (plen > 0) {
                        if (plen < MIN_PASS_LEN || plen > MAX_PASS_LEN) {
                            char msg[64];
                            snprintf(msg, sizeof(msg), "must be %u-%u bytes (null or clear_pass for an open network)",
                                     (unsigned)MIN_PASS_LEN, (unsigned)MAX_PASS_LEN);
                            snprintf(fieldName, sizeof(fieldName), "wifi[%d].pass", slot);
                            setStrErr(errField, errFieldLen, errReason, errReasonLen, fieldName, msg);
                            return false;
                        }
                        hasNonEmptyPass = true;
                        if (strcmp(out.wifi[slot].pass, p) != 0) {
                            memset(out.wifi[slot].pass, 0, sizeof(out.wifi[slot].pass));
                            strncpy(out.wifi[slot].pass, p, sizeof(out.wifi[slot].pass) - 1);
                            wifiChanged = true;
                        }
                    }
                } else {
                    snprintf(fieldName, sizeof(fieldName), "wifi[%d].pass", slot);
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, fieldName, "must be string or null");
                    return false;
                }
            }

            if (ssidChangedToNonEmpty && !hasNonEmptyPass && !clearPass) {
                snprintf(fieldName, sizeof(fieldName), "wifi[%d].pass", slot);
                setStrErr(errField, errFieldLen, errReason, errReasonLen, fieldName, "required when ssid changes");
                return false;
            }

            if (clearPass && out.wifi[slot].pass[0] != '\0') {
                memset(out.wifi[slot].pass, 0, sizeof(out.wifi[slot].pass));
                wifiChanged = true;
            }
            if (out.wifi[slot].ssid[0] == '\0') {
                memset(out.wifi[slot].pass, 0, sizeof(out.wifi[slot].pass));
            }
            idx++;
        }
        addApplied("wifi");
    }

    // 2. device_name
    if (hasJsonKey(obj, "device_name")) {
        if (!obj["device_name"].is<const char*>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "device_name", "must be a string");
            return false;
        }
        const char* s = obj["device_name"].as<const char*>();
        if (!isValidDeviceName(s)) {
            char msg[80];
            snprintf(msg, sizeof(msg), "must be 1-%u chars [a-z0-9-], not starting/ending with '-'",
                     (unsigned)MAX_DEVICE_NAME_LEN);
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "device_name", msg);
            return false;
        }
        memset(out.device_name, 0, sizeof(out.device_name));
        strncpy(out.device_name, s, sizeof(out.device_name) - 1);
        addApplied("device_name");
    }

    // 3. ap_ssid
    if (hasJsonKey(obj, "ap_ssid")) {
        if (!obj["ap_ssid"].is<const char*>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "ap_ssid", "must be a string");
            return false;
        }
        const char* s = obj["ap_ssid"].as<const char*>();
        size_t len = strlen(s);
        if (len < 1 || len > MAX_WIFI_SSID_LEN) {
            char msg[32];
            snprintf(msg, sizeof(msg), "must be 1-%u bytes", (unsigned)MAX_WIFI_SSID_LEN);
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "ap_ssid", msg);
            return false;
        }
        memset(out.ap_ssid, 0, sizeof(out.ap_ssid));
        strncpy(out.ap_ssid, s, sizeof(out.ap_ssid) - 1);
        addApplied("ap_ssid");
    }

    // 4. ap_pass (empty/absent = keep)
    if (hasJsonKey(obj, "ap_pass")) {
        if (!obj["ap_pass"].is<const char*>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "ap_pass", "must be a string");
            return false;
        }
        const char* p = obj["ap_pass"].as<const char*>();
        size_t plen = strlen(p);
        if (plen > 0) {
            if (plen < MIN_PASS_LEN || plen > MAX_PASS_LEN) {
                char msg[48];
                snprintf(msg, sizeof(msg), "must be empty to keep or %u-%u bytes",
                         (unsigned)MIN_PASS_LEN, (unsigned)MAX_PASS_LEN);
                setStrErr(errField, errFieldLen, errReason, errReasonLen, "ap_pass", msg);
                return false;
            }
            memset(out.ap_pass, 0, sizeof(out.ap_pass));
            strncpy(out.ap_pass, p, sizeof(out.ap_pass) - 1);
            addApplied("ap_pass");
        }
    }

    // 5. setup_window_s
    if (hasJsonKey(obj, "setup_window_s")) {
        int64_t v = 0;
        if (!parseIntRangeField(obj, "setup_window_s", MIN_SETUP_WINDOW_S, MAX_SETUP_WINDOW_S,
                                v, errField, errFieldLen, errReason, errReasonLen)) {
            return false;
        }
        out.setup_window_s = (uint16_t)v;
        addApplied("setup_window_s");
    }

    // 6. admin_pass (absent/empty = keep; null or clear_admin_pass=true = clear; else 8-63)
    bool clearAdm = false;
    if (hasJsonKey(obj, "clear_admin_pass")) {
        if (!obj["clear_admin_pass"].is<bool>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "clear_admin_pass", "must be a boolean");
            return false;
        }
        clearAdm = obj["clear_admin_pass"].as<bool>();
    }
    if (clearAdm && hasJsonKey(obj, "admin_pass")) {
        setStrErr(errField, errFieldLen, errReason, errReasonLen, "admin_pass", "cannot be sent together with clear_admin_pass");
        return false;
    }
    if (hasJsonKey(obj, "admin_pass")) {
        if (obj["admin_pass"].isNull()) {
            clearAdm = true;
        } else if (obj["admin_pass"].is<const char*>()) {
            const char* p = obj["admin_pass"].as<const char*>();
            size_t plen = strlen(p);
            if (plen > 0) {
                if (plen < MIN_PASS_LEN || plen > MAX_PASS_LEN) {
                    char msg[48];
                    snprintf(msg, sizeof(msg), "must be empty or %u-%u bytes",
                             (unsigned)MIN_PASS_LEN, (unsigned)MAX_PASS_LEN);
                    setStrErr(errField, errFieldLen, errReason, errReasonLen, "admin_pass", msg);
                    return false;
                }
                memset(out.admin_pass, 0, sizeof(out.admin_pass));
                strncpy(out.admin_pass, p, sizeof(out.admin_pass) - 1);
                addApplied("admin_pass");
            }
        } else {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "admin_pass", "must be string or null");
            return false;
        }
    }
    if (clearAdm) {
        memset(out.admin_pass, 0, sizeof(out.admin_pass));
        addApplied("admin_pass");
    }

    // 7. debounce_ms
    if (hasJsonKey(obj, "debounce_ms")) {
        int64_t v = 0;
        if (!parseIntRangeField(obj, "debounce_ms", MIN_DEBOUNCE_MS, MAX_DEBOUNCE_MS,
                                v, errField, errFieldLen, errReason, errReasonLen)) {
            return false;
        }
        out.debounce_ms = (uint32_t)v;
        addApplied("debounce_ms");
    }

    // 8. restore_cooldown_ms
    if (hasJsonKey(obj, "restore_cooldown_ms")) {
        int64_t v = 0;
        if (!parseIntRangeField(obj, "restore_cooldown_ms", MIN_COOLDOWN_MS, MAX_COOLDOWN_MS,
                                v, errField, errFieldLen, errReason, errReasonLen)) {
            return false;
        }
        out.restore_cooldown_ms = (uint32_t)v;
        addApplied("restore_cooldown_ms");
    }

    // 9. snooze_default_min
    if (hasJsonKey(obj, "snooze_default_min")) {
        int64_t v = 0;
        if (!parseIntRangeField(obj, "snooze_default_min", MIN_SNOOZE_MIN, MAX_SNOOZE_MIN,
                                v, errField, errFieldLen, errReason, errReasonLen)) {
            return false;
        }
        out.snooze_default_min = (uint16_t)v;
        addApplied("snooze_default_min");
    }

    // 10. alert_mode
    if (hasJsonKey(obj, "alert_mode")) {
        if (!obj["alert_mode"].is<int64_t>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "alert_mode", "must be 0 or 1");
            return false;
        }
        int64_t v = obj["alert_mode"].as<int64_t>();
        if (v != ALERT_OUTAGE_ONLY && v != ALERT_OUTAGE_AND_RESTORE) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "alert_mode", "must be 0 or 1");
            return false;
        }
        out.alert_mode = (AlertTrackingMode)v;
        addApplied("alert_mode");
    }

    // 11. cloud_enabled
    if (hasJsonKey(obj, "cloud_enabled")) {
        if (!obj["cloud_enabled"].is<bool>()) {
            setStrErr(errField, errFieldLen, errReason, errReasonLen, "cloud_enabled", "must be a boolean");
            return false;
        }
        out.cloud_enabled = obj["cloud_enabled"].as<bool>();
        addApplied("cloud_enabled");
    }

    // 12. webhook_url & local_url
    bool urlApplied = false;
    if (!parseUrlField(obj, "webhook_url", out.webhook_url, sizeof(out.webhook_url),
                       urlApplied, errField, errFieldLen, errReason, errReasonLen)) {
        return false;
    }
    if (urlApplied) addApplied("webhook_url");

    if (!parseUrlField(obj, "local_url", out.local_url, sizeof(out.local_url),
                       urlApplied, errField, errFieldLen, errReason, errReasonLen)) {
        return false;
    }
    if (urlApplied) addApplied("local_url");

    return true;
}

size_t buildConfigJsonBuf(const DeviceConfig& cfg, char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return 0;
    JsonDocument doc;
    JsonArray wifiArr = doc["wifi"].to<JsonArray>();
    for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
        JsonObject w = wifiArr.add<JsonObject>();
        w["slot"] = i;
        w["ssid"] = (const char*)cfg.wifi[i].ssid;
        w["has_pass"] = (strlen(cfg.wifi[i].pass) > 0);
    }
    doc["device_name"]         = (const char*)cfg.device_name;
    doc["ap_ssid"]             = (const char*)cfg.ap_ssid;
    doc["has_ap_pass"]         = (strlen(cfg.ap_pass) > 0);
    doc["setup_window_s"]      = cfg.setup_window_s;
    doc["has_admin_pass"]      = (strlen(cfg.admin_pass) > 0);
    doc["debounce_ms"]         = cfg.debounce_ms;
    doc["restore_cooldown_ms"] = cfg.restore_cooldown_ms;
    doc["snooze_default_min"]  = cfg.snooze_default_min;
    doc["alert_mode"]          = (uint8_t)cfg.alert_mode;
    doc["cloud_enabled"]       = cfg.cloud_enabled;
    doc["webhook_url"]         = (const char*)cfg.webhook_url;
    doc["local_url"]           = (const char*)cfg.local_url;

    if (measureJson(doc) >= outCap) {
        outBuf[0] = '\0';
        return 0;
    }
    return serializeJson(doc, outBuf, outCap);
}

void loadConfigFromKv(IKvStore& kv, DeviceConfig& cfg, uint64_t efuseMac) {
    initDefaultDeviceConfig(cfg, efuseMac);
    if (!kv.begin(NVS_NAMESPACE, false)) {
        HAL_LOG_PRINT("[NVS] Failed to open namespace; using defaults.");
        return;
    }

    uint8_t cfgVer = kv.getUChar("cfg_ver", 0);
    if (cfgVer > CFG_VERSION) {
        HAL_LOG_PRINTF("[NVS] Unknown schema cfg_ver=%u (supported <= %u); using defaults without modifying NVS.\n",
                       (unsigned)cfgVer, (unsigned)CFG_VERSION);
        kv.end();
        return;
    }

    cfg.armed = kv.getBool("armed", DEFAULT_ARMED);

    for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
        char ks[5], kp[5];
        snprintf(ks, sizeof(ks), "w%us", i);
        snprintf(kp, sizeof(kp), "w%up", i);
        kv.getString(ks, cfg.wifi[i].ssid, sizeof(cfg.wifi[i].ssid), "");
        kv.getString(kp, cfg.wifi[i].pass, sizeof(cfg.wifi[i].pass), "");
        size_t plen = strlen(cfg.wifi[i].pass);
        if (plen > 0 && (plen < MIN_PASS_LEN || plen > MAX_PASS_LEN)) {
            HAL_LOG_PRINTF("[NVS] Invalid stored password length for slot %u; clearing password.\n", (unsigned)i);
            memset(cfg.wifi[i].pass, 0, sizeof(cfg.wifi[i].pass));
        }
        if (cfg.wifi[i].ssid[0] == '\0') {
            memset(cfg.wifi[i].pass, 0, sizeof(cfg.wifi[i].pass));
        }
    }

    char defApSsid[MAX_WIFI_SSID_LEN + 1];
    formatDefaultApSsid(efuseMac, defApSsid, sizeof(defApSsid));

    char devNameTmp[64];
    memset(devNameTmp, 0, sizeof(devNameTmp));
    size_t devNameLen = kv.getString("dev_name", devNameTmp, sizeof(devNameTmp), DEFAULT_DEVICE_NAME);
    kv.getString("ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid), defApSsid);
    kv.getString("ap_pass", cfg.ap_pass, sizeof(cfg.ap_pass), DEFAULT_AP_PASS);
    kv.getString("adm_p", cfg.admin_pass, sizeof(cfg.admin_pass), "");
    kv.getString("webhook", cfg.webhook_url, sizeof(cfg.webhook_url), "");
    kv.getString("local_url", cfg.local_url, sizeof(cfg.local_url), "");

    if (devNameLen >= sizeof(cfg.device_name) || !isValidDeviceName(devNameTmp)) {
        HAL_LOG_PRINT("[NVS] Invalid or oversized stored dev_name -> falling back to default.");
        strncpy(cfg.device_name, DEFAULT_DEVICE_NAME, sizeof(cfg.device_name) - 1);
        cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';
    } else {
        strncpy(cfg.device_name, devNameTmp, sizeof(cfg.device_name) - 1);
        cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';
    }
    if (strlen(cfg.ap_ssid) == 0) {
        HAL_LOG_PRINT("[NVS] Empty stored ap_ssid -> falling back to default.");
        strncpy(cfg.ap_ssid, defApSsid, sizeof(cfg.ap_ssid) - 1);
        cfg.ap_ssid[sizeof(cfg.ap_ssid) - 1] = '\0';
    }
    size_t apPassLen = strlen(cfg.ap_pass);
    if (apPassLen < MIN_PASS_LEN || apPassLen > MAX_PASS_LEN) {
        HAL_LOG_PRINT("[NVS] Invalid stored ap_pass -> falling back to default.");
        memset(cfg.ap_pass, 0, sizeof(cfg.ap_pass));
        strncpy(cfg.ap_pass, DEFAULT_AP_PASS, sizeof(cfg.ap_pass) - 1);
    }
    size_t admLen = strlen(cfg.admin_pass);
    if (admLen > 0 && (admLen < MIN_PASS_LEN || admLen > MAX_PASS_LEN)) {
        HAL_LOG_PRINT("[NVS] Invalid stored adm_p -> clearing.");
        memset(cfg.admin_pass, 0, sizeof(cfg.admin_pass));
    }
    sanitizeStoredUrl(cfg.webhook_url, sizeof(cfg.webhook_url), "webhook");
    sanitizeStoredUrl(cfg.local_url, sizeof(cfg.local_url), "local_url");

    cfg.setup_window_s = (uint16_t)clampOrDefaultNvs(
        (int64_t)kv.getUShort("ap_win", DEFAULT_SETUP_WINDOW_S),
        MIN_SETUP_WINDOW_S, MAX_SETUP_WINDOW_S, DEFAULT_SETUP_WINDOW_S, "ap_win");
    cfg.debounce_ms = (uint32_t)clampOrDefaultNvs(
        (int64_t)kv.getULong("debounce", DEFAULT_DEBOUNCE_MS),
        MIN_DEBOUNCE_MS, MAX_DEBOUNCE_MS, DEFAULT_DEBOUNCE_MS, "debounce");
    cfg.restore_cooldown_ms = (uint32_t)clampOrDefaultNvs(
        (int64_t)kv.getULong("cooldown", DEFAULT_COOLDOWN_MS),
        MIN_COOLDOWN_MS, MAX_COOLDOWN_MS, DEFAULT_COOLDOWN_MS, "cooldown");
    cfg.snooze_default_min = (uint16_t)clampOrDefaultNvs(
        (int64_t)kv.getUShort("snooze", DEFAULT_SNOOZE_MIN),
        MIN_SNOOZE_MIN, MAX_SNOOZE_MIN, DEFAULT_SNOOZE_MIN, "snooze");
    cfg.alert_mode = (AlertTrackingMode)clampOrDefaultNvs(
        (int64_t)kv.getUChar("alert_mode", DEFAULT_ALERT_MODE),
        ALERT_OUTAGE_ONLY, ALERT_OUTAGE_AND_RESTORE, DEFAULT_ALERT_MODE, "alert_mode");
    cfg.cloud_enabled = kv.getBool("cloud_en", DEFAULT_CLOUD_ENABLED);

    kv.end();
}

bool saveConfigDiffToKv(IKvStore& kv, const DeviceConfig& cfg,
                        uint8_t* outWritten,
                        char writtenKeys[][16], uint8_t maxKeys) {
    uint8_t written = 0;
    if (outWritten) *outWritten = 0;

    if (!kv.begin(NVS_NAMESPACE, false)) {
        HAL_LOG_PRINT("[NVS] Failed to open namespace for save.");
        return false;
    }

    auto writeOk = [&](const char* k, size_t n) -> bool {
        if (n == 0) {
            HAL_LOG_PRINTF("[NVS] Failed writing key '%s'.\n", k);
            return false;
        }
        if (writtenKeys) {
            if (written < maxKeys) {
                strncpy(writtenKeys[written], k, 15);
                writtenKeys[written][15] = '\0';
                written++;
            }
        } else {
            written++;
        }
        return true;
    };

    bool ok = false;
    do {
        if (kv.getUChar("cfg_ver", 0) != CFG_VERSION) {
            if (!writeOk("cfg_ver", kv.putUChar("cfg_ver", CFG_VERSION))) break;
        }
        if (!kv.isKey("armed") || kv.getBool("armed", !cfg.armed) != cfg.armed) {
            if (!writeOk("armed", kv.putBool("armed", cfg.armed))) break;
        }

        char curStr[MAX_WEBHOOK_URL_LEN + 8];
        bool slotFailed = false;
        for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
            char ks[5], kp[5];
            snprintf(ks, sizeof(ks), "w%us", i);
            snprintf(kp, sizeof(kp), "w%up", i);
            kv.getString(ks, curStr, sizeof(curStr), "");
            if (strcmp(curStr, cfg.wifi[i].ssid) != 0) {
                if (!writeOk(ks, kv.putString(ks, cfg.wifi[i].ssid))) { slotFailed = true; break; }
            }
            kv.getString(kp, curStr, sizeof(curStr), "");
            if (strcmp(curStr, cfg.wifi[i].pass) != 0) {
                if (!writeOk(kp, kv.putString(kp, cfg.wifi[i].pass))) { slotFailed = true; break; }
            }
        }
        if (slotFailed) break;

        kv.getString("dev_name", curStr, sizeof(curStr), "");
        if (strcmp(curStr, cfg.device_name) != 0) {
            if (!writeOk("dev_name", kv.putString("dev_name", cfg.device_name))) break;
        }
        kv.getString("ap_ssid", curStr, sizeof(curStr), "");
        if (strcmp(curStr, cfg.ap_ssid) != 0) {
            if (!writeOk("ap_ssid", kv.putString("ap_ssid", cfg.ap_ssid))) break;
        }
        kv.getString("ap_pass", curStr, sizeof(curStr), "");
        if (strcmp(curStr, cfg.ap_pass) != 0) {
            if (!writeOk("ap_pass", kv.putString("ap_pass", cfg.ap_pass))) break;
        }
        if (!kv.isKey("ap_win") || kv.getUShort("ap_win", 65535) != cfg.setup_window_s) {
            if (!writeOk("ap_win", kv.putUShort("ap_win", cfg.setup_window_s))) break;
        }
        kv.getString("adm_p", curStr, sizeof(curStr), "");
        if (strcmp(curStr, cfg.admin_pass) != 0) {
            if (!writeOk("adm_p", kv.putString("adm_p", cfg.admin_pass))) break;
        }
        kv.getString("webhook", curStr, sizeof(curStr), "");
        if (strcmp(curStr, cfg.webhook_url) != 0) {
            if (!writeOk("webhook", kv.putString("webhook", cfg.webhook_url))) break;
        }
        kv.getString("local_url", curStr, sizeof(curStr), "");
        if (strcmp(curStr, cfg.local_url) != 0) {
            if (!writeOk("local_url", kv.putString("local_url", cfg.local_url))) break;
        }
        if (!kv.isKey("debounce") || kv.getULong("debounce", 0) != cfg.debounce_ms) {
            if (!writeOk("debounce", kv.putULong("debounce", cfg.debounce_ms))) break;
        }
        if (!kv.isKey("cooldown") || kv.getULong("cooldown", 999999UL) != cfg.restore_cooldown_ms) {
            if (!writeOk("cooldown", kv.putULong("cooldown", cfg.restore_cooldown_ms))) break;
        }
        if (!kv.isKey("snooze") || kv.getUShort("snooze", 0) != cfg.snooze_default_min) {
            if (!writeOk("snooze", kv.putUShort("snooze", cfg.snooze_default_min))) break;
        }
        if (!kv.isKey("alert_mode") || kv.getUChar("alert_mode", 255) != (uint8_t)cfg.alert_mode) {
            if (!writeOk("alert_mode", kv.putUChar("alert_mode", (uint8_t)cfg.alert_mode))) break;
        }
        if (!kv.isKey("cloud_en") || kv.getBool("cloud_en", !cfg.cloud_enabled) != cfg.cloud_enabled) {
            if (!writeOk("cloud_en", kv.putBool("cloud_en", cfg.cloud_enabled))) break;
        }

        ok = true;
    } while (false);

    kv.end();
    if (outWritten) *outWritten = written;
    return ok;
}

uint8_t loadLastGoodWifiSlot(IKvStore& kv) {
    if (!kv.begin(NVS_NAMESPACE, true)) {
        return WIFI_SLOT_NONE;
    }
    uint8_t slot = kv.getUChar("w_last", WIFI_SLOT_NONE);
    kv.end();
    return (slot < WIFI_SLOTS) ? slot : WIFI_SLOT_NONE;
}

bool saveLastGoodWifiSlot(IKvStore& kv, uint8_t slot) {
    if (slot >= WIFI_SLOTS) return false;
    if (!kv.begin(NVS_NAMESPACE, false)) {
        HAL_LOG_PRINT("[NVS] Failed to open namespace for w_last write.");
        return false;
    }
    size_t n = kv.putUChar("w_last", slot);
    kv.end();
    if (n == 0) {
        HAL_LOG_PRINT("[NVS] Failed writing w_last.");
        return false;
    }
    return true;
}

bool wipeConfigKv(IKvStore& kv) {
    if (!kv.begin(NVS_NAMESPACE, false)) {
        HAL_LOG_PRINT("[NVS] Failed to open namespace for wipe.");
        return false;
    }
    bool ok = kv.clear();
    kv.end();
    return ok;
}
