#!/usr/bin/env python3
"""Unit tests for beaconfix-agent (stdlib unittest). Run: python3 agent/tests/test_agent.py -v"""
import hashlib
import importlib.machinery
import importlib.util
import math
import os
import random
import socket
import tempfile
import threading
import time
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
_loader = importlib.machinery.SourceFileLoader("bfagent", os.path.join(HERE, "..", "beaconfix-agent"))
_spec = importlib.util.spec_from_loader("bfagent", _loader)
A = importlib.util.module_from_spec(_spec)
_loader.exec_module(A)


class ErrorModel:
    """Realistic static-GNSS error: two Ornstein-Uhlenbeck components per axis (fast multipath,
    slow atmosphere/geometry) + white noise + a constant per-run bias. Seeded, deterministic."""
    def __init__(self, rng, fast=(2.0, 60.0), slow=(1.0, 1200.0), white=0.3, bias=0.3):
        self.rng = rng
        self.fast, self.slow, self.white = fast, slow, white
        self.x = [[rng.gauss(0, fast[0]), rng.gauss(0, slow[0])] for _ in range(3)]
        self.b = [rng.gauss(0, bias) for _ in range(3)]

    def step(self, dt=1.0):
        out = []
        for i in range(3):
            for j, (sig, tau) in enumerate((self.fast, self.slow)):
                a = math.exp(-dt / tau)
                self.x[i][j] = a * self.x[i][j] + math.sqrt(1 - a * a) * sig * self.rng.gauss(0, 1)
            k = 2.0 if i == 2 else 1.0
            out.append(k * (self.x[i][0] + self.x[i][1] + self.b[i]) + self.rng.gauss(0, self.white))
        return out


LAT0, LON0 = 40.0000000, -75.0000000


def feed_parked(avg, rng, seconds, eph=None, t0=1.79e9, vary=False):
    if vary:                                   # the truth is NOT the averager's prior: draw the error model per run
        em = ErrorModel(rng, fast=(rng.uniform(1.2, 3.0), rng.uniform(30, 150)), slow=(rng.uniform(0.4, 1.6), rng.uniform(600, 2400)),
                        white=rng.uniform(0.1, 0.5), bias=rng.uniform(0.1, 0.4))
    else:
        em = ErrorModel(rng)
    fr = A.Frame(LAT0, LON0)
    for s in range(seconds):
        e, n, u = em.step()
        lat, lon = fr.ll(e, n)
        avg.add(t0 + s, lat, lon, 120.0 + u, eph=eph, speed=abs(rng.gauss(0, 0.08)))
    return fr


