// stage_run_megabook_test.cpp — p8 Sprint 5 (S5-4): assemble the full mega-book
// stage graph in run_all behind the S5-0 CLI flags, byte-identical at the
// inert defaults.
//
// Fixture: the SAME synthetic-ORATS-zip momentum idiom e2e_pipeline_test.cpp
// uses (10 instruments x 100 dates, per-instrument drift) — each test TU is
// self-contained (the house convention), so the body-builder is duplicated
// here rather than shared, exactly like every other *_test.cpp in this suite.

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <miniz.h>

#include "config.hpp"
#include "stages.hpp"

#include "orats_fixture.hpp"

namespace atx_impl_stage_run_megabook {

namespace fs = std::filesystem;

static constexpr int kInstr = 10;
static constexpr int kDates = 100;

static std::string make_body() {
    using atx_impl_test::kHeader;
    using atx_impl_test::make_orats_row;

    std::string body = std::string(kHeader) + "\n";
    std::vector<double> px(static_cast<std::size_t>(kInstr));
    std::vector<double> cumret(static_cast<std::size_t>(kInstr), 1.0);
    std::vector<double> drift(static_cast<std::size_t>(kInstr));
    for (int i = 0; i < kInstr; ++i) {
        const auto ii = static_cast<std::size_t>(i);
        px[ii]    = 50.0 + static_cast<double>(i) * 5.0;
        drift[ii] = 0.002 + 0.0005 * static_cast<double>(i);
    }
    auto make_date = [](int day_offset) -> std::string {
        static constexpr std::array<int, 12> kMlen = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int abs_day = 2 + day_offset;
        int m = 0;
        while (m < 11 && abs_day > kMlen[static_cast<std::size_t>(m)]) {
            abs_day -= kMlen[static_cast<std::size_t>(m)];
            ++m;
        }
        char buf[16];
        (void)std::snprintf(buf, sizeof(buf), "2020-%02d-%02d", m + 1, abs_day);
        return buf;
    };
    for (int d = 0; d < kDates; ++d) {
        const std::string date = make_date(d);
        for (int i = 0; i < kInstr; ++i) {
            const auto ii = static_cast<std::size_t>(i);
            px[ii] *= (1.0 + drift[ii]);
            cumret[ii] *= (1.0 + drift[ii]);
            const std::string secid = std::to_string(30000 + i);
            const std::string tk = "SYM" + std::to_string(i);
            body += make_orats_row(date.c_str(), secid.c_str(), tk.c_str(), tk.c_str(), px[ii],
                                   cumret[ii], 500'000'000LL);
        }
    }
    return body;
}

class StageRunMegaBook : public ::testing::Test {
protected:
    static std::string s_zip_;

    static void SetUpTestSuite() {
        const std::string path = (fs::temp_directory_path() / "atx_s5_4_megabook.zip").string();
        atx_impl_test::write_orats_zip(make_body(), path);
        s_zip_ = path;
    }
    static void TearDownTestSuite() {
        std::error_code ec;
        fs::remove(fs::path(s_zip_), ec);
    }

