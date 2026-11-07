#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "hal_esp32.h"
#include "config_codec.h"
#include "protocol_helpers.h"
#include "power_sensor.h"
#include "network_mgr.h"
#include "ota_update.h"
#include "web_ui.h"

// =====================================================================
// Global Component Instances
// =====================================================================
static Esp32KvStore    kvStore;
static PowerSensor     powerSensor;
static NetworkManager  networkMgr;
static WebServer       webServer(80);

// Global Runtime State
static DeviceConfig    deviceConfig;
static SystemMode      currentMode    = MODE_ARMED;
static uint32_t        snoozeUntilMs  = 0;

// Button Debounce and Long-Press Tracking
static bool            btnLastRawState = HIGH;
static bool            btnConfirmedState = HIGH;
static uint32_t        btnStateChangeMs = 0;
static uint32_t        btnDownStartMs  = 0;

// =====================================================================
// Forward Declarations
// =====================================================================
void loadConfiguration();
bool saveConfiguration();
void setupWebServer();
void updateStatusLed();
void updateButton();
void armSystem();
void disarmSystem();
void toggleArm();
bool snoozeSystem(uint16_t durationMin);
void testAlarm();
void performCleanRestart();
String buildStatusJson();

// =====================================================================
// Setup Routine
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==============================================");
    Serial.println("  ESP32 Power Outage Monitor & Alarm System   ");
    Serial.printf("  Firmware: v%s\n", FW_VERSION);
    Serial.println("==============================================");

    // Hardware Pins Configuration
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    // Initialize Hardware Watchdog Timer before NVS/Wi-Fi init
    esp_err_t wdtInitErr = esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    esp_err_t wdtAddErr  = esp_task_wdt_add(NULL);
    if (wdtInitErr != ESP_OK || wdtAddErr != ESP_OK) {
        Serial.printf("[System] Hardware WDT init failed (init=%d, add=%d); restarting.\n",
                      (int)wdtInitErr, (int)wdtAddErr);
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    // Load persistent settings from NVS Flash
    loadConfiguration();
    esp_task_wdt_reset();

    // Initialize Grid Power Sensor with configured debounce and cooldown
    powerSensor.init(PIN_MAINS_SENSE, deviceConfig.debounce_ms, deviceConfig.restore_cooldown_ms);

    // Event Callbacks from Power Sensor
    powerSensor.onOutage([]() {
        Serial.println("\n[EVENT] >>> MAINS POWER OUTAGE DETECTED <<<");
        networkMgr.sendPowerOffAlert(currentMode, powerSensor.getOutageDurationSeconds());
    });

    powerSensor.onRestored([](uint32_t durationS) {
        Serial.printf("\n[EVENT] >>> MAINS POWER RESTORED <<< (Outage duration: %u s)\n", durationS);
        networkMgr.sendPowerOnAlert(currentMode, durationS, deviceConfig.alert_mode);
    });

    // Wire restart callback and initialize Network, Wi-Fi, and mDNS
    networkMgr.onRestartRequested([]() { performCleanRestart(); });
    networkMgr.init(deviceConfig, kvStore);

    // Initialize Mobile Web Server & REST API
    setupWebServer();
    webServer.begin();
    esp_task_wdt_reset();

    Serial.println("[System] Initialization complete. Hardware WDT active. Monitoring started.");
}

// =====================================================================
// Main Loop
// =====================================================================
void loop() {
    // 1. Update Grid Mains Sensor & Debounce Engine
    powerSensor.update();

    // 2. Handle Snooze Auto-Rearm Timer (wrap-safe integer comparison)
    if (currentMode == MODE_SNOOZED && ((int32_t)(millis() - snoozeUntilMs) >= 0)) {
        Serial.println("[System] Snooze period expired – System auto-rearmed to ARMED.");
        armSystem();
    }

    // 3. Update Network & Pending Alert Processing
    networkMgr.update(
        powerSensor.getGridState(),
        currentMode,
        powerSensor.getOutageDurationSeconds()
    );

    // 4. Handle Web Server Requests
    webServer.handleClient();

    // 5. Physical BOOT Button Handler
    updateButton();

    // 6. LED Status Indicator Pattern Generator
    updateStatusLed();

    // 7. Pet Hardware Watchdog
    esp_task_wdt_reset();

    // Small yield for FreeRTOS scheduler
    delay(5);
}

