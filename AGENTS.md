# AGENTS.md

Guidance for AI coding assistants in this repository: the verification commands and the rules every
change must preserve. Rationale lives in [README.md](README.md) and [README_zh.md](README_zh.md) (the
Chinese translation, which must be updated in the same change) and in the section-header comments of
`include/simlog/simlog.h` (authoritative for *why*).

## The project in one sentence

simlog is a spdlog extension that stamps log lines with **simulation time** (`%*`) — no custom logger
type, no `init()`, no second time source — read from exactly one compile-time backend: **SVDPI**
(default, DPI-C time API, compiled into `libsimlog.so`) or **SystemC** (header-only).

## Build and verify (mandatory after changes)

Run the whole suite — the backend is a compile-time choice, so that means *both* configurations:

```bash
xmake f --backend=svdpi -y     # default backend
xmake                          # libsimlog.so (the `simlog` target: include/ + src/)
xmake run example              # examples/svdpi/tb.sv in VCS, diffed against 6 documented lines;
                               # skips itself when vcs is absent
xmake run vcs-test             # categories 2-4: the real simulator; skips itself when vcs is absent

xmake f --backend=systemc -y   # SystemC backend
xmake                          # the header-only `simlog` target
xmake run example              # 6 documented lines (0/10k/20k/30k/40k/40k ps)
xmake test                     # category 1: all 15 scenarios (one process each)

xmake test -v                  # also show stdout of passing tests
xmake run tests stamp          # run one scenario directly
```

- **One `simlog` target whose kind follows the backend**: `headeronly` for SystemC (the time source
  is inline in the header), `shared` for SVDPI (`include/` is the API, `src/simlog.cpp` is the
  backend + the SV-side C ABI → `libsimlog.so`). `src/simlog.cpp` is compiled exactly once, there;
  never into a test or example target. Dependents link `libsimlog.so`; the DPI-C ones add an
  `$ORIGIN` rpath so the simulator finds it.
- **The split is about the time source, not the build kind**: `simlog.h` holds the API and all
  shared code, `src/simlog.cpp` the SVDPI backend and the SV-side C ABI. Verify both backends:
  `xmake test` covers SystemC only, the SVDPI backend is verified by `xmake run vcs-test`. Without a
  simulator, the SVDPI check is `nm -D libsimlog.so` (five exported functions, four weak-undefined
  time symbols) — say so when reporting.
- `xmake` runs with `-Wall -Wextra -Werror` on every target; a warning is a build failure, never
  silence one to get green.
- spdlog (header-only), svdpi (header-only `<svdpi.h>`) and SystemC (shared) come from xrepo via
  `add_requires`. `add_packages` is per target: `simlog` takes `systemc` publicly only in the
  SystemC branch, and `svdpi` privately (only `src/simlog.cpp` includes it) only in the SVDPI one.
- `xmake run` is what injects the SystemC `LD_LIBRARY_PATH`; **never run the SystemC
  `build/.../example` or `build/.../tests` binaries directly**.
- A harmless `warning: Ascend SDK not found` appears on this machine; ignore it. The first SystemC
  build is slow, later builds are seconds.

## Layout

```
include/simlog/simlog.h   header-only API: backend selection / env config / %* flag / levels / get()
                          (the SystemC backend's time source is in here too; SVDPI's is not)
src/simlog.cpp            the SVDPI backend + the SV side's extern "C" ABI (declarations and
                          definitions in one file, no header), built as libsimlog.so
sv/simlog_pkg.sv          SystemVerilog side: simlog_logger (one object per logger) over that C ABI
examples/svdpi/tb.sv      pure-SystemVerilog example: two modules, each `simlog_logger` created
                          with `$sformatf("%m")`; run by examples/svdpi/run.sh (VCS; output checked
                          against the documented lines, skipped without a simulator)
examples/systemc/tb.cpp   minimal SystemC testbench example (the README output comes from here)
tests/cases.h             shared scenario contract: phase-stepped bodies, expected-stamp helpers
                          (no SystemC, no simulator, no time advancement)
tests/cases.cpp           the 13 backend-agnostic scenarios (format / stamp / env-* / registry /
                          parse-level), compiled into both the SystemC suite and the SVDPI
                          category-2 library
tests/systemc/tests.cpp   category 1: SystemC host (sc_main), sc_time between phases, the two
                          resolution scenarios SVDPI cannot run (100 fs / 10 ns)
tests/svdpi/cpp/          category 2: SVDPI with simlog used from C++ (core_dpi.cpp exports the
                          phase driver, tb_cpp.sv only applies #delay)
tests/svdpi/sv/           category 3: SVDPI with simlog used through sv/simlog_pkg.sv (proxy)
tests/svdpi/mixed/        category 4: C++ + SV through one logger entity (one shared object)
tests/svdpi/run.sh        categories 2-4: compile per timescale, run one scenario per process
                          (category 2 asserts in C++; 3 and 4 diff exact lines)
tests/check.h             CHECK / CHECK_EQ, stream capture, env helpers (no test framework)
xmake.lua                 backend option + one simlog target (headeronly / shared per backend);
                          svdpi: example + categories 2-4; systemc: example + category 1
```

