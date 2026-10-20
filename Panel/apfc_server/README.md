# APFC Panel — Backend & Dashboard Web

Backend Flask + dashboard untuk panel APFC (TDB-APFC-01 Subbab 7.6.2). Tim Aku Anak Sehat.

```
ESP32 ──MQTT──► Mosquitto ──► app.py ──► SQLite + CSV harian (folder data/)
                                  │
browser (laptop/HP) ◄──SSE────────┘   tombol di dashboard ──► POST /api/cmd ──► MQTT cmd ──► ESP32 ──► cmd/ack
```

```
apfc_server/
├─ app.py              backend: MQTT → SQLite/CSV → SSE, API perintah
├─ energy.py           perhitungan energi & penalti dengan/tanpa APFC (Subbab 7.5)
├─ templates/index.html  kerangka halaman (5 tab)
├─ static/style.css    warna, tipografi, tata letak  ← Aina: ubah token warna di :root
├─ static/app.js       logika tampilan, grafik, perintah
├─ static/chart.umd.js Chart.js 4.4.4 (lokal, dashboard jalan tanpa internet)
├─ tools/simulator.py  simulator panel (uji dashboard tanpa ESP32)
├─ requirements.txt
├─ jalankan_dashboard.bat · jalankan_simulator.bat
└─ data/               dibuat otomatis: apfc.db, YYYY-MM-DD_telemetry.csv, YYYY-MM-DD_event.csv
```

## 1. Persiapan (sekali saja)

1. Install **Python 3.10+** dari python.org. Saat install, centang **"Add python.exe to PATH"**.
2. Buka Command Prompt di folder `apfc_server`, lalu:
   ```
   pip install -r requirements.txt
   ```
3. Supaya dashboard bisa dibuka dari HP, buka port 5000 (CMD **admin**, sekali saja):
   ```
   netsh advfirewall firewall add rule name="APFC Dashboard 5000" dir=in action=allow protocol=TCP localport=5000
   ```

## 2. Menjalankan (setiap demo)

1. **Broker** (jendela 1, CMD admin):
   ```
   cd "C:\Program Files (x86)\Mosquitto"
   mosquitto -c C:\mqtt\apfc.conf -v
   ```
2. **Panel** dinyalakan → layar 3 LCD harus `MQTT OK`.
3. **Dashboard** (jendela 2): klik dua kali `jalankan_dashboard.bat`, atau:
   ```
   python app.py
   ```
   Muncul alamat yang bisa dibuka:
   ```
   Buka di laptop : http://localhost:5000
   Buka dari HP   : http://192.168.137.1:5000
   ```
4. Buka alamat tersebut di browser. Header harus menunjukkan **● Online** dan jam data yang terus berjalan.

Pilihan lain: `python app.py --help` (mis. `--mqtt-host`, `--port 8080`, `--tariff 1114.74`, `--scale 1000`).

## 3. Isi dashboard

| Tab | Isi |
|---|---|
| **Ringkasan** | Gauge PF (induktif kiri, kapasitif kanan, zona target 0,95), V/I/f/P/Q/S, batang kompensasi (Q beban → Q kapasitor → Q sisa), status step C1–C3 (2/4/6 µF) + hitung mundur jeda, status beban K1, info panel, grafik PF 10 menit |
| **Tren** | Grafik PF dan P/Q/Q-beban (10 menit, 1 jam, hari ini), garis target, tanda saat step berubah, tabel data |
| **Energi & Penalti** | Penalti tanpa vs dengan APFC, penghematan, rasio kVArh/kWh vs batas 0,62, rincian energi; periode sesi/hari ini/semua; skala ×1 / ×1000 |
| **Kontrol** | Mode AUTO/MANUAL, step ON/OFF (hanya MANUAL), tombol **Matikan / Nyalakan beban** (relay K1, kontak NC), parameter (pf_target, c1–c3, dll.), reset energi; riwayat perintah dengan hasil ack (diterima/ditolak + alasan) |
| **Log** | Kejadian (step, mode, fail-safe, perintah ditolak, online/offline) dengan filter; unduh CSV harian |

Status khusus: banner abu-abu bila panel offline, kuning bila FAIL-SAFE, merah bila broker/server terputus; chip "Data lama" bila tidak ada data > 6 s. Mode gelap mengikuti pengaturan Windows/HP.

**Energi "Sesi ini"** dihitung sejak `app.py` dijalankan — jalankan ulang `app.py` sesaat sebelum demo untuk memulai hitungan bersih (data lama tetap tersimpan di `data/`).

## 4. Simulator (tanpa panel)

Untuk latihan atau pengembangan tampilan (mis. oleh Aina) tanpa ESP32:
```
python tools\simulator.py                 # C1/C2/C3 = 2 / 4 / 6,36 µF (susunan panel sekarang)
python tools\simulator.py --c 6.36,6.36,6.36
```
Beban di MCB berganti otomatis tiap 40 s (kosong → 1 TL → 2 TL → 1 TL). Beban K1 (kontak NC, bawaan menyala, setara 1 TL) bisa dimatikan dari tab Kontrol. Algoritma pemilihan kombinasi, jeda sambung ulang, ack, dan penolakan perintah meniru firmware v1.2.0.
**Jangan jalankan simulator bersamaan dengan panel asli** (topiknya sama).

## 5. Masalah umum

| Gejala | Solusi |
|---|---|
| Banner merah "Backend tidak terhubung ke broker" | Mosquitto belum jalan (jendela 1). |
| Header "Menunggu panel…" / "Panel offline" | Panel belum terhubung MQTT — cek layar 3 LCD dan Serial Monitor. |
| HP tidak bisa membuka dashboard | HP harus tersambung ke hotspot/Wi-Fi yang sama; jalankan perintah firewall port 5000. |
| Tombol kontrol abu-abu | Panel offline, perintah sebelumnya masih menunggu, atau (untuk step) mode masih AUTO. |
| "tidak ada jawaban dari panel dalam 3 s" | Panel tidak menerima perintah: cek koneksi MQTT panel. |
