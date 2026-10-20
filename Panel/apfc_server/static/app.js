/* APFC dashboard — logika tampilan. Data masuk lewat SSE (/stream), perintah lewat POST /api/cmd. */
"use strict";
const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const cssv = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

const S = {
  tel: null, status: null, online: null, broker: false, server: false,
  rxLocal: 0, tReconnect: 10, tariff: 1114.74, scale: 1000, sessionStart: 0,
  range: "10", period: "session", eScale: 1000, evtFilter: "", events: [], pending: 0,
};

// ------------------------------------------------------------------ format
const nf = (x, d = 1) => (x == null || isNaN(x)) ? "—" :
  Number(x).toLocaleString("id-ID", { minimumFractionDigits: d, maximumFractionDigits: d });
const rp = (x) => (x == null || isNaN(x)) ? "—" : "Rp" + Math.round(x).toLocaleString("id-ID");
const pad = (n) => String(n).padStart(2, "0");
const hms = (t) => { const d = new Date(t * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`; };
const hm = (t) => { const d = new Date(t * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; };
const dmy = (t) => { const d = new Date(t * 1000); return `${pad(d.getDate())}/${pad(d.getMonth() + 1)}`; };
const dur = (s) => s < 90 ? `${Math.round(s)} s` : s < 5400 ? `${Math.round(s / 60)} menit` : `${nf(s / 3600, 1)} jam`;
const qStep = (uF, v) => 2 * Math.PI * 50 * uF * 1e-6 * v * v;

function toast(msg, kind = "") {
  const el = $("#toast");
  el.textContent = msg;
  el.className = `toast show ${kind}`;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (el.className = "toast"), 3200);
}

// ------------------------------------------------------------------ tab
$$(".tab").forEach((b) => b.addEventListener("click", () => {
  $$(".tab").forEach((x) => x.classList.toggle("active", x === b));
  $$(".pane").forEach((p) => p.classList.toggle("active", p.id === "pane-" + b.dataset.tab));
  if (b.dataset.tab === "tren") loadHistory();
  if (b.dataset.tab === "energi") loadEnergy();
  if (b.dataset.tab === "log") { loadEvents(); loadCsv(); }
  if (b.dataset.tab === "kontrol") renderCtl();
  history.replaceState(null, "", "#" + b.dataset.tab);
}));
function seg(id, cb) {
  $$(`#${id} button`).forEach((b) => b.addEventListener("click", () => {
    $$(`#${id} button`).forEach((x) => x.classList.toggle("on", x === b));
    cb(b.dataset.v);
  }));
}

// ------------------------------------------------------------------ header & banner
function renderHeader() {
  const on = $("#chip-online");
  let cls = "chip crit", txt = "Panel offline";
  if (!S.server) txt = "Server terputus";
  else if (!S.broker) txt = "Broker MQTT terputus";
  else if (S.online === true) { cls = "chip good"; txt = "Online"; }
  else if (S.online === null) { cls = "chip"; txt = "Menunggu panel…"; }
  on.className = cls;
  on.querySelector("b").textContent = txt;

  const st = (S.tel && S.tel.state) || (S.status && S.status.state) || "—";
  const mode = (S.tel && S.tel.mode) || (S.status && S.status.mode) || "—";
  const sc = $("#chip-state");
  sc.className = "chip" + (st === "FAILSAFE" ? " warn" : st === "INIT" ? " muted" : "") + (st === mode || st === "—" ? " hidden" : "");
  sc.querySelector("b").textContent = st === "FAILSAFE" ? "⚠ FAIL-SAFE" : st;
  $("#chip-mode b").textContent = "Mode " + mode;

  const bn = $("#banner");
  if (!S.server) { bn.className = "banner crit"; bn.textContent = "✕ Koneksi ke server dashboard terputus — mencoba menyambung lagi…"; }
  else if (!S.broker) { bn.className = "banner crit"; bn.textContent = "✕ Backend tidak terhubung ke broker MQTT. Pastikan Mosquitto berjalan."; }
  else if (S.online === false) { bn.className = "banner off"; bn.textContent = "Panel offline — angka yang tampil adalah data terakhir yang diterima."; }
  else if (st === "FAILSAFE") { bn.className = "banner warn"; bn.textContent = "⚠ FAIL-SAFE: meter tidak terbaca, semua step kapasitor dilepas. Cek kabel RS485."; }
  else bn.className = "banner hidden";
}
setInterval(() => {
  const el = $("#chip-age");
  if (!S.rxLocal) { el.className = "chip muted"; el.querySelector("b").textContent = "Data: —"; return; }
  const age = (Date.now() - S.rxLocal) / 1000;
  el.className = "chip " + (age > 6 ? "stale" : "muted");
  el.querySelector("b").textContent = age > 6 ? `Data lama · ${dur(age)}` : `Data ${hms(S.tel._rx)}`;
  renderStepsView(); renderCtlSteps(false);
}, 1000);

