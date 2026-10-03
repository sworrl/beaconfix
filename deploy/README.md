# Deploying the BeaconFix hub (and headless nodes)

The hub is `beaconfix --server`: the same binary as the desktop app, headless (`QCoreApplication` — no window, no
tray, no X/Wayland, no session D-Bus, no KWallet, no Wi-Fi scanning, no mDNS). It holds the master database and
serves BFS3 (docs/SECURE-API.md) plus the node / job protocol (docs/HUB.md) directly over TLS — no reverse proxy.

## The container (an unprivileged Proxmox LXC, Debian 13)

| | |
|---|---|
| Packages (build) | `build-essential cmake qt6-base-dev libssl-dev` (Qt ≥ 6.4; built and tested with 6.8 APIs only) |
| Packages (run) | `libqt6core6t64 libqt6network6 libqt6sql6 libqt6sql6-sqlite libqt6dbus6 libqt6gui6 libqt6widgets6 libssl3t64` — `libqt6sql6-sqlite` is **required** (the database driver); `qrencode` optional (QR codes for invites) |
| Qt TLS plugin | `ls /usr/lib/*/qt6/plugins/tls/libqopensslbackend.so` must exist (comes with Qt's network package) |
| User / group | `beaconfix` (system user, no shell, home `/var/lib/beaconfix`) |
| Binary | `/usr/local/bin/beaconfix` |
| Unit | `/etc/systemd/system/beaconfix-hub.service` (this directory) |
| Environment | `/etc/beaconfix/beaconfix-hub.env` (this directory) |
| TLS | `/etc/beaconfix/tls/fullchain.pem` (0644) and `/etc/beaconfix/tls/key.pem` (0640 root:beaconfix); the **whole chain** in fullchain.pem is sent; EC or RSA keys |
| Listens | `0.0.0.0:443/tcp` (TLS, BFS3 + `GET /healthz`) — via `AmbientCapabilities=CAP_NET_BIND_SERVICE`; `127.0.0.1:47823/tcp` (admin, plain, token-protected, loopback only) |
| State | `/var/lib/beaconfix` (StateDirectory, 0700): `state/beaconfix/beaconfix.db` (encrypted master DB), `state/beaconfix/.live/live.db` (its working copy while running), `state/beaconfix/bfs3-counters`, `state/beaconfix/hub-admin.json`, `config/sworrl/beaconfix.key` (the DB key — back up with the DB) |
| Outbound | none needed by the hub itself |
| Memory | idle ~60 MB + the database's working set; a re-encryption of the DB briefly needs ~2× its size. The unit caps at `MemoryMax=640M` |

## Build and install (as root in the CT)

```sh
apt install --no-install-recommends build-essential cmake qt6-base-dev libssl-dev libqt6sql6-sqlite qrencode
cd /usr/local/src/beaconfix                       # the rsynced tree
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBEACONFIX_TESTS=ON -DBEACONFIX_INSTALL_WIDGET=OFF -DBEACONFIX_INSTALL_POLKIT=OFF
cmake --build build -j2 --target beaconfix securechannel_test
(cd build && ctest -R securechannel --output-on-failure)     # the BFS3 vectors, byte for byte
install -m 0755 build/beaconfix /usr/local/bin/beaconfix

useradd --system --home-dir /var/lib/beaconfix --no-create-home --shell /usr/sbin/nologin beaconfix
install -d -m 0755 /etc/beaconfix
install -m 0644 deploy/beaconfix-hub.env /etc/beaconfix/beaconfix-hub.env
chgrp beaconfix /etc/beaconfix/tls/key.pem && chmod 0640 /etc/beaconfix/tls/key.pem
install -m 0644 deploy/beaconfix-hub.service /etc/systemd/system/beaconfix-hub.service
install -m 0755 deploy/beaconfix-hub-cli /usr/local/sbin/beaconfix-hub-cli
systemctl daemon-reload
systemctl enable --now beaconfix-hub
journalctl -u beaconfix-hub -n 20                 # "hub: TLS on port 443 … fingerprint <32 hex>"
```

The first start generates the database key, the database and the hub's X25519 key (`S_sk`, sealed in the DB). The
fingerprint printed is what every node pins at enrolment (it is inside each invite; nothing to type).

Check from a WireGuard peer: `curl https://hub.example.com/healthz` → `{"api":"bfs3","ok":true}`;
`curl https://hub.example.com/api/v1/state` → `404` (v1 is never on the network address).

If the unit fails with `status=226/NAMESPACE` in the unprivileged CT, comment out `PrivateDevices=` and the
`Protect{KernelTunables,KernelModules,KernelLogs,Clock,Hostname}=` lines and retry.

## Operating

```sh
beaconfix-hub-cli --invite "Pixel 10 Pro XL" --kind android   # single use, 15 min: text + QR
beaconfix-hub-cli --invite "$(hostname of the desktop)" --kind desktop
beaconfix-hub-cli --devices                                   # id, name, kind, scopes, last seen, counter, capabilities
beaconfix-hub-cli --status [--json]                           # listeners, invites, blocked sources, jobs
beaconfix-hub-cli --revoke <deviceId-or-name>
```
`beaconfix-hub-cli` runs `beaconfix --server …` as `beaconfix` with the unit's environment; the admin commands
talk to the running hub (loopback `127.0.0.1:47823` + the token from `hub-admin.json`).

Certificate renewal: replace the two files, then `systemctl try-restart beaconfix-hub` (the hub also notices a
changed `fullchain.pem` within an hour by itself).

Backup: stop or not — `state/beaconfix/beaconfix.db` is rewritten atomically; back it up with `config/sworrl/beaconfix.key`.

Optional settings in the env file: `BEACONFIX_HUB_ALLOW` (source CIDRs), `BEACONFIX_HUB_SELF_COMPUTE_MINUTES`
(the hub computes refits itself only after that many minutes without any node leasing jobs — default off),
`BEACONFIX_HUB_LEASE_SECONDS` (600), `BEACONFIX_KEY_FILE`.

## Nodes

**The desktop** (tray): `beaconfix --enroll 'bfs3:…'` with the invite from `--invite <its hostname>`, or the tray menu
"Connect to a hub…". From then on: sync every 2 min, position out / everyone's positions in every 30 s, jobs after each
sync. `beaconfix --hub` shows the link, `beaconfix --hub-sync` syncs now. A new workstation: install, enrol (new
invite), done — it pulls the whole master database on its first sync.

**A headless node** (the RV VM; needs NetworkManager for Wi-Fi):

```sh
install -m 0755 build/beaconfix /usr/local/bin/beaconfix
useradd --system --home-dir /var/lib/beaconfix --no-create-home --shell /usr/sbin/nologin beaconfix
install -d -m 0755 /etc/beaconfix
install -m 0644 deploy/beaconfix-node.env /etc/beaconfix/beaconfix-node.env
install -m 0644 deploy/beaconfix-node.service /etc/systemd/system/
install -m 0644 deploy/50-beaconfix-node.rules /etc/polkit-1/rules.d/
install -d -o beaconfix -g beaconfix -m 0700 /var/lib/beaconfix
runuser -u beaconfix -- env $(grep -v '^#' /etc/beaconfix/beaconfix-node.env | xargs) /usr/local/bin/beaconfix --node --enroll 'bfs3:…'
systemctl daemon-reload && systemctl enable --now beaconfix-node
```
