// Lapisan I/O: SSR step kapasitor, relay stopkontak, tombol darurat.
#pragma once

const uint8_t SSR_PIN[NUM_STEPS]     = {PIN_SSR1, PIN_SSR2, PIN_SSR3};
const uint8_t RELAY_PIN[NUM_SOCKETS] = {PIN_RELAY1, PIN_RELAY2};
#define SSR_OFF_LEVEL   (SSR_ON_LEVEL == HIGH ? LOW : HIGH)
#define RELAY_OFF_LEVEL (RELAY_ON_LEVEL == HIGH ? LOW : HIGH)

// Dipanggil paling awal di setup(): level OFF ditulis SEBELUM pin dijadikan output.
void outInit() {
  for (int k = 0; k < NUM_STEPS; k++)   { digitalWrite(SSR_PIN[k], SSR_OFF_LEVEL);     pinMode(SSR_PIN[k], OUTPUT); }
  for (int k = 0; k < NUM_SOCKETS; k++) { digitalWrite(RELAY_PIN[k], RELAY_OFF_LEVEL); pinMode(RELAY_PIN[k], OUTPUT); }
  pinMode(PIN_LED, OUTPUT);
}

// Panggil dengan Lock aktif.
void hwStep(int k, bool on)   { digitalWrite(SSR_PIN[k], on ? SSR_ON_LEVEL : SSR_OFF_LEVEL); }
void hwSocket(int k, bool on) { digitalWrite(RELAY_PIN[k], on ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL); }

// Ubah step dan catat waktunya (panggil dengan Lock aktif).
void setStepLocked(int k, bool on) {
  if (G.step[k] == on) return;
  G.step[k] = on;
  hwStep(k, on);
  if (on) G.stepOnMs[k] = millis(); else G.stepOffMs[k] = millis();
  G.statusDirty = true;
}

void allStepsOffLocked() { for (int k = 0; k < NUM_STEPS; k++) setStepLocked(k, false); }

void allOutputsOffLocked() {
  allStepsOffLocked();
  for (int k = 0; k < NUM_SOCKETS; k++) { G.sock[k] = false; hwSocket(k, false); }
  G.statusDirty = true;
}

// Sisa detik jeda sambung ulang step k (0 = boleh disambung).
int reconnectWaitS(int k) {
  if (G.step[k]) return 0;
  uint32_t el = millis() - G.stepOffMs[k];
  uint32_t need = (uint32_t)P.t_reconnect * 1000UL;
  if (G.stepOffMs[k] == 0 || el >= need) return 0;
  return (int)((need - el + 999) / 1000);
}
