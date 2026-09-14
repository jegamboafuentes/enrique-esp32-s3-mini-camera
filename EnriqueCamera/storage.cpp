#include "storage.h"
#include "board.h"

#include <SD.h>
#include <SPI.h>
#include <FFat.h>
#include <Preferences.h>

static bool ready = false;
static bool use_sd = false;

bool storageBegin() {
  if (ready) {
    return true;
  }

  pinMode(PIN_LCD_CS, OUTPUT);
  digitalWrite(PIN_LCD_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  SPI.begin(PIN_LCD_SCLK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_SD_CS);
  if (SD.begin(PIN_SD_CS, SPI, 20000000) || SD.begin(PIN_SD_CS, SPI, 4000000)) {
    if (SD.cardType() != CARD_NONE) {
      use_sd = true;
      ready = true;
      digitalWrite(PIN_SD_CS, HIGH);
      storageEnsureDir("/DCIM");
      Serial.printf("Storage: SD %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
      return true;
    }
  }
  digitalWrite(PIN_SD_CS, HIGH);

  if (FFat.begin(false) || FFat.begin(true)) {
    use_sd = false;
    ready = true;
    storageEnsureDir("/DCIM");
    Serial.println("Storage: onboard flash");
    return true;
  }
  Serial.println("Storage: none");
  return false;
}

bool storageReady() {
  return ready;
}

const char *storageLabel() {
  if (!ready) return "NO MEM";
  return use_sd ? "SD" : "FLASH";
}

fs::FS &storageFS() {
  if (use_sd) return SD;
  return FFat;
}

void storageEnsureDir(const char *path) {
  if (!ready) return;
  storageFS().mkdir(path);
}

uint32_t storageNextIndex() {
  Preferences prefs;
  prefs.begin("shoot", false);
  uint32_t n = prefs.getUInt("n", 1);
  prefs.putUInt("n", n + 1);
  prefs.end();
  return n;
}
