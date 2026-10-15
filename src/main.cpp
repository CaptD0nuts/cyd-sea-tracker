// Sea life tracker on the Cheap Yellow Display: OCEARCH-tagged sharks,
// turtles and friends on a sweeping sonar scope, with details for the nearest
// (or tapped) animal.
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "config.h"
#include "sea.h"
#include "sonar.h"
#if __has_include("secrets.h")
#include "secrets.h"  // optional WiFi + location filled in on the laptop, not in git
#endif

// The touch controller sits on its own SPI bus (VSPI pins).
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

// Raw XPT2046 range mapped to screen pixels (verified on this board).
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

#define FRAME_MS 40

TFT_eSPI tft;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

static float g_lat = NAN, g_lon = NAN;
static bool g_locSaved = false;  // location came from the setup page; wins over secrets.h
static View g_view = {DEFAULT_RANGE_NM, DEFAULT_FRESH_DAYS, 0};

static SeaSnapshot g_snap;       // big; keep it off the loop task's stack
static char g_selName[20] = "";  // empty = auto (nearest)
static bool g_wasTouched = false;

static bool locationValid(float lat, float lon) {
  return !isnan(lat) && !isnan(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 &&
         !(lat == 0 && lon == 0);
}

static void loadSettings() {
  prefs.begin("sea", true);
  g_lat = prefs.getFloat("lat", NAN);
  g_lon = prefs.getFloat("lon", NAN);
  g_locSaved = locationValid(g_lat, g_lon);
  g_view.rangeNm = prefs.getInt("range", DEFAULT_RANGE_NM);
  g_view.freshDays = prefs.getInt("fresh", DEFAULT_FRESH_DAYS);
  prefs.end();
}

static void saveInt(const char *key, int v) {
  prefs.begin("sea", false);
  prefs.putInt(key, v);
  prefs.end();
}

static void onPortal(WiFiManager *) {
  sonarMessage("SETUP", "Join WiFi: " SETUP_AP_NAME, "pick your network + enter lat/lon");
}

// WiFi + location setup. force = open the setup hotspot even if saved WiFi
// works (no location yet, or the user held the screen).
static void connect(bool force) {
  char latBuf[16] = "", lonBuf[16] = "";
  if (!isnan(g_lat)) snprintf(latBuf, sizeof latBuf, "%.4f", g_lat);
  if (!isnan(g_lon)) snprintf(lonBuf, sizeof lonBuf, "%.4f", g_lon);
  WiFiManagerParameter pLat("lat", "Latitude (e.g. 40.7128)", latBuf, 15);
  WiFiManagerParameter pLon("lon", "Longitude (e.g. -74.0060, west is negative)", lonBuf, 15);

  WiFiManager wm;
  wm.addParameter(&pLat);
  wm.addParameter(&pLon);
  wm.setAPCallback(onPortal);
  wm.setConfigPortalTimeout(300);
  bool ok = force ? wm.startConfigPortal(SETUP_AP_NAME) : wm.autoConnect(SETUP_AP_NAME);

  // Google Maps copies "40.7128, -74.0060" as one string: accept both numbers
  // pasted into the latitude box.
  const char *latStr = pLat.getValue();
  const char *lonStr = pLon.getValue();
  char *end;
  float lat = strtof(latStr, &end);
  if (end == latStr) lat = NAN;
  while (*end == ' ') ++end;
  if (*end == ',') lonStr = end + 1;
  float lon = strtof(lonStr, &end);
  if (end == lonStr) lon = NAN;
  bool valid = locationValid(lat, lon);
  // Lengths only: never log the coordinates themselves.
  Serial.printf("portal: wifi %s, lat field %d chars, lon field %d chars, location %s\n",
                ok ? "connected" : "not connected", (int)strlen(pLat.getValue()), (int)strlen(pLon.getValue()),
                valid ? "valid" : "INVALID");
  if (!valid && !locationValid(g_lat, g_lon)) {
    sonarMessage("LOCATION NOT SAVED", isnan(lat) ? "latitude missing" : isnan(lon) ? "longitude missing" : "lat/lon out of range",
                 "e.g. 40.7128 and -74.0060");
    delay(6000);
  }
  if (valid && (lat != g_lat || lon != g_lon)) {
    prefs.begin("sea", false);
    prefs.putFloat("lat", lat);
    prefs.putFloat("lon", lon);
    prefs.end();
    Serial.println("location saved");
  }
  if (!ok || force) ESP.restart();  // timed out, or settings may have changed: start clean
}

// Selected animal: the tapped one while it still passes the filters, else the
// nearest fresh one (shown even when it's beyond the outer ring).
static int selectedIndex() {
  if (g_selName[0]) {
    for (int i = 0; i < g_snap.count; ++i) {
      const Animal &a = g_snap.animals[i];
      if (strcmp(a.name, g_selName) == 0 && sonarFresh(a, g_view) && a.distNm <= g_view.rangeNm) return i;
    }
    g_selName[0] = 0;
  }
  for (int i = 0; i < g_snap.count; ++i)
    if (sonarFresh(g_snap.animals[i], g_view)) return i;
  return -1;
}

static void stepSetting(int &value, const int *steps, int n, int dir, const char *key) {
  int i = 0;
  while (i < n - 1 && steps[i] != value) ++i;
  i = (i + dir + n) % n;
  value = steps[i];
  saveInt(key, value);
}

// Networks from secrets.h, each with the location to center on when joined.
struct KnownNet {
  const char *ssid, *pass;
  float lat, lon;
};
static const KnownNet KNOWN_NETS[] = {
#ifdef SECRET_WIFI_SSID
    {SECRET_WIFI_SSID, SECRET_WIFI_PASS, SECRET_LAT, SECRET_LON},
#endif
#ifdef SECRET2_WIFI_SSID
    {SECRET2_WIFI_SSID, SECRET2_WIFI_PASS, SECRET2_LAT, SECRET2_LON},
#endif
    {"", "", 0, 0},  // keeps the array non-empty without secrets.h
};

static bool tryJoin(const KnownNet &n) {
  WiFi.begin(n.ssid, n.pass);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  return false;
}

// Joins the first secrets.h network in range (in list order) and, unless a
// location was saved on the setup page, centers on that network's location.
// Only list positions are logged, never names or coordinates.
static bool joinKnownNetwork() {
  const int count = sizeof KNOWN_NETS / sizeof KNOWN_NETS[0];
  bool usable[count] = {};
  bool any = false;
  for (int i = 0; i < count; ++i) {
    usable[i] = strlen(KNOWN_NETS[i].ssid) && strcmp(KNOWN_NETS[i].ssid, "PUT-WIFI-NAME-HERE") != 0;
    any |= usable[i];
  }
  if (!any) return false;
  WiFi.mode(WIFI_STA);
  int found = WiFi.scanNetworks();
  bool seen[count] = {};
  for (int s = 0; s < found; ++s)
    for (int i = 0; i < count; ++i)
      if (usable[i] && WiFi.SSID(s) == KNOWN_NETS[i].ssid) seen[i] = true;
  WiFi.scanDelete();
  // Networks seen in the scan first; then the rest anyway (hidden SSIDs, missed scans).
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < count; ++i) {
      if (!usable[i] || seen[i] != (pass == 0)) continue;
      if (!tryJoin(KNOWN_NETS[i])) {
        Serial.printf("secrets.h network %d: failed\n", i + 1);
        continue;
      }
      Serial.printf("secrets.h network %d: connected\n", i + 1);
      if (!g_locSaved && locationValid(KNOWN_NETS[i].lat, KNOWN_NETS[i].lon)) {
        g_lat = KNOWN_NETS[i].lat;
        g_lon = KNOWN_NETS[i].lon;
      }
      return true;
    }
  }
  Serial.println("no secrets.h network joined, falling back to setup page");
  return false;
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);  // landscape, USB port on the right
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);
  sonarBegin(tft);
  sonarMessage("SEA TRACKER", "connecting to WiFi...");

  // Holding the screen while powering on reopens the setup page. Only checked
  // at boot, so handling the board while it runs can't trigger it.
  bool forceSetup = false;
  if (touch.touched()) {
    sonarMessage("SETUP", "keep holding to open setup...");
    uint32_t t0 = millis();
    while (touch.touched() && millis() - t0 < PORTAL_HOLD_MS) delay(50);
    forceSetup = touch.touched();
  }

  loadSettings();
  bool connected = !forceSetup && joinKnownNetwork();
  bool haveLocation = locationValid(g_lat, g_lon);
  if (forceSetup || !connected || !haveLocation) connect(forceSetup || !haveLocation);
  Serial.printf("wifi up, ip %s, range %d nm, fresh %d d\n", WiFi.localIP().toString().c_str(), g_view.rangeNm,
                g_view.freshDays);

  sonarBegin(tft);
  seaBegin(g_lat, g_lon);
}

