"""
DDSU666 Power Monitor + CSV Logger
==================================
Membaca meter CHINT DDSU666 lewat Modbus RTU (USB-RS485), menampilkan dashboard
web lokal, dan MENCATAT SETIAP SAMPEL KE CSV (satu file per hari).

Yang dicatat per sampel:
  waktu, V, I, P, Q, S (=V*I), PF, frekuensi, energi meter (kWh),
  serta akumulasi kWh dan kVArh (induktif/kapasitif) hasil integrasi P*dt dan Q*dt.
  DDSU666 tidak punya register kVArh, jadi kVArh dihitung di sini.

Cara pakai:
  python monitor_ddsu666.py                  # pakai meter di COM5
  python monitor_ddsu666.py --port COM7      # port lain
  python monitor_ddsu666.py --simulate       # tanpa meter (uji dashboard & CSV)
  Buka http://127.0.0.1:5000

Data tersimpan di folder ./data :
  data/ddsu666_YYYY-MM-DD.csv   -> data mentah per sampel
  data/state.json               -> total kWh/kVArh agar tidak hilang saat program ditutup
"""

import argparse
import csv
import json
import math
import os
import random
import signal
import struct
import sys
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path

from flask import Flask, jsonify, render_template_string, send_file, abort

# ---------------------------------------------------------------------------
# Konfigurasi default (bisa diganti lewat argumen command line)
# ---------------------------------------------------------------------------
PORT = "COM5"
SLAVE_ID = 1
BAUDRATE = 9600            # meter: bAUd-3
STOPBITS = 1               # meter: n-8n1
POLL_INTERVAL_S = 2.0      # jeda antar pembacaan
DATA_DIR = "data"
WORD_SWAP = False          # sudah terverifikasi: high-word-first (False)
MAX_GAP_S = 10.0           # jika jeda antar sampel > ini, integrasi kVArh dilewati
RECONNECT_AFTER = 3        # buka ulang port serial setelah N kali gagal berturut-turut
HISTORY_POINTS = 300       # titik grafik di dashboard (300 x 2 s = 10 menit)
CONTRACT_VA = 2200         # daya tersambung rumah, hanya untuk estimasi % beban

# Batas wajar untuk menolak pembacaan rusak (misal urutan word salah / noise)
VALID = {
    "voltage_v": (150, 280),
    "current_a": (0, 80),
    "p_kw": (-20, 20),
    "q_kvar": (-20, 20),
    "pf": (-1.05, 1.05),
    "freq_hz": (45, 55),
}

CSV_FIELDS = [
    "timestamp", "status",
    "voltage_v", "current_a", "active_power_w", "reactive_power_var",
    "apparent_power_va", "power_factor", "frequency_hz",
    "energy_kwh_meter",
    "kwh_log", "kvarh_ind_log", "kvarh_cap_log",
    "read_ms", "error",
]


# ---------------------------------------------------------------------------
# Akses meter
# ---------------------------------------------------------------------------
def regs_to_float(hi, lo, swap=WORD_SWAP):
    if swap:
        hi, lo = lo, hi
    return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]


class DDSU666:
    """Pembaca DDSU666 lewat Modbus RTU (FC03, float32 high-word-first)."""

    def __init__(self, port, slave_id, baudrate, stopbits):
        self.port, self.slave_id = port, slave_id
        self.baudrate, self.stopbits = baudrate, stopbits
        self.inst = None
        self.open()

    def open(self):
        import minimalmodbus
        import serial
        self.close()
        inst = minimalmodbus.Instrument(self.port, self.slave_id, mode=minimalmodbus.MODE_RTU)
        inst.serial.baudrate = self.baudrate
        inst.serial.bytesize = 8
        inst.serial.parity = serial.PARITY_NONE
        inst.serial.stopbits = self.stopbits
        inst.serial.timeout = 1.0
        inst.clear_buffers_before_each_transaction = True
        inst.close_port_after_each_call = False
        self.inst = inst

    def close(self):
        try:
            if self.inst is not None and self.inst.serial.is_open:
                self.inst.serial.close()
        except Exception:
            pass

    def read(self):
        r = self.inst.read_registers(0x2000, 16, functioncode=3)
        e = self.inst.read_registers(0x4000, 2, functioncode=3)
        return {
            "voltage_v": regs_to_float(r[0], r[1]),
            "current_a": regs_to_float(r[2], r[3]),
            "p_kw": regs_to_float(r[4], r[5]),
            "q_kvar": regs_to_float(r[6], r[7]),
            "pf": regs_to_float(r[10], r[11]),
            "freq_hz": regs_to_float(r[14], r[15]),
            "energy_kwh": regs_to_float(e[0], e[1]),
        }


