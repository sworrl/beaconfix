#!/usr/bin/env bash
# Build, test and (unless --build-only) publish a BeaconFix Lite release from this repository.
#
#   tools/release_lite.sh --build-only     # test and build into lite/build/dist
#   tools/release_lite.sh                  # …then tag lite-vX.Y.Z and create the GitHub release (gh)
#
# The version is lite/build.gradle.kts's liteVersion. Lite releases are tagged lite-vX.Y.Z so they sit next to the
# desktop/app releases (vX.Y.Z) without either series' "latest" pointing at the other: lite releases are created
# with --latest=false.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^val liteVersion = "\(.*\)"/\1/p' lite/build.gradle.kts)
[ -n "$VER" ] || { echo "no liteVersion in lite/build.gradle.kts" >&2; exit 1; }
TAG="lite-v$VER"
DIST=lite/build/dist

(cd android && ./gradlew -q :lite:testDebugUnitTest :lite:assembleRelease :lite:coreJar :lite:sourcesZip)
cp lite/build/outputs/aar/lite-release.aar "$DIST/beaconfix-lite-$VER.aar"
(cd "$DIST" && sha256sum "beaconfix-lite-$VER.aar" "beaconfix-lite-core-$VER.jar" "beaconfix-lite-src-$VER.zip" > "beaconfix-lite-$VER.sha256")
ls -l "$DIST"

[ "${1:-}" = "--build-only" ] && exit 0

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then echo "$TAG exists already: bump liteVersion" >&2; exit 1; fi
[ -z "$(git status --porcelain -- lite)" ] || { echo "lite/ has uncommitted changes" >&2; exit 1; }
NOTES=$(awk -v v="$VER" '$0 ~ "^## \\[lite " v "\\]" {f=1; next} f && /^## \[/ {exit} f' CHANGELOG.md)
[ -n "$NOTES" ] || { echo "no '## [lite $VER]' section in CHANGELOG.md" >&2; exit 1; }
git tag -a "$TAG" -m "BeaconFix Lite $VER"
git push origin "$TAG"
gh release create "$TAG" --title "BeaconFix Lite $VER" --notes "$NOTES" --latest=false \
    "$DIST/beaconfix-lite-$VER.aar" "$DIST/beaconfix-lite-core-$VER.jar" "$DIST/beaconfix-lite-src-$VER.zip" "$DIST/beaconfix-lite-$VER.sha256"
