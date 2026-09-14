#include "apps.h"
#include "board.h"
#include "flight_server.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <math.h>
#include <string.h>
#include <ESPmDNS.h>

static const char *AP_SSID = "ESP32-Flights";
static const char *MDNS_NAME = "esp32-flights";

static bool sta_mode = false;
static bool started_ap = false;
static bool dirty = true;
static uint32_t last_poll = 0;
static uint32_t last_draw = 0;
static uint32_t last_tick = 0;
static char status_line[40] = "starting";
static void drawScreen();

enum FlightDir : uint8_t { DIR_NONE = 0, DIR_IN = 1, DIR_OUT = 2 };

struct Tracked {
  bool ok;
  char callsign[16];
  char country[18];
  char other_iata[8];
  char other_city[16];
  FlightDir dir;
  float dist_km;
  float t_cpa_s;
  float d_cpa_km;
  float pass_s;
  float t_apt_s;
  float alt_ft;
  float speed_kt;
  uint32_t stamp_ms;
};

static Tracked arriving = {};
static Tracked departing = {};

static const float DEG2RAD = 0.01745329252f;
static const float KM_PER_DEG = 111.32f;

static void setStatus(const char *s) {
  strncpy(status_line, s, sizeof(status_line) - 1);
  status_line[sizeof(status_line) - 1] = 0;
  dirty = true;
  drawScreen();
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

static void trimCallsign(char *dst, size_t n, const char *src) {
  while (*src == ' ') src++;
  size_t i = 0;
  while (src[i] && src[i] != ' ' && i + 1 < n) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    dst[i++] = c;
  }
  dst[i] = 0;
}

static bool looksCommercial(const char *cs) {
  if (!cs || !cs[0]) return false;
  if (cs[0] == 'N' && cs[1] >= '0' && cs[1] <= '9') return false;
  int letters = 0, i = 0;
  while (cs[i] >= 'A' && cs[i] <= 'Z') {
    letters++;
    i++;
  }
  if (letters != 3) return false;
  if (cs[i] < '0' || cs[i] > '9') return false;
  static const char *biz[] = {"EJA", "EJM", "LXJ", "XOJ", "FTH", "JTL", "TWY", "GPD", "USC", "NJE"};
  for (const char *p : biz) {
    if (!strncmp(cs, p, 3)) return false;
  }
  return true;
}

