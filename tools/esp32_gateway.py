#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
ESP32 Telemetry Gateway & Control CLI for BeaconFix.

Connects to ESP32 nodes via USB Serial (auto-detected), Wi-Fi UDP (port 47824),
or acts as a bridge relaying live monitor mode telemetry (probes, beacons,
trackers, deauth alerts) to the BeaconFix desktop client or phone.
"""

import argparse
import glob
import json
import os
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def find_esp32_port():
    """Auto-detect connected ESP32 serial port."""
    by_id = glob.glob("/dev/serial/by-id/*")
    for p in by_id:
        lower = p.lower()
        if "cp210" in lower or "ch340" in lower or "ftdi" in lower or "uart" in lower or "esp32" in lower:
            return os.path.realpath(p)

    candidates = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if candidates:
        return candidates[0]
    return None


class Esp32Node:
    def __init__(self, port=None, baud=115200, udp_port=47824):
        self.port = port or find_esp32_port()
        self.baud = baud
        self.udp_port = udp_port
        self.ser = None
        self.running = False
        self.udp_sock = None

    def connect(self):
        if not self.port or not os.path.exists(self.port):
            raise RuntimeError(f"ESP32 port not found (searched /dev/serial/by-id, /dev/ttyUSB*, /dev/ttyACM*)")
        self.ser = serial.Serial(self.port, self.baud, timeout=1.0)
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        return True

    def send_command(self, cmd: str) -> str:
        if not self.ser:
            self.connect()
        self.ser.write((cmd.strip() + "\n").encode("utf-8"))
        self.ser.flush()
        deadline = time.time() + 1.5
        while time.time() < deadline:
            line = self.ser.readline().decode("utf-8", errors="replace").strip()
            if line.startswith("{") and ("ack" in line or "status" in line or "help" in line or "error" in line):
                return line
        return '{"type":"timeout"}'

    def stream(self, relay_udp=True, on_packet=None):
        if not self.ser:
            self.connect()
        self.running = True
        print(f"[*] Streaming live telemetry from {self.port} at {self.baud} baud...", file=sys.stderr)
        if relay_udp:
            print(f"[*] Relaying UDP broadcasts to 255.255.255.255:{self.udp_port}", file=sys.stderr)

        while self.running:
            try:
                raw = self.ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("{"):
                    continue

                if relay_udp and self.udp_sock:
                    try:
                        self.udp_sock.sendto((line + "\n").encode("utf-8"), ("255.255.255.255", self.udp_port))
                    except Exception:
                        pass

                if on_packet:
                    on_packet(line)
                else:
                    print(line)
            except KeyboardInterrupt:
                break
            except Exception as e:
                print(f"[!] Serial read error: {e}", file=sys.stderr)
                time.sleep(1)

    def close(self):
        self.running = False
        if self.ser and self.ser.is_open:
            self.ser.close()
        if self.udp_sock:
            self.udp_sock.close()


def main():
    p = argparse.ArgumentParser(description="BeaconFix ESP32 Telemetry Gateway & Control CLI")
    p.add_argument("--port", "-p", default=None, help="Serial port (auto-detected if omitted)")
    p.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default 115200)")
    p.add_argument("--udp-port", type=int, default=47824, help="UDP broadcast port (default 47824)")
    p.add_argument("--status", action="store_true", help="Query node status and exit")
    p.add_argument("--channel", "-c", type=int, default=None, help="Set Wi-Fi channel (1-14, or 0 for auto-hop)")
    p.add_argument("--hop", type=int, default=None, help="Set channel hop interval in ms (20-5000)")
    p.add_argument("--led", type=str, default=None, help="Set or test LED pattern (heartbeat, probe, tracker, alert, radio_err, heap_err, sos, pin <N>)")
    p.add_argument("--reboot", action="store_true", help="Reboot ESP32 node")
    p.add_argument("--stream", action="store_true", help="Stream telemetry NDJSON to stdout and UDP")
    p.add_argument("--no-udp", action="store_true", help="Disable UDP relay during stream")

    args = p.parse_args()
    node = Esp32Node(port=args.port, baud=args.baud, udp_port=args.udp_port)

    if args.status:
        resp = node.send_command("status")
        print(resp)
        return

    if args.channel is not None:
        resp = node.send_command(f"channel {args.channel}")
        print(resp)
        return

    if args.hop is not None:
        resp = node.send_command(f"hop {args.hop}")
        print(resp)
        return

    if args.led is not None:
        resp = node.send_command(f"led {args.led}")
        print(resp)
        return

    if args.reboot:
        resp = node.send_command("reboot")
        print(resp)
        return

    # Default action: stream telemetry
    try:
        node.stream(relay_udp=not args.no_udp)
    finally:
        node.close()


if __name__ == "__main__":
    main()
