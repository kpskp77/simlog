# simlog — simulation-time stamps for spdlog in EDA simulation

A tiny header-only library that makes [spdlog](https://github.com/gabime/spdlog) print
**simulation-time** stamps inside an EDA simulation.

- **No custom logger type**: you get a native `spdlog::logger`, so `add_sink` / `set_level` work as usual
- **Minimal on the user side**: one line to get started, everything else is plain spdlog API
- Built on spdlog's official extension point `custom_flag_formatter` (`%*` in the pattern is the simulation time)

## Quick start (SystemC)

```cpp
#include <simlog/simlog.h>

int sc_main(int, char**) {
    auto log = simlog::get("tb_top");      // first call: create + register + attach the simulation-time formatter
    log->info("reset released");           // from here on it is native spdlog API

    // Getting the same name again later returns the already registered instance
    // simlog::get("tb_top") == log
}
```

Output (resolution 1 ps):

```
[       12340 ps][tb_top][info] reset released
[       12340 ps][tb_top][warning] retry limit reached   ← same timestep, same stamp
[       12345 ps][tb_top][info] write complete, addr=0x1ff
```

Running [`examples/systemc.cpp`](examples/systemc.cpp) with the default 1 ps resolution gives:

```
[           0 ps][tb][warning] sim start, resolution: 1 ps
[       10000 ps][tb][info] cnt=1, wake at 10 ns
[       20000 ps][tb][info] cnt=2, wake at 20 ns
[       30000 ps][tb][info] cnt=3, wake at 30 ns
[       40000 ps][tb][info] cnt=4, wake at 40 ns
[       40000 ps][tb][error] sim end
```

## Time source

There is exactly one time source, SystemC, and **no initialization call at all**:

- Value: `sc_time_stamp().value()` (an integer tick count, 0 during elaboration) × the resolution multiplier
- Unit: the resolution's base unit, printed as a plain symbol only (`fs/ps/ns/us/ms/s`)

A resolution with a multiplier (e.g. 100 fs = 100 fs per tick) does not put the multiplier into
the unit; the multiplier is **folded into the tick count**, so the unit column is always a plain
symbol, all arithmetic is integral with zero floating-point error, and `sc_set_time_resolution()`
takes effect for this library immediately. There is no time-source injection, no manual time
advancement and no display-unit configuration.

```
resolution 1 ps   → 1 ps per tick   → [       12340 ps]   (= 12.34 ns)
resolution 100 fs → 100 fs per tick → [     1234000 fs]   (12340 tick × 100 fs = 1.234 ns)
resolution 10 ns  → 10 ns per tick  → [      123400 ns]   (12340 tick × 10 ns = 123.4 us)
```

## Configuration

There is no `init()`. A single environment variable, `SIMLOG_LEVEL`, carries both the global level
and the per-module overrides; its form is detected automatically and it is read once on the first
`simlog::get()` call:

| `SIMLOG_LEVEL` | Meaning |
|---|---|
| `info` | a bare level name (`trace/debug/info/warn/err/critical/off`) sets the global level |
| `*=debug,dma=info` | `module=level` rules, `,` separated, are applied per module |

Default: `info`. An invalid value never silently takes effect: the default is kept (or the entry is
ignored) and a one-line notice is written to stderr.

The environment is the **only run-time knob**; there is no `set_level()` API (that would create two
sources of truth — "half configured through the API, half through the environment"). Levels are
applied once at logger creation; to change the level of an existing logger, call the native
`logger->set_level()`.

The pattern is fixed to the built-in default `[%*][%n][%^%l%$] %v` (`%*` = simulation time) and
cannot be overridden through the environment.

### Per-module levels

```bash
SIMLOG_LEVEL="*=debug,dma=info"     # everything debug, except module "dma" at info
SIMLOG_LEVEL="info"                # simply the global level
```

- Each entry is `module=level`; the only separator is `,` and entries are taken verbatim, so an
  entry containing whitespace (`"*=debug, dma=info"`) is rejected with a stderr notice instead of
  silently becoming a pattern that can never match
- `module` is matched against the logger name passed to `simlog::get()`, case-sensitively
- `*` matches any sequence of characters (including none), so a lone `*` is the global rule and
  `dma*` / `*_tb` narrow it down
- The **most specific matching rule wins**: an exact name beats any wildcard, among wildcards the
  one with more literal characters wins, and ties go to the rule written later — so
  `*=debug,dma=info` and `dma=info,*=debug` behave identically
- Modules without a matching rule use the global level, and so does the module of a malformed entry
  (reported on stderr, then ignored)
- Only applied when `simlog::get()` creates the logger; an existing logger (including one the user
  registered) is never re-leveled

## Design

**There is exactly one public API**: `simlog::get()`. Everything else (SystemC time reading /
resolution multiplier and unit / `%*` flag / formatter / environment parsing / default sink) lives
in `simlog::detail`.

**Single-threaded model**: an EDA simulation runs only on the main thread, so the library has no
locks and no atomics — the registration path in `simlog::get()` has no mutex, and the default sink
is the `_st` single-threaded variant.

```
simlog::get(name)          ← lazy factory: create native logger + attach formatter + register
  └─ detail::sim_time_flag (%*)  ← custom_flag_formatter: reads sc_time_stamp × resolution multiplier
```

- **Tick-based**: stamp = `sc_time_stamp().value()` × resolution multiplier (1/10/100), integral
  arithmetic throughout, no floating-point error
- **Single header**: the whole implementation is in `simlog.h`, which includes `<systemc>`
  unconditionally

## Known limitations (phase 1)

- **Synchronous loggers only**: an async logger calls `format()` on a background thread, so the
  stamp would be the time of output rather than the time of the log call
- A logger created with `spdlog::get()` directly, bypassing `simlog::get()`, has no simulation-time
  stamp (expected)

## Build

The project uses xmake; spdlog (header-only) and SystemC (shared library) are fetched from xrepo.

```bash
xmake                 # header-only simlog target + the SystemC example
xmake run example     # run the example (xmake sets LD_LIBRARY_PATH for libsystemc)
xmake test            # build and run all test scenarios
xmake f -m debug -y   # switch configuration mode when needed
```

Requirements: C++17, spdlog, SystemC (both pulled in by `add_requires`); the SystemC include path
must be available wherever `simlog.h` is used.

## Tests

The tests use plain assertions (no test framework) and are registered with xmake's test action.
Each scenario runs in its own process, because the library reads the environment once and SystemC
accepts a time resolution only once per run.

```bash
xmake test              # build and run every scenario
xmake test -v           # also show the output of passing tests
xmake run tests stamp   # run a single scenario directly
```

| Scenario | Covers |
|---|---|
| `format` | `%*` flag rendering: right-aligned 12-column stamp, pattern layout, flag cloning, stamp 0 during elaboration |
| `stamp` | `simlog::get()` contract (native logger, registration, repeated calls), shared default sink, per-call simulation time, native level filtering |
| `resolution-100fs` | resolution multiplier folded into the number (`12340` ticks → `1234000 fs`) |
| `resolution-10ns` | resolution multiplier folded into the number (`12340` ticks → `123400 ns`) |
| `env-default` | default level `info` when `SIMLOG_LEVEL` is unset |
| `env-warn` | a bare `SIMLOG_LEVEL=warn` sets the global level at logger creation |
| `env-off` | `SIMLOG_LEVEL=off` disables all output |
| `env-invalid` | an invalid `SIMLOG_LEVEL` keeps `info` and writes one stderr notice |
| `env-modules` | `module=level` entries (verbatim, comma-separated, empty entries skipped), unmatched modules using the built-in global level, rule actually driving output |
| `env-modules-wildcard` | `*` matching, exact-beats-wildcard and wildcard-specificity precedence |
| `env-modules-global` | the documented `"*=debug,dma=info"` form |
| `env-modules-order` | the same rules written in the opposite order give the same levels |
| `env-modules-invalid` | malformed entries (bad shape, embedded whitespace, unknown level) reported and ignored, valid entries still applied |
| `registry` | a logger registered beforehand is returned untouched (level, pattern and sinks kept) |
| `parse-level` | level-name parsing incl. case sensitivity and unknown names, `*` pattern matching, process-wide default sink |

## Project layout

```
include/simlog/
└── simlog.h          # single header: config / time source / %* flag / get()
examples/systemc.cpp  # SystemC integration example
tests/tests.cpp       # test scenarios (one process per scenario)
tests/check.h         # assertion macros + stdout/stderr capture helpers
xmake.lua             # build script: header-only target, example, tests
```
