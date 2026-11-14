#include "wifi_connector.h"
#include <string.h>
#include <stdio.h>

WifiConnector::WifiConnector(IWifiHal& wifi, const IClock& clock)
    : _wifi(wifi),
      _clock(clock),
      _savedCount(0),
      _lastGoodSlot(WIFI_SLOT_NONE),
      _persistedLastGoodSlot(WIFI_SLOT_NONE),
      _lastGoodDirty(false),
      _state(WIFI_CONN_IDLE),
      _connectedSlot(-1),
      _tryingSlot(-1),
      _fastPathFailedSlot(-1),
      _attemptStartMs(0),
      _scanStartMs(0),
      _candidateCount(0),
      _candidateIdx(0),
      _backoffIdx(0),
      _backoffStartMs(0),
      _backoffDurationMs(WIFI_BACKOFF_MS[0]),
      _disconnectedTimerRunning(false),
      _disconnectedSinceMs(0),
      _pendingForceReconnect(false),
      _configAttemptPending(false),
      _wakeFromBackoff(false) {
    memset(_slots, 0, sizeof(_slots));
    memset(_hostname, 0, sizeof(_hostname));
    memset(_candidates, 0, sizeof(_candidates));
    memset(_candidateVisible, 0, sizeof(_candidateVisible));
    memset(_candidateRssi, 0, sizeof(_candidateRssi));
}

void WifiConnector::init(const WifiCred slots[WIFI_SLOTS], uint8_t lastGoodSlot, const char* hostname) {
    memcpy(_slots, slots, sizeof(_slots));
    strncpy(_hostname, hostname ? hostname : DEFAULT_DEVICE_NAME, sizeof(_hostname) - 1);
    _hostname[sizeof(_hostname) - 1] = '\0';
    _lastGoodSlot = (lastGoodSlot < WIFI_SLOTS) ? lastGoodSlot : WIFI_SLOT_NONE;
    _persistedLastGoodSlot = _lastGoodSlot;
    _lastGoodDirty = false;
    _connectedSlot = -1;
    _tryingSlot = -1;
    _fastPathFailedSlot = -1;
    _backoffIdx = 0;
    _configAttemptPending = false;

    _wifi.setAutoReconnect(false);
    countSaved();

    if (_savedCount == 0) {
        _disconnectedTimerRunning = false;
        _state = WIFI_CONN_IDLE;
        HAL_LOG_PRINT("[WiFi] No saved networks configured.");
        return;
    }

    _disconnectedTimerRunning = true;
    _disconnectedSinceMs = _clock.millis();
    startCycle(true);
}

void WifiConnector::setNetworks(const WifiCred slots[WIFI_SLOTS], const char* hostname, bool restartNow) {
    // Keep the active STA link untouched when the connected slot's credentials did not change
    bool keepConnected = false;
    if (_wifi.status() == WL_CONNECTED && _connectedSlot >= 0 && _connectedSlot < WIFI_SLOTS) {
        uint8_t s = (uint8_t)_connectedSlot;
        if (slots[s].ssid[0] != '\0' &&
            strcmp(_slots[s].ssid, slots[s].ssid) == 0 &&
            strcmp(_slots[s].pass, slots[s].pass) == 0) {
            keepConnected = true;
        }
    }

    memcpy(_slots, slots, sizeof(_slots));
    strncpy(_hostname, hostname ? hostname : DEFAULT_DEVICE_NAME, sizeof(_hostname) - 1);
    _hostname[sizeof(_hostname) - 1] = '\0';
    countSaved();

    if (_lastGoodSlot < WIFI_SLOTS && _slots[_lastGoodSlot].ssid[0] == '\0') {
        _lastGoodSlot = WIFI_SLOT_NONE;
    }

    if (_savedCount == 0) {
        if (_wifi.status() == WL_CONNECTED) {
            _wifi.disconnect(false);
        }
        if (_state == WIFI_CONN_SCANNING) {
            _wifi.scanStop();
            _wifi.scanDelete();
        }
        _connectedSlot = -1;
        _tryingSlot = -1;
        _fastPathFailedSlot = -1;
        _disconnectedTimerRunning = false;
        _pendingForceReconnect = false;
        _configAttemptPending = false;
        _state = WIFI_CONN_IDLE;
        return;
    }

    if (keepConnected) {
        // Connected slot unchanged: keep active STA link and _connectedSlot
        _configAttemptPending = false;
        _state = WIFI_CONN_CONNECTED;
        _disconnectedTimerRunning = false;
        return;
    }

    if (restartNow) {
        _pendingForceReconnect = false;
        if (_wifi.status() == WL_CONNECTED) {
            _wifi.disconnect(false);
        }
        if (_state == WIFI_CONN_SCANNING) {
            _wifi.scanStop();
            _wifi.scanDelete();
        }
        _connectedSlot = -1;
        _tryingSlot = -1;
        _fastPathFailedSlot = -1;
        _backoffIdx = 0;
        _disconnectedTimerRunning = true;
        _disconnectedSinceMs = _clock.millis();
        _configAttemptPending = true;
        startCycle(true);
    }
}

