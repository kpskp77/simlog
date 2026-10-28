// The scenario bodies shared by both backends (see cases.h for the contract).
//
// Everything here is host-neutral: the checks are the same for SystemC and for
// SVDPI, and the only difference is who advances the clock between phases. A
// scenario therefore keeps its cross-phase state in function-local statics (a
// capture redirect and a logger handle), which is safe because every scenario
// runs in its own process on both backends.
#include "cases.h"

#include <simlog/simlog.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "check.h"

namespace test::cases {
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

// ── format: %* rendering, no logger involved ─────────────────────────
// Elaboration time is 0 ticks; time only advances once the host starts the
// simulation. The base declared by the host is also what resolution_unit() must
// report, so a backend that prints the wrong unit or scale fails here.
std::uint64_t format_step(const context& ctx, unsigned phase) {
  if (phase == 0) {
    CHECK_EQ(render("hello"), expect_line(0, ctx.base, "tb", "info", "hello"));
    CHECK_EQ(render("hello", "other", spdlog::level::warn),
             expect_line(0, ctx.base, "other", "warning", "hello"));
    CHECK_EQ(render(""), expect_line(0, ctx.base, "tb", "info", ""));

    CHECK_EQ(simlog::detail::now_ticks(), std::uint64_t{0});

    const simlog::detail::unit_scale unit = simlog::detail::resolution_unit();
    CHECK_EQ(unit.scale, ctx.base.scale);
    CHECK_EQ(std::string_view(unit.symbol), std::string_view(ctx.base.symbol));

    return 12340;
  }

  CHECK_EQ(simlog::detail::now_ticks(), std::uint64_t{12340});
  CHECK_EQ(render("after start"), expect_line(12340, ctx.base, "tb", "info", "after start"));

  // spdlog clones custom flags, so the flag must be independently copyable.
  const simlog::detail::sim_time_flag flag;
  const std::unique_ptr<spdlog::custom_flag_formatter> clone = flag.clone();
  CHECK(clone != nullptr);
  return 0;
}

// ── stamp: end-to-end through simlog::get(), across time ─────────────
// The capture stays redirecting stdout across the jumps: the host must be able
// to advance time while the logger (and its process-wide sink) is alive.
std::uint64_t stamp_step(const context& ctx, unsigned phase) {
  static std::unique_ptr<test::capture> out;
  static std::shared_ptr<spdlog::logger> log;

  if (phase == 0) {
    out = std::make_unique<test::capture>(test::capture::stream::out);
    CHECK(out->active());

    log = simlog::get(kLogger);
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
    return 12340;
  }

  if (phase == 1) {
    log->info("first");
    log->info("second");  // same timestep: identical stamp
    return 1;
  }

  log->info("third");
  log->flush();

  const auto lines = test::log_lines(out->stop());
  CHECK_EQ(lines.size(), std::size_t{4});
  if (lines.size() == 4) {
    CHECK_EQ(lines[0], expect_line(0, ctx.base, "tb", "info", "reset released"));
    CHECK_EQ(lines[1], expect_line(12340, ctx.base, "tb", "info", "first"));
    CHECK_EQ(lines[2], expect_line(12340, ctx.base, "tb", "info", "second"));
    CHECK_EQ(lines[3], expect_line(12341, ctx.base, "tb", "info", "third"));
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
    CHECK_EQ(quiet_lines[0], expect_line(12341, ctx.base, "tb", "warning", "kept"));
  }
  return 0;
}

// ── SIMLOG_LEVEL ─────────────────────────────────────────────────────
// No SIMLOG_LEVEL: the built-in default (info) applies.
std::uint64_t env_default_step(const context& ctx, unsigned) {
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
    CHECK_EQ(lines[0], expect_line(0, ctx.base, "tb", "info", "visible"));
  }
  return 0;
}

std::uint64_t env_warn_step(const context& ctx, unsigned) {
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
    CHECK_EQ(lines[0], expect_line(0, ctx.base, "tb", "warning", "visible"));
  }
  return 0;
}

std::uint64_t env_off_step(const context&, unsigned) {
  CHECK(test::set_env("SIMLOG_LEVEL", "off"));

  test::capture out(test::capture::stream::out);
  const auto log = simlog::get(kLogger);
  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::off));
  log->critical("hidden");
  log->flush();

  CHECK_EQ(test::log_lines(out.stop()).size(), std::size_t{0});
  return 0;
}

