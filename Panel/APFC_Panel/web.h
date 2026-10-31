// Web server kecil di panel (port WEB_PORT): tagihan bulanan dan log microSD tanpa mencabut kartu.
//   GET  /                      halaman sederhana: tagihan bulan ini + riwayat, kapasitas kartu, daftar file
//   GET  /bill                  JSON: tagihan bulan berjalan + riwayat 12 bulan
//   GET  /sd/list               JSON: kapasitas kartu + daftar file di /log
//   GET  /sd/file?name=X.csv    unduh satu file
//   POST /sd/delete?name=X.csv  hapus satu file (file yang sedang ditulis hari ini ditolak)
// Dijalankan task "web" (core 0, prioritas 1). Kendali kapasitor di core 1 tidak terpengaruh.
// Tanpa kata sandi: hanya untuk jaringan lokal (hotspot/Wi-Fi rumah), jangan dibuka ke internet.
#pragma once
#include <WebServer.h>
#include <stdarg.h>

WebServer web(WEB_PORT);

// Nama file yang boleh diunduh/dihapus: huruf, angka, _ - . ; berakhiran .csv; tanpa '/' dan '..'
bool webNameOk(const char* n) {
  size_t L = strlen(n);
  if (L < 5 || L >= sizeof(SdEntry::name) || strcmp(n + L - 4, ".csv") || strstr(n, "..")) return false;
  for (size_t i = 0; i < L; i++) {
    char c = n[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

// Respons bertahap (chunked) agar daftar file yang panjang tidak perlu ditampung utuh di RAM.
struct WebOut {
  char   buf[1024];
  size_t n = 0;
  void begin(const char* type) {
    web.sendHeader("Cache-Control", "no-store");
    web.setContentLength(CONTENT_LENGTH_UNKNOWN);
    web.send(200, type, "");
  }
  void add(const char* s) {
    size_t L = strlen(s);
    while (L) {
      size_t k = sizeof(buf) - n;
      if (k > L) k = L;
      memcpy(buf + n, s, k);
      n += k; s += k; L -= k;
      if (n == sizeof(buf)) flush();
    }
  }
  void addf(const char* fmt, ...) {
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    add(t);
  }
  void flush() { if (n) { web.sendContent(buf, n); n = 0; } }
  void end() { flush(); web.sendContent(""); }
};

void webJson(int code, bool ok, const char* msg) {
  char b[192];
  snprintf(b, sizeof(b), "{\"ok\":%d,\"msg\":\"%s\"}", ok ? 1 : 0, msg);
  web.send(code, "application/json", b);
}

bool webSdReady() { Lock l; return G.sdOk; }

// Daftar file + kapasitas terbaru (sdStatsLocked memperbarui G.sdTotalMB/G.sdUsedMB). count = jumlah semua file.
bool webCollect(std::vector<SdEntry>& files, uint32_t& count, char* today, size_t todayN) {
  char p[32];
  sdTodayPath(p, sizeof(p));
  strlcpy(today, sdBaseName(p), todayN);
  SdLock k;
  sdStatsLocked();
  return sdListLocked(files, count);
}

// Ukuran file ringkas: "512 B", "84.2 kB", "1.13 MB"
void webSize(char* out, size_t n, uint32_t b) {
  if (b < 1024) snprintf(out, n, "%lu B", (unsigned long)b);
  else if (b < 1048576) snprintf(out, n, "%.1f kB", b / 1024.0f);
  else snprintf(out, n, "%.2f MB", b / 1048576.0f);
}

void handleList() {
  if (!webSdReady()) { webJson(503, false, "microSD tidak terpasang di panel"); return; }
  std::vector<SdEntry> files;
  uint32_t count = 0;
  char today[24];
  if (!webCollect(files, count, today, sizeof(today))) { webJson(500, false, "folder /log tidak dapat dibaca"); return; }
  uint32_t tot, used;
  { Lock l; tot = G.sdTotalMB; used = G.sdUsedMB; }
  WebOut o;
  o.begin("application/json");
  o.addf("{\"ok\":1,\"panel\":\"%s\",\"fw\":\"%s\",\"today\":\"%s\",\"total_mb\":%lu,\"used_mb\":%lu,\"free_mb\":%lu,"
         "\"min_free_mb\":%d,\"count\":%lu,\"files\":[",
         PANEL_ID, FW_VERSION, today, (unsigned long)tot, (unsigned long)used, (unsigned long)(tot - used), SD_MIN_FREE_MB,
         (unsigned long)count);
  bool first = true;
  for (const SdEntry& e : files) {
    if (!webNameOk(e.name)) continue;
    o.addf("%s{\"name\":\"%s\",\"size\":%lu}", first ? "" : ",", e.name, (unsigned long)e.size);
    first = false;
  }
  o.add("]}");
  o.end();
}

void handleFile() {
  String n = web.arg("name");
  if (!webNameOk(n.c_str())) { webJson(400, false, "nama file tidak valid"); return; }
  if (!webSdReady()) { webJson(503, false, "microSD tidak terpasang di panel"); return; }
  String p = "/log/" + n;
  File f;
  size_t size = 0;
  bool opened = false;
  {
    SdLock k;
    f = SD.open(p.c_str(), FILE_READ);
    if (f && !f.isDirectory()) { size = f.size(); strlcpy(gSdOpen, n.c_str(), sizeof(gSdOpen)); opened = true; }
    else if (f) f.close();
  }
  if (!opened) { webJson(404, false, "file tidak ditemukan"); return; }

  web.sendHeader("Content-Disposition", String("attachment; filename=\"") + n + "\"");
  web.sendHeader("Cache-Control", "no-store");
  web.setContentLength(size);
  web.send(200, "text/csv; charset=utf-8", "");
  // Baca per potongan 2 kB. Kunci SD hanya dipegang saat membaca, sehingga task log tetap bisa menulis.
  static uint8_t buf[2048];
  size_t left = size;
  while (left > 0 && web.client().connected()) {
    size_t want = left < sizeof(buf) ? left : sizeof(buf), got;
    { SdLock k; got = f.read(buf, want); }
    if (got == 0) break;
    web.sendContent((const char*)buf, got);
    left -= got;
    vTaskDelay(1);                                     // beri giliran task lain di core 0
  }
  { SdLock k; f.close(); gSdOpen[0] = 0; }
  if (left) Serial.printf("[WEB] unduh %s terputus (%u dari %u byte)\n", n.c_str(), (unsigned)(size - left), (unsigned)size);
}

void handleDelete() {
  String n = web.arg("name");
  if (!webNameOk(n.c_str())) { webJson(400, false, "nama file tidak valid"); return; }
  if (!webSdReady()) { webJson(503, false, "microSD tidak terpasang di panel"); return; }
  char today[32];
  sdTodayPath(today, sizeof(today));
  if (n == sdBaseName(today)) { webJson(409, false, "file hari ini sedang ditulis, tidak boleh dihapus"); return; }
  String p = "/log/" + n;
  bool ok;
  {
    SdLock k;
    ok = SD.exists(p.c_str()) && SD.remove(p.c_str());
    if (ok) sdStatsLocked();
  }
  if (!ok) { webJson(404, false, "file tidak ditemukan atau gagal dihapus"); return; }
  char d[64];
  snprintf(d, sizeof(d), "%s dihapus lewat web", n.c_str());
  postEvent("SD_DELETE", d);
  char m[64];
  snprintf(m, sizeof(m), "%s dihapus", n.c_str());
  webJson(200, true, m);
}

// Rincian tagihan untuk /bill dan halaman utama (dibaca dengan Lock agar konsisten)
struct WebBill {
  BillNow   now;
  BillStore st;
  BillCost  c, cp;
  float     tarif, pbjt;
  uint8_t   pasca;
};
void webBill(WebBill& w) {
  Lock l;
  w.now = gBillNow;
  w.st = gBill;
  w.c = billCost(w.now.kwh, false);
  w.cp = billCost(w.now.projKwh, true);
  w.tarif = P.tarif_kwh;
  w.pbjt = P.pbjt;
  w.pasca = P.pasca;
}

// GET /bill — tagihan bulan berjalan + riwayat (JSON)
void handleBill() {
  WebBill w;
  webBill(w);
  time_t now = time(nullptr);
  WebOut o;
  o.begin("application/json");
  o.addf("{\"ok\":1,\"panel\":\"%s\",\"fw\":\"%s\",\"now\":%lu,\"time_ok\":%d,\"tarif\":%.2f,\"pbjt\":%.2f,\"pasca\":%u,"
         "\"daya_va\":%d,\"min_kwh\":%.1f,",
         PANEL_ID, FW_VERSION, (unsigned long)(now > 1700000000 ? now : 0), now > 1700000000 ? 1 : 0, w.tarif, w.pbjt,
         w.pasca, PLN_DAYA_VA, w.pasca ? 40.0f * PLN_DAYA_VA / 1000.0f : 0.0f);
  if (w.now.ok) {
    o.addf("\"cur\":{\"ym\":%lu,\"from\":%lu,\"to\":%lu,\"start\":%lu,\"kwh\":%.3f,\"energi\":%.0f,\"pbjt_rp\":%.0f,\"total\":%.0f,",
           (unsigned long)w.now.ym, (unsigned long)w.now.tFrom, (unsigned long)w.now.tTo, (unsigned long)w.now.t0, w.now.kwh,
           w.c.energi, w.c.pbjt, w.c.total);
    if (w.now.projKwh >= 0)
      o.addf("\"proj_kwh\":%.2f,\"proj_total\":%.0f,\"proj_min\":%d,", w.now.projKwh, w.cp.total, w.cp.minApplied ? 1 : 0);
    o.addf("\"fl\":%u,\"src\":\"%s\"},", w.now.flags, w.now.src ? "esp32" : "meter");
  } else {
    o.add("\"cur\":null,");
  }
  o.add("\"hist\":[");
  for (int i = 0; i < w.st.nh; i++)
    o.addf("%s{\"ym\":%lu,\"kwh\":%.3f,\"total\":%.0f,\"fl\":%u}", i ? "," : "", (unsigned long)w.st.hist[i].ym,
           w.st.hist[i].kwh, w.st.hist[i].rp, w.st.hist[i].flags);
  o.add("]}");
  o.end();
}

// Catatan singkat dari flag bulan
void webFlags(char* out, size_t n, uint8_t f) {
  out[0] = 0;
  if (f & BF_PARTIAL) strlcat(out, "belum sebulan penuh; ", n);
  if (f & BF_LATE) strlcat(out, "batas bulan perkiraan; ", n);
  if (f & BF_ESP32) strlcat(out, "kWh dari ESP32; ", n);
  if (f & BF_RESET) strlcat(out, "penghitung sempat reset; ", n);
  size_t L = strlen(out);
  if (L >= 2) out[L - 2] = 0;
}

// Halaman utama panel: tagihan bulan ini, riwayat, log microSD (cadangan bila dashboard laptop tidak berjalan).
void handleRoot() {
  bool sdOk;
  char ip[16];
  { Lock l; sdOk = G.sdOk; strlcpy(ip, G.ip, sizeof(ip)); }
  WebBill w;
  webBill(w);
  std::vector<SdEntry> files;
  uint32_t count = 0;
  char today[24] = "";
  bool listed = sdOk && webCollect(files, count, today, sizeof(today));
  uint32_t tot, used;
  { Lock l; tot = G.sdTotalMB; used = G.sdUsedMB; }
  char t1[24], t2[24], t3[24], fl[96];

  WebOut o;
  o.begin("text/html; charset=utf-8");
  o.add("<!doctype html><html lang=\"id\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  o.addf("<title>APFC %s</title>", PANEL_ID);
  o.add("<style>body{font:15px system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#f2f1ed;color:#111}"
        "header{background:#14212e;color:#f3f5f7;padding:14px 18px}h1{font-size:18px;margin:0}header small{color:#a9b6c2}"
        "main{max-width:760px;margin:0 auto;padding:16px}.card{background:#fcfcfb;border:1px solid #e2e0da;border-radius:12px;"
        "padding:14px 16px;margin-bottom:14px}.bar{height:10px;background:#e8e7e2;border-radius:5px;overflow:hidden;margin:8px 0}"
        ".bar i{display:block;height:100%;background:#2a78d6}table{width:100%;border-collapse:collapse}"
        "td,th{padding:8px 6px;border-bottom:1px solid #eceae4;text-align:left}th{font-size:13px;color:#52514e}"
        ".r{text-align:right;white-space:nowrap}a{color:#2a78d6;font-weight:600;text-decoration:none}"
        "button{border:1px solid #e3a3a3;color:#a52a2a;background:#fff;border-radius:8px;padding:4px 10px;cursor:pointer}"
        ".m{color:#75736d;font-size:13px}.w{color:#0a6e0a;font-size:13px}h2{font-size:15px;margin:0 0 8px}"
        ".big{font-size:28px;font-weight:700;margin:2px 0}.kv{display:flex;flex-wrap:wrap;gap:6px 22px;margin:8px 0}"
        ".kv b{display:block;font-size:18px}</style></head><body>");
  o.addf("<header><h1>APFC Panel %s</h1><small>firmware %s · %s</small></header><main>", PANEL_ID, FW_VERSION, ip);

  // ---- tagihan bulan ini
  o.add("<div class=\"card\"><h2>Tagihan PLN bulan ini</h2>");
  if (!w.now.ok) {
    o.add(time(nullptr) < 1700000000 ? "<p class=\"m\">Jam panel belum tersinkron. Sambungkan panel ke Wi-Fi yang "
                                       "terhubung internet (NTP).</p>"
                                     : "<p class=\"m\">Menunggu pembacaan kWh dari meter.</p>");
  } else {
    billYmText(t1, sizeof(t1), w.now.ym, true);
    fmtRp(t2, sizeof(t2), w.c.total);
    o.addf("<div class=\"m\">%s · 1 s.d. akhir bulan</div><div class=\"big\">%.2f kWh · Rp%s</div>", t1, w.now.kwh, t2);
    fmtRp(t1, sizeof(t1), w.c.energi);
    fmtRp(t2, sizeof(t2), w.c.pbjt);
    o.addf("<div class=\"m\">energi Rp%s + PBJT %.1f%% Rp%s (s.d. sekarang)</div>", t1, w.pbjt, t2);
    if (w.now.flags & BF_PARTIAL) {
      time_t t0 = w.now.t0;
      struct tm tm;
      localtime_r(&t0, &tm);
      o.addf("<div class=\"m\">Dicatat sejak %d/%d pukul %02d:%02d, jadi belum sebulan penuh.</div>", tm.tm_mday,
             tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    }
    if (w.now.projKwh >= 0) {
      fmtRp(t3, sizeof(t3), w.cp.total);
      o.addf("<div class=\"kv\"><span>Perkiraan sebulan<b>%.0f kWh · Rp%s</b></span></div>", w.now.projKwh, t3);
      if (w.cp.minApplied) o.add("<div class=\"m\">Termasuk rekening minimum pascabayar (40 jam nyala).</div>");
    } else {
      o.add("<div class=\"m\">Perkiraan sebulan muncul setelah data 24 jam.</div>");
    }
  }
  o.addf("<p class=\"m\">Tarif Rp%.2f/kWh, PBJT %.1f%%, %s %d VA. kWh dari %s. Biaya admin dan meterai tidak dihitung.</p>",
         w.tarif, w.pbjt, w.pasca ? "pascabayar" : "prabayar", PLN_DAYA_VA,
         w.now.src ? "integrasi ESP32" : "register meter DDSU666");
  o.add("</div>");

  // ---- riwayat
  o.add("<div class=\"card\"><h2>Riwayat bulanan</h2><table><tr><th>Bulan</th><th class=\"r\">kWh</th>"
        "<th class=\"r\">Biaya</th><th>Catatan</th></tr>");
  if (!w.st.nh) o.add("<tr><td colspan=\"4\" class=\"m\">Belum ada bulan yang selesai.</td></tr>");
  for (int i = 0; i < w.st.nh; i++) {
    billYmText(t1, sizeof(t1), w.st.hist[i].ym);
    fmtRp(t2, sizeof(t2), w.st.hist[i].rp);
    webFlags(fl, sizeof(fl), w.st.hist[i].flags);
    o.addf("<tr><td>%s</td><td class=\"r\">%.2f</td><td class=\"r\">Rp%s</td><td class=\"m\">%s</td></tr>", t1,
           w.st.hist[i].kwh, t2, fl);
  }
  o.add("</table></div>");

  // ---- log microSD
  if (!listed) {
    o.add("<div class=\"card\"><h2>Log microSD</h2><p class=\"m\">microSD tidak terpasang. Pasang kartu (FAT32); panel "
          "memeriksa lagi tiap 60 detik.</p></div></main></body></html>");
    o.end();
    return;
  }
  unsigned pct = tot ? (unsigned)((uint64_t)used * 100 / tot) : 0;
  o.addf("<div class=\"card\"><h2>Log microSD</h2><div class=\"bar\"><i style=\"width:%u%%\"></i></div>"
         "<div class=\"m\">Terpakai %lu MB dari %lu MB, sisa %lu MB. File harian tertua dihapus otomatis bila sisa "
         "&lt; %d MB.</div>",
         pct < 1 && used ? 1 : pct, (unsigned long)used, (unsigned long)tot, (unsigned long)(tot - used), SD_MIN_FREE_MB);
  o.add("<table><tr><th>File</th><th class=\"r\">Ukuran</th><th class=\"r\"></th></tr>");
  int shown = 0;
  char sz[16];
  for (const SdEntry& e : files) {
    if (!webNameOk(e.name)) continue;
    shown++;
    webSize(sz, sizeof(sz), e.size);
    o.addf("<tr><td><a href=\"/sd/file?name=%s\">%s</a></td><td class=\"r\">%s</td><td class=\"r\">", e.name, e.name, sz);
    if (!strcmp(e.name, today)) o.add("<span class=\"w\">sedang ditulis</span>");
    else o.addf("<button onclick=\"del('%s')\">Hapus</button>", e.name);
    o.add("</td></tr>");
  }
  if (!shown) o.add("<tr><td colspan=\"3\" class=\"m\">Belum ada file log.</td></tr>");
  o.add("</table>");
  if (count > files.size())
    o.addf("<p class=\"m\">Ditampilkan %u file terbaru dari %lu. File yang lebih lama tetap ada di kartu.</p>",
           (unsigned)files.size(), (unsigned long)count);
  o.add("</div><p class=\"m\">YYYYMMDD.csv = data tiap 10 detik (kolom ts_device = waktu Unix UTC), "
        "event.csv = kejadian panel. Hanya untuk jaringan lokal.</p></main><script>"
        "function del(n){if(!confirm('Hapus '+n+' dari microSD? Data di kartu tidak bisa dikembalikan.'))return;"
        "fetch('/sd/delete?name='+encodeURIComponent(n),{method:'POST'}).then(r=>r.json())"
        ".then(j=>{alert(j.msg);location.reload()}).catch(()=>alert('panel tidak menjawab'))}</script></body></html>");
  o.end();
}

// ---------------- task web (core 0, prioritas 1) ----------------
void taskWeb(void*) {
  for (;;) {                                           // tunggu Wi-Fi tersambung pertama kali
    bool w;
    { Lock l; w = G.wifiOk; }
    if (w) break;
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  web.enableCORS(true);
  web.on("/", HTTP_GET, handleRoot);
  web.on("/bill", HTTP_GET, handleBill);
  web.on("/sd/list", HTTP_GET, handleList);
  web.on("/sd/file", HTTP_GET, handleFile);
  web.on("/sd/delete", HTTP_POST, handleDelete);
  web.onNotFound([]() { webJson(404, false, "alamat tidak dikenal"); });
  web.begin();
  char ip[16];
  { Lock l; strlcpy(ip, G.ip, sizeof(ip)); }
  Serial.printf("[WEB] tagihan & log microSD: http://%s/\n", ip);
  for (;;) {
    web.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
