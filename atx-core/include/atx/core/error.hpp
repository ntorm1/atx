#pragma once

// Rust-style error handling on top of tl::expected.
//
//   Result<T>  ==  tl::expected<T, Error>   // a value-or-error
//   Status     ==  Result<void>             // success-or-error, no value
//   Ok(v)      -> success                    Err(code, msg) -> failure
//   ATX_TRY(decl, expr)  ~  Rust `let decl = expr?;`  (early-return on error)
//
// Design (agent profile §4): expected failures travel in the return type, not
// via exceptions. Errors are never ignored — Result is [[nodiscard]], and TRY
// forces propagation. The enclosing function of an ATX_TRY must itself return a
// Result/Status (or anything constructible from tl::unexpected<Error>).

#include <string>
#include <string_view>
#include <utility>

#include <tl/expected.hpp>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

// ---------------------------------------------------------------------
//  Error domain
// ---------------------------------------------------------------------
enum class ErrorCode : u16 {
  Unknown = 0,
  InvalidArgument,
  OutOfRange,
  NotFound,
  AlreadyExists,
  PermissionDenied,
  Unavailable,
  Internal,
  NotImplemented,
  IoError,
  ParseError,
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::Unknown:
    return "Unknown";
  case ErrorCode::InvalidArgument:
    return "InvalidArgument";
  case ErrorCode::OutOfRange:
    return "OutOfRange";
  case ErrorCode::NotFound:
    return "NotFound";
  case ErrorCode::AlreadyExists:
    return "AlreadyExists";
  case ErrorCode::PermissionDenied:
    return "PermissionDenied";
  case ErrorCode::Unavailable:
    return "Unavailable";
  case ErrorCode::Internal:
    return "Internal";
  case ErrorCode::NotImplemented:
    return "NotImplemented";
  case ErrorCode::IoError:
    return "IoError";
  case ErrorCode::ParseError:
    return "ParseError";
  }
  return "Unrecognized"; // unreachable for valid enumerators
}

// Carries a machine-readable code plus an optional human context string.
// Cheap to move; copies only when context is non-empty.
class Error {
public:
  Error() noexcept = default;
  explicit Error(ErrorCode code, std::string message = {})
      : code_{code}, message_{std::move(message)} {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }

  // "Code" or "Code: message".
  [[nodiscard]] std::string to_string() const {
    std::string out{atx::core::to_string(code_)};
    if (!message_.empty()) {
      out += ": ";
      out += message_;
    }
    return out;
  }

private:
  ErrorCode code_{ErrorCode::Unknown};
  std::string message_;
};

// ---------------------------------------------------------------------
//  Result
// ---------------------------------------------------------------------
//  A value-or-error built on tl::expected<T, Error>. Subclassed (rather than a
//  bare alias) purely to add the Rust-spelled predicates is_ok()/is_err() on top
//  of tl::expected's has_value(); it inherits every constructor and the implicit
//  conversion from tl::unexpected<Error>, so Ok(...)/Err(...) and ATX_TRY keep
//  working unchanged. No data members are added (Rule of Zero; same layout).
template <class T> struct Result : tl::expected<T, Error> {
  using tl::expected<T, Error>::expected; // inherit all base constructors

  // true when this holds a value (Rust `is_ok`); the negation is is_err().
  [[nodiscard]] constexpr bool is_ok() const noexcept { return this->has_value(); }
  // true when this holds an Error (Rust `is_err`).
  [[nodiscard]] constexpr bool is_err() const noexcept { return !this->has_value(); }
};

using Status = Result<void>; // success-or-error, carries no value

// ---------------------------------------------------------------------
//  Rust-style constructors
// ---------------------------------------------------------------------
template <class T> [[nodiscard]] constexpr Result<std::decay_t<T>> Ok(T &&value) {
  return Result<std::decay_t<T>>(std::forward<T>(value));
}

[[nodiscard]] inline Status Ok() noexcept { return Status{}; }

// tl::unexpected<Error> implicitly converts into any Result<T>, so a single
// overload set serves every return type.
[[nodiscard]] inline tl::unexpected<Error> Err(Error error) {
  return tl::unexpected<Error>(std::move(error));
}

[[nodiscard]] inline tl::unexpected<Error> Err(ErrorCode code, std::string message = {}) {
  return tl::unexpected<Error>(Error{code, std::move(message)});
}

} // namespace atx::core

// ---------------------------------------------------------------------
//  Propagation macros (Rust `?`)
//
//  ATX_TRY(decl, expr): evaluate `expr` (a Result). On error, return the error
//  from the enclosing function. On success, bind the unwrapped value to `decl`
//  (pass the declaration, e.g. `ATX_TRY(auto x, foo());`).
//
//  ATX_TRY_VOID(expr): same, but discards the value (for Status-returning
//  calls). Use when you only care that the call succeeded.
//
//  SAFETY: relies on a uniquely-named temporary (ATX_UNIQUE_NAME) so multiple
//  TRYs in one scope don't collide; the temporary is moved-from exactly once.
// ---------------------------------------------------------------------
#define ATX_TRY_IMPL(tmp, decl, expr)                                                              \
  auto &&tmp = (expr);                                                                             \
  if (!(tmp))                                                                                      \
    return ::tl::unexpected<::atx::core::Error>(std::move(tmp).error());                           \
  decl = *std::move(tmp)

#define ATX_TRY(decl, expr) ATX_TRY_IMPL(ATX_UNIQUE_NAME(_atx_try_), decl, expr)

#define ATX_TRY_VOID(expr)                                                                         \
  do {                                                                                             \
    auto &&ATX_TRY_TMP = (expr);                                                                   \
    if (!(ATX_TRY_TMP))                                                                            \
      return ::tl::unexpected<::atx::core::Error>(std::move(ATX_TRY_TMP).error());                 \
  } while (0)
