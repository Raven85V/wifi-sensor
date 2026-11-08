// ============================================================
//  Wi-Fi датчик для PixelHub 75 — ESP8266 (NodeMCU / Wemos D1 mini)
//  ------------------------------------------------------------
//  Пины:
//    BME280/BMP280: SDA = D2 (GPIO4), SCL = D1 (GPIO5), 3.3 В
//    DS18B20:       DATA = D5 (GPIO14), подтяжка 4.7 кОм к 3.3 В
//    Светодиод:     D4 (GPIO2), встроенный, активный уровень LOW
//
//  Настройка: при первом включении поднимается точка доступа
//  "Sensor-Setup" (без пароля). Подключитесь и откройте 192.168.4.1.
//  Данные уходят на матрицу: /api/sensor?id=...&t=..&h=..&p=..
// ============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define I2C_SDA_PIN   4     // D2
#define I2C_SCL_PIN   5     // D1
#define ONEWIRE_PIN   14    // D5
#define STATUS_LED    2     // D4, активный LOW

static const char *AP_SSID = "Sensor-Setup";

struct Config {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char server[48];
  char id[32];
  uint32_t interval;
};
static const uint32_t CFG_MAGIC = 0x50534831;   // "PSH1"
Config cfg;

ESP8266WebServer web(80);
DNSServer dns;

Adafruit_BME280 bme;
Adafruit_BMP280 bmp;
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds18(&oneWire);

enum SensorKind { SK_NONE, SK_BME280, SK_BMP280, SK_DS18B20 };
SensorKind kind = SK_NONE;

bool apMode = false;
uint32_t lastSend = 0, sentOk = 0, sentErr = 0;

// ---------------- LED (активный LOW) ----------------
void ledOn()  { digitalWrite(STATUS_LED, LOW); }
void ledOff() { digitalWrite(STATUS_LED, HIGH); }

// ---------------- настройки в EEPROM ----------------
void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CFG_MAGIC;
    strncpy(cfg.server, "192.168.1.172", sizeof(cfg.server) - 1);
    strncpy(cfg.id, "sensor1", sizeof(cfg.id) - 1);
    cfg.interval = 60;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
  if (cfg.interval < 10) cfg.interval = 10;
  if (cfg.interval > 3600) cfg.interval = 3600;
}

void saveConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------- страница настройки ----------------
String htmlPage() {
  String h = F("<!DOCTYPE html><html lang='ru'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>"
               "<title>Wi-Fi датчик</title><style>"
               "body{font-family:system-ui,Arial;margin:0;background:#111;color:#eee}"
               ".wrap{max-width:420px;margin:20px auto;padding:0 14px}"
               "h1{font-size:20px}label{display:block;margin:12px 0 4px;color:#9cf}"
               "input{width:100%;padding:10px;border-radius:8px;border:1px solid #444;"
               "background:#1b1b1b;color:#eee;font-size:16px}"
               "button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;"
               "background:#27ae60;color:#fff;font-size:16px}"
               ".hint{color:#888;font-size:13px;margin-top:8px}</style></head><body>"
               "<div class='wrap'><h1>Wi-Fi датчик</h1>"
               "<form method='POST' action='/save'>");
  h += "<label>Wi-Fi сеть (2.4 ГГц)</label><input name='ssid' value='" + String(cfg.ssid) + "'>";
  h += "<label>Пароль Wi-Fi</label><input name='pass' type='password' value='" + String(cfg.pass) + "'>";
  h += "<label>IP матрицы</label><input name='server' value='" + String(cfg.server) + "'>";
  h += "<label>Имя датчика (id)</label><input name='id' value='" + String(cfg.id) + "'>";
  h += "<label>Интервал отправки, сек</label><input name='interval' type='number' min='10' max='3600' value='" + String(cfg.interval) + "'>";
  h += "<button type='submit'>Сохранить и перезагрузить</button></form>"
       "<p class='hint'>Имя должно совпадать с id слота на странице «Датчики» матрицы.</p>"
       "<p class='hint'>Найдено: ";
  h += (kind == SK_BME280 ? "BME280" : kind == SK_BMP280 ? "BMP280" :
        kind == SK_DS18B20 ? "DS18B20" : "нет");
  h += " | отправлено: " + String(sentOk) + ", ошибок: " + String(sentErr);
  h += "</p></div></body></html>";
  return h;
}

void handleRoot() { web.send(200, "text/html; charset=utf-8", htmlPage()); }

