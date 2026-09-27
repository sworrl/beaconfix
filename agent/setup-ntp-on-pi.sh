#!/usr/bin/env bash
# Make a Raspberry Pi with a GPS HAT the LAN's stratum-1 NTP server (chrony), FROM this desktop:
#   agent/setup-ntp-on-pi.sh [--dry-run] [--allow CIDR,...] [--nmea-offset SECONDS] [--configure-pps] <user>@<host>
#
#   --allow CIDR,...   networks allowed to query it (default: the Pi's directly connected subnets)
#   --dry-run          only inventory the Pi and print the chrony.conf it would install
#   --nmea-offset S    constant delay of the NMEA sentence after the second (default: reuse a value from the
#                      old ntp/chrony config, else 0.2 s, typical for a 9600-baud MTK/ublox HAT)
#   --configure-pps    if /dev/pps0 is missing: add dtoverlay=pps-gpio,gpiopin=4 and reboot (prints first)
#
# Result: chrony with gpsd's NMEA time (SHM 0) and, when the HAT's PPS line is wired, PPS locked to it
# (stratum 1); internet pool servers as backup; "local stratum 10 orphan" so the LAN keeps one consistent
# time when both GPS and internet are gone; allow the given subnets; systemd-timesyncd and any other NTP
# daemon disabled so nothing else binds UDP 123. Prints tracking, sources and a few PPS pulses.
set -euo pipefail
DRY=0; OFF=""; PPSCFG=0; ALLOW=""; TARGET=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1; shift ;;
    --nmea-offset) OFF="$2"; shift 2 ;;
    --allow) ALLOW="$2"; shift 2 ;;
    --configure-pps) PPSCFG=1; shift ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) TARGET="$1"; shift ;;
  esac
done
[ -n "$TARGET" ] || { echo "usage: $0 [--dry-run] [--allow CIDR,...] [--nmea-offset S] [--configure-pps] <user>@<host>" >&2; exit 2; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8 "$TARGET")
"${SSH[@]}" true || { echo "cannot SSH to $TARGET with a key (ssh-copy-id -i ~/.ssh/id_ed25519.pub $TARGET)" >&2; exit 1; }
"${SSH[@]}" "DRY=$DRY OFF='$OFF' PPSCFG=$PPSCFG ALLOW='$ALLOW' bash -s" <<'REMOTE'
set -u
say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
SUDO=sudo; [ "$DRY" = 1 ] && SUDO="echo would:"
sudo -n true 2>/dev/null || { echo "passwordless sudo is required on $(hostname)"; exit 1; }

say "Inventory on $(hostname) ($(tr -d '\0' </proc/device-tree/model 2>/dev/null))"
for u in chrony ntp ntpsec openntpd systemd-timesyncd gpsd; do
  printf '    %-18s %s\n' "$u" "$(systemctl is-active "$u" 2>/dev/null)/$(systemctl is-enabled "$u" 2>/dev/null)"