Inside `simlog.h` the four numbered sections are the map: 1 config (env), 2 the backend-selected
time source, 3 `%*` flag/formatter/default sink, 4 `get()`. Keep new code in the matching section;
`simlog::detail` holds everything except `get()`.

## Invariants

- **Exactly one public C++ API: `simlog::get(name)`.** Everything else stays in `simlog::detail`. No
  builder type, no second high-level entry point, no `init()`. The C ABI of `libsimlog.so` is exactly
  five functions: the two time accessors declared in `simlog.h` (`simlog_sv_now_ticks` /
  `simlog_sv_resolution_unit`) plus the three SV-facing bridge functions (`simlog_open` /
  `simlog_log` / `simlog_flush`), which exist because SystemVerilog cannot call a C++ object.
- **The bridge is a facade**: obtain the logger through `simlog::get()` (never create or reconfigure
  one), log through the native logger with the message as a `"{}"` argument (never as the format
  string), take **no timestamp** from the SV side, and report a broken argument (empty name, null
  handle, level outside 0 … 5, null message) once on stderr while dropping the line.
- **`include/simlog/` keeps exactly one file.** The SVDPI backend is one file (`src/simlog.cpp`) with
  declarations first and definitions after, so no new header is needed anywhere.
- **One shared object is one library state** (registry, default sink, `SIMLOG_LEVEL` snapshot): load
  `libsimlog.so` or compile `src/simlog.cpp` into the DPI-C library, never both — that would give
  two loggers per name.
- **`sv/simlog_pkg.sv` adds no semantics**: no level filtering, no timestamps, no name guessing. The
  name is explicit (`new($sformatf("%m"))` is the module-level idiom) and `svGetScope()` is not used
  at all.
- **`get()` returns a native `spdlog::logger`** (`std::make_shared` + `spdlog::register_logger`); the
  user keeps `add_sink` / `set_level`. An already-registered logger of the same name is returned
  **untouched** — never re-level, re-pattern or re-sink it.
- **Time is read inside the flag's `format()`**, never cached and never ahead of the call: that is
  what makes the stamp the time *of the log call*, and (SystemC) makes `sc_set_time_resolution()` take
  effect immediately.
- **Exactly one backend is compiled in**, `SIMLOG_BACKEND_SVDPI` by default. The macros are the only
  switch — never a run-time switch, never both at once — and the header must keep working with **no**
  define at all.
- **The backends meet the rest of the library at exactly two functions**: `now_ticks()` and
  `resolution_unit()` (`{scale, symbol}`). The `%*` flag, the pattern, the levels, the default sink
  and `get()` stay backend-agnostic; new time-reading behaviour belongs in those two functions.
- **Each backend has exactly one time source per process.** SVDPI: `svGetTime(NULL, …)` +
  `svGetTimePrecision(NULL, …)` (`sv_sim_time`, never `sv_scaled_real_time`) when the simulator
  implements IEEE 1800-2023, otherwise `vpi_get_time(NULL, …)` + `tf_gettimeprecision()`, chosen once
  per process by which symbols the host exports; the two paths are never mixed. SystemC:
  `sc_time_stamp().value()` × `sc_get_time_resolution()`.
- **The resolution/precision multiplier is folded into the number, never into the unit**: at 100 fs
  resolution 12340 ticks print `1234000 fs`; a DPI precision of 10 ps prints `{10, "ps"}`. The unit
  column always holds a plain `fs/ps/ns/us/ms/s` symbol, and `scale ≤ 100`.
