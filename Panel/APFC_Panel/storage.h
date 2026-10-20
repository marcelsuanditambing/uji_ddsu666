// Penyimpanan parameter dan penghitung energi di NVS (Preferences).
#pragma once
#include <Preferences.h>

Preferences prefs;

void nvsLoad() {
  prefs.begin("apfc", true);
  P.pf_target   = prefs.getFloat("pf_target", P.pf_target);
  P.hyst        = prefs.getFloat("hyst", P.hyst);
  P.lead_w      = prefs.getFloat("lead_w", P.lead_w);
  P.t_reconnect = prefs.getUShort("t_reconn", P.t_reconnect);
  P.n_avg       = prefs.getUChar("n_avg", P.n_avg);
  P.n_skip      = prefs.getUChar("n_skip", P.n_skip);
  P.p_min       = prefs.getFloat("p_min", P.p_min);
  // Nilai c1–c3 di NVS hanya dipakai bila C_STEPx_UF di config.h tidak berubah sejak disimpan.
  char sig[32];
  snprintf(sig, sizeof(sig), "%.2f/%.2f/%.2f", C_STEP1_UF, C_STEP2_UF, C_STEP3_UF);
  bool sameCfg = prefs.getString("c_cfg", "") == String(sig);
  if (sameCfg) {
    P.c_uf[0] = prefs.getFloat("c1", P.c_uf[0]);
    P.c_uf[1] = prefs.getFloat("c2", P.c_uf[1]);
    P.c_uf[2] = prefs.getFloat("c3", P.c_uf[2]);
  }
  P.tariff      = prefs.getFloat("tariff", P.tariff);
  P.scale       = prefs.getFloat("scale", P.scale);
  G.e.kwh        = prefs.getDouble("kwh", 0);
  G.e.kvarh_ind  = prefs.getDouble("kvi", 0);
  G.e.kvarh_cap  = prefs.getDouble("kvc", 0);
  G.e.kvarh_load = prefs.getDouble("kvl", 0);
  G.mode = prefs.getUChar("mode", ST_AUTO) == ST_MANUAL ? ST_MANUAL : ST_AUTO;
  prefs.end();
  if (!sameCfg) {
    Serial.printf("[NVS] kapasitor di config.h berubah → c1..c3 = %s uF\n", sig);
    prefs.begin("apfc", false);
    prefs.putString("c_cfg", sig);
    prefs.putFloat("c1", P.c_uf[0]);
    prefs.putFloat("c2", P.c_uf[1]);
    prefs.putFloat("c3", P.c_uf[2]);
    prefs.end();
  }
}

void nvsSaveParams() {
  prefs.begin("apfc", false);
  prefs.putFloat("pf_target", P.pf_target);
  prefs.putFloat("hyst", P.hyst);
  prefs.putFloat("lead_w", P.lead_w);
  prefs.putUShort("t_reconn", P.t_reconnect);
  prefs.putUChar("n_avg", P.n_avg);
  prefs.putUChar("n_skip", P.n_skip);
  prefs.putFloat("p_min", P.p_min);
  prefs.putFloat("c1", P.c_uf[0]);
  prefs.putFloat("c2", P.c_uf[1]);
  prefs.putFloat("c3", P.c_uf[2]);
  prefs.putFloat("tariff", P.tariff);
  prefs.putFloat("scale", P.scale);
  prefs.putUChar("mode", G.mode);
  prefs.end();
}

void nvsSaveEnergy(const Energy& e) {
  prefs.begin("apfc", false);
  prefs.putDouble("kwh", e.kwh);
  prefs.putDouble("kvi", e.kvarh_ind);
  prefs.putDouble("kvc", e.kvarh_cap);
  prefs.putDouble("kvl", e.kvarh_load);
  prefs.end();
}

void nvsFactoryReset() {
  prefs.begin("apfc", false);
  prefs.clear();
  prefs.end();
}
