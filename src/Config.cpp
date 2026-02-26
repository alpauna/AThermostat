#include "Config.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/bignum.h"

uint8_t Config::_aesKey[32] = {0};
bool Config::_encryptionReady = false;
String Config::_obfuscationKey = "";

void Config::setObfuscationKey(const String& key) {
    _obfuscationKey = key;
}

Config::Config()
    : _fsInitialized(false)
    , _mqttHost(192, 168, 0, 46)
    , _mqttPort(1883)
    , _mqttUser("debian")
    , _mqttPassword("")
    , _wifiSSID("")
    , _wifiPassword("")
    , _adminPasswordHash("")
    , _proj(nullptr)
{
}

bool Config::initEncryption() {
    static const uint8_t salt[] = "AThermostat-Config-Encrypt-v1";
    esp_err_t err = esp_hmac_calculate(HMAC_KEY0, salt, sizeof(salt) - 1, _aesKey);
    _encryptionReady = (err == ESP_OK);
    return _encryptionReady;
}

String Config::encryptPassword(const String& plaintext) {
    if (plaintext.length() == 0) return plaintext;

    if (!_encryptionReady) {
        if (_obfuscationKey.length() == 0) return plaintext;
        size_t len = plaintext.length();
        uint8_t* xored = new uint8_t[len];
        for (size_t i = 0; i < len; i++) {
            xored[i] = plaintext[i] ^ _obfuscationKey[i % _obfuscationKey.length()];
        }
        size_t outLen = 0;
        mbedtls_base64_encode(nullptr, 0, &outLen, xored, len);
        uint8_t* b64 = new uint8_t[outLen + 1];
        mbedtls_base64_encode(b64, outLen + 1, &outLen, xored, len);
        b64[outLen] = '\0';
        String result = "$ENC$" + String((char*)b64);
        delete[] xored;
        delete[] b64;
        return result;
    }

    uint8_t iv[12];
    esp_fill_random(iv, sizeof(iv));

    size_t ptLen = plaintext.length();
    uint8_t* ciphertext = new uint8_t[ptLen];
    uint8_t tag[16];

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _aesKey, 256);
    mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, ptLen,
        iv, sizeof(iv), nullptr, 0,
        (const uint8_t*)plaintext.c_str(), ciphertext, sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);

    size_t packedLen = 12 + ptLen + 16;
    uint8_t* packed = new uint8_t[packedLen];
    memcpy(packed, iv, 12);
    memcpy(packed + 12, ciphertext, ptLen);
    memcpy(packed + 12 + ptLen, tag, 16);
    delete[] ciphertext;

    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, packed, packedLen);
    uint8_t* b64 = new uint8_t[b64Len + 1];
    mbedtls_base64_encode(b64, b64Len + 1, &b64Len, packed, packedLen);
    b64[b64Len] = '\0';
    delete[] packed;

    String result = "$AES$" + String((char*)b64);
    delete[] b64;
    return result;
}

String Config::decryptPassword(const String& encrypted) {
    if (encrypted.startsWith("$AES$")) {
        if (!_encryptionReady) return "";

        String b64Part = encrypted.substring(5);
        size_t decodedLen = 0;
        mbedtls_base64_decode(nullptr, 0, &decodedLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());

        if (decodedLen < 12 + 16) return "";

        uint8_t* decoded = new uint8_t[decodedLen];
        mbedtls_base64_decode(decoded, decodedLen, &decodedLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());

        uint8_t* iv = decoded;
        size_t ctLen = decodedLen - 12 - 16;
        uint8_t* ciphertext = decoded + 12;
        uint8_t* tag = decoded + 12 + ctLen;

        uint8_t* plaintext = new uint8_t[ctLen + 1];

        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _aesKey, 256);
        int ret = mbedtls_gcm_auth_decrypt(&gcm, ctLen,
            iv, 12, nullptr, 0, tag, 16,
            ciphertext, plaintext);
        mbedtls_gcm_free(&gcm);
        delete[] decoded;

        if (ret != 0) {
            delete[] plaintext;
            return "";
        }

        plaintext[ctLen] = '\0';
        String result = String((char*)plaintext);
        delete[] plaintext;
        return result;
    }

    if (encrypted.startsWith("$ENC$")) {
        if (_obfuscationKey.length() == 0) return encrypted;
        String b64Part = encrypted.substring(5);
        size_t outLen = 0;
        mbedtls_base64_decode(nullptr, 0, &outLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());
        uint8_t* decoded = new uint8_t[outLen + 1];
        mbedtls_base64_decode(decoded, outLen + 1, &outLen,
            (const uint8_t*)b64Part.c_str(), b64Part.length());
        for (size_t i = 0; i < outLen; i++) {
            decoded[i] ^= _obfuscationKey[i % _obfuscationKey.length()];
        }
        decoded[outLen] = '\0';
        String result = String((char*)decoded);
        delete[] decoded;
        return result;
    }

    return encrypted;
}

