#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/combine/signal_cube.hpp"
#include "atx/engine/combine/signal_combiner.hpp"
#include "atx/engine/combine/signal_store.hpp"
#include "atx/engine/combine/walk_forward_combiner.hpp"

namespace atx_test_e1_ic_cache {
namespace cb = atx::engine::combine;
using atx::f64;
using atx::usize;
constexpr usize dates = 80U, names = 24U, alphas = 3U;

struct Fixture {
  std::array<std::vector<f64>, alphas> signals;
  std::vector<f64> returns;
  Fixture() : returns(dates * names) {
    atx::u64 state = 0x49b18U;
    const auto noise = [&]() {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<f64>(state >> 11U) * 0x1.0p-53 - 0.5;
    };
    for (auto& signal : signals) {
      signal.resize(dates * names);
      for (auto& value : signal) value = noise();
    }
    for (usize i = 0U; i < returns.size(); ++i) {
      returns[i] = 0.1 * signals[0][i] - 0.07 * signals[1][i] + noise();
    }
    signals[1][5U * names] = cb::kSignalNaN;
    returns[11U * names + 2U] = cb::kSignalNaN;
  }
  cb::SignalStore store() const {
    auto result = cb::SignalStore::create(dates, names);
    EXPECT_TRUE(result);
    for (const auto& signal : signals) EXPECT_TRUE(result->add_signal(signal));
    EXPECT_TRUE(result->set_forward_returns(returns));
    return std::move(*result);
  }
};

void same_bits(std::span<const f64> a, std::span<const f64> b) {
  ASSERT_EQ(a.size(), b.size());
  for (usize i = 0U; i < a.size(); ++i)
    EXPECT_EQ(std::bit_cast<atx::u64>(a[i]), std::bit_cast<atx::u64>(b[i])) << i;
}

void same_path(const cb::WeightPath& a, const cb::WeightPath& b) {
  same_bits(a.w, b.w);
  EXPECT_EQ(a.refit_dates, b.refit_dates);
  EXPECT_EQ(a.adopted_dates, b.adopted_dates);
  EXPECT_EQ(a.failed_fits, b.failed_fits);
}

TEST(CombineSignalCubeConsumer, ExactStatisticsAndBothFitMethodsPreserveBits) {
  const auto store = Fixture{}.store();
  for (const auto treatment : {cb::IcReturnTreatment::RawV1, cb::IcReturnTreatment::WinsorizedV2}) {
    cb::SignalIcCacheConfig cfg{5U, 1U, 70U, treatment};
    auto cache = cb::SignalIcCache::from_store(store, {3U, 61U}, cfg);
    ASSERT_TRUE(cache);
    same_bits(cache->view().values, cb::ic_matrix(store, {3U, 61U}, treatment));
    for (const auto rule : {atx::engine::eval::hac::TStatRule::IidV1,
                           atx::engine::eval::hac::TStatRule::HorizonAwareV3}) {
      const cb::SignalInferenceConfig inference{rule, 5U, treatment};
      const cb::IcirEwmaCombiner ewma{13.0, 0.5, inference};
      const cb::GrinoldKahnCombiner gk{cb::CovTarget::LwIdentity, inference};
      for (const auto window : {cb::FitWindow{3U, 31U}, cb::FitWindow{10U, 61U}}) {
        const auto a = ewma.fit(store, window), b = ewma.fit(*cache, window);
        ASSERT_TRUE(a); ASSERT_TRUE(b); same_bits(a->w, b->w); same_bits(a->tstat, b->tstat);
        const auto c = gk.fit(store, window), d = gk.fit(*cache, window);
        ASSERT_TRUE(c); ASSERT_TRUE(d); same_bits(c->w, d->w); same_bits(c->tstat, d->tstat);
      }
    }
  }
}

TEST(CombineSignalCubeConsumer, InvalidMaturityRecipeShapeAndBudgetFailBeforeFit) {
  const auto store = Fixture{}.store();
  cb::SignalIcCacheConfig cfg{5U, 1U, 40U};
  EXPECT_FALSE(cb::SignalIcCache::from_store(store, {0U, 35U}, cfg)); // last endpoint=40
  EXPECT_FALSE(cb::SignalIcCache::from_store(store, {0U, 34U}, cfg, 1U));
  auto cache = cb::SignalIcCache::from_store(store, {0U, 34U}, cfg);
  ASSERT_TRUE(cache);
  cb::IcirEwmaCombiner combiner;
  EXPECT_FALSE(combiner.fit(*cache, {0U, 30U})); // daily inference cannot consume five-day IC
  combiner.inference.label_horizon = 5U;
  EXPECT_TRUE(combiner.fit(*cache, {0U, 30U}));
  auto malformed = cache->view();
  malformed.values = malformed.values.first(1U);
  EXPECT_FALSE(combiner.fit(malformed, {0U, 30U}));
  malformed = cache->view(); malformed.config.maturity_end = 30U;
  EXPECT_FALSE(combiner.fit(malformed, {0U, 20U})); // cache itself includes unmatured labels
  combiner.inference.return_treatment = cb::IcReturnTreatment::RawV1;
  EXPECT_FALSE(combiner.fit(*cache, {0U, 30U}));
}

TEST(CombineSignalCubeConsumer, ExistingWalkForwardReusesMatureRowsWithExactParity) {
  const auto store = Fixture{}.store();
  cb::WalkForwardCfg cfg;
  cfg.horizon = 5U; cfg.min_train = 10U; cfg.lookback = 20U; cfg.refit_every = 3U;
  const cb::SignalInferenceConfig inference{atx::engine::eval::hac::TStatRule::HorizonAwareV3, 5U};
  const auto qualify = [&](const auto& combiner) {
    const auto cached = cb::walk_forward(store, cfg, combiner);
    auto original = cfg; original.ic_cache_max_bytes = 0U;
    const auto uncached = cb::walk_forward(store, original, combiner);
    ASSERT_TRUE(cached); ASSERT_TRUE(uncached);
    EXPECT_TRUE(cached->used_ic_cache); EXPECT_FALSE(uncached->used_ic_cache);
    same_path(*cached, *uncached);
    ASSERT_FALSE(cached->refit_dates.empty());
    EXPECT_EQ(cached->ic_rows_computed, cached->refit_dates.back() + 1U - cfg.horizon);
    usize repeated_rows = 0U;
    for (const auto d : cached->refit_dates) repeated_rows += std::min(cfg.lookback, d + 1U - cfg.horizon);
    EXPECT_LT(cached->ic_rows_computed, repeated_rows);
  };
  qualify(cb::IcirEwmaCombiner{13.0, 0.0, inference});
  qualify(cb::GrinoldKahnCombiner{cb::CovTarget::LwIdentity, inference});
}

TEST(CombineSignalCubeConsumer, CachedWalkForwardCannotReadUnmaturedFutureLabels) {
  constexpr usize decision = 35U, horizon = 5U;
  const Fixture original;
  Fixture changed = original;
  for (usize t = 0U; t < dates; ++t) for (usize i = 0U; i < names; ++i) {
    if (t + horizon > decision) changed.returns[t * names + i] = 100.0 + static_cast<f64>(i);
    if (t > decision) for (auto& signal : changed.signals) signal[t * names + i] *= -1.0;
  }
  cb::WalkForwardCfg cfg; cfg.horizon = horizon; cfg.min_train = 10U; cfg.lookback = 20U;
  cb::IcirEwmaCombiner combiner; combiner.inference.label_horizon = horizon;
  const auto a = cb::walk_forward(original.store(), cfg, combiner);
  const auto b = cb::walk_forward(changed.store(), cfg, combiner);
  ASSERT_TRUE(a); ASSERT_TRUE(b); EXPECT_TRUE(a->used_ic_cache); EXPECT_TRUE(b->used_ic_cache);
  ASSERT_TRUE(a->has_weights(decision));
  same_bits(std::span{a->w}.first((decision + 1U) * alphas),
            std::span{b->w}.first((decision + 1U) * alphas));
}

TEST(CombineSignalCubeConsumer, DelayedCacheWalkForwardMatchesLegacyEmbargoAndRejectsMismatch) {
  const auto store = Fixture{}.store();
  cb::SignalIcCacheConfig recipe{5U, 1U, dates};
  auto cache = cb::SignalIcCache::from_store(store, {0U, dates - 6U}, recipe);
  ASSERT_TRUE(cache);
  cb::IcirEwmaCombiner combiner; combiner.inference.label_horizon = 5U;
  cb::WalkForwardCfg cfg; cfg.horizon = 5U; cfg.min_train = 10U;
  const auto cached = cb::walk_forward(*cache, cfg, combiner);
  auto original = cfg; original.horizon = 6U; original.ic_cache_max_bytes = 0U;
  const auto reference = cb::walk_forward(store, original, combiner);
  ASSERT_TRUE(cached); ASSERT_TRUE(reference); same_path(*cached, *reference);
  cfg.horizon = 6U; EXPECT_FALSE(cb::walk_forward(*cache, cfg, combiner));
}

TEST(CombineSignalCubeConsumer, CubeAdapterUsesPrequantizationExactStatsAndExplicitLossyOptIn) {
  const auto store = Fixture{}.store();
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path directory;
  for (usize attempt = 0U; attempt < 32U && directory.empty(); ++attempt) {
    const auto candidate = std::filesystem::temp_directory_path() /
        ("atx_e1_ic_cache_consumer_" + std::to_string(stamp) + "_" + std::to_string(attempt));
    std::error_code error;
    const bool created = std::filesystem::create_directory(candidate, error);
    ASSERT_FALSE(error) << error.message();
    if (created) directory = candidate;
  }
  ASSERT_FALSE(directory.empty());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{directory};
  cb::SignalCubeConfig cfg;
  cfg.dates = dates; cfg.alphas = alphas; cfg.instruments = names;
  cfg.normalization = cb::CubeNormalize::AlreadyNormalizedV1;
  cfg.source_sha256 = std::string(64U, 'a');
  cfg.transform_identity = "synthetic-zscore-v2"; cfg.return_identity = "synthetic-delayed-h5";
  cfg.membership_identity = "all"; cfg.alpha_identities = {"a", "b", "c"};
  std::array<std::span<const f64>, 4> labels;
  labels[0] = store.forward_returns();
  ASSERT_TRUE(cb::write_signal_cube(store, directory / "exact", cfg, labels));
  auto cube = cb::SignalCube::open(directory / "exact"); ASSERT_TRUE(cube);
  auto cache = cb::SignalIcCache::from_cube(*cube, {0U, dates - 6U}, 0U); ASSERT_TRUE(cache);
  EXPECT_EQ(cache->source_manifest_sha256(), cube->manifest_sha256());
  same_bits(cache->view().values, cb::ic_matrix(store, {0U, dates - 6U}));
  cb::IcirEwmaCombiner combiner; combiner.inference.label_horizon = 5U;
  const auto a = combiner.fit(store, {5U, 60U}), b = combiner.fit(*cache, {5U, 60U});
  ASSERT_TRUE(a); ASSERT_TRUE(b); same_bits(a->w, b->w); same_bits(a->tstat, b->tstat);
  cfg.stat_precision = cb::CubeStatPrecision::Float32V1;
  ASSERT_TRUE(cb::write_signal_cube(store, directory / "float32", cfg, labels));
  auto lossy = cb::SignalCube::open(directory / "float32"); ASSERT_TRUE(lossy);
  EXPECT_FALSE(cb::SignalIcCache::from_cube(*lossy, {0U, dates - 6U}, 0U));
  auto explicit_lossy = cb::SignalIcCache::from_cube(*lossy, {0U, dates - 6U}, 0U, true);
  ASSERT_TRUE(explicit_lossy);
  EXPECT_EQ(explicit_lossy->view().config.precision, cb::SignalIcPrecision::Float32V1);
}
} // namespace atx_test_e1_ic_cache
