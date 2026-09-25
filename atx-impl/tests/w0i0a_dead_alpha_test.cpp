// W0-I0a — "dead" means Dead/Decaying AS OF EACH STEP (I-06).
//
// Suite ImplDeadAlpha_*:
//   * DeadSetFollowsTheLifecycleAsOfEachDate — dead_set_at maps each panel date onto the
//     library's period axis (recorded holdout) and returns exactly the alphas that are
//     Decaying or Dead as of that date; the pre-W0 V1 set is the admitted/live pool read
//     at the library's last period, for every date.
//   * UnrecordedAxisIsFailOpen — a library without a recorded holdout has no V2 dead set.
//   * CrowdingStartsWhenTheAlphasDie — through run_optimize: books before the alphas' death
//     date are byte-identical to the crowding-off run; books after it are de-levered on the
//     crowded name. Under V1 even the first book is already de-levered (look-ahead).

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/library/lifecycle.hpp"
#include "atx/engine/library/record.hpp"

#include "config.hpp"
#include "dead_alpha_wire.hpp"
#include "diag_risk.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"
#include "w0i0a_fixtures.hpp"

namespace atx_test_w0_i0a_dead_alpha {

namespace fx = atx_test_w0_i0a_fixtures;
namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
namespace lib = atx::engine::library;
namespace risk = atx::engine::risk;
using atx::f64;
using atx::usize;
using LS = lib::LifecycleState;

[[nodiscard]] lib::GateConfig permissive_gate() {
    lib::GateConfig g;
    g.min_sharpe = -1e9;
    g.min_fitness = -1e9;
    g.max_turnover = 1e9;
    g.max_pool_corr = 1.1;
    return g;
}

[[nodiscard]] atx::engine::combine::AlphaMetrics metrics() {
    atx::engine::combine::AlphaMetrics m{};
    m.sharpe = 5.0;
    m.turnover = 0.05;
    m.returns = 1.0;
    m.drawdown = 0.1;
    m.margin = 10.0;
    m.fitness = 5.0;
    m.holding_days = 20.0;
    return m;
}

// n alphas over kT library periods, every one holding a crowded book centred on `center`
// at every period; ids returned in admit order.
[[nodiscard]] std::vector<lib::AlphaId> seed(lib::Library& library, usize n, usize kT, usize m,
                                             usize center) {
    const atx::engine::combine::AlphaGate gate{permissive_gate()};
    std::vector<lib::AlphaId> ids;
    for (usize k = 0; k < n; ++k) {
        std::vector<f64> pnl(kT, 0.0);
        std::vector<f64> pos(kT * m, 0.0);
        for (usize t = 1; t < kT; ++t) {
            pnl[t] = 0.01 + 0.0001 * static_cast<f64>(k + t);
            for (usize i = 0; i < m; ++i) {
                const f64 d =
                    (static_cast<f64>(i) - static_cast<f64>(center)) / static_cast<f64>(m);
                pos[t * m + i] = std::cos(std::numbers::pi * d);
            }
        }
        const lib::AlphaCandidate cand{0x7000ULL + k, pnl, pos, metrics(),
                                       lib::Provenance{"dead", std::vector<atx::u64>{}, 0, 50 + k},
                                       0U, nullptr};
        const auto v = library.admit(cand, gate);
        EXPECT_EQ(v.kind, lib::AdmitKind::Accept);
        ids.push_back(v.id);
    }
    return ids;
}

void record_holdout(const fs::path& dir, usize begin, usize end, usize n_dates) {
    auto r = atx::impl::make_split_range(atx::impl::SplitRole::DiscoverHoldout, begin, end,
                                         n_dates, {});
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(atx::impl::append_split_range(dir.string(), *r).has_value());
}

[[nodiscard]] std::string ids_str(const std::vector<lib::AlphaId>& ids) {
    std::string s = "{";
    for (usize k = 0; k < ids.size(); ++k) {
        s += (k > 0 ? "," : "") + std::to_string(ids[k].value);
    }
    return s + "}";
}

TEST(ImplDeadAlpha, DeadSetFollowsTheLifecycleAsOfEachDate) {
    const fs::path dir = fx::fresh_dir("dead_unit");
    constexpr usize kT = 10;
    std::vector<lib::AlphaId> ids;
    {
        lib::Library library = lib::Library::open(dir.string(), permissive_gate(), {9ULL});
        ids = seed(library, 3, kT, 6, 2);
        // alpha 0: Live@2 -> Decaying@5; alpha 1: Live@1 -> Decaying@3 -> Dead@7;
        // alpha 2: Live@1 and stays live.
        ASSERT_TRUE(library.mark(ids[0], LS::Live, 2).has_value());
        ASSERT_TRUE(library.mark(ids[0], LS::Decaying, 5).has_value());
        ASSERT_TRUE(library.mark(ids[1], LS::Live, 1).has_value());
        ASSERT_TRUE(library.mark(ids[1], LS::Decaying, 3).has_value());
        ASSERT_TRUE(library.mark(ids[1], LS::Dead, 7).has_value());
        ASSERT_TRUE(library.mark(ids[2], LS::Live, 1).has_value());
        ASSERT_TRUE(library.flush_all().has_value());
    }
    record_holdout(dir, 20, 30, 60); // library period p == panel date 20 + p
    lib::Library library =
        lib::Library::open(dir.string(), atx::engine::combine::GateConfig{}, {9ULL});
    const atx::impl::LibraryPeriodAxis axis =
        atx::impl::library_period_axis(dir.string(), library);
    ASSERT_TRUE(axis.known);
    EXPECT_EQ(axis.holdout_begin, 20U);
    EXPECT_EQ(axis.n_periods, kT);
    struct Row {
        usize date;
        std::vector<atx::u32> want;
    };
    const Row rows[] = {{0, {}},     {19, {}},     {22, {}},    {23, {1}},
                        {25, {0, 1}}, {27, {0, 1}}, {45, {0, 1}}};
    for (const Row& r : rows) {
        const atx::impl::DeadSet ds = atx::impl::dead_set_at(
            library, axis, atx::impl::DeadAlphaRule::DeadOrDecayingPerStepV2, r.date);
        std::vector<atx::u32> got;
        for (const auto id : ds.ids) got.push_back(id.value);
        EXPECT_EQ(got, r.want) << "date " << r.date;
        const atx::impl::DeadSet v1 = atx::impl::dead_set_at(
            library, axis, atx::impl::DeadAlphaRule::AdmittedAsOfLastPeriodV1, r.date);
        std::printf("[W0-I0a I-06] date %2zu: V2 dead=%s (as_of %zu)  V1 'dead'=%s (as_of %zu)\n",
                    r.date, ids_str(ds.ids).c_str(), ds.as_of, ids_str(v1.ids).c_str(), v1.as_of);
        EXPECT_EQ(v1.ids.size(), 3U) << "V1 = every admitted alpha, incl. the live one";
        EXPECT_EQ(v1.as_of, kT - 1U) << "V1 reads the library's last period for every date";
    }
}

TEST(ImplDeadAlpha, UnrecordedAxisIsFailOpen) {
    const fs::path dir = fx::fresh_dir("dead_unrecorded");
    {
        lib::Library library = lib::Library::open(dir.string(), permissive_gate(), {9ULL});
        const auto ids = seed(library, 2, 4, 6, 2);
        for (const auto id : ids) {
            ASSERT_TRUE(library.mark(id, LS::Live, 0).has_value());
            ASSERT_TRUE(library.mark(id, LS::Decaying, 0).has_value());
        }
        ASSERT_TRUE(library.flush_all().has_value());
    }
    lib::Library library =
        lib::Library::open(dir.string(), atx::engine::combine::GateConfig{}, {9ULL});
    const atx::impl::LibraryPeriodAxis axis =
        atx::impl::library_period_axis(dir.string(), library);
    EXPECT_FALSE(axis.known);
    const auto ds = atx::impl::dead_set_at(library, axis,
                                           atx::impl::DeadAlphaRule::DeadOrDecayingPerStepV2, 50);
    EXPECT_TRUE(ds.ids.empty()) << "no recorded period axis -> no PIT mapping -> no dead set";
}

// A gently trending research panel and a constant long-half / short-half combo (the same
// shapes stage_optimize_dead_alpha_wire_test.cpp uses).
void write_research_and_combo(const fs::path& dir, usize m, usize d) {
    std::vector<f64> close;
    std::vector<f64> a;
    for (usize t = 0; t < d; ++t) {
        for (usize i = 0; i < m; ++i) {
            close.push_back(100.0 * std::exp(0.0002 * (1.0 + 0.1 * static_cast<f64>(i)) *
                                             static_cast<f64>(t)) *
                            (1.0 + 0.002 * std::sin(0.9 * static_cast<f64>(t + i))));
            a.push_back(i < m / 2 ? 1.0 : -1.0);
        }
    }
    const std::vector<std::uint8_t> uni(d * m, 1U);
    auto r = alpha::Panel::create(d, m, {"close"}, {close}, uni);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(atx::impl::write_panel(*r, (dir / "research.bin").string()).has_value());
    auto c = alpha::Panel::create(d, m, {"alpha"}, {a}, uni);
    ASSERT_TRUE(c.has_value());
    ASSERT_TRUE(atx::impl::write_panel(*c, (dir / "combo.bin").string()).has_value());
}

TEST(ImplDeadAlpha, CrowdingStartsWhenTheAlphasDie) {
    constexpr usize M = 10;
    constexpr usize D = 40;
    constexpr usize kCenter = 3;
    const fs::path dir = fx::fresh_dir("dead_optimize");
    write_research_and_combo(dir, M, D);
    const fs::path lib_dir = dir / "lib";
    fs::create_directories(lib_dir);
    {
        lib::Library library = lib::Library::open(lib_dir.string(), permissive_gate(), {777ULL});
        const auto ids = seed(library, 3, 2, M, kCenter);
        for (const auto id : ids) { // retire all three at library period 1
            ASSERT_TRUE(library.mark(id, LS::Live, 1).has_value());
            ASSERT_TRUE(library.mark(id, LS::Decaying, 1).has_value());
            ASSERT_TRUE(library.mark(id, LS::Dead, 1).has_value());
        }
        ASSERT_TRUE(library.flush_all().has_value());
    }
    record_holdout(lib_dir, 10, 12, D); // period 1 == panel date 11: dead from date 11 on

    atx::impl::RunConfig cfg;
    cfg.allow_unidentified_panels = true; // synthetic legacy fixture: explicit diagnostic mode
    cfg.panel = (dir / "research.bin").string();
    cfg.combo = (dir / "combo.bin").string();
    cfg.gross = 1.0;
    cfg.name_cap = 1.0;
    cfg.rebalance = "weekly"; // steps at dates 0, 5, 10, 15, ..., 35
    cfg.risk_aversion = 1.0;
    cfg.set_flags.emplace("risk-aversion");
    cfg.dead_alpha_lib_dir = lib_dir.string();
    const auto run = [&](bool on, const atx::impl::DeployPitConfig& pit, const std::string& tag,
                         atx::impl::StageResult* sr) {
        atx::impl::RunConfig c = cfg;
        c.dead_alpha_factors = on;
        c.books_out = (dir / tag).string();
        risk::RiskModelConfig rc{};
        rc.dead_alpha_factors = on;
        auto r = atx::impl::run_optimize(c, rc, pit);
        EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
        if (r.has_value() && sr != nullptr) *sr = *r;
        return fx::book_rows(c.books_out);
    };
    atx::impl::StageResult on_sr;
    const auto off = run(false, {}, "off.bin", nullptr);
    const auto on = run(true, {}, "on.bin", &on_sr);
    atx::impl::DeployPitConfig v1;
    v1.dead = atx::impl::DeadAlphaRule::AdmittedAsOfLastPeriodV1;
    const auto on_v1 = run(true, v1, "on_v1.bin", nullptr);
    ASSERT_EQ(off.size(), 8U);
    ASSERT_EQ(on.size(), 8U);
    ASSERT_EQ(on_v1.size(), 8U);
    EXPECT_EQ(fx::find_kv(on_sr, "dead_alpha_rule"), "dead_or_decaying_per_step_v2");
    EXPECT_EQ(fx::find_kv(on_sr, "dead_alpha_axis"), "recorded");
    EXPECT_EQ(fx::find_kv(on_sr, "dead_alpha_steps"), "5") << "steps at dates 15..35";
    for (usize s = 0; s < off.size(); ++s) {
        const bool dead_by_now = s * 5U >= 11U;
        const bool same = std::bit_cast<std::uint64_t>(off[s][kCenter]) ==
                          std::bit_cast<std::uint64_t>(on[s][kCenter]);
        std::printf("[W0-I0a I-06] step %zu (date %2zu): |w_center| off=%.6f V2=%.6f V1=%.6f\n", s,
                    s * 5U, std::fabs(off[s][kCenter]), std::fabs(on[s][kCenter]),
                    std::fabs(on_v1[s][kCenter]));
        if (dead_by_now) {
            EXPECT_LT(std::fabs(on[s][kCenter]), std::fabs(off[s][kCenter]))
                << "after the alphas die their crowded name is de-levered";
        } else {
            EXPECT_TRUE(same) << "before the alphas die nothing is crowding-adjusted";
        }
    }
    EXPECT_LT(std::fabs(on_v1[0][kCenter]), std::fabs(off[0][kCenter]))
        << "V1 already de-levers the first book, before any alpha died (look-ahead)";
}

} // namespace atx_test_w0_i0a_dead_alpha
