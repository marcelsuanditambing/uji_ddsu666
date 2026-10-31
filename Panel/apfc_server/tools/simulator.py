"""
Simulator panel APFC — meniru ESP32 (kontrak MQTT, algoritma pemilihan kombinasi step,
energi kumulatif, perintah + ack). Untuk mengembangkan dan menguji dashboard tanpa panel.

    python tools/simulator.py                  # step C1/C2/C3 = 2 / 4 / 6,36 µF (susunan panel sekarang)
    python tools/simulator.py --c 6.36,6.36,6.36

Beban di MCB berganti otomatis tiap ~40 s: kosong → 1 TL → 2 TL → 1 TL → ...
Beban K1 (kontak NC, bawaan menyala, setara 1 TL) bisa dimatikan/dinyalakan dari dashboard.
microSD tiruan (firmware 1.3.0): web http://127.0.0.1:8081/ untuk menguji unduh/hapus log di tab Log
(file contoh dibuat di folder sementara; matikan dengan --sd-port 0).
JANGAN dijalankan bersamaan dengan panel asli pada PANEL_ID yang sama.
"""
import argparse
import itertools
import json
import math
import os
import random
import re
import tempfile
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--panel", default="panel01")
ap.add_argument("--c", default="2,4,6.36", help="kapasitansi efektif step C1,C2,C3 (µF)")
ap.add_argument("--k1", default="32,72.6", help="P,Q beban K1 pada 225 V (W,var)")
ap.add_argument("--phase", type=float, default=40.0, help="detik per fase beban")
ap.add_argument("--fast", action="store_true", help="jeda keputusan dipercepat (uji)")
ap.add_argument("--sd-port", type=int, default=8081, help="port web microSD tiruan (0 = mati)")
A = ap.parse_args()

BASE = f"apfc/{A.panel}/"
P = {"pf_target": 0.95, "hyst": 0.20, "lead_w": 1.2, "t_reconnect": 10, "n_avg": 5, "n_skip": 2, "p_min": 10.0,
     "c": [float(x) for x in A.c.split(",")], "tariff": 1114.74, "scale": 1000.0,
     "tarif_kwh": 1444.70, "pbjt": 8.0, "pasca": 0}
FW = "sim-1.4.0"

# beban (pada 225 V): TL 18 W + ballast → P 32 W, Q 72.6 var ; kompresor kecil (ilustratif)
TL = (32.0, 72.6)
K1 = tuple(float(x) for x in A.k1.split(","))
PHASES = [0, 1, 2, 1]   # jumlah TL di MCB per fase

S = {"mode": "AUTO", "state": "INIT", "steps": [0, 0, 0], "socks": [1], "off_t": [time.time()] * 3,
     "on_t": [0.0] * 3, "seq": 0, "ok": 0, "win": [], "skip": 0, "q_need": 0.0, "q_load": 0.0,
     "e": {"kwh": 0.0, "kvarh_ind": 0.0, "kvarh_cap": 0.0, "kvarh_load": 0.0}, "prev": None, "dirty": True,
     "t0": time.time(), "kwh_m": 12.34}


def qstep(k, v, f):
    return 2 * math.pi * f * P["c"][k] * 1e-6 * v * v


def wait_s(k, now):
    if S["steps"][k]:
        return 0
    el = now - S["off_t"][k]
    return max(0, math.ceil(P["t_reconnect"] - el))


def event(cli, etype, detail):
    cli.publish(BASE + "event", json.dumps({"ts": int(time.time()), "type": etype, "detail": detail}), qos=1)
    print(f"[EVENT] {etype} {detail}")
    S["dirty"] = True


def set_step(k, on, now):
    S["steps"][k] = 1 if on else 0
    if on:
        S["on_t"][k] = now
    else:
        S["off_t"][k] = now
    S["skip"] = P["n_skip"]
    S["win"].clear()
    S["dirty"] = True


