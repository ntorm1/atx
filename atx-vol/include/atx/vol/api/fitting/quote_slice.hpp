#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice_carry.hpp"
#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol {

// Lane H1 (phase B): the fitter's family hand-off (internal, slice_serve_record_detail.hpp)
// and the serving record (slice_serve_record.hpp) a fit may carry (QuoteSliceFit).
struct QuoteSliceServeHandoff;
struct QuoteSliceReimplyContext;
class SliceServeRecord;

// A single expiry, with separate variance and funding clocks. Volatility is
// annualized on variance_years; rate and continuous_yield on calendar_years.
// continuous_yield excludes any separately supplied cash dividends.
struct QuoteSliceMarket {
  double spot = 0.0;
  double variance_years = 0.0;
  double calendar_years = 0.0;
  double rate = 0.0;
  double continuous_yield = 0.0;
  double dividend_pv = 0.0;
};

struct QuoteSliceQuote {
  double strike = 0.0;
  Side side = Side::Call;
  double bid = 0.0;
  double ask = 0.0;
  // Optional contract carry, annualized on the CALENDAR clock and excluding
  // separately supplied cash dividends. Missing means the market reference
  // yield. Matched calls/puts at one strike must resolve to the same yield.
  std::optional<double> continuous_yield{};
};

enum class QuoteSliceDividendModel : std::uint8_t {
  ContinuousYield,
  // APPROXIMATION: replace aggregate cash PV with an equivalent continuous
  // yield. This preserves (spot - PV)*exp((rate-yield)*calendar_years), but
  // cannot preserve exercise decisions at unknown ex-dividend dates.
  AggregatePresentValue,
  // APPROXIMATION with a FUNDING-discounted cash PV: preserve
  // spot*exp((rate-yield)*calendar_years) - PV*exp(rate*calendar_years).
  // Equivalently, terminal prepaid stock is spot*exp(-yield*T)-PV.
  // This differs from escrowed AggregatePresentValue when yield != 0.
  // As with that model, missing ex-dates prevent exact exercise modeling.
  AggregateFundingPresentValue,
  // Use the discrete-cash lattice and the supplied event dates for BOTH IV
  // inversion and repricing. dividend_pv must be zero to avoid double counting.
  DatedCash
};

enum class QuoteSliceSidePolicy : std::uint8_t {
  All,
  // Puts below their local forward, calls above; both exactly at the forward. Optional
  // atm_log_band admits both sides within |log(K/F)| <= atm_log_band.
  OutOfTheMoney
};

// Policy for point observations, applied to every curve family. A point target
// always requires 0 < bid < ask: a missing bid is an upper-bound observation,
// not evidence that the fair price is half the ask. Such contracts are still
// repriced from an available fit. Bound-constrained fitting is not provided.
struct QuoteSliceQualityConfig {
  double min_bid = 0.0; // additional absolute floor; a bid must still be positive
  double max_spread_to_mid = 0.60; // full raw spread / midpoint; 0 disables
  double max_spread_vol = 0.05; // full floored spread / local Black vega; 0 disables
  double min_vega_weight = 1.0e-6; // (local Black vega / floored spread)^2; 0 disables
  // Upper clip on inverse total-variance uncertainty, matching CalibOpts.
  // This controls absolute weight scale, including spline roughness penalties.
  double max_weight = 1.0e3;
};

enum class QuoteSliceFitMethod : std::uint8_t {
  // Admitted point mids fitted by `curve` (the historical default).
  PointCurve,
  // Channel objective: every valid contract's bid/ask volatility channel (both
  // sides, bidless upper bounds included), a spread-normalised mid anchor and a
  // flat default-shape prior, solved by `channel.family` (default: the knot
  // spline on the standardized grid, served as SplineVol). `curve.kind` and
  // `min_quotes` are not used by the fit; `min_quotes` only grades evidence.
  ChannelSpline
};

// Where a fit's quote volatilities come from (opt-in, lane L2a 2026-10-18).
// PriceChannel (the default) is the historical path, bit for bit: every fit
// inverts each quote's bid, ask and mid through the American model again.
// VolChannel reads them from the chain's de-Americanised IV cache
// (src/fitting/deam_iv_cache.hpp): each quote is inverted ONCE and the channel
// objective - already a volatility-space objective - is solved on those bands by
// the same curve family; the fitted curve is then repriced ONCE and passes the
// unchanged AmericanPriceSlice certificate, and every repair and containment
// rule downstream acts on those American prices, so nothing is served on
// volatility-space evidence alone. Under VolChannel the point admission of
// fit_quote_slice and fit_quote_slice_channel reads its mid volatilities from
// the same cache (only when deam_cache is set for the point path).
enum class QuoteSliceFitSpace : std::uint8_t { PriceChannel, VolChannel };

// American kernel of the VolChannel cache inversions. Accurate: the fit's own
// model, so every cached volatility is bit-identical to the PriceChannel path's.
// Fast: the same Andersen-Lake scheme on AlProfile::Fast (american.hpp); an
// inversion whose documented Fast bound cannot be guaranteed (no usable vega,
// a failure, or a root at the volatility bounds) is redone on the accurate
// kernel. The dated-cash lattice has no Fast kernel and is always accurate.
// Screen (opt-in, SPY1s Phase 1 / L2g 2026-10-20): the measured al_screen_opts() tier
// (american.hpp AlProfile::Screen, |dP| <= 7.8e-4 max(P, 1), 4.6e-6 for T <= 0.1) is
// the SEED kernel of the Premium inversion (requires deam_inversion Premium): the
// iterates run on Screen, every accepted root is confirmed by one solve on the
// accurate kernel (or on AlProfile::Fast, deam_confirm_kernel) unless the band gate
// (deam_confirm) skips it; a price inside Screen's wide non-monotone gate runs its
// iterates on the accurate kernel instead. The kIvMin / kIvMax band edges are priced
// on Screen and every edge comparison is decided only when the quote clears the edge
// price by more than the tested bound; otherwise that edge is re-priced on the
// accurate kernel first (ambiguous_edges). A Screen value is never served or cached
// unconfirmed. The dated-cash lattice has no Screen kernel and stays accurate.
enum class QuoteSliceDeamProfile : std::uint8_t { Accurate, Fast, Screen };

// Inversion strategy of the VolChannel cache on the accurate kernel (opt-in, lane L2e
// 2026-10-19; american_iv.hpp IvStrategy). Search (the default) is the historical
// inverter, bit for bit. Premium inverts by de-Americanisation: a seed-leg model
// confirmed by cold Andersen-Lake solves, accepted at the same vol tolerance
// (QuoteSliceConfig::iv_tolerance), falling back to Search everywhere it cannot
// accept; a quote's bid and ask are seeded from its mid's cold points. Requires
// deam_profile Accurate or Screen (never Fast). The dated-cash lattice always uses Search.
enum class QuoteSliceDeamInversion : std::uint8_t { Search, Premium };

// Confirm policy and confirm kernel of a seed-tier Premium root (QuoteSliceChannelConfig
// deam_confirm / deam_confirm_kernel; SPY1s Phase 1, 2026-10-20).
enum class QuoteSliceDeamConfirm : std::uint8_t { Always, Band, Never };
enum class QuoteSliceDeamConfirmKernel : std::uint8_t { Accurate, Fast };

// Sigma-family iterates of a Screen-seeded Premium inversion (QuoteSliceChannelConfig
// deam_family; SPY1s Phase 2, opt-in; requires deam_profile Screen and deam_confirm
// Always). Off (the default): every iterate is a cold Screen solve. Adaptive: the chain's
// cache builds one AlSigmaFamily per side (american.hpp; 12 Reference node boundaries over
// the box of the side's two-sided mid Black-76 IVs, at the expiry's representative yield)
// when 5.4 x the side's two-sided quotes exceeds deam_family_c x 2 x 12, and the iterates
// price the D-model A(sigma; own q) + D_family(sigma) on it instead of a cold solve (a
// family refusal: the cold Screen iterate). Always: a family whenever one can be built
// (measurement arm). Every accepted root is confirmed on the confirm kernel at the
// contract's own yield as under Off. The dated-cash lattice and expiries below
// kAlSigmaFamilyMinTau never build one.
enum class QuoteSliceDeamFamily : std::uint8_t { Off, Adaptive, Always };

// Kernel of the certification grid (certify_and_serve's American reprice of the fitted
// curve at every strike; the served prices). Accurate (the default): the fit's own
// model, the historical path bit for bit. Fast (opt-in, SPY1s Phase 1 arm D; research
// R3): the same Andersen-Lake scheme on AlProfile::Fast, |dP| <= 1e-12 max(P, 1) below
// the certificate's allowance 1e-8 + 1e-9 max(S, K); a MEASUREMENT arm with its own hash
// set - the PM promotes or not at integration. The dated-cash lattice ignores it.
// Family (opt-in, SPY1s Phase 2 step 5; requires the vol-space cache with deam_family_q):
// each grid price on the chain cache's (sigma, q) family of its side at the strike's own q
// (|ln(K / S)| <= 0.25) where the family's VALIDATED domain-wide bound (lane K3: its error
// envelope's USD bound at that band's strike ends) x max(P, 1) is under the certificate's
// allowance 1e-8 + 1e-9 max(S, K); otherwise (no family, a refusal, an Uncovered envelope,
// a bound over the allowance) on AlProfile::Fast as under Fast. That bound is loose: lane
// K3 measured it over the allowance on every development cohort, where the key therefore
// serves the Fast grid's prices.
enum class QuoteSliceGridKernel : std::uint8_t { Accurate, Fast, Family };

