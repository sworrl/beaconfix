# The Plasma widget

`org.kde.plasma.beaconfix` is a panel or desktop applet that reads the same fix as the tray
(through `beaconfix --json`, started on demand) and shows it on a live map. Install it with
`install.sh` (per user, via `kpackagetool6`) or the Debian package (system-wide), then
*Add Widgets → BeaconFix*.

## Layout

- **Header**: place name, source chip (GPS / WI-FI / IP), coordinates, accuracy, age, elevation,
  sun times, speed and heading while moving or dwell when stopped. Clicking the header runs the
  desktop's *Show Desktop* action so the widget is unobstructed; click again to restore.
- **Map** tab: the beacon map (below).
- **Nearby** tab: places of interest nearest first, with a filter; *Show on map*.
- **Radar** tab: beacons by distance and bearing; names for the strongest, flashes on events.
- **Trip** tab: distances, moving time, speed, elevation, sun, places visited, milestones, stops.
- **Footer**: rank line, counters, live-scan interval, Re-check / OSM / Share / Open app.

In a panel the compact form shows the icon, the place and optionally an accuracy chip and
speed/heading or elevation.

## Map controls

Drag to pan, wheel to zoom about the cursor, double-click to zoom in, pinch on a touchscreen,
right-click or long-press for the context menu. Toolbar: zoom in/out, follow my position
(re-fits the view around the beacons), map style (dark, streets, satellite, topographic),
places filter, **Aa** (Wi-Fi names), **Events** (animations and ticker), **Security**
(highlight insecure beacons and open the audit panel), **Cinematic**.

Markers: gold diamond = mapped position (WiGLE / Apple); gold dot with dashed ring =
multilaterated; hollow diamond = observed in the internal database; cyan dot on a dotted
orbit = distance only, direction unknown; green = connected; magenta = travels with you;
orange = home network; grey = ignored. Clicking a beacon or its name pins a card with SSID,
BSSID, band and channel, level, status, how it was placed and its security notes.

**Cinematic mode**: events glide the camera to their location and back; every `tourMinutes`
the map eases out to city scale with a caption, alternately continuing to state scale, then
returns. Any drag, wheel or pinch cancels the tour and pauses it for 45 s.

**Events**: ripples for new beacons, shrink-and-fade for lost ones, ▲/▼ chevrons with the dB
delta, a glide with a gold flash when a beacon gets placed, a dashed arrow for a new fix, a
pin drop for a new stop, toasts for milestones, regions and offline saves. The ticker at the
bottom shows the newest event with a "+N" count and unfolds to the last five on hover.

**Security overlay**: each beacon is graded from its RSN/WPA flags (see SECURITY.md).
Insecure ones get a red or orange ring and glyph, name pills a coloured border, a summary chip
top-left, and the audit panel lists every beacon worst first with the reasoning and the known
devices attached to that SSID.

## Settings

| key | default | meaning |
|---|---|---|
| `pollSeconds` | 20 | how often the widget reads the state (5–3600) |
| `showPlaceInPanel` | true | place name next to the icon in a panel |
| `showAccuracyInPanel` | false | source + accuracy chip in a panel |
| `showMotionInPanel` | false | speed & heading (or elevation) in a panel |
| `mapLayer` | 0 | 0 dark · 1 streets · 2 satellite · 3 topographic |
| `hiddenCategories` | toilets | place categories hidden on the map |
| `startTab` | 0 | last tab shown |
| `showSsids` | true | Wi-Fi names beside the beacons |
| `showEvents` | true | event animations and ticker |
| `animatedMap` | true | cinematic mode |
| `tourMinutes` | 2 | minutes between overview zoom-outs (1–60) |
| `showMapTab` / `showNearbyTab` / `showRadarTab` / `showTripTab` | true | which tabs exist |
| `binary` | beaconfix | the executable to run |

Tiles come from the tray's localhost tile server (`http://127.0.0.1:47821/t/<layer>/<z>/<x>/<y>.png`,
tiles only) because OpenStreetMap refuses QML's generic User-Agent; without the tray the widget
falls back to Esri tiles.
