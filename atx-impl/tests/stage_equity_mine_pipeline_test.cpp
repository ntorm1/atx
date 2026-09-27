// Lane 9 equity-mine: the search -> score -> registry -> family -> BY/RW gate
// pipeline on synthetic panels. A planted alpha must be recovered and admitted;
// a null panel must admit nothing after FDR control.

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/eval/trial_registry.hpp"

#include "stage_equity_mine.hpp"

namespace atx_test_l9_realmine_pipeline {

using atx::engine::alpha::Panel;
namespace mine = atx::impl::mine;

constexpr std::size_t kDates = 520;
constexpr std::size_t kInst = 80;
constexpr std::size_t kTrainBegin = 30;
constexpr std::size_t kValBegin = 300;
constexpr std::size_t kHoldBegin = 420;

// Closes follow r(d)_i = beta * z_i(d - 2) + noise, where z = the `sig` field.
// beta = 0 gives a null panel (sig carries no information).
Panel synthetic_panel(double beta, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> n01(0.0, 1.0);
    const std::size_t cells = kDates * kInst;
    std::vector<double> sig(cells), close(cells), open(cells), high(cells), low(cells),
        volume(cells);
    for (auto &v : sig) v = n01(rng);
    std::vector<double> px(kInst, 50.0);
    for (std::size_t d = 0; d < kDates; ++d) {
        for (std::size_t i = 0; i < kInst; ++i) {
            const std::size_t c = d * kInst + i;
            if (d > 0) {
                const double planted = d >= 2 ? beta * sig[(d - 2) * kInst + i] : 0.0;
                px[i] *= 1.0 + planted + 0.02 * n01(rng);
            }
            close[c] = px[i];
            open[c] = px[i] * (1.0 + 0.002 * n01(rng));
            high[c] = std::max(open[c], close[c]) * 1.01;
            low[c] = std::min(open[c], close[c]) * 0.99;
            volume[c] = 1.0e6 * (1.5 + std::abs(n01(rng)));
        }
    }
    auto p = Panel::create(kDates, kInst, {"close", "open", "high", "low", "volume", "sig"},
                           {close, open, high, low, volume, sig},
                           std::vector<std::uint8_t>(cells, 1));
    EXPECT_TRUE(p.has_value());
    return std::move(p).value();
}

std::vector<mine::SeedExpr> seeds() {
    return {{"rank(sig)", "extra"},
            {"rank(close)", "extra"},
            {"rank(volume)", "extra"},
            {"-1 * rank(delta(close, 5))", "extra"},
            {"rank(open - close)", "extra"},
            {"rank(ts_mean(volume, 10))", "extra"},
            {"rank((high - low) / close)", "extra"},
            {"-1 * rank(close / delay(close, 20))", "extra"},
            {"this is not a dsl expression", "extra"}};
}

mine::MineConfig config(bool search) {
    mine::MineConfig cfg;
    cfg.run_search = search;
    cfg.search.master_seed = 7;
    cfg.search.population = 8;
    cfg.search.generations = 2;
    cfg.search.n_workers = 1;
    cfg.search_fields = {"close", "open", "high", "low", "volume", "sig"};
    cfg.score.cost_bps = 1.0;
    cfg.score.min_names = 20;
    cfg.max_validate = 20;
    cfg.max_corr = 0.9;
    cfg.fdr_q = 0.10;
    cfg.rw_alpha = 0.10;
    cfg.boot.n_boot = 200;
    cfg.boot.mean_block = 5.0;
    cfg.gate = mine::GateMode::By;
    return cfg;
}

atx::core::Result<mine::MineOutcome> run_mine(const Panel &panel, const mine::MineConfig &cfg,
                                             atx::engine::eval::TrialRegistry &reg) {
    static const atx::engine::alpha::Library lib;
    const std::vector<std::uint8_t> member(panel.cells(), 1);
    const mine::MineData train{&panel, member, {kTrainBegin, kValBegin}};
    const mine::MineData val{&panel, member, {kValBegin, kHoldBegin}};
    const auto s = seeds();
    return mine::mine(lib, train, val, s, cfg, reg);
}

atx::engine::eval::TrialRegistry make_registry() {
    atx::engine::eval::TrialRegistryConfig rc;
    rc.pnl_len = kValBegin - kTrainBegin;
    rc.sketch_dim = 64;
    auto r = atx::engine::eval::TrialRegistry::in_memory(rc);
    EXPECT_TRUE(r.has_value());
    return std::move(r).value();
}

TEST(EquityMinePipeline, PlantedAlphaIsRecoveredAndAdmitted) {
    const Panel panel = synthetic_panel(0.004, 11);
    auto reg = make_registry();
    auto out = run_mine(panel, config(false), reg);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out->seeds_invalid, 1u);
    ASSERT_FALSE(out->admitted.empty());
    const auto &top = out->candidates[out->admitted.front()];
    EXPECT_EQ(top.dsl, "rank(sig)");
    EXPECT_EQ(top.sign, 1.0);
    EXPECT_GT(top.validation.sharpe_net, 2.0);
    EXPECT_LE(top.p_by, 0.10);
    EXPECT_GT(top.train.ic_mean[0], 0.05);
    // The pre-registered family blend is hypothesis K+1 and carries the plant.
    ASSERT_TRUE(out->family_blend_scored);
    EXPECT_GT(out->family_blend_validation.sharpe_net, 0.0);
    EXPECT_LE(out->family_blend_p_by, 1.0);
    // Every scored candidate is a registered trial.
    std::size_t scored = 0;
    for (const auto &c : out->candidates) scored += c.scored ? 1 : 0;
    EXPECT_EQ(out->trials.n_raw, scored);
    EXPECT_GE(out->trials.n_eff, 1.0);
    // The complete-P&L branch must report the benchmark it actually used too.
    // These distinct planted/noise trials exercise a nonzero cluster penalty.
    EXPECT_EQ(out->trials.n_screened, 0U);
    EXPECT_EQ(out->dsr_rule, "cluster-mc-floor-v2");
    EXPECT_GT(out->dsr_clusters, 1U);
    EXPECT_GT(top.dsr_selection_benchmark, 0.0);
}

