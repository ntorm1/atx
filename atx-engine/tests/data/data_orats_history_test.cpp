#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <miniz.h>

#include "atx/engine/data/orats_history.hpp"
#include "atx/tsdb/segment_reader.hpp"

namespace {
namespace fs = std::filesystem;
using namespace atx::engine::data;

// The real 71-column header (from the file). Only the columns the loader needs are
// asserted; the rest are present so resolve_header sees the true layout.
constexpr const char *kHeader =
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
} // namespace

TEST(DataOratsHistory, DateToNanosMidnightUtc) {
  // 2020-01-02 is 18263 days after 1970-01-01.
  const atx::i64 expected = static_cast<atx::i64>(18263) * 86400LL * 1000000000LL;
  auto got = detail::date_to_nanos("2020-01-02");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, expected);
  EXPECT_FALSE(detail::date_to_nanos("2020-13-02").has_value()); // bad month
  EXPECT_FALSE(detail::date_to_nanos("not-a-date").has_value());
}

TEST(DataOratsHistory, DateToNanosValidatesCalendarAndRepresentableMidnight) {
  // Gregorian leap-year rules and exact digit-only YYYY-MM-DD grammar.
  EXPECT_TRUE(detail::date_to_nanos("2000-02-29").has_value());
  EXPECT_TRUE(detail::date_to_nanos("2024-02-29").has_value());
  for (const char *date : {"1900-02-29", "2100-02-29", "2023-02-29", "2020-04-31",
                           "2020-02-30", "2020-00-01", "2020-01-00", "2020--1-02",
                           "2020-1-02", " 020-01-01"}) {
    EXPECT_FALSE(detail::date_to_nanos(date).has_value()) << date;
  }
  // The midnight immediately inside each signed nanosecond boundary is valid;
  // the adjacent midnight is outside it. No overflowing multiply is attempted.
  constexpr atx::i64 kDay = 86400LL * 1000000000LL;
  EXPECT_EQ(detail::date_to_nanos("1677-09-22"), -106751LL * kDay);
  EXPECT_EQ(detail::date_to_nanos("2262-04-11"), 106751LL * kDay);
  for (const char *date : {"1677-09-21", "2262-04-12", "0000-01-01", "9999-12-31"}) {
    EXPECT_FALSE(detail::date_to_nanos(date).has_value()) << date;
  }
  EXPECT_EQ(detail::date_to_nanos("1969-12-31"), -kDay);
}

TEST(DataOratsHistory, ResolveHeaderFindsProjectedColumns) {
  auto idx = detail::resolve_header(kHeader);
  ASSERT_TRUE(idx.has_value()) << idx.error().to_string();
  EXPECT_EQ(idx->tradingDate, 0);
  EXPECT_EQ(idx->securityID, 1);
  EXPECT_EQ(idx->ticker_tk, 2);
  EXPECT_EQ(idx->todayTicker, 3);
  // field[0] == "open" is column 5. Segment field[10] is the 15-char name
  // "cumReturnFactor"; resolve_header maps it back to the real TSV header
  // "cumulReturnFactor" (column 65) — the same special-casing as gics->GICS.
  EXPECT_EQ(idx->field[0], 5);
  EXPECT_EQ(kOratsFields[10], "cumReturnFactor");
  EXPECT_EQ(idx->field[10], 65); // resolved to the TSV "cumulReturnFactor" column
}

TEST(DataOratsHistory, ResolveHeaderRejectsMissingColumn) {
  auto idx = detail::resolve_header("tradingDate\tsecurityID\topen"); // missing most
  ASSERT_FALSE(idx.has_value());
  EXPECT_EQ(idx.error().code(), atx::core::ErrorCode::ParseError);
}

