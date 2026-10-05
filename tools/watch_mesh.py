#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BeaconFix Mesh Auto-Update & Telemetry Watcher.

Streams live real-time mesh packet traffic, antenna safety statuses,
travel/motion metrics, and link quality between the Base Station
and mobile nodes (e.g. ObsidianCheetah Base Station <-> CosmicFalcon Mobile).
"""

import argparse
import datetime
import json
import os
import socket
import sys
import time
import urllib.request

UDP_PORT = 47824
API_URL = "http://127.0.0.1:47822/api/v1"

# ANSI terminal formatting
C_RESET = "\033[0m"
C_BOLD = "\033[1m"
C_DIM = "\033[2m"
C_CYAN = "\033[36m"
C_GREEN = "\033[32m"
C_YELLOW = "\033[33m"
C_RED = "\033[31m"
C_MAGENTA = "\033[35m"
C_BLUE = "\033[34m"
C_WHITE = "\033[37m"
C_BG_RED = "\033[41m\033[37m"
C_BG_YELLOW = "\033[43m\033[30m"
C_BG_GREEN = "\033[42m\033[30m"


def fetch_api_nodes():
    try:
        req = urllib.request.Request(f"{API_URL}/nodes", headers={"User-Agent": "BeaconFix-Watcher"})
        with urllib.request.urlopen(req, timeout=1.0) as resp:
            return json.loads(resp.read().decode())
    except Exception:
        return []


def format_ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def main():
    parser = argparse.ArgumentParser(description="Watch live mesh updates and telemetry between BeaconFix nodes")
    parser.add_argument("--node", help="Filter for a specific node name or substring (e.g. CosmicFalcon)")
    parser.add_argument("--json", action="store_true", help="Output raw JSON instead of formatted text")
    parser.add_argument("--port", type=int, default=UDP_PORT, help=f"UDP port (default: {UDP_PORT})")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except Exception:
            pass

    try:
        sock.bind(("0.0.0.0", args.port))
    except Exception as e:
        print(f"Error binding to UDP port {args.port}: {e}", file=sys.stderr)
        print("Note: ensure no conflicting non-shared UDP listener is blocking the port.", file=sys.stderr)
        sys.exit(1)

    print(f"{C_BOLD}{C_CYAN}=================================================================={C_RESET}")
    print(f"{C_BOLD}{C_CYAN}  📡 BEACONFIX MESH & TELEMETRY LIVE AUTO-UPDATE WATCHER{C_RESET}")
    print(f"{C_BOLD}{C_CYAN}=================================================================={C_RESET}")
    print(f"Listening on UDP port {args.port} | API: {API_URL}")
    print("Watching live auto-updates between Base Station and Mobile nodes...\n")

    nodes_cache = {}
    last_summary_print = 0

    while True:
        try:
            data, addr = sock.recvfrom(4096)
            line = data.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            if args.json:
                print(line)
                sys.stdout.flush()
                continue

            try:
                pkt = json.loads(line)
            except Exception:
                continue

            t = pkt.get("type", "")
            ts = format_ts()

            # Filter by node name if specified
            node_key = pkt.get("node") or pkt.get("origin") or pkt.get("name") or ""
            if args.node and args.node.lower() not in node_key.lower():
                continue

            # ── 1. Status Heartbeat ───────────────────────────────────────────
            if t == "status":
                node = pkt.get("node", "Unknown")
                mode = pkt.get("mode", "mobile")
                is_gw = pkt.get("is_base_station", False) or (mode == "base_station")
                role_icon = "🏠 BASE STATION" if is_gw else "📡 MOBILE NODE"
                batt_mv = pkt.get("batt_mv", 0)
                batt_pct = pkt.get("batt_pct", 0)
                pps = pkt.get("pps", 0)
                ch = pkt.get("ch", pkt.get("wifi_ch", 1))
                tot = pkt.get("total", pkt.get("total_frames", 0))

                # Antenna Status
                has_ant = pkt.get("antenna_detected", True)
                tx_inhib = pkt.get("tx_inhibited", False)
                amb_rssi = pkt.get("ambient_rssi", -110)
                if not has_ant or tx_inhib:
                    ant_badge = f"{C_BG_RED} ⚠️ NO ANTENNA! TX INHIBITED ({amb_rssi} dBm) {C_RESET}"
                else:
                    ant_badge = f"{C_GREEN}Antenna Connected ({amb_rssi} dBm){C_RESET}"

                # Traveling Status
                traveling = pkt.get("traveling", False)
                spd = pkt.get("speed_kmh", 0.0)
                hdg = pkt.get("heading", -1.0)
                if traveling and spd > 0:
                    trav_badge = f"{C_BG_GREEN} 🚗 TRAVELING: {spd:.1f} km/h ({spd * 0.621371:.1f} mph) {C_RESET}"
                else:
                    trav_badge = f"{C_DIM}[Stationary]{C_RESET}"

                # Attached / Follow Device
                att_dev = pkt.get("attached_dev", "")
                att_str = f" | Follows: {C_CYAN}{att_dev}{C_RESET}" if att_dev else ""

                print(f"[{ts}] {C_BOLD}{C_BLUE}[HEARTBEAT]{C_RESET} {C_BOLD}{node}{C_RESET} ({role_icon})")
                print(f"   ├─ Power  : {batt_pct}% ({batt_mv} mV) | Wi-Fi: Ch {ch} ({pps} pps, {tot} tot)")
                print(f"   ├─ RF/LoRa: {ant_badge}")
                print(f"   └─ Motion : {trav_badge}{att_str}")
                nodes_cache[node] = pkt

            # ── 2. Mesh Relay Telemetry (Base Station receiving from Mobile) ──
            elif t == "mesh_telemetry":
                origin = pkt.get("origin", "Unknown")
                mac = pkt.get("mac", "")
                hops = pkt.get("hops", 0)
                via = pkt.get("via", "")
                route = pkt.get("route", "")
                ts_us = pkt.get("ts_us", 0)
                inner = pkt.get("data", {})
                inner_type = inner.get("type", "data")

                hop_color = C_GREEN if hops <= 1 else (C_YELLOW if hops == 2 else C_RED)
                route_desc = f" [{route}]" if route else (f" [via {via}]" if via and via != "Direct" else "")
                print(f"[{ts}] {C_BOLD}{C_MAGENTA}[MESH RELAY]{C_RESET} {C_BOLD}{origin}{C_RESET} ({mac}) ──({hop_color}{hops} hops{route_desc}{C_RESET})──> Base Station | {inner_type}")
                if "batt_mv" in inner:
                    print(f"   └─ Mobile Battery: {inner.get('batt_pct', 0)}% ({inner.get('batt_mv', 0)} mV)")
                nodes_cache[origin] = inner

            # ── 3. Antenna Diagnostics ────────────────────────────────────────
            elif t == "antenna_status":
                det = pkt.get("detected", False)
                inhib = pkt.get("tx_inhibited", True)
                amb = pkt.get("ambient_rssi", -126)
                if not det or inhib:
                    print(f"[{ts}] {C_BG_RED} ⚠️ ANTENNA ALERT {C_RESET} Detected={det} | TX Inhibited={inhib} | Floor={amb} dBm (PA SAFE)")
                else:
                    print(f"[{ts}] {C_GREEN}[ANTENNA OK]{C_RESET} Detected=True | TX Ready (+22 dBm) | Floor={amb} dBm")

            # ── 4. ALPR Alert Detected ────────────────────────────────────────
            elif t == "alpr_alert" or (t == "alert" and pkt.get("event") == "alpr"):
                op = pkt.get("operator", "Flock Safety")
                mdl = pkt.get("model", "Falcon")
                dist = pkt.get("distance_m", 0.0)
                conf = pkt.get("conf", 0)
                spd = pkt.get("speed_kmh", 0.0)
                print(f"[{ts}] {C_BG_YELLOW}{C_BOLD} [!] ALPR DETECTED [!] {C_RESET} {op} {mdl} | Dist: {dist:.0f}m | Conf: {conf}% | Pass Speed: {spd:.0f} km/h")

            # ── 5. BLE Tracker Detected ───────────────────────────────────────
            elif t == "ble_tracker":
                kind = pkt.get("kind", "tracker")
                mac = pkt.get("mac", "")
                rssi = pkt.get("rssi", 0)
                name = pkt.get("name", "")
                name_str = f" ({name})" if name else ""
                print(f"[{ts}] {C_BOLD}{C_YELLOW}[TRACKER]{C_RESET} {kind.upper()} {mac}{name_str} | RSSI: {rssi} dBm")

            # ── 6. Wi-Fi Attack / Deauth ───────────────────────────────────────
            elif t == "alert" and pkt.get("event") in ("deauth", "disassoc"):
                sa = pkt.get("sa", "")
                da = pkt.get("da", "")
                ch = pkt.get("ch", 0)
                print(f"[{ts}] {C_RED}[DEAUTH ATTACK]{C_RESET} AP: {sa} -> Target: {da} (Ch {ch})")

            # ── 7. Travel / Motion Update ─────────────────────────────────────
            elif t == "travel_status":
                trav = pkt.get("traveling", False)
                state = pkt.get("state", "STATIONARY")
                spd_k = pkt.get("speed_kmh", 0.0)
                spd_m = pkt.get("speed_mph", 0.0)
                hdg = pkt.get("heading", 0.0)
                card = pkt.get("card", "")
                trip = pkt.get("trip_km", 0.0)
                print(f"[{ts}] {C_BOLD}{C_CYAN}[TRAVEL]{C_RESET} State: {state} | Speed: {spd_k:.1f} km/h ({spd_m:.1f} mph) | Course: {hdg:.0f}° {card} | Trip: {trip:.2f} km")

            sys.stdout.flush()

        except KeyboardInterrupt:
            print("\nExiting watcher.")
            break
        except Exception as e:
            time.sleep(0.1)


if __name__ == "__main__":
    main()
