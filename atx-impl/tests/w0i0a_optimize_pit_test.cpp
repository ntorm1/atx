// W0-I0a — the optimize stage is point-in-time at every rebalance (I-04, R-12).
//
// Suite ImplOptimizePit_*:
//   * ExpandingDiagonalMatchesTwoPass — diagonal_risk_models_expanding (one Welford pass)
//     equals diagonal_risk_model(research, fit_end) per step to 1e-12 relative.
//   * FutureMutationLeavesPastBooksIdentical — the plan's acceptance item "mutating future
//     volumes or prices leaves past books byte-identical": prices AND volumes on every date
//     >= kMutate are replaced; every book whose rebalance date is < kMutate is bit-identical,
//     for the default MVO (per-step diagonal lens), MVO + participation cap (trailing ADV
//     per rebalance), position mode + Garleanu-Pedersen (per-step lens) and the Factor
//     model. The *V1 rules move those past books (the test has teeth).
//   * ParticipationReferenceIsTrailingPit — R-12: the per-rebalance ADV/price reference at
//     date d is bit-identical under any mutation of rows > d; a delisted name has a real
//     reference while it trades and cap 0 afterwards, where the V1 last-date reference
//     gave it nothing for its whole history.
//   * PerStepLoopMirrorsTheDriver — the per-step optimizer loop reproduces
//     risk::MultiPeriodOptimizer::run bit-for-bit when the reference does not move.
//   * DelistedNameKeepsItsCapWhileListed — R-12 end to end: the delisted name carries
//     weight while listed and none afterwards.
//   * BindingParticipationCapIsPitPerRebalance — R-12 end to end with the cap BINDING (a
//     NAV at which the %ADV box is below name_cap for the illiquid names) and the diagonal
//     lens held at PerStepPitV2: past books are identical under future mutation with the
//     per-rebalance reference and move with the LastDateV1 reference alone.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "config.hpp"
#include "diag_risk.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"
#include "w0i0a_fixtures.hpp"

