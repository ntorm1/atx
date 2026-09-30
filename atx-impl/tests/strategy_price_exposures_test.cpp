#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/core/sha256.hpp"
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

// ---- v6 C2: industry (within-groups) neutralization ----
namespace {
// section(n, seed) whose target also loads on its groups: shift[i] is added to the
// desired-target shape before it is renormalized (demeaned, gross 1).
Section loaded_section(usize n, u64 seed, const std::vector<f64>& shift) {
  Section s = section(n, seed);
  for (usize i = 0; i < n; ++i) s.target[i] += shift[i];
  normalize(s.target, s.member);
  return s;
}
// Sum of w over the used rows whose id is `id` (NaN: the unknown ids).
f64 group_sum(const std::vector<f64>& w, const Section& s, const std::vector<f64>& ids, f64 id) {
  f64 sum = 0;
  for (usize i = 0; i < w.size(); ++i) {
    const bool same = std::isnan(id) ? std::isnan(ids[i]) : ids[i] == id;
    if (s.used(i) && same) sum += w[i];
  }
  return sum;
}
f64 gross_of(const std::vector<f64>& w) {
  f64 gross = 0;
  for (const f64 v : w) gross += std::abs(v);
  return gross;
}
} // namespace

// (a) price-risk-v1 bytes are unchanged by the within-groups work. The fixture is
// ResidualIsOrthogonalGrossPreservingAndRespectsSupport's. The pinned values come from
// the base-commit (04e9d5bc) operation order evaluated by an exact IEEE-754 binary64
// replica (the dev preset compiles no FMA contraction): the SHA-256 of the 90 output
// f64 (little endian), the residual gross and the four coefficients, bit for bit.
TEST(StrategyPriceNeutralizeV6, PriceRiskV1BytesArePinnedOnTheExistingFixture) {
  constexpr usize n = 90;
  Section s = section(n, 3);
  for (usize i = 0; i < n; ++i) {
    if (i % 13 == 0) {
      s.member[i] = 0;
    } else if (i % 11 == 5) {
      s.ok[i] = 0;
      for (usize k = 0; k < kCols; ++k) s.exposures[i * kCols + k] = missing;
    }
  }
  normalize(s.target, s.member);
  st::PriceExposureConfig cfg;
  cfg.min_names = 20;
  st::NeutralizeScratch scratch;
  st::NeutralizeStats stats;
  auto w = s.target;
  const auto status = st::neutralize_target(w, s.member, s.exposures, s.ok, cfg, scratch, stats);
  ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(stats.used, usize{75});
  EXPECT_EQ(co::sha256_hex(std::as_bytes(std::span<const f64>(w))).value(),
            "0145d9a452ecb8b032d3a9106a6cd3f04aae089d16ddc7e5c265ff1810dfdfc0");
  EXPECT_EQ(std::bit_cast<u64>(stats.residual_gross), 0x3fe4d6ca304e881fULL);
  const std::array<u64, kCols + 1> coefficients{0xbf452eb685e46372ULL, 0x3f86487261046f59ULL,
                                                0xbf4a1223e357f171ULL, 0xbf3b108c2c5ecb75ULL};
  for (usize k = 0; k < coefficients.size(); ++k)
    EXPECT_EQ(std::bit_cast<u64>(stats.coefficients[k]), coefficients[k]) << k;
  EXPECT_EQ(std::bit_cast<u64>(w[1]), 0x3f9769edb54fe169ULL);
  EXPECT_EQ(std::bit_cast<u64>(w[89]), 0xbf9082ec91c7fcb6ULL);
}

// The within-groups fields stay 0 on the price-risk-v1 path, also when a within-groups
// call filled them in the same stats object before (stats is reset on entry). Kept out
// of (a) so that (a) compiles verbatim at the base commit, which has no such fields.
TEST(StrategyPriceNeutralizeV6, PriceRiskV1LeavesTheWithinGroupsFieldsZero) {
  constexpr usize n = 40;
  const Section s = section(n, 9);
  std::vector<f64> ids(n);
  for (usize i = 0; i < n; ++i) ids[i] = i % 2 ? 1.0 : 2.0;
  st::PriceExposureConfig cfg;
  cfg.min_names = 10;
  st::NeutralizeScratch scratch;
  st::NeutralizeStats stats;
  auto grouped = s.target;
  ASSERT_TRUE(st::neutralize_target_within_groups(grouped, s.member, s.exposures, s.ok, ids,
                                                  cfg, scratch, stats));
  ASSERT_EQ(stats.groups, usize{2});
  auto w = s.target;
  ASSERT_TRUE(st::neutralize_target(w, s.member, s.exposures, s.ok, cfg, scratch, stats));
  EXPECT_EQ(stats.groups, usize{0});
  EXPECT_EQ(stats.unknown_group_names, usize{0});
  EXPECT_EQ(stats.fallback_names, usize{0});
}

