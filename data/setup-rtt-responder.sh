#!/usr/bin/env bash
# BeaconFix: turn this computer's Wi-Fi card into an IEEE 802.11mc (FTM) responder so phones
# can measure their distance to it with Wi-Fi RTT (≈1 m accuracy instead of ±40 m fixes).
#
# It adds a second, AP-type interface (bfrtt0) on the same radio, runs hostapd on a hidden SSID
# with ftm_responder=1, and keeps NetworkManager off that interface so the normal Wi-Fi
# interface stays managed for scanning. Nobody can join the AP (random passphrase, empty MAC
# accept list); RTT does not need association.
#
#   setup-rtt-responder.sh [--band auto|2g|5g] [--channel N] [--country CC] [--txpower DBM] [--wlan IFACE] [--no-scan-check]
#   setup-rtt-responder.sh --status | --remove
#   (internal, used by the systemd unit) --iface-up | --iface-down
#
# Channel choice: 5 GHz at 80 MHz gives RTT ~4× the resolution of 2.4 GHz at 20 MHz. UNII-3
# (ch 149, 5735–5815 MHz) is preferred because home routers usually sit in UNII-1 (ch 36–48,
# i.e. inside the ch-36 80 MHz block), so the responder does not take airtime from the house
# Wi-Fi. If UNII-3 is not allowed here, ch 36 with a 300 TU beacon interval; if no 5 GHz
# channel may transmit, 2.4 GHz ch 1. TX power defaults to 12 dBm:
# FTM needs a clean exchange, not range; the phones that range to it are a few metres away.
#
# Idempotent: re-running reconfigures and restarts. See docs/RANGING.md §6.
set -euo pipefail

IFACE=bfrtt0
CONF_DIR=/etc/beaconfix
CONF=$CONF_DIR/rtt-responder.conf
STATE=$CONF_DIR/rtt-responder.state          # shell KEY=VALUE, root-only (has nothing secret, but is ours)
INFO=$CONF_DIR/rtt-responder.json            # world-readable: what the API's /ranging/info reports
PSKFILE=$CONF_DIR/rtt-responder.psk          # root-only
ACCEPT=$CONF_DIR/rtt-responder.accept        # empty MAC accept list: no clients, ever
UNIT=/etc/systemd/system/beaconfix-rtt-responder.service
HELPER=/usr/local/libexec/beaconfix-rtt-responder
NMCONF=/etc/NetworkManager/conf.d/90-beaconfix-rtt.conf
REGDOM=/etc/modprobe.d/beaconfix-regdom.conf          # cfg80211 boots into the right country, not "00"
UDEVRULE=/etc/udev/rules.d/90-beaconfix-rtt.rules      # start the responder whenever a radio appears
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE=install BAND=auto CHANNEL="" COUNTRY="" WLAN="" SCAN_CHECK=1 NO_SCAN_ARG="" TXPOWER=12
while [ $# -gt 0 ]; do
    case "$1" in
        --status) MODE=status ;;
        --remove) MODE=remove ;;
        --iface-up) MODE=iface-up ;;
        --iface-down) MODE=iface-down ;;
        --post-start) MODE=post-start ;;
        --txpower) TXPOWER="${2:?}"; shift ;;
        --band) BAND="${2:?}"; shift ;;
        --channel) CHANNEL="${2:?}"; shift ;;
        --country) COUNTRY="${2:?}"; shift ;;
        --wlan) WLAN="${2:?}"; shift ;;
        --no-scan-check) SCAN_CHECK=0; NO_SCAN_ARG=--no-scan-check ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$MODE" != status ] && [ "$(id -u)" -ne 0 ]; then
    exec sudo -E "$0" ${MODE:+$( [ "$MODE" = install ] || echo "--$MODE")} \
        ${BAND:+--band "$BAND"} ${CHANNEL:+--channel "$CHANNEL"} ${COUNTRY:+--country "$COUNTRY"} \
        ${WLAN:+--wlan "$WLAN"} --txpower "$TXPOWER" ${NO_SCAN_ARG:+"$NO_SCAN_ARG"}
fi

# shellcheck disable=SC1090
load_state() { if [ -r "$STATE" ]; then . "$STATE"; fi; }

