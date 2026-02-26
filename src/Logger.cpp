#include "Logger.h"
#include <ESPAsyncWebServer.h>
#include <stdarg.h>
#include <time.h>
#include <WiFi.h>

Logger Log;

static const char* LEVEL_NAMES[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};

Logger::Logger()
    : _level(LOG_INFO)
    , _serialEnabled(true)
    , _mqttEnabled(false)
    , _fileLogEnabled(false)
    , _wsEnabled(false)
    , _mqttClient(nullptr)
    , _mqttTopic("thermostat/log")
    , _ws(nullptr)
    , _fsReady(false)
    , _logFilename("/log.txt")
    , _maxFileSize(DEFAULT_MAX_FILE_SIZE)
    , _maxRotatedFiles(DEFAULT_MAX_ROTATED_FILES)
    , _ringBufferMax(DEFAULT_RING_BUFFER_SIZE)
    , _ringBufferHead(0)
    , _ringBufferCount(0)
{
    _ringBuffer.resize(_ringBufferMax);
}

void Logger::setLevel(Level level) {
    _level = level;
}

Logger::Level Logger::getLevel() {
    return _level;
}

const char* Logger::getLevelName(Level level) {
    if (level >= 0 && level <= LOG_DEBUG) {
        return LEVEL_NAMES[level];
    }
    return "UNKN ";
}

void Logger::setMqttClient(AsyncMqttClient* client, const char* topic) {
    _mqttClient = client;
    _mqttTopic = topic;
    _mqttEnabled = (client != nullptr);
}

void Logger::setLogFile(const char* filename, uint32_t maxFileSize, uint8_t maxRotatedFiles) {
    _logFilename = filename;
    _maxFileSize = maxFileSize;
    _maxRotatedFiles = maxRotatedFiles;
    _fsReady = true;
    _fileLogEnabled = true;
}

void Logger::enableSerial(bool enable) {
    _serialEnabled = enable;
}

void Logger::enableMqtt(bool enable) {
    _mqttEnabled = enable && (_mqttClient != nullptr);
}

void Logger::enableFileLog(bool enable) {
    _fileLogEnabled = enable && _fsReady;
}

bool Logger::isSerialEnabled() {
    return _serialEnabled;
}

bool Logger::isMqttEnabled() {
    return _mqttEnabled;
}

bool Logger::isFileLogEnabled() {
    return _fileLogEnabled;
}

void Logger::setWebSocket(AsyncWebSocket* ws) {
    _ws = ws;
    _wsEnabled = (ws != nullptr);
}

void Logger::enableWebSocket(bool enable) {
    _wsEnabled = enable && (_ws != nullptr);
}

bool Logger::isWebSocketEnabled() {
    return _wsEnabled;
}

void Logger::setRingBufferSize(size_t maxEntries) {
    _ringBufferMax = maxEntries;
    _ringBuffer.resize(_ringBufferMax);
    _ringBufferHead = 0;
    _ringBufferCount = 0;
}

const std::vector<String>& Logger::getRingBuffer() const {
    return _ringBuffer;
}

size_t Logger::getRingBufferHead() const {
    return _ringBufferHead;
}

size_t Logger::getRingBufferCount() const {
    return _ringBufferCount;
}

void Logger::addToRingBuffer(const char* msg) {
    _ringBuffer[_ringBufferHead] = String(msg);
    _ringBufferHead = (_ringBufferHead + 1) % _ringBufferMax;
    if (_ringBufferCount < _ringBufferMax) {
        _ringBufferCount++;
    }
}

void Logger::writeToWebSocket(const char* msg) {
    if (_ws == nullptr || _ws->count() == 0) {
        return;
    }
    String json = "{\"type\":\"log\",\"message\":\"";
    for (const char* p = msg; *p; p++) {
        switch (*p) {
            case '"':  json += "\\\""; break;
            case '\\': json += "\\\\"; break;
            case '\n': json += "\\n"; break;
            case '\r': json += "\\r"; break;
            case '\t': json += "\\t"; break;
            default:   json += *p; break;
        }
    }
    json += "\"}";
    _ws->textAll(json);
}

void Logger::error(const char* tag, const char* format, ...) {
    if (_level >= LOG_ERROR) {
        va_list args;
        va_start(args, format);
        log(LOG_ERROR, tag, format, args);
        va_end(args);
    }
}

