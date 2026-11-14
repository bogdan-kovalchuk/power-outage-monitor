#ifndef TEST_FAKES_H
#define TEST_FAKES_H

#include "hal.h"
#include "protocol_helpers.h"
#include "setup_ap.h"

#include <map>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

class FakeClock : public IClock {
public:
    explicit FakeClock(uint32_t initialMs) : _now(initialMs) {}

    uint32_t millis() const override { return _now; }
    void set(uint32_t ms) { _now = ms; }
    void advance(uint32_t deltaMs) { _now += deltaMs; }

private:
    uint32_t _now;
};

struct FakeScanAp {
    std::string ssid;
    int32_t rssi;
    bool isOpen;
    int32_t channel;
};

struct FakeAcceptCred {
    std::string ssid;
    std::string pass;
    uint32_t connectDelayMs;
    int channel;
    std::string ipStr;
    int rssi;
};

struct WifiCallRecord {
    std::string fn;
    std::string arg1;
    std::string arg2;
    int intArg;
};

class FakeWifiHal : public IWifiHal {
public:
    FakeWifiHal(const IClock& clock)
        : _clock(clock),
          _mode(WIFI_OFF),
          _connecting(false),
          _connected(false),
          _dropInjected(false),
          _connectStartMs(0),
          _connectDelayMs(0),
          _shouldSucceed(false),
          _connectedChannel(1),
          _connectedRssi(-55),
          _connectedIp("0.0.0.0"),
          _scanRunning(false),
          _scanReady(false),
          _scanStuck(false),
          _scanStartMs(0),
          _scanDurationMs(200),
          _softApActive(false),
          _softApChannel(1),
          _softApClients(0),
          _softApIp(SETUP_AP_IP_STR),
          _softApStartFailCount(0) {}

    // Configuration helpers
    void addScanAp(const std::string& ssid, int32_t rssi, bool isOpen = false, int32_t channel = 1) {
        _scanAps.push_back(FakeScanAp{ssid, rssi, isOpen, channel});
    }

    void clearAcceptCreds() { _acceptCreds.clear(); }
    void addAcceptCred(const std::string& ssid,
                       const std::string& pass,
                       uint32_t connectDelayMs = 100,
                       int channel = 1,
                       const std::string& ipStr = "192.168.1.50",
                       int rssi = -55) {
        _acceptCreds.push_back(FakeAcceptCred{ssid, pass, connectDelayMs, channel, ipStr, rssi});
    }

    void setScanDurationMs(uint32_t ms) { _scanDurationMs = ms; }
    void setScanStuck(bool stuck) { _scanStuck = stuck; }
    void setApClients(uint8_t count) { _softApClients = count; }
    void setSoftApStartFailCount(int count) { _softApStartFailCount = count; }
    void simulateExternalConnect(const std::string& ssid,
                                 const std::string& ipStr = "192.168.1.99",
                                 int channel = 1,
                                 int rssi = -55) {
        _connecting = false;
        _connected = true;
        _dropInjected = false;
        _attemptSsid = ssid;
        _connectedIp = ipStr;
        _connectedChannel = channel;
        _connectedRssi = rssi;
    }
    void injectDrop() {
        _dropInjected = true;
        _connected = false;
        _connecting = false;
    }

    // Inspection helpers
    void clearCalls() { _calls.clear(); }
    const std::vector<WifiCallRecord>& calls() const { return _calls; }
    int countCalls(const std::string& fn) const {
        int c = 0;
        for (size_t i = 0; i < _calls.size(); i++) {
            if (_calls[i].fn == fn) c++;
        }
        return c;
    }
    bool isSoftApActive() const { return _softApActive; }
    const std::string& softApSsid() const { return _softApSsid; }
    const std::string& softApPass() const { return _softApPass; }
    int softApChannel() const { return _softApChannel; }