// =====================================================================
// System Arming / Snoozing Control Logic
// =====================================================================
void armSystem() {
    if (currentMode == MODE_ARMED && deviceConfig.armed) return; // NVS flash wear guard
    SystemMode prevMode = currentMode;
    currentMode = MODE_ARMED;
    snoozeUntilMs = 0;
    if (!deviceConfig.armed) {
        deviceConfig.armed = true;
        if (!saveConfiguration()) {
            Serial.println("[NVS] Warning: failed to persist armed state.");
        }
    }
    networkMgr.handleEnterArmed(prevMode, powerSensor.getGridState(), powerSensor.getOutageDurationSeconds());
    Serial.println("[Control] System state changed: ARMED");
}

void disarmSystem() {
    if (currentMode == MODE_DISARMED && !deviceConfig.armed) return; // NVS flash wear guard
    SystemMode prevMode = currentMode;
    currentMode = MODE_DISARMED;
    snoozeUntilMs = 0;
    deviceConfig.armed = false;
    if (!saveConfiguration()) {
        Serial.println("[NVS] Warning: failed to persist disarmed state.");
    }
    networkMgr.sendAlarmStop(prevMode, currentMode, 0);
    Serial.println("[Control] System state changed: DISARMED");
}

void toggleArm() {
    if (currentMode == MODE_ARMED) {
        disarmSystem();
    } else {
        armSystem();
    }
}

bool snoozeSystem(uint16_t durationMin) {
    if (!canSnoozeMode(currentMode)) {
        Serial.println("[Control] SNOOZE ignored: system is disarmed.");
        return false;
    }
    SystemMode prevMode = currentMode;
    uint16_t normMin = normalizeSnoozeMinutes(durationMin, deviceConfig.snooze_default_min);
    uint32_t snoozeRemS = (uint32_t)normMin * 60UL;
    currentMode = MODE_SNOOZED;
    snoozeUntilMs = millis() + (snoozeRemS * 1000UL);
    networkMgr.sendAlarmStop(prevMode, currentMode, snoozeRemS);
    Serial.printf("[Control] System state changed: SNOOZED for %u minutes\n", normMin);
    return true;
}

void testAlarm() {
    Serial.println("[Control] Triggering manual test alert...");
    networkMgr.sendTestAlert(currentMode);
}

void performCleanRestart() {
    esp_task_wdt_reset();
    esp_task_wdt_delete(NULL);
    webServer.stop();
    networkMgr.shutdownForRestart();
    Serial.flush();
    delay(100);
    ESP.restart();
}

// =====================================================================
// Physical Button Processing (GPIO 0 / BOOT Button)
// =====================================================================
void updateButton() {
    int raw = digitalRead(PIN_BOOT_BUTTON);

    if (raw != btnLastRawState) {
        btnLastRawState = raw;
        btnStateChangeMs = millis();
    }

    if ((millis() - btnStateChangeMs) > BTN_DEBOUNCE_MS) {
        if (raw != btnConfirmedState) {
            btnConfirmedState = raw;

            if (btnConfirmedState == LOW) {
                // Button Pressed Down
                btnDownStartMs = millis();
            } else {
                // Button Released Up
                uint32_t pressDuration = millis() - btnDownStartMs;
                ButtonAction act = classifyPress(pressDuration);

                if (act == BTN_ACT_FACTORY_RESET) {
                    // >10s: Factory NVS Wipe & Restart
                    Serial.printf("[Button] Factory reset detected (%u ms) -> Clearing NVS & rebooting\n", pressDuration);
                    if (!wipeConfigKv(kvStore)) {
                        Serial.println("[NVS] Error: failed to wipe NVS during factory reset.");
                    }
                    for (int i = 0; i < 10; ++i) {
                        digitalWrite(PIN_STATUS_LED, HIGH);
                        delay(50);
                        digitalWrite(PIN_STATUS_LED, LOW);
                        delay(50);
                        esp_task_wdt_reset();
                    }
                    performCleanRestart();
                } else if (act == BTN_ACT_SETUP_AP) {
                    // 5s - 10s: Open setup AP window for 5 min
                    Serial.printf("[Button] Setup AP hold detected (%u ms) -> Opening setup AP for %u s\n",
                                  pressDuration, SETUP_ONDEMAND_WINDOW_S);
                    networkMgr.openSetupAp(SETUP_ONDEMAND_WINDOW_S);
                } else if (act == BTN_ACT_TEST_RECONNECT) {
                    // 2s - 5s: Test Alarm + Wi-Fi reconnect
                    Serial.printf("[Button] Long press detected (%u ms) -> Test Alert & Wi-Fi reconnect\n", pressDuration);
                    testAlarm();
                    if (!networkMgr.isWiFiConnected()) {
                        networkMgr.reconnectWiFi();
                    }
                } else if (act == BTN_ACT_TOGGLE) {
                    // Short Press (50ms – 2.0s): Toggle ARM / DISARM
                    Serial.printf("[Button] Short press detected (%u ms) -> Toggle ARM/DISARM\n", pressDuration);
                    toggleArm();
                }
            }
        }
    }
}