class SimulatedMeter:
    """Meter tiruan untuk menguji dashboard & CSV tanpa hardware."""

    def __init__(self):
        self.energy = 20.0
        self.t0 = time.monotonic()
        self.last = time.monotonic()

    def open(self):
        pass

    def close(self):
        pass

    def read(self):
        now = time.monotonic()
        t = now - self.t0
        v = 217 + 1.5 * math.sin(t / 40) + random.uniform(-0.3, 0.3)
        p = 1.45 + 0.12 * math.sin(t / 25) + random.uniform(-0.02, 0.02)
        q = 0.21 + random.uniform(-0.01, 0.01)
        if int(t) % 60 < 6:            # sesekali "motor menyala"
            q += 0.45
            p += 0.25
        s = math.hypot(p, q) * 1.008   # sedikit distorsi harmonik
        self.energy += p * (now - self.last) / 3600
        self.last = now
        return {
            "voltage_v": v, "current_a": s * 1000 / v, "p_kw": p, "q_kvar": q,
            "pf": p / s, "freq_hz": 50 + random.uniform(-0.04, 0.04),
            "energy_kwh": self.energy,
        }


def validate(raw):
    for key, (lo, hi) in VALID.items():
        val = raw[key]
        if not (math.isfinite(val) and lo <= val <= hi):
            raise ValueError(f"nilai {key}={val!r} di luar batas wajar {lo}..{hi}")
    if not (math.isfinite(raw["energy_kwh"]) and raw["energy_kwh"] >= 0):
        raise ValueError(f"energi tidak wajar: {raw['energy_kwh']!r}")


# ---------------------------------------------------------------------------
# Pencatat CSV + akumulasi energi
# ---------------------------------------------------------------------------
class Logger:
    def __init__(self, data_dir):
        self.dir = Path(data_dir)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.state_path = self.dir / "state.json"
        self.day = None
        self.fh = None
        self.writer = None
        self.rows_today = 0
        self.totals = {"kwh_log": 0.0, "kvarh_ind_log": 0.0, "kvarh_cap_log": 0.0,
                       "since": datetime.now().astimezone().isoformat(timespec="seconds")}
        self._load_state()
        self._last_save = 0.0   # simpan pada baris pertama, lalu tiap 30 s

    def _load_state(self):
        try:
            data = json.loads(self.state_path.read_text(encoding="utf-8"))
            for k in self.totals:
                if k in data:
                    self.totals[k] = data[k]
        except FileNotFoundError:
            pass
        except Exception as exc:
            print(f"[peringatan] state.json tidak terbaca ({exc}); total dimulai dari 0.")

    def save_state(self):
        tmp = self.state_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.totals, indent=2), encoding="utf-8")
        os.replace(tmp, self.state_path)

    def reset_totals(self):
        self.totals.update(kwh_log=0.0, kvarh_ind_log=0.0, kvarh_cap_log=0.0,
                           since=datetime.now().astimezone().isoformat(timespec="seconds"))
        self.save_state()

    def path_for(self, day):
        return self.dir / f"ddsu666_{day}.csv"

    def _rotate(self, now):
        day = now.strftime("%Y-%m-%d")
        if day == self.day:
            return
        if self.fh:
            self.fh.close()
        path = self.path_for(day)
        new = not path.exists() or path.stat().st_size == 0
        self.rows_today = 0 if new else max(0, sum(1 for _ in open(path, encoding="utf-8")) - 1)
        self.fh = open(path, "a", newline="", encoding="utf-8")
        self.writer = csv.DictWriter(self.fh, fieldnames=CSV_FIELDS)
        if new:
            self.writer.writeheader()
        self.day = day

    def integrate(self, prev, cur, dt_s):
        """Integrasi trapesium P*dt dan Q*dt (kW, kvar -> kWh, kVArh)."""
        h = dt_s / 3600.0
        self.totals["kwh_log"] += 0.5 * (prev["p_kw"] + cur["p_kw"]) * h
        q = 0.5 * (prev["q_kvar"] + cur["q_kvar"])
        if q >= 0:
            self.totals["kvarh_ind_log"] += q * h
        else:
            self.totals["kvarh_cap_log"] += -q * h

    def write(self, now, row):
        self._rotate(now)
        self.writer.writerow(row)
        self.fh.flush()
        self.rows_today += 1
        if time.monotonic() - self._last_save > 30:
            self.save_state()
            self._last_save = time.monotonic()

    def close(self):
        try:
            self.save_state()
        finally:
            if self.fh:
                self.fh.close()


