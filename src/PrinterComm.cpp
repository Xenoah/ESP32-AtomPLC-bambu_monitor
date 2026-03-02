#include "PrinterComm.h"

#include "AppConfig.h"

namespace {

String mqttErrorLabel(int errorCode) {
  return "ERR " + String(errorCode);
}

// Convert Bambu Lab homing_status integer to a readable label.
// Values sourced from the OpenBambuAPI community documentation.
const char* homingLabel(int status) {
  switch (status) {
    case 0: return "IDLE";
    case 1: return "HOMING";
    case 2: return "DONE";
    default: return "?";
  }
}

}  // namespace

PrinterComm* PrinterComm::instance_ = nullptr;

PrinterComm::PrinterComm() : mqttClient_(tlsClient_) {}

void PrinterComm::begin(AppState& state) {
  state_    = &state;
  instance_ = this;

  snprintf(topicSubscribe_, sizeof(topicSubscribe_), "device/%s/report",
           AppConfig::kPrinterSerial);
  snprintf(topicPublish_, sizeof(topicPublish_), "device/%s/request",
           AppConfig::kPrinterSerial);

  tlsClient_.setInsecure();
  mqttClient_.setServer(AppConfig::kPrinterHost, AppConfig::kPrinterPort);
  mqttClient_.setCallback(mqttCallback);
  // Increased from 4096 — Bambu push_status with AMS data can exceed 4 KB.
  if (!mqttClient_.setBufferSize(8192)) {
    Serial.println("MQTT buffer alloc failed, using default size");
  }
  mqttClient_.setKeepAlive(60);

  setupWiFi(state);
}

void PrinterComm::tick(AppState& state) {
  if (state.halted) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    setFatalState(state, "WIFI LOST");
    return;
  }

  ensureMqtt(state);
  if (!state.halted && mqttClient_.connected()) {
    mqttClient_.loop();
  }
}

void PrinterComm::mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (instance_ != nullptr) {
    instance_->onMessage(topic, payload, length);
  }
}

void PrinterComm::setupWiFi(AppState& state) {
  state.wifiStatus = "CONNECT";
  state.lastEvent  = "JOIN WIFI";
  requestRender(state);

  // Set mode first so the WiFi stack is initialised before reading the MAC.
  WiFi.mode(WIFI_STA);

  // Read MAC after mode is set — ESP-IDF may return zeros before initialisation.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(mqttClientId_, sizeof(mqttClientId_), "ESP32Client-%02X%02X%02X",
           mac[3], mac[4], mac[5]);

  WiFi.begin(AppConfig::kWifiSsid, AppConfig::kWifiPassword);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs >= AppConfig::kWifiConnectTimeoutMs) {
      setFatalState(state, "WIFI TIMEOUT");
      return;
    }
    delay(250);
  }

  state.wifiStatus = "OK";
  state.ipAddress  = WiFi.localIP().toString();
  state.lastEvent  = "WIFI READY";
  requestRender(state);
}

void PrinterComm::ensureMqtt(AppState& state) {
  if (mqttClient_.connected()) {
    return;
  }

  int lastError = 0;
  for (int retry = 0; retry < AppConfig::kMqttMaxRetries && !mqttClient_.connected();
       ++retry) {
    state.mqttStatus = "TRY";
    state.lastEvent =
        "MQTT " + String(retry + 1) + "/" + String(AppConfig::kMqttMaxRetries);
    requestRender(state);

    if (mqttClient_.connect(mqttClientId_, AppConfig::kPrinterUser,
                            AppConfig::kPrinterPassword)) {
      state.mqttStatus = "OK";
      state.lastEvent  = "MQTT READY";
      requestRender(state);

      if (mqttClient_.subscribe(topicSubscribe_)) {
        state.lastEvent = "SUB OK";
      } else {
        state.lastEvent = "SUB FAIL";
      }
      requestRender(state);

      // Request a full status snapshot from the printer.
      // startHoming() was removed — sending G28 automatically on every
      // (re-)connect would abort an active print.
      requestPushAll(state);
      return;
    }

    lastError        = mqttClient_.state();
    state.mqttStatus = mqttErrorLabel(lastError);
    state.lastEvent  = state.mqttStatus;
    requestRender(state);

    // Break the retry delay into short slices so the RTOS watchdog is serviced
    // and the display remains responsive (avoids WDT reset on 5-second blocks).
    for (uint32_t elapsed = 0; elapsed < AppConfig::kMqttRetryDelayMs;
         elapsed += 100) {
      delay(100);
    }
  }

  setFatalState(state, "MQTT CODE " + String(lastError));
}

