// Checkpoint 15 / T2, design §9.2: `ConfigEquityUniverse` (C1-C4) and
// `StageEquityUniverse` (S1-S14). Every fixture below is SYNTHETIC — a handful of
// invented ids on consecutive UTC days written through the real SegmentBuilder so the
// stage exercises genuine `.seg` attachment, manifest binding and ledger appends.
// Nothing here is market data, no number below is evidence about any security, and
// no test asserts a real-data quantity (§9.4).
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"
#include "atx/tsdb/builder.hpp"
#include "config.hpp"
#include "stage_equity_universe.hpp"
#include "trial_ledger.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
namespace data = atx::engine::data;
using Json = nlohmann::json;
using atx::core::ErrorCode;
using atx::engine::data::detail::date_to_nanos;

constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// One synthetic name: constant bars, so every window is 63/63 valid and the §3.5
// median is the bar's own dollar volume. dv: A 50000, B 40000, C 30000, D 50000 but
// close 0.5 fails the R15-5 floor, E 500 with vendor shares and gics missing (R15-6)
// and an archive-scale id (DR15-1).
struct Name {
    atx::i64 id;
    double close;
    double volume;
    double shares;
    double gics;
};
constexpr std::array<Name, 5> kNames{{{101, 10.0, 5000.0, 100.0, 4510.0},
                                      {202, 20.0, 2000.0, 100.0, 4510.0},
                                      {303, 30.0, 1000.0, 100.0, 2010.0},
                                      {404, 0.5, 100000.0, 50.0, 4510.0},
                                      {1001001001070LL, 5.0, 100.0, kNaN, kNaN}}};
constexpr const char *kFirstA = "2012-10-28"; // 65 sessions to 2012-12-31 (>= 63 warmup)
constexpr atx::usize kSessionsA = 65;
constexpr const char *kFirstB = "2013-01-01"; // 32 sessions to 2013-02-01
constexpr atx::usize kSessionsB = 32;
constexpr const char *kRankStart = "2012-12-01";
constexpr const char *kRankEnd = "2013-01-31"; // rank 2012-12-31 and 2013-01-31: 2 rebalances

std::string contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> split_lines(const std::string &blob) {
    std::vector<std::string> lines;
    for (std::size_t start = 0; start < blob.size();) {
        const auto stop = blob.find('\n', start);
        if (stop == std::string::npos) break;
        lines.push_back(blob.substr(start, stop - start));
        start = stop + 1;
    }
    return lines;
}

std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == ',') { fields.push_back(current); current.clear(); continue; }
        current += c;
    }
    fields.push_back(current);
    return fields;
}

std::string shortest(double value) {
    std::array<char, 64> bytes{};
    const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    return {bytes.data(), result.ptr};
}

atx::u32 orats_field(std::string_view name) {
    for (atx::usize i = 0; i < data::kOratsFields.size(); ++i) {
        if (data::kOratsFields[i] == name) return static_cast<atx::u32>(i);
    }
    return 0xFFFFFFFFu;
}

std::string date_of(atx::i64 key) {
    const auto civil = data::pit_civil_of(key);
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02u", static_cast<int>(civil.year),
                  static_cast<unsigned>(civil.month), static_cast<unsigned>(civil.day));
    return std::string(buffer.data());
}

// One segment directory of consecutive UTC days, plus the two manifests the stage binds.
struct DirSpec {
    fs::path dir;
    fs::path preparation;
    atx::i64 first_key{};
    atx::usize sessions{};
    std::span<const Name> names;
    std::map<std::string, atx::i64> axis_overrides; // filename -> axis key (S1b, S2, S8)
    std::set<std::string> two_row;                  // filenames written with a 2-entry axis
    std::map<std::string, std::pair<int, int>> daily; // date -> {duplicate keys, rejected}
};

