#include "protocol_helpers.h"
#include "hal.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

const char* systemModeStr(SystemMode mode) {
    if (mode == MODE_ARMED) return "ARMED";
    if (mode == MODE_SNOOZED) return "SNOOZED";
    return "DISARMED";
}

bool canSnoozeMode(SystemMode mode) {
    return (mode == MODE_ARMED || mode == MODE_SNOOZED);
}

uint16_t normalizeSnoozeMinutes(uint32_t requestedMin, uint16_t fallbackMin) {
    uint32_t effective = requestedMin;
    if (effective == 0) {
        effective = (fallbackMin >= MIN_SNOOZE_MIN) ? fallbackMin : DEFAULT_SNOOZE_MIN;
    }
    if (effective < MIN_SNOOZE_MIN) return MIN_SNOOZE_MIN;
    if (effective > MAX_SNOOZE_MIN) return MAX_SNOOZE_MIN;
    return (uint16_t)effective;
}

ButtonAction classifyPress(uint32_t pressDurationMs) {
    if (pressDurationMs >= BTN_FACTORY_RESET_MIN) {
        return BTN_ACT_FACTORY_RESET;
    }
    if (pressDurationMs >= BTN_SETUP_AP_MIN) {
        return BTN_ACT_SETUP_AP;
    }
    if (pressDurationMs >= BTN_LONG_PRESS_MIN) {
        return BTN_ACT_TEST_RECONNECT;
    }
    if (pressDurationMs >= BTN_DEBOUNCE_MS) {
        return BTN_ACT_TOGGLE;
    }
    return BTN_ACT_NONE;
}

LedPattern selectLedPattern(GridState gridState, bool apActive, SystemMode mode, bool btnHeld, uint32_t btnHeldMs) {
    LedPattern pattern;
    if (gridState == GRID_OFFLINE) {
        pattern = LED_RAPID_STROBE;
    } else if (apActive) {
        pattern = LED_SETUP_AP;
    } else if (mode == MODE_ARMED) {
        pattern = LED_SOLID_ON;
    } else if (mode == MODE_SNOOZED) {
        pattern = LED_DOUBLE_BLINK;
    } else {
        pattern = LED_SLOW_BLINK;
    }

    if (btnHeld) {
        if (btnHeldMs >= BTN_FACTORY_RESET_MIN) {
            pattern = LED_FACTORY_WIPE;
        } else if (btnHeldMs >= BTN_SETUP_AP_MIN) {
            pattern = LED_SETUP_AP;
        }
    }
    return pattern;
}

bool ledLevel(LedPattern pattern, uint32_t nowMs) {
    switch (pattern) {
        case LED_SOLID_ON:
            return true;

        case LED_SLOW_BLINK:
            return ((nowMs % 1000UL) < 500UL);

        case LED_RAPID_STROBE:
            return ((nowMs % 200UL) < 100UL);

        case LED_DOUBLE_BLINK: {
            uint32_t cycle = nowMs % 1000UL;
            return (cycle < 100UL || (cycle >= 200UL && cycle < 300UL));
        }

        case LED_SETUP_AP: {
            uint32_t cycle = nowMs % 1200UL;
            return (cycle < 100UL || (cycle >= 200UL && cycle < 300UL) || (cycle >= 400UL && cycle < 500UL));
        }

        case LED_FACTORY_WIPE:
            return ((nowMs % 100UL) < 50UL);

        default:
            return false;
    }
}

bool parsePositiveSnoozeInt(const char* s, uint32_t& outVal) {
    if (!s || s[0] == '\0') return false;
    uint64_t acc = 0;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        acc = acc * 10ULL + (uint64_t)(c - '0');
        if (acc > 86400000ULL) acc = 86400000ULL;
    }
    if (acc < 1ULL) return false;
    outVal = (uint32_t)acc;
    return true;
}

