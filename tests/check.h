// Minimal test harness shared by the simlog tests.
//
// Every test scenario runs in its own process: the library reads SIMLOG_LEVEL
// once (on the first simlog::get()) and SystemC accepts a time resolution only
// once per run, so isolation has to happen at process level. A scenario reports
// failures through CHECK / CHECK_EQ and exits non-zero when anything failed,
// which is what `xmake test` picks up.
#pragma once

#include <spdlog/common.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace test {

inline int failures = 0;

template <typename T>
std::string show(const T& value) {
  return spdlog::fmt_lib::format("{}", value);
}

inline void report_failure(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, what.c_str());
  ++failures;
}

inline void check(bool ok, const char* expr, const char* file, int line) {
  if (!ok) report_failure(file, line, expr);
}

template <typename Actual, typename Expected>
void check_eq(const Actual& actual, const Expected& expected, const char* actual_expr,
              const char* expected_expr, const char* file, int line) {
  if (actual == expected) return;
  report_failure(file, line,
                 std::string(actual_expr) + " == " + expected_expr +
                     "\n  actual:   " + show(actual) + "\n  expected: " + show(expected));
}

namespace detail {

#if defined(_WIN32)
inline int dup_fd(int fd) {
  return ::_dup(fd);
}
inline int dup2_fd(int from, int to) {
  return ::_dup2(from, to);
}
inline int file_no(std::FILE* file) {
  return ::_fileno(file);
}
inline int close_fd(int fd) {
  return ::_close(fd);
}
#else
inline int dup_fd(int fd) {
  return ::dup(fd);
}
inline int dup2_fd(int from, int to) {
  return ::dup2(from, to);
}
inline int file_no(std::FILE* file) {
  return ::fileno(file);
}
inline int close_fd(int fd) {
  return ::close(fd);
}
#endif

}  // namespace detail

// Redirects stdout or stderr into an anonymous temporary file, so formatted
// logger output can be compared exactly.
class capture {
public:
  enum class stream { out, err };

  explicit capture(stream which)
      : fd_(which == stream::out ? 1 : 2),
        target_(which == stream::out ? stdout : stderr),
        file_(std::tmpfile()) {
    if (file_) {
      saved_ = detail::dup_fd(fd_);
      if (saved_ < 0 || detail::dup2_fd(detail::file_no(file_), fd_) < 0) {
        if (saved_ >= 0) detail::close_fd(saved_);
        saved_ = -1;
        std::fclose(file_);
        file_ = nullptr;
      }
    }
  }

  capture(const capture&) = delete;
  capture& operator=(const capture&) = delete;

  ~capture() { stop(); }

  [[nodiscard]] bool active() const { return file_ != nullptr; }

  // Restores the original stream and returns everything written meanwhile.
  std::string stop() {
    if (!file_) return {};
    std::fflush(target_);  // the sink buffers through stdout/stderr
    detail::dup2_fd(saved_, fd_);
    detail::close_fd(saved_);
    saved_ = -1;
    std::rewind(file_);
    std::string captured;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, file_)) > 0;) {
      captured.append(buf, n);
    }
    std::fclose(file_);
    file_ = nullptr;
    return captured;
  }

private:
  int fd_;
  std::FILE* target_;
  std::FILE* file_;
  int saved_ = -1;
};

inline bool set_env(const char* name, const char* value) {
#if defined(_WIN32)
  return ::_putenv_s(name, value) == 0;
#else
  return ::setenv(name, value, 1) == 0;
#endif
}

inline bool unset_env(const char* name) {
#if defined(_WIN32)
  return ::_putenv_s(name, "") == 0;
#else
  return ::unsetenv(name) == 0;
#endif
}

// Drops ANSI SGR sequences, so assertions also hold on a color terminal.
inline std::string strip_ansi(std::string_view text) {
  std::string plain;
  plain.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && text[i] != 'm') ++i;
      continue;
    }
    plain.push_back(text[i]);
  }
  return plain;
}

inline std::size_t count_occurrences(std::string_view text, std::string_view needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0;
  for (auto pos = text.find(needle); pos != std::string_view::npos;
       pos = text.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

// Keeps only log lines: SystemC writes its banner and info messages to the same
// stream, while every log line starts with the pattern's first field ("[...").
inline std::vector<std::string> log_lines(const std::string& captured) {
  std::vector<std::string> lines;
  std::string_view rest(captured);
  while (!rest.empty()) {
    const auto eol = rest.find('\n');
    std::string line = strip_ansi(rest.substr(0, eol));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line.front() == '[') lines.push_back(std::move(line));
    if (eol == std::string_view::npos) break;
    rest.remove_prefix(eol + 1);
  }
  return lines;
}

}  // namespace test

#define CHECK(expr) ::test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected) \
  ::test::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)
