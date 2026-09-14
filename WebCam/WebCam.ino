#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <esp_wifi.h>
#include <Arduino_GFX_Library.h>
#include "stream_server.h"

// Waveshare ESP32-S3-Touch-LCD-2 camera (OV2640 / OV5640)
#define PWDN_GPIO_NUM 17
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 8
#define SIOD_GPIO_NUM 21
#define SIOC_GPIO_NUM 16
#define Y9_GPIO_NUM 2
#define Y8_GPIO_NUM 7
#define Y7_GPIO_NUM 10
#define Y6_GPIO_NUM 14
#define Y5_GPIO_NUM 11
#define Y4_GPIO_NUM 15
#define Y3_GPIO_NUM 13
#define Y2_GPIO_NUM 12
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 4
#define PCLK_GPIO_NUM 9

#define PIN_LCD_SCLK 39
#define PIN_LCD_MOSI 38
#define PIN_LCD_MISO 40
#define PIN_LCD_DC 42
#define PIN_LCD_RST -1
#define PIN_LCD_CS 45
#define PIN_LCD_BL 1
#define PIN_BOOT 0

#define LCD_W 240
#define LCD_H 320
#define MDNS_NAME "esp32-webcam"

static const char *AP_SSID = "ESP32-Webcam";

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0, true, LCD_W, LCD_H);

static bool camera_ok = false;
static bool sta_mode = false;
static int last_stations = -1;
static int last_streams = -1;
static bool last_sta = false;
static char last_sta_ip[16] = "";
static uint32_t boot_held_at = 0;
static uint32_t sta_lost_at = 0;

static void showFatal(const char *line1, const char *line2 = nullptr) {
  gfx->fillScreen(0xA800);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(12, 40);
  gfx->println(line1);
  if (line2) {
    gfx->setTextSize(1);
    gfx->setCursor(12, 80);
    gfx->println(line2);
  }
}

static void disableWifiSleep() {
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

static void startMdns() {
  MDNS.end();
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS http://%s.local\n", MDNS_NAME);
  } else {
    Serial.println("mDNS failed");
  }
}

static void drawMessage(const char *title, const char *line1, const char *line2 = nullptr) {
  gfx->fillScreen(0x10A3);
  gfx->fillRect(0, 0, LCD_W, 36, 0x08A4);
  gfx->setTextColor(0x07FD);
  gfx->setTextSize(2);
  gfx->setCursor(14, 10);
  gfx->print("WEBCAM");
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(14, 56);
  gfx->print(title);
  gfx->setTextSize(1);
  gfx->setTextColor(0x8C71);
  gfx->setCursor(14, 90);
  gfx->print(line1);
  if (line2) {
    gfx->setCursor(14, 108);
    gfx->print(line2);
  }
}

static void drawStatus() {
  const uint16_t bg = 0x10A3;
  const uint16_t accent = 0x07FD;
  const uint16_t dim = 0x8C71;
  const uint16_t text = 0xFFFF;
  const uint16_t live = 0x07E8;

  gfx->fillScreen(bg);
  gfx->fillRect(0, 0, LCD_W, 36, 0x08A4);
  gfx->setTextColor(accent);
  gfx->setTextSize(2);
  gfx->setCursor(14, 10);
  gfx->print("WEBCAM");

  int y = 52;
  int streams = streamClientCount();

  if (sta_mode && WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(last_sta_ip, sizeof(last_sta_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    gfx->setTextSize(1);
    gfx->setTextColor(dim);
    gfx->setCursor(14, y);
    gfx->print("ON YOUR WI-FI");
    y += 16;
    gfx->setTextColor(accent);
    gfx->setTextSize(2);
    gfx->setCursor(14, y);
    gfx->print(last_sta_ip);
    y += 28;
    gfx->setTextSize(1);
    gfx->setTextColor(text);
    gfx->setCursor(14, y);
    gfx->print("esp32-webcam.local");
    y += 24;
    gfx->setTextColor(streams ? live : dim);
    gfx->setCursor(14, y);
    if (streams > 0) {
      gfx->printf("stream: LIVE (%d)", streams);
    } else {
      gfx->print("stream: waiting");
    }
    y += 28;
    gfx->setTextColor(dim);
    gfx->setCursor(14, y);
    gfx->print("Open that address in a");
    y += 14;
    gfx->setCursor(14, y);
    gfx->print("browser on the same Wi-Fi.");
    y += 24;
    gfx->setCursor(14, y);
    gfx->print("Hold BOOT 3s to reset Wi-Fi");
  } else {
    gfx->setTextSize(1);
    gfx->setTextColor(dim);
    gfx->setCursor(14, y);
    gfx->print("1. JOIN THIS WI-FI");
    y += 16;
    gfx->setTextColor(text);
    gfx->setTextSize(2);
    gfx->setCursor(14, y);
    gfx->print(AP_SSID);
    y += 22;
    gfx->setTextSize(1);
    gfx->setTextColor(dim);
    gfx->setCursor(14, y);
    gfx->print("open network  ·  no password");

    y += 28;
    gfx->setTextColor(dim);
    gfx->setCursor(14, y);
    gfx->print("2. OPEN IN A BROWSER");
    y += 16;
    gfx->setTextColor(accent);
    gfx->setTextSize(2);
    gfx->setCursor(14, y);
    gfx->print("192.168.4.1");

    y += 32;
    int stations = WiFi.softAPgetStationNum();
    gfx->setTextSize(1);
    gfx->setTextColor(stations ? live : dim);
    gfx->setCursor(14, y);
    gfx->printf("phones connected: %d", stations);
    y += 16;
    gfx->setTextColor(streams ? live : dim);
    gfx->setCursor(14, y);
    if (streams > 0) {
      gfx->printf("stream: LIVE (%d)", streams);
    } else {
      gfx->print("stream: waiting");
    }
    last_stations = stations;
  }

  last_streams = streams;
  last_sta = (WiFi.status() == WL_CONNECTED);
}

static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("VGA JPEG failed: 0x%x, trying QVGA\n", err);
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    err = esp_camera_init(&config);
  }
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);
    s->set_framesize(s, FRAMESIZE_VGA);
    s->set_quality(s, 12);
  }
  return true;
}

