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
import asyncio
import glob
import hashlib
import json
import os
import re
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
ARDUINO_CLI = shutil.which("arduino-cli") or os.path.expanduser("~/.local/bin/arduino-cli")
UDP_PORT = 47824
BEACONFIX_API = "http://127.0.0.1:47822"
# Your BeaconFix hub (docs/HUB.md), if you run one; queued alerts are pushed there when it answers
WIREGUARD_HUB_URL = os.environ.get("BEACONFIX_HUB_URL", "")
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
    """Trigger an immediate `beaconfix --hub-sync` with the hub."""
    def _sync():
        try:
            res = subprocess.run(["beaconfix", "--hub-sync"], capture_output=True, text=True, timeout=12)
            if "Synced with the hub" in res.stdout:
                print(f"[✓] WireGuard Uplink: {res.stdout.strip()}", file=sys.stderr)
        except Exception:
            pass
    threading.Thread(target=_sync, daemon=True).start()


def queue_worker():
    """Background thread: once the hub answers, sync and clear the store-and-forward queue."""
    while True:
        time.sleep(25)
        if not WIREGUARD_HUB_URL or not os.path.exists(QUEUE_FILE):
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


def handle_alert(data: dict, origin: str, verbose: bool = True):
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
                if verbose:
                    print(f"[*] Ingested ALPR alert from {origin} to database (HTTP {r.status})", file=sys.stderr)
        except Exception:
            pass
    trigger_hub_sync()

def handle_tracker(data: dict, origin: str, verbose: bool = True):
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


