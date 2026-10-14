#pragma once
#include <TFT_eSPI.h>
#include "sea.h"

// View settings the sonar needs on every frame.
struct View {
  int rangeNm;
  int freshDays;  // 0 = any age
  uint32_t nowEpoch;
};

// True if this animal passes the freshness setting.
bool sonarFresh(const Animal &a, const View &v);
void sonarBegin(TFT_eSPI &tft);
void sonarRangeChanged();
// -1 / +1 if (x, y) is on the zoom out / in button, else 0.
int sonarButtonHit(int x, int y);
// True if (x, y) is on the right panel (tap there cycles freshness).
bool sonarPanelHit(int x, int y);
// One animation frame of the scope. selected = index into snap.animals or -1.
void sonarDrawScope(const SeaSnapshot &snap, int selected, const View &v, uint32_t now);
// Right-hand panel; only repaints when its text changes.
void sonarDrawPanel(const SeaSnapshot &snap, int selected, bool autoSelect, const View &v, uint32_t now);
// Index of the fresh animal drawn nearest screen point (x, y), or -1 if none is close.
int sonarHitTest(const SeaSnapshot &snap, const View &v, int x, int y);
// Full-screen message (setup, errors).
void sonarMessage(const char *line1, const char *line2, const char *line3 = "");
