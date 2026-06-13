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
//  FRONT-DOOR GUARDS (returns Err BEFORE the loop, never UB in release): the
//  three MultiPeriodResult vectors (books / cost_bps / turnover) must each have
//  length == sched.periods.size() (InvalidArgument otherwise); every schedule date
//  sched.periods[s] must be < panel.dates() (OutOfRange otherwise); and every
//  book books.books[s] must have length == V.n_instruments() so both the pnl
//  reduction and the Xᵀw exposure are in-bounds (InvalidArgument otherwise). No
//  silent partial row — a mismatch is a caller (s7-5 pipeline) contract violation.
//
//  For schedule period s (book == books.books[s], realized at panel date
//  sched.periods[s]), reductions run in canonical ascending instrument order i:
//    * pnl_gross[s] = Σ_i book[i]·r[i], where r is the realized return cross-
//        section panel.field_cross_section(returns_field, sched.periods[s]). A NaN
//        r[i] — OR a name out of the point-in-time universe at that date — is
//        treated as 0 (no-survivorship: a missing/delisted name's holding earns
//        nothing; it never poisons the reduction with a NaN).
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
//  is fine. accumulate_report returns Err on a dimension/range violation (front-
//  door guards, never release UB); write_report returns Err(IoError) on a
//  filesystem failure. Both follow the Result/Status convention.

#include <array>      // std::array (lifecycle census)
#include <charconv>   // std::to_chars (locale-free shortest round-trip)
#include <cmath>      // std::isnan, std::isfinite, std::fabs
#include <filesystem> // create dir, path join
#include <fstream>    // std::ofstream (binary write)
#include <span>        // std::span (realized return cross-section, emit_tsv headers)
#include <string>      // file buffer
#include <string_view> // std::string_view (emit_tsv column headers)
#include <system_error> // std::errc (to_chars_result::ec)
#include <tuple>       // std::tuple_size_v (census-size static_assert)
#include <utility>     // std::move (Ok(std::move(rep)))
#include <vector>      // report series

#include <Eigen/Core> // Eigen::Index, MatX ops (Xᵀw)

#include "atx/core/error.hpp"         // Status, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/alpha/panel.hpp"       // alpha::Panel, FieldId
#include "atx/engine/combine/store.hpp"     // combine::AlphaId
#include "atx/engine/library/library.hpp"   // library::Library
#include "atx/engine/library/lifecycle.hpp" // library::LifecycleState (census index bound)
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

// The census array is indexed by LifecycleState's underlying value (0..5), so its
// size MUST equal the enumerator count — pinned here so a new state forces a bump
// (lets accumulate_report drop the runtime idx-in-range guard: the cast is total).
static_assert(static_cast<atx::usize>(library::LifecycleState::Recycled) + 1U ==
                  std::tuple_size_v<decltype(BookReport::lifecycle_census)>,
              "lifecycle_census size must match the LifecycleState enumerator count");

namespace detail {

// One period's accumulated scalars (the factor-exposure row is written directly
// into the report matrix by the caller — it already owns the M×K → K mapping).
struct PeriodAccum {
  atx::f64 pnl_gross;
  atx::f64 pnl_cost;
  atx::f64 gross;
  atx::f64 net;
};

// Accumulate one period's pnl/leverage scalars in canonical ascending instrument
// order. NaN return OR out-of-universe → 0 (no-survivorship, R4). PRECONDITION
// (front-door-guarded by the caller): book.size() == r.size() == V.n_instruments()
// and `date` < panel.dates(), so every read is in-bounds.
[[nodiscard]] inline PeriodAccum accumulate_period(const std::vector<atx::f64> &book,
                                                   std::span<const atx::f64> r,
                                                   const alpha::Panel &panel, atx::usize date,
                                                   atx::f64 cost_bps) noexcept {
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
  return PeriodAccum{pnl_gross, cost_bps * 1e-4 /*bps → fractional return*/, gross, net};
}

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

// Shortest decimal of a non-negative integer (locale-free) — index / count cells.
template <class Int> [[nodiscard]] inline std::string format_int(Int v) {
  char buf[24];
  const std::to_chars_result tc = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, tc.ptr); // a 24-byte buffer always fits a 64-bit integer
}

