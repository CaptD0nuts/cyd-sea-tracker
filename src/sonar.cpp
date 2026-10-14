// Sonar scope (left, 8-bit sprite redrawn every frame) and info panel (right,
// drawn straight to the screen only when it changes).
#include "sonar.h"
#include <math.h>

#define SCOPE_SIZE 220
#define SCOPE_X 6
#define SCOPE_Y 10
#define SCOPE_R 106          // px radius of the outer range ring
#define PANEL_X 232
#define PANEL_W (320 - PANEL_X)
#define SWEEP_MS 5000        // one full turn of the sweep line
#define SWEEP_TRAIL 16       // fading lines behind the sweep
#define BTN 30               // zoom buttons: squares in the scope's bottom corners

#define C_GRID   0x0291      // dim teal
#define C_GRID2  0x0148      // dimmer teal
#define C_SWEEP  0x07FF      // cyan
#define C_SEL    TFT_WHITE
#define C_GOLD   0xFEA0
#define C_DIM    0x8410      // gray
#define C_SEA    0x351F

static TFT_eSPI *s_tft;
static TFT_eSprite *s_spr;
static uint32_t s_rangeShownMs = 0;  // when the range last changed, for the big readout
static uint32_t s_panelHash = 0;

static uint16_t kindColor(Kind k) {
  switch (k) {
    case K_SHARK:   return 0xFA49;  // coral red
    case K_TURTLE:  return 0x47E8;  // green
    case K_WHALE:   return 0x5DDF;  // light blue
    case K_DOLPHIN: return 0x5DDF;
    case K_SEAL:    return 0xC618;  // silver
    case K_FISH:    return 0xFD20;  // amber
    case K_GATOR:   return 0x8C60;  // olive
    default:        return TFT_WHITE;
  }
}

bool sonarFresh(const Animal &a, const View &v) {
  return v.freshDays == 0 || (v.nowEpoch && v.nowEpoch - a.fixEpoch <= (uint32_t)v.freshDays * 86400UL);
}

void sonarBegin(TFT_eSPI &tft) {
  s_tft = &tft;
  if (!s_spr) {
    s_spr = new TFT_eSprite(&tft);
    s_spr->setColorDepth(8);
    s_spr->createSprite(SCOPE_SIZE, SCOPE_SIZE);
  }
  tft.fillScreen(TFT_BLACK);
  tft.drawFastVLine(PANEL_X - 4, 0, 240, C_GRID2);
  s_panelHash = 0;
}

void sonarRangeChanged() { s_rangeShownMs = millis(); }

int sonarButtonHit(int x, int y) {
  // Hit boxes a bit larger than the drawn buttons; touch is roughly calibrated.
  if (y < SCOPE_Y + SCOPE_SIZE - BTN - 10) return 0;
  if (x < SCOPE_X + BTN + 10) return -1;
  if (x > SCOPE_X + SCOPE_SIZE - BTN - 10 && x < PANEL_X - 4) return 1;
  return 0;
}

bool sonarPanelHit(int x, int y) { return x >= PANEL_X - 4; }

// Animal position in scope pixels relative to the center.
static void animalPx(const Animal &a, int rangeNm, float &px, float &py) {
  float scale = (float)SCOPE_R / rangeNm;
  float t = a.brgDeg * (float)M_PI / 180.0f;
  px = a.distNm * sinf(t) * scale;
  py = -a.distNm * cosf(t) * scale;  // north is up
}

// Shark = dorsal fin, turtle = shell, everything else = diamond.
static void drawAnimal(TFT_eSprite &s, int x, int y, Kind k, uint16_t color) {
  if (k == K_SHARK) {
    s.fillTriangle(x - 5, y + 3, x + 4, y + 3, x + 2, y - 6, color);
  } else if (k == K_TURTLE) {
    s.fillCircle(x, y, 4, color);
    s.fillCircle(x, y - 6, 2, color);
  } else {
    s.fillTriangle(x, y - 5, x - 4, y, x + 4, y, color);
    s.fillTriangle(x, y + 5, x - 4, y, x + 4, y, color);
  }
}

