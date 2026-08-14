// Underlier profile spine — see profile.hpp for the contract and PORT NOTES.
//
// Holds the compiled-in default profiles (spy-like, ordinary, illiquid, plus
// the Sprint-25 mega-cap-event and liquid-single-name clones), the heuristic
// classifier, the tier-priority map, and the OPRA tick-size lookup. Defaults
// follow the research note's numbers, ported value-for-value from
// ats_vol_profile.c.

#include "atx/vol/profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace atx::vol {

using atx::core::Err;

namespace {

// ── Calibration defaults helper (ports `make_calib_defaults`) ────────────
//
// Starts from a default-constructed `CalibOpts` (== `calib_default_opts()`) and
// overrides the six knobs the C helper takes. Per-level iteration caps are set
// by each profile builder below.
[[nodiscard]] CalibOpts make_calib_defaults(std::uint16_t outer, std::uint16_t inner,
                                            double huber_k, double prior_warm,
                                            double max_spread_vol,
                                            double min_vega_weight) noexcept {
  CalibOpts c{};
  c.max_outer_iter = outer;
  c.max_inner_iter = inner;
  c.huber_k = huber_k;
  c.prior_strength = prior_warm;
  c.max_spread_vol = max_spread_vol;
  c.min_vega_weight = min_vega_weight;
  return c;
}

// ── Built-in profile builders ────────────────────────────────────────────

// SPY-like — INDEX_ETF_ULTRA_LIQUID.
[[nodiscard]] Profile build_spy_like() {
  Profile p{};
  p.kind = ProfileKind::IndexEtfUltraLiquid;
  p.optimization_level = OptimizationLevel::Trading;
  // Production stays on eSSVI as the primary parametrization (Sprint 13c).
  p.base_surface = Parametrization::Essvi;
  p.pricing_route = PricingRoute::B76AlCache;

  p.filter.stale_seconds = 5;
  p.filter.now_ts_ns = 0;
  p.filter.wide_spread_pct = 0.50;
  p.filter.wide_min_mid = 0.05;
  p.filter.penny_floor = 0.05;
  p.filter.min_vega_filter = 1.0e-8;

  p.calib = make_calib_defaults(/*outer*/ 50, /*inner*/ 12, /*huber_k*/ 2.0,
                                /*prior_warm*/ 0.10, /*max_spread_vol*/ 0.05,
                                /*min_vega_weight*/ 1.0e-8);
  p.calib.max_iter_quick_mark = 12;
  p.calib.max_iter_trading = 50;
  p.calib.max_iter_risk = 150;
  p.calib.max_iter_reference = 400;
  p.calib.optimization_level = OptimizationLevel::Trading;
  p.calib.essvi_asymmetric_rho = false;                     // Sprint 28 K.2: deferred on SPY
  p.calib.residual_disable = false;                         // Sprint 11: deep-wing residual on
  // The executable residual has always been HingeQuad; name it truthfully.
  p.calib.residual_basis_kind = ResidualBasisKind::HingeQuad;
  p.calib.residual_n_basis_terms = 5;
  p.calib.loss_kind = CalibLossKind::Mid; // Sprint 25: INTERVAL reverted
  // PORT NOTE: the C also set tenor_buckets (5), residual_candidate_select,
  // fengler_n_basis/ridge/max_proj_iters, selector_loss_aware/safety_pp_weighted
  // and fallback_use_quality_score — all omitted from the ported CalibOpts.

  p.price_noise_ticks = 1.0;
  p.spread_vol_fraction = 0.50;
  p.max_residual_ticks = 1.0;
  p.marginal_improvement_ticks = 0.25;

  p.forward_atm_band = 0.03;
  p.ewma_alpha = 0.05;
  p.low_T_years = kFwdLowTDefaultYears;
  p.full_refit_ms = 250u;
  p.local_refit_us = 50u;
  p.subtick_zeroing_ticks = 0.0;
  return p;
}

// Ordinary single-name — the v1 default.
[[nodiscard]] Profile build_ordinary() {
  Profile p{};
  p.kind = ProfileKind::OrdinarySingleName;
  p.optimization_level = OptimizationLevel::Trading;
  p.base_surface = Parametrization::Essvi;
  p.pricing_route = PricingRoute::B76AlCache;

  p.filter.stale_seconds = 30;
  p.filter.now_ts_ns = 0;
  p.filter.wide_spread_pct = 1.50;
  p.filter.wide_min_mid = 0.05;
  p.filter.penny_floor = 0.05;
  p.filter.min_vega_filter = 1.0e-5;

  p.calib = make_calib_defaults(/*outer*/ 35, /*inner*/ 12, /*huber_k*/ 1.5,
                                /*prior_warm*/ 0.35, /*max_spread_vol*/ 0.12,
                                /*min_vega_weight*/ 1.0e-6);
  p.calib.max_iter_quick_mark = 8;
  p.calib.max_iter_trading = 35;
  p.calib.max_iter_risk = 100;
  p.calib.max_iter_reference = 250;
  p.calib.optimization_level = OptimizationLevel::Trading;

  p.price_noise_ticks = 1.5;
  p.spread_vol_fraction = 0.75;
  p.max_residual_ticks = 2.0;
  p.marginal_improvement_ticks = 0.50;

  p.forward_atm_band = 0.05;
  p.ewma_alpha = 0.05;
  p.low_T_years = kFwdLowTDefaultYears;
  p.full_refit_ms = 1000u;
  p.local_refit_us = 100u;
  p.subtick_zeroing_ticks = 0.0;
  return p;
}

// Illiquid small-cap.
[[nodiscard]] Profile build_illiquid() {
  Profile p{};
  p.kind = ProfileKind::IlliquidSmallCap;
  p.optimization_level = OptimizationLevel::QuickMark;
  p.base_surface = Parametrization::Svi;
  p.pricing_route = PricingRoute::B76AlCache;

  p.filter.stale_seconds = 60;
  p.filter.now_ts_ns = 0;
  p.filter.wide_spread_pct = 3.00; // wide markets common
  p.filter.wide_min_mid = 0.10;
  p.filter.penny_floor = 0.05;
  p.filter.min_vega_filter = 1.0e-4;

  p.calib = make_calib_defaults(/*outer*/ 20, /*inner*/ 8, /*huber_k*/ 1.0,
                                /*prior_warm*/ 0.70, /*max_spread_vol*/ 0.25,
                                /*min_vega_weight*/ 1.0e-5);
  p.calib.max_iter_quick_mark = 6;
  p.calib.max_iter_trading = 20;
  p.calib.max_iter_risk = 60;
  p.calib.max_iter_reference = 150;
  p.calib.optimization_level = OptimizationLevel::QuickMark;
  p.calib.max_spread_to_mid_pct = 3.00;

  p.price_noise_ticks = 2.0;
  p.spread_vol_fraction = 1.0;
  p.max_residual_ticks = 3.0;
  p.marginal_improvement_ticks = 1.0;

  p.forward_atm_band = 0.08;
  p.ewma_alpha = 0.10; // faster decay
  p.low_T_years = kFwdLowTDefaultYears;
  p.full_refit_ms = 5000u;
  p.local_refit_us = 200u;
  p.subtick_zeroing_ticks = 0.5; // zero sub-half-tick correction
  return p;
}

// MEGA_CAP_EVENT (Sprint 25) — clone SPY, loosen the single load-bearing
// prefit spread cap, relax the obs filter, and drop the SPY-tuned residual.
[[nodiscard]] Profile build_mega_cap_event(const Profile &spy) {
  Profile p = spy;
  p.kind = ProfileKind::MegaCapEvent;
  p.calib.essvi_asymmetric_rho = false; // Sprint 28: pin off for single names
  p.calib.residual_basis_kind = ResidualBasisKind::HingeQuad;
  p.calib.residual_n_basis_terms = 5;
  p.filter.wide_spread_pct = 1.20; // AAPL near-month spreads run wide
  p.calib.max_spread_vol = 0.20;   // Sprint 26: let the LM see event weeklies
  p.calib.min_vega_weight = 1.0e-7;
  p.calib.residual_disable = true; // SPY wing-bspline over-fits event wings
  // PORT NOTE: the C also set residual_candidate_select=0, use_source_vol_seed,
  // fallback_local_anchored, fengler_*=0, selector_*=0 and a 5-bucket
  // tenor_buckets table — all fields the ported CalibOpts omits.
  return p;
}

// LIQUID_SINGLE_NAME (Sprint 25) — clone SPY, loosen only the prefit spread cap;
// keep the residual layer enabled (smiles are smoother than mega-cap event).
[[nodiscard]] Profile build_liquid_single_name(const Profile &spy) {
  Profile p = spy;
  p.kind = ProfileKind::LiquidSingleName;
  p.filter.wide_spread_pct = 1.00;
  p.calib.essvi_asymmetric_rho = false;
  p.calib.residual_basis_kind = ResidualBasisKind::HingeQuad;
  // residual_disable stays false (residual layer on).
  // PORT NOTE: the C also zeroed tenor_buckets.n_buckets, fengler_* and
  // selector_* — fields the ported CalibOpts omits.
  return p;
}

// Volatility ETPs have broad, noisy wings and fewer PCP-consistent pairs than
// equity/ETF boards. Give them a dedicated SVI-oriented profile instead of the
// old SPY alias, whose tight spread-vol filter could reject every expiry.
[[nodiscard]] Profile build_vol_product(const Profile &ordinary) {
  Profile p = ordinary;
  p.kind = ProfileKind::VolProduct;
  p.base_surface = Parametrization::Svi;
  p.filter.wide_spread_pct = 3.00;
  p.calib.max_outer_iter = 20;
  p.calib.max_inner_iter = 8;
  p.calib.max_spread_vol = 0.35;
  p.calib.min_vega_weight = 1.0e-7;
  p.calib.max_spread_to_mid_pct = 2.00;
  p.calib.residual_disable = true;
  p.full_refit_ms = 1000u;
  return p;
}

// Immutable registry of the six concrete profiles, built once and pointed into
// for the life of the process.
struct ProfileTable {
  Profile spy_like;
  Profile ordinary;
  Profile illiquid;
  Profile mega_cap_event;
  Profile liquid_single_name;
  Profile vol_product;
};

[[nodiscard]] const ProfileTable &profiles() {
  static const ProfileTable table = [] {
    ProfileTable t{};
    t.spy_like = build_spy_like();
    t.ordinary = build_ordinary();
    t.illiquid = build_illiquid();
    t.mega_cap_event = build_mega_cap_event(t.spy_like);
    t.liquid_single_name = build_liquid_single_name(t.spy_like);
    t.vol_product = build_vol_product(t.ordinary);
    return t;
  }();
  return table;
}

// ── Ticker -> kind seed table (Sprint 24) ────────────────────────────────
//
// Exhaustive over the SR fixture symbol set; small enough to linear-scan.
struct TickerSeed {
  std::string_view ticker;
  ProfileKind kind;
};

constexpr std::array<TickerSeed, 21> kTickerSeeds = {{
    // INDEX_ETF_ULTRA_LIQUID — broad ETFs.
    {"SPY", ProfileKind::IndexEtfUltraLiquid},
    {"QQQ", ProfileKind::IndexEtfUltraLiquid},
    {"IWM", ProfileKind::IndexEtfUltraLiquid},
    {"DIA", ProfileKind::IndexEtfUltraLiquid},
    {"EEM", ProfileKind::IndexEtfUltraLiquid},
    {"EFA", ProfileKind::IndexEtfUltraLiquid},
    {"TLT", ProfileKind::IndexEtfUltraLiquid},
    // MEGA_CAP_EVENT — earnings-driven mega caps.
    {"AAPL", ProfileKind::MegaCapEvent},
    {"AMZN", ProfileKind::MegaCapEvent},
    {"NVDA", ProfileKind::MegaCapEvent},
    {"TSLA", ProfileKind::MegaCapEvent},
    {"META", ProfileKind::MegaCapEvent},
    {"GOOGL", ProfileKind::MegaCapEvent},
    {"GOOG", ProfileKind::MegaCapEvent},
    {"MSFT", ProfileKind::MegaCapEvent},
    // LIQUID_SINGLE_NAME — top liquid single names without earnings tilt.
    {"JPM", ProfileKind::LiquidSingleName},
    {"XOM", ProfileKind::LiquidSingleName},
    {"AMD", ProfileKind::LiquidSingleName},
    {"BAC", ProfileKind::LiquidSingleName},
    {"WMT", ProfileKind::LiquidSingleName},
    // HTB_DIVIDEND_NAME placeholder — MRNA routes to LIQUID (matches the C).
    {"MRNA", ProfileKind::LiquidSingleName},
}};

[[nodiscard]] bool ticker_seed_lookup(std::string_view ticker, ProfileKind &out_kind) noexcept {
  if (ticker.empty()) {
    return false;
  }
  for (const TickerSeed &seed : kTickerSeeds) {
    if (seed.ticker == ticker) {
      out_kind = seed.kind;
      return true;
    }
  }
  return false;
}

} // namespace

