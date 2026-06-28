#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

namespace {
constexpr int LED_RUN = 10;
constexpr int LED_WAIT = 9;
constexpr int LED_ALERT = 8;
constexpr int LED_COMMON_ANODE = 7;
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
constexpr bool BLE_ENABLED = true;

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

struct LedStep {
  uint8_t mask;
  uint16_t ms;
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
    {"custom", "Custom effect", 0, LedPattern::Steady, 1000},
    {"manual", "Manual LEDs", 0, LedPattern::Steady, 1000},
};
constexpr uint8_t ANIM_COUNT = sizeof(ANIMS) / sizeof(ANIMS[0]);
constexpr uint8_t MANUAL_ANIM_INDEX = ANIM_COUNT - 1;
constexpr uint8_t CUSTOM_ANIM_INDEX = ANIM_COUNT - 2;
constexpr uint8_t CUSTOM_STEP_MAX = 8;
constexpr uint16_t CUSTOM_STEP_MIN_MS = 20;
constexpr uint16_t CUSTOM_STEP_MAX_MS = 10000;

const int LED_PINS[] = {LED_RUN, LED_WAIT, LED_ALERT};

BLEServer *bleServer = nullptr;
BLECharacteristic *bleTxCharacteristic = nullptr;

uint8_t animIndex = 0;
uint8_t manualMask = 0;
uint8_t appliedMask = 0xFF;
LedStep customSteps[CUSTOM_STEP_MAX];
uint8_t customStepCount = 0;
uint32_t customStartedMs = 0;
bool autoCycle = true;
bool ledsEnabled = true;
uint8_t speedLevel = 2;
uint32_t nextAutoCycleMs = 0;
String serialLine;
String bleLine;
portMUX_TYPE bleCmdMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool blePendingCmd = false;
char blePendingBuf[513];
bool bleConnected = false;
bool bleWasConnected = false;
uint32_t lastBleCommandMs = 0;
uint32_t lastCommandMs = 0;
bool lastCommandWasBle = false;
bool linkEstablished = false;
String lastSource = "boot";

String stateJson();
bool applyCommandLine(String line, const char *source);
uint8_t maskFromValue(const String &value, uint8_t fallback = 0);
bool setCustomEffectFromJson(const String &line);

uint16_t clampStepMs(int value) {
  if (value < CUSTOM_STEP_MIN_MS) return CUSTOM_STEP_MIN_MS;
  if (value > CUSTOM_STEP_MAX_MS) return CUSTOM_STEP_MAX_MS;
  return static_cast<uint16_t>(value);
}

void setStep(LedStep &step, uint8_t mask, uint16_t ms) {
  step.mask = mask & 0x07;
  step.ms = ms;
}

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
  if (animIndex == CUSTOM_ANIM_INDEX) {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < customStepCount; ++i) mask |= customSteps[i].mask;
    return mask;
  }
  if (animIndex == MANUAL_ANIM_INDEX) return manualMask;
  return ANIMS[animIndex].mask;
}

uint8_t customMaskForNow(uint32_t now) {
  if (customStepCount == 0) return 0;
  uint32_t totalMs = 0;
  for (uint8_t i = 0; i < customStepCount; ++i) totalMs += customSteps[i].ms;
  if (totalMs == 0) return 0;

  uint32_t elapsed = (now - customStartedMs) % totalMs;
  for (uint8_t i = 0; i < customStepCount; ++i) {
    if (elapsed < customSteps[i].ms) return customSteps[i].mask;
    elapsed -= customSteps[i].ms;
  }
  return customSteps[customStepCount - 1].mask;
}

