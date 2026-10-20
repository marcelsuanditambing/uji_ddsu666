# APFC Panel — Firmware ESP32 v1.2.1

Firmware panel IoT Automatic Power Factor Correction (Fase 1) — Tim Aku Anak Sehat.
Spesifikasi lengkap: *Technical Design Book* TDB-APFC-01, Bab 7.

```
APFC_Panel/
├─ APFC_Panel.ino   setup(), task FreeRTOS meter/kendali, loop() konsol
├─ config.h         ← SATU-SATUNYA file yang perlu diubah (Wi-Fi, broker, pin, kapasitor, parameter)
├─ state.h          struktur data bersama, antrian, mutex
├─ outputs.h        SSR dan relay
├─ meter.h          Modbus RTU DDSU666 (FC03, float32)
├─ control.h        mesin keadaan, algoritma koreksi PF, energi & penalti, perintah
├─ storage.h        NVS (parameter + kWh/kVArh tersimpan saat mati listrik)
├─ comm.h           Wi-Fi, NTP, MQTT (topik apfc/panel01/...)
├─ ui.h             LCD 20×4 + LED
├─ logger.h         microSD (/log/YYYYMMDD.csv, /log/event.csv)
└─ console.h        perintah lewat Serial Monitor
platformio.ini      (opsional) bila memakai PlatformIO/VS Code
mosquitto.conf      contoh konfigurasi broker di laptop
```

Kode sudah dikompilasi tanpa error dan tanpa peringatan pada paket ESP32 **3.0.7** (flash 79 %) dan **2.0.17** (flash 36 %).

**Perubahan v1.2.1**
- Pin relay K1 (IO32) kini **open-drain** (`RELAY_OPEN_DRAIN 1`): saat tidak diperintah, pin dilepas sehingga tidak ada arus ke optocoupler → relay pasti diam dan beban K1 (kontak NC) MENYALA, walaupun VCC modul relay 5 V. Relay hanya bekerja setelah "Matikan beban" / `beban off`.
- `status` menampilkan keadaan relay dan level pin IO32 untuk diagnosa.

**Perubahan v1.2.0**
- Bank kapasitor baru: **SSR1 = 2 µF, SSR2 = 4 µF, SSR3 = 6 µF** (`C_STEP1_UF…C_STEP3_UF` = 2.00 / 4.00 / 6.36). ESP32 memilih kombinasi 2 / 4 / 6 / 2+4 / 2+6 / 4+6 / 2+4+6 µF sesuai kebutuhan Q.
- Bila nilai kapasitor di `config.h` diubah lalu di-upload, nilai c1–c3 lama di NVS **otomatis diganti** (tidak perlu `factory`).
- Relay **K1 memakai kontak NC**: relay tidak bekerja = beban MENYALA (juga saat ESP32 mati/reset). Status `sockets[0]` di MQTT kini berarti **beban menyala (1) / padam (0)**. Perintah Serial baru: `beban on|off`. Event baru `LOAD`.
- Relay K2–K4 tidak dipakai (IO33 bebas). `NUM_SOCKETS` = 1.

**Perubahan v1.1.1**
- LCD: layar 1 (V, I, P, Q, PF, f) tampil 60 s, layar 2 (energi) 10 s, layar 3 (status) 10 s — atur di `LCD_PAGE1_MS…LCD_PAGE3_MS`.
- Layar 1: label I, Q, f sejajar di kolom 11; arus 3 desimal. Layar 2: "Rasio" diganti "kVArh/kWh".
- Komentar pin RS485 disesuaikan: HV3 ↔ TXD modul MAX13487E, HV4 ↔ RXD modul.

**Perubahan v1.1.0**
- Fungsi emergency dihapus dari firmware: tombol E-stop dipasang **sebelum RCBO** sehingga memutus seluruh panel termasuk ESP32. IO34 kini bebas.
- Saat daya kembali, panel mulai dari `INIT` dengan semua step OFF; step pertama baru boleh masuk setelah jeda 10 s (praktisnya ±15–20 s).
- Energi disimpan ke NVS tiap 1 menit (sebelumnya 5 menit) karena panel bisa padam mendadak.
- Bawaan bank kapasitor 3 × 6 µF. Algoritma tetap memilih sendiri kombinasi terbaik (4 / 6 / 4+6 / 6+6 / 4+6+6 µF) begitu kapasitor 4 µF dipasang.

---

## 1. Persiapan Arduino IDE (sekali saja)

1. Install **Arduino IDE 2.x**.
2. *File → Preferences → Additional boards manager URLs*, isi:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. *Tools → Board → Boards Manager* → cari **esp32** (by Espressif Systems) → Install (versi 3.x atau 2.0.17).
4. *Tools → Manage Libraries* → install:
   - **PubSubClient** — Nick O'Leary
   - **ArduinoJson** — Benoit Blanchon (versi 7.x)
   - **LiquidCrystal I2C** — Frank de Brabander
   (Peringatan "may be incompatible with esp32" untuk LiquidCrystal I2C boleh diabaikan.)
