// simlog/simlog.h — spdlog extension for EDA simulation-time stamps (single header)
//
// There is exactly one time source: SystemC. The stamp value is
//   sc_time_stamp().value() × resolution multiplier
// and the unit is always the resolution's base symbol (fs/ps/ns/us/ms/s).
// No initialization call of any kind is needed.
//
// Requires C++17. Note: this header includes <systemc> unconditionally, so
// SystemC's include directory must be on the include path wherever simlog is
// used.
//
// The only public API is simlog::get(). Everything else (time flag / formatter /
// environment parsing / default sink) lives in simlog::detail.
//
// Design notes (single-threaded model — an EDA simulation runs on the main
// thread only, so there are no locks and no atomics):
//   - Everything is header-only inline variables/functions, so any translation
//     unit can include it without ODR problems
//   - Time is read inside the flag's format() call, so there is no "call
//     init() first" ordering constraint and the stamp is always the time of the
//     log call
//   - Levels are resolved once per logger, at creation; time-source changes
//     (e.g. sc_set_time_resolution()) apply to every existing logger at once
#pragma once

#if __cplusplus < 201703L
#error "simlog requires C++17 or later"
#endif

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <systemc>
#include <utility>
#include <vector>

namespace simlog::detail {

// ══════════════════════════════════════════════════════════════════════
// 1. Global configuration: log levels
//
// Levels are pure run-time knobs with a single entry point: the environment.
// There is deliberately no set_level()-style API, to avoid two sources of
// truth ("half configured through the API, half through the environment").
//
// SIMLOG_LEVEL carries both the global level and the per-module overrides; the
// form is detected automatically:
//
//   SIMLOG_LEVEL=info                 # a bare level name sets the global level
//   SIMLOG_LEVEL="*=debug,dma=info"   # module=level rules, ',' separated
//
// '*' matches any sequence of characters (including none), so a lone "*" is the
// global rule and a partial wildcard such as "dma*" or "*_tb" narrows it. The
// most specific matching rule wins: an exact name beats any wildcard, among
// wildcards the one with more literal characters wins, and ties go to the rule
// written later. Hence "*=debug,dma=info" logs "dma" at info and every other
// module at debug, no matter in which order the two rules appear.
//
// The variable is read once, on the first simlog::get().
// ══════════════════════════════════════════════════════════════════════

// One module rule: a logger-name pattern ('*' = any sequence) and its level.
// Rules are kept in the order they were written.
struct module_level {
  std::string pattern;
  spdlog::level::level_enum level;
};

struct env_config {
  spdlog::level::level_enum level = spdlog::level::info;  // used when no rule matches
  std::vector<module_level> modules;                      // module=level rules, incl. "*"
};

// Rejected settings are reported on stderr instead of being applied silently.
// Writes to stderr directly: an invalid setting must not throw or allocate its
// way into a logging recursion.
inline void warn_bad_entry(std::string_view entry) {
  std::fprintf(stderr,
               "[simlog] ignoring SIMLOG_LEVEL entry \"%.*s\" "
               "(expected a bare level or module=level without whitespace; "
               "levels: trace/debug/info/warn/err/critical/off)\n",
               static_cast<int>(entry.size()), entry.data());
}

// Map a level name to spdlog's enum; std::nullopt means "unknown name".
// spdlog::level::from_str() is deliberately not used: it maps unknown strings to
// off, which would silently disable logging.
[[nodiscard]] inline std::optional<spdlog::level::level_enum> parse_level(
    std::string_view name) noexcept {
  constexpr std::array k_levels{
      std::pair{std::string_view{"trace"}, spdlog::level::trace},
      std::pair{std::string_view{"debug"}, spdlog::level::debug},
      std::pair{std::string_view{"info"}, spdlog::level::info},
      std::pair{std::string_view{"warn"}, spdlog::level::warn},
      std::pair{std::string_view{"err"}, spdlog::level::err},
      std::pair{std::string_view{"critical"}, spdlog::level::critical},
      std::pair{std::string_view{"off"}, spdlog::level::off},
  };
  for (const auto& [level_name, level] : k_levels)
    if (level_name == name) return level;
  return std::nullopt;
}

[[nodiscard]] constexpr bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// SIMLOG_LEVEL is comma-separated and taken verbatim: whitespace is not a
// separator and is not skipped, so an entry containing it is rejected instead of
// silently becoming a pattern that can never match.
[[nodiscard]] inline bool has_space(std::string_view text) noexcept {
  for (const char c : text) {
    if (is_space(c)) return true;
  }
  return false;
}

// Glob match supporting only '*', which matches any sequence of characters
// (including none). Everything else matches literally and case-sensitively.
[[nodiscard]] inline bool wildcard_match(std::string_view pattern, std::string_view text) noexcept {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;  // position of the last '*' seen
  std::size_t resume = 0;                     // text offset to resume from
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      resume = t;
    } else if (p < pattern.size() && pattern[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++resume;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

// Specificity of a pattern: how many characters have to match literally.
[[nodiscard]] constexpr std::size_t literal_length(std::string_view pattern) noexcept {
  std::size_t literals = 0;
  for (const char c : pattern) {
    if (c != '*') ++literals;
  }
  return literals;
}

// "module=level,module=level,..." — only ',' separates; entries are used
// verbatim, empty entries are skipped and invalid entries (bad shape, embedded
// whitespace, unknown level) are reported and skipped, so those modules fall back
// to the global level just like unmatched modules do.
inline void parse_modules(std::string_view list, std::vector<module_level>& modules) {
  while (!list.empty()) {
    const auto comma = list.find(',');
    const std::string_view entry = list.substr(0, comma);
    if (!entry.empty()) {
      const auto equal = entry.find('=');
      const std::string_view pattern = entry.substr(0, equal);
      const auto level =
          equal == std::string_view::npos ? std::nullopt : parse_level(entry.substr(equal + 1));
      if (pattern.empty() || has_space(entry) || !level)
        warn_bad_entry(entry);
      else
        modules.push_back({std::string(pattern), *level});
    }
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
}

// A bare level name sets the global level; anything else is read as module rules.
inline void parse_config(std::string_view value, env_config& cfg) {
  if (const auto level = parse_level(value)) {
    cfg.level = *level;
    return;
  }
  // Not a bare level: a comma-separated rule list. A value that is neither ends
  // up here and is reported as one invalid entry, keeping the default level.
  parse_modules(value, cfg.modules);
}

// Read once, lazily, on first use. The function-local static keeps the read out
// of static-initialization order and avoids touching the environment before a
// simulation exists.
[[nodiscard]] inline const env_config& env() {
  static const env_config cfg = [] {
    env_config c;
    if (const char* raw = std::getenv("SIMLOG_LEVEL")) parse_config(raw, c);
    return c;
  }();
  return cfg;
}

// Level for one logger name: the most specific matching rule, else the global
// level (an exact name beats any wildcard, then more literal characters win,
// then the rule written later).
[[nodiscard]] inline spdlog::level::level_enum level_for(std::string_view name) {
  const env_config& cfg = env();
  const module_level* chosen = nullptr;
  bool chosen_exact = false;
  std::size_t chosen_literals = 0;
  for (const auto& rule : cfg.modules) {
    if (!wildcard_match(rule.pattern, name)) continue;
    const bool exact = rule.pattern.find('*') == std::string::npos;
    const std::size_t literals = literal_length(rule.pattern);
    const bool better =
        chosen == nullptr || (exact != chosen_exact ? exact : literals >= chosen_literals);
    if (better) {
      chosen = &rule;
      chosen_exact = exact;
      chosen_literals = literals;
    }
  }
  return chosen != nullptr ? chosen->level : cfg.level;
}

// ══════════════════════════════════════════════════════════════════════
// 2. SystemC time source (the only stamp source: no injection, no manual
//    time advancement, no unit configuration)
//
// A resolution may carry a multiplier (e.g. 100 fs = 100 fs per tick). The
// multiplier is folded into the number and the unit keeps only the base
// symbol: at 100 fs resolution, 12340 ticks print as "1234000 fs" rather than
// "12340 100fs" — the unit column always holds a plain symbol such as
// fs/ps/ns/us/ms/s. As a result sc_set_time_resolution() takes effect for this
// library immediately, and all arithmetic stays integral with no floating-point
// error.
// ══════════════════════════════════════════════════════════════════════

// Current simulation tick (1 tick = 1 time resolution). During elaboration
// sc_time_stamp() is 0, which is fine.
[[nodiscard]] inline std::uint64_t now_ticks() {
  return static_cast<std::uint64_t>(sc_core::sc_time_stamp().value());
}

// Display form of the resolution: how many base units one tick is, plus the
// base unit symbol. The symbol comes from SystemC itself (SystemC 3.0 may even
// go down to as/zs/ys) and is printed verbatim.
struct unit_scale {
  std::uint64_t scale;  // 1 tick = scale × symbol
  const char* symbol;   // "fs" / "ps" / "ns" / "us" / "ms" / "s" etc.
};

// sc_get_time_resolution().value() is always 1 (a resolution measured in
// itself), so the multiplier and the base unit are read in normalized form via
// sc_time_tuple:
//   1 fs → {1,"fs"}   100 fs → {100,"fs"}   10 ns → {10,"ns"}
// SystemC only accepts resolutions of 1/10/100 × unit (otherwise E514), hence
// scale ≤ 100.
[[nodiscard]] inline unit_scale resolution_unit() {
  const sc_core::sc_time_tuple t(sc_core::sc_get_time_resolution());
  return {static_cast<std::uint64_t>(t.value()), t.unit_symbol()};
}

// Tick count × multiplier. Use 128-bit arithmetic when available: 64 bits
// overflows above ~1.8e17 ticks (scale=100), whereas 128 bits is exact for any
// uint64 tick count.
#if defined(__SIZEOF_INT128__)
using wide_ticks = unsigned __int128;
#else
using wide_ticks = std::uint64_t;  // no 128-bit type: only extreme tick counts overflow
#endif

[[nodiscard]] inline wide_ticks scaled_ticks(std::uint64_t scale) noexcept {
  return static_cast<wide_ticks>(now_ticks()) * scale;
}

// ══════════════════════════════════════════════════════════════════════
// 3. Simulation-time pattern flag (%*)
//
// Built on spdlog's official extension point custom_flag_formatter (three
// requirements: format() / clone() / a sensible flag character). The flag
// character %* is a non-alphabetic character, so it cannot silently collide
// with a built-in flag.
//
// Output form (fixed width, easy to grep/diff): tick count × multiplier +
// base unit symbol, right-aligned in 12 columns:
//   [       12340 ps]   ← 1 ps resolution, i.e. 12.34 ns
//   [       12340 ns]   ← 1 ns resolution
//   [     1234000 fs]   ← 100 fs resolution (12340 tick × 100 fs = 1.234 ns)
//
// Known limitation (phase 1): a spdlog async logger runs format() on a
// background thread, so the stamp is the time of output rather than the time of
// the log call and would be inaccurate — only synchronous loggers are
// supported.
// ══════════════════════════════════════════════════════════════════════

class sim_time_flag final : public spdlog::custom_flag_formatter {
public:
  void format(const spdlog::details::log_msg&, const std::tm&,
              spdlog::memory_buf_t& dest) override {
    const unit_scale u = resolution_unit();
    // Fold the multiplier into the number; keep only the base symbol as unit.
    // fmt_lib is spdlog's fmt/std::format alias, so this also works with
    // SPDLOG_USE_STD_FORMAT.
    spdlog::fmt_lib::format_to(std::back_inserter(dest), "{:>12} {}", scaled_ticks(u.scale),
                               u.symbol);
  }

  std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<sim_time_flag>(*this);
  }
};

// Build a formatter carrying the simulation-time flag. %* in the pattern is the
// simulation time.
[[nodiscard]] inline std::unique_ptr<spdlog::formatter> make_sim_formatter() {
  auto pf = std::make_unique<spdlog::pattern_formatter>();
  pf->add_flag<sim_time_flag>('*');  // %* = simulation time
  pf->set_pattern("[%*][%n][%^%l%$] %v");
  return pf;
}

// Default sink: a process-wide shared colored stdout sink (single-threaded
// variant, matching the single-threaded simulation model). If you need a file
// sink, just push_back() onto logger->sinks() after obtaining the logger.
[[nodiscard]] inline std::shared_ptr<spdlog::sinks::stdout_color_sink_st> default_sink() {
  static const auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_st>();
  return sink;
}

}  // namespace simlog::detail

namespace simlog {

// ══════════════════════════════════════════════════════════════════════
// 4. Main user entry point: simlog::get() (the library's only public API)
//
// The single convention: obtain loggers through simlog::get(name).
//   - First call: create a native spdlog::logger → attach the simulation-time
//                 formatter (default pattern) → apply the level for this module
//                 (most specific SIMLOG_LEVEL rule, else the global level) →
//                 register with spdlog → return it
//   - Later calls / an already registered logger of the same name: returned
//                 directly by spdlog::get(), untouched
//
// The result is a native spdlog::logger, so callers are free to add_sink /
// set_level. Single-threaded model: the registration path takes no lock and the
// default sink is the single-threaded variant (_st).
//
// The name is taken as std::string (not std::string_view) because spdlog's
// registry only accepts std::string: a view would force an allocation on every
// call, including the registered-logger fast path.
// ══════════════════════════════════════════════════════════════════════

[[nodiscard]] inline std::shared_ptr<spdlog::logger> get(const std::string& name) {
  if (auto existing = spdlog::get(name))
    return existing;  // already registered (including user-created ones): return as-is

  auto logger = std::make_shared<spdlog::logger>(name, detail::default_sink());
  logger->set_formatter(detail::make_sim_formatter());
  logger->set_level(detail::level_for(name));
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace simlog
