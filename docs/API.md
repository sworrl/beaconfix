# LAN API

The tray serves `http://<beaconfix-host>:47822/api/v1/` (settings `apiEnabled`, default on;
`apiPort`, default 47822; if the port is busy the next nine are tried). The
service is advertised over Avahi as `_beaconfix._tcp` with TXT `name=<PC name> id=<identity id> link=1 api=3
port=<port>` (+ `iname host addr kind v features tls pair`, src/mdns.h). If `~/.config/sworrl/beaconfix.crt` and `beaconfix.key` exist the server speaks
TLS instead of plain HTTP.

## Access control

1. **LAN only.** Peers outside private and link-local ranges (10/8, 172.16/12, 192.168/16,
   127/8, fe80::/10, fc00::/7 and their IPv4-mapped forms) get `403` before the body is read.
2. **Known devices** (`apiKnownOnly`, default on): tokens only work from a peer that is in the
   known-device list (matched by `fixed_ip` / `ip`, then by MAC through the neighbour table for
   peers on this host's own subnets, then by MAC glob). Unknown peers get
   `403 {"error":"unknown device"}`. Link requests from unknown peers are accepted (the PC user
   compares the code and taps Link); a linked device is added to the list.
3. **Bearer tokens.** Every endpoint except `hello` and the link / pairing endpoints needs
   `Authorization: Bearer <token>`. Tokens are 32 random bytes, base64url, shown exactly once.
   Only their SHA-256 is stored (`~/.config/sworrl/beaconfix-devices.json`, mode 0600); comparison
   is constant-time. Scopes: `read`, `control`.
4. **Limits**: 60 requests per minute per address (`401`/`403` count double) → `429` with
   `Retry-After: 60`; 32 open connections; 8 concurrent streams; 4 KB request bodies → `413`.
5. Every request is logged (last 100: time, address, path, status) and shown in the app's
   Devices tab.

All responses are JSON with `Cache-Control: no-store`. Errors: `401` (`WWW-Authenticate: Bearer`),
`403`, `404`, `405` (`Allow:`), `413`, `429`, `503`.

## The hub (`/api/v3`, BFS3)

The same routes are served by the hub (`beaconfix --server`, docs/SECURE-API.md, docs/HUB.md) as
`https://<hub>/api/v3/<route>` — every request and response sealed per device (X25519 enrolment, per-message
ChaCha20-Poly1305 keys, counter + replay window, ±300 s). On the hub, `/api/v1` exists only on its loopback
admin listener with the admin token; the network address answers only `/api/v3/*` and `GET /healthz`. Hub-only
routes: `POST /api/v3/enroll` (plain JSON), `POST nodes/heartbeat`, `POST jobs/lease`, `POST jobs/<id>/result`,
`POST jobs/results`, `GET jobs`, `POST hub/invites` (a PC linking a phone: [LINKING.md](LINKING.md)); admin (loopback, `X-BF-Admin`): `GET hub/status`, `GET hub/devices`,
`POST hub/invite`, `POST hub/revoke`. `/api/v1/db/changes` rows keep their own `device` for fixes too (a hub's feed
carries every node's), so a node stores the phone's stops under the phone's name.
Plate events (3.10, [SIGHTINGS.md](SIGHTINGS.md) §5): `db/changes` carries `plateEvents` (records only) and `db/sync`
accepts them; `GET plate-events…` work over v3 too and `POST plate-events` needs the `sync` scope. The hub stores no
images: `POST plate-events/<uid>/media` answers `404` there and the media routes are served by the node (the desktop).

On a node, `GET /api/v1/state` carries `"hub": {enrolled, url, fingerprint, deviceId, lastSync, lastOk, lastError,
nextSync, failures, jobsDone …}` and `linkedDevices` includes every node the hub knows (positions pulled every 30 s).

## Linking (v3: QR or mDNS, a six-digit code on both screens, nothing typed)

Normative: [LINKING.md](LINKING.md). The PC's **Link a device…** (tray menu, Devices tab) shows a QR; a phone that
scans it is linked at once, a phone that picks the PC from its mDNS list asks and the PC user taps **Link** after
comparing the code. Either way the phone receives, sealed to the session key, a read + control token for this API
and (when the PC is enrolled with a hub) a hub invite. D-Bus for scripts / a PC without a screen: `LinkOffer()` →
`{sid, qr, expires}`, `LinkQr(sid)`, `LinkSessions()`, `LinkApprove(sid)`, `LinkReject(sid)`, `LinkCancel(sid)`.

```
phone                                             PC
  QR path
  |  POST /api/v1/link {sid,name,kind,pub,mac}       |   202 {"sid","pub","status":"approved","expires"}
  mDNS path
  |  POST /api/v1/link {name,kind,commit,proximity?} |   202 {"sid","pub","status":"commit","expires"}
  |  POST /api/v1/link/<sid> {pub}                   |   202 {"status":"pending"} — the PC shows "<name> wants to link — 123 456 [Link] [Reject]"
  both
  |  GET /api/v1/link/<sid>   (every 2 s)            |   {"status":"pending"} | {"status":"denied","reason"} | {"status":"approved","sealed"} (once)
  |  POST /api/v1/link/hub-invite  (token, control)  |   {"hub":"bfs3:…"}  (409 no hub, 503 hub unreachable)
```

## Pairing (v2: pictures + proximity — old apps only)

The server still answers this for app versions that predate linking, but no PC UI opens pairing any more
(only `beaconfix --pairing <minutes>`), and a pending v2 request can only be denied from the Devices tab.

```
device                                            BeaconFix (desktop)
  |  GET /api/v1/hello                               |   {"pairing":true, "api":2, "identity":{…}|null, "mdns":true, …}
  |------------------------------------------------->|
  |  POST /api/v1/pair                               |   202 {"id","code","expires","poll",
  |    {"name","kind","scopes",                      |        "proximity":{…verdict…},"sas":{"pub":<their X25519 pub>}}
  |     "identity":{id,pub,name}?,                   |   (only while pairing is open; ≤5 pending; 10 min)
  |     "sas":{"pub":<our X25519 pub>},              |
  |     "proximity":{"beacons":[{bssid,dbm}…],       |
  |                  lat,lon,acc,source,ts}}         |
  |------------------------------------------------->|
  |  both sides: shared = X25519(priv, otherPub)     |   the desktop shows THREE rows of three pictures:
  |  sas = HKDF-SHA256(shared, info="beaconfix-pair-sas-v1|"+id, 32 bytes)   one real, two decoys
  |  pictures = sas[0]%48, sas[2]%48, sas[4]%48      |   the device shows its (real) three pictures
  |  GET /api/v1/pair/<id>   (every 5 s)             |   {"status":"pending","proximity":{…},"sas":{"picked":false}}
  |------------------------------------------------->|   … the user taps the row that matches the device's screen
  |  GET /api/v1/pair/<id>                           |   {"status":"approved","scopes":[…],"token":"…"}   (token once)
  |------------------------------------------------->|   or {"status":"denied","reason":"wrong pictures"} / "cancelled"
  |  POST /api/v1/pair/<id>/cancel                   |   the device gave up: {"status":"cancelled"}
```

**SAS pictures.** 48 fixed icons (`Pairing::ICON_NAMES`, same order in both apps: anchor, apple,
balloon, banana, bell, bicycle, boat, book, butterfly, cactus, camera, candle, car, castle, cat,
cherry, clock, cloud, crown, diamond, dog, drum, elephant, feather, fish, flag, flower, fox, guitar,
hammer, heart, house, key, kite, leaf, lemon, lightbulb, moon, mushroom, owl, pencil, pizza, rocket,
star, sun, tree, umbrella, zebra). A wrong pick **denies** the request (`reason: "wrong pictures"`)
and raises an `error` event: only someone who sees both screens can pair. The 4-digit code still
exists (*Use the code instead…*) for devices without a screen.

**Proximity.** The request carries what the device hears; the desktop compares it with **every**
beacon of its own latest scan (home and travelling networks included: two radios in the same RV hear
its router at nearly the same level, which is the best co-location evidence there is) and the two
fixes:

| field | meaning |
|---|---|
| `shared`, `theirs`, `ours` | beacons heard by both / by the device / by us |
| `gainOffsetDb` | median of (their dBm − our dBm) over the shared beacons: one radio simply hears everything louder |
| `rssiDelta` | median absolute residual after removing that offset (dB) — how differently the two see the same room |
| `distanceM` | between the two fixes (absent when either is IP-based) |
| `strongestShared` | up to three names, by the device's level |
| `verdict` | `adjacent`: shared ≥ 4 and rssiDelta ≤ 4 dB · `room`: shared ≥ 2 and ≤ 7 dB · `near`: ≥ 1 shared or fixes ≤ 150 m · `far`: nothing shared and > 500 m apart (or > 5 beacons each side, none shared) · `unknown` otherwise. **3.7:** when ranging evidence exists the posterior's class replaces this rule (below) |
| `ranging` | 3.7: `{"class","distanceM","lowM","highM","sigmaM","method":["ble","wifi-diff","fix"],"sharedGroups","evidence","scoreVerdict"?}` — the requesting device's BLE advert (found through its identity's rotating tag), the shared-beacon fingerprint and both fixes, fused as in [RANGING.md](RANGING.md) §5.5; `class` is `adjacent` (84th percentile ≤ 2 m), `room` (≤ 6 m), `near` (≤ 30 m), `far` (16th percentile > 30 m) or `unknown`; `scoreVerdict` keeps the rule-of-thumb verdict it replaced |

Policy `apiPairProximity` (Devices tab, default `required`): `required` lets only
`adjacent` / `room` / `near` requests be picture-matched or auto-approved (*Pair anyway* needs the
word "pair" typed); `warn` shows the verdict and allows everything; `off` ignores it.
**Known devices** (`beaconfix-known.json`, address match) are auto-approved without pictures only
when the verdict is allowed; the response then has `"autoApproved":true` and the dialog shows
*Auto-approved (read access)* with **Allow control too**, which adds the control scope to the
token that request produced (`grantControl`). A device whose identity is linked to yours does not
need that: identity sign-in (`/identity/auth`) already yields a read + control token.

Open pairing (old apps) with `beaconfix --pairing 10` (minutes; `0` closes). Manual tokens: *Create token…* in the Devices
tab, or `beaconfix --token "Photo frame"` (add `--control` for the control scope). Revoke with
`beaconfix --revoke <name-or-id>`; list with `--devices`; status with `--api-status`.
**Upgrade an existing device to control** without a new token: `beaconfix --grant-control
<name-or-id>` (D-Bus `GrantControl(nameOrId)`); the device keeps its token and gains
`[read,control]` — e.g. so a phone can push its observations through `/db/sync`.

**Device kind.** `kind` (`android`, `laptop`, `desktop`, `pi`, `gnss`, `device`) comes from the
pairing request, the identity sign-in or `POST /devices/position`; for tokens paired before the
kind was recorded the desktop infers it from the client's `User-Agent` (OkHttp / Android →
`android`) and from the kind bits of the device's BLE advert, and remembers it.

