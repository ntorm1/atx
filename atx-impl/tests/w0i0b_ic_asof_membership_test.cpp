// W0-I0b / D-12: as-of point-in-time membership in the equity baseline views and the
// equity-ic stage, plus the IC-stage knobs this lane wires (E-18 min names, E-09
// delay and common-sample embargo, I-15 optional audit and terminal-return table).
//
// Synthetic fixtures only: a context is published through the real equity-baseline
// stage, so equity-ic consumes a genuine evaluation.bin / combo.bin pair. Nothing here
// is market data and no number below is evidence about any security.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "equity_baseline_views.hpp"
#include "panel_artifact.hpp"
#include "stage_equity_baseline.hpp"
#include "stage_equity_ic.hpp"
#include "w0i0b_support.hpp"

namespace atx_test_w0_i0b_ic_asof {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using Json = nlohmann::json;
using atx::engine::alpha::Panel;
using atx_test_w0_i0b_support::encode_membership;
using atx_test_w0_i0b_support::Rebalance;
using atx_test_w0_i0b_support::write_bytes;

constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::usize kWarmup = 256;
constexpr atx::usize kEval = 70; // > max(H) + delay = 64
constexpr atx::usize kNames = 16;
constexpr atx::usize kJoinRow = 30; // evaluation row where the joiners' rebalance takes effect
constexpr atx::usize kDelistRow = 50; // evaluation row from which three names have no close
// Names 3, 6 and 9 (ids 900004, 900007, 900010) delist; the joiners are 12..15.
constexpr bool is_delisted(atx::usize i) { return i == 3 || i == 6 || i == 9; }
// Fix pass 1: with `halt`, name 5 (id 900006) has no close on evaluation rows
// [kHaltRow, kResumeRow) and then trades again (a halt that resumed).
constexpr atx::usize kHaltName = 5;
constexpr atx::usize kHaltRow = 20;
constexpr atx::usize kResumeRow = 25;
constexpr const char *kStart = "2013-04-04";
constexpr const char *kEnd = "2013-06-13"; // exclusive; 70 observations

std::string contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

atx::i64 id_of(atx::usize i) { return 900001 + static_cast<atx::i64>(i); }

// ---------------------------------------------------------------------------
//  Views level: the smallest context that shows a joiner.
// ---------------------------------------------------------------------------
impl::PanelArtifact small_context(atx::usize dates, atx::usize names) {
    std::vector<double> close(dates * names);
    for (atx::usize d = 0; d < dates; ++d) {
        for (atx::usize i = 0; i < names; ++i) {
            close[d * names + i] = 50.0 + 3.0 * static_cast<double>(i) +
                                   (0.4 + 0.05 * static_cast<double>(i)) * static_cast<double>(d);
        }
    }
    std::vector<double> raw = close;
    for (auto &v : raw) v *= 0.5;
    std::vector<atx::u8> eligible(dates * names, 0);
    for (atx::usize d = kWarmup; d < dates; ++d) {
        for (atx::usize i = 0; i < names; ++i) eligible[d * names + i] = 1; // year-union mask
    }
    auto panel = Panel::create(dates, names, {"close", "raw_close", "volume"},
                               {close, raw, std::vector<double>(dates * names, 1e6)},
                               std::move(eligible))
                     .value();
    impl::PanelIdentity identity;
    identity.instrument_namespace = "synthetic.securityID";
    identity.recipe = "fixture: synthetic ramps";
    identity.parents = {{"fixture", std::string(64, 'a')}};
    for (atx::usize d = 0; d < dates; ++d) {
        identity.session_keys.push_back(static_cast<atx::i64>(d) * kDay);
    }
    for (atx::usize i = 0; i < names; ++i) {
        identity.instrument_ids.push_back(std::to_string(id_of(i)));
        identity.original_instrument_indices.push_back(i);
    }
    return {std::move(panel), std::move(identity), std::string(64, 'b'), std::string(64, 'c')};
}

impl::EquityBaselineConfig view_config(atx::usize dates) {
    impl::EquityBaselineConfig cfg;
    cfg.evaluation = {static_cast<atx::i64>(kWarmup) * kDay, static_cast<atx::i64>(dates) * kDay};
    cfg.observation_basis =
        impl::EquityBaselineObservationBasis::ArchiveRawVolumeAndPointwiseAdjustedCloseV1;
    return cfg;
}

impl::EquityAsOfMembership two_rebalances(atx::usize names, atx::i64 join_key) {
    impl::EquityAsOfMembership m;
    std::vector<atx::i64> early;
    std::vector<atx::i64> all;
    for (atx::usize i = 0; i < names; ++i) {
        all.push_back(id_of(i));
        if (i + 1 < names) early.push_back(id_of(i)); // the last name joins late
    }
    m.effective_session_keys = {0, join_key};
    m.security_ids = {early, all};
    return m;
}

TEST(ImplIcAsOfMembership_Views, MidYearJoinerIsInvisibleBeforeItsEffectiveSession) {
    constexpr atx::usize dates = kWarmup + 12;
    constexpr atx::usize names = 3;
    const auto context = small_context(dates, names);
    const atx::i64 join_key = static_cast<atx::i64>(kWarmup + 5) * kDay; // evaluation row 5

    auto union_cfg = view_config(dates);
    union_cfg.membership_rule = impl::EquityMembershipRule::ContextYearUnionV1;
    const auto year_union = impl::evaluate_equity_baseline(context, union_cfg);
    ASSERT_TRUE(year_union.has_value()) << year_union.error().message();

    auto asof_cfg = view_config(dates);
    asof_cfg.membership = two_rebalances(names, join_key);
    const auto asof = impl::evaluate_equity_baseline(context, asof_cfg);
    ASSERT_TRUE(asof.has_value()) << asof.error().message();

    // Year-union (D-12): the joiner is already admitted on the first evaluation row.
    EXPECT_TRUE(year_union->panel.in_universe(0, 2));
    // As-of: invisible on every session before its rebalance takes effect, visible on it.
    for (atx::usize row = 0; row < 12; ++row) {
        EXPECT_EQ(asof->panel.in_universe(row, 2), row >= 5) << "row " << row;
        EXPECT_TRUE(asof->panel.in_universe(row, 0));
        const bool finite = std::isfinite(asof->signals.alpha_cross_section(0, row)[2]);
        EXPECT_EQ(finite, row >= 5) << "the joiner's signal leaks into row " << row;
    }
    EXPECT_EQ(year_union->admitted_evaluation_cells, 12U * 3U);
    EXPECT_EQ(asof->admitted_evaluation_cells, 12U * 3U - 5U);
    EXPECT_EQ(asof->membership_rejected_cells, 5U);
    EXPECT_EQ(year_union->membership_rejected_cells, 0U);
    // Its time-series history is data, not selection: once admitted, the signal is
    // the same number the year-union view computed.
    EXPECT_EQ(asof->signals.alpha_cross_section(0, 7)[2],
              year_union->signals.alpha_cross_section(0, 7)[2]);
    std::printf("[ImplIcAsOfMembership] joiner admitted cells: year-union %zu, as-of %zu "
                "(rejected %zu)\n",
                static_cast<std::size_t>(year_union->admitted_evaluation_cells),
                static_cast<std::size_t>(asof->admitted_evaluation_cells),
                static_cast<std::size_t>(asof->membership_rejected_cells));
}

TEST(ImplIcAsOfMembership_Views, ConstantMembershipIsBitIdenticalToYearUnion) {
    constexpr atx::usize dates = kWarmup + 12;
    constexpr atx::usize names = 3;
    const auto context = small_context(dates, names);
    auto union_cfg = view_config(dates);
    union_cfg.membership_rule = impl::EquityMembershipRule::ContextYearUnionV1;
    auto asof_cfg = view_config(dates);
    impl::EquityAsOfMembership constant;
    constant.effective_session_keys = {0};
    constant.security_ids = {{id_of(0), id_of(1), id_of(2)}};
    asof_cfg.membership = constant;
    const auto a = impl::evaluate_equity_baseline(context, union_cfg);
    const auto b = impl::evaluate_equity_baseline(context, asof_cfg);
    ASSERT_TRUE(a.has_value() && b.has_value());
    ASSERT_EQ(a->signals.alphas.size(), b->signals.alphas.size());
    for (atx::usize s = 0; s < a->signals.alphas.size(); ++s) {
        const auto &x = a->signals.alphas[s].values;
        const auto &y = b->signals.alphas[s].values;
        ASSERT_EQ(x.size(), y.size());
        for (atx::usize c = 0; c < x.size(); ++c) {
            EXPECT_EQ(std::bit_cast<atx::u64>(x[c]), std::bit_cast<atx::u64>(y[c]));
        }
    }
    for (atx::usize row = 0; row < 12; ++row) {
        for (atx::usize i = 0; i < names; ++i) {
            EXPECT_EQ(a->panel.in_universe(row, i), b->panel.in_universe(row, i));
        }
    }
    EXPECT_EQ(a->admitted_evaluation_cells, b->admitted_evaluation_cells);
    EXPECT_EQ(b->membership_rejected_cells, 0U);
}

TEST(ImplIcAsOfMembership_Views, MembershipLookupAndCutParsing) {
    atx::engine::data::PitMembershipImage image{};
    image.top_n = {1000, 3000};
    image.band_bp = {0, 1000};
    const auto one = [](std::vector<atx::i64> ids) {
        return atx::engine::data::PitMembershipCut{std::move(ids), {}};
    };
    atx::engine::data::PitMembershipRebalance late{};
    late.effective_session_key = 20;
    late.cuts = {one({1}), one({1}), one({1, 2}), one({1, 2, 3})};
    atx::engine::data::PitMembershipRebalance early{};
    early.effective_session_key = 10;
    early.cuts = {one({1}), one({1}), one({2}), one({2})};
    image.rebalances = {late, early}; // stored out of order on purpose

    const auto cut = impl::equity_membership_cut_index(image, "3000:0.10");
    ASSERT_TRUE(cut.has_value()) << cut.error().message();
    EXPECT_EQ(*cut, 3U);
    EXPECT_EQ(*impl::equity_membership_cut_index(image, "3000:0.00"), 2U);
    EXPECT_FALSE(impl::equity_membership_cut_index(image, "2000:0.00").has_value());
    EXPECT_FALSE(impl::equity_membership_cut_index(image, "3000:0.1").has_value());
    const auto m = impl::equity_asof_membership(image, *cut);
    ASSERT_TRUE(m.has_value()) << m.error().message();
    EXPECT_FALSE(m->member(9, 2));  // before the first rebalance: nobody
    EXPECT_TRUE(m->member(10, 2));  // effective ON its session
    EXPECT_FALSE(m->member(19, 3)); // the joiner, one session early
    EXPECT_TRUE(m->member(20, 3));
    EXPECT_FALSE(m->member(25, 4));
    EXPECT_FALSE(impl::equity_asof_membership(image, 4).has_value());
}

// ---------------------------------------------------------------------------
//  Stage level: a checkpoint-16 membership context through baseline -> IC.
// ---------------------------------------------------------------------------
class ImplIcAsOfMembership_Stage : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                   ("atx_w0i0b_ic_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root)) return;
        }
        FAIL() << "Cannot reserve fixture root";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    [[nodiscard]] atx::i64 first_key() const {
        return *atx::engine::data::detail::date_to_nanos(kStart) -
               static_cast<atx::i64>(kWarmup) * kDay;
    }
    [[nodiscard]] atx::i64 eval_key(atx::usize row) const {
        return first_key() + static_cast<atx::i64>(kWarmup + row) * kDay;
    }

