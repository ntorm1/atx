// stage_metabook_riskmodel_wire_test.cpp -- p9 S2-2: the deferred RiskModelConfig-
// parameterized build_metabook_result/run_metabook overload (p8 sprint-2-progress.md's
// own documented S1/S5/final-wave seam). kind==Diagonal (default) is byte-identical;
// kind==Factor drives model_at with a per-rebalance-step PIT FactorModel, mirroring
// stage_optimize.cpp's own per-step loop.
//
// Suite: MetabookRiskModelWire

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_metabook.hpp"
#include "stage_riskmodel.hpp"

namespace atxtest_stage_metabook_riskmodel_wire {

using atx::impl::MetaBookStageConfig;
namespace alpha = atx::engine::alpha;
namespace data  = atx::engine::data;
namespace risk  = atx::engine::risk;

// D=17, weekly (step=5) -> sched.periods = [0,5,10,15]; date 16 is the ONE date past the
// last step's fit window (fit_end = 15+1 = 16) -- unused by any model, the PIT witness.
constexpr atx::usize kM = 6, kD = 17;

[[nodiscard]] std::string make_research(const std::filesystem::path &out,
                                        atx::f64 perturb_date3, atx::f64 perturb_date16) {
  std::vector<atx::f64> close(kD * kM);
  for (atx::usize t = 0; t < kD; ++t) {
    for (atx::usize i = 0; i < kM; ++i) {
      const atx::f64 drift = 0.0003 * (1.0 + static_cast<atx::f64>(i) * 0.15);
      close[t * kM + i] = 100.0 * std::exp(drift * static_cast<atx::f64>(t));
    }
  }
  if (perturb_date3 != 0.0) {
    for (atx::usize i = 0; i < kM; ++i) close[3 * kM + i] += perturb_date3;
  }
  if (perturb_date16 != 0.0) {
    for (atx::usize i = 0; i < kM; ++i) close[16 * kM + i] += perturb_date16;
  }
  std::vector<std::uint8_t> uni(kD * kM, 1U);
  auto panel = alpha::Panel::create(kD, kM, {"close"}, {close}, uni);
  EXPECT_TRUE(panel.has_value());
  auto digest = atx::impl::write_panel(*panel, out.string());
  EXPECT_TRUE(digest.has_value());
  return out.string();
}

// A common-shock correlated research panel (the SAME construction
// stage_optimize_riskmodel_test.cpp uses to make build_risk_model's Factor branch
// genuinely fit) -- used ONLY by the order-independence test (d), which calls
// build_risk_model directly and must get a real Factor artifact (has_value()), not the
// degenerate warm-up fallback the noiseless make_research above deliberately triggers.
[[nodiscard]] std::string make_correlated_research(const std::filesystem::path &out,
                                                   atx::usize M, atx::usize D) {
  std::vector<atx::f64> common_shock(D);
  for (atx::usize t = 0; t < D; ++t) {
    common_shock[t] = 0.01 * std::sin(0.31 * static_cast<atx::f64>(t));
  }
  std::vector<atx::f64> close(D * M, 100.0);
  for (atx::usize i = 0; i < M; ++i) {
    const atx::f64 idio_amp = 0.0005 * (1.0 + static_cast<atx::f64>(i % 7));
    atx::f64 level = 100.0;
    for (atx::usize t = 0; t < D; ++t) {
      const atx::f64 ret = common_shock[t] + idio_amp * std::sin(0.9 * static_cast<atx::f64>(t) +
                                                                 static_cast<atx::f64>(i));
      if (t > 0) level *= (1.0 + ret);
      close[t * M + i] = level;
    }
  }
  std::vector<std::uint8_t> uni(D * M, 1U);
  auto panel = alpha::Panel::create(D, M, {"close"}, {close}, uni);
  EXPECT_TRUE(panel.has_value());
  auto digest = atx::impl::write_panel(*panel, out.string());
  EXPECT_TRUE(digest.has_value());
  return out.string();
}

[[nodiscard]] std::string make_combo(const std::filesystem::path &out) {
  std::vector<atx::f64> a(kD * kM);
  for (atx::usize t = 0; t < kD; ++t) {
    for (atx::usize i = 0; i < kM; ++i) {
      a[t * kM + i] = static_cast<atx::f64>(i) - static_cast<atx::f64>(kM) / 2.0;
    }
  }
  std::vector<std::uint8_t> uni(kD * kM, 1U);
  auto panel = alpha::Panel::create(kD, kM, {"alpha"}, {a}, uni);
  EXPECT_TRUE(panel.has_value());
  auto digest = atx::impl::write_panel(*panel, out.string());
  EXPECT_TRUE(digest.has_value());
  return out.string();
}

[[nodiscard]] std::string tmp_dir(const std::string &tag) {
  const auto dir = std::filesystem::temp_directory_path() / "atx_s2_mb_rmw" / tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

[[nodiscard]] atx::impl::RunConfig base_cfg(const std::string &dir, atx::f64 p3, atx::f64 p16) {
  atx::impl::RunConfig cfg;
  cfg.panel = make_research(std::filesystem::path(dir) / "research.bin", p3, p16);
  cfg.combo = make_combo(std::filesystem::path(dir) / "combo.bin");
  cfg.gross = 1.0;
  cfg.name_cap = 1.0;
  cfg.rebalance = "weekly";
  cfg.risk_model = "factor";
  return cfg;
}

// ===========================================================================
//  (a) off-path byte-identity: the 2-arg overload (now cfg.risk_model-aware) at the
//  "diagonal" default must match the explicit 3-arg Diagonal call.
// ===========================================================================
TEST(MetabookRiskModelWire, DefaultTwoArgByteIdenticalToExplicitDiagonal) {
  const std::string dir = tmp_dir("offpath");
  atx::impl::RunConfig cfg = base_cfg(dir, 0.0, 0.0);
  cfg.risk_model = "diagonal";
  const MetaBookStageConfig scfg;

  auto r_default = atx::impl::build_metabook_result(cfg, scfg); // 2-arg
  ASSERT_TRUE(r_default.has_value()) << r_default.error().message();

  auto r_explicit = atx::impl::build_metabook_result(cfg, scfg, risk::RiskModelConfig{});
  ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

  ASSERT_EQ(r_default->fund_books.size(), r_explicit->fund_books.size());
  for (atx::usize s = 0; s < r_default->fund_books.size(); ++s) {
    ASSERT_EQ(r_default->fund_books[s].size(), r_explicit->fund_books[s].size());
    for (atx::usize i = 0; i < r_default->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(r_default->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(r_explicit->fund_books[s][i]))
          << "period " << s << " name " << i;
    }
  }
}

// ===========================================================================
//  (b) on-path RED->GREEN: PIT correctness of the new per-step model_at.
//  PIT-A: perturbing date 16 (strictly AFTER every step's fit window; the last step's
//  fit_end == 16) must not change ANY period's fund_books.
//  PIT-B: perturbing date 3 (inside step1's fit window [0,6), outside step0's [0,1))
//  must change step1's book but NOT step0's -- proving the model genuinely depends on
//  its OWN trailing window, not a degenerate constant.
// ===========================================================================
TEST(MetabookRiskModelWire, FutureDateDoesNotAffectAnyBook_PitA) {
  const std::string dir_base = tmp_dir("pit_a_base");
  const std::string dir_pert = tmp_dir("pit_a_pert");
  atx::impl::RunConfig cfg_base = base_cfg(dir_base, 0.0, 0.0);
  atx::impl::RunConfig cfg_pert = base_cfg(dir_pert, 0.0, /*perturb_date16=*/5.0);
  const MetaBookStageConfig scfg;

  auto r_base = atx::impl::build_metabook_result(cfg_base, scfg);
  auto r_pert = atx::impl::build_metabook_result(cfg_pert, scfg);
  ASSERT_TRUE(r_base.has_value()) << r_base.error().message();
  ASSERT_TRUE(r_pert.has_value()) << r_pert.error().message();

  ASSERT_EQ(r_base->fund_books.size(), r_pert->fund_books.size());
  for (atx::usize s = 0; s < r_base->fund_books.size(); ++s) {
    for (atx::usize i = 0; i < r_base->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(r_base->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(r_pert->fund_books[s][i]))
          << "PIT violation: period " << s << " name " << i
          << " changed from perturbing a date past every step's fit window";
    }
  }
}

TEST(MetabookRiskModelWire, EarlierWindowPerturbationChangesOnlyLaterSteps_PitB) {
  const std::string dir_base = tmp_dir("pit_b_base");
  const std::string dir_pert = tmp_dir("pit_b_pert");
  atx::impl::RunConfig cfg_base = base_cfg(dir_base, 0.0, 0.0);
  atx::impl::RunConfig cfg_pert = base_cfg(dir_pert, /*perturb_date3=*/5.0, 0.0);
  const MetaBookStageConfig scfg;

  auto r_base = atx::impl::build_metabook_result(cfg_base, scfg);
  auto r_pert = atx::impl::build_metabook_result(cfg_pert, scfg);
  ASSERT_TRUE(r_base.has_value()) << r_base.error().message();
  ASSERT_TRUE(r_pert.has_value()) << r_pert.error().message();

  // step 0 covers date 0, fit_end==1 -- date 3 is NOT in [0,1); must be unchanged.
  for (atx::usize i = 0; i < r_base->fund_books[0].size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r_base->fund_books[0][i]),
              std::bit_cast<std::uint64_t>(r_pert->fund_books[0][i]))
        << "step 0 must be PIT-blind to a date-3 perturbation";
  }
  // step 1 covers date 5, fit_end==6 -- date 3 IS in [0,6); the book must differ
  // somewhere (a genuinely live, window-dependent model, not a degenerate constant).
  bool any_diff = false;
  for (atx::usize i = 0; i < r_base->fund_books[1].size(); ++i) {
    if (r_base->fund_books[1][i] != r_pert->fund_books[1][i]) any_diff = true;
  }
  EXPECT_TRUE(any_diff) << "step 1's model must depend on date 3 -- if unchanged, "
                        << "model_at is not actually reading its trailing window";
}

