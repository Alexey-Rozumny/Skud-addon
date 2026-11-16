#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>

#define FW_VERSION "1.3"

#define PULSE_PIN 1
#define SENSOR_PIN 2
#define RESET_PIN 9
#define LED_PIN 8

#define PULSE_ACTIVE_LEVEL HIGH
#define PULSE_IDLE_LEVEL LOW
#define DOOR_OPEN_LEVEL HIGH
#define LED_ACTIVE_LEVEL LOW
#define LED_IDLE_LEVEL HIGH

const unsigned long DEFAULT_PULSE_MS = 2000UL;
const unsigned long DEBOUNCE_MS = 50UL;
const unsigned long REGISTER_INTERVAL_MS = 60000UL;
const unsigned long REGISTER_RETRY_MS = 15000UL;
const unsigned long EVENT_RETRY_MS = 2000UL;
const unsigned long SERVER_OFFLINE_MS = 180000UL;
const unsigned long REMOTE_WINDOW_MS = 5000UL;
const unsigned long WIFI_RETRY_MS = 10000UL;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 60000UL;
const unsigned long RESET_HOLD_MS = 5000UL;
const unsigned long FACTORY_RESET_HOLD_MS = 10000UL;
const unsigned long LED_FAST_MS = 100UL;
const unsigned long LED_SLOW_MS = 1000UL;
const int HTTP_TIMEOUT_MS = 3000;
const int EVENT_QUEUE_SIZE = 10;

struct PendingEvent {
  bool openState;
  bool remote;
};

WebServer server(80);
Preferences prefs;

String deviceSecret;
String serverHost;
String serverPort = "8081";
String sessionPhrase;
String authFailure;
String wifiSsid;
String wifiPass;

unsigned long phraseReceivedAt = 0;
unsigned long phraseTtlMs = 0;
unsigned long pulseDuration = DEFAULT_PULSE_MS;
unsigned long pulseStarted = 0;
unsigned long remoteWindowUntil = 0;
unsigned long lastRegisterAttempt = 0;
unsigned long lastEventAttempt = 0;
unsigned long lastSuccessMillis = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastLedToggle = 0;
unsigned long resetPressedAt = 0;

bool pulseActive = false;
bool resetPressed = false;
bool stableDoorOpen = false;
bool lastRawDoorOpen = false;
bool contactEnabled = true;
bool wifiProvisioned = false;
bool configMode = false;
bool ledState = false;
unsigned long lastRawChange = 0;

PendingEvent eventQueue[EVENT_QUEUE_SIZE];
int eventHead = 0;
int eventCount = 0;

