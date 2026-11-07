#include "setup_ap.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>

SetupApController::SetupApController(IWifiHal& wifi, const IClock& clock, WifiConnector& connector)
    : _wifi(wifi),
      _clock(clock),
      _connector(connector),
      _actions(nullptr),
      _efuseMac(0),
      _apFirstOpenedAtMs(0),
      _apOpenedAtMs(0),
      _apWindowDurationMs(0),
      _apLastActivityMs(0),
      _apHadClient(false),
      _apModeActive(false),
      _apStartPending(false),
      _apStartWindowS(0),
      _apNextRetryMs(0),
      _webScanPending(false),
      _webScanStartMs(0),
      _webScanResultMs(0),
      _hasWebScanResult(false),
      _deferredConnectorNetworks(false),
      _deferredConnectorRestart(false),
      _deferredForceReconnect(false),
      _fallbackApTriggered(false),
      _fallbackCooldownActive(false),
      _fallbackClosedAtMs(0),
      _pendingConfigApply(false),
      _pendingTriggerReconnect(false),
      _pendingOpenAp(false),
      _pendingOpenApWindowS(0),
      _pendingCloseAp(false),
      _pendingReboot(false),
      _pendingFactoryReset(false),
      _pendingActionReqMs(0) {
    memset(&_config, 0, sizeof(_config));
    memset(&_pendingConfig, 0, sizeof(_pendingConfig));
    memset(_effectiveApSsid, 0, sizeof(_effectiveApSsid));
    strncpy(_webScanResult, "{\"networks\":[]}", sizeof(_webScanResult) - 1);
    _webScanResult[sizeof(_webScanResult) - 1] = '\0';
}

void SetupApController::init(const DeviceConfig& config, uint64_t efuseMac, ISetupSystemActions* actions) {
    _config = config;
    _efuseMac = efuseMac;
    _actions = actions;
    formatDefaultApSsid(_efuseMac, _effectiveApSsid, sizeof(_effectiveApSsid));
    _apFirstOpenedAtMs = 0;
    _apModeActive = false;
    _apStartPending = false;
    _apStartWindowS = 0;
    _apNextRetryMs = 0;
    _apWindowDurationMs = 0;
    _apHadClient = false;
    _webScanPending = false;
    _hasWebScanResult = false;
    _deferredConnectorNetworks = false;
    _deferredConnectorRestart = false;
    _deferredForceReconnect = false;
    _fallbackApTriggered = false;
    _fallbackCooldownActive = false;
    _fallbackClosedAtMs = 0;
    _pendingConfigApply = false;
    _pendingTriggerReconnect = false;
    _pendingOpenAp = false;
    _pendingCloseAp = false;
    _pendingReboot = false;
    _pendingFactoryReset = false;

    if (!hasSavedNetworks()) {
        openSetupAp(0); // permanent until a network is saved
    } else if (_config.setup_window_s > 0) {
        openSetupAp(_config.setup_window_s); // Boot setup window runs alongside STA
    } else {
        applyWifiMode(); // WIFI_STA only
    }
}

bool SetupApController::hasSavedNetworks() const {
    for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
        if (_config.wifi[i].ssid[0] != '\0') return true;
    }
    return false;
}

void SetupApController::applyWifiMode() {
    if (_apModeActive) {
        _wifi.setMode(hasSavedNetworks() ? WIFI_AP_STA : WIFI_AP);
    } else {
        _wifi.setMode(WIFI_STA);
    }
}

bool SetupApController::startSoftApRadio(bool isFreshOpen, uint32_t now, uint32_t windowSeconds) {
    if (strlen(_config.ap_ssid) > 0) {
        strncpy(_effectiveApSsid, _config.ap_ssid, sizeof(_effectiveApSsid) - 1);
        _effectiveApSsid[sizeof(_effectiveApSsid) - 1] = '\0';
    } else {
        formatDefaultApSsid(_efuseMac, _effectiveApSsid, sizeof(_effectiveApSsid));
    }
    const char* apPass = (strlen(_config.ap_pass) >= MIN_PASS_LEN) ? _config.ap_pass : DEFAULT_AP_PASS;

    int prevMode = _wifi.getMode();
    _apModeActive = true;
    applyWifiMode();

    // Match the active STA channel so SoftAP and STA do not fight over radio channels
    int channel = (_wifi.status() == WL_CONNECTED) ? _wifi.channel() : 1;
    if (channel < 1 || channel > 13) channel = 1;

    if (!_wifi.softApStart(_effectiveApSsid, apPass, channel)) {
        _apModeActive = false;
        _wifi.setMode(prevMode);
        _apStartPending = true;
        _apStartWindowS = hasSavedNetworks() ? windowSeconds : 0;
        _apNextRetryMs = now + SETUP_AP_RETRY_MS;
        HAL_LOG_PRINTF("[WiFi] Setup AP failed to start (%s); retrying in %u ms.\n",
                       _effectiveApSsid, (unsigned)SETUP_AP_RETRY_MS);
        return false;
    }

    _apStartPending = false;
    if (isFreshOpen) {
        _apFirstOpenedAtMs = now;
    }
    _wifi.dnsStart();
    return true;
}

