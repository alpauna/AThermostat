#include "MQTTHandler.h"
#include "HX710.h"
#include <ArduinoJson.h>

MQTTHandler::MQTTHandler(Scheduler* ts)
    : _ts(ts), _tReconnect(nullptr), _thermostat(nullptr),
      _pressure1(nullptr), _pressure2(nullptr) {}

void MQTTHandler::begin(const IPAddress& host, uint16_t port,
                         const String& user, const String& password) {
    _client.onConnect([this](bool sessionPresent) {
        this->onConnect(sessionPresent);
    });
    _client.onDisconnect([this](AsyncMqttClientDisconnectReason reason) {
        this->onDisconnect(reason);
    });
    _client.onSubscribe([this](uint16_t packetId, uint8_t qos) {
        this->onSubscribe(packetId, qos);
    });
    _client.onUnsubscribe([this](uint16_t packetId) {
        this->onUnsubscribe(packetId);
    });
    _client.onMessage([this](char* topic, char* payload,
                             AsyncMqttClientMessageProperties properties,
                             size_t len, size_t index, size_t total) {
        this->onMessage(topic, payload, properties, len, index, total);
    });
    _client.onPublish([this](uint16_t packetId) {
        this->onPublish(packetId);
    });
    _client.setServer(host, port);
    _client.setCredentials(user.c_str(), password.c_str());

    _tReconnect = new Task(10 * TASK_SECOND, TASK_FOREVER, [this]() {
        if (_client.connected()) {
            _tReconnect->disable();
            return;
        }
        Log.info("MQTT", "Connecting to MQTT...");
        _client.connect();
    }, _ts, false);
}

void MQTTHandler::startReconnect() {
    if (_tReconnect) {
        _tReconnect->enableDelayed();
    }
}

void MQTTHandler::stopReconnect() {
    if (_tReconnect) {
        _tReconnect->disable();
    }
}

void MQTTHandler::disconnect() {
    _client.disconnect();
}

void MQTTHandler::setThermostat(Thermostat* thermostat) {
    _thermostat = thermostat;
}

void MQTTHandler::setPressureSensors(HX710* sensor1, HX710* sensor2) {
    _pressure1 = sensor1;
    _pressure2 = sensor2;
}

void MQTTHandler::publishState() {
    if (!_client.connected() || _thermostat == nullptr) return;

    JsonDocument doc;
    doc["mode"] = Thermostat::modeToString(_thermostat->getMode());
    doc["action"] = Thermostat::actionToString(_thermostat->getAction());
    doc["heat_level"] = Thermostat::heatLevelToString(_thermostat->getHeatLevel());
    doc["cool_level"] = Thermostat::coolLevelToString(_thermostat->getCoolLevel());

    if (_thermostat->hasValidTemperature()) {
        doc["current_temp"] = serialized(String(_thermostat->getCurrentTemperature(), 1));
    }
    doc["heat_setpoint"] = serialized(String(_thermostat->getHeatSetpoint(), 1));
    doc["cool_setpoint"] = serialized(String(_thermostat->getCoolSetpoint(), 1));

    doc["force_furnace"] = _thermostat->isForceFurnace();
    doc["force_no_hp"] = _thermostat->isForceNoHP();
    doc["defrost"] = _thermostat->isDefrostActive();

    // I/O states
    JsonObject outputs = doc["outputs"].to<JsonObject>();
    static const char* outNames[] = {"fan1","rev","furn_cool_low","furn_cool_high","w1","w2","comp1","comp2"};
    for (int i = 0; i < OUT_COUNT; i++) {
        if (_thermostat->getOutput((OutputIdx)i)) {
            outputs[outNames[i]] = _thermostat->getOutput((OutputIdx)i)->isPinOn();
        }
    }

    JsonObject inputs = doc["inputs"].to<JsonObject>();
    static const char* inNames[] = {"out_temp_ok","defrost_mode"};
    for (int i = 0; i < IN_COUNT; i++) {
        if (_thermostat->getInput((InputIdx)i)) {
            inputs[inNames[i]] = _thermostat->getInput((InputIdx)i)->isActive();
        }
    }

    // Pressure sensors
    if (_pressure1 && _pressure1->isValid()) {
        doc["pressure1"] = serialized(String(_pressure1->getLastValue(), 2));
    }
    if (_pressure2 && _pressure2->isValid()) {
        doc["pressure2"] = serialized(String(_pressure2->getLastValue(), 2));
    }

    char buf[512];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    String topic = _topicPrefix + "/state";
    _client.publish(topic.c_str(), 0, false, buf, len);
}

void MQTTHandler::onConnect(bool sessionPresent) {
    Log.info("MQTT", "Connected to MQTT (session present: %s)", sessionPresent ? "yes" : "no");
    Log.info("MQTT", "IP: %s", WiFi.localIP().toString().c_str());
    if (_tReconnect) {
        _tReconnect->disable();
    }

    // Subscribe to HA temperature topic
    if (_tempTopic.length() > 0) {
        _client.subscribe(_tempTopic.c_str(), 0);
        Log.info("MQTT", "Subscribed to temp topic: %s", _tempTopic.c_str());
    }
}

void MQTTHandler::onDisconnect(AsyncMqttClientDisconnectReason reason) {
    Log.warn("MQTT", "Disconnected from MQTT (reason: %d)", (int)reason);

    if (reason == AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT) {
        Log.error("MQTT", "Bad server fingerprint");
    }

    if (WiFi.isConnected()) {
        startReconnect();
    }
}

void MQTTHandler::onSubscribe(uint16_t packetId, uint8_t qos) {
    Log.debug("MQTT", "Subscribe acknowledged (packetId=%u qos=%u)", packetId, qos);
}

void MQTTHandler::onUnsubscribe(uint16_t packetId) {
    Log.debug("MQTT", "Unsubscribe acknowledged (packetId=%u)", packetId);
}

void MQTTHandler::onMessage(char* topic, char* payload,
                             AsyncMqttClientMessageProperties properties,
                             size_t len, size_t index, size_t total) {
    // Check if this is the temperature topic
    if (_thermostat && _tempTopic.length() > 0 && strcmp(topic, _tempTopic.c_str()) == 0) {
        char buf[32];
        size_t copyLen = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';

        float temp = atof(buf);
        if (temp > -50.0f && temp < 150.0f) {
            _thermostat->setCurrentTemperature(temp);
            Log.debug("MQTT", "Temperature update: %.1f°F", temp);
        } else {
            Log.warn("MQTT", "Invalid temperature value: %s", buf);
        }
        return;
    }

    Log.debug("MQTT", "Message on topic: %s (len=%u)", topic, len);
}

void MQTTHandler::onPublish(uint16_t packetId) {
    // Publish acknowledged — no action needed
}