void WifiConnector::forceReconnect() {
    if (_savedCount == 0) return;
    _wakeFromBackoff.store(false);
    if (_wifi.softApStationNum() > 0) {
        _pendingForceReconnect = true;
        HAL_LOG_PRINT("[WiFi] Reconnect deferred while AP client is connected.");
        return;
    }
    _pendingForceReconnect = false;
    _configAttemptPending = false;
    if (_wifi.status() == WL_CONNECTED) {
        _wifi.disconnect(false);
    }
    if (_state == WIFI_CONN_SCANNING) {
        _wifi.scanStop();
        _wifi.scanDelete();
    }
    _connectedSlot = -1;
    _tryingSlot = -1;
    _fastPathFailedSlot = -1;
    _backoffIdx = 0;
    if (!_disconnectedTimerRunning) {
        _disconnectedTimerRunning = true;
        _disconnectedSinceMs = _clock.millis();
    }
    startCycle(true);
}

void WifiConnector::wakeFromBackoff() {
    if (_savedCount == 0) return;
    if (_state == WIFI_CONN_BACKOFF) {
        _wakeFromBackoff.store(true);
    }
}

void WifiConnector::countSaved() {
    _savedCount = 0;
    for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
        if (_slots[i].ssid[0] != '\0') {
            _savedCount++;
        }
    }
}

void WifiConnector::startCycle(bool allowFastPath) {
    _wakeFromBackoff.store(false);
    if (_savedCount == 0) {
        _configAttemptPending = false;
        _state = WIFI_CONN_IDLE;
        return;
    }
    if (_wifi.softApStationNum() > 0 && !_configAttemptPending) {
        _state = WIFI_CONN_PAUSED_AP_CLIENT;
        return;
    }
    _fastPathFailedSlot = -1;
    if (allowFastPath && _lastGoodSlot < WIFI_SLOTS && _slots[_lastGoodSlot].ssid[0] != '\0') {
        _state = WIFI_CONN_FAST_PATH;
        trySlot(_lastGoodSlot, "fast-path");
        return;
    }
    startScan();
}

void WifiConnector::startScan() {
    if (_wifi.softApStationNum() > 0 && !_configAttemptPending) {
        _state = WIFI_CONN_PAUSED_AP_CLIENT;
        return;
    }
    _wifi.scanDelete();
    _wifi.disconnect(false);
    int16_t res = _wifi.scanStart(true, false);
    if (res == WIFI_SCAN_FAILED) {
        HAL_LOG_PRINT("[WiFi] Async scan failed to start; trying remaining slots in order.");
        buildCandidatesFromScan(-2, _fastPathFailedSlot);
        if (_candidateCount > 0) {
            _candidateIdx = 0;
            tryCandidate(0);
        } else {
            enterBackoff();
        }
        return;
    }
    _scanStartMs = _clock.millis();
    _state = WIFI_CONN_SCANNING;
    HAL_LOG_PRINT("[WiFi] Async scan started...");
}

void WifiConnector::trySlot(uint8_t slot, const char* reason) {
    if (slot >= WIFI_SLOTS || _slots[slot].ssid[0] == '\0') return;
    _tryingSlot = (int8_t)slot;
    _attemptStartMs = _clock.millis();
    _wifi.setHostname(_hostname);
    const char* pass = (strlen(_slots[slot].pass) > 0) ? _slots[slot].pass : nullptr;
    (void)reason;
    HAL_LOG_PRINTF("[WiFi] Trying slot %u (%s) [%s]...\n", slot, _slots[slot].ssid, reason);
    _wifi.begin(_slots[slot].ssid, pass);
}

