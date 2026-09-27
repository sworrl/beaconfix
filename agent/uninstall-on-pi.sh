#!/usr/bin/env bash
# Remove beaconfix-agent from a Pi FROM this desktop:   agent/uninstall-on-pi.sh [--keep-data] <user>@<host>
# Stops and removes the service, the program, its config (and token) and, unless --keep-data, its queue;
# revokes the Pi's token on this desktop. Debian packages (gpsd, bluez, ...) are left installed.
set -euo pipefail
KEEP=0; TARGET=""
while [ $# -gt 0 ]; do case "$1" in --keep-data) KEEP=1; shift ;; *) TARGET="$1"; shift ;; esac; done
[ -n "$TARGET" ] || { echo "usage: $0 [--keep-data] <user>@<host>" >&2; exit 2; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 "$TARGET")
NAME=$("${SSH[@]}" hostname)
"${SSH[@]}" "KEEP=$KEEP bash -s" <<'REMOTE'
set -e
sudo systemctl disable --now beaconfix-agent >/dev/null 2>&1 || true
sudo rm -f /etc/systemd/system/beaconfix-agent.service /usr/local/bin/beaconfix-agent
sudo rm -rf /usr/local/lib/beaconfix-agent /etc/beaconfix-agent
[ "$KEEP" = 1 ] || sudo rm -rf /var/lib/beaconfix-agent
sudo systemctl daemon-reload
id beaconfix-agent >/dev/null 2>&1 && sudo deluser --system beaconfix-agent >/dev/null 2>&1 || true
echo "removed beaconfix-agent from $(hostname)"
REMOTE
command -v beaconfix >/dev/null || PATH="$HOME/.local/bin:$PATH"
beaconfix --revoke "$NAME" >/dev/null 2>&1 && echo "revoked the desktop token for $NAME" || true
