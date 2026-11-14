#include "network_mgr.h"

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

NetworkManager::NetworkManager()
    : _kvStore(nullptr),
      _connector(_wifiHal, _clock),
      _setupAp(_wifiHal, _clock, _connector),
      _notifyStatusMutex(nullptr),
      _dnsMutex(nullptr),
      _tlsMutex(nullptr),
      _mdnsStarted(false),
      _lastMdnsAttemptMs(0),
      _cbRestart(nullptr) {
    memset(&_config, 0, sizeof(_config));
    memset(_notifyWorkers, 0, sizeof(_notifyWorkers));
    _notifyWorkers[NOTIFY_TARGET_LOCAL].mgr = this;
    _notifyWorkers[NOTIFY_TARGET_LOCAL].target = NOTIFY_TARGET_LOCAL;
    _notifyWorkers[NOTIFY_TARGET_LOCAL].name = "Local";
    _notifyWorkers[NOTIFY_TARGET_CLOUD].mgr = this;
    _notifyWorkers[NOTIFY_TARGET_CLOUD].target = NOTIFY_TARGET_CLOUD;
    _notifyWorkers[NOTIFY_TARGET_CLOUD].name = "Cloud";
}

void NetworkManager::init(const DeviceConfig& config, IKvStore& kvStore) {
    _config = config;
    _kvStore = &kvStore;

    if (!_notifyStatusMutex) {
        _notifyStatusMutex = xSemaphoreCreateMutex();
    }
    if (!_dnsMutex) {
        _dnsMutex = xSemaphoreCreateMutex();
    }
    if (!_tlsMutex) {
        _tlsMutex = xSemaphoreCreateMutex();
    }
    clearNotifyTarget(NOTIFY_TARGET_LOCAL);
    clearNotifyTarget(NOTIFY_TARGET_CLOUD);

    static const char* const TASK_NAMES[NOTIFY_TARGET_COUNT] = {
        "NotifyLocal",
        "NotifyCloud"
    };
    for (int i = 0; i < NOTIFY_TARGET_COUNT; ++i) {
        if (!_notifyWorkers[i].queue) {
            _notifyWorkers[i].queue = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(NotifyPayload));
        }
        if (_notifyWorkers[i].queue && !_notifyWorkers[i].task) {
            xTaskCreatePinnedToCore(
                notifyWorkerTask,
                TASK_NAMES[i],
                WEBHOOK_STACK_SIZE, // 8192 bytes for HTTP / mbedTLS handshake
                (void*)&_notifyWorkers[i],
                1,
                &_notifyWorkers[i].task,
                0 // Pin to Core 0 to leave Core 1 free for Arduino loop
            );
        }
    }

    uint8_t lastGoodSlot = loadLastGoodWifiSlot(*_kvStore);

    WiFi.persistent(false);
    _wifiHal.setHostname(_config.device_name);

    _setupAp.init(_config, ESP.getEfuseMac(), this);
    _connector.init(_config.wifi, lastGoodSlot, _config.device_name);

    // Initialize NTP time client (UTC)
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

String NetworkManager::pollWifiScanJson() {
    return String(_setupAp.pollWifiScanJsonPtr());
}

void NetworkManager::reconnectWiFi() {
    if (hasSavedNetworks()) {
        stopMdns();
    }
    _setupAp.reconnectWiFi();
}

void NetworkManager::onApplyConfig(const DeviceConfig& config, bool triggerReconnect) {
    _setupAp.onConfigApplied(config, triggerReconnect);
}

void NetworkManager::shutdownForRestart() {
    stopMdns();
    _wifiHal.dnsStop();
    WiFi.disconnect(true, true);
    _wifiHal.setMode(WIFI_OFF);
}

void NetworkManager::onFactoryReset() {
    if (_kvStore) {
        wipeConfigKv(*_kvStore);
    }
    if (_cbRestart) {
        _cbRestart();
    } else {
        shutdownForRestart();
        delay(100);
        ESP.restart();
    }
}

