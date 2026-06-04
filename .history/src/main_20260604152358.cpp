#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <WebServer.h>
#include <WiFi.h>

namespace {
constexpr int LED_RUN = 0;
constexpr int LED_WAIT = 1;
constexpr int LED_ALERT = 2;
constexpr bool LED_ACTIVE_LOW = true;
constexpr bool LED_CHASE_TEST = true;
constexpr uint16_t LED_CHASE_TEST_STEP_MS = 50000;

constexpr char AP_SSID[] = "Clawd-Mochi-Tank";
constexpr char AP_PASS[] = "clawd1234";
constexpr char BLE_NAME[] = "Claude-Mochi-Tank";
constexpr char BLE_SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr uint32_t BLE_DISCONNECT_BEACON_GRACE_MS = 10000;
constexpr uint32_t COMMAND_IDLE_TIMEOUT_MS = 30000;

enum LedBit : uint8_t {
  LED_BIT_RUN = 1 << 0,
  LED_BIT_WAIT = 1 << 1,
  LED_BIT_ALERT = 1 << 2,
};

enum class LedPattern : uint8_t {
  Steady,
  Blink,
  Chase,
  Alternate,
};

struct LedAnim {
  const char *id;
  const char *label;
  uint8_t mask;
  LedPattern pattern;
  uint16_t periodMs;
};

const LedAnim ANIMS[] = {
    {"idle", "Idle", LED_BIT_RUN, LedPattern::Steady, 1000},
    {"typing", "Typing", LED_BIT_RUN, LedPattern::Blink, 260},
    {"thinking", "Thinking", LED_BIT_WAIT, LedPattern::Blink, 650},
    {"building", "Building", LED_BIT_RUN | LED_BIT_WAIT, LedPattern::Alternate, 220},
    {"juggling", "Juggling", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 160},
    {"conducting", "Conducting", LED_BIT_RUN | LED_BIT_WAIT, LedPattern::Chase, 180},
    {"debugger", "Debugger", LED_BIT_RUN | LED_BIT_ALERT, LedPattern::Alternate, 180},
    {"wizard", "Wizard", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Blink, 380},
    {"beacon", "Beacon", LED_BIT_WAIT, LedPattern::Blink, 900},
    {"confused", "Confused", LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Alternate, 420},
    {"sweeping", "Sweeping", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 1000},
    {"walking", "Walking", LED_BIT_RUN | LED_BIT_WAIT, LedPattern::Alternate, 300},
    {"going_away", "Going away", LED_BIT_WAIT, LedPattern::Blink, 1200},
    {"alert", "Needs attention", LED_BIT_ALERT, LedPattern::Blink, 180},
    {"happy", "Happy", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Steady, 1000},
    {"sleeping", "Sleeping", LED_BIT_RUN, LedPattern::Blink, 1600},
    {"dizzy", "Dizzy", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 90},
    {"disconnected", "Disconnected", LED_BIT_ALERT, LedPattern::Blink, 1000},
    {"manual", "Manual LEDs", 0, LedPattern::Steady, 1000},
};
constexpr uint8_t ANIM_COUNT = sizeof(ANIMS) / sizeof(ANIMS[0]);
constexpr uint8_t MANUAL_ANIM_INDEX = ANIM_COUNT - 1;

const int LED_PINS[] = {LED_RUN, LED_WAIT, LED_ALERT};

WebServer server(80);
BLEServer *bleServer = nullptr;
BLECharacteristic *bleTxCharacteristic = nullptr;

uint8_t animIndex = 0;
uint8_t manualMask = 0;
uint8_t appliedMask = 0xFF;
bool autoCycle = true;
bool ledsEnabled = true;
uint8_t speedLevel = 2;
uint32_t nextAutoCycleMs = 0;
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

int8_t findAnimIndex(const String &id) {
  for (uint8_t i = 0; i < ANIM_COUNT; ++i) {
    if (id == ANIMS[i].id) return i;
  }
  return -1;
}

void writeLedPin(int pin, bool on) {
  digitalWrite(pin, LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

void applyLedMask(uint8_t mask) {
  mask &= 0x07;
  if (mask == appliedMask) return;
  appliedMask = mask;

  for (uint8_t i = 0; i < 3; ++i) {
    writeLedPin(LED_PINS[i], (mask & (1 << i)) != 0);
  }
}

uint8_t currentBaseMask() {
  if (animIndex == MANUAL_ANIM_INDEX) return manualMask;
  return ANIMS[animIndex].mask;
}

uint8_t activeMaskForNow(uint32_t now) {
  if (!ledsEnabled) return 0;

  const LedAnim &anim = ANIMS[animIndex];
  const uint8_t baseMask = currentBaseMask();
  if (baseMask == 0) return 0;

  uint16_t period = anim.periodMs;
  if (speedLevel == 1) period *= 2;
  if (speedLevel == 3) period = max<uint16_t>(40, period / 2);

  const uint32_t tick = now / max<uint16_t>(1, period);
  switch (anim.pattern) {
    case LedPattern::Steady:
      return baseMask;
    case LedPattern::Blink:
      return (tick % 2 == 0) ? baseMask : 0;
    case LedPattern::Alternate:
      return (tick % 2 == 0) ? (baseMask & (LED_BIT_RUN | LED_BIT_ALERT)) : (baseMask & LED_BIT_WAIT);
    case LedPattern::Chase: {
      const uint8_t bit = 1 << (tick % 3);
      return (baseMask & bit) ? bit : baseMask;
    }
  }
  return baseMask;
}

void updateLeds() {
  applyLedMask(activeMaskForNow(millis()));
}

void runLedChaseTest() {
  static uint8_t lastStep = 0xFF;
  const uint8_t step = (millis() / LED_CHASE_TEST_STEP_MS) % 3;
  if (step != lastStep) {
    lastStep = step;
    Serial.print("LED test: GPIO");
    Serial.println(LED_PINS[step]);
  }
  applyLedMask(1 << step);
}

void setOutputEnabled(bool on) {
  ledsEnabled = on;
  appliedMask = 0xFF;
  updateLeds();
}

void printWiring() {
  Serial.println();
  Serial.println("Clawd Mochi Tank LED output");
  Serial.println("3 LEDs common anode -> 3V3");
  Serial.println("LED RUN cathode   -> GPIO5");
  Serial.println("LED WAIT cathode  -> GPIO6");
  Serial.println("LED ALERT cathode -> GPIO7");
  Serial.println("GPIO LOW = LED on, GPIO HIGH = LED off");
}

void setAnim(uint8_t index, bool keepAuto = false) {
  if (index >= ANIM_COUNT) return;
  animIndex = index;
  if (!keepAuto) autoCycle = false;
  appliedMask = 0xFF;
  updateLeds();
}

bool setAnimById(const String &id, bool keepAuto = false) {
  const int8_t index = findAnimIndex(id);
  if (index < 0) return false;
  setAnim(index, keepAuto);
  return true;
}

bool setManualLeds(const String &value) {
  String text = value;
  text.trim();
  if (text.length() == 0) return false;

  uint8_t mask = 0;
  if (text.length() == 3 && (text[0] == '0' || text[0] == '1')) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (text[i] == '1') mask |= (1 << i);
    }
  } else {
    mask = static_cast<uint8_t>(text.toInt()) & 0x07;
  }

  manualMask = mask;
  setAnim(MANUAL_ANIM_INDEX);
  return true;
}

String maskBits(uint8_t mask) {
  String bits;
  for (uint8_t i = 0; i < 3; ++i) {
    bits += (mask & (1 << i)) ? '1' : '0';
  }
  return bits;
}

String stateJson() {
  const uint8_t baseMask = currentBaseMask();
  String json = "{\"anim\":\"";
  json += ANIMS[animIndex].id;
  json += "\",\"label\":\"";
  json += ANIMS[animIndex].label;
  json += "\",\"auto\":";
  json += autoCycle ? "true" : "false";
  json += ",\"speed\":";
  json += speedLevel;
  json += ",\"backlight\":";
  json += ledsEnabled ? "true" : "false";
  json += ",\"output\":\"leds\",\"active_low\":";
  json += LED_ACTIVE_LOW ? "true" : "false";
  json += ",\"mask\":\"";
  json += maskBits(baseMask);
  json += "\",\"applied\":\"";
  json += maskBits(appliedMask & 0x07);
  json += "\",\"ble\":";
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
  String ledsValue;
  bool handled = false;

  if (line[0] == '{') {
    animId = jsonValue(line, "anim");
    if (animId.length() == 0) animId = jsonValue(line, "id");
    ledsValue = jsonValue(line, "leds");
    if (ledsValue.length() == 0) ledsValue = jsonValue(line, "led");
    if (ledsValue.length() == 0) ledsValue = jsonValue(line, "mask");

    String autoValue = jsonValue(line, "auto");
    if (autoValue.length()) {
      autoCycle = truthyValue(autoValue, autoCycle);
      nextAutoCycleMs = millis() + 6000;
      handled = true;
    }

    String backlightValue = jsonValue(line, "backlight");
    if (backlightValue.length()) {
      setOutputEnabled(truthyValue(backlightValue, ledsEnabled));
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

    ledsValue = valueAfter(line, "led=");
    if (ledsValue.length() == 0) ledsValue = valueAfter(line, "leds=");
    if (ledsValue.length() == 0) ledsValue = valueAfter(line, "mask=");

    String autoValue = valueAfter(line, "auto=");
    if (autoValue.length()) {
      autoCycle = truthyValue(autoValue, autoCycle);
      nextAutoCycleMs = millis() + 6000;
      handled = true;
    }

    String backlightValue = valueAfter(line, "backlight=");
    if (backlightValue.length()) {
      setOutputEnabled(truthyValue(backlightValue, ledsEnabled));
      handled = true;
    }

    String speedValue = valueAfter(line, "speed=");
    if (speedValue.length()) {
      speedLevel = constrain(speedValue.toInt(), 1, 3);
      handled = true;
    }

    if (line == "next") {
      setAnim((animIndex + 1) % (ANIM_COUNT - 1));
      handled = true;
    } else if (line == "state" || line == "?") {
      handled = true;
    } else if (animId.length() == 0 && findAnimIndex(line) >= 0) {
      animId = line;
    }
  }

  if (ledsValue.length()) {
    handled = setManualLeds(ledsValue);
  } else if (animId.length()) {
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
  setAnimById("sweeping", true);
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
<title>Clawd LED Tank</title>
<style>
body{margin:0;background:#101216;color:#f7f1ea;font-family:system-ui,Segoe UI,sans-serif}
main{max-width:520px;margin:auto;padding:18px}.brand{font-size:30px;font-weight:800;line-height:1;margin:8px 0 4px}
.sub{color:#aaa;margin-bottom:18px}.panel{border:1px solid #303640;background:#171a20;border-radius:8px;padding:12px;margin-bottom:12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.leds{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
button{border:1px solid #3a3f47;background:#1c2027;color:#fff;border-radius:8px;padding:14px;font:inherit}
button.on{border-color:#ff6a3d;background:#2b1813}.wide{grid-column:1/-1}.row{display:flex;gap:10px;align-items:center}
input[type=range]{width:100%}.hint{color:#888;font-size:13px;margin-top:14px}.status{color:#d7d2cc;font-size:14px}
</style></head><body><main>
<div class=brand>Clawd LED Tank</div><div class=sub>Three common-anode LEDs on GPIO4, GPIO6, GPIO7.</div>
<div class="panel status" id=status>connecting...</div>
<div class="panel leds">
<button onclick="led('100')">RUN</button><button onclick="led('010')">WAIT</button><button onclick="led('001')">ALERT</button>
<button onclick="led('110')">RUN+WAIT</button><button onclick="led('011')">WAIT+ALERT</button><button onclick="led('111')">ALL</button>
</div>
<div class="panel grid">
<button onclick="anim('idle')">Idle</button><button onclick="anim('typing')">Typing</button>
<button onclick="anim('thinking')">Thinking</button><button onclick="anim('building')">Building</button>
<button onclick="anim('beacon')">Beacon</button><button onclick="anim('alert')">Alert</button>
<button onclick="anim('happy')">Happy</button><button onclick="anim('disconnected')">Disconnected</button>
<button onclick="nextAnim()">Next</button><button id=autoBtn onclick="autoMode()">Auto cycle</button>
<button id=blBtn class=wide onclick="backlight()">LED output on/off</button>
</div>
<div class=panel>
<label>Pattern speed <span id=spdText>normal</span></label>
<div class=row><input id=spd type=range min=1 max=3 value=2 oninput="speed(this.value)"></div>
</div>
<div class=hint>SSID: Clawd-Mochi-Tank / password: clawd1234 / open http://192.168.4.1</div>
</main><script>
let state={auto:false,backlight:true,speed:2,anim:'idle',label:'Idle',mask:'000',applied:'000'};
const speedLabels=['','slow','normal','fast'];
async function req(p){try{const r=await fetch(p);state=await r.json();paint()}catch(e){}}
function paint(){
  status.textContent=state.label+' / '+state.anim+' / set '+state.mask+' / now '+state.applied;
  autoBtn.classList.toggle('on',state.auto);
  blBtn.classList.toggle('on',state.backlight);
  blBtn.textContent=state.backlight?'LED output on':'LED output off';
  spd.value=state.speed; spdText.textContent=speedLabels[state.speed]||'normal';
}
async function anim(id){await req('/anim?id='+encodeURIComponent(id))}
async function led(v){await req('/leds?v='+v)}
async function autoMode(){await req('/auto?on='+(state.auto?0:1))}
async function backlight(){await req('/backlight?on='+(state.backlight?0:1))}
async function speed(v){await req('/speed?v='+v)}
async function nextAnim(){await req('/next')}
setInterval(()=>req('/state'),1000); req('/state');
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

void routeLeds() {
  String value = server.hasArg("v") ? server.arg("v") : server.arg("mask");
  if (!setManualLeds(value)) {
    server.send(400, "application/json", "{\"ok\":0}");
    return;
  }
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeAuto() {
  autoCycle = !server.hasArg("on") || server.arg("on") != "0";
  nextAutoCycleMs = millis() + 6000;
  notifyBleState();
  server.send(200, "application/json", stateJson());
}

void routeBacklight() {
  setOutputEnabled(!server.hasArg("on") || server.arg("on") != "0");
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
  setAnim((animIndex + 1) % (ANIM_COUNT - 1));
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
    case 'n': setAnim((animIndex + 1) % (ANIM_COUNT - 1)); break;
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
  server.on("/leds", HTTP_GET, routeLeds);
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

  for (uint8_t i = 0; i < 3; ++i) {
    pinMode(LED_PINS[i], OUTPUT);
    writeLedPin(LED_PINS[i], false);
  }

  if (LED_CHASE_TEST) {
    Serial.println("LED chase test mode: GPIO5 -> GPIO6 -> GPIO7");
    return;
  }

  autoCycle = false;
  setAnimById("beacon", true);
  startWeb();
  startBle();
  nextAutoCycleMs = millis() + 6000;

  Serial.println("Clawd LED Tank ready. Connect WiFi: Clawd-Mochi-Tank / clawd1234");
  Serial.println("Serial command examples: anim=typing, led=101, {\"leds\":\"010\"}, next, state");
  Serial.println("BLE device name: Claude-Mochi-Tank");
}

void loop() {
  if (LED_CHASE_TEST) {
    runLedChaseTest();
    delay(5);
    return;
  }

  server.handleClient();
  pollSerialCommands();
  pollBleState();
  pollBleCommands();
  pollCommandIdleTimeout();
  updateLeds();

  if (autoCycle && (int32_t)(millis() - nextAutoCycleMs) >= 0) {
    nextAutoCycleMs = millis() + 6000;
    setAnim((animIndex + 1) % (ANIM_COUNT - 1), true);
  }
}