static void clipCopy(char *dst, size_t n, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

static float wrap180(float d) {
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

static bool sameAirport(const char *a, const char *b) {
  if (!a || !b || !a[0] || !b[0]) return false;
  char na[12], nb[12];
  trimCallsign(na, sizeof(na), a);
  trimCallsign(nb, sizeof(nb), b);
  if (!strcmp(na, nb)) return true;
  if (strlen(na) == 4 && na[0] == 'K' && !strcmp(na + 1, nb)) return true;
  if (strlen(nb) == 4 && nb[0] == 'K' && !strcmp(nb + 1, na)) return true;
  return false;
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
  Serial.printf("Sky GET %d %s\n", code, url);
  if (code != HTTP_CODE_OK) {
    http.end();
    return code;
  }
  String body = http.getString();
  http.end();
  Serial.printf("Sky body %u bytes\n", (unsigned)body.length());
  if (body.length() < 2) return -1;
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    Serial.printf("Sky json %s\n", jerr.c_str());
    return -2;
  }
  return HTTP_CODE_OK;
}

static bool fetchJson(const char *url, JsonDocument &doc) {
  int code = httpGetJson(url, doc);
  if (code == HTTP_CODE_OK) return true;
  if (code == 0) setStatus("http begin fail");
  else if (code == -1) setStatus("empty body");
  else if (code == -2) setStatus("json error");
  else {
    char err[32];
    snprintf(err, sizeof(err), "API %d", code);
    setStatus(err);
  }
  return false;
}

struct RouteInfo {
  bool found;
  char orig[8];
  char dest[8];
  char orig_city[16];
  char dest_city[16];
  float olat, olon, dlat, dlon;
};

struct RouteCache {
  char cs[12];
  RouteInfo info;
  uint32_t ts;
};

static RouteCache route_cache[16] = {};

static bool cacheGet(const char *cs, RouteInfo *out) {
  uint32_t now = millis();
  for (RouteCache &e : route_cache) {
    if (e.cs[0] && !strcmp(e.cs, cs) && now - e.ts < 600000UL) {
      *out = e.info;
      return true;
    }
  }
  return false;
}

static void cachePut(const char *cs, const RouteInfo &info) {
  RouteCache *slot = &route_cache[0];
  for (RouteCache &e : route_cache) {
    if (!e.cs[0]) {
      slot = &e;
      break;
    }
    if (e.ts < slot->ts) slot = &e;
  }
  strncpy(slot->cs, cs, sizeof(slot->cs) - 1);
  slot->cs[sizeof(slot->cs) - 1] = 0;
  slot->info = info;
  slot->ts = millis();
}

static void fillAirport(JsonVariant v, char *iata, size_t iataN, char *city, size_t cityN, float *lat, float *lon) {
  const char *code = v["iata_code"] | "";
  if (!code[0]) code = v["iata"] | "";
  if (!code[0]) code = v["icao_code"] | "";
  if (!code[0]) code = v["icao"] | "";
  clipCopy(iata, iataN, code);
  const char *muni = v["municipality"] | "";
  if (!muni[0]) muni = v["location"] | "";
  if (!muni[0]) muni = v["name"] | "";
  clipCopy(city, cityN, muni);
  if (!v["latitude"].isNull()) *lat = v["latitude"].as<float>();
  else if (!v["lat"].isNull()) *lat = v["lat"].as<float>();
  else *lat = 0;
  if (!v["longitude"].isNull()) *lon = v["longitude"].as<float>();
  else if (!v["lon"].isNull()) *lon = v["lon"].as<float>();
  else *lon = 0;
}

static RouteInfo lookupRoute(const char *cs) {
  RouteInfo info = {};
  if (!cs || !cs[0]) return info;
  if (cacheGet(cs, &info)) return info;

  char url[96];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);
  JsonDocument doc;
  int code = httpGetJson(url, doc);
  if (code == HTTP_CODE_OK) {
    JsonVariant fr = doc["response"]["flightroute"];
    if (!fr.isNull()) {
      fillAirport(fr["origin"], info.orig, sizeof(info.orig), info.orig_city, sizeof(info.orig_city), &info.olat, &info.olon);
      fillAirport(fr["destination"], info.dest, sizeof(info.dest), info.dest_city, sizeof(info.dest_city), &info.dlat, &info.dlon);
      info.found = info.orig[0] || info.dest[0];
    }
  }
  cachePut(cs, info);
  delay(10);
  return info;
}

static bool airportHit(const char *code, float plat, float plon, const char *aname, float alat, float alon) {
  if (sameAirport(code, aname)) return true;
  if (plat != 0 || plon != 0) {
    if (distKm(plat, plon, alat, alon) < 12) return true;
  }
  return false;
}

