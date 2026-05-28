/*
  display_main.cpp
    ESP32 #2 — Bambu P1S Headless Monitor
    ST7735S 128x160 TFT Display Node

  Receives printer state via ESP-NOW from ESP32 #1 (bridge)
  Subscribes to HA/MQTT for idle screen environmental data
  Displays dashboard on ST7735S(TFT 1.8inch 128x160) with:
    - Printing status, filename, progress, time remaining, temps
    - Idle screen clock + env data (temp, pressure, AQI)
    - Error alerts (blinking)

  Wiring (locked config):
  -  VDD  -> 3.3V
  -  GND  -> GND
  -  SCL  -> IO18 (VSPI-SCK)
  -  SDA  -> IO23 (VSPI-MOSI)
  -  RST  -> IO4
  -  DC   -> IO27
  -  CS   -> IO5
  -  BLK  -> IO15
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <time.h>

// ── WiFi & MQTT ─────────────────────────────────────────────────────────────
#define WIFI_SSID    "YOUR_SSID"
#define WIFI_PASS    "YOUR_PSW"
#define MQTT_BROKER  "192.168.x.x"
#define MQTT_PORT    1883
#define MQTT_USER    "USERNAME"
#define MQTT_PASS    "PASSWORD"

// ── MQTT topics ──────────────────────────────────────────────────────────────
#define TOPIC_TEMP      "home/baro/temperature"
#define TOPIC_PRESSURE  "home/baro/pressure"
#define TOPIC_AQI       "home/baro/air_quality"

// ── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER  "pool.ntp.org"
#define TZ_OFFSET   -28800
#define TZ_DST      3600

// ── Display ─────────────────────────────────────────────────────────────────
#define TFT_BL_PIN  15
#define SCREEN_W    128
#define SCREEN_H    160

// ── Colors ───────────────────────────────────────────────────────────────────
#define C_BG     TFT_BLACK
#define C_WHITE  TFT_WHITE
#define C_GREEN  TFT_GREEN
#define C_RED    TFT_RED
#define C_YELLOW TFT_YELLOW
#define C_CYAN   TFT_CYAN
#define C_ORANGE 0xFD20
#define C_GREY   0x8410
#define C_DGREY  0x2104
#define C_BAMBU  0x07E0

// ── ESP-NOW data structure ───────────────────────────────────────────────────
typedef struct {
    char  status[16];
    char  filename[48];
    float nozzle_temp;
    float nozzle_target;
    float bed_temp;
    float bed_target;
    int   progress;
    int   time_remaining;
    char  error_msg[32];
    bool  valid;
} PrinterState;

// ── Global state ─────────────────────────────────────────────────────────────
PrinterState  printerState;
volatile bool newDataFlag  = false;

float envTemp     = 0;
float envPressure = 0;
int   envAQI      = 0;
bool  envValid    = false;

TFT_eSPI    tft    = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastMqttRetry  = 0;
unsigned long lastAlertBlink = 0;
unsigned long lastScreenDraw = 0;
bool          alertVisible   = false;

// ── ESP-NOW callback ─────────────────────────────────────────────────────────
void onDataReceived(const uint8_t *mac, const uint8_t *data, int len) {
    if (len == sizeof(PrinterState)) {
        memcpy(&printerState, data, sizeof(PrinterState));
        newDataFlag = true;
    }
}

// ── MQTT callback ────────────────────────────────────────────────────────────
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    char buf[32];
    snprintf(buf, min((unsigned int)sizeof(buf), length + 1),
             "%s", (char*)payload);
    Serial.println("[MQTT] Received: " + String(topic) + " = " + String(buf));
    if (strcmp(topic, TOPIC_TEMP) == 0)     envTemp     = atof(buf);
    if (strcmp(topic, TOPIC_PRESSURE) == 0) envPressure = atof(buf);
    if (strcmp(topic, TOPIC_AQI) == 0)      envAQI      = atoi(buf);
    envValid = true;
}

// ── MQTT reconnect ───────────────────────────────────────────────────────────
void mqttConnect() {
    if (millis() - lastMqttRetry < 10000) return;
    lastMqttRetry = millis();
    String id = "esp32-display-";
    id += String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS)) {
        mqtt.subscribe(TOPIC_TEMP);
        mqtt.subscribe(TOPIC_PRESSURE);
        mqtt.subscribe(TOPIC_AQI);
        Serial.println("[MQTT] Reconnected");
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────────
void formatTime(char* buf, int seconds) {
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    if (h > 0) snprintf(buf, 12, "%dh %02dm", h, m);
    else        snprintf(buf, 12, "%dm", m);
}

void truncateFilename(const char* src, char* dst, int maxChars) {
    strncpy(dst, src, maxChars);
    dst[maxChars] = '\0';
    char* dot = strrchr(dst, '.');
    if (dot) *dot = '\0';
}

uint16_t tempColor(float current, float target) {
    if (target == 0) return C_GREY;
    float diff = abs(current - target);
    if (diff < 3.0)  return C_GREEN;
    if (diff < 10.0) return C_YELLOW;
    return C_RED;
}

uint16_t aqiColor(int raw) {
    if (raw < 50)  return C_GREEN;
    if (raw < 100) return C_YELLOW;
    if (raw < 150) return C_ORANGE;
    return C_RED;
}

const char* aqiLabel(int raw) {
    if (raw < 50)  return "Good";
    if (raw < 100) return "Moderate";
    if (raw < 150) return "Poor";
    return "Bad";
}

String getClockString() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "--:-- --";
    char buf[12];
    strftime(buf, sizeof(buf), "%I:%M %p", &timeinfo);
    if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
    return String(buf);
}

void drawProgressBar(TFT_eSprite& s, int x, int y, int w, int h,
                     int pct, uint16_t color) {
    s.drawRect(x, y, w, h, C_GREY);
    int filled = (int)((float)(w - 2) * pct / 100.0f);
    if (filled > 0) s.fillRect(x + 1, y + 1, filled, h - 2, color);
}

// ── IDLE screen ──────────────────────────────────────────────────────────────
void drawIdleScreen() {
    tft.fillScreen(TFT_BLACK);

    // ── Header ──
    tft.fillRect(0, 0, SCREEN_W, 18, C_DGREY);
    tft.setTextColor(C_BAMBU, C_DGREY);
    tft.setTextSize(1);
    tft.setCursor(30, 5);
    tft.print("BAMBU P1S");

    // ── Clock ──
    String clockStr = getClockString();
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 30);
    tft.print(clockStr);

    // ── Status ──
    tft.setTextSize(1);
    tft.setTextColor(C_GREY, TFT_BLACK);
    tft.setCursor(52, 55);
    tft.print("IDLE");

    // ── Divider ──
    tft.drawFastHLine(10, 65, SCREEN_W - 20, C_DGREY);

    // ── Env data ──
    if (envValid) {
        float tempF = (envTemp * 9.0 / 5.0) + 32.0;
        char tempStr[12];
        snprintf(tempStr, sizeof(tempStr), "%.1fF", tempF);
        tft.setTextColor(C_GREY, TFT_BLACK);
        tft.setCursor(4, 75);
        tft.print("Temp");
        tft.setTextColor(C_CYAN, TFT_BLACK);
        tft.setCursor(80, 75);
        tft.print(tempStr);

        char pressStr[12];
        snprintf(pressStr, sizeof(pressStr), "%.0fhPa", envPressure);
        tft.setTextColor(C_GREY, TFT_BLACK);
        tft.setCursor(4, 95);
        tft.print("Pressure");
        tft.setTextColor(C_CYAN, TFT_BLACK);
        tft.setCursor(68, 95);
        tft.print(pressStr);

        char aqiStr[8];
        snprintf(aqiStr, sizeof(aqiStr), "%d", envAQI);
        tft.setTextColor(C_GREY, TFT_BLACK);
        tft.setCursor(4, 115);
        tft.print("Air");
        tft.setTextColor(aqiColor(envAQI), TFT_BLACK);
        tft.setCursor(80, 115);
        tft.print(aqiStr);
        tft.setCursor(4, 130);
        tft.setTextColor(aqiColor(envAQI), TFT_BLACK);
        tft.print(aqiLabel(envAQI));

    } else {
        tft.setTextColor(C_DGREY, TFT_BLACK);
        tft.setCursor(15, 100);
        tft.print("Waiting for HA...");
    }

    // ── IP address ──
    tft.setTextColor(C_GREY, TFT_BLACK);
    tft.setCursor(4, 150);
    tft.print(WiFi.localIP().toString());
}

// ── PRINTING screen ──────────────────────────────────────────────────────────
void drawPrintingScreen() {
    sprite.fillSprite(C_BG);

    uint16_t statusColor = C_GREEN;
    const char* statusText = "PRINTING";
    if (strcmp(printerState.status, "paused") == 0) {
        statusColor = C_YELLOW;
        statusText  = "PAUSED";
    } else if (strcmp(printerState.status, "error") == 0) {
        statusColor = C_RED;
        statusText  = "ERROR";
    } else if (strcmp(printerState.status, "finish") == 0) {
        statusColor = C_CYAN;
        statusText  = "FINISHED";
    }
    sprite.fillRect(0, 0, SCREEN_W, 18, statusColor);
    sprite.setTextColor(C_BG, statusColor);
    sprite.setTextSize(1);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString(statusText, SCREEN_W / 2, 9);
    sprite.setTextDatum(TL_DATUM);

    char fname[22];
    truncateFilename(printerState.filename, fname, 21);
    sprite.setTextColor(C_WHITE, C_BG);
    sprite.setTextSize(1);
    sprite.setCursor(2, 24);
    sprite.print(fname);

    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%d%%", printerState.progress);
    sprite.setTextSize(3);
    sprite.setTextColor(C_WHITE, C_BG);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(pctStr, SCREEN_W - 2, 34);
    sprite.setTextDatum(TL_DATUM);

    drawProgressBar(sprite, 2, 62, SCREEN_W - 4, 10,
                    printerState.progress, C_BAMBU);

    sprite.setTextSize(1);
    sprite.setTextColor(C_GREY, C_BG);
    sprite.setCursor(2, 76);
    sprite.print("Remaining:");
    char timeStr[12];
    formatTime(timeStr, printerState.time_remaining);
    sprite.setTextColor(C_WHITE, C_BG);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(timeStr, SCREEN_W - 2, 76);
    sprite.setTextDatum(TL_DATUM);

    sprite.drawFastHLine(2, 92, SCREEN_W - 4, C_DGREY);

    sprite.setTextColor(C_GREY, C_BG);
    sprite.setTextSize(1);
    sprite.setCursor(2, 98);
    sprite.print("Nozzle");
    char nozzleStr[12];
    snprintf(nozzleStr, sizeof(nozzleStr), "%.0f/%.0fC",
             printerState.nozzle_temp, printerState.nozzle_target);
    sprite.setTextColor(tempColor(printerState.nozzle_temp,
                                  printerState.nozzle_target), C_BG);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(nozzleStr, SCREEN_W - 2, 98);
    sprite.setTextDatum(TL_DATUM);

    sprite.setTextColor(C_GREY, C_BG);
    sprite.setCursor(2, 112);
    sprite.print("Bed");
    char bedStr[12];
    snprintf(bedStr, sizeof(bedStr), "%.0f/%.0fC",
             printerState.bed_temp, printerState.bed_target);
    sprite.setTextColor(tempColor(printerState.bed_temp,
                                  printerState.bed_target), C_BG);
    sprite.setTextDatum(TR_DATUM);
    sprite.drawString(bedStr, SCREEN_W - 2, 112);
    sprite.setTextDatum(TL_DATUM);

    bool showAlert = (strcmp(printerState.status, "paused") == 0 ||
                      strcmp(printerState.status, "error")  == 0 ||
                      strlen(printerState.error_msg) > 0);
    if (showAlert) {
        if (alertVisible) {
            sprite.fillRect(0, SCREEN_H - 18, SCREEN_W, 18, C_RED);
            sprite.setTextColor(C_WHITE, C_RED);
            sprite.setTextDatum(MC_DATUM);
            const char* msg = (strlen(printerState.error_msg) > 0)
                              ? printerState.error_msg : "Print Paused";
            sprite.drawString(msg, SCREEN_W / 2, SCREEN_H - 9);
            sprite.setTextDatum(TL_DATUM);
        } else {
            sprite.fillRect(0, SCREEN_H - 18, SCREEN_W, 18, C_DGREY);
        }
    }

    sprite.pushSprite(0, 0);
}

// ── NO SIGNAL screen ─────────────────────────────────────────────────────────
void drawNoSignalScreen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(C_DGREY, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(24, 60);
    tft.print("BAMBU P1S");
    tft.setCursor(20, 80);
    tft.print("Waiting for");
    tft.setCursor(12, 92);
    tft.print("bridge signal...");
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== BAMBU DISPLAY BOOT ===");

    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);

    tft.init();
    tft.setRotation(2);
    tft.fillScreen(TFT_BLACK);

    sprite.createSprite(SCREEN_W, SCREEN_H);
    sprite.fillSprite(C_BG);

    // Splash
    tft.setTextColor(C_BAMBU, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(20, 70);
    tft.print("BAMBU MONITOR");
    tft.setCursor(32, 86);
    tft.print("Starting...");

    // ── WiFi ──
    Serial.println("[WiFi] Connecting to: " + String(WIFI_SSID));
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - wifiStart > 20000) {
            Serial.println("\n[WiFi] FAILED — timeout after 20s");
            break;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connected");
        Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
        Serial.println("[WiFi] RSSI: " + String(WiFi.RSSI()) + " dBm");

        // NTP
        Serial.println("[NTP] Syncing time...");
        configTime(TZ_OFFSET, TZ_DST, NTP_SERVER);
        struct tm timeinfo;
        unsigned long ntpStart = millis();
        while (!getLocalTime(&timeinfo) && millis() - ntpStart < 5000) {
            delay(500);
            Serial.print(".");
        }
        if (getLocalTime(&timeinfo)) {
            char timeBuf[32];
            strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
            Serial.println("\n[NTP] Time synced: " + String(timeBuf));
        } else {
            Serial.println("\n[NTP] Sync failed");
        }

        // MQTT
        Serial.println("[MQTT] Connecting to: " + String(MQTT_BROKER));
        mqtt.setServer(MQTT_BROKER, MQTT_PORT);
        mqtt.setCallback(onMqttMessage);
        String clientId = "esp32-display-";
        clientId += String((uint32_t)ESP.getEfuseMac(), HEX);
        if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
            Serial.println("[MQTT] Connected");
            mqtt.subscribe(TOPIC_TEMP);
            mqtt.subscribe(TOPIC_PRESSURE);
            mqtt.subscribe(TOPIC_AQI);
        } else {
            Serial.println("[MQTT] FAILED rc=" + String(mqtt.state()));
        }
    }

    // ── ESP-NOW ──
    Serial.println("[ESP-NOW] Initializing...");
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Init FAILED");
    } else {
        esp_now_register_recv_cb(onDataReceived);
        Serial.println("[ESP-NOW] Ready");
        Serial.println("[ESP-NOW] MAC: " + WiFi.macAddress());
    }

    // Init printer state
    memset(&printerState, 0, sizeof(printerState));
    strcpy(printerState.status, "idle");
    printerState.valid = true;  // force idle for testing — set false when bridge ready

    Serial.println("=== BOOT COMPLETE ===\n");
    delay(500);
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqtt.connected()) mqttConnect();
        mqtt.loop();
    }

    if (millis() - lastAlertBlink > 600) {
        alertVisible   = !alertVisible;
        lastAlertBlink = millis();
    }

    if (millis() - lastScreenDraw > 1000) {
        lastScreenDraw = millis();

        if (!printerState.valid) {
            drawNoSignalScreen();
        } else if (strcmp(printerState.status, "idle")   == 0 ||
                   strcmp(printerState.status, "finish") == 0) {
            drawIdleScreen();
        } else {
            drawPrintingScreen();
        }
    }
}
