// Category 3 — SVDPI backend, logging through the SystemVerilog proxy class
// (sv/simlog_pkg.sv).
//
// Nothing in this bench imports the C ABI: it creates simlog_logger objects the
// way a user does and lets the package hide simlog_open/simlog_log/simlog_flush.
// The expected log lines are supplied by the runner, because the assertions live
// in C++ in categories 1 and 2 and the SV side cannot capture stdout: run.sh
// diffs the exact lines, which is where the stamps and the level filtering are
// checked.
//
// Scenarios are selected with +scenario=<name> so one compiled simv covers the
// whole category. SIMLOG_LEVEL stays a runner concern (the `env` case).
`timescale 1ps/1ps

module tb;
  import simlog_pkg::*;

  string scenario;

  // Every level method: trace/debug are filtered by the default info level, the
  // rest must be printed with spdlog's level names. The logger is created in the
  // initial block below with $sformatf("%m") — the documented idiom; inside
  // this task %m would expand to the task scope instead.
  task automatic levels_case(simlog_logger lg);
    lg.trace("trace hidden");
    lg.debug("debug hidden");
    lg.info("info kept");
    lg.warn("warn kept");
    lg.error("error kept");
    lg.critical("critical kept");
    #10;
    lg.info("info after delay");
    lg.flush();
  endtask

  // The message is an argument, never a format string: braces, percent signs and
  // $sformatf output have to survive verbatim.
  task automatic format_case();
    simlog_logger lg;
    lg = new("tb");
    lg.info("plain");
    lg.warn($sformatf("braces {} percent %% value=%0d", 42));
    lg.error("format chars: %d %s {");
    #20;
    lg.info("after 20");
    lg.flush();
  endtask

  // Two loggers at the same time: the logger name selects the SIMLOG_LEVEL rule,
  // the stamp comes from the one time source, so both lines carry the same time.
  task automatic modules_case();
    simlog_logger tb_lg;
    simlog_logger dma_lg;
    tb_lg = new("tb");
    dma_lg = new("dma0");
    tb_lg.info("tb first");
    dma_lg.info("dma first");
    #10;
    dma_lg.warn("dma second");
    tb_lg.info("tb second");
    tb_lg.flush();
    dma_lg.flush();
  endtask

  // SIMLOG_LEVEL is set by the runner: "tb=warn" filters tb at info while dma0
  // keeps the built-in global level.
  task automatic env_case();
    simlog_logger tb_lg;
    simlog_logger dma_lg;
    tb_lg = new("tb");
    dma_lg = new("dma0");
    tb_lg.info("tb info hidden");
    tb_lg.warn("tb warn kept");
    dma_lg.info("dma0 info kept");
    dma_lg.debug("dma0 debug hidden");
    tb_lg.flush();
    dma_lg.flush();
  endtask

  // A broken logger name/handle is reported once by the C side and the line is
  // dropped; the valid logger after it still works.
  task automatic empty_name_case();
    simlog_logger bad;
    simlog_logger lg;
    bad = new("");
    bad.info("dropped");
    lg = new("tb");
    lg.info("valid");
    lg.flush();
  endtask

  initial begin
    // Declaration initializer on purpose: this is the documented $m idiom and
    // VCS expands %m to the module scope here ("tb"). A later assignment inside
    // the same unnamed block would expand to tb.unnamed$$_0 instead.
    simlog_logger root_lg = new($sformatf("%m"));

    if (!$value$plusargs("scenario=%s", scenario)) $fatal(1, "missing +scenario=<name>");

    case (scenario)
      "levels": levels_case(root_lg);
      "format": format_case();
      "modules": modules_case();
      "env": env_case();
      "empty-name": empty_name_case();
      default: $fatal(1, "unknown scenario: %s", scenario);
    endcase

    $finish;
  end
endmodule
