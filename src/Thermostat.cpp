#include "Thermostat.h"
#include "Logger.h"

Thermostat::Thermostat(Scheduler* ts)
    : _ts(ts)
    , _tUpdate(nullptr)
{
}

void Thermostat::begin() {
    _tUpdate = new Task(1 * TASK_SECOND, TASK_FOREVER, [this]() {
        this->update();
    }, _ts, true);
    _lastActionChange = millis();
    _actionStartTime = millis();
    _fanIdleLastRun = millis();
    Log.info("Thermo", "Thermostat initialized, mode=%s", modeToString(_mode));
}

void Thermostat::setOutputPins(OutPin* pins[OUT_COUNT]) {
    for (int i = 0; i < OUT_COUNT; i++) {
        _outputs[i] = pins[i];
    }
}

void Thermostat::setInputPins(InputPin* pins[IN_COUNT]) {
    for (int i = 0; i < IN_COUNT; i++) {
        _inputs[i] = pins[i];
    }
}

void Thermostat::setCurrentTemperature(float temp) {
    _currentTemp = temp;
    _tempValid = true;
    _lastTempUpdate = millis();
}

void Thermostat::setMode(ThermostatMode mode) {
    if (mode == _mode) return;
    Log.info("Thermo", "Mode change: %s -> %s", modeToString(_mode), modeToString(mode));
    _mode = mode;

    if (mode == ThermostatMode::OFF) {
        allRelaysOff();
        _action = ThermostatAction::OFF;
        _heatLevel = HeatLevel::IDLE;
        _coolLevel = CoolLevel::IDLE;
        _defrostActive = false;
        _fanIdleRunning = false;
    } else {
        _action = ThermostatAction::IDLE;
        _lastActionChange = millis();
        _actionStartTime = millis();
        _fanIdleLastRun = millis();
    }
}

// --- Main update (called every 1s) ---

void Thermostat::update() {
    if (_mode == ThermostatMode::OFF) return;

    // Temperature validity check — stale after 5 minutes
    if (_tempValid && (millis() - _lastTempUpdate > 300000)) {
        Log.warn("Thermo", "Temperature stale (>5min), marking invalid");
        _tempValid = false;
    }

    // Handle defrost input
    if (_inputs[IN_DEFROST_MODE] && _inputs[IN_DEFROST_MODE]->isActive()) {
        if (!_defrostActive && _action == ThermostatAction::HEATING) {
            enterDefrost();
        }
    } else {
        if (_defrostActive) {
            exitDefrost();
        }
    }

    // Max run time enforcement
    if (_action == ThermostatAction::HEATING || _action == ThermostatAction::COOLING) {
        if (millis() - _actionStartTime > _config.maxRunTimeMs) {
            Log.warn("Thermo", "Max run time exceeded (%lus), forcing idle",
                     _config.maxRunTimeMs / 1000);
            allRelaysOff();
            _action = ThermostatAction::IDLE;
            _heatLevel = HeatLevel::IDLE;
            _coolLevel = CoolLevel::IDLE;
            _lastActionChange = millis();
            return;
        }
    }

    if (!_tempValid) return;  // Can't make decisions without temperature

    switch (_mode) {
        case ThermostatMode::HEAT:
            updateHeating();
            break;
        case ThermostatMode::COOL:
            updateCooling();
            break;
        case ThermostatMode::HEAT_COOL:
            // Auto mode: heat if below heat setpoint, cool if above cool setpoint
            if (_action == ThermostatAction::HEATING) {
                updateHeating();
            } else if (_action == ThermostatAction::COOLING) {
                updateCooling();
            } else {
                // IDLE — decide which direction
                if (_currentTemp < _heatSetpoint - _config.heatDeadband) {
                    updateHeating();
                } else if (_currentTemp > _coolSetpoint + _config.coolDeadband) {
                    updateCooling();
                } else {
                    updateFanIdle();
                }
            }
            break;
        case ThermostatMode::FAN_ONLY:
            updateFanOnly();
            break;
        default:
            break;
    }
}

// --- Heating logic ---

