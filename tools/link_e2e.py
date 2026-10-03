#!/usr/bin/env python3
"""Linking v3 end to end (docs/LINKING.md) against ISOLATED instances of build/beaconfix — never the live install:
a hub (beaconfix --server on loopback, plain HTTP), a PC (beaconfix --tray, offscreen, on a private D-Bus session whose
XDG_DATA_DIRS is empty so nothing installed is D-Bus-activated; its own HOME / XDG dirs; no mDNS, BLE or KWallet),
enrolled with that hub, and a phone played by this script with the functions of tools/link_ref.py + tools/bfs3_ref.py.

    python3 tools/link_e2e.py [path/to/beaconfix]

QR path: MAC → approved at once, same code, sealed payload (token + hub invite), single use, the token works, the phone
enrols with the hub using the invite and makes a sealed request, /link/hub-invite. mDNS path: commitment, key, Link
via D-Bus (what the dialog's button does), Reject, a key that does not match its commitment, at most 3 pending.
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import link_ref as L      # noqa: E402
import bfs3_ref as B      # noqa: E402
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey   # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS += 1


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def http(method, url, body=None, headers=None, raw=False, timeout=15):
    data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
    req = urllib.request.Request(url, data=data, method=method, headers=dict(headers or {}))
    if body is not None and "Content-Type" not in req.headers:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            b = r.read(); return r.status, (b if raw else json.loads(b or b"{}")), dict(r.headers)
    except urllib.error.HTTPError as e:
        b = e.read(); return e.code, (b if raw else json.loads(b or b"{}")), dict(e.headers)


def wait_http(url, secs=40):
    end = time.time() + secs
    while time.time() < end:
        try:
            urllib.request.urlopen(url, timeout=2).read(); return True
        except urllib.error.HTTPError:
            return True
        except Exception:
            time.sleep(0.3)
    return False


def iso_env(root, extra=None):
    """Everything this process may touch lives under root."""
    e = {k: v for k, v in os.environ.items() if k in ("PATH", "LANG", "LC_ALL", "DBUS_SESSION_BUS_ADDRESS")}
    for d in ("home", "cfg", "state", "data", "cache", "run"):
        os.makedirs(os.path.join(root, d), mode=0o700, exist_ok=True)
    e.update(HOME=f"{root}/home", XDG_CONFIG_HOME=f"{root}/cfg", XDG_STATE_HOME=f"{root}/state", XDG_DATA_HOME=f"{root}/data",
             XDG_DATA_DIRS=f"{root}/data", XDG_CACHE_HOME=f"{root}/cache", XDG_RUNTIME_DIR=f"{root}/run", QT_QPA_PLATFORM="offscreen",
             BEACONFIX_NO_MDNS="1", BEACONFIX_NO_BLE="1", BEACONFIX_NO_KWALLET="1")
    e.update(extra or {})
    return e


def dbus(env, method, *args):
    cmd = ["gdbus", "call", "--session", "--dest", "org.sworrl.BeaconFix", "--object-path", "/org/sworrl/BeaconFix", "--method", f"org.sworrl.BeaconFix.{method}",
           *["'" + a.replace("\\", "\\\\").replace("'", "\\'") + "'" for a in args]]
    out = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=90)
    if out.returncode:
        raise RuntimeError(out.stderr)
    s = out.stdout.strip()                    # "('…',)" / "(true,)"
    if s.startswith("('") and s.endswith("',)"):
        return s[2:-3].encode().decode("unicode_escape")
    return s


def x_pub(sk): return L.pub(sk)


def phone_keys():
    sk = X25519PrivateKey.generate(); return sk, L.b64u(x_pub(sk))


def shared_with(sk, pub_b64u): return sk.exchange(X25519PublicKey.from_public_bytes(B.unb64u(pub_b64u)))


def open_sealed(shared, sid, sealed):
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
    raw = B.unb64u(sealed)
    k = L.hkdf(shared, sid.encode(), b"beaconfix-link-payload-v1", 32)
    return json.loads(ChaCha20Poly1305(k).decrypt(raw[:12], raw[12:], f"bflink-payload\n{sid}".encode()))


def poll(base, sid, want=("approved", "denied"), secs=20):
    end = time.time() + secs
    while time.time() < end:
        st, o, _ = http("GET", f"{base}/api/v1/link/{sid}")
        if st != 200 or o.get("status") in want:
            return st, o
        time.sleep(0.5)
    return 0, {}


def hub_enroll(invite, name, kind):
    """The phone's side of BFS3 enrolment (docs/SECURE-API.md) with an invite from the link payload."""
    inv = json.loads(B.unb64u(invite[5:]))
    dsk = X25519PrivateKey.generate(); dpk = B.raw_pub(dsk); pub = B.b64u(dpk); ts = int(time.time())
    body = {"inviteId": inv["i"], "name": name, "kind": kind, "pub": pub, "ts": ts, "mac": B.enroll_mac(B.unb64u(inv["k"]), inv["i"], name, kind, pub, ts)}
    st, o, _ = http("POST", inv["u"].rstrip("/") + "/api/v3/enroll", body)
    spk = B.unb64u(inv["s"])
    rk = B.root_key(dsk.exchange(X25519PublicKey.from_public_bytes(spk)), spk, dpk)
    did = B.device_id(dpk)
    ok = st == 200 and o.get("deviceId") == did and o.get("proof") == B.proof(rk, did) and o.get("fingerprint") == B.fingerprint(spk)
    return ok, inv["u"].rstrip("/"), did, rk


