#ifndef WEBHANDLER_H
#define WEBHANDLER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Update.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>
#include <functional>
#include <TaskSchedulerDeclarations.h>
#include "Thermostat.h"
#include "Logger.h"
#include "Config.h"
#include "HttpsServer.h"
#include "SessionManager.h"

class HX710;

class WebHandler {
  public:
    WebHandler(uint16_t port, Scheduler* ts, Thermostat* thermostat);
    void begin();
    bool beginSecure(const uint8_t* cert, size_t certLen, const uint8_t* key, size_t keyLen);
    bool isSecureRunning() const { return _httpsServer != nullptr; }
    void startNtpSync();
    void setTimezone(const String& tz);
    void setConfig(Config* config) { _config = config; }
    bool shouldReboot() const { return _shouldReboot; }
    void setRebootRateLimited(bool* flag) { _rebootRateLimited = flag; }
    void setSafeMode(bool* flag, uint32_t* crashCount) { _safeMode = flag; _crashBootCount = crashCount; }
    void setPressureSensors(HX710* s1, HX710* s2) { _pressure1 = s1; _pressure2 = s2; }
    const char* getWiFiIP();

    typedef std::function<String()> APStartCallback;
    typedef std::function<void()> APStopCallback;
    void setAPCallbacks(APStartCallback startCb, APStopCallback stopCb);

    typedef std::function<void(int)> FtpEnableCallback;
    typedef std::function<void()> FtpDisableCallback;
    typedef std::function<String()> FtpStatusCallback;
    void setFtpControl(FtpEnableCallback enableCb, FtpDisableCallback disableCb, FtpStatusCallback statusCb);
    void setFtpState(bool* activePtr, unsigned long* stopTimePtr);

  private:
    AsyncWebServer _server;
    AsyncWebSocket _ws;
    HttpsServerHandle _httpsServer = nullptr;
    HttpsContext _httpsCtx = {};
    SessionManager _sessionMgr;

    Scheduler* _ts;
    Thermostat* _thermostat;
    Config* _config;
    HX710* _pressure1 = nullptr;
    HX710* _pressure2 = nullptr;

    bool _shouldReboot;
    bool* _rebootRateLimited = nullptr;
    bool* _safeMode = nullptr;
    uint32_t* _crashBootCount = nullptr;
    Task* _tDelayedReboot;
    bool _ntpSynced;
    Task* _tNtpSync;

    static constexpr float MB_MULTIPLIER = 1.0f / (1024.0f * 1024.0f);

    // NTP config
    static constexpr const char* NTP_SERVER1 = "192.168.0.1";
    static constexpr const char* NTP_SERVER2 = "time.nist.gov";
    String _timezone = "CST6CDT,M3.2.0,M11.1.0";
    static constexpr const char* NOT_AVAILABLE = "NA";
    String _wifiIPStr;

    APStartCallback _apStartCb;
    APStopCallback _apStopCb;

    FtpEnableCallback _ftpEnableCb;
    FtpDisableCallback _ftpDisableCb;
    FtpStatusCallback _ftpStatusCb;
    bool* _ftpActivePtr = nullptr;
    unsigned long* _ftpStopTimePtr = nullptr;

    // OTA upload state
    bool _otaUploadOk = false;

    // WiFi test state
    String _wifiTestState = "idle";
    String _wifiTestMessage;
    String _wifiTestNewSSID;
    String _wifiTestNewPassword;
    String _wifiOldSSID;
    String _wifiOldPassword;
    Task* _tWifiTest = nullptr;
    uint8_t _wifiTestCountdown = 0;

    bool isRebootBlocked() const { return _rebootRateLimited && *_rebootRateLimited; }
    bool checkAuth(AsyncWebServerRequest* request);
    void redirectToLogin(AsyncWebServerRequest* request, bool expired);
    void syncNtpTime();
    void setupRoutes();
    void serveFile(AsyncWebServerRequest* request, const String& path);
    static const char* getContentType(const String& path);
    void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len);
};

#endif