namespace atx_test_w0_i0a_optimize_pit {

namespace fx = atx_test_w0_i0a_fixtures;
namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace risk = atx::engine::risk;
using atx::f64;
using atx::usize;
using atx::impl::DeployPitConfig;

constexpr usize kDates = 120;
constexpr usize kInsts = 12;
constexpr usize kMutate = 70; // rows >= 70 mutated; weekly steps 0,5,...,65 must not move

// A combo "alpha" panel independent of the research prices (a deterministic sine), NaN
// wherever the research name is out of universe (as stage_combine writes it).
[[nodiscard]] std::string make_combo(const fs::path& path, const fx::PanelSpec& spec) {
    const fx::PanelColumns cols = fx::make_columns(spec);
    std::vector<f64> a(kDates * kInsts);
    for (usize t = 0; t < kDates; ++t) {
        for (usize i = 0; i < kInsts; ++i) {
            const f64 s = std::sin(0.2 * static_cast<f64>(t) + 0.7 * static_cast<f64>(i));
            a[t * kInsts + i] =
                cols.uni[t * kInsts + i] != 0U ? s : std::numeric_limits<f64>::quiet_NaN();
        }
    }
    auto p = alpha::Panel::create(kDates, kInsts, {"alpha"}, {a}, cols.uni);
    EXPECT_TRUE(p.has_value());
    auto w = atx::impl::write_panel(*p, path.string());
    EXPECT_TRUE(w.has_value());
    return path.string();
}

struct Pair {
    fs::path dir;
    std::string base;
    std::string mut;
    std::string combo;
};

[[nodiscard]] Pair make_pair(const std::string& tag, fx::PanelSpec spec) {
    Pair p;
    p.dir = fx::fresh_dir("opit_" + tag);
    spec.dates = kDates;
    spec.insts = kInsts;
    auto b = fx::write_research_panel(p.dir / "base.bin", spec);
    EXPECT_TRUE(b.has_value());
    p.combo = make_combo(p.dir / "combo.bin", spec);
    spec.mutate_from = kMutate;
    auto m = fx::write_research_panel(p.dir / "mut.bin", spec);
    EXPECT_TRUE(m.has_value());
    p.base = (p.dir / "base.bin").string();
    p.mut = (p.dir / "mut.bin").string();
    return p;
}

[[nodiscard]] atx::impl::RunConfig opt_cfg(const Pair& p) {
    atx::impl::RunConfig cfg;
    cfg.allow_unidentified_panels = true; // synthetic legacy fixture: explicit diagnostic mode
    cfg.combo = p.combo;
    cfg.gross = 1.0;
    cfg.name_cap = 0.5;
    cfg.rebalance = "weekly";
    cfg.risk_aversion = 1.0;
    cfg.set_flags.emplace("risk-aversion");
    return cfg;
}

[[nodiscard]] std::vector<std::vector<f64>> books(atx::impl::RunConfig cfg,
                                                  const std::string& panel, const fs::path& out,
                                                  const risk::RiskModelConfig& rc,
                                                  const DeployPitConfig& pit) {
    cfg.panel = panel;
    cfg.books_out = out.string();
    auto r = atx::impl::run_optimize(cfg, rc, pit);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    return r.has_value() ? fx::book_rows(cfg.books_out) : std::vector<std::vector<f64>>{};
}

// Number of leading steps (rebalance date < kMutate) whose books are bit-identical.
[[nodiscard]] usize identical_past_steps(const std::vector<std::vector<f64>>& a,
                                         const std::vector<std::vector<f64>>& b) {
    usize same = 0;
    for (usize s = 0; s < a.size() && s < b.size() && s * 5U < kMutate; ++s) {
        bool eq = a[s].size() == b[s].size();
        for (usize i = 0; eq && i < a[s].size(); ++i) {
            eq = std::bit_cast<std::uint64_t>(a[s][i]) == std::bit_cast<std::uint64_t>(b[s][i]);
        }
        same += eq ? 1U : 0U;
    }
    return same;
}

constexpr usize kPastSteps = (kMutate + 4U) / 5U; // steps 0..13 have dates 0..65
// A participation cap the augmented QP converges on for this fixture (tighter caps hit the
// ConstrainedQpSolver iteration budget with EITHER reference rule -- a pre-existing solver
// limit, recorded in the lane report).
constexpr f64 kPartCap = 0.05;
constexpr f64 kPartNav = 3.0e5;

TEST(ImplOptimizePit, ExpandingDiagonalMatchesTwoPass) {
    const fs::path dir = fx::fresh_dir("opit_diag");
    fx::PanelSpec spec;
    spec.dates = kDates;
    spec.insts = kInsts;
    spec.delist_inst = 3;
    spec.delist_from = 50;
    ASSERT_TRUE(fx::write_research_panel(dir / "r.bin", spec).has_value());
    auto research = atx::impl::read_panel((dir / "r.bin").string());
    ASSERT_TRUE(research.has_value());
    const std::vector<usize> fit_ends{1, 2, 3, 20, 51, 90, kDates};
    auto fast = atx::impl::diagonal_risk_models_expanding(*research, fit_ends);
    ASSERT_TRUE(fast.has_value()) << fast.error().message();
    f64 max_rel = 0.0;
    for (usize k = 0; k < fit_ends.size(); ++k) {
        auto slow = atx::impl::diagonal_risk_model(*research, fit_ends[k]);
        ASSERT_TRUE(slow.has_value());
        EXPECT_EQ((*fast)[k].fit_end(), fit_ends[k]);
        for (usize i = 0; i < kInsts; ++i) {
            const f64 a = (*fast)[k].specific_var()[static_cast<Eigen::Index>(i)];
            const f64 b = slow->specific_var()[static_cast<Eigen::Index>(i)];
            max_rel = std::max(max_rel, std::fabs(a - b) / b);
        }
    }
    std::printf("[W0-I0a I-04] expanding vs two-pass diagonal: max relative diff = %.3g\n",
                max_rel);
    EXPECT_LT(max_rel, 1e-12);
    const std::vector<usize> bad{5, 4};
    EXPECT_FALSE(atx::impl::diagonal_risk_models_expanding(*research, bad).has_value());
}

TEST(ImplOptimizePit, FutureMutationLeavesPastBooksIdentical) {
    const Pair p = make_pair("mut", fx::PanelSpec{});
    struct Case {
        const char* name;
        bool participation;
        bool position_gp;
        bool factor;
    };
    const Case cases[] = {{"mvo-diagonal", false, false, false},
                          {"mvo-participation-cap", true, false, false},
                          {"position-mode-gp", false, true, false},
                          {"mvo-factor", false, false, true}};
    usize n = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        atx::impl::RunConfig cfg = opt_cfg(p);
        if (c.participation) {
            // A cap the augmented QP converges on for this fixture (see
            // ParticipationReferenceIsTrailingPit for the reference itself).
            cfg.participation_cap = kPartCap;
            cfg.report_aum = kPartNav;
        }
        if (c.position_gp) {
            cfg.position_mode = true;
            cfg.gp_trading = true;
            cfg.gp_risk_aversion = 1.0; // lambda > 0: the aim portfolio reads the risk lens
        }
        risk::RiskModelConfig rc{};
        if (c.factor) {
            rc.kind = risk::RiskModelKind::Factor;
        }
        const std::string tag = std::to_string(n++);
        const auto b2 = books(cfg, p.base, p.dir / ("b2_" + tag), rc, {});
        const auto m2 = books(cfg, p.mut, p.dir / ("m2_" + tag), rc, {});
        ASSERT_EQ(b2.size(), kDates / 5U);
        const usize same_v2 = identical_past_steps(b2, m2);
        EXPECT_EQ(same_v2, kPastSteps) << "V2: every book dated before the mutation is identical";
        usize later_diff = 0;
        for (usize s = kPastSteps; s < b2.size(); ++s) {
            later_diff += (b2[s] != m2[s]) ? 1U : 0U;
        }
        EXPECT_GT(later_diff, 0U) << "the mutation must reach the later books";

        DeployPitConfig v1;
        v1.diag = atx::impl::DiagRiskRule::WholePanelV1;
        v1.participation = atx::impl::ParticipationAdvRule::LastDateV1;
        const auto b1 = books(cfg, p.base, p.dir / ("b1_" + tag), rc, v1);
        const auto m1 = books(cfg, p.mut, p.dir / ("m1_" + tag), rc, v1);
        const usize same_v1 = identical_past_steps(b1, m1);
        std::printf("[W0-I0a I-04/R-12] %-22s past steps identical under future mutation: "
                    "V2 %zu/%zu, V1 %zu/%zu\n",
                    c.name, same_v2, kPastSteps, same_v1, kPastSteps);
        if (!c.factor) {
            EXPECT_LT(same_v1, kPastSteps) << "the V1 lens/reference reads the future";
        }
    }
}

