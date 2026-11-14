#include "power_sensor.h"

PowerSensor::PowerSensor()
    : _sensePin(PIN_MAINS_SENSE),
      _debounceMs(DEFAULT_DEBOUNCE_MS),
      _cooldownMs(DEFAULT_COOLDOWN_MS),
      _currentState(GRID_BOOTING),
      _inTransition(false),
      _candidateRawState(LOW),
      _transitionStartMs(0),
      _outageTimingActive(false),
      _outageStartMs(0),
      _lastOutageDurationS(0),
      _readFn(nullptr),
      _clock(nullptr),
      _outageCallback(nullptr),
      _restoredCallback(nullptr) {}

uint32_t PowerSensor::nowMs() const {
    if (_clock) return _clock->millis();
#ifdef ARDUINO
    return ::millis();
#else
    return 0;
#endif
}

int PowerSensor::readPin() const {
    if (_readFn) return _readFn(_sensePin);
#ifdef ARDUINO
    return digitalRead(_sensePin);
#else
    return LOW;
#endif
}

void PowerSensor::init(uint8_t sensePin, uint32_t debounceMs, uint32_t cooldownMs,
                       GpioReadFn readFn, const IClock* clock) {
    _sensePin = sensePin;
    _debounceMs = debounceMs;
    _cooldownMs = cooldownMs;
    _clock = clock;
    _readFn = readFn;
#ifdef ARDUINO
    if (!readFn) {
        pinMode(_sensePin, INPUT_PULLDOWN);
    }
#endif

    _currentState = GRID_BOOTING;
    _outageTimingActive = false;
    _outageStartMs = 0;
    _lastOutageDurationS = 0;

    // Hold GRID_BOOTING until the initial pin level remains stable for the debounce window
    uint32_t now = nowMs();
    _inTransition = true;
    _candidateRawState = readPin();
    _transitionStartMs = now;
}

void PowerSensor::update() {
    int raw = readPin();
    bool stateDiffers = (_currentState == GRID_BOOTING) ||
                        (raw != ((_currentState == GRID_ONLINE) ? HIGH : LOW));

    if (stateDiffers) {
        uint32_t now = nowMs();
        if (!_inTransition) {
            // New transition detected
            _inTransition = true;
            _candidateRawState = raw;
            _transitionStartMs = now;
        } else if (raw == _candidateRawState) {
            // Check debounce threshold AND anti-flapping cooldown guard
            bool debouncePassed = (now - _transitionStartMs >= _debounceMs);
            // Cooldown applies strictly to restoration from a confirmed outage,
            // requiring continuous HIGH for max(_debounceMs, _cooldownMs).
            bool isRestoring = (raw == HIGH && _currentState == GRID_OFFLINE);
            bool cooldownPassed = isRestoring ? (now - _transitionStartMs >= _cooldownMs) : true;

            if (debouncePassed && cooldownPassed) {
                _inTransition = false;
                GridState oldState = _currentState;

                if (raw == HIGH) {
                    _currentState = GRID_ONLINE;
                    uint32_t durationS = 0;
                    if (_outageTimingActive) {
                        durationS = (now - _outageStartMs) / 1000UL;
                    }
                    _lastOutageDurationS = durationS;
                    _outageTimingActive = false;
                    _outageStartMs = 0;

                    if (oldState == GRID_OFFLINE && _restoredCallback) {
                        _restoredCallback(durationS);
                    }
                } else {
                    _currentState = GRID_OFFLINE;
                    _outageTimingActive = true;
                    // For an outage already present at boot, count duration from first LOW sample
                    _outageStartMs = (oldState == GRID_BOOTING) ? _transitionStartMs : now;

                    if ((oldState == GRID_ONLINE || oldState == GRID_BOOTING) && _outageCallback) {
                        _outageCallback();
                    }
                }
            }
        } else {
            // Noise/bounce: reset candidate
            _candidateRawState = raw;
            _transitionStartMs = now;
        }
    } else {
        // State matches current confirmed state: clear transition flag
        _inTransition = false;
    }
}

GridState PowerSensor::getGridState() const {
    return _currentState;
}

const char* gridStateStr(GridState state) {
    switch (state) {
        case GRID_ONLINE:  return "ONLINE";
        case GRID_OFFLINE: return "OFFLINE";
        case GRID_BOOTING:
        default:           return "BOOTING";
    }
}

uint32_t PowerSensor::getOutageDurationSeconds() const {
    if (_currentState == GRID_OFFLINE) {
        if (!_outageTimingActive) return 0;
        return (nowMs() - _outageStartMs) / 1000UL;
    }
    return _lastOutageDurationS;
}

void PowerSensor::setDebounceMs(uint32_t debounceMs) {
    _debounceMs = debounceMs;
}

void PowerSensor::setCooldownMs(uint32_t cooldownMs) {
    _cooldownMs = cooldownMs;
}

void PowerSensor::onOutage(OutageCallback callback) {
    _outageCallback = callback;
}

void PowerSensor::onRestored(RestoredCallback callback) {
    _restoredCallback = callback;
}
