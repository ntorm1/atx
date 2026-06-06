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

// Decode one decompressed DBN buffer into the column accumulators.
[[nodiscard]] Result<void> decode_into(std::span<const std::byte> dbn, Columns &c, i64 &decoded,
                                       i64 &skipped) {
  ATX_TRY(auto dec, dbn::DbnDecoder::open(dbn));
  for (;;) {
    ATX_TRY(auto rec, dec.next());
    if (!rec.has_value()) {
      break;
    }
    const auto &m = *rec;
    c.ts.push_back(time::Timestamp::from_unix_nanos(static_cast<i64>(m.hd.ts_event)));
    const std::string_view sym = dec.symbol_for(m.hd.instrument_id);
    c.symbol.push_back(sym.empty() ? std::to_string(m.hd.instrument_id) : std::string{sym});
    c.open.push_back(m.open);
    c.high.push_back(m.high);
    c.low.push_back(m.low);
    c.close.push_back(m.close);
    c.volume.push_back(m.volume > static_cast<u64>(std::numeric_limits<i64>::max())
                           ? std::numeric_limits<i64>::max()
                           : static_cast<i64>(m.volume));
    c.date.push_back(date_string(m.hd.ts_event));
    ++decoded;
  }
  skipped += dec.skipped_records();
  return Ok();
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

  Columns cols;
  LoadStats stats;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
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
    ++stats.files_processed;
  }
  mz_zip_reader_end(&zip);

  if (cols.ts.empty()) {
    return Err(ErrorCode::InvalidArgument, "no OHLCV records found in archive");
  }

  const std::vector<atx::core::io::WriteColumn> wcols = {
      {"ts", std::span<const time::Timestamp>(cols.ts)},
      {"symbol", std::span<const std::string>(cols.symbol)},
      {"open", std::span<const i64>(cols.open)},
      {"high", std::span<const i64>(cols.high)},
      {"low", std::span<const i64>(cols.low)},
      {"close", std::span<const i64>(cols.close)},
      {"volume", std::span<const i64>(cols.volume)},
      {"date", std::span<const std::string>(cols.date)},
  };
  ATX_TRY(auto nparts, atx::core::io::write_hive_parquet(wcols, dest_dir, "date"));
  stats.partitions_written = nparts;
  return Ok(stats);
}

} // namespace atx::external::databento