class StageEquityUniverse : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        std::error_code ec;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_equity_universe_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root, ec)) return;
        }
        FAIL() << "Cannot reserve fixture root";
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_equity_universe_")) {
            fs::remove_all(resolved, ec);
        }
    }

    [[nodiscard]] fs::path ledger() const { return root / "trial-ledger.jsonl"; }
    [[nodiscard]] fs::path dir_a() const { return root / "segA"; }
    [[nodiscard]] fs::path dir_b() const { return root / "segB"; }
    [[nodiscard]] fs::path prep_a() const { return root / "prepA" / "manifest.json"; }
    [[nodiscard]] fs::path prep_b() const { return root / "prepB" / "manifest.json"; }

    static void write_segment(const fs::path &path, atx::i64 axis_key,
                              std::span<const Name> names, bool two_row = false) {
        std::vector<std::string> fields;
        for (const auto field : data::kOratsFields) fields.emplace_back(field);
        std::vector<std::string> symbols;
        for (const auto &name : names) symbols.push_back(std::to_string(name.id));
        std::vector<atx::i64> axis{axis_key};
        if (two_row) axis.push_back(axis_key + kDay);
        atx::tsdb::SegmentBuilder builder(fields, symbols, axis);
        for (atx::usize j = 0; j < names.size(); ++j) {
            const auto inst = static_cast<atx::u32>(j);
            builder.set(orats_field("close"), 0, inst, names[j].close);
            builder.set(orats_field("volume"), 0, inst, names[j].volume);
            builder.set(orats_field("shares"), 0, inst, names[j].shares);
            builder.set(orats_field("gics"), 0, inst, names[j].gics);
        }
        const auto written = builder.write(path.string(), 0);
        ASSERT_TRUE(written.has_value()) << written.error().message();
    }

    // The preparation manifest FIRST (the ingestion receipt binds its digest), then
    // the ingestion receipt over whatever `.seg` files the directory holds now.
    static void write_manifests(const fs::path &dir, const fs::path &preparation,
                                atx::i64 first_key, atx::usize sessions,
                                const std::map<std::string, std::pair<int, int>> &daily) {
        fs::create_directories(preparation.parent_path());
        Json counts = Json::array();
        for (atx::usize s = 0; s < sessions; ++s) {
            const std::string date = date_of(first_key + static_cast<atx::i64>(s) * kDay);
            int duplicates = 0;
            int rejected = 0;
            const auto found = daily.find(date);
            if (found != daily.end()) {
                duplicates = found->second.first;
                rejected = found->second.second;
            }
            counts.push_back(Json{{"date", date}, {"accepted", 5}, {"selected", 5 + rejected},
                                  {"duplicate_positive_keys", duplicates},
                                  {"rejected", rejected}});
        }
        {
            std::ofstream out(preparation, std::ios::binary);
            out << Json{{"policy", Json::array({"synthetic fixture"})},
                        {"policy_version", "tickerhistory-qa-v1"}, {"status", "complete"},
                        {"daily_counts", counts}}.dump(2) << '\n';
        }
        const auto prep_sha = atx::core::sha256_file(preparation.string());
        ASSERT_TRUE(prep_sha.has_value());
        std::vector<std::string> names;
        for (const auto &entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (name.ends_with(".seg")) names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        Json segments = Json::array();
        for (const auto &name : names) {
            const auto sha = atx::core::sha256_file((dir / name).string());
            ASSERT_TRUE(sha.has_value());
            segments.push_back(Json{{"filename", name}, {"sha256", *sha},
                                    {"size_bytes", fs::file_size(dir / name)}});
        }
        const Json recipe{{"version", "tickerhistory-native-load-v1"},
                          {"executable_sha256", std::string(64, '9')}};
        const Json receipt{{"schema", "atx-ingestion-v1"}, {"status", "complete"},
            {"input", Json{{"filename", "fixture.zip"}, {"sha256", std::string(64, '8')},
                           {"size_bytes", 1}}},
            {"preparation", Json{{"manifest_sha256", *prep_sha},
                                 {"original_source_sha256", std::string(64, '7')},
                                 {"accepted_sha256", std::string(64, '8')},
                                 {"policy_version", "tickerhistory-qa-v1"}}},
            {"recipe", recipe.dump()}, {"segments", segments},
            {"historical_vintages_verified", false},
            {"binding_scope", "local-content-derivation-not-authentication"}};
        std::ofstream out(dir / "_ingestion.manifest.json", std::ios::binary);
        out << receipt.dump(2) << '\n';
    }

    static void write_dir(const DirSpec &spec) {
        fs::create_directories(spec.dir);
        for (atx::usize s = 0; s < spec.sessions; ++s) {
            const atx::i64 key = spec.first_key + static_cast<atx::i64>(s) * kDay;
            const std::string name = date_of(key) + ".seg";
            atx::i64 axis = key;
            const auto override_at = spec.axis_overrides.find(name);
            if (override_at != spec.axis_overrides.end()) axis = override_at->second;
            ASSERT_NO_FATAL_FAILURE(write_segment(spec.dir / name, axis, spec.names,
                                                  spec.two_row.contains(name)));
        }
        ASSERT_NO_FATAL_FAILURE(write_manifests(spec.dir, spec.preparation, spec.first_key,
                                                spec.sessions, spec.daily));
    }

    [[nodiscard]] DirSpec spec_a() const {
        DirSpec spec;
        spec.dir = dir_a();
        spec.preparation = prep_a();
        spec.first_key = *date_to_nanos(kFirstA);
        spec.sessions = kSessionsA;
        spec.names = std::span<const Name>(kNames.data(), kNames.size());
        spec.daily = {{"2012-11-05", {2, 5}}, {"2012-12-10", {1, 3}}};
        return spec;
    }
    [[nodiscard]] DirSpec spec_b() const {
        DirSpec spec;
        spec.dir = dir_b();
        spec.preparation = prep_b();
        spec.first_key = *date_to_nanos(kFirstB);
        spec.sessions = kSessionsB;
        spec.names = std::span<const Name>(kNames.data(), 4); // E stops trading after A
        spec.daily = {{"2013-01-15", {0, 4}}};
        return spec;
    }
    void build_default_fixture() {
        ASSERT_NO_FATAL_FAILURE(write_dir(spec_a()));
        ASSERT_NO_FATAL_FAILURE(write_dir(spec_b()));
    }

    [[nodiscard]] static std::string join(const std::vector<fs::path> &paths) {
        std::string out;
        for (const auto &path : paths) {
            if (!out.empty()) out += ';';
            out += path.generic_string();
        }
        return out;
    }

    [[nodiscard]] impl::RunConfig universe_config(const std::string &out,
                                                  const char *rank_start = kRankStart,
                                                  const char *rank_end = kRankEnd) const {
        impl::RunConfig cfg;
        cfg.subcommand = "equity-universe";
        cfg.equity_segments_dirs = join({dir_a(), dir_b()});
        cfg.equity_preparation_manifests = join({prep_a(), prep_b()});
        cfg.out = (root / out).string();
        cfg.equity_rank_start = rank_start;
        cfg.equity_rank_end = rank_end;
        cfg.equity_trial_ledger = ledger().string();
        return cfg;
    }

    [[nodiscard]] std::vector<Json> ledger_lines() const {
        std::vector<Json> lines;
        for (const auto &line : split_lines(contents(ledger()))) lines.push_back(Json::parse(line));
        return lines;
    }
};

auto parse(std::vector<std::string> arguments) {
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    return impl::parse_args(static_cast<int>(argv.size()), argv.data());
}
} // namespace

// --- §9.2 C1-C4 -----------------------------------------------------------

TEST(ConfigEquityUniverse, Subcommand_IsFourteenthAndLast) {
    ASSERT_EQ(impl::kSubcommands.size(), 14U);
    EXPECT_EQ(impl::kSubcommands.back(), "equity-universe");
    EXPECT_EQ(impl::kSubcommands[12], "equity-ic");
    const auto parsed = parse({"atx-impl", "equity-universe", "--segments-dirs", "a;b",
        "--preparation-manifests", "m;n", "--out", "u", "--rank-start", "2012-12-31",
        "--rank-end", "2019-11-29", "--max-working-bytes", "3000000000", "--trial-ledger", "l"});
    ASSERT_TRUE(parsed) << parsed.error().message();
    EXPECT_EQ(parsed->subcommand, "equity-universe");
    EXPECT_EQ(parsed->equity_segments_dirs, "a;b");
    EXPECT_EQ(parsed->equity_preparation_manifests, "m;n");
    EXPECT_EQ(parsed->equity_rank_start, "2012-12-31");
    EXPECT_EQ(parsed->equity_rank_end, "2019-11-29");
    EXPECT_EQ(parsed->equity_trial_ledger, "l");
    EXPECT_TRUE(parsed->set_flags.contains("segments-dirs"));
    EXPECT_FALSE(parsed->set_flags.contains("config"));
    // Pre-existing subcommands gain no value in the four new fields.
    const auto other = parse({"atx-impl", "equity-ic", "--panel", "p", "--out", "o"});
    ASSERT_TRUE(other) << other.error().message();
    EXPECT_TRUE(other->equity_segments_dirs.empty());
    EXPECT_TRUE(other->equity_rank_end.empty());
}

