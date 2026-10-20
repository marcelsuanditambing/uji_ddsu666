"""
APFC Panel — backend + dashboard web (TDB-APFC-01 Subbab 7.6.2)
Tim Aku Anak Sehat · DeepTech for Smart-X Hackathon 2026

Alur: ESP32 → MQTT (Mosquitto) → backend ini → SQLite + CSV harian → browser (SSE)
      browser → POST /api/cmd → MQTT apfc/panel01/cmd → ESP32 → cmd/ack → browser

Jalankan:  python app.py            (lihat python app.py --help)
Lalu buka: http://localhost:5000    (atau http://<IP laptop>:5000 dari HP)
"""
import argparse
import csv
import json
import os
import queue
import re
import socket
import sqlite3
import threading
import time
import uuid
from datetime import datetime

from flask import Flask, Response, abort, jsonify, render_template, request, send_from_directory
import paho.mqtt.client as mqtt

import energy

# ----------------------------------------------------------------------------- konfigurasi
ap = argparse.ArgumentParser(description="Backend + dashboard APFC Panel")
ap.add_argument("--mqtt-host", default=os.environ.get("APFC_MQTT_HOST", "127.0.0.1"))
ap.add_argument("--mqtt-port", type=int, default=int(os.environ.get("APFC_MQTT_PORT", 1883)))
ap.add_argument("--mqtt-user", default=os.environ.get("APFC_MQTT_USER", ""))
ap.add_argument("--mqtt-pass", default=os.environ.get("APFC_MQTT_PASS", ""))
ap.add_argument("--panel", default=os.environ.get("APFC_PANEL", "panel01"))
ap.add_argument("--port", type=int, default=int(os.environ.get("APFC_HTTP_PORT", 5000)))
ap.add_argument("--tariff", type=float, default=1114.74, help="Rp/kVArh (asumsi)")
ap.add_argument("--scale", type=float, default=1000.0, help="skala tampilan setara industri")
ap.add_argument("--t-reconnect", type=int, default=10, help="detik jeda sambung ulang step (sama dengan firmware)")
ap.add_argument("--data", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "data"))
CFG = ap.parse_args()

BASE = f"apfc/{CFG.panel}/"
os.makedirs(CFG.data, exist_ok=True)
START_T = time.time()

# ----------------------------------------------------------------------------- penyimpanan
DB_PATH = os.path.join(CFG.data, "apfc.db")
db = sqlite3.connect(DB_PATH, check_same_thread=False)
db.row_factory = sqlite3.Row
db_lock = threading.Lock()
with db_lock:
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("""CREATE TABLE IF NOT EXISTS telemetry (
        t REAL, ts_dev INTEGER, seq INTEGER,
        v REAL, i REAL, p REAL, q REAL, s REAL, pf REAL, f REAL, q_load REAL, q_need REAL,
        kwh REAL, kwh_m REAL, kvarh_ind REAL, kvarh_cap REAL, kvarh_load REAL,
        s1 INTEGER, s2 INTEGER, s3 INTEGER, sock1 INTEGER, sock2 INTEGER,
        mode TEXT, state TEXT, rssi INTEGER)""")
    db.execute("CREATE INDEX IF NOT EXISTS ix_tel_t ON telemetry(t)")
    db.execute("CREATE TABLE IF NOT EXISTS events (t REAL, ts_dev INTEGER, type TEXT, detail TEXT, src TEXT)")
    db.execute("CREATE INDEX IF NOT EXISTS ix_ev_t ON events(t)")
    db.commit()

TEL_COLS = ["v", "i", "p", "q", "s", "pf", "f", "q_load", "q_need",
            "kwh", "kwh_m", "kvarh_ind", "kvarh_cap", "kvarh_load"]
CSV_TEL_HEAD = ["time_server", "ts_device", "seq"] + TEL_COLS + ["s1", "s2", "s3", "beban_k1", "k2_tidak_dipakai",
                                                                  "mode", "state", "rssi"]


