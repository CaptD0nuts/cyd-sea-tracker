// Background fetcher: OCEARCH's tagged animals from their public Mapotic
// feed, on its own core-0 task so the sonar animation never waits on it.
// The feed is ~450 KB, so it's parsed one animal at a time off the stream.
#include "sea.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

static float s_lat, s_lon;
static SemaphoreHandle_t s_lock;
static SeaSnapshot s_snap;  // guarded by s_lock

// Fetch scratch, task only: two nearest-first lists merged at the end.
static Animal s_recent[MAX_RECENT], s_old[MAX_OLD];
static int s_nRecent, s_nOld;

const char *kindName(Kind k) {
  static const char *names[] = {"SHARK", "TURTLE", "WHALE", "DOLPHIN", "SEAL", "FISH", "GATOR", "OTHER"};
  return names[k];
}

static Kind kindOf(const char *cat) {
  if (!cat) return K_OTHER;
  if (strstr(cat, "Shark")) return K_SHARK;
  if (strstr(cat, "Turtle")) return K_TURTLE;
  if (strstr(cat, "Whale")) return K_WHALE;
  if (strstr(cat, "Dolphin")) return K_DOLPHIN;
  if (strstr(cat, "Seal")) return K_SEAL;
  if (strstr(cat, "fish")) return K_FISH;  // Swordfish
  if (strstr(cat, "Alligator")) return K_GATOR;
  return K_OTHER;
}

uint32_t seaNow() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;  // before 2023 = clock not set yet
}

// "2026-10-14T03:12:21Z" (fraction or offset ignored, treated as UTC) to epoch.
static uint32_t parseIso(const char *s) {
  int y, mo, d, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  // Days from civil (Howard Hinnant).
  y -= mo <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468L;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

// Species "Tiger Shark (Galeocerdo cuvier)" -> "Tiger Shark".
static void commonName(char *dst, size_t n, const char *src) {
  strlcpy(dst, src ? src : "", n);
  char *p = strstr(dst, " (");
  if (p) *p = 0;
}

// Length "12 ft 5 in." -> "12ft5in".
static void compactLength(char *dst, size_t n, const char *src) {
  size_t j = 0;
  for (const char *p = src ? src : ""; *p && j + 1 < n; ++p)
    if (*p != ' ' && *p != '.') dst[j++] = *p;
  dst[j] = 0;
}

// Insert into a nearest-first list of capacity cap, dropping the farthest when full.
static void keepNearest(Animal *list, int &n, int cap, const Animal &a) {
  if (n == cap && a.distNm >= list[n - 1].distNm) return;
  int i = n < cap ? n++ : n - 1;
  while (i > 0 && list[i - 1].distNm > a.distNm) {
    list[i] = list[i - 1];
    --i;
  }
  list[i] = a;
}

static void distBearing(float lat2, float lon2, float &distNm, float &brgDeg) {
  const float r = (float)M_PI / 180.0f;
  float p1 = s_lat * r, p2 = lat2 * r, dl = (lon2 - s_lon) * r;
  float a = sinf((p2 - p1) / 2) * sinf((p2 - p1) / 2) + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
  distNm = 2 * 3440.065f * asinf(sqrtf(a));
  brgDeg = atan2f(sinf(dl) * cosf(p2), cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl)) / r;
  if (brgDeg < 0) brgDeg += 360;
}

// Stream reader for the parser that sleeps (instead of spinning) while it
// waits for network data, and so lets core 0's idle task feed the watchdog.
struct YieldingReader {
  Stream &st;
  uint32_t timeoutMs;
  int read() {
    uint32_t t0 = millis();
    for (;;) {
      int c = st.read();
      if (c >= 0) return c;
      if (millis() - t0 >= timeoutMs) return -1;
      vTaskDelay(1);
    }
  }
  size_t readBytes(char *buf, size_t n) {
    size_t i = 0;
    while (i < n) {
      int c = read();
      if (c < 0) break;
      buf[i++] = (char)c;
    }
    return i;
  }
};

