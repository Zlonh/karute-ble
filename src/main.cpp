// karute-ble firmware.
// Replaces the original project's Wi-Fi softAP + web UI with a BLE GATT
// service (Nordic UART Service). The USB HID typing engine, NVS settings,
// LittleFS draft storage, RGB LED policy and physical button are preserved.
//
// App talks to this device by writing newline-terminated lines to the NUS RX
// characteristic; see ble_protocol.h for the command grammar.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <cstring>
#include <string>

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "esp32-hal-rgb-led.h"

#include "alt_encoding.h"
#include "ble_protocol.h"
#include "led_policy.h"
#include "typing_settings.h"

namespace {

// ---- Pins (override per-board via platformio.ini build_flags) ----------------
#ifndef KARUTE_BUTTON_PIN
#define KARUTE_BUTTON_PIN 41  // AtomS3U user button
#endif
#ifndef KARUTE_LED_PIN
#define KARUTE_LED_PIN 35  // AtomS3U RGB LED
#endif

constexpr size_t kMaxTextBytes = 32 * 1024;
constexpr char kFirmwareVersion[] = "karute-ble-1.0";
constexpr char kDeviceName[] = "karute-ble";
constexpr char kTextPath[] = "/body.txt";
constexpr char kTempTextPath[] = "/body.tmp";

// Nordic UART Service (NUS): de-facto standard, any BLE serial app speaks it.
constexpr char kNusService[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char kNusRx[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // app -> device
constexpr char kNusTx[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // device -> app

constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kStopHoldMs = 1000;
constexpr uint32_t kNotifyThrottleMs = 300;
constexpr uint32_t kRunDialogOpenWaitMs = 0;  // unused, kept for reference

enum class DeviceState { EMPTY, READY, TYPING, PAUSED, STOPPED, DONE, ERROR };
enum class LedEvent { NONE, BOOT };

USBHIDKeyboard keyboard;
USBHID altTransport;

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTx = nullptr;

TypingSettings typingSettings;

String currentText;
size_t currentCharacterCount = 0;
DeviceState deviceState = DeviceState::EMPTY;
bool storageReady = false;

bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
uint32_t rawStateChangedAtMs = 0;
bool startRequested = false;
bool abortRequested = false;
bool pauseRequested = false;
bool resumeRequested = false;
size_t typingIndex = 0;
size_t completedCharacters = 0;
uint32_t runEstimatedMs = 0;
uint32_t startDelayEndsAtMs = 0;
bool hasPrintProgress = false;
bool typingButtonArmed = false;
bool typingStopTracking = false;
uint32_t typingStopPressedAtMs = 0;
bool ignoreButtonUntilRelease = false;
uint32_t buttonPressedAtMs = 0;
bool startDelayActive = false;
bool typingIndicatorKeyDown = false;
bool altCharacterActive = false;
String inputError;

LedEvent ledEvent = LedEvent::BOOT;
uint32_t ledEventStartedAtMs = 0;

bool gConnected = false;
karute_ble::LineBuffer gLineBuffer;
uint32_t lastNotifyAtMs = 0;

volatile bool hostLedsKnown = false;
volatile uint8_t hostLeds = 0;

// ---------------------------------------------------------------------------
// UTF-8 validation and draft storage (kept from the original firmware).
// ---------------------------------------------------------------------------

struct Utf8CountResult {
  bool valid;
  size_t characters;
};

Utf8CountResult validateAndCountUtf8(const String& text) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.c_str());
  const size_t length = text.length();
  size_t index = 0;
  size_t characters = 0;
  while (index < length) {
    const uint8_t first = bytes[index];
    size_t seq = 0;
    if (first <= 0x7F) seq = 1;
    else if (first >= 0xC2 && first <= 0xDF) seq = 2;
    else if (first >= 0xE0 && first <= 0xEF) seq = 3;
    else if (first >= 0xF0 && first <= 0xF4) seq = 4;
    else return {false, 0};
    if (index + seq > length) return {false, 0};
    for (size_t o = 1; o < seq; ++o)
      if ((bytes[index + o] & 0xC0) != 0x80) return {false, 0};
    if (seq == 3 &&
        ((first == 0xE0 && bytes[index + 1] < 0xA0) ||
         (first == 0xED && bytes[index + 1] >= 0xA0)))
      return {false, 0};
    if (seq == 4 &&
        ((first == 0xF0 && bytes[index + 1] < 0x90) ||
         (first == 0xF4 && bytes[index + 1] > 0x8F)))
      return {false, 0};
    index += seq;
    ++characters;
  }
  return {true, characters};
}

bool persistText(const String& text) {
  if (!storageReady) return false;
  if (text.isEmpty()) {
    LittleFS.remove(kTempTextPath);
    return !LittleFS.exists(kTextPath) || LittleFS.remove(kTextPath);
  }
  File file = LittleFS.open(kTempTextPath, "w");
  if (!file) return false;
  const size_t written =
      file.write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
  file.flush();
  file.close();
  if (written != text.length()) {
    LittleFS.remove(kTempTextPath);
    return false;
  }
  if (!LittleFS.rename(kTempTextPath, kTextPath)) {
    LittleFS.remove(kTempTextPath);
    return false;
  }
  return true;
}

bool loadStoredText() {
  if (!storageReady) return false;
  if (!LittleFS.exists(kTextPath)) return true;
  File file = LittleFS.open(kTextPath, "r");
  if (!file || file.size() > kMaxTextBytes) return false;
  String loaded;
  if (!loaded.reserve(file.size())) return false;
  while (file.available()) loaded += static_cast<char>(file.read());
  if (loaded.length() != file.size()) return false;
  const Utf8CountResult count = validateAndCountUtf8(loaded);
  if (!count.valid) return false;
  currentText = loaded;
  currentCharacterCount = count.characters;
  deviceState = currentText.isEmpty() ? DeviceState::EMPTY : DeviceState::READY;
  return true;
}

bool initializeStorage() {
  Preferences preferences;
  if (!preferences.begin("karute", false)) return false;
  const bool initializedBefore = preferences.getBool("fsinit", false);
  const bool mounted = LittleFS.begin(!initializedBefore);
  if (mounted && !initializedBefore) preferences.putBool("fsinit", true);
  preferences.end();
  return mounted;
}

const char* stateName(DeviceState state) {
  switch (state) {
    case DeviceState::EMPTY: return "EMPTY";
    case DeviceState::READY: return "READY";
    case DeviceState::TYPING: return "TYPING";
    case DeviceState::PAUSED: return "PAUSED";
    case DeviceState::STOPPED: return "STOPPED";
    case DeviceState::DONE: return "DONE";
    case DeviceState::ERROR: return "ERROR";
  }
  return "ERROR";
}

// ---------------------------------------------------------------------------
// Status LED (unchanged policy from the original firmware).
// ---------------------------------------------------------------------------

void writeStatusLed(uint8_t r, uint8_t g, uint8_t b) {
  static uint8_t lr = 255, lg = 255, lb = 255;
  if (r == lr && g == lg && b == lb) return;
  neopixelWrite(KARUTE_LED_PIN, r, g, b);
  lr = r; lg = g; lb = b;
}

void updateStatusLed() {
  static DeviceState observed = DeviceState::EMPTY;
  static uint32_t changedAt = 0;
  const uint32_t now = millis();
  if (observed != deviceState) { observed = deviceState; changedAt = now; }

  if (deviceState == DeviceState::TYPING) {
    if (!typingSettings.inputLedEnabled) {
      writeStatusLed(0, 0, 0);
    } else if (startDelayActive) {
      const bool lit = ((now / 350) % 2) == 0;
      writeStatusLed(0, lit ? 14 : 2, 0);
    } else {
      writeStatusLed(0, typingIndicatorKeyDown ? 16 : 0, 0);
    }
    return;
  }
  if (!typingSettings.standbyLedEnabled) { writeStatusLed(0, 0, 0); return; }
  if (ledEvent == LedEvent::BOOT) {
    if ((now - ledEventStartedAtMs) < 500) { writeStatusLed(18, 18, 18); return; }
    ledEvent = LedEvent::NONE;
  }
  if (deviceState == DeviceState::PAUSED) { writeStatusLed(0, 3, 16); return; }
  if (deviceState == DeviceState::STOPPED) { writeStatusLed(18, 0, 0); return; }
  if (deviceState == DeviceState::ERROR) {
    const uint32_t e = now - changedAt;
    const bool lit = e < 180 || (e >= 320 && e < 500) || e >= 650;
    writeStatusLed(lit ? 18 : 0, 0, 0);
    return;
  }
  // Standby: cyan when a phone is connected, dim green otherwise.
  writeStatusLed(gConnected ? 0 : 0, gConnected ? 20 : 10, gConnected ? 40 : 0);
}

// ---------------------------------------------------------------------------
// Settings (NVS, same layout as the original firmware).
// ---------------------------------------------------------------------------

uint32_t estimateCurrentTextMs();
bool persistTypingSettings(const TypingSettings& updated);

void loadTypingSettings() {
  Preferences preferences;
  if (!preferences.begin("karute", true)) return;
  TypingSettings loaded;
  if (preferences.getBytesLength("cfg9") == sizeof(loaded) &&
      preferences.getBytes("cfg9", &loaded, sizeof(loaded)) == sizeof(loaded) &&
      validTypingSettings(loaded)) {
    typingSettings = loaded;
  } else {
    // No migration path here: BLE product ships fresh NVS defaults.
    typingSettings = TypingSettings{};
  }
  preferences.end();
}

bool persistTypingSettings(const TypingSettings& updated) {
  Preferences preferences;
  if (!preferences.begin("karute", false)) return false;
  const bool ok =
      preferences.putBytes("cfg9", &updated, sizeof(updated)) == sizeof(updated);
  preferences.end();
  if (ok) typingSettings = updated;
  return ok;
}

// ---------------------------------------------------------------------------
// BLE output
// ---------------------------------------------------------------------------

void notifyRaw(const std::string& msg) {
  if (pTx == nullptr || !gConnected) return;
  pTx->notify(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(msg.data())),
              msg.size());
}

void sendLine(const String& body) {
  notifyRaw(std::string(body.c_str()) + "\n");
}

void notifyState() {
  String out = "STATE ";
  out += stateName(deviceState);
  out += "|";
  out += currentText.length();
  out += "|";
  out += currentCharacterCount;
  out += "|";
  if (hasPrintProgress && currentCharacterCount)
    out += static_cast<unsigned>(completedCharacters * 100 / currentCharacterCount);
  else
    out += "0";
  out += "|";
  uint32_t remainingMs = 0;
  if (hasPrintProgress && currentCharacterCount > completedCharacters) {
    const size_t remaining = currentCharacterCount - completedCharacters;
    remainingMs = static_cast<uint32_t>(
        (static_cast<uint64_t>(runEstimatedMs) * remaining + currentCharacterCount - 1) /
        currentCharacterCount);
  }
  if (startDelayActive) {
    const int32_t left = static_cast<int32_t>(startDelayEndsAtMs - millis());
    if (left > 0) remainingMs += left;
  }
  out += remainingMs;
  out += "|";
  out += inputError;
  sendLine(out);
}

void maybeNotifyState() {
  const uint32_t now = millis();
  if (now - lastNotifyAtMs >= kNotifyThrottleMs) {
    lastNotifyAtMs = now;
    notifyState();
  }
}

// ---------------------------------------------------------------------------
// Typing engine (kept from the original firmware).
// ---------------------------------------------------------------------------

const SpeedSettings& activeSpeed() {
  if (isAltMode(typingSettings.mode)) return typingSettings.alt;
  return typingSettings.mode == 2 ? typingSettings.eng : typingSettings.vuc;
}

void checkPhysicalStopButton() {
  const bool pressed = (digitalRead(KARUTE_BUTTON_PIN) == LOW);
  if (!typingButtonArmed) {
    if (!pressed) typingButtonArmed = true;
    return;
  }
  if (!pressed) { typingStopTracking = false; return; }
  if (!typingStopTracking) {
    typingStopTracking = true;
    typingStopPressedAtMs = millis();
    return;
  }
  if ((millis() - typingStopPressedAtMs) >= kStopHoldMs) abortRequested = true;
}

bool waitWhileServing(uint32_t ms) {
  const uint32_t started = millis();
  while ((millis() - started) < ms) {
    checkPhysicalStopButton();
    updateStatusLed();
    maybeNotifyState();
    if (abortRequested && !altCharacterActive) return false;
    delay(1);
  }
  return !abortRequested || altCharacterActive;
}

bool decodeNextCodePoint(const String& text, size_t& index, uint32_t& codePoint) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.c_str());
  const size_t length = text.length();
  if (index >= length) return false;
  const uint8_t first = bytes[index++];
  if (first <= 0x7F) { codePoint = first; return true; }
  size_t cont = 0;
  if (first >= 0xC2 && first <= 0xDF) { cont = 1; codePoint = first & 0x1F; }
  else if (first >= 0xE0 && first <= 0xEF) { cont = 2; codePoint = first & 0x0F; }
  else if (first >= 0xF0 && first <= 0xF4) { cont = 3; codePoint = first & 0x07; }
  else return false;
  if (index + cont > length) return false;
  for (size_t i = 0; i < cont; ++i) {
    const uint8_t n = bytes[index++];
    if ((n & 0xC0) != 0x80) return false;
    codePoint = (codePoint << 6) | (n & 0x3F);
  }
  if ((cont == 1 && codePoint < 0x80) || (cont == 2 && codePoint < 0x800) ||
      (cont == 3 && codePoint < 0x10000) ||
      (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF)
    return false;
  return true;
}

