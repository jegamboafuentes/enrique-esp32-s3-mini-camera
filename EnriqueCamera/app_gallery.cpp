#include "apps.h"
#include "board.h"
#include "storage.h"

#include <img_converters.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

#define MAX_ITEMS 80
#define LIST_ROWS 4
#define VIEW_H 240

enum ItemKind { KIND_PHOTO, KIND_VIDEO, KIND_LAPSE };
enum View { VIEW_LIST, VIEW_MEDIA };

struct GalleryItem {
  ItemKind kind;
  char name[20];
};

static GalleryItem items[MAX_ITEMS];
static int item_count = 0;
static int list_top = 0;
static int selected = 0;
static View view = VIEW_LIST;

static uint16_t *preview = nullptr;
static uint8_t *jpg_buf = nullptr;
static size_t jpg_cap = 96 * 1024;

static File media;
static bool playing = false;
static uint32_t next_frame_ms = 0;
static int lapse_count = 0;
static int lapse_frame = 0;
static char status_msg[40] = "";

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static const char *kindChip(ItemKind k) {
  if (k == KIND_PHOTO) return "PIC";
  if (k == KIND_VIDEO) return "VID";
  return "HYP";
}

static const char *baseName(const char *n) {
  const char *slash = strrchr(n, '/');
  return slash ? slash + 1 : n;
}

static int itemNum(const GalleryItem *it) {
  const char *p = it->name;
  while (*p && (*p < '0' || *p > '9')) p++;
  return atoi(p);
}

static int cmpItems(const void *a, const void *b) {
  int na = itemNum((const GalleryItem *)a);
  int nb = itemNum((const GalleryItem *)b);
  if (na != nb) return nb - na;
  return strcmp(((const GalleryItem *)a)->name, ((const GalleryItem *)b)->name);
}

static uint32_t read32(File &f) {
  uint8_t b[4] = {0, 0, 0, 0};
  f.read(b, 4);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void setStatus(const char *msg) {
  strncpy(status_msg, msg, sizeof(status_msg) - 1);
  status_msg[sizeof(status_msg) - 1] = 0;
}

static void closeMedia() {
  if (media) media.close();
  playing = false;
}

static void itemPath(const GalleryItem *it, char *out, size_t n) {
  snprintf(out, n, "/DCIM/%s", it->name);
}

static void blitJpeg(const uint8_t *jpg, size_t len) {
  if (!preview || !jpg || len < 32) return;
  if (!jpg2rgb565(jpg, len, (uint8_t *)preview, JPG_SCALE_2X)) return;
  for (int y = 0; y < VIEW_H; y++) {
    gfx->draw16bitRGBBitmap(0, y, &preview[y * 320 + 40], LCD_W, 1);
  }
}

static bool showJpegFile(const char *path) {
  File f = storageFS().open(path, FILE_READ);
  if (!f) return false;
  size_t n = f.size();
  if (n < 32 || n > jpg_cap) {
    f.close();
    return false;
  }
  size_t got = f.read(jpg_buf, n);
  f.close();
  if (got < 32) return false;
  blitJpeg(jpg_buf, got);
  return true;
}

static void scanItems() {
  item_count = 0;
  list_top = 0;
  selected = 0;
  if (!storageReady()) return;

  File root = storageFS().open("/DCIM");
  if (!root) return;
  File f = root.openNextFile();
  while (f && item_count < MAX_ITEMS) {
    const char *base = baseName(f.name());
    if (f.isDirectory()) {
      if (strncmp(base, "HYP_", 4) == 0) {
        items[item_count].kind = KIND_LAPSE;
        strncpy(items[item_count].name, base, sizeof(items[item_count].name) - 1);
        items[item_count].name[sizeof(items[item_count].name) - 1] = 0;
        item_count++;
      }
    } else if (strncmp(base, "IMG_", 4) == 0 && strstr(base, ".jpg")) {
      items[item_count].kind = KIND_PHOTO;
      strncpy(items[item_count].name, base, sizeof(items[item_count].name) - 1);
      items[item_count].name[sizeof(items[item_count].name) - 1] = 0;
      item_count++;
    } else if (strncmp(base, "VID_", 4) == 0 && strstr(base, ".avi")) {
      items[item_count].kind = KIND_VIDEO;
      strncpy(items[item_count].name, base, sizeof(items[item_count].name) - 1);
      items[item_count].name[sizeof(items[item_count].name) - 1] = 0;
      item_count++;
    }
    f.close();
    f = root.openNextFile();
  }
  if (f) f.close();
  root.close();
  if (item_count > 1) qsort(items, item_count, sizeof(GalleryItem), cmpItems);
}

static int countLapseFrames(const char *dir) {
  int n = 0;
  File d = storageFS().open(dir);
  if (!d) return 0;
  File f = d.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char *b = baseName(f.name());
      if (strstr(b, ".jpg") || strstr(b, ".JPG")) n++;
    }
    f.close();
    f = d.openNextFile();
  }
  d.close();
  return n;
}

