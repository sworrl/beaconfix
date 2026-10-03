# Linking a device (v3): QR or mDNS, numeric comparison, nothing typed

One flow links a phone (or any device) to a BeaconFix PC **and** enrols it with the hub. The user never
types a code: they scan a QR on the PC's screen, or tap the PC in the list of PCs discovered on the LAN
(mDNS), and both screens show the same six-digit code to glance at (Bluetooth-style numeric comparison).
It replaces, in both apps' UIs, the picture rows, the 4-digit code, typing "pair" past the proximity check,
and pasting hub invites. (The old `/api/v1/pair*` endpoints stay on the server for old app versions; no PC UI
opens pairing any more.)

**Result of one link:** (1) LAN: the phone holds a read + control token for that PC's `/api/v1` (live view on
the RV LAN); (2) hub: the phone is enrolled with the hub (BFS3, docs/SECURE-API.md) with an invite the PC
obtained from the hub for it. If the hub is unreachable at that moment (WireGuard off), the phone keeps the
invite and enrols automatically when it can (invites are valid 15 min — the PC fetches a fresh one when asked).

Primitives as in BFS3: X25519, HKDF-SHA256 here (named below), ChaCha20-Poly1305 IETF, HMAC-SHA256, SHA-256, b64u
(base64url, no padding). Reference + vectors: `tools/link_ref.py`, `tests/fixtures/link_vectors.json`
(`tests/link_test.cpp` reproduces every value byte for byte; `tools/link_e2e.py` drives both paths end to end
against isolated PC + hub instances). All `expires` / `e` values are unix seconds.

> **Revision 2 (2026-10-02): the mDNS path has a commitment step.** Revision 1 sent the phone's key in the first
> POST. A LAN man-in-the-middle could then pick its own key (or, cheaper, the `sid` it shows the phone) *after*
> seeing both real keys and grind about 10⁶ HKDF evaluations — about a second — until the two six-digit codes
> matched, defeating the comparison. The phone now sends only a hash of its key first and reveals the key after
> it has the PC's key (step 1b), so neither side of a MITM can choose anything after seeing the other's key: one
> chance in a million per attempt. The QR path is unchanged (the MAC with `k` authenticates it). Vector `commit`.

## Discovery (mDNS)

The PC advertises `_beaconfix._tcp` with TXT `name=<PC name>`, `id=<identity id or "">`, `link=1`, `api=3`,
`port=<API port>` (plus `iname=<identity name>`, `host`, `addr`, `kind`, `v`, `features`, `tls`, `pair`). The phone
lists every such service it sees (name + host) on its Link screen. The PC name is its hostname. `GET /api/v1/hello`
says `"api":3, "link":true, "pcName"`.

## The QR

The PC's **Link a device** dialog shows a QR (and the same text, copyable) for a link session, renewed every
10 minutes while the dialog is open and right after each use, single use:

`bflink:` + b64u(JSON `{"v":1, "sid":<16 hex>, "name":<PC name>, "hosts":[<LAN IPv4s>, "<host>.local"],
"port":<API port>, "pub":b64u(<PC X25519 pub for this session>), "k":b64u(<32 random bytes>), "e":<expiry unix>,
"hub":"bfs3:…"|null}`)

Readers parse the JSON (key order is the reference's but must not be relied on; non-ASCII names may be raw UTF-8
or `\u` escapes). `<host>.local` is Avahi's name for the PC (it may be `host-2.local` after a name conflict).

`hub` is a fresh hub invite the PC obtained (below) when the PC is enrolled with a hub; else `null` (the phone
can still be linked on the LAN and ask for an invite later). The invite may join the QR a second after it first
appears (the PC asks the hub in the background); the payload carries what the QR had when it was used.

## The exchange (on the PC's existing `/api/v1`, plain HTTP on the LAN — nothing secret travels in clear)

