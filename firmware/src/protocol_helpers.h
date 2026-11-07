#ifndef PROTOCOL_HELPERS_H
#define PROTOCOL_HELPERS_H

#include "config.h"
#include "power_sensor.h"
#include <ArduinoJson.h>
#include <atomic>
#include <stdint.h>
#include <stddef.h>

enum ButtonAction {
    BTN_ACT_NONE = 0,
    BTN_ACT_TOGGLE,
    BTN_ACT_TEST_RECONNECT,
    BTN_ACT_SETUP_AP,
    BTN_ACT_FACTORY_RESET
};

struct NotifyTargetResult {
    bool valid;
    char event[16];
    bool ok;
    int code;
    char reason[64];
    uint32_t recordedAtMs;
};

struct NotifyStatusSnapshot {
    bool valid;
    char event[16];
    bool ok;
    int code;
    char reason[64];
    uint32_t ageS;
};

struct StatusSnapshot {
    const char* deviceName;
    const char* fwVersion;
    GridState gridState;
    SystemMode mode;
    uint32_t outageDurationS;
    uint32_t snoozeRemainingS;
    uint32_t uptimeS;
    bool wifiConnected;
    const char* wifiSsid;
    int8_t wifiSlot;
    const char* wifiIp;
    int wifiRssi;
    bool apActive;
    const char* apSsid;
    const char* apIp;
    uint8_t apClients;
    uint32_t apClosesInS;
    bool adminProtected;
    uint16_t snoozeDefaultMin;
    uint32_t freeHeap;
    uint32_t ts;
    NotifyStatusSnapshot notifyLocal;
    NotifyStatusSnapshot notifyCloud;
};

class IAlertOutput {
public:
    virtual ~IAlertOutput() {}
    virtual void queueNotification(NotifyTarget target, const char* event,
                                   uint32_t durationS, const char* jsonPayload) = 0;
};

enum PendingNotifyEvent {
    PENDING_EVENT_NONE = 0,
    PENDING_EVENT_POWER_OFF,
    PENDING_EVENT_POWER_ON,
    PENDING_EVENT_ALARM_STOP
};

struct PendingTargetState {
    PendingNotifyEvent event;
    uint32_t durationS;
    AlertTrackingMode alertMode;
    SystemMode stopMode;
    uint32_t snoozeRemainingS;
    uint32_t seq;
};

class AlertRouter {
public:
    AlertRouter();

    void handlePowerOff(const char* deviceName, SystemMode mode, uint32_t durationS,
                        uint32_t uptimeS, uint32_t epochTs,
                        bool staConnected, IAlertOutput& out);

    void handlePowerOn(const char* deviceName, SystemMode mode, uint32_t durationS,
                       uint32_t uptimeS, AlertTrackingMode alertMode, uint32_t epochTs,
                       IAlertOutput& out, bool staConnected = true);

    bool handleEnterArmed(const char* deviceName, SystemMode previousMode, GridState gridState,
                          uint32_t outageDurationS, uint32_t uptimeS, uint32_t epochTs,
                          bool staConnected, IAlertOutput& out);

    bool handleAlarmStop(const char* deviceName, SystemMode previousMode, SystemMode nextMode,
                         uint32_t snoozeRemainingS, uint32_t uptimeS, uint32_t epochTs,
                         IAlertOutput& out, bool staConnected = true);

    bool flushPendingOnReconnect(const char* deviceName, bool isOnline, SystemMode mode,
                                 uint32_t outageDurationS, uint32_t uptimeS, uint32_t epochTs,
                                 bool staConnected, IAlertOutput& out);

    void markPendingTargetEvent(NotifyTarget target, const char* eventName,
                                uint32_t durationS = 0,
                                AlertTrackingMode alertMode = ALERT_OUTAGE_AND_RESTORE,
                                uint32_t eventSeq = 0);

    void handleAttemptsExhausted(NotifyTarget target, const char* eventName,
                                 uint32_t durationS, AlertTrackingMode alertMode,
                                 uint32_t eventSeq, bool staConnected);

    void markPendingTargetAlarmStop(SystemMode stopMode, uint32_t snoozeRemainingS = 0,
                                    uint32_t eventSeq = 0);

    void markPendingTargetOutage(NotifyTarget target) {
        markPendingTargetEvent(target, "POWER_OFF");
    }

    uint32_t getTargetStateSeq(NotifyTarget target) const;

    // False once a newer state event has been routed to the target, so a worker stops sending an
    // older one instead of delivering it next to the newer event; TEST_ALARM (seq 0) never goes stale.
    bool isStateSeqCurrent(NotifyTarget target, uint32_t eventSeq) const;

private:
    mutable std::atomic<bool> _locked;
    uint32_t _targetSeq[NOTIFY_TARGET_COUNT];
    PendingTargetState _pending[NOTIFY_TARGET_COUNT];
    SystemMode _lastStopMode;
    uint32_t _lastStopSnoozeS;
};