// Channel knot-spline controls (src/fitting/slice_channel.hpp has the
// objective). The mid anchor is in channel half-widths; the channel penalty is
// in the fixed volatility unit min_half_width_vol.
struct QuoteSliceChannelConfig {
  double channel_weight = 100.0;      // steep penalty outside [bid, ask]; > 0
  double mid_weight = 1.0;            // spread-normalised mid anchor
  double itm_mid_weight = 1.0;        // extra factor on in-the-money legs' mid term
  double default_shape_weight = 0.05; // flat default shape when no term prior
  double prior_scale = 0.10;          // relative volatility unit of the prior penalty
  double roughness = 0.02;            // second-difference penalty on knot vols
  double min_half_width_vol = 0.0025; // floor on a channel half-width
  double mult_floor = 0.05;
  double mult_ceil = 3.0;
  std::uint16_t max_iterations = 50;
  std::size_t min_channel_quotes = 2; // usable channels required to fit
  // Curve family that solves the channel objective. Unset: the native knot
  // spline (slice_channel). Otherwise any point family except LinearVariance,
  // fitted by bounded active-set reweighting around its own weighted fitter
  // (fit_slice_curve): mid anchor, currently violated bounds and the prior
  // become weighted point targets; at most `family_iterations` refits.
  std::optional<VolCurveKind> family{};
  std::uint16_t family_iterations = 12;
  // Bounded-move convexity repair, applied only when the strict price
  // certificate fails: each side's prices are replaced by their greatest convex
  // minorant if every moved node moves, FROM ITS FITTED PRICE, by at most this
  // many of its own quote half-spreads (bidless: half the ask; a side without a
  // quote uses the other side's). This bounds the move; it does not keep a
  // price inside [bid, ask]. The unchanged certificate then runs again; moved
  // nodes are flagged in QuoteSliceFit::moved_nodes and report the volatility
  // of their served price, or PriceOnly (certified price, NaN volatility,
  // flagged in QuoteSliceFit::price_only_nodes) when that price has no
  // identifiable American volatility. 0 disables the repair.
  double repair_half_spreads = 0.0;
  // Lane L3a (opt-in; false = the historical fit): the native knot spline's wing
  // floor (slice_channel.hpp ChannelFitConfig::wing_floor) - beyond the outermost
  // lower-bounded channel of each side, the second solve keeps the wing at or above
  // the first solve's volatility at that edge, inside every quote's channel.
  bool wing_floor = false;
  // Opt-in channel dominance under graded admission (fit_quote_surface), run
  // after every other rung (pass A, channel passes, repair rung, calendar
  // pass): an expiry whose final fit is a POINT fit (served, or refused only
  // for a calendar crossing) with prices outside [bid, ask] on more than this
  // fraction of its strictly two-sided quotes, and on at least
  // `point_min_outside_quotes` of them, is offered a channel refit under the
  // prior of the other served expiries, gated point fits excluded (they are no
  // evidence). The refit replaces the point fit only
  // when the final served set is calendar-consistent with it (after the same
  // bounded-move and channel calendar repairs); otherwise the point fit is kept
  // (`point_fallback`; a served A becomes B). Once a refit is adopted, calendar
  // refusals are re-checked against the final served set
  // (`calendar_readmitted`). The gate never refuses an expiry and never changes
  // another served one. Per profile. Unset: off. In [0, 1].
  std::optional<double> point_max_outside_fraction{};
  std::size_t point_min_outside_quotes = 1; // >= 1; used only with the fraction
  // Opt-in vol-space channel (QuoteSliceFitSpace). The two deam_ keys act only
  // under VolChannel; the PriceChannel path ignores them. deam_cache: keep the
  // chain's cache in a bounded per-thread memo keyed by the EXACT inputs (market,
  // quotes, dividends, model and inverter controls), so every later fit of the
  // same chain - every graded rung, and the point fit - reuses its inversions;
  // without it each fit builds its own cache.
  QuoteSliceFitSpace fit_space = QuoteSliceFitSpace::PriceChannel;
  QuoteSliceDeamProfile deam_profile = QuoteSliceDeamProfile::Accurate;
  bool deam_cache = false;
  QuoteSliceDeamInversion deam_inversion = QuoteSliceDeamInversion::Search; // opt-in (L2e)
  // Opt-in confirm policy of a seed-tier (deam_profile Screen) Premium root (SPY1s Phase
  // 1, 2026-10-20; research R2 / gap report 5a). Always (the default): every accepted
  // seed-tier root is confirmed by one solve on the confirm kernel. Band: the confirm is
  // skipped only where (eps_A + eps_r) / v_min < half-spread-in-vol / deam_confirm_k -
  // eps_A the seed tier's tested price bound at the contract, eps_r the root's residual
  // on the seed map, v_min the seed map's chord slope at the root less its chord bound
  // (floored at the Fast-vega guard); a non-positive v_min, a price inside the seed
  // tier's non-monotone gate, a bidless quote or a root at the volatility bounds always
  // confirms. Never: the seed-tier root as is (MEASUREMENT ARM ONLY; never a candidate
  // default). Both require deam_profile Screen. The accuracy a skipped root carries is
  // measured, not assumed: quote-bench --deam-audit reports the max accurate-map Newton
  // distance of the skipped roots.
  QuoteSliceDeamConfirm deam_confirm = QuoteSliceDeamConfirm::Always;
  double deam_confirm_k = 10.0; // > 0
  // The confirming solve's kernel (deam_profile Screen only): the fit's accurate kernel
  // (the default) or the same scheme on AlProfile::Fast (|dP| <= 1e-12 max(P, 1); research
  // R3 measurement arm - the PM promotes or not at integration).
  QuoteSliceDeamConfirmKernel deam_confirm_kernel = QuoteSliceDeamConfirmKernel::Accurate;
  // Seed-tier routing gate (deam_profile Screen only; SPY1s Phase 2, opt-in): 0 (the
  // default) runs a price's iterates on the accurate kernel inside Screen's own wide
  // non-monotone gate (4e-4 K max(1, T^2), about 100x the measured Screen dip); > 0 narrows
  // that band to deam_seed_gate_kappa K max(1, T^2) (the measured dip times a margin, e.g.
  // 4.3e-5 = 10 x 4.3e-6), so more inversions iterate on Screen. Every Screen-iterated root
  // is confirmed (or band-gated) on the confirm kernel exactly as under the default. >= 0.
  double deam_seed_gate_kappa = 0.0;
  // Sigma-family iterates (QuoteSliceDeamFamily; SPY1s Phase 2) and the adaptive build's
  // threshold constant c (> 0).
  QuoteSliceDeamFamily deam_family = QuoteSliceDeamFamily::Off;
  double deam_family_c = 3.0;
  // (sigma, q) families (SPY1s Phase 2 step 5, opt-in; requires deam_family Adaptive /
  // Always): the cache builds L2h-b AlSigmaQFamily per side instead - n_sigma 16 over the
  // sigma box, n_q 4 (3 where T x q-width <= 1e-5) over the chain's q box (all quotes'
  // effective yields; a call side only where q > 0) - each with a VALIDATED error envelope
  // built from its own nodes (lane K3: refinement-verified Chebyshev tails, audited by
  // off-node probes against the cold accurate kernel; a probe above it makes the family
  // Uncovered); the iterates price each strike at its OWN q on it (no D-model). Every
  // accepted root is confirmed on the confirm kernel, except under deam_confirm Band
  // (allowed here: the B-family arm), where a root skips its confirm solve only when two
  // family prices and their envelope bounds enclose the accurate map's root in a bracket no
  // wider than 1e-7 vol. The half-spread band gate never skips a confirm under this key: a
  // quote whose side has no family is confirmed. The adaptive rule counts n_sigma x n_q
  // nodes.
  bool deam_family_q = false;
  // Family-native iterates (SPY1s Phase 3 lever 1, opt-in; requires deam_family_q and
  // deam_confirm Always): the (sigma, q) family's own map is iterated without the
  // Barone-Adesi-Whaley / Black-76 seed leg (a bracketed secant-Newton from the mid's
  // family points, else from the Black-76 IV with the family's chord); any family refusal
  // restarts the inversion on the seed-leg path. Acceptance (lane K3): a root skips its
  // confirm solve when two family prices and their envelope bounds enclose the accurate
  // map's root in a bracket no wider than 1e-7 vol - so under this key deam_confirm Always
  // does NOT confirm every root; otherwise the confirm kernel closes such a bracket (at most
  // three solves, each end certified by the confirm kernel beyond its tested bound or by the
  // family beyond its envelope bound), and a root it cannot bracket is answered by the
  // Search route. No secant slope or step length is a bound.
  bool deam_family_native = false;
  // The native fit route (lane K4, r6 2026-10-23; opt-in, requires deam_family_native; the
  // speed arm, no default changes): every inversion of the chain stays on typed routes and
  // NEVER runs the Search route. A price inside the non-monotone edge band is classified
  // (american_region.hpp, the exercise plateau) before any root attempt; a family-native
  // root the acceptance cannot close, a refused native attempt and an edge-band price go to
  // the bounded reference route (a certified bracketed root on the confirm kernel under the
  // Intrinsic exercise rule, started from the native attempt's retained bracket) - never to
  // the Barone-Adesi-Whaley seed leg; a price-only or interval acceptance outcome is typed
  // (no IV) instead of re-solved; a quote's bid and ask always start from its mid's carry.
  // The reference route is budgeted per chain cache: deam_reference_max_calls requests and
  // deam_reference_max_evaluations kernel evaluations (a deterministic proxy for its time);
  // an exhausted budget is a typed, counted refusal, never a silent success. Band edges are
  // decided by validated bounds (Black-76 below the kIvMax edge, the family's bounded price
  // at its sigma box floor above the kIvMin edge) before any edge is priced, and the
  // certification grid prices a node on the family only under its per-point bound
  // (family_price_bounded) below the certificate allowance.
  bool deam_native_route = false;
  std::uint32_t deam_reference_max_calls = 1024;       // > 0, per chain cache
  std::uint32_t deam_reference_max_evaluations = 8192; // > 0, per chain cache
  // The batched native route (lane K4b, r6 2026-10-23; opt-in, requires deam_native_route):
  // the chain's work group is prepared together before its bands are read - every channel
  // quote's band edges, then the mids' family-native lanes in one iv_batch::invert_many call
  // per side (on the AVX2 kernel of a precontracted family table when the host allows, the
  // scalar map otherwise), the bids' and asks' lanes from their mids' outcomes in a second
  // call, and the K3 acceptance's first confirm prices in one batch - and each request then
  // runs the native route exactly as without the key, consuming a prepared value only when
  // its inputs are bit-identical to the ones it would have computed (on the scalar map every
  // served value is the unbatched route's bit for bit; on AVX2 the batch kernel's documented
  // rounding class, american-iv-batch.md section 5).
  bool deam_native_batch = false;
  // A request whose side has no family on the native route (lane K4b, review-K4 m5; opt-in,
  // requires deam_native_route): answered by the bounded reference route instead of the
  // Barone-Adesi-Whaley seed leg (no seed leg, no BAW model on the route).
  bool deam_native_no_leg = false;
  // The reference route's wall-time budget per chain cache (lane K4b; microseconds, 0: none),
  // on top of the count budget above: a request that reaches the reference route after the
  // chain's reference wall reached it is refused, typed BudgetExhausted and counted
  // (reference_wall_refused). Host-dependent by construction: a binding wall budget makes the
  // served set depend on the host's speed, so it is a guard, measured never to bind.
  std::uint32_t deam_reference_max_wall_us = 0;
};

// InvalidArgument unless every channel control is finite and in range
// (channel_weight > 0, mult_ceil > mult_floor > 0, a supported family, ...).
// fit_quote_slice_channel and fit_quote_surface apply it before any fit.
[[nodiscard]] Status validate_quote_slice_channel(const QuoteSliceChannelConfig &config);

// Opt-in repair inside the bid/ask channel. Off is the historical behaviour.
enum class QuoteSliceRepairMode : std::uint8_t { Off, Channel };

// Channel repair, applied only after the strict price certificate has failed
// (and after the bounded-move convexity repair, when that is enabled, has not
// fixed it). First only the nodes more than the certificate's allowance above
// their side's lower convex envelope are lowered onto it (every other node
// keeps its fitted price); if that is not accepted, each side's node prices are
// projected (least squares, Dykstra) onto the set the certificate's strike
// checks describe: convex in strike, call slopes in [-discount_max, 0],
// put slopes in [0, discount_max], intrinsic and upper price bounds, and every
// node inside its own quote's [bid, ask] channel widened to contain its fitted
// price, so no node can end farther outside its channel than it started
// (bidless: [0, ask]; a side without a quote is held fixed). Nodes whose
// constraints never bind keep their fitted price exactly; a residual violation
// above the certificate allowance is lowered onto its envelope. A repair is
// accepted only when (a) the channel rule holds, (b) at most `max_nodes`
// strikes moved by more than `immaterial_price_move` (a floor at pricing
// accuracy: per-strike carry noise can leave many nodes a few 1e-5 above their
// envelope, and such moves are flagged and re-implied but do not count), and
// (c) every moved node-side WITHOUT its own quote (where no channel protects
// the price) moved by at most `max_vol_move` in American IV from the fitted
// curve. What bounds what: a QUOTED side is bounded only by its own channel
// [min(bid, fitted), max(ask, fitted)], so its IV can move by up to that
// channel's IV width (several vol points on wide markets) and `max_vol_move`
// does not apply to it; `max_vol_move` binds only an unquoted side, and such a
// side can move only through the residual lowering (the projection holds it
// fixed). The IV change of every moved node-side is recorded either way
// (QuoteSliceRepairRecord::max_vol_move, max_unquoted_vol_move). The
// unchanged certificate then runs again; nothing about certification is
// relaxed. Moved nodes are flagged in QuoteSliceFit::moved_nodes and re-implied
// as for the other repairs.
// Which question the price certificate answers (lane L3r1, 2026-10-20; opt-in).
// Exact (the historical default, bit for bit): the fitted node prices themselves
// must pass AmericanPriceSlice::create. Box: when they fail (and the bounded-move
// and channel repairs, where enabled, have not fixed it), the fitted prices are
// projected (american_price_slice.hpp certify_american_box: Dykstra, uniform
// weights — the channel repair's projection) onto the certificate's strike
// constraints intersected with every node side's QUOTE box [bid - h, ask + h]
// (bidless: [0, ask + h]; a side without a quote is held at its fitted price),
// h = box_half_ticks * box_tick, and the UNCHANGED certificate certifies that
// curve P*. No node or volatility budget applies: the box is the budget, so P*
// never leaves the market by more than h. P* is served where the exact curve
// failed and P* passes; its moved nodes are flagged in QuoteSliceFit::moved_nodes
// and re-implied exactly like a repair's (PriceOnly where the served price has no
// volatility), and QuoteSliceDiagnostics::box records the move. The
// non-monotone refusal, the nondividend joint check and every calendar rule
// downstream are unchanged and act on P*. The box certificate runs wherever the
// certificate runs (pass A included); it is a certificate, not a fallback rung.
enum class QuoteSliceCertificate : std::uint8_t { Exact, Box };

// Source of the native knot spline's served-multiple ceiling (lane L3r1 lever 3,
// 2026-10-21; opt-in). Policy (the historical default, bit for bit): the channel's
// mult_ceil. Market: after the channel solve the ceiling becomes
// max(mult_ceil, min(kQuoteSliceMarketWingMultMax, kQuoteSliceMarketWingMultMargin x m)),
// m = the larger of the market's own multiples (mid volatility, else the channel's
// centre, over the fit's ATM reference) at the outermost two-sided channel quote of
// each wing; without a two-sided quote the ceiling stays mult_ceil. Only the served
// clamp moves (the channel solve never reads the ceiling), so a dense market wing near
// 4 x ATM is served instead of being clamped onto 3 x, whose concave kink is a
// convexity failure of the price certificate. Native knot spline only (`family`
// unset); the term prior's multiples keep the policy ceiling.
enum class QuoteSliceWingMultSource : std::uint8_t { Policy, Market };
inline constexpr double kQuoteSliceMarketWingMultMax = 6.0;
inline constexpr double kQuoteSliceMarketWingMultMargin = 1.1;

// Distance-to-market certificate of fit_quote_surface (lane L3q, 2026-10-21; opt-in).
// Off (the historical default, bit for bit): a served row may sit any distance from
// its quotes. Gate: after every calendar pass, a served row (Success or PriceOnly)
// whose price lies more than `market_distance_spreads` spreads outside its own quote
// (american_price_slice.hpp market_distance_spreads, the scorer's spread) is refused
// ROW by row (QuoteSliceEvaluationStatus::MarketDistanceFailure); the slice and its
// certified curve are unchanged, so every served subset stays arbitrage-free.
// Repair: Gate, preceded by (1) a BIDIRECTIONAL calendar rung — a crossing no
// one-sided repair absorbs is projected jointly with the accepted slices it crosses,
// each inside its own quote boxes (repair_quote_slice_calendar_joint) — and (2) per
// served slice with a row beyond the bound, an in-box repair onto [bid, ask], then
// onto the bound's band, under the calendar clamp of every other served slice
// (repair_quote_slice_market); moved nodes are re-implied exactly like any repair's.
enum class QuoteSliceMarketDistance : std::uint8_t { Off, Gate, Repair };