def hub_get(url, did, rk, target, c):
    ts = int(time.time()); nonce = os.urandom(12)
    sealed = B.seal_request(rk, did, "GET", target, c, ts, nonce, b"")
    st, raw, h = http("GET", url + target, headers=B.request_headers(did, c, ts, nonce, sealed, False), raw=True)
    if "X-BF-Nonce" not in h:
        return st, {}
    return st, json.loads(B.open_response(rk, did, st, c, B.unb64u(h["X-BF-Nonce"]), raw))


def inner(root, exe):
    hp, ap, pp = free_port(), free_port(), free_port()
    # ── the hub ──
    hub_env = iso_env(f"{root}/hub", {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent", "BEACONFIX_HUB_LISTEN": f"127.0.0.1:{hp}",
                                      "BEACONFIX_HUB_ADMIN": f"127.0.0.1:{ap}", "BEACONFIX_HUB_URL": f"http://127.0.0.1:{hp}"})
    hub = subprocess.Popen([exe, "--server"], env=hub_env, stdout=open(f"{root}/hub.log", "w"), stderr=subprocess.STDOUT)
    pc = None
    try:
        check(wait_http(f"http://127.0.0.1:{hp}/healthz"), "hub up (isolated, loopback)")
        out = subprocess.run([exe, "--server", "--invite", "test-pc", "--kind", "desktop", "--json"], env=hub_env, capture_output=True, text=True, timeout=30)
        pc_invite = json.loads(out.stdout)["invite"]
        # ── the PC ──
        pc_env = iso_env(f"{root}/pc")
        os.makedirs(f"{root}/pc/cfg/sworrl", exist_ok=True)
        with open(f"{root}/pc/cfg/sworrl/beaconfix.conf", "w") as f:
            f.write(f"[General]\napiPort={pp}\napiKnownOnly=false\nuseStarlink=false\nuseIp=false\nuseApple=false\nintervalMinutes=60\nosTimeZone=false\nosGeoclue=false\nosNightLight=false\nosLocale=false\n")
        pc = subprocess.Popen([exe, "--tray"], env=pc_env, stdout=open(f"{root}/pc.log", "w"), stderr=subprocess.STDOUT)
        base = f"http://127.0.0.1:{pp}"
        check(wait_http(f"{base}/api/v1/hello"), f"PC up on port {pp}")
        st, hello, _ = http("GET", f"{base}/api/v1/hello")
        check(hello.get("api") == 3 and hello.get("link") is True, "hello: api 3, link")
        enr = json.loads(dbus(pc_env, "HubEnroll", pc_invite, "test-pc"))
        check(enr.get("ok") is True, f"PC enrolled with the hub ({enr.get('error', enr.get('deviceId'))})")

        # ── QR path ──
        offer = json.loads(dbus(pc_env, "LinkOffer"))
        sid = offer["sid"]
        qr_text = offer["qr"]
        for _ in range(40):                                   # the hub invite joins the QR within seconds
            q = json.loads(B.unb64u(qr_text[7:]))
            if q.get("hub"): break
            time.sleep(0.25); qr_text = dbus(pc_env, "LinkQr", sid)
        q = json.loads(B.unb64u(qr_text[7:]))
        check(qr_text.startswith("bflink:") and q["v"] == 1 and q["sid"] == sid and q["port"] == pp and len(B.unb64u(q["k"])) == 32 and q["e"] > time.time() + 500,
              "QR: v, sid, port, k, e")
        check(isinstance(q["hosts"], list) and any(h.endswith(".local") for h in q["hosts"]) and q["name"], f"QR: name {q['name']!r}, hosts {q['hosts']}")
        check(str(q.get("hub", "")).startswith("bfs3:"), "QR: a hub invite from the hub (POST /api/v3/hub/invites)")
        sk, pub = phone_keys()
        name, kind = "Test Pixel", "android"
        mac = L.qr_mac(B.unb64u(q["k"]), sid, name, kind, pub)
        st, o, _ = http("POST", f"{base}/api/v1/link", {"sid": sid, "name": name, "kind": kind, "pub": pub, "mac": "00" + mac[2:]})
        check(st == 403, f"QR: a wrong MAC is refused ({st})")
        st, o, _ = http("POST", f"{base}/api/v1/link", {"sid": sid, "name": name, "kind": kind, "pub": pub, "mac": mac})
        check(st == 202 and o.get("status") == "approved" and o.get("pub") == q["pub"], f"QR: approved at once ({st} {o.get('status')})")
        shared = shared_with(sk, q["pub"]); my_code = L.code(shared, sid)
        sess = {s["sid"]: s for s in json.loads(dbus(pc_env, "LinkSessions"))}
        check(sess.get(sid, {}).get("code") == my_code, f"QR: the PC shows the phone's code {my_code[:3]} {my_code[3:]}")
        st, o, _ = http("POST", f"{base}/api/v1/link", {"sid": sid, "name": name, "kind": kind, "pub": pub, "mac": mac})
        check(st == 409, f"QR: single use ({st})")
        st, o = poll(base, sid)
        check(st == 200 and o.get("status") == "approved" and o.get("sealed"), "QR: poll → approved + sealed")
        pl = open_sealed(shared, sid, o["sealed"])
        check(pl.get("scopes") == ["read", "control"] and pl.get("token") and pl["pc"]["port"] == pp and pl["pc"]["name"] == q["name"], "QR: payload opens (token, scopes, pc)")
        check(str(pl.get("hub", "")).startswith("bfs3:"), "QR: payload carries the hub invite")
        st, o, _ = http("GET", f"{base}/api/v1/link/{sid}")
        check(st == 404, f"QR: delivered once ({st})")
        tok = {"Authorization": "Bearer " + pl["token"]}
        st, me, _ = http("GET", f"{base}/api/v1/devices/me", headers=tok)
        check(st == 200 and me.get("name") == name and me.get("kind") == "android" and "control" in me.get("scopes", []), f"QR: the token works ({st} {me})")
        devs = json.loads(dbus(pc_env, "ApiStatus")).get("devices", [])
        check(any(d.get("name") == name and d.get("kind") == "android" for d in devs), "QR: the phone is listed under Devices")
        ok, hub_url, did, rk = hub_enroll(pl["hub"], name, kind)
        check(ok, "hub: the phone enrols with the invite from the payload (proof checks)")
        st, o = hub_get(hub_url, did, rk, "/api/v3/devices/me", 1)
        check(st == 200 and o.get("name") == name, f"hub: a sealed request as the phone ({st})")
        st, o, _ = http("POST", f"{base}/api/v1/link/hub-invite", {}, headers=tok)
        check(st == 200 and str(o.get("hub", "")).startswith("bfs3:"), f"POST /link/hub-invite → a fresh invite ({st})")
        lan = next((h for h in q["hosts"] if not h.endswith(".local")), "127.0.0.1")
        lbase = f"http://{lan}:{pp}"                          # not loopback: loopback callers get the local grant
        st, o, _ = http("POST", f"{lbase}/api/v1/link/hub-invite", {})
        check(st in (401, 403), f"POST /link/hub-invite without a token is refused ({st})")
        hstat = json.loads(subprocess.run([exe, "--server", "--status", "--json"], env=hub_env, capture_output=True, text=True, timeout=30).stdout)
        by = [i for i in hstat.get("invites", []) if i.get("by") not in (None, "admin")]
        check(len(by) >= 2, f"hub: invites made by the PC are recorded ({len(by)})")

        # ── mDNS path (from the LAN address: a separate rate budget) ──
        sk2, pub2 = phone_keys(); name2 = "Test Tablet"
        st, o, _ = http("POST", f"{base}/api/v1/link", {"name": name2, "kind": kind, "pub": pub2})
        check(st == 400, f"mDNS: a key without a commitment is refused ({st})")
        st, o, _ = http("POST", f"{base}/api/v1/link", {"name": name2, "kind": kind, "commit": L.commit(name2, kind, pub2), "proximity": {"beacons": []}})
        check(st == 202 and o.get("status") == "commit" and len(o.get("sid", "")) == 16, f"mDNS: commitment → sid + PC key ({st})")
        sid2, pcpub2 = o["sid"], o["pub"]
        st, o, _ = http("POST", f"{base}/api/v1/link/{sid2}", {"pub": pub2})
        check(st == 202 and o.get("status") == "pending", f"mDNS: key revealed → pending ({st})")
        code2 = L.code(shared_with(sk2, pcpub2), sid2)
        s2 = {s["sid"]: s for s in json.loads(dbus(pc_env, "LinkSessions"))}.get(sid2, {})
        check(s2.get("code") == code2 and s2.get("state") == "pending" and "label" in s2.get("proximity", {}), f"mDNS: the PC prompt shows code {code2[:3]} {code2[3:]} + distance")
        st, o, _ = http("GET", f"{base}/api/v1/link/{sid2}")
        check(st == 200 and o.get("status") == "pending", "mDNS: pending until Link")
        check(dbus(pc_env, "LinkApprove", sid2) == "(true,)", "mDNS: Link tapped")
        st, o = poll(base, sid2)
        pl2 = open_sealed(shared_with(sk2, pcpub2), sid2, o.get("sealed", "")) if o.get("status") == "approved" else {}
        check(pl2.get("token") and pl2.get("scopes") == ["read", "control"] and str(pl2.get("hub", "")).startswith("bfs3:"), "mDNS: payload opens (token + fresh hub invite)")
        st, me, _ = http("GET", f"{base}/api/v1/devices/me", headers={"Authorization": "Bearer " + pl2.get("token", "")})
        check(st == 200 and me.get("name") == name2, "mDNS: the token works")
        # Reject
        sk3, pub3 = phone_keys()
        st, o, _ = http("POST", f"{lbase}/api/v1/link", {"name": "Stranger", "kind": kind, "commit": L.commit("Stranger", kind, pub3)})
        sid3 = o.get("sid", "")
        http("POST", f"{lbase}/api/v1/link/{sid3}", {"pub": pub3})
        check(dbus(pc_env, "LinkReject", sid3) == "(true,)", "mDNS: Reject tapped")
        st, o = poll(lbase, sid3)
        check(o.get("status") == "denied", f"mDNS: rejected → denied ({o.get('reason')})")
        # A key that is not the committed one (what a MITM that ground a matching code would have to send)
        sk4, pub4 = phone_keys(); _, other = phone_keys()
        st, o, _ = http("POST", f"{lbase}/api/v1/link", {"name": "Mallory", "kind": kind, "commit": L.commit("Mallory", kind, pub4)})
        st, o, _ = http("POST", f"{lbase}/api/v1/link/{o.get('sid', '')}", {"pub": other})
        check(st == 403, f"mDNS: a key that does not match the commitment is refused ({st})")
        # ≤ 3 waiting
        codes = []
        for i in range(4):
            _, p = phone_keys()
            st, o, _ = http("POST", f"{lbase}/api/v1/link", {"name": f"P{i}", "kind": kind, "commit": L.commit(f"P{i}", kind, p)})
            codes.append(st)
        check(codes == [202, 202, 202, 429], f"mDNS: at most 3 waiting ({codes})")
    finally:
        for p in (pc, hub):
            if p:
                p.terminate()
                try: p.wait(10)
                except subprocess.TimeoutExpired: p.kill()


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--inner":
        inner(sys.argv[2], sys.argv[3])
        print(f"\n{'FAILED' if FAILS else 'all end-to-end link checks passed'} ({FAILS} failure{'s' if FAILS != 1 else ''})")
        sys.exit(1 if FAILS else 0)
    exe = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "beaconfix"))
    root = tempfile.mkdtemp(prefix="bf-link-e2e-")
    os.makedirs(f"{root}/empty", mode=0o700, exist_ok=True)
    # A private session bus: XDG_DATA_DIRS / XDG_DATA_HOME empty → no installed .service file is activated
    env = {k: v for k, v in os.environ.items() if k in ("PATH", "LANG", "LC_ALL")}
    env.update(XDG_DATA_HOME=f"{root}/empty", XDG_DATA_DIRS=f"{root}/empty", HOME=f"{root}/empty", XDG_RUNTIME_DIR=f"{root}/empty")
    r = subprocess.run(["dbus-run-session", "--", sys.executable, os.path.abspath(__file__), "--inner", root, exe], env=env)
    if r.returncode == 0:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print(f"(logs kept in {root}: hub.log, pc.log)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
