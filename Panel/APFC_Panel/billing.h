// Tagihan PLN bulanan (rumah tangga): kWh dari tanggal 1 pukul 00:00 WIB sampai akhir bulan, lalu biayanya.
//
// Sumber kWh: register energi meter DDSU666 (alamat 4000H). Register ini tetap menghitung walau ESP32 mati
// atau restart, sama seperti kWh meter PLN. Bila register tidak terbaca, dipakai integrasi daya di ESP32
// (energi saat panel mati tidak ikut terhitung; ditandai di riwayat).
// Pergantian bulan memakai jam dari NTP, jadi panel perlu Wi-Fi yang tersambung ke internet.
//
// Biaya = kWh × tarif + PBJT (pajak barang dan jasa tertentu atas tenaga listrik, dulu PPJ).
// Pascabayar: kWh minimum = 40 jam nyala × daya (rekening minimum). Rumah tangga ≤ 6.600 VA tidak dikenai PPN.
// Biaya admin bank/loket dan bea meterai tidak dihitung.
#pragma once
#include <Preferences.h>
#include <time.h>

#define BILL_HIST_N 12          // bulan yang disimpan di riwayat

enum : uint8_t {
  BF_PARTIAL = 1,   // pencatatan dimulai setelah tanggal 1 (mis. firmware baru dipasang): belum sebulan penuh
  BF_LATE    = 2,   // pergantian bulan terlambat terdeteksi (panel mati / jam belum sinkron): batas bulan perkiraan
  BF_ESP32   = 4,   // kWh dari integrasi ESP32, bukan register meter: energi saat panel mati tidak terhitung
  BF_RESET   = 8,   // penghitung kWh sempat mundur/melompat di bulan ini (meter diganti / reset energi)
};

struct BillMonth {
  uint32_t ym;              // YYYYMM
  float    kwh;             // pemakaian sebulan
  float    rp;              // perkiraan biaya saat bulan ditutup (tarif saat itu)
  uint8_t  flags;
  uint8_t  pad[3];
};

struct BillStore {          // disimpan utuh di NVS (namespace "bill", kunci "st")
  uint8_t  ver = 1;
  uint8_t  src = 0;         // sumber kWh bulan berjalan: 0 = register meter, 1 = integrasi ESP32
  uint8_t  flags = 0;       // flag bulan berjalan
  uint8_t  nh = 0;          // jumlah bulan di riwayat
  uint32_t ym = 0;          // bulan berjalan (YYYYMM); 0 = belum mulai
  uint32_t t0 = 0;          // awal pencatatan bulan berjalan (waktu Unix)
  double   k0 = 0;          // pembacaan kWh pada awal pencatatan
  double   acc = 0;         // kWh yang sudah terkumpul sebelum penghitung mundur/melompat
  BillMonth hist[BILL_HIST_N];   // bulan-bulan sebelumnya, terbaru dulu
};

struct BillRam {            // tidak disimpan: pembacaan terakhir untuk menyaring bacaan rusak
  double   kLast = -1;
  uint32_t tLast = 0;
  double   kRej = -1;       // bacaan yang ditolak (loncatan tidak wajar)
  uint8_t  nRej = 0;
};

struct BillNow {            // ringkasan bulan berjalan untuk LCD / MQTT / web
  bool     ok = false;      // false = jam belum sinkron atau belum ada pembacaan kWh
  uint32_t ym = 0, t0 = 0, tFrom = 0, tTo = 0;
  float    kwh = 0;
  float    projKwh = -1;    // perkiraan sebulan penuh; < 0 = belum cukup data (perlu ≥ 24 jam)
  uint8_t  flags = 0, src = 0;
};

struct BillCost { float kwh, kwhBill, energi, pbjt, total; bool minApplied; };

BillStore   gBill;          // gBill dan gBillNow dibaca/ditulis dengan Lock (gMutex)
BillRam     gBillRam;
BillNow     gBillNow;
Preferences billPrefs;

static const char* BULAN[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};
static const char* BULAN_PANJANG[] = {"Januari", "Februari", "Maret", "April", "Mei", "Juni",
                                      "Juli", "Agustus", "September", "Oktober", "November", "Desember"};

