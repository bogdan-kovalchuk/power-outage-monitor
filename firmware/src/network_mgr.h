#ifndef NETWORK_MGR_H
#define NETWORK_MGR_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "config_codec.h"
#include "hal_esp32.h"
#include "wifi_connector.h"
#include "setup_ap.h"
#include "protocol_helpers.h"

// Forward declaration of callbacks
typedef void (*RestartCallback)();

class NetworkManager;

struct NotifyWorkerContext {
    NetworkManager* mgr;
    NotifyTarget target;
    QueueHandle_t queue;
    TaskHandle_t task;
    const char* name;
    NotifyPayload rxPayload;
    // Written only by loop() when a state event evicts the oldest item from a full queue;
    // kept here instead of on the loop() stack because a payload is about 850 bytes.
    NotifyPayload evictedPayload;
};

class NetworkManager : private ISetupSystemActions, private IAlertOutput {
public:
    NetworkManager();

    // Initialization
    void init(const DeviceConfig& config, IKvStore& kvStore);

    // Periodic processing (deferred actions, web scan, Wi-Fi connector, mDNS, pending alerts)
    void update(GridState gridState, SystemMode mode, uint32_t outageDurationS);

    // Event dispatch methods
    void sendPowerOffAlert(SystemMode mode, uint32_t durationS);
    void sendPowerOnAlert(SystemMode mode, uint32_t durationS, AlertTrackingMode alertMode);
    void handleEnterArmed(SystemMode previousMode, GridState gridState, uint32_t outageDurationS);
    void sendAlarmStop(SystemMode previousMode, SystemMode nextMode, uint32_t snoozeRemainingS = 0);
    void sendTestAlert(SystemMode mode);

    // Lifecycle callback registration
    void onRestartRequested(RestartCallback cb) { _cbRestart = cb; }
    void shutdownForRestart();

    // Helpers
    bool isWiFiConnected() const;
    NotifyStatusSnapshot getNotifyStatus(NotifyTarget target) const;
    bool isApMode() const { return _setupAp.isApMode(); }
    bool isApClientIp(uint32_t localIp) const { return isApMode() && (localIp == _wifiHal.softApIpU32()); }
    bool hasSavedNetworks() const { return _setupAp.hasSavedNetworks(); }
    int8_t getWifiSlot() const { return _connector.getConnectedSlot(); }
    String getConnectedSsid() const;
    String getApSsid() const { return String(_setupAp.getApSsid()); }
    uint8_t getApClientCount() const { return _setupAp.getApClientCount(); }
    uint32_t getApClosesInSeconds() const { return _setupAp.getApClosesInSeconds(); }
    String getIpAddress() const;
    String getApIpAddress() const;
    int getRssi() const;
    uint32_t getEpochTime();
    void applyConfigImmediate(const DeviceConfig& config);
    void reconnectWiFi();
    void openSetupAp(uint32_t windowSeconds) { _setupAp.openSetupAp(windowSeconds); }
    void notifyApHttpActivity() { _setupAp.notifyApHttpActivity(); }
    String pollWifiScanJson();

    // Deferred disruptive actions (allow HTTP response TCP flush first)
    void requestApplyConfig(const DeviceConfig& config, bool triggerReconnect) { _setupAp.requestApplyConfig(config, triggerReconnect); }
    void requestOpenSetupAp(uint32_t windowSeconds) { _setupAp.requestOpenSetupAp(windowSeconds); }
    void requestCloseSetupAp() { _setupAp.requestCloseSetupAp(); }
    void requestReboot() { _setupAp.requestReboot(); }
    void requestFactoryReset() { _setupAp.requestFactoryReset(); }

private:
    DeviceConfig _config;
    IKvStore* _kvStore;
    Esp32Clock _clock;
    Esp32WifiHal _wifiHal;
    WifiConnector _connector;
    SetupApController _setupAp;
    AlertRouter _alertRouter;
    NotifyWorkerContext _notifyWorkers[NOTIFY_TARGET_COUNT];
    NotifyStatusTracker _notifyTracker;
    mutable SemaphoreHandle_t _notifyStatusMutex;
    SemaphoreHandle_t _dnsMutex;
    SemaphoreHandle_t _tlsMutex;
    bool _mdnsStarted;
    uint32_t _lastMdnsAttemptMs;

    RestartCallback _cbRestart;

    // ISetupSystemActions
    void onApplyConfig(const DeviceConfig& config, bool triggerReconnect) override;
    void onFactoryReset() override;
    void onReboot() override;

    // IAlertOutput
    void queueNotification(NotifyTarget target, const char* event,
                           uint32_t durationS, const char* jsonPayload) override;

    void triggerNotification(NotifyTarget target, const char* event, uint32_t durationS, const char* jsonPayload);
    const char* targetUrl(NotifyTarget target) const;
    void ensureMdnsStarted();
    void stopMdns();
    uint32_t getNotifyGeneration(NotifyTarget target) const;
    void recordNotifyResult(NotifyTarget target, uint32_t generation, const char* event, bool ok, int code, const char* reason);
    void clearNotifyTarget(NotifyTarget target);
    static void delayWithWdtReset(uint32_t totalMs);
    static int sendNotificationAttempt(const NotifyPayload& payload, const char* schemeHost,
                                       SemaphoreHandle_t dnsMutex, SemaphoreHandle_t tlsMutex,
                                       char* errReason, size_t errReasonCap);
    static void notifyWorkerTask(void* pvParameters);
};

#endif // NETWORK_MGR_H
