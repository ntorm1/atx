#include "atx/external/databento.hpp"

#include <miniz.h>
#include <zstd.h>

#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/external/dbn.hpp"

namespace atx::external::databento {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace time = atx::core::time;

namespace {

[[nodiscard]] bool ends_with(std::string_view s, std::string_view suf) noexcept {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// Decompress a zstd buffer (one or more concatenated frames) into raw bytes.
[[nodiscard]] Result<std::vector<std::byte>> zstd_decompress(std::span<const std::byte> in) {
  ZSTD_DStream *ds = ZSTD_createDStream();
  if (ds == nullptr) {
    return Err(ErrorCode::Internal, "zstd: alloc DStream");
  }
  ZSTD_initDStream(ds);
  ZSTD_inBuffer ib{in.data(), in.size(), 0};
  std::vector<std::byte> out;
  std::vector<std::byte> tmp(ZSTD_DStreamOutSize());
  while (ib.pos < ib.size) {
    ZSTD_outBuffer ob{tmp.data(), tmp.size(), 0};
    const std::size_t rc = ZSTD_decompressStream(ds, &ob, &ib);
    if (ZSTD_isError(rc)) {
      ZSTD_freeDStream(ds);
      return Err(ErrorCode::ParseError, std::string{"zstd: "} + ZSTD_getErrorName(rc));
    }
    out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(ob.pos));
    if (rc == 0 && ib.pos == ib.size) {
      break;
    }
  }
  ZSTD_freeDStream(ds);
  return Ok(std::move(out));
}

// Format ts_event (ns) as a "YYYY-MM-DD" partition value (UTC civil date).
[[nodiscard]] std::string date_string(u64 ts_event_ns) {
  const auto ct =
      time::to_civil_utc(time::Timestamp::from_unix_nanos(static_cast<i64>(ts_event_ns)));
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", ct.date.year, ct.date.month, ct.date.day);
  return std::string{buf};
}

struct Columns {
  std::vector<time::Timestamp> ts;
  std::vector<std::string> symbol;
  std::vector<i64> open, high, low, close, volume;
  std::vector<std::string> date;
};

// Decode one decompressed DBN buffer into the column accumulators. The date
// string is memoized across the record loop: daily-bar entries are one trading
// day, so this is one snprintf per distinct day instead of one per row.
[[nodiscard]] Result<void> decode_into(std::span<const std::byte> dbn, Columns &c, i64 &decoded,
                                       i64 &skipped) {
  ATX_TRY(auto dec, dbn::DbnDecoder::open(dbn));
  constexpr i64 kNsPerDay = 86'400'000'000'000LL;
  i64 cached_day = std::numeric_limits<i64>::min();
  std::string cached_date;
  for (;;) {
    ATX_TRY(auto rec, dec.next());
    if (!rec.has_value()) {
      break;
    }
    const auto &m = *rec;
    const i64 ns = static_cast<i64>(m.hd.ts_event);
    const i64 day = ns / kNsPerDay; // floor for ns >= 0 (daily bars are positive)
    if (day != cached_day) {
      cached_day = day;
      cached_date = date_string(m.hd.ts_event);
    }
    c.ts.push_back(time::Timestamp::from_unix_nanos(ns));
    const std::string_view sym = dec.symbol_for(m.hd.instrument_id);
    c.symbol.push_back(sym.empty() ? std::to_string(m.hd.instrument_id) : std::string{sym});
    c.open.push_back(m.open);
    c.high.push_back(m.high);
    c.low.push_back(m.low);
    c.close.push_back(m.close);
    c.volume.push_back(m.volume > static_cast<u64>(std::numeric_limits<i64>::max())
                           ? std::numeric_limits<i64>::max()
                           : static_cast<i64>(m.volume));
    c.date.push_back(cached_date);
    ++decoded;
  }
  skipped += dec.skipped_records();
  return Ok();
}

// Write one file's accumulated columns as hive-partitioned parquet (by date),
// returning the number of partitions written.
[[nodiscard]] Result<i64> write_columns(const Columns &c, std::string_view dest_dir) {
  const std::vector<atx::core::io::WriteColumn> wcols = {
      {"ts", std::span<const time::Timestamp>(c.ts)},
      {"symbol", std::span<const std::string>(c.symbol)},
      {"open", std::span<const i64>(c.open)},
      {"high", std::span<const i64>(c.high)},
      {"low", std::span<const i64>(c.low)},
      {"close", std::span<const i64>(c.close)},
      {"volume", std::span<const i64>(c.volume)},
      {"date", std::span<const std::string>(c.date)},
  };
  return atx::core::io::write_hive_parquet(wcols, dest_dir, "date");
}

} // namespace

Result<LoadStats> load_equs_summary_zip(std::string_view zip_path, std::string_view dest_dir) {
  namespace fs = std::filesystem;
  if (!fs::exists(fs::path{zip_path})) {
    return Err(ErrorCode::IoError, std::string{"zip not found: "} + std::string{zip_path});
  }

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, std::string{zip_path}.c_str(), 0)) {
    return Err(ErrorCode::ParseError, "failed to open zip archive");
  }

  LoadStats stats;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  i64 total_partitions = 0;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      continue;
    }
    const std::string_view name{static_cast<const char *>(st.m_filename)};
    const bool is_zst = ends_with(name, ".dbn.zst");
    const bool is_raw = ends_with(name, ".dbn");
    if (!is_zst && !is_raw) {
      continue;
    }

    std::size_t raw_size = 0;
    void *raw = mz_zip_reader_extract_to_heap(&zip, i, &raw_size, 0);
    if (raw == nullptr) {
      mz_zip_reader_end(&zip);
      return Err(ErrorCode::ParseError, std::string{"failed to extract "} + std::string{name});
    }
    const std::span<const std::byte> entry{static_cast<const std::byte *>(raw), raw_size};

    Columns cols; // ONE file's rows only -> peak memory bounded to one day
    Result<void> step = Ok();
    if (is_zst) {
      auto dec = zstd_decompress(entry);
      if (!dec.has_value()) {
        mz_free(raw);
        mz_zip_reader_end(&zip);
        return Err(dec.error());
      }
      step = decode_into(*dec, cols, stats.records_decoded, stats.records_skipped);
    } else {
      step = decode_into(entry, cols, stats.records_decoded, stats.records_skipped);
    }
    mz_free(raw);
    if (!step.has_value()) {
      mz_zip_reader_end(&zip);
      return Err(step.error());
    }
    // Write this entry's partitions before moving on, so a later malformed
    // entry cannot discard days that already decoded successfully.
    if (!cols.ts.empty()) {
      auto nparts = write_columns(cols, dest_dir);
      if (!nparts.has_value()) {
        mz_zip_reader_end(&zip);
        return Err(nparts.error());
      }
      total_partitions += *nparts;
    }
    ++stats.files_processed;
  }
  mz_zip_reader_end(&zip);

  if (stats.records_decoded == 0) {
    return Err(ErrorCode::InvalidArgument, "no OHLCV records found in archive");
  }
  stats.partitions_written = total_partitions;
  return Ok(stats);
}

} // namespace atx::external::databento