bool validateAltText(String& error) {
  if (!isAltMode(typingSettings.mode)) return true;
  size_t index = 0, pos = 0;
  while (index < currentText.length()) {
    uint32_t cp;
    if (!decodeNextCodePoint(currentText, index, cp)) {
      error = "UTF-8格式错误，未开始打印。";
      return false;
    }
    ++pos;
    uint16_t encoded;
    if (!karute_alt::outputCode(karute_alt::Mode::GBK, cp, encoded)) {
      char detail[110];
      snprintf(detail, sizeof(detail), "第%u个字符 U+%04lX 不支持当前模式，整篇未打印。",
               static_cast<unsigned>(pos), static_cast<unsigned long>(cp));
      error = detail;
      return false;
    }
  }
  return true;
}

bool altTransportReady(String& error) {
  if (!altTransport.ready()) { error = "USB键盘未就绪。"; return false; }
  if (hostLedsKnown && !(hostLeds & LED_NUMLOCK)) {
    error = "请开启电脑的Num Lock，再打印。";
    return false;
  }
  return true;
}

bool altStartReady(String& error) {
  if (!validateAltText(error)) return false;
  if (!isAltMode(typingSettings.mode)) return true;
  return altTransportReady(error);
}

bool sendAltReport(uint8_t modifier, uint8_t usage) {
  KeyReport report = {};
  static_assert(sizeof(report) == 8, "Standard keyboard report size");
  report.modifiers = modifier;
  report.keys[0] = usage;
  const bool ok = altTransport.SendReport(HID_REPORT_ID_KEYBOARD, &report,
                                         sizeof(report), 100);
  const uint8_t indicatorMode = typingSettings.mode;
  typingIndicatorKeyDown =
      typingSettings.inputLedEnabled && ok &&
      karute_led::altIndicatorKey(indicatorMode, usage);
  updateStatusLed();
  return ok;
}

