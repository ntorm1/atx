#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../src/strategy_price_exposures.hpp"

namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace co = atx::core;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kCols = st::kPriceExposureCount;

struct Lcg {
  u64 state{};
  f64 uniform() { // [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11) * 0x1.0p-53;
  }
};
struct Panel {
  usize dates{}, names{};
  std::vector<f64> close, raw, volume;
  std::vector<u8> present;
  Panel(usize d, usize n) : dates(d), names(n), close(d * n, 100.0), raw(d * n, 100.0),
      volume(d * n, 1000.0), present(d * n, 1) {}
  usize at(usize t, usize i) const { return t * names + i; }
  st::PriceExposureInput input() const { return {dates, names, close, raw, volume, present}; }
};
st::PriceExposureConfig small_config() {
  st::PriceExposureConfig cfg;
  cfg.beta_window = 40; cfg.vol_window = 20; cfg.adv_window = 10; cfg.min_return_pairs = 20;
  cfg.min_names = 5;
  return cfg;
}
// A common factor with per-name loadings plus idiosyncratic noise. Coprime strides
// decorrelate the loading, noise-scale and dollar-volume ranks across names.
Panel noisy_panel(usize dates, usize names, u64 seed) {
  Panel p(dates, names);
  Lcg rng{seed};
  for (usize i = 0; i < names; ++i) p.close[p.at(0, i)] = 20.0 + static_cast<f64>(i);
  for (usize t = 1; t < dates; ++t) {
    const f64 common = 0.03 * (rng.uniform() - 0.5);
    for (usize i = 0; i < names; ++i) {
      const f64 loading = 0.4 + 0.15 * static_cast<f64>((i * 5) % names);
      const f64 idio = 0.004 + 0.003 * static_cast<f64>((i * 7) % names);
      p.close[p.at(t, i)] =
          p.close[p.at(t - 1, i)] * (1 + loading * common + idio * (rng.uniform() - 0.5));
    }
  }
  for (usize t = 0; t < dates; ++t)
    for (usize i = 0; i < names; ++i) {
      p.raw[p.at(t, i)] = p.close[p.at(t, i)];
      p.volume[p.at(t, i)] =
          1e4 * static_cast<f64>(1 + (i * 11) % names) * (0.5 + rng.uniform());
    }
  return p;
}
struct Exposures {
  std::vector<f64> out;
  std::vector<u8> ok;
};
Exposures exposures_at(const Panel& p, const st::PriceExposureConfig& cfg, usize d,
                       st::PriceExposureScratch& scratch) {
  Exposures e{std::vector<f64>(p.names * kCols), std::vector<u8>(p.names)};
  const auto status = st::compute_price_exposures(p.input(), cfg, d, scratch, e.out, e.ok);
  EXPECT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  return e;
}
void expect_bits(std::span<const f64> actual, std::span<const f64> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (usize k = 0; k < actual.size(); ++k)
    EXPECT_EQ(std::bit_cast<u64>(actual[k]), std::bit_cast<u64>(expected[k])) << k;
}
void expect_code(const co::Status& status, co::ErrorCode code) {
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), code) << status.error().to_string();
}

