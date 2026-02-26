#ifndef HX710_H
#define HX710_H

#include <Arduino.h>

class HX710 {
public:
    HX710(int8_t doutPin, int8_t clkPin);

    void begin();
    bool isReady();
    int32_t readRaw();
    float readCalibrated();

    // Two-point linear calibration: value = slope * raw + offset
    void setCalibration(int32_t raw1, float val1, int32_t raw2, float val2);

    float getLastValue() const { return _lastValue; }
    int32_t getLastRaw() const { return _lastRaw; }
    bool isValid() const { return _valid; }

private:
    int8_t _doutPin;
    int8_t _clkPin;

    float _slope;
    float _offset;
    float _lastValue;
    int32_t _lastRaw;
    bool _valid;
};

#endif
