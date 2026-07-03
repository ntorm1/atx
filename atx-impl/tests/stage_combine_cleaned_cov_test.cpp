// stage_combine_cleaned_cov_test.cpp — p8 S3-4: ShrinkageMv consumes
// atx::engine::data::cleaned_alpha_cov behind risk::RiskModelConfig.kind ==
// Factor (default Diagonal, inert).
//
// Two call surfaces are exercised:
//   * The FULL stage, via run_combine(cfg, combiner_cfg, risk_cfg) — a real
//     DSL/panel fixture (mirrors stage_combine_stack_test.cpp's technique) —
//     proves kind==Diagonal is byte-identical to the pre-S3-4 legacy path
//     (determinism class (a), off-path byte-identity).
//   * fit_shrinkage_mv_cleaned_cov directly, against a HAND-BUILT pool whose
//     pnl streams reproduce risk_cleaned_alpha_cov_test.cpp's own N~T
//     "spurious large eigenvalue" fixture (a common sinusoid + small
//     idiosyncratic noise + one large near-independent outlier column) — the
//     exact regime that test's ShrinksSpuriousEigenvalueAndDiversifies proves
//     cleaned_alpha_cov diversifies against the RAW sample covariance. Here
//     the comparison is instead against combine::AlphaCombiner's OWN internal
//     Ledoit-Wolf-shrunk-to-identity ShrinkageMv fit (kind==Diagonal), the
//     actual alternative a caller would get by NOT opting in — proving the
//     wiring is live (weights change) and reporting the measured
//     diversification direction.
//
// Suite: StageCombineCleanedCov

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_combine.hpp"
#include "stages.hpp"

