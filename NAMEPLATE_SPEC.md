# Slide-and-Clip Nameplate Specification & Generator Guide

This document specifies the mechanical dimensions, retention geometry, and automated generator implementation for slide-and-clip nameplates used on the outdoor watertight enclosures:
- [`models/heltec_v3_housing`](file:///home/user/Documents/GitHub/3d_printing/models/heltec_v3_housing)
- [`models/esp32_housing`](file:///home/user/Documents/GitHub/3d_printing/models/esp32_housing)
- [`models/nameplates`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates)

The nameplates slide and clip into a standardized side-wall retention rail on the enclosure lids. They allow automatic generation and printing of custom callsigns, Meshtastic 4-character node short names (e.g. `!a1b2`, `NODE`), beacon identifiers, or deployment numbers during firmware flashing and node provisioning.

---

## 1. Mechanical Dimensions & Cross-Section

The nameplate rail is integrated into the left long outer wall of each enclosure lid. Both the 1U single character tiles and the full-span 4U custom nameplates share the identical self-supporting shallow dovetail cross-section.

```
                   <------- 3.7 mm (Front Face) ------->
                   +------------------------------------+  ▲
                  /                                      \ │
                 /                                        \│ 1.6 mm bevel height
+---------------+                                          +---------------+  ▲
│                                                                          │  │ 0.65 mm
│                                                                          │  │ (Total T)
+--------------------------------------------------------------------------+  ▼
<------------------------- 6.9 mm (Back Base Face) ------------------------>
```

### Critical Dimensions Table

| Parameter | Nominal | Clearance Applied | Actual Model Value | Notes |
|---|---|---|---|---|
| **Base Height ($H_{base}$)** | $7.5\text{ mm}$ | $-0.6\text{ mm}$ | **$6.9\text{ mm}$** | Fits $7.5\text{ mm}$ rail opening |
| **Front Face Height ($H_{face}$)** | $4.0\text{ mm}$ | $-0.3\text{ mm}$ | **$3.7\text{ mm}$** | Recessed display area |
| **Thickness ($T$)** | $0.75\text{ mm}$ | $-0.10\text{ mm}$ | **$0.65\text{ mm}$** | Fits $0.75\text{ mm}$ rail depth |
| **Bevel Height ($\Delta H$)** | $1.8\text{ mm}$ | $-0.2\text{ mm}$ | **$1.6\text{ mm}$** | Retaining bevels on top and bottom |
| **Bevel Angle** | $67.4^\circ$ | — | **$67.4^\circ$** from horizontal | $22.6^\circ$ from vertical, 100% self-supporting |
| **1U Width** | $8.0\text{ mm}$ | $-0.3\text{ mm}$ | **$7.7\text{ mm}$** | Single character slot |
| **2U Width** | $16.0\text{ mm}$ | $-0.3\text{ mm}$ | **$15.7\text{ mm}$** | Dual slot |
| **3U Width** | $24.0\text{ mm}$ | $-0.3\text{ mm}$ | **$23.7\text{ mm}$** | Triple slot |
| **4U Full Width** | $32.0\text{ mm}$ | $-0.3\text{ mm}$ | **$31.7\text{ mm}$** | Full-span single plate (fits $32.4\text{ mm}$ slot) |
| **Text Deboss Depth** | $0.3\text{ mm}$ | — | **$0.3\text{ mm}$** | 2 layers @ $0.16\text{ mm}$ |

---

## 2. Retention Rail Geometry (Lid Side Wall)

The mating retention rail on the enclosure lids has the following parameters:
- **Location**: Left long side wall ($y = -21.55\text{ mm}$ on Heltec V3, $y = -21.6\text{ mm}$ on ESP32).
- **Cavity Slot Length**: $32.4\text{ mm}$ (centered along X).
- **Cavity Profile**: Shallow dovetail starting at $Z_{print} = 0.9\text{ mm}$ up to $Z_{print} = 8.4\text{ mm}$.
  - Bottom lead-in: slopes from $(Y=0, Z=0.9)$ to $(Y=0.75, Z=2.8)$ ($68.5^\circ$ from horizontal).
  - Vertical throat: $(Y=0.75, Z=2.8)$ to $(Y=0.75, Z=6.7)$ ($90^\circ$ from horizontal).
  - Top overhang ceiling: slopes from $(Y=0.75, Z=6.7)$ to $(Y=0, Z=8.5)$ ($67.4^\circ$ from horizontal).
- **Zero-Support Guarantee**: Because the upper ceiling slopes at $67.4^\circ$ from horizontal (far exceeding the $30^\circ$ overhang threshold), Creality Print generates **0 tree supports** even when support is globally enabled.
- **Rear Stop**: Closed solid wall preventing tiles from sliding out the back.
- **Front Mouth**: Open entry slot with lead-in for easy slide-in insertion.

---

## 3. Print Settings for Nameplates

- **Orientation**: Flat back face down on the build plate ($Z = 0$).
- **Supports**: Disabled (`enable_support = 0`). Overhangs slope inward at $45^\circ$ and print support-free.
- **Process Preset**: `0.16mm Precise Fit K2 0.4 nozzle` or `0.20mm Standard`.
- **Wall Loops**: `4` (creates a 100% solid shell).
- **Infill**: `100%` rectilinear or concentric.
- **Materials**: PLA+ or PETG.
- **Print Time**: Under 1 minute per 1U tile, ~2 minutes for a full 4U plate (<1 gram filament).

---

## 4. Automated Python Generator Implementations

Beaconfix can automatically create custom STL nameplates for any node name during provisioning. Two ready-to-run generator methods are provided below.

### Method A: Standalone OpenSCAD CLI Generator

This method requires only `openscad` on the system and has zero Python dependencies.

Save as `tools/gen_nameplate_scad.py`:

```python
#!/usr/bin/env python3
"""
Generate custom 3D-printable slide-and-clip nameplate STLs using OpenSCAD.
Usage:
    python3 tools/gen_nameplate_scad.py "!a1b2" node_a1b2.stl
"""
import os
import sys
import subprocess
import tempfile

def generate_nameplate(text: str, out_stl: str, units: int = 4, font_size: float = 3.2):
    total_w = units * 8.0 - 0.3    # e.g. 31.7 mm for 4U
    base_h = 6.9                   # base width on bed
    face_h = 3.7                   # front recessed face
    total_t = 0.65                 # plate thickness
    bevel_h = 1.6                  # bevel width along height
    deboss_d = 0.3                 # deboss depth (2 layers @ 0.16mm)

    scad_code = f"""
    $fn = 32;
    difference() {{
        // Base plate with shallow dovetail bevels
        hull() {{
            // Bottom base face (touching print bed)
            translate([{-total_w/2}, {-base_h/2}, 0])
                cube([{total_w}, {base_h}, 0.01]);
            // Top front face
            translate([{-total_w/2}, {-face_h/2}, {total_t}])
                cube([{total_w}, {face_h}, 0.01]);
        }}
        // Debossed text on top face
        translate([0, 0, {total_t - deboss_d}])
            linear_extrude(height = {deboss_d + 0.1})
                text("{text}", size = {font_size}, font = "DejaVu Sans:style=Bold",
                     halign = "center", valign = "center");
    }}
    """

    with tempfile.NamedTemporaryFile('w', suffix='.scad', delete=False) as f:
        f.write(scad_code)
        scad_path = f.name

    try:
        os.makedirs(os.path.dirname(os.path.abspath(out_stl)), exist_ok=True)
        cmd = ['openscad', '-o', out_stl, scad_path]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        print(f"Generated nameplate STL: {out_stl} for '{text}' ({units}U)")
    finally:
        os.remove(scad_path)

if __name__ == '__main__':
    text = sys.argv[1] if len(sys.argv) > 1 else "NODE"
    out = sys.argv[2] if len(sys.argv) > 2 else f"nameplate_{text}.stl"
    generate_nameplate(text, out)
```

---

### Method B: FreeCAD CLI Generator (`freecadcmd`)

For environments with FreeCAD installed (matches the exact NURBS/BREP solid kernel used across the `3d_printing` models repository).

Save as `tools/gen_nameplate_freecad.py`:

```python
#!/usr/bin/env python3
"""
Generate custom 3D-printable slide-and-clip nameplate STLs using FreeCAD.
Usage:
    ~/Applications/FreeCAD-1.1.3.AppImage freecadcmd tools/gen_nameplate_freecad.py "!a1b2" node_a1b2.stl
"""
import os
import sys
import FreeCAD as App
import Part
import Draft
from FreeCAD import Vector as V

FONT_PATH = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'

def make_nameplate(text: str, out_stl: str, units: int = 4, font_size: float = 3.2):
    doc = App.newDocument('nameplate')
    total_w = units * 8.0 - 0.3
    total_t = 0.65
    deboss_d = 0.3

    # 1. 2D dovetail profile: (depth, height)
    pts = [
        (0.0, 0.0),
        (0.65, 1.6),
        (0.65, 5.3),
        (0.0, 6.9)
    ]
    edges = [
        Part.makeLine(V(0, pts[i][1] - 3.45, pts[i][0]),
                      V(0, pts[(i + 1) % len(pts)][1] - 3.45, pts[(i + 1) % len(pts)][0]))
        for i in range(len(pts))
    ]
    wire = Part.Wire(edges)
    face = Part.Face(wire)
    base = face.extrude(V(total_w, 0, 0))
    base.translate(V(-total_w / 2.0, 0, 0))

    # 2. Text debossing
    if text:
        ss = Draft.make_shapestring(text, FONT_PATH, font_size)
        doc.recompute()
        bb = ss.Shape.BoundBox
        ss_shape = ss.Shape.copy()
        ss_shape.translate(V(-bb.XMin - bb.XLength / 2.0, -bb.YMin - bb.YLength / 2.0, total_t - deboss_d))
        cutter = ss_shape.extrude(V(0, 0, deboss_d + 0.5))
        plate = base.cut(cutter)
    else:
        plate = base

    os.makedirs(os.path.dirname(os.path.abspath(out_stl)), exist_ok=True)
    plate.exportStl(out_stl)
    print(f"Generated FreeCAD nameplate STL: {out_stl} for '{text}'")

if __name__ == '__main__':
    text = sys.argv[1] if len(sys.argv) > 1 else "NODE"
    out = sys.argv[2] if len(sys.argv) > 2 else f"nameplate_{text}.stl"
    make_nameplate(text, out)
```

---

## 5. Pre-Generated Tile Assets

For instant manual printing, ready-to-slice STLs and plates for blank plates, digits 0–9, and letters A–Z / a–z are available in:
- Blank Plates: [`models/nameplates/stl/nameplate_4u_blank.stl`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates/stl/nameplate_4u_blank.stl)
- Digit Set (0–9): [`models/nameplates/stl/nameplate_1u_0.stl`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates/stl/nameplate_1u_0.stl) through [`nameplate_1u_9.stl`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates/stl/nameplate_1u_9.stl)
- Uppercase Set (A–Z): [`models/nameplates/stl/nameplate_1u_A.stl`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates/stl/nameplate_1u_A.stl) through [`nameplate_1u_Z.stl`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates/stl/nameplate_1u_Z.stl)
- Pre-sliced 3MF plates with Creality Print settings: [`models/nameplates/print/PLA/`](file:///home/user/Documents/GitHub/3d_printing/models/nameplates/print/PLA)
