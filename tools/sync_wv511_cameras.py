#!/usr/bin/env python3
"""
Sync live public traffic cameras from WV511 into BeaconFix map database.
"""
import urllib.request
import re
import json
import sqlite3
import os
import datetime
import sys

def sync_wv511():
    db_path = os.path.expandvars('$XDG_RUNTIME_DIR/beaconfix/live.db')
    if not os.path.exists(db_path):
        print(f"Error: live.db not found at {db_path}", file=sys.stderr)
        return False

    url = "https://wv511.org/wsvc/gmap.asmx/buildCamerasJSONjs"
    req = urllib.request.Request(url, headers={
        "Referer": "https://wv511.org/webmapi.aspx",
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64)"
    })
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            html = resp.read().decode("utf-8", "ignore")
    except Exception as e:
        print(f"Failed to fetch WV511 cameras: {e}", file=sys.stderr)
        return False

    m = re.search(r'camera_data\s*=\s*({.*?});?\s*$', html, re.DOTALL)
    if not m:
        print("Failed to parse WV511 camera_data JSON", file=sys.stderr)
        return False

    data = json.loads(m.group(1))
    cams = data.get("cams", [])
    print(f"Syncing {len(cams)} WV511 cameras into {db_path}...")

    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    now_iso = datetime.datetime.now().isoformat()
    count = 0

    for c in cams:
        cid = c.get("md5")
        if not cid:
            continue
        row_id = f"wv511:{cid}"
        try:
            lat = float(c.get("start_lat", 0.0))
            lon = float(c.get("start_lng", 0.0))
        except ValueError:
            continue
        if lat == 0.0 or lon == 0.0:
            continue

        title = c.get("title", "")
        tags = json.dumps({
            "contact:webcam": f"https://wv511.org/webmapi.aspx?CAMID={cid}",
            "ref": cid,
            "operator": "West Virginia Department of Transportation",
            "title": title,
            "surveillance:type": "camera",
            "camera_type": "webcam"
        })

        cur.execute("""
            INSERT INTO flock_cameras (id, lat, lon, source, model, operator, confidence, detection_method, first_seen, last_seen, camera_type, tags, vetted)
            VALUES (?, ?, ?, 'wv511', 'CAMERA', 'West Virginia Department of Transportation', 100, 'dot_feed', ?, ?, 'webcam', ?, 1)
            ON CONFLICT(id) DO UPDATE SET
                lat = excluded.lat,
                lon = excluded.lon,
                last_seen = excluded.last_seen,
                tags = excluded.tags,
                camera_type = 'webcam'
        """, (row_id, lat, lon, now_iso, now_iso, tags))
        count += 1

    conn.commit()
    conn.close()
    print(f"Successfully synced {count} WV511 cameras.")
    return True

if __name__ == "__main__":
    sync_wv511()
