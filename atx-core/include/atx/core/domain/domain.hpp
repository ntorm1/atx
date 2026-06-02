#pragma once

// atx::core::domain — market vocabulary built on the exact-money Decimal.
//
// Strong unit types (Price, Quantity, Notional) wrap a single private Decimal
// so the type system enforces dimensional discipline: you can multiply a Price
// by a Quantity to get a Notional, but you cannot add a Price to a Quantity.
// This kills the classic parameter-swap / unit-mixing bug class (agent §2) at
// compile time, while keeping every arithmetic op exact (no FP drift) by
// delegating to Decimal.
//
// ------------------------------------------------------------------
//  Design contract
// ------------------------------------------------------------------
//   from_decimal(Decimal) : exact wrap, constexpr, never fails.
//   from_int(i64)         : whole units via Decimal::from_int (ABORTS in debug
//                           on |whole| > Decimal::kMaxWhole — programmer error).
//   from_string(sv)       : Result; delegates to Decimal::from_string, so it
//                           inherits ParseError / OutOfRange for bad/huge input.
//   to_decimal()          : the wrapped Decimal (accessor).
//   to_string()           : Decimal's canonical form (trailing zeros trimmed,
//                           at least one fractional digit; e.g. 31.5 -> "31.5").
//   operator<=> / ==      : defaulted on the wrapped Decimal — numeric ordering.
//
//  Typed arithmetic:
//   Price   * Quantity -> Notional   (exact Decimal multiply, truncate-to-zero)
//   Notional +/- Notional -> Notional
//   Quantity +/- Quantity -> Quantity
//
//  POD records (Bar, Tick) timestamp with atx::core::time::Timestamp — the one
//  canonical timestamp; this module does NOT invent its own.

#include <compare>      // std::strong_ordering
#include <string>       // std::string (to_string return)
#include <string_view>  // std::string_view (from_string input)

#include "atx/core/datetime.hpp" // atx::core::time::Timestamp
#include "atx/core/decimal.hpp"  // atx::core::Decimal
#include "atx/core/error.hpp"    // Result, Ok, ATX_TRY
#include "atx/core/types.hpp"    // i64, u8

namespace atx::core::domain {

// =====================================================================
//  Strong unit types over Decimal.
//
//  Each is a value type (Rule of Zero) holding one Decimal. The macro-free
//  duplication is deliberate: the types are intentionally NOT interchangeable,
//  so they do not share a base and cannot be implicitly converted to one
//  another. Only the explicitly-defined cross-type operators bridge them.
// =====================================================================

class Price {
public:
  constexpr Price() noexcept = default;

  /// Exact wrap of an existing Decimal.
  [[nodiscard]] static constexpr Price from_decimal(Decimal value) noexcept { return Price{value}; }

  /// Whole-unit price. PRECONDITION: |whole| <= Decimal::kMaxWhole (ABORTS in
  /// debug); use from_string for untrusted input.
  [[nodiscard]] static constexpr Price from_int(i64 whole) noexcept {
    return Price{Decimal::from_int(whole)};
  }

  /// Parse a decimal string. Propagates Decimal::from_string's ParseError /
  /// OutOfRange unchanged.
  [[nodiscard]] static Result<Price> from_string(std::string_view text) {
    ATX_TRY(auto value, Decimal::from_string(text));
    return Ok(Price{value});
  }

  /// The wrapped Decimal.
  [[nodiscard]] constexpr Decimal to_decimal() const noexcept { return value_; }

  /// Canonical decimal string (Decimal::to_string policy).
  [[nodiscard]] std::string to_string() const { return value_.to_string(); }

  [[nodiscard]] constexpr bool operator==(const Price &) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Price &) const noexcept = default;

private:
  explicit constexpr Price(Decimal value) noexcept : value_{value} {}
  Decimal value_{};
};

class Quantity {
public:
  constexpr Quantity() noexcept = default;

  [[nodiscard]] static constexpr Quantity from_decimal(Decimal value) noexcept {
    return Quantity{value};
  }

  [[nodiscard]] static constexpr Quantity from_int(i64 whole) noexcept {
    return Quantity{Decimal::from_int(whole)};
  }