def firmware_version(hw_type: int) -> str:
    """The version the built binary reports, from its NodeConfig.h (was hard-coded and went stale)."""
    tree = "heltec_v3" if hw_type == 1 else "esp32_node"
    try:
        with open(os.path.join(DIR, f"firmware/{tree}/NodeConfig.h")) as f:
            m = re.search(r'#define BEACONFIX_FW_VERSION "([^"]+)"', f.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "0.0.0"


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
                cmd = f"mesh ota manifest {size} 192 {sha_hex} {sig_hex} {firmware_version(hw_type)} {hw_type}\n"
                self.ser.write(cmd.encode())
                time.sleep(0.05)
                if self.verbose:
                    print(f"[*] Primed {self.node_name} with firmware v{firmware_version(hw_type)} manifest for HW {hw_type} ({size:,} bytes)", file=sys.stderr)
        except Exception as e:
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
                        handle_alert(data, self.node_name, self.verbose)
                    elif t == "ble_tracker":
                        print(f"[*] TRACKER spotted by {self.node_name}: {data.get('kind')} {data.get('mac')}", file=sys.stderr)
                        handle_tracker(data, self.node_name, self.verbose)
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
                            handle_alert(inner, origin, self.verbose)
                        elif inner_type == "ble_tracker":
                            handle_tracker(inner, origin, self.verbose)
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


NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # we write commands here
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # node notifies telemetry here
NODE_NAME_RX = re.compile(r"^(BeaconFix-|BF-|BF_)|^[A-Z][a-z]+[A-Z][a-z]+[A-Z][a-z]+[0-9]{4}$")
BLE_MAX_LINKS = 4
BLE_LINKS: Dict[str, "BleNodeLink"] = {}   # node name -> live link, for commands from UDP 47825
# Who takes commands on the LAN, learnt from their own packets on 47824: nodes joined to the WiFi (by node name) and
# phones relaying their BLE links ("phone@<ip>"). Many WiFi networks drop broadcasts from wired to wireless clients,
# so commands go to these addresses directly.
LAN_PEERS: Dict[str, tuple] = {}            # key -> (ip, last seen)
LAN_PEER_TTL = 60.0


class BleNodeLink:
    """One BLE NUS link to a node. The node becomes a mesh gateway while we're subscribed, so this
    also carries every other node it hears over ESP-NOW (mesh_telemetry). Lines are relayed to UDP
    47824 exactly like the USB path, tagged transport "ble" so the desktop doesn't call them USB."""

    def __init__(self, address: str, name: str, verbose: bool = True):
        self.address = address
        self.node_name = name
        self.verbose = verbose
        self.buf = b""
        self.framed = False          # firmware that newline-terminates and chunks lines to the MTU
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.loop = None
        self.cmds = None

    def send_cmd(self, cmd: str):
        """Thread-safe: queue a command line for this node (written to NUS RX on the BLE loop)."""
        if self.loop and self.cmds is not None:
            self.loop.call_soon_threadsafe(self.cmds.put_nowait, cmd)

    def feed(self, data: bytes):
        if data.endswith(b"\n"):
            self.framed = True
        if not self.framed and data.startswith(b"{"):
            # Older firmware sends one notify per line, cut at MTU-3: each "{" starts a fresh line
            self.buf = b""
        self.buf += data
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            self.handle_line(line)
        if not self.framed and self.buf:
            try:
                json.loads(self.buf)
            except ValueError:
                pass
            else:
                self.handle_line(self.buf)
                self.buf = b""
        if len(self.buf) > 8192:
            self.buf = b""

    def handle_line(self, raw: bytes):
        line = raw.decode("utf-8", errors="replace").strip()
        if not line.startswith("{"):
            return
        try:
            data = json.loads(line)
        except ValueError:
            return
        t = data.get("type")
        if t == "status":
            if data.get("node") and data["node"] != self.node_name:
                BLE_LINKS.pop(self.node_name, None)
                self.node_name = data["node"]
                BLE_LINKS[self.node_name] = self
            data["is_usb"] = False
            data["transport"] = "ble"
        elif t == "alert":
            handle_alert(data, self.node_name, self.verbose)
        elif t == "ble_tracker":
            handle_tracker(data, self.node_name, self.verbose)
        elif t == "mesh_telemetry":
            data["via_link"] = "ble"
            data["gateway"] = self.node_name
            inner = data.get("data", {})
            origin = data.get("origin", "Unknown")
            if inner.get("type") == "alert":
                handle_alert(inner, origin, self.verbose)
            elif inner.get("type") == "ble_tracker":
                handle_tracker(inner, origin, self.verbose)
        try:
            self.udp_sock.sendto((json.dumps(data) + "\n").encode("utf-8"), ("255.255.255.255", UDP_PORT))
        except OSError:
            pass

    async def run(self):
        from bleak import BleakClient
        done = asyncio.Event()
        client = BleakClient(self.address, disconnected_callback=lambda _c: done.set(), timeout=15.0)
        self.loop = asyncio.get_running_loop()
        self.cmds = asyncio.Queue()
        try:
            await client.connect()
            await client.start_notify(NUS_TX, lambda _ch, d: self.feed(bytes(d)))
            BLE_LINKS[self.node_name] = self
            print(f"[+] BLE link up: {self.node_name} ({self.address})", file=sys.stderr)
            # Only "status" until framing is known: older firmware's newline-terminated command
            # replies would otherwise make its unterminated telemetry look framed
            await client.write_gatt_char(NUS_RX, b"status\n", response=False)
            synced = False
            next_sync = 0.0
            while not done.is_set():
                if self.framed and (not synced or time.time() >= next_sync):
                    await client.write_gatt_char(NUS_RX, f"time {int(time.time() * 1000000)}\n".encode(), response=False)
                    synced, next_sync = True, time.time() + 25.0   # periodic re-sync, like the USB path
                get = asyncio.ensure_future(self.cmds.get())
                gone = asyncio.ensure_future(done.wait())
                finished, _ = await asyncio.wait({get, gone}, timeout=25.0, return_when=asyncio.FIRST_COMPLETED)
                if get in finished:
                    await client.write_gatt_char(NUS_RX, (get.result().strip() + "\n").encode(), response=False)
                else:
                    get.cancel()
                if gone not in finished:
                    gone.cancel()
        except Exception as e:
            print(f"[!] BLE link {self.node_name} ({self.address}): {e}", file=sys.stderr)
        finally:
            try:
                await client.disconnect()
            except Exception:
                pass
            if BLE_LINKS.get(self.node_name) is self:
                del BLE_LINKS[self.node_name]
            self.udp_sock.close()
            print(f"[*] BLE link down: {self.node_name}", file=sys.stderr)


def ble_loop(usb_workers: Dict[str, "EspDeviceWorker"], verbose: bool = True):
    """Find nodes advertising NUS (or a node name) and hold a link to each, up to BLE_MAX_LINKS."""
    try:
        from bleak import BleakScanner
    except ImportError:
        print("[!] BLE linking off: python3-bleak is not installed", file=sys.stderr)
        return

    async def main():
        links: Dict[str, asyncio.Task] = {}
        retry_at: Dict[str, float] = {}
        last_error = ""
        while True:
            try:
                found = await BleakScanner.discover(timeout=6.0, return_adv=True)
            except Exception as e:
                # Say it once: with no adapter this fails every pass, and the phone or WiFi carries the nodes meanwhile
                if str(e) != last_error:
                    print(f"[!] BLE scan failed: {e} (retrying quietly every 30 s)", file=sys.stderr)
                    last_error = str(e)
                await asyncio.sleep(30)
                continue
            if last_error:
                print("[*] BLE scanning again", file=sys.stderr)
                last_error = ""
            for addr in [a for a, t in links.items() if t.done()]:
                del links[addr]
                retry_at[addr] = time.time() + 15
            usb_names = {w.node_name for w in list(usb_workers.values())}
            for addr, (dev, adv) in found.items():
                name = adv.local_name or dev.name or ""
                has_nus = NUS_SERVICE in [u.lower() for u in adv.service_uuids]
                if not (has_nus or NODE_NAME_RX.match(name)) or not name:
                    continue
                if addr in links or name in usb_names or time.time() < retry_at.get(addr, 0):
                    continue
                if len(links) >= BLE_MAX_LINKS:
                    break
                links[addr] = asyncio.create_task(BleNodeLink(addr, name, verbose).run())
            await asyncio.sleep(20)

    asyncio.run(main())


def autolink_loop(auto_flash=False, ble=True):
    print(f"[*] BeaconFix ESP32 Auto-Link Daemon running...", file=sys.stderr)
    print(f"[*] Relaying UDP broadcasts on port {UDP_PORT}", file=sys.stderr)
    if WIREGUARD_HUB_URL:
        print(f"[*] Hub uplink: {WIREGUARD_HUB_URL}", file=sys.stderr)
    active_workers: Dict[str, EspDeviceWorker] = {}

    # Background store-and-forward queue flusher to WireGuard hub
    threading.Thread(target=queue_worker, daemon=True).start()

    # Command dispatcher (UDP 47825). The apps on this machine send to 127.0.0.1; what no USB/BLE link here can
    # deliver also goes out as a LAN broadcast, where nodes joined to the WiFi and the phone (holding its own BLE
    # links) pick it up. Commands from other LAN hosts reach the links here only and are never sent on again, and
    # our own broadcast coming back is dropped, so nothing loops.
    own_ips = {"0.0.0.0"}
    own_ips_at = 0.0

    def is_own_ip(ip: str) -> bool:
        nonlocal own_ips, own_ips_at
        if time.time() - own_ips_at > 60:
            try:
                out = subprocess.run(["ip", "-j", "-4", "addr"], capture_output=True, text=True, timeout=3).stdout
                own_ips = {a["local"] for i in json.loads(out) for a in i.get("addr_info", [])} - {"127.0.0.1"}
            except Exception:
                pass
            own_ips_at = time.time()
        return ip in own_ips

    def cmd_listener():
        cmd_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cmd_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cmd_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            cmd_sock.bind(("0.0.0.0", 47825))
            while True:
                data, (src, _) = cmd_sock.recvfrom(1024)
                if is_own_ip(src):
                    continue
                line = data.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                # "@NodeName cmd" goes to that node only; anything else to every USB and BLE link
                target, cmd = None, line
                if cmd.startswith("@") and " " in cmd:
                    target, cmd = cmd[1:].split(" ", 1)
                delivered = False
                for w in list(active_workers.values()):
                    if target is None or w.node_name == target:
                        w.send_cmd(cmd)
                        delivered = True
                for name, link in list(BLE_LINKS.items()):
                    if target is None or name == target:
                        link.send_cmd(cmd)
                        delivered = True
                if src.startswith("127.") and (target is None or not delivered):
                    now = time.time()
                    live = {k: ip for k, (ip, seen) in list(LAN_PEERS.items()) if now - seen < LAN_PEER_TTL}
                    phones = {ip for k, ip in live.items() if k.startswith("phone@")}
                    if target is None:
                        dests = set(live.values())
                    elif target in live:
                        dests = {live[target]}
                    else:
                        dests = phones   # a phone delivers it if it holds that node's BLE link
                    # Broadcast too when nobody known could take it: a node that just joined, or a wired network
                    if not dests or (target is not None and target not in live):
                        dests.add("255.255.255.255")
                    for ip in dests:
                        try:
                            cmd_sock.sendto((line + "\n").encode(), (ip, 47825))
                        except OSError as e:
                            print(f"[!] LAN command relay to {ip} failed: {e}", file=sys.stderr)
        except Exception as e:
            print(f"[!] Command listener error: {e}", file=sys.stderr)

    threading.Thread(target=cmd_listener, daemon=True).start()

    def lan_peer_listener():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)   # the desktop app shares this port
        try:
            sock.bind(("0.0.0.0", UDP_PORT))
            while True:
                data, (src, _) = sock.recvfrom(4096)
                if src.startswith("127.") or is_own_ip(src):
                    continue
                for raw in data.decode("utf-8", errors="replace").splitlines():
                    if '"status"' not in raw and '"relay"' not in raw:
                        continue
                    try:
                        o = json.loads(raw)
                    except ValueError:
                        continue
                    if o.get("relay") == "phone":
                        LAN_PEERS[f"phone@{src}"] = (src, time.time())
                    elif o.get("type") == "status" and o.get("node"):
                        # The node itself on the WiFi, or another machine's bridge carrying it: either takes commands
                        LAN_PEERS[o["node"]] = (src, time.time())
        except Exception as e:
            print(f"[!] LAN peer listener error: {e}", file=sys.stderr)

    threading.Thread(target=lan_peer_listener, daemon=True).start()

    if ble:
        threading.Thread(target=ble_loop, args=(active_workers,), daemon=True).start()

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
    p.add_argument("--no-ble", action="store_true", help="Don't link nodes over Bluetooth LE")
    args = p.parse_args()
    autolink_loop(auto_flash=args.auto_flash, ble=not args.no_ble)


if __name__ == "__main__":
    main()
