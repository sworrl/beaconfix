#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BeaconFix ESP32 Firmware Signing & Verification Tool.

Signs firmware binaries using an ECDSA (secp256r1) private key kept securely on
the host. Generates a cryptographic signature that the ESP32 verifies using
the embedded public key before committing any OTA update.
"""

import argparse
import hashlib
import os
import sys
from pathlib import Path
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.backends import default_backend


def sign_firmware(bin_path: str, key_path: str, sig_path: str):
    bin_file = Path(bin_path)
    if not bin_file.exists():
        raise FileNotFoundError(f"Firmware binary not found: {bin_path}")

    key_file = Path(key_path)
    if not key_file.exists():
        raise FileNotFoundError(f"Private signing key not found: {key_path}")

    with open(key_file, "rb") as f:
        private_key = serialization.load_pem_private_key(
            f.read(), password=None, backend=default_backend()
        )

    with open(bin_file, "rb") as f:
        data = f.read()

    digest = hashlib.sha256(data).digest()
    signature = private_key.sign(digest, ec.ECDSA(hashes.SHA256()))

    with open(sig_path, "wb") as f:
        f.write(signature)

    print(f"[+] Signed firmware: {bin_path} ({len(data)} bytes)")
    print(f"[+] SHA256: {digest.hex()}")
    print(f"[+] Signature written to: {sig_path} ({len(signature)} bytes)")
    return True


def verify_firmware(bin_path: str, pub_path: str, sig_path: str):
    with open(pub_path, "rb") as f:
        public_key = serialization.load_pem_public_key(
            f.read(), backend=default_backend()
        )

    with open(bin_path, "rb") as f:
        data = f.read()

    with open(sig_path, "rb") as f:
        signature = f.read()

    digest = hashlib.sha256(data).digest()
    try:
        public_key.verify(signature, digest, ec.ECDSA(hashes.SHA256()))
        print(f"[✓] Signature VALID! Verified with {pub_path}")
        return True
    except Exception as e:
        print(f"[✗] Signature INVALID: {e}", file=sys.stderr)
        return False


def main():
    p = argparse.ArgumentParser(description="Sign or verify ESP32 firmware binary")
    p.add_argument("binary", help="Path to firmware .bin file")
    p.add_argument("--key", default="tools/keys/firmware_sign.key", help="Private key path")
    p.add_argument("--pub", default="tools/keys/firmware_pub.pem", help="Public key path")
    p.add_argument("--sig", default=None, help="Signature file path (default <binary>.sig)")
    p.add_argument("--verify", action="store_true", help="Verify signature instead of signing")

    args = p.parse_args()
    sig_file = args.sig or (args.binary + ".sig")

    if args.verify:
        ok = verify_firmware(args.binary, args.pub, sig_file)
        sys.exit(0 if ok else 1)
    else:
        sign_firmware(args.binary, args.key, sig_file)


if __name__ == "__main__":
    main()