def r(x, n):
    return None if x is None else round(x, n)


# ---------------------------------------------------------------------------
# Thread pembaca (satu-satunya yang menyentuh port serial)
# ---------------------------------------------------------------------------
class Poller(threading.Thread):
    def __init__(self, meter, logger, interval, meta):
        super().__init__(daemon=True)
        self.meter, self.logger, self.interval, self.meta = meter, logger, interval, meta
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.latest = {"ok": False, "error": "Menunggu pembacaan pertama..."}
        self.history = deque(maxlen=HISTORY_POINTS)
        self.prev = None          # (monotonic_time, raw) sampel valid sebelumnya
        self.fail_streak = 0
        self.ok_count = 0
        self.err_count = 0

    def run(self):
        next_t = time.monotonic()
        while not self.stop_event.is_set():
            self.poll_once()
            next_t += self.interval
            delay = next_t - time.monotonic()
            if delay < 0:                       # tertinggal: jangan menumpuk
                next_t = time.monotonic()
                delay = 0
            self.stop_event.wait(delay)

    def poll_once(self):
        now = datetime.now().astimezone()
        t0 = time.monotonic()
        try:
            raw = self.meter.read()
            validate(raw)
        except Exception as exc:
            self._on_error(now, exc, (time.monotonic() - t0) * 1000)
            return
        read_ms = (time.monotonic() - t0) * 1000
        self.fail_streak = 0
        self.ok_count += 1

        if self.prev is not None:
            dt = t0 - self.prev[0]
            if 0 < dt <= MAX_GAP_S:
                self.logger.integrate(self.prev[1], raw, dt)
        self.prev = (t0, raw)

        s_va = raw["voltage_v"] * raw["current_a"]
        tot = self.logger.totals
        row = {
            "timestamp": now.isoformat(timespec="seconds"), "status": "OK",
            "voltage_v": r(raw["voltage_v"], 2), "current_a": r(raw["current_a"], 3),
            "active_power_w": r(raw["p_kw"] * 1000, 1),
            "reactive_power_var": r(raw["q_kvar"] * 1000, 1),
            "apparent_power_va": r(s_va, 1), "power_factor": r(raw["pf"], 3),
            "frequency_hz": r(raw["freq_hz"], 2), "energy_kwh_meter": r(raw["energy_kwh"], 3),
            # format tetap agar tidak muncul notasi 6e-05 di Excel
            "kwh_log": f'{tot["kwh_log"]:.5f}', "kvarh_ind_log": f'{tot["kvarh_ind_log"]:.5f}',
            "kvarh_cap_log": f'{tot["kvarh_cap_log"]:.5f}',
            "read_ms": r(read_ms, 0), "error": "",
        }
        try:
            self.logger.write(now, row)
            log_err = None
        except Exception as exc:
            log_err = f"Gagal menulis CSV: {exc}"

        with self.lock:
            self.latest = {**row, "ok": True, "log_error": log_err,
                           "loading_pct": r(s_va / CONTRACT_VA * 100, 1)}
            self.history.append({"t": row["timestamp"], "p": row["active_power_w"],
                                 "q": row["reactive_power_var"], "pf": row["power_factor"]})

    def _on_error(self, now, exc, read_ms):
        self.fail_streak += 1
        self.err_count += 1
        msg = f"{type(exc).__name__}: {exc}"
        self.prev = None            # jangan integrasi melewati celah data
        try:
            self.logger.write(now, {"timestamp": now.isoformat(timespec="seconds"),
                                    "status": "ERR", "read_ms": r(read_ms, 0), "error": msg})
        except Exception:
            pass
        with self.lock:
            self.latest = {**self.latest, "ok": False, "error": msg,
                           "error_time": now.isoformat(timespec="seconds")}
        if self.fail_streak % RECONNECT_AFTER == 0:
            try:
                self.meter.open()
            except Exception:
                pass

    def snapshot(self):
        with self.lock:
            data = dict(self.latest)
        data.update(
            kwh_log=r(self.logger.totals["kwh_log"], 5),
            kvarh_ind_log=r(self.logger.totals["kvarh_ind_log"], 5),
            kvarh_cap_log=r(self.logger.totals["kvarh_cap_log"], 5),
            totals_since=self.logger.totals["since"],
            csv_file=str(self.logger.path_for(self.logger.day)) if self.logger.day else None,
            rows_today=self.logger.rows_today, ok_count=self.ok_count, err_count=self.err_count,
            **self.meta,
        )
        return data

    def history_list(self):
        with self.lock:
            return list(self.history)