// A reusable TSV builder: a fixed header line, then one row per period built by
// `row_cell(period, column)`. Tabs between columns, '\n' line endings, ascending
// row/column order — the deterministic locale-free serialization for every file.
template <class Cell>
[[nodiscard]] inline std::string emit_tsv(std::span<const std::string_view> headers,
                                          atx::usize n_rows, const Cell &row_cell) {
  std::string body;
  for (atx::usize c = 0; c < headers.size(); ++c) {
    if (c != 0) {
      body += '\t';
    }
    body += headers[c];
  }
  body += '\n';
  for (atx::usize s = 0; s < n_rows; ++s) {
    for (atx::usize c = 0; c < headers.size(); ++c) {
      if (c != 0) {
        body += '\t';
      }
      body += row_cell(s, c);
    }
    body += '\n';
  }
  return body;
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
//  accumulate_report — roll the realized book chain into a BookReport.
//
//  FRONT-DOOR GUARDED (no release UB): the three MultiPeriodResult vectors must
//  each match the schedule length; every schedule date must be < panel.dates();
//  every book must match V.n_instruments(). Any violation returns Err.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<BookReport>
accumulate_report(const risk::MultiPeriodResult &books, const alpha::Panel &panel,
                  alpha::FieldId returns_field, const risk::RebalanceSchedule &sched,
                  const risk::FactorModel &V, atx::f64 capacity_gross, const library::Library &lib,
                  atx::usize as_of) {
  const atx::usize n = sched.periods.size();
  const atx::usize m = V.n_instruments();
  if (books.books.size() != n || books.cost_bps.size() != n || books.turnover.size() != n) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "accumulate_report: MultiPeriodResult vectors must match schedule length");
  }
  for (atx::usize s = 0; s < n; ++s) {
    if (sched.periods[s] >= panel.dates()) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                            "accumulate_report: schedule date is out of the panel's date range");
    }
    if (books.books[s].size() != m) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "accumulate_report: book length must equal V.n_instruments()");
    }
  }

  BookReport rep;
  rep.equity_curve.reserve(n);
  rep.gross_leverage.reserve(n);
  rep.net_exposure.reserve(n);
  rep.turnover.reserve(n);
  rep.pnl_gross.reserve(n);
  rep.pnl_net.reserve(n);
  rep.pnl_cost.reserve(n);
  rep.capacity_utilization.reserve(n);
  rep.factor_exposures = atx::core::linalg::MatX::Zero(static_cast<Eigen::Index>(n),
                                                       static_cast<Eigen::Index>(V.n_factors()));

  atx::f64 equity = 0.0; // running cumulative net pnl
  for (atx::usize s = 0; s < n; ++s) {
    const std::vector<atx::f64> &book = books.books[s];
    const atx::usize date = sched.periods[s];
    const detail::PeriodAccum acc = detail::accumulate_period(
        book, panel.field_cross_section(returns_field, date), panel, date, books.cost_bps[s]);
    const atx::f64 pnl_net = acc.pnl_gross - acc.pnl_cost;
    equity += pnl_net;

    rep.pnl_gross.push_back(acc.pnl_gross);
    rep.pnl_cost.push_back(acc.pnl_cost);
    rep.pnl_net.push_back(pnl_net);
    rep.equity_curve.push_back(equity);
    rep.gross_leverage.push_back(acc.gross);
    rep.net_exposure.push_back(acc.net);
    rep.turnover.push_back(books.turnover[s]);
    rep.capacity_utilization.push_back(capacity_gross > 0.0 ? acc.gross / capacity_gross : 0.0);

    // Per-period book factor exposure Xᵀw → factor_exposures.row(s) (length K).
    // book.size() == m is front-door-guarded, so the Map is in-bounds.
    const Eigen::Map<const atx::core::linalg::VecX> w(book.data(),
                                                      static_cast<Eigen::Index>(book.size()));
    rep.factor_exposures.row(static_cast<Eigen::Index>(s)) =
        (V.exposures().transpose() * w).transpose();
  }

  // Lifecycle census at the as-of snapshot, ascending AlphaId (order-fixed). The
  // index is total: state_as_of returns a LifecycleState whose value is < 6 (the
  // census-size static_assert pins that), so no runtime range guard is needed.
  const atx::u64 n_alphas = lib.n_alphas();
  for (atx::u64 a = 0; a < n_alphas; ++a) {
    const auto st = lib.state_as_of(combine::AlphaId{static_cast<atx::u32>(a)}, as_of);
    if (st) {
      ++rep.lifecycle_census[static_cast<atx::usize>(*st)];
    }
  }
  return atx::core::Ok(std::move(rep));
}

