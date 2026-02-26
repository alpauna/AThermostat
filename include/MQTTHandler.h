#ifndef MQTTHANDLER_H
#define MQTTHANDLER_H

#include <Arduino.h>
#include <AsyncMqttClient.h>
#include <WiFi.h>
#include <TaskSchedulerDeclarations.h>
#include "Logger.h"
#include "Thermostat.h"

class HX710;

class MQTTHandler {
  public:
    MQTTHandler(Scheduler* ts);
    void begin(const IPAddress& host, uint16_t port,
               const String& user, const String& password);
    AsyncMqttClient* getClient() { return &_client; }
    bool connected() const { return _client.connected(); }
    void setThermostat(Thermostat* thermostat);
    void setPressureSensors(HX710* sensor1, HX710* sensor2);
    void publishState();
    void setTopicPrefix(const String& prefix) { _topicPrefix = prefix; }
    void setTempTopic(const String& topic) { _tempTopic = topic; }
    void startReconnect();
    void stopReconnect();
    void disconnect();

  private:
    AsyncMqttClient _client;
    Scheduler* _ts;
    Task* _tReconnect;
    Thermostat* _thermostat;
    HX710* _pressure1;
    HX710* _pressure2;
    String _topicPrefix = "thermostat";
    String _tempTopic = "homeassistant/sensor/average_home_temperature/state";
    String _user;
    String _password;

    void onConnect(bool sessionPresent);
    void onDisconnect(AsyncMqttClientDisconnectReason reason);
    void onSubscribe(uint16_t packetId, uint8_t qos);
    void onUnsubscribe(uint16_t packetId);
    void onMessage(char* topic, char* payload,
                   AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total);
    void onPublish(uint16_t packetId);
};

#endif
