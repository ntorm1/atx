// stage_combine_riskmodel_wire_test.cpp -- p9 S2-1: run_combine's 0-/1-arg entry points
// build risk::RiskModelConfig FROM cfg.risk_model instead of hardcoding kind==Diagonal
// (stage_combine.cpp:765,771, pre-S2). Proves the CLI-shaped RunConfig surface actually
// reaches the S3-4 Factor covariance path (fit_shrinkage_mv_cleaned_cov), not merely the
// already-tested explicit 3-arg call (stage_combine_cleaned_cov_test.cpp).
//
// Suite: StageCombineRiskModelWire

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_combine.hpp"
#include "stages.hpp"

namespace atxtest_stage_combine_riskmodel_wire {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;
namespace combine = atx::engine::combine;
namespace risk = atx::engine::risk;

struct Lcg {
    std::uint64_t s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t hi = s >> 11U;
        return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0;
    }
};

// Mirrors stage_combine_cleaned_cov_test.cpp's own noisy_close/make_panel exactly (same
// seed, same shape) -- the p8-S3-4-proven panel where ShrinkageMv's Factor-kind weight fit
// is known to diverge from the Diagonal-kind fit.
static std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
    std::vector<f64> drift(insts);
    for (usize j = 0; j < insts; ++j) {
        drift[j] = 0.006 - 0.0024 * static_cast<f64>(j % 4U);
    }
    std::vector<f64> close(dates * insts);
    std::vector<f64> px(insts, 100.0);
    Lcg rng{seed};
    for (usize t = 0; t < dates; ++t) {
        for (usize j = 0; j < insts; ++j) {
            px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
            close[t * insts + j] = px[j];
        }
    }
    return close;
}

