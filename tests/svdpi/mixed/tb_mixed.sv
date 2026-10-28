// Category 4 — mixed mode bench: C++ and SV log through one logger entity.
//
// run.sh loads only libsimlog_svdpi_mixed.so, which links libsimlog.so (where
// src/simlog.cpp lives). One shared object is one library state, so
// simlog::get() on the C++ side and simlog_open() behind simlog_logger share one
// registry.
//
// mixed_same_entity() returns 1 only when both paths resolve to the same logger;
// the expected lines asserted by run.sh then show that the two sides interleave
// on one stamp and obey one SIMLOG_LEVEL configuration.
`timescale 1ps/1ps

module tb;
  import simlog_pkg::*;
  import "DPI-C" function void mixed_cpp_log(input string name, input int level, input string msg);
  import "DPI-C" function void mixed_cpp_flush(input string name);
  import "DPI-C" function int mixed_same_entity(input string name);
  import "DPI-C" function int simlog_test_failures();

  string scenario;
  simlog_logger lg;

  initial begin
    if (!$value$plusargs("scenario=%s", scenario)) $fatal(1, "missing +scenario=<name>");

    case (scenario)
      // C++ creates the logger first; the SV proxy class must attach to it.
      "order": begin
        mixed_cpp_log("tb", SIMLOG_INFO, "cpp first");
        lg = new("tb");
        if (!mixed_same_entity("tb")) $fatal(1, "C++ and SV do not share the logger");
        lg.info("sv second");
        #10;
        lg.warn("sv third");
        mixed_cpp_log("tb", SIMLOG_INFO, "cpp fourth");
        lg.flush();
        mixed_cpp_flush("tb");
      end

      // The opposite creation order: SV opens the logger, C++ then logs through
      // simlog::get() and sees the same instance.
      "sv-first": begin
        lg = new("tb");
        lg.info("sv first");
        if (!mixed_same_entity("tb")) $fatal(1, "C++ and SV do not share the logger");
        mixed_cpp_log("tb", SIMLOG_INFO, "cpp second");
        #10;
        mixed_cpp_log("tb", SIMLOG_ERROR, "cpp third");
        lg.warn("sv fourth");
        lg.flush();
        mixed_cpp_flush("tb");
      end

      // SIMLOG_LEVEL is passed by the runner: one level for both sides.
      "level": begin
        lg = new("tb");
        if (!mixed_same_entity("tb")) $fatal(1, "C++ and SV do not share the logger");
        lg.info("sv info hidden");
        mixed_cpp_log("tb", SIMLOG_INFO, "cpp info hidden");
        lg.warn("sv warn kept");
        mixed_cpp_log("tb", SIMLOG_WARN, "cpp warn kept");
        lg.flush();
        mixed_cpp_flush("tb");
      end

      default: $fatal(1, "unknown scenario: %s", scenario);
    endcase

    if (simlog_test_failures() != 0) $fatal(1, "mixed C++ checks failed");
    $finish;
  end
endmodule