// ------------------------------------------------------------------ gauge (1,00 di atas; induktif kiri, kapasitif kanan)
const G = { cx: 120, cy: 112, r: 92, span: 0.6 };
function gPoint(x, r = G.r) {               // x: −span (induktif) … 0 (PF 1) … +span (kapasitif)
  const a = Math.PI / 2 - (x / G.span) * (Math.PI / 2);
  return [G.cx + r * Math.cos(a), G.cy - r * Math.sin(a)];
}
function gArc(x0, x1, r = G.r) {
  const [a, b] = gPoint(x0, r), [c, d] = gPoint(x1, r);
  return `M${a.toFixed(1)},${b.toFixed(1)} A${r},${r} 0 0 1 ${c.toFixed(1)},${d.toFixed(1)}`;
}
function drawGauge(pf, q, noLoad) {
  const z = [[-0.6, -0.15, "--crit", .45], [-0.15, -0.05, "--warn", .6], [-0.05, 0.05, "--good", .7],
             [0.05, 0.15, "--warn", .6], [0.15, 0.6, "--crit", .45]];
  let s = z.map(([a, b, c, o]) => `<path d="${gArc(a, b)}" stroke="${cssv(c)}" stroke-opacity="${o}" stroke-width="16" fill="none"/>`).join("");
  const lab = (x, t, anchor) => { const [px, py] = gPoint(x, G.r + 15); return `<text x="${px}" y="${py + 4}" text-anchor="${anchor}" font-size="10" fill="${cssv("--text-3")}">${t}</text>`; };
  s += lab(-0.6, "0,4", "middle") + lab(0.6, "0,4", "middle") + lab(-0.15, "0,85", "end") + lab(0.15, "0,85", "start") + lab(0, "1,00", "middle");
  s += `<text x="52" y="108" text-anchor="start" font-size="11" font-weight="700" fill="${cssv("--text-2")}">IND</text>`;
  s += `<text x="188" y="108" text-anchor="end" font-size="11" font-weight="700" fill="${cssv("--text-2")}">KAP</text>`;
  if (pf != null && !noLoad) {
    let x = (1 - Math.min(Math.max(pf, 0), 1)) * (q >= 0 ? -1 : 1);
    x = Math.max(-G.span, Math.min(G.span, x));
    const [nx, ny] = gPoint(x, G.r - 4);
    s += `<line x1="${G.cx}" y1="${G.cy}" x2="${nx}" y2="${ny}" stroke="${cssv("--text-1")}" stroke-width="3.5" stroke-linecap="round"/>`;
    s += `<circle cx="${G.cx}" cy="${G.cy}" r="7" fill="${cssv("--text-1")}"/>`;
  }
  $("#gauge").innerHTML = s;
}

// ------------------------------------------------------------------ ringkasan
function renderSummary() {
  const t = S.tel;
  if (!t) { drawGauge(null); return; }
  const noLoad = t.p < 10;
  $("#m-v").textContent = nf(t.v, 1);
  $("#m-i").textContent = nf(t.i, 3);
  $("#m-f").textContent = nf(t.f, 2);
  $("#m-p").textContent = nf(t.p, 1);
  $("#m-q").textContent = (t.q > 0 ? "+" : "") + nf(t.q, 1);
  $("#m-qn").textContent = noLoad ? "" : t.q >= 0 ? "induktif" : "kapasitif";
  $("#m-s").textContent = nf(t.s, 1);
  $("#pf-val").textContent = noLoad ? "—" : nf(t.pf, 3);
  drawGauge(t.pf, t.q, noLoad);
  const nat = $("#pf-nature");
  if (noLoad) { nat.className = "pf-nature"; nat.textContent = "Tanpa beban"; }
  else {
    const kind = t.q >= 0 ? "Induktif" : "Kapasitif";
    if (t.pf >= 0.95) { nat.className = "pf-nature good"; nat.textContent = `✓ ${kind} · sesuai target`; }
    else if (t.pf >= 0.85) { nat.className = "pf-nature warn"; nat.textContent = `! ${kind} · di bawah target`; }
    else { nat.className = "pf-nature crit"; nat.textContent = t.q >= 0 ? `✕ ${kind} · zona penalti` : `✕ ${kind} · kompensasi berlebih`; }
  }
  // kompensasi
  const load = Math.max(0, t.q_load || 0), cap = Math.max(0, (t.q_load || 0) - t.q), res = t.q;
  const mx = Math.max(load, cap, Math.abs(res), 50);
  $("#b-load").style.width = (load / mx * 100) + "%";
  $("#b-cap").style.width = (cap / mx * 100) + "%";
  $("#b-res").style.width = (Math.abs(res) / mx * 100) + "%";
  $("#v-load").textContent = nf(load, 0) + " var";
  $("#v-cap").textContent = nf(cap, 0) + " var";
  $("#v-res").textContent = (res > 0 ? "+" : "") + nf(res, 0) + " var";
  $("#comp-hint").textContent = noLoad ? "Tidak ada beban: semua step dilepas." :
    load > 1 ? `Kapasitor memasok ${nf(cap / load * 100, 0)} % kebutuhan Q beban` + (res < 0 ? " (sedikit berlebih, sisa bersifat kapasitif)." : ".") : "—";
  renderStepsView();
  renderPanelKv();
}

