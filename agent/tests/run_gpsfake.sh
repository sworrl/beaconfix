#!/usr/bin/env bash
# End-to-end GNSS test: synthetic NMEA -> gpsfake (a real gpsd) -> beaconfix-agent --gnss-only -> analysis.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$(mktemp -d)}"
PORT="${PORT:-29470}"
python3 "$HERE/make_nmea.py" "$WORK/scenario" --seed "${SEED:-42}"
# Ubuntu's AppArmor profile for /usr/sbin/gpsd forbids the pseudo-terminal gpsfake feeds it through;
# gpsfake looks in $GPSD_HOME first, and a private copy of the binary is not confined.
mkdir -p "$WORK/bin" && cp "$(command -v gpsd || echo /usr/sbin/gpsd)" "$WORK/bin/gpsd"
export GPSD_HOME="$WORK/bin"
# gpsfake waits for its first client before feeding, so the agent must be that client (no port probe).
( sleep 1.5; exec python3 "$HERE/../beaconfix-agent" --gnss-only --gpsd "127.0.0.1:$PORT" --report "$WORK/report.jsonl" --config /dev/null >"$WORK/final.json" ) &
AGENT=$!
trap 'kill $AGENT 2>/dev/null || true' EXIT
gpsfake -1 -q -P "$PORT" -c "${CYCLE:-0.0005}" "$WORK/scenario.nmea" >"$WORK/gpsfake.log" 2>&1 || true
wait $AGENT || true
python3 "$HERE/analyze_gnss.py" "$WORK/report.jsonl" "$WORK/scenario.truth.json"
