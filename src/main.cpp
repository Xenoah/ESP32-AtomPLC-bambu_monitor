#include <M5Atom.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

Adafruit_SSD1306 display(128, 64, &Wire, -1);

Adafruit_MCP23X17 mcp;
bool mcp_ready = false;

// ====== PLC風プロキシ ======
// Y: 出力 (Bポート GPB0..7, index=8..15)
struct YRef {
  uint8_t idx;
  YRef(uint8_t i): idx(i) {}
  YRef& operator=(int v){
    mcp.digitalWrite(8 + idx, v ? HIGH : LOW);
    return *this;
  }
  operator int() const {
    return (mcp.digitalRead(8 + idx) == HIGH) ? 1 : 0;
  }
};

// X: 入力 (Aポート GPA0..7, index=0..7) ※LOWで1（プルアップ前提）
struct XRef {
  uint8_t idx;
  XRef(uint8_t i): idx(i) {}
  operator int() const {
    return (mcp.digitalRead(idx) == LOW) ? 1 : 0;
  }
};

inline XRef X(uint8_t i){ return XRef(i); }
inline YRef Y(uint8_t i){ return YRef(i); }

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

// ===== LEDユーティリティ =====
static const CRGB COL_OFF = CRGB(0,0,0);

void fillAll(const CRGB& c){
  for(uint8_t y=0;y<5;y++) for(uint8_t x=0;x<5;x++) M5.dis.drawpix(x,y,c);
}
void setDot(uint8_t x,uint8_t y,const CRGB& c){ if(x<5&&y<5) M5.dis.drawpix(x,y,c); }

void drawX(uint8_t bits){
  for(uint8_t n=0;n<8;n++){
    uint8_t col=(n<4)?0:1, row=(n%4);
    setDot(col,row, (bits>>n)&1 ? CRGB(0,80,0) : CRGB(3,3,3));
  }
}
void drawY(uint8_t bits){
  for(uint8_t n=0;n<8;n++){
    uint8_t col=(n<4)?3:4, row=(n%4);
    setDot(col,row, (bits>>n)&1 ? CRGB(80,0,0) : CRGB(3,3,3));
  }
}

bool try_init_mcp(){
  if(!mcp.begin_I2C(0x20, &Wire)) return false;

  // Bポート: 出力 (Y)
  for(uint8_t i=0;i<8;i++){
    mcp.pinMode(8+i, OUTPUT);
    mcp.digitalWrite(8+i, LOW);
  }
  // Aポート: 入力+プルアップ (X)
  for(uint8_t i=0;i<8;i++){
    mcp.pinMode(i, INPUT_PULLUP);
  }
  return true;
}

static void oledPrintBits8(uint8_t v){
  for(int i=7;i>=0;i--) display.print((v >> i) & 1);
}

void setup(){
  M5.begin(true,false,true);
  M5.dis.setBrightness(20);
  M5.dis.clear();

  // あなたの環境でI2CスキャンOKだったピン
  Wire.begin(25,21); // SDA=25, SCL=21

  // ==== OLED初期化 ====
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)){
    fillAll(CRGB(80,0,0));
    while(1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println(F("initialising..."));
  display.display();

  // ==== MCP認識ループ ====
  while(true){
    if(try_init_mcp()){
      mcp_ready = true;
      fillAll(CRGB(0,80,0));    // 成功 → 全面緑
      delay(500);
      M5.dis.clear();

      display.clearDisplay();
      display.setCursor(0,0);
      display.println(F("MCP23017 OK"));
      display.display();
      break;
    }else{
      static bool on=false;
      on=!on;
      fillAll(on?CRGB(80,0,0):COL_OFF);  // 失敗 → 全面赤点滅

      display.clearDisplay();
      display.setCursor(0,0);
      display.println(F("MCP23017 ERROR"));
      display.display();
      delay(500);
    }
  }
}

void loop(){
  M5.update();

  // =========================================================
  // Y0..Y3: 4bitカウンタ（Y0が0ビット目）
  // 0..9を1秒刻みでカウント、10で0にリセット
  // =========================================================
  static uint32_t last_update = 0;
  static uint8_t counter = 0;

  if (millis() - last_update >= 1000) {
    last_update = millis();
    counter++;
    if (counter >= 10) counter = 0;
  }

  Y0 = (counter >> 0) & 1;
  Y1 = (counter >> 1) & 1;
  Y2 = (counter >> 2) & 1;
  Y3 = (counter >> 3) & 1;

  // =========================================================
  // === 表示更新（X/Yビット収集）===
  // =========================================================
  uint8_t xbits=0, ybits=0;
  for(uint8_t i=0;i<8;i++){
    int xi = X(i);        // 入力
    xbits |= (xi&1) << i;
    int yi = Y(i);        // 出力状態
    ybits |= (yi&1) << i;
  }

  // M5 5x5 LED表示
  M5.dis.clear();
  drawX(xbits);
  drawY(ybits);

  // 中央列: セパレータ＆ハートビート
  static uint32_t t0=millis();
  bool blink=((millis()-t0)/300)%2;
  for(uint8_t r=0;r<4;r++) setDot(2,r, blink?CRGB(0,0,50):CRGB(3,3,3));
  setDot(0,4,CRGB(0,80,0)); setDot(1,4,CRGB(0,80,0));
  setDot(3,4,CRGB(80,0,0)); setDot(4,4,CRGB(80,0,0));

  // OLED表示（ゼロサプレスなし＝常に8bit表示）
  display.clearDisplay();
  display.setCursor(0,0);

  display.print(F("X="));
  oledPrintBits8(xbits);
  display.println();

  display.print(F("Y="));
  oledPrintBits8(ybits);
  display.println();

  display.print(F("CNT="));
  display.println(counter);

  display.print(F("ms="));
  display.println(millis());

  display.display();

  delay(10);
}