namespace {
// One TSV data row: 71 tab-separated fields; fill only the ones the loader
// projects, zeros elsewhere.
std::string make_orats_row(const char *date, const char *secid, const char *tk, const char *today,
                           double close, double cumret, double shares, const char *gics = "5") {
  std::array<std::string, 71> f;
  for (auto &x : f) x = "0";
  f[0] = date; f[1] = secid; f[2] = tk; f[3] = today;
  f[5] = "1"; f[6] = "1"; f[7] = "1";              // open/high/low
  f[8] = std::to_string(close);                    // close (col 9, idx 8)
  f[10] = std::to_string(static_cast<long long>(shares)); // volume placeholder
  f[11] = std::to_string(static_cast<long long>(shares)); // shares (idx 11)
  f[62] = gics;                                    // GICS (idx 62)
  f[65] = std::to_string(cumret);                  // cumulReturnFactor (idx 65)
  std::string line;
  for (size_t i = 0; i < f.size(); ++i) { line += f[i]; if (i + 1 < f.size()) line += '\t'; }
  return line + "\n";
}

// Write `body` (header + rows) into a zip entry named like the real ORATS file.
std::string write_orats_zip(const std::string &body, const char *file_name) {
  const fs::path p = fs::temp_directory_path() / file_name;
  fs::remove(p);
  mz_zip_archive zip{};
  EXPECT_TRUE(mz_zip_writer_init_file(&zip, p.string().c_str(), 0));
  EXPECT_TRUE(mz_zip_writer_add_mem(&zip, "tbltickerhistory3_10y.txt", body.data(), body.size(),
                                    MZ_BEST_SPEED));
  EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
  EXPECT_TRUE(mz_zip_writer_end(&zip));
  return p.string();
}

std::string make_orats_zip() {
  // header + 1 pre-2020 row (filtered) + date A (2 securities) + date B (1 security)
  std::string body = std::string(kHeader) + "\n";
  body += make_orats_row("2019-12-31", "33449", "AAPL", "AAPL", 290.0, 0.9, 4000000000); // FILTERED
  body += make_orats_row("2020-01-02", "33449", "AAPL", "AAPL", 300.0, 1.0, 4000000000);
  body += make_orats_row("2020-01-02", "33008", "AA",   "HWM",  20.0,  0.5, 1000000000);
  body += make_orats_row("2020-01-03", "33449", "AAPL", "AAPL", 303.0, 1.0, 4000000000);
  return write_orats_zip(body, "atx_orats_tiny.zip");
}

fs::path guard_output(const char *tag) {
  return fs::temp_directory_path() / (std::string("atx_orats_guard_") + tag);
}

atx::core::Result<OratsLoadStats> load_guard_rows(const std::string &rows, const char *tag,
                                                bool exclude_no_sector = false) {
  const std::string filename = std::string("atx_orats_guard_") + tag + ".zip";
  const std::string zip = write_orats_zip(std::string(kHeader) + "\n" + rows, filename.c_str());
  const fs::path out = guard_output(tag);
  fs::remove_all(out);
  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;
  cfg.exclude_no_sector = exclude_no_sector;
  return load_orats_history(cfg);
}
} // namespace

TEST(DataOratsHistory, InvalidKeysAreCountedAndPositiveIdsAreCanonicalized) {
  std::string rows;
  for (const char *id : {"", "0", "-1", "12x", "+12", " 12", "9223372036854775808", "abc"}) {
    rows += make_orats_row("2020-01-02", id, "BAD", "BAD", 100.0, 1.0, 1000);
  }
  rows += make_orats_row("2020-01-02", "00012", "A", "A", 101.0, 1.0, 1000);
  rows += make_orats_row("2020-01-02", "9223372036854775807", "B", "B", 102.0, 1.0, 1000);
  const auto st = load_guard_rows(rows, "keys");
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_read, 10);
  EXPECT_EQ(st->rows_malformed, 8);
  EXPECT_EQ(st->rows_kept, 2);
  EXPECT_EQ(st->distinct_securities, 2);
  EXPECT_EQ(st->rows_read, st->rows_filtered + st->rows_malformed + st->rows_kept);
  auto rdr = atx::tsdb::SegmentReader::attach((guard_output("keys") / "2020-01-02.seg").string());
  ASSERT_TRUE(rdr.has_value()) << rdr.error().to_string();
  EXPECT_EQ(rdr->instrument_count(), 2u);
  EXPECT_EQ(rdr->symbol_name(0), "12");
  EXPECT_EQ(rdr->symbol_name(1), "9223372036854775807");
}

TEST(DataOratsHistory, InvalidCalendarRowsAreCountedWithoutAliasingAnotherDate) {
  std::string rows = make_orats_row("2020-02-30", "12", "A", "A", 900.0, 1.0, 1000);
  rows += make_orats_row("9999-12-31", "12", "A", "A", 800.0, 1.0, 1000);
  rows += make_orats_row("2020-03-01", "12", "A", "A", 100.0, 1.0, 1000);
  const auto st = load_guard_rows(rows, "calendar");
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_malformed, 2);
  EXPECT_EQ(st->rows_kept, 1);
  EXPECT_EQ(st->dates_written, 1);
  EXPECT_FALSE(fs::exists(guard_output("calendar") / "2020-02-30.seg"));
}

