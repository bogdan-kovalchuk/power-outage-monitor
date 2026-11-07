#ifndef HAL_ESP32_H
#define HAL_ESP32_H

#ifdef ARDUINO

#include "hal.h"
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

class Esp32Clock : public IClock {
public:
    uint32_t millis() const override;
};

class Esp32WifiHal : public IWifiHal {
public:
    Esp32WifiHal();

    int status() const override;
    void setAutoReconnect(bool autoReconnect) override;
    void setHostname(const char* hostname) override;
    bool begin(const char* ssid, const char* pass) override;
    void disconnect(bool wifioff = false) override;

    int16_t scanStart(bool async = true, bool showHidden = false) override;
    int16_t scanComplete() override;
    void scanDelete() override;
    void scanStop() override;
    void scanSsid(int16_t idx, char* buf, size_t len) const override;
    int32_t scanRssi(int16_t idx) const override;
    bool scanIsOpen(int16_t idx) const override;
    int32_t scanChannel(int16_t idx) const override;

    int getMode() const override;
    void setMode(int mode) override;
    bool softApStart(const char* ssid, const char* pass, int channel) override;
    void softApStop() override;
    uint8_t softApStationNum() const override;

    int channel() const override;
    int rssi() const override;
    void localIpStr(char* buf, size_t len) const override;
    void softApIpStr(char* buf, size_t len) const override;
    uint32_t softApIpU32() const override;
    void connectedSsid(char* buf, size_t len) const override;

    void dnsStart() override;
    void dnsStop() override;
    void dnsProcess() override;

private:
    DNSServer _dnsServer;
};

class Esp32KvStore : public IKvStore {
public:
    bool begin(const char* ns, bool readOnly) override;
    void end() override;
    bool isKey(const char* key) override;
    bool clear() override;

    size_t getString(const char* key, char* buf, size_t bufLen, const char* defaultVal) override;
    size_t putString(const char* key, const char* value) override;

    bool getBool(const char* key, bool defaultVal) override;
    size_t putBool(const char* key, bool value) override;

    uint8_t getUChar(const char* key, uint8_t defaultVal) override;
    size_t putUChar(const char* key, uint8_t value) override;

    uint16_t getUShort(const char* key, uint16_t defaultVal) override;
    size_t putUShort(const char* key, uint16_t value) override;

    uint32_t getULong(const char* key, uint32_t defaultVal) override;
    size_t putULong(const char* key, uint32_t value) override;

private:
    Preferences _prefs;
    bool _opened = false;
    bool _readOnly = false;
};

#endif // ARDUINO
#endif // HAL_ESP32_H
