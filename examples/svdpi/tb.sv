// SVDPI (DPI-C) integration example: a pure SystemVerilog testbench, no C++ of
// its own. `simlog_pkg::simlog_logger` is the SV face of the DPI-C library
// (libsimlog.so, built from src/simlog.cpp); the simulation-time stamp is added
// by the C++ side while the line is formatted, so nothing here may pass
// $time/$realtime in — that would be a second time source and a rounding step,
// exactly what simlog exists to avoid.
//
// Build and run (xmake run example does the same in the svdpi configuration):
//
//   vcs -full64 -sverilog -override_timescale=1ps/1ps sv/simlog_pkg.sv \
//     examples/svdpi/tb.sv <build>/libsimlog.so
//   ./simv
//
// -override_timescale is needed because sv/simlog_pkg.sv carries no `timescale
// and VCS rejects that next to a module that declares one; it also pins the
// simulation precision the documented output assumes. At 1 ps the six lines
// shown in README.md come out. The level knob is SIMLOG_LEVEL, read once on the
// first logger creation, so it has to be set before the run starts:
// `SIMLOG_LEVEL=tb.u_dut=off ./simv` drops the DUT line, `SIMLOG_LEVEL=warn
// ./simv` keeps only the warning and the error line.
`timescale 1ps/1ps

// The DUT logs on its own: one logger per module, each named with
// $sformatf("%m"). %m expands where the statement is written — here to this
// module's hierarchical name ("tb.u_dut") — which is what SIMLOG_LEVEL's module
// and wildcard rules match. The declaration initializer is the documented idiom;
// inside a task or function %m names that scope instead, and some simulators also
// name an unnamed block (VCS: "tb.unnamed$$_0").
module dut;
  import simlog_pkg::*;

  simlog_logger lg = new($sformatf("%m"));

  initial begin
    // 15000 ps = 15 ns, between two testbench steps: the order of the output
    // lines then does not depend on how the simulator schedules the two initial
    // blocks in the same time step.
    #15000;
    lg.warn("unexpected burst");
    lg.flush();
  end
endmodule

// The testbench. It is the only module that ends the simulation, so $finish
// comes after every logger has flushed: the simulator runs no destructors, and
// with stdout redirected to a file the last lines would otherwise be lost.
module tb;
  import simlog_pkg::*;

  simlog_logger lg = new($sformatf("%m"));
  dut u_dut();

  initial begin
    lg.info("sim start");
    for (int i = 1; i <= 3; ++i) begin
      #10000;  // 10000 ps = 10 ns
      // SystemVerilog has neither variadic functions nor variadic macros, so the
      // message is formatted at the call site; the level method takes the
      // finished string, and the level filters output, it does not skip the
      // $sformatf above.
      lg.info($sformatf("cnt=%0d", i));
    end
    lg.error("sim end");
    lg.flush();
    $finish;
  end
endmodule
