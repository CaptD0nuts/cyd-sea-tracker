#pragma once

#define USER_AGENT "cyd-sea-tracker (github.com/CaptD0nuts)"

// OCEARCH's public tracker map (hosted on Mapotic): every tagged animal with
// its last known position. ~450 KB, parsed one animal at a time.
#define FEED_URL "https://www.mapotic.com/api/v1/maps/3413/pois.geojson/"

#define FEED_POLL_MS   (30 * 60 * 1000UL)  // tags report a few times a day at most
#define FEED_RETRY_MS  (2 * 60 * 1000UL)   // after a failed fetch
#define FEED_STALE_MS  (3 * 3600 * 1000UL) // no good fetch for this long = NO SIGNAL

// Kept per fetch: every animal with a fix in the last year (nearest first),
// plus the nearest older ones for the ALL setting.
#define MAX_RECENT 80
#define MAX_OLD    40
#define MAX_ANIMALS (MAX_RECENT + MAX_OLD)
#define RECENT_DAYS 365

// Range steps for the on-screen -/+ buttons, in nautical miles.
#define RANGE_STEPS {100, 250, 500, 1000, 2500}
#define DEFAULT_RANGE_NM 500
// Tap the right panel to cycle: only show fixes newer than this many days (0 = all).
#define FRESH_STEPS {30, 90, 365, 0}
#define DEFAULT_FRESH_DAYS 90

#define SETUP_AP_NAME "SEA-TRACKER-SETUP"
#define PORTAL_HOLD_MS 3000      // hold the screen this long while powering on to reopen setup