String Config::generateRandomPassword(uint8_t length) {
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    uint8_t buf[32];
    if (length > sizeof(buf)) length = sizeof(buf);
    esp_fill_random(buf, length);
    String result;
    result.reserve(length);
    for (uint8_t i = 0; i < length; i++) {
        result += charset[buf[i] % (sizeof(charset) - 1)];
    }
    return result;
}

void Config::setAdminPassword(const String& plaintext) {
    _adminPasswordHash = plaintext;
}

bool Config::verifyAdminPassword(const String& plaintext) const {
    return plaintext == _adminPasswordHash;
}

bool Config::initFS() {
    if (!LittleFS.begin(true)) {
        Serial.println("\nLittleFS mount failed.");
        return false;
    }
    Serial.println("\nLittleFS mounted successfully.");
    _fsInitialized = true;
    return true;
}

bool Config::formatFS(ProjectInfo& proj) {
    if (!_fsInitialized) return false;

    Serial.println("FORMAT: Formatting LittleFS...");
    LittleFS.format();

    if (!LittleFS.begin(true)) {
        Serial.println("FORMAT: Re-mount failed.");
        return false;
    }

    // Restore certificates if loaded
    if (_certBuf && _certLen > 0 && _keyBuf && _keyLen > 0) {
        fs::File cf = LittleFS.open("/cert.pem", FILE_WRITE);
        if (cf) { cf.write(_certBuf, _certLen); cf.close(); }
        fs::File kf = LittleFS.open("/key.pem", FILE_WRITE);
        if (kf) { kf.write(_keyBuf, _keyLen); kf.close(); }
        Serial.println("FORMAT: Restored certificates.");
    }

    Serial.println("FORMAT: Writing config (preserving credentials)...");
    bool ok = saveConfiguration("/config.txt", proj);
    Serial.println(ok ? "FORMAT: Complete." : "FORMAT: Failed to write config.");
    return ok;
}

String Config::getFSInfo() const {
    if (!_fsInitialized) {
        return "{\"error\":\"LittleFS not initialized\"}";
    }
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;

    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"LittleFS\",\"totalKB\":%u,\"usedKB\":%u,\"freeKB\":%u}",
        (unsigned)(totalBytes / 1024), (unsigned)(usedBytes / 1024), (unsigned)(freeBytes / 1024));
    return String(buf);
}

bool Config::openConfigFile(const char* filename, ProjectInfo& proj) {
    if (!LittleFS.exists(filename)) {
        return saveConfiguration(filename, proj);
    }
    _configFile = LittleFS.open(filename, FILE_READ);
    if (!_configFile || _configFile.size() == 0) {
        _configFile.close();
        return saveConfiguration(filename, proj);
    }
    _configFile.close();

    _configFile = LittleFS.open(filename, FILE_READ);
    return (bool)_configFile;
}