- **Stamp field is fixed width, right-aligned in 12 columns**, `"{:>12} {}"`. Tests compare full
  lines, so any width change is a deliberate, test-updating change.
- **The pattern is fixed**: `[%*][%n][%^%l%$] %v`, `%*` registered through `custom_flag_formatter`
  (`add_flag<sim_time_flag>('*')`). Not configurable from the environment; `%*` must stay
  non-alphabetic so it cannot collide with a built-in flag.
- **The environment is the only run-time level knob.** `SIMLOG_LEVEL` carries the bare global level
  and `module=level` rules; it is read **once, lazily, on the first `get()`** and never re-read.
  There is no `set_level()`-style API; changing an existing logger's level is native
  `logger->set_level()`. Precedence is defined and order-independent: exact name > wildcard, then
  more literal characters, ties to the rule written later.
- **Invalid configuration is reported, never silently applied**: one line to stderr via `std::fprintf`,
  keeping the default/global level. `spdlog::level::from_str()` stays unused (unknown names map to
  `off`, silently disabling logging). The DPI backend follows the same rule: zero stamp **plus** one
  stderr notice per process.
- **Single-threaded model**: no locks, no atomics; the default sink is the `_st` variant and is one
  process-wide shared instance. Any change that needs a mutex is a design change.
- **Synchronous loggers only** (documented limitation): an async logger formats on a background
  thread and the stamp would be the output time.
- **C++17 floor.** Everything in `simlog.h` is `inline` so any translation unit can include it, and
  `simlog.h` must never include `<svdpi.h>` or declare a simulator symbol — that keeps
  `<vpi_user.h>` / `<veriuser.h>` usable next to it in any order.
- **The SVDPI backend asks the simulator for its time, never for a timestamp.** `<svdpi.h>` comes from
  the header-only `svdpi` xrepo package; only `src/simlog.cpp` includes it. `svGetTime` /
  `svGetTimePrecision` (IEEE 1800-2023) are declared **weak**, paired with a weak VPI/PLI fallback
  (`vpi_get_time` + `tf_gettimeprecision`) — a hard reference makes the library unlinkable on every
  pre-2023 simulator (VCS O-2018.09 exports neither). Keep the fallback a plain value query: no VPI
  handle, no `vpi_iterate`/`vpi_scan`, no callback, no registration, no simulator header include
  (the four signatures are declared in `simlog.cpp`; `svTimeVal` *is* `s_vpi_time`).
- **`get()` takes `const std::string&`**: spdlog's registry only accepts `std::string`, and a view
  would force an allocation on the registered-logger fast path.

## Where new content goes

| Content | Location |
|---|---|
| New environment knob / rule semantics | `simlog.h` section 1 (`parse_*`, `level_for`) |
| New time formatting / resolution handling (shared) | `simlog.h` section 3 (`sim_time_flag`) |
| Time reading of the SystemC backend | `simlog.h` section 2, inside the SystemC `#if` branch only |
| Time reading of the SVDPI backend | `src/simlog.cpp` — `simlog.h` keeps only the two `extern "C"` declarations |
| The SV-side C ABI | `src/simlog.cpp` (no header: the target that compiles it carries its own declaration — `tests/svdpi/mixed/mixed.cpp` does) |
| A third backend | `simlog.h` backend block + a new branch in section 2 (two functions!), the `backend` option in `xmake.lua`, a test suite, README/AGENTS — treat it as a scope decision |
| New scenario | **Shared (preferred):** a phase-stepped function in `tests/cases.cpp` + the `shared()` list, then the name in the `add_tests()` list in `xmake.lua` **and** in `CPP_SCENARIOS` in `tests/svdpi/run.sh`. **SystemC-only:** `tests/systemc/tests.cpp` + `add_tests()` |
| A real-simulator C++ case | a new shared scenario, or a dedicated phase/export in `tests/svdpi/cpp/core_dpi.cpp` + a plusarg arm in `tb_cpp.sv` + a run in `run.sh` |
| A new SV-side helper | `sv/simlog_pkg.sv` — and, only if new behaviour is needed, a function in `src/simlog.cpp`; never a time source or a level policy in SystemVerilog |
| SystemVerilog-side behaviour (category 3) | a task + `case` arm in `tests/svdpi/sv/proxy.sv` and one `check_sv` block in `tests/svdpi/run.sh` |
| Mixed C++ + SV behaviour (category 4) | a `case` arm in `tests/svdpi/mixed/tb_mixed.sv`, an export in `tests/svdpi/mixed/mixed.cpp`, one `check_mixed` block in `run.sh` |
| New assertion helper | `tests/check.h` (`namespace test`) |
| Example change | `examples/systemc/tb.cpp` / `examples/svdpi/tb.sv` (+ the expected lines in `examples/svdpi/run.sh`) — and the sample output in `README.md` and `README_zh.md` |