5. Driver USB: board ESP32 memakai chip **CP2102** → bila port tidak muncul, install driver *CP210x* dari Silicon Labs.

## 2. Atur `config.h`

| Item | Isi |
|---|---|
| `WIFI_SSID`, `WIFI_PASS` | Wi-Fi yang sama dengan laptop. Biarkan `""` untuk uji tanpa Wi-Fi. |
| `MQTT_HOST` | IP laptop (Windows: `ipconfig` → IPv4 Address) |
| `C_STEP1_UF … C_STEP3_UF` | Kapasitansi efektif tiap step: 2.00 / 4.00 / 6.36 µF (SSR1/SSR2/SSR3). Kalibrasi halus tanpa upload: `set c1 2.05` dst. |
| `LOAD_K1_NC` | 1 = beban di kontak NC relay K1 (bawaan), 0 = kontak NO. |
| `LCD_ADDR` | `0x27` (coba `0x3F` bila LCD kosong). |

Pemetaan pin (sesuai Gbr M-1): RS485 RX/TX = IO16/IO17 (lewat level shifter LV3/LV4) · LCD SDA/SCL = IO21/IO22 (LV1/LV2) · SSR CH1–CH3 = **IO25, IO26, IO27** · relay IN1 (K1, kontak NC → beban demo) = IO32 · IO33 bebas · microSD CS = IO5 · IO34 cadangan.

## 3. Upload

1. Buka `APFC_Panel/APFC_Panel.ino` (nama folder harus sama dengan nama file .ino).
2. *Tools → Board*: **ESP32 Dev Module** · *Upload Speed* 921600 (turunkan ke 115200 bila gagal) · *Port*: COM board.
3. **PSU 5 V panel harus MENYALA** saat ESP32 terpasang di shield dan USB tersambung. Bila PSU mati, USB laptop
   ikut memberi daya ke semua modul (LCD, relay, SSR, microSD, RS485) → tegangan drop → ESP32 reset terus,
   upload gagal ("Invalid head of packet 0x00") dan Serial Monitor hanya menampilkan kotak-kotak.
   Klik **Upload**. Bila macet di "Connecting…", tekan dan tahan tombol **BOOT** di board sampai upload dimulai.
4. Setelah selesai, buka **Serial Monitor** 115200 baud, pilih akhiran **Newline**, tekan tombol **EN** (reset).

## 4. Urutan commissioning (wajib, dengan meter tersambung)

Gunakan Serial Monitor. Ketik `help` untuk semua perintah.

1. **Boot** — muncul `APFC Panel v1.1.1` dan parameter. Panel mulai di `INIT`; setelah 3 bacaan valid → `AUTO`.
   - `meter tidak merespons` → ketik `raw on`. Bila 0 byte diterima: tukar kabel **HV3 ↔ HV4** (TXD/RXD), lalu A ↔ B. `raw off` setelah selesai.
2. **LCD** — bila kosong: `i2c` harus menampilkan `0x27` (atau `0x3F` → ubah `LCD_ADDR`). Putar trimpot biru di belakang LCD untuk kontras.
3. **Uji tiap step (mode manual)**, beban lampu menyala:
   ```
   mode manual
   step 1 on      → Q turun ±30 var (2 µF), pilot H1 & LED SSR CH1 menyala
   step 1 off
   step 2 on / off → Q turun ±60 var (4 µF)
   step 3 on / off → Q turun ±95 var (6 µF)
   ```
   Menyalakan step yang sama < 10 s setelah dilepas akan **ditolak** (jeda pembuangan muatan) — itu normal.
4. **Kalibrasi kapasitansi** (opsional, lebih akurat): dari penurunan Q tiap step hitung `C = ΔQ / (2π·50·V²)`, lalu `set c1 6.30` (dan c2, c3). Tersimpan permanen.
5. **Uji reset ESP32** — saat `step 1 on`, tekan tombol **EN**: SSR harus langsung OFF (LED SSR padam). Bila tidak, tambah resistor 10 kΩ dari IN SSR ke GND.
6. **Uji tombol darurat** — tekan E-stop: seluruh panel padam (termasuk ESP32 dan LCD). Putar untuk melepas: ESP32 menyala ulang, LCD `Memulai...`, semua step OFF, lalu kembali ke mode terakhir.
7. **Mode otomatis**: `mode auto`. 1 TL (Q ±70 var) → ESP32 memasukkan **C2 (4 µF)**; 2 TL (Q ±145 var) → menjadi **C3 + C1 (6 + 2 µF)**; saat beban turun lagi, ESP32 **menukar** kombinasi (lepas 6+2, masukkan 4). Satu step baru per keputusan (±15 s). Pantau di LCD dan Serial.
8. **Beban K1 (kontak NC)**: `beban off` → relay klik, beban padam; `beban on` → beban menyala lagi. Setelah reset/padam listrik, beban K1 selalu menyala.