namespace atxtest_stage_combine_cleaned_cov {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
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
    const std::string path = (fs::temp_directory_path() / ("atx_impl_scc_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

static std::string write_alpha_dir(const std::string& stem, const std::vector<std::string>& dsls) {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / ("atx_impl_scc_alphas_" + stem)).string();
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

// ===========================================================================
//  DiagonalKindByteIdenticalToLegacyPath — determinism class (a): the
//  inert-default off-path byte-identity proof. kind==Diagonal (the enum's
//  own default value, and the value every 0/1/2-arg run_combine caller
//  reaches via its default-constructed RiskModelConfig{} forward) must
//  produce a combo.bin digest AND weights-sidecar text BYTE-IDENTICAL to the
//  pre-S3-4 legacy zero-arg entry point on the SAME panel/DSL/method.
// ===========================================================================
TEST(StageCombineCleanedCov, DiagonalKindByteIdenticalToLegacyPath) {
    namespace fs = std::filesystem;
    auto panel_opt = make_panel(80, 6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "diag");
    const std::string alphas_dir = write_alpha_dir("diag", safe_dsls());
    const std::string combo_legacy = (fs::temp_directory_path() / "atx_impl_scc_legacy.bin").string();
    const std::string combo_explicit = (fs::temp_directory_path() / "atx_impl_scc_explicit.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.method     = "shrinkage-mv";

    cfg.combo_out = combo_legacy;
    auto r_legacy = atx::impl::run_combine(cfg); // 0-arg: pre-S3-4 entry point, untouched
    ASSERT_TRUE(r_legacy.has_value()) << r_legacy.error().message();

    cfg.combo_out = combo_explicit;
    combine::CombinerConfig ccfg{};
    ccfg.method = combine::CombineMethod::ShrinkageMv;
    auto r_explicit = atx::impl::run_combine(cfg, ccfg, risk::RiskModelConfig{}); // explicit kind==Diagonal
    ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

    EXPECT_EQ(r_legacy->digest, r_explicit->digest)
        << "kind==Diagonal (explicit or defaulted) must be byte-identical to the legacy path";
    EXPECT_NE(r_legacy->digest, atx::u64{0});

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo_legacy + ".weights.txt"), read_bytes(combo_explicit + ".weights.txt"));

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo_legacy, combo_explicit}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ---------------------------------------------------------------------------
// make_noisy_pool — reproduces risk_cleaned_alpha_cov_test.cpp's own N~T
// "spurious large eigenvalue" fixture (a common sinusoid + small idiosyncratic
// noise + one large near-independent outlier column), but as a hand-built
// combine::AlphaStore pool (pnl streams = the fixture's T x N return columns;
// positions are a 1-instrument stub — the ShrinkageMv weight fit reads ONLY
// pnl via combine::detail::window_means/complete_case_centered, never
// positions). Values are already column-demeaned (matching the S1 test's own
// "centered" contract), which window_means/complete_case_centered tolerate
// identically (re-demeaning an already ~0-mean column is a no-op).
// ---------------------------------------------------------------------------
static combine::AlphaStore make_noisy_pool(usize t, usize n) {
    combine::AlphaStore pool;
    MatX r(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(n));
    for (usize row = 0; row < t; ++row) {
        const f64 common = 0.01 * std::sin(0.2 * static_cast<f64>(row));
        for (usize col = 0; col < n; ++col) {
            f64 idio = 0.004 * std::sin(0.7 * static_cast<f64>(row) + static_cast<f64>(col));
            if (col == n - 1) {
                idio += 0.05 * std::sin(1.3 * static_cast<f64>(row) + 0.5);
            }
            r(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(col)) = common + idio;
        }
    }
    VecX col_mean = r.colwise().mean();
    for (Eigen::Index c = 0; c < r.cols(); ++c) {
        r.col(c).array() -= col_mean[c];
    }
    for (usize col = 0; col < n; ++col) {
        std::vector<f64> pnl(t);
        for (usize row = 0; row < t; ++row) {
            pnl[row] = r(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(col));
        }
        const std::vector<f64> pos(t, 0.0); // 1 instrument, unused by the fit
        const auto ins = pool.insert(nullptr, pnl, pos, combine::AlphaMetrics{});
        EXPECT_TRUE(ins.has_value());
    }
    return pool;
}

// ===========================================================================
//  FactorKindWiringIsLiveAndReportsMeasuredDiversification — determinism
//  class (b), on-path RED->GREEN: proves fit_shrinkage_mv_cleaned_cov's
//  weights genuinely differ from combine::AlphaCombiner{}.fit's (kind==
//  Diagonal) ShrinkageMv weights on the SAME N~T spurious-eigenvalue pool/
//  window (a no-op wire would leave them identical) -- and reports the
//  measured max|w| concentration in both directions (see the sprint-3 ledger
//  for the numbers actually observed on this fixture).
// ===========================================================================
TEST(StageCombineCleanedCov, FactorKindWiringIsLiveAndReportsMeasuredDiversification) {
    constexpr usize T = 20, N = 18; // N approx T -- the noise-dominated regime
    const combine::AlphaStore pool = make_noisy_pool(T, N);

    combine::AlphaCombiner plain; // default cfg: method == ShrinkageMv (kind==Diagonal's actual path)
    const auto r_diag = plain.fit(pool, 0U, T);
    ASSERT_TRUE(r_diag.has_value()) << r_diag.error().message();

    const auto r_factor = atx::impl::fit_shrinkage_mv_cleaned_cov(pool, 0U, T);
    ASSERT_TRUE(r_factor.has_value()) << r_factor.error().message();

    ASSERT_EQ(r_diag->weights.size(), r_factor->weights.size());
    bool any_diff = false;
    f64 max_diag = 0.0, max_factor = 0.0, gross_factor = 0.0;
    for (usize a = 0; a < r_diag->weights.size(); ++a) {
        if (r_diag->weights[a] != r_factor->weights[a]) any_diff = true;
        max_diag = std::max(max_diag, std::fabs(r_diag->weights[a]));
        max_factor = std::max(max_factor, std::fabs(r_factor->weights[a]));
        gross_factor += std::fabs(r_factor->weights[a]);
        EXPECT_TRUE(std::isfinite(r_factor->weights[a]));
    }
    EXPECT_TRUE(any_diff) << "kind==Factor must genuinely swap the covariance -- weights "
                          << "identical to the Diagonal path would mean the wire is dead";
    EXPECT_NEAR(gross_factor, 1.0, 1e-9);

    std::cout << "[StageCombineCleanedCov] max|w|_diagonal=" << max_diag
              << " max|w|_factor=" << max_factor << "\n";
    EXPECT_LT(max_factor, max_diag)
        << "expected the cleaned covariance's ShrinkageMv weights to be MORE diversified "
        << "(lower max|w|) than the plain LW-shrunk-to-identity path on this N~T spurious-"
        << "eigenvalue fixture: max_factor=" << max_factor << " max_diag=" << max_diag;
}

// ===========================================================================
//  TwiceRunByteIdentical — determinism class (c).
// ===========================================================================
TEST(StageCombineCleanedCov, TwiceRunByteIdentical) {
    constexpr usize T = 20, N = 18;
    const combine::AlphaStore pool = make_noisy_pool(T, N);

    const auto r1 = atx::impl::fit_shrinkage_mv_cleaned_cov(pool, 0U, T);
    const auto r2 = atx::impl::fit_shrinkage_mv_cleaned_cov(pool, 0U, T);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    ASSERT_EQ(r1->weights.size(), r2->weights.size());
    for (usize a = 0; a < r1->weights.size(); ++a) {
        EXPECT_EQ(r1->weights[a], r2->weights[a]) << "alpha " << a;
    }
    EXPECT_EQ(r1->fit_begin, r2->fit_begin);
    EXPECT_EQ(r1->fit_end, r2->fit_end);
}

} // namespace atxtest_stage_combine_cleaned_cov