function stepWait(k) {
  if (!S.tel || !S.tel._wait) return 0;
  return Math.max(0, Math.ceil(S.tel._wait[k] - (Date.now() - S.rxLocal) / 1000));
}
function cuf() { return (S.status && S.status.c_uf) || [6.36, 6.36, 6.36]; }
function renderStepsView() {
  const t = S.tel, c = cuf(), v = t ? t.v : 220;
  $("#steps-view").innerHTML = [0, 1, 2].map((k) => {
    const on = t && t.steps && t.steps[k];
    const w = stepWait(k);
    return `<div class="step ${on ? "on" : ""}"><div class="s-n">C${k + 1}</div>
      <div class="s-c num">${nf(c[k], 2)} µF<br>≈${nf(qStep(c[k], v), 0)} var</div>
      <div class="s-st">${on ? "● ON" : "○ OFF"}</div>
      <div class="s-w">${!on && w > 0 ? `jeda ${w} s` : ""}</div></div>`;
  }).join("");
}
function renderPanelKv() {
  const t = S.tel || {}, s = S.status || {};
  const pill = (x) => `<span class="pill ${x ? "on" : "off"}">${x ? "ON" : "OFF"}</span>`;
  const so = t.sockets || s.sockets || [1];
  const up = s.uptime != null ? s.uptime + Math.round((Date.now() / 1000 - (s._rx || Date.now() / 1000))) : null;
  const rows = [
    ["Beban demo (relay K1, kontak NC)", `<span class="pill ${so[0] ? "on" : "off"}">${so[0] ? "MENYALA" : "PADAM"}</span>`],
    ["Meter DDSU666", s.meter_ok === 0 ? "✕ tidak terbaca" : "✓ OK"],
    ["Wi-Fi", t.rssi ? `${t.rssi} dBm` : "—"],
    ["IP panel", s.ip || "—"],
    ["Firmware", s.fw || "—"],
    ["Uptime", up != null ? dur(up) : "—"],
  ];
  $("#panel-kv").innerHTML = rows.map(([a, b]) => `<li><span>${a}</span><b>${b}</b></li>`).join("");
}

// ------------------------------------------------------------------ grafik
Chart.defaults.font.family = cssv("--font") || "system-ui";
Chart.defaults.font.size = 12;
Chart.defaults.color = cssv("--text-2");

const crosshair = {
  id: "crosshair",
  afterDatasetsDraw(ch) {
    const a = ch.tooltip && ch.tooltip.getActiveElements();
    if (!a || !a.length) return;
    const x = a[0].element.x, { top, bottom } = ch.chartArea, c = ch.ctx;
    c.save(); c.strokeStyle = cssv("--text-3"); c.lineWidth = 1; c.beginPath(); c.moveTo(x, top); c.lineTo(x, bottom); c.stroke(); c.restore();
  },
};
const marksPlugin = {
  id: "marks",
  beforeDatasetsDraw(ch, _a, opt) {
    const items = opt.items || [], { top, bottom, left, right } = ch.chartArea, c = ch.ctx, xs = ch.scales.x;
    c.save();
    // garis referensi horizontal
    (opt.refs || []).forEach((r) => {
      const y = ch.scales.y.getPixelForValue(r.y);
      if (y < top || y > bottom) return;
      c.strokeStyle = r.color || cssv("--ref"); c.setLineDash(r.dash || [5, 4]); c.lineWidth = r.width || 1.2;
      c.beginPath(); c.moveTo(left, y); c.lineTo(right, y); c.stroke();
      if (r.label && right - left > 520) { c.setLineDash([]); c.fillStyle = cssv("--text-3"); c.font = "11px " + Chart.defaults.font.family; c.textAlign = "right"; c.fillText(r.label, right - 4, y - 5); }
    });
    c.setLineDash([2, 3]); c.lineWidth = 1;
    let lastX = -1e9;
    items.forEach((m) => {
      const x = xs.getPixelForValue(m.t * 1000);
      if (x < left || x > right) return;
      c.strokeStyle = cssv("--text-3"); c.beginPath(); c.moveTo(x, top); c.lineTo(x, bottom); c.stroke();
      if (x - lastX > 34 && right - left > 420) {
        c.fillStyle = cssv("--text-2"); c.font = "600 10.5px " + Chart.defaults.font.family; c.textAlign = "center";
        c.fillText(m.label, x, top + 10); lastX = x;
      }
    });
    c.restore();
  },
};
Chart.register(crosshair, marksPlugin);

