// W0-I0b: equity-mine requires --membership (I-16), refuses a same-close delay
// without the opt-in (B-02 impl), and records its trials with windows / IS flag /
// family tags, a cluster-N DSR, the registry chain head and the scorer's nw_lags,
// ic_horizons and periods_per_year (E-16 wiring, I-23). Synthetic fixtures only.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "panel_artifact.hpp"
#include "stage_equity_mine.hpp"
#include "w0i0b_support.hpp"

namespace atx_test_w0_i0b_mine {
namespace fs = std::filesystem;
using atx::engine::alpha::Panel;
using atx_test_w0_i0b_support::dispatch;
using atx_test_w0_i0b_support::encode_membership;
using atx_test_w0_i0b_support::Rebalance;
using atx_test_w0_i0b_support::write_bytes;
using nlohmann::json;

constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::size_t kInst = 60;
constexpr std::size_t kLate = 10; // ids 1050..1059 join mid-way in the as-of image

std::vector<std::int64_t> weekdays(std::int64_t first_ns, std::size_t n) {
    std::vector<std::int64_t> k;
    for (std::int64_t t = first_ns; k.size() < n; t += kDayNs) {
        const std::int64_t dow = ((t / kDayNs) + 4) % 7; // 1970-01-01 was a Thursday
        if (dow != 0 && dow != 6) k.push_back(t);
    }
    return k;
}

class ImplMineRequiresMembership_Cli : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("atx_w0i0b_mine_" + std::to_string(stamp) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(root_);
        keys_ = weekdays(atx::impl::mine::parse_iso_date_ns("2015-01-01").value(), 800);
        std::mt19937_64 rng(7);
        std::normal_distribution<double> z(0.0, 1.0);
        cols_.assign(6, std::vector<double>(keys_.size() * kInst));
        auto &close = cols_[0], &open = cols_[1], &high = cols_[2], &low = cols_[3],
             &vol = cols_[4], &sig = cols_[5];
        for (auto &v : sig) v = z(rng);
        std::vector<double> px(kInst, 40.0);
        for (std::size_t d = 0; d < keys_.size(); ++d) {
            for (std::size_t i = 0; i < kInst; ++i) {
                const std::size_t c = d * kInst + i;
                if (d > 0) {
                    px[i] *= 1.0 + (d >= 2 ? 0.004 * sig[(d - 2) * kInst + i] : 0.0) +
                             0.02 * z(rng);
                }
                close[c] = open[c] = px[i];
                high[c] = px[i] * 1.01;
                low[c] = px[i] * 0.99;
                vol[c] = 1.0e6;
            }
        }
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    std::string context(const std::string &name, std::size_t b, std::size_t e) {
        const std::size_t n = e - b;
        std::vector<std::vector<double>> cols;
        for (const auto &c : cols_) {
            cols.emplace_back(c.begin() + static_cast<std::ptrdiff_t>(b * kInst),
                              c.begin() + static_cast<std::ptrdiff_t>(e * kInst));
        }
        // Synthetic closes are unadjusted prices.
        cols.push_back(cols[0]);
        auto panel = Panel::create(n, kInst, {"close", "open", "high", "low", "volume", "sig", "raw_close"},
                                   std::move(cols), std::vector<std::uint8_t>(n * kInst, 1));
        EXPECT_TRUE(panel.has_value());
        atx::impl::PanelIdentity id;
        id.instrument_namespace = std::string{atx::impl::kSpiderRockSecurityIdNamespace};
        id.session_keys.assign(keys_.begin() + static_cast<std::ptrdiff_t>(b),
                               keys_.begin() + static_cast<std::ptrdiff_t>(e));
        for (std::size_t i = 0; i < kInst; ++i) {
            id.instrument_ids.push_back(std::to_string(1000 + i));
            id.original_instrument_indices.push_back(i);
        }
        id.recipe = "synthetic=w0i0b\n";
        id.parents = {{"source", atx::core::sha256_hex(name).value()}};
        const auto path = (root_ / (name + ".bin")).string();
        EXPECT_TRUE(atx::impl::write_panel_artifact(*panel, path, id).has_value());
        return path;
    }

    // `late` ids join at 2016-06-01 (0 = constant membership from before the data).
    std::string membership(const std::string &name, std::size_t late) {
        std::vector<std::int64_t> early;
        std::vector<std::int64_t> all;
        for (std::size_t i = 0; i < kInst; ++i) {
            all.push_back(1000 + static_cast<std::int64_t>(i));
            if (i + late < kInst) early.push_back(1000 + static_cast<std::int64_t>(i));
        }
        const auto first = atx::impl::mine::parse_iso_date_ns("2014-12-01").value();
        const auto join = atx::impl::mine::parse_iso_date_ns("2016-06-01").value();
        std::vector<Rebalance> r{{first, first, early}};
        if (late > 0) r.push_back({join - kDayNs, join, all});
        const auto path = root_ / name;
        write_bytes(path, encode_membership(3000, 0, r));
        return path.string();
    }

    std::vector<std::string> args(const fs::path &out) {
        if (train_.empty()) {
            train_ = context("train", 0, 430); // ends before 2016-09-15
            val_ = context("val", 300, 700);   // ends before 2017-10-01
            std::ofstream(root_ / "seeds.txt")
                << "rank(sig)\nrank(close)\n-1 * rank(delta(close, 5))\n";
        }
        const std::string &train = train_;
        const std::string &val = val_;
        return {"atx-impl", "equity-mine", "--train-contexts", train, "--validation-contexts", val,
                "--train-start", "2015-03-01", "--validation-start", "2016-09-15",
                "--holdout-start", "2017-10-01", "--extra-seeds", (root_ / "seeds.txt").string(),
                "--no-literature", "--no-search", "--min-names", "20", "--cost-bps", "1",
                "--n-boot", "100", "--threads", "2", "--out", out.string()};
    }

    json gate(const fs::path &out) {
        std::ifstream in(out / "gate_report.json");
        return json::parse(in);
    }

    fs::path root_;
    std::string train_;
    std::string val_;
    std::vector<std::int64_t> keys_;
    std::vector<std::vector<double>> cols_;
};

TEST_F(ImplMineRequiresMembership_Cli, WithoutMembershipTheRunIsRefused) {
    const auto refused = dispatch(args(root_ / "none"));
    EXPECT_EQ(refused.code, 2);
    EXPECT_NE(refused.err.find("--membership <membership.bin> is required"), std::string::npos)
        << refused.err;
    EXPECT_FALSE(fs::exists(root_ / "none"));
    // Asking for an image AND the year-union rule is contradictory.
    auto both = args(root_ / "both");
    both.insert(both.end(), {"--membership", membership("m.bin", 0), "--membership-rule",
                             "year-union-v1"});
    EXPECT_EQ(dispatch(both).code, 2);
}

TEST_F(ImplMineRequiresMembership_Cli, AsOfImageRunsAndTheYearUnionIsOnlyAnExplicitFallback) {
    auto asof = args(root_ / "asof");
    asof.insert(asof.end(), {"--membership", membership("join.bin", kLate)});
    const auto a = dispatch(asof);
    ASSERT_EQ(a.code, 0) << a.err;
    auto legacy = args(root_ / "legacy");
    legacy.insert(legacy.end(), {"--membership-rule", "year-union-v1"});
    const auto b = dispatch(legacy);
    ASSERT_EQ(b.code, 0) << b.err;

    const auto ga = gate(root_ / "asof");
    const auto gb = gate(root_ / "legacy");
    EXPECT_EQ(ga["train"]["membership_rule"], "as-of-pit-membership");
    EXPECT_EQ(gb["train"]["membership_rule"], "context-year-union-not-as-of");
    EXPECT_EQ(ga["membership"]["rule"], "as-of-v2");
    EXPECT_EQ(gb["membership"]["rule"], "year-union-v1");
    // The joiners are out of the tradable mask until 2016-06-01 under as-of.
    const auto asof_cells = ga["train"]["member_cells_in_window"].get<std::size_t>();
    const auto union_cells = gb["train"]["member_cells_in_window"].get<std::size_t>();
    EXPECT_LT(asof_cells, union_cells);
    std::printf("[ImplMineRequiresMembership] train member cells: as-of %zu vs year-union %zu "
                "(%zu joiner ids)\n", asof_cells, union_cells, kLate);
}

TEST_F(ImplMineRequiresMembership_Cli, RecordingWritesWindowsDsrChainHeadAndScorerKnobs) {
    auto a = args(root_ / "rec");
    a.insert(a.end(), {"--membership", membership("c.bin", 0)});
    const auto run = dispatch(a);
    ASSERT_EQ(run.code, 0) << run.err;
    const auto g = gate(root_ / "rec");
    // I-23: the statistics knobs are recorded, not implicit.
    EXPECT_EQ(g["config"]["nw_lags"], 5);
    EXPECT_EQ(g["config"]["ic_horizons"], json::array({1, 5, 21}));
    EXPECT_EQ(g["config"]["periods_per_year"], 252.0);
    EXPECT_EQ(g["config"]["delay_sessions"], 1);
    EXPECT_EQ(g["config"]["allow_same_close"], false);
    // E-16: one calendar spanning train + validation; every trial is in-sample.
    const auto &t = g["trials"];
    const auto train_len = t["train_window_len"].get<std::size_t>();
    const auto val_len = g["validation"]["window_sessions"].get<std::size_t>();
    EXPECT_EQ(train_len, g["train"]["window_sessions"].get<std::size_t>());
    EXPECT_EQ(t["pnl_len"].get<std::size_t>(), train_len + val_len);
    EXPECT_EQ(t["n_in_sample"], t["n_raw"]);
    EXPECT_EQ(t["n_out_of_sample"], 0);
    EXPECT_EQ(t["dsr_rule"], "cluster-mc-floor-v2");
    EXPECT_GE(t["dsr_clusters"].get<int>(), 1);
    EXPECT_EQ(t["chain_head"]["records"], t["n_raw"]);
    std::ifstream mf(root_ / "rec" / "manifest.json");
    const json m = json::parse(mf);
    EXPECT_EQ(m["trial_registry_chain_head"], t["chain_head"]);
    std::printf("[ImplMineRequiresMembership] trials n_raw=%d pnl_len=%zu (train %zu + val %zu) "
                "clusters=%d sr*_mc=%.4f\n",
                t["n_raw"].get<int>(), t["pnl_len"].get<std::size_t>(), train_len, val_len,
                t["dsr_clusters"].get<int>(), t["dsr_sr_star_mc_per_period"].get<double>());
}

// Fix pass 1 (RULES §2): the report-only train DSR rule is a versioned selector, and
// summary-n-eff-v1 re-derives the pre-W0 rule (N = n_eff over the registry summary).
std::vector<std::vector<std::string>> csv_rows(const fs::path &path) {
    std::ifstream in(path);
    std::vector<std::vector<std::string>> rows;
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> cells;
        std::stringstream ss(line);
        for (std::string cell; std::getline(ss, cell, ',');) cells.push_back(cell);
        rows.push_back(std::move(cells));
    }
    return rows;
}