// Desired-target shape: demeaned over members, gross 1, nonmembers exactly 0.
void normalize(std::vector<f64>& target, const std::vector<u8>& member) {
  f64 sum = 0, gross = 0;
  usize count = 0;
  for (usize i = 0; i < target.size(); ++i) {
    if (!member[i]) target[i] = 0;
    else { sum += target[i]; ++count; }
  }
  const f64 mean = sum / static_cast<f64>(count);
  for (usize i = 0; i < target.size(); ++i)
    if (member[i]) { target[i] -= mean; gross += std::abs(target[i]); }
  for (usize i = 0; i < target.size(); ++i)
    if (member[i]) target[i] /= gross;
}
struct Section {
  std::vector<f64> target, exposures;
  std::vector<u8> member, ok;
  bool used(usize i) const { return member[i] && ok[i]; }
};
// Uniform exposures (|z| < sqrt(3), so no clip binds) and a target loading on beta.
Section section(usize n, u64 seed) {
  Section s{std::vector<f64>(n), std::vector<f64>(n * kCols), std::vector<u8>(n, 1),
            std::vector<u8>(n, 1)};
  Lcg rng{seed};
  for (usize i = 0; i < n; ++i) {
    s.exposures[i * kCols + st::kExposureBeta] = 0.3 + 1.2 * rng.uniform();
    s.exposures[i * kCols + st::kExposureVol] = 0.01 + 0.03 * rng.uniform();
    s.exposures[i * kCols + st::kExposureLogAdv] = 12.0 + 6.0 * rng.uniform();
  }
  for (usize i = 0; i < n; ++i)
    s.target[i] = s.exposures[i * kCols + st::kExposureBeta] + (rng.uniform() - 0.5);
  normalize(s.target, s.member);
  return s;
}
// The regression design rebuilt from the contract: z over used rows, sample SD, clip.
std::vector<f64> design_z(const Section& s, f64 clip) {
  const usize n = s.target.size();
  std::vector<f64> z(n * kCols, 0.0);
  for (usize k = 0; k < kCols; ++k) {
    f64 sum = 0, squares = 0;
    usize count = 0;
    for (usize i = 0; i < n; ++i)
      if (s.used(i)) { sum += s.exposures[i * kCols + k]; ++count; }
    const f64 mean = sum / static_cast<f64>(count);
    for (usize i = 0; i < n; ++i)
      if (s.used(i)) squares += std::pow(s.exposures[i * kCols + k] - mean, 2);
    const f64 sd = std::sqrt(squares / static_cast<f64>(count - 1));
    for (usize i = 0; i < n; ++i)
      if (s.used(i))
        z[i * kCols + k] = std::clamp((s.exposures[i * kCols + k] - mean) / sd, -clip, clip);
  }
  return z;
}
// [sum w, sum w z_beta, sum w z_vol, sum w z_log_adv] over used rows.
std::array<f64, kCols + 1> moments(const std::vector<f64>& w, const Section& s,
                                   const std::vector<f64>& z) {
  std::array<f64, kCols + 1> m{};
  for (usize i = 0; i < w.size(); ++i) {
    if (!s.used(i)) continue;
    m[0] += w[i];
    for (usize k = 0; k < kCols; ++k) m[k + 1] += w[i] * z[i * kCols + k];
  }
  return m;
}
f64 sample_sd(const std::vector<f64>& xs) {
  f64 sum = 0, squares = 0;
  for (const f64 x : xs) sum += x;
  const f64 mean = sum / static_cast<f64>(xs.size());
  for (const f64 x : xs) squares += (x - mean) * (x - mean);
  return std::sqrt(squares / static_cast<f64>(xs.size() - 1));
}
} // namespace

TEST(StrategyPriceExposures, ExactMarketMultiplesRecoverBetaVolAndDollarVolume) {
  // Returns are exact multiples k of an alternating +-2% factor. mean(k) == 1, so
  // the equal-weight market IS the factor and beta_i == k_i.
  const std::array<f64, 3> k{0.5, 1.0, 1.5};
  const std::array<f64, 3> dollars{2.0e6, 3.5e7, 8.0e8};
  Panel p(61, 3);
  for (usize i = 0; i < p.names; ++i) {
    p.close[p.at(0, i)] = 50.0 + 10.0 * static_cast<f64>(i);
    for (usize t = 1; t < p.dates; ++t)
      p.close[p.at(t, i)] = p.close[p.at(t - 1, i)] * (1 + k[i] * (t % 2 ? -0.02 : 0.02));
    for (usize t = 0; t < p.dates; ++t) {
      p.raw[p.at(t, i)] = 2.0 * p.close[p.at(t, i)];            // constant adjustment
      p.volume[p.at(t, i)] = dollars[i] / p.raw[p.at(t, i)];    // constant dollar volume
    }
  }
  st::PriceExposureScratch scratch;
  const auto e = exposures_at(p, small_config(), 60, scratch);
  for (usize i = 0; i < p.names; ++i) {
    EXPECT_EQ(e.ok[i], 1) << i;
    EXPECT_NEAR(e.out[i * kCols + st::kExposureBeta], k[i], 1e-11) << i;
    // Last 20 intervals: ten each of +-0.02k -> mean 0, sample SD 0.02k sqrt(20/19).
    EXPECT_NEAR(e.out[i * kCols + st::kExposureVol], 0.02 * k[i] * std::sqrt(20.0 / 19.0),
                1e-13) << i;
    EXPECT_NEAR(e.out[i * kCols + st::kExposureLogAdv], std::log(dollars[i]), 1e-12) << i;
  }
}

