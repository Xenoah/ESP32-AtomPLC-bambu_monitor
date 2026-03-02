#pragma once

// ============================================================
// SETUP: Copy this file to src/AppSecrets.h and fill in your
//        values. AppSecrets.h is excluded from git — never
//        commit it with real credentials.
// ============================================================

namespace AppSecrets {

// Wi-Fi network
constexpr char kWifiSsid[]     = "TP-Link_7DE4";
constexpr char kWifiPassword[] = "09383685";

// Bambu Lab printer — LAN mode
// kPrinterHost:     IP address shown on the printer's network screen
// kPrinterPassword: Access Code shown on the printer's settings screen
// kPrinterSerial:   Serial number (starts with a letter, e.g. "01S00C...")
constexpr char kPrinterHost[]     = "192.168.0.142";
constexpr char kPrinterPassword[] = "15468416";
constexpr char kPrinterSerial[]   = "01P09C482401047";

}  // namespace AppSecrets
