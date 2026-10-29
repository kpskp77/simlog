set_project("simlog")
set_version("0.1.0")
set_languages("c++17")

add_rules("mode.debug", "mode.release")

-- Top-level on purpose: add_requires is not allowed after option/target blocks here.
-- Shared on purpose: the spdlog logger registry must live in one place per
-- process (libspdlog.so), not one copy per shared object. header_only must be
-- false or the package silently stays header-only and every DSO keeps its own
-- registry again.
add_requires("spdlog", { configs = { header_only = false, shared = true } })
add_requires("systemc", { configs = { shared = true } })
add_requires("svdpi")

-- The only backend switch: it picks the define on `simlog`, never a run-time option.
option("backend", function()
  set_default("svdpi")
  set_values("svdpi", "systemc")
  set_description("simulation-time backend: svdpi (DPI-C, default) or systemc")
  set_showmenu(true)
end)

-- One target, one backend: header-only for SystemC, libsimlog.so for SVDPI. src/simlog.cpp
-- is compiled exactly once, here.
target("simlog", function()
  set_warnings("all", "extra", "error")
  add_headerfiles("include/(simlog/*.h)")
  add_includedirs("include", { public = true })
  add_packages("spdlog", { public = true })
  if get_config("backend") == "systemc" then
    set_kind("headeronly")
    add_defines("SIMLOG_BACKEND_SYSTEMC", { public = true })
    add_packages("systemc", { public = true })
  else
    -- Always shared, never a static archive and never get_config("kind"): the
    -- built-in kind config defaults to static, and DPI-C must load this as a
    -- shared object — a static archive would duplicate the bridge and its
    -- state into every consumer (two loggers per name again).
    set_kind("shared")
    add_defines("SIMLOG_BACKEND_SVDPI", { public = true })
    add_files("src/simlog.cpp")
    add_packages("svdpi")
  end
end)

target("example", function()
  set_default(false)
  add_deps("simlog")
  if get_config("backend") == "systemc" then
    set_kind("binary")
    set_warnings("all", "extra", "error")
    add_files("examples/systemc/tb.cpp")
  else
    -- A pure SystemVerilog testbench (examples/svdpi/tb.sv): the example has no
    -- C++ of its own, so there is nothing to compile into a binary. The target
    -- only carries the dependency on libsimlog.so and hands its path to the
    -- runner, which compiles and runs the bench in VCS and skips itself when
    -- there is no simulator — the same contract as `vcs-test`.
    set_kind("phony")
    on_run(function(target)
      os.execv("bash", {
        path.join(os.projectdir(), "examples/svdpi/run.sh"),
        target:dep("simlog"):targetfile(),
      })
    end)
  end
end)

-- ── Tests: four categories ───────────────────────────────────────────
-- 1. SystemC backend          tests/systemc/  (`xmake test`)
-- 2. SVDPI, C++ side          tests/svdpi/cpp + tests/cases.cpp (same scenario
--                              bodies as category 1)
-- 3. SVDPI, SV proxy class    tests/svdpi/sv   (sv/simlog_pkg.sv)
-- 4. mixed C++ + SV            tests/svdpi/mixed (one library state)
--
-- One process per scenario everywhere: the library reads SIMLOG_LEVEL once and
-- SystemC accepts a time resolution once per run, so scenarios must not share a
-- process. Category 1 is a unit binary; categories 2-4 run in a real simulator.
if get_config("backend") == "systemc" then
  target("tests", function()
    set_kind("binary")
    set_default(false)
    set_warnings("all", "extra", "error")
    add_deps("simlog")
    -- Category 1: the SystemC host (sc_main) + the scenario bodies shared with
    -- the SVDPI C++ suite (category 2, tests/svdpi/cpp).
    add_files("tests/systemc/tests.cpp", "tests/cases.cpp")
    add_includedirs("tests")
    add_tests("format", { runargs = "format" })
    add_tests("stamp", { runargs = "stamp" })
    add_tests("resolution-100fs", { runargs = "resolution-100fs" })
    add_tests("resolution-10ns", { runargs = "resolution-10ns" })
    add_tests("env-default", { runargs = "env-default" })
    add_tests("env-warn", { runargs = "env-warn" })
    add_tests("env-off", { runargs = "env-off" })
    add_tests("env-invalid", { runargs = "env-invalid" })
    add_tests("env-modules", { runargs = "env-modules" })
    add_tests("env-modules-wildcard", { runargs = "env-modules-wildcard" })
    add_tests("env-modules-global", { runargs = "env-modules-global" })
    add_tests("env-modules-order", { runargs = "env-modules-order" })
    add_tests("env-modules-invalid", { runargs = "env-modules-invalid" })
    add_tests("registry", { runargs = "registry" })
    add_tests("parse-level", { runargs = "parse-level" })
  end)