function timeAxis(minutes) {
  return {
    type: "linear",
    ticks: { maxTicksLimit: 7, callback: (v) => { const t = v / 1000; return minutes <= 15 ? hms(t).slice(0, 8) : minutes > 1440 ? dmy(t) + " " + hm(t) : hm(t); } },
    grid: { color: cssv("--border"), drawTicks: false },
    border: { display: false },
  };
}
function baseOpts(minutes, yOpts, legend) {
  return {
    responsive: true, maintainAspectRatio: false, animation: false, parsing: false, normalized: true,
    interaction: { mode: "index", intersect: false },
    layout: { padding: { top: 14 } },
    scales: { x: timeAxis(minutes), y: Object.assign({ grid: { color: cssv("--border") }, border: { display: false } }, yOpts) },
    elements: { point: { radius: 0, hoverRadius: 4, hitRadius: 8 }, line: { borderWidth: 2, tension: 0.15 } },
    plugins: {
      legend: legend ? { display: true, align: "start", labels: { usePointStyle: true, pointStyle: "line", boxWidth: 26, color: cssv("--text-1") } } : { display: false },
      tooltip: {
        backgroundColor: cssv("--surface"), titleColor: cssv("--text-1"), bodyColor: cssv("--text-1"),
        borderColor: cssv("--border"), borderWidth: 1, padding: 10, boxPadding: 4, usePointStyle: true,
        callbacks: { title: (it) => it.length ? hms(it[0].parsed.x / 1000) : "" },
      },
      marks: { items: [], refs: [] },
    },
  };
}
function markLabel(e) {
  const m = /C(\d)/.exec(e.detail || "");
  if (e.type === "STEP_ON") return (m ? "C" + m[1] : "C") + "+";
  if (e.type === "STEP_OFF") return m ? "C" + m[1] + "−" : "semua−";
  if (e.type === "MODE") return e.detail || "mode";
  if (e.type === "LOAD") return /padam/.test(e.detail || "") ? "beban−" : "beban+";
  if (e.type === "FAILSAFE") return "⚠";
  return e.type;
}

const pfDs = (data) => ({ label: "PF", data, spanGaps: false, borderColor: cssv("--series-1"), backgroundColor: cssv("--series-1") });
const charts = {};
function makeCharts() {
  const pfRef = [{ y: 0.95, label: "target 0,95", color: cssv("--good") }, { y: 0.85, label: "batas penalti 0,85", color: cssv("--crit"), dash: [2, 4] }];
  const pfY = { min: 0.3, max: 1.0, ticks: { stepSize: 0.1, callback: (v) => nf(v, 2) } };
  charts.mini = new Chart($("#ch-mini"), { type: "line", data: { datasets: [pfDs([])] }, options: baseOpts(10, pfY, false) });
  charts.mini.options.plugins.marks.refs = pfRef;
  charts.pf = new Chart($("#ch-pf"), { type: "line", data: { datasets: [pfDs([])] }, options: baseOpts(10, pfY, false) });
  charts.pf.options.plugins.marks.refs = pfRef;
  charts.pf.options.plugins.tooltip.callbacks.label = (c) => ` PF ${nf(c.parsed.y, 3)}`;
  charts.mini.options.plugins.tooltip.callbacks.label = charts.pf.options.plugins.tooltip.callbacks.label;
  charts.pq = new Chart($("#ch-pq"), {
    type: "line",
    data: { datasets: [
      { label: "P (W)", data: [], borderColor: cssv("--series-1"), backgroundColor: cssv("--series-1") },
      { label: "Q (var)", data: [], borderColor: cssv("--series-2"), backgroundColor: cssv("--series-2") },
      { label: "Q beban tanpa APFC (var)", data: [], borderColor: cssv("--ref"), backgroundColor: cssv("--ref"), borderDash: [6, 4], borderWidth: 1.5 },
    ] },
    options: baseOpts(10, { title: { display: true, text: "W / var", color: cssv("--text-3") } }, true),
  });
  charts.pq.options.plugins.marks.refs = [{ y: 0, color: cssv("--text-3"), dash: [], width: 1 }];
  charts.pq.options.plugins.tooltip.callbacks.label = (c) => ` ${c.dataset.label.split(" (")[0]}: ${nf(c.parsed.y, 1)}`;
}

