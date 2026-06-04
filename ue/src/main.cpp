#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>

#include "tank_assets/sprite_alert.h"
#include "tank_assets/sprite_beacon.h"
#include "tank_assets/sprite_building.h"
#include "tank_assets/sprite_conducting.h"
#include "tank_assets/sprite_confused.h"
#include "tank_assets/sprite_debugger.h"
#include "tank_assets/sprite_disconnected.h"
#include "tank_assets/sprite_dizzy.h"
#include "tank_assets/sprite_going_away.h"
#include "tank_assets/sprite_happy.h"
#include "tank_assets/sprite_idle.h"
#include "tank_assets/sprite_juggling.h"
#include "tank_assets/sprite_sleeping.h"
#include "tank_assets/sprite_sweeping.h"
#include "tank_assets/sprite_thinking.h"
#include "tank_assets/sprite_typing.h"
#include "tank_assets/sprite_walking.h"
#include "tank_assets/sprite_wizard.h"

namespace {
constexpr int TFT_WIDTH = 240;
constexpr int TFT_HEIGHT = 240;

constexpr int TFT_SCLK = 22;
constexpr int TFT_MOSI = 21;
constexpr int TFT_RST = 17;
constexpr int TFT_DC = 16;
constexpr int TFT_CS = 18;
constexpr int TFT_BL = 26;

constexpr uint16_t TRANSPARENT = 0x18C5;
constexpr bool CRISP_EDGES = true;
constexpr bool CLAWD_ORANGE_PALETTE = true;
constexpr uint8_t RENDER_SCALE = 2;

constexpr char AP_SSID[] = "Clawd-Mochi-Tank";
constexpr char AP_PASS[] = "clawd1234";
constexpr char BLE_NAME[] = "Claude-Mochi-Tank";
constexpr char BLE_SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr uint32_t BLE_DISCONNECT_BEACON_GRACE_MS = 10000;
constexpr uint32_t COMMAND_IDLE_TIMEOUT_MS = 30000;

struct TankAnim {
  const char *id;
  const char *label;
  const uint16_t *rle;
  const uint32_t *offsets;
  uint16_t width;
  uint16_t height;
  uint16_t frameCount;
  uint16_t fps;
};

const TankAnim ANIMS[] = {
    {"idle", "Mochi idle", idle_rle_data, idle_frame_offsets, IDLE_WIDTH, IDLE_HEIGHT, IDLE_FRAME_COUNT, 12},
    {"typing", "Claude typing", typing_rle_data, typing_frame_offsets, TYPING_WIDTH, TYPING_HEIGHT, TYPING_FRAME_COUNT, 10},
    {"thinking", "Thinking", thinking_rle_data, thinking_frame_offsets, THINKING_WIDTH, THINKING_HEIGHT, THINKING_FRAME_COUNT, 8},
    {"building", "Building", building_rle_data, building_frame_offsets, BUILDING_WIDTH, BUILDING_HEIGHT, BUILDING_FRAME_COUNT, 8},
    {"juggling", "Juggling", juggling_rle_data, juggling_frame_offsets, JUGGLING_WIDTH, JUGGLING_HEIGHT, JUGGLING_FRAME_COUNT, 8},
    {"conducting", "Conducting", conducting_rle_data, conducting_frame_offsets, CONDUCTING_WIDTH, CONDUCTING_HEIGHT, CONDUCTING_FRAME_COUNT, 8},
    {"debugger", "Debugger", debugger_rle_data, debugger_frame_offsets, DEBUGGER_WIDTH, DEBUGGER_HEIGHT, DEBUGGER_FRAME_COUNT, 8},
    {"wizard", "Wizard", wizard_rle_data, wizard_frame_offsets, WIZARD_WIDTH, WIZARD_HEIGHT, WIZARD_FRAME_COUNT, 8},
    {"beacon", "Beacon", beacon_rle_data, beacon_frame_offsets, BEACON_WIDTH, BEACON_HEIGHT, BEACON_FRAME_COUNT, 8},
    {"confused", "Confused", confused_rle_data, confused_frame_offsets, CONFUSED_WIDTH, CONFUSED_HEIGHT, CONFUSED_FRAME_COUNT, 8},
    {"sweeping", "Sweeping", sweeping_rle_data, sweeping_frame_offsets, SWEEPING_WIDTH, SWEEPING_HEIGHT, SWEEPING_FRAME_COUNT, 8},
    {"walking", "Walking", walking_rle_data, walking_frame_offsets, WALKING_WIDTH, WALKING_HEIGHT, WALKING_FRAME_COUNT, 8},
    {"going_away", "Going away", going_away_rle_data, going_away_frame_offsets, GOING_AWAY_WIDTH, GOING_AWAY_HEIGHT, GOING_AWAY_FRAME_COUNT, 8},
    {"alert", "Needs attention", alert_rle_data, alert_frame_offsets, ALERT_WIDTH, ALERT_HEIGHT, ALERT_FRAME_COUNT, 10},
    {"happy", "Mochi happy", happy_rle_data, happy_frame_offsets, HAPPY_WIDTH, HAPPY_HEIGHT, HAPPY_FRAME_COUNT, 10},
    {"sleeping", "Sleeping", sleeping_rle_data, sleeping_frame_offsets, SLEEPING_WIDTH, SLEEPING_HEIGHT, SLEEPING_FRAME_COUNT, 8},
    {"dizzy", "Dizzy", dizzy_rle_data, dizzy_frame_offsets, DIZZY_WIDTH, DIZZY_HEIGHT, DIZZY_FRAME_COUNT, 8},
    {"disconnected", "Disconnected", disconnected_rle_data, disconnected_frame_offsets, DISCONNECTED_WIDTH, DISCONNECTED_HEIGHT, DISCONNECTED_FRAME_COUNT, 8},
};
constexpr uint8_t ANIM_COUNT = sizeof(ANIMS) / sizeof(ANIMS[0]);

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
BLEServer *bleServer = nullptr;
BLECharacteristic *bleTxCharacteristic = nullptr;

uint8_t animIndex = 0;
uint16_t frameIndex = 0;
uint32_t nextFrameMs = 0;
bool autoCycle = true;
bool backlightOn = true;
uint8_t speedLevel = 2;
uint32_t nextAutoCycleMs = 0;
uint16_t lineBuffer[TFT_WIDTH];
uint32_t lineHashes[TFT_HEIGHT];
String serialLine;
String bleLine;
portMUX_TYPE bleCmdMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool blePendingCmd = false;
char blePendingBuf[241];
bool bleConnected = false;
bool bleWasConnected = false;
uint32_t lastBleCommandMs = 0;
uint32_t lastCommandMs = 0;
bool commandIdleApplied = false;
bool lastCommandWasBle = false;
String lastSource = "boot";

String stateJson();
bool applyCommandLine(String line, const char *source);

uint8_t renderScale(const TankAnim &) {
  return RENDER_SCALE;
}

int8_t findAnimIndex(const String &id) {
  for (uint8_t i = 0; i < ANIM_COUNT; ++i) {
    if (id == ANIMS[i].id) return i;
  }
  return -1;
}

void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(TFT_BL, on ? HIGH : LOW);
}

