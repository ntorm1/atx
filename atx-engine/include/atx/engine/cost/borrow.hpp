#pragma once

// atx::engine::cost — Short-borrow / financing accrual (Sprint S6-5).
//
// ===========================================================================
//  What this header does — the last piece of the net-cost picture
// ===========================================================================
//  The S6 chain calibrates impact (S6-1), proves temp/perm separation (S6-2),
//  sizes capacity (S6-3) and prices turnover into the decision knobs (S6-4).
//  What remains for a NET cost is the financing leg: holding a short position
//  borrows the stock, and the lender charges interest on the borrowed notional.
//  This header models that daily borrow accrual and applies it ADDITIVELY to the
//  cash ledger — it never opens, closes, or marks a position.
//
// ===========================================================================
//  The accrual — short notional only
// ===========================================================================
//  A daily borrow charge is interest on the SHORT notional only (the long leg of
//  a dollar-neutral book is owned outright, not borrowed; a dollar-neutral book
//  is therefore ~half short and pays borrow on that half):
//
//      short_notional = Σ_{id : qty(id) < 0}  |qty(id)| · mark(id)
//      charge         = short_notional · annual_rate / denom(day_count)
//
//  where denom is the day-count basis (360 / 365 / 252). A long-only book has no
//  short notional and pays zero borrow.
//
// ===========================================================================
//  The Decimal ↔ f64 money boundary (one crossing, at the charge)
// ===========================================================================
//  short_notional is an f64 sum: |qty| is an integer share count but `mark` is
//  f64 market data (Market::mark), so the per-name term qty·mark is inherently
//  f64. The charge crosses to exact Decimal EXACTLY ONCE, via from_double, at the
//  ledger boundary — the same f64→Decimal idiom the execution simulator uses for
//  its fill price / fee (value_or(Decimal{}) absorbs the non-finite case; a
//  finite notional always converts). Downstream the charge is exact Decimal and
//  rides Portfolio::accrue_financing with no further FP.
//
// ===========================================================================
//  Why accrue_financing, not a synthetic fill (the S6-0 finding, §0.7)
// ===========================================================================
//  Portfolio::apply_fill ASSERTS f.qty != 0 (a zero-qty fill is not a
//  transaction — it aborts in Debug). A borrow charge is a pure cash debit with
//  NO position change, so it cannot ride apply_fill. accrue_financing — the ONE
//  reviewed engine touch of the whole S6 sprint — is the minimal additive
//  cash-only mutator it rides instead.
//
// ===========================================================================
//  Universe & ownership
// ===========================================================================
//  Portfolio exposes no public holdings iterator (the dense store is private), so
//  daily_borrow takes an EXPLICIT universe span and sums over holding(id) for each
//  id. The span is non-owning — the caller's InstrumentId storage must outlive the
//  call. The Market and Portfolio are read-only inputs to daily_borrow; only
//  accrue_borrow mutates (the cash ledger, via accrue_financing).

#include <span> // std::span (universe view)

#include "atx/core/decimal.hpp" // atx::core::Decimal (exact charge at the ledger)
#include "atx/core/macro.hpp"   // ATX_CHECK (exhaustive-switch fallthrough guard)
#include "atx/core/types.hpp"   // atx::u8, atx::f64, atx::i64

#include "atx/engine/loop/market.hpp"           // atx::engine::Market (mark source)
#include "atx/engine/loop/types.hpp"            // InstrumentId
#include "atx/engine/portfolio/portfolio.hpp"   // Portfolio, Holding (short-notional source)

namespace atx::engine::cost {

// ===========================================================================
//  DayCount — the borrow-rate annualization basis.
//
//  u8 underlying (single-byte, ring-slot discipline). Exhaustive switches over
//  it carry NO `default` — a new enumerator surfaces as a /W4 warning at every
//  switch rather than a silent fall-through.
// ===========================================================================
enum class DayCount : atx::u8 { D360, D365, D252 }; // closed; no `default` in switches

// ===========================================================================
//  BorrowModel — the short-borrow cost parameters.
//
//  annual_rate : annualized borrow rate as a fraction (0.05 == 5%/yr).
//  day_count   : the annualization basis (default 360, the money-market norm).
// ===========================================================================
struct BorrowModel {
  atx::f64 annual_rate;
  DayCount day_count = DayCount::D360;
};

// ---------------------------------------------------------------------------
//  day_count_denom — the day-count basis (days per year).
// ---------------------------------------------------------------------------
/// Days-per-year denominator for `d`: 360 / 365 / 252. Exhaustive switch, NO
/// `default` (a new DayCount enumerator surfaces as a /W4 warning here). The
/// trailing ATX_CHECK is unreachable for a valid enum value; it guards a
/// bit-corrupted cast (the value is consumed as a divisor, so a 0/garbage denom
/// would be a silent state error under NDEBUG).
[[nodiscard]] inline atx::f64 day_count_denom(DayCount d) {
  switch (d) {
  case DayCount::D360:
    return 360.0;
  case DayCount::D365:
    return 365.0;
  case DayCount::D252:
    return 252.0;
  }
  ATX_CHECK(false); // unreachable for a valid DayCount; bit-corrupted cast guard
  return 360.0;
}

// ---------------------------------------------------------------------------
//  daily_borrow — the exact daily borrow charge over the supplied universe.
// ---------------------------------------------------------------------------
/// Sums the SHORT notional over `universe` (for each id, if holding(id).qty < 0,
/// short_notional += |qty| · mark(id)) and returns the exact Decimal charge
/// short_notional · annual_rate / denom(day_count). A long-only book (no short
/// names) returns exactly Decimal{} (zero). The short-notional sum is f64 (qty is
/// integer, mark is f64 market data); it crosses to exact Decimal ONCE here, at
/// the ledger boundary, via from_double (value_or absorbs the non-finite case —
/// a finite notional always converts). SAFETY: `universe` is a non-owning view;
/// the caller's InstrumentId storage must outlive the call.
[[nodiscard]] inline atx::core::Decimal
daily_borrow(const BorrowModel& b, const Portfolio& pf, const atx::engine::Market& mkt,
             std::span<const InstrumentId> universe) {
  atx::f64 short_notional = 0.0;
  for (const InstrumentId id : universe) {
    const Holding& h = pf.holding(id);
    if (h.qty < 0) {
      const atx::f64 qty_abs = -static_cast<atx::f64>(h.qty); // |qty|, qty < 0 here
      short_notional += qty_abs * mkt.mark(id);
    }
  }
  const atx::f64 charge = short_notional * b.annual_rate / day_count_denom(b.day_count);
  return atx::core::Decimal::from_double(charge).value_or(atx::core::Decimal{});
}

// ---------------------------------------------------------------------------
//  accrue_borrow — ADDITIVELY debit the daily borrow charge from cash.
// ---------------------------------------------------------------------------
/// Debits daily_borrow(...) from the portfolio's cash via
/// Portfolio::accrue_financing — NOT a synthetic fill (apply_fill asserts
/// qty != 0; the S6-0 finding). No position change; cash is the only mutation.
inline void accrue_borrow(const BorrowModel& b, Portfolio& pf, const atx::engine::Market& mkt,
                          std::span<const InstrumentId> universe) {
  pf.accrue_financing(daily_borrow(b, pf, mkt, universe));
}

} // namespace atx::engine::cost
