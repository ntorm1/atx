#pragma once

// DerivBook — portfolio-layer pricing of vol-derivative (swap) books.
//
// A book of variance/volatility-swap positions priced against a `SurfaceSet`:
// the ADDITIVE companion to `PortfolioPricer` for the non-option legs of a
// desk's risk. It deliberately does NOT touch `Portfolio` / `PreparedPortfolio`
// — the option pipeline's dedup, bit-identity and SIMD grouping contracts are
// tuned for listed contracts and stay untouched. An option book and a deriv
// book are priced SEPARATELY and their `PriceTotals` combined through
// `combine_totals`, which is the whole integration surface between the two.
//
// ## What one call does
//
// For each position, in input order:
//   1. resolve `uid` through `SurfaceSet::find`;
//   2. snapshot that surface's own carry at the contract tenor (and, on the
//      greek path, at the theta-roll tenor) as a `CurveSet` — see
//      detail/deriv_ref_bridge.hpp;
//   3. mark it through `deriv_price` (and, unless `greeks=false`, differentiate
//      it through `deriv_greeks`) — the same pricers the standalone
//      derivatives API uses, so a book mark and a single-contract mark of the
//      same contract are the same number;
//   4. scale by `qty` and accumulate into the totals block.
//
// ## TENOR HYGIENE IS THE CALLER'S — there is no fitted-range gate
//
// The `PricedSurface`-native `deriv_price` / `deriv_greeks` overloads REFUSE
// (`OutOfRange`) a tenor outside the surface's fitted pillar range: past the end
// pillars a strip's forward clamps flat while the surface keeps extrapolating
// economically, the two disagree, and `K_var` is biased with no signal at all.
//
// THIS API does not apply that gate. It must behave identically for an owned
// surface and for a memory-mapped `PricedSurfaceView`, and only the former can
// expose its pillar list (see detail/deriv_ref_bridge.hpp). So a position whose
// `maturity_t` falls outside its surface's fitted range is priced against
// flat-extrapolated carry and comes back `Ok` with a SILENTLY biased fair strike
// — no flag, no status. A caller that does not already constrain its book to
// fitted tenors (a backtest driving contracts off the same surfaces it fitted
// does) must screen `maturity_t` against `PricedSurface::context()` itself.
//
// ## Failure is a ROW property, never the call's
//
// A missing surface marks that row `ModelUnavailable`; a malformed contract
// (the pricers' InvalidArgument) marks it `InvalidContract`; any other pricing
// failure — a reserved engine or correction mode (NotImplemented), unresolvable
// carry (OutOfRange), a numeric blow-up — marks it `NumericError`. A failed row
// is NaN-filled and EXCLUDED from the totals — excluded, not zeroed, so a book
// total never quietly absorbs a position nobody could price. `price_deriv_book`
// itself returns an error only for a structurally impossible request; an empty
// book is valid and yields an empty frame with zeroed totals.
//
// TWO CONDITIONS REPORT `InvalidContract` WITHOUT THE CONTRACT ITSELF BEING AT
// FAULT, deliberately: a non-finite `DerivPosition::qty` (rejected here, at the
// boundary, before it can scale a good mark into a NaN on an otherwise-Ok lane),
// and a surface whose `pricing().S` is not > 0 (the greek path's bump validator
// rejects that as InvalidArgument). `PriceStatus` is a shared four-value
// vocabulary the option pipeline also switches on, so widening it for two
// lane-input faults would hand every existing consumer a new case to handle;
// `InvalidContract` — "this lane's inputs are malformed" — is the honest fit.
//
// ## NaN = not computed (the PriceTotals convention)
//
// Inherited verbatim from `portfolio_pricer.hpp`: a column that a request did
// not ask for is NaN, never 0.0, because a 0.0 is indistinguishable from a book
// that is genuinely flat in that greek. So under `greeks=false` every greek
// field — per row AND in the totals, INCLUDING the gross `abs_vega` companion —
// is NaN. `dP_dq` is ALWAYS NaN here: the carry axis is defined on the option
// pipeline's per-contract IV lane, and a swap has no such lane.
//
// ## Scaling
//
// `DerivContract::notional` already scales the pricers' PV and greeks;
// `DerivPosition::qty` is the POSITION multiple on top of it. Every numeric
// output of a row is qty-scaled exactly once, at the row. The one exception is
// `DerivPriceRow::greeks.quote` — the pricer's own centre quote, kept verbatim
// as a per-contract diagnostic (fair strike, strip grid, flags, vol-of-vol), so
// it is UNSCALED by construction. `fair_strike_dec` is likewise a per-contract
// rate, not a cash amount, and is unscaled.
//
// ## Determinism / threading
//
// The loop is SERIAL and accumulates in fixed input order, so the totals are
// bit-reproducible for a given book and surface set (the same convention the
// option pricer's reduction guarantees across thread counts). No threading in
// v1: swap books are small (tens of positions) next to the option books the
// fan-out exists for, and each position already pays a full strip integration.
//
// Thread-safety: `price_deriv_book` and `combine_totals` are stateless pure
// functions of their inputs, safe to call concurrently from any number of
// threads (the underlying surfaces are concurrent-const-safe).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>       // DerivPriceFrame::vega_by_tenor (ordered = the ladder)
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/derivatives.hpp"      // DerivContract/DerivConfig/DerivGreeks/DerivGreekBumps
#include "atx/vol/detail/aggregate_arity.hpp" // DerivPriceFrame field-count drift pin (GK-C9b)
#include "atx/vol/portfolio_pricer.hpp" // SurfaceSet, PriceTotals, PriceStatus, kPriceColumnNaN
#include "atx/vol/types.hpp"            // Result

