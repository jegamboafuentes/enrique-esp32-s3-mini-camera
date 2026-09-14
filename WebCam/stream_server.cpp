#include "stream_server.h"

#include <string.h>
#include <stdlib.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_http_server.h"
#include "esp_camera.h"
#include "img_converters.h"

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

static httpd_handle_t camera_httpd = NULL;
static httpd_handle_t stream_httpd = NULL;
static volatile int g_stream_clients = 0;

static portMUX_TYPE wifiMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_wifi_pending = false;
static volatile bool g_forget_pending = false;
static char g_pending_ssid[33] = {0};
static char g_pending_pass[65] = {0};

int streamClientCount() {
  return g_stream_clients;
}

bool takePendingWifi(char *ssid, size_t ssidLen, char *pass, size_t passLen) {
  if (!g_wifi_pending) {
    return false;
  }
  portENTER_CRITICAL(&wifiMux);
  if (!g_wifi_pending) {
    portEXIT_CRITICAL(&wifiMux);
    return false;
  }
  strncpy(ssid, g_pending_ssid, ssidLen - 1);
  ssid[ssidLen - 1] = 0;
  strncpy(pass, g_pending_pass, passLen - 1);
  pass[passLen - 1] = 0;
  g_wifi_pending = false;
  portEXIT_CRITICAL(&wifiMux);
  return true;
}

bool takeForgetWifi() {
  if (!g_forget_pending) {
    return false;
  }
  g_forget_pending = false;
  return true;
}

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>ESP32 Webcam</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; font-family: ui-sans-serif, system-ui, sans-serif; background: #0b1020; color: #e8eefc; }
  header { padding: 14px 16px 10px; display: flex; align-items: center; gap: 12px; border-bottom: 1px solid #1c2744; }
  h1 { font-size: 18px; margin: 0; letter-spacing: .04em; }
  .dot { width: 10px; height: 10px; border-radius: 50%; background: #3dff9a; box-shadow: 0 0 10px #3dff9a; }
  .wrap { max-width: 960px; margin: 0 auto; padding: 12px; }
  .stage { background: #070b16; border: 1px solid #1c2744; border-radius: 14px; overflow: hidden; min-height: 180px; }
  #cam { display: block; width: 100%; height: auto; background: #000; }
  .bar { display: flex; flex-wrap: wrap; gap: 8px; padding: 12px 0; }
  button, select, input { background: #15203a; color: #e8eefc; border: 1px solid #2a3b63; border-radius: 10px; padding: 10px 12px; font-size: 14px; }
  button { cursor: pointer; }
  button:hover { border-color: #5eead4; }
  .ghost { background: transparent; }
  details { margin-top: 8px; background: #10182c; border: 1px solid #1c2744; border-radius: 12px; padding: 10px 12px; }
  summary { cursor: pointer; color: #9db0d4; }
  form { display: grid; gap: 8px; margin-top: 10px; }
  .hint { color: #9db0d4; font-size: 13px; line-height: 1.45; }
  a { color: #5eead4; }
</style>
</head>
<body>
<header>
  <div class="dot"></div>
  <h1>ESP32 Webcam</h1>
</header>
<div class="wrap">
  <div class="stage"><img id="cam" alt="Live camera" src="/stream"></div>
  <div class="bar">
    <select id="res">
      <option value="5">QVGA 320x240</option>
      <option value="8" selected>VGA 640x480</option>
      <option value="9">SVGA 800x600</option>
      <option value="11">HD 1280x720</option>
    </select>
    <button id="snap">Snapshot</button>
    <button id="flip" class="ghost">Flip</button>
    <button id="mirror" class="ghost">Mirror</button>
  </div>
  <p class="hint">Same-origin live stream on port 80. If the picture freezes, use Chrome or Firefox, or tap Snapshot.</p>
  <details>
    <summary>Join home Wi‑Fi</summary>
    <p class="hint">After it joins, switch this phone/laptop onto the same Wi‑Fi, then open the address on the LCD — or try <a href="http://esp32-webcam.local">http://esp32-webcam.local</a>.</p>
    <form action="/wifi" method="get">
      <input name="ssid" placeholder="Home Wi‑Fi name" required>
      <input name="pass" type="password" placeholder="Password">
      <button type="submit">Connect</button>
    </form>
    <p class="hint"><a href="/forget">Leave home Wi‑Fi</a> and go back to the ESP32-Webcam hotspot. You can also hold BOOT 3 seconds.</p>
  </details>
</div>
<script>
  const ctrl = (v, n) => fetch('/control?var=' + v + '&val=' + n);
  document.getElementById('res').onchange = (e) => ctrl('framesize', e.target.value);
  document.getElementById('snap').onclick = () => { window.open('/capture', '_blank'); };
  let flip = 1, mirror = 0;
  document.getElementById('flip').onclick = () => { flip ^= 1; ctrl('vflip', flip); };
  document.getElementById('mirror').onclick = () => { mirror ^= 1; ctrl('hmirror', mirror); };
</script>
</body>
</html>
)HTML";

static void urlDecode(char *dst, size_t dstLen, const char *src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < dstLen; i++) {
    if (src[i] == '%' && src[i + 1] && src[i + 2]) {
      char hex[3] = {src[i + 1], src[i + 2], 0};
      dst[j++] = (char)strtol(hex, nullptr, 16);
      i += 2;
    } else if (src[i] == '+') {
      dst[j++] = ' ';
    } else {
      dst[j++] = src[i];
    }
  }
  dst[j] = 0;
}

static esp_err_t parseQuery(httpd_req_t *req, char **obuf) {
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len <= 1) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  char *buf = (char *)malloc(buf_len);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (httpd_req_get_url_query_str(req, buf, buf_len) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  *obuf = buf;
  return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = ESP_OK;
  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, 80, &jpg, &jpg_len)) {
      res = httpd_resp_send(req, (const char *)jpg, jpg_len);
      free(jpg);
    } else {
      res = ESP_FAIL;
      httpd_resp_send_500(req);
    }
  }
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  struct timeval timestamp;
  esp_err_t res = ESP_OK;
  size_t jpg_len = 0;
  uint8_t *jpg = NULL;
  char part_buf[128];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

  g_stream_clients++;
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      timestamp.tv_sec = fb->timestamp.tv_sec;
      timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool ok = frame2jpg(fb, 80, &jpg, &jpg_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!ok) {
          res = ESP_FAIL;
        }
      } else {
        jpg_len = fb->len;
        jpg = fb->buf;
      }
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpg_len, (int)timestamp.tv_sec, (int)timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)jpg, jpg_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      jpg = NULL;
    } else if (jpg) {
      free(jpg);
      jpg = NULL;
    }
    if (res != ESP_OK) {
      break;
    }
  }
  if (g_stream_clients > 0) {
    g_stream_clients--;
  }
  return res;
}