struct QuoteSliceRepairConfig {
  QuoteSliceRepairMode mode = QuoteSliceRepairMode::Off;
  std::size_t max_nodes = 16;            // >= 1
  double max_vol_move = 0.01;            // > 0, volatility units
  double immaterial_price_move = 5.0e-4; // >= 0, price units
  // Opt-in box certificate (QuoteSliceCertificate); Exact is the historical path.
  QuoteSliceCertificate certificate = QuoteSliceCertificate::Exact;
  double box_half_ticks = 0.5; // >= 0, finite: each box edge is widened by this many ticks
  double box_tick = 0.01;      // > 0, finite, price units
  // Opt-in exact in-box projection rung (lane C4, r6 2026-10-23; default off: the historical
  // path bit for bit). Under the box certificate only (inert under Exact): when
  // certify_american_box refuses a slice's fitted prices, the fitted grid is projected onto
  // the certificate's polytope intersected with the same quote boxes by an exact interior-
  // point method (src/fitting/inbox_projection.hpp); the projected curve is served when the
  // UNCHANGED AmericanPriceSlice::create passes it, and an empty in-box polytope is typed
  // InfeasibleData with its conflict (QuoteSliceRepairRecord::inbox). Never widens a box.
  bool inbox_projection = false;
  // Opt-in market wing ceiling (QuoteSliceWingMultSource); Policy is the historical path.
  QuoteSliceWingMultSource wing_mult_source = QuoteSliceWingMultSource::Policy;
  // Opt-in distance-to-market gate / repair (QuoteSliceMarketDistance, lane L3q); Off is
  // the historical path. The bound is in spreads (>= 0, finite; 0 = strict containment:
  // every served row inside its quote); the spread floor is box_tick.
  QuoteSliceMarketDistance market_distance = QuoteSliceMarketDistance::Off;
  double market_distance_spreads = 1.0;
  // Sweep budget of the bidirectional calendar rung's joint projection (lane C2;
  // project_calendar_joint_bounded, >= 1). The default is the historical 20,000.
  std::size_t joint_max_sweeps = 20000;
  // Opt-in parity-coupled calendar clamp (lane K4b, C3 review D2; default off: the historical
  // path bit for bit). In repair_quote_slice_calendar, a node whose clamped call - put leaves
  // the certificate's affine bound prepaid_max - K discount_min (so AmericanPriceSlice::create
  // would refuse the slice) has its put raised to that bound inside its own calendar cap.
  bool calendar_parity_clamp = false;
};

// InvalidArgument unless the mode, certificate, wing source and market-distance mode
// are known and every budget, box control and bound is finite and in range.
// fit_quote_slice and fit_quote_surface apply it before any fit.
[[nodiscard]] Status validate_quote_slice_repair(const QuoteSliceRepairConfig &config);

enum class QuoteSliceRepairOutcome : std::uint8_t {
  NotAttempted, // certificate passed, mode Off, or no repair needed
  Applied,      // accepted and certified
  NoMove,       // the certificate failed but the projection moved no node
  Infeasible,   // no convex price set inside the channels (or no settled projection)
  LeftChannel,  // a moved node-side would end farther outside its channel
  NodeBudget,   // more than max_nodes strikes would move materially
  VolBudget,    // an unquoted node-side's volatility would move more than max_vol_move
  NotCertified, // within budget, but the certificate (or the calendar clamp) still fails
  NonConverged  // lane C2: the joint projection spent its sweep budget without settling
                // and without an infeasibility certificate (Infeasible names a conflict)
};

[[nodiscard]] const char *to_string(QuoteSliceRepairOutcome value) noexcept;

// ── Exact in-box projection rung (lane C4, r6 2026-10-23; opt-in) ──────────
// QuoteSliceRepairConfig::inbox_projection. Outcomes of one run of the rung: NotAttempted
// (key off, or the box certificate passed), Applied (the projected curve passed
// AmericanPriceSlice::create and replaced the fitted prices), InfeasibleData (no arbitrage-
// free curve exists inside the boxes: `conflict` names the constraint), NonConverged (the
// Newton budget ran out; nothing changed), NotCertified (create() refused the projected
// curve; nothing changed), InvalidInput (malformed nodes, boxes or bounds).
enum class QuoteSliceInboxOutcome : std::uint8_t {
  NotAttempted,
  Applied,
  InfeasibleData,
  NonConverged,
  NotCertified,
  InvalidInput
};
// The conflict of an InfeasibleData outcome. EmptyBox: a node side's quote box misses the
// certificate's own per-node bounds (intrinsic, upper; the non-dividend call floor).
// NoConvexCurve: one side's boxes admit no convex curve with the certificate's slopes.
// AffineBox: at one node no call / put pair inside the side curves' reach meets the affine
// call-minus-put bound. Coupled: every node passes those tests, yet the interior-point
// phase I proves that no curve pair meets every constraint at once; `excess` is then its
// certified lower bound on the least achievable worst violation (price units) and the node /
// side are those of the constraint carrying the largest dual weight.
enum class QuoteSliceInboxConflict : std::uint8_t {
  None,
  EmptyBox,
  NoConvexCurve,
  AffineBox,
  Coupled
};
// Why certify_american_box refused before the rung ran (diagnostic attribution only, read
// from its message): the box outside the exercise bounds, no convex curve inside the boxes,
// its Dykstra projection did not settle, or create() refused its per-side projection.
enum class QuoteSliceBoxRefusal : std::uint8_t {
  None,
  ExerciseBounds,
  NoConvexCurve,
  NotSettled,
  CertificateRefused,
  Other
};

[[nodiscard]] const char *to_string(QuoteSliceInboxOutcome value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceInboxConflict value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceBoxRefusal value) noexcept;

// What one run of the in-box projection rung did. Distances are in units of each quoted
// side's own box half-width (half the quote spread at h = 0): `max_move_half_spreads` from
// the fitted price, `max_mid_half_spreads` from the box centre (< 1: inside the box).
// `elapsed_us` is the rung's own steady_clock wall (diagnostic; never a decision).
struct QuoteSliceInboxRecord {
  QuoteSliceInboxOutcome outcome = QuoteSliceInboxOutcome::NotAttempted;
  QuoteSliceBoxRefusal box_refusal = QuoteSliceBoxRefusal::None;
  QuoteSliceInboxConflict conflict = QuoteSliceInboxConflict::None;
  Side conflict_side = Side::Call;
  std::size_t conflict_node = 0;
  double conflict_strike = 0.0;
  double conflict_excess = 0.0;
  std::size_t free_sides = 0;    // node sides the projection may move (lo < hi)
  std::size_t fitted_outside = 0; // of which the fitted price lies outside [lo, hi]
  std::size_t constraints = 0;   // barrier rows (implied rows dropped)
  std::size_t phase1_steps = 0;  // Newton steps proving feasibility
  std::size_t phase2_steps = 0;  // Newton steps of the projection itself
  double max_move_half_spreads = 0.0;
  double max_mid_half_spreads = 0.0;
  double elapsed_us = 0.0;
};

// What one channel repair did (or would have done before it was refused).
struct QuoteSliceRepairRecord {
  QuoteSliceRepairOutcome outcome = QuoteSliceRepairOutcome::NotAttempted;
  std::size_t nodes = 0;          // strikes whose call or put price moved
  std::size_t material_nodes = 0; // of which moved by more than immaterial_price_move
  double max_price_move = 0.0;     // largest absolute node-price move
  double max_vol_move = 0.0;       // largest |IV change| over identifiable moved node-sides
  double max_unquoted_vol_move = 0.0; // the same over node-sides without a quote (budgeted)
  std::size_t unidentified = 0;    // moved node-sides whose new price has no American IV
  // Joint projection only (repair_quote_slice_calendar_joint, lane C2): the sweeps run over
  // both sides, and the conflict that made the outcome Infeasible. The conflict is either
  // the projection's infeasibility certificate, or EmptyBox for the rung's own empty-box
  // exits (fix round 1, review m2). Those exits are: the fixed neighbours' floor above
  // their cap at a member's node, or a member's quote box outside that clamp. The kind is
  // None for every other outcome. JointProjectionConflict indexes the members in maturity
  // order.
  std::size_t sweeps = 0;
  JointProjectionConflict conflict{};
  // The box certificate's record only (QuoteSliceDiagnostics::box): what the opt-in exact
  // in-box projection rung did after certify_american_box refused (lane C4).
  QuoteSliceInboxRecord inbox{};
};

// How a fit publishes a served row whose volatility its certified price does not
// identify (opt-in). Legacy (the default) keeps the historical status and value bit for
// bit: a curve volatility or re-implied root at or below kIvMin, or a root the query
// types FlatExerciseRegion / LowVega, is a Success volatility - labelled with the typed
// query's answer (vol_convention None, the reason and interval; lane C0 fix round 1: one
// answer per contract). PriceOnly publishes it PriceOnly (QuoteSlicePriceOnlyReason
// FloorVol, FlatExerciseRegion or LowVega, below) with its certified price kept and NaN
// volatility, flagged in QuoteSliceFit::price_only_nodes (lane C0).
// Prices, certificates and every calendar decision are unchanged.
enum class QuoteSliceFloorPolicy : std::uint8_t { Legacy, PriceOnly };

// The served call floor (lane C3, opt-in). Legacy (the default) is the historical path bit
// for bit: a call node's hard floor is its American exercise value max(S - K, 0).
// DiscountedIntrinsic also holds every call node at or above the SHARED, maturity-consistent
// discounted intrinsic of the snapshot's non-dividend expiries: the largest
// spot - K * discount over the QuoteSliceCallFloorConfig::lines whose calendar_years is at
// most the expiry's own. A non-dividend American call is worth at least S - K * df (C1 of
// check_nondividend_american), and a later American call at least an earlier one, so the
// floor also lifts a LATER expiry - a dividend payer included, whose own model may sit at
// the exercise value S - K - off intrinsic wherever an earlier non-dividend expiry's bound
// is higher. Without it a deep-ITM non-dividend call is served at S - K, below S - K * df,
// whenever a later slice at S - K caps it through the calendar repair (review-C2 B(3)).
// A slice whose lifted nodes fail its own certificate keeps its historical prices; lifted
// nodes are moved nodes (QuoteSliceFit::moved_nodes, volatility re-implied).
enum class QuoteSliceCallFloor : std::uint8_t { Legacy, DiscountedIntrinsic };

[[nodiscard]] const char *to_string(QuoteSliceCallFloor value) noexcept;

// One non-dividend expiry's discounted intrinsic, spot - K * discount (lane C3).
// Valid: finite calendar_years >= 0, finite spot > 0, discount in (0, 1] (rate >= 0).
struct QuoteSliceCallFloorLine {
  double calendar_years = 0.0;
  double spot = 0.0;
  double discount = 1.0;
};

// QuoteSliceConfig::call_floor (lane C3). Under Legacy the line list must be empty (an
// inert floor is refused). Under DiscountedIntrinsic the caller supplies the snapshot's
// lines, normally quote_surface_call_floor_lines over the same expiries; an empty list
// lifts nothing.
struct QuoteSliceCallFloorConfig {
  QuoteSliceCallFloor mode = QuoteSliceCallFloor::Legacy;
  std::vector<QuoteSliceCallFloorLine> lines;
};

struct QuoteSliceConfig {
  CurveConfig curve{.kind = VolCurveKind::Essvi};
  QuoteSliceQualityConfig quality{};
  QuoteSliceDividendModel dividend_model = QuoteSliceDividendModel::ContinuousYield;
  QuoteSliceSidePolicy side_policy = QuoteSliceSidePolicy::OutOfTheMoney;
  std::optional<AlOpts> al_opts{};
  std::size_t min_quotes = 6;
  double atm_log_band = 0.0;
  double spread_floor = 0.01;
  double min_time_value = 1.0e-5;
  double iv_tolerance = 1.0e-7;
  std::uint16_t max_iv_iterations = 64;
  QuoteSliceGridKernel grid_kernel = QuoteSliceGridKernel::Accurate; // opt-in (SPY1s P1)
  int discrete_div_steps = kDiscreteDivDefaultSteps;
  // Opt-in carry resolution (quote_slice_carry.hpp). The default Supplied mode
  // is the historical path, unchanged.
  QuoteSliceCarryConfig carry{};
  QuoteSliceFitMethod method = QuoteSliceFitMethod::PointCurve;
  QuoteSliceChannelConfig channel{};
  // Lattice scheme of the DatedCash model (american.hpp, DiscreteDivScheme),
  // used for IV inversion AND certified repricing so the two stay one model.
  // Ignored by every other dividend model. SplicedCrr is the historical lattice
  // bit for bit; discrete_div_scheme_admits states the step requirements.
  DiscreteDivScheme discrete_div_scheme = DiscreteDivScheme::SplicedCrr;
  // Opt-in channel repair of a failed certificate (default Off).
  QuoteSliceRepairConfig repair{};
  // Opt-in identifiability publication (lane C0; QuoteSliceFloorPolicy). Legacy is the
  // historical path bit for bit.
  QuoteSliceFloorPolicy floor_policy = QuoteSliceFloorPolicy::Legacy;
  // Opt-in served call floor (lane C3; QuoteSliceCallFloor). Legacy is the historical path
  // bit for bit.
  QuoteSliceCallFloorConfig call_floor{};
};

