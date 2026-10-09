#pragma once
// ── atx::vol::alpha — the alpha layer's vocabulary ───────────────────────────
//
// WHAT A FEATURE IS, WHAT A TARGET IS, AND WHAT EITHER IS ALLOWED TO READ.
//
// WHY THIS EXISTS. Rounds 1-9 of the VRP panel grew features by editing three
// things in lockstep: a `double fN_...` struct field, a frozen
// `kVrpPanelColumnsVN` array, and a compile-time `kVrpFeatureCount` that 18
// call sites in `tools/vrp_train.hpp` index against. Adding one column meant
// minting a new schema VERSION. That is why nine rounds produced ten features,
// and why the round-9 lane's twelve additions arrived as a `V4` enumerator
// rather than as twelve registry entries.
//
// The rule this layer enforces instead:
//
//     A FEATURE IS DATA, NOT A TYPE.
//
// It is a named record with declared inputs, declared units, and a declared
// window. Nothing downstream knows how many features exist at compile time.
// `schema.hpp` derives panel identity from the ordered column names rather
// than from a hand-bumped enumerator, so a panel that gains a column gains a
// fingerprint, not a version.
//
// THE `reads` FIELD IS NOT DOCUMENTATION. It is the machine-checked half of
// the contract: it names WHICH input series a spec touches and over WHICH
// sessions, which is exactly the information `audit.hpp` needs to prove --
// mechanically, before a fit runs -- that a feature does not read the same
// session of the same series its target is built from.
//
// That check is not hypothetical. Round 8 shipped a benchmark whose predictor
// carried its own target's entry mark and scored Spearman +0.9935 against it;
// it was caught by a human reading 6000 lines of trainer. Round 9's twelve new
// features carry the same hazard in prose ("Same entry-mark caveat as f12").
// `reads` is how that stops being a reading exercise.
//
// THE TAXONOMY THIS FILE EXISTS TO KEEP SEPARATE. Two different things get
// called "contamination" and they have opposite remedies:
//
//   * FORWARD LEAK -- the feature reads a session AFTER t. Lookahead. The
//     number is fiction and the only remedy is deletion. Fatal.
//   * ENTRY-MARK CHANNEL -- the feature and the target both read the same
//     series at a session <= t. NOT lookahead: every leg is known at entry and
//     a book can actually trade it. But the predictor is now mechanically
//     correlated with the target whether or not it forecasts anything, so its
//     score is not evidence of skill. The remedy is a second, decontaminated
//     axis, never deletion.
//
// Collapsing those two is how a real, tradeable channel (`f5_hv_iv_gap` against
// a P&L that pays -iv^2) gets deleted as a bug, and how a genuine bug gets
// defended as a channel. They are separate `FindingKind`s here for that reason.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atx::vol::alpha {

// ── The input series a spec may read ────────────────────────────────────────
//
// A SeriesId names a QUANTITY KIND, not a column. Tenor-bearing kinds carry
// their tenor in `SeriesRef::tenor_sessions`, so `iv_fair` at 21 sessions and
// `iv_fair` at 63 sessions are distinguishable without minting two enumerators
// per tenor the board might one day quote.
//
// Realized volatility is deliberately NOT its own kind: it is a function of
// `Spot` over a window, and modelling it as a Spot read is what makes the
// audit correct by construction. `rv_trail_21d` reads Spot[t-20..t] and
// `rv_fwd_21d` reads Spot[t+1..t+21]; they share no session, so no audit rule
// has to know they are "the same statistic at different times".
enum class SeriesId : std::uint8_t {
  // Underlying close series of the panel's own symbol.
  Spot,
  // Var-swap fair strike converted to vol, at `tenor_sessions`. The panel's
  // `iv_fair_21d` / `iv_fair_63d` and round 9's 10d/126d/189d/252d strips.
  IvFairStrip,
  // ATM-forward implied vol read off the fitted surface, at `tenor_sessions`.
  // A DIFFERENT read from IvFairStrip at the same tenor, hence a different
  // enumerator: they are correlated, not identical, and the audit grades that
  // distinction as Info rather than as a shared term.
  IvAtmf,
  // Fixed-|delta| wing vols, at `tenor_sessions`. Feeds RR/BF constructions.
  IvWingCall,
  IvWingPut,
  // Measured ATM quoted half-spread (fraction of mid) and the strike count the
  // session's fit consumed. Reference data joined onto the bar axis.
  LiqHalfSpread,
  LiqStrikesFit,
  // Close series of the market proxy a beta/idio-share regression runs against.
  MarketSpot,
  // Scheduled-event indicator on the bar axis (earnings calendar). Reserved:
  // the loader exists, the data does not yet.
  EventFlag,
};

