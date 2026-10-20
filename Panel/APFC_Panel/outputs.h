// Lapisan I/O: SSR step kapasitor dan relay beban K1.
#pragma once

const uint8_t SSR_PIN[NUM_STEPS]     = {PIN_SSR1, PIN_SSR2, PIN_SSR3};
const uint8_t RELAY_PIN[NUM_SOCKETS] = {PIN_RELAY1};
#define SSR_OFF_LEVEL   (SSR_ON_LEVEL == HIGH ? LOW : HIGH)
#define RELAY_OFF_LEVEL (RELAY_ON_LEVEL == HIGH ? LOW : HIGH)

// Dipanggil paling awal di setup(): level OFF ditulis SEBELUM pin dijadikan output.
void outInit() {
  for (int k = 0; k < NUM_STEPS; k++)   { digitalWrite(SSR_PIN[k], SSR_OFF_LEVEL);     pinMode(SSR_PIN[k], OUTPUT); }
  // Relay tidak dienergikan saat boot. Dengan kontak NC, beban K1 berarti MENYALA.
  for (int k = 0; k < NUM_SOCKETS; k++) { digitalWrite(RELAY_PIN[k], RELAY_OFF_LEVEL); pinMode(RELAY_PIN[k], OUTPUT); G.sock[k] = LOAD_K1_NC; }
  pinMode(PIN_LED, OUTPUT);
}

// Panggil dengan Lock aktif.
void hwStep(int k, bool on)   { digitalWrite(SSR_PIN[k], on ? SSR_ON_LEVEL : SSR_OFF_LEVEL); }
// loadOn = beban menyala. Kontak NC: kumparan relay dienergikan untuk MEMATIKAN beban.
void hwSocket(int k, bool loadOn) {
  bool coil = LOAD_K1_NC ? !loadOn : loadOn;
  digitalWrite(RELAY_PIN[k], coil ? RELAY_ON_LEVEL : RELAY_OFF_LEVEL);
}

// Ubah step dan catat waktunya (panggil dengan Lock aktif).
void setStepLocked(int k, bool on) {
  if (G.step[k] == on) return;
  G.step[k] = on;
  hwStep(k, on);
  if (on) G.stepOnMs[k] = millis(); else G.stepOffMs[k] = millis();
  G.statusDirty = true;
}

void allStepsOffLocked() { for (int k = 0; k < NUM_STEPS; k++) setStepLocked(k, false); }


// Sisa detik jeda sambung ulang step k (0 = boleh disambung).
int reconnectWaitS(int k) {
  if (G.step[k]) return 0;
  uint32_t el = millis() - G.stepOffMs[k];
  uint32_t need = (uint32_t)P.t_reconnect * 1000UL;
  if (G.stepOffMs[k] == 0 || el >= need) return 0;
  return (int)((need - el + 999) / 1000);
}
