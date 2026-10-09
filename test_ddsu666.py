"""
Uji cepat koneksi CHINT DDSU666 (Modbus RTU).
Membaca register 0x2000-0x200F dan 0x4000, lalu menampilkan nilai dalam satuan asli.
Cocokkan hasilnya dengan LCD meter.

  python test_ddsu666.py            # COM5
  python test_ddsu666.py COM7       # port lain
"""
import struct
import sys

import minimalmodbus
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
SLAVE_ID = 1
BAUDRATE = 9600

meter = minimalmodbus.Instrument(PORT, SLAVE_ID, mode=minimalmodbus.MODE_RTU)
meter.serial.baudrate = BAUDRATE
meter.serial.bytesize = 8
meter.serial.parity = serial.PARITY_NONE
meter.serial.stopbits = 1
meter.serial.timeout = 2
meter.clear_buffers_before_each_transaction = True


def to_float(hi, lo):
    return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]


print("================================")
print("     CHINT DDSU666 TEST")
print("================================")
print(f"Port {PORT} | Slave ID {SLAVE_ID} | {BAUDRATE} bps 8N1")

try:
    r = meter.read_registers(0x2000, 16, functioncode=3)
    e = meter.read_registers(0x4000, 2, functioncode=3)
except Exception as error:
    print("\nBELUM BERHASIL MEMBACA METER")
    print(f"Jenis error: {type(error).__name__}")
    print(f"Detail     : {error}")
    print("\nPeriksa COM port, alamat meter, A/B, baud rate, dan format serial.")
    sys.exit(1)

V, I = to_float(r[0], r[1]), to_float(r[2], r[3])
P, Q = to_float(r[4], r[5]) * 1000, to_float(r[6], r[7]) * 1000
PF, F = to_float(r[10], r[11]), to_float(r[14], r[15])
E = to_float(e[0], e[1])

print("\nBERHASIL! Meter merespons.")
print("--------------------------------")
print(f"Tegangan   : {V:8.1f} V")
print(f"Arus       : {I:8.3f} A")
print(f"Daya aktif : {P:8.1f} W")
print(f"Daya reaktif:{Q:8.1f} VAr")
print(f"Daya semu  : {V * I:8.1f} VA  (V x I)")
print(f"PF         : {PF:8.3f}")
print(f"Frekuensi  : {F:8.2f} Hz")
print(f"Energi     : {E:8.2f} kWh")
print("--------------------------------")
print("Register mentah 0x2000-0x200F:", " ".join(f"{x:04X}" for x in r))