static void startHotspot() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  disableWifiSleep();
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  delay(200);
  sta_mode = false;
  last_sta_ip[0] = 0;
  Serial.printf("AP %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static bool connectStation(const char *ssid, const char *pass, uint32_t timeout_ms) {
  Serial.printf("STA connecting to %s\n", ssid);
  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.mode(WIFI_STA);
  disableWifiSleep();
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(ssid, pass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("STA IP %s  RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.printf("STA failed, status=%d\n", (int)WiFi.status());
  return false;
}

static void enterLanMode() {
  disableWifiSleep();
  WiFi.mode(WIFI_STA);
  startMdns();
  stopCameraServer();
  delay(50);
  startCameraServer();
  sta_mode = true;
  sta_lost_at = 0;
  Serial.printf("LAN http://%s  http://%s.local\n", WiFi.localIP().toString().c_str(), MDNS_NAME);
}

static void forgetHomeWifiAndRebootAp() {
  Serial.println("Forgetting home Wi-Fi");
  Preferences prefs;
  prefs.begin("webcam", false);
  prefs.clear();
  prefs.end();
  MDNS.end();
  WiFi.disconnect(true, true);
  delay(100);
  startHotspot();
  stopCameraServer();
  startCameraServer();
  drawStatus();
}

static void trySavedHomeWifi() {
  Preferences prefs;
  prefs.begin("webcam", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.isEmpty()) {
    return;
  }

  drawMessage("joining wifi", ssid.c_str(), "open the LCD address after");
  if (connectStation(ssid.c_str(), pass.c_str(), 30000)) {
    enterLanMode();
  } else {
    Serial.println("Saved Wi-Fi not reached, AP still available");
    WiFi.disconnect(true, false);
    startHotspot();
  }
}

static void pollBootButton() {
  if (digitalRead(PIN_BOOT) == LOW) {
    if (boot_held_at == 0) {
      boot_held_at = millis();
    } else if (millis() - boot_held_at > 3000) {
      boot_held_at = 0;
      drawMessage("wifi reset", "hotspot coming back");
      forgetHomeWifiAndRebootAp();
    }
  } else {
    boot_held_at = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32 Webcam");
  pinMode(PIN_BOOT, INPUT_PULLUP);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("STA disconnect reason %u\n", info.wifi_sta_disconnected.reason);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("STA got IP %s\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    }
  });

  if (!gfx->begin()) {
    Serial.println("LCD init failed");
  }
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  drawMessage("starting...", "camera + wifi");

  camera_ok = initCamera();
  if (!camera_ok) {
    showFatal("camera fail", "check the 24-pin ribbon");
    startHotspot();
    return;
  }

  Preferences prefs;
  prefs.begin("webcam", true);
  bool has_saved_wifi = !prefs.getString("ssid", "").isEmpty();
  prefs.end();

  if (has_saved_wifi) {
    trySavedHomeWifi();
  } else {
    startHotspot();
  }
  startCameraServer();
  drawStatus();

  if (sta_mode) {
    Serial.println("Already on home Wi-Fi. Open the LCD address.");
  } else {
    Serial.println("Connect your phone to Wi-Fi: ESP32-Webcam");
    Serial.println("Then open http://192.168.4.1");
  }
}

void loop() {
  if (!camera_ok) {
    delay(1000);
    return;
  }

  pollBootButton();

  if (takeForgetWifi()) {
    drawMessage("wifi reset", "hotspot coming back");
    delay(300);
    forgetHomeWifiAndRebootAp();
    return;
  }

  char ssid[33] = {0};
  char pass[65] = {0};
  if (takePendingWifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
    delay(800);
    drawMessage("joining wifi", ssid, "then use the LCD address");
    if (connectStation(ssid, pass, 30000)) {
      enterLanMode();
      drawStatus();
    } else {
      Serial.println("Join failed, keeping hotspot");
      WiFi.disconnect(true, false);
      startHotspot();
      drawMessage("join failed", "still on ESP32-Webcam", "check the password");
      delay(1800);
      drawStatus();
    }
    return;
  }

  if (sta_mode) {
    if (WiFi.status() == WL_CONNECTED) {
      sta_lost_at = 0;
    } else {
      if (sta_lost_at == 0) {
        sta_lost_at = millis();
        Serial.println("STA lost, waiting to reconnect");
      } else if (millis() - sta_lost_at > 25000) {
        Serial.println("STA still down, bringing hotspot back");
        startHotspot();
        stopCameraServer();
        startCameraServer();
        drawStatus();
      }
    }
  }

  int stations = sta_mode ? 0 : WiFi.softAPgetStationNum();
  int streams = streamClientCount();
  bool sta = (WiFi.status() == WL_CONNECTED);
  char ip[16] = "";
  if (sta) {
    IPAddress a = WiFi.localIP();
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  }

  if (stations != last_stations || streams != last_streams || sta != last_sta || strcmp(ip, last_sta_ip) != 0) {
    drawStatus();
  }
  delay(400);
}
