#include <unity.h>

#include "config_codec.h"
#include "fakes.h"
#include "ota_update.h"
#include "power_sensor.h"
#include "protocol_helpers.h"

namespace {

int g_outageCbCount = 0;
int g_restoreCbCount = 0;
uint32_t g_lastRestoredDur = 0;

void onOutageHook() { g_outageCbCount++; }
void onRestoredHook(uint32_t dur) {
    g_restoreCbCount++;
    g_lastRestoredDur = dur;
}

} // namespace

void setUp(void) {
    g_outageCbCount = 0;
    g_restoreCbCount = 0;
    g_lastRestoredDur = 0;
}

void tearDown(void) {}

void test_P1_power_sensor_debounce_cooldown_flapping_boot_wrap(void) {
    // 1. Outage at boot (millis() == 0, pin LOW -> GRID_BOOTING until debounce, then GRID_OFFLINE with timer from t=0)
    {
        FakeClock clk(0);
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, DEFAULT_DEBOUNCE_MS, DEFAULT_COOLDOWN_MS, &FakeGpio::readPin, &clk);

        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());
        TEST_ASSERT_FALSE(ps.isOnline());

        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        clk.advance(3500 - DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_UINT32(3, ps.getOutageDurationSeconds());
    }

    // 1b. Regression: outage confirmed at millis() == 0 keeps timing active
    // (an earlier version treated _outageStartMs == 0 as "not active").
    {
        FakeClock clk(0xFFFFFFFFUL - (2UL * DEFAULT_DEBOUNCE_MS) + 1UL);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        PowerSensor ps;
        g_outageCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.init(PIN_MAINS_SENSE, DEFAULT_DEBOUNCE_MS, DEFAULT_COOLDOWN_MS, &FakeGpio::readPin, &clk);
        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());

        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        ps.update();
        clk.set(0);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        clk.advance(4200);
        ps.update();
        TEST_ASSERT_EQUAL_UINT32(4, ps.getOutageDurationSeconds());
    }

    // 2. Debounce + flapping + restore cooldown + millis() wrap across 0xFFFFFFFF
    {
        FakeClock clk(0xFFFFF000UL);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        g_lastRestoredDur = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, DEFAULT_DEBOUNCE_MS, DEFAULT_COOLDOWN_MS, &FakeGpio::readPin, &clk);
        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();

        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());

        // Short glitch LOW below debounce -> ignored
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        clk.advance(200);
        ps.update();
        clk.advance(200);
        ps.update();
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        clk.advance(100);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);

        // Sustained LOW across millis() wrap
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        ps.update();
        clk.advance(400);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        clk.advance(200);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        // Return HIGH during restore cooldown: blocked until cooldown elapses
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(2000);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        clk.advance(2900);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        clk.advance(200);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_restoreCbCount);
        TEST_ASSERT_TRUE(g_lastRestoredDur >= 5);
    }
}

void test_P2_outage_while_sta_down_queues_pending_and_flushes_once(void) {
    AlertRouter router;
    FakeAlertOutput out;

    // STA down -> notifications NOT queued yet, pending armed
    router.handlePowerOff("mon", MODE_ARMED, 1, 120, 1700000120, false, out);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

    // Calling flush before STA reconnect does nothing
    TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 5, 125, 1700000125, false, out));

    // STA reconnects while still offline & armed -> queues BOTH targets ONCE
    out.clear();
    TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 10, 130, 1700000130, true, out));
    TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
    TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));
    TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[0].event.c_str());
    TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[1].event.c_str());
    TEST_ASSERT_NOT_NULL(strstr(out.notifications[0].jsonBody.c_str(), "\"event\":\"POWER_OFF\""));

    // Second flush call does not duplicate
    TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 11, 131, 1700000131, true, out));
    TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
}

void test_P3_pending_alert_fires_only_if_still_offline_and_armed(void) {
    // 1. Grid restored before STA reconnect -> pending cleared, nothing sent
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 1, 50, 0, false, out);

        out.clear();
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 15, 70, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        // Subsequent offline tick on reconnect does not fire stale pending state
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 16, 71, 0, true, out));
    }

    // 2. User disarmed before STA reconnect -> pending cleared, nothing sent
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 1, 50, 0, false, out);

        out.clear();
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_DISARMED, 15, 70, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        // Subsequent re-arm does not fire cleared pending state
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 16, 71, 0, true, out));
    }

    // 3. Outage while offline, disarm and re-arm while still offline -> one POWER_OFF per target on
    //    reconnect (the pending ALARM_STOP is superseded), and a second flush adds nothing
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 1, 50, 0, false, out);
        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_DISARMED, 0, 55, 0, out, false));
        TEST_ASSERT_TRUE(router.handleEnterArmed("mon", MODE_DISARMED, GRID_OFFLINE, 10, 60, 0, false, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 25, 90, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[1].event.c_str());

        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 26, 91, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
    }

    // 4. Pending outage, then re-arm in the same loop pass that sees Wi-Fi back -> handleEnterArmed
    //    sends POWER_OFF once per target and the following flush finds nothing pending
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 1, 50, 0, false, out);
        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_SNOOZED, 600, 55, 0, out, false));
        TEST_ASSERT_TRUE(router.handleEnterArmed("mon", MODE_SNOOZED, GRID_OFFLINE, 30, 80, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));

        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 31, 81, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
    }

    // 5. POWER_OFF still being retried when Wi-Fi drops, then disarm and re-arm offline -> the
    //    worker's older POWER_OFF is no longer current on either target, so only the re-arm's
    //    POWER_OFF goes out after reconnect; TEST_ALARM (seq 0) is never superseded
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 0, 50, 0, true, out);
        const uint32_t inFlightLocal = router.getTargetStateSeq(NOTIFY_TARGET_LOCAL);
        const uint32_t inFlightCloud = router.getTargetStateSeq(NOTIFY_TARGET_CLOUD);
        TEST_ASSERT_TRUE(router.isStateSeqCurrent(NOTIFY_TARGET_LOCAL, inFlightLocal));
        TEST_ASSERT_TRUE(router.isStateSeqCurrent(NOTIFY_TARGET_CLOUD, inFlightCloud));
        out.clear();

        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_DISARMED, 0, 55, 0, out, false));
        TEST_ASSERT_TRUE(router.handleEnterArmed("mon", MODE_DISARMED, GRID_OFFLINE, 10, 60, 0, false, out));
        TEST_ASSERT_FALSE(router.isStateSeqCurrent(NOTIFY_TARGET_LOCAL, inFlightLocal));
        TEST_ASSERT_FALSE(router.isStateSeqCurrent(NOTIFY_TARGET_CLOUD, inFlightCloud));
        TEST_ASSERT_TRUE(router.isStateSeqCurrent(NOTIFY_TARGET_LOCAL, 0));

        // A final-attempt drop reported for the older POWER_OFF cannot re-arm a pending flag
        router.handleAttemptsExhausted(NOTIFY_TARGET_LOCAL, "POWER_OFF", 0, ALERT_OUTAGE_ONLY, inFlightLocal, false);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 25, 90, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));
        TEST_ASSERT_TRUE(router.isStateSeqCurrent(NOTIFY_TARGET_LOCAL, router.getTargetStateSeq(NOTIFY_TARGET_LOCAL)));
    }
}

void test_P5_pending_webhook_outage_resends_webhook_only(void) {
    // 1. Worker drop sets target pending flag; reconnect re-queues ONLY that target
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.markPendingTargetOutage(NOTIFY_TARGET_CLOUD);

        // Still disconnected -> no-op
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 10, 100, 0, false, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        // STA reconnects while grid offline and armed -> queues CLOUD ONLY
        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 15, 105, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_CLOUD, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[0].event.c_str());

        // Second flush does not duplicate
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 16, 106, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
    }

    // 2. Local target alone pending re-queues LOCAL ONLY (with valid POWER_OFF JSON body)
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.markPendingTargetOutage(NOTIFY_TARGET_LOCAL);
        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 18, 108, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_NOT_NULL(strstr(out.notifications[0].jsonBody.c_str(), "\"event\":\"POWER_OFF\""));
    }

    // 3. Grid restored or disarmed before STA reconnect -> pending flags cleared, nothing sent
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.markPendingTargetOutage(NOTIFY_TARGET_LOCAL);
        router.markPendingTargetOutage(NOTIFY_TARGET_CLOUD);
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 20, 110, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 21, 111, 0, true, out));

        router.markPendingTargetOutage(NOTIFY_TARGET_CLOUD);
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_DISARMED, 20, 120, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 21, 121, 0, true, out));
    }
}

void test_P4_disarmed_snoozed_suppress_power_off_and_power_on_mode_rules(void) {
    AlertRouter router;
    FakeAlertOutput out;

    // Disarmed & snoozed suppress POWER_OFF completely
    router.handlePowerOff("mon", MODE_DISARMED, 1, 50, 0, true, out);
    router.handlePowerOff("mon", MODE_SNOOZED, 1, 60, 0, true, out);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
    TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 10, 70, 0, true, out));

    // POWER_ON with ALERT_OUTAGE_ONLY -> LOCAL queued, CLOUD suppressed
    router.handlePowerOn("mon", MODE_ARMED, 120, 200, ALERT_OUTAGE_ONLY, 0, out);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
    TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
    TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[0].event.c_str());
    TEST_ASSERT_NOT_NULL(strstr(out.notifications[0].jsonBody.c_str(), "\"event\":\"POWER_ON\""));
    TEST_ASSERT_NOT_NULL(strstr(out.notifications[0].jsonBody.c_str(), "\"alert_mode\":\"OUTAGE_ONLY\""));

    // POWER_ON with ALERT_OUTAGE_AND_RESTORE when ARMED -> LOCAL + CLOUD queued
    out.clear();
    router.handlePowerOn("mon", MODE_ARMED, 120, 300, ALERT_OUTAGE_AND_RESTORE, 0, out);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
    TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));
    TEST_ASSERT_EQUAL_STRING(out.notifications[0].jsonBody.c_str(), out.notifications[1].jsonBody.c_str());

    // POWER_ON with ALERT_OUTAGE_AND_RESTORE when DISARMED -> LOCAL + CLOUD still sent
    out.clear();
    router.handlePowerOn("mon", MODE_DISARMED, 120, 400, ALERT_OUTAGE_AND_RESTORE, 0, out);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
    TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[0].event.c_str());
    TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[1].event.c_str());
}

