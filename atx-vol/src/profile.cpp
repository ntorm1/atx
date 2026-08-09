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
  int max_votes = 0;

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
    ++max_votes;
  } else if (quotes_per_expiry >= 110.0) {
    ++votes_mega_cap;
    ++max_votes;
  } else if (quotes_per_expiry >= 50.0) {
    ++votes_liquid;
    ++max_votes;
  } else if (quotes_per_expiry >= 15.0) {
    ++votes_ordinary;
    ++max_votes;
  } else {
    ++votes_illiquid;
    ++max_votes;
  }

  // Median spread tier — tighter spreads => more liquid. Negated so a dead
  // board's sentinel (and any NaN) reads as maximally wide, not as tight.
  const bool wide_book = !(in.median_spread_pct < 0.40);
  if (in.median_spread_pct < 0.02) {
    ++votes_index_etf;
    ++max_votes;
  } else if (in.median_spread_pct < 0.05) {
    ++votes_mega_cap;
    ++max_votes;
  } else if (in.median_spread_pct < 0.15) {
    ++votes_liquid;
    ++max_votes;
  } else if (in.median_spread_pct < 0.40) {
    ++votes_ordinary;
    ++max_votes;
  } else {
    ++votes_illiquid;
    ++max_votes;
  }

  // 0DTE / weekly cadence => index ETF or mega-cap event.
  if (in.has_zerodte) {
    ++votes_index_etf;
    ++max_votes;
  } else if (in.has_weeklies && in.n_live_expiries >= 20u) {
    ++votes_mega_cap;
    ++max_votes;
  }

  // Imminent earnings => mega-cap-event regardless of size (2-vote bonus).
  if (in.event_distance_days > 0 && in.event_distance_days <= 7) {
    votes_mega_cap += 2;
    max_votes += 2;
  }

  // Forward dispersion: high p90 PCP-RMSE => data-quality issue => illiquid.
  if (in.forward_dispersion_bp > 100.0) {
    ++votes_illiquid;
    ++max_votes;
  }

  // Pick the winning bucket. The comparison order preserves the C's tie
  // precedence (ordinary < index_etf < mega_cap < liquid < illiquid).
  int best_votes = votes_ordinary;
  ProfileKind kind = ProfileKind::OrdinarySingleName;
  if (votes_index_etf > best_votes) {
    best_votes = votes_index_etf;
    kind = ProfileKind::IndexEtfUltraLiquid;
  }
  if (votes_mega_cap > best_votes) {
    best_votes = votes_mega_cap;
    kind = ProfileKind::MegaCapEvent;
  }
  if (votes_liquid > best_votes) {
    best_votes = votes_liquid;
    kind = ProfileKind::LiquidSingleName;
  }
  if (votes_illiquid > best_votes) {
    best_votes = votes_illiquid;
    kind = ProfileKind::IlliquidSmallCap;
  }

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
  // like. The vote still chooses freely between ordinary and illiquid, and the
  // reported confidence becomes the ordinary bucket's own share, which is the
  // truth: on a vetoed board the heuristics did NOT agree.
  if (wide_book && kind != ProfileKind::IlliquidSmallCap) {
    kind = ProfileKind::OrdinarySingleName;
    best_votes = votes_ordinary;
  }

  const double confidence =
      (max_votes > 0) ? static_cast<double>(best_votes) / static_cast<double>(max_votes) : 0.5;
  return {kind, confidence};
}

ClassifierInputs classifier_inputs_from_underlier(const Underlying &under) noexcept {
  // Aggregate quote-state across chains.
  std::uint32_t n_live = 0u;
  std::uint32_t n_atm = 0u;
  std::uint32_t n_quoted_expiries = 0u;
  std::uint32_t n_identifiable_expiries = 0u;
  std::uint32_t max_near_money_strikes = 0u;
  bool has_zerodte = false;
  bool has_weeklies = false;

  constexpr std::size_t kSpreadBuf = 256u;
  std::array<double, kSpreadBuf> spreads{};
  std::uint32_t n_spreads = 0u;
  // Stride-decimated sample of the relative spreads, in place of the reservoir
  // that simply took the first `kSpreadBuf` two-sided legs. Chains arrive sorted
  // ascending in T, so a prefix reservoir measures the FRONT of a large board,
  // not the board: measured over 1,544 OPRA board-sessions the prefix median ran
  // 20-35% WIDE of the true median at every liquidity tier (mega 0.067 vs 0.050,
  // small 0.312 vs 0.279), because a front expiry is mostly cheap far-OTM legs
  // whose relative spread is huge. Sampling every `stride`-th leg and doubling
  // `stride` whenever the buffer fills keeps the sample uniform over the whole
  // board -- hence over every expiry -- in one pass, with no allocation (this
  // function is `noexcept` and runs per board on the fit path).
  std::uint32_t stride = 1u;
  std::uint32_t n_seen = 0u;

  const double S = (under.spot > 0.0) ? under.spot : 0.0;
  // Near-money band as a price interval, so the per-strike test stays two
  // comparisons: |ln(K/S)| <= b  <=>  S*exp(-b) <= K <= S*exp(b).
  const double near_money_lo = S * std::exp(-kNearMoneyLogMoneyness);
  const double near_money_hi = S * std::exp(kNearMoneyLogMoneyness);
  for (const Chain &c : under.chains) {
    if (c.T <= 0.0) {
      continue;
    }
    if (c.T < 0.01) {
      has_zerodte = true;
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
          if (n_seen % stride == 0u) {
            if (n_spreads == kSpreadBuf) {
              // Halve the sample, keeping every second entry, and double the
              // stride: the buffer again holds legs 0, stride, 2*stride, ...
              for (std::uint32_t i = 0u; 2u * i < n_spreads; ++i) {
                spreads[i] = spreads[2u * i];
              }
              n_spreads = (n_spreads + 1u) / 2u;
              stride *= 2u;
            }
            if (n_seen % stride == 0u) {
              spreads[n_spreads] = (ask - bid) / mid;
              ++n_spreads;
            }
          }
          ++n_seen;
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

  // Cheap median estimate: insertion-sort the small sample. A board with no
  // two-sided quote has no spread to measure; 0.0 would be the TIGHTEST possible
  // value and would vote IndexEtfUltraLiquid, so a dead board must instead read as
  // maximally wide.
  double median_spread = std::numeric_limits<double>::max();
  if (n_spreads > 0u) {
    for (std::uint32_t i = 1u; i < n_spreads; ++i) {
      const double v = spreads[i];
      std::uint32_t j = i;
      while (j > 0u && spreads[j - 1u] > v) {
        spreads[j] = spreads[j - 1u];
        --j;
      }
      spreads[j] = v;
    }
    median_spread = spreads[n_spreads / 2u];
  }

  ClassifierInputs in{};
  in.n_live_quotes = n_live;
  in.n_live_expiries = static_cast<std::uint32_t>(under.chains.size());
  in.n_quoted_expiries = n_quoted_expiries;
  in.n_atm_quotes = n_atm;
  in.n_identifiable_expiries = n_identifiable_expiries;
  in.max_near_money_strikes = max_near_money_strikes;
  in.median_spread_pct = median_spread;
  in.has_zerodte = has_zerodte;
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