enum class QuoteSliceQuoteStatus : std::uint8_t {
  Admitted,
  InvalidContract,
  InvalidQuote,
  ExcludedSide,
  UnidentifiableVolatility,
  ImpliedVolatilityFailure,
  InvalidObservation,
  BidlessQuote,
  BelowMinimumBid,
  WideSpreadToMid,
  WideSpreadVol,
  LowVegaWeight
};
// PriceOnly: the row carries a certified price (fitted_price) but no volatility
// (fitted_iv NaN), because a repair moved its node to a price that has no
// identifiable American volatility under the row's own carry (for example
// below the zero-volatility exercise value). evaluate_quote_slice refuses the
// same contract. PriceOnly is final for the life of the fit: a later calendar
// repair refreshes the row's certified price but never re-implies a volatility
// for it (QuoteSliceFit::price_only_nodes), so a PriceOnly row is never served
// with a volatility. InvalidVolatility is a curve without a positive volatility
// at the strike; such a row carries neither price nor volatility.
// MarketDistanceFailure (lane L3q, opt-in QuoteSliceMarketDistance Gate / Repair): the
// slice is served but this row's certified price lies more than the bound outside its
// own quote; the row carries its (unserved) price for diagnostics and no volatility.
enum class QuoteSliceEvaluationStatus : std::uint8_t {
  Success,
  InvalidContract,
  NoFit,
  InvalidVolatility,
  PricingFailure,
  PriceOnly,
  MarketDistanceFailure
};
enum class QuoteSliceFitStatus : std::uint8_t {
  Success, InsufficientQuotes, FitFailure, ArbitrageFailure
};

[[nodiscard]] const char *to_string(QuoteSliceDividendModel value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceQuoteStatus value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceEvaluationStatus value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceFitStatus value) noexcept;

// ── Identifiability and output conventions (lane C0, 2026-10-22) ─────────────
//
// A certified price need not identify an American volatility. american_implied_vol
// reports its floor kIvMin as SUCCESS for a price at the zero-volatility value and for
// a sub-floor root (american_iv.hpp): a bound, not a measured volatility. PriceOnly
// rows and price-only query answers (query_quote_slice) carry the certified price, the
// reason and, where one is known, the volatility interval [iv_lower, iv_upper] that
// reproduces the price under the contract's own model and carry. Price coverage
// (Success or PriceOnly) and identified-volatility coverage (Success) are separate.
//
// FloorVol: the volatility the row or query would report (a re-implied root, or the
// fitted curve's value at the strike) is at or below kIvMin. Interval [0, kIvMin];
// when the price sits on the Andersen-Lake kernel's exercise plateau
// (al_non_monotone_region at kIvMin) every volatility up to the plateau edge prices it,
// so the interval is [0, sigma_ex] with sigma_ex the smallest volatility (to 1e-6,
// rounded up) at which the spot leaves the kernel's exercise region.
// FlatExerciseRegion / LowVega: a re-implied root above kIvMin that the opt-in
// non-monotone refusal of american_iv.hpp (IvOpts::refuse_non_monotone) types
// NonMonotoneRegion - inside the kernel's exercise region (interval [0, sigma_ex]), or
// not certified by the cold map's slope / Newton distance (no interval). Continuous
// carry only: the dated-cash lattice has no such refusal.
// UnidentifiedInterval: the inverter identifies no volatility (for example a repaired
// price below the zero-volatility value); no interval is claimed. This is the
// historical PriceOnly row, now typed.
enum class QuoteSlicePriceOnlyReason : std::uint8_t {
  None,
  FloorVol,
  FlatExerciseRegion,
  LowVega,
  UnidentifiedInterval
};

// Which volatility a row or query answer reports (master plan 3.2). None: no
// volatility. AmericanQuoting: the American implied volatility of the quote itself
// (QuoteSliceRow::market_iv). EuropeanEquivalent: a Black-76 equivalent (no quote-slice
// route reports one; named so consumers never confuse the two). FittedCurve: the
// fitted American-IV curve's value at the strike (an unmoved node). ReimpliedRepaired:
// re-implied from the certified served price (a repaired node, or between nodes).
enum class QuoteSliceVolConvention : std::uint8_t {
  None,
  AmericanQuoting,
  EuropeanEquivalent,
  FittedCurve,
  ReimpliedRepaired
};

// How an expiry's spot and carry relate to its quotes' own observations.
// ForwardPreserving: one authoritative snapshot spot (rebase_quote_slice_spot), each
// expiry's carry re-derived so its forward under the declared dividend model is
// unchanged. This preserves FORWARDS only: an American price (exercise value,
// early-exercise premium) depends on the spot itself, so it is not price-invariant.
enum class QuoteSliceCarryConvention : std::uint8_t { Supplied, ForwardPreserving };

// Which rule chose the spot an expiry was fitted on (lane C0 fix round 1: spot_version
// alone does not say). Own: the expiry's own spot (spot_version 0). Nearest: the snapshot
// spot S* observed at the observation instant nearest the snapshot time. Median: the
// median spot over the snapshot's observations, with no instant - also what a Nearest
// request falls back to when no observation carries an instant.
enum class QuoteSliceSpotRule : std::uint8_t { Own, Nearest, Median };

// The American model behind a fit's prices and volatilities.
enum class QuoteSliceModelKind : std::uint8_t { AndersenLake, DiscreteDividendLattice };

[[nodiscard]] const char *to_string(QuoteSlicePriceOnlyReason value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceVolConvention value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceFloorPolicy value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceCarryConvention value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceSpotRule value) noexcept;
[[nodiscard]] const char *to_string(QuoteSliceModelKind value) noexcept;

// Snapshot provenance of one expiry fit, supplied by the caller that built the market
// (fit functions do not observe clocks). Zero / NaN / false mean "not supplied".
struct QuoteSliceSnapshotInfo {
  std::int64_t quote_ns = 0;      // quote clock: the quotes' observation instant, Unix ns
  std::int64_t underlying_ns = 0; // underlying clock: instant of the spot the fit used
  std::uint32_t spot_version = 0; // 0: the expiry's own spot; >= 1: the snapshot spot's version
  QuoteSliceSpotRule spot_rule = QuoteSliceSpotRule::Own; // the rule that chose that spot
  std::uint32_t carry_version = 0; // 0: carry as supplied; >= 1: a re-derived carry's version
  QuoteSliceCarryConvention carry_convention = QuoteSliceCarryConvention::Supplied;
  double raw_spot = std::numeric_limits<double>::quiet_NaN(); // own spot before a rebase
  bool spot_stale = false; // raw_spot differs from the snapshot spot beyond the caller's bound
};

struct QuoteSliceRow {
  QuoteSliceQuoteStatus quote_status = QuoteSliceQuoteStatus::InvalidQuote;
  QuoteSliceEvaluationStatus evaluation_status = QuoteSliceEvaluationStatus::NoFit;
  bool admitted = false;
  double market_iv = std::numeric_limits<double>::quiet_NaN();
  double fitted_iv = std::numeric_limits<double>::quiet_NaN();
  double fitted_price = std::numeric_limits<double>::quiet_NaN();
  // Full floored spread in local volatility units, once IV and vega exist.
  double spread_vol = std::numeric_limits<double>::quiet_NaN();
  double weight_w = 0.0; // stored fit weight; zero when not admitted
  bool weight_capped = false;
  // query_quote_slice's reason and interval (NaN where none is known) for a served row
  // whose volatility the query does not identify: every PriceOnly row, and under Legacy
  // a Success row serving such a volatility. None / NaN on every other row, including a
  // row demoted from served (MarketDistanceFailure, NoFit, PricingFailure).
  QuoteSlicePriceOnlyReason price_only_reason = QuoteSlicePriceOnlyReason::None;
  double iv_lower = std::numeric_limits<double>::quiet_NaN();
  double iv_upper = std::numeric_limits<double>::quiet_NaN();
  // Where fitted_iv came from when it was set, None when the query does not identify it;
  // read it through vol_convention(row).
  QuoteSliceVolConvention vol_source = QuoteSliceVolConvention::None;
  // Per-contract error metadata of the certification grid (lane K4b, review-K4 m6; the
  // native fit route with grid_kernel Family): a bound (USD) on |grid price - the fit's
  // accurate American model| at this row's grid node (strike, side, curve sigma, contract
  // yield) - the family's per-point bound (family_price_bounded) for a family-priced node,
  // AlProfile::Fast's tested bound for a node priced on Fast. It describes the grid price
  // the certificate started from, not a repair's move (moved_nodes). NaN off that route or
  // where the row has no grid node.
  double grid_bound_usd = std::numeric_limits<double>::quiet_NaN();
};

// The convention of row.fitted_iv: vol_source for a Success row, None otherwise (so a
// later status change - a market-distance refusal, a price-only flip - never leaves a
// stale convention behind). A Legacy Success row whose volatility query_quote_slice does
// not identify (price_only_reason set) reports None: a bound, not an identified IV.
[[nodiscard]] QuoteSliceVolConvention vol_convention(const QuoteSliceRow &row) noexcept;

// Pre-solve screen of a channel fit (lane L2c, 2026-10-19): the typed reason a channel
// fit is refused BEFORE its solve, read from the usable bid/ask volatility bands only
// (the de-Am IV cache under VolChannel). EmptyBand: no quote has a usable band (every
// band empty or entirely outside the model's identifiable price range); TooFewChannels:
// some, but fewer than channel.min_channel_quotes. Both are today's "too few usable
// bid/ask channels" refusal (fit_status InsufficientQuotes, unchanged), now typed. Only
// necessary conditions of a served fit qualify, so the screen never refuses a slice
// the solve would serve.
enum class QuoteSlicePreScreen : std::uint8_t { Pass, EmptyBand, TooFewChannels };

[[nodiscard]] const char *to_string(QuoteSlicePreScreen value) noexcept;

// Populated only by the channel method.
struct QuoteSliceChannelDiagnostics {
  bool used = false;
  std::size_t channel_quotes = 0; // usable bid/ask volatility channels
  std::size_t mid_quotes = 0;     // of which carry a mid volatility
  std::size_t mid_strikes = 0;    // distinct strikes among the mid quotes
  std::size_t violations = 0;     // fitted volatility outside its channel
  double max_violation_half_widths = 0.0;
  double prior_strength = 0.0;
  bool term_prior = false;        // false: flat default shape
  bool converged = false;
  std::size_t iterations = 0;     // Newton iterations (knot spline) or family refits
  double solve_seconds = 0.0;     // wall time of the channel solve alone (diagnostic)
  // Convexity repair: required shift measured whenever the certificate failed,
  // nodes moved only when the repair was within tolerance and applied, and 0
  // when its result was discarded (still uncertified) in favour of the channel
  // repair (QuoteSliceDiagnostics::repair), whose moves are the served ones.
  std::size_t repaired_nodes = 0;
  double repair_max_price = 0.0;
  double repair_max_half_spreads = 0.0;
  QuoteSlicePreScreen prescreen = QuoteSlicePreScreen::Pass; // Pass unless refused pre-solve
  // Served-multiple ceiling set by QuoteSliceWingMultSource::Market (lane L3r1); 0 when
  // the policy ceiling applies (Policy, or a channel family).
  double wing_mult_cap = 0.0;
};

// The de-Americanised IV cache's part in one fit (QuoteSliceFitSpace::
// VolChannel; all zero otherwise). The counters are the work THIS fit added to
// the chain's cache: zero inversions on a complete reuse.
struct QuoteSliceDeamDiagnostics {
  bool used = false;              // quote volatilities were read from the cache
  bool reused = false;            // the cache was built by an earlier fit (deam_cache)
  std::size_t inversions = 0;     // American IV inversions (fallbacks included)
  std::size_t model_prices = 0;   // American prices of the band edges (kIvMin, kIvMax)
  std::size_t accurate_fallbacks = 0; // Fast inversions redone on the accurate kernel
  std::size_t premium_inversions = 0; // inversions attempted by the Premium strategy
  std::size_t premium_fallbacks = 0;  // ... of which handed to Search
  // Seed-tier wiring (QuoteSliceDeamProfile::Screen; zero otherwise): band edges whose
  // comparison lay inside the seed tier's bound and were re-priced on the accurate
  // kernel; accepted Premium roots confirmed by a solve on the confirm kernel; roots
  // whose confirm the band gate skipped (deam_confirm); inversions whose iterates were
  // routed to the accurate kernel because the price lay inside the seed tier's
  // non-monotone gate.
  std::size_t ambiguous_edges = 0;
  std::size_t confirms = 0; // confirming SOLVES (a root may take kIvPremiumConfirmSolves = 2)
  std::size_t confirms_skipped = 0;
  std::size_t seed_routed = 0;
  // Sigma-family iterates (deam_family; zero otherwise): families this fit built (0 when
  // the cache was reused), iterates priced on a family, Premium attempts a family refused.
  std::size_t family_builds = 0;
  std::size_t family_prices = 0;
  std::size_t family_refused = 0;
  // The native fit route (deam_native_route; zero otherwise): inversions answered on the
  // bounded reference route and its kernel evaluations, requests refused for an exhausted
  // reference budget, typed non-identified outcomes (price-only, interval), band edges
  // decided by validated bounds or priced, and certification-grid nodes priced on a family
  // under its per-point bound with the largest such bound (USD).
  std::size_t reference_inversions = 0;
  std::size_t reference_evaluations = 0;
  std::size_t reference_budget_exhausted = 0;
  std::size_t price_only_outcomes = 0;
  std::size_t interval_outcomes = 0;
  std::size_t edges_decided = 0;
  std::size_t edges_priced = 0;
  std::size_t grid_family_prices = 0;
  double grid_max_family_bound_usd = 0.0;
  // The batched native route (deam_native_batch; zero otherwise): lanes this fit inverted in
  // invert_many batches, requests that consumed a prepared lane, first confirm prices
  // evaluated in a batch and requests that consumed one, and reference requests refused by
  // the wall budget (deam_reference_max_wall_us; also in reference_budget_exhausted).
  std::size_t batch_lanes = 0;
  std::size_t batch_lane_hits = 0;
  std::size_t batch_confirms = 0;
  std::size_t batch_confirm_hits = 0;
  std::size_t reference_wall_refused = 0;
};