    // IWifiHal
    int status() const override {
        if (_dropInjected) return WL_DISCONNECTED;
        if (_connected) return WL_CONNECTED;
        if (_connecting) {
            uint32_t elapsed = _clock.millis() - _connectStartMs;
            if (_shouldSucceed && elapsed >= _connectDelayMs) {
                return WL_CONNECTED;
            }
            return WL_IDLE_STATUS;
        }
        return WL_DISCONNECTED;
    }

    void setAutoReconnect(bool autoReconnect) override {
        _calls.push_back(WifiCallRecord{"setAutoReconnect", "", "", autoReconnect ? 1 : 0});
    }

    void setHostname(const char* hostname) override {
        std::string h = hostname ? hostname : "";
        _calls.push_back(WifiCallRecord{"setHostname", h, "", 0});
    }

    bool begin(const char* ssid, const char* pass) override {
        std::string s = ssid ? ssid : "";
        std::string p = pass ? pass : "";
        _calls.push_back(WifiCallRecord{"begin", s, p, 0});
        _dropInjected = false;
        _connected = false;
        _connecting = true;
        _connectStartMs = _clock.millis();
        _attemptSsid = s;
        _shouldSucceed = false;
        _connectDelayMs = 0;

        for (size_t i = 0; i < _acceptCreds.size(); i++) {
            if (_acceptCreds[i].ssid == s && _acceptCreds[i].pass == p) {
                _shouldSucceed = true;
                _connectDelayMs = _acceptCreds[i].connectDelayMs;
                _connectedChannel = _acceptCreds[i].channel;
                _connectedIp = _acceptCreds[i].ipStr;
                _connectedRssi = _acceptCreds[i].rssi;
                if (_connectDelayMs == 0) {
                    _connected = true;
                    _connecting = false;
                }
                return true;
            }
        }
        return true;
    }

    void disconnect(bool wifioff = false) override {
        _calls.push_back(WifiCallRecord{"disconnect", "", "", wifioff ? 1 : 0});
        _connecting = false;
        _connected = false;
        _attemptSsid.clear();
    }

    int16_t scanStart(bool async = true, bool showHidden = false) override {
        _calls.push_back(WifiCallRecord{"scanStart", "", "", async ? 1 : 0});
        (void)showHidden;
        _scanRunning = true;
        _scanReady = false;
        _scanStartMs = _clock.millis();
        _latchedScan = _scanAps;
        if (!async && !_scanStuck) {
            _scanRunning = false;
            _scanReady = true;
            return (int16_t)_latchedScan.size();
        }
        return WIFI_SCAN_RUNNING;
    }

    int16_t scanComplete() override {
        if (_scanRunning && _scanStuck) return WIFI_SCAN_RUNNING;
        if (_scanReady) return (int16_t)_latchedScan.size();
        if (!_scanRunning) return WIFI_SCAN_FAILED;
        if ((uint32_t)(_clock.millis() - _scanStartMs) >= _scanDurationMs) {
            _scanRunning = false;
            _scanReady = true;
            return (int16_t)_latchedScan.size();
        }
        return WIFI_SCAN_RUNNING;
    }

    void scanDelete() override {
        _calls.push_back(WifiCallRecord{"scanDelete", "", "", 0});
        _scanRunning = false;
        _scanReady = false;
        _latchedScan.clear();
    }

    void scanStop() override {
        _calls.push_back(WifiCallRecord{"scanStop", "", "", 0});
        _scanRunning = false;
    }

    void scanSsid(int16_t idx, char* buf, size_t len) const override {
        if (!buf || len == 0) return;
        if (idx < 0 || (size_t)idx >= _latchedScan.size()) {
            buf[0] = '\0';
            return;
        }
        snprintf(buf, len, "%s", _latchedScan[idx].ssid.c_str());
    }

    int32_t scanRssi(int16_t idx) const override {
        if (idx < 0 || (size_t)idx >= _latchedScan.size()) return -120;
        return _latchedScan[idx].rssi;
    }

