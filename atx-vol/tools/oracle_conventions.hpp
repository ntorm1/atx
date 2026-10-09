#pragma once

// The isolated SpiderRock <-> atx-vol translation layer. Stage 3 resolves a
// single ConventionMap on aggregate smoke+tune data; production Mode A uses
// winning_convention() and never exposes a runtime convention flag.

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "oracle_cohort_reader.hpp"

namespace atx::vol::oracle {

enum class InputModel {
  CurrentSpotSdivYield,
  DiscreteDividendPvSdivYield,
  DiscreteForwardNetCarry,
  DiscreteForwardRateSdivYield,
  DiscreteForwardNetRate,
  DiscreteForwardZeroRates,
  DiscreteDividendPvNetRate,
  DiscreteDividendPvRatePlusSdiv,
  // The discrete-dividend ENGINE arm: spot stays uPrc (nothing is escrowed out
  // of it), rate = row.rate, carry = row.sdiv as the RESIDUAL continuous yield
  // (near-zero once ddiv carries the cash, but NOT negligible — zeroing it was
  // measured to cost calls 2.95 -> 72.55 ticks of price MAE), and the cash
  // dividends enter as a reconstructed per-snapshot schedule priced on the
  // Vellekoop-Nieuwenhuis spliced CRR lattice
  // (american_discrete_div_greek_bundle). A row with ddiv == 0 prices on the
  // continuous-carry engine instead: with no cash in (0, T] the two ARE the
  // same model, and the analytic pricer is both exact and cheaper there.
  // Appended LAST so no existing enumerator's value moves.
  DiscreteDividendTree,
};

enum class GreekSource { Delta, Gamma, Theta, Vega, Rho, CarryRho, Volga, Vanna, Charm };

// WHICH PRICER a row is entitled to — the exercise-style axis.
//
// It is a convention key and not a tuning knob because the underlying quantity
// is a property of the CONTRACT: an option that cannot be exercised early has
// no early-exercise premium, and pricing one through an American engine is
// simply the wrong function, not a mis-scaled one. On the aggregate smoke+tune
// population that mistake is the dominant price residual, because roughly half
// the rows are cash-settled European index options.
//
// It lives on the map (rather than being asserted in code) so the SWEEP picks
// it on measured evidence the way `input_model` is picked, and so the receipt
// names the rule that produced every published number.
//
// STAND-IN FOR DATA. Every rule below decides exercise style from the underlier
// ROOT, because the store carries no exercise-style / calcEngine column (see
// the `ExerciseStyle` banner in oracle_cohort_reader.hpp). `exercise_style_for`
// consults `OracleRow::ingested_exercise_style` FIRST, so the day such a column
// is ingested the rule stops deciding anything without a map change.
enum class ExerciseStyleRule {
  // Every row American. The historical behaviour, and the baseline arm every
  // published delta is measured against.
  AmericanAll,
  // The roots that are EUROPEAN-exercise, cash-settled index options by their
  // listing specification (kEuropeanIndexRoots in the .cpp names them and cites
  // what makes each one a contract fact). Nothing here is fitted.
  EuropeanCashSettledIndex,
  // The above PLUS any roots that only REPRODUCE European and cannot (yet) be
  // justified from a contract fact or an ingested column. The empirical table
  // is EMPTY today — MGTN, its only historical member, moved to
  // kEuropeanIndexRoots once the Cboe contract specification was located (the
  // .cpp carries the citation) — but the rule keeps its own identity so a
  // future measured, unexplained root lands here and its receipt says so:
  // "we measured this and cannot explain it" must never be mistaken for "this
  // is how the contract works". While the table is empty this rule routes
  // exactly the contract-fact set, and the sweep's deterministic identity
  // tie-break prefers the plain `european_cash_settled_index` id.
  EuropeanCashSettledIndexPlusEmpirical,
};

// HOW the two time-decay Greeks are FORMED — the time-decay axis.
//
// It is a convention key and not a tuning knob for the same reason
// `exercise_style` is: the two arms compute DIFFERENT QUANTITIES, not one
// quantity at two scales. A tangent and a secant over a finite step are not
// related by any multiplier, so no entry in the scale grid can turn one into
// the other, and a scale search asked to bridge them silently reports the
// closest wrong unit instead of the right quantity.
//
// THE EVIDENCE, two independent lines:
//   1. SpiderRock's own documentation says theta "is always measured
//      numerically, by calculating the difference between the current option
//      price and the option price calculated with volatility time decreased by
//      1/252 years", reported as a POSITIVE decay; `deDecay` is documented as
//      "how much the delta will change for a one-day decrease in time to
//      expiration" — the same finite difference taken on delta.
//   2. atx-vol/docs/LEDGER.md, 2026-08-18, recorded independently that theta
//      and deDecay "CARRY A BASIS ERROR, NOT A RESIDUAL": both moved the WRONG
//      way under standard-relative error while IMPROVING under the symmetric
//      one, which is the fingerprint of a basis/scale mismatch rather than of
//      a residual the fit could shrink.
//
// The vendor document explains the ledger entry: we report a scaled TANGENT
// where SpiderRock reports a SECANT. This axis puts both on the grid and lets
// the sweep answer on measured evidence, exactly as `exercise_style` does.
enum class TimeDecayMethod {
  // theta = the engine's analytic dP/dt jet times `theta_scale`, delta decay =
  // the analytic charm times `delta_decay_scale`. The historical behaviour, and
  // the baseline arm every published theta/deDecay number is measured against.
  AnalyticDerivative,
  // theta = P(T) - P(T - 1/252), delta decay = delta(T) - delta(T - 1/252),
  // both positive for a decaying position. `theta_scale` and
  // `delta_decay_scale` are INERT under this method — the secant is already a
  // one-day quantity, and multiplying it by a per-day scale would divide by a
  // day count twice.
  Secant252,
};

// The step `Secant252` differences over: ONE BUSINESS DAY out of 252, which is
// SpiderRock's own numeric theta step ("volatility time decreased by 1/252
// years"). Exposed so a test pins the same number the pricer bumps by instead
// of restating a literal that could drift from it.
inline constexpr double kTimeDecayStepYears = 1.0 / 252.0;

struct ConventionMap {
  InputModel input_model = InputModel::CurrentSpotSdivYield;
  // Which pricer each row is entitled to. Defaults to the historical
  // all-American behaviour so `baseline_convention()` keeps meaning exactly
  // what it meant before this axis existed.
  ExerciseStyleRule exercise_style = ExerciseStyleRule::AmericanAll;
  // How theta and delta decay are formed. Defaults to the historical analytic
  // derivative, for the same reason `exercise_style` defaults to AmericanAll:
  // `baseline_convention()` must keep meaning what it meant before the axis.
  TimeDecayMethod time_decay_method = TimeDecayMethod::AnalyticDerivative;
  double price_scale = 1.0;
  // Calendar days-to-expiry, used ONLY for the scorecard's 0-7/8-30/31-90/90+
  // banding. SpiderRock's theta day count is an unrelated convention, so the
  // sweep never writes this field: re-bucketing every band as a side effect of
  // a theta unit pick would silently change what the bands mean.
  double days_per_year = 365.0;
  // Day count implied by theta_scale; this is what the receipt reports as
  // `day_count`. Under `Secant252` it is implied by the METHOD instead — the
  // step is one business day out of 252 — because `theta_scale` is inert there.
  double theta_days_per_year = 365.0;
  double delta_scale = 1.0;
  double gamma_scale = 1.0;
  double theta_scale = 1.0 / 365.0;
  double vega_scale = 0.01;
  double rho_scale = 0.01;
  double phi_scale = 0.01;
  GreekSource volga_source = GreekSource::Volga;
  double volga_scale = 0.01;
  GreekSource vanna_source = GreekSource::Vanna;
  double vanna_scale = 0.01;
  double delta_decay_scale = 1.0 / 365.0;
};

[[nodiscard]] const ConventionMap &baseline_convention() noexcept;
[[nodiscard]] const ConventionMap &winning_convention() noexcept;
[[nodiscard]] std::string_view input_model_id(InputModel model) noexcept;
[[nodiscard]] std::string_view greek_source_id(GreekSource source) noexcept;
[[nodiscard]] std::string_view exercise_style_id(ExerciseStyleRule rule) noexcept;
[[nodiscard]] std::string_view time_decay_method_id(TimeDecayMethod method) noexcept;

// THE routing decision, in one place.
//
// PRECEDENCE, and the whole point of the seam: an ingested exercise style on
// the ROW wins outright; the map's root-list rule answers only while the row
// says Unknown. Today every row says Unknown, so today the rule always answers
// — which is exactly the state this function exists to make replaceable.
[[nodiscard]] ExerciseStyle exercise_style_for(const OracleRow &row,
                                               const ConventionMap &map) noexcept;

// Does `rule` route `underlier` to the European leg? Exposed so the root table
// is testable without synthesising a whole row.
[[nodiscard]] bool routes_european(std::string_view underlier, ExerciseStyleRule rule) noexcept;

struct EnginePricingInputs {
  double spot = 0.0;
  double strike = 0.0;
  double years = 0.0;
  double sigma = 0.0;
  double rate = 0.0;
  double carry = 0.0;
  Side side = Side::Call;
};

[[nodiscard]] EnginePricingInputs mode_a_inputs(const OracleRow &row,
                                                const ConventionMap &map) noexcept;
[[nodiscard]] EnginePricingInputs mode_a_inputs(const OracleRow &row) noexcept;

// ── The two pricing legs, behind one exercise-style-aware entry point ──────
//
// EVERY Mode A / sweep price goes through these, so the map's exercise-style
// key cannot be honoured in one place and forgotten in another.

// The EUROPEAN leg, in the SAME spot-based conventions `AmericanGreeks`
// carries, so a routed row's nine Greeks stay directly comparable to an
// unrouted row's.
//
// It is atx-vol's own cached-jet chain rule (american.cpp
// `american_greeks_first_order`) specialised to a ZERO early-exercise
// correction, evaluated on the public `black76_greeks` kernel — the same
// Black-76 leg the American path is built on top of, so the two legs cannot
// disagree about d1/d2, discounting, or the forward.
//
// It is deliberately NOT `american_greeks(..., (const CorrectionCache *)nullptr)`,
// whose documented null-cache contract also returns "the Black-76 leg": that
// path runs the served price through `floor_cached_price`, which floors at
// INTRINSIC. A European premium is entitled to sit below intrinsic and routinely
// does — the deep-ITM SPX put at S=7779.97, K=15000, T=5.38 is worth 4341.92
// against an intrinsic of 7220.03, and 4341.92 is what srPrc reports. Flooring
// it would have reintroduced the exact $2,878 error this axis exists to remove.
//
// `dp_dq_out` (optional) receives the carry sensitivity on the same
// specialisation: q enters only through F, so dP/dq = -T*F*(dP/dF).
//
// @return InvalidArgument on non-positive S/K/T/sigma — `american_greeks_al`'s
//         contract, so a row's admission does not depend on which leg it took.
[[nodiscard]] Result<AmericanGreeks> european_greeks(double S, double K, double T, double sigma,
                                                     double r, double q, Side side,
                                                     double *dp_dq_out = nullptr) noexcept;

// The INVERSE of `european_greeks`'s price: implied volatility measured from a
// EUROPEAN premium, for the rows `exercise_style_for` routes European. Declared
// beside its forward map for the same reason `price_from_oracle_units` sits
// beside `price_to_oracle_units`: both build F = S*e^{(r-q)T} and df = e^{-rT}
// from the same inputs, and a Mode B that derived them locally could drift from
// the leg it re-prices with.
//
// It is the library's closed-form Black-76 inverse (implied_vol.hpp:
// Stefanica-Radoicic seed + Halley polish) on exactly that forward/discount
// pair — the inverse of the same `black76_greeks` kernel `european_greeks`
// prices with, so the round trip closes to machine precision by construction.
//
// ADMISSION mirrors `american_implied_vol` leg-for-leg, with the EUROPEAN
// no-arbitrage band: the lower bound is the DISCOUNTED FORWARD intrinsic
// df*max(F-K, 0) (mirrored for puts) and the ceiling is df*F (call) / df*K
// (put). There is NO early-exercise floor — a deep-ITM premium below IMMEDIATE
// intrinsic is a legitimate European quote and inverts.
//   InvalidArgument — S/K/T <= 0 (the shared-input slice of the forward map's
//                     own contract; sigma is the unknown here)
//   OutOfRange      — non-finite input, or price outside the band
//   Unavailable     — deep-wing / near-expiry vega collapse
// A price at (or below) the discounted forward intrinsic reports Ok(kIvMin) —
// the library's documented clamp, NOT a measurement. Mode B never publishes it:
// its lower-bound screen refuses such a mid before the inverter runs, and its
// floor-clamp screen refuses any residual kIvMin result.
[[nodiscard]] Result<double> european_implied_vol(double price, double S, double K, double T,
                                                  double r, double q, Side side) noexcept;

// The INVERSE of the `DiscreteDividendTree` price: implied volatility measured
// from a premium under the V&N spliced-CRR lattice on the row's reconstructed
// cash-dividend schedule. Declared beside the convention layer's other inverse
// (`european_implied_vol`) for the same reason that one sits beside its forward
// map: the forward map here IS `mode_a_price`'s tree leg —
// `american_discrete_div_price` at `kDiscreteDivDefaultSteps` with the map's
// exercise rollback — so a Mode B that root-found a locally built lattice could
// drift from the leg it re-prices with.
//
// CONTRACT — `american_implied_vol`'s, leg for leg, with the same constants
// (american_iv.cpp): the search bracket floors at kIvMin and expands
// geometrically from 5.0 to a hard 40.0 ceiling; `tol` is the convergence
// tolerance in volatility units and `max_iter` the bounded-loop cap. The solver
// is a safeguarded secant (Illinois regula falsi) inside a maintained sign
// bracket — the lattice offers no cheap vega for a Newton step, and the price
// is monotone in sigma, so bracketed superlinear convergence is the right tool.
// `warm_start` (> 0, in-bracket) seeds the bracket from a prior nearby sigma;
// like the American inverter's, it moves the search path, never the root.
//
// `style` is the ORACLE routing vocabulary (`exercise_style_for`'s result);
// Unknown rolls back American, exactly as the forward map's `engine_exercise`
// maps it. Under an American rollback the admission band is verbatim
// `american_implied_vol`'s: price below immediate intrinsic / above the S-or-K
// ceiling is OutOfRange, price AT intrinsic reports Ok(kIvMin) — the
// documented clamp, not a root, which Mode B's floor screen refuses. A
// European rollback carries no intrinsic floor (a deep-ITM premium below
// immediate intrinsic legitimately inverts) and relies on the bracket itself:
//   InvalidArgument — S/K/T <= 0, or a schedule the pricer rejects
//   OutOfRange      — non-finite input, price outside the band, or a price
//                     above the max-vol lattice price
//   Unavailable     — no convergence in max_iter
//   Internal        — allocation failure (contained; the entry is noexcept)
// A price at/below the lattice's own kIvMin value reports Ok(kIvMin) — the
// same floor clamp both continuous inverters document. Mode B never publishes
// it: the floor-clamp screen refuses any kIvMin-adjacent result.
[[nodiscard]] Result<double>
discrete_tree_implied_vol(double price, double S, double K, double T, double r, double q,
                          Side side, std::span<const CashDividend> schedule, ExerciseStyle style,
                          double tol = 1.0e-7, std::uint16_t max_iter = 64,
                          double warm_start = 0.0) noexcept;

// One priced row: the inputs used, the nine raw engine Greeks, the carry
// sensitivity, and WHICH leg produced them. `dp_dq` is non-finite when the
// carry solve refused — only the phi metric reads it, so the row keeps its
// other eight Greeks (the American path's long-standing behaviour, preserved).
struct ModeAPricing {
  EnginePricingInputs inputs{};
  AmericanGreeks greeks{};
  double dp_dq = 0.0;
  ExerciseStyle style = ExerciseStyle::American;
  // The ONE-BUSINESS-DAY SECANTS, populated only under
  // `TimeDecayMethod::Secant252` and left non-finite otherwise — there is no
  // bumped valuation to report when the map never asked for one, and a 0.0
  // sentinel would read as a measured "no decay".
  //
  //   theta_secant       = P(T) - P(T - 1/252)
  //   delta_decay_secant = delta(T) - delta(T - 1/252)
  //
  // Both positive for a decaying position, matching SpiderRock's positive
  // decay convention. A bumped valuation that refuses leaves them non-finite
  // rather than discarding the row — exactly `dp_dq`'s policy above — so a
  // corner-regime row still reports its other seven Greeks.
  double theta_secant = std::numeric_limits<double>::quiet_NaN();
  double delta_decay_secant = std::numeric_limits<double>::quiet_NaN();
};

// The per-row DIVIDEND CONTEXT the `DiscreteDividendTree` input model prices
// on. The pricer sees one row at a time while a schedule is a property of a
// whole chain, so the caller resolves the row's snapshot schedule in a
// pre-pass (DividendScheduleIndex, oracle_dividends.hpp) and hands it in here.
// Deliberately just a span + flag — this layer never depends on the
// reconstructor, so targets that compile only the convention TUs still link.
//
// `refused = true` means reconstruction could not vouch for the row's
// snapshot: the tree model then REFUSES the row (Err) rather than silently
// pricing it on a different model and scoring it as if it were tree-priced.
// Every other input model ignores this struct entirely.
struct RowDividends {
  std::span<const CashDividend> schedule{};
  bool refused = false;
};

// The tree model's admission rule for one row, decided on the row's OWN
// evidence (`ddiv` is the vendor's statement that cash lands at or before this
// expiry — the accrual identity against the reconstructed schedule was
// measured to 1.8e-15):
//   Err        — reconstruction refused the snapshot, or the row claims cash
//                (ddiv != 0) the supplied schedule cannot account for. FAIL
//                CLOSED: never a silent fallback to a different model.
//   Ok(true)   — cash lands in (0, years]: price (or invert) on the lattice.
//   Ok(false)  — ddiv == 0: no cash in the option's life, the tree IS the
//                continuous-carry engine, and the analytic legs are exact.
// Exposed (it was file-private inside mode_a_price_row) so Mode B's inversion
// routing makes exactly the same admission decision as Mode A's pricing — one
// rule, two consumers, no way to drift.
[[nodiscard]] Result<bool> tree_admits_lattice(const OracleRow &row,
                                               const RowDividends &dividends);

// Prices one row end to end under `map`: `mode_a_inputs` for the inputs,
// `exercise_style_for` for the leg. Err exactly where the chosen leg errs, so
// `rows_engine_error` keeps counting the same thing.
//
// Under `TimeDecayMethod::Secant252` it additionally evaluates the BUMPED leg
// one business day closer to expiry and fills `theta_secant` /
// `delta_decay_secant`. That second valuation asks for price+delta only
// (`american_greeks_al`'s reduced first-order tier), so it costs one boundary
// solve rather than five. A refusal there leaves the two secants non-finite and
// does NOT fail the row.
//
// Under `InputModel::DiscreteDividendTree` a row with ddiv > 0 prices on the
// V&N spliced lattice (american_discrete_div_greek_bundle, 8 rollbacks; the
// map's exercise style selects the lattice's own American/European rollback),
// `dividends.schedule` supplies the cash events, and dp_dq is the bundle's phi.
// FAIL CLOSED: `dividends.refused`, or a ddiv > 0 row whose schedule carries no
// ex-date in (0, years], is an Err — never a silent fallback to another model.
// A ddiv == 0 row takes the continuous-carry legs below unchanged (same model
// when no cash lands in the option's life, and exact rather than discretized).
// Its Secant252 bump re-solves the LATTICE at T - 1/252 with the same schedule
// (events past the bumped expiry drop, per the engine's own (0, T] window).
[[nodiscard]] Result<ModeAPricing> mode_a_price_row(const OracleRow &row, const ConventionMap &map,
                                                    const std::optional<AlOpts> &opts,
                                                    const RowDividends &dividends);

// Convenience overload for callers with no schedule pre-pass. Identical for
// every model except `DiscreteDividendTree`, where it admits only ddiv == 0
// rows (their schedule is provably empty on the row's own evidence) and
// refuses ddiv != 0 rows — deriving a chain schedule from one row is exactly
// the silent wrongness the explicit overload exists to prevent.
[[nodiscard]] Result<ModeAPricing> mode_a_price_row(const OracleRow &row, const ConventionMap &map,
                                                    const std::optional<AlOpts> &opts);

// Price only, for the sweep's stage-1 input/exercise cut, which has no Greek to
// attribute. Same routing, same inputs, ~5x cheaper on the American leg because
// no boundary re-solves are requested; the tree arm's ddiv > 0 rows cost ONE
// lattice rollback (american_discrete_div_price) instead of the bundle's eight.
[[nodiscard]] Result<double> mode_a_price(const OracleRow &row, const ConventionMap &map,
                                          const std::optional<AlOpts> &opts,
                                          const RowDividends &dividends);

// Same convenience contract as the mode_a_price_row overload above.
[[nodiscard]] Result<double> mode_a_price(const OracleRow &row, const ConventionMap &map,
                                          const std::optional<AlOpts> &opts);
[[nodiscard]] double dte_days(double years, const ConventionMap &map) noexcept;
[[nodiscard]] double dte_days(double years) noexcept;
[[nodiscard]] double price_to_oracle_units(double engine_price, const ConventionMap &map) noexcept;
[[nodiscard]] double price_to_oracle_units(double engine_price) noexcept;

// The INVERSE map, which Mode B needs: raw NBBO arrives in ORACLE price units
// and must reach the engine's inverter in ENGINE units. Declared beside its
// forward so the two cannot drift — a Mode B that divided by a locally written
// literal would disagree with the pinned map the moment price_scale moves.
// Returns a non-finite value on a zero/non-finite scale instead of inventing
// one; callers screen finiteness at the boundary.
[[nodiscard]] double price_from_oracle_units(double oracle_price,
                                             const ConventionMap &map) noexcept;
[[nodiscard]] double price_from_oracle_units(double oracle_price) noexcept;

struct OracleUnitGreeks {
  double de = 0.0;
  double ga = 0.0;
  double th = 0.0;
  double ve = 0.0;
  double rh = 0.0;
  double ph = 0.0;
  double vo = 0.0;
  double va = 0.0;
  double de_decay = 0.0;
};

// The ANALYTIC-DERIVATIVE form. An `AmericanGreeks` jet carries no bumped
// valuation, so this overload cannot honour `Secant252` and does not pretend
// to: it asserts `map` is on the analytic arm rather than silently scaling a
// tangent where the map asked for a secant. Any caller holding a `ModeAPricing`
// must use the overload below, which honours the axis.
[[nodiscard]] OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq,
                                               const ConventionMap &map) noexcept;
[[nodiscard]] OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq) noexcept;

// THE axis-aware conversion, and the one every Mode A / sweep reporting path
// should call. Under `AnalyticDerivative` it is the overload above; under
// `Secant252` it publishes `theta_secant` / `delta_decay_secant` RAW, with
// `theta_scale` and `delta_decay_scale` deliberately NOT applied — the secant
// is already the one-day quantity, and applying a per-day scale on top of it
// would divide by a day count twice.
[[nodiscard]] OracleUnitGreeks to_oracle_units(const ModeAPricing &priced,
                                               const ConventionMap &map) noexcept;
[[nodiscard]] OracleUnitGreeks to_oracle_units(const ModeAPricing &priced) noexcept;

} // namespace atx::vol::oracle
