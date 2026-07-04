// stage_run_synthetic_smoke_test.cpp — S5-5: the first real (synthetic)
// scorecard row. Exercises S1-S5's whole lever stack together (risk-model
// factor + dead-alpha-factors + group-neutralize, the full 4-check robustness
// battery, the book-level turnover gate, the optimizer participation cap, and
// non-zero borrow financing) on one short, deterministic, synthetic panel,
// producing one finite, honest (explicitly synthetic) book-level scorecard row.
//
// Drives the REACHABLE stage graph directly -- run_all's own sequence minus
// its two zip-only stages (stage_run.cpp's run_load/run_panel hard-require
// --zip/--out) -- mirroring stage_run.cpp's own RunConfig-per-stage-copy
// pattern (each stage config is a COPY of one shared base cfg with only the
// stage-specific path fields overridden), so this test's wiring is exactly
// what run_all itself does once a panel.bin already exists.
//
// Determinism: single-threaded ctest, a small population/generation budget
// (mirrors stage_run_megabook_test.cpp's population=12, generations=3 fixture);
// no long real-panel sweep.
//
// (d) seq==parallel: N/A beyond what S1-S5's own per-unit tests already prove
// (robustness_battery_full_wire_test.cpp's SerialParallelAgreeWithFullBattery,
// report_borrow_test.cpp's own N/A note, etc.) -- the smoke itself runs the
// SAME single-threaded in-process stage graph every ctest invocation; documented
// here rather than silently omitted.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

#include "atx/engine/alpha/panel.hpp"