static void setErr(char* errField, size_t errFieldLen,
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

static bool hasJsonKeyLocal(JsonObjectConst obj, const char* key) {
    for (JsonPairConst kv : obj) {
        if (strcmp(kv.key().c_str(), key) == 0) return true;
    }
    return false;
}

bool parseSnoozeHttpRequest(bool hasMinArg, const char* minArg,
                            bool hasDurationArg, const char* durationArg,
                            const char* jsonBody, uint16_t defaultMin,
                            uint16_t& outMin,
                            char* errField, size_t errFieldLen,
                            char* errReason, size_t errReasonLen) {
    uint16_t durMin = normalizeSnoozeMinutes(0, defaultMin);

    if (hasMinArg) {
        uint32_t val = 0;
        if (!parsePositiveSnoozeInt(minArg, val)) {
            setErr(errField, errFieldLen, errReason, errReasonLen, "min", "must be a positive integer >= 1");
            return false;
        }
        durMin = normalizeSnoozeMinutes(val, defaultMin);
    } else if (hasDurationArg) {
        uint32_t durSec = 0;
        if (!parsePositiveSnoozeInt(durationArg, durSec)) {
            setErr(errField, errFieldLen, errReason, errReasonLen, "duration", "must be a positive integer >= 1 seconds");
            return false;
        }
        uint32_t m = (durSec >= 60UL) ? (durSec / 60UL) : MIN_SNOOZE_MIN;
        durMin = normalizeSnoozeMinutes(m, defaultMin);
    } else if (jsonBody && jsonBody[0] != '\0') {
        JsonDocument doc;
        if (deserializeJson(doc, jsonBody) || !doc.is<JsonObject>()) {
            setErr(errField, errFieldLen, errReason, errReasonLen, "body", "invalid JSON object");
            return false;
        }
        JsonObjectConst sObj = doc.as<JsonObjectConst>();
        if (hasJsonKeyLocal(sObj, "duration_min")) {
            if (!sObj["duration_min"].is<int64_t>()) {
                setErr(errField, errFieldLen, errReason, errReasonLen, "duration_min", "must be a positive integer >= 1");
                return false;
            }
            int64_t v = sObj["duration_min"].as<int64_t>();
            if (v < (int64_t)MIN_SNOOZE_MIN) {
                setErr(errField, errFieldLen, errReason, errReasonLen, "duration_min", "must be >= 1");
                return false;
            }
            uint32_t req = (v > (int64_t)MAX_SNOOZE_MIN) ? MAX_SNOOZE_MIN : (uint32_t)v;
            durMin = normalizeSnoozeMinutes(req, defaultMin);
        } else if (hasJsonKeyLocal(sObj, "duration")) {
            if (!sObj["duration"].is<int64_t>()) {
                setErr(errField, errFieldLen, errReason, errReasonLen, "duration", "must be a positive integer >= 1 seconds");
                return false;
            }
            int64_t v = sObj["duration"].as<int64_t>();
            if (v < 1) {
                setErr(errField, errFieldLen, errReason, errReasonLen, "duration", "must be >= 1 seconds");
                return false;
            }
            uint64_t m = ((uint64_t)v >= 60ULL) ? ((uint64_t)v / 60ULL) : (uint64_t)MIN_SNOOZE_MIN;
            uint32_t req = (m > (uint64_t)MAX_SNOOZE_MIN) ? MAX_SNOOZE_MIN : (uint32_t)m;
            durMin = normalizeSnoozeMinutes(req, defaultMin);
        } else {
            setErr(errField, errFieldLen, errReason, errReasonLen, "body", "expected duration_min or duration");
            return false;
        }
    }

    outMin = durMin;
    return true;
}

struct UrlAuthority {
    const char* hostStart;
    const char* effectiveHost;
    const char* authEnd;
};

static bool parseUrlAuthority(const char* url, UrlAuthority& out) {
    if (!url) return false;
    const char* schemeSep = strstr(url, "://");
    if (!schemeSep || schemeSep == url) return false;

    out.hostStart = schemeSep + 3;
    out.authEnd = out.hostStart + strlen(out.hostStart);
    for (const char* p = out.hostStart; *p != '\0'; ++p) {
        if (*p == '/' || *p == '?' || *p == '#') {
            out.authEnd = p;
            break;
        }
    }

    const char* atPos = nullptr;
    for (const char* p = out.hostStart; p < out.authEnd; ++p) {
        if (*p == '@') {
            atPos = p;
        }
    }
    out.effectiveHost = (atPos != nullptr) ? (atPos + 1) : out.hostStart;
    return true;
}

static bool equalsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

void extractSchemeHost(const char* url, char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return;
    UrlAuthority auth;
    if (!parseUrlAuthority(url, auth)) {
        snprintf(outBuf, outCap, "(invalid)");
        return;
    }
    size_t schemePrefixLen = (size_t)(auth.hostStart - url);
    size_t hostPortLen = (size_t)(auth.authEnd - auth.effectiveHost);
    size_t pos = 0;
    if (schemePrefixLen >= outCap) schemePrefixLen = outCap - 1;
    memcpy(outBuf + pos, url, schemePrefixLen);
    pos += schemePrefixLen;
    if (pos + hostPortLen >= outCap) {
        hostPortLen = (outCap - 1 > pos) ? (outCap - 1 - pos) : 0;
    }
    if (hostPortLen > 0) {
        memcpy(outBuf + pos, auth.effectiveHost, hostPortLen);
        pos += hostPortLen;
    }
    outBuf[pos] = '\0';
}

bool extractUrlHost(const char* url, char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return false;
    outBuf[0] = '\0';

    UrlAuthority auth;
    if (!parseUrlAuthority(url, auth)) return false;
    if (auth.effectiveHost >= auth.authEnd) return false;

    const char* hostBegin = auth.effectiveHost;
    const char* hostEnd = auth.authEnd;

    if (*hostBegin == '[') {
        const char* closeBracket = nullptr;
        for (const char* p = hostBegin + 1; p < auth.authEnd; ++p) {
            if (*p == ']') {
                closeBracket = p;
                break;
            }
        }
        if (!closeBracket) return false;
        if (closeBracket + 1 < auth.authEnd && *(closeBracket + 1) != ':') return false;
        ++hostBegin;
        hostEnd = closeBracket;
    } else {
        for (const char* p = hostBegin; p < auth.authEnd; ++p) {
            if (*p == ':') {
                hostEnd = p;
                break;
            }
        }
    }

    if (hostEnd <= hostBegin) return false;
    size_t len = (size_t)(hostEnd - hostBegin);
    if (len >= outCap) return false;

    memcpy(outBuf, hostBegin, len);
    outBuf[len] = '\0';
    return true;
}

static bool isIpv4Literal(const char* s) {
    if (!s || s[0] == '\0') return false;
    int octets = 0;
    const char* p = s;
    while (*p != '\0') {
        if (octets >= 4) return false;
        if (*p < '0' || *p > '9') return false;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;
        uint16_t val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = (uint16_t)(val * 10U + (uint16_t)(*p - '0'));
            if (val > 255U) return false;
            ++digits;
            ++p;
        }
        if (digits == 0 || digits > 3) return false;
        ++octets;
        if (*p == '.') {
            ++p;
            if (*p == '\0') return false;
        } else if (*p != '\0') {
            return false;
        }
    }
    return octets == 4;
}

