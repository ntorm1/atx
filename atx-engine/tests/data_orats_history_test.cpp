#include <array>
#include <filesystem>
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
                           double close, double cumret, double shares) {
  std::array<std::string, 71> f;
  for (auto &x : f) x = "0";
  f[0] = date; f[1] = secid; f[2] = tk; f[3] = today;
  f[5] = "1"; f[6] = "1"; f[7] = "1";              // open/high/low
  f[8] = std::to_string(close);                    // close (col 9, idx 8)
  f[10] = std::to_string(static_cast<long long>(shares)); // volume placeholder
  f[11] = std::to_string(static_cast<long long>(shares)); // shares (idx 11)
  f[62] = "5";                                     // GICS (idx 62)
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
} // namespace

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
