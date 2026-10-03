// combine_char_cov_hedge_test.cpp -- the characteristic-covariance hedge (P9 lane AL-COMB, the
// kernel of composition theme-cov-hedge-v1 in atx-impl): two closed forms, the window sums against
// a two-pass recomputation, the clock (no future session is read; the last known one is) and the
// refusals.
//
// Suite: CharCovHedge

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/char_cov_hedge.hpp"

namespace atx_test_p9_char_cov_hedge {

using atx::f64;
using atx::u64;
using atx::usize;
namespace cb = atx::engine::combine;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

u64 bits(f64 x) { return std::bit_cast<u64>(x); }

// A configuration the small fixtures fill: windows of 8 decisions, lag 2, a block every 4, 4 pairs.
// (The rule's registered 252 / 2 / 21 / 126 are atx-impl's; the kernel takes any valid config.)
cb::CharCovHedgeConfig small_config() {
  cb::CharCovHedgeConfig cfg;
  cfg.lookback = 8;
  cfg.lag = 2;
  cfg.step = 4;
  cfg.min_pairs = 4;
  return cfg;
}

// The common factor of session s: a non-periodic-looking path of multiples of 1/64, so every sum
// and product below is exact in binary.
f64 factor(usize s) {
  constexpr int pattern[15] = {1, -2, 3, -1, 2, -3, 1, 2, -1, -2, 3, 1, -3, 2, -1};
  return static_cast<f64>(pattern[s % 15U]) / 64.0;
}

struct Run {
  std::vector<f64> out, sleeve, hedge, ratio, beta;
};

Run run(const std::vector<f64>& score, const std::vector<f64>& returns, usize names,
        const cb::CharCovHedgeConfig& cfg) {
  const usize dates = score.size() / names;
  Run r{std::vector<f64>(score.size(), 7.0), std::vector<f64>(dates), std::vector<f64>(dates),
        std::vector<f64>(dates), std::vector<f64>(score.size())};
  cb::CharCovHedgeScratch scratch;
  const auto status = cb::char_cov_hedge(score, returns, names, cfg, r.out, scratch,
                                         {r.sleeve, r.hedge, r.ratio, r.beta});
  EXPECT_TRUE(status) << (status ? "" : status.error().to_string());
  return r;
}

// Deterministic uniform values in [-1, 1).
struct Lcg {
  u64 state;
  f64 next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) / static_cast<f64>(1ULL << 52) - 1.0;
  }
};

// A random panel: each name's return loads on a common factor with its own loading plus noise;
// about one cell in ten has no score, and about one in ten no valid return.
void random_panel(usize names, usize dates, u64 seed, std::vector<f64>& score,
                  std::vector<f64>& returns) {
  Lcg g{seed};
  std::vector<f64> loading(names);
  for (auto& b : loading) b = 1.0 + g.next();
  score.assign(names * dates, kNaN);
  returns.assign(names * dates, kNaN);
  for (usize d = 0; d < dates; ++d) {
    const f64 f = .01 * g.next();
    for (usize i = 0; i < names; ++i) {
      const f64 u = g.next(), v = g.next(), w = g.next();
      if (u > -.8) score[d * names + i] = v + .3 * loading[i];
      if (d > 0 && w > -.8) returns[d * names + i] = loading[i] * f + .005 * g.next();
    }
  }
}

// Two names: R = (+.5, -.5) every date, returns a_s and -a_s, so the sleeve is a_{d+2} exactly and
// the loadings are +1 and -1 exactly. Two names always lie in the span of [1, R]: beta_perp is 0,
// there is no hedge leg, the hedge ratio stays 0 and the output is the rank itself, bit for bit.
TEST(CharCovHedge, ClosedForm2Names) {
  constexpr usize names = 2, dates = 24;
  std::vector<f64> score(names * dates), returns(names * dates, kNaN);
  for (usize d = 0; d < dates; ++d) {
    score[d * names] = 1.0;
    score[d * names + 1] = 0.0;
  }
  for (usize s = 1; s < dates; ++s) {
    returns[s * names] = factor(s);
    returns[s * names + 1] = -factor(s);
  }
  const auto r = run(score, returns, names, small_config());
  for (usize d = 0; d < dates; ++d) {
    SCOPED_TRACE(d);
    if (d + 2 < dates)
      EXPECT_EQ(bits(r.sleeve[d]), bits(factor(d + 2)));
    else
      EXPECT_TRUE(std::isnan(r.sleeve[d]));
    EXPECT_TRUE(std::isnan(r.hedge[d]));
    EXPECT_EQ(bits(r.ratio[d]), bits(0.0));
    EXPECT_EQ(bits(r.out[d * names]), bits(0.5));
    EXPECT_EQ(bits(r.out[d * names + 1]), bits(-0.5));
    // Pairs of decisions 0 .. d - 2 are known at d: the first 4 by d = 5.
    if (d >= 5) {
      EXPECT_EQ(r.beta[d * names], 1.0);
      EXPECT_EQ(r.beta[d * names + 1], -1.0);
    } else {
      EXPECT_TRUE(std::isnan(r.beta[d * names]) && std::isnan(r.beta[d * names + 1]));
    }
  }
}