void handleSave() {
  String ssid = web.arg("ssid");
  String pass = web.arg("pass");
  String server = web.arg("server");
  String id = web.arg("id");
  uint32_t interval = web.arg("interval").toInt();
  if (interval < 10) interval = 10;
  if (interval > 3600) interval = 3600;

  memset(cfg.ssid, 0, sizeof(cfg.ssid));
  memset(cfg.pass, 0, sizeof(cfg.pass));
  memset(cfg.server, 0, sizeof(cfg.server));
  memset(cfg.id, 0, sizeof(cfg.id));
  strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
  strncpy(cfg.pass, pass.c_str(), sizeof(cfg.pass) - 1);
  strncpy(cfg.server, server.c_str(), sizeof(cfg.server) - 1);
  strncpy(cfg.id, id.length() ? id.c_str() : "sensor1", sizeof(cfg.id) - 1);
  cfg.interval = interval;
  saveConfig();

  web.send(200, "text/html; charset=utf-8",
           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta http-equiv='refresh' content='6;url=/'></head>"
           "<body style='font-family:sans-serif'><h2>Сохранено</h2>"
           "<p>Перезагрузка и подключение к сети…</p></body></html>");
  delay(800);
  ESP.restart();
}

// ---------------- поиск датчика ----------------
void initSensors() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (bme.begin(0x76) || bme.begin(0x77)) { kind = SK_BME280; Serial.println("BME280"); return; }
  if (bmp.begin(0x76) || bmp.begin(0x77)) { kind = SK_BMP280; Serial.println("BMP280"); return; }
  ds18.begin();
  if (ds18.getDeviceCount() > 0) { kind = SK_DS18B20; Serial.println("DS18B20"); return; }
  kind = SK_NONE;
  Serial.println("Датчик не найден");
}

bool readValues(float &t, float &h, float &p) {
  switch (kind) {
    case SK_BME280:
      t = bme.readTemperature(); h = bme.readHumidity(); p = bme.readPressure() / 100.0f;
      return !isnan(t) && !isnan(h);
    case SK_BMP280:
      t = bmp.readTemperature(); h = NAN; p = bmp.readPressure() / 100.0f;
      return !isnan(t);
    case SK_DS18B20:
      ds18.requestTemperatures();
      t = ds18.getTempCByIndex(0); h = NAN; p = NAN;
      return t > -60 && t < 90;
    default:
      return false;
  }
}

void sendValues() {
  float t = NAN, h = NAN, p = NAN;
  if (!readValues(t, h, p)) { sentErr++; return; }

  String url = "http://" + String(cfg.server) + "/api/sensor?id=" + String(cfg.id);
  if (!isnan(t)) url += "&t=" + String(t, 2);
  if (!isnan(h)) url += "&h=" + String(h, 1);
  if (!isnan(p)) url += "&p=" + String(p, 1);

  HTTPClient http;
  http.setTimeout(6000);
  WiFiClient client;
  if (!http.begin(client, url)) { sentErr++; return; }
  int code = http.GET();
  http.end();
  if (code >= 200 && code < 300) sentOk++; else sentErr++;
  Serial.printf("POST -> %d (ok %lu / err %lu)\n", code,
                (unsigned long)sentOk, (unsigned long)sentErr);
}

// ---------------- точка доступа ----------------
void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  delay(200);
  dns.start(53, "*", WiFi.softAPIP());
  web.on("/", handleRoot);
  web.on("/save", handleSave);
  web.onNotFound([]() { web.send(200, "text/html; charset=utf-8", htmlPage()); });
  web.begin();
  Serial.print("AP: ");
  Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(STATUS_LED, OUTPUT);
  ledOff();

  loadConfig();
  initSensors();

  Serial.printf("Wi-Fi: %s | сервер: %s | id: %s\n", cfg.ssid, cfg.server, cfg.id);

  if (strlen(cfg.ssid) == 0) { startAP(); return; }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    ledOn(); delay(120); ledOff(); delay(120);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Нет связи — режим настройки");
    startAP();
    return;
  }
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  web.on("/", handleRoot);
  web.on("/save", handleSave);
  web.begin();

  sendValues();
  lastSend = millis();
}

void loop() {
  if (apMode) {
    dns.processNextRequest();
    web.handleClient();
    return;
  }
  web.handleClient();
  if (millis() - lastSend >= cfg.interval * 1000UL) {
    lastSend = millis();
    sendValues();
    ledOn(); delay(40); ledOff();
  }
}