function rangeMinutes() { return S.range === "today" ? (Date.now() - new Date().setHours(0, 0, 0, 0)) / 60000 : Number(S.range); }
function trim(ds, minutes) { const lim = Date.now() - minutes * 60000; while (ds.length && ds[0].x < lim) ds.shift(); }

async function loadMini() {
  const r = await fetch("/api/history?min=10").then((x) => x.json());
  charts.mini.data.datasets[0].data = r.rows.map((p) => ({ x: p.t * 1000, y: p.p >= 10 ? p.pf : null }));
  charts.mini.options.plugins.marks.items = r.marks.filter((m) => m.type.startsWith("STEP") || m.type === "LOAD").map((m) => ({ t: m.t, label: markLabel(m) }));
  charts.mini.options.scales.x.min = Date.now() - 600000;
  charts.mini.update();
}
let trenRows = [];
async function loadHistory() {
  const q = S.range === "today" ? "range=today" : "min=" + S.range;
  const r = await fetch("/api/history?" + q).then((x) => x.json());
  trenRows = r.rows;
  const mins = rangeMinutes();
  [charts.pf, charts.pq].forEach((c) => { c.options.scales.x = timeAxis(mins); c.options.scales.x.min = r.from * 1000; });
  charts.pf.data.datasets[0].data = r.rows.map((p) => ({ x: p.t * 1000, y: p.p >= 10 ? p.pf : null }));
  charts.pq.data.datasets[0].data = r.rows.map((p) => ({ x: p.t * 1000, y: p.p }));
  charts.pq.data.datasets[1].data = r.rows.map((p) => ({ x: p.t * 1000, y: p.q }));
  charts.pq.data.datasets[2].data = r.rows.map((p) => ({ x: p.t * 1000, y: p.q_load }));
  const items = r.marks.map((m) => ({ t: m.t, label: markLabel(m) }));
  charts.pf.options.plugins.marks.items = items; charts.pq.options.plugins.marks.items = items;
  charts.pf.update(); charts.pq.update();
  renderTrenTable();
}
function renderTrenTable() {
  if ($("#tren-table-card").classList.contains("hidden")) return;
  const rows = trenRows.slice(-30).reverse();
  $("#tren-table").innerHTML = `<tr><th>Waktu</th><th class="r">PF</th><th class="r">P (W)</th><th class="r">Q (var)</th><th class="r">Q beban (var)</th><th>Step</th></tr>` +
    rows.map((p) => `<tr><td>${hms(p.t)}</td><td class="r">${nf(p.pf, 3)}</td><td class="r">${nf(p.p, 1)}</td><td class="r">${nf(p.q, 1)}</td><td class="r">${nf(p.q_load, 1)}</td><td>${[p.s1, p.s2, p.s3].map((s, i) => "C" + (i + 1) + (s ? "#" : "-")).join(" ")}</td></tr>`).join("");
}
seg("seg-range", (v) => { S.range = v; loadHistory(); });
$("#btn-table").addEventListener("click", () => {
  const c = $("#tren-table-card"); c.classList.toggle("hidden");
  $("#btn-table").textContent = c.classList.contains("hidden") ? "Tampilkan tabel" : "Sembunyikan tabel";
  renderTrenTable();
});

function liveAppend(t) {
  const x = t._rx * 1000;
  charts.mini.data.datasets[0].data.push({ x, y: t.p >= 10 ? t.pf : null });
  trim(charts.mini.data.datasets[0].data, 10);
  charts.mini.options.scales.x.min = Date.now() - 600000;
  charts.mini.update("none");
  if ($("#pane-tren").classList.contains("active")) {
    const mins = rangeMinutes();
    charts.pf.data.datasets[0].data.push({ x, y: t.p >= 10 ? t.pf : null });
    charts.pq.data.datasets[0].data.push({ x, y: t.p });
    charts.pq.data.datasets[1].data.push({ x, y: t.q });
    charts.pq.data.datasets[2].data.push({ x, y: t.q_load });
    if (S.range !== "today") {
      [charts.pf.data.datasets[0], ...charts.pq.data.datasets].forEach((d) => trim(d.data, mins));
      [charts.pf, charts.pq].forEach((c) => (c.options.scales.x.min = Date.now() - mins * 60000));
    }
    trenRows.push({ t: t._rx, pf: t.pf, p: t.p, q: t.q, q_load: t.q_load, s1: t.steps[0], s2: t.steps[1], s3: t.steps[2] });
    charts.pf.update("none"); charts.pq.update("none");
    renderTrenTable();
  }
}
function liveMark(e) {
  if (!["STEP_ON", "STEP_OFF", "MODE", "LOAD", "FAILSAFE", "RECOVER"].includes(e.type)) return;
  const m = { t: e.t, label: markLabel(e) };
  charts.pf.options.plugins.marks.items.push(m);
  if (e.type.startsWith("STEP") || e.type === "LOAD") charts.mini.options.plugins.marks.items.push(m);
}