TEST(StrategyPriceExposures, RewritingEverySessionAfterDecisionLeavesExposuresBitIdentical) {
  Panel p = noisy_panel(80, 8, 11);
  const auto cfg = small_config();
  constexpr usize d = 55;
  st::PriceExposureScratch scratch;
  const auto before = exposures_at(p, cfg, d, scratch);
  for (usize i = 0; i < p.names; ++i) EXPECT_EQ(before.ok[i], 1) << i;
  for (usize t = d + 1; t < p.dates; ++t)
    for (usize i = 0; i < p.names; ++i) {
      const usize k = p.at(t, i);
      p.close[k] = (t + i) % 3 ? 1e9 : missing;
      p.raw[k] = -1;
      p.volume[k] = 1e300;
      p.present[k] = static_cast<u8>((t + i) % 2);
    }
  const auto after = exposures_at(p, cfg, d, scratch);
  expect_bits(after.out, before.out);
  EXPECT_EQ(after.ok, before.ok);
  // Session d itself is inside every window.
  p.close[p.at(d, 0)] *= 1.01;
  p.raw[p.at(d, 0)] *= 1.01;
  const auto moved = exposures_at(p, cfg, d, scratch);
  for (usize k = 0; k < kCols; ++k)
    EXPECT_NE(std::bit_cast<u64>(moved.out[k]), std::bit_cast<u64>(before.out[k])) << k;
}

TEST(StrategyPriceExposures, ScratchReuseAcrossClippedAndFullWindowsIsStateless) {
  const Panel p = noisy_panel(70, 12, 5);
  const auto cfg = small_config();
  st::PriceExposureScratch reused;
  for (const usize d : {usize{0}, usize{3}}) {
    const auto early = exposures_at(p, cfg, d, reused); // windows clipped at panel start
    for (const u8 flag : early.ok) EXPECT_EQ(flag, 0) << d;
  }
  const auto late = exposures_at(p, cfg, 60, reused);
  const auto again = exposures_at(p, cfg, 45, reused);
  st::PriceExposureScratch fresh;
  const auto clean = exposures_at(p, cfg, 45, fresh);
  expect_bits(again.out, clean.out);
  EXPECT_EQ(again.ok, clean.ok);
  EXPECT_EQ(std::count(late.ok.begin(), late.ok.end(), u8{1}), 12);
}

TEST(StrategyPriceExposures, GuardedAndAbsentIntervalsLeaveMarketAndLosePairs) {
  // mean(k) == 1 with or without any single k == 1 name, so the market stays the
  // factor f exactly when an excluded name's interval is (correctly) dropped.
  const std::array<f64, 6> k{0.5, 1.5, 1.0, 1.0, 1.0, 1.0};
  constexpr usize split = 3, crash = 4, gap = 5;               // names
  constexpr usize split_t = 20, crash_t = 25, gap_s = 40;      // interval, interval, session
  const auto f = [](usize t) { return 0.01 * (static_cast<f64>((t * 7) % 11) - 5.0) / 5.0; };
  Panel p(50, 6);
  for (usize i = 0; i < p.names; ++i) {
    p.close[p.at(0, i)] = p.raw[p.at(0, i)] = 40.0 + 5.0 * static_cast<f64>(i);
    for (usize t = 1; t < p.dates; ++t) {
      const f64 g = 1 + k[i] * f(t);
      // Adjusted-only doubling (raw disagrees by log 2 > 0.10) and a corroborated
      // 5x move (|log| > 1.5): both guarded, one interval each.
      const bool split_now = i == split && t == split_t, crash_now = i == crash && t == crash_t;
      const f64 adj_jump = split_now ? 2.0 : crash_now ? 5.0 : 1.0;
      const f64 raw_jump = crash_now ? 5.0 : 1.0;
      p.close[p.at(t, i)] = p.close[p.at(t - 1, i)] * g * adj_jump;
      p.raw[p.at(t, i)] = p.raw[p.at(t - 1, i)] * g * raw_jump;
    }
    for (usize t = 0; t < p.dates; ++t)
      p.volume[p.at(t, i)] = 1e6 * static_cast<f64>(i + 1) / p.raw[p.at(t, i)];
  }
  const usize hole = p.at(gap_s, gap); // absent row: both adjacent intervals invalid
  p.present[hole] = 0;
  p.close[hole] = p.raw[hole] = p.volume[hole] = missing;
  st::PriceExposureConfig cfg;
  cfg.beta_window = 30; cfg.vol_window = 30; cfg.adv_window = 10; cfg.min_return_pairs = 29;
  cfg.min_names = 5;
  constexpr usize d = 45; // return intervals 16..45
  st::PriceExposureScratch scratch;
  const auto e = exposures_at(p, cfg, d, scratch);
  const auto x = [&](usize i, usize col) { return e.out[i * kCols + col]; };
  for (usize i = 0; i < gap; ++i) {
    EXPECT_EQ(e.ok[i], 1) << i; // split and crash lose one pair each: 29 >= 29
    EXPECT_NEAR(x(i, st::kExposureBeta), k[i], 1e-11) << i;
  }
  EXPECT_EQ(e.ok[gap], 0); // the gap loses two pairs: 28 < 29
  EXPECT_TRUE(std::isnan(x(gap, st::kExposureBeta)));
  const auto factor_sd = [&](usize skip_a, usize skip_b) {
    std::vector<f64> xs;
    for (usize t = d + 1 - cfg.vol_window; t <= d; ++t)
      if (t != skip_a && t != skip_b) xs.push_back(f(t));
    return sample_sd(xs);
  };
  EXPECT_NEAR(x(2, st::kExposureVol), factor_sd(0, 0), 1e-13);
  EXPECT_NEAR(x(split, st::kExposureVol), factor_sd(split_t, split_t), 1e-13);
  EXPECT_NEAR(x(crash, st::kExposureVol), factor_sd(crash_t, crash_t), 1e-13);
  EXPECT_NEAR(x(gap, st::kExposureVol), factor_sd(gap_s, gap_s + 1), 1e-13);
  // The absent session contributes zero dollars; the denominator stays adv_window.
  EXPECT_NEAR(x(2, st::kExposureLogAdv), std::log(3e6), 1e-12);
  EXPECT_NEAR(x(gap, st::kExposureLogAdv), std::log(0.9 * 6e6), 1e-12);
}

