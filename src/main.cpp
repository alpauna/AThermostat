#include <Arduino.h>
#include <esp_freertos_hooks.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <TaskSchedulerDeclarations.h>
#include "Logger.h"
#include "OutPin.h"
#include "InputPin.h"
#include "Thermostat.h"
#include "HX710.h"
#include "Config.h"
#include "WebHandler.h"
#include "MQTTHandler.h"
#include "OtaUtils.h"
#include <SimpleFTPServer.h>
#include <DNSServer.h>

#ifdef DEFAULT_AP_PASSWORD
static const char* _DEFAULT_AP_PW = DEFAULT_AP_PASSWORD;
#else
static const char* _DEFAULT_AP_PW = "";
#endif

extern const char compile_date[] = __DATE__ " " __TIME__;
const char* _filename = "/config.txt";

#if CIRCULAR_BUFFER_INT_SAFE
#else
#error "Needs to set CIRCULAR_BUFFER_INT_SAFE"
#endif

String _WIFI_SSID = "";
String _WIFI_PASSWORD = "";

// Config instance
Config config;

// FTP server
FtpServer ftpSrv;
bool ftpActive = false;
unsigned long ftpStopTime = 0;
static String _ftpActivePassword;  // Must persist — SimpleFTPServer stores pointer, not copy

std::map<String, InputPin*> _isrEvent;

// Scheduler
Scheduler ts;

// CPU load monitoring via FreeRTOS idle hooks
static volatile int64_t _lastIdleCore0 = 0;
static volatile int64_t _lastIdleCore1 = 0;
static volatile uint32_t _idleUsCore0 = 0;
static volatile uint32_t _idleUsCore1 = 0;
static uint8_t _cpuLoadCore0 = 0;
static uint8_t _cpuLoadCore1 = 0;

static bool idleHookCore0() {
  int64_t now = esp_timer_get_time();
  int64_t delta = now - _lastIdleCore0;
  _lastIdleCore0 = now;
  if (delta > 0 && delta < 200) _idleUsCore0 += (uint32_t)delta;
  return false;
}
static bool idleHookCore1() {
  int64_t now = esp_timer_get_time();
  int64_t delta = now - _lastIdleCore1;
  _lastIdleCore1 = now;
  if (delta > 0 && delta < 200) _idleUsCore1 += (uint32_t)delta;
  return false;
}

uint8_t getCpuLoadCore0() { return _cpuLoadCore0; }
uint8_t getCpuLoadCore1() { return _cpuLoadCore1; }

// WiFi AP fallback
static uint32_t _wifiDisconnectCount = 0;
bool _apModeActive = false;
String _apPassword;
DNSServer _dnsServer;
volatile bool _needStartHttps = false;

// Boot watchdog — RTC memory survives software resets
RTC_NOINIT_ATTR static uint32_t _rapidRebootCount;
static const uint32_t RAPID_REBOOT_THRESHOLD = 3;
static const uint32_t REBOOT_STABLE_MS = 5UL * 60 * 1000; // 5 min
static bool _rebootRateLimited = false;

RTC_NOINIT_ATTR static uint32_t _crashBootCount;
static const uint32_t CRASH_BOOT_THRESHOLD = 3;
static const uint32_t BOOT_STABLE_MS = 30UL * 1000; // 30s
bool _safeMode = false;

// --- Pin definitions (ESP32-S3-DevKitC-1) ---
static const uint8_t PIN_FAN1          = GPIO_NUM_4;
static const uint8_t PIN_REV           = GPIO_NUM_5;
static const uint8_t PIN_FURN_COOL_LOW = GPIO_NUM_6;
static const uint8_t PIN_FURN_COOL_HIGH= GPIO_NUM_7;
static const uint8_t PIN_W1            = GPIO_NUM_15;
static const uint8_t PIN_W2            = GPIO_NUM_16;
static const uint8_t PIN_COMP1         = GPIO_NUM_17;
static const uint8_t PIN_COMP2         = GPIO_NUM_18;

static const uint8_t PIN_OUT_TEMP_OK   = GPIO_NUM_45;
static const uint8_t PIN_DEFROST_MODE  = GPIO_NUM_47;

