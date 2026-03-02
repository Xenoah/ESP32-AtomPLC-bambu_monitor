#include "DisplayManager.h"

#include "AppConfig.h"

namespace {

constexpr uint16_t kColorBlack  = 0x0000;
constexpr uint16_t kColorWhite  = 0xFFFF;
constexpr uint16_t kColorRed    = 0xD104;
constexpr uint16_t kColorBlue   = 0x041F;
constexpr uint16_t kColorCyan   = 0x07FF;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kColorOrange = 0xFD20;
constexpr uint16_t kColorGreen  = 0x07E0;
constexpr uint16_t kColorGray   = 0x8410;

}  // namespace

bool DisplayManager::begin(AppState& state) {
  // Check init() return value — if hardware is absent or SPI config is wrong
  // this returns false and we must not proceed to draw.
  ready_ = lcd_.init();
  if (!ready_) {
    Serial.println("Display init failed!");
    return false;
  }

  lcd_.setRotation(1);
  lcd_.setBrightness(255);
  lcd_.setTextWrap(false);

  // Allocate a full-screen 16-bit sprite for flicker-free rendering.
  // Requires PSRAM (available on ESP32-S3-BOX-Lite via Octal SPI).
  sprite_.setColorDepth(16);
  spriteReady_ = sprite_.createSprite(lcd_.width(), lcd_.height());
  if (!spriteReady_) {
    Serial.println("Sprite alloc failed — using direct LCD draw (may flicker)");
  }

  // Splash screen drawn once directly to LCD (not through sprite).
  lcd_.fillScreen(kColorBlack);
  lcd_.setTextSize(2);
  lcd_.setCursor(16, 24);
  lcd_.setTextColor(kColorWhite, kColorBlack);
  lcd_.print("BAMBU MQTT");
  lcd_.setTextSize(1);
  lcd_.setCursor(16, 56);
  lcd_.setTextColor(kColorCyan, kColorBlack);
  lcd_.print("ESP32-S3-BOX-LITE");
  delay(800);

  state.displayDirty = true;
  return true;
}

void DisplayManager::render(const AppState& state) {
  if (!ready_) {
    return;
  }

  if (state.halted) {
    renderFatalScreen(state);
    return;
  }

  renderDashboard(state);
}

void DisplayManager::renderDashboard(const AppState& state) {
  // Draw to sprite when available; fall back to LCD directly.
  lgfx::LovyanGFX& c =
      spriteReady_ ? static_cast<lgfx::LovyanGFX&>(sprite_)
                   : static_cast<lgfx::LovyanGFX&>(lcd_);

  uint16_t statusColor = kColorOrange;
  if (state.mqttStatus == "OK") {
    statusColor = kColorGreen;
  } else if (state.mqttStatus.startsWith("ERR") || state.mqttStatus == "STOP") {
    statusColor = kColorRed;
  }

  c.setTextSize(1);
  c.setTextWrap(false);
  c.fillScreen(kColorBlack);
  c.fillRect(0, 0, lcd_.width(), 20, statusColor);
  drawText(c, 8, 6, "BAMBU MQTT", kColorBlack, statusColor);
  drawText(c, lcd_.width() - 72, 6, state.mqttStatus, kColorBlack, statusColor);

  drawField(c, 28, "WIFI",   state.wifiStatus,   kColorWhite);
  drawField(c, 42, "IP",     state.ipAddress,    kColorWhite);
  drawField(c, 56, "MQTT",   state.lastEvent,    kColorWhite);

  c.drawFastHLine(8, 72, lcd_.width() - 16, kColorGray);

  drawField(c,  82, "BED",    state.bedTemp,      kColorYellow);
  drawField(c,  96, "NOZZLE", state.nozzleTemp,   kColorOrange);
  drawField(c, 110, "P_WIFI", state.printerWifi,  kColorWhite);
  drawField(c, 124, "PROG",   state.progress,     kColorGreen);
  drawField(c, 138, "LAYER",  state.layer,        kColorWhite);
  drawField(c, 152, "STATE",  state.printState,   kColorWhite);
  drawField(c, 166, "HOMING", state.homingStatus, kColorWhite);
  drawField(c, 180, "SEQ",    state.sequenceId,   kColorWhite);

  c.fillRect(0, 198, lcd_.width(), lcd_.height() - 198, kColorBlue);
  drawText(c,  8, 206, "PRINTER",                        kColorWhite, kColorBlue);
  drawText(c, 78, 206, AppConfig::kPrinterSerial,        kColorWhite, kColorBlue);
  drawText(c,  8, 220, "UPTIME",                         kColorWhite, kColorBlue);
  drawText(c, 78, 220, String(millis() / 1000UL) + "s",  kColorWhite, kColorBlue);

  // Push the completed frame to the screen in one atomic operation.
  // This eliminates the fill→draw flicker visible with direct rendering.
  if (spriteReady_) {
    sprite_.pushSprite(0, 0);
  }
}

