# The hub, its nodes, and the job protocol

The **hub** (`beaconfix --server`, `https://hub.example.com`, reachable over WireGuard only) is data and
orchestration: the master database, the device / node registry, sync (`db/sync`, `db/changes`), every node's live
position, invites and revocation — and a **job queue**. It does no heavy processing of its own: refits (the
estimator), track smoothing and calibration run on the **nodes**:

| Node | How it runs | What it does |
|---|---|---|
| a desktop / workstation | the tray (`beaconfix --tray`), enrolled with `beaconfix --enroll` | scans Wi-Fi, syncs, shows the map, leases and computes jobs |
| a headless node (the RV VM) | `beaconfix --node` (deploy/beaconfix-node.service) | the same without a window or session bus |
| the phone | the Android app | collects (Wi-Fi, RTT, GPS), syncs; later light jobs |

Every request below is BFS3-sealed (docs/SECURE-API.md): `/api/v3/<route>`, the plaintext is the JSON shown, every
reply after authentication is sealed. Scopes: `GET` routes need `read`, the `POST`s here need `sync`.

## Invites from a PC (linking a phone)

`POST /api/v3/hub/invites {"name": "Pixel 10 Pro XL", "kind": "android"}` → `{"invite": "bfs3:…", "expires": <unix>}`
— for an enrolled device of kind `desktop`, `laptop` or `node` holding `control` (others: `403`). Same format and
lifetime as `beaconfix --server --invite` (single use, 15 min), scopes = the inviter's own, at most 10 open per
inviter (`429`). The journal logs `<deviceId> (<name>, <kind>) invited "<name>" (<kind>), invite <id>, from <ip>`;
`--server --status` shows `by` per invite. The PC puts the invite in its Link QR and in the sealed link payload, so a
phone linked on the LAN is enrolled with the hub too (docs/LINKING.md); no invite is ever pasted.

## Node registry

`POST /api/v3/nodes/heartbeat` (every 10 min, and capabilities also ride on every lease):

```json
{"capabilities": {"compute": 32, "gpu": "nvidia", "wifiScan": 2, "rtt": false, "mobile": false, "tensor": false,
                  "version": "3.9.0", "jobs": ["refit"], "role": "desktop", "estimator": 5}}
```
→ `{"ok": true, "pending": 12, "time": 1790900000, "leaseSeconds": 600}`

`compute` = usable cores (0 = does not compute), `jobs` = the job types it runs. The hub keeps them with the device
record; `beaconfix --server --devices --json` / `--status` show them, plus `lastLease`, `jobsDone`, `jobsFailed`.

## Jobs

A job is **(type, key) at a watermark**: the hub's change sequence when the job's input last changed. Types today:

| type | key | queued when | result |
|---|---|---|---|
| `refit` | BSSID (upper case) | new samples of it were merged on the hub | `{"fits": [{"bssid", "fit": <Estimator storage JSON>}], "engine": <Estimator::kVersion>}` — the BSSID and, for a multi-BSSID radio, its group members (same `groupRef`) |

`(type, key)` is unique: more samples for a queued BSSID only raise its watermark.

### Lease

`POST /api/v3/jobs/lease`

```json
{"types": ["refit"], "max": 100, "cursor": 48211, "capabilities": {…}}
```
`cursor` is the node's **pulled** sync cursor (its `db/changes` cursor with this hub): only jobs whose watermark the
node already holds are handed out, so it computes on the same data the hub has (plus its own). Omit it (or −1) to
take any. →

```json
{"jobs": [{"id": "9f2c…", "lease": "41ab…", "type": "refit", "key": "AA:BB:CC:DD:EE:FF", "watermark": 48190, "expires": 1790900600}],
 "leaseSeconds": 600, "pending": 37, "time": 1790900000}
```
Oldest first. A job is leased to one node at a time; a lease that runs out (`BEACONFIX_HUB_LEASE_SECONDS`,
default 600) puts the job back in the queue. A node that answered "cannot" for a job is not offered it again.