void test_P6_power_sensor_debounce_and_cooldown_exact_boundaries(void) {
    FakeClock clk(1000);
    FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
    PowerSensor ps;
    ps.onOutage(onOutageHook);
    ps.onRestored(onRestoredHook);
    ps.init(PIN_MAINS_SENSE, DEFAULT_DEBOUNCE_MS, DEFAULT_COOLDOWN_MS, &FakeGpio::readPin, &clk);
    clk.advance(DEFAULT_DEBOUNCE_MS);
    ps.update();
    TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());

    // Debounce boundary: no event at DEFAULT_DEBOUNCE_MS - 1, event at DEFAULT_DEBOUNCE_MS
    FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
    ps.update();
    clk.advance(DEFAULT_DEBOUNCE_MS - 1);
    ps.update();
    TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
    TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);

    clk.advance(1);
    ps.update();
    TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
    TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

    // Restore cooldown boundary: no event at DEFAULT_COOLDOWN_MS - 1, event at DEFAULT_COOLDOWN_MS
    FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
    ps.update();
    clk.advance(DEFAULT_COOLDOWN_MS - 1);
    ps.update();
    TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
    TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

    clk.advance(1);
    ps.update();
    TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
    TEST_ASSERT_EQUAL_INT(1, g_restoreCbCount);
}

void test_B1_button_press_classification_and_held_led_feedback(void) {
    TEST_ASSERT_EQUAL_INT(BTN_ACT_NONE, classifyPress(0));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_NONE, classifyPress(BTN_DEBOUNCE_MS - 1));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_TOGGLE, classifyPress(BTN_DEBOUNCE_MS));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_TOGGLE, classifyPress(BTN_LONG_PRESS_MIN - 1));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_TEST_RECONNECT, classifyPress(BTN_LONG_PRESS_MIN));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_TEST_RECONNECT, classifyPress(BTN_SETUP_AP_MIN - 1));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_SETUP_AP, classifyPress(BTN_SETUP_AP_MIN));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_SETUP_AP, classifyPress(BTN_FACTORY_RESET_MIN - 1));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_FACTORY_RESET, classifyPress(BTN_FACTORY_RESET_MIN));
    TEST_ASSERT_EQUAL_INT(BTN_ACT_FACTORY_RESET, classifyPress(BTN_FACTORY_RESET_MIN + 5000));

    // Held feedback overrides base pattern at 5-10s (LED_SETUP_AP) and >=10s (LED_FACTORY_WIPE)
    TEST_ASSERT_EQUAL_INT(LED_SOLID_ON, selectLedPattern(GRID_ONLINE, false, MODE_ARMED, true, 1500));
    TEST_ASSERT_EQUAL_INT(LED_SETUP_AP, selectLedPattern(GRID_ONLINE, false, MODE_ARMED, true, BTN_SETUP_AP_MIN));
    TEST_ASSERT_EQUAL_INT(LED_FACTORY_WIPE, selectLedPattern(GRID_OFFLINE, true, MODE_ARMED, true, BTN_FACTORY_RESET_MIN));
}

void test_B2_led_patterns_setup_ap_timing_and_outage_priority(void) {
    // Outage takes priority over setup AP and armed/disarmed; BOOTING does not strobe
    TEST_ASSERT_EQUAL_INT(LED_RAPID_STROBE, selectLedPattern(GRID_OFFLINE, true, MODE_ARMED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_SETUP_AP, selectLedPattern(GRID_ONLINE, true, MODE_ARMED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_SOLID_ON, selectLedPattern(GRID_ONLINE, false, MODE_ARMED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_DOUBLE_BLINK, selectLedPattern(GRID_ONLINE, false, MODE_SNOOZED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_SLOW_BLINK, selectLedPattern(GRID_ONLINE, false, MODE_DISARMED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_SOLID_ON, selectLedPattern(GRID_BOOTING, false, MODE_ARMED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_DOUBLE_BLINK, selectLedPattern(GRID_BOOTING, false, MODE_SNOOZED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_SLOW_BLINK, selectLedPattern(GRID_BOOTING, false, MODE_DISARMED, false, 0));
    TEST_ASSERT_EQUAL_INT(LED_SETUP_AP, selectLedPattern(GRID_BOOTING, true, MODE_ARMED, false, 0));

    // LED_SETUP_AP: 1200ms cycle = 100 ON, 100 OFF, 100 ON, 100 OFF, 100 ON, 700 OFF
    TEST_ASSERT_TRUE(ledLevel(LED_SETUP_AP, 0));
    TEST_ASSERT_TRUE(ledLevel(LED_SETUP_AP, 99));
    TEST_ASSERT_FALSE(ledLevel(LED_SETUP_AP, 100));
    TEST_ASSERT_FALSE(ledLevel(LED_SETUP_AP, 199));
    TEST_ASSERT_TRUE(ledLevel(LED_SETUP_AP, 200));
    TEST_ASSERT_FALSE(ledLevel(LED_SETUP_AP, 300));
    TEST_ASSERT_TRUE(ledLevel(LED_SETUP_AP, 400));
    TEST_ASSERT_FALSE(ledLevel(LED_SETUP_AP, 500));
    TEST_ASSERT_FALSE(ledLevel(LED_SETUP_AP, 1199));
    TEST_ASSERT_TRUE(ledLevel(LED_SETUP_AP, 1200));
}

void test_helpers_extract_scheme_host_sanitizes_path_query_userinfo(void) {
    char out[128];
    extractSchemeHost("https://user:secret@ntfy.sh/my-topic?title=x#frag", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh", out);

    extractSchemeHost("http://192.168.1.10:8080/hook", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8080", out);

    extractSchemeHost("https://user@example.com", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("https://example.com", out);
}

void test_P7_boot_state_machine_low_high_and_bounce_restarts_window(void) {
    TEST_ASSERT_EQUAL_STRING("BOOTING", gridStateStr(GRID_BOOTING));
    TEST_ASSERT_EQUAL_STRING("ONLINE", gridStateStr(GRID_ONLINE));
    TEST_ASSERT_EQUAL_STRING("OFFLINE", gridStateStr(GRID_OFFLINE));

    // 1. Boot with line LOW: stays BOOTING before debounce, then OFFLINE + 1 callback, timer starts at first LOW
    {
        FakeClock clk(1000);
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, 1500, 5000, &FakeGpio::readPin, &clk);

        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());
        TEST_ASSERT_FALSE(ps.isOnline());
        TEST_ASSERT_EQUAL_UINT32(0, ps.getOutageDurationSeconds());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);

        clk.advance(1499);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);
        TEST_ASSERT_EQUAL_UINT32(1, ps.getOutageDurationSeconds());

        clk.advance(500);
        ps.update();
        TEST_ASSERT_EQUAL_UINT32(2, ps.getOutageDurationSeconds());
    }

    // 2. Boot with line HIGH: restore cooldown does not delay first decision, ONLINE + no callback
    {
        FakeClock clk(0);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, 500, 5000, &FakeGpio::readPin, &clk);

        clk.advance(499);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_TRUE(ps.isOnline());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);
    }

    // 3. Level change during boot window restarts the debounce window (LOW -> HIGH)
    {
        FakeClock clk(0);
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, 500, 5000, &FakeGpio::readPin, &clk);

        clk.advance(300);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());

        clk.advance(499);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);
    }

    // 4. Level change during boot window restarts the debounce window (HIGH -> LOW)
    {
        FakeClock clk(0);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        PowerSensor ps;
        g_outageCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.init(PIN_MAINS_SENSE, 500, 5000, &FakeGpio::readPin, &clk);

        clk.advance(300);
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());

        clk.advance(499);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_BOOTING, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_outageCbCount);

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        clk.advance(500);
        ps.update();
        TEST_ASSERT_EQUAL_UINT32(1, ps.getOutageDurationSeconds());
    }
}

