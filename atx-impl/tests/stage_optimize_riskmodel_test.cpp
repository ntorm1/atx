// stage_optimize_riskmodel_test.cpp — p8 S1-2: covariance-source swap in
// stage_optimize.
//
// Suite: AtxImplOptimizeRiskModel
//
// Tests (per sprint-1-risk-model-covariance.md S1-2 Accept):
//   * DiagonalByteIdentical  — run_optimize_with_risk_model(cfg, RiskModelConfig{})
//       (kind==Diagonal, the inert default) produces the EXACT SAME books digest
//       as the plain run_optimize(cfg) (today's unconditional diagonal_risk_model
//       path) -- off-path byte-identity, the determinism-contract gate.
//   * FactorDeleversVsDiagonal — on a correlated-group fixture (the same common-
//       shock construction S1-1 uses), kind==Factor produces a book with
//       strictly lower ex-ante factor risk wᵀVw (measured against the FACTOR
//       model itself) and lower gross on the crowded pair than the diagonal
//       book at equal alpha -- the optimizer hedges the shared factor.
//   * TwiceRunFactorByteIdentical — same inputs -> identical books digest on the
//       Factor path (determinism holds beyond the diagonal path too).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_riskmodel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_stage_optimize_riskmodel {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace risk = atx::engine::risk;

using atx::f64;
using atx::usize;

