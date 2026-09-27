#!/usr/bin/env bash
# Agent -> desktop end-to-end test against a THROW-AWAY BeaconFix instance: private D-Bus session,
# temporary config/state (its own map DB and key), API on port 47890, no mDNS, OS integration off,
# known-device allowlist off (a fresh instance knows no devices).
# The live tray and its data are never touched. A parked GNSS log is looped through gpsfake.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BEACONFIX:-$HOME/.local/bin/beaconfix}"
T="${WORK:-$(mktemp -d)}"; mkdir -p "$T"
PORT=47890; GPORT=29472
export XDG_CONFIG_HOME="$T/config" XDG_STATE_HOME="$T/state" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache" XDG_RUNTIME_DIR="$T/run"
mkdir -p "$XDG_CONFIG_HOME/sworrl" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
cat > "$XDG_CONFIG_HOME/sworrl/beaconfix.conf" <<EOF
[General]
apiEnabled=true
apiPort=$PORT
osTimeZone=false
osGeoclue=false
osNightLight=false
osLocale=false
liveScanSeconds=0
prefetchTiles=false
apiKnownOnly=false
EOF
dbus-daemon --session --address="unix:path=$T/bus" --fork --print-pid=1 --nopidfile >"$T/dbus.pid"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$T/bus"
cleanup() {
  [ -n "${AGENT:-}" ] && kill "$AGENT" 2>/dev/null || true
  [ -n "${FAKE:-}" ] && kill "$FAKE" 2>/dev/null || true
  [ -n "${TRAY:-}" ] && kill "$TRAY" 2>/dev/null || true
  kill "$(cat "$T/dbus.pid")" 2>/dev/null || true
}
trap cleanup EXIT
BEACONFIX_NO_MDNS=1 QT_QPA_PLATFORM=offscreen "$BIN" --tray --no-mdns >"$T/tray.log" 2>&1 &
TRAY=$!
for _ in $(seq 1 60); do curl -s -m 2 "http://127.0.0.1:$PORT/api/v1/hello" >/dev/null && break; sleep 0.5; done
curl -s -m 3 "http://127.0.0.1:$PORT/api/v1/hello"; echo
"$BIN" --token pi-test --control 2>/dev/null | tail -1 >"$T/token"; chmod 600 "$T/token"
[ -s "$T/token" ] || { echo "FAIL: no token from the throw-away instance"; exit 1; }
# a parked GNSS feed, looped, at ~1 fix/s
python3 "$HERE/make_nmea.py" "$T/parked" --park1 900 --drive 1 --park2 1 >/dev/null
mkdir -p "$T/bin"; cp "$(command -v gpsd || echo /usr/sbin/gpsd)" "$T/bin/gpsd"; export GPSD_HOME="$T/bin"
gpsfake -q -P "$GPORT" -c 0.17 "$T/parked.nmea" >"$T/gpsfake.log" 2>&1 &
FAKE=$!
sleep 2
python3 "$HERE/../beaconfix-agent" --config /dev/null --desktop "http://127.0.0.1:$PORT" --token-file "$T/token" --gpsd "127.0.0.1:$GPORT" \
  --state-dir "$T/agent-state" --run-dir "$T/agent-run" --device pi-test --no-ble --fast --cycles "${CYCLES:-150}" -v >"$T/agent.log" 2>&1 &
AGENT=$!
wait "$AGENT" || true
AGENT=""
TOK=$(cat "$T/token")
echo "--- agent status";      python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); a=s.get("average") or {}; print(json.dumps({"state": s["state"], "avg": a and {k: a[k] for k in ("lat","lon","sigmaH","nEff","durationS")}, "wifi": s["wifi"], "desktop": s["desktop"], "queue": s["queue"], "stats": s["stats"]}, indent=1))' "$T/agent-run/status.json"
echo "--- desktop db stats";  "$BIN" --db-stats 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print({k: d.get(k) for k in ("observations","fixes","aps","observingDevices","seq")})'
echo "--- desktop sees the Pi"; curl -s -m 5 -H "Authorization: Bearer $TOK" "http://127.0.0.1:$PORT/api/v1/devices/positions" | python3 -c 'import json,sys; d=json.load(sys.stdin); [print(" ", x.get("device"), x.get("kind"), x.get("lat"), x.get("lon"), "acc", x.get("acc"), "source", x.get("source")) for x in d.get("devices", [])]'
echo "--- synced rows from the Pi"; curl -s -m 5 -H "Authorization: Bearer $TOK" "http://127.0.0.1:$PORT/api/v1/db/changes?since=0&limit=5000" | python3 -c '
import json,sys; d=json.load(sys.stdin)
obs=[o for o in d.get("observations",[]) if o.get("device")=="pi-test"]; fx=[f for f in d.get("fixes",[]) if f.get("device")=="pi-test"]
print("  observations from pi-test:", len(obs), " fixes:", len(fx))
if obs: print("  e.g.", {k: obs[0].get(k) for k in ("bssid","time","lat","lon","acc","dbm","device")})
sys.exit(0 if obs and fx else 3)'
RC=$?
echo "--- anchors endpoint"; curl -s -m 5 -o /dev/null -w "  GET /api/v1/anchors -> HTTP %{http_code}\n" -H "Authorization: Bearer $TOK" "http://127.0.0.1:$PORT/api/v1/anchors"
grep -E "desktop:|sync:|anchor|ERROR|Traceback" "$T/agent.log" | head -12
[ "$RC" = 0 ] && echo PASS || echo FAIL
exit "$RC"
