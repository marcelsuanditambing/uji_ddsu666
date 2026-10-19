// Wi-Fi, NTP dan MQTT (kontrak data Subbab 7.4). Hanya task komunikasi yang menyentuh klien MQTT.
#pragma once
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

WiFiClient   netClient;
PubSubClient mqtt(netClient);
String       topicBase = String("apfc/") + PANEL_ID + "/";

void mqttPublish(const char* sub, const char* payload, bool retain = false) {
  if (!mqtt.connected()) return;
  mqtt.publish((topicBase + sub).c_str(), payload, retain);
}

// "on" boleh berupa 1/0 atau true/false
int jsonOn(JsonVariantConst v) { return v.is<bool>() ? (v.as<bool>() ? 1 : 0) : ((v | 0) != 0 ? 1 : 0); }

// ---------------- perintah masuk: apfc/panel01/cmd ----------------
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) { Serial.println("[MQTT] cmd bukan JSON valid"); return; }
  Command c;
  c.fromMqtt = true;
  strlcpy(c.id, doc["id"] | "-", sizeof(c.id));
  const char* cmd = doc["cmd"] | "";
  if (!strcmp(cmd, "set_mode")) {
    c.type = C_MODE;
    c.a = !strcmp(doc["mode"] | "AUTO", "MANUAL") ? ST_MANUAL : ST_AUTO;
  } else if (!strcmp(cmd, "set_step")) {
    c.type = C_STEP; c.a = doc["step"] | 0; c.b = jsonOn(doc["on"]);
  } else if (!strcmp(cmd, "set_socket")) {
    c.type = C_SOCKET; c.a = doc["socket"] | 0; c.b = jsonOn(doc["on"]);
  } else if (!strcmp(cmd, "set_param")) {
    c.type = C_SET; strlcpy(c.key, doc["key"] | "", sizeof(c.key)); c.val = doc["value"] | 0.0f;
  } else if (!strcmp(cmd, "energy_reset")) {
    c.type = C_ENERGY_RESET;
  } else {
    OutMsg m;
    strlcpy(m.topic, "cmd/ack", sizeof(m.topic));
    snprintf(m.payload, sizeof(m.payload), "{\"id\":\"%s\",\"ok\":0,\"msg\":\"perintah tidak dikenal\"}", c.id);
    xQueueSend(gOutQ, &m, 0);
    return;
  }
  xQueueSend(gCmdQ, &c, 0);
}

void buildTelemetry(char* out, size_t n) {
  Shared s;
  { Lock l; s = G; }
  JsonDocument d;
  d["ts"] = unixTime();
  d["seq"] = s.seq;
  d["v"] = roundf(s.last.v * 10) / 10;
  d["i"] = roundf(s.last.i * 1000) / 1000;
  d["p"] = roundf(s.last.p * 10) / 10;
  d["q"] = roundf(s.last.q * 10) / 10;
  d["s"] = roundf(s.last.s * 10) / 10;
  d["pf"] = roundf(s.last.pf * 1000) / 1000;
  d["f"] = roundf(s.last.f * 100) / 100;
  d["q_load"] = roundf(s.q_load * 10) / 10;
  d["q_need"] = roundf(s.q_need * 10) / 10;
  d["kwh"] = s.e.kwh;
  d["kwh_m"] = s.last.kwh_meter;
  d["kvarh_ind"] = s.e.kvarh_ind;
  d["kvarh_cap"] = s.e.kvarh_cap;
  d["kvarh_load"] = s.e.kvarh_load;
  JsonArray st = d["steps"].to<JsonArray>();
  for (int k = 0; k < NUM_STEPS; k++) st.add(s.step[k] ? 1 : 0);
  JsonArray so = d["sockets"].to<JsonArray>();
  for (int k = 0; k < NUM_SOCKETS; k++) so.add(s.sock[k] ? 1 : 0);
  d["mode"] = STATE_NAME[s.mode];
  d["state"] = STATE_NAME[s.state];
  d["rssi"] = s.rssi;
  serializeJson(d, out, n);
}