1. Phone → `POST /api/v1/link`
   - **QR path:** `{"sid", "name", "kind":"android", "pub":b64u(<phone X25519 pub>), "mac"}` with
     `mac = hex(HMAC-SHA256(k, "bflink\n" + sid + "\n" + name + "\n" + kind + "\n" + pub))`.
     A valid MAC proves the phone saw the PC's screen: the PC approves at once (no tap) →
     `202 {"sid", "pub":b64u(<PC pub>), "status":"approved", "expires"}`. A wrong MAC: `403`; an unknown /
     expired / cancelled `sid`: `404`; a QR already used: `409`.
   - **mDNS path, 1a:** `{"name", "kind":"android", "commit", "proximity"?}` with
     `commit = hex(SHA-256("bflink-commit\n" + name + "\n" + kind + "\n" + pub))` — the key itself is **not** sent
     yet. The PC creates a session (fresh `sid` and X25519 pair) → `202 {"sid", "pub":b64u(<PC pub>),
     "status":"commit", "expires"}`. A body with `pub` but no `commit` is refused (`400`). `proximity` is the
     pairing-v2 object (`beacons[]`, `lat`, `lon`, `acc`, `source`); the verdict is shown to the PC user as
     information only.
   - **mDNS path, 1b:** Phone → `POST /api/v1/link/<sid>` `{"pub"}` from the same address. The PC checks the
     commitment (a mismatch denies the request: `403`), computes the code and pops a dialog / notification
     "<name> wants to link — code 123 456 [Link] [Reject]" with the proximity verdict as information →
     `202 {"sid", "status":"pending", "expires"}`.
   - At most 3 mDNS requests wait at once (`429`); 2 minutes each (from 1a); the same phone (address + name)
     asking again replaces its earlier request. Each source may make 12 link POSTs a minute (a bad MAC costs 3),
     then `429` with `Retry-After`.
2. Both sides: `shared = X25519(own_sk, other_pub)`;
   `code = u32be(HKDF-SHA256(ikm=shared, salt=sid (ASCII), info="beaconfix-link-code-v1", L=4)) mod 1 000 000`,
   shown zero-padded as `123 456` on both screens.
3. Phone polls `GET /api/v1/link/<sid>` every 2 s (≤ 2 min): `{"status":"pending"}` |
   `{"status":"denied","reason"}` | `{"status":"approved","sealed":b64u(nonce(12) ‖ ChaCha20-Poly1305(k_p, nonce, payload, aad))}` with
   `k_p = HKDF-SHA256(shared, salt=sid, info="beaconfix-link-payload-v1", L=32)`, `aad = "bflink-payload\n" + sid`,
   and `payload` = JSON `{"token":<API token, scopes read,control>, "scopes":[…], "pc":{"name","id","hosts","port"},
   "hub":"bfs3:…"|null}`. Delivered once; the session is then closed (`404` after). The token never crosses the LAN
   in clear. "pending" can follow an "approved" POST answer for a moment while the PC fetches a hub invite (mDNS
   path: after Link, ≤ 8 s). An approved payload nobody fetches within 2 minutes is dropped with its token.
4. Phone: stores the PC (token) exactly as a paired desktop today; if `hub` is set, enrols with the hub using
   that invite — **no fingerprint prompt** (the invite arrived over the authenticated link and carries the hub's
   key) — showing progress; if the hub is unreachable, it keeps the invite and retries in the background (and tells
   the user "turn on WireGuard to finish"); an expired invite is re-requested from the PC with
   `POST /api/v1/link/hub-invite` (token-authenticated, control scope) → `{"hub":"bfs3:…"}`; `409` when the PC is not
   enrolled with a hub, `503` when the PC cannot reach it.
5. Both screens end on "Linked ✓ <other name> · code 123 456". The PC lists the phone under Devices (kind, name,
   read + control) and adds it to its known devices (by MAC on the same subnet, else by address), so "only known
   devices may use tokens" lets it in. Unlinking = revoking that token under Devices.

## The PC getting a hub invite

`POST /api/v3/hub/invites {"name", "kind"}` over BFS3 — allowed for an enrolled device whose kind is `desktop`,
`laptop` or `node` and that holds the `control` scope (a PC enrols as `desktop` or `laptop`, by its battery); the
hub answers `{"invite":"bfs3:…","expires"}` (single use, 15 min, same format as the admin CLI's; scopes = the
inviter's own, never more; at most 10 open invites per inviter, then `429`). The hub logs who invited whom
(journal, and `by` in `beaconfix --server --status`).

## Security notes

- QR path: possession of `k` (only on the PC's screen) authenticates the phone's request; the code is assurance.
- mDNS path: the numeric comparison is the authentication (MITM on the LAN would show different codes, and the
  commitment stops it from grinding a match); the PC user must tap Link. A wrong tap is recoverable (revoke under
  Devices).
- Every secret (API token, hub invite) is sealed to the session key; the LAN sees only public keys, hashes and MACs.
- Sessions are single use and expire; a closed or renewed dialog cancels its QR; the hub invite is single use and
  15 min. The QR itself carries a hub invite: treat the screen as you would the invite.