    // Membership image: `late` names join at evaluation row kJoinRow (0 = constant).
    std::string write_membership(const std::string &name, atx::usize late) {
        std::vector<atx::i64> early;
        std::vector<atx::i64> all;
        for (atx::usize i = 0; i < kNames; ++i) {
            all.push_back(id_of(i));
            if (i + late < kNames) early.push_back(id_of(i));
        }
        std::vector<Rebalance> rebalances{{first_key(), first_key(), early}};
        if (late > 0) rebalances.push_back({eval_key(kJoinRow) - kDay, eval_key(kJoinRow), all});
        const auto bytes = encode_membership(3000, 0, rebalances);
        write_bytes(root / name, bytes);
        return atx::core::sha256_hex(std::string_view{bytes}).value();
    }

    // A checkpoint-16 context: the universe mask is the year union (every name, every
    // evaluation row), exactly what `panel --universe-membership` publishes.
    void write_context(const std::string &name, const std::string &membership_sha,
                       bool halt = false) {
        const atx::usize dates = kWarmup + kEval;
        std::vector<double> close(dates * kNames);
        std::vector<atx::u8> mask(dates * kNames, 0);
        for (atx::usize d = 0; d < dates; ++d) {
            for (atx::usize i = 0; i < kNames; ++i) {
                // Deterministic, non-monotone paths so every IC is non-degenerate.
                const double t = static_cast<double>(d);
                const double k = static_cast<double>(i);
                close[d * kNames + i] = 60.0 + 2.0 * k + (0.1 + 0.02 * k) * t +
                                        3.0 * std::sin(0.07 * t * (1.0 + 0.13 * k) + k);
                if (d >= kWarmup) mask[d * kNames + i] = 1;
                // Three names stop trading at evaluation row kDelistRow (the terminal
                // table test prices two of them and flags the third).
                if (is_delisted(i) && d >= kWarmup + kDelistRow) {
                    close[d * kNames + i] = std::nan("");
                    mask[d * kNames + i] = 0;
                }
                if (halt && i == kHaltName && d >= kWarmup + kHaltRow &&
                    d < kWarmup + kResumeRow) {
                    close[d * kNames + i] = std::nan("");
                    mask[d * kNames + i] = 0;
                }
            }
        }
        std::vector<double> raw = close;
        for (auto &v : raw) v *= 0.5;
        // The optional fields every checkpoint-17..22 family DSL reads (constant-ish
        // synthetic values; the families only need to compile and be finite).
        std::vector<double> earn_flag(dates * kNames, 0.0);
        std::vector<double> iv_21(dates * kNames);
        std::vector<double> iv_126(dates * kNames);
        std::vector<double> sector(dates * kNames);
        for (atx::usize d = 0; d < dates; ++d) {
            for (atx::usize i = 0; i < kNames; ++i) {
                const auto cell = d * kNames + i;
                earn_flag[cell] = d % 63 == 10 ? 1.0 : 0.0;
                iv_21[cell] = 0.30 + 0.01 * static_cast<double>(i);
                iv_126[cell] = 0.28 + 0.01 * static_cast<double>(i);
                sector[cell] = static_cast<double>(i % 2 + 10);
            }
        }
        std::vector<double> si_shares = close;
        for (auto &v : si_shares) v *= 1e6;
        std::vector<double> market_cap = raw;
        for (auto &v : market_cap) v *= 1e8;
        std::vector<double> high = close;
        std::vector<double> low = close;
        std::vector<double> open = close;
        for (atx::usize cell = 0; cell < close.size(); ++cell) {
            high[cell] = close[cell] * 1.01;
            low[cell] = close[cell] * 0.98;
            open[cell] = close[cell] * 0.995;
        }
        auto panel = Panel::create(dates, kNames,
            {"close", "raw_close", "volume", "earnFlag", "atmCenI_21d", "atmCenI_126d", "sector",
             "high", "low", "open", "si_shares", "market_cap"},
            {close, raw, std::vector<double>(dates * kNames, 1e6), earn_flag, iv_21, iv_126,
             sector, high, low, open, si_shares, market_cap}, std::move(mask));
        ASSERT_TRUE(panel.has_value()) << panel.error().message();
        impl::PanelIdentity identity;
        identity.instrument_namespace = impl::kSpiderRockSecurityIdNamespace;
        for (atx::usize i = 0; i < kNames; ++i) {
            identity.instrument_ids.push_back(std::to_string(id_of(i)));
            identity.original_instrument_indices.push_back(i);
        }
        for (atx::usize d = 0; d < dates; ++d) {
            identity.session_keys.push_back(first_key() + static_cast<atx::i64>(d) * kDay);
        }
        identity.parents = {{"synthetic-fixture", std::string(64, 'a')}};
        identity.recipe = Json{{"version", "tickerhistory-panel-v2-identified"},
            {"start_inclusive_nanos", std::to_string(first_key())},
            {"end_exclusive_nanos",
             std::to_string(first_key() + static_cast<atx::i64>(dates) * kDay)},
            {"research_ohlc", "raw-OHLC*cumulReturnFactor-pointwise"},
            {"raw_close", "unadjusted-as-traded"},
            {"volume", "raw-reported-volume-no-total-return-factor-rescaling"},
            {"universe", {{"adv_basis", "raw_close*raw_volume"},
                {"top_n_tie_break", "original-instrument-index"}, {"top_n_by_adv", 0},
                {"min_adv_usd", 0.0}, {"min_raw_price_exclusive", 1.0}, {"adv_window_bars", 21},
                {"min_mktcap_usd", 0.0}, {"require_sector", false},
                {"current_session_data_included", true},
                {"adv_missing", "full-trailing-window-any-NaN-invalid"}}},
            {"compact_to_universe", true},
            {"compaction", "keep-original-order-if-ever-in-universe-in-selected-window"},
            {"augmentation", {{"enabled", false}, {"adv_windows", Json::array()}}},
            {"universe_membership_sha256", membership_sha}, {"universe_cut", "3000:0.00"},
            {"universe_eval_start", kStart}, {"allow_list_size", kNames},
            {"membership_rule", "year-union-plus-last-prior-rebalance;not-as-of"},
            {"fixture", "synthetic-only-no-market-or-performance-evidence"}}.dump();
        const auto written =
            impl::write_panel_artifact(*panel, (root / name).string(), identity);
        ASSERT_TRUE(written.has_value()) << written.error().message();
    }