static bool pollTraffic() {
  if (!flightHasHome()) {
    setStatus("drop a pin on phone");
    arriving.ok = false;
    departing.ok = false;
    return false;
  }
  if (!flightHasAirport()) {
    setStatus("set an airport pin");
    arriving.ok = false;
    departing.ok = false;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("need home Wi-Fi");
    return false;
  }

  float lat, lon;
  flightGetHome(&lat, &lon);
  float alat = 0, alon = 0;
  char aname[12] = "";
  flightGetAirport(&alat, &alon, aname, sizeof(aname));

  const int radius_nm = 40;
  char url[180];
  JsonDocument doc;
  const char *src = "live adsb.fi";
  snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d", lat, lon, radius_nm);
  setStatus("polling adsb.fi");
  if (!fetchJson(url, doc)) {
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/lat/%.4f/lon/%.4f/dist/%d", lat, lon, radius_nm);
    doc.clear();
    src = "live adsb.lol";
    setStatus("polling adsb.lol");
    if (!fetchJson(url, doc)) {
      snprintf(url, sizeof(url), "https://api.airplanes.live/v2/point/%.4f/%.4f/%d", lat, lon, radius_nm);
      doc.clear();
      src = "live airplanes.live";
      setStatus("polling airplanes.live");
      if (!fetchJson(url, doc)) return false;
    }
  }

  struct Cand {
    char cs[12];
    char typ[8];
    char reg[10];
    float dist, t_cpa, d_cpa, pass_s, t_apt, alt, gs, d_apt, vs;
    int8_t geom;
    int prio;
  };
  Cand cands[24];
  int nc = 0;

  JsonArray acs = doc["ac"].as<JsonArray>();
  if (acs.isNull()) acs = doc["aircraft"].as<JsonArray>();
  const int nearby = acs.isNull() ? 0 : (int)acs.size();
  float rpass = flightPassRadiusKm();

  for (JsonVariant row : acs) {
    if (nc >= 24) break;
    if (row["lat"].isNull() || row["lon"].isNull()) continue;
    if (row["alt_baro"].is<const char *>()) continue;
    const char *cat = row["category"] | "";
    if (cat[0] == 'A' && cat[1] == '7') continue;
    char csbuf[12];
    trimCallsign(csbuf, sizeof(csbuf), row["flight"] | "");
    if (!looksCommercial(csbuf)) continue;
    float gs_kt = row["gs"].isNull() ? 0 : row["gs"].as<float>();
    if (gs_kt < 40) continue;
    float clat = row["lat"].as<float>();
    float clon = row["lon"].as<float>();
    float track = row["track"].isNull() ? row["true_heading"] | 0 : row["track"].as<float>();
    float alt_ft = row["alt_baro"].as<float>();
    float vel = gs_kt * 0.514444f;
    float vs = row["baro_rate"].isNull() ? 0 : row["baro_rate"].as<float>();

    float east, north;
    enu(clat, clon, lat, lon, &east, &north);
    float dist = sqrtf(east * east + north * north);
    float ve = vel * sinf(track * DEG2RAD) * 0.001f;
    float vn = vel * cosf(track * DEG2RAD) * 0.001f;
    float v2 = ve * ve + vn * vn;
    float t_cpa = 0;
    float d_cpa = dist;
    if (v2 > 1e-8f) {
      t_cpa = -(east * ve + north * vn) / v2;
      float ce = east + ve * t_cpa;
      float cn = north + vn * t_cpa;
      d_cpa = sqrtf(ce * ce + cn * cn);
    }
    float pass_s = 0;
    if (v2 > 1e-8f) {
      float a = v2;
      float b = 2 * (east * ve + north * vn);
      float c = east * east + north * north - rpass * rpass;
      float disc = b * b - 4 * a * c;
      if (disc > 0) {
        float sdisc = sqrtf(disc);
        float t1 = (-b - sdisc) / (2 * a);
        float t2 = (-b + sdisc) / (2 * a);
        if (t2 > 0) pass_s = t2 - fmaxf(0, t1);
      }
    }
    float ae, an;
    enu(alat, alon, clat, clon, &ae, &an);
    float d_apt = sqrtf(ae * ae + an * an);
    float bearing = atan2f(ae, an) / DEG2RAD;
    float in_err = fabsf(wrap180(track - bearing));
    float out_err = fabsf(wrap180(track - (bearing + 180)));
    int8_t geom = 0;
    if (d_apt < 55 && in_err < 35 && vs < -280 && alt_ft < 14000) geom = 1;
    else if (d_apt < 50 && out_err < 35 && vs > 280 && alt_ft < 16000) geom = -1;

    float toward = ae * ve + an * vn;
    float t_apt = NAN;
    if (vel > 1 && toward > 0) t_apt = (d_apt * 1000.0f) / vel;

    Cand &c = cands[nc];
    memset(&c, 0, sizeof(c));
    clipCopy(c.cs, sizeof(c.cs), csbuf);
    clipCopy(c.typ, sizeof(c.typ), row["t"] | "");
    clipCopy(c.reg, sizeof(c.reg), row["r"] | "");
    c.dist = dist;
    c.t_cpa = t_cpa;
    c.d_cpa = d_cpa;
    c.pass_s = pass_s;
    c.t_apt = t_apt;
    c.alt = alt_ft;
    c.gs = gs_kt;
    c.d_apt = d_apt;
    c.vs = vs;
    c.geom = geom;
    if (geom != 0) c.prio = (int)d_apt;
    else if (d_apt < 40) c.prio = 400 + (int)d_apt;
    else c.prio = 2000 + (int)dist;
    nc++;
  }
  doc.clear();

  for (int i = 0; i < nc; i++) {
    for (int j = i + 1; j < nc; j++) {
      if (cands[j].prio < cands[i].prio) {
        Cand tmp = cands[i];
        cands[i] = cands[j];
        cands[j] = tmp;
      }
    }
  }

  setStatus("checking routes");
  int lookups_left = 10;
  Tracked best_in = {};
  Tracked best_out = {};
  float best_in_score = 1e9f;
  float best_out_score = 1e9f;
  int related = 0;

  for (int i = 0; i < nc; i++) {
    Cand &c = cands[i];
    if (!c.cs[0]) continue;
    RouteInfo route = {};
    bool have = cacheGet(c.cs, &route);
    if (!have && lookups_left > 0 && (c.geom != 0 || c.d_apt < 40)) {
      route = lookupRoute(c.cs);
      lookups_left--;
      have = true;
    }

    bool to_apt = have && route.found && airportHit(route.dest, route.dlat, route.dlon, aname, alat, alon);
    bool from_apt = have && route.found && airportHit(route.orig, route.olat, route.olon, aname, alat, alon);
    FlightDir dir = DIR_NONE;
    const char *oiata = "";
    const char *ocity = "";
    if (to_apt && from_apt) {
      dir = (c.vs > 120) ? DIR_OUT : DIR_IN;
      oiata = dir == DIR_IN ? route.orig : route.dest;
      ocity = dir == DIR_IN ? route.orig_city : route.dest_city;
    } else if (to_apt) {
      dir = DIR_IN;
      oiata = route.orig;
      ocity = route.orig_city;
    } else if (from_apt) {
      dir = DIR_OUT;
      oiata = route.dest;
      ocity = route.dest_city;
    } else if (route.found) {
      continue;
    } else if (c.geom != 0) {
      dir = c.geom > 0 ? DIR_IN : DIR_OUT;
    } else {
      continue;
    }
    related++;

    float score = c.d_apt * 3.0f + c.dist * 0.4f;
    if (dir == DIR_IN) {
      if (c.vs > -80) score += 250;
      if (c.alt > 14000) score += 200;
    } else {
      if (c.vs < 80) score += 250;
      if (c.alt > 16000) score += 200;
    }
    if (c.t_cpa >= -15 && c.t_cpa < 1200 && c.d_cpa < 18) score -= 80;

    Tracked *slot = dir == DIR_IN ? &best_in : &best_out;
    float *best_score = dir == DIR_IN ? &best_in_score : &best_out_score;
    if (score >= *best_score) continue;
    *best_score = score;
    slot->ok = true;
    strncpy(slot->callsign, c.cs, sizeof(slot->callsign) - 1);
    slot->callsign[sizeof(slot->callsign) - 1] = 0;
    if (c.typ[0] && c.reg[0]) snprintf(slot->country, sizeof(slot->country), "%s  %s", c.typ, c.reg);
    else strncpy(slot->country, c.typ[0] ? c.typ : c.reg, sizeof(slot->country) - 1);
    clipCopy(slot->other_iata, sizeof(slot->other_iata), oiata);
    clipCopy(slot->other_city, sizeof(slot->other_city), ocity);
    slot->dir = dir;
    slot->dist_km = c.dist;
    slot->t_cpa_s = c.t_cpa;
    slot->d_cpa_km = c.d_cpa;
    slot->pass_s = c.pass_s;
    slot->t_apt_s = dir == DIR_IN ? c.t_apt : NAN;
    slot->alt_ft = c.alt;
    slot->speed_kt = c.gs;
    slot->stamp_ms = millis();
  }

  arriving = best_in;
  departing = best_out;
  if (arriving.ok || departing.ok) setStatus(src);
  else {
    char msg[40];
    snprintf(msg, sizeof(msg), "%d nearby, 0 at %s", nearby, aname[0] ? aname : "apt");
    setStatus(msg);
  }
  Serial.printf("Sky nearby=%d related=%d in=%s out=%s\n", nearby, related,
                arriving.callsign, departing.callsign);
  dirty = true;
  return true;
}

