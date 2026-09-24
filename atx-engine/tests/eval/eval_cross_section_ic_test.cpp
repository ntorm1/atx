// atx-engine/tests/eval/eval_cross_section_ic_test.cpp
//
// Checkpoint 14, task T1: the types, the §6.2 validation surface and the §5
// calendar seal of `eval::cross_section_ic`. Case names and numbers are those of
// §9.1 of
// `atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md`.
//
// T2/T3 added every remaining §9.1 case (1, 2b, 3..13b, 14..20e, 28, 29) plus
// the §9.2 native export, which prints one `CROSS_SECTION_IC_MEASUREMENT ` line
// per oracle scenario for `build-equity/audits/iteration14_native_comparator.py`.
//
// Synthetic panels only — no file I/O, no clock, no RNG.

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/stats/cross_section.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"
#include "atx/engine/learn/feature_matrix.hpp" // case 2b ONLY
#include "atx/engine/learn/latent.hpp"         // case 2a ONLY: learn::detail::pearson
#include "atx/engine/learn/linear_alpha.hpp"   // case 2b ONLY: learn::detail::oof_ic_series

namespace atxtest_eval_cross_section_ic {

using atx::core::ErrorCode;
using namespace atx::engine::eval;

// The two calendar boundaries are NOT restated here: ruling RR-2 makes the
// header the single spelling, and this file uses `eval::kValidationBeginNs` /
// `eval::kSealedBeginNs` throughout so a transposed digit cannot hide behind a
// test that agrees with itself. `kNanosPerDay` likewise comes from the header.
constexpr atx::i64 kDayNs = kNanosPerDay;
constexpr atx::i64 k2013Apr04Ns = 1'365'033'600'000'000'000; // 2013-04-04T00:00:00Z

// ---------------------------------------------------------------------------
//  Panel — backing storage for a well-formed CrossSectionIcInput. Every span of
//  the view borrows from this object, so a Panel must outlive every view taken
//  from it (the BORROWED-span contract of §6).
// ---------------------------------------------------------------------------
struct Panel {
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<atx::f64> signal, price, raw_price, terminal_value;
  std::vector<atx::u8> mask, terminal, terminal_evidenced, excluded_audited;
  std::vector<atx::i64> session_keys;

  Panel(atx::usize d, atx::usize i) : dates{d}, instruments{i} {
    const atx::usize cells = d * i;
    signal.assign(cells, 0.0);
    price.assign(cells, 100.0);
    raw_price.assign(cells, 100.0);
    terminal_value.assign(cells, 0.0);
    mask.assign(cells, atx::u8{1});
    terminal.assign(cells, atx::u8{0});
    terminal_evidenced.assign(cells, atx::u8{0});
    excluded_audited.assign(cells, atx::u8{0});
    session_keys.resize(d);
    for (atx::usize t = 0U; t < d; ++t) {
      session_keys[t] = k2013Apr04Ns + kDayNs * static_cast<atx::i64>(t);
    }
  }

