#!/usr/bin/env bash
# BeaconFix installer: builds from source and installs for the current user (default prefix
# ~/.local): the binary, desktop entry, D-Bus activation, tray autostart and the Plasma widget.
# Re-run to upgrade. See docs/DEVELOPMENT.md for the manual route and packaging.
#
#   ./install.sh [-y] [--prefix DIR] [--no-widget] [--no-autostart] [--no-deps] [--no-restart]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
PLASMOID_ID="org.kde.plasma.beaconfix"
ASSUME_YES=0 WANT_WIDGET=1 WANT_AUTOSTART=1 CHECK_DEPS=1 RESTART_PLASMA_OK=1
[ "${NO_PLASMA_RESTART:-0}" = 1 ] && RESTART_PLASMA_OK=0

usage() { sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) ASSUME_YES=1 ;;
        --prefix) PREFIX="$2"; shift ;;
        --prefix=*) PREFIX="${1#*=}" ;;
        --no-widget) WANT_WIDGET=0 ;;
        --no-autostart) WANT_AUTOSTART=0 ;;
        --no-deps) CHECK_DEPS=0 ;;
        --no-restart) RESTART_PLASMA_OK=0 ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ask()  { [ "$ASSUME_YES" = 1 ] && return 0; read -r -p "$1 [Y/n] " a; [ -z "$a" ] || [[ "$a" =~ ^[Yy] ]]; }