// HX710 pressure sensor pins
static const uint8_t PIN_HX710_1_DOUT  = GPIO_NUM_19;
static const uint8_t PIN_HX710_1_CLK   = GPIO_NUM_20;
static const uint8_t PIN_HX710_2_DOUT  = GPIO_NUM_10;
static const uint8_t PIN_HX710_2_CLK   = GPIO_NUM_11;

// I2C (reserved)
static const uint8_t PIN_SDA           = GPIO_NUM_8;
static const uint8_t PIN_SCL           = GPIO_NUM_9;

// ProjectInfo with defaults
ProjectInfo proj = {
  "AThermostat",         // name
  compile_date,          // createdOnDate
  "Thermostat controller for Goodman furnace + heatpump", // description
  "",                    // encrypt
  false,                 // encrypted
  512 * 1024,            // maxLogSize: 512KB (LittleFS)
  3,                     // maxOldLogCount
  "CST6CDT,M3.2.0,M11.1.0",  // timezone
  68.0f,                 // heatSetpoint
  76.0f,                 // coolSetpoint
  0,                     // thermostatMode: OFF
  false,                 // forceFurnace
  false,                 // forceNoHP
  180000,                // minOnTimeMs: 3 min
  180000,                // minOffTimeMs: 3 min
  60000,                 // minIdleTimeMs: 1 min
  1800000,               // maxRunTimeMs: 30 min
  600000,                // escalationDelayMs: 10 min
  0.5f,                  // heatDeadband
  0.5f,                  // coolDeadband
  0.5f,                  // heatOverrun
  0.5f,                  // coolOverrun
  false,                 // fanIdleEnabled
  15,                    // fanIdleWaitMin
  5,                     // fanIdleRunMin
  -134333, 6340104,      // hx710_1 raw points
  0.3214f, 83.4454f,     // hx710_1 val points
  -134333, 6340104,      // hx710_2 raw points
  3.4414f, 86.5653f,     // hx710_2 val points
  600,                   // apFallbackSeconds
  _DEFAULT_AP_PW,        // apPassword
  "",                    // ftpPassword (empty = default "admin")
  "dark",                // theme
  2,                     // pollIntervalSec
  "AThermostat",         // systemName
  "thermostat",          // mqttPrefix
  "homeassistant/sensor/average_home_temperature/state", // mqttTempTopic
  0,                     // sessionTimeoutMinutes
  false                  // forceSafeMode
};

// Thermostat, WebHandler, MQTTHandler
Thermostat thermostat(&ts);
WebHandler webHandler(80, &ts, &thermostat);
MQTTHandler mqttHandler(&ts);

// HX710 pressure sensors
HX710 hx710_1(PIN_HX710_1_DOUT, PIN_HX710_1_CLK);
HX710 hx710_2(PIN_HX710_2_DOUT, PIN_HX710_2_CLK);

// Output pins (no activation delay — thermostat min-time handles cycling)
void onInput(InputPin *pin);
bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent);

OutPin outFan1(&ts, 0, PIN_FAN1, "fan1", "GPIO4", onOutpin);
OutPin outRev(&ts, 0, PIN_REV, "rev", "GPIO5", onOutpin);
OutPin outFurnCoolLow(&ts, 0, PIN_FURN_COOL_LOW, "furn_cool_low", "GPIO6", onOutpin);
OutPin outFurnCoolHigh(&ts, 0, PIN_FURN_COOL_HIGH, "furn_cool_high", "GPIO7", onOutpin);
OutPin outW1(&ts, 0, PIN_W1, "w1", "GPIO15", onOutpin);
OutPin outW2(&ts, 0, PIN_W2, "w2", "GPIO16", onOutpin);
OutPin outComp1(&ts, 0, PIN_COMP1, "comp1", "GPIO17", onOutpin);
OutPin outComp2(&ts, 0, PIN_COMP2, "comp2", "GPIO18", onOutpin);

// Input pins with debounce
InputPin inOutTempOk(&ts, 4000, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL,
                     PIN_OUT_TEMP_OK, "out_temp_ok", "GPIO45", onInput);
InputPin inDefrostMode(&ts, 2000, InputResistorType::IT_PULLDOWN, InputPinType::IT_DIGITAL,
                       PIN_DEFROST_MODE, "defrost_mode", "GPIO47", onInput);

// --- Callbacks ---

void onInput(InputPin *pin) {
  Log.info("InputPin", "%s: active=%d", pin->getName().c_str(), pin->isActive());
}

