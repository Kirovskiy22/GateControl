/*

    WT32-ETH01 — HTTP API для реле ворот + датчик положения
    Версия: 1.4.0
    Плата Arduino IDE: "WT32-ETH01 Ethernet Module"
    Partition Scheme: Default 4MB with spiffs (нужны OTA-слоты)
    API:
    GET / статус без авторизации (оставлено для совместимости)
    GET /status статус, требует X-Token
    GET /open включает/удерживает реле OPEN
    GET /close снимает удержание реле (HOLD_MODE=true)
    GET /stop снимает все реле
    POST /update загрузка прошивки, требует X-Token
    ВАЖНО:
        Датчик ИО 102-61 Б2П подключён между IO32 и GND.
        Магниты вместе => геркон замкнут => GPIO32 LOW => ворота OPEN/подняты.
        Магниты раздельно => GPIO32 HIGH => NOT_OPEN (это НЕ означает CLOSED).
        Успешный HTTP-ответ команды не подтверждает фактическое движение ворот.
        Не подключать напряжение платы управления ворот к GPIO ESP32.
    HTTP OTA:
    curl -H "X-Token: CTrehyf1" -F "firmware=@wt32_gate_relay.ino.bin" \

       http://<IP>/update

    ArduinoOTA:
    После первой настройки доступны обновления из Arduino IDE по сети,
    порт UDP 3232. В этой версии ArduinoOTA оставлен без пароля для
    совместимости с исходной прошивкой; используйте только доверенную LAN. */

#include <ETH.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <WiFi.h>

// ===== НАСТРОЙКИ =====
static const char *API_TOKEN = "CTrehyf1";
static const char *HOSTNAME = "wt32-gate";
static const char *FW_VERSION = "1.4.0";

static const bool USE_DHCP = true;
static IPAddress LOCAL_IP(192, 168, 88, 210);
static IPAddress GATEWAY(192, 168, 88, 1);
static IPAddress SUBNET(255, 255, 255, 0);
static IPAddress DNS1(192, 168, 88, 1);

// Существующие управляющие выходы — не меняем.
static const int PIN_OPEN = 4;
static const int PIN_CLOSE = 14;
static const int PIN_STOP = 15;

static const bool RELAY_ACTIVE_HIGH = true;
static const uint32_t PULSE_MS = 1000;
static const bool HOLD_MODE = true;

// Датчик: геркон между IO32 и GND.
// Магниты вместе / контакт замкнут => ворота открыты.
// Внимание: IO12 заменен на IO32, так как IO12 является strap-пином и мешал загрузке платы.
static const int PIN_GATE_SENSOR = 32;
static const uint32_t SENSOR_DEBOUNCE_MS = 60;

static const uint16_t HTTP_PORT = 80;
// ======================

WebServer server(HTTP_PORT);

static bool ethConnected = false;
static String lastCommand = "none";
static uint32_t lastCommandMs = 0;
static int heldPin = -1;
static int pendingPulsePin = -1;
static uint32_t pendingPulseUntilMs = 0;
static int deferredCommand = 0;
static bool otaReady = false;
static bool updateAuthOk = false;
static bool updateWroteOk = false;

// Фильтр датчика: обновляем состояние после устойчивого уровня.
static bool sensorRawOpen = false;
static bool sensorStableOpen = false;
static uint32_t sensorRawChangedMs = 0;
static bool sensorInitialized = false;

static int relayLevel(bool on) {
    if (RELAY_ACTIVE_HIGH) return on ? HIGH : LOW;
    return on ? LOW : HIGH;
}

static void relayOff(int pin) {
    if (pin < 0) return;
    digitalWrite(pin, relayLevel(false));
    Serial.printf("[RELAY] pin %d OFF (level %d)\n", pin, relayLevel(false));
}

static void relayOn(int pin) {
    if (pin < 0) return;
    digitalWrite(pin, relayLevel(true));
    Serial.printf("[RELAY] pin %d ON (level %d)\n", pin, relayLevel(true));
}