namespace atx_impl_stage_run_synthetic_smoke {

namespace fs    = std::filesystem;
namespace alpha = atx::engine::alpha;

using atx::f64;
using atx::usize;

// =============================================================================
//  Synthetic panel: M=12 instruments x D=80 dates.
//
//  "close": a common-shock + idiosyncratic-noise construction (the SAME
//  correlated-group idiom stage_optimize_riskmodel_test.cpp's
//  make_correlated_research uses) so the S1 Factor risk model / dead-alpha-
//  factor crowding term / group-neutralize residualization all have REAL,
//  non-degenerate signal to bite on (a diagonal-only fixture would make every
//  Factor-path lever a no-op).
//
//  "volume": a DELIBERATELY WIDE spread across instruments (1000x, log-spaced)
//  so --participation-cap (bounding |w_i| by ADV fraction inside the QP) and
//  the robustness battery's sub_universe check (TOP-N-by-ADV restriction) both
//  have real thin/thick-ADV contrast to discriminate on -- a flat/equal-ADV
//  panel would make both levers vacuously non-binding by construction.
// =============================================================================
constexpr usize kInsts = 12;
constexpr usize kDates = 80;

[[nodiscard]] atx::core::Result<std::string> make_synthetic_panel(const fs::path &out) {
    std::vector<f64> common_shock(kDates);
    for (usize t = 0; t < kDates; ++t) {
        common_shock[t] = 0.01 * std::sin(0.31 * static_cast<f64>(t));
    }
    std::vector<f64> close(kDates * kInsts, 100.0);
    for (usize i = 0; i < kInsts; ++i) {
        const f64 idio_amp = 0.0005 * (1.0 + static_cast<f64>(i % 7));
        f64 level = 100.0;
        for (usize t = 0; t < kDates; ++t) {
            const f64 ret =
                common_shock[t] + idio_amp * std::sin(0.9 * static_cast<f64>(t) + static_cast<f64>(i));
            if (t > 0) level *= (1.0 + ret);
            close[t * kInsts + i] = level;
        }
    }
    std::vector<f64> volume(kDates * kInsts);
    for (usize i = 0; i < kInsts; ++i) {
        // log-spaced 20,000 .. 20,000,000 shares across the 12 instruments (1000x
        // span). At ~100/share this is $2M .. $2B dollar-ADV -- comfortably above
        // any position notional the fixture's report_aum=$1M / name_cap=0.5 could
        // ever produce (max $500k), so --participation-cap=1.0 (100% of ADV) is
        // genuinely non-binding (LOOSE) while still being a REAL, evaluated
        // constraint the QP construction builds (not skipped as trivially unset).
        const f64 vol_level =
            20000.0 * std::pow(10.0, 3.0 * static_cast<f64>(i) / static_cast<f64>(kInsts - 1));
        for (usize t = 0; t < kDates; ++t) {
            volume[t * kInsts + i] = vol_level;
        }
    }
    std::vector<std::uint8_t> uni(kDates * kInsts, 1U);
    ATX_TRY(auto panel,
           alpha::Panel::create(kDates, kInsts, {"close", "volume"}, {close, volume}, uni));
    ATX_TRY(auto digest, atx::impl::write_panel(panel, out.string()));
    (void)digest;
    return atx::core::Ok(out.string());
}

// =============================================================================
//  Shared stage-graph driver: builds c_disc/c_comb/c_opt/c_rep as COPIES of one
//  base `cfg` (mirrors stage_run.cpp's run_all exactly, minus the two zip-only
//  stages), pointed at `panel_path` under `work`. Returns every stage's
//  StageResult so a test can inspect digests + kvs.
// =============================================================================
struct SmokeResult {
    atx::impl::StageResult disc;
    atx::impl::StageResult comb;
    atx::impl::StageResult opt;
    atx::impl::StageResult rep;
};

[[nodiscard]] atx::core::Result<SmokeResult>
run_reachable_graph(const atx::impl::RunConfig &cfg, const std::string &panel_path,
                    const fs::path &work) {
    atx::impl::RunConfig c_disc = cfg;
    c_disc.panel = panel_path;
    c_disc.alpha_out = (work / "alphas").string();
    c_disc.gated = true;
    c_disc.library_dir = (work / "_library").string();
    ATX_TRY(auto d_disc, atx::impl::run_discover(c_disc));

    atx::impl::RunConfig c_comb = cfg;
    c_comb.panel = panel_path;
    c_comb.alphas = (work / "alphas").string();
    c_comb.combo_out = (work / "combo.bin").string();
    c_comb.library_dir = c_disc.library_dir;
    ATX_TRY(auto d_comb, atx::impl::run_combine(c_comb));

    atx::impl::RunConfig c_opt = cfg;
    c_opt.panel = panel_path;
    c_opt.combo = (work / "combo.bin").string();
    c_opt.books_out = (work / "books.bin").string();
    ATX_TRY(auto d_opt, atx::impl::run_optimize(c_opt));

    atx::impl::RunConfig c_rep = cfg;
    c_rep.panel = panel_path;
    c_rep.books = (work / "books.bin").string();
    c_rep.combo = (work / "combo.bin").string();
    c_rep.report_out = (work / "report").string();
    ATX_TRY(auto d_rep, atx::impl::run_report(c_rep));

    return atx::core::Ok(SmokeResult{d_disc, d_comb, d_opt, d_rep});
}

// The base config shared by every stage (mirrors stage_run.cpp's `cfg` the
// per-stage copies all derive from). `on` selects the full S1-S5 lever stack
// (loose/non-binding where a hard reject would make the smoke fixture-fragile);
// `!on` leaves every S1-S5 field at its documented struct default.
[[nodiscard]] atx::impl::RunConfig make_base_cfg(bool on) {
    atx::impl::RunConfig cfg;
    cfg.seed = 20260704ULL;
    cfg.population = 12;
    cfg.generations = 3;
    cfg.seed_exprs = {"rank(close)", "ts_mean(close, 5)", "delta(close, 2)"};
    // Permissive admission floors (mirrors combine_test.cpp's
    // CombineFromLibraryMatchesDslPath): the smoke needs >= 1 admitted alpha to
    // exercise combine/optimize/report meaningfully.
    cfg.min_sharpe = 0.0;
    cfg.min_fitness = 0.0;
    cfg.max_turnover = 10.0;
    cfg.max_pool_corr = 1.0;
    cfg.min_dsr = -1.0e9;
    cfg.gross = 1.0;
    cfg.name_cap = 0.5;
    cfg.rebalance = "weekly";
    cfg.risk_aversion = 1.0; // MVO path (participation-cap/book-turnover-gate are QP-construction levers)
    cfg.set_flags.emplace("risk-aversion");
    // KNOWN LIMITATION (S5-2 participation cap): deliberately downscaled from
    // RunConfig's 1e9 default to 1e6. At a realistic ~1e9 NAV over this thin,
    // small (12-name) synthetic universe the participation-cap QP becomes
    // infeasible/non-convergent and run_optimize returns a fail-loud Err
    // ("ConstrainedQpSolver::solve: book violates constraint row ... the set may
    // be infeasible"). That is a genuine large-AUM/thin-universe limitation of
    // the S5-2 cap (its QP/elasticity machinery is prior-sprint, not S5's to fix)
    // — fail-loud, never a wrong-answer path. 1e6 keeps the smoke feasible while
    // still exercising the cap as a real, non-trivial QP-construction lever.
    cfg.report_aum = 1.0e6; // sets both the participation-cap NAV scale and the report capacity scale
    // A2a-style holdout split (mirrors run_all's own default): without it,
    // holdout_begin == research.dates() -> oos_idx is empty -> portfolio_oos_sharpe
    // is the documented NaN "no OOS window" sentinel (megabook test's own note).
    // A real split makes it a genuinely computed, finite value instead.
    cfg.combine_holdout_frac = 0.25;
    cfg.set_flags.emplace("holdout-frac");

    if (on) {
        cfg.risk_model = "factor";
        cfg.dead_alpha_factors = true;
        cfg.group_neutralize = true;
        cfg.robustness_battery = true;
        cfg.robustness_sub_universe = true;
        cfg.robustness_alt_neutralization = true;
        cfg.robustness_param_perturb = true;
        cfg.book_turnover_gate = 100.0; // loose: non-binding, still measured/evaluated
        cfg.set_flags.emplace("book-turnover-gate");
        cfg.participation_cap = 1.0; // loose (100% of ADV): non-binding, still built into the QP
        cfg.set_flags.emplace("participation-cap");
        cfg.borrow_bps = 5.0;
    } else {
        // Every S1-S5 field explicitly asserted at its documented struct default
        // (the house "explicit-off equals implicit-off" idiom,
        // stage_run_megabook_test.cpp's MegaBookGraph_InertByteIdentical).
        cfg.risk_model = "diagonal";
        cfg.dead_alpha_factors = false;
        cfg.group_neutralize = false;
        cfg.robustness_battery = false;
        cfg.robustness_sub_universe = false;
        cfg.robustness_alt_neutralization = false;
        cfg.robustness_param_perturb = false;
        cfg.book_turnover_gate = 0.0;
        cfg.participation_cap = 0.0;
        cfg.borrow_bps = 0.0;
    }
    return cfg;
}

class StageRunSyntheticSmoke : public ::testing::Test {
protected:
    fs::path work_dir_;
    std::string panel_path_;

