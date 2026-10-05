#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BeaconFix Slide-and-Clip Nameplate Generator.
Implements the full mechanical specification from NAMEPLATE_SPEC.md.

Uses the official node naming dictionaries (ADJECTIVES, NOUNS, ROLES)
to mint new node names, callsigns, and generate 3D-printable enclosure STLs.
"""

import argparse
import hashlib
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from typing import Optional, Tuple

# ── Official BeaconFix Node Dictionaries (from firmware/heltec_v3/NodeConfig.h) ──
ADJECTIVES = [
    "Swift", "Brave", "Quiet", "Curious", "Silent", "Silver", "Golden", "Iron",
    "Crimson", "Frosty", "Shadow", "Astral", "Cosmic", "Vibrant", "Fierce", "Steady",
    "Mighty", "Wild", "Noble", "Clever", "Electric", "Solar", "Lunar", "Thunder",
    "Echo", "Phantom", "Stormy", "Hidden", "Ancient", "Radiant", "Apex", "Turbo",
    "Rapid", "Velvet", "Keen", "Amber", "Crystal", "Obsidian", "Hyper", "Blaze",
    "Ember", "Ghost", "Cobalt", "Rustic", "Mystic", "Daring", "Sonic", "Prism",
    "Copper", "Vivid", "Titan", "Alpha", "Stellar", "Vector", "Flash", "Arctic",
    "Neon", "Granite", "Zephyr", "Onyx", "Rogue", "Spirited", "Vortex", "Horizon"
]

NOUNS = [
    "Falcon", "Wolf", "Hawk", "Raven", "Lynx", "Tiger", "Viper", "Badger",
    "Otter", "Fox", "Bear", "Eagle", "Cobra", "Panther", "Stag", "Puma",
    "Dragon", "Jackal", "Bison", "Raptor", "Jaguar", "Kestrel", "Cheetah", "Osprey",
    "Mustang", "Condor", "Badger", "Mantis", "Orca", "Hornet", "Sparrow", "Bison",
    "Grizzly", "Badger", "Condor", "Stallion", "Vulture", "Leopard", "Bison", "Coyote",
    "Heron", "Chameleon", "Gazelle", "Buffalo", "Lynx", "Scorpion", "Pelican", "Wolverine",
    "Beaver", "Penguin", "Armadillo", "Moose", "Badger", "Sturgeon", "Barracuda", "Shark",
    "Falcon", "Cougar", "Mastiff", "Husky", "Foxhound", "Terrier", "Collie", "Retriever"
]

ROLES = [
    "Ranger", "Hunter", "Wanderer", "Seeker", "Watcher", "Tracker", "Scout", "Runner",
    "Drifter", "Nomad", "Guardian", "Prowler", "Rover", "Pilot", "Sentry", "Stalker",
    "Breaker", "Voyager", "Observer", "Crafter", "Strider", "Explorer", "Surveyor", "Chaser",
    "Patrol", "Navi", "Finder", "Recon", "Monitor", "Anchor", "Beacon", "Pioneer",
    "Spotter", "Sniper", "Leader", "Caster", "Cipher", "Scribe", "Herald", "Keeper",
    "Protector", "Vanguard", "Sentinel", "Operative", "Specialist", "Detective", "Guide", "Marshal",
    "Warden", "Surveyor", "Harvester", "Pathfinder", "Outrider", "Captain", "Master", "Agent",
    "Scout", "Lookout", "Navigator", "Signaler", "Relay", "Listener", "Sniffer", "Tracer"
]

# ── Mechanical Specification Table (from NAMEPLATE_SPEC.md) ───────────────────
NOMINAL_HEIGHT = 7.5          # mm
HEIGHT_CLEARANCE = -0.3       # mm
MODEL_HEIGHT = 7.2            # mm (fits 7.8mm rail cavity)
TOTAL_THICKNESS = 1.4         # mm (fits 1.6mm rail depth)
BEVEL_ANGLE_DEG = 45.0        # degrees
BEVEL_INSET = 0.8             # mm (leaves 5.6mm flat front face)
DEBOSS_DEPTH = 0.4            # mm (2 layers @ 0.20mm standard)
UNIT_PITCH = 8.0              # mm per U
CLEARANCE_X = 0.3             # mm total clearance across width

DEFAULT_OUTPUT_DIR = os.path.expanduser("~/.local/share/beaconfix/nameplates")


def mint_node_name(mac_bytes: Optional[bytes] = None) -> Tuple[str, str]:
    """
    Mint a node name using the official dictionaries.
    If mac_bytes (6 bytes) is provided, name generation is deterministic.
    Returns:
        (full_name, short_callsign) e.g. ("CosmicFalconMaster9463", "CF94")
    """
    if mac_bytes and len(mac_bytes) >= 6:
        # FNV-1a deterministic hash matching firmware (firmware/heltec_v3/NodeConfig.h)
        h = 2166136261
        for b in mac_bytes[:6]:
            h ^= b
            h = (h * 16777619) & 0xFFFFFFFF
        idx_adj = (h >> 16) % len(ADJECTIVES)
        idx_noun = (h >> 8) % len(NOUNS)
        idx_role = h % len(ROLES)
        num = 1000 + ((h >> 4) % 9000)
    else:
        idx_adj = random.randrange(len(ADJECTIVES))
        idx_noun = random.randrange(len(NOUNS))
        idx_role = random.randrange(len(ROLES))
        num = random.randint(1000, 9999)

    adj = ADJECTIVES[idx_adj]
    noun = NOUNS[idx_noun]
    role = ROLES[idx_role]
    full_name = f"{adj}{noun}{role}{num}"

    # Generate 4-character short callsign
    short_callsign = f"{adj[0]}{noun[0]}{str(num)[-2:]}"
    return full_name, short_callsign


def compute_plate_dimensions(units: int = 4) -> Tuple[float, float, float]:
    """
    Returns (width_mm, height_mm, thickness_mm) according to NAMEPLATE_SPEC.md.
    1U = 7.7 mm, 2U = 15.7 mm, 3U = 23.7 mm, 4U = 31.7 mm.
    """
    units = max(1, min(4, units))
    width_mm = (units * UNIT_PITCH) - CLEARANCE_X
    return width_mm, MODEL_HEIGHT, TOTAL_THICKNESS


def generate_scad_script(text: str, units: int = 4, font_size: float = 3.6, font_style: str = "DejaVu Sans:style=Bold") -> str:
    """Generate OpenSCAD script for the slide-and-clip nameplate."""
    w, h, t = compute_plate_dimensions(units)
    lip_h = BEVEL_INSET
    deboss_d = DEBOSS_DEPTH

    # Choose font size depending on text length if not explicitly set
    if len(text) <= 4:
        f_size = font_size if font_size > 0 else 4.2
    elif len(text) <= 8:
        f_size = 3.4
    else:
        f_size = 2.8

    scad = f"""// BeaconFix Slide-and-Clip Nameplate