    [[nodiscard]] impl::RunConfig baseline_cfg(const std::string &context,
                                               const std::string &out,
                                               const std::string &membership,
                                               const std::string &rule) const {
        impl::RunConfig cfg;
        cfg.subcommand = "equity-baseline";
        cfg.panel = (root / context).string();
        cfg.out = (root / out).string();
        cfg.equity_evaluation_start = kStart;
        cfg.equity_evaluation_end = kEnd;
        cfg.report_aum = 1000;
        cfg.equity_membership = membership.empty() ? "" : (root / membership).string();
        cfg.equity_membership_rule = rule;
        return cfg;
    }

    [[nodiscard]] impl::RunConfig ic_cfg(const std::string &context, const std::string &baseline,
                                         const std::string &out, const std::string &membership,
                                         const std::string &rule) const {
        impl::RunConfig cfg;
        cfg.subcommand = "equity-ic";
        cfg.panel = (root / context).string();
        cfg.equity_baseline_dir = (root / baseline).string();
        cfg.out = (root / out).string();
        cfg.equity_evaluation_start = kStart;
        cfg.equity_evaluation_end = kEnd;
        cfg.equity_trial_ledger = (root / "ledger.jsonl").string();
        cfg.equity_ic_min_names_per_date = 2; // 16 synthetic names
        cfg.set_flags = {"min-names-per-date"};
        cfg.equity_membership = membership.empty() ? "" : (root / membership).string();
        cfg.equity_membership_rule = rule;
        return cfg;
    }
};