Anything that needs a new file in `include/simlog/` is a scope decision: update `README.md`,
`xmake.lua` (`add_headerfiles`) and this file in the same change.

## Test conventions

- **The unit suite is SystemC-only.** `tests/systemc/tests.cpp` is the SystemC host (`sc_main`,
  15 scenarios) and the only target with `add_tests()`. The bodies live in `tests/cases.cpp` and are
  shared with the SVDPI category-2 library, which keeps both backends honest about the same API
  contract. Only the two resolution scenarios are SystemC-only. Do not add a fake-host harness for
  SVDPI: it would have to imitate the DPI/VPI API, which a simulator already provides.
- **One process per scenario, on purpose**: `SIMLOG_LEVEL` is read once and SystemC accepts a time
  resolution once per run. A new shared scenario must be registered in `xmake.lua` (`add_tests()`)
  **and** in `tests/svdpi/run.sh` (`CPP_SCENARIOS`); registering only one silently loses coverage.
- **The four categories are fixed**: (1) SystemC backend; (2) SVDPI with simlog used from C++;
  (3) SVDPI through `sv/simlog_pkg.sv`; (4) mixed C++ + SV through one logger entity. Keep new
  coverage in the category whose surface it exercises instead of creating a fifth harness.
- Scenarios whose name does not start with `env-` get `SIMLOG_LEVEL` unset before they run; an
  `env-*` scenario manages the variable itself, before the first `simlog::get()` in that process.
- Use `test::capture` + `test::log_lines()` to assert on exact output lines (`log_lines()` keeps only
  lines starting with `[` and strips ANSI SGR, because SystemC prints its banner on the same stream).
  Flush the logger before `stop()`.
- `CHECK` / `CHECK_EQ` only; a scenario exits non-zero when any check failed. No test framework.
- Every scenario must be deterministic: no wall-clock, no thread ordering, no reliance on sink
  interleaving. Time advances only through the host — `sc_start()` in the SystemC suite, the bench's
  `#delay` in category 2 — never inside a scenario body.
- **The SVDPI side has one harness: the simulator.** `tests/svdpi/run.sh` is where coverage is added.
  Category 2 runs the shared scenarios through `core_dpi.cpp` + `tb_cpp.sv` and asserts in C++;
  categories 3 and 4 pass expected lines on stdin to `check_sv` / `check_mixed` and diff the exact
  log lines. Category 2 runs the full shared suite at 1 ps and `format`/`stamp` at 100 fs / 1 ns /
  1 fs; the runner passes `+scale`/`+unit` so expected stamps stay backend-independent.

## Code style (mandatory before committing)

```bash
clang-format -i <changed .h/.cpp>
stylua xmake.lua
```

- `.clang-format` (Google base, 2-space indent, column limit 100) and `.stylua.toml` (100 columns, 2
  spaces, double quotes) are authoritative. Format only files you changed.
- `.verible-format` (100 columns, 2 spaces) is the style for the `.sv` files, but verible is not
  installed and is not part of the verification loop: format those by hand in the surrounding style.
- Comments carry the design rationale in this project. A non-obvious rule added in code belongs in a
  comment at least as much as in this file; keep the section banners and per-function "why" comments
  when editing nearby code.
- `SIMLOG_BACKEND_*` is defined **once**, on the `simlog` target in `xmake.lua` (public
  `add_defines`), so every dependent target inherits exactly one backend. Never add a backend
  `add_defines` to an individual target, and never "fix" a mis-selected backend with `-U`.

## Pitfalls in this environment (already hit)

- **`sc_main` must stay exported**: `examples/systemc/tb.cpp` uses `SIMLOG_EXAMPLE_EXPORT` and
  `tests/systemc/tests.cpp` uses `SIMLOG_TEST_EXPORT`, so they still run if a build ever uses hidden
  default visibility. Dropping it links but never runs.
