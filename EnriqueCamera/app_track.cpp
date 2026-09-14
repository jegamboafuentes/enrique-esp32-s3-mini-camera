#include "apps.h"
#include "board.h"
#include "flight_server.h"
#include "track_server.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <math.h>
#include <string.h>
#include <ESPmDNS.h>

static const char *AP_SSID = "ESP32-Track";
static const char *MDNS_NAME = "esp32-track";
static const float DEG2RAD = 0.01745329252f;
static const float KM_PER_DEG = 111.32f;

static bool sta_mode = false;
static bool started_ap = false;
static bool dirty = true;
static uint32_t last_poll = 0;
static uint32_t last_draw = 0;
static uint32_t last_tick = 0;
static char status_line[40] = "starting";
static char wanted[12] = "";
static void drawScreen();

struct Watch {
  bool live;
  bool route_ok;
  char callsign[16];
  char hex[8];
  char airline[18];
  char orig_iata[8];
  char dest_iata[8];
  char orig_city[16];
  char dest_city[16];
  float orig_lat, orig_lon, dest_lat, dest_lon;
  float lat, lon, track, alt_ft, gs_kt;
  float dist_home, t_home_s, d_cpa_km;
  float dist_apt, t_apt_s;
  uint32_t stamp_ms;
};

static Watch watch = {};

static void setStatus(const char *s) {
  strncpy(status_line, s, sizeof(status_line) - 1);
  status_line[sizeof(status_line) - 1] = 0;
  dirty = true;
  drawScreen();
}

static void clipCopy(char *dst, size_t n, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

static void stripCs(char *dst, size_t n, const char *src) {
  size_t o = 0;
  for (size_t i = 0; src && src[i] && o + 1 < n; i++) {
    char c = src[i];
    if (c == ' ' || c == '-') continue;
    if (c >= 'a' && c <= 'z') c -= 32;
    dst[o++] = c;
  }
  dst[o] = 0;
}

static bool sameFlight(const char *a, const char *b) {
  char na[16], nb[16];
  stripCs(na, sizeof(na), a);
  stripCs(nb, sizeof(nb), b);
  if (!na[0] || !nb[0]) return false;
  if (!strcmp(na, nb)) return true;
  char pa[8] = {0}, pb[8] = {0};
  int ia = 0, ib = 0, i = 0;
  while (na[i] >= 'A' && na[i] <= 'Z' && ia < 7) pa[ia++] = na[i++];
  int numa = atoi(na + i);
  i = 0;
  while (nb[i] >= 'A' && nb[i] <= 'Z' && ib < 7) pb[ib++] = nb[i++];
  int numb = atoi(nb + i);
  return pa[0] && !strcmp(pa, pb) && numa > 0 && numa == numb;
}

static void disableWifiSleep() {
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
}

static void fmtTime(char *out, size_t n, float sec) {
  if (!isfinite(sec)) {
    snprintf(out, n, "--");
    return;
  }
  if (sec < 0) sec = 0;
  long s = lroundf(sec);
  if (s < 90) snprintf(out, n, "%lds", s);
  else if (s < 3600) snprintf(out, n, "%ldm %lds", s / 60, s % 60);
  else snprintf(out, n, "%ldh %ldm", s / 3600, (s / 60) % 60);
}

static void enu(float lat, float lon, float lat0, float lon0, float *east_km, float *north_km) {
  *north_km = (lat - lat0) * KM_PER_DEG;
  *east_km = (lon - lon0) * KM_PER_DEG * cosf(lat0 * DEG2RAD);
}

static float distKm(float lat1, float lon1, float lat2, float lon2) {
  float e, n;
  enu(lat1, lon1, lat2, lon2, &e, &n);
  return sqrtf(e * e + n * n);
}

static void startAp() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  disableWifiSleep();
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  delay(120);
  sta_mode = false;
  started_ap = true;
}

