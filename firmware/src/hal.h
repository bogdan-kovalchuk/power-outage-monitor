#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#define HAL_LOG_PRINT(msg) Serial.println(msg)
#define HAL_LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#ifndef WL_CONNECTED
enum {
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6
};
#endif
#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif
#ifndef WIFI_SCAN_FAILED
#define WIFI_SCAN_FAILED (-2)
#endif
#ifndef WIFI_OFF
enum {
    WIFI_OFF = 0,
    WIFI_STA = 1,
    WIFI_AP = 2,
    WIFI_AP_STA = 3
};
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef HIGH
#define HIGH 1
#endif
#define HAL_LOG_PRINT(msg) ((void)0)
#define HAL_LOG_PRINTF(...) ((void)0)
#endif

class IClock {
public:
    virtual ~IClock() {}
    virtual uint32_t millis() const = 0;
};

class IWifiHal {
public:
    virtual ~IWifiHal() {}

    virtual int status() const = 0;
    virtual void setAutoReconnect(bool autoReconnect) = 0;
    virtual void setHostname(const char* hostname) = 0;
    virtual bool begin(const char* ssid, const char* pass) = 0;
    virtual void disconnect(bool wifioff = false) = 0;

    virtual int16_t scanStart(bool async = true, bool showHidden = false) = 0;
    virtual int16_t scanComplete() = 0;
    virtual void scanDelete() = 0;
    virtual void scanStop() = 0;
    virtual void scanSsid(int16_t idx, char* buf, size_t len) const = 0;
    virtual int32_t scanRssi(int16_t idx) const = 0;
    virtual bool scanIsOpen(int16_t idx) const = 0;
    virtual int32_t scanChannel(int16_t idx) const = 0;

    virtual int getMode() const = 0;
    virtual void setMode(int mode) = 0;
    virtual bool softApStart(const char* ssid, const char* pass, int channel) = 0;
    virtual void softApStop() = 0;
    virtual uint8_t softApStationNum() const = 0;

    virtual int channel() const = 0;
    virtual int rssi() const = 0;
    virtual void localIpStr(char* buf, size_t len) const = 0;
    virtual void softApIpStr(char* buf, size_t len) const = 0;
    virtual uint32_t softApIpU32() const = 0;
    virtual void connectedSsid(char* buf, size_t len) const = 0;

    virtual void dnsStart() = 0;
    virtual void dnsStop() = 0;
    virtual void dnsProcess() = 0;
};

class IKvStore {
public:
    virtual ~IKvStore() {}

    virtual bool begin(const char* ns, bool readOnly) = 0;
    virtual void end() = 0;
    virtual bool isKey(const char* key) = 0;
    virtual bool clear() = 0;

    virtual size_t getString(const char* key, char* buf, size_t bufLen, const char* defaultVal) = 0;
    virtual size_t putString(const char* key, const char* value) = 0;

    virtual bool getBool(const char* key, bool defaultVal) = 0;
    virtual size_t putBool(const char* key, bool value) = 0;

    virtual uint8_t getUChar(const char* key, uint8_t defaultVal) = 0;
    virtual size_t putUChar(const char* key, uint8_t value) = 0;

    virtual uint16_t getUShort(const char* key, uint16_t defaultVal) = 0;
    virtual size_t putUShort(const char* key, uint16_t value) = 0;

    virtual uint32_t getULong(const char* key, uint32_t defaultVal) = 0;
    virtual size_t putULong(const char* key, uint32_t value) = 0;
};

#endif // HAL_H
