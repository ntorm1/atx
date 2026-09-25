// W0-I0a — nested splits: discover train < combine fit < final test (I-01).
//
// Suite ImplNestedSplits_*:
//   * GeometryIsOrderedAndEmbargoed — resolve_nested_split carves three consecutive windows
//     with an h+delay gap between them, and rejects impossible fractions.
//   * LegacySameWindowIsRefused — the pre-W0 wiring (discover admits on the last 25%,
//     combine then calls the same 25% "OOS") is refused by the library split-range ledger;
//     HoldoutGuardRule::NoCheckV1 still reproduces it.
//   * PipelineWindowsAreDisjointAndRecorded — the run_all wiring: discover on the prefix,
//     combine on the fresh fit window, final test after the embargo; the ledger records
//     all three ranges and they do not overlap.
//   * DiscoverRefusesRecordedFinalTest — a later discover run cannot read a recorded test.
//   * ShiftedHoldoutRefused — a library's holdout (its period axis) cannot be re-anchored.
//   * DiscoverWindowIgnoresLaterRows — discover on the prefix is byte-identical whatever
//     the panel holds after discover_end.
//   * FinalTestMutationLeavesShippedWeightsIdentical — the whole nested discover+combine
//     chain ships byte-identical weights when every date of the final test is mutated.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "dead_alpha_wire.hpp"
#include "stage_combine.hpp"
#include "stages.hpp"
#include "w0i0a_fixtures.hpp"

