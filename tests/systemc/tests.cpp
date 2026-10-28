// Category 1 — SystemC backend test suite.
//
// The binary takes a scenario name as its only argument; `xmake test` runs it
// once per scenario (see add_tests() in xmake.lua) so that every scenario gets a
// fresh process and therefore a fresh environment, time resolution and logger
// registry.
//
// The backend-agnostic scenarios live in tests/cases.cpp and are shared with
// the SVDPI C++ suite (tests/svdpi/cpp). This file is only the SystemC host: it
// advances sc_time between the shared phases and owns the two scenarios that
// exist because SystemC can change its time resolution at run time. SVDPI gets
// its resolution from the simulator's timescale instead, so those two scenarios
// have no SVDPI counterpart.
#include <simlog/simlog.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <string_view>

#include "cases.h"
#include "check.h"

// sc_main is called by the SystemC kernel, so it has to stay exported even if the
// program is ever built with hidden default visibility.
#if defined(_WIN32)
#define SIMLOG_TEST_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define SIMLOG_TEST_EXPORT __attribute__((visibility("default")))
#else
#define SIMLOG_TEST_EXPORT
#endif

namespace {

constexpr const char* kLogger = "tb";

// A resolution multiplier is folded into the tick count instead of the unit.
void resolution_100fs_scenario() {
  sc_core::sc_set_time_resolution(100, sc_core::SC_FS);
  const auto unit = simlog::detail::resolution_unit();
  CHECK_EQ(unit.scale, std::uint64_t{100});
  CHECK_EQ(std::string_view(unit.symbol), std::string_view("fs"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  log->info("start");
  sc_core::sc_start(1234000, sc_core::SC_FS);  // 12340 ticks, 100 fs each
  CHECK_EQ(sc_core::sc_time_stamp().value(), static_cast<sc_dt::uint64>(12340));
  log->info("later");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{2});
  if (lines.size() == 2) {
    CHECK_EQ(lines[0], std::string("[           0 fs][tb][info] start"));
    CHECK_EQ(lines[1], std::string("[     1234000 fs][tb][info] later"));
  }
}

void resolution_10ns_scenario() {
  sc_core::sc_set_time_resolution(10, sc_core::SC_NS);
  const auto unit = simlog::detail::resolution_unit();
  CHECK_EQ(unit.scale, std::uint64_t{10});
  CHECK_EQ(std::string_view(unit.symbol), std::string_view("ns"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  log->info("start");
  sc_core::sc_start(123400, sc_core::SC_NS);  // 12340 ticks, 10 ns each
  CHECK_EQ(sc_core::sc_time_stamp().value(), static_cast<sc_dt::uint64>(12340));
  log->info("later");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{2});
  if (lines.size() == 2) {
    CHECK_EQ(lines[0], std::string("[           0 ns][tb][info] start"));
    CHECK_EQ(lines[1], std::string("[      123400 ns][tb][info] later"));
  }
}

}  // namespace

SIMLOG_TEST_EXPORT int sc_main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argc > 0 ? argv[0] : "tests");
    return 2;
  }

  const std::string_view name(argv[1]);

  // Keep scenarios hermetic: only the env-* ones set SIMLOG_LEVEL themselves.
  if (name.substr(0, 4) != "env-") test::unset_env("SIMLOG_LEVEL");

  if (const test::cases::scenario* scn = test::cases::find(name)) {
    test::cases::prepare(*scn);
    // SystemC's default resolution is 1 ps; the shared scenarios expect exactly
    // that base (the two resolution scenarios above set their own).
    const test::cases::context ctx{{1, "ps"}};
    test::cases::run(*scn, ctx, [](std::uint64_t ticks) {
      sc_core::sc_start(sc_core::sc_time::from_value(static_cast<sc_dt::uint64>(ticks)));
    });
  } else if (name == "resolution-100fs") {
    resolution_100fs_scenario();
  } else if (name == "resolution-10ns") {
    resolution_10ns_scenario();
  } else {
    std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return 2;
  }

  if (test::failures != 0) {
    std::fprintf(stderr, "%d check(s) failed (scenario %s)\n", test::failures, argv[1]);
    return 1;
  }
  return 0;
}
