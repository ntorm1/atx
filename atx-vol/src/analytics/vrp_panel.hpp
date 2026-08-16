#pragma once

// vrp_panel.hpp — round-1 versioned VRP label/feature panel builder
// (FROZEN CONTRACT `vrp_panel_v1`; 2026-08-15 vrp-ml sprint, lane vrp-panel).
//
// Header-only core behind the `bev_label_factory --vrp-panel` mode: walks one
// or MORE SurfaceDb roots (the SPY corpus is one root per year, so spot/iv
// history is STITCHED across root boundaries — trailing and forward windows
// straddle a year boundary exactly as they would in one concatenated root)
// and emits one tab-separated row per (symbol, session):
//
//   iv_fair   = sqrt(var_swap_fair_strike(PricedSurface, T).fair_strike_dec)
//               at T = 21/252 (and 63/252 for the term-slope feature only)
//   rv_fwd    = realized_vol(CloseToClose, 252) over the spot-mirror bars of
//               sessions t+1 .. t+21 (a 21-bar span => 20 close-to-close
//               return terms; the span deliberately starts at t+1, so session
//               t's own close never enters the label — see the off-by-one
//               gate test)
//   label     = (rv_fwd^2 - iv_fair^2) * (21/252)      [variance units, the
//               LONG-VOL sign convention of the vrp-portfolio digest:
//               negative on average = the short side collects the carry]
//
// Row policy (frozen):
//   * a session whose 21d strip is unavailable (surface missing / invalid
//     spot / OutOfRange / strip error / non-positive strike) is DROPPED, with
//     per-reason counters printed and echoed into the file's meta header;
//     its SPOT still participates in neighbours' trailing/forward windows;
//   * OutOfRange (or any failure) at the 63d tenor only NaNs iv_fair_63d and
//     f4_term_slope — the row is KEPT;
//   * rows within 21 sessions of the panel tail emit rv_fwd_21d = label =
//     NaN and are KEPT (predict-time rows), counted separately;
//   * features are RAW — per-asset standardization happens in-fold in the
//     trainer (digest "Normalization" + Pitfall 6), never here.
//
// Feature windows (all information available at session t's close; "trailing
// k-session c2c variance" == realized_vol over the (k+1)-bar span ending at
// and including t, i.e. k close-to-close return terms r_{t-k+1..t}):
//   f0_log_rv1     ln(max(252*r_cc(t)^2, 1e-8))
//   f1_log_rv5     ln(trailing 5-session annualized c2c variance)
//   f2_log_rv21    ln(trailing 21-session annualized c2c variance)
//   f3_iv_level    ln(iv_fair_21d^2)
//   f4_term_slope  iv_fair_63d - iv_fair_21d          (NaN when 63d missing)
//   f5_hv_iv_gap   ln(rv_trail_21d / iv_fair_21d)
//   f6_vrp_lag     iv_fair_21d^2 - trailing 21d annualized c2c variance
//   f7_ret_21d     sum of r_cc over the trailing 21 sessions
//                  == ln(spot[t]/spot[t-21])
//   f8_jump_recent 1 if max|r_cc| over the trailing 5 sessions exceeds 4x the
//                  trailing-63-session DAILY c2c sigma, else 0 — the cheap
//                  earnings-proxy mask (round 1 has NO earnings calendar;
//                  documented limitation), NaN inside the 63-session warmup
//   f9_vov_63d     sample stdev (n-1 denominator) of the daily first
//                  difference of iv_fair_21d over the trailing 63 deltas
//                  (sessions t-62..t, so iv is needed back to t-63); NaN when
//                  the window is short or any iv in it is missing
// A window with insufficient trailing history yields NaN for that feature;
// the row is still emitted. A zero-variance trailing window yields -inf
// through the ln() (degenerate fixtures only; never dropped here).
//
// Determinism: output is byte-deterministic — rows sorted (symbol, session),
// %.17g round-trip double formatting, and the meta header carries only the
// schema line, the horizon, and run COUNTERS (no paths, no timestamps), so a
// stitched multi-root run and the same sessions in one concatenated root
// produce byte-identical files (gate-tested).
//
// Placement: private Tier-B header beside realized_vol.hpp — consumed by the
// bev_label_factory example TU (and, through the example's macro-guarded
// textual inclusion, by the gate test TU). Header-only so the example's
// no-CMake-edit contract holds; every function is `inline`.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analytics/realized_vol.hpp" // OhlcBar, RvEstimator, realized_vol
#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface
#include "atx/vol/api/core/types.hpp" // Result, Status, ErrorCode, ATX_TRY
#include "atx/vol/api/pricing/derivatives.hpp" // var_swap_fair_strike, DerivQuote
#include "atx/vol/api/storage/surface_db.hpp"  // SurfaceDb, DbPartitionInfo

