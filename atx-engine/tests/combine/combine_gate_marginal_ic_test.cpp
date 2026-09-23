// combine_gate_marginal_ic_test.cpp — Lane 5: AlphaGate marginal-IC admission mode.
//
//   * Default config: byte-identical verdicts (a perfect PnL copy is RejectCorrelated).
//   * Mode on: the PnL-correlation screen is replaced — a PnL copy with marginal IC
//     above the floor is admitted, one below is RejectCorrelated.
//   * Mode on but no marginal_ic supplied (NaN sentinel) fails CLOSED.
//   * Floors (fitness etc.) still run first in marginal mode.
//   * Positional 3-field GateDeflation brace-init (the library tests' form) still
//     compiles under /WX and leaves the new field at its sentinel.
//
// Suite: AlphaGateMarginalIc

#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"

namespace atx_test_l5_combine_gate_marginal_ic {

using atx::f64;
namespace cb = atx::engine::combine;

cb::AlphaMetrics good_metrics() {
  return cb::AlphaMetrics{/*sharpe*/ 2.0,   /*turnover*/ 0.30, /*returns*/ 0.10,
                          /*drawdown*/ 0.1, /*margin*/ 1.0,    /*fitness*/ 2.0,
                          /*holding_days*/ 3.3};
}

struct Fixture {
  cb::AlphaStore pool;
  std::vector<f64> member{0.0, 0.01, -0.02, 0.03, 0.04};
  Fixture() {
    const std::vector<f64> pos(member.size(), 0.0);
    EXPECT_TRUE(pool.insert(nullptr, member, pos, cb::AlphaMetrics{}).has_value());
  }
};

TEST(AlphaGateMarginalIc, DefaultConfigUnchanged) {
  const Fixture fx;
  const cb::AlphaGate gate{cb::GateConfig{}};
  EXPECT_FALSE(gate.cfg.use_marginal_ic);
  EXPECT_EQ(gate.admit(good_metrics(), fx.member, fx.pool), cb::GateVerdict::RejectCorrelated);
  cb::GateDeflation d;
  d.marginal_ic = 0.5; // ignored when the mode is off
  EXPECT_EQ(gate.admit(good_metrics(), fx.member, fx.pool, d), cb::GateVerdict::RejectCorrelated);
}

TEST(AlphaGateMarginalIc, ReplacesCorrelationScreen) {
  const Fixture fx;
  cb::GateConfig cfg;
  cfg.use_marginal_ic = true;
  cfg.min_marginal_ic = 0.01;
  const cb::AlphaGate gate{cfg};
  cb::GateDeflation d;
  d.marginal_ic = 0.02;
  EXPECT_EQ(gate.admit(good_metrics(), fx.member, fx.pool, d), cb::GateVerdict::Accept);
  d.marginal_ic = 0.005;
  EXPECT_EQ(gate.admit(good_metrics(), fx.member, fx.pool, d), cb::GateVerdict::RejectCorrelated);
}

TEST(AlphaGateMarginalIc, UnsuppliedMarginalIcFailsClosed) {
  const Fixture fx;
  cb::GateConfig cfg;
  cfg.use_marginal_ic = true;
  const cb::AlphaGate gate{cfg};
  EXPECT_TRUE(std::isnan(cb::kInertDeflation.marginal_ic));
  EXPECT_EQ(gate.admit(good_metrics(), fx.member, fx.pool), cb::GateVerdict::RejectCorrelated);
  const cb::GateDeflation positional{/*dsr*/ 0.9, /*pbo*/ 0.1, /*split_stable*/ true};
  EXPECT_TRUE(std::isnan(positional.marginal_ic));
}

TEST(AlphaGateMarginalIc, FloorsStillRunFirst) {
  const Fixture fx;
  cb::GateConfig cfg;
  cfg.use_marginal_ic = true;
  const cb::AlphaGate gate{cfg};
  cb::AlphaMetrics m = good_metrics();
  m.fitness = 0.1;
  cb::GateDeflation d;
  d.marginal_ic = 1.0;
  EXPECT_EQ(gate.admit(m, fx.member, fx.pool, d), cb::GateVerdict::RejectFitness);
}

} // namespace atx_test_l5_combine_gate_marginal_ic