TEST(DataOratsHistory, DuplicatePositiveKeyFailsBeforeWritingConflictingDate) {
  for (const char *second_id : {"33449", "033449"}) {
    std::string rows = make_orats_row("2020-01-02", "33449", "AAPL", "AAPL", 300.0, 1.0, 1000);
    rows += make_orats_row("2020-01-02", second_id, "AAPL", "AAPL", 900.0, 1.0, 1000);
    const auto st = load_guard_rows(rows, "duplicates");
    ASSERT_FALSE(st.has_value()) << second_id;
    EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_NE(st.error().to_string().find("33449"), std::string::npos);
    EXPECT_NE(st.error().to_string().find("2020-01-02"), std::string::npos);
    EXPECT_FALSE(fs::exists(guard_output("duplicates") / "2020-01-02.seg"));
    EXPECT_FALSE(fs::exists(guard_output("duplicates") / "_manifest.json"));
  }
}

TEST(DataOratsHistory, DuplicateKeyGuardPrecedesSectorFilter) {
  std::string rows = make_orats_row("2020-01-02", "12", "A", "A", 100.0, 1.0, 1000, "");
  rows += make_orats_row("2020-01-02", "12", "A", "A", 101.0, 1.0, 1000);
  const auto st = load_guard_rows(rows, "sector_duplicate", true);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(DataOratsHistory, DuplicateSubfloorKeysAreFilteredAndIdsCanRepeatOnLaterDates) {
  std::string rows;
  rows += make_orats_row("2019-12-31", "12", "A", "A", 98.0, 1.0, 1000);
  rows += make_orats_row("2019-12-31", "12", "A", "A", 99.0, 1.0, 1000);
  rows += make_orats_row("2020-01-02", "12", "A", "A", 100.0, 1.0, 1000);
  rows += make_orats_row("2020-01-03", "00012", "A", "A", 101.0, 1.0, 1000);
  const auto st = load_guard_rows(rows, "key_scope");
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_filtered, 2);
  EXPECT_EQ(st->rows_kept, 2);
  EXPECT_EQ(st->dates_written, 2);
  EXPECT_EQ(st->distinct_securities, 1);
}

TEST(DataOratsHistory, DateRegressionIsDetectedAmongSubfloorRows) {
  std::string rows = make_orats_row("2019-12-31", "12", "A", "A", 100.0, 1.0, 1000);
  rows += make_orats_row("2019-12-30", "12", "A", "A", 100.0, 1.0, 1000);
  const auto st = load_guard_rows(rows, "subfloor_order");
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(DataOratsHistory, DateRegressionIsDetectedAfterMalformedIdentity) {
  std::string rows = make_orats_row("2020-01-03", "0", "BAD", "BAD", 100.0, 1.0, 1000);
  rows += make_orats_row("2020-01-02", "12", "A", "A", 100.0, 1.0, 1000);
  const auto st = load_guard_rows(rows, "invalid_id_order");
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(DataOratsHistory, LoadsTinyZipIntoPerDateSegments) {
  const std::string zip = make_orats_zip();
  const fs::path out = fs::temp_directory_path() / "atx_orats_out";
  fs::remove_all(out);

  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;

  auto st = load_orats_history(cfg);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_read, 4);
  EXPECT_EQ(st->rows_filtered, 1);     // the 2019 row
  EXPECT_EQ(st->rows_kept, 3);
  EXPECT_EQ(st->dates_written, 2);     // 2020-01-02, 2020-01-03
  EXPECT_EQ(st->distinct_securities, 2);
  // Counting invariant: every data row lands in exactly one bucket.
  EXPECT_EQ(st->rows_read, st->rows_filtered + st->rows_malformed + st->rows_kept);

  // Side-cars are written alongside the per-date segments.
  EXPECT_TRUE(fs::exists(out / "_symbology.parquet"));
  EXPECT_TRUE(fs::exists(out / "_manifest.json"));

  // The 2020-01-02 segment has 2 instruments; close field carries 300 and 20.
  auto rdr = atx::tsdb::SegmentReader::attach((out / "2020-01-02.seg").string());
  ASSERT_TRUE(rdr.has_value()) << rdr.error().to_string();
  EXPECT_EQ(rdr->instrument_count(), 2u);
  EXPECT_EQ(rdr->time_count(), 1u);
  const auto close_fid = rdr->field_index("close");
  ASSERT_TRUE(close_fid.has_value());
  // securityID "33449" interned first -> inst 0.
  EXPECT_EQ(rdr->symbol_name(0), "33449");
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 0), 300.0);
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 1), 20.0);
}

