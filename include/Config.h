#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <map>
#include <LittleFS.h>
#include "ArduinoJson.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"

struct ProjectInfo {
    String name;
    String createdOnDate;
    String description;
    String encrypt;
    bool encrypted;
    uint32_t maxLogSize;
    uint8_t maxOldLogCount;
    String timezone;

    // Thermostat set points and mode
    float heatSetpoint;
    float coolSetpoint;
    uint8_t thermostatMode;   // ThermostatMode enum value
    bool forceFurnace;
    bool forceNoHP;

    // Thermostat timing (ms)
    uint32_t minOnTimeMs;
    uint32_t minOffTimeMs;
    uint32_t minIdleTimeMs;
    uint32_t maxRunTimeMs;
    uint32_t escalationDelayMs;

    // Temperature deadbands
    float heatDeadband;
    float coolDeadband;
    float heatOverrun;
    float coolOverrun;

    // Fan idle duty cycle
    bool fanIdleEnabled;
    uint32_t fanIdleWaitMin;
    uint32_t fanIdleRunMin;

    // HX710 calibration
    int32_t hx710_1_raw1, hx710_1_raw2;
    float hx710_1_val1, hx710_1_val2;
    int32_t hx710_2_raw1, hx710_2_raw2;
    float hx710_2_val1, hx710_2_val2;

    // WiFi / networking
    uint32_t apFallbackSeconds;
    String apPassword;
    String ftpPassword;  // empty = default "admin"

    // UI
    String theme;
    uint8_t pollIntervalSec;

    // System identity
    String systemName;
    String mqttPrefix;
    String mqttTempTopic;  // HA temperature subscription topic
    uint32_t sessionTimeoutMinutes;

    // Safe mode
    bool forceSafeMode;
};

class Config {
  public:
    Config();

    // LittleFS operations
    bool initFS();
    bool openConfigFile(const char* filename, ProjectInfo& proj);
    bool loadConfig(const char* filename, ProjectInfo& proj);
    bool saveConfiguration(const char* filename, ProjectInfo& proj);
    bool updateConfig(const char* filename, ProjectInfo& proj);
    bool updateThermostatState(const char* filename, ProjectInfo& proj);
    bool formatFS(ProjectInfo& proj);
    String getFSInfo() const;

    // Getters for loaded config values
    String getWifiSSID() const { return _wifiSSID; }
    String getWifiPassword() const { return _wifiPassword; }
    IPAddress getMqttHost() const { return _mqttHost; }
    uint16_t getMqttPort() const { return _mqttPort; }
    String getMqttUser() const { return _mqttUser; }
    String getMqttPassword() const { return _mqttPassword; }

    // Setters for config values
    void setWifiSSID(const String& ssid) { _wifiSSID = ssid; }
    void setWifiPassword(const String& password) { _wifiPassword = password; }
    void setMqttHost(const IPAddress& host) { _mqttHost = host; }
    void setMqttPort(uint16_t port) { _mqttPort = port; }
    void setMqttUser(const String& user) { _mqttUser = user; }
    void setMqttPassword(const String& password) { _mqttPassword = password; }

    // Admin password
    bool hasAdminPassword() const { return _adminPasswordHash.length() > 0; }
    void setAdminPassword(const String& plaintext);
    bool verifyAdminPassword(const String& plaintext) const;

    // Certificate loading for HTTPS
    bool loadCertificates(const char* certFile, const char* keyFile);
    bool generateSelfSignedCert();
    bool isCertExpired() const;
    bool hasCertificates() const { return _certBuf != nullptr && _keyBuf != nullptr; }
    const uint8_t* getCert() const { return _certBuf; }
    size_t getCertLen() const { return _certLen; }
    const uint8_t* getKey() const { return _keyBuf; }
    size_t getKeyLen() const { return _keyLen; }

    // FS state
    bool isFSInitialized() const { return _fsInitialized; }

    // ProjectInfo access
    void setProjectInfo(ProjectInfo* proj) { _proj = proj; }
    ProjectInfo* getProjectInfo() { return _proj; }

    // Password encryption
    bool initEncryption();
    static bool isEncryptionReady() { return _encryptionReady; }
    static void setObfuscationKey(const String& key);
    static String encryptPassword(const String& plaintext);
    static String decryptPassword(const String& encrypted);
    static String generateRandomPassword(uint8_t length = 8);

  private:
    fs::File _configFile;
    bool _fsInitialized;

    String _wifiSSID;
    String _wifiPassword;
    IPAddress _mqttHost;
    uint16_t _mqttPort;
    String _mqttUser;
    String _mqttPassword;
    String _adminPasswordHash;

    ProjectInfo* _proj;

    // AES-256-GCM encryption
    static uint8_t _aesKey[32];
    static bool _encryptionReady;
    static String _obfuscationKey;

    // HTTPS certificate buffers
    uint8_t* _certBuf = nullptr;
    size_t _certLen = 0;
    uint8_t* _keyBuf = nullptr;
    size_t _keyLen = 0;
};

#endif
