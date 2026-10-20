"""
Simulator panel APFC — meniru ESP32 (kontrak MQTT, algoritma pemilihan kombinasi step,
energi kumulatif, perintah + ack). Untuk mengembangkan dan menguji dashboard tanpa panel.

    python tools/simulator.py                  # step C1/C2/C3 = 2 / 4 / 6,36 µF (susunan panel sekarang)
    python tools/simulator.py --c 6.36,6.36,6.36

Beban di MCB berganti otomatis tiap ~40 s: kosong → 1 TL → 2 TL → 1 TL → ...
Beban K1 (kontak NC, bawaan menyala, setara 1 TL) bisa dimatikan/dinyalakan dari dashboard.
JANGAN dijalankan bersamaan dengan panel asli pada PANEL_ID yang sama.
"""
import argparse
import itertools
import json
import math
import random
import time

import paho.mqtt.client as mqtt

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="127.0.0.1")
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--panel", default="panel01")
ap.add_argument("--c", default="2,4,6.36", help="kapasitansi efektif step C1,C2,C3 (µF)")
ap.add_argument("--k1", default="32,72.6", help="P,Q beban K1 pada 225 V (W,var)")
ap.add_argument("--phase", type=float, default=40.0, help="detik per fase beban")
ap.add_argument("--fast", action="store_true", help="jeda keputusan dipercepat (uji)")
A = ap.parse_args()

BASE = f"apfc/{A.panel}/"
P = {"pf_target": 0.95, "hyst": 0.20, "lead_w": 1.2, "t_reconnect": 10, "n_avg": 5, "n_skip": 2, "p_min": 10.0,
     "c": [float(x) for x in A.c.split(",")], "tariff": 1114.74, "scale": 1000.0}
FW = "sim-1.2.0"

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
          "tariff": (1, 100000), "scale": (1, 1000)}


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
        else:
            P[key] = val
        S["dirty"] = True
        ack(cli, cid, True, f"{key} = {val:g}")
    elif cmd == "energy_reset":
        S["e"] = {k: 0.0 for k in S["e"]}
        ack(cli, cid, True, "penghitung energi direset")
    else:
        ack(cli, cid, False, "perintah tidak dikenal")


def status(cli):
    d = {"state": S["state"], "mode": S["mode"], "steps": S["steps"], "sockets": S["socks"], "meter_ok": 1,
         "fw": FW, "uptime": int(time.time() - S["t0"]), "ip": "192.168.137.50",
         "c_uf": P["c"], "pf_target": P["pf_target"]}
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
    last_status = 0
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
            if S["dirty"] or now - last_status > 60:
                status(cli)
                last_status = now
            time.sleep(2.0 - (time.time() - now) % 2.0)
    except KeyboardInterrupt:
        cli.publish(BASE + "online", "0", qos=1, retain=True).wait_for_publish(2)
        cli.loop_stop()


if __name__ == "__main__":
    main()
