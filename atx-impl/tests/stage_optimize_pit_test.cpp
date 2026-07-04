// stage_optimize_pit_test.cpp — p8 S1 fix-loop: production-path PIT test.
//
// Suite: AtxImplOptimizePit
//
// The S1 task review found the Factor risk-model path has a look-ahead:
// build_risk_model fits ONE factor model over the WHOLE research panel
// (fit_end == research.dates()) and stage_optimize.cpp applies THAT single
// model to EVERY rebalance period via model_at. So a rebalance decision at an
// EARLY period is silently informed by data from LATER dates.
//
// This test exercises the REAL run_optimize(cfg, risk_cfg) production call
// path (file-backed research/combo panels, the actual RunConfig entry point
// -- not a direct build_risk_model unit call) and proves the point-in-time
// contract: perturbing research rows STRICTLY AFTER an interior rebalance
// date t* must not change the book at any rebalance step whose period <= t*.
//
// RED (pre-fix): build_risk_model's single whole-panel fit reads the
// perturbed tail directly into its estimation window, so EVERY period's book
// changes, including period 0 -- the assertion fails at the very first
// checked step.
// GREEN (post-fix): each rebalance step's model is fit at fit_end ==
// period+1, so a step whose period <= t* only ever reads rows <= t*, which
// are byte-identical between the baseline and perturbed panels -- the book at
// that step is therefore identical too.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_riskmodel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_stage_optimize_pit {

namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace risk = atx::engine::risk;

using atx::f64;
using atx::usize;

namespace {

// D dates, M instruments; every instrument gets a distinct gentle drift (same
// family as optimize_test.cpp's make_research_panel), so per-name volatility
// differs and the Factor path's Volatility style column is well-defined.
[[nodiscard]] std::vector<f64> make_close(usize M, usize D) {
  std::vector<f64> close;
  close.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    for (usize i = 0; i < M; ++i) {
      const f64 drift = 0.0002 * (1.0 + static_cast<f64>(i) * 0.1);
      close.push_back(100.0 * std::exp(drift * static_cast<f64>(t)));
    }
  }
  return close;
}

[[nodiscard]] atx::core::Result<std::string>
write_research(const fs::path& out, const std::vector<f64>& close, usize M, usize D) {
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"close"}, {close}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

[[nodiscard]] atx::core::Result<std::string> make_combo(const fs::path& out, usize M, usize D) {
  std::vector<f64> alpha_data;
  alpha_data.reserve(D * M);
  for (usize t = 0; t < D; ++t) {
    const f64 wobble = 0.01 * static_cast<f64>(t % 5);
    for (usize i = 0; i < M; ++i) {
      alpha_data.push_back((static_cast<f64>(i) - static_cast<f64>(M) / 2.0) + wobble);
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1u);
  ATX_TRY(auto panel, alpha::Panel::create(D, M, {"alpha"}, {alpha_data}, uni));
  ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
  (void)digest;
  return atx::core::Ok(out.string());
}

} // namespace

class AtxImplOptimizePit : public ::testing::Test {
protected:
  fs::path tmp_dir_;
  void SetUp() override {
    tmp_dir_ = fs::temp_directory_path() / "atx_impl_optimize_pit_test";
    fs::create_directories(tmp_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir_, ec);
  }
};