    bool scanIsOpen(int16_t idx) const override {
        if (idx < 0 || (size_t)idx >= _latchedScan.size()) return false;
        return _latchedScan[idx].isOpen;
    }

    int32_t scanChannel(int16_t idx) const override {
        if (idx < 0 || (size_t)idx >= _latchedScan.size()) return 1;
        return _latchedScan[idx].channel;
    }

    int getMode() const override { return _mode; }
    void setMode(int mode) override {
        _mode = mode;
        _calls.push_back(WifiCallRecord{"setMode", "", "", mode});
    }

    bool softApStart(const char* ssid, const char* pass, int channel) override {
        _calls.push_back(WifiCallRecord{"softApStart", ssid ? ssid : "", pass ? pass : "", channel});
        if (_softApStartFailCount != 0) {
            if (_softApStartFailCount > 0) _softApStartFailCount--;
            _softApActive = false;
            return false;
        }
        _softApActive = true;
        _softApSsid = ssid ? ssid : "";
        _softApPass = pass ? pass : "";
        _softApChannel = channel;
        return true;
    }

    void softApStop() override {
        _softApActive = false;
        _calls.push_back(WifiCallRecord{"softApStop", "", "", 0});
    }

    uint8_t softApStationNum() const override { return _softApClients; }
    int channel() const override { return (status() == WL_CONNECTED) ? _connectedChannel : 1; }
    int rssi() const override { return (status() == WL_CONNECTED) ? _connectedRssi : 0; }

    void localIpStr(char* buf, size_t len) const override {
        if (!buf || len == 0) return;
        snprintf(buf, len, "%s", (status() == WL_CONNECTED) ? _connectedIp.c_str() : "0.0.0.0");
    }

    void softApIpStr(char* buf, size_t len) const override {
        if (!buf || len == 0) return;
        snprintf(buf, len, "%s", _softApIp.c_str());
    }

    uint32_t softApIpU32() const override { return 0x0104A8C0UL; }

    void connectedSsid(char* buf, size_t len) const override {
        if (!buf || len == 0) return;
        snprintf(buf, len, "%s", (status() == WL_CONNECTED) ? _attemptSsid.c_str() : "");
    }

    void dnsStart() override {
        _calls.push_back(WifiCallRecord{"dnsStart", "", "", 0});
    }

    void dnsStop() override {
        _calls.push_back(WifiCallRecord{"dnsStop", "", "", 0});
    }

    void dnsProcess() override {}

private:
    const IClock& _clock;
    int _mode;

    bool _connecting;
    bool _connected;
    bool _dropInjected;
    uint32_t _connectStartMs;
    uint32_t _connectDelayMs;
    bool _shouldSucceed;
    std::string _attemptSsid;
    int _connectedChannel;
    int _connectedRssi;
    std::string _connectedIp;

    std::vector<FakeScanAp> _scanAps;
    std::vector<FakeScanAp> _latchedScan;
    std::vector<FakeAcceptCred> _acceptCreds;

    bool _scanRunning;
    bool _scanReady;
    bool _scanStuck;
    uint32_t _scanStartMs;
    uint32_t _scanDurationMs;

    bool _softApActive;
    std::string _softApSsid;
    std::string _softApPass;
    int _softApChannel;
    uint8_t _softApClients;
    std::string _softApIp;
    int _softApStartFailCount;

    std::vector<WifiCallRecord> _calls;
};

struct KvWriteRecord {
    std::string key;
    std::string type;
};

class FakeKvStore : public IKvStore {
public:
    enum ValType { VT_STR, VT_BOOL, VT_U8, VT_U16, VT_U32 };
    struct Entry {
        ValType type;
        std::string strVal;
        uint32_t numVal;
    };

    static const size_t MAX_NVS_KEY_LEN = 15;

    FakeKvStore() : _opened(false), _readOnly(false), _failBegin(false), _failPutAfter(-1) {}

    void setFailBegin(bool fail) { _failBegin = fail; }
    void setFailPutAfter(int allowCount) { _failPutAfter = allowCount; }