void WifiConnector::tryCandidate(uint8_t idx) {
    if (idx >= _candidateCount) {
        enterBackoff();
        return;
    }
    uint8_t slot = _candidates[idx];
    char reason[32];
    if (_candidateVisible[idx]) {
        snprintf(reason, sizeof(reason), "RSSI %d dBm", _candidateRssi[idx]);
    } else {
        snprintf(reason, sizeof(reason), "hidden/unseen");
    }
    _state = WIFI_CONN_TRYING_CANDIDATE;
    trySlot(slot, reason);
}

void WifiConnector::enterBackoff() {
    _configAttemptPending = false;
    _wakeFromBackoff.store(false);
    _backoffDurationMs = WIFI_BACKOFF_MS[_backoffIdx];
    if (_backoffIdx + 1 < WIFI_BACKOFF_COUNT) {
        _backoffIdx++;
    }
    _backoffStartMs = _clock.millis();
    if (_wifi.softApStationNum() > 0) {
        _state = WIFI_CONN_PAUSED_AP_CLIENT;
        HAL_LOG_PRINT("[WiFi] Cycle failed; pausing background retries while AP client is associated.");
        return;
    }
    _state = WIFI_CONN_BACKOFF;
    HAL_LOG_PRINTF("[WiFi] Cycle failed; backing off %u s\n",
                   (unsigned)(_backoffDurationMs / 1000UL));
}

void WifiConnector::buildCandidatesFromScan(int16_t scanCount, int8_t skipSlot) {
    bool found[WIFI_SLOTS] = { false };
    int16_t bestRssi[WIFI_SLOTS];
    for (uint8_t s = 0; s < WIFI_SLOTS; ++s) {
        bestRssi[s] = -1000;
    }

    if (scanCount > 0) {
        char apSsid[33];
        for (int16_t i = 0; i < scanCount; ++i) {
            _wifi.scanSsid(i, apSsid, sizeof(apSsid));
            int32_t rssi = _wifi.scanRssi(i);
            for (uint8_t s = 0; s < WIFI_SLOTS; ++s) {
                if ((int8_t)s == skipSlot) continue;
                if (_slots[s].ssid[0] != '\0' && strcmp(apSsid, _slots[s].ssid) == 0) {
                    found[s] = true;
                    if (rssi > bestRssi[s]) {
                        bestRssi[s] = (int16_t)rssi;
                    }
                }
            }
        }
    }

    uint8_t visSlots[WIFI_SLOTS];
    uint8_t visCount = 0;
    for (uint8_t s = 0; s < WIFI_SLOTS; ++s) {
        if ((int8_t)s == skipSlot) continue;
        if (_slots[s].ssid[0] != '\0' && found[s]) {
            visSlots[visCount++] = s;
        }
    }

    for (uint8_t i = 0; i < visCount; ++i) {
        for (uint8_t j = i + 1; j < visCount; ++j) {
            uint8_t si = visSlots[i];
            uint8_t sj = visSlots[j];
            if (bestRssi[sj] > bestRssi[si] || (bestRssi[sj] == bestRssi[si] && sj < si)) {
                visSlots[i] = sj;
                visSlots[j] = si;
            }
        }
    }

    _candidateCount = 0;
    for (uint8_t i = 0; i < visCount; ++i) {
        uint8_t s = visSlots[i];
        _candidates[_candidateCount] = s;
        _candidateVisible[_candidateCount] = true;
        _candidateRssi[_candidateCount] = bestRssi[s];
        _candidateCount++;
    }

    for (uint8_t s = 0; s < WIFI_SLOTS; ++s) {
        if ((int8_t)s == skipSlot) continue;
        if (_slots[s].ssid[0] != '\0' && !found[s]) {
            _candidates[_candidateCount] = s;
            _candidateVisible[_candidateCount] = false;
            _candidateRssi[_candidateCount] = -100;
            _candidateCount++;
        }
    }
}