TEST(ConfigEquityUniverse, Flags_OutsideAllowList_Rejected) {
    for (const std::string flag : {"top-n", "band", "config", "panel", "rank-key", "cadence"}) {
        impl::RunConfig cfg;
        cfg.subcommand = "equity-universe";
        cfg.equity_segments_dirs = "a";
        cfg.equity_preparation_manifests = "m";
        cfg.out = "u";
        cfg.equity_rank_start = "2012-12-31";
        cfg.equity_rank_end = "2019-11-29";
        cfg.set_flags = {flag};
        const auto result = impl::run_equity_universe(cfg);
        ASSERT_FALSE(result) << "equity-universe accepted --" << flag;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(result.error().message().find("unsupported flag --" + flag), std::string::npos);
    }
    impl::RunConfig programmatic;
    programmatic.subcommand = "equity-universe";
    programmatic.config_file = "book.cfg";
    const auto rejected = impl::run_equity_universe(programmatic);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
}

TEST(ConfigEquityUniverse, Flags_EmptyOrDashValues_Rejected) {
    for (const std::string flag : {"--segments-dirs", "--preparation-manifests", "--rank-start",
                                   "--rank-end"}) {
        EXPECT_FALSE(parse({"atx-impl", "equity-universe", flag})) << flag;
        EXPECT_FALSE(parse({"atx-impl", "equity-universe", flag, ""})) << flag;
        EXPECT_FALSE(parse({"atx-impl", "equity-universe", flag, "--out", "u"})) << flag;
    }
}

TEST(ConfigEquityUniverse, RankEnd_After2019_Rejected) {
    impl::RunConfig cfg;
    cfg.subcommand = "equity-universe";
    cfg.equity_segments_dirs = "nonexistent-dir";
    cfg.equity_preparation_manifests = "nonexistent-manifest";
    cfg.out = "u";
    cfg.equity_rank_start = "2012-12-31";
    cfg.equity_rank_end = "2020-01-02";
    const auto result = impl::run_equity_universe(cfg);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(result.error().message().find("2020-01-01"), std::string::npos);
    // A malformed or reversed window is the same refusal, before any path is touched.
    cfg.equity_rank_end = "2019-1-1";
    EXPECT_FALSE(impl::run_equity_universe(cfg));
    cfg.equity_rank_start = "2019-12-31";
    cfg.equity_rank_end = "2019-11-29";
    EXPECT_FALSE(impl::run_equity_universe(cfg));
}

// --- §9.2 S1-S14 ----------------------------------------------------------

TEST_F(StageEquityUniverse, SealedSegmentInDir_RefusesNotSkips) {
    DirSpec sealed;
    sealed.dir = root / "sealed";
    sealed.preparation = root / "sealed-prep" / "manifest.json";
    sealed.first_key = *date_to_nanos("2019-12-30");
    sealed.sessions = 4; // 2019-12-30, 2019-12-31, 2020-01-01, 2020-01-02
    sealed.names = std::span<const Name>(kNames.data(), kNames.size());
    ASSERT_NO_FATAL_FAILURE(write_dir(sealed));
    impl::RunConfig cfg = universe_config("out_sealed", "2019-12-01", "2019-12-31");
    cfg.equity_segments_dirs = sealed.dir.generic_string();
    cfg.equity_preparation_manifests = sealed.preparation.generic_string();
    const auto result = impl::run_equity_universe(cfg);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    // Directory enumeration order is not pinned, so either sealed file may be named.
    EXPECT_NE(result.error().message().find("2020-01-0"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "out_sealed"));
    EXPECT_FALSE(fs::exists(ledger()));
}

TEST_F(StageEquityUniverse, SealedAxisBehindValidName_RejectedByAxisCheck) {
    // 96 sessions 2019-09-27..2019-12-31; the last file's AXIS says 2020-01-02. The
    // filename passes the seal, so the axis==filename check (§5.4) must catch it — the
    // engine's own PermissionDenied is unreachable through the stage.
    DirSpec spec;
    spec.dir = root / "seg2019";
    spec.preparation = root / "prep2019" / "manifest.json";
    spec.first_key = *date_to_nanos("2019-09-27");
    spec.sessions = 96;
    spec.names = std::span<const Name>(kNames.data(), kNames.size());
    spec.axis_overrides = {{"2019-12-31.seg", *date_to_nanos("2020-01-02")}};
    ASSERT_NO_FATAL_FAILURE(write_dir(spec));
    impl::RunConfig cfg = universe_config("out_axis", "2019-11-01", "2019-11-30");
    cfg.equity_segments_dirs = spec.dir.generic_string();
    cfg.equity_preparation_manifests = spec.preparation.generic_string();
    const auto result = impl::run_equity_universe(cfg);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(result.error().message().find("2019-12-31.seg"), std::string::npos);
    EXPECT_FALSE(fs::exists(root / "out_axis" / "manifest.json"));
}

TEST_F(StageEquityUniverse, MisnamedOrMultiDateSegment_Rejected) {
    {
        DirSpec b = spec_b();
        b.axis_overrides = {{"2013-01-02.seg", *date_to_nanos("2013-01-03")}};
        ASSERT_NO_FATAL_FAILURE(write_dir(spec_a()));
        ASSERT_NO_FATAL_FAILURE(write_dir(b));
        const auto result = impl::run_equity_universe(universe_config("out_misnamed"));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(result.error().message().find("2013-01-02.seg"), std::string::npos);
    }
    {
        fs::remove_all(dir_b());
        DirSpec b = spec_b();
        b.two_row = {"2013-01-10.seg"};
        ASSERT_NO_FATAL_FAILURE(write_dir(b));
        const auto result = impl::run_equity_universe(universe_config("out_two_row"));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_NE(result.error().message().find("2013-01-10.seg"), std::string::npos);
    }
}

TEST_F(StageEquityUniverse, FreshOut_ReservationDiagnostics) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    fs::create_directories(root / "taken");
    const auto exists = impl::run_equity_universe(universe_config("taken"));
    ASSERT_FALSE(exists);
    EXPECT_EQ(exists.error().code(), ErrorCode::AlreadyExists);
    // The M-6 fix: a parent that cannot be created is an I/O error, not "exists".
    { std::ofstream blocker(root / "blocker.txt", std::ios::binary); blocker << "x"; }
    impl::RunConfig cfg = universe_config("unused");
    cfg.out = (root / "blocker.txt" / "out").string();
    const auto unwritable = impl::run_equity_universe(cfg);
    ASSERT_FALSE(unwritable);
    EXPECT_EQ(unwritable.error().code(), ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(ledger())) << "no reservation, no ledger line";
}

TEST_F(StageEquityUniverse, PreparationManifest_MustMatchIngestionBinding) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    impl::RunConfig swapped = universe_config("out_swapped");
    swapped.equity_preparation_manifests = join({prep_b(), prep_a()});
    const auto positional = impl::run_equity_universe(swapped);
    ASSERT_FALSE(positional);
    EXPECT_EQ(positional.error().code(), ErrorCode::InvalidArgument);

    impl::RunConfig fewer = universe_config("out_fewer");
    fewer.equity_preparation_manifests = prep_a().generic_string();
    const auto mismatch = impl::run_equity_universe(fewer);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code(), ErrorCode::InvalidArgument);

    { std::ofstream out(prep_b(), std::ios::binary | std::ios::app); out << "\n"; }
    const auto edited = impl::run_equity_universe(universe_config("out_edited"));
    ASSERT_FALSE(edited);
    EXPECT_EQ(edited.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(ledger()));
}

