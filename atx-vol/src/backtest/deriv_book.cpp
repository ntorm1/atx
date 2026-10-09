#include "atx/vol/api/backtest/deriv_book.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

#include "atx/core/error.hpp"
#include "pricing/deriv_ref_bridge.hpp" // deriv_price_on_ref / deriv_greeks_on_ref (P-6)

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = kPriceColumnNaN;

// A DerivGreeks in which NOTHING is claimed to have been computed. The struct
// default-initializes its sensitivities to 0.0 (Task 7's convention, where a
// fully-aged contract genuinely HAS zero market greeks), which at the portfolio
// layer would be indistinguishable from a measured zero. Every numeric field --
// the thirteen sensitivities (the original nine, plus Task C-10's theta_carry /
// theta_zero_fixing and Task F-7's skew_vega / convexity_vega) AND the embedded
// centre quote's -- is therefore overwritten with the frame's "not computed"
// sentinel. `strip_nodes_used == 0` and `flags == None` are already exactly
// "no strip ran".
//
// THIS LIST IS HAND-ENUMERATED and `DerivGreeks`' arity pin cannot see an
// omission from it -- a new sensitivity left out here ships un-NaN'd on every
// failed lane, reading as a measured zero. Extend it in the same commit that
// appends the field.
[[nodiscard]] DerivGreeks nan_greeks() noexcept {
  DerivGreeks g{};
  g.pv = kNaN;
  g.delta = kNaN;
  g.gamma = kNaN;
  g.vega = kNaN;
  g.volga = kNaN;
  g.vanna = kNaN;
  g.theta = kNaN;
  g.rho = kNaN;
  g.charm = kNaN;
  g.theta_carry = kNaN;
  g.theta_zero_fixing = kNaN;
  g.skew_vega = kNaN;
  g.convexity_vega = kNaN;
  g.quote.fair_strike_dec = kNaN;
  g.quote.fair_strike_points = kNaN;
  g.quote.pv = kNaN;
  g.quote.undiscounted_expectation_dec = kNaN;
  g.quote.uncapped_var_dec = kNaN;
  g.quote.accrued_component_dec = kNaN;
  g.quote.future_component_dec = kNaN;
  g.quote.convexity_adjustment_dec = kNaN;
  g.quote.integration_error_est = kNaN;
  g.quote.vol_of_vol_used = kNaN;
  g.quote.cap_option_value_dec = kNaN;
  g.quote.strip_k_lo_used = kNaN;
  g.quote.strip_k_hi_used = kNaN;
  return g;
}

// Position-scale a computed greek block. The nine sensitivities, the pv, the
// two carry-theta diagnostics (Task C-10: theta_carry / theta_zero_fixing are
// cash-amounts-per-year exactly like theta, not per-contract rates) AND the two
// smile greeks (Task F-7) are cash amounts and scale; `quote` is the
// per-contract centre diagnostic and is carried through VERBATIM (see
// deriv_book.hpp "Scaling" -- "every numeric output of a row is qty-scaled
// exactly once", which already covers the appended fields by that general
// wording, not just the field list this function happened to enumerate before
// Task C-10 added to DerivGreeks).
//
// SAME HAND-ENUMERATION HAZARD as `nan_greeks` above: `out = g` copies the new
// field through UNSCALED, so an omission here is a silent mark bug on every
// qty != 1 position rather than a compile error. Extend both lists together.
[[nodiscard]] DerivGreeks scaled_greeks(const DerivGreeks &g, double qty) noexcept {
  DerivGreeks out = g;
  out.pv = qty * g.pv;
  out.delta = qty * g.delta;
  out.gamma = qty * g.gamma;
  out.vega = qty * g.vega;
  out.volga = qty * g.volga;
  out.vanna = qty * g.vanna;
  out.theta = qty * g.theta;
  out.rho = qty * g.rho;
  out.charm = qty * g.charm;
  out.theta_carry = qty * g.theta_carry;
  out.theta_zero_fixing = qty * g.theta_zero_fixing;
  // Task F-7: dPV/ds and dPV/dc are cash amounts per unit of smile
  // coefficient, exactly like vega is per unit of vol -- so they scale.
  out.skew_vega = qty * g.skew_vega;
  out.convexity_vega = qty * g.convexity_vega;
  return out;
}