// (b) Two groups of 30: the neutralized target's within-group sums (hence means) are 0,
// its gross is the entry gross 1, and it stays orthogonal to the clipped z design; the
// group bet the input carried is gone and the result is not price-risk-v1's.
TEST(StrategyPriceNeutralizeV6, WithinGroupsZeroesGroupMeansKeepsGrossAndExposure) {
  constexpr usize n = 60;
  std::vector<f64> ids(n), shift(n, 0.0);
  for (usize i = 0; i < n; ++i) {
    ids[i] = i % 2 ? 3.0 : 7.0;
    if (ids[i] == 7.0) shift[i] = 0.01;
  }
  const Section s = loaded_section(n, 31, shift);
  st::PriceExposureConfig cfg;
  cfg.min_names = 20;
  st::NeutralizeScratch scratch;
  st::NeutralizeStats stats;
  auto w = s.target;
  const auto status =
      st::neutralize_target_within_groups(w, s.member, s.exposures, s.ok, ids, cfg, scratch, stats);
  ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(stats.used, n);
  EXPECT_EQ(stats.groups, usize{2});
  EXPECT_EQ(stats.unknown_group_names, usize{0});
  EXPECT_EQ(stats.fallback_names, usize{0});
  EXPECT_NEAR(stats.gross, 1.0, 1e-12);
  EXPECT_GT(std::abs(group_sum(s.target, s, ids, 7.0)), 0.1); // the input's group bet
  EXPECT_LE(std::abs(group_sum(w, s, ids, 7.0)), 1e-12);
  EXPECT_LE(std::abs(group_sum(w, s, ids, 3.0)), 1e-12);
  EXPECT_NEAR(gross_of(w), 1.0, 1e-12);
  const auto z = design_z(s, cfg.clip_z);
  const auto after = moments(w, s, z);
  for (usize k = 0; k < after.size(); ++k) EXPECT_LE(std::abs(after[k]), 1e-12) << k;
  auto v1 = s.target;
  st::NeutralizeStats v1_stats;
  ASSERT_TRUE(st::neutralize_target(v1, s.member, s.exposures, s.ok, cfg, scratch, v1_stats));
  f64 largest = 0;
  for (usize i = 0; i < n; ++i) largest = std::max(largest, std::abs(w[i] - v1[i]));
  EXPECT_GT(largest, 1e-3);
  // Scratch reuse (after a v1 call) is stateless.
  auto again = s.target;
  st::NeutralizeStats again_stats;
  ASSERT_TRUE(st::neutralize_target_within_groups(again, s.member, s.exposures, s.ok, ids, cfg,
                                                  scratch, again_stats));
  expect_bits(again, w);
}