  [[nodiscard]] CrossSectionIcInput view() const {
    CrossSectionIcInput in{};
    in.dates = dates;
    in.instruments = instruments;
    in.signal = signal;
    in.price = price;
    in.raw_price = raw_price;
    in.mask = mask;
    in.terminal = terminal;
    in.terminal_evidenced = terminal_evidenced;
    in.terminal_value = terminal_value;
    in.excluded_audited = excluded_audited;
    in.session_keys = session_keys;
    return in;
  }
};

// A configuration that passes every §6.2 branch; each test perturbs one field.
[[nodiscard]] CrossSectionIcConfig good_config(std::span<const atx::usize> horizons,
                                               atx::usize draws = 64U) {
  CrossSectionIcConfig cfg{};
  cfg.horizons = horizons;
  cfg.quantiles = 10U;
  cfg.min_names_per_date = 2U;
  cfg.bootstrap_draws = draws;
  cfg.block_len_floor = 5U;
  cfg.bootstrap_seed = 20260920ULL;
  cfg.trade_bps = 5.0;
  cfg.annual_borrow_bps = 365.0;
  cfg.short_leg_gross = 1.0;
  cfg.day_basis = 365;
  cfg.common_sample_dates = 0U;
  cfg.forward_variant = ForwardReturnVariant::DropMissingForward;
  cfg.ties = IcTieHandling::AverageRanksV1;
  return cfg;
}

[[nodiscard]] ErrorCode plan_error(const CrossSectionIcInput &in,
                                   const CrossSectionIcConfig &cfg) {
  auto planned = plan_cross_section_ic(in, cfg);
  if (planned.has_value()) {
    return ErrorCode::Unknown; // "no error"; every caller below expects otherwise
  }
  return planned.error().code();
}

// ===========================================================================
//  Case 2a — the R-6 obligation.
//
//  eval::detail::pearson is a deliberate COPY of learn::detail::pearson
//  (latent.hpp:151-178) so an eval TU does not depend on learn/. This is the
//  only case that includes a learn/ header, and it pins exact f64 equality — no
//  tolerance — so the two copies cannot drift silently. The n < 2 and
//  constant-series inputs are part of the same comparison because both guard
//  branches are contract, not padding (§3.4).
// ===========================================================================
TEST(EvalCrossSectionIc, Pearson_FixedVector_IsBitIdenticalToLearnDetailPearson) {
  const std::vector<atx::f64> a{-3.5, 0.25, 1.0, 7.125, 11.0, -0.75};
  const std::vector<atx::f64> b{2.0, -1.5, 0.5, 9.25, 3.0, -8.125};
  EXPECT_EQ(detail::pearson(a, b), atx::engine::learn::detail::pearson(a, b));

  // MIN-7: both copies are inlined in this TU, so equality alone would survive a
  // mutation that broke them identically (zeroing `cov`, say). Pin that the
  // shared value is a real correlation: finite, non-zero, and inside [-1, 1].
  const atx::f64 shared = detail::pearson(a, b);
  EXPECT_TRUE(std::isfinite(shared));
  EXPECT_NE(shared, 0.0);
  EXPECT_GE(shared, -1.0);
  EXPECT_LE(shared, 1.0);

  // n < 2 early return: 0.0, not NaN.
  const std::vector<atx::f64> one{4.0};
  EXPECT_EQ(detail::pearson(one, one), atx::engine::learn::detail::pearson(one, one));
  EXPECT_EQ(detail::pearson(one, one), 0.0);

  // constant series: the va == 0 / vb == 0 branch, 0.0 rather than 0/0 NaN.
  const std::vector<atx::f64> flat{2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
  EXPECT_EQ(detail::pearson(flat, b), atx::engine::learn::detail::pearson(flat, b));
  EXPECT_EQ(detail::pearson(flat, b), 0.0);
  EXPECT_EQ(detail::pearson(a, flat), atx::engine::learn::detail::pearson(a, flat));
  EXPECT_EQ(detail::pearson(a, flat), 0.0);
}

// ===========================================================================
//  The two frozen bootstrap predicates, tested DIRECTLY (ruling RR-3).
//
//  Both are pinned here as well as through the `draws == 0` acceptance path,
//  because a frozen value deserves the test a reader expects to find beside it,
//  and because each clause below is one a mutation could delete while the
//  indirect tests stayed green.
// ===========================================================================

// §3.10 / §4.3: L_h = max(block_len_floor, ceil(h/2)) -> {5, 5, 5, 11, 32} for
// the frozen horizon set at the pre-registered floor of 5. BOTH terms decide
// somewhere in that table, so neither can be dropped.
TEST(EvalCrossSectionIc, BlockLength_FrozenHorizonSet_MatchesThePreRegisteredTable) {
  EXPECT_EQ(detail::block_len(1U, 5U), 5U);   // floor decides: ceil(1/2) = 1
  EXPECT_EQ(detail::block_len(5U, 5U), 5U);   // tie: ceil(5/2) = 3 < 5
  EXPECT_EQ(detail::block_len(10U, 5U), 5U);  // ceil(10/2) = 5, equal to the floor
  EXPECT_EQ(detail::block_len(21U, 5U), 11U); // ceil term decides: ceil(21/2) = 11
  EXPECT_EQ(detail::block_len(63U, 5U), 32U); // ceil term decides: ceil(63/2) = 32

  // ceil, not floor, for odd horizons — the whole point of the (h + 1) / 2
  // spelling, and the difference between 32 and 31 at h = 63.
  EXPECT_EQ(detail::block_len(3U, 1U), 2U);
  EXPECT_EQ(detail::block_len(11U, 1U), 6U);
  EXPECT_EQ(detail::block_len(12U, 1U), 6U);
}

// §3.10 / §4.3: reportable iff draws >= 1 AND n >= 20 AND floor(n/L) >= 10.
// Each clause is made the deciding one at least once.
TEST(EvalCrossSectionIc, SeriesReportable_FrozenRule_EachClauseDecidesIndependently) {
  EXPECT_TRUE(detail::series_reportable(188U, 5U, 2000U));  // h = 1  at T = 189
  EXPECT_TRUE(detail::series_reportable(168U, 11U, 2000U)); // h = 21 at T = 189
  EXPECT_FALSE(detail::series_reportable(126U, 32U, 2000U)); // h = 63: 126/32 = 3

  // the draws clause decides
  EXPECT_FALSE(detail::series_reportable(188U, 5U, 0U));
  EXPECT_TRUE(detail::series_reportable(188U, 5U, 1U));

  // the n >= 20 clause decides: 19/1 = 19 >= 10, so only the length bar fails
  EXPECT_FALSE(detail::series_reportable(19U, 1U, 2000U));
  EXPECT_TRUE(detail::series_reportable(20U, 1U, 2000U));

  // the floor(n/L) >= 10 clause decides: n is long enough, the blocks are not
  EXPECT_FALSE(detail::series_reportable(99U, 10U, 2000U)); // 99/10 = 9
  EXPECT_TRUE(detail::series_reportable(100U, 10U, 2000U)); // 100/10 = 10
}

// ===========================================================================
//  §6.2 validation — the configuration surface.
// ===========================================================================

// Case 24 — rejectable enums: a value-initialized config must not acquire a
// policy by omission.
TEST(EvalCrossSectionIc, Config_UnknownVariantOrTieHandling_ReturnsInvalidArgument) {
  const Panel p{30U, 4U};
  const std::vector<atx::usize> h{1U};

  CrossSectionIcConfig cfg = good_config(h);
  cfg.forward_variant = ForwardReturnVariant::Unknown;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.ties = IcTieHandling::Unknown;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  // A default-constructed config is Unknown in both fields, so it rejects.
  CrossSectionIcConfig bare{};
  bare.horizons = h;
  EXPECT_EQ(plan_error(p.view(), bare), ErrorCode::InvalidArgument);

  // The baseline the perturbations are measured against actually passes.
  EXPECT_TRUE(plan_cross_section_ic(p.view(), good_config(h)).has_value());
}

// Case 25 — one mis-sized span at a time; every panel span is dates*instruments
// and session_keys is dates.
TEST(EvalCrossSectionIc, Config_MisSizedSpan_ReturnsOutOfRange) {
  const Panel p{30U, 4U};
  const std::vector<atx::usize> h{1U};
  const CrossSectionIcConfig cfg = good_config(h);

  {
    CrossSectionIcInput in = p.view();
    in.signal = in.signal.first(in.signal.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.price = in.price.first(in.price.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.raw_price = in.raw_price.first(in.raw_price.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.mask = in.mask.first(in.mask.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.terminal = in.terminal.first(in.terminal.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.terminal_evidenced = in.terminal_evidenced.first(in.terminal_evidenced.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.terminal_value = in.terminal_value.first(in.terminal_value.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.excluded_audited = in.excluded_audited.first(in.excluded_audited.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.session_keys = in.session_keys.first(in.session_keys.size() - 1U);
    EXPECT_EQ(plan_error(in, cfg), ErrorCode::OutOfRange);
  }
}

// Case 26 — strictly increasing session keys. A repeated key is as bad as a
// reversed one: §3.12 divides their differences into calendar days.
TEST(EvalCrossSectionIc, Config_NonMonotonicSessionKeys_ReturnsInvalidArgument) {
  const std::vector<atx::usize> h{1U};
  {
    Panel p{30U, 4U};
    p.session_keys[7] = p.session_keys[6]; // equal — not STRICTLY increasing
    EXPECT_EQ(plan_error(p.view(), good_config(h)), ErrorCode::InvalidArgument);
  }
  {
    Panel p{30U, 4U};
    p.session_keys[7] = p.session_keys[6] - kDayNs; // reversed
    EXPECT_EQ(plan_error(p.view(), good_config(h)), ErrorCode::InvalidArgument);
  }
}

// Case 27 — a horizon at or past `dates` leaves no date with a forward mark, so
// it is a shape error rather than a short series.
TEST(EvalCrossSectionIc, Config_HorizonAtOrPastDates_ReturnsOutOfRange) {
  const Panel p{30U, 4U};
  {
    const std::vector<atx::usize> h{1U, 30U}; // == dates
    EXPECT_EQ(plan_error(p.view(), good_config(h)), ErrorCode::OutOfRange);
  }
  {
    const std::vector<atx::usize> h{1U, 31U}; // > dates
    EXPECT_EQ(plan_error(p.view(), good_config(h)), ErrorCode::OutOfRange);
  }
  {
    const std::vector<atx::usize> h{29U}; // dates - 1: the last admissible horizon
    EXPECT_TRUE(plan_cross_section_ic(p.view(), good_config(h)).has_value());
  }
}

// Case 14b — the C-7 item 3 UB branch is rejected at validation and never
// reached: with draws == 0 the percentile step would read an empty span behind
// an ATX_ASSERT that compiles away in `rel`.
TEST(EvalCrossSectionIc, Bootstrap_ZeroDrawsWithReportableHorizon_ReturnsInvalidArgument) {
  // dates = 60, h = 1 -> n = 59, L = max(5, 1) = 5, floor(59/5) = 11 >= 10 and
  // n >= 20, so this horizon WOULD be reportable but for draws == 0.
  const Panel p{60U, 4U};
  const std::vector<atx::usize> h{1U};
  EXPECT_EQ(plan_error(p.view(), good_config(h, 0U)), ErrorCode::InvalidArgument);
}

// Companion to 14b: draws == 0 is a legitimate whole-run choice when no horizon
// would have produced an interval anyway — the rule rejects silent degradation,
// not disabled intervals as such.
TEST(EvalCrossSectionIc, Config_ZeroDrawsWithNoReportableHorizon_IsAccepted) {
  // dates = 30, h = 1 -> n = 29, L = 5, floor(29/5) = 5 < 10: not reportable.
  const Panel p{30U, 4U};
  const std::vector<atx::usize> h{1U};
  EXPECT_TRUE(plan_cross_section_ic(p.view(), good_config(h, 0U)).has_value());
}

// IMP-2, through the validation surface: the ceil(h/2) term of `block_len` is
// what makes these horizons unreportable, so `draws == 0` is accepted. Mutate
// `block_len` to `return floor_len;` and L drops to 5, both series become
// reportable, and the plan rejects — which is the same mutation that would emit
// a bootstrap interval at h = 63 where §4.4 pre-registers a null one.
TEST(EvalCrossSectionIc, Bootstrap_ZeroDrawsAtLongHorizon_IsAcceptedBecauseCeilTermSetsBlockLen) {
  {
    // n = 200 - 63 = 137, L = 32 -> 137/32 = 4 < 10. Mutated: 137/5 = 27 >= 10.
    const Panel p{200U, 4U};
    const std::vector<atx::usize> h{63U};
    EXPECT_TRUE(plan_cross_section_ic(p.view(), good_config(h, 0U)).has_value());
  }
  {
    // n = 100 - 21 = 79, L = 11 -> 79/11 = 7 < 10. Mutated: 79/5 = 15 >= 10.
    // NOTE: dates = 100, not the 200 the review suggested — at 200 the true
    // L = 11 already gives 179/11 = 16 >= 10, so that fixture rejects under BOTH
    // the correct and the mutated implementation and discriminates nothing.
    const Panel p{100U, 4U};
    const std::vector<atx::usize> h{21U};
    EXPECT_TRUE(plan_cross_section_ic(p.view(), good_config(h, 0U)).has_value());
  }
}

// IMP-3, through the validation surface: at block_len_floor = 1 the n/L clause
// passes (19/1 = 19 >= 10) and only `n >= 20` keeps the series unreportable, so
// `draws == 0` is accepted. Delete that clause and the plan rejects.
TEST(EvalCrossSectionIc, Bootstrap_ZeroDrawsWithNineteenPointSeries_IsAcceptedAsTooShort) {
  const Panel p{20U, 4U};
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h, 0U);
  cfg.block_len_floor = 1U;
  EXPECT_TRUE(plan_cross_section_ic(p.view(), cfg).has_value());

  // One more date and the same configuration is reportable, so draws == 0 is
  // then rejected — the boundary is at n == 20 exactly.
  const Panel wider{21U, 4U};
  EXPECT_EQ(plan_error(wider.view(), cfg), ErrorCode::InvalidArgument);
}

// The remaining §6.2 scalar contracts, one perturbation each.
TEST(EvalCrossSectionIc, Config_OutOfContractScalars_ReturnInvalidArgument) {
  const Panel p{30U, 4U};
  const std::vector<atx::usize> h{1U};
  const atx::f64 nan_value = std::numeric_limits<atx::f64>::quiet_NaN();
  const atx::f64 inf_value = std::numeric_limits<atx::f64>::infinity();

  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 1U;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.min_names_per_date = 1U;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.block_len_floor = 0U;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.trade_bps = -1.0;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.trade_bps = nan_value;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.annual_borrow_bps = inf_value;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.short_leg_gross = 0.0; // positive, not merely non-negative
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.day_basis = 366;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  // IMP-4: §6.2 and §4.3 admit 360 OR 365. Only the 360 side needs pinning — a
  // mutation to `day_basis != 365` would silently reject an ACT/360 sensitivity
  // or a reconciliation against a 360-basis replay configuration, while the
  // mirror mutation (`!= 360`) is already caught by the deployed-365 baseline.
  cfg = good_config(h);
  cfg.day_basis = 360;
  EXPECT_TRUE(plan_cross_section_ic(p.view(), cfg).has_value());

  cfg = good_config(h);
  cfg.common_sample_dates = 31U; // > dates
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.common_sample_dates = 30U; // == dates is the boundary and is admissible
  EXPECT_TRUE(plan_cross_section_ic(p.view(), cfg).has_value());
}

// Zero extents and a malformed horizon list.
TEST(EvalCrossSectionIc, Config_ZeroExtentsOrMalformedHorizons_ReturnInvalidArgument) {
  const Panel p{30U, 4U};
  const std::vector<atx::usize> h{1U};

  {
    CrossSectionIcInput in = p.view();
    in.dates = 0U;
    EXPECT_EQ(plan_error(in, good_config(h)), ErrorCode::InvalidArgument);
  }
  {
    CrossSectionIcInput in = p.view();
    in.instruments = 0U;
    EXPECT_EQ(plan_error(in, good_config(h)), ErrorCode::InvalidArgument);
  }
  {
    const std::vector<atx::usize> empty{};
    EXPECT_EQ(plan_error(p.view(), good_config(empty)), ErrorCode::InvalidArgument);
  }
  {
    const std::vector<atx::usize> with_zero{0U, 5U}; // horizon 0 is not a forward return
    EXPECT_EQ(plan_error(p.view(), good_config(with_zero)), ErrorCode::InvalidArgument);
  }
  {
    const std::vector<atx::usize> unsorted{5U, 1U};
    EXPECT_EQ(plan_error(p.view(), good_config(unsorted)), ErrorCode::InvalidArgument);
  }
  {
    const std::vector<atx::usize> repeated{5U, 5U}; // strictly increasing, not merely sorted
    EXPECT_EQ(plan_error(p.view(), good_config(repeated)), ErrorCode::InvalidArgument);
  }
}

// The bounded maxima of §6.1 / ruling R-7 are enforced, not decorative.
TEST(EvalCrossSectionIc, Config_BoundedMaxima_RejectOversizedHorizonsQuantilesAndDraws) {
  const Panel p{30U, 4U};

  std::vector<atx::usize> too_many;
  for (atx::usize k = 0U; k <= kMaxIcHorizons; ++k) {
    too_many.push_back(k + 1U); // kMaxIcHorizons + 1 strictly increasing horizons
  }
  ASSERT_GT(too_many.size(), kMaxIcHorizons);
  EXPECT_EQ(plan_error(p.view(), good_config(too_many)), ErrorCode::InvalidArgument);

  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = kMaxIcQuantiles + 1U;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::InvalidArgument);

  cfg = good_config(h);
  cfg.bootstrap_draws = kMaxBootstrapDraws + 1U;
  EXPECT_EQ(plan_error(p.view(), cfg), ErrorCode::OutOfRange);
}

// Ruling RR-1 (design §11.5): `dates` and `instruments` carry compile-time
// maxima too, checked before any sizing, so `plan_cross_section_ic` cannot reach
// an `.assign()` large enough to throw `std::bad_alloc` in a unit that promises
// no path throws. Both sides of each boundary are pinned, so a `>=`-for-`>`
// mutation cannot hide.
TEST(EvalCrossSectionIc, Config_ExtentsAboveBoundedMaxima_ReturnOutOfRange) {
  const Panel p{30U, 4U};
  const std::vector<atx::usize> h{1U};

  // Rejecting side. The extent bound is checked before the span-shape checks, so
  // overriding the declared extent on a small panel reaches it without
  // allocating a 4,097-wide fixture.
  {
    CrossSectionIcInput in = p.view();
    in.dates = kMaxIcDates + 1U;
    EXPECT_EQ(plan_error(in, good_config(h)), ErrorCode::OutOfRange);
  }
  {
    CrossSectionIcInput in = p.view();
    in.instruments = kMaxIcInstruments + 1U;
    EXPECT_EQ(plan_error(in, good_config(h)), ErrorCode::OutOfRange);
  }

  // Accepting side, at the boundary exactly. Kept cheap by making the other
  // extent 1 (4,096 cells) rather than squaring the maximum.
  {
    const Panel tall{kMaxIcDates, 1U};
    EXPECT_TRUE(plan_cross_section_ic(tall.view(), good_config(h)).has_value());
  }
  {
    const Panel wide{2U, kMaxIcInstruments};
    EXPECT_TRUE(plan_cross_section_ic(wide.view(), good_config(h)).has_value());
  }

  // Stage 2's planned shape (~1,950 dates x ~1,661 instruments) is inside both
  // bounds; asserted as arithmetic so the constants cannot be lowered under it.
  EXPECT_GT(kMaxIcDates, 1950U);
  EXPECT_GT(kMaxIcInstruments, 1661U);
}

// ===========================================================================
//  plan_cross_section_ic — scratch sizing (§6.1).
// ===========================================================================

[[nodiscard]] std::size_t scratch_bytes(const CrossSectionIcScratch &s) {
  const std::size_t f64s = s.x.size() + s.r.size() + s.rx.size() + s.rr.size() + s.buf.size() +
                           s.w_prev.size() + s.w_curr.size() + s.draw.size() + s.stat.size();
  const std::size_t usizes = s.order.size() + s.perm.size();
  return f64s * sizeof(atx::f64) + usizes * sizeof(atx::usize);
}

TEST(EvalCrossSectionIc, Plan_SizesEveryScratchBufferToItsDeclaredExtent) {
  const Panel p{30U, 7U};
  const std::vector<atx::usize> h{1U, 5U};
  auto planned = plan_cross_section_ic(p.view(), good_config(h, 64U));
  ASSERT_TRUE(planned.has_value());
  const CrossSectionIcScratch &s = *planned;

  EXPECT_EQ(s.x.size(), 7U);
  EXPECT_EQ(s.r.size(), 7U);
  EXPECT_EQ(s.rx.size(), 7U);
  EXPECT_EQ(s.rr.size(), 7U);
  EXPECT_EQ(s.buf.size(), 7U);
  EXPECT_EQ(s.order.size(), 7U);
  EXPECT_EQ(s.perm.size(), 7U);
  EXPECT_EQ(s.w_prev.size(), 7U);
  EXPECT_EQ(s.w_curr.size(), 7U);
  EXPECT_EQ(s.draw.size(), 30U);  // one circular-block resample, `dates` long
  EXPECT_EQ(s.stat.size(), 64U);  // the B draw statistics; without it the
                                  // zero-allocation claim does not hold
}

// §6.1's arithmetic, at the frozen Stage-1 shape: I = 1,661, T = 189, B = 2,000
// gives 9*1661*8 + 189*8 + 2000*8 = 119,592 + 1,512 + 16,000 = 137,104 bytes.
TEST(EvalCrossSectionIc, Plan_FrozenStageOneShape_SizesScratchToOneHundredThirtySevenKilobytes) {
  const Panel p{189U, 1661U};
  const std::vector<atx::usize> h{1U, 5U, 10U, 21U, 63U};
  CrossSectionIcConfig cfg = good_config(h, 2000U);
  cfg.common_sample_dates = 126U; // MIN-10: T - max(H); the actual frozen shape
  auto planned = plan_cross_section_ic(p.view(), cfg);
  ASSERT_TRUE(planned.has_value());
  EXPECT_EQ(scratch_bytes(*planned), std::size_t{137104});
}

// ===========================================================================
//  The calendar seal (§5.2).
//
//  Cases 21 and 23 together are the non-vacuity argument: the gate REJECTS a
//  sealed observation (21) and PASSES the frozen 2013 slice while recording that
//  it counted zero (23). Non-vacuous by code, vacuous by data.
// ===========================================================================

[[nodiscard]] std::vector<atx::i64> seal_keys(atx::usize n_2013, bool append_sealed) {
  std::vector<atx::i64> keys;
  for (atx::usize t = 0U; t < n_2013; ++t) {
    keys.push_back(k2013Apr04Ns + kDayNs * static_cast<atx::i64>(t));
  }
  if (append_sealed) {
    keys.push_back(kSealedBeginNs); // the boundary itself is INSIDE the seal
  }
  return keys;
}

// Ruling RR-2: the engine owns the one spelling of each calendar boundary, and
// this pins the values against the design's own dates so that a transposed digit
// breaks a test rather than silently sealing the wrong period — a failure mode
// the Stage-1 receipt (`dates_at_or_after_sealed == 0`) could not detect. The
// header cross-checks its two spellings with static_asserts; this repeats the
// check from the outside, in the units the design writes them in.
TEST(EvalCrossSectionIc, Seal_ExportedBoundaryConstants_MatchTheFrozenCalendarDates) {
  EXPECT_EQ(kNanosPerDay, atx::i64{86'400'000'000'000});
  EXPECT_EQ(kValidationBeginNs, atx::i64{1'577'836'800'000'000'000}); // 2020-01-01T00:00:00Z
  EXPECT_EQ(kSealedBeginNs, atx::i64{1'672'531'200'000'000'000});     // 2023-01-01T00:00:00Z
  // Days since the Unix epoch. 19358, NOT 19723 — 19723 is 2024-01-01
  // (1'704'067'200 s), and the header's static_assert caught exactly that error
  // in fix round 1 before it could seal a year too late.
  EXPECT_EQ(kValidationBeginNs / kNanosPerDay, atx::i64{18262});
  EXPECT_EQ(kSealedBeginNs / kNanosPerDay, atx::i64{19358});
  EXPECT_LT(kValidationBeginNs, kSealedBeginNs);

  // The frozen 2013 evaluation window sits strictly below both boundaries, which
  // is the "vacuous by data" half of §5.2's claim.
  EXPECT_LT(k2013Apr04Ns, kValidationBeginNs);

  // A default-constructed CalendarSeal carries the frozen boundaries but still
  // an Unknown policy, so it rejects rather than sealing by omission.
  const CalendarSeal defaulted{};
  EXPECT_EQ(defaulted.validation_begin_ns, kValidationBeginNs);
  EXPECT_EQ(defaulted.sealed_begin_ns, kSealedBeginNs);
  EXPECT_EQ(defaulted.policy, SealPolicy::Unknown);
}

// Case 21.
TEST(EvalCrossSectionIc, Seal_DateAtOrAfterSealedBoundary_ReturnsPermissionDenied) {
  const std::vector<atx::i64> keys = seal_keys(5U, true);
  const CalendarSeal seal{kValidationBeginNs, kSealedBeginNs, SealPolicy::RejectSealedV1};

  auto sealed = apply_calendar_seal(keys, seal);
  ASSERT_FALSE(sealed.has_value());
  EXPECT_EQ(sealed.error().code(), ErrorCode::PermissionDenied);
  // The count travels in the message: Result<SealReport> carries no report on
  // the error path, so the message is where the stage reads it.
  EXPECT_NE(sealed.error().message().find("1 input observations at or after the sealed boundary"),
            std::string::npos);
}

// Case 22 — no default policy.
TEST(EvalCrossSectionIc, Seal_UnknownPolicy_ReturnsInvalidArgument) {
  const std::vector<atx::i64> keys = seal_keys(5U, false);
  const CalendarSeal seal{kValidationBeginNs, kSealedBeginNs, SealPolicy::Unknown};

  auto sealed = apply_calendar_seal(keys, seal);
  ASSERT_FALSE(sealed.has_value());
  EXPECT_EQ(sealed.error().code(), ErrorCode::InvalidArgument);

  // A default-constructed CalendarSeal is Unknown, so it rejects too.
  auto bare = apply_calendar_seal(keys, CalendarSeal{});
  ASSERT_FALSE(bare.has_value());
  EXPECT_EQ(bare.error().code(), ErrorCode::InvalidArgument);
}

// Case 23 — the Stage-1 shape.
TEST(EvalCrossSectionIc, Seal_AllDatesBeforeBoundary_ReportsZeroCountsAndSucceeds) {
  const std::vector<atx::i64> keys = seal_keys(189U, false);
  const CalendarSeal seal{kValidationBeginNs, kSealedBeginNs, SealPolicy::RejectSealedV1};

  auto sealed = apply_calendar_seal(keys, seal);
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->dates_total, 189U);
  EXPECT_EQ(sealed->dates_visible, 189U);
  EXPECT_EQ(sealed->dates_at_or_after_validation, 0U);
  EXPECT_EQ(sealed->dates_at_or_after_sealed, 0U);
  // reserve_lockbox's index carve is NOT used: on a 2013-only panel it would
  // reserve 2013 dates, not 2023-2025 (ruling R-2). Nothing was carved here.
  EXPECT_EQ(sealed->used_reserve_window, atx::u8{0});
  EXPECT_EQ(sealed->content_address, 0U);
  EXPECT_EQ(sealed->embargo_len, 0U);
}

// The masking branch: `dates_visible` is the FIRST sealed index and the caller
// truncates. Not used in checkpoint 14; it exists so the enum has a rejectable
// alternative rather than a silent one.
TEST(EvalCrossSectionIc, Seal_MaskPolicy_ReportsFirstSealedIndexAsDatesVisible) {
  std::vector<atx::i64> keys = seal_keys(4U, false);
  keys.push_back(kSealedBeginNs);
  keys.push_back(kSealedBeginNs + kDayNs);
  const CalendarSeal seal{kValidationBeginNs, kSealedBeginNs, SealPolicy::MaskSealedV1};

  auto sealed = apply_calendar_seal(keys, seal);
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->dates_total, 6U);
  EXPECT_EQ(sealed->dates_visible, 4U);
  EXPECT_EQ(sealed->dates_at_or_after_sealed, 2U);
  EXPECT_EQ(sealed->dates_at_or_after_validation, 2U);
}

// The validation boundary is counted independently of the sealed one, and both
// counts are recorded even when the seal does not trip.
TEST(EvalCrossSectionIc, Seal_DatesInValidationBandOnly_CountsValidationNotSealed) {
  std::vector<atx::i64> keys = seal_keys(3U, false);
  keys.push_back(kValidationBeginNs);
  keys.push_back(kValidationBeginNs + kDayNs);
  const CalendarSeal seal{kValidationBeginNs, kSealedBeginNs, SealPolicy::RejectSealedV1};

  auto sealed = apply_calendar_seal(keys, seal);
  ASSERT_TRUE(sealed.has_value());
  EXPECT_EQ(sealed->dates_at_or_after_validation, 2U);
  EXPECT_EQ(sealed->dates_at_or_after_sealed, 0U);
  EXPECT_EQ(sealed->dates_visible, 5U);
}

TEST(EvalCrossSectionIc, Seal_NonMonotonicSessionKeys_ReturnsInvalidArgument) {
  std::vector<atx::i64> keys = seal_keys(5U, false);
  keys[3] = keys[2];
  const CalendarSeal seal{kValidationBeginNs, kSealedBeginNs, SealPolicy::RejectSealedV1};

  auto sealed = apply_calendar_seal(keys, seal);
  ASSERT_FALSE(sealed.has_value());
  EXPECT_EQ(sealed.error().code(), ErrorCode::InvalidArgument);
}

// ===========================================================================
//  T2/T3 — shared fixtures.
//
//  Every panel below mirrors a fixture of the independent exact oracle
//  `build-equity/audits/iteration14_cross_section_oracle.py`, so the same
//  numbers are asserted here by hand AND compared against the oracle through
//  the §9.2 export at the bottom of this file. Nothing here reads a file, a
//  clock or an RNG outside the seeded bootstrap.
// ===========================================================================

constexpr atx::i64 ns_of_epoch_day(atx::i64 epoch_day) { return epoch_day * kNanosPerDay; }

// Epoch day numbers, cross-checked against the header's own boundary constants
// in `Fixtures_SessionKeyDayNumbers_MatchTheCalendar` below.
constexpr atx::i64 kApr01 = 15796; // Mon 2013-04-01
constexpr atx::i64 kApr05 = 15800; // Fri 2013-04-05
constexpr atx::i64 kApr08 = 15803; // Mon 2013-04-08 (Fri -> Mon is 3 calendar days)
constexpr atx::i64 kApr09 = 15804; // Tue 2013-04-09
constexpr atx::i64 kApr10 = 15805; // Wed 2013-04-10
constexpr atx::i64 kMay24 = 15849; // Fri 2013-05-24
constexpr atx::i64 kMay28 = 15853; // Tue 2013-05-28 (Memorial Day weekend: 4 days)

[[nodiscard]] CrossSectionIcResult run_or_die(const CrossSectionIcInput &in,
                                              const CrossSectionIcConfig &cfg) {
  auto planned = plan_cross_section_ic(in, cfg);
  EXPECT_TRUE(planned.has_value());
  if (!planned.has_value()) {
    return CrossSectionIcResult{};
  }
  auto computed = compute_cross_section_ic(in, cfg, *planned);
  EXPECT_TRUE(computed.has_value());
  if (!computed.has_value()) {
    return CrossSectionIcResult{};
  }
  return *computed;
}

// The oracle's 6 x 8 fully-admitted panel: every cell eligible, finite and
// priced. Row 4's signal is constant, which is the §3.4 `va == 0` branch, and
// row 2 carries ties in both directions.
[[nodiscard]] Panel make_f1_panel() {
  static constexpr atx::f64 kSignal[6][8] = {{8, 7, 6, 5, 4, 3, 2, 1},
                                             {1, 3, 5, 7, 2, 4, 6, 8},
                                             {2, 2, 5, 5, 9, 9, 1, 1},
                                             {4, 1, 7, 3, 8, 6, 2, 5},
                                             {5, 5, 5, 5, 5, 5, 5, 5},
                                             {3, 1, 4, 1, 5, 9, 2, 6}};
  static constexpr atx::f64 kPrice[6][8] = {{100, 50, 200, 25, 80, 40, 160, 10},
                                            {101, 51, 198, 26, 79, 41, 158, 11},
                                            {102, 49, 202, 24, 82, 39, 162, 9},
                                            {103, 52, 196, 27, 78, 42, 164, 12},
                                            {104, 48, 204, 23, 83, 38, 156, 8},
                                            {105, 53, 194, 28, 77, 43, 166, 13}};
  static constexpr atx::i64 kDays[6] = {15796, 15797, 15798, 15799, 15800, 15803};
  Panel p{6U, 8U};
  for (atx::usize t = 0U; t < 6U; ++t) {
    p.session_keys[t] = ns_of_epoch_day(kDays[t]);
    for (atx::usize i = 0U; i < 8U; ++i) {
      p.signal[t * 8U + i] = kSignal[t][i];
      p.price[t * 8U + i] = kPrice[t][i];
      p.raw_price[t * 8U + i] = kPrice[t][i];
    }
  }
  return p;
}

// The oracle's 3 x 8 coverage panel (F4): a NaN signal, absent marks, a negative
// close that is NOT repaired, four members of the audited 34-ID set, and both an
// evidenced and an unevidenced terminal flag.
constexpr atx::f64 kMissingMark = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] Panel make_f4_panel() {
  static constexpr atx::u8 kMask[3][8] = {{1, 1, 1, 1, 1, 1, 0, 1},
                                          {1, 1, 1, 1, 1, 1, 1, 0},
                                          {1, 1, 1, 1, 1, 1, 1, 1}};
  const atx::f64 nan_signal = std::numeric_limits<atx::f64>::quiet_NaN();
  const atx::f64 kSignal[3][8] = {{1, 2, 3, nan_signal, 4, 5, 6, 7},
                                  {1, 2, 3, 8, 4, 5, 6, 7},
                                  {1, 2, 3, 8, 4, 5, 6, 7}};
  const atx::f64 kPrice[3][8] = {
      {100, 50, 200, 25, 80, 40, 160, 10},
      {101, 51, 198, 26, kMissingMark, kMissingMark, 161, kMissingMark},
      {102, -49, kMissingMark, 24, kMissingMark, kMissingMark, 162, 8}};
  static constexpr atx::u8 kTerminal[3][8] = {{0, 0, 1, 0, 1, 1, 0, 0},
                                              {0, 0, 0, 0, 1, 1, 0, 0},
                                              {0, 0, 0, 0, 0, 0, 0, 0}};
  static constexpr atx::u8 kEvidenced[3][8] = {{0, 0, 1, 0, 1, 0, 0, 0},
                                               {0, 0, 0, 0, 1, 0, 0, 0},
                                               {0, 0, 0, 0, 0, 0, 0, 0}};
  // Instrument ids 37648 / 35715 / 39970 / 146189 sit at indices 2..5; they are
  // the fixture's members of the 34 required-mark IDs (§3.8 / AR-7).
  static constexpr atx::u8 kEx34[8] = {0, 0, 1, 1, 1, 1, 0, 0};

  Panel p{3U, 8U};
  for (atx::usize t = 0U; t < 3U; ++t) {
    p.session_keys[t] = ns_of_epoch_day(kApr01 + static_cast<atx::i64>(t));
    for (atx::usize i = 0U; i < 8U; ++i) {
      const atx::usize c = t * 8U + i;
      p.mask[c] = kMask[t][i];
      p.signal[c] = kSignal[t][i];
      p.price[c] = kPrice[t][i];
      p.raw_price[c] = kPrice[t][i];
      p.terminal[c] = kTerminal[t][i];
      p.terminal_evidenced[c] = kEvidenced[t][i];
      // Any finite consideration serves: F4 pins the COUNTERS, and the counter
      // does not depend on the leg's value.
      p.terminal_value[c] = (kTerminal[t][i] != 0U) ? 100.0 : 0.0;
      p.excluded_audited[c] = kEx34[i];
    }
  }
  return p;
}

// The oracle's 3 x 4 net-spread panel (F5): Q = 2, h = 1, session keys Fri/Mon/Tue
// so days_h(t) is 3 then 1, and date 1's signal is date 0's reversed — a COMPLETE
// decile turnover of both legs.
[[nodiscard]] Panel make_f5_panel() {
  static constexpr atx::f64 kSignal[3][4] = {{4, 3, 2, 1}, {1, 2, 3, 4}, {4, 3, 2, 1}};
  static constexpr atx::f64 kPrice[3][4] = {
      {100, 50, 200, 25}, {101, 51, 198, 27}, {102, 52, 196, 28}};
  static constexpr atx::i64 kDays[3] = {kApr05, kApr08, kApr09};
  Panel p{3U, 4U};
  for (atx::usize t = 0U; t < 3U; ++t) {
    p.session_keys[t] = ns_of_epoch_day(kDays[t]);
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[t * 4U + i] = kSignal[t][i];
      p.price[t * 4U + i] = kPrice[t][i];
      p.raw_price[t * 4U + i] = kPrice[t][i];
    }
  }
  return p;
}

// A deterministic, well-conditioned panel long enough for a REPORTABLE series.
// Values are pure functions of (t, i): no RNG, no clock.
[[nodiscard]] Panel make_long_panel(atx::usize dates, atx::usize instruments) {
  Panel p{dates, instruments};
  for (atx::usize t = 0U; t < dates; ++t) {
    p.session_keys[t] = ns_of_epoch_day(kApr01 + static_cast<atx::i64>(t));
    for (atx::usize i = 0U; i < instruments; ++i) {
      const atx::usize c = t * instruments + i;
      p.signal[c] = static_cast<atx::f64>((t * 7U + i * 13U) % 11U) * 0.5 - 2.0;
      const atx::f64 px = 100.0 + static_cast<atx::f64>((t * 3U + i * 5U) % 17U) * 0.25 +
                          static_cast<atx::f64>(t) * 0.125;
      p.price[c] = px;
      p.raw_price[c] = px;
    }
  }
  return p;
}

// A three-name terminal fixture (§3.8): instrument 0 is the subject, 1 and 2 are
// always-priced controls. With Q = 3 each name is its own decile, so decile 0's
// mean forward return IS the subject's forward return — the only way the
// per-date API exposes one cell's return.
[[nodiscard]] Panel make_terminal_panel(atx::usize dates) {
  Panel p{dates, 3U};
  for (atx::usize t = 0U; t < dates; ++t) {
    p.session_keys[t] = ns_of_epoch_day(kApr01 + static_cast<atx::i64>(t));
    p.signal[t * 3U + 0U] = 10.0; // the subject always ranks first
    p.signal[t * 3U + 1U] = 2.0;
    p.signal[t * 3U + 2U] = 1.0;
    const atx::f64 c1 = 100.0 + static_cast<atx::f64>(t) * 5.0;
    const atx::f64 c2 = 200.0 + static_cast<atx::f64>(t) * 7.0;
    p.price[t * 3U + 1U] = c1;
    p.raw_price[t * 3U + 1U] = c1;
    p.price[t * 3U + 2U] = c2;
    p.raw_price[t * 3U + 2U] = c2;
  }
  return p;
}

// Set the subject cell (instrument 0) at row `t`.
void set_subject(Panel &p, atx::usize t, atx::f64 price, atx::f64 raw, atx::u8 terminal,
                 atx::u8 evidenced, atx::f64 terminal_value) {
  const atx::usize c = t * p.instruments;
  p.price[c] = price;
  p.raw_price[c] = raw;
  p.terminal[c] = terminal;
  p.terminal_evidenced[c] = evidenced;
  p.terminal_value[c] = terminal_value;
}

[[nodiscard]] CrossSectionIcConfig terminal_config(std::span<const atx::usize> horizons) {
  CrossSectionIcConfig cfg = good_config(horizons);
  cfg.quantiles = 3U;
  cfg.forward_variant = ForwardReturnVariant::IncludeAuditedTerminalV1;
  return cfg;
}

TEST(EvalCrossSectionIc, Fixtures_SessionKeyDayNumbers_MatchTheCalendar) {
  // The fixtures are written as epoch DAY numbers; the header owns the one
  // nanosecond spelling. Anchor them against T1's already-pinned 2013-04-04.
  EXPECT_EQ(ns_of_epoch_day(15799), k2013Apr04Ns);
  EXPECT_EQ(kApr08 - kApr05, atx::i64{3});  // Friday -> Monday
  EXPECT_EQ(kApr10 - kApr09, atx::i64{1});  // Tuesday -> Wednesday
  EXPECT_EQ(kMay28 - kMay24, atx::i64{4});  // Memorial Day weekend
  EXPECT_EQ(kApr08 - kApr01, atx::i64{7});  // five sessions, seven calendar days
}

// ===========================================================================
//  §3.4 / §3.5 — Pearson, rank IC and the two degenerate branches.
// ===========================================================================

// Case 1. r is a strictly increasing affine image of x, built through the PANEL
// so the whole §3.3 -> §3.4 -> §3.5 path is exercised, not just the primitive:
// P(t) == 1 and P(t+1) == 1 + r makes the forward return exactly r.
TEST(EvalCrossSectionIc, PearsonIc_PerfectlyOrderedCrossSection_ReturnsOne) {
  Panel p{2U, 4U};
  for (atx::usize i = 0U; i < 4U; ++i) {
    const atx::f64 x = static_cast<atx::f64>(i) + 1.0;
    const atx::f64 r = 2.0 * x + 1.0; // {3, 5, 7, 9}
    p.signal[i] = x;
    p.price[i] = 1.0;
    p.raw_price[i] = 1.0;
    p.price[4U + i] = 1.0 + r;
    p.raw_price[4U + i] = 1.0 + r;
  }
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  ASSERT_EQ(out.horizons.size(), 1U);
  ASSERT_EQ(out.horizons[0].series.size(), 1U);
  const IcDatePoint &pt = out.horizons[0].series[0];
  EXPECT_EQ(pt.emitted, atx::u8{1});
  EXPECT_NEAR(pt.pearson_ic, 1.0, 1e-15);
  EXPECT_EQ(pt.rank_ic, 1.0); // ranks of two strictly monotone vectors are equal
}

// Case 2b — the T2 acceptance check (§8): the emitted per-date IC series is
// element-wise BIT-IDENTICAL to what `learn::detail::oof_ic_series`
// (`linear_alpha.cpp:119-147`) produces from the same cross-sections. Same `>= 2`
// gate, same ascending single walk, same constant-series-to-zero convention.
//
// The shared fixture is the F1 panel: the FeatureMatrix carries one row per
// (date, instrument) in the same ascending order the engine gathers in, the OOF
// prediction is the signal (cnt == 1, so `oof_sum / oof_cnt` is exact) and the
// label is the §3.3 forward return.
TEST(EvalCrossSectionIc, PearsonIc_MatchesOofIcSeriesSemantics_OnEquivalentInput) {
  const Panel p = make_f1_panel();
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);
  ASSERT_EQ(out.horizons.size(), 1U);

  atx::engine::learn::FeatureMatrix fm;
  fm.n_dates = p.dates;
  fm.n_instruments = p.instruments;
  fm.n_features = 1U;
  std::vector<atx::f64> oof_sum;
  std::vector<atx::u32> oof_cnt;
  std::vector<atx::f64> labels;
  for (atx::usize t = 0U; t + 1U < p.dates; ++t) {
    for (atx::usize i = 0U; i < p.instruments; ++i) {
      const atx::usize c = t * p.instruments + i;
      static_cast<void>(fm.push_row(t, i));
      fm.X.push_back(p.signal[c]);
      fm.row_valid.push_back(atx::u8{1});
      oof_sum.push_back(p.signal[c]);
      oof_cnt.push_back(atx::u32{1});
      labels.push_back(p.price[(t + 1U) * p.instruments + i] / p.price[c] - 1.0);
    }
  }
  fm.Y.assign(1U, labels);

  const std::vector<atx::f64> reference = atx::engine::learn::detail::oof_ic_series(
      fm, std::span<const atx::f64>{oof_sum}, std::span<const atx::u32>{oof_cnt});

  std::vector<atx::f64> emitted;
  for (const IcDatePoint &pt : out.horizons[0].series) {
    if (pt.emitted != 0U) {
      emitted.push_back(pt.pearson_ic);
    }
  }
  ASSERT_EQ(emitted.size(), reference.size());
  ASSERT_EQ(emitted.size(), 5U);
  for (atx::usize k = 0U; k < emitted.size(); ++k) {
    EXPECT_EQ(emitted[k], reference[k]) << "series point " << k;
  }
  // Row 4's signal is constant, so that point is the shared constant-series
  // convention rather than an accident of agreement.
  EXPECT_EQ(emitted[4], 0.0);
}

// Case 3 — the va == 0 branch: a constant cross-section contributes 0.0, never a
// 0/0 NaN that would poison every downstream mean.
TEST(EvalCrossSectionIc, PearsonIc_ConstantSignalCrossSection_ReturnsZeroNotNan) {
  Panel p{2U, 4U};
  static constexpr atx::f64 kForward[4] = {0.01, 0.02, -0.01, 0.04};
  for (atx::usize i = 0U; i < 4U; ++i) {
    p.signal[i] = 3.0;
    p.price[i] = 100.0;
    p.raw_price[i] = 100.0;
    p.price[4U + i] = 100.0 * (1.0 + kForward[i]);
    p.raw_price[4U + i] = p.price[4U + i];
  }
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  ASSERT_EQ(out.horizons[0].series.size(), 1U);
  EXPECT_EQ(out.horizons[0].series[0].emitted, atx::u8{1});
  EXPECT_EQ(out.horizons[0].series[0].pearson_ic, 0.0);
  EXPECT_FALSE(std::isnan(out.horizons[0].series[0].pearson_ic));
  EXPECT_EQ(out.horizons[0].series[0].rank_ic, 0.0);
}

// Case 3b — the n < 2U early return, at the primitive.
TEST(EvalCrossSectionIc, Pearson_FewerThanTwoNames_ReturnsZeroNotNan) {
  const std::vector<atx::f64> one{7.0};
  const std::vector<atx::f64> lab{0.1};
  EXPECT_EQ(detail::pearson(one, lab), 0.0);
  const std::vector<atx::f64> none{};
  EXPECT_EQ(detail::pearson(none, none), 0.0);
}

// Case 4 — averaged ranks (`cross_section.hpp:123-136`): a run of bit-equal
// values shares the mean of the positions it occupies.
TEST(EvalCrossSectionIc, RankIc_TiedSignalValues_AveragesRanks) {
  const std::vector<atx::f64> tied{1.0, 1.0, 2.0, 2.0};
  std::vector<atx::f64> ranks(4U, 0.0);
  std::vector<atx::usize> order(4U, 0U);
  atx::core::stats::rank(tied, std::span<atx::f64>{ranks}, std::span<atx::usize>{order});
  EXPECT_DOUBLE_EQ(ranks[0], 0.5 / 3.0);
  EXPECT_DOUBLE_EQ(ranks[1], 0.5 / 3.0);
  EXPECT_DOUBLE_EQ(ranks[2], 2.5 / 3.0);
  EXPECT_DOUBLE_EQ(ranks[3], 2.5 / 3.0);
}

// Case 5 — equality inside rank() is BITWISE on f64, not a tolerance: two values
// one ulp apart are two ranks, not a tie.
TEST(EvalCrossSectionIc, RankIc_OneUlpApart_AreDistinctRanks) {
  const atx::f64 up = std::nextafter(1.0, 2.0);
  ASSERT_NE(up, 1.0);
  const std::vector<atx::f64> values{1.0, up, 2.0, 3.0};
  std::vector<atx::f64> ranks(4U, 0.0);
  std::vector<atx::usize> order(4U, 0U);
  atx::core::stats::rank(values, std::span<atx::f64>{ranks}, std::span<atx::usize>{order});
  EXPECT_EQ(ranks[0], 0.0);
  EXPECT_DOUBLE_EQ(ranks[1], 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(ranks[2], 2.0 / 3.0);
  EXPECT_EQ(ranks[3], 1.0);
  EXPECT_NE(ranks[0], ranks[1]);
}

// ===========================================================================
//  §3.3 / §3.7 / §3.8 — forward returns, coverage counters, terminal legs.
// ===========================================================================

// Case 6 — a date below `min_names_per_date` is NOT emitted and IS counted.
TEST(EvalCrossSectionIc, Date_WithFewerThanMinNames_IsNotEmittedAndIsCounted) {
  Panel p{2U, 2U};
  p.mask[1] = atx::u8{0}; // one admitted name at date 0
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 2U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  ASSERT_EQ(out.horizons[0].series.size(), 1U);
  EXPECT_EQ(out.horizons[0].series[0].emitted, atx::u8{0});
  EXPECT_EQ(out.horizons[0].series[0].n_used, 1U);
  EXPECT_EQ(out.horizons[0].series[0].pearson_ic, 0.0); // untouched, must be ignored
  EXPECT_EQ(out.horizons[0].dates_below_min_names, 1U);
}

// Case 7 — variant A drops a cell with no mark at t+h and counts it. This is the
// Shumway selection the design reports rather than corrects.
TEST(EvalCrossSectionIc, ForwardReturn_MissingTerminalMark_DropVariantDropsAndCounts) {
  Panel p = make_terminal_panel(2U);
  set_subject(p, 0U, 50.0, 50.0, atx::u8{1}, atx::u8{1}, 55.0);
  p.price[1U * 3U] = kMissingMark;
  p.raw_price[1U * 3U] = kMissingMark;
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = terminal_config(h);
  cfg.forward_variant = ForwardReturnVariant::DropMissingForward;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  const IcDatePoint &pt = out.horizons[0].series[0];
  EXPECT_EQ(pt.n_signal_finite, 3U);
  EXPECT_EQ(pt.n_with_forward, 2U);
  EXPECT_EQ(pt.n_dropped_missing_forward, 1U);
  EXPECT_EQ(pt.n_terminal_applied, 0U); // variant A never applies a leg
  EXPECT_EQ(pt.n_used, 2U);
}

// Case 8 — the evidenced HNZ shape: the declared leg is applied against the RAW
// close and the return matches the §3.8 formula to the ulp.
TEST(EvalCrossSectionIc, ForwardReturn_AuditedEvidencedTerminal_AppliesDeclaredLeg) {
  Panel p = make_terminal_panel(3U);
  set_subject(p, 0U, 72.00, 72.00, atx::u8{1}, atx::u8{1}, 72.50);
  set_subject(p, 1U, 72.49, 72.49, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  const std::vector<atx::usize> h{2U};
  const CrossSectionIcConfig cfg = terminal_config(h);
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  const IcHorizonSummary &sum = out.horizons[0];
  ASSERT_EQ(sum.series.size(), 1U);
  EXPECT_EQ(sum.series[0].n_terminal_applied, 1U);
  EXPECT_EQ(sum.series[0].n_used, 3U);
  // t_last is resolved inside (t, t+h] only: offset 1, not the absent offset 2.
  const atx::f64 leg = 72.50 / 72.49 - 1.0;
  const atx::f64 expected = (72.49 / 72.00) * (1.0 + leg) - 1.0;
  ASSERT_EQ(sum.buckets.size(), 3U);
  EXPECT_DOUBLE_EQ(sum.buckets[0].mean_forward_return, expected);
  EXPECT_NEAR(expected, 1.0 / 144.0, 1e-15);
}

// Case 8a — ruling AR-2, the inclusive side of the record-date boundary. The
// CALLER gates the special dividend (§6: `terminal_value` arrives "already gated
// by the caller on record date"), so the fixture supplies 13.75 + 0.13 for an
// observation at 2013-10-28 and this case pins the leg the engine then forms.
TEST(EvalCrossSectionIc, TerminalLeg_ObservationAtOrBeforeRecordDate_IncludesSpecialDividend) {
  Panel p = make_terminal_panel(3U);
  p.session_keys[0] = ns_of_epoch_day(16006); // 2013-10-28, the record date itself
  p.session_keys[1] = ns_of_epoch_day(16007);
  p.session_keys[2] = ns_of_epoch_day(16008);
  set_subject(p, 0U, 13.90, 13.90, atx::u8{1}, atx::u8{1}, 13.75 + 0.13);
  set_subject(p, 1U, 13.86, 13.86, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  const std::vector<atx::usize> h{2U};
  const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));

  const atx::f64 leg = (13.75 + 0.13) / 13.86 - 1.0;
  const atx::f64 expected = (13.86 / 13.90) * (1.0 + leg) - 1.0;
  EXPECT_EQ(out.horizons[0].series[0].n_terminal_applied, 1U);
  EXPECT_DOUBLE_EQ(out.horizons[0].buckets[0].mean_forward_return, expected);
}

// Case 8b — the exclusive side of the same boundary, one session later: the
// consideration alone applies and the leg is strictly smaller.
TEST(EvalCrossSectionIc, TerminalLeg_ObservationAfterRecordDate_ExcludesSpecialDividend) {
  Panel p = make_terminal_panel(3U);
  p.session_keys[0] = ns_of_epoch_day(16007); // 2013-10-29, after the record date
  p.session_keys[1] = ns_of_epoch_day(16008);
  p.session_keys[2] = ns_of_epoch_day(16009);
  set_subject(p, 0U, 13.90, 13.90, atx::u8{1}, atx::u8{1}, 13.75);
  set_subject(p, 1U, 13.86, 13.86, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  const std::vector<atx::usize> h{2U};
  const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));

  const atx::f64 leg = 13.75 / 13.86 - 1.0;
  const atx::f64 expected = (13.86 / 13.90) * (1.0 + leg) - 1.0;
  EXPECT_EQ(out.horizons[0].series[0].n_terminal_applied, 1U);
  EXPECT_DOUBLE_EQ(out.horizons[0].buckets[0].mean_forward_return, expected);
  EXPECT_LT(leg, (13.75 + 0.13) / 13.86 - 1.0);
}

// Case 8c — ruling A-6, the precedence rule. A terminal-flagged, EVIDENCED ID
// that still has a finite positive P(t+h) takes the ORDINARY return; no leg is
// applied and `n_terminal_applied` does not move. Read literally the §3.8 formula
// would set t_last == t+h and add a settlement on top of a live price.
TEST(EvalCrossSectionIc, TerminalLeg_LiveCloseAtTPlusH_UsesOrdinaryReturnAndAppliesNoLeg) {
  Panel p = make_terminal_panel(3U);
  set_subject(p, 0U, 13.90, 13.90, atx::u8{1}, atx::u8{1}, 13.88);
  set_subject(p, 1U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 2U, 13.86, 13.86, atx::u8{0}, atx::u8{0}, 0.0); // a LIVE close at t+h
  const std::vector<atx::usize> h{2U};
  const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));

  const IcDatePoint &pt = out.horizons[0].series[0];
  EXPECT_EQ(pt.n_terminal_applied, 0U);
  EXPECT_EQ(pt.n_with_forward, 3U);
  EXPECT_EQ(pt.n_used, 3U);
  EXPECT_DOUBLE_EQ(out.horizons[0].buckets[0].mean_forward_return, 13.86 / 13.90 - 1.0);
  // The consideration is NOT added: the overlay reading would give a strictly
  // larger return, and this is the number that separates the two readings.
  const atx::f64 overlay = (13.86 / 13.90) * (1.0 + (13.88 / 13.86 - 1.0)) - 1.0;
  EXPECT_NE(out.horizons[0].buckets[0].mean_forward_return, overlay);
}

// Case 8d — t_last is searched in (t, t+h] only (M-8). With no valid mark there
// the cell has no terminal path and drops as an ordinary missing forward return,
// even though a later mark exists outside the window.
TEST(EvalCrossSectionIc, TerminalLeg_NoValidMarkInsideForwardWindow_DropsAsMissing) {
  Panel p = make_terminal_panel(4U);
  set_subject(p, 0U, 72.00, 72.00, atx::u8{1}, atx::u8{1}, 72.50);
  set_subject(p, 1U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 3U, 72.49, 72.49, atx::u8{0}, atx::u8{0}, 0.0); // OUTSIDE (0, 2]
  const std::vector<atx::usize> h{2U};
  const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));

  const IcDatePoint &pt = out.horizons[0].series[0];
  EXPECT_EQ(pt.n_terminal_applied, 0U);
  EXPECT_EQ(pt.n_dropped_missing_forward, 1U);
  EXPECT_EQ(pt.n_used, 2U);
  EXPECT_EQ(pt.spread_emitted, atx::u8{0}); // 2 < Q == 3
}

