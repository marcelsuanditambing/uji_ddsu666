// LCD 20×4 I²C (Gbr 7-4) dan LED status. Hanya task UI yang memakai bus I²C.
#pragma once
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(LCD_ADDR, 20, 4);

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void lcdLine(uint8_t row, const char* fmt, ...) {
  char b[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  size_t n = strlen(b);
  if (n > 20) b[20] = 0;
  for (size_t k = n; k < 20; k++) b[k] = ' ';
  b[20] = 0;
  lcd.setCursor(0, row);
  lcd.print(b);
}

// Satu baris dua kolom: kolom kiri mulai kolom 0, kolom kanan mulai kolom 10 (I, Q, f sejajar).
void lcdCols(uint8_t row, const char* left, const char* right) {
  char b[24];
  snprintf(b, sizeof(b), "%-10.10s%-10.10s", left, right);
  lcd.setCursor(0, row);
  lcd.print(b);
}

void stepsText(const Shared& s, char* out) {   // "C1# C2- C3#"
  char* p = out;
  for (int k = 0; k < NUM_STEPS; k++) p += sprintf(p, "%sC%d%c", k ? " " : "", k + 1, s.step[k] ? '#' : '-');
}

void drawPage(const Shared& s, int page) {
  char st[24];
  stepsText(s, st);
  if (s.state == ST_FAILSAFE) {
    lcdLine(0, "!!  METER ERROR  !!");
    lcdLine(1, "Semua step OFF");
    lcdLine(2, "Cek kabel RS485");
    lcdLine(3, "Coba ulang: %d/%d", s.okCnt, N_RECOVER);
    return;
  }
  if (s.state == ST_INIT) {
    lcdLine(0, "APFC Panel  v%s", FW_VERSION);
    lcdLine(1, "Memulai...");
    lcdLine(2, s.meterOk ? "Meter OK" : "Menunggu meter...");
    lcdLine(3, "Aku Anak Sehat");
    return;
  }
  const char* mode = s.state == ST_MANUAL ? "MANUAL" : "AUTO  ";
  if (page == 0) {
    char l[16], r[16];
    snprintf(l, sizeof(l), "V %6.1fV", s.last.v);
    snprintf(r, sizeof(r), "I %6.3fA", s.last.i);
    lcdCols(0, l, r);
    snprintf(l, sizeof(l), "P %5.0fW", s.last.p);
    snprintf(r, sizeof(r), "Q %+5.0fvar", s.last.q);
    lcdCols(1, l, r);
    snprintf(l, sizeof(l), "PF %5.3f%c", s.last.pf, s.last.q >= 0 ? ' ' : 'c');   // c = kapasitif
    snprintf(r, sizeof(r), "f %5.2fHz", s.last.f);
    lcdCols(2, l, r);
    lcdLine(3, "%s %s", mode, st);
  } else if (page == 1) {
    float ratio = s.e.kwh > 1e-6 ? (float)(s.e.kvarh_ind / s.e.kwh) : 0.0f;
    lcdLine(0, "kWh   %12.4f", s.e.kwh);
    lcdLine(1, "kVArh %12.4f", s.e.kvarh_ind);
    // kVArh induktif ÷ kWh. PLN mengenakan denda bila > 0,62 (setara cos phi < 0,85).
    if (ratio > 9.99f) ratio = 9.99f;
    lcdLine(2, "kVArh/kWh %4.2f %s0.62", ratio, ratio <= 0.62f ? "<" : ">");
    lcdLine(3, "Penalti Rp%10.0f", penaltyRp(s.e));
  } else {
    if (s.wifiOk) lcdLine(0, "WiFi %ddBm MQTT %s", s.rssi, s.mqttOk ? "OK" : "--");
    else lcdLine(0, "WiFi --  MQTT --");
    lcdLine(1, "IP %s", s.ip);
    lcdLine(2, "Meter %s #%lu", s.meterOk ? "OK" : "--", (unsigned long)(s.seq % 100000));
    uint32_t up = millis() / 1000;
    lcdLine(3, "Up %02lu:%02lu:%02lu v%s", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
            (unsigned long)(up % 60), FW_VERSION);
  }
}

void taskUI(void*) {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  bool ok = i2cPresent(LCD_ADDR);
  if (ok) { lcd.init(); lcd.backlight(); lcd.clear(); }
  else Serial.printf("[LCD] tidak terdeteksi di 0x%02X (cek level shifter / alamat, perintah: i2c)\n", LCD_ADDR);
  { Lock l; G.lcdOk = ok; }
  uint32_t lastRetry = millis(), pageT = millis();
  int page = 0;
  bool led = false;
  for (;;) {
    Shared s;
    { Lock l; s = G; }
    if (!ok && millis() - lastRetry > 10000) {
      lastRetry = millis();
      if (i2cPresent(LCD_ADDR)) { ok = true; lcd.init(); lcd.backlight(); lcd.clear(); Lock l; G.lcdOk = true; }
    }
    static const uint32_t PAGE_MS[3] = {LCD_PAGE1_MS, LCD_PAGE2_MS, LCD_PAGE3_MS};
    if (millis() - pageT >= PAGE_MS[page]) { pageT = millis(); page = (page + 1) % 3; }
    if (ok) drawPage(s, page);

    // LED: kedip cepat = fail-safe, nyala tetap = MQTT terhubung, kedip lambat = lainnya
    if (s.state == ST_FAILSAFE) {
      for (int k = 0; k < 4; k++) { digitalWrite(PIN_LED, k % 2 == 0); vTaskDelay(pdMS_TO_TICKS(125)); }
      continue;
    }
    led = s.mqttOk ? true : !led;
    digitalWrite(PIN_LED, led);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