TEST_F(StageEquityUniverse, Run_SyntheticTwoDirs_WritesAllFilesAndPins) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    const auto result = impl::run_equity_universe(universe_config("out1"));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const fs::path out = root / "out1";
    for (const char *name : {"request.json", "seal.json", "membership.csv", "churn.csv",
                             "coverage_by_year.csv", "union_by_year.csv", "delisting.csv",
                             "survivorship.json", "membership.bin", "manifest.json"}) {
        EXPECT_TRUE(fs::exists(out / name)) << name;
        EXPECT_FALSE(fs::exists(out / (std::string(name) + ".partial"))) << name;
    }
    EXPECT_FALSE(fs::exists(out / ".pending"));
    EXPECT_FALSE(fs::exists(out / "failure.json"));

    // manifest.json pins every attached segment SHA (§5.9) and every published file.
    const auto manifest = Json::parse(contents(out / "manifest.json"));
    EXPECT_EQ(manifest.at("status"), "complete");
    EXPECT_EQ(manifest.at("rebalance_count"), 2);
    EXPECT_EQ(manifest.at("sessions"), kSessionsA + kSessionsB);
    EXPECT_EQ(manifest.at("source_ids"), 5);
    std::map<std::string, std::string> pinned;
    atx::usize segment_parents = 0;
    for (const auto &parent : manifest.at("parents")) {
        if (parent.at("role") == "segment") {
            ++segment_parents;
            pinned[parent.at("filename").get<std::string>()] = parent.at("sha256");
        }
    }
    EXPECT_EQ(segment_parents, kSessionsA + kSessionsB);
    for (const fs::path &dir : {dir_a(), dir_b()}) {
        for (const auto &entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (!name.ends_with(".seg")) continue;
            ASSERT_TRUE(pinned.contains(name)) << name;
            EXPECT_EQ(pinned.at(name), *atx::core::sha256_file(entry.path().string())) << name;
        }
    }
    for (const auto &file : manifest.at("files")) {
        const auto name = file.at("filename").get<std::string>();
        EXPECT_EQ(file.at("sha256"), *atx::core::sha256_file((out / name).string())) << name;
    }
    EXPECT_EQ(manifest.at("design_note").at("sha256"),
              std::string(impl::kEquityUniverseDesignNoteSha256));
    EXPECT_EQ(manifest.at("trial_ledger").at("trial_count_declared"), 0);
    EXPECT_TRUE(manifest.at("runtime").at("peak_working_set_bytes").is_null());
    EXPECT_GE(manifest.at("runtime_seconds").get<double>(), 0.0);
    EXPECT_EQ(manifest.at("loader_executables").at("sha256").size(), 1U);

    // churn.csv: one_way_turnover equals (adds+drops)/(2*top_n) recomputed (T3 pin 7).
    const auto churn = split_lines(contents(out / "churn.csv"));
    ASSERT_EQ(churn.size(), 1U + 2U * 6U);
    for (atx::usize i = 1; i < churn.size(); ++i) {
        const auto f = split_csv(churn[i]);
        ASSERT_EQ(f.size(), 10U) << churn[i];
        const double moved = std::stod(f[4]) + std::stod(f[5]) + std::stod(f[6]);
        EXPECT_EQ(f[9], shortest(moved / (2.0 * std::stod(f[2])))) << churn[i];
    }
    // coverage_by_year.csv: the three preparation columns equal the fixture sums.
    const auto coverage = split_lines(contents(out / "coverage_by_year.csv"));
    ASSERT_EQ(coverage.size(), 3U);
    const auto header = split_csv(coverage[0]);
    ASSERT_EQ(header.size(), 9U + 3U * 6U);
    EXPECT_EQ(header[2], "dates_with_quarantined_duplicates");
    EXPECT_EQ(header[9], "members_median_t1000_b0");
    EXPECT_EQ(header[15], "nonmissing_fraction_median_t1000_b0");
    EXPECT_EQ(header[21], "gics_missing_members_median_t1000_b0");
    EXPECT_EQ(header[26], "gics_missing_members_median_t3000_b1000");
    const auto y2012 = split_csv(coverage[1]);
    const auto y2013 = split_csv(coverage[2]);
    ASSERT_EQ(y2012.size(), header.size());
    EXPECT_EQ(y2012[0], "2012");
    EXPECT_EQ(y2012[1], "65");
    EXPECT_EQ(y2012[2], "2"); // 2012-11-05 and 2012-12-10 carry quarantined duplicates
    EXPECT_EQ(y2012[3], "3");
    EXPECT_EQ(y2012[4], "8");
    EXPECT_EQ(y2012[5], "5");
    EXPECT_EQ(y2012[6], "5");
    EXPECT_EQ(y2012[7], "1"); // the 2012-12-31 rank session (engine: rank-session year)
    EXPECT_EQ(y2012[8], "4");
    EXPECT_EQ(y2012[9], "4");
    EXPECT_EQ(y2012[15], "1");
    EXPECT_EQ(y2012[21], "1"); // E carries no gics
    EXPECT_EQ(y2013[0], "2013");
    EXPECT_EQ(y2013[1], "32");
    EXPECT_EQ(y2013[2], "0");
    EXPECT_EQ(y2013[3], "0");
    EXPECT_EQ(y2013[4], "4");
    EXPECT_EQ(y2013[5], "4");
    EXPECT_EQ(y2013[7], "1");
    EXPECT_EQ(y2013[8], "3");
    EXPECT_EQ(y2013[21], "0");
    // union_by_year.csv (§4.4): the effective year 2013 only; 4 distinct in every cut.
    const auto unions = split_lines(contents(out / "union_by_year.csv"));
    ASSERT_EQ(unions.size(), 2U);
    EXPECT_EQ(unions[0], "year,distinct_t1000_b0,distinct_t1000_b1000,distinct_t2000_b0,"
                         "distinct_t2000_b1000,distinct_t3000_b0,distinct_t3000_b1000,"
                         "cumulative_t1000_b0,cumulative_t1000_b1000,cumulative_t2000_b0,"
                         "cumulative_t2000_b1000,cumulative_t3000_b0,cumulative_t3000_b1000");
    EXPECT_EQ(unions[1], "2013,4,4,4,4,4,4,4,4,4,4,4,4");
    // survivorship.json (§4.6): E's last bar precedes the window end; per-year 2013 only.
    const auto survivorship = Json::parse(contents(out / "survivorship.json"));
    EXPECT_EQ(survivorship.at("window_end"), "2013-02-01");
    ASSERT_EQ(survivorship.at("cuts").size(), 6U);
    const auto &cut0 = survivorship.at("cuts").at(0);
    EXPECT_EQ(cut0.at("top_n"), 1000);
    EXPECT_EQ(cut0.at("band"), "0.00");
    EXPECT_EQ(cut0.at("ever_members"), 4);
    EXPECT_EQ(cut0.at("ended_before_window_end"), 1);
    EXPECT_EQ(cut0.at("censored"), 3);
    EXPECT_DOUBLE_EQ(cut0.at("fraction_ended").get<double>(), 0.25);
    ASSERT_EQ(cut0.at("per_year").size(), 1U);
    EXPECT_EQ(cut0.at("per_year").at(0).at("year"), 2013);
    EXPECT_EQ(cut0.at("per_year").at(0).at("members_at_first_rebalance"), 4);
    EXPECT_EQ(cut0.at("per_year").at(0).at("exited_within_year"), 1);
    EXPECT_NE(survivorship.at("caveat").get<std::string>().find("LOWER BOUNDS"),
              std::string::npos);
    // seal.json and request.json (§4.8, T3 pin 9).
    const auto seal = Json::parse(contents(out / "seal.json"));
    EXPECT_EQ(seal.at("policy"), "RefuseAtOrAfterValidationBeginV1");
    EXPECT_EQ(seal.at("segments_refused"), 0);
    EXPECT_EQ(seal.at("latest_attached"), "2013-02-01");
    EXPECT_EQ(seal.at("validation_begin"), "2020-01-01");
    const auto request = Json::parse(contents(out / "request.json"));
    EXPECT_EQ(request.at("rebalance_count"), 2);
    EXPECT_EQ(request.at("rank_session_first"), "2012-12-31");
    EXPECT_EQ(request.at("rank_session_last"), "2013-01-31");
    EXPECT_EQ(request.at("config").at("top_n"), Json::array({1000, 2000, 3000}));
    EXPECT_EQ(request.at("config").at("band"), Json::array({"0.00", "0.10"}));
    EXPECT_EQ(request.at("trial_ledger").at("trial_id"),
              "iteration15-point-in-time-universe-0001");
    EXPECT_EQ(request.at("segments").size(), kSessionsA + kSessionsB);
    ASSERT_EQ(request.at("segments_dirs").size(), 2U);
    EXPECT_EQ(request.at("segments_dirs").at(0).at("role"), "segments-2012");
    EXPECT_EQ(request.at("segments_dirs").at(1).at("role"), "segments-2013");
}