// ── Registry accessors ───────────────────────────────────────────────────

const Profile &profile_default() noexcept { return profiles().ordinary; }

Result<const Profile *> profile_lookup(ProfileKind kind) {
  const ProfileTable &t = profiles();
  switch (kind) {
  case ProfileKind::IndexEtfUltraLiquid:
    return &t.spy_like;
  case ProfileKind::MegaCapEvent:
    return &t.mega_cap_event;
  case ProfileKind::LiquidSingleName:
    return &t.liquid_single_name;
  // VOL_PRODUCT has no fixture yet — route to the spy-like default (matches
  // the C).
  case ProfileKind::VolProduct:
    return &t.vol_product;
  // ORDINARY / HTB_DIVIDEND both route to the ordinary default.
  case ProfileKind::OrdinarySingleName:
    return &t.ordinary;
  case ProfileKind::HtbDividendName:
    return &t.ordinary;
  case ProfileKind::IlliquidSmallCap:
    return &t.illiquid;
  }
  // Reached only for an out-of-range integer cast to ProfileKind (the C's
  // NULL-on-invalid path); a valid enumerator is handled above.
  return Err(ErrorCode::NotFound, "profile_lookup: unknown ProfileKind");
}

Profile profile_make_cold_fast(const Profile &base) noexcept {
  Profile p = base;
  p.optimization_level = OptimizationLevel::ColdFast;
  p.calib.optimization_level = OptimizationLevel::ColdFast;
  // Outer IRLS cap 50 -> 10; keep the trading inner cap of 12.
  if (p.calib.max_iter_cold_fast == 0) {
    p.calib.max_iter_cold_fast = 10;
  }
  p.calib.max_inner_iter = 12;
  p.calib.residual_disable = true;
  // Mathematical guardrails kept on — cheap no-ops in the well-conditioned
  // regime.
  p.calib.morozov_stop = true;
  p.calib.lee_bound_project = true;
  // PORT NOTE: the C also cleared residual_candidate_select and
  // fallback_use_quality_score — fields the ported CalibOpts omits.
  return p;
}

