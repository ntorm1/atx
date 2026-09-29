#pragma once

// PRIVATE to atx-impl (v7 W4, review B7). Share orders of one decision: the decide
// path's weight deltas turned into lot-rounded share quantities at a reference price,
// with the minimum-notional rule, the rounding residuals and the expected book after the
// orders fill. Pure arithmetic over spans (no I/O), so the rule is tested on its own.
//
// Rounding rule (deterministic; IEEE binary64, evaluated as written):
//   scale            = nav_dollars / nav
//   current_shares_i = shares_i x scale                  (positions carry a shares column)
//                    = held_i x scale / mark_i            (otherwise; 0 when held_i == 0)
//   raw_i            = target_i x nav_dollars / price_i - current_shares_i
//   order_shares_i   = lot x round_half_away_from_zero(raw_i / lot)   (std::round)
//   residual_i       = raw_i - order_shares_i             (reported, never traded)
// An order is formed only where target_i != current_i (the orders.csv predicate). It is
// not sent when: price_i is not a finite positive close (refused_no_close, B7), it rounds
// to 0 lots (rounded_to_zero), or |order_shares_i x price_i| < min_notional and it is not
// an exit (dropped_min_notional; review exec F10: exits, target_i == 0 on a held name, are
// kept so no dust position survives). A name never sent keeps its current shares.

#include <span>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy::orders {
inline constexpr const char* rounding_rule =
    "scale = nav_dollars / nav; current_shares = shares x scale (positions shares column) or "
    "held_dollars x scale / mark (mark = the last present raw close at or before the as-of; "
    "0 when flat); raw = target_weight x nav_dollars / price - current_shares; shares = lot x "
    "round-half-away-from-zero(raw / lot); residual = raw - shares; an order is formed where "
    "target != current and sent unless the price is not a finite positive close "
    "(refused_no_close), it rounds to 0 lots (rounded_to_zero) or |shares x price| < "
    "min_notional on a non-exit (dropped_min_notional; exits, target 0 on a held name, are "
    "kept: review exec F10)";
inline constexpr atx::u64 max_lot_size = 1'000'000;

// One value per name for every non-empty span (shares may be empty).
struct Inputs {
  std::span<const atx::f64> target, current, held;
  std::span<const atx::f64> shares; // broker shares; empty: derived from held / mark
  std::span<const atx::f64> price;  // the reference close at the as-of (NaN: none)
  std::span<const atx::f64> mark;   // last present close <= as-of (NaN: never priced)
  std::span<const atx::f64> adv;    // raw-dollar ADV of the filling session
  atx::f64 nav{}, nav_dollars{}, min_notional{};
  atx::f64 max_participation{}; // the scenario's per-session cap (fraction of ADV); 0: none
  atx::u64 lot_size{1};
};
struct Order {
  atx::usize index{};
  atx::i64 shares{};
  atx::f64 raw_shares{}, residual_shares{}, price{}, notional{}, adv{}, participation{};
  bool exit{}, short_sale{}, below_min_kept{};
};
// The book after every sent order fills: one row per name with nonzero current shares or
// a sent order, ascending by index.
struct Expected {
  atx::usize index{};
  atx::f64 shares{}, current_shares{}, price{}; // price: the close, else the mark
  atx::i64 order_shares{};
};
struct Summary {
  atx::usize orders{}, buys{}, sells{}, short_sales{}, exits{};
  atx::usize refused_no_close{}, rounded_to_zero{}, dropped_min_notional{};
  atx::usize exits_below_min_kept{}, no_adv{}, above_participation_cap{};
  atx::f64 buy_notional{}, sell_notional{}, dropped_notional{}, rounded_to_zero_notional{};
  atx::f64 residual_notional_net{}, residual_notional_abs{}, residual_notional_max_abs{};
  atx::f64 participation_max{}; // over sent orders with ADV > 0 (0 when none)
};
struct Book {
  std::vector<Order> orders; // ascending by index
  std::vector<Expected> expected;
  Summary summary;
};
// Refuses (InvalidArgument) span sizes that differ, a non-positive or non-finite nav or
// nav_dollars, a negative or non-finite min_notional, lot_size outside [1, max_lot_size],
// a negative max_participation, and any order whose share count is not exact in binary64
// (|shares| > 2^53).
[[nodiscard]] atx::core::Result<Book> build(const Inputs& in);

// lot x round_half_away_from_zero(raw / lot); refused when raw is not finite or the
// result is not an exact integer.
[[nodiscard]] atx::core::Result<atx::i64> round_to_lots(atx::f64 raw_shares, atx::u64 lot);
} // namespace atx::impl::strategy::orders