// Pricer error -> lane status. The pricers report every malformed-contract
// rejection (T <= 0 on a live leg, a cap on an uncapped kind, a negative
// vol-of-vol, a non-positive bump) as InvalidArgument; everything else --
// OutOfRange carry, NotImplemented reserved engines, numeric blow-ups -- is a
// model/numeric failure on a well-formed contract.
[[nodiscard]] PriceStatus status_for(const Error &e) noexcept {
  return e.code() == ErrorCode::InvalidArgument ? PriceStatus::InvalidContract
                                                : PriceStatus::NumericError;
}

// ── Task P-6 (GK-P book memo): book-level shared VarSwap strip ─────────────
//
// L VarSwap positions sharing (uid, T) each used to pay their own full
// strip (and, on the greeks path, market-bump grid or P-4 analytic block)
// even though none of that expensive work reads anything but (uid, T, the
// book-wide cfg, and this uid's own certified wing band) -- see
// `detail::VarSwapSharedBlock`'s own doc (deriv_ref_bridge.hpp) for the
// bit-identity argument. This memo, built and consumed ONLY within one
// `price_deriv_book` call (mirrors `pnl_attribution.cpp`'s (uid,T) pivot
// memo, :113-127, the pattern this follows), resolves that shared block ONCE
// per distinct key and reuses it for every row that shares it.
//
// KEY-FIELD AUDIT -- every field that can change what the shared block
// computes, and why it is or is not part of the key:
//   uid            IN KEY. Selects the surface (and its forward/rate/vol
//                  reads) the strip integrates against.
//   T (maturity_t) IN KEY, compared by EXACT BITS (`t_bits`, never a
//                  tolerance -- two contracts a femtosecond apart in T must
//                  not collide). The strip's own grid/quadrature is a
//                  function of T.
//   wing band      IN KEY (`wing_band_of(uid)`'s resolved value, bits +
//                  presence). Feeds `resolve_wing_clamp` inside the strip.
//                  Documented as "uid -> certified wing-band resolver" (a
//                  pure function of uid), so in EVERY expected caller this
//                  is redundant with `uid` already being in the key -- kept
//                  as an EXPLICIT key field anyway (not assumed-constant) so
//                  a resolver that is NOT actually pure per-uid degrades to
//                  "separate blocks, one per distinct band observed" rather
//                  than silently serving a stale band to a later row. Costs
//                  nothing when the assumption holds (one wing-band value
//                  per uid -> one map entry per uid, same as omitting it).
//   kind-class     IN KEY, but as an ELIGIBILITY GATE rather than a stored
//                  key dimension: only `DerivKind::VarSwap` rows ever reach
//                  the memo at all (VolSwap/CappedVarSwap/CappedVolSwap price
//                  through a genuinely nonlinear model layer with no such
//                  shared strip -- P-4's own `DerivGreekMethod::AnalyticStrip`
//                  scope draws this exact line, derivatives.hpp). Every entry
//                  the map ever holds is therefore VarSwap by construction, so
//                  adding `kind` as a stored key field would be a no-op --
//                  functionally identical to gating eligibility up front,
//                  which is what this does.
//   cfg (whole)    NOT IN KEY, and provably so: `cfg` is ONE value for the
//                  ENTIRE `price_deriv_book` call (a single parameter, not
//                  per-row -- deriv_book.hpp's own signature), so it cannot
//                  vary between two rows the SAME memo instance ever prices.
//                  A per-row cfg field cannot cause a same-map collision
//                  regardless of whether it is a key dimension. What DOES
//                  gate eligibility (see `var_swap_memo_eligible` below) is
//                  `cfg.discrete_correction_mode`: memo-eligibility requires
//                  `== None`. `Diffusion1OverN` folds a QUADRATIC-in-K_var
//                  addend keyed off each ROW's own remaining-fixing count
//                  into the future leg BEFORE it becomes the shared value
//                  this memo would cache raw -- reproducing that per row
//                  would need caching `resolve_carry_diff`'s result too and
//                  re-deriving the addend per row, out of this task's scope
//                  (mirrors `DerivGreekMethod::AnalyticStrip`'s OWN identical
//                  exclusion, and the exact class of bug P-4 shipped once
//                  already -- a scope predicate that ignored this same field,
//                  CHANGELOG.md). `FullMc` is reserved (`NotImplemented`)
//                  regardless of memo. Book-wide, so gated ONCE, not per row.
//                  Task F-1 (`DerivConfig::wing_mode`) audited against this
//                  SAME "cfg is one book-wide value" proof and found
//                  PROVABLY IRRELEVANT to the key: `wing_mode` changes WHAT
//                  VALUE the shared block's strip serves (FlatClamp vs
//                  LeeSlopeExtrapolation vs Raw), never WHETHER two rows may
//                  share one -- every row this memo ever serves reads the
//                  SAME `cfg.wing_mode`, so the block it gets is built under
//                  that mode by construction, and the memo itself is a local
//                  variable scoped to one `price_deriv_book` call (built
//                  fresh, never persisted across calls -- see the file
//                  comment above), so there is no cross-call contamination
//                  path either. Unlike `discrete_correction_mode`, wing_mode
//                  needed NO eligibility gate: `Diffusion1OverN` changes
//                  which VALUE is linear in what the shared block caches
//                  (excluded above), but every `StripWingMode` still resolves
//                  to one well-defined K_var(T) the affine per-row combine
//                  can share identically.
//   bumps (whole)  NOT IN KEY, same reasoning as `cfg`: one value for the
//                  whole call. `bumps.method` (FiniteDifference vs
//                  AnalyticStrip) selects WHICH shared sub-block
//                  (`VarSwapSharedBlock::have_analytic`) gets built, but
//                  every row in ONE memo instance sees the SAME `bumps`, so
//                  there is no cross-row contamination risk to key against --
//                  see the P-4 interaction note below.
//   contract.strike_dec / notional / rv_spec / id
//                  NOT IN KEY, by design: these are exactly what the affine
//                  per-row combine (`assemble_var_swap_quote`,
//                  derivatives.cpp) applies AFTER the shared block, so two
//                  rows differing ONLY in these fields correctly, and
//                  intentionally, share one block. `VarSwapMemo.DiffersOnly*`
//                  (deriv_book_test.cpp) pins that changing each of these
//                  alone still changes the row's OWN total while the shared
//                  block's build count stays flat.
//   contract.marking
//                  NOT IN KEY, but for the SAME reason `cap_dec` below is, not
//                  the reason above it. Task F-5 made `CboeVarianceFuture` a
//                  hard `NotImplemented` in `validate_deriv_dispatch`, so `Otc`
//                  is now the only value that reaches a shared block at all and
//                  two rows CANNOT differ in it. This line used to list it
//                  alongside the affine-combine fields, which was true only
//                  because nothing read the field: rows differing in `marking`
//                  did share one block, and were all silently priced OTC.
//   contract.cap_dec
//                  NOT IN KEY. For a VarSwap this must be exactly 0.0 (a
//                  nonzero cap_dec on an uncapped kind is a malformed
//                  contract, `InvalidArgument`) -- `validate_var_swap_dispatch`
//                  (derivatives.cpp) checks this PER ROW regardless of the
//                  memo, so a malformed row fails correctly without needing
//                  a key entry of its own.
//   qty            NOT IN KEY. Applied by `price_one` itself, AFTER the
//                  shared-block call returns, exactly as it always was
//                  (`p.qty * g->pv`, `scaled_greeks(*g, p.qty)`) -- the memo
//                  is entirely upstream of this multiply.
//
// P-4 INTERACTION (analytic vs FD, read carefully). `VarSwapSharedBlock`
// resolves EITHER the raw FD market-bump grid OR the raw P-4 analytic block
// for a given (uid, T) group, chosen ONCE by `bumps.method` when the group is
// first built (`ensure_var_swap_greeks_block`, derivatives.cpp) -- never
// both, and never re-chosen later. Because `bumps` and `cfg` are both the
// SAME book-wide values for every row a single `price_deriv_book` call ever
// prices, EVERY row that reaches this memo (VarSwap, `discrete_correction_mode
// == None`) resolves `analytic_in_scope` to the SAME boolean -- there is no
// row-specific input left that could route two rows of the SAME (uid, T)
// group to different sub-blocks, so a row can never read a block built for
// the other method. (A hypothetical FUTURE per-row bumps override would need
// `bumps.method` added to the key -- there is none today, so it is not.)
// Review fix m-4: kind-class (VarSwap only) plus `discrete_correction_mode ==
// None` is NOT, on its own, P-4's full `AnalyticStrip` scope predicate --
// Task F-1 added `wing_mode != LeeSlopeExtrapolation` to it, so the two
// sentences above describing "the SAME boolean" understated what that
// boolean depends on. The conclusion is unaffected: this site's
// `analytic_in_scope` is `bumps.method == AnalyticStrip &&
// analytic_scope_from_cfg(cfg)`, calling the exact same
// `analytic_scope_from_cfg(const DerivConfig&)` P-4's other call site
// (`deriv_greeks`) does (review fix I-2's single shared definition, closing
// what used to be two independently-written copies) -- so a row this memo
// serves under FD would have taken the SAME FD path unmemoized, and one
// served under AnalyticStrip (which now also requires LeeSlope not be the
// active wing mode) would have taken that SAME path -- the memo still
// changes nothing about WHICH method prices a row, only whether the strip
// work is repeated.
[[nodiscard]] std::uint64_t bits_of(double x) noexcept {
  std::uint64_t b = 0;
  std::memcpy(&b, &x, sizeof b);
  return b;
}

