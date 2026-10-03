#!/usr/bin/env python3
"""BFS3 reference implementation (docs/SECURE-API.md) and test-vector generator.

    python3 tools/bfs3_ref.py > tests/fixtures/bfs3_vectors.json

Deterministic: fixed keys, fixed nonces. C++ (src/securechannel.*) and Kotlin (Bfs3.kt) must
reproduce every value in the vectors. Rev. 2 (docs/SECURE-API.md): bodiless requests carry their sealed bytes in
X-BF-Seal (request_headers); the vectors are unchanged. open_* are the receiving sides.
"""
import base64, hashlib, hmac, json, struct
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization


def b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def raw_pub(sk: X25519PrivateKey) -> bytes:
    return sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def hkdf(ikm: bytes, salt: bytes, info: bytes, n: int = 32) -> bytes:
    return HKDF(algorithm=hashes.SHA512(), length=n, salt=salt if salt else None, info=info).derive(ikm)


def u64be(x: int) -> bytes:
    return struct.pack(">Q", x)


def fingerprint(s_pk: bytes) -> str:
    return hashlib.sha256(s_pk).hexdigest()[:32]


def device_id(d_pk: bytes) -> str:
    return "d" + hashlib.sha256(d_pk).hexdigest()[:24]


def root_key(shared: bytes, s_pk: bytes, d_pk: bytes) -> bytes:
    return hkdf(shared, hashlib.sha256(b"bfs3" + s_pk + d_pk).digest(), b"beaconfix.bfs3.root")


def enroll_mac(invite_secret: bytes, invite_id: str, name: str, kind: str, pub: str, ts: int) -> str:
    msg = f"bfs3-enroll\n{invite_id}\n{name}\n{kind}\n{pub}\n{ts}".encode()
    return hmac.new(invite_secret, msg, hashlib.sha512).hexdigest()


def proof(rk: bytes, did: str) -> str:
    return hmac.new(rk, f"bfs3-enrolled\n{did}".encode(), hashlib.sha512).hexdigest()


def seal_request(rk, did, method, target, c, ts, nonce, plaintext: bytes) -> bytes:
    k = hkdf(rk, b"", b"bfs3 req" + u64be(c))
    aad = f"bfs3\n{method}\n{target}\n{did}\n{c}\n{ts}".encode()
    return ChaCha20Poly1305(k).encrypt(nonce, plaintext, aad)


def seal_response(rk, did, status, c, nonce, plaintext: bytes) -> bytes:
    k = hkdf(rk, b"", b"bfs3 resp" + u64be(c))
    aad = f"bfs3-resp\n{status}\n{did}\n{c}".encode()
    return ChaCha20Poly1305(k).encrypt(nonce, plaintext, aad)


def seal_event(rk, did, c, seq, nonce, plaintext: bytes) -> str:
    k = hkdf(rk, b"", b"bfs3 evt" + u64be(c) + u64be(seq))
    aad = f"bfs3-evt\n{did}\n{c}\n{seq}".encode()
    return b64u(nonce + ChaCha20Poly1305(k).encrypt(nonce, plaintext, aad))