// (c) Groups of 20, 20, 3 and 2 names plus 15 unknown (NaN) ids, interleaved. The 3- and
// 2-name groups fall back: pooled, they are demeaned together (their joint sum is 0, the
// 3-name group's own sum is not), and relabelling them as one 5-name group and the NaN
// names as a real id reproduces the result bit for bit.
TEST(StrategyPriceNeutralizeV6, SmallGroupsPoolIntoOneFallbackAndUnknownIdsAreOneGroup) {
  constexpr usize n = 60;
  std::vector<f64> block(n);
  for (usize i = 0; i < n; ++i)
    block[i] = i < 20 ? 1.0 : i < 40 ? 2.0 : i < 43 ? 5.0 : i < 45 ? 9.0 : missing;
  std::vector<f64> ids(n), shift(n, 0.0);
  for (usize i = 0; i < n; ++i) {
    ids[i] = block[(i * 7) % n];
    shift[i] = ids[i] == 5.0 ? 0.02 : ids[i] == 2.0 ? -0.005 : 0.0;
  }
  const Section s = loaded_section(n, 32, shift);
  st::PriceExposureConfig cfg;
  cfg.min_names = 20;
  st::NeutralizeScratch scratch;
  st::NeutralizeStats stats;
  auto w = s.target;
  const auto status =
      st::neutralize_target_within_groups(w, s.member, s.exposures, s.ok, ids, cfg, scratch, stats);
  ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(stats.used, n);
  EXPECT_EQ(stats.unknown_group_names, usize{15});
  EXPECT_EQ(stats.fallback_names, usize{5});
  EXPECT_EQ(stats.groups, usize{4}); // 1, 2, unknown, pooled fallback
  for (const f64 id : {1.0, 2.0, missing})
    EXPECT_LE(std::abs(group_sum(w, s, ids, id)), 1e-12) << id;
  EXPECT_LE(std::abs(group_sum(w, s, ids, 5.0) + group_sum(w, s, ids, 9.0)), 1e-12);
  EXPECT_GT(std::abs(group_sum(w, s, ids, 5.0)), 1e-3); // no level of its own
  EXPECT_NEAR(gross_of(w), 1.0, 1e-12);
  std::vector<f64> relabelled(ids);
  for (auto& id : relabelled) id = std::isnan(id) ? 42.0 : (id == 5.0 || id == 9.0) ? 11.0 : id;
  auto same = s.target;
  st::NeutralizeStats same_stats;
  ASSERT_TRUE(st::neutralize_target_within_groups(same, s.member, s.exposures, s.ok, relabelled,
                                                  cfg, scratch, same_stats));
  expect_bits(same, w);
  EXPECT_EQ(same_stats.groups, usize{4});
  EXPECT_EQ(same_stats.unknown_group_names, usize{0});
  EXPECT_EQ(same_stats.fallback_names, usize{0});
}

// Contract: ids are integers in [0, kMaxGroupId] where read (used rows only), the group
// span has the target's length (an empty span never falls back to price-risk-v1), and an
// exposure constant within every group is refused as spanned; the target is unmodified.
TEST(StrategyPriceNeutralizeV6, WithinGroupsRefusesBadIdsGeometryAndSpannedExposure) {
  constexpr usize n = 40;
  Section s = section(n, 9);
  std::vector<f64> ids(n);
  for (usize i = 0; i < n; ++i) ids[i] = i % 2 ? 1.0 : 2.0;
  st::PriceExposureConfig cfg;
  cfg.min_names = 10;
  st::NeutralizeScratch scratch;
  const auto run = [&](const Section& x, std::span<const f64> group) {
    auto w = x.target;
    st::NeutralizeStats stats;
    const auto status =
        st::neutralize_target_within_groups(w, x.member, x.exposures, x.ok, group, cfg, scratch,
                                            stats);
    if (!status) expect_bits(w, x.target);
    return status;
  };
  EXPECT_TRUE(run(s, ids));
  for (const f64 bad : {3.5, -1.0, static_cast<f64>(st::kMaxGroupId) + 1,
                        std::numeric_limits<f64>::infinity()}) {
    auto wrong = ids;
    wrong[7] = bad;
    expect_code(run(s, wrong), co::ErrorCode::InvalidArgument);
  }
  // Unread rows: a nonmember (target 0) and a member without exposures.
  Section skip = s;
  skip.member[3] = 0;
  skip.ok[8] = 0;
  normalize(skip.target, skip.member);
  auto unread = ids;
  unread[3] = 3.5;
  unread[8] = -7.0;
  EXPECT_TRUE(run(skip, unread));
  expect_code(run(s, std::span<const f64>(ids).first(n - 1)), co::ErrorCode::InvalidArgument);
  expect_code(run(s, {}), co::ErrorCode::InvalidArgument);
  Section between = s; // log ADV differs across the two groups only
  for (usize i = 0; i < n; ++i) between.exposures[i * kCols + st::kExposureLogAdv] = 12.0 + ids[i];
  expect_code(run(between, ids), co::ErrorCode::Unavailable);
}