## 5. Cara kerja algoritma (ringkas)

- Baca meter tiap 2 s; sampel transien (√(P²+Q²) ≠ U·I) dan 2 sampel setelah switching diabaikan.
- Keputusan memakai rata-rata 5 sampel (10 s); bila beban melompat, rata-rata dimulai ulang.
- Dari 8 kombinasi step, dipilih yang membuat Q sisa paling kecil (kondisi kapasitif diberi bobot 1,2×). Panel **diam** bila |Q| ≤ P·tan(arccos 0,95), dan hanya pindah bila perbaikannya > 0,2 × Q step terkecil (histeresis, anti-hunting). Maksimal 1 step disambung per keputusan; jeda sambung ulang 10 s.
- P < 10 W → semua step dilepas. Meter gagal 3× → **FAIL-SAFE** (semua step OFF), pulih setelah 5 bacaan valid.
- kWh, kVArh induktif/kapasitif, dan kVArh "tanpa APFC" diintegrasi lalu disimpan ke NVS tiap 1 menit.

Perkiraan hasil dengan data ukur 18 Okt (225 V; per step ≈ 32 / 64 / 101 var):

| Beban | PF tanpa APFC | Kombinasi dipilih | Q sisa | PF dengan APFC |
|---|---|---|---|---|
| 1 TL | 0,40 | 4 µF | +9 var | 0,96 |
| 2 TL | 0,41 | 2+6,36 µF | +11 var | 0,99 |

Dari 0 ke 2 TL ESP32 memasukkan 6 µF dulu, lalu 2 µF (satu step baru per keputusan). Dari 2 TL turun ke 1 TL, ESP32 langsung menukar 6+2 µF menjadi 4 µF.

## 6. MQTT (untuk dashboard)

1. Install **Mosquitto** di laptop. Salin `mosquitto.conf` dari folder ini, lalu jalankan:
   `mosquitto -c mosquitto.conf -v`
   (Windows: izinkan port 1883 di Firewall.)
2. Pantau data: `mosquitto_sub -h localhost -t "apfc/panel01/#" -v`
3. Kirim perintah, contoh:
   ```
   mosquitto_pub -h localhost -t apfc/panel01/cmd -m "{\"id\":\"1\",\"cmd\":\"set_mode\",\"mode\":\"MANUAL\"}"
   mosquitto_pub -h localhost -t apfc/panel01/cmd -m "{\"id\":\"2\",\"cmd\":\"set_step\",\"step\":1,\"on\":1}"
   mosquitto_pub -h localhost -t apfc/panel01/cmd -m "{\"id\":\"3\",\"cmd\":\"set_param\",\"key\":\"c3\",\"value\":4.0}"
   ```
   Jawaban muncul di `apfc/panel01/cmd/ack`.

| Topik | Isi |
|---|---|
| `apfc/panel01/telemetry` | tiap 2 s: ts, seq, v, i, p, q, s, pf, f, q_load, q_need, kwh, kwh_m, kvarh_ind, kvarh_cap, kvarh_load, steps[3], sockets[1] (1 = beban K1 menyala), mode, state, rssi |
| `apfc/panel01/status` (retained) | state, mode, steps, sockets, meter_ok, fw, uptime, ip, c_uf, pf_target |
| `apfc/panel01/event` | STEP_ON, STEP_OFF, MODE, LOAD, FAILSAFE, RECOVER, CMD_REJECT |
| `apfc/panel01/cmd` → `cmd/ack` | set_mode, set_step, set_socket {socket:1, on:1 = nyalakan beban / 0 = matikan}, set_param {key,value}, energy_reset |
| `apfc/panel01/online` (retained, LWT) | "1" / "0" |

## 7. Masalah umum

| Gejala | Penyebab / solusi |
|---|---|
| `FAILSAFE` terus | Kabel RS485: tukar HV3↔HV4, lalu A↔B; cek meter: alamat 1, 9600 8N1 |
| ESP32 restart saat relay/Wi-Fi aktif | Tambah elco 1000 µF di 5V–GND shield; cek tegangan PSU 5,0–5,2 V |
| LCD hanya kotak-kotak | Atur kontras (trimpot), cek alamat dengan `i2c` |
| `[SD] microSD tidak terdeteksi` | VCC modul ke **5 V**, kartu FAT32, kabel SPI pendek |
| Step tidak mau ON di manual | Tunggu jeda 10 s (juga berlaku 10 s pertama setelah panel menyala), atau keadaan bukan MANUAL (lihat `status`) |
| Nilai `config.h` tidak berubah setelah upload | Parameter yang pernah diubah dengan `set` tersimpan di NVS dan menimpa `config.h`. Ketik `params` untuk melihat, `factory` untuk kembali ke `config.h`. |
