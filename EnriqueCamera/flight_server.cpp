#include "flight_server.h"
#include "board.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_http_server.h"

static httpd_handle_t server = NULL;
static portMUX_TYPE wifiMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool wifi_pending = false;
static char pending_ssid[33] = {0};
static char pending_pass[65] = {0};

static bool have_home = false;
static bool have_airport = false;
static float home_lat = 42.4125961f;
static float home_lon = -70.9912261f;
static float apt_lat = 42.3656f;
static float apt_lon = -71.0096f;
static char apt_name[12] = "BOS";
static char home_label[20] = "394 Ocean Ave";
static float pass_km = 8;
static uint8_t dir_filter = FLIGHT_FILTER_BOTH;

static const float kDefLat = 42.4125961f;
static const float kDefLon = -70.9912261f;
static const float kBosLat = 42.3656f;
static const float kBosLon = -71.0096f;

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

static void savePrefs();

void flightLoadPrefs() {
  Preferences p;
  p.begin("flight", true);
  have_home = p.getBool("home", false);
  home_lat = p.getFloat("lat", kDefLat);
  home_lon = p.getFloat("lon", kDefLon);
  have_airport = p.getBool("apt", false);
  apt_lat = p.getFloat("alat", kBosLat);
  apt_lon = p.getFloat("alon", kBosLon);
  String n = p.getString("aname", "BOS");
  strncpy(apt_name, n.c_str(), sizeof(apt_name) - 1);
  apt_name[sizeof(apt_name) - 1] = 0;
  String h = p.getString("hname", "394 Ocean Ave");
  strncpy(home_label, h.c_str(), sizeof(home_label) - 1);
  home_label[sizeof(home_label) - 1] = 0;
  pass_km = p.getFloat("rkm", 8);
  dir_filter = p.getUChar("filt", FLIGHT_FILTER_BOTH);
  p.end();
  if (pass_km < 2) pass_km = 2;
  if (pass_km > 40) pass_km = 40;
  if (dir_filter > FLIGHT_FILTER_OUT) dir_filter = FLIGHT_FILTER_BOTH;

  bool seed = false;
  if (!have_home || (home_lat == 0 && home_lon == 0)) {
    have_home = true;
    home_lat = kDefLat;
    home_lon = kDefLon;
    strncpy(home_label, "394 Ocean Ave", sizeof(home_label) - 1);
    seed = true;
  }
  if (!have_airport) {
    have_airport = true;
    apt_lat = kBosLat;
    apt_lon = kBosLon;
    strncpy(apt_name, "BOS", sizeof(apt_name) - 1);
    seed = true;
  }
  if (seed) savePrefs();
}

static void savePrefs() {
  Preferences p;
  p.begin("flight", false);
  p.putBool("home", have_home);
  p.putFloat("lat", home_lat);
  p.putFloat("lon", home_lon);
  p.putBool("apt", have_airport);
  p.putFloat("alat", apt_lat);
  p.putFloat("alon", apt_lon);
  p.putString("aname", apt_name);
  p.putString("hname", home_label);
  p.putFloat("rkm", pass_km);
  p.putUChar("filt", dir_filter);
  p.end();
}

bool flightHasHome() { return have_home; }
void flightGetHome(float *lat, float *lon) {
  *lat = home_lat;
  *lon = home_lon;
}
const char *flightHomeLabel() { return home_label; }
void flightSetHome(float lat, float lon) {
  home_lat = lat;
  home_lon = lon;
  have_home = true;
  if (fabsf(lat - kDefLat) < 0.0003f && fabsf(lon - kDefLon) < 0.0003f) {
    strncpy(home_label, "394 Ocean Ave", sizeof(home_label) - 1);
  } else {
    strncpy(home_label, "custom pin", sizeof(home_label) - 1);
  }
  savePrefs();
}
bool flightHasAirport() { return have_airport; }
void flightGetAirport(float *lat, float *lon, char *name, size_t nameLen) {
  *lat = apt_lat;
  *lon = apt_lon;
  strncpy(name, apt_name, nameLen - 1);
  name[nameLen - 1] = 0;
}
void flightSetAirport(float lat, float lon, const char *name) {
  apt_lat = lat;
  apt_lon = lon;
  strncpy(apt_name, name && name[0] ? name : "AIR", sizeof(apt_name) - 1);
  have_airport = true;
  savePrefs();
}
void flightClearAirport() {
  have_airport = false;
  apt_name[0] = 0;
  savePrefs();
}
float flightPassRadiusKm() { return pass_km; }
void flightSetPassRadiusKm(float km) {
  pass_km = km;
  if (pass_km < 2) pass_km = 2;
  if (pass_km > 40) pass_km = 40;
  savePrefs();
}
uint8_t flightGetFilter() { return dir_filter; }
void flightSetFilter(uint8_t mode) {
  if (mode > FLIGHT_FILTER_OUT) mode = FLIGHT_FILTER_BOTH;
  dir_filter = mode;
  savePrefs();
}

