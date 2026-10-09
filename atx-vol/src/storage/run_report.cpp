#include "atx/vol/tools/run_report.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "atx/core/error.hpp" // Err, Ok, ErrorCode

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// `%.10g` for headline metric scalars (10 significant digits; readable, not
// meant to round-trip bit-exactly).
[[nodiscard]] std::string fmt10(double v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.10g", v);
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

[[nodiscard]] std::string fmt_i64(std::int64_t v) {
  char buf[32];
  const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

[[nodiscard]] std::string fmt_u64(std::uint64_t v) {
  char buf[32];
  const int len = std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(v));
  return std::string(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Single shared writer for the "meta+header+rows" file shape: prepend one
// `# key=value` line per `meta` entry (given order), then the caller-
// assembled `body` (header row + data rows, already `\n`-terminated). Binary
// mode avoids CRLF translation so the byte stream — and its `\n` line
// endings — is identical on every platform. Every public writer below goes
// through this; no per-writer duplication of the open/prepend/write/error
// sequence.
[[nodiscard]] Status write_meta_body(const MetaKv &meta, const std::string &body,
                                     std::string_view path, const char *who) {
  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return Err(ErrorCode::IoError, std::string(who) + ": cannot open file");
  }

  std::string out;
  out.reserve(body.size() + meta.size() * 32 + 16);
  for (const auto &[k, v] : meta) {
    out += "# ";
    out += k;
    out += '=';
    out += v;
    out += '\n';
  }
  out += body;

  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os) {
    return Err(ErrorCode::IoError, std::string(who) + ": write failed");
  }
  return Ok();
}

} // namespace

Status write_backtest_series_csv(const BacktestResult &r, const MetaKv &meta,
                                 std::string_view path) {
  // Plan 4.6: the row loop below indexes every column at the same row, so a
  // skewed result used to read out of range instead of reporting a shape error.
  ATX_TRY_VOID(r.validate());
  // Fixed column order (must match the header string below): the double
  // columns after date/ts_ns, in the pinned order from run_report.hpp.
  const std::pair<const char *, const std::vector<double> *> dbl_cols[] = {
      {"pnl_total", &r.pnl_total},
      {"nav", &r.nav},
      {"pnl_delta", &r.pnl_delta},
      {"pnl_gamma", &r.pnl_gamma},
      {"pnl_vega", &r.pnl_vega},
      {"pnl_vanna", &r.pnl_vanna},
      {"pnl_volga", &r.pnl_volga},
      {"pnl_theta", &r.pnl_theta},
      {"pnl_rho", &r.pnl_rho},
      {"pnl_charm", &r.pnl_charm},
      {"pnl_unexplained", &r.pnl_unexplained},
      {"pnl_settlement", &r.pnl_settlement},
      {"pnl_shares", &r.pnl_shares},
      {"financing", &r.financing},
      {"cost", &r.cost},
      {"cash", &r.cash},
      {"gross_delta", &r.gross_delta},
      {"gross_gamma", &r.gross_gamma},
      {"gross_vega", &r.gross_vega},
      {"gross_theta", &r.gross_theta},
      {"turnover_notional", &r.turnover_notional},
      {"turnover_vega", &r.turnover_vega},
      {"n_open_lots", &r.n_open_lots},
      {"n_unpriced_lots", &r.n_unpriced_lots},
      {"n_unpriced_greeks", &r.n_unpriced_greeks},
  };

  std::string body;
  body.reserve(r.size() * 512 + 256);

  char buf[64];
  const auto put_double = [&](double v) {
    const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
    body.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
  };

  // ── Header ──
  body += "date,ts_ns";
  for (const auto &[name, col] : dbl_cols) {
    (void)col;
    body += ',';
    body += name;
  }
  for (const auto &sig : r.signals) {
    body += ',';
    body += sig.first;
  }
  body += '\n';

  // ── Data rows ──
  for (std::size_t i = 0; i < r.size(); ++i) {
    body += r.date[i];
    body += ',';
    const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(r.ts_ns[i]));
    body.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
    for (const auto &[name, col] : dbl_cols) {
      (void)name;
      body += ',';
      put_double((*col)[i]);
    }
    for (const auto &sig : r.signals) {
      body += ',';
      put_double(sig.second[i]);
    }
    body += '\n';
  }

  return write_meta_body(meta, body, path, "write_backtest_series_csv");
}

Status write_metrics_csv(const MetaKv &meta, const MetaKv &metrics, std::string_view path) {
  std::string body;
  body.reserve(metrics.size() * 24 + 16);
  body += "metric,value\n";
  for (const auto &[k, v] : metrics) {
    body += k;
    body += ',';
    body += v;
    body += '\n';
  }
  return write_meta_body(meta, body, path, "write_metrics_csv");
}

MetaKv strategy_metrics(const TearSheet &ts) {
  return {
      {"total_return", fmt10(ts.total_return)},
      {"ann_return", fmt10(ts.ann_return)},
      {"ann_vol", fmt10(ts.ann_vol)},
      {"sharpe", fmt10(ts.sharpe)},
      {"max_drawdown", fmt10(ts.max_drawdown)},
      {"hit_rate", fmt10(ts.hit_rate)},
      {"avg_turnover", fmt10(ts.avg_turnover)},
      {"total_cost", fmt10(ts.total_cost)},
      {"total_financing", fmt10(ts.total_financing)},
      {"attr_delta", fmt10(ts.attr_delta)},
      {"attr_gamma", fmt10(ts.attr_gamma)},
      {"attr_vega", fmt10(ts.attr_vega)},
      {"attr_vanna", fmt10(ts.attr_vanna)},
      {"attr_volga", fmt10(ts.attr_volga)},
      {"attr_theta", fmt10(ts.attr_theta)},
      {"attr_rho", fmt10(ts.attr_rho)},
      {"attr_charm", fmt10(ts.attr_charm)},
      {"attr_unexplained", fmt10(ts.attr_unexplained)},
      {"return_on_gross_vega", fmt10(ts.return_on_gross_vega)},
      {"vega_adj_sharpe", fmt10(ts.vega_adj_sharpe)},
      {"pnl_per_vega_traded", fmt10(ts.pnl_per_vega_traded)},
      {"avg_gross_vega", fmt10(ts.avg_gross_vega)},
      {"avg_gross_gamma", fmt10(ts.avg_gross_gamma)},
  };
}