void test_P8_entering_armed_alerts_only_from_non_armed_when_grid_offline(void) {
    // 1. DISARMED -> ARMED while GRID_OFFLINE alerts once with duration so far
    {
        AlertRouter router;
        FakeAlertOutput out;
        TEST_ASSERT_TRUE(router.handleEnterArmed("mon", MODE_DISARMED, GRID_OFFLINE, 42, 100, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, out.notifications[0].jsonBody).code());
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", doc["event"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(42, doc["duration_s"].as<uint32_t>());
    }

    // 2. SNOOZED -> ARMED while GRID_OFFLINE alerts once with duration so far
    {
        AlertRouter router;
        FakeAlertOutput out;
        TEST_ASSERT_TRUE(router.handleEnterArmed("mon", MODE_SNOOZED, GRID_OFFLINE, 15, 200, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, out.notifications[0].jsonBody).code());
        TEST_ASSERT_EQUAL_UINT32(15, doc["duration_s"].as<uint32_t>());
    }

    // 3. ARMED -> ARMED while GRID_OFFLINE sends nothing
    {
        AlertRouter router;
        FakeAlertOutput out;
        TEST_ASSERT_FALSE(router.handleEnterArmed("mon", MODE_ARMED, GRID_OFFLINE, 42, 100, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
    }

    // 4. DISARMED -> ARMED while GRID_ONLINE or GRID_BOOTING sends nothing
    {
        AlertRouter router;
        FakeAlertOutput out;
        TEST_ASSERT_FALSE(router.handleEnterArmed("mon", MODE_DISARMED, GRID_ONLINE, 0, 100, 0, true, out));
        TEST_ASSERT_FALSE(router.handleEnterArmed("mon", MODE_DISARMED, GRID_BOOTING, 0, 100, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
    }
}

void test_P9_snooze_mode_gate_limits_and_parsers(void) {
    // 1. Mode gate
    TEST_ASSERT_FALSE(canSnoozeMode(MODE_DISARMED));
    TEST_ASSERT_TRUE(canSnoozeMode(MODE_ARMED));
    TEST_ASSERT_TRUE(canSnoozeMode(MODE_SNOOZED));

    // 2. Normaliser boundaries: 0 (fallback), 1, 1440, 1441
    TEST_ASSERT_EQUAL_UINT16(30, normalizeSnoozeMinutes(0, 30));
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SNOOZE_MIN, normalizeSnoozeMinutes(0, 0));
    TEST_ASSERT_EQUAL_UINT16(MAX_SNOOZE_MIN, normalizeSnoozeMinutes(0, MAX_SNOOZE_MIN + 100));
    TEST_ASSERT_EQUAL_UINT16(MIN_SNOOZE_MIN, normalizeSnoozeMinutes(MIN_SNOOZE_MIN, 30));
    TEST_ASSERT_EQUAL_UINT16(MAX_SNOOZE_MIN, normalizeSnoozeMinutes(MAX_SNOOZE_MIN, 30));
    TEST_ASSERT_EQUAL_UINT16(MAX_SNOOZE_MIN, normalizeSnoozeMinutes(MAX_SNOOZE_MIN + 1, 30));

    // 3. HTTP parser
    uint16_t outMin = 0;
    char ef[32] = {0};
    char er[80] = {0};
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "", 45, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(45, outMin);

    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(true, "0", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_STRING("min", ef);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(true, "1", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(true, "1440", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(true, "1441", false, "", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);

    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", true, "0", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_STRING("duration", ef);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", true, "30", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", true, "86460", "", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);

    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":0}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_STRING("duration_min", ef);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":1}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":1440}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":1441}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);
    TEST_ASSERT_TRUE(parseSnoozeHttpRequest(false, "", false, "", "{\"duration_min\":5000000000}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_UINT16(1440, outMin);

    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", false, "", "{\"min\":5}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_STRING("body", ef);
    TEST_ASSERT_EQUAL_STRING("expected duration_min or duration", er);
    TEST_ASSERT_FALSE(parseSnoozeHttpRequest(false, "", false, "", "{}", 30, outMin, ef, sizeof(ef), er, sizeof(er)));
    TEST_ASSERT_EQUAL_STRING("body", ef);
    TEST_ASSERT_EQUAL_STRING("expected duration_min or duration", er);
}

void test_P10_payload_builders_contract_fields_and_ts_zero(void) {
    char buf[768];

    // 1. POWER_OFF
    {
        buildPowerOffJsonBuf("mon", 12, 100, 0, buf, sizeof(buf));
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        TEST_ASSERT_EQUAL_UINT(9, (unsigned int)doc.as<JsonObject>().size());
        TEST_ASSERT_EQUAL_UINT32(0, doc["ts"].as<uint32_t>());
        TEST_ASSERT_EQUAL_STRING("OFFLINE", doc["grid"].as<const char*>());
        TEST_ASSERT_FALSE(doc["power"].as<bool>());
    }

    // 2. POWER_ON
    {
        buildPowerOnJsonBuf("mon", MODE_ARMED, 60, 200, ALERT_OUTAGE_ONLY, 0, buf, sizeof(buf));
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        TEST_ASSERT_EQUAL_UINT(10, (unsigned int)doc.as<JsonObject>().size());
        TEST_ASSERT_EQUAL_STRING("OUTAGE_ONLY", doc["alert_mode"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(0, doc["ts"].as<uint32_t>());
    }

    // 3. TEST_ALARM: armed follows mode
    {
        buildTestAlarmJsonBuf("mon", MODE_ARMED, 50, 0, buf, sizeof(buf));
        JsonDocument docA;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(docA, buf).code());
        TEST_ASSERT_EQUAL_UINT(9, (unsigned int)docA.as<JsonObject>().size());
        TEST_ASSERT_TRUE(docA["armed"].as<bool>());
        TEST_ASSERT_EQUAL_STRING("ARMED", docA["state"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(0, docA["ts"].as<uint32_t>());

        buildTestAlarmJsonBuf("mon", MODE_DISARMED, 50, 0, buf, sizeof(buf));
        JsonDocument docD;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(docD, buf).code());
        TEST_ASSERT_EQUAL_UINT(9, (unsigned int)docD.as<JsonObject>().size());
        TEST_ASSERT_FALSE(docD["armed"].as<bool>());
        TEST_ASSERT_EQUAL_STRING("DISARMED", docD["state"].as<const char*>());

        buildTestAlarmJsonBuf("mon", MODE_SNOOZED, 50, 0, buf, sizeof(buf));
        JsonDocument docS;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(docS, buf).code());
        TEST_ASSERT_EQUAL_UINT(9, (unsigned int)docS.as<JsonObject>().size());
        TEST_ASSERT_FALSE(docS["armed"].as<bool>());
        TEST_ASSERT_EQUAL_STRING("SNOOZED", docS["state"].as<const char*>());
    }

    // 4. StatusSnapshot across all three GridStates
    {
        StatusSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        snap.deviceName = "mon";
        snap.mode = MODE_ARMED;
        snap.snoozeDefaultMin = 30;
        snap.ts = 0;

        snap.gridState = GRID_BOOTING;
        buildStatusJsonBuf(snap, buf, sizeof(buf));
        JsonDocument docBoot;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(docBoot, buf).code());
        TEST_ASSERT_EQUAL_UINT(16, (unsigned int)docBoot.as<JsonObject>().size());
        TEST_ASSERT_EQUAL_STRING(FW_VERSION, docBoot["fw_version"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("BOOTING", docBoot["grid"].as<const char*>());
        TEST_ASSERT_FALSE(docBoot["power"].as<bool>());
        TEST_ASSERT_EQUAL_UINT32(0, docBoot["ts"].as<uint32_t>());

        snap.gridState = GRID_ONLINE;
        buildStatusJsonBuf(snap, buf, sizeof(buf));
        JsonDocument docOn;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(docOn, buf).code());
        TEST_ASSERT_EQUAL_STRING("ONLINE", docOn["grid"].as<const char*>());
        TEST_ASSERT_TRUE(docOn["power"].as<bool>());

        snap.gridState = GRID_OFFLINE;
        buildStatusJsonBuf(snap, buf, sizeof(buf));
        JsonDocument docOff;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(docOff, buf).code());
        TEST_ASSERT_EQUAL_STRING("OFFLINE", docOff["grid"].as<const char*>());
        TEST_ASSERT_FALSE(docOff["power"].as<bool>());
    }
}

void test_P11_expand_notification_url_placeholders_and_overflow(void) {
    char out[MAX_NOTIFY_URL_LEN + 1];

    TEST_ASSERT_TRUE(expandNotificationUrl("http://192.168.1.60:8080/{event}", "POWER_OFF", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/power_off", out);

    TEST_ASSERT_TRUE(expandNotificationUrl("http://192.168.1.60:8080/{event}", "POWER_ON", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/power_on", out);

    TEST_ASSERT_TRUE(expandNotificationUrl("http://192.168.1.60:8080/{event}", "TEST_ALARM", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/test_alarm", out);

    // Multiple occurrences and non-matching placeholders
    TEST_ASSERT_TRUE(expandNotificationUrl("https://ex.com/{event}/hook?ev={event}&other={Event}",
                                           "POWER_OFF", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://ex.com/power_off/hook?ev=power_off&other={Event}", out);

    // No placeholder -> copied unchanged
    TEST_ASSERT_TRUE(expandNotificationUrl("https://ntfy.sh/topic", "TEST_ALARM", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/topic", out);

    // Exact buffer boundary & overflow detection
    char exact[18]; // "http://x/power_on" is 17 chars + NUL = 18 bytes
    TEST_ASSERT_TRUE(expandNotificationUrl("http://x/{event}", "POWER_ON", exact, sizeof(exact)));
    TEST_ASSERT_EQUAL_STRING("http://x/power_on", exact);

    char tooSmall[17]; // "http://x/power_on" overflows 17-byte buffer; "test_alarm" also overflows
    TEST_ASSERT_FALSE(expandNotificationUrl("http://x/{event}", "POWER_ON", tooSmall, sizeof(tooSmall)));
    TEST_ASSERT_EQUAL_STRING("", tooSmall);
    TEST_ASSERT_FALSE(expandNotificationUrl("http://x/literal_too_long", "POWER_ON", tooSmall, sizeof(tooSmall)));
    TEST_ASSERT_EQUAL_STRING("", tooSmall);

    // Invalid inputs
    TEST_ASSERT_FALSE(expandNotificationUrl(nullptr, "POWER_OFF", out, sizeof(out)));
    TEST_ASSERT_FALSE(expandNotificationUrl("http://x/{event}", "", out, sizeof(out)));
}

void test_P12_extract_url_host_and_ntfy_detection(void) {
    char host[128];

    TEST_ASSERT_TRUE(extractUrlHost("https://ntfy.sh/my-topic", host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("ntfy.sh", host);

    TEST_ASSERT_TRUE(extractUrlHost("https://user:pass@NTFY.SH:8443/topic?x=1", host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("NTFY.SH", host);

    TEST_ASSERT_TRUE(extractUrlHost("http://192.168.1.60:8080/power_off", host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("192.168.1.60", host);

    TEST_ASSERT_TRUE(extractUrlHost("http://[::1]:8080/test_alarm", host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("::1", host);

    TEST_ASSERT_FALSE(extractUrlHost("not-a-url", host, sizeof(host)));
    TEST_ASSERT_FALSE(extractUrlHost("http://:8080/path", host, sizeof(host)));
    TEST_ASSERT_FALSE(extractUrlHost("http://[::1/path", host, sizeof(host)));
    char tiny[4];
    TEST_ASSERT_FALSE(extractUrlHost("https://ntfy.sh/t", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);

    // Host-exact case-insensitive ntfy.sh detection (no substring false positives)
    TEST_ASSERT_TRUE(isNtfyHost("https://ntfy.sh/power-test"));
    TEST_ASSERT_TRUE(isNtfyHost("http://NTFY.SH:8080/power-test"));
    TEST_ASSERT_TRUE(isNtfyHost("https://tok@ntfy.sh/power-test"));
    TEST_ASSERT_FALSE(isNtfyHost("https://ntfy.sh.evil.com/power-test"));
    TEST_ASSERT_FALSE(isNtfyHost("https://evil.com/ntfy.sh/power-test"));
    TEST_ASSERT_FALSE(isNtfyHost("http://192.168.1.60:8080/ntfy.sh"));
    TEST_ASSERT_FALSE(isNtfyHost("ntfy.sh/power-test"));
}

void test_P13_should_notify_target_routing_matrix(void) {
    // Empty/null URLs suppress delivery for both targets
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, "", "POWER_OFF", ALERT_OUTAGE_AND_RESTORE, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, nullptr, "POWER_OFF", ALERT_OUTAGE_AND_RESTORE, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, "", "POWER_OFF", ALERT_OUTAGE_AND_RESTORE, true));

    // Local target sends all alert events regardless of alert_mode or cloud_enabled
    const char* localUrl = "http://192.168.1.60:8080/{event}";
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "POWER_OFF", ALERT_OUTAGE_ONLY, false));
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "POWER_ON", ALERT_OUTAGE_ONLY, false));
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "TEST_ALARM", ALERT_OUTAGE_ONLY, false));
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "POWER_ON", ALERT_OUTAGE_AND_RESTORE, true));

    // Cloud target skips everything when cloudEnabled is false, even with valid URL
    const char* cloudUrl = "https://ntfy.sh/topic";
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "POWER_OFF", ALERT_OUTAGE_AND_RESTORE, false));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "POWER_ON", ALERT_OUTAGE_AND_RESTORE, false));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "TEST_ALARM", ALERT_OUTAGE_AND_RESTORE, false));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "ALARM_STOP", ALERT_OUTAGE_AND_RESTORE, false));

    // Cloud target gates POWER_ON on ALERT_OUTAGE_AND_RESTORE when cloudEnabled is true
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "POWER_OFF", ALERT_OUTAGE_ONLY, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "POWER_ON", ALERT_OUTAGE_ONLY, true));
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "TEST_ALARM", ALERT_OUTAGE_ONLY, true));
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "POWER_ON", ALERT_OUTAGE_AND_RESTORE, true));

    // Unknown events are ignored for both targets
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "UNKNOWN_EVENT", ALERT_OUTAGE_AND_RESTORE, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "OTHER_EVENT", ALERT_OUTAGE_AND_RESTORE, true));
}