bool flightTakePendingWifi(char *ssid, size_t ssidLen, char *pass, size_t passLen) {
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
<title>Sky pin</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>
  :root { color-scheme: dark; }
  body { margin:0; font-family:ui-sans-serif,system-ui,sans-serif; background:#07101c; color:#e8eefc; }
  header { padding:14px 16px; border-bottom:1px solid #1c2744; }
  h1 { margin:0; font-size:18px; }
  p { color:#9db0d4; font-size:14px; }
  .wrap { padding:12px; display:grid; gap:10px; }
  #map { height:260px; border-radius:14px; border:1px solid #1c2744; background:#10182c; }
  input, button { width:100%; box-sizing:border-box; background:#15203a; color:#e8eefc;
    border:1px solid #2a3b63; border-radius:10px; padding:10px 12px; font-size:15px; }
  button { cursor:pointer; }
  .row { display:flex; gap:8px; }
  .row > * { flex:1; }
  .ok { background:#134e4a; border-color:#2dd4bf; }
  .ghost { background:transparent; }
  .hint { font-size:12px; color:#7b8cb0; }
  .seg button { font-size:13px; padding:10px 6px; }
</style>
</head>
<body>
<header>
  <h1>Sky settings</h1>
  <p>Default pin is 394 Ocean Ave, Revere MA. Sky only shows flights arriving at or leaving the airport pin (BOS by default).</p>
</header>
<div class="wrap">
  <p class="hint">Show flights</p>
  <div class="row seg">
    <button type="button" id="fIn" class="ghost">Arriving</button>
    <button type="button" id="fOut" class="ghost">Departing</button>
    <button type="button" id="fBoth" class="ghost">Both</button>
  </div>
  <div id="map"></div>
  <div class="row">
    <button type="button" id="modeHome" class="ok">Pin: my spot</button>
    <button type="button" id="modeApt" class="ghost">Pin: airport</button>
  </div>
  <input id="gmaps" placeholder="Paste a Google Maps link here">
  <div class="row">
    <input id="lat" placeholder="latitude" inputmode="decimal">
    <input id="lon" placeholder="longitude" inputmode="decimal">
  </div>
  <div class="row">
    <input id="alat" placeholder="airport lat">
    <input id="alon" placeholder="airport lon">
  </div>
  <input id="aname" placeholder="Airport name (BOS, Logan…)">
  <input id="radius" type="number" min="2" max="40" step="1" value="%RADIUS%" placeholder="Pass radius km">
  <p class="hint">Pass radius is how far counts as “passing by you” (default 8 km).</p>
  <button id="save" class="ok">Save to the device</button>
  <p class="hint" id="status">Mode: my spot. Current home: %HOME%</p>
  <details>
    <summary>Home Wi-Fi (needed for live flights)</summary>
    <form action="/wifi" method="get" style="display:grid;gap:8px;margin-top:10px">
      <input name="ssid" placeholder="Wi-Fi name" required>
      <input name="pass" type="password" placeholder="Password">
      <button>Join Wi-Fi</button>
    </form>
  </details>
</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
let mode = 'home';
const home = {lat:%HLAT%, lon:%HLON%};
const apt = {lat:%ALAT%, lon:%ALON%};
function parseMaps(s) {
  let m = s.match(/@(-?\d+\.\d+),(-?\d+\.\d+)/) || s.match(/[?&]q=(-?\d+\.\d+),(-?\d+\.\d+)/)
       || s.match(/!3d(-?\d+\.\d+)!4d(-?\d+\.\d+)/) || s.match(/(-?\d+\.\d+)\s*,\s*(-?\d+\.\d+)/);
  return m ? {lat:+m[1], lon:+m[2]} : null;
}
const start = (home.lat || home.lon) ? [home.lat, home.lon] : [42.4126, -70.9912];
let map, marker, amarker;
try {
  map = L.map('map').setView(start, (home.lat||home.lon) ? 13 : 10);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {maxZoom:19}).addTo(map);
  if (home.lat || home.lon) marker = L.marker([home.lat, home.lon]).addTo(map);
  if (apt.lat || apt.lon) amarker = L.marker([apt.lat, apt.lon], {title:'airport'}).addTo(map);
  map.on('click', e => apply(e.latlng.lat, e.latlng.lng));
} catch (e) {
  document.getElementById('map').innerHTML = '<p style="padding:16px">Map tiles need home Wi-Fi. Paste a Google Maps link instead.</p>';
}
function apply(lat, lon) {
  lat = +lat; lon = +lon;
  if (mode === 'apt') {
    document.getElementById('alat').value = lat.toFixed(5);
    document.getElementById('alon').value = lon.toFixed(5);
    if (map) {
      if (amarker) amarker.setLatLng([lat, lon]);
      else amarker = L.marker([lat, lon]).addTo(map);
    }
  } else {
    document.getElementById('lat').value = lat.toFixed(5);
    document.getElementById('lon').value = lon.toFixed(5);
    if (map) {
      if (marker) marker.setLatLng([lat, lon]);
      else marker = L.marker([lat, lon]).addTo(map);
    }
  }
}
document.getElementById('modeHome').onclick = () => {
  mode = 'home';
  document.getElementById('modeHome').className = 'ok';
  document.getElementById('modeApt').className = 'ghost';
  document.getElementById('status').textContent = 'Mode: my spot — tap the map or paste a Google pin.';
};
document.getElementById('modeApt').onclick = () => {
  mode = 'apt';
  document.getElementById('modeApt').className = 'ok';
  document.getElementById('modeHome').className = 'ghost';
  document.getElementById('status').textContent = 'Mode: airport — drop a pin on the airport you care about.';
};
document.getElementById('gmaps').addEventListener('change', () => {
  const p = parseMaps(document.getElementById('gmaps').value);
  if (p) apply(p.lat, p.lon);
});
document.getElementById('save').onclick = async () => {
  const q = new URLSearchParams({
    lat: document.getElementById('lat').value,
    lon: document.getElementById('lon').value,
    alat: document.getElementById('alat').value,
    alon: document.getElementById('alon').value,
    aname: document.getElementById('aname').value,
    radius: document.getElementById('radius').value,
    filter: window.skyFilter
  });
  const r = await fetch('/save?' + q.toString());
  document.getElementById('status').textContent = r.ok ? 'Saved. Look at the device screen.' : 'Save failed';
};
window.skyFilter = %FILTER%;
function setFilter(v) {
  window.skyFilter = v;
  document.getElementById('fIn').className = v === 1 ? 'ok' : 'ghost';
  document.getElementById('fOut').className = v === 2 ? 'ok' : 'ghost';
  document.getElementById('fBoth').className = v === 0 ? 'ok' : 'ghost';
}
document.getElementById('fIn').onclick = () => setFilter(1);
document.getElementById('fOut').onclick = () => setFilter(2);
document.getElementById('fBoth').onclick = () => setFilter(0);
setFilter(window.skyFilter);
if (home.lat || home.lon) apply(home.lat, home.lon);
mode = 'apt';
if (apt.lat || apt.lon) apply(apt.lat, apt.lon);
mode = 'home';
document.getElementById('aname').value = '%ANAME%';
</script>
</body>
</html>
)HTML";

static void sendIndex(httpd_req_t *req) {
  String html = INDEX_HTML;
  char buf[64];
  if (have_home && home_label[0]) {
    snprintf(buf, sizeof(buf), "%s (%.4f, %.4f)", home_label, home_lat, home_lon);
  } else {
    snprintf(buf, sizeof(buf), have_home ? "%.4f, %.4f" : "not set", home_lat, home_lon);
  }
  html.replace("%HOME%", buf);
  snprintf(buf, sizeof(buf), "%.2f", pass_km);
  html.replace("%RADIUS%", buf);
  snprintf(buf, sizeof(buf), "%.5f", have_home ? home_lat : 0);
  html.replace("%HLAT%", buf);
  snprintf(buf, sizeof(buf), "%.5f", have_home ? home_lon : 0);
  html.replace("%HLON%", buf);
  snprintf(buf, sizeof(buf), "%.5f", have_airport ? apt_lat : 0);
  html.replace("%ALAT%", buf);
  snprintf(buf, sizeof(buf), "%.5f", have_airport ? apt_lon : 0);
  html.replace("%ALON%", buf);
  html.replace("%ANAME%", apt_name[0] ? apt_name : "");
  snprintf(buf, sizeof(buf), "%u", (unsigned)dir_filter);
  html.replace("%FILTER%", buf);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t indexHandler(httpd_req_t *req) {
  sendIndex(req);
  return ESP_OK;
}

static float queryFloat(const char *buf, const char *key) {
  char tmp[32] = {0};
  if (httpd_query_key_value(buf, key, tmp, sizeof(tmp)) != ESP_OK) return NAN;
  return atof(tmp);
}

static esp_err_t saveHandler(httpd_req_t *req) {
  size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    return ESP_FAIL;
  }
  char *buf = (char *)malloc(len);
  if (!buf || httpd_req_get_url_query_str(req, buf, len) != ESP_OK) {
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  float lat = queryFloat(buf, "lat");
  float lon = queryFloat(buf, "lon");
  float alat = queryFloat(buf, "alat");
  float alon = queryFloat(buf, "alon");
  float r = queryFloat(buf, "radius");
  float filt = queryFloat(buf, "filter");
  char name_enc[32] = {0};
  char name[16] = {0};
  httpd_query_key_value(buf, "aname", name_enc, sizeof(name_enc));
  urlDecode(name, sizeof(name), name_enc);
  free(buf);

  if (!isnan(lat) && !isnan(lon) && !(lat == 0 && lon == 0)) {
    flightSetHome(lat, lon);
  }
  if (!isnan(alat) && !isnan(alon) && !(alat == 0 && alon == 0)) {
    flightSetAirport(alat, alon, name);
  }
  if (!isnan(r) && r > 0) flightSetPassRadiusKm(r);
  if (!isnan(filt)) flightSetFilter((uint8_t)lroundf(filt));
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

void flightServerStart() {
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

void flightServerStop() {
  if (!server) return;
  httpd_stop(server);
  server = NULL;
}