## Endpoints

Identity endpoints (challenge sign-in, linking, bundle hand-off) are specified in [IDENTITY.md](IDENTITY.md); the table lists them for completeness.

| method | path | scope | response |
|---|---|---|---|
| GET | `/api/v1/hello` | none | `{"name","version","hostname","pairing","tls","ts","api":3,"link":true,"pcName","kind":"desktop","mdns":<bool>,"identity":{"id","name"}|null,"features":["sync","locate","home","events","stream","estimates","identity","peers","anchors","ranging","aps-paging","grant-control","pediatric","whoami","grades","flock","heatmap","plate-events","camera-trust","route-avoid"]}` (`camera-trust`, `route-avoid`: the camera routes and routing below; `plate-events`: 3.10, the plate-event routes below; `pediatric`: 3.8, the pediatric ER fields below; `whoami`: 3.8, `GET /devices/me`; `grades`: 3.9, graded estimates below) |
| POST | `/api/v1/link` body `{"sid","name","kind","pub","mac"}` (QR) or `{"name","kind","commit","proximity"?}` (mDNS) | none | QR: `202 {"sid","pub","status":"approved","expires"}`, `403` bad MAC, `404` unknown / expired, `409` used; mDNS: `202 {"sid","pub","status":"commit","expires"}`, `400` without `commit`, `429` when three wait or the source spent 12 a minute ([LINKING.md](LINKING.md)) |
| POST | `/api/v1/link/<sid>` body `{"pub"}` | none | mDNS: the key behind the commitment → `202 {"sid","status":"pending","expires"}`; `403` mismatch (the request is denied) or another address; `409` sent twice |
| GET | `/api/v1/link/<sid>` | none | `{"status":"pending"}` · `{"status":"denied","reason"}` · `{"status":"approved","sealed"}` (once, then `404`) |
| POST | `/api/v1/link/hub-invite` | control | a fresh hub invite for the calling device `{"hub":"bfs3:…"}`; `409` this PC is not enrolled, `503` the hub is unreachable |
| POST | `/api/v1/pair` body `{"name","kind","scopes":["read"],"identity":{id,pub,name}?,"sas":{"pub"},"proximity":{beacons[],lat,lon,acc,source,ts}}` | none | `202 {"id","code","expires","poll","proximity":{…},"sas":{"pub"},"autoApproved"?}`; `403` when pairing is closed; `429` when five are pending |
| GET | `/api/v1/pair/<id>` | none | `{"status":"pending","proximity":{…},"sas":{"picked":<bool>}}` · `{"status":"denied","reason":"wrong pictures"?}` · `{"status":"cancelled"}` · `{"status":"approved","scopes":[…],"token":"…"}` (token once); `404` unknown/expired |
| POST | `/api/v1/pair/<id>/cancel` | none | the device withdraws its request → `{"status":"cancelled"}` |
| GET | `/api/v1/peers` | read | BeaconFix devices on this network (mDNS `_beaconfix._tcp` + hello): `{"peers":[{"name","host","addresses":[…],"port","url","identityId","identityName","version","kind","api","features":[…],"pairing","tls","self","interface","lastSeen","source":"mdns"|"scan","sameIdentity","linked","ours"}],"count","scanned","mdns","self":{"host","addresses":[…],"port"},"ts"}` |
| GET | `/api/v1/peers?scan=1` | control | the same after probing every host of the local /24s (networks that block multicast); answers when the scan is done |
| GET | `/api/v1/devices/me` | read | the calling token's own record: `{"name","kind","scopes":["read","control"?],"identity":<id>|null}` (3.8, feature `whoami`). A phone re-reads its scopes here on every sync: `beaconfix --grant-control` upgrades a token after pairing |
| GET | `/api/v1/devices/positions` | read | our other devices on the map: `{"devices":[{"device","kind","identityId","identityName","lat","lon","acc","time","ageS","source","place","online","beacons","lastSeen","distanceM"?,"range"?}],"count","ts"}` (newest synced fix per peer device, plus what they POST below). `range` (3.7) is the **measured** distance when ranging has data for that device — the object of `GET /ranging` below; `distanceM` stays the fix-to-fix distance |
| POST | `/api/v1/devices/position` body `{"lat","lon","acc","time"?,"source"?,"beacons"?:<int or array>,"kind"?,"place"?,"events"?:[{"type","text","time"?,"lat"?,"lon"?,…}]}` | read | the calling device's own position now → `{"ok":true,"device","events":<n>,"ts"}`; raises `device` / `device_online` events. `events[]` (3.7, ≤ 20 per call): things the device noticed (a Pi agent: GNSS lock gained/lost, PPS, a jump) become events of type **`device`** in our feed — `text` prefixed with the device name, `type` kept as `deviceEvent`, other fields kept, `device` + `kind` added. A `kind` in the body is remembered for the token when it had none. A `pi` / `gnss` device reporting `acc ≤ 5` m while we hear a home network feeds the **rv-gnss** positioning tier ([RANGING.md](RANGING.md) §4.3.7) |
| GET | `/api/v1/location` | read | the fix, see below |
| GET | `/api/v1/state` | read | everything the tray knows (same as D-Bus `StateJson()` / `beaconfix --json`) |
| GET | `/api/v1/events?since=<id>` | read | `{"events":[…],"lastEventId"}` newer than `id` |
| GET | `/api/v1/aps?offset=<n>&limit=<n>` | read | the beacons heard now with estimates and security, paged (3.7): `{"aps":[…],"count","total","offset","next"}` (`limit` 1–5000, default 1000; `next` = the next offset or `null`). No longer builds the whole state, so it answers fast with ~100k beacons in the database |
| GET | `/api/v1/aps?all=1&after=<bssid>&limit=<n>` | read | every beacon in the map database, BSSID order, keyset-paged: `{"aps":[{"bssid","ssid","freq","lat"?,"lon"?,"acc"?,"source","home","travelling","ignored","security","seq"}],"count","next"}` — pass `next` as `after` until it is `null`; anchored BSSIDs report the anchor's position with `source:"anchor"` |
| GET | `/api/v1/anchors` | read | `[anchor…]` — surveyed transmitters / places, the frozen object of [RANGING.md](RANGING.md) §4.1 plus `headingAssumed` and `seq` |
| POST | `/api/v1/anchors` body: an anchor | control | create or update by `id` (absent → a new UUID) → the stored anchor. Validated and normalised (BSSIDs upper-cased, `accM` clamped to 0.05–500, unknown kinds rejected with `400`). The **newest `placedAt` wins**: omit `placedAt` to stamp "now"; an older one leaves the stored anchor as is. `placedBy` defaults to the calling device's kind. `rv:true` anchors get their `rvOffset` measured from the RV reference |
| GET | `/api/v1/anchors/<id>` | read | one anchor; `404` |
| DELETE | `/api/v1/anchors/<id>` | control | `{"deleted": id}` — a tombstone `{"id","deleted":true,"deletedAt","seq"}` travels through `/db/changes` so every device drops it; `404` when unknown |
| GET | `/api/v1/ranging/info` | read | what a peer needs to range with us: `{"rtt":{"bssid","freqMHz","centerFreq0MHz","bandwidthMHz","channel","preamble","enabled","txPowerDbm","anchorId"?}|null,"ble":{"serviceUuid","txPower","txPowerConfirmed","enabled","scanning","intervalMs","error"},"anchor":<this-computer anchor>|null}` — `rtt.enabled` is true only while the responder AP is actually up; `ble.txPower` is our advert's byte 8 and `ble.txPowerConfirmed` says BlueZ reported it as the level the controller selected (not just the request) |
| POST | `/api/v1/ranging` body `{"device","time","rtt":[{"bssid","distMm","stdMm","rssi","burst","n","time"}],"ble":[{"rssi","channel"?,"txPower","time"}],"wifi":[{"bssid","rssi","freq"}],"baro"?,"moving","fix"?:{"lat","lon","acc","time","source"},"rttState"?}` | read | the peer's measurements (times in epoch ms; `ble` = what it heard of **our** advert) → that device's estimate (below). The authenticated device is who it is about. `rttState` = why `rtt` is (not) empty: `ok`, `doze` (Android has RTT off in deep Doze until the phone is unlocked, charged or moved), `wifi-off`, `location-off`, `unavailable`, `unsupported`, `no-permission`, `no-response`, `not-80211mc`, `timeout`, `bad-config`, `no-responder`, `idle`, `away`, `backoff`, `slow` (Android 1.4: nothing moved and the distance held for 2 min, one burst every 30 s), `failed:<code>` (RANGING.md §7). `rtt`, `ble`, `wifi`, `moving`, `ble[].txPower` and `fix.source` are always present from Android 1.3.2 on (older apps omit empty/default values) |
| GET | `/api/v1/ranging` | read | `{"updated","anchor","devices":[{"device","kind","distanceM","sigmaM","lowM","highM","method":["rtt","ble","wifi-diff","wifi-geo","fix"],"bearingDeg","bearingSigmaDeg","dz","class","updated","samples":{"rtt","ble","bleDown","bleUp","wifiDiff","since","total":{"rtt","bleDown","bleUp"}},"lastRtt","rttState","rttStateAt","calib":{"rttOffsetM","rttOffsetSigmaM","bleP0","bleN","bleP0Up","bleNUp","calibrated","calibratedAt","distanceM","rttCalibrated","last":{"at","ok","distanceM","rttBursts","rttAgreed","text"}},"calibrating","lat"?,"lon"?}]}` — `distanceM` is the posterior median, `lowM`/`highM` the 16th/84th percentiles; `lat`/`lon` only when the bearing is observable; `samples` count since the tray started (`since`), `total` includes earlier runs; `rttState` is the peer's last word on its RTT; `calib.rttCalibrated` and `calib.last` are new in 3.8 (a calibration measured the RTT offset; the last calibration's outcome and why, RANGING.md §7–8) |
| POST | `/api/v1/ranging/calibrate` body `{"device"?,"distanceM","durationS"?}` | control | "these two are `distanceM` apart": collects `durationS` (default 20, 5–120) of RTT + BLE, then fixes the RTT pair offset and both BLE `P0`s ([RANGING.md](RANGING.md) §8) → `202 {"device","calibrating":true,"distanceM","until","hint"}`; `device` defaults to the caller |
| GET | `/api/v1/pois` | read | `{"pois":[…],"count","categories":[…],"note","origin":{"lat","lon","time","radiusKm"},"pedsOrigin":{…}|null,"ts"}` places (each with `cat`, `group`, `address`, `phone`, `hours`, `website`, `wheelchair`, `emergency`, `osm`, `osmType`, `osmId`, `d`, `brg`; 3.8 adds `peds`, `er`, `campusEr`, `scope` and, for the help categories, `driveS`/`driveM`/`driveEst` — see "Pediatric ER" below). `categories` is the `poiCategories` table of the state (`key`, `label`, `icon`, `color`, `group`, `groupLabel`, `wide`, `reachKm`); `origin` is where the places were fetched, `pedsOrigin` where the pediatric ER search ran |
| GET | `/api/v1/pois?cat=police,fire&group=kids&radius=<km>` | read | filtered places: categories and/or groups (`civic`, `kids`, `services`), within `radius` km; the same top-level `categories`, `note`, `origin`, `pedsOrigin` |
| GET | `/api/v1/emergency` | read | nearest `police`, `fire`, `hospital` (the nearest **general** ER, else the nearest hospital), `urgent`, `pharmacy`, `vet` with `d` / `brg` / `phone` / `address` / `hours` / `website` / `osm` / `driveS` / `driveM` / `driveEst`, and `number` — the local emergency number; 3.8 adds `pediatric`, `pediatricCloser`, `pediatricUrgent`, `pediatricNote`, `pediatricSearchKm`, `pediatricTime`, `origin` (below) |
| GET | `/api/v1/track` | read | `{"track":[…]}` the trip log |
| GET | `/api/v1/trip` | read | `{"stats":{…}}` |
| GET | `/api/v1/flock?bbox=s,w,n,e` / `?lat=&lon=&km=` / `&limit=` / `?all=1` | read | `{"cameras":[…],"area","limit","truncated","stats","loading"}` surveillance (ALPR) cameras in an area, nearest first: the box, or `km` (default 50, ≤ 2000) around `lat`/`lon` (default: the fix); `limit` default 5000, ≤ 20000. A nationwide sync stores ~140k, so the whole table only comes with `all=1` (or `?geojson=1`, the export) |
| GET | `/api/v1/plate-events?since=<seq>&limit=<n>&kind=camera_pass\|plate_search` | read | plate events ([SIGHTINGS.md](SIGHTINGS.md)), the feed: `{"events":[…],"cursor","more"}` oldest first after `since` (`limit` 1–1000, default 200). Each event = every `plate_events` column **under its column name** (`uid`, `kind`, `plate`, `time`, `lat`, `lon`, `acc`, `camera_id`, `camera_lat`, `camera_lon`, `distance_m`, `speed_kmh`, `heading_deg`, `approach_bearing_deg`, `camera_dir_deg`, `facing`, `operator`, `agency`, `model`, `camera_type`, `source`, `source_url`, `source_name`, `confidence`, `leaky`, `details`, `metrics` (an object), `device`, `created_at`, `updated_at`, `seq`) but `raw`, plus `media:[{uid, kind, mime, width, height, bytes, attribution, license, originalUrl?, capturedAt?, jpegReconstructible, originalMime?, originalBytes?}]`. `?latest=1&limit=` (an extension): newest first by `time`, for lists. Feature `plate-events` in `hello` |
| GET | `/api/v1/plate-events/<uid>` | read | one event, `raw` included (an object); `<uid>` may be percent-encoded (`pass:osm:node/123:29843267` holds `/` and `:`), a uid merged into another pass resolves to the survivor; `404` |
| GET | `/api/v1/plate-events/media/<mediaUid>?as=display\|stored` | read | the image: `display` (default) = the original JPEG rebuilt bit for bit (`image/jpeg`) for a JPEG-recompressed JXL, a PNG for any other JXL, WebP as stored; `stored` = the stored bytes (`image/jxl` / `image/webp`). Webcam stills are never served (`404`, SIGHTINGS.md §2.0) |
| POST | `/api/v1/plate-events` body `{"events":[…],"device"?}` | control | a phone's passes / searches (column names; camelCase aliases such as `cameraId`, `distanceM` accepted; `metrics`/`raw` objects or JSON text; `uid` optional — derived per §1.1) merged per §1.1 → `{"accepted","uids":[surviving uid \| null],"errors"?}`. A new `phone_live` / `dashcam` ALPR pass or a new plate search notifies on the desktop |
| POST | `/api/v1/plate-events/<uid>/media` body `{"kind":"dashcam"\|"camera_photo","mime","data":<base64>,"width","height","capturedAt","attribution"?,"license"?,"originalUrl"?}` (≤ 40 MB request) | control | re-encoded losslessly (§3.1) and attached to the surviving event (`camera_photo`: to its camera) → `{"uid":<media uid>,"event"}`; `404` unknown event, `422` not a readable image. The hub refuses it (`404`): media stay on the nodes |
| GET | `/api/v1/plate-events/status` | read | `{"backfill":{kv plate_events_backfill + "running","chunksLeft"?},"hibf":{kv hibf_watch + "enabled","plates","sources":{fetched,updatedAt,files,agencies}},"hibfSources":[{tokens,state,files,records,latest,file,url}],"counts":{total,cameraPass,alprPass,cameraOnlyPass,plateSearch,leaky,latest,passesByType,media,mediaBytes,mediaOriginalBytes},"eyesOnFlock":{kv eyesonflock: status (ok \| error \| blocked), error, fetched, lastAttempt, portals, bytes, camerasMatched, license, attribution},"routing":{ready, default, providers},"roads":{queued, fresh, busy},"active","tools":{cjxl,djxl,ffmpeg}}` |
| POST | `/api/v1/plate-events/backfill` body `{"restart":true}`? | control | start / resume the backfill (`restart` reruns it from the first fix) → `202 {"ok","restart","backfill"}`; `409` on the hub (it does not detect) |
| | *(plate events, continued)* | | `GET /plate-events/<uid>` and `?latest=1` also carry `agencyFacts` when the camera's operator / the searching agency matches an Eyes on Flock transparency portal (SIGHTINGS.md §4.6; CC BY-SA 4.0, `attribution` and `license` inside). Camera passes' `metrics` carry `pReadBase`, `snapFactor`, `snap` (road snapping, §2.6), `trustFactor`, `trust` (the camera's trust terms, §2.7) |
| GET | `/api/v1/cameras/<id>` | read | one camera ([SIGHTINGS.md](SIGHTINGS.md) §2.6, §2.7, §4.6): the `flock_cameras` row (camelCase, as `/flock`) + `cameraType`, `trust` (0–1), `trustDetail` (`{logit, trust, sourceClass, terms:[{term, value, note}], computed, weights}`), `verdict`, `verdictAt`, `roads` (`{fetched, watchedWay, watched:{wayId, distanceM, bearingDeg, layer, oneway, basis}}`), `agencyFacts`?; `<id>` may be percent-encoded (`osm:node/123`); `404` |
| POST | `/api/v1/cameras/<id>/verdict` body `{"verdict":"present"\|"absent"\|"clear"}` | control | the user saw the camera (log-odds +1) / it is not there (−1.5) / forget it; the trust is recomputed and every pass of the camera rescored → `{"camera","verdict","verdictAt","trust","trustDetail","passesRescored"}`; `400` bad verdict, `404` unknown camera |
| POST | `/api/v1/route/avoid` body `{"to":{"lat","lon"},"from":{"lat","lon"}?,"provider":"ors"\|"graphhopper"?}` | read | a route around the ALPR cameras' cones ([SIGHTINGS.md](SIGHTINGS.md) §8) with the user's own OpenRouteService / GraphHopper key (desktop Settings); `from` defaults to the current fix, `[lat, lon]` arrays accepted → `200 {"provider","providerName","route":{"type":"LineString","coordinates":[[lon,lat]…]},"distanceM","durationS","avoided":{"areas","cameras","corridorCameras","capped","corridorM"},"passes":[{"id","lat","lon","operator","model","distanceM","alongM","inCone","trust"}],"passesInCone","attribution"}`; `400` bad input, `412 {"error","needsKey":true}` no key (or the provider refused it), `502` the provider failed. Over the hub: read scope |
| GET | `/api/v1/route/status` | read | `{"ready","default","providers":[{"id","name","hasKey","signup","terms"}]}` — never the keys |
| GET | `/api/v1/flock/audits?plate=&limit=` | read | (older apps) `{"audits":[{id, plate, cameraId, lat, lon, operator, timestamp, source, confidence, details}],"count"}` — now plate events in the old shape: a pass gives its camera and operator, a search its agency. `/flock/encounters` likewise lists camera passes (`encounterNum` = the pass's number at that camera). `POST /flock/crossref` starts a HaveIBeenFlocked check (within its floors) and returns the plate searches stored; `POST /flock/recalculate` restarts the backfill |
| GET | `/api/v1/home` | read | `{"patterns":[…],"atHome","awayKm","awayText","homeLat","homeLon","homeTime"}` |
| PUT | `/api/v1/home` body `{"patterns":[…]}` | control | replaces the home networks |
| POST | `/api/v1/locate` body `{"wifiAccessPoints":[{"macAddress","signalStrength"}]}` | read | `{"location":{"lat","lng"},"accuracy","used"}` from the internal map, or `404` |
| GET | `/api/v1/db/stats` | read | database statistics (3.9 adds `grades` {letter: count} and `scanCells`) |
| GET | `/api/v1/estimator` | read | the estimator's state (3.9, [GRADING.md](GRADING.md)): `{"version","kappa","calibration":{"anchors","withSamples","fixes","regions","meanNees","kappa","coverage95","regionCoverage95","byGrade":{"A":{"n","medianErrorM"},…},"updated"},"deviceOffsets":{device: dB},"groups":[{"ref","members":[{"bssid","offsetDb"}]}],"grades":{letter: count},"kinds":{"fix","region","mobile","none"},"upgradePending","scanCells","suggestions":[{"bssid","ssid","grade","lat","lon","gain","distanceM"}],"environment"}` — also D-Bus `EstimatorJson()` and `beaconfix --estimator` |
| POST | `/api/v1/db/observations` body `{"observations":[{"bssid","ssid","dbm","lat","lon","acc","time","rangeM"?,"rangeSd"?}]}` | control | merges another device's observations into the map; `rangeM`/`rangeSd` (m, 1-σ) when the AP answered Wi-Fi RTT (also in `/db/sync` and `/db/changes`); times with an offset or `Z` are stored as local time |
| GET | `/api/v1/db/changes?since=<seq>&limit=<n>` | read | sync feed: `{since,cursor,more,count,device,identity,aps[],observations[],fixes[],anchors[],plateEvents[]}` after a cursor, oldest first (every row of aps / observations / fixes carries `identity`; `anchors` holds anchors and tombstones, 3.7; `plateEvents` (3.10) = plate-event records with `raw`, no media — [SIGHTINGS.md](SIGHTINGS.md) §5) |
| POST | `/api/v1/db/sync` body `{"device","identity"?,"observations":[…],"aps":[…],"fixes":[…],"anchors"?:[…],"plateEvents"?:[…],"nodeDetections"?:[…],"sinceCursor"?}` | control | merges a peer's data (1 MB bodies), queues refits, returns `{accepted:{observations,aps,fixes,anchors,plateEvents,nodeDetections},cursor,refitQueued,identity,changes?}`; anchors merge by newest `placedAt` / `deletedAt`, plate events per [SIGHTINGS.md](SIGHTINGS.md) §1.1 (a row without `device` gets the sender's name); `403 identity not linked` when the peer names an identity that is not yours or linked |
| GET | `/api/v1/node-detections?since=<ms>&limit=<n>&node=<name>&kind=<k>` | read | what ESP32 nodes handed a phone over BLE and the phone pushed with `/db/sync` `nodeDetections` (`{"uid","node","kind","mac","ssid","rssi","ch","detail","timeMs","lat"?,"lon"?,"acc"?,"locSource","stored"}`, `kind` probe \| beacon \| alert \| ble_tracker, `uid` the merge key, a row already stored still counts as accepted): `{"detections":[…],"count"}`, newest first, `limit` 1–5000 (default 500) |
| GET | `/api/v1/db/export` | control | JSON dump of the database |
| POST | `/api/v1/db/import?name=<file>&from=<ISO>&to=<ISO>&what=positions,wifi,places` body: the raw file (≤ 200 MB, streamed to a temporary file) | control | imports your history — Google `Timeline.json` / `Records.json` / Semantic Location History, WiGLE CSV, GPX, KML or a BeaconFix export ([DATABASE.md](DATABASE.md)) → the summary object with `"ok"`; `400 {"ok":false,"error"}` when unreadable; `409` while another import runs; `413` above the limit |
| GET | `/api/v1/identity` | none | the public identity record + `linkedIds`, `grouped`, `unlocked`; `404` until one exists |
| GET | `/api/v1/identity/challenge` | none | `{"nonce","host","expires","id"}` — 60 s, single use |
| POST | `/api/v1/identity/auth` body `{"id","pub","device":{"name","kind"},"nonce","sig"}` | none | `{"token","scopes":["read","control"],"identity":{id,name},"device"}` when the identity is yours or linked; `403 unknown identity` (listed for linking) otherwise |
| POST | `/api/v1/identity/link` body: a link statement (`a`,`b`,`ts`,`sigA`,`sigB`, optional `pubA`/`pubB`), or `{"statement":"beaconfix://statement/…"}` | none | verifies, co-signs **only when `ts` is a link offer this BeaconFix displayed in the last 10 minutes** (single use), stores when complete → `{"statement","linkedIds"}`; `403` otherwise |
| POST | `/api/v1/identity/export` body `{"passphrase"?}` | control | holds our encrypted bundle 10 min under a one-time code → `{"code","expires","fetch","words"?}` (`words` = generated 6-word code when no passphrase was given) |
| GET | `/api/v1/identity/export/<code>` | none | `{"bundle":"beaconfix://identity/…"}` once; `404` afterwards or after 5 wrong codes |
| GET | `/api/v1/stream` | read | Server-Sent Events: `event: fix`, `event: beacon` (one event JSON), `event: ping` every 30 s |

Events (`/events`, the stream, D-Bus `eventLogged`) added in 3.6: `ap_refit` — a beacon's estimate
moved (> max(5 m, 10 % of the old accuracy)) or tightened (> 15 %): `bssid`, `ssid`, `lat`/`lon`
(new), `fromLat`/`fromLon` (old), `kind:"trilat"`, `acc`, `prevAcc`, `n`, `vantage`, `rms`,
`movedM`, `vantagePoints:[{lat,lon,dbm,device}]` (≤ 6, at least one per contributing device), text
"Refined <ssid>: ±140 m → ±38 m (24 samples, 5 vantage points)"; `device` — one of your devices
moved > 100 m (`device`, `kind`, `movedM`, `distanceM`, `acc`, position); `device_online` /
`device_offline` (`device`; a device is online while it was heard from in the last 10 minutes);
`import` — a history import finished (the summary as fields). Added in 3.7: `anchor` — an anchor
was placed, moved, removed, or the RV's anchors were re-projected after a move (`anchor`, `kind`,
`placedBy`, `reprojected`, `headingAssumed`); `device` also carries the events a device reports in
`POST /devices/position` (`deviceEvent`) and ranging calibrations (`calibrated`, `distanceM`,
`rttOffsetM`).

