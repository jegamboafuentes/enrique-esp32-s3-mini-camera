#include "apps.h"
#include "board.h"

#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>

#define WALL_COUNT 6

struct Wall {
  uint8_t r0, g0, b0, r1, g1, b1;
};

static const Wall kWalls[WALL_COUNT] = {
    {28, 18, 78, 98, 42, 148},    // purple
    {8, 28, 72, 20, 120, 160},    // ocean
    {90, 20, 24, 180, 80, 30},    // sunset
    {10, 42, 28, 30, 110, 60},    // forest
    {18, 18, 22, 70, 74, 82},     // graphite
    {90, 20, 70, 180, 70, 120},   // pink
};

static int wallpaper_id = 0;
static int brightness = 100;
static bool wifi_busy = false;
static uint32_t wifi_try_at = 0;
static bool dirty = true;

int settingsWallpaper() {
  return wallpaper_id;
}

void settingsSetWallpaper(int id) {
  if (id < 0) id = 0;
  if (id >= WALL_COUNT) id = WALL_COUNT - 1;
  wallpaper_id = id;
  Preferences prefs;
  prefs.begin("ecp", false);
  prefs.putUChar("wall", (uint8_t)wallpaper_id);
  prefs.end();
}

void settingsApplyBrightness() {
  if (brightness < 20) brightness = 20;
  if (brightness > 100) brightness = 100;
  uint32_t duty = (255UL * (uint32_t)brightness) / 100;
  ledcDetach(PIN_LCD_BL);
  ledcAttachChannel(PIN_LCD_BL, 5000, 8, 2);
  ledcWrite(PIN_LCD_BL, duty);
}

static void saveBrightness() {
  Preferences prefs;
  prefs.begin("ecp", false);
  prefs.putUChar("bl", (uint8_t)brightness);
  prefs.end();
  settingsApplyBrightness();
}

void settingsLoad() {
  Preferences prefs;
  prefs.begin("ecp", true);
  wallpaper_id = prefs.getUChar("wall", 0);
  brightness = prefs.getUChar("bl", 100);
  prefs.end();
  if (wallpaper_id < 0 || wallpaper_id >= WALL_COUNT) wallpaper_id = 0;
  if (brightness < 20 || brightness > 100) brightness = 100;
}