TEST_F(StageEquityUniverse, Csv_HeaderAndRowText_Pinned) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("out_pin")).has_value());
    const auto membership = split_lines(contents(root / "out_pin" / "membership.csv"));
    // r0: 4 eligible x 6 cuts; r1: 3 eligible x 6 cuts (D fails the price floor, E is gone).
    ASSERT_EQ(membership.size(), 1U + 24U + 18U);
    EXPECT_EQ(membership[0],
              "rebalance_rank_date,effective_date,top_n,band,security_id,rank,adv63_usd,"
              "raw_close,vendor_market_cap_usd,gics,valid_observations,nonmissing_fraction,"
              "status");
    EXPECT_EQ(membership[1], "2012-12-31,2013-01-01,1000,0.00,101,1,50000,10,1000,4510,63,1,add");
    EXPECT_EQ(membership[2], "2012-12-31,2013-01-01,1000,0.00,202,2,40000,20,2000,4510,63,1,add");
    EXPECT_EQ(membership[3], "2012-12-31,2013-01-01,1000,0.00,303,3,30000,30,3000,2010,63,1,add");
    // Empty NaN cells for vendor market cap and gics; the archive-scale id flows through.
    EXPECT_EQ(membership[4], "2012-12-31,2013-01-01,1000,0.00,1001001001070,4,500,5,,,63,1,add");
    EXPECT_EQ(membership[5], "2012-12-31,2013-01-01,1000,0.10,101,1,50000,10,1000,4510,63,1,add");
    EXPECT_EQ(membership[9], "2012-12-31,2013-01-01,2000,0.00,101,1,50000,10,1000,4510,63,1,add");
    // Rebalance order, then cut order, then rank order; incumbents read `keep`.
    EXPECT_EQ(membership[25], "2013-01-31,2013-02-01,1000,0.00,101,1,50000,10,1000,4510,63,1,keep");
    EXPECT_EQ(membership[27], "2013-01-31,2013-02-01,1000,0.00,303,3,30000,30,3000,2010,63,1,keep");
    for (atx::usize i = 1; i < membership.size(); ++i) {
        EXPECT_EQ(split_csv(membership[i]).size(), 13U) << membership[i];
    }
    const auto churn = split_lines(contents(root / "out_pin" / "churn.csv"));
    ASSERT_EQ(churn.size(), 13U);
    EXPECT_EQ(churn[0], "rebalance_rank_date,effective_date,top_n,band,adds,drops_rank,"
                        "drops_last_bar,kept,members,one_way_turnover");
    EXPECT_EQ(churn[1], "2012-12-31,2013-01-01,1000,0.00,4,0,0,0,4,0.002");
    EXPECT_EQ(churn[2], "2012-12-31,2013-01-01,1000,0.10,4,0,0,0,4,0.002");
    // r1: E has no bar on the rank session => drops_last_bar (DR15-10); turnover 1/2000.
    EXPECT_EQ(churn[7], "2013-01-31,2013-02-01,1000,0.00,0,0,1,3,3," + shortest(1.0 / 2000.0));
    EXPECT_EQ(churn[12], "2013-01-31,2013-02-01,3000,0.10,0,0,1,3,3," + shortest(1.0 / 6000.0));
}

TEST_F(StageEquityUniverse, Delisting_OrderAndDates) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("out_delist")).has_value());
    const auto rows = split_lines(contents(root / "out_delist" / "delisting.csv"));
    ASSERT_EQ(rows.size(), 1U + 6U * 4U); // D is never a member
    EXPECT_EQ(rows[0], "top_n,band,security_id,first_bar,last_bar,first_member_effective_date,"
                       "last_member_rank_date,exit_kind");
    EXPECT_EQ(rows[1], "1000,0.00,101,2012-10-28,2013-02-01,2013-01-01,2013-01-31,window_end");
    EXPECT_EQ(rows[2], "1000,0.00,202,2012-10-28,2013-02-01,2013-01-01,2013-01-31,window_end");
    EXPECT_EQ(rows[3], "1000,0.00,303,2012-10-28,2013-02-01,2013-01-01,2013-01-31,window_end");
    EXPECT_EQ(rows[4], "1000,0.00,1001001001070,2012-10-28,2012-12-31,2013-01-01,2012-12-31,"
                       "last_bar_within_window");
    EXPECT_EQ(rows[5], "1000,0.10,101,2012-10-28,2013-02-01,2013-01-01,2013-01-31,window_end");
    EXPECT_EQ(split_csv(rows[24])[0], "3000");
    EXPECT_EQ(split_csv(rows[24])[1], "0.10");
}