// Mechanical Spec: NAMEPLATE_SPEC.md
$fn = 40;

total_w = {w:.3f};
total_h = {h:.3f};
total_t = {t:.3f};
lip_h = {lip_h:.3f};
deboss_d = {deboss_d:.3f};

difference() {{
    // Base plate with 45-degree retention bevels along top and bottom
    hull() {{
        // Bottom face on print bed
        translate([-total_w/2, -total_h/2, 0])
            cube([total_w, total_h, 0.01]);
        // Top face recessed by 0.8mm bevel inset
        translate([-total_w/2, -total_h/2 + lip_h, total_t])
            cube([total_w, total_h - 2*lip_h, 0.01]);
    }}

    // Text debossing on top face (0.4mm depth)
    translate([0, 0, total_t - deboss_d])
        linear_extrude(height = deboss_d + 0.2)
            text("{text}", size = {f_size:.2f}, font = "{font_style}",
                 halign = "center", valign = "center");
}}
"""
    return scad


def build_stl_openscad(text: str, out_stl_path: str, units: int = 4, font_size: float = 3.6) -> bool:
    """Compile nameplate STL using OpenSCAD."""
    scad_code = generate_scad_script(text, units, font_size)

    with tempfile.NamedTemporaryFile("w", suffix=".scad", delete=False) as f:
        f.write(scad_code)
        scad_path = f.name

    try:
        os.makedirs(os.path.dirname(os.path.abspath(out_stl_path)), exist_ok=True)
        cmd = ["openscad", "-o", out_stl_path, scad_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        return os.path.exists(out_stl_path) and os.path.getsize(out_stl_path) > 100
    except Exception as e:
        print(f"[!] OpenSCAD execution failed: {e}", file=sys.stderr)
        return False
    finally:
        if os.path.exists(scad_path):
            os.remove(scad_path)


def generate_nameplate_for_node(node_name: str, out_dir: Optional[str] = None, units: int = 4, short_mode: bool = False) -> str:
    """
    Generates a standardized nameplate STL for a given node.
    Returns the absolute path to the generated STL file.
    """
    if not out_dir:
        out_dir = DEFAULT_OUTPUT_DIR
    os.makedirs(out_dir, exist_ok=True)

    # Determine display text
    if short_mode or len(node_name) > 12:
        # Extract short callsign or acronym (e.g. CosmicFalconMaster9463 -> CF94)
        m = re.match(r"^([A-Z][a-z]+)?([A-Z][a-z]+)?([A-Z][a-z]+)?(\d+)?", node_name)
        if m and any(m.groups()):
            parts = [g for g in m.groups() if g]
            if len(parts) >= 2 and parts[-1].isdigit():
                text = "".join(p[0].upper() for p in parts[:-1]) + parts[-1][-2:]
            else:
                text = node_name[:4].upper()
        else:
            text = node_name[:4].upper()
    else:
        text = node_name

    safe_name = re.sub(r"[^A-Za-z0-9_-]", "_", node_name)
    out_stl = os.path.join(out_dir, f"nameplate_{safe_name}.stl")

    ok = build_stl_openscad(text, out_stl, units=units)
    if ok:
        print(f"[✓] Generated 3D Nameplate: {out_stl} for '{node_name}' (plate text: '{text}')", file=sys.stderr)
    return out_stl if ok else ""



def main():
    parser = argparse.ArgumentParser(description="BeaconFix Slide-and-Clip Nameplate Generator (NAMEPLATE_SPEC.md)")
    parser.add_argument("--mint", action="store_true", help="Mint a fresh node name from dictionaries")
    parser.add_argument("--name", type=str, default="", help="Node name or custom text on the nameplate")
    parser.add_argument("--out", type=str, default="", help="Output STL path (default: ~/.local/share/beaconfix/nameplates/)")
    parser.add_argument("--units", type=int, default=4, choices=[1, 2, 3, 4], help="Unit size (1U=7.7mm, 2U=15.7mm, 3U=23.7mm, 4U=31.7mm)")
    parser.add_argument("--short", action="store_true", help="Use 4-character callsign / short ID mode")
    parser.add_argument("--mac", type=str, default="", help="ESP32 MAC address (e.g. 34:85:18:12:34:56) for deterministic minting")
    parser.add_argument("--json", action="store_true", help="Output result in JSON format")
    parser.add_argument("--list", action="store_true", help="List all generated nameplates in output directory")
    args = parser.parse_args()

    import json

    out_dir = args.out if (args.out and os.path.isdir(args.out)) else DEFAULT_OUTPUT_DIR
    if args.list:
        os.makedirs(out_dir, exist_ok=True)
        files = []
        for fn in sorted(os.listdir(out_dir)):
            if fn.endswith(".stl"):
                fp = os.path.join(out_dir, fn)
                st = os.stat(fp)
                files.append({
                    "fileName": fn,
                    "filePath": fp,
                    "sizeBytes": st.st_size,
                    "mtime": st.st_mtime
                })
        if args.json:
            print(json.dumps({"ok": True, "dir": out_dir, "nameplates": files}, indent=2))
        else:
            print(f"Nameplates in {out_dir}:")
            for f in files:
                print(f"  • {f['fileName']} ({f['sizeBytes']:,} bytes)")
        return

    node_name = args.name
    callsign = ""

    if args.mint or not node_name:
        mac_bytes = None
        if args.mac:
            clean_mac = args.mac.replace(":", "").replace("-", "")
            if len(clean_mac) == 12:
                mac_bytes = bytes.fromhex(clean_mac)
        node_name, callsign = mint_node_name(mac_bytes)
        if not args.json:
            print(f"[*] Minted Node Name: {node_name} (Callsign: {callsign})")

    out_file = args.out
    if not out_file or os.path.isdir(out_file):
        target_dir = out_file if (out_file and os.path.isdir(out_file)) else DEFAULT_OUTPUT_DIR
        out_file = os.path.join(target_dir, f"nameplate_{node_name}.stl")

    display_text = callsign if (args.short and callsign) else node_name
    stl_path = generate_nameplate_for_node(display_text, out_dir=os.path.dirname(out_file), units=args.units, short_mode=args.short)
    
    w_mm, h_mm, t_mm = compute_plate_dimensions(args.units)
    if stl_path:
        if args.json:
            print(json.dumps({
                "ok": True,
                "name": node_name,
                "callsign": callsign,
                "text": display_text,
                "units": args.units,
                "width_mm": w_mm,
                "height_mm": h_mm,
                "thickness_mm": t_mm,
                "stl": stl_path,
                "stlFileName": os.path.basename(stl_path)
            }))
        else:
            print(f"SUCCESS: Nameplate saved to {stl_path}")
    else:
        if args.json:
            print(json.dumps({"ok": False, "error": "Failed to generate STL"}))
        sys.exit(1)


if __name__ == "__main__":
    main()

