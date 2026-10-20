// Mesin keadaan, algoritma koreksi faktor daya, integrasi energi, dan eksekusi perintah.
#pragma once

// ---------------- jendela rata-rata ----------------
struct Window {
  float p[10], q[10], v[10], f[10];
  uint8_t n = 0, head = 0;
  void clear() { n = 0; head = 0; }
  void push(const Sample& s) {
    p[head] = s.p; q[head] = s.q; v[head] = s.v; f[head] = s.f;
    head = (head + 1) % 10;
    if (n < 10) n++;
  }
  // rata-rata m sampel terakhir
  void mean(uint8_t m, float& P_, float& Q_, float& V_, float& F_) const {
    P_ = Q_ = V_ = F_ = 0;
    for (uint8_t k = 0; k < m; k++) {
      uint8_t i = (head + 10 - 1 - k) % 10;
      P_ += p[i]; Q_ += q[i]; V_ += v[i]; F_ += f[i];
    }
    P_ /= m; Q_ /= m; V_ /= m; F_ /= m;
  }
} win;

uint8_t  skipLeft = 0;
uint8_t  initOk = 0;
uint16_t inconsistentRun = 0;
uint32_t lastEnergyMs = 0;
Sample   prevSample;
bool     havePrev = false;

void afterSwitchLocked() { win.clear(); skipLeft = P.n_skip; }

// ---------------- integrasi energi (trapesium) ----------------
void integrateEnergyLocked(const Sample& s) {
  float qOn = 0;
  for (int k = 0; k < NUM_STEPS; k++) if (G.step[k]) qOn += qStep(k, s.v, s.f);
  G.q_load = s.q + qOn;
  if (havePrev) {
    float dt_h = (s.ms - prevSample.ms) / 3.6e6f;
    if (dt_h > 0 && dt_h < 15.0f / 3600.0f) {   // abaikan celah > 15 s
      float pm = 0.5f * (s.p + prevSample.p);
      float qm = 0.5f * (s.q + prevSample.q);
      G.e.kwh       += max(0.0f, pm) * dt_h / 1000.0;
      G.e.kvarh_ind += max(0.0f, qm) * dt_h / 1000.0;
      G.e.kvarh_cap += max(0.0f, -qm) * dt_h / 1000.0;
      G.e.kvarh_load += max(0.0f, G.q_load) * dt_h / 1000.0;
    }
  }
  prevSample = s;
  havePrev = true;
}

// ---------------- algoritma AUTO ----------------
// Memilih kombinasi step (2^3) yang membuat Q sisa paling dekat ke 0 dengan bobot lebih besar
// untuk kondisi kapasitif. Berpindah hanya bila kondisi saat ini di luar pita target DAN
// perbaikannya lebih besar dari margin histeresis; maksimal MAX_ON_PER_DECISION step disambung.
float qCost(float qa) { return qa >= 0 ? qa : -qa * P.lead_w; }

void autoDecideLocked() {
  if (G.mode != ST_AUTO || G.state != ST_AUTO) return;
  if (skipLeft > 0 || win.n < P.n_avg) return;
  float Pm, Qm, Vm, Fm;
  win.mean(P.n_avg, Pm, Qm, Vm, Fm);

  float qs[NUM_STEPS], qOn = 0, qMin = 1e9;
  for (int k = 0; k < NUM_STEPS; k++) {
    qs[k] = qStep(k, Vm, Fm);
    if (G.step[k]) qOn += qs[k];
    qMin = min(qMin, qs[k]);
  }
  float qt = Pm * tanf(acosf(P.pf_target));   // Q yang masih boleh tersisa
  float qLoad = Qm + qOn;
  G.q_need = Qm - qt;

  // tanpa beban → lepas semua step
  if (Pm < P.p_min) {
    bool any = false;
    for (int k = 0; k < NUM_STEPS; k++) any |= G.step[k];
    if (any) {
      allStepsOffLocked();
      afterSwitchLocked();
      postEvent("STEP_OFF", "semua (P < p_min)");
    }
    return;
  }

  // kondisi saat ini sudah di dalam pita target (PF ≥ target, induktif maupun kapasitif)
  if (fabsf(Qm) <= qt) return;

  int curMask = 0;
  for (int k = 0; k < NUM_STEPS; k++) if (G.step[k]) curMask |= 1 << k;
  float curCost = qCost(Qm);
  int best = curMask;
  float bestScore = curCost;
  uint32_t now = millis();
  for (int m = 0; m < (1 << NUM_STEPS); m++) {
    if (m == curMask) continue;
    int newOn = 0;
    bool feasible = true;
    float qa = qLoad;
    float tie = 0;   // pemecah seri: utamakan step yang paling lama beristirahat / paling lama menyala
    for (int k = 0; k < NUM_STEPS; k++) {
      bool on = m & (1 << k);
      if (on) qa -= qs[k];
      if (on && !G.step[k]) {
        newOn++;
        if (reconnectWaitS(k) > 0) feasible = false;
        tie -= (now - G.stepOffMs[k]) * 1e-9f;
      }
      if (!on && G.step[k]) tie -= (now - G.stepOnMs[k]) * 1e-9f;
    }
    if (!feasible || newOn > MAX_ON_PER_DECISION) continue;
    float score = qCost(qa) + tie;
    if (score < bestScore) { bestScore = score; best = m; }
  }
  if (best == curMask || bestScore > curCost - P.hyst * qMin) return;

  char d[80];
  for (int k = 0; k < NUM_STEPS; k++) {
    bool on = best & (1 << k);
    if (on == G.step[k]) continue;
    setStepLocked(k, on);
    snprintf(d, sizeof(d), "C%d Qneed=%.0f Qload=%.0f", k + 1, G.q_need, qLoad);
    postEvent(on ? "STEP_ON" : "STEP_OFF", d);
  }
  afterSwitchLocked();
}

