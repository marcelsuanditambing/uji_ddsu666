    <div class="small">Riwayat sesi browser ini; maksimum 60 titik terakhir.</div>
  </div>
  <div class="card">
    <h2>Status pembacaan</h2>
    <div class="meter-info">
      <div class="info"><div class="label">Estimasi beban terhadap 2.200 VA</div><div class="value"><span id="loading">—</span><span class="unit">%</span></div></div>
      <div class="info"><div class="label">Update terakhir</div><div id="updated" style="margin-top:8px;font-weight:600">—</div></div>
      <div class="info"><div class="label">Port</div><div style="margin-top:8px;font-weight:600">COM5</div></div>
      <div class="info"><div class="label">Modbus</div><div style="margin-top:8px;font-weight:600">ID 1 · 9600 bps</div></div>
    </div>
    <div class="small">Estimasi persentase memakai daya semu dibanding 2.200 VA. Ini indikator saja, bukan izin melampaui batas MCB atau daya tersambung.</div>
  </div>
</section>
<footer>Refresh otomatis setiap 2 detik. Jangan mengubah kabel terminal AC saat bertegangan.</footer>
</main>
<script>
const history = [];
const $ = id => document.getElementById(id);
const fmt = (n, digits=2) => (typeof n === 'number' && Number.isFinite(n))
  ? n.toLocaleString('id-ID', {minimumFractionDigits:digits, maximumFractionDigits:digits})
  : '—';

function drawChart() {
  const canvas = $('chart'), ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height, pad = 38;
  ctx.clearRect(0, 0, w, h);
  ctx.font = '12px Segoe UI, Arial';
  ctx.strokeStyle = '#25344d'; ctx.fillStyle = '#98a9c4'; ctx.lineWidth = 1;
  const vals = history.map(p => p.power);
  const max = Math.max(0.2, ...vals) * 1.15;
  for (let i=0; i<=4; i++) {
    const y = pad + (h-2*pad)*i/4;
    ctx.beginPath(); ctx.moveTo(pad,y); ctx.lineTo(w-pad,y); ctx.stroke();
    ctx.fillText((max*(1-i/4)).toFixed(2)+' kW', 2, y+4);
  }
  if (history.length < 1) return;
  ctx.strokeStyle = '#65a8ff'; ctx.lineWidth = 3; ctx.beginPath();
  history.forEach((p,i) => {
    const x = history.length === 1 ? w/2 : pad + (w-2*pad)*i/(history.length-1);
    const y = h-pad - (p.power/max)*(h-2*pad);
    if (i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.stroke();
}

async function update() {
  try {
    const response = await fetch('/api/data', {cache:'no-store'});
    const d = await response.json();
    if (!response.ok || !d.ok) throw new Error(d.error || 'Gagal membaca meter');

    $('status').textContent = 'Terhubung · memperbarui otomatis';
    $('status').className = 'status ok';
    $('error').style.display = 'none';
    $('voltage').textContent = fmt(d.voltage_v, 1);
    $('current').textContent = fmt(d.current_a, 3);
    $('power').textContent = fmt(d.active_power_w, 1);
    $('pf').textContent = fmt(d.power_factor, 3);
    $('reactive').textContent = fmt(d.reactive_power_kvar, 3);
    $('apparent').textContent = fmt(d.apparent_power_kva, 3);
    $('frequency').textContent = fmt(d.frequency_hz, 2);
    $('energy').textContent = d.energy_kwh == null ? 'N/A' : fmt(d.energy_kwh, 2);
    $('loading').textContent = fmt(d.loading_pct, 1);
    $('updated').textContent = new Date(d.timestamp).toLocaleTimeString('id-ID');

    history.push({power: d.active_power_kw});
    if (history.length > 60) history.shift();
    drawChart();
  } catch (e) {
    $('status').textContent = 'Tidak terhubung';
    $('status').className = 'status bad';
    $('error').style.display = 'block';
    $('error').textContent = 'Pembacaan gagal: ' + e.message +
      '\\nPeriksa apakah COM5 sedang digunakan aplikasi lain, kabel A/B, serta setting Modbus.';
  }
}
update();
setInterval(update, 2000);
</script>
</body>
</html>
"""


@app.route("/")
def index():
    return render_template_string(PAGE)


@app.route("/api/data")
def api_data():
    try:
        return jsonify(read_meter())
    except Exception as exc:
        app.logger.exception("Gagal membaca DDSU666")
        return jsonify({
            "ok": False,
            "error": f"{type(exc).__name__}: {exc}"
        }), 503


if __name__ == "__main__":
    print("DDSU666 Power Monitor")
    print(f"Port: {PORT} | Slave ID: {SLAVE_ID} | Baudrate: {BAUDRATE}")
    print("Buka browser: http://127.0.0.1:5000")
    print("Tekan Ctrl+C untuk menghentikan server.")
    app.run(host="127.0.0.1", port=5000, debug=False, threaded=True)