// Under the null every admission is a false discovery. BY bounds the expected
// false-discovery proportion by q, so across R independent null panels the
// number of runs admitting anything is ~Binomial(R, <= q). R = 12, q = 0.10:
// P(more than 3 runs admit) < 3% even at the bound; the realized count is far
// below it because BY is conservative.
TEST(EquityMinePipeline, NullPanelAdmitsAlmostNothingAfterFdr) {
    std::size_t runs_admitting = 0;
    std::size_t total_admitted = 0;
    std::size_t total_family = 0;
    for (std::uint64_t seed = 21; seed < 33; ++seed) {
        const Panel panel = synthetic_panel(0.0, seed);
        auto reg = make_registry();
        auto out = run_mine(panel, config(false), reg);
        ASSERT_TRUE(out.has_value()) << out.error().message();
        runs_admitting += (out->admitted.empty() && !out->family_blend_admitted) ? 0 : 1;
        total_admitted += out->admitted.size();
        total_family += out->family.size();
    }
    EXPECT_GT(total_family, 0u);
    EXPECT_LE(runs_admitting, 3u) << "admitted " << total_admitted << " of " << total_family;
    EXPECT_LE(total_admitted * 10, total_family) << "false admissions above 10% of tests";
}

TEST(EquityMinePipeline, SearchAddsRegisteredTrialsAndStillRecoversPlant) {
    const Panel panel = synthetic_panel(0.004, 12);
    auto reg = make_registry();
    auto out = run_mine(panel, config(true), reg);
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_GT(out->candidates.size(), seeds().size());
    EXPECT_GT(out->search_trial_count, 0u);
    std::size_t scored = 0;
    for (const auto &c : out->candidates) scored += c.scored ? 1 : 0;
    EXPECT_EQ(reg.size(), scored);
    bool planted = false;
    for (auto idx : out->admitted) planted |= out->candidates[idx].dsl == "rank(sig)";
    EXPECT_TRUE(planted);
}