static bool isIpv6Literal(const char* s) {
    if (!s || s[0] == '\0') return false;
    const char* p = s;
    bool hasDoubleColon = false;
    int groups = 0;
    int colons = 0;

    if (*p == ':') {
        if (p[1] != ':') return false;
        hasDoubleColon = true;
        colons = 2;
        p += 2;
        if (*p == '\0') return true; // "::"
    }

    while (*p != '\0') {
        // An IPv4 tail is permitted only as the final segment (no ':' remaining)
        if (strchr(p, ':') == nullptr && strchr(p, '.') != nullptr) {
            if (colons < 2 || !isIpv4Literal(p)) return false;
            groups += 2;
            break;
        }

        int hexDigits = 0;
        while (*p != '\0' && *p != ':') {
            if (!isxdigit((unsigned char)*p)) return false;
            ++hexDigits;
            if (hexDigits > 4) return false;
            ++p;
        }
        if (hexDigits == 0) return false;
        ++groups;

        if (*p == ':') {
            ++p;
            ++colons;
            if (*p == ':') {
                if (hasDoubleColon) return false;
                hasDoubleColon = true;
                ++p;
                ++colons;
                if (*p == ':') return false;
                if (*p == '\0') break;
            } else if (*p == '\0') {
                return false; // Trailing single ':'
            }
        }
    }

    if (colons < 2) return false;
    return hasDoubleColon ? (groups <= 7) : (groups == 8);
}

bool isIpLiteral(const char* host) {
    if (!host || host[0] == '\0') return false;
    return isIpv4Literal(host) || isIpv6Literal(host);
}

bool isNtfyHost(const char* url) {
    char host[MAX_NOTIFY_URL_LEN + 1];
    if (!extractUrlHost(url, host, sizeof(host))) return false;
    return equalsIgnoreCase(host, "ntfy.sh");
}

bool hasNtfyTemplate(const char* eventName) {
    if (!eventName || eventName[0] == '\0') return false;
    return strcmp(eventName, "POWER_OFF") == 0 ||
           strcmp(eventName, "POWER_ON") == 0 ||
           strcmp(eventName, "TEST_ALARM") == 0;
}

bool shouldUseNtfyFormat(const char* url, const char* eventName) {
    return isNtfyHost(url) && hasNtfyTemplate(eventName);
}

bool expandNotificationUrl(const char* tmpl, const char* eventName, char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return false;
    outBuf[0] = '\0';
    if (!tmpl || !eventName || eventName[0] == '\0') return false;

    size_t pos = 0;
    const char* p = tmpl;
    while (*p != '\0') {
        if (strncmp(p, "{event}", 7) == 0) {
            for (const char* e = eventName; *e != '\0'; ++e) {
                if (pos + 1 >= outCap) {
                    outBuf[0] = '\0';
                    return false;
                }
                outBuf[pos++] = (char)tolower((unsigned char)*e);
            }
            p += 7;
        } else {
            if (pos + 1 >= outCap) {
                outBuf[0] = '\0';
                return false;
            }
            outBuf[pos++] = *p++;
        }
    }
    outBuf[pos] = '\0';
    return true;
}

bool shouldNotifyTarget(NotifyTarget target, const char* targetUrl,
                        const char* eventName, AlertTrackingMode alertMode,
                        bool cloudEnabled) {
    if (!targetUrl || targetUrl[0] == '\0' || !eventName) return false;

    bool isPowerOff  = (strcmp(eventName, "POWER_OFF") == 0);
    bool isPowerOn   = (strcmp(eventName, "POWER_ON") == 0);
    bool isTestAlarm = (strcmp(eventName, "TEST_ALARM") == 0);
    bool isAlarmStop = (strcmp(eventName, "ALARM_STOP") == 0);
    if (!isPowerOff && !isPowerOn && !isTestAlarm && !isAlarmStop) return false;

    if (target == NOTIFY_TARGET_LOCAL) {
        return true;
    }
    if (target == NOTIFY_TARGET_CLOUD) {
        if (!cloudEnabled) return false;
        if (isAlarmStop) return false;
        if (isPowerOn) return (alertMode == ALERT_OUTAGE_AND_RESTORE);
        return true;
    }
    return false;
}

// State events change what the phone is doing (ring or stop) and follow latest-wins per target;
// TEST_ALARM is a one-off check that may be lost without leaving the phone in a wrong state.
bool isStateNotifyEvent(const char* eventName) {
    if (!eventName) return false;
    return strcmp(eventName, "POWER_OFF") == 0 ||
           strcmp(eventName, "POWER_ON") == 0 ||
           strcmp(eventName, "ALARM_STOP") == 0;
}