static void allRelaysOff() {
    relayOff(PIN_OPEN);
    relayOff(PIN_CLOSE);
    relayOff(PIN_STOP);
    heldPin = -1;
    pendingPulsePin = -1;
}

static void schedulePulse(int pin) {
    if (pin < 0) return;
    allRelaysOff();
    relayOn(pin);
    pendingPulsePin = pin;
    pendingPulseUntilMs = millis() + PULSE_MS;
}

static void servicePendingPulse() {
    if (pendingPulsePin < 0) return;
    if ((int32_t)(millis() - pendingPulseUntilMs) >= 0) {
        relayOff(pendingPulsePin);
        pendingPulsePin = -1;
    }
}

static void holdRelay(int pin) {
    if (pin < 0) return;
    allRelaysOff();
    relayOn(pin);
    heldPin = pin;
}

static bool tokenOk() {
    if (API_TOKEN == nullptr || API_TOKEN[0] == '\0') return true;
    if (server.hasHeader("X-Token") && server.header("X-Token") == API_TOKEN) return true;
    if (server.hasArg("token") && server.arg("token") == API_TOKEN) return true;
    return false;
}

static void sendJson(int code, const String &body) {
    server.sendHeader("Connection", "close");
    server.send(code, "application/json", body);
    delay(1);
    server.client().flush();
    server.client().stop();
}

static void requireAuthOrAbort() {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
}

static void updateSensor() {
    const bool rawOpen = (digitalRead(PIN_GATE_SENSOR) == LOW);

    if (!sensorInitialized) {
        sensorRawOpen = rawOpen;
        sensorStableOpen = rawOpen;
        sensorRawChangedMs = millis();
        sensorInitialized = true;
        return;
    }

    if (rawOpen != sensorRawOpen) {
        sensorRawOpen = rawOpen;
        sensorRawChangedMs = millis();
    }

    if (sensorStableOpen != sensorRawOpen && (uint32_t)(millis() - sensorRawChangedMs) >= SENSOR_DEBOUNCE_MS) {
        sensorStableOpen = sensorRawOpen;
        Serial.printf("[SENSOR] gate_status=%s\n", sensorStableOpen ? "OPEN" : "NOT_OPEN");
    }
}

static String statusJson() {
    const String ip = ethConnected ? ETH.localIP().toString() : String("0.0.0.0");
    String body;
    body.reserve(400);
    body += "{";
    body += "\"ok\":true,";
    body += "\"device\":\""; body += HOSTNAME; body += "\",";
    body += "\"fw\":\""; body += FW_VERSION; body += "\",";
    body += "\"ip\":\""; body += ip; body += "\",";
    body += "\"eth\":"; body += ethConnected ? "true" : "false"; body += ",";
    body += "\"hold_mode\":"; body += HOLD_MODE ? "true" : "false"; body += ",";
    body += "\"pulse_ms\":"; body += String(PULSE_MS); body += ",";
    body += "\"pins\":{\"open\":"; body += String(PIN_OPEN);
    body += ",\"close\":"; body += String(PIN_CLOSE);
    body += ",\"stop\":"; body += String(PIN_STOP);
    body += ",\"gate_sensor\":"; body += String(PIN_GATE_SENSOR); body += "},";
    body += "\"held_pin\":"; body += String(heldPin); body += ",";
    body += "\"last_command\":\""; body += lastCommand; body += "\",";
    body += "\"last_command_age_ms\":"; body += String(lastCommandMs == 0 ? 0 : (millis() - lastCommandMs)); body += ",";
    body += "\"gate_open_sensor\":"; body += sensorStableOpen ? "true" : "false"; body += ",";
    body += "\"gate_status\":\""; body += sensorStableOpen ? "OPEN" : "NOT_OPEN"; body += "\",";
    body += "\"sensor_debounce_ms\":"; body += String(SENSOR_DEBOUNCE_MS);
    body += "}";
    return body;
}

static void remember(const char *cmd) {
    lastCommand = cmd;
    lastCommandMs = millis();
}