static void fillWallPreview(int x, int y, int w, int h, int id) {
  const Wall &c = kWalls[id];
  for (int i = 0; i < h; i++) {
    uint8_t r = c.r0 + (int)(c.r1 - c.r0) * i / h;
    uint8_t g = c.g0 + (int)(c.g1 - c.g0) * i / h;
    uint8_t b = c.b0 + (int)(c.b1 - c.b0) * i / h;
    gfx->drawFastHLine(x, y + i, w, gfx->color565(r, g, b));
  }
}

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void drawSettings() {
  gfx->fillScreen(gfx->color565(242, 242, 247));
  gfx->fillRect(0, 0, LCD_W, 36, gfx->color565(242, 242, 247));
  gfx->setTextSize(2);
  gfx->setTextColor(gfx->color565(20, 20, 24));
  gfx->setCursor(14, 10);
  gfx->print("Settings");

  gfx->fillRoundRect(12, 48, 216, 86, 16, 0xFFFF);
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(80, 80, 90));
  gfx->setCursor(24, 58);
  gfx->print("HOME WI-FI");
  gfx->setTextColor(gfx->color565(20, 20, 24));
  gfx->setTextSize(2);
  gfx->setCursor(24, 74);

  Preferences prefs;
  prefs.begin("webcam", true);
  String ssid = prefs.getString("ssid", "");
  prefs.end();
  if (ssid.isEmpty()) {
    gfx->setTextSize(1);
    gfx->print("not saved yet");
    gfx->setCursor(24, 96);
    gfx->setTextColor(gfx->color565(80, 80, 90));
    gfx->print("Save it once in Webcam");
  } else {
    gfx->print(ssid.length() > 12 ? ssid.substring(0, 12) : ssid);
    gfx->setTextSize(1);
    gfx->setCursor(24, 96);
    if (WiFi.status() == WL_CONNECTED) {
      gfx->setTextColor(gfx->color565(30, 140, 70));
      gfx->printf("on  %s", WiFi.localIP().toString().c_str());
    } else if (wifi_busy) {
      gfx->setTextColor(gfx->color565(180, 120, 20));
      gfx->print("joining...");
    } else {
      gfx->setTextColor(gfx->color565(80, 80, 90));
      gfx->print("disconnected");
    }
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  gfx->fillRoundRect(24, 112, 88, 16, 8, connected ? gfx->color565(230, 80, 80) : gfx->color565(50, 180, 90));
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(connected ? 36 : 40, 115);
  gfx->print(connected ? "Disconnect" : "Connect");

  gfx->fillRoundRect(12, 144, 216, 52, 16, 0xFFFF);
  gfx->setTextColor(gfx->color565(80, 80, 90));
  gfx->setCursor(24, 152);
  gfx->print("SCREEN BRIGHTNESS");
  gfx->setTextColor(gfx->color565(20, 20, 24));
  gfx->setCursor(24, 168);
  gfx->printf("%d%%", brightness);
  gfx->fillRoundRect(132, 156, 40, 28, 8, gfx->color565(230, 232, 236));
  gfx->fillRoundRect(180, 156, 40, 28, 8, gfx->color565(230, 232, 236));
  gfx->setTextColor(gfx->color565(20, 20, 24));
  gfx->setTextSize(2);
  gfx->setCursor(146, 162);
  gfx->print("-");
  gfx->setCursor(194, 162);
  gfx->print("+");
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(80, 80, 90));
  gfx->setCursor(24, 182);
  gfx->print("rear reds = PWR/CHG");

  gfx->fillRoundRect(12, 206, 216, 78, 16, 0xFFFF);
  gfx->setTextColor(gfx->color565(80, 80, 90));
  gfx->setCursor(24, 216);
  gfx->print("HOME BACKGROUND");
  for (int i = 0; i < WALL_COUNT; i++) {
    int x = 24 + i * 32;
    fillWallPreview(x, 234, 26, 26, i);
    gfx->drawRect(x, 234, 26, 26, i == wallpaper_id ? gfx->color565(20, 20, 24) : gfx->color565(200, 200, 206));
    if (i == wallpaper_id) {
      gfx->drawRect(x + 1, 235, 24, 24, gfx->color565(20, 20, 24));
    }
  }

  gfx->setTextColor(gfx->color565(80, 80, 90));
  gfx->setCursor(14, 300);
  gfx->print("BOOT = Home");
  dirty = false;
}

void settingsEnter() {
  wifi_busy = false;
  dirty = true;
  settingsApplyBrightness();
  drawSettings();
}

void settingsLeave() {
  wifi_busy = false;
}

static void startHomeWifi() {
  Preferences prefs;
  prefs.begin("webcam", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.isEmpty()) {
    return;
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifi_busy = true;
  wifi_try_at = millis();
}

static void stopHomeWifi() {
  wifi_busy = false;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
}

void settingsLoop(bool tapped, uint16_t tx, uint16_t ty) {
  if (wifi_busy) {
    if (WiFi.status() == WL_CONNECTED || millis() - wifi_try_at > 20000) {
      wifi_busy = false;
      dirty = true;
    }
  }

  if (tapped) {
    if (inRect(tx, ty, 24, 108, 88, 24)) {
      if (WiFi.status() == WL_CONNECTED) stopHomeWifi();
      else startHomeWifi();
      dirty = true;
    } else if (inRect(tx, ty, 132, 152, 40, 36)) {
      brightness -= 20;
      saveBrightness();
      dirty = true;
    } else if (inRect(tx, ty, 180, 152, 40, 36)) {
      brightness += 20;
      saveBrightness();
      dirty = true;
    } else if (inRect(tx, ty, 24, 230, 192, 34)) {
      int id = (tx - 24) / 32;
      if (id >= 0 && id < WALL_COUNT) {
        settingsSetWallpaper(id);
        dirty = true;
      }
    }
  }

  if (dirty) {
    drawSettings();
  }
  delay(30);
}
