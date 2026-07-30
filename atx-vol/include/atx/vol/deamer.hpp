#pragma once

// De-Americanization pipeline: American equity-option market quotes ->
// European-equivalent implied vols (+ a per-term implied borrow cost).
//
// This is the input the surface calibrators (SVI/eSSVI/C8/CStar) expect: a
// clean (log-moneyness, European IV) strip per expiry, on a forward that is
// consistent with the borrow the market is actually pricing. It is the Vola
// Dynamics workflow (Klassen 2017, slide 26):
//
//   1. pick a rate            (yield curve / passed-in r)
//   2. pick cash dividends    (DividendEvent schedule, hybrid dividend model)
//   3. imply the borrow       per term via put-call parity (this module)
//   4. imply vol-by-strike    invert each American quote to a European-equiv IV
//   5. fit                    hand the strip to the surface calibrator
//
// ## Where this sits in the seam
//
// `american_iv.hpp` recovers the lognormal sigma that reprices an American
// premium *through the American pricer*. That recovered sigma IS the
// European-equivalent implied vol: the pricer's forward is European
// (S·e^{(r−q)T}), so the vol that reprices the American premium is by
// construction the vol a European option on that same forward would carry.
// This module is the thin orchestration layer above that primitive.
//
// `dividend.hpp` owns the hybrid forward and the *European* put-call-parity
// borrow solve, and deliberately deferred the American-quote case to here
// (see its PORT NOTE): for American options PCP is a band, not an equality, so
// a borrow implied directly from American mids is biased by the early-exercise
// premium. This module closes that seam by de-Americanizing both legs first
// (American premium -> European-equivalent premium) and only then invoking the
// European PCP solve.
//
// ## The q_eff bridge (load-bearing, documented once here)
//
// The hybrid forward can embed discrete cash dividends, so it is *not* of the
// form S·e^{(r−q)T} for a single continuous yield q. The American pricer,
// however, takes a continuous carry q and prices off S·e^{(r−q)T}. To make the
// pricer's internal forward equal the discrete-dividend forward F we pass an
// *effective* carry
//
//     q_eff = r − ln(F / S) / T          =>   S·e^{(r−q_eff)T} = F   (identity)
//
// so a single scalar q_eff reproduces the discrete-div forward exactly for the
// (single-expiry) inversion. Borrow enters F through the hybrid forward, hence
// through q_eff; a positive borrow lowers F and therefore raises q_eff.
//
// ## OTM-side selection (documented once here)
//
// At each strike we invert only the out-of-the-money leg: the CALL for
// k = ln(K/F) >= 0 and the PUT for k < 0. OTM options carry the least
// early-exercise premium, so the American pricer's early-exercise correction
// is smallest exactly where we invert — the recovered European-equivalent vol
// is the best-conditioned and least model-dependent there. `otm_side` exposes
// this rule.
//
// Stateless and pure — every entry is safe to call concurrently from any
// thread. Cost is dominated by the cold American pricer/inverter, so this is a
// cold-path (surface-fit cadence) routine, not a per-tick kernel.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"   // AmericanMethod, AlOpts
#include "atx/vol/correction.hpp" // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/curve.hpp"      // DividendEvent
#include "atx/vol/dividend.hpp"   // HybridDivParams, hybrid_forward, PCP borrow
#include "atx/vol/types.hpp"      // Side, Result / ErrorCode (atx::core)
#include "atx/vol/universe.hpp"   // Chain, chain_index