bool Config::loadConfig(const char* filename, ProjectInfo& proj) {
    if (!_configFile) {
        return false;
    }
    _configFile.seek(0);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, _configFile);

    if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return false;
    }

    const char* project = doc["project"];
    const char* created = doc["created"];
    const char* description = doc["description"];
    proj.name = project != nullptr ? project : "AThermostat";
    proj.createdOnDate = created != nullptr ? created : "";
    proj.description = description != nullptr ? description : "";

    // WiFi
    JsonObject wifiObj = doc["wifi"];
    const char* wifi_ssid = wifiObj["ssid"];
    const char* wifi_password = wifiObj["password"];
    _wifiSSID = wifi_ssid != nullptr ? wifi_ssid : "";
    _wifiPassword = wifi_password != nullptr ? decryptPassword(wifi_password != nullptr ? wifi_password : "") : "";
    proj.apFallbackSeconds = wifiObj["apFallbackSeconds"] | 600;
    const char* apPw = wifiObj["apPassword"];
    proj.apPassword = (apPw != nullptr && strlen(apPw) > 0) ? decryptPassword(String(apPw)) : "";
    const char* ftpPw = wifiObj["ftpPassword"];
    proj.ftpPassword = (ftpPw != nullptr && strlen(ftpPw) > 0) ? decryptPassword(String(ftpPw)) : "";
    Serial.printf("Read WiFi SSID:%s apFallback:%us\n", wifi_ssid ? wifi_ssid : "", proj.apFallbackSeconds);

    // MQTT
    JsonObject mqtt = doc["mqtt"];
    const char* mqtt_user = mqtt["user"];
    const char* mqtt_password = mqtt["password"];
    const char* mqtt_host = mqtt["host"];
    int mqtt_port = mqtt["port"];
    _mqttPort = mqtt_port;
    _mqttUser = mqtt_user != nullptr ? mqtt_user : "";
    _mqttPassword = mqtt_password != nullptr ? decryptPassword(mqtt_password != nullptr ? mqtt_password : "") : "";
    _mqttHost.fromString(mqtt_host != nullptr ? mqtt_host : "192.168.1.2");
    const char* tempTopic = mqtt["tempTopic"];
    proj.mqttTempTopic = (tempTopic != nullptr) ? String(tempTopic) : "homeassistant/sensor/average_home_temperature/state";
    Serial.printf("Read MQTT Host:%s tempTopic:%s\n", _mqttHost.toString().c_str(), proj.mqttTempTopic.c_str());

    // Logging
    JsonObject logging = doc["logging"];
    proj.maxLogSize = logging["maxLogSize"] | (512 * 1024);
    proj.maxOldLogCount = logging["maxOldLogCount"] | 3;

    // Timezone
    JsonObject timezone = doc["timezone"];
    if (timezone["posix"].is<const char*>()) {
        proj.timezone = timezone["posix"].as<String>();
    } else {
        proj.timezone = "CST6CDT,M3.2.0,M11.1.0";
    }

    // Thermostat state
    JsonObject thermo = doc["thermostat"];
    proj.heatSetpoint = thermo["heatSetpoint"] | 68.0f;
    proj.coolSetpoint = thermo["coolSetpoint"] | 76.0f;
    proj.thermostatMode = thermo["mode"] | 0;
    proj.forceFurnace = thermo["forceFurnace"] | false;
    proj.forceNoHP = thermo["forceNoHP"] | false;

    // Thermostat timing
    JsonObject timing = thermo["timing"];
    proj.minOnTimeMs = timing["minOnMs"] | 180000;
    proj.minOffTimeMs = timing["minOffMs"] | 180000;
    proj.minIdleTimeMs = timing["minIdleMs"] | 60000;
    proj.maxRunTimeMs = timing["maxRunMs"] | 1800000;
    proj.escalationDelayMs = timing["escalationMs"] | 600000;

    // Temperature deadbands
    JsonObject deadband = thermo["deadband"];
    proj.heatDeadband = deadband["heat"] | 0.5f;
    proj.coolDeadband = deadband["cool"] | 0.5f;
    proj.heatOverrun = deadband["heatOverrun"] | 0.5f;
    proj.coolOverrun = deadband["coolOverrun"] | 0.5f;

    // Fan idle
    JsonObject fanIdle = thermo["fanIdle"];
    proj.fanIdleEnabled = fanIdle["enabled"] | false;
    proj.fanIdleWaitMin = fanIdle["waitMin"] | 15;
    proj.fanIdleRunMin = fanIdle["runMin"] | 5;

    // HX710 calibration
    JsonObject hx1 = doc["hx710"]["sensor1"];
    proj.hx710_1_raw1 = hx1["raw1"] | -134333;
    proj.hx710_1_val1 = hx1["val1"] | 0.3214f;
    proj.hx710_1_raw2 = hx1["raw2"] | 6340104;
    proj.hx710_1_val2 = hx1["val2"] | 83.4454f;

    JsonObject hx2 = doc["hx710"]["sensor2"];
    proj.hx710_2_raw1 = hx2["raw1"] | -134333;
    proj.hx710_2_val1 = hx2["val1"] | 3.4414f;
    proj.hx710_2_raw2 = hx2["raw2"] | 6340104;
    proj.hx710_2_val2 = hx2["val2"] | 86.5653f;

    Serial.printf("Read thermostat: mode=%d heat=%.1f cool=%.1f forceFurnace=%d forceNoHP=%d\n",
                  proj.thermostatMode, proj.heatSetpoint, proj.coolSetpoint,
                  proj.forceFurnace, proj.forceNoHP);

    // UI
    const char* uiTheme = doc["ui"]["theme"];
    proj.theme = (uiTheme != nullptr) ? String(uiTheme) : "dark";
    proj.pollIntervalSec = doc["ui"]["pollIntervalSec"] | 2;
    if (proj.pollIntervalSec < 1) proj.pollIntervalSec = 1;
    if (proj.pollIntervalSec > 10) proj.pollIntervalSec = 10;

    // Safe mode
    proj.forceSafeMode = doc["safeMode"]["force"] | false;

    // System identity
    JsonObject systemObj = doc["system"];
    const char* sysName = systemObj["name"];
    proj.systemName = (sysName != nullptr && strlen(sysName) > 0) ? String(sysName) : "AThermostat";
    const char* mqttPfx = systemObj["mqttPrefix"];
    proj.mqttPrefix = (mqttPfx != nullptr && strlen(mqttPfx) > 0) ? String(mqttPfx) : "thermostat";

    // Session timeout
    proj.sessionTimeoutMinutes = doc["auth"]["sessionTimeoutMinutes"] | 0;

    // Admin password
    const char* adminPw = doc["admin"]["password"];
    String adminPwStr = (adminPw != nullptr && strlen(adminPw) > 0) ? String(adminPw) : "";
    _adminPasswordHash = decryptPassword(adminPwStr);

    _configFile.close();
    return true;
}

