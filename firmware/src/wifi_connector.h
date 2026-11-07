#ifndef WIFI_CONNECTOR_H
#define WIFI_CONNECTOR_H

#include "config.h"
#include "hal.h"
#include <atomic>

enum WifiConnState {
    WIFI_CONN_IDLE,
    WIFI_CONN_FAST_PATH,
    WIFI_CONN_SCANNING,
    WIFI_CONN_TRYING_CANDIDATE,
    WIFI_CONN_BACKOFF,
    WIFI_CONN_CONNECTED,
    WIFI_CONN_PAUSED_AP_CLIENT
};

class WifiConnector {
public:
    WifiConnector(IWifiHal& wifi, const IClock& clock);

    void init(const WifiCred slots[WIFI_SLOTS], uint8_t lastGoodSlot, const char* hostname);
    void setNetworks(const WifiCred slots[WIFI_SLOTS], const char* hostname, bool restartNow);
    void forceReconnect();
    void wakeFromBackoff();

    // Non-blocking update driven from NetworkManager::update()
    void update(bool apHasClients, bool webScanActive = false);

    WifiConnState getState() const { return _state; }
    bool isScanning() const { return _state == WIFI_CONN_SCANNING; }
    int8_t getConnectedSlot() const { return (_wifi.status() == WL_CONNECTED) ? _connectedSlot : -1; }
    void getConnectedSsid(char* buf, size_t len) const;
    uint32_t getDisconnectedDurationMs() const;

    // Returns true once when last-good slot changed and needs NVS persistence
    bool consumeLastGoodSlotChanged(uint8_t& outSlot);

private:
    IWifiHal& _wifi;
    const IClock& _clock;

    WifiCred _slots[WIFI_SLOTS];
    char _hostname[33];
    uint8_t _savedCount;
    uint8_t _lastGoodSlot;
    uint8_t _persistedLastGoodSlot;
    bool _lastGoodDirty;

    WifiConnState _state;
    int8_t _connectedSlot;
    int8_t _tryingSlot;
    int8_t _fastPathFailedSlot;
    uint32_t _attemptStartMs;
    uint32_t _scanStartMs;

    uint8_t _candidates[WIFI_SLOTS];
    bool _candidateVisible[WIFI_SLOTS];
    int16_t _candidateRssi[WIFI_SLOTS];
    uint8_t _candidateCount;
    uint8_t _candidateIdx;

    uint8_t _backoffIdx;
    uint32_t _backoffStartMs;
    uint32_t _backoffDurationMs;
    bool _disconnectedTimerRunning;
    uint32_t _disconnectedSinceMs;
    bool _pendingForceReconnect;
    bool _configAttemptPending;
    std::atomic<bool> _wakeFromBackoff;

    void countSaved();
    void startCycle(bool allowFastPath);
    void startScan();
    void trySlot(uint8_t slot, const char* reason);
    void tryCandidate(uint8_t idx);
    void enterBackoff();
    void buildCandidatesFromScan(int16_t scanCount, int8_t skipSlot);
};

#endif // WIFI_CONNECTOR_H
