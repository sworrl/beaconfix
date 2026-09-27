# BeaconFix agent (a Raspberry Pi with a GNSS HAT)

`agent/beaconfix-agent` turns a Raspberry Pi with a GPS HAT into a headless BeaconFix node. It is the most precise witness BeaconFix has:

* **The RV's true position.** While parked it averages the GNSS fix and reports an honest error, so the
  desktop can stop guessing from Wi-Fi (±40 m) and pin the RV to a metre or so.
* **The best vantage points the estimator will ever get.** Every Wi-Fi scan it takes is tagged with that
  averaged position and its σ, and synced into the desktop's map database.
* **Another radio for ranging.** It advertises the BeaconFix BLE beacon (kind `pi`, its tag made from a
  per-device id derived from the desktop's identity and its device name, RANGING.md §9.1) and reports what it
  hears from the desktop and the phone (docs/RANGING.md).
* **Its own health.** Link changes, under-voltage and the battery level go to the desktop as device events,
  so a dying battery shows up in BeaconFix before the Pi disappears.
* **The LAN's clock.** `agent/setup-ntp-on-pi.sh` makes it a stratum-1 NTP server (chrony, GPS + PPS).

Python 3 and Debian packages only; nothing is fetched at runtime.

## Install (from the desktop)

Prerequisites on the Pi: key-based SSH for a user with passwordless `sudo`.

```sh
ssh-copy-id -i ~/.ssh/id_ed25519.pub <user>@<pi>      # once, with that user's password
ssh <user>@<pi> 'echo "$USER ALL=(ALL) NOPASSWD: ALL" | sudo tee /etc/sudoers.d/90-beaconfix'
agent/install-on-pi.sh <user>@<pi>
agent/setup-ntp-on-pi.sh --allow 192.0.2.0/24,198.51.100.0/24 <user>@<pi>   # the NTP server (below)
```

The `NOPASSWD` rule is only needed while these scripts run (the agent itself runs as its own system
user without sudo); remove it afterwards with `ssh <user>@<pi> sudo rm /etc/sudoers.d/90-beaconfix`
and add it back before an update or `uninstall-on-pi.sh`.

`install-on-pi.sh` installs `gpsd gpsd-clients python3-gps python3-dbus python3-gi bluez avahi-utils iw
ethtool i2c-tools`, checks that gpsd sees the GPS (it only fills `/etc/default/gpsd DEVICES` when that is
empty, with a backup), creates the `beaconfix-agent` system user, installs the program and
`beaconfix-agent.service`, mints a control-scope token on the desktop (`beaconfix --token <pi> --control`)
and copies it to `/etc/beaconfix-agent/token` (0640 root:beaconfix-agent, never printed), makes sure the
Pi's interfaces are in the desktop's known-device list, starts the service and prints a status summary.

`--configure-uart-pps` is the only option that touches `/boot`: when gpsd sees no GPS it enables the UART
(`enable_uart=1`), loads the PPS overlay (`dtoverlay=pps-gpio,gpiopin=4`), removes the serial console from
`cmdline.txt`, disables the serial getty and reboots, printing every change first. `--keep-token` reuses the
Pi's token; `--no-apt` skips packages. `agent/uninstall-on-pi.sh [--keep-data] <user>@<host>` removes it all
and revokes the token.

### The GPS HAT and Bluetooth on a Pi 4

GPIO HATs (Adafruit Ultimate GPS, Waveshare L76X, u-blox based) talk on GPIO 14/15 (`/dev/serial0`) and put
PPS on GPIO 4. On a Pi 4 Bluetooth normally owns the good UART (PL011) and `serial0` is the mini-UART, which
is fine at 9600 baud once `enable_uart=1` pins the core clock. `dtoverlay=disable-bt` gives the GPS the PL011
but **switches Bluetooth off**, so the agent's BLE part reports "no Bluetooth adapter"; `dtoverlay=miniuart-bt`
keeps both. Check with `ls -l /dev/serial0`, `gpspipe -w -n 5`, `ppstest /dev/pps0`.

## The NTP server (`agent/setup-ntp-on-pi.sh`)

Inventories what was set up before (chrony, ntp/ntpsec, openntpd, systemd-timesyncd, what owns UDP 123,
gpsd's devices, `/dev/pps0`, the PPS overlay, old refclock lines and their offsets), then installs chrony:

| Source | Line | Role |
|---|---|---|
| NMEA via gpsd shared memory 0 | `refclock SHM 0 refid NMEA offset <S> delay 0.2 noselect` | numbers the seconds |
| PPS edge on GPIO 4 | `refclock PPS /dev/pps0 refid PPS lock NMEA prefer trust` | sub-µs, stratum 1 |
| internet | `pool 2.debian.pool.ntp.org iburst`, `server time.cloudflare.com` | backup |
| nothing left | `local stratum 10 orphan` | the LAN keeps one consistent time |

`allow` lists the networks given with `--allow` (default: the Pi's directly connected subnets); `makestep 1 3`, `rtcsync`.
systemd-timesyncd is masked and any other NTP daemon disabled so nothing else binds UDP 123; gpsd gets `-n`
so it reads the GPS without clients. The NMEA offset (sentence delay after the second) is reused from the
old configuration when it had one, else 0.2 s; tune it until `chronyc sourcestats` shows NMEA near 0.
**Without PPS** (no `/dev/pps0`) NMEA alone is good to tens of milliseconds, so it is published honestly
as stratum 2 and the internet pool is preferred when reachable; `--configure-pps` adds the overlay and
reboots. `--dry-run` prints the inventory and the config without changing anything. The script ends with
`chronyc tracking`, `chronyc sources -v`, `chronyc sourcestats` and a few `ppstest` pulses.

Check from any machine on the LAN: `sntp <pi>` or `chronyc -h <pi> -n tracking` (the latter needs
`cmdallow` on the Pi). A reply with leap indicator 3 means "not synchronised" and clients ignore it —
typical right after a Pi without an RTC boots from its last saved time, days behind, before GPS locks.

## What it sends

| When | Endpoint | Body |
|---|---|---|
| every 10 s | `POST /api/v1/devices/position` (read) | `lat lon acc time source kind:"pi" beacons` + `gnss{nEff durationS sigmaU sats hdop}` + `link{…}` (wired speed/duplex, Wi-Fi dBm, `power{throttled undervoltageNow battery{percent charging voltage source}}`) + `events[]` |
| immediately on a link/power event | same | the event in `events[]` |
| every 60 s | `POST /api/v1/db/sync` (control) | `observations[]` `{bssid ssid freq dbm time lat lon acc source device}` and `fixes[]` `{time lat lon acc source provider elev device}` |
| every 10 min, or when σ improves by 20 % or the average moves | `POST /api/v1/anchors` (control) | the Pi's antenna as an anchor (`source:"gps-average"`, `accM` = σH, `rv:true`, plus `gnss{…}`) |
| every 5 s with samples | `POST /api/v1/ranging` (read) | BLE samples heard from the desktop in `ble[]`, everything heard (desktop, phone) in `heard[]`, the fix |

Optional endpoints are feature-detected (a 404 is cached for 30 min). The agent stays under 40 requests a
minute (the desktop allows 60). Anything undelivered waits in `/var/lib/beaconfix-agent/queue.db`
(SQLite, WAL, capped at 200 000 rows per table) and is flushed on reconnect. API traffic is bound to the
wired interface when it is up on the desktop's subnet.

Anchor fields follow the frozen contract in RANGING.md §4.1 except two values the contract does not list
yet: `placedBy:"pi"` and, when BLE advertising fails, `kind:"custom"` (with BLE it is `kind:"ble"`,
`ble:<service UUID>`).

## The averaging, and why its σ is honest

Errors of a static single-frequency receiver are strongly autocorrelated: multipath over a minute or two,
atmosphere and satellite geometry over tens of minutes, plus biases that never average out. N fixes are not
N independent samples. For each axis:

```
sigma2_epoch = (k * prior^2 + Neff * s^2) / (k + Neff)          k = 3; prior = receiver's claim (epx/epy/eph), capped at 5 m
Neff         = T / tau_int,  tau_int = max(2 * tau_prior, IAT)   tau_prior = 90 s; IAT = Sokal's integrated
                                                                  autocorrelation time of 5-s bin means
sigma^2      = sigma2_epoch / Neff + lp^2 * F(T, tau_lp) + floor^2
F(T, tau)    = 2 tau/T * (1 - tau/T * (1 - exp(-T/tau)))          exact variance factor of an OU mean
lp = 1 m, tau_lp = 20 min (long-period error one session cannot observe); floor = 0.5 m horizontal, 1.0 m vertical
```

The data may only *lengthen* `tau_int`: one session's IAT estimate is ±50 % and biased low for slow error
components, and underestimating it makes σ overconfident. Movement ends a session: speed > 0.6 m/s for three
epochs (every HAT reports speed); without speed, the last 10 s drifting from the mean by more than
max(4.5 σ, 12 m) for 8 s. With speed available the drift test is only a gross backstop (6 σ, 25 m) —
correlated error makes 3-σ excursions of several seconds routine, and a false reset throws away hours of
averaging. A new session starts after 20 s at rest.

Tests (`agent/tests/test_agent.py`, 40 simulated sessions per row, each with its own randomly drawn
two-timescale error model, so the truth is **not** the prior):

| Parked | 2σ coverage (target 95 %) | Mean reported σH | Actual RMS per axis |
|---|---|---|---|
| 15 min | 95.0 % | 1.55 m | 1.56 m |
| 60 min | 98.8 % | 1.10 m | 0.89 m |
| 180 min | 97.5 % | 0.85 m | 0.72 m |

End to end through a real gpsd (`agent/tests/run_gpsfake.sh`: synthetic NMEA → gpsfake → agent):
45-min park 0.70 m error vs σH 1.20 m; 20-min park 1.66 m vs σH 1.64 m; both inside 2σ; departure
detected 2 s after the vehicle moved.

## Configuration (`/etc/beaconfix-agent/agent.conf`)

| Section / key | Default | Meaning |
|---|---|---|
| `agent.device`, `agent.kind` | hostname, `pi` | how the desktop names this node |
| `desktop.url` | empty (the installer writes the desktop's address) | empty = mDNS `_beaconfix._tcp` (docker bridge addresses are skipped) |
| `desktop.token_file` | `/etc/beaconfix-agent/token` | bearer token (control scope) |
| `gpsd.host`, `gpsd.port` | `127.0.0.1`, `2947` | |
| `wifi.enabled`, `wifi.interface` | `yes`, `auto` | `iw` scans (real dBm); NetworkManager percent → dBm as a fallback |
| `ble.enabled` | `yes` | advertise (btmgmt) and scan (BlueZ) |
| `anchor.enabled`, `anchor.name` | `yes`, `<host> GPS antenna` | |
| `power.ups` | `auto` | `none` switches battery probing off |
| `intervals.*` | position 10, sync 60, fix 60, anchor 600, wifi 30 (s) | |
| `averaging.*` | tau_prior 90, floor 0.5, floor_vertical 1.0, longperiod_sigma 1.0, longperiod_tau 1200 | |

`beaconfix-agent --status` prints the live state (`/run/beaconfix-agent/status.json`); logs are in
`journalctl -u beaconfix-agent`.

## Power, and a Pi that keeps disappearing

A battery-powered Pi does not simply switch off when the battery runs down:
under-voltage makes the Wi-Fi radio drop and re-associate over and over, and the Ethernet PHY renegotiates
at 10 Mb/s half duplex before the link goes away, which looks exactly like a bad cable. When it comes back
it has no real-time clock, so it boots from the last saved time, days behind, until GPS or the network
corrects it.

What the agent does about it:

* `vcgencmd get_throttled` every 10 s (group `video`): bit 0 under-voltage **now**, bit 16 has happened since
  boot, bits 2/18 throttling. New bits become `undervoltage` / `throttled` events, sent at once.
* Battery level from common UPS/battery HATs, read-only, silent when none is present:
  `/sys/class/power_supply/*` (HATs with a kernel driver), the PiSugar server (TCP 127.0.0.1:8423),
  a MAX17040/43/48 fuel gauge at I2C 0x36 (Geekworm X7xx/X1200), an INA219 at 0x40–0x43 (Waveshare UPS HAT
  (B) at 0x42 is 2S, 6.0–8.4 V; (C)/(D) at 0x43 are 1S, 3.0–4.2 V). Only registers are read. Events:
  `battery-low` (≤ 20 %), `battery-critical` (≤ 10 %), `on-battery` / `on-power`.
* The wired link's speed and duplex (sysfs, `ethtool`): anything under 100 Mb/s or half duplex is a
  `degraded` event — on a battery-powered Pi, check the supply before the cable.

What to do: power the Pi from a 5.1 V / 3 A supply, or from a UPS HAT large enough for the HAT plus Wi-Fi,
with a low-battery shutdown configured; a Pi 4 with a GPS HAT and Wi-Fi draws about 3–4 W. `vcgencmd
get_throttled` should read `throttled=0x0`. An RTC (many UPS HATs include one) keeps the clock across
outages.

## Security

The service runs as the `beaconfix-agent` system user with one capability, `CAP_NET_ADMIN`, for
`iw … scan trigger` and `btmgmt add-adv`; reading scans, gpsd, BlueZ discovery and the API need none.
`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `NoNewPrivileges`, and only the UNIX, IP, netlink and
Bluetooth socket families. The token file is 0640 root:beaconfix-agent. The desktop accepts the token only
from known devices; the installer adds the Pi's MACs when they are missing.

## Tests

```sh
python3 agent/tests/test_agent.py            # averaging coverage, movement, iw parser, BLE payload, queue, UPS readers
agent/tests/run_gpsfake.sh                    # synthetic NMEA through a real gpsd (needs gpsd, gpsd-tools)
agent/tests/run_desktop_e2e.sh                # the agent against a throw-away desktop instance on port 47890
```

`run_gpsfake.sh` uses a private copy of the gpsd binary because Ubuntu's AppArmor profile for
`/usr/sbin/gpsd` forbids the pseudo-terminal gpsfake feeds it through. `run_desktop_e2e.sh` never touches the
live desktop: private D-Bus session, temporary config and state (its own map DB and key), no mDNS,
OS integration off, and the known-device allowlist off because a fresh instance knows no devices.