bool Config::saveConfiguration(const char* filename, ProjectInfo& proj) {
    if (LittleFS.exists(filename)) {
        _configFile = LittleFS.open(filename, FILE_READ);
        if (_configFile && _configFile.size() > 0) {
            _configFile.close();
            return false;
        }
        _configFile.close();
    }
    _configFile = LittleFS.open(filename, FILE_WRITE);
    if (!_configFile) {
        Serial.printf("open failed: \"%s\"\n", filename);
        return false;
    }
    JsonDocument doc;

    doc["project"] = proj.name.length() > 0 ? proj.name : "AThermostat";
    doc["created"] = proj.createdOnDate;
    doc["description"] = proj.description;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = "MEGA";
    wifi["password"] = "";
    wifi["apFallbackSeconds"] = proj.apFallbackSeconds;

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["user"] = "debian";
    mqtt["password"] = "";
    mqtt["host"] = "192.168.1.1";
    mqtt["port"] = 1883;
    mqtt["tempTopic"] = proj.mqttTempTopic.length() > 0 ? proj.mqttTempTopic : "homeassistant/sensor/average_home_temperature/state";

    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["maxLogSize"] = proj.maxLogSize;
    logging["maxOldLogCount"] = proj.maxOldLogCount;

    JsonObject timezone = doc["timezone"].to<JsonObject>();
    timezone["posix"] = proj.timezone.length() > 0 ? proj.timezone : "CST6CDT,M3.2.0,M11.1.0";

    // Thermostat state
    JsonObject thermo = doc["thermostat"].to<JsonObject>();
    thermo["heatSetpoint"] = proj.heatSetpoint;
    thermo["coolSetpoint"] = proj.coolSetpoint;
    thermo["mode"] = proj.thermostatMode;
    thermo["forceFurnace"] = proj.forceFurnace;
    thermo["forceNoHP"] = proj.forceNoHP;

    JsonObject timing = thermo["timing"].to<JsonObject>();
    timing["minOnMs"] = proj.minOnTimeMs;
    timing["minOffMs"] = proj.minOffTimeMs;
    timing["minIdleMs"] = proj.minIdleTimeMs;
    timing["maxRunMs"] = proj.maxRunTimeMs;
    timing["escalationMs"] = proj.escalationDelayMs;

    JsonObject deadband = thermo["deadband"].to<JsonObject>();
    deadband["heat"] = proj.heatDeadband;
    deadband["cool"] = proj.coolDeadband;
    deadband["heatOverrun"] = proj.heatOverrun;
    deadband["coolOverrun"] = proj.coolOverrun;

    JsonObject fanIdle = thermo["fanIdle"].to<JsonObject>();
    fanIdle["enabled"] = proj.fanIdleEnabled;
    fanIdle["waitMin"] = proj.fanIdleWaitMin;
    fanIdle["runMin"] = proj.fanIdleRunMin;

    // HX710 calibration
    JsonObject hx710 = doc["hx710"].to<JsonObject>();
    JsonObject hx1 = hx710["sensor1"].to<JsonObject>();
    hx1["raw1"] = proj.hx710_1_raw1;
    hx1["val1"] = proj.hx710_1_val1;
    hx1["raw2"] = proj.hx710_1_raw2;
    hx1["val2"] = proj.hx710_1_val2;
    JsonObject hx2 = hx710["sensor2"].to<JsonObject>();
    hx2["raw1"] = proj.hx710_2_raw1;
    hx2["val1"] = proj.hx710_2_val1;
    hx2["raw2"] = proj.hx710_2_raw2;
    hx2["val2"] = proj.hx710_2_val2;

    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["theme"] = proj.theme.length() > 0 ? proj.theme : "dark";
    ui["pollIntervalSec"] = proj.pollIntervalSec;

    JsonObject safeModeObj = doc["safeMode"].to<JsonObject>();
    safeModeObj["force"] = proj.forceSafeMode;

    JsonObject systemObj = doc["system"].to<JsonObject>();
    systemObj["name"] = proj.systemName.length() > 0 ? proj.systemName : "AThermostat";
    systemObj["mqttPrefix"] = proj.mqttPrefix.length() > 0 ? proj.mqttPrefix : "thermostat";

    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["sessionTimeoutMinutes"] = proj.sessionTimeoutMinutes;

    JsonObject admin = doc["admin"].to<JsonObject>();
    admin["password"] = "";

    serializeJson(doc, _configFile);
    _configFile.close();

    String output;
    serializeJsonPretty(doc, output);
    Serial.println("Config saved:");
    Serial.println(output);
    return true;
}