bool onOutpin(OutPin *pin, bool on, bool inCallback, float &newPercent, float origPercent) {
  Log.info("OutPin", "%s: state=%d newPercent=%.0f", pin->getName().c_str(), on, newPercent);
  return true;
}

// --- ISR ---

void IRAM_ATTR inputISRChange(void *arg) {
  InputPin* pinInfo = static_cast<InputPin*>(arg);
  if (pinInfo == nullptr) return;
  pinInfo->setPrevValue();
  pinInfo->changedNow();
  if (_isrEvent.find(pinInfo->getName()) == _isrEvent.end()) {
    _isrEvent[pinInfo->getName()] = pinInfo;
  }
}

bool CheckTickTime(InputPin *pin) {
  uint32_t curTime = millis();
  if (pin == nullptr) return false;
  if (curTime >= pin->changedAtTick() + 50 || pin->getPreValue() != pin->getValue()) {
    return true;
  }
  return false;
}

// --- WiFi ---

bool onWifiWaitEnable() {
  if (WiFi.isConnected()) return false;
  mqttHandler.disconnect();
  return true;
}

void onWifiWaitDisable() {
  if (WiFi.isConnected()) {
    Log.info("WiFi", "Connected! IP: %s", WiFi.localIP().toString().c_str());
    mqttHandler.startReconnect();
  }
}

Task tWaitOnWiFi(TASK_SECOND, 60, [](){
  Serial.print(".");
}, &ts, false, onWifiWaitEnable, onWifiWaitDisable);

// --- AP Fallback ---

void onAPReconnect();
Task tAPReconnect(TASK_MINUTE, TASK_FOREVER, &onAPReconnect, &ts, false);

void startAPMode() {
  String apSSID = proj.systemName.length() > 0 ? proj.systemName : "AThermostat";
  if (proj.apPassword.length() >= 8) {
    _apPassword = proj.apPassword;
  } else if (_apPassword.length() == 0) {
    _apPassword = Config::generateRandomPassword();
  }
  if (!_apModeActive) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSSID.c_str(), _apPassword.c_str());
    _dnsServer.start(53, "*", WiFi.softAPIP());
    _apModeActive = true;
  }
  Log.warn("WiFi", "AP MODE ACTIVE - SSID: %s Pass: %s IP: %s",
           apSSID.c_str(), _apPassword.c_str(), WiFi.softAPIP().toString().c_str());

  if (!tAPReconnect.isEnabled()) {
    tAPReconnect.setInterval(30 * (unsigned long)TASK_SECOND);
    tAPReconnect.enableDelayed();
  }

  if (_WIFI_SSID.length() > 0 && !WiFi.isConnected()) {
    Log.info("WiFi", "Starting STA connection to '%s'", _WIFI_SSID.c_str());
    WiFi.begin(_WIFI_SSID.c_str(), _WIFI_PASSWORD.c_str());
  }
}

String startAPModeTest() {
  String apSSID = proj.systemName.length() > 0 ? proj.systemName : "AThermostat";
  if (proj.apPassword.length() >= 8) {
    _apPassword = proj.apPassword;
  } else if (_apPassword.length() == 0) {
    _apPassword = Config::generateRandomPassword();
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID.c_str(), _apPassword.c_str());
  _apModeActive = true;
  return _apPassword;
}

void stopAPMode() {
  if (!_apModeActive) return;
  _dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _apModeActive = false;
  tAPReconnect.disable();
  Log.info("WiFi", "AP mode stopped");
}

void onAPReconnect() {
  if (WiFi.isConnected()) {
    _wifiDisconnectCount = 0;
    return;
  }
  if (_WIFI_SSID.length() > 0) {
    Log.info("WiFi", "AP mode STA reconnect attempt to '%s'", _WIFI_SSID.c_str());
    WiFi.begin(_WIFI_SSID.c_str(), _WIFI_PASSWORD.c_str());
  }
}

// --- Tasks ---

void onCheckInputQueue();
Task _tGetInputs(500 * TASK_MILLISECOND, TASK_FOREVER, &onCheckInputQueue, &ts, false);

// Save thermostat state to LittleFS every 5 minutes
void onSaveThermostatState();
Task tSaveState(5 * TASK_MINUTE, TASK_FOREVER, &onSaveThermostatState, &ts, false);