def load_now(now):
    n_tl = PHASES[int((now - S["t0"]) / A.phase) % len(PHASES)]
    v = 220.0 + 3.0 * math.sin(now / 37.0) + random.uniform(-0.3, 0.3)
    k = (v / 225.0) ** 2
    p = (n_tl * TL[0] + (K1[0] if S["socks"][0] else 0)) * k
    q = (n_tl * TL[1] + (K1[1] if S["socks"][0] else 0)) * k
    p += random.uniform(-0.4, 0.4) if p > 0 else 0
    q += random.uniform(-0.6, 0.6) if q > 0 else 0
    return v, 50.0 + random.uniform(-0.05, 0.05), max(p, 0.0), max(q, 0.0)


def decide(cli, now, pm, qm, vm, fm):
    if S["mode"] != "AUTO" or S["state"] != "AUTO":
        return
    qs = [qstep(k, vm, fm) for k in range(3)]
    q_on = sum(qs[k] for k in range(3) if S["steps"][k])
    qt = pm * math.tan(math.acos(P["pf_target"]))
    q_load = qm + q_on
    S["q_need"] = qm - qt
    if pm < P["p_min"]:
        if any(S["steps"]):
            for k in range(3):
                if S["steps"][k]:
                    set_step(k, 0, now)
            event(cli, "STEP_OFF", "semua (P < p_min)")
        return
    if abs(qm) <= qt:
        return
    cost = lambda qa: qa if qa >= 0 else -qa * P["lead_w"]
    cur = tuple(S["steps"])
    best, best_score = cur, cost(qm)
    cur_cost = best_score
    for m in itertools.product((0, 1), repeat=3):
        if m == cur:
            continue
        new_on = [k for k in range(3) if m[k] and not cur[k]]
        if len(new_on) > 1 or any(wait_s(k, now) > 0 for k in new_on):
            continue
        qa = q_load - sum(qs[k] for k in range(3) if m[k])
        sc = cost(qa)
        if sc < best_score:
            best, best_score = m, sc
    if best == cur or best_score > cur_cost - P["hyst"] * min(qs):
        return
    for k in range(3):
        if best[k] != cur[k]:
            set_step(k, best[k], now)
            event(cli, "STEP_ON" if best[k] else "STEP_OFF", f"C{k+1} Qneed={S['q_need']:.0f} Qload={q_load:.0f}")


def ack(cli, cid, ok, msg):
    cli.publish(BASE + "cmd/ack", json.dumps({"id": cid, "ok": 1 if ok else 0, "msg": msg}), qos=1)
    print(f"[{'OK' if ok else 'DITOLAK'}] {msg}")
    if not ok:
        event(cli, "CMD_REJECT", f"{cid}: {msg}")


RANGES = {"pf_target": (0.90, 0.99), "hyst": (0.05, 0.9), "lead_w": (1.0, 5.0), "t_reconnect": (10, 60),
          "n_avg": (1, 10), "n_skip": (0, 5), "p_min": (0, 200), "c1": (0.5, 20), "c2": (0.5, 20), "c3": (0.5, 20),
          "tariff": (1, 100000), "scale": (1, 1000), "tarif_kwh": (100, 10000), "pbjt": (0, 25), "pasca": (0, 1)}


