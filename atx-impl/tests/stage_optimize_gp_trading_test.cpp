// stage_optimize_gp_trading_test.cpp — p9 S3-1/S3-2: GP aim-portfolio trade wiring.
//
// Suite: AtxImplOptimizeGpTrading
//
// (d) seq==parallel: N/A for this sprint -- the position-mode trade loop is an inherently
// sequential per-period state machine (w[s] depends on prev=w[s-1]); no parallel_for/executor
// touches stage_optimize.cpp's position-mode branch before or after S3. See sprint-3's
// Determinism contract section for the full justification.
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/core/types.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_stage_optimize_gp_trading {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using atx::f64;
using atx::usize;

class AtxImplOptimizeGpTrading : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_impl_gp_trading_test";
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
};

// GP must move the book when set. The GP aim (2λV)^-1·α reweights each name by
// 1/D_i (its own return variance); the legacy shape_book target does not. For a
// 2-name dollar-neutral book any reweighting is undone by the ±0.5 symmetry the
// demean forces, so this needs M>=4 with GENUINELY DIFFERING per-name variances:
// names 0-1 are smooth (variance hits diag_risk's 1e-4 floor) while names 2-3
// carry a large / medium deterministic oscillation (variance well above the
// floor and distinct from each other). The GP aim therefore down-weights the
// high-variance names relative to the raw target -> a different book -> a
// different digest. Pre-wire, gp_trading is unread so both runs are the legacy
// book (EQUAL digest -> RED); post-wire the digests diverge (GREEN).
TEST_F(AtxImplOptimizeGpTrading, GpTradingChangesBookWhenSet) {
  constexpr usize M = 4, D = 40;
  std::vector<f64> close(D * M);
  for (usize t = 0; t < D; ++t) {
    const f64 tf = static_cast<f64>(t);
    close[t * M + 0] = 100.0 * std::exp(0.0010 * tf);              // smooth up  -> D=1e-4
    close[t * M + 1] = 100.0 * std::exp(-0.0008 * tf);             // smooth down-> D=1e-4
    close[t * M + 2] = 100.0 * (1.0 + 0.10 * std::sin(1.1 * tf));  // high var  -> D>>1e-4
    close[t * M + 3] = 100.0 * (1.0 + 0.04 * std::cos(0.7 * tf));  // med var   -> D> 1e-4
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
  ASSERT_TRUE(rp.has_value());
  const std::string research_path = (tmp_dir_ / "research.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());

  std::vector<f64> combo(D * M);
  for (usize t = 0; t < D; ++t) {
    combo[t * M + 0] = 1.0; combo[t * M + 1] = -1.0;
    combo[t * M + 2] = 1.0; combo[t * M + 3] = -1.0;
  }
  auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
  ASSERT_TRUE(cp.has_value());
  const std::string combo_path = (tmp_dir_ / "combo.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path;
  cfg.combo = combo_path;
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "daily";
  cfg.position_mode = true;
  cfg.trade_rate = 0.4;
  cfg.set_flags.emplace("trade-rate");

  cfg.books_out = (tmp_dir_ / "books_legacy.bin").string();
  auto legacy = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().message();

  cfg.gp_trading = true;
  cfg.gp_risk_aversion = 1.0;
  cfg.books_out = (tmp_dir_ / "books_gp.bin").string();
  auto gp = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(gp.has_value()) << gp.error().message();

  EXPECT_NE(legacy->digest, gp->digest)
      << "gp_trading=true must route through a different partial-trade step than the "
         "legacy linear blend on a fixture where the risk model is non-trivial";
}

} // namespace atxtest_stage_optimize_gp_trading