namespace atx::vol {

// Out-of-the-money side at log-moneyness k = ln(K/F): CALL for k >= 0, PUT for
// k < 0. The OTM leg carries the least early-exercise premium and inverts most
// cleanly to a European-equivalent vol (see header rationale). ATM (k == 0)
// resolves to Call by convention.
[[nodiscard]] constexpr Side otm_side(double k_log) noexcept {
  return (k_log >= 0.0) ? Side::Call : Side::Put;
}

// ── Single-quote conversions ────────────────────────────────────────────

// European-equivalent implied vol of a single American quote.
//
// The recovered lognormal sigma (via american_implied_vol) IS the
// European-equivalent vol: the same American pricer is the forward map, so the
// round-trip is self-consistent and the recovered vol reprices a European
// option on the pricer's forward S·e^{(r−q_eff)T}. `q_eff` is the effective
// carry that makes that forward equal the term forward F (see the q_eff bridge
// in the header); the caller derives it as r − ln(F/S)/T so borrow and cash
// dividends are already folded in.
//
// @param american_mid observed American premium (inside the no-arb band)
// @param S,K          spot and strike (> 0)
// @param T            year-fraction to expiry (> 0)
// @param r            continuously-compounded rate (finite)
// @param q_eff        effective continuous carry making S·e^{(r−q_eff)T} == F
// @param side         Call or Put (typically the OTM leg; see otm_side)
// @param method       cold pricer used as the forward map
// @param opts         Andersen-Lake accuracy preset (AndersenLake only)
// @return             the European-equivalent IV, or any error propagated by
//                     american_implied_vol (InvalidArgument / OutOfRange /
//                     Unavailable, or a pricer error).
// `tol` / `max_iter` are the American-IV Newton controls (defaults mirror
// american_implied_vol's 1e-7 / 64). Keep `tol` at/above the pricer's own price
// accuracy: a tol tighter than the (fast-preset) pricer can resolve turns the
// safeguarded Newton into a bisection stall, each step a full American solve.
[[nodiscard]] atx::core::Result<double>
european_equiv_iv(double american_mid, double S, double K, double T, double r, double q_eff,
                  Side side, AmericanMethod method = AmericanMethod::AndersenLake,
                  const std::optional<AlOpts> &opts = std::nullopt,
                  const CorrectionCache *correction = nullptr, double tol = 1.0e-7,
                  std::uint16_t max_iter = 64) noexcept;

// Per-term borrow implied from a co-terminal near-ATM American call+put.
//
// De-Americanize both legs (American premium -> European-equivalent premium:
// invert each to its recovered vol, then reprice European at that vol on the
// term forward), then solve the *European* put-call parity for the borrow via
// imply_borrow_european_pcp. Because the forward the legs are de-Americanized
// on itself depends on the borrow (through q_eff), this is a bounded
// fixed-point: borrow -> F -> q_eff -> recovered vols -> European mids ->
// borrow. The injected-borrow round-trip is an exact fixed point of that map.
//
// This is the American-PCP-band-aware borrow the dividend module deliberately
// deferred to here: de-Americanize first, then apply the European PCP equality.
struct TermBorrow {
  double borrow = 0.0;     // implied continuous borrow (as in hybrid_forward)
  double forward = 0.0;    // hybrid_forward at the implied borrow
  double rmse_pcp = 0.0;   // |European PCP residual| at the solution (diagnostic)
  double sigma_call = 0.0; // recovered call vol at convergence (cross-pair warm seed)
  double sigma_put = 0.0;  // recovered put vol at convergence (cross-pair warm seed)
};

// Independent cold-reference audit of an implied-vol proposal.  Approximate
// pricers and correction caches may propose a sigma, but only the accurate
// Andersen-Lake forward map certifies it for risk calibration.
struct IvRepricingAudit {
  double accurate_price{0.0};
  double abs_residual{0.0};
  double residual_half_spreads{0.0};
  bool passed{false};
};

[[nodiscard]] atx::core::Result<IvRepricingAudit>
audit_european_equiv_iv(double american_mid, double bid_ask_spread, double sigma, double S,
                        double K, double T, double r, double q_eff, Side side,
                        double max_residual_half_spreads = 0.25) noexcept;

// Batched sibling of audit_european_equiv_iv (perf-review F3 / P3). Reprices
// MANY audited rows that share one (S, T, r, q_eff, side) but each carry their
// OWN recovered σ, in a single call.
//
// audit_european_equiv_iv does ONE ACCURATE-preset cold Andersen-Lake solve
// (12-node boundary / 24 fp-quad / 48-node premium — the library's most
// expensive scheme) PER row. Every row of one (expiry, side) shares
// (S, T, r, q_eff) and differs only in (K, σ) — exactly the shape of the
// σ-boundary-interpolant slice route. This entry routes the reprice through
// andersen_lake_{call,put}_slice_sigma: ONE n_σ-node Chebyshev interpolant of
// the dimensionless early-exercise boundary shared by the whole side, so the
// per-side cost drops from O(strikes) cold solves to O(n_σ) (≈ 8). The premium
// quadrature is UNCHANGED — the slice route runs the same ACCURATE-preset
// 48-node quadrature (scheme_from_opts(nullopt)); only the boundary PATH becomes
// σ-interpolated instead of per-row cold-solved. For a side with ≤ n_σ rows, or
// any row whose σ falls outside the box/guards, the route degrades to the
// bit-identical cold solve, so those verdicts are unchanged to the last bit.
//
// POLICY (PM-decided, perf-review F3): this remains a valid independence check.
// The audit certifies IV-INVERSION consistency (does the recovered σ reprice the
// American mid inside the half-spread budget?), NOT boundary-PATH independence.
// The σ-interpolant's qualified maximum price gap versus a per-row cold solve is
// 3.8e-5/share (Task 11 §P2.5 ship gate, american.hpp), orders of magnitude
// below the half-spread economic budget the audit is measured against, so no
// verdict the budget can resolve is affected.
//
// The verdict math is bit-identical to the scalar entry: per row `out[i]` is
// Ok({price, |price − mid|, |price − mid| / (0.5·spread), passed}) or the same
// Err the scalar returns on an invalid row / non-finite reprice. All spans must
// have equal length; `out` is written one Result per input row.
//
// @return InvalidArgument on a shared-input or span-length problem (no row is
//         written); otherwise Ok() with every `out[i]` populated.
[[nodiscard]] atx::core::Status audit_european_equiv_iv_batch(
    double S, double T, double r, double q_eff, Side side, std::span<const double> strikes,
    std::span<const double> sigmas, std::span<const double> american_mids,
    std::span<const double> bid_ask_spreads, double max_residual_half_spreads,
    std::span<atx::core::Result<IvRepricingAudit>> out) noexcept;

// @param call_mid,put_mid co-terminal American call/put mids at (K, T)
// @param S,K,T            spot, strike, year-fraction (all > 0)
// @param r                continuously-compounded rate (finite)
// @param cash_divs        discrete cash dividend schedule (hybrid_forward)
// @param expiry_ns,now_ts_ns option expiry / valuation timestamp (epoch ns)
// @param hyb              blend + proportional-yield hybrid-forward params
// @param method,opts      cold pricer + accuracy preset for the inversions
// @return                 the term borrow bundle, or an error:
//                           InvalidArgument — non-finite/non-positive input
//                           OutOfRange      — implied borrow outside the PCP
//                                             solver bracket
//                           Unavailable     — fixed point did not converge
//                         (plus any pricer/inverter error propagated).
[[nodiscard]] atx::core::Result<TermBorrow> imply_term_borrow(
    double call_mid, double put_mid, double S, double K, double T, double r,
    std::span<const DividendEvent> cash_divs, std::int64_t expiry_ns, std::int64_t now_ts_ns,
    const HybridDivParams &hyb, AmericanMethod method = AmericanMethod::AndersenLake,
    const std::optional<AlOpts> &opts = std::nullopt, const AmericanCorrectionCaches &caches = {},
    double borrow_seed = 0.0, double sigma_c_seed = 0.0, double sigma_p_seed = 0.0,
    bool skip_redundant_final = false) noexcept;

// ── Chain driver ────────────────────────────────────────────────────────

struct DeAmOptions; // forward decl for resolve_chain_forward

// Just the per-term (forward, borrow) for a chain: the borrow-implication
// front half of de_americanize_chain WITHOUT the per-strike inversion strip.
// A surface build that rebuilds its own per-strike observations (run_surface_
// parity) needs only (forward, borrow) from this step; calling the full
// de_americanize_chain there would invert every strike a second, wasted time.
struct CarryPairDiagnostic {
  std::uint16_t strike_index{0};
  double strike{0.0};
  double borrow{0.0};
  double forward{0.0};
  double pcp_residual{0.0};
  double relative_spread{0.0};
  double age_seconds{0.0};
  double base_weight{0.0};
  double robust_weight{0.0};
  bool retained{false};
};

// Provenance of a per-expiry carry (borrow). A `Solved` carry is inferred
// directly from THIS expiry's own co-terminal put/call pairs. The two fallback
// values are stamped by the board-level term-structure repair pass (Decision B,
// bt-hotpath sprint): when an expiry's own solve is not confident under the risk
// build, its borrow is DERIVED from the borrow-vs-T structure of the board's
// CONFIDENT expiries rather than the expiry being dropped. The distinction is
// load-bearing — a fallback carry must never be laundered downstream as a solved
// one (certificates read `source`, not just `confident`).
enum class CarrySource : std::uint8_t {
  Solved = 0,          // borrow inferred from this expiry's own co-terminal pairs
  TermStructureInterp, // fallback: linearly interpolated between bracketing confident expiries
  TermStructureExtrap, // fallback: flat-extended from the nearest confident expiry
};

// Carry is a measured input, not an invisible scalar.  These diagnostics make
// the robustness and single-pair sensitivity available to the admission layer.
struct CarryDiagnostics {
  std::vector<CarryPairDiagnostic> pairs;
  std::size_t n_candidates{0};
  std::size_t n_attempted{0};
  std::size_t n_solved{0};
  std::size_t n_retained{0};
  double effective_pair_count{0.0};
  double dispersion{0.0};
  double max_leave_one_out_shift{0.0};
  double confidence_half_width{0.0};
  double max_pcp_residual{0.0};
  bool confident{false};
  // Decision B provenance. Defaults to Solved so every existing construction /
  // serialized carry keeps its meaning; only the board-level repair pass stamps
  // a fallback value. `confident` is NEVER set true for a fallback carry.
  CarrySource source{CarrySource::Solved};
};

struct ChainForward {
  double forward = 0.0; // hybrid_forward at the resolved borrow
  double borrow = 0.0;  // implied (or fixed) per-term borrow
  CarryDiagnostics carry{};
};

[[nodiscard]] atx::core::Result<ChainForward>
resolve_chain_forward(const Chain &chain, double S, double r,
                      std::span<const DividendEvent> cash_divs, std::int64_t now_ts_ns,
                      const DeAmOptions &opts) noexcept;

// Strike indices of the co-terminal pairs ELIGIBLE for the robust carry solve
// — exactly the selection `resolve_chain_forward` makes (both legs quotable,
// the k nearest to spot with k = min(max(n_atm, in-band count),
// max_borrow_pairs, n_valid)). Ordered by ascending |K − S|. Empty when carry
// is fixed (imply_borrow == false), the chain is degenerate, or no pair is
// quotable. Exposed so the certified observation cache can invalidate on ANY
// change to a carry-relevant quote (price, spread, timestamp, or eligibility),
// including pairs the nearest-pair fallback selects outside the ATM band.
[[nodiscard]] std::vector<std::uint16_t> carry_pair_strikes(const Chain &chain, double S,
                                                            const DeAmOptions &opts);

struct DeAmOptions {
  HybridDivParams hyb{};
  AmericanMethod method = AmericanMethod::AndersenLake;
  std::optional<AlOpts> al_opts = std::nullopt;
  // Carry inference is an economically 1e-4 input, independent of the served
  // per-strike de-Am preset. The fast AL scheme holds that bound without paying
  // accurate-query quadrature for every fixed-point leg.
  std::optional<AlOpts> carry_al_opts = al_fast_opts();
  // Perf 2b: the AL preset BAKED INTO the fitted surface's pricing config
  // (VolaSession::to_priced_surface -> PricedSurfacePricing::al_opts), which is what
  // a QUERY re-prices with. Empty (the default) means "same as `al_opts`" — the
  // historical behaviour, bit-for-bit.
  //
  // WHY IT IS SEPARABLE. `al_opts` above drives the FIT: the de-Am inversion, the
  // correction-cache sampling, the diagnostic/parity passes. A bulk-populate tier
  // wants the cheapest Andersen-Lake rung that still clears the inversion's
  // economic budget there (docs/al-preset-ladder.md §5, fit-de-Am tier), and the
  // cheapest rung uses the DECOUPLED premium order `n_quad_price` — which the
  // surface archive, the v1 archive and the surface-db symbol record all decompose
  // AlOpts field-by-field WITHOUT (see surface_archive.cpp al_n_collocation /
  // al_n_quadrature / al_max_newton_iter). Baking a decoupled rung would therefore
  // read back with `n_quad_price = 0` — silently TIED to the cheap fixed-point
  // order — and serve queries from an 8-node premium quadrature nobody asked for.
  // Pinning the serve rung separately keeps the stored pricing config inside the
  // fields that actually round-trip, so the fit gets the cheap rung and the served
  // surface keeps today's. Which rung FITTED the surface is recorded where it
  // belongs, in the manifest's per-symbol `preset` byte.
  std::optional<AlOpts> serve_al_opts = std::nullopt;
  bool imply_borrow = true;         // if false, use borrow_fixed
  double borrow_fixed = 0.0;        // borrow used when imply_borrow == false
  std::size_t n_atm = 3;            // near-ATM co-terminal pairs used for the borrow
  std::size_t max_borrow_pairs = 5; // real-OPRA accuracy plateau; 1 = fastest
  std::size_t min_confident_borrow_pairs = 3;
  double carry_atm_band = 0.06;           // |K/S - 1| adaptive band
  double max_carry_dispersion = 0.02;     // annualized borrow-rate units
  double max_carry_leave_one_out = 0.005; // annualized borrow-rate units
  bool require_carry_confidence = false;  // compatibility seam; admission may require it
  // American-IV inversion Newton controls for the per-strike de-Am solve. The
  // default (1e-7 / 64) matches american_implied_vol and keeps the cold path
  // bit-identical; the fast-preset session loosens `iv_tol` to the fast pricer's
  // accuracy floor so the inversion converges by Newton, not bisection stall.
  double iv_tol = 1.0e-7;
  std::uint16_t iv_max_iter = 64;
  // Every cache/fast-pricer IV is audited against the cold accurate
  // Andersen-Lake forward map.  A failed proposal is recomputed accurately;
  // a node is dropped if even the fallback exceeds this half-spread budget.
  double max_iv_residual_half_spreads = 0.25;
  // Audit the FIT-observation inversions of the aligned-obs (eSSVI) surface
  // path against the cold Andersen-Lake reference (charter §8.1: every
  // shortcut/cache route needs a cold-reference audit before its output may be
  // certified): a failed proposal is recomputed accurately and re-audited, and
  // a row that still misses `max_iv_residual_half_spreads` is DROPPED, never
  // fitted. Default false keeps the historical fit bit-identical; the risk
  // serving policy enables it so no served surface carries an unaudited
  // inversion. AndersenLake only — other methods have no audit and can never
  // be certified.
  bool audit_fit_inversions = false;
  // Fast carry solve for the robust term-borrow (resolve_chain_carry). Two
  // coupled American-solve reductions, both opt-in:
  //   (1) cross-pair warm start — seed each near-ATM pair's borrow fixed-point +
  //       inner-Newton from the previous pair's converged state (pairs visited in
  //       ascending |K-S| order, so adjacent borrows/vols are nearly equal). Cuts
  //       fixed-point iterations, most on hard-to-borrow names whose borrow is far
  //       from the cold 0 seed.
  //   (2) skip the redundant post-convergence self-consistent de-Am step
  //       (2 American solves/pair): the reported forward is
  //       hybrid_forward(converged borrow) — bit-identical to that step's forward
  //       — so only the diagnostic rmse_pcp / returned warm-seed vols move by
  //       < kBorrowFpTol=1e-8.
  // Default TRUE (P2 / perf F2): both reductions ship on by the default carry
  // solve. Cross-pair seeds change only the fixed-point/Newton starting guesses,
  // so the converged borrow, forward, and per-leg vols move by < kBorrowFpTol=1e-8
  // (the fixed-point tolerance) — a sub-tolerance shift in load-bearing outputs and
  // in the diagnostic rmse_pcp / warm-seed vols. A caller that needs the exact
  // legacy carry diagnostics (bit-exact rmse_pcp / seed vols / same iteration path)
  // opts back out with false. The pricer stays uncached Andersen-Lake throughout.
  bool warm_start_carry = true;
  // C2 (perf): reuse caller-supplied `caches` for the eSSVI FIT de-Am instead of
  // building fresh per-side session caches. Set by the cross-date warm-start chain
  // (corpus.cpp) after its stale-gate confirms the prior date's cache covers this
  // board's (k_log, T) box at a compatible baked carry; VolaSession::build then
  // SKIPS build_session_caches (the ~192-solve/board rebuild, finding 11) and uses
  // the supplied caches. Default false keeps every fit building its own caches
  // (bit-identical to pre-C2). Ignored unless `caches` has a populated side.
  bool reuse_supplied_caches = false;
  // C2 (perf): build the per-side correction cache over a WIDER (k_log, T) box so
  // it stays reusable across a symbol's date chain — a later date's front expiry
  // shrinks below the default tight T box (0.9*T_lo) and its strikes drift with
  // spot, which would otherwise fail the reuse stale-gate. Only the FIRST cold
  // build of a chain (and any stale-gate-miss rebuild) sets this; the amortized
  // wider cache then covers several forward dates. Default false keeps the tight
  // production box (bit-identical cold path). The wider box is gated by the C2
  // quality-parity suite (it must keep every date in-band vs the tight cold fit).
  bool chain_cache_mode = false;
  // Optional per-side hot-path caches for the per-strike chain driver. Carry
  // inference deliberately stays uncached and uses carry_al_opts so query-cache
  // approximation error cannot bias the term forward.
  AmericanCorrectionCaches caches{};
};

struct DeAmResult {
  double forward = 0.0;          // per-term forward used
  double borrow = 0.0;           // implied or fixed
  std::vector<double> k_log;     // ln(K/F) per surviving strike
  std::vector<double> iv;        // European-equivalent IV per chosen (strike,side)
  std::vector<double> weight;    // vega/spread weight hint (parallel to iv)
  std::size_t n_used = 0;        // surviving strikes inverted
  std::size_t n_dropped = 0;     // strikes skipped (bad quote / failed invert)
  std::size_t n_iv_audited = 0;  // cache/fast proposals cold-reference checked
  std::size_t n_iv_fallback = 0; // failed proposals recomputed accurately
  double max_iv_residual_half_spreads = 0.0;
  CarryDiagnostics carry{};
};

// De-Americanize one expiry's Chain into a European-equivalent IV strip.
//
// Pipeline (honouring the header design notes):
//   1. (optional) imply the term borrow from the n_atm near-ATM co-terminal
//      pairs FIRST, so the forward and every log-moneyness are consistent with
//      the market's borrow; otherwise use opts.borrow_fixed.
//   2. build the term forward F and the effective carry q_eff = r − ln(F/S)/T.
//   3. per strike, choose the OTM leg (otm_side of k = ln(K/F)), invert it to a
//      European-equivalent IV, and record (k, iv, weight).
//
// A strike whose chosen leg has a non-positive or crossed bid/ask, a missing
// mid, or that fails to invert is dropped and counted in n_dropped; it is not
// written into iv/k_log/weight. The result vectors are returned (a const Chain&
// cannot be edited in place); the caller writes them back into the universe.
//
// @param chain      the (uid, expiry) bucket to de-Americanize (uses its T,
//                   expiry_ns, strikes, and per-side bids/asks/mids)
// @param S,r        spot (> 0), continuously-compounded rate (finite)
// @param cash_divs  discrete cash dividend schedule (hybrid_forward)
// @param now_ts_ns  valuation timestamp (epoch ns)
// @param opts       borrow/hybrid/pricer options
// @return           the strip + forward/borrow, or an error:
//                     InvalidArgument — S <= 0, chain.T <= 0, non-finite r, or
//                                       an empty chain
//                     Unavailable     — imply_borrow requested but no near-ATM
//                                       pair yielded a borrow
//                     Internal        — a non-finite/non-positive term forward
[[nodiscard]] atx::core::Result<DeAmResult>
de_americanize_chain(const Chain &chain, double S, double r,
                     std::span<const DividendEvent> cash_divs, std::int64_t now_ts_ns,
                     const DeAmOptions &opts) noexcept;

} // namespace atx::vol
