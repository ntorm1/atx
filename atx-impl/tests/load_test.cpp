#include <array>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <miniz.h>

#include "atx/core/error.hpp"
#include "config.hpp"
#include "stages.hpp"

namespace {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture helpers (copied from atx-engine/tests/data/data_orats_history_test.cpp
// — there is no shared fixture header; copying is correct and expected).
// ---------------------------------------------------------------------------

// The real 71-column header (from the file). Only the columns the loader needs
// are asserted; the rest are present so resolve_header sees the true layout.
constexpr const char* kHeader =
    "tradingDate\tsecurityID\tticker_tk\ttodayTicker\tdn\topen\thigh\tlow\tclose\tclosePr\t"
    "volume\tshares\tearnFlag\tccVar\thlVar\trvVar\texpiryCount\thEMove\tiEMove\tshD1\tlnD1\t"
    "atmCenI_decay\tatmCenI_st\tatmCenI_lt\tatmCenI_5d\tatmCenI_21d\tatmCenI_42d\tatmCenI_63d\t"
    "atmCenI_84d\tatmCenI_105d\tatmCenI_126d\tatmCenI_189d\tatmCenI_252d\tatmCenI_378d\t"
    "atmCenI_504d\tatmCenH_st\tatmCenH_lt\tatmCenH_decay\tatmCenH_5d\tatmCenH_21d\tatmCenH_42d\t"
    "atmCenH_63d\tatmCenH_84d\tatmCenH_105d\tatmCenH_126d\tatmCenH_189d\tatmCenH_252d\t"
    "atmCenH_378d\tatmCenH_504d\tnEarnCnt\tnEarnCnt_5d\tnEarnCnt_21d\tnEarnCnt_42d\tnEarnCnt_63d\t"
    "nEarnCnt_84d\tnEarnCnt_105d\tnEarnCnt_126d\tnEarnCnt_189d\tnEarnCnt_252d\tnEarnCnt_378d\t"
    "nEarnCnt_504d\tGICS\tcloseUnadjPr\treturnFactor\ttotalReturn\tcumulReturnFactor\twkD1\t"
    "atmCenI_10d\tatmCenH_10d\tnEarnCnt_10d\tqtrD1";

// One TSV data row: 71 tab-separated fields; fill only the ones the loader
// projects, zeros elsewhere.
std::string make_orats_row(const char* date, const char* secid, const char* tk,
                            const char* today, double close, double cumret,
                            double shares) {
    std::array<std::string, 71> f;
    for (auto& x : f) x = "0";
    f[0] = date;  f[1] = secid;  f[2] = tk;  f[3] = today;
    f[5] = "1";   f[6] = "1";    f[7] = "1";              // open/high/low
    f[8]  = std::to_string(close);
    f[10] = std::to_string(static_cast<long long>(shares)); // volume placeholder
    f[11] = std::to_string(static_cast<long long>(shares)); // shares
    f[62] = "5";                                            // GICS
    f[65] = std::to_string(cumret);                         // cumulReturnFactor
    std::string line;
    for (size_t i = 0; i < f.size(); ++i) {
        line += f[i];
        if (i + 1 < f.size()) line += '\t';
    }
    return line + "\n";
}

// Write `body` (header + rows) into a zip entry named like the real ORATS file.
std::string write_orats_zip(const std::string& body, const char* file_name) {
    const fs::path p = fs::temp_directory_path() / file_name;
    fs::remove(p);
    mz_zip_archive zip{};
    EXPECT_TRUE(mz_zip_writer_init_file(&zip, p.string().c_str(), 0));
    EXPECT_TRUE(mz_zip_writer_add_mem(&zip, "tbltickerhistory3_10y.txt",
                                      body.data(), body.size(), MZ_BEST_SPEED));
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
    EXPECT_TRUE(mz_zip_writer_end(&zip));
    return p.string();
}

// Canonical tiny fixture: header + 1 pre-2020 row (filtered) +
// 2020-01-02 (2 securities) + 2020-01-03 (1 security).
std::string make_orats_zip() {
    std::string body = std::string(kHeader) + "\n";
    body += make_orats_row("2019-12-31", "33449", "AAPL", "AAPL", 290.0, 0.9, 4000000000); // FILTERED
    body += make_orats_row("2020-01-02", "33449", "AAPL", "AAPL", 300.0, 1.0, 4000000000);
    body += make_orats_row("2020-01-02", "33008", "AA",   "HWM",   20.0, 0.5, 1000000000);
    body += make_orats_row("2020-01-03", "33449", "AAPL", "AAPL", 303.0, 1.0, 4000000000);
    return write_orats_zip(body, "atx_impl_orats_tiny.zip");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1: happy path — tiny zip loads correctly, kvs and output files present
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, LoadsTinyZip) {
    const std::string zip = make_orats_zip();
    const fs::path out = fs::temp_directory_path() / "atx_impl_load_tiny_out";
    fs::remove_all(out);

    atx::impl::RunConfig cfg;
    cfg.zip      = zip;
    cfg.out      = out.string();
    cfg.min_date = "2020-01-01";

    auto r = atx::impl::run_load(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    // Check kvs by scanning the vector.
    auto find_kv = [&](const std::string& key) -> std::string {
        for (const auto& [k, v] : r->kvs) {
            if (k == key) return v;
        }
        return "";
    };

    EXPECT_EQ(find_kv("rows_kept"),    "3");
    EXPECT_EQ(find_kv("dates_written"), "2");
    EXPECT_EQ(find_kv("distinct"),     "2");

    // Side-car files must exist.
    EXPECT_TRUE(fs::exists(out / "_symbology.parquet"));
    EXPECT_TRUE(fs::exists(out / "_manifest.json"));
    EXPECT_TRUE(fs::exists(out / "2020-01-02.seg"));

    // Clean up.
    fs::remove_all(out);
}

// ---------------------------------------------------------------------------
// Test 2: digest is stable across two independent runs with the same inputs
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, DigestStableAcrossRuns) {
    const std::string zip = make_orats_zip();

    auto run_once = [&](const char* tag) {
        const fs::path out =
            fs::temp_directory_path() / (std::string("atx_impl_load_det_") + tag);
        fs::remove_all(out);
        atx::impl::RunConfig cfg;
        cfg.zip      = zip;
        cfg.out      = out.string();
        cfg.min_date = "2020-01-01";
        auto r = atx::impl::run_load(cfg);
        EXPECT_TRUE(r.has_value()) << r.error().message();
        atx::u64 d = r.has_value() ? r->digest : 0u;
        fs::remove_all(out);
        return d;
    };

    const atx::u64 d1 = run_once("a");
    const atx::u64 d2 = run_once("b");

    EXPECT_NE(d1, 0u)  << "digest must be non-zero";
    EXPECT_EQ(d1, d2)  << "digest must be identical across runs";
}

// ---------------------------------------------------------------------------
// Test 3: malformed min_date => Err(InvalidArgument)
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, MalformedMinDateFails) {
    const std::string zip = make_orats_zip();
    const fs::path out = fs::temp_directory_path() / "atx_impl_load_baddate_out";

    atx::impl::RunConfig cfg;
    cfg.zip      = zip;
    cfg.out      = out.string();
    cfg.min_date = "not-a-date";

    auto r = atx::impl::run_load(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Test 4: missing --zip => Err(InvalidArgument)
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, MissingArgsFails) {
    atx::impl::RunConfig cfg;
    // cfg.zip intentionally empty
    cfg.out      = "some/out";
    cfg.min_date = "2020-01-01";

    auto r = atx::impl::run_load(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}