void PrinterComm::onMessage(char* topic, byte* payload, unsigned int length) {
  if (state_ == nullptr) {
    return;
  }

  Serial.print("Message on topic: ");
  Serial.println(topic);

  // Deserialise directly from the raw payload buffer — avoids allocating an
  // intermediate String copy (reduces heap fragmentation on long sessions).
  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, reinterpret_cast<const char*>(payload), length);
  if (error) {
    state_->lastEvent = "JSON ERR";
    requestRender(*state_);
    return;
  }

  JsonVariantConst printValue = doc["print"];
  if (printValue.isNull() || !printValue.is<JsonObjectConst>()) {
    return;
  }

  JsonObjectConst print   = printValue.as<JsonObjectConst>();
  JsonVariantConst command = print["command"];
  if (!command.is<const char*>() ||
      strcmp(command.as<const char*>(), "push_status") != 0) {
    return;
  }

  state_->lastEvent = "PUSH STATUS";

  if (!print["bed_temper"].isNull()) {
    state_->bedTemp = formatTemperature(print["bed_temper"].as<float>());
  }
  if (!print["nozzle_temper"].isNull()) {
    state_->nozzleTemp = formatTemperature(print["nozzle_temper"].as<float>());
  }
  if (!print["wifi_signal"].isNull()) {
    // wifi_signal is a dBm string from the printer (e.g. "-60").
    state_->printerWifi = print["wifi_signal"].as<String>() + "dBm";
  }
  if (!print["sequence_id"].isNull()) {
    state_->sequenceId = print["sequence_id"].as<String>();
  }
  if (!print["homing_status"].isNull()) {
    state_->homingStatus = homingLabel(print["homing_status"].as<int>());
  }
  if (!print["gcode_state"].isNull()) {
    state_->printState = print["gcode_state"].as<String>();
  }
  if (!print["mc_percent"].isNull()) {
    state_->progress = formatPercent(print["mc_percent"].as<int>());
  }
  if (!print["layer_num"].isNull()) {
    String layerText = String(print["layer_num"].as<int>());
    if (!print["total_layer_num"].isNull()) {
      layerText += "/";
      layerText += String(print["total_layer_num"].as<int>());
    }
    state_->layer = layerText;
  }

  requestRender(*state_);
}

void PrinterComm::requestPushAll(AppState& state) {
  // Correct Bambu Lab LAN MQTT command to request a full status snapshot.
  // Uses the "pushing" top-level key (not "print").
  // Reference: OpenBambuAPI mqtt.md
  JsonDocument doc;
  JsonObject   pushing     = doc["pushing"].to<JsonObject>();
  pushing["sequence_id"]   = String(++sequenceId_);
  pushing["command"]       = "pushall";
  publishJson(state, doc, "PUSHALL SENT", "PUSHALL FAIL");
}

bool PrinterComm::publishJson(AppState& state, JsonDocument& doc,
                              const char* okEvent, const char* failEvent) {
  String payload;
  serializeJson(doc, payload);

  // retain=false: command topics must not be retained by the broker.
  // A retained command would be replayed to any future subscriber,
  // potentially triggering unintended printer actions.
  const bool published =
      mqttClient_.publish(topicPublish_, payload.c_str(), false);
  state.lastEvent = published ? okEvent : failEvent;
  requestRender(state);
  return published;
}

void PrinterComm::setFatalState(AppState& state, const String& reason) {
  state.mqttStatus  = "STOP";
  state.lastEvent   = "FATAL";
  state.errorReason = reason;
  state.halted      = true;
  requestRender(state);
  Serial.println("Fatal error: " + reason);
}

void PrinterComm::requestRender(AppState& state) {
  if (state.immediateRender) {
    // Startup phase: append the current event to the terminal log and push
    // the frame immediately (the main loop hasn't started yet).
    state.appendLog(state.lastEvent);
    state.immediateRender(state);
  }
  state.displayDirty = true;
}
