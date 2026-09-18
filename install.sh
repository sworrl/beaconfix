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

echo "==> (Re)starting tray"
if pgrep -x beaconfix >/dev/null 2>&1; then
    pkill -x beaconfix || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x beaconfix >/dev/null 2>&1 || break; sleep 0.5; done
    pkill -9 -x beaconfix 2>/dev/null || true
fi
nohup "$PREFIX/bin/beaconfix" --tray >/dev/null 2>&1 &
disown

# Make sure the tray icon lands in the visible row, not the overflow.
# The system tray's item lists live in the group that holds "extraItems=".
TRAYRC="$HOME/.config/plasma-org.kde.plasma.desktop-appletsrc"
if [ -f "$TRAYRC" ]; then
    grp="$(awk '/^\[/{g=$0} /^extraItems=/{print g; exit}' "$TRAYRC")"
    if [ -n "$grp" ]; then
        # "[Containments][409][Applets][414][General]" -> --group Containments --group 409 ...
        args=(); for part in $(printf '%s' "$grp" | sed -E 's/\]\[/ /g; s/^\[//; s/\]$//'); do args+=(--group "$part"); done
        shown="$(kreadconfig6 --file "$TRAYRC" "${args[@]}" --key shownItems 2>/dev/null || true)"
        case ",$shown," in *,beaconfix,*) ;; *)
            kwriteconfig6 --file "$TRAYRC" "${args[@]}" --key shownItems "${shown:+$shown,}beaconfix"
            RESTART_PLASMA=1 ;;
        esac
    fi
fi

if [ "${RESTART_PLASMA:-0}" = 1 ] && [ "${NO_PLASMA_RESTART:-0}" != 1 ]; then
    echo "==> Restarting plasmashell so the widget reloads"
    systemctl --user restart plasma-plasmashell.service 2>/dev/null || true
fi
echo "Done."