void WifiConnector::update(bool apHasClients, bool webScanActive) {
    int st = _wifi.status();

    // Update link state before web-scan or AP-client guards so connects and drops are never missed
    if (st == WL_CONNECTED) {
        _configAttemptPending = false;
        if (_state != WIFI_CONN_CONNECTED) {
            _state = WIFI_CONN_CONNECTED;
            if (_tryingSlot >= 0 && _tryingSlot < WIFI_SLOTS) {
                _connectedSlot = _tryingSlot;
            } else {
                char activeSsid[MAX_WIFI_SSID_LEN + 1];
                activeSsid[0] = '\0';
                _wifi.connectedSsid(activeSsid, sizeof(activeSsid));
                _connectedSlot = -1;
                if (activeSsid[0] != '\0') {
                    for (uint8_t i = 0; i < WIFI_SLOTS; ++i) {
                        if (_slots[i].ssid[0] != '\0' && strcmp(_slots[i].ssid, activeSsid) == 0) {
                            _connectedSlot = (int8_t)i;
                            break;
                        }
                    }
                }
            }
            _tryingSlot = -1;
            _fastPathFailedSlot = -1;
            _backoffIdx = 0;
            _disconnectedTimerRunning = false;
            if (_connectedSlot >= 0 && (uint8_t)_connectedSlot != _persistedLastGoodSlot) {
                _lastGoodSlot = (uint8_t)_connectedSlot;
                _lastGoodDirty = true;
            }
            char ipBuf[32];
            _wifi.localIpStr(ipBuf, sizeof(ipBuf));
            HAL_LOG_PRINTF("[WiFi] Connected to slot %d (%s), IP: %s, RSSI: %d dBm\n",
                           _connectedSlot,
                           (_connectedSlot >= 0) ? _slots[_connectedSlot].ssid : "(unknown)",
                           ipBuf,
                           _wifi.rssi());
        }
        if (_pendingForceReconnect && !apHasClients && !webScanActive) {
            forceReconnect();
        }
        return;
    }

    // Handle drop from connected state even if webScanActive or apHasClients
    if (_state == WIFI_CONN_CONNECTED) {
        int8_t lostSlot = _connectedSlot;
        HAL_LOG_PRINTF("[WiFi] Disconnected from slot %d (%s); restarting cycle\n",
                       lostSlot, (lostSlot >= 0 && lostSlot < WIFI_SLOTS) ? _slots[lostSlot].ssid : "");
        if (lostSlot >= 0 && lostSlot < WIFI_SLOTS) {
            _lastGoodSlot = (uint8_t)lostSlot;
        }
        _connectedSlot = -1;
        _tryingSlot = -1;
        _fastPathFailedSlot = -1;
        _backoffIdx = 0;
        _disconnectedTimerRunning = true;
        _disconnectedSinceMs = _clock.millis();

        if (_savedCount == 0) {
            _state = WIFI_CONN_IDLE;
            return;
        }
        if (apHasClients) {
            _wifi.disconnect(false);
            _state = WIFI_CONN_PAUSED_AP_CLIENT;
            return;
        }
        if (webScanActive) {
            _state = WIFI_CONN_BACKOFF;
            _backoffStartMs = _clock.millis();
            _backoffDurationMs = 0; // resume immediately once web scan finishes
            return;
        }
        startCycle(true);
        return;
    }

    if (_savedCount == 0) {
        _disconnectedTimerRunning = false;
        _state = WIFI_CONN_IDLE;
        return;
    }

    // Keep the disconnected timer running across retries while saved networks exist
    if (!_disconnectedTimerRunning) {
        _disconnectedTimerRunning = true;
        _disconnectedSinceMs = _clock.millis();
    }

    // A configuration change that adds or changes networks runs one immediate STA attempt cycle even
    // while a setup-AP client is associated; only background retries of an already failing station
    // pause for AP clients so periodic scans do not drop the configuring client off the AP.
    if (apHasClients && !_configAttemptPending) {
        if (_state != WIFI_CONN_PAUSED_AP_CLIENT) {
            HAL_LOG_PRINT("[WiFi] AP client associated; pausing STA scan/connect attempts.");
            if (_state == WIFI_CONN_FAST_PATH || _state == WIFI_CONN_TRYING_CANDIDATE) {
                _wifi.disconnect(false);
            }
            if (_state == WIFI_CONN_SCANNING) {
                _wifi.scanStop();
                _wifi.scanDelete();
            }
            _state = WIFI_CONN_PAUSED_AP_CLIENT;
        }
        return;
    }

    // Yield the radio to an active web scan instead of starting overlapping STA scans
    if (webScanActive) {
        return;
    }

    if (_pendingForceReconnect) {
        HAL_LOG_PRINT("[WiFi] Executing deferred reconnect.");
        forceReconnect();
        return;
    }

    if (_state == WIFI_CONN_PAUSED_AP_CLIENT) {
        HAL_LOG_PRINT("[WiFi] AP has no clients; resuming STA connection cycle.");
        startCycle(true);
        return;
    }

    uint32_t now = _clock.millis();

    switch (_state) {
        case WIFI_CONN_IDLE:
            startCycle(true);
            break;

        case WIFI_CONN_FAST_PATH:
            if (now - _attemptStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
                HAL_LOG_PRINTF("[WiFi] Fast-path slot %d (%s) timed out\n",
                               _tryingSlot, (_tryingSlot >= 0) ? _slots[_tryingSlot].ssid : "");
                _fastPathFailedSlot = _tryingSlot; // Skip failed fast-path slot for the rest of this cycle
                _wifi.disconnect(false);
                startScan();
            }
            break;

        case WIFI_CONN_SCANNING: {
            int16_t n = _wifi.scanComplete();
            if (n == WIFI_SCAN_RUNNING) {
                // Guard against a driver scan that never completes
                if (now - _scanStartMs >= WIFI_SCAN_TIMEOUT_MS) {
                    HAL_LOG_PRINTF("[WiFi] Connector scan timed out (%us); stopping scan.\n",
                                   (unsigned)(WIFI_SCAN_TIMEOUT_MS / 1000UL));
                    _wifi.scanStop();
                    _wifi.scanDelete();
                    buildCandidatesFromScan(-2, _fastPathFailedSlot);
                    if (_candidateCount == 0) {
                        enterBackoff();
                    } else {
                        _candidateIdx = 0;
                        tryCandidate(0);
                    }
                }
                return;
            }
            HAL_LOG_PRINTF("[WiFi] Scan complete (%d APs found)\n", (int)n);
            buildCandidatesFromScan(n, _fastPathFailedSlot);
            _wifi.scanDelete();
            if (_candidateCount == 0) {
                enterBackoff();
            } else {
                _candidateIdx = 0;
                tryCandidate(0);
            }
            break;
        }

        case WIFI_CONN_TRYING_CANDIDATE:
            if (now - _attemptStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
                HAL_LOG_PRINTF("[WiFi] Slot %d (%s) timed out\n",
                               _tryingSlot, (_tryingSlot >= 0) ? _slots[_tryingSlot].ssid : "");
                _wifi.disconnect(false);
                _candidateIdx++;
                if (_candidateIdx < _candidateCount) {
                    tryCandidate(_candidateIdx);
                } else {
                    enterBackoff();
                }
            }
            break;

        case WIFI_CONN_BACKOFF:
            if (_wakeFromBackoff.exchange(false)) {
                _backoffIdx = 0;
                HAL_LOG_PRINT("[WiFi] Waking from backoff for pending notification.");
                startCycle(true);
            } else if (now - _backoffStartMs >= _backoffDurationMs) {
                startCycle(true);
            }
            break;

        default:
            break;
    }
}

void WifiConnector::getConnectedSsid(char* buf, size_t len) const {
    if (!buf || len == 0) return;
    if (_wifi.status() == WL_CONNECTED) {
        if (_connectedSlot >= 0 && _connectedSlot < WIFI_SLOTS && _slots[_connectedSlot].ssid[0] != '\0') {
            strncpy(buf, _slots[_connectedSlot].ssid, len - 1);
            buf[len - 1] = '\0';
            return;
        }
        _wifi.connectedSsid(buf, len);
        return;
    }
    buf[0] = '\0';
}

uint32_t WifiConnector::getDisconnectedDurationMs() const {
    if (_wifi.status() == WL_CONNECTED || _savedCount == 0 || !_disconnectedTimerRunning) {
        return 0;
    }
    return _clock.millis() - _disconnectedSinceMs;
}

bool WifiConnector::consumeLastGoodSlotChanged(uint8_t& outSlot) {
    if (_lastGoodDirty) {
        _lastGoodDirty = false;
        _persistedLastGoodSlot = _lastGoodSlot;
        outSlot = _lastGoodSlot;
        return true;
    }
    return false;
}
