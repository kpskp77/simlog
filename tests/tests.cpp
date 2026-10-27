// Unit and integration tests for simlog.
//
// The binary takes a scenario name as its only argument; `xmake test` runs it
// once per scenario (see add_tests() in xmake.lua) so that every scenario gets a
// fresh process and therefore a fresh environment, time resolution and logger
// registry.
#include <simlog/simlog.h>
#include <spdlog/details/os.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check.h"

// sc_main is called by the SystemC kernel, so it has to stay exported even when
// the program is built with hidden default visibility.
#if defined(_WIN32)
#define SIMLOG_TEST_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define SIMLOG_TEST_EXPORT __attribute__((visibility("default")))
#else
#define SIMLOG_TEST_EXPORT
#endif

namespace {

constexpr const char* kLogger = "tb";

// Format one message through the library's formatter, without a sink, and
// return the line with its line ending removed.
std::string render(std::string_view payload, std::string_view logger = kLogger,
                   spdlog::level::level_enum level = spdlog::level::info) {
  const auto formatter = simlog::detail::make_sim_formatter();
  const spdlog::details::log_msg msg(spdlog::string_view_t(logger.data(), logger.size()), level,
                                     spdlog::string_view_t(payload.data(), payload.size()));
  spdlog::memory_buf_t buf;
  formatter->format(msg, buf);
  std::string line(buf.data(), buf.size());
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  return line;
}

// Stamp field is fixed width: value × resolution multiplier, right-aligned in
// 12 columns, plus the resolution's base unit.
void format_scenario() {
  CHECK_EQ(render("hello"), std::string("[           0 ps][tb][info] hello"));
  CHECK_EQ(render("hello", "other", spdlog::level::warn),
           std::string("[           0 ps][other][warning] hello"));
  CHECK_EQ(render(""), std::string("[           0 ps][tb][info] "));

  // Elaboration time is 0 ticks; time only advances once sc_start() runs.
  CHECK_EQ(simlog::detail::now_ticks(), std::uint64_t{0});
  CHECK_EQ(render("before start"), std::string("[           0 ps][tb][info] before start"));

  sc_core::sc_start(12340, sc_core::SC_PS);
  CHECK_EQ(simlog::detail::now_ticks(), std::uint64_t{12340});
  CHECK_EQ(render("after start"), std::string("[       12340 ps][tb][info] after start"));

  // spdlog clones custom flags, so the flag must be independently copyable.
  const simlog::detail::sim_time_flag flag;
  const std::unique_ptr<spdlog::custom_flag_formatter> clone = flag.clone();
  CHECK(clone != nullptr);
}

// End-to-end: simlog::get() returns a native, registered spdlog logger whose
// output carries the simulation time of the log call.
void stamp_scenario() {
  test::capture out(test::capture::stream::out);
  CHECK(out.active());

  const auto log = simlog::get(kLogger);
  CHECK(log != nullptr);
  CHECK_EQ(log->name(), std::string(kLogger));
  CHECK(simlog::get(kLogger) == log);  // repeated calls return the same instance
  CHECK(spdlog::get(kLogger) == log);  // and it is registered with spdlog
  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::info));

  // The default sink is process-wide: every simlog logger shares one instance.
  const auto other = simlog::get("other");
  CHECK_EQ(other->sinks().size(), std::size_t{1});
  CHECK(other->sinks().front() == log->sinks().front());

  log->info("reset released");
  sc_core::sc_start(12340, sc_core::SC_PS);
  log->info("first");
  log->info("second");  // same timestep: identical stamp
  sc_core::sc_start(1, sc_core::SC_PS);
  log->info("third");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{4});
  if (lines.size() == 4) {
    CHECK_EQ(lines[0], std::string("[           0 ps][tb][info] reset released"));
    CHECK_EQ(lines[1], std::string("[       12340 ps][tb][info] first"));
    CHECK_EQ(lines[2], std::string("[       12340 ps][tb][info] second"));
    CHECK_EQ(lines[3], std::string("[       12341 ps][tb][info] third"));
  }

  // The logger stays a plain spdlog logger: level filtering is native behaviour.
  test::capture quiet(test::capture::stream::out);
  log->set_level(spdlog::level::warn);
  log->info("dropped");
  log->warn("kept");
  log->flush();

  const auto quiet_lines = test::log_lines(quiet.stop());
  CHECK_EQ(quiet_lines.size(), std::size_t{1});
  if (quiet_lines.size() == 1) {
    CHECK_EQ(quiet_lines[0], std::string("[       12341 ps][tb][warning] kept"));
  }
}

