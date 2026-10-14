// BLE command framing shared by firmware and (reimplemented in) the Android app.
// Pure logic, no Arduino dependency, so it can be unit-tested with g++.
//
// Wire format: each command is one '\n'-terminated line sent to the NUS RX
// characteristic. BLE may split a write mid-line; LineBuffer reassembles.
//
//   T <base64>          append base64-decoded UTF-8 bytes to the draft text
//   C RESET             clear the draft text
//   C START             start typing (after startDelay)
//   C PAUSE | RESUME | STOP
//   C STATUS            ask the device to emit a STATE notification immediately
//   PING                -> device replies PONG
//   S k=v k=v ...       update settings, e.g.  S mode=4 vucDownMs=5
//
// Device -> app (notify on TX):
//   OK                  command accepted
//   ERR <message>       command rejected (UTF-8, Chinese allowed)
//   STATE <fields>     pipe-separated:
//                       state|bytes|characters|progressPct|remainingMs|error
//
// The draft text itself is never base64-split on purpose: the app sends as
// many T lines as it needs; the device appends them in arrival order.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace karute_ble {

enum class CommandKind {
  kAppendText,
  kReset,
  kStart,
  kPause,
  kResume,
  kStop,
  kStatus,
  kPing,
  kSet,
  kUnknown,
};

struct Command {
  CommandKind kind = CommandKind::kUnknown;
  std::string payload;  // kAppendText: raw base64 body; kSet: unparsed body
};

// Accumulates partial writes until a newline completes a command line.
class LineBuffer {
 public:
  void feed(const char* data, size_t n) {
    if (data == nullptr || n == 0) return;
    for (size_t i = 0; i < n; ++i) {
      if (data[i] == '\n' || data[i] == '\r') {
        if (!line_.empty()) {
          complete_.push_back(line_);
          line_.clear();
        }
      } else {
        line_.push_back(data[i]);
      }
    }
  }

  // Pop the next complete line. Returns false when none is pending.
  bool pop(std::string& out) {
    if (complete_.empty()) return false;
    out = complete_.front();
    complete_.erase(complete_.begin());
    return true;
  }

 private:
  std::string line_;
  std::vector<std::string> complete_;
};

inline Command parseLine(const std::string& raw) {
  Command result;
  if (raw.empty()) return result;
  const size_t sp = raw.find(' ');
  const std::string head = raw.substr(0, sp);
  const std::string rest = sp == std::string::npos ? "" : raw.substr(sp + 1);

  if (head == "T") {
    result.kind = CommandKind::kAppendText;
    result.payload = rest;
  } else if (head == "C") {
    if (rest == "RESET") result.kind = CommandKind::kReset;
    else if (rest == "START") result.kind = CommandKind::kStart;
    else if (rest == "PAUSE") result.kind = CommandKind::kPause;
    else if (rest == "RESUME") result.kind = CommandKind::kResume;
    else if (rest == "STOP") result.kind = CommandKind::kStop;
    else if (rest == "STATUS") result.kind = CommandKind::kStatus;
  } else if (head == "PING") {
    result.kind = CommandKind::kPing;
  } else if (head == "S") {
    result.kind = CommandKind::kSet;
    result.payload = rest;
  }
  return result;
}

// Standard base64. Whitespace and newlines are stripped before decoding so a
// chunk split across BLE writes still works after reassembly. Returns empty on
// malformed input.
inline std::string base64Decode(const std::string& in) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int lut[256];
  for (int& x : lut) x = -1;
  for (int i = 0; i < 64; ++i) lut[static_cast<unsigned char>(kTable[i])] = i;

  std::string clean;
  clean.reserve(in.size());
  for (char c : in) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    clean.push_back(c);
  }
  const size_t n = clean.size();
  if (n % 4 != 0) return "";

  std::string out;
  out.reserve(n / 4 * 3);
  int buf = 0, bits = 0;
  for (size_t i = 0; i < n; ++i) {
    if (clean[i] == '=') break;
    const int v = lut[static_cast<unsigned char>(clean[i])];
    if (v < 0) return "";
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buf >> bits) & 0xFF));
      buf &= (1u << bits) - 1;  // drop consumed high bits
    }
  }
  return out;
}

// Split "key=value" into parts. Returns false when the token has no '=' or an
// empty key.
inline bool splitKeyValue(const std::string& kv, std::string& key,
                          std::string& value) {
  const size_t eq = kv.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  key = kv.substr(0, eq);
  value = kv.substr(eq + 1);
  return !key.empty() && !value.empty();
}

}  // namespace karute_ble