void buildStatus(char* out, size_t n) {
  Shared s;
  { Lock l; s = G; G.statusDirty = false; }
  JsonDocument d;
  d["state"] = STATE_NAME[s.state];
  d["mode"] = STATE_NAME[s.mode];
  JsonArray st = d["steps"].to<JsonArray>();
  for (int k = 0; k < NUM_STEPS; k++) st.add(s.step[k] ? 1 : 0);
  JsonArray so = d["sockets"].to<JsonArray>();
  for (int k = 0; k < NUM_SOCKETS; k++) so.add(s.sock[k] ? 1 : 0);
  d["meter_ok"] = s.meterOk ? 1 : 0;
  d["fw"] = FW_VERSION;
  d["uptime"] = millis() / 1000;
  d["ip"] = s.ip;
  JsonArray c = d["c_uf"].to<JsonArray>();
  for (int k = 0; k < NUM_STEPS; k++) c.add(P.c_uf[k]);
  d["pf_target"] = P.pf_target;
  serializeJson(d, out, n);
}

// ---------------- task komunikasi (core 0) ----------------
void taskComm(void*) {
  bool wifiEnabled = strlen(WIFI_SSID) > 0;
  uint32_t lastWifiTry = 0, lastMqttTry = 0, lastStatus = 0, lastSeq = 0;
  bool ntpStarted = false;
  static char buf[640];
  if (wifiEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    lastWifiTry = millis();
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);
    mqtt.setCallback(onMqttMessage);
    mqtt.setSocketTimeout(3);
  } else {
    Serial.println("[WIFI] WIFI_SSID kosong: panel berjalan tanpa Wi-Fi/MQTT");
  }

  for (;;) {
    if (wifiEnabled) {
      bool w = WiFi.status() == WL_CONNECTED;
      if (!w && millis() - lastWifiTry > 15000) {
        lastWifiTry = millis();
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      if (w && !ntpStarted) {
        configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
        ntpStarted = true;
        Serial.printf("[WIFI] terhubung, IP %s\n", WiFi.localIP().toString().c_str());
      }
      {
        Lock l;
        if (G.wifiOk != w) G.statusDirty = true;
        G.wifiOk = w;
        G.rssi = w ? WiFi.RSSI() : 0;
        strlcpy(G.ip, w ? WiFi.localIP().toString().c_str() : "-", sizeof(G.ip));
      }

      if (w && !mqtt.connected() && millis() - lastMqttTry > 5000) {
        lastMqttTry = millis();
        String cid = String("apfc-") + PANEL_ID;
        String will = topicBase + "online";
        bool ok = strlen(MQTT_USER)
                      ? mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, will.c_str(), 1, true, "0")
                      : mqtt.connect(cid.c_str(), will.c_str(), 1, true, "0");
        if (ok) {
          Serial.printf("[MQTT] terhubung ke %s:%d\n", MQTT_HOST, MQTT_PORT);
          mqttPublish("online", "1", true);
          mqtt.subscribe((topicBase + "cmd").c_str(), 1);
          { Lock l; G.statusDirty = true; }
        } else {
          Serial.printf("[MQTT] gagal terhubung (rc=%d), coba lagi 5 s\n", mqtt.state());
        }
      }
      bool mc = mqtt.connected();
      { Lock l; if (G.mqttOk != mc) G.statusDirty = true; G.mqttOk = mc; }

      if (mc) {
        mqtt.loop();
        uint32_t seq;
        bool dirty;
        { Lock l; seq = G.seq; dirty = G.statusDirty; }
        if (seq != lastSeq) {
          lastSeq = seq;
          buildTelemetry(buf, sizeof(buf));
          mqttPublish("telemetry", buf);
        }
        if (dirty || millis() - lastStatus > STATUS_PERIOD_MS) {
          lastStatus = millis();
          buildStatus(buf, sizeof(buf));
          mqttPublish("status", buf, true);
        }
      }
    }
    // kirim event / ack yang mengantre (dibuang bila MQTT belum terhubung)
    OutMsg m;
    while (xQueueReceive(gOutQ, &m, 0) == pdTRUE) mqttPublish(m.topic, m.payload);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
