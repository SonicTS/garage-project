// firmware.ino - ESP8266 sketch for polling garage-controller
// -----------------------------------------------------------
// PURPOSE:
//   Poll a remote HTTPS endpoint for a command. When cmd == "open", pulse a relay GPIO.
//
// TODOs (fill these before flashing):
const char* WIFI_SSID = "YOUR_WIFI_SSID";              // TODO: Change
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";          // TODO: Change
const char* SERVER_HOST = "example.ddns.net";          // TODO: Replace with <DOMAIN_OR_DDNS>
const int   SERVER_PORT = 443;                         // Use 443 for HTTPS (preferred)
const char* DEVICE_ID = "brother";                     // TODO: Must match config.yml
const char* DEVICE_TOKEN = "CHANGE_ME_LONG_RANDOM";    // TODO: Long random token from config.yml
const int   POLL_INTERVAL_MS = 5000;                   // Poll every 5 seconds (adjust as needed)
const int   RELAY_PIN = D1;                            // TODO: Adjust to your board/wiring

// TLS / security notes:
//   For production-like security, validate server certificate with fingerprint or CA.
//   Many prototypes use WiFiClientSecure with setInsecure(), which skips validation (NOT recommended).

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
//#include <ArduinoJson.h> // Optional for parsing JSON (add via Library Manager). For now we use placeholder parsing.

WiFiClientSecure client;  // Secure client for HTTPS.
unsigned long lastPoll = 0;

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("[garage] Booting...");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Assume LOW = idle (adjust if relay needs HIGH).

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] Connecting to "); Serial.println(WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[wifi] Connected. IP: "); Serial.println(WiFi.localIP());

  // WARNING: Skipping certificate validation for simplicity.
  // TODO: Implement proper certificate/fingerprint pinning.
  client.setInsecure();
}

void loop() {
  unsigned long now = millis();
  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    pollServer();
  }
  // Do other housekeeping tasks here if needed.
}

void pollServer() {
  Serial.println("[poll] Checking for command...");
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println("[poll] Connection failed.");
    return;
  }

  // Construct HTTP GET request.
  String url = "/garage/poll?device=" + String(DEVICE_ID) + "&token=" + String(DEVICE_TOKEN);
  client.println("GET " + url + " HTTP/1.1");
  client.println("Host: " + String(SERVER_HOST));
  client.println("Connection: close");
  client.println();

  // Read response (very naive parsing for skeleton).
  bool headersEnded = false;
  String body;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (!headersEnded) {
      if (line == "\r") {
        headersEnded = true; // End of headers.
      }
    } else {
      body += line + "\n"; // Accumulate body lines.
    }
  }
  client.stop();

  // TODO: Parse JSON properly. Expecting something like {"cmd":"open"} or {"cmd":"idle"}.
  // Placeholder: simple substring search.
  if (body.indexOf("\"cmd\": \"open\"") >= 0 || body.indexOf("\"cmd\":\"open\"") >= 0) {
    Serial.println("[poll] OPEN command received. Pulsing relay...");
    pulseRelay();
  } else {
    Serial.println("[poll] Idle.");
  }
}

void pulseRelay() {
  // TODO: Adjust logic if relay is active LOW vs active HIGH.
  digitalWrite(RELAY_PIN, HIGH);
  delay(500); // 500 ms pulse
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("[relay] Pulse complete.");
}