void Logger::warn(const char* tag, const char* format, ...) {
    if (_level >= LOG_WARN) {
        va_list args;
        va_start(args, format);
        log(LOG_WARN, tag, format, args);
        va_end(args);
    }
}

void Logger::info(const char* tag, const char* format, ...) {
    if (_level >= LOG_INFO) {
        va_list args;
        va_start(args, format);
        log(LOG_INFO, tag, format, args);
        va_end(args);
    }
}

void Logger::debug(const char* tag, const char* format, ...) {
    if (_level >= LOG_DEBUG) {
        va_list args;
        va_start(args, format);
        log(LOG_DEBUG, tag, format, args);
        va_end(args);
    }
}

void Logger::log(Level level, const char* tag, const char* format, va_list args) {
    char msgBuffer[384];
    vsnprintf(msgBuffer, sizeof(msgBuffer), format, args);

    struct tm timeinfo;
    char timeStr[20] = "----/--/-- --:--:--";
    if(WiFi.isConnected()){
        if (getLocalTime(&timeinfo)) {
            strftime(timeStr, sizeof(timeStr), "%Y/%m/%d %H:%M:%S", &timeinfo);
        }
    }
    snprintf(_buffer, sizeof(_buffer), "[%s] [%s] [%s] %s",
             timeStr, getLevelName(level), tag, msgBuffer);

    addToRingBuffer(_buffer);

    if (_serialEnabled) {
        writeToSerial(_buffer);
    }
    if (_mqttEnabled) {
        writeToMqtt(_buffer);
    }
    if (_fileLogEnabled) {
        writeToFile(_buffer);
    }
    if (_wsEnabled) {
        writeToWebSocket(_buffer);
    }
}

void Logger::writeToSerial(const char* msg) {
    Serial.println(msg);
}

void Logger::writeToMqtt(const char* msg) {
    if (_mqttClient == nullptr || !_mqttClient->connected()) {
        return;
    }
    _mqttClient->publish(_mqttTopic.c_str(), 0, false, msg);
}

void Logger::writeToFile(const char* msg) {
    if (!_fsReady) {
        return;
    }

    // Check file size first
    fs::File logFile = LittleFS.open(_logFilename.c_str(), FILE_READ);
    if (logFile) {
        size_t sz = logFile.size();
        logFile.close();
        if (sz > _maxFileSize) {
            rotateLogFiles();
        }
    }

    logFile = LittleFS.open(_logFilename.c_str(), FILE_APPEND);
    if (!logFile) {
        return;
    }

    logFile.println(msg);
    logFile.close();
}

String Logger::getRotatedFilename(uint8_t index) {
    int dotIndex = _logFilename.lastIndexOf('.');
    String baseName = (dotIndex > 0) ? _logFilename.substring(0, dotIndex) : _logFilename;
    return baseName + "." + String(index) + ".txt";
}

void Logger::rotateLogFiles() {
    if (!_fsReady) {
        return;
    }

    Serial.println("[Logger] Starting log rotation...");

    // Delete the oldest rotated file if it exists
    String oldestFile = getRotatedFilename(_maxRotatedFiles);
    if (LittleFS.exists(oldestFile.c_str())) {
        LittleFS.remove(oldestFile.c_str());
        Serial.printf("[Logger] Deleted oldest: %s\n", oldestFile.c_str());
    }

    // Shift existing rotated files
    for (int i = _maxRotatedFiles - 1; i >= 1; i--) {
        String oldName = getRotatedFilename(i);
        String newName = getRotatedFilename(i + 1);

        if (LittleFS.exists(oldName.c_str())) {
            LittleFS.rename(oldName.c_str(), newName.c_str());
            Serial.printf("[Logger] Renamed %s -> %s\n", oldName.c_str(), newName.c_str());
        }
    }

    // Rename current log to .1.txt (no compression on LittleFS to save flash wear)
    String rotatedName = getRotatedFilename(1);
    if (LittleFS.rename(_logFilename.c_str(), rotatedName.c_str())) {
        Serial.printf("[Logger] Rotated %s -> %s\n", _logFilename.c_str(), rotatedName.c_str());
    } else {
        Serial.printf("[Logger] CRITICAL: Failed to rotate %s\n", _logFilename.c_str());
    }

    Serial.println("[Logger] Log rotation complete");
}
