# AGENTS.md

For AI coding assistants working in this repository: the build/verification commands and the rules
every change must preserve. Rationale lives in [README.md](README.md) (positioning, time source,
`SIMLOG_LEVEL` semantics, known limitations) and in the section-header comments of
`include/simlog/simlog.h`, which are authoritative for *why* each decision was made.

## The project in one sentence

simlog is a single-header spdlog extension that stamps log lines with **SystemC simulation time**
(`%*`) — no custom logger type, no `init()` call, no second time source.

## Build and verify (mandatory after changes)

**Principle: after every change run the whole suite — no cherry-picking, no skipping.**

```bash
xmake                 # header-only simlog target + the SystemC example
xmake run example     # smoke: prints the documented 6-line output (0/10k/20k/30k/40k/40k ps)
xmake test            # build and run all 15 scenarios (one process each)
xmake test -v         # also show stdout of passing tests
xmake run tests stamp # run one scenario directly
```

- The library is a **single header**: any change means retesting everything, not just the scenario
  named after the file you touched.
- `xmake` runs with `-Wall -Wextra -Werror` (`set_warnings("all","extra","error")` on every target),
  so a warning is a build failure — never silence one to get green.
- spdlog (header-only) and SystemC (shared) come from xrepo via `add_requires`; `xmake run` is what
  injects the SystemC `LD_LIBRARY_PATH`, so **never run `build/.../example` or `build/.../tests`
  directly** (libsystemc.so will not be found).
- A harmless `warning: Ascend SDK not found` appears on this machine; ignore it.
- If SystemC's own package build is missing, `xmake` fetches it — the first build is slow, later
  builds are seconds.

## Layout

```
include/simlog/simlog.h   the whole library: env config / SystemC time / %* flag / get()
examples/systemc.cpp      minimal testbench example (the README output comes from here)
tests/tests.cpp           all scenarios; argv[1] selects one, dispatched from sc_main
tests/check.h             CHECK / CHECK_EQ, stream capture, env helpers (no test framework)
xmake.lua                 header-only target, example, and one add_tests() entry per scenario
```

Inside `simlog.h` the four numbered sections are the map: 1 config (env), 2 SystemC time source,
3 `%*` flag/formatter/default sink, 4 `get()`. Keep new code in the matching section;
`simlog::detail` holds everything except `get()`.

## Invariants (check each one when changing `simlog.h`)

- **Exactly one public API: `simlog::get(name)`.** Everything else stays under `simlog::detail`. Do
  not add a second entry point, a builder type, or an `init()`.
- **`get()` returns a native `spdlog::logger`** created with `std::make_shared<spdlog::logger>` and
  registered via `spdlog::register_logger`; the user must keep `add_sink` / `set_level` unchanged.
  An already-registered logger of the same name (including one the user created) is returned
  **untouched** — never re-level, re-pattern or re-sink it.
- **Time is read inside the flag's `format()`**, never cached per logger and never ahead of the
  call: that is what makes the stamp the time *of the log call* and makes
  `sc_set_time_resolution()` take effect immediately, without an ordering constraint.
- **There is exactly one time source: SystemC.** `sc_time_stamp().value()` × resolution multiplier,
  integral arithmetic only. No timestamp injection, no manual clock, no display-unit configuration.
- **The resolution multiplier is folded into the number, never into the unit**: at 100 fs
  resolution, 12340 ticks print `1234000 fs`, not `12340 100fs`. The unit column always holds a
  plain `fs/ps/ns/us/ms/s` symbol taken from `sc_time_tuple::unit_symbol()`.
- **Stamp field is fixed width, right-aligned in 12 columns**, `"{:>12} {}"`. Tests compare full
  lines, so any width/padding change is a deliberate, test-updating change.
- **The pattern is fixed**: `[%*][%n][%^%l%$] %v`, `%*` registered through spdlog's official
  extension point `custom_flag_formatter` (`add_flag<sim_time_flag>('*')`). The pattern is not
  configurable from the environment, and `%*` must stay a non-alphabetic character so it cannot
  collide with a built-in flag.
- **The environment is the only run-time level knob.** `SIMLOG_LEVEL` carries both the bare global
  level and `module=level` rules; it is read **once, lazily, on the first `get()`** (function-local
  static) and never re-read. There is no `set_level()`-style API on purpose (two sources of truth);
  changing an existing logger's level is native `logger->set_level()`.
- **Invalid configuration is reported, never silently applied**: a one-line notice to stderr via
  `std::fprintf` (no allocation through the logging path) and the default / global level is kept.
  `spdlog::level::from_str()` stays unused (it maps unknown names to `off`, silently disabling
  logging).
- **Rule precedence is defined and order-independent**: exact name > wildcard, then more literal
  characters, ties to the rule written later.