bool typeAltCodePoint(uint32_t cp) {
  uint16_t value;
  if (!karute_alt::outputCode(karute_alt::Mode::GBK, cp, value)) return false;
  altCharacterActive = true;
  const bool ok = karute_alt::emitTimedCode(
      value, typingSettings.alt.keyDownMs, typingSettings.alt.interKeyMs,
      typingSettings.altBeforeMs, typingSettings.altReleaseMs,
      typingSettings.alt.afterSpaceMs,
      [](uint8_t m, uint8_t u) { return sendAltReport(m, u); },
      [](uint32_t ms) { return waitWhileServing(ms); });
  altCharacterActive = false;
  if (!ok) {
    sendAltReport(0, 0);
    inputError = "USB发送失败，已停止。";
  }
  return ok;
}

size_t vucHexDigitCount(uint32_t codePoint) {
  size_t digits = 1;
  while (codePoint >= 16) { codePoint >>= 4; ++digits; }
  return digits < 4 ? 4 : digits;
}

uint64_t estimateVucCodePointMs(uint32_t codePoint) {
  const SpeedSettings& speed = typingSettings.vuc;
  const size_t keys = 3 + vucHexDigitCount(codePoint);
  uint64_t total = static_cast<uint64_t>(keys + 1) * speed.keyDownMs;
  total += static_cast<uint64_t>(keys - 1) * speed.interKeyMs;
  total += speed.beforeSpaceMs + speed.afterSpaceMs;
  return total;
}