namespace atx::vol {

// ── Frozen contract constants (vrp_panel_v1) ─────────────────────────────

inline constexpr std::string_view kVrpPanelSchemaV1 = "vrp_panel_v1";
inline constexpr std::size_t kVrpHorizonSessions = 21;    // forward-RV window
inline constexpr double kVrpTenor21Years = 21.0 / 252.0;  // strip tenor + label scale
inline constexpr double kVrpTenor63Years = 63.0 / 252.0;  // slope tenor
inline constexpr double kVrpLogRv1Floor = 1e-8;           // f0's ln() floor
inline constexpr std::size_t kVrpJumpWindowSessions = 5;  // f8 max|r| window
inline constexpr double kVrpJumpSigmaMultiple = 4.0;      // f8 threshold
inline constexpr std::size_t kVrpSigmaWindowSessions = 63; // f8 sigma window
inline constexpr std::size_t kVrpVovWindowSessions = 63;   // f9 delta count

// Column names in EXACTLY the emitted order. Any change is a schema v2 bump.
inline constexpr std::array<std::string_view, 18> kVrpPanelColumnsV1{
    "symbol",        "date",         "entry_ts_ns", "spot",
    "iv_fair_21d",   "iv_fair_63d",  "rv_fwd_21d",  "label",
    "f0_log_rv1",    "f1_log_rv5",   "f2_log_rv21", "f3_iv_level",
    "f4_term_slope", "f5_hv_iv_gap", "f6_vrp_lag",  "f7_ret_21d",
    "f8_jump_recent", "f9_vov_63d"};
inline constexpr std::size_t kVrpPanelColumnCount = kVrpPanelColumnsV1.size();

// ── Config / counters / row / series ─────────────────────────────────────

struct VrpPanelConfig {
  std::vector<std::string> db_roots; // >= 1; stitched in session-date order
  // Symbol filter. Empty => the union of every root's manifest symbol table
  // (an error if that union is empty — pass --uid for roots whose partitions
  // carry symbols the manifest never registered).
  std::vector<std::string> symbols;
  // Optional inclusive ISO-date bounds on the SESSION axis (they bound the
  // loaded history too, so rows near the bounds carry warmup/tail NaNs; pad
  // the window when full features/labels are needed at its edges).
  std::string entry_start;
  std::string entry_end;
  std::string out; // TSV path
};

struct VrpPanelCounters {
  std::size_t n_sessions{0};          // merged session axis length
  std::size_t n_symbol_sessions{0};   // (symbol, session) pairs attempted
  std::size_t n_no_surface{0};        // symbol absent from that partition
  std::size_t n_bad_spot{0};          // non-finite/non-positive spot
  std::size_t n_var21_out_of_range{0}; // 21d tenor outside fitted pillars
  std::size_t n_var21_error{0};        // any other 21d strip failure
  std::size_t n_var21_nonfinite{0};    // strip Ok but K_var not finite/positive
  std::size_t n_63d_unavailable{0};    // 63d strip missing (row kept, f4 NaN)
  std::size_t n_rows_tail_nan_label{0}; // kept rows with NaN forward window
  std::size_t n_rows_written{0};
};

// One symbol's stitched per-session history (the "bar axis"): parallel
// arrays, one entry per session where the symbol HAD a surface with a valid
// spot. iv21 is NaN where the 21d strip was unavailable (row dropped, bar
// kept); iv63 is NaN where only the 63d strip was unavailable (row kept).
struct VrpSeries {
  std::vector<std::string> dates;  // partition keys, ascending
  std::vector<std::int64_t> ts_ns; // strictly ascending session timestamps
  std::vector<double> spot;        // finite, > 0
  std::vector<double> iv21;
  std::vector<double> iv63;
};

// One emitted panel row (symbol lives beside the row batch, not in it).
struct VrpPanelRow {
  std::string date;
  std::int64_t entry_ts_ns{0};
  double spot{0.0};
  double iv_fair_21d{0.0};
  double iv_fair_63d{0.0};
  double rv_fwd_21d{0.0};
  double label{0.0};
  double f0_log_rv1{0.0};
  double f1_log_rv5{0.0};
  double f2_log_rv21{0.0};
  double f3_iv_level{0.0};
  double f4_term_slope{0.0};
  double f5_hv_iv_gap{0.0};
  double f6_vrp_lag{0.0};
  double f7_ret_21d{0.0};
  double f8_jump_recent{0.0};
  double f9_vov_63d{0.0};
};

namespace vrp_detail {

[[nodiscard]] inline double nan_d() noexcept {
  return std::numeric_limits<double>::quiet_NaN();
}

// %.17g — max_digits10, the minimum that round-trips any double bit-exactly
// (same convention as bev_label_factory's writer; snprintf is locale-stable
// under "C" numeric formatting). NaN is canonicalized to the single spelling
// "nan" FIRST: NaN-propagating arithmetic (e.g. qNaN * qNaN in the warmup
// features) can carry a set sign bit, which the Windows UCRT prints as
// "-nan(ind)" — an alternate spelling a frozen byte-deterministic contract
// cannot admit (and one Python's float() refuses to parse). A NaN's sign is
// meaningless, so no information is lost.
inline void append_double(std::string &out, double v) {
  if (std::isnan(v)) {
    out += "nan";
    return;
  }
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

inline void append_i64(std::string &out, std::int64_t v) {
  char buf[32];
  const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Tab/newline would corrupt the TSV framing; mirror bev's sanitizer.
inline void append_sanitized(std::string &out, std::string_view s) {
  for (const char c : s) {
    out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
}

inline void append_meta_count(std::string &out, std::string_view key, std::size_t value) {
  out += "# ";
  out += key;
  out += '=';
  out += std::to_string(value);
  out += '\n';
}

// Annualized c2c vol over the (k+1)-bar span ending at index `i` — exactly
// `k` close-to-close return terms r_{i-k+1..i}. NaN when the trailing
// history is short. Defensive NaN (never an error) if realized_vol rejects
// the slice — impossible for the validated spot-mirror bars built below.
[[nodiscard]] inline double trailing_c2c_vol(std::span<const OhlcBar> bars, std::size_t i,
                                             std::size_t k) {
  if (k == 0 || i < k) {
    return nan_d();
  }
  const Result<double> v =
      realized_vol(bars.subspan(i - k, k + 1), RvEstimator::CloseToClose, 252.0);
  return v.has_value() ? *v : nan_d();
}

// f9: sample stdev (n-1) of the 63 daily iv21 first differences ending at
// `i` (deltas at sessions i-62..i, so iv21 is read back to i-63). NaN when
// the window is short or any iv21 inside it is missing — a dropped session
// mid-window deliberately NaNs the whole window rather than silently
// bridging the gap.
[[nodiscard]] inline double vov_63d(std::span<const double> iv21, std::size_t i) {
  const std::size_t w = kVrpVovWindowSessions;
  if (i < w) {
    return nan_d();
  }
  double mean = 0.0;
  // Bounded: exactly w iterations.
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double d = iv21[j] - iv21[j - 1];
    if (!std::isfinite(d)) {
      return nan_d();
    }
    mean += d;
  }
  mean /= static_cast<double>(w);
  double acc = 0.0;
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double d = iv21[j] - iv21[j - 1];
    acc += (d - mean) * (d - mean);
  }
  return std::sqrt(acc / static_cast<double>(w - 1));
}

// f8's max|r_cc| over the trailing kVrpJumpWindowSessions sessions ending at
// `i` (returns at bars i-4..i, so closes back to i-5). NaN inside the warmup.
[[nodiscard]] inline double max_abs_r_5d(std::span<const double> spot, std::size_t i) {
  const std::size_t w = kVrpJumpWindowSessions;
  if (i < w) {
    return nan_d();
  }
  double mx = 0.0;
  // Bounded: exactly w iterations.
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    mx = std::max(mx, std::fabs(std::log(spot[j] / spot[j - 1])));
  }
  return mx;
}

} // namespace vrp_detail

// ── Row building (pure; the gate tests drive this without a SurfaceDb) ───

// Series -> panel rows. Bars with NaN iv21 are the already-counted dropped
// sessions: skipped here, but their spots still feed every window (they are
// in `s`). Counts n_rows_written / n_rows_tail_nan_label into `counters`.
// Errors (InvalidArgument) only on a malformed series: mismatched parallel
// arrays, non-ascending ts, or an invalid spot — the loader guarantees all
// three, so an error here means a caller bug, not data quality.
[[nodiscard]] inline Result<std::vector<VrpPanelRow>> build_vrp_rows(const VrpSeries &s,
                                                                     VrpPanelCounters &counters) {
  const std::size_t n = s.spot.size();
  if (s.dates.size() != n || s.ts_ns.size() != n || s.iv21.size() != n || s.iv63.size() != n) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "build_vrp_rows: parallel series arrays disagree in size");
  }
  std::vector<OhlcBar> bars;
  bars.reserve(n);
  // Bounded by n. Validates the series while mirroring spots into bars.
  for (std::size_t i = 0; i < n; ++i) {
    if (!(std::isfinite(s.spot[i]) && s.spot[i] > 0.0)) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "build_vrp_rows: non-finite/non-positive spot at index " +
                                std::to_string(i));
    }
    if (i > 0 && s.ts_ns[i] <= s.ts_ns[i - 1]) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "build_vrp_rows: session ts not strictly ascending at index " +
                                std::to_string(i));
    }
    bars.push_back(OhlcBar{
        .ts_ns = s.ts_ns[i], .open = s.spot[i], .high = s.spot[i], .low = s.spot[i],
        .close = s.spot[i]});
  }

  std::vector<VrpPanelRow> rows;
  rows.reserve(n);
  // Bounded by n — one pass over the bar axis.
  for (std::size_t i = 0; i < n; ++i) {
    const double iv21 = s.iv21[i];
    if (!std::isfinite(iv21)) {
      continue; // dropped session (reason counted at load time); bar kept above
    }
    VrpPanelRow row;
    row.date = s.dates[i];
    row.entry_ts_ns = s.ts_ns[i];
    row.spot = s.spot[i];
    row.iv_fair_21d = iv21;
    row.iv_fair_63d = s.iv63[i];

    // Forward leg: bars t+1..t+21 only — session t's close never enters
    // (the off-by-one gate test plants spikes at t and t+22 and requires
    // the label at t to hold still under both).
    double rv_fwd = vrp_detail::nan_d();
    if (i + kVrpHorizonSessions < n) {
      const Result<double> v =
          realized_vol(std::span<const OhlcBar>{bars.data() + i + 1, kVrpHorizonSessions},
                       RvEstimator::CloseToClose, 252.0);
      if (v.has_value()) {
        rv_fwd = *v;
      }
    } else {
      ++counters.n_rows_tail_nan_label; // predict-time row: kept, label NaN
    }
    row.rv_fwd_21d = rv_fwd;
    row.label = std::isfinite(rv_fwd)
                    ? (rv_fwd * rv_fwd - iv21 * iv21) * kVrpTenor21Years
                    : vrp_detail::nan_d();

    const double r1 = (i >= 1) ? std::log(s.spot[i] / s.spot[i - 1]) : vrp_detail::nan_d();
    row.f0_log_rv1 = std::isfinite(r1)
                         ? std::log(std::max(252.0 * r1 * r1, kVrpLogRv1Floor))
                         : vrp_detail::nan_d();
    const double vol5 = vrp_detail::trailing_c2c_vol(bars, i, 5);
    row.f1_log_rv5 = std::log(vol5 * vol5); // NaN propagates through log
    const double vol21 = vrp_detail::trailing_c2c_vol(bars, i, 21);
    row.f2_log_rv21 = std::log(vol21 * vol21);
    row.f3_iv_level = std::log(iv21 * iv21);
    row.f4_term_slope = s.iv63[i] - iv21; // NaN when the 63d strip was missing
    row.f5_hv_iv_gap = std::log(vol21 / iv21);
    row.f6_vrp_lag = iv21 * iv21 - vol21 * vol21;
    row.f7_ret_21d = (i >= 21) ? std::log(s.spot[i] / s.spot[i - 21]) : vrp_detail::nan_d();
    const double mx5 = vrp_detail::max_abs_r_5d(s.spot, i);
    const double vol63 = vrp_detail::trailing_c2c_vol(bars, i, kVrpSigmaWindowSessions);
    const double sigma_daily = vol63 / std::sqrt(252.0);
    row.f8_jump_recent = (std::isnan(mx5) || std::isnan(sigma_daily))
                             ? vrp_detail::nan_d()
                             : ((mx5 > kVrpJumpSigmaMultiple * sigma_daily) ? 1.0 : 0.0);
    row.f9_vov_63d = vrp_detail::vov_63d(s.iv21, i);

    rows.push_back(std::move(row));
    ++counters.n_rows_written;
  }
  return atx::core::Ok(std::move(rows));
}