    bool begin(const char* ns, bool readOnly) override {
        if (_failBegin || !isValidKey(ns)) {
            _opened = false;
            return false;
        }
        std::string nsStr(ns);
        if (readOnly && _stores.find(nsStr) == _stores.end()) {
            _opened = false;
            return false;
        }
        _ns = nsStr;
        _opened = true;
        _readOnly = readOnly;
        if (!readOnly) {
            _stores[_ns];
        }
        return true;
    }

    void end() override { _opened = false; }

    bool isKey(const char* key) override {
        if (!_opened || !isValidKey(key)) return false;
        const std::map<std::string, Entry>& d = _stores[_ns];
        return d.find(key) != d.end();
    }

    bool clear() override {
        if (!_opened || _readOnly) return false;
        _stores[_ns].clear();
        return true;
    }

    size_t getString(const char* key, char* buf, size_t bufLen, const char* defaultVal) override {
        const char* def = defaultVal ? defaultVal : "";
        const char* src = def;
        if (_opened && isValidKey(key)) {
            const std::map<std::string, Entry>& d = _stores[_ns];
            std::map<std::string, Entry>::const_iterator it = d.find(key);
            if (it != d.end() && it->second.type == VT_STR) {
                src = it->second.strVal.c_str();
            }
        }
        if (!buf || bufLen == 0) return 0;
        snprintf(buf, bufLen, "%s", src);
        return strlen(buf);
    }

    size_t putString(const char* key, const char* value) override {
        if (!_opened || _readOnly || !isValidKey(key) || shouldFailPut()) return 0;
        const char* v = value ? value : "";
        _stores[_ns][key] = Entry{VT_STR, v, 0};
        _writes.push_back(KvWriteRecord{key, "str"});
        return strlen(v) + 1;
    }

    bool getBool(const char* key, bool defaultVal) override {
        if (!_opened || !isValidKey(key)) return defaultVal;
        const std::map<std::string, Entry>& d = _stores[_ns];
        std::map<std::string, Entry>::const_iterator it = d.find(key);
        if (it != d.end() && it->second.type == VT_BOOL) return it->second.numVal != 0;
        return defaultVal;
    }

    size_t putBool(const char* key, bool value) override {
        if (!_opened || _readOnly || !isValidKey(key) || shouldFailPut()) return 0;
        _stores[_ns][key] = Entry{VT_BOOL, "", value ? 1U : 0U};
        _writes.push_back(KvWriteRecord{key, "bool"});
        return 1;
    }

    uint8_t getUChar(const char* key, uint8_t defaultVal) override {
        if (!_opened || !isValidKey(key)) return defaultVal;
        const std::map<std::string, Entry>& d = _stores[_ns];
        std::map<std::string, Entry>::const_iterator it = d.find(key);
        if (it != d.end() && it->second.type == VT_U8) return (uint8_t)it->second.numVal;
        return defaultVal;
    }

    size_t putUChar(const char* key, uint8_t value) override {
        if (!_opened || _readOnly || !isValidKey(key) || shouldFailPut()) return 0;
        _stores[_ns][key] = Entry{VT_U8, "", value};
        _writes.push_back(KvWriteRecord{key, "u8"});
        return 1;
    }

    uint16_t getUShort(const char* key, uint16_t defaultVal) override {
        if (!_opened || !isValidKey(key)) return defaultVal;
        const std::map<std::string, Entry>& d = _stores[_ns];
        std::map<std::string, Entry>::const_iterator it = d.find(key);
        if (it != d.end() && it->second.type == VT_U16) return (uint16_t)it->second.numVal;
        return defaultVal;
    }

    size_t putUShort(const char* key, uint16_t value) override {
        if (!_opened || _readOnly || !isValidKey(key) || shouldFailPut()) return 0;
        _stores[_ns][key] = Entry{VT_U16, "", value};
        _writes.push_back(KvWriteRecord{key, "u16"});
        return 2;
    }