void NetworkManager::onReboot() {
    if (_cbRestart) {
        _cbRestart();
    } else {
        shutdownForRestart();
        delay(100);
        ESP.restart();
    }
}

void NetworkManager::applyConfigImmediate(const DeviceConfig& config) {
    bool nameChanged = (strcmp(_config.device_name, config.device_name) != 0);
    bool clearLocal = false;
    bool clearCloud = false;
    computeNotifyTargetChanges(_config, config, clearLocal, clearCloud);

    _config = config;
    if (clearLocal) {
        clearNotifyTarget(NOTIFY_TARGET_LOCAL);
    }
    if (clearCloud) {
        clearNotifyTarget(NOTIFY_TARGET_CLOUD);
    }

    if (nameChanged) {
        _wifiHal.setHostname(_config.device_name);
        stopMdns();
        _lastMdnsAttemptMs = 0; // allow immediate mDNS restart on live name change
        if (_wifiHal.status() == WL_CONNECTED) {
            ensureMdnsStarted();
        }
    }
}

bool NetworkManager::isWiFiConnected() const {
    return (_wifiHal.status() == WL_CONNECTED);
}

String NetworkManager::getConnectedSsid() const {
    char buf[33];
    _connector.getConnectedSsid(buf, sizeof(buf));
    return String(buf);
}

String NetworkManager::getIpAddress() const {
    // Report STA IP only (empty when disconnected)
    if (_wifiHal.status() == WL_CONNECTED) {
        char buf[20];
        _wifiHal.localIpStr(buf, sizeof(buf));
        return String(buf);
    }
    return String("");
}

String NetworkManager::getApIpAddress() const {
    char buf[32];
    _setupAp.getApIpAddress(buf, sizeof(buf));
    return String(buf);
}

int NetworkManager::getRssi() const {
    if (isWiFiConnected()) {
        return _wifiHal.rssi();
    }
    return -100;
}

uint32_t NetworkManager::getEpochTime() {
    time_t now = 0;
    time(&now);
    if (now > 100000000) {
        return (uint32_t)now;
    }
    return 0;
}

void NetworkManager::update(GridState gridState, SystemMode mode, uint32_t outageDurationS) {
    _setupAp.update();

    // Persist w_last only when the successfully connected slot changes
    uint8_t newLastSlot = 255;
    if (_connector.consumeLastGoodSlotChanged(newLastSlot) && _kvStore) {
        saveLastGoodWifiSlot(*_kvStore, newLastSlot);
    }

    if (_wifiHal.status() == WL_CONNECTED) {
        // Start mDNS responder once connected (throttled on failure)
        ensureMdnsStarted();

        // Router UPS Glitch Shield: Re-emit pending outage alert if grid is still offline
        _alertRouter.flushPendingOnReconnect(
            _config.device_name,
            (gridState == GRID_ONLINE),
            mode,
            outageDurationS,
            millis() / 1000UL,
            getEpochTime(),
            true,
            *this
        );
    } else {
        stopMdns();
    }
}

void NetworkManager::ensureMdnsStarted() {
    if (_mdnsStarted) return;
    uint32_t now = millis();
    // Throttle mDNS retries so repeated failures do not flood the responder
    if (_lastMdnsAttemptMs != 0 && (now - _lastMdnsAttemptMs < MDNS_RETRY_MS)) {
        return;
    }
    _lastMdnsAttemptMs = now;
    MDNS.end();
    if (MDNS.begin(_config.device_name)) {
        MDNS.addService("http", "tcp", 80);
        _mdnsStarted = true;
        Serial.printf("[mDNS] Responder active at http://%s.local\n", _config.device_name);
    }
}

void NetworkManager::stopMdns() {
    if (_mdnsStarted) {
        MDNS.end();
        _mdnsStarted = false;
    }
}

void NetworkManager::queueNotification(NotifyTarget target, const char* event,
                                       uint32_t durationS, const char* jsonPayload) {
    triggerNotification(target, event, durationS, jsonPayload);
}