TEST_F(ImplMineRequiresMembership_Cli, DsrRuleSelectorReproducesThePreW0SummaryRule) {
    const auto mem = membership("c.bin", 0);
    // Correlated seeds (three variants of rank(sig)) so the registry's n_eff falls
    // below n_raw and the two summary rules genuinely differ.
    const auto seeds = root_ / "correlated_seeds.txt";
    std::ofstream(seeds) << "rank(sig)\nrank(sig + 0.2 * rank(close))\n"
                            "rank(sig + 0.4 * rank(close))\nrank(close)\n";
    const auto run = [&](const std::string &leaf, const std::string &rule) {
        auto a = args(root_ / leaf);
        for (std::size_t k = 0; k + 1 < a.size(); ++k) {
            if (a[k] == "--extra-seeds") a[k + 1] = seeds.string();
        }
        a.insert(a.end(), {"--membership", mem, "--no-output-dedup"});
        if (!rule.empty()) a.insert(a.end(), {"--dsr-rule", rule});
        return dispatch(a);
    };
    const auto raw_n = run("raw_n", "summary-raw-n-v2");
    ASSERT_EQ(raw_n.code, 0) << raw_n.err;
    const auto n_eff = run("n_eff", "summary-n-eff-v1");
    ASSERT_EQ(n_eff.code, 0) << n_eff.err;
    const auto gr = gate(root_ / "raw_n");
    const auto ge = gate(root_ / "n_eff");
    EXPECT_EQ(gr["trials"]["dsr_rule"], "summary-raw-n-v2");
    EXPECT_EQ(ge["trials"]["dsr_rule"], "summary-n-eff-v1");
    EXPECT_EQ(ge["config"]["dsr_rule_requested"], "summary-n-eff-v1");
    EXPECT_EQ(ge["trials"]["dsr_clusters"], 0) << "a summary rule never runs accounting";
    EXPECT_EQ(ge["trials"]["dsr_fallback_reason"], "");
    const double n_raw = ge["trials"]["n_raw"].get<double>();
    const double n_eff_value = ge["trials"]["n_eff"].get<double>();
    ASSERT_GE(n_eff_value, 1.0);
    ASSERT_LT(n_eff_value, n_raw) << "the fixture must make the rules differ";

    // dsr_train is report-only: every other candidate column is identical, and since
    // n_eff <= n_raw lowers E[max SR], the pre-W0 DSR is never below the raw-N one.
    const auto rows_r = csv_rows(root_ / "raw_n" / "candidates.csv");
    const auto rows_e = csv_rows(root_ / "n_eff" / "candidates.csv");
    ASSERT_EQ(rows_r.size(), rows_e.size());
    ASSERT_GE(rows_r.size(), 2U);
    std::size_t dsr_col = 0;
    for (std::size_t c = 0; c < rows_r[0].size(); ++c) {
        if (rows_r[0][c] == "dsr_train") dsr_col = c;
    }
    ASSERT_GT(dsr_col, 0U);
    std::size_t compared = 0;
    std::size_t raised = 0;
    for (std::size_t r = 1; r < rows_r.size(); ++r) {
        ASSERT_GT(rows_r[r].size(), dsr_col);
        for (std::size_t c = 0; c < dsr_col; ++c) EXPECT_EQ(rows_r[r][c], rows_e[r][c]) << r;
        if (rows_r[r][3] != "1") continue; // unscored rows carry no DSR
        const double pre_w0 = std::stod(rows_e[r][dsr_col]);
        const double raw = std::stod(rows_r[r][dsr_col]);
        EXPECT_GE(pre_w0 + 1e-12, raw) << r;
        raised += pre_w0 > raw ? 1U : 0U;
        ++compared;
    }
    EXPECT_GT(compared, 0U);
    EXPECT_GT(raised, 0U) << "n_eff < n_raw must lower SR* and raise some pre-W0 DSR";
    // The DSR rule never changes selection: the same admitted set, in the same order.
    ASSERT_EQ(gr["admitted"].size(), ge["admitted"].size());
    for (std::size_t k = 0; k < gr["admitted"].size(); ++k) {
        EXPECT_EQ(gr["admitted"][k]["dsl"], ge["admitted"][k]["dsl"]);
        EXPECT_EQ(gr["admitted"][k]["p_by"], ge["admitted"][k]["p_by"]);
    }
    std::printf("[ImplMineRequiresMembership] dsr rules: n_raw=%.0f n_eff=%.4f, %zu scored rows "
                "compared, %zu raised (n-eff-v1 >= raw-n-v2)\n", n_raw, n_eff_value, compared,
                raised);

    auto bad = args(root_ / "bad");
    bad.insert(bad.end(), {"--membership", mem, "--dsr-rule", "raw"});
    const auto refused = dispatch(bad);
    EXPECT_EQ(refused.code, 2);
    EXPECT_NE(refused.err.find("--dsr-rule must be"), std::string::npos) << refused.err;
    EXPECT_EQ(atx::impl::mine::train_dsr_rule_label(atx::impl::mine::TrainDsrRule::SummaryNEffV1),
              "summary-n-eff-v1");
}

