# Identity

One identity across every BeaconFix you run: the desktop in the motorhome, the laptop, the
phone. It is an **Ed25519 key pair plus a display name**, minted in any of the apps and moved to
the others as an encrypted bundle (QR code, text, file, or a one-time code on the LAN). Two
identities created independently can be **linked** later; a link statement signed by both sides
makes them one owner set, so data collected under either belongs to both. Devices holding the
identity (or a linked one) sign in to the LAN API with a challenge signature — no pairing codes.

The first part of this page is the shared specification that the desktop and the Android app
implement byte for byte. The second part is how the desktop exposes it.

## Shared specification (v1)

Identity = an Ed25519 key pair plus a display name. It is minted in ANY app (desktop, Android,
laptop) and moved to the others cryptographically; two identities created independently can be
LINKED later (merged) in any direction.

- **id**: base32 (Crockford, lower-case, no padding) of the first 16 bytes of SHA-256(pub),
  e.g. `7k3m…` (26 chars); shown with 4-char groups.
- **keys**: Ed25519 (RFC 8032). pub = 32 bytes, seed = 32 bytes. Private material never leaves a
  device unencrypted.
- **record** (JSON):
  `{"v":1,"id":<id>,"name":<utf8 ≤ 64>,"created":<ISO-8601 UTC>,"pub":<b64>,"devices":[{"name","kind":"desktop|android|laptop","added":<ISO>,"pub"?}],"links":[<link statement>…]}`
- **link statement** (merge of identities A and B, either direction):
  `{"v":1,"a":<idA>,"b":<idB>,"ts":<ISO>,"sigA":<b64 sig by A over canon>,"sigB":<b64 sig by B over canon>}`
  canon = `"beaconfix-link|v1|" + min(idA,idB) + "|" + max(idA,idB) + "|" + ts`. A link is valid
  only with BOTH signatures. Linked identities form one owner set; data rows keep their original
  identity id, ownership = "self or linked". Links propagate by sync (idempotent).
- **export bundle** (what QR / file / paste carries):
  `{"v":1,"t":"beaconfix-identity","kdf":{"name":"scrypt","n":32768,"r":8,"p":1,"salt":<b64 16B>},"aead":{"name":"aes-256-gcm","nonce":<b64 12B>},"ct":<b64>}`;
  ct = AES-256-GCM(key = scrypt(passphrase_utf8, salt), nonce, plaintext, aad = `"beaconfix-identity-v1"`);
  plaintext = `{"record":<record>,"seed":<b64 32B>}`. Passphrase: user-chosen (≥ 8 chars) OR an
  auto-generated 6-word code (EFF short wordlist) shown once.
  Text form: `"BFID1:" + base64url(json bundle)`. QR: the text form.
- **LAN hand-off** (no QR): the sender app calls its own API `POST /api/v1/identity/export`
  (control scope or local UI) → the server holds the bundle for 10 min under a one-time 6-digit
  code; the receiver calls `GET /api/v1/identity/export/<code>` on the sender (LAN only, code
  single-use, 5 tries) → bundle → asks the passphrase.
- **challenge auth** (replaces/augments pairing tokens): client `GET /api/v1/identity/challenge`
  → `{nonce (b64 32B), host, expires (60 s)}`; client `POST /api/v1/identity/auth
  {"id","pub":<b64>,"device":{"name","kind"},"nonce","sig":<b64>}` with sig = Ed25519 over
  `"beaconfix-auth|v1|" + host + "|" + nonce + "|" + id + "|" + device.name`. The server accepts if
  id == own id OR id ∈ linked set → issues a bearer token (scope control, device recorded, no
  pairing prompt) → `{"token","scopes","identity":{id,name}}`. Unknown identity → 403
  `{"error":"unknown identity"}` and the request is listed as pending so the user can LINK it from
  the UI (linking then requires the other side to co-sign: the pending entry carries its pub; the
  UI offers "Link this identity" which needs that device to confirm with its own signature via a
  second call `POST /api/v1/identity/link {statement}`).
- **endpoints**: `GET /api/v1/identity` (public: id, name, pub, devices, links, linkedIds);
  `POST /api/v1/identity/link {statement}` (either side; validated); export / challenge / auth
  as above. `hello.features` gains `"identity"`.
- **storage**: desktop `~/.config/sworrl/identity.json` (record + seed encrypted with the map-DB
  key via AES-256-GCM, 0600); Android: Room + EncryptedSharedPreferences (seed in Android
  Keystore-wrapped prefs). Pending link requests persisted.
- **sync**: `/db/sync` and `/db/changes` rows gain `"identity":<id>`; sync from a peer whose
  identity is not self/linked is refused (403) unless the peer holds a pairing token (legacy
  path). Merging two data sets after a late link needs no rewrite — ownership is evaluated through
  the link set.
- **first run / onboarding**: if no identity: "Create a new identity" (name → key pair) or
  "Import" (scan QR / paste BFID1 text / open file / from another BeaconFix on this network: enter
  the 6-digit code shown there) — in ANY app. Settings → Identity: show id (grouped), name,
  devices, links; Export (QR + text + file, passphrase or 6-word code); "Link with another
  identity" (linking needs no secrets: A shows a link QR, B scans, both sign); "Forget this
  device".
- **test vectors**: a fixed seed test (seed = 32 × 0x01) → the pub and an auth signature over a
  fixed string, printed by the desktop `--identity-selftest`, asserted by the Android unit tests.

### Implementation notes (both apps follow these)

