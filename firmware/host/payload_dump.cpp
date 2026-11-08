#include "config.h"
#include "config_codec.h"
#include "protocol_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void printUsage(FILE* stream) {
    fprintf(
        stream,
        "usage: payload_dump <output_dir> [config_cases.json]\n"
        "  <output_dir>         Writes sample contract JSON payloads.\n"
        "  [config_cases.json]  Optional JSON test suite run through validateAndMergeConfig;\n"
        "                       writes <output_dir>/config_cases_result.json.\n"
    );
}

static DeviceConfig makeSampleConfig() {
    DeviceConfig c;
    memset(&c, 0, sizeof(c));
    snprintf(c.device_name, sizeof(c.device_name), "%s", DEFAULT_DEVICE_NAME);
    snprintf(c.ap_ssid, sizeof(c.ap_ssid), "%s", "PowerMonitor-A1B2");
    snprintf(c.ap_pass, sizeof(c.ap_pass), "%s", DEFAULT_AP_PASS);
    c.setup_window_s = DEFAULT_SETUP_WINDOW_S;
    c.debounce_ms = DEFAULT_DEBOUNCE_MS;
    c.restore_cooldown_ms = DEFAULT_COOLDOWN_MS;
    c.snooze_default_min = DEFAULT_SNOOZE_MIN;
    c.alert_mode = (AlertTrackingMode)DEFAULT_ALERT_MODE;
    c.cloud_enabled = DEFAULT_CLOUD_ENABLED;
    c.armed = DEFAULT_ARMED;
    return c;
}

static char* readFileBytes(const char* path, size_t& outLen) {
    outLen = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return nullptr;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    outLen = n;
    return buf;
}