TEST(StrategyPriceExposures, RejectsBadConfigGeometryDecisionAndPresenceBytes) {
  Panel p = noisy_panel(30, 8, 2);
  const auto cfg = small_config();
  st::PriceExposureScratch scratch;
  std::vector<f64> out(p.names * kCols), short_out(p.names * kCols - 1);
  std::vector<u8> ok(p.names);
  const auto run = [&](const st::PriceExposureConfig& c, usize d, std::span<f64> o) {
    return st::compute_price_exposures(p.input(), c, d, scratch, o, ok);
  };
  EXPECT_TRUE(run(cfg, 29, out));
  expect_code(run(cfg, 30, out), co::ErrorCode::InvalidArgument);
  expect_code(run(cfg, 29, short_out), co::ErrorCode::InvalidArgument);
  auto bad = cfg; bad.min_return_pairs = bad.beta_window + 1;
  expect_code(run(bad, 29, out), co::ErrorCode::InvalidArgument);
  bad = cfg; bad.min_names = st::kPriceExposureCount + 1;
  expect_code(run(bad, 29, out), co::ErrorCode::InvalidArgument);
  bad = cfg; bad.clip_z = 0;
  expect_code(run(bad, 29, out), co::ErrorCode::InvalidArgument);
  bad = cfg; bad.vol_window = 1;
  expect_code(run(bad, 29, out), co::ErrorCode::InvalidArgument);
  // A malformed presence byte is refused only where it is read: never after d.
  p.present[p.at(29, 3)] = 2;
  EXPECT_TRUE(run(cfg, 28, out));
  expect_code(run(cfg, 29, out), co::ErrorCode::InvalidArgument);
}

TEST(StrategyPriceNeutralize, ResidualIsOrthogonalGrossPreservingAndRespectsSupport) {
  constexpr usize n = 90;
  Section s = section(n, 3);
  usize used = 0, excluded = 0;
  for (usize i = 0; i < n; ++i) {
    if (i % 13 == 0) {
      s.member[i] = 0; // nonmember: must stay exactly 0
    } else if (i % 11 == 5) {
      s.ok[i] = 0; // member without exposures: forced to 0
      for (usize k = 0; k < kCols; ++k) s.exposures[i * kCols + k] = missing;
    }
    used += s.used(i) ? 1U : 0U;
    excluded += (s.member[i] && !s.ok[i]) ? 1U : 0U;
  }
  normalize(s.target, s.member);
  st::PriceExposureConfig cfg;
  cfg.min_names = 20;
  st::NeutralizeScratch scratch;
  st::NeutralizeStats stats;
  auto w = s.target;
  const auto status = st::neutralize_target(w, s.member, s.exposures, s.ok, cfg, scratch, stats);
  ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(stats.used, used);
  EXPECT_EQ(stats.excluded, excluded);
  EXPECT_GT(excluded, usize{0});
  EXPECT_NEAR(stats.gross, 1.0, 1e-12);
  f64 gross = 0;
  for (usize i = 0; i < n; ++i) {
    if (!s.used(i)) {
      EXPECT_EQ(w[i], 0.0) << i;
    }
    gross += std::abs(w[i]);
  }
  EXPECT_NEAR(gross, 1.0, 1e-12);
  const auto z = design_z(s, cfg.clip_z);
  const auto before = moments(s.target, s, z), after = moments(w, s, z);
  EXPECT_GT(std::abs(before[1]), 1e-2); // the input really loaded on beta
  for (usize k = 0; k < after.size(); ++k) EXPECT_LE(std::abs(after[k]), 1e-12) << k;
  // Scratch reuse is stateless: a second call reproduces the result bit for bit.
  auto again = s.target;
  st::NeutralizeStats again_stats;
  ASSERT_TRUE(st::neutralize_target(again, s.member, s.exposures, s.ok, cfg, scratch,
                                    again_stats));
  expect_bits(again, w);
}

