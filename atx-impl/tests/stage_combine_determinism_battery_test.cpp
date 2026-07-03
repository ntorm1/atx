// stage_combine_determinism_battery_test.cpp — p8 S3-5: the consolidated
// determinism gate for the whole sprint (no new engine code — tests only,
// per the sprint spec's S3-5 unit).
//
// Covers the THREE determinism-battery classes not already discharged by an
// earlier S3 unit's own RED->GREEN test:
//
//   (a) combine_default_byte_identical — each of the five legacy
//       CombineMethods (+ the default "" string) is untouched by the S3-0
//       enum append and the S3-4 new dispatch branch: the 0-arg legacy
//       run_combine(cfg) entry point and the explicit 3-arg overload (with a
//       default-constructed CombinerConfig/RiskModelConfig — kind==Diagonal)
//       produce BYTE-IDENTICAL combo.bin digests and weights-sidecar bytes.
//
//   (d) stack_seq_eq_parallel / regime_seq_eq_parallel — N independent
//       fit_stack_combo calls (four different hand-built pools, the same
//       "each fold/partition is independent" premise the sprint doc
//       describes) dispatched across atx::engine::parallel::DetPool produce,
//       per index, BYTE-IDENTICAL Combination weights + verdict_hash to the
//       same N calls run in a plain sequential loop. fit_stack_combo carries
//       no shared mutable state across calls (every seed derives from
//       combiner_cfg.stack_master_seed, a per-call const-ref input), so this
//       proves concurrent dispatch cannot cross-contaminate results.
//
// The remaining two S3-5 accept bullets are NOT re-implemented here, only
// re-confirmed via the regression sweep this unit's ledger entry records:
//   * stack_twice_run / regime_stack_twice_run — already proven in S3-1
//     (StageCombineStack.TwiceRunByteIdenticalComboAndVerdictHash) and S3-3
//     (StageCombineRegime.TwiceRunByteIdentical).
//   * combine_method_enum_layout_pin — already proven in S3-0
//     (CombineMethodEnumLayoutPin, combine_method_enum_layout_pin_test.cpp).
//
// Suite: CombineDeterminismBattery

#include <cmath>
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
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_combine.hpp"
#include "stages.hpp"

namespace atxtest_stage_combine_determinism_battery {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::alpha::Panel;
namespace combine = atx::engine::combine;
namespace risk = atx::engine::risk;
namespace parallel = atx::engine::parallel;

struct Lcg {
    u64 s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const u64 hi = s >> 11U;
        return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0;
    }
};

