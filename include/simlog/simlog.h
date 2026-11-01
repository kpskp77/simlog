// simlog/simlog.h — the public surface of simlog: exactly one function,
// simlog::get(). All implementation lives in src/simlog.cpp, compiled once into
// libsimlog.so.
//
// simlog is an spdlog extension that stamps log lines with simulation time: the
// fixed pattern "[%*][%n][%^%l%$] %v" carries a %* flag that prints the current
// simulation time (tick count × resolution multiplier + base unit symbol, e.g.
// "[       12340 ps]"). This header carries no implementation on purpose: it
// never includes <systemc> or <svdpi.h>, so it needs no simulator or SystemC
// include path and sits next to <vpi_user.h> / <veriuser.h> without the
// t_vpi_time / VPI_TIME redefinition trap.
//
// Exactly one compile-time backend is compiled into the library, chosen by the
// SIMLOG_BACKEND_* macro defined on the `simlog` target in xmake.lua
// (--backend=..., SVDPI by default); code that includes this header needs no
// define at all:
//
//   SIMLOG_BACKEND_SVDPI    (default) the simulator's own time, read through the
//                           DPI time API (IEEE 1800-2023 svGetTime /
//                           svGetTimePrecision) or the VPI/PLI fallback
//                           (vpi_get_time / tf_gettimeprecision) on older
//                           simulators; the default sink prints through the
//                           simulator (vpi_printf), so `simv -l sim.log`
//                           contains the lines. The library also exports the
//                           SystemVerilog-facing C ABI (simlog_open /
//                           simlog_log / simlog_flush) that sv/simlog_pkg.sv
//                           wraps in `simlog_logger`.
//   SIMLOG_BACKEND_SYSTEMC  sc_time_stamp().value() × sc_get_time_resolution();
//                           the default sink is colored stdout.
//
// Where a finished line has to go is a property of the host (the simulator's
// output stream vs stdout), so the default sink follows the backend; the %*
// flag, the pattern, the levels and get() are the same for both. No
// initialization call of any kind is needed: the first simlog::get() lazily
// reads SIMLOG_LEVEL (once per process) and creates the logger.
//
// Single-threaded model (an EDA simulation runs on the main thread only): no
// locks, no atomics. Requires C++17.
#pragma once

#if __cplusplus < 201703L
#error "simlog requires C++17 or later"
#endif

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

// One shared object is one library state, so get() must resolve to the copy in
// libsimlog.so even when a consumer is built with hidden default visibility:
// SIMLOG_API pins default visibility (dllexport/dllimport on Windows, where
// SIMLOG_BUILD marks the library side of the boundary — set privately on the
// `simlog` target in xmake.lua).
#if defined(_WIN32)
#if defined(SIMLOG_BUILD)
#define SIMLOG_API __declspec(dllexport)
#else
#define SIMLOG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define SIMLOG_API __attribute__((visibility("default")))
#else
#define SIMLOG_API
#endif

namespace simlog {

// The library's only public API. Everything else — the time source, the %*
// flag, the default sink, the SIMLOG_LEVEL rules — lives in src/simlog.cpp (the
// test suites' white-box declarations are tests/simlog_detail.h, not this
// header):
//
//   - First call: create a native spdlog::logger → attach the simulation-time
//                 formatter (fixed pattern) → apply the level for this module
//                 (most specific SIMLOG_LEVEL rule, else the global level) →
//                 register with spdlog → return it
//   - Later calls / an already registered logger of the same name: returned
//                 directly by spdlog::get(), untouched — never re-leveled,
//                 re-patterned or re-sinked
//
// The result is a native spdlog::logger, so callers are free to add_sink /
// set_level. The name is std::string (not string_view) because spdlog's registry
// only accepts std::string: a view would force an allocation even on the
// registered-logger fast path.
[[nodiscard]] SIMLOG_API std::shared_ptr<spdlog::logger> get(const std::string& name);

}  // namespace simlog