// Brightness of a blip: full right after the sweep passes, fading to 35%.
static uint16_t afterglow(uint16_t color, float blipAng, float sweepAng) {
  float since = sweepAng - blipAng;
  while (since < 0) since += 2 * (float)M_PI;
  float k = 1.0f - 0.65f * since / (2 * (float)M_PI);
  uint8_t r = ((color >> 11) & 0x1F) * 255 / 31, g = ((color >> 5) & 0x3F) * 255 / 63, b = (color & 0x1F) * 255 / 31;
  return s_spr->color565(r * k, g * k, b * k);
}

void sonarDrawScope(const SeaSnapshot &snap, int selected, const View &v, uint32_t now) {
  TFT_eSprite &s = *s_spr;
  const int c = SCOPE_SIZE / 2;
  s.fillSprite(TFT_BLACK);

  // Range rings, crosshair, north mark.
  for (int i = 1; i <= 3; ++i) s.drawCircle(c, c, SCOPE_R * i / 3, i == 3 ? C_GRID : C_GRID2);
  s.drawFastHLine(c - SCOPE_R, c, SCOPE_R * 2, C_GRID2);
  s.drawFastVLine(c, c - SCOPE_R, SCOPE_R * 2, C_GRID2);
  s.setTextColor(C_GRID, TFT_BLACK);
  s.setTextDatum(TC_DATUM);
  s.drawString("N", c, 0, 1);
  s.setTextDatum(TL_DATUM);
  char rng[10];
  snprintf(rng, sizeof rng, "%dNM", v.rangeNm);
  s.drawString(rng, c + 3, c - SCOPE_R + 2, 1);

  // Sweep line with a fading trail behind it.
  float a0 = (now % SWEEP_MS) * 2.0f * (float)M_PI / SWEEP_MS;
  for (int i = SWEEP_TRAIL; i >= 0; --i) {
    float a = a0 - i * 0.035f;
    uint8_t g = 255 - i * (230 / SWEEP_TRAIL);
    s.drawLine(c, c, c + sinf(a) * SCOPE_R, c - cosf(a) * SCOPE_R, s.color565(0, g, g));
  }

  // Animals in range, farthest first so the nearest sits on top.
  bool labels = true;
  int shown = 0;
  for (int i = 0; i < snap.count; ++i)
    if (sonarFresh(snap.animals[i], v) && snap.animals[i].distNm <= v.rangeNm) ++shown;
  labels = shown <= 6;
  for (int i = snap.count - 1; i >= 0; --i) {
    const Animal &a = snap.animals[i];
    if (!sonarFresh(a, v) || a.distNm > v.rangeNm) continue;
    float px, py;
    animalPx(a, v.rangeNm, px, py);
    int x = c + px, y = c + py;
    bool sel = i == selected;
    drawAnimal(s, x, y, a.kind, sel ? C_SEL : afterglow(kindColor(a.kind), a.brgDeg * (float)M_PI / 180.0f, a0));
    if (sel) s.drawRect(x - 9, y - 10, 19, 19, C_SEL);
    if (labels || sel) {
      s.setTextColor(sel ? C_SEL : kindColor(a.kind), TFT_BLACK);
      s.drawString(a.name, x + 8, y - 3, 1);
    }
  }

  // Selected animal beyond the outer ring: arrow on the rim pointing at it.
  if (selected >= 0 && snap.animals[selected].distNm > v.rangeNm) {
    float t = snap.animals[selected].brgDeg * (float)M_PI / 180.0f;
    float fx = sinf(t), fy = -cosf(t), rx = -fy, ry = fx;
    float tx = c + fx * (SCOPE_R + 2), ty = c + fy * (SCOPE_R + 2);
    s.fillTriangle(tx, ty, tx - fx * 10 + rx * 6, ty - fy * 10 + ry * 6, tx - fx * 10 - rx * 6, ty - fy * 10 - ry * 6, C_SEL);
  }

  // Zoom buttons.
  s.setTextDatum(MC_DATUM);
  s.setTextColor(C_SWEEP, TFT_BLACK);
  s.drawRect(0, SCOPE_SIZE - BTN, BTN, BTN, C_GRID);
  s.drawString("-", BTN / 2, SCOPE_SIZE - BTN / 2, 4);
  s.drawRect(SCOPE_SIZE - BTN, SCOPE_SIZE - BTN, BTN, BTN, C_GRID);
  s.drawString("+", SCOPE_SIZE - BTN / 2, SCOPE_SIZE - BTN / 2, 4);
  s.setTextDatum(TL_DATUM);

  s.fillCircle(c, c, 2, C_GOLD);  // you are here

  // Big range readout for a moment after a zoom, so the tap visibly did something.
  if (s_rangeShownMs && now - s_rangeShownMs < 1500) {
    char big[12];
    snprintf(big, sizeof big, "%d NM", v.rangeNm);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(TFT_WHITE, TFT_BLACK);
    s.drawString(big, c, c - 30, 4);
    s.setTextDatum(TL_DATUM);
  }
  s.pushSprite(SCOPE_X, SCOPE_Y);
}