static bool connectSavedWifi(uint32_t timeout_ms) {
  Preferences p;
  p.begin("webcam", true);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();
  if (ssid.isEmpty()) return false;
  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  delay(60);
  WiFi.mode(WIFI_STA);
  disableWifiSleep();
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(MDNS_NAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) delay(200);
  if (WiFi.status() != WL_CONNECTED) return false;
  sta_mode = true;
  started_ap = false;
  MDNS.end();
  MDNS.begin(MDNS_NAME);
  return true;
}

static int httpGetJson(const char *url, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);
  http.setTimeout(16000);
  http.setConnectTimeout(12000);
  http.setUserAgent("Mozilla/5.0 (compatible; EnriqueCamera/1.0)");
  if (!http.begin(client, url)) return 0;
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  Serial.printf("Track GET %d %s\n", code, url);
  if (code != HTTP_CODE_OK) {
    http.end();
    return code;
  }
  String body = http.getString();
  http.end();
  if (body.length() < 2) return -1;
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) return -2;
  return HTTP_CODE_OK;
}

static void fillAirport(JsonVariant v, char *iata, size_t iataN, char *city, size_t cityN, float *lat, float *lon) {
  const char *code = v["iata_code"] | "";
  if (!code[0]) code = v["icao_code"] | "";
  clipCopy(iata, iataN, code);
  const char *muni = v["municipality"] | "";
  if (!muni[0]) muni = v["name"] | "";
  clipCopy(city, cityN, muni);
  *lat = v["latitude"].isNull() ? 0 : v["latitude"].as<float>();
  *lon = v["longitude"].isNull() ? 0 : v["longitude"].as<float>();
}

static bool lookupRoute(const char *cs) {
  char url[96];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);
  JsonDocument doc;
  if (httpGetJson(url, doc) != HTTP_CODE_OK) return false;
  JsonVariant fr = doc["response"]["flightroute"];
  if (fr.isNull()) return false;
  const char *icao = fr["callsign_icao"] | cs;
  clipCopy(watch.callsign, sizeof(watch.callsign), icao);
  const char *al = fr["airline"]["name"] | "";
  clipCopy(watch.airline, sizeof(watch.airline), al);
  fillAirport(fr["origin"], watch.orig_iata, sizeof(watch.orig_iata), watch.orig_city, sizeof(watch.orig_city),
              &watch.orig_lat, &watch.orig_lon);
  fillAirport(fr["destination"], watch.dest_iata, sizeof(watch.dest_iata), watch.dest_city, sizeof(watch.dest_city),
              &watch.dest_lat, &watch.dest_lon);
  watch.route_ok = watch.orig_iata[0] || watch.dest_iata[0];
  return watch.route_ok;
}

static bool applyPosition(float clat, float clon, float gs_kt, float track, float alt_ft) {
  float vel = gs_kt * 0.514444f;
  float lat = 0, lon = 0;
  float dist = NAN, t_home = NAN, d_cpa = NAN;
  if (flightHasHome()) {
    flightGetHome(&lat, &lon);
    float east, north;
    enu(clat, clon, lat, lon, &east, &north);
    dist = sqrtf(east * east + north * north);
    float ve = vel * sinf(track * DEG2RAD) * 0.001f;
    float vn = vel * cosf(track * DEG2RAD) * 0.001f;
    float v2 = ve * ve + vn * vn;
    d_cpa = dist;
    float closing = (v2 > 1e-8f) ? -(east * ve + north * vn) : 0;
    if (gs_kt >= 50 && vel > 1) {
      if (dist > 40) {
        if (closing > 0) t_home = (dist * 1000.0f) / vel;
      } else if (v2 > 1e-8f) {
        t_home = -(east * ve + north * vn) / v2;
        float ce = east + ve * t_home;
        float cn = north + vn * t_home;
        d_cpa = sqrtf(ce * ce + cn * cn);
      }
    }
  }

  float t_apt = NAN;
  float d_apt = NAN;
  float dlat = watch.dest_lat, dlon = watch.dest_lon;
  if (dlat == 0 && dlon == 0 && flightHasAirport()) {
    char aname[12];
    flightGetAirport(&dlat, &dlon, aname, sizeof(aname));
  }
  if ((dlat != 0 || dlon != 0) && vel > 1) {
    d_apt = distKm(clat, clon, dlat, dlon);
    t_apt = (d_apt * 1000.0f) / vel;
  }

  watch.live = true;
  watch.lat = clat;
  watch.lon = clon;
  watch.track = track;
  watch.alt_ft = alt_ft;
  watch.gs_kt = gs_kt;
  watch.dist_home = dist;
  watch.t_home_s = t_home;
  watch.d_cpa_km = d_cpa;
  watch.dist_apt = d_apt;
  watch.t_apt_s = t_apt;
  watch.stamp_ms = millis();
  return true;
}

