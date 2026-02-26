#include "HX710.h"
#include "Logger.h"

// FreeRTOS spinlock for timing-critical bit-bang
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

HX710::HX710(int8_t doutPin, int8_t clkPin)
    : _doutPin(doutPin)
    , _clkPin(clkPin)
    , _slope(1.0f)
    , _offset(0.0f)
    , _lastValue(0.0f)
    , _lastRaw(0)
    , _valid(false)
{
}

void HX710::begin() {
    pinMode(_clkPin, OUTPUT);
    pinMode(_doutPin, INPUT);
    digitalWrite(_clkPin, LOW);
}

bool HX710::isReady() {
    return digitalRead(_doutPin) == LOW;
}

int32_t HX710::readRaw() {
    if (!isReady()) {
        return _lastRaw;
    }

    int32_t value = 0;

    // Disable interrupts for timing-critical bit-bang
    portENTER_CRITICAL_SAFE(&spinlock);

    // Clock in 24 data bits
    for (int i = 0; i < 24; i++) {
        digitalWrite(_clkPin, HIGH);
        delayMicroseconds(1);
        value = (value << 1) | digitalRead(_doutPin);
        digitalWrite(_clkPin, LOW);
        delayMicroseconds(1);
    }

    // 3 extra clock pulses for Mode 3 (differential input, 40Hz)
    for (int i = 0; i < 3; i++) {
        digitalWrite(_clkPin, HIGH);
        delayMicroseconds(1);
        digitalWrite(_clkPin, LOW);
        delayMicroseconds(1);
    }

    portEXIT_CRITICAL_SAFE(&spinlock);

    // Sign extend from 24-bit to 32-bit
    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    _lastRaw = value;
    _valid = true;
    return value;
}

float HX710::readCalibrated() {
    int32_t raw = readRaw();
    _lastValue = _slope * (float)raw + _offset;
    return _lastValue;
}

void HX710::setCalibration(int32_t raw1, float val1, int32_t raw2, float val2) {
    // Two-point linear calibration: val = slope * raw + offset
    if (raw2 != raw1) {
        _slope = (val2 - val1) / (float)(raw2 - raw1);
        _offset = val1 - _slope * (float)raw1;
    }
    Log.info("HX710", "Calibration: slope=%.8f offset=%.4f (GPIO %d/%d)",
             _slope, _offset, _doutPin, _clkPin);
}
