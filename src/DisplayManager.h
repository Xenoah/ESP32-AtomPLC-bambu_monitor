#pragma once

// Do NOT manually define LGFX_ESP32_S3_BOX_LITE here.
// LGFX_AUTODETECT.hpp handles board detection at runtime.
// Combining a manual #define with AUTODETECT caused panel
// config conflicts that resulted in a blank or flashing screen.
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>

#include "AppState.h"

class DisplayManager {
 public:
  bool begin(AppState& state);
  void render(const AppState& state);
  // Terminal-style log shown during startup while Wi-Fi / MQTT connect.
  void renderStartup(const AppState& state);

 private:
  void renderDashboard(const AppState& state);
  void renderFatalScreen(const AppState& state);

  // All draw helpers take a canvas reference so they work on both
  // the full-screen sprite (double-buffer path) and the LCD directly
  // (fallback when PSRAM allocation fails).
  void drawField(lgfx::LovyanGFX& c, int16_t y, const char* label,
                 const String& value, uint16_t valueColor);
  void drawText(lgfx::LovyanGFX& c, int16_t x, int16_t y, const String& text,
                uint16_t fg, uint16_t bg);
  void drawText(lgfx::LovyanGFX& c, int16_t x, int16_t y, const char* text,
                uint16_t fg, uint16_t bg);

  LGFX        lcd_;
  LGFX_Sprite sprite_{&lcd_};  // full-screen off-screen buffer
  bool ready_       = false;
  bool spriteReady_ = false;
};