bool shouldSendAlarmStop(SystemMode previousMode, SystemMode nextMode) {
    if (nextMode == MODE_DISARMED) {
        return previousMode == MODE_ARMED;
    }
    if (nextMode == MODE_SNOOZED) {
        return previousMode == MODE_ARMED || previousMode == MODE_SNOOZED;
    }
    return false;
}

size_t buildPowerOffJsonBuf(const char* deviceName, uint32_t durationS,
                            uint32_t uptimeS, uint32_t epochTs, char* outBuf, size_t outCap) {
    JsonDocument doc;
    doc["event"] = "POWER_OFF";
    doc["device"] = deviceName ? deviceName : DEFAULT_DEVICE_NAME;
    doc["state"] = "ARMED";
    doc["grid"] = "OFFLINE";
    doc["power"] = false;
    doc["armed"] = true;
    doc["duration_s"] = durationS;
    doc["uptime_s"] = uptimeS;
    doc["ts"] = epochTs;
    return serializeJson(doc, outBuf, outCap);
}

size_t buildPowerOnJsonBuf(const char* deviceName, SystemMode mode, uint32_t durationS,
                           uint32_t uptimeS, AlertTrackingMode alertMode, uint32_t epochTs,
                           char* outBuf, size_t outCap) {
    JsonDocument doc;
    doc["event"] = "POWER_ON";
    doc["device"] = deviceName ? deviceName : DEFAULT_DEVICE_NAME;
    doc["state"] = systemModeStr(mode);
    doc["grid"] = "ONLINE";
    doc["power"] = true;
    doc["armed"] = (mode == MODE_ARMED);
    doc["duration_s"] = durationS;
    doc["uptime_s"] = uptimeS;
    doc["alert_mode"] = (alertMode == ALERT_OUTAGE_ONLY) ? "OUTAGE_ONLY" : "OUTAGE_AND_RESTORE";
    doc["ts"] = epochTs;
    return serializeJson(doc, outBuf, outCap);
}

size_t buildAlarmStopJsonBuf(const char* deviceName, SystemMode mode, uint32_t snoozeRemainingS,
                             uint32_t uptimeS, uint32_t epochTs, char* outBuf, size_t outCap) {
    JsonDocument doc;
    doc["event"] = "ALARM_STOP";
    doc["device"] = deviceName ? deviceName : DEFAULT_DEVICE_NAME;
    doc["state"] = systemModeStr(mode);
    doc["snooze_remaining_s"] = snoozeRemainingS;
    doc["uptime_s"] = uptimeS;
    doc["ts"] = epochTs;
    return serializeJson(doc, outBuf, outCap);
}

size_t buildTestAlarmJsonBuf(const char* deviceName, SystemMode mode, uint32_t uptimeS,
                             uint32_t epochTs, char* outBuf, size_t outCap) {
    JsonDocument doc;
    doc["event"] = "TEST_ALARM";
    doc["device"] = deviceName ? deviceName : DEFAULT_DEVICE_NAME;
    doc["state"] = systemModeStr(mode);
    doc["grid"] = "TEST";
    doc["power"] = false;
    doc["armed"] = (mode == MODE_ARMED);
    doc["duration_s"] = 0;
    doc["uptime_s"] = uptimeS;
    doc["ts"] = epochTs;
    return serializeJson(doc, outBuf, outCap);
}

size_t buildStatusJsonBuf(const StatusSnapshot& snap, char* outBuf, size_t outCap) {
    JsonDocument doc;
    doc["device"]             = snap.deviceName ? snap.deviceName : DEFAULT_DEVICE_NAME;
    doc["fw_version"]         = (snap.fwVersion && snap.fwVersion[0] != '\0') ? snap.fwVersion : FW_VERSION;
    doc["grid"]               = gridStateStr(snap.gridState);
    doc["mode"]               = systemModeStr(snap.mode);
    doc["power"]              = (snap.gridState == GRID_ONLINE);
    doc["armed"]              = (snap.mode == MODE_ARMED);
    doc["outage_duration_s"]  = snap.outageDurationS;
    doc["snooze_remaining_s"] = snap.snoozeRemainingS;
    doc["uptime_s"]           = snap.uptimeS;

    JsonObject wifiObj = doc["wifi"].to<JsonObject>();
    wifiObj["connected"] = snap.wifiConnected;
    wifiObj["ssid"]      = snap.wifiSsid ? snap.wifiSsid : "";
    wifiObj["slot"]      = snap.wifiSlot;
    wifiObj["ip"]        = snap.wifiIp ? snap.wifiIp : "";
    wifiObj["rssi"]      = snap.wifiRssi;

    JsonObject apObj = doc["setup_ap"].to<JsonObject>();
    apObj["active"]      = snap.apActive;
    apObj["ssid"]        = snap.apSsid ? snap.apSsid : "";
    apObj["ip"]          = snap.apIp ? snap.apIp : "";
    apObj["clients"]     = snap.apClients;
    apObj["closes_in_s"] = snap.apClosesInS;

    doc["admin_protected"]    = snap.adminProtected;
    doc["snooze_default_min"] = snap.snoozeDefaultMin;
    doc["free_heap"]          = snap.freeHeap;
    doc["ts"]                 = snap.ts;

    auto writeNotifySlot = [](JsonObject parent, const char* key, const NotifyStatusSnapshot& slot) {
        if (!slot.valid) {
            parent[key] = nullptr;
            return;
        }
        JsonObject obj = parent[key].to<JsonObject>();
        obj["event"]  = slot.event;
        obj["ok"]     = slot.ok;
        obj["code"]   = slot.code;
        obj["reason"] = slot.reason;
        obj["age_s"]  = slot.ageS;
    };
    JsonObject notifyObj = doc["notify"].to<JsonObject>();
    writeNotifySlot(notifyObj, "local", snap.notifyLocal);
    writeNotifySlot(notifyObj, "cloud", snap.notifyCloud);

    return serializeJson(doc, outBuf, outCap);
}

