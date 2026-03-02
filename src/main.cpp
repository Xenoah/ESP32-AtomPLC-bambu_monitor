#include <M5Atom.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
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
Adafruit_MCP23X17 mcp;
AppState         appState;
PrinterComm      printerComm;

bool oledReady = false;
bool mcpReady  = false;

uint8_t xBits = 0;
uint8_t yBits = 0;

struct YRef {
  uint8_t idx;
  explicit YRef(uint8_t i) : idx(i) {}

  YRef& operator=(int value) {
    if (mcpReady) {
      mcp.digitalWrite(8 + idx, value ? HIGH : LOW);
    }
    return *this;
  }

  operator int() const {
    return mcpReady && mcp.digitalRead(8 + idx) == HIGH ? 1 : 0;
  }
};

struct XRef {
  uint8_t idx;
  explicit XRef(uint8_t i) : idx(i) {}

  operator int() const {
    return mcpReady && mcp.digitalRead(idx) == LOW ? 1 : 0;
  }
};

inline XRef X(uint8_t i) { return XRef(i); }
inline YRef Y(uint8_t i) { return YRef(i); }

#define X0 X(0)
#define X1 X(1)
#define X2 X(2)
#define X3 X(3)
#define X4 X(4)
#define X5 X(5)
#define X6 X(6)
#define X7 X(7)

#define Y0 Y(0)
#define Y1 Y(1)
#define Y2 Y(2)
#define Y3 Y(3)
#define Y4 Y(4)
#define Y5 Y(5)
#define Y6 Y(6)
#define Y7 Y(7)

String clipText(const String& text, size_t maxLen) {
  if (text.length() <= maxLen) {
    return text;
  }
  if (maxLen <= 3) {
    return text.substring(0, maxLen);
  }
  return text.substring(0, maxLen - 3) + "...";
}

void setDot(uint8_t x, uint8_t y, const CRGB& color) {
  if (x < 5 && y < 5) {
    M5.dis.drawpix(x, y, color);
  }
}

bool tryInitMcp() {
  if (!mcp.begin_I2C(0x20, &Wire)) {
    return false;
  }

  for (uint8_t i = 0; i < 8; ++i) {
    mcp.pinMode(8 + i, OUTPUT);
    mcp.digitalWrite(8 + i, LOW);
  }
  for (uint8_t i = 0; i < 8; ++i) {
    mcp.pinMode(i, INPUT_PULLUP);
  }
  return true;
}

void applyCurrentOutputs() {
  if (!mcpReady) {
    return;
  }

  Y0 = 0;
  Y1 = 1;
  Y2 = 0;
  Y3 = 0;
}

void sampleMcpState() {
  xBits = 0;
  yBits = 0;
  if (!mcpReady) {
    return;
  }

  for (uint8_t i = 0; i < 8; ++i) {
    xBits |= (static_cast<uint8_t>(X(i)) & 1U) << i;
    yBits |= (static_cast<uint8_t>(Y(i)) & 1U) << i;
  }
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
    setDot(x, 4, x < litDots ? CRGB(0, 50, 50) : CRGB(2, 2, 2));
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
  printLine(String("IO:") + (mcpReady ? "OK " : "WAIT ") + "X=" + String(xBits, BIN));
  printLine("B:" + state.bedTemp + " N:" + state.nozzleTemp);
  printLine("P:" + state.progress + " L:" + state.layer);
  printLine("S:" + state.printState + " Y=" + String(yBits, BIN));
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

void initMcp() {
  mcpReady = tryInitMcp();
  if (!mcpReady) {
    appState.lastEvent = "MCP FAIL";
    return;
  }

  appState.lastEvent = "MCP READY";
  sampleMcpState();
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

  initMcp();
  printerComm.begin(appState);

  appState.immediateRender = nullptr;
  appState.displayDirty    = true;
  renderState(appState);
}

void loop() {
  M5.update();

  applyCurrentOutputs();
  sampleMcpState();
  printerComm.tick(appState);
  if (appState.displayDirty) {
    renderState(appState);
  }

  delay(appState.halted ? AppConfig::kHaltedLoopDelayMs
                        : AppConfig::kActiveLoopDelayMs);
}
