#include <M5Atom.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "AppSecrets.h"

// 認証情報から実行時設定へ名前を束ねる。
namespace AppConfig {

using AppSecrets::kWifiSsid;
using AppSecrets::kWifiPassword;
using AppSecrets::kPrinterHost;
using AppSecrets::kPrinterPassword;
using AppSecrets::kPrinterSerial;

constexpr char     kPrinterUser[]          = "bblp";
constexpr uint16_t kPrinterPort            = 8883;
constexpr uint32_t kWifiConnectTimeoutMs   = 20000;
constexpr uint8_t  kMqttMaxRetries         = 5;
constexpr uint32_t kMqttRetryDelayMs       = 5000;
constexpr uint32_t kActiveLoopDelayMs      = 10;
constexpr uint32_t kHaltedLoopDelayMs      = 250;

}  // namespace AppConfig

// 画面表示と通信状態をひとまとめに持つ共有状態。
struct AppState {
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

  using RenderFn = void (*)(AppState&);
  RenderFn immediateRender = nullptr;
};

inline String formatTemperature(float value) {
  return String(value, 1) + " C";
}

inline String formatPercent(int value) {
  return String(value) + "%";
}

class PrinterComm {
 public:
  PrinterComm();

  void begin(AppState& state);
  void tick(AppState& state);

 private:
  static void mqttCallback(char* topic, byte* payload, unsigned int length);

  void setupWiFi(AppState& state);
  void ensureMqtt(AppState& state);
  void onMessage(char* topic, byte* payload, unsigned int length);
  void requestPushAll(AppState& state);
  bool publishJson(AppState& state, JsonDocument& doc, const char* okEvent,
                   const char* failEvent);
  void setFatalState(AppState& state, const String& reason);
  void requestRender(AppState& state);

  static PrinterComm* instance_;

  WiFiClientSecure tlsClient_;
  PubSubClient     mqttClient_;
  AppState*        state_      = nullptr;
  uint32_t         sequenceId_ = 0;
  char topicSubscribe_[96] = {};
  char topicPublish_[96]   = {};
  char mqttClientId_[64]   = {};
};

namespace {

// PubSubClient の数値エラーを簡易ラベルに変換する。
String mqttErrorLabel(int errorCode) {
  return "ERR " + String(errorCode);
}

// プリンタの homing_status を表示用の短い文字列へ変換する。
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

// Wi-Fi と MQTT の接続準備を行う。
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
  if (!mqttClient_.setBufferSize(8192)) {
    Serial.println("MQTT buffer alloc failed, using default size");
  }
  mqttClient_.setKeepAlive(60);

  setupWiFi(state);
}

// 通信監視のメイン処理。接続維持と MQTT 受信を担当する。
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

// C 形式のコールバックからインスタンスメソッドへ渡すための中継。
void PrinterComm::mqttCallback(char* topic, byte* payload,
                               unsigned int length) {
  if (instance_ != nullptr) {
    instance_->onMessage(topic, payload, length);
  }
}

// 起動時に Wi-Fi へ接続し、表示状態も更新する。
void PrinterComm::setupWiFi(AppState& state) {
  state.wifiStatus = "CONNECT";
  state.lastEvent  = "JOIN WIFI";
  requestRender(state);

  WiFi.mode(WIFI_STA);

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

// MQTT 未接続時のみ再接続を試み、接続成功後に初回状態取得を要求する。
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

      requestPushAll(state);
      return;
    }

    lastError        = mqttClient_.state();
    state.mqttStatus = mqttErrorLabel(lastError);
    state.lastEvent  = state.mqttStatus;
    requestRender(state);

    for (uint32_t elapsed = 0; elapsed < AppConfig::kMqttRetryDelayMs;
         elapsed += 100) {
      delay(100);
    }
  }

  setFatalState(state, "MQTT CODE " + String(lastError));
}