TEST(DataOratsHistory, MoveFlushPreservesValues) {
  const std::string zip = make_orats_zip();
  const fs::path out = fs::temp_directory_path() / "atx_orats_move_out";
  fs::remove_all(out);
  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;
  auto st = load_orats_history(cfg);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  auto rdr = atx::tsdb::SegmentReader::attach((out / "2020-01-02.seg").string());
  ASSERT_TRUE(rdr.has_value()) << rdr.error().to_string();
  const auto close_fid = rdr->field_index("close");
  ASSERT_TRUE(close_fid.has_value());
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 0), 300.0);
  EXPECT_DOUBLE_EQ(rdr->value(*close_fid, 0, 1), 20.0);
}

TEST(DataOratsHistory, RejectsNonMonotonicDates) {
  // Two rows, both >= floor, but the dates regress (01-03 then 01-02). The input
  // contract is date-major; a regression must fail closed with InvalidArgument.
  std::string body = std::string(kHeader) + "\n";
  body += make_orats_row("2020-01-03", "33449", "AAPL", "AAPL", 303.0, 1.0, 4000000000);
  body += make_orats_row("2020-01-02", "33449", "AAPL", "AAPL", 300.0, 1.0, 4000000000);
  const std::string zip = write_orats_zip(body, "atx_orats_ooo.zip");

  const fs::path out = fs::temp_directory_path() / "atx_orats_ooo_out";
  fs::remove_all(out);

  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;

  auto st = load_orats_history(cfg);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(DataOratsHistory, FramingAcrossBufferBoundary) {
  std::string body = std::string(kHeader) + "\n";
  // ~50k rows on one date >> any single inflate read won't align to line ends.
  constexpr int kRows = 50000;
  for (int i = 0; i < kRows; ++i) {
    body += make_orats_row("2020-01-02", std::to_string(20000 + i).c_str(),
                           "T", "T", 100.0 + i, 1.0, 1000000);
  }
  const std::string zip = write_orats_zip(body, "atx_orats_boundary.zip");
  const fs::path out = fs::temp_directory_path() / "atx_orats_boundary_out";
  fs::remove_all(out);
  OratsLoadConfig cfg;
  cfg.zip_path = zip;
  cfg.out_dir = out.string();
  cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
  cfg.created_at_nanos = 0;
  auto st = load_orats_history(cfg);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();
  EXPECT_EQ(st->rows_kept, kRows);
  EXPECT_EQ(st->dates_written, 1);
}

TEST(DataOratsHistory, ParallelOutputIsDeterministicAcrossRuns) {
  // Multi-date fixture: 40 dates x 500 symbols — enough to exercise the queue
  // and multiple workers concurrently.
  std::string body = std::string(kHeader) + "\n";
  for (int d = 0; d < 40; ++d) {
    char date[11];
    std::snprintf(date, sizeof(date), "2020-%02d-%02d", 1 + d / 28, 1 + d % 28);
    for (int s = 0; s < 500; ++s) {
      body += make_orats_row(date, std::to_string(30000 + s).c_str(), "T", "T",
                             100.0 + s + d, 1.0, 1000000);
    }
  }
  const std::string zip = write_orats_zip(body, "atx_orats_det.zip");

  auto run = [&](const char *tag) {
    const fs::path out = fs::temp_directory_path() / (std::string("atx_orats_det_") + tag);
    fs::remove_all(out);
    OratsLoadConfig cfg;
    cfg.zip_path = zip;
    cfg.out_dir = out.string();
    cfg.min_date_nanos = *detail::date_to_nanos("2020-01-01");
    cfg.created_at_nanos = 0;
    auto st = load_orats_history(cfg);
    // ASSERT_TRUE can't be used here (lambda returns fs::path, not void).
    // ADD_FAILURE works in any return type and surfaces the real loader error
    // immediately, instead of a confusing downstream "missing in run b".
    if (!st.has_value()) {
      ADD_FAILURE() << "load (" << tag << ") failed: " << st.error().to_string();
    }
    return out;
  };

  const fs::path a = run("a");
  const fs::path b = run("b");

  // Every .seg and the symbology parquet must be byte-identical across runs.
  for (const auto &e : fs::directory_iterator(a)) {
    const fs::path rel = e.path().filename();
    const fs::path bp = b / rel;
    ASSERT_TRUE(fs::exists(bp)) << "missing in run b: " << rel.string();
    ASSERT_EQ(fs::file_size(e.path()), fs::file_size(bp)) << "size differs: " << rel.string();
    std::ifstream fa(e.path(), std::ios::binary), fb(bp, std::ios::binary);
    std::string sa((std::istreambuf_iterator<char>(fa)), {});
    std::string sb((std::istreambuf_iterator<char>(fb)), {});
    EXPECT_EQ(sa, sb) << "content differs: " << rel.string();
  }
}