namespace atx::vol {

// One held vol-derivative position. `id` is an opaque caller key echoed into
// every frame row; `uid` is matched against the registered surfaces' uids.
struct DerivPosition {
  std::uint64_t id{0};
  std::uint32_t uid{0};
  DerivContract contract{};
  double qty{1.0}; // signed position scale ON TOP OF contract.notional
};

// One priced position (input order preserved). `pv` and every `greeks` field
// are qty-scaled; `fair_strike_dec` and `greeks.quote` are per-contract and
// unscaled. On any non-Ok status every numeric field is NaN.
struct DerivPriceRow {
  std::uint64_t id{0};
  std::uint32_t uid{0};
  double pv{kPriceColumnNaN};
  double fair_strike_dec{kPriceColumnNaN};
  // The centre (unbumped) quote inside `greeks.quote` is populated on every Ok
  // row — including a marks-only one, where the nine sensitivity fields are all
  // NaN. It carries the strip diagnostics (resolved grid, flags, vol-of-vol) a
  // caller needs to reproduce or audit the mark.
  DerivGreeks greeks{};
  PriceStatus status{PriceStatus::Ok};
};

// A uid -> certified wing-band resolver (FIT-C7 / Task C-6). A caller who
// tracks each surface's build quality mode (e.g. `MarketSnapshot::provenance`
// / `certified_wing_band_for`, backtest.hpp -- the SurfaceSet this call prices
// against typically comes from one) supplies one so every position's
// `deriv_price`/`deriv_greeks` trusts exactly the band that mode certified
// instead of the mode-blind default. An empty resolver (the default) resolves
// the mode-blind band for every position -- unchanged prior behaviour for a
// caller that does not (yet) supply one.
using WingBandResolver = std::function<std::optional<double>(std::uint32_t uid)>;

// Rows in input order plus the column sums over the Ok rows.
struct DerivPriceFrame {
  std::vector<DerivPriceRow> rows;
  PriceTotals totals{};
  // GK-C9b. `totals.{theta,vanna,charm}` are NaN-poisoned by design the moment
  // ANY Ok row's own column is NaN (theta/charm: a contract too short to roll
  // past `bumps.time_years`; vanna/charm: additionally `second_order == false`)
  // -- that poisoning is unchanged. What was missing was a count: a NaN total
  // used to be a dead end with no way to tell "1 excluded lane" from "every
  // lane excluded". These name how many Ok rows were excluded from each
  // column, so the desk theta going NaN comes with a reason attached instead
  // of silence. 0 when `greeks` was false (nothing was attempted, not "nothing
  // excluded") or when every Ok lane's column was finite.
  std::uint32_t n_theta_excluded{0};
  std::uint32_t n_vanna_excluded{0};
  std::uint32_t n_charm_excluded{0};