void NetworkManager::sendPowerOffAlert(SystemMode mode, uint32_t durationS) {
    if (mode == MODE_ARMED && !isWiFiConnected()) {
        _connector.wakeFromBackoff();
    }
    _alertRouter.handlePowerOff(
        _config.device_name,
        mode,
        durationS,
        millis() / 1000UL,
        getEpochTime(),
        _wifiHal.status() == WL_CONNECTED,
        *this
    );
}

void NetworkManager::sendPowerOnAlert(SystemMode mode, uint32_t durationS, AlertTrackingMode alertMode) {
    Serial.printf("[Alert] POWER_ON Restored (duration: %us, alert_mode: %s)\n",
                  durationS,
                  (alertMode == ALERT_OUTAGE_ONLY) ? "OUTAGE_ONLY" : "OUTAGE_AND_RESTORE");
    if (!isWiFiConnected()) {
        _connector.wakeFromBackoff();
    }
    _alertRouter.handlePowerOn(
        _config.device_name,
        mode,
        durationS,
        millis() / 1000UL,
        alertMode,
        getEpochTime(),
        *this,
        _wifiHal.status() == WL_CONNECTED
    );
}

void NetworkManager::handleEnterArmed(SystemMode previousMode, GridState gridState, uint32_t outageDurationS) {
    if (previousMode != MODE_ARMED && gridState == GRID_OFFLINE && !isWiFiConnected()) {
        _connector.wakeFromBackoff();
    }
    _alertRouter.handleEnterArmed(
        _config.device_name,
        previousMode,
        gridState,
        outageDurationS,
        millis() / 1000UL,
        getEpochTime(),
        _wifiHal.status() == WL_CONNECTED,
        *this
    );
}

void NetworkManager::sendAlarmStop(SystemMode previousMode, SystemMode nextMode, uint32_t snoozeRemainingS) {
    if (shouldSendAlarmStop(previousMode, nextMode) && !isWiFiConnected()) {
        _connector.wakeFromBackoff();
    }
    _alertRouter.handleAlarmStop(
        _config.device_name,
        previousMode,
        nextMode,
        snoozeRemainingS,
        millis() / 1000UL,
        getEpochTime(),
        *this,
        _wifiHal.status() == WL_CONNECTED
    );
}

void NetworkManager::sendTestAlert(SystemMode mode) {
    char jsonOut[384];
    buildTestAlarmJsonBuf(_config.device_name, mode, millis() / 1000UL, getEpochTime(), jsonOut, sizeof(jsonOut));

    Serial.println("[Alert] TEST_ALARM dispatched");
    triggerNotification(NOTIFY_TARGET_LOCAL, "TEST_ALARM", 0, jsonOut);
    triggerNotification(NOTIFY_TARGET_CLOUD, "TEST_ALARM", 0, jsonOut);
}

const char* NetworkManager::targetUrl(NotifyTarget target) const {
    if (target == NOTIFY_TARGET_LOCAL) return _config.local_url;
    if (target == NOTIFY_TARGET_CLOUD) return _config.webhook_url;
    return "";
}

