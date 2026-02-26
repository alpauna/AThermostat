#ifndef THERMOSTAT_H
#define THERMOSTAT_H

#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>
#include "OutPin.h"
#include "InputPin.h"

// User-selectable modes
enum class ThermostatMode : uint8_t {
    OFF = 0,
    HEAT,
    COOL,
    HEAT_COOL,
    FAN_ONLY
};

// System-determined actions
enum class ThermostatAction : uint8_t {
    OFF = 0,
    IDLE,
    HEATING,
    COOLING,
    FAN_RUNNING
};

// Heat levels (escalation stages)
enum class HeatLevel : uint8_t {
    IDLE = 0,
    HP_LOW,       // Heat pump stage 1: fan1+furn_cool_low+comp1
    HP_HIGH,      // Heat pump stage 2: fan1+furn_cool_low+comp1+comp2
    FURNACE_LOW,  // Furnace stage 1: fan1+w1
    FURNACE_HIGH, // Furnace stage 2: fan1+w1+w2
    DEFROST       // Defrost: fan1+furn_cool_low+w1+comp1
};

// Cool levels
enum class CoolLevel : uint8_t {
    IDLE = 0,
    COOL,         // Normal: fan1+rev+furn_cool_low+comp1
    COOL_SUPP     // Supplemental: fan1+rev+furn_cool_low+furn_cool_high+comp1+comp2
};

// Output pin indices (order in array)
enum OutputIdx : uint8_t {
    OUT_FAN1 = 0,
    OUT_REV,
    OUT_FURN_COOL_LOW,
    OUT_FURN_COOL_HIGH,
    OUT_W1,
    OUT_W2,
    OUT_COMP1,
    OUT_COMP2,
    OUT_COUNT
};

// Input pin indices
enum InputIdx : uint8_t {
    IN_OUT_TEMP_OK = 0,
    IN_DEFROST_MODE,
    IN_COUNT
};

struct ThermostatConfig {
    // Temperature deadbands
    float heatDeadband = 0.5f;        // Degrees below setpoint to start heating
    float coolDeadband = 0.5f;        // Degrees above setpoint to start cooling
    float heatOverrun = 0.5f;         // Degrees above setpoint to stop heating
    float coolOverrun = 0.5f;         // Degrees below setpoint to stop cooling

    // Timing (milliseconds)
    uint32_t minOnTimeMs = 180000;    // 3 min minimum on time
    uint32_t minOffTimeMs = 180000;   // 3 min minimum off time
    uint32_t minIdleTimeMs = 60000;   // 1 min idle between mode changes
    uint32_t maxRunTimeMs = 1800000;  // 30 min max continuous run
    uint32_t escalationDelayMs = 600000; // 10 min before escalating heat stage

    // Fan idle duty cycle
    bool fanIdleEnabled = false;
    uint32_t fanIdleWaitMin = 15;     // Minutes between fan-only runs when idle
    uint32_t fanIdleRunMin = 5;       // Minutes of fan-only per cycle
};

class Thermostat {
public:
    Thermostat(Scheduler* ts);

    void begin();
    void update();  // Called every 1s by task scheduler

    // Temperature
    void setCurrentTemperature(float temp);
    float getCurrentTemperature() const { return _currentTemp; }
    bool hasValidTemperature() const { return _tempValid; }
    unsigned long lastTempUpdateMs() const { return _lastTempUpdate; }

    // Set points
    void setHeatSetpoint(float temp) { _heatSetpoint = temp; }
    void setCoolSetpoint(float temp) { _coolSetpoint = temp; }
    float getHeatSetpoint() const { return _heatSetpoint; }
    float getCoolSetpoint() const { return _coolSetpoint; }

    // Mode
    void setMode(ThermostatMode mode);
    ThermostatMode getMode() const { return _mode; }
    ThermostatAction getAction() const { return _action; }

    // Heat/cool levels
    HeatLevel getHeatLevel() const { return _heatLevel; }
    CoolLevel getCoolLevel() const { return _coolLevel; }

    // Force flags
    void setForceFurnace(bool force) { _forceFurnace = force; }
    bool isForceFurnace() const { return _forceFurnace; }
    void setForceNoHP(bool noHP) { _forceNoHP = noHP; }
    bool isForceNoHP() const { return _forceNoHP; }
    bool isDefrostActive() const { return _defrostActive; }

    // Config
    ThermostatConfig& config() { return _config; }
    const ThermostatConfig& config() const { return _config; }

    // Pin access
    void setOutputPins(OutPin* pins[OUT_COUNT]);
    void setInputPins(InputPin* pins[IN_COUNT]);
    OutPin* getOutput(OutputIdx idx) const { return _outputs[idx]; }
    InputPin* getInput(InputIdx idx) const { return _inputs[idx]; }

    // String helpers
    static const char* modeToString(ThermostatMode mode);
    static const char* actionToString(ThermostatAction action);
    static const char* heatLevelToString(HeatLevel level);
    static const char* coolLevelToString(CoolLevel level);
    static ThermostatMode stringToMode(const char* str);

private:
    void updateHeating();
    void updateCooling();
    void updateFanOnly();
    void updateFanIdle();

    void applyHeatLevel(HeatLevel level);
    void applyCoolLevel(CoolLevel level);
    void allRelaysOff();

    bool canTurnOn() const;
    bool canTurnOff() const;
    bool canEscalate() const;

    void enterDefrost();
    void exitDefrost();

    Scheduler* _ts;
    Task* _tUpdate;

    // Output and input pins
    OutPin* _outputs[OUT_COUNT] = {};
    InputPin* _inputs[IN_COUNT] = {};

    // State
    ThermostatMode _mode = ThermostatMode::OFF;
    ThermostatAction _action = ThermostatAction::OFF;
    HeatLevel _heatLevel = HeatLevel::IDLE;
    CoolLevel _coolLevel = CoolLevel::IDLE;

    // Temperature
    float _currentTemp = NAN;
    float _heatSetpoint = 68.0f;
    float _coolSetpoint = 76.0f;
    bool _tempValid = false;
    unsigned long _lastTempUpdate = 0;

    // Flags
    bool _forceFurnace = false;
    bool _forceNoHP = false;
    bool _defrostActive = false;
    HeatLevel _preDefrostLevel = HeatLevel::IDLE;

    // Timing
    unsigned long _lastActionChange = 0;
    unsigned long _lastEscalation = 0;
    unsigned long _actionStartTime = 0;

    // Fan idle state
    unsigned long _fanIdleLastRun = 0;
    bool _fanIdleRunning = false;

    ThermostatConfig _config;
};

#endif
