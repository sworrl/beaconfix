#!/usr/bin/env bash
# Install beaconfix-agent on a Raspberry Pi FROM this desktop:   agent/install-on-pi.sh [options] <user>@<host>
#   --desktop URL          the BeaconFix desktop the Pi reports to (default: this machine's LAN address, port 47822)
#   --configure-uart-pps   ONLY if gpsd sees no GPS: enable the Pi's UART and the PPS overlay, free the serial
#                          console, then reboot (prints every change first; nothing touches /boot otherwise)
#   --keep-token           reuse the token already on the Pi instead of minting a new one
#   --no-apt               skip package installation
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DESKTOP=""; UART=0; KEEP=0; APT=1; TARGET=""
while [ $# -gt 0 ]; do
  case "$1" in
    --desktop) DESKTOP="$2"; shift 2 ;;
    --configure-uart-pps) UART=1; shift ;;
    --keep-token) KEEP=1; shift ;;
    --no-apt) APT=0; shift ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) TARGET="$1"; shift ;;
  esac
done
[ -n "$TARGET" ] || { echo "usage: $0 [options] <user>@<host>" >&2; exit 2; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new "$TARGET")
say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

say "Checking SSH to $TARGET"
"${SSH[@]}" true || die "cannot SSH to $TARGET with a key (try: ssh-copy-id -i ~/.ssh/id_ed25519.pub $TARGET)"
"${SSH[@]}" 'sudo -n true' 2>/dev/null || die "passwordless sudo is not available on $TARGET for $(echo "$TARGET" | cut -d@ -f1); run it once interactively or add a sudoers rule"
"${SSH[@]}" 'cat /proc/device-tree/model 2>/dev/null | tr -d "\0"; echo; . /etc/os-release; echo "$PRETTY_NAME"' | sed 's/^/    /'