TEST(EquityMinePipeline, RomanoWolfGateIsAtLeastAsStrictAsBothSubsets) {
    const Panel panel = synthetic_panel(0.004, 13);
    auto cfg = config(false);
    cfg.gate = mine::GateMode::Both;
    auto reg = make_registry();
    auto out = run_mine(panel, cfg, reg);
    ASSERT_TRUE(out.has_value());
    for (auto idx : out->admitted) {
        EXPECT_LE(out->candidates[idx].p_by, cfg.fdr_q);
        EXPECT_LE(out->candidates[idx].p_rw, cfg.rw_alpha);
    }
    ASSERT_FALSE(out->admitted.empty());
}

TEST(EquityMinePipeline, HoldoutScoresAdmittedAndBlend) {
    const Panel panel = synthetic_panel(0.004, 14);
    auto reg = make_registry();
    auto out = run_mine(panel, config(false), reg);
    ASSERT_TRUE(out.has_value());
    ASSERT_FALSE(out->admitted.empty());
    std::vector<mine::CandidateRow> admitted;
    for (auto idx : out->admitted) admitted.push_back(out->candidates[idx]);
    static const atx::engine::alpha::Library lib;
    const std::vector<std::uint8_t> member(panel.cells(), 1);
    const mine::MineData hold{&panel, member, {kHoldBegin, kDates}};
    auto rows = mine::evaluate_holdout(lib, hold, admitted, config(false).score);
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), admitted.size() + 1);
    EXPECT_EQ(rows->back().dsl, "<equal-weight blend>");
    EXPECT_GT(rows->front().score.sharpe_net, 1.0);
}

TEST(EquityMinePipeline, BlendOfOneAlphaEqualsItsOwnBook) {
    const Panel panel = synthetic_panel(0.004, 15);
    static const atx::engine::alpha::Library lib;
    const std::vector<std::uint8_t> member(panel.cells(), 1);
    const mine::MineData data{&panel, member, {kValBegin, kHoldBegin}};
    mine::CandidateRow row;
    row.dsl = "rank(sig)";
    row.sign = 1.0;
    const std::vector<mine::CandidateRow> rows{row};
    auto blend = mine::evaluate_blend(lib, data, rows, config(false).score);
    ASSERT_TRUE(blend.has_value()) << blend.error().message();
    auto hold = mine::evaluate_holdout(lib, data, rows, config(false).score);
    ASSERT_TRUE(hold.has_value());
    // A monotone transform of one signal ranks identically -> identical book.
    EXPECT_NEAR(blend->sharpe_net, hold->front().score.sharpe_net, 1e-9);
    EXPECT_FALSE(mine::evaluate_blend(lib, data, {}, config(false).score).has_value());
}

TEST(EquityMinePipeline, RejectsRegistryLengthMismatch) {
    const Panel panel = synthetic_panel(0.0, 5);
    atx::engine::eval::TrialRegistryConfig rc;
    rc.pnl_len = 10;
    auto reg = atx::engine::eval::TrialRegistry::in_memory(rc);
    ASSERT_TRUE(reg.has_value());
    auto out = run_mine(panel, config(false), *reg);
    EXPECT_FALSE(out.has_value());
}

// Cartesian-product exposures with a tiny alternating y perturbation: the null
// has balanced positive/negative IC and nonzero uncertainty (an exact constant
// IC correctly fails open). y is an explicit positive control. The null is
// constructed independently of the kernel's decision and no future labels fit it.
Panel screen_panel(bool poison_future = false) {
    constexpr std::size_t dates = 240, names = 256;
    std::vector<double> close(dates * names), sig(dates * names), plant(dates * names);
    double cumulative = 0.0;
    for (std::size_t d = 0; d < dates; ++d) {
        cumulative += 0.001 * (1.0 + 0.2 * std::sin(static_cast<double>(d)));
        for (std::size_t i = 0; i < names; ++i) {
            const auto cell = d * names + i;
            const double x = static_cast<double>(i / 8) - 15.5;
            const double y = static_cast<double>(i % 8) - 3.5;
            close[cell] = 50.0 * std::exp(cumulative * y);
            if (poison_future && d >= 200) close[cell] *= std::exp(x);
            sig[cell] = x + (d % 2 == 0 ? 0.0001 : -0.0001) * y;
            plant[cell] = y;
        }
    }
    auto p = Panel::create(dates, names, {"close", "sig", "plant"},
                           {close, sig, plant}, std::vector<std::uint8_t>(dates * names, 1));
    EXPECT_TRUE(p);
    return std::move(*p);
}