// A resolution multiplier is folded into the tick count instead of the unit.
void resolution_100fs_scenario() {
  sc_core::sc_set_time_resolution(100, sc_core::SC_FS);
  const auto unit = simlog::detail::resolution_unit();
  CHECK_EQ(unit.scale, std::uint64_t{100});
  CHECK_EQ(std::string_view(unit.symbol), std::string_view("fs"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  log->info("start");
  sc_core::sc_start(1234000, sc_core::SC_FS);  // 12340 ticks, 100 fs each
  CHECK_EQ(sc_core::sc_time_stamp().value(), static_cast<sc_dt::uint64>(12340));
  log->info("later");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{2});
  if (lines.size() == 2) {
    CHECK_EQ(lines[0], std::string("[           0 fs][tb][info] start"));
    CHECK_EQ(lines[1], std::string("[     1234000 fs][tb][info] later"));
  }
}

void resolution_10ns_scenario() {
  sc_core::sc_set_time_resolution(10, sc_core::SC_NS);
  const auto unit = simlog::detail::resolution_unit();
  CHECK_EQ(unit.scale, std::uint64_t{10});
  CHECK_EQ(std::string_view(unit.symbol), std::string_view("ns"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  log->info("start");
  sc_core::sc_start(123400, sc_core::SC_NS);  // 12340 ticks, 10 ns each
  CHECK_EQ(sc_core::sc_time_stamp().value(), static_cast<sc_dt::uint64>(12340));
  log->info("later");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{2});
  if (lines.size() == 2) {
    CHECK_EQ(lines[0], std::string("[           0 ns][tb][info] start"));
    CHECK_EQ(lines[1], std::string("[      123400 ns][tb][info] later"));
  }
}

// No SIMLOG_LEVEL: the built-in default (info) applies.
void env_default_scenario() {
  test::unset_env("SIMLOG_LEVEL");

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::info));
  log->debug("hidden");
  log->info("visible");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{1});
  if (lines.size() == 1) {
    CHECK_EQ(lines[0], std::string("[           0 ps][tb][info] visible"));
  }
}

void env_warn_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "warn"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::warn));
  log->info("hidden");
  log->warn("visible");
  log->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{1});
  if (lines.size() == 1) {
    CHECK_EQ(lines[0], std::string("[           0 ps][tb][warning] visible"));
  }
}

void env_off_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "off"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::off));
  log->critical("hidden");
  log->flush();

  CHECK_EQ(test::log_lines(out.stop()).size(), std::size_t{0});
}

// An invalid value keeps the default and warns once on stderr.
void env_invalid_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "Bogus"));

  test::capture err(test::capture::stream::err);
  const auto log = simlog::get(kLogger);
  const auto warnings = err.stop();  // the notice is emitted during get()

  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::info));
  CHECK(warnings.find("ignoring SIMLOG_LEVEL entry") != std::string::npos);
  CHECK(warnings.find("\"Bogus\"") != std::string::npos);
  CHECK(warnings.find("expected a bare level") != std::string::npos);
}

// Module rules in the same variable: verbatim, comma-separated entries (empty
// ones skipped), unmatched modules keeping the global level.
void env_modules_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "tb=debug,,other=warn,dma0=off,"));

  const auto tb = simlog::get("tb");
  const auto other = simlog::get("other");
  const auto dma0 = simlog::get("dma0");
  const auto uart = simlog::get("uart");  // no rule → built-in global level

  CHECK_EQ(static_cast<int>(tb->level()), static_cast<int>(spdlog::level::debug));
  CHECK_EQ(static_cast<int>(other->level()), static_cast<int>(spdlog::level::warn));
  CHECK_EQ(static_cast<int>(dma0->level()), static_cast<int>(spdlog::level::off));
  CHECK_EQ(static_cast<int>(uart->level()), static_cast<int>(spdlog::level::info));

  // The rule really drives output: "tb" now emits debug lines.
  test::capture out(test::capture::stream::out);
  tb->debug("module level applied");
  tb->flush();

  const auto lines = test::log_lines(out.stop());
  CHECK_EQ(lines.size(), std::size_t{1});
  if (lines.size() == 1) {
    CHECK_EQ(lines[0], std::string("[           0 ps][tb][debug] module level applied"));
  }
}