static void runDeferredCommand() {
    if (deferredCommand == 0) return;
    const int cmd = deferredCommand;
    deferredCommand = 0;
    Serial.printf("[CMD] run %d (hold_mode=%d)\n", cmd, HOLD_MODE ? 1 : 0);

    if (cmd == 1) {
        if (HOLD_MODE) holdRelay(PIN_OPEN);
        else schedulePulse(PIN_OPEN);
    } else if (cmd == 2) {
        if (HOLD_MODE) {
            Serial.println("[CMD] release hold (close)");
            allRelaysOff();
        } else if (PIN_CLOSE >= 0) {
            schedulePulse(PIN_CLOSE);
        } else {
            allRelaysOff();
        }
    } else if (cmd == 3) {
        Serial.println("[CMD] stop / all off");
        allRelaysOff();
    }
}

static void handleRoot() {
    // Оставлено как в исходной версии для совместимости.
    sendJson(200, statusJson());
}

static void handleStatus() {
    if (!tokenOk()) { requireAuthOrAbort(); return; }
    sendJson(200, statusJson());
}

static void handleOpen() {
    if (!tokenOk()) { requireAuthOrAbort(); return; }
    remember("open");
    Serial.println("[CMD] /open accepted");
    sendJson(200, "{\"ok\":true,\"command\":\"open\",\"mode\":\"hold\"}");
    deferredCommand = 1;
}

static void handleClose() {
    if (!tokenOk()) { requireAuthOrAbort(); return; }
    remember("close");
    Serial.println("[CMD] /close accepted (release)");
    sendJson(200, "{\"ok\":true,\"command\":\"close\",\"mode\":\"release\"}");
    deferredCommand = 2;
}

static void handleStop() {
    if (!tokenOk()) { requireAuthOrAbort(); return; }
    remember("stop");
    Serial.println("[CMD] /stop accepted (release)");
    sendJson(200, "{\"ok\":true,\"command\":\"stop\",\"mode\":\"release\"}");
    deferredCommand = 3;
}

static void handleNotFound() {
    sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
}