static bool ingestRow(JsonVariant row, const char *cs) {
  if (row["lat"].isNull() || row["lon"].isNull()) return false;
  const char *hx = row["hex"] | "";
  if (hx[0]) clipCopy(watch.hex, sizeof(watch.hex), hx);
  const char *fl = row["flight"] | cs;
  char trimmed[16];
  clipCopy(trimmed, sizeof(trimmed), fl);
  for (char *p = trimmed; *p; p++) {
    if (*p == ' ') {
      *p = 0;
      break;
    }
  }
  if (trimmed[0]) clipCopy(watch.callsign, sizeof(watch.callsign), trimmed);
  if (row["alt_baro"].is<const char *>()) {
    watch.live = true;
    watch.alt_ft = 0;
    watch.gs_kt = row["gs"].isNull() ? 0 : row["gs"].as<float>();
    watch.lat = row["lat"].as<float>();
    watch.lon = row["lon"].as<float>();
    watch.t_home_s = NAN;
    watch.t_apt_s = NAN;
    if (flightHasHome()) {
      float lat, lon;
      flightGetHome(&lat, &lon);
      watch.dist_home = distKm(watch.lat, watch.lon, lat, lon);
    }
    watch.stamp_ms = millis();
    return true;
  }
  float gs = row["gs"].isNull() ? 0 : row["gs"].as<float>();
  float track = row["track"].isNull() ? row["true_heading"] | 0 : row["track"].as<float>();
  float alt = row["alt_baro"].as<float>();
  return applyPosition(row["lat"].as<float>(), row["lon"].as<float>(), gs, track, alt);
}

static bool scanAc(JsonDocument &doc, const char *cs) {
  JsonArray acs = doc["ac"].as<JsonArray>();
  if (acs.isNull()) acs = doc["aircraft"].as<JsonArray>();
  for (JsonVariant row : acs) {
    const char *fl = row["flight"] | "";
    if (sameFlight(fl, cs) || sameFlight(fl, wanted)) return ingestRow(row, cs);
  }
  return false;
}

static bool pollCallsign(const char *cs) {
  char url[160];
  const char *fmts[] = {
    "https://opendata.adsb.fi/api/v2/callsign/%s",
    "https://api.adsb.lol/v2/callsign/%s",
    "https://api.airplanes.live/v2/callsign/%s",
  };
  for (const char *fmt : fmts) {
    snprintf(url, sizeof(url), fmt, cs);
    JsonDocument doc;
    if (httpGetJson(url, doc) == HTTP_CODE_OK && scanAc(doc, cs)) return true;
  }
  return false;
}

static bool pollHex(const char *hex) {
  if (!hex || !hex[0]) return false;
  char url[128];
  const char *fmts[] = {
    "https://opendata.adsb.fi/api/v2/hex/%s",
    "https://api.adsb.lol/v2/hex/%s",
    "https://opendata.adsb.fi/api/v2/icao/%s",
  };
  for (const char *fmt : fmts) {
    snprintf(url, sizeof(url), fmt, hex);
    JsonDocument doc;
    if (httpGetJson(url, doc) == HTTP_CODE_OK && scanAc(doc, wanted)) return true;
  }
  return false;
}