[[nodiscard]] constexpr std::string_view to_string(SeriesId id) noexcept {
  switch (id) {
  case SeriesId::Spot:
    return "spot";
  case SeriesId::IvFairStrip:
    return "iv_fair_strip";
  case SeriesId::IvAtmf:
    return "iv_atmf";
  case SeriesId::IvWingCall:
    return "iv_wing_call";
  case SeriesId::IvWingPut:
    return "iv_wing_put";
  case SeriesId::LiqHalfSpread:
    return "liq_half_spread";
  case SeriesId::LiqStrikesFit:
    return "liq_strikes_fit";
  case SeriesId::MarketSpot:
    return "market_spot";
  case SeriesId::EventFlag:
    return "event_flag";
  }
  return "unknown";
}

// True for the implied-vol marks of the panel's own name. Two different marks
// of the same tenor at the same session are not the same number, but they are
// close enough that a predictor built on one and a target built on the other
// deserve to be reported. See `FindingKind::CorrelatedEntryMark`.
[[nodiscard]] constexpr bool is_implied_mark(SeriesId id) noexcept {
  return id == SeriesId::IvFairStrip || id == SeriesId::IvAtmf ||
         id == SeriesId::IvWingCall || id == SeriesId::IvWingPut;
}

// ── The sessions a read touches, as offsets from the entry session t ─────────
//
// Both endpoints are INCLUSIVE and both are relative to t, so a trailing
// 21-session close-to-close window is {-20, 0} (21 bars, 20 return terms
// ending at t) and a forward 21-session realized window is {+1, +21}.
//
// A window is CAUSAL at entry iff `last <= 0`. That single inequality is the
// whole lookahead test, which is why it is a field and not a comment.
struct Window {
  std::ptrdiff_t first{0}; // earliest session read, relative to t
  std::ptrdiff_t last{0};  // latest session read, relative to t

  [[nodiscard]] constexpr bool causal() const noexcept { return last <= 0; }

  // Sessions spanned, >= 1 for any well-formed window.
  [[nodiscard]] constexpr std::ptrdiff_t span() const noexcept { return last - first + 1; }

  // Trailing history needed before t, in sessions. Zero for a window that
  // starts at t. Forward-only windows also report zero -- they need no warmup.
  [[nodiscard]] constexpr std::size_t warmup() const noexcept {
    return first < 0 ? static_cast<std::size_t>(-first) : 0U;
  }

  [[nodiscard]] constexpr bool well_formed() const noexcept { return first <= last; }

  // Do these two windows touch a common session?
  [[nodiscard]] constexpr bool overlaps(const Window &other) const noexcept {
    return first <= other.last && other.first <= last;
  }

  // The same window shifted `lag` sessions further into the past. This is what
  // `--feature-lag k` does to every feature window, and expressing it here is
  // what lets the audit COMPUTE that a lag closes an entry-mark channel rather
  // than asserting it in a comment.
  [[nodiscard]] constexpr Window lagged(std::ptrdiff_t lag) const noexcept {
    return Window{first - lag, last - lag};
  }
};

[[nodiscard]] constexpr bool operator==(const Window &a, const Window &b) noexcept {
  return a.first == b.first && a.last == b.last;
}

// One declared read: a series, its tenor if it has one, and the sessions.
struct SeriesRef {
  SeriesId series{SeriesId::Spot};
  // Tenor in SESSIONS for tenor-bearing kinds; 0 when the kind carries none.
  // Sessions rather than years so 21/63 compare exactly as integers -- the
  // panel's own `kVrpTenor21Years = 21.0/252.0` does not round-trip through a
  // double comparison the way a session count does.
  std::int32_t tenor_sessions{0};
  Window window{};

  // Two reads collide when they are the same quantity at a shared session.
  [[nodiscard]] constexpr bool same_quantity(const SeriesRef &other) const noexcept {
    return series == other.series && tenor_sessions == other.tenor_sessions;
  }
};

// ── Units ───────────────────────────────────────────────────────────────────
//
// Carried so the audit can refuse to compare a vol-point quantity against a
// log-variance one, and so a report can label an axis without the reader
// having to remember which round chose which scale.
enum class Unit : std::uint8_t {
  Dimensionless,
  VolDecimal,  // annualized vol, 0.24 == 24%
  VolPoints,   // annualized vol x 100, 24.0 == 24%
  Variance,    // annualized variance, vol^2
  LogVariance, // ln(annualized variance)
  LogRatio,    // ln(a / b), e.g. f5_hv_iv_gap
  LogReturn,
  Fraction,  // unit-free share in [0, 1]
  Rank,      // percentile in (0, 1)
  Indicator, // 0 or 1
};