// Case 9 — the PCS shape (ruling AR-1 / R-A): flagged terminal, no evidenced
// consideration. The cell drops, `n_terminal_unevidenced` counts it, and NO
// return is invented.
TEST(EvalCrossSectionIc, ForwardReturn_AuditedUnevidencedTerminal_DropsAndCountsSeparately) {
  Panel p = make_terminal_panel(3U);
  set_subject(p, 0U, 11.00, 11.00, atx::u8{1}, atx::u8{0}, 0.0);
  set_subject(p, 1U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
  const std::vector<atx::usize> h{2U};
  const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));

  const IcDatePoint &pt = out.horizons[0].series[0];
  EXPECT_EQ(pt.n_terminal_unevidenced, 1U);
  EXPECT_EQ(pt.n_terminal_applied, 0U);
  EXPECT_EQ(pt.n_used, 2U);
}

// Case 10 — a zero or negative close is a MISSING mark, never repaired, at either
// endpoint of the return.
TEST(EvalCrossSectionIc, ForwardReturn_ZeroOrNegativePrice_IsMissingNotRepaired) {
  {
    Panel p = make_terminal_panel(2U);
    set_subject(p, 0U, 0.0, 0.0, atx::u8{0}, atx::u8{0}, 0.0); // zero at t
    set_subject(p, 1U, 50.0, 50.0, atx::u8{0}, atx::u8{0}, 0.0);
    const std::vector<atx::usize> h{1U};
    const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));
    EXPECT_EQ(out.horizons[0].series[0].n_with_forward, 2U);
    EXPECT_EQ(out.horizons[0].series[0].n_dropped_missing_forward, 1U);
  }
  {
    Panel p = make_terminal_panel(2U);
    set_subject(p, 0U, 50.0, 50.0, atx::u8{0}, atx::u8{0}, 0.0);
    set_subject(p, 1U, -49.0, -49.0, atx::u8{0}, atx::u8{0}, 0.0); // negative at t+h
    const std::vector<atx::usize> h{1U};
    const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));
    EXPECT_EQ(out.horizons[0].series[0].n_with_forward, 2U);
    EXPECT_EQ(out.horizons[0].series[0].n_dropped_missing_forward, 1U);
  }
}

