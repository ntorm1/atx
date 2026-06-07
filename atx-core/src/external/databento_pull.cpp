#include "atx/external/databento.hpp"

// Live-pull surface (estimate_cost + pull_equity_l1_1m_to_parquet) backed by the
// official Databento C++ client. Kept in a SEPARATE translation unit from the
// self-contained zip loader in databento.cpp: that file includes <miniz.h>, whose
// zlib-compatibility macros (crc32/adler32/z_stream/...) collide with the <zlib.h>
// that the databento client drags in transitively (record.hpp -> exceptions.hpp ->
// httplib.h -> zlib.h). The two cannot coexist in one TU, so they are split here.

#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <databento/datetime.hpp>
#include <databento/dbn.hpp>
#include <databento/enums.hpp>
#include <databento/historical.hpp>
#include <databento/metadata.hpp>
#include <databento/record.hpp>
#include <databento/symbol_map.hpp>
#include <databento/timeseries.hpp>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet_writer.hpp"

namespace atx::external::databento {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace time = atx::core::time;

namespace {

[[nodiscard]] Result<::databento::Schema> schema_from_string(std::string_view s) {
  if (s == "bbo-1m") return Ok(::databento::Schema::Bbo1M);
  if (s == "cbbo-1m") return Ok(::databento::Schema::Cbbo1M);
  if (s == "bbo-1s") return Ok(::databento::Schema::Bbo1S);
  if (s == "cbbo-1s") return Ok(::databento::Schema::Cbbo1S);
  return Err(ErrorCode::InvalidArgument, std::string{"unsupported schema: "} + std::string{s});
}

[[nodiscard]] Result<::databento::SType> stype_from_string(std::string_view s) {
  if (s == "raw_symbol") return Ok(::databento::SType::RawSymbol);
  if (s == "parent") return Ok(::databento::SType::Parent);
  if (s == "instrument_id") return Ok(::databento::SType::InstrumentId);
  return Err(ErrorCode::InvalidArgument, std::string{"unsupported stype: "} + std::string{s});
}

struct L1Columns {
  std::vector<time::Timestamp> ts;
  std::vector<std::string> symbol;
  std::vector<i64> bid_px, ask_px, bid_sz, ask_sz;
  void push(u64 ts_event, std::string sym, i64 bpx, i64 apx, u32 bsz, u32 asz) {
    ts.push_back(time::Timestamp::from_unix_nanos(static_cast<i64>(ts_event)));
    symbol.push_back(std::move(sym));
    bid_px.push_back(bpx);
    ask_px.push_back(apx);
    bid_sz.push_back(static_cast<i64>(bsz));
    ask_sz.push_back(static_cast<i64>(asz));
  }
};

constexpr i64 kUnsetPx = std::numeric_limits<i64>::min();

[[nodiscard]] i64 px_or_unset(std::int64_t px) {
  return px == ::databento::kUndefPrice ? kUnsetPx : static_cast<i64>(px);
}

[[nodiscard]] ::databento::Historical make_client(std::string_view api_key) {
  return ::databento::Historical::Builder().SetKey(std::string{api_key}).Build();
}

} // namespace

Result<double> estimate_cost(std::string_view api_key, std::string_view dataset,
                             const std::pair<std::string, std::string>& range_utc,
                             std::span<const std::string> symbols, std::string_view schema,
                             std::string_view stype_in) {
  ATX_TRY(auto sch, schema_from_string(schema));
  ATX_TRY(auto sty, stype_from_string(stype_in));
  try {
    auto client = make_client(api_key);
    const ::databento::DateTimeRange<std::string> range{range_utc.first, range_utc.second};
    const std::vector<std::string> syms(symbols.begin(), symbols.end());
    const double cost = client.MetadataGetCost(std::string{dataset}, range, syms, sch, sty, 0);
    return Ok(cost);
  } catch (const std::exception& e) {
    return Err(ErrorCode::Internal, std::string{"estimate_cost: "} + e.what());
  }
}

Result<PullStats> pull_equity_l1_1m_to_parquet(
    std::string_view api_key, std::string_view dataset, std::span<const std::string> symbols,
    const std::pair<std::string, std::string>& range_utc, std::string_view schema,
    std::string_view out_path, double cap_usd) {
  ATX_TRY(auto sch, schema_from_string(schema));
  const auto sty = ::databento::SType::RawSymbol;
  PullStats stats;
  L1Columns c;
  try {
    auto client = make_client(api_key);
    const ::databento::DateTimeRange<std::string> range{range_utc.first, range_utc.second};
    auto est = [&](std::span<const std::string> b) -> double {
      const std::vector<std::string> v(b.begin(), b.end());
      return client.MetadataGetCost(std::string{dataset}, range, v, sch, sty, 0);
    };
    const auto batches = split_under_cap(symbols, cap_usd, est);
    for (const auto& batch : batches) {
      stats.cost_usd += est(std::span<const std::string>(batch));
      ::databento::TsSymbolMap tsmap;
      client.TimeseriesGetRange(
          std::string{dataset}, range, batch, sch, sty, ::databento::SType::InstrumentId, 0,
          [&](::databento::Metadata&& m) { tsmap = m.CreateSymbolMap(); },
          [&](const ::databento::Record& rec) {
            if (rec.Holds<::databento::CbboMsg>()) {
              const auto& q = rec.Get<::databento::CbboMsg>();
              const auto& l = q.levels[0];
              const auto it = tsmap.Find(q);
              c.push(q.ts_recv.time_since_epoch().count(),
                     it != tsmap.Map().end() ? *it->second
                                             : std::to_string(q.hd.instrument_id),
                     px_or_unset(l.bid_px), px_or_unset(l.ask_px), l.bid_sz, l.ask_sz);
            } else if (rec.Holds<::databento::BboMsg>()) {
              const auto& q = rec.Get<::databento::BboMsg>();
              const auto& l = q.levels[0];
              const auto it = tsmap.Find(q);
              c.push(q.ts_recv.time_since_epoch().count(),
                     it != tsmap.Map().end() ? *it->second
                                             : std::to_string(q.hd.instrument_id),
                     px_or_unset(l.bid_px), px_or_unset(l.ask_px), l.bid_sz, l.ask_sz);
            }
            return ::databento::KeepGoing::Continue;
          });
      ++stats.api_calls;
    }
  } catch (const std::exception& e) {
    return Err(ErrorCode::Internal, std::string{"pull_equity_l1: "} + e.what());
  }
  stats.records = static_cast<i64>(c.ts.size());
  const std::vector<atx::core::io::WriteColumn> cols{
      {"ts", std::span<const time::Timestamp>(c.ts)},
      {"symbol", std::span<const std::string>(c.symbol)},
      {"bid_px", std::span<const i64>(c.bid_px)},
      {"ask_px", std::span<const i64>(c.ask_px)},
      {"bid_sz", std::span<const i64>(c.bid_sz)},
      {"ask_sz", std::span<const i64>(c.ask_sz)},
  };
  auto w = atx::core::io::write_parquet(cols, out_path);
  if (!w.has_value()) {
    return Err(w.error());
  }
  return Ok(stats);
}

} // namespace atx::external::databento
