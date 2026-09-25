// W0-I0a — the combine stage never reads the final test (I-02, I-03, I-08).
//
// Suite ImplCombineNoHoldoutRead_*:
//   * ShippedWeightsIgnoreHoldoutMutation — the plan's acceptance item "mutating holdout
//     PnL leaves the shipped weights byte-identical": prices AND volumes on every date
//     >= fit_end are replaced by a different random walk; with conviction, crowding,
//     capacity (floor + target AUM), Kelly and a walk-forward harness all ON, the weights
//     sidecar lines and every fit-derived kv (conviction, capacity, walk-forward, breadth,
//     stack verdict) stay byte-identical, for several methods including stack.
//   * LegacyRulesReadTheHoldout — the same mutation under the *V1 rules moves the
//     conviction-weighted weights, the capacity kvs and the walk-forward kvs: the test has
//     teeth, and the pre-W0 behaviour is still reproducible.
//   * WalkForwardStackRuns — the acceptance item "walk-forward with --method stack runs":
//     V2 runs K=2 folds with an (h + delay) embargo; the V1 fold loop fails for stack.
//   * CapacityChargesTradesNotHoldings — I-03: a zero-turnover alpha (rank(size)) with a
//     positive edge is no longer capacity-starved by the size of its holdings.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "stage_combine.hpp"
#include "stages.hpp"
#include "w0i0a_fixtures.hpp"

namespace atx_test_w0_i0a_combine_no_holdout_read {

namespace fx = atx_test_w0_i0a_fixtures;
namespace fs = std::filesystem;
using atx::f64;
using atx::usize;
using atx::impl::CombinePitConfig;

constexpr usize kDates = 160;
constexpr usize kInsts = 12;
constexpr usize kFitEnd = 120; // holdout_frac 0.25 -> fit [0, 120), test [120, 160)

struct Fixture {
    fs::path dir;
    std::string base_panel;
    std::string mutated_panel;
    std::string alphas;
};

[[nodiscard]] Fixture make_fixture(const std::string& tag) {
    Fixture f;
    f.dir = fx::fresh_dir("cnhr_" + tag);
    fx::PanelSpec spec;
    spec.dates = kDates;
    spec.insts = kInsts;
    auto a = fx::write_research_panel(f.dir / "base.bin", spec);
    EXPECT_TRUE(a.has_value()) << (a ? "" : a.error().message());
    spec.mutate_from = kFitEnd;
    auto b = fx::write_research_panel(f.dir / "mutated.bin", spec);
    EXPECT_TRUE(b.has_value()) << (b ? "" : b.error().message());
    f.base_panel = (f.dir / "base.bin").string();
    f.mutated_panel = (f.dir / "mutated.bin").string();
    f.alphas = (f.dir / "alphas").string();
    fx::write_dsl_dir(f.alphas, {"rank(close)", "ts_mean(close,10)", "delta(close,2)",
                                 "rank(volume)", "rank(size)"});
    return f;
}

[[nodiscard]] atx::impl::RunConfig combine_cfg(const Fixture& f, const std::string& method) {
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.alphas = f.alphas;
    cfg.method = method;
    cfg.combine_holdout_frac = 0.25;
    cfg.set_flags.emplace("holdout-frac");
    cfg.conviction = true;
    cfg.corr_penalty = 0.3;
    cfg.capacity_floor = 1.0e6;
    cfg.target_aum = 1.0e7;
    cfg.walk_forward = 2;
    return cfg;
}

struct CombineRun {
    atx::impl::StageResult sr;
    std::vector<std::string> weights;
};

[[nodiscard]] CombineRun run_on(atx::impl::RunConfig cfg, const std::string& panel,
                                const fs::path& out, const CombinePitConfig& pit) {
    cfg.panel = panel;
    cfg.combo_out = out.string();
    auto r = atx::impl::run_combine(cfg, pit);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    CombineRun run;
    if (r.has_value()) {
        run.sr = *r;
        run.weights = fx::weight_lines(cfg.combo_out);
    }
    return run;
}

// Every kv except the combined-signal digest ("combo"), which legitimately changes: the
// combined SIGNAL on holdout dates is built from holdout prices.
void expect_fit_kvs_equal(const atx::impl::StageResult& a, const atx::impl::StageResult& b) {
    ASSERT_EQ(a.kvs.size(), b.kvs.size());
    for (usize i = 0; i < a.kvs.size(); ++i) {
        ASSERT_EQ(a.kvs[i].first, b.kvs[i].first);
        if (a.kvs[i].first == "combo") {
            continue;
        }
        EXPECT_EQ(a.kvs[i].second, b.kvs[i].second) << "kv " << a.kvs[i].first;
    }
}

TEST(ImplCombineNoHoldoutRead, ShippedWeightsIgnoreHoldoutMutation) {
    const Fixture f = make_fixture("ship");
    struct Case {
        const char* method;
        f64 kelly;
    };
    const Case cases[] = {{"shrinkage-mv", 0.0}, {"ic", 0.0}, {"bounded", 0.0},
                          {"shrinkage-mv", 0.5}, {"stack", 0.0}};
    usize n = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string{c.method} + (c.kelly > 0.0 ? "+kelly" : ""));
        atx::impl::RunConfig cfg = combine_cfg(f, c.method);
        cfg.kelly_fraction = c.kelly;
        const std::string tag = std::to_string(n++);
        const CombineRun base = run_on(cfg, f.base_panel, f.dir / ("b" + tag + ".bin"), {});
        const CombineRun mut = run_on(cfg, f.mutated_panel, f.dir / ("m" + tag + ".bin"), {});
        ASSERT_EQ(base.weights.size(), 5U);
        EXPECT_EQ(base.weights, mut.weights) << "shipped weights read the holdout";
        expect_fit_kvs_equal(base.sr, mut.sr);
        EXPECT_NE(fx::find_kv(base.sr, "combo"), fx::find_kv(mut.sr, "combo"))
            << "the mutation must reach the holdout signal (else the test is vacuous)";
        EXPECT_EQ(fx::find_kv(base.sr, "holdout_begin"), std::to_string(kFitEnd));
        EXPECT_FALSE(fx::find_kv(base.sr, "walk_forward_oos_sharpe").empty());
        EXPECT_FALSE(fx::find_kv(base.sr, "capacity_alpha_aum").empty());
        EXPECT_FALSE(fx::find_kv(base.sr, "conviction_scores").empty());
        std::printf("[W0-I0a I-02/I-03/I-08] %s%s weights (same under holdout mutation):\n",
                    c.method, c.kelly > 0.0 ? "+kelly" : "");
        for (const std::string& w : base.weights) {
            std::printf("  %s\n", w.substr(0, w.find(' ')).c_str());
        }
    }
}