def unb64u(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def request_headers(did, c, ts, nonce, sealed: bytes, has_body: bool) -> dict:
    """Rev. 2: a bodiless request (GET, DELETE) sends no body and carries the sealed bytes (the 16-byte tag) in X-BF-Seal."""
    h = {"X-BF-Device": did, "X-BF-Counter": str(c), "X-BF-Time": str(ts), "X-BF-Nonce": b64u(nonce)}
    if has_body:
        h["Content-Type"] = "application/vnd.beaconfix.sealed"
    else:
        h["X-BF-Seal"] = b64u(sealed)
    return h


def open_request(rk, did, method, target, c, ts, nonce, sealed: bytes) -> bytes:   # raises InvalidTag
    k = hkdf(rk, b"", b"bfs3 req" + u64be(c))
    return ChaCha20Poly1305(k).decrypt(nonce, sealed, f"bfs3\n{method}\n{target}\n{did}\n{c}\n{ts}".encode())


def open_response(rk, did, status, c, nonce, sealed: bytes) -> bytes:              # raises InvalidTag
    k = hkdf(rk, b"", b"bfs3 resp" + u64be(c))
    return ChaCha20Poly1305(k).decrypt(nonce, sealed, f"bfs3-resp\n{status}\n{did}\n{c}".encode())


def open_event(rk, did, c, seq, line: str) -> bytes:                                # raises InvalidTag
    raw = unb64u(line)
    k = hkdf(rk, b"", b"bfs3 evt" + u64be(c) + u64be(seq))
    return ChaCha20Poly1305(k).decrypt(raw[:12], raw[12:], f"bfs3-evt\n{did}\n{c}\n{seq}".encode())


def main():
    s_sk = X25519PrivateKey.from_private_bytes(bytes(range(1, 33)))
    d_sk = X25519PrivateKey.from_private_bytes(bytes(range(101, 133)))
    s_pk, d_pk = raw_pub(s_sk), raw_pub(d_sk)
    shared = d_sk.exchange(X25519PublicKey.from_public_bytes(s_pk))
    assert shared == s_sk.exchange(X25519PublicKey.from_public_bytes(d_pk))
    did = device_id(d_pk)
    rk = root_key(shared, s_pk, d_pk)
    invite_secret = bytes(range(201, 233))
    invite_id = "0123456789abcdef"
    ts = 1790900000
    url = "https://hub.example.com"
    invite = "bfs3:" + b64u(json.dumps({"u": url, "s": b64u(s_pk), "i": invite_id, "k": b64u(invite_secret), "e": ts + 900},
                                       separators=(",", ":")).encode())
    req_body = json.dumps({"device": "Pixel", "observations": [{"bssid": "AA:BB:CC:DD:EE:FF", "dbm": -61, "lat": 40.0031, "lon": -75.0686,
                                                                 "acc": 4.0, "time": "2026-10-01T19:40:00", "rangeM": 7.3, "rangeSd": 0.9}]},
                          separators=(",", ":")).encode()
    vec = {
        "spec": "docs/SECURE-API.md",
        "server_sk": b64u(bytes(range(1, 33))), "server_pk": b64u(s_pk), "fingerprint": fingerprint(s_pk),
        "device_sk": b64u(bytes(range(101, 133))), "device_pk": b64u(d_pk), "device_id": did,
        "shared": b64u(shared), "root_key": b64u(rk),
        "enroll": {"invite": invite, "invite_id": invite_id, "invite_secret": b64u(invite_secret), "name": "Pixel 10 Pro XL", "kind": "android",
                   "ts": ts, "mac": enroll_mac(invite_secret, invite_id, "Pixel 10 Pro XL", "android", b64u(d_pk), ts), "proof": proof(rk, did)},
        "requests": [],
        "responses": [],
        "events": [],
    }
    for c, method, target, body in ((1, "GET", "/api/v3/state", b""), (2, "POST", "/api/v3/db/sync", req_body), (2**40 + 7, "GET", "/api/v3/db/changes?since=12&limit=500", b"")):
        nonce = hashlib.sha256(f"nonce-req-{c}".encode()).digest()[:12]
        ct = seal_request(rk, did, method, target, c, ts, nonce, body)
        vec["requests"].append({"counter": c, "method": method, "target": target, "ts": ts, "nonce": b64u(nonce),
                                "key": b64u(hkdf(rk, b"", b"bfs3 req" + u64be(c))),
                                "aad": f"bfs3\n{method}\n{target}\n{did}\n{c}\n{ts}", "plaintext": body.decode(), "sealed": b64u(ct)})
    for c, status, body in ((1, 200, b'{"valid":true,"lat":40.00313}'), (2, 401, b'{"error":"unauthorized"}')):
        nonce = hashlib.sha256(f"nonce-resp-{c}".encode()).digest()[:12]
        vec["responses"].append({"counter": c, "status": status, "nonce": b64u(nonce), "plaintext": body.decode(),
                                 "sealed": b64u(seal_response(rk, did, status, c, nonce, body))})
    for seq in (1, 2):
        nonce = hashlib.sha256(f"nonce-evt-{seq}".encode()).digest()[:12]
        body = json.dumps({"type": "fix", "seq": seq}, separators=(",", ":")).encode()
        vec["events"].append({"counter": 5, "seq": seq, "nonce": b64u(nonce), "plaintext": body.decode(), "line": seal_event(rk, did, 5, seq, nonce, body)})
    print(json.dumps(vec, indent=2))


if __name__ == "__main__":
    main()