class TestAveraging(unittest.TestCase):
    def test_iat_of_an_ou_process(self):
        """Averaged over independent 6-hour series the estimate approaches the theory (2*tau for an
        OU process); a single series is +-50 %, which is why the averager never trusts a SHORT one."""
        tau, est = 90.0, []
        for seed in range(8):
            rng = random.Random(seed)
            x, a, series = 0.0, math.exp(-1 / tau), []
            for _ in range(6 * 3600):
                x = a * x + math.sqrt(1 - a * a) * rng.gauss(0, 1)
                series.append(x)
            bins = [sum(series[i:i + 5]) / 5 for i in range(0, len(series), 5)]
            t, _ = A.integrated_autocorr_time(bins)
            est.append(t * 5.0)
        mean = sum(est) / len(est)
        print("\n  IAT estimates (s): %s  mean %.0f  theory %.0f" % (" ".join("%.0f" % e for e in est), mean, 2 * tau))
        self.assertAlmostEqual(mean, 2 * tau, delta=0.30 * 2 * tau)

    def test_coverage_and_sharpness(self):
        """Over many parked sessions the reported 1-sigma must be honest: about 95 % of the true
        per-axis errors inside 2 sigma, and not absurdly conservative."""
        rows = []
        for minutes in (15, 60, 180):
            inside, n_axes, sig_sum, err2_sum, runs = 0, 0, 0.0, 0.0, 40
            for r in range(runs):
                rng = random.Random(1000 * minutes + r)
                avg = A.StaticAverager()
                feed_parked(avg, rng, minutes * 60, eph=rng.choice([None, 6.0, 12.0]), vary=True)
                res = avg.result()
                self.assertIsNotNone(res)
                fr = A.Frame(LAT0, LON0)
                e, n = fr.en(res["lat"], res["lon"])
                for err, sig in ((e, res["sigmaE"]), (n, res["sigmaN"])):
                    inside += abs(err) <= 2 * sig
                    n_axes += 1
                sig_sum += res["sigmaH"]
                err2_sum += (e * e + n * n) / 2
            cov = inside / n_axes
            rms = math.sqrt(err2_sum / runs)
            rows.append((minutes, cov, sig_sum / runs, rms))
        print("\n  parked  2-sigma coverage  mean reported sigmaH  actual RMS per axis")
        for m, c, sg, e in rows:
            print("  %4d min      %5.1f %%            %5.2f m             %5.2f m" % (m, 100 * c, sg, e))
        for m, c, sg, e in rows:
            self.assertGreaterEqual(c, 0.88, "coverage too low at %d min: %.2f" % (m, c))
            self.assertLessEqual(sg, 3.0 * e + 0.5, "sigma absurdly conservative at %d min" % m)

    def test_sigma_never_below_floor(self):
        avg = A.StaticAverager()
        feed_parked(avg, random.Random(3), 6 * 3600)
        r = avg.result()
        self.assertGreaterEqual(r["sigmaH"], 0.5)
        self.assertGreaterEqual(r["sigmaU"], 1.0)

    def test_movement_ends_session_and_parking_restarts(self):
        rng = random.Random(11)
        avg = A.StaticAverager()
        feed_parked(avg, rng, 600)
        self.assertEqual(avg.state, "parked")
        fr = A.Frame(LAT0, LON0)
        t = 1.79e9 + 600
        moved_at = None
        for s in range(120):                                   # drive east at 12 m/s
            lat, lon = fr.ll(12.0 * s, 0.0)
            avg.add(t + s, lat, lon, 120.0, speed=12.0)
            if avg.state == "moving" and moved_at is None:
                moved_at = s
        self.assertIsNotNone(moved_at)
        self.assertLessEqual(moved_at, 5)
        self.assertIsNotNone(avg.last_parked)
        self.assertGreater(avg.last_parked["n"], 500)
        parked_at = None
        for s in range(120, 200):                              # stop 1440 m east
            lat, lon = fr.ll(1440.0 + rng.gauss(0, 2), rng.gauss(0, 2))
            avg.add(t + s, lat, lon, 120.0, speed=abs(rng.gauss(0, 0.05)))
            if avg.state == "parked" and parked_at is None:
                parked_at = s - 120
        self.assertIsNotNone(parked_at)
        self.assertLessEqual(parked_at, 25)

    def test_slow_creep_without_speed_is_detected(self):
        """A receiver that reports no speed: the position test alone must end the session, and a new
        one starts only after 20 s of real stillness at the new spot."""
        rng = random.Random(5)
        avg = A.StaticAverager()
        fr = A.Frame(LAT0, LON0)
        for s in range(900):
            lat, lon = fr.ll(rng.gauss(0, 1.5), rng.gauss(0, 1.5))
            avg.add(1.79e9 + s, lat, lon, None)
        self.assertEqual(avg.state, "parked")
        first = avg.session
        ended_at = None
        for s in range(900, 990):
            lat, lon = fr.ll(20.0 + rng.gauss(0, 1.5), rng.gauss(0, 1.5))
            avg.add(1.79e9 + s, lat, lon, None)
            if ended_at is None and avg.state != "parked":
                ended_at = s - 900
        self.assertIsNotNone(ended_at, "the 20 m move was not noticed")
        self.assertLessEqual(ended_at, 15)
        self.assertEqual(avg.last_parked["n"], 900 + ended_at - 1 if False else avg.last_parked["n"])
        self.assertGreater(avg.session, first)
        r = avg.result()
        self.assertIsNotNone(r, "no new session after stopping at the new spot")
        e, n = fr.en(r["lat"], r["lon"])
        self.assertAlmostEqual(e, 20.0, delta=3.0)