void test_P14_status_fw_version_and_ota_guard_decision(void) {
    // 1. Status JSON fw_version defaults to FW_VERSION and respects explicit value
    char buf[768];
    StatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.deviceName = "mon";
    snap.gridState = GRID_ONLINE;
    snap.mode = MODE_ARMED;
    snap.snoozeDefaultMin = 30;
    buildStatusJsonBuf(snap, buf, sizeof(buf));
    {
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        TEST_ASSERT_EQUAL_STRING(FW_VERSION, doc["fw_version"].as<const char*>());
    }
    snap.fwVersion = "2.1.0";
    buildStatusJsonBuf(snap, buf, sizeof(buf));
    {
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        TEST_ASSERT_EQUAL_STRING("2.1.0", doc["fw_version"].as<const char*>());
    }

    // 2. Pure OTA guard decision matrix
    OtaGuardOutcome d = evaluateOtaGuard(false, false, false, false);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_NO_CSRF, d.result);
    TEST_ASSERT_EQUAL_UINT16(403, d.httpStatus);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"csrf: missing X-Requested-With header\"}", d.errorJson);

    d = evaluateOtaGuard(false, true, true, true);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_NO_CSRF, d.result);
    TEST_ASSERT_EQUAL_UINT16(403, d.httpStatus);

    d = evaluateOtaGuard(true, false, false, false);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_NO_ADMIN_PASS, d.result);
    TEST_ASSERT_EQUAL_UINT16(403, d.httpStatus);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"update: set an admin password first\"}", d.errorJson);

    d = evaluateOtaGuard(true, false, true, true);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_NO_ADMIN_PASS, d.result);

    d = evaluateOtaGuard(true, true, false, false);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_UNAUTHORIZED, d.result);
    TEST_ASSERT_EQUAL_UINT16(401, d.httpStatus);

    d = evaluateOtaGuard(true, true, false, true);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_UNAUTHORIZED, d.result);

    d = evaluateOtaGuard(true, true, true, true);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_BUSY, d.result);
    TEST_ASSERT_EQUAL_UINT16(409, d.httpStatus);
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"update: already in progress\"}", d.errorJson);

    d = evaluateOtaGuard(true, true, true, false);
    TEST_ASSERT_EQUAL_INT(OTA_GUARD_ALLOW, d.result);
    TEST_ASSERT_EQUAL_UINT16(200, d.httpStatus);

    // 3. Field and error normalization helpers
    TEST_ASSERT_TRUE(isOtaFirmwareField("firmware"));
    TEST_ASSERT_FALSE(isOtaFirmwareField("filesystem"));
    TEST_ASSERT_FALSE(isOtaFirmwareField("Firmware"));
    TEST_ASSERT_FALSE(isOtaFirmwareField(""));
    TEST_ASSERT_FALSE(isOtaFirmwareField(nullptr));

    TEST_ASSERT_EQUAL_STRING("Wrong Magic Byte", normalizeOtaErrorReason("Wrong Magic Byte", "fallback"));
    TEST_ASSERT_EQUAL_STRING("begin failed", normalizeOtaErrorReason("No Error", "begin failed"));
    TEST_ASSERT_EQUAL_STRING("begin failed", normalizeOtaErrorReason("", "begin failed"));
    TEST_ASSERT_EQUAL_STRING("begin failed", normalizeOtaErrorReason(nullptr, "begin failed"));
}

void test_P15_is_ip_literal(void) {
    // IPv4 literals
    TEST_ASSERT_TRUE(isIpLiteral("192.168.1.50"));
    TEST_ASSERT_TRUE(isIpLiteral("127.0.0.1"));
    TEST_ASSERT_TRUE(isIpLiteral("0.0.0.0"));
    TEST_ASSERT_TRUE(isIpLiteral("255.255.255.255"));
    TEST_ASSERT_TRUE(isIpLiteral("10.0.0.1"));

    // IPv6 literals
    TEST_ASSERT_TRUE(isIpLiteral("::1"));
    TEST_ASSERT_TRUE(isIpLiteral("::"));
    TEST_ASSERT_TRUE(isIpLiteral("2001:db8::1"));
    TEST_ASSERT_TRUE(isIpLiteral("fe80::1"));
    TEST_ASSERT_TRUE(isIpLiteral("2001:0db8:85a3:0000:0000:8a2e:0370:7334"));
    TEST_ASSERT_TRUE(isIpLiteral("::ffff:192.0.2.128"));

    // Hostnames and malformed addresses
    TEST_ASSERT_FALSE(isIpLiteral(nullptr));
    TEST_ASSERT_FALSE(isIpLiteral(""));
    TEST_ASSERT_FALSE(isIpLiteral("ntfy.sh"));
    TEST_ASSERT_FALSE(isIpLiteral("example.com"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.1.50.com"));
    TEST_ASSERT_FALSE(isIpLiteral("256.1.1.1"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.1"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.1.1.2"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168..1"));
    TEST_ASSERT_FALSE(isIpLiteral(".192.168.1"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.1."));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.01.1"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.1.1a"));
    TEST_ASSERT_FALSE(isIpLiteral("192.168.1.1:8080"));
    TEST_ASSERT_FALSE(isIpLiteral(":::1"));
    TEST_ASSERT_FALSE(isIpLiteral("::1::2"));
    TEST_ASSERT_FALSE(isIpLiteral(":1"));
    TEST_ASSERT_FALSE(isIpLiteral("1:"));
    TEST_ASSERT_FALSE(isIpLiteral("1:2:3"));
    TEST_ASSERT_FALSE(isIpLiteral("gggg::1"));
    TEST_ASSERT_FALSE(isIpLiteral("host:8080"));
    TEST_ASSERT_FALSE(isIpLiteral("::ffff:999.0.0.1"));
}

void test_P17_alarm_stop_decision_matrix(void) {
    // All 9 mode transitions (prev -> next)
    TEST_ASSERT_FALSE(shouldSendAlarmStop(MODE_ARMED, MODE_ARMED));
    TEST_ASSERT_TRUE(shouldSendAlarmStop(MODE_ARMED, MODE_DISARMED));
    TEST_ASSERT_TRUE(shouldSendAlarmStop(MODE_ARMED, MODE_SNOOZED));

    TEST_ASSERT_FALSE(shouldSendAlarmStop(MODE_DISARMED, MODE_ARMED));
    TEST_ASSERT_FALSE(shouldSendAlarmStop(MODE_DISARMED, MODE_DISARMED));
    TEST_ASSERT_FALSE(shouldSendAlarmStop(MODE_DISARMED, MODE_SNOOZED));

    TEST_ASSERT_FALSE(shouldSendAlarmStop(MODE_SNOOZED, MODE_ARMED));
    TEST_ASSERT_FALSE(shouldSendAlarmStop(MODE_SNOOZED, MODE_DISARMED));
    TEST_ASSERT_TRUE(shouldSendAlarmStop(MODE_SNOOZED, MODE_SNOOZED));
}

void test_P18_alarm_stop_builder_and_local_only_routing(void) {
    char buf[384];

    // 1. Builder: DISARMED and SNOOZED shapes with exact key count
    {
        size_t n = buildAlarmStopJsonBuf("mon", MODE_DISARMED, 0, 120, 0, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        JsonObject obj = doc.as<JsonObject>();
        TEST_ASSERT_EQUAL_UINT(6, (unsigned int)obj.size());
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", obj["event"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("mon", obj["device"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("DISARMED", obj["state"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(0, obj["snooze_remaining_s"].as<uint32_t>());
        TEST_ASSERT_EQUAL_UINT32(120, obj["uptime_s"].as<uint32_t>());
        TEST_ASSERT_EQUAL_UINT32(0, obj["ts"].as<uint32_t>());
        TEST_ASSERT_FALSE(obj["armed"].is<JsonVariantConst>());
    }
    {
        size_t n = buildAlarmStopJsonBuf(nullptr, MODE_SNOOZED, 1800, 450, 1700000450, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        JsonObject obj = doc.as<JsonObject>();
        TEST_ASSERT_EQUAL_UINT(6, (unsigned int)obj.size());
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", obj["event"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING(DEFAULT_DEVICE_NAME, obj["device"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("SNOOZED", obj["state"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(1800, obj["snooze_remaining_s"].as<uint32_t>());
        TEST_ASSERT_EQUAL_UINT32(450, obj["uptime_s"].as<uint32_t>());
        TEST_ASSERT_EQUAL_UINT32(1700000450UL, obj["ts"].as<uint32_t>());
    }

    // 2. Target routing & URL expansion: local only, never cloud
    const char* localUrl = "http://192.168.1.60:8080/{event}";
    const char* cloudUrl = "https://ntfy.sh/topic";
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "ALARM_STOP", ALERT_OUTAGE_ONLY, false));
    TEST_ASSERT_TRUE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, localUrl, "ALARM_STOP", ALERT_OUTAGE_AND_RESTORE, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_LOCAL, "", "ALARM_STOP", ALERT_OUTAGE_AND_RESTORE, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "ALARM_STOP", ALERT_OUTAGE_ONLY, true));
    TEST_ASSERT_FALSE(shouldNotifyTarget(NOTIFY_TARGET_CLOUD, cloudUrl, "ALARM_STOP", ALERT_OUTAGE_AND_RESTORE, true));

    char expanded[MAX_NOTIFY_URL_LEN + 1];
    TEST_ASSERT_TRUE(expandNotificationUrl(localUrl, "ALARM_STOP", expanded, sizeof(expanded)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/alarm_stop", expanded);

    // 3. AlertRouter::handleAlarmStop: queues LOCAL only
    {
        AlertRouter router;
        FakeAlertOutput out;

        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_DISARMED, 0, 100, 1700000100, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_UINT32(0, out.notifications[0].durationS);
        {
            JsonDocument doc;
            TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, out.notifications[0].jsonBody).code());
            TEST_ASSERT_EQUAL_STRING("DISARMED", doc["state"].as<const char*>());
            TEST_ASSERT_EQUAL_UINT32(0, doc["snooze_remaining_s"].as<uint32_t>());
        }

        out.clear();
        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_SNOOZED, 1800, 120, 1700000120, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", out.notifications[0].event.c_str());
        {
            JsonDocument doc;
            TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, out.notifications[0].jsonBody).code());
            TEST_ASSERT_EQUAL_STRING("SNOOZED", doc["state"].as<const char*>());
            TEST_ASSERT_EQUAL_UINT32(1800, doc["snooze_remaining_s"].as<uint32_t>());
        }

        out.clear();
        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_SNOOZED, MODE_SNOOZED, 900, 150, 1700000150, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);

        out.clear();
        TEST_ASSERT_FALSE(router.handleAlarmStop("mon", MODE_DISARMED, MODE_DISARMED, 0, 160, 0, out));
        TEST_ASSERT_FALSE(router.handleAlarmStop("mon", MODE_SNOOZED, MODE_DISARMED, 0, 160, 0, out));
        TEST_ASSERT_FALSE(router.handleAlarmStop("mon", MODE_DISARMED, MODE_SNOOZED, 900, 160, 0, out));
        TEST_ASSERT_FALSE(router.handleAlarmStop("mon", MODE_DISARMED, MODE_ARMED, 0, 160, 0, out));
        TEST_ASSERT_FALSE(router.handleAlarmStop("mon", MODE_SNOOZED, MODE_ARMED, 0, 160, 0, out));
        TEST_ASSERT_FALSE(router.handleAlarmStop("mon", MODE_ARMED, MODE_ARMED, 0, 160, 0, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
    }
}

void test_P19_notify_status_tracker_and_age_wrap() {
    NotifyStatusTracker tracker;

    // Nothing sent yet -> valid == false even when configured == true
    NotifyStatusSnapshot localSnap = tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 5000);
    NotifyStatusSnapshot cloudSnap = tracker.getSnapshot(NOTIFY_TARGET_CLOUD, true, 5000);
    TEST_ASSERT_FALSE(localSnap.valid);
    TEST_ASSERT_FALSE(cloudSnap.valid);

    const uint32_t localGen = tracker.getGeneration(NOTIFY_TARGET_LOCAL);
    const uint32_t cloudGen = tracker.getGeneration(NOTIFY_TARGET_CLOUD);

    // Record ok on local; stays null if configured == false
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_LOCAL, localGen, "TEST_ALARM", true, 200, "", 1000));
    TEST_ASSERT_FALSE(tracker.getSnapshot(NOTIFY_TARGET_LOCAL, false, 6000).valid);

    localSnap = tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 16500);
    TEST_ASSERT_TRUE(localSnap.valid);
    TEST_ASSERT_EQUAL_STRING("TEST_ALARM", localSnap.event);
    TEST_ASSERT_TRUE(localSnap.ok);
    TEST_ASSERT_EQUAL_INT(200, localSnap.code);
    TEST_ASSERT_EQUAL_STRING("", localSnap.reason);
    TEST_ASSERT_EQUAL_UINT32(15, localSnap.ageS);

    // Overwrite local with failure using implicit HTTP code reason
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_LOCAL, localGen, "POWER_OFF", false, 502, "", 20000));
    localSnap = tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 140000);
    TEST_ASSERT_TRUE(localSnap.valid);
    TEST_ASSERT_EQUAL_STRING("POWER_OFF", localSnap.event);
    TEST_ASSERT_FALSE(localSnap.ok);
    TEST_ASSERT_EQUAL_INT(502, localSnap.code);
    TEST_ASSERT_EQUAL_STRING("HTTP 502", localSnap.reason);
    TEST_ASSERT_EQUAL_UINT32(120, localSnap.ageS);

    // 32-bit millis() wrap-around on cloud target: 0xFFFFFF00 -> 0x00001000 = 4352 ms -> 4 s
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_CLOUD, cloudGen, "POWER_OFF", false, -11, "", 0xFFFFFF00UL));
    cloudSnap = tracker.getSnapshot(NOTIFY_TARGET_CLOUD, true, 0x00001000UL);
    TEST_ASSERT_TRUE(cloudSnap.valid);
    TEST_ASSERT_EQUAL_STRING("Read timeout", cloudSnap.reason);
    TEST_ASSERT_EQUAL_UINT32(4, cloudSnap.ageS);

    // clearTarget clears only the targeted slot
    tracker.clearTarget(NOTIFY_TARGET_LOCAL);
    TEST_ASSERT_FALSE(tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 145000).valid);
    TEST_ASSERT_TRUE(tracker.getSnapshot(NOTIFY_TARGET_CLOUD, true, 0x00001000UL).valid);
}

