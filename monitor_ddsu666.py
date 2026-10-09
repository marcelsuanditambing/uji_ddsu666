from flask import Flask, jsonify, render_template_string
import minimalmodbus
import serial
import struct
import math
import threading
from datetime import datetime

# Konfigurasi meter sesuai hasil pengujian sebelumnya
PORT = "COM5"
SLAVE_ID = 1
BAUDRATE = 9600
STOPBITS = 1  # Mulai dengan 8N1; bila gagal, coba 2 sesuai bagian lain manual.

app = Flask(__name__)
meter_lock = threading.Lock()

meter = minimalmodbus.Instrument(PORT, SLAVE_ID, mode=minimalmodbus.MODE_RTU)
meter.serial.baudrate = BAUDRATE
meter.serial.bytesize = 8
meter.serial.parity = serial.PARITY_NONE
meter.serial.stopbits = STOPBITS
meter.serial.timeout = 2
meter.clear_buffers_before_each_transaction = True


def float_from_registers(reg_hi, reg_lo, swap_words=False):
    """Gabungkan dua register 16-bit menjadi IEEE-754 float 32-bit."""
    if swap_words:
        reg_hi, reg_lo = reg_lo, reg_hi
    raw = (reg_hi << 16) | reg_lo
    return struct.unpack(">f", raw.to_bytes(4, byteorder="big"))[0]


def decode_pair(registers, index, low, high):
    """Pilih urutan word yang masuk akal; verifikasi hasil dengan LCD meter."""
    normal = float_from_registers(registers[index], registers[index + 1])
    swapped = float_from_registers(registers[index], registers[index + 1], True)

    def valid(value):
        return math.isfinite(value) and low <= value <= high

    if valid(normal):
        return normal, "high-word-first"
    if valid(swapped):
        return swapped, "word-swapped"
    return normal, "unknown"


def read_meter():
    with meter_lock:
        r = meter.read_registers(0x2000, 16, functioncode=3)

        voltage, order_v = decode_pair(r, 0, 150, 280)
        current, order_i = decode_pair(r, 2, 0, 80)
        power_kw, order_p = decode_pair(r, 4, -20, 20)
        reactive_kvar, order_q = decode_pair(r, 6, -20, 20)
        pf, order_pf = decode_pair(r, 10, -1.1, 1.1)
        frequency, order_f = decode_pair(r, 14, 40, 70)

        energy_kwh = None
        try:
            er = meter.read_registers(0x4000, 2, functioncode=3)
            e_normal = float_from_registers(er[0], er[1])
            e_swapped = float_from_registers(er[0], er[1], True)
            if math.isfinite(e_normal) and 0 <= e_normal < 1e9:
                energy_kwh = e_normal
            elif math.isfinite(e_swapped) and 0 <= e_swapped < 1e9:
                energy_kwh = e_swapped
        except Exception:
            pass

    apparent_kva = math.sqrt(power_kw * power_kw + reactive_kvar * reactive_kvar)
    loading_pct = apparent_kva / 2.2 * 100

    return {
        "ok": True,
        "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
        "voltage_v": voltage,
        "current_a": current,
        "active_power_kw": power_kw,
        "active_power_w": power_kw * 1000,
        "reactive_power_kvar": reactive_kvar,
        "apparent_power_kva": apparent_kva,
        "power_factor": pf,
        "frequency_hz": frequency,
        "energy_kwh": energy_kwh,
        "loading_pct": loading_pct,
        "word_order": {
            "voltage": order_v, "current": order_i, "active_power": order_p,
            "reactive_power": order_q, "power_factor": order_pf, "frequency": order_f
        }
    }


