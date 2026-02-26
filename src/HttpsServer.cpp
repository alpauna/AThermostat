// Separate compilation unit for ESP-IDF HTTPS server.
// esp_https_server.h and ESPAsyncWebServer.h define conflicting HTTP method
// enums (HTTP_PUT, HTTP_OPTIONS, HTTP_PATCH) and cannot coexist in the same TU.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_https_server.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <TaskSchedulerDeclarations.h>
#include <LittleFS.h>
#include "mbedtls/base64.h"
#include <Wire.h>
#include "HttpsServer.h"
#include "OtaUtils.h"
#include "Config.h"
#include "Thermostat.h"
#include "HX710.h"
#include "Logger.h"
#include "SessionManager.h"
#include <lwip/sockets.h>

extern uint8_t getCpuLoadCore0();
extern uint8_t getCpuLoadCore1();
extern bool _apModeActive;
extern const char compile_date[];

// Get client IP from esp_http_server request via socket fd
// Uses sockaddr_in6 to handle both IPv4 and IPv4-mapped IPv6 (::ffff:x.x.x.x)
static String getClientIP(httpd_req_t* req) {
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) return "unknown";
    struct sockaddr_in6 addr;
    socklen_t addrLen = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr*)&addr, &addrLen) != 0) return "unknown";
    char ip[INET6_ADDRSTRLEN];
    if (addr.sin6_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, ip, sizeof(ip));
    } else {
        inet_ntop(AF_INET6, &addr.sin6_addr, ip, sizeof(ip));
        // Strip ::ffff: prefix for IPv4-mapped addresses
        if (strncmp(ip, "::ffff:", 7) == 0) return String(ip + 7);
    }
    return String(ip);
}

// Get User-Agent header from esp_http_server request
static String getUserAgent(httpd_req_t* req) {
    size_t len = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (len == 0) return "unknown";
    char* buf = new char[len + 1];
    if (httpd_req_get_hdr_value_str(req, "User-Agent", buf, len + 1) != ESP_OK) {
        delete[] buf;
        return "unknown";
    }
    String ua(buf);
    delete[] buf;
    return ua;
}

// --- HTTPS Auth helpers ---

static void sendUnauthorized(httpd_req_t* req, HttpsContext* ctx) {
    String realm = "Basic realm=\"" + (ctx->systemName.length() > 0 ? ctx->systemName : String("AThermostat")) + "\"";
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", realm.c_str());
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
}

static void redirectToLoginHttps(httpd_req_t* req, bool expired) {
    // Build redirect URL from the request URI
    size_t uriLen = strlen(req->uri);
    String url = "/login?redirect=" + String(req->uri);
    if (expired) url += "&expired=1";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url.c_str());
    httpd_resp_send(req, NULL, 0);
}

static bool checkHttpsAuth(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    if (!ctx->config || !ctx->config->hasAdminPassword()) return true;

    // Session mode: check cookie first
    if (ctx->sessionMgr && ctx->sessionMgr->isEnabled()) {
        size_t cookieLen = httpd_req_get_hdr_value_len(req, "Cookie");
        if (cookieLen > 0) {
            char* cookieBuf = (char*)malloc(cookieLen + 1);
            if (cookieBuf) {
                httpd_req_get_hdr_value_str(req, "Cookie", cookieBuf, cookieLen + 1);
                String token = SessionManager::extractSessionToken(String(cookieBuf));
                free(cookieBuf);
                if (token.length() > 0) {
                    if (ctx->sessionMgr->validateSession(token))
                        return true;
                    // Expired session — fall through to Basic Auth if header present
                }
            }
        }
        // No valid session cookie — allow Basic Auth fallback for API/script access
        size_t authCheck = httpd_req_get_hdr_value_len(req, "Authorization");
        if (authCheck == 0) {
            redirectToLoginHttps(req, false);
            return false;
        }
    }

    // Basic Auth (legacy mode or session mode fallback for scripts/API clients)
    size_t authLen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (authLen == 0) {
        sendUnauthorized(req, ctx);
        return false;
    }

    char* authBuf = (char*)malloc(authLen + 1);
    if (!authBuf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return false;
    }
    httpd_req_get_hdr_value_str(req, "Authorization", authBuf, authLen + 1);

    // Expect "Basic <base64>"
    if (strncmp(authBuf, "Basic ", 6) != 0) {
        free(authBuf);
        sendUnauthorized(req, ctx);
        return false;
    }

    const char* b64 = authBuf + 6;
    size_t b64Len = strlen(b64);
    size_t decodedLen = 0;
    mbedtls_base64_decode(nullptr, 0, &decodedLen, (const uint8_t*)b64, b64Len);
    uint8_t* decoded = (uint8_t*)malloc(decodedLen + 1);
    if (!decoded) { free(authBuf); return false; }
    mbedtls_base64_decode(decoded, decodedLen + 1, &decodedLen, (const uint8_t*)b64, b64Len);
    decoded[decodedLen] = '\0';
    free(authBuf);

    // Split at ':'
    char* colon = strchr((char*)decoded, ':');
    if (!colon) {
        free(decoded);
        sendUnauthorized(req, ctx);
        return false;
    }
    String password = String(colon + 1);
    free(decoded);

    if (ctx->config->verifyAdminPassword(password)) {
        return true;
    }

    sendUnauthorized(req, ctx);
    return false;
}

// --- LittleFS file serving helper ---

