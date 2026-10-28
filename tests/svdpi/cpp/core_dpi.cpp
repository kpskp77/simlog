// Category 2 — SVDPI backend, logging from the C++ side.
//
// This is the DPI-C half of the shared suite: it exports three functions to the
// testbench (tests/svdpi/cpp/tb_cpp.sv) and runs the same scenario bodies as the
// SystemC suite (tests/cases.cpp). The only thing the SV side does is advance
// time, because a DPI-C function cannot: the bench calls simlog_test_step(),
// applies #delay with the returned tick count and calls it again.
//
// The expected base comes from the runner, which knows the timescale it gave
// vcs (+scale= +unit=). That keeps the expected stamps independent of
// simlog::detail::resolution_unit(): if the backend reports the wrong precision,
// the shared format scenario fails instead of agreeing with itself.
//
// Built into libsimlog_svdpi_cpp.so next to libsimlog.so, which run.sh passes to
// vcs as well. The simlog backend symbols therefore stay undefined here and the
// simulator resolves them from libsimlog.so at load time — the same split a real
// project uses when its DPI-C library only contains the logging code.
#include <simlog/simlog.h>

#include <cstdint>
#include <string>

#include "cases.h"
#include "check.h"

namespace {

const test::cases::scenario* g_scenario = nullptr;
test::cases::context g_context{};
unsigned g_phase = 0;
bool g_finished = false;

}  // namespace

extern "C" {

// Picks the scenario and the time base the runner compiled this bench for.
// Returns 0 on success. A string arrives as a pointer that is only valid for the
// duration of the call, so the unit symbol is copied into g_context.
int simlog_test_begin(const char* name, std::uint64_t scale, const char* unit) {
  if (name == nullptr || unit == nullptr) return 1;
  g_scenario = test::cases::find(name);
  if (g_scenario == nullptr) return 1;
  g_context.base = {scale, unit};
  g_phase = 0;
  g_finished = false;
  test::cases::prepare(*g_scenario);
  return 0;
}

// Runs the checks of the current phase and returns the ticks the bench must
// advance before the next call. 0 means the scenario is finished.
std::uint64_t simlog_test_step() {
  if (g_scenario == nullptr || g_finished) return 0;
  const std::uint64_t ticks = g_scenario->step(g_context, g_phase);
  ++g_phase;
  if (ticks == 0) g_finished = true;
  return ticks;
}

// Check failures are reported on stderr by CHECK/CHECK_EQ; the bench turns a
// non-zero count into $fatal so simv's exit status carries the result.
int simlog_test_failures() {
  return test::failures;
}

}  // extern "C"