struct QuoteSliceDiagnostics {
  std::size_t quotes_seen = 0;
  std::size_t iv_attempted = 0;
  std::size_t quotes_admitted = 0;
  std::size_t distinct_strikes = 0;
  std::size_t prices_produced = 0; // rows carrying a certified price (Success or PriceOnly)
  std::size_t weights_capped = 0;
  double forward = 0.0;
  double discount = 0.0;
  double effective_rate = 0.0;
  double effective_yield = 0.0;
  FitDiag curve_fit{};
  QuoteSliceChannelDiagnostics channel{};
  // Channel repair of this slice's own certificate (QuoteSliceConfig::repair).
  QuoteSliceRepairRecord repair{};
  // Box certificate of this slice's fitted prices (QuoteSliceRepairConfig::
  // certificate Box): Applied with the price moves (max_vol_move is not
  // measured: the box bounds the price), Infeasible when no arbitrage-free
  // curve exists inside the quote boxes, NotCertified when the in-box curve
  // failed a cross-side check; NotAttempted under Exact or a passed certificate.
  QuoteSliceRepairRecord box{};
  QuoteSliceDeamDiagnostics deam{};
  // Lane C3 (QuoteSliceCallFloor::DiscountedIntrinsic): the call-floor lines this expiry
  // is held to (calendar_years at most its own; empty under Legacy), the call nodes the
  // floor lifted, and whether a lift was refused because the lifted nodes failed the
  // certificate (the historical prices are kept).
  std::vector<QuoteSliceCallFloorLine> call_floor_lines;
  std::size_t call_floor_lifted = 0;
  bool call_floor_refused = false;
};

struct QuoteSliceCarryPoint {
  double strike = 0.0;
  double continuous_yield = 0.0; // calendar annualized, excluding cash dividends
};

// An immutable serving snapshot when published by the caller. The curve owns
// its fitted state, and the schedule is copied, so query lifetime is independent
// of every input span and CurveConfig (including a caller's spline-knot span).
struct QuoteSliceFit {
  QuoteSliceFitStatus fit_status = QuoteSliceFitStatus::InsufficientQuotes;
  std::string fit_error;
  QuoteSliceDiagnostics diagnostics;
  std::unique_ptr<IVolCurve> curve;
  // The accepted serving representation. Its owned piecewise-linear American
  // prices enforce strike shape, exercise bounds and conservative cross-side
  // bounds throughout its finite domain. The intermediate IV curve is NOT a
  // certified serving representation. Absence means the fit cannot be served.
  // The SERVED representation is this curve restricted to its support (withheld_nodes):
  // serve through query_quote_slice / evaluate_quote_slice / served_quote_slice_price,
  // which refuse every withheld strike, never through prices->price directly.
  std::optional<AmericanPriceSlice> prices;
  // Empty unless a repair moved served prices off the curve. Otherwise one entry
  // per prices->nodes() entry: value 1 = call moved, 2 = put moved (OR'd). At
  // such a node the curve's volatility is not the served price's.
  std::vector<std::uint8_t> moved_nodes;
  // Empty unless a row became PriceOnly. Otherwise one entry per
  // prices->nodes() entry: value 1 = call, 2 = put (OR'd), the node sides whose
  // served price had no identifiable volatility. They stay price-only for the
  // life of the fit: evaluate_quote_slice refuses their volatility, and a later
  // calendar repair refreshes their price but never re-implies them.
  std::vector<std::uint8_t> price_only_nodes;
  // Empty unless a row's certified price was refused (lane C2: a MarketDistanceFailure
  // row). Otherwise one entry per prices->nodes() entry: value 1 = call, 2 = put (OR'd),
  // the node sides whose certified value is WITHHELD from the served representation. A
  // withheld node side and every strike strictly between it and its neighbouring nodes
  // (where the interpolation reads its value) lie outside the support: query_quote_slice
  // answers MarketDistanceFailure there and evaluate_quote_slice / served_quote_slice_price
  // refuse. quote_slice_gaps lists the gaps. Every row of a withheld node side is refused.
  std::vector<std::uint8_t> withheld_nodes;
  std::vector<QuoteSliceRow> rows; // same order and length as input quotes
  QuoteSliceMarket market;
  QuoteSliceDividendModel dividend_model = QuoteSliceDividendModel::ContinuousYield;
  std::optional<AlOpts> al_opts;
  int discrete_div_steps = kDiscreteDivDefaultSteps;
  DiscreteDivScheme discrete_div_scheme = DiscreteDivScheme::SplicedCrr;
  std::vector<CashDividend> variance_clock_dividends;
  // Empty when no quote supplies a yield override. Otherwise owns one
  // resolved carry per valid contract strike, including rejected bid/ask rows.
  // Strictly increasing strikes; interpolation is linear in log strike, with
  // flat edges. Conflicting same-strike carries are refused during fitting.
  std::vector<QuoteSliceCarryPoint> carry_curve;
  // How the expiry carry was resolved (quote_slice_carry.hpp). Under an applied
  // implied forward, `market` holds the implied yield the fit actually used.
  QuoteSliceCarryResolution carry_resolution{};
  // The fit's identifiability policy (QuoteSliceConfig::floor_policy), kept so a later
  // calendar repair re-implies moved rows under the same rule.
  QuoteSliceFloorPolicy floor_policy = QuoteSliceFloorPolicy::Legacy;
  // QuoteSliceRepairConfig::calendar_parity_clamp of the fit's config (lane K4b), kept so a
  // later repair_quote_slice_calendar applies the same rule.
  bool calendar_parity_clamp = false;
  // Caller-supplied snapshot provenance (clocks, spot and carry versions). fit_quote_slice
  // leaves it default; fit_quote_surface copies QuoteSurfaceExpiry::snapshot.
  QuoteSliceSnapshotInfo snapshot{};
  // Lane H1 (phase B). Both empty by default; neither changes a fitted or served value.
  // serve_handoff: what the last certification of this fit (certify_and_serve) read from
  // its (sigma, q) families - set only when the fit ran with deam_family_q - for the
  // serve_build stage's record builder (build_serve_record). serve_record: a serving
  // record built from THIS fit and attached by the caller; when present,
  // evaluate_quote_slice answers a between-node or moved-node volatility from the record's
  // family where lane K3's enclosure accepts the root, and as without it everywhere else
  // (slice_serve_record.hpp).
  std::shared_ptr<const QuoteSliceServeHandoff> serve_handoff;
  std::shared_ptr<const SliceServeRecord> serve_record;
  // Runtime-only immutable ownership of the native fit's American family and model
  // inputs and current typed identifications at repaired nodes. With native fitting
  // and PriceOnly enabled, repairs reuse its qualified inverse and queries reuse the
  // published identification. Model/price changes invalidate the corresponding reuse.
  // Archives do not persist this acceleration: restored fits retain certified prices
  // but use historical Search and do not retain the native IV root certificate.
  std::shared_ptr<const QuoteSliceReimplyContext> reimply_context;
};

// The American model of a fit: the dated-cash lattice for DatedCash, else Andersen-Lake.
[[nodiscard]] QuoteSliceModelKind model_kind(const QuoteSliceFit &fit) noexcept;

struct QuoteSliceValue {
  double volatility = 0.0;
  double price = 0.0;
};

// A typed query answer (query_quote_slice). status Success: `volatility` is identified
// (vol_convention FittedCurve at an unmoved node, ReimpliedRepaired otherwise). status
// PriceOnly: `price` is certified and served, `volatility` is NaN, `reason` says why and
// [iv_lower, iv_upper] is the interval where known (NaN otherwise). status
// MarketDistanceFailure (lane C2): the strike lies inside the certified domain but outside
// the served support (QuoteSliceFit::withheld_nodes); no price and no volatility.
struct QuoteSliceQuery {
  QuoteSliceEvaluationStatus status = QuoteSliceEvaluationStatus::NoFit;
  QuoteSlicePriceOnlyReason reason = QuoteSlicePriceOnlyReason::None;
  QuoteSliceVolConvention vol_convention = QuoteSliceVolConvention::None;
  double price = std::numeric_limits<double>::quiet_NaN();
  double volatility = std::numeric_limits<double>::quiet_NaN();
  double iv_lower = std::numeric_limits<double>::quiet_NaN();
  double iv_upper = std::numeric_limits<double>::quiet_NaN();
};

// One authoritative spot per snapshot (lane C0): the expiry re-expressed on the snapshot
// spot S*. `market` is the input with spot = S* and continuous_yield + yield_shift;
// every contract yield override moves by the same yield_shift (calendar clock, as
// QuoteSliceMarket::continuous_yield), so each forward under the declared dividend model
// is unchanged: ContinuousYield / AggregateFundingPresentValue ln(S*/S)/T_cal;
// AggregatePresentValue ln((S* - PV)/(S - PV))/T_cal; DatedCash solves the lattice
// forward S e^{(r-q)T} - sum D e^{(r-q)(T-tau)} (calendar tau, as fit_quote_slice)
// rather than the continuous formula. FORWARD-preserving only (QuoteSliceCarryConvention::
// ForwardPreserving): American prices are not preserved.
struct QuoteSliceSpotRebase {
  QuoteSliceMarket market;
  double yield_shift = 0.0;
  double forward = std::numeric_limits<double>::quiet_NaN(); // the preserved forward
};

// InvalidArgument: a non-finite or non-positive spot, S*, clock or rate input, a dividend
// span outside DatedCash, or an unknown model. OutOfRange: no finite carry keeps the
// forward (for example S* at or below the escrowed PV).
[[nodiscard]] Result<QuoteSliceSpotRebase>
rebase_quote_slice_spot(const QuoteSliceMarket &market, double snapshot_spot,
                        QuoteSliceDividendModel model,
                        std::span<const CashDividend> calendar_dividends = {});

// Explicit input normalization for duplicated strikes with slightly different supplied
// carries. The canonical carry is the midpoint of the group's extrema (order independent).
// Accept only when a bound on the resulting American call/put price change, uniformly in
// strike and volatility, is at most max_price_change. Quote boxes are never changed.
// Zero budget retains exact equality. Material conflicts, non-finite inputs and any
// conflicting DatedCash carry refuse without changing the input span. O(n log n) cold work.
struct QuoteSliceCarryCoalescing {
  std::size_t strikes_changed = 0;
  double max_abs_yield_change = 0.0;
  double max_price_change_bound = 0.0;
};

[[nodiscard]] Result<QuoteSliceCarryCoalescing>
coalesce_quote_slice_carry(const QuoteSliceMarket &market, std::span<QuoteSliceQuote> quotes,
                           QuoteSliceDividendModel model, double max_price_change);

// Fits raw American bid/ask quotes, then reprices every valid contract with the
// fitted smile, including quotes excluded from fitting. No reference fair
// values or reference volatilities are accepted by this API.
// Per-contract yields are used for IV extraction and repricing. The fitted
// volatility curve retains the COMMON market-reference forward coordinate;
// European observation prices/spreads are transformed to that coordinate.
// Weights use local-forward Black vega as a quote-sensitivity proxy. Quality
// gates act before curve fitting, using QuoteSliceConfig::quality for ALL curve
// kinds; CurveConfig::parametric observation-builder gates do not run here.
//
// DatedCash dividends use CALENDAR tau measured from the same valuation instant
// as calendar_years. The fitter assumes a constant variance/calendar clock-speed
// ratio at every intermediate exercise time. Two maturity totals cannot encode
// session-dependent clocks; those require an event/time-dependent model.
// Other models require an empty event span;
// ContinuousYield also requires dividend_pv == 0.
//
// Shared malformed inputs return Err. Quote-level failures are retained in rows;
// insufficient data or a curve failure returns Ok with a non-Success fit_status.
// Non-finite yield overrides and conflicting resolved carries at the same
// valid contract strike are shared InvalidArgument errors, never averaged.
// The cold fitting path allocates O(quotes + dividends) scratch. Serving reuses
// the immutable curve; the discrete-cash engine retains its own lattice cost.
[[nodiscard]] Result<QuoteSliceFit>
fit_quote_slice(const QuoteSliceMarket &market, std::span<const QuoteSliceQuote> quotes,
                const QuoteSliceConfig &config = {},
                std::span<const CashDividend> calendar_dividends = {});

