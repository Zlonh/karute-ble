#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "ble_protocol.h"

using namespace karute_ble;

static void expectKind(const std::string& line, CommandKind expected) {
  Command c = parseLine(line);
  if (c.kind != expected) {
    std::cerr << "line [" << line << "] expected kind " << (int)expected
              << " got " << (int)c.kind << "\n";
    std::abort();
  }
}

int main() {
  // Command dispatch
  expectKind("PING", CommandKind::kPing);
  expectKind("C RESET", CommandKind::kReset);
  expectKind("C START", CommandKind::kStart);
  expectKind("C PAUSE", CommandKind::kPause);
  expectKind("C RESUME", CommandKind::kResume);
  expectKind("C STOP", CommandKind::kStop);
  expectKind("C STATUS", CommandKind::kStatus);
  expectKind("S mode=4 vucDownMs=5", CommandKind::kSet);
  expectKind("T SGVsbG8=", CommandKind::kAppendText);
  expectKind("BOGUS", CommandKind::kUnknown);
  expectKind("C FROB", CommandKind::kUnknown);

  // T payload keeps everything after the first space
  Command t = parseLine("T SGVsbG8=IQ==");
  assert(t.payload == "SGVsbG8=IQ==");

  // LineBuffer reassembles chunks split across BLE writes
  LineBuffer lb;
  std::string got;
  lb.feed("PI", 2);
  assert(!lb.pop(got));
  lb.feed("NG\nC STA", 8);
  assert(lb.pop(got));
  assert(got == "PING");
  assert(!lb.pop(got));
  lb.feed("RT\n", 3);
  assert(lb.pop(got));
  assert(got == "C START");

  // CRLF line endings also split cleanly
  LineBuffer lb2;
  lb2.feed("C RESET\r\n", 9);
  assert(lb2.pop(got));
  assert(got == "C RESET");

  // Base64 decode
  assert(base64Decode("SGVsbG8=") == "Hello");
  assert(base64Decode("5Lit5paH") == std::string("\xe4\xb8\xad\xe6\x96\x87", 6));  // 中文
  assert(base64Decode("!!!") == "");
  assert(base64Decode("") == "");

  // splitKeyValue
  std::string k, v;
  assert(splitKeyValue("mode=4", k, v));
  assert(k == "mode" && v == "4");
  assert(!splitKeyValue("novalue", k, v));
  assert(!splitKeyValue("=x", k, v));

  std::cout << "ble_protocol tests passed\n";
  return 0;
}
