/*

    WT32-ETH01 — HTTP API для реле шлагбаума / ворот
    Плата в Arduino IDE: "WT32-ETH01 Ethernet Module"
    Partition: Default 4MB with spiffs (нужны OTA-слоты)
    Управление (HOLD_MODE=true):
    curl -H "X-Token: CTrehyf1" http://<IP>/open
    curl -H "X-Token: CTrehyf1" http://<IP>/close
    curl -H "X-Token: CTrehyf1" http://<IP>/stop
    curl http://<IP>/status
    Удалённая прошивка по HTTP (предпочтительно, без USB):
        Arduino: Скетч → Export compiled Binary → *.ino.bin
        curl -H "X-Token: CTrehyf1" -F "firmware=@wt32_gate_relay.ino.bin" http://<IP>/update
    Arduino IDE OTA (сеть): Порт = 3232
    Реле: GPIO 4 → IN реле. IO0 после прошивки не на GND (такт Ethernet). */

#include <ETH.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ESPmDNS.h>

// ====== НАСТРОЙКИ (шлагбаум / ворота — меняйте здесь) ======
static const char *API_TOKEN = "CTrehyf1"; // пустая строка "" = без проверки
static const char *HOSTNAME = "wt32-gate";
static const char *FW_VERSION = "1.3.0";

// true = DHCP. false = статический IP ниже (запасной вариант).
static const bool USE_DHCP = true;
static IPAddress LOCAL_IP(192, 168, 88, 210);
static IPAddress GATEWAY(192, 168, 88, 1);
static IPAddress SUBNET(255, 255, 255, 0);
static IPAddress DNS1(192, 168, 88, 1);

// Пины реле (безопасные на WT32-ETH01: не ETH/UART0/IO0)
static const int PIN_OPEN = 4; // IO4
static const int PIN_CLOSE = 14; // IO14
static const int PIN_STOP = 15; // IO15; -1 если стоп не нужен

// false = реле включается LOW (часто модули с оптопарой).
// true = включается HIGH.
static const bool RELAY_ACTIVE_HIGH = true;

// Импульс на реле, мс (только если HOLD_MODE=false)
static const uint32_t PULSE_MS = 1000;

// true = /open держит реле, пока /close или /stop
static const bool HOLD_MODE = true;

static const uint16_t HTTP_PORT = 80;
// ===========================================================

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

static int relayLevel(bool on) {
    if (RELAY_ACTIVE_HIGH) {
        return on ? HIGH : LOW;
    }
    return on ? LOW : HIGH;
}

static void relayOff(int pin) {
    if (pin < 0) {
        return;
    }
    digitalWrite(pin, relayLevel(false));
    Serial.printf("[RELAY] pin %d OFF (level %d)\n", pin, relayLevel(false));
}

static void relayOn(int pin) {
    if (pin < 0) {
        return;
    }
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
    if (pin < 0) {
        return;
    }
    allRelaysOff();
    relayOn(pin);
    pendingPulsePin = pin;
    pendingPulseUntilMs = millis() + PULSE_MS;
}

static void servicePendingPulse() {
    if (pendingPulsePin < 0) {
        return;
    }
    if ((int32_t)(millis() - pendingPulseUntilMs) >= 0) {
        relayOff(pendingPulsePin);
        pendingPulsePin = -1;
    }
}

static void holdRelay(int pin) {
    if (pin < 0) {
        return;
    }
    allRelaysOff();
    relayOn(pin);
    heldPin = pin;
}

static bool tokenOk() {
    if (API_TOKEN == nullptr || API_TOKEN[0] == '\0') {
        return true;
    }
    if (server.hasHeader("X-Token") && server.header("X-Token") == API_TOKEN) {
        return true;
    }
    if (server.hasArg("token") && server.arg("token") == API_TOKEN) {
        return true;
    }
    return false;
}

static void sendJson(int code, const String &body) {
    server.sendHeader("Connection", "close");
    server.send(code, "application/json", body);
    delay(1);
    server.client().flush();
    server.client().stop();
}

static void sendJson(int code, const char *body) {
    sendJson(code, String(body));
}

static void requireAuthOrAbort() {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
}

static String statusJson() {
    String ip = ethConnected ? ETH.localIP().toString() : String("0.0.0.0");
    String body;
    body.reserve(256);
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
    body += ",\"stop\":"; body += String(PIN_STOP); body += "},";
    body += "\"held_pin\":"; body += String(heldPin); body += ",";
    body += "\"last_command\":\""; body += lastCommand; body += "\",";
    body += "\"last_command_age_ms\":"; body += String(lastCommandMs == 0 ? 0 : (millis() - lastCommandMs));
    body += "}";
    return body;
}

static void remember(const char *cmd) {
    lastCommand = cmd;
    lastCommandMs = millis();
}