static std::optional<Panel> make_panel(usize dates, usize insts) {
    const std::vector<f64> close = noisy_close(dates, insts, 0x0FEEDBABEULL);
    auto r = Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path = (fs::temp_directory_path() / ("atx_impl_scrmw_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

static std::string write_alpha_dir(const std::string& stem, const std::vector<std::string>& dsls) {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / ("atx_impl_scrmw_alphas_" + stem)).string();
    fs::create_directories(dir);
    for (usize i = 0; i < dsls.size(); ++i) {
        std::ofstream f{(fs::path{dir} / ("alpha_" + std::to_string(i) + ".dsl")).string()};
        EXPECT_TRUE(f.is_open());
        f << dsls[i] << '\n';
    }
    return dir;
}

static std::vector<std::string> safe_dsls() {
    return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
}

struct Fixture { std::string panel_path; std::string alphas_dir; };

static Fixture make_fixture(const std::string& tag) {
    Fixture fx;
    auto panel_opt = make_panel(80, 6);
    EXPECT_TRUE(panel_opt.has_value());
    fx.panel_path = write_panel_tmp(*panel_opt, tag);
    fx.alphas_dir = write_alpha_dir(tag, safe_dsls());
    return fx;
}

// ===========================================================================
//  (a) off-path byte-identity: cfg.risk_model defaulted ("diagonal") through the
//  NOW-FIXED 0-arg run_combine(cfg) must still match the pre-p9 legacy digest --
//  proven against the EXPLICIT kind==Diagonal 3-arg call.
// ===========================================================================
TEST(StageCombineRiskModelWire, DefaultRiskModelByteIdenticalToExplicitDiagonal) {
    namespace fs = std::filesystem;
    const Fixture fx = make_fixture("offpath");

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = fx.panel_path;
    cfg.alphas     = fx.alphas_dir;
    cfg.method     = "shrinkage-mv";
    ASSERT_EQ(cfg.risk_model, "diagonal");

    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_offpath_0arg.bin").string();
    auto r0 = atx::impl::run_combine(cfg); // 0-arg, S2-fixed
    ASSERT_TRUE(r0.has_value()) << r0.error().message();

    combine::CombinerConfig ccfg{};
    ccfg.method = combine::CombineMethod::ShrinkageMv;
    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_offpath_explicit.bin").string();
    auto r_explicit = atx::impl::run_combine(cfg, ccfg, risk::RiskModelConfig{});
    ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

    EXPECT_EQ(r0->digest, r_explicit->digest)
        << "cfg.risk_model=='diagonal' (the default) must route the 0-arg wrapper to the "
        << "SAME Diagonal path an explicit RiskModelConfig{} reaches";
}

// ===========================================================================
//  (b) on-path RED->GREEN: cfg.risk_model=="factor" through the 0-arg run_combine(cfg)
//  reaches EXACTLY the S3-4 Factor covariance path (digest-equal to the explicit 3-arg
//  call) and is LIVE (digest differs from the Diagonal path).
// ===========================================================================
TEST(StageCombineRiskModelWire, RiskModelFactorReachesS3_4CleanedCovPath) {
    namespace fs = std::filesystem;
    const Fixture fx = make_fixture("onpath");

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = fx.panel_path;
    cfg.alphas     = fx.alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.risk_model = "factor";

    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_onpath_0arg.bin").string();
    auto r_wire = atx::impl::run_combine(cfg); // 0-arg, S2-fixed
    ASSERT_TRUE(r_wire.has_value()) << r_wire.error().message();

    combine::CombinerConfig ccfg{};
    ccfg.method = combine::CombineMethod::ShrinkageMv;
    risk::RiskModelConfig factor_cfg{};
    factor_cfg.kind = risk::RiskModelKind::Factor;
    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_onpath_explicit.bin").string();
    auto r_explicit = atx::impl::run_combine(cfg, ccfg, factor_cfg);
    ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

    EXPECT_EQ(r_wire->digest, r_explicit->digest)
        << "cfg.risk_model=='factor' through the 0-arg wrapper must reach the EXACT SAME "
        << "S3-4 Factor path an explicit RiskModelConfig{kind=Factor} call reaches";

    risk::RiskModelConfig diag_cfg{}; // kind==Diagonal
    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_onpath_diag.bin").string();
    auto r_diag = atx::impl::run_combine(cfg, ccfg, diag_cfg);
    ASSERT_TRUE(r_diag.has_value()) << r_diag.error().message();

    EXPECT_NE(r_wire->digest, r_diag->digest)
        << "the wire must be LIVE: factor and diagonal digests must differ, or "
        << "cfg.risk_model is a silent no-op (the exact p9 anti-roadmap violation)";

    // The measured diversification itself (max|w| dropping under the cleaned covariance)
    // is S3-4's own proven claim (stage_combine_cleaned_cov_test.cpp,
    // FactorKindWiringIsLiveAndReportsMeasuredDiversification: max|w|_diagonal=0.164248 ->
    // max|w|_factor=0.142554 on its N~T=18/20 pool fixture), unmodified by S2. The
    // digest-equivalence assertion above is S2's own proof: that same win is now reachable
    // through cfg.risk_model, not just a direct 3-arg call.
}

// ===========================================================================
//  (c) twice-run.
// ===========================================================================
TEST(StageCombineRiskModelWire, TwiceRunByteIdentical) {
    namespace fs = std::filesystem;
    const Fixture fx = make_fixture("twice");

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = fx.panel_path;
    cfg.alphas     = fx.alphas_dir;
    cfg.method     = "shrinkage-mv";
    cfg.risk_model = "factor";

    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_twice_a.bin").string();
    auto r1 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_twice_b.bin").string();
    auto r2 = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    EXPECT_EQ(r1->digest, r2->digest);
}

// (d) seq==parallel: documented N/A, not vacuously tested. S2-1 is a pure 3-line routing
// fix in the 0-/1-arg wrapper bodies -- no new loop, no new threading, no new shared
// mutable state. The Factor computation itself (fit_shrinkage_mv_cleaned_cov /
// cleaned_alpha_cov) is S3-4's unmodified code, already covered by
// CombineDeterminismBattery's parallel-safety story. Re-confirmed by the S2-3 regression
// sweep, not re-implemented here.

} // namespace atxtest_stage_combine_riskmodel_wire
