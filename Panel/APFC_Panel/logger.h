// Log cadangan ke microSD: /log/YYYYMMDD.csv tiap 10 s dan /log/event.csv.
// Kartu diakses dua task (log dan web), jadi setiap akses SD dikunci bergantian dengan SdLock.
// Bila sisa ruang < SD_MIN_FREE_MB, file harian TERTUA dihapus otomatis (file hari ini tidak pernah dihapus).
#pragma once
#include <SPI.h>
#include <SD.h>
#include <sys/stat.h>
#include <vector>
#include <algorithm>

#define SD_MOUNT    "/sd"   // titik mount VFS kartu (bawaan pustaka SD)
#define SD_LIST_MAX 500     // maks. file di daftar web (RAM terbatas); bila lebih, yang tampil file terbaru

SemaphoreHandle_t gSdMutex;
struct SdLock {
  SdLock()  { xSemaphoreTake(gSdMutex, portMAX_DELAY); }
  ~SdLock() { xSemaphoreGive(gSdMutex); }
};
char gSdOpen[24] = "";   // file yang sedang diunduh lewat web ("" = tidak ada); dibaca/ditulis dengan SdLock

const char* SD_HDR = "ts_device,uptime_s,seq,v,i,p,q,s,pf,f,q_load,q_need,kwh,kvarh_ind,kvarh_cap,kvarh_load,s1,s2,s3,beban_k1,mode,state";

struct SdEntry { char name[24]; uint32_t size; };

// Path file log yang sedang ditulis: /log/YYYYMMDD.csv, atau /log/no_time.csv bila jam belum tersinkron.
void sdTodayPath(char* path, size_t n) {
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(path, n, "/log/%Y%m%d.csv", &tmv);
  } else {
    strlcpy(path, "/log/no_time.csv", n);
  }
}