TEST_F(ImplIcAsOfMembership_Stage, ConstantMembershipOutputIsIdenticalToTheYearUnionPath) {
    const auto sha = write_membership("constant.bin", 0);
    ASSERT_NO_FATAL_FAILURE(write_context("ctx.bin", sha));
    const auto union_base = impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base_union", "", "year-union-v1"));
    ASSERT_TRUE(union_base.has_value()) << union_base.error().message();
    const auto asof_base = impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base_asof", "constant.bin", "as-of-v2"));
    ASSERT_TRUE(asof_base.has_value()) << asof_base.error().message();

    const auto union_eval =
        impl::read_panel_artifact((root / "base_union/evaluation.bin").string());
    const auto asof_eval = impl::read_panel_artifact((root / "base_asof/evaluation.bin").string());
    ASSERT_TRUE(union_eval.has_value() && asof_eval.has_value());
    EXPECT_EQ(union_eval->payload_sha256, asof_eval->payload_sha256)
        << "constant membership must publish the same evaluation payload";

    const auto union_ic = impl::run_equity_ic(
        ic_cfg("ctx.bin", "base_union", "ic_union", "", "year-union-v1"));
    ASSERT_TRUE(union_ic.has_value()) << union_ic.error().message();
    const auto asof_ic = impl::run_equity_ic(
        ic_cfg("ctx.bin", "base_asof", "ic_asof", "constant.bin", "as-of-v2"));
    ASSERT_TRUE(asof_ic.has_value()) << asof_ic.error().message();
    for (const char *file : {"coverage.csv", "ic.csv", "ic_decay.csv", "signal_autocorr.csv",
                             "quantile_spread.csv"}) {
        const auto a = contents(root / "ic_union" / file);
        ASSERT_FALSE(a.empty()) << file;
        EXPECT_EQ(a, contents(root / "ic_asof" / file)) << file << " differs";
    }
    const auto a = Json::parse(contents(root / "ic_union/ic_summary.json"));
    const auto b = Json::parse(contents(root / "ic_asof/ic_summary.json"));
    EXPECT_EQ(a.at("series"), b.at("series"));
    // Labels: the as-of run says so everywhere a reader looks.
    EXPECT_EQ(b.at("membership").at("rule"), "as-of-pit-membership-v2");
    EXPECT_TRUE(b.at("membership").at("applied").get<bool>());
    EXPECT_EQ(a.at("membership").at("rule"), "context-year-union-v1");
    const auto manifest = Json::parse(contents(root / "base_asof/manifest.json"));
    EXPECT_EQ(manifest.at("recipe").at("membership_mask").at("rule"), "as-of-pit-membership-v2");
    EXPECT_EQ(manifest.at("recipe").at("decision_eligibility"),
              "inherited-context-mask-intersect-as-of-membership-intersect-common-readiness");
    const auto ic_manifest = Json::parse(contents(root / "ic_asof/manifest.json"));
    EXPECT_EQ(ic_manifest.at("membership").at("rule"), "as-of-pit-membership-v2");
    std::printf("[ImplIcAsOfMembership] constant membership: 5 IC csv files byte-identical, "
                "evaluation payload %s\n", asof_eval->payload_sha256.substr(0, 16).c_str());
}