TEST_F(StageEquityUniverse, Run_Deterministic_ByteIdenticalTwice) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("run1")).has_value());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("run2")).has_value());
    for (const char *name : {"membership.bin", "membership.csv", "churn.csv",
                             "coverage_by_year.csv", "union_by_year.csv", "delisting.csv",
                             "survivorship.json", "seal.json"}) {
        const auto a = contents(root / "run1" / name);
        const auto b = contents(root / "run2" / name);
        ASSERT_FALSE(a.empty()) << name;
        EXPECT_EQ(a, b) << name << " is not reproducible";
    }
    // request.json / manifest.json differ ONLY through the per-run ledger binding and
    // wall time (I-9): trial_id, the pre-registration line digest, runtime, and what is
    // derived from them (universe_id and the request.json entry in files[]).
    auto request1 = Json::parse(contents(root / "run1" / "request.json"));
    auto request2 = Json::parse(contents(root / "run2" / "request.json"));
    EXPECT_EQ(request1.at("trial_ledger").at("trial_id"),
              "iteration15-point-in-time-universe-0001");
    EXPECT_EQ(request2.at("trial_ledger").at("trial_id"),
              "iteration15-point-in-time-universe-0002");
    for (auto *doc : {&request1, &request2}) doc->erase("trial_ledger");
    EXPECT_EQ(request1, request2);
    auto manifest1 = Json::parse(contents(root / "run1" / "manifest.json"));
    auto manifest2 = Json::parse(contents(root / "run2" / "manifest.json"));
    for (auto *doc : {&manifest1, &manifest2}) {
        doc->erase("trial_ledger");
        doc->erase("runtime");
        doc->erase("runtime_seconds");
        doc->erase("universe_id");
        Json kept = Json::array();
        for (const auto &file : doc->at("files")) {
            if (file.at("filename") != "request.json") kept.push_back(file);
        }
        (*doc)["files"] = kept;
    }
    EXPECT_EQ(manifest1, manifest2);
}

TEST_F(StageEquityUniverse, Ledger_PreAndTerminalLinesDeclareZero) {
    // A cp14 line first, so the cp14 sum is visibly untouched by the cp15 zero lines.
    impl::TrialLedgerEntry cp14;
    cp14.trial_id = "iteration14-cross-section-ic-0001";
    cp14.appended_utc = "2026-09-20T00:00:00Z";
    cp14.checkpoint = 14;
    cp14.purpose = "training-only-forecast-evaluation";
    cp14.status = "pre-registered";
    cp14.trial_count_declared = 30;
    cp14.parents = {{"design-note", "", std::string(64, 'a')}};
    cp14.recipe.signals = {{"momentum_252", "dsl", std::string(64, 'b')}};
    cp14.window = {"2013-04-04", "2014-01-01", 189};
    cp14.producer_executable_sha256 = std::string(64, 'c');
    cp14.notes = "cp14 fixture line";
    ASSERT_TRUE(impl::append_trial(ledger().string(), cp14).has_value());

    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("out_ledger")).has_value());
    const auto head = impl::verify_trial_ledger(ledger().string());
    ASSERT_TRUE(head.has_value()) << head.error().message();
    EXPECT_EQ(head->lines, 3U);
    const auto lines = ledger_lines();
    ASSERT_EQ(lines.size(), 3U);
    for (const auto &line : {lines[1], lines[2]}) {
        EXPECT_EQ(line.at("checkpoint"), 15);
        EXPECT_EQ(line.at("trial_count_declared"), 0);
        EXPECT_EQ(line.at("purpose"), "point-in-time-universe-construction");
        EXPECT_EQ(line.at("trial_id"), "iteration15-point-in-time-universe-0001");
        EXPECT_EQ(line.at("window").at("observations"), 2);
        EXPECT_EQ(line.at("window").at("start"), "2013-01-01");
        EXPECT_EQ(line.at("window").at("end_exclusive"), "2013-02-02");
        EXPECT_EQ(line.at("recipe").at("alignment"), "rank-at-t-effective-at-t-plus-1-session");
        EXPECT_EQ(line.at("recipe").at("restrictions").size(), 6U);
        EXPECT_EQ(line.at("recipe").at("signals").at(0).at("name"), "adv63_median_dollar_volume");
        EXPECT_EQ(line.at("fit_boundary").at("fit_kind"), "unfit-no-fitting-performed");
        bool design = false;
        bool segments = false;
        for (const auto &parent : line.at("parents")) {
            if (parent.at("role") == "design-note") {
                design = parent.at("sha256") == std::string(impl::kEquityUniverseDesignNoteSha256);
            }
            if (parent.at("role") == "segments-2012") segments = true;
        }
        EXPECT_TRUE(design);
        EXPECT_TRUE(segments);
    }
    EXPECT_EQ(lines[1].at("status"), "pre-registered");
    EXPECT_EQ(lines[2].at("status"), "completed");
    EXPECT_FALSE(lines[2].at("result").at("manifest_sha256").is_null());
    EXPECT_EQ(lines[2].at("result").at("manifest_sha256"),
              *atx::core::sha256_file((root / "out_ledger" / "manifest.json").string()));
    const auto fifteen = impl::declared_trials_for_checkpoint(ledger().string(), 15);
    ASSERT_TRUE(fifteen.has_value()) << fifteen.error().message();
    EXPECT_EQ(*fifteen, 0U);
    const auto fourteen = impl::declared_trials_for_checkpoint(ledger().string(), 14);
    ASSERT_TRUE(fourteen.has_value()) << fourteen.error().message();
    EXPECT_EQ(*fourteen, 30U);
}

TEST_F(StageEquityUniverse, TrialId_IncrementsAcrossRuns) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("id1")).has_value());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("id2")).has_value());
    const auto lines = ledger_lines();
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_EQ(lines[0].at("trial_id"), "iteration15-point-in-time-universe-0001");
    EXPECT_EQ(lines[2].at("trial_id"), "iteration15-point-in-time-universe-0002");
    EXPECT_EQ(*impl::pre_registered_lines_for_checkpoint(ledger().string(), 15), 2U);
}