def on_message(cli, ud, msg):
    try:
        d = json.loads(msg.payload)
    except ValueError:
        return
    cid, cmd, now = d.get("id", "-"), d.get("cmd"), time.time()
    on = 1 if d.get("on") in (1, True, "1") else 0
    if cmd == "set_mode":
        S["mode"] = "MANUAL" if d.get("mode") == "MANUAL" else "AUTO"
        if S["state"] in ("AUTO", "MANUAL"):
            S["state"] = S["mode"]
            S["win"].clear()
        event(cli, "MODE", S["mode"])
        ack(cli, cid, True, f"mode {S['mode']}")
    elif cmd == "set_step":
        k = int(d.get("step", 0)) - 1
        if not 0 <= k < 3:
            return ack(cli, cid, False, "step harus 1-3")
        if S["state"] != "MANUAL":
            return ack(cli, cid, False, f"step hanya bisa diatur di MANUAL (sekarang {S['state']})")
        if on and wait_s(k, now) > 0:
            return ack(cli, cid, False, f"jeda sambung ulang, tunggu {wait_s(k, now)} s")
        set_step(k, on, now)
        event(cli, "STEP_ON" if on else "STEP_OFF", f"C{k+1} {'ON' if on else 'OFF'}")
        ack(cli, cid, True, f"C{k+1} {'ON' if on else 'OFF'}")
    elif cmd == "set_socket":
        k = int(d.get("socket", 0)) - 1
        if k != 0:
            return ack(cli, cid, False, "hanya beban 1 (relay K1)")
        S["socks"][0] = on
        S["dirty"] = True
        S["win"].clear()
        event(cli, "LOAD", "beban K1 menyala" if on else "beban K1 padam")
        ack(cli, cid, True, f"beban K1 {'dinyalakan' if on else 'dimatikan'}")
    elif cmd == "set_param":
        key, val = str(d.get("key", "")), float(d.get("value", 0))
        if key not in RANGES:
            return ack(cli, cid, False, f"kunci tidak dikenal: {key}")
        lo, hi = RANGES[key]
        if not lo <= val <= hi:
            return ack(cli, cid, False, f"{key} harus {lo:g}..{hi:g}")
        if key in ("c1", "c2", "c3"):
            P["c"][int(key[1]) - 1] = val
        elif key == "pasca":
            P[key] = 1 if val >= 0.5 else 0
        else:
            P[key] = val
        S["dirty"] = True
        ack(cli, cid, True, f"{key} = {val:g}")
    elif cmd == "energy_reset":
        S["e"] = {k: 0.0 for k in S["e"]}
        ack(cli, cid, True, "penghitung energi direset")
    else:
        ack(cli, cid, False, "perintah tidak dikenal")


# ----------------------------------------------------------------------------- microSD tiruan
SD_DIR = os.path.join(tempfile.gettempdir(), f"apfc_sim_sd_{A.panel}")
SD_TOTAL_MB, SD_MIN_FREE_MB = 7580, 100
SD_HDR = ("ts_device,uptime_s,seq,v,i,p,q,s,pf,f,q_load,q_need,kwh,kvarh_ind,kvarh_cap,kvarh_load,"
          "s1,s2,s3,beban_k1,mode,state")
SD_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,19}\.csv$")
sd_lock = threading.Lock()


def sd_today():
    return f"{datetime.now():%Y%m%d}.csv"


def sd_used_mb():
    return 5 + sum(os.path.getsize(os.path.join(SD_DIR, f)) for f in os.listdir(SD_DIR)) // 1048576


def sd_seed():
    """Buat contoh file harian 3 hari terakhir (data rumah 2200 VA sintetis) + event.csv."""
    os.makedirs(SD_DIR, exist_ok=True)
    for back in (3, 2, 1):
        day = datetime.now() - timedelta(days=back)
        path = os.path.join(SD_DIR, f"{day:%Y%m%d}.csv")
        if os.path.exists(path):
            continue
        t0 = int(day.replace(hour=0, minute=0, second=0, microsecond=0).timestamp())
        kwh = 0.0
        with open(path, "w", newline="") as fh:
            fh.write(SD_HDR + "\n")
            for n in range(0, 8640, 3):                     # tiap 30 s agar file contoh tidak terlalu besar
                t = t0 + n * 10
                h = (t - t0) / 3600
                p = 250 + 900 * max(0, math.sin((h - 6) / 12 * math.pi)) + random.uniform(-40, 40)
                q = 0.18 * p + random.uniform(-10, 10)
                v = 218 + random.uniform(-3, 3)
                s_ = math.hypot(p, q)
                kwh += p * 30 / 3.6e6
                fh.write(f"{t},{n * 10},{n},{v:.1f},{s_ / v:.3f},{p:.1f},{q:.1f},{s_:.1f},{p / s_:.3f},50.00,"
                         f"{q:.1f},{q - p * 0.3287:.1f},{kwh:.5f},{kwh * 0.18:.5f},0.00000,{kwh * 0.18:.5f},0,0,0,1,AUTO,AUTO\n")
    ev = os.path.join(SD_DIR, "event.csv")
    if not os.path.exists(ev):
        with open(ev, "w") as fh:
            fh.write('{"ts":%d,"type":"MODE","detail":"AUTO"}\n' % int(time.time() - 3 * 86400))