done
echo "    UDP 123 owners: $(sudo ss -ulpnH 'sport = :123' 2>/dev/null | sed -E 's/.*users:\(\("([^"]+)".*/\1/' | sort -u | tr '\n' ' ')"
echo "    gpsd: $(sed -n 's/^\(DEVICES\|GPSD_OPTIONS\)=/\1=/p' /etc/default/gpsd 2>/dev/null | tr '\n' ' ')"
echo "    PPS device: $(ls /dev/pps* 2>/dev/null | tr '\n' ' ' || true)"
cfg=/boot/firmware/config.txt; [ -f "$cfg" ] || cfg=/boot/config.txt
echo "    $cfg: $(grep -E '^(dtoverlay=pps|enable_uart|dtoverlay=(disable-bt|miniuart-bt))' "$cfg" 2>/dev/null | tr '\n' ' ')"
for f in /etc/chrony/chrony.conf /etc/chrony/conf.d/*.conf /etc/ntpsec/ntp.conf /etc/ntp.conf; do
  [ -f "$f" ] && grep -nE '^\s*(refclock|server +127\.127|fudge|pool|server|allow|local|makestep)' "$f" 2>/dev/null | sed "s|^|    $f:|" | head -12
done
if command -v chronyc >/dev/null; then chronyc -n tracking 2>/dev/null | sed -n '1,4p' | sed 's/^/    tracking: /'; fi

# reuse a measured NMEA offset from the old configuration when there is one
if [ -z "$OFF" ]; then
  OFF=$(grep -hE '^\s*refclock\s+(SHM\s+0|SOCK).*offset' /etc/chrony/chrony.conf /etc/chrony/conf.d/*.conf 2>/dev/null | sed -nE 's/.*offset +(-?[0-9.]+).*/\1/p' | head -1)
  [ -z "$OFF" ] && OFF=$(grep -hE '^\s*fudge\s+127\.127\.(28|46)\.0.*time1' /etc/ntpsec/ntp.conf /etc/ntp.conf 2>/dev/null | sed -nE 's/.*time1 +(-?[0-9.]+).*/\1/p' | head -1)
  [ -z "$OFF" ] && OFF=0.2
fi
PPS=0; [ -e /dev/pps0 ] && PPS=1
if [ -z "$ALLOW" ]; then ALLOW=$(ip -4 route show proto kernel scope link | awk '{print $1}' | sort -u | paste -sd, -); fi
ALLOWLINES=$(printf '%s\n' "$ALLOW" | tr ',' '\n' | sed '/^$/d; s/^/allow /')

if [ "$PPS" = 0 ] && [ "$PPSCFG" = 1 ]; then
  say "No /dev/pps0: adding the PPS overlay to $cfg and rebooting (backup: $cfg.beaconfix-backup)"
  $SUDO cp -a "$cfg" "$cfg.beaconfix-backup"
  if [ "$DRY" = 0 ]; then grep -q '^dtoverlay=pps-gpio' "$cfg" || printf '\n[all]\ndtoverlay=pps-gpio,gpiopin=4\n' | sudo tee -a "$cfg" >/dev/null; fi
  echo "    re-run this script after the reboot"; $SUDO systemctl reboot; exit 0
fi

say "Installing chrony and pps-tools"
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq chrony pps-tools gpsd >/dev/null

CONF=$(cat <<EOF
# /etc/chrony/chrony.conf — written by beaconfix setup-ntp-on-pi.sh on $(date -u +%Y-%m-%dT%H:%MZ)
# $(hostname): stratum-1 time server. GPS (gpsd) + PPS when wired; internet pool as backup;
# orphan mode keeps the LAN consistent when both GPS and internet are gone.

# gpsd writes the GPS's NMEA time to shared memory segment 0. Its constant delay after the second
# (sentence length / baud rate + receiver latency) is compensated by "offset".
EOF
)
if [ "$PPS" = 1 ]; then
  CONF="$CONF
refclock SHM 0 refid NMEA offset $OFF delay 0.2 precision 1e-1 poll 3 noselect
# The HAT's PPS edge (GPIO 4, dtoverlay=pps-gpio): sub-microsecond, numbered by the NMEA seconds.
refclock PPS /dev/pps0 refid PPS lock NMEA poll 3 precision 1e-7 prefer trust"
else
  CONF="$CONF
# No /dev/pps0: NMEA alone is good to tens of milliseconds, so it is published honestly as stratum 2
# (refclock stratum 1) and the internet pool is preferred whenever it is reachable.
refclock SHM 0 refid NMEA offset $OFF delay 0.5 precision 1e-1 poll 3 stratum 1"
fi
CONF="$CONF

pool 2.debian.pool.ntp.org iburst maxsources 4
server time.cloudflare.com iburst

# Networks allowed to query this server.
$ALLOWLINES

# Keep serving a consistent time to the LAN when every upstream is lost.
local stratum 10 orphan

makestep 1 3
rtcsync
driftfile /var/lib/chrony/chrony.drift
leapsectz right/UTC
logdir /var/log/chrony
log tracking statistics refclocks
confdir /etc/chrony/conf.d
sourcedir /etc/chrony/sources.d
ntsdumpdir /var/lib/chrony
keyfile /etc/chrony/chrony.keys"
say "chrony.conf to install (PPS: $([ "$PPS" = 1 ] && echo present || echo ABSENT), NMEA offset $OFF s)"
printf '%s\n' "$CONF" | sed 's/^/    /'
[ "$DRY" = 1 ] && { say "dry run: nothing changed"; exit 0; }

say "Stopping other time daemons (nothing else may bind UDP 123)"
for u in systemd-timesyncd ntp ntpsec openntpd; do
  systemctl list-unit-files "$u.service" >/dev/null 2>&1 && { sudo systemctl disable --now "$u" >/dev/null 2>&1 || true; }
done
sudo systemctl mask systemd-timesyncd >/dev/null 2>&1 || true

say "gpsd must poll the GPS without clients (-n) to feed chrony"
if ! grep -qE '^GPSD_OPTIONS=.*-n' /etc/default/gpsd; then
  sudo cp -a /etc/default/gpsd /etc/default/gpsd.beaconfix-backup
  if grep -q '^GPSD_OPTIONS=' /etc/default/gpsd; then sudo sed -i -E 's/^GPSD_OPTIONS="?([^"]*)"?/GPSD_OPTIONS="\1 -n"/' /etc/default/gpsd; else echo 'GPSD_OPTIONS="-n"' | sudo tee -a /etc/default/gpsd >/dev/null; fi
fi
sudo systemctl enable gpsd.socket gpsd >/dev/null 2>&1 || true

say "Installing chrony.conf (backup: /etc/chrony/chrony.conf.beaconfix-backup)"
[ -f /etc/chrony/chrony.conf ] && sudo cp -a /etc/chrony/chrony.conf /etc/chrony/chrony.conf.beaconfix-backup
printf '%s\n' "$CONF" | sudo tee /etc/chrony/chrony.conf >/dev/null
sudo chronyd -p -f /etc/chrony/chrony.conf >/dev/null 2>&1 || { echo "chronyd rejects the new config; restoring the backup"; sudo cp -a /etc/chrony/chrony.conf.beaconfix-backup /etc/chrony/chrony.conf; exit 1; }
if command -v ufw >/dev/null && sudo ufw status 2>/dev/null | grep -q 'Status: active'; then
  for n in $(printf '%s' "$ALLOW" | tr ',' ' '); do sudo ufw allow from "$n" to any port 123 proto udp >/dev/null; done
fi
sudo systemctl restart gpsd
sudo systemctl enable chrony >/dev/null 2>&1 || true
sudo systemctl restart chrony
say "Waiting 60 s for the first samples"
sleep 60
say "chronyc tracking";      chronyc -n tracking | sed 's/^/    /'
say "chronyc sources -v";    chronyc -n sources -v | sed 's/^/    /'
say "chronyc sourcestats";   chronyc -n sourcestats | sed 's/^/    /'
if [ "$PPS" = 1 ]; then say "PPS pulses (ppstest)"; sudo timeout 6 ppstest /dev/pps0 2>&1 | head -8 | sed 's/^/    /'; fi
say "UDP 123 is served by: $(sudo ss -ulpnH 'sport = :123' | sed -E 's/.*users:\(\("([^"]+)".*/\1/' | sort -u | tr '\n' ' ')"
REMOTE
