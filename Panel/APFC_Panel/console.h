// Konsol Serial Monitor (115200 bps, akhiri baris dengan Newline) untuk commissioning dan uji.
#pragma once

volatile bool gPrintLive = true;

void printHelp() {
  Serial.println(F(
      "\n=== Perintah APFC (ketik lalu Enter) ===\n"
      "status                 ringkasan keadaan\n"
      "live on|off            tampilkan/sembunyikan data tiap 2 s\n"
      "mode auto|manual       pilih mode\n"
      "step <1-3> on|off      atur step kapasitor (hanya MANUAL)\n"
      "beban on|off           nyalakan/matikan beban demo (relay K1, kontak NC)\n"
      "params                 tampilkan parameter\n"
      "set <kunci> <nilai>    ubah parameter, mis. set c3 4.1 | set pf_target 0.95\n"
      "energy reset           nolkan penghitung kWh/kVArh\n"
      "sd                     status microSD dan daftar file log (unduh: http://<IP panel>/)\n"
      "tagihan                kWh & biaya PLN bulan ini + riwayat (set tarif_kwh | pbjt | pasca)\n"
      "tagihan reset          hapus riwayat tagihan, mulai menghitung lagi dari sekarang\n"
      "raw on|off             tampilkan byte Modbus mentah (diagnosa RS485)\n"
      "i2c                    pindai alamat I2C (cek LCD)\n"
      "factory                hapus parameter, energi & riwayat tagihan tersimpan, lalu restart\n"
      "restart                restart ESP32\n"));
}

void printParams() {
  Serial.printf("pf_target=%.3f hyst=%.2f lead_w=%.2f t_reconnect=%u n_avg=%u n_skip=%u p_min=%.1f\n", P.pf_target,
                P.hyst, P.lead_w, P.t_reconnect, P.n_avg, P.n_skip, P.p_min);
  Serial.printf("c1=%.2f c2=%.2f c3=%.2f uF  tariff=%.2f scale=%.0f\n", P.c_uf[0], P.c_uf[1], P.c_uf[2], P.tariff, P.scale);
  Serial.printf("tarif_kwh=%.2f Rp/kWh  pbjt=%.1f %%  pasca=%u (%s, %d VA)\n", P.tarif_kwh, P.pbjt, P.pasca,
                P.pasca ? "pascabayar" : "prabayar", PLN_DAYA_VA);
}

