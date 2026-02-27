#include "WebHandler.h"
#include <LittleFS.h>
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include "OtaUtils.h"
#include "HX710.h"
#include "mbedtls/base64.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

extern const char compile_date[];
extern uint8_t getCpuLoadCore0();
extern uint8_t getCpuLoadCore1();
extern bool _apModeActive;

WebHandler::WebHandler(uint16_t port, Scheduler* ts, Thermostat* thermostat)
    : _server(port), _ws("/ws"), _ts(ts), _thermostat(thermostat),
      _config(nullptr), _shouldReboot(false), _tDelayedReboot(nullptr),
      _ntpSynced(false), _tNtpSync(nullptr) {}

void WebHandler::setAPCallbacks(APStartCallback startCb, APStopCallback stopCb) {
    _apStartCb = startCb;
    _apStopCb = stopCb;
}

void WebHandler::setFtpControl(FtpEnableCallback enableCb, FtpDisableCallback disableCb, FtpStatusCallback statusCb) {
    _ftpEnableCb = enableCb;
    _ftpDisableCb = disableCb;
    _ftpStatusCb = statusCb;
}

void WebHandler::setFtpState(bool* activePtr, unsigned long* stopTimePtr) {
    _ftpActivePtr = activePtr;
    _ftpStopTimePtr = stopTimePtr;
}

bool WebHandler::checkAuth(AsyncWebServerRequest* request) {
    if (!_config || !_config->hasAdminPassword()) return true;

    // Session mode: check cookie first
    if (_sessionMgr.isEnabled()) {
        if (request->hasHeader("Cookie")) {
            String token = SessionManager::extractSessionToken(request->header("Cookie"));
            if (token.length() > 0) {
                if (_sessionMgr.validateSession(token))
                    return true;
            }
        }
        if (!request->hasHeader("Authorization")) {
            redirectToLogin(request, false);
            return false;
        }
    }

    // Basic Auth
    String authHeader = request->header("Authorization");
    if (!authHeader.startsWith("Basic ")) {
        request->requestAuthentication(nullptr, false);
        return false;
    }

    String b64 = authHeader.substring(6);
    size_t decodedLen = 0;
    mbedtls_base64_decode(nullptr, 0, &decodedLen,
        (const uint8_t*)b64.c_str(), b64.length());
    uint8_t* decoded = new uint8_t[decodedLen + 1];
    mbedtls_base64_decode(decoded, decodedLen + 1, &decodedLen,
        (const uint8_t*)b64.c_str(), b64.length());
    decoded[decodedLen] = '\0';

    String credentials = String((char*)decoded);
    delete[] decoded;

    int colonIdx = credentials.indexOf(':');
    if (colonIdx < 0) {
        request->requestAuthentication(nullptr, false);
        return false;
    }

    String password = credentials.substring(colonIdx + 1);
    if (_config->verifyAdminPassword(password)) {
        return true;
    }

    request->requestAuthentication(nullptr, false);
    return false;
}

void WebHandler::redirectToLogin(AsyncWebServerRequest* request, bool expired) {
    String url = "/login?redirect=" + request->url();
    if (expired) url += "&expired=1";
    request->redirect(url);
}

void WebHandler::begin() {
    _tNtpSync = new Task(2 * TASK_HOUR, TASK_FOREVER, [this]() {
        this->syncNtpTime();
    }, _ts, false);

    // CORS headers for all responses (needed when captive portal DNS is active)
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    _server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
            return;
        }
        request->send(404);
    });

    _ws.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
        this->onWsEvent(server, client, type, arg, data, len);
    });
    _server.addHandler(&_ws);

    Log.setWebSocket(&_ws);
    Log.enableWebSocket(true);

    if (_config && _config->getProjectInfo()) {
        _sessionMgr.setTimeoutMinutes(_config->getProjectInfo()->sessionTimeoutMinutes);
    }

    setupRoutes();
    _server.begin();
    Log.info("HTTP", "HTTP server started");
}