void Thermostat::updateHeating() {
    if (_defrostActive) return;  // Don't change state during defrost

    if (_action == ThermostatAction::IDLE || _action == ThermostatAction::FAN_RUNNING) {
        // Check if we need to start heating
        if (_currentTemp < _heatSetpoint - _config.heatDeadband) {
            if (!canTurnOn()) return;

            Log.info("Thermo", "Starting HEAT (temp=%.1f setpoint=%.1f)", _currentTemp, _heatSetpoint);
            _action = ThermostatAction::HEATING;
            _lastActionChange = millis();
            _actionStartTime = millis();
            _lastEscalation = millis();
            _fanIdleRunning = false;

            // Determine initial heat level
            bool outTempOk = _inputs[IN_OUT_TEMP_OK] && _inputs[IN_OUT_TEMP_OK]->isActive();
            if ((outTempOk && !_forceFurnace && !_forceNoHP)) {
                applyHeatLevel(HeatLevel::HP_LOW);
            } else {
                applyHeatLevel(HeatLevel::FURNACE_LOW);
            }
        } else {
            updateFanIdle();
        }
    } else if (_action == ThermostatAction::HEATING) {
        // Check if we should stop heating
        if (_currentTemp >= _heatSetpoint + _config.heatOverrun) {
            if (!canTurnOff()) return;

            Log.info("Thermo", "Stopping HEAT (temp=%.1f setpoint=%.1f)", _currentTemp, _heatSetpoint);
            allRelaysOff();
            _action = ThermostatAction::IDLE;
            _heatLevel = HeatLevel::IDLE;
            _lastActionChange = millis();
            _fanIdleLastRun = millis();
            return;
        }

        // Escalation logic
        if (canEscalate()) {
            bool outTempOk = _inputs[IN_OUT_TEMP_OK] && _inputs[IN_OUT_TEMP_OK]->isActive();

            if (_heatLevel == HeatLevel::HP_LOW && !_forceFurnace) {
                Log.info("Thermo", "Escalating HP_LOW -> HP_HIGH");
                applyHeatLevel(HeatLevel::HP_HIGH);
                _lastEscalation = millis();
            } else if (_heatLevel == HeatLevel::HP_HIGH ||
                       (_heatLevel == HeatLevel::HP_LOW && _forceFurnace)) {
                // HP exhausted or force furnace — switch to furnace
                Log.info("Thermo", "Escalating to FURNACE_LOW (outTempOk=%d forceFurnace=%d)",
                         outTempOk, _forceFurnace);
                applyHeatLevel(HeatLevel::FURNACE_LOW);
                _lastEscalation = millis();
            } else if (_heatLevel == HeatLevel::FURNACE_LOW) {
                Log.info("Thermo", "Escalating FURNACE_LOW -> FURNACE_HIGH");
                applyHeatLevel(HeatLevel::FURNACE_HIGH);
                _lastEscalation = millis();
            }
            // FURNACE_HIGH is max — no further escalation
        }

        // Check if out_temp_ok changed and we should switch HP <-> Furnace
        bool outTempOk = _inputs[IN_OUT_TEMP_OK] && _inputs[IN_OUT_TEMP_OK]->isActive();
        if (!outTempOk && !_forceFurnace && !_forceNoHP &&
            (_heatLevel == HeatLevel::HP_LOW || _heatLevel == HeatLevel::HP_HIGH)) {
            // Outdoor temp dropped — switch to furnace
            Log.info("Thermo", "Out temp not OK, switching HP -> Furnace");
            applyHeatLevel(HeatLevel::FURNACE_LOW);
            _lastEscalation = millis();
        }
    }
}

// --- Cooling logic ---