TEST_F(ImplIcAsOfMembership_Stage, MidWindowJoinersAreOutOfTheIcCrossSectionUntilEffective) {
    constexpr atx::usize kLate = 4;
    const auto sha = write_membership("join.bin", kLate);
    ASSERT_NO_FATAL_FAILURE(write_context("ctx.bin", sha));
    ASSERT_TRUE(impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base_union", "", "year-union-v1")).has_value());
    const auto asof_base = impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base_asof", "join.bin", "as-of-v2"));
    ASSERT_TRUE(asof_base.has_value()) << asof_base.error().message();
    const auto summary = Json::parse(contents(root / "base_asof/summary.json"));
    EXPECT_EQ(summary.at("readiness").at("membership_rejected_cells"), kJoinRow * kLate);
    EXPECT_EQ(summary.at("readiness").at("admitted_evaluation_cells"),
              kEval * kNames - kJoinRow * kLate - 3U * (kEval - kDelistRow));

    ASSERT_TRUE(impl::run_equity_ic(
        ic_cfg("ctx.bin", "base_union", "ic_union", "", "year-union-v1")).has_value());
    const auto asof_ic = impl::run_equity_ic(
        ic_cfg("ctx.bin", "base_asof", "ic_asof", "join.bin", "as-of-v2"));
    ASSERT_TRUE(asof_ic.has_value()) << asof_ic.error().message();
    // coverage.csv: n_eligible per (date, block). Before the join row the as-of
    // cross-section has 12 names; the year-union one has all 16.
    const auto eligible_on = [&](const std::string &dir, atx::usize date) {
        std::istringstream in(contents(root / dir / "coverage.csv"));
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::vector<std::string> f;
            std::stringstream ss(line);
            for (std::string cell; std::getline(ss, cell, ',');) f.push_back(cell);
            if (f[0] == std::to_string(date) && f[2] == "momentum_252" && f[3] == "1" &&
                f[4] == "DropMissingForward" && f[5] == "full") {
                return std::stoul(f[6]);
            }
        }
        return 0UL;
    };
    EXPECT_EQ(eligible_on("ic_union", 0), kNames);
    EXPECT_EQ(eligible_on("ic_asof", 0), kNames - kLate);
    EXPECT_EQ(eligible_on("ic_asof", kJoinRow - 1), kNames - kLate);
    EXPECT_EQ(eligible_on("ic_asof", kJoinRow), kNames);
    EXPECT_NE(contents(root / "ic_union/ic.csv"), contents(root / "ic_asof/ic.csv"));
    std::printf("[ImplIcAsOfMembership] joiners: n_eligible row0 union %lu as-of %lu; row %zu "
                "as-of %lu\n", eligible_on("ic_union", 0), eligible_on("ic_asof", 0),
                static_cast<std::size_t>(kJoinRow), eligible_on("ic_asof", kJoinRow));
}