// Read HX710 pressure sensors every 5 seconds
void onReadPressure();
Task tReadPressure(5 * TASK_SECOND, TASK_FOREVER, &onReadPressure, &ts, false);

// Publish MQTT state every 30 seconds
void onPublishMqttState();
Task tMqttPublish(30 * TASK_SECOND, TASK_FOREVER, &onPublishMqttState, &ts, false);

// CPU load calculation every 1 second
void onCalcCpuLoad();
Task tCpuLoad(TASK_SECOND, TASK_FOREVER, &onCalcCpuLoad, &ts, false);

// Boot watchdog tasks
Task tBootStable(BOOT_STABLE_MS, TASK_ONCE, []() {
    _crashBootCount = 0;
    Log.info("MAIN", "Boot stable (30s), crash counter reset to 0");
}, &ts, true);

Task tRebootStable(REBOOT_STABLE_MS, TASK_ONCE, []() {
    _rapidRebootCount = 0;
    _rebootRateLimited = false;
    Log.info("MAIN", "Stable uptime (5 min), reboot rate limit cleared");
}, &ts, true);

// NTP sync every 2 hours
void onNtpSync();
Task tNtpSync(2 * TASK_HOUR, TASK_FOREVER, &onNtpSync, &ts, false);

// --- Task implementations ---

void onCheckInputQueue() {
  if (_isrEvent.empty()) return;
  for (auto it = _isrEvent.begin(); it != _isrEvent.end(); ) {
    InputPin* pin = it->second;
    if (pin == nullptr || CheckTickTime(pin)) {
      if (pin != nullptr) {
        bool liveState = pin->readLiveState();
        pin->setPendingState(liveState ? 1 : 0);
        pin->getTask()->restartDelayed();
      }
      it = _isrEvent.erase(it);
    } else {
      ++it;
    }
  }
}

void onSaveThermostatState() {
  proj.heatSetpoint = thermostat.getHeatSetpoint();
  proj.coolSetpoint = thermostat.getCoolSetpoint();
  proj.thermostatMode = (uint8_t)thermostat.getMode();
  proj.forceFurnace = thermostat.isForceFurnace();
  proj.forceNoHP = thermostat.isForceNoHP();
  config.updateThermostatState(_filename, proj);
  Log.debug("MAIN", "Thermostat state saved to flash");
}

void onReadPressure() {
  hx710_1.readCalibrated();
  hx710_2.readCalibrated();
}

void onPublishMqttState() {
  mqttHandler.publishState();
}

void onCalcCpuLoad() {
  // EMA: alpha=0.3 for smooth load % (100 - idle%)
  uint32_t idle0 = _idleUsCore0;
  uint32_t idle1 = _idleUsCore1;
  _idleUsCore0 = 0;
  _idleUsCore1 = 0;

  uint8_t load0 = 100 - min(100U, idle0 / 10000);
  uint8_t load1 = 100 - min(100U, idle1 / 10000);

  _cpuLoadCore0 = (_cpuLoadCore0 * 7 + load0 * 3) / 10;
  _cpuLoadCore1 = (_cpuLoadCore1 * 7 + load1 * 3) / 10;
}

void onNtpSync() {
  webHandler.startNtpSync();
}

// --- WiFi event handler ---

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Log.info("WiFi", "Connected: %s", WiFi.localIP().toString().c_str());
      _wifiDisconnectCount = 0;
      mqttHandler.startReconnect();
      if (_apModeActive) {
        stopAPMode();
      }
      // Defer HTTPS start to main loop (can't do it from WiFi event context)
      if (config.getCertLen() > 0 && !webHandler.isSecureRunning()) {
        _needStartHttps = true;
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      _wifiDisconnectCount++;
      if (_wifiDisconnectCount <= 3 || _wifiDisconnectCount % 10 == 0) {
        Log.warn("WiFi", "Disconnected (count=%u)", _wifiDisconnectCount);
      }
      mqttHandler.stopReconnect();
      if (!_apModeActive && _wifiDisconnectCount >= (proj.apFallbackSeconds / 10)) {
        startAPMode();
      }
      break;
    default:
      break;
  }
}

