#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "board.h"
#include "apps.h"
#include "bsp_cst816.h"

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0, true, LCD_W, LCD_H);

enum Screen { SCREEN_HOME, SCREEN_FOLDER, SCREEN_WEBCAM, SCREEN_RECOG, SCREEN_HELLO, SCREEN_SETTINGS, SCREEN_SHOOT, SCREEN_GALLERY, SCREEN_FLIGHTS, SCREEN_TRACK };
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

static void drawAppGlyph(AppGlyph id, int x, int y, int size) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  uint16_t ink = 0xFFFF;
  uint16_t hole = gfx->color565(18, 22, 32);
  if (id == GLYPH_WEBCAM) {
    int bw = size * 30 / 54;
    int bh = size * 17 / 54;
    gfx->fillRoundRect(cx - bw / 2, cy - bh / 5, bw, bh, size / 9, ink);
    gfx->fillRoundRect(cx - size / 9, cy - bh / 2 - size / 14, size * 2 / 9, size / 6, 2, ink);
    gfx->fillCircle(cx + bw / 3, cy - bh / 5 + size / 10, size / 16, gfx->color565(90, 230, 140));
    gfx->fillCircle(cx, cy + size / 14, size / 6, hole);
    gfx->fillCircle(cx, cy + size / 14, size / 12, ink);
  } else if (id == GLYPH_SCAN) {
    int m = size * 15 / 54;
    int t = size >= 40 ? 3 : 2;
    int arm = size * 8 / 54;
    gfx->fillRect(cx - m, cy - m, arm, t, ink);
    gfx->fillRect(cx - m, cy - m, t, arm, ink);
    gfx->fillRect(cx + m - arm, cy - m, arm, t, ink);
    gfx->fillRect(cx + m - t, cy - m, t, arm, ink);
    gfx->fillRect(cx - m, cy + m - t, arm, t, ink);
    gfx->fillRect(cx - m, cy + m - arm, t, arm, ink);
    gfx->fillRect(cx + m - arm, cy + m - t, arm, t, ink);
    gfx->fillRect(cx + m - t, cy + m - arm, t, arm, ink);
    gfx->fillTriangle(cx, cy - size / 7, cx + size / 10, cy + size / 16, cx - size / 10, cy + size / 16, ink);
    gfx->fillCircle(cx, cy + size / 14, size / 14, ink);
  } else if (id == GLYPH_SHOOT) {
    int bw = size * 32 / 54;
    int bh = size * 18 / 54;
    gfx->fillRoundRect(cx - bw / 2, cy - 2, bw, bh, size / 9, ink);
    gfx->fillRoundRect(cx - size / 7, cy - bh / 2 - 1, size * 2 / 7, size / 6, 2, ink);
    gfx->fillCircle(cx, cy + bh / 5, size / 6, hole);
    gfx->fillCircle(cx, cy + bh / 5, size / 11, ink);
    gfx->fillCircle(cx + bw / 2 - size / 9, cy + 2, size / 14, gfx->color565(255, 70, 80));
  } else if (id == GLYPH_GALLERY) {
    int pw = size * 24 / 54;
    int ph = size * 18 / 54;
    gfx->fillRoundRect(cx - pw / 2 + size / 10, cy - ph / 2 - size / 14, pw, ph, 3, gfx->color565(190, 230, 245));
    gfx->fillRoundRect(cx - pw / 2 - size / 14, cy - ph / 2 + size / 12, pw, ph, 3, ink);
    int mx = cx - size / 14;
    int my = cy + size / 8;
    gfx->fillTriangle(mx - size / 7, my + size / 10, mx, my - size / 8, mx + size / 6, my + size / 10, gfx->color565(30, 110, 150));
    gfx->fillCircle(cx + size / 8, cy - size / 18, size / 14, gfx->color565(255, 200, 70));
  } else if (id == GLYPH_FLIGHT) {
    gfx->fillTriangle(cx + size / 4, cy, cx - size / 5, cy - size / 6, cx - size / 5, cy + size / 6, ink);
    gfx->fillTriangle(cx - size / 14, cy, cx + size / 10, cy - size / 4, cx + size / 10, cy + size / 4, ink);
  } else if (id == GLYPH_TRACK) {
    gfx->drawCircle(cx, cy, size / 4, ink);
    gfx->drawCircle(cx, cy, size / 7, ink);
    gfx->fillCircle(cx, cy, 2, ink);
    gfx->fillTriangle(cx + size / 5, cy - size / 5, cx + size / 12, cy - size / 14, cx + size / 4, cy - size / 14, ink);
  } else {
    gfx->fillCircle(cx, cy, size * 15 / 54, ink);
    int e = size >= 40 ? 3 : 2;
    gfx->fillCircle(cx - size / 8, cy - size / 12, e, hole);
    gfx->fillCircle(cx + size / 8, cy - size / 12, e, hole);
    gfx->drawCircle(cx, cy + size / 18, size / 6, hole);
    gfx->fillRect(cx - size / 5, cy - size / 10, size * 2 / 5, size / 6, ink);
  }
}

