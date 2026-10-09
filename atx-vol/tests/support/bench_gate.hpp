#pragma once
#include <cstddef>
#include <cstdlib>
#include <gtest/gtest.h>
namespace atx::vol::testkit {
// A benchmark (asserts timing/throughput, not correctness) runs only when
// ATX_VOL_BENCH is set to a non-empty, non-"0" value. Off by default so the
// standard parallel gate neither pays their cost nor flakes on wall-clock.
//
// The value is read with _dupenv_s under MSVC/clang-cl: plain std::getenv trips
// /WX (-Wdeprecated-declarations) here, and this header is included after
// <cstdlib>, so a local _CRT_SECURE_NO_WARNINGS define would be too late.
// Matches the codebase env-read pattern (see atx-engine data test _dupenv_s).
[[nodiscard]] inline bool bench_enabled() noexcept {
#if defined(_MSC_VER)
  char* e = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&e, &n, "ATX_VOL_BENCH") != 0 || e == nullptr) {
    return false;
  }
  const bool on = e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
  std::free(e);
  return on;
#else
  const char* e = std::getenv("ATX_VOL_BENCH");
  return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
#endif
}
}  // namespace atx::vol::testkit
#define ATX_VOL_SKIP_UNLESS_BENCH()                                            \
  do {                                                                         \
    if (!::atx::vol::testkit::bench_enabled()) {                               \
      GTEST_SKIP() << "benchmark (timing, not correctness); set ATX_VOL_BENCH=1 to run"; \
    }                                                                          \
  } while (0)