// Case 11 — §3.3's no-forward-eligibility rule. Conditioning date t's
// cross-section on t+h universe membership would be lookahead of exactly the
// class `report-holding-period-audit.md:59-67` documents.
TEST(EvalCrossSectionIc, Admission_NameEligibleAtTButNotAtTPlusH_IsStillUsed) {
  Panel p = make_terminal_panel(2U);
  p.mask[1U * 3U] = atx::u8{0}; // subject is out of the universe at t+h
  const std::vector<atx::usize> h{1U};
  const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h));

  const IcDatePoint &pt = out.horizons[0].series[0];
  EXPECT_EQ(pt.n_eligible, 3U);
  EXPECT_EQ(pt.n_with_forward, 3U);
  EXPECT_EQ(pt.n_used, 3U);
}

// ===========================================================================
//  Purity, the scratch contract and the summary branches.
// ===========================================================================

// Case 12 — scratch re-use changes nothing; compute is pure in (in, cfg).
TEST(EvalCrossSectionIc, Compute_ReusedScratch_IsBitIdenticalToFreshScratch) {
  const Panel p = make_f1_panel();
  const std::vector<atx::usize> h{1U, 5U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;

  auto shared = plan_cross_section_ic(p.view(), cfg);
  ASSERT_TRUE(shared.has_value());
  auto first = compute_cross_section_ic(p.view(), cfg, *shared);
  ASSERT_TRUE(first.has_value());
  auto second = compute_cross_section_ic(p.view(), cfg, *shared); // SAME, dirty scratch
  ASSERT_TRUE(second.has_value());
  auto other = plan_cross_section_ic(p.view(), cfg);
  ASSERT_TRUE(other.has_value());
  auto fresh = compute_cross_section_ic(p.view(), cfg, *other);
  ASSERT_TRUE(fresh.has_value());

  ASSERT_EQ(first->horizons.size(), second->horizons.size());
  ASSERT_EQ(first->horizons.size(), fresh->horizons.size());
  for (atx::usize k = 0U; k < first->horizons.size(); ++k) {
    ASSERT_EQ(first->horizons[k].series.size(), second->horizons[k].series.size());
    for (atx::usize t = 0U; t < first->horizons[k].series.size(); ++t) {
      EXPECT_EQ(first->horizons[k].series[t].pearson_ic, second->horizons[k].series[t].pearson_ic);
      EXPECT_EQ(first->horizons[k].series[t].rank_ic, fresh->horizons[k].series[t].rank_ic);
      EXPECT_EQ(first->horizons[k].series[t].spread_net, fresh->horizons[k].series[t].spread_net);
    }
    EXPECT_EQ(first->horizons[k].full.ic_mean, fresh->horizons[k].full.ic_mean);
    EXPECT_EQ(first->horizons[k].decile_one_way_turnover,
              fresh->horizons[k].decile_one_way_turnover);
  }
  EXPECT_EQ(first->autocorr.rho_rank, fresh->autocorr.rho_rank);
}

// Case 28 — an undersized scratch is a caller-contract violation, reported as
// `Internal` rather than read out of bounds.
TEST(EvalCrossSectionIc, Compute_UndersizedScratch_ReturnsInternal) {
  const Panel p = make_f1_panel();
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;

  auto planned = plan_cross_section_ic(p.view(), cfg);
  ASSERT_TRUE(planned.has_value());
  CrossSectionIcScratch shrunk = *planned;
  shrunk.rx.pop_back();
  auto computed = compute_cross_section_ic(p.view(), cfg, shrunk);
  ASSERT_FALSE(computed.has_value());
  EXPECT_EQ(computed.error().code(), ErrorCode::Internal);

  CrossSectionIcScratch shrunk_draw = *planned;
  shrunk_draw.draw.pop_back();
  auto second = compute_cross_section_ic(p.view(), cfg, shrunk_draw);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code(), ErrorCode::Internal);
}

// Case 29 — n == 1 and n == 0 are DEFINED, never NaN (I-11): the (n-1) divisor is
// undefined at n == 1 and the `sd == 0.0` test would not catch the resulting NaN.
TEST(EvalCrossSectionIc, Summary_SinglePointSeries_IsUnreportableNotNan) {
  {
    Panel p{2U, 4U};
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[i] = static_cast<atx::f64>(i);
      p.price[4U + i] = 100.0 + static_cast<atx::f64>(i);
      p.raw_price[4U + i] = p.price[4U + i];
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const IcSampleStats &s = out.horizons[0].full;
    EXPECT_EQ(s.dates_emitted, 1U);
    EXPECT_EQ(s.summary_reportable, atx::u8{0});
    EXPECT_EQ(s.unreportable_reason, atx::u8{1});
    EXPECT_EQ(s.ic_mean, out.horizons[0].series[0].pearson_ic); // ic_mean == IC_0
    EXPECT_EQ(s.ic_sd, 0.0);
    EXPECT_EQ(s.icir, 0.0);
    EXPECT_EQ(s.naive_t, 0.0);
    EXPECT_FALSE(std::isnan(s.ic_sd));
    EXPECT_FALSE(std::isnan(s.icir));
  }
  {
    // Companion n == 0: the single evaluable date does not emit at all.
    Panel p{2U, 2U};
    p.mask[1] = atx::u8{0};
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const IcSampleStats &s = out.horizons[0].full;
    EXPECT_EQ(s.dates_emitted, 0U);
    EXPECT_EQ(s.summary_reportable, atx::u8{0});
    EXPECT_EQ(s.unreportable_reason, atx::u8{1});
    EXPECT_EQ(s.ic_mean, 0.0);
    EXPECT_EQ(s.ic_sd, 0.0);
    EXPECT_FALSE(std::isnan(s.ic_mean));
  }
}

// ===========================================================================
//  §3.12 — quantile assignment, gross and net spread, turnover, borrow days.
// ===========================================================================

// Case 16 — n == Q: q = floor(p*Q/n) == p, exactly one name per bucket.
TEST(EvalCrossSectionIc, QuantileSpread_ExactlyQNames_AssignsOnePerBucket) {
  const Panel f1 = make_f1_panel();
  Panel p{2U, 4U};
  for (atx::usize t = 0U; t < 2U; ++t) {
    p.session_keys[t] = f1.session_keys[t];
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[t * 4U + i] = f1.signal[t * 8U + i];
      p.price[t * 4U + i] = f1.price[t * 8U + i];
      p.raw_price[t * 4U + i] = f1.price[t * 8U + i];
    }
  }
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  ASSERT_EQ(out.horizons[0].buckets.size(), 4U);
  for (atx::usize q = 0U; q < 4U; ++q) {
    EXPECT_EQ(out.horizons[0].buckets[q].quantile, q);
    EXPECT_EQ(out.horizons[0].buckets[q].n_dates, 1U);
    EXPECT_DOUBLE_EQ(out.horizons[0].buckets[q].mean_names, 1.0);
  }
  // Signals 8 > 7 > 6 > 5, so decile 0 is instrument 0 and decile 3 instrument 3.
  EXPECT_EQ(out.horizons[0].series[0].spread_emitted, atx::u8{1});
  EXPECT_DOUBLE_EQ(out.horizons[0].series[0].spread_gross, (101.0 / 100.0) - (26.0 / 25.0));
}

// Case 17 — fewer than Q names: the date is rejected for spread purposes and
// counted, while the IC (which needs only `min_names_per_date`) still emits.
TEST(EvalCrossSectionIc, QuantileSpread_FewerThanQNames_EmitsNoSpreadAndCounts) {
  const Panel f1 = make_f1_panel();
  Panel p{2U, 3U};
  for (atx::usize t = 0U; t < 2U; ++t) {
    p.session_keys[t] = f1.session_keys[t];
    for (atx::usize i = 0U; i < 3U; ++i) {
      p.signal[t * 3U + i] = f1.signal[t * 8U + i];
      p.price[t * 3U + i] = f1.price[t * 8U + i];
      p.raw_price[t * 3U + i] = f1.price[t * 8U + i];
    }
  }
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  EXPECT_EQ(out.horizons[0].series[0].emitted, atx::u8{1});
  EXPECT_EQ(out.horizons[0].series[0].spread_emitted, atx::u8{0});
  EXPECT_EQ(out.horizons[0].series[0].spread_gross, 0.0); // untouched, must be ignored
  EXPECT_EQ(out.horizons[0].dates_below_quantile_count, 1U);

  // Parent ruling I-6 / design §11.8: the spread family carries its OWN
  // reportability. Here the IC family emits (n_used == 3 >= min_names) while the
  // spread series is empty, so `summary_reportable` says nothing useful about the
  // spread and the two new fields say it explicitly.
  EXPECT_EQ(out.horizons[0].full.summary_reportable, atx::u8{0}); // one IC date
  EXPECT_EQ(out.horizons[0].spread_reportable, atx::u8{0});
  EXPECT_EQ(out.horizons[0].spread_unreportable_reason, atx::u8{1}); // n_spread == 0 < 20
  // The means are 0.0 because the gate closed, not because the spread was zero.
  EXPECT_EQ(out.horizons[0].full.spread_gross_mean, 0.0);
  EXPECT_EQ(out.horizons[0].full.spread_net_mean, 0.0);
  EXPECT_EQ(out.horizons[0].full.spread_gross_ci.series_len, 0U);
}