static esp_err_t serveFileHttps(httpd_req_t* req, const char* fsPath) {
    fs::File file = LittleFS.open(fsPath, FILE_READ);
    if (!file) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    size_t fileSize = file.size();
    char* buf = (char*)ps_malloc(fileSize + 1);
    if (!buf) {
        file.close();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    file.read((uint8_t*)buf, fileSize);
    buf[fileSize] = '\0';
    file.close();

    const char* contentType = "text/html";
    if (strstr(fsPath, ".css")) contentType = "text/css";
    else if (strstr(fsPath, ".js")) contentType = "application/javascript";
    else if (strstr(fsPath, ".json")) contentType = "application/json";

    httpd_resp_set_type(req, contentType);
    httpd_resp_send(req, buf, fileSize);
    free(buf);
    return ESP_OK;
}

// --- ESP-IDF httpd handler callbacks ---

static esp_err_t configGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    // Gate: redirect to admin setup if no admin password set
    if (ctx->config && !ctx->config->hasAdminPassword()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/admin/setup");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (!checkHttpsAuth(req)) return ESP_OK;

    // Check for ?format=json
    size_t qLen = httpd_req_get_url_query_len(req);
    bool wantJson = false;
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "format", val, sizeof(val)) == ESP_OK) {
                wantJson = (strcmp(val, "json") == 0);
            }
        }
        free(qBuf);
    }

    if (wantJson) {
        if (!ctx->config || !ctx->config->getProjectInfo()) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Config not available\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        ProjectInfo* proj = ctx->config->getProjectInfo();
        JsonDocument doc;
        doc["wifiSSID"] = ctx->config->getWifiSSID();
        doc["wifiPassword"] = "******";
        doc["mqttHost"] = ctx->config->getMqttHost().toString();
        doc["mqttPort"] = ctx->config->getMqttPort();
        doc["mqttUser"] = ctx->config->getMqttUser();
        doc["mqttPassword"] = "******";
        doc["timezone"] = proj->timezone;
        doc["heatSetpoint"] = proj->heatSetpoint;
        doc["coolSetpoint"] = proj->coolSetpoint;
        doc["thermostatMode"] = proj->thermostatMode;
        doc["forceFurnace"] = proj->forceFurnace;
        doc["forceNoHP"] = proj->forceNoHP;
        doc["minOnTimeSec"] = proj->minOnTimeMs / 1000;
        doc["minOffTimeSec"] = proj->minOffTimeMs / 1000;
        doc["minIdleTimeSec"] = proj->minIdleTimeMs / 1000;
        doc["maxRunTimeSec"] = proj->maxRunTimeMs / 1000;
        doc["escalationDelaySec"] = proj->escalationDelayMs / 1000;
        doc["heatDeadband"] = proj->heatDeadband;
        doc["coolDeadband"] = proj->coolDeadband;
        doc["heatOverrun"] = proj->heatOverrun;
        doc["coolOverrun"] = proj->coolOverrun;
        doc["fanIdleEnabled"] = proj->fanIdleEnabled;
        doc["fanIdleWaitMin"] = proj->fanIdleWaitMin;
        doc["fanIdleRunMin"] = proj->fanIdleRunMin;
        doc["hx710_1_raw1"] = proj->hx710_1_raw1;
        doc["hx710_1_raw2"] = proj->hx710_1_raw2;
        doc["hx710_1_val1"] = proj->hx710_1_val1;
        doc["hx710_1_val2"] = proj->hx710_1_val2;
        doc["hx710_2_raw1"] = proj->hx710_2_raw1;
        doc["hx710_2_raw2"] = proj->hx710_2_raw2;
        doc["hx710_2_val1"] = proj->hx710_2_val1;
        doc["hx710_2_val2"] = proj->hx710_2_val2;
        doc["apFallbackMinutes"] = proj->apFallbackSeconds / 60;
        doc["apPassword"] = proj->apPassword;
        doc["maxLogSize"] = proj->maxLogSize;
        doc["maxOldLogCount"] = proj->maxOldLogCount;
        doc["adminPasswordSet"] = ctx->config->hasAdminPassword();
        doc["theme"] = proj->theme.length() > 0 ? proj->theme : "dark";
        doc["systemName"] = proj->systemName.length() > 0 ? proj->systemName : "AThermostat";
        doc["mqttPrefix"] = proj->mqttPrefix.length() > 0 ? proj->mqttPrefix : "thermostat";
        doc["mqttTempTopic"] = proj->mqttTempTopic;
        doc["forceSafeMode"] = proj->forceSafeMode;
        doc["safeMode"] = ctx->safeMode ? *(ctx->safeMode) : false;
        doc["sessionTimeoutMinutes"] = proj->sessionTimeoutMinutes;
        doc["pollIntervalSec"] = proj->pollIntervalSec;
        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    return serveFileHttps(req, "/www/config.html");
}

