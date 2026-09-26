#!/usr/bin/env bash
# One-line installer:  curl -fsSL https://raw.githubusercontent.com/sworrl/beaconfix/master/get.sh | bash
# Clones (or updates) the source into ~/.local/src/beaconfix and runs install.sh -y.
# Pass options through:      ... | bash -s -- --no-widget --prefix ~/apps
set -euo pipefail
REPO="${BEACONFIX_REPO:-https://github.com/sworrl/beaconfix.git}"
REF="${BEACONFIX_REF:-master}"
SRC="${BEACONFIX_SRC:-$HOME/.local/src/beaconfix}"
command -v git >/dev/null 2>&1 || { echo "git is required (apt install git)"; exit 1; }
if [ -d "$SRC/.git" ]; then
    echo "==> Updating $SRC"
    git -C "$SRC" fetch -q origin "$REF"
    git -C "$SRC" checkout -q "$REF" 2>/dev/null || true
    git -C "$SRC" pull -q --ff-only origin "$REF"
else
    echo "==> Cloning into $SRC"
    mkdir -p "$(dirname "$SRC")"
    git clone -q --branch "$REF" "$REPO" "$SRC"
fi
exec bash "$SRC/install.sh" -y "$@"