// ===========================================================================
//  write_report — deterministic, byte-identical-across-runs per-series files.
// ===========================================================================

namespace detail {

// pnl.tsv — gross / net / cost attribution + cumulative equity, one row/period.
[[nodiscard]] inline std::string serialize_pnl(const BookReport &rep) {
  static constexpr std::string_view kHdr[] = {"gross", "net", "cost", "equity"};
  return emit_tsv(std::span<const std::string_view>{kHdr}, rep.pnl_gross.size(),
                  [&](atx::usize s, atx::usize c) -> std::string {
                    switch (c) {
                    case 0:
                      return format_f64(rep.pnl_gross[s]);
                    case 1:
                      return format_f64(rep.pnl_net[s]);
                    case 2:
                      return format_f64(rep.pnl_cost[s]);
                    default:
                      return format_f64(rep.equity_curve[s]);
                    }
                  });
}

// leverage.tsv — gross / net / turnover / capacity-utilization per period.
[[nodiscard]] inline std::string serialize_leverage(const BookReport &rep) {
  static constexpr std::string_view kHdr[] = {"gross_leverage", "net_exposure", "turnover",
                                              "capacity_util"};
  return emit_tsv(std::span<const std::string_view>{kHdr}, rep.gross_leverage.size(),
                  [&](atx::usize s, atx::usize c) -> std::string {
                    switch (c) {
                    case 0:
                      return format_f64(rep.gross_leverage[s]);
                    case 1:
                      return format_f64(rep.net_exposure[s]);
                    case 2:
                      return format_f64(rep.turnover[s]);
                    default:
                      return format_f64(rep.capacity_utilization[s]);
                    }
                  });
}

// exposure.tsv — the per-period factor-exposure matrix (rows=periods, K cols),
// ascending row then column. Headers are factor_0..factor_{K-1}.
[[nodiscard]] inline std::string serialize_exposure(const BookReport &rep) {
  const auto k_cols = static_cast<atx::usize>(rep.factor_exposures.cols());
  std::vector<std::string> names(k_cols);
  std::vector<std::string_view> headers(k_cols);
  for (atx::usize k = 0; k < k_cols; ++k) {
    names[k] = "factor_" + format_int(k);
    headers[k] = names[k];
  }
  return emit_tsv(std::span<const std::string_view>{headers},
                  static_cast<atx::usize>(rep.factor_exposures.rows()),
                  [&](atx::usize s, atx::usize c) -> std::string {
                    return format_f64(rep.factor_exposures(static_cast<Eigen::Index>(s),
                                                           static_cast<Eigen::Index>(c)));
                  });
}

// census.tsv — the lifecycle-state census (one count per state, fixed order).
[[nodiscard]] inline std::string serialize_census(const BookReport &rep) {
  static constexpr std::string_view kHdr[] = {"state", "count"};
  return emit_tsv(std::span<const std::string_view>{kHdr}, rep.lifecycle_census.size(),
                  [&](atx::usize s, atx::usize c) -> std::string {
                    return c == 0 ? format_int(s) : format_int(rep.lifecycle_census[s]);
                  });
}

} // namespace detail

[[nodiscard]] inline atx::core::Status write_report(const BookReport &rep, const std::string &dir) {
  std::error_code ec;
  const std::filesystem::path root{dir};
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          "write_report: cannot create output directory");
  }
  ATX_TRY_VOID(detail::write_file(root, "pnl.tsv", detail::serialize_pnl(rep)));
  ATX_TRY_VOID(detail::write_file(root, "leverage.tsv", detail::serialize_leverage(rep)));
  ATX_TRY_VOID(detail::write_file(root, "exposure.tsv", detail::serialize_exposure(rep)));
  ATX_TRY_VOID(detail::write_file(root, "census.tsv", detail::serialize_census(rep)));
  return atx::core::Ok();
}

} // namespace atx::engine::book