TEST(ImplOptimizePit, ParticipationReferenceIsTrailingPit) {
    fx::PanelSpec spec;
    spec.dates = kDates;
    spec.insts = kInsts;
    spec.delist_inst = 11; // the most liquid name, delisted at date 60
    spec.delist_from = 60;
    const fx::PanelColumns base = fx::make_columns(spec);
    spec.mutate_from = kMutate;
    const fx::PanelColumns mut = fx::make_columns(spec);
    std::vector<f64> adv_b(kInsts), px_b(kInsts), adv_m(kInsts), px_m(kInsts);
    usize identical = 0;
    for (usize d = 0; d < kMutate; ++d) {
        atx::impl::trailing_participation_reference(base.volume, base.close, kInsts, d, adv_b,
                                                    px_b);
        atx::impl::trailing_participation_reference(mut.volume, mut.close, kInsts, d, adv_m,
                                                    px_m);
        bool eq = true;
        for (usize i = 0; i < kInsts; ++i) {
            const auto bits = [](f64 x) { return std::bit_cast<std::uint64_t>(x); };
            eq = eq && bits(adv_b[i]) == bits(adv_m[i]) && bits(px_b[i]) == bits(px_m[i]);
        }
        identical += eq ? 1U : 0U;
    }
    EXPECT_EQ(identical, kMutate) << "the reference at d reads rows <= d only";
    // The delisted name: a real ADV and price while it trades, cap 0 (price 0) afterwards.
    atx::impl::trailing_participation_reference(base.volume, base.close, kInsts, 59, adv_b, px_b);
    const f64 adv_listed = adv_b[11];
    const f64 px_listed = px_b[11];
    atx::impl::trailing_participation_reference(base.volume, base.close, kInsts, 60, adv_b, px_b);
    EXPECT_GT(adv_listed, 0.0);
    EXPECT_GT(px_listed, 0.0);
    EXPECT_EQ(px_b[11], 0.0) << "no price on the delisting date -> no participation";
    // V1 used the panel's LAST date for every rebalance: for the delisted name that is a
    // NaN price and an empty volume window, i.e. no capacity over its whole history.
    const usize last = kDates - 1U;
    atx::impl::trailing_participation_reference(base.volume, base.close, kInsts, last, adv_b, px_b);
    std::printf("[W0-I0a R-12] delisted name 11: V2 reference at d=59 adv=%.0f px=%.2f; "
                "V1 (last-date) reference adv=%.0f px=%.2f for every rebalance\n",
                adv_listed, px_listed, adv_b[11], px_b[11]);
    EXPECT_EQ(adv_b[11], 0.0);
    EXPECT_EQ(px_b[11], 0.0);
}