static float liveCpa(const Tracked &t) {
  if (!t.ok) return NAN;
  return t.t_cpa_s - (millis() - t.stamp_ms) / 1000.0f;
}

static float liveApt(const Tracked &t) {
  if (!t.ok || !isfinite(t.t_apt_s)) return NAN;
  return t.t_apt_s - (millis() - t.stamp_ms) / 1000.0f;
}

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void drawRunway(int cx, int y, int w, uint16_t c) {
  gfx->fillRoundRect(cx - w / 2, y, w, 3, 1, c);
  gfx->fillRect(cx - 1, y - 2, 3, 2, c);
}

static void drawArriveIcon(int cx, int cy, int s, uint16_t c) {
  gfx->fillTriangle(cx + s / 2, cy + s / 5, cx - s / 3, cy - s / 5, cx - s / 5, cy + s / 3, c);
  gfx->fillTriangle(cx - s / 12, cy, cx + s / 10, cy - s / 3, cx + s / 3, cy + s / 10, c);
  drawRunway(cx, cy + s / 2, s + 4, c);
}

static void drawDepartIcon(int cx, int cy, int s, uint16_t c) {
  gfx->fillTriangle(cx + s / 2, cy - s / 5, cx - s / 3, cy + s / 5, cx - s / 5, cy - s / 3, c);
  gfx->fillTriangle(cx - s / 12, cy, cx + s / 10, cy + s / 3, cx + s / 3, cy - s / 10, c);
  drawRunway(cx, cy + s / 2, s + 4, c);
}

