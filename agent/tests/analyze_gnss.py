#!/usr/bin/env python3
"""Compare beaconfix-agent --gnss-only output with the truth written by make_nmea.py."""
import json
import math
import sys

M = 111320.0


def main():
    rep, truth = sys.argv[1], json.load(open(sys.argv[2]))
    rows = [json.loads(line) for line in open(rep) if line.strip()]
    segs = {s["name"]: s for s in truth["segments"]}
    out = {"epochs": len(rows)}
    ok = True
    for name in ("park1", "park2"):
        s = segs[name]
        inside = [r for r in rows if s["start"] <= r["t"] < s["end"] and r["avg"]]
        if not inside:
            out[name] = "no average"
            ok = False
            continue
        last = inside[-1]["avg"]
        k = M * math.cos(math.radians(s["lat"]))
        de = (last["lon"] - s["lon"]) * k
        dn = (last["lat"] - s["lat"]) * M
        err = math.hypot(de, dn)
        first_avg = inside[0]["t"] - s["start"]
        out[name] = {"durationMin": round((s["end"] - s["start"]) / 60), "startedAfterS": round(first_avg), "errE": round(de, 2), "errN": round(dn, 2),
                     "errH": round(err, 2), "sigmaE": round(last["sigmaE"], 2), "sigmaN": round(last["sigmaN"], 2), "sigmaH": round(last["sigmaH"], 2),
                     "nEff": round(last["nEff"], 1), "tauIntS": last["tauIntS"] and round(last["tauIntS"]),
                     "within2sigma": abs(de) <= 2 * last["sigmaE"] and abs(dn) <= 2 * last["sigmaN"]}
        ok = ok and out[name]["within2sigma"]
    d = segs["drive"]
    moving = [r["t"] for r in rows if r["t"] >= d["start"] and r["state"] == "moving"]
    out["movingDetectedAfterS"] = round(moving[0] - d["start"], 1) if moving else None
    ok = ok and moving and moving[0] - d["start"] <= 10
    print(json.dumps(out, indent=1))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
