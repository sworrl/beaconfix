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
returns. All automatic zooms (tours, event glides, re-fits after a real move) share one budget
of at most one per 10 minutes, and none starts while the pointer is over the widget, for
3 minutes after you use the map, or while **Follow** is off: once you pan, centre elsewhere or
pick a place, the camera stays where you left it until you press Follow again. Any press on the
map or its controls (a drag, a place marker, a toolbar button, the security chip or panel, a
card button) stops a running glide or tour where it is; switching Cinematic off mid-tour flies
straight home instead.

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
| `tourMinutes` | 20 | minutes between overview zoom-outs (0 never, up to 180; under 10 means every 10) |
| `spotlightMinutes` | 10 | at most one glide to an event per this many minutes (0 never, up to 240; under 10 means every 10) |
| `showMapTab` / `showNearbyTab` / `showRadarTab` / `showTripTab` | true | which tabs exist |
| `binary` | beaconfix | the executable to run |
| `satSource` | 0 | satellite imagery: 0 Esri World Imagery (newest high-res), 1 Esri Clarity, 2 USGS National Map (US), 3 NASA VIIRS (yesterday) |
| `showContours` | true | contour lines (feet) over the satellite map |

Tiles come from the tray's localhost tile server (`http://127.0.0.1:47821/t/<layer>/<z>/<x>/<y>.png`,
tiles only) because OpenStreetMap refuses QML's generic User-Agent; without the tray the widget
falls back to Esri tiles. Layers are `0`–`3`, `L` (satellite labels), and the other satellite
sources `C` (Esri Clarity), `U` (USGS, to z16) and `V` (NASA GIBS VIIRS, yesterday's pass, to z9).
All are free and keyless; each layer shows its own credit line.

Satellite (the default, `mapLayer` 2) is a hybrid for placing beacons in the real world: imagery, then
contour lines (`K`: drawn by the tray from the AWS Terrain Tiles, USGS 3DEP elevation in the US;
100 ft zoomed out down to 5 ft from z18, every fifth labelled), Esri roads (`R`) and place labels (`L`).
The map zooms to z23 (~2 cm a pixel): imagery is scaled past its deepest level, while beacons, error
ellipses, contours, heat tiles and the scale bar (metric and feet) stay sharp. Beacons closer than a
zoom-dependent distance share one area marker with a count (one per beacon from z19); clicking it
zooms in. From z16 the heat map shows each fix as a dot (the vantage points the estimates come from)
over a hairline path, instead of the route glow.

The route heat map is drawn by the tray as transparent tiles too
(`/h/<heatGen>/<z>/<x>/<y>.png`; `heatGen` in the state changes when the route does), so the shell
never strokes thousands of route points itself. While a zoom is in progress the map only scales
what it last painted (GPU) and repaints once it settles: zooming holds 60 fps.

## Nearby: help, addresses and phone numbers (3.5)

The Nearby tab opens with a **nearest help** card: the local emergency number, then the nearest
police station, fire station, hospital with an emergency department and urgent care, each with
distance, bearing, address and a call button (`tel:` link). Three chips filter the list by group
(🚔 emergency & civic, 🛝 kids & fun, ⛽ services); the filter box also matches addresses and
group names. Every row shows the phone number (call button; right-click copies it) and the
address (copy button). The map's *Places to show* menu has one submenu per group with
show-all / hide-all. The compact tooltip adds the resolved time zone and the local emergency
number.

Config keys added: none (categories, groups and hidden lists reuse `hiddenCategories`).

## Nearby: pediatric ER (3.8)

With desktop 3.8 or later (`"pediatric"` in the features list) the help card adds, after the
general ER row (which always stays):

- **🧸 Pediatric ER**: the nearest pediatric emergency department with its distance, bearing,
  estimated drive time ("~1 h 20 min drive (est.)", straight line × 1.4 at 70 km/h unless the
  desktop has a road route) and call button, plus a confidence line: *Dedicated pediatric ER*,
  *Children's hospital · ER on campus: … — call ahead*, *Children's hospital · ER not confirmed —
  call ahead* or *ER with a pediatrics department*.
- **🧸 Closer**: a closer but less certain site, only when the pick above is a confirmed one.
- **🩹 Pediatric urgent care**, always marked *Not an ER*.
- The desktop's pediatric note in italics ("No pediatric ER mapped within 150 km — go to the
  nearest ER", "Saved 4 d ago, 38 km from here — may be incomplete", "Overpass busy — will retry").

A **🧸 Kids ER** chip next to the group chips narrows the list to pediatric ERs and pediatric
urgent care (hidden categories included). Rows show the drive-time estimate when the desktop
sends one. On the map, a cluster takes the icon of the most urgent help inside it (pediatric ER,
then an ER, then police or fire) before its most common category, pediatric ER markers draw over
their neighbours, and the place card adds the ER status, campus ER, drive time and address. The
category list is re-read whenever the desktop's changes, so new categories appear in *Places to
show* without re-adding the widget. With an older desktop none of this shows.
