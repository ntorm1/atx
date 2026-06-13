#pragma once

// atx::engine::book — reproducible operating-book reporting (S7-4, §4.5).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  Two cold-path pieces that turn a realized multi-period book chain into a
//  durable, byte-reproducible report:
//
//    * accumulate_report(...) — walk the rebalance schedule and roll the realized
//        book chain (risk::MultiPeriodResult) into a BookReport: net-of-cost
//        equity, per-period pnl attribution (gross / net / cost), gross-leverage /
//        net-exposure / turnover, the per-period book factor exposure Xᵀw, capacity
//        utilization, and a lifecycle-state census at an as-of snapshot.
//
//    * write_report(report, dir) — serialize the report to deterministic,
//        locale-INDEPENDENT, byte-identical-across-runs per-series files under dir
//        (the R8 reproducibility pin — see the formatting note below).
//
// ===========================================================================
//  Per-period accumulation (order-fixed, no-survivorship — R4)
// ===========================================================================
//  For schedule period s (book == books.books[s], realized at panel date
//  sched.periods[s]), reductions run in canonical ascending instrument order i:
//    * pnl_gross[s] = Σ_i book[i]·r[i], where r is the realized return cross-
//        section panel.field_cross_section(returns_field, sched.periods[s]). A NaN
//        r[i] — OR a name out of the point-in-time universe at that date — is
//        treated as 0 (no-survivorship: a missing/delisted name's holding earns
//        nothing; it never poisons the reduction with a NaN). Guarded by
//        book.size() == V.n_instruments() (else the report is left empty-row;
//        the s7-5 pipeline owns the dimension contract).
//    * pnl_cost[s] = books.cost_bps[s] · 1e-4. UNIT CONVENTION: MultiPeriodResult
//        .cost_bps[s] is the per-period charge in BASIS POINTS (turnover ×
//        round_trip_cost_bps, see risk::MultiPeriodOptimizer::run). pnl_gross is a
//        FRACTIONAL return (Σ weight·return), so we convert bps→fraction by ·1e-4
//        before subtracting. pnl_net[s] = pnl_gross[s] − pnl_cost[s].
//    * gross_leverage[s] = Σ_i |book[i]|; net_exposure[s] = Σ_i book[i];
//        turnover[s]      = books.turnover[s].
//    * equity_curve[s] = cumulative SUM of pnl_net (a net-return series; simplest
//        and matches an additive returns stream — NOT a (1+r) compounding product).
//    * capacity_utilization[s] = (capacity_gross > 0) ? gross/capacity : 0.
//    * factor_exposures.row(s) = (Xᵀ·book)ᵀ — a length-K (n_factors) row; the raw
//        per-period book factor exposure. (V.exposures() is M×K.)
//  lifecycle_census: a zero-init array<usize,6>; for a in [0, lib.n_alphas()),
//  state_as_of(AlphaId{a}, as_of) increments census[(usize)state] (ascending a).
//
//  DEFERRED RESIDUAL (YAGNI): the §4.5 OLS/Brinson factor-attribution regression
//  (splitting pnl into per-factor contributions) is NOT built here. factor_exposures
//  gives the raw Xᵀw per period; the attribution split is the deferred enhancement.
//
// ===========================================================================
//  Byte-identical writes (R8 — the crux)
// ===========================================================================
//  write_report formats every f64 with std::to_chars (shortest exact round-trip,
//  locale-INDEPENDENT — the SAME path alpha/unparse.hpp uses for literal hashing),
//  NOT std::ostream << (locale-dependent, lossy). Non-finite values serialize to a
//  FIXED token ("nan" / "inf" / "-inf") identically both runs. Files are opened in
//  BINARY mode and lines end in a literal "\n" (no CRLF translation on Windows).
//  No timestamps, no map iteration, no absolute paths inside the files; rows in
//  ascending period order. Same report ⇒ byte-identical files.
//
// ===========================================================================
//  Determinism / allocation
// ===========================================================================
//  NO RNG / clock / map. Cold path (report) — std::vector / std::string allocation
//  is fine. A filesystem failure returns Err(IoError); a panel/dim mismatch is
//  guarded (empty rows), not fatal.

#include <array>      // std::array (lifecycle census)
#include <charconv>   // std::to_chars (locale-free shortest round-trip)
#include <cmath>      // std::isnan, std::isfinite, std::fabs
#include <filesystem> // create dir, path join
#include <fstream>    // std::ofstream (binary write)
#include <span>       // std::span (realized return cross-section)
#include <string>     // file buffer
#include <system_error> // std::errc (to_chars_result::ec)
#include <vector>     // report series

#include <Eigen/Core> // Eigen::Index, MatX ops (Xᵀw)

#include "atx/core/error.hpp"         // Status, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/alpha/panel.hpp"       // alpha::Panel, FieldId
#include "atx/engine/combine/store.hpp"     // combine::AlphaId
#include "atx/engine/library/library.hpp"   // library::Library
#include "atx/engine/risk/factor_model.hpp" // risk::FactorModel (exposures())
#include "atx/engine/risk/multi_period.hpp" // risk::MultiPeriodResult, RebalanceSchedule

