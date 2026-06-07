#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_sleep.h>

namespace {
constexpr int LED_RUN = 2;
constexpr int LED_WAIT = 0;
constexpr int LED_ALERT = 1;
constexpr bool LED_ACTIVE_LOW = true;
constexpr bool LED_CHASE_TEST = false;
constexpr bool LED_SEQUENCE_TEST = false;
constexpr uint16_t LED_CHASE_TEST_STEP_MS = 500;
constexpr uint16_t LED_SEQUENCE_TEST_STEP_MS = 2000;

constexpr char BLE_NAME[] = "Claude-Mochi-Tank";
constexpr char BLE_SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr uint32_t BLE_DISCONNECT_BEACON_GRACE_MS = 10000;
constexpr uint32_t COMMAND_IDLE_TIMEOUT_MS = 180000;
constexpr uint64_t LOW_POWER_SLEEP_SLICE_US = 250000;
constexpr bool BLE_ENABLED = false;

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
  PairChase,
};

struct LedAnim {
  const char *id;
  const char *label;
  uint8_t mask;
  LedPattern pattern;
  uint16_t periodMs;
};

const LedAnim ANIMS[] = {
    {"idle", "Idle", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 2000},
    {"typing", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"thinking", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"building", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"juggling", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"conducting", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"debugger", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"wizard", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"beacon", "Waiting for connection", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 100},
    {"confused", "Waiting for confirmation", LED_BIT_WAIT, LedPattern::Steady, 1000},
    {"sweeping", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"walking", "Normal work", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 500},
    {"going_away", "Sleeping", 0, LedPattern::Steady, 1000},
    {"alert", "Waiting for confirmation", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Blink, 1000},
    {"happy", "Task complete", LED_BIT_RUN, LedPattern::Steady, 1000},
    {"sleeping", "Sleeping", 0, LedPattern::Steady, 1000},
    {"dizzy", "Error", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::PairChase, 300},
    {"disconnected", "Waiting for connection", LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT, LedPattern::Chase, 100},
    {"manual", "Manual LEDs", 0, LedPattern::Steady, 1000},
};
constexpr uint8_t ANIM_COUNT = sizeof(ANIMS) / sizeof(ANIMS[0]);
constexpr uint8_t MANUAL_ANIM_INDEX = ANIM_COUNT - 1;

const int LED_PINS[] = {LED_RUN, LED_WAIT, LED_ALERT};

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
bool linkEstablished = false;
bool lowPowerMode = false;
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
    case LedPattern::PairChase: {
      const uint8_t pairMasks[] = {
          static_cast<uint8_t>(LED_BIT_RUN | LED_BIT_WAIT),
          static_cast<uint8_t>(LED_BIT_WAIT | LED_BIT_ALERT),
          static_cast<uint8_t>(LED_BIT_RUN | LED_BIT_ALERT),
      };
      return pairMasks[tick % 3] & baseMask;
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

void runLedSequenceTest() {
  static uint8_t lastStep = 0xFF;
  const uint8_t step = (millis() / LED_SEQUENCE_TEST_STEP_MS) % 3;
  if (step != lastStep) {
    lastStep = step;
    Serial.print("LED sequence test: GPIO");
    Serial.println(LED_PINS[step]);
  }
  applyLedMask(1 << step);
}

void setOutputEnabled(bool on) {
  ledsEnabled = on;
  appliedMask = 0xFF;
  updateLeds();
}

void enterLowPowerMode() {
  if (lowPowerMode) return;
  lowPowerMode = true;
  setOutputEnabled(false);
  esp_sleep_enable_timer_wakeup(LOW_POWER_SLEEP_SLICE_US);
}

void exitLowPowerMode() {
  if (!lowPowerMode) return;
  lowPowerMode = false;
  setOutputEnabled(true);
}

void printWiring() {
  Serial.println();
  Serial.println("Clawd Mochi Tank LED output");
  Serial.println("3 LEDs common anode -> 3V3");
  Serial.print("LED RUN cathode   -> GPIO");
  Serial.println(LED_RUN);
  Serial.print("LED WAIT cathode  -> GPIO");
  Serial.println(LED_WAIT);
  Serial.print("LED ALERT cathode -> GPIO");
  Serial.println(LED_ALERT);
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

bool isSleepState() {
  const String id = ANIMS[animIndex].id;
  return id == "sleeping" || id == "going_away";
}

void markLinkActivity(const char *source) {
  exitLowPowerMode();
  lastCommandMs = millis();
  lastCommandWasBle = strcmp(source, "ble") == 0;
  if (lastCommandWasBle) {
    lastBleCommandMs = lastCommandMs;
  }
  commandIdleApplied = false;
  linkEstablished = true;
  lastSource = source;
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
  json += ",\"linked\":";
  json += linkEstablished ? "true" : "false";
  json += ",\"low_power\":";
  json += lowPowerMode ? "true" : "false";
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
    markLinkActivity(source);
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
  if (!BLE_ENABLED) {
    Serial.println("BLE disabled");
    return;
  }

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

  if (connected) {
    markLinkActivity("ble_connected");
    setAnimById("idle", true);
  } else if (lastCommandWasBle && millis() - lastBleCommandMs > BLE_DISCONNECT_BEACON_GRACE_MS) {
    linkEstablished = false;
    setAnimById("beacon", true);
  }

  Serial.println(stateJson());
  notifyBleState();
}

void pollCommandIdleTimeout() {
  if (commandIdleApplied || lastCommandMs == 0) return;
  if (!linkEstablished) return;
  if (isSleepState()) return;
  if (millis() - lastCommandMs <= COMMAND_IDLE_TIMEOUT_MS) return;

  commandIdleApplied = true;
  lastCommandMs = millis();
  lastSource = "command_timeout_low_power";
  enterLowPowerMode();
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
    Serial.println("LED chase test mode");
    return;
  }

  if (LED_SEQUENCE_TEST) {
    Serial.println("LED sequence test mode");
    return;
  }

  autoCycle = false;
  setAnimById("beacon", true);
  startBle();
  nextAutoCycleMs = millis() + 6000;

  Serial.println("Clawd LED Tank ready. BLE is disabled; use USB serial.");
  Serial.println("Serial command examples: anim=typing, led=101, {\"leds\":\"010\"}, next, state");
}

void loop() {
  if (LED_CHASE_TEST) {
    runLedChaseTest();
    delay(5);
    return;
  }

  if (LED_SEQUENCE_TEST) {
    runLedSequenceTest();
    delay(5);
    return;
  }

  if (lowPowerMode) {
    esp_light_sleep_start();
  }

  pollSerialCommands();
  if (BLE_ENABLED) {
    pollBleState();
    pollBleCommands();
  }
  pollCommandIdleTimeout();
  if (lowPowerMode) return;
  updateLeds();

  if (autoCycle && (int32_t)(millis() - nextAutoCycleMs) >= 0) {
    nextAutoCycleMs = millis() + 6000;
    setAnim((animIndex + 1) % (ANIM_COUNT - 1), true);
  }
}