// A gently-trending panel identical in shape to optimize_test.cpp's fixture
// (kept local + minimal so this test file has no cross-file test dependency).
static atx::core::Result<std::string> make_trend_research(const fs::path& out, usize M, usize D) {
  std::vector<f64> close;
  close.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      const f64 drift = 0.0002 * (1.0 + static_cast<f64>(i) * 0.1);
      close.push_back(100.0 * std::exp(drift * static_cast<f64>(t)));
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// The S1-1 common-shock correlated-group fixture (every instrument shares a
// dominant common shock + small idiosyncratic noise) so a long/short book
// across the two halves is a genuine hedge the factor model, but not the
// diagonal model, can recognize.
static atx::core::Result<std::string> make_correlated_research(const fs::path& out, usize M,
                                                                usize D) {
  std::vector<f64> common_shock(D);
  for (usize t = 0; t < D; ++t) {
    common_shock[t] = 0.01 * std::sin(0.31 * static_cast<f64>(t));
  }
  std::vector<f64> close(D * M, 100.0);
  std::vector<f64> volume(D * M, 2'000'000.0);
  for (usize i = 0; i < M; ++i) {
    const f64 idio_amp = 0.0005 * (1.0 + static_cast<f64>(i % 7));
    f64 level = 100.0;
    for (usize t = 0; t < D; ++t) {
      const f64 ret = common_shock[t] + idio_amp * std::sin(0.9 * static_cast<f64>(t) +
                                                             static_cast<f64>(i));
      if (t > 0) level *= (1.0 + ret);
      close[t * M + i] = level;
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close", "volume"}, {close, volume}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

// combo: a fixed long-first-half / short-second-half alpha signal, CONSTANT
// across periods -- the "crowded pair" book the factor model should hedge.
static atx::core::Result<std::string> make_pair_combo(const fs::path& out, usize M, usize D) {
  std::vector<f64> alpha_data;
  alpha_data.reserve(D * M);
  const usize half = M / 2;
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      alpha_data.push_back(i < half ? 1.0 : -1.0);
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

class AtxImplOptimizeRiskModel : public ::testing::Test {
protected:
  fs::path tmp_dir_;

  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_impl_optimize_riskmodel_test";
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
};

// ---------------------------------------------------------------------------
// Test 1: off-path byte-identity -- the mandatory determinism-contract gate.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimizeRiskModel, DiagonalByteIdentical) {
  constexpr usize M = 12, D = 60;
  const fs::path research_path = tmp_dir_ / "research.bin";
  const fs::path combo_path = tmp_dir_ / "combo.bin";
  ASSERT_TRUE(make_trend_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 0.5;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  cfg.books_out = (tmp_dir_ / "books_legacy.bin").string();
  auto legacy_r = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(legacy_r.has_value()) << legacy_r.error().message();

  cfg.books_out = (tmp_dir_ / "books_inert.bin").string();
  auto inert_r = atx::impl::run_optimize(cfg, risk::RiskModelConfig{});
  ASSERT_TRUE(inert_r.has_value()) << inert_r.error().message();

  EXPECT_EQ(legacy_r->digest, inert_r->digest)
      << "explicit inert RiskModelConfig{} (kind=Diagonal) must reproduce the "
         "SAME books digest as the plain run_optimize(cfg) call -- routing leak";

  std::ifstream fa((tmp_dir_ / "books_legacy.bin").string(), std::ios::binary);
  std::ifstream fb((tmp_dir_ / "books_inert.bin").string(), std::ios::binary);
  const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  EXPECT_EQ(da, db) << "books.bin not byte-identical between the legacy and inert-Factor-config paths";
}

// ---------------------------------------------------------------------------
// Test 2: Factor de-levers the crowded pair vs the diagonal book.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimizeRiskModel, FactorDeleversVsDiagonal) {
  constexpr usize M = 10, D = 160;
  const fs::path research_path = tmp_dir_ / "research_corr.bin";
  const fs::path combo_path = tmp_dir_ / "combo_pair.bin";
  ASSERT_TRUE(make_correlated_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0; // uncapped: let the optimizer freely re-tilt the book
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  risk::RiskModelConfig diag_cfg; // Diagonal
  cfg.books_out = (tmp_dir_ / "books_diag.bin").string();
  auto diag_sr = atx::impl::run_optimize(cfg, diag_cfg);
  ASSERT_TRUE(diag_sr.has_value()) << diag_sr.error().message();

  risk::RiskModelConfig factor_cfg;
  factor_cfg.kind = risk::RiskModelKind::Factor;
  factor_cfg.fit_lookback_days = 100U;
  factor_cfg.style_mom = false;
  factor_cfg.style_beta = false;
  factor_cfg.style_size = false;
  factor_cfg.industry = false;
  // NOTE: without a group_map this cfg gives K=1 (Volatility only); the POINT
  // of this test is the BOOK comparison (gross on the crowded pair, ex-ante
  // risk against the FACTOR model), which only needs kind==Factor to route
  // through stage_riskmodel's real estimator -- not a specific K.
  cfg.books_out = (tmp_dir_ / "books_factor.bin").string();
  auto factor_sr = atx::impl::run_optimize(cfg, factor_cfg);
  ASSERT_TRUE(factor_sr.has_value()) << factor_sr.error().message();

  // Reload both books' last period and compare gross on the crowded pair +
  // ex-ante factor risk wᵀVw evaluated against the FACTOR model (the model
  // the optimizer that produced books_factor actually saw).
  auto diag_books_r = atx::impl::read_panel((tmp_dir_ / "books_diag.bin").string());
  auto factor_books_r = atx::impl::read_panel((tmp_dir_ / "books_factor.bin").string());
  ASSERT_TRUE(diag_books_r.has_value());
  ASSERT_TRUE(factor_books_r.has_value());

  auto diag_wfid = diag_books_r->field_id("weight");
  auto factor_wfid = factor_books_r->field_id("weight");
  ASSERT_TRUE(diag_wfid.has_value());
  ASSERT_TRUE(factor_wfid.has_value());

  const atx::usize last_period = factor_books_r->dates() - 1;
  const auto diag_w = diag_books_r->field_cross_section(*diag_wfid, last_period);
  const auto factor_w = factor_books_r->field_cross_section(*factor_wfid, last_period);
  ASSERT_EQ(diag_w.size(), M);
  ASSERT_EQ(factor_w.size(), M);

  // Build the SAME factor model books_factor's stage saw, to price BOTH books
  // ex-ante under it (a fair head-to-head: same V, different w).
  auto research_r = atx::impl::read_panel(research_path.string());
  ASSERT_TRUE(research_r.has_value());
  auto factor_artifact_r = atx::impl::build_risk_model(*research_r, factor_cfg);
  ASSERT_TRUE(factor_artifact_r.has_value()) << factor_artifact_r.error().message();
  auto factor_model_r = atx::engine::data::artifact_to_factor_model(*factor_artifact_r);
  ASSERT_TRUE(factor_model_r.has_value()) << factor_model_r.error().message();

  std::vector<f64> diag_w_vec(diag_w.begin(), diag_w.end());
  std::vector<f64> factor_w_vec(factor_w.begin(), factor_w.end());
  const f64 diag_book_risk_under_factor_v = factor_model_r->risk(diag_w_vec);
  const f64 factor_book_risk_under_factor_v = factor_model_r->risk(factor_w_vec);

  EXPECT_LT(factor_book_risk_under_factor_v, diag_book_risk_under_factor_v)
      << "expected the Factor-routed book to have LOWER ex-ante risk under the "
         "factor model than the diagonal-routed book (same alpha, same V): "
      << "factor_book_risk=" << factor_book_risk_under_factor_v
      << " diag_book_risk=" << diag_book_risk_under_factor_v;

  // Gross on the crowded pair (sum |w| over all names, since the whole book IS
  // the crowded pair here) must also be lower or equal for the Factor route --
  // the optimizer sizing down the shared-factor bet.
  auto gross_of = [](const std::vector<f64>& w) {
    f64 g = 0.0;
    for (f64 wi : w) g += std::fabs(wi);
    return g;
  };
  EXPECT_LE(gross_of(factor_w_vec), gross_of(diag_w_vec) + 1e-9)
      << "expected the Factor book's gross on the crowded pair to be <= the diagonal book's";
}

// ---------------------------------------------------------------------------
// Test 3: twice-run determinism on the Factor path.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimizeRiskModel, TwiceRunFactorByteIdentical) {
  constexpr usize M = 10, D = 160;
  const fs::path research_path = tmp_dir_ / "research_corr2.bin";
  const fs::path combo_path = tmp_dir_ / "combo_pair2.bin";
  ASSERT_TRUE(make_correlated_research(research_path, M, D).has_value());
  ASSERT_TRUE(make_pair_combo(combo_path, M, D).has_value());

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  risk::RiskModelConfig factor_cfg;
  factor_cfg.kind = risk::RiskModelKind::Factor;
  factor_cfg.fit_lookback_days = 100U;
  factor_cfg.style_mom = false;
  factor_cfg.style_beta = false;
  factor_cfg.style_size = false;
  factor_cfg.industry = false;

  cfg.books_out = (tmp_dir_ / "books_a.bin").string();
  auto r1 = atx::impl::run_optimize(cfg, factor_cfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();

  cfg.books_out = (tmp_dir_ / "books_b.bin").string();
  auto r2 = atx::impl::run_optimize(cfg, factor_cfg);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  EXPECT_EQ(r1->digest, r2->digest);
  std::ifstream fa((tmp_dir_ / "books_a.bin").string(), std::ios::binary);
  std::ifstream fb((tmp_dir_ / "books_b.bin").string(), std::ios::binary);
  const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  EXPECT_EQ(da, db);
}

} // namespace atxtest_stage_optimize_riskmodel
