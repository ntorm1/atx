// Lane 9 equity-mine: end-to-end CLI over synthetic identified contexts.
// Publishes a fresh directory whose manifest (written last) hash-binds every
// output; refuses sessions at or after the seal; never touches an existing --out.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/panel.hpp"

#include "dispatch.hpp"
#include "panel_artifact.hpp"
#include "stage_equity_mine.hpp"

namespace atx_test_l9_realmine_cli {

namespace fs = std::filesystem;
using atx::engine::alpha::Panel;
using nlohmann::json;

constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::size_t kInst = 60;

// Weekday session keys from `first` (inclusive) for `n` sessions.
std::vector<std::int64_t> weekdays(std::int64_t first_ns, std::size_t n) {
    std::vector<std::int64_t> k;
    std::int64_t t = first_ns;
    while (k.size() < n) {
        const std::int64_t dow = ((t / kDayNs) + 4) % 7; // 1970-01-01 was a Thursday
        if (dow != 0 && dow != 6) k.push_back(t);
        t += kDayNs;
    }
    return k;
}

struct World {
    std::vector<std::int64_t> keys;
    std::vector<std::vector<double>> cols; // close open high low volume sig
};

// One global synthetic history; each context is a slice of it, so overlapping
// contexts agree bit-for-bit. r(d) = 0.004 * sig(d - 2) + 2% noise.
World make_world(std::size_t n) {
    World w;
    w.keys = weekdays(atx::impl::mine::parse_iso_date_ns("2015-01-01").value(), n);
    std::mt19937_64 rng(99);
    std::normal_distribution<double> z(0.0, 1.0);
    w.cols.assign(6, std::vector<double>(n * kInst));
    auto &close = w.cols[0], &open = w.cols[1], &high = w.cols[2], &low = w.cols[3],
         &vol = w.cols[4], &sig = w.cols[5];
    for (auto &v : sig) v = z(rng);
    std::vector<double> px(kInst, 40.0);
    for (std::size_t d = 0; d < n; ++d) {
        for (std::size_t i = 0; i < kInst; ++i) {
            const std::size_t c = d * kInst + i;
            if (d > 0) px[i] *= 1.0 + (d >= 2 ? 0.004 * sig[(d - 2) * kInst + i] : 0.0) +
                                0.02 * z(rng);
            close[c] = px[i];
            open[c] = px[i];
            high[c] = px[i] * 1.01;
            low[c] = px[i] * 0.99;
            vol[c] = 1.0e6;
        }
    }
    return w;
}

class EquityMineCli : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("atx_l9_mine_cli_" + std::to_string(stamp) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(root_);
        world_ = make_world(1000);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    // Write a context holding sessions [b, e) of the world.
    std::string context(const std::string &name, std::size_t b, std::size_t e) {
        const std::size_t D = e - b;
        std::vector<std::vector<double>> cols;
        for (const auto &c : world_.cols) {
            cols.emplace_back(c.begin() + static_cast<std::ptrdiff_t>(b * kInst),
                              c.begin() + static_cast<std::ptrdiff_t>(e * kInst));
        }
        auto panel = Panel::create(D, kInst, {"close", "open", "high", "low", "volume", "sig"},
                                   std::move(cols), std::vector<std::uint8_t>(D * kInst, 1));
        EXPECT_TRUE(panel.has_value());
        atx::impl::PanelIdentity id;
        id.instrument_namespace = std::string{atx::impl::kSpiderRockSecurityIdNamespace};
        id.session_keys.assign(world_.keys.begin() + static_cast<std::ptrdiff_t>(b),
                               world_.keys.begin() + static_cast<std::ptrdiff_t>(e));
        for (std::size_t i = 0; i < kInst; ++i) {
            id.instrument_ids.push_back(std::to_string(1000 + i));
            id.original_instrument_indices.push_back(i);
        }
        id.recipe = "synthetic=lane9\n";
        id.parents = {{"source", atx::core::sha256_hex(name).value()}};
        const auto path = (root_ / (name + ".bin")).string();
        auto w = atx::impl::write_panel_artifact(*panel, path, id);
        EXPECT_TRUE(w.has_value()) << (w ? "" : w.error().message());
        return path;
    }

    int run(std::vector<std::string> args, std::string &out, std::string &err) {
        args.insert(args.begin(), {"atx-impl", "equity-mine"});
        std::vector<char *> argv;
        for (auto &a : args) argv.push_back(a.data());
        std::ostringstream o, e;
        const int rc = atx::impl::dispatch(static_cast<int>(argv.size()), argv.data(), o, e);
        out = o.str();
        err = e.str();
        return rc;
    }

    std::vector<std::string> base_args(const fs::path &out) {
        // world sessions: 1000 weekdays from 2015-01-01 (~ through 2018-10).
        const std::string train = context("train", 0, 520);    // 2015 .. 2016-12
        const std::string val = context("val", 300, 780);      // warmup 2016 .. 2017-12
        const std::string hold = context("hold", 560, 1000);   // warmup 2017 .. 2018-10
        std::ofstream(root_ / "seeds.txt") << "# lane 9\nrank(sig)\nrank(close)\n"
                                              "-1 * rank(delta(close, 5))\nrank(volume)\n"
                                              "not a valid expression (\n";
        return {"--train-contexts", train,
                "--validation-contexts", val,
                "--holdout-contexts", hold,
                "--train-start", "2015-03-01",
                "--validation-start", "2017-01-01",
                "--holdout-start", "2018-01-01",
                "--holdout-end", "2019-01-01",
                "--extra-seeds", (root_ / "seeds.txt").string(),
                "--no-literature",
                "--no-search",
                "--min-names", "20",
                "--cost-bps", "1",
                "--n-boot", "200",
                "--threads", "2",
                // W0-I0b / D-12: as-of membership is the default and needs an
                // image; this fixture has none, so it opts into the legacy rule.
                "--membership-rule", "year-union-v1",
                "--out", out.string()};
    }

    fs::path root_;
    World world_;
};

TEST_F(EquityMineCli, PublishesHashBoundLibraryWithPlantedAlpha) {
    const fs::path out = root_ / "mine_out";
    std::string o, e;
    auto args = base_args(out);
    args.insert(args.end(), {"--holdout", "publish", "--holdout-prior-reads", "2"});
    const int rc = run(args, o, e);
    ASSERT_EQ(rc, 0) << e;
    EXPECT_NE(o.find("[atx-impl] stage=equity-mine"), std::string::npos) << o;
    EXPECT_NE(o.find("admitted="), std::string::npos);
    for (const char *f : {"candidates.csv", "validation.csv", "library.tsv", "holdout.csv",
                          "gate_report.json", "trial_registry.bin", "manifest.json"}) {
        EXPECT_TRUE(fs::exists(out / f)) << f;
    }
    std::ifstream mf(out / "manifest.json");
    const json m = json::parse(mf);
    ASSERT_EQ(m["files"].size(), 6u);
    for (const auto &f : m["files"]) {
        const auto sha = atx::core::sha256_file((out / f["path"].get<std::string>()).string());
        ASSERT_TRUE(sha.has_value());
        EXPECT_EQ(*sha, f["sha256"].get<std::string>()) << f["path"];
    }
    std::ifstream gf(out / "gate_report.json");
    const json g = json::parse(gf);
    EXPECT_EQ(g["counts"]["seeds_invalid"].get<int>(), 1);
    EXPECT_GE(g["counts"]["admitted"].get<int>(), 1);
    EXPECT_EQ(g["train"]["overlap_mismatch_cells"].get<int>(), 0);
    EXPECT_EQ(g["validation"]["overlap_mismatch_cells"].get<int>(), 0);
    EXPECT_EQ(g["train"]["membership_rule"].get<std::string>(), "context-year-union-not-as-of");
    EXPECT_GE(g["trials"]["n_raw"].get<int>(), 3);
    ASSERT_FALSE(g["admitted"].empty());
    EXPECT_EQ(g["admitted"][0]["dsl"].get<std::string>(), "rank(sig)");
    EXPECT_GT(g["admitted"][0]["holdout"]["sharpe_net"].get<double>(), 1.0);
    EXPECT_TRUE(g["holdout"]["evaluated"].get<bool>());
    EXPECT_EQ(g["holdout"]["status"].get<std::string>(), "reused");
    EXPECT_EQ(g["holdout"]["prior_reads"].get<int>(), 2);
    for (const char *role : {"train", "validation", "holdout"}) {
        EXPECT_TRUE(g[role]["return_guard"]["enabled"].get<bool>()) << role;
        EXPECT_EQ(g[role]["return_guard"]["excluded_one_day_cells_span"].get<int>(), 0) << role;
    }
    std::ifstream cands(out / "candidates.csv");
    std::string cand_header;
    std::getline(cands, cand_header);
    EXPECT_NE(cand_header.find("train_sharpe_gross,train_ic_h1"), std::string::npos) << cand_header;
    std::ifstream lib(out / "library.tsv");
    std::string header, first;
    std::getline(lib, header);
    std::getline(lib, first);
    EXPECT_NE(first.find("rank(sig)"), std::string::npos);
}

TEST_F(EquityMineCli, SmoothWindowsAddDecayedVariantsAsTrials) {
    const fs::path out = root_ / "smooth";
    auto args = base_args(out);
    args.insert(args.end(), {"--smooth-windows", "5;10"});
    std::string o, e;
    ASSERT_EQ(run(args, o, e), 0) << e;
    std::ifstream gf(out / "gate_report.json");
    const json g = json::parse(gf);
    EXPECT_EQ(g["counts"]["seeds"].get<int>(), 15);        // 5 base lines x (1 + 2 windows)
    EXPECT_EQ(g["counts"]["seeds_invalid"].get<int>(), 3); // the bad line in every form
    // 4 valid base seeds x 3 forms = 12 scored candidates. W0-A0 / A-01 (average rank
    // ties): the world's volume is a constant 1e6, so rank(volume) and its two decayed
    // forms are all-tie cross-sections and now degenerate (ordinal ties used to break
    // them by index into a spurious signal); they are never registered as trials.
    EXPECT_EQ(g["counts"]["degenerate"].get<int>(), 3);
    EXPECT_EQ(g["trials"]["n_raw"].get<int>(), 9);
    std::string o2, e2;
    auto bad = args;
    bad.back() = (root_ / "smooth_bad").string();
    bad.insert(bad.end(), {"--smooth-windows", "1"});
    EXPECT_EQ(run(bad, o2, e2), 2);
}

TEST_F(EquityMineCli, RefusesContextsAtOrAfterTheSeal) {
    const fs::path out = root_ / "sealed";
    auto args = base_args(out);
    args.insert(args.end(), {"--seal", "2018-06-01", "--holdout", "publish"});
    args[std::find(args.begin(), args.end(), "--holdout-end") - args.begin() + 1] = "2018-05-01";
    std::string o, e;
    EXPECT_EQ(run(args, o, e), 1);
    EXPECT_NE(e.find("seal"), std::string::npos) << e;
    EXPECT_FALSE(fs::exists(out / "manifest.json"));
}

TEST_F(EquityMineCli, HoldoutOffByDefaultNeverLoadsHoldoutContexts) {
    const fs::path out = root_ / "no_holdout";
    auto args = base_args(out);
    // Point the holdout at a file that does not exist: off must never open it.
    args[std::find(args.begin(), args.end(), "--holdout-contexts") - args.begin() + 1] =
        (root_ / "missing_holdout.bin").string();
    std::string o, e;
    ASSERT_EQ(run(args, o, e), 0) << e;
    EXPECT_FALSE(fs::exists(out / "holdout.csv"));
    std::ifstream gf(out / "gate_report.json");
    const json g = json::parse(gf);
    EXPECT_FALSE(g["holdout"]["evaluated"].get<bool>());
    EXPECT_EQ(g["holdout"]["mode"].get<std::string>(), "off");
    EXPECT_FALSE(g["holdout"].contains("contexts"));
    for (const auto &a : g["admitted"]) EXPECT_FALSE(a.contains("holdout"));
    std::ifstream mf(out / "manifest.json");
    const json m = json::parse(mf);
    EXPECT_EQ(m["files"].size(), 5u);
    EXPECT_TRUE(m["inputs"]["holdout"].empty());

    // publish without holdout contexts / end is a usage error.
    std::string o2, e2;
    EXPECT_EQ(run({"--train-contexts", "a", "--validation-contexts", "b", "--train-start",
                   "2015-01-01", "--validation-start", "2016-01-01", "--holdout-start",
                   "2017-01-01", "--holdout", "publish", "--out", (root_ / "p").string()},
                  o2, e2),
              2);
    EXPECT_NE(e2.find("--holdout publish requires"), std::string::npos) << e2;
}

TEST_F(EquityMineCli, RefusesARoleSpanReachingTheNextRole) {
    const fs::path out = root_ / "leak";
    const auto base = base_args(out);
    auto args = base;
    // Sessions [0, 600) run past --validation-start 2017-01-01 (about index 522).
    args[std::find(args.begin(), args.end(), "--train-contexts") - args.begin() + 1] =
        context("train_long", 0, 600);
    std::string o, e;
    EXPECT_EQ(run(args, o, e), 1);
    EXPECT_NE(e.find("leak"), std::string::npos) << e;
    EXPECT_NE(e.find("train"), std::string::npos) << e;
    EXPECT_FALSE(fs::exists(out / "manifest.json"));

    const fs::path out2 = root_ / "leak_val";
    auto args2 = base;
    args2.back() = out2.string();
    // Validation sessions [300, 800) run past --holdout-start 2018-01-01.
    args2[std::find(args2.begin(), args2.end(), "--validation-contexts") - args2.begin() + 1] =
        context("val_long", 300, 800);
    std::string o3, e3;
    EXPECT_EQ(run(args2, o3, e3), 1);
    EXPECT_NE(e3.find("validation"), std::string::npos) << e3;
    EXPECT_FALSE(fs::exists(out2 / "manifest.json"));
}

TEST_F(EquityMineCli, NeverWritesIntoAnExistingOut) {
    const fs::path out = root_ / "exists";
    fs::create_directories(out);
    std::string o, e;
    EXPECT_EQ(run(base_args(out), o, e), 1);
    EXPECT_TRUE(fs::is_empty(out));
}

TEST_F(EquityMineCli, RejectsUnknownFlagAndMissingRequired) {
    std::string o, e;
    EXPECT_EQ(run({"--bogus", "1"}, o, e), 2);
    EXPECT_EQ(run({"--out", (root_ / "x").string()}, o, e), 2);
    EXPECT_NE(e.find("required"), std::string::npos);
}

} // namespace atx_test_l9_realmine_cli