def csv_append(kind, head, row):
    path = os.path.join(CFG.data, f"{datetime.now():%Y-%m-%d}_{kind}.csv")
    new = not os.path.exists(path)
    try:
        with open(path, "a", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            if new:
                w.writerow(head)
            w.writerow(row)
    except OSError as e:      # mis. berkas sedang dibuka di Excel → baris ini dilewati, SQLite tetap mencatat
        print(f"[CSV] gagal menulis {os.path.basename(path)}: {e}")


def store_telemetry(t, d):
    steps = (list(d.get("steps") or []) + [0, 0, 0])[:3]
    socks = (list(d.get("sockets") or []) + [0, 0])[:2]
    vals = [d.get(k) for k in TEL_COLS]
    with db_lock:
        db.execute(f"INSERT INTO telemetry VALUES ({','.join('?' * 25)})",
                   [t, d.get("ts"), d.get("seq")] + vals + steps + socks +
                   [d.get("mode"), d.get("state"), d.get("rssi")])
        db.commit()
    csv_append("telemetry", CSV_TEL_HEAD,
               [datetime.fromtimestamp(t).isoformat(timespec="seconds"), d.get("ts"), d.get("seq")] +
               vals + steps + socks + [d.get("mode"), d.get("state"), d.get("rssi")])


def store_event(t, ts_dev, etype, detail, src="panel"):
    with db_lock:
        db.execute("INSERT INTO events VALUES (?,?,?,?,?)", (t, ts_dev, etype, detail, src))
        db.commit()
    csv_append("event", ["time_server", "ts_device", "type", "detail", "source"],
               [datetime.fromtimestamp(t).isoformat(timespec="seconds"), ts_dev, etype, detail, src])


# ----------------------------------------------------------------------------- keadaan & SSE
state = {
    "telemetry": None,      # telemetry terakhir (+ _rx, _wait)
    "status": None,         # status retained terakhir
    "online": None,         # True/False/None (belum tahu)
    "broker": False,        # backend terhubung ke broker
    "rx_t": None,           # waktu terima telemetry terakhir (server)
}
state_lock = threading.Lock()
step_prev = [None, None, None]
step_off_t = [0.0, 0.0, 0.0]

clients = set()
clients_lock = threading.Lock()


def broadcast(kind, data):
    msg = f"event: {kind}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"
    with clients_lock:
        dead = []
        for q in clients:
            try:
                q.put_nowait(msg)
            except queue.Full:
                dead.append(q)
        for q in dead:
            clients.discard(q)


def snapshot():
    with state_lock:
        return {
            "telemetry": state["telemetry"],
            "status": state["status"],
            "online": state["online"],
            "broker": state["broker"],
            "server_t": time.time(),
            "session_start": START_T,
            "panel": CFG.panel,
            "tariff": CFG.tariff,
            "scale": CFG.scale,
            "t_reconnect": CFG.t_reconnect,
        }


# ----------------------------------------------------------------------------- MQTT
pending = {}            # id → {"ev": Event, "res": dict}
pending_lock = threading.Lock()


def on_connect(client, userdata, *a):
    rc = a[1] if len(a) > 1 else a[0]
    ok = (getattr(rc, "value", rc) == 0)
    with state_lock:
        state["broker"] = ok
    if ok:
        client.subscribe(BASE + "#", qos=1)
        print(f"[MQTT] terhubung ke {CFG.mqtt_host}:{CFG.mqtt_port}, berlangganan {BASE}#")
    else:
        print(f"[MQTT] koneksi ditolak: {rc}")
    broadcast("broker", {"ok": ok})


def on_disconnect(client, userdata, *a):
    with state_lock:
        state["broker"] = False
    print("[MQTT] terputus dari broker, mencoba lagi...")
    broadcast("broker", {"ok": False})


def handle_telemetry(t, d):
    # deteksi step lepas untuk hitung mundur jeda sambung ulang
    steps = (list(d.get("steps") or []) + [0, 0, 0])[:3]
    for k in range(3):
        if step_prev[k] == 1 and steps[k] == 0:
            step_off_t[k] = t
        step_prev[k] = steps[k]
    d["_rx"] = t
    d["_wait"] = [max(0, int(round(CFG.t_reconnect - (t - step_off_t[k])))) if not steps[k] and step_off_t[k] else 0
                  for k in range(3)]
    with state_lock:
        state["telemetry"] = d
        state["rx_t"] = t
        was_online = state["online"]
        if not state["online"]:
            state["online"] = True
    if was_online is False:
        store_event(t, None, "ONLINE", "data panel diterima lagi", "backend")
        broadcast("event", {"t": t, "type": "ONLINE", "detail": "data panel diterima lagi", "src": "backend"})
        broadcast("online", {"online": True})
    store_telemetry(t, d)
    broadcast("telemetry", d)


def on_message(client, userdata, msg):
    t = time.time()
    sub = msg.topic[len(BASE):] if msg.topic.startswith(BASE) else msg.topic
    raw = msg.payload.decode("utf-8", "replace")
    try:
        if sub == "telemetry":
            handle_telemetry(t, json.loads(raw))
        elif sub == "status":
            d = json.loads(raw)
            d["_rx"] = t
            with state_lock:
                state["status"] = d
            broadcast("status", d)
        elif sub == "event":
            d = json.loads(raw)
            store_event(t, d.get("ts"), d.get("type", "?"), d.get("detail", ""))
            broadcast("event", {"t": t, "type": d.get("type"), "detail": d.get("detail"), "src": "panel"})
        elif sub == "cmd/ack":
            d = json.loads(raw)
            with pending_lock:
                p = pending.get(d.get("id"))
                if p:
                    p["res"] = d
                    p["ev"].set()
            broadcast("ack", d)
        elif sub == "online":
            on = raw.strip() == "1"
            with state_lock:
                changed = state["online"] is not None and state["online"] != on
                state["online"] = on
            if changed:
                etype = "ONLINE" if on else "OFFLINE"
                detail = "panel terhubung ke broker" if on else "panel terputus (LWT)"
                store_event(t, None, etype, detail, "backend")
                broadcast("event", {"t": t, "type": etype, "detail": detail, "src": "backend"})
            broadcast("online", {"online": on})
        # topik "cmd" (kiriman backend sendiri) diabaikan
    except (ValueError, TypeError) as e:
        print(f"[MQTT] payload {sub} tidak valid: {e}: {raw[:120]}")
    except Exception as e:      # jangan sampai satu pesan menghentikan thread MQTT
        print(f"[MQTT] galat saat memproses {sub}: {e!r}")


try:
    mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"apfc-backend-{uuid.uuid4().hex[:6]}")