TEST(ImplOptimizePit, PerStepLoopMirrorsTheDriver) {
    // With constant closes and volumes every per-step reference equals the last-date one,
    // so the V2 per-step loop must reproduce risk::MultiPeriodOptimizer::run bit-for-bit.
    const fs::path dir = fx::fresh_dir("opit_mirror");
    std::vector<f64> close(kDates * kInsts);
    std::vector<f64> volume(kDates * kInsts);
    for (usize t = 0; t < kDates; ++t) {
        for (usize i = 0; i < kInsts; ++i) {
            close[t * kInsts + i] = 50.0 + 5.0 * static_cast<f64>(i);
            volume[t * kInsts + i] = 1.0e5 * (1.0 + static_cast<f64>(i));
        }
    }
    const std::vector<std::uint8_t> uni(kDates * kInsts, 1U);
    auto r = alpha::Panel::create(kDates, kInsts, {"close", "volume"}, {close, volume}, uni);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(atx::impl::write_panel(*r, (dir / "r.bin").string()).has_value());
    Pair p;
    p.dir = dir;
    fx::PanelSpec combo_spec;
    combo_spec.dates = kDates;
    combo_spec.insts = kInsts;
    p.combo = make_combo(dir / "c.bin", combo_spec);
    atx::impl::RunConfig cfg = opt_cfg(p);
    cfg.panel = (dir / "r.bin").string();
    cfg.participation_cap = kPartCap;
    cfg.report_aum = kPartNav;
    const risk::RiskModelConfig rc{};
    DeployPitConfig v1;
    v1.participation = atx::impl::ParticipationAdvRule::LastDateV1;
    cfg.books_out = (dir / "v2").string();
    auto a = atx::impl::run_optimize(cfg, rc, {});
    cfg.books_out = (dir / "v1").string();
    auto b = atx::impl::run_optimize(cfg, rc, v1);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    ASSERT_TRUE(b.has_value()) << b.error().message();
    EXPECT_EQ(a->digest, b->digest);
    EXPECT_EQ(fx::find_kv(*a, "participation_adv_ref"), "trailing_pit_per_rebalance_v2");
    EXPECT_EQ(fx::find_kv(*b, "participation_adv_ref"), "last_date_v1");
}

TEST(ImplOptimizePit, DelistedNameKeepsItsCapWhileListed) {
    fx::PanelSpec spec;
    spec.delist_inst = 11; // the most liquid name
    spec.delist_from = 60;
    const Pair p = make_pair("delist", spec);
    atx::impl::RunConfig cfg = opt_cfg(p);
    cfg.participation_cap = kPartCap;
    cfg.report_aum = kPartNav;
    const risk::RiskModelConfig rc{};
    const auto v2 = books(cfg, p.base, p.dir / "v2", rc, {});
    ASSERT_EQ(v2.size(), kDates / 5U);
    f64 listed_gross = 0.0;
    f64 delisted_gross = 0.0;
    for (usize s = 0; s < v2.size(); ++s) {
        (s * 5U < 60U ? listed_gross : delisted_gross) += std::fabs(v2[s][11]);
    }
    EXPECT_GT(listed_gross, 0.0) << "a listed name must be tradable under its trailing ADV";
    EXPECT_LT(delisted_gross, 1e-6) << "a delisted name has no price, so cap 0 (ADMM tol 1e-6)";

    DeployPitConfig v1;
    v1.participation = atx::impl::ParticipationAdvRule::LastDateV1;
    atx::impl::RunConfig cfg1 = cfg;
    cfg1.panel = p.base;
    cfg1.books_out = (p.dir / "v1").string();
    auto r1 = atx::impl::run_optimize(cfg1, rc, v1);
    // Measured, not asserted: on this fixture the V1 run (NaN last-date price for this name)
    // gives it the same weight as V2 while listed -- its cap does not bind here -- so the V1
    // defect shows as the look-ahead itself (ParticipationReferenceIsTrailingPit).
    f64 v1_listed = 0.0;
    if (r1.has_value()) {
        const auto b1 = fx::book_rows(cfg1.books_out);
        for (usize s = 0; s < b1.size() && s * 5U < 60U; ++s) {
            v1_listed += std::fabs(b1[s][11]);
        }
    }
    const std::string v1_str = r1.has_value() ? std::to_string(v1_listed)
                                              : "run failed: " + r1.error().message().substr(0, 60);
    std::printf("[W0-I0a R-12] delisted name 11: sum|w| while listed V2=%.6f V1=%s; "
                "after delisting V2=%.3g\n",
                listed_gross, v1_str.c_str(), delisted_gross);
}