uint32_t estimateCurrentTextMs() {
  if (currentText.isEmpty()) return 0;
  uint64_t total = typingSettings.startDelayMs;
  const uint32_t vucTap = typingSettings.vuc.keyDownMs + typingSettings.vuc.interKeyMs;
  const uint32_t engTap = typingSettings.eng.keyDownMs + typingSettings.eng.interKeyMs;
  size_t index = 0;
  while (index < currentText.length()) {
    uint32_t cp = 0;
    if (!decodeNextCodePoint(currentText, index, cp)) break;
    if (cp == '\r' && index < currentText.length() && currentText[index] == '\n') continue;
    if (isAltMode(typingSettings.mode)) {
      uint16_t value;
      if (karute_alt::outputCode(karute_alt::Mode::GBK, cp, value))
        total += karute_alt::timedEstimateMs(value, typingSettings.alt.keyDownMs,
                                             typingSettings.alt.interKeyMs,
                                             typingSettings.altBeforeMs,
                                             typingSettings.altReleaseMs,
                                             typingSettings.alt.afterSpaceMs);
      continue;
    }
    if (cp == '\r' || cp == '\n' || cp == '\t' || cp == '\b' || cp == ' ')
      total += typingSettings.mode == 2 ? engTap : vucTap;
    else if (typingSettings.mode == 2 && cp >= 0x20 && cp <= 0x7E) total += engTap;
    else if (typingSettings.mode == 1 && cp >= 0x20) total += estimateVucCodePointMs(cp);
  }
  return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}

