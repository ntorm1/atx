// atx-engine — ORATS loader throughput benchmark. Generates a synthetic
// date-major ORATS TSV (N_DATES x N_SYMBOLS) into a temp .zip ONCE, then times
// load_orats_history end-to-end. Set ATX_ORATS_PROFILE=1 to also print the
// inflate/parse/build_write split to stderr.
#include <benchmark/benchmark.h>
#include <miniz.h>

#include <filesystem>
#include <string>

#include "atx/engine/data/orats_history.hpp"

namespace {
namespace fs = std::filesystem;

// Real 71-column header (mirrors data_orats_history_test.cpp::kHeader).
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

// Build the TSV body: kDates dates (advancing 1 day from 2020-01-02), each with
// kSymbols rows. Only the projected columns carry non-zero values; the rest are
// "0" so the row width matches the header.
std::string make_body(int kDates, int kSymbols) {
  std::string body;
  body.reserve(static_cast<size_t>(kDates) * kSymbols * 160);
  body += kHeader;
  body += '\n';
  // 2020-01-02 is unix-day 18263.
  for (int d = 0; d < kDates; ++d) {
    // Render YYYY-MM-DD by stepping a simple counter; the loader only needs a
    // valid, non-decreasing date string. Use 2020 + month/day arithmetic via a
    // fixed base; here we keep it inside a single year window for the bench
    // (kDates <= 336 keeps us within 2020).
    const int month = 1 + (d / 28);
    const int day = 1 + (d % 28);
    char date[11];
    std::snprintf(date, sizeof(date), "2020-%02d-%02d", month, day);
    for (int s = 0; s < kSymbols; ++s) {
      const int secid = 10001 + s;
      // tradingDate, securityID, ticker_tk, todayTicker
      body += date; body += '\t';
      body += std::to_string(secid); body += "\tT"; body += std::to_string(secid);
      body += "\tT"; body += std::to_string(secid); body += '\t';
      // remaining 67 columns: put a price in close (idx 8) and volume (idx 10),
      // zeros elsewhere. Columns 4..70 (0-based) after the first 4.
      for (int c = 4; c < 71; ++c) {
        if (c == 8) body += "123.45";
        else if (c == 10) body += "1000000";
        else body += '0';
        if (c < 70) body += '\t';
      }
      body += '\n';
    }
  }
  return body;
}

std::string write_zip_once(int kDates, int kSymbols) {
  static std::string cached;
  if (!cached.empty()) return cached;
  const fs::path p = fs::temp_directory_path() / "atx_orats_bench.zip";
  std::error_code ec;
  fs::remove(p, ec);
  const std::string body = make_body(kDates, kSymbols);
  mz_zip_archive zip{};
  mz_zip_writer_init_file(&zip, p.string().c_str(), 0);
  mz_zip_writer_add_mem(&zip, "tbltickerhistory3_10y.txt", body.data(), body.size(),
                        MZ_DEFAULT_LEVEL);
  mz_zip_writer_finalize_archive(&zip);
  mz_zip_writer_end(&zip);
  cached = p.string();
  return cached;
}

void BM_OratsLoad(benchmark::State &state) {
  const int kDates = static_cast<int>(state.range(0));
  const int kSymbols = static_cast<int>(state.range(1));
  const std::string zip = write_zip_once(kDates, kSymbols);
  const auto min_date = atx::engine::data::detail::date_to_nanos("2020-01-01");
  if (!min_date.has_value()) { state.SkipWithError("date_to_nanos failed"); return; }
  for (auto _ : state) {
    const fs::path out = fs::temp_directory_path() / "atx_orats_bench_out";
    std::error_code ec; fs::remove_all(out, ec);
    atx::engine::data::OratsLoadConfig cfg;
    cfg.zip_path = zip;
    cfg.out_dir = out.string();
    cfg.min_date_nanos = *min_date;
    cfg.created_at_nanos = 0;
    auto st = atx::engine::data::load_orats_history(cfg);
    if (!st) state.SkipWithError(st.error().to_string().c_str());
    benchmark::DoNotOptimize(st);
  }
  state.SetLabel(std::to_string(kDates) + "d x " + std::to_string(kSymbols) + "sym");
}
BENCHMARK(BM_OratsLoad)
    ->Args({240, 3000})   // ~720k rows, ~240 .seg files
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
} // namespace
