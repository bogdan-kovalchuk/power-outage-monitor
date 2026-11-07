#ifdef ARDUINO

#include "ota_update.h"
#include "network_mgr.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_task_wdt.h>

namespace {

struct OtaControllerState {
    bool busy;
    bool uploadSeen;
    bool activeForThisRequest;
    bool errorOccurred;
    OtaGuardOutcome guardOutcome;
    char errorReason[96];
    // Peer of the connection that sent the first file part (see discardUnfinishedUpload)
    uint32_t uploadPeerIp;
    uint16_t uploadPeerPort;
};

OtaControllerState g_otaState = {
    false,
    false,
    false,
    false,
    { OTA_GUARD_ALLOW, 200, "" },
    { 0 },
    0,
    0
};

void setOtaError(OtaControllerState& st, const char* reason) {
    const char* msg = (reason && reason[0] != '\0') ? reason : "unknown error";
    strncpy(st.errorReason, msg, sizeof(st.errorReason) - 1);
    st.errorReason[sizeof(st.errorReason) - 1] = '\0';
}

// WebServer drops a request whose form it cannot finish parsing (for example too many fields)
// without calling the route handler or UPLOAD_FILE_ABORTED, which would leave the update running
// and the next request refused. WebServer closes every connection after one request, so upload
// state recorded for another peer belongs to such a request and is released here.
void discardUnfinishedUpload(WebServer& server) {
    if (!g_otaState.uploadSeen) return;
    WiFiClient client = server.client();
    if ((uint32_t)client.remoteIP() == g_otaState.uploadPeerIp &&
        client.remotePort() == g_otaState.uploadPeerPort) {
        return;
    }
    if (g_otaState.activeForThisRequest) {
        if (Update.isRunning()) {
            Update.abort();
        }
        g_otaState.busy = false;
        g_otaState.activeForThisRequest = false;
    }
    g_otaState.uploadSeen = false;
    g_otaState.errorOccurred = false;
    g_otaState.errorReason[0] = '\0';
    Serial.println("[OTA] Discarded an unfinished earlier update request");
}

OtaGuardOutcome evaluateServerOtaGuard(WebServer& server, const DeviceConfig& config, bool busy) {
    bool hasCsrf = server.hasHeader("X-Requested-With") &&
                   server.header("X-Requested-With") == "pm-ui";
    bool hasAdmin = (strlen(config.admin_pass) > 0);
    bool authOk = hasAdmin && server.authenticate("admin", config.admin_pass);
    return evaluateOtaGuard(hasCsrf, hasAdmin, authOk, busy);
}

void sendOtaFailureJson(WebServer& server, int statusCode, const char* reason) {
    JsonDocument doc;
    doc["error"] = String("update: ") + (reason ? reason : "unknown error");
    String out;
    serializeJson(doc, out);
    server.send(statusCode, "application/json", out);
}

} // namespace

