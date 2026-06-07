// build_ochain — join OPRA_BBO option quotes onto OHLC1M underlying quotes for
// one date partition, compute ACT/252 time-to-expiry, mid-price implied vol and
// Black-Scholes greeks for every row in parallel, and write the OCHAIN partition.
//
// Usage: build_ochain [DATA_ROOT] [DATE]   (defaults: data/universe 2026-06-05)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/series/column.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/disk.hpp"
#include "atx/engine/quant/black_scholes.hpp"
#include "atx/engine/quant/osi.hpp"
#include "atx/engine/quant/trading_calendar.hpp"

using namespace atx::engine::data;
namespace io = atx::core::io;
namespace q = atx::engine::quant;
namespace atxtime = atx::core::time;
using atx::i64;
using atx::f64;

namespace {

// Flat r ~= US 3M risk-free rate on the snapshot date (2026-06-05). q=0 is a
// known simplification (no dividend schedule); it biases IV for high-yield names.
constexpr double kRate = 0.043;
constexpr double kDiv = 0.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

[[nodiscard]] double px_f64(i64 v) {  // 1e-9 fixed-point; non-positive/unset -> NaN
  return v > 0 ? static_cast<double>(v) * 1e-9 : kNaN;
}

[[nodiscard]] std::string strip_dot(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c != '.') {
      out.push_back(c);
    }
  }
  return out;
}