// Query inside the certified price domain. At a grid node the fitted IV is
// cached through the immutable curve, except at a node a repair moved off the
// curve (moved_nodes), which is inverted like a point between nodes, and at a
// node side served price-only (price_only_nodes), whose volatility is refused
// with OutOfRange exactly like the PriceOnly row it serves. Between
// nodes, invert the certified piecewise-linear price using the snapshot's local
// carry and American model; this keeps returned price and IV consistent.
// Outside-domain requests fail.
// Every answer query_quote_slice types PriceOnly is refused with OutOfRange (lane
// C0): a volatility at or below kIvMin is never returned as an identified IV.
// A strike outside the served support (QuoteSliceFit::withheld_nodes, lane C2) is refused
// with Unavailable ("withheld"): a refused row's certified value is never returned.
// For allocation-free PRICE-ONLY serving, call served_quote_slice_price(fit, K, side).
// A slice enforces strike constraints; publish several expiries together only
// after check_american_calendar has accepted their overlapping price domains.
// Fast path (lane H1, opt-in by QuoteSliceFit::serve_record): a between-node or moved-node
// query whose record family root lane K3's enclosure accepts returns the same certified
// price with that root (within the record's iv_tolerance of the accurate root) instead of
// the cold inversion; every other query is answered as without a record.
[[nodiscard]] Result<QuoteSliceValue> evaluate_quote_slice(const QuoteSliceFit &fit, double strike,
                                                           Side side);

// The typed form of evaluate_quote_slice (lane C0): the same certified price and
// volatility routes, but an unidentified volatility is a PriceOnly answer carrying the
// price, the reason and the interval where known (QuoteSlicePriceOnlyReason), never an
// error. The classification is always on (independent of the fit's floor_policy). A node
// side the fit flagged price-only (price_only_nodes; final for the life of the fit) is
// always PriceOnly: with the reason its current certified price classifies to, or
// UnidentifiedInterval when that price would now identify a volatility (a later calendar
// repair moved it). A strike outside the served support (lane C2) is a
// MarketDistanceFailure answer with no price and no volatility. Err exactly where
// evaluate_quote_slice fails for other reasons: an invalid contract (InvalidArgument),
// no certified slice (Unavailable), a strike outside the certified domain (OutOfRange).
[[nodiscard]] Result<QuoteSliceQuery> query_quote_slice(const QuoteSliceFit &fit, double strike,
                                                        Side side);

// ── Served support (lane C2, 2026-10-22) ─────────────────────────────────────────

// The served price of (strike, side): the certified price when the strike lies in the
// served support, nullopt otherwise (no certified slice, outside the domain, an invalid
// side, or a withheld strike: a withheld node side or a strike strictly between it and a
// neighbouring node). Allocation-free; never throws.
[[nodiscard]] std::optional<double> served_quote_slice_price(const QuoteSliceFit &fit,
                                                             double strike, Side side) noexcept;

// One gap of the served support of one side: the strikes in [lo, hi] are withheld, except
// an endpoint flagged open (that endpoint is a served node). Adjacent withheld segments
// merge into one gap.
struct QuoteSliceGap {
  Side side = Side::Call;
  double lo = 0.0;
  double hi = 0.0;
  bool lo_open = false;
  bool hi_open = false;
};

// The support/gap mask of a fit, calls first then puts, each in strike order; empty when
// nothing is withheld. Allocates.
[[nodiscard]] std::vector<QuoteSliceGap> quote_slice_gaps(const QuoteSliceFit &fit);

// A smile prior for one expiry: an ATM volatility and a shape as multiples
// sigma/atm_vol on an increasing standardized grid z = ln(K/F)/(atm_vol*sqrt(T))
// (variance clock), with the effective strength already applied. Spans are
// borrowed for the duration of the call only.
struct QuoteSlicePrior {
  double atm_vol = 0.0;
  std::span<const double> z;
  std::span<const double> multiples;
  double strength = 0.0;
};

// Channel knot-spline fit of one expiry (QuoteSliceFitMethod::ChannelSpline),
// optionally regularized toward `prior` instead of the flat default shape.
// Every valid contract contributes its bid/ask channel, inverted with the SAME
// American model and per-contract carry as the point path; quote statuses and
// admission provenance are reported exactly as by the point path. Final prices
// pass the same mandatory AmericanPriceSlice certification. Fewer than
// `channel.min_channel_quotes` usable channels returns InsufficientQuotes; a
// numerical failure of the spline (or of `channel.family`'s first refit)
// returns FitFailure. Requires supplied carry (config.carry.mode == Supplied);
// fit_quote_slice resolves carry before its prior-free channel fit.
[[nodiscard]] Result<QuoteSliceFit>
fit_quote_slice_channel(const QuoteSliceMarket &market, std::span<const QuoteSliceQuote> quotes,
                        const QuoteSliceConfig &config, const QuoteSlicePrior *prior,
                        std::span<const CashDividend> calendar_dividends = {});

// Bounded-move calendar repair. Moves the certified node prices of `fit` into
// [max over `earlier` slices, min over `later` slices] wherever a node lies
// inside their domains, restores convexity with the greatest convex minorant,
// and accepts only when every node moved by at most `half_spreads` of its own
// quote half-spread (bidless: half the ask) FROM ITS PREVIOUS PRICE, the clamp
// remains satisfied and the unchanged AmericanPriceSlice certificate passes.
// The bound limits the move; it does not keep a price inside [bid, ask]. On
// success replaces fit.prices, flags moved nodes in fit.moved_nodes, refreshes
// the prices of Success and PriceOnly rows (a moved node of a Success row is
// re-implied: the volatility of the served price, or PriceOnly with the
// certified price and a NaN volatility, flagged in fit.price_only_nodes, when
// that price has no identifiable volatility; a PriceOnly row stays PriceOnly
// and is never re-implied) and returns true; otherwise leaves `fit` unchanged.
// The largest required move, in half-spreads, is written to `required` when
// non-null. The caller must still check the result against every accepted
// slice. `recheck` (lane C2, review-L3q m3): Recheck also refuses a result of a
// non-dividend slice that fails check_nondividend_american (the certificate the slice
// was first served under); Skip is the historical rule, which fit_quote_surface keeps on
// its default path (QuoteSurfacePublication::Off) so the default surface stays bit for
// bit.
enum class QuoteSliceNondividendRecheck : std::uint8_t { Recheck, Skip };

[[nodiscard]] bool repair_quote_slice_calendar(
    QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
    std::span<const AmericanPriceSlice *const> earlier,
    std::span<const AmericanPriceSlice *const> later, double half_spreads,
    double *required = nullptr,
    QuoteSliceNondividendRecheck recheck = QuoteSliceNondividendRecheck::Recheck);

// Calendar repair inside the bid/ask channel: the certified node prices of
// `fit` are projected exactly as by the channel repair (QuoteSliceRepairConfig),
// with every node's box further intersected with [max over `earlier`, min over
// `later`] wherever it lies inside their domains, and acceptance follows the
// same channel, node and volatility budgets (`budget.mode` is ignored) instead
// of a half-spread bound. On success replaces fit.prices, ORs the moved nodes
// into fit.moved_nodes, refreshes row prices and volatilities exactly as
// repair_quote_slice_calendar does and returns true; otherwise leaves `fit`
// unchanged. The repaired slice must pass the unchanged certificate AND
// check_american_calendar against every slice in `earlier` and `later` (the
// clamp acts at this slice's own nodes only, so a neighbour's knot between
// them can still cross); either failure is NotCertified. `record` (when
// non-null) receives what the repair did or why it was refused. Under
// `budget.certificate` Box (lane L3r1) the boxes are the box certificate's quote
// boxes [bid - h, ask + h] instead of the fitted-widened channels and no node or
// volatility budget applies (the box is the budget): a calendar crossing is
// repaired exactly when the quotes can absorb it, and Infeasible otherwise.
// `recheck` as for repair_quote_slice_calendar (lane C2 fix round 1, review m4):
// Recheck, the default, also refuses (NotCertified) a non-dividend result that fails
// check_nondividend_american. Skip is the historical rule, which fit_quote_surface keeps
// unless QuoteSurfacePublication::Validate. An invalid value is refused.
[[nodiscard]] bool repair_quote_slice_calendar_in_channel(
    QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
    std::span<const AmericanPriceSlice *const> earlier,
    std::span<const AmericanPriceSlice *const> later, const QuoteSliceRepairConfig &budget,
    QuoteSliceRepairRecord *record = nullptr,
    QuoteSliceNondividendRecheck recheck = QuoteSliceNondividendRecheck::Recheck);

// ── Distance to market (lane L3q, 2026-10-21; QuoteSliceMarketDistance Repair) ──

// In-box market repair of one served slice: its certified node prices are projected
// (project_convex_price_box, uniform weights) per side onto the quote boxes [bid, ask]
// (bidless [0, ask]; a side without a valid quote held at its price) intersected with
// the certificate's hard bounds and the calendar clamp of `earlier` / `later` (as
// repair_quote_slice_calendar_in_channel); when no arbitrage-free curve fits those
// boxes, once more onto the bound's band [bid - b s, ask + b s] (b =
// config.market_distance_spreads, s = max(ask - bid, config.box_tick)); a band result that
// moves a row lying inside its own quote [bid, ask] out of it is refused (LeftChannel:
// lane C2, the renewed market check). The result
// must move some node by more than config.immaterial_price_move, pass the unchanged
// certificate and check_american_calendar against every slice in `earlier` and
// `later`; it is then committed exactly as a calendar repair (fit.prices, row prices,
// moved nodes re-implied or PriceOnly, fit.moved_nodes) and true is returned.
// Otherwise `fit` is unchanged. `record` (nullable) receives the outcome and moves.
// `recheck` works as for repair_quote_slice_calendar_in_channel.
[[nodiscard]] bool repair_quote_slice_market(
    QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
    std::span<const AmericanPriceSlice *const> earlier,
    std::span<const AmericanPriceSlice *const> later, const QuoteSliceRepairConfig &config,
    QuoteSliceRepairRecord *record = nullptr,
    QuoteSliceNondividendRecheck recheck = QuoteSliceNondividendRecheck::Recheck);

// One slice of a bidirectional calendar repair: its fit (certified prices, non-null)
// and its quotes (the fit's row order).
struct QuoteSliceJointMember {
  QuoteSliceFit *fit = nullptr;
  std::span<const QuoteSliceQuote> quotes;
};

// An accepted slice that stays fixed during a bidirectional repair, with its
// calendar maturity (it is a floor for later members and a cap for earlier ones).
struct QuoteSliceCalendarNeighbour {
  const AmericanPriceSlice *prices = nullptr;
  double calendar_years = 0.0;
};

// Bidirectional calendar repair (lane L3q): the members' certified prices (distinct
// maturities, any order) are projected JOINTLY per side (project_calendar_joint) —
// each member inside its own boxes, the quote boxes [bid - h, ask + h] (h =
// config.box_half_ticks * config.box_tick under QuoteSliceCertificate::Box, else 0;
// bidless [0, ask + h]; unquoted sides held) widened to contain the node's current
// price (no node ends farther outside its quote than it started), intersected with
// the hard bounds and the calendar clamp of the `fixed` neighbours — subject to every
// member being convex with the certificate's end slopes and calendar-ordered against
// every other member. Accepted only
// when every member then passes the unchanged certificate and check_american_calendar
// against every other member and every fixed neighbour; then each member that moved
// is committed as a calendar repair, its moved node count written to `moved` (one
// entry per member, nullable), and true is returned. Otherwise no member changes.
// `record` (nullable) receives the outcome, the summed node counts and the largest
// move over all members. Bounded: one projection per side (lane C2:
// project_calendar_joint_bounded with config.joint_max_sweeps; an exact infeasibility
// certificate is Infeasible with record->conflict, a spent budget NonConverged, and either
// rolls back: no member changes; record->sweeps counts the sweeps run). `recheck` as for
// repair_quote_slice_calendar: Recheck (the default) also refuses (NotCertified) a result
// in which a non-dividend member fails check_nondividend_american; Skip is the historical
// rule, which fit_quote_surface keeps unless QuoteSurfacePublication::Validate, so a
// keys-ON repair arm serves every row its base served (PM ruling 2026-10-23); an invalid
// value is refused (false, nothing changes).
[[nodiscard]] bool repair_quote_slice_calendar_joint(
    std::span<const QuoteSliceJointMember> members,
    std::span<const QuoteSliceCalendarNeighbour> fixed, const QuoteSliceRepairConfig &config,
    std::vector<std::size_t> *moved = nullptr, QuoteSliceRepairRecord *record = nullptr,
    QuoteSliceNondividendRecheck recheck = QuoteSliceNondividendRecheck::Recheck);