std::uint8_t profile_tier_priority(ProfileKind kind) noexcept {
  switch (kind) {
  case ProfileKind::IndexEtfUltraLiquid:
    return 0u;
  case ProfileKind::VolProduct:
    return 0u;
  case ProfileKind::MegaCapEvent:
    return 1u;
  case ProfileKind::LiquidSingleName:
    return 1u;
  case ProfileKind::HtbDividendName:
    return 2u;
  case ProfileKind::OrdinarySingleName:
    return 2u;
  case ProfileKind::IlliquidSmallCap:
    return 3u;
  }
  return 3u; // C default for an out-of-range kind
}

// ── OPRA tick-size lookup ─────────────────────────────────────────────────

double tick_size(double price, bool is_penny_pilot) noexcept {
  if (!std::isfinite(price) || price < 0.0) {
    return 0.05; // fall-through
  }
  if (is_penny_pilot) {
    return 0.01; // 1c across all prices
  }
  // Penny Interval Program / standard lattice: 1c below $3, 5c at/above $3.
  return (price < 3.0) ? 0.01 : 0.05;
}

// ── Heuristic classifier ──────────────────────────────────────────────────

ProfileVerdict classify_profile(const ClassifierInputs &in) noexcept {
  // Vol products: explicit operator hint short-circuits.
  if (in.vol_product) {
    return {ProfileKind::VolProduct, 0.95};
  }
  // HTB dominates the dividend-name routing.
  if (in.htb_flag) {
    return {ProfileKind::HtbDividendName, 0.90};
  }

  // The remaining kinds are voted on by liquidity / event heuristics.
  int votes_index_etf = 0;
  int votes_mega_cap = 0;
  int votes_liquid = 0;
  int votes_ordinary = 0;
  int votes_illiquid = 0;

  // The five voted kinds are not unordered categories: they are RUNGS of one
  // liquidity ladder, IndexEtfUltraLiquid (0) down to IlliquidSmallCap (4).
  // Every axis below therefore votes for a position on that ladder, and the
  // span of the positions voted is what the reported confidence measures --
  // see the return statement.
  int min_rung = static_cast<int>(kLiquidityLadderRungs);
  int max_rung = -1;
  const auto rung = [&min_rung, &max_rung](ProfileKind voted) noexcept {
    const int r = static_cast<int>(voted);
    min_rung = std::min(min_rung, r);
    max_rung = std::max(max_rung, r);
  };

  // Quote-density tiers, PER EXPIRY (research §2.1, Table 2, renormalised).
  //
  // The ported tiers were absolute leg counts (4000/1500/500/100), which
  // conflate "well quoted" with "lists many maturities": measured on OPRA the
  // median expiry count runs 24 (mega/ETF), 16 (large), 12 (mid), 9 (small), so
  // an absolute count reads a mid cap with a long ladder as denser than a mega
  // cap with a short one. 3,000 legs over 30 expiries and 3,000 over 6 are not
  // the same board and the absolute rule cannot tell them apart.
  //
  // The boundaries sit at the geometric midpoint between the measured median
  // two-sided-legs-per-expiry of the liquidity tiers each one separates
  // (1,544 board-sessions: S&P 100 4 sessions plus lqbench at both snapshot
  // minutes) -- mega/ETF 157, large 77, mid 35, small 24, so 110 and 50 -- with
  // the top and bottom boundaries quantile-matched to the absolute rule's own
  // index-ETF and illiquid shares (4.7% and 11.0%), which no tier median
  // anchors. Boundaries derived instead by quantile-matching all four shares
  // give 199/94/40/15; the 40 was measured to be the wrong call, because it
  // moves boards whose median relative spread is 45% into LiquidSingleName,
  // whose SPY-derived `max_spread_vol = 0.05` then filters most of the board
  // away. The tier-midpoint boundaries keep every tier's median board in the
  // bucket the absolute rule gives it.
  const std::uint32_t quoted_expiries = in.n_quoted_expiries > 0u ? in.n_quoted_expiries
                                        : in.n_live_expiries > 0u ? in.n_live_expiries
                                                                  : 1u;
  const double quotes_per_expiry =
      static_cast<double>(in.n_live_quotes) / static_cast<double>(quoted_expiries);
  if (quotes_per_expiry >= 200.0) {
    ++votes_index_etf;
    rung(ProfileKind::IndexEtfUltraLiquid);
  } else if (quotes_per_expiry >= 110.0) {
    ++votes_mega_cap;
    rung(ProfileKind::MegaCapEvent);
  } else if (quotes_per_expiry >= 50.0) {
    ++votes_liquid;
    rung(ProfileKind::LiquidSingleName);
  } else if (quotes_per_expiry >= 15.0) {
    ++votes_ordinary;
    rung(ProfileKind::OrdinarySingleName);
  } else {
    ++votes_illiquid;
    rung(ProfileKind::IlliquidSmallCap);
  }

  // Median spread tier — tighter spreads => more liquid. Negated so a dead
  // board's sentinel (and any NaN) reads as maximally wide, not as tight.
  const bool wide_book = !(in.median_spread_pct < 0.40);
  if (in.median_spread_pct < 0.02) {
    ++votes_index_etf;
    rung(ProfileKind::IndexEtfUltraLiquid);
  } else if (in.median_spread_pct < 0.05) {
    ++votes_mega_cap;
    rung(ProfileKind::MegaCapEvent);
  } else if (in.median_spread_pct < 0.15) {
    ++votes_liquid;
    rung(ProfileKind::LiquidSingleName);
  } else if (in.median_spread_pct < 0.40) {
    ++votes_ordinary;
    rung(ProfileKind::OrdinarySingleName);
  } else {
    ++votes_illiquid;
    rung(ProfileKind::IlliquidSmallCap);
  }

  // Listing cadence => index ETF or mega-cap event.
  //
  // A DAILY expiry cycle is an index-complex property, and it is asserted here
  // by counting expiries in a fixed forward window rather than by asking how far
  // away the front one happens to be today -- the latter answers "what weekday
  // is it", not "what does this underlier list". See kFrontExpiryWindowYears.
  if (in.n_front_expiries >= kMinDailyCycleExpiries) {
    ++votes_index_etf;
    rung(ProfileKind::IndexEtfUltraLiquid);
  } else if (in.has_weeklies && in.n_live_expiries >= 20u) {
    ++votes_mega_cap;
    rung(ProfileKind::MegaCapEvent);
  }

  // Imminent earnings => mega-cap-event regardless of size (2-vote bonus).
  if (in.event_distance_days > 0 && in.event_distance_days <= 7) {
    votes_mega_cap += 2;
    rung(ProfileKind::MegaCapEvent);
  }

  // Forward dispersion: high p90 PCP-RMSE => data-quality issue => illiquid.
  if (in.forward_dispersion_bp > 100.0) {
    ++votes_illiquid;
    rung(ProfileKind::IlliquidSmallCap);
  }

  // Pick the winning bucket. The comparison order preserves the C's tie
  // precedence (ordinary < index_etf < mega_cap < liquid < illiquid).
  int best_votes = votes_ordinary;
  ProfileKind kind = ProfileKind::OrdinarySingleName;
  const auto challenge = [&best_votes, &kind](int votes, ProfileKind candidate) noexcept {
    if (votes > best_votes) {
      best_votes = votes;
      kind = candidate;
    }
  };
  challenge(votes_index_etf, ProfileKind::IndexEtfUltraLiquid);
  challenge(votes_mega_cap, ProfileKind::MegaCapEvent);
  challenge(votes_liquid, ProfileKind::LiquidSingleName);
  challenge(votes_illiquid, ProfileKind::IlliquidSmallCap);

  // A wide book vetoes the liquid buckets.
  //
  // The spread axis is the only one that measures execution quality directly,
  // and when it says "illiquid" that vote is counted LAST above with a strict
  // `>`, so it loses every tie to a more liquid bucket. Measured consequence:
  // UVXY (41% median spread) classified LiquidSingleName and BKNG (40%, and it
  // lists 0DTE) classified IndexEtfUltraLiquid -- both then had their entire
  // board filtered away by a profile whose quote filter assumes a penny market
  // (`max_spread_vol = 0.05`), and both returned "no expiry produced a usable
  // slice". A board quoting 40% wide is at best ORDINARY whatever else it looks
  // like. The vote still chooses freely between ordinary and illiquid, and a
  // vetoed board's confidence stays low on its own, without special-casing:
  // the spread axis has voted the bottom rung while a liquid density vote sits
  // near the top, so the ladder span below is already wide.
  if (wide_book && kind != ProfileKind::IlliquidSmallCap) {
    kind = ProfileKind::OrdinarySingleName;
  }

  // Confidence = how tightly the axes agree on WHERE the board sits, measured
  // as the span they cover on the liquidity ladder.
  //
  // This was `best_votes / max_votes`, the modal bucket's vote share, and that
  // is the wrong statistic for two reasons.
  //
  // It is not ordinal. The five kinds are a ladder, so an axis that lands one
  // rung away from the winner is near-agreement, while one that lands four
  // rungs away contradicts it outright -- a vote share scores both as simply
  // "not the mode". With two or three axes voting, "the axes landed on the SAME
  // rung" is a demanding test that a healthy mid-liquidity board fails about
  // half the time: measured over 767 board-voted OPRA sessions, 37% of boards
  // had their two liquidity axes one rung apart, which is sampling behaviour,
  // not ambiguity.
  //
  // And it quantizes onto a handful of rationals that straddle the caller's
  // gate. Over the same corpus the share took only the values 0, 1/3, 1/2, 2/3
  // and 1, and `FitPolicyConfig::min_direct_confidence` defaults to 0.70 -- so
  // the routing decision turned on whether a board landed on 2/3 or on 1,
  // a ONE-VOTE difference, for 20% of the universe. The span measure below
  // takes the values 1, 0.75, 0.5, 0.25, 0 and leaves the same 0.70 gate in the
  // middle of a 0.25-wide empty band: crossing it now requires two axes to
  // disagree about the board's rung by two rungs rather than by one.
  //
  // Measured effect on cross-session reproducibility of the cross-validation
  // decision itself, same corpus, adjacent sessions: 28.6% / 13.5% of symbols
  // changed routes at 10:30 / 15:55 under the vote share, 2.3% / 2.3% under the
  // span.
  const double confidence = (max_rung >= min_rung)
                                ? 1.0 - static_cast<double>(max_rung - min_rung) /
                                            static_cast<double>(kLiquidityLadderRungs - 1u)
                                : 0.5;
  return {kind, confidence};
}