def sd_append(tel):
    with sd_lock:
        path = os.path.join(SD_DIR, sd_today())
        new = not os.path.exists(path)
        with open(path, "a", newline="") as fh:
            if new:
                fh.write(SD_HDR + "\n")
            st = tel["steps"]
            fh.write(f"{tel['ts']},{int(time.time() - S['t0'])},{tel['seq']},{tel['v']},{tel['i']},{tel['p']},{tel['q']},"
                     f"{tel['s']},{tel['pf']},{tel['f']},{tel['q_load']},{tel['q_need']},{tel['kwh']:.5f},"
                     f"{tel['kvarh_ind']:.5f},{tel['kvarh_cap']:.5f},{tel['kvarh_load']:.5f},{st[0]},{st[1]},{st[2]},"
                     f"{tel['sockets'][0]},{tel['mode']},{tel['state']}\n")


# ---------------- tagihan bulanan tiruan (firmware 1.4.0: GET /bill, status "bill") ----------------
# Rumah ±7,2 kWh/hari ditambah beban simulator; riwayat 3 bulan contoh (bulan tertua "belum sebulan penuh").
HOME_KWH_DAY = 7.2
BILL_HIST = [(-1, 214.6, 0), (-2, 198.3, 0), (-3, 61.9, 1)]


def ym_add(y, m, k):
    n = y * 12 + (m - 1) + k
    return n // 12, n % 12 + 1


def bill_cost(kwh, apply_min):
    min_kwh = 40 * 2.2 if (apply_min and P["pasca"]) else 0.0
    kb = max(kwh, min_kwh)
    energi = kb * P["tarif_kwh"]
    pbjt = energi * P["pbjt"] / 100
    return {"energi": energi, "pbjt": pbjt, "total": energi + pbjt, "min": kwh < min_kwh}


def bill_now():
    now = time.time()
    lt = time.localtime(now)
    y, m = lt.tm_year, lt.tm_mon
    t_from = time.mktime((y, m, 1, 0, 0, 0, 0, 0, -1))
    ny, nm = ym_add(y, m, 1)
    t_to = time.mktime((ny, nm, 1, 0, 0, 0, 0, 0, -1))
    el = now - t_from
    kwh = HOME_KWH_DAY * el / 86400 + (S["kwh_m"] - 12.34)
    proj = kwh / el * (t_to - t_from) if el >= 86400 else None
    hist = []
    for k, kw, fl in BILL_HIST:
        hy, hm = ym_add(y, m, k)
        hist.append({"ym": hy * 100 + hm, "kwh": kw, "total": round(bill_cost(kw, not fl)["total"]), "fl": fl})
    return {"ym": y * 100 + m, "from": int(t_from), "to": int(t_to), "start": int(t_from), "kwh": kwh, "proj": proj,
            "hist": hist}


