#include <M5Atom.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "AppConfig.h"
#include "AppState.h"
#include "PrinterComm.h"

namespace {

constexpr uint8_t kOledWidth   = 128;
constexpr uint8_t kOledHeight  = 64;
constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kSdaPin      = 25;
constexpr uint8_t kSclPin      = 21;
constexpr uint8_t kMaxLineLen  = 21;

Adafruit_SSD1306 display(kOledWidth, kOledHeight, &Wire, -1);
AppState         appState;
PrinterComm      printerComm;
bool             oledReady = false;

String clipText(const String& text, size_t maxLen) {
  if (text.length() <= maxLen) {
    return text;
  }
  if (maxLen <= 3) {
    return text.substring(0, maxLen);
  }
  return text.substring(0, maxLen - 3) + "...";
}

CRGB wifiColor(const AppState& state) {
  if (state.halted) {
    return CRGB(80, 0, 0);
  }
  if (state.wifiStatus == "OK") {
    return CRGB(0, 80, 0);
  }
  if (state.wifiStatus == "CONNECT" || state.wifiStatus == "BOOT") {
    return CRGB(0, 0, 80);
  }
  return CRGB(80, 40, 0);
}

CRGB mqttColor(const AppState& state) {
  if (state.halted || state.mqttStatus == "STOP" ||
      state.mqttStatus.startsWith("ERR")) {
    return CRGB(80, 0, 0);
  }
  if (state.mqttStatus == "OK") {
    return CRGB(0, 80, 0);
  }
  if (state.mqttStatus == "TRY") {
    return CRGB(80, 50, 0);
  }
  return CRGB(20, 20, 20);
}

void setDot(uint8_t x, uint8_t y, const CRGB& color) {
  if (x < 5 && y < 5) {
    M5.dis.drawpix(x, y, color);
  }
}

int progressPercent(const String& value) {
  if (!value.endsWith("%")) {
    return -1;
  }
  String digits = value;
  digits.replace("%", "");
  return constrain(digits.toInt(), 0, 100);
}

void renderMatrix(const AppState& state) {
  M5.dis.clear();

  const CRGB leftColor  = wifiColor(state);
  const CRGB rightColor = mqttColor(state);
  for (uint8_t y = 0; y < 4; ++y) {
    setDot(0, y, leftColor);
    setDot(1, y, leftColor);
    setDot(3, y, rightColor);
    setDot(4, y, rightColor);
  }

  const bool heartbeatOn = ((millis() / 300) % 2) != 0;
  for (uint8_t y = 0; y < 4; ++y) {
    setDot(2, y, heartbeatOn ? CRGB(0, 0, 40) : CRGB(2, 2, 2));
  }

  const int progress = progressPercent(state.progress);
  const uint8_t litDots =
      progress < 0 ? 0 : static_cast<uint8_t>((progress + 19) / 20);
  for (uint8_t x = 0; x < 5; ++x) {
    const bool lit = x < litDots;
    setDot(x, 4, lit ? CRGB(0, 50, 50) : CRGB(2, 2, 2));
  }
}

void printLine(const String& text) {
  display.println(clipText(text, kMaxLineLen));
}

void renderStartupLog(AppState& state) {
  if (!oledReady) {
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  printLine("Bambu startup");

  const uint8_t visibleLines = 7;
  const uint8_t start =
      state.logCount > visibleLines ? state.logCount - visibleLines : 0;
  for (uint8_t i = start; i < state.logCount; ++i) {
    printLine(state.logLines[i]);
  }
  display.display();
}

void renderDashboard(AppState& state) {
  if (!oledReady) {
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  printLine("Bambu monitor");
  printLine("W:" + state.wifiStatus + " " + state.ipAddress);
  printLine("M:" + state.mqttStatus + " " + state.lastEvent);
  printLine("B:" + state.bedTemp + " N:" + state.nozzleTemp);
  printLine("P:" + state.progress + " L:" + state.layer);
  printLine("S:" + state.printState);
  printLine("PW:" + state.printerWifi + " SQ:" + state.sequenceId);
  printLine(state.halted ? "ERR:" + state.errorReason : "EV:" + state.lastEvent);
  display.display();
}

void renderState(AppState& state) {
  renderMatrix(state);
  if (state.immediateRender != nullptr) {
    renderStartupLog(state);
  } else {
    renderDashboard(state);
  }
  state.displayDirty = false;
}

void initDisplay() {
  Wire.begin(kSdaPin, kSclPin);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, kOledAddress);
  if (!oledReady) {
    Serial.println("OLED init failed");
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setTextWrap(false);
  display.display();
}

}  // namespace

void setup() {
  M5.begin(true, false, true);
  M5.dis.setBrightness(20);
  M5.dis.clear();

  initDisplay();

  appState.appendLog("BOOT");
  appState.immediateRender = renderState;
  renderState(appState);

  printerComm.begin(appState);

  appState.immediateRender = nullptr;
  appState.displayDirty    = true;
  renderState(appState);
}

void loop() {
  M5.update();

  printerComm.tick(appState);
  if (appState.displayDirty) {
    renderState(appState);
  }

  delay(appState.halted ? AppConfig::kHaltedLoopDelayMs
                        : AppConfig::kActiveLoopDelayMs);
}