- **Link flow, made precise.** A's *link payload* carries no signature, because the canonical
  string needs both ids: `BFLNK1:` + base64url of `{"v":1,"t":"beaconfix-link","id","name","pub","ts"}`.
  B builds the statement (`a`/`b` in either order), signs its own side and sends it — either by
  `POST /api/v1/identity/link` to A's API, or as a `BFLINK1:` + base64url(statement JSON) text /
  QR that A pastes. A statement may carry `pubA` / `pubB` while it is being completed so the
  receiver can verify the other side before signing. A stores a link only when both signatures
  verify against public keys whose ids match; a half-signed statement from a stranger is refused.
- **Statement responses.** `POST /identity/link` returns `{"statement": <completed statement with
  pubs>, "linkedIds": […]}`. The completed statement is what the other device stores; sync carries
  it too, so a link made on one device reaches the rest.
- **Sealed storage on the desktop.** `identity.json` = `{"v":1,"public":<record>,"sealed":<b64
  nonce‖ct‖tag>,"pending":[…]}`; the sealed part is AES-256-GCM with the map-database key and the
  AAD `beaconfix-identity-store-v1`. The public record is readable without the key (so `--identity`
  works everywhere); signing needs the key.
- **Tokens issued through identity auth** carry the identity id in the devices file; one live
  token per (identity, device name) — a new sign-in from the same device name revokes the old one.
- **Test vectors** (seed = 32 × `0x01`), from `beaconfix --identity-selftest`:

  | field | value |
  |---|---|
  | pub | `iojj3XQJ8ZX9UtstPLpdcspnCb8dlBIb83SIAbQPb1w=` |
  | id | `6htgz65xb7yfs53dmhdanfmk7c` |
  | sig over `beaconfix-selftest\|v1` | `1D2T5r6f0mdU+A4q1szoSUDjZ66v8SKbDW+Rhkg4HNwGeeBym6VLKNpJb9CEGEAW9A9ImesmCGwpdHgcSYzLAA==` |
  | auth canon example | `beaconfix-auth\|v1\|host\|AAAA\|6htgz65xb7yfs53dmhdanfmk7c\|dev` |
  | sig over that | `CdR41OtXMkB+lgWnqgQms5XKCfG4LzBpp7hTKhlLeV7PNxaNHEpvF371qIWedikSy1sGatIyXoYWZNjWTGW8BQ==` |

  These were cross-checked against an independent Ed25519 implementation.

## On the desktop

**Where it lives.** Settings window → **Identity** tab; tray menu → *Identity…*; the LAN API;
and the CLI. If the tray starts without an identity it shows one notification ("Create or import
your BeaconFix identity") and otherwise creates nothing on its own.

**CLI**

| command | what it does |
|---|---|
| `beaconfix --identity` | id (grouped), name, key state, devices, links, pending link requests, file |
| `beaconfix --identity-new "<name>"` | create (refuses if one exists; forget it from the app first) |
| `beaconfix --identity-export [--words] [--passphrase <p>] [--file <f>]` | encrypted `BFID1:` bundle as text, a QR in the terminal (`qrencode`), optionally a file. `--words` makes a 6-word code and prints it once |
| `beaconfix --identity-import <file-or-BFID1-text> [--passphrase <p>]` | import (asks for the passphrase / word code otherwise) |
| `beaconfix --identity-link-qr` | our `BFLNK1:` link payload as text + QR |
| `beaconfix --identity-selftest` | the fixed-seed vectors above |

The CLI works on the file directly and tells a running tray to reload. Before the tray has ever
run, `--identity-new` creates the map-database key file too, so the identity can be sealed.

**Moving it to another device.** Identity tab → *Export*. Choose a 6-word code (default) or a
passphrase, and whether to also hold the bundle for the LAN. You get a QR to scan, the text to
paste, a *Save to file…* button, and — if held — a 6-digit code another BeaconFix on the network
can use under Import → *From the LAN* (`GET /api/v1/identity/export/<code>` on this machine,
single-use, gone after 10 minutes or 5 wrong guesses).

**Linking.** Identity tab → *Link with another identity…*: paste the other device's link payload
(`BFLNK1:…`, shown as its link QR) to produce our half-signed statement for it to co-sign, or paste
a statement it signed (`BFLINK1:…` or JSON) to complete the link. When a device signs in with an
identity that is not yours, the Devices tab lists it under *Identity sign-ins to link* with a
**Link this identity** button that produces the same half-signed statement.

**Forget this device** removes `identity.json`. Other devices keep their copies.

**API** (see [API.md](API.md) for the full table): `GET /api/v1/identity`,
`GET /api/v1/identity/challenge`, `POST /api/v1/identity/auth`, `POST /api/v1/identity/link`,
`POST /api/v1/identity/export` (control), `GET /api/v1/identity/export/<code>`. `hello.features`
lists `identity`. `GET /api/v1/db/changes` and `POST /api/v1/db/sync` carry `identity`.

**D-Bus** (`org.sworrl.BeaconFix`): `IdentityJson()`, `IdentityCreate(name)`,
`IdentityExport(passphrase)`, `IdentityImport(textOrPath, passphrase)`, `IdentityLinkPayload()`,
`IdentityAcceptLink(statementJson)`, `IdentityForget()`, `IdentityReload()`.

**Files**: `~/.config/sworrl/identity.json` (0600). The seed is only ever usable with the
map-database key (`~/.config/sworrl/beaconfix.key` or KWallet) — back both up together.