static bool sameHex(const char *a, const char *b) {
  if (!a || !b || !a[0] || !b[0]) return false;
  char na[12], nb[12];
  stripCs(na, sizeof(na), a);
  stripCs(nb, sizeof(nb), b);
  return na[0] && !strcmp(na, nb);
}

static bool ingestOpenSkyArray(JsonArray ac, const char *cs) {
  if (ac.size() < 11) return false;
  const char *icao = ac[0] | "";
  const char *fl = ac[1] | "";
  if (!sameFlight(fl, cs) && !sameFlight(fl, wanted) && !sameHex(icao, watch.hex)) return false;
  if (ac[5].isNull() || ac[6].isNull()) return false;
  if (icao[0]) clipCopy(watch.hex, sizeof(watch.hex), icao);
  if (fl[0]) {
    char trimmed[16];
    clipCopy(trimmed, sizeof(trimmed), fl);
    for (char *p = trimmed; *p; p++) {
      if (*p == ' ') {
        *p = 0;
        break;
      }
    }
    if (trimmed[0]) clipCopy(watch.callsign, sizeof(watch.callsign), trimmed);
  } else {
    clipCopy(watch.callsign, sizeof(watch.callsign), cs);
  }
  float clon = ac[5].as<float>();
  float clat = ac[6].as<float>();
  float alt_m = ac[7].isNull() ? 0 : ac[7].as<float>();
  float vel = ac[9].isNull() ? 0 : ac[9].as<float>();
  float track = ac[10].isNull() ? 0 : ac[10].as<float>();
  return applyPosition(clat, clon, vel * 1.94384f, track, alt_m * 3.28084f);
}

static bool pollOpenSkyIcao(const char *hex, const char *cs) {
  if (!hex || !hex[0]) return false;
  char url[160];
  snprintf(url, sizeof(url), "https://opensky-network.org/api/states/all?icao24=%s", hex);
  JsonDocument doc;
  if (httpGetJson(url, doc) != HTTP_CODE_OK) return false;
  JsonArray states = doc["states"].as<JsonArray>();
  if (states.isNull() || !states.size()) return false;
  return ingestOpenSkyArray(states[0].as<JsonArray>(), cs);
}

static int findNeedle(const String &body, const char *cs) {
  if (!cs || !cs[0]) return -1;
  char quoted[24];
  snprintf(quoted, sizeof(quoted), "\"%s", cs);
  return body.indexOf(quoted);
}

static bool parseOpenSkySlice(const String &body, int needle, const char *cs) {
  int start = needle;
  while (start > 0 && !(body[start] == '[' && start + 2 < (int)body.length() && body[start + 1] == '"')) start--;
  if (start < 0 || body[start] != '[') return false;
  int end = start + 1, depth = 1;
  for (; end < (int)body.length() && depth; end++) {
    if (body[end] == '[') depth++;
    else if (body[end] == ']') depth--;
  }
  JsonDocument row;
  if (deserializeJson(row, body.c_str() + start, end - start)) return false;
  return ingestOpenSkyArray(row.as<JsonArray>(), cs);
}

static bool pollOpenSky(float lat, float lon, float deg, const char *cs) {
  if (lat == 0 && lon == 0) return false;
  char url[220];
  snprintf(url, sizeof(url),
           "https://opensky-network.org/api/states/all?lamin=%.2f&lomin=%.2f&lamax=%.2f&lomax=%.2f",
           lat - deg, lon - deg, lat + deg, lon + deg);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);
  http.setTimeout(14000);
  http.setUserAgent("Mozilla/5.0 (compatible; EnriqueCamera/1.0)");
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  Serial.printf("Track OpenSky %d\n", code);
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  int idx = findNeedle(body, cs);
  if (idx < 0) idx = findNeedle(body, wanted);
  if (idx < 0 && watch.hex[0]) {
    char hx[16];
    snprintf(hx, sizeof(hx), "\"%s\"", watch.hex);
    idx = body.indexOf(hx);
  }
  if (idx < 0) return false;
  return parseOpenSkySlice(body, idx, cs);
}