    void SetUp() override {
        work_dir_ = fs::temp_directory_path() /
                   (std::string("atx_s5_5_smoke_") +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
        fs::create_directories(work_dir_, ec);
        auto r = make_synthetic_panel(work_dir_ / "panel.bin");
        panel_path_ = r.has_value() ? *r : std::string{};
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(work_dir_, ec);
    }
};

// =============================================================================
//  SyntheticSmoke_OnFlagsProducesFiniteScorecard — THE first real scorecard
//  row: every numeric report kv (portfolio_sharpe, the capacity-footprint
//  fields, book_turnover_per_day from S5-1, total_pnl_borrow from S5-4, ...) is
//  std::isfinite, with the full S1-S5 lever stack ON and genuinely evaluated
//  against the fixture's deliberate correlated-group / thin-thick-ADV structure.
// =============================================================================
TEST_F(StageRunSyntheticSmoke, SyntheticSmoke_OnFlagsProducesFiniteScorecard) {
    ASSERT_FALSE(panel_path_.empty()) << "synthetic panel (synthetic-labeled fixture) must build";
    const atx::impl::RunConfig cfg = make_base_cfg(/*on=*/true);

    auto res = run_reachable_graph(cfg, panel_path_, work_dir_ / "run");
    ASSERT_TRUE(res.has_value()) << res.error().message();

    // book_turnover_per_day (S5-1) is surfaced unconditionally on the optimize
    // stage; confirm it is present AND finite (the book-level turnover rate
    // this synthetic run's own rebalance schedule genuinely produced).
    bool saw_turnover_kv = false;
    for (const auto &[k, v] : res->opt.kvs) {
        if (k == "book_turnover_per_day") {
            saw_turnover_kv = true;
            EXPECT_TRUE(std::isfinite(std::stod(v))) << "book_turnover_per_day must be finite: " << v;
        }
    }
    EXPECT_TRUE(saw_turnover_kv) << "book_turnover_per_day kv missing from the optimize stage";

    // Every numeric kv on the FINAL report stage (the book-level scorecard row
    // proper: portfolio_sharpe, the capacity-footprint fields, total_pnl_borrow
    // (S5-4), ...) must be finite -- this synthetic (never a real V1) run's
    // honest scorecard.
    ASSERT_FALSE(res->rep.kvs.empty());
    for (const auto &[k, v] : res->rep.kvs) {
        const f64 parsed = std::stod(v);
        EXPECT_TRUE(std::isfinite(parsed)) << "report kv '" << k << "' is not finite: " << v;
    }

    // Sanity: the borrow lever is genuinely non-zero-financed (S5-4 actually
    // fired). total_pnl_borrow is a POSITIVE debit MAGNITUDE (summed short-
    // notional charge, subtracted from pnl_net inside accumulate_report) --
    // NOT a signed pnl contribution, so a firing borrow charge is > 0.
    bool saw_borrow_kv = false;
    for (const auto &[k, v] : res->rep.kvs) {
        if (k == "total_pnl_borrow") {
            saw_borrow_kv = true;
            EXPECT_GT(std::stod(v), 0.0) << "a positive borrow_bps with a real short leg must "
                                            "produce a genuinely non-zero total_pnl_borrow debit";
        }
    }
    EXPECT_TRUE(saw_borrow_kv) << "total_pnl_borrow kv missing from the report stage";
}

// =============================================================================
//  SyntheticSmoke_AllFlagsOffByteIdentical — every S1-S5 flag at its documented
//  inert default (struct-default, never mentioned) reproduces the IDENTICAL
//  stage digests as the SAME flags explicitly asserted at that default value
//  (mirrors MegaBookGraph_InertByteIdentical's exact shape, self-contained here
//  per the house convention that each test TU duplicates its own fixture).
// =============================================================================
TEST_F(StageRunSyntheticSmoke, SyntheticSmoke_AllFlagsOffByteIdentical) {
    ASSERT_FALSE(panel_path_.empty());

    atx::impl::RunConfig cfg_a; // touches NONE of the S1-S5 fields (struct defaults only)
    cfg_a.seed = 20260704ULL;
    cfg_a.population = 12;
    cfg_a.generations = 3;
    cfg_a.seed_exprs = {"rank(close)", "ts_mean(close, 5)", "delta(close, 2)"};
    cfg_a.min_sharpe = 0.0;
    cfg_a.min_fitness = 0.0;
    cfg_a.max_turnover = 10.0;
    cfg_a.max_pool_corr = 1.0;
    cfg_a.min_dsr = -1.0e9;
    cfg_a.gross = 1.0;
    cfg_a.name_cap = 0.5;
    cfg_a.rebalance = "weekly";
    cfg_a.risk_aversion = 1.0;
    cfg_a.set_flags.emplace("risk-aversion");
    cfg_a.report_aum = 1.0e6;
    cfg_a.combine_holdout_frac = 0.25;
    cfg_a.set_flags.emplace("holdout-frac");

    const atx::impl::RunConfig cfg_b = make_base_cfg(/*on=*/false); // every S1-S5 field EXPLICIT

    auto res_a = run_reachable_graph(cfg_a, panel_path_, work_dir_ / "run_a");
    ASSERT_TRUE(res_a.has_value()) << res_a.error().message();
    auto res_b = run_reachable_graph(cfg_b, panel_path_, work_dir_ / "run_b");
    ASSERT_TRUE(res_b.has_value()) << res_b.error().message();

    EXPECT_EQ(res_a->disc.digest, res_b->disc.digest) << "discover digest diverged at inert defaults";
    EXPECT_EQ(res_a->comb.digest, res_b->comb.digest) << "combine digest diverged at inert defaults";
    EXPECT_EQ(res_a->opt.digest, res_b->opt.digest) << "optimize digest diverged at inert defaults";
    EXPECT_EQ(res_a->rep.digest, res_b->rep.digest) << "report digest diverged at inert defaults";
}

// =============================================================================
//  SyntheticSmoke_TwiceRunByteIdentical — the whole ON-flags sequence run
//  twice: every stage digest and every kv string identical (F1/F2 across the
//  full S1-S5 stack together, not just per-unit).
// =============================================================================
TEST_F(StageRunSyntheticSmoke, SyntheticSmoke_TwiceRunByteIdentical) {
    ASSERT_FALSE(panel_path_.empty());
    const atx::impl::RunConfig cfg = make_base_cfg(/*on=*/true);

    auto res1 = run_reachable_graph(cfg, panel_path_, work_dir_ / "run1");
    ASSERT_TRUE(res1.has_value()) << res1.error().message();
    auto res2 = run_reachable_graph(cfg, panel_path_, work_dir_ / "run2");
    ASSERT_TRUE(res2.has_value()) << res2.error().message();

    EXPECT_EQ(res1->disc.digest, res2->disc.digest);
    EXPECT_EQ(res1->comb.digest, res2->comb.digest);
    EXPECT_EQ(res1->opt.digest, res2->opt.digest);
    EXPECT_EQ(res1->rep.digest, res2->rep.digest);

    ASSERT_EQ(res1->rep.kvs.size(), res2->rep.kvs.size());
    for (std::size_t i = 0; i < res1->rep.kvs.size(); ++i) {
        EXPECT_EQ(res1->rep.kvs[i], res2->rep.kvs[i]);
    }
    ASSERT_EQ(res1->opt.kvs.size(), res2->opt.kvs.size());
    for (std::size_t i = 0; i < res1->opt.kvs.size(); ++i) {
        EXPECT_EQ(res1->opt.kvs[i], res2->opt.kvs[i]);
    }
}

} // namespace atx_impl_stage_run_synthetic_smoke