void test_P20_status_json_notify_fields_and_reason_format() {
    char reasonBuf[64];
    formatNotifyFailureReason(0, "DNS failed", reasonBuf, sizeof(reasonBuf));
    TEST_ASSERT_EQUAL_STRING("DNS failed", reasonBuf);
    formatNotifyFailureReason(404, "", reasonBuf, sizeof(reasonBuf));
    TEST_ASSERT_EQUAL_STRING("HTTP 404", reasonBuf);
    formatNotifyFailureReason(-1, "", reasonBuf, sizeof(reasonBuf));
    TEST_ASSERT_EQUAL_STRING("Connection refused", reasonBuf);
    formatNotifyFailureReason(-42, "", reasonBuf, sizeof(reasonBuf));
    TEST_ASSERT_EQUAL_STRING("Client error -42", reasonBuf);
    formatNotifyFailureReason(0, "", reasonBuf, sizeof(reasonBuf));
    TEST_ASSERT_EQUAL_STRING("Failed", reasonBuf);

    StatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.deviceName = "mon";
    snap.fwVersion = "1.0.0";
    snap.gridState = GRID_ONLINE;
    snap.mode = MODE_ARMED;
    snap.snoozeDefaultMin = 30;

    char buf[1280];
    buildStatusJsonBuf(snap, buf, sizeof(buf));
    {
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        TEST_ASSERT_TRUE(doc["notify"].is<JsonObject>());
        TEST_ASSERT_TRUE(doc["notify"]["local"].isNull());
        TEST_ASSERT_TRUE(doc["notify"]["cloud"].isNull());
    }

    snap.notifyLocal.valid = true;
    snprintf(snap.notifyLocal.event, sizeof(snap.notifyLocal.event), "TEST_ALARM");
    snap.notifyLocal.ok = true;
    snap.notifyLocal.code = 200;
    snap.notifyLocal.ageS = 120;

    snap.notifyCloud.valid = true;
    snprintf(snap.notifyCloud.event, sizeof(snap.notifyCloud.event), "POWER_OFF");
    snap.notifyCloud.ok = false;
    snap.notifyCloud.code = 500;
    snprintf(snap.notifyCloud.reason, sizeof(snap.notifyCloud.reason), "HTTP 500");
    snap.notifyCloud.ageS = 45;

    buildStatusJsonBuf(snap, buf, sizeof(buf));
    {
        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
        TEST_ASSERT_EQUAL_STRING("TEST_ALARM", doc["notify"]["local"]["event"].as<const char*>());
        TEST_ASSERT_TRUE(doc["notify"]["local"]["ok"].as<bool>());
        TEST_ASSERT_EQUAL_INT(200, doc["notify"]["local"]["code"].as<int>());
        TEST_ASSERT_EQUAL_STRING("", doc["notify"]["local"]["reason"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(120, doc["notify"]["local"]["age_s"].as<uint32_t>());

        TEST_ASSERT_EQUAL_STRING("POWER_OFF", doc["notify"]["cloud"]["event"].as<const char*>());
        TEST_ASSERT_FALSE(doc["notify"]["cloud"]["ok"].as<bool>());
        TEST_ASSERT_EQUAL_INT(500, doc["notify"]["cloud"]["code"].as<int>());
        TEST_ASSERT_EQUAL_STRING("HTTP 500", doc["notify"]["cloud"]["reason"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(45, doc["notify"]["cloud"]["age_s"].as<uint32_t>());
    }
}

void test_P21_notify_status_generation_ignores_stale_results(void) {
    NotifyStatusTracker tracker;

    const uint32_t localGen0 = tracker.getGeneration(NOTIFY_TARGET_LOCAL);
    const uint32_t cloudGen0 = tracker.getGeneration(NOTIFY_TARGET_CLOUD);
    TEST_ASSERT_TRUE(localGen0 > 0);
    TEST_ASSERT_TRUE(cloudGen0 > 0);
    TEST_ASSERT_EQUAL_UINT32(0, tracker.getGeneration((NotifyTarget)99));
    TEST_ASSERT_FALSE(tracker.recordResult((NotifyTarget)99, 0, "POWER_OFF", true, 200, "", 100));

    // Enqueue under localGen0, then change local URL -> clearTarget increments local only
    tracker.clearTarget(NOTIFY_TARGET_LOCAL);
    const uint32_t localGen1 = tracker.getGeneration(NOTIFY_TARGET_LOCAL);
    TEST_ASSERT_EQUAL_UINT32(localGen0 + 1, localGen1);
    TEST_ASSERT_EQUAL_UINT32(cloudGen0, tracker.getGeneration(NOTIFY_TARGET_CLOUD));

    // Stale completion from localGen0 must not resurrect the cleared slot
    TEST_ASSERT_FALSE(tracker.recordResult(NOTIFY_TARGET_LOCAL, localGen0, "POWER_OFF", false, -11, "Read timeout", 2000));
    TEST_ASSERT_FALSE(tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 3000).valid);

    // Cloud generation was unaffected and still accepts cloudGen0
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_CLOUD, cloudGen0, "POWER_OFF", true, 200, "", 2500));
    NotifyStatusSnapshot cloudSnap = tracker.getSnapshot(NOTIFY_TARGET_CLOUD, true, 3500);
    TEST_ASSERT_TRUE(cloudSnap.valid);
    TEST_ASSERT_EQUAL_STRING("POWER_OFF", cloudSnap.event);

    // New local URL result under localGen1 succeeds, and another stale write cannot overwrite it
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_LOCAL, localGen1, "TEST_ALARM", true, 200, "", 4000));
    TEST_ASSERT_FALSE(tracker.recordResult(NOTIFY_TARGET_LOCAL, localGen0, "POWER_OFF", false, 500, "", 5000));
    NotifyStatusSnapshot localSnap = tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 6000);
    TEST_ASSERT_TRUE(localSnap.valid);
    TEST_ASSERT_EQUAL_STRING("TEST_ALARM", localSnap.event);
    TEST_ASSERT_TRUE(localSnap.ok);

    // resetAll increments both generations and rejects prior generations for both targets
    tracker.resetAll();
    TEST_ASSERT_EQUAL_UINT32(localGen1 + 1, tracker.getGeneration(NOTIFY_TARGET_LOCAL));
    TEST_ASSERT_EQUAL_UINT32(cloudGen0 + 1, tracker.getGeneration(NOTIFY_TARGET_CLOUD));
    TEST_ASSERT_FALSE(tracker.recordResult(NOTIFY_TARGET_LOCAL, localGen1, "TEST_ALARM", true, 200, "", 7000));
    TEST_ASSERT_FALSE(tracker.recordResult(NOTIFY_TARGET_CLOUD, cloudGen0, "POWER_OFF", true, 200, "", 7000));
    TEST_ASSERT_FALSE(tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 8000).valid);
    TEST_ASSERT_FALSE(tracker.getSnapshot(NOTIFY_TARGET_CLOUD, true, 8000).valid);
}