mine::MineConfig screen_config() {
    auto cfg = config(false);
    cfg.search.ic_screen.rule = atx::engine::factory::IcScreenRule::ConservativeV2;
    cfg.search.ic_screen.horizons = {1, 2, 3, 4};
    cfg.search.ic_screen.min_dates = 128;
    cfg.score.guard_returns = false;
    cfg.dsr_rule = mine::TrainDsrRule::SummaryNEffV1; // must fall back with unknown trials
    return cfg;
}

atx::engine::eval::TrialRegistry screen_registry() {
    atx::engine::eval::TrialRegistryConfig rc;
    rc.pnl_len = 180;
    rc.sketch_dim = 32;
    rc.format = atx::engine::eval::TrialLogFormat::V3;
    auto reg = atx::engine::eval::TrialRegistry::in_memory(rc);
    EXPECT_TRUE(reg);
    return std::move(*reg);
}

TEST(EquityMinePipeline, IcScreenSeedsCountWithoutBacktestsAndKeepPositiveControl) {
    const auto panel = screen_panel();
    const atx::engine::alpha::Library lib;
    const mine::MineData train{&panel, std::vector<std::uint8_t>(panel.cells(), 1), {20, 200}};
    const std::vector<mine::SeedExpr> inputs{{"sig", "null"}, {"plant", "positive"}};
    auto reg = screen_registry();
    auto out = mine::mine_train(lib, train, inputs, screen_config(), reg);
    ASSERT_TRUE(out) << out.error().message();
    ASSERT_EQ(out->candidates.size(), 2U);
    const auto &null = out->candidates[0];
    EXPECT_TRUE(null.ic_rejected);
    EXPECT_FALSE(null.scored);
    EXPECT_TRUE(null.train.net.empty());
    EXPECT_TRUE(std::isnan(null.dsr_train));
    EXPECT_TRUE(out->candidates[1].scored);
    EXPECT_FALSE(out->candidates[1].ic_rejected);
    EXPECT_EQ(out->trials.n_raw, 2U);
    EXPECT_EQ(out->trials.n_screened, 1U);
    EXPECT_EQ(out->trials.n_full_pnl, 1U);
    EXPECT_TRUE(std::isnan(out->trials.n_eff));
    EXPECT_EQ(out->dsr_rule, "summary-raw-n-v2");
    EXPECT_EQ(out->dsr_marginal_floor_count, 1U);
    EXPECT_GT(out->candidates[1].dsr_selection_benchmark, 0.0);
    EXPECT_DOUBLE_EQ(out->trials.var_sr, 0.0); // observed moment, never replaced by the floor
    EXPECT_EQ(out->ic_screen_evaluations, 2U);
    EXPECT_EQ(out->ic_screen.window_begin, 20U);
    EXPECT_EQ(out->ic_screen.window_end, 200U);
    EXPECT_EQ(out->ic_screen.maturity_end, 200U);
    EXPECT_FALSE(out->ic_screen_recipe.empty());
}

TEST(EquityMinePipeline, IcScreenUsesTrainingCutoffAndRejectIdentityBindsRecipe) {
    const auto a = screen_panel(), b = screen_panel(true);
    const atx::engine::alpha::Library lib;
    const std::vector<mine::SeedExpr> inputs{{"sig", "null"}};
    auto reg = screen_registry();
    auto cfg = screen_config();
    const mine::MineData ta{&a, std::vector<std::uint8_t>(a.cells(), 1), {20, 200}};
    mine::MineData tb{&b, std::vector<std::uint8_t>(b.cells(), 1), {20, 200}};
    std::fill(tb.member.begin() + 200 * b.instruments(), tb.member.end(), 0);
    auto first = mine::mine_train(lib, ta, inputs, cfg, reg);
    auto future = mine::mine_train(lib, tb, inputs, cfg, reg);
    ASSERT_TRUE(first); ASSERT_TRUE(future);
    EXPECT_TRUE(first->candidates[0].ic_rejected);
    EXPECT_EQ(first->candidates[0].config_hash, future->candidates[0].config_hash);
    EXPECT_EQ(reg.size(), 1U); // same recipe/decision despite future-only changes
    cfg.search.ic_screen.practical_abs_ic = 0.03;
    auto changed = mine::mine_train(lib, ta, inputs, cfg, reg);
    ASSERT_TRUE(changed);
    EXPECT_TRUE(changed->candidates[0].ic_rejected);
    EXPECT_NE(first->candidates[0].config_hash, changed->candidates[0].config_hash);
    EXPECT_EQ(reg.size(), 2U);
}