// ===========================================================================
//  (a) combine_default_byte_identical
// ===========================================================================
static std::vector<f64> noisy_close(usize dates, usize insts, u64 seed) {
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
    const std::vector<f64> close = noisy_close(dates, insts, 0x51CE0FF1ULL);
    auto r = Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path = (fs::temp_directory_path() / ("atx_impl_scdb_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

static std::string write_alpha_dir(const std::string& stem, const std::vector<std::string>& dsls) {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / ("atx_impl_scdb_alphas_" + stem)).string();
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

static combine::CombineMethod method_of(const std::string& s) {
    if (s.empty() || s == "shrinkage-mv") return combine::CombineMethod::ShrinkageMv;
    if (s == "equal") return combine::CombineMethod::EqualWeight;
    if (s == "rank") return combine::CombineMethod::RankAverage;
    if (s == "ic") return combine::CombineMethod::IcWeighted;
    if (s == "bounded") return combine::CombineMethod::BoundedRegression;
    ADD_FAILURE() << "unrecognized legacy method string: " << s;
    return combine::CombineMethod::ShrinkageMv;
}

static std::vector<char> read_bytes(const std::string& path) {
    std::ifstream f{path, std::ios::binary};
    return std::vector<char>{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

TEST(CombineDeterminismBattery, DefaultByteIdenticalAcrossAllFiveLegacyMethodsAndDefaultString) {
    namespace fs = std::filesystem;
    auto panel_opt = make_panel(80, 6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "default");
    const std::string alphas_dir = write_alpha_dir("default", safe_dsls());

    // The five --method strings the CLI accepts today PLUS the empty-string
    // default (method_from_string's own "" -> ShrinkageMv arm).
    const std::vector<std::string> methods = {"", "shrinkage-mv", "equal", "rank", "ic", "bounded"};

    for (const std::string& m : methods) {
        SCOPED_TRACE("method='" + m + "'");
        const std::string combo_legacy =
            (fs::temp_directory_path() / ("atx_impl_scdb_legacy_" + (m.empty() ? "default" : m) + ".bin")).string();
        const std::string combo_explicit =
            (fs::temp_directory_path() / ("atx_impl_scdb_explicit_" + (m.empty() ? "default" : m) + ".bin")).string();

        atx::impl::RunConfig cfg;
        cfg.subcommand = "combine";
        cfg.panel      = panel_path;
        cfg.alphas     = alphas_dir;
        cfg.method     = m;

        cfg.combo_out = combo_legacy;
        auto r_legacy = atx::impl::run_combine(cfg); // 0-arg: the pre-S3 legacy entry point, untouched
        ASSERT_TRUE(r_legacy.has_value()) << r_legacy.error().message();

        cfg.combo_out = combo_explicit;
        combine::CombinerConfig ccfg{};
        ccfg.method = method_of(m);
        auto r_explicit = atx::impl::run_combine(cfg, ccfg, risk::RiskModelConfig{}); // explicit defaults
        ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

        EXPECT_EQ(r_legacy->digest, r_explicit->digest)
            << "method '" << m << "' must be byte-identical across the legacy and explicit-default "
            << "3-arg call surfaces (the S3-0 enum append / S3-4 new dispatch branch must not "
            << "perturb any legacy method)";
        EXPECT_EQ(read_bytes(combo_legacy + ".weights.txt"), read_bytes(combo_explicit + ".weights.txt"));

        std::error_code ec;
        for (const std::string& co : {combo_legacy, combo_explicit}) {
            fs::remove(co, ec);
            fs::remove(co + ".weights.txt", ec);
            fs::remove(co + ".meta", ec);
        }
    }

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
}

// ===========================================================================
//  (d) stack_seq_eq_parallel / regime_seq_eq_parallel
// ===========================================================================

// Mirrors stage_combine_stack_gate_test.cpp's own fixture technique (hand-set
// position columns + a close series whose 1-period forward return recovers
// the label exactly) so fit_stack_combo's full wiring is exercised, not a
// hand-built FeatureMatrix. Positions are LINEARLY combinable (no interaction
// term) -- the admit/reject outcome does not matter for this determinism
// proof (either branch is a deterministic pure function of its inputs); a
// linear fixture just keeps every one of the kN pools cheap and reliable.
template <typename ColFn, typename LabelFn>
static void build_fixture(usize n_dates, usize n_inst, usize n_features, u64 seed, ColFn col_fn,
                          LabelFn label_fn, combine::AlphaStore& pool_out, std::vector<f64>& close_out) {
    Lcg rng{seed};
    std::vector<std::vector<f64>> cols_per_alpha(n_features, std::vector<f64>(n_dates * n_inst, 0.0));
    close_out.assign(n_dates * n_inst, 100.0);
    for (usize d = 0; d < n_dates; ++d) {
        for (usize i = 0; i < n_inst; ++i) {
            std::vector<f64> cols(n_features, 0.0);
            col_fn(d, i, cols, rng);
            for (usize f = 0; f < n_features; ++f) cols_per_alpha[f][d * n_inst + i] = cols[f];
            const f64 noise = rng.next();
            if (d + 1U < n_dates) {
                const f64 y = label_fn(cols, noise);
                close_out[(d + 1U) * n_inst + i] = close_out[d * n_inst + i] * (1.0 + y);
            }
        }
    }
    for (usize f = 0; f < n_features; ++f) {
        std::vector<f64> pnl(n_dates, 0.0);
        for (usize d = 0; d < n_dates; ++d) pnl[d] = 0.001 * cols_per_alpha[f][d * n_inst];
        const auto r = pool_out.insert(nullptr, pnl, cols_per_alpha[f], combine::AlphaMetrics{});
        ASSERT_TRUE(r.has_value());
    }
}

static combine::AlphaStore linearly_combinable_pool(u64 seed, std::vector<f64>& close_out) {
    combine::AlphaStore pool;
    const auto cols = [](usize, usize, std::vector<f64>& c, Lcg& rng) {
        for (f64& v : c) v = rng.next();
    };
    const auto label = [](const std::vector<f64>& c, f64 noise) -> f64 {
        return 0.6 * c[0] + 0.4 * c[1] + 0.05 * noise;
    };
    build_fixture(48U, 14U, 4U, seed, cols, label, pool, close_out);
    return pool;
}

static combine::CombinerConfig seq_par_cfg(u64 seed, atx::u32 regime_n_states) {
    combine::CombinerConfig c{};
    c.method = regime_n_states > 1U ? combine::CombineMethod::RegimeStack : combine::CombineMethod::Stack;
    c.stack_master_seed = seed;
    c.stack_cpcv_groups = 4;
    c.stack_cpcv_test_groups = 1;
    c.stack_cpcv_embargo = 0.0;
    c.stack_horizon = 1;
    c.regime_n_states = regime_n_states;
    return c;
}

// Runs kN independent fit_stack_combo calls (kN independently-seeded pools --
// the "each fold/partition is independent" premise) once sequentially and
// once dispatched across a DetPool, and asserts the per-index results match
// bit-for-bit between the two execution substrates.
static void run_seq_eq_parallel(bool with_regime) {
    constexpr usize kN = 4;
    std::vector<std::vector<f64>> closes(kN);
    std::vector<combine::AlphaStore> pools;
    pools.reserve(kN);
    for (usize k = 0; k < kN; ++k) {
        pools.push_back(linearly_combinable_pool(/*seed=*/1000ULL + static_cast<u64>(k), closes[k]));
    }
    const combine::CombinerConfig cfg = seq_par_cfg(/*seed=*/2026ULL, with_regime ? 2U : 1U);

    using Res = atx::core::Result<atx::impl::StackFitResult>;
    std::vector<Res> seq(kN, atx::core::Err(atx::core::ErrorCode::Internal, "unset"));
    for (usize k = 0; k < kN; ++k) {
        seq[k] = atx::impl::fit_stack_combo(pools[k], std::span<const f64>{closes[k]}, pools[k].n_instruments(),
                                            0U, pools[k].n_periods(), cfg, with_regime);
    }

    std::vector<Res> par(kN, atx::core::Err(atx::core::ErrorCode::Internal, "unset"));
    parallel::DetPool det_pool(4);
    det_pool.parallel_for(kN, [&](atx::usize i, atx::usize) {
        par[i] = atx::impl::fit_stack_combo(pools[i], std::span<const f64>{closes[i]}, pools[i].n_instruments(),
                                            0U, pools[i].n_periods(), cfg, with_regime);
    });

    for (usize k = 0; k < kN; ++k) {
        ASSERT_TRUE(seq[k].has_value()) << "sequential index " << k << ": " << seq[k].error().message();
        ASSERT_TRUE(par[k].has_value()) << "parallel index " << k << ": " << par[k].error().message();
        EXPECT_EQ(seq[k]->verdict.verdict_hash, par[k]->verdict.verdict_hash) << "index " << k;
        EXPECT_EQ(seq[k]->verdict.admitted, par[k]->verdict.admitted) << "index " << k;
        ASSERT_EQ(seq[k]->combo.weights.size(), par[k]->combo.weights.size()) << "index " << k;
        for (usize a = 0; a < seq[k]->combo.weights.size(); ++a) {
            EXPECT_EQ(seq[k]->combo.weights[a], par[k]->combo.weights[a]) << "index " << k << " alpha " << a;
        }
    }
}

TEST(CombineDeterminismBattery, StackSeqEqParallel) {
    run_seq_eq_parallel(/*with_regime=*/false);
}

TEST(CombineDeterminismBattery, RegimeStackSeqEqParallel) {
    run_seq_eq_parallel(/*with_regime=*/true);
}

} // namespace atxtest_stage_combine_determinism_battery