// A lone '*' is the global rule; named and longer patterns override it no matter
// in which order they appear, and ties go to the rule written later.
void env_modules_wildcard_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "dma=info,*=debug,dma*=warn,*tb=err"));

  CHECK(simlog::detail::level_for("dma") == spdlog::level::info);   // exact beats every wildcard
  CHECK(simlog::detail::level_for("dma0") == spdlog::level::warn);  // "dma*" beats "*"
  CHECK(simlog::detail::level_for("dma_bus") == spdlog::level::warn);
  CHECK(simlog::detail::level_for("core_tb") == spdlog::level::err);  // "*tb" beats "*"
  CHECK(simlog::detail::level_for("tb") == spdlog::level::err);       // "*" may match nothing
  CHECK(simlog::detail::level_for("uart0") == spdlog::level::debug);  // global rule
  CHECK(simlog::detail::level_for("") == spdlog::level::debug);

  CHECK_EQ(static_cast<int>(simlog::get("dma")->level()), static_cast<int>(spdlog::level::info));
  CHECK_EQ(static_cast<int>(simlog::get("core_tb")->level()), static_cast<int>(spdlog::level::err));
}

// The documented example: "*=debug,dma=info".
void env_modules_global_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "*=debug,dma=info"));

  CHECK(simlog::detail::level_for("dma") == spdlog::level::info);
  CHECK(simlog::detail::level_for("tb") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("uart0") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("dma0") == spdlog::level::debug);
}

// Named rules win over the global rule regardless of the order they are written
// in: the same value with the named rule first gives the same levels.
void env_modules_order_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL", "dma=info,*=debug"));

  CHECK(simlog::detail::level_for("dma") == spdlog::level::info);
  CHECK(simlog::detail::level_for("tb") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("uart0") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("dma0") == spdlog::level::debug);
}

// Malformed entries are reported once each and ignored, so those modules fall
// back to the global level like unmatched ones.
void env_modules_invalid_scenario() {
  CHECK(test::set_env("SIMLOG_LEVEL",
                      "tb=bogus,uart=,=debug,broken,core=err, dma1=warn,uart0 =warn,dma2= warn"));

  test::capture err(test::capture::stream::err);
  const auto tb = simlog::get("tb");      // unknown level
  const auto uart = simlog::get("uart");  // missing level
  const auto core = simlog::get("core");  // valid rule after invalid ones
  const auto notices = err.stop();

  CHECK_EQ(static_cast<int>(tb->level()), static_cast<int>(spdlog::level::info));
  CHECK_EQ(static_cast<int>(uart->level()), static_cast<int>(spdlog::level::info));
  CHECK_EQ(static_cast<int>(core->level()), static_cast<int>(spdlog::level::err));

  // Whitespace is not skipped and not a separator: such entries are rejected
  // instead of turning into patterns that can never match.
  CHECK(simlog::detail::level_for("dma1") == spdlog::level::info);
  CHECK(simlog::detail::level_for("uart0") == spdlog::level::info);
  CHECK(simlog::detail::level_for("dma2") == spdlog::level::info);

  CHECK_EQ(test::count_occurrences(notices, "ignoring SIMLOG_LEVEL entry"), std::size_t{7});
  CHECK(notices.find("tb=bogus") != std::string::npos);
  CHECK(notices.find("uart=") != std::string::npos);
  CHECK(notices.find("=debug") != std::string::npos);
  CHECK(notices.find("broken") != std::string::npos);
  CHECK(notices.find("\" dma1=warn\"") != std::string::npos);  // reported verbatim
}

