// Category 2 — SVDPI backend, C++ side (the bench half of the shared suite).
//
// tests/svdpi/cpp/core_dpi.cpp holds the scenario bodies (the same ones the
// SystemC suite runs). This bench only drives the clock:
//
//   1. pick the scenario and the resolution run.sh compiled for
//   2. call simlog_test_step() until it returns 0, applying #delay between calls
//   3. fail the run when any C++ check failed
//
// Nothing here formats or logs a line. The expected stamps are asserted inside
// C++ against the exact log output, so a wrong simulation-time backend fails the
// DPI-C process instead of being compared against output produced by that same
// backend.
//
// run.sh compiles this bench with -override_timescale, which is how the shared
// scenarios run under 1 ps / 100 fs / 1 ns / 1 fs precision. `+scale` and
// `+unit` are the resolution the runner compiled for, not something read from
// simlog, so the checks stay independent.
`timescale 1ps/1ps

module tb;
  import "DPI-C" function int simlog_test_begin(input string name,
                                                input longint unsigned scale,
                                                input string unit);
  import "DPI-C" function longint unsigned simlog_test_step();
  import "DPI-C" function int simlog_test_failures();

  initial begin
    string scenario;
    string unit;
    longint unsigned scale;

    if (!$value$plusargs("scenario=%s", scenario)) $fatal(1, "missing +scenario=<name>");
    if (!$value$plusargs("scale=%d", scale)) $fatal(1, "missing +scale=<ticks-per-unit>");
    if (!$value$plusargs("unit=%s", unit)) $fatal(1, "missing +unit=<symbol>");

    if (simlog_test_begin(scenario, scale, unit) != 0) begin
      $fatal(1, "unknown scenario: %s", scenario);
    end

    forever begin
      longint unsigned delay = simlog_test_step();
      if (delay == 0) break;
      #(delay);
    end

    if (simlog_test_failures() != 0) $fatal(1, "simlog checks failed");
    $finish;
  end
endmodule
