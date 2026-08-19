#pragma once
// ── atx::vol::alpha — the feature and target catalogues ─────────────────────
//
// An insertion-ordered, name-unique registry, plus the BUILT-IN catalogue that
// describes the panel this repo actually emits: `f0_log_rv1 .. f21_front_curv_10d`,
// the two measured liquidity columns, and every target axis the trainer grades.
//
// The catalogue is not a parallel universe. It is the single place those specs
// are written down, and `audit.hpp` is the only consumer that needs them. A
// tenth round adds a feature by appending ONE `FeatureSpec` here -- no struct
// field, no `kVrpPanelColumnsV5`, no `kVrpFeatureCount` bump, and no edits to
// the eighteen trainer call sites that index a fixed-width `std::array`.
//
// SELECTION IS BY NAME, NOT BY POSITION. `select()` takes patterns
// (`f4_term_slope`, `f1?_*`, `liq_*`, `*`) so a run's feature set is a string
// in a config, not a recompiled `kAllFeatures` array. That is what makes
// "which features did this fit use" answerable from the artifact.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/alpha/spec.hpp"

namespace atx::vol::alpha {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

// ── Glob matching: `*` (any run) and `?` (one char). Nothing else ───────────
//
// Iterative with a backtrack anchor rather than recursive: the JPL discipline
// in `.agents/cpp/agent.md` S3 wants a statically obvious bound, and this form
// is O(n*m) worst case with no stack growth on an adversarial pattern.
[[nodiscard]] inline bool glob_match(std::string_view pattern, std::string_view text) noexcept {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;
  std::size_t star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p;
      star_t = t;
      ++p;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      ++star_t;
      t = star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

// ── Registry ────────────────────────────────────────────────────────────────
//
// Insertion order IS the catalogue order, and the catalogue order is what
// `PanelSchema` freezes into a fingerprint.
//
// LIFETIME CONTRACT: `find()` and `select()` hand out pointers INTO the
// registry's own `std::vector`, so an `add()` after the fact invalidates them.
// The index therefore stores a POSITION rather than a pointer, and the
// intended use is build-then-query: populate the registry, then resolve. The
// built-in catalogues are fully populated before they are returned.
template <typename Spec> class Registry {
public:
  Registry() = default;

  // Rejects a duplicate name. Silent shadowing of a spec is how two rounds end
  // up disagreeing about what `f9_vov_63d` means.
  [[nodiscard]] Status add(Spec spec) {
    if (spec.name.empty()) {
      return Err(ErrorCode::InvalidArgument, "alpha::Registry: spec has an empty name");
    }
    if (index_.find(spec.name) != index_.end()) {
      return Err(ErrorCode::AlreadyExists, "alpha::Registry: duplicate spec name '" + spec.name + "'");
    }
    index_.emplace(spec.name, specs_.size());
    specs_.push_back(std::move(spec));
    return Ok();
  }

  [[nodiscard]] const Spec *find(std::string_view name) const noexcept {
    const auto it = index_.find(std::string(name));
    if (it == index_.end()) {
      return nullptr;
    }
    return &specs_[it->second];
  }

  [[nodiscard]] std::span<const Spec> all() const noexcept { return specs_; }
  [[nodiscard]] std::size_t size() const noexcept { return specs_.size(); }
  [[nodiscard]] bool empty() const noexcept { return specs_.empty(); }

  // Resolve patterns to specs in CATALOGUE order (not pattern order), so the
  // same set of patterns always yields the same column order regardless of how
  // the caller spelled or ordered them. A pattern that matches nothing is an
  // error, never a silent empty: a typo'd `--features f4_term_slop` must not
  // quietly train on nine features and report a clean run.
  [[nodiscard]] Result<std::vector<const Spec *>> select(std::span<const std::string> patterns) const {
    if (patterns.empty()) {
      return Err(ErrorCode::InvalidArgument, "alpha::Registry::select: no patterns given");
    }
    std::vector<bool> hit(specs_.size(), false);
    for (const std::string &pattern : patterns) {
      if (pattern.empty()) {
        return Err(ErrorCode::InvalidArgument, "alpha::Registry::select: empty pattern");
      }
      bool matched = false;
      for (std::size_t i = 0; i < specs_.size(); ++i) {
        if (glob_match(pattern, specs_[i].name)) {
          hit[i] = true;
          matched = true;
        }
      }
      if (!matched) {
        return Err(ErrorCode::NotFound, "alpha::Registry::select: pattern '" + pattern +
                                            "' matched no registered spec");
      }
    }
    std::vector<const Spec *> out;
    out.reserve(specs_.size());
    for (std::size_t i = 0; i < specs_.size(); ++i) {
      if (hit[i]) {
        out.push_back(&specs_[i]);
      }
    }
    return Ok(std::move(out));
  }

  [[nodiscard]] std::vector<std::string> names() const {
    std::vector<std::string> out;
    out.reserve(specs_.size());
    for (const Spec &spec : specs_) {
      out.push_back(spec.name);
    }
    return out;
  }

private:
  std::vector<Spec> specs_;
  std::unordered_map<std::string, std::size_t> index_;
};

using FeatureRegistry = Registry<FeatureSpec>;
using TargetRegistry = Registry<TargetSpec>;

// ── Construction helpers, so the catalogue below reads as a table ───────────

[[nodiscard]] inline SeriesRef spot(std::ptrdiff_t first, std::ptrdiff_t last) {
  return SeriesRef{SeriesId::Spot, 0, Window{first, last}};
}
[[nodiscard]] inline SeriesRef market(std::ptrdiff_t first, std::ptrdiff_t last) {
  return SeriesRef{SeriesId::MarketSpot, 0, Window{first, last}};
}
[[nodiscard]] inline SeriesRef strip(std::int32_t tenor, std::ptrdiff_t first, std::ptrdiff_t last) {
  return SeriesRef{SeriesId::IvFairStrip, tenor, Window{first, last}};
}
[[nodiscard]] inline SeriesRef atmf(std::int32_t tenor, std::ptrdiff_t first, std::ptrdiff_t last) {
  return SeriesRef{SeriesId::IvAtmf, tenor, Window{first, last}};
}
[[nodiscard]] inline SeriesRef wing_call(std::int32_t tenor, std::ptrdiff_t first, std::ptrdiff_t last) {
  return SeriesRef{SeriesId::IvWingCall, tenor, Window{first, last}};
}
[[nodiscard]] inline SeriesRef wing_put(std::int32_t tenor, std::ptrdiff_t first, std::ptrdiff_t last) {
  return SeriesRef{SeriesId::IvWingPut, tenor, Window{first, last}};
}

// ── The built-in feature catalogue ──────────────────────────────────────────
//
// Order matches the panel's emitted column order (v1's f0..f9, then v3's two
// liquidity columns, then round 9's f10..f21) so a catalogue-ordered selection
// lines up with a panel row without a permutation step.
//
// EVERY `reads` ENTRY IS A CLAIM ABOUT THE IMPLEMENTATION, and the audit is
// only as sound as those claims. They are transcribed from the window
// definitions in `src/analytics/vrp_panel.hpp` (the f0..f9 banner) and from the
// round-9 lane's v4 banner, which state each window in sessions.
[[nodiscard]] inline FeatureRegistry builtin_features() {
  FeatureRegistry reg;
  const auto put = [&reg](FeatureSpec spec) {
    // A duplicate name in a compile-time-fixed table is a programming error,
    // not a runtime condition; the catalogue is closed, so the only way to
    // trip this is to edit it wrong.
    const Status st = reg.add(std::move(spec));
    (void)st;
  };

  put({"f0_log_rv1", Unit::LogVariance, SignPrior::None, "",
       {spot(-1, 0)},
       "ln(max(252*r_cc(t)^2, 1e-8)) -- one squared close-to-close return, annualized."});
  put({"f1_log_rv5", Unit::LogVariance, SignPrior::None, "",
       {spot(-5, 0)},
       "ln(trailing 5-session annualized c2c variance)."});
  put({"f2_log_rv21", Unit::LogVariance, SignPrior::None, "",
       {spot(-21, 0)},
       "ln(trailing 21-session annualized c2c variance)."});
  put({"f3_iv_level", Unit::LogVariance, SignPrior::None, "",
       {strip(21, 0, 0)},
       "ln(iv_fair_21d^2). THE ENTRY MARK ITSELF: shares a leg with every "
       "target that pays -iv^2, so its score against those axes is a channel, "
       "not a forecast."});
  put({"f4_term_slope", Unit::VolDecimal, SignPrior::BuyHigh,
       "Vasquez, JFQA 52(6) 2017",
       {strip(21, 0, 0), strip(63, 0, 0)},
       "iv_fair_63d - iv_fair_21d. Vasquez defines the slope on the LONGEST "
       "maturity in the 50-360 day band, so this is a TRUNCATED reproduction; "
       "f17..f19 carry the long anchors."});
  put({"f5_hv_iv_gap", Unit::LogRatio, SignPrior::BuyHigh,
       "Goyal & Saretto, JFE 94(2) 2009",
       {spot(-21, 0), strip(21, 0, 0)},
       "ln(rv_trail_21d / iv_fair_21d). Buy vol when realized has been running "
       "above implied. Verbatim Goyal-Saretto."});
  put({"f6_vrp_lag", Unit::Variance, SignPrior::BuyLow,
       "Goyal & Saretto, JFE 94(2) 2009 (difference scale)",
       {strip(21, 0, 0), spot(-21, 0)},
       "iv_fair_21d^2 - trailing 21d annualized c2c variance. The same two legs "
       "as f5 on a difference rather than a log-ratio scale."});
  put({"f7_ret_21d", Unit::LogReturn, SignPrior::None, "",
       {spot(-21, 0)},
       "ln(spot[t]/spot[t-21]). Leverage/skew proxy."});
  put({"f8_jump_recent", Unit::Indicator, SignPrior::None, "",
       {spot(-63, 0)},
       "1 when max|r_cc| over the trailing 5 sessions exceeds 4x the trailing "
       "63-session daily sigma. The cheap earnings PROXY -- there is no "
       "earnings calendar in the panel yet."});
  put({"f9_vov_63d", Unit::VolDecimal, SignPrior::BuyLow,
       "Cao, Vasquez, Xiao & Zhan, QJF 2023 (window/transform differ)",
       {strip(21, -63, 0)},
       "stdev of the 63 daily LEVEL differences of iv_fair_21d. The cited "
       "construct is 21 daily LOG changes of the ATMF mark; f16 carries that "
       "one, so f9-vs-f16 isolates window-and-transform."});

  put({"liq_hspread_frac", Unit::Fraction, SignPrior::BuyLow,
       "Christoffersen, Goyenko, Jacobs & Karoui, RFS 31(2) 2018",
       {SeriesRef{SeriesId::LiqHalfSpread, 0, Window{0, 0}}},
       "Measured ATM quoted half-spread as a fraction of mid. A COST INPUT that "
       "is also a SIGNAL: delta-hedged option returns are significantly more "
       "negative when the option is less liquid, so a long-vega book wants the "
       "tight end. The panel has only ever used it as a cost."});
  put({"liq_strikes_fit", Unit::Dimensionless, SignPrior::None, "",
       {SeriesRef{SeriesId::LiqStrikesFit, 0, Window{0, 0}}},
       "Strikes the session's surface fit consumed. Board-depth proxy; moves "
       "with, but is not, the quoted width."});

  put({"f10_iv_rank_252", Unit::Rank, SignPrior::None, "",
       {atmf(21, -251, 0)},
       "Own-history mid-rank percentile of iv_atmf_21d over 252 trailing "
       "sessions. LOW CONFIDENCE: no peer-reviewed cross-sectional option-return "
       "study uses IV rank; expected collinear with the slope family."});
  put({"f11_rr25_21d", Unit::VolDecimal, SignPrior::None, "",
       {wing_call(21, 0, 0), wing_put(21, 0, 0)},
       "iv(25d CALL) - iv(25d PUT) at 21d. NOTE: the NEGATIVE of analytics.hpp's "
       "put-minus-call `risk_reversal`."});
  put({"f12_bf25_21d", Unit::VolDecimal, SignPrior::None, "",
       {wing_call(21, 0, 0), wing_put(21, 0, 0), atmf(21, 0, 0)},
       "(iv(25dC)+iv(25dP))/2 - iv_atmf_21d. CONTAINS THE ENTRY MARK with a "
       "minus sign; must not be read at feature lag 0 against an IV-change axis."});
  put({"f13_term_curv", Unit::VolDecimal, SignPrior::None, "",
       {strip(10, 0, 0), strip(21, 0, 0), strip(63, 0, 0)},
       "iv_fair_63d - 2*iv_fair_21d + iv_fair_10d. The front kink the 21/63 "
       "slope cannot see."});
  put({"f14_iv_chg_5d", Unit::VolDecimal, SignPrior::None, "",
       {atmf(21, -5, 0)},
       "iv_atmf_21d[t] - iv_atmf_21d[t-5]. Buy names whose implied vol just "
       "fell. Same entry-mark caveat as f12."});
  put({"f15_idio_share", Unit::Fraction, SignPrior::BuyLow,
       "Cao & Han, JFE 108(1) 2013",
       {spot(-63, 0), market(-63, 0)},
       "1 - R^2 of this name's daily log returns on the market proxy's over 63 "
       "trailing pairs. Cao-Han find delta-hedged returns fall monotonically in "
       "idiosyncratic vol, and the IVOL coefficient MORE THAN DOUBLES "
       "(-0.0373 -> -0.0822) once log(HV/IV) -- this panel's own f5 -- is "
       "controlled for: published evidence of incrementality over the incumbent."});
  put({"f16_iv_vov_21d", Unit::Dimensionless, SignPrior::BuyLow,
       "Cao, Vasquez, Xiao & Zhan, QJF 2023",
       {atmf(21, -21, 0)},
       "stdev of the 21 daily LOG changes in iv_atmf_21d. THE CITED "
       "CONSTRUCTION: Fama-MacBeth coefficient -3.003 (t -6.30), surviving joint "
       "controls for Goyal-Saretto, Cao-Han AND Vasquez."});
  put({"f17_slope_126d", Unit::VolDecimal, SignPrior::BuyHigh,
       "Vasquez, JFQA 52(6) 2017",
       {strip(21, 0, 0), strip(126, 0, 0)},
       "iv_fair_126d - iv_fair_21d. Vasquez's slope on a ~6-month long leg."});
  put({"f18_slope_189d", Unit::VolDecimal, SignPrior::BuyHigh,
       "Vasquez, JFQA 52(6) 2017",
       {strip(21, 0, 0), strip(189, 0, 0)},
       "iv_fair_189d - iv_fair_21d. ~9-month long leg."});
  put({"f19_slope_252d", Unit::VolDecimal, SignPrior::BuyHigh,
       "Vasquez, JFQA 52(6) 2017",
       {strip(21, 0, 0), strip(252, 0, 0)},
       "iv_fair_252d - iv_fair_21d. ~12-month long leg. Three anchors ship "
       "because long-pillar availability is a property of each session's fitted "
       "board: the NaN rate IS the coverage curve."});
  put({"f20_iv_vov_63d", Unit::VolDecimal, SignPrior::BuyLow,
       "Cao, Vasquez, Xiao & Zhan, QJF 2023 (window/transform differ)",
       {atmf(21, -63, 0)},
       "stdev of the 63 daily LEVEL differences of iv_atmf_21d. f20-vs-f9 "
       "isolates implied-vs-realized; f20-vs-f16 isolates window-and-transform."});
  put({"f21_front_curv_10d", Unit::Dimensionless, SignPrior::None,
       "Alexiou, Goyal, Kostakis & Rompolis, Review of Finance 29(4) 2025 (proxy)",
       {wing_call(10, 0, 0), wing_put(10, 0, 0), atmf(10, 0, 0)},
       "3-pivot quadratic curvature of the 10d smile at k in {-0.10, 0, +0.10}. "
       "A CONTINUOUS PROXY for AGKR front-smile concavity, NOT their dummy. "
       "Prior is None on purpose: AGKR conclude the earnings premium is priced "
       "in GAMMA, not vega."});

  // ── Round 11: predictors of REALIZED vol, not of implied richness ───────
  //
  // Everything above f22 is, in the end, some statement about whether IV is
  // cheap. The measured consequence on this book was f4_term_slope: best
  // signal on the money axis, ZERO of 21 non-overlapping phases positive
  // against forward realized vol. Campasano & Linn (SSRN 2871616, 2017) give
  // the mechanism in so many words -- "the term structure inverts due largely
  // to an increase in one month implied volatility as opposed to an increase
  // in volatility of the underlying asset" -- so that whole family is an
  // IV-overreaction signal wearing a realized-vol costume. The block below is
  // deliberately the other kind.
  //
  // READ THE PRIORS CAREFULLY. f22/f23/f25 are calibrated against FORWARD
  // REALIZED VOL, because that is what their source regresses. Whether IV
  // already prices them is the MEASUREMENT, not the prior -- a feature can
  // forecast realized vol perfectly and still lose money if the mark already
  // reflects it. f24/f26/f27 come from sources whose dependent variable is a
  // delta-hedged P&L, so their priors are money-axis priors.
  put({"f22_semivar_dn_21d", Unit::LogVariance, SignPrior::BuyHigh,
       "Patton & Sheppard, REStat 97(3) 2015 (daily-return proxy for 5-min RS-)",
       {spot(-21, 0)},
       "ln of the trailing 21-session DOWNSIDE realized variance, sum of r^2 "
       "over negative-return sessions only, annualized. In the cited panel of "
       "105 individual names at h=22 -- this exact horizon -- the downside "
       "semivariance coefficient is 0.388 (t 12.8) against 0.091 (t 7.3) for "
       "the upside one: 3-4x the loading. DEGRADED: the source uses 5-minute "
       "returns and we have daily closes, so this is the coarse analogue."});
  put({"f23_semivar_up_21d", Unit::LogVariance, SignPrior::BuyHigh,
       "Patton & Sheppard, REStat 97(3) 2015 (daily-return proxy for 5-min RS+)",
       {spot(-21, 0)},
       "ln of the trailing 21-session UPSIDE realized variance. Ships beside "
       "f22 rather than folded into it because the whole point of the "
       "decomposition is that the two carry DIFFERENT coefficients; a single "
       "combined feature would assert they do not."});
  put({"f24_signed_jump_21d", Unit::Dimensionless, SignPrior::BuyLow,
       "Patton & Sheppard, REStat 97(3) 2015 (signed jump variation)",
       {spot(-21, 0)},
       "(RS+ - RS-) / RV, the signed jump variation scaled to a share. The "
       "continuous part cancels in the difference, leaving the jump asymmetry. "
       "BuyLow: downside-dominated jumps predict HIGHER forward realized vol, "
       "so the cheap-vega side is the negative tail of this feature."});
  put({"f25_leverage_21d", Unit::LogVariance, SignPrior::BuyHigh,
       "Patton & Sheppard, REStat 97(3) 2015 (RV * 1{r<0} leverage term)",
       {spot(-21, 0)},
       "ln(trailing 21d variance) on down-close sessions, ln(floor) otherwise "
       "-- the source's RV_t * 1{r_t < 0} interaction, which enters their h=22 "
       "panel at +0.036 (t 5.1) ON TOP of both semivariances. It is an "
       "interaction, so it is only meaningful WITH f22/f23 in the same fit."});
  put({"f26_gs_hviv_252d", Unit::LogRatio, SignPrior::BuyHigh,
       "Goyal & Saretto, JFE 94(2) 2009 (the AS-PUBLISHED 252-session window)",
       {spot(-252, 0), strip(21, 0, 0)},
       "ln(rv_trail_252d / iv_fair_21d). THE CITED SIGNAL. f5_hv_iv_gap uses a "
       "21-session realized leg, which is NOT what Goyal & Saretto sort on -- "
       "they use one YEAR of daily returns against one-month IV. Since this "
       "panel also carries f2 = ln(rv_21^2) and f3 = ln(iv^2), f5 is close to "
       "collinear with f2 - f3 and the published feature was simply missing. "
       "Campasano & Linn corroborate the long window independently: front IV "
       "is more sensitive to the previous YEAR's realized vol than the "
       "previous month's."});
  put({"f27_sysvol_share_63d", Unit::Dimensionless, SignPrior::BuyHigh,
       "Cao & Han, JFE 108(1) 2013 (SysVOL, the complement of their IVOL)",
       {spot(-63, 0), market(-63, 0)},
       "Systematic share of trailing variance, 1 - resid_var/total_var from a "
       "63-session regression on the market proxy -- exactly what f15_idio_"
       "share is one minus. It ships as its OWN feature because Cao & Han "
       "report the two with OPPOSITE signs in the same Fama-MacBeth: IVOL at "
       "-0.0405 (t -15.46) and SysVOL at +0.016 (t +3.79). The vol you are "
       "PAID to own is market-correlated; the vol you are CHARGED to own is "
       "idiosyncratic, because a dealer cannot hedge it. Reading that as one "
       "signed axis throws away the asymmetry."});
  return reg;
}

// ── The built-in target catalogue ───────────────────────────────────────────
//
// `tradeable` separates a FORECAST axis from a P&L a book can hold. Both are
// legitimate to report; only the second may headline a gate.
[[nodiscard]] inline TargetRegistry builtin_targets() {
  TargetRegistry reg;
  const auto put = [&reg](TargetSpec spec) {
    const Status st = reg.add(std::move(spec));
    (void)st;
  };

  put({"rv_fwd_21d", Unit::VolDecimal, 21, false,
       {spot(1, 21)},
       "Forward realized vol over the holding window: the label's realized leg "
       "ALONE. The primary forecasting gate. Not a P&L -- nobody is paid "
       "rv_fwd."});
  put({"vol_chg_21d", Unit::LogRatio, 21, false,
       {spot(-21, 0), spot(1, 21)},
       "ln(rv_fwd_21d / rv_trail_21d). No iv_fair leg anywhere. NOTE the "
       "trailing read: the axis carries an explicit -ln(rv_trail) leg, so "
       "trailing-vol features are mechanically related to it and the audit says "
       "so."});
  put({"label_contaminated", Unit::Variance, 21, false,
       {spot(1, 21), strip(21, 0, 0)},
       "(rv_fwd^2 - iv_fair_21d^2) * H. Retained for continuity with rounds "
       "1-4 and never gated on: its rank ordering is ANTI-correlated with "
       "realized-vol forecasting skill (rv_trail^2 scores -0.1240 against it)."});
  put({"iv_chg_21d_raw", Unit::VolPoints, 21, false,
       {atmf(21, 0, 0), atmf(21, 21, 21)},
       "100*(iv_atmf_21d[t+H] - iv_atmf_21d[t]). NOT tradeable: iv_atmf_21d is "
       "a CONSTANT-MATURITY index, and the 21d option bought at t has expired "
       "by t+H, so no holder is marked at iv_atmf_21d[t+H]."});
  put({"iv_chg_21d_roll", Unit::VolPoints, 21, true,
       {atmf(21, 0, 0), atmf(21, 21, 21), strip(21, 0, 0), strip(63, 0, 0)},
       "iv_chg_raw minus the term-structure roll a constant-maturity 21d vega "
       "position pays over the same H sessions: roll = 100*(iv_fair_63d - "
       "iv_fair_21d)/2. THE IV-CHANGE AXIS THE MONEY IS GRADED ON."});
  put({"dh_straddle_pnl_21d", Unit::VolPoints, 21, true,
       {spot(1, 21), strip(21, 0, 0)},
       "Delta-hedged ATM-forward straddle P&L per unit vega, ~ (rv_fwd - "
       "iv_fair_21d) in vol points. THE MONEY AXIS FOR A GAMMA/VEGA BOOK. It "
       "carries the entry mark by construction -- that is the trade, not a bug "
       "-- so any feature reading iv_fair_21d[t] scores against it through a "
       "channel as well as through skill, and must be cross-read on rv_fwd_21d "
       "to separate the two."});
  return reg;
}

} // namespace atx::vol::alpha