TEST(ImplCombineNoHoldoutRead, LegacyRulesReadTheHoldout) {
    const Fixture f = make_fixture("legacy");
    const atx::impl::RunConfig cfg = combine_cfg(f, "shrinkage-mv");
    CombinePitConfig v1;
    v1.conviction = atx::impl::ConvictionWindowRule::FullStreamV1;
    v1.capacity = atx::impl::CapacityRule::FullPeriodHoldingsV1;
    v1.walk_forward = atx::impl::WalkForwardRule::LinearNoEmbargoV1;
    const CombineRun base = run_on(cfg, f.base_panel, f.dir / "b.bin", v1);
    const CombineRun mut = run_on(cfg, f.mutated_panel, f.dir / "m.bin", v1);
    ASSERT_EQ(base.weights.size(), mut.weights.size());
    EXPECT_NE(base.weights, mut.weights) << "V1 conviction/capacity read the holdout";
    EXPECT_NE(fx::find_kv(base.sr, "conviction_scores"),
              fx::find_kv(mut.sr, "conviction_scores"));
    EXPECT_NE(fx::find_kv(base.sr, "capacity_alpha_aum"),
              fx::find_kv(mut.sr, "capacity_alpha_aum"));
    EXPECT_NE(fx::find_kv(base.sr, "walk_forward_oos_sharpe"),
              fx::find_kv(mut.sr, "walk_forward_oos_sharpe"));
    const std::vector<f64> wb = fx::weight_values((f.dir / "b.bin").string());
    const std::vector<f64> wm = fx::weight_values((f.dir / "m.bin").string());
    f64 max_dw = 0.0;
    for (usize a = 0; a < wb.size(); ++a) {
        max_dw = std::max(max_dw, std::fabs(wb[a] - wm[a]));
    }
    std::printf("[W0-I0a V1 leak] max |dw| under holdout mutation = %.6g\n"
                "  conviction V1 base=%s\n  conviction V1 mut =%s\n"
                "  capacity   V1 base=%s\n  capacity   V1 mut =%s\n",
                max_dw, fx::find_kv(base.sr, "conviction_scores").c_str(),
                fx::find_kv(mut.sr, "conviction_scores").c_str(),
                fx::find_kv(base.sr, "capacity_alpha_aum").c_str(),
                fx::find_kv(mut.sr, "capacity_alpha_aum").c_str());
    EXPECT_GT(max_dw, 0.0);
}

