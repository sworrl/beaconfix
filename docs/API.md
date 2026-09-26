# LAN API

The tray serves `http://<beaconfix-host>:47822/api/v1/` (settings `apiEnabled`, default on;
`apiPort`, default 47822; if the port is busy the next nine are tried). When
`avahi-publish-service` is installed the service is advertised as `_beaconfix._tcp` with TXT
`v=1 pair=0|1`. If `~/.config/sworrl/beaconfix.crt` and `beaconfix.key` exist the server speaks
TLS instead of plain HTTP.

## Access control

1. **LAN only.** Peers outside private and link-local ranges (10/8, 172.16/12, 192.168/16,
   127/8, fe80::/10, fc00::/7 and their IPv4-mapped forms) get `403` before the body is read.
2. **Known devices** (`apiKnownOnly`, default on): tokens only work from a peer that is in the
   known-device list (matched by `fixed_ip` / `ip`, then by MAC through the neighbour table for
   peers on this host's own subnets, then by MAC glob). Unknown peers get
   `403 {"error":"unknown device"}`. Pairing requests from unknown peers are still accepted while
   pairing is open, flagged for manual approval.
3. **Bearer tokens.** Every endpoint except `hello` and the pairing endpoints needs
   `Authorization: Bearer <token>`. Tokens are 32 random bytes, base64url, shown exactly once.
   Only their SHA-256 is stored (`~/.config/sworrl/beaconfix-devices.json`, mode 0600); comparison
   is constant-time. Scopes: `read`, `control`.
4. **Limits**: 60 requests per minute per address (`401`/`403` count double) → `429` with
   `Retry-After: 60`; 32 open connections; 8 concurrent streams; 4 KB request bodies → `413`.
5. Every request is logged (last 100: time, address, path, status) and shown in the app's
   Devices tab.

All responses are JSON with `Cache-Control: no-store`. Errors: `401` (`WWW-Authenticate: Bearer`),
`403`, `404`, `405` (`Allow:`), `413`, `429`, `503`.

## Pairing

```
device                                  BeaconFix
  |  GET /api/v1/hello                     |   {"pairing": true|false, ...}
  |--------------------------------------->|
  |  POST /api/v1/pair {"name","scopes"}   |   202 {"id","code":"4831","expires","poll"}
  |--------------------------------------->|   (only while pairing is open; ≤5 pending; 10 min)
  |         shows the code on its screen   |   the same code appears in Devices / notification
  |  GET /api/v1/pair/<id>   (every 5 s)   |   {"status":"pending"}
  |--------------------------------------->|   … user approves (or: auto-approved, known device)
  |  GET /api/v1/pair/<id>                 |   {"status":"approved","scopes":[...],"token":"…"}
  |--------------------------------------->|   the token is returned exactly once
  |  GET /api/v1/location  (Bearer token)  |   200
```

Open pairing from the Devices tab, the tray menu ("Allow a device to pair"), or
`beaconfix --pairing 10` (minutes; `0` closes). Manual tokens: *Create token…* in the Devices
tab, or `beaconfix --token "Photo frame"` (add `--control` for the control scope). Revoke with
`beaconfix --revoke <name-or-id>`; list with `--devices`; status with `--api-status`.

## Endpoints

| method | path | scope | response |
|---|---|---|---|
| GET | `/api/v1/hello` | none | `{"name","version","hostname","pairing","tls","ts"}` |
| POST | `/api/v1/pair` body `{"name":"…","scopes":["read"]}` | none | `202 {"id","code","expires","poll"}`; `403` when pairing is closed; `429` when five are pending |
| GET | `/api/v1/pair/<id>` | none | `{"status":"pending"}` · `{"status":"denied"}` · `{"status":"approved","scopes":[…],"token":"…"}` (token once); `404` unknown/expired |
| GET | `/api/v1/location` | read | the fix, see below |
| GET | `/api/v1/state` | read | everything the tray knows (same as D-Bus `StateJson()` / `beaconfix --json`) |
| GET | `/api/v1/events?since=<id>` | read | `{"events":[…],"lastEventId"}` newer than `id` |
| GET | `/api/v1/aps` | read | `{"aps":[…]}` beacons with estimates and security |
| GET | `/api/v1/pois` | read | `{"pois":[…]}` places |
| GET | `/api/v1/track` | read | `{"track":[…]}` the trip log |
| GET | `/api/v1/trip` | read | `{"stats":{…}}` |
| GET | `/api/v1/home` | read | `{"patterns":[…],"atHome","awayKm","awayText","homeLat","homeLon","homeTime"}` |
| PUT | `/api/v1/home` body `{"patterns":[…]}` | control | replaces the home networks |
| POST | `/api/v1/locate` body `{"wifiAccessPoints":[{"macAddress","signalStrength"}]}` | read | `{"location":{"lat","lng"},"accuracy","used"}` from the internal map, or `404` |
| GET | `/api/v1/db/stats` | read | database statistics |
| POST | `/api/v1/db/observations` body `{"observations":[{"bssid","ssid","dbm","lat","lon","acc","time"}]}` | control | merges another device's observations into the map |
| GET | `/api/v1/db/export` | control | JSON dump of the database |
| GET | `/api/v1/stream` | read | Server-Sent Events: `event: fix`, `event: beacon` (one event JSON), `event: ping` every 30 s |
| POST | `/api/v1/refresh` | control | re-check the position now (`202`) |
| POST | `/api/v1/prefetch` | control | save map tiles around the fix (`202`) |

### `GET /api/v1/location`

```json
{
  "valid": true, "lat": 40.00293, "lon": -75.06806, "accuracy": 40,
  "source": "wifi", "provider": "internal",
  "place": "Example, Somewhere", "city": "Example", "region": "Somewhere", "country": "…",
  "elevation": 317, "time": "2026-09-26T17:13:46", "age_s": 42,
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