// ---------------------------------------------------------------------------
// The key production-path PIT test.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimizePit, FactorPathIgnoresRowsAfterRebalanceDate) {
  constexpr usize M = 8, D = 300;
  constexpr usize step = 5;      // weekly rebalance cadence (matches stage_optimize.cpp)
  constexpr usize t_star = 200;  // interior rebalance date, a multiple of step, with
                                 // ample trailing history (fit_lookback_days below).

  const fs::path combo_path = tmp_dir_ / "combo.bin";
  ASSERT_TRUE(make_combo(combo_path, M, D).has_value());

  // Panel A: the baseline.
  const std::vector<f64> close_a = make_close(M, D);
  const fs::path research_a_path = tmp_dir_ / "research_a.bin";
  ASSERT_TRUE(write_research(research_a_path, close_a, M, D).has_value());

  // Panel B: BYTE-IDENTICAL to A on [0, t_star+1); rows > t_star continue
  // SMOOTHLY from close_a's level at t_star (no discontinuous jump -- so the
  // per-date return series stays realistic / well-conditioned) but with a
  // markedly different (reversed-sign, larger-magnitude) drift, so a fit
  // window that reads into this region estimates a genuinely different
  // (X, F, D) than one that does not.
  std::vector<f64> close_b = close_a;
  for (usize i = 0; i < M; ++i) {
    const f64 base = close_a[t_star * M + i];
    const f64 alt_drift = -0.0010 * (1.0 + static_cast<f64>(i) * 0.1);
    for (usize t = t_star + 1; t < D; ++t) {
      const f64 dt = static_cast<f64>(t - t_star);
      close_b[t * M + i] = base * std::exp(alt_drift * dt);
    }
  }
  const fs::path research_b_path = tmp_dir_ / "research_b.bin";
  ASSERT_TRUE(write_research(research_b_path, close_b, M, D).has_value());

  risk::RiskModelConfig risk_cfg;
  risk_cfg.kind = risk::RiskModelKind::Factor;
  risk_cfg.fit_lookback_days = 60U;
  risk_cfg.style_mom = false;
  risk_cfg.style_beta = false;
  risk_cfg.style_size = false;
  risk_cfg.industry = false; // style_vol stays true (default) -> K=1, real per-date estimation

  atx::impl::RunConfig cfg;
  cfg.combo = combo_path.string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  cfg.panel = research_a_path.string();
  cfg.books_out = (tmp_dir_ / "books_a.bin").string();
  auto result_a = atx::impl::run_optimize(cfg, risk_cfg);
  ASSERT_TRUE(result_a.has_value()) << result_a.error().message();

  cfg.panel = research_b_path.string();
  cfg.books_out = (tmp_dir_ / "books_b.bin").string();
  auto result_b = atx::impl::run_optimize(cfg, risk_cfg);
  ASSERT_TRUE(result_b.has_value()) << result_b.error().message();

  auto books_a_r = atx::impl::read_panel((tmp_dir_ / "books_a.bin").string());
  auto books_b_r = atx::impl::read_panel((tmp_dir_ / "books_b.bin").string());
  ASSERT_TRUE(books_a_r.has_value()) << books_a_r.error().message();
  ASSERT_TRUE(books_b_r.has_value()) << books_b_r.error().message();

  auto wfid_a = books_a_r->field_id("weight");
  auto wfid_b = books_b_r->field_id("weight");
  ASSERT_TRUE(wfid_a.has_value());
  ASSERT_TRUE(wfid_b.has_value());

  ASSERT_EQ(books_a_r->dates(), books_b_r->dates());
  const usize S = books_a_r->dates();

  usize checked = 0;
  for (usize s = 0; s < S; ++s) {
    const usize period = s * step; // matches stage_optimize.cpp's sched.periods[s]
    if (period > t_star) continue;
    ++checked;
    const auto wa = books_a_r->field_cross_section(*wfid_a, s);
    const auto wb = books_b_r->field_cross_section(*wfid_b, s);
    ASSERT_EQ(wa.size(), wb.size());
    for (usize i = 0; i < wa.size(); ++i) {
      EXPECT_EQ(wa[i], wb[i])
          << "look-ahead: book at rebalance step s=" << s << " (period=" << period
          << " <= t*=" << t_star
          << ") changed when only rows STRICTLY AFTER t* were perturbed -- i=" << i
          << " a=" << wa[i] << " b=" << wb[i];
    }
  }
  ASSERT_GT(checked, 0u) << "test construction error: no rebalance steps <= t* were checked";
}

// ---------------------------------------------------------------------------
// Warm-up fallback: a short-history Factor run must not Err, and every
// rebalance step (including the earliest, which cannot support a genuine
// Factor fit -- fit_end < 2, or an under-determined cross-section since
// style_vol's 60-row lookback is never available on a 30-date panel) must
// still produce a valid dollar-neutral, finite book via the PIT diagonal
// fallback.
// ---------------------------------------------------------------------------
TEST_F(AtxImplOptimizePit, WarmUpFallbackOnShortHistoryPanelProducesValidBooks) {
  constexpr usize M = 6, D = 30; // too short for style_vol's 60-row lookback anywhere
  constexpr usize step = 5;      // weekly

  const fs::path research_path = tmp_dir_ / "research_short.bin";
  const fs::path combo_path = tmp_dir_ / "combo_short.bin";
  ASSERT_TRUE(write_research(research_path, make_close(M, D), M, D).has_value());
  ASSERT_TRUE(make_combo(combo_path, M, D).has_value());

  risk::RiskModelConfig risk_cfg;
  risk_cfg.kind = risk::RiskModelKind::Factor;
  risk_cfg.fit_lookback_days = 60U; // deliberately larger than the whole panel
  risk_cfg.style_mom = false;
  risk_cfg.style_beta = false;
  risk_cfg.style_size = false;
  risk_cfg.industry = false; // style_vol stays true (default): needs a 60-row
                             // trailing lookback that NO date in a 30-row
                             // panel can ever supply -- every step's Factor
                             // fit fails, so every step exercises the warm-up
                             // diagonal fallback.

  atx::impl::RunConfig cfg;
  cfg.panel = research_path.string();
  cfg.combo = combo_path.string();
  cfg.books_out = (tmp_dir_ / "books_warmup.bin").string();
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_aversion = 1.0;
  cfg.set_flags.emplace("risk-aversion");

  auto result = atx::impl::run_optimize(cfg, risk_cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();

  auto books_r = atx::impl::read_panel((tmp_dir_ / "books_warmup.bin").string());
  ASSERT_TRUE(books_r.has_value()) << books_r.error().message();
  auto wfid = books_r->field_id("weight");
  ASSERT_TRUE(wfid.has_value());

  const usize expected_S = (D + step - 1) / step; // ceil(D/step)
  EXPECT_EQ(books_r->dates(), expected_S);

  for (usize s = 0; s < books_r->dates(); ++s) {
    const auto ws = books_r->field_cross_section(*wfid, s);
    ASSERT_EQ(ws.size(), M);
    double sum_w = 0.0;
    for (double w : ws) {
      ASSERT_TRUE(std::isfinite(w))
          << "s=" << s << " produced a non-finite weight -- warm-up fallback failed";
      sum_w += w;
    }
    EXPECT_LT(std::fabs(sum_w), 1e-6) << "book s=" << s << " not dollar-neutral: sum=" << sum_w;
  }
}

} // namespace atxtest_stage_optimize_pit