namespace atx::engine::book {

// ===========================================================================
//  BookReport — the per-period book diagnostics + an as-of lifecycle census.
// ===========================================================================
struct BookReport {
  std::vector<atx::f64> equity_curve;                 // net-of-cost cumulative (sum of pnl_net)
  std::vector<atx::f64> gross_leverage;               // Σ_i |book[i]| per period
  std::vector<atx::f64> net_exposure;                 // Σ_i book[i] per period
  std::vector<atx::f64> turnover;                     // books.turnover[s] per period
  std::vector<atx::f64> pnl_gross;                    // Σ_i book[i]·r[i] (NaN/non-live → 0)
  std::vector<atx::f64> pnl_net;                      // pnl_gross − pnl_cost
  std::vector<atx::f64> pnl_cost;                     // cost_bps · 1e-4 (bps → fraction)
  atx::core::linalg::MatX factor_exposures;           // per-period Xᵀw (rows=periods, cols=n_factors)
  std::vector<atx::f64> capacity_utilization;         // gross / capacity per period
  std::array<atx::usize, 6> lifecycle_census{};       // count per LifecycleState at as_of
};

// ===========================================================================
//  accumulate_report — roll the realized book chain into a BookReport.
// ===========================================================================
[[nodiscard]] inline BookReport
accumulate_report(const risk::MultiPeriodResult &books, const alpha::Panel &panel,
                  alpha::FieldId returns_field, const risk::RebalanceSchedule &sched,
                  const risk::FactorModel &V, atx::f64 capacity_gross, const library::Library &lib,
                  atx::usize as_of) {
  const atx::usize n = sched.periods.size();
  BookReport rep;
  rep.equity_curve.reserve(n);
  rep.gross_leverage.reserve(n);
  rep.net_exposure.reserve(n);
  rep.turnover.reserve(n);
  rep.pnl_gross.reserve(n);
  rep.pnl_net.reserve(n);
  rep.pnl_cost.reserve(n);
  rep.capacity_utilization.reserve(n);
  rep.factor_exposures =
      atx::core::linalg::MatX::Zero(static_cast<Eigen::Index>(n),
                                    static_cast<Eigen::Index>(V.n_factors()));

  atx::f64 equity = 0.0; // running cumulative net pnl
  for (atx::usize s = 0; s < n; ++s) {
    const std::vector<atx::f64> &book = books.books[s];
    const atx::usize date = sched.periods[s];
    const std::span<const atx::f64> r = panel.field_cross_section(returns_field, date);

    // Order-fixed reductions over the book; NaN return OR out-of-universe → 0 (R4).
    atx::f64 pnl_gross = 0.0;
    atx::f64 gross = 0.0;
    atx::f64 net = 0.0;
    for (atx::usize i = 0; i < book.size(); ++i) {
      const atx::f64 w = book[i];
      gross += std::fabs(w);
      net += w;
      const bool live = i < r.size() && !std::isnan(r[i]) && panel.in_universe(date, i);
      if (live) {
        pnl_gross += w * r[i];
      }
    }
    const atx::f64 cost = books.cost_bps[s] * 1e-4; // bps → fractional return
    const atx::f64 pnl_net = pnl_gross - cost;
    equity += pnl_net;

    rep.pnl_gross.push_back(pnl_gross);
    rep.pnl_cost.push_back(cost);
    rep.pnl_net.push_back(pnl_net);
    rep.equity_curve.push_back(equity);
    rep.gross_leverage.push_back(gross);
    rep.net_exposure.push_back(net);
    rep.turnover.push_back(books.turnover[s]);
    rep.capacity_utilization.push_back(capacity_gross > 0.0 ? gross / capacity_gross : 0.0);

    // Per-period book factor exposure Xᵀw → factor_exposures.row(s) (length K).
    // Guarded: only compute when the book matches the model's instrument count.
    if (book.size() == V.n_instruments()) {
      const Eigen::Map<const atx::core::linalg::VecX> w(book.data(),
                                                        static_cast<Eigen::Index>(book.size()));
      rep.factor_exposures.row(static_cast<Eigen::Index>(s)) = (V.exposures().transpose() * w).transpose();
    }
  }

  // Lifecycle census at the as-of snapshot, ascending AlphaId (order-fixed).
  const atx::u64 n_alphas = lib.n_alphas();
  for (atx::u64 a = 0; a < n_alphas; ++a) {
    const auto st = lib.state_as_of(combine::AlphaId{static_cast<atx::u32>(a)}, as_of);
    if (st) {
      const auto idx = static_cast<atx::usize>(*st);
      if (idx < rep.lifecycle_census.size()) {
        ++rep.lifecycle_census[idx];
      }
    }
  }
  return rep;
}

namespace detail {

// Shortest exact round-trip decimal of a finite f64 (locale-INDEPENDENT, the same
// std::to_chars path alpha/unparse.hpp uses). Non-finite values map to a FIXED
// token so two runs serialize byte-identically (R8). 64 bytes always suffices.
[[nodiscard]] inline std::string format_f64(atx::f64 v) {
  if (std::isnan(v)) {
    return "nan";
  }
  if (!std::isfinite(v)) {
    return v < 0.0 ? "-inf" : "inf";
  }
  char buf[64];
  const std::to_chars_result tc = std::to_chars(buf, buf + sizeof(buf), v);
  if (tc.ec != std::errc{}) {
    return "nan"; // unreachable for a finite double in 64 bytes; defensive
  }
  return std::string(buf, tc.ptr);
}

// Append one TSV row of an f64 series (one value per line) to `body`. Header line
// first. Caller writes `body` in one binary shot.
inline void emit_series(std::string &body, const char *header, const std::vector<atx::f64> &xs) {
  body += header;
  body += '\n';
  for (const atx::f64 x : xs) {
    body += format_f64(x);
    body += '\n';
  }
}

// Write `body` to dir/name in BINARY mode (no CRLF translation). Err on failure.
[[nodiscard]] inline atx::core::Status write_file(const std::filesystem::path &dir,
                                                  const char *name, const std::string &body) {
  std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
  if (!out) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          std::string{"write_report: cannot open '"} + name + "'");
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          std::string{"write_report: write failed for '"} + name + "'");
  }
  return atx::core::Ok();
}

} // namespace detail