uint8_t activeMaskForNow(uint32_t now) {
  if (!ledsEnabled) return 0;
  if (animIndex == CUSTOM_ANIM_INDEX) return customMaskForNow(now);

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

void printWiring() {
  Serial.println();
  Serial.println("Clawd Mochi Tank LED output");
  Serial.print("3 LEDs common anode -> GPIO");
  Serial.print(LED_COMMON_ANODE);
  Serial.println(" (3.3V output)");
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

void markLinkActivity(const char *source) {
  lastCommandMs = millis();
  lastCommandWasBle = strcmp(source, "ble") == 0;
  if (lastCommandWasBle) {
    lastBleCommandMs = lastCommandMs;
  }
  linkEstablished = true;
  lastSource = source;
}

bool setManualLeds(const String &value) {
  String text = value;
  text.trim();
  if (text.length() == 0) return false;

  manualMask = maskFromValue(text);
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

String jsonValueInRange(const String &line, const char *key, int start, int end) {
  if (start < 0) start = 0;
  if (end < 0 || end > static_cast<int>(line.length())) end = line.length();
  if (start >= end) return "";

  String pattern = "\"";
  pattern += key;
  pattern += "\"";
  int pos = line.indexOf(pattern, start);
  if (pos < 0 || pos >= end) return "";
  pos = line.indexOf(':', pos + pattern.length());
  if (pos < 0 || pos >= end) return "";
  ++pos;
  while (pos < end && isspace(line[pos])) ++pos;

  if (pos < end && line[pos] == '"') {
    const int valueStart = pos + 1;
    const int valueEnd = line.indexOf('"', valueStart);
    return valueEnd > valueStart && valueEnd <= end ? line.substring(valueStart, valueEnd) : "";
  }

  const int valueStart = pos;
  while (pos < end && line[pos] != ',' && line[pos] != '}') ++pos;
  String value = line.substring(valueStart, pos);
  value.trim();
  return value;
}

bool truthyValue(const String &value, bool fallback) {
  if (value.length() == 0) return fallback;
  if (value == "1" || value == "true" || value == "on") return true;
  if (value == "0" || value == "false" || value == "off") return false;
  return fallback;
}

uint8_t maskFromValue(const String &value, uint8_t fallback) {
  String text = value;
  text.trim();
  if (text.length() == 0) return fallback;

  uint8_t mask = 0;
  if (text.length() == 3 && (text[0] == '0' || text[0] == '1')) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (text[i] == '1') mask |= (1 << i);
    }
    return mask;
  }
  return static_cast<uint8_t>(text.toInt()) & 0x07;
}

void applyCustomSteps(const LedStep *steps, uint8_t count) {
  customStepCount = min<uint8_t>(count, CUSTOM_STEP_MAX);
  for (uint8_t i = 0; i < customStepCount; ++i) customSteps[i] = steps[i];
  customStartedMs = millis();
  setAnim(CUSTOM_ANIM_INDEX);
}

bool setCustomPattern(const String &patternValue, const String &maskValue, uint16_t periodMs) {
  String pattern = patternValue;
  pattern.trim();
  pattern.toLowerCase();
  const uint8_t mask = maskFromValue(maskValue, LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT);
  periodMs = clampStepMs(periodMs);

  LedStep steps[CUSTOM_STEP_MAX];
  uint8_t count = 0;
  if (pattern == "steady") {
    setStep(steps[count++], mask, periodMs);
  } else if (pattern == "blink") {
    setStep(steps[count++], mask, periodMs);
    setStep(steps[count++], 0, periodMs);
  } else if (pattern == "chase") {
    for (uint8_t i = 0; i < 3; ++i) {
      const uint8_t bit = 1 << i;
      if (mask & bit) setStep(steps[count++], bit, periodMs);
    }
  } else if (pattern == "alternate") {
    setStep(steps[count++], static_cast<uint8_t>(mask & (LED_BIT_RUN | LED_BIT_ALERT)), periodMs);
    setStep(steps[count++], static_cast<uint8_t>(mask & LED_BIT_WAIT), periodMs);
  } else if (pattern == "pair_chase" || pattern == "pairchase") {
    setStep(steps[count++], static_cast<uint8_t>(mask & (LED_BIT_RUN | LED_BIT_WAIT)), periodMs);
    setStep(steps[count++], static_cast<uint8_t>(mask & (LED_BIT_WAIT | LED_BIT_ALERT)), periodMs);
    setStep(steps[count++], static_cast<uint8_t>(mask & (LED_BIT_RUN | LED_BIT_ALERT)), periodMs);
  } else {
    return false;
  }

  if (count == 0) setStep(steps[count++], 0, periodMs);
  applyCustomSteps(steps, count);
  return true;
}

bool setCustomEffectFromJson(const String &line) {
  if (line.indexOf("\"effect\"") < 0 && line.indexOf("\"steps\"") < 0 && line.indexOf("\"pattern\"") < 0) {
    return false;
  }

  const int stepsKey = line.indexOf("\"steps\"");
  if (stepsKey >= 0) {
    const int arrayStart = line.indexOf('[', stepsKey);
    const int arrayEnd = line.indexOf(']', arrayStart);
    if (arrayStart < 0 || arrayEnd < 0) return false;

    LedStep steps[CUSTOM_STEP_MAX];
    uint8_t count = 0;
    int pos = arrayStart + 1;
    while (pos < arrayEnd && count < CUSTOM_STEP_MAX) {
      const int objectStart = line.indexOf('{', pos);
      if (objectStart < 0 || objectStart >= arrayEnd) break;
      const int objectEnd = line.indexOf('}', objectStart);
      if (objectEnd < 0 || objectEnd > arrayEnd) break;

      const String maskValue = jsonValueInRange(line, "mask", objectStart, objectEnd);
      String msValue = jsonValueInRange(line, "ms", objectStart, objectEnd);
      if (msValue.length() == 0) msValue = jsonValueInRange(line, "duration", objectStart, objectEnd);
      if (maskValue.length() && msValue.length()) {
        setStep(steps[count++], maskFromValue(maskValue), clampStepMs(msValue.toInt()));
      }
      pos = objectEnd + 1;
    }
    if (count == 0) return false;
    applyCustomSteps(steps, count);
    return true;
  }

  String patternValue = jsonValue(line, "pattern");
  if (patternValue.length() == 0) return false;
  String maskValue = jsonValue(line, "mask");
  if (maskValue.length() == 0) maskValue = "111";
  String periodValue = jsonValue(line, "period");
  if (periodValue.length() == 0) periodValue = jsonValue(line, "period_ms");
  const uint16_t periodMs = periodValue.length() ? periodValue.toInt() : 300;
  return setCustomPattern(patternValue, maskValue, periodMs);
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

    if (setCustomEffectFromJson(line)) {
      handled = true;
      animId = "";
      ledsValue = "";
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
        strncpy(blePendingBuf, bleLine.c_str(), 512);
        blePendingBuf[512] = '\0';
        blePendingCmd = true;
        taskEXIT_CRITICAL(&bleCmdMux);
        bleLine = "";
      } else if (bleLine.length() < 512) {
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
    } else if (serialLine.length() < 512) {
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

void pollBleCommands() {
  bool hasPending = false;
  char localBuf[513];
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

  pinMode(LED_COMMON_ANODE, OUTPUT);
  digitalWrite(LED_COMMON_ANODE, HIGH);

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
  Serial.println("Serial command examples: anim=typing, led=101, {\"leds\":\"010\"}, {\"effect\":{\"pattern\":\"blink\",\"mask\":\"111\",\"period\":250}}, next, state");
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

  pollSerialCommands();
  if (BLE_ENABLED) {
    pollBleState();
    pollBleCommands();
  }
  updateLeds();

  if (autoCycle && (int32_t)(millis() - nextAutoCycleMs) >= 0) {
    nextAutoCycleMs = millis() + 6000;
    setAnim((animIndex + 1) % (ANIM_COUNT - 1), true);
  }
}