void DisplayManager::renderFatalScreen(const AppState& state) {
  lgfx::LovyanGFX& c =
      spriteReady_ ? static_cast<lgfx::LovyanGFX&>(sprite_)
                   : static_cast<lgfx::LovyanGFX&>(lcd_);

  c.setTextWrap(false);
  c.fillScreen(kColorRed);
  c.setTextSize(2);

  // Show a title that matches the actual error type.
  const char* title = state.errorReason.startsWith("WIFI") ? "WIFI ERROR" : "MQTT ERROR";
  drawText(c, 12, 20, title, kColorWhite, kColorRed);
  c.drawFastHLine(12, 52, lcd_.width() - 24, kColorWhite);

  c.setTextSize(1);
  drawText(c, 12,  68, "REASON",               kColorYellow, kColorRed);
  drawText(c, 12,  84, state.errorReason,       kColorWhite,  kColorRed);
  drawText(c, 12, 112, "WIFI",                  kColorYellow, kColorRed);
  drawText(c, 80, 112, state.wifiStatus,        kColorWhite,  kColorRed);
  drawText(c, 12, 128, "IP",                    kColorYellow, kColorRed);
  drawText(c, 80, 128, state.ipAddress,         kColorWhite,  kColorRed);
  drawText(c, 12, 144, "MQTT",                  kColorYellow, kColorRed);
  drawText(c, 80, 144, state.lastEvent,         kColorWhite,  kColorRed);
  drawText(c, 12, 204, "FIX SETTINGS AND RESET", kColorWhite, kColorRed);

  if (spriteReady_) {
    sprite_.pushSprite(0, 0);
  }
}

void DisplayManager::renderStartup(const AppState& state) {
  if (!ready_) {
    return;
  }

  lgfx::LovyanGFX& c =
      spriteReady_ ? static_cast<lgfx::LovyanGFX&>(sprite_)
                   : static_cast<lgfx::LovyanGFX&>(lcd_);

  c.setTextWrap(false);
  c.fillScreen(kColorBlack);

  // ── Header (same layout as the splash screen) ───────────────────────────
  c.setTextSize(2);
  drawText(c, 16, 24, "BAMBU MQTT", kColorWhite, kColorBlack);
  c.setTextSize(1);
  drawText(c, 16, 56, "ESP32-S3-BOX-LITE", kColorCyan, kColorBlack);
  c.drawFastHLine(8, 70, lcd_.width() - 16, kColorGray);

  // ── Terminal log area ───────────────────────────────────────────────────
  // Each line: green ">" prompt + message text.
  // The most-recent entry is white; older entries are dimmed to gray.
  constexpr int16_t kLogStartY = 78;
  constexpr int16_t kLineH     = 10;  // 8px char + 2px gap

  c.setTextSize(1);
  for (uint8_t i = 0; i < state.logCount; ++i) {
    const int16_t y = kLogStartY + static_cast<int16_t>(i) * kLineH;
    if (y + 8 > lcd_.height()) {
      break;
    }
    const bool    isLatest   = (i == state.logCount - 1);
    const uint16_t textColor = isLatest ? kColorWhite : kColorGray;
    drawText(c,  4, y, ">",                  kColorGreen, kColorBlack);
    drawText(c, 14, y, state.logLines[i],    textColor,   kColorBlack);
  }

  if (spriteReady_) {
    sprite_.pushSprite(0, 0);
  }
}

void DisplayManager::drawField(lgfx::LovyanGFX& c, int16_t y, const char* label,
                               const String& value, uint16_t valueColor) {
  drawText(c,  8, y, label, kColorCyan,  kColorBlack);
  drawText(c, 96, y, value, valueColor,  kColorBlack);
}

void DisplayManager::drawText(lgfx::LovyanGFX& c, int16_t x, int16_t y,
                              const String& text, uint16_t fg, uint16_t bg) {
  c.setCursor(x, y);
  c.setTextColor(fg, bg);
  c.print(text);
}

void DisplayManager::drawText(lgfx::LovyanGFX& c, int16_t x, int16_t y,
                              const char* text, uint16_t fg, uint16_t bg) {
  c.setCursor(x, y);
  c.setTextColor(fg, bg);
  c.print(text);
}