bool tapAsciiKey(char c, const SpeedSettings& speed, uint16_t afterMs) {
  if (c < 0x20 || c > 0x7E) return false;
  const bool flash = typingSettings.inputLedEnabled &&
                     karute_led::asciiIndicatorKey(typingSettings.mode, c);
  if (flash) { typingIndicatorKeyDown = true; updateStatusLed(); }
  if (keyboard.press(static_cast<uint8_t>(c)) == 0) {
    keyboard.releaseAll();
    if (flash) { typingIndicatorKeyDown = false; updateStatusLed(); }
    return false;
  }
  const bool downOk = waitWhileServing(speed.keyDownMs);
  keyboard.releaseAll();
  if (flash) { typingIndicatorKeyDown = false; updateStatusLed(); }
  if (!downOk) return false;
  return waitWhileServing(afterMs);
}

bool tapSpecialKey(uint8_t key, const SpeedSettings& speed, uint16_t afterMs) {
  if (keyboard.press(key) == 0) { keyboard.releaseAll(); return false; }
  if (!waitWhileServing(speed.keyDownMs)) { keyboard.releaseAll(); return false; }
  keyboard.releaseAll();
  return waitWhileServing(afterMs);
}

bool typeUnicodeCodePoint(uint32_t cp) {
  constexpr char kHex[] = "0123456789abcdef";
  char reversed[8];
  size_t len = 0;
  do {
    reversed[len++] = kHex[cp & 0x0F];
    cp >>= 4;
  } while (cp != 0 && len < sizeof(reversed));
  while (len < 4) reversed[len++] = '0';

  const SpeedSettings& speed = typingSettings.vuc;
  if (!tapAsciiKey('v', speed, speed.interKeyMs) ||
      !tapAsciiKey('u', speed, speed.interKeyMs) ||
      !tapAsciiKey('c', speed, speed.interKeyMs))
    return false;
  while (len > 0) {
    const char d = reversed[--len];
    const uint16_t wait = len == 0 ? speed.beforeSpaceMs : speed.interKeyMs;
    if (!tapAsciiKey(d, speed, wait)) return false;
  }
  return tapAsciiKey(' ', speed, speed.afterSpaceMs);
}

void finishStoppedRun() {
  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  abortRequested = false;
  pauseRequested = false;
  startDelayActive = false;
  typingStopTracking = false;
  ignoreButtonUntilRelease = true;
  typingIndex = 0;
  deviceState = currentText.isEmpty() ? DeviceState::EMPTY : DeviceState::STOPPED;
}

void finishPausedRun() {
  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  pauseRequested = false;
  startDelayActive = false;
  typingStopTracking = false;
  if (digitalRead(KARUTE_BUTTON_PIN) == LOW) ignoreButtonUntilRelease = true;
  deviceState = DeviceState::PAUSED;
}

