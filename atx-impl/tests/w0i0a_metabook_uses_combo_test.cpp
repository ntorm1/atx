// W0-I0a — metabook sleeves take their signals from the fitted combo weights (I-07), and
// the mega-book is point-in-time at every rebalance (I-04).
//
// Suite ImplMetabookUsesCombo_*:
//   * OneSleeveReproducesTheCombo — SingleSleeve over the library blends the members with
//     the fitted combo weights, so its fund book is byte-identical to the book built from
//     the combo panel itself; the pre-W0 equal-weight sleeve (EqualWeightV1) is not.
//   * SleevesFollowTheFittedWeights — multi-sleeve (BySignalFamily): refitting the combo
//     with a different method moves the V2 books; the V1 books ignore the combo entirely.
//   * MemberWithoutAComboWeightIsRefused — a library alpha the combo never weighted.
//   * FutureMutationLeavesPastBooksIdentical — prices and volumes mutated from kMutate on
//     (combo refit on each panel over a fit window ending at kMutate): every mega-book row
//     dated before kMutate is byte-identical; the V1 whole-panel risk lens moves them.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/record.hpp"

#include "config.hpp"
#include "stage_metabook.hpp"
#include "stages.hpp"
#include "w0i0a_fixtures.hpp"

namespace atx_test_w0_i0a_metabook_uses_combo {

namespace fx = atx_test_w0_i0a_fixtures;
namespace fs = std::filesystem;
namespace lib = atx::engine::library;
namespace combine = atx::engine::combine;
using atx::f64;
using atx::usize;
using atx::impl::MetaBookStageConfig;
using atx::impl::SleeveAssignment;
using atx::impl::SleeveSignalRule;

constexpr usize kDates = 120;
constexpr usize kMutate = 80;
const std::vector<std::string> kExprs{"rank(close)", "ts_mean(close,10)", "delta(close,2)",
                                      "rank(volume)"};

// A library holding kExprs (admission PnL is a decorrelated placeholder: the metabook
// re-evaluates each member's DSL on the research panel, it never reads library PnL).
void make_library(const fs::path& dir, const std::vector<std::string>& exprs) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    lib::GateConfig g;
    g.min_sharpe = -1e9;
    g.min_fitness = -1e9;
    g.max_turnover = 1e9;
    g.max_pool_corr = 1.1;
    const combine::AlphaGate gate{g};
    lib::Library facade = lib::Library::open(dir.string(), g, {});
    for (usize k = 0; k < exprs.size(); ++k) {
        std::vector<f64> pnl(16, 0.0);
        for (usize t = 0; t < pnl.size(); ++t) {
            pnl[t] = 0.01 * std::sin(0.7 * static_cast<f64>((k + 1) * (t + 1)));
        }
        const std::vector<f64> pos(pnl.size(), 0.10);
        const combine::AlphaMetrics m = combine::compute_metrics(pnl, pos, 1, 1.0);
        const lib::Provenance prov{exprs[k], {}, 0, 900ULL + k};
        const lib::AlphaCandidate cand{900ULL + k, pnl, std::span<const f64>{pos}, m, prov, 1,
                                       nullptr};
        const auto v = facade.admit(cand, gate);
        ASSERT_EQ(v.kind, lib::AdmitKind::Accept) << exprs[k];
    }
    ASSERT_TRUE(facade.flush_all().has_value());
}

struct World {
    fs::path dir;
    std::string panel;
    std::string lib_dir;
};

[[nodiscard]] World make_world(const std::string& tag, usize mutate_from = fx::kNever) {
    World w;
    w.dir = fx::fresh_dir("mbc_" + tag);
    fx::PanelSpec spec;
    spec.dates = kDates;
    spec.mutate_from = mutate_from;
    auto p = fx::write_research_panel(w.dir / "research.bin", spec);
    EXPECT_TRUE(p.has_value());
    w.panel = (w.dir / "research.bin").string();
    w.lib_dir = (w.dir / "lib").string();
    make_library(w.lib_dir, kExprs);
    return w;
}

