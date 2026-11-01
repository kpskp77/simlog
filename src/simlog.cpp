// src/simlog.cpp — all of simlog in one translation unit, for both backends.
//
// The public header (include/simlog/simlog.h) declares the library's single
// entry point, simlog::get(); everything that implements the stamps lives here
// and is compiled exactly once into libsimlog.so (the `simlog` target, either
// backend), in four numbered sections:
//
//   1. Global configuration      SIMLOG_LEVEL parsing and level_for(); the
//                                environment is the only run-time level knob
//   2. Simulation-time source    exactly one backend, from the SIMLOG_BACKEND_*
//                                macro defined on the library target:
//                                  SystemC  sc_time_stamp() ×
//                                           sc_get_time_resolution()
//                                  SVDPI    the simulator's own time (DPI time
//                                           API with a VPI/PLI fallback) plus
//                                           the writer behind the default sink
//   3. The %* pattern flag,      sim_time_flag, the fixed pattern and the
//      the pattern, the sink      default sink: the simulator's own output on
//                                SVDPI (vpi_printf), colored stdout on SystemC —
//                                where a line has to go is a property of the host
//   4. simlog::get()             create/configure/register on first call, return
//                                the already-registered logger untouched later
//
// The SVDPI backend additionally exports the SystemVerilog-facing C ABI
// (simlog_open / simlog_log / simlog_flush) that sv/simlog_pkg.sv wraps, and the
// two backend-contract functions (simlog_sv_now_ticks / simlog_sv_resolution_unit
// / simlog_sv_write) that sections 2 and 3 build on; see the backend block at
// the bottom of this file.
//
// Why one translation unit in one shared object, for both backends:
//   - One shared object is one library state: the default sink and the
//     SIMLOG_LEVEL snapshot live here, and the spdlog registry lives in the
//     shared libspdlog.so (one per process), so every consumer of libsimlog.so —
//     a SystemC testbench executable, a DPI-C library, a simulator loading the
//     .so — meets at one registry and one sink.
//   - The header stays free of <systemc> and <svdpi.h>: no simulator or SystemC
//     include path for consumers, and no include-order trap next to
//     <vpi_user.h> / <veriuser.h>.
//   - The SVDPI backend's simulator symbols (svGetTime, vpi_get_time,
//     vpi_printf, …) are weak references in this one library instead of in every
//     consumer translation unit.
//
// Single-threaded model: no locks, no atomics. Time is read inside the flag's
// format() call, never cached, so the stamp is always the time of the log call.
//
// Requires C++17.

// ── Backend selection ────────────────────────────────────────────────
// The two backends are strictly alternative: one time source each, compiled
// into the library. SVDPI is the default, so the common case needs no define;
// the SystemC backend has to be asked for. xmake defines exactly one of the two
// on the `simlog` target (--backend=...); when this file is compiled by hand,
// the same default applies.
#if !defined(SIMLOG_BACKEND_SVDPI) && !defined(SIMLOG_BACKEND_SYSTEMC)
#define SIMLOG_BACKEND_SVDPI 1
#endif
#if defined(SIMLOG_BACKEND_SVDPI) && defined(SIMLOG_BACKEND_SYSTEMC)
#error "simlog: define at most one of SIMLOG_BACKEND_SVDPI and SIMLOG_BACKEND_SYSTEMC"
#endif

#include <simlog/simlog.h>

// spdlog machinery behind the %* flag (section 3) and both default-sink
// variants, plus the formatter/sink types the white-box surface returns.
#include <spdlog/details/null_mutex.h>
#include <spdlog/formatter.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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

#else  // SIMLOG_BACKEND_SVDPI

// The simulator's own DPI header: only this translation unit needs it, and only
// here may svScope / svTimeVal / sv_sim_time appear.
#include <svdpi.h>

