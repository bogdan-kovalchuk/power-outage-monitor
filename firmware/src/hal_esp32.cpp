#ifdef ARDUINO

#include "hal_esp32.h"
#include <esp_wifi.h>
#include <string.h>

static void copyCStr(char* dst, size_t dstLen, const char* src) {
    if (!dst || dstLen == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstLen - 1);
    dst[dstLen - 1] = '\0';
}

uint32_t Esp32Clock::millis() const {
    return ::millis();
}

Esp32WifiHal::Esp32WifiHal() {}

int Esp32WifiHal::status() const {
    return (int)WiFi.status();
}

void Esp32WifiHal::setAutoReconnect(bool autoReconnect) {
    WiFi.setAutoReconnect(autoReconnect);
}

void Esp32WifiHal::setHostname(const char* hostname) {
    WiFi.setHostname(hostname);
}

bool Esp32WifiHal::begin(const char* ssid, const char* pass) {
    return WiFi.begin(ssid, pass) != WL_CONNECT_FAILED;
}

void Esp32WifiHal::disconnect(bool wifioff) {
    WiFi.disconnect(wifioff);
}

int16_t Esp32WifiHal::scanStart(bool async, bool showHidden) {
    return WiFi.scanNetworks(async, showHidden);
}

int16_t Esp32WifiHal::scanComplete() {
    return WiFi.scanComplete();
}

void Esp32WifiHal::scanDelete() {
    WiFi.scanDelete();
}

void Esp32WifiHal::scanStop() {
    esp_wifi_scan_stop();
}

void Esp32WifiHal::scanSsid(int16_t idx, char* buf, size_t len) const {
    String s = WiFi.SSID(idx);
    copyCStr(buf, len, s.c_str());
}

int32_t Esp32WifiHal::scanRssi(int16_t idx) const {
    return WiFi.RSSI(idx);
}

bool Esp32WifiHal::scanIsOpen(int16_t idx) const {
    return WiFi.encryptionType(idx) == WIFI_AUTH_OPEN;
}

int32_t Esp32WifiHal::scanChannel(int16_t idx) const {
    return WiFi.channel(idx);
}

int Esp32WifiHal::getMode() const {
    return (int)WiFi.getMode();
}

void Esp32WifiHal::setMode(int mode) {
    WiFi.mode((wifi_mode_t)mode);
}

bool Esp32WifiHal::softApStart(const char* ssid, const char* pass, int channel) {
    IPAddress apIP(SETUP_AP_IP_O1, SETUP_AP_IP_O2, SETUP_AP_IP_O3, SETUP_AP_IP_O4);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    return WiFi.softAP(ssid, pass, channel);
}

void Esp32WifiHal::softApStop() {
    WiFi.softAPdisconnect(true);
}

uint8_t Esp32WifiHal::softApStationNum() const {
    return (uint8_t)WiFi.softAPgetStationNum();
}

int Esp32WifiHal::channel() const {
    return WiFi.channel();
}

int Esp32WifiHal::rssi() const {
    return WiFi.RSSI();
}

void Esp32WifiHal::localIpStr(char* buf, size_t len) const {
    copyCStr(buf, len, WiFi.localIP().toString().c_str());
}

void Esp32WifiHal::softApIpStr(char* buf, size_t len) const {
    copyCStr(buf, len, WiFi.softAPIP().toString().c_str());
}

uint32_t Esp32WifiHal::softApIpU32() const {
    return (uint32_t)WiFi.softAPIP();
}

void Esp32WifiHal::connectedSsid(char* buf, size_t len) const {
    String s = WiFi.SSID();
    copyCStr(buf, len, s.c_str());
}

void Esp32WifiHal::dnsStart() {
    _dnsServer.start(DNS_PORT, "*", IPAddress(SETUP_AP_IP_O1, SETUP_AP_IP_O2, SETUP_AP_IP_O3, SETUP_AP_IP_O4));
}

void Esp32WifiHal::dnsStop() {
    _dnsServer.stop();
}

void Esp32WifiHal::dnsProcess() {
    _dnsServer.processNextRequest();
}

bool Esp32KvStore::begin(const char* ns, bool readOnly) {
    _opened = _prefs.begin(ns, readOnly);
    _readOnly = readOnly;
    return _opened;
}

void Esp32KvStore::end() {
    _prefs.end();
    _opened = false;
    _readOnly = false;
}

bool Esp32KvStore::isKey(const char* key) {
    return _prefs.isKey(key);
}

bool Esp32KvStore::clear() {
    return _prefs.clear();
}

size_t Esp32KvStore::getString(const char* key, char* buf, size_t bufLen, const char* defaultVal) {
    if (!buf || bufLen == 0) return 0;
    if (!key || !_prefs.isKey(key)) {
        copyCStr(buf, bufLen, defaultVal ? defaultVal : "");
        return strlen(buf);
    }
    String v = _prefs.getString(key, defaultVal ? defaultVal : "");
    copyCStr(buf, bufLen, v.c_str());
    return strlen(buf);
}

size_t Esp32KvStore::putString(const char* key, const char* value) {
    if (!_opened || _readOnly || !key) return 0;
    const char* v = value ? value : "";
    if (v[0] == '\0') {
        _prefs.putString(key, "");
        return (_prefs.isKey(key) && _prefs.getString(key, "__err__").length() == 0) ? 1 : 0;
    }
    return _prefs.putString(key, v);
}

bool Esp32KvStore::getBool(const char* key, bool defaultVal) {
    if (!key || !_prefs.isKey(key)) return defaultVal;
    return _prefs.getBool(key, defaultVal);
}

size_t Esp32KvStore::putBool(const char* key, bool value) {
    return _prefs.putBool(key, value);
}

uint8_t Esp32KvStore::getUChar(const char* key, uint8_t defaultVal) {
    if (!key || !_prefs.isKey(key)) return defaultVal;
    return _prefs.getUChar(key, defaultVal);
}

size_t Esp32KvStore::putUChar(const char* key, uint8_t value) {
    return _prefs.putUChar(key, value);
}

uint16_t Esp32KvStore::getUShort(const char* key, uint16_t defaultVal) {
    if (!key || !_prefs.isKey(key)) return defaultVal;
    return _prefs.getUShort(key, defaultVal);
}

size_t Esp32KvStore::putUShort(const char* key, uint16_t value) {
    return _prefs.putUShort(key, value);
}

uint32_t Esp32KvStore::getULong(const char* key, uint32_t defaultVal) {
    if (!key || !_prefs.isKey(key)) return defaultVal;
    return _prefs.getULong(key, defaultVal);
}

size_t Esp32KvStore::putULong(const char* key, uint32_t value) {
    return _prefs.putULong(key, value);
}

#endif // ARDUINO