static esp_err_t configPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 4096) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) {
            free(body);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Receive failed\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument data;
    DeserializationError err = deserializeJson(data, body);
    free(body);
    if (err) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!ctx->config || !ctx->config->getProjectInfo()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Config not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ProjectInfo* proj = ctx->config->getProjectInfo();
    bool needsReboot = false;
    String errors = "";

    // WiFi SSID
    String newSSID = data["wifiSSID"] | ctx->config->getWifiSSID();
    if (newSSID != ctx->config->getWifiSSID()) {
        ctx->config->setWifiSSID(newSSID);
        needsReboot = true;
    }

    // WiFi password
    String wifiPw = data["wifiPassword"] | String("******");
    if (wifiPw != "******" && wifiPw.length() > 0) {
        String curPw = data["curWifiPw"] | String("");
        if (curPw == ctx->config->getWifiPassword() || ctx->config->verifyAdminPassword(curPw)) {
            ctx->config->setWifiPassword(wifiPw);
            needsReboot = true;
        } else {
            errors += "WiFi password: current password incorrect. ";
        }
    }

    // MQTT host
    String mqttHost = data["mqttHost"] | ctx->config->getMqttHost().toString();
    IPAddress newMqttHost;
    newMqttHost.fromString(mqttHost);
    if (newMqttHost != ctx->config->getMqttHost()) {
        ctx->config->setMqttHost(newMqttHost);
        needsReboot = true;
    }

    uint16_t mqttPort = data["mqttPort"] | ctx->config->getMqttPort();
    if (mqttPort != ctx->config->getMqttPort()) {
        ctx->config->setMqttPort(mqttPort);
        needsReboot = true;
    }

    String mqttUser = data["mqttUser"] | ctx->config->getMqttUser();
    if (mqttUser != ctx->config->getMqttUser()) {
        ctx->config->setMqttUser(mqttUser);
        needsReboot = true;
    }

    // MQTT password
    String mqttPw = data["mqttPassword"] | String("******");
    if (mqttPw != "******" && mqttPw.length() > 0) {
        String curPw = data["curMqttPw"] | String("");
        if (curPw == ctx->config->getMqttPassword() || ctx->config->verifyAdminPassword(curPw)) {
            ctx->config->setMqttPassword(mqttPw);
            needsReboot = true;
        } else {
            errors += "MQTT password: current password incorrect. ";
        }
    }

    // Admin password
    String adminPw = data["adminPassword"] | String("");
    if (adminPw.length() > 0) {
        if (!ctx->config->hasAdminPassword()) {
            ctx->config->setAdminPassword(adminPw);
            Log.info("AUTH", "Admin password set for first time (HTTPS)");
        } else {
            String curAdminPw = data["curAdminPw"] | String("");
            if (ctx->config->verifyAdminPassword(curAdminPw)) {
                ctx->config->setAdminPassword(adminPw);
                Log.info("AUTH", "Admin password changed (HTTPS)");
            } else {
                errors += "Admin password: current password incorrect. ";
            }
        }
    }

    // Session timeout (live)
    if (data["sessionTimeoutMinutes"].is<int>()) {
        uint32_t stm = data["sessionTimeoutMinutes"] | 0;
        proj->sessionTimeoutMinutes = stm;
        if (ctx->sessionMgr) ctx->sessionMgr->setTimeoutMinutes(stm);
    }

    // Poll interval (live)
    if (data["pollIntervalSec"].is<int>()) {
        uint8_t pi = data["pollIntervalSec"] | 2;
        if (pi < 1) pi = 1;
        if (pi > 10) pi = 10;
        proj->pollIntervalSec = pi;
    }

    // Timezone (live)
    String tz = data["timezone"] | proj->timezone;
    if (tz != proj->timezone) {
        proj->timezone = tz;
        *(ctx->timezone) = tz;
        configTzTime(tz.c_str(), "192.168.0.1", "time.nist.gov");
    }

    // Thermostat set points (live)
    float heatSP = data["heatSetpoint"] | proj->heatSetpoint;
    if (heatSP != proj->heatSetpoint) {
        proj->heatSetpoint = heatSP;
        ctx->thermostat->setHeatSetpoint(heatSP);
    }

    float coolSP = data["coolSetpoint"] | proj->coolSetpoint;
    if (coolSP != proj->coolSetpoint) {
        proj->coolSetpoint = coolSP;
        ctx->thermostat->setCoolSetpoint(coolSP);
    }

    // Thermostat mode (live)
    if (data["thermostatMode"].is<int>()) {
        uint8_t mode = data["thermostatMode"] | proj->thermostatMode;
        proj->thermostatMode = mode;
        ctx->thermostat->setMode((ThermostatMode)mode);
    }

    // Force flags (live)
    if (data["forceFurnace"].is<bool>()) {
        bool ff = data["forceFurnace"];
        proj->forceFurnace = ff;
        ctx->thermostat->setForceFurnace(ff);
    }
    if (data["forceNoHP"].is<bool>()) {
        bool fnh = data["forceNoHP"];
        proj->forceNoHP = fnh;
        ctx->thermostat->setForceNoHP(fnh);
    }

    // Thermostat timing (live)
    if (data["minOnTimeSec"].is<int>()) {
        uint32_t val = (data["minOnTimeSec"] | (int)(proj->minOnTimeMs / 1000)) * 1000UL;
        proj->minOnTimeMs = val;
        ctx->thermostat->config().minOnTimeMs = val;
    }
    if (data["minOffTimeSec"].is<int>()) {
        uint32_t val = (data["minOffTimeSec"] | (int)(proj->minOffTimeMs / 1000)) * 1000UL;
        proj->minOffTimeMs = val;
        ctx->thermostat->config().minOffTimeMs = val;
    }
    if (data["minIdleTimeSec"].is<int>()) {
        uint32_t val = (data["minIdleTimeSec"] | (int)(proj->minIdleTimeMs / 1000)) * 1000UL;
        proj->minIdleTimeMs = val;
        ctx->thermostat->config().minIdleTimeMs = val;
    }
    if (data["maxRunTimeSec"].is<int>()) {
        uint32_t val = (data["maxRunTimeSec"] | (int)(proj->maxRunTimeMs / 1000)) * 1000UL;
        proj->maxRunTimeMs = val;
        ctx->thermostat->config().maxRunTimeMs = val;
    }
    if (data["escalationDelaySec"].is<int>()) {
        uint32_t val = (data["escalationDelaySec"] | (int)(proj->escalationDelayMs / 1000)) * 1000UL;
        proj->escalationDelayMs = val;
        ctx->thermostat->config().escalationDelayMs = val;
    }

    // Temperature deadbands (live)
    if (data["heatDeadband"].is<float>()) {
        float v = data["heatDeadband"] | proj->heatDeadband;
        proj->heatDeadband = v;
        ctx->thermostat->config().heatDeadband = v;
    }
    if (data["coolDeadband"].is<float>()) {
        float v = data["coolDeadband"] | proj->coolDeadband;
        proj->coolDeadband = v;
        ctx->thermostat->config().coolDeadband = v;
    }
    if (data["heatOverrun"].is<float>()) {
        float v = data["heatOverrun"] | proj->heatOverrun;
        proj->heatOverrun = v;
        ctx->thermostat->config().heatOverrun = v;
    }
    if (data["coolOverrun"].is<float>()) {
        float v = data["coolOverrun"] | proj->coolOverrun;
        proj->coolOverrun = v;
        ctx->thermostat->config().coolOverrun = v;
    }

    // Fan idle (live)
    if (data["fanIdleEnabled"].is<bool>()) {
        bool v = data["fanIdleEnabled"];
        proj->fanIdleEnabled = v;
        ctx->thermostat->config().fanIdleEnabled = v;
    }
    if (data["fanIdleWaitMin"].is<int>()) {
        uint32_t v = data["fanIdleWaitMin"] | proj->fanIdleWaitMin;
        proj->fanIdleWaitMin = v;
        ctx->thermostat->config().fanIdleWaitMin = v;
    }
    if (data["fanIdleRunMin"].is<int>()) {
        uint32_t v = data["fanIdleRunMin"] | proj->fanIdleRunMin;
        proj->fanIdleRunMin = v;
        ctx->thermostat->config().fanIdleRunMin = v;
    }

    // HX710 calibration (live)
    if (data["hx710_1_raw1"].is<int>()) proj->hx710_1_raw1 = data["hx710_1_raw1"];
    if (data["hx710_1_raw2"].is<int>()) proj->hx710_1_raw2 = data["hx710_1_raw2"];
    if (data["hx710_1_val1"].is<float>()) proj->hx710_1_val1 = data["hx710_1_val1"];
    if (data["hx710_1_val2"].is<float>()) proj->hx710_1_val2 = data["hx710_1_val2"];
    if (data["hx710_2_raw1"].is<int>()) proj->hx710_2_raw1 = data["hx710_2_raw1"];
    if (data["hx710_2_raw2"].is<int>()) proj->hx710_2_raw2 = data["hx710_2_raw2"];
    if (data["hx710_2_val1"].is<float>()) proj->hx710_2_val1 = data["hx710_2_val1"];
    if (data["hx710_2_val2"].is<float>()) proj->hx710_2_val2 = data["hx710_2_val2"];
    // Apply calibration to HX710 sensors if present
    if (ctx->pressure1) {
        ctx->pressure1->setCalibration(proj->hx710_1_raw1, proj->hx710_1_val1,
                                       proj->hx710_1_raw2, proj->hx710_1_val2);
    }
    if (ctx->pressure2) {
        ctx->pressure2->setCalibration(proj->hx710_2_raw1, proj->hx710_2_val1,
                                       proj->hx710_2_raw2, proj->hx710_2_val2);
    }

    // AP fallback timeout (live)
    uint32_t apMinutes = data["apFallbackMinutes"] | (proj->apFallbackSeconds / 60);
    proj->apFallbackSeconds = apMinutes * 60;

    // AP password
    if (data["apPassword"].is<const char*>()) {
        proj->apPassword = data["apPassword"] | String("");
    }

    // Logging (live)
    uint32_t maxLogSize = data["maxLogSize"] | proj->maxLogSize;
    uint8_t maxOldLogCount = data["maxOldLogCount"] | proj->maxOldLogCount;
    proj->maxLogSize = maxLogSize;
    proj->maxOldLogCount = maxOldLogCount;

    // UI theme
    String theme = data["theme"] | proj->theme;
    if (theme == "dark" || theme == "light") {
        proj->theme = theme;
    }

    // MQTT temp topic (requires reboot)
    if (data["mqttTempTopic"].is<const char*>()) {
        String newTopic = data["mqttTempTopic"] | proj->mqttTempTopic;
        if (newTopic != proj->mqttTempTopic) {
            proj->mqttTempTopic = newTopic;
            needsReboot = true;
        }
    }

    // System name (requires reboot)
    if (data["systemName"].is<const char*>()) {
        String newName = data["systemName"] | proj->systemName;
        // Strip non-alphanumeric/space chars, clamp to 20
        String cleaned;
        for (size_t i = 0; i < newName.length() && cleaned.length() < 20; i++) {
            char c = newName[i];
            if (isalnum(c) || c == ' ') cleaned += c;
        }
        if (cleaned.length() > 0 && cleaned != proj->systemName) {
            proj->systemName = cleaned;
            needsReboot = true;
        }
    }

    // MQTT prefix (requires reboot)
    if (data["mqttPrefix"].is<const char*>()) {
        String newPrefix = data["mqttPrefix"] | proj->mqttPrefix;
        if (newPrefix.length() > 0 && newPrefix != proj->mqttPrefix) {
            proj->mqttPrefix = newPrefix;
            needsReboot = true;
        }
    }

    // Force safe mode on next boot (one-shot)
    if (data["forceSafeMode"].is<bool>()) {
        proj->forceSafeMode = data["forceSafeMode"] | false;
        if (proj->forceSafeMode) needsReboot = true;
    }

    // Save to LittleFS
    bool saved = ctx->config->updateConfig("/config.txt", *proj);

    JsonDocument respDoc;
    if (!saved) {
        respDoc["error"] = "Failed to save config to filesystem";
        if (errors.length() > 0) respDoc["error"] = errors + "Also failed to save.";
    } else if (errors.length() > 0) {
        respDoc["error"] = errors + "Other settings saved.";
    } else if (needsReboot) {
        respDoc["message"] = "Settings saved. Rebooting in 2 seconds...";
        respDoc["reboot"] = true;
    } else {
        respDoc["message"] = "Settings saved and applied.";
    }
    String response;
    serializeJson(respDoc, response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());

    if (needsReboot && saved && errors.length() == 0) {
        Log.info("CONFIG", "Config changed via HTTPS, rebooting in 2s...");
        if (!*(ctx->delayedReboot)) {
            *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
                *(ctx->shouldReboot) = true;
            }, ctx->scheduler, false);
        }
        (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    }
    return ESP_OK;
}

