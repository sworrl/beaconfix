#!/usr/bin/env bash
# Build + install BeaconFix into ~/.local: binary, desktop entry, D-Bus activation,
# tray autostart, and the Plasma widget. Re-run to upgrade.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
PLASMOID_ID="org.kde.plasma.beaconfix"

echo "==> Building"
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$HERE/build" -j"$(nproc)"
cmake --install "$HERE/build" >/dev/null

echo "==> Desktop integration"
mkdir -p "$PREFIX/share/dbus-1/services" "$HOME/.config/autostart"
sed "s|^Exec=.*|Exec=$PREFIX/bin/beaconfix --tray|" "$HERE/data/org.sworrl.BeaconFix.service" \
    > "$PREFIX/share/dbus-1/services/org.sworrl.BeaconFix.service"
sed "s|^Exec=.*|Exec=$PREFIX/bin/beaconfix --tray|" "$HERE/data/beaconfix-tray.desktop" \
    > "$HOME/.config/autostart/beaconfix-tray.desktop"
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true

if ! command -v grpcurl >/dev/null 2>&1 && [ ! -x "$HOME/go/bin/grpcurl" ]; then
    if command -v go >/dev/null 2>&1; then
        echo "==> Installing grpcurl (Starlink dish GPS tier)"
        GOFLAGS=-mod=mod go install github.com/fullstorydev/grpcurl/cmd/grpcurl@latest || echo "   (skipped — Starlink tier will be unavailable)"
    else
        echo "   grpcurl/go not found: Starlink dish GPS tier will be unavailable (Wi-Fi + IP still work)"
    fi
fi

echo "==> Plasma widget"
if [ -d "$HOME/.local/share/plasma/plasmoids/$PLASMOID_ID" ]; then
    rm -rf "$HOME/.cache/plasmashell/qmlcache/"
    kpackagetool6 --type Plasma/Applet --upgrade "$HERE/plasmoid/$PLASMOID_ID"
    RESTART_PLASMA=1
else
    kpackagetool6 --type Plasma/Applet --install "$HERE/plasmoid/$PLASMOID_ID"
    echo "   Add 'BeaconFix' via right-click panel/desktop → Add Widgets."
fi

echo "==> Starting tray"
if ! pgrep -x beaconfix >/dev/null 2>&1; then
    nohup "$PREFIX/bin/beaconfix" --tray >/dev/null 2>&1 &
    disown
else
    echo "   already running — quit it from the tray and re-run, or: kill \$(pgrep -x beaconfix) && beaconfix --tray &"
fi

if [ "${RESTART_PLASMA:-0}" = 1 ] && [ "${NO_PLASMA_RESTART:-0}" != 1 ]; then
    echo "==> Restarting plasmashell so the widget reloads"
    systemctl --user restart plasma-plasmashell.service 2>/dev/null || true
fi
echo "Done."
