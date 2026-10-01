#pragma once

// atx::engine::book -- per-name, per-period stock-borrow schedule for replay.
//
// Replaces the single ReplayConfig::annual_borrow_bps with a dense
// (period x instrument) fee and locate grid, plus a short-proceeds rebate and a
// cash rate, when a replay opts in through ReplayConfig::borrow_schedule.
//
//   fee(i, t)    annual borrow fee in bps on marked short dollars of name i
//                held over the interval that starts at observation t. Empty
//                grid == default_fee_bps for every cell.
//   locate(i, t) maximum short DOLLARS the desk can locate for name i at
//                execution period t. +inf == unlimited, 0 == hard-to-borrow with
//                no locate. Empty grid == unlimited everywhere.
//   rebate_bps   annual rebate earned on total short dollars (reduces the charge),
//                in the usual rebate = policy rate - borrow fee definition: the
//                fee is ALREADY netted inside a quoted rebate.
//                May be negative when the borrow fee exceeds proceeds interest.
//   cash_bps     annual rate earned on positive FREE cash and paid on negative
//                free cash (a margin loan), after the trade.
//
// ShortFinancing picks how the fee and the rebate combine (B-05):
//   FeeOnceV2 (THE DEFAULT) counts the borrow fee exactly once. A schedule is
//     quoted EITHER as fees (fee grid / default_fee_bps, rebate_bps == 0): the
//     fee is charged and the short proceeds earn the cash rate like any cash,
//       sum_i short_i * fee(i,t) - cash * cash_bps;
//     OR as a rebate (rebate_bps != 0, every fee == 0): the proceeds earn the
//     rebate, which already has the fee netted, and only free cash
//     (settled cash - short dollars) earns the cash rate,
//       - shorts * rebate - (cash - shorts) * cash_bps.
//     A schedule with both a nonzero fee and a nonzero rebate is rejected.
//   FeeAndRebateV1 (pre-W0) charges the fee AND credits the rebate,
//       sum_i short_i * fee(i,t) - shorts * rebate - (cash - shorts) * cash_bps,
//     which counts the fee twice when the rebate is quoted net of it. Kept only
//     to re-derive frozen artifacts.
// Either way the result is ReplayInterval::borrow_cost on the elapsed day basis.
// That NET financing figure can be negative (a cash-rich book earns interest);
// it is still reconciled exactly by
//   nav(t+1) == pretrade_nav + gross_pnl - trade_cost - borrow_cost.
//
// LOCATE RULE: a trade that would leave name i short by more than locate(i, t)
// AND increases the short (in marked dollars) is clipped to the locate and
// reported (ReplayConfig::locate_breach == ClipV2, the default) or rejected
// (AbortV1). A short that only shrinks or is carried is never force-covered by
// a vanished locate: recalls are an explicit policy decision, not a replay side
// effect.

#include <cmath>
#include <limits>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

enum class ShortFinancing : atx::u8 { FeeAndRebateV1 = 1, FeeOnceV2 = 2 };

struct BorrowSchedule {
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<atx::f64> fee_bps;        // dates * instruments, period-major; or empty.
  std::vector<atx::f64> locate_dollars; // dates * instruments, period-major; or empty.
  atx::f64 default_fee_bps{};
  atx::f64 rebate_bps{};
  atx::f64 cash_bps{};
  ShortFinancing financing{ShortFinancing::FeeOnceV2};

  // True iff any fee (default or grid cell) is nonzero.
  [[nodiscard]] bool quotes_fee() const noexcept {
    if (default_fee_bps != 0.0) return true;
    for (const auto fee_value : fee_bps) {
      if (fee_value != 0.0) return true;
    }
    return false;
  }

  [[nodiscard]] atx::f64 fee(atx::usize instrument, atx::usize period) const noexcept {
    return fee_bps.empty() ? default_fee_bps : fee_bps[period * instruments + instrument];
  }
  [[nodiscard]] atx::f64 locate(atx::usize instrument, atx::usize period) const noexcept {
    return locate_dollars.empty() ? std::numeric_limits<atx::f64>::infinity()
                                  : locate_dollars[period * instruments + instrument];
  }

  // Shape must match the replay panel; fees are finite and >= 0; locates are
  // >= 0 (inf allowed); rebate and cash rates are finite (either sign);
  // the financing rule is recognized, and under FeeOnceV2 a fee and a rebate
  // are not both quoted.
  [[nodiscard]] atx::core::Status validate(atx::usize panel_dates,
                                           atx::usize panel_instruments) const {
    using atx::core::Err;
    using atx::core::ErrorCode;
    if (dates != panel_dates || instruments != panel_instruments) {
      return Err(ErrorCode::InvalidArgument, "borrow schedule: shape does not match panel");
    }
    const auto cells = dates * instruments;
    if ((!fee_bps.empty() && fee_bps.size() != cells) ||
        (!locate_dollars.empty() && locate_dollars.size() != cells)) {
      return Err(ErrorCode::InvalidArgument, "borrow schedule: grid size mismatch");
    }
    for (const auto fee_value : fee_bps) {
      if (!std::isfinite(fee_value) || fee_value < 0.0) {
        return Err(ErrorCode::InvalidArgument, "borrow schedule: invalid fee");
      }
    }
    for (const auto located : locate_dollars) {
      if (std::isnan(located) || located < 0.0) {
        return Err(ErrorCode::InvalidArgument, "borrow schedule: invalid locate");
      }
    }
    if (!std::isfinite(default_fee_bps) || default_fee_bps < 0.0 ||
        !std::isfinite(rebate_bps) || !std::isfinite(cash_bps)) {
      return Err(ErrorCode::InvalidArgument, "borrow schedule: invalid scalar rate");
    }
    if (financing != ShortFinancing::FeeAndRebateV1 && financing != ShortFinancing::FeeOnceV2) {
      return Err(ErrorCode::InvalidArgument, "borrow schedule: unrecognized financing rule");
    }
    if (financing == ShortFinancing::FeeOnceV2 && rebate_bps != 0.0 && quotes_fee()) {
      return Err(ErrorCode::InvalidArgument,
                 "borrow schedule: a fee and a rebate are both quoted; the rebate already "
                 "nets the fee (FeeOnceV2), quote one of them");
    }
    return atx::core::Ok();
  }
};

} // namespace atx::engine::book
