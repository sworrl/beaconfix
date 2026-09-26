# Contributing

- Build with `cmake -S . -B build && cmake --build build -j"$(nproc)"`; keep it warning-free.
- Lint the widget with `qmllint` (see docs/DEVELOPMENT.md) before opening a pull request.
- Never commit anything from `~/.config/sworrl` or `~/.local/state/beaconfix`: tokens, keys,
  device lists and the database are personal.
- Keep examples free of real addresses, MAC prefixes, SSIDs and hostnames; use
  `<beaconfix-host>`, `192.0.2.10`, `AA:BB:CC:?D:EE:F?`, `Example Wi-Fi`.
- One change per pull request, with a CHANGELOG entry under *Unreleased*.