- **`xmake run` is mandatory** for anything linking SystemC (no rpath from `add_packages`).
- **The SVDPI time symbols are weak on purpose**: `svGetTime` / `svGetTimePrecision` and
  `vpi_get_time` / `tf_gettimeprecision` are `__attribute__((weak))` (other compilers get strong
  references and no fallback). Never turn them back into hard references: VCS O-2018.09 exports none
  of them, so the link fails. Detection is the pointer being non-null at run time — no env-var or
  compile-time switch.
- **`libsimlog.so` must keep its five entry points exported and its time symbols unresolved.**
  `SIMLOG_EXPORT` (default visibility) is what makes `nm -D` list `simlog_open` / `simlog_log` /
  `simlog_flush` / `simlog_sv_now_ticks` / `simlog_sv_resolution_unit`. The header's inline functions
  (notably `simlog::get`) stay weak-exported on purpose: category 4's library links `libsimlog.so`
  and shares its spdlog registry by interposition, so hiding them breaks that test. The four
  simulator time functions must stay **weak undefined** (`w` in `nm -D`): never link a simulator
  library, never add `--no-undefined`. **C linkage is name-only**, so the names in `src/simlog.cpp`
  and `sv/simlog_pkg.sv` must be kept in step by hand — a renamed function still links and corrupts
  the arguments at run time.
- **A shared library cannot see an executable's symbols unless they are exported**: `libsimlog.so`
  resolves the four time functions in the global scope, so the *host* has to export them (a simulator
  does). If you link the library into a host binary, that binary needs default visibility and
  `-rdynamic` — otherwise every stamp silently becomes `0 s` plus a notice.
- **SystemVerilog has no variadic functions, no variadic macros and no method overloading**: that is
  why `simlog_logger` takes a finished string and the call site uses `$sformatf`. Do not pass a format
  plus arguments through DPI, and do not move `$sformatf` into C++.
- **Mixing `<vpi_user.h>` with `<svdpi.h>`**: VCS's `vpi_user.h` defines `t_vpi_time` without the
  `VPI_TIME` guard the DPI header checks, so including both in one translation unit redefines the
  struct. Confined to `src/simlog.cpp`; do not put `<svdpi.h>` back into the header.
- **VCS specifics (O-2018.09-SP2)**: it has no `svGetTime`, so the real-simulator run exercises the
  VPI/PLI path; `tf_gettimeprecision()` reports the global precision even from a module with a
  coarser timescale. When VCS compiles C++ sources itself, `-cc gcc -cpp g++` is required.
  `-override_timescale` overrides every module, including one that declares its own timescale —
  category 2 relies on that, and the expected tick counts assume the overriding timescale.
- **`%m` is scope-sensitive in VCS**: `simlog_logger lg = new($sformatf("%m"));` as a declaration
  initializer in a module's `initial` block yields the module name, but a later assignment yields
  `tb.unnamed$$_0`, and inside a task/function it yields the task scope. `tests/svdpi/sv/proxy.sv`
  therefore creates the levels logger with the declaration initializer and passes it into the task.
- **Switching the backend needs a reconfigure** (`xmake f --backend=… -y`), and the two
  configurations do not share a test entry point (`xmake test` is SystemC only, `xmake run vcs-test`
  is SVDPI only), so a green run of one says nothing about the other.
- The `svdpi` package lives in the local `myrepo` xmake repository and must stay a **top-level**
  `add_requires("svdpi")` (this xmake version rejects it after `option`/`target` blocks). Do not
  replace it with a simulator include path.
- The `tests` target has `set_default(false)`: `xmake` alone does not build it — `xmake test` does.
- `build/` and `.xmake/` are git-ignored; never commit or hand-edit them, and never edit generated
  files to make a test pass.
- SystemC rejects resolutions other than 1/10/100 × unit (E514), which is why `scale ≤ 100` and the
  SystemC tests only cover 1 ps / 100 fs / 10 ns.
- Tick × multiplier uses `__int128` when available (`__SIZEOF_INT128__`): 64-bit overflow starts
  around 1.8e17 ticks at scale 100. Do not replace `wide_ticks` with a plain `std::uint64_t`.
- During SystemC elaboration `sc_time_stamp()` is 0 and that is not an error — the stamp prints
  `[           0 ps]`. The DPI analogue is a host that returns 0 ticks (or cannot answer at all:
  stamp 0 + one stderr notice).
