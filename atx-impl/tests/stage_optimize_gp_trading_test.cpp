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
#include <string>
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

// (a) Off-path byte-identity: an implicit-default run must equal a run with every
// new field set EXPLICITLY to its inert value (proves the fields are inert even
// when PRESENT, not merely when absent -- a routing-leak guard).
TEST_F(AtxImplOptimizeGpTrading, OffPathByteIdentical) {
  constexpr usize M = 4, D = 60;
  std::vector<f64> close(D * M);
  for (usize t = 0; t < D; ++t)
    for (usize i = 0; i < M; ++i)
      close[t * M + i] = 100.0 * std::exp(0.0003 * (1.0 + 0.2 * static_cast<f64>(i)) * static_cast<f64>(t));
  std::vector<std::uint8_t> uni(D * M, 1u);
  auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
  ASSERT_TRUE(rp.has_value());
  const std::string research_path = (tmp_dir_ / "research_a.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());

  std::vector<f64> combo(D * M);
  for (usize t = 0; t < D; ++t)
    for (usize i = 0; i < M; ++i)
      combo[t * M + i] = (i % 2 == 0) ? 1.0 : -1.0;
  auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
  ASSERT_TRUE(cp.has_value());
  const std::string combo_path = (tmp_dir_ / "combo_a.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path;
  cfg.combo = combo_path;
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "daily";
  cfg.position_mode = true;
  cfg.trade_rate = 0.5;
  cfg.set_flags.emplace("trade-rate");

  cfg.books_out = (tmp_dir_ / "books_implicit.bin").string();
  auto implicit_r = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(implicit_r.has_value()) << implicit_r.error().message();

  cfg.gp_trading = false;
  cfg.gp_risk_aversion = 0.0;
  cfg.gp_trade_cost_scale = 0.0;
  cfg.books_out = (tmp_dir_ / "books_explicit_inert.bin").string();
  auto explicit_r = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(explicit_r.has_value()) << explicit_r.error().message();

  EXPECT_EQ(implicit_r->digest, explicit_r->digest)
      << "explicit inert gp_trading/gp_risk_aversion/gp_trade_cost_scale must reproduce the "
         "SAME digest as the implicit (never-touched) defaults -- routing leak";

  std::ifstream fa((tmp_dir_ / "books_implicit.bin").string(), std::ios::binary);
  std::ifstream fb((tmp_dir_ / "books_explicit_inert.bin").string(), std::ios::binary);
  const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  EXPECT_EQ(da, db) << "books.bin not byte-identical between implicit and explicit-inert configs";
}

// (b) Mean-reverting turnover proof: GP realizes strictly LOWER cumulative turnover
// than the legacy linear blend at matched-or-better capital on the true-edge names.
// Names 0-1 are stable/low-noise with a genuine constant-sign edge (variance hits
// diag_risk's 1e-4 floor); names 2-3 are HIGH-variance, zero-drift, mean-reverting
// (alpha flips sign every period). The GP aim divides by each name's own return
// variance, so the noisy flippers 2-3 are damped -- GP stops chasing their
// every-period flip at full rate, while the legacy target re-shapes and fully
// deploys them each period. trade_rate=1.0 isolates the aim-vs-target choice itself.
TEST_F(AtxImplOptimizeGpTrading, GpLowersTurnoverAtMatchedOrBetterSharpe) {
  constexpr usize M = 4, D = 120;
  struct Lcg {
    std::uint64_t s;
    f64 next() noexcept {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return 2.0 * (static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U)) - 1.0;
    }
  };
  Lcg rng{0xC0FFEEULL};
  std::vector<f64> close(D * M, 100.0);
  std::vector<f64> combo(D * M, 0.0);
  std::vector<f64> px(M, 100.0);
  for (usize t = 0; t < D; ++t) {
    // Names 0-1: stable, low-noise, genuinely profitable, constant-sign alpha.
    for (usize i = 0; i < 2; ++i) {
      const f64 sign = (i == 0) ? 1.0 : -1.0;
      const f64 ret = sign * 0.0015 + 0.0005 * rng.next();
      px[i] *= (1.0 + ret);
      close[t * M + i] = px[i];
      combo[t * M + i] = sign;
    }
    // Names 2-3: noisy, zero-drift, mean-reverting (flipping) alpha, zero true edge.
    for (usize i = 2; i < M; ++i) {
      const f64 ret = 0.05 * rng.next(); // high vol, zero mean
      px[i] *= (1.0 + ret);
      close[t * M + i] = px[i];
      combo[t * M + i] = (t % 2 == 0) ? 1.0 : -1.0;
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
  ASSERT_TRUE(rp.has_value());
  const std::string research_path = (tmp_dir_ / "research_mr.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());
  auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
  ASSERT_TRUE(cp.has_value());
  const std::string combo_path = (tmp_dir_ / "combo_mr.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

  auto total_turnover = [](const std::string& meta_path) {
    std::ifstream f(meta_path);
    std::string line;
    f64 total = 0.0;
    while (std::getline(f, line)) {
      const auto pos = line.find("turnover=");
      if (pos == std::string::npos) continue;
      const auto end = line.find(' ', pos);
      total += std::stod(line.substr(pos + 9, end - (pos + 9)));
    }
    return total;
  };

  atx::impl::RunConfig cfg;
  cfg.panel = research_path;
  cfg.combo = combo_path;
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "daily";
  cfg.position_mode = true;
  cfg.trade_rate = 1.0; // full step both ways -- isolates the aim vs. target choice itself

  cfg.books_out = (tmp_dir_ / "books_legacy_mr.bin").string();
  auto legacy = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().message();
  const f64 legacy_turnover = total_turnover(cfg.books_out + ".meta.txt");

  cfg.gp_trading = true;
  cfg.gp_risk_aversion = 0.5;
  cfg.books_out = (tmp_dir_ / "books_gp_mr.bin").string();
  auto gp = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(gp.has_value()) << gp.error().message();
  const f64 gp_turnover = total_turnover(cfg.books_out + ".meta.txt");

  // "Matched-or-better Sharpe": realized book alignment with the persistent-edge names
  // (0-1) must not be worse under GP -- the noisy names carry no true edge, so damping
  // them costs nothing. Proxy: time-avg (|w0|+|w1|), the capital kept on the real edge.
  auto avg_edge_weight = [&](const std::string& books_path) {
    auto br = atx::impl::read_panel(books_path);
    if (!br.has_value()) return 0.0;
    auto wfid = br->field_id("weight");
    if (!wfid.has_value()) return 0.0;
    f64 total = 0.0;
    for (usize s = 0; s < br->dates(); ++s) {
      const auto row = br->field_cross_section(*wfid, s);
      total += std::fabs(row[0]) + std::fabs(row[1]);
    }
    return total / static_cast<f64>(br->dates());
  };
  const f64 legacy_edge = avg_edge_weight((tmp_dir_ / "books_legacy_mr.bin").string());
  const f64 gp_edge     = avg_edge_weight((tmp_dir_ / "books_gp_mr.bin").string());

  EXPECT_LT(gp_turnover, legacy_turnover)
      << "GP-wired trading must realize strictly lower cumulative turnover than the legacy "
         "linear blend on the mean-reverting/noisy fixture (gp=" << gp_turnover
      << " legacy=" << legacy_turnover << ")";

  EXPECT_GE(gp_edge, legacy_edge - 1e-6)
      << "GP must not reduce capital on the genuinely profitable names (0-1) vs legacy "
         "(gp_edge=" << gp_edge << " legacy_edge=" << legacy_edge << ")";
}

// (c) Twice-run determinism on the GP path: same panel/config -> identical digest
// and identical books.bin bytes (GP is an order-fixed pure-function chain, no
// RNG/clock/map -- garleanu_pedersen.hpp Determinism section).
TEST_F(AtxImplOptimizeGpTrading, TwiceRunByteIdentical) {
  constexpr usize M = 2, D = 20;
  std::vector<f64> close(D * M);
  for (usize t = 0; t < D; ++t) {
    close[t * M + 0] = 100.0 * std::exp(0.001 * static_cast<f64>(t));
    close[t * M + 1] = 100.0 * std::exp(-0.0005 * static_cast<f64>(t));
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
  ASSERT_TRUE(rp.has_value());
  const std::string research_path = (tmp_dir_ / "research_tw.bin").string();
  ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());
  std::vector<f64> combo(D * M);
  for (usize t = 0; t < D; ++t) { combo[t * M + 0] = 1.0; combo[t * M + 1] = -1.0; }
  auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
  ASSERT_TRUE(cp.has_value());
  const std::string combo_path = (tmp_dir_ / "combo_tw.bin").string();
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
  cfg.gp_trading = true;
  cfg.gp_risk_aversion = 1.0;
  cfg.gp_trade_cost_scale = 0.25;

  cfg.books_out = (tmp_dir_ / "books_tw_a.bin").string();
  auto r1 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  cfg.books_out = (tmp_dir_ / "books_tw_b.bin").string();
  auto r2 = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_EQ(r1->digest, r2->digest);
  std::ifstream fa((tmp_dir_ / "books_tw_a.bin").string(), std::ios::binary);
  std::ifstream fb((tmp_dir_ / "books_tw_b.bin").string(), std::ios::binary);
  const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  EXPECT_EQ(da, db);
}

} // namespace atxtest_stage_optimize_gp_trading
