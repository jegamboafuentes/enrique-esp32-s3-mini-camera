#include "track_server.h"
#include "flight_server.h"

#include <string.h>
#include <stdlib.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_http_server.h"

static httpd_handle_t server = NULL;
static portMUX_TYPE wifiMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool wifi_pending = false;
static char pending_ssid[33] = {0};
static char pending_pass[65] = {0};
static char callsign[12] = "";

static void urlDecode(char *dst, size_t dstLen, const char *src) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < dstLen; i++) {
    if (src[i] == '%' && src[i + 1] && src[i + 2]) {
      char hex[3] = {src[i + 1], src[i + 2], 0};
      dst[o++] = (char)strtol(hex, nullptr, 16);
      i += 2;
    } else if (src[i] == '+') {
      dst[o++] = ' ';
    } else {
      dst[o++] = src[i];
    }
  }
  dst[o] = 0;
}

static void normalizeCallsign(char *dst, size_t n, const char *src) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 1 < n; i++) {
    char c = src[i];
    if (c == ' ' || c == '-' || c == '.') continue;
    if (c >= 'a' && c <= 'z') c -= 32;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) dst[o++] = c;
  }
  dst[o] = 0;
}

static void savePrefs() {
  Preferences p;
  p.begin("track", false);
  p.putString("cs", callsign);
  p.end();
}

void trackLoadPrefs() {
  Preferences p;
  p.begin("track", true);
  String cs = p.getString("cs", "");
  p.end();
  normalizeCallsign(callsign, sizeof(callsign), cs.c_str());
}

bool trackHasCallsign() { return callsign[0] != 0; }
const char *trackGetCallsign() { return callsign; }
void trackSetCallsign(const char *cs) {
  normalizeCallsign(callsign, sizeof(callsign), cs ? cs : "");
  savePrefs();
}

bool trackTakePendingWifi(char *ssid, size_t ssidLen, char *pass, size_t passLen) {
  if (!wifi_pending) return false;
  portENTER_CRITICAL(&wifiMux);
  if (!wifi_pending) {
    portEXIT_CRITICAL(&wifiMux);
    return false;
  }
  strncpy(ssid, pending_ssid, ssidLen - 1);
  ssid[ssidLen - 1] = 0;
  strncpy(pass, pending_pass, passLen - 1);
  pass[passLen - 1] = 0;
  wifi_pending = false;
  portEXIT_CRITICAL(&wifiMux);
  return true;
}

static const char INDEX_HTML[] = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Track a flight</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; font-family:ui-sans-serif,system-ui,sans-serif; background:#07101c; color:#e8eefc; }
  header { padding:14px 16px; border-bottom:1px solid #1c2744; }
  h1 { margin:0; font-size:18px; }
  p { color:#9db0d4; font-size:14px; }
  .wrap { padding:12px; display:grid; gap:10px; }
  input, button { width:100%; box-sizing:border-box; background:#15203a; color:#e8eefc;
    border:1px solid #2a3b63; border-radius:10px; padding:12px; font-size:16px; }
  button { cursor:pointer; background:#134e4a; border-color:#2dd4bf; }
  .hint { font-size:12px; color:#7b8cb0; }
  .card { background:#10182c; border:1px solid #1c2744; border-radius:12px; padding:12px; }
</style>
</head>
<body>
<header>
  <h1>Track one flight</h1>
  <p>Type a flight number. The screen times it to %HOME% and %AIRPORT%.</p>
</header>
<div class="wrap">
  <input id="cs" placeholder="Flight number  (JBU2230, UA577, B62230)" value="%CS%" autocapitalize="characters">
  <button id="save">Save flight</button>
  <p class="hint" id="status">Uses your Sky home pin and airport.</p>
  <div class="card hint">
    Home: %HOME%<br>
    Airport: %AIRPORT%<br>
    Change those pins in the Sky app.
  </div>
  <details>
    <summary>Home Wi-Fi</summary>
    <form action="/wifi" method="get" style="display:grid;gap:8px;margin-top:10px">
      <input name="ssid" placeholder="Wi-Fi name" required>
      <input name="pass" type="password" placeholder="Password">
      <button>Join Wi-Fi</button>
    </form>
  </details>
</div>
<script>
document.getElementById('save').onclick = async () => {
  const cs = document.getElementById('cs').value.trim();
  const r = await fetch('/save?cs=' + encodeURIComponent(cs));
  document.getElementById('status').textContent = r.ok ? 'Saved. Watch the device.' : 'Save failed';
};
</script>
</body>
</html>
)HTML";

static void sendIndex(httpd_req_t *req) {
  flightLoadPrefs();
  String html = INDEX_HTML;
  html.replace("%CS%", callsign);
  char home[48] = "not set";
  if (flightHasHome()) {
    const char *label = flightHomeLabel();
    if (label && label[0]) strncpy(home, label, sizeof(home) - 1);
  }
  html.replace("%HOME%", home);
  char apt[16] = "not set";
  if (flightHasAirport()) {
    float alat, alon;
    flightGetAirport(&alat, &alon, apt, sizeof(apt));
  }
  html.replace("%AIRPORT%", apt);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t indexHandler(httpd_req_t *req) {
  sendIndex(req);
  return ESP_OK;
}

static esp_err_t saveHandler(httpd_req_t *req) {
  size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing");
    return ESP_FAIL;
  }
  char *buf = (char *)malloc(len);
  if (!buf || httpd_req_get_url_query_str(req, buf, len) != ESP_OK) {
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  char enc[32] = {0};
  char raw[24] = {0};
  httpd_query_key_value(buf, "cs", enc, sizeof(enc));
  urlDecode(raw, sizeof(raw), enc);
  free(buf);
  trackSetCallsign(raw);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "ok");
}

static esp_err_t wifiHandler(httpd_req_t *req) {
  size_t len = httpd_req_get_url_query_len(req) + 1;
  char *buf = (char *)malloc(len);
  if (!buf || httpd_req_get_url_query_str(req, buf, len) != ESP_OK) {
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  char ssid_enc[64] = {0};
  char pass_enc[64] = {0};
  httpd_query_key_value(buf, "ssid", ssid_enc, sizeof(ssid_enc));
  httpd_query_key_value(buf, "pass", pass_enc, sizeof(pass_enc));
  free(buf);
  char ssid[33] = {0};
  char pass[65] = {0};
  urlDecode(ssid, sizeof(ssid), ssid_enc);
  urlDecode(pass, sizeof(pass), pass_enc);
  if (!ssid[0]) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
    return ESP_FAIL;
  }
  Preferences p;
  p.begin("webcam", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  portENTER_CRITICAL(&wifiMux);
  strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
  strncpy(pending_pass, pass, sizeof(pending_pass) - 1);
  wifi_pending = true;
  portEXIT_CRITICAL(&wifiMux);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "joining... watch the LCD");
}

void trackServerStart() {
  if (server) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.lru_purge_enable = true;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    server = NULL;
    return;
  }
  httpd_uri_t u1 = {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = NULL};
  httpd_uri_t u2 = {.uri = "/save", .method = HTTP_GET, .handler = saveHandler, .user_ctx = NULL};
  httpd_uri_t u3 = {.uri = "/wifi", .method = HTTP_GET, .handler = wifiHandler, .user_ctx = NULL};
  httpd_register_uri_handler(server, &u1);
  httpd_register_uri_handler(server, &u2);
  httpd_register_uri_handler(server, &u3);
}

void trackServerStop() {
  if (!server) return;
  httpd_stop(server);
  server = NULL;
}