- **Single-threaded model**: no locks, no atomics; the default sink is the `_st` variant and is one
  process-wide shared instance (`detail::default_sink()`). Any change that needs a mutex is a design
  change, not a fix.
- **Synchronous loggers only** (documented limitation): an async logger formats on a background
  thread and the stamp would be the output time. Do not "fix" it by reading time at enqueue time.
- **Header-only, C++17 floor**: everything is `inline` functions/variables so any translation unit
  can include it without ODR problems; the `<systemc>` include is unconditional. `#error` on
  pre-C++17 stays.
- **`get()` takes `const std::string&`**, not a `string_view` — the spdlog registry only accepts
  `std::string`, and a view would force an allocation on the registered-logger fast path.

## Where new content goes

| Content | Location |
|---|---|
| New environment knob / rule semantics | `simlog.h` section 1 (`parse_*`, `level_for`) |
| Time formatting / resolution handling | `simlog.h` sections 2–3 (`now_ticks`, `resolution_unit`, `sim_time_flag`) |
| New scenario | `tests/tests.cpp` (`<name>_scenario()` + dispatch in `sc_main`) **and** `add_tests()` in `xmake.lua` |
| New assertion helper | `tests/check.h` (`namespace test`) |
| Example change | `examples/systemc.cpp` — and update the sample output in `README.md` |

Anything that needs a new file in `include/simlog/` is a scope decision: update `README.md`
(Project layout), `xmake.lua` (`add_headerfiles`) and this file in the same change.

## Test conventions

- **One process per scenario, on purpose**: `SIMLOG_LEVEL` is read once and SystemC accepts a time
  resolution once per run, so scenarios must not share a process. A new scenario therefore must be
  both dispatched in `sc_main` **and** registered with `add_tests()` in `xmake.lua`; registering
  only one of the two silently loses coverage.
- Scenarios whose name does not start with `env-` get `SIMLOG_LEVEL` unset by `sc_main` before they
  run; an `env-*` scenario manages the variable itself. Set the env var **before** the first
  `simlog::get()` in that process.
- Use `test::capture` + `test::log_lines()` to assert on exact output lines: `log_lines()` keeps
  only lines starting with `[` and strips ANSI SGR, because SystemC prints its banner on the same
  stream.
  Flush the logger before `stop()`.
- `CHECK` / `CHECK_EQ` only; a scenario exits non-zero when any check failed. No test framework, no
  `run_case`, no dynamic registration.
- Every scenario must be deterministic: no wall-clock, no thread ordering, no reliance on sink
  interleaving. Time advances only through `sc_start()`.

## Code style (mandatory before committing)

```bash
clang-format -i include/simlog/simlog.h examples/systemc.cpp tests/tests.cpp tests/check.h
stylua xmake.lua
```

- `.clang-format` (Google base, 2-space indent, column limit 100, attached braces, left pointers)
  and `.stylua.toml` (100 columns, 2 spaces, double quotes) are authoritative. Only format files you
  changed — no whole-repository reshuffles.
- `.verible-format` is inherited from the sibling project; this repository has **no** `.sv` files,
  so verible is not part of the workflow here.
- Comments carry the design rationale in this project (why a rule exists, what breaks otherwise).
  A non-obvious rule added in code belongs in a comment at least as much as in this file — keep the
  section banners and the per-function "why" comments intact when editing nearby code.

## Pitfalls in this environment (already hit)

- **`sc_main` must stay exported** even under `-fvisibility=hidden`: `examples/systemc.cpp` uses
  `SIMLOG_EXAMPLE_EXPORT` and `tests/tests.cpp` uses `SIMLOG_TEST_EXPORT`
  (`__attribute__((visibility("default")))` / `__declspec(dllexport)`). Dropping it links but never
  runs.
- **`xmake run` is mandatory** for anything linking SystemC (no rpath from `add_packages`).
- This xmake version: `add_requires` may only appear at **top-level scope** (never after
  `option`/`target` blocks). Keep the two `add_requires` lines where they are.
- The `tests` target has `set_default(false)`: `xmake` alone does **not** build it — `xmake test`
  does. Do not "fix" the default to `true`.
- `build/` and `.xmake/` are git-ignored build artifacts; never commit or hand-edit them, and never
  edit generated files to make a test pass.
- SystemC rejects resolutions other than 1/10/100 × unit (E514), which is why
  `resolution_unit().scale ≤ 100` and the tests only cover 1 ps / 100 fs / 10 ns.
- Tick × multiplier uses `__int128` when available (`__SIZEOF_INT128__`): 64-bit overflow starts
  around 1.8e17 ticks at scale 100. Do not replace `wide_ticks` with a plain `std::uint64_t`.
- During elaboration `sc_time_stamp()` is 0 and that is not an error — the stamp prints
  `[           0 ps]`; tests assert this.