class TestWifi(unittest.TestCase):
    DUMP = (
        "BSS 02:11:22:33:44:55(on wlan0)\n"
        "\tlast seen: 32151.680s [boottime]\n"
        "\tfreq: 5680.0\n"
        "\tsignal: -70.00 dBm\n"
        "\tlast seen: 13478 ms ago\n"
        "\tSSID: Caf\\xc3\\xa9\\xf0\\x9f\\x93\\xb6Net\n"
        "BSS aa:bb:cc:00:11:22(on wlan0) -- associated\n"
        "\tfreq: 2412\n"
        "\tsignal: -41.50 dBm\n"
        "\tlast seen: 120 ms ago\n"
        "\tSSID: \n")

    def test_parse(self):
        b = A.parse_iw_dump(self.DUMP)
        self.assertEqual(len(b), 2)
        self.assertEqual(b[0]["bssid"], "02:11:22:33:44:55")
        self.assertEqual(b[0]["ssid"], "Caf\u00e9\U0001F4F6Net")
        self.assertEqual(b[0]["freq"], 5680)
        self.assertEqual(b[0]["dbm"], -70.0)
        self.assertEqual(b[0]["ageMs"], 13478)
        self.assertEqual(b[1]["ssid"], "")
        self.assertEqual(b[1]["dbm"], -41.5)

    def test_nm_mapping_matches_desktop(self):
        self.assertEqual(A.nm_strength_to_dbm(100), -40)
        self.assertEqual(A.nm_strength_to_dbm(0), -100)
        self.assertEqual(A.nm_strength_to_dbm(50), -70)
        self.assertEqual(A.nm_strength_to_dbm(77), -40 - (23 * 60) // 100)


class TestBle(unittest.TestCase):
    IDENT = "abcdefghjkmnpqrstvwxyz0123"

    def test_tag_and_layout(self):
        t = 1790478720.0
        w = int(t // 900)
        exp = hashlib.sha256(("beaconfix-ble-v1|%s|%d" % (self.IDENT, w)).encode()).digest()[:8]
        sd = A.ble_service_data(self.IDENT, t, tx_power=-7, kind=3, api=False)
        self.assertEqual(len(sd), 10)
        self.assertEqual(sd[:8], exp)
        self.assertEqual(sd[8], (-7) & 0xFF)
        self.assertEqual(sd[9], 3 << 2)
        ad = A.ble_adv_data(self.IDENT, t)
        self.assertEqual(len(ad), 28)
        self.assertEqual(ad[0], 27)
        self.assertEqual(ad[1], 0x21)
        self.assertEqual(ad[2:18], uuid.UUID(A.BLE_UUID).bytes[::-1])
        p = A.parse_service_data(sd)
        self.assertEqual(p["tag"], exp)
        self.assertEqual(p["txPower"], -7)
        self.assertEqual(p["kind"], "pi")

    def test_kind_is_three_bits(self):
        sd = A.ble_service_data(self.IDENT, 0.0, kind=4, calibrating=True)
        self.assertEqual(sd[9], (4 << 2) | 0x20)
        p = A.parse_service_data(sd)
        self.assertEqual(p["kind"], "gnss")
        self.assertTrue(p["calibrating"])
        self.assertEqual(A.parse_service_data(bytes(8) + bytes([0, 7 << 2]))["kind"], "other")

    def test_own_beacon_id_is_not_the_desktops(self):
        bid = A.beacon_id(self.IDENT, "pi")
        self.assertEqual(bid, hashlib.sha256(("beaconfix-agent-beacon-v1|%s|pi" % self.IDENT).encode()).hexdigest()[:26])
        self.assertNotEqual(bid, self.IDENT)
        b = A.Ble(threading.Event())
        b.set_identities(self.IDENT, "pi")
        self.assertEqual(b.identity, bid)                       # our advert
        w = int(time.time() // 900)
        self.assertIn(A.ble_tag(self.IDENT, w), b.tags())       # whose adverts we keep: the desktop's
        self.assertNotIn(A.ble_tag(bid, w), b.tags())

    def test_desktop_kind_bits(self):
        p = A.parse_service_data(bytes(8) + bytes([127, 0b00000011]))
        self.assertEqual(p["kind"], "desktop")
        self.assertTrue(p["rtt"] and p["api"])
        self.assertEqual(p["txPower"], 127)

    def test_agent_kind_is_pi_or_gnss_never_a_desktop(self):
        self.assertEqual(A.agent_kind_code("pi"), 3)
        self.assertEqual(A.agent_kind_code("GNSS"), 4)
        self.assertEqual(A.agent_kind_code("desktop"), 3)       # an agent must never pass for its desktop
        self.assertEqual(A.agent_kind_code(""), 3)

    def test_tx_power_request(self):
        self.assertEqual(A.adv_tx_request(7, True, -34, 7), (True, 7))
        self.assertEqual(A.adv_tx_request(7, True, -20, 4), (True, 4))      # clamped to the controller's maximum
        self.assertEqual(A.adv_tx_request(7, True, None, None), (True, 7))
        self.assertEqual(A.adv_tx_request(7, False, -34, 7), (False, 127))  # no CanSetTxPower: the controller picks
        self.assertEqual(A.adv_tx_request(127, True, -34, 7), (False, 127))
        self.assertEqual(A.adv_tx_request(7, True, 10, 0), (False, 127))    # nonsense range

    def test_advert_carries_its_tx_power_and_kind(self):
        b = A.Ble(threading.Event())
        b.set_identities(self.IDENT, "pi")
        b.kind_code = A.agent_kind_code("gnss")
        b.tx_settable, b.tx = A.adv_tx_request(A.BLE_TX_DBM, True, -34, 7)
        p = A.parse_service_data(b.service_data(1790478720.0))
        self.assertEqual(p["txPower"], 7)
        self.assertEqual(p["kind"], "gnss")
        self.assertEqual(p["tag"], A.ble_tag(A.beacon_id(self.IDENT, "pi"), int(1790478720.0 // 900)))
        b.tx_settable, b.tx = A.adv_tx_request(A.BLE_TX_DBM, False)
        self.assertEqual(A.parse_service_data(b.service_data(1790478720.0))["txPower"], 127)

    def test_adv_properties_fit_a_legacy_advert(self):
        sd = A.ble_service_data(self.IDENT, 0.0, tx_power=7)
        p = A.adv_properties(sd, 7, True, True)
        self.assertEqual(p["Type"], "broadcast")
        self.assertEqual(p["Includes"], ["tx-power"])
        self.assertEqual(p["TxPower"], 7)
        self.assertEqual(p["ServiceData"][A.BLE_UUID], sd)
        self.assertEqual(len(A.ble_adv_data(self.IDENT, 0.0)) + 3, 31)   # service data AD + TX-power AD = a full legacy PDU
        q = A.adv_properties(sd, 127, False, False)
        self.assertNotIn("TxPower", q)                          # absent, not 127: BlueZ rejects values outside −127…+20
        self.assertEqual(q["Includes"], [])


class TestQueue(unittest.TestCase):
    def test_put_peek_drop_cap(self):
        with tempfile.TemporaryDirectory() as d:
            q = A.Queue(os.path.join(d, "q.db"), cap=100)
            q.put("obs", [{"i": i} for i in range(150)])
            self.assertEqual(q.count("obs"), 100)
            ids, rows = q.peek("obs", 10)
            self.assertEqual(rows[0]["i"], 50)                # the oldest 50 were dropped
            q.drop("obs", ids)
            self.assertEqual(q.count("obs"), 90)


class TestUps(unittest.TestCase):
    def setUp(self):
        self._run = A.run
        self._exists = A.os.path.exists

    def tearDown(self):
        A.run = self._run
        A.os.path.exists = self._exists

    @staticmethod
    def i2c_fake(regs):
        def fake(cmd, timeout=10, **kw):
            if cmd[0] != "i2cget":
                return 127, "", ""
            key = (int(cmd[3], 16), int(cmd[4], 16))
            if key not in regs:
                return 1, "", "Error: Read failed"
            v = regs[key]                                     # big-endian register value → SMBus little-endian word
            return 0, "0x%04x\n" % (((v & 0xFF) << 8) | (v >> 8)), ""
        return fake

    def test_waveshare_ups_b_ina219(self):
        # 7.60 V bus (0x42): (7.60 / 0.004) << 3 = 15200; shunt +50 mV*... -> 0.05 A charging
        A.run = self.i2c_fake({(0x42, 0x02): int(7.60 / 0.004) << 3, (0x42, 0x01): 500})
        A.os.path.exists = lambda p: p == "/dev/i2c-1" or self._exists(p)
        r = A.Ups("auto")._ina219()
        self.assertEqual(r["source"], "ina219@0x42")
        self.assertAlmostEqual(r["voltage"], 7.60, places=2)
        volt = (int(7.60 / 0.004)) * 0.004                  # what the 4 mV register can hold
        self.assertEqual(r["percent"], round((volt - 6.0) / 2.4 * 100))
        self.assertTrue(r["charging"])

    def test_max17040(self):
        A.run = self.i2c_fake({(0x36, 0x08): 0x0003, (0x36, 0x04): (57 << 8) | 128, (0x36, 0x02): int(3.85 / 0.00125) << 4})
        A.os.path.exists = lambda p: p == "/dev/i2c-1" or self._exists(p)
        r = A.Ups("auto")._max1704x()
        self.assertEqual(r["percent"], 58)
        self.assertAlmostEqual(r["voltage"], 3.85, places=2)

    def test_pisugar_server(self):
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(4)
        port = srv.getsockname()[1]
        answers = {"get battery": "battery: 42.7", "get battery_charging": "battery_charging: false", "get battery_v": "battery_v: 3.71"}

        def serve():
            for _ in range(3):
                c, _ = srv.accept()
                q = c.recv(64).decode().strip()
                c.sendall((answers.get(q, "") + "\n").encode())
                c.close()
        th = threading.Thread(target=serve, daemon=True)
        th.start()
        real = A.socket.create_connection
        A.socket.create_connection = lambda addr, timeout=None: real(("127.0.0.1", port), timeout=timeout)
        try:
            r = A.Ups("auto")._pisugar()
        finally:
            A.socket.create_connection = real
            srv.close()
        self.assertEqual(r["percent"], 43)
        self.assertFalse(r["charging"])
        self.assertAlmostEqual(r["voltage"], 3.71)

    def test_none_mode(self):
        self.assertIsNone(A.Ups("none").read())


if __name__ == "__main__":
    unittest.main(verbosity=2)