    uint32_t getULong(const char* key, uint32_t defaultVal) override {
        if (!_opened || !isValidKey(key)) return defaultVal;
        const std::map<std::string, Entry>& d = _stores[_ns];
        std::map<std::string, Entry>::const_iterator it = d.find(key);
        if (it != d.end() && it->second.type == VT_U32) return it->second.numVal;
        return defaultVal;
    }

    size_t putULong(const char* key, uint32_t value) override {
        if (!_opened || _readOnly || !isValidKey(key) || shouldFailPut()) return 0;
        _stores[_ns][key] = Entry{VT_U32, "", value};
        _writes.push_back(KvWriteRecord{key, "u32"});
        return 4;
    }

    // Test inspection helpers
    void clearWriteLog() { _writes.clear(); }
    const std::vector<KvWriteRecord>& writes() const { return _writes; }
    bool hasWriteForKey(const std::string& key) const {
        for (size_t i = 0; i < _writes.size(); i++) {
            if (_writes[i].key == key) return true;
        }
        return false;
    }

private:
    static bool isValidKey(const char* key) {
        if (!key || key[0] == '\0') return false;
        return strlen(key) <= MAX_NVS_KEY_LEN;
    }
    bool shouldFailPut() {
        if (_failPutAfter < 0) return false;
        if (_failPutAfter == 0) return true;
        _failPutAfter--;
        return false;
    }

    std::string _ns;
    bool _opened;
    bool _readOnly;
    bool _failBegin;
    int _failPutAfter;
    std::map<std::string, std::map<std::string, Entry> > _stores;
    std::vector<KvWriteRecord> _writes;
};

struct FakeGpio {
    static int& pinRef(uint8_t pin) {
        static int pins[64] = {0};
        return pins[pin % 64];
    }
    static void setPin(uint8_t pin, int level) { pinRef(pin) = level; }
    static int readPin(uint8_t pin) { return pinRef(pin); }
};

struct NotifyRecord {
    NotifyTarget target;
    std::string event;
    uint32_t durationS;
    std::string jsonBody;
};

class FakeAlertOutput : public IAlertOutput {
public:
    void queueNotification(NotifyTarget target, const char* event,
                           uint32_t durationS, const char* jsonPayload) override {
        notifications.push_back(NotifyRecord{
            target,
            event ? event : "",
            durationS,
            jsonPayload ? jsonPayload : ""
        });
    }
    size_t countForTarget(NotifyTarget target) const {
        size_t n = 0;
        for (size_t i = 0; i < notifications.size(); ++i) {
            if (notifications[i].target == target) ++n;
        }
        return n;
    }
    void clear() {
        notifications.clear();
    }
    std::vector<NotifyRecord> notifications;
};

class FakeSystemActions : public ISetupSystemActions {
public:
    FakeSystemActions(SetupApController* ap, DeviceConfig* trackedCfg)
        : _ap(ap), _trackedCfg(trackedCfg), applyCount(0), factoryResetCount(0), rebootCount(0), lastTriggerReconnect(false) {}

    void onApplyConfig(const DeviceConfig& config, bool triggerReconnect) override {
        applyCount++;
        lastTriggerReconnect = triggerReconnect;
        if (_trackedCfg) *_trackedCfg = config;
        if (_ap) _ap->onConfigApplied(config, triggerReconnect);
    }
    void onFactoryReset() override { factoryResetCount++; }
    void onReboot() override { rebootCount++; }

    SetupApController* _ap;
    DeviceConfig* _trackedCfg;
    int applyCount;
    int factoryResetCount;
    int rebootCount;
    bool lastTriggerReconnect;
};

inline DeviceConfig makeDefaultTestConfig() {
    DeviceConfig c;
    memset(&c, 0, sizeof(c));
    snprintf(c.device_name, sizeof(c.device_name), "%s", DEFAULT_DEVICE_NAME);
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

#endif // TEST_FAKES_H