// ---------------- kalender (waktu lokal WIB dari configTime) ----------------
uint32_t billYm(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  return (uint32_t)(tm.tm_year + 1900) * 100 + tm.tm_mon + 1;
}
uint32_t billYmNext(uint32_t ym) { return ym % 100 == 12 ? (ym / 100 + 1) * 100 + 1 : ym + 1; }
time_t billYmStart(uint32_t ym) {   // tanggal 1 pukul 00:00 waktu lokal
  struct tm tm = {};
  tm.tm_year = (int)(ym / 100) - 1900;
  tm.tm_mon = (int)(ym % 100) - 1;
  tm.tm_mday = 1;
  tm.tm_isdst = 0;
  return mktime(&tm);
}
// "Nov 2026" / "November 2026"
void billYmText(char* out, size_t n, uint32_t ym, bool panjang = false) {
  int m = (int)(ym % 100);
  if (m < 1 || m > 12) { strlcpy(out, "-", n); return; }
  snprintf(out, n, "%s %lu", panjang ? BULAN_PANJANG[m - 1] : BULAN[m - 1], (unsigned long)(ym / 100));
}

// Rupiah tanpa desimal dengan titik ribuan: 1234567.4 → "1.234.567"
void fmtRp(char* out, size_t n, float rp) {
  char d[16];
  snprintf(d, sizeof(d), "%lu", (unsigned long)lroundf(rp < 0 ? 0 : rp));
  size_t L = strlen(d), j = 0;
  for (size_t i = 0; i < L && j + 1 < n; i++) {
    if (i && (L - i) % 3 == 0 && j + 2 < n) out[j++] = '.';
    out[j++] = d[i];
  }
  out[j] = 0;
}

// Biaya untuk sejumlah kWh dengan pengaturan sekarang (P dibaca; panggil dengan Lock bila perlu konsisten).
// applyMin = terapkan rekening minimum pascabayar (hanya untuk bulan penuh / perkiraan sebulan).
BillCost billCost(float kwh, bool applyMin) {
  BillCost c;
  float minKwh = (applyMin && P.pasca) ? 40.0f * PLN_DAYA_VA / 1000.0f : 0.0f;
  c.kwh = kwh < 0 ? 0 : kwh;
  c.minApplied = c.kwh < minKwh;
  c.kwhBill = c.minApplied ? minKwh : c.kwh;
  c.energi = c.kwhBill * P.tarif_kwh;
  c.pbjt = c.energi * P.pbjt / 100.0f;
  c.total = c.energi + c.pbjt;
  return c;
}

// ---------------- logika inti (tanpa NVS/RTOS, diuji juga di laptop) ----------------
// now: waktu Unix; kMeter: register meter (≤ 0 = tidak terbaca); kEsp: integrasi ESP32;
// allowEsp: boleh memakai integrasi ESP32 bila register meter belum pernah terbaca.
// Mengisi out (ringkasan). Mengembalikan true bila st berubah (perlu disimpan ke NVS).
// Bila sebuah bulan ditutup, *closed diisi dan *didClose = true.
bool billStep(BillStore& st, BillRam& r, BillNow& out, time_t now, double kMeter, double kEsp, bool allowEsp,
              BillMonth* closed, bool* didClose) {
  *didClose = false;
  if (now < 1700000000) { out.ok = false; return false; }     // jam belum sinkron (NTP)
  uint32_t ym = billYm(now);
  bool changed = false;

  if (st.ym == 0) {                                            // pertama kali dipakai
    double k;
    uint8_t src;
    if (kMeter > 0) { k = kMeter; src = 0; }
    else if (allowEsp) { k = kEsp; src = 1; }
    else { out.ok = false; return false; }
    st.ym = ym; st.t0 = (uint32_t)now; st.k0 = k; st.acc = 0; st.src = src;
    st.flags = (now - billYmStart(ym) > 1800 ? BF_PARTIAL : 0) | (src ? BF_ESP32 : 0);
    r.kLast = k; r.tLast = (uint32_t)now; r.nRej = 0;
    changed = true;
  }

  double k;
  if (st.src == 0) {
    if (kMeter <= 0) return changed;                           // register belum terbaca: pakai ringkasan lama
    k = kMeter;
  } else {
    k = kEsp;
  }

  // Saring bacaan: loncatan naik tidak wajar (> 20 kW rata-rata) diabaikan, kecuali menetap 3 kali (meter diganti).
  double kRef = r.kLast >= 0 ? r.kLast : st.k0;
  uint32_t tRef = r.kLast >= 0 ? r.tLast : st.t0;
  double maxUp = 20.0 * ((now > (time_t)tRef ? now - tRef : 0) / 3600.0) + 0.05;
  if (k - kRef > maxUp) {
    if (r.nRej > 0 && fabs(k - r.kRej) < 0.5) r.nRej++;
    else r.nRej = 1;
    r.kRej = k;
    if (r.nRej < 3) return changed;
    st.acc += kRef - st.k0;                                    // loncatan menetap: mulai dasar baru
    st.k0 = k;
    st.flags |= BF_RESET;
    changed = true;
  } else if (k < kRef - 0.005) {                               // penghitung mundur (meter diganti / reset energi)
    st.acc += kRef - st.k0;
    st.k0 = k;
    st.flags |= BF_RESET;
    changed = true;
  }
  r.nRej = 0;

  if (ym != st.ym) {
    if (ym < st.ym) return changed;                            // jam mundur (NTP keliru): tunggu
    time_t mStart = billYmStart(ym);
    bool late = (now - mStart) > 1800 || billYmNext(st.ym) != ym;
    BillMonth m = {};
    m.ym = st.ym;
    m.kwh = (float)(st.acc + (k - st.k0));
    m.flags = st.flags | (late ? BF_LATE : 0);
    m.rp = billCost(m.kwh, !(m.flags & BF_PARTIAL)).total;
    for (int i = BILL_HIST_N - 1; i > 0; i--) st.hist[i] = st.hist[i - 1];
    st.hist[0] = m;
    if (st.nh < BILL_HIST_N) st.nh++;
    *closed = m;
    *didClose = true;
    if (st.src == 1 && kMeter > 0) { st.src = 0; k = kMeter; }   // bulan baru: utamakan register meter
    st.ym = ym; st.t0 = (uint32_t)now; st.k0 = k; st.acc = 0;
    st.flags = (late ? BF_LATE : 0) | (st.src ? BF_ESP32 : 0);
    changed = true;
  }
  r.kLast = k;
  r.tLast = (uint32_t)now;

  out.ok = true;
  out.ym = st.ym;
  out.t0 = st.t0;
  out.tFrom = (uint32_t)billYmStart(st.ym);
  out.tTo = (uint32_t)billYmStart(billYmNext(st.ym));
  out.kwh = (float)(st.acc + (k - st.k0));
  out.flags = st.flags;
  out.src = st.src;
  uint32_t tStart = st.t0 > out.tFrom ? st.t0 : out.tFrom;
  double el = (double)now - tStart;
  out.projKwh = el >= 86400 ? (float)(out.kwh / el * (out.tTo - out.tFrom)) : -1.0f;
  return changed;
}