static void drawFilterChip(int x, int y, int w, bool on, uint8_t mode, const char *label) {
  uint16_t fill = on ? gfx->color565(18, 90, 110) : gfx->color565(14, 24, 40);
  uint16_t edge = on ? gfx->color565(80, 220, 255) : gfx->color565(40, 58, 90);
  uint16_t ink = on ? gfx->color565(220, 248, 255) : gfx->color565(140, 160, 190);
  gfx->fillRoundRect(x, y, w, 28, 10, fill);
  gfx->drawRoundRect(x, y, w, 28, 10, edge);
  int icx = x + 14;
  int icy = y + 13;
  if (mode == FLIGHT_FILTER_IN) drawArriveIcon(icx, icy, 10, ink);
  else if (mode == FLIGHT_FILTER_OUT) drawDepartIcon(icx, icy, 10, ink);
  else {
    drawArriveIcon(icx - 1, icy - 2, 7, ink);
    drawDepartIcon(icx + 4, icy + 3, 7, ink);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(x + 26, y + 10);
  gfx->print(label);
}

static void drawFilterBar() {
  uint8_t f = flightGetFilter();
  drawFilterChip(8, 40, 72, f == FLIGHT_FILTER_IN, FLIGHT_FILTER_IN, "In");
  drawFilterChip(84, 40, 72, f == FLIGHT_FILTER_OUT, FLIGHT_FILTER_OUT, "Out");
  drawFilterChip(160, 40, 72, f == FLIGHT_FILTER_BOTH, FLIGHT_FILTER_BOTH, "Both");
}

static void drawEmptyCard(int y, int h, bool inbound, const char *aname) {
  uint16_t accent = inbound ? gfx->color565(80, 220, 255) : gfx->color565(255, 176, 80);
  gfx->fillRoundRect(12, y, 216, h, 14, gfx->color565(12, 28, 46));
  if (inbound) drawArriveIcon(40, y + h / 2, 18, accent);
  else drawDepartIcon(40, y + h / 2, 18, accent);
  gfx->setTextSize(1);
  gfx->setTextColor(accent);
  gfx->setCursor(68, y + h / 2 - 8);
  gfx->print(inbound ? "No arrivals" : "No departures");
  gfx->setTextColor(gfx->color565(140, 160, 190));
  gfx->setCursor(68, y + h / 2 + 8);
  gfx->print(aname);
}

static void drawFlightCard(const Tracked &t, int y, int h, const char *aname, bool compact) {
  bool inbound = t.dir == DIR_IN;
  uint16_t accent = inbound ? gfx->color565(80, 220, 255) : gfx->color565(255, 176, 80);
  uint16_t card = inbound ? gfx->color565(10, 36, 52) : gfx->color565(42, 28, 14);
  gfx->fillRoundRect(12, y, 216, h, 14, card);
  if (inbound) drawArriveIcon(36, y + 28, compact ? 16 : 20, accent);
  else drawDepartIcon(36, y + 28, compact ? 16 : 20, accent);

  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(62, y + 8);
  gfx->print(t.callsign);

  gfx->setTextSize(1);
  gfx->setTextColor(accent);
  gfx->setCursor(62, y + 28);
  gfx->print(inbound ? "ARRIVING" : "DEPARTING");
  gfx->printf(" %s", aname);

  gfx->setTextColor(0xFFFF);
  gfx->setCursor(62, y + 42);
  gfx->print(inbound ? "from " : "to ");
  if (t.other_city[0]) gfx->print(t.other_city);
  else if (t.other_iata[0]) gfx->print(t.other_iata);
  else gfx->print(aname);
  if (t.other_iata[0] && t.other_city[0]) {
    gfx->print(" ");
    gfx->print(t.other_iata);
  }

  char tbuf[16];
  float tcpa = liveCpa(t);
  gfx->setTextColor(gfx->color565(160, 180, 210));
  gfx->setCursor(22, y + 60);
  gfx->print("pass ");
  gfx->setTextColor(0xFFFF);
  if (tcpa < 0 && tcpa > -25) gfx->print("NOW");
  else if (tcpa < -25) gfx->print("passed");
  else {
    fmtTime(tbuf, sizeof(tbuf), tcpa);
    gfx->print(tbuf);
  }
  gfx->setTextColor(gfx->color565(160, 180, 210));
  gfx->printf("  %.0fkm  %ldft", t.dist_km, (long)lroundf(t.alt_ft));

  if (!compact) {
    gfx->setCursor(22, y + 80);
    gfx->setTextColor(accent);
    gfx->print(inbound ? "LANDING  " : "EN ROUTE  ");
    gfx->setTextColor(0xFFFF);
    if (inbound) {
      float ta = liveApt(t);
      if (!isfinite(ta)) gfx->print("turning to airport");
      else {
        fmtTime(tbuf, sizeof(tbuf), ta);
        gfx->print(tbuf);
      }
    } else if (t.other_city[0]) gfx->print(t.other_city);
    else gfx->print(aname);
    gfx->setTextColor(gfx->color565(140, 160, 190));
    gfx->setCursor(22, y + 96);
    gfx->print(t.country);
    gfx->printf("  %ld kt", (long)lroundf(t.speed_kt));
  }
}

static void drawScreen() {
  gfx->fillScreen(gfx->color565(5, 12, 22));
  gfx->fillRect(0, 0, LCD_W, 36, gfx->color565(8, 24, 44));
  gfx->setTextSize(2);
  gfx->setTextColor(gfx->color565(80, 220, 255));
  gfx->setCursor(12, 10);
  gfx->print("SKY");

  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(140, 170, 200));
  gfx->setCursor(70, 8);
  if (WiFi.status() == WL_CONNECTED) gfx->print(WiFi.localIP());
  else if (started_ap) gfx->print("192.168.4.1");
  else gfx->print("no wifi");

  gfx->setCursor(70, 20);
  gfx->setTextColor(gfx->color565(90, 210, 160));
  char aname[12] = "";
  if (flightHasAirport()) {
    float alat, alon;
    flightGetAirport(&alat, &alon, aname, sizeof(aname));
    gfx->print(aname[0] ? aname : "airport");
    gfx->print("  ");
  }
  gfx->setTextColor(gfx->color565(120, 150, 180));
  const char *label = flightHomeLabel();
  if (label && label[0]) gfx->print(label);
  if (!aname[0]) strncpy(aname, "BOS", sizeof(aname) - 1);

  drawFilterBar();

  uint8_t f = flightGetFilter();
  const Tracked *show = nullptr;
  if (f == FLIGHT_FILTER_IN) show = arriving.ok ? &arriving : nullptr;
  else if (f == FLIGHT_FILTER_OUT) show = departing.ok ? &departing : nullptr;

  if (f == FLIGHT_FILTER_BOTH) {
    if (arriving.ok) drawFlightCard(arriving, 74, 108, aname, true);
    else drawEmptyCard(74, 108, true, aname);
    if (departing.ok) drawFlightCard(departing, 188, 108, aname, true);
    else drawEmptyCard(188, 108, false, aname);
  } else if (show) {
    drawFlightCard(*show, 74, 220, aname, false);
  } else {
    drawEmptyCard(112, 120, f != FLIGHT_FILTER_OUT, aname);
    gfx->setTextColor(gfx->color565(140, 160, 190));
    gfx->setCursor(24, 250);
    gfx->print("Tap In / Out / Both.");
  }

  gfx->setTextSize(1);
  gfx->setTextColor(gfx->color565(110, 130, 160));
  gfx->setCursor(12, 304);
  gfx->print(status_line);
  gfx->setCursor(150, 304);
  gfx->print("BOOT=Home");
  last_draw = millis();
  dirty = false;
}

