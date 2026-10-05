#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BeaconFix Mesh Over-The-Air (OTA) Firmware Flashing Tool.

Wirelessly streams cryptographically signed firmware binaries over the ESP-NOW
mesh network to update remote nodes without plugging them into USB.
Supports:
  - 1-to-many broadcast flashing (all nodes update simultaneously)
  - Unicast targeted flashing (specific node MAC)
  - Automatic cryptographic ECDSA signing & hash generation
  - Adaptive sliding-window pacing & drop recovery
  - Real-time progress bar with transfer rate and remote status
"""

import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("Error: pyserial is required. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)

DIR = Path(__file__).resolve().parent.parent
DEFAULT_BIN = DIR / "firmware/heltec_v3/build/heltec_v3.ino.bin"
DEFAULT_KEY = DIR / "tools/keys/firmware_sign.key"
DEFAULT_PUB = DIR / "tools/keys/firmware_pub.pem"

# Terminal ANSI colors
C_RESET = "\033[0m"
C_BOLD = "\033[1m"
C_CYAN = "\033[36m"
C_GREEN = "\033[32m"
C_YELLOW = "\033[33m"
C_RED = "\033[31m"
C_MAGENTA = "\033[35m"


def find_serial_port() -> str:
    """Auto-detect connected ESP32 / Heltec USB serial port."""
    by_id = glob.glob("/dev/serial/by-id/*")
    for p in by_id:
        lower = p.lower()
        if any(chip in lower for chip in ["cp210", "ch340", "ftdi", "uart", "esp32", "silicon_labs"]):
            return os.path.realpath(p)

    candidates = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if candidates:
        return candidates[0]
    return ""


def ensure_signed_firmware(bin_path: Path, key_path: Path) -> Path:
    """Verify or create ECDSA cryptographic signature for firmware binary."""
    sig_path = Path(str(bin_path) + ".sig")
    if not bin_path.exists():
        raise FileNotFoundError(f"Firmware binary not found: {bin_path}")

    # Check if signature exists and is newer than binary
    if sig_path.exists() and sig_path.stat().st_mtime >= bin_path.stat().st_mtime:
        return sig_path

    print(f"[*] Generating cryptographic signature for {bin_path.name}...")
    sys.path.insert(0, str(DIR))
    from tools.sign_firmware import sign_firmware
    sign_firmware(str(bin_path), str(key_path), str(sig_path))
    return sig_path


def render_progress_bar(pct: float, current: int, total: int, speed_kbps: float, state: str, elapsed: float):
    width = 36
    filled = int(width * pct / 100.0)
    bar = "█" * filled + "░" * (width - filled)
    eta = ((total - current) / (current / elapsed)) if current > 0 and elapsed > 0 else 0
    sys.stdout.write(
        f"\r{C_BOLD}{C_CYAN}[MESH FLASH]{C_RESET} [{bar}] {C_BOLD}{pct:5.1f}%{C_RESET} "
        f"({current}/{total}) | {C_GREEN}{speed_kbps:5.1f} KB/s{C_RESET} | "
        f"ETA: {int(eta)}s | {C_YELLOW}{state.upper()}{C_RESET}   "
    )
    if not sys.stdout.isatty() and (current % 350 == 0 or current == total):
        sys.stdout.write("\n")
    sys.stdout.flush()


def flash_mesh(bin_path: Path, key_path: Path, target: str, port: str,
               baud: int = 921600, channel: int = 1, chunk_size: int = 192,
               pacing: float = 0.0025, version: str = "3.10.0", hardware_type: int = 0):
    sig_path = ensure_signed_firmware(bin_path, key_path)

    if hardware_type == 0:
        if "esp32_node" in str(bin_path):
            hardware_type = 2
        elif "heltec" in str(bin_path):
            hardware_type = 1
        else:
            hardware_type = 1

    hw_names = {0: "All / Any", 1: "Heltec V3 ESP32-S3", 2: "Generic ESP32"}

    with open(bin_path, "rb") as f:
        firmware_bytes = f.read()

    with open(sig_path, "rb") as f:
        sig_bytes = f.read()

    total_bytes = len(firmware_bytes)
    sha256_hash = hashlib.sha256(firmware_bytes).digest()
    sha256_hex = sha256_hash.hex()
    sig_hex = sig_bytes.hex()
    total_chunks = (total_bytes + chunk_size - 1) // chunk_size

    print(f"\n{C_BOLD}{C_CYAN}=================================================================={C_RESET}")
    print(f"{C_BOLD}{C_CYAN}  🚀 BEACONFIX MESH OVER-THE-AIR (OTA) FLASHING SYSTEM{C_RESET}")
    print(f"{C_BOLD}{C_CYAN}=================================================================={C_RESET}")
    print(f"Firmware Binary : {bin_path} ({total_bytes:,} bytes, {total_chunks:,} chunks)")
    print(f"Hardware Target : {hw_names.get(hardware_type, str(hardware_type))} (Type ID {hardware_type})")
    print(f"SHA-256 Digest  : {sha256_hex}")
    print(f"ECDSA Signature : {len(sig_bytes)} bytes (secp256r1)")
    print(f"Target Receiver : {C_BOLD}{target.upper()}{C_RESET}")
    print(f"Transport Radio : ESP-NOW Mesh (Wi-Fi Channel {channel})")

    # Temporarily stop bridge service and autolink daemon so we have exclusive high-speed serial access
    bridge_was_active = False
    try:
        res = subprocess.run(["systemctl", "--user", "is-active", "beaconfix-esp32-bridge.service"],
                             capture_output=True, text=True, check=False)
        if res.stdout.strip() == "active":
            bridge_was_active = True
            print("[*] Temporarily pausing beaconfix-esp32-bridge for exclusive flashing bandwidth...")
            subprocess.run(["systemctl", "--user", "stop", "beaconfix-esp32-bridge.service"], check=False)
            time.sleep(0.5)
    except Exception:
        pass

    autolink_was_running = False
    try:
        out = subprocess.run(["pgrep", "-f", "tools/esp32_autolink.py"], capture_output=True, text=True, check=False)
        pids = [int(p.strip()) for p in out.stdout.split() if p.strip()]
        if pids:
            autolink_was_running = True
            print(f"[*] Temporarily pausing esp32_autolink (PID {pids}) for exclusive serial access...")
            subprocess.run(["pkill", "-TERM", "-f", "tools/esp32_autolink.py"], check=False)
            time.sleep(0.5)
    except Exception:
        pass

    ser = None
    try:
        print(f"[*] Opening coordinator serial port {port} at 115200 baud...")
        ser = serial.Serial(port, 115200, timeout=1.0)
        time.sleep(0.2)

        # Flush buffer
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        # Query version & status
        ser.write(b"version\n")
        time.sleep(0.1)
        ser.write(b"status\n")
        time.sleep(0.1)

        # Switch baud rate to high-speed for fast USB forwarding if requested
        if baud != 115200:
            print(f"[*] Elevating USB serial link to {baud} baud...")
            ser.write(f"baud {baud}\n".encode())
            time.sleep(0.08)
            ser.baudrate = baud
            time.sleep(0.1)
            ser.reset_input_buffer()

        # Prime coordinator with firmware manifest in NVS for autonomous epidemic mesh seeding
        manifest_cmd = f"mesh ota manifest {total_bytes} {chunk_size} {sha256_hex} {sig_hex} {version} {hardware_type}\n"
        ser.write(manifest_cmd.encode())
        time.sleep(0.1)

        # Send OTA Start Command
        print(f"[*] Broadcasting MESH_MSG_OTA_START across mesh...")
        start_cmd = f"mesh ota start {target} {total_bytes} {chunk_size} {sha256_hex} {sig_hex} {version} {channel} {hardware_type}\n"
        ser.write(start_cmd.encode())
        ser.flush()

        time.sleep(0.2)
        # Read acknowledgment
        deadline = time.time() + 2.0
        ack_seen = False
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if "mesh_ota_start_sent" in line:
                ack_seen = True
                print(f"[✓] Coordinator ACK: {line}")
                break

        print(f"[*] Streaming {total_chunks:,} chunks over the mesh update network...\n")

        start_time = time.time()
        chunk_idx = 0
        remote_state = "streaming"
        last_query_time = time.time()
        rx_buf = ""

        while chunk_idx < total_chunks:
            chunk_start = chunk_idx * chunk_size
            chunk_end = min(chunk_start + chunk_size, total_bytes)
            chunk_data = firmware_bytes[chunk_start:chunk_end]
            hex_payload = chunk_data.hex()

            try:
                ser.write(f"mesh ota chunk {target} {chunk_idx} {hex_payload}\n".encode())
            except (OSError, serial.SerialException):
                time.sleep(0.05)
                try:
                    ser.write(f"mesh ota chunk {target} {chunk_idx} {hex_payload}\n".encode())
                except Exception:
                    pass
            if pacing > 0:
                time.sleep(pacing)

            chunk_idx += 1

            # Non-blocking buffer read for remote status feedback
            try:
                if ser and ser.is_open and ser.in_waiting > 0:
                    raw_data = ser.read(ser.in_waiting).decode("utf-8", errors="replace")
                    rx_buf += raw_data
                    while "\n" in rx_buf:
                        resp_line, rx_buf = rx_buf.split("\n", 1)
                        resp_line = resp_line.strip()
                        if "mesh_ota_remote_status" in resp_line:
                            try:
                                st = json.loads(resp_line)
                                remote_state = st.get("state", remote_state)
                                needed_chunk = st.get("next_chunk", chunk_idx)
                                if target != "all" and needed_chunk < chunk_idx - 2 and needed_chunk > chunk_idx - 20:
                                    chunk_idx = needed_chunk
                            except Exception:
                                pass
                        elif "verified_flashed" in resp_line or "success_rebooting" in resp_line:
                            remote_state = "verified"
            except (OSError, serial.SerialException):
                pass

            # Update progress bar
            now = time.time()
            elapsed = now - start_time
            bytes_sent = min(chunk_idx * chunk_size, total_bytes)
            speed = (bytes_sent / 1024.0 / elapsed) if elapsed > 0 else 0.0
            pct = (chunk_idx / total_chunks) * 100.0
            render_progress_bar(pct, chunk_idx, total_chunks, speed, remote_state, elapsed)

        print("\n[*] All chunks transmitted! Waiting for remote cryptographic verification...")

        # Wait for verification & success
        verify_deadline = time.time() + 10.0
        success = False
        while time.time() < verify_deadline:
            if ser.in_waiting > 0:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if "mesh_ota_remote_status" in line or "mesh_ota" in line:
                    try:
                        st = json.loads(line)
                        state = st.get("state") or st.get("status")
                        if state in ("success", "verified_flashed", "verified"):
                            success = True
                            print(f"\n{C_BOLD}{C_GREEN}[✓] MESH UPDATE SUCCESSFUL!{C_RESET}")
                            print(f"    Target node verified signature and is rebooting into v{version}!")
                            break
                        elif state in ("failed", "signature_verification_rejected", "sha256_mismatch"):
                            print(f"\n{C_BOLD}{C_RED}[✗] MESH UPDATE FAILED: {line}{C_RESET}")
                            break
                    except Exception:
                        pass
            time.sleep(0.05)

        if not success:
            print(f"\n{C_YELLOW}[*] Completed streaming to mesh. Final status: {remote_state}{C_RESET}")

        # Restore baud rate
        if baud != 115200:
            ser.write(b"baud 115200\n")
            time.sleep(0.05)

    except KeyboardInterrupt:
        print("\n[!] Flashing aborted by user.")
        if ser:
            try:
                ser.write(f"mesh ota abort {target}\n".encode())
            except Exception:
                pass
    finally:
        if ser and ser.is_open:
            ser.close()
        if bridge_was_active:
            print("[*] Restoring beaconfix-esp32-bridge.service...")
            subprocess.run(["systemctl", "--user", "start", "beaconfix-esp32-bridge.service"], check=False)
            time.sleep(0.5)
        if autolink_was_running:
            print("[*] Restoring tools/esp32_autolink.py daemon in background...")
            autolink_script = DIR / "tools/esp32_autolink.py"
            subprocess.Popen([sys.executable, str(autolink_script)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
            time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description="Flash ESP32 / Heltec nodes over the wireless mesh network")
    parser.add_argument("--firmware", default=str(DEFAULT_BIN), help=f"Firmware binary path (default: {DEFAULT_BIN})")
    parser.add_argument("--key", default=str(DEFAULT_KEY), help=f"Signing private key (default: {DEFAULT_KEY})")
    parser.add_argument("--target", default="all", help="Target node MAC or 'all' (default: all)")
    parser.add_argument("--port", default=None, help="Coordinator serial port (default: auto-detect)")
    parser.add_argument("--baud", type=int, default=921600, help="Coordinator USB baud rate (default: 921600)")
    parser.add_argument("--channel", type=int, default=1, help="Wi-Fi channel (default: 1)")
    parser.add_argument("--chunk-size", type=int, default=192, help="Chunk size in bytes (default: 192)")
    parser.add_argument("--pacing", type=float, default=0.040, help="Inter-chunk delay in seconds (default: 0.040 = 40ms)")
    parser.add_argument("--version", default="3.10.0", help="Firmware version tag (default: 3.10.0)")
    parser.add_argument("--hardware-type", type=int, default=0, help="Hardware architecture type (1=Heltec S3, 2=Generic ESP32, 0=auto-detect)")
    args = parser.parse_args()

    port = args.port or find_serial_port()
    if not port:
        print("Error: No coordinator serial port detected. Connect a Heltec V3 via USB.", file=sys.stderr)
        sys.exit(1)

    flash_mesh(
        bin_path=Path(args.firmware),
        key_path=Path(args.key),
        target=args.target,
        port=port,
        baud=args.baud,
        channel=args.channel,
        chunk_size=args.chunk_size,
        pacing=args.pacing,
        version=args.version,
        hardware_type=args.hardware_type
    )


if __name__ == "__main__":
    main()