bool Config::updateConfig(const char* filename, ProjectInfo& proj) {
    if (!_fsInitialized) return false;

    fs::File file = LittleFS.open(filename, FILE_READ);
    if (!file) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) return false;

    doc["project"] = proj.name;
    doc["description"] = proj.description;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = _wifiSSID;
    wifi["password"] = encryptPassword(_wifiPassword);
    wifi["apFallbackSeconds"] = proj.apFallbackSeconds;
    if (proj.apPassword.length() > 0) {
        wifi["apPassword"] = encryptPassword(proj.apPassword);
    } else {
        wifi.remove("apPassword");
    }
    if (proj.ftpPassword.length() > 0) {
        wifi["ftpPassword"] = encryptPassword(proj.ftpPassword);
    } else {
        wifi.remove("ftpPassword");
    }

    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["user"] = _mqttUser;
    mqtt["password"] = encryptPassword(_mqttPassword);
    mqtt["host"] = _mqttHost.toString();
    mqtt["port"] = _mqttPort;
    mqtt["tempTopic"] = proj.mqttTempTopic;

    JsonObject logging = doc["logging"].to<JsonObject>();
    logging["maxLogSize"] = proj.maxLogSize;
    logging["maxOldLogCount"] = proj.maxOldLogCount;

    JsonObject timezone = doc["timezone"].to<JsonObject>();
    timezone["posix"] = proj.timezone.length() > 0 ? proj.timezone : "CST6CDT,M3.2.0,M11.1.0";

    // Thermostat
    JsonObject thermo = doc["thermostat"].to<JsonObject>();
    thermo["heatSetpoint"] = proj.heatSetpoint;
    thermo["coolSetpoint"] = proj.coolSetpoint;
    thermo["mode"] = proj.thermostatMode;
    thermo["forceFurnace"] = proj.forceFurnace;
    thermo["forceNoHP"] = proj.forceNoHP;

    JsonObject timing = thermo["timing"].to<JsonObject>();
    timing["minOnMs"] = proj.minOnTimeMs;
    timing["minOffMs"] = proj.minOffTimeMs;
    timing["minIdleMs"] = proj.minIdleTimeMs;
    timing["maxRunMs"] = proj.maxRunTimeMs;
    timing["escalationMs"] = proj.escalationDelayMs;

    JsonObject deadband = thermo["deadband"].to<JsonObject>();
    deadband["heat"] = proj.heatDeadband;
    deadband["cool"] = proj.coolDeadband;
    deadband["heatOverrun"] = proj.heatOverrun;
    deadband["coolOverrun"] = proj.coolOverrun;

    JsonObject fanIdle = thermo["fanIdle"].to<JsonObject>();
    fanIdle["enabled"] = proj.fanIdleEnabled;
    fanIdle["waitMin"] = proj.fanIdleWaitMin;
    fanIdle["runMin"] = proj.fanIdleRunMin;

    // HX710
    JsonObject hx710 = doc["hx710"].to<JsonObject>();
    JsonObject hx1 = hx710["sensor1"].to<JsonObject>();
    hx1["raw1"] = proj.hx710_1_raw1;
    hx1["val1"] = proj.hx710_1_val1;
    hx1["raw2"] = proj.hx710_1_raw2;
    hx1["val2"] = proj.hx710_1_val2;
    JsonObject hx2 = hx710["sensor2"].to<JsonObject>();
    hx2["raw1"] = proj.hx710_2_raw1;
    hx2["val1"] = proj.hx710_2_val1;
    hx2["raw2"] = proj.hx710_2_raw2;
    hx2["val2"] = proj.hx710_2_val2;

    JsonObject ui = doc["ui"].to<JsonObject>();
    ui["theme"] = proj.theme.length() > 0 ? proj.theme : "dark";
    ui["pollIntervalSec"] = proj.pollIntervalSec;

    JsonObject safeModeUpd = doc["safeMode"].to<JsonObject>();
    safeModeUpd["force"] = proj.forceSafeMode;

    JsonObject systemObj = doc["system"].to<JsonObject>();
    systemObj["name"] = proj.systemName.length() > 0 ? proj.systemName : "AThermostat";
    systemObj["mqttPrefix"] = proj.mqttPrefix.length() > 0 ? proj.mqttPrefix : "thermostat";

    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["sessionTimeoutMinutes"] = proj.sessionTimeoutMinutes;

    JsonObject admin = doc["admin"].to<JsonObject>();
    admin["password"] = encryptPassword(_adminPasswordHash);

    file = LittleFS.open(filename, FILE_WRITE);
    if (!file) return false;
    serializeJson(doc, file);
    file.close();
    return true;
}

