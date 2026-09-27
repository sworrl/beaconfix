# D-Bus interface

Service `org.sworrl.BeaconFix`, object `/org/sworrl/BeaconFix`, interface `org.sworrl.BeaconFix`
on the session bus. D-Bus activated: calling it starts the tray if it is not running.

```sh
qdbus6 org.sworrl.BeaconFix /org/sworrl/BeaconFix org.sworrl.BeaconFix.latitude
qdbus6 org.sworrl.BeaconFix /org/sworrl/BeaconFix org.sworrl.BeaconFix.StateJson | jq .place
dbus-monitor "interface='org.sworrl.BeaconFix',member='FixChanged'"
```

## Properties

| property | type | meaning |
|---|---|---|
| `valid` | bool | a fix exists |
| `latitude`, `longitude` | double | degrees |
| `accuracy` | double | metres (IP fixes report 50000) |
| `source` | string | `starlink` · `wifi` · `ip` |
| `place` | string | reverse-geocoded name |
| `timestamp` | string | ISO-8601 local time of the fix |
| `apCount` | int | access points heard in the last scan |
| `intervalMinutes` | int, read/write | minutes between position checks |
| `elevation` | double | metres above sea level (−9999 unknown) |
| `speedKmh` | double | speed of the last leg (−1 unknown) |
| `headingDeg` | double | heading of the last leg |
| `rank` | string | current rank name |
| `geoUri` | string | `geo:lat,lon;u=acc` |
| `lastEventId` | int | id of the newest event |
| `apiListening` | bool | the LAN API is up |
| `atHome` | bool | a home network is in range |
| `awayKm` | double | distance from home (−1 unknown) |

## Methods

| method | returns | what |
|---|---|---|
| `Refresh()` | — | re-check the position now |
| `ShowWindow()` | — | raise the app window |
| `StateJson()` | string | everything as JSON (same as `beaconfix --json` and `GET /api/v1/state`) |
| `ExportGpx(path)` | bool | write the trip log as GPX |
| `CopyToClipboard(what)` | bool | `coords` · `geo` · `osm` · `google` · `apple` · `text` |
| `StartTrip()` | — | start a new trip here |
| `PrefetchTiles()` | — | save map tiles around the fix |
| `ApiStatus()` | string (JSON) | port, pairing state, devices, pending |
| `ApproveDevice(id)` / `DenyDevice(id)` | bool | pairing decisions |
| `RevokeDevice(nameOrId)` | bool | revoke a token |
| `CreateToken(name, scopes)` | string | scopes `"read"` or `"read,control"`; the token, once |
| `OpenPairing(minutes)` | bool | 0 closes |
| `HomeNetworks()` / `SetHomeNetworks(list)` | string list / — | the home patterns |
| `ImportHomeNetworks(path)` | int | merge a `home-networks.json`; patterns added |
| `KnownDevices()` | string (JSON) | the known devices |
| `KnownAdd(mac, name)` / `KnownRemove(mac)` | bool | manage them |
| `KnownImport(path)` | int | merge a `known-devices.json` |
| `DbStats()` | string (JSON) | database statistics |
| `DbExport(path)` | bool | JSON dump |
| `DbImport(path)` | int | merge a dump; rows added |

## Signals

| signal | payload | when |
|---|---|---|
| `FixChanged()` | — | a fix was accepted (or a coarse fix was rejected in favour of a recent precise one) |
| `stopAdded(lat, lon)` | doubles | a new entry in the trip log |
| `achievementUnlocked(key, title)` | strings | a milestone |
| `eventLogged(json)` | string | one event (`ap_new`, `ap_lost`, `ap_up`, `ap_down`, `ap_placed`, `ap_insecure`, `fix`, `stop`, `home`, `achievement`, `region`, `prefetch`, `error`) |
| `pairingRequested(json)` | string | a device asked for API access (`id`, `name`, `ip`, `code`) |
| `deviceApproved(name)` | string | a pairing was approved |
| `probeStarted()` / `probeFinished(ok, message)` | — / bool, string | a position check |
| `scanUpdated()` / `poisUpdated()` / `elevationUpdated()` | — | data changed |
| `statusMessage(message)` | string | progress text |
| `notificationFallback(summary, body)` | strings | no notification daemon answered |
| `prefetchRequested()` / `showWindowRequested()` | — | internal |

### Added in 3.4 / 3.5

| method | returns | what |
|---|---|---|
| `Refit()` | int | re-estimate every beacon from its samples; valid fits |
| `Sync(url, token)` | string (JSON) | one sync round with another BeaconFix |
| `RefreshPlaces()` | — | re-query OpenStreetMap for places around the fix |
| `ApplyOs(dryRun)` | string (JSON) | run the OS integration now (time zone, GeoClue, Night Light) |
| `TimeZoneForFix()` | string | the IANA zone resolved for the fix |
| `IdentityJson()` | string (JSON) | public identity record, linked ids, pending link requests |
| `IdentityCreate(name)` / `IdentityImport(textOrPath, passphrase)` / `IdentityForget()` / `IdentityReload()` | bool / — | manage the identity |
| `IdentityExport(passphrase)` | string | `BFID1:` bundle (empty on error) |
| `IdentityLinkPayload()` | string | `BFLNK1:` link payload |
| `IdentityAcceptLink(statementJson)` | string (JSON) | verify / co-sign / store a link statement |

Every slot, property and signal is exported (`ExportAllSlots | ExportAllProperties |
ExportAllSignals`), so `qdbus6 org.sworrl.BeaconFix /org/sworrl/BeaconFix` lists them all.
