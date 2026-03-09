#include "CANBus.h"
#include "Logger.h"

CANBus::CANBus(Scheduler* ts, gpio_num_t txPin, gpio_num_t rxPin)
    : _ts(ts), _txPin(txPin), _rxPin(rxPin) {}

bool CANBus::begin(twai_timing_config_t timing) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(_txPin, _rxPin, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 16;
    g_config.tx_queue_len = 8;

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &timing, &f_config);
    if (err != ESP_OK) {
        Log.error("CAN", "Driver install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = twai_start();
    if (err != ESP_OK) {
        Log.error("CAN", "Start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return false;
    }

    _running = true;

    // Poll for received messages every 10ms on main loop
    _tPoll = new Task(10, TASK_FOREVER, [this]() { poll(); }, _ts, true);

    // Send heartbeat every 5 seconds
    _tHeartbeat = new Task(5000, TASK_FOREVER, [this]() {
        sendHeartbeat(CANNodeId::THERMOSTAT);
    }, _ts, true);

    Log.info("CAN", "Bus started (TX=GPIO%d RX=GPIO%d)", _txPin, _rxPin);
    return true;
}

void CANBus::stop() {
    if (!_running) return;

    if (_tPoll) { _tPoll->disable(); delete _tPoll; _tPoll = nullptr; }
    if (_tHeartbeat) { _tHeartbeat->disable(); delete _tHeartbeat; _tHeartbeat = nullptr; }

    twai_stop();
    twai_driver_uninstall();
    _running = false;
    Log.info("CAN", "Bus stopped");
}

bool CANBus::send(uint32_t id, const uint8_t* data, uint8_t len) {
    if (!_running || len > 8) return false;

    twai_message_t msg = {};
    msg.identifier = id;
    msg.data_length_code = len;
    memcpy(msg.data, data, len);

    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(10));
    if (err == ESP_OK) {
        _txCount++;
        return true;
    }

    _errCount++;
    if (err != ESP_ERR_TIMEOUT) {
        Log.warn("CAN", "TX failed (0x%03X): %s", id, esp_err_to_name(err));
    }
    return false;
}

bool CANBus::sendThermoState(uint8_t mode, int16_t heatSP, int16_t coolSP,
                             uint8_t flags) {
    // Byte layout:
    //  [0]   mode (0=off,1=heat,2=cool,3=auto,4=fan)
    //  [1-2] heat setpoint (x10, e.g. 685 = 68.5F)
    //  [3-4] cool setpoint (x10)
    //  [5]   flags: bit0=forceFurnace, bit1=forceNoHP, bit2=defrost
    uint8_t data[6];
    data[0] = mode;
    data[1] = (heatSP >> 8) & 0xFF;
    data[2] = heatSP & 0xFF;
    data[3] = (coolSP >> 8) & 0xFF;
    data[4] = coolSP & 0xFF;
    data[5] = flags;
    return send(CAN_ID_THERMO_STATE, data, 6);
}

bool CANBus::sendSensors(int16_t indoorTemp, int16_t pressure1, int16_t pressure2) {
    // Byte layout:
    //  [0-1] indoor temp (x10, e.g. 725 = 72.5F)
    //  [2-3] pressure1 (x100, e.g. 125 = 1.25 inWC)
    //  [4-5] pressure2 (x100)
    uint8_t data[6];
    data[0] = (indoorTemp >> 8) & 0xFF;
    data[1] = indoorTemp & 0xFF;
    data[2] = (pressure1 >> 8) & 0xFF;
    data[3] = pressure1 & 0xFF;
    data[4] = (pressure2 >> 8) & 0xFF;
    data[5] = pressure2 & 0xFF;
    return send(CAN_ID_THERMO_SENSORS, data, 6);
}

bool CANBus::sendHeartbeat(CANNodeId node) {
    // Byte layout:
    //  [0]   node ID
    //  [1-4] uptime seconds (uint32, big-endian)
    uint32_t uptime = millis() / 1000;
    uint8_t data[5];
    data[0] = static_cast<uint8_t>(node);
    data[1] = (uptime >> 24) & 0xFF;
    data[2] = (uptime >> 16) & 0xFF;
    data[3] = (uptime >> 8) & 0xFF;
    data[4] = uptime & 0xFF;
    return send(CAN_ID_HEARTBEAT, data, 5);
}

void CANBus::poll() {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        _rxCount++;
        if (_rxCallback) {
            _rxCallback(msg.identifier, msg.data, msg.data_length_code);
        }
    }

    // Check for bus-off and recover
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) {
        if (status.state == TWAI_STATE_BUS_OFF) {
            recoverBus();
        }
    }
}

void CANBus::recoverBus() {
    Log.warn("CAN", "Bus-off detected, initiating recovery");
    _errCount++;
    esp_err_t err = twai_initiate_recovery();
    if (err != ESP_OK) {
        Log.error("CAN", "Recovery failed: %s", esp_err_to_name(err));
    }
}

twai_status_info_t CANBus::getStatus() {
    twai_status_info_t status = {};
    twai_get_status_info(&status);
    return status;
}