// プリンタから届く push_status を読み取り、画面表示用の状態に反映する。
void PrinterComm::onMessage(char* topic, byte* payload, unsigned int length) {
  if (state_ == nullptr) {
    return;
  }

  Serial.print("Message on topic: ");
  Serial.println(topic);

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

  JsonObjectConst print = printValue.as<JsonObjectConst>();
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

// Bambu の全状態スナップショットを要求する。
void PrinterComm::requestPushAll(AppState& state) {
  JsonDocument doc;
  JsonObject   pushing   = doc["pushing"].to<JsonObject>();
  pushing["sequence_id"] = String(++sequenceId_);
  pushing["command"]     = "pushall";
  publishJson(state, doc, "PUSHALL SENT", "PUSHALL FAIL");
}

// JSON を MQTT publish し、結果を lastEvent に残す。
bool PrinterComm::publishJson(AppState& state, JsonDocument& doc,
                              const char* okEvent, const char* failEvent) {
  String payload;
  serializeJson(doc, payload);

  const bool published =
      mqttClient_.publish(topicPublish_, payload.c_str(), false);
  state.lastEvent = published ? okEvent : failEvent;
  requestRender(state);
  return published;
}

// 復旧不能エラー時に停止状態へ切り替える。
void PrinterComm::setFatalState(AppState& state, const String& reason) {
  state.mqttStatus  = "STOP";
  state.lastEvent   = "FATAL";
  state.errorReason = reason;
  state.halted      = true;
  requestRender(state);
  Serial.println("Fatal error: " + reason);
}

// 起動中は即時描画、本稼働後は dirty フラグだけを立てる。
void PrinterComm::requestRender(AppState& state) {
  if (state.immediateRender) {
    state.appendLog(state.lastEvent);
    state.immediateRender(state);
  }
  state.displayDirty = true;
}

namespace {

// OLED と MCP23017 のハードウェア定数。
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

// 取得した I/O 状態を OLED に表示しやすい形で保持する。
uint8_t xBits = 0;
uint8_t yBits = 0;

// Y 側は出力ポート。代入演算子で PLC 出力っぽく扱えるようにする。
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

// X 側は入力ポート。読み取り専用の簡易ラッパー。
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

// OLED の1行に収まる長さへ丸める。
String clipText(const String& text, size_t maxLen) {
  if (text.length() <= maxLen) {
    return text;
  }
  if (maxLen <= 3) {
    return text.substring(0, maxLen);
  }
  return text.substring(0, maxLen - 3) + "...";
}

// Atom の 5x5 LED マトリクスに安全に描画する。
void setDot(uint8_t x, uint8_t y, const CRGB& color) {
  if (x < 5 && y < 5) {
    M5.dis.drawpix(x, y, color);
  }
}

// MCP23017 を初期化し、前半を入力・後半を出力として使う。
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

// 現在の固定出力を PLC 側へ反映する。
void applyCurrentOutputs() {
  if (!mcpReady) {
    return;
  }

  Y0 = 0;
  Y1 = 1;
  Y2 = 0;
  Y3 = 0;
}

// MCP の入力/出力状態をビット列として読み直す。
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

// Wi-Fi 状態を LED 色へ変換する。
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

// MQTT 状態を LED 色へ変換する。
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

// "85%" のような文字列を数値へ戻す。
int progressPercent(const String& value) {
  if (!value.endsWith("%")) {
    return -1;
  }
  String digits = value;
  digits.replace("%", "");
  return constrain(digits.toInt(), 0, 100);
}

// 5x5 LED に Wi-Fi / MQTT / heartbeat / 進捗を表示する。
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

// 起動中だけ表示する簡易ログ画面。
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

// 通常運転時のダッシュボード画面。
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

// LED マトリクスと OLED をまとめて再描画する。
void renderState(AppState& state) {
  renderMatrix(state);
  if (state.immediateRender != nullptr) {
    renderStartupLog(state);
  } else {
    renderDashboard(state);
  }
  state.displayDirty = false;
}

// OLED を初期化する。
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

// MCP を初期化し、状態表示を更新する。
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

// 起動時に表示系と通信系を順番に立ち上げる。
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

// 周期処理。I/O 更新、通信処理、必要時のみ再描画を行う。
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