MetaKv result_summary_metrics(const BacktestResult &r) {
  const std::size_t n = r.size();
  const double total_pnl = n > 0 ? r.nav.back() : 0.0;

  // True per-step average: `step_pnl_total` is the full per-step series retained at
  // any record stride (the recorded `pnl_total` rows are block sums when
  // record_every_n>1). Hand-built results fall back to the pnl_total rows.
  double daily_sum = 0.0;
  double avg_daily_pnl = 0.0;
  if (!r.step_pnl_total.empty()) {
    for (const double p : r.step_pnl_total) {
      daily_sum += p;
    }
    avg_daily_pnl = daily_sum / static_cast<double>(r.step_pnl_total.size());
  } else {
    for (std::size_t i = 1; i < n; ++i) {
      daily_sum += r.pnl_total[i];
    }
    avg_daily_pnl = n > 1 ? daily_sum / static_cast<double>(n - 1) : 0.0;
  }

  double vega_sum = 0.0;
  double theta_sum = 0.0;
  double lots_sum = 0.0;
  double peak_lots = 0.0;
  double unpriced_lots_sum = 0.0;
  double unpriced_greeks_sum = 0.0;
  std::size_t open_rows = 0;
  for (std::size_t i = 0; i < n; ++i) {
    lots_sum += r.n_open_lots[i];
    if (r.n_open_lots[i] > peak_lots) {
      peak_lots = r.n_open_lots[i];
    }
    if (r.n_open_lots[i] > 0.0) {
      vega_sum += r.gross_vega[i];
      theta_sum += r.gross_theta[i];
      ++open_rows;
    }
    unpriced_lots_sum += r.n_unpriced_lots[i];
    unpriced_greeks_sum += r.n_unpriced_greeks[i];
  }
  const double avg_net_vega = open_rows > 0 ? vega_sum / static_cast<double>(open_rows) : 0.0;
  const double avg_net_theta = open_rows > 0 ? theta_sum / static_cast<double>(open_rows) : 0.0;
  const double avg_open_lots = n > 0 ? lots_sum / static_cast<double>(n) : 0.0;

  return {
      {"total_pnl", fmt10(total_pnl)},
      {"avg_daily_pnl", fmt10(avg_daily_pnl)},
      {"avg_net_vega", fmt10(avg_net_vega)},
      {"avg_net_theta", fmt10(avg_net_theta)},
      {"avg_open_lots", fmt10(avg_open_lots)},
      {"peak_open_lots", fmt10(peak_lots)},
      {"total_unpriced_lots", fmt10(unpriced_lots_sum)},
      {"total_unpriced_greeks", fmt10(unpriced_greeks_sum)},
      {"n_steps", fmt10(static_cast<double>(n))},
  };
}

MetaKv engine_metrics(const EngineRunStats &s) {
  const double wall_s = s.wall_clock_ms / 1000.0;
  const double steps_per_s = wall_s > 0.0 ? static_cast<double>(s.n_steps) / wall_s : 0.0;
  return {
      {"wall_clock_ms", fmt10(s.wall_clock_ms)},
      {"steps_per_s", fmt10(steps_per_s)},
      {"n_steps", fmt10(static_cast<double>(s.n_steps))},
      {"cache_loads", fmt10(static_cast<double>(s.cache.loads))},
      {"cache_hits", fmt10(static_cast<double>(s.cache.hits))},
      {"cache_prefetches", fmt10(static_cast<double>(s.cache.prefetches))},
  };
}

Status write_surface_db_stats_csv(const SurfaceDb &db, const MetaKv &meta, std::string_view path) {
  std::vector<DbPartitionInfo> parts = db.partitions();
  std::sort(parts.begin(), parts.end(),
            [](const DbPartitionInfo &a, const DbPartitionInfo &b) { return a.key < b.key; });

  std::uint64_t total_file_size = 0;
  for (const auto &p : parts) {
    total_file_size += p.file_size;
  }

  MetaKv full_meta = meta;
  full_meta.emplace_back("db_root", db.root());
  full_meta.emplace_back("generation", fmt_u64(db.generation()));
  full_meta.emplace_back("n_symbols", fmt_u64(db.symbols().size()));
  full_meta.emplace_back("n_partitions", fmt_u64(parts.size()));
  full_meta.emplace_back("total_file_size", fmt_u64(total_file_size));

  std::string body;
  body.reserve(parts.size() * 48 + 64);
  body += "key,surface_count,file_size,created_ts_ns\n";
  for (const auto &p : parts) {
    body += p.key;
    body += ',';
    body += fmt_u64(p.surface_count);
    body += ',';
    body += fmt_u64(p.file_size);
    body += ',';
    body += fmt_i64(p.created_ts_ns);
    body += '\n';
  }

  return write_meta_body(full_meta, body, path, "write_surface_db_stats_csv");
}

} // namespace atx::vol