bool WebHandler::beginSecure(const uint8_t* cert, size_t certLen, const uint8_t* key, size_t keyLen) {
    _httpsCtx.config = _config;
    _httpsCtx.thermostat = _thermostat;
    _httpsCtx.scheduler = _ts;
    _httpsCtx.shouldReboot = &_shouldReboot;
    _httpsCtx.delayedReboot = &_tDelayedReboot;
    _httpsCtx.timezone = &_timezone;
    _httpsCtx.pressure1 = _pressure1;
    _httpsCtx.pressure2 = _pressure2;
    _httpsCtx.wifiTestState = &_wifiTestState;
    _httpsCtx.wifiTestMessage = &_wifiTestMessage;
    _httpsCtx.wifiTestNewSSID = &_wifiTestNewSSID;
    _httpsCtx.wifiTestNewPassword = &_wifiTestNewPassword;
    _httpsCtx.wifiOldSSID = &_wifiOldSSID;
    _httpsCtx.wifiOldPassword = &_wifiOldPassword;
    _httpsCtx.wifiTestCountdown = &_wifiTestCountdown;
    _httpsCtx.wifiTestTask = &_tWifiTest;
    _httpsCtx.apStartCb = _apStartCb;
    _httpsCtx.apStopCb = _apStopCb;
    _httpsCtx.ftpEnableCb = _ftpEnableCb;
    _httpsCtx.ftpDisableCb = _ftpDisableCb;
    _httpsCtx.ftpActive = _ftpActivePtr;
    _httpsCtx.ftpStopTime = _ftpStopTimePtr;
    _httpsCtx.systemName = _config && _config->getProjectInfo() ?
        _config->getProjectInfo()->systemName : "AThermostat";
    _httpsCtx.rebootRateLimited = _rebootRateLimited;
    _httpsCtx.safeMode = _safeMode;
    _httpsCtx.sessionMgr = &_sessionMgr;

    _httpsServer = httpsStart(cert, certLen, key, keyLen, &_httpsCtx);
    if (_httpsServer) {
        Log.info("HTTPS", "HTTPS server started on port 443");
        return true;
    }
    Log.warn("HTTPS", "HTTPS server failed to start");
    return false;
}

const char* WebHandler::getWiFiIP() {
    if (!WiFi.isConnected()) return NOT_AVAILABLE;
    _wifiIPStr = WiFi.localIP().toString();
    return _wifiIPStr.length() > 0 ? _wifiIPStr.c_str() : NOT_AVAILABLE;
}

void WebHandler::startNtpSync() {
    if (_tNtpSync) {
        _tNtpSync->enable();
    }
}

void WebHandler::setTimezone(const String& tz) {
    _timezone = tz;
}

void WebHandler::syncNtpTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Log.warn("NTP", "WiFi not connected, skipping NTP sync");
        return;
    }
    Log.info("NTP", "Syncing time from NTP servers...");
    configTzTime(_timezone.c_str(), NTP_SERVER1, NTP_SERVER2);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M:%S", &timeinfo);
        Log.info("NTP", "Time synced: %s", buf);
        _ntpSynced = true;
    } else {
        Log.warn("NTP", "NTP sync failed");
    }
}

const char* WebHandler::getContentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".ico")) return "image/x-icon";
    return "text/plain";
}

void WebHandler::serveFile(AsyncWebServerRequest* request, const String& path) {
    String fullPath = "/www" + path;
    if (LittleFS.exists(fullPath)) {
        request->send(LittleFS, fullPath, getContentType(path));
    } else {
        request->send(404, "text/plain", "Not found");
    }
}

void WebHandler::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                           AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Log.debug("WS", "Client connected: %u", client->id());
    } else if (type == WS_EVT_DISCONNECT) {
        Log.debug("WS", "Client disconnected: %u", client->id());
    }
}