static void runDeferredCommand() {
    if (deferredCommand == 0) {
        return;
    }
    const int cmd = deferredCommand;
    deferredCommand = 0;
    Serial.printf("[CMD] run %d (hold_mode=%d)\n", cmd, HOLD_MODE ? 1 : 0);

    if (cmd == 1) {
        // /open — замкнуть и держать Start (PIN_OPEN)
        if (HOLD_MODE) {
            holdRelay(PIN_OPEN);
        } else {
            schedulePulse(PIN_OPEN);
        }
    } else if (cmd == 2) {
        // /close — снять удержание (разомкнуть Start).
        // Отдельное реле CLOSE (IO14) только если HOLD_MODE=false и пин разведён.
        if (HOLD_MODE) {
            Serial.println("[CMD] release hold (close)");
            allRelaysOff();
        } else if (PIN_CLOSE >= 0) {
            schedulePulse(PIN_CLOSE);
        } else {
            allRelaysOff();
        }
    } else if (cmd == 3) {
        // /stop — всегда снять все контакты
        Serial.println("[CMD] stop / all off");
        allRelaysOff();
    }
}

static void handleRoot() {
    sendJson(200, statusJson());
}

static void handleStatus() {
    if (!tokenOk()) {
        requireAuthOrAbort();
        return;
    }
    sendJson(200, statusJson());
}

static void handleOpen() {
    if (!tokenOk()) {
        requireAuthOrAbort();
        return;
    }
    remember("open");
    Serial.println("[CMD] /open accepted (hold)");
    sendJson(200, "{\"ok\":true,\"command\":\"open\",\"mode\":\"hold\"}");
    deferredCommand = 1;
}

static void handleClose() {
    if (!tokenOk()) {
        requireAuthOrAbort();
        return;
    }
    remember("close");
    Serial.println("[CMD] /close accepted (release)");
    sendJson(200, "{\"ok\":true,\"command\":\"close\",\"mode\":\"release\"}");
    deferredCommand = 2;
}

static void handleStop() {
    if (!tokenOk()) {
        requireAuthOrAbort();
        return;
    }
    remember("stop");
    Serial.println("[CMD] /stop accepted (release)");
    sendJson(200, "{\"ok\":true,\"command\":\"stop\",\"mode\":\"release\"}");
    deferredCommand = 3;
}

static void handleNotFound() {
    sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}");
}

static void startOtaAfterIp() {
    if (otaReady) {
        return;
    }
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPort(3232);
    // Без пароля — как в IDE по умолчанию. При необходимости:
    // ArduinoOTA.setPassword("ota-secret");
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
    Serial.println(":3232 (Arduino → сетевой порт; firewall UDP 3232)");
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
            Serial.println("[ETH] link up (кабель/линк есть)");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ethConnected = true;
            Serial.print("[ETH] IP: ");
            Serial.println(ETH.localIP());
            Serial.print("[ETH] MAC: ");
            Serial.println(ETH.macAddress());
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
            Serial.println("[ETH] link up (кабель/линк есть)");
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
    if (PIN_OPEN >= 0) {
        pinMode(PIN_OPEN, OUTPUT);
    }
    if (PIN_CLOSE >= 0) {
        pinMode(PIN_CLOSE, OUTPUT);
    }
    if (PIN_STOP >= 0) {
        pinMode(PIN_STOP, OUTPUT);
    }
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
    Serial.println("================================");

    setupRelays();

    // На WT32-ETH01 IO0 = 50 МГц clock Ethernet. Любая перемычка IO0-GND
    // после прошивки убивает link. Питание лучше с 5V.
    Serial.println("[ETH] init... IO0 must be FREE (not to GND)");

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    Network.onEvent(onNetworkEvent);
    // Вариант платы WT32-ETH01 уже задаёт PHY в pins_arduino.h
    bool ethOk = ETH.begin();
#else
    WiFi.onEvent(onEthEvent);
    bool ethOk = ETH.begin(1, 16, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
#endif
    Serial.println(ethOk ? "[ETH] begin() OK" : "[ETH] begin() FAILED");

    setupHttp();
    // OTA стартует только после ETH GOT_IP (см. startOtaAfterIp)

    Serial.println("[HINT] waiting for link: [ETH] link up → [ETH] IP: ...");
    Serial.println("[HINT] if no link in 10s: check cable, IO0 free, 5V power, switch port");
}

void loop() {
    server.handleClient();
    runDeferredCommand();
    if (otaReady) {
        ArduinoOTA.handle();
    }
    servicePendingPulse();

    static uint32_t lastDiagMs = 0;
    if (millis() - lastDiagMs > 5000) {
        lastDiagMs = millis();
        const bool up = ETH.linkUp();
        if (!up) {
            ethConnected = false;
        }
        Serial.printf("[ETH] linkUp=%s connected=%s ip=%s\n", up ? "yes" : "no", ethConnected ? "yes" : "no", ETH.localIP().toString().c_str());
        if (!up) {
            Serial.println("[ETH] link down — кабель/порт/питание 5V");
        }
    }
}
