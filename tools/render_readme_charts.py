#!/usr/bin/env python3
"""The README's charts and diagram, as SVG that follows the reader's light/dark theme.

    python3 tools/render_readme_charts.py      # writes docs/img/*.svg

Every number here is a measurement, with where it came from next to it. Change the data, re-run, commit the SVGs.
Palette: two categorical slots (blue, orange), validated for colour-vision deficiency in both modes.
"""
import math
import os

OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "img")

STYLE = """<style>
  .bg{fill:none}
  .t{font:600 15px system-ui,-apple-system,'Segoe UI',sans-serif;fill:#0b0b0b}
  .s{font:400 12px system-ui,-apple-system,'Segoe UI',sans-serif;fill:#52514e}
  .l{font:400 12px system-ui,-apple-system,'Segoe UI',sans-serif;fill:#0b0b0b}
  .v{font:600 12px system-ui,-apple-system,'Segoe UI',sans-serif;fill:#0b0b0b}
  .g{stroke:#e4e3df;stroke-width:1}
  .ax{stroke:#a8a7a1;stroke-width:1}
  .c1{fill:#2a78d6}.c2{fill:#eb6834}.k1{stroke:#2a78d6}.k2{stroke:#eb6834}
  .box{fill:#f4f3f0;stroke:#d6d5d0;stroke-width:1}
  .hl{fill:#e8f0fb;stroke:#2a78d6;stroke-width:1.5}
  .ln{stroke:#8a8983;stroke-width:1.5;fill:none}
  .lnb{stroke:#2a78d6;stroke-width:2;fill:none}
  .arr{fill:#8a8983}
  @media (prefers-color-scheme: dark){
    .t,.l,.v{fill:#f0efec}.s{fill:#c3c2b7}
    .g{stroke:#34332f}.ax{stroke:#6c6b66}
    .c1{fill:#3987e5}.c2{fill:#d95926}.k1{stroke:#3987e5}.k2{stroke:#d95926}
    .box{fill:#22221f;stroke:#45443f}
    .hl{fill:#1b2a3d;stroke:#3987e5}
    .ln{stroke:#8f8e87}.lnb{stroke:#3987e5}.arr{fill:#8f8e87}
  }
</style>"""


def svg(w, h, body, title, desc):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img">'
            f'<title>{title}</title><desc>{desc}</desc>{STYLE}{body}</svg>\n')


def bar(x, y, w, h, cls):
    """A horizontal bar anchored at x, 4 px rounding on the data end only."""
    if w <= 0:
        return ""
    r = min(4, h / 2, w)
    return (f'<path class="{cls}" d="M{x:.1f},{y:.1f} h{w - r:.1f} a{r},{r} 0 0 1 {r},{r} v{h - 2 * r:.1f} '
            f'a{r},{r} 0 0 1 -{r},{r} h-{w - r:.1f} z"/>')


def legend(x, y, items):
    out, cx = [], x
    for cls, text in items:
        out.append(f'<rect class="{cls}" x="{cx}" y="{y - 9}" width="10" height="10" rx="2"/>')
        out.append(f'<text class="s" x="{cx + 15}" y="{y}">{text}</text>')
        cx += 30 + len(text) * 6.4
    return "".join(out)


# ── 1. Where an AP is: estimator 3, synthetic study, 300 trials per scenario (docs/GRADING.md) ──
AP_SCENARIOS = [  # scenario, median error (m), median R95 (m)
    ("One walk around the building", 10.5, 23.1),
    ("Two walks around it", 9.3, 20.0),
    ("A walk + samples 60–120 m up a hill", 15.5, 27.6),
    ("A walk + hours indoors", 10.0, 46.1),
    ("A 40–80 m street loop", 12.5, 27.1),
]


def ap_accuracy():
    w, left, right, top, row = 760, 230, 70, 74, 46
    h = top + row * len(AP_SCENARIOS) + 40
    xmax = 50.0
    sx = lambda v: left + (w - left - right) * v / xmax
    b = [f'<rect class="bg" width="{w}" height="{h}"/>',
         '<text class="t" x="0" y="20">How close BeaconFix puts an access point</text>',
         '<text class="s" x="0" y="40">Median over 300 simulated surveys per scenario, with walls, shadowing and GPS error (estimator 3)</text>',
         legend(left, 62, [("c1", "median miss"), ("c2", "95 % radius it reports")])]
    for t in range(0, 51, 10):
        x = sx(t)
        b.append(f'<line class="g" x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{h - 34}"/>')
        b.append(f'<text class="s" x="{x:.1f}" y="{h - 18}" text-anchor="middle">{t} m</text>')
    for i, (name, err, r95) in enumerate(AP_SCENARIOS):
        y = top + i * row
        b.append(f'<text class="l" x="{left - 12}" y="{y + 19}" text-anchor="end">{name}</text>')
        b.append(bar(left, y + 3, sx(err) - left, 14, "c1"))
        b.append(f'<text class="v" x="{sx(err) + 6:.1f}" y="{y + 14}">{err:g} m</text>')
        b.append(bar(left, y + 20, sx(r95) - left, 14, "c2"))
        b.append(f'<text class="s" x="{sx(r95) + 6:.1f}" y="{y + 31}">{r95:g} m</text>')
    b.append(f'<line class="ax" x1="{left}" y1="{top - 4}" x2="{left}" y2="{h - 34}"/>')
    return svg(w, h, "".join(b), "How close BeaconFix puts an access point",
               "Median miss and reported 95 percent radius per scenario: " +
               "; ".join(f"{n}: {e} m, {r} m" for n, e, r in AP_SCENARIOS))