// The one-call step is compute_price_exposures + neutralize_target_within_groups bit for
// bit; a nonmember's id is never read.
TEST(StrategyPriceNeutralizeV6, WithinGroupsOneCallStepMatchesPrimitives) {
  const Panel p = noisy_panel(70, 12, 21);
  const auto cfg = small_config();
  constexpr usize d = 60;
  std::vector<u8> member(p.names, 1);
  member[4] = 0;
  std::vector<f64> ids(p.names);
  for (usize i = 0; i < p.names; ++i) ids[i] = i < 6 ? 2.0 : 5.0;
  ids[4] = 0.5; // nonmember: unread
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
  const auto status = st::neutralize_target_within_groups(
      expected, member, exposures, ok, ids, cfg, neutralize_scratch, expected_stats);
  ASSERT_TRUE(status) << status.error().to_string();
  EXPECT_EQ(expected_stats.used, usize{11});
  EXPECT_EQ(expected_stats.groups, usize{2});
  st::PriceRiskScratch scratch;
  for (int pass = 0; pass < 2; ++pass) {
    auto w = target;
    st::NeutralizeStats stats;
    ASSERT_TRUE(st::neutralize_price_risk_within_groups(p.input(), cfg, d, w, member, ids,
                                                        scratch, stats));
    expect_bits(w, expected);
    EXPECT_EQ(std::bit_cast<u64>(stats.residual_gross),
              std::bit_cast<u64>(expected_stats.residual_gross));
  }
  auto w = target;
  st::NeutralizeStats stats;
  expect_code(st::neutralize_price_risk_within_groups(p.input(), cfg, d, w, member, {}, scratch,
                                                      stats),
              co::ErrorCode::InvalidArgument);
  expect_bits(w, target);
}

// Locate-in-aim under the industry ids (review I3): a held row (hold_zero set, entering
// at 0) is reset to 0 after the within-group demeaning, so it cannot come back as -(its
// group mean). Three zero-aim names of the positively loaded group 7 carry that group's
// mean exposures, so their demeaned z rows vanish and the fit reaches them only through
// the intercept: unheld each ends at -(group mean) x scale; held, at -(intercept) x scale
// with intercept = 3 x (group mean) / 60 used rows, and group 7 keeps 3 x (group mean) -
// 30 x intercept before the rescale. A hold byte on a nonzero aim changes nothing; a hold
// span of another length is refused with the target unmodified.
TEST(StrategyPriceNeutralizeV6, HeldZeroAimsAreResetAfterTheGroupDemeaning) {
  constexpr usize n = 60;
  std::vector<f64> ids(n), shift(n, 0.0);
  for (usize i = 0; i < n; ++i) {
    ids[i] = i % 2 ? 3.0 : 7.0;
    if (ids[i] == 7.0) shift[i] = 0.01;
  }
  Section s = loaded_section(n, 31, shift);
  constexpr std::array<usize, 3> held{4, 20, 36}; // group 7
  std::array<f64, kCols> mean{};
  for (usize i = 0; i < n; ++i) {
    if (ids[i] != 7.0 || std::find(held.begin(), held.end(), i) != held.end()) continue;
    for (usize k = 0; k < kCols; ++k) mean[k] += s.exposures[i * kCols + k];
  }
  std::vector<u8> hold(n, 0);
  for (const usize h : held) {
    s.target[h] = 0;
    hold[h] = 1;
    for (usize k = 0; k < kCols; ++k) s.exposures[h * kCols + k] = mean[k] / 27.0;
  }
  f64 group_sum_7 = 0; // group 7's entry target, held rows (0) included
  for (usize i = 0; i < n; ++i)
    if (ids[i] == 7.0) group_sum_7 += s.target[i];
  const f64 group_mean = group_sum_7 / 30.0;
  ASSERT_GT(group_mean, 1e-3); // the group bet a zeroed short would inherit, negated
  st::PriceExposureConfig cfg;
  cfg.min_names = 20;
  st::NeutralizeScratch scratch;
  st::NeutralizeStats unheld_stats, held_stats;
  auto unheld = s.target, w = s.target;
  ASSERT_TRUE(st::neutralize_target_within_groups(unheld, s.member, s.exposures, s.ok, ids, cfg,
                                                  scratch, unheld_stats));
  ASSERT_TRUE(st::neutralize_target_within_groups(w, s.member, s.exposures, s.ok, ids, cfg,
                                                  scratch, held_stats, hold));
  const f64 unheld_scale = unheld_stats.gross / unheld_stats.residual_gross;
  const f64 held_scale = held_stats.gross / held_stats.residual_gross;
  const f64 intercept = 3.0 * group_mean / static_cast<f64>(n);
  for (const usize h : held) {
    EXPECT_NEAR(unheld[h], -group_mean * unheld_scale, 1e-12) << h;
    EXPECT_NEAR(w[h], -intercept * held_scale, 1e-12) << h;
  }
  EXPECT_NEAR(held_stats.coefficients[0], intercept, 1e-12);
  EXPECT_NEAR(group_sum(w, s, ids, 7.0), (3.0 * group_mean - 30.0 * intercept) * held_scale,
              1e-12);
  EXPECT_NEAR(gross_of(w), held_stats.gross, 1e-12);
  const std::vector<u8> every(n, 1); // only the zero aims are held
  auto same = s.target;
  st::NeutralizeStats same_stats;
  ASSERT_TRUE(st::neutralize_target_within_groups(same, s.member, s.exposures, s.ok, ids, cfg,
                                                  scratch, same_stats, every));
  expect_bits(same, w);
  auto refused = s.target;
  st::NeutralizeStats refused_stats;
  expect_code(st::neutralize_target_within_groups(refused, s.member, s.exposures, s.ok, ids, cfg,
                                                  scratch, refused_stats,
                                                  std::span<const u8>(hold).first(n - 1)),
              co::ErrorCode::InvalidArgument);
  expect_bits(refused, s.target);
}

