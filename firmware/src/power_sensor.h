#ifndef POWER_SENSOR_H
#define POWER_SENSOR_H

#include "config.h"
#include "hal.h"

// Callback type definitions
typedef void (*OutageCallback)();
typedef void (*RestoredCallback)(uint32_t durationSeconds);
typedef int (*GpioReadFn)(uint8_t pin);

class PowerSensor {
public:
    PowerSensor();

    // Initialize GPIO pin and initial state (injectable readFn & clock for host testing)
    void init(uint8_t sensePin,
              uint32_t debounceMs,
              uint32_t cooldownMs,
              GpioReadFn readFn = nullptr,
              const IClock* clock = nullptr);

    // Main non-blocking update loop (call frequently from loop())
    void update();

    // Current grid status
    GridState getGridState() const;

    // Outage duration: elapsed seconds if currently offline, or duration of last outage
    uint32_t getOutageDurationSeconds() const;

    // Configuration
    void setDebounceMs(uint32_t debounceMs);
    void setCooldownMs(uint32_t cooldownMs);

    // Event hooks
    void onOutage(OutageCallback callback);
    void onRestored(RestoredCallback callback);

private:
    uint8_t _sensePin;
    uint32_t _debounceMs;
    uint32_t _cooldownMs;
    GridState _currentState;

    bool _inTransition;
    int _candidateRawState;
    uint32_t _transitionStartMs;

    bool _outageTimingActive;
    uint32_t _outageStartMs;
    uint32_t _lastOutageDurationS;

    GpioReadFn _readFn;
    const IClock* _clock;

    OutageCallback _outageCallback;
    RestoredCallback _restoredCallback;

    uint32_t nowMs() const;
    int readPin() const;
};

const char* gridStateStr(GridState state);

#endif // POWER_SENSOR_H