void SetupApController::openSetupAp(uint32_t windowSeconds) {
    bool wasActive = _apModeActive;
    uint32_t now = _clock.millis();
    _apOpenedAtMs = now;
    _apWindowDurationMs = hasSavedNetworks() ? (windowSeconds * 1000UL) : 0;
    _apLastActivityMs = now;
    _apHadClient = wasActive && (_wifi.softApStationNum() > 0);

    // Reconfigure SoftAP only when starting from inactive so connected clients are not dropped
    if (!wasActive) {
        if (!startSoftApRadio(true, now, windowSeconds)) {
            return;
        }
    }

    HAL_LOG_PRINTF("[WiFi] Setup AP %s: %s (IP: %s, window: %us)\n",
                   wasActive ? "window refreshed" : "started",
                   _effectiveApSsid, SETUP_AP_IP_STR, (unsigned)(_apWindowDurationMs / 1000UL));
}

bool SetupApController::closeSetupAp() {
    if (!_apModeActive) {
        if (_apStartPending && !hasSavedNetworks()) {
            return false;
        }
        if (_apStartPending) {
            _apStartPending = false;
            applyWifiMode();
        }
        return true;
    }
    if (!hasSavedNetworks()) {
        HAL_LOG_PRINT("[WiFi] Cannot close setup AP: no saved networks.");
        return false;
    }
    _wifi.dnsStop();
    _wifi.softApStop();
    _apModeActive = false;
    _apStartPending = false;
    _apWindowDurationMs = 0;
    _apHadClient = false;
    applyWifiMode(); // switches to WIFI_STA without disturbing STA connection

    // Preserve connector's disconnected timer; start fallback cooldown if STA is offline
    if (_wifi.status() != WL_CONNECTED) {
        _fallbackApTriggered = true;
        _fallbackCooldownActive = true;
        _fallbackClosedAtMs = _clock.millis();
    }
    HAL_LOG_PRINT("[WiFi] Setup AP closed; STA mode active.");
    return true;
}

void SetupApController::notifyApHttpActivity() {
    if (_apModeActive) {
        _apLastActivityMs = _clock.millis();
    }
}

void SetupApController::reconnectWiFi() {
    if (!hasSavedNetworks()) {
        openSetupAp(0);
        return;
    }
    if (_webScanPending) {
        _deferredForceReconnect = true;
        return;
    }
    _connector.forceReconnect();
}

void SetupApController::requestApplyConfig(const DeviceConfig& config, bool triggerReconnect) {
    _pendingConfig = config;
    _pendingTriggerReconnect = _pendingTriggerReconnect || triggerReconnect;
    _pendingConfigApply = true;
    _pendingActionReqMs = _clock.millis();
}

void SetupApController::requestOpenSetupAp(uint32_t windowSeconds) {
    _pendingCloseAp = false;
    _pendingOpenAp = true;
    _pendingOpenApWindowS = windowSeconds;
    _pendingActionReqMs = _clock.millis();
}

void SetupApController::requestCloseSetupAp() {
    _pendingOpenAp = false;
    _pendingCloseAp = true;
    _pendingActionReqMs = _clock.millis();
}

void SetupApController::requestReboot() {
    _pendingReboot = true;
    _pendingActionReqMs = _clock.millis();
}

void SetupApController::requestFactoryReset() {
    _pendingFactoryReset = true;
    _pendingActionReqMs = _clock.millis();
}