TEST(ImplOptimizePit, BindingParticipationCapIsPitPerRebalance) {
    // At NAV 2e6 the box 0.05 * ADV * px / NAV is ~0.125 for the least liquid name and rises
    // along the 30x liquidity ladder, so it sits below name_cap (0.5) on the illiquid names
    // and below their uncapped weights (up to ~0.2). (The augmented ConstrainedQpSolver does
    // not converge at every binding NAV -- e.g. 5e5, 1e6, 3e6 hit its fixed iteration budget
    // under either reference rule, a pre-existing solver limit -- 2e6 converges for all four
    // runs below.)
    constexpr f64 kBindNav = 2.0e6;
    fx::PanelSpec spec;
    const Pair p = make_pair("bind", spec);
    spec.dates = kDates;
    spec.insts = kInsts;
    const fx::PanelColumns cols = fx::make_columns(spec); // == the base panel's columns
    atx::impl::RunConfig cfg = opt_cfg(p);
    const risk::RiskModelConfig rc{};
    cfg.participation_cap = kPartCap;
    cfg.report_aum = kPartNav; // box >= 0.83 > name_cap: the same augmented QP, cap slack
    const auto slack = books(cfg, p.base, p.dir / "slack", rc, {});
    cfg.report_aum = kBindNav;
    const auto b2 = books(cfg, p.base, p.dir / "b2", rc, {});
    const auto m2 = books(cfg, p.mut, p.dir / "m2", rc, {});
    DeployPitConfig last_date; // diagonal lens stays PerStepPitV2: only the reference moves
    last_date.participation = atx::impl::ParticipationAdvRule::LastDateV1;
    const auto b1 = books(cfg, p.base, p.dir / "b1", rc, last_date);
    const auto m1 = books(cfg, p.mut, p.dir / "m1", rc, last_date);
    ASSERT_EQ(slack.size(), kDates / 5U);
    ASSERT_EQ(b2.size(), slack.size());
    ASSERT_EQ(b1.size(), slack.size());

    // The cap binds: past V2 books sit ON their per-rebalance box for some names, and differ
    // from the books of the same QP with a slack cap.
    std::vector<f64> adv(kInsts);
    std::vector<f64> px(kInsts);
    usize bound_cells = 0;
    usize capped_steps = 0;
    for (usize s = 0; s < kPastSteps; ++s) {
        atx::impl::trailing_participation_reference(cols.volume, cols.close, kInsts, s * 5U,
                                                    adv, px);
        for (usize i = 0; i < kInsts; ++i) {
            const f64 box = kPartCap * adv[i] * px[i] / kBindNav;
            if (box < cfg.name_cap && std::fabs(b2[s][i]) >= box * (1.0 - 1e-3)) {
                ++bound_cells;
            }
        }
        capped_steps += (b2[s] != slack[s]) ? 1U : 0U;
    }
    const usize same_v2 = identical_past_steps(b2, m2);
    const usize same_last_date = identical_past_steps(b1, m1);
    std::printf("[W0-I0a R-12] binding cap (NAV %.0e): %zu past (step,name) cells on their "
                "%%ADV box, %zu/%zu past steps differ from the slack-cap run; past books identical under "
                "future mutation: TrailingPitPerRebalanceV2 %zu/%zu, LastDateV1 %zu/%zu "
                "(diag PerStepPitV2 in both)\n",
                kBindNav, bound_cells, capped_steps, kPastSteps, same_v2, kPastSteps,
                same_last_date, kPastSteps);
    EXPECT_GT(bound_cells, 0U) << "the %ADV box must bind on this fixture";
    EXPECT_GT(capped_steps, 0U) << "the binding cap must change the past books";
    EXPECT_EQ(same_v2, kPastSteps) << "the per-rebalance reference reads rows <= d only";
    EXPECT_LT(same_last_date, kPastSteps)
        << "the last-date reference alone carries the future into past books";
}

} // namespace atx_test_w0_i0a_optimize_pit