// A logger registered before simlog::get() is returned untouched.
void registry_scenario() {
  CHECK(spdlog::get("pre") == nullptr);

  std::ostringstream pre_stream;
  const auto pre_sink = std::make_shared<spdlog::sinks::ostream_sink_st>(pre_stream);
  const auto pre = std::make_shared<spdlog::logger>("pre", pre_sink);
  pre->set_level(spdlog::level::err);
  pre->set_pattern("custom: %v");
  spdlog::register_logger(pre);

  const auto got = simlog::get("pre");
  CHECK(got == pre);  // not replaced
  CHECK_EQ(static_cast<int>(got->level()), static_cast<int>(spdlog::level::err));
  CHECK_EQ(got->sinks().size(), std::size_t{1});
  CHECK(got->sinks().front() == pre_sink);

  pre->error("kept pattern");
  pre->flush();
  CHECK_EQ(pre_stream.str(), std::string("custom: kept pattern\n"));
}

void parse_level_scenario() {
  using spdlog::level::level_enum;
  CHECK(simlog::detail::parse_level("trace") == level_enum::trace);
  CHECK(simlog::detail::parse_level("debug") == level_enum::debug);
  CHECK(simlog::detail::parse_level("info") == level_enum::info);
  CHECK(simlog::detail::parse_level("warn") == level_enum::warn);
  CHECK(simlog::detail::parse_level("err") == level_enum::err);
  CHECK(simlog::detail::parse_level("critical") == level_enum::critical);
  CHECK(simlog::detail::parse_level("off") == level_enum::off);

  CHECK(!simlog::detail::parse_level("bogus"));
  CHECK(!simlog::detail::parse_level("WARN"));     // names are case-sensitive
  CHECK(!simlog::detail::parse_level("warning"));  // spdlog's own spelling
  CHECK(!simlog::detail::parse_level(""));

  // SIMLOG_LEVEL patterns: '*' is the only special character.
  CHECK(simlog::detail::wildcard_match("tb", "tb"));
  CHECK(!simlog::detail::wildcard_match("tb", "tb_top"));
  CHECK(simlog::detail::wildcard_match("*", ""));
  CHECK(simlog::detail::wildcard_match("*", "anything"));
  CHECK(simlog::detail::wildcard_match("dma*", "dma0"));
  CHECK(!simlog::detail::wildcard_match("dma*", "xdma0"));
  CHECK(simlog::detail::wildcard_match("*tb", "core_tb"));
  CHECK(simlog::detail::wildcard_match("a*b*c", "aXbYbZc"));
  CHECK(!simlog::detail::wildcard_match("a*b*c", "aXbYbZ"));
  CHECK(simlog::detail::wildcard_match("**", "x"));
  CHECK(!simlog::detail::wildcard_match("", "x"));
  CHECK(simlog::detail::wildcard_match("", ""));

  CHECK(simlog::detail::default_sink() == simlog::detail::default_sink());
}

}  // namespace

SIMLOG_TEST_EXPORT int sc_main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scenario>\n", argc > 0 ? argv[0] : "tests");
    return 2;
  }

  const std::string_view scenario(argv[1]);

  // Keep scenarios hermetic: only the env-* ones set SIMLOG_LEVEL themselves.
  if (scenario.substr(0, 4) != "env-") test::unset_env("SIMLOG_LEVEL");

  if (scenario == "format") {
    format_scenario();
  } else if (scenario == "stamp") {
    stamp_scenario();
  } else if (scenario == "resolution-100fs") {
    resolution_100fs_scenario();
  } else if (scenario == "resolution-10ns") {
    resolution_10ns_scenario();
  } else if (scenario == "env-default") {
    env_default_scenario();
  } else if (scenario == "env-warn") {
    env_warn_scenario();
  } else if (scenario == "env-off") {
    env_off_scenario();
  } else if (scenario == "env-invalid") {
    env_invalid_scenario();
  } else if (scenario == "env-modules") {
    env_modules_scenario();
  } else if (scenario == "env-modules-wildcard") {
    env_modules_wildcard_scenario();
  } else if (scenario == "env-modules-global") {
    env_modules_global_scenario();
  } else if (scenario == "env-modules-order") {
    env_modules_order_scenario();
  } else if (scenario == "env-modules-invalid") {
    env_modules_invalid_scenario();
  } else if (scenario == "registry") {
    registry_scenario();
  } else if (scenario == "parse-level") {
    parse_level_scenario();
  } else {
    std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return 2;
  }

  if (test::failures != 0) {
    std::fprintf(stderr, "%d check(s) failed (scenario %s)\n", test::failures, argv[1]);
    return 1;
  }
  return 0;
}
