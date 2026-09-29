#include "strategy_orders.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace atx::impl::strategy::orders {
namespace {
using namespace atx;
namespace co = atx::core;
constexpr f64 exact_integer_max = 9'007'199'254'740'992.0; // 2^53
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();

co::Status validate(const Inputs& in) {
  const usize n = in.target.size();
  const bool sizes = in.current.size() == n && in.held.size() == n &&
                     (in.shares.empty() || in.shares.size() == n) && in.price.size() == n &&
                     in.mark.size() == n && in.adv.size() == n;
  if (!sizes)
    return co::Err(co::ErrorCode::InvalidArgument, "share orders: one value per name");
  if (!std::isfinite(in.nav) || !(in.nav > 0) || !std::isfinite(in.nav_dollars) ||
      !(in.nav_dollars > 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "share orders: nav and nav_dollars must be finite and > 0");
  if (!std::isfinite(in.min_notional) || in.min_notional < 0)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "share orders: min_notional must be finite and >= 0");
  if (in.lot_size < 1 || in.lot_size > max_lot_size)
    return co::Err(co::ErrorCode::InvalidArgument, "share orders: lot size in [1, 1000000]");
  if (!std::isfinite(in.max_participation) || in.max_participation < 0)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "share orders: max_participation must be finite and >= 0");
  for (usize i = 0; i < n; ++i)
    if (!std::isfinite(in.target[i]) || !std::isfinite(in.current[i]) ||
        !std::isfinite(in.held[i]) || (!in.shares.empty() && !std::isfinite(in.shares[i])))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "share orders: weights, held dollars and shares must be finite");
  return co::Ok();
}
f64 current_shares(const Inputs& in, usize i, f64 scale) {
  if (!in.shares.empty()) return in.shares[i] * scale;
  if (in.held[i] == 0) return 0.0;
  return in.held[i] * scale / in.mark[i]; // NaN when never priced
}
void add_sent(Summary& s, const Order& o, f64 max_participation) {
  ++s.orders;
  (o.shares > 0 ? s.buys : s.sells) += 1;
  (o.shares > 0 ? s.buy_notional : s.sell_notional) += std::abs(o.notional);
  s.short_sales += o.short_sale ? 1U : 0U;
  s.exits += o.exit ? 1U : 0U;
  s.exits_below_min_kept += o.below_min_kept ? 1U : 0U;
  const f64 residual = o.residual_shares * o.price;
  s.residual_notional_net += residual;
  s.residual_notional_abs += std::abs(residual);
  s.residual_notional_max_abs = std::max(s.residual_notional_max_abs, std::abs(residual));
  if (std::isnan(o.participation)) { ++s.no_adv; return; }
  s.participation_max = std::max(s.participation_max, o.participation);
  if (max_participation > 0 && o.participation > max_participation) ++s.above_participation_cap;
}
} // namespace

co::Result<i64> round_to_lots(f64 raw_shares, u64 lot) {
  if (!std::isfinite(raw_shares) || lot < 1 || lot > max_lot_size)
    return co::Err(co::ErrorCode::InvalidArgument, "share orders: raw shares or lot size");
  const f64 lot_value = static_cast<f64>(lot);
  const f64 shares = std::round(raw_shares / lot_value) * lot_value;
  if (!(std::abs(shares) <= exact_integer_max))
    return co::Err(co::ErrorCode::OutOfRange,
                   "share orders: order size beyond exact binary64 integers");
  return co::Ok(static_cast<i64>(shares));
}

co::Result<Book> build(const Inputs& in) {
  ATX_TRY_VOID(validate(in));
  try {
    const usize n = in.target.size();
    const f64 scale = in.nav_dollars / in.nav;
    Book book;
    for (usize i = 0; i < n; ++i) {
      const f64 current = current_shares(in, i, scale);
      const f64 price = in.price[i];
      const bool priced = std::isfinite(price) && price > 0;
      Expected keep{i, current, current, priced ? price : in.mark[i], 0};
      const bool keep_row = current != 0;
      if (in.target[i] == in.current[i]) {
        if (keep_row) book.expected.push_back(keep);
        continue;
      }
      if (!priced || !std::isfinite(current)) {
        ++book.summary.refused_no_close;
        if (keep_row) book.expected.push_back(keep);
        continue;
      }
      Order o;
      o.index = i; o.price = price; o.adv = in.adv[i];
      o.raw_shares = in.target[i] * in.nav_dollars / price - current;
      ATX_TRY(o.shares, round_to_lots(o.raw_shares, in.lot_size));
      o.residual_shares = o.raw_shares - static_cast<f64>(o.shares);
      o.notional = static_cast<f64>(o.shares) * price;
      o.exit = in.target[i] == 0 && current != 0;
      if (o.shares == 0) {
        ++book.summary.rounded_to_zero;
        book.summary.rounded_to_zero_notional += std::abs(o.raw_shares * price);
        if (keep_row) book.expected.push_back(keep);
        continue;
      }
      if (std::abs(o.notional) < in.min_notional) {
        if (!o.exit) {
          ++book.summary.dropped_min_notional;
          book.summary.dropped_notional += std::abs(o.notional);
          if (keep_row) book.expected.push_back(keep);
          continue;
        }
        o.below_min_kept = true;
      }
      o.participation = std::isfinite(o.adv) && o.adv > 0 ? std::abs(o.notional) / o.adv : nan;
      const f64 after = current + static_cast<f64>(o.shares);
      o.short_sale = o.shares < 0 && after < 0;
      add_sent(book.summary, o, in.max_participation);
      book.orders.push_back(o);
      book.expected.push_back({i, after, current, price, o.shares});
    }
    return co::Ok(std::move(book));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "share orders: allocation failed");
  }
}
} // namespace atx::impl::strategy::orders