except AttributeError:          # paho-mqtt 1.x
    mq = mqtt.Client(client_id=f"apfc-backend-{uuid.uuid4().hex[:6]}")
if CFG.mqtt_user:
    mq.username_pw_set(CFG.mqtt_user, CFG.mqtt_pass)
mq.on_connect = on_connect
mq.on_disconnect = on_disconnect
mq.on_message = on_message
mq.reconnect_delay_set(1, 10)


def watchdog():
    """Tandai panel offline bila telemetry berhenti > 15 s (mis. Wi-Fi putus tanpa LWT)."""
    while True:
        time.sleep(2)
        with state_lock:
            rx, on = state["rx_t"], state["online"]
        if on and rx and time.time() - rx > 15:
            with state_lock:
                state["online"] = False
            t = time.time()
            store_event(t, None, "OFFLINE", "tidak ada telemetry > 15 s", "backend")
            broadcast("event", {"t": t, "type": "OFFLINE", "detail": "tidak ada telemetry > 15 s", "src": "backend"})
            broadcast("online", {"online": False})


# ----------------------------------------------------------------------------- HTTP
app = Flask(__name__)
app.json.ensure_ascii = False


@app.get("/")
def index():
    return render_template("index.html", panel=CFG.panel)


@app.get("/stream")
def stream():
    q = queue.Queue(maxsize=200)
    with clients_lock:
        clients.add(q)

    def gen():
        try:
            yield "retry: 2000\n"
            yield f"event: hello\ndata: {json.dumps(snapshot())}\n\n"
            while True:
                try:
                    yield q.get(timeout=15)
                except queue.Empty:
                    yield ": ping\n\n"
        finally:
            with clients_lock:
                clients.discard(q)

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.get("/api/latest")
def api_latest():
    return jsonify(snapshot())


def period_bounds():
    now = time.time()
    rng = request.args.get("range") or request.args.get("period")
    if request.args.get("min"):
        return now - float(request.args["min"]) * 60, now
    if rng == "today":
        d = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
        return d.timestamp(), now
    if rng == "session":
        return START_T, now
    if rng == "all":
        return 0.0, now
    if request.args.get("from"):
        f = datetime.fromisoformat(request.args["from"]).timestamp()
        to = datetime.fromisoformat(request.args["to"]).timestamp() if request.args.get("to") else now
        return f, to
    return now - 600, now