static bool writeTextFile(const char* dir, const char* name, const char* content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(content);
    bool ok = (fwrite(content, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool dumpContractPayloads(const char* dir) {
    char buf[2048];
    DeviceConfig cfg = makeSampleConfig();
    snprintf(cfg.wifi[0].ssid, sizeof(cfg.wifi[0].ssid), "HomeWiFi");
    snprintf(cfg.wifi[0].pass, sizeof(cfg.wifi[0].pass), "secretpass");
    snprintf(cfg.admin_pass, sizeof(cfg.admin_pass), "admsecret");
    cfg.cloud_enabled = true;
    snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "https://ntfy.sh/power-test");
    snprintf(cfg.local_url, sizeof(cfg.local_url), "http://192.168.1.60:8080/{event}");

    buildPowerOffJsonBuf(cfg.device_name, 2, 120, 1700000120, buf, sizeof(buf));
    if (!writeTextFile(dir, "POWER_OFF.json", buf)) return false;

    buildPowerOnJsonBuf(cfg.device_name, MODE_ARMED, 305, 425, ALERT_OUTAGE_AND_RESTORE, 1700000425, buf, sizeof(buf));
    if (!writeTextFile(dir, "POWER_ON.json", buf)) return false;

    buildAlarmStopJsonBuf(cfg.device_name, MODE_DISARMED, 0, 650, 1700000650, buf, sizeof(buf));
    if (!writeTextFile(dir, "ALARM_STOP.json", buf)) return false;

    buildTestAlarmJsonBuf(cfg.device_name, MODE_ARMED, 700, 1700000700, buf, sizeof(buf));
    if (!writeTextFile(dir, "TEST_ALARM.json", buf)) return false;

    StatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.deviceName = cfg.device_name;
    snap.fwVersion = FW_VERSION;
    snap.gridState = GRID_ONLINE;
    snap.mode = MODE_ARMED;
    snap.outageDurationS = 0;
    snap.snoozeRemainingS = 0;
    snap.uptimeS = 700;
    snap.wifiConnected = true;
    snap.wifiSsid = "HomeWiFi";
    snap.wifiSlot = 0;
    snap.wifiIp = "192.168.1.50";
    snap.wifiRssi = -55;
    snap.apActive = false;
    snap.apSsid = "PowerMonitor-5566";
    snap.apIp = "";
    snap.apClients = 0;
    snap.apClosesInS = 0;
    snap.adminProtected = true;
    snap.snoozeDefaultMin = 30;
    snap.freeHeap = 180000;
    snap.ts = 1700000700;
    snap.notifyLocal.valid = true;
    snprintf(snap.notifyLocal.event, sizeof(snap.notifyLocal.event), "TEST_ALARM");
    snap.notifyLocal.ok = true;
    snap.notifyLocal.code = 200;
    snap.notifyLocal.reason[0] = '\0';
    snap.notifyLocal.ageS = 15;
    snap.notifyCloud.valid = true;
    snprintf(snap.notifyCloud.event, sizeof(snap.notifyCloud.event), "POWER_OFF");
    snap.notifyCloud.ok = false;
    snap.notifyCloud.code = -11;
    snprintf(snap.notifyCloud.reason, sizeof(snap.notifyCloud.reason), "Read timeout");
    snap.notifyCloud.ageS = 120;
    buildStatusJsonBuf(snap, buf, sizeof(buf));
    if (!writeTextFile(dir, "status.json", buf)) return false;

    snap.gridState = GRID_BOOTING;
    snap.uptimeS = 0;
    snap.ts = 0;
    memset(&snap.notifyLocal, 0, sizeof(snap.notifyLocal));
    memset(&snap.notifyCloud, 0, sizeof(snap.notifyCloud));
    buildStatusJsonBuf(snap, buf, sizeof(buf));
    if (!writeTextFile(dir, "status_booting.json", buf)) return false;

    buildConfigJsonBuf(cfg, buf, sizeof(buf));
    if (!writeTextFile(dir, "config.json", buf)) return false;

    return true;
}

static bool runConfigCases(const char* outDir, const char* casesPath) {
    size_t rawLen = 0;
    char* raw = readFileBytes(casesPath, rawLen);
    if (!raw) {
        fprintf(stderr, "Failed to read config cases: %s\n", casesPath);
        return false;
    }

    JsonDocument casesDoc;
    DeserializationError err = deserializeJson(casesDoc, raw, rawLen);
    free(raw);
    if (err) {
        fprintf(stderr, "Failed to parse %s: %s\n", casesPath, err.c_str());
        return false;
    }

    JsonArrayConst casesArr;
    if (casesDoc.is<JsonObjectConst>() && casesDoc["cases"].is<JsonArrayConst>()) {
        casesArr = casesDoc["cases"].as<JsonArrayConst>();
    } else if (casesDoc.is<JsonArrayConst>()) {
        casesArr = casesDoc.as<JsonArrayConst>();
    } else {
        fprintf(stderr, "Invalid config cases format in %s\n", casesPath);
        return false;
    }

    JsonDocument outDoc;
    JsonArray results = outDoc.to<JsonArray>();

    for (JsonVariantConst item : casesArr) {
        if (!item.is<JsonObjectConst>()) {
            fprintf(stderr, "Each config case must be an object\n");
            return false;
        }
        JsonObjectConst entry = item.as<JsonObjectConst>();
        const char* name = entry["name"] | "";

        DeviceConfig cur = makeSampleConfig();
        if (entry["initial"].is<JsonObjectConst>()) {
            DeviceConfig mergedInit;
            char initField[64] = {0};
            char initReason[128] = {0};
            char initApplied[16][24];
            uint8_t initAppliedCount = 0;
            bool initWifiChanged = false;
            if (!validateAndMergeConfig(
                    cur,
                    entry["initial"].as<JsonObjectConst>(),
                    mergedInit,
                    initField,
                    sizeof(initField),
                    initReason,
                    sizeof(initReason),
                    initApplied,
                    initAppliedCount,
                    initWifiChanged
                )) {
                fprintf(stderr, "Invalid initial config in case '%s': %s (%s)\n",
                        name, initField, initReason);
                return false;
            }
            cur = mergedInit;
        }

        JsonObject res = results.add<JsonObject>();
        res["name"] = name;

        if (!entry["body"].is<JsonObjectConst>()) {
            res["ok"] = false;
            res["field"] = "body";
            res["reason"] = "invalid JSON object";
            res["applied"].to<JsonArray>();
            continue;
        }

        DeviceConfig outCfg;
        char errField[64] = {0};
        char errReason[128] = {0};
        char applied[16][24];
        uint8_t appliedCount = 0;
        bool wifiChanged = false;

        bool ok = validateAndMergeConfig(
            cur,
            entry["body"].as<JsonObjectConst>(),
            outCfg,
            errField,
            sizeof(errField),
            errReason,
            sizeof(errReason),
            applied,
            appliedCount,
            wifiChanged
        );

        res["ok"] = ok;
        res["field"] = ok ? "" : errField;
        res["reason"] = ok ? "" : errReason;
        JsonArray appArr = res["applied"].to<JsonArray>();
        if (ok) {
            for (uint8_t i = 0; i < appliedCount; ++i) {
                appArr.add((const char*)applied[i]);
            }
        }
    }

    size_t needed = measureJson(outDoc);
    char* outBuf = (char*)malloc(needed + 1);
    if (!outBuf) return false;
    serializeJson(outDoc, outBuf, needed + 1);
    bool wrote = writeTextFile(outDir, "config_cases_result.json", outBuf);
    free(outBuf);
    return wrote;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printUsage(stdout);
        return 0;
    }
    if (argc < 2 || !argv[1] || argv[1][0] == '\0') {
        printUsage(stderr);
        return 64;
    }
    if (!dumpContractPayloads(argv[1])) {
        fprintf(stderr, "Failed to dump payloads to %s\n", argv[1]);
        return 2;
    }
    printf("Dumped sample payloads to %s\n", argv[1]);

    if (argc >= 3 && argv[2] && argv[2][0] != '\0') {
        if (!runConfigCases(argv[1], argv[2])) {
            fprintf(stderr, "Failed to run config cases from %s\n", argv[2]);
            return 3;
        }
        printf("Wrote config case results to %s/config_cases_result.json\n", argv[1]);
    }
    return 0;
}
