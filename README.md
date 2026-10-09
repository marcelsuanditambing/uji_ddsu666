# Uji DDSU666 — Monitor & Logger CSV

Membaca meter **CHINT DDSU666** (Modbus RTU via USB–RS485), menampilkan dashboard lokal,
dan mencatat setiap sampel ke CSV. Bagian dari prototipe APFC (DeepTech Hackathon 2026).

## Instalasi
```
pip install -r requirements.txt
```

## Setting meter (sudah terverifikasi)
Protokol `Modbus` · alamat `001` · `bAUd-3` (9600) · `n-8n1`.
Wiring: terminal 1 = L masuk, 3 = L keluar, 2/4 = N, 24 = A, 25 = B.

## Pemakaian
```
python test_ddsu666.py              # uji koneksi sekali baca (COM5)
python monitor_ddsu666.py           # dashboard + logger CSV  → http://127.0.0.1:5000
python monitor_ddsu666.py --port COM7 --interval 2
python monitor_ddsu666.py --simulate          # tanpa meter, untuk uji tampilan
python monitor_ddsu666.py --reset-totals      # nolkan akumulasi kWh/kVArh
```
Hentikan dengan **Ctrl+C** agar total tersimpan rapi (tetap tersimpan otomatis tiap 30 detik).

## Data
`data/ddsu666_YYYY-MM-DD.csv` — satu file per hari, satu baris per sampel:

| Kolom | Keterangan |
|---|---|
| timestamp | waktu lokal ISO-8601 |
| status | `OK` atau `ERR` (baris ERR = pembacaan gagal, nilai kosong) |
| voltage_v, current_a | tegangan & arus RMS |
| active_power_w, reactive_power_var | P dan Q dari meter (Q + induktif / − kapasitif) |
| apparent_power_va | S = V × I (termasuk efek harmonik) |
| power_factor, frequency_hz | dari meter |
| energy_kwh_meter | register energi aktif meter (4000H) |
| kwh_log, kvarh_ind_log, kvarh_cap_log | akumulasi ∫P dt dan ∫Q dt (trapesium) — DDSU666 tidak punya register kVArh |
| read_ms, error | waktu baca Modbus & pesan error |

Integrasi dilewati bila ada celah > 10 detik (misal meter tidak menjawab), sehingga kVArh tidak
"menebak" data yang hilang. Total akumulasi disimpan di `data/state.json`.

Buka CSV di Excel lewat **Data → From Text/CSV** (pemisah koma, desimal titik).