namespace {

struct ScopedSpinLock {
    std::atomic<bool>& flag;
    explicit ScopedSpinLock(std::atomic<bool>& f) : flag(f) {
        while (flag.exchange(true, std::memory_order_acquire)) {}
    }
    ~ScopedSpinLock() {
        flag.store(false, std::memory_order_release);
    }
};

} // namespace

AlertRouter::AlertRouter()
    : _locked(false),
      _lastStopMode(MODE_DISARMED),
      _lastStopSnoozeS(0) {
    memset(_targetSeq, 0, sizeof(_targetSeq));
    memset(_pending, 0, sizeof(_pending));
}

uint32_t AlertRouter::getTargetStateSeq(NotifyTarget target) const {
    if ((int)target < 0 || (int)target >= NOTIFY_TARGET_COUNT) return 0;
    ScopedSpinLock guard(_locked);
    return _targetSeq[target];
}

bool AlertRouter::isStateSeqCurrent(NotifyTarget target, uint32_t eventSeq) const {
    if (eventSeq == 0) return true;
    return getTargetStateSeq(target) == eventSeq;
}

void AlertRouter::markPendingTargetEvent(NotifyTarget target, const char* eventName,
                                         uint32_t durationS, AlertTrackingMode alertMode,
                                         uint32_t eventSeq) {
    if ((int)target < 0 || (int)target >= NOTIFY_TARGET_COUNT || !eventName) return;

    PendingNotifyEvent ev = PENDING_EVENT_NONE;
    if (strcmp(eventName, "POWER_OFF") == 0) {
        ev = PENDING_EVENT_POWER_OFF;
    } else if (strcmp(eventName, "POWER_ON") == 0) {
        ev = PENDING_EVENT_POWER_ON;
    } else if (strcmp(eventName, "ALARM_STOP") == 0) {
        ev = PENDING_EVENT_ALARM_STOP;
    } else {
        return;
    }

    ScopedSpinLock guard(_locked);
    if (eventSeq != 0 && eventSeq < _targetSeq[target]) {
        return;
    }
    if (eventSeq == 0) {
        ++_targetSeq[target];
    }

    if (target == NOTIFY_TARGET_CLOUD) {
        if (ev == PENDING_EVENT_ALARM_STOP) {
            return;
        }
        if (ev == PENDING_EVENT_POWER_ON && alertMode != ALERT_OUTAGE_AND_RESTORE) {
            if (_pending[NOTIFY_TARGET_CLOUD].event == PENDING_EVENT_POWER_OFF) {
                _pending[NOTIFY_TARGET_CLOUD].event = PENDING_EVENT_NONE;
            }
            return;
        }
    }

    PendingTargetState& slot = _pending[target];
    slot.event = ev;
    slot.durationS = durationS;
    slot.alertMode = alertMode;
    slot.stopMode = _lastStopMode;
    slot.snoozeRemainingS = _lastStopSnoozeS;

    if (ev == PENDING_EVENT_POWER_ON && alertMode == ALERT_OUTAGE_ONLY &&
        _pending[NOTIFY_TARGET_CLOUD].event == PENDING_EVENT_POWER_OFF) {
        _pending[NOTIFY_TARGET_CLOUD].event = PENDING_EVENT_NONE;
        ++_targetSeq[NOTIFY_TARGET_CLOUD];
    }
}

void AlertRouter::handleAttemptsExhausted(NotifyTarget target, const char* eventName,
                                          uint32_t durationS, AlertTrackingMode alertMode,
                                          uint32_t eventSeq, bool staConnected) {
    if (staConnected) {
        return;
    }
    markPendingTargetEvent(target, eventName, durationS, alertMode, eventSeq);
}

void AlertRouter::markPendingTargetAlarmStop(SystemMode stopMode, uint32_t snoozeRemainingS) {
    {
        ScopedSpinLock guard(_locked);
        _lastStopMode = (stopMode == MODE_SNOOZED) ? MODE_SNOOZED : MODE_DISARMED;
        _lastStopSnoozeS = (_lastStopMode == MODE_SNOOZED) ? snoozeRemainingS : 0;
        if (_pending[NOTIFY_TARGET_CLOUD].event == PENDING_EVENT_POWER_OFF) {
            _pending[NOTIFY_TARGET_CLOUD].event = PENDING_EVENT_NONE;
            ++_targetSeq[NOTIFY_TARGET_CLOUD];
        }
    }
    markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "ALARM_STOP", 0, ALERT_OUTAGE_ONLY);
}