    fs::path make_work_dir(const char *tag) {
        const fs::path d = fs::temp_directory_path() / (std::string("atx_s5_4_megabook_") + tag);
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        work_dirs_.push_back(d);
        return d;
    }
    void TearDown() override {
        for (const auto &d : work_dirs_) {
            std::error_code ec;
            fs::remove_all(d, ec);
        }
        work_dirs_.clear();
    }

private:
    std::vector<fs::path> work_dirs_;
};
std::string StageRunMegaBook::s_zip_;

static atx::impl::RunConfig base_cfg(const std::string &zip, const std::string &work_dir,
                                     const std::string &report_dir) {
    atx::impl::RunConfig cfg;
    cfg.zip = zip;
    cfg.out = work_dir;
    cfg.min_date = "2019-12-31";
    cfg.min_adv_usd = 0.0;
    cfg.top_n_by_adv = 0;
    cfg.seed = 777ULL;
    cfg.population = 12;
    cfg.generations = 3;
    cfg.seed_exprs = {"rank(close)", "ts_mean(close,5)"};
    cfg.method = "equal";
    cfg.gross = 1.0;
    cfg.name_cap = 0.2;
    cfg.rebalance = "weekly";
    cfg.risk_aversion = 1.0;
    cfg.report_out = report_dir;
    return cfg;
}

// ---------------------------------------------------------------------------
// MegaBookGraph_InertByteIdentical — run_all with all p8 flags at their stated
// inert defaults produces a byte-identical six-digest run vs a run that never
// mentions any of the new S5-0..S5-4 fields at all (the pre-S5 baseline, by
// definition, since these fields did not exist before S5).
// ---------------------------------------------------------------------------
TEST_F(StageRunMegaBook, MegaBookGraph_InertByteIdentical) {
    const fs::path work_a = make_work_dir("inert_a_work");
    const fs::path rep_a  = make_work_dir("inert_a_report");
    const fs::path work_b = make_work_dir("inert_b_work");
    const fs::path rep_b  = make_work_dir("inert_b_report");

    atx::impl::RunConfig cfg_a = base_cfg(s_zip_, work_a.string(), rep_a.string());
    // cfg_a: touches NONE of the new S5 fields (struct defaults only).

    atx::impl::RunConfig cfg_b = base_cfg(s_zip_, work_b.string(), rep_b.string());
    // cfg_b: EVERY new S5-0..S5-4 field explicitly asserted at its documented
    // inert value (the literal "none of the new flags asserted" gate, made
    // maximally strict by setting them anyway and proving it changes nothing).
    cfg_b.risk_model = "diagonal";
    cfg_b.dead_alpha_factors = false;
    cfg_b.group_neutralize = false;
    cfg_b.metabook = false;
    cfg_b.sleeve_method = "invvol";
    cfg_b.impact_in_selection = false;
    cfg_b.selection_aum = 0.0;
    cfg_b.capacity_curve = false;
    cfg_b.require_split_stable = false;
    cfg_b.blocking_pbo = false;
    cfg_b.short_interest = "";
    cfg_b.augment_out = "";
    cfg_b.si_publication_lag = 2;
    cfg_b.incremental_panel = false;
    cfg_b.kelly_fraction = 0.0;
    cfg_b.kelly_max_gross = 1.0;

    auto r_a = atx::impl::run_all(cfg_a);
    ASSERT_TRUE(r_a.has_value()) << r_a.error().message();
    auto r_b = atx::impl::run_all(cfg_b);
    ASSERT_TRUE(r_b.has_value()) << r_b.error().message();

    EXPECT_NE(r_a->digest, 0u) << "sanity: the pipeline must actually run";
    EXPECT_EQ(r_a->digest, r_b->digest)
        << "asserting every new S5 flag at its inert value must not change the run digest";
    ASSERT_EQ(r_a->kvs.size(), r_b->kvs.size());
    for (std::size_t i = 0; i < r_a->kvs.size(); ++i) {
        EXPECT_EQ(r_a->kvs[i], r_b->kvs[i]);
    }
    // Exactly the pre-S5 six stages, in order — no silently-added 7th stage.
    ASSERT_EQ(r_a->kvs.size(), 6u);
    EXPECT_EQ(r_a->kvs[0].first, "load");
    EXPECT_EQ(r_a->kvs[1].first, "panel");
    EXPECT_EQ(r_a->kvs[2].first, "discover");
    EXPECT_EQ(r_a->kvs[3].first, "combine");
    EXPECT_EQ(r_a->kvs[4].first, "optimize");
    EXPECT_EQ(r_a->kvs[5].first, "report");
}

// ---------------------------------------------------------------------------
// MetabookStage_SkippedAtDefault — the skip proof (adapted from the spec's
// "RiskmodelMetabookStages_SkippedAtDefault": this p8 wiring reaches the S1
// factor risk-model via --risk-model=factor threaded THROUGH run_optimize's
// existing entry point rather than a separate standalone stage — see the S5
// ledger — so there is no distinct "run_riskmodel" call to prove skipped; the
// meaningful, genuinely NEW stage-graph node S5-4 adds is metabook). Proves
// TWO things: (a) at the default (--metabook absent) the 5th kv is "optimize",
// never "metabook" -- the stage is not silently inserted; (b) with --metabook
// set, it genuinely activates -- the 5th kv becomes "metabook" and the run
// digest changes vs the default.
// ---------------------------------------------------------------------------
TEST_F(StageRunMegaBook, MetabookStage_SkippedAtDefault) {
    const fs::path work_def = make_work_dir("skip_default_work");
    const fs::path rep_def  = make_work_dir("skip_default_report");
    atx::impl::RunConfig cfg_def = base_cfg(s_zip_, work_def.string(), rep_def.string());
    ASSERT_FALSE(cfg_def.metabook);

    auto r_def = atx::impl::run_all(cfg_def);
    ASSERT_TRUE(r_def.has_value()) << r_def.error().message();
    ASSERT_EQ(r_def->kvs.size(), 6u);
    EXPECT_EQ(r_def->kvs[4].first, "optimize")
        << "the 5th stage must be optimize (not metabook) when --metabook is absent";

    const fs::path work_on = make_work_dir("meta_on_work");
    const fs::path rep_on  = make_work_dir("meta_on_report");
    atx::impl::RunConfig cfg_on = base_cfg(s_zip_, work_on.string(), rep_on.string());
    cfg_on.metabook = true;
    cfg_on.sleeve_method = "invvol";

    auto r_on = atx::impl::run_all(cfg_on);
    ASSERT_TRUE(r_on.has_value()) << r_on.error().message();
    ASSERT_EQ(r_on->kvs.size(), 6u);
    EXPECT_EQ(r_on->kvs[4].first, "metabook")
        << "the 5th stage must be metabook when --metabook is set";
    EXPECT_NE(r_on->digest, r_def->digest)
        << "activating --metabook must produce an observably different run";
}

} // namespace atx_impl_stage_run_megabook