TEST(EquityMinePipeline, IcScreenPropagatesSearchRejectToEquivalentSeed) {
    const auto panel = screen_panel();
    const atx::engine::alpha::Library lib;
    const mine::MineData train{&panel, std::vector<std::uint8_t>(panel.cells(), 1), {20, 200}};
    const std::vector<mine::SeedExpr> inputs{{"rank( sig )", "null"}};
    auto cfg = screen_config();
    cfg.run_search = true;
    cfg.search.population = 4;
    cfg.search.generations = 1;
    cfg.search.n_immigrants = 0;
    cfg.search.fidelity.enabled = false;
    cfg.search_fields = {"sig"};
    auto reg = screen_registry();
    auto out = mine::mine_train(lib, train, inputs, cfg, reg);
    ASSERT_TRUE(out) << out.error().message();
    EXPECT_GT(out->search_ic_screen_rejected, 0U);
    EXPECT_TRUE(out->candidates[0].ic_rejected);
    EXPECT_FALSE(out->candidates[0].ic_screen_evaluated); // inherited; never re-evaluated
    EXPECT_FALSE(out->candidates[0].scored);
    EXPECT_TRUE(out->candidates[0].train.net.empty());
    EXPECT_EQ(out->trials.n_raw, out->trials.n_full_pnl + out->trials.n_screened);
}

TEST(EquityMinePipeline, IcScreenFailsOpenWithMissingSupportOrCacheBudget) {
    const auto panel = screen_panel();
    const atx::engine::alpha::Library lib;
    mine::MineData train{&panel, std::vector<std::uint8_t>(panel.cells(), 1), {20, 200}};
    const std::vector<mine::SeedExpr> inputs{{"sig", "null"}};
    auto cfg = screen_config();
    cfg.search.ic_screen.max_cache_bytes = 1;
    auto reg = screen_registry();
    auto out = mine::mine_train(lib, train, inputs, cfg, reg);
    ASSERT_TRUE(out);
    EXPECT_FALSE(out->candidates[0].ic_rejected);
    EXPECT_EQ(out->ic_screen_evaluations, 0U);
    EXPECT_EQ(out->ic_screen_unavailable, 1U);
    cfg = screen_config();
    cfg.score.guard_returns = true;
    train.guard.dates = panel.dates();
    train.guard.instruments = panel.instruments();
    train.guard.bad_prefix.resize(panel.cells());
    for (std::size_t d = 0; d < panel.dates(); ++d)
        for (std::size_t i = 0; i < panel.instruments(); ++i)
            train.guard.bad_prefix[d * panel.instruments() + i] = static_cast<atx::u32>(d);
    auto guarded = mine::mine_train(lib, train, inputs, cfg, reg);
    ASSERT_TRUE(guarded);
    EXPECT_FALSE(guarded->candidates[0].ic_rejected);
    EXPECT_EQ(guarded->candidates[0].ic_screen_reason,
              atx::engine::factory::IcScreenReason::InsufficientEvidence);
    cfg.score.guard_returns = false;
    std::fill(train.member.begin(), train.member.end(), 0);
    auto no_members = mine::mine_train(lib, train, inputs, cfg, reg);
    ASSERT_TRUE(no_members);
    EXPECT_FALSE(no_members->candidates[0].ic_rejected);
}

} // namespace atx_test_l9_realmine_pipeline