TEST(ImplMineRequiresMembership_Tags, FamilyAndThemeComeFromTheOrigin) {
    using atx::impl::mine::trial_family_of;
    using atx::impl::mine::trial_theme_of;
    EXPECT_EQ(trial_family_of("wq101:12"), "wq101");
    EXPECT_EQ(trial_family_of("lit:momentum_12_1+decay5"), "lit");
    EXPECT_EQ(trial_family_of("search"), "search");
    EXPECT_EQ(trial_theme_of("lit:momentum_12_1+decay5"), "lit:momentum_12_1");
    EXPECT_EQ(trial_theme_of("extra"), "extra");
}

// B-02 (impl) at the mine boundary.
TEST(ImplDelayGuard_Mine, DelayZeroNeedsAllowSameClose) {
    const std::vector<std::string> base{"atx-impl", "equity-mine", "--train-contexts", "a",
        "--validation-contexts", "b", "--train-start", "2015-01-01", "--validation-start",
        "2016-01-01", "--holdout-start", "2017-01-01", "--membership", "m.bin", "--delay", "0",
        "--out", (fs::temp_directory_path() / "atx_w0i0b_never_created").string()};
    const auto refused = dispatch(base);
    EXPECT_EQ(refused.code, 2);
    EXPECT_NE(refused.err.find("--allow-same-close"), std::string::npos) << refused.err;
    auto allowed = base;
    allowed.push_back("--allow-same-close");
    // Parsing now passes; the stage then fails on the missing inputs (exit 1, not 2).
    EXPECT_EQ(dispatch(allowed).code, 1);
    // Fix pass 1: as in the main parser, an optional boolean literal follows the flag.
    for (const std::string on : {"true", "1"}) {
        auto with_value = base;
        with_value.insert(with_value.end(), {"--allow-same-close", on});
        EXPECT_EQ(dispatch(with_value).code, 1) << on;
    }
    for (const std::string off : {"false", "0"}) {
        auto with_value = base;
        with_value.insert(with_value.end(), {"--allow-same-close", off});
        const auto outcome = dispatch(with_value);
        EXPECT_EQ(outcome.code, 2) << off;
        EXPECT_NE(outcome.err.find("--delay 0 fills at the signal close"), std::string::npos)
            << outcome.err;
    }
    // A following flag is not swallowed as the value; a non-literal is a stray token.
    auto then_flag = base;
    then_flag.insert(then_flag.begin() + 2, {"--allow-same-close", "--quiet"});
    EXPECT_EQ(dispatch(then_flag).code, 1);
    auto stray = base;
    stray.insert(stray.end(), {"--allow-same-close", "maybe"});
    const auto bad = dispatch(stray);
    EXPECT_EQ(bad.code, 2);
    EXPECT_NE(bad.err.find("unexpected argument 'maybe'"), std::string::npos) << bad.err;
}

} // namespace atx_test_w0_i0b_mine