static void rememberHex() {
  if (!watch.hex[0] || !wanted[0]) return;
  Preferences p;
  p.begin("track", false);
  p.putString("hex", watch.hex);
  p.putString("hexcs", wanted);
  p.end();
}

static void loadSavedHex() {
  Preferences p;
  p.begin("track", true);
  String hx = p.getString("hex", "");
  String cs = p.getString("hexcs", "");
  p.end();
  if (hx.length() && cs.length() && sameFlight(cs.c_str(), wanted)) {
    clipCopy(watch.hex, sizeof(watch.hex), hx.c_str());
  }
}

static bool pollTraffic() {
  strncpy(wanted, trackGetCallsign(), sizeof(wanted) - 1);
  wanted[sizeof(wanted) - 1] = 0;
  if (!wanted[0]) {
    setStatus("set a flight on phone");
    watch = {};
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("need home Wi-Fi");
    return false;
  }

  if (!watch.route_ok) {
    setStatus("looking up route");
    lookupRoute(wanted);
  }
  if (!watch.callsign[0]) clipCopy(watch.callsign, sizeof(watch.callsign), wanted);
  if (!watch.hex[0]) loadSavedHex();
  const char *live_cs = watch.callsign[0] ? watch.callsign : wanted;

  bool stale_ok = watch.live && (millis() - watch.stamp_ms < 180000UL);
  float last_lat = watch.lat, last_lon = watch.lon;

  setStatus("finding aircraft");
  bool found = false;
  if (watch.hex[0]) {
    setStatus("OpenSky hex");
    found = pollOpenSkyIcao(watch.hex, live_cs);
    if (!found) found = pollHex(watch.hex);
  }
  if (!found) {
    setStatus("adsb.fi callsign");
    found = pollCallsign(live_cs);
  }
  if (!found && strcmp(live_cs, wanted) != 0) found = pollCallsign(wanted);

  if (!found && (last_lat || last_lon)) {
    setStatus("OpenSky last pos");
    found = pollOpenSky(last_lat, last_lon, 4.0f, live_cs);
  }
  if (!found && (watch.orig_lat || watch.orig_lon)) {
    setStatus("OpenSky origin");
    found = pollOpenSky(watch.orig_lat, watch.orig_lon, 4.5f, live_cs);
  }
  if (!found && (watch.orig_lat || watch.dest_lat) && !(last_lat || last_lon)) {
    float plat = watch.orig_lat + 0.2f * (watch.dest_lat - watch.orig_lat);
    float plon = watch.orig_lon + 0.2f * (watch.dest_lon - watch.orig_lon);
    setStatus("OpenSky en route");
    found = pollOpenSky(plat, plon, 5.0f, live_cs);
  }

  if (found) {
    rememberHex();
    setStatus("live ADS-B");
  } else if (stale_ok) {
    watch.live = true;
    watch.lat = last_lat;
    watch.lon = last_lon;
    setStatus("last ADS-B");
  } else {
    watch.live = false;
    if (watch.route_ok) setStatus("no live position");
    else setStatus("flight not found");
  }
  dirty = true;
  return true;
}

static float liveHome() {
  if (!watch.live) return NAN;
  float dt = (millis() - watch.stamp_ms) / 1000.0f;
  if (!isfinite(watch.t_home_s)) return NAN;
  return watch.t_home_s - dt;
}

static float liveApt() {
  if (!watch.live || !isfinite(watch.t_apt_s)) return NAN;
  float dt = (millis() - watch.stamp_ms) / 1000.0f;
  return watch.t_apt_s - dt;
}