void NetworkManager::triggerNotification(NotifyTarget target, const char* event,
                                         uint32_t durationS, const char* jsonPayload) {
    if ((int)target < 0 || (int)target >= NOTIFY_TARGET_COUNT) return;
    NotifyWorkerContext& worker = _notifyWorkers[target];
    if (!worker.queue) return;

    const char* rawUrl = targetUrl(target);
    uint32_t generation = getNotifyGeneration(target);
    NotifyPayload payload;
    char errReason[64] = { 0 };
    NotifyPrepareResult prep = prepareNotifyPayload(
        _config, target, generation, event, durationS, jsonPayload,
        payload, errReason, sizeof(errReason));
    if (prep == NOTIFY_PREP_SKIP) return;
    if (prep == NOTIFY_PREP_ERROR) {
        char schemeHost[128];
        extractSchemeHost(rawUrl, schemeHost, sizeof(schemeHost));
        Serial.printf("[Notify/%s] Dropping event '%s' to %s: expanded URL exceeds %u bytes\n",
                      worker.name, event, schemeHost, (unsigned)MAX_NOTIFY_URL_LEN);
        recordNotifyResult(target, generation, event, false, 0, errReason);
        return;
    }

    bool isStateEvent = isStateNotifyEvent(event);
    payload.stateSeq = isStateEvent ? _alertRouter.getTargetStateSeq(target) : 0;
    if (!isWiFiConnected()) {
        _connector.wakeFromBackoff();
    }

    // Push to the target's worker queue without blocking loop(). A full queue means the worker is
    // retrying a dead endpoint; a state event must still get through, so it evicts the oldest
    // queued item, while a TEST_ALARM is dropped. loop() is the only producer, so the slot freed
    // by the eviction (or by the worker meanwhile) cannot be taken before the second send.
    bool queued = (xQueueSend(worker.queue, &payload, 0) == pdPASS);
    if (!queued && isStateEvent) {
        if (xQueueReceive(worker.queue, &worker.evictedPayload, 0) == pdPASS) {
            char evictedHost[128];
            extractSchemeHost(worker.evictedPayload.url, evictedHost, sizeof(evictedHost));
            Serial.printf("[Notify/%s] Queue full, discarded oldest event '%s' for %s to queue '%s'\n",
                          worker.name, worker.evictedPayload.event, evictedHost, payload.event);
        }
        queued = (xQueueSend(worker.queue, &payload, 0) == pdPASS);
    }
    if (!queued) {
        char schemeHost[128];
        extractSchemeHost(payload.url, schemeHost, sizeof(schemeHost));
        Serial.printf("[Notify/%s] Warning: Queue full, dropped event '%s' for %s\n",
                      worker.name, payload.event, schemeHost);
        recordNotifyResult(target, generation, payload.event, false, 0, "Queue full");
    }
}

uint32_t NetworkManager::getNotifyGeneration(NotifyTarget target) const {
    if (_notifyStatusMutex) {
        xSemaphoreTake(_notifyStatusMutex, portMAX_DELAY);
    }
    uint32_t gen = _notifyTracker.getGeneration(target);
    if (_notifyStatusMutex) {
        xSemaphoreGive(_notifyStatusMutex);
    }
    return gen;
}

void NetworkManager::recordNotifyResult(NotifyTarget target, uint32_t generation, const char* event,
                                        bool ok, int code, const char* reason) {
    if (_notifyStatusMutex) {
        xSemaphoreTake(_notifyStatusMutex, portMAX_DELAY);
    }
    _notifyTracker.recordResult(target, generation, event, ok, code, reason, millis());
    if (_notifyStatusMutex) {
        xSemaphoreGive(_notifyStatusMutex);
    }
}

void NetworkManager::clearNotifyTarget(NotifyTarget target) {
    if (_notifyStatusMutex) {
        xSemaphoreTake(_notifyStatusMutex, portMAX_DELAY);
    }
    _notifyTracker.clearTarget(target);
    if (_notifyStatusMutex) {
        xSemaphoreGive(_notifyStatusMutex);
    }
}

NotifyStatusSnapshot NetworkManager::getNotifyStatus(NotifyTarget target) const {
    bool configured = false;
    if (target == NOTIFY_TARGET_LOCAL) {
        configured = (_config.local_url[0] != '\0');
    } else if (target == NOTIFY_TARGET_CLOUD) {
        configured = (_config.cloud_enabled && _config.webhook_url[0] != '\0');
    }
    if (_notifyStatusMutex) {
        xSemaphoreTake(_notifyStatusMutex, portMAX_DELAY);
    }
    NotifyStatusSnapshot snap = _notifyTracker.getSnapshot(target, configured, millis());
    if (_notifyStatusMutex) {
        xSemaphoreGive(_notifyStatusMutex);
    }
    return snap;
}

