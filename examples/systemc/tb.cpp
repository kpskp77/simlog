// SystemC integration example (SystemC backend: --backend=systemc /
// SIMLOG_BACKEND_SYSTEMC): a minimal testbench, no initialization call — just
// get() a logger and use it. examples/svdpi/tb.sv is the SystemVerilog
// counterpart (its own six lines, at 1 ps simulation resolution).
#include <simlog/simlog.h>

// sc_main is invoked by libsystemc, so it must stay exported if the program is
// ever built with hidden default visibility.
#if defined(_WIN32)
#define SIMLOG_EXAMPLE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define SIMLOG_EXAMPLE_EXPORT __attribute__((visibility("default")))
#else
#define SIMLOG_EXAMPLE_EXPORT
#endif

SIMLOG_EXAMPLE_EXPORT int sc_main(int, char**) {
  auto log = simlog::get("tb");
  sc_core::sc_spawn([&log] {
    log->warn("sim start, resolution: {}", sc_core::sc_get_time_resolution().to_string());
    for (int i = 1; i < 5; ++i) {
      sc_core::wait(10, sc_core::SC_NS);
      log->info("cnt={}, wake at {}", i, sc_core::sc_time_stamp().to_string());
    }
    log->error("sim end");
    sc_core::sc_stop();
  });
  sc_core::sc_start();
  return 0;
}
