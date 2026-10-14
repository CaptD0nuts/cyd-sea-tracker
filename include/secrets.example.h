#pragma once
// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is in .gitignore, so your WiFi password and location stay local.
// Without secrets.h the board opens a setup hotspot instead.

#define SECRET_WIFI_SSID "PUT-WIFI-NAME-HERE"   // exact name, case-sensitive, 2.4 GHz
#define SECRET_WIFI_PASS "PUT-WIFI-PASSWORD-HERE"

// From Google Maps: long-press your spot, copy the two numbers.
// West longitudes are negative (anywhere in the US starts with a minus sign).
#define SECRET_LAT 0.0
#define SECRET_LON 0.0