void AlertRouter::handlePowerOff(const char* deviceName, SystemMode mode, uint32_t durationS,
                                 uint32_t uptimeS, uint32_t epochTs,
                                 bool staConnected, IAlertOutput& out) {
    if (mode != MODE_ARMED) {
        HAL_LOG_PRINTF("[Alert] Outage detected but system is %s: POWER_OFF suppressed.\n",
                       (mode == MODE_SNOOZED) ? "SNOOZED" : "DISARMED");
        return;
    }

    if (!staConnected) {
        markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_OFF", durationS);
        markPendingTargetEvent(NOTIFY_TARGET_CLOUD, "POWER_OFF", durationS);
        HAL_LOG_PRINT("[Alert] Wi-Fi offline during outage – POWER_OFF marked pending for reconnect.");
        return;
    }

    {
        ScopedSpinLock guard(_locked);
        for (int i = 0; i < NOTIFY_TARGET_COUNT; ++i) {
            ++_targetSeq[i];
            _pending[i].event = PENDING_EVENT_NONE;
        }
    }

    char jsonBuf[512];
    buildPowerOffJsonBuf(deviceName, durationS, uptimeS, epochTs, jsonBuf, sizeof(jsonBuf));
    HAL_LOG_PRINT("[Alert] POWER_OFF Outage Alert queued for notification targets.");
    out.queueNotification(NOTIFY_TARGET_LOCAL, "POWER_OFF", durationS, jsonBuf);
    out.queueNotification(NOTIFY_TARGET_CLOUD, "POWER_OFF", durationS, jsonBuf);
}

void AlertRouter::handlePowerOn(const char* deviceName, SystemMode mode, uint32_t durationS,
                                uint32_t uptimeS, AlertTrackingMode alertMode, uint32_t epochTs,
                                IAlertOutput& out, bool staConnected) {
    if (!staConnected) {
        markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_ON", durationS, alertMode);
        markPendingTargetEvent(NOTIFY_TARGET_CLOUD, "POWER_ON", durationS, alertMode);
        HAL_LOG_PRINT("[Alert] Wi-Fi offline during restore – POWER_ON marked pending for reconnect.");
        return;
    }

    {
        ScopedSpinLock guard(_locked);
        for (int i = 0; i < NOTIFY_TARGET_COUNT; ++i) {
            ++_targetSeq[i];
            _pending[i].event = PENDING_EVENT_NONE;
        }
    }

    char jsonBuf[512];
    buildPowerOnJsonBuf(deviceName, mode, durationS, uptimeS, alertMode, epochTs, jsonBuf, sizeof(jsonBuf));

    // Local notify URL always receives POWER_ON so phone alarm macros can silence on restore
    out.queueNotification(NOTIFY_TARGET_LOCAL, "POWER_ON", durationS, jsonBuf);

    // Cloud Webhook is strictly gated by ALERT_OUTAGE_AND_RESTORE policy
    if (alertMode == ALERT_OUTAGE_AND_RESTORE) {
        out.queueNotification(NOTIFY_TARGET_CLOUD, "POWER_ON", durationS, jsonBuf);
    } else {
        HAL_LOG_PRINT("[Policy] Tracking mode is OUTAGE_ONLY: Cloud restoration webhook suppressed.");
    }
}

bool AlertRouter::handleEnterArmed(const char* deviceName, SystemMode previousMode, GridState gridState,
                                   uint32_t outageDurationS, uint32_t uptimeS, uint32_t epochTs,
                                   bool staConnected, IAlertOutput& out) {
    if (previousMode == MODE_ARMED) return false;
    if (gridState != GRID_OFFLINE) return false;

    handlePowerOff(deviceName, MODE_ARMED, outageDurationS, uptimeS, epochTs, staConnected, out);
    return true;
}

bool AlertRouter::handleAlarmStop(const char* deviceName, SystemMode previousMode, SystemMode nextMode,
                                  uint32_t snoozeRemainingS, uint32_t uptimeS, uint32_t epochTs,
                                  IAlertOutput& out, bool staConnected) {
    if (!shouldSendAlarmStop(previousMode, nextMode)) return false;

    if (!staConnected) {
        markPendingTargetAlarmStop(nextMode, snoozeRemainingS);
        HAL_LOG_PRINT("[Alert] Wi-Fi offline during alarm stop – ALARM_STOP marked pending for reconnect.");
        return true;
    }

    {
        ScopedSpinLock guard(_locked);
        _lastStopMode = (nextMode == MODE_SNOOZED) ? MODE_SNOOZED : MODE_DISARMED;
        _lastStopSnoozeS = (_lastStopMode == MODE_SNOOZED) ? snoozeRemainingS : 0;
        ++_targetSeq[NOTIFY_TARGET_LOCAL];
        _pending[NOTIFY_TARGET_LOCAL].event = PENDING_EVENT_NONE;
        if (_pending[NOTIFY_TARGET_CLOUD].event == PENDING_EVENT_POWER_OFF) {
            ++_targetSeq[NOTIFY_TARGET_CLOUD];
            _pending[NOTIFY_TARGET_CLOUD].event = PENDING_EVENT_NONE;
        }
    }

    char jsonBuf[384];
    buildAlarmStopJsonBuf(deviceName, nextMode, snoozeRemainingS, uptimeS, epochTs, jsonBuf, sizeof(jsonBuf));
    out.queueNotification(NOTIFY_TARGET_LOCAL, "ALARM_STOP", 0, jsonBuf);
    return true;
}