// v8 D-1 session ring (review P-9): a scratch bound to one panel logs each session once
// and keeps every interval return; its exposures are the stateless window recompute's
// bits (a fresh scratch per decision) in any decision order: ascending through the
// clipped start, cadence jumps, a jump past the whole ring, backward jumps; across a
// config change that resizes the ring; on another panel (served statelessly, the ring
// kept); and after a rebind. An absent row and a guarded split sit inside the windows.
TEST(LogRing, ExposuresBitIdenticalToWindowRecompute) {
  Panel p = noisy_panel(170, 9, 17);
  const usize hole = p.at(50, 2);
  p.present[hole] = 0;
  p.close[hole] = p.raw[hole] = p.volume[hole] = missing;
  for (usize t = 70; t < p.dates; ++t) p.close[p.at(t, 4)] *= 2.0; // adjusted-only split
  const auto cfg = small_config(); // beta 40, vol 20: a ring of 40 intervals
  st::PriceExposureScratch ring;
  EXPECT_FALSE(ring.ring.bound);
  st::enable_session_ring(ring, p.input());
  const auto check = [&](const Panel& panel, const st::PriceExposureConfig& c, usize d) {
    st::PriceExposureScratch fresh;
    const auto expected = exposures_at(panel, c, d, fresh);
    const auto actual = exposures_at(panel, c, d, ring);
    expect_bits(actual.out, expected.out);
    EXPECT_EQ(actual.ok, expected.ok) << d;
  };
  for (usize d = 0; d < 60; ++d) check(p, cfg, d); // clipped windows, then full ones
  EXPECT_EQ(ring.ring.capacity, 40U);
  EXPECT_EQ(ring.ring.logged, 59U); // the last session logged is the last decision's
  for (usize d = 60; d < 130; d += 7) check(p, cfg, d); // cadence jumps
  const std::array<usize, 8> jumps{169, 20, 100, 41, 168, 3, 90, 91}; // past the ring, back
  for (const usize d : jumps) check(p, cfg, d);
  auto wide = cfg;
  wide.vol_window = 60; // block 60: the ring is resized and refilled
  for (const usize d : std::array<usize, 4>{150, 80, 81, 169}) check(p, wide, d);
  EXPECT_EQ(ring.ring.capacity, 60U);
  auto narrow = cfg;
  narrow.beta_window = 30; narrow.min_return_pairs = 15; // block 30
  for (const usize d : std::array<usize, 3>{60, 61, 100}) check(p, narrow, d);
  const Panel other = noisy_panel(170, 9, 23);
  check(other, narrow, 120); // not the bound panel: stateless
  EXPECT_EQ(ring.ring.close, p.close.data());
  check(p, narrow, 101);
  st::enable_session_ring(ring, other.input());
  EXPECT_EQ(ring.ring.capacity, 0U);
  EXPECT_EQ(ring.ring.seconds, 0.0);
  for (usize d = 100; d < 112; ++d) check(other, cfg, d);
  EXPECT_GE(ring.ring.seconds, 0.0);
  EXPECT_EQ(st::session_ring_bytes(cfg, 9),
            u64{9} * (2 * 40 + 4) * sizeof(f64) + 40 * (2 * sizeof(f64) + sizeof(usize)));
  auto bad = cfg;
  bad.beta_window = 5000; // block above 4096: refused by compute_price_exposures
  EXPECT_EQ(st::session_ring_bytes(bad, 9), 0U);
}