// =====================================================================
// Status LED Signaling Pattern Engine (GPIO 2)
// =====================================================================
void updateStatusLed() {
    uint32_t now = millis();
    uint32_t heldMs = (btnConfirmedState == LOW) ? (now - btnDownStartMs) : 0;
    LedPattern pattern = selectLedPattern(
        powerSensor.getGridState(),
        networkMgr.isApMode(),
        currentMode,
        btnConfirmedState == LOW,
        heldMs
    );
    digitalWrite(PIN_STATUS_LED, ledLevel(pattern, now) ? HIGH : LOW);
}

// =====================================================================
// JSON Status Serializer
// =====================================================================
String buildStatusJson() {
    uint32_t snoozeLeftS = 0;
    if (currentMode == MODE_SNOOZED && ((int32_t)(snoozeUntilMs - millis()) > 0)) {
        snoozeLeftS = (snoozeUntilMs - millis()) / 1000UL;
    }

    String wifiSsid = networkMgr.getConnectedSsid();
    String wifiIp   = networkMgr.getIpAddress();
    String apSsid   = networkMgr.getApSsid();
    String apIp     = networkMgr.getApIpAddress();

    StatusSnapshot snap;
    snap.deviceName       = deviceConfig.device_name;
    snap.fwVersion        = FW_VERSION;
    snap.gridState        = powerSensor.getGridState();
    snap.mode             = currentMode;
    snap.outageDurationS  = powerSensor.getOutageDurationSeconds();
    snap.snoozeRemainingS = snoozeLeftS;
    snap.uptimeS          = millis() / 1000UL;
    snap.wifiConnected    = networkMgr.isWiFiConnected();
    snap.wifiSsid         = wifiSsid.c_str();
    snap.wifiSlot         = networkMgr.getWifiSlot();
    snap.wifiIp           = wifiIp.c_str();
    snap.wifiRssi         = networkMgr.getRssi();
    snap.apActive         = networkMgr.isApMode();
    snap.apSsid           = apSsid.c_str();
    snap.apIp             = apIp.c_str();
    snap.apClients        = networkMgr.getApClientCount();
    snap.apClosesInS      = networkMgr.getApClosesInSeconds();
    snap.adminProtected   = (strlen(deviceConfig.admin_pass) > 0);
    snap.snoozeDefaultMin = deviceConfig.snooze_default_min;
    snap.freeHeap         = ESP.getFreeHeap();
    snap.ts               = networkMgr.getEpochTime();
    snap.notifyLocal      = networkMgr.getNotifyStatus(NOTIFY_TARGET_LOCAL);
    snap.notifyCloud      = networkMgr.getNotifyStatus(NOTIFY_TARGET_CLOUD);

    char buf[1280];
    size_t n = buildStatusJsonBuf(snap, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf) - 1) {
        return String("");
    }
    return String(buf);
}

