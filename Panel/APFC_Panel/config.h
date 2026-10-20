// =====================================================================
//  APFC Panel — konfigurasi (UBAH FILE INI SAJA untuk menyesuaikan)
//  Tim Aku Anak Sehat · DeepTech for Smart-X Hackathon 2026
// =====================================================================
#pragma once

#define FW_VERSION      "1.2.0"
#define PANEL_ID        "panel01"          // prefiks topik MQTT: apfc/panel01/...

// ---------------- Wi-Fi & MQTT ----------------
// Kosongkan WIFI_SSID ("") untuk menjalankan panel tanpa Wi-Fi (LCD + microSD + Serial tetap jalan).
#define WIFI_SSID       ""
#define WIFI_PASS       ""
#define MQTT_HOST       "192.168.1.10"     // IP laptop yang menjalankan Mosquitto
#define MQTT_PORT       1883
#define MQTT_USER       ""                 // isi bila broker memakai password
#define MQTT_PASS       ""
#define TZ_OFFSET_S     (7 * 3600)         // WIB (UTC+7)

// ---------------- Pemetaan pin (Tabel 4-1, Gbr M-1) ----------------
#define PIN_RS485_RX    16   // ← LV3/HV3 ← TXD modul MAX13487E (label modul: TXD = keluaran data)
#define PIN_RS485_TX    17   // → LV4/HV4 → RXD modul MAX13487E
#define PIN_I2C_SDA     21   // → LV1 level shifter → SDA LCD
#define PIN_I2C_SCL     22   // → LV2 level shifter → SCL LCD
#define PIN_SD_CS       5    // microSD: SCK 18, MISO 19, MOSI 23 (SPI bawaan)
#define PIN_SSR1        25   // SSR CH1 → step C1 (2 µF)
#define PIN_SSR2        26   // SSR CH2 → step C2 (4 µF)
#define PIN_SSR3        27   // SSR CH3 → step C3 (6 µF)
#define PIN_RELAY1      32   // relay IN1 → K1 → beban demo terkendali (lewat kontak NC)
// IO33 (IN2) dan relay K2–K4 tidak dipakai.
#define LOAD_K1_NC      1    // 1 = beban di kontak NC: relay tidak bekerja → beban MENYALA (juga saat ESP32 mati)
                             // 0 = beban di kontak NO: relay bekerja → beban menyala
// IO34 bebas (cadangan input). E-stop dipasang sebelum RCBO dan memutus seluruh panel termasuk ESP32,
// sehingga firmware tidak perlu membaca tombol darurat.
#define PIN_LED         2    // LED biru bawaan board

#define SSR_ON_LEVEL    HIGH // modul SSR high-level trigger
#define RELAY_ON_LEVEL  LOW  // modul relay aktif-LOW

// ---------------- Meter CHINT DDSU666 ----------------
#define METER_ID        1
#define METER_BAUD      9600
#define METER_TIMEOUT_MS 300
#define SAMPLE_PERIOD_MS 2000

// ---------------- Bank kapasitor (µF efektif, hasil kalibrasi) ----------------
// ESP32 memilih sendiri kombinasi terbaik: 2 / 4 / 6 / 2+4 / 2+6 / 4+6 / 2+4+6 µF (bertahap ±2 µF).
// Bila nilai di sini diubah lalu di-upload, nilai c1–c3 lama di NVS otomatis diganti nilai baru.
// Kalibrasi halus tanpa upload: "set c1 2.05" dst. di Serial Monitor / dashboard.
#define C_STEP1_UF      2.00f   // SSR1: CBB61 2 µF
#define C_STEP2_UF      4.00f   // SSR2: 4 µF
#define C_STEP3_UF      6.36f   // SSR3: CBB60 6 µF (efektif terukur 6,36–6,41 µF)

// ---------------- Parameter kendali bawaan (bisa diubah lewat perintah "set") ----------------
#define DEF_PF_TARGET   0.95f   // target cos φ
#define DEF_HYST        0.20f   // perbaikan minimal untuk berpindah = HYST × Q step terkecil
#define DEF_LEAD_W      1.2f    // bobot hukuman kondisi kapasitif (leading) dibanding induktif
#define DEF_T_RECONNECT 10      // detik, jeda minimum sebelum step yang sama disambung lagi (≥ 10)
#define DEF_N_AVG       5       // jumlah sampel rata-rata (5 × 2 s = 10 s)
#define DEF_N_SKIP      2       // sampel diabaikan setelah switching
#define DEF_P_MIN       10.0f   // W; di bawah ini semua step dilepas (tanpa beban)
#define DEF_TARIFF      1114.74f// Rp/kVArh (asumsi, Bab 3)
#define DEF_SCALE       1000.0f // skala tampilan setara industri
#define N_FAIL          3       // gagal baca berturut-turut → FAIL-SAFE
#define N_RECOVER       5       // bacaan valid berturut-turut → pulih
#define MAX_ON_PER_DECISION 1   // maksimal step yang disambung per keputusan

// ---------------- Tampilan, log, penyimpanan ----------------
#define LCD_ADDR        0x27    // coba 0x3F bila LCD tidak tampil (cek dengan perintah "i2c")
#define LCD_PAGE1_MS    60000   // layar 1: V, I, P, Q, PF, f, step (60 s)
#define LCD_PAGE2_MS    10000   // layar 2: energi, rasio kVArh/kWh, penalti (10 s)
#define LCD_PAGE3_MS    10000   // layar 3: Wi-Fi, MQTT, meter, uptime (10 s)
#define LOG_PERIOD_MS   10000   // tulis microSD tiap 10 s
#define NVS_SAVE_MS     60000   // simpan energi ke NVS tiap 1 menit (panel bisa mati mendadak lewat E-stop)
#define STATUS_PERIOD_MS 60000  // publikasi status retained tiap 60 s
#define SERIAL_BAUD     115200