@app.get("/api/history")
def api_history():
    t0, t1 = period_bounds()
    span = max(1.0, t1 - t0)
    bucket = max(2.0, span / 900.0)          # ≤ ~900 titik per seri
    with db_lock:
        if bucket <= 2.0:
            rows = db.execute("SELECT t,pf,p,q,q_load,v,s1,s2,s3 FROM telemetry WHERE t>=? AND t<=? ORDER BY t",
                              (t0, t1)).fetchall()
        else:
            rows = db.execute(
                "SELECT AVG(t) t, AVG(pf) pf, AVG(p) p, AVG(q) q, AVG(q_load) q_load, AVG(v) v, "
                "MAX(s1) s1, MAX(s2) s2, MAX(s3) s3 FROM telemetry WHERE t>=? AND t<=? "
                "GROUP BY CAST(t / ? AS INTEGER) ORDER BY t", (t0, t1, bucket)).fetchall()
        marks = db.execute("SELECT t,type,detail FROM events WHERE t>=? AND t<=? AND type IN "
                           "('STEP_ON','STEP_OFF','MODE','LOAD','FAILSAFE','RECOVER') ORDER BY t", (t0, t1)).fetchall()
    return jsonify({"from": t0, "to": t1, "bucket": bucket,
                    "rows": [dict(r) for r in rows], "marks": [dict(m) for m in marks]})


@app.get("/api/energy")
def api_energy():
    t0, t1 = period_bounds()
    scale = float(request.args.get("scale", CFG.scale))
    with db_lock:
        rows = db.execute("SELECT t,kwh,kvarh_ind,kvarh_cap,kvarh_load FROM telemetry "
                          "WHERE t>=? AND t<=? ORDER BY t", (t0, t1)).fetchall()
    first = rows[0]["t"] if rows else None
    last = rows[-1]["t"] if rows else None
    res = energy.summarize(rows, CFG.tariff, scale, first, last)
    res.update({"from": t0, "to": t1, "first": first, "last": last})
    return jsonify(res)


@app.get("/api/events")
def api_events():
    limit = min(int(request.args.get("limit", 200)), 2000)
    etype = request.args.get("type")
    with db_lock:
        if etype:
            rows = db.execute("SELECT * FROM events WHERE type=? ORDER BY t DESC LIMIT ?", (etype, limit)).fetchall()
        else:
            rows = db.execute("SELECT * FROM events ORDER BY t DESC LIMIT ?", (limit,)).fetchall()
    return jsonify([dict(r) for r in rows])


CSV_RE = re.compile(r"^\d{4}-\d{2}-\d{2}_(telemetry|event)\.csv$")


@app.get("/api/csv")
def api_csv_list():
    files = sorted((f for f in os.listdir(CFG.data) if CSV_RE.match(f)), reverse=True)
    return jsonify([{"name": f, "size": os.path.getsize(os.path.join(CFG.data, f))} for f in files])


@app.get("/api/csv/<name>")
def api_csv_get(name):
    if not CSV_RE.match(name):
        abort(404)
    return send_from_directory(CFG.data, name, as_attachment=True)


ALLOWED = {
    "set_mode": {"mode"},
    "set_step": {"step", "on"},
    "set_socket": {"socket", "on"},
    "set_param": {"key", "value"},
    "energy_reset": set(),
}


@app.post("/api/cmd")
def api_cmd():
    body = request.get_json(silent=True) or {}
    cmd = body.get("cmd")
    if cmd not in ALLOWED:
        return jsonify({"ok": 0, "msg": f"perintah tidak dikenal: {cmd}"}), 400
    with state_lock:
        broker = state["broker"]
    if not broker:
        return jsonify({"ok": 0, "msg": "backend belum terhubung ke broker MQTT"}), 503
    cid = uuid.uuid4().hex[:6]
    payload = {"id": cid, "cmd": cmd}
    for k in ALLOWED[cmd]:
        if k in body:
            payload[k] = body[k]
    ev = threading.Event()
    with pending_lock:
        pending[cid] = {"ev": ev, "res": None}
    mq.publish(BASE + "cmd", json.dumps(payload), qos=1)
    got = ev.wait(3.0)
    with pending_lock:
        res = pending.pop(cid)["res"]
    if not got:
        return jsonify({"id": cid, "ok": 0, "timeout": True, "msg": "tidak ada jawaban dari panel dalam 3 s"})
    return jsonify(res)


def lan_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127."))


if __name__ == "__main__":
    mq.connect_async(CFG.mqtt_host, CFG.mqtt_port, keepalive=30)
    mq.loop_start()
    threading.Thread(target=watchdog, daemon=True).start()
    print("=" * 60)
    print(f" APFC dashboard — panel '{CFG.panel}', broker {CFG.mqtt_host}:{CFG.mqtt_port}")
    print(f" Buka di laptop : http://localhost:{CFG.port}")
    for ip in lan_ips():
        print(f" Buka dari HP   : http://{ip}:{CFG.port}")
    print(f" Data disimpan  : {CFG.data}")
    print("=" * 60)
    app.run(host="0.0.0.0", port=CFG.port, threaded=True, debug=False)