// Every repair above re-certifies its result before committing it, with the unchanged
// AmericanPriceSlice certificate. Lane C2 added a second check: the non-dividend
// certificate the slice was first served under, check_nondividend_american. It applies to
// a non-dividend slice (zero carry at every node, no cash dividend, nonnegative rate), and
// only when the repair is called with QuoteSliceNondividendRecheck::Recheck. That is the
// default for a direct caller. fit_quote_surface passes Recheck to all four repairs
// (bounded-move, channel / box, market, joint) only under QuoteSurfacePublication::Validate,
// and Skip otherwise (the historical rule). Without Validate, a surface can therefore
// serve a repaired non-dividend slice that fails the non-dividend certificate. A failure
// of either check is NotCertified and leaves the fit unchanged.

// ── Publication validation (lane C2, 2026-10-22) ─────────────────────────────────
//
// The final representation-level check of what API callers evaluate: the served price
// function (served_quote_slice_price - the certified curve on its support) of every given
// slice, at the evaluation points dense_points interior points per node segment, every
// node (quote strikes are nodes) and every other slice's nodes inside its domain.
// Violation classes:
//  Certificate:   the published nodes fail AmericanPriceSlice::create.
//  Shape:         at served evaluation points a call rises or a put falls, a slope leaves
//                 [-discount_max, 0] (calls) / [0, discount_max] (puts), a second
//                 difference is negative, or a price leaves its exercise / upper bound.
//  CrossSide:     where both sides are served, call - put leaves the certificate's affine
//                 bounds [prepaid_min - discount_max K, prepaid_max - discount_min K].
//  Nondividend:   a non-dividend slice fails check_nondividend_american.
//  QuoteBox:      a served row's price lies beyond market_distance_spreads spreads outside
//                 its own quote (an infinite market_distance_spreads: not checked).
//  Calendar:      for EVERY pair of slices (not adjacent maturities only: a nested domain
//                 can cross a non-neighbour), a later served price below an earlier one at
//                 the union of both slices' nodes and the midpoints between them.
//  Extrapolation: a strike just outside the certified domain is served.
//  Support:       a row disagrees with the API at its strike: a served row (Success or
//                 PriceOnly) whose strike the API refuses or whose price differs from the
//                 served price, or a MarketDistanceFailure row whose strike is served.
//  Identification: a served row's labels disagree with query_quote_slice's typed answer for
//                 its contract (lane C0's one answer per contract): an identified answer
//                 that is not the row's Success volatility and convention bit for bit, or
//                 an unidentified one whose reason / interval bits the row does not carry
//                 with no row convention (under PriceOnly the row must be PriceOnly).
// Every comparison allows the certificate's 1e-8 + 1e-9 * max(spot, K). `focus` (an index)
// restricts the check to that slice and the calendar pairs it belongs to (the after-
// mutation form); npos checks every slice and pair. Pure: reads the fits only.
enum class QuoteSlicePublicationViolation : std::uint8_t {
  Certificate,
  Shape,
  CrossSide,
  Nondividend,
  QuoteBox,
  Calendar,
  Extrapolation,
  Support,
  Identification
};
inline constexpr std::size_t kQuoteSlicePublicationViolationKinds = 9;
inline constexpr std::size_t kQuoteSlicePublicationMaxFindings = 64;
inline constexpr std::size_t kQuoteSlicePublicationAll = static_cast<std::size_t>(-1);

[[nodiscard]] const char *to_string(QuoteSlicePublicationViolation value) noexcept;

// One violation: slice `expiry` (index into the checked span), the calendar partner `other`
// (kQuoteSlicePublicationAll otherwise), the row (QuoteBox / Support / Identification; else
// kQuoteSlicePublicationAll), side, strike and the excess beyond the allowance (price
// units; QuoteBox: spreads beyond the bound).
struct QuoteSlicePublicationFinding {
  QuoteSlicePublicationViolation kind = QuoteSlicePublicationViolation::Certificate;
  std::size_t expiry = 0;
  std::size_t other = kQuoteSlicePublicationAll;
  std::size_t row = kQuoteSlicePublicationAll;
  Side side = Side::Call;
  double strike = std::numeric_limits<double>::quiet_NaN();
  double excess = 0.0;
};

struct QuoteSlicePublicationReport {
  std::size_t slices = 0;          // slices checked
  std::size_t points = 0;          // served evaluation points (per side)
  std::size_t calendar_pairs = 0;  // pairs with overlapping domains
  std::size_t withheld_rows = 0;   // rows the API refuses (MarketDistanceFailure)
  std::size_t gap_segments = 0;    // withheld (side, node segment) pairs
  std::array<std::size_t, kQuoteSlicePublicationViolationKinds> violations{};
  std::vector<QuoteSlicePublicationFinding> findings; // the first kQuoteSlicePublicationMaxFindings
  [[nodiscard]] std::size_t total() const noexcept;
  [[nodiscard]] std::size_t count(QuoteSlicePublicationViolation kind) const noexcept;
  [[nodiscard]] bool clean() const noexcept { return total() == 0u; }
};

// One published slice: its fit (certified prices, non-null) and its quotes (row order).
struct QuoteSlicePublicationSlice {
  const QuoteSliceFit *fit = nullptr;
  std::span<const QuoteSliceQuote> quotes;
};

struct QuoteSlicePublicationConfig {
  double market_distance_spreads = std::numeric_limits<double>::infinity(); // >= 0
  double box_tick = 0.01;                                // > 0, finite
  std::size_t dense_points = 3;                          // interior points per segment, <= 64
  std::size_t focus = kQuoteSlicePublicationAll;
  // The Identification class (one query per served row; a moved node's re-implication).
  bool identification = true;
};

// InvalidArgument on a null fit, a fit without certified prices, rows not matching the
// quotes, equal calendar maturities, a focus out of range or an invalid config.
[[nodiscard]] Result<QuoteSlicePublicationReport>
validate_published_representation(std::span<const QuoteSlicePublicationSlice> slices,
                                  const QuoteSlicePublicationConfig &config = {});

// ── Graded admission over one valuation surface ──────────────────────────────

// Confidence grade carried by every served expiry.
//  A: the expiry's own admitted point quotes satisfy the profile minimum
//     (min_quotes admitted quotes and distinct strikes) and pass A served it with
//     the profile's own method (fit_quote_slice: the point fitter, or for a
//     ChannelSpline profile the channel fit with only the flat default shape),
//     with no term prior and no served price moved by a repair.
//  B: a channel refit (under the leave-one-out term prior when another expiry
//     supplies a node, else the flat default shape) with at least
//     `grade_b_min_mid_strikes` distinct mid strikes, or a pass-A slice whose
//     served prices a convexity or calendar repair moved. Opt-in rungs are B as
//     well: a pass-A fit served only after the channel repair (repair rung), and
//     a pass-A point fit that failed the channel-dominance gate and was kept
//     because its channel refit was not calendar-consistent (point fallback). A
//     dominance refit is graded as any channel refit. A repaired slice is never A.
//  C: a channel refit (same prior rule) with fewer mid strikes (dominated by
//     bid/ask bounds and the prior or default shape).
enum class QuoteSliceGrade : std::uint8_t { A, B, C, Refused };

[[nodiscard]] const char *to_string(QuoteSliceGrade value) noexcept;

struct QuoteSurfaceExpiry {
  QuoteSliceMarket market;
  std::span<const QuoteSliceQuote> quotes;
  std::span<const CashDividend> calendar_dividends{};
  // Caller-supplied provenance, copied onto the expiry's QuoteSliceFit::snapshot (lane C0).
  QuoteSliceSnapshotInfo snapshot{};
};

// The shared call-floor lines of one snapshot's expiries (lane C3, QuoteSliceCallFloor):
// one {calendar_years, spot, exp(-rate * calendar_years)} line per expiry that is
// non-dividend on its inputs - rate >= 0, zero continuous yield on the market and on every
// quote override, zero dividend PV and no calendar dividend - in input order. The regime is
// the one check_nondividend_american certifies (lane C2's detail::nondividend_discount);
// a carry-shifted expiry (snapshot.one_spot) is not non-dividend. Allocates.
[[nodiscard]] std::vector<QuoteSliceCallFloorLine>
quote_surface_call_floor_lines(std::span<const QuoteSurfaceExpiry> expiries);

// Admission ladder of fit_quote_surface. Off is the historical point-first
// admission, bit for bit. KnotFirst (opt-in, point profiles only) is described
// at fit_quote_surface.
enum class QuoteSurfaceLadder : std::uint8_t { Off, KnotFirst };

// Knot-first ladder wing tail (lane L3a, opt-in; Off is the historical path, bit
// for bit; refused without the ladder). FloorPriceOnly: after the ladder's
// calendar pass, a served Success row whose volatility was RE-IMPLIED from a
// repaired price (its node side is in QuoteSliceFit::moved_nodes) and equals the
// inverter's reported floor kIvMin is published PriceOnly - certified price kept,
// NaN volatility, flagged in QuoteSliceFit::price_only_nodes - exactly as when the
// re-implication fails. A repair that moves a zero-time-value wing node (a deep
// out-of-the-money side, or an in-the-money side at its exercise value) leaves a
// price at or below the zero-volatility value, for which american_implied_vol
// reports kIvMin (american_iv.hpp): a bound, not a measured volatility. Prices,
// certificates, containment and every calendar decision are unchanged.
// FloorPriceOnlyWingFloor: FloorPriceOnly, and every channel fit of the surface
// (the knot rung, its repair rung, every channel refit and retry) runs with
// QuoteSliceChannelConfig::wing_floor, so a knot wing never droops below its
// outermost lower-bounded quote's fitted level; this one changes fitted prices.
enum class QuoteSurfaceWingTail : std::uint8_t { Off, FloorPriceOnly, FloorPriceOnlyWingFloor };

[[nodiscard]] const char *to_string(QuoteSurfaceWingTail value) noexcept;

// Publication validation of fit_quote_surface (lane C2, opt-in; Off is the historical
// path bit for bit). Validate: (1) after EVERY mutation of a served or accepted slice's
// prices (bounded-move, channel, joint and market repairs) the mutated slice is checked by
// validate_published_representation (focus) against the slices it was made consistent
// with; a violation rolls the mutation back (the repair is treated as refused) and counts
// QuoteSurfaceSlice::publication_rollbacks; the check runs on the repair trial after the
// library commit, i.e. after lane C0's align_with_query, so the labels it validates are
// the ones published; (2) last, after the market-distance pass, the whole served set is
// checked (QuoteBox at the market-distance bound when one is set) until it is clean: a
// slice with a structural violation (Certificate, Shape, CrossSide, Nondividend,
// Calendar, Extrapolation) is refused ("publication_invalid") - for a calendar pair, the
// lower-confidence slice - and a QuoteBox / Support row is refused (MarketDistanceFailure)
// and withheld; Identification findings of the final set are counted only. Independently
// of the mode, the market-distance gate withholds every row it refuses
// (QuoteSliceFit::withheld_nodes), so the API refuses the same contracts.
//
// (3) The enforcement in (1) and (2) is a backstop that no fit reaches today (lane C2 fix
// round 1, review m1). By construction, every mutation path already re-certifies what the
// check validates: the certificate, the calendar against every accepted slice, and the
// non-dividend certificate under Validate. The market-distance gate also removes QuoteBox
// rows first. A test seam (src/fitting/publication_seam.hpp) injects findings to pin the
// rollback and both refusals.
//
// (4) Fit adoption is checked only by the final pass in (2), not per write. That covers
// the certify_and_serve grid write, channel refits and serve_held_point. So any new
// final-grid write or refit adoption must come before the final pass, or go through the
// after-mutation check (lane K4: keep it so).
enum class QuoteSurfacePublication : std::uint8_t { Off, Validate };

[[nodiscard]] const char *to_string(QuoteSurfacePublication value) noexcept;