void Thermostat::updateCooling() {
    if (_action == ThermostatAction::IDLE || _action == ThermostatAction::FAN_RUNNING) {
        if (_currentTemp > _coolSetpoint + _config.coolDeadband) {
            if (!canTurnOn()) return;

            Log.info("Thermo", "Starting COOL (temp=%.1f setpoint=%.1f)", _currentTemp, _coolSetpoint);
            _action = ThermostatAction::COOLING;
            _lastActionChange = millis();
            _actionStartTime = millis();
            _lastEscalation = millis();
            _fanIdleRunning = false;
            applyCoolLevel(CoolLevel::COOL);
        } else {
            updateFanIdle();
        }
    } else if (_action == ThermostatAction::COOLING) {
        if (_currentTemp <= _coolSetpoint - _config.coolOverrun) {
            if (!canTurnOff()) return;

            Log.info("Thermo", "Stopping COOL (temp=%.1f setpoint=%.1f)", _currentTemp, _coolSetpoint);
            allRelaysOff();
            _action = ThermostatAction::IDLE;
            _coolLevel = CoolLevel::IDLE;
            _lastActionChange = millis();
            _fanIdleLastRun = millis();
            return;
        }

        // Escalation: supplemental cooling
        if (_coolLevel == CoolLevel::COOL && canEscalate()) {
            Log.info("Thermo", "Escalating to supplemental cooling");
            applyCoolLevel(CoolLevel::COOL_SUPP);
            _lastEscalation = millis();
        }
    }
}

// --- Fan only mode ---

void Thermostat::updateFanOnly() {
    if (_action != ThermostatAction::FAN_RUNNING) {
        _action = ThermostatAction::FAN_RUNNING;
        allRelaysOff();
        _outputs[OUT_FAN1]->turnOn();
        _lastActionChange = millis();
    }
}

// --- Fan idle duty cycle ---

void Thermostat::updateFanIdle() {
    if (!_config.fanIdleEnabled) return;
    if (_action != ThermostatAction::IDLE && _action != ThermostatAction::FAN_RUNNING) return;

    unsigned long now = millis();

    if (_fanIdleRunning) {
        // Check if fan-only run period is over
        if (now - _fanIdleLastRun > _config.fanIdleRunMin * 60000UL) {
            Log.debug("Thermo", "Fan idle cycle complete");
            _outputs[OUT_FAN1]->turnOff();
            _action = ThermostatAction::IDLE;
            _fanIdleRunning = false;
            _fanIdleLastRun = now;
        }
    } else {
        // Check if wait period is over
        if (now - _fanIdleLastRun > _config.fanIdleWaitMin * 60000UL) {
            Log.debug("Thermo", "Starting fan idle cycle");
            _outputs[OUT_FAN1]->turnOn();
            _action = ThermostatAction::FAN_RUNNING;
            _fanIdleRunning = true;
            _fanIdleLastRun = now;
        }
    }
}

// --- Relay mapping ---

void Thermostat::applyHeatLevel(HeatLevel level) {
    _heatLevel = level;
    _coolLevel = CoolLevel::IDLE;
    allRelaysOff();

    switch (level) {
        case HeatLevel::HP_LOW:
            // fan1 + furn_cool_low + comp1
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_FURN_COOL_LOW]->turnOn();
            _outputs[OUT_COMP1]->turnOn();
            break;
        case HeatLevel::HP_HIGH:
            // fan1 + furn_cool_low + comp1 + comp2
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_FURN_COOL_LOW]->turnOn();
            _outputs[OUT_COMP1]->turnOn();
            _outputs[OUT_COMP2]->turnOn();
            break;
        case HeatLevel::FURNACE_LOW:
            // fan1 + w1
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_W1]->turnOn();
            break;
        case HeatLevel::FURNACE_HIGH:
            // fan1 + w1 + w2
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_W1]->turnOn();
            _outputs[OUT_W2]->turnOn();
            break;
        case HeatLevel::DEFROST:
            // fan1 + furn_cool_low + w1 + comp1
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_FURN_COOL_LOW]->turnOn();
            _outputs[OUT_W1]->turnOn();
            _outputs[OUT_COMP1]->turnOn();
            break;
        case HeatLevel::IDLE:
        default:
            break;
    }

    Log.info("Thermo", "Heat level: %s", heatLevelToString(level));
}

void Thermostat::applyCoolLevel(CoolLevel level) {
    _coolLevel = level;
    _heatLevel = HeatLevel::IDLE;
    allRelaysOff();

    switch (level) {
        case CoolLevel::COOL:
            // fan1 + rev + furn_cool_low + comp1
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_REV]->turnOn();
            _outputs[OUT_FURN_COOL_LOW]->turnOn();
            _outputs[OUT_COMP1]->turnOn();
            break;
        case CoolLevel::COOL_SUPP:
            // fan1 + rev + furn_cool_low + furn_cool_high + comp1 + comp2
            _outputs[OUT_FAN1]->turnOn();
            _outputs[OUT_REV]->turnOn();
            _outputs[OUT_FURN_COOL_LOW]->turnOn();
            _outputs[OUT_FURN_COOL_HIGH]->turnOn();
            _outputs[OUT_COMP1]->turnOn();
            _outputs[OUT_COMP2]->turnOn();
            break;
        case CoolLevel::IDLE:
        default:
            break;
    }

    Log.info("Thermo", "Cool level: %s", coolLevelToString(level));
}

