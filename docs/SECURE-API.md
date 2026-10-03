# BeaconFix Secure API (BFS3)

The hub (`hub.example.com`, a headless BeaconFix on its own isolated container, reachable
only over WireGuard) holds the master database. Every node — desktops, phones, the Steam Deck,
future agents — talks to it through BFS3: the existing `/api/v1` routes, carried inside a
per-device, per-message authenticated encryption layer, inside TLS. It follows the FalconTechnix
pattern (OTE v2, AD Commando): X25519 device keys, HKDF-SHA512 key derivation, a monotonic counter
per message, a ±300 s time window and replay rejection.

Normative. The reference implementation is `tools/bfs3_ref.py`; its vectors are
`tests/fixtures/bfs3_vectors.json`. C++ (`src/securechannel.*`, OpenSSL) and Kotlin
(`android/…/net/Bfs3.kt`, BouncyCastle) must reproduce every vector byte for byte.

## Primitives

| | |
|---|---|
| Key agreement | X25519 (RFC 7748) |
| KDF | HKDF-SHA512 (RFC 5869) |
| AEAD | ChaCha20-Poly1305, IETF (RFC 8439): 32-byte key, 12-byte nonce, 16-byte tag appended |
| MAC | HMAC-SHA512, lowercase hex |
| Encoding | `b64u` = base64url **without** padding; integers in decimal ASCII; `u64be` = 8-byte big-endian |

Every message key is used exactly once (it is derived from the message counter), so a random
96-bit nonce is safe; the nonce is random anyway (defence in depth).

## Identities

- **Server**: a long-term X25519 key pair `S_sk / S_pk`, generated on first start, stored in the
  hub's encrypted database. `fingerprint = hex(SHA-256(S_pk))[0:32]` — shown at the hub, pinned by
  every node at enrolment.