// Combine from the library (the run_all wiring), fit window [0, fit_end).
[[nodiscard]] std::string combine_from_library(const World& w, const std::string& method,
                                               usize fit_end, const std::string& tag) {
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.panel = w.panel;
    cfg.library_dir = w.lib_dir;
    cfg.method = method;
    cfg.fit_end = static_cast<long>(fit_end);
    cfg.set_flags.insert("fit-end");
    cfg.combo_out = (w.dir / ("combo_" + tag + ".bin")).string();
    auto r = atx::impl::run_combine(cfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    return cfg.combo_out;
}

[[nodiscard]] atx::impl::RunConfig meta_cfg(const World& w, const std::string& combo, bool lib) {
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.panel = w.panel;
    cfg.combo = combo;
    cfg.library_dir = lib ? w.lib_dir : std::string{};
    cfg.gross = 1.0;
    cfg.name_cap = 1.0;
    cfg.rebalance = "weekly";
    return cfg;
}

[[nodiscard]] std::vector<std::vector<f64>> fund_books(const atx::impl::RunConfig& cfg,
                                                       const MetaBookStageConfig& scfg) {
    auto r = atx::impl::build_metabook_result(cfg, scfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    return r.has_value() ? r->fund_books : std::vector<std::vector<f64>>{};
}

[[nodiscard]] bool bit_equal(const std::vector<f64>& a, const std::vector<f64>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i])) return false;
    }
    return true;
}

[[nodiscard]] f64 max_abs_diff(const std::vector<std::vector<f64>>& a,
                               const std::vector<std::vector<f64>>& b) {
    f64 m = 0.0;
    for (usize s = 0; s < a.size() && s < b.size(); ++s) {
        for (usize i = 0; i < a[s].size() && i < b[s].size(); ++i) {
            m = std::max(m, std::fabs(a[s][i] - b[s][i]));
        }
    }
    return m;
}

TEST(ImplMetabookUsesCombo, OneSleeveReproducesTheCombo) {
    const World w = make_world("one");
    const std::string combo = combine_from_library(w, "shrinkage-mv", 90, "mv");
    MetaBookStageConfig scfg; // SingleSleeve, ComboWeightsV2
    const auto from_combo = fund_books(meta_cfg(w, combo, /*lib=*/false), scfg);
    const auto from_lib = fund_books(meta_cfg(w, combo, /*lib=*/true), scfg);
    ASSERT_EQ(from_combo.size(), kDates / 5U);
    ASSERT_EQ(from_lib.size(), from_combo.size());
    usize identical = 0;
    for (usize s = 0; s < from_combo.size(); ++s) {
        identical += bit_equal(from_combo[s], from_lib[s]) ? 1U : 0U;
    }
    EXPECT_EQ(identical, from_combo.size())
        << "one sleeve blended with the fitted weights IS the combo signal";
    MetaBookStageConfig v1 = scfg;
    v1.sleeve_signal = SleeveSignalRule::EqualWeightV1;
    const auto equal_weight = fund_books(meta_cfg(w, combo, /*lib=*/true), v1);
    const f64 v1_gap = max_abs_diff(from_combo, equal_weight);
    std::printf("[W0-I0a I-07] SingleSleeve over the library: V2 rows identical to the combo "
                "book %zu/%zu; V1 equal-weight max |dw| vs combo book = %.6g\n",
                identical, from_combo.size(), v1_gap);
    EXPECT_GT(v1_gap, 0.0) << "V1 ignored the fitted combiner";
}

