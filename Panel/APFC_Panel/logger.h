// Log cadangan ke microSD: /log/YYYYMMDD.csv tiap 10 s dan /log/event.csv.
#pragma once
#include <SPI.h>
#include <SD.h>

bool sdMount() {
  if (!SD.begin(PIN_SD_CS, SPI, 4000000)) return false;
  if (!SD.exists("/log")) SD.mkdir("/log");
  return true;
}

void sdAppend(const char* path, const char* header, const char* line) {
  bool isNew = !SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (isNew && header) f.println(header);
  f.println(line);
  f.close();
}

void taskLog(void*) {
  bool ok = sdMount();
  Serial.println(ok ? "[SD] microSD siap" : "[SD] microSD tidak terdeteksi (log cadangan nonaktif, coba lagi tiap 60 s)");
  { Lock l; G.sdOk = ok; }
  uint32_t lastLog = 0, lastTry = millis();
  char path[32], line[256];
  const char* HDR = "ts_device,uptime_s,seq,v,i,p,q,s,pf,f,q_load,q_need,kwh,kvarh_ind,kvarh_cap,kvarh_load,s1,s2,s3,xs1,xs2,mode,state";
  for (;;) {
    if (!ok && millis() - lastTry > 60000) {
      lastTry = millis();
      ok = sdMount();
      Lock l; G.sdOk = ok;
    }
    OutMsg m;
    while (xQueueReceive(gLogQ, &m, 0) == pdTRUE)
      if (ok) sdAppend("/log/event.csv", nullptr, m.payload);

    if (ok && millis() - lastLog >= LOG_PERIOD_MS) {
      lastLog = millis();
      Shared s;
      { Lock l; s = G; }
      if (s.seq > 0) {
        time_t t = time(nullptr);
        if (t > 1700000000) {
          struct tm tmv;
          localtime_r(&t, &tmv);
          strftime(path, sizeof(path), "/log/%Y%m%d.csv", &tmv);
        } else {
          strlcpy(path, "/log/no_time.csv", sizeof(path));
        }
        snprintf(line, sizeof(line), "%lu,%lu,%lu,%.1f,%.3f,%.1f,%.1f,%.1f,%.3f,%.2f,%.1f,%.1f,%.5f,%.5f,%.5f,%.5f,%d,%d,%d,%d,%d,%s,%s",
                 (unsigned long)unixTime(), (unsigned long)(millis() / 1000), (unsigned long)s.seq, s.last.v, s.last.i,
                 s.last.p, s.last.q, s.last.s, s.last.pf, s.last.f, s.q_load, s.q_need, s.e.kwh, s.e.kvarh_ind,
                 s.e.kvarh_cap, s.e.kvarh_load, s.step[0], s.step[1], s.step[2], s.sock[0], s.sock[1],
                 STATE_NAME[s.mode], STATE_NAME[s.state]);
        File probe = SD.open("/log");
        if (!probe) { ok = false; Lock l; G.sdOk = false; Serial.println("[SD] kartu terlepas"); }
        else { probe.close(); sdAppend(path, HDR, line); }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