[[nodiscard]] constexpr std::string_view to_string(Unit unit) noexcept {
  switch (unit) {
  case Unit::Dimensionless:
    return "dimensionless";
  case Unit::VolDecimal:
    return "vol_decimal";
  case Unit::VolPoints:
    return "vol_points";
  case Unit::Variance:
    return "variance";
  case Unit::LogVariance:
    return "log_variance";
  case Unit::LogRatio:
    return "log_ratio";
  case Unit::LogReturn:
    return "log_return";
  case Unit::Fraction:
    return "fraction";
  case Unit::Rank:
    return "rank";
  case Unit::Indicator:
    return "indicator";
  }
  return "unknown";
}

// ── The prior a LONG-VEGA book carries into the fit ─────────────────────────
//
// Recorded per feature BEFORE anything is measured, with the citation that
// supplies it, so that a disagreement between the published sign and this
// panel's measurement is a FINDING rather than a fitting choice. `None` is an
// honest answer and the commonest one; it is not a placeholder.
enum class SignPrior : std::uint8_t {
  None,    // no published cross-sectional result in the long-vega direction
  BuyHigh, // a long-vega book wants the HIGH end of this feature
  BuyLow,  // ... the LOW end
};

[[nodiscard]] constexpr std::string_view to_string(SignPrior prior) noexcept {
  switch (prior) {
  case SignPrior::None:
    return "none";
  case SignPrior::BuyHigh:
    return "buy_high";
  case SignPrior::BuyLow:
    return "buy_low";
  }
  return "unknown";
}

// ── A feature ───────────────────────────────────────────────────────────────
struct FeatureSpec {
  std::string name;
  Unit unit{Unit::Dimensionless};
  SignPrior prior{SignPrior::None};
  // Peer-reviewed source for the construct AND its sign, or empty when the
  // construct is this repo's own. Empty is not a defect; an unsourced feature
  // simply cannot claim a prior, which is why `prior` defaults to `None`.
  std::string citation;
  std::vector<SeriesRef> reads;
  std::string doc;

  // Trailing sessions of history the feature needs before it can emit a
  // finite value: the deepest lookback across its reads.
  [[nodiscard]] std::size_t warmup_sessions() const noexcept {
    std::size_t worst = 0;
    for (const SeriesRef &ref : reads) {
      const std::size_t need = ref.window.warmup();
      if (need > worst) {
        worst = need;
      }
    }
    return worst;
  }

  // Causal iff every declared read ends at or before t.
  [[nodiscard]] bool causal() const noexcept {
    for (const SeriesRef &ref : reads) {
      if (!ref.window.causal()) {
        return false;
      }
    }
    return true;
  }
};

// ── A target ────────────────────────────────────────────────────────────────
//
// `tradeable` is load-bearing, not a label. A forecast axis (`rv_fwd_21d`) and
// a P&L axis (`iv_chg_21d_roll`) are both legitimate to REPORT, but only the
// second is a number a book can actually hold, and a gate that headlines the
// first is quoting a forecast as a return. The audit says so.
struct TargetSpec {
  std::string name;
  Unit unit{Unit::Dimensionless};
  // Holding period in sessions. Zero for a contemporaneous axis.
  std::size_t horizon_sessions{0};
  // Is this a P&L a book can hold, net of the roll/mark mechanics that make a
  // constant-maturity index NOT a position? See `iv_chg_21d_raw` vs
  // `iv_chg_21d_roll` in `tools/vrp_train.hpp`.
  bool tradeable{false};
  std::vector<SeriesRef> reads;
  std::string doc;

  // The forward sessions the target consumes: the widest `last` across reads.
  // A feature touching any session in (0, forward_reach] is a forward leak.
  [[nodiscard]] std::ptrdiff_t forward_reach() const noexcept {
    std::ptrdiff_t reach = 0;
    for (const SeriesRef &ref : reads) {
      if (ref.window.last > reach) {
        reach = ref.window.last;
      }
    }
    return reach;
  }

  // The legs of the target that are already known at entry. A predictor
  // sharing one of these is an entry-mark channel, never a leak.
  [[nodiscard]] std::vector<SeriesRef> entry_legs() const {
    std::vector<SeriesRef> legs;
    legs.reserve(reads.size());
    for (const SeriesRef &ref : reads) {
      if (ref.window.causal()) {
        legs.push_back(ref);
      }
    }
    return legs;
  }
};

} // namespace atx::vol::alpha