// ── Multi-root session merge ──────────────────────────────────────────────

struct VrpSessionRef {
  std::string date;    // partition key (ISO date)
  std::size_t db_idx{0}; // which root serves it
};

// Union of every root's partition keys, optionally bounded to the inclusive
// ISO window [lo, hi] (empty = unbounded), sorted ascending. ISO dates sort
// lexicographically == chronologically. A date served by MORE than one root
// is an error — the stitch would be ambiguous (which root's surface wins?),
// and the production yearly roots are disjoint by construction.
[[nodiscard]] inline Result<std::vector<VrpSessionRef>>
merge_vrp_sessions(std::span<const SurfaceDb> dbs, std::string_view lo, std::string_view hi) {
  std::vector<VrpSessionRef> out;
  // Bounded by total partition count across roots.
  for (std::size_t d = 0; d < dbs.size(); ++d) {
    for (const DbPartitionInfo &p : dbs[d].partitions()) {
      if (!lo.empty() && std::string_view{p.key} < lo) {
        continue;
      }
      if (!hi.empty() && std::string_view{p.key} > hi) {
        continue;
      }
      out.push_back(VrpSessionRef{p.key, d});
    }
  }
  std::sort(out.begin(), out.end(), [](const VrpSessionRef &a, const VrpSessionRef &b) {
    return a.date != b.date ? a.date < b.date : a.db_idx < b.db_idx;
  });
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i].date == out[i - 1].date) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "merge_vrp_sessions: session date '" + out[i].date +
                                "' appears in more than one --db root");
    }
  }
  return atx::core::Ok(std::move(out));
}