void printWiring() {
  Serial.println();
  Serial.println("Clawd Mochi Tank");
  Serial.println("VCC -> 3V3");
  Serial.println("GND -> GND");
  Serial.println("SCK -> GPIO22");
  Serial.println("MOSI -> GPIO21");
  Serial.println("RST -> GPIO17");
  Serial.println("DC  -> GPIO16");
  Serial.println("CS  -> GPIO18");
  Serial.println("BL  -> GPIO26");
}

void hardResetPanel() {
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_RST, HIGH);
  delay(50);
  digitalWrite(TFT_RST, LOW);
  delay(120);
  digitalWrite(TFT_RST, HIGH);
  delay(120);
}

uint8_t r5(uint16_t c) { return (c >> 11) & 0x1F; }
uint8_t g6(uint16_t c) { return (c >> 5) & 0x3F; }
uint8_t b5(uint16_t c) { return c & 0x1F; }
uint8_t r8(uint16_t c) { return (r5(c) << 3) | (r5(c) >> 2); }
uint8_t g8(uint16_t c) { return (g6(c) << 2) | (g6(c) >> 4); }
uint8_t b8(uint16_t c) { return (b5(c) << 3) | (b5(c) >> 2); }
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

bool isMatteEdge(uint16_t color) {
  if (color == TRANSPARENT) return true;
  if (!CRISP_EDGES) return false;

  const int dr = abs(static_cast<int>(r5(color)) - static_cast<int>(r5(TRANSPARENT)));
  const int dg = abs(static_cast<int>(g6(color)) - static_cast<int>(g6(TRANSPARENT)));
  const int db = abs(static_cast<int>(b5(color)) - static_cast<int>(b5(TRANSPARENT)));
  const int brightness = r5(color) * 2 + g6(color) + b5(color) * 2;

  return brightness < 42 && dr + dg + db < 18;
}