TEST(ImplMetabookUsesCombo, SleevesFollowTheFittedWeights) {
    const World w = make_world("multi");
    const std::string combo_mv = combine_from_library(w, "shrinkage-mv", 90, "mv");
    const std::string combo_ic = combine_from_library(w, "ic", 90, "ic");
    ASSERT_NE(fx::weight_lines(combo_mv), fx::weight_lines(combo_ic));
    MetaBookStageConfig scfg;
    scfg.assignment = SleeveAssignment::BySignalFamily; // rank{0,3}, ts_mean{1}, delta{2}
    auto r = atx::impl::run_metabook(
        [&] {
            auto c = meta_cfg(w, combo_mv, true);
            c.books_out = (w.dir / "books_mv.bin").string();
            return c;
        }(),
        scfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(fx::find_kv(*r, "sleeves"), "3");
    const auto v2_mv = fund_books(meta_cfg(w, combo_mv, true), scfg);
    const auto v2_ic = fund_books(meta_cfg(w, combo_ic, true), scfg);
    MetaBookStageConfig v1 = scfg;
    v1.sleeve_signal = SleeveSignalRule::EqualWeightV1;
    const auto v1_mv = fund_books(meta_cfg(w, combo_mv, true), v1);
    const auto v1_ic = fund_books(meta_cfg(w, combo_ic, true), v1);
    const f64 v2_gap = max_abs_diff(v2_mv, v2_ic);
    const f64 v1_gap = max_abs_diff(v1_mv, v1_ic);
    std::printf("[W0-I0a I-07] 3 sleeves: max |dw| between shrinkage-mv and ic combos: "
                "V2=%.6g V1=%.6g\n", v2_gap, v1_gap);
    EXPECT_GT(v2_gap, 0.0) << "the sleeves must follow the fitted combo";
    EXPECT_EQ(v1_gap, 0.0) << "V1 never read the combo weights";
}

TEST(ImplMetabookUsesCombo, MemberWithoutAComboWeightIsRefused) {
    const World w = make_world("missing");
    // Combo fitted on a loose DSL dir holding only the first two expressions.
    const fs::path alphas = w.dir / "subset";
    fx::write_dsl_dir(alphas, {kExprs[0], kExprs[1]});
    atx::impl::RunConfig cc = fx::permissive_cfg();
    cc.panel = w.panel;
    cc.alphas = alphas.string();
    cc.method = "equal";
    cc.combo_out = (w.dir / "combo_subset.bin").string();
    ASSERT_TRUE(atx::impl::run_combine(cc).has_value());
    MetaBookStageConfig scfg;
    scfg.assignment = SleeveAssignment::BySignalFamily;
    auto r = atx::impl::build_metabook_result(meta_cfg(w, cc.combo_out, true), scfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message().find("no fitted combo weight"), std::string::npos)
        << r.error().message();
}

TEST(ImplMetabookUsesCombo, FutureMutationLeavesPastBooksIdentical) {
    const World a = make_world("fut_a");
    const World b = make_world("fut_b", kMutate);
    const std::string ca = combine_from_library(a, "shrinkage-mv", kMutate, "mv");
    const std::string cb = combine_from_library(b, "shrinkage-mv", kMutate, "mv");
    ASSERT_EQ(fx::weight_lines(ca), fx::weight_lines(cb)) << "fit window ends at the mutation";
    const usize past = (kMutate + 4U) / 5U; // steps dated 0..75
    for (const bool multi : {false, true}) {
        SCOPED_TRACE(multi ? "BySignalFamily" : "SingleSleeve(no library)");
        MetaBookStageConfig scfg;
        scfg.assignment = multi ? SleeveAssignment::BySignalFamily : SleeveAssignment::SingleSleeve;
        const auto ba = fund_books(meta_cfg(a, ca, multi), scfg);
        const auto bb = fund_books(meta_cfg(b, cb, multi), scfg);
        MetaBookStageConfig v1 = scfg;
        v1.pit.diag = atx::impl::DiagRiskRule::WholePanelV1;
        const auto va = fund_books(meta_cfg(a, ca, multi), v1);
        const auto vb = fund_books(meta_cfg(b, cb, multi), v1);
        ASSERT_EQ(ba.size(), kDates / 5U);
        usize same_v2 = 0;
        usize same_v1 = 0;
        for (usize s = 0; s < past; ++s) {
            same_v2 += bit_equal(ba[s], bb[s]) ? 1U : 0U;
            same_v1 += bit_equal(va[s], vb[s]) ? 1U : 0U;
        }
        std::printf("[W0-I0a I-04] metabook %s: past rows identical under future mutation "
                    "V2 %zu/%zu, V1 %zu/%zu\n",
                    multi ? "3 sleeves" : "1 sleeve", same_v2, past, same_v1, past);
        EXPECT_EQ(same_v2, past);
        EXPECT_LT(same_v1, past) << "the V1 whole-panel lens reads the future";
    }
}

} // namespace atx_test_w0_i0a_metabook_uses_combo