- **Device**: its own X25519 pair `D_sk / D_pk`, generated on the device, never leaving it
  (Android Keystore-wrapped / the desktop's encrypted store). `deviceId = "d" + hex(SHA-256(D_pk))[0:24]`.

## Root key

```
rk = HKDF-SHA512(ikm  = X25519(D_sk, S_pk)        (= X25519(S_sk, D_pk)),
                 salt = SHA-256("bfs3" ‖ S_pk ‖ D_pk),
                 info = "beaconfix.bfs3.root",
                 L    = 32)
```

## Enrolment (one time per device)

1. At the hub: `beaconfix --server --invite "<name>"` prints an invite (text + QR), single use,
   valid 15 minutes — or an enrolled PC (kind `desktop` / `laptop` / `node`, `control` scope) asks for one with
   `POST /api/v3/hub/invites {"name","kind"}` → `{"invite","expires"}` and hands it to the phone it links, sealed
   ([LINKING.md](LINKING.md); the invite's scopes are the PC's own; ≤ 10 open per PC; logged with who asked):
   `bfs3:` + b64u(JSON `{"u": url, "s": b64u(S_pk), "i": inviteId, "k": b64u(I), "e": expiryUnix}`)
   — `inviteId` 16 hex chars, `I` 32 random bytes.
2. The device generates `D_sk/D_pk` and sends (plain JSON inside TLS — nothing secret in it):
   `POST /api/v3/enroll {"inviteId", "name", "kind", "pub": b64u(D_pk), "ts", "mac"}`
   `mac = HMAC-SHA512(I, "bfs3-enroll\n" + inviteId + "\n" + name + "\n" + kind + "\n" + pub + "\n" + ts)`
3. The hub checks: invite exists, unused, unexpired; `|now − ts| ≤ 300`; the MAC (constant time).
   It marks the invite used, stores the device (id, name, kind, pub, scopes `read,sync,control`),
   and answers `{"deviceId", "fingerprint", "proof"}`,
   `proof = HMAC-SHA512(rk, "bfs3-enrolled\n" + deviceId)`.
4. The device derives `rk`, checks `proof` (it proves the hub holds `S_sk`) and that
   `fingerprint` matches the invite's `S_pk`, then stores `deviceId`, `S_pk`, `D_sk`.

Wrong MAC, used / expired invite, bad time: `403 {"error":"enrolment refused"}` (no detail).

## A request

Method `M`, target `T` (path + query exactly as on the wire, e.g. `/api/v3/db/changes?since=12`),
counter `c` (u64, starts at 1, +1 per request per device, persisted), `ts` (unix seconds).

```
k     = HKDF-SHA512(ikm = rk, salt = "", info = "bfs3 req" ‖ u64be(c), L = 32)
aad   = "bfs3\n" + M + "\n" + T + "\n" + deviceId + "\n" + c + "\n" + ts
body  = ChaCha20-Poly1305(k, nonce, plaintext, aad)          (plaintext may be empty: GET)
```

Headers: `X-BF-Device: deviceId`, `X-BF-Counter: c`, `X-BF-Time: ts`, `X-BF-Nonce: b64u(nonce)`,
`Content-Type: application/vnd.beaconfix.sealed`. The body is the raw ciphertext ‖ tag.

**Bodiless requests (GET, DELETE).** HTTP stacks refuse a body on GET (OkHttp throws, `HttpURLConnection`
turns the request into a POST), so a request whose plaintext is empty sends **no body** and carries the sealed
bytes — exactly the 16-byte tag — in `X-BF-Seal: b64u(sealed)` instead. The hub reads the body when there is one,
else `X-BF-Seal`. (Rev. 2 of this document; the vectors are unchanged: `sealed` of a GET vector is what goes into
the header.)

`T` must be the request target **byte for byte as it goes on the wire** — the client's final, percent-encoded
path + query (OkHttp: `url.encodedPath` + `"?"` + `url.encodedQuery`; Qt: `QUrl::toEncoded(RemoveScheme |
RemoveAuthority | RemoveFragment)`). Keep endpoints ASCII and pre-encoded so nothing re-encodes them.

The hub: unknown or revoked device, `|now − ts| > 300`, a replayed counter, or a failed tag →
`401 {"error":"unauthorized"}`, nothing more (and a per-source rate limit). Replay window: the
highest counter seen `H` and a 128-bit bitmap below it — `c ≤ H − 128` or already seen = replay
(requests may arrive out of order when a phone runs several at once). `H` is persisted.

`/api/v3/<route>` is then served exactly as `/api/v1/<route>` for that device with its scopes
(the plaintext is the v1 request body). Scopes per route: `GET` = `read` (`db/export` = `control`);
`POST db/sync`, `db/observations`, `db/fixes`, `devices/position`, `anchors`, `ranging`, `flock/sighting` and
`DELETE anchors/<id>` = `sync`; every other write = `control`. Not served over BFS3: `pair*`, `identity*`,
`db/import` (LAN-only flows). A missing scope is a **sealed** `403`, an unknown route a sealed `404`.

The device name the hub knows (from enrolment) is the name under which `devices/position` reports appear; a node
should enrol with the same name it puts into `db/sync`'s `"device"` (the desktop uses its hostname for both).

## A response

```
k'    = HKDF-SHA512(ikm = rk, salt = "", info = "bfs3 resp" ‖ u64be(c), L = 32)
aad'  = "bfs3-resp\n" + status + "\n" + deviceId + "\n" + c
```
Headers `X-BF-Counter: c`, `X-BF-Nonce: b64u(nonce')`; body sealed with `k'`. The client rejects a
response whose tag fails or whose counter is not its request's. Every status after a successful open is sealed
(200, 400, 403, 404, 500 …); the HTTP status line carries the same `status` as the AAD.

Informative headers (not authenticated, never needed): `X-BF-Type` on a sealed response is the inner
`Content-Type` (normally `application/json; charset=utf-8`); `X-BF-Time: <hub unix seconds>` is on every hub
response including the plain `401`, so a client can tell "my clock is off" from "I am revoked".
A response without `X-BF-Counter` is unauthenticated (`401`, `429`, `404` for a non-v3 path …): trust only its status.

## Event streams

`GET /api/v3/stream` (server-sent events). Each event line is
`data: b64u(nonce ‖ ChaCha20-Poly1305(k_e, nonce, json, aad_e))` with
`k_e = HKDF-SHA512(rk, "", "bfs3 evt" ‖ u64be(c) ‖ u64be(seq), 32)`,
`aad_e = "bfs3-evt\n" + deviceId + "\n" + c + "\n" + seq`, `seq` = 1, 2, … within the stream
opened by request counter `c`. The response headers carry `X-BF-Counter: c`; there are no `event:` lines — the
v1 event name travels inside: the plaintext is `{"type": "<fix|beacon|ping>", "seq": seq, "data": <the v1 event
data object>}`. The first event (seq 1) is the current `fix`, then `beacon` events as they happen and a `ping`
every 30 s. A client drops the stream on a tag failure or a `seq` gap.

## Operations

The node registry and the job protocol (heavy processing on the nodes, the hub only stores and orchestrates) ride on
BFS3 as ordinary sealed routes: docs/HUB.md.

- `beaconfix --server --devices` lists devices (id, name, kind, last seen, counter);
  `--revoke <deviceId>` revokes one (its requests fail from the next one on; its open streams end).
  `--server --status` adds the listeners, open / used invites (with `by`: `admin` or the PC's device id that asked
  through `hub/invites`) and blocked sources.
- Only `/api/v3/*` and `GET /healthz` (`{"ok":true,"api":"bfs3"}`, no data) are served on the hub's network
  address; anything else is a plain `404` before any body is read. `/api/v1` exists only on the loopback admin
  listener (`127.0.0.1:47823`), and there only with the admin token the hub writes to `<state>/hub-admin.json`
  (0600) on every start — that is how the CLI's `--invite` / `--devices` / `--revoke` reach the running hub.
- Abuse: 20 authentication or enrolment failures from one source within 10 minutes → that source's connections
  are dropped for 15 minutes; plus the general per-source request cap.
- At rest: `S_sk`, device records and invites live in the hub's AES-256-GCM database (S_sk and invite secrets
  additionally sealed with a key derived from the database key); the replay windows are a small sealed file
  (`<state>/bfs3-counters`) rewritten atomically after every accepted request, so a crash never forgets a
  counter. The CT's disk is on the host's LUKS store.
- Node side (desktop): `beaconfix --enroll 'bfs3:…'` (or the tray's "Connect to hub…"); `D_sk` is sealed with
  the map-database key in that node's encrypted database; the request counter is reserved 64 ahead in the
  database, and after an unauthenticated `401` the client jumps it by 256 (recovers from a lost reservation).
