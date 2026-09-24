// Lane 4 (l4-mtest): TrialRegistry — content-addressed trial log, Welford V[SR],
// effective number of independent trials, crash-safe durable reopen, and the
// registry-fed Deflated Sharpe overload.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/breadth.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp"
#include "atx/engine/eval/trial_registry.hpp"

namespace atx_test_l4_mtest_trial_registry {

using namespace atx::engine::eval;
using atx::f64;
using atx::u64;
using atx::usize;

class Gauss {
public:
  explicit Gauss(u64 seed) : s_{seed} {}
  f64 next() {
    const f64 u1 = (static_cast<f64>(mix() >> 11U) + 0.5) * 0x1.0p-53;
    const f64 u2 = (static_cast<f64>(mix() >> 11U) + 0.5) * 0x1.0p-53;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

private:
  u64 mix() {
    u64 z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }
  u64 s_;
};

std::vector<f64> noise(usize t, u64 seed) {
  Gauss g{seed};
  std::vector<f64> v(t);
  for (f64 &x : v) {
    x = 0.01 * g.next();
  }
  return v;
}

f64 sharpe_of(const std::vector<f64> &x) {
  f64 m = 0.0;
  for (const f64 v : x) {
    m += v;
  }
  m /= static_cast<f64>(x.size());
  f64 ss = 0.0;
  for (const f64 v : x) {
    ss += (v - m) * (v - m);
  }
  return m / std::sqrt(ss / static_cast<f64>(x.size() - 1U));
}

std::filesystem::path fresh_path(const std::string &name) {
  const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                  ("atx_l4_trial_registry_" + name + ".bin");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p;
}

TrialRegistry must_mem(usize t, usize d = 256U) {
  TrialRegistryConfig cfg;
  cfg.pnl_len = t;
  cfg.sketch_dim = d;
  auto r = TrialRegistry::in_memory(cfg);
  EXPECT_TRUE(r.has_value());
  return std::move(*r);
}

TEST(EvalTrialRegistry, PerfectlyCorrelatedTrialsCollapseToOne) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t);
  const std::vector<f64> base = noise(t, 1U);
  for (usize i = 0; i < 50U; ++i) {
    std::vector<f64> x(t);
    for (usize s = 0; s < t; ++s) {
      x[s] = (1.0 + 0.1 * static_cast<f64>(i)) * base[s] + 0.001 * static_cast<f64>(i);
    }
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, 1000U + i, x, sharpe_of(x)).has_value());
  }
  const TrialSummary s = reg.summary();
  EXPECT_EQ(s.n_raw, 50U);
  EXPECT_NEAR(s.n_eff, 1.0, 1e-6);
}

TEST(EvalTrialRegistry, IndependentTrialsCountFully) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t);
  for (usize i = 0; i < 50U; ++i) {
    const std::vector<f64> x = noise(t, 100U + i);
    ASSERT_TRUE(reg.record(TrialKind::CombinerHyper, i, x, sharpe_of(x)).has_value());
  }
  const TrialSummary s = reg.summary();
  EXPECT_EQ(s.n_raw, 50U);
  EXPECT_NEAR(s.n_eff, 50.0, 5.0);
  // Without the bias correction ρ̂² noise alone costs ~N/(1 + (N-1)/T) ≈ 42.
  EXPECT_LT(s.n_eff_uncorrected, s.n_eff);
}

TEST(EvalTrialRegistry, TwoClustersGiveAboutTwo) {
  const usize t = 400U;
  TrialRegistry reg = must_mem(t, 512U);
  const std::vector<f64> a = noise(t, 7U);
  const std::vector<f64> b = noise(t, 8U);
  for (usize i = 0; i < 40U; ++i) {
    const std::vector<f64> eps = noise(t, 500U + i);
    std::vector<f64> x(t);
    const std::vector<f64> &c = (i % 2U == 0U) ? a : b;
    for (usize s = 0; s < t; ++s) {
      x[s] = c[s] + 0.05 * eps[s]; // ρ within a cluster ≈ 0.9975
    }
    ASSERT_TRUE(reg.record(TrialKind::StackHyper, i, x, sharpe_of(x)).has_value());
  }
  EXPECT_NEAR(reg.summary().n_eff, 2.0, 0.2);
}

