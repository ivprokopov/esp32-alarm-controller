#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

const char* WIFI_SSID = "ALARM_LOCAL";
const char* WIFI_PASSWORD = "change_me_123456";

WebServer server(80);

String alarmMode = "DISARMED";
bool zone1Open = false;
bool zone2Open = false;

void handleRoot() {
  String html = "";
  html += "<!doctype html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ESP32 Alarm</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#111;color:#eee;margin:20px;}";
  html += ".card{background:#222;padding:16px;border-radius:12px;margin-bottom:12px;}";
  html += "button{padding:12px 16px;margin:6px;border:0;border-radius:8px;font-size:16px;}";
  html += ".ok{color:#00e676}.bad{color:#ff5252}.warn{color:#ffd740}";
  html += "</style>";
  html += "</head><body>";

  html += "<h2>ESP32-C6 Alarm Controller</h2>";

  html += "<div class='card'>";
  html += "<b>Alarm mode:</b> <span class='warn'>" + alarmMode + "</span><br>";
  html += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
  html += "<b>Wi-Fi RSSI:</b> " + String(WiFi.RSSI()) + " dBm";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>Zones</h3>";
  html += "Zone 1: ";
  html += zone1Open ? "<span class='bad'>OPEN</span><br>" : "<span class='ok'>CLOSED</span><br>";
  html += "Zone 2: ";
  html += zone2Open ? "<span class='bad'>OPEN</span><br>" : "<span class='ok'>CLOSED</span><br>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<button onclick=\"location.href='/arm'\">ARM</button>";
  html += "<button onclick=\"location.href='/disarm'\">DISARM</button>";
  html += "<button onclick=\"location.href='/zone?id=1&state=open'\">Zone 1 OPEN</button>";
  html += "<button onclick=\"location.href='/zone?id=1&state=closed'\">Zone 1 CLOSED</button>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>Shelly webhook examples</h3>";
  html += "<p>/zone?id=1&state=open</p>";
  html += "<p>/zone?id=1&state=closed</p>";
  html += "<p>/zone?id=2&state=open</p>";
  html += "<p>/zone?id=2&state=closed</p>";
  html += "</div>";

  html += "</body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleStatus() {
  String json = "{";
  json += "\"mode\":\"" + alarmMode + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"zone1\":\"" + String(zone1Open ? "open" : "closed") + "\",";
  json += "\"zone2\":\"" + String(zone2Open ? "open" : "closed") + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleArm() {
  alarmMode = "ARMED";
  Serial.println("[ALARM] Armed");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirect");
}

void handleDisarm() {
  alarmMode = "DISARMED";
  Serial.println("[ALARM] Disarmed");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirect");
}

void handleZone() {
  if (!server.hasArg("id") || !server.hasArg("state")) {
    server.send(400, "text/plain", "Missing id or state");
    return;
  }

  int id = server.arg("id").toInt();
  String state = server.arg("state");

  bool isOpen = (state == "open" || state == "1" || state == "on" || state == "true");

  if (id == 1) {
    zone1Open = isOpen;
  } else if (id == 2) {
    zone2Open = isOpen;
  } else {
    server.send(400, "text/plain", "Invalid zone id");
    return;
  }

  Serial.print("[ZONE] Zone ");
  Serial.print(id);
  Serial.print(" -> ");
  Serial.println(isOpen ? "OPEN" : "CLOSED");

  if (alarmMode == "ARMED" && isOpen) {
    alarmMode = "ALARM";
    Serial.println("[ALARM] Triggered");
  }

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Redirect");
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WiFi] Connecting");

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Failed. Starting setup AP.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-ALARM-SETUP", "setup123456");

    Serial.print("[WiFi] AP IP: ");
    Serial.println(WiFi.softAPIP());
  }
}

void setupServer() {
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/arm", handleArm);
  server.on("/disarm", handleDisarm);
  server.on("/zone", handleZone);

  server.begin();
  Serial.println("[HTTP] Server started");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("[SYSTEM] ESP32-C6 Alarm Controller booting...");

  setupWiFi();
  setupServer();

  Serial.println("[SYSTEM] Ready");
}

void loop() {
  server.handleClient();
}