void NetworkManager::delayWithWdtReset(uint32_t totalMs) {
    uint32_t remaining = totalMs;
    while (remaining > 0) {
        uint32_t step = (remaining > WEBHOOK_WIFI_POLL_MS) ? WEBHOOK_WIFI_POLL_MS : remaining;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(step));
        esp_task_wdt_reset();
        remaining -= step;
    }
}

struct ScopedWdtMutex {
    SemaphoreHandle_t handle;
    bool locked;

    explicit ScopedWdtMutex(SemaphoreHandle_t h) : handle(h), locked(false) {
        if (handle) {
            while (xSemaphoreTake(handle, pdMS_TO_TICKS(WEBHOOK_WIFI_POLL_MS)) != pdTRUE) {
                esp_task_wdt_reset();
            }
            locked = true;
        }
    }

    ~ScopedWdtMutex() {
        if (locked && handle) {
            xSemaphoreGive(handle);
        }
    }

    ScopedWdtMutex(const ScopedWdtMutex&) = delete;
    ScopedWdtMutex& operator=(const ScopedWdtMutex&) = delete;
};

// Resolves the host once before HTTPClient connects, so a DNS failure is reported as such instead
// of as a connect failure. The caller holds the DNS mutex.
static bool resolveHostWithWdt(const char* host, const char* schemeHost) {
    IPAddress resolvedIp((uint32_t)0);
    esp_task_wdt_reset();
    int dnsOk = WiFi.hostByName(host, resolvedIp);
    esp_task_wdt_reset();

    if (dnsOk != 1 || (uint32_t)resolvedIp == 0) {
        Serial.printf("[Notify] DNS resolution failed for %s\n", schemeHost);
        return false;
    }
    return true;
}

int NetworkManager::sendNotificationAttempt(const NotifyPayload& payload, const char* schemeHost,
                                            SemaphoreHandle_t dnsMutex, SemaphoreHandle_t tlsMutex,
                                            char* errReason, size_t errReasonCap) {
    if (errReason && errReasonCap > 0) errReason[0] = '\0';
    char host[MAX_NOTIFY_URL_LEN + 1];
    if (!extractUrlHost(payload.url, host, sizeof(host))) {
        Serial.printf("[Notify] Invalid target URL host: %s\n", schemeHost);
        if (errReason && errReasonCap > 0) snprintf(errReason, errReasonCap, "Invalid URL host");
        return -1;
    }
    // WiFi.hostByName() waits on one global WIFI_DNS_IDLE_BIT for up to 16 s without feeding the
    // task watchdog, and WiFiClient/WiFiClientSecure::connect() looks the host up again inside
    // http.POST() (the lwIP cache keeps 4 records and expires them by TTL, so that lookup can
    // reach the network). Holding the mutex for the whole attempt means the other worker waits
    // here, feeding the watchdog, and never inside the framework. IP literals never touch DNS and
    // skip the lock.
    bool needsDns = !isIpLiteral(host);
    ScopedWdtMutex dnsLock(needsDns ? dnsMutex : nullptr);
    if (needsDns && !resolveHostWithWdt(host, schemeHost)) {
        if (errReason && errReasonCap > 0) snprintf(errReason, errReasonCap, "DNS failed");
        return -1;
    }

    String url(payload.url);
    bool isHttps = url.startsWith("https://");
    bool isNtfy = shouldUseNtfyFormat(payload.url, payload.event);

    // Arduino-ESP32 esp_crt_bundle stores a single global pointer array freed and reallocated
    // by setCACertBundle(); serialize HTTPS handshakes across worker tasks to prevent races.
    ScopedWdtMutex tlsLock(isHttps ? tlsMutex : nullptr);

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(WEBHOOK_TIMEOUT_MS);
    http.setTimeout(WEBHOOK_TIMEOUT_MS);

    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    bool beginSuccess = false;

    if (isHttps) {
        secureClient.setCACertBundle(rootca_crt_bundle_start);
        secureClient.setTimeout(WEBHOOK_TIMEOUT_MS / 1000);
        secureClient.setHandshakeTimeout(WEBHOOK_TLS_HANDSHAKE_TIMEOUT_S);
        beginSuccess = http.begin(secureClient, url);
    } else {
        plainClient.setTimeout(WEBHOOK_TIMEOUT_MS / 1000);
        beginSuccess = http.begin(plainClient, url);
    }

    if (!beginSuccess) {
        Serial.printf("[Notify] Failed to connect to host: %s\n", schemeHost);
        if (errReason && errReasonCap > 0) snprintf(errReason, errReasonCap, "Connect failed");
        return -1;
    }

    esp_task_wdt_reset();
    int httpCode = -1;
    if (isNtfy) {
        // Optimized headers for ntfy.sh push notifications
        if (strcmp(payload.event, "POWER_OFF") == 0) {
            http.addHeader("Title", "Power Outage Alert");
            http.addHeader("Priority", "urgent");
            http.addHeader("Tags", "warning,rotating_light,zap");
        } else if (strcmp(payload.event, "POWER_ON") == 0) {
            http.addHeader("Title", "Power Restored");
            http.addHeader("Priority", "default");
            http.addHeader("Tags", "white_check_mark,electric_plug");
        } else {
            http.addHeader("Title", "Power Monitor Test");
            http.addHeader("Priority", "default");
            http.addHeader("Tags", "bell,test_tube");
        }

        String messageBody = String("Event: ") + payload.event +
                             "\nDevice: " + payload.device +
                             "\nDuration: " + payload.duration_s + "s";
        httpCode = http.POST(messageBody);
    } else {
        http.addHeader("Content-Type", "application/json");
        String jsonBody(payload.jsonBody);
        httpCode = http.POST(jsonBody);
    }
    if (isHttps && httpCode == -1 && errReason && errReasonCap > 0) {
        char tlsErr[96] = { 0 };
        int tlsCode = secureClient.lastError(tlsErr, sizeof(tlsErr));
        formatTlsErrorReason(tlsCode, tlsErr, errReason, errReasonCap);
    }
    http.end();
    return httpCode;
}

