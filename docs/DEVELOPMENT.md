# Development

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
./build/beaconfix --help
```

Dependencies: Qt 6.4+ (Core, Gui, Widgets, Network, DBus, Sql + the SQLite driver), OpenSSL
(libcrypto), CMake 3.16+, a C++17 compiler. The widget needs KDE Plasma 6 and
`kpackagetool6` to install; `qmllint` (from `qt6-declarative-dev-tools`) to lint:

```sh
/usr/lib/qt6/bin/qmllint -I /usr/lib/x86_64-linux-gnu/qt6/qml plasmoid/org.kde.plasma.beaconfix/contents/ui/*.qml
```

Packages: `cmake -S . -B build-pkg -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build-pkg &&
(cd build-pkg && cpack -G DEB)` (or `-G RPM`). `BEACONFIX_INSTALL_WIDGET=OFF` skips the
system-wide widget install (the per-user installer does this and uses `kpackagetool6` instead).

## Layout

| path | what |
|---|---|
| `src/main.cpp` | CLI, single-instance / D-Bus registration, standalone mode |
| `src/locator.{h,cpp}` | the brain: locate chain, scans, events, trip log, stats, home networks, achievements, D-Bus object |
| `src/wifiscanner.{h,cpp}` | NetworkManager D-Bus scan, per-AP security classification |
| `src/mapdb.{h,cpp}` | SQLite store, migration, AES-256-GCM at rest, key handling |
| `src/apiserver.{h,cpp}` | the LAN API: HTTP/1.1 over QTcpServer, tokens, pairing, allowlist, SSE |
| `src/tilesource.{h,cpp}` | tile cache, localhost tile server for the widget, prefetch |
| `src/beaconview.{h,cpp}` | the QPainter map (no GL, no WebEngine) |
| `src/mainwindow.{h,cpp}`, `src/tray.{h,cpp}` | the window and the tray |
| `plasmoid/org.kde.plasma.beaconfix/` | the Plasma widget (QML + `security.js`) |
| `data/` | desktop entries, D-Bus service template, icon |

## How a position check works

`Locator::probe()` → `tryStarlink()` (grpcurl, if enabled) → Wi-Fi scan → `onScanFinished()`
dispatches: `diffScan()` turns the scan into events, then either the live path (update the
beacons, no geolocation) or the probe path `onScan()` → internal-database self-locate →
`queryBeaconDb()` → `tryApple()` → `tryIp()`. Each tier calls `accept(fix)` on success or the
next tier's entry point with a reason string on failure; `finish(ok, message)` ends the probe.
`accept()` decides whether the fix is a new stop, notes observations for the beacons heard,
updates home state, and emits `FixChanged`.

## Adding a tier

1. Add a `tryX(const QList<AccessPoint> &usable, const QString &why)` slot in `Locator`.
2. Insert it in the chain (the caller's failure path) and make its own failure path call the
   next tier with a reason.
3. On success build a `Fix` with `source` and `provider` set and call `accept(f)` then
   `finish(true, message)`.
4. Add a `useX` setting (load in the constructor, `setUseX()` writer, Settings checkbox) and a
   line in README's privacy table saying what leaves the machine.

## Testing without touching a live install

- **Standalone locator**: point `DBUS_SESSION_BUS_ADDRESS` at a dead socket
  (`unix:path=/nonexistent`) and run `build/beaconfix --once`; it runs the chain in-process
  without the tray and prints JSON.
- **Isolated tray**: `XDG_CONFIG_HOME=/tmp/bf-cfg XDG_STATE_HOME=/tmp/bf-state
  XDG_RUNTIME_DIR=/tmp/bf-run QT_QPA_PLATFORM=offscreen build/beaconfix --tray` on a private bus
  (`dbus-run-session` or `dbus-daemon --session --print-address`) with `apiPort` set to a spare
  port in `/tmp/bf-cfg/sworrl/beaconfix.conf`. Copy `*.json` state files in to test migration.
- **API**: `curl` against that port; see docs/API.md for the flow.
- **Widget**: `qmllint` for static checks; `plasmoidviewer -a plasmoid/org.kde.plasma.beaconfix`
  for a live preview; `--snapshot file.png` renders the app window without focus.

## Releases

Bump `project(... VERSION x.y.z)` in CMakeLists.txt and `Version` in the widget's
`metadata.json`, add a CHANGELOG entry, tag `vx.y.z`, build the .deb with cpack and attach it to
the GitHub release.
