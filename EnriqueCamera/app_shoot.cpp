#include "apps.h"
#include "board.h"
#include "storage.h"

#include <esp_camera.h>
#include <img_converters.h>
#include <esp_heap_caps.h>

#define PREVIEW_H 196
#define HUD_Y 196

enum ShootMode { MODE_PHOTO, MODE_VIDEO, MODE_LAPSE };
static const int kIntervals[] = {1, 2, 3, 5, 10, 15, 30, 60};
static const int kIntervalCount = 8;

static ShootMode mode = MODE_PHOTO;
static int interval_i = 3;
static bool cam_ok = false;
static bool rec = false;
static uint16_t *preview = nullptr;
static char status_msg[40] = "ready";
static uint32_t rec_started = 0;
static uint32_t last_lapse_ms = 0;
static uint32_t photo_count = 0;
static uint32_t last_hud = 0;
static bool hud_dirty = true;
static int lapse_dir_index = 0;
static char lapse_dir[32] = "";

static File avi;
static uint32_t avi_frames = 0;
static uint32_t avi_w = 640;
static uint32_t avi_h = 480;
static uint32_t movi_size = 0;
static uint32_t *idx_off = nullptr;
static uint32_t *idx_sz = nullptr;
static uint32_t idx_cap = 0;

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void setStatus(const char *msg) {
  strncpy(status_msg, msg, sizeof(status_msg) - 1);
  status_msg[sizeof(status_msg) - 1] = 0;
  hud_dirty = true;
}

static void put32(File &f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  f.write(b, 4);
}

static void put16(File &f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  f.write(b, 2);
}

static void aviWriteHeaderPlaceholder() {
  avi.seek(0);
  avi.write((const uint8_t *)"RIFF", 4);
  put32(avi, 0);
  avi.write((const uint8_t *)"AVI ", 4);
  avi.write((const uint8_t *)"LIST", 4);
  put32(avi, 192);
  avi.write((const uint8_t *)"hdrl", 4);
  avi.write((const uint8_t *)"avih", 4);
  put32(avi, 56);
  for (int i = 0; i < 56; i++) avi.write((uint8_t)0);
  avi.write((const uint8_t *)"LIST", 4);
  put32(avi, 116);
  avi.write((const uint8_t *)"strl", 4);
  avi.write((const uint8_t *)"strh", 4);
  put32(avi, 56);
  for (int i = 0; i < 56; i++) avi.write((uint8_t)0);
  avi.write((const uint8_t *)"strf", 4);
  put32(avi, 40);
  for (int i = 0; i < 40; i++) avi.write((uint8_t)0);
  avi.write((const uint8_t *)"LIST", 4);
  put32(avi, 0);
  avi.write((const uint8_t *)"movi", 4);
}

static void writeZeros(File &f, int n) {
  uint8_t z = 0;
  for (int i = 0; i < n; i++) f.write(z);
}

static void aviPatchAndClose(int fps) {
  if (!avi) return;
  uint32_t movi_list = 4 + movi_size;
  uint32_t idx1_bytes = 8 + avi_frames * 16;
  uint32_t riff = 4 + (8 + 192) + (8 + movi_list) + idx1_bytes;

  avi.write((const uint8_t *)"idx1", 4);
  put32(avi, avi_frames * 16);
  for (uint32_t i = 0; i < avi_frames; i++) {
    avi.write((const uint8_t *)"00dc", 4);
    put32(avi, 0x10);
    put32(avi, idx_off[i]);
    put32(avi, idx_sz[i]);
  }

  uint32_t usec = fps > 0 ? (1000000 / fps) : 125000;
  avi.seek(4);
  put32(avi, riff);

  avi.seek(32);
  put32(avi, usec);
  put32(avi, avi_w * avi_h * 3 * (uint32_t)fps);
  put32(avi, 0);
  put32(avi, 16);
  put32(avi, avi_frames);
  put32(avi, 0);
  put32(avi, 1);
  put32(avi, 0);
  put32(avi, avi_w);
  put32(avi, avi_h);
  writeZeros(avi, 16);

  avi.seek(108);
  avi.write((const uint8_t *)"vids", 4);
  avi.write((const uint8_t *)"MJPG", 4);
  put32(avi, 0);
  put32(avi, 0);
  put32(avi, 0);
  put32(avi, usec);
  put32(avi, 1000000);
  put32(avi, 0);
  put32(avi, avi_frames);
  put32(avi, 0);
  put32(avi, 0);
  put32(avi, 0);
  put16(avi, 0);
  put16(avi, 0);
  put32(avi, 0);

  avi.seek(172);
  put32(avi, 40);
  put32(avi, avi_w);
  put32(avi, avi_h);
  put16(avi, 1);
  put16(avi, 24);
  avi.write((const uint8_t *)"MJPG", 4);
  put32(avi, avi_w * avi_h * 3);
  writeZeros(avi, 16);

  avi.seek(216);
  put32(avi, movi_list);
  avi.close();
}