const char *htmlPage =
R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Терминал СКУД</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; margin-top: 10vh; background-color: #f8f9fa; color: #333; }
    .card { background: white; max-width: 420px; margin: 0 auto; padding: 30px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
    h2 { margin-top: 0; color: #495057; }
    .status-row { display: flex; justify-content: space-between; align-items: center; padding: 14px 0; border-bottom: 1px solid #e9ecef; font-size: 18px; }
    .status-row:last-child { border-bottom: none; }
    .badge { padding: 8px 15px; border-radius: 20px; font-weight: bold; color: white; font-size: 15px; }
    .bg-green { background-color: #198754; }
    .bg-red { background-color: #dc3545; }
    .bg-gray { background-color: #6c757d; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Терминал СКУД</h2>
    <div class="status-row">
      <span>Состояние двери:</span>
      <span id="door_status" class="badge bg-gray">Опрос...</span>
    </div>
    <div class="status-row">
      <span>Связь с сервером:</span>
      <span id="server_status" class="badge bg-gray">Опрос...</span>
    </div>
    <div class="status-row">
      <span>Регистрация:</span>
      <span id="registered_status" class="badge bg-gray">Опрос...</span>
    </div>
    <div class="status-row">
      <span>Wi-Fi / IP:</span>
      <span id="wifi_status" class="badge bg-gray">Опрос...</span>
    </div>
  </div>
  <script>
    function updateStatus() {
      fetch('/status')
        .then(response => response.json())
        .then(data => {
          const doorEl = document.getElementById('door_status');
          if (!data.contact_enabled) { doorEl.textContent = 'КОНТАКТ ОТКЛЮЧЁН'; doorEl.className = 'badge bg-gray'; }
          else if (data.door_open) { doorEl.textContent = 'ОТКРЫТА'; doorEl.className = 'badge bg-red'; }
          else { doorEl.textContent = 'ЗАКРЫТА'; doorEl.className = 'badge bg-green'; }

          const srvEl = document.getElementById('server_status');
          if (data.server_ok) { srvEl.textContent = 'ОНЛАЙН'; srvEl.className = 'badge bg-green'; }
          else { srvEl.textContent = 'НЕТ СВЯЗИ'; srvEl.className = 'badge bg-red'; }

          const regEl = document.getElementById('registered_status');
          if (data.registered) { regEl.textContent = 'ЕСТЬ ФРАЗА'; regEl.className = 'badge bg-green'; }
          else { regEl.textContent = 'НЕТ ФРАЗЫ'; regEl.className = 'badge bg-red'; }

          const wifiEl = document.getElementById('wifi_status');
          if (data.ip && data.ip !== '0.0.0.0') { wifiEl.textContent = data.ip + ' / ' + data.rssi + ' dBm'; wifiEl.className = 'badge bg-green'; }
          else { wifiEl.textContent = 'НЕТ СЕТИ'; wifiEl.className = 'badge bg-red'; }
        }).catch(err => console.error(err));
    }
    setInterval(updateStatus, 2000);
    updateStatus();
  </script>
</body>
</html>
)rawliteral";

bool readDoorOpen() {
  return digitalRead(SENSOR_PIN) == DOOR_OPEN_LEVEL;
}

bool isServerOnline() {
  return lastSuccessMillis != 0 && (millis() - lastSuccessMillis) < SERVER_OFFLINE_MS;
}

String jsonString(const String &source, const char *key) {
  String pattern = String("\"") + key + "\":";
  int start = source.indexOf(pattern);
  if (start < 0) {
    return "";
  }
  start += pattern.length();
  while (start < (int)source.length() && (source[start] == ' ' || source[start] == '\t' || source[start] == '\r' || source[start] == '\n')) {
    start++;
  }
  if (start >= (int)source.length() || source[start] != '"') {
    return "";
  }
  start++;
  int end = source.indexOf('"', start);
  if (end < 0) {
    return "";
  }
  return source.substring(start, end);
}

long jsonNumber(const String &source, const char *key) {
  String pattern = String("\"") + key + "\":";
  int start = source.indexOf(pattern);
  if (start < 0) {
    return -1;
  }
  start += pattern.length();
  while (start < (int)source.length() && (source[start] == ' ' || source[start] == '\t')) {
    start++;
  }
  int end = start;
  if (end < (int)source.length() && source[end] == '-') {
    end++;
  }
  while (end < (int)source.length() && source[end] >= '0' && source[end] <= '9') {
    end++;
  }
  if (end == start) {
    return -1;
  }
  return source.substring(start, end).toInt();
}

void normalizeServerConfig() {
  deviceSecret.trim();
  serverHost.trim();
  serverPort.trim();

  if (serverHost.startsWith("http://")) {
    serverHost.remove(0, 7);
  } else if (serverHost.startsWith("https://")) {
    serverHost.remove(0, 8);
  }

  int slash = serverHost.indexOf('/');
  if (slash >= 0) {
    serverHost.remove(slash, serverHost.length() - slash);
  }

  if (serverPort.isEmpty()) {
    serverPort = "8081";
  }

  long portNumber = serverPort.toInt();
  if (portNumber <= 0 || portNumber > 65535) {
    serverPort = "8081";
  } else {
    serverPort = String(portNumber);
  }
}

void loadConfig() {
  prefs.begin("skud", true);
  deviceSecret = prefs.getString("secret", "");
  serverHost = prefs.getString("host", "");
  serverPort = prefs.getString("port", "8081");
  contactEnabled = prefs.getBool("contact", true);
  wifiProvisioned = prefs.getBool("wifi_set", false);
  wifiSsid = prefs.getString("wifi_ssid", "");
  wifiPass = prefs.getString("wifi_pass", "");
  prefs.end();

  normalizeServerConfig();
}

void saveConfig() {
  prefs.begin("skud", false);
  prefs.putString("secret", deviceSecret);
  prefs.putString("host", serverHost);
  prefs.putString("port", serverPort);
  prefs.putBool("contact", contactEnabled);
  prefs.end();
}

String htmlEscape(const String &value) {
  String result = value;
  result.replace("&", "&amp;");
  result.replace("<", "&lt;");
  result.replace(">", "&gt;");
  result.replace("\"", "&quot;");
  result.replace("'", "&#39;");
  return result;
}

String settingsPage(bool saved, bool authError) {
  String secretState = deviceSecret.isEmpty() ? "не задан" : "задан";
  String secretHint = deviceSecret.isEmpty()
                          ? "<div class=\"muted\">Секрет ещё не задан — введите его в поле «Новый секрет».</div>"
                          : "<div class=\"mb-3\"><label class=\"form-label\">Текущий секрет (обязателен для сохранения)</label><input type=\"password\" class=\"form-control\" name=\"current_secret\" required></div>";

  String notice;
  if (authError) {
    notice = "<div class=\"alert alert-danger py-2\">Неверный текущий секрет</div>";
  } else if (saved) {
    notice = "<div class=\"alert alert-success py-2\">Настройки сохранены</div>";
  }

  String page = "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"UTF-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  page += "<title>Настройки СКУД</title>";
  page += "<style>body{font-family:'Segoe UI',Tahoma,sans-serif;background:#f8f9fa;color:#333;margin:0;padding:24px}";
  page += ".card{background:#fff;max-width:460px;margin:0 auto;padding:24px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,.1)}";
  page += "h2{margin-top:0}.form-label{display:block;margin:12px 0 6px;font-weight:600}";
  page += ".form-control{width:100%;box-sizing:border-box;padding:9px;border:1px solid #ced4da;border-radius:6px}";
  page += ".btn{margin-top:16px;padding:10px 16px;border:0;border-radius:6px;background:#0d6efd;color:#fff;font-weight:600;cursor:pointer}";
  page += ".muted{color:#6c757d;font-size:14px}.row{display:flex;align-items:center;gap:8px;margin-top:14px}";
  page += "a{color:#0d6efd}</style></head><body><div class=\"card\"><h2>Настройки терминала</h2>";
  page += notice;
  page += "<form method=\"POST\" action=\"/settings\">";
  page += "<div class=\"mb-3\"><label class=\"form-label\">Сервер (host/IP)</label>";
  page += "<input class=\"form-control\" name=\"server\" value=\"" + htmlEscape(serverHost) + "\" placeholder=\"192.168.1.10\"></div>";
  page += "<div class=\"mb-3\"><label class=\"form-label\">Порт сервера</label>";
  page += "<input class=\"form-control\" name=\"port\" value=\"" + htmlEscape(serverPort) + "\" placeholder=\"8081\"></div>";
  page += "<div class=\"muted\">Секрет устройства: " + secretState + "</div>";
  page += secretHint;
  page += "<div class=\"mb-3\"><label class=\"form-label\">Новый секрет (необязательно)</label>";
  page += "<input type=\"password\" class=\"form-control\" name=\"new_secret\" placeholder=\"оставьте пустым, чтобы не менять\"></div>";
  if (!deviceSecret.isEmpty()) {
    page += "<div class=\"muted\">Чтобы изменить только сервер/IP, заполните поле «Текущий секрет», а «Новый секрет» оставьте пустым.</div>";
  }
  page += "<div class=\"row\"><input type=\"checkbox\" id=\"contact\" name=\"contact\" value=\"1\"";
  page += contactEnabled ? " checked" : "";
  page += "><label for=\"contact\">Использовать дверной контакт</label></div>";
  page += "<button class=\"btn\" type=\"submit\">Сохранить</button>";
  page += "</form><p class=\"muted\" style=\"margin-top:16px\"><a href=\"/\">Статус устройства</a></p>";
  page += "</div></body></html>";
  return page;
}

int httpPost(const String &path, const String &body, String &response) {
  response = "";
  if (WiFi.status() != WL_CONNECTED || serverHost.isEmpty() || deviceSecret.isEmpty()) {
    return -1;
  }

  String url = "http://" + serverHost + ":" + serverPort + path;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    return -1;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Api-Key", deviceSecret);

  int code = http.POST(body);
  if (code > 0) {
    response = http.getString();
  }
  http.end();
  return code;
}

bool registerDevice() {
  String body = String("{\"ip\":\"") + WiFi.localIP().toString() +
                "\",\"fw\":\"" + FW_VERSION + "\"" +
                ",\"contact\":" + (contactEnabled ? "true" : "false");

  if (contactEnabled) {
    body += String(",\"door_open\":") + (stableDoorOpen ? "true" : "false");
  }
  body += "}";

  String response;
  int code = httpPost("/api/register", body, response);
  if (code != 200) {
    Serial.print("register failed, HTTP ");
    Serial.print(code);
    Serial.print(": ");
    Serial.println(response);
    return false;
  }

  String phrase = jsonString(response, "phrase");
  if (phrase.isEmpty()) {
    Serial.print("register response without phrase: ");
    Serial.println(response);
    return false;
  }

  long ttl = jsonNumber(response, "ttl");
  long pulse = jsonNumber(response, "pulse_ms");

  sessionPhrase = phrase;
  phraseReceivedAt = millis();
  phraseTtlMs = (ttl > 0 ? (unsigned long)ttl : 3600UL) * 1000UL;

  if (pulse > 0) {
    pulseDuration = (unsigned long)pulse;
  }

  Serial.print("registered, phrase='");
  Serial.print(sessionPhrase);
  Serial.print("' ttl=");
  Serial.println(ttl);

  lastSuccessMillis = millis();
  return true;
}

void enqueueEvent(bool openState, bool remote) {
  if (eventCount >= EVENT_QUEUE_SIZE) {
    eventHead = (eventHead + 1) % EVENT_QUEUE_SIZE;
    eventCount--;
  }

  int index = (eventHead + eventCount) % EVENT_QUEUE_SIZE;
  eventQueue[index].openState = openState;
  eventQueue[index].remote = remote;
  eventCount++;
}

bool sendEvent(const PendingEvent &event) {
  String body = String("{\"open\":") + (event.openState ? "true" : "false") +
                ",\"trigger\":\"" + (event.remote ? "remote_pulse" : "local") + "\"}";

  String response;
  int code = httpPost("/api/event", body, response);
  if (code == 200) {
    lastSuccessMillis = millis();
    return true;
  }
  return false;
}

void flushEvents() {
  if (eventCount == 0 || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (millis() - lastEventAttempt < EVENT_RETRY_MS) {
    return;
  }

  lastEventAttempt = millis();
  while (eventCount > 0) {
    if (!sendEvent(eventQueue[eventHead])) {
      break;
    }
    eventHead = (eventHead + 1) % EVENT_QUEUE_SIZE;
    eventCount--;
  }
}

void pollDoor() {
  if (!contactEnabled) {
    return;
  }

  bool rawOpen = readDoorOpen();

  if (rawOpen != lastRawDoorOpen) {
    lastRawDoorOpen = rawOpen;
    lastRawChange = millis();
  }

  if ((millis() - lastRawChange) >= DEBOUNCE_MS && rawOpen != stableDoorOpen) {
    stableDoorOpen = rawOpen;

    bool remote = stableDoorOpen && (remoteWindowUntil != 0) && ((long)(millis() - remoteWindowUntil) < 0);
    enqueueEvent(stableDoorOpen, remote);

    if (stableDoorOpen && remote) {
      remoteWindowUntil = 0;
    }
  }
}

void startPulse() {
  digitalWrite(PULSE_PIN, PULSE_ACTIVE_LEVEL);
  pulseActive = true;
  pulseStarted = millis();
  remoteWindowUntil = millis() + pulseDuration + REMOTE_WINDOW_MS;
}

void updatePulse() {
  if (pulseActive && (millis() - pulseStarted) >= pulseDuration) {
    digitalWrite(PULSE_PIN, PULSE_IDLE_LEVEL);
    pulseActive = false;
  }
}

bool authorized() {
  authFailure = "";
  if (deviceSecret.isEmpty()) {
    authFailure = "no_secret";
  } else if (sessionPhrase.isEmpty()) {
    authFailure = "no_phrase";
  } else if ((millis() - phraseReceivedAt) > phraseTtlMs) {
    authFailure = "phrase_expired";
  } else {
    String key = server.header("X-Api-Key");
    String phrase = server.header("X-Phrase");
    key.trim();
    phrase.trim();
    String expected = deviceSecret;
    expected.trim();

    Serial.print("pulse headers: key_len=");
    Serial.print(key.length());
    Serial.print(" secret_len=");
    Serial.print(expected.length());
    Serial.print(" phrase_len=");
    Serial.println(phrase.length());

    if (key != expected) {
      authFailure = "secret";
    } else if (phrase != sessionPhrase) {
      authFailure = "phrase";
    }
  }

  if (authFailure.isEmpty()) {
    return true;
  }

  Serial.print("pulse denied: ");
  Serial.println(authFailure);
  return false;
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", htmlPage);
}

void handleStatus() {
  String json = String("{\"door_open\":") + ((contactEnabled && stableDoorOpen) ? "true" : "false") +
                ",\"contact_enabled\":" + (contactEnabled ? "true" : "false") +
                ",\"server_ok\":" + (isServerOnline() ? "true" : "false") +
                ",\"registered\":" + (!sessionPhrase.isEmpty() ? "true" : "false") +
                ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
                ",\"rssi\":" + String(WiFi.RSSI()) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handlePulse() {
  if (!authorized()) {
    String body = "Forbidden: " + authFailure;
    String key = server.header("X-Api-Key");
    key.trim();
    body += " key_len=" + String(key.length());
    body += " secret_len=" + String(deviceSecret.length());
    server.send(403, "text/plain", body);
    return;
  }

  Serial.println("pulse accepted");
  lastSuccessMillis = millis();
  startPulse();
  server.send(200, "text/plain", "OK");
}

void handleSettingsGet() {
  bool saved = server.hasArg("saved");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", settingsPage(saved, false));
}

void handleSettingsPost() {
  String provided = server.arg("current_secret");
  provided.trim();
  if (!deviceSecret.isEmpty() && provided != deviceSecret) {
    Serial.println("settings rejected: wrong current secret");
    server.sendHeader("Cache-Control", "no-store");
    server.send(403, "text/html", settingsPage(false, true));
    return;
  }

  serverHost = server.arg("server");
  serverPort = server.arg("port");
  String newSecret = server.arg("new_secret");
  newSecret.trim();

  normalizeServerConfig();

  if (!newSecret.isEmpty()) {
    deviceSecret = newSecret;
    normalizeServerConfig();
  }

  contactEnabled = server.hasArg("contact");

  if (contactEnabled) {
    stableDoorOpen = readDoorOpen();
    lastRawDoorOpen = stableDoorOpen;
    lastRawChange = millis();
  } else {
    stableDoorOpen = false;
  }

  saveConfig();

  Serial.print("settings saved: ");
  Serial.print(serverHost);
  Serial.print(":");
  Serial.print(serverPort);
  Serial.print(" secret=");
  Serial.println(deviceSecret.isEmpty() ? "not set" : "set");

  sessionPhrase = "";
  lastRegisterAttempt = millis() - REGISTER_RETRY_MS;

  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", "/settings?saved=1", true);
  server.send(303, "text/plain", "");
}

String configPortalPage(const String &error) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Настройка WiFi</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, sans-serif; max-width: 420px; margin: 40px auto; padding: 0 16px; background: #f8f9fa; color: #333; }
    .card { background: #fff; padding: 24px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,.1); }
    h2 { margin-top: 0; }
    label { display: block; margin-top: 16px; font-weight: 600; }
    input { width: 100%; padding: 10px; box-sizing: border-box; font-size: 16px; margin-top: 6px; }
    button { margin-top: 20px; width: 100%; padding: 12px; font-size: 16px; border: 0; border-radius: 8px; background: #0d6efd; color: #fff; }
    .error { color: #b00020; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Настройка WiFi</h2>
    <p>Терминал СКУД, ESP32-C3</p>
    <p class="error">%ERROR%</p>
    <form method="post" action="/save">
      <label>SSID</label>
      <input name="ssid" required maxlength="32" autocomplete="off">
      <label>Пароль</label>
      <input name="pass" type="password" maxlength="64">
      <button type="submit">Подключиться</button>
    </form>
  </div>
</body>
</html>
)rawliteral";
  html.replace("%ERROR%", error);
  return html;
}

String wifiSavedPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Сохранено</title>
  <style>body { font-family: sans-serif; text-align: center; margin-top: 20vh; color: #333; }</style>
</head>
<body>
  <h2>Настройки сохранены</h2>
  <p>Плата перезагружается и подключается к WiFi.</p>
</body>
</html>
)rawliteral";
}

void saveWifiConfig(const String &ssid, const String &password) {
  prefs.begin("skud", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", password);
  prefs.putBool("wifi_set", true);
  prefs.end();
  wifiSsid = ssid;
  wifiPass = password;
  wifiProvisioned = true;
}

void clearWifiConfig() {
  prefs.begin("skud", false);
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  prefs.remove("wifi_set");
  prefs.end();
  wifiSsid = "";
  wifiPass = "";
  wifiProvisioned = false;
}

void updateLed() {
  unsigned long interval = configMode ? LED_FAST_MS : LED_SLOW_MS;
  if (millis() - lastLedToggle >= interval) {
    lastLedToggle = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LED_ACTIVE_LEVEL : LED_IDLE_LEVEL);
  }
}

void handleResetButton();

void startConfigPortal() {
  configMode = true;
  WiFi.disconnect(true);
  delay(100);
  Serial.println("Starting config portal ESP32C3_Setup");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32C3_Setup");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  WebServer portal(80);
  portal.on("/", HTTP_GET, [&]() {
    portal.send(200, "text/html", configPortalPage(""));
  });
  portal.on("/save", HTTP_POST, [&]() {
    String ssid = portal.arg("ssid");
    String password = portal.arg("pass");
    ssid.trim();
    if (ssid.isEmpty()) {
      portal.send(200, "text/html", configPortalPage("Введите имя сети"));
      return;
    }
    saveWifiConfig(ssid, password);
    portal.send(200, "text/html", wifiSavedPage());
    delay(1500);
    ESP.restart();
  });
  portal.onNotFound([&]() {
    portal.sendHeader("Location", "http://192.168.4.1/");
    portal.send(302, "text/plain", "");
  });
  portal.begin();

  while (true) {
    portal.handleClient();
    handleResetButton();
    updateLed();
    delay(2);
  }
}

void handleResetButton() {
  bool pressed = digitalRead(RESET_PIN) == LOW;
  if (pressed) {
    if (!resetPressed) {
      resetPressed = true;
      resetPressedAt = millis();
    } else if ((millis() - resetPressedAt) >= FACTORY_RESET_HOLD_MS) {
      Serial.println("factory reset");
      prefs.begin("skud", false);
      prefs.clear();
      prefs.end();
      WiFi.disconnect(true, true);
      delay(500);
      ESP.restart();
    }
  } else {
    if (resetPressed) {
      unsigned long held = millis() - resetPressedAt;
      resetPressed = false;
      if (held >= RESET_HOLD_MS) {
        Serial.println("wifi reset");
        clearWifiConfig();
        WiFi.disconnect(true, true);
        delay(500);
        ESP.restart();
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_IDLE_LEVEL);

  Serial.println();
  Serial.print("Reset reason: ");
  Serial.println((int)esp_reset_reason());

  prefs.begin("skud", false);
  uint32_t bootCount = prefs.getUInt("boot_count", 0) + 1;
  prefs.putUInt("boot_count", bootCount);
  prefs.end();
  Serial.print("Boot count: ");
  Serial.println(bootCount);

  pinMode(PULSE_PIN, OUTPUT);
  digitalWrite(PULSE_PIN, PULSE_IDLE_LEVEL);

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(RESET_PIN, INPUT_PULLUP);

  stableDoorOpen = readDoorOpen();
  lastRawDoorOpen = stableDoorOpen;
  lastRawChange = millis();

  loadConfig();

  Serial.println();
  Serial.print("Server: ");
  if (serverHost.isEmpty()) {
    Serial.println("not set");
  } else {
    Serial.print(serverHost);
    Serial.print(":");
    Serial.println(serverPort);
  }
  Serial.print("Device secret: ");
  Serial.println(deviceSecret.isEmpty() ? "not set" : "set");

  Serial.println("Starting WiFi setup");
  Serial.print("WiFi provisioned: ");
  Serial.println(wifiProvisioned ? "yes" : "no");

  if (wifiProvisioned && !wifiSsid.isEmpty()) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(wifiSsid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    unsigned long connectStarted = millis();
    unsigned long lastDot = connectStarted;
    while (WiFi.status() != WL_CONNECTED && (millis() - connectStarted) < WIFI_CONNECT_TIMEOUT_MS) {
      updateLed();
      if (millis() - lastDot >= 500) {
        lastDot = millis();
        Serial.print(".");
      }
      delay(10);
    }
    Serial.println();
  }

  if (WiFi.status() != WL_CONNECTED) {
    startConfigPortal();
  }

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  const char *headerKeys[] = {"X-Api-Key", "X-Phrase"};
  server.collectHeaders(headerKeys, 2);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/pulse", HTTP_POST, handlePulse);
  server.begin();

  lastRegisterAttempt = millis() - REGISTER_RETRY_MS;
  Serial.println("Skud terminal ready");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  updateLed();
  updatePulse();
  pollDoor();
  handleResetButton();

  if ((millis() - lastHeartbeat) >= 5000) {
    lastHeartbeat = millis();
    Serial.print("alive, WiFi=");
    Serial.print((int)WiFi.status());
    Serial.print(" IP=");
    Serial.print(WiFi.localIP());
    Serial.print(" heap=");
    Serial.println(ESP.getFreeHeap());
  }

  if (WiFi.status() == WL_CONNECTED) {
    unsigned long interval = sessionPhrase.isEmpty() ? REGISTER_RETRY_MS : REGISTER_INTERVAL_MS;
    if ((millis() - lastRegisterAttempt) >= interval) {
      lastRegisterAttempt = millis();
      registerDevice();
    }
    flushEvents();
  } else {
    if ((millis() - lastWiFiAttempt) >= WIFI_RETRY_MS) {
      lastWiFiAttempt = millis();
      WiFi.reconnect();
    }
  }
}
