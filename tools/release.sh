#!/usr/bin/env bash
# Build and publish a BeaconFix release (desktop .deb, Android APK) from this repository.
#
#   tools/release.sh --build-only      # build into dist/ and stop
#   tools/release.sh                   # …then tag vX.Y.Z and create the GitHub release (marked latest)
#   SKIP_BUILD=1 tools/release.sh      # publish what's already built
#
# The version is CMakeLists.txt's project VERSION; the Android versionName must match it. Each release also carries
# the files under fixed names (beaconfix_amd64.deb, beaconfix.apk) so the one-line installers in the README can use
# releases/latest/download/… and always get the newest. Node firmware is NOT attached: a binary built with your
# signing key would make strangers' boards take updates only from you. flasher.falcontechnix.com builds it with a
# throwaway key and writes each user's own key in, in their browser.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^project(beaconfix VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
AVER=$(sed -n 's/.*versionName = "\(.*\)"/\1/p' android/app/build.gradle.kts)
[ "$VER" = "$AVER" ] || { echo "CMakeLists.txt says $VER, the Android app says $AVER" >&2; exit 1; }
TAG="v$VER"
DIST=dist

if [ "${SKIP_BUILD:-0}" != 1 ]; then
    cmake -S . -B build-pkg -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
    cmake --build build-pkg -j"$(nproc)"
    (cd build-pkg && cpack -G DEB)
    (cd android && ./gradlew -q :app:assembleRelease)
fi
APK=android/app/build/outputs/apk/release/app-release.apk
DEB="build-pkg/beaconfix_${VER}_amd64.deb"
[ -f "$APK" ] && [ -f "$DEB" ] || { echo "missing $APK or $DEB" >&2; exit 1; }
rm -rf "$DIST"; mkdir -p "$DIST"
cp "$DEB" "$DIST/beaconfix_${VER}_amd64.deb"; cp "$DEB" "$DIST/beaconfix_amd64.deb"
cp "$APK" "$DIST/beaconfix-$VER.apk";         cp "$APK" "$DIST/beaconfix.apk"
(cd "$DIST" && sha256sum ./* | sed 's| \./| |' > SHA256SUMS)
ls -l "$DIST"

[ "${1:-}" = "--build-only" ] && exit 0

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then echo "$TAG exists already: bump the version" >&2; exit 1; fi
[ -z "$(git status --porcelain -- src android/app plasmoid CMakeLists.txt)" ] || { echo "uncommitted changes in the release sources" >&2; exit 1; }
NOTES=$(awk -v v="$VER" '$0 ~ "^## \\[" v "\\]" {f=1; next} f && /^## \[/ {exit} f' CHANGELOG.md)
[ -n "$NOTES" ] || { echo "no '## [$VER]' section in CHANGELOG.md" >&2; exit 1; }
git tag -a "$TAG" -m "BeaconFix $VER"
git push origin "$TAG"
gh release create "$TAG" --title "BeaconFix $VER" --notes "$NOTES" --latest "$DIST"/*
