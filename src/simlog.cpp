// src/simlog.cpp — the SVDPI backend of simlog, in one translation unit:
//
//   1. the time source (asks the simulator for the global simulation time)
//   2. the writer behind the default sink (prints the line through the simulator,
//      so `simv -l sim.log` contains it)
//   3. the SystemVerilog-facing C ABI (`simlog_open` / `simlog_log` /
//      `simlog_flush`) that sv/simlog_pkg.sv wraps in `simlog_logger`
//
// All three are compiled into libsimlog.so (the `simlog` target, SVDPI
// configuration only). Keeping them here is what leaves simlog.h free of
// `<svdpi.h>` and of any simulator declaration, so DPI-C code may include
// `<vpi_user.h>` / `<veriuser.h>` next to it without the `t_vpi_time` / `VPI_TIME`
// redefinition trap, and the time functions are weak references in one library
// instead of in every user translation unit.
//
// The split is asymmetric on purpose: SystemC stays header-only, SVDPI is compiled
// because it exists to be *loaded* by a simulator. Using it means linking this file
// (as libsimlog.so, or compiled into the DPI-C library that logs):
//
//   vcs -full64 -sverilog -sv_lib <path>/libsimlog tb.sv        # SystemVerilog only
//   g++ -shared my_dpi.cpp src/simlog.cpp -I...                 # own DPI library
//
// One shared object is one library state: the spdlog registry, the default sink and
// the SIMLOG_LEVEL snapshot live in the copy of `simlog::get()` inside that .so.
// Loading libsimlog.so *and* building the same code into another DPI library gives
// two independent loggers per name — compile this file into the library that also
// holds the C++ that logs.
//
// ── 1. The time source ───────────────────────────────────────────────
// The simulator is asked for the global simulation time (null scope = "the whole
// simulation"), in two ways, tried in this order:
//
//   1. the DPI time API (IEEE 1800-2023):
//        svGetTime(NULL, &t)            → 64-bit tick count, t.type = sv_sim_time
//        svGetTimePrecision(NULL, &exp) → precision exponent, e.g. -12 for 1 ps
//   2. the VPI/PLI time functions, which simulators have provided far longer:
//        vpi_get_time(NULL, &t)         → the same tick count, t.type = vpiSimTime
//        tf_gettimeprecision()          → the same exponent
//
// Path 1 alone is not enough: svGetTime/svGetTimePrecision only entered the DPI
// annex with IEEE 1800-2023, so pre-2023 simulators (VCS O-2018.09 neither exports
// nor declares them) fail to link against a plain reference. Both are therefore
// declared weak: a 2023-capable simulator binds its own definition at load time, an
// older one leaves the pointers null and path 2 answers. With neither (a host-side
// program with no simulator) the stamp stays 0 and one stderr line says so.
//
// The two paths are never mixed within a process, and the value is read per
// format() call, so the stamp is the time of the log call, never a cached value.
// Both ask for the integer tick count, never a scaled real (a double would
// reintroduce the rounding the tick arithmetic avoids). No VPI handle is looked up
// and no VPI callback is registered: vpi_get_time(NULL, …) is a value query.
//
// ── 2. The SystemVerilog-facing C ABI ────────────────────────────────
//   void* simlog_open(const char* name);                            logger handle
//   void  simlog_log(void* handle, int level, const char* msg);     log one line
//   void  simlog_flush(void* handle);                               flush that logger
//
// The handle *is* the logger simlog::get() returned: that call already creates,
// configures and registers it, and the registry keeps it alive for the rest of the
// process, so the pointer stays valid (never spdlog::drop() it), two opens of one
// name return the same pointer, and an already registered logger is returned
// untouched. A handle instead of a name per call because the SV side opens once and
// then passes one pointer per line, whereas a name would marshall a string and look
// the logger up in the registry on every line.
//
// The bridge adds no semantics of its own: levels (SIMLOG_LEVEL), the pattern and
// the time source stay in simlog.h. In particular the SV side never passes a
// timestamp in — the stamp is read inside the flag's format() call, so it is the
// time of the log call. Passing $time or $realtime would add a second time source
// and a rounding step, which is the one thing this ABI deliberately does not offer.
//
// A bad argument is reported once per process on stderr and the line is dropped,
// never silently applied — the same rule SIMLOG_LEVEL and the time source follow.
//
// ── Export list ──────────────────────────────────────────────────────
// The six `extern "C"` functions below are the ABI: they are marked SIMLOG_EXPORT
// (default visibility) so `nm -D libsimlog.so` lists them by name. The header's
// inline functions are exported weak as usual — that is what lets another DPI-C
// library that links libsimlog.so share this library's state (the category-4 test
// relies on it).
//
// There is deliberately no header for the bridge: nothing outside this file needs
// those declarations. C++ testbench code includes simlog.h; a SystemVerilog
// testbench loads the library; a DPI-C library that wants the bridge compiles this
// file. The other side of the contract is sv/simlog_pkg.sv, which imports the three
// names — C linkage is name-only, so the two have to be kept in step by hand: a
// renamed or re-signatured function still links and corrupts the arguments.