void flightsEnter() {
  arriving = {};
  departing = {};
  dirty = true;
  last_poll = 0;
  sta_mode = false;
  started_ap = false;
  flightLoadPrefs();
  gfx->fillScreen(gfx->color565(7, 16, 28));
  gfx->setTextColor(0xFFFF);
  gfx->setTextSize(2);
  gfx->setCursor(16, 80);
  gfx->print("Sky");
  gfx->setTextSize(1);
  gfx->setCursor(16, 110);
  gfx->print("joining Wi-Fi...");

  if (WiFi.status() == WL_CONNECTED) {
    sta_mode = true;
    MDNS.begin(MDNS_NAME);
  } else if (!connectSavedWifi(18000)) {
    startAp();
  }
  flightServerStart();
  setStatus(sta_mode ? "wifi ok" : "AP ESP32-Flights");
  drawScreen();
}

void flightsLoop(bool tapped, uint16_t tx, uint16_t ty) {
  char ssid[33] = {0};
  char pass[65] = {0};
  if (flightTakePendingWifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
    gfx->fillScreen(gfx->color565(7, 16, 28));
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
      flightServerStop();
      delay(40);
      flightServerStart();
      setStatus("wifi ok");
    } else {
      startAp();
      setStatus("join failed");
    }
    dirty = true;
  }

  if (tapped) {
    uint8_t cur = flightGetFilter();
    uint8_t next = cur;
    if (inRect(tx, ty, 8, 36, 76, 36)) next = FLIGHT_FILTER_IN;
    else if (inRect(tx, ty, 84, 36, 76, 36)) next = FLIGHT_FILTER_OUT;
    else if (inRect(tx, ty, 160, 36, 76, 36)) next = FLIGHT_FILTER_BOTH;
    if (next != cur) {
      flightSetFilter(next);
    }
    last_poll = 0;
    dirty = true;
  }

  uint32_t now = millis();
  if (sta_mode && (last_poll == 0 || now - last_poll > 25000)) {
    last_poll = now;
    pollTraffic();
  }

  if ((arriving.ok || departing.ok) && now - last_tick > 1000) {
    last_tick = now;
    dirty = true;
  }

  if (dirty || now - last_draw > 2000) drawScreen();
  delay(40);
}

void flightsLeave() {
  flightServerStop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  if (started_ap) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
  }
  started_ap = false;
  sta_mode = false;
}