  // Task F-7 term-bucket vega: `contract.maturity_t` -> the sum of that
  // tenor's qty-scaled `greeks.vega` over the Ok rows, i.e. `totals.vega`
  // split by expiry. A vol desk hedges a term STRUCTURE, and a single net vega
  // hides the commonest real exposure -- long front, short back, flat overall.
  //
  // Keyed by the raw maturity in years, so `std::map`'s ordering IS the ladder,
  // front to back. Exact double equality is the bucketing rule: two positions
  // land together iff they carry the identical `maturity_t`, which is what a
  // caller who built the book off one calendar gets. Callers wanting coarser
  // buckets (1M / 3M / 6M) round before building the book, or re-bucket this
  // map -- deliberately no rounding policy here, which would have to guess.
  //
  // Same NaN discipline as `totals.vega`: an Ok lane whose own vega is NaN
  // poisons ITS bucket only, leaving the other tenors readable. A row that did
  // not price at all contributes nothing (it is not an Ok row). EMPTY when
  // `price_deriv_book`'s `greeks` argument was false -- no vega was computed,
  // and an empty ladder says that, where a map of zeros would read as a
  // genuinely vega-flat book.
  //
  // Costs no extra repricing: it is a plain reduction over the same per-row
  // vegas `totals.vega` already sums.
  std::map<double, double> vega_by_tenor;

  // Number of rows that priced. Equals `totals.n_ok` by construction; counted
  // from `rows` so the frame stays self-describing if it is ever rebuilt or
  // filtered by a caller.
  [[nodiscard]] std::size_t n_ok() const noexcept;
};

// Drift pin: DerivPriceFrame has exactly SIX fields (v1.1 appended
// n_theta_excluded/n_vanna_excluded/n_charm_excluded in GK-C9b, then
// vega_by_tenor in Task F-7). Adding, removing, or splitting one breaks this
// line -- update the count, and confirm every construction site still
// default-constructs plus designated/member assignment (there is no positional
// brace-init of this type anywhere in this codebase today).
static_assert(detail::aggregate_arity_is_v<DerivPriceFrame, 6>,
              "DerivPriceFrame field count changed: update this pin.");

// Price every position in `book` against its uid's surface.
//
// @param surfaces uid -> surface resolver; a uid it does not know marks that
//                 ROW `ModelUnavailable`.
// @param book     positions, priced and reported in input order. Empty is
//                 valid and yields an empty frame with zeroed totals.
// @param cfg      pricing config, applied to every position.
// @param greeks   false prices MARKS ONLY: one `deriv_price` per position
//                 instead of the up-to-16 repricings a greek block costs (14
//                 bump-table evaluations, the centre, and `carry_theta`'s own
//                 fair-strike resolve), or up-to-20 with `bumps.smile_greeks`.
//                 Task F-7 recount: this said "~8-17 / up to 17", stale since
//                 Task P-2 removed the FD rate bump without re-counting; the
//                 replacement figures are the ones
//                 `SmileGreeks.OffByDefaultCostsNothing` measures. Every greek
//                 field (rows and totals) is then NaN, and `vega_by_tenor` is
//                 empty.
// @param bumps    finite-difference bump sizes; ignored when `greeks` is false.
//                 A non-positive bump is rejected by the pricer PER POSITION,
//                 so a malformed `bumps` shows up as every row reporting
//                 `InvalidContract` rather than as a call-level error.
// @param wing_band_of  FIT-C7 / Task C-6: uid -> certified wing-band resolver
//                 (see `WingBandResolver` above). Called once per position
//                 when set; unset (the default) resolves the mode-blind band
//                 for every position, unchanged prior behaviour.
// @return the frame. Per-position failures are reported as row status, so this
//         does not fail on any book the pricers can reject lane-by-lane.
[[nodiscard]] Result<DerivPriceFrame>
price_deriv_book(const SurfaceSet &surfaces, std::span<const DerivPosition> book,
                 const DerivConfig &cfg = DerivConfig{}, bool greeks = true,
                 const DerivGreekBumps &bumps = DerivGreekBumps{},
                 const WingBandResolver &wing_band_of = {});

// Field-wise sum of two totals blocks — the seam that lets an option book's
// `PriceTotals` and a deriv book's be reported as one desk-level risk number.
//
// NaN PROPAGATES per field, deliberately: if either side did not compute a
// column, the combined column is "not computed" too. Silently treating a
// marks-only block's NaN vega as 0 would report the option book's vega as the
// desk's, which is exactly the false-zero this convention exists to prevent.
// `n_ok` adds (saturating at UINT32_MAX rather than wrapping).
[[nodiscard]] PriceTotals combine_totals(const PriceTotals &a, const PriceTotals &b) noexcept;

} // namespace atx::vol