// =============================================================================
// setup()
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== AThermostat ===");
  Serial.printf("Build: %s\n", compile_date);
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("PSRAM: %u bytes\n", ESP.getFreePsram());

  // Boot watchdog — detect crash loops
  esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT ||
      resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT) {
    _crashBootCount++;
    Serial.printf("!!! Crash boot detected (count=%u, reason=%d)\n", _crashBootCount, resetReason);
    if (_crashBootCount >= CRASH_BOOT_THRESHOLD) {
      _safeMode = true;
      Serial.println("!!! SAFE MODE — thermostat control disabled");
    }
  } else if (resetReason == ESP_RST_SW) {
    _rapidRebootCount++;
    if (_rapidRebootCount >= RAPID_REBOOT_THRESHOLD) {
      _rebootRateLimited = true;
      Serial.printf("!!! Rapid reboot detected (count=%u), API reboot rate-limited\n", _rapidRebootCount);
    }
  } else {
    _crashBootCount = 0;
    _rapidRebootCount = 0;
  }

  // CPU load idle hooks
  esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);

  // Init LittleFS
  if (!config.initFS()) {
    Serial.println("FATAL: LittleFS init failed!");
  }

  // Try encryption init (eFuse HMAC)
  if (!config.initEncryption()) {
    Serial.println("eFuse HMAC not available, using XOR obfuscation");
    #ifdef XOR_KEY
    Config::setObfuscationKey(XOR_KEY);
    #endif
  }

  // Load config
  if (config.openConfigFile(_filename, proj)) {
    if (!config.loadConfig(_filename, proj)) {
      Serial.println("Config load failed, using defaults");
    }
  }
  config.setProjectInfo(&proj);

  // Apply WiFi credentials
  _WIFI_SSID = config.getWifiSSID();
  _WIFI_PASSWORD = config.getWifiPassword();

  // Set logger options
  Log.setLogFile("/log.txt", proj.maxLogSize, proj.maxOldLogCount);

  // Init output pins
  outFan1.initPin();
  outRev.initPin();
  outFurnCoolLow.initPin();
  outFurnCoolHigh.initPin();
  outW1.initPin();
  outW2.initPin();
  outComp1.initPin();
  outComp2.initPin();

  // Init input pins
  inOutTempOk.initPin();
  inDefrostMode.initPin();

  // Attach ISRs
  attachInterruptArg(PIN_OUT_TEMP_OK, inputISRChange, &inOutTempOk, CHANGE);
  attachInterruptArg(PIN_DEFROST_MODE, inputISRChange, &inDefrostMode, CHANGE);

  // Init HX710 pressure sensors
  hx710_1.begin();
  hx710_1.setCalibration(proj.hx710_1_raw1, proj.hx710_1_val1,
                          proj.hx710_1_raw2, proj.hx710_1_val2);
  hx710_2.begin();
  hx710_2.setCalibration(proj.hx710_2_raw1, proj.hx710_2_val1,
                          proj.hx710_2_raw2, proj.hx710_2_val2);

  // Init thermostat
  OutPin* outputs[OUT_COUNT] = {
    &outFan1, &outRev, &outFurnCoolLow, &outFurnCoolHigh,
    &outW1, &outW2, &outComp1, &outComp2
  };
  InputPin* inputs[IN_COUNT] = { &inOutTempOk, &inDefrostMode };
  thermostat.setOutputPins(outputs);
  thermostat.setInputPins(inputs);

  // Apply config to thermostat
  thermostat.config().heatDeadband = proj.heatDeadband;
  thermostat.config().coolDeadband = proj.coolDeadband;
  thermostat.config().heatOverrun = proj.heatOverrun;
  thermostat.config().coolOverrun = proj.coolOverrun;
  thermostat.config().minOnTimeMs = proj.minOnTimeMs;
  thermostat.config().minOffTimeMs = proj.minOffTimeMs;
  thermostat.config().minIdleTimeMs = proj.minIdleTimeMs;
  thermostat.config().maxRunTimeMs = proj.maxRunTimeMs;
  thermostat.config().escalationDelayMs = proj.escalationDelayMs;
  thermostat.config().fanIdleEnabled = proj.fanIdleEnabled;
  thermostat.config().fanIdleWaitMin = proj.fanIdleWaitMin;
  thermostat.config().fanIdleRunMin = proj.fanIdleRunMin;

  thermostat.setHeatSetpoint(proj.heatSetpoint);
  thermostat.setCoolSetpoint(proj.coolSetpoint);
  thermostat.setForceFurnace(proj.forceFurnace);
  thermostat.setForceNoHP(proj.forceNoHP);

  if (!_safeMode) {
    thermostat.begin();
    thermostat.setMode((ThermostatMode)proj.thermostatMode);
  } else {
    Log.warn("MAIN", "Safe mode — thermostat not started");
  }

  // WiFi
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  if (_WIFI_SSID.length() > 0) {
    WiFi.begin(_WIFI_SSID.c_str(), _WIFI_PASSWORD.c_str());
    Serial.printf("Connecting to WiFi: %s\n", _WIFI_SSID.c_str());
    tWaitOnWiFi.enable();
  } else {
    Serial.println("No WiFi SSID configured, starting AP mode");
    startAPMode();
  }

  // Web handler
  webHandler.setConfig(&config);
  webHandler.setTimezone(proj.timezone);
  webHandler.setRebootRateLimited(&_rebootRateLimited);
  webHandler.setSafeMode(&_safeMode, &_crashBootCount);
  webHandler.setPressureSensors(&hx710_1, &hx710_2);
  webHandler.setAPCallbacks(startAPModeTest, stopAPMode);

  // FTP control callbacks — LittleFS is already initialized
  webHandler.setFtpControl(
    // Enable callback
    [](int durationMin) {
      _ftpActivePassword = proj.ftpPassword.length() > 0 ? proj.ftpPassword : "admin";
      ftpSrv.begin("admin", _ftpActivePassword.c_str());
      ftpActive = true;
      ftpStopTime = millis() + ((unsigned long)durationMin * 60000UL);
      Log.info("FTP", "FTP enabled for %d minutes", durationMin);
    },
    // Disable callback
    []() {
      if (ftpActive) {
        ftpSrv.end();
        ftpActive = false;
        ftpStopTime = 0;
        Log.info("FTP", "FTP disabled");
      }
    },
    // Status callback
    []() -> String {
      int remainingMin = 0;
      if (ftpActive && ftpStopTime > 0) {
        unsigned long now = millis();
        if (ftpStopTime > now) {
          remainingMin = (int)((ftpStopTime - now) / 60000) + 1;
        }
      }
      String ftpPw = proj.ftpPassword.length() > 0 ? proj.ftpPassword : "admin";
      return "{\"active\":" + String(ftpActive ? "true" : "false") +
             ",\"remainingMinutes\":" + String(remainingMin) +
             ",\"password\":\"" + ftpPw + "\"}";
    }
  );
  webHandler.setFtpState(&ftpActive, &ftpStopTime);

  webHandler.begin();

  // HTTPS — only start once WiFi STA is connected (not in AP mode)
  // Certs are loaded; HTTPS will be started in onWiFiEvent when STA connects
  config.loadCertificates("/cert.pem", "/key.pem");

  // MQTT
  mqttHandler.setThermostat(&thermostat);
  mqttHandler.setPressureSensors(&hx710_1, &hx710_2);
  mqttHandler.setTopicPrefix(proj.mqttPrefix);
  mqttHandler.setTempTopic(proj.mqttTempTopic);
  mqttHandler.begin(config.getMqttHost(), config.getMqttPort(),
                    config.getMqttUser(), config.getMqttPassword());
  Log.setMqttClient(mqttHandler.getClient(), (proj.mqttPrefix + "/log").c_str());

  // Enable periodic tasks
  _tGetInputs.enable();
  tSaveState.enable();
  tReadPressure.enable();
  tMqttPublish.enable();
  tCpuLoad.enable();

  Log.info("MAIN", "Setup complete. Free heap: %u PSRAM: %u",
           ESP.getFreeHeap(), ESP.getFreePsram());
}

// =============================================================================
// loop()
// =============================================================================

void loop() {
  // FTP auto-timeout
  if (ftpActive && ftpStopTime > 0 && millis() >= ftpStopTime) {
    ftpSrv.end();
    ftpActive = false;
    ftpStopTime = 0;
    Log.info("FTP", "FTP auto-disabled (timeout)");
  }
  if (ftpActive) ftpSrv.handleFTP();
  if (_apModeActive) _dnsServer.processNextRequest();

  // Deferred HTTPS start (can't run from WiFi event callback)
  if (_needStartHttps) {
    _needStartHttps = false;
    webHandler.beginSecure(config.getCert(), config.getCertLen(),
                           config.getKey(), config.getKeyLen());
  }

  ts.execute();
}