#if defined(SIMLOG_BACKEND_SYSTEMC)
#error "src/simlog.cpp is the SVDPI backend; it is not part of the SystemC configuration"
#endif

#include <simlog/simlog.h>

// The simulator's own DPI header: only this translation unit needs it, and only
// here may svScope / svTimeVal / sv_sim_time appear.
#include <svdpi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// The six entry points below are exported by name (SIMLOG_EXPORT); the header's
// inline functions stay weak-exported too, so they can be interposed. On Windows
// dllexport is what exports them at all.
#if defined(_WIN32)
#define SIMLOG_EXPORT __declspec(dllexport)
#define SIMLOG_WEAK_SYMBOL
#define SIMLOG_HAS_WEAK_SYMBOLS 0
#elif defined(__GNUC__) || defined(__clang__)
#define SIMLOG_EXPORT __attribute__((visibility("default")))
#define SIMLOG_WEAK_SYMBOL __attribute__((weak))
#define SIMLOG_HAS_WEAK_SYMBOLS 1
#else
#define SIMLOG_EXPORT
#define SIMLOG_WEAK_SYMBOL
#define SIMLOG_HAS_WEAK_SYMBOLS 0
#endif

#if SIMLOG_HAS_WEAK_SYMBOLS
// <svdpi.h> has already declared the first two; the weak attribute on the
// redeclaration is what turns the hard link-time dependency into a run-time check.
// The VPI/PLI ones are declared here instead of including
// <vpi_user.h>/<veriuser.h>: this file must compile without any simulator include
// path beyond the header-only `svdpi` package, and only the handful of scalar
// signatures involved are needed (svTimeVal *is* VPI's s_vpi_time, PLI_INT32 is
// int, and vpi_printf's PLI_INT32 return is int).
extern "C" {
SIMLOG_WEAK_SYMBOL int svGetTime(const svScope scope, svTimeVal* time);
SIMLOG_WEAK_SYMBOL int svGetTimePrecision(const svScope scope, std::int32_t* precision);
SIMLOG_WEAK_SYMBOL void vpi_get_time(void* object, svTimeVal* time);
SIMLOG_WEAK_SYMBOL std::int32_t tf_gettimeprecision(void);
SIMLOG_WEAK_SYMBOL int vpi_printf(const char* format, ...);
}  // extern "C"
#endif  // SIMLOG_HAS_WEAK_SYMBOLS

namespace {

using simlog::detail::unit_scale;

// Reported once per process: a host that answers neither time API would otherwise
// leave every stamp at 0 without a trace (a silent wrong value is worse than a
// line on stderr).
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

// Whether the IEEE 1800-2023 DPI time API is present in this process. Without weak
// symbols the reference is hard, so a successful link means it is there.
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

// Precision exponent → {multiplier, symbol}. The DPI time API reports an exponent
// only, so the precision is the tick and the printed unit is the largest base unit
// at or below it, with the remaining decades folded into the number (same rule as
// the SystemC backend):
//   -12 → {1,"ps"}   -13 → {100,"fs"}   -11 → {10,"ps"}   -9 → {1,"ns"}   0 → {1,"s"}
// SystemVerilog timescales span 1/10/100 × fs..s, so exponents outside [-15, 0] are
// clamped to the range that has a symbol at all.
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
// simulation runs on one thread), and the static initializers of C++11 are guarded
// anyway.
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

// ── The backend contract declared in simlog.h ────────────────────────
// Declarations first (the ABI, as simlog.h sees it), so the compiler checks every
// definition below against them instead of against a half-remembered copy.
SIMLOG_EXPORT std::uint64_t simlog_sv_now_ticks();
SIMLOG_EXPORT simlog::detail::unit_scale simlog_sv_resolution_unit();
SIMLOG_EXPORT void simlog_sv_write(const char* text, std::size_t size);

SIMLOG_EXPORT void* simlog_open(const char* name);
SIMLOG_EXPORT void simlog_log(void* handle, int level, const char* msg);
SIMLOG_EXPORT void simlog_flush(void* handle);

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

// Display form of the current simulation precision. The precision — not the time
// unit — is the tick, mirroring the SystemC backend, which prints ticks of the
// time resolution.
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
// default sink (simlog.h) formats the line and hands it here:
//
//   vpi_printf("%.*s", size, text)      → the simulator's output stream
//   std::fwrite(text, 1, size, stdout)   → when the host has no VPI at all
//
// vpi_printf() is the standard IEEE 1800 VPI routine on purpose: it is what a
// simulator exports to a DPI-C library, VCS included, and it needs no vendor
// header. It is declared weak for the same reason the time functions are — a host
// without a simulator must still load the library and fall back, never fail to
// link. The line is passed as a "%.*s" argument, never as the format string: a
// message containing '%' is a message, not a printf template.
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
  // the final string crosses the DPI boundary. "{}" + the message as an argument,
  // never the message as the format string: a message containing "{}", "%d" or a
  // lone '{' is a message, not a format template (and spdlog would throw on the
  // brace).
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

#undef SIMLOG_EXPORT
#undef SIMLOG_WEAK_SYMBOL
#undef SIMLOG_HAS_WEAK_SYMBOLS
