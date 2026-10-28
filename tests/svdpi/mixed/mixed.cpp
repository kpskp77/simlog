// Category 4 — mixed mode: C++ and SystemVerilog log through the same logger.
//
// The key point is the build: this file becomes a DPI-C library that links
// libsimlog.so (the `simlog` target, which holds src/simlog.cpp), and run.sh
// loads only this library. One shared object is one library state, so
// simlog::get() here and simlog_open() (which src/simlog.cpp exports for
// sv/simlog_pkg.sv) hit the same spdlog registry. src/simlog.cpp is compiled
// once, into libsimlog.so; compiling it into this library too would duplicate
// the core.
//
// mixed_same_entity() is the direct assertion: the logger this C++ code gets is
// the very handle the SV proxy class opens for the same name. The benches in
// tb_mixed.sv additionally verify the observable side (interleaved lines, one
// stamp per timestep, SIMLOG_LEVEL applying to both sides).
#include <simlog/simlog.h>

#include <cstdint>
#include <string>

#include "check.h"

// The bridge function of src/simlog.cpp, resolved from libsimlog.so, which this
// library links. C linkage is name-only; sv/simlog_pkg.sv imports the same name
// and signature.
extern "C" void* simlog_open(const char* name);

extern "C" {

// True when the C++ logger and the handle the SV side would open are the same
// object. Recorded through CHECK as well, so a mismatch also shows file/line.
int mixed_same_entity(const char* name) {
  if (name == nullptr) return 0;
  const auto logger = simlog::get(name);
  void* via_bridge = simlog_open(name);
  CHECK_EQ(static_cast<const void*>(logger.get()), static_cast<const void*>(via_bridge));
  CHECK_EQ(logger->name(), std::string(name));
  return static_cast<void*>(logger.get()) == via_bridge ? 1 : 0;
}

// Log one line through the idiomatic C++ API. The SV side reaches the same
// logger through simlog_log(); the level numbers are spdlog's (simlog_level_e in
// sv/simlog_pkg.sv), so the two calls are interchangeable.
void mixed_cpp_log(const char* name, int level, const char* msg) {
  if (name == nullptr || msg == nullptr) return;
  const auto logger = simlog::get(name);
  logger->log(static_cast<spdlog::level::level_enum>(level), "{}", msg);
}

void mixed_cpp_flush(const char* name) {
  if (name == nullptr) return;
  simlog::get(name)->flush();
}

// The bench turns a non-zero count into $fatal.
int simlog_test_failures() {
  return test::failures;
}

}  // extern "C"