namespace atx_test_w0_i0a_nested_splits {

namespace fx = atx_test_w0_i0a_fixtures;
namespace fs = std::filesystem;
using atx::usize;
using atx::impl::SplitRange;
using atx::impl::SplitRole;

constexpr usize kDates = 200;

[[nodiscard]] std::string make_panel(const fs::path& dir, usize mutate_from = fx::kNever) {
    fx::PanelSpec spec;
    spec.dates = kDates;
    spec.mutate_from = mutate_from;
    auto r = fx::write_research_panel(dir / (mutate_from == fx::kNever ? "base.bin" : "mut.bin"),
                                      spec);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    return r.has_value() ? *r : std::string{};
}

[[nodiscard]] atx::impl::RunConfig discover_cfg(const std::string& panel, const fs::path& work) {
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.panel = panel;
    cfg.alpha_out = (work / "alphas").string();
    cfg.gated = true;
    cfg.library_dir = (work / "_library").string();
    return cfg;
}

[[nodiscard]] atx::impl::RunConfig combine_cfg(const atx::impl::RunConfig& disc,
                                               const fs::path& work) {
    atx::impl::RunConfig cfg = fx::permissive_cfg();
    cfg.panel = disc.panel;
    cfg.alphas = disc.alpha_out;
    cfg.library_dir = disc.library_dir;
    cfg.combo_out = (work / "combo.bin").string();
    cfg.method = "shrinkage-mv";
    return cfg;
}

[[nodiscard]] atx::impl::NestedSplit default_split() {
    auto s = atx::impl::resolve_nested_split(kDates, atx::impl::NestedSplitConfig{});
    EXPECT_TRUE(s.has_value());
    return s.has_value() ? *s : atx::impl::NestedSplit{};
}

// Discover on [0, discover_end) + combine on [fit_begin, fit_end) with the final test from
// test_begin: the exact run_all wiring.
[[nodiscard]] atx::core::Result<atx::impl::StageResult>
run_nested(const std::string& panel, const fs::path& work, atx::impl::StageResult* disc_out) {
    const atx::impl::NestedSplit split = default_split();
    const atx::impl::RunConfig dcfg = discover_cfg(panel, work);
    ATX_TRY(auto d, atx::impl::run_discover_window(dcfg, split.discover_end));
    if (disc_out != nullptr) {
        *disc_out = d;
    }
    atx::impl::RunConfig ccfg = combine_cfg(dcfg, work);
    ccfg.fit_begin = static_cast<long>(split.fit_begin);
    ccfg.fit_end = static_cast<long>(split.fit_end);
    ccfg.set_flags.insert("fit-end");
    atx::impl::CombinePitConfig pit;
    pit.test_begin = split.test_begin;
    return atx::impl::run_combine(ccfg, pit);
}

TEST(ImplNestedSplits, GeometryIsOrderedAndEmbargoed) {
    const atx::impl::NestedSplit s = default_split();
    EXPECT_EQ(s.n_dates, 200U);
    EXPECT_EQ(s.test_begin, 150U);  // 200 - floor(0.25 * 200)
    EXPECT_EQ(s.fit_end, 148U);     // test_begin - embargo(2)
    EXPECT_EQ(s.fit_begin, 98U);    // fit_end - floor(0.25 * 200)
    EXPECT_EQ(s.discover_end, 96U); // fit_begin - embargo(2)
    atx::impl::NestedSplitConfig bad;
    bad.test_frac = 0.6;
    bad.combine_frac = 0.5;
    EXPECT_FALSE(atx::impl::resolve_nested_split(200, bad).has_value()) << "fractions sum >= 1";
    bad.test_frac = 0.0;
    bad.combine_frac = 0.25;
    EXPECT_FALSE(atx::impl::resolve_nested_split(200, bad).has_value()) << "empty test";
    EXPECT_FALSE(atx::impl::resolve_nested_split(9, atx::impl::NestedSplitConfig{}).has_value())
        << "9 dates cannot hold three >= 2-date windows plus two embargoes";
    auto tight = atx::impl::resolve_nested_split(10, atx::impl::NestedSplitConfig{});
    ASSERT_TRUE(tight.has_value());
    EXPECT_EQ(tight->discover_end, 2U) << "the smallest legal discover window";
}

TEST(ImplNestedSplits, LegacySameWindowIsRefused) {
    const fs::path work = fx::fresh_dir("ns_legacy");
    const std::string panel = make_panel(work);
    // The pre-W0 run_all wiring: discover on the WHOLE panel (the library's accumulation
    // default holds out the last 25% for admission), then combine with holdout 0.25.
    const atx::impl::RunConfig dcfg = discover_cfg(panel, work);
    auto d = atx::impl::run_discover(dcfg);
    ASSERT_TRUE(d.has_value()) << d.error().message();
    atx::impl::RunConfig ccfg = combine_cfg(dcfg, work);
    ccfg.combine_holdout_frac = 0.25;
    ccfg.set_flags.insert("holdout-frac");
    auto c = atx::impl::run_combine(ccfg);
    ASSERT_FALSE(c.has_value()) << "the final test IS the discover lockbox -- must be refused";
    EXPECT_NE(c.error().message().find("discover_holdout"), std::string::npos)
        << c.error().message();
    std::printf("[W0-I0a I-01] legacy wiring refused: %s\n", c.error().message().c_str());

    atx::impl::CombinePitConfig v1;
    v1.holdout_guard = atx::impl::HoldoutGuardRule::NoCheckV1;
    ccfg.combo_out = (work / "combo_v1.bin").string();
    auto c1 = atx::impl::run_combine(ccfg, v1);
    EXPECT_TRUE(c1.has_value()) << "NoCheckV1 reproduces the pre-W0 (leaky) wiring";
}

TEST(ImplNestedSplits, PipelineWindowsAreDisjointAndRecorded) {
    const fs::path work = fx::fresh_dir("ns_pipeline");
    const std::string panel = make_panel(work);
    atx::impl::StageResult disc;
    auto c = run_nested(panel, work, &disc);
    ASSERT_TRUE(c.has_value()) << c.error().message();
    EXPECT_EQ(fx::find_kv(disc, "discover_end"), "96");
    EXPECT_EQ(fx::find_kv(*c, "fit_begin"), "98");
    EXPECT_EQ(fx::find_kv(*c, "fit_end"), "148");
    EXPECT_EQ(fx::find_kv(*c, "holdout_begin"), "150");
    EXPECT_EQ(fx::find_kv(*c, "final_test_guard"), "checked");
    {
        std::ifstream meta{(work / "combo.bin.meta").string()};
        std::string all((std::istreambuf_iterator<char>(meta)), std::istreambuf_iterator<char>());
        EXPECT_NE(all.find("holdout_begin=150"), std::string::npos) << all;
    }
    auto ranges = atx::impl::read_split_ranges((work / "_library").string());
    ASSERT_TRUE(ranges.has_value()) << ranges.error().message();
    const SplitRange* train = nullptr;
    const SplitRange* holdout = nullptr;
    const SplitRange* test = nullptr;
    for (const SplitRange& r : *ranges) {
        std::printf("[W0-I0a I-01] ledger %s [%zu,%zu) of %zu\n",
                    std::string{atx::impl::split_role_name(r.role)}.c_str(), r.begin, r.end,
                    r.n_dates);
        if (r.role == SplitRole::DiscoverTrain) train = &r;
        if (r.role == SplitRole::DiscoverHoldout) holdout = &r;
        if (r.role == SplitRole::FinalTest) test = &r;
    }
    ASSERT_NE(train, nullptr);
    ASSERT_NE(holdout, nullptr);
    ASSERT_NE(test, nullptr);
    EXPECT_EQ(ranges->size(), 3U);
    // discover (train [0,72) + lockbox [72,96)) < fit [98,148) < test [150,200).
    EXPECT_EQ(train->begin, 0U);
    EXPECT_EQ(train->end, holdout->begin);
    EXPECT_EQ(holdout->begin, 72U);
    EXPECT_EQ(holdout->end, 96U);
    EXPECT_LT(holdout->end, 98U);
    EXPECT_EQ(test->begin, 150U);
    EXPECT_EQ(test->end, 200U);
}

TEST(ImplNestedSplits, DiscoverRefusesRecordedFinalTest) {
    const fs::path work = fx::fresh_dir("ns_refuse_test");
    const std::string panel = make_panel(work);
    auto c = run_nested(panel, work, nullptr);
    ASSERT_TRUE(c.has_value()) << c.error().message();
    // A later discover run into the same library over the whole panel would search and
    // admit on the recorded final test [150, 200).
    atx::impl::RunConfig dcfg = discover_cfg(panel, work);
    dcfg.seed = 7ULL;
    auto d = atx::impl::run_discover(dcfg);
    ASSERT_FALSE(d.has_value());
    EXPECT_NE(d.error().message().find("final test"), std::string::npos) << d.error().message();
    // ... while another seed on the same nested prefix is fine (accumulation continues).
    auto d2 = atx::impl::run_discover_window(dcfg, default_split().discover_end);
    EXPECT_TRUE(d2.has_value()) << (d2 ? "" : d2.error().message());
}

TEST(ImplNestedSplits, ShiftedHoldoutRefused) {
    const fs::path work = fx::fresh_dir("ns_shift");
    const std::string panel = make_panel(work);
    const atx::impl::RunConfig dcfg = discover_cfg(panel, work);
    auto a = atx::impl::run_discover_window(dcfg, 96);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    // floor(0.25*97) == floor(0.25*96) == 24: same holdout length, shifted by one date --
    // Library::try_admit's length check cannot see it; the ledger must.
    auto b = atx::impl::run_discover_window(dcfg, 97);
    ASSERT_FALSE(b.has_value());
    EXPECT_NE(b.error().message().find("differs from the holdout"), std::string::npos)
        << b.error().message();
}

TEST(ImplNestedSplits, DiscoverWindowIgnoresLaterRows) {
    const fs::path work = fx::fresh_dir("ns_prefix");
    const std::string base = make_panel(work);
    const std::string mut = make_panel(work, 96); // every row >= discover_end differs
    const atx::impl::RunConfig ca = discover_cfg(base, work / "a");
    const atx::impl::RunConfig cb = discover_cfg(mut, work / "b");
    auto a = atx::impl::run_discover_window(ca, 96);
    auto b = atx::impl::run_discover_window(cb, 96);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    ASSERT_TRUE(b.has_value()) << b.error().message();
    EXPECT_EQ(a->digest, b->digest);
    EXPECT_EQ(fx::find_kv(*a, "factory_digest"), fx::find_kv(*b, "factory_digest"));
    EXPECT_EQ(fx::find_kv(*a, "admitted"), fx::find_kv(*b, "admitted"));
    std::printf("[W0-I0a I-01] discover prefix [0,96): admitted=%s factory_digest=%s "
                "(identical with rows >= 96 mutated)\n",
                fx::find_kv(*a, "admitted").c_str(), fx::find_kv(*a, "factory_digest").c_str());
    // Whole-panel discover DOES see the mutated rows (the test is not vacuous).
    const atx::impl::RunConfig fa = discover_cfg(base, work / "fa");
    const atx::impl::RunConfig fb = discover_cfg(mut, work / "fb");
    auto wa = atx::impl::run_discover(fa);
    auto wb = atx::impl::run_discover(fb);
    ASSERT_TRUE(wa.has_value() && wb.has_value());
    EXPECT_NE(fx::find_kv(*wa, "factory_digest"), fx::find_kv(*wb, "factory_digest"));
}

TEST(ImplNestedSplits, FinalTestMutationLeavesShippedWeightsIdentical) {
    const fs::path work = fx::fresh_dir("ns_final_mut");
    const std::string base = make_panel(work);
    const std::string mut = make_panel(work, default_split().test_begin);
    atx::impl::StageResult da;
    atx::impl::StageResult db;
    auto a = run_nested(base, work / "a", &da);
    auto b = run_nested(mut, work / "b", &db);
    ASSERT_TRUE(a.has_value()) << a.error().message();
    ASSERT_TRUE(b.has_value()) << b.error().message();
    EXPECT_EQ(da.digest, db.digest) << "discover never reads the final test";
    const auto wa = fx::weight_lines((work / "a" / "combo.bin").string());
    const auto wb = fx::weight_lines((work / "b" / "combo.bin").string());
    ASSERT_FALSE(wa.empty());
    EXPECT_EQ(wa, wb) << "combine never reads the final test";
    EXPECT_NE(a->digest, b->digest) << "the combined signal on test dates must differ";
}

} // namespace atx_test_w0_i0a_nested_splits
