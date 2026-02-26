#ifndef HTTPSSERVER_H
#define HTTPSSERVER_H

#include <cstdint>
#include <cstddef>
#include <functional>

// Forward declarations — avoid including esp_https_server.h here
// to prevent enum conflicts with ESPAsyncWebServer
class Config;
class Thermostat;
class Scheduler;
class Task;
class SessionManager;
class HX710;

struct HttpsContext {
    Config* config;
    Thermostat* thermostat;
    Scheduler* scheduler;
    bool* shouldReboot;
    Task** delayedReboot;
    String* timezone;
    HX710* pressure1;
    HX710* pressure2;
    // WiFi test state (shared with WebHandler)
    String* wifiTestState;
    String* wifiTestMessage;
    String* wifiTestNewSSID;
    String* wifiTestNewPassword;
    String* wifiOldSSID;
    String* wifiOldPassword;
    uint8_t* wifiTestCountdown;
    Task** wifiTestTask;
    std::function<String()> apStartCb;
    std::function<void()> apStopCb;
    std::function<void(int)> ftpEnableCb;
    std::function<void()> ftpDisableCb;
    bool* ftpActive;
    unsigned long* ftpStopTime;
    String systemName;
    bool* rebootRateLimited;
    bool* safeMode;
    SessionManager* sessionMgr;
};

// Opaque handle
typedef void* HttpsServerHandle;

HttpsServerHandle httpsStart(const uint8_t* cert, size_t certLen,
                             const uint8_t* key, size_t keyLen,
                             HttpsContext* ctx);

#endif