// =====================================================================
// Web Server & REST API Configuration
// =====================================================================
static void recordApTrafficIfLocal() {
    if (networkMgr.isApClientIp((uint32_t)webServer.client().localIP())) {
        networkMgr.notifyApHttpActivity();
    }
}

static bool ensureCsrfAndAuth() {
    recordApTrafficIfLocal();
    if (!webServer.hasHeader("X-Requested-With") ||
        webServer.header("X-Requested-With") != "pm-ui") {
        webServer.send(403, "application/json", "{\"error\":\"csrf: missing X-Requested-With header\"}");
        return false;
    }
    if (strlen(deviceConfig.admin_pass) == 0) {
        return true;
    }
    if (!webServer.authenticate("admin", deviceConfig.admin_pass)) {
        webServer.requestAuthentication();
        return false;
    }
    return true;
}

static void sendJsonError(const char* field, const char* reason) {
    JsonDocument doc;
    doc["error"] = String(field) + ": " + reason;
    String out;
    serializeJson(doc, out);
    webServer.send(400, "application/json", out);
}

static String buildConfigJson() {
    char buf[2048];
    size_t n = buildConfigJsonBuf(deviceConfig, buf, sizeof(buf));
    if (n == 0) {
        return String("");
    }
    return String(buf);
}