static void drawArriveIcon(int cx, int cy, int s, uint16_t c) {
  gfx->fillTriangle(cx + s / 2, cy + s / 5, cx - s / 3, cy - s / 5, cx - s / 5, cy + s / 3, c);
  gfx->fillRoundRect(cx - s / 2, cy + s / 2, s, 3, 1, c);
}

static void drawPinIcon(int cx, int cy, int s, uint16_t c) {
  gfx->fillCircle(cx, cy - s / 6, s / 4, c);
  gfx->fillTriangle(cx, cy + s / 3, cx - s / 5, cy - s / 8, cx + s / 5, cy - s / 8, c);
  gfx->fillCircle(cx, cy - s / 6, s / 10, gfx->color565(5, 12, 22));
}

static void drawScreen() {
  gfx->fillScreen(gfx->color565(5, 12, 22));
  gfx->fillRect(0, 0, LCD_W, 36, gfx->color565(8, 24, 44));
  gfx->setTextSize(2);
  gfx->setTextColor(gfx->color565(255, 176, 80));
  gfx->setCursor(12, 10);
  gfx->print("FLIGHT");
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(140, 170, 200));
  gfx->setCursor(110, 14);
  if (WiFi.status() == WL_CONNECTED) gfx->print(WiFi.localIP());
  else if (started_ap) gfx->print("192.168.4.1");
  else gfx->print("no wifi");

  const char *cs = watch.callsign[0] ? watch.callsign : (wanted[0] ? wanted : trackGetCallsign());
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(12, 48);
  gfx->print(cs[0] ? cs : "--------");

  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(160, 180, 210));
  gfx->setCursor(12, 70);
  if (watch.airline[0]) gfx->print(watch.airline);
  else gfx->print("Set flight from the phone");

  gfx->fillRoundRect(12, 86, 216, 44, 12, gfx->color565(16, 28, 48));
  gfx->setTextColor(gfx->color565(90, 210, 160));
  gfx->setCursor(22, 96);
  gfx->print(watch.orig_city[0] ? watch.orig_city : (watch.orig_iata[0] ? watch.orig_iata : "from --"));
  gfx->setTextColor(gfx->color565(255, 176, 80));
  gfx->setCursor(22, 112);
  gfx->print(watch.orig_iata[0] ? watch.orig_iata : "");
  gfx->setTextColor(gfx->color565(140, 160, 190));
  gfx->setCursor(108, 104);
  gfx->print(">");
  gfx->setTextColor(gfx->color565(80, 220, 255));
  gfx->setCursor(128, 96);
  gfx->print(watch.dest_city[0] ? watch.dest_city : (watch.dest_iata[0] ? watch.dest_iata : "to --"));
  gfx->setCursor(128, 112);
  gfx->print(watch.dest_iata[0] ? watch.dest_iata : "");

  char tbuf[16];
  float th = liveHome();
  gfx->fillRoundRect(12, 138, 216, 70, 14, gfx->color565(12, 36, 32));
  drawPinIcon(34, 164, 18, gfx->color565(90, 210, 160));
  gfx->setTextColor(gfx->color565(90, 210, 160));
  gfx->setCursor(56, 148);
  gfx->print("OVER YOU");
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(56, 164);
  if (!watch.live) gfx->print(watch.route_ok ? "no ADS-B" : "wait");
  else if (!isfinite(th)) gfx->print("en route");
  else if (th < 0 && th > -25 && watch.dist_home < 40) gfx->print("NOW");
  else if (th < -25 && watch.dist_home < 40) gfx->print("passed");
  else {
    fmtTime(tbuf, sizeof(tbuf), th);
    gfx->print(tbuf);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(160, 190, 180));
  gfx->setCursor(56, 188);
  const char *home = flightHomeLabel();
  if (watch.live && isfinite(watch.dist_home))
    gfx->printf("%s   %.0f km", home && home[0] ? home : "home", watch.dist_home);
  else gfx->print(home && home[0] ? home : "home pin");

  float ta = liveApt();
  gfx->fillRoundRect(12, 216, 216, 70, 14, gfx->color565(12, 28, 46));
  drawArriveIcon(34, 244, 16, gfx->color565(80, 220, 255));
  gfx->setTextColor(gfx->color565(80, 220, 255));
  gfx->setCursor(56, 226);
  char aname[12] = "";
  float alat, alon;
  if (flightHasAirport()) flightGetAirport(&alat, &alon, aname, sizeof(aname));
  gfx->print("TO AIRPORT");
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(56, 242);
  if (!watch.live) gfx->print(watch.route_ok ? "no ADS-B" : "wait");
  else if (watch.alt_ft == 0 && watch.gs_kt < 40) gfx->print("on ground");
  else if (!isfinite(ta)) gfx->print("en route");
  else if (ta < 0 && ta > -30) gfx->print("NOW");
  else {
    fmtTime(tbuf, sizeof(tbuf), ta);
    gfx->print(tbuf);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(160, 180, 210));
  gfx->setCursor(56, 266);
  if (watch.dest_iata[0]) gfx->printf("%s  %s", watch.dest_city[0] ? watch.dest_city : watch.dest_iata, watch.dest_iata);
  else if (aname[0]) gfx->print(aname);
  else gfx->print("set airport in Sky");

  gfx->setTextColor(gfx->color565(110, 130, 160));
  gfx->setCursor(12, 304);
  gfx->print(status_line);
  gfx->setCursor(150, 304);
  gfx->print("BOOT=Home");
  last_draw = millis();
  dirty = false;
}