// Five names ranked A < B < C < D < E (R = -.5, -.25, 0, .25, .5); returns k a_s with k = 1, 3, 2
// for A, B, D, none for C and E. Then s = -.75 a, beta = k / -.75 = (-4/3, -4, -8/3); their
// residual on [1, R] over A, B, D is c (1, -1.5, .5), c > 0, so H = (.5, -.5, 0, 0, 0) and g = .5 a
// - 1.5 a = -a; h = cov(s, g) / var(g) = .75 and out = R - .75 H = (-.875, .125, 0, .25, .5), whose
// return -.875 a + .375 a + .5 a is 0: the hedge removes the sleeve's whole common variation.
// Loadings exist from decision 5 (four pairs); the first block (decision 9) has three hedge pairs,
// the second (13) seven: before 13 the output is the rank bit for bit.
TEST(CharCovHedge, ClosedFormFiveNames) {
  constexpr usize names = 5, dates = 24;
  std::vector<f64> score(names * dates), returns(names * dates, kNaN);
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i) score[d * names + i] = static_cast<f64>(i);
  for (usize s = 1; s < dates; ++s) {
    returns[s * names + 0] = 1.0 * factor(s);
    returns[s * names + 1] = 3.0 * factor(s);
    returns[s * names + 3] = 2.0 * factor(s);
  }
  const auto r = run(score, returns, names, small_config());
  const std::vector<f64> rank{-.5, -.25, 0.0, .25, .5}, hedged{-.875, .125, 0.0, .25, .5};
  const std::vector<f64> beta{-4.0 / 3.0, -4.0, kNaN, -8.0 / 3.0, kNaN};
  for (usize d = 0; d < dates; ++d) {
    SCOPED_TRACE(d);
    if (d + 2 < dates) EXPECT_EQ(bits(r.sleeve[d]), bits(-.75 * factor(d + 2)));
    if (d >= 5 && d + 2 < dates)
      EXPECT_EQ(bits(r.hedge[d]), bits(-factor(d + 2)));
    else
      EXPECT_TRUE(std::isnan(r.hedge[d]));
    for (usize i = 0; i < names; ++i) {
      const f64 b = r.beta[d * names + i];
      if (d < 5 || std::isnan(beta[i]))
        EXPECT_TRUE(std::isnan(b)) << i;
      else
        EXPECT_NEAR(b, beta[i], 1e-12) << i;
    }
    if (d < 13) {
      EXPECT_EQ(bits(r.ratio[d]), bits(0.0));
      for (usize i = 0; i < names; ++i) EXPECT_EQ(bits(r.out[d * names + i]), bits(rank[i])) << i;
      continue;
    }
    EXPECT_NEAR(r.ratio[d], .75, 1e-12);
    for (usize i = 0; i < names; ++i) EXPECT_NEAR(r.out[d * names + i], hedged[i], 1e-12) << i;
    if (d + 2 >= dates) continue;
    f64 book = 0.0; // the hedged book's return, earned at d + 2
    for (usize i = 0; i < names; ++i) {
      const f64 ret = returns[(d + 2) * names + i];
      if (std::isfinite(ret)) book += r.out[d * names + i] * ret;
    }
    EXPECT_NEAR(book, 0.0, 1e-12);
  }
}