static uint32_t hashStr(uint32_t h, const char *s) {
  while (*s) h = h * 31 + (uint8_t)*s++;
  return h * 31 + 0xFF;
}

static const char *compass(float deg) {
  static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return pts[(int)lroundf(deg / 45.0f) % 8];
}

// Split a common name over two ~11-char lines at a space.
static void wrap2(const char *src, char *a, char *b, size_t n) {
  a[0] = b[0] = 0;
  size_t len = strlen(src);
  if (len <= 11) {
    strlcpy(a, src, n);
    return;
  }
  int cut = -1;
  for (int i = 0; i < (int)len && i <= 11; ++i) if (src[i] == ' ') cut = i;
  if (cut < 0) cut = 11;
  strlcpy(a, src, min((size_t)cut + 1, n));
  strlcpy(b, src + cut + (src[cut] == ' '), n);
}

void sonarDrawPanel(const SeaSnapshot &snap, int selected, bool autoSelect, const View &v, uint32_t now) {
  char l[10][32] = {};
  uint32_t age = snap.everOk ? (now - snap.fetchedMs) / 1000 : 0;
  int shown = 0;
  for (int i = 0; i < snap.count; ++i)
    if (sonarFresh(snap.animals[i], v) && snap.animals[i].distNm <= v.rangeNm) ++shown;
  if (!snap.everOk) snprintf(l[9], 32, snap.lastError ? "NO SIGNAL %d" : "PINGING...", snap.lastError);
  else if (age > FEED_STALE_MS / 1000) snprintf(l[9], 32, "NO SIGNAL %luh", (unsigned long)(age / 3600));
  else snprintf(l[9], 32, "%d IN %dNM", shown, v.rangeNm);
  if (v.freshDays) snprintf(l[8], 32, "FIX < %dD", v.freshDays);
  else snprintf(l[8], 32, "FIX ANY AGE");

  if (selected >= 0 && selected < snap.count) {
    const Animal &a = snap.animals[selected];
    snprintf(l[0], 32, "%s", a.name);
    wrap2(a.species, l[1], l[2], 32);
    snprintf(l[3], 32, "%s %c %s", kindName(a.kind), a.gender, a.length);
    float mi = a.distNm * 1.15078f;
    snprintf(l[4], 32, "%.0fmi %s", mi, compass(a.brgDeg));
    if (v.nowEpoch && v.nowEpoch > a.fixEpoch) {
      uint32_t s = v.nowEpoch - a.fixEpoch;
      if (s < 2 * 3600) snprintf(l[5], 32, "%lum AGO", (unsigned long)(s / 60));
      else if (s < 2 * 86400) snprintf(l[5], 32, "%luh AGO", (unsigned long)(s / 3600));
      else if (s < 730 * 86400UL) snprintf(l[5], 32, "%lud AGO", (unsigned long)(s / 86400));
      else snprintf(l[5], 32, "%luy AGO", (unsigned long)(s / (365 * 86400UL)));
    }
    snprintf(l[6], 32, autoSelect ? "NEAREST" : "TAP=AUTO");
  } else if (snap.everOk) {
    snprintf(l[0], 32, "NO TAGS");
    snprintf(l[1], 32, "tap here for");
    snprintf(l[2], 32, "older fixes");
  }

  uint32_t h = 7;
  for (auto &s : l) h = hashStr(h, s);
  if (h == s_panelHash) return;
  s_panelHash = h;

  TFT_eSPI &t = *s_tft;
  t.fillRect(PANEL_X, 0, PANEL_W, 240, TFT_BLACK);
  t.setTextDatum(TL_DATUM);
  t.setTextColor(selected >= 0 ? kindColor(snap.animals[selected].kind) : C_GOLD, TFT_BLACK);
  // Font 4 is ~14px/char; drop to font 2 for long names so they fit 88px.
  t.drawString(l[0], PANEL_X, 10, strlen(l[0]) <= 6 ? 4 : 2);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString(l[1], PANEL_X, 40, 2);
  t.drawString(l[2], PANEL_X, 56, 2);
  t.setTextColor(C_DIM, TFT_BLACK);
  t.drawString(l[3], PANEL_X, 80, 1);
  t.setTextColor(C_SEA, TFT_BLACK);
  t.drawString(l[4], PANEL_X, 100, 2);
  if (l[5][0]) {
    t.setTextColor(C_DIM, TFT_BLACK);
    t.drawString("LAST FIX", PANEL_X, 120, 1);
    t.setTextColor(C_GOLD, TFT_BLACK);
    t.drawString(l[5], PANEL_X, 130, 2);
  }
  t.setTextColor(C_DIM, TFT_BLACK);
  t.drawString(l[6], PANEL_X, 160, 2);
  t.setTextColor(C_SWEEP, TFT_BLACK);
  t.drawString(l[8], PANEL_X, 190, 1);
  t.setTextColor(snap.everOk && age <= FEED_STALE_MS / 1000 ? C_SWEEP : TFT_RED, TFT_BLACK);
  t.drawString(l[9], PANEL_X, 216, 1);
}

