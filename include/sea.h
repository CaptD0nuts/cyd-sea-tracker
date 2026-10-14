#pragma once
#include <Arduino.h>
#include "config.h"

enum Kind : uint8_t { K_SHARK, K_TURTLE, K_WHALE, K_DOLPHIN, K_SEAL, K_FISH, K_GATOR, K_OTHER };

struct Animal {
  char name[20];
  char species[32];  // common name only, Latin dropped
  char length[12];   // compacted, e.g. 12ft5in
  char gender;       // 'F', 'M' or '?'
  Kind kind;
  uint32_t fixEpoch; // UTC seconds of the last position fix
  float distNm;      // great-circle distance from home
  float brgDeg;      // initial bearing from home, 0 = north
};

struct SeaSnapshot {
  Animal animals[MAX_ANIMALS];  // nearest first
  int count;
  uint32_t fetchedMs;  // millis() of the last good fetch
  bool everOk;
  int lastError;       // HTTP code or negative client error of the last failure, 0 if fine
};

void seaBegin(float lat, float lon);
void seaSnapshot(SeaSnapshot &out);
// Current UTC time from NTP, or 0 if the clock isn't set yet.
uint32_t seaNow();
const char *kindName(Kind k);