// {uid, T-bits, wing-band-present, wing-band-bits}. `std::map`'s default
// lexicographic `<` gives deterministic, total ordering -- mirrors
// `pnl_attribution.cpp`'s own `std::map<std::pair<uint32_t,uint64_t>,...>`
// pivot memo exactly (same file, :113).
using VarSwapMemoKey = std::tuple<std::uint32_t, std::uint64_t, bool, std::uint64_t>;

[[nodiscard]] VarSwapMemoKey var_swap_memo_key(std::uint32_t uid, double maturity_t,
                                               std::optional<double> wing_band) noexcept {
  return VarSwapMemoKey{uid, bits_of(maturity_t), wing_band.has_value(),
                        wing_band.has_value() ? bits_of(*wing_band) : 0u};
}

using VarSwapMemo = std::map<VarSwapMemoKey, detail::VarSwapSharedBlock>;

// Eligibility gate ("kind-class" -- see the key-field audit above).
// `discrete_correction_ok` is resolved ONCE, book-wide, by the caller.
[[nodiscard]] bool var_swap_memo_eligible(const DerivContract &contract,
                                          bool discrete_correction_ok) noexcept {
  return discrete_correction_ok && contract.kind == DerivKind::VarSwap;
}

// Price ONE position. Never fails: every rejection is encoded in `status` with
// a NaN-filled row, which is what keeps a bad lane from failing the call.
[[nodiscard]] DerivPriceRow price_one(const SurfaceSet &surfaces, const DerivPosition &p,
                                      const DerivConfig &cfg, bool greeks,
                                      const DerivGreekBumps &bumps,
                                      const WingBandResolver &wing_band_of,
                                      bool discrete_correction_ok, VarSwapMemo &var_swap_memo) {
  DerivPriceRow row{};
  row.id = p.id;
  row.uid = p.uid;
  row.pv = kNaN;
  row.fair_strike_dec = kNaN;
  row.greeks = nan_greeks();

  const SurfaceRef ref = surfaces.find(p.uid);
  if (ref == nullptr) {
    row.status = PriceStatus::ModelUnavailable;
    return row;
  }
  // Boundary validation of the one input the pricers never see. A non-finite
  // qty would scale a perfectly good mark into a NaN on an "Ok" lane and poison
  // the totals; qty == 0 is a legitimate flat position and is priced normally.
  if (!std::isfinite(p.qty)) {
    row.status = PriceStatus::InvalidContract;
    return row;
  }

  // FIT-C7 / Task C-6: this row's own certified wing band, when the caller
  // supplied a resolver; std::nullopt (an unset resolver) resolves the
  // mode-blind default, unchanged prior behaviour.
  const std::optional<double> wing_band = wing_band_of ? wing_band_of(p.uid) : std::nullopt;

  // Task F-7: `VarSwapSharedBlock` carries no smile-bump slots, so a greeks row
  // that asks for `skew_vega`/`convexity_vega` takes the UNMEMOIZED path, which
  // computes them. The alternative -- letting the memo serve the row and
  // silently returning NaN for a greek the caller explicitly requested -- is
  // the "missing key" failure this codebase keeps paying for, and a second
  // bit-identical smile implementation inside the shared block is a cost worth
  // paying only once there is a book workload that needs it. Nothing existing
  // is affected: `smile_greeks` is off by default, so every prior caller keeps
  // the memo on exactly the rows it already had it on. Marks-only rows ignore
  // `bumps` entirely (see `price_deriv_book`'s own @param doc) and so keep the
  // memo regardless.
  const bool use_memo = var_swap_memo_eligible(p.contract, discrete_correction_ok) &&
                        !(greeks && bumps.smile_greeks);

  if (greeks) {
    const Result<DerivGreeks> g =
        use_memo ? detail::deriv_greeks_var_swap_on_ref_shared(
                       ref, p.contract, cfg, bumps,
                       var_swap_memo[var_swap_memo_key(p.uid, p.contract.maturity_t, wing_band)],
                       wing_band)
                : detail::deriv_greeks_on_ref(ref, p.contract, cfg, bumps, wing_band);
    if (!g.has_value()) {
      row.status = status_for(g.error());
      return row;
    }
    row.pv = p.qty * g->pv;
    row.fair_strike_dec = g->quote.fair_strike_dec;
    row.greeks = scaled_greeks(*g, p.qty);
    row.status = PriceStatus::Ok;
    return row;
  }

  const Result<DerivQuote> q =
      use_memo ? detail::deriv_price_var_swap_on_ref_shared(
                     ref, p.contract, cfg,
                     var_swap_memo[var_swap_memo_key(p.uid, p.contract.maturity_t, wing_band)],
                     wing_band)
              : detail::deriv_price_on_ref(ref, p.contract, cfg, wing_band);
  if (!q.has_value()) {
    row.status = status_for(q.error());
    return row;
  }
  row.pv = p.qty * q->pv;
  row.fair_strike_dec = q->fair_strike_dec;
  // The sensitivities stay NaN (not requested), but the centre quote is real
  // and carries the strip diagnostics a caller needs to audit this mark.
  row.greeks.quote = *q;
  row.status = PriceStatus::Ok;
  return row;
}