void Thermostat::allRelaysOff() {
    for (int i = 0; i < OUT_COUNT; i++) {
        if (_outputs[i]) {
            _outputs[i]->turnOff();
        }
    }
}

// --- Timing guards ---

bool Thermostat::canTurnOn() const {
    return (millis() - _lastActionChange) >= _config.minOffTimeMs;
}

bool Thermostat::canTurnOff() const {
    return (millis() - _actionStartTime) >= _config.minOnTimeMs;
}

bool Thermostat::canEscalate() const {
    return (millis() - _lastEscalation) >= _config.escalationDelayMs;
}

// --- Defrost handling ---

void Thermostat::enterDefrost() {
    if (_defrostActive) return;
    _defrostActive = true;
    _preDefrostLevel = _heatLevel;
    Log.info("Thermo", "Entering DEFROST (was %s)", heatLevelToString(_preDefrostLevel));
    applyHeatLevel(HeatLevel::DEFROST);
}

void Thermostat::exitDefrost() {
    if (!_defrostActive) return;
    _defrostActive = false;
    Log.info("Thermo", "Exiting DEFROST, restoring %s", heatLevelToString(_preDefrostLevel));

    // Restore pre-defrost heat level
    if (_action == ThermostatAction::HEATING && _preDefrostLevel != HeatLevel::IDLE) {
        applyHeatLevel(_preDefrostLevel);
    } else {
        allRelaysOff();
        _heatLevel = HeatLevel::IDLE;
        _action = ThermostatAction::IDLE;
        _lastActionChange = millis();
    }
}

// --- String helpers ---

const char* Thermostat::modeToString(ThermostatMode mode) {
    switch (mode) {
        case ThermostatMode::OFF:       return "off";
        case ThermostatMode::HEAT:      return "heat";
        case ThermostatMode::COOL:      return "cool";
        case ThermostatMode::HEAT_COOL: return "heat_cool";
        case ThermostatMode::FAN_ONLY:  return "fan_only";
        default:                        return "unknown";
    }
}

const char* Thermostat::actionToString(ThermostatAction action) {
    switch (action) {
        case ThermostatAction::OFF:         return "off";
        case ThermostatAction::IDLE:        return "idle";
        case ThermostatAction::HEATING:     return "heating";
        case ThermostatAction::COOLING:     return "cooling";
        case ThermostatAction::FAN_RUNNING: return "fan";
        default:                            return "unknown";
    }
}

const char* Thermostat::heatLevelToString(HeatLevel level) {
    switch (level) {
        case HeatLevel::IDLE:         return "idle";
        case HeatLevel::HP_LOW:       return "hp_low";
        case HeatLevel::HP_HIGH:      return "hp_high";
        case HeatLevel::FURNACE_LOW:  return "furnace_low";
        case HeatLevel::FURNACE_HIGH: return "furnace_high";
        case HeatLevel::DEFROST:      return "defrost";
        default:                      return "unknown";
    }
}

const char* Thermostat::coolLevelToString(CoolLevel level) {
    switch (level) {
        case CoolLevel::IDLE:      return "idle";
        case CoolLevel::COOL:      return "cool";
        case CoolLevel::COOL_SUPP: return "cool_supp";
        default:                   return "unknown";
    }
}

ThermostatMode Thermostat::stringToMode(const char* str) {
    if (strcmp(str, "heat") == 0)      return ThermostatMode::HEAT;
    if (strcmp(str, "cool") == 0)      return ThermostatMode::COOL;
    if (strcmp(str, "heat_cool") == 0) return ThermostatMode::HEAT_COOL;
    if (strcmp(str, "fan_only") == 0)  return ThermostatMode::FAN_ONLY;
    return ThermostatMode::OFF;
}