void registerOtaUpdateRoute(WebServer& server,
                            const DeviceConfig& config,
                            NetworkManager& networkMgr,
                            OtaActivityCallback onActivity) {
    WebServer* srvPtr = &server;
    const DeviceConfig* cfgPtr = &config;
    NetworkManager* netPtr = &networkMgr;

    server.on(
        "/api/update",
        HTTP_POST,
        [srvPtr, cfgPtr, netPtr, onActivity]() {
            if (onActivity) onActivity();
            esp_task_wdt_reset();
            discardUnfinishedUpload(*srvPtr);

            if (!g_otaState.uploadSeen) {
                OtaGuardOutcome guard = evaluateServerOtaGuard(*srvPtr, *cfgPtr, g_otaState.busy);
                if (guard.result == OTA_GUARD_UNAUTHORIZED) {
                    srvPtr->requestAuthentication();
                    return;
                }
                if (guard.result != OTA_GUARD_ALLOW) {
                    srvPtr->send(guard.httpStatus, "application/json", guard.errorJson);
                    return;
                }
                srvPtr->send(400, "application/json", "{\"error\":\"update: missing firmware file\"}");
                return;
            }

            OtaGuardOutcome guard = g_otaState.guardOutcome;
            bool failed = g_otaState.errorOccurred;
            bool wasActive = g_otaState.activeForThisRequest;
            char reasonBuf[sizeof(g_otaState.errorReason)];
            strncpy(reasonBuf, g_otaState.errorReason, sizeof(reasonBuf) - 1);
            reasonBuf[sizeof(reasonBuf) - 1] = '\0';

            g_otaState.uploadSeen = false;
            g_otaState.activeForThisRequest = false;
            g_otaState.errorOccurred = false;
            g_otaState.errorReason[0] = '\0';

            if (guard.result == OTA_GUARD_UNAUTHORIZED) {
                srvPtr->requestAuthentication();
                return;
            }
            if (guard.result != OTA_GUARD_ALLOW) {
                srvPtr->send(guard.httpStatus, "application/json", guard.errorJson);
                return;
            }

            if (failed) {
                if (wasActive) {
                    g_otaState.busy = false;
                }
                sendOtaFailureJson(*srvPtr, 500, reasonBuf);
                return;
            }

            // The image becomes the boot image only here, once WebServer has parsed the whole
            // request: an extra form part or a disconnect after the file still finds the update
            // running and can abort it.
            if (!Update.end(true)) {
                const char* reason = normalizeOtaErrorReason(Update.errorString(), "verification failed");
                if (Update.isRunning()) {
                    Update.abort();
                }
                g_otaState.busy = false;
                Serial.printf("[OTA] Update failed at end: %s\n", reason);
                sendOtaFailureJson(*srvPtr, 500, reason);
                return;
            }
            Serial.println("[OTA] Update succeeded; reboot scheduled");

            // Keep busy true until the deferred reboot restarts the device
            srvPtr->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Firmware written, rebooting\"}");
            netPtr->requestReboot();
        },
        [srvPtr, cfgPtr, onActivity]() {
            esp_task_wdt_reset();
            HTTPUpload& upload = srvPtr->upload();

            if (upload.status == UPLOAD_FILE_START) {
                if (onActivity) onActivity();

                discardUnfinishedUpload(*srvPtr);

                if (g_otaState.uploadSeen) {
                    if (g_otaState.activeForThisRequest) {
                        if (Update.isRunning()) {
                            Update.abort();
                        }
                        g_otaState.busy = false;
                        g_otaState.activeForThisRequest = false;
                    }
                    g_otaState.errorOccurred = true;
                    setOtaError(g_otaState, "unexpected extra file field");
                    Serial.println("[OTA] Update failed: unexpected extra file field");
                    return;
                }

                WiFiClient client = srvPtr->client();
                g_otaState.uploadSeen = true;
                g_otaState.uploadPeerIp = (uint32_t)client.remoteIP();
                g_otaState.uploadPeerPort = client.remotePort();
                g_otaState.activeForThisRequest = false;
                g_otaState.errorOccurred = false;
                g_otaState.errorReason[0] = '\0';

                g_otaState.guardOutcome = evaluateServerOtaGuard(*srvPtr, *cfgPtr, g_otaState.busy);
                if (g_otaState.guardOutcome.result != OTA_GUARD_ALLOW) {
                    Serial.printf("[OTA] Update request rejected (status %u)\n",
                                  (unsigned)g_otaState.guardOutcome.httpStatus);
                    return;
                }

                if (!isOtaFirmwareField(upload.name.c_str())) {
                    g_otaState.errorOccurred = true;
                    setOtaError(g_otaState, "expected 'firmware' form field");
                    Serial.printf("[OTA] Update rejected: unexpected form field '%s'\n",
                                  upload.name.c_str());
                    return;
                }

                g_otaState.busy = true;
                g_otaState.activeForThisRequest = true;

                int contentLen = srvPtr->clientContentLength();
                if (contentLen > 0) {
                    Serial.printf("[OTA] Update started: file='%s', request_bytes=%d\n",
                                  upload.filename.c_str(), contentLen);
                } else {
                    Serial.printf("[OTA] Update started: file='%s', request_bytes=unknown\n",
                                  upload.filename.c_str());
                }

                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    const char* reason = normalizeOtaErrorReason(Update.errorString(), "begin failed");
                    setOtaError(g_otaState, reason);
                    g_otaState.errorOccurred = true;
                    if (Update.isRunning()) {
                        Update.abort();
                    }
                    Serial.printf("[OTA] Update failed at begin: %s\n", g_otaState.errorReason);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (!g_otaState.activeForThisRequest || g_otaState.errorOccurred) {
                    return;
                }
                if (upload.currentSize > 0) {
                    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                        const char* reason = normalizeOtaErrorReason(Update.errorString(), "flash write failed");
                        setOtaError(g_otaState, reason);
                        g_otaState.errorOccurred = true;
                        if (Update.isRunning()) {
                            Update.abort();
                        }
                        Serial.printf("[OTA] Update failed during write: %s\n", g_otaState.errorReason);
                    }
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (onActivity) onActivity();
                if (!g_otaState.activeForThisRequest || g_otaState.errorOccurred) {
                    return;
                }
                if (upload.totalSize == 0) {
                    if (Update.isRunning()) {
                        Update.abort();
                    }
                    setOtaError(g_otaState, "empty firmware file");
                    g_otaState.errorOccurred = true;
                    Serial.println("[OTA] Update failed: empty firmware file");
                    return;
                }
                Serial.printf("[OTA] Firmware file received (%u bytes)\n", (unsigned)upload.totalSize);
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                if (g_otaState.activeForThisRequest) {
                    if (Update.isRunning()) {
                        Update.abort();
                    }
                    Serial.printf("[OTA] Update aborted after %u bytes\n",
                                  (unsigned)upload.totalSize);
                    g_otaState.busy = false;
                    g_otaState.activeForThisRequest = false;
                }
                g_otaState.uploadSeen = false;
                g_otaState.errorOccurred = false;
                g_otaState.errorReason[0] = '\0';
            }
        }
    );
}

#endif // ARDUINO