static void drawAppIcon(int x, int y, uint16_t color, AppGlyph glyph, const char *label) {
  gfx->fillRoundRect(x, y, 54, 54, 12, color);
  drawAppGlyph(glyph, x, y, 54);
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(40, 44, 60));
  int lw = (int)strlen(label) * 6;
  gfx->setCursor(x + (54 - lw) / 2, y + 58);
  gfx->print(label);
}

static void drawSettingsIcon(int x, int y) {
  gfx->fillRoundRect(x, y, 70, 70, 16, gfx->color565(140, 148, 160));
  int cx = x + 35;
  int cy = y + 35;
  uint16_t light = gfx->color565(230, 232, 236);
  uint16_t dark = gfx->color565(140, 148, 160);
  gfx->fillCircle(cx, cy, 16, light);
  static const int8_t teeth[8][2] = {{0, -18}, {13, -13}, {18, 0}, {13, 13}, {0, 18}, {-13, 13}, {-18, 0}, {-13, -13}};
  for (int i = 0; i < 8; i++) {
    gfx->fillCircle(cx + teeth[i][0], cy + teeth[i][1], 5, light);
  }
  gfx->fillCircle(cx, cy, 7, dark);
  gfx->fillCircle(cx, cy, 3, light);
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
      gfx->color565(220, 50, 70),
      gfx->color565(40, 180, 200)};
  const AppGlyph glyphs[4] = {GLYPH_WEBCAM, GLYPH_SCAN, GLYPH_SHOOT, GLYPH_GALLERY};
  for (int i = 0; i < 4; i++) {
    int cx = x + 10 + (i % 2) * 28;
    int cy = y + 10 + (i / 2) * 28;
    gfx->fillRoundRect(cx, cy, 22, 22, 6, mini[i]);
    drawAppGlyph(glyphs[i], cx, cy, 22);
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
  gfx->fillRoundRect(12, 36, 216, 272, 22, gfx->color565(232, 236, 242));
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(40, 44, 60));
  gfx->setCursor(96, 46);
  gfx->print("Apps");

  drawAppIcon(22, 62, gfx->color565(50, 200, 90), GLYPH_WEBCAM, "Webcam");
  drawAppIcon(93, 62, gfx->color565(140, 80, 255), GLYPH_SCAN, "Scan");
  drawAppIcon(164, 62, gfx->color565(220, 50, 70), GLYPH_SHOOT, "Shoot");
  drawAppIcon(22, 138, gfx->color565(40, 180, 200), GLYPH_GALLERY, "Gallery");
  drawAppIcon(93, 138, gfx->color565(30, 140, 220), GLYPH_FLIGHT, "Sky");
  drawAppIcon(164, 138, gfx->color565(80, 160, 255), GLYPH_HELLO, "Hello");
  drawAppIcon(93, 214, gfx->color565(255, 140, 50), GLYPH_TRACK, "Flight");

  gfx->setTextColor(gfx->color565(80, 90, 120));
  gfx->setCursor(62, 286);
  gfx->print("BOOT = Home");
}

static void leaveCurrentApp() {
  if (screen == SCREEN_WEBCAM) webcamLeave();
  else if (screen == SCREEN_RECOG) recogLeave();
  else if (screen == SCREEN_HELLO) helloLeave();
  else if (screen == SCREEN_SETTINGS) settingsLeave();
  else if (screen == SCREEN_SHOOT) shootLeave();
  else if (screen == SCREEN_GALLERY) galleryLeave();
  else if (screen == SCREEN_FLIGHTS) flightsLeave();
  else if (screen == SCREEN_TRACK) trackLeave();
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
  else if (next == SCREEN_FLIGHTS) flightsEnter();
  else if (next == SCREEN_TRACK) trackEnter();
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
    if (inRect(x, y, 22, 62, 54, 74)) openApp(SCREEN_WEBCAM);
    else if (inRect(x, y, 93, 62, 54, 74)) openApp(SCREEN_RECOG);
    else if (inRect(x, y, 164, 62, 54, 74)) openApp(SCREEN_SHOOT);
    else if (inRect(x, y, 22, 138, 54, 74)) openApp(SCREEN_GALLERY);
    else if (inRect(x, y, 93, 138, 54, 74)) openApp(SCREEN_FLIGHTS);
    else if (inRect(x, y, 164, 138, 54, 74)) openApp(SCREEN_HELLO);
    else if (inRect(x, y, 93, 214, 54, 74)) openApp(SCREEN_TRACK);
    else if (!inRect(x, y, 12, 36, 216, 272)) goHome();
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
  else if (screen == SCREEN_FLIGHTS) flightsLoop(newTap, tapx, tapy);
  else if (screen == SCREEN_TRACK) trackLoop(newTap, tapx, tapy);
  else delay(20);
}