// Case 18 — §3.12 step 2's total order is (signal DESC, instrument index ASC).
//
// Two fixtures. The first has both tied pairs falling ENTIRELY inside one bucket,
// which pins the membership but NOT the tie-break direction (finding I-2: with
// `{3,5,5,1,4,4,2,0}` at Q = 4 the ties occupy positions 0-1 and 2-3, so
// reversing the comparator permutes names within a bucket and changes nothing).
// The second puts a tie ACROSS a bucket boundary, where the direction decides
// which decile a name lands in — and therefore the spread.
TEST(EvalCrossSectionIc, QuantileSpread_TiedSignal_BreaksByAscendingInstrumentIndex) {
  const Panel f1 = make_f1_panel();
  const auto with_signals = [&f1](const atx::f64 (&signal)[8]) {
    Panel p{2U, 8U};
    for (atx::usize t = 0U; t < 2U; ++t) {
      p.session_keys[t] = f1.session_keys[t];
      for (atx::usize i = 0U; i < 8U; ++i) {
        p.signal[t * 8U + i] = signal[i];
        p.price[t * 8U + i] = f1.price[t * 8U + i];
        p.raw_price[t * 8U + i] = f1.price[t * 8U + i];
      }
    }
    return p;
  };
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;

  {
    static constexpr atx::f64 kTied[8] = {3, 5, 5, 1, 4, 4, 2, 0};
    const Panel p = with_signals(kTied);
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    // Ordered: 1(5), 2(5), 4(4), 5(4), 0(3), 6(2), 3(1), 7(0); buckets of two, so
    // decile 0 is {1, 2} and decile 3 is {3, 7}.
    const atx::f64 top = ((51.0 / 50.0 - 1.0) + (198.0 / 200.0 - 1.0)) / 2.0;
    const atx::f64 bottom = ((26.0 / 25.0 - 1.0) + (11.0 / 10.0 - 1.0)) / 2.0;
    EXPECT_EQ(out.horizons[0].series[0].spread_emitted, atx::u8{1});
    EXPECT_DOUBLE_EQ(out.horizons[0].series[0].spread_gross, top - bottom);
  }
  {
    // I-2: the tied 5s at instruments 1 and 2 land at ordered positions 1 and 2,
    // which `q = floor(p * 4 / 8)` splits across deciles 0 and 1. ASCENDING index
    // puts instrument 1 in the top decile; inverting the tie-break puts
    // instrument 2 there instead, and the top-decile mean moves from
    // (0.01 + 0.02)/2 = 0.015 to (0.01 + -0.01)/2 = 0.0.
    static constexpr atx::f64 kStraddle[8] = {9, 5, 5, 4, 3, 2, 1, 0};
    const Panel p = with_signals(kStraddle);
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const atx::f64 r0 = 101.0 / 100.0 - 1.0; // instrument 0
    const atx::f64 r1 = 51.0 / 50.0 - 1.0;   // instrument 1 — ascending tie-break
    const atx::f64 r2 = 198.0 / 200.0 - 1.0; // instrument 2 — the inverted reading
    const atx::f64 bottom = ((11.0 / 10.0 - 1.0) + (158.0 / 160.0 - 1.0)) / 2.0;
    ASSERT_EQ(out.horizons[0].buckets.size(), 4U);
    EXPECT_DOUBLE_EQ(out.horizons[0].buckets[0].mean_forward_return, (r0 + r1) / 2.0);
    EXPECT_NE(out.horizons[0].buckets[0].mean_forward_return, (r0 + r2) / 2.0);
    EXPECT_DOUBLE_EQ(out.horizons[0].series[0].spread_gross, (r0 + r1) / 2.0 - bottom);
  }
}

// Case 19b — ruling AR-11's scale check, and the one case that would have caught
// the gross-1.0 / gross-2.0 mismatch. Weights are +/-1.0/n, so a COMPLETE decile
// turnover gives sum|dw| == 4, oneway == 2 and a 20 bps trade drag.
TEST(EvalCrossSectionIc, QuantileSpread_CompleteDecileTurnover_CostsTwentyBps) {
  {
    const Panel p = make_f5_panel(); // date 1's signal reverses date 0's
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    EXPECT_DOUBLE_EQ(out.horizons[0].decile_one_way_turnover, 2.0);
    EXPECT_DOUBLE_EQ(out.horizons[0].decile_one_way_turnover * 2.0, 4.0); // sum|dw|
    EXPECT_DOUBLE_EQ(out.horizons[0].trade_drag, 0.0020);                 // 20 bps
  }
  {
    // Half turnover: one name retained in each leg -> sum|dw| == 2, 10 bps.
    Panel p = make_f5_panel();
    static constexpr atx::f64 kHalf[4] = {4, 1, 3, 0};
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[4U + i] = kHalf[i];
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    EXPECT_DOUBLE_EQ(out.horizons[0].decile_one_way_turnover, 1.0);
    EXPECT_DOUBLE_EQ(out.horizons[0].trade_drag, 0.0010); // 10 bps
  }
}

// Case 19 — ruling AR-3: the net series is the gross series minus a PER-DATE
// drag, on exactly the same dates. No rebalance grid, no lost interval.
TEST(EvalCrossSectionIc, QuantileSpread_NetIsGrossMinusPerDateDrag) {
  const Panel p = make_f5_panel();
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 2U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);
  const IcHorizonSummary &sum = out.horizons[0];

  ASSERT_EQ(sum.series.size(), 2U);
  EXPECT_DOUBLE_EQ(sum.trade_drag, (5.0 / 1e4) * 2.0 * sum.decile_one_way_turnover);
  atx::usize emitted = 0U;
  for (const IcDatePoint &pt : sum.series) {
    const atx::f64 borrow =
        (365.0 / 1e4) * (static_cast<atx::f64>(pt.days_forward) / 365.0) * 1.0;
    EXPECT_DOUBLE_EQ(pt.borrow_drag, borrow);
    EXPECT_DOUBLE_EQ(pt.cost_drag, sum.trade_drag + pt.borrow_drag);
    if (pt.spread_emitted != 0U) {
      EXPECT_EQ(pt.spread_net, pt.spread_gross - pt.cost_drag);
      ++emitted;
    }
  }
  EXPECT_EQ(emitted, 2U);
  // The gross and net series are the same rows of the same vector, so they can
  // never disagree in length, and their intervals share a reportability.
  EXPECT_EQ(sum.full.spread_net_ci.series_len, sum.full.spread_gross_ci.series_len);
  EXPECT_EQ(sum.full.spread_net_ci.reportable, sum.full.spread_gross_ci.reportable);
  // gross(0) = mean{0.01, 0.02} - mean{-0.01, 0.08} = -1/50, and
  // cost_drag(0) = 20 bps of trading + 3 calendar days of borrow = 0.0023.
  // NEAR, not DOUBLE_EQ: the decimal literals are the EXACT rationals the oracle
  // carries, and the engine reaches them through four f64 divisions, so a
  // fixed-ulp comparison against the literal is the wrong instrument.
  EXPECT_NEAR(sum.series[0].spread_gross, -0.02, 1e-15);
  EXPECT_NEAR(sum.series[0].cost_drag, 0.0023, 1e-16);
}

// Case 19c — the day count comes from the CALENDAR, not from h.
TEST(EvalCrossSectionIc, BorrowDays_FridayToMonday_IsThreeCalendarDays) {
  {
    const Panel p = make_f1_panel(); // ... Fri 2013-04-05, Mon 2013-04-08
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    ASSERT_EQ(out.horizons[0].series.size(), 5U);
    EXPECT_EQ(out.horizons[0].series[4].days_forward, atx::i64{3});
    EXPECT_DOUBLE_EQ(out.horizons[0].series[4].borrow_drag, (365.0 / 1e4) * (3.0 / 365.0) * 1.0);
    // Tue -> Wed inside the same week is one day, from the same code path.
    EXPECT_EQ(out.horizons[0].series[1].days_forward, atx::i64{1});
  }
  {
    // A three-session gap (a holiday weekend): Fri 2013-05-24 -> Tue 2013-05-28.
    Panel p{2U, 2U};
    p.session_keys[0] = ns_of_epoch_day(kMay24);
    p.session_keys[1] = ns_of_epoch_day(kMay28);
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    EXPECT_EQ(out.horizons[0].series[0].days_forward, atx::i64{4});
    EXPECT_DOUBLE_EQ(out.horizons[0].series[0].borrow_drag, (365.0 / 1e4) * (4.0 / 365.0) * 1.0);
  }
}

// Case 19d — a long horizon spans weekends, so the calendar day count EXCEEDS
// the observation count: the observation-count convention is provably not in use.
TEST(EvalCrossSectionIc, BorrowDays_LongHorizonSpansWeekends_ExceedsHorizonCount) {
  const Panel p = make_f1_panel(); // Mon 2013-04-01 .. Mon 2013-04-08, 6 sessions
  const std::vector<atx::usize> h{5U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  ASSERT_EQ(out.horizons[0].series.size(), 1U);
  EXPECT_EQ(out.horizons[0].series[0].days_forward, atx::i64{7});
  EXPECT_GT(out.horizons[0].series[0].days_forward, atx::i64{5});
  EXPECT_DOUBLE_EQ(out.horizons[0].series[0].borrow_drag, (365.0 / 1e4) * (7.0 / 365.0) * 1.0);
}

// ===========================================================================
//  §3.11 — autocorrelation and the implied-turnover proxy.
// ===========================================================================

// Case 20.
TEST(EvalCrossSectionIc, Autocorr_IdenticalConsecutiveCrossSections_RhoIsOneTurnoverZero) {
  Panel p{3U, 4U};
  for (atx::usize t = 0U; t < 3U; ++t) {
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[t * 4U + i] = static_cast<atx::f64>(i);
    }
  }
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  EXPECT_EQ(out.autocorr.lag, 1U);
  EXPECT_EQ(out.autocorr.pairs_emitted, 2U);
  EXPECT_DOUBLE_EQ(out.autocorr.rho_pearson, 1.0);
  EXPECT_DOUBLE_EQ(out.autocorr.rho_rank, 1.0);
  EXPECT_DOUBLE_EQ(out.autocorr.implied_one_way_turnover, 0.0);
}

// Case 20b — ruling AR-5: the proxy is derived from `rho_rank`, never from
// `rho_pearson`. The fixture makes the two genuinely disagree.
TEST(EvalCrossSectionIc, Autocorr_TurnoverIsDerivedFromRankNotPearson) {
  Panel p{2U, 4U};
  static constexpr atx::f64 kPrev[4] = {1, 2, 3, 4};
  static constexpr atx::f64 kCurr[4] = {1, 2, 100, 3};
  for (atx::usize i = 0U; i < 4U; ++i) {
    p.signal[i] = kPrev[i];
    p.signal[4U + i] = kCurr[i];
  }
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  EXPECT_EQ(out.autocorr.pairs_emitted, 1U);
  EXPECT_NE(out.autocorr.rho_pearson, out.autocorr.rho_rank);
  EXPECT_NEAR(out.autocorr.rho_rank, 0.8, 1e-12);
  EXPECT_DOUBLE_EQ(out.autocorr.implied_one_way_turnover, 1.0 - out.autocorr.rho_rank);
  // The pearson reading would give a visibly different proxy; pin that it is not
  // the one used.
  EXPECT_NE(out.autocorr.implied_one_way_turnover, 1.0 - out.autocorr.rho_pearson);
}

// ===========================================================================
//  §3.13 — the common-sample prefix, and §3.8's _ex34 restriction.
// ===========================================================================

[[nodiscard]] Panel make_common_sample_panel() { return make_long_panel(40U, 4U); }

[[nodiscard]] CrossSectionIcConfig common_sample_config(std::span<const atx::usize> horizons) {
  CrossSectionIcConfig cfg = good_config(horizons, 16U);
  cfg.quantiles = 4U;
  cfg.common_sample_dates = 30U; // T - max(H) = 40 - 10
  return cfg;
}

// Case 20c — ruling R-B: the common sample is the PREFIX `t < common_sample_dates`
// and nothing else, so on a fixture that guarantees every date emits, all three
// horizons share one sample of exactly that length.
TEST(EvalCrossSectionIc, CommonSample_EveryPrefixDateEmits_AllHorizonsShareOneSample) {
  const Panel p = make_common_sample_panel();
  const std::vector<atx::usize> h{1U, 5U, 10U};
  const CrossSectionIcConfig cfg = common_sample_config(h);
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);

  ASSERT_EQ(out.horizons.size(), 3U);
  for (const IcHorizonSummary &sum : out.horizons) {
    for (const IcDatePoint &pt : sum.series) {
      EXPECT_EQ(pt.in_common_sample, (pt.date < 30U) ? atx::u8{1} : atx::u8{0});
    }
    EXPECT_EQ(sum.common.dates_emitted, 30U);
    EXPECT_EQ(sum.common.common_prefix_gaps, 0U);
    EXPECT_EQ(sum.common.summary_reportable, atx::u8{1});
    EXPECT_EQ(sum.common.unreportable_reason, atx::u8{0});
    EXPECT_EQ(sum.full.common_prefix_gaps, 0U); // meaningless on the full block
  }
}

// Case 20d — ruling NEW-1: ONE counter per (horizon, sample), shared by the IC
// and spread families, and any gap voids the ENTIRE common block — means
// included. Both sub-cases (no IC, and IC-but-no-spread) give the SAME outcome.
TEST(EvalCrossSectionIc, CommonSample_OnePrefixDateFailsToEmit_VoidsTheWholeCommonBlock) {
  const std::vector<atx::usize> h{1U, 5U, 10U};
  const CrossSectionIcConfig cfg = common_sample_config(h);

  {
    Panel p = make_common_sample_panel();
    for (atx::usize i = 1U; i < 4U; ++i) {
      p.mask[3U * 4U + i] = atx::u8{0}; // prefix date 3 falls below min_names
    }
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    for (const IcHorizonSummary &sum : out.horizons) {
      EXPECT_EQ(sum.series[3].in_common_sample, atx::u8{1}); // still IN the prefix
      EXPECT_EQ(sum.series[3].emitted, atx::u8{0});
      EXPECT_EQ(sum.common.dates_emitted, 29U);
      EXPECT_EQ(sum.common.common_prefix_gaps, 1U);
      EXPECT_EQ(sum.common.summary_reportable, atx::u8{0});
      EXPECT_EQ(sum.common.unreportable_reason, atx::u8{2});
      // Every *_common field is nulled, MEANS INCLUDED.
      EXPECT_EQ(sum.common.ic_mean, 0.0);
      EXPECT_EQ(sum.common.rank_ic_mean, 0.0);
      EXPECT_EQ(sum.common.spread_gross_mean, 0.0);
      EXPECT_EQ(sum.common.spread_net_mean, 0.0);
      EXPECT_EQ(sum.common.ic_mean_ci.reportable, atx::u8{0});
      EXPECT_EQ(sum.common.ic_mean_ci.unreportable_reason, atx::u8{2});
      EXPECT_EQ(sum.common.spread_gross_ci.unreportable_reason, atx::u8{2});
      // The full-sample block for the same horizon is untouched: it simply loses
      // the one date that did not emit, and stays reportable.
      EXPECT_EQ(sum.full.summary_reportable, atx::u8{1});
      EXPECT_EQ(sum.full.unreportable_reason, atx::u8{0});
      EXPECT_EQ(sum.full.dates_emitted, sum.series.size() - 1U);
      EXPECT_EQ(sum.full.common_prefix_gaps, 0U);
    }
  }
  {
    // Companion: min_names_per_date <= n < Q. The IC emits, the spread does not,
    // and the whole block is voided just the same — one counter, one outcome.
    Panel p = make_common_sample_panel();
    p.mask[5U * 4U + 3U] = atx::u8{0}; // 3 admitted names, Q == 4
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    for (const IcHorizonSummary &sum : out.horizons) {
      EXPECT_EQ(sum.series[5].emitted, atx::u8{1});
      EXPECT_EQ(sum.series[5].spread_emitted, atx::u8{0});
      EXPECT_EQ(sum.common.common_prefix_gaps, 1U);
      EXPECT_EQ(sum.common.summary_reportable, atx::u8{0});
      EXPECT_EQ(sum.common.unreportable_reason, atx::u8{2});
      EXPECT_EQ(sum.common.ic_mean, 0.0);
    }
  }
}

// Case 20e — ruling AR-7: the `_ex34` restriction removes the named IDs from the
// sub-universe and the unrestricted run counts them.
TEST(EvalCrossSectionIc, Ex34Restriction_ExcludesNamedIdsAndCountsThem) {
  const Panel p = make_f4_panel();
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig full = good_config(h);
  full.quantiles = 2U;
  full.forward_variant = ForwardReturnVariant::IncludeAuditedTerminalV1;
  CrossSectionIcConfig restricted = full;
  restricted.stream_restriction_id = 1U;

  const CrossSectionIcResult unrestricted_out = run_or_die(p.view(), full);
  const CrossSectionIcResult restricted_out = run_or_die(p.view(), restricted);

  EXPECT_EQ(unrestricted_out.horizons[0].series[0].n_excluded_audited, 4U);
  EXPECT_EQ(unrestricted_out.horizons[0].series[0].n_eligible, 7U);
  // Under the restriction the four audited IDs are not in the universe at all,
  // so nothing is left to count.
  EXPECT_EQ(restricted_out.horizons[0].series[0].n_excluded_audited, 0U);
  EXPECT_EQ(restricted_out.horizons[0].series[0].n_eligible, 3U);
  // An excluded name carries signal here, so the statistics genuinely differ.
  EXPECT_NE(unrestricted_out.horizons[0].series[0].pearson_ic,
            restricted_out.horizons[0].series[0].pearson_ic);
}

