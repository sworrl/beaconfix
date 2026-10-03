#!/usr/bin/env python3
"""Linking v3 reference (docs/LINKING.md, revision 2: the mDNS path commits to the phone's key first) and vectors:
    python3 tools/link_ref.py > tests/fixtures/link_vectors.json"""
import base64, hashlib, hmac, json, struct
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization


def b64u(b): return base64.urlsafe_b64encode(b).rstrip(b"=").decode()
def pub(sk): return sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def hkdf(ikm, salt: bytes, info: bytes, n): return HKDF(algorithm=hashes.SHA256(), length=n, salt=salt, info=info).derive(ikm)


def qr_mac(k, sid, name, kind, pub_b64u):
    return hmac.new(k, f"bflink\n{sid}\n{name}\n{kind}\n{pub_b64u}".encode(), hashlib.sha256).hexdigest()


def commit(name, kind, pub_b64u):
    """mDNS path: the phone's commitment to its key, sent BEFORE it sees the PC's (docs/LINKING.md "Why the commitment")."""
    return hashlib.sha256(f"bflink-commit\n{name}\n{kind}\n{pub_b64u}".encode()).hexdigest()


def code(shared, sid):
    v = struct.unpack(">I", hkdf(shared, sid.encode(), b"beaconfix-link-code-v1", 4))[0] % 1000000
    return f"{v:06d}"


def seal_payload(shared, sid, nonce, payload: bytes):
    k = hkdf(shared, sid.encode(), b"beaconfix-link-payload-v1", 32)
    return b64u(nonce + ChaCha20Poly1305(k).encrypt(nonce, payload, f"bflink-payload\n{sid}".encode()))


def main():
    pc = X25519PrivateKey.from_private_bytes(bytes(range(51, 83)))
    ph = X25519PrivateKey.from_private_bytes(bytes(range(151, 183)))
    shared = ph.exchange(X25519PublicKey.from_public_bytes(pub(pc)))
    assert shared == pc.exchange(X25519PublicKey.from_public_bytes(pub(ph)))
    sid, k = "a1b2c3d4e5f60718", bytes(range(7, 39))
    qr = "bflink:" + b64u(json.dumps({"v": 1, "sid": sid, "name": "desk-pc", "hosts": ["192.0.2.202", "desk-pc.local"], "port": 47897,
                                      "pub": b64u(pub(pc)), "k": b64u(k), "e": 1790900600, "hub": "bfs3:example"}, separators=(",", ":")).encode())
    payload = json.dumps({"token": "tok_example", "scopes": ["read", "control"], "pc": {"name": "desk-pc", "id": "", "hosts": ["192.0.2.202"], "port": 47897},
                          "hub": "bfs3:example"}, separators=(",", ":")).encode()
    nonce = hashlib.sha256(b"link-nonce").digest()[:12]
    print(json.dumps({
        "spec": "docs/LINKING.md", "pc_sk": b64u(bytes(range(51, 83))), "pc_pub": b64u(pub(pc)), "phone_sk": b64u(bytes(range(151, 183))),
        "phone_pub": b64u(pub(ph)), "shared": b64u(shared), "sid": sid, "k": b64u(k), "qr": qr,
        "mac": qr_mac(k, sid, "Pixel 10 Pro XL", "android", b64u(pub(ph))), "mac_name": "Pixel 10 Pro XL", "mac_kind": "android",
        "commit": commit("Pixel 10 Pro XL", "android", b64u(pub(ph))),
        "code": code(shared, sid), "payload": payload.decode(), "nonce": b64u(nonce), "sealed": seal_payload(shared, sid, nonce, payload),
    }, indent=2))


if __name__ == "__main__":
    main()