TEST_F(StageEquityUniverse, Ledger_FailedRunKeepsBothLines) {
    // The injected failure: a segment whose axis disagrees with its name is detected at
    // attach (§5.5 step 5), which runs AFTER the pre-registration line (step 4).
    DirSpec b = spec_b();
    b.axis_overrides = {{"2013-01-20.seg", *date_to_nanos("2013-01-21")}};
    ASSERT_NO_FATAL_FAILURE(write_dir(spec_a()));
    ASSERT_NO_FATAL_FAILURE(write_dir(b));
    const auto failed = impl::run_equity_universe(universe_config("out_failed"));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code(), ErrorCode::InvalidArgument);
    const auto head = impl::verify_trial_ledger(ledger().string());
    ASSERT_TRUE(head.has_value()) << head.error().message();
    ASSERT_EQ(head->lines, 2U);
    const auto lines = ledger_lines();
    EXPECT_EQ(lines[0].at("status"), "pre-registered");
    EXPECT_EQ(lines[1].at("status"), "failed");
    EXPECT_EQ(lines[1].at("result").at("outcome"), "failed");
    EXPECT_EQ(lines[1].at("trial_id"), lines[0].at("trial_id"));
    EXPECT_TRUE(lines[1].at("result").at("manifest_sha256").is_null());
    ASSERT_FALSE(lines[1].at("result").at("failure_sha256").is_null());
    EXPECT_FALSE(lines[1].at("runtime").at("wall_seconds").is_null());
    const fs::path out = root / "out_failed";
    EXPECT_TRUE(fs::exists(out / "failure.json"));
    EXPECT_TRUE(fs::exists(out / ".pending"));
    EXPECT_FALSE(fs::exists(out / "manifest.json"));
    EXPECT_EQ(lines[1].at("result").at("failure_sha256"),
              *atx::core::sha256_file((out / "failure.json").string()));
    const auto failure = Json::parse(contents(out / "failure.json"));
    EXPECT_EQ(failure.at("status"), "failed");
    EXPECT_NE(failure.at("error").get<std::string>().find("2013-01-20.seg"), std::string::npos);
}

TEST_F(StageEquityUniverse, Budget_BelowReserve_Rejected) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    impl::RunConfig at_reserve = universe_config("out_reserve");
    at_reserve.equity_max_working_bytes = 512'000'000ULL;
    const auto rejected = impl::run_equity_universe(at_reserve);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    impl::RunConfig tiny = universe_config("out_tiny");
    tiny.equity_max_working_bytes = 512'000'001ULL;
    const auto preflight = impl::run_equity_universe(tiny);
    ASSERT_FALSE(preflight);
    EXPECT_EQ(preflight.error().code(), ErrorCode::OutOfRange);
    EXPECT_FALSE(fs::exists(root / "out_tiny")) << "preflight precedes the reservation";
    EXPECT_FALSE(fs::exists(ledger()));
}

TEST_F(StageEquityUniverse, MembershipBin_IsEngineEncodingAndPinned) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    ASSERT_TRUE(impl::run_equity_universe(universe_config("out_bin")).has_value());
    const auto bytes = contents(root / "out_bin" / "membership.bin");
    ASSERT_FALSE(bytes.empty());
    // An identically fed builder: dir A's five names for 65 sessions, dir B's four for 32.
    data::PitUniverseConfig cfg;
    cfg.top_n = {1000, 2000, 3000, 0};
    cfg.top_n_count = 3;
    cfg.band_bp = {0, 1000};
    cfg.band_count = 2;
    cfg.max_rebalances = 2;
    cfg.max_sessions = kSessionsA + kSessionsB;
    auto builder = data::PitUniverseBuilder::create(cfg);
    ASSERT_TRUE(builder.has_value()) << builder.error().message();
    const atx::i64 first = *date_to_nanos(kFirstA);
    const std::set<atx::i64> rank_keys{*date_to_nanos("2012-12-31"), *date_to_nanos("2013-01-31")};
    for (atx::usize s = 0; s < kSessionsA + kSessionsB; ++s) {
        const atx::i64 key = first + static_cast<atx::i64>(s) * kDay;
        const atx::usize n = s < kSessionsA ? kNames.size() : 4U;
        std::vector<atx::i64> ids;
        std::vector<double> close, volume, shares, gics;
        for (atx::usize j = 0; j < n; ++j) {
            ids.push_back(kNames[j].id);
            close.push_back(kNames[j].close);
            volume.push_back(kNames[j].volume);
            shares.push_back(kNames[j].shares);
            gics.push_back(kNames[j].gics);
        }
        const auto observed = builder->observe_session(key, ids, close, volume, shares, gics);
        ASSERT_TRUE(observed.has_value()) << observed.error().message();
        if (rank_keys.contains(key)) {
            const auto view = builder->rebalance(key);
            ASSERT_TRUE(view.has_value()) << view.error().message();
        }
    }
    const auto encoded = data::encode_membership_bin(*builder);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().message();
    EXPECT_EQ(bytes, *encoded);
    const auto image = data::decode_membership_bin(bytes);
    ASSERT_TRUE(image.has_value()) << image.error().message();
    ASSERT_EQ(image->rebalances.size(), 2U);
    EXPECT_EQ(image->rebalances[0].rank_session_key, *date_to_nanos("2012-12-31"));
    EXPECT_EQ(image->rebalances[0].effective_session_key, *date_to_nanos("2013-01-01"));
    EXPECT_EQ(image->rebalances[1].effective_session_key, *date_to_nanos("2013-02-01"));
    EXPECT_EQ(image->rebalances[0].cuts[0].security_ids,
              (std::vector<atx::i64>{101, 202, 303, 1001001001070LL}));
    EXPECT_EQ(image->rebalances[0].cuts[0].ranks, (std::vector<atx::u32>{1, 2, 3, 4}));
    EXPECT_EQ(image->rebalances[1].cuts[5].security_ids, (std::vector<atx::i64>{101, 202, 303}));
    // manifest.json records the SHA-256 and the FNV trailer (DR15-6, §5.9).
    const auto manifest = Json::parse(contents(root / "out_bin" / "manifest.json"));
    EXPECT_EQ(manifest.at("membership_bin").at("sha256"), *atx::core::sha256_hex(bytes));
    EXPECT_EQ(manifest.at("membership_bin").at("byte_length"), bytes.size());
    EXPECT_EQ(manifest.at("membership_bin").at("fnv1a64_trailer"), std::to_string(image->fnv1a64));
    EXPECT_EQ(manifest.at("membership_bin").at("rebalance_count"), 2);
}

TEST(StageEquityUniverseDesign, DesignNoteSha_IsSixtyFourLowerHex) {
    const std::string_view sha = impl::kEquityUniverseDesignNoteSha256;
    EXPECT_EQ(sha.size(), 64U);
    EXPECT_EQ(sha.find_first_not_of("0123456789abcdef"), std::string_view::npos);
    EXPECT_TRUE(impl::kEquityUniverseDesignNoteRelativePath.ends_with(
        "2026-09-20-iteration15-point-in-time-universe-design.md"));
}