# ---------------------------------------------------------------------------
# Web
# ---------------------------------------------------------------------------
app = Flask(__name__)
poller = None  # diisi di main()

HTML = r"""
<!doctype html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DDSU666 Power Monitor</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--panel:#121c2e;--line:#25344d;--text:#edf3ff;--muted:#98a9c4;--blue:#65a8ff;--amber:#fbbf24}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 Segoe UI,Arial,sans-serif}
main{max-width:1120px;margin:auto;padding:28px 18px 44px}header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:12px;margin-bottom:22px}
h1{margin:0;font-size:clamp(23px,4vw,32px)}h2{font-size:18px;margin:0 0 12px}.subtitle,.label,.small,footer{color:var(--muted)}
.subtitle{margin-top:4px}.status{border:1px solid var(--line);border-radius:999px;padding:7px 12px;color:var(--muted)}
.status.ok{color:#86efac;border-color:#276749}.status.bad{color:#fca5a5;border-color:#7f1d1d}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}.card{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:18px;min-width:0}
.card.acc{border-color:#4a3b12}.card.acc .value{color:var(--amber)}
.label{font-size:13px}.value{font-size:clamp(22px,3vw,32px);font-weight:700;margin-top:7px;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}.unit{font-size:14px;color:var(--muted);font-weight:500}
.small{font-size:12px;margin-top:8px}.section-title{margin:26px 0 10px;font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted)}
.trend-panel{margin-top:14px}.trend-header{display:flex;justify-content:space-between;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:12px}.trend-header h2{margin:0}
.trend-controls{display:flex;gap:8px;flex-wrap:wrap}.btn{background:#0d1626;color:#c5d4ec;border:1px solid #344761;border-radius:9px;padding:9px 13px;font-size:13px;cursor:pointer;text-decoration:none;display:inline-block}
.btn:hover{border-color:#65a8ff}.btn.active{background:#1d4d83;border-color:#65a8ff;color:#fff;font-weight:700}
canvas{width:100%;height:340px;display:block}
.meter-info{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}.info{background:#0d1626;border:1px solid var(--line);border-radius:12px;padding:12px;min-width:0}
.info .v{margin-top:6px;font-weight:600;overflow-wrap:anywhere}
.error{display:none;background:#451a1a;border:1px solid #7f1d1d;color:#fecaca;padding:13px;border-radius:12px;margin-bottom:14px;white-space:pre-wrap}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
footer{margin-top:18px;font-size:12px}
@media(max-width:850px){.grid,.meter-info{grid-template-columns:repeat(2,minmax(0,1fr))}canvas{height:280px}}
@media(max-width:430px){main{padding:18px 12px 30px}.card{padding:14px}.grid{gap:9px}}
</style>
</head>
<body><main>
<header><div><h1>DDSU666 Power Monitor</h1><div class="subtitle">Pemantauan listrik rumah · CHINT DDSU666 · Modbus RTU · <span id="mode"></span></div></div><div id="status" class="status">Menghubungkan...</div></header>
<div id="error" class="error"></div>

<section class="grid">
<div class="card"><div class="label">Tegangan</div><div class="value"><span id="voltage">—</span> <span class="unit">V</span></div><div class="small">Voltage RMS</div></div>
<div class="card"><div class="label">Arus</div><div class="value"><span id="current">—</span> <span class="unit">A</span></div><div class="small">Current RMS</div></div>
<div class="card"><div class="label">Daya aktif</div><div class="value"><span id="power">—</span> <span class="unit">W</span></div><div class="small">P</div></div>
<div class="card"><div class="label">Power factor</div><div class="value" id="pf">—</div><div class="small">cos φ (dari meter)</div></div>
<div class="card"><div class="label">Daya reaktif</div><div class="value"><span id="reactive">—</span> <span class="unit">VAr</span></div><div class="small">Q (+ induktif / − kapasitif)</div></div>
<div class="card"><div class="label">Daya semu</div><div class="value"><span id="apparent">—</span> <span class="unit">VA</span></div><div class="small">S = V × I</div></div>
<div class="card"><div class="label">Frekuensi</div><div class="value"><span id="frequency">—</span> <span class="unit">Hz</span></div><div class="small">Frekuensi jaringan</div></div>
<div class="card"><div class="label">Energi aktif meter</div><div class="value"><span id="energy">—</span> <span class="unit">kWh</span></div><div class="small">Register 4000H</div></div>
</section>

<div class="section-title">Akumulasi pencatatan <span id="since"></span></div>
<section class="grid">
<div class="card"><div class="label">Energi aktif (log)</div><div class="value"><span id="kwhlog">—</span> <span class="unit">kWh</span></div><div class="small">∫ P dt</div></div>
<div class="card acc"><div class="label">Energi reaktif induktif</div><div class="value"><span id="kvarhind">—</span> <span class="unit">kVArh</span></div><div class="small">∫ Q dt, Q &gt; 0</div></div>
<div class="card"><div class="label">Energi reaktif kapasitif</div><div class="value"><span id="kvarhcap">—</span> <span class="unit">kVArh</span></div><div class="small">∫ |Q| dt, Q &lt; 0</div></div>
<div class="card"><div class="label">Rasio kVArh / kWh</div><div class="value" id="ratio">—</div><div class="small">Referensi PLN: 0,62 (≈ cos φ 0,85)</div></div>
</section>

<section class="card trend-panel">
<div class="trend-header">
  <h2>Tren Pengukuran</h2>
  <div class="trend-controls" role="group" aria-label="Pilih grafik">
    <button class="btn active" data-chart="p" type="button">Daya aktif</button>
    <button class="btn" data-chart="q" type="button">Daya reaktif</button>
    <button class="btn" data-chart="pf" type="button">Power factor</button>
  </div>
</div>
<canvas id="trendChart"></canvas>
<div id="chartCaption" class="small"></div>
</section>

<section class="card trend-panel">
<h2>Status pencatatan</h2>
<div class="meter-info">
<div class="info"><div class="label">File CSV hari ini</div><div class="v" id="csvfile">—</div></div>
<div class="info"><div class="label">Baris tercatat hari ini</div><div class="v" id="rows">—</div></div>
<div class="info"><div class="label">Sukses / gagal (sesi)</div><div class="v" id="okerr">—</div></div>
<div class="info"><div class="label">Update terakhir</div><div class="v" id="updated">—</div></div>
<div class="info"><div class="label">Port · Modbus</div><div class="v" id="portinfo">—</div></div>
<div class="info"><div class="label">Interval</div><div class="v" id="interval">—</div></div>
<div class="info"><div class="label">Waktu baca meter</div><div class="v" id="readms">—</div></div>
<div class="info"><div class="label">Beban vs daya tersambung</div><div class="v" id="loading">—</div></div>
</div>
<div class="actions"><a class="btn" href="/download/today">Unduh CSV hari ini</a><a class="btn" href="/files">Daftar semua file CSV</a></div>
</section>
<footer>Data dibaca di latar belakang dan dicatat ke CSV walaupun halaman ini ditutup. Jangan mengubah kabel terminal AC saat bertegangan.</footer>
</main>
<script>
const $=id=>document.getElementById(id);
const fmt=(n,d=2)=>(typeof n==='number'&&Number.isFinite(n))?n.toLocaleString('id-ID',{minimumFractionDigits:d,maximumFractionDigits:d}):'—';
let historyData=[],selected='p',lastT=null;
const cfg={
 p:{title:'Daya aktif',unit:'W',d:0,min:null,max:null},
 q:{title:'Daya reaktif',unit:'VAr',d:0,min:null,max:null},
 pf:{title:'Power factor',unit:'PF',d:2,min:0,max:1}
};
function draw(){
 const c=$('trendChart'),dpr=window.devicePixelRatio||1,W=c.clientWidth,H=c.clientHeight;
 if(c.width!==W*dpr||c.height!==H*dpr){c.width=W*dpr;c.height=H*dpr;}
 const ctx=c.getContext('2d');ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,W,H);
 const k=cfg[selected],padL=62,padR=16,padT=14,padB=28;
 ctx.font='12px Segoe UI,Arial';ctx.fillStyle='#98a9c4';ctx.strokeStyle='#25344d';ctx.lineWidth=1;
 const vals=historyData.map(p=>p[selected]).filter(v=>Number.isFinite(v));
 $('chartCaption').textContent=`${k.title} (${k.unit}) · ${historyData.length} titik terakhir (maks. 10 menit).`;
 if(!vals.length){ctx.fillText('Menunggu data...',padL,H/2);return;}
 let min=k.min??Math.min(...vals),max=k.max??Math.max(...vals);
 if(k.min===null){const span=Math.max(max-min,Math.abs(max)*0.05,1);min-=span*0.15;max+=span*0.15;if(Math.min(...vals)>=0)min=Math.max(0,min);}
 const X=i=>padL+(W-padL-padR)*(historyData.length===1?0.5:i/(historyData.length-1));
 const Y=v=>padT+(H-padT-padB)*(1-(v-min)/(max-min));
 for(let i=0;i<=4;i++){const v=max-(max-min)*i/4,y=Y(v);ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();ctx.fillText(fmt(v,k.d),6,y+4);}
 if(min<0&&max>0){ctx.strokeStyle='#4b5d7a';ctx.setLineDash([4,4]);ctx.beginPath();ctx.moveTo(padL,Y(0));ctx.lineTo(W-padR,Y(0));ctx.stroke();ctx.setLineDash([]);}
 ctx.strokeStyle='#65a8ff';ctx.lineWidth=2.5;ctx.beginPath();let started=false;
 historyData.forEach((p,i)=>{const v=p[selected];if(!Number.isFinite(v)){started=false;return;}const x=X(i),y=Y(v);if(!started){ctx.moveTo(x,y);started=true;}else ctx.lineTo(x,y);});
 ctx.stroke();
 const last=historyData[historyData.length-1];
 if(last&&Number.isFinite(last[selected])){ctx.fillStyle='#65a8ff';ctx.beginPath();ctx.arc(X(historyData.length-1),Y(last[selected]),4.5,0,Math.PI*2);ctx.fill();}
 if(historyData.length>1){ctx.fillStyle='#98a9c4';const t0=new Date(historyData[0].t).toLocaleTimeString('id-ID'),t1=new Date(last.t).toLocaleTimeString('id-ID');ctx.fillText(t0,padL,H-8);const w=ctx.measureText(t1).width;ctx.fillText(t1,W-padR-w,H-8);}
}
document.querySelectorAll('[data-chart]').forEach(b=>b.addEventListener('click',()=>{selected=b.dataset.chart;document.querySelectorAll('[data-chart]').forEach(x=>x.classList.toggle('active',x===b));draw();}));
window.addEventListener('resize',draw);

async function loadHistory(){
 try{const r=await fetch('/api/history',{cache:'no-store'});const h=await r.json();historyData=h;lastT=h.length?h[h.length-1].t:null;draw();}catch(e){}
}
async function update(){
 try{
  const res=await fetch('/api/data',{cache:'no-store'}),d=await res.json();
  $('mode').textContent=d.simulate?'MODE SIMULASI':`${d.port}`;
  $('csvfile').textContent=d.csv_file||'—';$('rows').textContent=fmt(d.rows_today,0);
  $('okerr').textContent=`${fmt(d.ok_count,0)} / ${fmt(d.err_count,0)}`;
  $('portinfo').textContent=`${d.port} · ID ${d.slave_id} · ${d.baudrate} bps`;$('interval').textContent=`${d.interval_s} detik`;
  $('since').textContent=d.totals_since?`(sejak ${new Date(d.totals_since).toLocaleString('id-ID')})`:'';
  $('kwhlog').textContent=fmt(d.kwh_log,3);$('kvarhind').textContent=fmt(d.kvarh_ind_log,3);$('kvarhcap').textContent=fmt(d.kvarh_cap_log,3);
  $('ratio').textContent=d.kwh_log>0?fmt(d.kvarh_ind_log/d.kwh_log,3):'—';
  if(!d.ok){throw new Error(d.error||'Gagal membaca meter');}
  $('status').textContent='Terhubung · mencatat ke CSV';$('status').className='status ok';
  $('error').style.display=d.log_error?'block':'none';if(d.log_error)$('error').textContent=d.log_error;
  $('voltage').textContent=fmt(d.voltage_v,1);$('current').textContent=fmt(d.current_a,3);$('power').textContent=fmt(d.active_power_w,1);
  $('pf').textContent=fmt(d.power_factor,3);$('reactive').textContent=fmt(d.reactive_power_var,1);$('apparent').textContent=fmt(d.apparent_power_va,1);
  $('frequency').textContent=fmt(d.frequency_hz,2);$('energy').textContent=fmt(d.energy_kwh_meter,2);
  $('loading').textContent=`${fmt(d.loading_pct,1)} %`;$('readms').textContent=`${fmt(d.read_ms,0)} ms`;
  $('updated').textContent=new Date(d.timestamp).toLocaleTimeString('id-ID');
  if(d.timestamp!==lastT){historyData.push({t:d.timestamp,p:d.active_power_w,q:d.reactive_power_var,pf:d.power_factor});if(historyData.length>300)historyData.shift();lastT=d.timestamp;draw();}
 }catch(e){
  $('status').textContent='Tidak terhubung';$('status').className='status bad';$('error').style.display='block';
  $('error').textContent='Pembacaan gagal: '+e.message+'\nPeriksa port COM (tidak dipakai aplikasi lain), kabel A/B, dan setting Modbus meter.';
 }
}
loadHistory().then(update);setInterval(update,2000);
</script></body></html>
"""


