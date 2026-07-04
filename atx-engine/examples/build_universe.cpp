// build_universe — EQUS zip -> OHLC1D, top-100 by 20-day median notional, then
// equity L1 (OHLC1M) + OPRA cbbo (OPRA_BBO) snapshots at 15:55 ET on the last day.
// Each Databento query is preflight-gated < $2. Key from DATABENTO_API_KEY.
//
// Usage: build_universe [DATA_ROOT] [EQUS_ZIP]

// std::getenv is standard C++; silence MSVC's platform-specific deprecation nag.
#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/disk.hpp"
#include "atx/engine/data/universe.hpp"
#include "atx/external/databento.hpp"

using namespace atx::engine::data;
namespace dbn = atx::external::databento;
namespace io = atx::core::io;
using atx::i64;

namespace {
const char* getenv_or(const char* k, const char* d) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : d;
}
struct EquityFeed { std::string dataset, schema; };
EquityFeed pick_equity_feed(const std::string& key,
                            const std::pair<std::string,std::string>& range,
                            const std::string& probe_symbol) {
  const std::vector<EquityFeed> candidates{
      {"DBEQ.BASIC", "cbbo-1m"}, {"EQUS.MINI", "cbbo-1m"}, {"XNAS.ITCH", "bbo-1m"}};
  const std::vector<std::string> one{probe_symbol};
  for (const auto& f : candidates) {
    auto c = dbn::estimate_cost(key, f.dataset, range, one, f.schema, "raw_symbol");
    if (c.has_value()) {
      std::printf("Equity feed: %s %s (probe cost $%.6f)\n", f.dataset.c_str(),
                  f.schema.c_str(), *c);
      return f;
    }
  }
  std::printf("WARN: no equity feed probed cleanly; defaulting XNAS.ITCH bbo-1m\n");
  return {"XNAS.ITCH", "bbo-1m"};
}
} // namespace

int main(int argc, char** argv) {
  const std::string data_root = argc > 1 ? argv[1] : "data/universe";
  const std::string zip = argc > 2 ? argv[2]
      : "C:/Users/natha/Downloads/EQUS-20260606-CEXECEMBY6.zip";
  const std::string key = getenv_or("DATABENTO_API_KEY", "");
  if (key.empty()) { std::fprintf(stderr, "DATABENTO_API_KEY not set\n"); return 1; }

  auto store_r = DiskStore::open(data_root);
  if (!store_r.has_value()) { std::fprintf(stderr, "open: %s\n", store_r.error().message().c_str()); return 1; }
  DiskStore store = std::move(*store_r);

  auto load = dbn::load_equs_summary_zip(zip, store.store_dir(Store::Ohlc1D).string());
  if (!load.has_value()) { std::fprintf(stderr, "load zip: %s\n", load.error().message().c_str()); return 1; }
  std::printf("OHLC1D: %lld files, %lld records, %lld partitions\n",
              (long long)load->files_processed, (long long)load->records_decoded,
              (long long)load->partitions_written);

  auto dates_r = store.list_dates(Store::Ohlc1D);
  if (!dates_r.has_value() || dates_r->empty()) { std::fprintf(stderr, "no OHLC1D dates\n"); return 1; }
  const std::vector<std::string>& dates = *dates_r;
  const std::string last = dates.back();
  const std::size_t window = dates.size() < 20 ? dates.size() : 20;
  std::printf("OHLC1D dates=%zu last=%s window=%zu\n", dates.size(), last.c_str(), window);

  std::unordered_map<std::string, std::vector<double>> notionals;
  for (std::size_t i = dates.size() - window; i < dates.size(); ++i) {
    auto lazy = store.scan_partition(Store::Ohlc1D, dates[i]);
    if (!lazy.has_value()) { continue; }
    auto tbl = lazy->collect();
    if (!tbl.has_value()) { continue; }
    auto syms = tbl->strings("symbol");
    auto close = tbl->column_view<i64>("close");
    auto vol = tbl->column_view<i64>("volume");
    if (!syms.has_value() || !close.has_value() || !vol.has_value()) { continue; }
    const auto n = static_cast<std::size_t>(tbl->num_rows());
    for (std::size_t r = 0; r < n; ++r) {
      const double notional = (static_cast<double>((*close)[r]) * 1e-9) *
                              static_cast<double>((*vol)[r]);
      notionals[std::string{(*syms)[r]}].push_back(notional);
    }
  }
  auto picks = top_n_by_median_notional(notionals, 100);
  std::printf("picked %zu symbols\n", picks.size());

  const std::pair<std::string,std::string> range{last + "T19:55:00", last + "T19:56:00"};

  const EquityFeed feed = pick_equity_feed(key, range, picks.empty() ? "AAPL" : picks.front());
  auto eq = dbn::pull_equity_l1_1m_to_parquet(
      key, feed.dataset, picks, range, feed.schema,
      store.partition_path(Store::Ohlc1M, last).string(), 2.0);
  if (!eq.has_value()) { std::fprintf(stderr, "equity L1: %s\n", eq.error().message().c_str()); }
  else { std::printf("OHLC1M: %lld rows, %lld calls, $%.6f\n",
                     (long long)eq->records, (long long)eq->api_calls, eq->cost_usd); }

  auto op = dbn::pull_opra_cbbo_1m_to_parquet(
      key, picks, range, store.partition_path(Store::OpraBbo, last).string(), 2.0);
  if (!op.has_value()) { std::fprintf(stderr, "OPRA cbbo: %s\n", op.error().message().c_str()); }
  else { std::printf("OPRA_BBO: %lld rows, %lld calls, $%.6f\n",
                     (long long)op->records, (long long)op->api_calls, op->cost_usd); }

  return 0;
}