namespace {

// One-pass, allocation-free EXACT-BUCKET median of the board's relative spreads.
//
// History, because both predecessors failed for the same underlying reason. The
// first estimator kept the first `kCap` two-sided legs; chains arrive sorted
// ascending in T, so that measured the FRONT of a large board, not the board --
// over 1,544 OPRA board-sessions it ran 20-35% WIDE of the true median at every
// liquidity tier (mega 0.067 vs 0.050, small 0.312 vs 0.279), because a front
// expiry is mostly cheap far-OTM legs whose relative spread is huge. Its
// replacement kept every `stride`-th leg and doubled `stride` on fill, which
// spread the sample uniformly over the stream and removed that bias.
//
// It remained a SAMPLE, though: ~256 of a board's legs, so its answer was a
// function of the ORDER the legs arrived in. That is not a hypothetical. On
// lqbench 2026-08-03, admitting one-sided rows as bounds appends strikes to a
// chain's axis and pushes every later strike along, permuting the leg stream
// while changing no two-sided quote at all -- the two-sided multiset and
// `n_live_quotes` are identical on all 225 boards. That permutation alone moved
// the estimate on 104 of the 225 and moved two of them (IREN, PLTR) across a
// classifier bucket edge. Against the exact median the sample misplaced 12 of
// 225 boards before that change and 10 after; IREN's true median is 0.153846 and
// the sample read it as 0.149533, one side of the 0.15 edge each.
//
// A histogram removes both faults at once, and it is available here only because
// the quantity has a BOUNDED domain: for a two-sided leg 0 < bid < ask, so
// (ask - bid)/mid = 2(ask - bid)/(ask + bid) lies strictly in (0, 2). A fixed bin
// count therefore covers the whole range, which is what turns an estimate into a
// decision procedure. Bin width 1/1000 makes every bucket edge the classifier
// tests (0.02, 0.05, 0.15, 0.40) an exact bin boundary, so reporting a bin's
// LOWER edge preserves each comparison exactly: for a threshold t on that grid,
// floor(1000m)/1000 < t if and only if m < t. The reported value is thus the true
// median rounded down to 0.001 and it votes the bucket the true median votes --
// on all 225 boards, against 10 the sample misplaced.
//
// Still allocation-free (`classifier_inputs_from_underlier` is `noexcept` and
// runs per board on the fit path, so a throwing allocation would std::terminate)
// and still single-pass; the counter array is 8 KiB of stack.
class HistogramMedian {
public:
  void push(double v) noexcept {
    // SAFETY: the domain is (0, 2) for every leg the caller admits (it pushes
    // only when bid > 0 && ask > bid), but clamp rather than trust it -- an
    // out-of-domain value must degrade the statistic, never index out of bounds.
    const double scaled = v * kBinsPerUnit;
    std::size_t bin = 0u;
    if (scaled >= static_cast<double>(kBins - 1u)) {
      bin = kBins - 1u;
    } else if (scaled > 0.0) {
      bin = static_cast<std::size_t>(scaled);
    }
    ++counts_[bin];
    ++n_;
  }