bool isClawdOrangePixel(uint16_t color) {
  const uint8_t r = r8(color);
  const uint8_t g = g8(color);
  const uint8_t b = b8(color);

  return r > 110 && g > 35 && g < 185 && b < 150 && r > g + 20 && r > b + 45;
}

uint16_t clawdOrange(uint16_t color) {
  const uint8_t r = r8(color);
  const uint8_t g = g8(color);
  const uint8_t b = b8(color);
  const uint16_t lum = r * 3 + g * 2 + b;

  if (lum < 430) return rgb565(92, 34, 12);     // hard shadow
  if (lum < 560) return rgb565(154, 57, 18);    // dark orange
  if (lum < 690) return rgb565(218, 82, 24);    // Claude orange
  return rgb565(242, 128, 46);                  // highlight
}

uint16_t displayColor(uint16_t color) {
  if (isMatteEdge(color)) return ST77XX_BLACK;
  if (CLAWD_ORANGE_PALETTE && isClawdOrangePixel(color)) {
    return clawdOrange(color);
  }
  return color;
}

void clearLineBuffer() {
  for (uint16_t px = 0; px < TFT_WIDTH; ++px) {
    lineBuffer[px] = ST77XX_BLACK;
  }
}

uint32_t hashLineRange(uint16_t x, uint16_t w) {
  uint32_t hash = 2166136261UL;
  for (uint16_t i = 0; i < w; ++i) {
    const uint16_t color = lineBuffer[x + i];
    hash ^= color & 0xFF;
    hash *= 16777619UL;
    hash ^= color >> 8;
    hash *= 16777619UL;
  }
  return hash;
}

void invalidateLineCache() {
  for (uint16_t y = 0; y < TFT_HEIGHT; ++y) {
    lineHashes[y] = 0xFFFFFFFFUL;
  }
}

void drawSpriteFrame(const TankAnim &anim, uint16_t frame) {
  const uint8_t scale = renderScale(anim);

  const int16_t outW = anim.width * scale;
  const int16_t outH = anim.height * scale;
  const int16_t x0 = (TFT_WIDTH - outW) / 2;
  const int16_t y0 = (TFT_HEIGHT - outH) / 2;
  const int16_t visibleX0 = max<int16_t>(0, x0);
  const int16_t visibleX1 = min<int16_t>(TFT_WIDTH, x0 + outW);
  const int16_t visibleW = visibleX1 - visibleX0;
  if (visibleW <= 0 || y0 >= TFT_HEIGHT || y0 + outH <= 0) return;

  const uint16_t *rle = anim.rle + anim.offsets[frame];
  const uint32_t end = anim.offsets[frame + 1];
  const uint32_t pairs = (end - anim.offsets[frame]) / 2;
  uint32_t pos = 0;
  int16_t bufferedY = -1;

  auto flushLine = [&]() {
    if (bufferedY < 0) return;
    const int16_t drawY = y0 + bufferedY * scale;
    for (uint8_t sy = 0; sy < scale; ++sy) {
      const int16_t screenY = drawY + sy;
      if (screenY >= 0 && screenY < TFT_HEIGHT) {
        const uint32_t hash = hashLineRange(visibleX0, visibleW);
        if (lineHashes[screenY] != hash) {
          tft.drawRGBBitmap(visibleX0, screenY, lineBuffer + visibleX0, visibleW, 1);
          lineHashes[screenY] = hash;
        }
      }
    }
  };

  for (uint32_t i = 0; i < pairs; ++i) {
    const uint16_t color = *rle++;
    uint16_t count = *rle++;

    while (count > 0) {
      const uint16_t x = pos % anim.width;
      const uint16_t y = pos / anim.width;
      const uint16_t span = min<uint16_t>(count, anim.width - x);
      if (bufferedY != static_cast<int16_t>(y)) {
        flushLine();
        bufferedY = y;
        clearLineBuffer();
      }

      const uint16_t drawColor = displayColor(color);
      const int16_t dstStart = x0 + x * scale;
      const int16_t dstEnd = x0 + (x + span) * scale;
      const int16_t clipStart = max<int16_t>(0, dstStart);
      const int16_t clipEnd = min<int16_t>(TFT_WIDTH, dstEnd);
      for (int16_t px = clipStart; px < clipEnd; ++px) {
        lineBuffer[px] = drawColor;
      }

      pos += span;
      count -= span;
    }
  }

  flushLine();
}

