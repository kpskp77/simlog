// simlog_pkg — the SystemVerilog side of the SVDPI backend.
//
// simlog.h is the C++ side of the library; a testbench written in SystemVerilog
// cannot call simlog::get() directly, so this package wraps the small C ABI of
// src/simlog.cpp (the DPI-C library `libsimlog.so`) in the one object a
// testbench actually wants:
//
//   import simlog_pkg::*;
//
//   module tb;
//     initial begin
//       // %m is expanded where this statement is written, i.e. to this module's
//       // hierarchical name ("tb", "tb.dut.u_dma", ...), which is exactly the
//       // name SIMLOG_LEVEL's module rules match. Create the logger at module
//       // scope like this: inside a task/function %m names that scope instead,
//       // and some simulators also name an unnamed block (VCS: "tb.unnamed$$_0").
//       simlog_logger lg = new($sformatf("%m"));
//       lg.info("reset released");
//       lg.warn($sformatf("cnt=%0d", cnt));
//       lg.flush();  // before $finish: the simulator runs no destructors
//     end
//   endmodule
//
// Compile it next to the DPI-C library (vcs ... simlog_pkg.sv tb.sv libsimlog.so,
// or add sv/ to +incdir+ and `include it).
//
// The timestamp is never passed in: the C++ side reads the simulation time
// itself when it formats the line, so nothing here may hand over $time or
// $realtime. Passing a time in would add a second time source and a rounding
// step, which is precisely what simlog exists to avoid.
//
// Why the methods take a ready-made string: SystemVerilog has neither variadic
// functions nor variadic macros, so `lg.info("cnt=%0d", cnt)` cannot exist.
// Formatting happens at the call site with $sformatf() — which also means it
// happens even when SIMLOG_LEVEL filters the line out; the level is for output
// control, not for skipping work.
`ifndef SIMLOG_PKG_SV
`define SIMLOG_PKG_SV

package simlog_pkg;

  // spdlog's levels in spdlog's numbering: the C side casts the int straight to
  // spdlog::level::level_enum, and SIMLOG_LEVEL speaks the same names
  // (trace/debug/info/warn/err/critical). "off" is a filter value, not a level, so
  // it is not part of this enum; the C side rejects it like any other out-of-range
  // number (one stderr notice, line dropped).
  typedef enum int {
    SIMLOG_TRACE    = 0,
    SIMLOG_DEBUG    = 1,
    SIMLOG_INFO     = 2,
    SIMLOG_WARN     = 3,
    SIMLOG_ERROR    = 4,
    SIMLOG_CRITICAL = 5
  } simlog_level_e;

  // The C ABI of src/simlog.cpp, one for one: a chandle is a
  // void*, an int is an int, and a string is a const char* borrowed for the
  // duration of the call. The imports are not "context": the backend asks the
  // simulator for the global simulation time through a null scope, so no scope
  // of the caller is needed and these stay cheap.
  import "DPI-C" function chandle simlog_open(input string name);
  import "DPI-C" function void simlog_log(chandle handle, input int level, input string msg);
  import "DPI-C" function void simlog_flush(chandle handle);

  // One logger: open it once (typically in the module that logs, with $sformatf
  // ("%m") as the name) and use it from anywhere — modules, classes, functions,
  // tasks. The handle is the native spdlog logger the C++ side registered, so
  // the simulation-time stamp, the pattern and the SIMLOG_LEVEL rules are the
  // same as for any other simlog logger; the object adds no policy of its own.
  //
  // A logger name that is empty leaves the handle null: every call is then a
  // no-op and the C++ side says so once on stderr. It is not worth a SystemVerilog
  // check here — the diagnostic cannot be more precise than that one.
  class simlog_logger;
    local chandle handle;
    local string  name;

    function new(string name);
      this.name   = name;
      this.handle = simlog_open(name);
    endfunction

    // The name this logger was opened with (useful for messages; not the same
    // thing as the C++ logger's name when this is a copy).
    function string get_name();
      return name;
    endfunction

    function void trace(string msg);
      simlog_log(handle, SIMLOG_TRACE, msg);
    endfunction

    function void debug(string msg);
      simlog_log(handle, SIMLOG_DEBUG, msg);
    endfunction

    function void info(string msg);
      simlog_log(handle, SIMLOG_INFO, msg);
    endfunction

    function void warn(string msg);
      simlog_log(handle, SIMLOG_WARN, msg);
    endfunction

    function void error(string msg);
      simlog_log(handle, SIMLOG_ERROR, msg);
    endfunction

    function void critical(string msg);
      simlog_log(handle, SIMLOG_CRITICAL, msg);
    endfunction

    // Flush this logger's sinks. Call it (or let a `final` block call it) before
    // the simulation ends: $finish ends the process without running destructors,
    // and with stdout redirected to a file the last lines would be lost.
    function void flush();
      simlog_flush(handle);
    endfunction
  endclass

endpackage

`endif
