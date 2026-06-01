#pragma once

// Core logging infrastructure (spdlog-backed).
//
// Design (agent profile §1, §6):
//   - Lazy static singleton (Meyers): thread-safe first-call init, no global
//     ctor ordering hazard, no manual lifetime management.
//   - Default level: `err` — errors + critical only. Raise via Log::set_level.
//   - Single colorized console sink. Pattern carries timestamp, short level,
//     and source `file:line` (captured by the ATX_* macros in macro.hpp).
//
// Use the ATX_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL macros (macro.hpp) for
// logging — they inject __FILE__/__LINE__ that this pattern renders. Call this
// header's Log directly only to configure (set_level).

#include <memory>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace atx::core {

using LogLevel = spdlog::level::level_enum;

class Log {
public:
  Log(const Log &) = delete;
  Log &operator=(const Log &) = delete;
  Log(Log &&) = delete;
  Log &operator=(Log &&) = delete;

  // Process-wide logger. First call constructs and configures it; subsequent
  // calls return the same instance. Thread-safe (C++11 magic statics).
  // Precondition: none. Never returns a dangling reference — storage is static.
  [[nodiscard]] static spdlog::logger &get() noexcept {
    static Log instance;
    return *instance.logger_;
  }

  // Set the minimum level that is emitted. Default is `err`.
  static void set_level(LogLevel level) noexcept { get().set_level(level); }

  [[nodiscard]] static LogLevel level() noexcept { return get().level(); }

private:
  Log() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    // %^…%$  : color range            %L : short level (E/W/I/…)
    // %s:%#  : source basename:line    %v : message
    sink->set_pattern("%^[%H:%M:%S.%e] [%L] [%s:%#] %v%$");

    logger_ = std::make_shared<spdlog::logger>("atx", std::move(sink));
    logger_->set_level(spdlog::level::err); // default: errors only
    logger_->flush_on(spdlog::level::warn); // never lose a warning+ on crash
  }

  std::shared_ptr<spdlog::logger> logger_;
};

} // namespace atx::core