**Anchors and ranging from the command line / D-Bus (3.7).** `beaconfix --anchors`,
`--anchor-set <json | b64:<standard base64 of the UTF-8 JSON>>` (a JSON array sets several; prints
each id), `--anchor-remove <id>`, `--ranging` (info + every ranged device), `--ranging-calibrate
"<device>@<metres>[@<seconds>]"`, `--grant-control <name-or-id>`. D-Bus (`org.sworrl.BeaconFix`):
`Anchors()` → JSON array, `SetAnchor(json)` → id or `"error: …"`, `RemoveAnchor(id)`,
`Ranging()`, `RangingInfo()`, `RangingCalibrate(device, metres, seconds)`, `GrantControl(nameOrId)`.
`--json` / `StateJson()` carry `anchors:[…]`, `features:[…]` and `environment` (the per-band
path-loss fit learned from anchors: `{"2.4"|"5"|"6": {"p0","n","sigmaP0","sigmaN","samples"}}`).
| POST | `/api/v1/refresh` | control | re-check the position now (`202`) |
| POST | `/api/v1/prefetch` | control | save map tiles around the fix (`202`) |

### Graded estimates (3.9, feature `grades`)

Every AP object (`/aps`, `/state`, D-Bus `StateJson()`) whose position is our own estimate carries
a grade ([GRADING.md](GRADING.md)). New values of `kind`: **`region`** (only an area is known:
`lat`/`lon` the centre, `r` = R95) and **`mobile`** (it travels: `lat`/`lon` where it was last
heard). New top-level fields: `grade` (`A`–`F`, `R` region, `M` mobile), `score` (0–100),
`r95` (m). The `fit` object keeps its 3.6 keys (`n`, `vantage`, `rms`, `acc`, `p0`, `pathloss`,
`quality`, `rejected`, `semiMajor`, `semiMinor`, `orient`, `updated`; `acc`/`semiMajor` are 1-σ,
`orient` the bearing of the major axis) and adds:

