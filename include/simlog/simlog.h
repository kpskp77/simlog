// simlog/simlog.h — spdlog extension for EDA simulation-time stamps
//
// The stamp is always "tick count × resolution multiplier" plus a plain unit
// symbol (fs/ps/ns/us/ms/s). Where the ticks come from is decided by exactly one
// compile-time backend:
//
//   SIMLOG_BACKEND_SVDPI    (default) the simulator's own time, read through the
//                           DPI time API (IEEE 1800-2023 svGetTime /
//                           svGetTimePrecision) or through the VPI/PLI fallback
//                           (vpi_get_time / tf_gettimeprecision) on older
//                           simulators. Compiled backend: its implementation is
//                           src/simlog.cpp → libsimlog.so, so this header never
//                           sees <svdpi.h> or a simulator declaration.
//   SIMLOG_BACKEND_SYSTEMC  SystemC: sc_time_stamp().value() ×
//                           sc_get_time_resolution(). Header-only, includes
//                           <systemc>, so SystemC's include path is needed.
//
// Define one of the two macros to pick the backend (xmake: --backend=...). The
// SVDPI default means DPI-C code needs no define at all; defining both is an
// #error. Nothing else depends on the choice: the backends answer the same two
// questions (now_ticks(), resolution_unit()) and everything else — the %* flag,
// the pattern, the levels, the default sink, simlog::get() — is shared.
// No initialization call of any kind is needed.
//
// The only public API is simlog::get(); everything else lives in simlog::detail.
// Single-threaded model (an EDA simulation runs on the main thread only): no
// locks, no atomics. Everything here is inline, so any translation unit may
// include the header; only the SVDPI implementation lives in src/simlog.cpp.
// Time is read inside the flag's format() call, never cached, so the stamp is
// always the time of the log call. Levels are resolved once, at logger creation.
//
// Requires C++17.
#pragma once

#if __cplusplus < 201703L
#error "simlog requires C++17 or later"
#endif

// ── Backend selection ────────────────────────────────────────────────
// The two backends are strictly alternative: one time source each, chosen when
// the code is compiled. SVDPI is the default, so the common case (a DPI-C
// library) needs no defines; the SystemC backend has to be asked for.
#if !defined(SIMLOG_BACKEND_SVDPI) && !defined(SIMLOG_BACKEND_SYSTEMC)
#define SIMLOG_BACKEND_SVDPI 1
#endif
#if defined(SIMLOG_BACKEND_SVDPI) && defined(SIMLOG_BACKEND_SYSTEMC)
#error "simlog: define at most one of SIMLOG_BACKEND_SVDPI and SIMLOG_BACKEND_SYSTEMC"
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
#include <utility>
#include <vector>

#if defined(SIMLOG_BACKEND_SYSTEMC)
#include <systemc>
#endif  // SIMLOG_BACKEND_SYSTEMC