TEST(ImplCombineNoHoldoutRead, WalkForwardStackRuns) {
    const Fixture f = make_fixture("wfstack");
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.alphas = f.alphas;
    cfg.method = "stack"; // the --method stack CLI string, parsed by run_combine
    cfg.combine_holdout_frac = 0.25;
    cfg.set_flags.emplace("holdout-frac");
    cfg.walk_forward = 2;
    cfg.panel = f.base_panel;
    cfg.combo_out = (f.dir / "stack.bin").string();
    auto r = atx::impl::run_combine(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(fx::find_kv(*r, "method"), "stack");
    EXPECT_EQ(fx::find_kv(*r, "walk_forward_folds"), "2");
    EXPECT_EQ(fx::find_kv(*r, "walk_forward_embargo"), "2") << "h=1 stack horizon + delay 1";
    const std::string sharpes = fx::find_kv(*r, "walk_forward_oos_sharpe");
    ASSERT_FALSE(sharpes.empty());
    const auto comma = sharpes.find(',');
    ASSERT_NE(comma, std::string::npos);
    EXPECT_TRUE(std::isfinite(std::stod(sharpes.substr(0, comma))));
    EXPECT_TRUE(std::isfinite(std::stod(sharpes.substr(comma + 1))));
    std::printf("[W0-I0a I-08] stack walk-forward folds=2 embargo=2 oos_sharpes=%s mean=%s\n",
                sharpes.c_str(), fx::find_kv(*r, "walk_forward_oos_sharpe_mean").c_str());

    CombinePitConfig v1;
    v1.walk_forward = atx::impl::WalkForwardRule::LinearNoEmbargoV1;
    cfg.combo_out = (f.dir / "stack_v1.bin").string();
    auto r1 = atx::impl::run_combine(cfg, v1);
    EXPECT_FALSE(r1.has_value()) << "the pre-W0 fold loop fits a plain AlphaCombiner, which "
                                    "rejects Stack -- the I-08 defect";
}

TEST(ImplCombineNoHoldoutRead, WalkForwardFoldsStayInsideTheFitWindow) {
    const Fixture f = make_fixture("wfwindow");
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.alphas = f.alphas;
    cfg.method = "shrinkage-mv";
    cfg.combine_holdout_frac = 0.25;
    cfg.set_flags.emplace("holdout-frac");
    cfg.walk_forward = 3;
    const CombineRun base = run_on(cfg, f.base_panel, f.dir / "b.bin", {});
    const CombineRun mut = run_on(cfg, f.mutated_panel, f.dir / "m.bin", {});
    EXPECT_EQ(fx::find_kv(base.sr, "walk_forward_oos_sharpe"),
              fx::find_kv(mut.sr, "walk_forward_oos_sharpe"))
        << "a V2 fold never scores a date >= fit_end";
    // K=3 inside [0,120): seg=30; 3 + 2 embargo dates per fold are too many for seg < 4.
    cfg.walk_forward = 40;
    cfg.panel = f.base_panel;
    cfg.combo_out = (f.dir / "too_many.bin").string();
    auto r = atx::impl::run_combine(cfg);
    EXPECT_FALSE(r.has_value()) << "40 folds leave < 2 test dates after the embargo";
}

TEST(ImplCombineNoHoldoutRead, CapacityChargesTradesNotHoldings) {
    const Fixture f = make_fixture("capacity");
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.method = "equal";
    cfg.combine_holdout_frac = 0.25;
    cfg.set_flags.emplace("holdout-frac");
    cfg.capacity_floor = 1.0e6;
    cfg.target_aum = 1.0e7;
    const fs::path dir = f.dir / "cap_alphas";
    fx::write_dsl_dir(dir, {"rank(size)", "delta(close,1)"});
    cfg.alphas = dir.string();
    const CombineRun v2 = run_on(cfg, f.base_panel, f.dir / "v2.bin", {});
    CombinePitConfig v1_pit;
    v1_pit.capacity = atx::impl::CapacityRule::FullPeriodHoldingsV1;
    const CombineRun v1 = run_on(cfg, f.base_panel, f.dir / "v1.bin", v1_pit);
    const std::string caps_v2 = fx::find_kv(v2.sr, "capacity_alpha_aum");
    const std::string caps_v1 = fx::find_kv(v1.sr, "capacity_alpha_aum");
    std::printf("[W0-I0a I-03] capacity AUM (rank(size), delta(close,1)): V2 trades=%s  "
                "V1 holdings=%s  max_part V2=%s V1=%s\n",
                caps_v2.c_str(), caps_v1.c_str(),
                fx::find_kv(v2.sr, "capacity_max_participation").c_str(),
                fx::find_kv(v1.sr, "capacity_max_participation").c_str());
    const auto first = [](const std::string& csv) { return csv.substr(0, csv.find(',')); };
    const auto second = [](const std::string& csv) { return csv.substr(csv.find(',') + 1); };
    // rank(size) holds one book for the whole window: it trades once (the entry at t=0) and
    // never again, so its trade-based capacity must exceed the holdings-based figure.
    const f64 slow_v2 = first(caps_v2) == "inf" ? std::numeric_limits<f64>::infinity()
                                                : std::stod(first(caps_v2));
    const f64 slow_v1 = first(caps_v1) == "inf" ? std::numeric_limits<f64>::infinity()
                                                : std::stod(first(caps_v1));
    EXPECT_GT(slow_v1, 0.0) << "rank(size) must carry a positive edge on this fixture";
    EXPECT_GT(slow_v2, slow_v1) << "V2 must not charge a buy-and-hold book for its holdings";
    // delta(close,1) re-trades every day: its capacity stays finite under V2.
    const std::string fast = second(caps_v2);
    EXPECT_NE(fast, "inf");
}

} // namespace atx_test_w0_i0a_combine_no_holdout_read