# ── 2. The phone sitting still, before and after 3.12 (Pixel 10 Pro XL, measured 2026-10-09) ──
PHONE = [  # panel title, unit, before, after, note
    ("BeaconFix CPU, phone parked", "% of one core", 17.0, 3.0, "1 h 55 min of CPU over 11.4 h → under 2 min an hour"),
    ("GPS held by BeaconFix", "hours of the last 11.5", 5.6, 0.0, "high accuracy, a fix every 2.5 s → off"),
]


def phone_idle():
    w, pw, gap, top = 760, 360, 40, 64
    h = 210
    b = [f'<rect class="bg" width="{w}" height="{h}"/>',
         '<text class="t" x="0" y="20">The phone app when you\'re not going anywhere</text>',
         '<text class="s" x="0" y="40">Pixel 10 Pro XL parked with a node linked, screen off. Before (battery stats over 11.4 h) and after this release.</text>']
    for p, (title, unit, before, after, note) in enumerate(PHONE):
        x0 = p * (pw + gap)
        left = x0 + 70
        span = pw - 70 - 60
        vmax = before * 1.05
        sx = lambda v: left + span * v / vmax
        b.append(f'<text class="l" x="{x0}" y="{top + 8}" font-weight="600">{title}</text>')
        b.append(f'<text class="s" x="{x0}" y="{top + 26}">{unit}</text>')
        for j, (label, v, cls) in enumerate((("before", before, "c2"), ("after", after, "c1"))):
            y = top + 44 + j * 36
            b.append(f'<text class="l" x="{left - 10}" y="{y + 15}" text-anchor="end">{label}</text>')
            if v > 0:
                b.append(bar(left, y + 2, sx(v) - left, 20, cls))
            else:
                b.append(f'<line class="k1" x1="{left}" y1="{y + 2}" x2="{left}" y2="{y + 22}" stroke-width="2"/>')
            b.append(f'<text class="v" x="{(sx(v) if v > 0 else left) + 7:.1f}" y="{y + 16}">{v:g}</text>')
        b.append(f'<line class="ax" x1="{left}" y1="{top + 40}" x2="{left}" y2="{top + 120}"/>')
        b.append(f'<text class="s" x="{x0}" y="{top + 140}">{note}</text>')
    return svg(w, h, "".join(b), "The phone app when you're not going anywhere",
               "; ".join(f"{t}: {bf} before, {af} after ({u})" for t, u, bf, af, _ in PHONE))


# ── 3. What a look costs BeaconFix Lite, on the MikuOS M500 itself (measured 2026-10-09) ──
LITE = [  # what, milliseconds
    ("Same APs as last time (reuse)", 1.5),
    ("Reuse after a restart (cache load)", 12.7),
    ("Full offline solve", 17.6),
    ("First fix here (one Apple lookup)", 672.0),
]