bool AlertRouter::flushPendingOnReconnect(const char* deviceName, bool isOnline, SystemMode mode,
                                          uint32_t outageDurationS, uint32_t uptimeS, uint32_t epochTs,
                                          bool staConnected, IAlertOutput& out) {
    if (!staConnected) return false;

    PendingTargetState snap[NOTIFY_TARGET_COUNT];
    bool anyPending = false;
    {
        ScopedSpinLock guard(_locked);
        for (int i = 0; i < NOTIFY_TARGET_COUNT; ++i) {
            snap[i] = _pending[i];
            if (snap[i].event != PENDING_EVENT_NONE) {
                anyPending = true;
            }
            _pending[i].event = PENDING_EVENT_NONE;
        }
    }
    if (!anyPending) return false;

    bool anyQueued = false;
    char powerOffJson[512];
    bool powerOffBuilt = false;

    for (int i = 0; i < NOTIFY_TARGET_COUNT; ++i) {
        const PendingTargetState& st = snap[i];
        NotifyTarget target = (NotifyTarget)i;

        if (st.event == PENDING_EVENT_POWER_OFF) {
            if (isOnline || mode != MODE_ARMED) {
                HAL_LOG_PRINTF("[Alert] Pending POWER_OFF cleared on reconnect without re-send (target: %d, grid: %s, mode: %s).\n",
                               i, isOnline ? "ONLINE" : "OFFLINE", systemModeStr(mode));
                continue;
            }
            if (!powerOffBuilt) {
                buildPowerOffJsonBuf(deviceName, outageDurationS, uptimeS, epochTs, powerOffJson, sizeof(powerOffJson));
                powerOffBuilt = true;
            }
            HAL_LOG_PRINTF("[Alert] Wi-Fi restored during active blackout! Re-queuing pending POWER_OFF for target %d.\n", i);
            out.queueNotification(target, "POWER_OFF", outageDurationS, powerOffJson);
            anyQueued = true;
        } else if (st.event == PENDING_EVENT_POWER_ON) {
            if (target == NOTIFY_TARGET_CLOUD && st.alertMode != ALERT_OUTAGE_AND_RESTORE) {
                continue;
            }
            char powerOnJson[512];
            buildPowerOnJsonBuf(deviceName, mode, st.durationS, uptimeS, st.alertMode, epochTs, powerOnJson, sizeof(powerOnJson));
            HAL_LOG_PRINTF("[Alert] Wi-Fi restored! Re-queuing pending POWER_ON for target %d.\n", i);
            out.queueNotification(target, "POWER_ON", st.durationS, powerOnJson);
            anyQueued = true;
        } else if (st.event == PENDING_EVENT_ALARM_STOP) {
            if (target != NOTIFY_TARGET_LOCAL) {
                continue;
            }
            SystemMode stopMode = (st.stopMode == MODE_SNOOZED) ? MODE_SNOOZED : MODE_DISARMED;
            uint32_t snoozeRemS = (stopMode == MODE_SNOOZED) ? st.snoozeRemainingS : 0;
            char alarmStopJson[384];
            buildAlarmStopJsonBuf(deviceName, stopMode, snoozeRemS, uptimeS, epochTs, alarmStopJson, sizeof(alarmStopJson));
            HAL_LOG_PRINTF("[Alert] Wi-Fi restored! Re-queuing pending ALARM_STOP for target %d.\n", i);
            out.queueNotification(target, "ALARM_STOP", 0, alarmStopJson);
            anyQueued = true;
        }
    }

    return anyQueued;
}

void computeNotifyTargetChanges(const DeviceConfig& prev, const DeviceConfig& next,
                                bool& clearLocal, bool& clearCloud) {
    clearLocal = (strcmp(prev.local_url, next.local_url) != 0) ||
                 (next.local_url[0] == '\0');
    clearCloud = (strcmp(prev.webhook_url, next.webhook_url) != 0) ||
                 (next.webhook_url[0] == '\0') ||
                 (prev.cloud_enabled != next.cloud_enabled) ||
                 (!next.cloud_enabled);
}

NotifyPrepareResult prepareNotifyPayload(const DeviceConfig& config, NotifyTarget target,
                                         uint32_t generation, const char* event,
                                         uint32_t durationS, const char* jsonPayload,
                                         NotifyPayload& outPayload,
                                         char* errReason, size_t errReasonCap) {
    if (errReason && errReasonCap > 0) errReason[0] = '\0';
    if ((int)target < 0 || (int)target >= NOTIFY_TARGET_COUNT) return NOTIFY_PREP_SKIP;
    if (!event || !jsonPayload) return NOTIFY_PREP_SKIP;

    const char* rawUrl = (target == NOTIFY_TARGET_LOCAL) ? config.local_url : config.webhook_url;
    if (!shouldNotifyTarget(target, rawUrl, event, config.alert_mode, config.cloud_enabled)) {
        return NOTIFY_PREP_SKIP;
    }

    memset(&outPayload, 0, sizeof(outPayload));
    outPayload.urlGeneration = generation;
    if (!expandNotificationUrl(rawUrl, event, outPayload.url, sizeof(outPayload.url))) {
        if (errReason && errReasonCap > 0) {
            snprintf(errReason, errReasonCap, "URL too long");
        }
        return NOTIFY_PREP_ERROR;
    }

    strncpy(outPayload.event, event, sizeof(outPayload.event) - 1);
    strncpy(outPayload.device, config.device_name, sizeof(outPayload.device) - 1);
    outPayload.duration_s = durationS;
    strncpy(outPayload.jsonBody, jsonPayload, sizeof(outPayload.jsonBody) - 1);
    return NOTIFY_PREP_READY;
}