// ------------------------------------------------------------------ energi
async function loadEnergy() {
  const r = await fetch(`/api/energy?period=${S.period}&scale=${S.eScale}`).then((x) => x.json());
  const unit = S.eScale > 1 ? " (setara)" : "";
  $("#e-pw").textContent = rp(r.penalty_without);
  $("#e-pa").textContent = rp(r.penalty_with);
  $("#e-sv").textContent = rp(r.saving);
  $("#e-pw-s").textContent = `kelebihan ${nf(r.over_without, 3)} kVArh`;
  $("#e-pa-s").textContent = `kelebihan ${nf(r.over_with, 3)} kVArh`;
  $("#e-sv-s").textContent = r.saving_per_hour != null ? `≈ ${rp(r.saving_per_hour)} per jam operasi` : "kumpulkan data beberapa menit";
  $("#e-tariff").textContent = "Rp" + nf(r.tariff, 2) + "/kVArh";
  const rw = r.ratio_without, ra = r.ratio_with, mx = Math.max(1, (rw || 0) * 1.15, (ra || 0) * 1.15);
  $("#r-without").style.width = Math.min(100, (rw || 0) / mx * 100) + "%";
  $("#r-with").style.width = Math.min(100, (ra || 0) / mx * 100) + "%";
  $$(".r-lim").forEach((e) => (e.style.left = (0.62 / mx * 100) + "%"));
  $(".r-lim-l").style.left = (0.62 / mx * 100) + "%";
  $("#r-without-v").textContent = rw == null ? "—" : nf(rw, 2);
  $("#r-with-v").textContent = ra == null ? "—" : nf(ra, 2);
  $("#e-period").textContent = r.first ? `${hms(r.first)}–${hms(r.last)} · ${dur(r.last - r.first)} · ${r.samples} sampel` : "belum ada data";
  const row = (a, b, c) => `<tr><td>${a}</td><td class="r">${b}</td><td class="r">${c}</td></tr>`;
  $("#e-table").innerHTML = `<tr><th>Besaran${unit}</th><th class="r">Tanpa APFC</th><th class="r">Dengan APFC</th></tr>` +
    row("Energi aktif (kWh)", nf(r.kwh, 4), nf(r.kwh, 4)) +
    row("Energi reaktif induktif (kVArh)", nf(r.kvarh_load, 4), nf(r.kvarh_ind, 4)) +
    row("Batas 0,62 × kWh", nf(0.62 * r.kwh, 4), nf(0.62 * r.kwh, 4)) +
    row("Kelebihan kVArh", nf(r.over_without, 4), nf(r.over_with, 4)) +
    row("Energi reaktif kapasitif (kVArh)", "—", nf(r.kvarh_cap, 4)) +
    row("<b>Penalti</b>", `<b>${rp(r.penalty_without)}</b>`, `<b>${rp(r.penalty_with)}</b>`);
}
seg("seg-period", (v) => { S.period = v; loadEnergy(); });
seg("seg-scale", (v) => { S.eScale = Number(v); loadEnergy(); });
setInterval(() => { if ($("#pane-energi").classList.contains("active")) loadEnergy(); }, 10000);