// Background FreeRTOS task for non-blocking HTTP / HTTPS notification dispatch
void NetworkManager::notifyWorkerTask(void* pvParameters) {
    NotifyWorkerContext* ctx = static_cast<NotifyWorkerContext*>(pvParameters);
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();
        if (!ctx || !ctx->queue) {
            vTaskDelay(pdMS_TO_TICKS(WEBHOOK_WIFI_POLL_MS));
            continue;
        }
        if (xQueueReceive(ctx->queue, &ctx->rxPayload, pdMS_TO_TICKS(1000)) != pdPASS) {
            continue;
        }

        NetworkManager* self = ctx->mgr;
        const NotifyPayload& payload = ctx->rxPayload;
        if (payload.url[0] == '\0') continue;

        // Print only scheme + host so secret path/topic tokens stay out of serial logs
        char schemeHost[128];
        extractSchemeHost(payload.url, schemeHost, sizeof(schemeHost));

        bool delivered = false;
        bool aborted = false;
        int lastCode = -1;
        char lastReason[64] = "";

        for (uint8_t attempt = 0; attempt < NOTIFY_MAX_ATTEMPTS; ++attempt) {
            if (attempt > 0) {
                size_t gapCount = sizeof(NOTIFY_RETRY_GAPS_MS) / sizeof(NOTIFY_RETRY_GAPS_MS[0]);
                uint32_t gapMs = (attempt - 1 < gapCount) ? NOTIFY_RETRY_GAPS_MS[attempt - 1] : NOTIFY_RETRY_GAPS_MS[0];
                Serial.printf("[Notify/%s] Retrying event '%s' to %s in %ums (attempt %u/%u)\n",
                              ctx->name, payload.event, schemeHost, (unsigned)gapMs,
                              (unsigned)(attempt + 1), (unsigned)NOTIFY_MAX_ATTEMPTS);
                delayWithWdtReset(gapMs);
            }

            if (self && self->getNotifyGeneration(ctx->target) != payload.urlGeneration) {
                aborted = true;
                break;
            }
            if (self && !self->_alertRouter.isStateSeqCurrent(ctx->target, payload.stateSeq)) {
                Serial.printf("[Notify/%s] Skipping event '%s' to %s: superseded by a newer state event\n",
                              ctx->name, payload.event, schemeHost);
                aborted = true;
                break;
            }

            // Wait up to WEBHOOK_WIFI_WAIT_MS if Wi-Fi is temporarily re-associating
            uint32_t maxPolls = WEBHOOK_WIFI_WAIT_MS / WEBHOOK_WIFI_POLL_MS;
            uint32_t pollCount = 0;
            while ((!self || self->_wifiHal.status() != WL_CONNECTED) && pollCount < maxPolls) {
                if (self && self->getNotifyGeneration(ctx->target) != payload.urlGeneration) {
                    aborted = true;
                    break;
                }
                if (self && !self->_alertRouter.isStateSeqCurrent(ctx->target, payload.stateSeq)) {
                    aborted = true;
                    break;
                }
                if (self) {
                    self->_connector.wakeFromBackoff();
                }
                delayWithWdtReset(WEBHOOK_WIFI_POLL_MS);
                ++pollCount;
            }
            if (aborted) {
                break;
            }

            if (!self || self->_wifiHal.status() != WL_CONNECTED) {
                Serial.printf("[Notify/%s] Dropping event '%s' to %s: Wi-Fi re-association timeout (%us)\n",
                              ctx->name, payload.event, schemeHost,
                              (unsigned)(WEBHOOK_WIFI_WAIT_MS / 1000UL));
                if (self) {
                    self->_alertRouter.markPendingTargetEvent(
                        ctx->target, payload.event, payload.duration_s,
                        self->_config.alert_mode, payload.stateSeq);
                    self->recordNotifyResult(ctx->target, payload.urlGeneration, payload.event, false, 0, "Wi-Fi timeout");
                }
                aborted = true;
                break;
            }

            esp_task_wdt_reset();
            lastReason[0] = '\0';
            lastCode = sendNotificationAttempt(payload, schemeHost,
                                               self ? self->_dnsMutex : nullptr,
                                               self ? self->_tlsMutex : nullptr,
                                               lastReason, sizeof(lastReason));
            esp_task_wdt_reset();

            if (lastCode >= 200 && lastCode < 300) {
                Serial.printf("[Notify/%s] Delivered event '%s' to %s (code: %d, attempt %u/%u)\n",
                              ctx->name, payload.event, schemeHost, lastCode,
                              (unsigned)(attempt + 1), (unsigned)NOTIFY_MAX_ATTEMPTS);
                if (self) {
                    self->recordNotifyResult(ctx->target, payload.urlGeneration, payload.event, true, lastCode, "");
                }
                delivered = true;
                break;
            }

            Serial.printf("[Notify/%s] Attempt %u/%u for event '%s' to %s failed (code: %d)\n",
                          ctx->name, (unsigned)(attempt + 1), (unsigned)NOTIFY_MAX_ATTEMPTS,
                          payload.event, schemeHost, lastCode);
        }

        if (!delivered && !aborted) {
            Serial.printf("[Notify/%s] Exhausted %u attempts for event '%s' to %s (last code: %d)\n",
                          ctx->name, (unsigned)NOTIFY_MAX_ATTEMPTS,
                          payload.event, schemeHost, lastCode);
            if (self) {
                bool staConnected = (self->_wifiHal.status() == WL_CONNECTED);
                if (!staConnected) {
                    self->_connector.wakeFromBackoff();
                }
                self->_alertRouter.handleAttemptsExhausted(
                    ctx->target, payload.event, payload.duration_s,
                    self->_config.alert_mode, payload.stateSeq, staConnected);
                self->recordNotifyResult(ctx->target, payload.urlGeneration, payload.event, false, lastCode, lastReason);
            }
        }
    }
}