void typeCurrentText(bool resume) {
  if (currentText.isEmpty()) { deviceState = DeviceState::EMPTY; return; }
  inputError = "";
  if (!altStartReady(inputError)) { deviceState = DeviceState::STOPPED; return; }

  abortRequested = false;
  pauseRequested = false;
  typingStopTracking = false;
  typingButtonArmed = (digitalRead(KARUTE_BUTTON_PIN) == HIGH);
  deviceState = DeviceState::TYPING;
  typingIndicatorKeyDown = false;
  keyboard.releaseAll();
  notifyState();

  if (!resume) {
    typingIndex = 0;
    completedCharacters = 0;
    hasPrintProgress = true;
    runEstimatedMs = estimateCurrentTextMs() - typingSettings.startDelayMs;
    startDelayEndsAtMs = millis() + typingSettings.startDelayMs;
    startDelayActive = typingSettings.startDelayMs > 0;
    if (!waitWhileServing(typingSettings.startDelayMs)) {
      startDelayActive = false;
      finishStoppedRun();
      return;
    }
    startDelayActive = false;
    if (pauseRequested && typingIndex < currentText.length()) {
      finishPausedRun();
      return;
    }
  }
  updateStatusLed();

  while (typingIndex < currentText.length()) {
    if (abortRequested) { finishStoppedRun(); return; }
    uint32_t cp = 0;
    if (!decodeNextCodePoint(currentText, typingIndex, cp)) {
      deviceState = DeviceState::ERROR;
      keyboard.releaseAll();
      return;
    }
    if (cp == '\r' && typingIndex < currentText.length() && currentText[typingIndex] == '\n') {
      ++completedCharacters;
      continue;
    }

    bool ok = true;
    if (isAltMode(typingSettings.mode)) {
      ok = typeAltCodePoint(cp);
    } else {
      const SpeedSettings& ctl = activeSpeed();
      if (cp == '\r' || cp == '\n') ok = tapSpecialKey(KEY_RETURN, ctl, ctl.interKeyMs);
      else if (cp == '\t') ok = tapSpecialKey(KEY_TAB, ctl, ctl.interKeyMs);
      else if (cp == '\b') ok = tapSpecialKey(KEY_BACKSPACE, ctl, ctl.interKeyMs);
      else if (cp == ' ') ok = tapAsciiKey(' ', ctl, ctl.interKeyMs);
      else if (typingSettings.mode == 2 && cp >= 0x20 && cp <= 0x7E)
        ok = tapAsciiKey(static_cast<char>(cp), typingSettings.eng,
                         typingSettings.eng.interKeyMs);
      else if (typingSettings.mode == 1 && cp >= 0x20)
        ok = typeUnicodeCodePoint(cp);
      else
        ok = typingSettings.mode == 2;  // ENG skips non-ASCII.
    }

    if (!ok) {
      if (abortRequested) finishStoppedRun();
      else { deviceState = DeviceState::ERROR; keyboard.releaseAll(); }
      notifyState();
      return;
    }
    ++completedCharacters;
    maybeNotifyState();
    if (abortRequested) { finishStoppedRun(); return; }
    if (pauseRequested && typingIndex < currentText.length()) {
      finishPausedRun();
      notifyState();
      return;
    }
  }
  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  if (typingStopTracking || digitalRead(KARUTE_BUTTON_PIN) == LOW)
    ignoreButtonUntilRelease = true;
  typingStopTracking = false;
  deviceState = DeviceState::DONE;
  typingIndex = 0;
  notifyState();
}

// ---------------------------------------------------------------------------
// Settings application and command dispatch.
// ---------------------------------------------------------------------------

bool parseUint16(const String& v, uint16_t min, uint16_t max, uint16_t& out) {
  if (v.isEmpty()) return false;
  for (size_t i = 0; i < v.length(); ++i)
    if (v[i] < '0' || v[i] > '9') return false;
  const long n = v.toInt();
  if (n < min || n > max) return false;
  out = static_cast<uint16_t>(n);
  return true;
}

bool applySettingsBody(const String& body, String& err) {
  TypingSettings cand = typingSettings;
  int pos = 0;
  while (pos < (int)body.length()) {
    int sp = body.indexOf(' ', pos);
    if (sp < 0) sp = body.length();
    const String token = body.substring(pos, sp);
    pos = sp + 1;
    if (token.isEmpty()) continue;
    const int eq = token.indexOf('=');
    if (eq < 0) { err = "设置项格式错误: " + token; return false; }
    const String key = token.substring(0, eq);
    const String val = token.substring(eq + 1);

    uint16_t u16;
    if (key == "mode") {
      uint16_t m;
      if (!parseUint16(val, 1, 4, m)) { err = "mode 取值需为 1/2/4"; return false; }
      if (!isValidMode(m)) { err = "mode 取值需为 1/2/4"; return false; }
      cand.mode = m;
    } else if (key == "vucDownMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "vucDownMs 超出 1~100"; return false; }
      cand.vuc.keyDownMs = u16;
    } else if (key == "vucIntervalMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "vucIntervalMs 超出 1~100"; return false; }
      cand.vuc.interKeyMs = u16;
    } else if (key == "vucBeforeSpaceMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "vucBeforeSpaceMs 超出 1~100"; return false; }
      cand.vuc.beforeSpaceMs = u16;
    } else if (key == "vucAfterSpaceMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "vucAfterSpaceMs 超出 1~100"; return false; }
      cand.vuc.afterSpaceMs = u16;
    } else if (key == "engDownMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "engDownMs 超出 1~100"; return false; }
      cand.eng.keyDownMs = u16;
    } else if (key == "engIntervalMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "engIntervalMs 超出 1~100"; return false; }
      cand.eng.interKeyMs = u16;
    } else if (key == "altBeforeMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "altBeforeMs 超出 1~100"; return false; }
      cand.altBeforeMs = u16;
    } else if (key == "altDownMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "altDownMs 超出 1~100"; return false; }
      cand.alt.keyDownMs = u16;
    } else if (key == "altIntervalMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "altIntervalMs 超出 1~100"; return false; }
      cand.alt.interKeyMs = u16;
    } else if (key == "altReleaseMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "altReleaseMs 超出 1~100"; return false; }
      cand.altReleaseMs = u16;
    } else if (key == "altAfterMs") {
      if (!parseUint16(val, 1, 100, u16)) { err = "altAfterMs 超出 1~100"; return false; }
      cand.alt.afterSpaceMs = u16;
    } else if (key == "startDelayMs") {
      long n = val.toInt();
      if (n < 0 || n > 10000) { err = "startDelayMs 超出 0~10000"; return false; }
      cand.startDelayMs = static_cast<uint16_t>(n);
    } else if (key == "inputLed") {
      if (val != "0" && val != "1") { err = "inputLed 需为 0/1"; return false; }
      cand.inputLedEnabled = val == "1";
    } else if (key == "standbyLed") {
      if (val != "0" && val != "1") { err = "standbyLed 需为 0/1"; return false; }
      cand.standbyLedEnabled = val == "1";
    } else {
      err = "未知设置项: " + key;
      return false;
    }
  }
  if (!validTypingSettings(cand)) { err = "组合后设置非法"; return false; }
  if (!persistTypingSettings(cand)) { err = "NVS 写入失败"; return false; }
  return true;
}

