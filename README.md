# simlog — simulation-time stamps for spdlog in EDA simulation

**English** | [中文](README_zh.md)

A tiny [spdlog](https://github.com/gabime/spdlog) extension that prints **simulation-time** stamps
inside an EDA simulation, from one of two compile-time backends:

| Backend | Macro | Simulation time comes from | Needs |
|---|---|---|---|
| **SVDPI** (default) | `SIMLOG_BACKEND_SVDPI` | the DPI time API `svGetTime()` / `svGetTimePrecision()` (IEEE 1800-2023), or on older simulators `vpi_get_time()` / `tf_gettimeprecision()` | spdlog + `libsimlog.so` (compiled backend: `src/simlog.cpp`) |
| SystemC | `SIMLOG_BACKEND_SYSTEMC` | `sc_time_stamp()` × `sc_get_time_resolution()` | spdlog + SystemC (header-only) |

- **No custom logger type**: `simlog::get()` returns a native `spdlog::logger`, so `add_sink` /
  `set_level` work as usual

## Quick start

### SVDPI (default backend)

#### DPI-C code inside a SystemVerilog simulator

```cpp
#include <simlog/simlog.h>  // SIMLOG_BACKEND_SVDPI is the default: no define needed

extern "C" void tb_step(int cycle) {  // any function the SystemVerilog side imports
  auto log = simlog::get("tb_top");   // first call: create + register + attach the formatter
  log->info("cycle {}", cycle);       // from here on it is native spdlog API
}
```

Output at 1 ps simulation
precision:

```
[       12340 ps][tb_top][info] cycle 42
```

#### From SystemVerilog: `simlog_logger` (`sv/simlog_pkg.sv`)

```systemverilog
import simlog_pkg::*;

module tb;
  initial begin
    // %m is expanded here, so this is the module's hierarchical name — exactly the
    // name SIMLOG_LEVEL's module rules match.
    simlog_logger lg = new($sformatf("%m"));
    lg.info("reset released");
    #10;
    lg.warn($sformatf("cnt=%0d", 1));  // SystemVerilog has no variadic calls
    lg.flush();                        // $finish runs no destructors
  end
endmodule
```

```
[           0 ps][tb][info] reset released
[       10000 ps][tb][warning] cnt=1
```

`xmake` puts the library at `build/<platform>/<arch>/release/libsimlog.so`; hand it to the simulator
together with the SV sources:

```bash
vcs -full64 -sverilog -sv_lib <build-dir>/libsimlog tb.sv   # or pass the .so as a file
```

Where the lines end up is part of the backend: the SVDPI default sink prints through the simulator's
own output routine (`vpi_printf`), so `simv -l sim.log` contains them next to the `$display` lines.
A DPI-C library's own `printf` would not: `-l` mirrors the simulator's stream, not the C runtime's
stdout. Without a VPI host (a plain host-side program) the lines go to stdout as before.

| SystemVerilog | C ABI (`libsimlog.so`) | Notes |
|---|---|---|
| `new(name)` | `simlog_open(name)` | once per logger; the handle *is* the registered spdlog logger, so two opens of one name are one logger |
| `lg.trace/debug/info/warn/error/critical(msg)` | `simlog_log(handle, level, msg)` | `SIMLOG_TRACE` … `SIMLOG_CRITICAL` are spdlog's levels 0 … 5 |
| `lg.flush()` | `simlog_flush(handle)` | flush before `$finish` |

### SystemC (`--backend=systemc`)

```cpp
#include <simlog/simlog.h>

int sc_main(int, char**) {
  auto log = simlog::get("tb_top");  // first call: create + register + attach the formatter
  log->info("reset released");       // from here on it is native spdlog API
  // getting the same name again later returns the already registered instance
}
```

Output (resolution 1 ps):

```
[       12340 ps][tb_top][info] reset released
[       12340 ps][tb_top][warning] retry limit reached   ← same timestep, same stamp
[       12345 ps][tb_top][info] write complete, addr=0x1ff
```

## Configuration

There is no `init()`. A single environment variable, `SIMLOG_LEVEL`, carries both the global level
and the per-module overrides; its form is detected automatically and it is read once on the first
`simlog::get()` call:

| `SIMLOG_LEVEL` | Meaning |
|---|---|
| `info` | a bare level name (`trace/debug/info/warn/err/critical/off`) sets the global level |
| `*=debug,dma=info` | `module=level` rules, `,` separated, are applied per module |

Default: `info`. An invalid value never silently takes effect: the default level is kept (or the
entry ignored) and a one-line notice is written to stderr.

The environment is the **only run-time knob**; there is no `set_level()` API (two sources of truth).
Levels are applied once at logger creation; to change the level of an existing logger, call the
native `logger->set_level()`. The pattern is fixed to `[%*][%n][%^%l%$] %v` and cannot be
overridden through the environment.

### Per-module levels

```bash
SIMLOG_LEVEL="*=debug,dma=info"    # everything debug, except module "dma" at info
SIMLOG_LEVEL="info"                # simply the global level
```

- Each entry is `module=level`; the only separator is `,` and entries are taken verbatim, so an
  entry containing whitespace (`"*=debug, dma=info"`) is rejected with a stderr notice instead of
  silently becoming a pattern that can never match
- `module` is matched case-sensitively against the logger name passed to `simlog::get()`
- `*` matches any sequence of characters (including none), so a lone `*` is the global rule and
  `dma*` / `*_tb` narrow it down
- The **most specific matching rule wins**: an exact name beats any wildcard, then more literal
  characters, then the rule written later — so `*=debug,dma=info` and `dma=info,*=debug` behave
  identically
- Modules without a matching rule use the global level, and so does the module of a malformed entry
- Only applied when `simlog::get()` creates the logger; an existing logger (including one the user
  registered) is never re-leveled

## Build

The project uses xmake; spdlog (shared library — one logger registry per process), svdpi
(header-only, the DPI-C `<svdpi.h>`, needed only to compile `src/simlog.cpp`) and SystemC (shared
library, SystemC backend only) come from xrepo.

```bash
xmake                          # default backend (svdpi): libsimlog.so, built from include/ + src/
xmake run example              # compile + run examples/svdpi/tb.sv in VCS; skips without vcs
xmake run vcs-test             # categories 2-4 in a real simulator (VCS); skips without vcs
xmake f --backend=systemc -y   # switch to the SystemC backend
xmake run example              # the SystemC example, 6 documented lines
xmake test                     # the unit suite — SystemC configuration only
xmake f --backend=svdpi -y     # switch back
```

The backend option is the compile-time macro of the same name: `--backend=svdpi` defines
`SIMLOG_BACKEND_SVDPI` for everything that depends on the `simlog` target, `--backend=systemc`
defines `SIMLOG_BACKEND_SYSTEMC`. Consumers that use the header without xmake simply define one
macro, or nothing at all for the default backend.

`libsimlog.so` is NEEDED on `libspdlog.so`: outside xmake (a simulator loading it by hand), make
sure the shared spdlog is findable, e.g. via `LD_LIBRARY_PATH`. Under xmake, `xmake run` and
`xmake test` inject the package lib dir automatically.

## Tests

The suite has four categories. Categories 1 and 2 run the same scenario bodies
([`tests/cases.cpp`](tests/cases.cpp)), so the API contract is checked on both backends from one
source; categories 3 and 4 exist because the SystemVerilog side is a separate surface.

| # | Category | Where | How |
|---|---|---|---|
| 1 | SystemC backend | [`tests/systemc/tests.cpp`](tests/systemc/tests.cpp) + [`tests/cases.cpp`](tests/cases.cpp) | `xmake test` — 15 scenarios, one process each |
| 2 | SVDPI, simlog used from C++ | [`tests/svdpi/cpp/`](tests/svdpi/cpp) + the same `cases.cpp` | `xmake run vcs-test` — the shared scenarios assert exact log lines inside C++, the bench only applies `#delay` |
| 3 | SVDPI, simlog used from SV | [`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv) | `xmake run vcs-test` — expected lines supplied to the runner, diffed against `simv -l sim.log` |
| 4 | mixed C++ + SV, one logger | [`tests/svdpi/mixed/`](tests/svdpi/mixed) | `xmake run vcs-test` — diffed lines plus a same-entity check |

```bash
xmake f --backend=systemc -y && xmake test    # category 1: 15 scenarios
xmake test -v                                 # also show the output of passing tests
xmake run tests stamp                         # run a single scenario directly
xmake f --backend=svdpi -y && xmake run vcs-test   # categories 2-4 in a real simulator (VCS)
```

How the categories work internally — one process per scenario, why there is no host-side SVDPI
harness, what each scenario covers — is maintenance detail; see [AGENTS.md](AGENTS.md) (`Test
conventions`) and the scenario bodies in [`tests/cases.cpp`](tests/cases.cpp).