class SdHandler(BaseHTTPRequestHandler):
    """Meniru web server firmware 1.3.0: /sd/list, /sd/file, /sd/delete."""
    def log_message(self, *a):
        pass

    def _json(self, code, d):
        b = json.dumps(d).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urlparse(self.path)
        name = (parse_qs(u.query).get("name") or [""])[0]
        if u.path == "/sd/list":
            with sd_lock:
                files = sorted(({"name": f, "size": os.path.getsize(os.path.join(SD_DIR, f))}
                                for f in os.listdir(SD_DIR) if SD_NAME.match(f)), key=lambda x: x["name"], reverse=True)
                used = sd_used_mb()
            self._json(200, {"ok": 1, "panel": A.panel, "fw": FW, "today": sd_today(), "total_mb": SD_TOTAL_MB,
                             "used_mb": used, "free_mb": SD_TOTAL_MB - used, "min_free_mb": SD_MIN_FREE_MB,
                             "count": len(files), "files": files[:500]})   # firmware: maks. 500 file terbaru
        elif u.path == "/sd/file":
            path = os.path.join(SD_DIR, name)
            if not SD_NAME.match(name) or not os.path.exists(path):
                return self._json(404, {"ok": 0, "msg": "file tidak ditemukan"})
            with sd_lock, open(path, "rb") as fh:
                data = fh.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif u.path == "/bill":
            b = bill_now()
            c = bill_cost(b["kwh"], False)
            cur = {"ym": b["ym"], "from": b["from"], "to": b["to"], "start": b["start"], "kwh": round(b["kwh"], 3),
                   "energi": round(c["energi"]), "pbjt_rp": round(c["pbjt"]), "total": round(c["total"]), "fl": 0, "src": "meter"}
            if b["proj"] is not None:
                cp = bill_cost(b["proj"], True)
                cur.update({"proj_kwh": round(b["proj"], 2), "proj_total": round(cp["total"]), "proj_min": int(cp["min"])})
            self._json(200, {"ok": 1, "panel": A.panel, "fw": FW, "now": int(time.time()), "time_ok": 1,
                             "tarif": P["tarif_kwh"], "pbjt": P["pbjt"], "pasca": P["pasca"], "daya_va": 2200,
                             "min_kwh": 88.0 if P["pasca"] else 0.0, "cur": cur, "hist": b["hist"]})
        elif u.path == "/":
            self._json(200, {"ok": 1, "msg": "simulator: halaman HTML hanya ada di firmware asli"})
        else:
            self._json(404, {"ok": 0, "msg": "alamat tidak dikenal"})

    def do_POST(self):
        u = urlparse(self.path)
        name = (parse_qs(u.query).get("name") or [""])[0]
        if u.path != "/sd/delete":
            return self._json(404, {"ok": 0, "msg": "alamat tidak dikenal"})
        if not SD_NAME.match(name):
            return self._json(400, {"ok": 0, "msg": "nama file tidak valid"})
        if name == sd_today():
            return self._json(409, {"ok": 0, "msg": "file hari ini sedang ditulis, tidak boleh dihapus"})
        path = os.path.join(SD_DIR, name)
        with sd_lock:
            if not os.path.exists(path):
                return self._json(404, {"ok": 0, "msg": "file tidak ditemukan atau gagal dihapus"})
            os.remove(path)
        self._json(200, {"ok": 1, "msg": f"{name} dihapus"})
        if SD_CLI:
            event(SD_CLI, "SD_DELETE", f"{name} dihapus lewat web")


SD_CLI = None


def status(cli):
    d = {"state": S["state"], "mode": S["mode"], "steps": S["steps"], "sockets": S["socks"], "meter_ok": 1,
         "fw": FW, "uptime": int(time.time() - S["t0"]), "ip": "127.0.0.1" if A.sd_port else "192.168.137.50",
         "c_uf": P["c"], "pf_target": P["pf_target"],
         "sd_ok": 1 if A.sd_port else 0, "sd_total_mb": SD_TOTAL_MB if A.sd_port else 0,
         "sd_used_mb": sd_used_mb() if A.sd_port else 0, "web": A.sd_port}
    b = bill_now()
    d["bill"] = {"ok": 1, "ym": b["ym"], "kwh": round(b["kwh"], 2), "rp": round(bill_cost(b["kwh"], False)["total"]),
                 "fl": 0, "tarif": P["tarif_kwh"], "pbjt": P["pbjt"], "pasca": P["pasca"]}
    if b["proj"] is not None:
        d["bill"].update({"proj_kwh": round(b["proj"], 1), "proj_rp": round(bill_cost(b["proj"], True)["total"])})
    cli.publish(BASE + "status", json.dumps(d), qos=1, retain=True)
    S["dirty"] = False