void test_P22_should_use_ntfy_format_requires_template_and_host(void) {
    // Template check
    TEST_ASSERT_TRUE(hasNtfyTemplate("POWER_OFF"));
    TEST_ASSERT_TRUE(hasNtfyTemplate("POWER_ON"));
    TEST_ASSERT_TRUE(hasNtfyTemplate("TEST_ALARM"));
    TEST_ASSERT_FALSE(hasNtfyTemplate("ALARM_STOP"));
    TEST_ASSERT_FALSE(hasNtfyTemplate("UNKNOWN_EVENT"));
    TEST_ASSERT_FALSE(hasNtfyTemplate("power_off"));
    TEST_ASSERT_FALSE(hasNtfyTemplate(""));
    TEST_ASSERT_FALSE(hasNtfyTemplate(nullptr));

    // Combined host + template check
    TEST_ASSERT_TRUE(shouldUseNtfyFormat("https://ntfy.sh/power-test", "POWER_OFF"));
    TEST_ASSERT_TRUE(shouldUseNtfyFormat("https://ntfy.sh/power-test", "POWER_ON"));
    TEST_ASSERT_TRUE(shouldUseNtfyFormat("http://NTFY.SH:8080/power-test", "TEST_ALARM"));

    // ALARM_STOP (and non-template events) sent to ntfy.sh must use JSON POST, not ntfy plain text
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("https://ntfy.sh/power-test", "ALARM_STOP"));
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("https://ntfy.sh/power-test", "UNKNOWN_EVENT"));
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("https://ntfy.sh/power-test", ""));
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("https://ntfy.sh/power-test", nullptr));

    // Non-ntfy hosts never use ntfy format even for template events
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("http://192.168.1.60:8080/power_off", "POWER_OFF"));
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("https://ntfy.sh.evil.com/topic", "POWER_OFF"));
    TEST_ASSERT_FALSE(shouldUseNtfyFormat("", "POWER_OFF"));
    TEST_ASSERT_FALSE(shouldUseNtfyFormat(nullptr, "POWER_OFF"));
}

void test_P27_webhook_and_watchdog_timing_invariants(void) {
    const uint32_t handshakeMs = WEBHOOK_TLS_HANDSHAKE_TIMEOUT_S * 1000UL;
    const uint32_t wdtMs = WDT_TIMEOUT_SECONDS * 1000UL;

    // TLS handshake budget must exceed the 4 s TCP/HTTP timeout (measured ESP32 ntfy.sh ~5.9-6.5 s)
    TEST_ASSERT_GREATER_THAN_UINT32(WEBHOOK_TIMEOUT_MS, handshakeMs);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(10000UL, handshakeMs);

    // Worst-case single blocking http.POST() (host lookup inside connect() on a DNS cache miss +
    // TCP connect + handshake + 1 SO_RCVTIMEO tail + HTTP read) must stay below WDT_TIMEOUT_SECONDS
    // with at least 5 s margin.
    const uint32_t httpsWorstBlockMs = DNS_LOOKUP_MAX_MS + WEBHOOK_TIMEOUT_MS + handshakeMs +
                                       WEBHOOK_TIMEOUT_MS + WEBHOOK_TIMEOUT_MS;
    TEST_ASSERT_LESS_THAN_UINT32(wdtMs, httpsWorstBlockMs + 5000UL);
}

void test_P31_cloud_off_routing_and_status_null(void) {
    // Cloud disabled -> notify.cloud snapshot is invalid -> status JSON serializes null
    NotifyStatusTracker tracker;
    const uint32_t cloudGen = tracker.getGeneration(NOTIFY_TARGET_CLOUD);
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_CLOUD, cloudGen, "TEST_ALARM", true, 200, "", 1000));
    TEST_ASSERT_TRUE(tracker.getSnapshot(NOTIFY_TARGET_CLOUD, true, 5000).valid);

    StatusSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.deviceName = "mon";
    snap.fwVersion = FW_VERSION;
    snap.gridState = GRID_ONLINE;
    snap.mode = MODE_ARMED;
    snap.snoozeDefaultMin = 30;
    snap.notifyCloud = tracker.getSnapshot(NOTIFY_TARGET_CLOUD, false, 5000);
    TEST_ASSERT_FALSE(snap.notifyCloud.valid);

    char buf[1280];
    buildStatusJsonBuf(snap, buf, sizeof(buf));
    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, buf).code());
    TEST_ASSERT_TRUE(doc["notify"]["cloud"].isNull());
}

void test_P39_immediate_notify_config_apply_and_url_change(void) {
    // (a) Saving local_url makes immediate TEST_ALARM and POWER_OFF queue to the new URL
    DeviceConfig cfg = makeDefaultTestConfig();
    NotifyStatusTracker tracker;

    DeviceConfig next = cfg;
    snprintf(next.local_url, sizeof(next.local_url), "http://192.168.1.60:8080/{event}");

    bool clearLocal = false;
    bool clearCloud = false;
    computeNotifyTargetChanges(cfg, next, clearLocal, clearCloud);
    TEST_ASSERT_TRUE(clearLocal);
    if (clearLocal) tracker.clearTarget(NOTIFY_TARGET_LOCAL);
    if (clearCloud) tracker.clearTarget(NOTIFY_TARGET_CLOUD);
    cfg = next;

    char jsonBuf[512];
    char errReason[64];
    NotifyPayload testPayload;
    buildTestAlarmJsonBuf(cfg.device_name, MODE_ARMED, 10, 0, jsonBuf, sizeof(jsonBuf));
    TEST_ASSERT_EQUAL_INT(
        NOTIFY_PREP_READY,
        prepareNotifyPayload(cfg, NOTIFY_TARGET_LOCAL, tracker.getGeneration(NOTIFY_TARGET_LOCAL),
                             "TEST_ALARM", 0, jsonBuf, testPayload, errReason, sizeof(errReason)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/test_alarm", testPayload.url);

    AlertRouter router;
    FakeAlertOutput alertOut;
    router.handlePowerOff(cfg.device_name, MODE_ARMED, 0, 12, 0, true, alertOut);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned int)alertOut.notifications.size());

    NotifyPayload offLocalPayload;
    TEST_ASSERT_EQUAL_INT(
        NOTIFY_PREP_READY,
        prepareNotifyPayload(cfg, NOTIFY_TARGET_LOCAL, tracker.getGeneration(NOTIFY_TARGET_LOCAL),
                             alertOut.notifications[0].event.c_str(),
                             alertOut.notifications[0].durationS,
                             alertOut.notifications[0].jsonBody.c_str(),
                             offLocalPayload, errReason, sizeof(errReason)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.60:8080/power_off", offLocalPayload.url);

    NotifyPayload offCloudPayload;
    TEST_ASSERT_EQUAL_INT(
        NOTIFY_PREP_SKIP,
        prepareNotifyPayload(cfg, NOTIFY_TARGET_CLOUD, tracker.getGeneration(NOTIFY_TARGET_CLOUD),
                             alertOut.notifications[1].event.c_str(),
                             alertOut.notifications[1].durationS,
                             alertOut.notifications[1].jsonBody.c_str(),
                             offCloudPayload, errReason, sizeof(errReason)));
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_LOCAL, offLocalPayload.urlGeneration,
                                          "POWER_OFF", true, 200, "", 1000));

    // (b) Changing local_url bumps generation synchronously; an immediate event uses the new
    // generation and is not dropped when recorded, while an older generation is rejected.
    const uint32_t genBefore = tracker.getGeneration(NOTIFY_TARGET_LOCAL);
    DeviceConfig next2 = cfg;
    snprintf(next2.local_url, sizeof(next2.local_url), "http://192.168.1.99:8080/{event}");
    next2.cloud_enabled = true;
    snprintf(next2.webhook_url, sizeof(next2.webhook_url), "https://ntfy.sh/test-topic");

    computeNotifyTargetChanges(cfg, next2, clearLocal, clearCloud);
    TEST_ASSERT_TRUE(clearLocal);
    TEST_ASSERT_TRUE(clearCloud);
    if (clearLocal) tracker.clearTarget(NOTIFY_TARGET_LOCAL);
    if (clearCloud) tracker.clearTarget(NOTIFY_TARGET_CLOUD);
    cfg = next2;

    const uint32_t genAfter = tracker.getGeneration(NOTIFY_TARGET_LOCAL);
    TEST_ASSERT_NOT_EQUAL(genBefore, genAfter);

    alertOut.clear();
    router.handlePowerOff(cfg.device_name, MODE_ARMED, 3, 20, 0, true, alertOut);
    NotifyPayload updatedPayload;
    TEST_ASSERT_EQUAL_INT(
        NOTIFY_PREP_READY,
        prepareNotifyPayload(cfg, NOTIFY_TARGET_LOCAL, genAfter,
                             alertOut.notifications[0].event.c_str(),
                             alertOut.notifications[0].durationS,
                             alertOut.notifications[0].jsonBody.c_str(),
                             updatedPayload, errReason, sizeof(errReason)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.99:8080/power_off", updatedPayload.url);
    TEST_ASSERT_EQUAL_UINT32(genAfter, updatedPayload.urlGeneration);

    // Prior in-flight result is dropped; immediate post-save event is recorded.
    TEST_ASSERT_FALSE(tracker.recordResult(NOTIFY_TARGET_LOCAL, genBefore, "TEST_ALARM", true, 200, "", 1500));
    TEST_ASSERT_TRUE(tracker.recordResult(NOTIFY_TARGET_LOCAL, updatedPayload.urlGeneration,
                                          "POWER_OFF", true, 200, "", 2000));
    NotifyStatusSnapshot snap = tracker.getSnapshot(NOTIFY_TARGET_LOCAL, true, 3000);
    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL_STRING("POWER_OFF", snap.event);
    TEST_ASSERT_TRUE(snap.ok);
    TEST_ASSERT_EQUAL_INT(200, snap.code);

    // Cloud enabled synchronously also allows immediate cloud queuing.
    TEST_ASSERT_EQUAL_INT(
        NOTIFY_PREP_READY,
        prepareNotifyPayload(cfg, NOTIFY_TARGET_CLOUD, tracker.getGeneration(NOTIFY_TARGET_CLOUD),
                             alertOut.notifications[1].event.c_str(),
                             alertOut.notifications[1].durationS,
                             alertOut.notifications[1].jsonBody.c_str(),
                             offCloudPayload, errReason, sizeof(errReason)));
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/test-topic", offCloudPayload.url);
}

void test_P39_tls_error_reason_formatting(void) {
    char tlsReason[64];
    char finalReason[64];

    // No TLS error recorded -> falls back to standard -1 mapping ("Connection refused")
    formatTlsErrorReason(0, "", tlsReason, sizeof(tlsReason));
    TEST_ASSERT_EQUAL_STRING("", tlsReason);
    formatNotifyFailureReason(-1, tlsReason, finalReason, sizeof(finalReason));
    TEST_ASSERT_EQUAL_STRING("Connection refused", finalReason);

    // start_ssl_client -1 sentinel -> concise TLS connect/handshake failure
    formatTlsErrorReason(-1, "UNKNOWN ERROR CODE (0001)", tlsReason, sizeof(tlsReason));
    formatNotifyFailureReason(-1, tlsReason, finalReason, sizeof(finalReason));
    TEST_ASSERT_EQUAL_STRING("TLS: connect or handshake failed", finalReason);

    // Real mbedTLS error code -> prefixed and truncated cleanly to 63 chars
    formatTlsErrorReason(-9984, "X509 - Certificate verification failed", tlsReason, sizeof(tlsReason));
    formatNotifyFailureReason(-1, tlsReason, finalReason, sizeof(finalReason));
    TEST_ASSERT_EQUAL_STRING("TLS: X509 - Certificate verification failed", finalReason);

    formatTlsErrorReason(
        -9984,
        "X509 - Certificate verification failed, e.g. CRL, CA or signature check failed everywhere",
        tlsReason, sizeof(tlsReason));
    formatNotifyFailureReason(-1, tlsReason, finalReason, sizeof(finalReason));
    TEST_ASSERT_EQUAL_UINT(sizeof(finalReason) - 1, (unsigned int)strlen(finalReason));
    TEST_ASSERT_EQUAL_INT(0, strncmp(finalReason, "TLS: X509 - ", 12));

    // Empty mbedTLS text with non-zero code -> numeric TLS fallback
    formatTlsErrorReason(-29184, "", tlsReason, sizeof(tlsReason));
    formatNotifyFailureReason(-1, tlsReason, finalReason, sizeof(finalReason));
    TEST_ASSERT_EQUAL_STRING("TLS error -29184", finalReason);
}

void test_P40_dropped_power_on_and_alarm_stop_sent_after_reconnect(void) {
    // 1. Dropped POWER_ON on local target is flushed once after STA reconnects
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_ON", 185, ALERT_OUTAGE_ONLY);

        // Still disconnected -> no-op
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 0, 300, 1700000300, false, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        // Reconnect while online -> sends POWER_ON with original outage duration_s
        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 0, 310, 1700000310, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_UINT32(185, out.notifications[0].durationS);

        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, out.notifications[0].jsonBody).code());
        TEST_ASSERT_EQUAL_STRING("POWER_ON", doc["event"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("ONLINE", doc["grid"].as<const char*>());
        TEST_ASSERT_TRUE(doc["power"].as<bool>());
        TEST_ASSERT_EQUAL_UINT32(185, doc["duration_s"].as<uint32_t>());
        TEST_ASSERT_EQUAL_STRING("OUTAGE_ONLY", doc["alert_mode"].as<const char*>());

        // Second flush does not duplicate
        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 0, 311, 1700000311, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
    }

    // 2. Dropped ALARM_STOP (via handleAlarmStop + worker timeout) is flushed on reconnect, TEST_ALARM is ignored
    {
        AlertRouter router;
        FakeAlertOutput out;
        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_SNOOZED, 1800, 100, 0, out));
        out.clear();

        const uint32_t seq = router.getTargetStateSeq(NOTIFY_TARGET_LOCAL);
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "ALARM_STOP", 0, ALERT_OUTAGE_ONLY, seq);
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "TEST_ALARM", 0, ALERT_OUTAGE_ONLY);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_SNOOZED, 20, 115, 1700000115, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", out.notifications[0].event.c_str());

        JsonDocument doc;
        TEST_ASSERT_EQUAL_INT((int)DeserializationError::Ok, (int)deserializeJson(doc, out.notifications[0].jsonBody).code());
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", doc["event"].as<const char*>());
        TEST_ASSERT_EQUAL_STRING("SNOOZED", doc["state"].as<const char*>());
        TEST_ASSERT_EQUAL_UINT32(1800, doc["snooze_remaining_s"].as<uint32_t>());
    }
}