static bool aviOpen(const char *path, uint32_t w, uint32_t h) {
  avi = storageFS().open(path, FILE_WRITE);
  if (!avi) return false;
  avi_w = w;
  avi_h = h;
  avi_frames = 0;
  movi_size = 0;
  if (!idx_off) {
    idx_cap = 4000;
    idx_off = (uint32_t *)heap_caps_malloc(idx_cap * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    idx_sz = (uint32_t *)heap_caps_malloc(idx_cap * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!idx_off || !idx_sz) {
    avi.close();
    return false;
  }
  aviWriteHeaderPlaceholder();
  return true;
}

static bool aviAddJpeg(const uint8_t *jpg, uint32_t len) {
  if (!avi || avi_frames >= idx_cap) return false;
  uint32_t pad = len & 1;
  uint32_t chunk = 8 + len + pad;
  idx_off[avi_frames] = 4 + movi_size;
  idx_sz[avi_frames] = len;
  avi.write((const uint8_t *)"00dc", 4);
  put32(avi, len + pad);
  avi.write(jpg, len);
  if (pad) avi.write((uint8_t)0);
  movi_size += chunk;
  avi_frames++;
  return true;
}

static void drawHud() {
  gfx->fillRect(0, HUD_Y, LCD_W, LCD_H - HUD_Y, gfx->color565(10, 12, 20));
  const char *modes[3] = {"PHOTO", "VIDEO", "LAPSE"};
  for (int i = 0; i < 3; i++) {
    int x = 8 + i * 76;
    uint16_t bg = (mode == i) ? gfx->color565(220, 50, 50) : gfx->color565(36, 42, 58);
    gfx->fillRoundRect(x, HUD_Y + 6, 70, 18, 8, bg);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(x + 16, HUD_Y + 10);
    gfx->print(modes[i]);
  }

  if (mode == MODE_LAPSE) {
    gfx->fillRoundRect(8, HUD_Y + 28, 28, 20, 6, gfx->color565(36, 42, 58));
    gfx->fillRoundRect(204, HUD_Y + 28, 28, 20, 6, gfx->color565(36, 42, 58));
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(18, HUD_Y + 33);
    gfx->print("-");
    gfx->setCursor(214, HUD_Y + 33);
    gfx->print("+");
    gfx->setCursor(50, HUD_Y + 33);
    gfx->printf("every %d sec", kIntervals[interval_i]);
  }

  int shutter_y = (mode == MODE_LAPSE) ? HUD_Y + 52 : HUD_Y + 32;
  gfx->fillCircle(120, shutter_y + 22, 18, rec ? gfx->color565(220, 40, 40) : 0xFFFF);
  gfx->fillCircle(120, shutter_y + 22, 12, rec ? gfx->color565(180, 20, 20) : gfx->color565(220, 50, 50));

  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(160, 170, 190));
  gfx->setCursor(8, 304);
  gfx->print(storageLabel());
  gfx->print("  ");
  gfx->print(status_msg);
  gfx->setCursor(170, 304);
  gfx->print("BOOT=Home");
  last_hud = millis();
  hud_dirty = false;
}

static void showPreview(camera_fb_t *fb) {
  if (!fb || !preview || fb->format != PIXFORMAT_JPEG) return;
  if (!jpg2rgb565(fb->buf, fb->len, (uint8_t *)preview, JPG_SCALE_2X)) return;
  const int src_w = 320;
  for (int y = 0; y < PREVIEW_H; y++) {
    gfx->draw16bitRGBBitmap(0, y, &preview[y * src_w + 40], LCD_W, 1);
  }
  if (rec) {
    gfx->fillCircle(16, 16, 6, gfx->color565(255, 40, 40));
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(1);
    gfx->setCursor(28, 12);
    uint32_t sec = (millis() - rec_started) / 1000;
    if (mode == MODE_VIDEO) gfx->printf("REC %02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    else gfx->printf("LAPSE %lu", (unsigned long)photo_count);
  }
}

static bool saveJpegFile(const char *path, camera_fb_t *fb) {
  File f = storageFS().open(path, FILE_WRITE);
  if (!f) return false;
  size_t n = f.write(fb->buf, fb->len);
  f.close();
  return n == fb->len;
}

static void takePhoto() {
  if (!storageReady()) {
    setStatus("no memory");
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    setStatus("capture fail");
    return;
  }
  char path[40];
  snprintf(path, sizeof(path), "/DCIM/IMG_%04lu.jpg", (unsigned long)storageNextIndex());
  bool ok = saveJpegFile(path, fb);
  showPreview(fb);
  esp_camera_fb_return(fb);
  setStatus(ok ? path + 6 : "save fail");
}

static void stopRecording() {
  if (!rec) return;
  rec = false;
  if (mode == MODE_VIDEO && avi) {
    aviPatchAndClose(8);
    setStatus("video saved");
  } else if (mode == MODE_LAPSE) {
    char msg[40];
    snprintf(msg, sizeof(msg), "%lu shots saved", (unsigned long)photo_count);
    setStatus(msg);
  }
  hud_dirty = true;
}

static void startVideo() {
  if (!storageReady()) {
    setStatus("no memory");
    return;
  }
  char path[40];
  snprintf(path, sizeof(path), "/DCIM/VID_%04lu.avi", (unsigned long)storageNextIndex());
  if (!aviOpen(path, avi_w, avi_h)) {
    setStatus("video open fail");
    return;
  }
  rec = true;
  rec_started = millis();
  photo_count = 0;
  setStatus("recording");
}

static void startLapse() {
  if (!storageReady()) {
    setStatus("no memory");
    return;
  }
  lapse_dir_index = (int)storageNextIndex();
  snprintf(lapse_dir, sizeof(lapse_dir), "/DCIM/HYP_%04d", lapse_dir_index);
  storageEnsureDir(lapse_dir);
  rec = true;
  rec_started = millis();
  last_lapse_ms = 0;
  photo_count = 0;
  setStatus("hyperlapse");
}

static void lapseTick() {
  uint32_t gap = (uint32_t)kIntervals[interval_i] * 1000UL;
  if (last_lapse_ms != 0 && millis() - last_lapse_ms < gap) return;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  char path[48];
  snprintf(path, sizeof(path), "%s/%05lu.jpg", lapse_dir, (unsigned long)(photo_count + 1));
  if (saveJpegFile(path, fb)) {
    photo_count++;
    last_lapse_ms = millis();
    char msg[40];
    snprintf(msg, sizeof(msg), "%lu shots  %ds", (unsigned long)photo_count, kIntervals[interval_i]);
    setStatus(msg);
  }
  showPreview(fb);
  esp_camera_fb_return(fb);
}

static void handleShutter() {
  if (mode == MODE_PHOTO) {
    takePhoto();
  } else if (mode == MODE_VIDEO) {
    if (rec) stopRecording();
    else startVideo();
  } else {
    if (rec) stopRecording();
    else startLapse();
  }
  hud_dirty = true;
}

void shootEnter() {
  mode = MODE_PHOTO;
  rec = false;
  photo_count = 0;
  hud_dirty = true;
  setStatus("starting...");
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(16, 80);
  gfx->println("Shoot");

  storageBegin();
  cam_ok = cameraInitJpeg();
  if (!cam_ok) {
    setStatus("camera fail");
    drawHud();
    return;
  }
  if (!preview) {
    preview = (uint16_t *)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    framesize_t fs = s->status.framesize;
    if (fs == FRAMESIZE_QVGA) {
      avi_w = 320;
      avi_h = 240;
    } else {
      avi_w = 640;
      avi_h = 480;
    }
  }
  setStatus(storageReady() ? "ready" : "no SD/flash");
  drawHud();
}

void shootLoop(bool tapped, uint16_t tx, uint16_t ty) {
  if (tapped) {
    if (inRect(tx, ty, 8, HUD_Y + 6, 70, 18) && !rec) {
      mode = MODE_PHOTO;
      hud_dirty = true;
    } else if (inRect(tx, ty, 84, HUD_Y + 6, 70, 18) && !rec) {
      mode = MODE_VIDEO;
      hud_dirty = true;
    } else if (inRect(tx, ty, 160, HUD_Y + 6, 70, 18) && !rec) {
      mode = MODE_LAPSE;
      hud_dirty = true;
    } else if (mode == MODE_LAPSE && !rec && inRect(tx, ty, 8, HUD_Y + 28, 28, 20)) {
      if (interval_i > 0) interval_i--;
      hud_dirty = true;
    } else if (mode == MODE_LAPSE && !rec && inRect(tx, ty, 204, HUD_Y + 28, 28, 20)) {
      if (interval_i < kIntervalCount - 1) interval_i++;
      hud_dirty = true;
    } else if (ty > HUD_Y + 48 || inRect(tx, ty, 90, HUD_Y + 28, 60, 50)) {
      handleShutter();
    }
  }

  if (!cam_ok) {
    if (hud_dirty) drawHud();
    delay(40);
    return;
  }

  if (mode == MODE_LAPSE && rec) {
    lapseTick();
  } else if (mode == MODE_VIDEO && rec) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      if (avi_frames == 0 && fb->width && fb->height) {
        avi_w = fb->width;
        avi_h = fb->height;
      }
      aviAddJpeg(fb->buf, fb->len);
      photo_count++;
      if ((photo_count & 7) == 0) showPreview(fb);
      esp_camera_fb_return(fb);
    }
  } else {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      showPreview(fb);
      esp_camera_fb_return(fb);
    }
  }

  if (hud_dirty || millis() - last_hud > 500) drawHud();
}

void shootLeave() {
  stopRecording();
  cameraStop();
  cam_ok = false;
}