def lite_cost():
    w, left, right, top, row = 760, 250, 80, 64, 34
    h = top + row * len(LITE) + 44
    lo, hi = 1.0, 1000.0
    sx = lambda v: left + (w - left - right) * (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
    b = [f'<rect class="bg" width="{w}" height="{h}"/>',
         '<text class="t" x="0" y="20">What a look costs BeaconFix Lite</text>',
         '<text class="s" x="0" y="40">Measured on the MikuOS M500 (arm64, Android 14), 33 APs heard. Log scale: each line is 10×.</text>']
    for t in (1, 10, 100, 1000):
        x = sx(t)
        b.append(f'<line class="g" x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{h - 38}"/>')
        b.append(f'<text class="s" x="{x:.1f}" y="{h - 22}" text-anchor="middle">{t:g} ms</text>')
    for i, (name, ms) in enumerate(LITE):
        y = top + i * row
        b.append(f'<text class="l" x="{left - 12}" y="{y + 16}" text-anchor="end">{name}</text>')
        b.append(bar(left, y + 3, sx(ms) - left, 18, "c1"))
        b.append(f'<text class="v" x="{sx(ms) + 6:.1f}" y="{y + 16}">{ms:g} ms</text>')
    b.append(f'<line class="ax" x1="{left}" y1="{top - 4}" x2="{left}" y2="{h - 38}"/>')
    return svg(w, h, "".join(b), "What a look costs BeaconFix Lite",
               "; ".join(f"{n}: {ms} ms" for n, ms in LITE))


# ── 4. How the pieces talk ──
def architecture():
    w, h = 760, 360
    r1, r2, bw, bh = 74, 240, 190, 92

    def box(x, y, title, sub, cls="box", dash=False):
        d = ' stroke-dasharray="5 4"' if dash else ""
        return (f'<rect class="{cls}" x="{x}" y="{y}" width="{bw}" height="{bh}" rx="8"{d}/>'
                f'<text class="l" x="{x + bw / 2}" y="{y + 24}" text-anchor="middle" font-weight="600">{title}</text>'
                + "".join(f'<text class="s" x="{x + bw / 2}" y="{y + 44 + 16 * k}" text-anchor="middle">{s}</text>'
                          for k, s in enumerate(sub)))

    def path(pts, label="", lx=0, ly=0, anchor="middle"):
        """A polyline with an arrowhead on its last segment."""
        (x1, y1), (x2, y2) = pts[-2], pts[-1]
        ang = math.atan2(y2 - y1, x2 - x1)
        a1 = (x2 - 8 * math.cos(ang) + 4 * math.sin(ang), y2 - 8 * math.sin(ang) - 4 * math.cos(ang))
        a2 = (x2 - 8 * math.cos(ang) - 4 * math.sin(ang), y2 - 8 * math.sin(ang) + 4 * math.cos(ang))
        d = "M" + " L".join(f"{x},{y}" for x, y in pts[:-1]) + f" L{x2 - 7 * math.cos(ang):.1f},{y2 - 7 * math.sin(ang):.1f}"
        out = f'<path class="ln" d="{d}"/><path class="arr" d="M{x2},{y2} L{a1[0]:.1f},{a1[1]:.1f} L{a2[0]:.1f},{a2[1]:.1f} z"/>'
        if label:
            out += f'<text class="s" x="{lx}" y="{ly}" text-anchor="{anchor}">{label}</text>'
        return out

    b = [f'<rect class="bg" width="{w}" height="{h}"/>',
         '<text class="t" x="0" y="20">How the pieces talk</text>',
         box(0, r1, "ESP32 / Heltec nodes", ["Wi-Fi monitor, BLE scanner", "ESP-NOW + LoRa mesh"]),
         box(285, r1, "Android app", ["GPS, scans, ALPR camera", "store-and-forward for nodes"]),
         box(570, r1, "Desktop + tray", ["the locator, the map,", "the encrypted database"], "hl"),
         box(285, r2, "Pi agent", ["a remote radio", "for the desktop"]),
         box(570, r2, "Your hub (optional)", ["headless, self-hosted,", "reachable over your VPN"]),
         box(0, r2, "BeaconFix Lite", ["the locator as a library:", "players, frames, sensors"], dash=True),
         # nodes ⇄ phone
         path([(190, r1 + 34), (285, r1 + 34)], "BLE", 237, r1 + 26),
         path([(285, r1 + 60), (190, r1 + 60)], "GPS, time", 237, r1 + 78),
         # phone → desktop
         path([(475, r1 + 46), (570, r1 + 46)], "LAN sync", 522, r1 + 38),
         # nodes on Wi-Fi → desktop, over the top
         path([(95, r1), (95, 46), (665, 46), (665, r1)], "nodes on Wi-Fi: UDP", 380, 40),
         # Pi agent → desktop
         path([(475, r2 + 30), (520, r2 + 30), (520, r1 + bh + 26), (620, r1 + bh + 26), (620, r1 + bh)], "LAN", 528, r2 + 24, "start"),
         # desktop and phone → hub
         path([(700, r1 + bh), (700, r2)], "BFS3 sync", 708, r1 + bh + 46, "start"),
         '<text class="s" x="95" y="350" text-anchor="middle">asks Apple / BeaconDB only for</text>',
         '<text class="s" x="95" y="335" text-anchor="middle" dy="0"></text>',
         '<text class="s" x="95" y="366" text-anchor="middle"></text>']
    b[-3] = '<text class="s" x="95" y="348" text-anchor="middle">asks Apple / BeaconDB only for APs</text>'
    b[-2] = ''
    b[-1] = '<text class="s" x="95" y="362" text-anchor="middle">it can\'t place itself</text>'
    return svg(w, h + 8, "".join(b), "How the BeaconFix pieces talk",
               "Nodes reach the phone over BLE and the desktop over UDP; the phone syncs with the desktop on the LAN; "
               "the desktop and the phone sync with the optional hub over BFS3; the Pi agent is a remote radio; "
               "Lite is the locator as a library.")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, fn in (("ap-accuracy", ap_accuracy), ("phone-idle", phone_idle), ("lite-cost", lite_cost), ("architecture", architecture)):
        path = os.path.join(OUT, name + ".svg")
        with open(path, "w") as f:
            f.write(fn())
        print("wrote", os.path.relpath(path))
