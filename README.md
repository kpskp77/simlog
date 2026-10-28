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
- **Nothing simulator-specific in your translation units**: `<svdpi.h>` and the VPI/PLI
  declarations live in the compiled SVDPI backend, not in the header
- `%*` in the pattern is the simulation time (spdlog's `custom_flag_formatter`, no `init()` call)
- Only the time reading differs between backends: `%*`, the pattern, the levels and `get()` are
  shared code

## Quick start

### SVDPI (default backend) — DPI-C code inside a SystemVerilog simulator

```cpp
#include <simlog/simlog.h>  // SIMLOG_BACKEND_SVDPI is the default: no define needed

extern "C" void tb_step(int cycle) {  // any function the SystemVerilog side imports
  auto log = simlog::get("tb_top");   // first call: create + register + attach the formatter
  log->info("cycle {}", cycle);       // from here on it is native spdlog API
}
```

Compile it into the DPI-C shared library as usual (`-fPIC -shared`, spdlog's and the `svdpi`
package's include paths, no simulator library to link) **plus `src/simlog.cpp`**, or simply link
`libsimlog.so`. The simulator's time functions are weak references in that library, so they resolve
from the simulator when it loads it (see [SVDPI backend](#svdpi-backend)). Output at 1 ps simulation
precision:

```
[       12340 ps][tb_top][info] cycle 42
```

#### From SystemVerilog: `simlog_logger` (`sv/simlog_pkg.sv`)

Testbench code should not need one DPI import per log call, so the SV side gets one object per
logger, backed by three `extern "C"` functions of [`src/simlog.cpp`](src/simlog.cpp) — the same file
that holds the SVDPI backend. A testbench with no C++ of its own therefore only loads
`libsimlog.so`:

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

The library links no simulator library at all: its time functions stay weak, unresolved references
that the simulator binds when it loads it, the same way as for a hand-written DPI-C file. **If your
DPI-C library already contains simlog C++ code, compile `src/simlog.cpp` into that library instead of
loading a second one** — each shared object keeps its own logger registry, default sink and
`SIMLOG_LEVEL` snapshot, so two libraries would hold two independent loggers under the same name.

| SystemVerilog | C ABI (`libsimlog.so`) | Notes |
|---|---|---|
| `new(name)` | `simlog_open(name)` | once per logger; the handle *is* the registered spdlog logger, so two opens of one name are one logger |
| `lg.trace/debug/info/warn/error/critical(msg)` | `simlog_log(handle, level, msg)` | `SIMLOG_TRACE` … `SIMLOG_CRITICAL` are spdlog's levels 0 … 5 |
| `lg.flush()` | `simlog_flush(handle)` | flush before `$finish` |

- **The message is a finished string.** SystemVerilog has neither variadic functions nor variadic
  macros, so `$sformatf` runs at the call site; the stamp still comes from the backend, and the SV
  side never passes `$time`/`$realtime` in (a second time source plus a rounding step)
- **The name is explicit.** Inside a module `new($sformatf("%m"))` gives the hierarchical name; code
  that logs from classes passes its own name. The package does not guess it: `svGetScope()` needs a
  `context` import and returns the scope where the import was *declared* — a package
- **Bad arguments are reported, never applied**: an empty name, a null handle or a level outside
  0 … 5 produces one stderr notice per process and the line is dropped, like an invalid
  `SIMLOG_LEVEL`
- **The C ABI is a facade**: it calls `simlog::get()`, logs through the native logger and adds no
  policy of its own (no re-levelling, no pattern, no second time source). The five `extern "C"`
  functions are the ABI (`SIMLOG_EXPORT`, default visibility); the header's inline functions stay
  weak-exported, which is what lets a DPI-C library that links `libsimlog.so` share its state

### SystemC — testbench code (`--backend=systemc`)

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

### The examples

Each backend ships a minimal example; `xmake run example` runs the one of the configured backend. The
SVDPI example is a pure SystemVerilog testbench: two modules, each creating its own `simlog_logger`
with `$sformatf("%m")`, so the stamps and the logger names are the ones a real testbench sees (the
DUT's line carries `tb.u_dut`, which `SIMLOG_LEVEL`'s module rules match):

```
$ xmake f --backend=svdpi -y && xmake run example
[           0 ps][tb][info] sim start
[       10000 ps][tb][info] cnt=1
[       15000 ps][tb.u_dut][warning] unexpected burst
[       20000 ps][tb][info] cnt=2
[       30000 ps][tb][info] cnt=3
[       30000 ps][tb][error] sim end

$ xmake f --backend=systemc -y && xmake run example
[           0 ps][tb][warning] sim start, resolution: 1 ps
[       10000 ps][tb][info] cnt=1, wake at 10 ns
[       20000 ps][tb][info] cnt=2, wake at 20 ns
[       30000 ps][tb][info] cnt=3, wake at 30 ns
[       40000 ps][tb][info] cnt=4, wake at 40 ns
[       40000 ps][tb][error] sim end
```

[`examples/svdpi/run.sh`](examples/svdpi/run.sh) compiles and runs `tb.sv` with VCS, compares exactly
the lines above and skips itself when `vcs` is missing; [`examples/systemc/tb.cpp`](examples/systemc/tb.cpp)
is the SystemC `sc_main` testbench.

## Time source: one per backend

The stamp is always **tick count × resolution multiplier**, with the unit printed as a plain symbol.
Which clock the ticks come from is the backend's business.

### SVDPI backend

The simulator is asked for the global simulation time with a null scope, i.e. "the whole simulation",
and for **integer ticks**, never for a double. There are two ways to ask, tried in this order:

| Path | Value | Unit |
|---|---|---|
| DPI time API (IEEE 1800-2023) | `svGetTime(NULL, &t)` → 64-bit tick count in `high`/`low`, `t.type = sv_sim_time` | `svGetTimePrecision(NULL, &exp)` → precision exponent |
| VPI/PLI | `vpi_get_time(NULL, &t)` with `vpiSimTime` → the same tick count | `tf_gettimeprecision()` → the same exponent |

Path 1 only entered the DPI annex with IEEE 1800-2023, so simulators released before that do not
implement it — VCS O-2018.09, for example, does not even declare the two functions in its
`<svdpi.h>` — and a plain hard reference would fail to link. The two symbols are therefore **weak
references**: a 2023-capable simulator binds its own definition and path 1 is used, an older one
leaves the pointers null and path 2 answers. Either way there is exactly one time source per process,
it is read per log call, and it yields an integer tick count in units of the simulation precision (0
before `initial` starts).

Path 2 is a plain value query with a null object: no VPI handle is looked up, no VPI callback is
registered, and `sv_sim_time` and `vpiSimTime` are the same representation, so both paths return the
identical tick count. On VCS `tf_gettimeprecision()` reports the global (finest) simulation
precision, which is what the DPI null scope means — multi-timescale testbenches included.

A precision that is not a single base unit is **folded into the number**, exactly like the SystemC
backend's resolution multiplier, so the unit column always holds `fs/ps/ns/us/ms/s`:

```
precision 1 ps    (exp -12) → {1, "ps"}     [       12340 ps]
precision 100 fs  (exp -13) → {100, "fs"}   [     1234000 fs]   (12340 tick × 100 fs = 1.234 ns)
precision 10 ps   (exp -11) → {10, "ps"}    [      123400 ps]
precision 1 ns    (exp -9)  → {1, "ns"}     [       12340 ns]
```

The standard `<svdpi.h>` comes from the header-only [`svdpi`](https://github.com/kpskp77/svdpi)
package (`add_requires("svdpi")`), and the VPI/PLI functions are declared by `src/simlog.cpp` itself.
Neither is in `simlog.h`, so a DPI-C translation unit sees no simulator declaration at all and needs
no include-order trick for `<vpi_user.h>` / `<veriuser.h>`. A host that answers neither API gets an
all-zero stamp plus a one-line notice on stderr, once.

### SystemC backend

The time source is SystemC, and **no initialization call** is needed:

- Value: `sc_time_stamp().value()` (integer tick count, 0 during elaboration) × the resolution multiplier
- Unit: the resolution's base unit, printed as a plain symbol only (`fs/ps/ns/us/ms/s`)

A resolution with a multiplier (e.g. 100 fs per tick) does not put the multiplier into the unit; it is
**folded into the tick count**, so all arithmetic is integral with zero floating-point error and
`sc_set_time_resolution()` takes effect immediately:

```
resolution 1 ps   → 1 ps per tick   → [       12340 ps]   (= 12.34 ns)
resolution 100 fs → 100 fs per tick → [     1234000 fs]   (12340 tick × 100 fs = 1.234 ns)
resolution 10 ns  → 10 ns per tick  → [      123400 ns]   (12340 tick × 10 ns = 123.4 us)
```

There is no time-source injection, no manual time advancement and no display-unit configuration.

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

## Design

**There is exactly one public C++ API**: `simlog::get()`. Everything else (time reading / resolution
multiplier and unit / `%*` flag / formatter / environment parsing / default sink) lives in
`simlog::detail`.

The one addition is for the other side of the DPI boundary, and it is not a second public header:
three `extern "C"` functions — `simlog_open`, `simlog_log`, `simlog_flush` — live in
`src/simlog.cpp` (declarations and definitions in one file, no header) and are built into
`libsimlog.so`, so SystemVerilog can reach a logger without a hand-written DPI import;
`sv/simlog_pkg.sv` wraps them in `simlog_logger`. `include/simlog/` still holds exactly one file.

**Exactly one backend is compiled in.** The two backends are alternative, not layered: one time
source each, chosen with `SIMLOG_BACKEND_SVDPI` (default) or `SIMLOG_BACKEND_SYSTEMC`; defining both
is a compile error. They meet the rest of the library at exactly two functions, which is why nothing
else has to know which one is active:

```
simlog::get(name)          ← lazy factory: create native logger + attach formatter + register
  └─ detail::sim_time_flag (%*)          ← custom_flag_formatter: reads the clock per log line
       └─ detail::now_ticks() + detail::resolution_unit()   ← the only backend-specific pair
            ├─ SVDPI:   svGetTime / svGetTimePrecision       (IEEE 1800-2023)
            │           vpi_get_time / tf_gettimeprecision   (older simulators)
            └─ SystemC: sc_time_stamp / sc_get_time_resolution
```

**Single-threaded model**: an EDA simulation runs only on the main thread, so the library has no
locks and no atomics — the registration path in `simlog::get()` has no mutex, and the default sink is
the `_st` single-threaded variant. The stamp is computed integrally (tick count × multiplier), so
there is no floating-point error in either backend.

## Known limitations

- **Synchronous loggers only**: an async logger calls `format()` on a background thread, so the
  stamp would be the time of output rather than the time of the log call
- **The backend is a compile-time choice**, one per program (a shared DPI-C library is either a
  SystemC build or a DPI build, not both)
- **SVDPI: simulators older than IEEE 1800-2023** are served by the VPI/PLI fallback, which needs
  the code to run inside a simulator (a bare host-side program has neither API and stamps 0). It
  returns the same tick count and precision exponent as the DPI API; verified with VCS
  O-2018.09-SP2 via `xmake run vcs-test`
- **The SystemVerilog side formats before it logs**: because SV has no variadic calls, `$sformatf`
  runs at the call site, so a line that `SIMLOG_LEVEL` filters out has already paid for formatting
- **One line per call**: a message with embedded newlines prints them as-is, so only its first line
  carries the stamp
- **SVDPI is the compiled backend**: unlike SystemC it is not header-only — link `libsimlog.so`, or
  compile `src/simlog.cpp` into the DPI-C library that logs
- **One simlog per shared object**: `libsimlog.so` carries its own logger registry, so loading it
  *and* a DPI-C library that also contains simlog C++ code gives two independent loggers per name
- **No UVM adapter** (out of scope so far): `uvm_info` / `uvm_error` do not reach simlog by
  themselves; that would need a `uvm_report_server` subclass
- A logger created with `spdlog::get()` directly, bypassing `simlog::get()`, has no simulation-time
  stamp (expected)

## Build

The project uses xmake; spdlog (header-only), svdpi (header-only, the DPI-C `<svdpi.h>`, needed only
to compile `src/simlog.cpp`) and SystemC (shared library, SystemC backend only) come from xrepo.

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

There is one `simlog` target for both backends — `include/` and `src/` are its core code — and its
kind follows the backend: `headeronly` for SystemC and `shared` for SVDPI (the header plus
`src/simlog.cpp`, linked as `libsimlog.so`). Every target that uses the API depends on it and links
`libsimlog.so`; the DPI-C test libraries add an `$ORIGIN` rpath so the simulator finds the library
next to them. `src/simlog.cpp` is compiled exactly once, into the `simlog` target.

Requirements: C++17 and spdlog in both backends. The SVDPI backend additionally needs the header-only
`svdpi` package to build `src/simlog.cpp` and links `libsimlog.so` (or compiles that file in). The
SystemC backend needs SystemC. `xmake run example` (SVDPI configuration) and `xmake run vcs-test`
additionally need an installed VCS (`vcs` on `PATH`); both skip without it.

## Tests

The suite has four categories. Categories 1 and 2 run the same scenario bodies
([`tests/cases.cpp`](tests/cases.cpp)), so the API contract is checked on both backends from one
source; categories 3 and 4 exist because the SystemVerilog side is a separate surface.

| # | Category | Where | How |
|---|---|---|---|
| 1 | SystemC backend | [`tests/systemc/tests.cpp`](tests/systemc/tests.cpp) + [`tests/cases.cpp`](tests/cases.cpp) | `xmake test` — 15 scenarios, one process each |
| 2 | SVDPI, simlog used from C++ | [`tests/svdpi/cpp/`](tests/svdpi/cpp) + the same `cases.cpp` | `xmake run vcs-test` — the shared scenarios assert exact log lines inside C++, the bench only applies `#delay` |
| 3 | SVDPI, simlog used from SV | [`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv) | `xmake run vcs-test` — expected lines supplied to the runner and diffed |
| 4 | mixed C++ + SV, one logger | [`tests/svdpi/mixed/`](tests/svdpi/mixed) | `xmake run vcs-test` — diffed lines plus a same-entity check |

```bash
xmake f --backend=systemc -y && xmake test    # category 1: 15 scenarios
xmake test -v                                 # also show the output of passing tests
xmake run tests stamp                         # run a single scenario directly
xmake f --backend=svdpi -y   && xmake         # build libsimlog.so
xmake run example                             # SV example in VCS (skips without vcs)
xmake run vcs-test                            # categories 2-4 in a real simulator (VCS)
```

- **Category 1 — SystemC** (plain assertions, no framework). One process per scenario, because the
  library reads the environment once and SystemC accepts a time resolution only once per run. Only
  `resolution-100fs` and `resolution-10ns` are SystemC-only: SVDPI gets its resolution from the
  simulator's timescale at compile time.
- **Category 2 — SVDPI with simlog used from C++**. There is no host-side harness: DPI-C cannot
  advance simulation time, so `tb_cpp.sv` only applies the `#delay` that `simlog_test_step()` asks
  for, and the checks are the shared `cases.cpp` bodies. The runner supplies the expected time base
  (`+scale`/`+unit`) from the timescale it compiled for, so a wrong backend answer fails instead of
  agreeing with itself. The full shared suite runs at 1 ps; `format` and `stamp` also run at
  100 fs / 1 ns / 1 fs via `-override_timescale`.
- **Category 3 — SVDPI with simlog used from SystemVerilog**. The bench uses `simlog_logger` and
  never imports the C ABI: one logger per module, `$sformatf("%m")` as the name, one method per
  level, an explicit flush before `$finish`. The runner compares the exact log lines, including
  braces/percent signs surviving as message text and the invalid-name/handle notices.
- **Category 4 — mixed C++ and SV through one logger entity**. The mixed library links
  `libsimlog.so` like every other consumer and adds `$ORIGIN`; run.sh loads the mixed library only.
  So `simlog::get()` in C++ and `simlog_open()` behind `simlog_logger` resolve to the same logger
  (asserted by `mixed_same_entity()`), and the diffed lines show both sides interleaving on one stamp
  and obeying one `SIMLOG_LEVEL`.
- **Why no fake-host SVDPI harness**: its whole surface is the simulator — the DPI/VPI time
  functions, a loaded DPI-C library, the exported C ABI, the SystemVerilog package — so categories
  2-4 are verified where they run. That run is also what proves the VPI/PLI fallback on VCS
  O-2018.09-SP2, which has no `svGetTime` at all. Without `vcs` on `PATH` both `xmake run example`
  and `xmake run vcs-test` report a skip and exit 0, so on such a machine `nm -D libsimlog.so` is
  what can still be inspected.

### What each scenario covers

Categories 1 and 2 run the 13 shared bodies in [`tests/cases.cpp`](tests/cases.cpp) — plus the two
SystemC-only resolution scenarios:

- `format`, `stamp` — `%*` rendering (right-aligned 12-column stamp, pattern layout, flag cloning,
  stamp 0 during elaboration) and the `simlog::get()` contract (native logger, registration, shared
  default sink, per-call time, native level filtering)
- `resolution-100fs`, `resolution-10ns` — SystemC-only: the multiplier is folded into the number
  (`12340` ticks → `1234000 fs` / `123400 ns`)
- `env-default`, `env-warn`, `env-off`, `env-invalid` — the global level from `SIMLOG_LEVEL`
  (unset → `info`, bare name, `off`, invalid value → notice + `info`)
- `env-modules`, `env-modules-wildcard`, `env-modules-global`, `env-modules-order`,
  `env-modules-invalid` — `module=level` rules: verbatim comma-separated parsing, `*` matching and
  precedence, order independence, malformed entries reported and ignored
- `registry`, `parse-level` — a pre-registered logger is returned untouched (level, pattern, sinks);
  level-name and pattern parsing, process-wide default sink

Category 3 ([`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv)): `levels` (one method per level,
default `info` filters trace/debug, `$sformatf("%m")` names the module), `format` (braces and percent
signs survive as message text), `modules` (two loggers at one timestep share the stamp), `env`
(`SIMLOG_LEVEL=tb=warn` drives the SV-side logger), `empty-name` (notice, line dropped, next logger
still works).

Category 4 ([`tests/svdpi/mixed/`](tests/svdpi/mixed)): `order` and `sv-first` (either creation
order, same entity and stamp), `level` (one `SIMLOG_LEVEL` filters both sides).

## Project layout

```
include/simlog/simlog.h  # the API: backend selection / %* flag / levels / get()
                         # (the whole backend too in the SystemC configuration)
src/simlog.cpp           # the SVDPI backend + the SV side's extern "C" ABI → libsimlog.so
                         # (SVDPI configuration only — SystemC needs no source file)
sv/simlog_pkg.sv         # SystemVerilog side: simlog_logger (one object per logger)
examples/svdpi/tb.sv     # SV example: a pure-SystemVerilog bench using sv/simlog_pkg.sv
examples/svdpi/run.sh    # compiles + runs it in VCS (skips without vcs)
examples/systemc/tb.cpp  # SystemC integration example (sc_main)
tests/cases.h            # shared scenario contract: phase-stepped bodies + expected-stamp helpers
tests/cases.cpp          # categories 1+2: the 13 backend-agnostic scenarios
tests/systemc/tests.cpp  # category 1: SystemC host (sc_main) + the two resolution scenarios
tests/svdpi/cpp/         # category 2: SVDPI, simlog used from C++ (phase driver + #delay-only bench)
tests/svdpi/sv/          # category 3: SVDPI, simlog used through sv/simlog_pkg.sv
tests/svdpi/mixed/       # category 4: C++ + SV through one logger entity (one shared object)
tests/svdpi/run.sh       # categories 2-4: compile per timescale, run one scenario per process
tests/check.h            # assertion macros + stdout/stderr capture helpers
xmake.lua                # build script: backend option, one simlog target (kind per backend), tests
```