void loop() {
  uint32_t start = millis();
  seaSnapshot(g_snap);
  g_view.nowEpoch = seaNow();

  bool touched = touch.touched();
  if (touched && !g_wasTouched) {
    TS_Point p = touch.getPoint();
    int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 319), 0, 319);
    int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 239), 0, 239);
    int zoom = sonarButtonHit(x, y);
    if (zoom) {
      // "-" zooms out (bigger range), "+" zooms in; no wrap-around.
      static const int steps[] = RANGE_STEPS;
      const int n = sizeof steps / sizeof steps[0];
      int i = 0;
      while (i < n - 1 && steps[i] < g_view.rangeNm) ++i;
      i = constrain(i - zoom, 0, n - 1);
      if (steps[i] != g_view.rangeNm) {
        g_view.rangeNm = steps[i];
        saveInt("range", g_view.rangeNm);
        sonarRangeChanged();
      }
    } else if (sonarPanelHit(x, y)) {
      static const int steps[] = FRESH_STEPS;
      stepSetting(g_view.freshDays, steps, sizeof steps / sizeof steps[0], 1, "fresh");
      g_selName[0] = 0;
    } else {
      int hit = sonarHitTest(g_snap, g_view, x, y);
      if (hit >= 0) strlcpy(g_selName, g_snap.animals[hit].name, sizeof g_selName);
      else g_selName[0] = 0;
    }
    Serial.printf("tap x=%d y=%d range %d fresh %d sel '%s'\n", x, y, g_view.rangeNm, g_view.freshDays, g_selName);
  }
  g_wasTouched = touched;

  int sel = selectedIndex();
  sonarDrawScope(g_snap, sel, g_view, start);
  sonarDrawPanel(g_snap, sel, g_selName[0] == 0, g_view, start);

  uint32_t spent = millis() - start;
  if (spent < FRAME_MS) delay(FRAME_MS - spent);
}