// Open the totals accumulator for the requested mode. Which columns are NaN is
// driven by the MODE, not by the row count: an empty greek-bearing book has a
// genuine zero delta, whereas a marks-only book has no delta at all.
[[nodiscard]] PriceTotals open_totals(bool greeks) noexcept {
  PriceTotals t{};
  if (greeks) {
    // `abs_vega` defaults to NaN ("not computed"); this reduction does compute
    // it, so open the accumulator explicitly (mirrors the option pricer's
    // reduce_price_totals).
    t.abs_vega = 0.0;
  } else {
    t.delta = t.gamma = t.vega = t.theta = t.rho = kNaN;
    t.vanna = t.volga = t.charm = kNaN; // abs_vega is already NaN by default
  }
  // `dP_dq` stays NaN unconditionally: the carry/borrow axis is defined on the
  // option pipeline's per-contract IV lane, and a swap has no such lane.
  return t;
}

// Accumulate one row. Non-Ok rows are EXCLUDED, not zeroed. A greek a lane
// itself reported as "not computed" (theta/charm on a contract too short to
// roll, vanna/charm under second_order=false) propagates its NaN into that
// column of the total -- the total is genuinely unknown, and a partial sum
// presented as complete is the failure mode being avoided. That NaN-poisoning
// is unchanged; GK-C9b adds the count of WHICH Ok rows caused it, per column,
// so a NaN total names its own cause instead of going silent.
//
// Task F-7: `maturity_t` is passed in rather than read off the row because
// neither `DerivPriceRow` nor the `DerivQuote` it embeds carries a tenor --
// the driver loop's own `p.contract` is the only place it exists at this
// point.
//
// NO LONGER `noexcept` (Task F-7): inserting a new tenor bucket ALLOCATES, so
// the old marking would have turned an out-of-memory into `std::terminate`
// instead of an exception. `price_deriv_book`, this function's only caller,
// already allocates freely (`rows.reserve`/`push_back`) and is not noexcept
// either, so propagating matches what the layer around it already does.
void accumulate(DerivPriceFrame &frame, const DerivPriceRow &row, bool greeks,
                double maturity_t) {
  if (row.status != PriceStatus::Ok) {
    return;
  }
  PriceTotals &t = frame.totals;
  t.pv += row.pv;
  if (greeks) {
    const DerivGreeks &g = row.greeks;
    t.delta += g.delta;
    t.gamma += g.gamma;
    const double leg_vega = g.vega;
    t.vega += leg_vega;
    t.abs_vega += std::fabs(leg_vega);
    // Term-bucket vega (Task F-7). A non-finite maturity is not a bucket any
    // caller could look up again, so such a lane is left out of the ladder
    // rather than allowed to create a NaN KEY -- which `std::map`'s `<` orders
    // inconsistently and would corrupt the whole container's invariants, not
    // just its own entry. `totals.vega` still carries the lane, so the ladder
    // summing short of the net total is the visible, honest signal.
    if (std::isfinite(maturity_t)) {
      frame.vega_by_tenor[maturity_t] += leg_vega;
    }
    t.theta += g.theta;
    frame.n_theta_excluded += std::isfinite(g.theta) ? 0u : 1u;
    t.rho += g.rho;
    t.vanna += g.vanna;
    frame.n_vanna_excluded += std::isfinite(g.vanna) ? 0u : 1u;
    t.volga += g.volga;
    t.charm += g.charm;
    frame.n_charm_excluded += std::isfinite(g.charm) ? 0u : 1u;
  }
  ++t.n_ok;
}

} // namespace

