#include "apps.h"
#include "board.h"
#include "stream_server.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_camera.h>

static const char *AP_SSID = "ESP32-Webcam";
static const char *MDNS_NAME = "esp32-webcam";

static bool camera_ok = false;
static bool sta_mode = false;
static int last_stations = -1;
static int last_streams = -1;
static bool last_sta = false;
static char last_sta_ip[16] = "";
static uint32_t sta_lost_at = 0;
static bool wifi_events_on = false;

static void disableWifiSleep() {
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

static void startMdns() {
  MDNS.end();
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
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
  gfx->setCursor(14, 300);
  gfx->print("BOOT = Home");
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
    if (streams > 0) gfx->printf("stream: LIVE (%d)", streams);
    else gfx->print("stream: waiting");
    y += 28;
    gfx->setTextColor(dim);
    gfx->setCursor(14, y);
    gfx->print("Open that address in a");
    y += 14;
    gfx->setCursor(14, y);
    gfx->print("browser on the same Wi-Fi.");
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
    if (streams > 0) gfx->printf("stream: LIVE (%d)", streams);
    else gfx->print("stream: waiting");
    last_stations = stations;
  }

  gfx->setTextSize(1);
  gfx->setTextColor(dim);
  gfx->setCursor(14, 300);
  gfx->print("BOOT = Home");

  last_streams = streams;
  last_sta = (WiFi.status() == WL_CONNECTED);
}

static void startHotspot() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  disableWifiSleep();
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  delay(150);
  sta_mode = false;
  last_sta_ip[0] = 0;
}

static bool connectStation(const char *ssid, const char *pass, uint32_t timeout_ms) {
  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  delay(80);
  WiFi.mode(WIFI_STA);
  disableWifiSleep();
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(ssid, pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void enterLanMode() {
  disableWifiSleep();
  WiFi.mode(WIFI_STA);
  startMdns();
  stopCameraServer();
  delay(40);
  startCameraServer();
  sta_mode = true;
  sta_lost_at = 0;
}

static void forgetHomeWifiAndRebootAp() {
  Preferences prefs;
  prefs.begin("webcam", false);
  prefs.clear();
  prefs.end();
  MDNS.end();
  WiFi.disconnect(true, true);
  delay(80);
  startHotspot();
  stopCameraServer();
  startCameraServer();
  drawStatus();
}

void webcamEnter() {
  last_stations = -1;
  last_streams = -1;
  last_sta = false;
  sta_mode = false;
  sta_lost_at = 0;
  camera_ok = false;

  if (!wifi_events_on) {
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.printf("STA disconnect reason %u\n", info.wifi_sta_disconnected.reason);
      }
    });
    wifi_events_on = true;
  }

  drawMessage("starting...", "camera + hotspot");
  camera_ok = cameraInitJpeg();
  if (!camera_ok) {
    gfx->fillScreen(0xA800);
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(2);
    gfx->setCursor(12, 40);
    gfx->println("camera fail");
    gfx->setTextSize(1);
    gfx->setCursor(12, 80);
    gfx->println("check the 24-pin ribbon");
    gfx->setCursor(12, 300);
    gfx->print("BOOT = Home");
    startHotspot();
    return;
  }

  startHotspot();
  startCameraServer();
  drawStatus();
}

void webcamLoop() {
  if (!camera_ok) {
    delay(200);
    return;
  }

  if (takeForgetWifi()) {
    drawMessage("wifi reset", "hotspot coming back");
    delay(200);
    forgetHomeWifiAndRebootAp();
    return;
  }

  char ssid[33] = {0};
  char pass[65] = {0};
  if (takePendingWifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
    delay(600);
    drawMessage("joining wifi", ssid, "then use the LCD address");
    if (connectStation(ssid, pass, 20000)) {
      enterLanMode();
      drawStatus();
    } else {
      WiFi.disconnect(true, false);
      startHotspot();
      drawMessage("join failed", "still on ESP32-Webcam", "check the password");
      delay(1200);
      drawStatus();
    }
    return;
  }

  if (sta_mode) {
    if (WiFi.status() == WL_CONNECTED) {
      sta_lost_at = 0;
    } else if (sta_lost_at == 0) {
      sta_lost_at = millis();
    } else if (millis() - sta_lost_at > 25000) {
      startHotspot();
      stopCameraServer();
      startCameraServer();
      drawStatus();
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
  delay(250);
}

void webcamLeave() {
  stopCameraServer();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  cameraStop();
  camera_ok = false;
  sta_mode = false;
}