// ---------------- dipanggil task meter setiap siklus ----------------
void onReadFailLocked() {
  G.readErrTotal++;
  G.okCnt = 0;
  if (G.failCnt < 255) G.failCnt++;
  G.meterOk = false;
  if (G.failCnt >= N_FAIL && (G.state == ST_AUTO || G.state == ST_MANUAL || G.state == ST_INIT)) {
    allStepsOffLocked();
    G.state = ST_FAILSAFE;
    havePrev = false;
    postEvent("FAILSAFE", "meter tidak terbaca 3x");
  }
}

void onSampleLocked(Sample& s) {
  if (!s.valid) { onReadFailLocked(); return; }   // nilai di luar rentang = gagal baca
  G.failCnt = 0;
  G.meterOk = true;

  // sampel transien (S ≠ U·I) dilewati; bila terus-menerus ≥ 15 sampel (beban non-linear) → diterima
  if (!s.consistent) {
    if (inconsistentRun < 15) { inconsistentRun++; return; }
  } else {
    inconsistentRun = 0;
  }

  G.last = s;
  G.seq++;
  integrateEnergyLocked(s);

  switch (G.state) {
    case ST_INIT:
      if (++initOk >= 3) {
        G.state = G.mode;
        initOk = 0;
        afterSwitchLocked();
        postEvent("MODE", STATE_NAME[G.state]);
      }
      break;
    case ST_FAILSAFE:
      if (++G.okCnt >= N_RECOVER) {
        G.okCnt = 0;
        G.state = G.mode;
        afterSwitchLocked();
        postEvent("RECOVER", STATE_NAME[G.state]);
      }
      break;
    case ST_AUTO:
    case ST_MANUAL:
      if (skipLeft > 0) {
        skipLeft--;
      } else {
        // beban berubah (P atau Q melompat) → mulai rata-rata baru agar keputusan tidak memakai data lama
        if (win.n > 0) {
          float mp, mq, mv, mf, qmin = 1e9f;
          win.mean(min(win.n, P.n_avg), mp, mq, mv, mf);
          for (int k = 0; k < NUM_STEPS; k++) qmin = min(qmin, qStep(k, s.v, s.f));
          if (fabsf(s.q - mq) > 0.5f * qmin || fabsf(s.p - mp) > max(10.0f, 0.2f * fabsf(mp))) win.clear();
        }
        win.push(s);
      }
      autoDecideLocked();
      break;
    default:
      break;
  }
  // q_need tetap diperbarui untuk telemetry di mode MANUAL
  if (G.state != ST_AUTO) G.q_need = s.q - fabsf(s.p) * tanf(acosf(P.pf_target));
}