end

-- ── SVDPI: real-simulator run (Synopsys VCS) ──────────────────────────
-- Categories 2-4 live under tests/svdpi/. There is deliberately no host-side
-- unit harness for this backend: its whole surface is the simulator (DPI/VPI
-- time functions, loaded DPI-C libraries, the exported C ABI), so it is verified
-- where it runs. Category 2 runs the shared tests/cases.cpp scenario bodies
-- directly in the DPI-C process; categories 3 and 4 drive the SystemVerilog
-- side. `xmake run vcs-test` builds the two DPI-C libraries (xmake owns the
-- include paths) and lets tests/svdpi/run.sh compile + run the benches; the
-- script skips itself when there is no vcs, so this target never becomes a hard
-- dependency of the normal workflow.
if get_config("backend") == "svdpi" then
  -- Category 2 — C++ side of simlog, driven by an SV bench that only advances
  -- time. The backend comes from libsimlog.so, which run.sh passes to vcs next
  -- to this library; its symbols stay undefined here, exactly the way a real
  -- project loads the packaged DPI-C library. tests/cases.cpp is the shared
  -- scenario core, the same file the SystemC unit suite compiles.
  target("svdpi_cpp", function()
    set_kind("shared")
    set_default(false)
    set_warnings("all", "extra", "error")
    set_filename("libsimlog_svdpi_cpp.so")
    add_deps("simlog")
    add_files("tests/svdpi/cpp/core_dpi.cpp", "tests/cases.cpp")
    add_includedirs("tests")
    add_rpathdirs("$ORIGIN")
  end)

  -- Category 4 — mixed mode. run.sh loads only this library, and this library
  -- links libsimlog.so (the `simlog` target) rather than compiling
  -- src/simlog.cpp again: one shared object is one library state, so the C++
  -- logger registry and the SV-side bridge (simlog_open/simlog_log) are the
  -- same state, exactly as for any other consumer. src/simlog.cpp is compiled
  -- once, into the `simlog` target — never duplicated into a test library.
  target("svdpi_mixed", function()
    set_kind("shared")
    set_default(false)
    set_warnings("all", "extra", "error")
    set_filename("libsimlog_svdpi_mixed.so")
    add_deps("simlog")
    add_files("tests/svdpi/mixed/mixed.cpp")
    add_includedirs("tests")
    add_packages("svdpi")
    add_rpathdirs("$ORIGIN")
  end)

  -- Categories 2, 3 and 4 in one real-simulator run. The script skips itself
  -- when there is no vcs, so this target never becomes a hard dependency of the
  -- normal workflow.
  target("vcs-test", function()
    set_kind("phony")
    set_default(false)
    add_deps("svdpi_cpp", "simlog", "svdpi_mixed")
    on_run(function(target)
      local cpp = target:dep("svdpi_cpp")
      local bridge = target:dep("simlog")
      local mixed = target:dep("svdpi_mixed")
      os.execv("bash", {
        path.join(os.projectdir(), "tests/svdpi/run.sh"),
        cpp:targetfile(),
        bridge:targetfile(),
        mixed:targetfile(),
      })
    end)
  end)
end
