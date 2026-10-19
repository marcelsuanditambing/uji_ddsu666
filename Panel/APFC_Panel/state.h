// Struktur data bersama, antrian dan kunci (mutex) antar-task.
#pragma once
#include <Arduino.h>

enum State : uint8_t { ST_INIT, ST_AUTO, ST_MANUAL, ST_FAILSAFE };
static const char* STATE_NAME[] = {"INIT", "AUTO", "MANUAL", "FAILSAFE"};

#define NUM_STEPS   3
#define NUM_SOCKETS 2

struct Sample {
  bool     valid = false;     // nilai dalam rentang
  bool     consistent = true; // S ≈ U·I (bukan transien)
  uint32_t ms = 0;
  float v = 0, i = 0, p = 0, q = 0, s = 0, pf = 1, f = 50, pf_meter = 1, kwh_meter = 0;
};

struct Params {
  float    pf_target = DEF_PF_TARGET;
  float    hyst = DEF_HYST;
  float    lead_w = DEF_LEAD_W;
  uint16_t t_reconnect = DEF_T_RECONNECT;
  uint8_t  n_avg = DEF_N_AVG;
  uint8_t  n_skip = DEF_N_SKIP;
  float    p_min = DEF_P_MIN;
  float    c_uf[NUM_STEPS] = {C_STEP1_UF, C_STEP2_UF, C_STEP3_UF};
  float    tariff = DEF_TARIFF;
  float    scale = DEF_SCALE;
};

struct Energy {
  double kwh = 0, kvarh_ind = 0, kvarh_cap = 0, kvarh_load = 0;
};

struct Shared {
  Sample   last;
  uint32_t seq = 0;
  State    state = ST_INIT;
  State    mode = ST_AUTO;            // mode pilihan pengguna (AUTO/MANUAL)
  bool     step[NUM_STEPS] = {false, false, false};
  bool     sock[NUM_SOCKETS] = {false, false};
  uint32_t stepOffMs[NUM_STEPS] = {0, 0, 0};
  uint32_t stepOnMs[NUM_STEPS] = {0, 0, 0};
  float    q_load = 0, q_need = 0;
  Energy   e;
  bool     meterOk = false;
  uint8_t  failCnt = 0, okCnt = 0;
  uint32_t readErrTotal = 0;
  bool     wifiOk = false, mqttOk = false, sdOk = false, lcdOk = false;
  int      rssi = 0;
  char     ip[16] = "-";
  bool     statusDirty = true;
};

Shared   G;
Params   P;
SemaphoreHandle_t gMutex;

struct Lock {
  Lock()  { xSemaphoreTake(gMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(gMutex); }
};

// ---------------- perintah (Serial / MQTT) → task kendali ----------------
enum CmdType : uint8_t { C_MODE, C_STEP, C_SOCKET, C_SET, C_ENERGY_RESET };
struct Command {
  CmdType type;
  int     a = 0;        // mode (ST_AUTO/ST_MANUAL) atau nomor step/stopkontak (1..n)
  int     b = 0;        // 1 = ON, 0 = OFF
  float   val = 0;
  char    key[16] = "";
  char    id[24] = "";
  bool    fromMqtt = false;
};
QueueHandle_t gCmdQ;

// ---------------- pesan keluar (event / ack) → task komunikasi & log ----------------
struct OutMsg {
  char topic[16];       // "event" atau "cmd/ack"
  char payload[176];
};
QueueHandle_t gOutQ;    // ke MQTT
QueueHandle_t gLogQ;    // ke microSD (event)

uint32_t unixTime() {
  time_t now = time(nullptr);
  return now > 1700000000 ? (uint32_t)now : 0;
}

// Q satu step pada tegangan v dan frekuensi f (var)
inline float qStep(int k, float v, float f) { return 2.0f * PI * f * P.c_uf[k] * 1e-6f * v * v; }

void postEvent(const char* type, const char* detail) {
  OutMsg m;
  strlcpy(m.topic, "event", sizeof(m.topic));
  snprintf(m.payload, sizeof(m.payload), "{\"ts\":%lu,\"type\":\"%s\",\"detail\":\"%s\"}",
           (unsigned long)unixTime(), type, detail);
  xQueueSend(gOutQ, &m, 0);
  xQueueSend(gLogQ, &m, 0);
  Serial.printf("[EVENT] %s %s\n", type, detail);
  G.statusDirty = true;
}

void reportAck(const Command& c, bool ok, const char* msg) {
  Serial.printf("[%s] %s\n", ok ? "OK" : "DITOLAK", msg);
  if (!c.fromMqtt) return;
  OutMsg m;
  strlcpy(m.topic, "cmd/ack", sizeof(m.topic));
  snprintf(m.payload, sizeof(m.payload), "{\"id\":\"%s\",\"ok\":%d,\"msg\":\"%s\"}", c.id, ok ? 1 : 0, msg);
  xQueueSend(gOutQ, &m, 0);
  if (!ok) {
    char d[96];
    snprintf(d, sizeof(d), "%s: %s", c.id, msg);
    postEvent("CMD_REJECT", d);
  }
}
