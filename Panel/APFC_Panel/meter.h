// Klien Modbus RTU sederhana (FC03) untuk CHINT DDSU666 — float32, word tinggi lebih dulu.
#pragma once

HardwareSerial RS485(2);
volatile bool gDumpRaw = false;   // perintah serial "raw"

uint16_t mbCrc(const uint8_t* b, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= b[i];
    for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// Baca n register mulai addr. Toleran terhadap echo (modul auto-direction yang ikut memantulkan request).
bool mbRead(uint16_t addr, uint16_t n, uint16_t* out) {
  uint8_t req[8] = {METER_ID, 0x03, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(n >> 8), (uint8_t)n, 0, 0};
  uint16_t c = mbCrc(req, 6);
  req[6] = c & 0xFF; req[7] = c >> 8;
  while (RS485.available()) RS485.read();
  RS485.write(req, 8);
  RS485.flush();

  const size_t need = 5 + 2 * n;
  uint8_t buf[96];
  size_t got = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < METER_TIMEOUT_MS && got < sizeof(buf)) {
    while (RS485.available() && got < sizeof(buf)) buf[got++] = RS485.read();
    // cari bingkai valid di posisi mana pun (melewati echo bila ada)
    for (size_t o = 0; o + need <= got; o++) {
      if (buf[o] != METER_ID || buf[o + 1] != 0x03 || buf[o + 2] != 2 * n) continue;
      uint16_t rc = buf[o + need - 2] | (buf[o + need - 1] << 8);
      if (mbCrc(buf + o, need - 2) != rc) continue;
      for (uint16_t k = 0; k < n; k++) out[k] = (buf[o + 3 + 2 * k] << 8) | buf[o + 4 + 2 * k];
      if (gDumpRaw) {
        Serial.printf("[RAW %04X] ", addr);
        for (uint16_t k = 0; k < n; k++) Serial.printf("%04X ", out[k]);
        Serial.println();
      }
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (gDumpRaw) {
    Serial.printf("[RAW %04X] gagal, %u byte diterima:", addr, (unsigned)got);
    for (size_t k = 0; k < got; k++) Serial.printf(" %02X", buf[k]);
    Serial.println();
  }
  return false;
}

float regFloat(const uint16_t* r) {
  uint32_t u = ((uint32_t)r[0] << 16) | r[1];
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// Satu siklus baca: 2000H×16 (U, I, P, Q, -, PF, -, f) dan 4000H×2 (energi). Retry sekali.
bool meterRead(Sample& s) {
  uint16_t r[16], e[2];
  bool ok = mbRead(0x2000, 16, r) || mbRead(0x2000, 16, r);
  if (!ok) return false;
  bool okE = mbRead(0x4000, 2, e);
  s.ms = millis();
  s.v = regFloat(&r[0]);
  s.i = regFloat(&r[2]);
  s.p = regFloat(&r[4]) * 1000.0f;   // kW → W
  s.q = regFloat(&r[6]) * 1000.0f;   // kvar → var (positif = induktif)
  s.pf_meter = regFloat(&r[10]);
  s.f = regFloat(&r[14]);
  if (okE) s.kwh_meter = regFloat(e);
  s.s = sqrtf(s.p * s.p + s.q * s.q);
  s.pf = s.s > 1.0f ? fabsf(s.p) / s.s : 1.0f;

  // validasi rentang (Tabel 7-3)
  s.valid = isfinite(s.v) && isfinite(s.i) && isfinite(s.p) && isfinite(s.q) && isfinite(s.f) &&
            s.v >= 150 && s.v <= 260 && s.i >= 0 && s.i <= 20 && fabsf(s.p) <= 4600 && fabsf(s.q) <= 4600 &&
            s.f >= 45 && s.f <= 55;
  // konsistensi: √(P²+Q²) ≈ U·I (menyaring sampel transien saat beban/step berpindah)
  float ui = s.v * s.i;
  s.consistent = fabsf(s.s - ui) <= max(10.0f, 0.08f * ui);
  return true;
}

void meterInit() { RS485.begin(METER_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX); }