int sonarHitTest(const SeaSnapshot &snap, const View &v, int x, int y) {
  const int c = SCOPE_SIZE / 2;
  int best = -1;
  float bestD2 = 22 * 22;  // tap must land within 22px of an animal
  for (int i = 0; i < snap.count; ++i) {
    const Animal &a = snap.animals[i];
    if (!sonarFresh(a, v) || a.distNm > v.rangeNm) continue;
    float px, py;
    animalPx(a, v.rangeNm, px, py);
    float dx = SCOPE_X + c + px - x, dy = SCOPE_Y + c + py - y;
    float d2 = dx * dx + dy * dy;
    if (d2 < bestD2) bestD2 = d2, best = i;
  }
  return best;
}

void sonarMessage(const char *line1, const char *line2, const char *line3) {
  TFT_eSPI &t = *s_tft;
  t.fillScreen(TFT_BLACK);
  t.setTextDatum(MC_DATUM);
  t.setTextColor(C_SWEEP, TFT_BLACK);
  t.drawString(line1, 160, 90, 4);
  t.setTextColor(TFT_WHITE, TFT_BLACK);
  t.drawString(line2, 160, 130, 2);
  t.setTextColor(C_DIM, TFT_BLACK);
  t.drawString(line3, 160, 155, 2);
  t.setTextDatum(TL_DATUM);
  s_panelHash = 0;
}