bool Config::updateThermostatState(const char* filename, ProjectInfo& proj) {
    if (!_fsInitialized) return false;

    fs::File file = LittleFS.open(filename, FILE_READ);
    if (!file) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) return false;

    // Only update volatile thermostat state
    doc["thermostat"]["heatSetpoint"] = proj.heatSetpoint;
    doc["thermostat"]["coolSetpoint"] = proj.coolSetpoint;
    doc["thermostat"]["mode"] = proj.thermostatMode;
    doc["thermostat"]["forceFurnace"] = proj.forceFurnace;
    doc["thermostat"]["forceNoHP"] = proj.forceNoHP;

    file = LittleFS.open(filename, FILE_WRITE);
    if (!file) return false;
    serializeJson(doc, file);
    file.close();
    return true;
}

bool Config::loadCertificates(const char* certFile, const char* keyFile) {
    if (!_fsInitialized) return false;

    auto readFile = [](const char* path, uint8_t*& buf, size_t& len) -> bool {
        fs::File f = LittleFS.open(path, FILE_READ);
        if (!f) return false;
        len = f.size();
        if (len == 0) { f.close(); return false; }
        buf = (uint8_t*)ps_malloc(len + 1);
        if (!buf) { f.close(); len = 0; return false; }
        if ((size_t)f.read(buf, len) != len) {
            free(buf); buf = nullptr; len = 0; f.close(); return false;
        }
        buf[len] = '\0';
        f.close();
        return true;
    };

    bool certOk = readFile(certFile, _certBuf, _certLen);
    bool keyOk = readFile(keyFile, _keyBuf, _keyLen);

    if (!certOk || !keyOk) {
        if (_certBuf) { free(_certBuf); _certBuf = nullptr; _certLen = 0; }
        if (_keyBuf) { free(_keyBuf); _keyBuf = nullptr; _keyLen = 0; }
        return false;
    }
    return true;
}