void handleCommand(const karute_ble::Command& cmd) {
  switch (cmd.kind) {
    case karute_ble::CommandKind::kPing:
      notifyRaw("PONG\n");
      return;
    case karute_ble::CommandKind::kStatus:
      notifyState();
      return;
    case karute_ble::CommandKind::kReset:
      if (deviceState == DeviceState::TYPING || deviceState == DeviceState::PAUSED) {
        sendLine("ERR 打印期间不能清空。");
        return;
      }
      currentText = "";
      currentCharacterCount = 0;
      hasPrintProgress = false;
      completedCharacters = 0;
      persistText("");
      deviceState = DeviceState::EMPTY;
      sendLine("OK");
      notifyState();
      return;
    case karute_ble::CommandKind::kAppendText: {
      if (deviceState == DeviceState::TYPING || deviceState == DeviceState::PAUSED) {
        sendLine("ERR 打印期间不能修改正文。");
        return;
      }
      const std::string decoded = karute_ble::base64Decode(cmd.payload);
      if (decoded.empty()) { sendLine("ERR base64 解码失败。"); return; }
      if (currentText.length() + decoded.size() > kMaxTextBytes) {
        sendLine("ERR 正文超过 32KB 上限。");
        return;
      }
      currentText += String(decoded.c_str(), decoded.size());
      const Utf8CountResult count = validateAndCountUtf8(currentText);
      if (!count.valid) {
        // Roll back the chunk.
        currentText = currentText.substring(0, currentText.length() - decoded.size());
        sendLine("ERR UTF-8 格式错误。");
        return;
      }
      currentCharacterCount = count.characters;
      deviceState = currentText.isEmpty() ? DeviceState::EMPTY : DeviceState::READY;
      sendLine("OK");
      notifyState();
      return;
    }
    case karute_ble::CommandKind::kStart: {
      if (deviceState == DeviceState::TYPING) { sendLine("ERR 正在输入。"); return; }
      if (deviceState == DeviceState::PAUSED) { sendLine("ERR 已暂停，请用 RESUME。"); return; }
      if (currentText.isEmpty()) { sendLine("ERR 没有正文。"); return; }
      String err;
      if (!altStartReady(err)) { sendLine("ERR " + err); return; }
      // Persist the draft once, right before typing.
      persistText(currentText);
      startRequested = true;
      sendLine("OK");
      return;
    }
    case karute_ble::CommandKind::kPause:
      if (deviceState != DeviceState::TYPING) { sendLine("ERR 没有正在打印。"); return; }
      pauseRequested = true;
      sendLine("OK");
      return;
    case karute_ble::CommandKind::kResume:
      if (deviceState != DeviceState::PAUSED) { sendLine("ERR 当前未暂停。"); return; }
      resumeRequested = true;
      sendLine("OK");
      return;
    case karute_ble::CommandKind::kStop:
      if (deviceState != DeviceState::TYPING && deviceState != DeviceState::PAUSED) {
        sendLine("ERR 设备空闲。");
        return;
      }
      if (deviceState == DeviceState::PAUSED) finishStoppedRun();
      else abortRequested = true;
      sendLine("OK");
      notifyState();
      return;
    case karute_ble::CommandKind::kSet: {
      if (deviceState == DeviceState::TYPING || deviceState == DeviceState::PAUSED) {
        sendLine("ERR 打印期间不能改设置。");
        return;
      }
      String err;
      if (!applySettingsBody(String(cmd.payload.c_str()), err)) {
        sendLine("ERR " + err);
        return;
      }
      sendLine("OK");
      notifyState();
      return;
    }
    default:
      sendLine("ERR 未知命令。");
      return;
  }
}