void formatTlsErrorReason(int tlsCode, const char* mbedtlsText,
                          char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return;
    outBuf[0] = '\0';
    if (tlsCode == 0) return;
    // start_ssl_client() returns -1 for socket connect/select/handshake timeouts,
    // which is not a real mbedTLS error code.
    if (tlsCode == -1) {
        snprintf(outBuf, outCap, "TLS: connect or handshake failed");
        return;
    }
    if (mbedtlsText && mbedtlsText[0] != '\0') {
        snprintf(outBuf, outCap, "TLS: %s", mbedtlsText);
        return;
    }
    snprintf(outBuf, outCap, "TLS error %d", tlsCode);
}

void formatNotifyFailureReason(int code, const char* explicitReason,
                               char* outBuf, size_t outCap) {
    if (!outBuf || outCap == 0) return;
    if (explicitReason && explicitReason[0] != '\0') {
        snprintf(outBuf, outCap, "%s", explicitReason);
        return;
    }
    if (code > 0) {
        snprintf(outBuf, outCap, "HTTP %d", code);
        return;
    }
    switch (code) {
        case -1:  snprintf(outBuf, outCap, "Connection refused"); break;
        case -2:  snprintf(outBuf, outCap, "Send header failed"); break;
        case -3:  snprintf(outBuf, outCap, "Send payload failed"); break;
        case -4:  snprintf(outBuf, outCap, "Not connected"); break;
        case -5:  snprintf(outBuf, outCap, "Connection lost"); break;
        case -6:  snprintf(outBuf, outCap, "No stream"); break;
        case -7:  snprintf(outBuf, outCap, "No HTTP server"); break;
        case -8:  snprintf(outBuf, outCap, "Out of memory"); break;
        case -9:  snprintf(outBuf, outCap, "Encoding error"); break;
        case -10: snprintf(outBuf, outCap, "Stream write error"); break;
        case -11: snprintf(outBuf, outCap, "Read timeout"); break;
        default:
            if (code < 0) {
                snprintf(outBuf, outCap, "Client error %d", code);
            } else {
                snprintf(outBuf, outCap, "Failed");
            }
            break;
    }
}

NotifyStatusTracker::NotifyStatusTracker() {
    memset(_generations, 0, sizeof(_generations));
    resetAll();
}

void NotifyStatusTracker::resetAll() {
    for (int i = 0; i < NOTIFY_TARGET_COUNT; ++i) {
        clearTarget((NotifyTarget)i);
    }
}

void NotifyStatusTracker::clearTarget(NotifyTarget target) {
    int idx = (int)target;
    if (idx < 0 || idx >= NOTIFY_TARGET_COUNT) return;
    memset(&_targets[idx], 0, sizeof(_targets[idx]));
    ++_generations[idx];
}

uint32_t NotifyStatusTracker::getGeneration(NotifyTarget target) const {
    int idx = (int)target;
    if (idx < 0 || idx >= NOTIFY_TARGET_COUNT) return 0;
    return _generations[idx];
}

bool NotifyStatusTracker::recordResult(NotifyTarget target, uint32_t generation,
                                       const char* eventName, bool ok, int code,
                                       const char* reason, uint32_t nowMs) {
    int idx = (int)target;
    if (idx < 0 || idx >= NOTIFY_TARGET_COUNT) return false;
    // Drop completions belonging to a target URL that has since been changed or cleared.
    if (generation != _generations[idx]) return false;
    NotifyTargetResult& slot = _targets[idx];
    slot.valid = true;
    snprintf(slot.event, sizeof(slot.event), "%s", eventName ? eventName : "");
    slot.ok = ok;
    slot.code = code;
    if (ok) {
        slot.reason[0] = '\0';
    } else {
        formatNotifyFailureReason(code, reason, slot.reason, sizeof(slot.reason));
    }
    slot.recordedAtMs = nowMs;
    return true;
}

NotifyStatusSnapshot NotifyStatusTracker::getSnapshot(NotifyTarget target, bool configured,
                                                      uint32_t nowMs) const {
    NotifyStatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    int idx = (int)target;
    if (!configured || idx < 0 || idx >= NOTIFY_TARGET_COUNT) {
        return snap;
    }
    const NotifyTargetResult& slot = _targets[idx];
    if (!slot.valid) {
        return snap;
    }
    snap.valid = true;
    snprintf(snap.event, sizeof(snap.event), "%s", slot.event);
    snap.ok = slot.ok;
    snap.code = slot.code;
    snprintf(snap.reason, sizeof(snap.reason), "%s", slot.reason);
    snap.ageS = (uint32_t)(nowMs - slot.recordedAtMs) / 1000UL;
    return snap;
}