TEST(EvalTrialRegistry, SketchModeTracksExactMode) {
  const usize t = 1000U;
  TrialRegistry sk = must_mem(t, 128U); // count-sketch, d << T
  TrialRegistry sk_corr = must_mem(t, 128U);
  const std::vector<f64> base = noise(t, 3U);
  for (usize i = 0; i < 60U; ++i) {
    const std::vector<f64> x = noise(t, 900U + i);
    ASSERT_TRUE(sk.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
    std::vector<f64> y(t);
    for (usize s = 0; s < t; ++s) {
      y[s] = base[s] * (1.0 + 0.01 * static_cast<f64>(i));
    }
    ASSERT_TRUE(sk_corr.record(TrialKind::MinerExpr, i, y, sharpe_of(y)).has_value());
  }
  EXPECT_NEAR(sk.summary().n_eff, 60.0, 12.0);
  EXPECT_NEAR(sk_corr.summary().n_eff, 1.0, 0.25);
}

TEST(EvalTrialRegistry, UncorrectedRatioIsTheBreadthParticipationRatio) {
  const usize t = 120U;
  const usize n = 10U;
  TrialRegistry reg = must_mem(t);
  std::vector<std::vector<f64>> xs;
  const std::vector<f64> common = noise(t, 42U);
  for (usize i = 0; i < n; ++i) {
    std::vector<f64> x = noise(t, 60U + i);
    for (usize s = 0; s < t; ++s) {
      x[s] += common[s] * static_cast<f64>(i) * 0.2;
    }
    ASSERT_TRUE(reg.record(TrialKind::RegimeCount, i, x, sharpe_of(x)).has_value());
    xs.push_back(std::move(x));
  }
  // Correlation matrix of the n trials -> breadth.hpp participation ratio.
  atx::core::linalg::MatX corr(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
  std::vector<f64> mean(n, 0.0);
  std::vector<f64> sd(n, 0.0);
  for (usize i = 0; i < n; ++i) {
    for (const f64 v : xs[i]) {
      mean[i] += v;
    }
    mean[i] /= static_cast<f64>(t);
    for (const f64 v : xs[i]) {
      sd[i] += (v - mean[i]) * (v - mean[i]);
    }
    sd[i] = std::sqrt(sd[i]);
  }
  for (usize i = 0; i < n; ++i) {
    for (usize j = 0; j < n; ++j) {
      f64 c = 0.0;
      for (usize s = 0; s < t; ++s) {
        c += (xs[i][s] - mean[i]) * (xs[j][s] - mean[j]);
      }
      corr(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = c / (sd[i] * sd[j]);
    }
  }
  EXPECT_NEAR(reg.summary().n_eff_uncorrected, effective_breadth(corr), 1e-9);
}

TEST(EvalTrialRegistry, ContentAddressedDedup) {
  const usize t = 50U;
  TrialRegistry reg = must_mem(t);
  const std::vector<f64> x = noise(t, 5U);
  const auto a = reg.record(TrialKind::MinerExpr, 77U, x, 0.1);
  const auto b = reg.record(TrialKind::MinerExpr, 77U, x, 0.1);
  const auto c = reg.record(TrialKind::OptimizerHyper, 77U, x, 0.1);
  ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
  EXPECT_TRUE(a->inserted);
  EXPECT_FALSE(b->inserted);
  EXPECT_EQ(a->id, b->id);
  EXPECT_TRUE(c->inserted);
  EXPECT_FALSE(a->id == c->id);
  EXPECT_EQ(a->id, trial_id(TrialKind::MinerExpr, 77U));
  EXPECT_EQ(reg.size(), 2U);
  EXPECT_TRUE(reg.contains(trial_id(TrialKind::OptimizerHyper, 77U)));
}

TEST(EvalTrialRegistry, WelfordSharpeMoments) {
  const usize t = 30U;
  TrialRegistry reg = must_mem(t);
  const std::vector<f64> srs{0.1, -0.05, 0.2, 0.07, 0.0, 0.15};
  for (usize i = 0; i < srs.size(); ++i) {
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, noise(t, i + 1U), srs[i]).has_value());
  }
  f64 m = 0.0;
  for (const f64 v : srs) {
    m += v;
  }
  m /= static_cast<f64>(srs.size());
  f64 ss = 0.0;
  for (const f64 v : srs) {
    ss += (v - m) * (v - m);
  }
  const TrialSummary s = reg.summary();
  EXPECT_NEAR(s.mean_sr, m, 1e-15);
  EXPECT_NEAR(s.var_sr, ss / 5.0, 1e-15);
  EXPECT_DOUBLE_EQ(s.max_sr, 0.2);
}

TEST(EvalTrialRegistry, RejectsBadInput) {
  TrialRegistryConfig cfg;
  cfg.pnl_len = 2U;
  EXPECT_FALSE(TrialRegistry::in_memory(cfg).has_value());
  TrialRegistry reg = must_mem(20U);
  EXPECT_FALSE(reg.record(TrialKind::MinerExpr, 1U, noise(19U, 1U), 0.1).has_value());
  EXPECT_FALSE(reg.record(TrialKind::MinerExpr, 1U, std::vector<f64>(20U, 0.5), 0.1).has_value());
  std::vector<f64> bad = noise(20U, 2U);
  bad[3] = std::nan("");
  EXPECT_FALSE(reg.record(TrialKind::MinerExpr, 1U, bad, 0.1).has_value());
  EXPECT_FALSE(reg.record(TrialKind::MinerExpr, 1U, noise(20U, 3U), INFINITY).has_value());
  EXPECT_EQ(reg.size(), 0U);
  EXPECT_EQ(reg.summary().n_eff, 0.0);
}

TEST(EvalTrialRegistry, SurvivesCrashAndReopen) {
  const std::filesystem::path path = fresh_path("crash");
  TrialRegistryConfig cfg;
  cfg.pnl_len = 64U;
  cfg.sketch_dim = 32U; // sketch mode exercised through the file format too
  TrialSummary before;
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
    for (usize i = 0; i < 20U; ++i) {
      const std::vector<f64> x = noise(64U, 300U + i);
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
    }
    before = reg->summary();
  }
  // Simulate a crash mid-append: a torn partial record at the tail.
  {
    std::ofstream f(path, std::ios::binary | std::ios::app);
    const char junk[37] = {1, 2, 3, 4, 5, 6, 7};
    f.write(junk, sizeof(junk));
  }
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
    const TrialSummary after = reg->summary();
    EXPECT_EQ(after.n_raw, before.n_raw);
    EXPECT_EQ(after.n_eff, before.n_eff);
    EXPECT_EQ(after.var_sr, before.var_sr);
    EXPECT_EQ(after.registry_hash, before.registry_hash);
    // Deduplication survives the reopen, and appends continue after the repair.
    const std::vector<f64> x0 = noise(64U, 300U);
    const auto dup = reg->record(TrialKind::MinerExpr, 0U, x0, sharpe_of(x0));
    ASSERT_TRUE(dup.has_value());
    EXPECT_FALSE(dup->inserted);
    const std::vector<f64> x = noise(64U, 999U);
    ASSERT_TRUE(reg->record(TrialKind::StackHyper, 5U, x, sharpe_of(x)).has_value());
  }
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->summary().n_raw, 21U);
  }
  // A config that disagrees with the file header is refused.
  TrialRegistryConfig other = cfg;
  other.pnl_len = 65U;
  EXPECT_FALSE(TrialRegistry::open(path, other).has_value());
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(EvalTrialRegistry, CorruptRecordBodyIsTruncated) {
  const std::filesystem::path path = fresh_path("corrupt");
  TrialRegistryConfig cfg;
  cfg.pnl_len = 16U;
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value());
    for (usize i = 0; i < 3U; ++i) {
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, noise(16U, 40U + i), 0.1).has_value());
    }
  }
  // Flip a byte in the LAST record: it fails its checksum and is dropped.
  const auto size = std::filesystem::file_size(path);
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(size - 20U));
    const char b = 0x5a;
    f.write(&b, 1);
  }
  auto reg = TrialRegistry::open(path, cfg);
  ASSERT_TRUE(reg.has_value());
  EXPECT_EQ(reg->size(), 2U);
  reg = TrialRegistry::in_memory(cfg); // release the file before removal
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Review finding: a bad checksum in the MIDDLE of the log is not a torn tail.
// Reopen must refuse (ParseError) and leave the file byte-identical, never
// silently truncate acknowledged trials (which would undercount N).
TEST(EvalTrialRegistry, MidLogCorruptionIsRefusedNotTruncated) {
  const std::filesystem::path path = fresh_path("midlog");
  TrialRegistryConfig cfg;
  cfg.pnl_len = 16U;
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value());
    for (usize i = 0; i < 10U; ++i) {
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, noise(16U, 70U + i), 0.1).has_value());
    }
  }
  const auto size = std::filesystem::file_size(path);
  const usize rb = (size - 48U) / 10U; // header 48 bytes, 10 fixed-size records
  ASSERT_EQ(48U + 10U * rb, size);
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(static_cast<std::streamoff>(48U + 2U * rb + 20U)); // inside record 3
    const char b = 0x5a;
    f.write(&b, 1);
  }
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_FALSE(reg.has_value());
    EXPECT_EQ(reg.error().code(), atx::core::ErrorCode::ParseError);
  }
  EXPECT_EQ(std::filesystem::file_size(path), size); // nothing deleted
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Review finding: in count-sketch mode the sketch is renormalized, so
// identical trials collapse to exactly 1 for EVERY sketch seed (unnormalized
// sketches read 1.2-3.4 at d = 32..64), while independent trials still count
// about fully.
TEST(EvalTrialRegistry, SketchModeIdenticalTrialsCollapseAcrossSeeds) {
  const usize t = 2520U;
  const usize n = 50U;
  f64 indep_sum = 0.0;
  const usize seeds = 12U;
  for (usize k = 0; k < seeds; ++k) {
    for (const usize d : {usize{32}, usize{64}}) {
      TrialRegistryConfig cfg;
      cfg.pnl_len = t;
      cfg.sketch_dim = d;
      cfg.sketch_seed = 0x1000U + k;
      auto same = TrialRegistry::in_memory(cfg);
      ASSERT_TRUE(same.has_value());
      const std::vector<f64> base = noise(t, 7000U + k);
      for (usize i = 0; i < n; ++i) {
        std::vector<f64> y(t);
        for (usize s = 0; s < t; ++s) {
          y[s] = base[s] * (1.0 + 0.01 * static_cast<f64>(i)) + 0.001 * static_cast<f64>(i);
        }
        ASSERT_TRUE(same->record(TrialKind::MinerExpr, i, y, sharpe_of(y)).has_value());
      }
      EXPECT_NEAR(same->summary().n_eff, 1.0, 1e-6) << "d=" << d << " seed=" << k;
      if (d == 64U) {
        auto ind = TrialRegistry::in_memory(cfg);
        ASSERT_TRUE(ind.has_value());
        for (usize i = 0; i < n; ++i) {
          const std::vector<f64> x = noise(t, 90000U + 100U * k + i);
          ASSERT_TRUE(ind->record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
        }
        indep_sum += ind->summary().n_eff;
      }
    }
  }
  EXPECT_NEAR(indep_sum / static_cast<f64>(seeds), static_cast<f64>(n), 8.0);
}

TEST(EvalTrialRegistry, RegistryFedDsrIsLessOverDeflatedOnCorrelatedTrials) {
  const usize t = 252U;
  TrialRegistry reg = must_mem(t);
  const std::vector<f64> base = noise(t, 17U);
  for (usize i = 0; i < 100U; ++i) {
    const std::vector<f64> eps = noise(t, 2000U + i);
    std::vector<f64> x(t);
    for (usize s = 0; s < t; ++s) {
      x[s] = base[s] + 0.1 * eps[s] + 0.0004;
    }
    ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
  }
  const TrialSummary s = reg.summary();
  ASSERT_LT(s.n_eff, 5.0);
  const f64 sr = s.max_sr;
  const DsrResult raw = deflated_sharpe(sr, t, 0.0, 0.0, static_cast<usize>(s.n_raw), s.var_sr);
  const DsrResult fed = deflated_sharpe(sr, s, t, 0.0, 0.0);
  EXPECT_GE(fed.dsr, raw.dsr);
  EXPECT_LE(fed.sr_star, raw.sr_star);
  // One trial == no selection: the overload collapses to PSR(0).
  TrialRegistry one = must_mem(t);
  const std::vector<f64> x = noise(t, 4U);
  ASSERT_TRUE(one.record(TrialKind::MinerExpr, 1U, x, 0.1).has_value());
  EXPECT_NEAR(deflated_sharpe(0.1, one.summary(), t, 0.0, 0.0).dsr,
              probabilistic_sharpe(0.1, 0.0, t, 0.0, 0.0), 1e-12);
}

} // namespace atx_test_l4_mtest_trial_registry
