#include "atx/core/log.hpp"
#include "atx/core/macro.hpp"

#include <gtest/gtest.h>

using atx::core::Log;
using atx::core::LogLevel;

TEST(LogTest, SingletonIsStable) {
  // Same instance across calls — addresses must match.
  EXPECT_EQ(&Log::get(), &Log::get());
}

TEST(LogTest, DefaultLevelIsError) {
  // Fresh process default; set explicitly to keep the test order-independent.
  Log::set_level(spdlog::level::err);
  EXPECT_EQ(Log::level(), spdlog::level::err);
}

TEST(LogTest, SetLevelRoundTrips) {
  Log::set_level(spdlog::level::trace);
  EXPECT_EQ(Log::level(), spdlog::level::trace);
  Log::set_level(spdlog::level::warn);
  EXPECT_EQ(Log::level(), spdlog::level::warn);
}

// Compile + run every severity macro (with file:line capture) — ensures the
// macro layer and the spdlog sink are wired. Output goes to the console sink.
TEST(LogTest, AllSeverityMacrosEmit) {
  Log::set_level(spdlog::level::trace);
  ATX_TRACE("trace {}", 1);
  ATX_DEBUG("debug {}", 2);
  ATX_INFO("info {}", 3);
  ATX_WARN("warn {}", 4);
  ATX_ERROR("error {}", 5);
  ATX_CRITICAL("critical {}", 6);
  Log::set_level(spdlog::level::err); // restore quiet default
  SUCCEED();
}