void SetupApController::processDeferredActions() {
    uint32_t now = _clock.millis();
    if (now - _pendingActionReqMs < DEFERRED_APPLY_DELAY_MS) return;

    if (_pendingFactoryReset) {
        _pendingFactoryReset = false;
        if (_actions) _actions->onFactoryReset();
        return;
    }

    if (_pendingReboot) {
        _pendingReboot = false;
        if (_actions) _actions->onReboot();
        return;
    }

    if (_pendingOpenAp) {
        _pendingOpenAp = false;
        uint32_t winS = _pendingOpenApWindowS;
        openSetupAp(winS);
    }

    if (_pendingCloseAp) {
        _pendingCloseAp = false;
        closeSetupAp();
    }

    if (_pendingConfigApply) {
        _pendingConfigApply = false;
        DeviceConfig cfgCopy = _pendingConfig;
        bool trig = _pendingTriggerReconnect;
        _pendingTriggerReconnect = false;
        if (_actions) {
            _actions->onApplyConfig(cfgCopy, trig);
        } else {
            onConfigApplied(cfgCopy, trig);
        }
    }
}

void SetupApController::onConfigApplied(const DeviceConfig& config, bool triggerReconnect) {
    bool hadSavedBefore = hasSavedNetworks();
    bool apCredsChanged = (strcmp(_config.ap_ssid, config.ap_ssid) != 0) ||
                          (strcmp(_config.ap_pass, config.ap_pass) != 0);
    _config = config;

    if (_webScanPending) {
        _deferredConnectorNetworks = true;
        _deferredConnectorRestart = _deferredConnectorRestart || triggerReconnect;
    } else {
        _connector.setNetworks(_config.wifi, _config.device_name, triggerReconnect);
    }
    bool hasSavedNow = hasSavedNetworks();

    if (!hasSavedNow) {
        if (!_apModeActive) {
            openSetupAp(0);
        } else {
            _apWindowDurationMs = 0;
            if (apCredsChanged) {
                _wifi.dnsStop();
                _wifi.softApStop();
                startSoftApRadio(false, _clock.millis(), 0);
            } else if (!_webScanPending) {
                applyWifiMode();
            }
        }
    } else if (_apModeActive) {
        uint32_t now = _clock.millis();
        // When the first network is saved in permanent AP mode, switch to a timed window so AP closes automatically
        if (!hadSavedBefore && _apWindowDurationMs == 0) {
            _apFirstOpenedAtMs = now;
            _apOpenedAtMs = now;
            _apLastActivityMs = now;
            _apWindowDurationMs = (uint32_t)SETUP_FALLBACK_WINDOW_S * 1000UL;
            _apHadClient = (_wifi.softApStationNum() > 0);
            HAL_LOG_PRINTF("[WiFi] First network saved; setup AP converted to %us timed window.\n",
                           (unsigned)SETUP_FALLBACK_WINDOW_S);
        }
        if (apCredsChanged) {
            _wifi.dnsStop();
            _wifi.softApStop();
            uint32_t remWindowS = (_apWindowDurationMs + 999UL) / 1000UL;
            startSoftApRadio(false, now, remWindowS);
        } else if (!_webScanPending) {
            applyWifiMode();
        }
    }
}