// ---------------- eksekusi perintah (hanya dari task kendali) ----------------
bool setParam(const char* key, float v, char* msg, size_t n) {
  struct { const char* k; float lo, hi; } R[] = {
      {"pf_target", 0.90f, 0.99f}, {"hyst", 0.05f, 0.9f}, {"lead_w", 1.0f, 5.0f}, {"t_reconnect", 10, 60},
      {"n_avg", 1, 10}, {"n_skip", 0, 5}, {"p_min", 0, 200}, {"c1", 0.5f, 20}, {"c2", 0.5f, 20}, {"c3", 0.5f, 20},
      {"tariff", 1, 100000}, {"scale", 1, 1000}};
  for (auto& r : R) {
    if (strcmp(key, r.k)) continue;
    if (v < r.lo || v > r.hi) { snprintf(msg, n, "%s di luar rentang %.2f-%.2f", key, r.lo, r.hi); return false; }
    if (!strcmp(key, "pf_target")) P.pf_target = v;
    else if (!strcmp(key, "hyst")) P.hyst = v;
    else if (!strcmp(key, "lead_w")) P.lead_w = v;
    else if (!strcmp(key, "t_reconnect")) P.t_reconnect = (uint16_t)v;
    else if (!strcmp(key, "n_avg")) P.n_avg = (uint8_t)v;
    else if (!strcmp(key, "n_skip")) P.n_skip = (uint8_t)v;
    else if (!strcmp(key, "p_min")) P.p_min = v;
    else if (!strcmp(key, "c1")) P.c_uf[0] = v;
    else if (!strcmp(key, "c2")) P.c_uf[1] = v;
    else if (!strcmp(key, "c3")) P.c_uf[2] = v;
    else if (!strcmp(key, "tariff")) P.tariff = v;
    else if (!strcmp(key, "scale")) P.scale = v;
    snprintf(msg, n, "%s = %.3f (tersimpan)", key, v);
    return true;
  }
  snprintf(msg, n, "kunci '%s' tidak dikenal", key);
  return false;
}

void execCommand(const Command& c) {
  char msg[96];
  bool ok = false, saveParams = false, saveEnergy = false;
  {
    Lock l;
    switch (c.type) {
      case C_MODE:
        G.mode = (State)c.a;
        if (G.state == ST_AUTO || G.state == ST_MANUAL) { G.state = G.mode; afterSwitchLocked(); }
        snprintf(msg, sizeof(msg), "mode %s", STATE_NAME[G.mode]);
        ok = saveParams = true;
        postEvent("MODE", STATE_NAME[G.mode]);
        break;
      case C_STEP: {
        int k = c.a - 1;
        if (k < 0 || k >= NUM_STEPS) { snprintf(msg, sizeof(msg), "step harus 1-%d", NUM_STEPS); break; }
        if (G.state != ST_MANUAL) { snprintf(msg, sizeof(msg), "step hanya bisa diatur di MANUAL (sekarang %s)", STATE_NAME[G.state]); break; }
        if (c.b && reconnectWaitS(k) > 0) { snprintf(msg, sizeof(msg), "jeda sambung ulang, tunggu %d s", reconnectWaitS(k)); break; }
        setStepLocked(k, c.b);
        afterSwitchLocked();
        snprintf(msg, sizeof(msg), "C%d %s", k + 1, c.b ? "ON" : "OFF");
        postEvent(c.b ? "STEP_ON" : "STEP_OFF", msg);
        ok = true;
        break;
      }
      case C_SOCKET: {
        int k = c.a - 1;
        if (k < 0 || k >= NUM_SOCKETS) { snprintf(msg, sizeof(msg), "hanya beban 1 (relay K1)"); break; }
        G.sock[k] = c.b;
        hwSocket(k, c.b);
        G.statusDirty = true;
        snprintf(msg, sizeof(msg), "beban K1 %s", c.b ? "dinyalakan" : "dimatikan");
        postEvent("LOAD", c.b ? "beban K1 menyala" : "beban K1 padam");
        ok = true;
        break;
      }
      case C_SET:
        ok = saveParams = setParam(c.key, c.val, msg, sizeof(msg));
        break;
      case C_ENERGY_RESET:
        G.e = Energy();
        snprintf(msg, sizeof(msg), "penghitung energi direset");
        ok = saveEnergy = true;
        break;
    }
  }
  if (saveParams) nvsSaveParams();
  if (saveEnergy) nvsSaveEnergy(G.e);
  reportAck(c, ok, msg);
}

// Hitung penalti untuk LCD / Serial
float penaltyRp(const Energy& e) {
  double over = e.kvarh_ind - 0.62 * e.kwh;
  return over > 0 ? (float)(over * P.tariff * P.scale) : 0.0f;
}
