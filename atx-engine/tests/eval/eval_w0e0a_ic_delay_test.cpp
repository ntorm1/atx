// atx-engine/tests/eval/eval_w0e0a_ic_delay_test.cpp
//
// W0-E0a / E-09: `CrossSectionIcConfig::execution_delay`. The forward return used to start
// at the signal close t while every book trades at t + 1; with the delay (default 1) the
// return runs from the entry row t + d to t + d + h and the label embargo is h + d.
//
// Acceptance: "A same-day reversal signal's IC collapses at delay 1."
// Synthetic panels only.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"

namespace atx_test_w0_e0a_ic_delay {

using namespace atx::engine::eval;
using atx::core::ErrorCode;

constexpr atx::i64 kBaseNs = 1'357'084'800'000'000'000; // 2013-01-02T00:00:00Z
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Panel {
  std::size_t dates{};
  std::size_t instruments{};
  std::vector<double> signal, price, raw_price, terminal_value;
  std::vector<atx::u8> mask, terminal, terminal_evidenced, excluded_audited;
  std::vector<atx::i64> session_keys;

  Panel(std::size_t d, std::size_t i) : dates{d}, instruments{i} {
    const std::size_t cells = d * i;
    signal.assign(cells, 0.0);
    price.assign(cells, 100.0);
    raw_price.assign(cells, 100.0);
    terminal_value.assign(cells, 0.0);
    mask.assign(cells, atx::u8{1});
    terminal.assign(cells, atx::u8{0});
    terminal_evidenced.assign(cells, atx::u8{0});
    excluded_audited.assign(cells, atx::u8{0});
    session_keys.resize(d);
    for (std::size_t t = 0U; t < d; ++t) {
      session_keys[t] = kBaseNs + kNanosPerDay * static_cast<atx::i64>(t);
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
  [[nodiscard]] std::size_t at(std::size_t t, std::size_t i) const { return t * instruments + i; }
};

[[nodiscard]] CrossSectionIcConfig config(std::span<const std::size_t> horizons,
                                          std::size_t delay) {
  CrossSectionIcConfig cfg{};
  cfg.horizons = horizons;
  cfg.quantiles = 2U;
  cfg.bootstrap_draws = 64U;
  cfg.bootstrap_seed = 5U;
  cfg.forward_variant = ForwardReturnVariant::DropMissingForward;
  cfg.ties = IcTieHandling::AverageRanksV1;
  cfg.execution_delay = delay;
  return cfg;
}

[[nodiscard]] CrossSectionIcResult run(const Panel &p, const CrossSectionIcConfig &cfg) {
  auto scratch = plan_cross_section_ic(p.view(), cfg);
  if (!scratch.has_value()) {
    ADD_FAILURE() << "plan: " << scratch.error().message();
    return CrossSectionIcResult{};
  }
  auto res = compute_cross_section_ic(p.view(), cfg, *scratch);
  if (!res.has_value()) {
    ADD_FAILURE() << "compute: " << res.error().message();
    return CrossSectionIcResult{};
  }
  return std::move(*res);
}

// ===========================================================================
//  Acceptance: the same-day reversal.
//
//  Daily return into row t is r_t = 0.01 (e_t - 0.8 e_{t-1}): today's shock partly
//  reverses tomorrow and not after. The signal at row t is the reversal bet -r_t, computed
//  from the close at t. Trading AT that close (delay 0) earns r_{t+1}, which the signal
//  predicts (population IC 0.8 / 1.64 = 0.488); a book that enters at the next close
//  (delay 1) earns r_{t+2}, which is independent of the signal: the edge is untradeable.
// ===========================================================================
TEST(EvalIcDelay, SameDayReversal_IcCollapsesAtDelayOne) {
  constexpr std::size_t kT = 300U;
  constexpr std::size_t kI = 200U;
  Panel p{kT, kI};
  atx::core::Xoshiro256pp rng{0xDE1A'7ULL};
  std::vector<double> prev_e(kI, 0.0);
  std::vector<double> level(kI, 50.0);
  for (std::size_t t = 0U; t < kT; ++t) {
    for (std::size_t i = 0U; i < kI; ++i) {
      const double e = rng.normal();
      const double r = 0.01 * (e - 0.8 * prev_e[i]);
      prev_e[i] = e;
      level[i] *= 1.0 + r;
      p.price[p.at(t, i)] = level[i];
      p.raw_price[p.at(t, i)] = level[i];
      p.signal[p.at(t, i)] = -r;
    }
  }
  const std::vector<std::size_t> h{1U};
  const CrossSectionIcResult d0 = run(p, config(h, 0U));
  const CrossSectionIcResult d1 = run(p, config(h, 1U));
  ASSERT_EQ(d0.horizons.size(), 1U);
  ASSERT_EQ(d1.horizons.size(), 1U);
  const IcSampleStats &s0 = d0.horizons[0].full;
  const IcSampleStats &s1 = d1.horizons[0].full;
  std::printf("[EvalIcDelay] same-day reversal, %zu dates x %zu names, h=1: delay 0 IC %.4f "
              "(HAC t %.2f, rank IC %.4f)  delay 1 IC %.4f (HAC t %.2f, rank IC %.4f)\n",
              kT, kI, s0.ic_mean, s0.ic_mean_hac.t, s0.rank_ic_mean, s1.ic_mean,
              s1.ic_mean_hac.t, s1.rank_ic_mean);
  // Signal-close convention: a large, significant edge.
  EXPECT_GT(s0.ic_mean, 0.40);
  EXPECT_GT(s0.ic_mean_hac.t, 20.0);
  // Next-close entry: the edge is gone (|IC| under 0.02, HAC t insignificant).
  EXPECT_LT(std::fabs(s1.ic_mean), 0.02);
  EXPECT_LT(std::fabs(s1.rank_ic_mean), 0.02);
  ASSERT_EQ(s1.ic_mean_hac.reportable, 1U);
  EXPECT_LT(std::fabs(s1.ic_mean_hac.t), 3.0);
  // Collapse ratio.
  EXPECT_LT(std::fabs(s1.ic_mean), 0.05 * s0.ic_mean);
}

// ===========================================================================
//  Semantics.
// ===========================================================================
TEST(EvalIcDelay, DefaultsAndEmbargo) {
  const CrossSectionIcConfig cfg{};
  EXPECT_EQ(cfg.execution_delay, 1U);
  EXPECT_EQ(label_embargo(21U, 1U), 22U);
  EXPECT_EQ(label_embargo(5U, 0U), 5U);
  Panel p{40U, 3U};
  for (std::size_t c = 0U; c < p.signal.size(); ++c) {
    p.signal[c] = static_cast<double>(c % 3U);
  }
  const std::vector<std::size_t> h{1U, 5U};
  for (const std::size_t d : {0U, 1U, 3U}) {
    const CrossSectionIcResult r = run(p, config(h, d));
    ASSERT_EQ(r.horizons.size(), 2U);
    for (const IcHorizonSummary &hs : r.horizons) {
      EXPECT_EQ(hs.execution_delay, d);
      EXPECT_EQ(hs.embargo, hs.horizon + d);
      // Evaluable signal rows: T - h - d (every one has an entry and an exit row).
      EXPECT_EQ(hs.series.size(), p.dates - hs.horizon - d);
    }
  }
}

TEST(EvalIcDelay, PriceGapAtTheEntryRowDropsTheNameOnTheRightSignalDate) {
  Panel p{20U, 3U};
  for (std::size_t t = 0U; t < p.dates; ++t) {
    for (std::size_t i = 0U; i < p.instruments; ++i) {
      p.signal[p.at(t, i)] = static_cast<double>(i);
      p.price[p.at(t, i)] = 100.0 + static_cast<double>(t * (i + 1U));
    }
  }
  p.price[p.at(5U, 0U)] = kNaN; // one missing close at row 5
  const std::vector<std::size_t> h{1U};
  const CrossSectionIcResult d0 = run(p, config(h, 0U));
  const CrossSectionIcResult d1 = run(p, config(h, 1U));
  ASSERT_EQ(d0.horizons.size(), 1U);
  ASSERT_EQ(d1.horizons.size(), 1U);
  const auto &s0 = d0.horizons[0].series;
  const auto &s1 = d1.horizons[0].series;
  // Delay 0: row 5 is the entry of signal date 5 and the exit of signal date 4.
  EXPECT_EQ(s0[4].n_used, 2U);
  EXPECT_EQ(s0[5].n_used, 2U);
  EXPECT_EQ(s0[3].n_used, 3U);
  EXPECT_EQ(s0[6].n_used, 3U);
  // Delay 1: row 5 is the entry of signal date 4 and the exit of signal date 3.
  EXPECT_EQ(s1[3].n_used, 2U);
  EXPECT_EQ(s1[4].n_used, 2U);
  EXPECT_EQ(s1[5].n_used, 3U);
  EXPECT_EQ(s1[2].n_used, 3U);
}

TEST(EvalIcDelay, DaysForwardSpansTheHoldingWindow) {
  Panel p{12U, 3U};
  // A weekend after row 2: rows 0..2 are consecutive days, row 3 is three days after row 2.
  for (std::size_t t = 0U; t < p.dates; ++t) {
    const atx::i64 day = static_cast<atx::i64>(t) + ((t >= 3U) ? 2 : 0);
    p.session_keys[t] = kBaseNs + kNanosPerDay * day;
  }
  for (std::size_t c = 0U; c < p.signal.size(); ++c) {
    p.signal[c] = static_cast<double>(c % 3U);
  }
  const std::vector<std::size_t> h{1U};
  const CrossSectionIcResult d0 = run(p, config(h, 0U));
  const CrossSectionIcResult d1 = run(p, config(h, 1U));
  ASSERT_EQ(d0.horizons.size(), 1U);
  ASSERT_EQ(d1.horizons.size(), 1U);
  EXPECT_EQ(d0.horizons[0].series[2].days_forward, 3); // row 2 -> row 3
  EXPECT_EQ(d0.horizons[0].series[1].days_forward, 1);
  EXPECT_EQ(d1.horizons[0].series[1].days_forward, 3); // entry row 2 -> exit row 3
  EXPECT_EQ(d1.horizons[0].series[2].days_forward, 1); // entry row 3 -> exit row 4
}

TEST(EvalIcDelay, TerminalTripleIsReadAtTheEntryRow) {
  Panel p{14U, 4U};
  for (std::size_t t = 0U; t < p.dates; ++t) {
    for (std::size_t i = 0U; i < p.instruments; ++i) {
      p.signal[p.at(t, i)] = static_cast<double>(i);
      p.price[p.at(t, i)] = 100.0 + static_cast<double>(t + i);
      p.raw_price[p.at(t, i)] = 100.0 + static_cast<double>(t + i);
    }
  }
  // Instrument 1 delists after row 8; its audited terminal coverage is stamped on row 7.
  for (std::size_t t = 9U; t < p.dates; ++t) {
    p.price[p.at(t, 1U)] = kNaN;
    p.raw_price[p.at(t, 1U)] = kNaN;
  }
  p.terminal[p.at(7U, 1U)] = atx::u8{1};
  p.terminal_evidenced[p.at(7U, 1U)] = atx::u8{1};
  p.terminal_value[p.at(7U, 1U)] = 90.0;
  const std::vector<std::size_t> h{2U};
  CrossSectionIcConfig c0 = config(h, 0U);
  c0.forward_variant = ForwardReturnVariant::IncludeAuditedTerminalV1;
  CrossSectionIcConfig c1 = c0;
  c1.execution_delay = 1U;
  const CrossSectionIcResult d0 = run(p, c0);
  const CrossSectionIcResult d1 = run(p, c1);
  ASSERT_EQ(d0.horizons.size(), 1U);
  ASSERT_EQ(d1.horizons.size(), 1U);
  // Delay 0: the window (7, 9] of signal date 7 needs the terminal leg.
  EXPECT_EQ(d0.horizons[0].series[7].n_terminal_applied, 1U);
  EXPECT_EQ(d0.horizons[0].series[6].n_terminal_applied, 0U);
  // Delay 1: the same window (7, 9] belongs to signal date 6 (entry row 7).
  EXPECT_EQ(d1.horizons[0].series[6].n_terminal_applied, 1U);
  EXPECT_EQ(d1.horizons[0].series[7].n_terminal_applied, 0U);
}

TEST(EvalIcDelay, Validation_DelayBoundsAndHorizonPlusDelayPastDates) {
  const Panel p{30U, 4U};
  const auto code = [&p](const CrossSectionIcConfig &cfg) {
    auto planned = plan_cross_section_ic(p.view(), cfg);
    return planned.has_value() ? ErrorCode::Unknown : planned.error().code();
  };
  const std::vector<std::size_t> h29{29U};
  EXPECT_EQ(code(config(h29, 0U)), ErrorCode::Unknown);    // 29 + 0 < 30
  EXPECT_EQ(code(config(h29, 1U)), ErrorCode::OutOfRange); // 29 + 1 == 30
  const std::vector<std::size_t> h1{1U};
  EXPECT_EQ(code(config(h1, kMaxIcExecutionDelay + 1U)), ErrorCode::InvalidArgument);
  EXPECT_EQ(code(config(h1, 28U)), ErrorCode::Unknown); // 1 + 28 < 30
}

} // namespace atx_test_w0_e0a_ic_delay