TEST(StrategyPriceNeutralize, TooFewNamesOrDegenerateExposuresRefuseWithoutMutation) {
  st::PriceExposureConfig cfg;
  cfg.min_names = 10;
  st::NeutralizeScratch scratch;
  const auto refuses = [&](const Section& s, co::ErrorCode code) {
    auto w = s.target;
    st::NeutralizeStats stats;
    expect_code(st::neutralize_target(w, s.member, s.exposures, s.ok, cfg, scratch, stats), code);
    expect_bits(w, s.target);
  };
  Section few = section(12, 1); // 9 usable rows < min_names 10
  few.ok[0] = few.ok[1] = few.ok[2] = 0;
  refuses(few, co::ErrorCode::Unavailable);
  Section collinear = section(40, 2); // vol an exact affine image of beta
  for (usize i = 0; i < 40; ++i)
    collinear.exposures[i * kCols + st::kExposureVol] =
        2 * collinear.exposures[i * kCols + st::kExposureBeta] + 3;
  refuses(collinear, co::ErrorCode::Unavailable);
  Section constant = section(40, 4);
  for (usize i = 0; i < 40; ++i) constant.exposures[i * kCols + st::kExposureLogAdv] = 15.25;
  refuses(constant, co::ErrorCode::Unavailable);
  Section spanned = section(40, 6); // target exactly in the exposure span: no residual
  for (usize i = 0; i < 40; ++i)
    spanned.target[i] = spanned.exposures[i * kCols + st::kExposureBeta];
  normalize(spanned.target, spanned.member);
  refuses(spanned, co::ErrorCode::Unavailable);
  Section dirty = section(40, 8); // nonmember carrying weight
  dirty.member[3] = 0;
  refuses(dirty, co::ErrorCode::InvalidArgument);
  Section short_ok = section(40, 9);
  short_ok.ok.pop_back();
  refuses(short_ok, co::ErrorCode::InvalidArgument);
}

TEST(StrategyPriceNeutralize, OneCallStepMatchesPrimitivesAndFlatTargetStaysFlat) {
  const Panel p = noisy_panel(70, 12, 21);
  const auto cfg = small_config();
  constexpr usize d = 60;
  std::vector<u8> member(p.names, 1);
  member[4] = 0;
  std::vector<f64> target(p.names);
  Lcg rng{9};
  for (auto& v : target) v = rng.uniform() - 0.5;
  normalize(target, member);
  st::PriceExposureScratch exposure_scratch;
  std::vector<f64> exposures(p.names * kCols);
  std::vector<u8> ok(p.names);
  ASSERT_TRUE(st::compute_price_exposures(p.input(), cfg, d, exposure_scratch, exposures, ok));
  auto expected = target;
  st::NeutralizeScratch neutralize_scratch;
  st::NeutralizeStats expected_stats;
  ASSERT_TRUE(st::neutralize_target(expected, member, exposures, ok, cfg, neutralize_scratch,
                                    expected_stats));
  EXPECT_EQ(expected_stats.used, usize{11});
  st::PriceRiskScratch scratch;
  for (int pass = 0; pass < 2; ++pass) { // second pass reuses grown scratch
    auto w = target;
    st::NeutralizeStats stats;
    ASSERT_TRUE(st::neutralize_price_risk(p.input(), cfg, d, w, member, scratch, stats));
    expect_bits(w, expected);
    EXPECT_EQ(stats.used, expected_stats.used);
    EXPECT_EQ(std::bit_cast<u64>(stats.residual_gross),
              std::bit_cast<u64>(expected_stats.residual_gross));
  }
  std::vector<f64> flat(p.names, 0.0);
  st::NeutralizeStats flat_stats;
  ASSERT_TRUE(st::neutralize_price_risk(p.input(), cfg, d, flat, member, scratch, flat_stats));
  for (const f64 v : flat) EXPECT_EQ(v, 0.0);
  EXPECT_EQ(flat_stats.gross, 0.0);
  EXPECT_EQ(flat_stats.used, usize{11});
}