bool Config::generateSelfSignedCert() {
    if (!_fsInitialized) return false;

    if (_certBuf) { free(_certBuf); _certBuf = nullptr; _certLen = 0; }
    if (_keyBuf) { free(_keyBuf); _keyBuf = nullptr; _keyLen = 0; }

    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_mpi serial;
    bool success = false;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_mpi_init(&serial);

    const char* pers = "esp32_cert_gen";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const uint8_t*)pers, strlen(pers));
    if (ret != 0) goto cleanup;

    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) goto cleanup;

    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                               mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) goto cleanup;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);

    {
        String cn = "CN=";
        cn += (_proj && _proj->systemName.length() > 0) ? _proj->systemName : "AThermostat";
        ret = mbedtls_x509write_crt_set_subject_name(&crt, cn.c_str());
        if (ret != 0) goto cleanup;
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, cn.c_str());
        if (ret != 0) goto cleanup;
    }

    ret = mbedtls_mpi_lset(&serial, (int)esp_random());
    if (ret != 0) goto cleanup;
    ret = mbedtls_x509write_crt_set_serial(&crt, &serial);
    if (ret != 0) goto cleanup;

    {
        struct tm timeinfo;
        char notBefore[16], notAfter[16];
        if (getLocalTime(&timeinfo, 0)) {
            strftime(notBefore, sizeof(notBefore), "%Y%m%d%H%M%S", &timeinfo);
            timeinfo.tm_year += 10;
            strftime(notAfter, sizeof(notAfter), "%Y%m%d%H%M%S", &timeinfo);
        } else {
            strcpy(notBefore, "20260101000000");
            strcpy(notAfter,  "20360101000000");
        }
        ret = mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter);
        if (ret != 0) goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1);
    if (ret != 0) goto cleanup;
    ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    if (ret != 0) goto cleanup;
    ret = mbedtls_x509write_crt_set_authority_key_identifier(&crt);
    if (ret != 0) goto cleanup;

    {
        const size_t bufSize = 4096;
        uint8_t* certPem = (uint8_t*)ps_malloc(bufSize);
        uint8_t* keyPem = (uint8_t*)ps_malloc(bufSize);
        if (!certPem || !keyPem) {
            if (certPem) free(certPem);
            if (keyPem) free(keyPem);
            goto cleanup;
        }

        ret = mbedtls_x509write_crt_pem(&crt, certPem, bufSize,
                                          mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret != 0) { free(certPem); free(keyPem); goto cleanup; }

        ret = mbedtls_pk_write_key_pem(&key, keyPem, bufSize);
        if (ret != 0) { free(certPem); free(keyPem); goto cleanup; }

        _certLen = strlen((char*)certPem);
        _keyLen = strlen((char*)keyPem);

        fs::File cf = LittleFS.open("/cert.pem", FILE_WRITE);
        if (cf) { cf.write(certPem, _certLen); cf.close(); }
        else { free(certPem); free(keyPem); goto cleanup; }

        fs::File kf = LittleFS.open("/key.pem", FILE_WRITE);
        if (kf) { kf.write(keyPem, _keyLen); kf.close(); }
        else { free(certPem); free(keyPem); goto cleanup; }

        _certBuf = certPem;
        _keyBuf = keyPem;
        success = true;
    }

cleanup:
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return success;
}

bool Config::isCertExpired() const {
    if (!_certBuf || _certLen == 0) return false;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return false;

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);

    int ret = mbedtls_x509_crt_parse(&crt, _certBuf, _certLen + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return false;
    }

    const mbedtls_x509_time& na = crt.valid_to;
    bool expired = false;
    int year = timeinfo.tm_year + 1900;
    int mon = timeinfo.tm_mon + 1;
    int day = timeinfo.tm_mday;

    if (year > na.year) expired = true;
    else if (year == na.year && mon > na.mon) expired = true;
    else if (year == na.year && mon == na.mon && day > na.day) expired = true;

    mbedtls_x509_crt_free(&crt);
    return expired;
}