static void drawList() {
  gfx->fillScreen(gfx->color565(8, 10, 18));
  gfx->fillRect(0, 0, LCD_W, 36, gfx->color565(18, 22, 36));
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(12, 10);
  gfx->print("Gallery");
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(140, 160, 190));
  gfx->setCursor(168, 14);
  gfx->print(storageLabel());

  if (!storageReady()) {
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(28, 140);
    gfx->print("No memory mounted");
    return;
  }
  if (item_count == 0) {
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(40, 130);
    gfx->print("No photos yet");
    gfx->setTextColor(gfx->color565(140, 160, 190));
    gfx->setCursor(28, 152);
    gfx->print("Shoot something first");
    gfx->setCursor(70, 300);
    gfx->print("BOOT = Home");
    return;
  }

  for (int i = 0; i < LIST_ROWS; i++) {
    int idx = list_top + i;
    if (idx >= item_count) break;
    int y = 48 + i * 52;
    uint16_t bg = (idx == selected) ? gfx->color565(40, 48, 72) : gfx->color565(22, 26, 40);
    gfx->fillRoundRect(10, y, 220, 46, 10, bg);
    uint16_t chip = gfx->color565(255, 150, 40);
    if (items[idx].kind == KIND_VIDEO) chip = gfx->color565(220, 50, 70);
    if (items[idx].kind == KIND_LAPSE) chip = gfx->color565(150, 90, 255);
    gfx->fillRoundRect(18, y + 12, 46, 22, 6, chip);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(28, y + 18);
    gfx->print(kindChip(items[idx].kind));
    gfx->setCursor(72, y + 10);
    gfx->print(items[idx].name);
    gfx->setTextColor(gfx->color565(140, 160, 190));
    gfx->setCursor(72, y + 26);
    if (items[idx].kind == KIND_LAPSE) gfx->print("play as video");
    else if (items[idx].kind == KIND_VIDEO) gfx->print("tap to play");
    else gfx->print("tap to view");
  }

  gfx->fillRoundRect(10, 262, 70, 28, 8, gfx->color565(36, 42, 58));
  gfx->fillRoundRect(90, 262, 60, 28, 8, gfx->color565(36, 42, 58));
  gfx->fillRoundRect(160, 262, 70, 28, 8, gfx->color565(36, 42, 58));
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(32, 271);
  gfx->print("UP");
  gfx->setCursor(108, 271);
  gfx->print("OPEN");
  gfx->setCursor(176, 271);
  gfx->print("DOWN");
  gfx->setTextColor(gfx->color565(140, 160, 190));
  gfx->setCursor(8, 304);
  gfx->printf("%d items  BOOT=Home", item_count);
}

static void drawViewerHud() {
  gfx->fillRect(0, VIEW_H, LCD_W, LCD_H - VIEW_H, gfx->color565(10, 12, 20));
  gfx->setTextSize(1);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(8, 248);
  if (selected >= 0 && selected < item_count) gfx->print(items[selected].name);
  gfx->setTextColor(gfx->color565(140, 160, 190));
  gfx->setCursor(8, 262);
  gfx->print(status_msg);

  gfx->fillRoundRect(8, 280, 50, 28, 8, gfx->color565(36, 42, 58));
  gfx->fillRoundRect(66, 280, 36, 28, 8, gfx->color565(36, 42, 58));
  gfx->fillRoundRect(110, 280, 52, 28, 8, playing ? gfx->color565(220, 50, 70) : gfx->color565(50, 160, 90));
  gfx->fillRoundRect(170, 280, 28, 28, 8, gfx->color565(36, 42, 58));
  gfx->fillRoundRect(204, 280, 28, 28, 8, gfx->color565(36, 42, 58));
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(16, 289);
  gfx->print("Back");
  gfx->setCursor(78, 289);
  gfx->print("<");
  gfx->setCursor(124, 289);
  gfx->print(playing ? "STOP" : "PLAY");
  gfx->setCursor(178, 289);
  gfx->print(">");
}

static bool playAviFrame() {
  if (!media) return false;
  char fcc[4];
  if (media.read((uint8_t *)fcc, 4) != 4) {
    media.seek(224);
    return false;
  }
  if (memcmp(fcc, "00dc", 4) != 0) {
    media.seek(224);
    return false;
  }
  uint32_t sz = read32(media);
  if (sz < 32 || sz > jpg_cap) {
    media.seek(224);
    return false;
  }
  size_t got = media.read(jpg_buf, sz);
  if (got < 32) {
    media.seek(224);
    return false;
  }
  blitJpeg(jpg_buf, got);
  return true;
}