void test_P40_latest_event_wins_while_offline_and_cloud_routing_preserved(void) {
    // 1. POWER_OFF then POWER_ON while offline yields ONLY POWER_ON (OUTAGE_ONLY -> local only, cloud POWER_OFF cleared)
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 1, 100, 0, false, out);
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_ON", 45, ALERT_OUTAGE_ONLY);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 0, 155, 1700000155, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_UINT32(45, out.notifications[0].durationS);
    }

    // 2. POWER_OFF then POWER_ON while offline with ALERT_OUTAGE_AND_RESTORE yields ONLY POWER_ON on both targets,
    // and prepareNotifyPayload preserves cloud_enabled gating.
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_OFF", 2);
        router.markPendingTargetEvent(NOTIFY_TARGET_CLOUD, "POWER_OFF", 2);
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_ON", 90, ALERT_OUTAGE_AND_RESTORE);
        router.markPendingTargetEvent(NOTIFY_TARGET_CLOUD, "POWER_ON", 90, ALERT_OUTAGE_AND_RESTORE);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 0, 210, 1700000210, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[1].event.c_str());
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_LOCAL));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.countForTarget(NOTIFY_TARGET_CLOUD));

        // Cloud routing preserved through prepareNotifyPayload: skipped when cloud_enabled is false, ready when true
        DeviceConfig cfg = makeDefaultTestConfig();
        snprintf(cfg.local_url, sizeof(cfg.local_url), "http://192.168.1.60:8080/{event}");
        snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "https://ntfy.sh/topic");
        cfg.alert_mode = ALERT_OUTAGE_AND_RESTORE;
        cfg.cloud_enabled = false;

        NotifyPayload payload;
        char errReason[64];
        TEST_ASSERT_EQUAL_INT(
            NOTIFY_PREP_SKIP,
            prepareNotifyPayload(cfg, NOTIFY_TARGET_CLOUD, 1,
                                 out.notifications[1].event.c_str(),
                                 out.notifications[1].durationS,
                                 out.notifications[1].jsonBody.c_str(),
                                 payload, errReason, sizeof(errReason)));
        cfg.cloud_enabled = true;
        TEST_ASSERT_EQUAL_INT(
            NOTIFY_PREP_READY,
            prepareNotifyPayload(cfg, NOTIFY_TARGET_CLOUD, 1,
                                 out.notifications[1].event.c_str(),
                                 out.notifications[1].durationS,
                                 out.notifications[1].jsonBody.c_str(),
                                 payload, errReason, sizeof(errReason)));
    }

    // 3. POWER_ON then POWER_OFF while offline yields ONLY POWER_OFF
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_ON", 60, ALERT_OUTAGE_AND_RESTORE);
        router.markPendingTargetEvent(NOTIFY_TARGET_CLOUD, "POWER_ON", 60, ALERT_OUTAGE_AND_RESTORE);

        // Subsequent outage while still offline replaces pending POWER_ON on both targets
        router.handlePowerOff("mon", MODE_ARMED, 3, 180, 0, false, out);
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());

        // Stale worker timeout from the earlier POWER_ON (seq 1 < current seq) cannot overwrite POWER_OFF
        router.markPendingTargetEvent(NOTIFY_TARGET_LOCAL, "POWER_ON", 60, ALERT_OUTAGE_AND_RESTORE, 1);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 14, 195, 1700000195, true, out));
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[1].event.c_str());
        TEST_ASSERT_EQUAL_UINT32(14, out.notifications[0].durationS);
        TEST_ASSERT_EQUAL_UINT32(14, out.notifications[1].durationS);
    }
}