// ------------------------------------------------------------------ kontrol
const CMD_LABEL = {
  set_mode: (b) => `Mode → ${b.mode}`, set_step: (b) => `Step C${b.step} → ${b.on ? "ON" : "OFF"}`,
  set_socket: (b) => b.on ? "Nyalakan beban K1" : "Matikan beban K1",
  set_param: (b) => `Parameter ${b.key} = ${b.value}`, energy_reset: () => "Reset penghitung energi",
};
async function sendCmd(body) {
  const ul = $("#cmdlog");
  const empty = ul.querySelector(".empty"); if (empty) empty.remove();
  const li = document.createElement("li");
  li.innerHTML = `<span class="t">${hms(Date.now() / 1000)}</span><span>${CMD_LABEL[body.cmd](body)}</span><span class="res wait">menunggu…</span>`;
  ul.prepend(li);
  S.pending++; renderCtl();
  let r;
  try {
    r = await fetch("/api/cmd", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }).then((x) => x.json());
  } catch (e) { r = { ok: 0, msg: "server tidak dapat dihubungi" }; }
  S.pending--;
  const res = li.querySelector(".res");
  res.className = "res " + (r.ok ? "ok" : "no");
  res.textContent = r.ok ? "✓ " + r.msg : "✕ " + r.msg;
  toast((r.ok ? "✓ " : "✕ Ditolak: ") + r.msg, r.ok ? "ok" : "no");
  renderCtl();
  return r;
}
function curMode() { return (S.tel && S.tel.mode) || (S.status && S.status.mode) || "AUTO"; }
function renderCtl() {
  const mode = curMode(), busy = S.pending > 0, live = S.online === true && S.broker;
  $$("#ctl-mode button").forEach((b) => { b.classList.toggle("on", b.dataset.v === mode); b.disabled = busy || !live; });
  $("#ctl-mode-hint").textContent = mode === "AUTO"
    ? "AUTO: ESP32 memilih sendiri kombinasi kapasitor sesuai kebutuhan Q. Pindah ke MANUAL untuk mengatur step dari sini."
    : "MANUAL: step diatur dari dashboard. Step yang baru dilepas harus menunggu jeda sambung ulang sebelum boleh ON lagi.";
  renderCtlSteps(true);
  const so = (S.tel && S.tel.sockets) || (S.status && S.status.sockets) || [1];
  const lon = !!so[0];
  $("#ctl-socks").innerHTML =
    `<div class="load-state ${lon ? "on" : "off"}"><span class="ls-dot"></span><div><div class="ls-t">Beban ${lon ? "MENYALA" : "PADAM"}</div>
       <div class="ctl-sub">Relay K1 · kontak NC — relay ${lon ? "tidak bekerja" : "bekerja (kontak NC terbuka)"}</div></div></div>
     <div class="load-btns">
       <button class="btn danger load-btn" data-sock="1" data-on="0" ${busy || !live || !lon ? "disabled" : ""}>⏻ Matikan beban</button>
       <button class="btn load-btn" data-sock="1" data-on="1" ${busy || !live || lon ? "disabled" : ""}>Nyalakan beban</button>
     </div>
     <div class="hint">Perubahan beban akan dikoreksi ESP32 secara otomatis dalam ±15 s (mode AUTO). Bila ESP32 mati atau reset, beban kembali MENYALA karena memakai kontak NC.</div>`;
  const s = S.status || {}, c = cuf();
  $("#p-cur").textContent = `Nilai saat ini: pf_target ${s.pf_target != null ? nf(s.pf_target, 2) : "—"} · c1 ${nf(c[0], 2)} · c2 ${nf(c[1], 2)} · c3 ${nf(c[2], 2)} µF`;
  $("#ctl-param button").disabled = busy || !live;
  $("#btn-ereset").disabled = busy || !live;
}
function renderCtlSteps(force) {
  if (!force && !$("#pane-kontrol").classList.contains("active")) return;
  const mode = curMode(), busy = S.pending > 0, live = S.online === true && S.broker, c = cuf(), t = S.tel;
  $("#ctl-steps").innerHTML = [0, 1, 2].map((k) => {
    const on = t && t.steps && t.steps[k], w = stepWait(k);
    const dis = busy || !live || mode !== "MANUAL";
    return `<div class="ctl-row"><div><div class="ctl-name">C${k + 1} <span class="pill ${on ? "on" : "off"}">${on ? "ON" : "OFF"}</span></div>
      <div class="ctl-sub num">${nf(c[k], 2)} µF · ≈${nf(qStep(c[k], t ? t.v : 220), 0)} var${!on && w > 0 ? ` · jeda ${w} s` : ""}${mode !== "MANUAL" ? " · diatur otomatis" : ""}</div></div>
      <div class="ctl-btns"><button class="btn sm" data-step="${k + 1}" data-on="1" ${dis || on || w > 0 ? "disabled" : ""}>ON</button>
      <button class="btn sm ghost" data-step="${k + 1}" data-on="0" ${dis || !on ? "disabled" : ""}>OFF</button></div></div>`;
  }).join("");
}
$$("#ctl-mode button").forEach((b) => b.addEventListener("click", () => sendCmd({ cmd: "set_mode", mode: b.dataset.v })));
$("#ctl-steps").addEventListener("click", (e) => { const b = e.target.closest("button[data-step]"); if (b) sendCmd({ cmd: "set_step", step: +b.dataset.step, on: +b.dataset.on }); });
$("#ctl-socks").addEventListener("click", (e) => { const b = e.target.closest("button[data-sock]"); if (b) sendCmd({ cmd: "set_socket", socket: +b.dataset.sock, on: +b.dataset.on }); });
$("#ctl-param").addEventListener("submit", (e) => { e.preventDefault(); sendCmd({ cmd: "set_param", key: $("#p-key").value, value: parseFloat($("#p-val").value) }); });
$("#btn-ereset").addEventListener("click", () => {
  const d = $("#dlg");
  $("#dlg-t").textContent = "Reset penghitung energi?";
  $("#dlg-m").textContent = "kWh, kVArh induktif/kapasitif, dan kVArh tanpa APFC di panel akan dinolkan. Data yang sudah tersimpan di laptop tidak terhapus.";
  d.onclose = () => { if (d.returnValue === "yes") sendCmd({ cmd: "energy_reset" }); };
  d.showModal();
});