TEST_F(StageEquityUniverse, SegmentSha_MustMatchIngestionManifest) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    // A byte-flipped segment: its digest no longer matches the receipt.
    const fs::path victim = dir_b() / "2013-01-05.seg";
    {
        std::string bytes = contents(victim);
        ASSERT_GT(bytes.size(), 64U);
        bytes[bytes.size() - 20] = static_cast<char>(bytes[bytes.size() - 20] ^ 0x01);
        std::ofstream out(victim, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto flipped = impl::run_equity_universe(universe_config("out_flipped"));
    ASSERT_FALSE(flipped);
    EXPECT_EQ(flipped.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(flipped.error().message().find("2013-01-05.seg"), std::string::npos);
    // A segment absent from the receipt (written after the manifest).
    fs::remove_all(dir_b());
    ASSERT_NO_FATAL_FAILURE(write_dir(spec_b()));
    ASSERT_NO_FATAL_FAILURE(write_segment(dir_b() / "2013-02-02.seg", *date_to_nanos("2013-02-02"),
                                          std::span<const Name>(kNames.data(), 4)));
    const auto extra = impl::run_equity_universe(universe_config("out_extra"));
    ASSERT_FALSE(extra);
    EXPECT_EQ(extra.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(extra.error().message().find("2013-02-02.seg"), std::string::npos);
    // A receipt entry whose file is gone.
    fs::remove_all(dir_b());
    ASSERT_NO_FATAL_FAILURE(write_dir(spec_b()));
    fs::remove(dir_b() / "2013-01-25.seg");
    const auto missing = impl::run_equity_universe(universe_config("out_missing"));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(missing.error().message().find("2013-01-25.seg"), std::string::npos);
    EXPECT_FALSE(fs::exists(ledger())) << "binding precedes the pre-registration line (I-12)";
}

TEST_F(StageEquityUniverse, LargeSecurityId_FlowsThroughStage) {
    // DR15-1: an archive-scale id (> 2^20, here 1,001,001,001,070) is parsed as a
    // canonical positive i64, ranked, and lands in membership.csv and membership.bin.
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    const auto result = impl::run_equity_universe(universe_config("out_large"));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto membership = contents(root / "out_large" / "membership.csv");
    EXPECT_NE(membership.find(",1001001001070,4,500,5,,,63,1,add"), std::string::npos);
    const auto image = data::decode_membership_bin(contents(root / "out_large" / "membership.bin"));
    ASSERT_TRUE(image.has_value()) << image.error().message();
    const auto &ids = image->rebalances.at(0).cuts.at(0).security_ids;
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1001001001070LL), ids.end());
    // A non-canonical symbol ("0101") is refused, never aliased onto 101.
    fs::remove_all(dir_b());
    DirSpec b = spec_b();
    ASSERT_NO_FATAL_FAILURE(write_dir(b));
    {
        std::vector<std::string> fields;
        for (const auto field : data::kOratsFields) fields.emplace_back(field);
        atx::tsdb::SegmentBuilder alias(fields, {"0101"}, {*date_to_nanos("2013-01-12")});
        alias.set(orats_field("close"), 0, 0, 10.0);
        alias.set(orats_field("volume"), 0, 0, 5000.0);
        ASSERT_TRUE(alias.write((dir_b() / "2013-01-12.seg").string(), 0).has_value());
    }
    ASSERT_NO_FATAL_FAILURE(write_manifests(dir_b(), prep_b(), *date_to_nanos(kFirstB),
                                            kSessionsB, spec_b().daily));
    const auto refused = impl::run_equity_universe(universe_config("out_alias"));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(refused.error().message().find("0101"), std::string::npos);
}

TEST_F(StageEquityUniverse, SegmentsDirs_Rejections) {
    ASSERT_NO_FATAL_FAILURE(build_default_fixture());
    // A path containing `;` splits into two entries: the count no longer pairs, and the
    // fragment is not a directory either way.
    impl::RunConfig semicolon = universe_config("out_semicolon");
    semicolon.equity_segments_dirs =
        dir_a().generic_string() + ";" + (root / "no;such").generic_string();
    const auto split = impl::run_equity_universe(semicolon);
    ASSERT_FALSE(split);
    EXPECT_EQ(split.error().code(), ErrorCode::InvalidArgument);
    impl::RunConfig trailing = universe_config("out_trailing");
    trailing.equity_segments_dirs += ";";
    const auto empty_entry = impl::run_equity_universe(trailing);
    ASSERT_FALSE(empty_entry);
    EXPECT_EQ(empty_entry.error().code(), ErrorCode::InvalidArgument);
    // A `foo.seg` name.
    ASSERT_NO_FATAL_FAILURE(write_segment(dir_b() / "foo.seg", *date_to_nanos("2013-03-01"),
                                          std::span<const Name>(kNames.data(), 4)));
    const auto foo = impl::run_equity_universe(universe_config("out_foo"));
    ASSERT_FALSE(foo);
    EXPECT_EQ(foo.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(foo.error().message().find("foo.seg"), std::string::npos);
    fs::remove(dir_b() / "foo.seg");
    // The same date in two directories.
    DirSpec twin = spec_b();
    twin.dir = root / "segTwin";
    twin.preparation = root / "prepTwin" / "manifest.json";
    twin.first_key = *date_to_nanos("2013-01-20");
    twin.sessions = 3;
    ASSERT_NO_FATAL_FAILURE(write_dir(twin));
    impl::RunConfig duplicate = universe_config("out_duplicate");
    duplicate.equity_segments_dirs = join({dir_a(), dir_b(), twin.dir});
    duplicate.equity_preparation_manifests = join({prep_a(), prep_b(), twin.preparation});
    const auto dup = impl::run_equity_universe(duplicate);
    ASSERT_FALSE(dup);
    EXPECT_EQ(dup.error().code(), ErrorCode::InvalidArgument);
    // Fewer than 63 sessions at or before the first rank session (2012-11-30).
    const auto warmup = impl::run_equity_universe(universe_config("out_warmup", "2012-11-01"));
    ASSERT_FALSE(warmup);
    EXPECT_EQ(warmup.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(warmup.error().message().find("63"), std::string::npos);
    // --rank-end equal to the last attached session (DR15-3 / C-6).
    const auto last =
        impl::run_equity_universe(universe_config("out_last", kRankStart, "2013-02-01"));
    ASSERT_FALSE(last);
    EXPECT_EQ(last.error().code(), ErrorCode::InvalidArgument);
    // A window with no month-last session inside it.
    const auto none =
        impl::run_equity_universe(universe_config("out_none", "2013-01-02", "2013-01-30"));
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(ledger()));
}