void setAnim(uint8_t index, bool keepAuto = false) {
  if (index >= ANIM_COUNT) return;
  animIndex = index;
  frameIndex = 0;
  nextFrameMs = 0;
  if (!keepAuto) autoCycle = false;
  tft.fillScreen(ST77XX_BLACK);
  invalidateLineCache();
  drawSpriteFrame(ANIMS[animIndex], frameIndex);
}

bool setAnimById(const String &id, bool keepAuto = false) {
  const int8_t index = findAnimIndex(id);
  if (index < 0) return false;
  setAnim(index, keepAuto);
  return true;
}

String stateJson() {
  String json = "{\"anim\":\"";
  json += ANIMS[animIndex].id;
  json += "\",\"label\":\"";
  json += ANIMS[animIndex].label;
  json += "\",\"auto\":";
  json += autoCycle ? "true" : "false";
  json += ",\"speed\":";
  json += speedLevel;
  json += ",\"backlight\":";
  json += backlightOn ? "true" : "false";
  json += ",\"scale\":";
  json += renderScale(ANIMS[animIndex]);
  json += ",\"ble\":";
  json += bleConnected ? "true" : "false";
  json += ",\"last_source\":\"";
  json += lastSource;
  json += "\",\"last_ms\":";
  json += lastCommandMs;
  json += "}";
  return json;
}

void notifyBleState() {
  if (!bleConnected || bleTxCharacteristic == nullptr) return;
  String line = stateJson();
  line += "\n";
  bleTxCharacteristic->setValue(reinterpret_cast<uint8_t *>(const_cast<char *>(line.c_str())), line.length());
  bleTxCharacteristic->notify();
}

String jsonValue(const String &line, const char *key) {
  String pattern = "\"";
  pattern += key;
  pattern += "\"";
  int pos = line.indexOf(pattern);
  if (pos < 0) return "";
  pos = line.indexOf(':', pos + pattern.length());
  if (pos < 0) return "";
  ++pos;
  while (pos < static_cast<int>(line.length()) && isspace(line[pos])) ++pos;

  if (pos < static_cast<int>(line.length()) && line[pos] == '"') {
    const int start = pos + 1;
    const int end = line.indexOf('"', start);
    return end > start ? line.substring(start, end) : "";
  }

  const int start = pos;
  while (pos < static_cast<int>(line.length()) && line[pos] != ',' && line[pos] != '}') ++pos;
  String value = line.substring(start, pos);
  value.trim();
  return value;
}

bool truthyValue(const String &value, bool fallback) {
  if (value.length() == 0) return fallback;
  if (value == "1" || value == "true" || value == "on") return true;
  if (value == "0" || value == "false" || value == "off") return false;
  return fallback;
}

String valueAfter(const String &line, const char *prefix) {
  if (!line.startsWith(prefix)) return "";
  String value = line.substring(strlen(prefix));
  value.trim();
  return value;
}

