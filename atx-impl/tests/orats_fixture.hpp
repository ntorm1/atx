#pragma once

// Shared ORATS zip fixture helpers for atx-impl tests.
//
// All functions are `inline` and data is `inline constexpr` so that multiple
// test TUs can include this header without ODR violations.
//
// Design notes:
//  - No anonymous namespace: callers include from named test TUs; the inline
//    linkage of every symbol here prevents ODR issues across TUs.
//  - write_orats_zip is void-returning and uses ASSERT_TRUE so that a miniz
//    failure aborts the test immediately rather than returning a bad path.
//  - make_orats_row takes shares as long long (it is cast to long long internally
//    anyway; passing double invited narrowing-conversion surprises).

#include <array>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <miniz.h>

namespace atx_impl_test {

// The real 71-column header (from the file). Only the columns the loader needs
// are asserted; the rest are present so resolve_header sees the true layout.
inline constexpr const char* kHeader =
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
// `shares` is long long: it is stored as an integer security count.
inline std::string make_orats_row(const char* date, const char* secid, const char* tk,
                                   const char* today, double close, double cumret,
                                   long long shares) {
    std::array<std::string, 71> f;
    for (auto& x : f) x = "0";
    f[0] = date;  f[1] = secid;  f[2] = tk;  f[3] = today;
    f[5] = "1";   f[6] = "1";    f[7] = "1";              // open/high/low
    f[8]  = std::to_string(close);
    f[10] = std::to_string(shares);                        // volume placeholder
    f[11] = std::to_string(shares);                        // shares
    f[62] = "5";                                            // GICS
    f[65] = std::to_string(cumret);                         // cumulReturnFactor
    std::string line;
    for (size_t i = 0; i < f.size(); ++i) {
        line += f[i];
        if (i + 1 < f.size()) line += '\t';
    }
    return line + "\n";
}

// Write `body` (header + rows) into a zip file at `out_path`.
// Uses ASSERT_TRUE so a miniz failure aborts the calling test immediately.
// Must be called from a void-returning function (ASSERT_TRUE requirement).
inline void write_orats_zip(const std::string& body, const std::string& out_path) {
    namespace fs = std::filesystem;
    const fs::path p(out_path);
    fs::remove(p);
    mz_zip_archive zip{};
    ASSERT_TRUE(mz_zip_writer_init_file(&zip, p.string().c_str(), 0))
        << "mz_zip_writer_init_file failed for " << out_path;
    ASSERT_TRUE(mz_zip_writer_add_mem(&zip, "tbltickerhistory3_10y.txt",
                                      body.data(), body.size(), MZ_BEST_SPEED))
        << "mz_zip_writer_add_mem failed";
    ASSERT_TRUE(mz_zip_writer_finalize_archive(&zip))
        << "mz_zip_writer_finalize_archive failed";
    ASSERT_TRUE(mz_zip_writer_end(&zip))
        << "mz_zip_writer_end failed";
}

// Canonical tiny fixture: header + 1 pre-2020 row (filtered) +
// 2020-01-02 (2 securities) + 2020-01-03 (1 security).
// Writes the zip to a temp path and returns the path string.
// Must be called from a void-returning context OR wrapped appropriately —
// because write_orats_zip uses ASSERT_TRUE, make_orats_zip itself must be
// called from a test body (not from a helper that returns a non-void value).
inline std::string make_orats_zip_path() {
    return (std::filesystem::temp_directory_path() / "atx_impl_orats_tiny.zip").string();
}

// Build the body of the canonical tiny fixture (does not write to disk).
inline std::string make_orats_zip_body() {
    std::string body = std::string(kHeader) + "\n";
    body += make_orats_row("2019-12-31", "33449", "AAPL", "AAPL", 290.0, 0.9, 4000000000LL); // FILTERED
    body += make_orats_row("2020-01-02", "33449", "AAPL", "AAPL", 300.0, 1.0, 4000000000LL);
    body += make_orats_row("2020-01-02", "33008", "AA",   "HWM",   20.0, 0.5, 1000000000LL);
    body += make_orats_row("2020-01-03", "33449", "AAPL", "AAPL", 303.0, 1.0, 4000000000LL);
    return body;
}

} // namespace atx_impl_test