# ── helpers used by the unit ─────────────────────────────────────────────────
# The radio's phy index is not stable (phy0 becomes phy1 after the card is re-attached or the
# driver reloads), so find it by the managed interface's MAC, then by name, then by capability,
# and wait for it: at boot the unit can start before the PCIe device or its firmware is up.
find_phy() {
    local want="${WLAN_MAC:-}" d
    if [ -z "$want" ] && [ -n "${MAC:-}" ]; then        # older state: our MAC minus the locally-administered bit
        want=$(printf '%02x%s' $(( 0x${MAC:0:2} & ~0x02 )) "${MAC:2}")
    fi
    if [ -n "$want" ]; then
        for d in /sys/class/net/*; do
            [ -e "$d/phy80211" ] || continue
            [ "$(basename "$d")" = "$IFACE" ] && continue
            [ "$(cat "$d/address" 2>/dev/null)" = "${want,,}" ] && { cat "$d/phy80211/name"; return 0; }
        done
    fi
    [ -n "${WLAN:-}" ] && [ -e "/sys/class/net/$WLAN/phy80211" ] && { cat "/sys/class/net/$WLAN/phy80211/name"; return 0; }
    for d in /sys/class/ieee80211/*; do
        [ -e "$d" ] || continue
        iw phy "$(basename "$d")" info 2>/dev/null | grep -q ENABLE_FTM_RESPONDER && { basename "$d"; return 0; }
    done
    return 1
}
iface_up() {
    load_state
    [ -n "${PHY:-}" ] || die "no state in $STATE; run the setup first"
    local phy=""
    for _ in $(seq 1 90); do phy=$(find_phy) && break; sleep 1; done
    [ -n "$phy" ] || die "no Wi-Fi radio appeared within 90 s (was $PHY); is the card attached?"
    [ "$phy" = "$PHY" ] || say "radio is $phy now (state said $PHY)"
    if [ -n "${COUNTRY_SET:-}" ] && [ "$COUNTRY_SET" != 00 ]; then
        iw reg set "$COUNTRY_SET" || true
        for _ in $(seq 1 20); do iw reg get 2>/dev/null | grep -q "country $COUNTRY_SET" && break; sleep 0.5; done
    fi
    if ! ip link show "$IFACE" >/dev/null 2>&1; then
        for _ in $(seq 1 10); do iw phy "$phy" interface add "$IFACE" type __ap 2>/dev/null && break; sleep 2; done
        ip link show "$IFACE" >/dev/null 2>&1 || die "could not add $IFACE on $phy"
    fi
    ip link set "$IFACE" down 2>/dev/null || true
    ip link set "$IFACE" address "$MAC"
    # A scan on the managed side (NetworkManager scans often while disconnected) makes the radio
    # EBUSY and hostapd then gives up (exiting 0). Abort it right before hostapd starts.
    local n
    for n in /sys/class/ieee80211/"$phy"/device/net/*; do
        [ -e "$n" ] && [ "$(basename "$n")" != "$IFACE" ] && { iw dev "$(basename "$n")" scan abort 2>/dev/null || true; }
    done
    return 0
}
post_start() {                                   # hostapd has no TX power option: set it once the AP is up
    load_state
    for _ in $(seq 1 20); do iw dev "$IFACE" info 2>/dev/null | grep -q 'channel' && break; sleep 0.5; done
    iw dev "$IFACE" set txpower fixed "$(( ${TXPOWER:-12} * 100 ))" 2>/dev/null || true
}
iface_down() {
    if ip link show "$IFACE" >/dev/null 2>&1; then iw dev "$IFACE" del || true; fi
}

status() {
    load_state
    echo "unit:      $(systemctl is-active beaconfix-rtt-responder.service 2>/dev/null || echo absent) / $(systemctl is-enabled beaconfix-rtt-responder.service 2>/dev/null || echo -)"
    echo "radio:     ${PHY:-?} (managed side: ${WLAN_STATE:-${WLAN:-?}})"
    if ip link show "$IFACE" >/dev/null 2>&1; then
        iw dev "$IFACE" info | sed -n 's/^\t\{1,\}//p' | grep -E '^(addr|type|channel|txpower)' | sed 's/^/  /'
    else
        echo "  $IFACE: absent"
    fi
    [ -r "$INFO" ] && echo "info:      $(cat "$INFO")"
    echo "NM:        $(nmcli -t -f DEVICE,STATE dev status 2>/dev/null | grep -E "^($IFACE|${WLAN:-wlp}):" | tr '\n' ' ')"
}

remove() {
    say "Stopping and removing the RTT responder"
    systemctl disable --now beaconfix-rtt-responder.service 2>/dev/null || true
    iface_down
    rm -f "$UNIT" "$HELPER" "$NMCONF" "$REGDOM" "$UDEVRULE" "$CONF" "$STATE" "$INFO" "$ACCEPT"
    rm -f "$PSKFILE"
    rmdir "$CONF_DIR" 2>/dev/null || true
    systemctl daemon-reload
    systemctl reload NetworkManager 2>/dev/null || true
    say "Removed (hostapd itself stays installed)."
}

case "$MODE" in
    iface-up) iface_up; exit 0 ;;
    iface-down) iface_down; exit 0 ;;
    post-start) post_start; exit 0 ;;
    status) status; exit 0 ;;
    remove) remove; exit 0 ;;
esac

# ── install / reconfigure ────────────────────────────────────────────────────
if [ -z "$WLAN" ]; then
    WLAN=$(for d in /sys/class/net/*; do [ -e "$d/phy80211" ] && [ "$(basename "$d")" != "$IFACE" ] && basename "$d"; done | head -1)
fi
if [ -z "$WLAN" ] || [ ! -e "/sys/class/net/$WLAN/phy80211" ]; then die "no Wi-Fi interface found (use --wlan)"; fi
PHY=$(cat "/sys/class/net/$WLAN/phy80211/name")
say "Wi-Fi: $WLAN on $PHY"

iw phy "$PHY" info | grep -q 'ENABLE_FTM_RESPONDER' || die "$PHY cannot be an FTM responder (no ENABLE_FTM_RESPONDER)"
iw phy "$PHY" info | sed -n '/Supported interface modes/,/Band 1/p' | grep -qE '\* AP$' || die "$PHY has no AP mode"

if ! command -v hostapd >/dev/null 2>&1; then
    say "Installing hostapd"
    if command -v apt-get >/dev/null 2>&1; then DEBIAN_FRONTEND=noninteractive apt-get install -y -q hostapd >/dev/null
    elif command -v dnf >/dev/null 2>&1; then dnf install -y -q hostapd
    elif command -v pacman >/dev/null 2>&1; then pacman -S --noconfirm --needed hostapd
    else die "install hostapd, then re-run"; fi
fi
# Distribution hostapd.service stays as it is (Debian ships it masked); we run our own unit.

# Country: explicit, else what BeaconFix knows about where we are, else the world domain (2.4 GHz only).
if [ -z "$COUNTRY" ]; then
    RUNAS=${SUDO_USER:-}
    BIN=$(command -v beaconfix 2>/dev/null || true)
    [ -z "$BIN" ] && [ -n "$RUNAS" ] && [ -x "$(getent passwd "$RUNAS" | cut -d: -f6)/.local/bin/beaconfix" ] && BIN="$(getent passwd "$RUNAS" | cut -d: -f6)/.local/bin/beaconfix"
    if [ -n "$RUNAS" ] && [ -n "$BIN" ]; then
        COUNTRY=$(runuser -u "$RUNAS" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$RUNAS")/bus" timeout 15 "$BIN" --json 2>/dev/null \
            | python3 -c 'import json,sys; print((json.load(sys.stdin).get("stats",{}).get("locale",{}) or {}).get("countryCode","").upper())' 2>/dev/null || true)
    fi
fi
COUNTRY=${COUNTRY:-00}
say "Regulatory country: $COUNTRY"
[ "$COUNTRY" != 00 ] && { iw reg set "$COUNTRY" 2>/dev/null || warn "iw reg set $COUNTRY failed"; sleep 1; }

# MAC: wlan MAC with the locally-administered bit set (a second BSSID on the same radio)
MACW=$(cat "/sys/class/net/$WLAN/address")
O1=${MACW%%:*}; REST=${MACW#*:}
N1=$(( 0x$O1 | 0x02 )); [ "$N1" -eq $((0x$O1)) ] && N1=$(( 0x$O1 ^ 0x04 ))
MAC=$(printf '%02x:%s' "$N1" "$REST")

# Band: 5 GHz channel 36 at 80 MHz if the regulatory domain lets us transmit there, else 2.4 GHz
chan_ok() {                                      # may we transmit on this frequency (MHz)?
    local f=$1
    iw phy "$PHY" channels 2>/dev/null | grep -qE "\\* $f MHz" || return 1
    iw phy "$PHY" channels 2>/dev/null | awk -v f="$f" '$0 ~ "\\* "f" MHz" {p=1; print; next} p && /\* [0-9]+ MHz/ {exit} p' \
        | grep -qiE 'no ir|disabled|radar' && return 1
    iw reg get 2>/dev/null | awk '/^global/{g=1} /^phy#/{g=0} g' | awk -v f="$f" -F'[(), -]+' '/@/ {lo=$2; hi=$3; if (f-10 >= lo && f+10 <= hi) print}' \
        | grep -qiE 'NO-IR|PASSIVE-SCAN|DFS' && return 1
    return 0
}
if [ "$BAND" = auto ]; then if chan_ok 5745 || chan_ok 5180; then BAND=5g; else BAND=2g; fi; fi
BEACON_INT=100
if [ "$BAND" = 5g ]; then
    if [ -z "$CHANNEL" ]; then
        if chan_ok 5745 && chan_ok 5805; then CHANNEL=149; else CHANNEL=36; fi
    fi
    HWMODE=a; FREQ=$((5000 + 5 * CHANNEL)); BW=80; PREAMBLE=vht
    if [ "$CHANNEL" -ge 149 ]; then CENTER_IDX=$(( (CHANNEL - 149) / 16 * 16 + 155 )); else CENTER_IDX=$(( (CHANNEL - 36) / 16 * 16 + 42 )); fi
    CENTER=$((5000 + 5 * CENTER_IDX))
    [ "$CHANNEL" -lt 100 ] && BEACON_INT=300     # UNII-1 is where home routers live: stay quiet there
    # hostapd's 20/40 MHz neighbour scan fails with EBUSY when the managed interface happens to be
    # scanning; the unit restarts it (Restart=on-failure, 5 s) and it comes up on a quiet moment.
    # (Debian/Ubuntu hostapd 2.10 has no "noscan" option to skip that scan.)
    EXTRA=$(printf 'ieee80211ac=1\nht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40]\nvht_oper_chwidth=1\nvht_oper_centr_freq_seg0_idx=%s\nvht_capab=[SHORT-GI-80]' "$CENTER_IDX")
else
    CHANNEL=${CHANNEL:-1}; HWMODE=g; FREQ=$((2407 + 5 * CHANNEL)); BW=20; CENTER=$FREQ; PREAMBLE=ht
    EXTRA='ht_capab=[SHORT-GI-20]'
fi
say "Responder: $MAC on channel $CHANNEL ($FREQ MHz, ${BW} MHz wide, $BAND), beacon ${BEACON_INT} TU, ${TXPOWER} dBm"

install -d -m 0755 "$CONF_DIR"
[ -s "$PSKFILE" ] || { umask 077; openssl rand -base64 96 | tr -dc 'A-Za-z0-9' | cut -c1-63 > "$PSKFILE"; }
chmod 600 "$PSKFILE"; : > "$ACCEPT"; chmod 644 "$ACCEPT"

SSID="BeaconFix-RTT-$(echo "$MAC" | tr -d : | tail -c 5 | tr a-f A-F)"
TEMPLATE="$HERE/beaconfix-rtt-responder.conf.in"
[ -r "$TEMPLATE" ] || TEMPLATE=/usr/share/beaconfix/beaconfix-rtt-responder.conf.in
[ -r "$TEMPLATE" ] || die "template beaconfix-rtt-responder.conf.in not found"
python3 - "$TEMPLATE" "$CONF" <<PY
import sys
t = open(sys.argv[1]).read()
for k, v in {"IFACE": "$IFACE", "SSID": "$SSID", "COUNTRY": "$( [ "$COUNTRY" = 00 ] && echo US || echo "$COUNTRY")",
             "HWMODE": "$HWMODE", "CHANNEL": "$CHANNEL", "PSK": open("$PSKFILE").read().strip(), "BEACON_INT": "$BEACON_INT",
             "BAND_EXTRA": """$EXTRA"""}.items():
    t = t.replace("@" + k + "@", v)
if "$COUNTRY" == "00":                      # world domain: do not claim a country
    t = "\n".join(l for l in t.splitlines() if not l.startswith(("country_code=", "ieee80211d="))) + "\n"
open(sys.argv[2], "w").write(t)
PY
chmod 600 "$CONF"

cat > "$STATE" <<EOF
WLAN=$WLAN
WLAN_MAC=$MACW
PHY=$PHY
MAC=$MAC
BAND=$BAND
CHANNEL=$CHANNEL
FREQ=$FREQ
CENTER=$CENTER
BW=$BW
PREAMBLE=$PREAMBLE
BEACON_INT=$BEACON_INT
TXPOWER=$TXPOWER
COUNTRY_SET=$COUNTRY
EOF
chmod 600 "$STATE"
BSSID_UP=$(echo "$MAC" | tr a-f A-F)
printf '{"bssid":"%s","freqMHz":%s,"centerFreq0MHz":%s,"bandwidthMHz":%s,"channel":%s,"preamble":"%s","band":"%s","iface":"%s","beaconIntervalTU":%s,"txPowerDbm":%s,"enabled":true}\n' \
    "$BSSID_UP" "$FREQ" "$CENTER" "$BW" "$CHANNEL" "$PREAMBLE" "$BAND" "$IFACE" "$BEACON_INT" "$TXPOWER" > "$INFO"
chmod 644 "$INFO"

install -D -m 0755 "$0" "$HELPER"
install -D -m 0644 "$HERE/beaconfix-rtt-responder.service" "$UNIT" 2>/dev/null || install -D -m 0644 /usr/share/beaconfix/beaconfix-rtt-responder.service "$UNIT"
install -D -m 0644 "$HERE/90-beaconfix-rtt.nm.conf" "$NMCONF" 2>/dev/null || install -D -m 0644 /usr/share/beaconfix/90-beaconfix-rtt.nm.conf "$NMCONF"
systemctl reload NetworkManager 2>/dev/null || true
if [ -n "$COUNTRY" ] && [ "$COUNTRY" != 00 ]; then
    printf '# BeaconFix RTT responder: boot cfg80211 in the right regulatory domain.\noptions cfg80211 ieee80211_regdom=%s\n' "$COUNTRY" > "$REGDOM"
fi
printf '%s\n' '# BeaconFix: (re)start the RTT responder when a Wi-Fi radio is added (boot, hot-plug, driver reload).' \
    'ACTION=="add", SUBSYSTEM=="ieee80211", TAG+="systemd", ENV{SYSTEMD_WANTS}+="beaconfix-rtt-responder.service"' > "$UDEVRULE"
udevadm control --reload 2>/dev/null || true

count_scan() { timeout 40 nmcli -t -f BSSID dev wifi list --rescan yes ifname "$WLAN" 2>/dev/null | wc -l; }
BEFORE=0
if [ "$SCAN_CHECK" = 1 ] && ! systemctl is-active -q beaconfix-rtt-responder.service; then
    BEFORE=$(count_scan); say "Scan before: $BEFORE access points"
fi

systemctl daemon-reload
systemctl enable beaconfix-rtt-responder.service >/dev/null 2>&1
systemctl restart beaconfix-rtt-responder.service
# Bringing the AP up can collide with a scan on the managed interface (EBUSY); systemd restarts it,
# so give that a minute before deciding the band does not work here.
up=0
for _ in $(seq 1 30); do
    sleep 2
    if systemctl is-active -q beaconfix-rtt-responder.service && iw dev "$IFACE" info 2>/dev/null | grep -q 'channel'; then up=1; break; fi
done
if [ "$up" != 1 ]; then
    journalctl -u beaconfix-rtt-responder.service -n 15 --no-pager >&2 || true
    if [ "$BAND" = 5g ]; then
        warn "hostapd did not come up on 5 GHz; falling back to 2.4 GHz"
        exec "$0" --band 2g ${COUNTRY:+--country "$COUNTRY"} --wlan "$WLAN" --txpower "$TXPOWER" ${NO_SCAN_ARG:+"$NO_SCAN_ARG"}
    fi
    systemctl disable --now beaconfix-rtt-responder.service >/dev/null 2>&1 || true
    die "the responder did not start (see the log above)"
fi

if [ "$SCAN_CHECK" = 1 ]; then
    sleep 3
    AFTER=$(count_scan); say "Scan with the responder up: $AFTER access points"
    if [ "$BEFORE" -gt 0 ] && [ "$AFTER" -lt $(( BEFORE / 2 )) ]; then
        systemctl disable --now beaconfix-rtt-responder.service >/dev/null 2>&1 || true
        die "scanning on $WLAN dropped from $BEFORE to $AFTER access points with the AP up; responder stopped and disabled (re-run with --no-scan-check to force)"
    fi
fi
say "RTT responder running"
status