void WebHandler::setupRoutes() {
    // --- Static file serving ---
    _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
        request->redirect("/dashboard");
    });

    _server.on("/dashboard", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        serveFile(request, "/dashboard.html");
    });

    _server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        serveFile(request, "/config.html");
    });

    _server.on("/pins", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        serveFile(request, "/pins.html");
    });

    _server.on("/update", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        serveFile(request, "/update.html");
    });

    _server.on("/login", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/login.html");
    });

    _server.on("/log/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        serveFile(request, "/log.html");
    });

    _server.on("/heap/view", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        serveFile(request, "/heap.html");
    });

    _server.on("/admin", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/admin.html");
    });

    _server.on("/theme.css", HTTP_GET, [this](AsyncWebServerRequest *request) {
        serveFile(request, "/theme.css");
    });

    // --- Theme API ---
    _server.on("/theme", HTTP_GET, [this](AsyncWebServerRequest *request) {
        JsonDocument doc;
        if (_config && _config->getProjectInfo()) {
            doc["theme"] = _config->getProjectInfo()->theme;
            doc["systemName"] = _config->getProjectInfo()->systemName;
        } else {
            doc["theme"] = "dark";
            doc["systemName"] = "AThermostat";
        }
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- Login API ---
    _server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index + len != total) return;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        String pw = doc["password"] | "";
        if (_config && _config->verifyAdminPassword(pw)) {
            String clientIP = request->client()->remoteIP().toString();
            String token = _sessionMgr.createSession(clientIP);
            AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"ok\":true}");
            String cookie = "session=" + token + "; Path=/; HttpOnly; SameSite=Strict";
            response->addHeader("Set-Cookie", cookie);
            request->send(response);
        } else {
            request->send(401, "application/json", "{\"error\":\"Invalid password\"}");
        }
    });

    // --- Admin setup ---
    _server.on("/admin/setup", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index + len != total) return;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) { request->send(400); return; }
        String pw = doc["password"] | "";
        String cf = doc["confirm"] | "";
        if (pw.length() < 4) {
            request->send(400, "application/json", "{\"error\":\"Password too short\"}");
            return;
        }
        if (pw != cf) {
            request->send(400, "application/json", "{\"error\":\"Passwords do not match\"}");
            return;
        }
        _config->setAdminPassword(pw);
        _config->updateConfig("/config.txt", *_config->getProjectInfo());
        request->send(200, "application/json", "{\"message\":\"Password set\"}");
    });

    // --- Thermostat status API ---
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        JsonDocument doc;
        doc["mode"] = Thermostat::modeToString(_thermostat->getMode());
        doc["action"] = Thermostat::actionToString(_thermostat->getAction());
        doc["heat_level"] = Thermostat::heatLevelToString(_thermostat->getHeatLevel());
        doc["cool_level"] = Thermostat::coolLevelToString(_thermostat->getCoolLevel());

        if (_thermostat->hasValidTemperature()) {
            doc["current_temp"] = serialized(String(_thermostat->getCurrentTemperature(), 1));
        } else {
            doc["current_temp"] = nullptr;
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
            OutPin* p = _thermostat->getOutput((OutputIdx)i);
            if (p) outputs[outNames[i]] = p->isPinOn();
        }

        JsonObject inputs = doc["inputs"].to<JsonObject>();
        static const char* inNames[] = {"out_temp_ok","defrost_mode"};
        for (int i = 0; i < IN_COUNT; i++) {
            InputPin* p = _thermostat->getInput((InputIdx)i);
            if (p) inputs[inNames[i]] = p->isActive();
        }

        // Pressure sensors
        if (_pressure1 && _pressure1->isValid()) {
            doc["pressure1"] = serialized(String(_pressure1->getLastValue(), 2));
        }
        if (_pressure2 && _pressure2->isValid()) {
            doc["pressure2"] = serialized(String(_pressure2->getLastValue(), 2));
        }

        doc["uptime"] = millis() / 1000;
        doc["wifi_ip"] = getWiFiIP();
        doc["build"] = compile_date;
        doc["safe_mode"] = _safeMode ? *_safeMode : false;

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- Set mode ---
    _server.on("/api/mode", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (index + len != total) return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { request->send(400); return; }
        const char* mode = doc["mode"];
        if (!mode) { request->send(400, "application/json", "{\"error\":\"missing mode\"}"); return; }
        _thermostat->setMode(Thermostat::stringToMode(mode));
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // --- Set setpoints ---
    _server.on("/api/setpoint", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (index + len != total) return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { request->send(400); return; }
        if (doc.containsKey("heat")) _thermostat->setHeatSetpoint(doc["heat"].as<float>());
        if (doc.containsKey("cool")) _thermostat->setCoolSetpoint(doc["cool"].as<float>());
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // --- Fan idle settings ---
    _server.on("/api/fan_idle", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (index + len != total) return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { request->send(400); return; }
        ThermostatConfig& cfg = _thermostat->config();
        if (doc.containsKey("enabled")) cfg.fanIdleEnabled = doc["enabled"].as<bool>();
        if (doc.containsKey("wait_min")) cfg.fanIdleWaitMin = doc["wait_min"].as<uint32_t>();
        if (doc.containsKey("run_min")) cfg.fanIdleRunMin = doc["run_min"].as<uint32_t>();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // --- Force no HP ---
    _server.on("/api/force_no_hp", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (index + len != total) return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { request->send(400); return; }
        if (doc.containsKey("enabled")) _thermostat->setForceNoHP(doc["enabled"].as<bool>());
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // --- Force furnace ---
    _server.on("/api/force_furnace", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (index + len != total) return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { request->send(400); return; }
        if (doc.containsKey("enabled")) _thermostat->setForceFurnace(doc["enabled"].as<bool>());
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // --- Heap / system info ---
    _server.on("/heap", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        JsonDocument doc;
        doc["free heap"] = ESP.getFreeHeap();
        doc["free psram MB"] = ESP.getFreePsram() * MB_MULTIPLIER;
        doc["used psram MB"] = (ESP.getPsramSize() - ESP.getFreePsram()) * MB_MULTIPLIER;
        doc["cpuLoad0"] = getCpuLoadCore0();
        doc["cpuLoad1"] = getCpuLoadCore1();
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- Log API ---
    _server.on("/log", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        int limit = 100;
        if (request->hasParam("limit")) {
            limit = request->getParam("limit")->value().toInt();
        }
        if (limit < 1) limit = 1;
        if (limit > 500) limit = 500;

        const auto& ringBuf = Log.getRingBuffer();
        size_t count = Log.getRingBufferCount();
        size_t head = Log.getRingBufferHead();
        size_t max = ringBuf.size();

        JsonDocument doc;
        JsonArray entries = doc["entries"].to<JsonArray>();

        size_t start = 0;
        if (count > (size_t)limit) start = count - limit;

        for (size_t i = start; i < count; i++) {
            size_t idx = (head + max - count + i) % max;
            entries.add(ringBuf[idx]);
        }
        doc["count"] = count;

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- Pins API ---
    _server.on("/api/pins", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        JsonDocument doc;

        static const char* outNames[] = {"fan1","rev","furn_cool_low","furn_cool_high","w1","w2","comp1","comp2"};
        static const char* outBoardPins[] = {"GPIO4","GPIO5","GPIO6","GPIO7","GPIO15","GPIO16","GPIO17","GPIO18"};
        JsonArray outs = doc["outputs"].to<JsonArray>();
        for (int i = 0; i < OUT_COUNT; i++) {
            JsonObject o = outs.add<JsonObject>();
            o["name"] = outNames[i];
            o["boardPin"] = outBoardPins[i];
            OutPin* p = _thermostat->getOutput((OutputIdx)i);
            o["on"] = p ? p->isPinOn() : false;
        }

        static const char* inNames[] = {"out_temp_ok","defrost_mode"};
        static const char* inBoardPins[] = {"GPIO45","GPIO47"};
        JsonArray ins = doc["inputs"].to<JsonArray>();
        for (int i = 0; i < IN_COUNT; i++) {
            JsonObject o = ins.add<JsonObject>();
            o["name"] = inNames[i];
            o["boardPin"] = inBoardPins[i];
            InputPin* p = _thermostat->getInput((InputIdx)i);
            o["active"] = p ? p->isActive() : false;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- eFuse API ---
    _server.on("/api/efuse", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;

        doc["DIS_USB_JTAG"] = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG);
        doc["DIS_USB_SERIAL_JTAG"] = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_SERIAL_JTAG);
        doc["DIS_PAD_JTAG"] = esp_efuse_read_field_bit(ESP_EFUSE_HARD_DIS_JTAG);
        doc["SOFT_DIS_JTAG"] = esp_efuse_read_field_bit(ESP_EFUSE_SOFT_DIS_JTAG);
        doc["USB_EXCHG_PINS"] = esp_efuse_read_field_bit(ESP_EFUSE_USB_EXCHG_PINS);
        doc["USB_EXT_PHY_ENABLE"] = esp_efuse_read_field_bit(ESP_EFUSE_USB_EXT_PHY_ENABLE);
        doc["USB_PHY_SEL"] = esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
        doc["STRAP_JTAG_SEL"] = esp_efuse_read_field_bit(ESP_EFUSE_STRAP_JTAG_SEL);
        doc["VDD_SPI_XPD"] = esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_XPD);
        doc["VDD_SPI_TIEH"] = esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_TIEH);
        doc["VDD_SPI_FORCE"] = esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_FORCE);
        doc["DIS_DOWNLOAD_MODE"] = esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE);
        doc["DIS_USB"] = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB);
        doc["SECURE_BOOT_EN"] = esp_efuse_read_field_bit(ESP_EFUSE_SECURE_BOOT_EN);
        doc["DIS_DIRECT_BOOT"] = esp_efuse_read_field_bit(ESP_EFUSE_DIS_DIRECT_BOOT);

        uint8_t spi_crypt_cnt = 0;
        esp_efuse_read_field_blob(ESP_EFUSE_SPI_BOOT_CRYPT_CNT, &spi_crypt_cnt, 3);
        doc["SPI_BOOT_CRYPT_CNT"] = spi_crypt_cnt;

        uint8_t uart_print = 0;
        esp_efuse_read_field_blob(ESP_EFUSE_UART_PRINT_CONTROL, &uart_print, 2);
        doc["UART_PRINT_CONTROL"] = uart_print;

        doc["PIN_POWER_SELECTION"] = esp_efuse_read_field_bit(ESP_EFUSE_PIN_POWER_SELECTION);

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- Config save ---
    _server.on("/api/config/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    }, nullptr, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (index + len != total) return;
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) { request->send(400); return; }

        ProjectInfo* p = _config->getProjectInfo();
        if (!p) { request->send(500); return; }

        bool needsReboot = false;

        // WiFi
        if (doc.containsKey("wifi_ssid")) {
            String newSSID = doc["wifi_ssid"].as<String>();
            if (newSSID != _config->getWifiSSID()) needsReboot = true;
            _config->setWifiSSID(newSSID);
        }
        if (doc.containsKey("wifi_password") && doc["wifi_password"].as<String>().length() > 0) {
            _config->setWifiPassword(doc["wifi_password"].as<String>());
            needsReboot = true;
        }

        // MQTT
        if (doc.containsKey("mqtt_host")) {
            IPAddress ip;
            ip.fromString(doc["mqtt_host"].as<String>());
            _config->setMqttHost(ip);
            needsReboot = true;
        }
        if (doc.containsKey("mqtt_port")) { _config->setMqttPort(doc["mqtt_port"]); needsReboot = true; }
        if (doc.containsKey("mqtt_user")) { _config->setMqttUser(doc["mqtt_user"].as<String>()); needsReboot = true; }
        if (doc.containsKey("mqtt_password") && doc["mqtt_password"].as<String>().length() > 0) {
            _config->setMqttPassword(doc["mqtt_password"].as<String>());
            needsReboot = true;
        }
        if (doc.containsKey("mqtt_temp_topic")) { p->mqttTempTopic = doc["mqtt_temp_topic"].as<String>(); needsReboot = true; }

        // Thermostat timing
        if (doc.containsKey("min_on_ms")) { p->minOnTimeMs = doc["min_on_ms"]; _thermostat->config().minOnTimeMs = p->minOnTimeMs; }
        if (doc.containsKey("min_off_ms")) { p->minOffTimeMs = doc["min_off_ms"]; _thermostat->config().minOffTimeMs = p->minOffTimeMs; }
        if (doc.containsKey("max_run_ms")) { p->maxRunTimeMs = doc["max_run_ms"]; _thermostat->config().maxRunTimeMs = p->maxRunTimeMs; }
        if (doc.containsKey("escalation_ms")) { p->escalationDelayMs = doc["escalation_ms"]; _thermostat->config().escalationDelayMs = p->escalationDelayMs; }

        // Deadbands
        if (doc.containsKey("heat_deadband")) { p->heatDeadband = doc["heat_deadband"]; _thermostat->config().heatDeadband = p->heatDeadband; }
        if (doc.containsKey("cool_deadband")) { p->coolDeadband = doc["cool_deadband"]; _thermostat->config().coolDeadband = p->coolDeadband; }
        if (doc.containsKey("heat_overrun")) { p->heatOverrun = doc["heat_overrun"]; _thermostat->config().heatOverrun = p->heatOverrun; }
        if (doc.containsKey("cool_overrun")) { p->coolOverrun = doc["cool_overrun"]; _thermostat->config().coolOverrun = p->coolOverrun; }

        // Fan idle
        if (doc.containsKey("fan_idle_enabled")) { p->fanIdleEnabled = doc["fan_idle_enabled"]; _thermostat->config().fanIdleEnabled = p->fanIdleEnabled; }
        if (doc.containsKey("fan_idle_wait")) { p->fanIdleWaitMin = doc["fan_idle_wait"]; _thermostat->config().fanIdleWaitMin = p->fanIdleWaitMin; }
        if (doc.containsKey("fan_idle_run")) { p->fanIdleRunMin = doc["fan_idle_run"]; _thermostat->config().fanIdleRunMin = p->fanIdleRunMin; }

        // UI
        if (doc.containsKey("theme")) p->theme = doc["theme"].as<String>();
        if (doc.containsKey("poll_interval")) {
            p->pollIntervalSec = doc["poll_interval"];
            if (p->pollIntervalSec < 1) p->pollIntervalSec = 1;
            if (p->pollIntervalSec > 10) p->pollIntervalSec = 10;
        }

        // System
        if (doc.containsKey("system_name")) { p->systemName = doc["system_name"].as<String>(); }
        if (doc.containsKey("mqtt_prefix")) { p->mqttPrefix = doc["mqtt_prefix"].as<String>(); needsReboot = true; }
        if (doc.containsKey("timezone")) { p->timezone = doc["timezone"].as<String>(); setTimezone(p->timezone); syncNtpTime(); }
        if (doc.containsKey("ap_fallback_sec")) { p->apFallbackSeconds = doc["ap_fallback_sec"]; }
        if (doc.containsKey("ap_password") && doc["ap_password"].as<String>().length() >= 8) {
            p->apPassword = doc["ap_password"].as<String>();
        }
        if (doc.containsKey("session_timeout")) { p->sessionTimeoutMinutes = doc["session_timeout"]; _sessionMgr.setTimeoutMinutes(p->sessionTimeoutMinutes); }

        // Admin password
        if (doc.containsKey("admin_password") && doc["admin_password"].as<String>().length() >= 4) {
            _config->setAdminPassword(doc["admin_password"].as<String>());
        }

        // FTP password (live — takes effect on next FTP enable)
        if (doc["ftpPassword"].is<const char*>()) {
            p->ftpPassword = doc["ftpPassword"] | String("");
        }

        _config->updateConfig("/config.txt", *p);

        JsonDocument resp;
        resp["ok"] = true;
        resp["needsReboot"] = needsReboot;
        String response;
        serializeJson(resp, response);
        request->send(200, "application/json", response);
    });

    // --- Config load ---
    _server.on("/api/config/load", HTTP_GET, [this](AsyncWebServerRequest *request) {
        Serial.printf("config/load from %s\n", request->client()->remoteIP().toString().c_str());
        if (!checkAuth(request)) return;
        ProjectInfo* p = _config->getProjectInfo();
        if (!p) { Serial.println("config/load: no ProjectInfo!"); request->send(500); return; }

        JsonDocument doc;
        doc["wifi_ssid"] = _config->getWifiSSID();
        doc["mqtt_host"] = _config->getMqttHost().toString();
        doc["mqtt_port"] = _config->getMqttPort();
        doc["mqtt_user"] = _config->getMqttUser();
        doc["mqtt_temp_topic"] = p->mqttTempTopic;
        doc["system_name"] = p->systemName;
        doc["mqtt_prefix"] = p->mqttPrefix;
        doc["timezone"] = p->timezone;
        doc["theme"] = p->theme;
        doc["poll_interval"] = p->pollIntervalSec;
        doc["ap_fallback_sec"] = p->apFallbackSeconds;
        doc["session_timeout"] = p->sessionTimeoutMinutes;
        doc["has_password"] = _config->hasAdminPassword();
        doc["has_ap_password"] = p->apPassword.length() >= 8;

        // Thermostat timing
        doc["min_on_ms"] = p->minOnTimeMs;
        doc["min_off_ms"] = p->minOffTimeMs;
        doc["max_run_ms"] = p->maxRunTimeMs;
        doc["escalation_ms"] = p->escalationDelayMs;

        // Deadbands
        doc["heat_deadband"] = p->heatDeadband;
        doc["cool_deadband"] = p->coolDeadband;
        doc["heat_overrun"] = p->heatOverrun;
        doc["cool_overrun"] = p->coolOverrun;

        // Fan idle
        doc["fan_idle_enabled"] = p->fanIdleEnabled;
        doc["fan_idle_wait"] = p->fanIdleWaitMin;
        doc["fan_idle_run"] = p->fanIdleRunMin;

        // Log settings
        doc["max_log_size"] = p->maxLogSize;
        doc["max_old_log_count"] = p->maxOldLogCount;

        String response;
        serializeJson(doc, response);
        Serial.printf("config/load: %u bytes, ssid='%s'\n", response.length(), _config->getWifiSSID().c_str());
        request->send(200, "application/json", response);
    });

    // --- Reboot ---
    _server.on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        if (isRebootBlocked()) {
            request->send(429, "application/json", "{\"error\":\"Rate limited\"}");
            return;
        }
        request->send(200, "application/json", "{\"ok\":true}");
        _shouldReboot = true;
        _tDelayedReboot = new Task(500, TASK_ONCE, [this]() {
            ESP.restart();
        }, _ts, true);
    });

    // --- Firmware update ---
    _server.on("/update", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        String msg = _otaUploadOk ? "{\"ok\":true}" : "{\"error\":\"Upload failed\"}";
        AsyncWebServerResponse *response = request->beginResponse(_otaUploadOk ? 200 : 500, "application/json", msg);
        request->send(response);
        if (_otaUploadOk) {
            _shouldReboot = true;
            _tDelayedReboot = new Task(1000, TASK_ONCE, []() { ESP.restart(); }, _ts, true);
        }
    }, [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!checkAuth(request)) return;
        if (index == 0) {
            Log.info("OTA", "Upload start: %s", filename.c_str());
            backupFirmwareToFS("/firmware.bak", compile_date);
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Log.error("OTA", "Update.begin failed");
                Update.printError(Serial);
                _otaUploadOk = false;
                return;
            }
            _otaUploadOk = true;
        }
        if (_otaUploadOk && len > 0) {
            if (Update.write(data, len) != len) {
                Log.error("OTA", "Update.write failed");
                Update.printError(Serial);
                _otaUploadOk = false;
            }
        }
        if (final) {
            if (_otaUploadOk && Update.end(true)) {
                Log.info("OTA", "Upload complete: %u bytes", index + len);
            } else {
                Log.error("OTA", "Upload finalize failed");
                _otaUploadOk = false;
            }
        }
    });

    // --- Firmware info ---
    _server.on("/update/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        JsonDocument doc;
        doc["currentBuild"] = compile_date;
        doc["hasBackup"] = firmwareBackupExists();
        doc["backupSize"] = firmwareBackupSize();
        doc["backupBuild"] = getBackupBuildDate();
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- Firmware revert ---
    _server.on("/update/revert", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        if (!firmwareBackupExists()) {
            request->send(404, "application/json", "{\"error\":\"No backup\"}");
            return;
        }
        bool ok = revertFirmwareFromFS();
        if (ok) {
            request->send(200, "application/json", "{\"ok\":true}");
            _shouldReboot = true;
            _tDelayedReboot = new Task(1000, TASK_ONCE, []() { ESP.restart(); }, _ts, true);
        } else {
            request->send(500, "application/json", "{\"error\":\"Revert failed\"}");
        }
    });

    // --- FS info ---
    _server.on("/fs/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        request->send(200, "application/json", _config->getFSInfo());
    });

    // --- WiFi scan ---
    _server.on("/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);
            request->send(200, "application/json", "{\"scanning\":true}");
            return;
        }
        if (n == WIFI_SCAN_RUNNING) {
            request->send(200, "application/json", "{\"scanning\":true}");
            return;
        }
        JsonDocument doc;
        JsonArray networks = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject net = networks.add<JsonObject>();
            net["ssid"] = WiFi.SSID(i);
            net["rssi"] = WiFi.RSSI(i);
            net["encrypted"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        doc["scanning"] = false;
        WiFi.scanDelete();
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // --- FTP control endpoints ---
    _server.on("/ftp", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!checkAuth(request)) return;
        String json = _ftpStatusCb ? _ftpStatusCb() : "{\"active\":false}";
        request->send(200, "application/json", json);
    });

    auto* ftpPostHandler = new AsyncCallbackJsonWebHandler("/ftp", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!checkAuth(request)) return;
        JsonObject data = json.as<JsonObject>();
        int duration = data["duration"] | 0;
        if (duration > 0 && _ftpEnableCb) {
            _ftpEnableCb(duration);
            request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"FTP enabled\"}");
        } else if (_ftpDisableCb) {
            _ftpDisableCb();
            request->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"FTP disabled\"}");
        } else {
            request->send(500, "application/json", "{\"error\":\"FTP control not available\"}");
        }
    });
    _server.addHandler(ftpPostHandler);
}
