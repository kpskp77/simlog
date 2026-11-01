// White-box declarations for the simlog test suites.
//
// include/simlog/simlog.h exposes exactly one public function, simlog::get();
// the suites additionally exercise internals — the time source (so a wrong
// backend answer fails instead of being copied into an expectation), the
// SIMLOG_LEVEL parser, the default sink, the %* formatter. Those functions are
// exported by libsimlog.so; this header is their only declaration outside
// src/simlog.cpp, and it deliberately lives in tests/, not include/simlog/ —
// it is not public API and must not ship.
//
// Keep the signatures in sync with src/simlog.cpp. unit_scale is defined in
// both places: it is a two-word POD returned by value, which is ABI-safe across
// the DSO boundary, and the format scenario re-checks its fields against the
// host-declared time base on every run.
#pragma once

#include <spdlog/common.h>
#include <spdlog/formatter.h>
#include <spdlog/sinks/sink.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace simlog::detail {

struct unit_scale {
  std::uint64_t scale;
  const char* symbol;
};

std::uint64_t now_ticks();
unit_scale resolution_unit();
std::optional<spdlog::level::level_enum> parse_level(std::string_view name);
bool wildcard_match(std::string_view pattern, std::string_view text);
spdlog::level::level_enum level_for(std::string_view name);
std::shared_ptr<spdlog::sinks::sink> default_sink();
std::unique_ptr<spdlog::formatter> make_sim_formatter();

}  // namespace simlog::detail
