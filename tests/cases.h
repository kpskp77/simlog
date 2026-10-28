// Backend-agnostic test scenarios, shared by the SystemC and SVDPI suites.
//
// The scenario bodies below only use simlog's public contract (simlog::get(),
// the %* flag, SIMLOG_LEVEL, the spdlog registry). They never touch the SystemC
// kernel and never hand a timestamp to anything: time is advanced by the host,
// because only the host knows how:
//
//   SystemC  tests/systemc/tests.cpp advances sc_time between phases
//   SVDPI    tests/svdpi/cpp/core_dpi.cpp lets the SystemVerilog bench apply
//            #delay between phases (the DPI-C side cannot advance time itself)
//
// That split is what makes the two suites one suite: a scenario is a sequence of
// phases, step() runs the checks for one phase and returns how many simulation
// ticks the host must advance before the next one. 0 means "the scenario is
// done". The expected stamps are built from the time base the host declares, not
// from simlog::detail::resolution_unit(), so a wrong backend answer is caught
// instead of being copied into the expectation.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace test::cases {

// What one simulation tick means in this run: the host declares it (SystemC
// resolution, the simulator timescale) and the core builds the expected stamp
// from it: ticks * scale, right-aligned in 12 columns, then the base symbol.
struct time_base {
  std::uint64_t scale;
  std::string symbol;
};

struct context {
  time_base base;
};

struct scenario {
  const char* name;
  // Runs phase `phase` and returns the ticks to advance before the next phase.
  // 0 terminates the scenario.
  std::uint64_t (*step)(const context&, unsigned phase);
};

// The scenarios that run on both backends (format / stamp / env-* / registry /
// parse-level). Resolution scenarios that set a time base themselves stay in the
// SystemC suite: SVDPI gets its base from the simulator's timescale at compile
// time and cannot change it at run time.
const std::vector<scenario>& shared();

const scenario* find(std::string_view name);

// Keep scenarios hermetic: non-env-* scenarios run with SIMLOG_LEVEL unset; the
// env-* ones set the variable themselves. Must be called before the first
// simlog::get() in the process.
void prepare(const scenario& scn);

// Exact expected stamp field, e.g. "[       1234000 fs]".
std::string expect_stamp(std::uint64_t ticks, const time_base& base);

// A complete expected log line in the library's fixed pattern.
std::string expect_line(std::uint64_t ticks, const time_base& base, std::string_view logger,
                        std::string_view level, std::string_view payload);

// Runs a scenario to completion through `advance`, which advances the host clock
// by the given number of simulation ticks.
template <typename Advance>
void run(const scenario& scn, const context& ctx, Advance&& advance) {
  for (unsigned phase = 0;; ++phase) {
    const std::uint64_t ticks = scn.step(ctx, phase);
    if (ticks == 0) break;
    advance(ticks);
  }
}

}  // namespace test::cases