// ── Series loading (SurfaceDb -> VrpSeries per symbol) ────────────────────

// Date-major walk (one partition mapping serves every symbol on that date;
// the S5 LRU cache then never thrashes). Per (symbol, session): reconstruct
// the owned PricedSurface — the surface-native var_swap_fair_strike overload
// needs the fitted pillars only PricedSurface carries — read its spot and
// price the 21d/63d strips. NotFound = the symbol is simply absent that
// session (counted, skipped); any OTHER load error is a corrupt corpus and
// fails the run loudly.
[[nodiscard]] inline Result<std::vector<VrpSeries>>
load_vrp_series(std::span<const SurfaceDb> dbs, std::span<const VrpSessionRef> sessions,
                std::span<const std::string> symbols, VrpPanelCounters &counters) {
  std::vector<VrpSeries> series(symbols.size());
  // Bounded by sessions.size() * symbols.size().
  for (const VrpSessionRef &sr : sessions) {
    const SurfaceDb &db = dbs[sr.db_idx];
    for (std::size_t k = 0; k < symbols.size(); ++k) {
      ++counters.n_symbol_sessions;
      const Result<PricedSurface> surf = db.load_surface(sr.date, symbols[k]);
      if (!surf.has_value()) {
        if (surf.error().code() == ErrorCode::NotFound) {
          ++counters.n_no_surface;
          continue;
        }
        return atx::core::Err(surf.error().code(),
                              "load_vrp_series: load_surface('" + sr.date + "', '" + symbols[k] +
                                  "'): " + surf.error().to_string());
      }
      const double S = surf->pricing().S;
      if (!(std::isfinite(S) && S > 0.0)) {
        ++counters.n_bad_spot;
        continue;
      }
      const std::int64_t ts = surf->pricing().now_ts_ns;
      VrpSeries &s = series[k];
      if (!s.ts_ns.empty() && ts <= s.ts_ns.back()) {
        return atx::core::Err(ErrorCode::InvalidArgument,
                              "load_vrp_series: session ts not ascending across stitched roots "
                              "for symbol '" + symbols[k] + "' at date " + sr.date);
      }
      double iv21 = vrp_detail::nan_d();
      double iv63 = vrp_detail::nan_d();
      {
        const Result<DerivQuote> q = var_swap_fair_strike(*surf, kVrpTenor21Years);
        if (q.has_value()) {
          const double k_var = q->fair_strike_dec;
          if (std::isfinite(k_var) && k_var > 0.0) {
            iv21 = std::sqrt(k_var);
          } else {
            ++counters.n_var21_nonfinite;
          }
        } else if (q.error().code() == ErrorCode::OutOfRange) {
          ++counters.n_var21_out_of_range;
        } else {
          ++counters.n_var21_error;
        }
      }
      {
        const Result<DerivQuote> q = var_swap_fair_strike(*surf, kVrpTenor63Years);
        if (q.has_value() && std::isfinite(q->fair_strike_dec) && q->fair_strike_dec > 0.0) {
          iv63 = std::sqrt(q->fair_strike_dec);
        } else {
          ++counters.n_63d_unavailable; // row kept; f4 NaN
        }
      }
      s.dates.push_back(sr.date);
      s.ts_ns.push_back(ts);
      s.spot.push_back(S);
      s.iv21.push_back(iv21);
      s.iv63.push_back(iv63);
    }
  }
  return atx::core::Ok(std::move(series));
}

