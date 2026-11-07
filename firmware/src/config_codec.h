#ifndef CONFIG_CODEC_H
#define CONFIG_CODEC_H

#include "config.h"
#include "hal.h"
#include <ArduinoJson.h>

bool isKnownConfigKey(const char* key);
bool isValidDeviceName(const char* s);

bool validateAndMergeConfig(
    const DeviceConfig& cur,
    JsonObjectConst obj,
    DeviceConfig& out,
    char* errField,
    size_t errFieldLen,
    char* errReason,
    size_t errReasonLen,
    char applied[][24],
    uint8_t& appliedCount,
    bool& wifiChanged
);

size_t buildConfigJsonBuf(const DeviceConfig& cfg, char* outBuf, size_t outCap);

void loadConfigFromKv(IKvStore& kv, DeviceConfig& cfg, uint64_t efuseMac);
bool saveConfigDiffToKv(IKvStore& kv, const DeviceConfig& cfg,
                        uint8_t* outWritten = nullptr,
                        char writtenKeys[][16] = nullptr, uint8_t maxKeys = 0);
uint8_t loadLastGoodWifiSlot(IKvStore& kv);
bool saveLastGoodWifiSlot(IKvStore& kv, uint8_t slot);
bool wipeConfigKv(IKvStore& kv);

#endif // CONFIG_CODEC_H