std::size_t DerivPriceFrame::n_ok() const noexcept {
  const auto n = std::count_if(rows.begin(), rows.end(), [](const DerivPriceRow &r) noexcept {
    return r.status == PriceStatus::Ok;
  });
  return static_cast<std::size_t>(n);
}

Result<DerivPriceFrame> price_deriv_book(const SurfaceSet &surfaces,
                                         std::span<const DerivPosition> book,
                                         const DerivConfig &cfg, bool greeks,
                                         const DerivGreekBumps &bumps,
                                         const WingBandResolver &wing_band_of) {
  // The only STRUCTURAL rejection: `PriceTotals::n_ok` is a uint32 counter, so a
  // book that cannot be counted in one would silently wrap. Refused before any
  // allocation (mirrors Portfolio::create's index-representability gate).
  if (book.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    return Err(ErrorCode::InvalidArgument, "deriv book: position count exceeds UINT32_MAX");
  }

  DerivPriceFrame frame;
  frame.rows.reserve(book.size());
  frame.totals = open_totals(greeks);
  // Task P-6 (GK-P book memo): resolved ONCE, book-wide -- see the key-field
  // audit above `var_swap_memo_eligible`'s own doc for why `cfg` as a whole,
  // and this field specifically, is never a per-row concern.
  const bool discrete_correction_ok = cfg.discrete_correction_mode == DerivDiscreteCorrection::None;
  VarSwapMemo var_swap_memo;
  // Serial, fixed input order: the float-add association is the book's own
  // order, so the totals are bit-reproducible for a given input. The memo
  // does not change this -- it changes HOW EXPENSIVELY a row's own numbers
  // are produced, never their value or the order they are produced in.
  for (const DerivPosition &p : book) {
    frame.rows.push_back(
        price_one(surfaces, p, cfg, greeks, bumps, wing_band_of, discrete_correction_ok, var_swap_memo));
    accumulate(frame, frame.rows.back(), greeks, p.contract.maturity_t);
  }
  return Ok(std::move(frame));
}

PriceTotals combine_totals(const PriceTotals &a, const PriceTotals &b) noexcept {
  PriceTotals out{};
  out.pv = a.pv + b.pv;
  out.delta = a.delta + b.delta;
  out.gamma = a.gamma + b.gamma;
  out.vega = a.vega + b.vega;
  out.abs_vega = a.abs_vega + b.abs_vega;
  out.theta = a.theta + b.theta;
  out.rho = a.rho + b.rho;
  out.vanna = a.vanna + b.vanna;
  out.volga = a.volga + b.volga;
  out.charm = a.charm + b.charm;
  out.dP_dq = a.dP_dq + b.dP_dq;
  // Saturating rather than wrapping: an overflowed lane count would understate
  // the priced population, which is worse than pinning it at the maximum.
  constexpr std::uint64_t kMaxOk =
      static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
  const std::uint64_t n = static_cast<std::uint64_t>(a.n_ok) + static_cast<std::uint64_t>(b.n_ok);
  out.n_ok = static_cast<std::uint32_t>(n < kMaxOk ? n : kMaxOk);
  return out;
}

} // namespace atx::vol
