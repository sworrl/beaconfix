#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BeaconFix ESP32 Auto-Link Daemon.

Monitors connected USB serial ports for ESP32 nodes (CP2102, CH340, FTDI).
When an ESP32 is plugged in:
 1. Auto-connects and verifies firmware status.
 2. Auto-links the node into BeaconFix (registers as kind 'esp32-node' anchor).
 3. Relays live monitor mode telemetry (probes, beacons, trackers, battery)
    over local network UDP broadcast (port 47824) so mobile phones and laptops
    auto-link with zero manual configuration.
 4. Multi-device support: tracks all plugged-in ESP32s concurrently.
 5. Auto-flashes fresh/unflashed boards if --auto-flash is enabled.
"""

import argparse
import glob
import hashlib
import json
import os
import socket
import subprocess
import sys
import threading
import shutil
import time
import urllib.request
from typing import Dict, Set

try:
    import serial
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    sys.exit(1)

DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARDUINO_CLI = shutil.which("arduino-cli") or "/home/user/.local/bin/arduino-cli"
UDP_PORT = 47824
BEACONFIX_API = "http://127.0.0.1:47822"
WIREGUARD_HUB_URL = "https://hub.example.com"
QUEUE_FILE = os.path.expanduser("~/.config/beaconfix/autolink_queue.jsonl")
QUEUE_LOCK = threading.Lock()


def store_and_forward_enqueue(item: dict):
    """Enqueue telemetry / alerts to disk for reliable store-and-forward delivery."""
    try:
        os.makedirs(os.path.dirname(QUEUE_FILE), exist_ok=True)
        with QUEUE_LOCK:
            with open(QUEUE_FILE, "a") as f:
                f.write(json.dumps(item) + "\n")
    except Exception:
        pass


def trigger_hub_sync():
    """Trigger immediate sync with the master database on the hub (192.0.2.1) over WireGuard."""
    def _sync():
        try:
            res = subprocess.run(["beaconfix", "--hub-sync"], capture_output=True, text=True, timeout=12)
            if "Synced with the hub" in res.stdout:
                print(f"[✓] WireGuard Uplink: {res.stdout.strip()}", file=sys.stderr)
        except Exception:
            pass
    threading.Thread(target=_sync, daemon=True).start()


def queue_worker():
    """Background thread to drain and forward offline queued records to WireGuard hub."""
    while True:
        time.sleep(25)
        if not os.path.exists(QUEUE_FILE):
            continue
        try:
            req = urllib.request.Request(f"{WIREGUARD_HUB_URL}/healthz")
            with urllib.request.urlopen(req, timeout=3.0) as r:
                if r.status == 200:
                    trigger_hub_sync()
                    with QUEUE_LOCK:
                        with open(QUEUE_FILE, "w") as f:
                            f.truncate(0)
        except Exception:
            pass


def get_esp32_ports() -> Set[str]:
    """Find all plugged-in USB-UART bridges."""
    ports = set()
    by_id = glob.glob("/dev/serial/by-id/*")
    for p in by_id:
        lower = p.lower()
        if any(chip in lower for chip in ["cp210", "ch340", "ftdi", "uart", "esp32", "silicon_labs"]):
            ports.add(os.path.realpath(p))

    for p in glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"):
        ports.add(os.path.realpath(p))

    return ports


class EspDeviceWorker(threading.Thread):
    def __init__(self, port: str, auto_flash: bool = False, verbose: bool = True):
        super().__init__(daemon=True)
        self.port = port
        self.auto_flash = auto_flash
        self.verbose = verbose
        self.running = True
        self.node_name = f"ESP32-{os.path.basename(port)}"
        self.ser = None
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    def send_cmd(self, cmd: str):
        if self.ser and self.ser.is_open:
            if not cmd.endswith("\n"):
                cmd += "\n"
            try:
                self.ser.write(cmd.encode("utf-8"))
            except Exception as e:
                print(f"[!] Write error on {self.port}: {e}", file=sys.stderr)

    def register_with_beaconfix(self, node_name: str, batt_info: dict = None):
        """Register the ESP32 node as an anchor in the BeaconFix database."""
        try:
            # Query desktop state for current location if available
            req = urllib.request.Request(f"{BEACONFIX_API}/api/v1/state")
            lat, lon = 0.0, 0.0
            try:
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    state = json.loads(resp.read().decode())
                    loc = state.get("location", {})
                    lat = loc.get("lat", 0.0)
                    lon = loc.get("lon", 0.0)
            except Exception:
                pass

            anchor_payload = {
                "id": f"esp32-{node_name.lower().replace('beaconfix-node-', '')}",
                "name": node_name,
                "kind": "esp32-node",
                "lat": lat,
                "lon": lon,
                "accM": 1.0,
                "placedBy": "desktop-autolink",
                "source": "esp32-usb"
            }
            post_data = json.dumps(anchor_payload).encode("utf-8")
            post_req = urllib.request.Request(
                f"{BEACONFIX_API}/api/v1/anchors",
                data=post_data,
                headers={"Content-Type": "application/json"}
            )
            with urllib.request.urlopen(post_req, timeout=1.0) as resp:
                if self.verbose:
                    print(f"[*] Auto-linked {node_name} to BeaconFix database (HTTP {resp.status})", file=sys.stderr)

            # Register battery capacity if node is a mobile battery-powered node (0 for Base Station USB)
            try:
                is_base = (batt_info or {}).get("mode") == "base_station" or (batt_info or {}).get("is_base_station", False)
                has_batt = (batt_info or {}).get("has_battery", not is_base)
                mah = 240 if (has_batt and not is_base) else 0
                batt_req = urllib.request.Request(
                    f"{BEACONFIX_API}/api/v1/nodes/battery",
                    data=json.dumps({"name": node_name, "mah": mah}).encode("utf-8"),
                    headers={"Content-Type": "application/json"}
                )
                with urllib.request.urlopen(batt_req, timeout=1.0) as resp:
                    pass
            except Exception:
                pass
            
            # Auto-generate 3D-printable slide-and-clip nameplate STL for newly minted node (NAMEPLATE_SPEC.md)
            try:
                sys.path.insert(0, DIR)
                from tools.nameplate_generator import generate_nameplate_for_node
                stl_path = generate_nameplate_for_node(node_name)
                if self.verbose and stl_path:
                    print(f"[*] 3D Nameplate minted for {node_name}: {stl_path}", file=sys.stderr)
            except Exception as ne:
                if self.verbose:
                    print(f"[!] Nameplate generation warning: {ne}", file=sys.stderr)
        except Exception as e:
            # BeaconFix desktop API not running or unreachable, continues running
            pass

    def prime_node_manifest(self, hw_type: int = 1):
        """Send the latest signed firmware manifest to the connected node for autonomous mesh seeding."""
        try:
            if hw_type == 1:
                bin_path = os.path.join(DIR, "firmware/heltec_v3/build/heltec_v3.ino.bin")
            else:
                bin_path = os.path.join(DIR, "firmware/esp32_node/build/esp32_node.ino.bin")

            sig_file = bin_path + ".sig"
            if os.path.exists(bin_path) and os.path.exists(sig_file):
                size = os.path.getsize(bin_path)
                with open(bin_path, "rb") as f:
                    sha_hex = hashlib.sha256(f.read()).hexdigest()
                with open(sig_file, "rb") as f:
                    sig_hex = f.read().hex()
                cmd = f"mesh ota manifest {size} 192 {sha_hex} {sig_hex} 3.10.2 {hw_type}\n"
                self.ser.write(cmd.encode())
                time.sleep(0.05)
                if self.verbose:
                    print(f"[*] Primed {self.node_name} with firmware v3.10.2 manifest for HW {hw_type} ({size:,} bytes)", file=sys.stderr)
        except Exception as e:
            pass

    def handle_alert(self, data: dict, origin: str):
        store_and_forward_enqueue({"type": "alert", "origin": origin, "data": data, "ts": time.time()})
        if any(k in data for k in ["alpr", "camera_pass", "operator", "plate", "plate_event"]):
            try:
                lat = float(data.get("lat", 0.0))
                lon = float(data.get("lon", 0.0))
                if lat == 0.0 and lon == 0.0:
                    try:
                        req_st = urllib.request.Request(f"{BEACONFIX_API}/api/v1/state")
                        with urllib.request.urlopen(req_st, timeout=1.0) as resp:
                            st = json.loads(resp.read().decode())
                            loc = st.get("location", {})
                            lat = float(loc.get("lat", 0.0))
                            lon = float(loc.get("lon", 0.0))
                    except Exception:
                        pass

                camera_id = data.get("camera_id") or data.get("cam_id") or data.get("id") or f"rf-{origin.lower()}-{int(time.time())}"
                mfg = data.get("manufacturer") or ""
                op = data.get("operator") or mfg or "Flock Safety"
                mdl = data.get("model") or ("Falcon" if "flock" in op.lower() else "ALPR")
                ev = {
                    "kind": "camera_pass",
                    "camera_id": camera_id,
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "operator": op,
                    "model": mdl,
                    "distanceM": float(data.get("distance_m", data.get("dist", 45.0))),
                    "lat": lat,
                    "lon": lon,
                    "confidence": int(data.get("conf", 95)),
                    "source": f"mesh-{origin.lower()}"
                }
                body = json.dumps({"device": origin, "events": [ev]}).encode()
                req = urllib.request.Request(f"{BEACONFIX_API}/api/v1/plate-events", data=body,
                                             headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=1.5) as r:
                    if self.verbose:
                        print(f"[*] Ingested ALPR alert from {origin} to database (HTTP {r.status})", file=sys.stderr)
            except Exception:
                pass
        trigger_hub_sync()

    def handle_tracker(self, data: dict, origin: str):
        store_and_forward_enqueue({"type": "tracker", "origin": origin, "data": data, "ts": time.time()})
        try:
            obs = {
                "mac": data.get("mac", ""),
                "kind": data.get("kind", "tracker"),
                "rssi": int(data.get("rssi", -70)),
                "source": f"mesh-{origin.lower()}",
                "time": time.time()
            }
            body = json.dumps({"device": origin, "observations": [obs]}).encode()
            req = urllib.request.Request(f"{BEACONFIX_API}/api/v1/db/observations", data=body,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=1.5) as r:
                pass
        except Exception:
            pass

    def push_host_telemetry(self):
        """Push host GPS location and trip odometer to attached ESP32."""
        try:
            req_loc = urllib.request.Request(f"{BEACONFIX_API}/api/v1/location")
            with urllib.request.urlopen(req_loc, timeout=0.6) as resp:
                loc = json.loads(resp.read().decode())
                lat = float(loc.get("lat", 0.0))
                lon = float(loc.get("lon", 0.0))
                acc = float(loc.get("accuracy", 1.0))
                elev = float(loc.get("elevation", 0.0))

            req_trip = urllib.request.Request(f"{BEACONFIX_API}/api/v1/trip")
            trip_km = 0.0
            speed_kmh = 0.0
            hdg = -1.0
            moving = False
            try:
                with urllib.request.urlopen(req_trip, timeout=0.6) as resp:
                    t_data = json.loads(resp.read().decode())
                    trip = t_data.get("trip", {})
                    trip_km = float(trip.get("distanceTripKm", trip.get("distanceTodayKm", 0.0)))
                    moving = bool(trip.get("moving", False))
                    raw_spd = float(trip.get("speedKmh", -1.0))
                    speed_kmh = raw_spd if raw_spd >= 0.0 else 0.0
                    hdg = float(trip.get("headingDeg", -1.0))
            except Exception:
                pass

            if self.ser and self.ser.is_open and (lat != 0.0 or lon != 0.0):
                speed_mps = speed_kmh / 3.6
                gps_cmd = f"gps {lat:.6f},{lon:.6f},{acc:.1f},{speed_mps:.2f},{hdg:.1f},{elev:.1f},0,{trip_km:.2f}\n"
                self.ser.write(gps_cmd.encode())
                travel_cmd = f"travel {'moving' if moving else 'stationary'} {speed_kmh:.1f} {hdg:.1f} {elev:.1f} {trip_km:.2f}\n"
                self.ser.write(travel_cmd.encode())
                self.ser.write(f"trip {trip_km:.2f}\n".encode())
        except Exception:
            pass

    def run(self):
        print(f"[*] Worker started for ESP32 on {self.port}", file=sys.stderr)
        try:
            self.ser = serial.Serial(self.port, 115200, timeout=1.0)
            # Pulse EN/RTS to bring chip cleanly out of any bootloader download mode
            self.ser.dtr = False
            self.ser.rts = True
            time.sleep(0.05)
            self.ser.rts = False
            time.sleep(0.2)
        except Exception as e:
            print(f"[!] Could not open {self.port}: {e}", file=sys.stderr)
            return

        time.sleep(0.5)
        # Send initial sub-ms epoch time synchronization (desktop clock -> ESP32 master)
        now_us = int(time.time() * 1000000)
        self.ser.write(f"time {now_us}\n".encode())
        time.sleep(0.05)
        self.ser.write(b"attach Desktop\n")
        time.sleep(0.05)
        self.ser.write(b"channel 0\n")
        time.sleep(0.05)
        self.ser.write(b"status\n")
        time.sleep(0.05)
        self.prime_node_manifest()
        self.push_host_telemetry()

        registered = False
        last_time_sync = time.time()
        last_host_sync = time.time()
        registered_origins = set()
        first_line_received = False
        connect_time = time.time()

        while self.running and os.path.exists(self.port):
            try:
                now = time.time()
                # Periodic sub-ms time synchronization (every 25 seconds)
                if now - last_time_sync > 25.0:
                    last_time_sync = now
                    try:
                        self.ser.write(f"time {int(now * 1000000)}\n".encode())
                    except Exception:
                        pass

                # Periodic GPS fix & trip odometer streaming from desktop (every 3 seconds)
                if now - last_host_sync > 3.0:
                    last_host_sync = now
                    self.push_host_telemetry()

                raw = self.ser.readline()
                if not raw:
                    # If auto-flash is explicitly enabled and board is uncommunicative:
                    if self.auto_flash and not first_line_received and (time.time() - connect_time > 10.0):
                        print(f"[*] Board on {self.port} uncommunicative. Attempting flash...", file=sys.stderr)
                        self.ser.close()
                        self.ser = None
                        flash_cmd = [
                            ARDUINO_CLI, "upload", "-p", self.port,
                            "--fqbn", "esp32:esp32:esp32s3",
                            "--input-dir", os.path.join(DIR, "firmware/heltec_v3/build")
                        ]
                        subprocess.run(flash_cmd, check=False)
                        time.sleep(1.5)
                        self.ser = serial.Serial(self.port, 115200, timeout=1.0)
                        self.ser.write(f"time {int(time.time() * 1000000)}\n".encode())
                        time.sleep(0.1)
                        self.ser.write(b"channel 0\nstatus\n")
                        connect_time = time.time()
                    continue

                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("{"):
                    if line:
                        print(f"[*] [{self.node_name}] {line}", file=sys.stderr)
                    # Check for flash read error or bootloader prompts indicating unflashed chip
                    if self.auto_flash and ("flash read err" in line or "rst:0x10" in line):
                        print(f"[*] Detected blank chip bootloader on {self.port}: {line}", file=sys.stderr)
                        self.ser.close()
                        self.ser = None
                        flash_cmd = [
                            ARDUINO_CLI, "upload", "-p", self.port,
                            "--fqbn", "esp32:esp32:esp32s3",
                            "--input-dir", os.path.join(DIR, "firmware/heltec_v3/build")
                        ]
                        subprocess.run(flash_cmd, check=False)
                        time.sleep(1.5)
                        self.ser = serial.Serial(self.port, 115200, timeout=1.0)
                        self.ser.write(f"time {int(time.time() * 1000000)}\n".encode())
                        time.sleep(0.1)
                        self.ser.write(b"channel 0\nstatus\n")
                        connect_time = time.time()
                    continue

                first_line_received = True

                # Parse JSON
                try:
                    data = json.loads(line)
                    t = data.get("type")
                    if t == "error":
                        print(f"[!] ESP ERROR from {self.node_name}: {line}", file=sys.stderr)
                    if t == "status":
                        self.node_name = data.get("node", self.node_name)
                        data["is_usb"] = True
                        data["usb_port"] = self.port
                        data["transport"] = "usb_mesh_dual"
                        hw = data.get("hardware", "")
                        hw_type = 1 if ("Heltec" in hw or "S3" in hw) else 2
                        if not registered:
                            self.register_with_beaconfix(self.node_name, data)
                            self.prime_node_manifest(hw_type)
                            registered = True
                    elif t == "alert":
                        print(f"[!] ALERT from {self.node_name}: {line}", file=sys.stderr)
                        self.handle_alert(data, self.node_name)
                    elif t == "ble_tracker":
                        print(f"[*] TRACKER spotted by {self.node_name}: {data.get('kind')} {data.get('mac')}", file=sys.stderr)
                        self.handle_tracker(data, self.node_name)
                    elif t == "mesh_telemetry":
                        origin = data.get("origin", "Unknown")
                        origin_mac = data.get("mac", "")
                        hops = data.get("hops", 0)
                        ts_us = data.get("ts_us", 0)
                        inner = data.get("data", {})
                        inner_type = inner.get("type", "data")
                        print(f"[*] MESH RELAY: {origin} ({origin_mac}, {hops} hops) -> {self.node_name} | {inner_type}", file=sys.stderr)
                        if origin not in registered_origins:
                            self.register_with_beaconfix(origin, inner)
                            registered_origins.add(origin)
                        if inner_type == "alert":
                            self.handle_alert(inner, origin)
                        elif inner_type == "ble_tracker":
                            self.handle_tracker(inner, origin)
                    elif t == "mesh_ota_remote_status":
                        mac = data.get("mac", "")
                        node = data.get("node", "RemoteNode")
                        st = data.get("state", "")
                        pct = data.get("pct", 0)
                        chunk = data.get("next_chunk", 0)
                        tot = data.get("total", 0)
                        print(f"[*] MESH OTA PROGRESS: {node} ({mac}) - {pct}% (chunk {chunk}/{tot}) [{st}]", file=sys.stderr)
                    elif t == "mesh_ota_seeder":
                        st = data.get("status", "")
                        target = data.get("target", "")
                        chunks = data.get("chunks", 0)
                        node = data.get("node", "")
                        print(f"[*] MESH OTA SEEDER: {st} -> {node or target} ({chunks} chunks)", file=sys.stderr)
                    elif t == "mesh_ota":
                        st = data.get("status", "")
                        ver = data.get("version", "")
                        act = data.get("action", "")
                        print(f"[*] MESH OTA: {st} {ver} {act}".strip(), file=sys.stderr)

                    # Broadcast enriched JSON to local network UDP
                    try:
                        out_bytes = (json.dumps(data) + "\n").encode("utf-8")
                        self.udp_sock.sendto(out_bytes, ("255.255.255.255", UDP_PORT))
                    except Exception:
                        pass
                except Exception:
                    pass

            except Exception as e:
                print(f"[!] Error on {self.port}: {e}", file=sys.stderr)
                break

        print(f"[*] ESP32 on {self.port} disconnected.", file=sys.stderr)
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.udp_sock.close()


def autolink_loop(auto_flash=False):
    print(f"[*] BeaconFix ESP32 Auto-Link Daemon running...", file=sys.stderr)
    print(f"[*] Relaying UDP broadcasts on port {UDP_PORT}", file=sys.stderr)
    print(f"[*] WireGuard Uplink active: {WIREGUARD_HUB_URL} via wg0", file=sys.stderr)
    active_workers: Dict[str, EspDeviceWorker] = {}

    # Background store-and-forward queue flusher to WireGuard hub
    threading.Thread(target=queue_worker, daemon=True).start()

    # Local command dispatcher thread (UDP 47825)
    def cmd_listener():
        cmd_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cmd_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            cmd_sock.bind(("0.0.0.0", 47825))
            while True:
                data, _ = cmd_sock.recvfrom(1024)
                cmd = data.decode("utf-8", errors="replace").strip()
                if cmd:
                    for w in list(active_workers.values()):
                        w.send_cmd(cmd)
        except Exception as e:
            print(f"[!] Command listener error: {e}", file=sys.stderr)

    threading.Thread(target=cmd_listener, daemon=True).start()

    while True:
        try:
            current_ports = get_esp32_ports()

            # Clean up dead workers
            for p in list(active_workers.keys()):
                if not active_workers[p].is_alive():
                    del active_workers[p]

            # Start new workers for newly plugged-in ESPs
            for p in current_ports:
                if p not in active_workers:
                    print(f"[+] Detected newly plugged-in ESP on {p}", file=sys.stderr)
                    if auto_flash:
                        print(f"[*] Auto-flashing {p} before connecting...", file=sys.stderr)
                        subprocess.run([os.path.join(DIR, "tools/flash_esp32.sh"), p], check=False)
                        time.sleep(1)

                    worker = EspDeviceWorker(p, auto_flash=auto_flash)
                    worker.start()
                    active_workers[p] = worker

            time.sleep(2)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"[!] Daemon error: {e}", file=sys.stderr)
            time.sleep(3)


def main():
    p = argparse.ArgumentParser(description="BeaconFix ESP32 Auto-Link Daemon")
    p.add_argument("--auto-flash", action="store_true", help="Automatically compile and flash any freshly plugged-in ESP32")
    args = p.parse_args()
    autolink_loop(auto_flash=args.auto_flash)


if __name__ == "__main__":
    main()