// ── TSV writer ────────────────────────────────────────────────────────────

// Meta header (schema + horizon first — the two frozen comment lines — then
// deterministic run counters ONLY: no paths, no timestamps, no root list, so
// a stitched run and its concatenated-root twin are byte-identical), the
// column header, then rows sorted (symbol, session).
[[nodiscard]] inline Status
write_vrp_panel_tsv(std::string_view path, std::span<const std::string> symbols,
                    std::span<const std::vector<VrpPanelRow>> rows_per_symbol,
                    const VrpPanelCounters &c) {
  std::string out;
  out += "# schema=";
  out += kVrpPanelSchemaV1;
  out += '\n';
  vrp_detail::append_meta_count(out, "horizon_days", kVrpHorizonSessions);
  vrp_detail::append_meta_count(out, "n_symbols", symbols.size());
  vrp_detail::append_meta_count(out, "n_sessions", c.n_sessions);
  vrp_detail::append_meta_count(out, "n_symbol_sessions", c.n_symbol_sessions);
  vrp_detail::append_meta_count(out, "n_no_surface", c.n_no_surface);
  vrp_detail::append_meta_count(out, "n_bad_spot", c.n_bad_spot);
  vrp_detail::append_meta_count(out, "n_var21_out_of_range", c.n_var21_out_of_range);
  vrp_detail::append_meta_count(out, "n_var21_error", c.n_var21_error);
  vrp_detail::append_meta_count(out, "n_var21_nonfinite", c.n_var21_nonfinite);
  vrp_detail::append_meta_count(out, "n_63d_unavailable", c.n_63d_unavailable);
  vrp_detail::append_meta_count(out, "n_rows_tail_nan_label", c.n_rows_tail_nan_label);
  vrp_detail::append_meta_count(out, "n_rows", c.n_rows_written);

  for (std::size_t i = 0; i < kVrpPanelColumnCount; ++i) {
    if (i > 0) {
      out += '\t';
    }
    out += kVrpPanelColumnsV1[i];
  }
  out += '\n';

  // Bounded by total row count.
  for (std::size_t k = 0; k < rows_per_symbol.size(); ++k) {
    for (const VrpPanelRow &r : rows_per_symbol[k]) {
      vrp_detail::append_sanitized(out, symbols[k]);
      out += '\t';
      vrp_detail::append_sanitized(out, r.date);
      out += '\t';
      vrp_detail::append_i64(out, r.entry_ts_ns);
      const double doubles[] = {r.spot,          r.iv_fair_21d,  r.iv_fair_63d,
                                r.rv_fwd_21d,    r.label,        r.f0_log_rv1,
                                r.f1_log_rv5,    r.f2_log_rv21,  r.f3_iv_level,
                                r.f4_term_slope, r.f5_hv_iv_gap, r.f6_vrp_lag,
                                r.f7_ret_21d,    r.f8_jump_recent, r.f9_vov_63d};
      for (const double v : doubles) {
        out += '\t';
        vrp_detail::append_double(out, v);
      }
      out += '\n';
    }
  }

  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return atx::core::Err(ErrorCode::IoError,
                          "write_vrp_panel_tsv: cannot open '" + std::string(path) + "'");
  }
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os) {
    return atx::core::Err(ErrorCode::IoError, "write_vrp_panel_tsv: write failed");
  }
  return atx::core::Ok();
}