  [[nodiscard]] static Result<Quantity> from_string(std::string_view text) {
    ATX_TRY(auto value, Decimal::from_string(text));
    return Ok(Quantity{value});
  }

  [[nodiscard]] constexpr Decimal to_decimal() const noexcept { return value_; }
  [[nodiscard]] std::string to_string() const { return value_.to_string(); }

  [[nodiscard]] constexpr bool operator==(const Quantity &) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Quantity &) const noexcept = default;

private:
  explicit constexpr Quantity(Decimal value) noexcept : value_{value} {}
  Decimal value_{};
};

class Notional {
public:
  constexpr Notional() noexcept = default;

  [[nodiscard]] static constexpr Notional from_decimal(Decimal value) noexcept {
    return Notional{value};
  }

  [[nodiscard]] static constexpr Notional from_int(i64 whole) noexcept {
    return Notional{Decimal::from_int(whole)};
  }

  [[nodiscard]] static Result<Notional> from_string(std::string_view text) {
    ATX_TRY(auto value, Decimal::from_string(text));
    return Ok(Notional{value});
  }

  [[nodiscard]] constexpr Decimal to_decimal() const noexcept { return value_; }
  [[nodiscard]] std::string to_string() const { return value_.to_string(); }

  [[nodiscard]] constexpr bool operator==(const Notional &) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Notional &) const noexcept = default;

private:
  explicit constexpr Notional(Decimal value) noexcept : value_{value} {}
  Decimal value_{};
};

// =====================================================================
//  Typed arithmetic — the only sanctioned bridges between units.
//
//  All delegate to the exact Decimal ops, so they inherit Decimal's overflow
//  precondition (ABORTS in debug; programmer error in normal market ranges).
// =====================================================================

/// Price x Quantity = Notional (exact Decimal multiply, truncated toward zero).
[[nodiscard]] constexpr Notional operator*(Price price, Quantity quantity) noexcept {
  return Notional::from_decimal(price.to_decimal() * quantity.to_decimal());
}

/// Quantity x Price = Notional (commutative convenience).
[[nodiscard]] constexpr Notional operator*(Quantity quantity, Price price) noexcept {
  return price * quantity;
}

[[nodiscard]] constexpr Notional operator+(Notional a, Notional b) noexcept {
  return Notional::from_decimal(a.to_decimal() + b.to_decimal());
}

[[nodiscard]] constexpr Notional operator-(Notional a, Notional b) noexcept {
  return Notional::from_decimal(a.to_decimal() - b.to_decimal());
}

[[nodiscard]] constexpr Quantity operator+(Quantity a, Quantity b) noexcept {
  return Quantity::from_decimal(a.to_decimal() + b.to_decimal());
}

[[nodiscard]] constexpr Quantity operator-(Quantity a, Quantity b) noexcept {
  return Quantity::from_decimal(a.to_decimal() - b.to_decimal());
}

// =====================================================================
//  Side — order/trade aggressor direction.
// =====================================================================
enum class Side : u8 { Buy, Sell };

/// The opposite side. Total over the two enumerators (no default — a new
/// enumerator would surface as a compiler warning under /W4).
[[nodiscard]] constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

// =====================================================================
//  POD market records — trivially-copyable value aggregates.
//
//  Timestamps use atx::core::time::Timestamp (the canonical instant type).
// =====================================================================

/// One OHLCV bar over a fixed interval. `ts` is the bar's reference instant
/// (open or close per the producer's convention — this type stays agnostic).
struct Bar {
  atx::core::time::Timestamp ts{};
  Price open{};
  Price high{};
  Price low{};
  Price close{};
  Quantity volume{};

  [[nodiscard]] friend constexpr bool operator==(const Bar &, const Bar &) noexcept = default;
};

/// OHLCV is the conventional spelling of a Bar; alias for call-site clarity.
using OHLCV = Bar;

/// A single trade print: instant, price, size, and aggressor side.
struct Tick {
  atx::core::time::Timestamp ts{};
  Price price{};
  Quantity size{};
  Side side{Side::Buy};

  [[nodiscard]] friend constexpr bool operator==(const Tick &, const Tick &) noexcept = default;
};

} // namespace atx::core::domain