def main():
    try:
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"apfc-{A.panel}")
    except AttributeError:
        cli = mqtt.Client(client_id=f"apfc-{A.panel}")
    cli.will_set(BASE + "online", "0", qos=1, retain=True)
    cli.on_message = on_message
    cli.connect(A.host, A.port, keepalive=30)
    cli.subscribe(BASE + "cmd", qos=1)
    cli.publish(BASE + "online", "1", qos=1, retain=True)
    cli.loop_start()
    print(f"Simulator panel '{A.panel}' → {A.host}:{A.port}, C = {P['c']} µF. Ctrl+C untuk berhenti.")
    global SD_CLI
    if A.sd_port:
        sd_seed()
        SD_CLI = cli
        srv = ThreadingHTTPServer(("127.0.0.1", A.sd_port), SdHandler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        print(f"microSD tiruan: http://127.0.0.1:{A.sd_port}/sd/list  (folder {SD_DIR})")
    last_status = 0
    last_sd = 0
    try:
        while True:
            now = time.time()
            v, f, pl, ql = load_now(now)
            qc = sum(qstep(k, v, f) for k in range(3) if S["steps"][k])
            p, q = pl, ql - qc
            s = math.hypot(p, q)
            i = s / v if v else 0
            pf = p / s if s > 1e-6 else 1.0
            # energi (trapesium)
            if S["prev"]:
                dt = now - S["prev"][0]
                pp, qq, qlq = S["prev"][1:]
                e = S["e"]
                e["kwh"] += 0.5 * (p + pp) * dt / 3.6e6
                e["kvarh_ind"] += 0.5 * (max(q, 0) + max(qq, 0)) * dt / 3.6e6
                e["kvarh_cap"] += 0.5 * (max(-q, 0) + max(-qq, 0)) * dt / 3.6e6
                e["kvarh_load"] += 0.5 * (ql + qlq) * dt / 3.6e6
                S["kwh_m"] += 0.5 * (p + pp) * dt / 3.6e6
            S["prev"] = (now, p, q, ql)
            S["q_load"] = q + qc
            # keadaan
            if S["state"] == "INIT":
                S["ok"] += 1
                if S["ok"] >= 3:
                    S["state"] = S["mode"]
                    event(cli, "MODE", S["state"])
            elif S["skip"] > 0:
                S["skip"] -= 1
            else:
                S["win"].append((p, q, v, f))
                S["win"] = S["win"][-P["n_avg"]:]
                if len(S["win"]) >= (2 if A.fast else P["n_avg"]):
                    n = len(S["win"])
                    pm, qm, vm, fm = (sum(w[j] for w in S["win"]) / n for j in range(4))
                    decide(cli, now, pm, qm, vm, fm)
            S["seq"] += 1
            tel = {"ts": int(now), "seq": S["seq"], "v": round(v, 1), "i": round(i, 3), "p": round(p, 1),
                   "q": round(q, 1), "s": round(s, 1), "pf": round(pf, 3), "f": round(f, 2),
                   "q_load": round(S["q_load"], 1), "q_need": round(S["q_need"], 1),
                   **{k: round(val, 6) for k, val in S["e"].items()}, "kwh_m": round(S["kwh_m"], 2),
                   "steps": S["steps"], "sockets": S["socks"], "mode": S["mode"], "state": S["state"],
                   "rssi": -55 + random.randint(-3, 3)}
            cli.publish(BASE + "telemetry", json.dumps(tel))
            if A.sd_port and now - last_sd >= 10:
                last_sd = now
                sd_append(tel)
            if S["dirty"] or now - last_status > 60:
                status(cli)
                last_status = now
            time.sleep(2.0 - (time.time() - now) % 2.0)
    except KeyboardInterrupt:
        cli.publish(BASE + "online", "0", qos=1, retain=True).wait_for_publish(2)
        cli.loop_stop()


if __name__ == "__main__":
    main()