// ===========================================================================
//  write_report — deterministic, byte-identical-across-runs per-series files.
// ===========================================================================
[[nodiscard]] inline atx::core::Status write_report(const BookReport &rep, const std::string &dir) {
  std::error_code ec;
  const std::filesystem::path root{dir};
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          "write_report: cannot create output directory");
  }

  // pnl.tsv — gross/net/cost attribution + the cumulative equity, one row/period.
  {
    std::string body = "gross\tnet\tcost\tequity\n";
    for (atx::usize s = 0; s < rep.pnl_gross.size(); ++s) {
      body += detail::format_f64(rep.pnl_gross[s]);
      body += '\t';
      body += detail::format_f64(rep.pnl_net[s]);
      body += '\t';
      body += detail::format_f64(rep.pnl_cost[s]);
      body += '\t';
      body += detail::format_f64(rep.equity_curve[s]);
      body += '\n';
    }
    ATX_TRY_VOID(detail::write_file(root, "pnl.tsv", body));
  }

  // leverage.tsv — gross / net / turnover / capacity-utilization per period.
  {
    std::string body = "gross_leverage\tnet_exposure\tturnover\tcapacity_util\n";
    for (atx::usize s = 0; s < rep.gross_leverage.size(); ++s) {
      body += detail::format_f64(rep.gross_leverage[s]);
      body += '\t';
      body += detail::format_f64(rep.net_exposure[s]);
      body += '\t';
      body += detail::format_f64(rep.turnover[s]);
      body += '\t';
      body += detail::format_f64(rep.capacity_utilization[s]);
      body += '\n';
    }
    ATX_TRY_VOID(detail::write_file(root, "leverage.tsv", body));
  }

  // exposure.tsv — the per-period factor-exposure matrix (rows=periods, K cols),
  // ascending row then column (column-fixed, locale-free).
  {
    std::string body;
    for (Eigen::Index k = 0; k < rep.factor_exposures.cols(); ++k) {
      if (k != 0) {
        body += '\t';
      }
      body += "factor_";
      char kbuf[16];
      const std::to_chars_result tc = std::to_chars(kbuf, kbuf + sizeof(kbuf), k);
      body.append(kbuf, tc.ptr);
    }
    body += '\n';
    for (Eigen::Index s = 0; s < rep.factor_exposures.rows(); ++s) {
      for (Eigen::Index k = 0; k < rep.factor_exposures.cols(); ++k) {
        if (k != 0) {
          body += '\t';
        }
        body += detail::format_f64(rep.factor_exposures(s, k));
      }
      body += '\n';
    }
    ATX_TRY_VOID(detail::write_file(root, "exposure.tsv", body));
  }

  // census.tsv — the lifecycle-state census (one count per state, fixed order).
  {
    std::string body = "state\tcount\n";
    for (atx::usize i = 0; i < rep.lifecycle_census.size(); ++i) {
      char ibuf[16];
      const std::to_chars_result tci = std::to_chars(ibuf, ibuf + sizeof(ibuf), i);
      body.append(ibuf, tci.ptr);
      body += '\t';
      char cbuf[24];
      const std::to_chars_result tcc =
          std::to_chars(cbuf, cbuf + sizeof(cbuf), rep.lifecycle_census[i]);
      body.append(cbuf, tcc.ptr);
      body += '\n';
    }
    ATX_TRY_VOID(detail::write_file(root, "census.tsv", body));
  }

  return atx::core::Ok();
}

} // namespace atx::engine::book
