#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <Arduino_GFX_Library.h>
#include <ESP_TF.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "bsp_cst816.h"
#include "model_data.h"
#include "labels.h"

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
#define PIN_TP_SDA 48
#define PIN_TP_SCL 47

#define LCD_W 240
#define LCD_H 320
#define PREVIEW_H 240
#define HUD_Y 240
#define HUD_H 80
#define SCAN_INTERVAL_MS 8000
#define TENSOR_ARENA_SIZE (400 * 1024)

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);

const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;
uint8_t *tensor_arena = nullptr;

struct Prediction {
  const char *label;
  float score;
};

Prediction top3[3];
uint32_t last_infer_ms = 0;
uint32_t infer_time_ms = 0;
bool model_ready = false;
bool camera_ready = false;
volatile bool scanning = false;
const char *status_line = "point at an object";

static uint16_t *frame_copy = nullptr;
static volatile int copy_w = 0;
static volatile int copy_h = 0;
static TaskHandle_t infer_task = nullptr;
static portMUX_TYPE result_mux = portMUX_INITIALIZER_UNLOCKED;

static void showMessage(uint16_t bg, const char *line1, const char *line2 = nullptr) {
  gfx->fillScreen(bg);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(8, 80);
  gfx->println(line1);
  if (line2) {
    gfx->setTextSize(1);
    gfx->setCursor(8, 120);
    gfx->println(line2);
  }
}

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
    gfx->printf("%lu ms  tap to scan", (unsigned long)infer_time_ms);
  } else {
    gfx->setTextColor(gfx->color565(140, 160, 190));
    gfx->setTextSize(1);
    gfx->setCursor(8, HUD_Y + 32);
    gfx->println("Live camera + on-device");
    gfx->setCursor(8, HUD_Y + 46);
    gfx->println("MobileNet ImageNet (1000 classes)");
  }
}

static void blitPreview(const camera_fb_t *fb) {
  if (!fb || !fb->buf || fb->format != PIXFORMAT_RGB565) {
    return;
  }
  // draw16bitBeRGBBitmap clips 240px width to 239 on this 240-wide panel
  // (off-by-one), which shears the image on a corner-to-corner diagonal.
  uint16_t *src = (uint16_t *)fb->buf;
  if (fb->width == LCD_W && fb->height >= PREVIEW_H) {
    gfx->draw16bitRGBBitmap(0, 0, src, LCD_W, PREVIEW_H);
    return;
  }
  const int copy_w = min((int)fb->width, LCD_W);
  const int copy_h = min((int)fb->height, PREVIEW_H);
  const int src_x0 = ((int)fb->width - copy_w) / 2;
  const int src_y0 = ((int)fb->height - copy_h) / 2;
  for (int y = 0; y < copy_h; y++) {
    gfx->draw16bitRGBBitmap(0, y, &src[(src_y0 + y) * fb->width + src_x0], copy_w, 1);
  }
}

static int8_t quantizePreprocessed(float value_0_255) {
  // MobileNet Keras preprocess: RGB/127.5 - 1, then quantize with tensor params.
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
  if (!model_ready || !fb) return false;
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
  Serial.printf("1. %s %.1f%%  2. %s %.1f%%  3. %s %.1f%%  (%lums)\n",
                top3[0].label ? top3[0].label : "-", top3[0].score * 100.0f,
                top3[1].label ? top3[1].label : "-", top3[1].score * 100.0f,
                top3[2].label ? top3[2].label : "-", top3[2].score * 100.0f,
                (unsigned long)infer_time_ms);
  return true;
}

static void inferTask(void *param) {
  (void)param;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    camera_fb_t fake = {};
    fake.buf = (uint8_t *)frame_copy;
    fake.width = copy_w;
    fake.height = copy_h;
    fake.format = PIXFORMAT_RGB565;
    runInference(&fake);
  }
}

static bool queueInference(const camera_fb_t *fb) {
  if (!fb || scanning || !frame_copy || !infer_task) return false;
  const size_t bytes = (size_t)fb->width * fb->height * 2;
  memcpy(frame_copy, fb->buf, bytes);
  copy_w = fb->width;
  copy_h = fb->height;
  scanning = true;
  xTaskNotifyGive(infer_task);
  return true;
}

static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_1;
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
  config.frame_size = FRAMESIZE_240X240;
  config.pixel_format = PIXFORMAT_RGB565;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera 240x240 failed: 0x%x, trying QVGA\n", err);
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_count = 1;
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
  }
  return true;
}

static bool initModel() {
  tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tensor_arena) {
    Serial.println("PSRAM arena alloc failed, trying internal RAM");
    tensor_arena = (uint8_t *)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!tensor_arena) {
    Serial.println("tensor arena alloc failed");
    return false;
  }

  model = tflite::GetModel(g_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("Model schema %lu != %d\n", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

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
  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors failed");
    return false;
  }
  input = interpreter->input(0);
  output = interpreter->output(0);
  Serial.printf("input type=%d dims=%d,%d,%d,%d scale=%f zp=%d\n",
                input->type,
                input->dims->data[0], input->dims->data[1], input->dims->data[2], input->dims->data[3],
                input->params.scale, input->params.zero_point);
  Serial.printf("output type=%d classes=%d scale=%f zp=%d\n",
                output->type, output->dims->data[output->dims->size - 1],
                output->params.scale, output->params.zero_point);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32-S3-Touch-LCD-2 object recognizer");
  Serial.printf("PSRAM size: %u  free: %u\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());

  if (!gfx->begin()) {
    Serial.println("LCD init failed");
  }
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  showMessage(gfx->color565(10, 14, 32), "Object lens", "starting camera + MobileNet...");

  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);
  if (!bsp_touch_init(&Wire, 0, LCD_W, LCD_H)) {
    Serial.println("Touch not found (continuing)");
  }

  camera_ready = initCamera();
  if (!camera_ready) {
    showMessage(RED, "No camera", "Plug OV2640/OV5640 into the 24-pin header");
    return;
  }

  model_ready = initModel();
  if (!model_ready) {
    showMessage(gfx->color565(80, 20, 20), "Model failed", "TFLite arena / schema error");
    return;
  }

  frame_copy = (uint16_t *)heap_caps_malloc(320 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!frame_copy) {
    showMessage(RED, "No PSRAM", "Could not allocate frame copy");
    return;
  }
  xTaskCreatePinnedToCore(inferTask, "infer", 8192, NULL, 1, &infer_task, 0);
  disableCore0WDT();

  gfx->fillScreen(BLACK);
  drawHud();
  status_line = "tap to scan";
}

void loop() {
  if (!camera_ready) {
    delay(250);
    return;
  }

  bool tapped = false;
  bsp_touch_read();
  uint16_t tx, ty;
  if (bsp_touch_get_coordinates(&tx, &ty)) {
    tapped = true;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    delay(10);
    return;
  }

  static bool logged_size = false;
  if (!logged_size) {
    Serial.printf("Camera frame %dx%d len=%u format=%d\n",
                  fb->width, fb->height, (unsigned)fb->len, fb->format);
    logged_size = true;
  }
  blitPreview(fb);

  const bool due = (millis() - last_infer_ms) >= SCAN_INTERVAL_MS;
  if (model_ready && !scanning && (tapped || due || last_infer_ms == 0)) {
    queueInference(fb);
  }

  drawHud();
  esp_camera_fb_return(fb);
}