// The weak-symbol machinery is GCC/Clang only: other compilers get strong
// references and no fallback (a successful link then means the API is there).
#if defined(_WIN32)
#define SIMLOG_WEAK_SYMBOL
#define SIMLOG_HAS_WEAK_SYMBOLS 0
#elif defined(__GNUC__) || defined(__clang__)
#define SIMLOG_WEAK_SYMBOL __attribute__((weak))
#define SIMLOG_HAS_WEAK_SYMBOLS 1
#else
#define SIMLOG_WEAK_SYMBOL
#define SIMLOG_HAS_WEAK_SYMBOLS 0
#endif

#if SIMLOG_HAS_WEAK_SYMBOLS
// <svdpi.h> has already declared the first two; the weak attribute on the
// redeclaration is what turns the hard link-time dependency into a run-time
// check. The VPI/PLI ones are declared here instead of including
// <vpi_user.h>/<veriuser.h>: this file must compile without any simulator
// include path beyond the header-only `svdpi` package, and only the handful of
// scalar signatures involved are needed (svTimeVal *is* VPI's s_vpi_time,
// PLI_INT32 is int, and vpi_printf's PLI_INT32 return is int).
extern "C" {
SIMLOG_WEAK_SYMBOL int svGetTime(const svScope scope, svTimeVal* time);
SIMLOG_WEAK_SYMBOL int svGetTimePrecision(const svScope scope, std::int32_t* precision);
SIMLOG_WEAK_SYMBOL void vpi_get_time(void* object, svTimeVal* time);
SIMLOG_WEAK_SYMBOL std::int32_t tf_gettimeprecision(void);
SIMLOG_WEAK_SYMBOL int vpi_printf(const char* format, ...);
}  // extern "C"
#endif  // SIMLOG_HAS_WEAK_SYMBOLS

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
void warn_bad_entry(std::string_view entry) {
  std::fprintf(stderr,
               "[simlog] ignoring SIMLOG_LEVEL entry \"%.*s\" "
               "(expected a bare level or module=level without whitespace; "
               "levels: trace/debug/info/warn/err/critical/off)\n",
               static_cast<int>(entry.size()), entry.data());
}