// ===========================================================================
//  §3.10 — the circular block bootstrap.
// ===========================================================================

// dates = 51 at h = 1 gives n = 50 and L = 5: floor(50/5) == 10 and n >= 20, so
// this is the shortest REPORTABLE shape the frozen rule admits at the floor.
[[nodiscard]] CrossSectionIcResult run_bootstrap_panel(atx::u64 seed, atx::usize draws) {
  const Panel p = make_long_panel(51U, 4U);
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h, draws);
  cfg.quantiles = 4U;
  cfg.bootstrap_seed = seed;
  return run_or_die(p.view(), cfg);
}

// Case 13.
TEST(EvalCrossSectionIc, Bootstrap_SameSeed_IsBitIdentical) {
  const CrossSectionIcResult a = run_bootstrap_panel(20260920ULL, 200U);
  const CrossSectionIcResult b = run_bootstrap_panel(20260920ULL, 200U);
  const BootstrapInterval &ia = a.horizons[0].full.ic_mean_ci;
  const BootstrapInterval &ib = b.horizons[0].full.ic_mean_ci;
  ASSERT_EQ(ia.reportable, atx::u8{1});
  EXPECT_EQ(ia.lo, ib.lo);
  EXPECT_EQ(ia.hi, ib.hi);
  EXPECT_EQ(a.horizons[0].full.icir_ci.lo, b.horizons[0].full.icir_ci.lo);
  EXPECT_EQ(a.horizons[0].full.icir_ci.hi, b.horizons[0].full.icir_ci.hi);
  EXPECT_LE(ia.lo, ia.hi);
}

TEST(EvalCrossSectionIc, Bootstrap_DifferentSeed_Differs) {
  const CrossSectionIcResult a = run_bootstrap_panel(20260920ULL, 200U);
  const CrossSectionIcResult b = run_bootstrap_panel(20260921ULL, 200U);
  const BootstrapInterval &ia = a.horizons[0].full.ic_mean_ci;
  const BootstrapInterval &ib = b.horizons[0].full.ic_mean_ci;
  ASSERT_EQ(ia.reportable, atx::u8{1});
  ASSERT_EQ(ib.reportable, atx::u8{1});
  EXPECT_EQ(ia.point, ib.point); // the POINT estimate is seed-independent
  EXPECT_TRUE(ia.lo != ib.lo || ia.hi != ib.hi);
}

// Case 15 — the bounded-loop fallback counter is MEASURED, not asserted away.
TEST(EvalCrossSectionIc, Bootstrap_ModuloFallbackCounter_IsZero) {
  const CrossSectionIcResult out = run_bootstrap_panel(20260920ULL, 200U);
  const IcSampleStats &s = out.horizons[0].full;
  ASSERT_EQ(s.ic_mean_ci.reportable, atx::u8{1});
  EXPECT_EQ(s.ic_mean_ci.modulo_fallbacks, 0U);
  EXPECT_EQ(s.icir_ci.modulo_fallbacks, 0U);
  EXPECT_EQ(s.ic_mean_ci.draws, 200U);
  EXPECT_EQ(s.ic_mean_ci.block_len, 5U);
  EXPECT_EQ(s.ic_mean_ci.series_len, 50U);
  EXPECT_EQ(s.ic_mean_ci.blocks, 10U); // ceil(50 / 5)
}

// Case 13b — the exact §3.10 stream, pinned index-for-index against the
// independent oracle (`iteration14-cross-section-oracle.json`, cases
// `bootstrap_f6_sample0` / `bootstrap_f6_sample1`). Asserting BOTH sample ids is
// what proves ruling R-C's `sample_id` field actually separates the streams.
[[nodiscard]] std::vector<atx::u64> first_draw_starts(atx::u64 sample_id, atx::usize n,
                                                      atx::usize count,
                                                      atx::usize &modulo_fallbacks,
                                                      atx::u64 &stream_key, atx::u64 &seed_x) {
  stream_key = detail::bootstrap_stream_key(BootstrapStatisticId::IcMean, 0ULL, 0ULL, 0ULL, 0ULL,
                                            sample_id, 20260920ULL);
  atx::u64 state = stream_key;
  seed_x = detail::splitmix64_next(state);
  atx::core::Xoshiro256pp rng{seed_x};
  std::vector<atx::u64> starts;
  starts.reserve(count);
  for (atx::usize k = 0U; k < count; ++k) {
    starts.push_back(detail::draw_below(rng, static_cast<atx::u64>(n), modulo_fallbacks));
  }
  return starts;
}

TEST(EvalCrossSectionIc, Bootstrap_FirstEightDrawStarts_MatchThePinnedRecipe) {
  atx::usize fallbacks = 0U;
  atx::u64 key0 = 0ULL;
  atx::u64 seed0 = 0ULL;
  const std::vector<atx::u64> s0 = first_draw_starts(0ULL, 188U, 8U, fallbacks, key0, seed0);
  atx::u64 key1 = 0ULL;
  atx::u64 seed1 = 0ULL;
  const std::vector<atx::u64> s1 = first_draw_starts(1ULL, 126U, 8U, fallbacks, key1, seed1);

  // The key layout: every field is 0 for sample 0, so the key IS the seed.
  EXPECT_EQ(key0, 20260920ULL);
  EXPECT_EQ(key1, 20260920ULL ^ (1ULL << 48U));
  EXPECT_EQ(key1, 281474996971576ULL);
  EXPECT_EQ(seed0, 17691334895777841011ULL);
  EXPECT_EQ(seed1, 16877986321376366485ULL);

  const std::vector<atx::u64> expect0{66U, 86U, 112U, 67U, 3U, 156U, 0U, 39U};
  const std::vector<atx::u64> expect1{63U, 67U, 5U, 74U, 55U, 71U, 67U, 111U};
  EXPECT_EQ(s0, expect0);
  EXPECT_EQ(s1, expect1);
  EXPECT_NE(s0, s1); // the whole point of ruling R-C
  EXPECT_EQ(fallbacks, 0U);

  // Both block lengths at h = 1 are the pre-registered floor.
  EXPECT_EQ(detail::block_len(1U, 5U), 5U);

  // --- I-3: every one of the six byte-aligned fields, not just sample_id ------
  // Both F6 oracle tuples are all-zero except `sample_id`, so five of the six
  // shifts were previously unpinned: reverting §3.10's move of `horizon_index`
  // from bits 0-7 to bits 16-23, or transposing `signal_index` and `variant_id`,
  // changed no test. At seed 0 the key IS the single field's shifted value.
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::Icir, 0, 0, 0, 0, 0, 0ULL),
            1ULL << 8U);
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::IcMean, 2, 0, 0, 0, 0, 0ULL),
            2ULL << 16U);
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::IcMean, 0, 3, 0, 0, 0, 0ULL),
            3ULL << 24U);
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::IcMean, 0, 0, 1, 0, 0, 0ULL),
            1ULL << 32U);
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::IcMean, 0, 0, 0, 1, 0, 0ULL),
            1ULL << 40U);
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::IcMean, 0, 0, 0, 0, 1, 0ULL),
            1ULL << 48U);
  // The frozen statistic ids occupy bits 8-15 and nothing else.
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::SpreadNet, 0, 0, 0, 0, 0, 0ULL),
            5ULL << 8U);

  // All six nonzero at once, hand-derived against the frozen seed:
  //   20260920            = 0x0135'2838
  //   ^ (Icir = 1) << 8   -> 0x28 ^ 0x01 = 0x29   => 0x0135'2938
  //   ^ (h_idx = 2) << 16 -> 0x35 ^ 0x02 = 0x37   => 0x0137'2938
  //   ^ (sig   = 3) << 24 -> 0x01 ^ 0x03 = 0x02   => 0x0237'2938
  //   ^ (var   = 1) << 32 |
  //   ^ (restr = 1) << 40 | high word 0x0001'0101
  //   ^ (sample= 1) << 48 |
  //   => 0x0001'0101'0237'2938
  EXPECT_EQ(detail::bootstrap_stream_key(BootstrapStatisticId::Icir, 2, 3, 1, 1, 1, 20260920ULL),
            0x0001010102372938ULL);

  // --- I-3, behavioural half: the key must actually reach the drawn stream ---
  // `make_interval` builds the key from cfg, so a field that never reaches the
  // generator would leave two configurations with identical intervals.
  {
    const Panel panel = make_long_panel(51U, 4U);
    const std::vector<atx::usize> horizons{1U};
    CrossSectionIcConfig base = good_config(horizons, 64U);
    base.quantiles = 4U;
    CrossSectionIcConfig other = base;
    other.stream_signal_index = 1U;
    CrossSectionIcConfig third = base;
    third.stream_variant_id = 1U;
    const CrossSectionIcResult a = run_or_die(panel.view(), base);
    const CrossSectionIcResult b = run_or_die(panel.view(), other);
    const CrossSectionIcResult c = run_or_die(panel.view(), third);
    ASSERT_EQ(a.horizons[0].full.ic_mean_ci.reportable, atx::u8{1});
    // Same data, same point estimate, different stream -> different interval.
    EXPECT_EQ(a.horizons[0].full.ic_mean_ci.point, b.horizons[0].full.ic_mean_ci.point);
    EXPECT_TRUE(a.horizons[0].full.ic_mean_ci.lo != b.horizons[0].full.ic_mean_ci.lo ||
                a.horizons[0].full.ic_mean_ci.hi != b.horizons[0].full.ic_mean_ci.hi);
    EXPECT_TRUE(a.horizons[0].full.ic_mean_ci.lo != c.horizons[0].full.ic_mean_ci.lo ||
                a.horizons[0].full.ic_mean_ci.hi != c.horizons[0].full.ic_mean_ci.hi);
  }
}

// ---------------------------------------------------------------------------
//  I-4 — the ENGINE's resample path, pinned end to end.
//
//  `replay_single_draw` reproduces §3.10's resample from the engine's OWN
//  emitted source series: `ceil(n/L)` starts, block `j` contributing
//  `S[(s_j + k) mod n]`, the concatenation truncated to exactly `n`, and §3.9's
//  two-pass mean-then-sd with the `n - 1` divisor. It is not a second oracle —
//  the series it consumes comes out of the engine — so it pins the modulus, the
//  truncation, the draw count and the divisor, each of which a mutation could
//  change while determinism, `modulo_fallbacks == 0` and the F6 stream export
//  all stayed green.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::f64 replay_single_draw(std::span<const atx::f64> source, atx::usize len,
                                          BootstrapStatisticId statistic, atx::u64 sample_id,
                                          atx::u64 seed, bool info_ratio) {
  atx::u64 state =
      detail::bootstrap_stream_key(statistic, 0ULL, 0ULL, 0ULL, 0ULL, sample_id, seed);
  const atx::u64 seed_x = detail::splitmix64_next(state);
  atx::core::Xoshiro256pp rng{seed_x};
  const atx::usize n = source.size();
  const atx::usize blocks = (n + len - 1U) / len;
  atx::usize fallbacks = 0U;
  std::vector<atx::u64> starts;
  starts.reserve(blocks);
  for (atx::usize j = 0U; j < blocks; ++j) {
    starts.push_back(detail::draw_below(rng, static_cast<atx::u64>(n), fallbacks));
  }
  EXPECT_EQ(fallbacks, 0U);

  atx::f64 total = 0.0;
  atx::usize filled = 0U;
  for (const atx::u64 start : starts) {
    for (atx::usize k = 0U; k < len && filled < n; ++k, ++filled) {
      total += source[(static_cast<atx::usize>(start) + k) % n];
    }
  }
  const atx::f64 mean = total / static_cast<atx::f64>(n);
  if (!info_ratio) {
    return mean;
  }
  atx::f64 ss = 0.0;
  filled = 0U;
  for (const atx::u64 start : starts) {
    for (atx::usize k = 0U; k < len && filled < n; ++k, ++filled) {
      const atx::f64 d = source[(static_cast<atx::usize>(start) + k) % n] - mean;
      ss += d * d;
    }
  }
  const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(n - 1U));
  return (sd == 0.0) ? 0.0 : mean / sd;
}

TEST(EvalCrossSectionIc, Bootstrap_SingleDraw_CollapsesToTheReplayedDrawStatistic) {
  const Panel p = make_long_panel(51U, 4U);
  const std::vector<atx::usize> h{1U};
  CrossSectionIcConfig cfg = good_config(h, 1U); // draws == 1
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);
  const IcHorizonSummary &sum = out.horizons[0];
  const IcSampleStats &s = sum.full;

  ASSERT_EQ(s.ic_mean_ci.reportable, atx::u8{1});
  EXPECT_EQ(s.ic_mean_ci.draws, 1U);
  EXPECT_EQ(s.ic_mean_ci.blocks, 10U); // ceil(50 / 5)
  // With one draw, `quantile_sorted_asc` on a one-element span returns
  // sorted[0] for both percentiles, so the interval collapses onto the draw.
  EXPECT_EQ(s.ic_mean_ci.lo, s.ic_mean_ci.hi);
  EXPECT_EQ(s.icir_ci.lo, s.icir_ci.hi);

  std::vector<atx::f64> source;
  for (const IcDatePoint &pt : sum.series) {
    if (pt.emitted != 0U) {
      source.push_back(pt.pearson_ic);
    }
  }
  ASSERT_EQ(source.size(), 50U);
  ASSERT_EQ(sum.block_len, 5U);

  // I-6's accepting side on the same fixture: the spread series is also 50 dates
  // long at L = 5, so its own gate opens and the means are published.
  EXPECT_EQ(sum.spread_reportable, atx::u8{1});
  EXPECT_EQ(sum.spread_unreportable_reason, atx::u8{0});
  EXPECT_EQ(s.spread_gross_ci.series_len, 50U);
  EXPECT_GT(sum.mean_cost_drag, 0.0);
  EXPECT_LT(s.spread_net_mean, s.spread_gross_mean); // a positive drag, every date

  EXPECT_EQ(s.ic_mean_ci.lo,
            replay_single_draw(source, sum.block_len, BootstrapStatisticId::IcMean,
                               cfg.stream_sample_id, cfg.bootstrap_seed, false));
  EXPECT_EQ(s.icir_ci.lo,
            replay_single_draw(source, sum.block_len, BootstrapStatisticId::Icir,
                               cfg.stream_sample_id, cfg.bootstrap_seed, true));
}

// Case 14 — the frozen `unreportable_reason` enum and its lowest-nonzero-wins
// precedence (ruling A-3), one deciding condition at a time.
TEST(EvalCrossSectionIc, Bootstrap_SeriesShorterThanTenBlocks_IsNotReportable) {
  {
    // n = 40, L = 5 -> floor(40/5) == 8 < 10, and n >= 20: code 3 alone.
    const Panel p = make_long_panel(41U, 4U);
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h, 32U);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const BootstrapInterval &iv = out.horizons[0].full.ic_mean_ci;
    EXPECT_EQ(iv.series_len, 40U);
    EXPECT_EQ(iv.reportable, atx::u8{0});
    EXPECT_EQ(iv.unreportable_reason, atx::u8{3});
    EXPECT_EQ(iv.lo, 0.0); // untouched
    EXPECT_EQ(iv.hi, 0.0);
  }
  {
    // n = 19 with block_len_floor == 1 -> floor(19/1) == 19 >= 10, so ONLY the
    // n < 20 bar fails: code 1 alone.
    const Panel p = make_long_panel(20U, 4U);
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h, 32U);
    cfg.quantiles = 4U;
    cfg.block_len_floor = 1U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const BootstrapInterval &iv = out.horizons[0].full.ic_mean_ci;
    EXPECT_EQ(iv.series_len, 19U);
    EXPECT_EQ(iv.block_len, 1U);
    EXPECT_EQ(iv.unreportable_reason, atx::u8{1});
  }
  {
    // n < 20 AND floor(n/L) < 10 together: the LOWEST nonzero code wins, so 1.
    const Panel p = make_long_panel(20U, 4U);
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h, 32U);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const BootstrapInterval &iv = out.horizons[0].full.ic_mean_ci;
    EXPECT_EQ(iv.series_len, 19U);
    EXPECT_EQ(iv.block_len, 5U); // floor(19/5) == 3 < 10 also holds
    EXPECT_EQ(iv.unreportable_reason, atx::u8{1});
  }
  {
    // A common block that is BOTH prefix-gapped and block-short reports 2: it
    // wins over a co-occurring code 3 by the same lowest-nonzero rule (§3.13).
    Panel p = make_long_panel(60U, 4U);
    for (atx::usize i = 1U; i < 4U; ++i) {
      p.mask[2U * 4U + i] = atx::u8{0}; // one prefix date fails to emit
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h, 32U);
    cfg.quantiles = 4U;
    cfg.common_sample_dates = 40U; // n_common = 39: >= 20, but 39/5 == 7 < 10
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const IcSampleStats &s = out.horizons[0].common;
    EXPECT_EQ(s.common_prefix_gaps, 1U);
    EXPECT_EQ(s.ic_mean_ci.series_len, 39U);
    EXPECT_EQ(s.ic_mean_ci.unreportable_reason, atx::u8{2});
    EXPECT_EQ(s.unreportable_reason, atx::u8{2});
  }
}