void trackEnter() {
  watch = {};
  dirty = true;
  last_poll = 0;
  sta_mode = false;
  started_ap = false;
  flightLoadPrefs();
  trackLoadPrefs();
  strncpy(wanted, trackGetCallsign(), sizeof(wanted) - 1);
  gfx->fillScreen(gfx->color565(5, 12, 22));
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(16, 80);
  gfx->print("Flight");
  gfx->setTextSize(1);
  gfx->setCursor(16, 110);
  gfx->print("joining Wi-Fi...");

  if (WiFi.status() == WL_CONNECTED) {
    sta_mode = true;
    MDNS.begin(MDNS_NAME);
  } else if (!connectSavedWifi(18000)) {
    startAp();
  }
  trackServerStart();
  setStatus(sta_mode ? "wifi ok" : "AP ESP32-Track");
  drawScreen();
}

void trackLoop(bool tapped, uint16_t tx, uint16_t ty) {
  (void)tx;
  (void)ty;
  char ssid[33] = {0};
  char pass[65] = {0};
  if (trackTakePendingWifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
    gfx->fillScreen(gfx->color565(5, 12, 22));
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(16, 80);
    gfx->print("joining...");
    WiFi.softAPdisconnect(true);
    Preferences p;
    p.begin("webcam", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
    if (connectSavedWifi(20000)) {
      trackServerStop();
      delay(40);
      trackServerStart();
      setStatus("wifi ok");
    } else {
      startAp();
      setStatus("join failed");
    }
    dirty = true;
  }

  if (strcmp(wanted, trackGetCallsign()) != 0) {
    strncpy(wanted, trackGetCallsign(), sizeof(wanted) - 1);
    last_poll = 0;
    watch = {};
    dirty = true;
  }

  if (tapped) last_poll = 0;

  uint32_t now = millis();
  if (sta_mode && (last_poll == 0 || now - last_poll > 25000)) {
    last_poll = now;
    pollTraffic();
  }

  if (watch.live && now - last_tick > 1000) {
    last_tick = now;
    dirty = true;
  }

  if (dirty || now - last_draw > 2000) drawScreen();
  delay(40);
}

void trackLeave() {
  trackServerStop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  if (started_ap) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
  }
  started_ap = false;
  sta_mode = false;
}
