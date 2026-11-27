/*
  SMARTFLOOD - ESP32 water-level monitor
  Reads a water sensor, shows NORMAL / WARNING / DANGER on LEDs + buzzer,
  and POSTs JSON to the Django API:

    POST  <SERVER_URL>            (default: http://<PC-IP>:8000/api/sensor-data/)
    Header: X-API-Key: <API_KEY>   (the key of this device, from the Manage Devices page)
    Body:   {"sensor_value": 450, "water_status": "WARNING"}

  SETTINGS PORTAL (change Wi-Fi / server URL / API key WITHOUT re-flashing):
    The ESP32 opens its own hotspot  "SmartFloodSetUp"  (password: AP_PASSWORD below)
    - any time you type  setup  in the Serial Monitor message box and press Send/Enter
      (set the Serial Monitor line ending to "Newline" or "Both NL & CR", baud 115200)
    - automatically when no Wi-Fi has been saved yet, or Wi-Fi cannot connect at boot
    - any time you HOLD the BOOT button (GPIO 0) for 3 seconds
    Connect a phone/PC to that hotspot; the setup page opens by itself (or browse to
    http://192.168.4.1). Settings are saved in flash and survive power loss.
    The sensor, LEDs and buzzer keep working while the portal is open.

  Board: any "ESP32 Dev Module". No extra libraries needed
  (WiFi, HTTPClient, WiFiClientSecure, WebServer, DNSServer, Preferences come with the ESP32 Arduino core).

  WIRING (3.3 V logic everywhere):
    Water sensor  VCC -> 3V3   (NOT 5V: its AO output could then exceed 3.3 V and damage GPIO 34)
    Water sensor  GND -> GND
    Water sensor  AO  -> GPIO 34 (ADC1, input-only, works while Wi-Fi is on)
    Green  LED anode -> 220 ohm -> GPIO 25, cathode -> GND
    Yellow LED anode -> 220 ohm -> GPIO 26, cathode -> GND
    Red    LED anode -> 220 ohm -> GPIO 27, cathode -> GND
    Buzzer (active, bare 2-pin type) via NPN transistor (2N2222 / BC547 / S8050):
      GPIO 14 -> 1k ohm -> transistor BASE
      transistor EMITTER -> GND
      transistor COLLECTOR -> buzzer "-"
      buzzer "+" -> 3V3  (or 5V/VIN if it is a 5 V buzzer; GPIO only drives the base so it stays safe)
    If your buzzer is a 3-pin module with its own transistor (VCC, GND, I/O),
    connect VCC -> 3V3, GND -> GND, I/O -> GPIO 14 directly.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ========================= CONFIGURATION =========================
// The three values below are only DEFAULTS. Anything saved through the setup portal
// (stored in flash) overrides them, so you normally never need to edit them.
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Local test:   http://192.168.1.50:8000/api/sensor-data/   (your PC's LAN IP, NOT localhost)
// PythonAnywhere: https://yourusername.pythonanywhere.com/api/sensor-data/
const char* SERVER_URL = "http://192.168.1.50:8000/api/sensor-data/";

// Paste THIS DEVICE's key from the website: log in > Manage Devices > Add device > copy its API key.
// Every ESP32 has its own key.
const char* API_KEY = "PASTE-DEVICE-API-KEY-HERE";

const unsigned long SEND_INTERVAL_MS = 5000;   // how often to POST a reading
const unsigned long SAMPLE_INTERVAL_MS = 200;  // how often to read sensor / update LEDs
const unsigned long WIFI_RETRY_MS = 10000;     // wait between reconnect attempts
const unsigned long WIFI_FIRST_TIMEOUT_MS = 20000;
const unsigned long HTTP_TIMEOUT_MS = 8000;

// ---- setup portal (ESP32 hotspot) ----
const char* AP_SSID     = "SmartFloodSetUp";          // hotspot name shown in your phone/PC Wi-Fi list
const char* AP_PASSWORD = "smartflood123";            // hotspot password, 8+ characters. CHANGE IT.
const int   PIN_PORTAL_BUTTON = 0;                    // BOOT button on most ESP32 dev boards
const unsigned long PORTAL_HOLD_MS = 3000;            // hold BOOT this long to open the portal
const unsigned long PORTAL_IDLE_TIMEOUT_MS = 600000;  // close hotspot after 10 min unused (only if already configured)

// Thresholds are RAW ADC values (0-4095). CALIBRATE THEM - see README.
const int WARNING_THRESHOLD = 1200;
const int DANGER_THRESHOLD  = 2400;
const int HYSTERESIS        = 100;  // prevents flickering around a threshold

const int ADC_SAMPLES = 10;         // readings averaged per sample
const bool STARTUP_SELFTEST = true; // blink all LEDs + beep once at power-up
// ==============================================================================

// ------------------------------- pins ---------------------------------------
const int PIN_SENSOR = 34;
const int PIN_GREEN  = 25;
const int PIN_YELLOW = 26;
const int PIN_RED    = 27;
const int PIN_BUZZER = 14;

enum WaterStatus { NORMAL, WARNING, DANGER };
const char* STATUS_TEXT[] = { "NORMAL", "WARNING", "DANGER" };

WaterStatus currentStatus = NORMAL;
int currentValue = 0;
unsigned long lastSample = 0, lastSend = 0, lastWifiTry = 0, lastBeep = 0;
bool buzzerOn = false;
unsigned long okCount = 0, failCount = 0;

// ---- runtime settings (loaded from flash, see loadConfig) ----
String cfgSsid, cfgPass, cfgUrl, cfgKey;
int lastHttpCode = 0;   // last POST result, shown on the portal page

// ---- portal state ----
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool portalActive = false;
String apName;
unsigned long portalLastActivity = 0, buttonDownSince = 0, rebootAt = 0;

// ------------------------------- sensor -------------------------------------
int readSensorAveraged() {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(PIN_SENSOR);
    delay(2);
  }
  return (int)(sum / ADC_SAMPLES);
}

// Uses the previous status so going DOWN needs HYSTERESIS extra margin.
WaterStatus classify(int v) {
  if (v >= DANGER_THRESHOLD) return DANGER;
  if (currentStatus == DANGER && v >= DANGER_THRESHOLD - HYSTERESIS) return DANGER;
  if (v >= WARNING_THRESHOLD) return WARNING;
  if (currentStatus != NORMAL && v >= WARNING_THRESHOLD - HYSTERESIS) return WARNING;
  return NORMAL;
}

// ------------------------------- outputs ------------------------------------
void applyOutputs() {
  digitalWrite(PIN_GREEN,  currentStatus == NORMAL  ? HIGH : LOW);
  digitalWrite(PIN_YELLOW, currentStatus == WARNING ? HIGH : LOW);
  digitalWrite(PIN_RED,    currentStatus == DANGER  ? HIGH : LOW);
  if (currentStatus != DANGER) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerOn = false;
  }
}

// Non-blocking beeping (300 ms on / 300 ms off) while DANGER.
void updateBuzzer() {
  if (currentStatus != DANGER) return;
  if (millis() - lastBeep >= 300) {
    lastBeep = millis();
    buzzerOn = !buzzerOn;
    digitalWrite(PIN_BUZZER, buzzerOn ? HIGH : LOW);
  }
}

void selfTest() {
  Serial.println("Self-test: LEDs and buzzer...");
  const int leds[] = { PIN_GREEN, PIN_YELLOW, PIN_RED };
  for (int i = 0; i < 3; i++) {
    digitalWrite(leds[i], HIGH);
    delay(300);
    digitalWrite(leds[i], LOW);
  }
  digitalWrite(PIN_BUZZER, HIGH);
  delay(150);
  digitalWrite(PIN_BUZZER, LOW);
}


// ------------------------------- settings storage ---------------------------
bool isConfigured() {
  return cfgSsid.length() > 0 && cfgSsid != "YOUR_WIFI_NAME";
}

void loadConfig() {
  prefs.begin("smartflood", true);   // read-only
  cfgSsid = prefs.getString("ssid", WIFI_SSID);
  cfgPass = prefs.getString("pass", WIFI_PASSWORD);
  cfgUrl  = prefs.getString("url",  SERVER_URL);
  cfgKey  = prefs.getString("key",  API_KEY);
  prefs.end();
}

void saveConfig() {
  prefs.begin("smartflood", false);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("url",  cfgUrl);
  prefs.putString("key",  cfgKey);
  prefs.end();
}

void clearConfig() {
  prefs.begin("smartflood", false);
  prefs.clear();
  prefs.end();
}

// ------------------------------- setup portal -------------------------------
String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String jsonEscape(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

String maskedKey() {
  if (cfgKey.length() == 0 || cfgKey == "PASTE-DEVICE-API-KEY-HERE") return "(not set)";
  if (cfgKey.length() <= 4) return "****";
  return "****" + cfgKey.substring(cfgKey.length() - 4);
}

void portalTouch() { portalLastActivity = millis(); }

void sendPage(const String& title, const String& body) {
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>SMARTFLOOD Setup</title><style>"
               "body{font-family:system-ui,Arial,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}"
               ".c{max-width:460px;margin:0 auto;background:#1e293b;border-radius:12px;padding:20px}"
               "h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:18px 0 6px;color:#38bdf8}"
               "p,small{color:#94a3b8}label{display:block;margin:12px 0 4px;font-size:14px}"
               "input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #475569;"
               "background:#0f172a;color:#e2e8f0;font-size:16px}"
               "button{width:100%;margin-top:16px;padding:12px;border:0;border-radius:8px;background:#0ea5e9;"
               "color:#fff;font-size:16px;font-weight:600}button.d{background:#b91c1c}button.s{background:#475569;margin-top:8px}"
               ".ok{color:#4ade80}.bad{color:#f87171}table{width:100%;font-size:14px}td:first-child{color:#94a3b8;width:42%}"
               "</style></head><body><div class='c'><h1>SMARTFLOOD ESP32 Setup</h1><p>");
  h += title;
  h += F("</p>");
  h += body;
  h += F("</div></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", h);
}

void handleRoot() {
  portalTouch();
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  String b;
  b += F("<h2>Status</h2><table>");
  b += "<tr><td>Wi-Fi</td><td class='" + String(wifiOk ? "ok'>connected to " + htmlEscape(WiFi.SSID()) : "bad'>not connected") + "</td></tr>";
  if (wifiOk) b += "<tr><td>ESP32 IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  b += "<tr><td>Last server reply</td><td>" + (lastHttpCode == 0 ? String("none yet") : String(lastHttpCode)) + "</td></tr>";
  b += "<tr><td>Sensor now</td><td>" + String(currentValue) + " (" + STATUS_TEXT[currentStatus] + ")</td></tr>";
  b += "<tr><td>Sent OK / failed</td><td>" + String(okCount) + " / " + String(failCount) + "</td></tr></table>";

  b += F("<form method='POST' action='/save' autocomplete='off'><h2>Wi-Fi</h2>"
         "<label>Nearby networks (2.4 GHz)</label><select id='nets' onchange=\"document.getElementById('ssid').value=this.value\">"
         "<option value=''>Scanning...</option></select>"
         "<label>Network name (SSID)</label><input id='ssid' name='ssid' maxlength='32' required value='");
  b += htmlEscape(cfgSsid);
  b += F("'><label>Wi-Fi password</label><input id='pass' name='pass' type='password' maxlength='63' placeholder='");
  b += cfgPass.length() ? F("leave blank to keep the saved password") : F("blank = open network");
  b += F("'><small><input type='checkbox' style='width:auto' onclick=\"document.getElementById('pass').type=this.checked?'text':'password'\"> show password</small>"
         "<h2>Server</h2><label>Server URL</label><input name='url' maxlength='200' required placeholder='http://192.168.1.50:8000/api/sensor-data/' value='");
  b += htmlEscape(cfgUrl);
  b += F("'><label>Device API key</label><input name='key' maxlength='100' placeholder='");
  b += "current: " + htmlEscape(maskedKey()) + " (blank = keep)";
  b += F("'><button type='submit'>Save &amp; restart</button></form>"
         "<form method='POST' action='/reset' onsubmit=\"return confirm('Erase saved settings and restart?')\">"
         "<button class='d' type='submit'>Erase saved settings</button></form>"
         "<p><small>Saved settings are kept in the ESP32's flash. If the Wi-Fi password is wrong the ESP32 reopens this hotspot "
         "after restart. Or hold BOOT for 3 seconds at any time.</small></p>"
         "<script>function ld(){fetch('/networks').then(r=>r.json()).then(j=>{var s=document.getElementById('nets');"
         "if(j.scanning){setTimeout(ld,1500);return}s.innerHTML='<option value=\"\">'+(j.n.length?'-- choose --':'none found')+'</option>';"
         "j.n.forEach(function(n){var o=document.createElement('option');o.value=n.s;o.textContent=n.s+'  ('+n.r+' dBm)';s.appendChild(o)})})"
         ".catch(function(){})}ld();</script>");
  sendPage("Change Wi-Fi, server URL or API key.", b);
}

void handleNetworks() {
  portalTouch();
  int n = WiFi.scanComplete();
  String out;
  if (n == WIFI_SCAN_RUNNING) {
    out = F("{\"scanning\":true,\"n\":[]}");
  } else {
    if (n < 0) { WiFi.scanNetworks(true); out = F("{\"scanning\":true,\"n\":[]}"); }
    else {
      out = F("{\"scanning\":false,\"n\":[");
      bool first = true;
      for (int i = 0; i < n && i < 20; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"s\":\"" + jsonEscape(ssid) + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
      }
      out += "]}";
      WiFi.scanDelete();   // next request triggers a fresh scan
    }
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleSave() {
  portalTouch();
  String ssid = server.arg("ssid");  ssid.trim();
  String pass = server.arg("pass");
  String url  = server.arg("url");   url.trim();
  String key  = server.arg("key");   key.trim();

  String err;
  if (ssid.length() == 0 || ssid.length() > 32) err = "Wi-Fi name must be 1-32 characters.";
  else if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) err = "Wi-Fi password must be 8-63 characters (or blank).";
  else if (!(url.startsWith("http://") || url.startsWith("https://")) || url.length() < 12) err = "Server URL must start with http:// or https://";
  if (err.length()) {
    sendPage("<span class='bad'>Not saved.</span>", "<p class='bad'>" + htmlEscape(err) + "</p><button class='s' onclick='history.back()'>Go back</button>");
    return;
  }

  // Blank password keeps the old one only when the network name is unchanged;
  // a NEW network with a blank password is treated as an open network.
  if (pass.length() == 0 && ssid == cfgSsid) pass = cfgPass;
  if (key.length() == 0) key = cfgKey;

  cfgSsid = ssid; cfgPass = pass; cfgUrl = url; cfgKey = key;
  saveConfig();
  Serial.printf("Portal: saved settings (SSID \"%s\", URL %s). Restarting...\n", cfgSsid.c_str(), cfgUrl.c_str());
  sendPage("<span class='ok'>Saved!</span>", F("<p>The ESP32 is restarting and will join your Wi-Fi. This hotspot will close. "
           "If the connection fails, the hotspot reappears so you can fix the settings.</p>"));
  rebootAt = millis() + 1500;
}

void handleReset() {
  portalTouch();
  clearConfig();
  Serial.println("Portal: saved settings erased. Restarting...");
  sendPage("<span class='ok'>Erased.</span>", F("<p>Defaults restored. The ESP32 is restarting and will reopen this hotspot for setup.</p>"));
  rebootAt = millis() + 1500;
}

// Captive-portal: any unknown URL (phone connectivity checks etc.) goes to the setup page.
void handleNotFound() {
  portalTouch();
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void startPortal() {
  if (portalActive) return;
  apName = AP_SSID;

  WiFi.mode(WIFI_AP_STA);   // keep the station side so Wi-Fi retries continue
  if (!WiFi.softAP(apName.c_str(), AP_PASSWORD)) {
    Serial.println("Portal: could not start the hotspot.");
    return;
  }
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.begin();
  WiFi.scanNetworks(true);  // async, results appear on the page
  portalActive = true;
  portalTouch();
  Serial.println("=== SETUP PORTAL OPEN ===");
  Serial.printf("  Hotspot : %s   Password: %s\n", apName.c_str(), AP_PASSWORD);
  Serial.printf("  Browse  : http://%s\n", WiFi.softAPIP().toString().c_str());
}

void stopPortal() {
  if (!portalActive) return;
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive = false;
  Serial.println("Setup portal closed.");
}

void setupPortalRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/networks", HTTP_GET, handleNetworks);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
}

// Called every loop: serves the portal, watches the BOOT button, handles restart / idle timeout.
void handlePortal() {
  // BOOT button held -> open portal
  if (digitalRead(PIN_PORTAL_BUTTON) == LOW) {
    if (buttonDownSince == 0) buttonDownSince = millis();
    else if (!portalActive && millis() - buttonDownSince >= PORTAL_HOLD_MS) {
      Serial.println("BOOT held: opening setup portal.");
      startPortal();
    }
  } else {
    buttonDownSince = 0;
  }

  if (portalActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (WiFi.softAPgetStationNum() > 0) portalTouch();   // someone is connected: not idle
    if (isConfigured() && rebootAt == 0 && millis() - portalLastActivity > PORTAL_IDLE_TIMEOUT_MS) stopPortal();
  }

  if (rebootAt != 0 && (long)(millis() - rebootAt) >= 0) {
    server.client().stop();
    delay(100);
    ESP.restart();
  }
}

// Serial Monitor commands: type  setup  (then Send) to open the SmartFloodSetUp hotspot.
String serialLine;
void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      serialLine.trim();
      serialLine.toLowerCase();
      if (serialLine == "setup") {
        if (portalActive) {
          portalTouch();
          Serial.println("Setup hotspot is already open:");
          Serial.printf("  Hotspot : %s   Password: %s\n", apName.c_str(), AP_PASSWORD);
          Serial.printf("  Browse  : http://%s\n", WiFi.softAPIP().toString().c_str());
        } else {
          Serial.println("Command 'setup' received: opening hotspot...");
          startPortal();
        }
      } else if (serialLine.length() > 0) {
        Serial.printf("Unknown command \"%s\". Type  setup  to open the %s hotspot.\n", serialLine.c_str(), AP_SSID);
      }
      serialLine = "";
    } else if (serialLine.length() < 32) {
      serialLine += c;
    }
  }
}

// ------------------------------- Wi-Fi --------------------------------------
void startWifi() {
  WiFi.mode(portalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  lastWifiTry = millis();
}

void connectWifiAtBoot() {
  Serial.printf("Connecting to Wi-Fi \"%s\"", cfgSsid.c_str());
  startWifi();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_FIRST_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi connected. ESP32 IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi not connected yet. Will keep trying in the background.");
  }
}

// Called every loop: retries without blocking the LEDs/buzzer.
void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry >= WIFI_RETRY_MS) {
    Serial.println("Wi-Fi lost. Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    lastWifiTry = millis();
  }
}

// ------------------------------- HTTP ---------------------------------------
bool sendReading(int value, WaterStatus status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Send skipped: Wi-Fi not connected.");
    return false;
  }
  if (cfgUrl.length() == 0) {
    Serial.println("Send skipped: no server URL set (open the setup portal).");
    return false;
  }

  char json[80];
  snprintf(json, sizeof(json), "{\"sensor_value\": %d, \"water_status\": \"%s\"}", value, STATUS_TEXT[status]);

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  bool began;
  if (cfgUrl.startsWith("https://")) {
    secureClient.setInsecure();  // encrypts, but does not verify the server certificate (see README)
    began = http.begin(secureClient, cfgUrl);
  } else {
    began = http.begin(plainClient, cfgUrl);
  }
  if (!began) {
    Serial.println("HTTP begin() failed - check the server URL in the setup portal.");
    return false;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", cfgKey);

  int code = http.POST((uint8_t*)json, strlen(json));
  lastHttpCode = code;
  bool ok = (code == 200 || code == 201);
  if (code > 0) {
    String response = http.getString();
    Serial.printf("POST %s -> HTTP %d  %s\n", json, code, response.c_str());
    if (code == 401) Serial.println("  API key rejected: open the setup portal and paste this device's key from Manage Devices (or the device is disabled/deleted).");
    if (code == 400) Serial.println("  Server rejected the data (field names/values).");
  } else {
    Serial.printf("POST failed: %s (code %d)\n", http.errorToString(code).c_str(), code);
  }
  http.end();
  return ok;
}

// ------------------------------- Arduino ------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== SMARTFLOOD ESP32 ===");

  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_YELLOW, LOW);
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  pinMode(PIN_PORTAL_BUTTON, INPUT_PULLUP);
  loadConfig();
  setupPortalRoutes();
  Serial.printf("Tip: type  setup  in the Serial Monitor to open the \"%s\" hotspot (change Wi-Fi / server / API key).\n", AP_SSID);

  analogReadResolution(12);                        // 0..4095
  analogSetPinAttenuation(PIN_SENSOR, ADC_11db);   // input range up to about 3.1 V

  if (STARTUP_SELFTEST) selfTest();

  if (isConfigured()) {
    connectWifiAtBoot();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi failed at boot -> opening setup portal so you can fix the settings.");
      startPortal();
    }
  } else {
    Serial.println("No Wi-Fi saved yet -> opening setup portal.");
    startPortal();
  }
  currentValue = readSensorAveraged();
  currentStatus = classify(currentValue);
  applyOutputs();
}

void loop() {
  handleSerialCommands();
  handlePortal();
  if (isConfigured()) maintainWifi();
  updateBuzzer();

  unsigned long now = millis();
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    currentValue = readSensorAveraged();
    WaterStatus newStatus = classify(currentValue);
    if (newStatus != currentStatus) {
      currentStatus = newStatus;
      Serial.printf("Status changed -> %s\n", STATUS_TEXT[currentStatus]);
      applyOutputs();
    }
  }

  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    Serial.printf("Sensor: %d | Status: %s | Wi-Fi: %s | Sent OK: %lu | Failed: %lu\n",
                  currentValue, STATUS_TEXT[currentStatus],
                  WiFi.status() == WL_CONNECTED ? "connected" : "DISCONNECTED", okCount, failCount);
    if (sendReading(currentValue, currentStatus)) okCount++; else failCount++;
  }
}