// An invalid value keeps the default and warns once on stderr.
std::uint64_t env_invalid_step(const context&, unsigned) {
  CHECK(test::set_env("SIMLOG_LEVEL", "Bogus"));

  test::capture err(test::capture::stream::err);
  const auto log = simlog::get(kLogger);
  const auto warnings = err.stop();  // the notice is emitted during get()

  CHECK_EQ(static_cast<int>(log->level()), static_cast<int>(spdlog::level::info));
  CHECK(warnings.find("ignoring SIMLOG_LEVEL entry") != std::string::npos);
  CHECK(warnings.find("\"Bogus\"") != std::string::npos);
  CHECK(warnings.find("expected a bare level") != std::string::npos);
  return 0;
}

// Module rules in the same variable: verbatim, comma-separated entries (empty
// ones skipped), unmatched modules keeping the global level.
std::uint64_t env_modules_step(const context& ctx, unsigned) {
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
    CHECK_EQ(lines[0], expect_line(0, ctx.base, "tb", "debug", "module level applied"));
  }
  return 0;
}

// A lone '*' is the global rule; named and longer patterns override it no matter
// in which order they appear, and ties go to the rule written later.
std::uint64_t env_modules_wildcard_step(const context&, unsigned) {
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
  return 0;
}

// The documented example: "*=debug,dma=info".
std::uint64_t env_modules_global_step(const context&, unsigned) {
  CHECK(test::set_env("SIMLOG_LEVEL", "*=debug,dma=info"));

  CHECK(simlog::detail::level_for("dma") == spdlog::level::info);
  CHECK(simlog::detail::level_for("tb") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("uart0") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("dma0") == spdlog::level::debug);
  return 0;
}

// Named rules win over the global rule regardless of the order they are written
// in: the same value with the named rule first gives the same levels.
std::uint64_t env_modules_order_step(const context&, unsigned) {
  CHECK(test::set_env("SIMLOG_LEVEL", "dma=info,*=debug"));

  CHECK(simlog::detail::level_for("dma") == spdlog::level::info);
  CHECK(simlog::detail::level_for("tb") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("uart0") == spdlog::level::debug);
  CHECK(simlog::detail::level_for("dma0") == spdlog::level::debug);
  return 0;
}

// Malformed entries are reported once each and ignored, so those modules fall
// back to the global level like unmatched ones.
std::uint64_t env_modules_invalid_step(const context&, unsigned) {
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
  return 0;
}

// A logger registered before simlog::get() is returned untouched.
std::uint64_t registry_step(const context&, unsigned) {
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
  return 0;
}

std::uint64_t parse_level_step(const context&, unsigned) {
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
  return 0;
}

}  // namespace

std::string expect_stamp(std::uint64_t ticks, const time_base& base) {
  const std::uint64_t value = ticks * base.scale;  // test tick counts stay small
  std::string number = std::to_string(value);
  if (number.size() < 12) number.insert(0, 12 - number.size(), ' ');
  return "[" + number + " " + base.symbol + "]";
}

std::string expect_line(std::uint64_t ticks, const time_base& base, std::string_view logger,
                        std::string_view level, std::string_view payload) {
  return expect_stamp(ticks, base) + "[" + std::string(logger) + "][" + std::string(level) + "] " +
         std::string(payload);
}

const std::vector<scenario>& shared() {
  static const std::vector<scenario> scenarios{
      {"format", format_step},
      {"stamp", stamp_step},
      {"env-default", env_default_step},
      {"env-warn", env_warn_step},
      {"env-off", env_off_step},
      {"env-invalid", env_invalid_step},
      {"env-modules", env_modules_step},
      {"env-modules-wildcard", env_modules_wildcard_step},
      {"env-modules-global", env_modules_global_step},
      {"env-modules-order", env_modules_order_step},
      {"env-modules-invalid", env_modules_invalid_step},
      {"registry", registry_step},
      {"parse-level", parse_level_step},
  };
  return scenarios;
}

const scenario* find(std::string_view name) {
  for (const scenario& scn : shared()) {
    if (name == scn.name) return &scn;
  }
  return nullptr;
}

void prepare(const scenario& scn) {
  // Keep scenarios hermetic: only the env-* ones set SIMLOG_LEVEL themselves.
  if (std::string_view(scn.name).substr(0, 4) != "env-") test::unset_env("SIMLOG_LEVEL");
}

}  // namespace test::cases
