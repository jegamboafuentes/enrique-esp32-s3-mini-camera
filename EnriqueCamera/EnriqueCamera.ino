#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "board.h"
#include "apps.h"
#include "bsp_cst816.h"

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0, true, LCD_W, LCD_H);

enum Screen { SCREEN_HOME, SCREEN_FOLDER, SCREEN_WEBCAM, SCREEN_RECOG, SCREEN_HELLO, SCREEN_SETTINGS, SCREEN_SHOOT, SCREEN_GALLERY };
static Screen screen = SCREEN_HOME;
static bool boot_down = false;
static uint32_t boot_down_at = 0;
static uint32_t last_tap_ms = 0;
static bool tap_held = false;

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void drawWallpaper() {
  static const uint8_t walls[][6] = {
      {28, 18, 78, 98, 42, 148},
      {8, 28, 72, 20, 120, 160},
      {90, 20, 24, 180, 80, 30},
      {10, 42, 28, 30, 110, 60},
      {18, 18, 22, 70, 74, 82},
      {90, 20, 70, 180, 70, 120},
  };
  int id = settingsWallpaper();
  if (id < 0 || id > 5) id = 0;
  const uint8_t *c = walls[id];
  for (int y = 0; y < LCD_H; y++) {
    uint8_t r = c[0] + (int)(c[3] - c[0]) * y / LCD_H;
    uint8_t g = c[1] + (int)(c[4] - c[1]) * y / LCD_H;
    uint8_t b = c[2] + (int)(c[5] - c[2]) * y / LCD_H;
    gfx->drawFastHLine(0, y, LCD_W, gfx->color565(r, g, b));
  }
}

static void drawStatusBar() {
  gfx->fillRect(0, 0, LCD_W, 22, 0x10A4);
  gfx->setTextSize(1);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(8, 7);
  gfx->print("ECP");
  gfx->fillCircle(208, 11, 2, 0x07E0);
  gfx->fillCircle(216, 11, 2, 0x07E0);
  gfx->fillCircle(224, 11, 2, 0xC618);
  gfx->drawRoundRect(230, 7, 6, 10, 1, 0xFFFF);
  gfx->fillRect(231, 9, 4, 6, 0x07E0);
}

static void drawAppIcon(int x, int y, uint16_t color, const char *glyph, const char *label) {
  gfx->fillRoundRect(x, y, 54, 54, 12, color);
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  int gw = (int)strlen(glyph) * 12;
  gfx->setCursor(x + (54 - gw) / 2, y + 18);
  gfx->print(glyph);
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(40, 44, 60));
  int lw = (int)strlen(label) * 6;
  gfx->setCursor(x + (54 - lw) / 2, y + 58);
  gfx->print(label);
}

static void drawSettingsIcon(int x, int y) {
  gfx->fillRoundRect(x, y, 70, 70, 16, gfx->color565(140, 148, 160));
  gfx->fillCircle(x + 35, y + 35, 14, gfx->color565(230, 232, 236));
  gfx->fillCircle(x + 35, y + 35, 6, gfx->color565(140, 148, 160));
  for (int i = 0; i < 4; i++) {
    int dx = (i == 1) ? 18 : (i == 3) ? -18 : 0;
    int dy = (i == 0) ? -18 : (i == 2) ? 18 : 0;
    gfx->fillCircle(x + 35 + dx, y + 35 + dy, 4, gfx->color565(230, 232, 236));
  }
  gfx->setTextSize(1);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(x + 11, y + 76);
  gfx->print("Settings");
}

static void drawFolderIcon(int x, int y) {
  gfx->fillRoundRect(x, y, 70, 70, 16, gfx->color565(210, 214, 220));
  const uint16_t mini[4] = {
      gfx->color565(50, 200, 90),
      gfx->color565(160, 90, 255),
      gfx->color565(80, 170, 255),
      gfx->color565(255, 90, 80)};
  for (int i = 0; i < 4; i++) {
    int cx = x + 10 + (i % 2) * 28;
    int cy = y + 10 + (i / 2) * 28;
    gfx->fillRoundRect(cx, cy, 22, 22, 6, mini[i]);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(x + 23, y + 76);
  gfx->print("Apps");
}

static void drawHome() {
  settingsApplyBrightness();
  gfx->setTextWrap(false);
  drawWallpaper();
  drawStatusBar();

  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(18, 40);
  gfx->print("Enrique");
  gfx->setCursor(18, 62);
  gfx->print("Camera");
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(210, 220, 255));
  gfx->setCursor(18, 86);
  gfx->print("Project");

  drawFolderIcon(18, 130);
  drawSettingsIcon(112, 130);

  gfx->fillRoundRect(18, 278, 204, 30, 15, gfx->color565(20, 24, 48));
  gfx->fillRoundRect(100, 290, 40, 5, 2, 0xC618);
}