void setupWebServer() {
    static const char* COLLECTED_HEADERS[] = { "X-Requested-With" };
    webServer.collectHeaders(COLLECTED_HEADERS, 1);

    // 1. Root SPA Dashboard (PROGMEM, captive portal host redirect on AP)
    webServer.on("/", HTTP_GET, []() {
        recordApTrafficIfLocal();
        if (networkMgr.isApClientIp((uint32_t)webServer.client().localIP())) {
            String host = webServer.hostHeader();
            if (host.length() > 0 && host != SETUP_AP_IP_STR && host != (String(SETUP_AP_IP_STR) + ":80")) {
                webServer.sendHeader("Location", String("http://") + SETUP_AP_IP_STR + "/", true);
                webServer.send(302, "text/plain", "");
                return;
            }
        }
        webServer.send_P(200, "text/html", INDEX_HTML, sizeof(INDEX_HTML) - 1);
    });

    // 2. GET /api/status -> System Telemetry (open, no secrets)
    webServer.on("/api/status", HTTP_GET, []() {
        recordApTrafficIfLocal();
        String json = buildStatusJson();
        if (json.length() == 0) {
            webServer.send(500, "application/json", "{\"error\":\"status: json overflow\"}");
            return;
        }
        webServer.send(200, "application/json", json);
    });

    // 3. POST /api/arm -> Arm System
    webServer.on("/api/arm", HTTP_POST, []() {
        recordApTrafficIfLocal();
        armSystem();
        webServer.send(200, "application/json", "{\"status\":\"ok\",\"mode\":\"ARMED\",\"message\":\"System armed\"}");
    });

    // 4. POST /api/disarm -> Disarm System
    webServer.on("/api/disarm", HTTP_POST, []() {
        recordApTrafficIfLocal();
        disarmSystem();
        webServer.send(200, "application/json", "{\"status\":\"ok\",\"mode\":\"DISARMED\",\"message\":\"System disarmed\"}");
    });

    // 5. POST /api/toggle -> Toggle Arm / Disarm
    webServer.on("/api/toggle", HTTP_POST, []() {
        recordApTrafficIfLocal();
        toggleArm();
        const char* m = (currentMode == MODE_ARMED) ? "ARMED" : "DISARMED";
        String resp = String("{\"status\":\"ok\",\"mode\":\"") + m + "\",\"message\":\"State toggled\"}";
        webServer.send(200, "application/json", resp);
    });

    // 6. POST /api/snooze -> Snooze Alarms (supports ?min=30, ?duration=1800, or JSON)
    webServer.on("/api/snooze", HTTP_POST, []() {
        recordApTrafficIfLocal();
        if (!canSnoozeMode(currentMode)) {
            webServer.send(409, "application/json", "{\"error\":\"snooze: system is disarmed\"}");
            return;
        }
        uint16_t durMin = DEFAULT_SNOOZE_MIN;
        char errField[32] = { 0 };
        char errReason[80] = { 0 };
        String minArg = webServer.hasArg("min") ? webServer.arg("min") : String("");
        String durArg = webServer.hasArg("duration") ? webServer.arg("duration") : String("");
        String bodyArg = webServer.hasArg("plain") ? webServer.arg("plain") : String("");
        if (!parseSnoozeHttpRequest(
                webServer.hasArg("min"), minArg.c_str(),
                webServer.hasArg("duration"), durArg.c_str(),
                bodyArg.c_str(), deviceConfig.snooze_default_min,
                durMin, errField, sizeof(errField), errReason, sizeof(errReason))) {
            sendJsonError(errField, errReason);
            return;
        }

        snoozeSystem(durMin);
        String resp = String("{\"status\":\"ok\",\"mode\":\"SNOOZED\",\"remaining_s\":") + ((uint32_t)durMin * 60UL) +
                             ",\"message\":\"Alarm snoozed for " + durMin + " min\"}";
        webServer.send(200, "application/json", resp);
    });

    // 7. POST /api/test -> Dispatch Test Alert
    webServer.on("/api/test", HTTP_POST, []() {
        recordApTrafficIfLocal();
        testAlarm();
        webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Test alert dispatched\"}");
    });

    // 8a. GET /api/config -> Full Configuration (without secret passwords)
    webServer.on("/api/config", HTTP_GET, []() {
        if (!ensureCsrfAndAuth()) return;
        String json = buildConfigJson();
        if (json.length() == 0) {
            webServer.send(500, "application/json", "{\"error\":\"config: json overflow\"}");
            return;
        }
        webServer.send(200, "application/json", json);
    });

    // 8b. POST /api/config -> Validate & Apply Partial Configuration Atomically
    webServer.on("/api/config", HTTP_POST, []() {
        if (!ensureCsrfAndAuth()) return;
        if (webServer.clientContentLength() > (int)HTTP_MAX_BODY_BYTES ||
            (webServer.hasArg("plain") && webServer.arg("plain").length() > HTTP_MAX_BODY_BYTES)) {
            webServer.send(413, "application/json", "{\"error\":\"body: too large\"}");
            return;
        }
        if (!webServer.hasArg("plain")) {
            webServer.send(400, "application/json", "{\"error\":\"body: missing request body\"}");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, webServer.arg("plain"));
        if (err || !doc.is<JsonObject>()) {
            webServer.send(400, "application/json", "{\"error\":\"body: invalid JSON object\"}");
            return;
        }

        JsonObjectConst obj = doc.as<JsonObjectConst>();
        DeviceConfig next;
        char errField[48] = { 0 };
        char errReason[96] = { 0 };
        char applied[16][24];
        uint8_t appliedCount = 0;
        bool wifiChanged = false;
        if (!validateAndMergeConfig(
                deviceConfig, obj, next,
                errField, sizeof(errField),
                errReason, sizeof(errReason),
                applied, appliedCount, wifiChanged)) {
            sendJsonError(errField, errReason);
            return;
        }

        // Persist to NVS before updating RAM state or answering HTTP
        if (!saveConfigDiffToKv(kvStore, next)) {
            Serial.println("[NVS] Error: failed to save configuration to flash.");
            webServer.send(500, "application/json", "{\"error\":\"nvs: write failed\"}");
            return;
        }
        Serial.println("[NVS] Configuration saved to flash.");
        deviceConfig = next;
        powerSensor.setDebounceMs(deviceConfig.debounce_ms);
        powerSensor.setCooldownMs(deviceConfig.restore_cooldown_ms);
        networkMgr.applyConfigImmediate(deviceConfig);

        JsonDocument resp;
        resp["status"] = "ok";
        JsonArray appArr = resp["applied"].to<JsonArray>();
        for (uint8_t i = 0; i < appliedCount; ++i) {
            appArr.add((const char*)applied[i]);
        }
        resp["message"] = "Configuration applied";

        String respStr;
        serializeJson(resp, respStr);
        webServer.send(200, "application/json", respStr);
        networkMgr.requestApplyConfig(deviceConfig, wifiChanged);
    });

    // 8c. GET /api/scan -> Async Wi-Fi Scan
    webServer.on("/api/scan", HTTP_GET, []() {
        if (!ensureCsrfAndAuth()) return;
        String json = networkMgr.pollWifiScanJson();
        webServer.send(200, "application/json", json);
    });

    // 8d. POST /api/factory-reset -> Wipe NVS & Deferred Reboot
    webServer.on("/api/factory-reset", HTTP_POST, []() {
        if (!ensureCsrfAndAuth()) return;
        Serial.println("[API] Factory reset requested -> Scheduling NVS wipe & reboot");
        webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Factory reset complete. Rebooting...\"}");
        networkMgr.requestFactoryReset();
    });

    // 9. POST /api/setup-ap -> Deferred Open Setup AP for on-demand window
    webServer.on("/api/setup-ap", HTTP_POST, []() {
        if (!ensureCsrfAndAuth()) return;
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "{\"status\":\"ok\",\"message\":\"Setup AP opened for %u minutes\"}",
                 (unsigned)(SETUP_ONDEMAND_WINDOW_S / 60U));
        webServer.send(200, "application/json", msg);
        networkMgr.requestOpenSetupAp(SETUP_ONDEMAND_WINDOW_S);
    });

    // 10. POST /api/setup-ap/close -> Deferred Close Setup AP
    webServer.on("/api/setup-ap/close", HTTP_POST, []() {
        if (!ensureCsrfAndAuth()) return;
        if (!networkMgr.hasSavedNetworks()) {
            webServer.send(400, "application/json", "{\"error\":\"setup_ap: cannot close without saved Wi-Fi networks\"}");
            return;
        }
        webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Setup AP closing\"}");
        networkMgr.requestCloseSetupAp();
    });

    // 11. POST /api/reboot -> Deferred Reboot ESP32
    webServer.on("/api/reboot", HTTP_POST, []() {
        if (!ensureCsrfAndAuth()) return;
        webServer.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Rebooting device...\"}");
        networkMgr.requestReboot();
    });

    // 12. POST /api/update -> Multipart OTA Firmware Upload
    registerOtaUpdateRoute(webServer, deviceConfig, networkMgr, recordApTrafficIfLocal);

    // Captive Portal Catch-All (AP interface only) & 404 Handler (STA interface)
    webServer.onNotFound([]() {
        if (networkMgr.isApClientIp((uint32_t)webServer.client().localIP())) {
            networkMgr.notifyApHttpActivity();
            webServer.sendHeader("Location", String("http://") + SETUP_AP_IP_STR + "/", true);
            webServer.send(302, "text/plain", "");
        } else {
            webServer.send(404, "text/plain", "404 Not Found");
        }
    });
}