// Nama tanpa folder (paket esp32 1.x mengembalikan path lengkap, 2.x/3.x hanya nama).
const char* sdBaseName(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

// File harian: 8 digit + ".csv" (YYYYMMDD.csv)
bool sdIsDaily(const char* name) {
  if (strlen(name) != 12 || strcmp(name + 8, ".csv")) return false;
  for (int i = 0; i < 8; i++)
    if (!isdigit((unsigned char)name[i])) return false;
  return true;
}

// ---------------- fungsi berikut dipanggil dengan SdLock aktif ----------------
bool sdMountLocked() {
  SD.end();                                   // lepas sesi lama: wajib agar kartu yang dicabut-pasang dikenali lagi
  if (!SD.begin(PIN_SD_CS, SPI, 4000000, SD_MOUNT)) return false;
  if (!SD.exists("/log")) SD.mkdir("/log");
  return true;
}

void sdAppendLocked(const char* path, const char* header, const char* line) {
  bool isNew = !SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  if (isNew && header) f.println(header);
  f.println(line);
  f.close();
}

// Kapasitas kartu (MB) → G, untuk status MQTT, LCD dan halaman web.
void sdStatsLocked() {
  uint32_t tot = (uint32_t)(SD.totalBytes() >> 20), used = (uint32_t)(SD.usedBytes() >> 20);
  Lock l;
  if (G.sdTotalMB != tot || G.sdUsedMB != used) G.statusDirty = true;
  G.sdTotalMB = tot;
  G.sdUsedMB = used;
}

// Nama berikutnya di folder yang dibuka; "" = habis. Hanya membaca direktori (tidak membuka tiap file),
// jadi tetap cepat walau kartu berisi ratusan file harian.
String sdNextName(File& dir, bool& isDir) {
  isDir = false;
  return dir.getNextFileName(&isDir);
}

// push_back yang tidak membuat panel restart bila RAM tidak cukup
bool sdPush(std::vector<SdEntry>& v, const SdEntry& e) {
#if __cpp_exceptions
  try { v.push_back(e); } catch (...) { return false; }
#else
  v.push_back(e);
#endif
  return true;
}

// Isi /log (nama + ukuran), terbaru dulu (event.csv dan no_time.csv ikut di atas, lalu file harian terbaru).
// Paling banyak maxN entri disimpan; total = jumlah semua file di /log.
bool sdListLocked(std::vector<SdEntry>& out, uint32_t& total, size_t maxN = SD_LIST_MAX) {
  out.clear();
  total = 0;
  File dir = SD.open("/log");
  if (!dir || !dir.isDirectory()) return false;
  uint32_t cnt = 0;
  bool isDir;
  for (String p = sdNextName(dir, isDir); p.length(); p = sdNextName(dir, isDir)) {
    if ((++cnt & 31) == 0) vTaskDelay(1);            // kartu berisi ratusan file: beri giliran task lain
    const char* nm = sdBaseName(p.c_str());
    if (isDir || strlen(nm) >= sizeof(SdEntry::name)) continue;
    total++;
    SdEntry e;
    strlcpy(e.name, nm, sizeof(e.name));
    e.size = 0;
    if (out.size() < maxN) {
      if (sdPush(out, e)) continue;
      maxN = out.size();                             // RAM tidak cukup: daftar dipersingkat
    }
    if (out.empty()) continue;
    size_t mi = 0;                                   // daftar penuh: ganti nama terkecil (paling lama) bila e lebih baru
    for (size_t i = 1; i < out.size(); i++) if (strcmp(out[i].name, out[mi].name) < 0) mi = i;
    if (strcmp(e.name, out[mi].name) > 0) out[mi] = e;
  }
  dir.close();
  std::sort(out.begin(), out.end(), [](const SdEntry& a, const SdEntry& b) { return strcmp(a.name, b.name) > 0; });
  char path[48];
  struct stat st;
  for (size_t i = 0; i < out.size(); i++) {         // ukuran lewat stat(): tanpa membuka file
    snprintf(path, sizeof(path), SD_MOUNT "/log/%s", out[i].name);
    out[i].size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    if ((i & 31) == 31) vTaskDelay(1);
  }
  return true;
}

// File harian dengan tanggal paling lama, kecuali skip1/skip2. Mengembalikan false bila tidak ada.
bool sdOldestDailyLocked(char* out, size_t n, const char* skip1, const char* skip2) {
  out[0] = 0;
  File dir = SD.open("/log");
  if (!dir || !dir.isDirectory()) return false;
  uint32_t cnt = 0;
  bool isDir;
  for (String p = sdNextName(dir, isDir); p.length(); p = sdNextName(dir, isDir)) {
    if ((++cnt & 63) == 0) vTaskDelay(1);
    const char* nm = sdBaseName(p.c_str());
    if (!isDir && sdIsDaily(nm) && strcmp(nm, skip1) && strcmp(nm, skip2) && (!out[0] || strcmp(nm, out) < 0))
      strlcpy(out, nm, n);
  }
  dir.close();
  return out[0] != 0;
}

// Hapus file harian tertua sampai sisa ruang ≥ SD_MIN_FREE_MB. File hari ini dan file yang sedang diunduh dilewati.
void sdAutoCleanLocked() {
  static bool warned = false;                       // SD_FULL dikirim sekali per kejadian
  const uint64_t minFree = (uint64_t)SD_MIN_FREE_MB << 20;
  const uint64_t total = SD.totalBytes();
  auto freeBytes = [total]() { uint64_t u = SD.usedBytes(); return total > u ? total - u : 0; };
  uint64_t fr = freeBytes();
  if (total == 0 || fr >= minFree) { warned = false; return; }

  char today[32], oldest[24], p[32], d[96];
  sdTodayPath(today, sizeof(today));
  const char* stuck = nullptr;                      // alasan berhenti sebelum ruang cukup
  for (int n = 0; n < 60 && fr < minFree; n++) {    // maks. 60 file per pemeriksaan, sisanya pemeriksaan berikutnya
    if (!sdOldestDailyLocked(oldest, sizeof(oldest), sdBaseName(today), gSdOpen)) { stuck = "tidak ada file lama untuk dihapus"; break; }
    snprintf(p, sizeof(p), "/log/%s", oldest);
    if (!SD.remove(p)) { stuck = "file lama gagal dihapus"; break; }
    fr = freeBytes();
    snprintf(d, sizeof(d), "%s dihapus otomatis, sisa %lu MB", oldest, (unsigned long)(fr >> 20));
    postEvent("SD_CLEAN", d);
    vTaskDelay(1);
  }
  if (fr >= minFree) warned = false;
  else if (stuck && !warned) {
    warned = true;
    snprintf(d, sizeof(d), "microSD hampir penuh: sisa %lu MB, %s", (unsigned long)(fr >> 20), stuck);
    postEvent("SD_FULL", d);
  }
  sdStatsLocked();
}

// ---------------- task log (core 0, prioritas 1) ----------------
void taskLog(void*) {
  bool ok;
  {
    SdLock k;
    ok = sdMountLocked();
    if (ok) { sdAutoCleanLocked(); sdStatsLocked(); }
  }
  Serial.println(ok ? "[SD] microSD siap" : "[SD] microSD tidak terdeteksi (log cadangan nonaktif, coba lagi tiap 60 s)");
  { Lock l; G.sdOk = ok; G.statusDirty = true; }
  uint32_t lastLog = 0, lastTry = millis(), lastCheck = millis();
  char path[32], line[256];
  for (;;) {
    if (!ok && millis() - lastTry > 60000) {
      lastTry = millis();
      {
        SdLock k;
        if (!gSdOpen[0]) {
          ok = sdMountLocked();
          if (ok) { sdAutoCleanLocked(); sdStatsLocked(); }
        }
      }
      if (ok) {
        Serial.println("[SD] microSD terpasang kembali");
        Lock l; G.sdOk = true; G.statusDirty = true;
      }
    }
    OutMsg m;
    while (xQueueReceive(gLogQ, &m, 0) == pdTRUE)
      if (ok) { SdLock k; sdAppendLocked("/log/event.csv", nullptr, m.payload); }

    if (ok && millis() - lastCheck >= SD_CHECK_MS) {
      lastCheck = millis();
      SdLock k;
      sdAutoCleanLocked();
      sdStatsLocked();                               // sisa ruang untuk LCD/status tetap terbaru
    }

    if (ok && millis() - lastLog >= LOG_PERIOD_MS) {
      lastLog = millis();
      Shared s;
      { Lock l; s = G; }
      if (s.seq > 0) {
        sdTodayPath(path, sizeof(path));
        snprintf(line, sizeof(line), "%lu,%lu,%lu,%.1f,%.3f,%.1f,%.1f,%.1f,%.3f,%.2f,%.1f,%.1f,%.5f,%.5f,%.5f,%.5f,%d,%d,%d,%d,%s,%s",
                 (unsigned long)unixTime(), (unsigned long)(millis() / 1000), (unsigned long)s.seq, s.last.v, s.last.i,
                 s.last.p, s.last.q, s.last.s, s.last.pf, s.last.f, s.q_load, s.q_need, s.e.kwh, s.e.kvarh_ind,
                 s.e.kvarh_cap, s.e.kvarh_load, s.step[0], s.step[1], s.step[2], s.sock[0],
                 STATE_NAME[s.mode], STATE_NAME[s.state]);
        bool lost = false;
        {
          SdLock k;
          File probe = SD.open("/log");
          if (!probe) lost = true;
          else { probe.close(); sdAppendLocked(path, SD_HDR, line); }
        }
        if (lost) {
          ok = false;
          lastTry = millis();
          Serial.println("[SD] kartu terlepas");
          Lock l; G.sdOk = false; G.statusDirty = true;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