struct QuoteSurfaceConfig {
  double term_prior_weight = 1.0;     // scale of the neighbour prior's strength
  double prior_decay_log_time = 1.0;  // confidence e-folding in |ln(T/T_nearest)|
  double prior_quote_halflife = 6.0;  // strength * n0 / (n0 + own two-sided strikes)
  double channel_node_weight = 0.5;   // prior-node weight of a channel fit vs a point fit
  double calendar_retry_factor = 4.0; // prior strength multiplier on a calendar retry
  std::size_t grade_b_min_mid_strikes = 4;
  // Bounded-move calendar repair for a slice that crosses accepted ones
  // (repair_quote_slice_calendar); 0 disables it. A repaired pass-A slice is
  // graded B because its served prices are no longer its fit's.
  double calendar_repair_half_spreads = 0.0;
  QuoteSurfaceLadder ladder = QuoteSurfaceLadder::Off;
  // Market containment of the knot-first ladder (unused when it is off): no
  // rung serves a slice whose final prices lie outside [bid, ask] on more than
  // this share of its strictly two-sided quotes (0 < bid < ask) AND on at least
  // ladder_min_outside_quotes of them. ladder_max_outside_fraction governs the
  // point-fit rungs (pass A, the repair rung, the calendar point fallback);
  // ladder_channel_max_outside_fraction the channel-fit rungs (the knot rung,
  // its repair rung, every channel refit), whose objective already penalises
  // leaving the channel but floors a channel's half-width at
  // channel.min_half_width_vol, so a channel fit can sit outside a market that
  // is narrower than that floor. Each in [0, 1]; 1 lets those rungs serve
  // regardless of containment, so only an explicit setting relaxes the rule.
  double ladder_max_outside_fraction = 0.05;
  double ladder_channel_max_outside_fraction = 0.05;
  std::size_t ladder_min_outside_quotes = 1; // >= 1
  QuoteSurfaceWingTail ladder_wing_tail = QuoteSurfaceWingTail::Off; // ladder only
  // Expiry fan-out width of the per-expiry stages (pass A or the ladder's pass
  // A, then the B1 and B2 channel refits), longest expiry first: 1 = serial on
  // the calling thread (the historical path), N = N workers, 0 = auto
  // (ATX_VOL_FIT_WORKERS when set to a positive integer, else
  // hardware_concurrency). It changes speed only. Every expiry writes its own
  // slot and every cross-expiry stage (priors, calendar, repair rungs,
  // dominance) stays serial, so results are bit-identical for any value.
  unsigned fit_workers = 1;
  // Opt-in per-expiry rung memo (lane L2c, 2026-10-19; default off = the historical
  // path). Every expiry's slot owns a memo that all of its per-expiry fits share,
  // whichever thread runs them: (1) under QuoteSliceChannelConfig::deam_cache the
  // expiry's de-Am IV cache lives in the slot instead of the fitting thread's memo, so
  // the expiry's quotes are inverted ONCE per surface fit at any fit_workers; (2) an
  // unchanged candidate (the same chain inputs and the same fitted volatility at every
  // certification strike, as when a repair rung refits the pass-A fit with the repair
  // on) reuses its American certification grid instead of repricing it. Both are
  // exact-keyed, so results are bit-identical with or without the memo. Speed only.
  bool rung_memo = false;
  // Opt-in publication validation (QuoteSurfacePublication, lane C2).
  QuoteSurfacePublication publication = QuoteSurfacePublication::Off;
};

// Which fitter produced a slice's served (or last attempted) fit: the pass-A
// point fitter (also the point repair rung), the pass-A term-prior-free channel
// fit (a ChannelSpline profile's pass A, or the knot-first ladder's knot rung,
// also its repair rung), or a channel refit (term prior or flat default shape:
// B1, B2, the calendar retry).
enum class QuoteSurfaceMethod : std::uint8_t { Point, Channel, ChannelRefit };

// Knot-first ladder only: why the knot rung does not serve the slice. None:
// it does; or the knot slice certified inside the market but lacks the A
// rule's own point evidence, and the slice takes the channel passes as without
// the ladder (method channel_refit when a refit serves). The point fit can
// never serve such a slice at pass A: it shares the knot fit's quote admission,
// so it lacks the same evidence and is refused (InsufficientQuotes). Hence None
// never labels a slice that pass A serves with its point fit; that fallback
// always names the knot rung's failure. Certificate: the knot slice failed its
// price certificate after its bounded-move convexity repair. KnotFailure: too few
// usable channels or a numerical failure. Containment: the knot slice is
// outside the market (QuoteSurfaceConfig::ladder_channel_max_outside_fraction).
// Calendar: the knot slice crossed a higher-confidence slice and its own
// bounded-move calendar repair did not fix it, so a later rung (the point fit,
// clean then repaired, or the stronger-prior retry) serves the slice.
enum class QuoteSurfaceLadderFallback : std::uint8_t {
  None, Certificate, KnotFailure, Containment, Calendar
};

[[nodiscard]] const char *to_string(QuoteSurfaceMethod value) noexcept;
[[nodiscard]] const char *to_string(QuoteSurfaceLadderFallback value) noexcept;

struct QuoteSurfaceSlice {
  // For a refused expiry the fit keeps its diagnostics and row provenance but
  // never carries certified prices or fitted values: its Success and PriceOnly
  // rows become NoFit with a NaN price and volatility (other row statuses keep
  // their reason).
  QuoteSliceFit fit;
  QuoteSliceGrade grade = QuoteSliceGrade::Refused;
  // Empty when served. Otherwise one of: invalid_input, insufficient_quotes,
  // fit_failure, arbitrage_failure, calendar_arbitrage.
  std::string refusal;
  std::string error;                  // detail for invalid_input / fit failures
  QuoteSliceFitStatus point_status = QuoteSliceFitStatus::InsufficientQuotes;
  bool channel_refit = false;         // served or last attempted by the channel fitter
  bool calendar_retry = false;        // a calendar retry was attempted
  bool calendar_repaired = false;     // served after a bounded-move calendar repair
  // For a calendar refusal: the largest price crossing against the accepted
  // slices (earlier minus later, price units) and its strike.
  double calendar_gap = 0.0;
  double calendar_gap_strike = 0.0;
  // Largest node move (in half-spreads) the calendar repair required, over its
  // attempts; infinite when a moved node had no quote. Zero when never tried.
  double calendar_repair_required = 0.0;
  // Opt-in provenance; every field below keeps its default when the options are
  // off, except point_error (the pass-A fit_error, a diagnostic).
  std::string point_error;
  // Repair rung: served by the pass-A fit refitted with the channel repair, and
  // that refit's repair record (also when it was refused). calendar_repaired is
  // also set after a channel calendar repair.
  bool repair_rung = false;
  QuoteSliceRepairRecord rung_repair{};
  // Served after a channel calendar repair; the last such attempt's record.
  bool calendar_channel_repaired = false;
  QuoteSliceRepairRecord calendar_channel_repair{};
  // Dominance gate (QuoteSliceChannelConfig::point_max_outside_fraction),
  // measured on the final point fit after the calendar pass: its share and
  // count of two-sided quotes outside [bid, ask], whether it failed the gate,
  // and whether the point fit was kept because the channel refit was not
  // calendar-consistent (a gated slice that is not a fallback is served by the
  // refit, channel_refit true).
  double point_outside_fraction = 0.0;
  std::size_t point_outside_quotes = 0;
  bool point_gated = false;
  bool point_fallback = false;
  // A calendar refusal served after the gate adopted a refit (its own or a
  // neighbour's), re-checked against the final served set; grade B or C.
  bool calendar_readmitted = false;
  // Fitter of the served (or last attempted) fit; filled with or without the
  // ladder (diagnostic).
  QuoteSurfaceMethod method = QuoteSurfaceMethod::Point;
  // Knot-first ladder only (defaults otherwise): the knot rung's status and
  // error, why it does not serve, and the final served prices' share and count
  // of strictly two-sided quotes outside [bid, ask] (measured before a refused
  // slice is stripped).
  QuoteSurfaceLadderFallback ladder_fallback = QuoteSurfaceLadderFallback::None;
  QuoteSliceFitStatus knot_status = QuoteSliceFitStatus::InsufficientQuotes;
  std::string knot_error;
  double outside_fraction = 0.0;
  std::size_t outside_quotes = 0;
  // Distance-to-market provenance (QuoteSliceMarketDistance, lane L3q; defaults when
  // Off): served rows the gate measured (a valid quote and a price), the rows it
  // refused, the largest distance in spreads before the in-box repair and after it
  // (the served maximum is at most the bound), the in-box repair's record, whether
  // the slice was admitted by the bidirectional calendar rung, and how many of its
  // nodes such a rung moved while it was a fixed-maturity partner of another slice.
  std::size_t market_checked_rows = 0;
  std::size_t market_gated_rows = 0;
  double market_max_spreads = 0.0;
  double market_max_spreads_after = 0.0;
  QuoteSliceRepairRecord market_repair{};
  bool calendar_joint_repaired = false;
  std::size_t calendar_joint_partner_moves = 0;
  // Rows published PriceOnly by QuoteSurfaceConfig::ladder_wing_tail (0 when off).
  std::size_t wing_tail_price_only = 0;
  // Lane C2. Rows withheld from the served support (a refused row and every other row of
  // its node side; QuoteSliceFit::withheld_nodes), whatever the publication mode.
  std::size_t withheld_rows = 0;
  // Bidirectional calendar rung attempts by outcome (project_calendar_joint_bounded):
  // settled, refused by an infeasibility certificate, budget spent. The last two roll
  // back (no member changes).
  std::size_t joint_feasible = 0;
  std::size_t joint_infeasible = 0;
  std::size_t joint_nonconverged = 0;
  // QuoteSurfacePublication::Validate only (zero otherwise): mutations of this slice the
  // after-mutation validation checked and rolled back, the final pass's violations of
  // this slice it acted on (a structural one refuses it: refusal "publication_invalid";
  // a QuoteBox / Support one withholds its row), and the Identification findings of the
  // final published slice (labels: counted, never refused - the price stays valid and
  // query_quote_slice is the answer).
  std::size_t publication_checks = 0;
  std::size_t publication_rollbacks = 0;
  std::size_t publication_violations = 0;
  std::size_t publication_label_violations = 0;
};

// Graded admission for every expiry of ONE underlier/deliverable at ONE valuation
// instant (the caller owns identity). Pass A runs the profile's own fit,
// unchanged (fit_quote_slice: the point fitter, or the term-prior-free channel
// fit for a ChannelSpline profile); expiries it cannot serve with enough own point
// evidence are refitted on the channel objective under a leave-one-out term
// prior built from the other fitted expiries (two stages, so snapshots without
// any pass-A fit still borrow shape from their neighbours). Calendar
// consistency is enforced on the final American prices: slices are accepted
// greedily in confidence order; a slice that crosses an accepted one is first
// given a bounded-move repair (when calendar_repair_half_spreads > 0), then
// refitted once with a stronger prior (and that refit repaired), and otherwise
// refused. No certification is relaxed. With config.repair.mode == Channel the
// channel repair runs only where a certificate or calendar check has already
// failed, so every fit that certifies without it is unchanged: pass A runs
// without it; channel refits (B1, B2 and the calendar retry) run with it after
// their bounded-move repair; an expiry still refused for a failed certificate
// whose pass-A fit failed its certificate too gets that pass-A fit again with
// the repair (grade B, `repair_rung`); and a crossing that neither the
// bounded-move repair nor the retry fixes is offered
// repair_quote_slice_calendar_in_channel on the expiry's own prices, then on
// the retry's. A newly served expiry still feeds the B2 prior and the greedy
// calendar order, so it can change other expiries' channel refits and displace
// a later candidate (the benchmark reports this on the intersection rows).
// The channel-dominance gate (QuoteSliceChannelConfig::point_max_outside_fraction)
// runs last and only substitutes calendar-consistent refits (then re-admits
// calendar refusals that the substitution unblocked), so it changes no prior,
// no acceptance order and no other served expiry. Distinct calendar maturities are
// required; at most kQuoteSurfaceMaxExpiries expiries; carry must be supplied
// (resolved carry is not yet composed with the term prior). Shared malformed
// configuration (including config.channel and config.repair) is Err;
// per-expiry input errors are
// refusals with refusal == "invalid_input".
//
// Knot-first ladder (surface.ladder == KnotFirst; a point profile, without the
// dominance gate, else Err). Pass A computes both pass-A rungs of every expiry:
// the term-prior-free channel fit (the knot rung: the profile's channel config,
// the native knot spline unless channel.family is set, with its bounded-move
// convexity repair) and the point fit. The knot slice serves when it certifies,
// is inside the market and has the A rule's own point evidence; otherwise the
// point fit serves when it certifies and is inside the market (ladder_fallback
// names why the knot slice did not). Either is grade A under the unchanged A
// rule (B once a repair moved its served prices). Expiries neither rung serves take the channel passes as
// without the ladder; the repair rung tries the knot fit with the channel
// repair, then the point fit. MARKET CONTAINMENT: no rung serves a slice whose
// prices lie outside [bid, ask] on more than its limit's share of its strictly
// two-sided quotes (0 < bid < ask) and on at least ladder_min_outside_quotes of
// them; the limit is ladder_max_outside_fraction for a point fit and
// ladder_channel_max_outside_fraction for a channel fit (knot slice or refit). A
// pass-A rung or repair rung outside is skipped, a channel refit outside is
// refused ("outside_market"), and every calendar repair or retry must end
// inside. CALENDAR (confidence-ordered, single pass): slices are visited once
// in a total order on intrinsic slice properties (first the slices inside the
// market under the smaller of the two limits, so a slice served only under an
// explicitly relaxed limit never out-ranks one inside the market; then grade
// A, B, C, where a pass-A slice keeps its A rank until the final relabel; then
// served prices that are the fit's own or moved only by the bounded-move
// convexity repair before prices certified by the channel repair or a repair
// rung; then more strictly two-sided quotes; then the nearer maturity) and
// each one is resolved against
// the slices already accepted before the next is visited, so a lower-confidence
// slice can never constrain a higher-confidence one. A slice that crosses gets,
// in order: its own bounded-move calendar repair; the point fit (knot-served
// slices with an in-market point fit only), clean then repaired; the
// stronger-prior retry under the accepted slices' prior, clean then repaired;
// the channel calendar repair of its own prices, then of the retry's
// (config.repair.mode == Channel); else it is refused. The dominance gate does
// not run (the containment rule replaces it).
inline constexpr std::size_t kQuoteSurfaceMaxExpiries = 128;

[[nodiscard]] Result<std::vector<QuoteSurfaceSlice>>
fit_quote_surface(std::span<const QuoteSurfaceExpiry> expiries, const QuoteSliceConfig &config,
                  const QuoteSurfaceConfig &surface = {});

} // namespace atx::vol