[[nodiscard]] std::string fmt_date(int y, int m, int d) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
  return std::string{buf};
}

} // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "data/universe";
  const std::string date = argc > 2 ? argv[2] : "2026-06-05";

  auto store_r = DiskStore::open(root);
  if (!store_r.has_value()) {
    std::fprintf(stderr, "open: %s\n", store_r.error().message().c_str());
    return 1;
  }
  const DiskStore store = std::move(*store_r);

  // --- underlying map: strip-dot(symbol) -> (bid_px,ask_px) ------------------
  auto equ_lazy = store.scan_partition(Store::Ohlc1M, date);
  if (!equ_lazy.has_value()) {
    std::fprintf(stderr, "OHLC1M: %s\n", equ_lazy.error().message().c_str());
    return 1;
  }
  auto equ_tbl_r = equ_lazy->collect();
  if (!equ_tbl_r.has_value()) {
    std::fprintf(stderr, "OHLC1M collect: %s\n", equ_tbl_r.error().message().c_str());
    return 1;
  }
  const io::ParquetTable equ = std::move(*equ_tbl_r);
  auto equ_sym = equ.strings("symbol");
  auto equ_bid = equ.column_view<i64>("bid_px");
  auto equ_ask = equ.column_view<i64>("ask_px");
  if (!equ_sym.has_value() || !equ_bid.has_value() || !equ_ask.has_value()) {
    std::fprintf(stderr, "OHLC1M columns missing\n");
    return 1;
  }
  // OPRA 'underlying' is already dot-stripped (osi_root form, e.g. BRK.B->BRKB,
  // see databento_pull.cpp). OHLC1M symbols may carry dots, so strip both sides
  // to the same root before joining.
  std::unordered_map<std::string, std::pair<i64, i64>> umap;
  umap.reserve(equ_sym->size());
  for (std::size_t i = 0; i < equ_sym->size(); ++i) {
    umap.emplace(strip_dot((*equ_sym)[i]), std::make_pair((*equ_bid)[i], (*equ_ask)[i]));
  }

  // --- OPRA chain ------------------------------------------------------------
  auto opt_lazy = store.scan_partition(Store::OpraBbo, date);
  if (!opt_lazy.has_value()) {
    std::fprintf(stderr, "OPRA_BBO: %s\n", opt_lazy.error().message().c_str());
    return 1;
  }
  auto opt_tbl_r = opt_lazy->collect();
  if (!opt_tbl_r.has_value()) {
    std::fprintf(stderr, "OPRA_BBO collect: %s\n", opt_tbl_r.error().message().c_str());
    return 1;
  }
  const io::ParquetTable opt = std::move(*opt_tbl_r);
  auto ts_r = opt.to_column<atxtime::Timestamp>("ts");
  auto under = opt.strings("underlying");
  auto osi = opt.strings("symbol");
  auto obid_i = opt.column_view<i64>("bid_px");
  auto oask_i = opt.column_view<i64>("ask_px");
  if (!ts_r.has_value() || !under.has_value() || !osi.has_value() || !obid_i.has_value() ||
      !oask_i.has_value()) {
    std::fprintf(stderr, "OPRA_BBO columns missing\n");
    return 1;
  }
  const atx::core::series::Column<atxtime::Timestamp> ts_col = std::move(*ts_r);
  const std::size_t n = osi->size();

  // observation date (partition) -> (y,m,d)
  const int oy = std::stoi(date.substr(0, 4));
  const int om = std::stoi(date.substr(5, 2));
  const int od = std::stoi(date.substr(8, 2));

  // --- output buffers --------------------------------------------------------
  std::vector<std::string> c_underlying(n), c_expiry(n), c_callput(n);
  std::vector<f64> c_strike(n), c_obid(n), c_oask(n), c_ubid(n), c_uask(n), c_years(n),
      c_iv(n), c_de(n), c_ga(n), c_ve(n), c_th(n);

  // --- parallel per-row compute ---------------------------------------------
  const unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
  const std::size_t chunk = (n + nthreads - 1) / nthreads;
  const auto work = [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      const std::string u{(*under)[i]};
      c_underlying[i] = u;
      const double obid = px_f64((*obid_i)[i]);
      const double oask = px_f64((*oask_i)[i]);
      c_obid[i] = obid;
      c_oask[i] = oask;

      const auto parsed = q::parse_osi((*osi)[i]);
      double ubid = kNaN, uask = kNaN, strike = kNaN, years = kNaN, iv = kNaN;
      q::Greeks g{kNaN, kNaN, kNaN, kNaN};
      if (parsed.has_value()) {
        c_expiry[i] = fmt_date(parsed->year, parsed->month, parsed->day);
        c_callput[i] = parsed->is_call ? "C" : "P";
        strike = parsed->strike;
        years = q::act252_years(oy, om, od, parsed->year, parsed->month, parsed->day);
        const auto it = umap.find(strip_dot(u));
        if (it != umap.end()) {
          ubid = px_f64(it->second.first);
          uask = px_f64(it->second.second);
        }
        const double S = 0.5 * (ubid + uask);
        const double mid = 0.5 * (obid + oask);
        if (std::isfinite(S) && std::isfinite(mid) && years > 0.0) {
          iv = q::implied_vol(mid, S, strike, years, kRate, kDiv, parsed->is_call);
          if (std::isfinite(iv)) {
            g = q::bs_greeks(S, strike, years, kRate, kDiv, iv, parsed->is_call);
          }
        }
      } else {
        c_expiry[i].clear();
        c_callput[i].clear();
      }
      c_strike[i] = strike;
      c_ubid[i] = ubid;
      c_uask[i] = uask;
      c_years[i] = years;
      c_iv[i] = iv;
      c_de[i] = g.delta;
      c_ga[i] = g.gamma;
      c_ve[i] = g.vega;
      c_th[i] = g.theta;
    }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < nthreads; ++t) {
    const std::size_t lo = static_cast<std::size_t>(t) * chunk;
    const std::size_t hi = std::min(n, lo + chunk);
    if (lo >= hi) {
      break;
    }
    pool.emplace_back(work, lo, hi);
  }
  for (auto& th : pool) {
    th.join();
  }

  // --- write OCHAIN ----------------------------------------------------------
  const std::vector<std::string> c_date(n, date);
  const std::vector<io::WriteColumn> cols{
      {"timestamp", ts_col.view()},
      {"date", std::span<const std::string>(c_date)},
      {"underlying", std::span<const std::string>(c_underlying)},
      {"expiry", std::span<const std::string>(c_expiry)},
      {"call_put", std::span<const std::string>(c_callput)},
      {"strike", std::span<const f64>(c_strike)},
      {"obid", std::span<const f64>(c_obid)},
      {"oask", std::span<const f64>(c_oask)},
      {"ubid", std::span<const f64>(c_ubid)},
      {"uask", std::span<const f64>(c_uask)},
      {"years", std::span<const f64>(c_years)},
      {"mid_iv", std::span<const f64>(c_iv)},
      {"de", std::span<const f64>(c_de)},
      {"ga", std::span<const f64>(c_ga)},
      {"ve", std::span<const f64>(c_ve)},
      {"th", std::span<const f64>(c_th)},
  };
  const std::string out = store.partition_path(Store::OChain, date).string();
  auto w = io::write_parquet(cols, out);
  if (!w.has_value()) {
    std::fprintf(stderr, "write OCHAIN: %s\n", w.error().message().c_str());
    return 1;
  }

  std::size_t solved = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (std::isfinite(c_iv[i])) {
      ++solved;
    }
  }
  std::printf("OCHAIN %s: %zu rows, %zu IV solved, %zu NaN, threads=%u -> %s\n", date.c_str(), n,
              solved, n - solved, nthreads, out.c_str());
  return 0;
}
