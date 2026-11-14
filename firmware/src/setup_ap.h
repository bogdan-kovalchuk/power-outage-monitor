#ifndef SETUP_AP_H
#define SETUP_AP_H

#include "config.h"
#include "hal.h"
#include "wifi_connector.h"

class ISetupSystemActions {
public:
    virtual ~ISetupSystemActions() {}
    virtual void onApplyConfig(const DeviceConfig& config, bool triggerReconnect) = 0;
    virtual void onFactoryReset() = 0;
    virtual void onReboot() = 0;
};

class SetupApController {
public:
    SetupApController(IWifiHal& wifi, const IClock& clock, WifiConnector& connector);

    void init(const DeviceConfig& config, uint64_t efuseMac, ISetupSystemActions* actions = nullptr);
    void update();

    void openSetupAp(uint32_t windowSeconds);
    void closeSetupAp();
    void notifyApHttpActivity();
    void applyWifiMode();
    void reconnectWiFi();

    void onConfigApplied(const DeviceConfig& config, bool triggerReconnect);

    // Web scan
    const char* pollWifiScanJsonPtr();
    size_t buildWebScanJson(int16_t scanCount, char* outBuf, size_t outCap);

    // Deferred HTTP actions
    void requestApplyConfig(const DeviceConfig& config, bool triggerReconnect);
    void requestOpenSetupAp(uint32_t windowSeconds);
    void requestCloseSetupAp();
    void requestReboot();
    void requestFactoryReset();
    void processDeferredActions();

    // Queries
    bool isApMode() const { return _apModeActive; }
    bool hasSavedNetworks() const;
    const char* getApSsid() const { return _effectiveApSsid; }
    uint8_t getApClientCount() const;
    uint32_t getApClosesInSeconds() const;
    void getApIpAddress(char* buf, size_t len) const;

private:
    IWifiHal& _wifi;
    const IClock& _clock;
    WifiConnector& _connector;
    ISetupSystemActions* _actions;

    DeviceConfig _config;
    uint64_t _efuseMac;
    char _effectiveApSsid[33];

    uint32_t _apFirstOpenedAtMs;
    uint32_t _apOpenedAtMs;
    uint32_t _apWindowDurationMs;
    uint32_t _apLastActivityMs;
    bool _apHadClient;
    bool _apModeActive;
    bool _apStartPending;
    uint32_t _apStartWindowS;
    uint32_t _apNextRetryMs;

    // Web scan state & cache
    bool _webScanPending;
    uint32_t _webScanStartMs;
    char _webScanResult[2560];
    uint32_t _webScanResultMs;
    bool _hasWebScanResult;
    bool _deferredConnectorNetworks;
    bool _deferredConnectorRestart;
    bool _deferredForceReconnect;

    // Prolonged STA-disconnect fallback AP tracking
    bool _fallbackApTriggered;
    bool _fallbackCooldownActive;
    uint32_t _fallbackClosedAtMs;

    // Deferred HTTP-triggered actions
    bool _pendingConfigApply;
    DeviceConfig _pendingConfig;
    bool _pendingTriggerReconnect;
    bool _pendingOpenAp;
    uint32_t _pendingOpenApWindowS;
    bool _pendingCloseAp;
    bool _pendingReboot;
    bool _pendingFactoryReset;
    uint32_t _pendingActionReqMs;

    void processWebScan();
    void flushDeferredConnectorActions();
    bool startSoftApRadio(bool isFreshOpen, uint32_t now, uint32_t windowSeconds);
};

#endif // SETUP_AP_H