// ------------------------------------------------------------------ log
const EV = {
  STEP_ON: ["Step ON", ""], STEP_OFF: ["Step OFF", ""], MODE: ["Mode", ""], LOAD: ["Beban K1", ""], FAILSAFE: ["⚠ Fail-safe", "alarm"],
  RECOVER: ["✓ Pulih", ""], CMD_REJECT: ["✕ Perintah ditolak", "alarm"], ONLINE: ["✓ Online", ""], OFFLINE: ["⚠ Offline", "crit"],
};
const GROUP = { STEP: ["STEP_ON", "STEP_OFF"], LOAD: ["LOAD"], MODE: ["MODE"], ALARM: ["FAILSAFE", "RECOVER"], CMD_REJECT: ["CMD_REJECT"], NET: ["ONLINE", "OFFLINE"] };
async function loadEvents() { S.events = await fetch("/api/events?limit=500").then((x) => x.json()); renderEvents(); }
function renderEvents() {
  const f = S.evtFilter ? GROUP[S.evtFilter] : null;
  const rows = S.events.filter((e) => !f || f.includes(e.type)).slice(0, 300);
  $("#evt-table").innerHTML = `<tr><th>Waktu</th><th>Jenis</th><th>Detail</th><th>Sumber</th></tr>` + (rows.length ? rows.map((e) => {
    const [lab, cls] = EV[e.type] || [e.type, ""];
    return `<tr class="${cls}"><td class="num">${dmy(e.t)} ${hms(e.t)}</td><td class="etype">${lab}</td><td>${(e.detail || "").replace(/</g, "&lt;")}</td><td>${e.src === "backend" ? "server" : "panel"}</td></tr>`;
  }).join("") : `<tr><td colspan="4">Belum ada kejadian.</td></tr>`);
}
seg("seg-evt", (v) => { S.evtFilter = v; renderEvents(); });
async function loadCsv() {
  const l = await fetch("/api/csv").then((x) => x.json());
  $("#csv-list").innerHTML = l.length ? l.map((f) => `<li><a href="/api/csv/${f.name}">${f.name}</a><span class="num">${nf(f.size / 1024, 1)} kB</span></li>`).join("") : "<li>Belum ada berkas.</li>";
}

// ------------------------------------------------------------------ SSE
function applyTelemetry(t) {
  S.tel = t; S.rxLocal = Date.now();
  if (S.online === null) S.online = true;
  renderSummary(); renderHeader();
  if ($("#pane-kontrol").classList.contains("active")) renderCtl();
}
function connect() {
  const es = new EventSource("/stream");
  es.onopen = () => { S.server = true; renderHeader(); };
  es.onerror = () => { S.server = false; renderHeader(); };
  es.addEventListener("hello", (e) => {
    const d = JSON.parse(e.data);
    Object.assign(S, { status: d.status, online: d.online, broker: d.broker, tReconnect: d.t_reconnect, tariff: d.tariff, scale: d.scale, sessionStart: d.session_start });
    if (d.telemetry) { S.tel = d.telemetry; S.rxLocal = Date.now() - Math.max(0, (d.server_t - d.telemetry._rx) * 1000); }
    renderSummary(); renderHeader(); renderCtl(); loadMini();
  });
  es.addEventListener("telemetry", (e) => { const t = JSON.parse(e.data); applyTelemetry(t); liveAppend(t); });
  es.addEventListener("status", (e) => { S.status = JSON.parse(e.data); renderPanelKv(); renderHeader(); renderCtl(); renderStepsView(); });
  es.addEventListener("online", (e) => { S.online = JSON.parse(e.data).online; renderHeader(); renderCtl(); });
  es.addEventListener("broker", (e) => { S.broker = JSON.parse(e.data).ok; renderHeader(); renderCtl(); });
  es.addEventListener("event", (e) => {
    const ev = JSON.parse(e.data);
    S.events.unshift(ev);
    if ($("#pane-log").classList.contains("active")) renderEvents();
    liveMark(ev);
  });
}

// ------------------------------------------------------------------ mulai
makeCharts();
drawGauge(null);
renderHeader(); renderCtl();
connect();
const startTab = location.hash.slice(1);
if (startTab) { const b = $(`.tab[data-tab="${startTab}"]`); if (b) b.click(); }
window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => location.reload());