static esp_err_t updateGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    return serveFileHttps(req, "/www/update.html");
}

static esp_err_t updatePostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;

    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send(req, "FAIL: no data", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    File fw = LittleFS.open("/firmware.new", FILE_WRITE);
    if (!fw) {
        httpd_resp_send(req, "FAIL: LittleFS open error", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    Log.info("OTA", "Saving firmware to LittleFS (%d bytes)", remaining);

    char buf[1024];
    while (remaining > 0) {
        int toRead = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int ret = httpd_req_recv(req, buf, toRead);
        if (ret <= 0) {
            fw.close();
            LittleFS.remove("/firmware.new");
            httpd_resp_send(req, "FAIL: receive error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (fw.write((uint8_t*)buf, ret) != (size_t)ret) {
            fw.close();
            LittleFS.remove("/firmware.new");
            httpd_resp_send(req, "FAIL: LittleFS write error", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        remaining -= ret;
    }

    fw.close();
    Log.info("OTA", "Firmware saved to LittleFS");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t applyGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    bool exists = firmwareBackupExists("/firmware.new");
    size_t size = exists ? firmwareBackupSize("/firmware.new") : 0;
    String json = "{\"exists\":" + String(exists ? "true" : "false") +
                  ",\"size\":" + String(size) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t applyPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!firmwareBackupExists("/firmware.new")) {
        httpd_resp_send(req, "FAIL: no firmware uploaded", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (applyFirmwareFromFS("/firmware.new", compile_date)) {
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        if (!*(ctx->delayedReboot)) {
            *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
                *(ctx->shouldReboot) = true;
            }, ctx->scheduler, false);
        }
        (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    } else {
        httpd_resp_send(req, "FAIL", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// --- AP mode handlers ---

static esp_err_t apTestHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    httpd_resp_set_type(req, "application/json");
    if (ctx->apStartCb) {
        String password = ctx->apStartCb();
        String ssid = ctx->systemName.length() > 0 ? ctx->systemName : "AThermostat";
        String json = "{\"ssid\":\"" + ssid + "\",\"password\":\"" + password + "\",\"ip\":\"192.168.4.1\"}";
        httpd_resp_send(req, json.c_str(), json.length());
    } else {
        httpd_resp_send(req, "{\"error\":\"AP control not available\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t apStopHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    httpd_resp_set_type(req, "application/json");
    if (ctx->apStopCb) {
        ctx->apStopCb();
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"AP mode stopped\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"error\":\"AP control not available\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// --- Heap handler ---

static esp_err_t heapGetHandler(httpd_req_t* req) {
    static constexpr float MB = 1.0f / (1024.0f * 1024.0f);
    String json = "{";
    json += "\"free heap\":" + String(ESP.getFreeHeap());
    json += ",\"free psram MB\":" + String(ESP.getFreePsram() * MB);
    json += ",\"used psram MB\":" + String((ESP.getPsramSize() - ESP.getFreePsram()) * MB);
    json += ",\"cpuLoad0\":" + String(getCpuLoadCore0());
    json += ",\"cpuLoad1\":" + String(getCpuLoadCore1());
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- State handler ---

static esp_err_t stateGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    JsonDocument doc;

    Thermostat* ts = ctx->thermostat;

    doc["mode"] = Thermostat::modeToString(ts->getMode());
    doc["action"] = Thermostat::actionToString(ts->getAction());
    doc["heatLevel"] = Thermostat::heatLevelToString(ts->getHeatLevel());
    doc["coolLevel"] = Thermostat::coolLevelToString(ts->getCoolLevel());
    doc["heatSetpoint"] = ts->getHeatSetpoint();
    doc["coolSetpoint"] = ts->getCoolSetpoint();
    doc["currentTemp"] = ts->getCurrentTemperature();
    doc["tempValid"] = ts->hasValidTemperature();
    doc["forceFurnace"] = ts->isForceFurnace();
    doc["forceNoHP"] = ts->isForceNoHP();
    doc["defrostActive"] = ts->isDefrostActive();

    // Output pin states
    JsonObject outputs = doc["outputs"].to<JsonObject>();
    static const char* outNames[] = {"fan1","rev","furn_cool_low","furn_cool_high","w1","w2","comp1","comp2"};
    for (int i = 0; i < OUT_COUNT; i++) {
        OutPin* p = ts->getOutput((OutputIdx)i);
        if (p) outputs[outNames[i]] = p->isPinOn();
    }

    // Input pin states
    JsonObject inputs = doc["inputs"].to<JsonObject>();
    static const char* inNames[] = {"out_temp_ok","defrost_mode"};
    for (int i = 0; i < IN_COUNT; i++) {
        InputPin* p = ts->getInput((InputIdx)i);
        if (p) inputs[inNames[i]] = p->isActive();
    }

    // Pressure sensors
    if (ctx->pressure1 && ctx->pressure1->isValid()) {
        doc["pressure1"] = ctx->pressure1->getLastValue();
        doc["pressure1Raw"] = ctx->pressure1->getLastRaw();
    }
    if (ctx->pressure2 && ctx->pressure2->isValid()) {
        doc["pressure2"] = ctx->pressure2->getLastValue();
        doc["pressure2Raw"] = ctx->pressure2->getLastRaw();
    }

    doc["cpuLoad0"] = getCpuLoadCore0();
    doc["cpuLoad1"] = getCpuLoadCore1();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["wifiSSID"] = WiFi.SSID();
    doc["wifiRSSI"] = WiFi.RSSI();
    doc["wifiIP"] = WiFi.localIP().toString();
    doc["apMode"] = _apModeActive;
    doc["buildDate"] = compile_date;
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        char buf[20];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        doc["datetime"] = buf;
    }

    String json;
    serializeJson(doc, json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Pins handler ---

static esp_err_t pinsGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    // Check for ?format=json
    size_t qLen = httpd_req_get_url_query_len(req);
    bool wantJson = false;
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "format", val, sizeof(val)) == ESP_OK) {
                wantJson = (strcmp(val, "json") == 0);
            }
        }
        free(qBuf);
    }

    if (wantJson) {
        JsonDocument doc;
        Thermostat* ts = ctx->thermostat;

        doc["mode"] = Thermostat::modeToString(ts->getMode());
        doc["action"] = Thermostat::actionToString(ts->getAction());
        doc["heatLevel"] = Thermostat::heatLevelToString(ts->getHeatLevel());
        doc["coolLevel"] = Thermostat::coolLevelToString(ts->getCoolLevel());
        doc["defrostActive"] = ts->isDefrostActive();

        static const char* outNames[] = {"fan1","rev","furn_cool_low","furn_cool_high","w1","w2","comp1","comp2"};
        JsonArray outputsArr = doc["outputs"].to<JsonArray>();
        for (int i = 0; i < OUT_COUNT; i++) {
            OutPin* p = ts->getOutput((OutputIdx)i);
            if (p) {
                JsonObject out = outputsArr.add<JsonObject>();
                out["pin"] = p->getPin();
                out["name"] = outNames[i];
                out["on"] = p->isPinOn();
            }
        }

        static const char* inNames[] = {"out_temp_ok","defrost_mode"};
        JsonArray inputsArr = doc["inputs"].to<JsonArray>();
        for (int i = 0; i < IN_COUNT; i++) {
            InputPin* p = ts->getInput((InputIdx)i);
            if (p) {
                JsonObject inp = inputsArr.add<JsonObject>();
                inp["pin"] = p->getPin();
                inp["name"] = inNames[i];
                inp["active"] = p->isActive();
            }
        }

        // Pressure sensors
        if (ctx->pressure1) {
            JsonObject p1 = doc["pressure1"].to<JsonObject>();
            p1["valid"] = ctx->pressure1->isValid();
            p1["value"] = ctx->pressure1->getLastValue();
            p1["raw"] = ctx->pressure1->getLastRaw();
        }
        if (ctx->pressure2) {
            JsonObject p2 = doc["pressure2"].to<JsonObject>();
            p2["valid"] = ctx->pressure2->isValid();
            p2["value"] = ctx->pressure2->getLastValue();
            p2["raw"] = ctx->pressure2->getLastRaw();
        }

        String json;
        serializeJson(doc, json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    return serveFileHttps(req, "/www/pins.html");
}

static esp_err_t pinsPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument data;
    if (deserializeJson(data, body)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    free(body);

    httpd_resp_set_type(req, "application/json");

    // Set thermostat mode
    if (data["mode"].is<const char*>()) {
        String modeStr = data["mode"] | String("OFF");
        ThermostatMode mode = Thermostat::stringToMode(modeStr.c_str());
        ctx->thermostat->setMode(mode);
        JsonDocument resp;
        resp["status"] = "ok";
        resp["mode"] = Thermostat::modeToString(ctx->thermostat->getMode());
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    // Set heat setpoint
    if (data["heatSetpoint"].is<float>()) {
        float sp = data["heatSetpoint"];
        ctx->thermostat->setHeatSetpoint(sp);
        JsonDocument resp;
        resp["status"] = "ok";
        resp["heatSetpoint"] = sp;
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    // Set cool setpoint
    if (data["coolSetpoint"].is<float>()) {
        float sp = data["coolSetpoint"];
        ctx->thermostat->setCoolSetpoint(sp);
        JsonDocument resp;
        resp["status"] = "ok";
        resp["coolSetpoint"] = sp;
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    // Force furnace
    if (data["forceFurnace"].is<bool>()) {
        bool ff = data["forceFurnace"];
        ctx->thermostat->setForceFurnace(ff);
        JsonDocument resp;
        resp["status"] = "ok";
        resp["forceFurnace"] = ff;
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    // Force no HP
    if (data["forceNoHP"].is<bool>()) {
        bool fnh = data["forceNoHP"];
        ctx->thermostat->setForceNoHP(fnh);
        JsonDocument resp;
        resp["status"] = "ok";
        resp["forceNoHP"] = fnh;
        String json;
        serializeJson(resp, json);
        httpd_resp_send(req, json.c_str(), json.length());
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"error\":\"Invalid request\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- Dashboard handler ---

static esp_err_t dashboardGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/dashboard.html");
}

static esp_err_t logViewGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/log.html");
}

static esp_err_t heapViewGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/heap.html");
}

static esp_err_t wifiViewGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/wifi.html");
}

static esp_err_t wifiStatusGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    String json = "{\"status\":\"" + *ctx->wifiTestState + "\"";
    if (ctx->wifiTestMessage->length() > 0) {
        json += ",\"message\":\"" + *ctx->wifiTestMessage + "\"";
    }
    json += "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

static esp_err_t wifiTestPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (*ctx->wifiTestState == "testing") {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Test already in progress\"}");
        return ESP_OK;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid body\"}");
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Out of memory\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }
    free(body);

    String ssid = doc["ssid"] | String("");
    String password = doc["password"] | String("");
    String curPassword = doc["curPassword"] | String("");

    if (ssid.length() == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"SSID required\"}");
        return ESP_OK;
    }

    bool verified = false;
    if (ctx->config->getWifiPassword().length() > 0) {
        verified = (curPassword == ctx->config->getWifiPassword());
    } else if (ctx->config->hasAdminPassword()) {
        verified = ctx->config->verifyAdminPassword(curPassword);
    } else {
        verified = true;
    }
    if (!verified) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Current password incorrect\"}");
        return ESP_OK;
    }

    // Store old credentials and start test
    *ctx->wifiOldSSID = ctx->config->getWifiSSID();
    *ctx->wifiOldPassword = ctx->config->getWifiPassword();
    *ctx->wifiTestNewSSID = ssid;
    *ctx->wifiTestNewPassword = password;
    *ctx->wifiTestState = "testing";
    *ctx->wifiTestMessage = "";
    *ctx->wifiTestCountdown = 15;

    if (!*ctx->wifiTestTask) {
        *ctx->wifiTestTask = new Task(TASK_SECOND, TASK_FOREVER, [ctx]() {
            if (*ctx->wifiTestCountdown == 15) {
                extern bool _apModeActive;
                if (_apModeActive) {
                    WiFi.mode(WIFI_AP_STA);
                } else {
                    WiFi.disconnect(true);
                }
                WiFi.begin(ctx->wifiTestNewSSID->c_str(), ctx->wifiTestNewPassword->c_str());
                Log.info("WiFi", "Testing connection to '%s'...", ctx->wifiTestNewSSID->c_str());
            }
            (*ctx->wifiTestCountdown)--;
            if (WiFi.status() == WL_CONNECTED) {
                String newIP = WiFi.localIP().toString();
                ctx->config->setWifiSSID(*ctx->wifiTestNewSSID);
                ctx->config->setWifiPassword(*ctx->wifiTestNewPassword);
                ProjectInfo* proj = ctx->config->getProjectInfo();
                ctx->config->updateConfig("/config.txt", *proj);
                *ctx->wifiTestState = "success";
                *ctx->wifiTestMessage = newIP;
                Log.info("WiFi", "Test OK — connected to '%s' at %s. Rebooting...",
                         ctx->wifiTestNewSSID->c_str(), newIP.c_str());
                (*ctx->wifiTestTask)->disable();
                if (!*ctx->delayedReboot) {
                    *ctx->delayedReboot = new Task(3 * TASK_SECOND, TASK_ONCE, [ctx]() {
                        *ctx->shouldReboot = true;
                    }, ctx->scheduler, false);
                }
                (*ctx->delayedReboot)->restartDelayed(3 * TASK_SECOND);
                return;
            }
            if (*ctx->wifiTestCountdown == 0) {
                Log.warn("WiFi", "Test FAILED — could not connect to '%s'", ctx->wifiTestNewSSID->c_str());
                WiFi.disconnect(true);
                extern bool _apModeActive;
                if (_apModeActive) {
                    WiFi.mode(WIFI_AP);
                } else {
                    WiFi.begin(ctx->wifiOldSSID->c_str(), ctx->wifiOldPassword->c_str());
                }
                *ctx->wifiTestState = "failed";
                *ctx->wifiTestMessage = "Could not connect to " + *ctx->wifiTestNewSSID;
                (*ctx->wifiTestTask)->disable();
            }
        }, ctx->scheduler, false);
    }
    (*ctx->wifiTestTask)->restartDelayed(TASK_SECOND);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"testing\"}");
    return ESP_OK;
}

static esp_err_t scanGetHandler(httpd_req_t* req) {
    String json = "[";
    int n = WiFi.scanComplete();
    if (n == -2) {
        WiFi.scanNetworks(true);
    } else if (n) {
        for (int i = 0; i < n; ++i) {
            if (i) json += ",";
            json += "{\"rssi\":" + String(WiFi.RSSI(i));
            json += ",\"ssid\":\"" + WiFi.SSID(i) + "\"";
            json += ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"";
            json += ",\"channel\":" + String(WiFi.channel(i));
            json += ",\"secure\":" + String(WiFi.encryptionType(i));
            json += "}";
        }
        WiFi.scanDelete();
        if (WiFi.scanComplete() == -2) {
            WiFi.scanNetworks(true);
        }
    }
    json += "]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// --- Theme CSS handler ---

static esp_err_t themeCssGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/theme.css");
}

static esp_err_t themeGetHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;
    String theme = "dark";
    String sysName = "AThermostat";
    uint8_t poll = 2;
    if (ctx->config && ctx->config->getProjectInfo()) {
        theme = ctx->config->getProjectInfo()->theme;
        if (theme.length() == 0) theme = "dark";
        if (ctx->config->getProjectInfo()->systemName.length() > 0)
            sysName = ctx->config->getProjectInfo()->systemName;
        poll = ctx->config->getProjectInfo()->pollIntervalSec;
    }
    String json = "{\"theme\":\"" + theme + "\",\"systemName\":\"" + sysName + "\",\"pollIntervalSec\":" + String(poll) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Root/index handler ---

static esp_err_t rootGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/index.html");
}

// --- Log handler (proxy to ring buffer) ---

static esp_err_t logGetHandler(httpd_req_t* req) {
    size_t count = Log.getRingBufferCount();
    size_t limit = count;

    size_t qLen = httpd_req_get_url_query_len(req);
    if (qLen > 0) {
        char* qBuf = (char*)malloc(qLen + 1);
        if (qBuf && httpd_req_get_url_query_str(req, qBuf, qLen + 1) == ESP_OK) {
            char val[16] = {};
            if (httpd_query_key_value(qBuf, "limit", val, sizeof(val)) == ESP_OK) {
                size_t l = atoi(val);
                if (l < limit) limit = l;
            }
        }
        free(qBuf);
    }

    const auto& buffer = Log.getRingBuffer();
    size_t head = Log.getRingBufferHead();
    size_t bufSize = buffer.size();
    size_t startOffset = count - limit;

    String json = "{\"count\":" + String(limit) + ",\"entries\":[";
    for (size_t i = 0; i < limit; i++) {
        size_t idx = (head + bufSize - count + startOffset + i) % bufSize;
        if (i > 0) json += ",";
        json += "\"";
        const String& entry = buffer[idx];
        for (size_t j = 0; j < entry.length(); j++) {
            char c = entry[j];
            switch (c) {
                case '"':  json += "\\\""; break;
                case '\\': json += "\\\\"; break;
                case '\n': json += "\\n"; break;
                case '\r': json += "\\r"; break;
                case '\t': json += "\\t"; break;
                default:   json += c; break;
            }
        }
        json += "\"";
    }
    json += "]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Log level/config handlers ---

static esp_err_t logLevelGetHandler(httpd_req_t* req) {
    String json = "{\"level\":" + String(Log.getLevel()) +
                  ",\"levelName\":\"" + String(Log.getLevelName(Log.getLevel())) + "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t logLevelPostHandler(httpd_req_t* req) {
    size_t qLen = httpd_req_get_url_query_len(req);
    if (qLen == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing level param");
        return ESP_OK;
    }
    char* qBuf = (char*)malloc(qLen + 1);
    if (!qBuf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
        return ESP_OK;
    }
    httpd_req_get_url_query_str(req, qBuf, qLen + 1);
    char val[16] = {};
    if (httpd_query_key_value(qBuf, "level", val, sizeof(val)) == ESP_OK) {
        int level = atoi(val);
        if (level >= 0 && level <= 3) {
            Log.setLevel((Logger::Level)level);
            Log.info("HTTPS", "Log level changed to %d", level);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "level must be 0-3");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing level param");
    }
    free(qBuf);
    return ESP_OK;
}

static esp_err_t logConfigGetHandler(httpd_req_t* req) {
    String json = "{\"level\":" + String(Log.getLevel()) +
                  ",\"levelName\":\"" + String(Log.getLevelName(Log.getLevel())) +
                  "\",\"serial\":" + String(Log.isSerialEnabled() ? "true" : "false") +
                  ",\"mqtt\":" + String(Log.isMqttEnabled() ? "true" : "false") +
                  ",\"sdcard\":" + String(Log.isFileLogEnabled() ? "true" : "false") +
                  ",\"websocket\":" + String(Log.isWebSocketEnabled() ? "true" : "false") + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t logConfigPostHandler(httpd_req_t* req) {
    size_t qLen = httpd_req_get_url_query_len(req);
    if (qLen == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no params");
        return ESP_OK;
    }
    char* qBuf = (char*)malloc(qLen + 1);
    if (!qBuf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
        return ESP_OK;
    }
    httpd_req_get_url_query_str(req, qBuf, qLen + 1);
    char val[16] = {};
    if (httpd_query_key_value(qBuf, "serial", val, sizeof(val)) == ESP_OK)
        Log.enableSerial(strcmp(val, "true") == 0);
    if (httpd_query_key_value(qBuf, "mqtt", val, sizeof(val)) == ESP_OK)
        Log.enableMqtt(strcmp(val, "true") == 0);
    if (httpd_query_key_value(qBuf, "sdcard", val, sizeof(val)) == ESP_OK)
        Log.enableFileLog(strcmp(val, "true") == 0);
    if (httpd_query_key_value(qBuf, "websocket", val, sizeof(val)) == ESP_OK)
        Log.enableWebSocket(strcmp(val, "true") == 0);
    free(qBuf);
    Log.info("HTTPS", "Log config updated");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- Revert handlers ---

static esp_err_t revertGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    bool exists = firmwareBackupExists();
    size_t size = exists ? firmwareBackupSize() : 0;
    String json = "{\"exists\":" + String(exists ? "true" : "false") +
                  ",\"size\":" + String(size) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t revertPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!firmwareBackupExists()) {
        httpd_resp_send(req, "FAIL: no backup", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (revertFirmwareFromFS()) {
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        if (!*(ctx->delayedReboot)) {
            *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
                *(ctx->shouldReboot) = true;
            }, ctx->scheduler, false);
        }
        (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    } else {
        httpd_resp_send(req, "FAIL", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t rebootPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (ctx->rebootRateLimited && *(ctx->rebootRateLimited)) {
        String clientIP = getClientIP(req);
        String ua = getUserAgent(req);
        Log.error("SEC", "REBOOT BLOCKED (rate limited) from %s UA='%s'",
            clientIP.c_str(), ua.c_str());
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "Reboot rate limited — too many rapid reboots", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String clientIP = getClientIP(req);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    Log.info("HTTPS", "Reboot requested from %s, rebooting in 2s...", clientIP.c_str());
    if (!*(ctx->delayedReboot)) {
        *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
            *(ctx->shouldReboot) = true;
        }, ctx->scheduler, false);
    }
    (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    return ESP_OK;
}

// --- Admin setup handlers ---

static esp_err_t safeModeClearHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (ctx->config && ctx->config->getProjectInfo()) {
        ctx->config->getProjectInfo()->forceSafeMode = false;
        ctx->config->updateConfig("/config.txt", *ctx->config->getProjectInfo());
    }
    Log.info("HTTPS", "Safe mode cleared, rebooting...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Safe mode cleared, rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    if (!*(ctx->delayedReboot)) {
        *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
            *(ctx->shouldReboot) = true;
        }, ctx->scheduler, false);
    }
    (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    return ESP_OK;
}

static esp_err_t safeModeForceHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (ctx->config && ctx->config->getProjectInfo()) {
        ctx->config->getProjectInfo()->forceSafeMode = true;
        ctx->config->updateConfig("/config.txt", *ctx->config->getProjectInfo());
    }
    Log.warn("HTTPS", "Force safe mode set, rebooting...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Rebooting into safe mode...\"}", HTTPD_RESP_USE_STRLEN);
    if (!*(ctx->delayedReboot)) {
        *(ctx->delayedReboot) = new Task(2 * TASK_SECOND, TASK_ONCE, [ctx]() {
            *(ctx->shouldReboot) = true;
        }, ctx->scheduler, false);
    }
    (*(ctx->delayedReboot))->restartDelayed(2 * TASK_SECOND);
    return ESP_OK;
}

static esp_err_t adminSetupGetHandler(httpd_req_t* req) {
    return serveFileHttps(req, "/www/admin.html");
}

static esp_err_t adminSetupPostHandler(httpd_req_t* req) {
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (ctx->config->hasAdminPassword()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Admin password already set. Change it from the config page.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    free(body);

    String pw = doc["password"] | String("");
    String confirm = doc["confirm"] | String("");

    if (pw.length() < 4) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Password must be at least 4 characters.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (pw != confirm) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Passwords do not match.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ctx->config->setAdminPassword(pw);
    ProjectInfo* proj = ctx->config->getProjectInfo();
    ctx->config->updateConfig("/config.txt", *proj);
    Log.info("AUTH", "Admin password set via setup page (HTTPS)");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Admin password set.\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- FS info/format handlers ---

static esp_err_t fsInfoGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!ctx->config) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Config not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String json = ctx->config->getFSInfo();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t fsFormatPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;
    HttpsContext* ctx = (HttpsContext*)req->user_ctx;

    if (!ctx->config) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Config not available\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 256) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char* body = (char*)malloc(remaining + 1);
    if (!body) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int received = 0;
    while (received < remaining) {
        int ret = httpd_req_recv(req, body + received, remaining - received);
        if (ret <= 0) { free(body); return ESP_OK; }
        received += ret;
    }
    body[received] = '\0';

    JsonDocument data;
    if (deserializeJson(data, body)) {
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid JSON\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    free(body);

    String confirm = data["confirm"] | String("");
    if (confirm != "FORMAT") {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Must send {\\\"confirm\\\":\\\"FORMAT\\\"}\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ProjectInfo* proj = ctx->config->getProjectInfo();
    bool ok = ctx->config->formatFS(*proj);

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Filesystem formatted. Credentials preserved. Certs will auto-generate on reboot.\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"error\":\"Format failed\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// --- LittleFS www file upload/list/delete handlers ---

static bool isValidWwwFilename(const char* name) {
    if (!name || name[0] == '\0') return false;
    // Reject path traversal
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return false;
    size_t len = strlen(name);
    if (len > 64) return false;
    // Only allow alphanumeric, dots, hyphens, underscores
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    // Must end with allowed extension
    static const char* exts[] = {".html",".css",".js",".json",".ico",".png",".svg"};
    for (int i = 0; i < 7; i++) {
        size_t elen = strlen(exts[i]);
        if (len > elen && strcmp(name + len - elen, exts[i]) == 0) return true;
    }
    return false;
}

static esp_err_t wwwUploadPostHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;

    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 51200) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"File too large (max 50KB)\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char filename[80] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing X-Filename header\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!isValidWwwFilename(filename)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid filename\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String path = "/www/" + String(filename);
    fs::File file = LittleFS.open(path.c_str(), FILE_WRITE);
    if (!file) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Failed to open file for writing\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[1024];
    int totalWritten = 0;
    while (remaining > 0) {
        int toRead = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int ret = httpd_req_recv(req, buf, toRead);
        if (ret <= 0) {
            file.close();
            LittleFS.remove(path.c_str());
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Receive failed\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        if (file.write((uint8_t*)buf, ret) != (size_t)ret) {
            file.close();
            LittleFS.remove(path.c_str());
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"Write failed\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        remaining -= ret;
        totalWritten += ret;
    }

    file.close();
    Log.info("WWW", "Uploaded %s (%d bytes)", filename, totalWritten);

    String json = "{\"status\":\"ok\",\"filename\":\"" + String(filename) +
                  "\",\"size\":" + String(totalWritten) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t wwwListGetHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;

    File dir = LittleFS.open("/www");
    if (!dir || !dir.isDirectory()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"files\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String json = "{\"files\":[";
    bool first = true;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            int slashIdx = name.lastIndexOf('/');
            if (slashIdx >= 0) name = name.substring(slashIdx + 1);
            size_t size = entry.size();
            if (!first) json += ",";
            json += "{\"name\":\"" + name + "\",\"size\":" + String(size) + "}";
            first = false;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    json += "]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t wwwDeleteHandler(httpd_req_t* req) {
    if (!checkHttpsAuth(req)) return ESP_OK;

    char filename[80] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing X-Filename header\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!isValidWwwFilename(filename)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Invalid filename\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String path = "/www/" + String(filename);
    if (!LittleFS.exists(path.c_str())) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    LittleFS.remove(path.c_str());
    Log.info("WWW", "Deleted %s", filename);

    String json = "{\"status\":\"ok\",\"deleted\":\"" + String(filename) + "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

static esp_err_t i2cScanHandler(httpd_req_t* req) {
    String json = "[";
    bool first = true;
    for (uint8_t addr = 1; addr < 127; addr++) {
        bool found = false;
        Wire.beginTransmission(addr);
        found = (Wire.endTransmission() == 0);
        if (found) {
            if (!first) json += ",";
            char hex[7];
            snprintf(hex, sizeof(hex), "0x%02X", addr);
            json += "{\"address\":\"" + String(hex) + "\",\"decimal\":" + String(addr) + "}";
            first = false;
        }
    }
    json += "]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

// --- Public API ---

HttpsServerHandle httpsStart(const uint8_t* cert, size_t certLen,
                             const uint8_t* key, size_t keyLen,
                             HttpsContext* ctx) {
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.cacert_pem = cert;
    cfg.cacert_len = certLen + 1;  // PEM null terminator
    cfg.prvtkey_pem = key;
    cfg.prvtkey_len = keyLen + 1;
    cfg.port_secure = 443;
    cfg.httpd.max_uri_handlers = 50;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_ssl_start(&server, &cfg);
    if (err != ESP_OK) {
        Log.error("HTTPS", "Failed to start HTTPS server: %s", esp_err_to_name(err));
        return nullptr;
    }

    // Register URI handlers
    httpd_uri_t themeCssGet = {
        .uri = "/theme.css",
        .method = HTTP_GET,
        .handler = themeCssGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &themeCssGet);

    httpd_uri_t themeGet = {
        .uri = "/theme",
        .method = HTTP_GET,
        .handler = themeGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &themeGet);

    httpd_uri_t rootGet = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = rootGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &rootGet);

    httpd_uri_t adminGet = {
        .uri = "/admin/setup",
        .method = HTTP_GET,
        .handler = adminSetupGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &adminGet);

    httpd_uri_t adminPost = {
        .uri = "/admin/setup",
        .method = HTTP_POST,
        .handler = adminSetupPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &adminPost);

    httpd_uri_t pinsGet = {
        .uri = "/pins",
        .method = HTTP_GET,
        .handler = pinsGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &pinsGet);

    httpd_uri_t pinsPost = {
        .uri = "/pins",
        .method = HTTP_POST,
        .handler = pinsPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &pinsPost);

    httpd_uri_t dashGet = {
        .uri = "/dashboard",
        .method = HTTP_GET,
        .handler = dashboardGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &dashGet);

    httpd_uri_t logViewGet = {
        .uri = "/log/view",
        .method = HTTP_GET,
        .handler = logViewGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logViewGet);

    httpd_uri_t heapViewGet = {
        .uri = "/heap/view",
        .method = HTTP_GET,
        .handler = heapViewGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &heapViewGet);

    httpd_uri_t wifiViewGet = {
        .uri = "/wifi/view",
        .method = HTTP_GET,
        .handler = wifiViewGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wifiViewGet);

    httpd_uri_t scanGet = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scanGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &scanGet);

    httpd_uri_t wifiStatusGet = {
        .uri = "/wifi/status",
        .method = HTTP_GET,
        .handler = wifiStatusGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wifiStatusGet);

    httpd_uri_t wifiTestPost = {
        .uri = "/wifi/test",
        .method = HTTP_POST,
        .handler = wifiTestPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wifiTestPost);

    httpd_uri_t stateGet = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = stateGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &stateGet);

    httpd_uri_t cfgGet = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = configGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &cfgGet);

    httpd_uri_t cfgPost = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = configPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &cfgPost);

    httpd_uri_t updGet = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = updateGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &updGet);

    httpd_uri_t updPost = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = updatePostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &updPost);

    httpd_uri_t appGet = {
        .uri = "/apply",
        .method = HTTP_GET,
        .handler = applyGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &appGet);

    httpd_uri_t appPost = {
        .uri = "/apply",
        .method = HTTP_POST,
        .handler = applyPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &appPost);

    httpd_uri_t revGet = {
        .uri = "/revert",
        .method = HTTP_GET,
        .handler = revertGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &revGet);

    httpd_uri_t revPost = {
        .uri = "/revert",
        .method = HTTP_POST,
        .handler = revertPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &revPost);

    httpd_uri_t rebootPost = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = rebootPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &rebootPost);

    httpd_uri_t smClear = {
        .uri = "/safemode/clear",
        .method = HTTP_POST,
        .handler = safeModeClearHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &smClear);

    httpd_uri_t smForce = {
        .uri = "/safemode/force",
        .method = HTTP_POST,
        .handler = safeModeForceHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &smForce);

    httpd_uri_t apTestPost = {
        .uri = "/ap/test",
        .method = HTTP_POST,
        .handler = apTestHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &apTestPost);

    httpd_uri_t apStopPost = {
        .uri = "/ap/stop",
        .method = HTTP_POST,
        .handler = apStopHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &apStopPost);

    httpd_uri_t fsInfoGet = {
        .uri = "/fs/info",
        .method = HTTP_GET,
        .handler = fsInfoGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &fsInfoGet);

    httpd_uri_t fsFormatPost = {
        .uri = "/fs/format",
        .method = HTTP_POST,
        .handler = fsFormatPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &fsFormatPost);

    httpd_uri_t logGet = {
        .uri = "/log",
        .method = HTTP_GET,
        .handler = logGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logGet);

    httpd_uri_t logLevelGet = {
        .uri = "/log/level",
        .method = HTTP_GET,
        .handler = logLevelGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logLevelGet);

    httpd_uri_t logLevelPost = {
        .uri = "/log/level",
        .method = HTTP_POST,
        .handler = logLevelPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logLevelPost);

    httpd_uri_t logConfigGet = {
        .uri = "/log/config",
        .method = HTTP_GET,
        .handler = logConfigGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logConfigGet);

    httpd_uri_t logConfigPost = {
        .uri = "/log/config",
        .method = HTTP_POST,
        .handler = logConfigPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logConfigPost);

    httpd_uri_t heapGet = {
        .uri = "/heap",
        .method = HTTP_GET,
        .handler = heapGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &heapGet);

    httpd_uri_t wwwUploadPost = {
        .uri = "/www/upload",
        .method = HTTP_POST,
        .handler = wwwUploadPostHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wwwUploadPost);

    httpd_uri_t wwwListGet = {
        .uri = "/www/list",
        .method = HTTP_GET,
        .handler = wwwListGetHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wwwListGet);

    httpd_uri_t wwwDelete = {
        .uri = "/www/upload",
        .method = HTTP_DELETE,
        .handler = wwwDeleteHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wwwDelete);

    httpd_uri_t wwwDeletePost = {
        .uri = "/www/delete",
        .method = HTTP_POST,
        .handler = wwwDeleteHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &wwwDeletePost);

    httpd_uri_t i2cScanGet = {
        .uri = "/i2c/scan",
        .method = HTTP_GET,
        .handler = i2cScanHandler,
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &i2cScanGet);

    // Login page (no auth)
    httpd_uri_t loginGet = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            return serveFileHttps(req, "/www/login.html");
        },
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &loginGet);

    // Login API (no auth — validates password, creates session)
    httpd_uri_t loginPost = {
        .uri = "/api/login",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            HttpsContext* ctx = (HttpsContext*)req->user_ctx;
            if (!ctx->config || !ctx->config->hasAdminPassword()) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"error\":\"No admin password set\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            int remaining = req->content_len;
            if (remaining <= 0 || remaining > 1024) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"error\":\"Invalid body\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            char* body = (char*)malloc(remaining + 1);
            if (!body) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
            int received = 0;
            while (received < remaining) {
                int ret = httpd_req_recv(req, body + received, remaining - received);
                if (ret <= 0) { free(body); return ESP_OK; }
                received += ret;
            }
            body[received] = '\0';

            JsonDocument data;
            if (deserializeJson(data, body)) { free(body); return ESP_OK; }
            free(body);

            String pw = data["password"] | String("");
            if (!ctx->config->verifyAdminPassword(pw)) {
                httpd_resp_set_status(req, "403 Forbidden");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"error\":\"Invalid password\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            String clientIP = getClientIP(req);
            String token = ctx->sessionMgr->createSession(clientIP);
            uint32_t maxAge = ctx->sessionMgr->getTimeoutMinutes() * 60;
            String cookie = "session=" + token + "; Path=/; HttpOnly; SameSite=Strict; Secure";
            if (maxAge > 0) cookie += "; Max-Age=" + String(maxAge);
            httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
            httpd_resp_set_type(req, "application/json");
            String resp = "{\"ok\":true,\"timeout\":" + String(ctx->sessionMgr->getTimeoutMinutes()) + "}";
            httpd_resp_send(req, resp.c_str(), resp.length());
            Log.info("AUTH", "HTTPS session created for %s", clientIP.c_str());
            return ESP_OK;
        },
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &loginPost);

    // Logout API
    httpd_uri_t logoutPost = {
        .uri = "/api/logout",
        .method = HTTP_POST,
        .handler = [](httpd_req_t* req) -> esp_err_t {
            HttpsContext* ctx = (HttpsContext*)req->user_ctx;
            size_t cookieLen = httpd_req_get_hdr_value_len(req, "Cookie");
            if (cookieLen > 0) {
                char* cookieBuf = (char*)malloc(cookieLen + 1);
                if (cookieBuf) {
                    httpd_req_get_hdr_value_str(req, "Cookie", cookieBuf, cookieLen + 1);
                    String token = SessionManager::extractSessionToken(String(cookieBuf));
                    free(cookieBuf);
                    if (token.length() > 0 && ctx->sessionMgr) {
                        ctx->sessionMgr->invalidateSession(token);
                    }
                }
            }
            httpd_resp_set_hdr(req, "Set-Cookie", "session=; Path=/; HttpOnly; Max-Age=0");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = ctx
    };
    httpd_register_uri_handler(server, &logoutPost);

    Log.info("HTTPS", "HTTPS server started on port 443");
    return (HttpsServerHandle)server;
}