// ===========================================================================
//  §9.2 — the native export the oracle comparator consumes.
//
//  One `CROSS_SECTION_IC_MEASUREMENT ` line per oracle scenario, JSON after the
//  marker, no JSON dependency and no file I/O. `std::to_chars` is locale
//  independent and shortest-round-trip, which is what makes the printed decimal
//  recover the exact binary64 the engine held.
//
//  Ruling A-5: EVERY line carries `inputs.n` and `inputs.sample_id`; the
//  bootstrap-bearing lines additionally reconstruct their own stream key.
// ===========================================================================

[[nodiscard]] std::string jnum(atx::f64 v) {
  // M-8: `std::to_chars` renders a non-finite double as the bare tokens
  // `inf` / `-inf` / `nan`, which are not JSON — the comparator's `raw_decode`
  // would then report "undecodable measurement JSON" and a NaN escape would be
  // diagnosed as a formatting bug rather than as the numeric defect it is. JSON
  // `null` is legible to `compare_leaf`'s `nonfinite` kind, which accepts
  // `native is None`. The design guarantees this branch is unreachable.
  if (!std::isfinite(v)) {
    return "null";
  }
  std::array<char, 64> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return std::string(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
}

[[nodiscard]] std::string jint(atx::usize v) { return std::to_string(v); }

[[nodiscard]] std::string ju64(atx::u64 v) { return "\"" + std::to_string(v) + "\""; }

[[nodiscard]] std::string jnumlist(const std::vector<atx::f64> &values) {
  std::string out = "[";
  for (atx::usize k = 0U; k < values.size(); ++k) {
    if (k != 0U) {
      out += ",";
    }
    out += jnum(values[k]);
  }
  return out + "]";
}

[[nodiscard]] std::string jintlist(const std::vector<atx::usize> &values) {
  std::string out = "[";
  for (atx::usize k = 0U; k < values.size(); ++k) {
    if (k != 0U) {
      out += ",";
    }
    out += std::to_string(values[k]);
  }
  return out + "]";
}

class Obj {
public:
  Obj &raw(const char *key, const std::string &value) {
    if (!body_.empty()) {
      body_ += ",";
    }
    body_ += "\"";
    body_ += key;
    body_ += "\":";
    body_ += value;
    return *this;
  }
  Obj &num(const char *key, atx::f64 value) { return raw(key, jnum(value)); }
  Obj &integer(const char *key, atx::usize value) { return raw(key, jint(value)); }
  [[nodiscard]] std::string json() const { return "{" + body_ + "}"; }

private:
  std::string body_;
};

// `n` and `sample_id` are mandatory on every line (ruling A-5): a line missing
// either is a comparator FAILURE, not a warning.
[[nodiscard]] Obj inputs_of(atx::usize n, atx::usize sample_id) {
  Obj o;
  o.integer("n", n).integer("sample_id", sample_id);
  return o;
}

// Parent ruling I-6 / design §11.8: the spread family's own reportability,
// published beside every spread measurement so a reader never has to infer it
// from `summary_reportable`, which describes the IC family only. The oracle
// carries no expectation for these, so the comparator records them under
// `native_keys_with_no_oracle_expectation`; T5 publishes them in the CSV/JSON.
void add_spread_gate(Obj &values, const IcHorizonSummary &sum) {
  values.integer("spread_reportable", sum.spread_reportable)
      .integer("spread_unreportable_reason", sum.spread_unreportable_reason);
}

void emit_measurement(const char *case_id, const Obj &inputs, const Obj &values) {
  std::cout << "CROSS_SECTION_IC_MEASUREMENT "
            << "{\"schema\":\"atx-cross-section-ic-measurement-v1\",\"case_id\":\"" << case_id
            << "\",\"inputs\":" << inputs.json() << ",\"values\":" << values.json() << "}\n";
}

void emit_f1_family() {
  const Panel p = make_f1_panel();
  const std::vector<atx::usize> h{1U, 5U};
  CrossSectionIcConfig cfg = good_config(h);
  cfg.quantiles = 4U;
  const CrossSectionIcResult out = run_or_die(p.view(), cfg);
  ASSERT_EQ(out.horizons.size(), 2U);

  static const char *kIcCases[5] = {"f1_ic_h1_t0", "f1_ic_h1_t1", "f1_ic_h1_t2", "f1_ic_h1_t3",
                                    "f1_ic_h1_t4"};
  for (atx::usize t = 0U; t < 5U; ++t) {
    const IcDatePoint &pt = out.horizons[0].series[t];
    Obj values;
    values.num("pearson_ic", pt.pearson_ic).num("rank_ic", pt.rank_ic);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 1U).integer("date_index", t);
    emit_measurement(kIcCases[t], inputs, values);
  }
  {
    const IcDatePoint &pt = out.horizons[1].series[0];
    Obj values;
    values.num("pearson_ic", pt.pearson_ic).num("rank_ic", pt.rank_ic);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 5U).integer("date_index", 0U);
    emit_measurement("f1_ic_h5_t0", inputs, values);
  }
  static const char *kSeriesCases[2] = {"f1_series_h1", "f1_series_h5"};
  for (atx::usize k = 0U; k < 2U; ++k) {
    const IcSampleStats &s = out.horizons[k].full;
    Obj values;
    values.integer("summary_reportable", s.summary_reportable)
        .num("ic_mean", s.ic_mean)
        .num("ic_sd", s.ic_sd)
        .num("icir", s.icir)
        .num("naive_t", s.naive_t)
        .num("rank_ic_mean", s.rank_ic_mean)
        .num("rank_ic_sd", s.rank_ic_sd)
        .num("rank_icir", s.rank_icir)
        .num("rank_naive_t", s.rank_naive_t);
    Obj inputs = inputs_of(s.dates_emitted, 0U);
    inputs.integer("horizon", out.horizons[k].horizon);
    emit_measurement(kSeriesCases[k], inputs, values);
  }
  // f2_spread_q4_eight_names shares this run: date 0 at Q = 4 on eight names.
  {
    const IcDatePoint &pt = out.horizons[0].series[0];
    Obj values;
    values.num("spread", pt.spread_gross).integer("spread_emitted", pt.spread_emitted);
    add_spread_gate(values, out.horizons[0]);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 1U).integer("quantile_count", 4U);
    emit_measurement("f2_spread_q4_eight_names", inputs, values);
  }
  // The two F5 day cases whose session keys this panel already carries.
  {
    const IcDatePoint &pt = out.horizons[0].series[4]; // Fri 04-05 -> Mon 04-08
    Obj values;
    values.integer("days_forward", static_cast<atx::usize>(pt.days_forward))
        .num("borrow_drag", pt.borrow_drag);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 1U);
    emit_measurement("f5_days_friday_to_monday_is_three", inputs, values);
  }
  {
    const IcDatePoint &pt = out.horizons[1].series[0]; // Mon 04-01 -> Mon 04-08 at h = 5
    Obj values;
    values.integer("days_forward", static_cast<atx::usize>(pt.days_forward))
        .num("borrow_drag", pt.borrow_drag);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 5U);
    emit_measurement("f5_days_horizon_five_spans_seven_days", inputs, values);
  }
}

void emit_primitive_cases() {
  // Perfect affine image (oracle f1_pearson_perfect_affine_is_one).
  {
    Panel p{2U, 4U};
    for (atx::usize i = 0U; i < 4U; ++i) {
      const atx::f64 x = static_cast<atx::f64>(i) + 1.0;
      p.signal[i] = x;
      p.price[i] = 1.0;
      p.raw_price[i] = 1.0;
      p.price[4U + i] = 1.0 + (2.0 * x + 1.0);
      p.raw_price[4U + i] = p.price[4U + i];
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.num("pearson_ic", out.horizons[0].series[0].pearson_ic)
        .num("rank_ic", out.horizons[0].series[0].rank_ic);
    emit_measurement("f1_pearson_perfect_affine_is_one", inputs_of(4U, 0U), values);
  }
  // Constant signal (oracle f1_pearson_constant_signal_is_zero_not_nan).
  {
    Panel p{2U, 4U};
    static constexpr atx::f64 kForward[4] = {0.01, 0.02, -0.01, 0.04};
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[i] = 3.0;
      p.price[4U + i] = 100.0 * (1.0 + kForward[i]);
      p.raw_price[4U + i] = p.price[4U + i];
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.num("pearson_ic", out.horizons[0].series[0].pearson_ic);
    emit_measurement("f1_pearson_constant_signal_is_zero_not_nan", inputs_of(4U, 0U), values);
  }
  // The n < 2 early return, through the panel (oracle
  // f1_pearson_fewer_than_two_names_is_zero_not_nan).
  {
    Panel p{2U, 2U};
    p.mask[1] = atx::u8{0};
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.num("pearson_ic", out.horizons[0].series[0].pearson_ic)
        .integer("date_emitted", out.horizons[0].series[0].emitted)
        .integer("dates_below_min_names_increment", out.horizons[0].dates_below_min_names);
    emit_measurement("f1_pearson_fewer_than_two_names_is_zero_not_nan", inputs_of(1U, 0U), values);
  }
  // The two rank cases, at the primitive `core::stats::rank` the engine calls.
  {
    const std::vector<atx::f64> tied{1.0, 1.0, 2.0, 2.0};
    std::vector<atx::f64> ranks(4U, 0.0);
    std::vector<atx::usize> order(4U, 0U);
    atx::core::stats::rank(tied, std::span<atx::f64>{ranks}, std::span<atx::usize>{order});
    Obj values;
    values.raw("ranks", jnumlist(ranks));
    emit_measurement("f1_rank_tied_values_average_positions", inputs_of(4U, 0U), values);
  }
  {
    const std::vector<atx::f64> values_in{1.0, std::nextafter(1.0, 2.0), 2.0, 3.0};
    std::vector<atx::f64> ranks(4U, 0.0);
    std::vector<atx::usize> order(4U, 0U);
    atx::core::stats::rank(values_in, std::span<atx::f64>{ranks}, std::span<atx::usize>{order});
    Obj values;
    values.raw("ranks", jnumlist(ranks));
    emit_measurement("f1_rank_one_ulp_apart_are_distinct_ranks", inputs_of(4U, 0U), values);
  }
}

void emit_f2_family() {
  const Panel f1 = make_f1_panel();
  // Tie-break fixture.
  {
    Panel p{2U, 8U};
    static constexpr atx::f64 kTied[8] = {3, 5, 5, 1, 4, 4, 2, 0};
    for (atx::usize t = 0U; t < 2U; ++t) {
      p.session_keys[t] = f1.session_keys[t];
      for (atx::usize i = 0U; i < 8U; ++i) {
        p.signal[t * 8U + i] = kTied[i];
        p.price[t * 8U + i] = f1.price[t * 8U + i];
        p.raw_price[t * 8U + i] = f1.price[t * 8U + i];
      }
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.num("spread", out.horizons[0].series[0].spread_gross)
        .integer("spread_emitted", out.horizons[0].series[0].spread_emitted);
    add_spread_gate(values, out.horizons[0]);
    Obj inputs = inputs_of(8U, 0U);
    inputs.integer("quantile_count", 4U);
    emit_measurement("f2_tie_break_by_ascending_instrument_index", inputs, values);
  }
  // n == Q == 4.
  {
    Panel p{2U, 4U};
    for (atx::usize t = 0U; t < 2U; ++t) {
      p.session_keys[t] = f1.session_keys[t];
      for (atx::usize i = 0U; i < 4U; ++i) {
        p.signal[t * 4U + i] = f1.signal[t * 8U + i];
        p.price[t * 4U + i] = f1.price[t * 8U + i];
        p.raw_price[t * 4U + i] = f1.price[t * 8U + i];
      }
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    std::vector<atx::usize> sizes;
    std::vector<atx::f64> means;
    for (const QuantileBucketStat &b : out.horizons[0].buckets) {
      sizes.push_back(static_cast<atx::usize>(b.mean_names));
      means.push_back(b.mean_forward_return);
    }
    Obj values;
    values.num("spread", out.horizons[0].series[0].spread_gross)
        .integer("spread_emitted", out.horizons[0].series[0].spread_emitted)
        .raw("bucket_sizes", jintlist(sizes))
        .raw("bucket_means", jnumlist(means));
    add_spread_gate(values, out.horizons[0]);
    Obj inputs = inputs_of(4U, 0U);
    inputs.integer("quantile_count", 4U);
    emit_measurement("f2_exactly_q_names_one_per_bucket", inputs, values);
  }
  // n == 3 < Q == 4.
  {
    Panel p{2U, 3U};
    for (atx::usize t = 0U; t < 2U; ++t) {
      p.session_keys[t] = f1.session_keys[t];
      for (atx::usize i = 0U; i < 3U; ++i) {
        p.signal[t * 3U + i] = f1.signal[t * 8U + i];
        p.price[t * 3U + i] = f1.price[t * 8U + i];
        p.raw_price[t * 3U + i] = f1.price[t * 8U + i];
      }
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 4U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.integer("spread_emitted", out.horizons[0].series[0].spread_emitted)
        .integer("dates_below_quantile_count_increment",
                 out.horizons[0].dates_below_quantile_count);
    add_spread_gate(values, out.horizons[0]);
    Obj inputs = inputs_of(3U, 0U);
    inputs.integer("quantile_count", 4U);
    emit_measurement("f2_fewer_than_q_names_emits_no_spread", inputs, values);
  }
  // The pre-registered Q = 10 on exactly ten names.
  {
    Panel p{2U, 10U};
    for (atx::usize t = 0U; t < 2U; ++t) {
      p.session_keys[t] = ns_of_epoch_day(kApr01 + static_cast<atx::i64>(t));
    }
    for (atx::usize i = 0U; i < 10U; ++i) {
      const atx::f64 base = 100.0 + static_cast<atx::f64>(i);
      p.signal[i] = 10.0 - static_cast<atx::f64>(i);
      p.price[i] = base;
      p.raw_price[i] = base;
      p.price[10U + i] = base + (static_cast<atx::f64>(i) - 4.0) * 0.25;
      p.raw_price[10U + i] = p.price[10U + i];
    }
    const std::vector<atx::usize> h{1U};
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 10U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.num("spread", out.horizons[0].series[0].spread_gross)
        .integer("spread_emitted", out.horizons[0].series[0].spread_emitted);
    add_spread_gate(values, out.horizons[0]);
    Obj inputs = inputs_of(10U, 0U);
    inputs.integer("quantile_count", 10U);
    emit_measurement("f2_spread_q10_ten_names", inputs, values);
  }
}

// ---------------------------------------------------------------------------
//  `t_last_offset`, DERIVED FROM THE ENGINE'S OWN OUTPUT, not re-implemented.
//
//  The API exposes no `t_last`. Rather than restate `last_valid_mark` in the
//  test — which would compare the engine against nothing — this identifies the
//  offset the engine ACTUALLY used, by asking which candidate mark reproduces
//  the measured forward return under §3.8's composition. If `last_valid_mark`
//  ever picked a different mark, the measured return would stop matching the
//  offset reported here.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::usize identify_t_last(const Panel &p, atx::usize t, atx::usize h,
                                         atx::f64 measured_return) {
  const atx::usize inst = p.instruments;
  const atx::f64 p0 = p.price[t * inst];
  const atx::f64 consideration = p.terminal_value[t * inst];
  atx::usize best = 0U;
  atx::f64 best_gap = std::numeric_limits<atx::f64>::infinity();
  for (atx::usize u = t + 1U; u <= t + h; ++u) {
    const atx::f64 pu = p.price[u * inst];
    const atx::f64 raw_u = p.raw_price[u * inst];
    if (!std::isfinite(pu) || pu <= 0.0 || !std::isfinite(raw_u) || raw_u <= 0.0) {
      continue;
    }
    const atx::f64 candidate = (pu / p0) * (1.0 + (consideration / raw_u - 1.0)) - 1.0;
    const atx::f64 gap = std::fabs(candidate - measured_return);
    if (gap < best_gap) {
      best_gap = gap;
      best = u - t;
    }
  }
  return best;
}

// `terminal_leg`, likewise derived from the measured return rather than
// recomputed: leg = (1 + r) / (P(t_last)/P(t)) - 1 inverts §3.8's composition,
// so it is wrong exactly when the engine's leg is wrong.
[[nodiscard]] atx::f64 derive_terminal_leg(const Panel &p, atx::usize t, atx::usize t_last_offset,
                                           atx::f64 measured_return) {
  const atx::usize inst = p.instruments;
  const atx::f64 ratio = p.price[(t + t_last_offset) * inst] / p.price[t * inst];
  return (1.0 + measured_return) / ratio - 1.0;
}

void emit_f3_family() {
  const std::vector<atx::usize> h2{2U};

  // The five evidenced-leg cases. Four are RE-CUT to the oracle-v2 shapes (T7a §6.2
  // table 2, parent ruling C-1/I-1 reversing deviation D-8). v1 placed the last
  // mark AT `t+h`, which ruling A-6 turns into the ordinary return; v2 moves the
  // window so the mark lies STRICTLY inside `(t, t+h)` and every expected value
  // is unchanged. All nine keys are now emitted.
  //
  // Provenance of each key, stated because it matters: `forward_return`,
  // `cell_used`, `t_last_found` and the two counter increments are MEASURED from
  // the engine; `t_last_offset` and `terminal_leg` are DERIVED by inverting
  // §3.8's composition against the measured return (see the two helpers above),
  // so they move if the engine's leg or chosen mark moves;
  // `special_dividend_included` / `special_dividend_applied` are FIXTURE-DECLARED
  // — ruling AR-2's record-date gating lives in the CALLER (§6: `terminal_value`
  // arrives "already gated"), which finding I-5 assigns to T5, so this unit
  // cannot measure them and does not pretend to.
  struct LegCase {
    const char *id;
    atx::usize horizon;
    atx::usize mark_offset; // STRICTLY inside (t, t+h), so A-6's precedence never fires
    atx::i64 session_day;
    atx::f64 price_t;
    atx::f64 terminal_value;
    atx::f64 special_applied;
    atx::usize special_included;
    atx::f64 price_at_last_mark;
    atx::f64 raw_at_last_mark;
  };
  static constexpr LegCase kLeg[5] = {
      // HNZ was already A-6-safe in v1 (mark at offset 1, h = 2) and is unchanged.
      {"f3_terminal_hnz_evidenced_no_special", 2U, 1U, 15859, 72.00, 72.50, 0.0, 0U, 72.49, 72.49},
      // v2: DELL moves to h = 3 with the only mark at offset 2.
      {"f3_terminal_dell_at_record_date_includes_special", 3U, 2U, 16006, 13.90, 13.88, 0.13, 1U,
       13.86, 13.86},
      {"f3_terminal_dell_after_record_date_excludes_special", 3U, 2U, 16007, 13.90, 13.75, 0.0, 0U,
       13.86, 13.86},
      {"f3_terminal_leg_uses_raw_close_not_adjusted_close", 3U, 2U, 16006, 13.90, 13.88, 0.13, 1U,
       13.86 * 1.25, 13.86},
      // v2: MOLX moves to h = 2 with the only mark at offset 1.
      {"f3_terminal_molx_evidenced_zero_leg", 2U, 1U, 16041, 38.50, 38.68, 0.0, 0U, 38.68, 38.68}};
  for (const LegCase &c : kLeg) {
    const atx::usize dates = c.horizon + 1U;
    Panel p = make_terminal_panel(dates);
    for (atx::usize t = 0U; t < dates; ++t) {
      p.session_keys[t] = ns_of_epoch_day(c.session_day + static_cast<atx::i64>(t));
    }
    set_subject(p, 0U, c.price_t, c.price_t, atx::u8{1}, atx::u8{1}, c.terminal_value);
    for (atx::usize t = 1U; t < dates; ++t) {
      set_subject(p, t, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
    }
    set_subject(p, c.mark_offset, c.price_at_last_mark, c.raw_at_last_mark, atx::u8{0}, atx::u8{0},
                0.0);
    const std::vector<atx::usize> horizons{c.horizon};
    const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(horizons));
    const IcDatePoint &pt = out.horizons[0].series[0];
    const atx::f64 measured = out.horizons[0].buckets[0].mean_forward_return;
    const atx::usize t_last = identify_t_last(p, 0U, c.horizon, measured);
    Obj values;
    values.integer("cell_used", (pt.n_used == 3U) ? 1U : 0U)
        .integer("t_last_offset", t_last)
        .integer("t_last_found", (pt.n_terminal_applied == 1U) ? 1U : 0U)
        .integer("special_dividend_included", c.special_included)
        .num("special_dividend_applied", c.special_applied)
        .num("terminal_leg", derive_terminal_leg(p, 0U, t_last, measured))
        .num("forward_return", measured)
        .integer("n_terminal_applied_increment", pt.n_terminal_applied)
        .integer("n_terminal_unevidenced_increment", pt.n_terminal_unevidenced);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", c.horizon);
    emit_measurement(c.id, inputs, values);
  }

  // Oracle v2's new case: a terminal-flagged, EVIDENCED ID that still has a live
  // positive close AT t+h. Ruling A-6 makes it the ordinary return with no leg —
  // the branch the whole v1/v2 re-cut exists to separate. The forbidden overlay
  // value is deliberately NOT emitted: v2 carries it as a non-binding diagnostic
  // and an export of it would read as a competing expectation.
  {
    Panel p = make_terminal_panel(3U);
    set_subject(p, 0U, 13.90, 13.90, atx::u8{1}, atx::u8{1}, 13.75 + 0.13);
    set_subject(p, 1U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
    set_subject(p, 2U, 13.86, 13.86, atx::u8{0}, atx::u8{0}, 0.0);
    const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h2));
    const IcDatePoint &pt = out.horizons[0].series[0];
    Obj values;
    // The two controls are priced at every date by construction, so the subject's
    // own contribution to each counter is the total minus the controls'.
    values.integer("cell_used", (pt.n_used == 3U) ? 1U : 0U)
        .num("forward_return", out.horizons[0].buckets[0].mean_forward_return)
        .integer("terminal_leg_applied", (pt.n_terminal_applied == 1U) ? 1U : 0U)
        .integer("t_last_found", (pt.n_terminal_applied == 1U) ? 1U : 0U)
        .integer("n_with_forward_increment", pt.n_with_forward - 2U)
        .integer("n_terminal_applied_increment", pt.n_terminal_applied)
        .integer("n_terminal_unevidenced_increment", pt.n_terminal_unevidenced)
        .integer("n_dropped_missing_forward_increment", pt.n_dropped_missing_forward);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 2U);
    emit_measurement("f3_terminal_live_close_at_t_plus_h_uses_ordinary_return", inputs, values);
  }

  // The PCS shape: flagged terminal, unevidenced. Dropped and counted.
  {
    Panel p = make_terminal_panel(3U);
    set_subject(p, 0U, 11.00, 11.00, atx::u8{1}, atx::u8{0}, 0.0);
    set_subject(p, 1U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
    set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
    const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h2));
    const IcDatePoint &pt = out.horizons[0].series[0];
    Obj values;
    values.integer("cell_used", (pt.n_used == 3U) ? 1U : 0U)
        .integer("n_terminal_unevidenced_increment", pt.n_terminal_unevidenced)
        .integer("n_terminal_applied_increment", pt.n_terminal_applied)
        .integer("forward_return_defined", (pt.n_used == 3U) ? 1U : 0U);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 2U);
    emit_measurement("f3_terminal_unevidenced_drops_and_counts", inputs, values);
  }

  // No valid mark anywhere in (t, t+h]: the cell has no terminal path either.
  {
    Panel p = make_terminal_panel(3U);
    set_subject(p, 0U, 72.00, 72.00, atx::u8{1}, atx::u8{1}, 72.50);
    set_subject(p, 1U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
    set_subject(p, 2U, kMissingMark, kMissingMark, atx::u8{0}, atx::u8{0}, 0.0);
    const CrossSectionIcResult out = run_or_die(p.view(), terminal_config(h2));
    const IcDatePoint &pt = out.horizons[0].series[0];
    Obj values;
    values.integer("cell_used", (pt.n_used == 3U) ? 1U : 0U)
        .integer("t_last_found", (pt.n_terminal_applied == 1U) ? 1U : 0U)
        .integer("n_dropped_missing_forward_increment", pt.n_dropped_missing_forward)
        .integer("n_terminal_applied_increment", pt.n_terminal_applied)
        .integer("forward_return_defined", (pt.n_used == 3U) ? 1U : 0U);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 2U);
    emit_measurement("f3_terminal_no_valid_mark_in_forward_window_drops", inputs, values);
  }
}