namespace simlog::detail {

// ══════════════════════════════════════════════════════════════════════
// 1. Global configuration: log levels
//
// Levels have exactly one entry point, the environment — there is deliberately
// no set_level()-style API (that would be two sources of truth). SIMLOG_LEVEL
// carries both the global level and the per-module overrides; the form is
// detected automatically:
//
//   SIMLOG_LEVEL=info                 # a bare level name sets the global level
//   SIMLOG_LEVEL="*=debug,dma=info"   # module=level rules, ',' separated
//
// '*' matches any sequence of characters (including none), so a lone "*" is the
// global rule and "dma*" / "*_tb" narrow it. The most specific matching rule
// wins: an exact name beats any wildcard, then more literal characters, then the
// rule written later — so "*=debug,dma=info" logs "dma" at info and everything
// else at debug, in either order.
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
// fprintf directly: an invalid setting must not allocate its way into a logging
// recursion.
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
// verbatim, empty entries are skipped, invalid ones (bad shape, embedded
// whitespace, unknown level) are reported and skipped, so those modules fall back
// to the global level like unmatched modules do.
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

// Read once, lazily, on first use: the function-local static keeps the read out
// of static-initialization order.
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
// 2. Simulation-time source — exactly one, from the selected backend
//
// Both backends answer the same two questions and nothing outside this section
// knows which one is active:
//   now_ticks()        integer tick count of the current simulation time
//   resolution_unit()  {multiplier, base unit symbol} of one tick
//
// The multiplier is folded into the number and the unit keeps only the base
// symbol (100 fs resolution, 12340 ticks → "1234000 fs", never "12340 100fs"),
// so all arithmetic is integral and the stamp has no floating-point error.
// ══════════════════════════════════════════════════════════════════════

// Display form of one time resolution: how many base units one tick is, plus the
// base unit symbol. Shared by both backends (and the contract the %* flag sees).
struct unit_scale {
  std::uint64_t scale;  // 1 tick = scale × symbol
  const char* symbol;   // "fs" / "ps" / "ns" / "us" / "ms" / "s" etc.
};

#if defined(SIMLOG_BACKEND_SYSTEMC)

// ── Backend: SystemC ─────────────────────────────────────────────────

// Current simulation tick (1 tick = 1 time resolution). During elaboration
// sc_time_stamp() is 0, which is fine and is not an error.
[[nodiscard]] inline std::uint64_t now_ticks() {
  return static_cast<std::uint64_t>(sc_core::sc_time_stamp().value());
}

// sc_get_time_resolution().value() is always 1 (a resolution measured in
// itself), so the multiplier and the base unit are read in normalized form via
// sc_time_tuple:
//   1 fs → {1,"fs"}   100 fs → {100,"fs"}   10 ns → {10,"ns"}
// The symbol comes from SystemC itself (SystemC 3.0 may even go down to
// as/zs/ys) and is printed verbatim. SystemC only accepts resolutions of
// 1/10/100 × unit (otherwise E514), hence scale ≤ 100.
[[nodiscard]] inline unit_scale resolution_unit() {
  const sc_core::sc_time_tuple t(sc_core::sc_get_time_resolution());
  return {static_cast<std::uint64_t>(t.value()), t.unit_symbol()};
}

#else  // SIMLOG_BACKEND_SVDPI

// ── Backend: SVDPI (SystemVerilog DPI-C) ─────────────────────────────
//
// The implementation is src/simlog.cpp, built as libsimlog.so (the `simlog`
// target, SVDPI configuration only). That split keeps this header free of the
// simulator — no <svdpi.h>, no VPI/PLI declaration, no `<vpi_user.h>` /
// `<veriuser.h>` conflict in user code — at the price of not being header-only:
// whoever uses this backend links src/simlog.cpp, either as libsimlog.so or
// compiled into the DPI-C library that also holds the logging code. One shared
// object is one library state (registry, default sink, SIMLOG_LEVEL snapshot), so
// hold exactly one copy.
//
// The two functions below are the entire contract between this header (the %*
// flag, the pattern) and the backend (the clock); `extern "C"` because they are
// what the shared library exports. src/simlog.cpp asks the simulator through the
// DPI time API and falls back to the VPI/PLI functions, both as weak references; a
// host that answers neither keeps the stamp at 0 and says so once on stderr.

extern "C" {

// Current simulation tick, in units of the simulation precision (0 when no API
// reports a time, e.g. in a host-side program without a simulator).
[[nodiscard]] std::uint64_t simlog_sv_now_ticks();

// Display form of the current simulation precision: {multiplier, base symbol}.
[[nodiscard]] unit_scale simlog_sv_resolution_unit();

}  // extern "C"

// What the shared code (and the tests) see: one call per use, no caching — the
// stamp is the time of the log call, never a value read ahead of it.
[[nodiscard]] inline std::uint64_t now_ticks() {
  return simlog_sv_now_ticks();
}
[[nodiscard]] inline unit_scale resolution_unit() {
  return simlog_sv_resolution_unit();
}

#endif  // SIMLOG_BACKEND_SYSTEMC

// Tick count × multiplier — backend independent. Use 128-bit arithmetic when
// available: 64 bits overflows above ~1.8e17 ticks (scale=100), whereas 128 bits
// is exact for any uint64 tick count.
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
// Built on spdlog's extension point custom_flag_formatter. The flag character
// %* is non-alphabetic, so it cannot collide with a built-in flag.
//
// Output form (fixed width, easy to grep/diff): tick count × multiplier + base
// unit symbol, right-aligned in 12 columns:
//   [       12340 ps]   ← 1 ps resolution, i.e. 12.34 ns
//   [       12340 ns]   ← 1 ns resolution
//   [     1234000 fs]   ← 100 fs resolution (12340 tick × 100 fs = 1.234 ns)
//
// Known limitation: a spdlog async logger runs format() on a background thread,
// so the stamp would be the time of output rather than of the log call — only
// synchronous loggers are supported.
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
// set_level. The name is std::string (not string_view) because spdlog's registry
// only accepts std::string: a view would force an allocation even on the
// registered-logger fast path.
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