// The rolling window sums are the window's own: every loading the kernel used at d is the two-pass
// OLS slope over exactly the pairs of decisions d - lag - lookback + 1 .. d - lag (at least
// min_pairs), and every block's hedge ratio the two-pass slope of the sleeve on the hedge return
// over the same window (an off-by-one in the pair that leaves would show here).
TEST(CharCovHedge, WindowSumsAreTheTrailingWindowOls) {
  constexpr usize names = 9, dates = 60;
  const auto cfg = small_config();
  std::vector<f64> score, returns;
  random_panel(names, dates, 11U, score, returns);
  const auto r = run(score, returns, names, cfg);
  const auto ols = [](const std::vector<f64>& x, const std::vector<f64>& y) {
    f64 mx = 0, my = 0;
    for (usize k = 0; k < x.size(); ++k) { mx += x[k]; my += y[k]; }
    mx /= static_cast<f64>(x.size());
    my /= static_cast<f64>(x.size());
    f64 sxx = 0, sxy = 0;
    for (usize k = 0; k < x.size(); ++k) {
      sxx += (x[k] - mx) * (x[k] - mx);
      sxy += (x[k] - mx) * (y[k] - my);
    }
    return sxy / sxx;
  };
  usize checked = 0, blocks = 0;
  for (usize d = cfg.lag; d < dates; ++d) {
    const usize last = d - cfg.lag, first = last + 1 >= cfg.lookback ? last + 1 - cfg.lookback : 0;
    for (usize i = 0; i < names; ++i) {
      std::vector<f64> x, y;
      for (usize p = first; p <= last; ++p) {
        const f64 ret = returns[(p + cfg.lag) * names + i];
        if (!std::isfinite(r.sleeve[p]) || !std::isfinite(ret)) continue;
        x.push_back(r.sleeve[p]);
        y.push_back(ret);
      }
      const f64 b = r.beta[d * names + i];
      if (std::isnan(score[d * names + i]) || x.size() < cfg.min_pairs) {
        EXPECT_TRUE(std::isnan(b)) << d << ' ' << i;
        continue;
      }
      EXPECT_NEAR(b, ols(x, y), 1e-9 * (1.0 + std::abs(b))) << d << ' ' << i;
      ++checked;
    }
    if (d + 1 >= cfg.lookback + cfg.lag && (d + 1 - cfg.lookback - cfg.lag) % cfg.step == 0) {
      std::vector<f64> g, s;
      for (usize p = first; p <= last; ++p) {
        if (!std::isfinite(r.sleeve[p]) || !std::isfinite(r.hedge[p])) continue;
        g.push_back(r.hedge[p]);
        s.push_back(r.sleeve[p]);
      }
      const f64 want = g.size() >= cfg.min_pairs ? ols(g, s) : 0.0;
      EXPECT_NEAR(r.ratio[d], want, 1e-9 * (1.0 + std::abs(want))) << d;
      blocks += g.size() >= cfg.min_pairs ? 1U : 0U;
    }
  }
  EXPECT_GT(checked, 200U);
  EXPECT_GE(blocks, 8U);
}

// The clock. Perturbing every session after d0 (returns and scores) leaves every output, loading
// and ratio up to d0 and every sleeve and hedge return known by d0 bit for bit; perturbing session
// d0's returns alone changes the output at d0 (the pair of decision d0 - 2 enters at d0, and d0 is
// a block start), while nothing before d0 moves: the probe has teeth.
TEST(CharCovHedge, FutureReturnDoesNotChangePast) {
  constexpr usize names = 7, dates = 40, d0 = 25; // 25 = 9 + 4 x 4: a block start
  const auto cfg = small_config();
  std::vector<f64> score, returns;
  random_panel(names, dates, 5U, score, returns);
  const auto base = run(score, returns, names, cfg);
  ASSERT_NE(base.ratio[d0], 0.0);
  auto later_score = score, later_returns = returns;
  for (usize k = (d0 + 1) * names; k < later_score.size(); ++k) {
    later_score[k] = -3.0 * later_score[k] + 1.0;
    later_returns[k] = std::isfinite(later_returns[k]) ? 5.0 * later_returns[k] - .01 : .02;
  }
  const auto later = run(later_score, later_returns, names, cfg);
  for (usize d = 0; d <= d0; ++d) {
    SCOPED_TRACE(d);
    EXPECT_EQ(bits(later.ratio[d]), bits(base.ratio[d]));
    for (usize i = 0; i < names; ++i) {
      EXPECT_EQ(bits(later.out[d * names + i]), bits(base.out[d * names + i])) << i;
      EXPECT_EQ(bits(later.beta[d * names + i]), bits(base.beta[d * names + i])) << i;
    }
    if (d + cfg.lag <= d0) {
      EXPECT_EQ(bits(later.sleeve[d]), bits(base.sleeve[d]));
      EXPECT_EQ(bits(later.hedge[d]), bits(base.hedge[d]));
    }
  }
  auto now_returns = returns;
  for (usize i = 0; i < names; ++i) {
    f64& cell = now_returns[d0 * names + i];
    cell = std::isfinite(cell) ? -2.0 * cell : .01;
  }
  const auto now = run(score, now_returns, names, cfg);
  for (usize k = 0; k < d0 * names; ++k) EXPECT_EQ(bits(now.out[k]), bits(base.out[k])) << k;
  bool moved = false;
  for (usize i = 0; i < names; ++i)
    moved = moved || bits(now.out[d0 * names + i]) != bits(base.out[d0 * names + i]);
  EXPECT_TRUE(moved);
}

