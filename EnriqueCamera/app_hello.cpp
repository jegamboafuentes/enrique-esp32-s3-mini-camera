#include "apps.h"
#include "board.h"

static uint32_t last_hello_ms = 0;

void helloEnter() {
  gfx->fillScreen(BLACK);
  gfx->setCursor(16, 24);
  gfx->setTextColor(RED);
  gfx->setTextSize(2);
  gfx->println("Hello World!");
  gfx->setTextSize(1);
  gfx->setTextColor(0x8C71);
  gfx->setCursor(16, 300);
  gfx->print("BOOT = Home");
  last_hello_ms = 0;
}

void helloLoop() {
  if (millis() - last_hello_ms < 1000) return;
  last_hello_ms = millis();
  gfx->setCursor(random(0, LCD_W - 40), random(40, LCD_H - 40));
  gfx->setTextColor(random(0xffff), random(0xffff));
  gfx->setTextSize(random(1, 5), random(1, 5), random(0, 2));
  gfx->print("Hello World!");
}

void helloLeave() {
  gfx->setTextSize(1);
}
