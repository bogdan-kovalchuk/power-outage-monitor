#ifndef CONFIG_H
#define CONFIG_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#endif

// =====================================================================
// Firmware Identity & Hardware Pin Mapping (ESP32 38-Pin DevKit V1)
// =====================================================================
static const char* const FW_VERSION      = "1.0.0";
static const uint8_t PIN_MAINS_SENSE = 4; // GPIO 4: LM2596 OUT+ (3.1V = Online, 0V = Outage)
static const uint8_t PIN_BOOT_BUTTON = 0; // GPIO 0: Onboard BOOT Button (Active LOW)
static const uint8_t PIN_STATUS_LED  = 2; // GPIO 2: Onboard Blue LED (Active HIGH)

// =====================================================================
// Default System Values & Timing Constants
// =====================================================================
static const char* const DEFAULT_AP_SSID_PREFIX = "PowerMonitor-";
static const char* const DEFAULT_AP_PASS      = "12345678";
static const char* const DEFAULT_DEVICE_NAME  = "power-monitor";
static const uint32_t    MIN_DEBOUNCE_MS      = 50;
static const uint32_t    MAX_DEBOUNCE_MS      = 10000;
static const uint32_t    DEFAULT_DEBOUNCE_MS  = 500;   // 500ms software debounce (+ ~1.0s LM2596 capacitor decay = ~1.5s total latency)
static const uint32_t    MIN_COOLDOWN_MS      = 0;
static const uint32_t    MAX_COOLDOWN_MS      = 60000;
static const uint32_t    DEFAULT_COOLDOWN_MS  = 5000;  // Flapping/Brownout guard cooldown (5.0s)
static const uint16_t    MIN_SNOOZE_MIN       = 1;     // Minimum snooze duration (minutes)
static const uint16_t    MAX_SNOOZE_MIN       = 1440;  // Maximum snooze duration (24 hours, minutes)
static const uint16_t    DEFAULT_SNOOZE_MIN   = 30;    // 30 minutes snooze
static const uint16_t    MIN_SETUP_WINDOW_S   = 0;
static const uint16_t    MAX_SETUP_WINDOW_S   = 600;
static const uint16_t    DEFAULT_SETUP_WINDOW_S = 60;  // 60s boot setup AP window
static const uint8_t     DEFAULT_ALERT_MODE   = 0;     // ALERT_OUTAGE_ONLY
static const bool        DEFAULT_CLOUD_ENABLED = false;
static const bool        DEFAULT_ARMED        = true;

// Field Length & HTTP Body Limits
static const size_t      MAX_WIFI_SSID_LEN    = 32;  // MAX_SSID_LEN is taken by esp_wifi_types.h
static const size_t      MIN_PASS_LEN         = 8;
static const size_t      MAX_PASS_LEN         = 63;
static const size_t      MAX_DEVICE_NAME_LEN  = 32;
static const size_t      MAX_WEBHOOK_URL_LEN  = 256;
static const size_t      MAX_NOTIFY_URL_LEN   = 384; // 256-byte template with {event} -> test_alarm expansions
static const size_t      HTTP_MAX_BODY_BYTES  = 2048;

// Button timing parameters
static const uint32_t    BTN_DEBOUNCE_MS      = 50;
static const uint32_t    BTN_LONG_PRESS_MIN   = 2000;  // 2.0s – 5.0s: Reconnect / Test Alarm
static const uint32_t    BTN_SETUP_AP_MIN     = 5000;  // 5.0s – 10.0s: Open Setup AP for 5 min
static const uint32_t    BTN_FACTORY_RESET_MIN= 10000; // >= 10.0s: Factory Reset NVS & Reboot

// NVS Storage Namespace & Schema
static const char* const NVS_NAMESPACE        = "pwr_mon";
static const uint8_t     CFG_VERSION          = 2;
static const uint8_t     WIFI_SLOTS           = 5;
static const uint8_t     WIFI_SLOT_NONE       = 255;

// Setup AP IPv4 address (single source for SoftAP, DNS, and captive-portal redirect)
#define SETUP_AP_IP_O1 192
#define SETUP_AP_IP_O2 168
#define SETUP_AP_IP_O3 4
#define SETUP_AP_IP_O4 1
#define POM_STR_HELPER(x) #x
#define POM_STR(x) POM_STR_HELPER(x)
static const char* const SETUP_AP_IP_STR =
    POM_STR(SETUP_AP_IP_O1) "." POM_STR(SETUP_AP_IP_O2) "."
    POM_STR(SETUP_AP_IP_O3) "." POM_STR(SETUP_AP_IP_O4);

// Wi-Fi Connection & Fallback Timing
static const uint32_t    WIFI_CONNECT_TIMEOUT_MS = 10000;  // 10s per slot attempt
static const uint32_t    WIFI_SCAN_TIMEOUT_MS    = 15000;  // 15s max async scan guard
static const uint32_t    WIFI_BACKOFF_MS[]       = { 15000, 30000, 60000 }; // Cycle retry back-off (15s, 30s, 60s cap)
static const uint8_t     WIFI_BACKOFF_COUNT      = sizeof(WIFI_BACKOFF_MS) / sizeof(WIFI_BACKOFF_MS[0]);
static const uint32_t    WEB_SCAN_CACHE_TTL_MS   = 30000;  // 30s cached web scan results
static const uint32_t    MDNS_RETRY_MS           = 10000;  // 10s mDNS retry throttle
static const uint32_t    DEFERRED_APPLY_DELAY_MS = 300;    // 300ms TCP flush delay before network actions
static const uint32_t    NO_STA_AP_FALLBACK_MS   = 300000; // 5 min continuous disconnect -> open setup AP
static const uint16_t    SETUP_IDLE_TIMEOUT_S    = 120;    // 120s idle after last client leaves
static const uint16_t    SETUP_FALLBACK_WINDOW_S = 120;    // 120s window for no-Wi-Fi fallback and first-save conversion
static const uint16_t    SETUP_ONDEMAND_WINDOW_S = 300;    // 5 min on-demand setup AP window
static const uint16_t    SETUP_HARD_CAP_S        = 1800;   // 30 min max setup window when networks exist
static const uint32_t    SETUP_AP_RETRY_MS       = 5000;   // Retry interval when SoftAP start fails

