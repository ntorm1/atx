// stage_combine_stack_test.cpp — p8 S3-1: `CombineMethod::Stack` wired end-to-
// end in the atx-impl combine stage (direct-call integration tests — CLI flag
// threading is Sprint 5's job; the run_combine(cfg, combiner_cfg[, risk_cfg])
// overloads, declared in stage_combine.hpp, are the S3 direct-call seam).
//
// Suite: StageCombineStack

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

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stage_combine.hpp"
#include "stages.hpp"

namespace atxtest_stage_combine_stack {

using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;
namespace combine = atx::engine::combine;

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
    const std::vector<f64> close = noisy_close(dates, insts, 0xABCDEF01ULL);
    auto r = Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    namespace fs = std::filesystem;
    const std::string path = (fs::temp_directory_path() / ("atx_impl_scs_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

static std::string write_alpha_dir(const std::string& stem, const std::vector<std::string>& dsls) {
    namespace fs = std::filesystem;
    const std::string dir = (fs::temp_directory_path() / ("atx_impl_scs_alphas_" + stem)).string();
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

static std::string find_kv(const atx::impl::StageResult& sr, const std::string& k) {
    for (const auto& p : sr.kvs) {
        if (p.first == k) return p.second;
    }
    return "";
}

// A small CPCV config so the fixture's fit window (down to ~60 periods once
// PIT-guard tests shrink fit_end) still has usable folds.
static combine::CombinerConfig stack_cfg(atx::u64 seed = 4242ULL) {
    combine::CombinerConfig c{};
    c.method = combine::CombineMethod::Stack;
    c.stack_master_seed = seed;
    c.stack_cpcv_groups = 4;
    c.stack_cpcv_test_groups = 1;
    c.stack_cpcv_embargo = 0.0;
    c.stack_horizon = 1;
    return c;
}

// ===========================================================================
//  ProducesWellFormedWeightsAndStableVerdictHash — S3-1 accept criterion.
// ===========================================================================
TEST(StageCombineStack, ProducesWellFormedWeightsAndStableVerdictHash) {
    namespace fs = std::filesystem;
    auto panel_opt = make_panel(96, 6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "produces");
    const std::string alphas_dir = write_alpha_dir("produces", safe_dsls());
    const std::string combo_out = (fs::temp_directory_path() / "atx_impl_scs_produces.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;
    cfg.combo_out  = combo_out;

    auto r = atx::impl::run_combine(cfg, stack_cfg());
    ASSERT_TRUE(r.has_value()) << r.error().message();

    EXPECT_EQ(find_kv(*r, "method"), "stack");
    const std::string vh = find_kv(*r, "stack_verdict_hash");
    ASSERT_FALSE(vh.empty()) << "stack_verdict_hash telemetry must be emitted";
    EXPECT_NE(vh, "0") << "verdict_hash must be a real hash, not a default-constructed zero";

    // Read the weights sidecar: 3 weights, all finite, Sum|w| ~= 1.
    std::ifstream wf{combo_out + ".weights.txt"};
    ASSERT_TRUE(wf.is_open());
    std::string line;
    f64 gross = 0.0;
    int count = 0;
    while (std::getline(wf, line)) {
        if (line.rfind("w[", 0) == 0) {
            const auto eq = line.find('=');
            const auto sp = line.find(' ', eq);
            const f64 w = std::stod(line.substr(eq + 1, sp - eq - 1));
            EXPECT_TRUE(std::isfinite(w)) << "weight must be finite: " << line;
            gross += std::abs(w);
            ++count;
        }
    }
    EXPECT_EQ(count, 3) << "one weight per pool alpha";
    // Tolerance loosened from the theoretical bit-exact 1.0: the sidecar
    // round-trips weights through operator<<'s default (6 significant digit)
    // text precision, so re-parsing via std::stod loses precision the
    // in-memory renorm_abs_sum invariant does not carry.
    EXPECT_NEAR(gross, 1.0, 1e-4) << "Sum|w| must be normalized to 1 (renorm_abs_sum)";

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    fs::remove(combo_out, ec);
    fs::remove(combo_out + ".weights.txt", ec);
    fs::remove(combo_out + ".meta", ec);
}

// ===========================================================================
//  TwiceRunByteIdenticalComboAndVerdictHash — determinism class (c).
// ===========================================================================
TEST(StageCombineStack, TwiceRunByteIdenticalComboAndVerdictHash) {
    namespace fs = std::filesystem;
    auto panel_opt = make_panel(96, 6);
    ASSERT_TRUE(panel_opt.has_value());
    const Panel& panel = *panel_opt;
    const std::string panel_path = write_panel_tmp(panel, "twice");
    const std::string alphas_dir = write_alpha_dir("twice", safe_dsls());
    const std::string combo1 = (fs::temp_directory_path() / "atx_impl_scs_twice1.bin").string();
    const std::string combo2 = (fs::temp_directory_path() / "atx_impl_scs_twice2.bin").string();

    atx::impl::RunConfig cfg;
    cfg.subcommand = "combine";
    cfg.panel      = panel_path;
    cfg.alphas     = alphas_dir;

    cfg.combo_out = combo1;
    auto r1 = atx::impl::run_combine(cfg, stack_cfg());
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    cfg.combo_out = combo2;
    auto r2 = atx::impl::run_combine(cfg, stack_cfg());
    ASSERT_TRUE(r2.has_value()) << r2.error().message();

    EXPECT_EQ(r1->digest, r2->digest) << "same panel -> same combo bytes";
    EXPECT_NE(r1->digest, atx::u64{0});
    EXPECT_EQ(find_kv(*r1, "stack_verdict_hash"), find_kv(*r2, "stack_verdict_hash"))
        << "same seed -> same verdict_hash (M1)";

    auto read_bytes = [](const std::string& path) -> std::vector<char> {
        std::ifstream f{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    };
    EXPECT_EQ(read_bytes(combo1), read_bytes(combo2));

    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alphas_dir, ec);
    for (const std::string& co : {combo1, combo2}) {
        fs::remove(co, ec);
        fs::remove(co + ".weights.txt", ec);
        fs::remove(co + ".meta", ec);
    }
}

// ===========================================================================
//  ForwardReturnLabelIsPitCausal — the S3-1 PIT guard.
//
// With an EXPLICIT fit_end well inside the panel, perturbing panel rows >=
// fit_end must leave the FITTED weights (and the verdict) BYTE-IDENTICAL:
// windowed_pool never reads a pool row outside [fit_begin,fit_end), and
// build_forward_returns_window never reads a close cell at date >= fit_end.
//
// NOTE: the STAGE's full combo.bin digest is NOT the right invariant here —
// step 9 applies the fitted weights to EVERY panel date (including the OOS
// region past fit_end, by design, so report-stage scoring can read it), and
// the alpha DSL streams themselves legitimately differ at t>=fit_end once
// those prices are perturbed (a momentum/rank alpha's OWN causal window
// changes there) — that is expected, not a leak. The PIT claim under test is
// narrower: the FIT (windowed_pool + build_forward_returns_window + fit_stack
// + the projection) must not have been influenced by the perturbation, so it
// is checked directly against the weights sidecar's "w[...]=" lines (the
// fitted output) and the verdict_hash kv, not the whole-panel digest.
// ===========================================================================
TEST(StageCombineStack, ForwardReturnLabelIsPitCausal) {
    namespace fs = std::filesystem;
    constexpr usize kDates = 96;
    constexpr usize kInsts = 6;
    constexpr usize kFitEnd = 70; // holds out the last 26 periods

    const std::vector<f64> base_close = noisy_close(kDates, kInsts, 0x13572468ULL);

    struct Fitted {
        atx::impl::StageResult sr;
        std::string weight_lines; // just the "w[a]=value" lines (fit output only)
    };

    auto run_with_close = [&](std::vector<f64> close, const std::string& tag) -> Fitted {
        auto pr = Panel::create(kDates, kInsts, {"close"}, {close}, {});
        EXPECT_TRUE(pr.has_value());
        const std::string panel_path = write_panel_tmp(*pr, tag);
        const std::string alphas_dir = write_alpha_dir(tag, safe_dsls());
        const std::string combo_out = (fs::temp_directory_path() / ("atx_impl_scs_" + tag + ".bin")).string();

        atx::impl::RunConfig cfg;
        cfg.subcommand = "combine";
        cfg.panel      = panel_path;
        cfg.alphas     = alphas_dir;
        cfg.combo_out  = combo_out;
        cfg.fit_end    = static_cast<int>(kFitEnd);
        cfg.set_flags.insert("fit-end");

        auto r = atx::impl::run_combine(cfg, stack_cfg());
        EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());

        std::string weight_lines;
        {
            std::ifstream wf{combo_out + ".weights.txt"};
            std::string line;
            while (std::getline(wf, line)) {
                if (line.rfind("w[", 0) == 0) {
                    // Strip the trailing per-alpha label (a directory path
                    // that legitimately differs between the two temp runs);
                    // keep only "w[a]=value".
                    const auto sp = line.find(' ');
                    weight_lines += line.substr(0, sp) + '\n';
                }
            }
        }

        std::error_code ec;
        fs::remove(panel_path, ec);
        fs::remove_all(alphas_dir, ec);
        fs::remove(combo_out, ec);
        fs::remove(combo_out + ".weights.txt", ec);
        fs::remove(combo_out + ".meta", ec);
        return Fitted{r.value_or(atx::impl::StageResult{}), weight_lines};
    };

    const Fitted base = run_with_close(base_close, "pit_base");

    // Perturb every close cell at date >= fit_end by a LARGE, structure-
    // breaking multiplicative shock — if the fit ever read this region the
    // fitted weights would visibly change.
    std::vector<f64> perturbed = base_close;
    for (usize t = kFitEnd; t < kDates; ++t) {
        for (usize i = 0; i < kInsts; ++i) {
            perturbed[t * kInsts + i] *= 3.7;
        }
    }
    const Fitted pert = run_with_close(perturbed, "pit_pert");

    ASSERT_FALSE(base.weight_lines.empty());
    EXPECT_EQ(base.weight_lines, pert.weight_lines)
        << "perturbing panel rows >= fit_end must not change the FITTED Stack weights (PIT)";
    EXPECT_EQ(find_kv(base.sr, "stack_verdict_hash"), find_kv(pert.sr, "stack_verdict_hash"))
        << "the verdict itself must be PIT-invariant too";
}

} // namespace atxtest_stage_combine_stack
