#include "apps.h"
#include "board.h"
#include "bsp_cst816.h"
#include "model_data.h"
#include "labels.h"

#include <math.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <ESP_TF.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#define PREVIEW_H 240
#define HUD_Y 240
#define HUD_H 80
#define SCAN_INTERVAL_MS 8000
#define TENSOR_ARENA_SIZE (400 * 1024)

static const tflite::Model *model = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input = nullptr;
static TfLiteTensor *output = nullptr;
static uint8_t *tensor_arena = nullptr;

struct Prediction {
  const char *label;
  float score;
};

static Prediction top3[3];
static uint32_t last_infer_ms = 0;
static uint32_t infer_time_ms = 0;
static bool model_ready = false;
static bool camera_ready = false;
static volatile bool scanning = false;
static volatile bool recog_active = false;
static const char *status_line = "point at an object";
static uint16_t *frame_copy = nullptr;
static volatile int copy_w = 0;
static volatile int copy_h = 0;
static TaskHandle_t infer_task = nullptr;
static portMUX_TYPE result_mux = portMUX_INITIALIZER_UNLOCKED;

static void drawHud() {
  gfx->fillRect(0, HUD_Y, LCD_W, HUD_H, gfx->color565(8, 12, 28));
  gfx->drawFastHLine(0, HUD_Y, LCD_W, gfx->color565(0, 180, 255));
  gfx->setTextColor(gfx->color565(0, 200, 255));
  gfx->setTextSize(1);
  gfx->setCursor(8, HUD_Y + 6);
  gfx->print(scanning ? "SCANNING..." : status_line);

  if (top3[0].label) {
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(8, HUD_Y + 20);
    gfx->print(top3[0].label);
    const int bar_x = 8;
    const int bar_y = HUD_Y + 42;
    const int bar_w = LCD_W - 16;
    gfx->drawRect(bar_x, bar_y, bar_w, 10, gfx->color565(40, 60, 90));
    int fill = (int)(top3[0].score * (bar_w - 2));
    if (fill < 0) fill = 0;
    if (fill > bar_w - 2) fill = bar_w - 2;
    gfx->fillRect(bar_x + 1, bar_y + 1, fill, 8, gfx->color565(0, 220, 120));
    gfx->setTextSize(1);
    gfx->setTextColor(gfx->color565(180, 220, 180));
    gfx->setCursor(8, HUD_Y + 56);
    gfx->printf("%d%%", (int)(top3[0].score * 100.0f));
    if (top3[1].label) {
      gfx->printf("   2. %s %d%%", top3[1].label, (int)(top3[1].score * 100.0f));
    }
    gfx->setCursor(8, HUD_Y + 68);
    gfx->setTextColor(gfx->color565(120, 140, 170));
    gfx->printf("%lu ms  BOOT=Home  tap=scan", (unsigned long)infer_time_ms);
  } else {
    gfx->setTextColor(gfx->color565(140, 160, 190));
    gfx->setTextSize(1);
    gfx->setCursor(8, HUD_Y + 28);
    gfx->println("On-device MobileNet");
    gfx->setCursor(8, HUD_Y + 44);
    gfx->println("Tap to scan   BOOT = Home");
  }
}

static void blitPreview(const camera_fb_t *fb) {
  if (!fb || !fb->buf || fb->format != PIXFORMAT_RGB565) return;
  uint16_t *src = (uint16_t *)fb->buf;
  if (fb->width == LCD_W && fb->height >= PREVIEW_H) {
    gfx->draw16bitRGBBitmap(0, 0, src, LCD_W, PREVIEW_H);
    return;
  }
  const int w = min((int)fb->width, LCD_W);
  const int h = min((int)fb->height, PREVIEW_H);
  const int src_x0 = ((int)fb->width - w) / 2;
  const int src_y0 = ((int)fb->height - h) / 2;
  for (int y = 0; y < h; y++) {
    gfx->draw16bitRGBBitmap(0, y, &src[(src_y0 + y) * fb->width + src_x0], w, 1);
  }
}

static int8_t quantizePreprocessed(float value_0_255) {
  const float x = value_0_255 / 127.5f - 1.0f;
  int q = (int)lrintf(x / input->params.scale + input->params.zero_point);
  if (q < -128) q = -128;
  if (q > 127) q = 127;
  return (int8_t)q;
}

static void fillModelInput(const camera_fb_t *fb) {
  const int in_h = input->dims->data[1];
  const int in_w = input->dims->data[2];
  const uint16_t *src = (const uint16_t *)fb->buf;
  int8_t *dst = input->data.int8;
  for (int y = 0; y < in_h; y++) {
    const int sy = y * fb->height / in_h;
    for (int x = 0; x < in_w; x++) {
      const int sx = x * fb->width / in_w;
      const uint16_t p = src[sy * fb->width + sx];
      const float r = ((p >> 11) & 0x1F) * (255.0f / 31.0f);
      const float g = ((p >> 5) & 0x3F) * (255.0f / 63.0f);
      const float b = (p & 0x1F) * (255.0f / 31.0f);
      const int i = (y * in_w + x) * 3;
      dst[i + 0] = quantizePreprocessed(r);
      dst[i + 1] = quantizePreprocessed(g);
      dst[i + 2] = quantizePreprocessed(b);
    }
  }
}