TEST_F(ImplIcAsOfMembership_Stage, AMembershipContextNeedsItsOwnImageUnderTheAsOfRule) {
    const auto sha = write_membership("join.bin", 4);
    (void)write_membership("other.bin", 2);
    ASSERT_NO_FATAL_FAILURE(write_context("ctx.bin", sha));
    const auto missing = impl::run_equity_baseline(baseline_cfg("ctx.bin", "b1", "", "as-of-v2"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message().find("--membership"), std::string::npos);
    const auto wrong =
        impl::run_equity_baseline(baseline_cfg("ctx.bin", "b2", "other.bin", "as-of-v2"));
    ASSERT_FALSE(wrong.has_value());
    EXPECT_NE(wrong.error().message().find("was built from"), std::string::npos);
    const auto both =
        impl::run_equity_baseline(baseline_cfg("ctx.bin", "b3", "join.bin", "year-union-v1"));
    ASSERT_FALSE(both.has_value());
    // An as-of baseline cannot be read back by a year-union IC (the rule is recorded).
    ASSERT_TRUE(impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base_asof", "join.bin", "as-of-v2")).has_value());
    const auto mixed = impl::run_equity_ic(
        ic_cfg("ctx.bin", "base_asof", "ic_mixed", "", "year-union-v1"));
    ASSERT_FALSE(mixed.has_value());
    EXPECT_NE(mixed.error().message().find("different membership rule"), std::string::npos);
}

// ---------------------------------------------------------------------------
//  IC knobs wired by this lane: E-18, E-09 (+ common-sample embargo), I-15.
// ---------------------------------------------------------------------------
TEST_F(ImplIcAsOfMembership_Stage, IcKnobsMinNamesDelayEmbargoAndOptionalAudit) {
    const auto sha = write_membership("constant.bin", 0);
    ASSERT_NO_FATAL_FAILURE(write_context("ctx.bin", sha));
    ASSERT_TRUE(impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base", "constant.bin", "as-of-v2")).has_value());

    // No required-mark audit exists beside this baseline: I-15 made it optional.
    auto cfg = ic_cfg("ctx.bin", "base", "ic_default", "constant.bin", "as-of-v2");
    const auto run = impl::run_equity_ic(cfg);
    ASSERT_TRUE(run.has_value()) << run.error().message();
    const auto summary = Json::parse(contents(root / "ic_default/ic_summary.json"));
    EXPECT_EQ(summary.at("execution_delay"), 1);
    EXPECT_EQ(summary.at("common_sample_dates"), kEval - (63U + 1U)); // E-09 embargo
    EXPECT_EQ(summary.at("block_len_rule"), "two-horizon-v2");
    EXPECT_EQ(summary.at("alignment"),
              "signal-at-t-return-from-entry-close-t-plus-1-deployed-book-executes-at-t-plus-1");
    EXPECT_EQ(summary.at("required_mark_audit"), "absent-optional-ex34-restriction-empty");
    bool saw_block = false;
    for (const auto &row : summary.at("series")) {
        if (row.at("horizon") != 21) continue;
        EXPECT_EQ(row.at("block_len"), 42);
        EXPECT_EQ(row.at("execution_delay"), 1);
        EXPECT_EQ(row.at("embargo"), 22);
        EXPECT_TRUE(row.at("full").contains("ic_mean_hac"));
        saw_block = true;
    }
    EXPECT_TRUE(saw_block);
    const auto manifest = Json::parse(contents(root / "ic_default/manifest.json"));
    EXPECT_EQ(manifest.at("recipe").at("min_names_per_date"), 2);
    EXPECT_EQ(manifest.at("terminal_evidence").at("terminal_table_source"),
              "frozen-checkpoint14-2013-audit-table");

    // E-18: the default floor of 50 names refuses every 16-name date.
    auto strict = ic_cfg("ctx.bin", "base", "ic_min50", "constant.bin", "as-of-v2");
    strict.equity_ic_min_names_per_date = 50;
    strict.set_flags.clear();
    const auto floored = impl::run_equity_ic(strict);
    ASSERT_TRUE(floored.has_value()) << floored.error().message();
    const auto strict_manifest = Json::parse(contents(root / "ic_min50/manifest.json"));
    EXPECT_EQ(strict_manifest.at("recipe").at("min_names_per_date"), 50);
    // Every emitted date is below the floor on every block, so nothing is reportable.
    const auto &confirmed = strict_manifest.at("predictions_confirmed");
    EXPECT_GT(confirmed.at("dates_below_min_names_max_over_blocks").get<int>(), 0);
    const auto strict_summary = Json::parse(contents(root / "ic_min50/ic_summary.json"));
    for (const auto &row : strict_summary.at("series")) {
        EXPECT_EQ(row.at("full").at("dates_emitted"), 0);
    }

    // E-09: delay 0 is refused without the opt-in and reproduces the old label with it.
    auto same_close = ic_cfg("ctx.bin", "base", "ic_delay0", "constant.bin", "as-of-v2");
    same_close.equity_ic_execution_delay = 0;
    same_close.set_flags = {"min-names-per-date", "ic-execution-delay"};
    const auto refused = impl::run_equity_ic(same_close);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("--allow-same-close"), std::string::npos);
    auto legacy = ic_cfg("ctx.bin", "base", "ic_legacy", "constant.bin", "as-of-v2");
    legacy.equity_ic_execution_delay = 0;
    legacy.allow_same_close = true;
    legacy.equity_ic_block_len_rule = "half-horizon-v1";
    legacy.set_flags = {"min-names-per-date", "ic-execution-delay", "allow-same-close",
                        "ic-block-len-rule"};
    const auto legacy_run = impl::run_equity_ic(legacy);
    ASSERT_TRUE(legacy_run.has_value()) << legacy_run.error().message();
    const auto legacy_summary = Json::parse(contents(root / "ic_legacy/ic_summary.json"));
    EXPECT_EQ(legacy_summary.at("alignment"),
              "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1");
    EXPECT_EQ(legacy_summary.at("common_sample_dates"), kEval - 63U);
    for (const auto &row : legacy_summary.at("series")) {
        if (row.at("horizon") == 21) EXPECT_EQ(row.at("block_len"), 11);
    }
}

TEST_F(ImplIcAsOfMembership_Stage, TerminalReturnTableReplacesTheFrozen2013Table) {
    const auto sha = write_membership("constant.bin", 0);
    ASSERT_NO_FATAL_FAILURE(write_context("ctx.bin", sha));
    ASSERT_TRUE(impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base", "constant.bin", "as-of-v2")).has_value());
    // Security 900004 exits by a cash merger (value), 900007 by a -30% delisting
    // return, and 900010 is flagged terminal without evidence (never priced).
    const auto table_path = root / "terminal.csv";
    {
        std::ofstream out(table_path, std::ios::binary);
        out << "security_id,terminal_value,terminal_return,special_dividend,record_date,"
               "evidenced,source\n"
               "900004,75.5,,,,true,synthetic merger\n"
               "900007,,-0.3,,,true,synthetic delisting\n"
               "900010,,,,,false,synthetic unevidenced\n";
    }
    auto cfg = ic_cfg("ctx.bin", "base", "ic_table", "constant.bin", "as-of-v2");
    cfg.equity_terminal_returns = table_path.string();
    cfg.set_flags = {"min-names-per-date", "terminal-returns"};
    const auto run = impl::run_equity_ic(cfg);
    ASSERT_TRUE(run.has_value()) << run.error().message();
    const auto request = Json::parse(contents(root / "ic_table/request.json"));
    EXPECT_EQ(request.at("terminal_table").at("events"), 3);
    EXPECT_EQ(request.at("required_mark_audit").at("terminal_hypothesis_ids"),
              Json::array({900004, 900007}));
    EXPECT_EQ(request.at("required_mark_audit").at("terminal_unevidenced_ids"),
              Json::array({900010}));
    const auto manifest = Json::parse(contents(root / "ic_table/manifest.json"));
    bool parent = false;
    for (const auto &p : manifest.at("parents")) {
        parent = parent || p.at("role") == "terminal-return-table";
    }
    EXPECT_TRUE(parent) << "the table is hash-bound as a parent";
    EXPECT_GT(manifest.at("terminal_evidence").at("n_terminal_applied_cells_measured").get<int>(),
              0);
    EXPECT_GT(
        manifest.at("terminal_evidence").at("n_terminal_unevidenced_cells_measured").get<int>(),
        0);
    // Fix pass 1: the computed R-A unclassified count is published in request.json and
    // manifest.json as well as the ledger (no audit here, so the partition is empty).
    EXPECT_EQ(request.at("required_mark_audit").at("unclassified_id_count"), 0);
    EXPECT_EQ(manifest.at("terminal_evidence").at("unclassified_id_count"), 0);
    EXPECT_NE(contents(root / "ledger.jsonl").find("\"unclassified_id_count\":0"),
              std::string::npos);
}

TEST_F(ImplIcAsOfMembership_Stage, TerminalReturnRowAfterAResumedHaltIsRefused) {
    // A return row prices off the security's last close. For a halt that resumed, a
    // horizon ending inside the halt would be priced off a later close (look-ahead),
    // so the view is refused; a value row does not depend on any close and still runs.
    const auto sha = write_membership("constant.bin", 0);
    ASSERT_NO_FATAL_FAILURE(write_context("ctx.bin", sha, /*halt=*/true));
    ASSERT_TRUE(impl::run_equity_baseline(
        baseline_cfg("ctx.bin", "base", "constant.bin", "as-of-v2")).has_value());
    const auto run_with = [&](const std::string &leaf, const std::string &row) {
        const auto table_path = root / (leaf + ".csv");
        {
            std::ofstream out(table_path, std::ios::binary);
            out << "security_id,terminal_value,terminal_return,special_dividend,record_date,"
                   "evidenced,source\n"
                << row;
        }
        auto cfg = ic_cfg("ctx.bin", "base", leaf, "constant.bin", "as-of-v2");
        cfg.equity_terminal_returns = table_path.string();
        cfg.set_flags = {"min-names-per-date", "terminal-returns"};
        return impl::run_equity_ic(cfg);
    };
    const auto halted = run_with("ic_halt_return", "900006,,-0.3,,,true,synthetic halt\n");
    ASSERT_FALSE(halted.has_value());
    EXPECT_NE(halted.error().message().find("after an interior gap"), std::string::npos)
        << halted.error().message();
    EXPECT_NE(halted.error().message().find("900006"), std::string::npos);
    // A genuine delisting (no close after its gap) still prices off its last close.
    const auto delisted = run_with("ic_delist_return", "900007,,-0.3,,,true,synthetic delist\n");
    ASSERT_TRUE(delisted.has_value()) << delisted.error().message();
    const auto valued = run_with("ic_halt_value", "900006,75.5,,,,true,synthetic merger\n");
    ASSERT_TRUE(valued.has_value()) << valued.error().message();
}

TEST(ImplIcAsOfMembership_TerminalTable, ParserEnforcesTheContract) {
    const std::string header =
        "security_id,terminal_value,terminal_return,special_dividend,record_date,evidenced,"
        "source\n";
    const auto ok = impl::parse_equity_terminal_table(
        header + "35715,13.75,,0.13,2013-10-28,true,audit\n146189,,,,,false,pcs\n");
    ASSERT_TRUE(ok.has_value()) << ok.error().message();
    ASSERT_EQ(ok->events.size(), 2U);
    const auto *dell = ok->find(35715);
    ASSERT_NE(dell, nullptr);
    const auto record = *atx::engine::data::detail::date_to_nanos("2013-10-28");
    EXPECT_DOUBLE_EQ(impl::equity_terminal_cash(*dell, record), 13.88);
    EXPECT_DOUBLE_EQ(impl::equity_terminal_cash(*dell, record + kDay), 13.75);
    EXPECT_DOUBLE_EQ(impl::equity_terminal_cash(*ok->find(146189), record), 0.0);
    // The frozen table reproduces the checkpoint-14 function exactly.
    const auto frozen = impl::equity_ic_frozen_terminal_table();
    for (const atx::i64 id : {37648, 35715, 39970, 146189, 150340}) {
        const auto *event = frozen.find(id);
        for (const atx::i64 key : {record - kDay, record, record + kDay}) {
            EXPECT_DOUBLE_EQ(event ? impl::equity_terminal_cash(*event, key) : 0.0,
                             impl::equity_ic_terminal_value(id, key));
        }
    }
    for (const std::string &bad :
         {std::string("security_id,value\n1,2\n"),                        // header
          header + "1,2,-0.1,,,true,both\n",                                // value AND return
          header + "1,,,,,true,neither\n",                                  // evidenced, nothing
          header + "1,nan,,,,true,nan\n",                                   // non-finite
          header + "1,5,,0.2,,true,dividend without record date\n",         // AR-2
          header + "1,5,,,,maybe,flag\n",                                   // evidenced value
          header + "1,5,,,,true,a\n1,6,,,,true,dup\n",                      // duplicate id
          header + "01,5,,,,true,noncanonical\n",                           // id spelling
          header + "2,,-1,,,true,total loss is value 0 not a return\n"}) {  // return <= -1
        EXPECT_FALSE(impl::parse_equity_terminal_table(bad).has_value()) << bad;
    }
}

} // namespace atx_test_w0_i0b_ic_asof