void updateButton() {
  const bool raw = (digitalRead(KARUTE_BUTTON_PIN) == LOW);
  const uint32_t now = millis();
  if (ignoreButtonUntilRelease) {
    lastRawButtonPressed = raw;
    stableButtonPressed = raw;
    rawStateChangedAtMs = now;
    if (!raw) ignoreButtonUntilRelease = false;
    return;
  }
  if (raw != lastRawButtonPressed) {
    lastRawButtonPressed = raw;
    rawStateChangedAtMs = now;
  }
  if (stableButtonPressed && raw && deviceState == DeviceState::PAUSED &&
      (now - buttonPressedAtMs) >= kStopHoldMs) {
    finishStoppedRun();
    notifyState();
    return;
  }
  if ((now - rawStateChangedAtMs) < kDebounceMs || raw == stableButtonPressed) return;
  stableButtonPressed = raw;
  if (stableButtonPressed) { buttonPressedAtMs = now; return; }
  if ((now - buttonPressedAtMs) >= kStopHoldMs) return;  // long press = stop
  if (deviceState == DeviceState::READY || deviceState == DeviceState::DONE ||
      deviceState == DeviceState::STOPPED) {
    startRequested = true;
  } else if (deviceState == DeviceState::PAUSED) {
    resumeRequested = true;
  }
}

// ---------------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------------

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    gConnected = true;
    updateStatusLed();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    gConnected = false;
    NimBLEDevice::startAdvertising();
    updateStatusLed();
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, NimBLEConnInfo&) override {
    const std::string v = ch->getValue();
    gLineBuffer.feed(v.data(), v.size());
  }
};

void setupBle() {
  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setMTU(247);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCb());
  NimBLEService* pService = pServer->createService(kNusService);
  pTx = pService->createCharacteristic(kNusTx, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pRx = pService->createCharacteristic(
      kNusRx, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRx->setCallbacks(new RxCb());
  pService->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(kNusService);
  pAdv->setName(kDeviceName);
  pAdv->start();
}

}  // namespace

void setup() {
#if ARDUINO_USB_MODE != 0
#error "Requires native USB-OTG/TinyUSB mode."
#endif
#if ARDUINO_USB_CDC_ON_BOOT != 0 || ARDUINO_USB_MSC_ON_BOOT != 0 || \
    ARDUINO_USB_DFU_ON_BOOT != 0
#error "Runtime USB must expose only the HID keyboard."
#endif
  pinMode(KARUTE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(KARUTE_LED_PIN, OUTPUT);
  ledEventStartedAtMs = millis();

  loadTypingSettings();
  updateStatusLed();

  keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT,
      [](void*, esp_event_base_t, int32_t, void* data) {
        hostLeds = static_cast<arduino_usb_hid_keyboard_event_data_t*>(data)->leds;
        hostLedsKnown = true;
      });
  keyboard.begin();
  USB.manufacturerName("Karute");
  USB.productName("karute-ble keyboard");
  USB.serialNumber("KARUTE-BLE-1");
  USB.usbClass(0x00);
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.webUSB(false);
  USB.begin();

  currentText.reserve(kMaxTextBytes);
  storageReady = initializeStorage();
  if (!storageReady || !loadStoredText()) {
    deviceState = DeviceState::ERROR;
    inputError = "存储初始化失败。";
  }

  setupBle();
}

void loop() {
  updateButton();
  updateStatusLed();

  std::string line;
  while (gLineBuffer.pop(line)) {
    handleCommand(karute_ble::parseLine(line));
  }

  if (startRequested &&
      (deviceState == DeviceState::READY || deviceState == DeviceState::DONE ||
       deviceState == DeviceState::STOPPED)) {
    startRequested = false;
    typeCurrentText(false);
  }
  if (resumeRequested && deviceState == DeviceState::PAUSED) {
    resumeRequested = false;
    typeCurrentText(true);
  }
  delay(2);
}