static int fetchFeed(uint32_t now) {
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();  // public read-only data, so the certificate isn't checked
  http.useHTTP10(true);  // no chunked encoding, so the body can be read as a stream
  http.setTimeout(20000);
  http.setUserAgent(USER_AGENT);
  if (!http.begin(client, FEED_URL)) return -100;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return code == 0 ? -101 : code;
  }

  Stream &st = http.getStream();
  if (!st.find("\"features\"") || !st.find("[")) {
    http.end();
    return -102;
  }

  JsonDocument filter;
  filter["geometry"]["coordinates"] = true;
  JsonObject fp = filter["properties"].to<JsonObject>();
  for (const char *k : {"name", "species", "length", "gender", "last_move_datetime", "is_published"}) fp[k] = true;
  fp["category_name"]["en"] = true;

  YieldingReader rd{st, 20000};
  s_nRecent = s_nOld = 0;
  int seen = 0;
  const uint32_t recentCut = now - RECENT_DAYS * 86400UL;
  for (;;) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, rd, DeserializationOption::Filter(filter));
    if (err) {
      Serial.printf("feed json after %d animals: %s\n", seen, err.c_str());
      http.end();
      return -103;
    }
    ++seen;
    JsonObject p = doc["properties"];
    JsonArray c = doc["geometry"]["coordinates"];
    uint32_t fix = parseIso(p["last_move_datetime"]);
    if ((p["is_published"] | true) && c.size() >= 2 && fix) {
      Animal a = {};
      strlcpy(a.name, p["name"] | "?", sizeof a.name);
      commonName(a.species, sizeof a.species, p["species"]);
      compactLength(a.length, sizeof a.length, p["length"]);
      const char *g = p["gender"] | "";
      a.gender = g[0] == 'F' ? 'F' : g[0] == 'M' ? 'M' : '?';
      a.kind = kindOf(p["category_name"]["en"]);
      a.fixEpoch = fix;
      distBearing(c[1].as<float>(), c[0].as<float>(), a.distNm, a.brgDeg);
      if (fix >= recentCut) keepNearest(s_recent, s_nRecent, MAX_RECENT, a);
      else keepNearest(s_old, s_nOld, MAX_OLD, a);
    }
    // Next element: a comma means another animal, the closing bracket ends the list.
    char sep = 0;
    while (rd.readBytes(&sep, 1) == 1 && sep != ',' && sep != ']') {}
    if (sep != ',') break;
    vTaskDelay(1);  // let the idle task run even when data arrives faster than it parses
  }
  http.end();
  Serial.printf("feed: %d animals, %d recent + %d older kept\n", seen, s_nRecent, s_nOld);
  return seen > 0 ? 0 : -104;
}

static void task(void *) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t nextPoll = 0;
  for (;;) {
    uint32_t now = seaNow();
    if (WiFi.status() == WL_CONNECTED && now && (int32_t)(millis() - nextPoll) >= 0) {
      uint32_t t0 = millis();
      int rc = fetchFeed(now);
      Serial.printf("feed fetch rc=%d in %lums, heap %u\n", rc, (unsigned long)(millis() - t0), ESP.getFreeHeap());
      nextPoll = millis() + (rc ? FEED_RETRY_MS : FEED_POLL_MS);
      xSemaphoreTake(s_lock, portMAX_DELAY);
      if (rc == 0) {
        // Merge the two nearest-first lists.
        int i = 0, j = 0, n = 0;
        while (i < s_nRecent || j < s_nOld) {
          bool takeRecent = j >= s_nOld || (i < s_nRecent && s_recent[i].distNm <= s_old[j].distNm);
          s_snap.animals[n++] = takeRecent ? s_recent[i++] : s_old[j++];
        }
        s_snap.count = n;
        s_snap.fetchedMs = millis();
        s_snap.everOk = true;
      }
      s_snap.lastError = rc;
      xSemaphoreGive(s_lock);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void seaBegin(float lat, float lon) {
  s_lat = lat;
  s_lon = lon;
  s_lock = xSemaphoreCreateMutex();
  // TLS needs a deep stack.
  xTaskCreatePinnedToCore(task, "sea", 12288, nullptr, 1, nullptr, 0);
}

void seaSnapshot(SeaSnapshot &out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  out.count = s_snap.count;
  memcpy(out.animals, s_snap.animals, sizeof(Animal) * s_snap.count);
  out.fetchedMs = s_snap.fetchedMs;
  out.everOk = s_snap.everOk;
  out.lastError = s_snap.lastError;
  xSemaphoreGive(s_lock);
}