if [ -z "$DESKTOP" ]; then
  PI_IP=$("${SSH[@]}" 'hostname -I' | awk '{print $1}')
  SRC=$(ip -4 route get "$PI_IP" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
  [ -n "$SRC" ] || die "cannot work out this desktop's address towards the Pi; pass --desktop http://<desktop>:47822"
  DESKTOP="http://$SRC:47822"
fi
say "Desktop API: $DESKTOP"

if [ "$APT" = 1 ]; then
  say "Installing packages (gpsd, gpsd-clients, python3-gps, python3-dbus, python3-gi, bluez, avahi-utils, iw, ethtool, i2c-tools)"
  "${SSH[@]}" 'sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gpsd gpsd-clients python3-gps python3-dbus python3-gi bluez avahi-utils iw ethtool i2c-tools >/dev/null' \
    || die "package installation failed"
fi

say "Checking that gpsd sees the GPS"
"${SSH[@]}" 'bash -s' <<'REMOTE'
set -u
f=/etc/default/gpsd
cur=$(sed -n 's/^DEVICES="\(.*\)"/\1/p' "$f" 2>/dev/null)
echo "    /etc/default/gpsd DEVICES=\"$cur\""
if [ -z "$cur" ]; then
  dev=""
  for d in /dev/serial0 /dev/ttyAMA0 /dev/ttyS0 /dev/ttyACM0 /dev/ttyUSB0; do [ -e "$d" ] && { dev="$d"; break; }; done
  pps=""; [ -e /dev/pps0 ] && pps=" /dev/pps0"
  if [ -n "$dev" ]; then
    sudo cp -a "$f" "$f.beaconfix-backup" 2>/dev/null || true
    echo "    setting DEVICES=\"$dev$pps\" GPSD_OPTIONS=\"-n\" (backup: $f.beaconfix-backup)"
    sudo sed -i "s|^DEVICES=.*|DEVICES=\"$dev$pps\"|; s|^GPSD_OPTIONS=.*|GPSD_OPTIONS=\"-n\"|" "$f"
    grep -q '^DEVICES=' "$f" || echo "DEVICES=\"$dev$pps\"" | sudo tee -a "$f" >/dev/null
  fi
fi
sudo systemctl enable --now gpsd.socket >/dev/null 2>&1 || true
sudo systemctl restart gpsd >/dev/null 2>&1 || true
sleep 3
out=$(timeout 25 gpspipe -w -n 25 2>/dev/null)
devs=$(printf '%s\n' "$out" | grep -o '"class":"DEVICE"[^}]*"path":"[^"]*"' | sed 's/.*"path":"\([^"]*\)"/\1/' | sort -u | tr '\n' ' ')
mode=$(printf '%s\n' "$out" | grep -o '"class":"TPV"[^}]*"mode":[0-9]' | sed 's/.*"mode"://' | sort -n | tail -1)
echo "    gpsd devices: ${devs:-none}   best TPV mode: ${mode:-none} (2 = 2D, 3 = 3D)"
if grep -qE 'console=(serial0|ttyAMA0|ttyS0)' /boot/firmware/cmdline.txt /boot/cmdline.txt 2>/dev/null; then
  echo "    WARNING: the Linux serial console is on the GPS UART (cmdline.txt); gpsd will read garbage until it is removed (--configure-uart-pps)"
fi
[ -e /dev/pps0 ] && echo "    PPS: /dev/pps0 present" || echo "    PPS: no /dev/pps0 (the PPS overlay is not loaded)"
[ -n "$devs" ] && exit 0 || exit 3
REMOTE
GPS_RC=$?
if [ "$GPS_RC" -ne 0 ]; then
  if [ "$UART" = 1 ]; then
    say "--configure-uart-pps: these changes will be made on $TARGET, then it reboots:"
    cat <<'PLAN'
    /boot/firmware/config.txt (or /boot/config.txt): add under [all]
        enable_uart=1
        dtoverlay=pps-gpio,gpiopin=4
    /boot/firmware/cmdline.txt: remove console=serial0,115200 (and console=ttyAMA0/ttyS0)
    systemctl disable --now serial-getty@ttyS0 serial-getty@ttyAMA0 serial-getty@serial0
    /etc/default/gpsd: DEVICES="/dev/serial0 /dev/pps0"  GPSD_OPTIONS="-n"
PLAN
    "${SSH[@]}" 'bash -s' <<'REMOTE'
set -e
cfg=/boot/firmware/config.txt; [ -f "$cfg" ] || cfg=/boot/config.txt
cmd=/boot/firmware/cmdline.txt; [ -f "$cmd" ] || cmd=/boot/cmdline.txt
sudo cp -a "$cfg" "$cfg.beaconfix-backup"; sudo cp -a "$cmd" "$cmd.beaconfix-backup"
grep -q '^enable_uart=1' "$cfg" || printf '\n[all]\nenable_uart=1\n' | sudo tee -a "$cfg" >/dev/null
grep -q '^dtoverlay=pps-gpio' "$cfg" || echo 'dtoverlay=pps-gpio,gpiopin=4' | sudo tee -a "$cfg" >/dev/null
sudo sed -i -E 's/ ?console=(serial0|ttyAMA0|ttyS0),[0-9]+//g' "$cmd"
for u in serial-getty@ttyS0 serial-getty@ttyAMA0 serial-getty@serial0; do sudo systemctl disable --now "$u" >/dev/null 2>&1 || true; done
sudo sed -i 's|^DEVICES=.*|DEVICES="/dev/serial0 /dev/pps0"|; s|^GPSD_OPTIONS=.*|GPSD_OPTIONS="-n"|' /etc/default/gpsd
REMOTE
  else
    say "gpsd sees no GPS yet. The agent is installed anyway and waits for it; if the HAT is on the Pi's UART,"
    echo "    re-run with --configure-uart-pps (prints every change first), or see docs/AGENT.md 'GPS HAT'."
  fi
fi

say "Installing the agent"
scp -q -o BatchMode=yes "$HERE/beaconfix-agent" "$HERE/beaconfix-agent.service" "$HERE/agent.conf.example" "$TARGET:/tmp/"
"${SSH[@]}" "DESKTOP='$DESKTOP' bash -s" <<'REMOTE'
set -e
id beaconfix-agent >/dev/null 2>&1 || sudo adduser --system --group --no-create-home --home /var/lib/beaconfix-agent beaconfix-agent >/dev/null
for g in bluetooth video i2c netdev gpio; do getent group "$g" >/dev/null && sudo usermod -aG "$g" beaconfix-agent; done
sudo install -D -m 0755 /tmp/beaconfix-agent /usr/local/lib/beaconfix-agent/beaconfix-agent
sudo ln -sf /usr/local/lib/beaconfix-agent/beaconfix-agent /usr/local/bin/beaconfix-agent
sudo install -m 0644 /tmp/beaconfix-agent.service /etc/systemd/system/beaconfix-agent.service
sudo install -d -m 0750 -o root -g beaconfix-agent /etc/beaconfix-agent
if [ ! -f /etc/beaconfix-agent/agent.conf ]; then
  sed "s|^url =\$|url = $DESKTOP|; s|^# device = .*|device = $(hostname)|; s|^# name defaults.*|name = $(hostname) GPS antenna|" /tmp/agent.conf.example | sudo install -m 0640 -o root -g beaconfix-agent /dev/stdin /etc/beaconfix-agent/agent.conf
fi
rm -f /tmp/beaconfix-agent /tmp/beaconfix-agent.service /tmp/agent.conf.example
REMOTE

if [ "$KEEP" = 1 ] && "${SSH[@]}" 'sudo test -s /etc/beaconfix-agent/token'; then
  say "Keeping the existing token"
else
  say "Minting a token on this desktop and copying it to the Pi (never printed)"
  NAME=$("${SSH[@]}" hostname)
  command -v beaconfix >/dev/null || PATH="$HOME/.local/bin:$PATH"
  beaconfix --revoke "$NAME" >/dev/null 2>&1 || true
  TOKEN=$(beaconfix --token "$NAME" --control 2>/dev/null | tail -1)
  [ -n "$TOKEN" ] && [ "${#TOKEN}" -ge 20 ] || die "could not mint a token (is the BeaconFix tray running here?)"
  printf '%s\n' "$TOKEN" | "${SSH[@]}" 'sudo sh -c "umask 027; cat > /etc/beaconfix-agent/token.new && chown root:beaconfix-agent /etc/beaconfix-agent/token.new && chmod 0640 /etc/beaconfix-agent/token.new && mv /etc/beaconfix-agent/token.new /etc/beaconfix-agent/token"'
  unset TOKEN
  # the desktop only accepts tokens from its known devices: make sure this Pi's interfaces are known
  for mac in $("${SSH[@]}" "ip -br link | awk '\$1 ~ /^(eth|end|wlan)/ {print \$3}'"); do
    beaconfix --known-list 2>/dev/null | grep -qi "$mac" || beaconfix --known-add "$mac" --known-name "$NAME" >/dev/null 2>&1 || true
  done
fi

say "Starting the service"
"${SSH[@]}" 'sudo systemctl daemon-reload && sudo systemctl enable --now beaconfix-agent >/dev/null 2>&1 && sudo systemctl restart beaconfix-agent'
if [ "$GPS_RC" -ne 0 ] && [ "$UART" = 1 ]; then
  say "Rebooting $TARGET to load the UART/PPS changes"
  "${SSH[@]}" 'sudo systemctl reboot' || true
  exit 0
fi
sleep 25
say "Status"
"${SSH[@]}" 'bash -s' <<'REMOTE'
systemctl is-active beaconfix-agent | sed 's/^/    service: /'
beaconfix-agent --status 2>/dev/null | python3 -c '
import json, sys
try: s = json.load(sys.stdin)
except ValueError: print("    (no status yet)"); sys.exit()
g = s.get("gps") or {}; a = s.get("average") or {}; d = s.get("desktop") or {}
print("    gpsd: connected=%s mode=%s sats=%s hdop=%s pps=%s" % (g.get("connected"), g.get("mode"), (g.get("sky") or {}).get("used"), (g.get("sky") or {}).get("hdop"), g.get("pps")))
print("    state: %s   average: %s" % (s.get("state"), ("%.7f, %.7f  sigmaH %.2f m  Neff %.1f over %.0f s" % (a["lat"], a["lon"], a["sigmaH"], a["nEff"], a["durationS"])) if a else "none yet"))
print("    wifi: %s   ble: %s" % (s.get("wifi"), s.get("ble")))
print("    desktop: %s  last ok %s  %s" % (d.get("url"), d.get("lastOk"), d.get("error") or ""))
print("    queue: %s   stats: %s" % (s.get("queue"), s.get("stats")))
'
vcgencmd get_throttled 2>/dev/null | sed 's/^/    power: /'
journalctl -u beaconfix-agent -n 8 --no-pager 2>/dev/null | sed 's/^/    | /'
REMOTE
