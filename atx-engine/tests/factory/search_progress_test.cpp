// atx::engine::factory — SearchDriver population serialize/deserialize tests
// (resumable-discover, Task 1).
//
// Tests that serialize_population / deserialize_population round-trip a
// population faithfully: the multiset of canon_hashes after
// deserialize(serialize(pop)) equals the multiset of the original pop.
//
// Because both helpers are private, we reach them through a friend accessor
// struct declared here and friended in SearchDriver.

#include <algorithm> // std::sort
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

// ---- test-access friend -------------------------------------------------------
// Must live in atx::engine::factory so the unqualified friend declaration in
// SearchDriver (search_driver.hpp) resolves to this struct.
namespace atx::engine::factory {

// This struct is declared as a friend inside SearchDriver (see search_driver.hpp).
// It calls private members on the driver so the test can exercise them without
// exposing them publicly.
struct SearchProgressTestAccess {
  static std::vector<Genome> init_population(const SearchDriver &drv,
                                             const SearchConfig &cfg) {
    return drv.init_population(cfg);
  }

  static std::vector<std::string>
  serialize_population(const SearchDriver &drv, const std::vector<Genome> &pop) {
    return drv.serialize_population(pop);
  }

  static atx::core::Result<std::vector<Genome>>
  deserialize_population(const SearchDriver &drv,
                         const std::vector<std::string> &exprs) {
    return drv.deserialize_population(exprs);
  }
};

} // namespace atx::engine::factory

namespace atxtest_search_progress_test {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchProgressTestAccess;
using atx::engine::WeightPolicy;

// ---- fixture helpers (mirrors factory_search_driver_test.cpp) -----------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] Panel fixture_panel(usize dates, usize insts) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{0xA11Cu};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + 0.006 - 0.0024 * static_cast<f64>(j) + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

struct Fixture {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  [[nodiscard]] SearchDriver driver() {
    // Two distinct in-grammar seed expressions (rank and ts_mean over different
    // fields) so init_population produces a population with >= 2 distinct genomes.
    return SearchDriver{lib,
                        panel,
                        policy,
                        sim,
                        {"rank(close)", "ts_mean(rev, 3)"},
                        {"close", "rev"}};
  }
};

// =============================================================================
//  PopulationRoundTrip — serialize then deserialize produces the same multiset
//  of canon_hashes (and the same count) as the original population.
// =============================================================================
TEST(SearchProgress, PopulationRoundTrip) {
  Fixture fx;
  SearchDriver drv = fx.driver();

  // Build a real population via init_population (4 slots, seeds cycle correctly).
  SearchConfig cfg;
  cfg.master_seed = 0;
  cfg.population = 4;
  cfg.generations = 1;
  const std::vector<Genome> pop =
      SearchProgressTestAccess::init_population(drv, cfg);
  ASSERT_FALSE(pop.empty()) << "init_population must produce at least one genome";

  // Serialize.
  const std::vector<std::string> exprs =
      SearchProgressTestAccess::serialize_population(drv, pop);
  ASSERT_EQ(exprs.size(), pop.size()) << "serialize must emit one string per genome";

  // Deserialize.
  auto deser_result = SearchProgressTestAccess::deserialize_population(drv, exprs);
  ASSERT_TRUE(deser_result.has_value())
      << "deserialize_population must succeed: "
      << (deser_result.has_value() ? "" : deser_result.error().message());
  const std::vector<Genome> &round = deser_result.value();
  ASSERT_EQ(round.size(), pop.size()) << "round-trip count must match";

  // Compare multisets of canon_hashes (order may differ).
  std::vector<u64> orig_hashes;
  orig_hashes.reserve(pop.size());
  for (const auto &g : pop) {
    orig_hashes.push_back(g.canon_hash);
  }
  std::vector<u64> rt_hashes;
  rt_hashes.reserve(round.size());
  for (const auto &g : round) {
    rt_hashes.push_back(g.canon_hash);
  }
  std::sort(orig_hashes.begin(), orig_hashes.end());
  std::sort(rt_hashes.begin(), rt_hashes.end());
  EXPECT_EQ(orig_hashes, rt_hashes)
      << "round-trip must reproduce the same multiset of canon_hashes";
}

} // namespace atxtest_search_progress_test