bool applyCommandLine(String line, const char *source) {
  line.trim();
  if (line.length() == 0) return false;

  String animId;
  bool handled = false;

  if (line[0] == '{') {
    animId = jsonValue(line, "anim");
    if (animId.length() == 0) animId = jsonValue(line, "id");

    String autoValue = jsonValue(line, "auto");
    if (autoValue.length()) {
      autoCycle = truthyValue(autoValue, autoCycle);
      nextAutoCycleMs = millis() + 6000;
      handled = true;
    }

    String backlightValue = jsonValue(line, "backlight");
    if (backlightValue.length()) {
      setBacklight(truthyValue(backlightValue, backlightOn));
      handled = true;
    }

    String speedValue = jsonValue(line, "speed");
    if (speedValue.length()) {
      speedLevel = constrain(speedValue.toInt(), 1, 3);
      handled = true;
    }
  } else {
    animId = valueAfter(line, "anim=");
    if (animId.length() == 0) animId = valueAfter(line, "id=");
    if (animId.length() == 0) animId = valueAfter(line, "/anim?id=");

    String autoValue = valueAfter(line, "auto=");
    if (autoValue.length()) {
      autoCycle = truthyValue(autoValue, autoCycle);
      nextAutoCycleMs = millis() + 6000;
      handled = true;
    }

    String backlightValue = valueAfter(line, "backlight=");
    if (backlightValue.length()) {
      setBacklight(truthyValue(backlightValue, backlightOn));
      handled = true;
    }

    String speedValue = valueAfter(line, "speed=");
    if (speedValue.length()) {
      speedLevel = constrain(speedValue.toInt(), 1, 3);
      handled = true;
    }

    if (line == "next") {
      setAnim((animIndex + 1) % ANIM_COUNT);
      handled = true;
    } else if (line == "state" || line == "?") {
      handled = true;
    } else if (animId.length() == 0 && findAnimIndex(line) >= 0) {
      animId = line;
    }
  }

  if (animId.length()) {
    handled = setAnimById(animId);
  }

  if (handled) {
    lastCommandMs = millis();
    lastCommandWasBle = strcmp(source, "ble") == 0;
    if (lastCommandWasBle) {
      lastBleCommandMs = lastCommandMs;
    }
    commandIdleApplied = false;
    lastSource = source;
    Serial.print("[");
    Serial.print(source);
    Serial.print("] ");
    Serial.println(stateJson());
    notifyBleState();
  } else {
    Serial.print("[");
    Serial.print(source);
    Serial.print("] ignored: ");
    Serial.println(line);
  }

  return handled;
}

class TankBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleConnected = true;
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    server->startAdvertising();
  }
};

class TankBleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const std::string value = characteristic->getValue();
    for (uint16_t i = 0; i < value.length(); ++i) {
      const char ch = value[i];
      if (ch == '\r') continue;
      if (ch == '\n') {
        taskENTER_CRITICAL(&bleCmdMux);
        strncpy(blePendingBuf, bleLine.c_str(), 240);
        blePendingBuf[240] = '\0';
        blePendingCmd = true;
        taskEXIT_CRITICAL(&bleCmdMux);
        bleLine = "";
      } else if (bleLine.length() < 240) {
        bleLine += ch;
      }
    }
  }
};

void startBle() {
  BLEDevice::init(BLE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new TankBleServerCallbacks());

  BLEService *service = bleServer->createService(BLE_SERVICE_UUID);
  BLECharacteristic *rxCharacteristic = service->createCharacteristic(
      BLE_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxCharacteristic->setCallbacks(new TankBleRxCallbacks());

  bleTxCharacteristic = service->createCharacteristic(
      BLE_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(new BLE2902());

  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.print("BLE UART discoverable: ");
  Serial.println(BLE_NAME);
}

void pollSerialCommands() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch == '\n') {
      applyCommandLine(serialLine, "serial");
      serialLine = "";
    } else if (serialLine.length() < 240) {
      serialLine += ch;
    }
  }
}

void pollBleState() {
  const bool connected = bleConnected;
  if (connected == bleWasConnected) return;

  bleWasConnected = connected;
  lastSource = connected ? "ble_connected" : "ble_waiting";
  lastCommandMs = millis();

  if (connected) {
    setAnimById("idle", true);
  } else if (lastCommandWasBle && millis() - lastBleCommandMs > BLE_DISCONNECT_BEACON_GRACE_MS) {
    setAnimById("beacon", true);
  }

  Serial.println(stateJson());
  notifyBleState();
}

