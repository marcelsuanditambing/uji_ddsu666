/*
 * =====================================================================
 *  APFC Panel — firmware ESP32 (Phase 1 Prototype)
 *  IoT Automatic Power Factor Correction · Tim Aku Anak Sehat
 *  Spesifikasi: Technical Design Book TDB-APFC-01, Bab 7
 * =====================================================================
 *  Core 1 : taskMeter  (2 s,   prio 3)  — baca DDSU666, energi, algoritma, perintah
 *  Tombol darurat dipasang sebelum RCBO: memutus seluruh panel (termasuk ESP32),
 *  sehingga tidak ditangani firmware. Saat daya kembali, panel mulai dari INIT dengan
 *  semua step OFF dan jeda sambung ulang 10 s.
 *  Core 0 : taskComm   (prio 2)         — Wi-Fi, NTP, MQTT
 *           taskUI     (1 s,   prio 1)  — LCD 20x4, LED
 *           taskLog    (10 s,  prio 1)  — microSD
 *  loop() : konsol Serial (115200)
 *
 *  Library (Arduino Library Manager):
 *    - PubSubClient  (Nick O'Leary)       v2.8
 *    - ArduinoJson   (Benoit Blanchon)    v7.x
 *    - LiquidCrystal I2C (Frank de Brabander) v1.1.2+
 *  Board: "ESP32 Dev Module" (paket esp32 by Espressif, 2.0.x atau 3.x)
 */
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include "config.h"
#include "state.h"
#include "outputs.h"
#include "storage.h"
#include "meter.h"
#include "control.h"
#include "comm.h"
#include "ui.h"
#include "logger.h"
#include "console.h"

#define WDT_TIMEOUT_MS 10000

void wdtSetup() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {.timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
#endif
}

// ---------------- meter + kendali + perintah (core 1) ----------------
void taskMeter(void*) {
  esp_task_wdt_add(NULL);
  uint32_t next = millis(), lastSave = millis();
  for (;;) {
    esp_task_wdt_reset();
    // perintah dieksekusi di sini agar hanya task ini yang mengubah step/stopkontak
    Command c;
    while (xQueueReceive(gCmdQ, &c, 0) == pdTRUE) execCommand(c);

    if ((int32_t)(millis() - next) >= 0) {
      next += SAMPLE_PERIOD_MS;
      if ((int32_t)(millis() - next) > 0) next = millis() + SAMPLE_PERIOD_MS;
      Sample s;
      bool got = meterRead(s);
      bool live;
      {
        Lock l;
        if (got) onSampleLocked(s); else onReadFailLocked();
        live = gPrintLive;
      }
      if (live) {
        Shared v;
        { Lock l; v = G; }
        if (got)
          Serial.printf("[%s] V=%.1f I=%.3f P=%.1f Q=%+.1f PF=%.3f f=%.2f | C1%c C2%c C3%c | Qneed=%+.0f%s\n",
                        STATE_NAME[v.state], s.v, s.i, s.p, s.q, s.pf, s.f, v.step[0] ? '#' : '-',
                        v.step[1] ? '#' : '-', v.step[2] ? '#' : '-', v.q_need,
                        !s.valid ? " (di luar rentang)" : (!s.consistent ? " (transien)" : ""));
        else
          Serial.printf("[%s] meter tidak merespons (%u/%d)\n", STATE_NAME[v.state], v.failCnt, N_FAIL);
      }
      if (millis() - lastSave > NVS_SAVE_MS) {
        lastSave = millis();
        Energy e;
        { Lock l; e = G.e; }
        nvsSaveEnergy(e);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  outInit();                                  // semua output OFF sebelum hal lain
  // Setelah daya kembali (mis. E-stop dilepas), anggap semua step baru saja dilepas:
  // step baru boleh disambung setelah jeda sambung ulang (muatan kapasitor sudah dibuang resistor).
  for (int k = 0; k < NUM_STEPS; k++) G.stepOffMs[k] = millis() | 1;
  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.printf("\n\nAPFC Panel v%s — %s\n", FW_VERSION, PANEL_ID);
  Serial.printf("[RELAY] K1 diam (IO%d %s), beban K1 %s\n", PIN_RELAY1, RELAY_OPEN_DRAIN ? "open-drain dilepas" : "HIGH",
                G.sock[0] ? "MENYALA" : "PADAM");

  gMutex = xSemaphoreCreateMutex();
  gCmdQ = xQueueCreate(8, sizeof(Command));
  gOutQ = xQueueCreate(12, sizeof(OutMsg));
  gLogQ = xQueueCreate(12, sizeof(OutMsg));

  nvsLoad();
  printParams();
  meterInit();
  wdtSetup();

  xTaskCreatePinnedToCore(taskMeter, "meter", 6144, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(taskComm, "comm", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskUI, "ui", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(taskLog, "log", 6144, nullptr, 1, nullptr, 0);
  Serial.println("Ketik 'help' untuk daftar perintah.");
}

void loop() {
  consolePoll();
  delay(20);
}
