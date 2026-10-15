# Sea Tracker CYD

A standalone sea-life tracker for the **Cheap Yellow Display** (ESP32-2432S028R, 2.8" 320x240 touch). It pulls every animal tagged by [OCEARCH](https://www.ocearch.org/tracker/) (sharks, sea turtles, seals, swordfish, alligators and dolphins) straight from their public tracker feed over WiFi. It draws them on a sweeping sonar scope centered on your location and shows details for the nearest one. No server or other hardware needed.

- Cyan sonar scope with a rotating sweep, range rings and a north mark
- Each animal is drawn at its last known position: sharks as a fin, turtles as a shell, everything else as a diamond. Blips brighten as the sweep passes over them
- Side panel for the nearest animal (or the one you tap): name, species, sex, length, distance and bearing, and how long ago its position was recorded
- If the nearest animal is beyond the outer ring, a white arrow on the rim points toward it
- **−** / **+** buttons change the range through 100, 250, 500, 1000 and 2500 nautical miles
- A freshness filter hides animals whose last position is older than 30, 90 or 365 days (or shows any age)
- Range and freshness settings are saved across power cycles

## Why the freshness filter

OCEARCH keeps every animal's last known position on the map, even after its tag goes quiet. Many positions are years old, so the closest animal on the raw map is often one that hasn't reported since 2016. By default the board only shows positions from the last 90 days. Tap the right panel to change that.

A tag only reports a position when the animal is at the surface with a satellite overhead, so even "fresh" fixes can be days or weeks old.

## Hardware

Any ESP32-2432S028R "Cheap Yellow Display" (the version with the ILI9341 screen and XPT2046 resistive touch). Plain ESP32, 4 MB flash. The ESP32 only does 2.4 GHz WiFi.

## Build and flash

Uses [PlatformIO](https://platformio.org).

```sh
pio run -t upload --upload-port COM4    # your serial port
```

## Setup: WiFi and location

The board needs your WiFi and the spot to center the scope on. There are two ways to set them.

**Option A: settings file (easiest when flashing from a computer).**
Copy `include/secrets.example.h` to `include/secrets.h`, fill in your WiFi name and password and your latitude/longitude, then build and flash. For an open network, leave the password as `""`. `secrets.h` is in `.gitignore`, so it never gets committed.

The file has room for a second network with its own location (the `SECRET2_` lines), for example home and a vacation rental. At power-on the board scans, joins whichever listed network it can see (the first one if both are in range), and centers the scope on that network's location. Delete the `SECRET2_` lines if you only need one.

To get coordinates: in Google Maps, long-press your location and copy the two numbers. West longitudes are negative.

**Option B: phone setup page.**
Without `secrets.h` (or if its WiFi fails), the board opens a hotspot called `SEA-TRACKER-SETUP`. Join it from your phone, open `http://192.168.4.1` (turn mobile data off if the page won't load), choose **Configure WiFi**, pick your network, and fill in latitude and longitude on the same page. Pasting both coordinates into the latitude box (`40.7128, -74.0060`) works too.

To reopen the setup page later, hold a finger on the screen while plugging the board in, and keep holding for 3 seconds. A location saved on the setup page takes priority over the ones in `secrets.h`, wherever the board is.

## Using it

| Action | Result |
|---|---|
| Tap an animal | Shows its details in the panel |
| Tap empty scope | Back to auto (nearest animal) |
| Tap **−** (bottom-left of the scope) | Zoom out |
| Tap **+** (bottom-right of the scope) | Zoom in |
| Tap the right panel | Cycle freshness: 30 days, 90 days, 365 days, any age |

Press firmly: the resistive touch ignores very light taps.

The bottom of the panel shows the current freshness setting (`FIX < 90D`) and how many animals are on the scope (`7 IN 500NM`). The status line turns red with `NO SIGNAL` if the feed hasn't loaded for three hours.

## How it works

- The feed is OCEARCH's public map on Mapotic: `https://www.mapotic.com/api/v1/maps/3413/pois.geojson/` (no key needed). It's about 450 KB for ~470 animals, too big to hold in the ESP32's memory, so the board reads it off the network one animal at a time and keeps only the nearest ones (`include/config.h`: `MAX_RECENT`, `MAX_OLD`).
- The board refreshes every 30 minutes (retrying after 2 minutes on failure); tags report a few times a day at most. A refresh takes about 7 seconds and runs on the second CPU core, so the scope keeps animating.
- The age of a position comes from the feed's `last_move_datetime`. The feed's `zping_datetime` is a surfacing ping without a new position, so it isn't used.
- The clock comes from NTP (`pool.ntp.org`). Until it's set, nothing is fetched.
- Distances are great-circle nautical miles from your location; the panel shows statute miles.
- HTTPS certificates aren't checked (`setInsecure`); the data is public and read-only.

## Notes

- OCEARCH doesn't tag whales. Any category name containing "Whale" would still get its own color if one ever shows up.
- The feed is an undocumented public endpoint behind OCEARCH's own tracker page and could change without notice.
- If you fork this, change `USER_AGENT` in `include/config.h` to your own contact.

## Related

**Tide Display CYD** is a companion project for the same board. It draws an animated beach whose water line follows the real tide from NOAA predictions, with live waves, wind and water temperature from an NDBC buoy, the sun and moon at their real positions, and a 36-hour tide chart.

## License

MIT, see [LICENSE](LICENSE). Libraries pulled in by PlatformIO (TFT_eSPI, ArduinoJson, WiFiManager, XPT2046_Touchscreen) keep their own licenses. Animal data belongs to OCEARCH.