// ===========================================================================
//  (c) twice-run.
// ===========================================================================
TEST(MetabookRiskModelWire, TwiceRunByteIdentical) {
  const std::string dir = tmp_dir("twice");
  atx::impl::RunConfig cfg = base_cfg(dir, 0.0, 0.0);
  const MetaBookStageConfig scfg;

  auto r1 = atx::impl::build_metabook_result(cfg, scfg);
  auto r2 = atx::impl::build_metabook_result(cfg, scfg);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  ASSERT_EQ(r1->fund_books.size(), r2->fund_books.size());
  for (atx::usize s = 0; s < r1->fund_books.size(); ++s) {
    for (atx::usize i = 0; i < r1->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->fund_books[s][i]),
                std::bit_cast<std::uint64_t>(r2->fund_books[s][i]));
    }
  }
}

// ===========================================================================
//  (d) seq==parallel: the new per-step loop's own primitive, build_risk_model, is
//  order-independent (S1's own documented contract: "fitting window s never reads window
//  s' state") -- re-verified DIRECTLY here on this file's own fixture shape (not just
//  cited), forward vs. reverse construction order.
// ===========================================================================
TEST(MetabookRiskModelWire, PerStepBuildRiskModelOrderIndependent) {
  const std::string dir = tmp_dir("order");
  // A genuine Factor fit needs real cross-sectional structure + enough dates: the noiseless
  // make_research above (small, degenerate) makes build_risk_model return Err ("M_s < K"),
  // exercising only the warm-up fallback. To PROVE the Factor primitive itself is
  // order-independent we fit it on the same correlated fixture stage_optimize_riskmodel_test
  // uses, at fit_ends deep enough for a real fit.
  constexpr atx::usize kOrderM = 10, kOrderD = 160;
  const std::string research_path =
      make_correlated_research(std::filesystem::path(dir) / "research.bin", kOrderM, kOrderD);
  auto research = atx::impl::read_panel(research_path);
  ASSERT_TRUE(research.has_value());

  const std::vector<atx::usize> fit_ends = {80, 120, 160};
  // Mirror stage_optimize_riskmodel_test's proven-fittable Factor cfg: styles other than
  // Volatility off (K=1) so the fit is well-determined without a market_cap/volume field
  // (build_exposures drops NaN-exposure instruments; a bare "close" panel cannot supply
  // size/beta exposures, which is exactly why the noiseless make_research above falls back).
  risk::RiskModelConfig factor_cfg{};
  factor_cfg.kind = risk::RiskModelKind::Factor;
  factor_cfg.fit_lookback_days = 100U;
  factor_cfg.style_size = false;
  factor_cfg.style_mom = false;
  factor_cfg.style_beta = false;
  factor_cfg.industry = false;

  std::vector<atx::u64> forward, reverse(fit_ends.size());
  for (atx::usize k = 0; k < fit_ends.size(); ++k) {
    auto a = atx::impl::build_risk_model(*research, factor_cfg, {}, nullptr, {}, 0, fit_ends[k]);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    forward.push_back(data::digest_artifact(*a));
  }
  for (atx::usize k = fit_ends.size(); k-- > 0;) {
    auto a = atx::impl::build_risk_model(*research, factor_cfg, {}, nullptr, {}, 0, fit_ends[k]);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    reverse[k] = data::digest_artifact(*a);
  }
  EXPECT_EQ(forward, reverse)
      << "each fit-window's artifact must be independent of construction order";
}

} // namespace atxtest_stage_metabook_riskmodel_wire