// =====================================================================
// Enumerations
// =====================================================================

// Grid sensing states
enum GridState {
    GRID_BOOTING,
    GRID_ONLINE,
    GRID_OFFLINE
};

// System arming modes
enum SystemMode {
    MODE_ARMED,
    MODE_DISARMED,
    MODE_SNOOZED
};

// Alert tracking modes (Outage-only vs Outage + Restoration)
enum AlertTrackingMode {
    ALERT_OUTAGE_ONLY = 0,         // Alert only on grid loss, completely silent on grid return
    ALERT_OUTAGE_AND_RESTORE = 1   // Alert on grid loss AND send notification on grid restoration
};

// LED indicator signaling patterns
enum LedPattern {
    LED_SOLID_ON,      // Armed & Grid Online
    LED_SLOW_BLINK,    // Disarmed (1 Hz: 500ms ON, 500ms OFF)
    LED_RAPID_STROBE,  // Power Outage / Alerting (5 Hz: 100ms ON, 100ms OFF)
    LED_DOUBLE_BLINK,  // Snoozed (100ms ON, 100ms OFF, 100ms ON, 700ms OFF)
    LED_SETUP_AP,      // Setup AP Active (three 100ms flashes separated by 100ms gaps, then 700ms off = 1200ms cycle)
    LED_FACTORY_WIPE   // Factory reset hold feedback (10 Hz ultra-rapid burst while held >= 10s; wipe runs on release)
};

// =====================================================================
// Watchdog & Edge-Case Protection Constants
// =====================================================================
// Longest single blocking call is an HTTPS http.POST() attempt: the host lookup
// inside connect() (up to DNS_LOOKUP_MAX_MS when the lwIP cache misses; the
// notify worker holds the DNS mutex, so it never also waits for the other
// worker's lookup) + TCP connect (4 s) + TLS handshake (12 s limit; up to 16 s
// if a 4 s SO_RCVTIMEO read stalls right before the 12 s deadline) + HTTP
// response read (4 s) = 39 s worst case. 45 s adds a 6 s margin above 39 s
// while keeping loop() and worker tasks supervised.
static const uint32_t DNS_LOOKUP_MAX_MS               = 15000; // WiFi.hostByName() answer wait (Arduino-ESP32 2.0.17)
static const uint32_t WDT_TIMEOUT_SECONDS             = 45;    // Hardware Watchdog Timer
static const uint8_t  DNS_PORT                        = 53;    // Captive portal DNS intercept port
static const uint32_t WEBHOOK_STACK_SIZE              = 8192;  // FreeRTOS TLS Task Stack (8 KB)
static const uint32_t WEBHOOK_TIMEOUT_MS              = 4000;  // TCP connect and HTTP read timeout (4s)
static const uint32_t WEBHOOK_TLS_HANDSHAKE_TIMEOUT_S = 12;    // TLS handshake limit (12s; measured ntfy.sh 5.9-6.5s)
static const uint32_t WEBHOOK_WIFI_POLL_MS  = 500;   // Wi-Fi re-association poll interval
static const uint32_t WEBHOOK_WIFI_WAIT_MS  = 10000; // Max wait for Wi-Fi re-association before dropping (10s)
static const uint8_t  NOTIFY_QUEUE_DEPTH    = 4;     // Queued events per target
static const uint8_t  NOTIFY_MAX_ATTEMPTS   = 3;     // Initial send + 2 retries
static const uint32_t NOTIFY_RETRY_GAPS_MS[] = { 2000, 8000 }; // Wait before attempt 2 and attempt 3

enum NotifyTarget {
    NOTIFY_TARGET_LOCAL = 0,
    NOTIFY_TARGET_CLOUD = 1,
    NOTIFY_TARGET_COUNT = 2
};

// =====================================================================
// Persistent Device Configuration
// =====================================================================
struct WifiCred {
    char ssid[33];
    char pass[65];
};

struct DeviceConfig {
    WifiCred wifi[WIFI_SLOTS];
    char device_name[33];
    char ap_ssid[33];
    char ap_pass[65];
    uint16_t setup_window_s;
    char admin_pass[65];
    char webhook_url[MAX_WEBHOOK_URL_LEN + 1];
    char local_url[MAX_WEBHOOK_URL_LEN + 1];
    uint32_t debounce_ms;
    uint32_t restore_cooldown_ms;
    uint16_t snooze_default_min;
    AlertTrackingMode alert_mode;
    bool cloud_enabled;
    bool armed;
};

inline void formatDefaultApSsid(uint64_t mac, char* buf, size_t len) {
    uint8_t b4 = (uint8_t)((mac >> 32) & 0xFF);
    uint8_t b5 = (uint8_t)((mac >> 40) & 0xFF);
    snprintf(buf, len, "%s%02X%02X", DEFAULT_AP_SSID_PREFIX, b4, b5);
}

#endif // CONFIG_H