// Map a level name to spdlog's enum; std::nullopt means "unknown name".
// spdlog::level::from_str() is deliberately not used: it maps unknown strings to
// off, which would silently disable logging.
//
// White-box surface: exported for the test suites (declared in
// tests/simlog_detail.h), not public API — simlog.h exposes only get().
[[nodiscard]] SIMLOG_API std::optional<spdlog::level::level_enum> parse_level(
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
[[nodiscard]] bool has_space(std::string_view text) noexcept {
  for (const char c : text) {
    if (is_space(c)) return true;
  }
  return false;
}

// Glob match supporting only '*', which matches any sequence of characters
// (including none). Everything else matches literally and case-sensitively.
// (White-box surface, see parse_level.)
[[nodiscard]] SIMLOG_API bool wildcard_match(std::string_view pattern,
                                             std::string_view text) noexcept {
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
void parse_modules(std::string_view list, std::vector<module_level>& modules) {
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
void parse_config(std::string_view value, env_config& cfg) {
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
[[nodiscard]] const env_config& env() {
  static const env_config cfg = [] {
    env_config c;
    if (const char* raw = std::getenv("SIMLOG_LEVEL")) parse_config(raw, c);
    return c;
  }();
  return cfg;
}

// Level for one logger name: the most specific matching rule, else the global
// level (an exact name beats any wildcard, then more literal characters win,
// then the rule written later). (White-box surface, see parse_level.)
[[nodiscard]] SIMLOG_API spdlog::level::level_enum level_for(std::string_view name) {
  const env_config& cfg = env();
  const module_level* chosen = nullptr;
  bool chosen_exact = false;
  std::size_t chosen_literals = 0;
  for (const auto& rule : cfg.modules) {
    if (!wildcard_match(rule.pattern, name)) continue;
    const bool exact = rule.pattern.find('*') == std::string_view::npos;
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
//
// White-box surface: exported for the test suites (declared in
// tests/simlog_detail.h), not public API — simlog.h exposes only get().
[[nodiscard]] SIMLOG_API std::uint64_t now_ticks() {
  return static_cast<std::uint64_t>(sc_core::sc_time_stamp().value());
}

// sc_get_time_resolution().value() is always 1 (a resolution measured in
// itself), so the multiplier and the base unit are read in normalized form via
// sc_time_tuple:
//   1 fs → {1,"fs"}   100 fs → {100,"fs"}   10 ns → {10,"ns"}
// The symbol comes from SystemC itself (SystemC 3.0 may even go down to
// as/zs/ys) and is printed verbatim. SystemC only accepts resolutions of
// 1/10/100 × unit (otherwise E514), hence scale ≤ 100. (White-box surface, see
// now_ticks.)
[[nodiscard]] SIMLOG_API unit_scale resolution_unit() {
  const sc_core::sc_time_tuple t(sc_core::sc_get_time_resolution());
  return {static_cast<std::uint64_t>(t.value()), t.unit_symbol()};
}

#else  // SIMLOG_BACKEND_SVDPI

// ── Backend: SVDPI (SystemVerilog DPI-C) ─────────────────────────────
//
// The clock and the simulator's output live in this file, at the bottom: the
// time functions ask the simulator through the DPI time API (IEEE 1800-2023)
// and fall back to the VPI/PLI functions, all as weak references; a host that
// answers neither keeps the stamp at 0 and says so once on stderr, and a host
// without vpi_printf() gets the sink's lines on stdout instead. The three
// functions below are the entire contract between the shared code (this
// section, the %* flag, the default sink) and the backend — `extern "C"`
// because they are what the shared library exports.

extern "C" {

// Current simulation tick, in units of the simulation precision (0 when no API
// reports a time, e.g. in a host-side program without a simulator).
SIMLOG_API std::uint64_t simlog_sv_now_ticks();

// Display form of the current simulation precision: {multiplier, base symbol}.
SIMLOG_API unit_scale simlog_sv_resolution_unit();

// Hand one finished log line to the simulator's own output (the default sink of
// this backend, section 3). A DPI-C library's stdout is not part of the
// simulator's output stream, so `simv -l sim.log` would miss every line; the
// implementation prefers the standard VPI routine vpi_printf() and writes to
// stdout itself only when the host has no VPI (a plain host-side program). The
// text is not NUL-terminated: `size` is authoritative.
SIMLOG_API void simlog_sv_write(const char* text, std::size_t size);

}  // extern "C"

// What the shared code (and the tests) see: one call per use, no caching — the
// stamp is the time of the log call, never a value read ahead of it. (White-box
// surface, see the SystemC now_ticks.)
[[nodiscard]] SIMLOG_API std::uint64_t now_ticks() {
  return simlog_sv_now_ticks();
}
[[nodiscard]] SIMLOG_API unit_scale resolution_unit() {
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

[[nodiscard]] wide_ticks scaled_ticks(std::uint64_t scale) noexcept {
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
// simulation time. (White-box surface, see parse_level.)
[[nodiscard]] SIMLOG_API std::unique_ptr<spdlog::formatter> make_sim_formatter() {
  auto pf = std::make_unique<spdlog::pattern_formatter>();
  pf->add_flag<sim_time_flag>('*');  // %* = simulation time
  pf->set_pattern("[%*][%n][%^%l%$] %v");
  return pf;
}

#if defined(SIMLOG_BACKEND_SVDPI)
// The SVDPI default sink: format the line here and let the simulator print it.
//
// The reason is the log file, not cosmetics: a DPI-C library is an ordinary
// shared object, so its stdout is the C runtime's stream and `simv -l sim.log`
// does not see it. vpi_printf() is the simulator's own output routine, which is
// exactly what -l duplicates into the file (and what $display goes through), so
// the C++ side of a real testbench finally lands in the simulation log, in
// order, next to the SystemVerilog lines.
//
// It is a plain base_sink on purpose: the color codes of a console sink are
// noise in a log file, and the only writer that reaches the simulator's stream
// is the backend's. base_sink already runs the logger's formatter (set by
// get()) and ignores the color range markers of the fixed pattern, so the line
// is formatted once, exactly as for the SystemC backend.
class simulator_sink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
  simulator_sink() = default;

protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    simlog_sv_write(formatted.data(), formatted.size());
  }

  // Nothing is buffered on this side: vpi_printf() hands each line over
  // immediately, and the stdout fallback flushes inside the writer. A no-op here
  // is the honest implementation, not a missing one.
  void flush_() override {}
};
#endif  // SIMLOG_BACKEND_SVDPI

// Default sink: one process-wide shared instance (single-threaded variant,
// matching the single-threaded simulation model).
//   SVDPI:   the simulator's output stream, through vpi_printf() (see above), so
//            `simv -l sim.log` contains the lines; stdout when there is no VPI.
//   SystemC: colored stdout.
// If you need a file sink, just push_back() onto logger->sinks() after obtaining
// the logger. (White-box surface, see parse_level.)
[[nodiscard]] SIMLOG_API std::shared_ptr<spdlog::sinks::sink> default_sink() {
#if defined(SIMLOG_BACKEND_SVDPI)
  static const auto sink = std::make_shared<simulator_sink>();
#else
  static const auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_st>();
#endif
  return sink;
}

}  // namespace simlog::detail

namespace simlog {

// ══════════════════════════════════════════════════════════════════════
// 4. Main user entry point: simlog::get() (the library's only public API;
//    declared in simlog.h)
//
// The single convention: obtain loggers through simlog::get(name).
//   - First call: create a native spdlog::logger → attach the simulation-time
//                 formatter (default pattern) → apply the level for this module
//                 (most specific SIMLOG_LEVEL rule, else the global level) →
//                 register with spdlog → return it
//   - Later calls / an already registered logger of the same name: returned
//                 directly by spdlog::get(), untouched
// ══════════════════════════════════════════════════════════════════════

SIMLOG_API std::shared_ptr<spdlog::logger> get(const std::string& name) {
  if (auto existing = spdlog::get(name))
    return existing;  // already registered (including user-created ones): return as-is

  auto logger = std::make_shared<spdlog::logger>(name, detail::default_sink());
  logger->set_formatter(detail::make_sim_formatter());
  logger->set_level(detail::level_for(name));
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace simlog

#if defined(SIMLOG_BACKEND_SVDPI)

// ══════════════════════════════════════════════════════════════════════
// The SVDPI backend: the clock, the default sink's writer and the
// SystemVerilog-facing C ABI
//
// ── 1. The time source ───────────────────────────────────────────────
// The simulator is asked for the global simulation time (null scope = "the
// whole simulation"), in two ways, tried in this order:
//
//   1. the DPI time API (IEEE 1800-2023):
//        svGetTime(NULL, &t)            → 64-bit tick count, t.type = sv_sim_time
//        svGetTimePrecision(NULL, &exp) → precision exponent, e.g. -12 for 1 ps
//   2. the VPI/PLI time functions, which simulators have provided far longer:
//        vpi_get_time(NULL, &t)         → the same tick count, t.type = vpiSimTime
//        tf_gettimeprecision()          → the same exponent
//
// Path 1 alone is not enough: svGetTime/svGetTimePrecision only entered the DPI
// annex with IEEE 1800-2023, so pre-2023 simulators (VCS O-2018.09 neither
// exports nor declares them) fail to link against a plain reference. Both are
// therefore declared weak: a 2023-capable simulator binds its own definition at
// load time, an older one leaves the pointers null and path 2 answers. With
// neither (a host-side program with no simulator) the stamp stays 0 and one
// stderr line says so.
//
// The two paths are never mixed within a process, and the value is read per
// format() call, so the stamp is the time of the log call, never a cached value.
// Both ask for the integer tick count, never a scaled real (a double would
// reintroduce the rounding the tick arithmetic avoids). No VPI handle is looked
// up and no VPI callback is registered: vpi_get_time(NULL, …) is a value query.
//
// ── 2. The SystemVerilog-facing C ABI ────────────────────────────────
//   void* simlog_open(const char* name);                            logger handle
//   void  simlog_log(void* handle, int level, const char* msg);     log one line
//   void  simlog_flush(void* handle);                               flush that logger
//
// The handle *is* the logger simlog::get() returned: that call already creates,
// configures and registers it, and the registry keeps it alive for the rest of
// the process, so the pointer stays valid (never spdlog::drop() it), two opens
// of one name return the same pointer, and an already registered logger is
// returned untouched. A handle instead of a name per call because the SV side
// opens once and then passes one pointer per line, whereas a name would
// marshall a string and look the logger up in the registry on every line.
//
// The bridge adds no semantics of its own: levels (SIMLOG_LEVEL), the pattern
// and the time source stay in this file's sections 1-3. In particular the SV
// side never passes a timestamp in — the stamp is read inside the flag's
// format() call, so it is the time of the log call. Passing $time or
// $realtime would add a second time source and a rounding step, which is the
// one thing this ABI deliberately does not offer.
//
// A bad argument is reported once per process on stderr and the line is
// dropped, never silently applied — the same rule SIMLOG_LEVEL and the time
// source follow.
//
// ── Export list ──────────────────────────────────────────────────────
// The six `extern "C"` functions below are the ABI: they are marked SIMLOG_API
// (default visibility) so `nm -D libsimlog.so` lists them by name, alongside
// the C++ white-box symbols the test suites use.
//
// There is deliberately no header for the bridge: nothing outside this file
// needs those declarations. C++ testbench code includes simlog.h; a
// SystemVerilog testbench loads the library; a DPI-C library that wants the
// bridge declares the names itself (tests/svdpi/mixed/mixed.cpp does). The
// other side of the contract is sv/simlog_pkg.sv, which imports the three
// names — C linkage is name-only, so the two have to be kept in step by hand:
// a renamed or re-signatured function still links and corrupts the arguments.
// ══════════════════════════════════════════════════════════════════════

namespace {

using simlog::detail::unit_scale;

// Reported once per process: a host that answers neither time API would
// otherwise leave every stamp at 0 without a trace (a silent wrong value is
// worse than a line on stderr).
void warn_no_sim_time_once() {
  static const bool once = [] {
    std::fprintf(stderr,
                 "[simlog] no simulation time: neither the DPI time API nor the VPI time "
                 "functions answered; stamps stay 0 (is this code running inside a "
                 "simulator?)\n");
    return true;
  }();
  (void)once;
}

// Whether the IEEE 1800-2023 DPI time API is present in this process. Without
// weak symbols the reference is hard, so a successful link means it is there.
[[nodiscard]] bool dpi_time_api_available() noexcept {
#if SIMLOG_HAS_WEAK_SYMBOLS
  return svGetTime != nullptr && svGetTimePrecision != nullptr;
#else
  return true;
#endif
}

// The VPI/PLI compatibility path: reads the current time, returns false when the
// host does not provide the function (no simulator at all).
[[nodiscard]] bool read_vpi_time(svTimeVal& t) noexcept {
#if SIMLOG_HAS_WEAK_SYMBOLS
  if (vpi_get_time != nullptr) {
    vpi_get_time(nullptr, &t);  // value query on the whole simulation
    return true;
  }
#else
  (void)t;
#endif
  return false;
}

[[nodiscard]] bool read_vpi_precision(std::int32_t& exponent) noexcept {
#if SIMLOG_HAS_WEAK_SYMBOLS
  if (tf_gettimeprecision != nullptr) {
    exponent = tf_gettimeprecision();
    return true;
  }
#else
  (void)exponent;
#endif
  return false;
}

// The 64-bit tick count arrives split into high/low; the high word has to be
// shifted back in, not dropped.
[[nodiscard]] std::uint64_t ticks_of(const svTimeVal& t) noexcept {
  return (static_cast<std::uint64_t>(t.high) << 32) | static_cast<std::uint64_t>(t.low);
}

// Whether the simulator exports its print routine. Without weak symbols the
// reference is hard, so a successful link means it is there (same rule as the
// time functions).
[[nodiscard]] bool vpi_printf_available() noexcept {
#if SIMLOG_HAS_WEAK_SYMBOLS
  return vpi_printf != nullptr;
#else
  return true;
#endif
}

// Precision exponent → {multiplier, symbol}. The DPI time API reports an
// exponent only, so the precision is the tick and the printed unit is the
// largest base unit at or below it, with the remaining decades folded into the
// number (same rule as the SystemC backend):
//   -12 → {1,"ps"}   -13 → {100,"fs"}   -11 → {10,"ps"}   -9 → {1,"ns"}   0 → {1,"s"}
// SystemVerilog timescales span 1/10/100 × fs..s, so exponents outside [-15, 0]
// are clamped to the range that has a symbol at all.
[[nodiscard]] unit_scale unit_for_precision(std::int32_t exponent) noexcept {
  struct exponent_symbol {
    std::int32_t exponent;
    const char* symbol;
  };
  constexpr std::array k_symbols{exponent_symbol{-15, "fs"}, exponent_symbol{-12, "ps"},
                                 exponent_symbol{-9, "ns"},  exponent_symbol{-6, "us"},
                                 exponent_symbol{-3, "ms"},  exponent_symbol{0, "s"}};
  if (exponent > 0) exponent = 0;      // coarser than 1 s has no symbol
  if (exponent < -15) exponent = -15;  // finer than 1 fs is not a legal timescale
  std::size_t i = 0;
  while (i + 1 < k_symbols.size() && k_symbols[i + 1].exponent <= exponent) ++i;
  std::uint64_t scale = 1;
  for (std::int32_t decade = 0; decade < exponent - k_symbols[i].exponent; ++decade) scale *= 10;
  return {scale, k_symbols[i].symbol};
}

// The range that can be logged: trace(0) … critical(5), spdlog's numbering. off
// (6) is a filter value, not a level to log at, so it is rejected like any other
// out-of-range number. The SV enum in sv/simlog_pkg.sv uses the same numbers; a
// rejected level therefore means a stale package or a hand-written import.
constexpr int kLastLevel = static_cast<int>(spdlog::level::critical);

// One-line notices for the bridge, each at most once per process. There is no
// locking: the library's single-threaded model covers the SV side too (a
// simulation runs on one thread), and the static initializers of C++11 are
// guarded anyway.
void warn_name_once() {
  static const bool once = [] {
    std::fprintf(stderr,
                 "[simlog] simlog_open: empty logger name; the handle is null and every "
                 "line for it is dropped\n");
    return true;
  }();
  (void)once;
}

void warn_level_once(int level) {
  static const bool once = [level] {
    std::fprintf(stderr,
                 "[simlog] simlog_log: level %d is not a log level (0 trace … 5 critical); "
                 "the line is dropped\n",
                 level);
    return true;
  }();
  (void)once;
}

void warn_message_once() {
  static const bool once = [] {
    std::fprintf(stderr, "[simlog] simlog_log: null message; the line is dropped\n");
    return true;
  }();
  (void)once;
}

void warn_log_handle_once() {
  static const bool once = [] {
    std::fprintf(stderr,
                 "[simlog] simlog_log: null logger handle (did simlog_open fail?); the line "
                 "is dropped\n");
    return true;
  }();
  (void)once;
}

void warn_flush_handle_once() {
  static const bool once = [] {
    std::fprintf(stderr, "[simlog] simlog_flush: null logger handle; nothing to flush\n");
    return true;
  }();
  (void)once;
}

[[nodiscard]] spdlog::logger* to_logger(void* handle) noexcept {
  return static_cast<spdlog::logger*>(handle);
}

}  // namespace

extern "C" {

// ── The backend contract declared in section 2 ───────────────────────
// simlog_sv_now_ticks / simlog_sv_resolution_unit / simlog_sv_write were
// declared there (the ABI, as the shared code sees it), so the compiler checks
// every definition below against those declarations instead of against a
// half-remembered copy.

// Current simulation tick, in units of the simulation precision (0 while no API
// reports a time, e.g. in a host-side program without a simulator).
std::uint64_t simlog_sv_now_ticks() {
  svTimeVal t{};         // zero-initialized: only type is set below
  t.type = sv_sim_time;  // 64-bit ticks, not a double
  if (dpi_time_api_available()) {
    if (svGetTime(nullptr, &t) != 0) {
      warn_no_sim_time_once();
      return 0;
    }
    return ticks_of(t);
  }
  if (read_vpi_time(t)) return ticks_of(t);
  warn_no_sim_time_once();
  return 0;
}

// Display form of the current simulation precision. The precision — not the
// time unit — is the tick, mirroring the SystemC backend, which prints ticks of
// the time resolution.
simlog::detail::unit_scale simlog_sv_resolution_unit() {
  std::int32_t exponent = 0;
  if (dpi_time_api_available()) {
    if (svGetTimePrecision(nullptr, &exponent) != 0) {
      warn_no_sim_time_once();
      return {1, "s"};
    }
    return unit_for_precision(exponent);
  }
  if (read_vpi_precision(exponent)) return unit_for_precision(exponent);
  warn_no_sim_time_once();
  return {1, "s"};
}

// ── The default sink's writer ────────────────────────────────────────
// A DPI-C library is an ordinary shared object: its stdout is the C runtime's
// stream, and `simv -l sim.log` does not see it, so a testbench's C++ log lines
// would be missing from the simulation log. The simulator's own print routine is
// the stream -l duplicates into the file and $display goes through, so the
// default sink (section 3) formats the line and hands it here:
//
//   vpi_printf("%.*s", size, text)      → the simulator's output stream
//   std::fwrite(text, 1, size, stdout)   → when the host has no VPI at all
//
// vpi_printf() is the standard IEEE 1800 VPI routine on purpose: it is what a
// simulator exports to a DPI-C library, VCS included, and it needs no vendor
// header. It is declared weak for the same reason the time functions are — a
// host without a simulator must still load the library and fall back, never
// fail to link. The line is passed as a "%.*s" argument, never as the format
// string: a message containing '%' is a message, not a printf template.
void simlog_sv_write(const char* text, std::size_t size) {
  if (text == nullptr || size == 0) return;
  // "%.*s" + the length keeps the line out of the format position.
  if (vpi_printf_available()) {
    vpi_printf("%.*s", static_cast<int>(size), text);
    return;
  }
  // No VPI host (a plain host-side program): the C runtime's stdout is then the
  // only stream there is. Flushed per line so redirecting it to a file behaves
  // like a simulator's log file.
  std::fwrite(text, 1, size, stdout);
  std::fflush(stdout);
}

// ── The SystemVerilog-facing C ABI ───────────────────────────────────
SIMLOG_API void* simlog_open(const char* name);
SIMLOG_API void simlog_log(void* handle, int level, const char* msg);
SIMLOG_API void simlog_flush(void* handle);

void* simlog_open(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    warn_name_once();
    return nullptr;
  }
  // simlog::get() is the whole logger contract: create + configure + register on
  // the first call, return the registered instance (untouched) afterwards.
  return simlog::get(name).get();
}

void simlog_log(void* handle, int level, const char* msg) {
  spdlog::logger* logger = to_logger(handle);
  if (logger == nullptr) {
    warn_log_handle_once();
    return;
  }
  if (level < 0 || level > kLastLevel) {
    warn_level_once(level);
    return;
  }
  if (msg == nullptr) {
    warn_message_once();
    return;
  }
  // The message arrives already formatted: SystemVerilog has neither variadic
  // functions nor variadic macros, so $sformatf() runs at the call site and only
  // the final string crosses the DPI boundary. "{}" + the message as an
  // argument, never the message as the format string: a message containing "{}",
  // "%d" or a lone '{' is a message, not a format template (and spdlog would
  // throw on the brace).
  logger->log(static_cast<spdlog::level::level_enum>(level), "{}", msg);
}

void simlog_flush(void* handle) {
  spdlog::logger* logger = to_logger(handle);
  if (logger == nullptr) {
    warn_flush_handle_once();
    return;
  }
  logger->flush();
}

}  // extern "C"

#undef SIMLOG_WEAK_SYMBOL
#undef SIMLOG_HAS_WEAK_SYMBOLS

#endif  // SIMLOG_BACKEND_SVDPI