void test_P41_final_attempt_wifi_drop_marks_pending_and_connected_failure_does_not(void) {
    // 1. POWER_OFF dispatched while Wi-Fi connected, then Wi-Fi drops during the final attempt on CLOUD ->
    //    CLOUD is marked pending and re-sent on reconnect; LOCAL (which succeeded) is not duplicated.
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 0, 100, 1700000100, true, out);
        TEST_ASSERT_EQUAL_UINT(2, (unsigned int)out.notifications.size());
        out.clear();

        const uint32_t cloudSeq = router.getTargetStateSeq(NOTIFY_TARGET_CLOUD);
        router.handleAttemptsExhausted(NOTIFY_TARGET_CLOUD, "POWER_OFF", 0, ALERT_OUTAGE_ONLY, cloudSeq, false);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 28, 128, 1700000128, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_CLOUD, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("POWER_OFF", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_UINT32(28, out.notifications[0].durationS);
    }

    // 2. Latest-wins and state events (POWER_ON, ALARM_STOP) when Wi-Fi drops during the final attempt
    {
        AlertRouter router;
        FakeAlertOutput out;

        // POWER_OFF fails on final attempt with Wi-Fi down -> pending POWER_OFF on both targets
        router.handlePowerOff("mon", MODE_ARMED, 0, 100, 0, true, out);
        const uint32_t offLocalSeq = router.getTargetStateSeq(NOTIFY_TARGET_LOCAL);
        const uint32_t offCloudSeq = router.getTargetStateSeq(NOTIFY_TARGET_CLOUD);
        out.clear();
        router.handleAttemptsExhausted(NOTIFY_TARGET_LOCAL, "POWER_OFF", 0, ALERT_OUTAGE_ONLY, offLocalSeq, false);
        router.handleAttemptsExhausted(NOTIFY_TARGET_CLOUD, "POWER_OFF", 0, ALERT_OUTAGE_ONLY, offCloudSeq, false);

        // Later POWER_ON also fails on final attempt with Wi-Fi down (OUTAGE_ONLY) ->
        // replaces pending POWER_OFF on LOCAL and clears pending POWER_OFF on CLOUD
        router.handlePowerOn("mon", MODE_ARMED, 55, 155, ALERT_OUTAGE_ONLY, 0, out, true);
        const uint32_t onLocalSeq = router.getTargetStateSeq(NOTIFY_TARGET_LOCAL);
        out.clear();
        router.handleAttemptsExhausted(NOTIFY_TARGET_LOCAL, "POWER_ON", 55, ALERT_OUTAGE_ONLY, onLocalSeq, false);

        // Stale final-attempt completion from the earlier POWER_OFF (offLocalSeq < onLocalSeq) is ignored
        router.handleAttemptsExhausted(NOTIFY_TARGET_LOCAL, "POWER_OFF", 0, ALERT_OUTAGE_ONLY, offLocalSeq, false);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", true, MODE_ARMED, 0, 170, 1700000170, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("POWER_ON", out.notifications[0].event.c_str());
        TEST_ASSERT_EQUAL_UINT32(55, out.notifications[0].durationS);

        // ALARM_STOP failing on final attempt with Wi-Fi down is also remembered and flushed on reconnect
        out.clear();
        TEST_ASSERT_TRUE(router.handleAlarmStop("mon", MODE_ARMED, MODE_SNOOZED, 1200, 200, 0, out, true));
        const uint32_t stopSeq = router.getTargetStateSeq(NOTIFY_TARGET_LOCAL);
        out.clear();
        router.handleAttemptsExhausted(NOTIFY_TARGET_LOCAL, "ALARM_STOP", 0, ALERT_OUTAGE_ONLY, stopSeq, false);

        TEST_ASSERT_TRUE(router.flushPendingOnReconnect("mon", false, MODE_SNOOZED, 10, 215, 1700000215, true, out));
        TEST_ASSERT_EQUAL_UINT(1, (unsigned int)out.notifications.size());
        TEST_ASSERT_EQUAL_INT((int)NOTIFY_TARGET_LOCAL, (int)out.notifications[0].target);
        TEST_ASSERT_EQUAL_STRING("ALARM_STOP", out.notifications[0].event.c_str());
    }

    // 3. Exhausted attempts while Wi-Fi remains connected (target down / HTTP error) do NOT mark pending
    {
        AlertRouter router;
        FakeAlertOutput out;
        router.handlePowerOff("mon", MODE_ARMED, 0, 100, 0, true, out);
        const uint32_t seqLocal = router.getTargetStateSeq(NOTIFY_TARGET_LOCAL);
        const uint32_t seqCloud = router.getTargetStateSeq(NOTIFY_TARGET_CLOUD);
        out.clear();

        router.handleAttemptsExhausted(NOTIFY_TARGET_LOCAL, "POWER_OFF", 0, ALERT_OUTAGE_AND_RESTORE, seqLocal, true);
        router.handleAttemptsExhausted(NOTIFY_TARGET_CLOUD, "POWER_OFF", 0, ALERT_OUTAGE_AND_RESTORE, seqCloud, true);

        TEST_ASSERT_FALSE(router.flushPendingOnReconnect("mon", false, MODE_ARMED, 30, 130, 0, true, out));
        TEST_ASSERT_EQUAL_UINT(0, (unsigned int)out.notifications.size());
    }
}

void test_P42_restore_cooldown_after_long_outage_flicker_boundaries_and_wrap(void) {
    // 1. Long outage (> restore_cooldown_ms): flickers shorter than cooldown do not confirm;
    //    any LOW restarts the window; -1 ms / +0 ms boundaries; duration_s spans outage start to confirmed restore.
    {
        FakeClock clk(1000);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        g_lastRestoredDur = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, DEFAULT_DEBOUNCE_MS, DEFAULT_COOLDOWN_MS, &FakeGpio::readPin, &clk);

        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());

        // Confirm outage at t = 2000 ms (_outageStartMs = 2000)
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        ps.update();
        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        // Stay offline for 120 s (well beyond the 5 s restore_cooldown_ms)
        clk.advance(120000);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_UINT32(120, ps.getOutageDurationSeconds());

        // Flicker 1: HIGH for debounce_ms (500 ms), then LOW for 200 ms -> must NOT confirm POWER_ON
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        clk.advance(200);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());

        // Flicker 2: HIGH for restore_cooldown_ms - 1 (4999 ms), then LOW for 300 ms -> must restart window
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(DEFAULT_COOLDOWN_MS - 1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        clk.advance(300);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        // Stable HIGH: -1 ms boundary stays OFFLINE, +0 ms boundary confirms ONLINE
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(DEFAULT_COOLDOWN_MS - 1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_restoreCbCount);
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);
        // Total elapsed since _outageStartMs: 120000 + 500 + 200 + 4999 + 300 + 5000 = 130999 ms -> 130 s
        TEST_ASSERT_EQUAL_UINT32(130, g_lastRestoredDur);
        TEST_ASSERT_EQUAL_UINT32(130, ps.getOutageDurationSeconds());
    }

    // 2. max(debounce_ms, restore_cooldown_ms) when debounce_ms > restore_cooldown_ms (2000 ms vs 500 ms)
    {
        FakeClock clk(5000);
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, 2000, 500, &FakeGpio::readPin, &clk);

        clk.advance(2000);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());

        clk.advance(30000);
        ps.update();

        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(1999);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_restoreCbCount);
    }

    // 3. millis() wrap-around during long outage, flicker, and cooldown boundary (-1 ms / +0 ms)
    {
        // Start 70 s before 32-bit rollover
        FakeClock clk(0xFFFFFFFFUL - 70000UL);
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        PowerSensor ps;
        g_outageCbCount = 0;
        g_restoreCbCount = 0;
        g_lastRestoredDur = 0;
        ps.onOutage(onOutageHook);
        ps.onRestored(onRestoredHook);
        ps.init(PIN_MAINS_SENSE, DEFAULT_DEBOUNCE_MS, DEFAULT_COOLDOWN_MS, &FakeGpio::readPin, &clk);

        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());

        // Confirm outage 69 s before wrap
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        ps.update();
        clk.advance(DEFAULT_DEBOUNCE_MS);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_outageCbCount);

        // Advance 64 s (now 5 s before 0xFFFFFFFF)
        clk.advance(64000);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());

        // Flicker HIGH for 2000 ms, then LOW for 1000 ms (now 2 s before 0xFFFFFFFF)
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(2000);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        FakeGpio::setPin(PIN_MAINS_SENSE, LOW);
        clk.advance(1000);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());

        // Start continuous HIGH 2 s before 0xFFFFFFFF -> wraps past 0x00000000
        FakeGpio::setPin(PIN_MAINS_SENSE, HIGH);
        ps.update();
        clk.advance(DEFAULT_COOLDOWN_MS - 1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_OFFLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(0, g_restoreCbCount);

        clk.advance(1);
        ps.update();
        TEST_ASSERT_EQUAL_INT(GRID_ONLINE, ps.getGridState());
        TEST_ASSERT_EQUAL_INT(1, g_restoreCbCount);
        // Total outage: 64000 + 2000 + 1000 + 5000 = 72000 ms -> 72 s
        TEST_ASSERT_EQUAL_UINT32(72, g_lastRestoredDur);
        TEST_ASSERT_EQUAL_UINT32(72, ps.getOutageDurationSeconds());
    }
}

void test_P43_state_events_evict_from_full_queue_test_alarm_does_not(void) {
    // Decides which incoming event may evict the oldest item from a full per-target queue
    TEST_ASSERT_TRUE(isStateNotifyEvent("POWER_OFF"));
    TEST_ASSERT_TRUE(isStateNotifyEvent("POWER_ON"));
    TEST_ASSERT_TRUE(isStateNotifyEvent("ALARM_STOP"));
    TEST_ASSERT_FALSE(isStateNotifyEvent("TEST_ALARM"));
    TEST_ASSERT_FALSE(isStateNotifyEvent("power_off"));
    TEST_ASSERT_FALSE(isStateNotifyEvent("POWER_OFF_X"));
    TEST_ASSERT_FALSE(isStateNotifyEvent(""));
    TEST_ASSERT_FALSE(isStateNotifyEvent(nullptr));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_P1_power_sensor_debounce_cooldown_flapping_boot_wrap);
    RUN_TEST(test_P2_outage_while_sta_down_queues_pending_and_flushes_once);
    RUN_TEST(test_P3_pending_alert_fires_only_if_still_offline_and_armed);
    RUN_TEST(test_P5_pending_webhook_outage_resends_webhook_only);
    RUN_TEST(test_P4_disarmed_snoozed_suppress_power_off_and_power_on_mode_rules);
    RUN_TEST(test_P6_power_sensor_debounce_and_cooldown_exact_boundaries);
    RUN_TEST(test_B1_button_press_classification_and_held_led_feedback);
    RUN_TEST(test_B2_led_patterns_setup_ap_timing_and_outage_priority);
    RUN_TEST(test_helpers_extract_scheme_host_sanitizes_path_query_userinfo);
    RUN_TEST(test_P7_boot_state_machine_low_high_and_bounce_restarts_window);
    RUN_TEST(test_P8_entering_armed_alerts_only_from_non_armed_when_grid_offline);
    RUN_TEST(test_P9_snooze_mode_gate_limits_and_parsers);
    RUN_TEST(test_P10_payload_builders_contract_fields_and_ts_zero);
    RUN_TEST(test_P11_expand_notification_url_placeholders_and_overflow);
    RUN_TEST(test_P12_extract_url_host_and_ntfy_detection);
    RUN_TEST(test_P13_should_notify_target_routing_matrix);
    RUN_TEST(test_P14_status_fw_version_and_ota_guard_decision);
    RUN_TEST(test_P15_is_ip_literal);
    RUN_TEST(test_P17_alarm_stop_decision_matrix);
    RUN_TEST(test_P18_alarm_stop_builder_and_local_only_routing);
    RUN_TEST(test_P19_notify_status_tracker_and_age_wrap);
    RUN_TEST(test_P20_status_json_notify_fields_and_reason_format);
    RUN_TEST(test_P21_notify_status_generation_ignores_stale_results);
    RUN_TEST(test_P22_should_use_ntfy_format_requires_template_and_host);
    RUN_TEST(test_P27_webhook_and_watchdog_timing_invariants);
    RUN_TEST(test_P31_cloud_off_routing_and_status_null);
    RUN_TEST(test_P39_immediate_notify_config_apply_and_url_change);
    RUN_TEST(test_P39_tls_error_reason_formatting);
    RUN_TEST(test_P40_dropped_power_on_and_alarm_stop_sent_after_reconnect);
    RUN_TEST(test_P40_latest_event_wins_while_offline_and_cloud_routing_preserved);
    RUN_TEST(test_P41_final_attempt_wifi_drop_marks_pending_and_connected_failure_does_not);
    RUN_TEST(test_P42_restore_cooldown_after_long_outage_flicker_boundaries_and_wrap);
    RUN_TEST(test_P43_state_events_evict_from_full_queue_test_alarm_does_not);
    return UNITY_END();
}