static esp_err_t control_handler(httpd_req_t *req) {
  char *buf = NULL;
  if (parseQuery(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  char variable[32] = {0};
  char value[32] = {0};
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
      httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  int val = atoi(value);
  int res = 0;
  if (!strcmp(variable, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) {
      res = s->set_framesize(s, (framesize_t)val);
    }
  } else if (!strcmp(variable, "quality")) {
    res = s->set_quality(s, val);
  } else if (!strcmp(variable, "vflip")) {
    res = s->set_vflip(s, val);
  } else if (!strcmp(variable, "hmirror")) {
    res = s->set_hmirror(s, val);
  } else if (!strcmp(variable, "brightness")) {
    res = s->set_brightness(s, val);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  (void)res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "ok", 2);
}

static esp_err_t wifi_handler(httpd_req_t *req) {
  char *buf = NULL;
  if (parseQuery(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  char ssid_enc[64] = {0};
  char pass_enc[64] = {0};
  char ssid[33] = {0};
  char pass[65] = {0};
  httpd_query_key_value(buf, "ssid", ssid_enc, sizeof(ssid_enc));
  httpd_query_key_value(buf, "pass", pass_enc, sizeof(pass_enc));
  free(buf);
  urlDecode(ssid, sizeof(ssid), ssid_enc);
  urlDecode(pass, sizeof(pass), pass_enc);

  if (ssid[0] == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    return ESP_FAIL;
  }

  Preferences prefs;
  prefs.begin("webcam", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();

  portENTER_CRITICAL(&wifiMux);
  strncpy(g_pending_ssid, ssid, sizeof(g_pending_ssid) - 1);
  strncpy(g_pending_pass, pass, sizeof(g_pending_pass) - 1);
  g_wifi_pending = true;
  portEXIT_CRITICAL(&wifiMux);

  httpd_resp_set_type(req, "text/html");
  const char *body =
      "<html><body style='font-family:sans-serif;background:#0b1020;color:#e8eefc;padding:24px'>"
      "<h2>Joining home Wi‑Fi…</h2>"
      "<p>Watch the LCD. Then leave this hotspot, join your home Wi‑Fi, and open the address on the screen.</p>"
      "<p>You can also try <a href='http://esp32-webcam.local' style='color:#5eead4'>http://esp32-webcam.local</a></p>"
      "</body></html>";
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t forget_handler(httpd_req_t *req) {
  g_forget_pending = true;
  httpd_resp_set_type(req, "text/html");
  const char *body =
      "<html><body style='font-family:sans-serif;background:#0b1020;color:#e8eefc;padding:24px'>"
      "<h2>Leaving home Wi‑Fi…</h2>"
      "<p>The hotspot <b>ESP32-Webcam</b> will come back. Open <a href='http://192.168.4.1' style='color:#5eead4'>http://192.168.4.1</a></p>"
      "</body></html>";
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

void stopCameraServer() {
  if (stream_httpd) {
    httpd_stop(stream_httpd);
    stream_httpd = NULL;
  }
  if (camera_httpd) {
    httpd_stop(camera_httpd);
    camera_httpd = NULL;
  }
  g_stream_clients = 0;
}

void startCameraServer() {
  if (camera_httpd || stream_httpd) {
    stopCameraServer();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;
  config.stack_size = 10240;
  config.max_open_sockets = 7;

  httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
  httpd_uri_t capture_uri = {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};
  httpd_uri_t control_uri = {.uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = NULL};
  httpd_uri_t wifi_uri = {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_handler, .user_ctx = NULL};
  httpd_uri_t forget_uri = {.uri = "/forget", .method = HTTP_GET, .handler = forget_handler, .user_ctx = NULL};
  httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};

  config.server_port = 80;
  config.ctrl_port = 32768;
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_uri);
    httpd_register_uri_handler(camera_httpd, &forget_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
  }

  config.server_port = 81;
  config.ctrl_port = 32769;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}