// =====================================================================
// NVS Storage Persistence
// =====================================================================
void loadConfiguration() {
    loadConfigFromKv(kvStore, deviceConfig, ESP.getEfuseMac());
    currentMode = deviceConfig.armed ? MODE_ARMED : MODE_DISARMED;

    Serial.println("[NVS] Configuration loaded from flash.");
    Serial.printf("[NVS] Device: %s, AP: %s (win %us), Armed: %s, Debounce: %u ms, Cooldown: %u ms, Snooze: %um, AlertMode: %s, Cloud: %s\n",
                  deviceConfig.device_name,
                  deviceConfig.ap_ssid,
                  deviceConfig.setup_window_s,
                  deviceConfig.armed ? "YES" : "NO",
                  deviceConfig.debounce_ms,
                  deviceConfig.restore_cooldown_ms,
                  deviceConfig.snooze_default_min,
                  (deviceConfig.alert_mode == ALERT_OUTAGE_ONLY) ? "OUTAGE_ONLY" : "OUTAGE_AND_RESTORE",
                  deviceConfig.cloud_enabled ? "ON" : "OFF");
}

bool saveConfiguration() {
    if (!saveConfigDiffToKv(kvStore, deviceConfig)) {
        Serial.println("[NVS] Error: failed to save configuration to flash.");
        return false;
    }
    Serial.println("[NVS] Configuration saved to flash.");
    return true;
}