void printBill() {
  BillNow b;
  BillStore st;
  BillCost c, cp;
  { Lock l; b = gBillNow; st = gBill; c = billCost(b.kwh, false); cp = billCost(b.projKwh, true); }
  char ym[24], t[16], t2[16], t3[16];
  Serial.printf("Tarif Rp%.2f/kWh + PBJT %.1f %% (%s). Ubah: set tarif_kwh <Rp> | set pbjt <%%> | set pasca 0/1\n",
                P.tarif_kwh, P.pbjt, P.pasca ? "pascabayar, min. 40 jam nyala" : "prabayar");
  if (!b.ok) {
    Serial.println(time(nullptr) < 1700000000 ? "Bulan ini: jam belum tersinkron (perlu Wi-Fi berinternet untuk NTP)"
                                              : "Bulan ini: menunggu pembacaan kWh dari meter");
  } else {
    billYmText(ym, sizeof(ym), b.ym, true);
    fmtRp(t, sizeof(t), c.energi); fmtRp(t2, sizeof(t2), c.pbjt); fmtRp(t3, sizeof(t3), c.total);
    Serial.printf("%s: %.2f kWh -> energi Rp%s + PBJT Rp%s = Rp%s (sumber: %s)\n", ym, b.kwh, t, t2, t3,
                  b.src ? "integrasi ESP32" : "register meter DDSU666");
    if (b.flags & BF_PARTIAL) {
      time_t t0 = b.t0;
      struct tm tm;
      localtime_r(&t0, &tm);
      Serial.printf("  dicatat sejak %02d/%02d %02d:%02d (belum sebulan penuh)\n", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    }
    if (b.projKwh >= 0) {
      fmtRp(t, sizeof(t), cp.total);
      Serial.printf("  perkiraan sebulan penuh: %.1f kWh -> Rp%s%s\n", b.projKwh, t, cp.minApplied ? " (rekening minimum)" : "");
    } else {
      Serial.println("  perkiraan sebulan: tunggu data 24 jam");
    }
  }
  if (!st.nh) Serial.println("Riwayat: belum ada bulan yang selesai");
  for (int i = 0; i < st.nh; i++) {
    billYmText(ym, sizeof(ym), st.hist[i].ym);
    fmtRp(t, sizeof(t), st.hist[i].rp);
    Serial.printf("  %-9s %9.2f kWh  Rp%12s%s%s%s%s\n", ym, st.hist[i].kwh, t, st.hist[i].flags & BF_PARTIAL ? "  sebagian" : "",
                  st.hist[i].flags & BF_LATE ? "  batas bulan perkiraan" : "", st.hist[i].flags & BF_ESP32 ? "  integrasi ESP32" : "",
                  st.hist[i].flags & BF_RESET ? "  penghitung sempat reset" : "");
  }
}

void printSd() {
  Shared s;
  { Lock l; s = G; }
  if (!s.sdOk) { Serial.println("microSD tidak terpasang (dicoba lagi tiap 60 s)"); return; }
  std::vector<SdEntry> files;
  uint32_t count = 0;
  char p[32];
  sdTodayPath(p, sizeof(p));
  {
    SdLock k;
    sdStatsLocked();
    sdListLocked(files, count, 40);                  // cukup 40 file terbaru untuk Serial Monitor
  }
  { Lock l; s.sdTotalMB = G.sdTotalMB; s.sdUsedMB = G.sdUsedMB; }
  Serial.printf("microSD: terpakai %lu MB dari %lu MB, sisa %lu MB (hapus otomatis bila sisa < %d MB)\n",
                (unsigned long)s.sdUsedMB, (unsigned long)s.sdTotalMB, (unsigned long)(s.sdTotalMB - s.sdUsedMB),
                SD_MIN_FREE_MB);
  Serial.printf("ditulis sekarang: %s\n", p);
  for (const SdEntry& e : files) Serial.printf("  %-14s %9.1f kB\n", e.name, e.size / 1024.0f);
  if (count > files.size()) Serial.printf("  ... dan %lu file lebih lama\n", (unsigned long)(count - files.size()));
  if (s.wifiOk && WEB_ENABLE) Serial.printf("unduh lewat browser: http://%s/\n", s.ip);
}

void printStatus() {
  Shared s;
  { Lock l; s = G; }
  Serial.printf("\nstate=%s mode=%s meter=%s errTotal=%lu wifi=%d mqtt=%d sd=%d lcd=%d ip=%s\n",
                STATE_NAME[s.state], STATE_NAME[s.mode], s.meterOk ? "OK" : "--", (unsigned long)s.readErrTotal,
                s.wifiOk, s.mqttOk, s.sdOk, s.lcdOk, s.ip);
  Serial.printf("V=%.1f I=%.3f P=%.1f Q=%.1f PF=%.3f f=%.2f Qload=%.1f Qneed=%.1f\n", s.last.v, s.last.i, s.last.p,
                s.last.q, s.last.pf, s.last.f, s.q_load, s.q_need);
  Serial.printf("steps C1=%d C2=%d C3=%d  beban K1=%s (relay %s, pin IO%d terbaca %s)\n", s.step[0], s.step[1], s.step[2],
                s.sock[0] ? "MENYALA" : "PADAM", (LOAD_K1_NC ? !s.sock[0] : s.sock[0]) ? "BEKERJA" : "DIAM", PIN_RELAY1,
                digitalRead(PIN_RELAY1) ? "HIGH" : "LOW");
  Serial.printf("kWh=%.5f kVArh_ind=%.5f kVArh_cap=%.5f kVArh_tanpaAPFC=%.5f penalti=Rp%.0f\n", s.e.kwh,
                s.e.kvarh_ind, s.e.kvarh_cap, s.e.kvarh_load, penaltyRp(s.e));
  for (int k = 0; k < NUM_STEPS; k++)
    Serial.printf("Qstep C%d = %.1f var (%.2f uF)\n", k + 1, qStep(k, s.last.v > 0 ? s.last.v : 220, 50), P.c_uf[k]);
}

void i2cScan() {
  Serial.print("[I2C] perangkat ditemukan:");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.println(n ? "" : " (tidak ada)");
}

void queueCmd(Command c) { xQueueSend(gCmdQ, &c, pdMS_TO_TICKS(100)); }

void handleLine(String line) {
  line.trim();
  line.toLowerCase();
  if (!line.length()) return;
  char a[16] = "", b[16] = "", c3[16] = "";
  sscanf(line.c_str(), "%15s %15s %15s", a, b, c3);
  Command c;
  if (!strcmp(a, "help") || !strcmp(a, "?")) printHelp();
  else if (!strcmp(a, "status")) printStatus();
  else if (!strcmp(a, "params")) printParams();
  else if (!strcmp(a, "tagihan") && !strcmp(b, "reset")) { billClear(); Serial.println("riwayat tagihan dihapus, menghitung lagi dari sekarang"); }
  else if (!strcmp(a, "tagihan")) printBill();
  else if (!strcmp(a, "live")) { gPrintLive = strcmp(b, "off") != 0; Serial.printf("live %s\n", gPrintLive ? "on" : "off"); }
  else if (!strcmp(a, "raw")) { gDumpRaw = strcmp(b, "off") != 0; Serial.printf("raw %s\n", gDumpRaw ? "on" : "off"); }
  else if (!strcmp(a, "mode")) {
    if (strcmp(b, "auto") && strcmp(b, "manual")) { Serial.println("pakai: mode auto|manual"); return; }
    c.type = C_MODE; c.a = !strcmp(b, "manual") ? ST_MANUAL : ST_AUTO; queueCmd(c);
  } else if (!strcmp(a, "beban")) {
    if (strcmp(b, "on") && strcmp(b, "off")) { Serial.println("pakai: beban on|off"); return; }
    c.type = C_SOCKET; c.a = 1; c.b = !strcmp(b, "on"); queueCmd(c);
  } else if (!strcmp(a, "step") || !strcmp(a, "socket")) {
    c.type = !strcmp(a, "step") ? C_STEP : C_SOCKET;
    c.a = atoi(b);
    c.b = !strcmp(c3, "on") ? 1 : 0;
    if (strcmp(c3, "on") && strcmp(c3, "off")) { Serial.printf("pakai: %s <n> on|off\n", a); return; }
    queueCmd(c);
  }
  else if (!strcmp(a, "set")) {
    if (!b[0] || !c3[0]) { Serial.println("pakai: set <kunci> <nilai>"); return; }
    c.type = C_SET; strlcpy(c.key, b, sizeof(c.key)); c.val = atof(c3); queueCmd(c);
  } else if (!strcmp(a, "energy") && !strcmp(b, "reset")) { c.type = C_ENERGY_RESET; queueCmd(c); }
  else if (!strcmp(a, "sd")) printSd();
  else if (!strcmp(a, "i2c")) i2cScan();
  else if (!strcmp(a, "factory")) { nvsFactoryReset(); billClear(); Serial.println("NVS dihapus, restart..."); delay(300); ESP.restart(); }
  else if (!strcmp(a, "restart")) ESP.restart();
  else Serial.println("perintah tidak dikenal, ketik: help");
}

void consolePoll() {
  static String buf;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { if (buf.length()) handleLine(buf); buf = ""; }
    else if (buf.length() < 64) buf += ch;
  }
}