// Refusals write nothing; the scratch byte count follows its formula.
TEST(CharCovHedge, RefusesBadInputAndCountsScratch) {
  constexpr usize names = 3, dates = 12;
  std::vector<f64> score(names * dates, 0.0), returns(names * dates, 0.0), out(names * dates, 7.0);
  std::vector<f64> short_trace(dates - 1);
  cb::CharCovHedgeScratch scratch;
  const auto cfg = small_config();
  const auto refused = [&](std::span<const f64> s, std::span<const f64> r, usize n,
                           const cb::CharCovHedgeConfig& c, std::span<f64> o,
                           cb::CharCovHedgeTrace t) {
    const auto status = cb::char_cov_hedge(s, r, n, c, o, scratch, t);
    return !status && status.error().code() == atx::core::ErrorCode::InvalidArgument;
  };
  EXPECT_TRUE(refused(score, returns, 0, cfg, out, {}));
  EXPECT_TRUE(refused(score, std::span<const f64>(returns).first(names), names, cfg, out, {}));
  EXPECT_TRUE(refused(score, returns, names, cfg, std::span<f64>(out).first(names), {}));
  EXPECT_TRUE(refused(score, returns, 5, cfg, out, {}));
  auto bad = cfg;
  bad.lag = 0;
  EXPECT_TRUE(refused(score, returns, names, bad, out, {}));
  bad = cfg;
  bad.min_pairs = cfg.lookback + 1;
  EXPECT_TRUE(refused(score, returns, names, bad, out, {}));
  bad = cfg;
  bad.step = 0;
  EXPECT_TRUE(refused(score, returns, names, bad, out, {}));
  bad = cfg;
  bad.lookback = 1;
  bad.min_pairs = 1;
  EXPECT_TRUE(refused(score, returns, names, bad, out, {}));
  EXPECT_TRUE(refused(score, returns, names, cfg, out, {short_trace, {}, {}, {}}));
  EXPECT_TRUE(refused(score, returns, names, cfg, out, {{}, {}, {}, short_trace}));
  for (const f64 x : out) EXPECT_EQ(x, 7.0);
  const auto bytes = cb::char_cov_hedge_scratch_bytes(dates, names, cfg);
  ASSERT_TRUE(bytes);
  const u64 want = 3U * 2U * names * sizeof(f64) + 3U * 2U + dates * 2U * sizeof(f64) +
                   names * (7U * sizeof(f64) + 2U * sizeof(usize) + sizeof(cb::RankedName));
  EXPECT_EQ(*bytes, want);
  EXPECT_FALSE(cb::char_cov_hedge_scratch_bytes(0, names, cfg));
  bad = cfg;
  bad.lag = 0;
  EXPECT_FALSE(cb::char_cov_hedge_scratch_bytes(dates, names, bad));
  // A flat, all-zero panel is accepted: no rank varies the hedge, nothing is estimated.
  ASSERT_TRUE(cb::char_cov_hedge(score, returns, names, cfg, out, scratch));
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i) EXPECT_EQ(out[d * names + i], 0.0);
}

} // namespace atx_test_p9_char_cov_hedge