HTML = r"""
<!doctype html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DDSU666 Power Monitor</title>
<style>
:root{color-scheme:dark;--bg:#0b1220;--panel:#121c2e;--line:#25344d;--text:#edf3ff;--muted:#98a9c4;--blue:#65a8ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 Segoe UI,Arial,sans-serif}
main{max-width:1120px;margin:auto;padding:28px 18px 44px}header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:12px;margin-bottom:22px}
h1{margin:0;font-size:clamp(23px,4vw,32px)}h2{font-size:18px;margin:0 0 12px}.subtitle,.label,.small,footer{color:var(--muted)}
.subtitle{margin-top:4px}.status{border:1px solid var(--line);border-radius:999px;padding:7px 12px;color:var(--muted)}
.status.ok{color:#86efac;border-color:#276749}.status.bad{color:#fca5a5;border-color:#7f1d1d}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}.card{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:18px;min-width:0}
.label{font-size:13px}.value{font-size:clamp(23px,3vw,34px);font-weight:700;margin-top:7px;overflow-wrap:anywhere}.unit{font-size:14px;color:var(--muted);font-weight:500}
.small{font-size:12px;margin-top:8px}.trend-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:14px;margin-top:14px}.lower{display:grid;grid-template-columns:1fr;gap:14px;margin-top:14px}
canvas{width:100%;height:230px;display:block}.meter-info{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.info{background:#0d1626;border:1px solid var(--line);border-radius:12px;padding:12px}.error{display:none;background:#451a1a;border:1px solid #7f1d1d;color:#fecaca;padding:13px;border-radius:12px;margin-bottom:14px;white-space:pre-wrap}
footer{margin-top:18px;font-size:12px}@media(max-width:1050px){.trend-grid{grid-template-columns:1fr 1fr}}@media(max-width:850px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.trend-grid{grid-template-columns:1fr}.lower{grid-template-columns:1fr}}
@media(max-width:430px){main{padding:18px 12px 30px}.card{padding:14px}.grid{gap:9px}}
</style>
</head>
<body><main>
<header><div><h1>DDSU666 Power Monitor</h1><div class="subtitle">Pemantauan listrik rumah · CHINT DDSU666 · Modbus RTU</div></div><div id="status" class="status">Menghubungkan...</div></header>
<div id="error" class="error"></div>
<section class="grid">
<div class="card"><div class="label">Tegangan</div><div class="value"><span id="voltage">—</span> <span class="unit">V</span></div><div class="small">Voltage RMS</div></div>
<div class="card"><div class="label">Arus</div><div class="value"><span id="current">—</span> <span class="unit">A</span></div><div class="small">Current RMS</div></div>
<div class="card"><div class="label">Daya aktif</div><div class="value"><span id="power">—</span> <span class="unit">W</span></div><div class="small">Daya yang digunakan</div></div>
<div class="card"><div class="label">Power factor</div><div class="value" id="pf">—</div><div class="small">Faktor daya</div></div>
<div class="card"><div class="label">Daya reaktif</div><div class="value"><span id="reactive">—</span> <span class="unit">kvar</span></div><div class="small">Reactive power</div></div>
<div class="card"><div class="label">Daya semu</div><div class="value"><span id="apparent">—</span> <span class="unit">kVA</span></div><div class="small">√(P² + Q²)</div></div>
<div class="card"><div class="label">Frekuensi</div><div class="value"><span id="frequency">—</span> <span class="unit">Hz</span></div><div class="small">Frekuensi jaringan</div></div>
<div class="card"><div class="label">Energi aktif</div><div class="value"><span id="energy">—</span> <span class="unit">kWh</span></div><div class="small">Energi kumulatif</div></div>
</section>
<section class="trend-grid">
<div class="card"><h2>Tren daya aktif</h2><canvas id="chartPower" width="700" height="240"></canvas><div class="small">Daya aktif (kW) · 60 titik terakhir.</div></div>
<div class="card"><h2>Tren daya reaktif</h2><canvas id="chartReactive" width="700" height="240"></canvas><div class="small">Daya reaktif (kvar) · 60 titik terakhir.</div></div>
<div class="card"><h2>Tren power factor</h2><canvas id="chartPF" width="700" height="240"></canvas><div class="small">Power factor · 60 titik terakhir.</div></div>
</section>
<section class="lower">
<div class="card"><h2>Status pembacaan</h2><div class="meter-info">
<div class="info"><div class="label">Estimasi beban terhadap 2.200 VA</div><div class="value"><span id="loading">—</span><span class="unit">%</span></div></div>
<div class="info"><div class="label">Update terakhir</div><div id="updated" style="margin-top:8px;font-weight:600">—</div></div>
<div class="info"><div class="label">Port</div><div style="margin-top:8px;font-weight:600">COM5</div></div>
<div class="info"><div class="label">Modbus</div><div style="margin-top:8px;font-weight:600">ID 1 · 9600 bps</div></div>
</div><div class="small">Persentase beban adalah perkiraan daya semu dibanding 2.200 VA, bukan pengganti batas MCB atau daya tersambung.</div></div>
</section>
<footer>Refresh otomatis setiap 2 detik. Jangan mengubah kabel terminal AC saat bertegangan.</footer>
</main>
<script>
const historyData=[];
const $=id=>document.getElementById(id);
const fmt=(n,d=2)=>(typeof n==='number'&&Number.isFinite(n))?n.toLocaleString('id-ID',{minimumFractionDigits:d,maximumFractionDigits:d}):'—';

function drawLineChart(canvasId,key,unit,fixedMin=null,fixedMax=null){
 const canvas=$(canvasId),ctx=canvas.getContext('2d'),w=canvas.width,h=canvas.height,pad=42;
 ctx.clearRect(0,0,w,h);ctx.font='12px Segoe UI,Arial';ctx.strokeStyle='#25344d';ctx.fillStyle='#98a9c4';ctx.lineWidth=1;
 const vals=historyData.map(p=>p[key]).filter(v=>Number.isFinite(v));
 if(!vals.length){ctx.fillText('Menunggu data...',pad,h/2);return;}
 let min=fixedMin!==null?fixedMin:Math.min(0,...vals);
 let max=fixedMax!==null?fixedMax:Math.max(0.1,...vals);
 if(fixedMin===null&&fixedMax===null){const span=Math.max(max-min,0.05);min=Math.min(0,min-span*0.1);max+=span*0.15;}
 if(max===min)max=min+1;
 for(let i=0;i<=4;i++){const y=pad+(h-2*pad)*i/4,val=max-(max-min)*i/4;ctx.beginPath();ctx.moveTo(pad,y);ctx.lineTo(w-pad,y);ctx.stroke();ctx.fillText(val.toFixed(2),2,y+4);}
 ctx.fillText(unit,w-pad+2,h-pad+18);
 if(vals.length===1){const x=w/2,y=h-pad-(vals[0]-min)/(max-min)*(h-2*pad);ctx.fillStyle='#65a8ff';ctx.beginPath();ctx.arc(x,y,4,0,Math.PI*2);ctx.fill();return;}
 ctx.strokeStyle='#65a8ff';ctx.lineWidth=2.5;ctx.beginPath();
 historyData.forEach((p,i)=>{const x=pad+(w-2*pad)*i/(historyData.length-1),y=h-pad-(p[key]-min)/(max-min)*(h-2*pad);if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});
 ctx.stroke();
}
function drawAllCharts(){
 drawLineChart('chartPower','power','kW');
 drawLineChart('chartReactive','reactive','kvar');
 drawLineChart('chartPF','pf','PF',0,1);
}
async function update(){
 try{
  const res=await fetch('/api/data',{cache:'no-store'}),d=await res.json();
  if(!res.ok||!d.ok)throw new Error(d.error||'Gagal membaca meter');
  $('status').textContent='Terhubung · memperbarui otomatis';$('status').className='status ok';$('error').style.display='none';
  $('voltage').textContent=fmt(d.voltage_v,1);$('current').textContent=fmt(d.current_a,3);$('power').textContent=fmt(d.active_power_w,1);
  $('pf').textContent=fmt(d.power_factor,3);$('reactive').textContent=fmt(d.reactive_power_kvar,3);$('apparent').textContent=fmt(d.apparent_power_kva,3);
  $('frequency').textContent=fmt(d.frequency_hz,2);$('energy').textContent=d.energy_kwh==null?'N/A':fmt(d.energy_kwh,2);
  $('loading').textContent=fmt(d.loading_pct,1);$('updated').textContent=new Date(d.timestamp).toLocaleTimeString('id-ID');
  historyData.push({power:d.active_power_kw,reactive:d.reactive_power_kvar,pf:d.power_factor});
  if(historyData.length>60)historyData.shift();drawAllCharts();
 }catch(e){$('status').textContent='Tidak terhubung';$('status').className='status bad';$('error').style.display='block';$('error').textContent='Pembacaan gagal: '+e.message+'\\nPeriksa apakah COM5 dipakai aplikasi lain, kabel A/B, dan setting Modbus.';}
}
update();setInterval(update,2000);
</script></body></html>
"""


@app.route("/")
def index():
    return render_template_string(HTML)


@app.route("/api/data")
def api_data():
    try:
        return jsonify(read_meter())
    except Exception as exc:
        app.logger.exception("Gagal membaca DDSU666")
        return jsonify({"ok": False, "error": f"{type(exc).__name__}: {exc}"}), 503


if __name__ == "__main__":
    print("DDSU666 Power Monitor")
    print(f"Port: {PORT} | Slave ID: {SLAVE_ID} | Baudrate: {BAUDRATE}")
    print("Buka browser: http://127.0.0.1:5000")
    print("Tekan Ctrl+C untuk menghentikan server.")
    app.run(host="127.0.0.1", port=5000, debug=False, threaded=True)
