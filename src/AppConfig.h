#pragma once

#include <Arduino.h>

// Credentials live in AppSecrets.h (excluded from git).
// Copy src/AppSecrets.h.template to src/AppSecrets.h and fill in your values.
#include "AppSecrets.h"

namespace AppConfig {

// Pull credentials from AppSecrets into this namespace so all existing
// code continues to use AppConfig::kWifiSsid etc. without modification.
using AppSecrets::kWifiSsid;
using AppSecrets::kWifiPassword;
using AppSecrets::kPrinterHost;
using AppSecrets::kPrinterPassword;
using AppSecrets::kPrinterSerial;

constexpr char     kPrinterUser[] = "bblp";  // Fixed value for Bambu Lab LAN MQTT
constexpr uint16_t kPrinterPort   = 8883;

constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint8_t  kMqttMaxRetries       = 5;
constexpr uint32_t kMqttRetryDelayMs     = 5000;
constexpr uint32_t kActiveLoopDelayMs    = 10;
constexpr uint32_t kHaltedLoopDelayMs    = 250;

}  // namespace AppConfig