### Submit

`POST /api/v3/jobs/<id>/result` — `{"lease": "41ab…", "result": {…}}` or `{"lease": "41ab…", "error": "no samples of … here"}`

`POST /api/v3/jobs/results` — several at once: `{"results": [{"id", "lease", "result" | "error"}, …]}` →
`{"results": [{"id", "status", "accepted", "requeued" | "dropped" | "error"}], "accepted": n}`

| Answer | Meaning |
|---|---|
| `200 {"accepted": true, "requeued": false}` | stored; the job is done |
| `200 {"accepted": true, "requeued": true}` | stored, but the job's input changed while it was leased (its watermark rose): it stays queued at the new watermark |
| `200 {"accepted": false}` | the node said it cannot (`error`): queued for someone else; dropped after 5 attempts |
| `409 {"error": "lease expired or superseded"}` | late or duplicate (expired, re-leased, or not ours): ignore it |
| `400 {"error": "invalid result"}` | the result failed validation (wrong BSSID, a value out of range — lat/lon, accuracy, r95, p0, path-loss exponent, grade, kind); queued again |

The hub stores an accepted refit exactly as if it had computed it: the `estimates` row (with the full metrics) and the
AP's position (`aps`, source `trilat` for a fix), each with a new change sequence — so it reaches every node and the
phone through `db/changes` like any estimate (`aps[].fit`). Anchored BSSIDs are never overwritten.

`GET /api/v3/jobs` → `{"queued", "leased", "byType", "leaseSeconds", "selfComputeIdleMinutes", "lastLease"}`.

### Fallback

`BEACONFIX_HUB_SELF_COMPUTE_MINUTES=N` (default 0 = off): when no node has leased anything for N minutes and jobs are
waiting, the hub computes them itself in 200 ms slices, so the map never stalls for good. Leave it off (or long, e.g.
720) on a small shared host.

### What a node does (the desktop's reference behaviour, src/hubclient.cpp)

1. After every successful sync round and every minute: lease up to `hub/jobBatch` (100) jobs with its pulled cursor.
2. For each `refit`: if its own fit of that BSSID is current (no new samples since its last refit), send it; else refit
   now (the same estimator, deterministic — golden-tested). No samples of it at all → `error`.
3. Submit all results in one `jobs/results`; lease again at once when the batch was full.
4. Opt out with `hub/compute=false` (QSettings); it then reports `compute: 0` and leases nothing.

Not standalone: a node that is **not** enrolled keeps refitting everything locally exactly as before; an enrolled
node does too (its own map), and additionally answers the hub's jobs.

## Live positions

`POST /api/v3/devices/position` (a node's own fix: `lat, lon, acc, time, source, place, kind, beacons`), and
`GET /api/v3/devices/positions` → every node's newest position (`devices[]`: `device, kind, lat, lon, acc, time,
source, place, online, lastSeen …`). The desktop posts its fix when it changes (else every 5 min) and pulls everyone's
every 30 s into its linked devices — the map and the Plasma widget draw them.

## Headless node (`beaconfix --node`)

```
systemctl stop beaconfix-node                       # it owns its database while it runs
beaconfix --node --enroll 'bfs3:…'                  # as the node's user (deploy/beaconfix-node.service)
systemctl start beaconfix-node
```
Positions itself like the desktop (Starlink dish, Wi-Fi via NetworkManager, BeaconDB / Apple, IP), syncs, publishes
its position, computes jobs. No session bus: the database key is a key file. NetworkManager scans need the polkit
rule in deploy/50-beaconfix-node.rules. `BEACONFIX_NODE_API=1` also starts the LAN API (port 47822; loopback is
trusted, e.g. to place a `this-computer` anchor that pins the static site: `curl -X POST localhost:47822/api/v1/anchors -d …`).