static void drawFolder() {
  drawHome();
  gfx->fillRect(0, 22, LCD_W, LCD_H - 22, gfx->color565(8, 10, 24));
  gfx->fillRoundRect(12, 44, 216, 248, 22, gfx->color565(232, 236, 242));
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(40, 44, 60));
  gfx->setCursor(96, 54);
  gfx->print("Apps");

  drawAppIcon(22, 74, gfx->color565(50, 200, 90), "Cam", "Webcam");
  drawAppIcon(93, 74, gfx->color565(140, 80, 255), "AI", "Scan");
  drawAppIcon(164, 74, gfx->color565(220, 50, 70), "Rec", "Shoot");
  drawAppIcon(54, 160, gfx->color565(40, 180, 200), "Pic", "Gallery");
  drawAppIcon(140, 160, gfx->color565(80, 160, 255), "Hi", "Hello");

  gfx->setTextColor(gfx->color565(80, 90, 120));
  gfx->setCursor(48, 248);
  gfx->print("Tap an app  ·  BOOT = Home");
}

static void leaveCurrentApp() {
  if (screen == SCREEN_WEBCAM) webcamLeave();
  else if (screen == SCREEN_RECOG) recogLeave();
  else if (screen == SCREEN_HELLO) helloLeave();
  else if (screen == SCREEN_SETTINGS) settingsLeave();
  else if (screen == SCREEN_SHOOT) shootLeave();
  else if (screen == SCREEN_GALLERY) galleryLeave();
}

static void goHome() {
  if (screen == SCREEN_HOME) {
    drawHome();
    return;
  }
  leaveCurrentApp();
  screen = SCREEN_HOME;
  drawHome();
  Serial.println("Home");
}

static void openApp(Screen next) {
  screen = next;
  if (next == SCREEN_WEBCAM) webcamEnter();
  else if (next == SCREEN_RECOG) recogEnter();
  else if (next == SCREEN_HELLO) helloEnter();
  else if (next == SCREEN_SETTINGS) settingsEnter();
  else if (next == SCREEN_SHOOT) shootEnter();
  else if (next == SCREEN_GALLERY) galleryEnter();
}

static void handleTap(uint16_t x, uint16_t y) {
  if (millis() - last_tap_ms < 350) return;
  last_tap_ms = millis();

  if (screen == SCREEN_HOME) {
    if (inRect(x, y, 18, 130, 70, 92)) {
      screen = SCREEN_FOLDER;
      drawFolder();
    } else if (inRect(x, y, 112, 130, 70, 92)) {
      openApp(SCREEN_SETTINGS);
    }
    return;
  }

  if (screen == SCREEN_FOLDER) {
    if (inRect(x, y, 22, 74, 54, 74)) openApp(SCREEN_WEBCAM);
    else if (inRect(x, y, 93, 74, 54, 74)) openApp(SCREEN_RECOG);
    else if (inRect(x, y, 164, 74, 54, 74)) openApp(SCREEN_SHOOT);
    else if (inRect(x, y, 54, 160, 54, 74)) openApp(SCREEN_GALLERY);
    else if (inRect(x, y, 140, 160, 54, 74)) openApp(SCREEN_HELLO);
    else if (!inRect(x, y, 12, 44, 216, 248)) goHome();
    return;
  }
}

static void pollHomeButton() {
  bool down = digitalRead(PIN_BOOT) == LOW;
  if (down && !boot_down) {
    boot_down = true;
    boot_down_at = millis();
  } else if (!down && boot_down) {
    uint32_t held = millis() - boot_down_at;
    boot_down = false;
    if (held > 40 && held < 2500 && screen != SCREEN_HOME) {
      goHome();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nEnrique Camera Project");
  pinMode(PIN_BOOT, INPUT_PULLUP);
  settingsLoad();

  if (!gfx->begin()) {
    Serial.println("LCD init failed");
  }
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  settingsApplyBrightness();
  gfx->setTextWrap(false);

  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  if (!bsp_touch_init(&Wire, 0, LCD_W, LCD_H)) {
    Serial.println("Touch not found (BOOT still works)");
  }

  drawHome();
}

void loop() {
  pollHomeButton();

  bsp_touch_read();
  uint16_t tx = 0, ty = 0;
  bool touching = bsp_touch_get_coordinates(&tx, &ty);
  bool newTap = false;
  uint16_t tapx = 0, tapy = 0;
  if (touching && !tap_held) {
    tap_held = true;
    newTap = true;
    tapx = tx;
    tapy = ty;
    if (screen == SCREEN_HOME || screen == SCREEN_FOLDER) {
      handleTap(tx, ty);
    }
  } else if (!touching) {
    tap_held = false;
  }

  if (screen == SCREEN_WEBCAM) webcamLoop();
  else if (screen == SCREEN_RECOG) recogLoop(newTap);
  else if (screen == SCREEN_HELLO) helloLoop();
  else if (screen == SCREEN_SETTINGS) settingsLoop(newTap, tapx, tapy);
  else if (screen == SCREEN_SHOOT) shootLoop(newTap, tapx, tapy);
  else if (screen == SCREEN_GALLERY) galleryLoop(newTap, tapx, tapy);
  else delay(20);
}