| key | meaning |
|---|---|
| `kind` | `fix` · `region` · `mobile` |
| `grade`, `pendingGrade`?, `score` | the letter, a letter change waiting for confirmation (hysteresis), 0–100 |
| `r95`, `cep50`, `pWithin25` | radius holding 95 % / 50 % of the probability (m); P(error < 25 m) |
| `cxx`, `cxy`, `cyy` | position covariance (m², x east, y north); the 95 % ellipse is 2.4477 × the 1-σ one |
| `lat`, `lon` | the estimate (equal to the AP's `lat`/`lon` when it is shown) |
| `devices`, `sessions`, `inHull`, `ambiguous`, `modes`, `moved` | see GRADING.md §2 |
| `flags` | `extrapolated`, `ambiguous`, `moved`, `fragile`, `rangeScale` |
| `components` | `{"P","G","E","F","S","T","X"?}` score components 0–1 (X only with a WiGLE/Apple placement) |
| `metrics` | `{"rssDop","crlbR95","rbar","maxGapDeg","inHull","linRatio","chi2nu","sigmaDb","outlierFrac","ess","spearman","p0RangeCorr","dminRatio","modes","driftD2","jackMax","nisEwma","fadingDb","extD2"?,"altLat"?,"altLon"?}` |
| `suggest` | `{"lat","lon","gain"}` where one more sample helps most (gain in nats), when useful |
| `group` | `{"ref","size"}` when pooled with other BSSIDs of the same radio |

`quality` stays for older clients: A/B `good`, C/D `fair`, E/F/R `poor`. `GET /db/changes` puts the
graded keys (`kind` … `flags`, without `components`/`metrics`) into each AP's `fit`; older peers
ignore them. `ap_refit` / `ap_placed` events add `grade`, `prevGrade`, `score`, `r95`, and their text
reads "Refined <ssid>: ±300 m → ±80 m, grade C (…)" (R95 now, not 1-σ).

### Pediatric ER (3.8, feature `pediatric`)

Two categories join the civic group right after `urgent`: `peds_er` (🧸 "Pediatric ER", `reachKm`
150) and `peds_urgent` (🩹 "Pediatric urgent care", `reachKm` 50). Children's hospitals are no
longer `health`, so `hospital` in `/emergency` is the nearest general ER. Each place carries a
tier in `peds`:

| `peds` | meaning | `cat` | shown as |
|---|---|---|---|
| 1 | dedicated pediatric ER, confirmed (`emergency=yes`) | `peds_er` | "pediatric ER" |
| 2 | children's hospital, ER not confirmed | `peds_er` | "ER not confirmed — call ahead", or "ER on campus: <name> — call ahead" when a hospital with an ER is within 600 m (`campusEr`) |
| 3 | general ER with a pediatrics department | `health` | "ER · pediatrics dept." |
| 4 | pediatric urgent care | `peds_urgent` | "not an ER" |

Place fields added in 3.8 (left out at their default): `osmType`, `osmId`, `peds`, `er`
(`"yes"` / `"no"`), `campusEr`, `scope` (`"far"` for places from the pediatric search), and for
`peds_er`, `peds_urgent`, `health`, `urgent`, `police`, `fire`: `driveS`, `driveM`, `driveEst` —
the straight-line distance × 1.4 at 70 km/h, rounded to 5 minutes (at least 5), `driveEst: true`.
The classifier rules are in `src/poiclassify.cpp`, with the shared test fixture
`tests/fixtures/pediatric_tags.json`. A hospital is never a pediatric ER when its name reads like a
rehab, psychiatric, residential or outpatient site, an office or medical building, or a single
department ("… - Cardiology"), or when all of its `healthcare:speciality` values are non-emergency
ones (psychiatry, rehabilitation, dentistry, …); it stays a general hospital.

`/emergency` additions:

```json
{
  "pediatric": {"name", "lat", "lon", "d", "brg", "phone", "address", "hours", "website", "osm",
                "tier", "er", "campusEr", "driveS", "driveM", "driveEst"} | null,
  "pediatricCloser": { same } | null,
  "pediatricUrgent": { same, "notEr": true } | null,
  "pediatricNote": "No pediatric ER mapped within 150 km — go to the nearest ER",
  "pediatricSearchKm": 150,
  "pediatricTime": "2026-09-27T10:00:00" | "",
  "origin": {"lat", "lon", "acc", "source", "time"} | null
}
```

- **`pediatric`**: rank 0 is tier 1, or tier 2 with an ER on the same campus; rank 1 is tier 2
  without one, or tier 3. The pick is the lowest `driveS` among rank 0, else among rank 1.
- **`pediatricCloser`**: a rank-1 site with a lower `driveS` than a rank-0 `pediatric`.
- **`pediatricUrgent`**: the nearest pediatric urgent care; never an ER.
- **`urgent`** (3.8): the nearest actual urgent care — tagged `urgent_care`, or named like one
  (urgent, express care, after hours, walk-in, immediate care, convenient care, MedExpress) —
  with `"urgentCare": true`; `null` when there is none. The `urgent` category itself still holds
  every clinic and doctor's office.
- **`pediatricNote`** (first match): no search yet and the last one failed → "Overpass busy — will
  retry"; searched and nothing found → "No pediatric ER mapped within N km — go to the nearest
  ER"; the saved answer is older than 30 days, more than half the radius from here, or the last
  refresh failed → "Saved N d ago, X km from here — may be incomplete"; `pediatric` is tier 2 →
  "ER not confirmed — call ahead"; otherwise "". An IP-only fix never starts a search.
- **The search** is its own Overpass query, out to `pedsRadiusKm` (settings key, 50–300 km,
  default 150; the map's context menu sets it): every hospital in that box, children's hospitals
  mapped only as a building, and pediatric urgent care within 50 km. Only exact key=value
  lookups (every hospital, hospital building and clinic in the boxes, `[timeout:90]`, the client
  waits 120 s): name regexes made the server time out, so the names are matched here. It keeps the nearest 5
  confirmed pediatric ERs (tier 1) plus the nearest 5 other pediatric sites, 5 pediatric urgent cares
  and 8 general ERs (within 80 km), merged with the places list (the places query wins for an object
  both found). It runs again only when the fix moved a quarter of the radius, the answer is 30 days
  old, the radius changed, the answer was classified by older rules (once after an update), or when asked
  (D-Bus `RefreshPlaces()`, the window's and the map's "reload places"). It never runs at the
  same time as the places query (one Overpass query at a time, 5 s apart), and only in the tray
  (`--once`, `--snapshot` and other one-shot commands never query Overpass); failures in a row
  back off 10, 20, 40 … minutes (at most 4 h) and keep the saved answer.

`StateJson()` / `/state` add `pedsNote`, `pedsTime` and `pedsRadiusKm`, and each
`poiCategories[]` entry has `reachKm`. `beaconfix --nearby emergency` prints the pediatric
lines; `beaconfix --nearby pediatric` lists pediatric ERs and urgent care with their tier.

### `GET /api/v1/location`

```json
{
  "valid": true, "lat": 40.00293, "lon": -75.06806, "accuracy": 40,
  "source": "wifi", "provider": "internal",
  "place": "Example, Somewhere", "city": "Example", "region": "Somewhere", "country": "…",
  "elevation": 120, "time": "2026-09-26T17:13:46", "age_s": 42,
  "sun": {"sunrise": "…", "sunset": "…", "solarNoon": "…", "goldenEveningStart": "…", "civilDawn": "…", "civilDusk": "…", "dayLength": 43200},
  "geo": "geo:40.00293,-75.06806;u=40",
  "links": {"osm": "https://www.openstreetmap.org/?mlat=…", "google": "…", "apple": "…"},
  "home": {"atHome": true, "awayKm": 0, "awayText": "at home", "homeLat": …, "homeLon": …, "homeTime": "…", "patterns": ["…"]}
}
```

`source` is `starlink` | `wifi` | `ip`; `provider` is `internal` | `beacondb` | `apple` for Wi-Fi
fixes. `accuracy` is metres (IP fixes report 50000).

## Examples

```sh
H=http://<beaconfix-host>:47822/api/v1
curl $H/hello
curl -X POST -H 'Content-Type: application/json' -d '{"name":"Photo frame","scopes":["read"]}' $H/pair
curl $H/pair/<id>                                   # repeat until "approved"
curl -H "Authorization: Bearer $TOKEN" $H/location
curl -N -H "Authorization: Bearer $TOKEN" $H/stream   # SSE
curl -X POST -H "Authorization: Bearer $TOKEN" $H/refresh
```

### Python

```python
import json, time, urllib.request

BASE = "http://<beaconfix-host>:47822/api/v1"

def call(path, token=None, body=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method or ("POST" if data else "GET"))
    req.add_header("Content-Type", "application/json")
    if token: req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)

pair = call("/pair", body={"name": "laptop", "scopes": ["read"]})
print("show this code to the BeaconFix user:", pair["code"])
while True:
    p = call(pair["poll"].replace("/api/v1", ""))
    if p["status"] == "approved": token = p["token"]; break
    if p["status"] == "denied": raise SystemExit("denied")
    time.sleep(5)
loc = call("/location", token)
print(loc["place"], loc["lat"], loc["lon"], "±", loc["accuracy"], "m")
```

### Android (Kotlin, `HttpURLConnection`, no libraries)

```kotlin
val base = "http://<beaconfix-host>:47822/api/v1"     // or resolve _beaconfix._tcp with NsdManager

fun json(path: String, token: String? = null, body: String? = null): JSONObject {
    val c = URL(base + path).openConnection() as HttpURLConnection
    c.connectTimeout = 5000; c.readTimeout = 10000
    if (token != null) c.setRequestProperty("Authorization", "Bearer $token")
    if (body != null) { c.requestMethod = "POST"; c.setRequestProperty("Content-Type", "application/json"); c.outputStream.use { it.write(body.toByteArray()) } }
    if (c.responseCode == 401) throw SecurityException("token rejected")     // forget it and pair again
    return JSONObject(c.inputStream.bufferedReader().readText())
}

var token = prefs.getString("beaconfix_token", null)
if (token == null) {
    val r = json("/pair", body = """{"name":"Photo frame","scopes":["read"]}""")
    showCode(r.getString("code"))
    while (true) {
        val p = json("/pair/" + r.getString("id"))
        when (p.getString("status")) {
            "approved" -> { token = p.getString("token"); prefs.edit().putString("beaconfix_token", token).apply() }
            "denied" -> return
        }
        if (token != null) break
        Thread.sleep(5000)
    }
}
val loc = json("/location", token)
if (loc.getBoolean("valid")) useLocation(loc.getDouble("lat"), loc.getDouble("lon"), loc.getString("place"))
```

A device that is in the known-device list with `ours: true` is approved automatically; its
pairing request returns `approved` on the first poll. See CONFIGURATION.md for the file format.
