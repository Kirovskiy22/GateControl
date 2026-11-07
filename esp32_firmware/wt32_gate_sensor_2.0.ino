#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Выбираем GPIO 34 и 35 (работают только на вход, требуют внешних подтягивающих резисторов к 3.3V)
const int PIN_SENSOR_OPENED = 34;
const int PIN_SENSOR_CLOSED = 35;

// Пин для управления реле
const int PIN_RELAY = 4;

WebServer server(80);

void setup() {
  Serial.begin(115200);

  // Настраиваем пины датчиков на вход. Требуются внешние подтягивающие резисторы 10кОм к 3.3В
  pinMode(PIN_SENSOR_OPENED, INPUT);
  pinMode(PIN_SENSOR_CLOSED, INPUT);

  // Настраиваем пин реле
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());

  server.on("/status", HTTP_GET, []() {
    // Считываем значения пинов.
    // Поскольку у нас внешняя подтяжка к 3.3В, замыкание датчика на землю (GND) даст LOW.
    // Если датчик сработал (ворота дошли до него) - он замыкает цепь, и мы читаем LOW (0).
    // Если датчик не сработал - читаем HIGH (1).
    bool is_opened = (digitalRead(PIN_SENSOR_OPENED) == LOW);
    bool is_closed = (digitalRead(PIN_SENSOR_CLOSED) == LOW);

    String state = "MOVING";
    if (is_opened && is_closed) {
      state = "SENSOR_ERROR"; // Ворота не могут быть одновременно и открыты, и закрыты
    } else if (is_opened) {
      state = "OPENED";
    } else if (is_closed) {
      state = "CLOSED";
    }

    String json = "{\"ok\":true, \"gate_status\":\"" + state + "\", \"ip\":\"" + WiFi.localIP().toString() + "\"}";
    server.send(200, "application/json", json);
  });

  server.on("/open", HTTP_GET, []() {
    digitalWrite(PIN_RELAY, HIGH);
    delay(800);
    digitalWrite(PIN_RELAY, LOW);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/stop", HTTP_GET, []() {
    // В зависимости от привода STOP может быть просто еще одним импульсом на реле
    digitalWrite(PIN_RELAY, HIGH);
    delay(800);
    digitalWrite(PIN_RELAY, LOW);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
}

void loop() {
  server.handleClient();
}