static void handleUpdateUpload() {
    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        updateAuthOk = tokenOk();
        updateWroteOk = false;

        if (!updateAuthOk) {
            Serial.println("[OTA-HTTP] unauthorized upload attempt");
            Update.abort();
            return;
        }

        Serial.printf("[OTA-HTTP] start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
            updateWroteOk = false;
        } else {
            updateWroteOk = true;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (updateAuthOk && updateWroteOk) {
            const size_t written = Update.write(upload.buf, upload.currentSize);
            if (written != upload.currentSize) {
                Update.printError(Serial);
                updateWroteOk = false;
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (updateAuthOk && updateWroteOk) {
            if (Update.end(true)) {
                Serial.printf("[OTA-HTTP] success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
                updateWroteOk = false;
            }
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        updateWroteOk = false;
        Serial.println("[OTA-HTTP] upload aborted");
    }
}

static void handleUpdateResult() {
    if (!tokenOk()) { requireAuthOrAbort(); return; }

    if (Update.hasError() || !updateWroteOk) {
        sendJson(500, "{\"ok\":false,\"error\":\"firmware_update_failed\"}");
        return;
    }

    sendJson(200, "{\"ok\":true,\"update\":\"written\",\"rebooting\":true}");
    delay(500);
    ESP.restart();
}

static void startOtaAfterIp() {
    if (otaReady) return;

    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPort(3232);
    // Совместимость с исходной прошивкой: пароль OTA не задан.
    ArduinoOTA.onStart([]() { Serial.println("[OTA] start"); });
    ArduinoOTA.onEnd([]() { Serial.println("\n[OTA] end"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[OTA] %u%%\r", (progress * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] error %u\n", error);
    });
    ArduinoOTA.begin();
    otaReady = true;
    Serial.print("[OTA] ready on ");
    Serial.print(ETH.localIP());
    Serial.println(":3232 (Arduino IDE network port; UDP 3232)");
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
    (void)info;
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ETH.setHostname(HOSTNAME);
            if (!USE_DHCP) {
                ETH.config(LOCAL_IP, GATEWAY, SUBNET, DNS1);
                Serial.println("[ETH] static IP mode");
            } else {
                Serial.println("[ETH] DHCP mode");
            }
            Serial.println("[ETH] started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] link up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ethConnected = true;
            Serial.print("[ETH] IP: ");
            Serial.println(ETH.localIP());
            startOtaAfterIp();
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethConnected = false;
            Serial.println("[ETH] link down");
            break;
        case ARDUINO_EVENT_ETH_STOP:
            ethConnected = false;
            Serial.println("[ETH] stopped");
            break;
        default:
            break;
    }
}
#else
static void onEthEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ETH.setHostname(HOSTNAME);
            if (!USE_DHCP) {
                ETH.config(LOCAL_IP, GATEWAY, SUBNET, DNS1);
                Serial.println("[ETH] static IP mode");
            } else {
                Serial.println("[ETH] DHCP mode");
            }
            Serial.println("[ETH] started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("[ETH] link up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ethConnected = true;
            Serial.print("[ETH] IP: ");
            Serial.println(ETH.localIP());
            startOtaAfterIp();
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
        case ARDUINO_EVENT_ETH_STOP:
            ethConnected = false;
            Serial.println("[ETH] link down");
            break;
        default:
            break;
    }
}
#endif

static void setupRelays() {
    if (PIN_OPEN >= 0) pinMode(PIN_OPEN, OUTPUT);
    if (PIN_CLOSE >= 0) pinMode(PIN_CLOSE, OUTPUT);
    if (PIN_STOP >= 0) pinMode(PIN_STOP, OUTPUT);
    allRelaysOff();
}

static void setupHttp() {
    const char *headerKeys[] = {"X-Token"};
    server.collectHeaders(headerKeys, 1);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/open", HTTP_GET, handleOpen);
    server.on("/close", HTTP_GET, handleClose);
    server.on("/stop", HTTP_GET, handleStop);
    server.on("/open", HTTP_POST, handleOpen);
    server.on("/close", HTTP_POST, handleClose);
    server.on("/stop", HTTP_POST, handleStop);

    // OTA HTTP upload. Upload handler receives chunks; result handler responds after upload.
    server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);

    server.onNotFound(handleNotFound);
    server.begin();
    Serial.printf("[HTTP] listening on :%u\n", HTTP_PORT);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("================================");
    Serial.println("WT32-ETH01 gate relay API");
    Serial.printf("Firmware %s\n", FW_VERSION);
    Serial.println("================================");

    // Initialize sensor early; GPIO12 is a boot strap pin, check physical setup.
    pinMode(PIN_GATE_SENSOR, INPUT_PULLUP);
    sensorInitialized = false;
    updateSensor();

    setupRelays();

    Serial.println("[ETH] init... IO0 must be FREE (not to GND)");

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    Network.onEvent(onNetworkEvent);
    bool ethOk = ETH.begin();
#else
    WiFi.onEvent(onEthEvent);
    bool ethOk = ETH.begin(1, 16, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
#endif
    Serial.println(ethOk ? "[ETH] begin() OK" : "[ETH] begin() FAILED");

    setupHttp();
    Serial.println("[HINT] waiting for Ethernet link and DHCP address");
}

void loop() {
    updateSensor();
    server.handleClient();
    runDeferredCommand();

    if (otaReady) ArduinoOTA.handle();
    servicePendingPulse();

    static uint32_t lastDiagMs = 0;
    if (millis() - lastDiagMs > 5000) {
        lastDiagMs = millis();
        const bool up = ETH.linkUp();
        if (!up) ethConnected = false;
        Serial.printf("[ETH] linkUp=%s connected=%s ip=%s\n", up ? "yes" : "no", ethConnected ? "yes" : "no", ETH.localIP().toString().c_str());
        if (!up) Serial.println("[ETH] link down — check cable, port, 5V power");
    }
}