size_t SetupApController::buildWebScanJson(int16_t scanCount, char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return 0;
    if (scanCount <= 0) {
        int n = snprintf(outBuf, outCap, "{\"networks\":[]}");
        if (n < 0 || (size_t)n >= outCap) {
            outBuf[0] = '\0';
            return 0;
        }
        return (size_t)n;
    }

    struct NetEntry {
        char ssid[33];
        int32_t rssi;
        bool secure;
        int32_t channel;
    };
    NetEntry list[20];
    uint8_t count = 0;

    for (int16_t i = 0; i < scanCount; ++i) {
        char ssid[33];
        _wifi.scanSsid(i, ssid, sizeof(ssid));
        if (ssid[0] == '\0') continue;
        int32_t rssi = _wifi.scanRssi(i);
        bool sec = !_wifi.scanIsOpen(i);
        int32_t ch = _wifi.scanChannel(i);

        int foundIdx = -1;
        for (uint8_t k = 0; k < count; ++k) {
            if (strcmp(list[k].ssid, ssid) == 0) {
                foundIdx = k;
                break;
            }
        }
        if (foundIdx >= 0) {
            if (rssi > list[foundIdx].rssi) {
                list[foundIdx].rssi = rssi;
                list[foundIdx].secure = sec;
                list[foundIdx].channel = ch;
            }
        } else if (count < 20) {
            strncpy(list[count].ssid, ssid, 32);
            list[count].ssid[32] = '\0';
            list[count].rssi = rssi;
            list[count].secure = sec;
            list[count].channel = ch;
            count++;
        } else {
            uint8_t minIdx = 0;
            for (uint8_t k = 1; k < count; ++k) {
                if (list[k].rssi < list[minIdx].rssi) minIdx = k;
            }
            if (rssi > list[minIdx].rssi) {
                strncpy(list[minIdx].ssid, ssid, 32);
                list[minIdx].ssid[32] = '\0';
                list[minIdx].rssi = rssi;
                list[minIdx].secure = sec;
                list[minIdx].channel = ch;
            }
        }
    }

    for (uint8_t i = 0; i < count; ++i) {
        for (uint8_t j = i + 1; j < count; ++j) {
            if (list[j].rssi > list[i].rssi) {
                NetEntry tmp = list[i];
                list[i] = list[j];
                list[j] = tmp;
            }
        }
    }

    uint8_t emitCount = count;
    while (emitCount > 0) {
        JsonDocument doc;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (uint8_t i = 0; i < emitCount; ++i) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = (const char*)list[i].ssid;
            o["rssi"] = list[i].rssi;
            o["secure"] = list[i].secure;
            o["channel"] = list[i].channel;
        }
        if (measureJson(doc) < outCap) {
            return serializeJson(doc, outBuf, outCap);
        }
        emitCount--;
    }

    int n = snprintf(outBuf, outCap, "{\"networks\":[]}");
    if (n < 0 || (size_t)n >= outCap) {
        outBuf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

void SetupApController::flushDeferredConnectorActions() {
    if (_deferredConnectorNetworks) {
        _deferredConnectorNetworks = false;
        bool restart = _deferredConnectorRestart;
        _deferredConnectorRestart = false;
        _connector.setNetworks(_config.wifi, _config.device_name, restart);
    }
    if (_deferredForceReconnect) {
        _deferredForceReconnect = false;
        _connector.forceReconnect();
    }
}

void SetupApController::processWebScan() {
    if (!_webScanPending) return;

    uint32_t now = _clock.millis();
    int16_t n = _wifi.scanComplete();

    if (n == WIFI_SCAN_RUNNING) {
        // Force-clear a hung web scan after WIFI_SCAN_TIMEOUT_MS so connector actions resume
        if (now - _webScanStartMs >= WIFI_SCAN_TIMEOUT_MS) {
            HAL_LOG_PRINTF("[WiFi] Web scan timed out (%us); stopping scan.\n",
                           (unsigned)(WIFI_SCAN_TIMEOUT_MS / 1000UL));
            _wifi.scanStop();
            _wifi.scanDelete();
            strncpy(_webScanResult, "{\"networks\":[]}", sizeof(_webScanResult) - 1);
            _webScanResult[sizeof(_webScanResult) - 1] = '\0';
            _webScanResultMs = now;
            _hasWebScanResult = true;
            _webScanPending = false;
            if (!hasSavedNetworks() && _apModeActive) {
                applyWifiMode();
            }
            flushDeferredConnectorActions();
        }
        return;
    }

    buildWebScanJson(n, _webScanResult, sizeof(_webScanResult));
    _wifi.scanDelete();
    _webScanResultMs = now;
    _hasWebScanResult = true;
    _webScanPending = false;
    if (!hasSavedNetworks() && _apModeActive) {
        applyWifiMode();
    }
    flushDeferredConnectorActions();
}

const char* SetupApController::pollWifiScanJsonPtr() {
    if (_webScanPending) {
        return "{\"status\":\"scanning\"}";
    }

    // Serve cached scan result while within WEB_SCAN_CACHE_TTL_MS
    if (_hasWebScanResult && (_clock.millis() - _webScanResultMs < WEB_SCAN_CACHE_TTL_MS)) {
        return _webScanResult;
    }

    if (_connector.isScanning() || _wifi.scanComplete() == WIFI_SCAN_RUNNING) {
        return "{\"status\":\"scanning\"}";
    }

    if (_wifi.getMode() == WIFI_AP) {
        _wifi.setMode(WIFI_AP_STA);
    }
    _wifi.scanDelete();
    int16_t res = _wifi.scanStart(true, false);
    if (res == WIFI_SCAN_FAILED) {
        if (!hasSavedNetworks() && _apModeActive) {
            applyWifiMode();
        }
        return "{\"networks\":[]}";
    }
    _webScanPending = true;
    _webScanStartMs = _clock.millis();
    _hasWebScanResult = false;
    return "{\"status\":\"scanning\"}";
}

// _config changes only in init() and onConfigApplied(), so the deferred apply compares the
// saved networks and AP credentials against the state before the save.
void SetupApController::update() {

    processDeferredActions();
    processWebScan();

    if (!_apModeActive && _apStartPending) {
        uint32_t now = _clock.millis();
        if ((int32_t)(now - _apNextRetryMs) >= 0) {
            uint32_t retryWin = hasSavedNetworks() ? _apStartWindowS : 0;
            openSetupAp(retryWin);
        }
    }

    bool apHasClients = false;
    if (_apModeActive) {
        _wifi.dnsProcess();
        uint8_t clients = _wifi.softApStationNum();
        apHasClients = (clients > 0);
        if (apHasClients) {
            _apHadClient = true;
            _apLastActivityMs = _clock.millis();
        }

        // Window expiration check when saved networks exist and window is finite
        if (hasSavedNetworks() && _apWindowDurationMs > 0) {
            uint32_t now = _clock.millis();
            bool hardCapHit = (now - _apFirstOpenedAtMs >= (uint32_t)SETUP_HARD_CAP_S * 1000UL);
            bool expired = false;
            if (hardCapHit) {
                expired = true;
                HAL_LOG_PRINTF("[WiFi] Setup AP %u-min hard cap reached.\n",
                               (unsigned)(SETUP_HARD_CAP_S / 60U));
            } else if (!apHasClients) {
                if (_apHadClient) {
                    if (now - _apLastActivityMs >= (uint32_t)SETUP_IDLE_TIMEOUT_S * 1000UL) {
                        expired = true;
                        HAL_LOG_PRINT("[WiFi] Setup AP idle timeout after last client left.");
                    }
                } else if (now - _apOpenedAtMs >= _apWindowDurationMs) {
                    expired = true;
                    HAL_LOG_PRINT("[WiFi] Setup AP window elapsed with no client.");
                }
            }
            if (expired) {
                closeSetupAp();
                apHasClients = false;
            }
        }
    }

    _connector.update(apHasClients, _webScanPending);

    if (_wifi.status() == WL_CONNECTED) {
        _fallbackApTriggered = false;
        _fallbackCooldownActive = false;
    } else {
        // Open a fallback setup AP after prolonged STA loss; re-arm only after close + cooldown
        if (hasSavedNetworks() && !_apModeActive) {
            if (!_fallbackApTriggered) {
                if (_connector.getDisconnectedDurationMs() >= NO_STA_AP_FALLBACK_MS) {
                    HAL_LOG_PRINTF("[WiFi] Disconnected for %u minutes -> opening setup AP window.\n",
                                   (unsigned)(NO_STA_AP_FALLBACK_MS / 60000UL));
                    _fallbackApTriggered = true;
                    _fallbackCooldownActive = false;
                    openSetupAp(SETUP_FALLBACK_WINDOW_S);
                }
            } else if (_fallbackCooldownActive && (_clock.millis() - _fallbackClosedAtMs >= NO_STA_AP_FALLBACK_MS)) {
                HAL_LOG_PRINTF("[WiFi] Still disconnected %u minutes after fallback AP closed -> reopening setup AP.\n",
                               (unsigned)(NO_STA_AP_FALLBACK_MS / 60000UL));
                _fallbackCooldownActive = false;
                openSetupAp(SETUP_FALLBACK_WINDOW_S);
            }
        }
    }
}

uint8_t SetupApController::getApClientCount() const {
    return _apModeActive ? _wifi.softApStationNum() : 0;
}

uint32_t SetupApController::getApClosesInSeconds() const {
    // Return 0 only when AP is inactive or permanent (no saved networks)
    if (!_apModeActive || !hasSavedNetworks() || _apWindowDurationMs == 0) {
        return 0;
    }

    uint32_t now = _clock.millis();
    uint32_t hardCapDeadline = _apFirstOpenedAtMs + (uint32_t)SETUP_HARD_CAP_S * 1000UL;
    int32_t hardRemMs = (int32_t)(hardCapDeadline - now);
    if (hardRemMs <= 0) {
        return 0;
    }

    if (_wifi.softApStationNum() > 0) {
        return (uint32_t)(hardRemMs + 999L) / 1000UL;
    }

    uint32_t idleDeadline = _apHadClient
        ? (_apLastActivityMs + (uint32_t)SETUP_IDLE_TIMEOUT_S * 1000UL)
        : (_apOpenedAtMs + _apWindowDurationMs);
    int32_t idleRemMs = (int32_t)(idleDeadline - now);
    int32_t minRemMs = (idleRemMs < hardRemMs) ? idleRemMs : hardRemMs;
    if (minRemMs <= 0) {
        return 0;
    }
    return (uint32_t)(minRemMs + 999L) / 1000UL;
}

void SetupApController::getApIpAddress(char* buf, size_t len) const {
    if (!buf || len == 0) return;
    if (_apModeActive) {
        _wifi.softApIpStr(buf, len);
        return;
    }
    buf[0] = '\0';
}