@app.route("/")
def index():
    return render_template_string(HTML)


@app.route("/api/data")
def api_data():
    return jsonify(poller.snapshot())


@app.route("/api/history")
def api_history():
    return jsonify(poller.history_list())


@app.route("/download/today")
def download_today():
    path = poller.logger.path_for(datetime.now().strftime("%Y-%m-%d"))
    if not path.exists():
        abort(404, "Belum ada file CSV hari ini.")
    return send_file(path.resolve(), as_attachment=True, download_name=path.name)


@app.route("/download/<name>")
def download_file(name):
    path = (poller.logger.dir / name).resolve()
    if path.parent != poller.logger.dir.resolve() or path.suffix != ".csv" or not path.exists():
        abort(404)
    return send_file(path, as_attachment=True, download_name=path.name)


@app.route("/files")
def list_files():
    files = sorted(poller.logger.dir.glob("ddsu666_*.csv"), reverse=True)
    items = "".join(
        f'<li><a href="/download/{f.name}">{f.name}</a> · {f.stat().st_size/1024:.0f} KB</li>' for f in files
    ) or "<li>Belum ada file.</li>"
    return (f'<body style="background:#0b1220;color:#edf3ff;font:15px Segoe UI,Arial;padding:24px">'
            f'<h2>File CSV</h2><ul style="line-height:2">{items}</ul>'
            f'<a style="color:#65a8ff" href="/">← Kembali</a></body>')