  // Sentinel when the stream was empty: `max()` so a board with nothing to
  // measure reads as maximally WIDE. 0.0 would be the tightest possible value
  // and would vote the most liquid bucket.
  [[nodiscard]] double median() const noexcept {
    if (n_ == 0u) {
      return std::numeric_limits<double>::max();
    }
    // Same rank convention as the sample estimators this replaces: the element
    // at 0-based rank n/2 (the upper median when n is even).
    const std::uint32_t target = n_ / 2u;
    std::uint32_t cum = 0u;
    for (std::size_t bin = 0; bin < kBins; ++bin) {
      cum += counts_[bin];
      if (cum > target) {
        return static_cast<double>(bin) / kBinsPerUnit;
      }
    }
    return static_cast<double>(kBins - 1u) / kBinsPerUnit;
  }

private:
  static constexpr double kBinsPerUnit = 1000.0;
  static constexpr std::size_t kBins = 2000u; // covers [0, 2), the whole domain
  std::array<std::uint32_t, kBins> counts_{};
  std::uint32_t n_{0u};
};

} // namespace

ClassifierInputs classifier_inputs_from_underlier(const Underlying &under) noexcept {
  // Aggregate quote-state across chains.
  std::uint32_t n_live = 0u;
  std::uint32_t n_atm = 0u;
  std::uint32_t n_quoted_expiries = 0u;
  std::uint32_t n_identifiable_expiries = 0u;
  std::uint32_t max_near_money_strikes = 0u;
  std::uint32_t n_front_expiries = 0u;
  bool has_weeklies = false;

  HistogramMedian board_spreads;

  const double S = (under.spot > 0.0) ? under.spot : 0.0;
  // Near-money band as a price interval, so the per-strike test stays two
  // comparisons: |ln(K/S)| <= b  <=>  S*exp(-b) <= K <= S*exp(b).
  const double near_money_lo = S * std::exp(-kNearMoneyLogMoneyness);
  const double near_money_hi = S * std::exp(kNearMoneyLogMoneyness);
  for (const Chain &c : under.chains) {
    if (c.T <= 0.0) {
      continue;
    }
    if (c.T <= kFrontExpiryWindowYears) {
      ++n_front_expiries;
    }
    if (c.T <= 0.10) {
      has_weeklies = true;
    }

    const std::size_t ns = c.n_strikes();
    const std::size_t n_side = ns * 2u;
    // SAFETY: the SoA invariant guarantees each per-side array is exactly
    // 2*n_strikes long; guard a malformed chain so operator[] below stays in
    // bounds (the C trusted the invariant unconditionally).
    if (c.bids.size() < n_side || c.asks.size() < n_side) {
      continue;
    }

    bool expiry_quoted = false;
    std::uint32_t near_money_strikes = 0u;
    for (std::size_t s = 0; s < ns; ++s) {
      const double K = c.strikes[s];
      const bool near_money = S > 0.0 && K >= near_money_lo && K <= near_money_hi;
      bool strike_quoted = false;
      for (int side_i = 0; side_i < 2; ++side_i) {
        const Side side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
        const std::size_t ix = chain_index(static_cast<std::uint16_t>(s), side);
        const double bid = c.bids[ix];
        const double ask = c.asks[ix];
        if (!(bid > 0.0 && ask > bid)) {
          continue;
        }
        ++n_live;
        expiry_quoted = true;
        strike_quoted = true;
        if (near_money) {
          ++n_atm;
        }
        const double mid = 0.5 * (bid + ask);
        if (mid > 0.0) {
          board_spreads.push((ask - bid) / mid);
        }
      }
      if (near_money && strike_quoted) {
        ++near_money_strikes;
      }
    }
    if (expiry_quoted) {
      ++n_quoted_expiries;
    }
    if (near_money_strikes >= kMinIdentifiableSliceStrikes) {
      ++n_identifiable_expiries;
    }
    max_near_money_strikes = std::max(max_near_money_strikes, near_money_strikes);
  }

  ClassifierInputs in{};
  in.n_live_quotes = n_live;
  in.n_live_expiries = static_cast<std::uint32_t>(under.chains.size());
  in.n_quoted_expiries = n_quoted_expiries;
  in.n_atm_quotes = n_atm;
  in.n_identifiable_expiries = n_identifiable_expiries;
  in.max_near_money_strikes = max_near_money_strikes;
  in.median_spread_pct = board_spreads.median();
  in.n_front_expiries = n_front_expiries;
  in.has_weeklies = has_weeklies;
  in.htb_flag = (under.flags & kUflagHtb) != 0u;
  in.vol_product = false;
  // n_dividends / median_q_eff / event_distance_days / forward_dispersion_bp
  // stay 0 — see the header PORT NOTE (none feed the vote).

  return in;
}

ProfileVerdict classify_underlier(const Underlying &under) noexcept {
  return classify_profile(classifier_inputs_from_underlier(under));
}

std::optional<ProfileKind> ticker_seed_profile(std::string_view ticker) noexcept {
  ProfileKind kind = ProfileKind::OrdinarySingleName;
  if (!ticker_seed_lookup(ticker, kind)) {
    return std::nullopt;
  }
  return kind;
}

ProfileVerdict classify_underlier_with_ticker(const Underlying &under,
                                              std::string_view ticker) noexcept {
  ProfileKind seed_kind = ProfileKind::OrdinarySingleName;
  if (ticker_seed_lookup(ticker, seed_kind)) {
    return {seed_kind, kTickerSeedConfidence};
  }
  return classify_underlier(under);
}

} // namespace atx::vol