const char* systemModeStr(SystemMode mode);
bool canSnoozeMode(SystemMode mode);
uint16_t normalizeSnoozeMinutes(uint32_t requestedMin, uint16_t fallbackMin = DEFAULT_SNOOZE_MIN);

ButtonAction classifyPress(uint32_t pressDurationMs);
LedPattern selectLedPattern(GridState gridState, bool apActive, SystemMode mode, bool btnHeld, uint32_t btnHeldMs);
bool ledLevel(LedPattern pattern, uint32_t nowMs);

bool parsePositiveSnoozeInt(const char* s, uint32_t& outVal);
bool parseSnoozeHttpRequest(bool hasMinArg, const char* minArg,
                            bool hasDurationArg, const char* durationArg,
                            const char* jsonBody, uint16_t defaultMin,
                            uint16_t& outMin,
                            char* errField, size_t errFieldLen,
                            char* errReason, size_t errReasonLen);

void extractSchemeHost(const char* url, char* outBuf, size_t outCap);
bool extractUrlHost(const char* url, char* outBuf, size_t outCap);
bool isIpLiteral(const char* host);
bool isNtfyHost(const char* url);
bool hasNtfyTemplate(const char* eventName);
bool shouldUseNtfyFormat(const char* url, const char* eventName);
bool expandNotificationUrl(const char* tmpl, const char* eventName, char* outBuf, size_t outCap);
bool shouldNotifyTarget(NotifyTarget target, const char* targetUrl,
                        const char* eventName, AlertTrackingMode alertMode,
                        bool cloudEnabled);
bool shouldSendAlarmStop(SystemMode previousMode, SystemMode nextMode);
bool isStateNotifyEvent(const char* eventName);

size_t buildPowerOffJsonBuf(const char* deviceName, uint32_t durationS,
                            uint32_t uptimeS, uint32_t epochTs, char* outBuf, size_t outCap);
size_t buildPowerOnJsonBuf(const char* deviceName, SystemMode mode, uint32_t durationS,
                           uint32_t uptimeS, AlertTrackingMode alertMode, uint32_t epochTs,
                           char* outBuf, size_t outCap);
size_t buildAlarmStopJsonBuf(const char* deviceName, SystemMode mode, uint32_t snoozeRemainingS,
                             uint32_t uptimeS, uint32_t epochTs, char* outBuf, size_t outCap);
class NotifyStatusTracker {
public:
    NotifyStatusTracker();

    void resetAll();
    void clearTarget(NotifyTarget target);
    uint32_t getGeneration(NotifyTarget target) const;
    bool recordResult(NotifyTarget target, uint32_t generation, const char* eventName,
                      bool ok, int code, const char* reason, uint32_t nowMs);
    NotifyStatusSnapshot getSnapshot(NotifyTarget target, bool configured, uint32_t nowMs) const;

private:
    NotifyTargetResult _targets[NOTIFY_TARGET_COUNT];
    uint32_t _generations[NOTIFY_TARGET_COUNT];
};

struct NotifyPayload {
    char url[MAX_NOTIFY_URL_LEN + 1];
    char event[16];
    char device[MAX_DEVICE_NAME_LEN + 1];
    uint32_t duration_s;
    uint32_t urlGeneration;
    uint32_t stateSeq;
    char jsonBody[384];
};

enum NotifyPrepareResult {
    NOTIFY_PREP_SKIP = 0,
    NOTIFY_PREP_ERROR,
    NOTIFY_PREP_READY
};

void computeNotifyTargetChanges(const DeviceConfig& prev, const DeviceConfig& next,
                                bool& clearLocal, bool& clearCloud);
NotifyPrepareResult prepareNotifyPayload(const DeviceConfig& config, NotifyTarget target,
                                         uint32_t generation, const char* event,
                                         uint32_t durationS, const char* jsonPayload,
                                         NotifyPayload& outPayload,
                                         char* errReason, size_t errReasonCap);

void formatTlsErrorReason(int tlsCode, const char* mbedtlsText,
                          char* outBuf, size_t outCap);
void formatNotifyFailureReason(int code, const char* explicitReason,
                               char* outBuf, size_t outCap);

size_t buildTestAlarmJsonBuf(const char* deviceName, SystemMode mode, uint32_t uptimeS,
                             uint32_t epochTs, char* outBuf, size_t outCap);
size_t buildStatusJsonBuf(const StatusSnapshot& snap, char* outBuf, size_t outCap);

#endif // PROTOCOL_HELPERS_H
