#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdint.h>
#include <string.h>

enum OtaGuardResult {
    OTA_GUARD_ALLOW = 0,
    OTA_GUARD_NO_CSRF,
    OTA_GUARD_NO_ADMIN_PASS,
    OTA_GUARD_UNAUTHORIZED,
    OTA_GUARD_BUSY
};

struct OtaGuardOutcome {
    OtaGuardResult result;
    uint16_t httpStatus;
    const char* errorJson;
};

// Pure decision helper for POST /api/update request guards.
inline OtaGuardOutcome evaluateOtaGuard(bool hasCsrfHeader,
                                        bool hasAdminPass,
                                        bool authOk,
                                        bool updateInProgress) {
    if (!hasCsrfHeader) {
        OtaGuardOutcome out = {
            OTA_GUARD_NO_CSRF,
            403,
            "{\"error\":\"csrf: missing X-Requested-With header\"}"
        };
        return out;
    }
    if (!hasAdminPass) {
        OtaGuardOutcome out = {
            OTA_GUARD_NO_ADMIN_PASS,
            403,
            "{\"error\":\"update: set an admin password first\"}"
        };
        return out;
    }
    if (!authOk) {
        OtaGuardOutcome out = {
            OTA_GUARD_UNAUTHORIZED,
            401,
            ""
        };
        return out;
    }
    if (updateInProgress) {
        OtaGuardOutcome out = {
            OTA_GUARD_BUSY,
            409,
            "{\"error\":\"update: already in progress\"}"
        };
        return out;
    }
    OtaGuardOutcome out = { OTA_GUARD_ALLOW, 200, "" };
    return out;
}

inline bool isOtaFirmwareField(const char* fieldName) {
    return (fieldName != nullptr && strcmp(fieldName, "firmware") == 0);
}

inline const char* normalizeOtaErrorReason(const char* rawReason, const char* fallbackReason) {
    if (!rawReason || rawReason[0] == '\0' || strcmp(rawReason, "No Error") == 0) {
        return fallbackReason ? fallbackReason : "unknown error";
    }
    return rawReason;
}

#ifdef ARDUINO
#include <WebServer.h>
#include "config.h"

class NetworkManager;
typedef void (*OtaActivityCallback)();

void registerOtaUpdateRoute(WebServer& server,
                            const DeviceConfig& config,
                            NetworkManager& networkMgr,
                            OtaActivityCallback onActivity);
#endif

#endif // OTA_UPDATE_H
