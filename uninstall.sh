#!/usr/bin/env bash
# Remove a per-user BeaconFix install made by install.sh (default prefix ~/.local).
# Your data stays unless you pass --purge (state, database, key, settings, tokens).
#
#   ./uninstall.sh [--prefix DIR] [--purge] [--system]
#   --system also removes the root helper + polkit files installed by install.sh (sudo)
#
set -euo pipefail
PREFIX="${PREFIX:-$HOME/.local}"; PURGE=0; SYSTEM=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift ;;
        --prefix=*) PREFIX="${1#*=}" ;;
        --purge) PURGE=1 ;;
        --system) SYSTEM=1 ;;
        -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac; shift
done
pkill -x beaconfix 2>/dev/null || true
rm -f "$PREFIX/bin/beaconfix" \
      "$PREFIX/share/applications/beaconfix.desktop" \
      "$PREFIX/share/dbus-1/services/org.sworrl.BeaconFix.service" \
      "$PREFIX/share/icons/hicolor/scalable/apps/beaconfix.svg" \
      "$HOME/.config/autostart/beaconfix-tray.desktop"
if command -v kpackagetool6 >/dev/null 2>&1 && [ -d "$HOME/.local/share/plasma/plasmoids/org.kde.plasma.beaconfix" ]; then
    kpackagetool6 --type Plasma/Applet --remove org.kde.plasma.beaconfix >/dev/null 2>&1 || true
    echo "widget removed (restart plasmashell or log out to clear it from the desktop)"
fi
echo "BeaconFix removed from $PREFIX"
if [ "$SYSTEM" = 1 ]; then
    sudo rm -f /usr/local/libexec/beaconfix-osd /usr/share/polkit-1/actions/org.sworrl.beaconfix.policy /etc/polkit-1/rules.d/50-beaconfix.rules /etc/geolocation
    echo "system-side helper, polkit files and /etc/geolocation removed"
fi
if [ "$PURGE" = 1 ]; then
    rm -rf "$HOME/.local/state/beaconfix" "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/beaconfix"
    rm -f "$HOME/.config/sworrl/beaconfix.conf" "$HOME/.config/sworrl/beaconfix.key" \
          "$HOME/.config/sworrl/beaconfix-devices.json" "$HOME/.config/sworrl/beaconfix-known.json" \
          "$HOME/.config/sworrl/identity.json" "$HOME/.config/sworrl/beaconfix-sync.json"
    echo "data purged (~/.local/state/beaconfix, settings, key file, tokens, known devices)"
else
    echo "data kept in ~/.local/state/beaconfix and ~/.config/sworrl (use --purge to delete)"
fi
