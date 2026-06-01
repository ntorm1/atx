#pragma once

// Project-wide macros: token utilities, compiler hints, logging, assertions.
//
// Policy (agent profile §9): macros are a last resort, used only where the
// language cannot express the intent (source-location capture, conditional
// compilation, early-return propagation). Every macro is UPPER_SNAKE, ATX_-
// prefixed, and parenthesizes its arguments. The error-propagation macros
// (ATX_TRY / ATX_TRY_VOID) live in error.hpp, next to the Result type.

#include <cstdlib> // std::abort

#include <spdlog/spdlog.h>

#include "atx/core/log.hpp"

// =====================================================================
//  Token utilities
// =====================================================================
#define ATX_STRINGIFY_IMPL(x) #x
#define ATX_STRINGIFY(x) ATX_STRINGIFY_IMPL(x)

#define ATX_CONCAT_IMPL(a, b) a##b
#define ATX_CONCAT(a, b) ATX_CONCAT_IMPL(a, b)

// Unique identifier per expansion (distinct even on the same source line).
#define ATX_UNIQUE_NAME(base) ATX_CONCAT(base, __COUNTER__)

// =====================================================================
//  Compiler hints / attributes
// =====================================================================
#if defined(__GNUC__) || defined(__clang__)
#define ATX_LIKELY(x) __builtin_expect(!!(x), 1)
#define ATX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ATX_FORCE_INLINE inline __attribute__((always_inline))
#define ATX_DEBUG_BREAK() __builtin_trap()
#elif defined(_MSC_VER)
#define ATX_LIKELY(x) (x)
#define ATX_UNLIKELY(x) (x)
#define ATX_FORCE_INLINE __forceinline
#define ATX_DEBUG_BREAK() __debugbreak()
#else
#define ATX_LIKELY(x) (x)
#define ATX_UNLIKELY(x) (x)
#define ATX_FORCE_INLINE inline
#define ATX_DEBUG_BREAK() ((void)0)
#endif

// Silence an unused-variable/parameter without dropping the name.
#define ATX_UNUSED(x) (void)(x)

// Special-member suppression for non-value types (agent profile §1, Rule of Five).
#define ATX_DISABLE_COPY(T)                                                                        \
  T(const T &) = delete;                                                                           \
  T &operator=(const T &) = delete
#define ATX_DISABLE_MOVE(T)                                                                        \
  T(T &&) = delete;                                                                                \
  T &operator=(T &&) = delete
#define ATX_DISABLE_COPY_MOVE(T)                                                                   \
  ATX_DISABLE_COPY(T);                                                                             \
  ATX_DISABLE_MOVE(T)

// =====================================================================
//  Logging — wrap spdlog's source-location-aware macros.
//  __FILE__/__LINE__ are captured by spdlog and rendered as `file:line`
//  by the pattern in log.hpp. Compile-time stripped below SPDLOG_ACTIVE_LEVEL
//  (set to TRACE via CMake; raise it in release to remove low levels).
// =====================================================================
#define ATX_TRACE(...) SPDLOG_LOGGER_TRACE(&::atx::core::Log::get(), __VA_ARGS__)
#define ATX_DEBUG(...) SPDLOG_LOGGER_DEBUG(&::atx::core::Log::get(), __VA_ARGS__)
#define ATX_INFO(...) SPDLOG_LOGGER_INFO(&::atx::core::Log::get(), __VA_ARGS__)
#define ATX_WARN(...) SPDLOG_LOGGER_WARN(&::atx::core::Log::get(), __VA_ARGS__)
#define ATX_ERROR(...) SPDLOG_LOGGER_ERROR(&::atx::core::Log::get(), __VA_ARGS__)
#define ATX_CRITICAL(...) SPDLOG_LOGGER_CRITICAL(&::atx::core::Log::get(), __VA_ARGS__)

// =====================================================================
//  Fatal handlers & assertions
//
//  ATX_PANIC     : log critical, break, abort. Unrecoverable. Always active.
//  ATX_UNREACHABLE: mark logically impossible paths. Always active (aborts
//                   rather than invoking UB like std::unreachable()).
//  ATX_TODO      : unimplemented path; aborts so it can never ship silently.
//  ATX_CHECK(F)  : always-on precondition. ATX_CHECKF adds a format message.
//  ATX_ASSERT(F) : debug-only (compiled out when NDEBUG). Same forms.
// =====================================================================
#define ATX_PANIC(...)                                                                             \
  do {                                                                                             \
    ATX_CRITICAL(__VA_ARGS__);                                                                     \
    ATX_DEBUG_BREAK();                                                                             \
    std::abort();                                                                                  \
  } while (0)

#define ATX_UNREACHABLE()                                                                          \
  do {                                                                                             \
    ATX_CRITICAL("reached unreachable code");                                                      \
    ATX_DEBUG_BREAK();                                                                             \
    std::abort();                                                                                  \
  } while (0)

#define ATX_TODO()                                                                                 \
  do {                                                                                             \
    ATX_CRITICAL("not implemented");                                                               \
    ATX_DEBUG_BREAK();                                                                             \
    std::abort();                                                                                  \
  } while (0)

#define ATX_CHECK(cond)                                                                            \
  do {                                                                                             \
    if (ATX_UNLIKELY(!(cond))) {                                                                   \
      ATX_CRITICAL("CHECK failed: {}", ATX_STRINGIFY(cond));                                       \
      ATX_DEBUG_BREAK();                                                                           \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

#define ATX_CHECKF(cond, ...)                                                                      \
  do {                                                                                             \
    if (ATX_UNLIKELY(!(cond))) {                                                                   \
      ATX_CRITICAL("CHECK failed: {}", ATX_STRINGIFY(cond));                                       \
      ATX_CRITICAL(__VA_ARGS__);                                                                   \
      ATX_DEBUG_BREAK();                                                                           \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

#ifndef NDEBUG
#define ATX_ASSERT(cond) ATX_CHECK(cond)
#define ATX_ASSERTF(cond, ...) ATX_CHECKF(cond, __VA_ARGS__)
#else
#define ATX_ASSERT(cond) ((void)0)
#define ATX_ASSERTF(cond, ...) ((void)0)
#endif