void emit_f4_family() {
  const Panel p = make_f4_panel();
  const std::vector<atx::usize> h{1U, 2U};
  CrossSectionIcConfig base = good_config(h);
  base.quantiles = 2U;
  base.forward_variant = ForwardReturnVariant::IncludeAuditedTerminalV1;

  struct Slot {
    const char *id;
    atx::usize horizon_index;
    atx::usize date;
  };
  static constexpr Slot kFull[3] = {{"f4_counters_t0_h1_full", 0U, 0U},
                                    {"f4_counters_t1_h1_full", 0U, 1U},
                                    {"f4_counters_t0_h2_full", 1U, 0U}};
  static constexpr Slot kEx34[3] = {{"f4_counters_t0_h1_ex34", 0U, 0U},
                                    {"f4_counters_t1_h1_ex34", 0U, 1U},
                                    {"f4_counters_t0_h2_ex34", 1U, 0U}};

  for (atx::usize pass = 0U; pass < 2U; ++pass) {
    CrossSectionIcConfig cfg = base;
    cfg.stream_restriction_id = pass;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const Slot *slots = (pass == 0U) ? kFull : kEx34;
    for (atx::usize k = 0U; k < 3U; ++k) {
      const IcDatePoint &pt = out.horizons[slots[k].horizon_index].series[slots[k].date];
      Obj values;
      values.integer("n_eligible", pt.n_eligible)
          .integer("n_signal_finite", pt.n_signal_finite)
          .integer("n_with_forward", pt.n_with_forward)
          .integer("n_dropped_missing_forward", pt.n_dropped_missing_forward)
          .integer("n_terminal_applied", pt.n_terminal_applied)
          .integer("n_terminal_unevidenced", pt.n_terminal_unevidenced)
          .integer("n_excluded_audited", pt.n_excluded_audited)
          .integer("n_used", pt.n_used);
      Obj inputs = inputs_of(pt.n_used, 0U);
      inputs.integer("horizon", out.horizons[slots[k].horizon_index].horizon)
          .integer("date_index", slots[k].date)
          .integer("restriction_id", pass);
      emit_measurement(slots[k].id, inputs, values);
    }
  }
}

void emit_f5_family() {
  const std::vector<atx::usize> h{1U};
  // The complete-turnover panel also carries the per-date net series.
  {
    const Panel p = make_f5_panel();
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const IcHorizonSummary &sum = out.horizons[0];
    {
      Obj values;
      values.num("decile_one_way_turnover", sum.decile_one_way_turnover)
          .num("oneway_turnover", sum.decile_one_way_turnover)
          .num("sum_abs_weight_change", sum.decile_one_way_turnover * 2.0)
          .num("trade_drag", sum.trade_drag)
          .num("trade_drag_bps", sum.trade_drag * 1e4);
      add_spread_gate(values, sum);
      Obj inputs = inputs_of(sum.full.dates_emitted, 0U);
      inputs.integer("horizon", 1U).integer("quantile_count", 2U);
      emit_measurement("f5_complete_decile_turnover_costs_twenty_bps", inputs, values);
    }
    {
      std::vector<atx::usize> days;
      std::vector<atx::f64> gross;
      std::vector<atx::f64> borrow;
      std::vector<atx::f64> cost;
      std::vector<atx::f64> net;
      for (const IcDatePoint &pt : sum.series) {
        if (pt.spread_emitted == 0U) {
          continue;
        }
        days.push_back(static_cast<atx::usize>(pt.days_forward));
        gross.push_back(pt.spread_gross);
        borrow.push_back(pt.borrow_drag);
        cost.push_back(pt.cost_drag);
        net.push_back(pt.spread_net);
      }
      Obj values;
      values.integer("dates_emitted", gross.size())
          .num("decile_one_way_turnover", sum.decile_one_way_turnover)
          .num("trade_drag", sum.trade_drag)
          .raw("days_forward", jintlist(days))
          .raw("gross", jnumlist(gross))
          .raw("borrow_drag", jnumlist(borrow))
          .raw("cost_drag", jnumlist(cost))
          .raw("net", jnumlist(net))
          .integer("net_series_length_equals_gross", (net.size() == gross.size()) ? 1U : 0U);
      add_spread_gate(values, sum);
      Obj inputs = inputs_of(gross.size(), 0U);
      inputs.integer("horizon", 1U).integer("quantile_count", 2U);
      emit_measurement("f5_net_is_gross_minus_per_date_drag", inputs, values);
    }
  }
  // Half turnover.
  {
    Panel p = make_f5_panel();
    static constexpr atx::f64 kHalf[4] = {4, 1, 3, 0};
    for (atx::usize i = 0U; i < 4U; ++i) {
      p.signal[4U + i] = kHalf[i];
    }
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    Obj values;
    values.num("decile_one_way_turnover", out.horizons[0].decile_one_way_turnover)
        .num("oneway_turnover", out.horizons[0].decile_one_way_turnover)
        .num("sum_abs_weight_change", out.horizons[0].decile_one_way_turnover * 2.0)
        .num("trade_drag", out.horizons[0].trade_drag)
        .num("trade_drag_bps", out.horizons[0].trade_drag * 1e4);
    add_spread_gate(values, out.horizons[0]);
    Obj inputs = inputs_of(out.horizons[0].full.dates_emitted, 0U);
    inputs.integer("horizon", 1U).integer("quantile_count", 2U);
    emit_measurement("f5_half_decile_turnover_costs_ten_bps", inputs, values);
  }
  // The two remaining calendar spans, each on its own two-session panel.
  struct DayCase {
    const char *id;
    atx::i64 start;
    atx::i64 end;
  };
  static constexpr DayCase kDays[2] = {{"f5_days_tuesday_to_wednesday_is_one", kApr09, kApr10},
                                       {"f5_days_holiday_weekend_is_four", kMay24, kMay28}};
  for (const DayCase &c : kDays) {
    Panel p{2U, 2U};
    p.session_keys[0] = ns_of_epoch_day(c.start);
    p.session_keys[1] = ns_of_epoch_day(c.end);
    CrossSectionIcConfig cfg = good_config(h);
    cfg.quantiles = 2U;
    const CrossSectionIcResult out = run_or_die(p.view(), cfg);
    const IcDatePoint &pt = out.horizons[0].series[0];
    Obj values;
    values.integer("days_forward", static_cast<atx::usize>(pt.days_forward))
        .num("borrow_drag", pt.borrow_drag);
    Obj inputs = inputs_of(pt.n_used, 0U);
    inputs.integer("horizon", 1U);
    emit_measurement(c.id, inputs, values);
  }
}

void emit_f6_family() {
  // The two design-named streams. `native_export: "required"` — these two cases
  // cannot be downgraded by the comparator's --optional-case escape hatch.
  static constexpr atx::usize kPrimaryN[2] = {188U, 126U};
  static const char *kIds[2] = {"bootstrap_f6_sample0", "bootstrap_f6_sample1"};
  for (atx::usize sample = 0U; sample < 2U; ++sample) {
    const atx::usize n = kPrimaryN[sample];
    const atx::usize block = detail::block_len(1U, 5U);
    const atx::usize blocks = (n + block - 1U) / block;
    atx::usize fallbacks = 0U;
    atx::u64 key = 0ULL;
    atx::u64 seed_x = 0ULL;
    const std::vector<atx::u64> starts =
        first_draw_starts(static_cast<atx::u64>(sample), n, 8U, fallbacks, key, seed_x);

    // The FULL first draw, so the resample-index head and tail are measured
    // rather than re-derived from the first eight starts.
    atx::usize full_fallbacks = 0U;
    atx::u64 full_key = 0ULL;
    atx::u64 full_seed = 0ULL;
    const std::vector<atx::u64> all_starts = first_draw_starts(
        static_cast<atx::u64>(sample), n, blocks, full_fallbacks, full_key, full_seed);
    EXPECT_EQ(full_key, key);    // the stream is a pure function of its six fields
    EXPECT_EQ(full_seed, seed_x);
    std::vector<atx::usize> resample;
    resample.reserve(n);
    for (const atx::u64 start : all_starts) {
      for (atx::usize k = 0U; k < block && resample.size() < n; ++k) {
        resample.push_back((static_cast<atx::usize>(start) + k) % n);
      }
    }
    std::vector<atx::usize> start_list;
    for (const atx::u64 s : starts) {
      start_list.push_back(static_cast<atx::usize>(s));
    }
    const std::vector<atx::usize> head{resample.begin(), resample.begin() + 16};
    const std::vector<atx::usize> tail{resample.end() - 4, resample.end()};

    Obj values;
    values.raw("stream_key", ju64(key))
        .raw("seed_x", ju64(seed_x))
        .integer("block_len", block)
        .integer("blocks_per_draw", blocks)
        .raw("draw_starts", jintlist(start_list))
        .integer("modulo_fallbacks", fallbacks + full_fallbacks)
        .integer("reportable", detail::series_reportable(n, block, 2000U) ? 1U : 0U)
        .raw("first_draw_resample_indices_head", jintlist(head))
        .raw("first_draw_resample_indices_tail", jintlist(tail))
        .integer("first_draw_resample_length", resample.size());
    Obj inputs = inputs_of(n, sample);
    inputs.integer("statistic_id", static_cast<atx::usize>(BootstrapStatisticId::IcMean))
        .integer("horizon_index", 0U)
        .integer("horizon", 1U)
        .integer("signal_index", 0U)
        .integer("variant_id", 0U)
        .integer("restriction_id", 0U)
        .integer("block_len", block)
        .integer("bootstrap_seed", 20260920U)
        .integer("bootstrap_draws", 2000U);
    emit_measurement(kIds[sample], inputs, values);
  }

  // The frozen stage-1 reportability grid: §4.4 predicts exactly one null
  // horizon, h = 63, in BOTH the full-sample and common-sample columns.
  {
    static constexpr atx::usize kHorizons[5] = {1U, 5U, 10U, 21U, 63U};
    std::vector<atx::usize> null_horizons;
    for (const atx::usize hz : kHorizons) {
      const atx::usize block = detail::block_len(hz, 5U);
      const bool full_ok = detail::series_reportable(189U - hz, block, 2000U);
      const bool common_ok = detail::series_reportable(126U, block, 2000U);
      if (!full_ok || !common_ok) {
        null_horizons.push_back(hz);
      }
    }
    Obj values;
    values.raw("predicted_null_horizons", jintlist(null_horizons));
    Obj inputs = inputs_of(189U, 0U);
    inputs.integer("bootstrap_draws", 2000U).integer("common_sample_dates", 126U);
    emit_measurement("f6_reportability_grid_stage1", inputs, values);
  }
}

TEST(EvalCrossSectionIc, Measurements_OracleFamilies_AreExportedForTheComparator) {
  emit_f1_family();
  emit_primitive_cases();
  emit_f2_family();
  emit_f3_family();
  emit_f4_family();
  emit_f5_family();
  emit_f6_family();
  std::cout.flush();
}

} // namespace atxtest_eval_cross_section_ic
