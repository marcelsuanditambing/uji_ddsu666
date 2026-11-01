# APFC Panel — Backend & Dashboard Web

Backend Flask + dashboard untuk panel APFC (TDB-APFC-01 Subbab 7.6.2). Tim Aku Anak Sehat.

```
ESP32 ──MQTT──► Mosquitto ──► app.py ──► SQLite + CSV harian (folder data/)
                                  │
browser (laptop/HP) ◄──SSE────────┘   tombol di dashboard ──► POST /api/cmd ──► MQTT cmd ──► ESP32 ──► cmd/ack
tab Log ──► /api/sd/... ──► app.py ──HTTP──► http://<IP panel>/sd/...   (log microSD, firmware ≥ 1.3.0)
tab Tagihan ► /api/bill ──► app.py ──HTTP──► http://<IP panel>/bill      (tagihan bulanan, firmware ≥ 1.4.0)
```

```
apfc_server/
├─ app.py              backend: MQTT → SQLite/CSV → SSE, API perintah
├─ energy.py           perhitungan energi & penalti dengan/tanpa APFC (Subbab 7.5)
├─ templates/index.html  kerangka halaman (6 tab)
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
Alamat web panel untuk log microSD diambil otomatis dari status panel (IP + port). Bila perlu dipaksa: `--panel-http 192.168.137.221`.

## 3. Isi dashboard

| Tab | Isi |
|---|---|
| **Ringkasan** | Gauge PF (induktif kiri, kapasitif kanan, zona target 0,95), V/I/f/P/Q/S, batang kompensasi (Q beban → Q kapasitor → Q sisa), status step C1–C3 (2/4/6 µF) + hitung mundur jeda, status beban K1, info panel, grafik PF 10 menit |
| **Tren** | Grafik PF dan P/Q/Q-beban (10 menit, 1 jam, hari ini), garis target, tanda saat step berubah, tabel data |
| **Tagihan PLN** | kWh bulan berjalan (tanggal 1 s.d. akhir bulan), biaya sampai sekarang (energi + PBJT), perkiraan tagihan sebulan, riwayat 12 bulan, dan pengaturan tarif/PBJT/prabayar-pascabayar yang disimpan ke panel. Dihitung panel dari register kWh meter, jadi tetap lengkap walau laptop mati. |
| **Energi & Penalti** | Penalti tanpa vs dengan APFC, penghematan, rasio kVArh/kWh vs batas 0,62, rincian energi; periode sesi/hari ini/semua; skala ×1 / ×1000 |
| **Kontrol** | Mode AUTO/MANUAL, step ON/OFF (hanya MANUAL), tombol **Matikan / Nyalakan beban** (relay K1, kontak NC), parameter (pf_target, c1–c3, dll.), reset energi; riwayat perintah dengan hasil ack (diterima/ditolak + alasan) |
| **Log** | Kejadian (step, mode, fail-safe, perintah ditolak, online/offline, microSD) dengan filter; **microSD di panel**: kapasitas kartu, daftar file harian, unduh dan hapus (file hari ini dikunci); CSV yang dicatat laptop |

Status khusus: banner abu-abu bila panel offline, kuning bila FAIL-SAFE, merah bila broker/server terputus; chip "Data lama" bila tidak ada data > 6 s. Mode gelap mengikuti pengaturan Windows/HP.

**Energi "Sesi ini"** dihitung sejak `app.py` dijalankan — jalankan ulang `app.py` sesaat sebelum demo untuk memulai hitungan bersih (data lama tetap tersimpan di `data/`).

## 4. Simulator (tanpa panel)

Untuk latihan atau pengembangan tampilan (mis. oleh Aina) tanpa ESP32:
```
python tools\simulator.py                 # C1/C2/C3 = 2 / 4 / 6,36 µF (susunan panel sekarang)
python tools\simulator.py --c 6.36,6.36,6.36
```
Beban di MCB berganti otomatis tiap 40 s (kosong → 1 TL → 2 TL → 1 TL). Beban K1 (kontak NC, bawaan menyala, setara 1 TL) bisa dimatikan dari tab Kontrol. Algoritma pemilihan kombinasi, jeda sambung ulang, ack, dan penolakan perintah meniru firmware v1.4.1 (termasuk batas Q kapasitif `cap_lim`).
Simulator juga meniru **microSD panel** (firmware 1.3.0) di `http://127.0.0.1:8081/` dengan contoh file 3 hari terakhir, sehingga unduh/hapus di tab Log bisa dicoba tanpa panel (`--sd-port 0` untuk mematikan). Di port yang sama simulator meniru `/bill` (firmware 1.4.0): rumah ±7,2 kWh/hari dengan riwayat 3 bulan contoh.
**Jangan jalankan simulator bersamaan dengan panel asli** (topiknya sama).

## 5. Masalah umum

| Gejala | Solusi |
|---|---|
| Banner merah "Backend tidak terhubung ke broker" | Mosquitto belum jalan (jendela 1). |
| Header "Menunggu panel…" / "Panel offline" | Panel belum terhubung MQTT — cek layar 3 LCD dan Serial Monitor. |
| HP tidak bisa membuka dashboard | HP harus tersambung ke hotspot/Wi-Fi yang sama; jalankan perintah firewall port 5000. |
| Tombol kontrol abu-abu | Panel offline, perintah sebelumnya masih menunggu, atau (untuk step) mode masih AUTO. |
| "tidak ada jawaban dari panel dalam 3 s" | Panel tidak menerima perintah: cek koneksi MQTT panel. |
| Tab Log: "firmware panel ... belum punya fitur unduh microSD" | Upload firmware v1.3.0 ke panel. |
| Tab Tagihan: "belum punya fitur tagihan bulanan" | Upload firmware v1.4.0 ke panel. |
| Tab Tagihan: riwayat tidak tersedia, angka bulan ini tetap ada | Web panel tidak terjangkau; ringkasan bulan ini diambil dari status MQTT. Cek jaringan seperti pada tab Log. |
| Tab Log: "panel tidak dapat dihubungi di http://…" | Laptop dan panel harus satu jaringan (hotspot laptop); cek IP di layar 3 LCD, coba buka alamat itu di browser laptop. |
| Tab Log: "microSD tidak terpasang di panel" | Pasang kartu FAT32; panel memeriksa lagi tiap 60 s (Serial: `sd`). |
