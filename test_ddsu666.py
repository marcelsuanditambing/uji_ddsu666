
import minimalmodbus
import serial

# Konfigurasi sesuai meter Anda
PORT = "COM5"
SLAVE_ID = 1
BAUDRATE = 9600

meter = minimalmodbus.Instrument(
    PORT,
    SLAVE_ID,
    mode=minimalmodbus.MODE_RTU
)

meter.serial.baudrate = BAUDRATE
meter.serial.bytesize = 8
meter.serial.parity = serial.PARITY_NONE
meter.serial.stopbits = 1
meter.serial.timeout = 2

meter.clear_buffers_before_each_transaction = True

print("================================")
print("     CHINT DDSU666 TEST")
print("================================")
print(f"Port     : {PORT}")
print(f"Slave ID : {SLAVE_ID}")
print(f"Baudrate : {BAUDRATE}")
print("Membaca register Modbus...")

try:
    # Baca 16 register mulai dari alamat 0x2000
    data = meter.read_registers(
        0x2000,
        16,
        functioncode=3
    )

    print("\nBERHASIL! Meter merespons.")
    print("--------------------------------")

    for i, value in enumerate(data):
        address = 0x2000 + i
        print(f"Register 0x{address:04X}: {value}")

except Exception as error:
    print("\nBELUM BERHASIL MEMBACA METER")
    print(f"Jenis error: {type(error).__name__}")
    print(f"Detail     : {error}")
    print("\nPeriksa COM port, alamat meter, A/B,")
    print("baud rate, dan format serial.")
