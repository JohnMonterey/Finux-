#pragma once

// A deliberately tiny assertion harness.
//
// The project has no test-framework dependency: every third-party library
// pulled in is one more thing that must build on a user's machine before they
// can see a taskbar, and these tests need nothing gtest would provide.

#include <cstdio>
#include <cstdlib>
#include <string>

namespace finux::check {

inline int failures = 0;

inline void report(bool ok, const char* expression, const char* file, int line,
                   const std::string& detail = {}) {
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
  if (!detail.empty()) std::fprintf(stderr, "     %s\n", detail.c_str());
}

// Exit code for a test that cannot run in this environment (no fonts, no
// display). CMake is told to treat this as "skipped" rather than "passed", so
// a missing dependency never masquerades as a green test.
constexpr int kSkipExitCode = 77;

inline int finish(const char* name) {
  if (failures == 0) {
    std::printf("PASS %s\n", name);
    return 0;
  }
  std::fprintf(stderr, "FAILED %s (%d check%s)\n", name, failures,
               failures == 1 ? "" : "s");
  return 1;
}

inline int skip(const char* name, const std::string& why) {
  std::printf("SKIP %s: %s\n", name, why.c_str());
  return kSkipExitCode;
}

}  // namespace finux::check

#define CHECK(expr) \
  ::finux::check::report((expr), #expr, __FILE__, __LINE__)

#define CHECK_MSG(expr, detail) \
  ::finux::check::report((expr), #expr, __FILE__, __LINE__, (detail))

#define CHECK_EQ(a, b)                                                     \
  ::finux::check::report((a) == (b), #a " == " #b, __FILE__, __LINE__,     \
                         std::string("got ") + std::to_string(a) +         \
                             ", want " + std::to_string(b))