static float dequantizeOutput(int index) {
  if (output->type == kTfLiteUInt8) {
    return (output->data.uint8[index] - output->params.zero_point) * output->params.scale;
  }
  return (output->data.int8[index] - output->params.zero_point) * output->params.scale;
}

static const char *labelForIndex(int index, int n_classes) {
  if (n_classes == 1001) {
    if (index <= 0) return "background";
    index -= 1;
  }
  if (index < 0 || index >= kImageNetLabelCount) return "unknown";
  return kImageNetLabels[index];
}

static void rankTop3() {
  const int n = output->dims->data[output->dims->size - 1];
  int idx[3] = {-1, -1, -1};
  float scores[3] = {-1, -1, -1};
  for (int i = 0; i < n; i++) {
    const float s = dequantizeOutput(i);
    if (s > scores[0]) {
      scores[2] = scores[1];
      idx[2] = idx[1];
      scores[1] = scores[0];
      idx[1] = idx[0];
      scores[0] = s;
      idx[0] = i;
    } else if (s > scores[1]) {
      scores[2] = scores[1];
      idx[2] = idx[1];
      scores[1] = s;
      idx[1] = i;
    } else if (s > scores[2]) {
      scores[2] = s;
      idx[2] = i;
    }
  }
  for (int i = 0; i < 3; i++) {
    top3[i].label = (idx[i] >= 0) ? labelForIndex(idx[i], n) : nullptr;
    top3[i].score = scores[i] < 0 ? 0 : scores[i];
  }
}

static bool runInference(const camera_fb_t *fb) {
  if (!model_ready || !fb || !recog_active) return false;
  scanning = true;
  fillModelInput(fb);
  const uint32_t t0 = millis();
  if (interpreter->Invoke() != kTfLiteOk) {
    status_line = "inference failed";
    scanning = false;
    return false;
  }
  infer_time_ms = millis() - t0;
  portENTER_CRITICAL(&result_mux);
  rankTop3();
  portEXIT_CRITICAL(&result_mux);
  status_line = "object found";
  scanning = false;
  last_infer_ms = millis();
  return true;
}

static void inferTask(void *param) {
  (void)param;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!recog_active) continue;
    camera_fb_t fake = {};
    fake.buf = (uint8_t *)frame_copy;
    fake.width = copy_w;
    fake.height = copy_h;
    fake.format = PIXFORMAT_RGB565;
    runInference(&fake);
  }
}

static bool queueInference(const camera_fb_t *fb) {
  if (!fb || scanning || !frame_copy || !infer_task || !recog_active) return false;
  memcpy(frame_copy, fb->buf, (size_t)fb->width * fb->height * 2);
  copy_w = fb->width;
  copy_h = fb->height;
  scanning = true;
  xTaskNotifyGive(infer_task);
  return true;
}

static bool initModel() {
  if (model_ready) return true;
  tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tensor_arena) {
    tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!tensor_arena) return false;
  model = tflite::GetModel(g_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) return false;
  static tflite::MicroMutableOpResolver<10> resolver;
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddMean();
  resolver.AddShape();
  resolver.AddStridedSlice();
  resolver.AddPack();
  resolver.AddReshape();
  resolver.AddSoftmax();
  static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, TENSOR_ARENA_SIZE);
  interpreter = &static_interpreter;
  if (interpreter->AllocateTensors() != kTfLiteOk) return false;
  input = interpreter->input(0);
  output = interpreter->output(0);
  return true;
}

void recogEnter() {
  recog_active = true;
  last_infer_ms = 0;
  top3[0].label = nullptr;
  status_line = "starting...";
  gfx->fillScreen(gfx->color565(10, 14, 32));
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 80);
  gfx->println("Recognizer");
  gfx->setTextSize(1);
  gfx->setCursor(12, 120);
  gfx->println("starting camera + MobileNet");

  camera_ready = cameraInitRgb565();
  if (!camera_ready) {
    gfx->fillScreen(RED);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(8, 80);
    gfx->println("No camera");
    gfx->setTextSize(1);
    gfx->setCursor(8, 120);
    gfx->println("BOOT = Home");
    return;
  }

  model_ready = initModel();
  if (!model_ready) {
    gfx->fillScreen(gfx->color565(80, 20, 20));
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(8, 80);
    gfx->println("Model failed");
    return;
  }

  if (!frame_copy) {
    frame_copy = (uint16_t *)heap_caps_malloc(320 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!infer_task) {
    xTaskCreatePinnedToCore(inferTask, "infer", 8192, NULL, 1, &infer_task, 0);
    disableCore0WDT();
  }

  gfx->fillScreen(BLACK);
  drawHud();
  status_line = "tap to scan";
}

void recogLoop(bool tapped) {
  if (!camera_ready || !recog_active) {
    delay(50);
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    delay(10);
    return;
  }
  blitPreview(fb);
  const bool due = (millis() - last_infer_ms) >= SCAN_INTERVAL_MS;
  if (model_ready && !scanning && (tapped || due || last_infer_ms == 0)) {
    queueInference(fb);
  }
  drawHud();
  esp_camera_fb_return(fb);
}

void recogLeave() {
  recog_active = false;
  scanning = false;
  cameraStop();
  camera_ready = false;
}