// ── Top-level runner ──────────────────────────────────────────────────────

// Open every root, merge sessions, load per-symbol stitched series, build
// rows, write the TSV, print the per-reason drop accounting. Errors:
// InvalidArgument (bad config, duplicate session date across roots),
// NotFound (no sessions / no symbols / zero rows), or any loud corpus
// failure from the loader.
[[nodiscard]] inline Result<VrpPanelCounters> run_vrp_panel(const VrpPanelConfig &cfg) {
  if (cfg.db_roots.empty() || cfg.out.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "run_vrp_panel: at least one --db root and --out are required");
  }
  if (!cfg.entry_start.empty() && !cfg.entry_end.empty() && cfg.entry_end < cfg.entry_start) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "run_vrp_panel: --entry-end precedes --entry-start");
  }
  std::vector<SurfaceDb> dbs;
  dbs.reserve(cfg.db_roots.size());
  // Bounded by the root count.
  for (const std::string &root : cfg.db_roots) {
    Result<SurfaceDb> db = SurfaceDb::open(root);
    if (!db.has_value()) {
      return atx::core::Err(db.error().code(),
                            "run_vrp_panel: cannot open --db '" + root +
                                "': " + db.error().to_string());
    }
    dbs.push_back(std::move(*db));
  }

  VrpPanelCounters counters;
  ATX_TRY(const std::vector<VrpSessionRef> sessions,
          merge_vrp_sessions(dbs, cfg.entry_start, cfg.entry_end));
  counters.n_sessions = sessions.size();
  if (sessions.empty()) {
    return atx::core::Err(ErrorCode::NotFound,
                          "run_vrp_panel: no sessions in the requested window");
  }

  std::vector<std::string> symbols = cfg.symbols;
  if (symbols.empty()) {
    for (const SurfaceDb &db : dbs) {
      const std::vector<std::string> names = db.symbols();
      symbols.insert(symbols.end(), names.begin(), names.end());
    }
  }
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
  if (symbols.empty()) {
    return atx::core::Err(ErrorCode::NotFound,
                          "run_vrp_panel: no symbols in any root's manifest; pass --uid");
  }

  ATX_TRY(std::vector<VrpSeries> series, load_vrp_series(dbs, sessions, symbols, counters));

  std::vector<std::vector<VrpPanelRow>> rows_per_symbol(symbols.size());
  // Bounded by symbol count.
  for (std::size_t k = 0; k < symbols.size(); ++k) {
    Result<std::vector<VrpPanelRow>> rows = build_vrp_rows(series[k], counters);
    if (!rows.has_value()) {
      return atx::core::Err(rows.error().code(), "run_vrp_panel: symbol '" + symbols[k] +
                                                     "': " + rows.error().to_string());
    }
    rows_per_symbol[k] = std::move(*rows);
  }
  if (counters.n_rows_written == 0) {
    return atx::core::Err(
        ErrorCode::NotFound,
        "run_vrp_panel: produced zero rows (sessions=" + std::to_string(counters.n_sessions) +
            " symbol_sessions=" + std::to_string(counters.n_symbol_sessions) +
            " no_surface=" + std::to_string(counters.n_no_surface) +
            " bad_spot=" + std::to_string(counters.n_bad_spot) +
            " var21_oor=" + std::to_string(counters.n_var21_out_of_range) +
            " var21_err=" + std::to_string(counters.n_var21_error) +
            " var21_nonfinite=" + std::to_string(counters.n_var21_nonfinite) + ")");
  }

  ATX_TRY_VOID(write_vrp_panel_tsv(cfg.out, symbols, rows_per_symbol, counters));

  // The brief's "per-reason counts printed" — one deterministic line.
  std::printf("[vrp_panel] sessions=%zu symbol_sessions=%zu no_surface=%zu bad_spot=%zu "
              "var21_oor=%zu var21_err=%zu var21_nonfinite=%zu slope63_unavailable=%zu "
              "tail_nan_label=%zu rows=%zu -> %s\n",
              counters.n_sessions, counters.n_symbol_sessions, counters.n_no_surface,
              counters.n_bad_spot, counters.n_var21_out_of_range, counters.n_var21_error,
              counters.n_var21_nonfinite, counters.n_63d_unavailable,
              counters.n_rows_tail_nan_label, counters.n_rows_written, cfg.out.c_str());
  return atx::core::Ok(counters);
}

} // namespace atx::vol