void pollCommandIdleTimeout() {
  if (commandIdleApplied || lastCommandMs == 0) return;
  if (!lastCommandWasBle) return;
  if (millis() - lastCommandMs <= COMMAND_IDLE_TIMEOUT_MS) return;
  if (bleConnected) return;

  commandIdleApplied = true;
  lastCommandMs = millis();
  lastSource = "command_timeout_beacon";
  setAnimById("beacon", true);
  Serial.println(stateJson());
  notifyBleState();
}

void pollBleCommands() {
  bool hasPending = false;
  char localBuf[241];
  taskENTER_CRITICAL(&bleCmdMux);
  if (blePendingCmd) {
    memcpy(localBuf, blePendingBuf, sizeof(blePendingBuf));
    hasPending = true;
    blePendingCmd = false;
  }
  taskEXIT_CRITICAL(&bleCmdMux);
  if (hasPending) applyCommandLine(String(localBuf), "ble");
}

const char INDEX_HTML[] PROGMEM = R"html(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Clawd Mochi Tank</title>
<style>
body{margin:0;background:#101216;color:#f7f1ea;font-family:system-ui,Segoe UI,sans-serif}
main{max-width:520px;margin:auto;padding:18px}.brand{font-size:30px;font-weight:800;line-height:1;margin:8px 0 4px}
.sub{color:#aaa;margin-bottom:18px}.panel{border:1px solid #303640;background:#171a20;border-radius:8px;padding:12px;margin-bottom:12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
button{border:1px solid #3a3f47;background:#1c2027;color:#fff;border-radius:8px;padding:14px;font:inherit}
button.on{border-color:#ff6a3d;background:#2b1813}.wide{grid-column:1/-1}.row{display:flex;gap:10px;align-items:center}
input[type=range]{width:100%}.hint{color:#888;font-size:13px;margin-top:14px}.status{color:#d7d2cc;font-size:14px}
</style></head><body><main>
<div class=brand>Clawd Mochi Tank</div><div class=sub>Tank animations on your 240x240 ST7789 Mochi.</div>
<div class="panel status" id=status>connecting...</div>
<div class="panel grid">
<button onclick="anim('idle')">Idle</button><button onclick="anim('typing')">Typing</button>
<button onclick="anim('thinking')">Thinking</button><button onclick="anim('building')">Building</button>
<button onclick="anim('juggling')">Juggling</button><button onclick="anim('conducting')">Conducting</button>
<button onclick="anim('debugger')">Debugger</button><button onclick="anim('wizard')">Wizard</button>
<button onclick="anim('beacon')">Beacon</button><button onclick="anim('confused')">Confused</button>
<button onclick="anim('sweeping')">Sweeping</button><button onclick="anim('walking')">Walking</button>
<button onclick="anim('going_away')">Going away</button><button onclick="anim('alert')">Alert</button>
<button onclick="anim('happy')">Happy</button><button onclick="anim('sleeping')">Sleep</button>
<button onclick="anim('dizzy')">Dizzy</button><button onclick="anim('disconnected')">Disconnected</button>
<button onclick="nextAnim()">Next</button><button id=autoBtn onclick="autoMode()">Auto cycle</button>
<button id=blBtn class=wide onclick="backlight()">Display on/off</button>
</div>
<div class=panel>
<label>Animation speed <span id=spdText>normal</span></label>
<div class=row><input id=spd type=range min=1 max=3 value=2 oninput="speed(this.value)"></div>
</div>
<div class=hint>SSID: Clawd-Mochi-Tank / password: clawd1234 / open http://192.168.4.1</div>
</main><script>
let state={auto:false,backlight:true,speed:2,anim:'idle',label:'Idle',scale:1};
const speedLabels=['','slow','normal','fast'];
async function req(p){try{const r=await fetch(p);state=await r.json();paint()}catch(e){}}
function paint(){
  status.textContent=state.label+' / '+state.anim+' / '+state.scale+'x';
  autoBtn.classList.toggle('on',state.auto);
  blBtn.classList.toggle('on',state.backlight);
  blBtn.textContent=state.backlight?'Display on':'Display off';
  spd.value=state.speed; spdText.textContent=speedLabels[state.speed]||'normal';
}
async function anim(id){await req('/anim?id='+encodeURIComponent(id))}
async function autoMode(){await req('/auto?on='+(state.auto?0:1))}
async function backlight(){await req('/backlight?on='+(state.backlight?0:1))}
async function speed(v){await req('/speed?v='+v)}
async function nextAnim(){await req('/next')}
setInterval(()=>req('/state'),1500); req('/state');
</script></body></html>
)html";

void routeRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void routeAnim() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }

  const int8_t index = findAnimIndex(server.arg("id"));
  if (index >= 0) {
    setAnim(index);
    notifyBleState();
    server.send(200, "application/json", stateJson());
    return;
  }

  server.send(404, "application/json", "{\"ok\":0}");
}

void routeAuto() {
  autoCycle = !server.hasArg("on") || server.arg("on") != "0";
  nextAutoCycleMs = millis() + 6000;
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeBacklight() {
  setBacklight(!server.hasArg("on") || server.arg("on") != "0");
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeSpeed() {
  if (server.hasArg("v")) {
    speedLevel = constrain(server.arg("v").toInt(), 1, 3);
  }
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeNext() {
  setAnim((animIndex + 1) % ANIM_COUNT);
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeCmd() {
  if (!server.hasArg("k") || server.arg("k").isEmpty()) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }

  const char k = server.arg("k")[0];
  switch (k) {
    case 'i': setAnim(findAnimIndex("idle")); break;
    case 't': setAnim(findAnimIndex("typing")); break;
    case 'h': setAnim(findAnimIndex("thinking")); break;
    case 'b': setAnim(findAnimIndex("building")); break;
    case 'a': setAnim(findAnimIndex("alert")); break;
    case 'w': setAnim(findAnimIndex("happy")); break;
    case 's': setAnim(findAnimIndex("sleeping")); break;
    case 'd': setAnim(findAnimIndex("dizzy")); break;
    case 'n': setAnim((animIndex + 1) % ANIM_COUNT); break;
    case 'o':
      autoCycle = !autoCycle;
      nextAutoCycleMs = millis() + 6000;
      break;
    default:
      server.send(404, "application/json", "{\"ok\":0}");
      return;
  }
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeState() {
  server.send(200, "application/json", stateJson());
}

void startWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", HTTP_GET, routeRoot);
  server.on("/anim", HTTP_GET, routeAnim);
  server.on("/auto", HTTP_GET, routeAuto);
  server.on("/backlight", HTTP_GET, routeBacklight);
  server.on("/speed", HTTP_GET, routeSpeed);
  server.on("/next", HTTP_GET, routeNext);
  server.on("/cmd", HTTP_GET, routeCmd);
  server.on("/state", HTTP_GET, routeState);
  server.begin();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  printWiring();

  pinMode(TFT_RST, OUTPUT);
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_BL, OUTPUT);
  setBacklight(true);
  hardResetPanel();

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.invertDisplay(true);
  tft.fillScreen(ST77XX_BLACK);

  autoCycle = false;
  setAnimById("beacon", true);
  startWeb();
  startBle();
  nextAutoCycleMs = millis() + 6000;

  Serial.println("Clawd Mochi Tank ready. Connect WiFi: Clawd-Mochi-Tank / clawd1234");
  Serial.println("Serial command examples: anim=typing, {\"anim\":\"building\"}, next, state");
  Serial.println("BLE device name: Claude-Mochi-Tank");
}

void loop() {
  server.handleClient();
  pollSerialCommands();
  pollBleState();
  pollBleCommands();
  pollCommandIdleTimeout();

  const TankAnim &anim = ANIMS[animIndex];
  const uint32_t now = millis();
  uint16_t frameDelay = 1000 / anim.fps;
  if (speedLevel == 1) frameDelay *= 2;
  if (speedLevel == 3) frameDelay = max<uint16_t>(20, frameDelay / 2);

  if (backlightOn && (int32_t)(now - nextFrameMs) >= 0) {
    nextFrameMs = now + frameDelay;
    frameIndex = (frameIndex + 1) % anim.frameCount;
    drawSpriteFrame(anim, frameIndex);
  }

  if (autoCycle && (int32_t)(now - nextAutoCycleMs) >= 0) {
    nextAutoCycleMs = now + 6000;
    setAnim((animIndex + 1) % ANIM_COUNT, true);
  }
}