// ---------------- NVS ----------------
void billLoad() {
  billPrefs.begin("bill", false);
  BillStore st;
  if (billPrefs.getBytesLength("st") == sizeof(BillStore)) {
    billPrefs.getBytes("st", &st, sizeof(st));
    if (st.ver == 1 && st.nh <= BILL_HIST_N) gBill = st;
  }
  billPrefs.end();
}

void billSave(const BillStore& st) {
  billPrefs.begin("bill", false);
  billPrefs.putBytes("st", &st, sizeof(st));
  billPrefs.end();
}

void billClear() {                                             // dipakai perintah Serial "tagihan reset"
  { Lock l; gBill = BillStore(); gBillRam = BillRam(); gBillNow = BillNow(); }
  billPrefs.begin("bill", false);
  billPrefs.clear();
  billPrefs.end();
}

// ---------------- task tagihan (core 0, prioritas 1, tiap 10 s) ----------------
void taskBill(void*) {
  uint32_t boot = millis();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    time_t now = time(nullptr);
    double km, ke;
    bool mOk;
    uint32_t seq;
    { Lock l; km = G.last.kwh_meter; ke = G.e.kwh; mOk = G.meterOk; seq = G.seq; }
    if (seq == 0) continue;                                    // belum ada sampel meter
    if (!mOk || !isfinite(km) || km <= 0) km = 0;
    bool allowEsp = millis() - boot > 180000;                  // 3 menit pertama: tunggu register meter
    BillMonth closed;
    bool didClose, save;
    BillStore copy;
    {
      Lock l;
      save = billStep(gBill, gBillRam, gBillNow, now, km, ke, allowEsp, &closed, &didClose);
      if (save) copy = gBill;
      if (didClose) G.statusDirty = true;
    }
    if (save) billSave(copy);
    if (didClose) {
      char ymT[16], rp[16], d[96];
      billYmText(ymT, sizeof(ymT), closed.ym);
      fmtRp(rp, sizeof(rp), closed.rp);
      snprintf(d, sizeof(d), "%s: %.2f kWh, Rp%s%s", ymT, closed.kwh, rp,
               closed.flags & (BF_PARTIAL | BF_LATE | BF_ESP32 | BF_RESET) ? " (tidak lengkap/perkiraan)" : "");
      postEvent("BILL_MONTH", d);
    }
  }
}
