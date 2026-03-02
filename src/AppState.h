#pragma once

#include <Arduino.h>

struct AppState {
  // Dashboard fields
  String wifiStatus   = "BOOT";
  String ipAddress    = "--";
  String mqttStatus   = "WAIT";
  String bedTemp      = "--";
  String nozzleTemp   = "--";
  String printerWifi  = "--";
  String progress     = "--";
  String layer        = "--";
  String printState   = "--";
  String homingStatus = "--";
  String sequenceId   = "--";
  String lastEvent    = "INIT";
  String errorReason  = "";
  bool   halted       = false;
  bool   displayDirty = true;

  // ── Startup terminal log ────────────────────────────────────────────────
  // Circular buffer of messages shown while Wi-Fi and MQTT are connecting.
  static constexpr uint8_t kLogCapacity = 14;
  String  logLines[kLogCapacity];
  uint8_t logCount = 0;

  void appendLog(const String& line) {
    if (logCount < kLogCapacity) {
      logLines[logCount++] = line;
    } else {
      for (uint8_t i = 0; i < kLogCapacity - 1; ++i) {
        logLines[i] = logLines[i + 1];
      }
      logLines[kLogCapacity - 1] = line;
    }
  }

  // ── Immediate-render callback ───────────────────────────────────────────
  // Set by main.cpp before printerComm.begin() so requestRender() can push
  // screen updates during the blocking startup phase.
  // Cleared to nullptr once the main loop begins.
  using RenderFn = void (*)(AppState&);
  RenderFn immediateRender = nullptr;
};

inline String formatTemperature(float value) {
  return String(value, 1) + " C";
}

inline String formatPercent(int value) {
  return String(value) + "%";
}
