// atx-engine/tests/factory/search_quality_test.cpp
#include <gtest/gtest.h>
#include <vector>
#include <string>
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx::engine::factory {
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::WeightPolicy;
using atx::engine::combine::AlphaStore;

// --- Fixture (mirrors factory_search_driver_test.cpp) ---------------------
namespace {
ExecutionSimulator frictionless_sim() {
  using namespace atx::engine::exec;
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{}, VolumeCapCfg{1.0}};
}
struct Lcg { std::uint64_t s;
  double next() noexcept { s = s*6364136223846793005ULL + 1442695040888963407ULL;
    return 2.0*(static_cast<double>(s>>11U)/static_cast<double>(1ULL<<53U)) - 1.0; } };
Panel fixture_panel(atx::usize dates, atx::usize insts) {
  std::vector<double> drift(insts);
  for (atx::usize j=0;j<insts;++j) drift[j]=0.006-0.0024*static_cast<double>(j);
  std::vector<double> close(dates*insts), px(insts,100.0); Lcg rng{0xA11Cu};
  for (atx::usize t=0;t<dates;++t) for (atx::usize j=0;j<insts;++j){
    px[j]*=(1.0+drift[j]+0.010*rng.next()); close[t*insts+j]=px[j]; }
  std::vector<double> rev(dates*insts,0.0);
  for (atx::usize t=1;t<dates;++t) for (atx::usize j=0;j<insts;++j){
    rev[t*insts+j] = -(close[t*insts+j]/close[(t-1)*insts+j]-1.0); }
  auto r = Panel::create(dates, insts, {"close","rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value()); return std::move(r.value());
}
std::vector<std::string> seed_exprs() {
  return {"rank(close)","rank(rev)","ts_mean(close, 5)","ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))","delta(close, 2)"}; }
struct Fixture {
  Library lib{}; Panel panel = fixture_panel(96,6); WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver() { return SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close","rev"}}; }
};
SearchConfig base_cfg(atx::u64 seed) {
  SearchConfig c; c.master_seed=seed; c.population=16; c.generations=4;
  c.elites=2; c.k_tournament=3; c.p_cross=0.5; return c; }
} // namespace

// Behavioral novelty now toggles via enable_behavioral_novelty (NOT novelty_w).
TEST(SearchNoveltyKnob, BehavioralNoveltyTogglesDigest) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool_on{}, pool_off{};
  SearchConfig on = base_cfg(777);  on.objective_mode = ObjectiveMode::MultiObjective;
  on.enable_behavioral_novelty = true;
  SearchConfig off = on; off.enable_behavioral_novelty = false;
  const auto r_on  = d.run(on,  pool_on);
  const auto r_off = d.run(off, pool_off);
  // Behavioral novelty is a live 4th objective when ON -> different survivor set/digest.
  EXPECT_NE(r_on.digest, r_off.digest);
}
} // namespace atx::engine::factory
