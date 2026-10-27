-- simlog — spdlog extension for EDA simulation-time stamps (xmake build)
-- Usage:
--   xmake                 # build the header-only target and the SystemC example
--   xmake test            # build and run all test scenarios
--   xmake -b tests        # build the tests only
--   xmake run tests stamp # run one scenario directly
--   xmake run example     # run the example (xmake sets LD_LIBRARY_PATH for the
--                         # shared SystemC package)

set_project("simlog")
set_version("0.1.0")
set_languages("c++17")

add_rules("mode.debug", "mode.release")

-- header-only dependencies come from xrepo; SystemC is required because
-- simlog.h includes <systemc> unconditionally
add_requires("spdlog", { configs = { header_only = true } })
add_requires("systemc", { configs = { shared = true } })

-- ── Core library: header-only ─────────────────────────────────────────
target("simlog", function()
  set_kind("headeronly")
  set_warnings("all", "extra", "error")
  add_headerfiles("include/(simlog/*.h)")
  add_includedirs("include", { public = true })
  add_packages("spdlog", { public = true })
  -- simlog.h always needs SystemC headers and links against libsystemc
  add_packages("systemc", { public = true })
end)

-- ── Example ───────────────────────────────────────────────────────────
target("example", function()
  set_kind("binary")
  set_warnings("all", "extra", "error")
  add_files("examples/systemc.cpp")
  add_deps("simlog")
end)

-- ── Tests ─────────────────────────────────────────────────────────────
-- One binary, one process per scenario: the library reads SIMLOG_LEVEL once
-- and SystemC accepts a time resolution once per run, so scenarios must not
-- share a process. Run them with `xmake test` (or `xmake run tests <scenario>`).
target("tests", function()
  set_kind("binary")
  set_default(false)
  set_warnings("all", "extra", "error")
  add_files("tests/tests.cpp")
  add_deps("simlog")
  add_tests("format", {runargs = "format"})
  add_tests("stamp", {runargs = "stamp"})
  add_tests("resolution-100fs", {runargs = "resolution-100fs"})
  add_tests("resolution-10ns", {runargs = "resolution-10ns"})
  add_tests("env-default", {runargs = "env-default"})
  add_tests("env-warn", {runargs = "env-warn"})
  add_tests("env-off", {runargs = "env-off"})
  add_tests("env-invalid", {runargs = "env-invalid"})
  add_tests("env-modules", {runargs = "env-modules"})
  add_tests("env-modules-wildcard", {runargs = "env-modules-wildcard"})
  add_tests("env-modules-global", {runargs = "env-modules-global"})
  add_tests("env-modules-order", {runargs = "env-modules-order"})
  add_tests("env-modules-invalid", {runargs = "env-modules-invalid"})
  add_tests("registry", {runargs = "registry"})
  add_tests("parse-level", {runargs = "parse-level"})
end)
