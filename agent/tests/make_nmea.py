#!/usr/bin/env python3
"""Synthetic NMEA for gpsfake: park, drive, park, with realistic correlated GNSS error.
Writes <out>.nmea and <out>.truth.json. Usage: make_nmea.py OUT [--seed N]"""
import argparse
import datetime as dt
import json
import math
import random

M = 111320.0


def cks(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return "$%s*%02X" % (body, c)


def dm(v, lat):
    a = abs(v)
    d = int(a)
    m = (a - d) * 60
    return ("%02d%09.6f" % (d, m) if lat else "%03d%09.6f" % (d, m)), ("N" if v >= 0 else "S") if lat else ("E" if v >= 0 else "W")


class OU:
    def __init__(self, rng, sigma, tau):
        self.rng, self.s, self.t, self.x = rng, sigma, tau, rng.gauss(0, sigma)

    def step(self, dtt=1.0):
        a = math.exp(-dtt / self.t)
        self.x = a * self.x + math.sqrt(1 - a * a) * self.s * self.rng.gauss(0, 1)
        return self.x


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--park1", type=int, default=45 * 60)
    ap.add_argument("--drive", type=int, default=6 * 60)
    ap.add_argument("--park2", type=int, default=20 * 60)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    lat0, lon0, alt0 = 40.0000000, -75.0000000, 120.0
    k = M * math.cos(math.radians(lat0))
    err = [[OU(rng, 2.0, 60.0), OU(rng, 1.0, 1200.0)] for _ in range(3)]
    bias = [rng.gauss(0, 0.3) for _ in range(3)]
    t0 = dt.datetime(2026, 9, 27, 4, 0, 0, tzinfo=dt.timezone.utc)
    v, brg = 12.0, math.radians(70.0)
    # truth: east/north of the vehicle over time
    total = a.park1 + a.drive + a.park2
    truth = []
    for s in range(total):
        if s < a.park1:
            e, n, spd = 0.0, 0.0, 0.0
        elif s < a.park1 + a.drive:
            d = v * (s - a.park1)
            e, n, spd = d * math.sin(brg), d * math.cos(brg), v
        else:
            d = v * a.drive
            e, n, spd = d * math.sin(brg), d * math.cos(brg), 0.0
        truth.append((e, n, spd))
    prns = [2, 5, 7, 9, 13, 15, 18, 20, 29]
    lines = []
    for s, (e, n, spd) in enumerate(truth):
        ee = [err[i][0].step() + err[i][1].step() + bias[i] + rng.gauss(0, 0.3) for i in range(3)]
        lat = lat0 + (n + ee[1]) / M
        lon = lon0 + (e + ee[0]) / k
        alt = alt0 + 2.0 * ee[2]
        t = t0 + dt.timedelta(seconds=s)
        hms = t.strftime("%H%M%S") + ".00"
        la, ns = dm(lat, True)
        lo, ew = dm(lon, False)
        rs = spd + abs(rng.gauss(0, 0.05)) if spd == 0 else spd + rng.gauss(0, 0.1)
        course = math.degrees(brg) if spd > 0 else rng.uniform(0, 360)
        hdop = 0.9 + 0.1 * math.sin(s / 600.0)
        lines.append(cks("GPGGA,%s,%s,%s,%s,%s,1,%02d,%.1f,%.1f,M,-33.0,M,," % (hms, la, ns, lo, ew, len(prns), hdop, alt)))
        lines.append(cks("GPGSA,A,3,%s,1.6,%.1f,1.3" % (",".join("%02d" % p for p in prns) + "," * (12 - len(prns)), hdop)))
        for i in range(3):
            chunk = prns[i * 4:(i + 1) * 4]
            sv = ",".join("%02d,%02d,%03d,%02d" % (p, 20 + 7 * j, (40 * p) % 360, 35 + j) for j, p in enumerate(chunk))
            lines.append(cks("GPGSV,3,%d,%02d,%s" % (i + 1, len(prns), sv)))
        lines.append(cks("GPRMC,%s,A,%s,%s,%s,%s,%.2f,%.1f,%s,,,A" % (hms, la, ns, lo, ew, rs / 0.514444, course, t.strftime("%d%m%y"))))
    with open(a.out + ".nmea", "w") as f:
        f.write("\r\n".join(lines) + "\r\n")
    segs = [{"name": "park1", "start": t0.timestamp(), "end": t0.timestamp() + a.park1, "lat": lat0, "lon": lon0},
            {"name": "drive", "start": t0.timestamp() + a.park1, "end": t0.timestamp() + a.park1 + a.drive},
            {"name": "park2", "start": t0.timestamp() + a.park1 + a.drive, "end": t0.timestamp() + total,
             "lat": lat0 + truth[-1][1] / M, "lon": lon0 + truth[-1][0] / k}]
    with open(a.out + ".truth.json", "w") as f:
        json.dump({"segments": segs, "biasENU": bias, "epochs": total}, f, indent=1)
    print("wrote %d epochs, %d sentences" % (total, len(lines)))


if __name__ == "__main__":
    main()