static bool playLapseFrame() {
  if (lapse_count <= 0 || selected < 0) return false;
  char dir[40];
  itemPath(&items[selected], dir, sizeof(dir));
  char path[48];
  snprintf(path, sizeof(path), "%s/%05d.jpg", dir, lapse_frame + 1);
  bool ok = showJpegFile(path);
  lapse_frame = (lapse_frame + 1) % lapse_count;
  return ok;
}

static void openSelected();

static void stepItem(int delta) {
  if (item_count <= 0) return;
  selected = (selected + delta + item_count) % item_count;
  if (view == VIEW_MEDIA) openSelected();
  else {
    if (selected < list_top) list_top = selected;
    if (selected >= list_top + LIST_ROWS) list_top = selected - LIST_ROWS + 1;
    drawList();
  }
}

static void openSelected() {
  if (selected < 0 || selected >= item_count) return;
  closeMedia();
  view = VIEW_MEDIA;
  gfx->fillRect(0, 0, LCD_W, VIEW_H, BLACK);
  char path[48];
  itemPath(&items[selected], path, sizeof(path));

  if (items[selected].kind == KIND_PHOTO) {
    playing = false;
    setStatus(showJpegFile(path) ? "photo" : "can't open");
    drawViewerHud();
    return;
  }

  if (items[selected].kind == KIND_VIDEO) {
    media = storageFS().open(path, FILE_READ);
    if (!media) {
      setStatus("can't open video");
      drawViewerHud();
      return;
    }
    media.seek(224);
    playing = true;
    next_frame_ms = millis() + 120;
    setStatus("playing video");
    playAviFrame();
    drawViewerHud();
    return;
  }

  lapse_count = countLapseFrames(path);
  lapse_frame = 0;
  if (lapse_count <= 0) {
    setStatus("empty hyperlapse");
    drawViewerHud();
    return;
  }
  playing = true;
  next_frame_ms = millis() + 120;
  snprintf(status_msg, sizeof(status_msg), "lapse %d shots", lapse_count);
  playLapseFrame();
  drawViewerHud();
}

static void backToList() {
  closeMedia();
  view = VIEW_LIST;
  drawList();
}

void galleryEnter() {
  view = VIEW_LIST;
  playing = false;
  selected = 0;
  list_top = 0;
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(16, 80);
  gfx->print("Gallery");

  storageBegin();
  if (!preview) {
    preview = (uint16_t *)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!jpg_buf) {
    jpg_buf = (uint8_t *)heap_caps_malloc(jpg_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!preview || !jpg_buf) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(1);
    gfx->setCursor(16, 140);
    gfx->print("Out of memory");
    return;
  }
  scanItems();
  drawList();
}

void galleryLoop(bool tapped, uint16_t tx, uint16_t ty) {
  if (tapped && view == VIEW_LIST) {
    if (inRect(tx, ty, 10, 262, 70, 28)) {
      if (selected > 0) selected--;
      if (selected < list_top) list_top = selected;
      drawList();
    } else if (inRect(tx, ty, 160, 262, 70, 28)) {
      if (selected + 1 < item_count) selected++;
      if (selected >= list_top + LIST_ROWS) list_top = selected - LIST_ROWS + 1;
      drawList();
    } else if (inRect(tx, ty, 90, 262, 60, 28) || inRect(tx, ty, 10, 48, 220, 208)) {
      int row = (ty - 48) / 52;
      if (inRect(tx, ty, 10, 48, 220, 208) && row >= 0 && row < LIST_ROWS && list_top + row < item_count) {
        selected = list_top + row;
      }
      openSelected();
    }
  } else if (tapped && view == VIEW_MEDIA) {
    if (inRect(tx, ty, 8, 280, 50, 28) || ty < 24) {
      backToList();
    } else if (inRect(tx, ty, 66, 280, 36, 28) || (ty < VIEW_H && tx < 70)) {
      stepItem(-1);
    } else if (inRect(tx, ty, 204, 280, 28, 28) || (ty < VIEW_H && tx > 170)) {
      stepItem(1);
    } else if (inRect(tx, ty, 110, 280, 52, 28) || inRect(tx, ty, 70, 40, 100, 180)) {
      if (selected >= 0 && items[selected].kind != KIND_PHOTO) {
        playing = !playing;
        next_frame_ms = 0;
        setStatus(playing ? "playing" : "paused");
        drawViewerHud();
      }
    }
  }

  if (view == VIEW_MEDIA && playing && millis() >= next_frame_ms) {
    if (items[selected].kind == KIND_VIDEO) playAviFrame();
    else if (items[selected].kind == KIND_LAPSE) playLapseFrame();
    next_frame_ms = millis() + 120;
  } else if (!(view == VIEW_MEDIA && playing)) {
    delay(12);
  }
}

void galleryLeave() {
  closeMedia();
  view = VIEW_LIST;
  playing = false;
}