# ---------------------------------------------------------------------------
def main():
    global poller
    ap = argparse.ArgumentParser(description="Monitor & logger CSV untuk CHINT DDSU666")
    ap.add_argument("--port", default=PORT, help=f"port serial (default {PORT})")
    ap.add_argument("--id", type=int, default=SLAVE_ID, help="alamat Modbus meter")
    ap.add_argument("--baud", type=int, default=BAUDRATE)
    ap.add_argument("--interval", type=float, default=POLL_INTERVAL_S, help="detik antar pembacaan")
    ap.add_argument("--data-dir", default=DATA_DIR, help="folder penyimpanan CSV")
    ap.add_argument("--reset-totals", action="store_true", help="nolkan akumulasi kWh/kVArh log")
    ap.add_argument("--simulate", action="store_true", help="pakai meter tiruan (tanpa hardware)")
    ap.add_argument("--web-port", type=int, default=5000)
    args = ap.parse_args()

    logger = Logger(args.data_dir)
    if args.reset_totals:
        logger.reset_totals()

    if args.simulate:
        meter = SimulatedMeter()
    else:
        try:
            meter = DDSU666(args.port, args.id, args.baud, STOPBITS)
        except Exception as exc:
            raise SystemExit(f"Tidak bisa membuka {args.port}: {exc}\n"
                             "Cek Device Manager, atau tutup aplikasi lain yang memakai port ini.")

    meta = {"port": "SIMULASI" if args.simulate else args.port, "slave_id": args.id,
            "baudrate": args.baud, "interval_s": args.interval, "simulate": args.simulate}
    poller = Poller(meter, logger, args.interval, meta)
    poller.start()
    # Jendela ditutup / proses dihentikan -> tetap simpan state.json
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    print("DDSU666 Power Monitor + CSV Logger")
    print(f"Port: {meta['port']} | Slave ID: {args.id} | Baud: {args.baud} | Interval: {args.interval} s")
    print(f"CSV : {Path(args.data_dir).resolve()}")
    print(f"Buka browser: http://127.0.0.1:{args.web_port}   (Ctrl+C untuk berhenti)")
    try:
        app.run(host="127.0.0.1", port=args.web_port, debug=False, threaded=True, use_reloader=False)
    finally:
        poller.stop_event.set()
        poller.join(timeout=5)
        logger.close()
        meter.close()
        print("Berhenti. Total kWh/kVArh tersimpan di state.json.")


if __name__ == "__main__":
    main()