# ── 1. Dependencies ───────────────────────────────────────────────────────────
# Build: cmake, a C++17 compiler, Qt 6 (Core Gui Widgets Network DBus Sql), OpenSSL.
# Run:   the Qt SQLite driver, NetworkManager (Wi-Fi scans), Plasma 6 for the widget.
distro_id() { . /etc/os-release 2>/dev/null; echo "${ID:-unknown} ${ID_LIKE:-}"; }
if [ "$CHECK_DEPS" = 1 ]; then
    say "Checking build dependencies"
    missing=()
    command -v cmake >/dev/null 2>&1 || missing+=(cmake)
    command -v c++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || missing+=(compiler)
    command -v pkg-config >/dev/null 2>&1 || missing+=(pkg-config)
    have_qt=0; for q in qmake6 qmake; do command -v "$q" >/dev/null 2>&1 && have_qt=1; done
    ls /usr/lib/*/cmake/Qt6/Qt6Config.cmake /usr/lib64/cmake/Qt6/Qt6Config.cmake >/dev/null 2>&1 && have_qt=1
    [ "$have_qt" = 1 ] || missing+=(qt6)
    pkg-config --exists libcrypto 2>/dev/null || missing+=(openssl)
    if [ ${#missing[@]} -gt 0 ]; then
        note "missing: ${missing[*]}"
        case "$(distro_id)" in
            *neon*|*ubuntu*|*debian*)
                pkgs=(build-essential cmake pkg-config qt6-base-dev qt6-base-dev-tools libqt6sql6-sqlite libssl-dev)
                [ "$WANT_WIDGET" = 1 ] && pkgs+=(kf6-kpackage)
                note "Debian/Ubuntu/KDE neon: sudo apt install ${pkgs[*]}"
                if ask "Install them now with sudo apt?"; then sudo apt install -y "${pkgs[@]}"; else die "install the packages above and re-run"; fi ;;
            *fedora*|*rhel*)
                pkgs=(cmake gcc-c++ pkgconf-pkg-config qt6-qtbase-devel qt6-qtbase-sqlite openssl-devel)
                [ "$WANT_WIDGET" = 1 ] && pkgs+=(kf6-kpackage)
                note "Fedora: sudo dnf install ${pkgs[*]}"
                if ask "Install them now with sudo dnf?"; then sudo dnf install -y "${pkgs[@]}"; else die "install the packages above and re-run"; fi ;;
            *arch*)
                pkgs=(cmake gcc pkgconf qt6-base openssl)
                [ "$WANT_WIDGET" = 1 ] && pkgs+=(kpackage)
                note "Arch: sudo pacman -S --needed ${pkgs[*]}"
                if ask "Install them now with sudo pacman?"; then sudo pacman -S --needed --noconfirm "${pkgs[@]}"; else die "install the packages above and re-run"; fi ;;
            *) die "install cmake, a C++17 compiler, Qt 6 (base + sql/sqlite), OpenSSL and re-run" ;;
        esac
    else
        note "all present"
    fi
    if ! command -v nmcli >/dev/null 2>&1; then
        note "NetworkManager (nmcli) not found: Wi-Fi scans need a NetworkManager-managed interface"
    fi
fi

# ── 2. Build + install ────────────────────────────────────────────────────────
say "Building (prefix $PREFIX)"
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DBEACONFIX_INSTALL_WIDGET=OFF >/dev/null
cmake --build "$HERE/build" -j"$(nproc)"
cmake --install "$HERE/build" >/dev/null
note "installed $PREFIX/bin/beaconfix"

# ── 3. Desktop integration for a user prefix ─────────────────────────────────
# cmake --install already placed the desktop entry, icon and D-Bus service under $PREFIX/share.
# The autostart entry belongs in ~/.config/autostart, which cmake's sysconfdir is not.
say "Desktop integration"
if [ "$WANT_AUTOSTART" = 1 ]; then
    mkdir -p "$HOME/.config/autostart"
    sed "s|@CMAKE_INSTALL_FULL_BINDIR@|$PREFIX/bin|" "$HERE/data/beaconfix-tray.desktop.in" \
        > "$HOME/.config/autostart/beaconfix-tray.desktop"
    note "tray autostarts at login (~/.config/autostart/beaconfix-tray.desktop)"
fi
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) note "add $PREFIX/bin to your PATH to use the beaconfix command" ;; esac

if ! command -v grpcurl >/dev/null 2>&1 && [ ! -x "$HOME/go/bin/grpcurl" ]; then
    if command -v go >/dev/null 2>&1 && ask "Install grpcurl with Go (needed only for the Starlink dish GPS tier)?"; then
        GOFLAGS=-mod=mod go install github.com/fullstorydev/grpcurl/cmd/grpcurl@latest || note "grpcurl install failed; the Starlink tier stays off"
    else
        note "grpcurl not found: the Starlink dish GPS tier is unavailable (Wi-Fi, map database and IP still work)"
    fi
fi

# ── 4. Plasma widget (per-user) ──────────────────────────────────────────────
RESTART_PLASMA=0
if [ "$WANT_WIDGET" = 1 ]; then
    if command -v kpackagetool6 >/dev/null 2>&1; then
        say "Plasma widget"
        if [ -d "$HOME/.local/share/plasma/plasmoids/$PLASMOID_ID" ]; then
            rm -rf "$HOME/.cache/plasmashell/qmlcache/"
            kpackagetool6 --type Plasma/Applet --upgrade "$HERE/plasmoid/$PLASMOID_ID" >/dev/null
            note "upgraded"; RESTART_PLASMA=1
        else
            kpackagetool6 --type Plasma/Applet --install "$HERE/plasmoid/$PLASMOID_ID" >/dev/null
            note "installed — add 'BeaconFix' via right-click on the desktop or a panel → Add Widgets"
        fi
    else
        note "kpackagetool6 not found: skipping the Plasma widget (install kf6-kpackage / plasma-workspace)"
    fi
fi

# ── 5. (Re)start the tray ─────────────────────────────────────────────────────
say "Starting the tray"
if pgrep -x beaconfix >/dev/null 2>&1; then
    pkill -x beaconfix || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x beaconfix >/dev/null 2>&1 || break; sleep 0.5; done
    pkill -9 -x beaconfix 2>/dev/null || true
fi
nohup "$PREFIX/bin/beaconfix" --tray >/dev/null 2>&1 &
disown
note "tray running; first start migrates older JSON state into the encrypted map database"

# Put the tray icon in the visible row of the Plasma system tray, not the overflow.
TRAYRC="$HOME/.config/plasma-org.kde.plasma.desktop-appletsrc"
if [ -f "$TRAYRC" ] && command -v kreadconfig6 >/dev/null 2>&1; then
    grp="$(awk '/^\[/{g=$0} /^extraItems=/{print g; exit}' "$TRAYRC")"
    if [ -n "$grp" ]; then
        args=(); for part in $(printf '%s' "$grp" | sed -E 's/\]\[/ /g; s/^\[//; s/\]$//'); do args+=(--group "$part"); done
        shown="$(kreadconfig6 --file "$TRAYRC" "${args[@]}" --key shownItems 2>/dev/null || true)"
        case ",$shown," in *,beaconfix,*) ;; *)
            kwriteconfig6 --file "$TRAYRC" "${args[@]}" --key shownItems "${shown:+$shown,}beaconfix"
            RESTART_PLASMA=1 ;;
        esac
    fi
fi

if [ "$RESTART_PLASMA" = 1 ] && [ "$RESTART_PLASMA_OK" = 1 ]; then
    say "Restarting plasmashell so the widget reloads"
    systemctl --user restart plasma-plasmashell.service 2>/dev/null || true
fi

say "Done"
note "app:     $PREFIX/bin/beaconfix        tray: beaconfix --tray (autostarts)"
note "status:  beaconfix --json | beaconfix --api-status | beaconfix --db-stats"
note "docs:    $HERE/README.md and $HERE/docs/"
note "remove:  $HERE/uninstall.sh"
