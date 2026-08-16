#pragma once

// vrp_train.hpp — walk-forward VRP model trainer (vrp-ml sprint, lane
// vrp-model). Library-shaped logic behind the atx-vol-vrp-train CLI
// (tools/vrp_train_main.cpp is the thin argv shell); gate-tested by the
// VrpTrain* suites in tests/vrp_model_test.cpp.
//
// Pipeline (run_vrp_train):
//   1. Parse a FROZEN vrp_panel_v1 TSV (18 tab-separated columns: symbol,
//      date, entry_ts_ns, spot, iv_fair_21d, iv_fair_63d, rv_fwd_21d, label,
//      f0_log_rv1 .. f9_vov_63d; comment lines `# schema=vrp_panel_v1` and
//      `# horizon_days=21`; label = (rv_fwd_21d^2 - iv_fair_21d^2) * 21/252,
//      TOTAL variance units over [t, t+21]). Fail closed on any schema/
//      header/count mismatch. Tail rows carry NaN label + NaN rv_fwd_21d and
//      are KEPT (predict-time rows); NaN features (f4_term_slope from an
//      OutOfRange iv_fair_63d) survive parsing as NaN.
//   2. Pool across symbols; map labeled rows onto ResearchObservations
//      (label interval [t, t+21 sessions]; label_end is the POOLED session
//      axis timestamp 21 DISTINCT sessions later -- the TRUE horizon, so a
//      sparse symbol's emitted-row gaps never inflate its label span or,
//      through the global-max embargo, purge the whole corpus -- round-2
//      review major 2) and build a purged + embargoed anchored walk-forward
//      via make_purged_walk_forward_plan (research_validation.hpp).
//      embargo_ns is derived from the panel as the MAXIMUM observed
//      21-session wall-clock span, so it always covers >= 21 sessions. The
//      t+21 same-symbol-row invariant is enforced PER ROW (round-2 F1): a
//      labeled row whose symbol lacks an emitted row 21 positions later
//      (interior surface holes inside the final horizon) is REJECTED AND
//      COUNTED, never fatal for the symbol or the run; an UNLABELED row
//      that does have a t+21 successor is a panel-contract violation (a
//      mid-sample NaN label would be scored with hindsight models) and
//      fails closed. When cfg.walk_auto is set the fold plan is derived
//      from the labeled decision-group depth (derive_vrp_walk_forward
//      below), so thin histories still train instead of dying on the
//      production 252/63/63 defaults.
//   3. Per fold, with per-asset standardization fit on TRAIN-fold rows ONLY
//      (digest Pitfall 6: full-sample z-scores leak the future vol level;
//      NaN features impute to z = 0, the per-asset train mean):
//        (a) baseline log-HAR elastic-net via atx-engine fit_linear on
//            {f0, f1, f2} against ln(rv_fwd^2), with the exp(s^2/2)
//            lognormal retransformation and the Clements-Preve "insanity
//            filter" clip to the train-window label range (digest [20]);
//        (b) pooled GBT via atx-engine fit_gbt (fixed master_seed,
//            conservative GbtCfg defaults) on all 10 features against the
//            panel label directly.
//      QLIKE is scored in VARIANCE LEVELS (never log-vol) against the
//      close-to-close proxy rv_fwd_21d^2, plus per-date Spearman rank IC of
//      predicted vs realized label. The GBT's label-space prediction implies
//      a variance forecast that can go <= 0 on thin folds (round-1 SP100
//      experiment: QLIKE 1e6..3e7); its QLIKE forecast passes through the
//      SAME Clements-Preve insanity clip as the baseline (train-window label
//      variance range, digest [20]) so the reported loss is finite and every
//      scored variance forecast is positive. The clip touches ONLY the QLIKE
//      scoring path -- pred_label in the signal stays the raw GBT output
//      (rank information preserved).
//   4. Write the FROZEN vrp_signal_v1 TSV (`# schema=vrp_signal_v1`;
//      columns EXACTLY symbol, date, pred_label, pred_edge_norm, vov_63d):
//      one row per OOS test observation (each labeled row appears in at
//      most one test fold; step >= test) plus the NaN-label tail rows
//      scored by the FINAL fold's models. pred_label is the GBT prediction
//      in variance*N units; pred_edge_norm standardizes it by the row's
//      asset's TRAIN-fold label mean/sd (cross-sectionally rankable);
//      vov_63d passes a FINITE f9 through raw, and imputes a non-finite f9
//      (63-session warmup / iv gaps) with the row's asset's TRAIN-fold f9
//      mean from the fold that scored the row (round-2 F2: the frozen
//      vrp_signal_v1 loader fail-closes on non-finite vov_63d, so emitted
//      rows NEVER carry one; the digest's per-asset train-fold imputation
//      recipe, applied at the writer against the untouched loader). Also
//      writes a metrics TSV (carrying the F1 rejection counters plus, per
//      fold, the purge/embargo-removed train counts and the GBT insanity-
//      clip count and post-clip extrema as `# key=value` meta lines --
//      round-2 review majors 2 + 3), the two serialized schema-2 model files
//      (pricing/theo.hpp formats), and the vrp_fold_stats_v1 SIDECAR
//      (save_vrp_fold_stats below): per fold, the per-asset feature
//      mean/sd, per-asset label mean/sd, and the baseline retransform
//      state (s2, train_mean_log, train-window variance clip bounds), so a
//      consumer can score RAW panel rows from {model file + sidecar} alone
//      and reproduce pred_label / pred_edge_norm / the clipped baseline
//      variance forecast. The serialized models score PER-ASSET-STANDARDIZED
//      panel features (the trainer folds its fit-internal global
//      standardization into the coefficients/thresholds; the per-asset
//      train-window stats live in the sidecar). The linear file scores
//      ln(rv_fwd^2) BEFORE retransformation/clip; s2 and the clip bounds it
//      needs are persisted per fold in the sidecar.
//
// Layering: this header (and the two TUs that include it -- the CLI and the
// test) is the ONLY place in atx-vol that includes atx-engine headers. The
// atx-vol LIBRARY never does.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "pricing/theo.hpp" // kVrpFeatureSchemaV1, model files (schema 2)
#include "atx/vol/api/backtest/research_validation.hpp"
#include "atx/vol/api/core/types.hpp"

// atx-engine learn layer (fit_gbt / fit_linear / elastic_net kernels; the
// definitions live in the atx-engine library, linked by the two consumer
// targets only -- see the CMake comments).
#include "atx/engine/learn/gbt.hpp"
#include "atx/engine/learn/linear_alpha.hpp"

namespace atx::vol::vrp {

namespace learn = atx::engine::learn;

using atx::core::Err;
using atx::core::Ok;

// ── Frozen vrp_panel_v1 contract ────────────────────────────────────────────

inline constexpr std::string_view kVrpPanelSchemaValue = "vrp_panel_v1";
inline constexpr std::size_t kVrpPanelColumnCount = 18;
inline constexpr std::size_t kVrpHorizonSessions = 21;
inline constexpr double kVrpHorizonYears = 21.0 / 252.0;

inline constexpr std::array<std::string_view, kVrpPanelColumnCount> kVrpPanelColumns{
    "symbol",        "date",       "entry_ts_ns", "spot",       "iv_fair_21d", "iv_fair_63d",
    "rv_fwd_21d",    "label",      "f0_log_rv1",  "f1_log_rv5", "f2_log_rv21", "f3_iv_level",
    "f4_term_slope", "f5_hv_iv_gap", "f6_vrp_lag", "f7_ret_21d", "f8_jump_recent", "f9_vov_63d"};

struct VrpPanelRow {
  std::string symbol;
  std::string date; // opaque session label (YYYY-MM-DD in real panels)
  std::int64_t entry_ts_ns{0};
  double spot{0.0};
  double iv_fair_21d{0.0};
  double iv_fair_63d{0.0};
  double rv_fwd_21d{0.0}; // NaN on tail (predict-time) rows
  double label{0.0};      // NaN on tail rows; else (rv^2 - iv21^2) * 21/252
  std::array<double, kVrpFeatureCount> f{}; // RAW panel features; NaN allowed
};

// Rows in canonical (entry_ts_ns, symbol) ascending order; `symbols` sorted
// unique; `row_symbol[i]` indexes `symbols` for `rows[i]`.
struct VrpPanel {
  std::vector<VrpPanelRow> rows;
  std::vector<std::string> symbols;
  std::vector<std::size_t> row_symbol;
  std::uint64_t source_file_size{0};
};

// A labeled panel row is trainable AND scoreable: finite label + finite
// close-to-close proxy. Tail rows fail this and stay predict-time only.
[[nodiscard]] inline bool is_labeled_row(const VrpPanelRow &row) noexcept {
  return std::isfinite(row.label) && std::isfinite(row.rv_fwd_21d);
}

namespace detail {

[[nodiscard]] inline std::string_view rstrip_cr(std::string_view v) noexcept {
  if (!v.empty() && v.back() == '\r') {
    v.remove_suffix(1);
  }
  return v;
}

[[nodiscard]] inline std::string_view trim(std::string_view v) noexcept {
  const std::size_t start = v.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = v.find_last_not_of(" \t");
  return v.substr(start, end - start + 1);
}

// Strict tab split (empty fields preserved -- a missing field must FAIL the
// count check, not silently shift its neighbors).
inline void split_tabs(std::string_view line, std::vector<std::string_view> &out) {
  out.clear();
  std::size_t start = 0;
  // Bounded by line length: every iteration advances `start` past one tab.
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      out.push_back(line.substr(start));
      return;
    }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

// Double parse accepting the panel's NaN spellings ("nan"/"NaN"/"-nan"...).
[[nodiscard]] inline bool parse_double(std::string_view tok, double &out) {
  const auto r =
      std::from_chars(tok.data(), tok.data() + tok.size(), out, std::chars_format::general);
  if (r.ec == std::errc{} && r.ptr == tok.data() + tok.size()) {
    return true;
  }
  std::string lower(tok);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "nan" || lower == "-nan" || lower == "+nan") {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  return false;
}

[[nodiscard]] inline bool parse_i64(std::string_view tok, std::int64_t &out) {
  const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), out);
  return r.ec == std::errc{} && r.ptr == tok.data() + tok.size();
}

// std::to_chars shortest round-trip -- the canonical double spelling every
// output file uses (byte-stable across identical runs).
[[nodiscard]] inline std::string fmt_double(double v) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return std::string(buf.data(), r.ptr);
}

[[nodiscard]] inline std::string expected_panel_header() {
  std::string h;
  for (std::size_t i = 0; i < kVrpPanelColumns.size(); ++i) {
    if (i != 0) {
      h += '\t';
    }
    h += kVrpPanelColumns[i];
  }
  return h;
}

} // namespace detail

// ── Panel loader (fail closed on any contract mismatch) ─────────────────────

[[nodiscard]] inline Result<VrpPanel> load_vrp_panel(std::string_view path) {
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError, "load_vrp_panel: cannot open '" + std::string{path} + "'");
  }

  std::optional<std::string> schema_value;
  std::optional<std::int64_t> horizon_days;
  bool header_seen = false;
  VrpPanel panel;
  std::string raw_line;
  std::vector<std::string_view> fields;
  // Bounded by the file's own line count -- std::getline terminates at EOF.
  while (std::getline(in, raw_line)) {
    const std::string_view line = detail::rstrip_cr(raw_line);
    if (detail::trim(line).empty()) {
      continue;
    }
    if (line.front() == '#') {
      const std::string_view body = detail::trim(line.substr(1));
      constexpr std::string_view kSchemaKey = "schema=";
      constexpr std::string_view kHorizonKey = "horizon_days=";
      if (!schema_value.has_value() && body.substr(0, kSchemaKey.size()) == kSchemaKey) {
        schema_value = std::string(body.substr(kSchemaKey.size()));
      } else if (!horizon_days.has_value() &&
                 body.substr(0, kHorizonKey.size()) == kHorizonKey) {
        std::int64_t h = 0;
        if (!detail::parse_i64(body.substr(kHorizonKey.size()), h)) {
          return Err(ErrorCode::ParseError, "load_vrp_panel: unparseable horizon_days comment");
        }
        horizon_days = h;
      }
      continue;
    }
    if (!header_seen) {
      // Frozen contract gate BEFORE touching data: schema comment first.
      if (!schema_value.has_value() || *schema_value != kVrpPanelSchemaValue) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_panel: schema comment mismatch (expected '# schema=vrp_panel_v1')");
      }
      if (!horizon_days.has_value() ||
          *horizon_days != static_cast<std::int64_t>(kVrpHorizonSessions)) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_panel: horizon_days comment mismatch (expected 21)");
      }
      if (line != detail::expected_panel_header()) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_panel: header row does not match the frozen vrp_panel_v1 "
                   "column order");
      }
      header_seen = true;
      continue;
    }
    detail::split_tabs(line, fields);
    if (fields.size() != kVrpPanelColumnCount) {
      return Err(ErrorCode::ParseError,
                 "load_vrp_panel: expected " + std::to_string(kVrpPanelColumnCount) +
                     " tab-separated fields, got " + std::to_string(fields.size()));
    }
    VrpPanelRow row;
    row.symbol = std::string(fields[0]);
    row.date = std::string(fields[1]);
    if (row.symbol.empty() || row.date.empty()) {
      return Err(ErrorCode::ParseError, "load_vrp_panel: empty symbol/date field");
    }
    if (!detail::parse_i64(fields[2], row.entry_ts_ns)) {
      return Err(ErrorCode::ParseError,
                 "load_vrp_panel: unparseable entry_ts_ns '" + std::string(fields[2]) + "'");
    }
    const auto parse_field = [&](std::size_t idx, double &out) -> bool {
      return detail::parse_double(fields[idx], out);
    };
    bool ok = parse_field(3, row.spot) && parse_field(4, row.iv_fair_21d) &&
              parse_field(5, row.iv_fair_63d) && parse_field(6, row.rv_fwd_21d) &&
              parse_field(7, row.label);
    for (std::size_t k = 0; ok && k < kVrpFeatureCount; ++k) {
      ok = parse_field(8 + k, row.f[k]);
    }
    if (!ok) {
      return Err(ErrorCode::ParseError, "load_vrp_panel: unparseable numeric field in row for '" +
                                            row.symbol + "' @ '" + row.date + "'");
    }
    if (std::isfinite(row.label)) {
      // A labeled row must carry a usable proxy + fair-IV pair: QLIKE needs
      // rv^2 > 0 and the edge mapping needs iv^2 (fail closed, contract-tied).
      if (!(std::isfinite(row.rv_fwd_21d) && row.rv_fwd_21d > 0.0) ||
          !(std::isfinite(row.iv_fair_21d) && row.iv_fair_21d > 0.0)) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_panel: labeled row without positive rv_fwd_21d/iv_fair_21d ('" +
                       row.symbol + "' @ '" + row.date + "')");
      }
    }
    // f9_vov_63d is a sample stdev, so a finite value must be >= 0 (round-2
    // review minor). Validated at the boundary so neither the raw signal
    // pass-through nor the per-asset imputation mean (signal_vov below) can
    // ever hand the frozen vrp_signal_v1 loader a negative vov_63d.
    if (std::isfinite(row.f[9]) && row.f[9] < 0.0) {
      return Err(ErrorCode::ParseError,
                 "load_vrp_panel: negative f9_vov_63d ('" + row.symbol + "' @ '" + row.date +
                     "'); vov_63d is a sample stdev, so a finite value must be >= 0");
    }
    panel.rows.push_back(std::move(row));
  }
  if (!header_seen) {
    return Err(ErrorCode::ParseError, "load_vrp_panel: missing header row");
  }
  if (panel.rows.empty()) {
    return Err(ErrorCode::ParseError, "load_vrp_panel: no data rows");
  }

  // Canonical (entry_ts_ns, symbol) order + duplicate-key rejection.
  std::sort(panel.rows.begin(), panel.rows.end(),
            [](const VrpPanelRow &a, const VrpPanelRow &b) {
              if (a.entry_ts_ns != b.entry_ts_ns) {
                return a.entry_ts_ns < b.entry_ts_ns;
              }
              return a.symbol < b.symbol;
            });
  for (std::size_t i = 1; i < panel.rows.size(); ++i) {
    if (panel.rows[i - 1].entry_ts_ns == panel.rows[i].entry_ts_ns &&
        panel.rows[i - 1].symbol == panel.rows[i].symbol) {
      return Err(ErrorCode::ParseError, "load_vrp_panel: duplicate (entry_ts_ns, symbol) row '" +
                                            panel.rows[i].symbol + "'");
    }
  }

  panel.symbols.reserve(8);
  for (const VrpPanelRow &row : panel.rows) {
    panel.symbols.push_back(row.symbol);
  }
  std::sort(panel.symbols.begin(), panel.symbols.end());
  panel.symbols.erase(std::unique(panel.symbols.begin(), panel.symbols.end()),
                      panel.symbols.end());
  panel.row_symbol.reserve(panel.rows.size());
  for (const VrpPanelRow &row : panel.rows) {
    const auto it = std::lower_bound(panel.symbols.begin(), panel.symbols.end(), row.symbol);
    panel.row_symbol.push_back(static_cast<std::size_t>(it - panel.symbols.begin()));
  }

  std::error_code ec;
  const std::uintmax_t bytes = std::filesystem::file_size(std::filesystem::path{path}, ec);
  panel.source_file_size = ec ? 1u : static_cast<std::uint64_t>(bytes);
  return Ok(std::move(panel));
}

// ── Per-asset standardization (train-fold rows ONLY -- digest Pitfall 6) ────

struct VrpStandardization {
  std::vector<std::array<double, kVrpFeatureCount>> mean; // per symbol index
  std::vector<std::array<double, kVrpFeatureCount>> sd;   // population sd; 0 => degenerate
};

// Stats over EXACTLY the given rows (the caller passes train-fold rows only;
// the test pins that a perturbed test row changes nothing). Per (symbol,
// feature), NaN cells are excluded from the accumulation -- a feature that is
// sometimes missing still standardizes on its observed values.
[[nodiscard]] inline VrpStandardization
compute_asset_standardization(const VrpPanel &panel, std::span<const std::size_t> rows) {
  const std::size_t n_sym = panel.symbols.size();
  VrpStandardization stz;
  stz.mean.assign(n_sym, {});
  stz.sd.assign(n_sym, {});
  std::vector<std::array<double, kVrpFeatureCount>> sum(n_sym, {});
  std::vector<std::array<std::size_t, kVrpFeatureCount>> cnt(n_sym, {});
  for (const std::size_t r : rows) {
    const std::size_t s = panel.row_symbol[r];
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      const double x = panel.rows[r].f[f];
      if (std::isfinite(x)) {
        sum[s][f] += x;
        ++cnt[s][f];
      }
    }
  }
  for (std::size_t s = 0; s < n_sym; ++s) {
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      if (cnt[s][f] > 0) {
        stz.mean[s][f] = sum[s][f] / static_cast<double>(cnt[s][f]);
      }
    }
  }
  std::vector<std::array<double, kVrpFeatureCount>> sq(n_sym, {});
  for (const std::size_t r : rows) {
    const std::size_t s = panel.row_symbol[r];
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      const double x = panel.rows[r].f[f];
      if (std::isfinite(x)) {
        const double d = x - stz.mean[s][f];
        sq[s][f] += d * d;
      }
    }
  }
  for (std::size_t s = 0; s < n_sym; ++s) {
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      if (cnt[s][f] > 0) {
        stz.sd[s][f] = std::sqrt(sq[s][f] / static_cast<double>(cnt[s][f]));
      }
    }
  }
  return stz;
}

// z-score of one raw panel feature under the given (train-fold) stats.
// NaN feature or degenerate sd -> 0.0: the per-asset TRAIN-MEAN imputation
// in z-space -- explicit NaN handling, never silently dropping the column.
[[nodiscard]] inline double standardized_feature(const VrpPanel &panel,
                                                 const VrpStandardization &stz, std::size_t row,
                                                 std::size_t feature) {
  const double x = panel.rows[row].f[feature];
  if (!std::isfinite(x)) {
    return 0.0;
  }
  const std::size_t s = panel.row_symbol[row];
  const double sd = stz.sd[s][feature];
  if (sd == 0.0) {
    return 0.0;
  }
  return (x - stz.mean[s][feature]) / sd;
}

// ── Observations + purged/embargoed walk-forward plan ───────────────────────

struct VrpObservations {
  std::vector<ResearchObservation> obs; // canonical (decision_ts_ns, uid) order
  std::vector<std::size_t> row_of;      // obs index -> panel row index
  // F1 accounting (surfaced in the metrics meta lines and the CLI's stderr):
  std::size_t n_labeled_rows{0};           // labeled panel rows seen
  std::size_t n_rows_rejected_no_t21{0};   // labeled rows lacking a t+21 row
  std::size_t n_symbols_fully_rejected{0}; // symbols whose EVERY labeled row was rejected
};

// One ResearchObservation per USABLE labeled panel row. decision =
// entry_ts_ns; label interval [t, t+21 sessions]: label_end is the POOLED
// session axis timestamp 21 DISTINCT sessions later -- the TRUE horizon
// (round-2 review major 2; the panel builder labels over 21 bar-axis
// sessions, and the pooled axis is the panel's own session calendar). The
// previous 21-EMITTED-rows-later end overstated sparse symbols' label
// spans (up to 250 calendar days on the real SP100 corpus) and, through
// make_vrp_plan's global-max embargo, purged every training observation on
// the motivating 102-name panel. The t+21 invariant stays PER ROW and
// UNCHANGED (F1): a labeled row whose symbol lacks an emitted row 21
// positions later (interior surface holes inside the final horizon) loses
// only itself -- rejected and counted, with fully-rejected symbols counted
// separately -- never the symbol or the run. The mirrored check fails
// CLOSED: an unlabeled (NaN-label) row that DOES have a t+21 successor is
// not a tail row, and silently scoring it with the final fold's models
// would hand it a hindsight prediction (review minor, round 1).
// uid = symbol index + 1 (uid 0 is rejected upstream).
[[nodiscard]] inline Result<VrpObservations> build_vrp_observations(const VrpPanel &panel) {
  const std::size_t n_sym = panel.symbols.size();
  std::vector<std::vector<std::size_t>> sym_rows(n_sym);
  for (std::size_t r = 0; r < panel.rows.size(); ++r) {
    sym_rows[panel.row_symbol[r]].push_back(r); // ascending ts per symbol
  }
  std::vector<std::size_t> pos_in_symbol(panel.rows.size(), 0);
  for (std::size_t s = 0; s < n_sym; ++s) {
    for (std::size_t p = 0; p < sym_rows[s].size(); ++p) {
      pos_in_symbol[sym_rows[s][p]] = p;
    }
  }
  // Pooled session axis: sorted distinct entry_ts_ns over ALL symbols' rows.
  // Rows are canonical ascending, so one linear scan builds the axis and
  // each row's session index.
  std::vector<std::int64_t> session_ts;
  session_ts.reserve(panel.rows.size());
  std::vector<std::size_t> row_session(panel.rows.size(), 0);
  for (std::size_t r = 0; r < panel.rows.size(); ++r) {
    const std::int64_t ts = panel.rows[r].entry_ts_ns;
    if (session_ts.empty() || session_ts.back() != ts) {
      session_ts.push_back(ts);
    }
    row_session[r] = session_ts.size() - 1;
  }

  VrpObservations out;
  std::vector<std::size_t> sym_labeled(n_sym, 0);
  std::vector<std::size_t> sym_rejected(n_sym, 0);
  for (std::size_t r = 0; r < panel.rows.size(); ++r) {
    const VrpPanelRow &row = panel.rows[r];
    const std::size_t s = panel.row_symbol[r];
    const std::size_t p = pos_in_symbol[r];
    // sym_rows[s] contains r, so .size() >= 1 and the -1 below cannot wrap.
    const bool has_t21_row = p + kVrpHorizonSessions <= sym_rows[s].size() - 1;
    if (!is_labeled_row(row)) {
      if (has_t21_row) {
        return Err(ErrorCode::InvalidArgument,
                   "build_vrp_observations: non-tail unlabeled row ('" + row.symbol + "' @ '" +
                       row.date + "' has a t+21 session row; a mid-sample NaN label would be "
                       "scored with hindsight models)");
      }
      continue;
    }
    ++out.n_labeled_rows;
    ++sym_labeled[s];
    if (!has_t21_row) {
      ++out.n_rows_rejected_no_t21; // F1: this row only, never the symbol
      ++sym_rejected[s];
      continue;
    }
    // TRUE t+21 horizon on the pooled session axis. In range by admission:
    // the row's 21st same-symbol emitted successor exists, within-symbol
    // rows occupy strictly increasing pooled session indices, so that
    // successor sits at pooled index >= row_session[r] + 21. For the same
    // reason the pooled label_end is never LATER than the old emitted-row
    // one: the true window is a lower bound the old code only overstated.
    const std::size_t end_session = row_session[r] + kVrpHorizonSessions;
    ResearchObservation ob;
    ob.uid = static_cast<std::uint32_t>(s + 1);
    ob.observed_ts_ns = row.entry_ts_ns;
    ob.available_ts_ns = row.entry_ts_ns;
    ob.decision_ts_ns = row.entry_ts_ns;
    ob.execution_ts_ns = row.entry_ts_ns + 1; // strictly after the decision
    ob.label_end_ts_ns = session_ts[end_session];
    ob.signal = 0.0;
    ob.forward_pnl = row.label;
    ob.lagged_capital = 1.0;
    ob.source_identity.file_size = panel.source_file_size == 0 ? 1u : panel.source_file_size;
    out.obs.push_back(ob);
    out.row_of.push_back(r);
  }
  for (std::size_t s = 0; s < n_sym; ++s) {
    if (sym_labeled[s] > 0 && sym_rejected[s] == sym_labeled[s]) {
      ++out.n_symbols_fully_rejected;
    }
  }
  if (out.obs.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "build_vrp_observations: no usable labeled rows (labeled=" +
                   std::to_string(out.n_labeled_rows) + ", rejected_no_t21=" +
                   std::to_string(out.n_rows_rejected_no_t21) + ")");
  }
  // Panel rows are canonical (ts, symbol) and uid follows the symbol order,
  // so `obs` is already in canonical (decision_ts_ns, uid) order.
  return Ok(std::move(out));
}

struct VrpWalkForwardCfg {
  std::size_t min_train_sessions{126};
  std::size_t test_sessions{21};
  std::size_t step_sessions{21};
};

// Auto-scaled walk-forward defaults (round-2 F1 companion): keep the
// production 252/63/63 plan whenever the panel carries enough labeled
// decision groups for it, otherwise scale the fold plan to the available
// depth -- test/step = clamp(n/6, 21, 63) (never below one full label
// horizon), min_train = clamp(n/3, 84, 252). The floors make the smallest
// trainable panel 105 labeled groups (126 sessions with the 21-session NaN
// tail); anything thinner still fails closed inside make_vrp_plan with the
// "insufficient decision groups" error -- auto-scaling changes WHERE the
// line sits, never removes it.
[[nodiscard]] inline VrpWalkForwardCfg derive_vrp_walk_forward(std::size_t n_groups) noexcept {
  constexpr std::size_t kDefaultTrain = 252;
  constexpr std::size_t kDefaultTest = 63;
  constexpr std::size_t kTrainFloor = 84;
  constexpr std::size_t kTestFloor = kVrpHorizonSessions;
  if (n_groups >= kDefaultTrain + kDefaultTest) {
    return VrpWalkForwardCfg{kDefaultTrain, kDefaultTest, kDefaultTest};
  }
  const std::size_t test = std::clamp(n_groups / 6, kTestFloor, kDefaultTest);
  const std::size_t train = std::clamp(n_groups / 3, kTrainFloor, kDefaultTrain);
  return VrpWalkForwardCfg{train, test, test};
}

// Anchored purged walk-forward over decision-timestamp groups (sessions).
// embargo_ns = max observed [t, t+21-session] wall-clock span, so the
// embargo always covers >= 21 sessions regardless of weekends/holidays.
// Label spans are TRUE pooled-axis horizons (build_vrp_observations), so
// the max is ~21 sessions plus calendar gaps -- a sparse symbol can no
// longer stretch it to months (round-2 review major 2).
[[nodiscard]] inline Result<ResearchValidationPlan>
make_vrp_plan(const VrpObservations &observations, const VrpWalkForwardCfg &walk) {
  std::int64_t embargo_ns = 0;
  for (const ResearchObservation &ob : observations.obs) {
    embargo_ns = std::max(embargo_ns, ob.label_end_ts_ns - ob.decision_ts_ns);
  }
  ResearchWalkForwardSpec spec;
  spec.kind = ResearchWalkForwardKind::Anchored;
  spec.min_train_groups = walk.min_train_sessions;
  spec.test_groups = walk.test_sessions;
  spec.step_groups = walk.step_sessions;
  spec.embargo_ns = embargo_ns;
  return make_purged_walk_forward_plan(
      std::span<const ResearchObservation>{observations.obs}, spec);
}

// ── Losses + retransformation ───────────────────────────────────────────────

// QLIKE in VARIANCE levels: L(F, P) = P/F - ln(P/F) - 1 (Patton 2011's
// robust loss; ranking is proxy-noise-robust ONLY in variance levels --
// never score log-vol). Precondition: F > 0, P > 0.
[[nodiscard]] inline double vrp_qlike(double forecast_var, double proxy_var) noexcept {
  const double ratio = proxy_var / forecast_var;
  return ratio - std::log(ratio) - 1.0;
}

// Lognormal retransformation + insanity filter (digest [20]): a log-space
// forecast mu with residual variance s2 retransforms to exp(mu + s2/2), then
// clips into the train-window label range [var_min, var_max].
[[nodiscard]] inline double vrp_retransform_clip(double log_var_forecast, double residual_var,
                                                 double train_var_min,
                                                 double train_var_max) noexcept {
  const double f = std::exp(log_var_forecast + 0.5 * residual_var);
  return std::clamp(f, train_var_min, train_var_max);
}

// ── FeatureMatrix bridge + model predict helpers ────────────────────────────

namespace detail {

// Build a pooled FeatureMatrix over `rows` (panel indices, canonical order):
// X = per-asset-standardized features (subset `features`, z-imputed finite),
// Y[0] = `labels[i]` for rows[i] (NaN rows are skipped by the engine's
// build_design). Dates become dense ordinals in encounter order (rows are
// ascending in entry_ts_ns, so ordinals are chronological).
[[nodiscard]] inline learn::FeatureMatrix
build_feature_matrix(const VrpPanel &panel, const VrpStandardization &stz,
                     std::span<const std::size_t> rows, std::span<const std::size_t> features,
                     std::span<const double> labels) {
  learn::FeatureMatrix fm;
  fm.n_instruments = panel.symbols.size();
  fm.n_features = features.size();
  fm.Y.resize(1);
  std::int64_t prev_ts = std::numeric_limits<std::int64_t>::min();
  std::size_t date_ord = 0;
  bool first = true;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::size_t r = rows[i];
    const std::int64_t ts = panel.rows[r].entry_ts_ns;
    if (first) {
      first = false;
    } else if (ts != prev_ts) {
      ++date_ord;
    }
    prev_ts = ts;
    (void)fm.push_row(date_ord, panel.row_symbol[r]);
    for (const std::size_t f : features) {
      fm.X.push_back(standardized_feature(panel, stz, r, f));
    }
    fm.Y[0].push_back(labels[i]);
    fm.row_valid.push_back(1); // z-imputation keeps every feature finite
  }
  fm.n_dates = rows.empty() ? 0 : date_ord + 1;
  return fm;
}

// Deployed-model scalar prediction for one z-feature row (shared augmented
// layout: standardization folded in by the model's own feat_mean/feat_sd).
[[nodiscard]] inline double predict_model(const learn::LearnedModel &m,
                                          std::span<const double> base) {
  const std::size_t k = m.aug.pca.has_value() ? static_cast<std::size_t>(m.aug.pca->k) : 0u;
  std::vector<double> latent(k, 0.0);
  std::vector<double> aug(m.augmented_dim(), 0.0);
  if (!learn::build_augmented_row(m, base, std::span<double>{latent}, std::span<double>{aug})) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return learn::predict_blended(m, std::span<const double>{aug});
}

// Mean per-date Spearman rank IC of (prediction, realized) pairs grouped by
// timestamp; dates with < 2 pairs contribute nothing; no qualifying date ->
// 0.0. `ts` parallels `pred`/`real` and arrives ascending.
[[nodiscard]] inline double mean_per_date_spearman(std::span<const std::int64_t> ts,
                                                   std::span<const double> pred,
                                                   std::span<const double> realized) {
  double sum = 0.0;
  std::size_t n_dates = 0;
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    if (end - begin >= 2) {
      const std::vector<double> p(pred.begin() + static_cast<std::ptrdiff_t>(begin),
                                  pred.begin() + static_cast<std::ptrdiff_t>(end));
      const std::vector<double> q(realized.begin() + static_cast<std::ptrdiff_t>(begin),
                                  realized.begin() + static_cast<std::ptrdiff_t>(end));
      sum += learn::detail::spearman(std::span<const double>{p}, std::span<const double>{q});
      ++n_dates;
    }
    begin = end;
  }
  return n_dates == 0 ? 0.0 : sum / static_cast<double>(n_dates);
}

} // namespace detail

// ── Model conversion (engine LearnedModel -> schema-2 file forms) ───────────

// Folds the model's fit-internal global standardization (feat_mean/feat_sd
// over the z-feature training design) into RAW-input coefficients:
//   score = sum_f c_f * (x_f - m_f)/s_f  ==>  b_f = c_f/s_f,
//   b0 = add_intercept - sum_f c_f m_f / s_f.
// `feature_slots[j]` places the fm's column j at that schema slot (unused
// slots stay 0). `add_intercept` carries the trainer's own label mean (the
// elastic-net kernel fits no intercept; with zero-mean standardized columns
// the coefficients match a centered fit, so the mean adds back linearly).
[[nodiscard]] inline Result<LinearFairVolParams>
linear_model_to_fair_vol_params(const learn::LearnedModel &m, double add_intercept,
                                std::uint32_t schema,
                                std::span<const std::size_t> feature_slots) {
  const std::size_t width = fair_vol_feature_count(schema);
  if (width == 0) {
    return Err(ErrorCode::InvalidArgument,
               "linear_model_to_fair_vol_params: unsupported schema");
  }
  if (m.kind != learn::ModelKind::Linear || m.coeffs.size() != 1 || m.blend_w.size() != 1 ||
      m.aug.pca.has_value() || !m.aug.interactions.empty() ||
      static_cast<std::size_t>(m.n_base_features) != feature_slots.size()) {
    return Err(ErrorCode::InvalidArgument,
               "linear_model_to_fair_vol_params: model shape mismatch");
  }
  if (std::fabs(m.blend_w[0] - 1.0) > 1e-12) {
    return Err(ErrorCode::InvalidArgument,
               "linear_model_to_fair_vol_params: single-horizon blend weight != 1");
  }
  LinearFairVolParams params;
  params.feature_schema = schema;
  params.coefficients.assign(width, 0.0);
  double intercept = add_intercept;
  for (std::size_t j = 0; j < feature_slots.size(); ++j) {
    const double c = m.coeffs[0](static_cast<Eigen::Index>(j));
    const double sd = m.feat_sd[j];
    if (sd == 0.0) {
      if (c != 0.0) {
        return Err(ErrorCode::InvalidArgument,
                   "linear_model_to_fair_vol_params: nonzero coefficient on a degenerate "
                   "(sd == 0) column");
      }
      continue;
    }
    const std::size_t slot = feature_slots[j];
    if (slot >= width) {
      return Err(ErrorCode::InvalidArgument,
                 "linear_model_to_fair_vol_params: feature slot out of schema width");
    }
    params.coefficients[slot] = c / sd;
    intercept -= c * m.feat_mean[j] / sd;
  }
  params.intercept = intercept;
  return Ok(std::move(params));
}

// Flattens the single-horizon deployed forest into the schema-2 flat-array
// form, un-standardizing each split threshold the same way (z < t  <=>
// x < t*s_f + m_f; s_f > 0 for any split column -- a constant column can
// never win a split, so sd == 0 under a split is a hard error).
[[nodiscard]] inline Result<GbtFairVolModelData>
gbt_model_to_fair_vol_data(const learn::LearnedModel &m, std::uint32_t schema) {
  const std::size_t width = fair_vol_feature_count(schema);
  if (width == 0) {
    return Err(ErrorCode::InvalidArgument, "gbt_model_to_fair_vol_data: unsupported schema");
  }
  if (m.kind != learn::ModelKind::Gbt || m.forests.size() != 1 || m.blend_w.size() != 1 ||
      m.aug.pca.has_value() || !m.aug.interactions.empty() ||
      static_cast<std::size_t>(m.n_base_features) != width) {
    return Err(ErrorCode::InvalidArgument, "gbt_model_to_fair_vol_data: model shape mismatch");
  }
  if (std::fabs(m.blend_w[0] - 1.0) > 1e-12) {
    return Err(ErrorCode::InvalidArgument,
               "gbt_model_to_fair_vol_data: single-horizon blend weight != 1");
  }
  const learn::GbtForest &forest = m.forests[0];
  GbtFairVolModelData data;
  data.feature_schema = schema;
  data.base = forest.base;
  for (const learn::GbtTree &tree : forest.trees) {
    const auto offset = static_cast<std::uint32_t>(data.feature_idx.size());
    data.tree_first_node.push_back(offset);
    for (const learn::GbtNode &node : tree.nodes) {
      if (node.is_leaf) {
        data.feature_idx.push_back(0);
        data.threshold.push_back(0.0);
        data.left.push_back(-1);
        data.right.push_back(-1);
        data.leaf_value.push_back(node.leaf_value);
        continue;
      }
      const auto f = static_cast<std::size_t>(node.feature);
      if (f >= width) {
        return Err(ErrorCode::InvalidArgument,
                   "gbt_model_to_fair_vol_data: split feature out of schema width");
      }
      const double sd = m.feat_sd[f];
      if (sd == 0.0) {
        return Err(ErrorCode::InvalidArgument,
                   "gbt_model_to_fair_vol_data: split on a degenerate (sd == 0) column");
      }
      data.feature_idx.push_back(node.feature);
      data.threshold.push_back(node.threshold * sd + m.feat_mean[f]);
      data.left.push_back(node.left + static_cast<std::int32_t>(offset));
      data.right.push_back(node.right + static_cast<std::int32_t>(offset));
      data.leaf_value.push_back(0.0);
    }
  }
  return Ok(std::move(data));
}

// ── Trainer configuration / report ──────────────────────────────────────────

struct VrpTrainConfig {
  std::string panel_path;
  std::string out_dir;
  std::uint64_t master_seed{42};
  VrpWalkForwardCfg walk{};
  // true => ignore `walk` and derive the fold plan from the panel's labeled
  // decision-group depth (derive_vrp_walk_forward). The CLI turns this on
  // whenever the caller passes none of the three walk flags.
  bool walk_auto{false};
  double en_lambda{1e-3};
  double en_alpha{0.5};
};

struct VrpFoldMetrics {
  std::uint32_t fold_id{0};
  std::size_t n_train{0};
  std::size_t n_test{0};
  double qlike_baseline{0.0};
  double qlike_gbt{0.0};
  double qlike_mean_forecast{0.0};
  double ic_baseline{0.0};
  double ic_gbt{0.0};
  double train_var_min{0.0};            // insanity-clip lower bound (variance)
  double train_var_max{0.0};            // insanity-clip upper bound (variance)
  double baseline_test_forecast_max{0.0}; // max clipped baseline forecast on test
  // GBT QLIKE-path insanity clip (digest [20], round-2 item 4): how many test
  // rows' implied variance forecasts left the train range, and the post-clip
  // extrema (min must stay > 0 -- QLIKE's precondition). Persisted in the
  // metrics meta lines (round-2 review major 3) and printed by the CLI.
  std::size_t n_gbt_forecast_clipped{0};
  double gbt_test_forecast_min{0.0};
  double gbt_test_forecast_max{0.0};
  // Train candidates the plan removed for this fold (round-2 review major 2:
  // surface the purge/embargo loss) -- outcome-purged and pre-test-embargoed
  // observation counts from the fold's ResearchValidationFold.
  std::size_t n_train_purged{0};
  std::size_t n_train_embargoed{0};
  std::vector<std::size_t> train_rows;  // panel row indices
  std::vector<std::size_t> test_rows;   // panel row indices
};

// ── Per-fold stats sidecar (vrp_fold_stats_v1) ──────────────────────────────
//
// The ADDITIVE live-path artifact written next to the model files: everything
// a consumer needs, per fold, to score a RAW panel row from files alone --
// per-asset feature mean/sd (standardize raw features exactly as the trainer
// did, NaN/degenerate -> z = 0), per-asset label mean/sd (pred_edge_norm),
// and the baseline retransform state (s2, train_mean_log, and the insanity
// clip bounds; the linear model file scores ln(rv_fwd^2) PRE-retransform).
// Canonical TSV grammar (byte-stable; save(load(f)) of a save-produced file
// is byte-identical):
//   # schema=vrp_fold_stats_v1
//   fold_id\tkind\tsymbol\tname\tvalue
//   <id>\tfold\t-\t<name>\t<value>          (fixed fold-field order below)
//   <id>\tasset\t<symbol>\t<name>\t<value>  (symbols ascending; label_mean,
//                                            label_sd, f0_mean, f0_sd, ...)
// The loader is STRICT: it accepts exactly the canonical layout and fails
// closed (ParseError) on anything else.
struct VrpFoldStats {
  std::uint32_t fold_id{0};
  std::size_t n_train{0};
  std::size_t n_test{0};
  double baseline_s2{0.0};             // train residual variance, log space
  double baseline_train_mean_log{0.0}; // ybar the no-intercept kernel omits
  double train_var_min{0.0};           // insanity clip bounds (variance)
  double train_var_max{0.0};
  double train_var_mean{0.0};          // the mean-forecast benchmark
  std::vector<std::string> symbols;    // == panel.symbols (sorted unique)
  std::vector<std::array<double, kVrpFeatureCount>> feat_mean; // per symbol
  std::vector<std::array<double, kVrpFeatureCount>> feat_sd;
  std::vector<double> label_mean;
  std::vector<double> label_sd;

  [[nodiscard]] bool operator==(const VrpFoldStats &) const = default;
};

struct VrpTrainReport {
  VrpPanel panel;
  VrpObservations observations;
  ResearchValidationPlan plan;
  std::vector<VrpFoldMetrics> folds;
  std::vector<VrpFoldStats> fold_stats; // parallel to `folds`
  std::filesystem::path signal_path;
  std::filesystem::path metrics_path;
  std::filesystem::path gbt_model_path;
  std::filesystem::path baseline_model_path;
  std::filesystem::path fold_stats_path;
};

inline constexpr std::string_view kVrpFoldStatsSchemaValue = "vrp_fold_stats_v1";

namespace detail {

// Canonical fold-level field order of the vrp_fold_stats_v1 grammar. The
// writer emits exactly this order; the loader requires it (fail closed).
inline constexpr std::array<std::string_view, 7> kVrpFoldStatsFoldFields{
    "n_train",       "n_test",        "baseline_s2", "baseline_train_mean_log",
    "train_var_min", "train_var_max", "train_var_mean"};

inline constexpr std::string_view kVrpFoldStatsHeader = "fold_id\tkind\tsymbol\tname\tvalue";

[[nodiscard]] inline Status validate_fold_stats(const VrpFoldStats &fs) {
  const std::size_t n_sym = fs.symbols.size();
  if (n_sym == 0 || fs.feat_mean.size() != n_sym || fs.feat_sd.size() != n_sym ||
      fs.label_mean.size() != n_sym || fs.label_sd.size() != n_sym) {
    return Err(ErrorCode::InvalidArgument,
               "vrp_fold_stats: per-asset arrays disagree with the symbol count");
  }
  const std::array<double, 5> scalars{fs.baseline_s2, fs.baseline_train_mean_log,
                                      fs.train_var_min, fs.train_var_max, fs.train_var_mean};
  for (const double v : scalars) {
    if (!std::isfinite(v)) {
      return Err(ErrorCode::InvalidArgument, "vrp_fold_stats: non-finite fold scalar");
    }
  }
  for (std::size_t s = 0; s < n_sym; ++s) {
    if (fs.symbols[s].empty() ||
        fs.symbols[s].find_first_of("\t\r\n") != std::string::npos ||
        (s > 0 && !(fs.symbols[s - 1] < fs.symbols[s]))) {
      return Err(ErrorCode::InvalidArgument,
                 "vrp_fold_stats: symbols must be non-empty, tab-free, strictly ascending");
    }
    if (!std::isfinite(fs.label_mean[s]) || !std::isfinite(fs.label_sd[s])) {
      return Err(ErrorCode::InvalidArgument, "vrp_fold_stats: non-finite label stat");
    }
    for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
      if (!std::isfinite(fs.feat_mean[s][f]) || !std::isfinite(fs.feat_sd[s][f])) {
        return Err(ErrorCode::InvalidArgument, "vrp_fold_stats: non-finite feature stat");
      }
    }
  }
  return Ok();
}

} // namespace detail

// Canonical writer for the vrp_fold_stats_v1 sidecar (grammar on the
// VrpFoldStats banner). `Err(InvalidArgument)` on structurally invalid or
// non-finite stats (validated before any I/O); `Err(IoError)` on write
// failure. Byte-deterministic: fixed row order, std::to_chars doubles.
[[nodiscard]] inline Status save_vrp_fold_stats(std::span<const VrpFoldStats> folds,
                                                const std::filesystem::path &path) {
  if (folds.empty()) {
    return Err(ErrorCode::InvalidArgument, "save_vrp_fold_stats: no folds");
  }
  for (std::size_t i = 0; i < folds.size(); ++i) {
    if (const Status st = detail::validate_fold_stats(folds[i]); !st.has_value()) {
      return st;
    }
    if (i > 0 && folds[i - 1].fold_id >= folds[i].fold_id) {
      return Err(ErrorCode::InvalidArgument,
                 "save_vrp_fold_stats: fold ids must be strictly ascending");
    }
  }
  std::string body = "# schema=";
  body += kVrpFoldStatsSchemaValue;
  body += '\n';
  body += detail::kVrpFoldStatsHeader;
  body += '\n';
  const auto row = [&body](std::uint32_t fold_id, std::string_view kind, std::string_view symbol,
                           std::string_view name, std::string_view value) {
    body += std::to_string(fold_id);
    body += '\t';
    body += kind;
    body += '\t';
    body += symbol;
    body += '\t';
    body += name;
    body += '\t';
    body += value;
    body += '\n';
  };
  for (const VrpFoldStats &fs : folds) {
    row(fs.fold_id, "fold", "-", "n_train", std::to_string(fs.n_train));
    row(fs.fold_id, "fold", "-", "n_test", std::to_string(fs.n_test));
    row(fs.fold_id, "fold", "-", "baseline_s2", detail::fmt_double(fs.baseline_s2));
    row(fs.fold_id, "fold", "-", "baseline_train_mean_log",
        detail::fmt_double(fs.baseline_train_mean_log));
    row(fs.fold_id, "fold", "-", "train_var_min", detail::fmt_double(fs.train_var_min));
    row(fs.fold_id, "fold", "-", "train_var_max", detail::fmt_double(fs.train_var_max));
    row(fs.fold_id, "fold", "-", "train_var_mean", detail::fmt_double(fs.train_var_mean));
    for (std::size_t s = 0; s < fs.symbols.size(); ++s) {
      row(fs.fold_id, "asset", fs.symbols[s], "label_mean",
          detail::fmt_double(fs.label_mean[s]));
      row(fs.fold_id, "asset", fs.symbols[s], "label_sd", detail::fmt_double(fs.label_sd[s]));
      for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
        const std::string base = "f" + std::to_string(f);
        row(fs.fold_id, "asset", fs.symbols[s], base + "_mean",
            detail::fmt_double(fs.feat_mean[s][f]));
        row(fs.fold_id, "asset", fs.symbols[s], base + "_sd",
            detail::fmt_double(fs.feat_sd[s][f]));
      }
    }
  }
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) {
    return Err(ErrorCode::IoError,
               "save_vrp_fold_stats: cannot open '" + path.string() + "' for writing");
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    return Err(ErrorCode::IoError, "save_vrp_fold_stats: write failed for '" + path.string() +
                                       "'");
  }
  return Ok();
}

// Strict loader for the vrp_fold_stats_v1 sidecar: accepts exactly the
// canonical layout save_vrp_fold_stats emits and fails closed (ParseError)
// on any deviation -- wrong schema/header, out-of-order fields or symbols,
// missing rows, or an unparseable value. save(load(f)) of a save-produced
// file is byte-identical.
[[nodiscard]] inline Result<std::vector<VrpFoldStats>>
load_vrp_fold_stats(const std::filesystem::path &path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError, "load_vrp_fold_stats: cannot open '" + path.string() + "'");
  }
  struct RawRow {
    std::uint32_t fold_id;
    std::string kind;
    std::string symbol;
    std::string name;
    std::string value;
  };
  std::vector<RawRow> raw;
  bool saw_schema = false;
  bool saw_header = false;
  std::string raw_line;
  std::vector<std::string_view> fields;
  // Bounded by the file's own line count.
  while (std::getline(in, raw_line)) {
    const std::string_view line = detail::rstrip_cr(raw_line);
    if (detail::trim(line).empty()) {
      continue;
    }
    if (!saw_schema) {
      if (line != std::string("# schema=") + std::string(kVrpFoldStatsSchemaValue)) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_fold_stats: missing '# schema=vrp_fold_stats_v1' line");
      }
      saw_schema = true;
      continue;
    }
    if (!saw_header) {
      if (line != detail::kVrpFoldStatsHeader) {
        return Err(ErrorCode::ParseError, "load_vrp_fold_stats: header row mismatch");
      }
      saw_header = true;
      continue;
    }
    detail::split_tabs(line, fields);
    if (fields.size() != 5) {
      return Err(ErrorCode::ParseError, "load_vrp_fold_stats: expected 5 fields, got " +
                                            std::to_string(fields.size()));
    }
    std::int64_t id = 0;
    if (!detail::parse_i64(fields[0], id) || id < 0 ||
        id > std::numeric_limits<std::uint32_t>::max()) {
      return Err(ErrorCode::ParseError, "load_vrp_fold_stats: unparseable fold_id");
    }
    raw.push_back(RawRow{static_cast<std::uint32_t>(id), std::string(fields[1]),
                         std::string(fields[2]), std::string(fields[3]),
                         std::string(fields[4])});
  }
  if (!saw_header || raw.empty()) {
    return Err(ErrorCode::ParseError, "load_vrp_fold_stats: no data rows");
  }

  std::vector<VrpFoldStats> out;
  std::size_t i = 0;
  // Bounded: every block consumes >= 1 raw row.
  while (i < raw.size()) {
    VrpFoldStats fs;
    fs.fold_id = raw[i].fold_id;
    if (!out.empty() && out.back().fold_id >= fs.fold_id) {
      return Err(ErrorCode::ParseError, "load_vrp_fold_stats: fold ids not ascending");
    }
    // The 7 fold-level rows, fixed order.
    for (const std::string_view name : detail::kVrpFoldStatsFoldFields) {
      if (i >= raw.size() || raw[i].fold_id != fs.fold_id || raw[i].kind != "fold" ||
          raw[i].symbol != "-" || raw[i].name != name) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_fold_stats: fold block for id " + std::to_string(fs.fold_id) +
                       " missing field '" + std::string(name) + "'");
      }
      if (name == "n_train" || name == "n_test") {
        std::int64_t v = 0;
        if (!detail::parse_i64(raw[i].value, v) || v < 0) {
          return Err(ErrorCode::ParseError, "load_vrp_fold_stats: bad integer for '" +
                                                std::string(name) + "'");
        }
        (name == "n_train" ? fs.n_train : fs.n_test) = static_cast<std::size_t>(v);
      } else {
        double v = 0.0;
        if (!detail::parse_double(raw[i].value, v) || !std::isfinite(v)) {
          return Err(ErrorCode::ParseError, "load_vrp_fold_stats: bad value for '" +
                                                std::string(name) + "'");
        }
        if (name == "baseline_s2") {
          fs.baseline_s2 = v;
        } else if (name == "baseline_train_mean_log") {
          fs.baseline_train_mean_log = v;
        } else if (name == "train_var_min") {
          fs.train_var_min = v;
        } else if (name == "train_var_max") {
          fs.train_var_max = v;
        } else {
          fs.train_var_mean = v;
        }
      }
      ++i;
    }
    // Asset blocks: 22 rows per symbol, symbols strictly ascending.
    while (i < raw.size() && raw[i].fold_id == fs.fold_id) {
      const std::string symbol = raw[i].symbol;
      if (raw[i].kind != "asset" || symbol.empty() ||
          (!fs.symbols.empty() && !(fs.symbols.back() < symbol))) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_fold_stats: asset rows out of canonical order");
      }
      std::array<double, kVrpFeatureCount> mean{};
      std::array<double, kVrpFeatureCount> sd{};
      double label_mean = 0.0;
      double label_sd = 0.0;
      const auto take = [&](std::string_view name, double &dst) -> bool {
        if (i >= raw.size() || raw[i].fold_id != fs.fold_id || raw[i].kind != "asset" ||
            raw[i].symbol != symbol || raw[i].name != name) {
          return false;
        }
        double v = 0.0;
        if (!detail::parse_double(raw[i].value, v) || !std::isfinite(v)) {
          return false;
        }
        dst = v;
        ++i;
        return true;
      };
      bool ok = take("label_mean", label_mean) && take("label_sd", label_sd);
      for (std::size_t f = 0; ok && f < kVrpFeatureCount; ++f) {
        const std::string base = "f" + std::to_string(f);
        ok = take(base + "_mean", mean[f]) && take(base + "_sd", sd[f]);
      }
      if (!ok) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_fold_stats: incomplete asset block for '" + symbol + "'");
      }
      fs.symbols.push_back(symbol);
      fs.feat_mean.push_back(mean);
      fs.feat_sd.push_back(sd);
      fs.label_mean.push_back(label_mean);
      fs.label_sd.push_back(label_sd);
    }
    if (fs.symbols.empty()) {
      return Err(ErrorCode::ParseError, "load_vrp_fold_stats: fold block without asset rows");
    }
    out.push_back(std::move(fs));
  }
  return Ok(std::move(out));
}

namespace detail {

// Per-asset TRAIN-fold label mean/sd (population) for pred_edge_norm.
struct LabelStats {
  std::vector<double> mean;
  std::vector<double> sd;
};

[[nodiscard]] inline LabelStats compute_label_stats(const VrpPanel &panel,
                                                    std::span<const std::size_t> rows) {
  const std::size_t n_sym = panel.symbols.size();
  LabelStats st;
  st.mean.assign(n_sym, 0.0);
  st.sd.assign(n_sym, 0.0);
  std::vector<double> sum(n_sym, 0.0);
  std::vector<std::size_t> cnt(n_sym, 0);
  for (const std::size_t r : rows) {
    sum[panel.row_symbol[r]] += panel.rows[r].label;
    ++cnt[panel.row_symbol[r]];
  }
  for (std::size_t s = 0; s < n_sym; ++s) {
    if (cnt[s] > 0) {
      st.mean[s] = sum[s] / static_cast<double>(cnt[s]);
    }
  }
  std::vector<double> sq(n_sym, 0.0);
  for (const std::size_t r : rows) {
    const double d = panel.rows[r].label - st.mean[panel.row_symbol[r]];
    sq[panel.row_symbol[r]] += d * d;
  }
  for (std::size_t s = 0; s < n_sym; ++s) {
    if (cnt[s] > 0) {
      st.sd[s] = std::sqrt(sq[s] / static_cast<double>(cnt[s]));
    }
  }
  return st;
}

// The log-HAR baseline's per-fold fitted state.
struct BaselineFit {
  learn::LearnedModel model;   // 3-feature linear (z inputs)
  double train_mean_log{0.0};  // ybar over train rows (kernel fits no intercept)
  double s2{0.0};              // train residual variance in log space
  double train_var_min{0.0};   // insanity clip bounds: train label variance range
  double train_var_max{0.0};
  double train_var_mean{0.0};  // the "mean forecast" benchmark
};

inline constexpr std::array<std::size_t, 3> kBaselineFeatures{0, 1, 2};
inline constexpr std::array<std::size_t, kVrpFeatureCount> kAllFeatures{0, 1, 2, 3, 4,
                                                                        5, 6, 7, 8, 9};

[[nodiscard]] inline BaselineFit fit_vrp_baseline(const VrpPanel &panel,
                                                  const VrpStandardization &stz,
                                                  std::span<const std::size_t> train_rows,
                                                  const VrpTrainConfig &cfg) {
  BaselineFit fit;
  std::vector<double> y_log(train_rows.size(), 0.0);
  fit.train_var_min = std::numeric_limits<double>::infinity();
  fit.train_var_max = -std::numeric_limits<double>::infinity();
  double var_sum = 0.0;
  double log_sum = 0.0;
  for (std::size_t i = 0; i < train_rows.size(); ++i) {
    const double rv = panel.rows[train_rows[i]].rv_fwd_21d;
    const double var = rv * rv;
    y_log[i] = std::log(var);
    log_sum += y_log[i];
    var_sum += var;
    fit.train_var_min = std::min(fit.train_var_min, var);
    fit.train_var_max = std::max(fit.train_var_max, var);
  }
  const auto n = static_cast<double>(train_rows.size());
  fit.train_mean_log = log_sum / n;
  fit.train_var_mean = var_sum / n;

  const learn::FeatureMatrix fm =
      build_feature_matrix(panel, stz, train_rows,
                           std::span<const std::size_t>{kBaselineFeatures},
                           std::span<const double>{y_log});
  const learn::LinearAlphaCfg lin_cfg{
      .en = learn::ElasticNetCfg{.lambda = cfg.en_lambda, .alpha = cfg.en_alpha},
      .use_ridge_baseline = false,
      .cpcv = {},
      .master_seed = cfg.master_seed,
      .horizons = {static_cast<atx::u16>(kVrpHorizonSessions)}};
  fit.model = learn::fit_linear(fm, learn::LatentAugmentation{}, lin_cfg);

  // Train residual variance in log space (for the exp(s2/2) retransform).
  // Predictions omit the mean (no-intercept kernel over zero-mean columns),
  // so the residual is measured against pred + ybar.
  double sq = 0.0;
  std::array<double, 3> base{};
  for (std::size_t i = 0; i < train_rows.size(); ++i) {
    for (std::size_t j = 0; j < kBaselineFeatures.size(); ++j) {
      base[j] = standardized_feature(panel, stz, train_rows[i], kBaselineFeatures[j]);
    }
    const double pred = predict_model(fit.model, std::span<const double>{base});
    const double resid = y_log[i] - (pred + fit.train_mean_log);
    sq += resid * resid;
  }
  fit.s2 = sq / n;
  return fit;
}

// The clipped baseline VARIANCE forecast for one panel row.
[[nodiscard]] inline double baseline_forecast_var(const VrpPanel &panel,
                                                  const VrpStandardization &stz,
                                                  const BaselineFit &fit, std::size_t row) {
  std::array<double, 3> base{};
  for (std::size_t j = 0; j < kBaselineFeatures.size(); ++j) {
    base[j] = standardized_feature(panel, stz, row, kBaselineFeatures[j]);
  }
  const double pred = predict_model(fit.model, std::span<const double>{base});
  return vrp_retransform_clip(pred + fit.train_mean_log, fit.s2, fit.train_var_min,
                              fit.train_var_max);
}

// The GBT's direct label-space prediction for one panel row.
[[nodiscard]] inline double gbt_predict_label(const VrpPanel &panel,
                                              const VrpStandardization &stz,
                                              const learn::LearnedModel &model,
                                              std::size_t row) {
  std::array<double, kVrpFeatureCount> base{};
  for (std::size_t f = 0; f < kVrpFeatureCount; ++f) {
    base[f] = standardized_feature(panel, stz, row, f);
  }
  return predict_model(model, std::span<const double>{base});
}

struct SignalEntry {
  std::size_t panel_row{0};
  double pred_label{0.0};
  double pred_edge_norm{0.0};
  // FINITE by construction (round-2 F2): raw f9 when finite, else the
  // scoring fold's per-asset train-fold f9 mean -- the frozen vrp_signal_v1
  // loader fail-closes on a non-finite vov_63d, so the writer never emits
  // one.
  double vov_63d{0.0};
};

// The vov_63d a signal row carries: raw panel f9 when finite, else the
// per-asset train-fold mean under the fold that scored the row (z = 0
// imputation mapped back to raw space). Emitted values are ALWAYS >= 0
// (round-2 review minor): load_vrp_panel rejects finite negative f9 at the
// boundary, so both the raw pass-through and every accumulated mean are
// >= 0. DOCUMENTED FALLBACK: an asset with ZERO finite f9 observations in
// the scoring fold's train window imputes the default-constructed mean 0.0
// -- finite and >= 0, accepted by the frozen vrp_signal_v1 loader, and
// floored by vov_floor at sizing downstream.
[[nodiscard]] inline double signal_vov(const VrpPanel &panel, const VrpStandardization &stz,
                                       std::size_t row) {
  const double raw = panel.rows[row].f[9];
  if (std::isfinite(raw)) {
    return raw;
  }
  return stz.mean[panel.row_symbol[row]][9];
}

[[nodiscard]] inline Status write_signal_file(const VrpPanel &panel,
                                              std::span<const SignalEntry> entries,
                                              const std::filesystem::path &path) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) {
    return Err(ErrorCode::IoError,
               "write_signal_file: cannot open '" + path.string() + "' for writing");
  }
  std::string body = "# schema=vrp_signal_v1\n";
  body += "symbol\tdate\tpred_label\tpred_edge_norm\tvov_63d\n";
  for (const SignalEntry &e : entries) {
    const VrpPanelRow &row = panel.rows[e.panel_row];
    body += row.symbol;
    body += '\t';
    body += row.date;
    body += '\t';
    body += fmt_double(e.pred_label);
    body += '\t';
    body += fmt_double(e.pred_edge_norm);
    body += '\t';
    body += fmt_double(e.vov_63d); // finite: raw f9 or train-fold imputation
    body += '\n';
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    return Err(ErrorCode::IoError, "write_signal_file: write failed for '" + path.string() + "'");
  }
  return Ok();
}

[[nodiscard]] inline Status write_metrics_file(std::span<const VrpFoldMetrics> folds,
                                               const VrpObservations &observations,
                                               const std::filesystem::path &path) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) {
    return Err(ErrorCode::IoError,
               "write_metrics_file: cannot open '" + path.string() + "' for writing");
  }
  std::string body = "# schema=vrp_train_metrics_v1\n";
  // F1 rejection accounting as meta lines (columns below stay round-1 shaped).
  body += "# n_labeled_rows=" + std::to_string(observations.n_labeled_rows) + "\n";
  body += "# n_rows_rejected_no_t21=" + std::to_string(observations.n_rows_rejected_no_t21) +
          "\n";
  body += "# n_symbols_fully_rejected=" +
          std::to_string(observations.n_symbols_fully_rejected) + "\n";
  // Per-fold accounting meta lines (round-2 review majors 2 + 3): the
  // purge/embargo train-row losses and the GBT QLIKE-path insanity-clip
  // count + post-clip extrema, via the same `# key=value` mechanism as the
  // F1 counters. Additive only -- the tabular columns below stay round-1
  // shaped.
  for (const VrpFoldMetrics &m : folds) {
    const std::string p = "# fold_" + std::to_string(m.fold_id) + "_";
    body += p + "n_train_purged=" + std::to_string(m.n_train_purged) + "\n";
    body += p + "n_train_embargoed=" + std::to_string(m.n_train_embargoed) + "\n";
    body += p + "n_gbt_forecast_clipped=" + std::to_string(m.n_gbt_forecast_clipped) + "\n";
    body += p + "gbt_test_forecast_min=" + fmt_double(m.gbt_test_forecast_min) + "\n";
    body += p + "gbt_test_forecast_max=" + fmt_double(m.gbt_test_forecast_max) + "\n";
  }
  body += "fold_id\tmodel\tn_train\tn_test\tqlike\tspearman_ic\ttrain_var_min\ttrain_var_max\n";
  const auto row = [&](const VrpFoldMetrics &m, std::string_view model, double qlike,
                       double ic) {
    body += std::to_string(m.fold_id);
    body += '\t';
    body += model;
    body += '\t';
    body += std::to_string(m.n_train);
    body += '\t';
    body += std::to_string(m.n_test);
    body += '\t';
    body += fmt_double(qlike);
    body += '\t';
    body += fmt_double(ic);
    body += '\t';
    body += fmt_double(m.train_var_min);
    body += '\t';
    body += fmt_double(m.train_var_max);
    body += '\n';
  };
  for (const VrpFoldMetrics &m : folds) {
    row(m, "baseline_log_har", m.qlike_baseline, m.ic_baseline);
    row(m, "gbt", m.qlike_gbt, m.ic_gbt);
    row(m, "mean_forecast", m.qlike_mean_forecast,
        std::numeric_limits<double>::quiet_NaN());
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    return Err(ErrorCode::IoError,
               "write_metrics_file: write failed for '" + path.string() + "'");
  }
  return Ok();
}

} // namespace detail

// ── The trainer ─────────────────────────────────────────────────────────────

[[nodiscard]] inline Result<VrpTrainReport> run_vrp_train(const VrpTrainConfig &cfg) {
  auto panel_r = load_vrp_panel(cfg.panel_path);
  if (!panel_r.has_value()) {
    return Err(panel_r.error());
  }
  VrpTrainReport report;
  report.panel = std::move(*panel_r);

  auto obs_r = build_vrp_observations(report.panel);
  if (!obs_r.has_value()) {
    return Err(obs_r.error());
  }
  report.observations = std::move(*obs_r);

  VrpWalkForwardCfg walk = cfg.walk;
  if (cfg.walk_auto) {
    // Distinct decision-timestamp groups among the USABLE labeled rows --
    // obs is canonical (decision_ts_ns, uid) ascending, so a linear scan
    // counts groups exactly.
    std::size_t n_groups = 0;
    std::int64_t prev_ts = std::numeric_limits<std::int64_t>::min();
    bool first = true;
    for (const ResearchObservation &ob : report.observations.obs) {
      if (first || ob.decision_ts_ns != prev_ts) {
        ++n_groups;
        first = false;
      }
      prev_ts = ob.decision_ts_ns;
    }
    walk = derive_vrp_walk_forward(n_groups);
  }
  auto plan_r = make_vrp_plan(report.observations, walk);
  if (!plan_r.has_value()) {
    return Err(plan_r.error());
  }
  report.plan = std::move(*plan_r);

  const learn::GbtCfg gbt_cfg{.master_seed = cfg.master_seed,
                              .cpcv = {},
                              .horizons = {static_cast<atx::u16>(kVrpHorizonSessions)}};

  std::vector<detail::SignalEntry> signal_entries;
  // Final-fold artifacts (largest anchored train window): serialize these,
  // and score the NaN-label tail rows with them.
  std::optional<VrpStandardization> last_stz;
  std::optional<detail::BaselineFit> last_baseline;
  std::optional<learn::LearnedModel> last_gbt;
  std::optional<detail::LabelStats> last_label_stats;

  for (const ResearchValidationFold &fold : report.plan.folds) {
    VrpFoldMetrics m;
    m.fold_id = fold.id;
    m.train_rows.reserve(fold.train_indices.size());
    for (const std::size_t oi : fold.train_indices) {
      m.train_rows.push_back(report.observations.row_of[oi]);
    }
    m.test_rows.reserve(fold.test_indices.size());
    for (const std::size_t oi : fold.test_indices) {
      m.test_rows.push_back(report.observations.row_of[oi]);
    }
    m.n_train = m.train_rows.size();
    m.n_test = m.test_rows.size();
    m.n_train_purged = fold.purged_indices.size();
    m.n_train_embargoed = fold.embargoed_indices.size();

    const VrpStandardization stz =
        compute_asset_standardization(report.panel, std::span<const std::size_t>{m.train_rows});
    const detail::LabelStats label_stats =
        detail::compute_label_stats(report.panel, std::span<const std::size_t>{m.train_rows});

    const detail::BaselineFit baseline = detail::fit_vrp_baseline(
        report.panel, stz, std::span<const std::size_t>{m.train_rows}, cfg);
    m.train_var_min = baseline.train_var_min;
    m.train_var_max = baseline.train_var_max;

    std::vector<double> gbt_labels(m.train_rows.size(), 0.0);
    for (std::size_t i = 0; i < m.train_rows.size(); ++i) {
      gbt_labels[i] = report.panel.rows[m.train_rows[i]].label;
    }
    const learn::FeatureMatrix fm10 = detail::build_feature_matrix(
        report.panel, stz, std::span<const std::size_t>{m.train_rows},
        std::span<const std::size_t>{detail::kAllFeatures}, std::span<const double>{gbt_labels});
    const learn::LearnedModel gbt = learn::fit_gbt(fm10, learn::LatentAugmentation{}, gbt_cfg);

    double q_base = 0.0;
    double q_gbt = 0.0;
    double q_mean = 0.0;
    double base_forecast_max = -std::numeric_limits<double>::infinity();
    double gbt_forecast_min = std::numeric_limits<double>::infinity();
    double gbt_forecast_max = -std::numeric_limits<double>::infinity();
    std::vector<std::int64_t> test_ts;
    std::vector<double> pred_base;
    std::vector<double> pred_gbt;
    std::vector<double> realized;
    test_ts.reserve(m.test_rows.size());
    for (const std::size_t r : m.test_rows) {
      const VrpPanelRow &row = report.panel.rows[r];
      const double proxy_var = row.rv_fwd_21d * row.rv_fwd_21d;
      const double iv_var = row.iv_fair_21d * row.iv_fair_21d;

      const double f_base = detail::baseline_forecast_var(report.panel, stz, baseline, r);
      base_forecast_max = std::max(base_forecast_max, f_base);
      const double label_base = (f_base - iv_var) * kVrpHorizonYears;

      const double label_gbt = detail::gbt_predict_label(report.panel, stz, gbt, r);
      // The GBT's implied variance forecast goes through the SAME insanity
      // clip as the baseline (train-window label variance range, digest
      // [20]): a signed-label prediction can imply a variance <= 0 on thin
      // folds, and QLIKE against a near-zero floor explodes (round-1 SP100:
      // 1e6..3e7). train_var_min > 0 (labeled rows carry rv_fwd > 0), so the
      // clipped forecast always satisfies QLIKE's F > 0 precondition.
      // pred_label below stays the RAW prediction -- rank info untouched.
      const double f_gbt_raw = label_gbt / kVrpHorizonYears + iv_var;
      const double f_gbt =
          std::clamp(f_gbt_raw, baseline.train_var_min, baseline.train_var_max);
      if (f_gbt != f_gbt_raw) {
        ++m.n_gbt_forecast_clipped;
      }
      gbt_forecast_min = std::min(gbt_forecast_min, f_gbt);
      gbt_forecast_max = std::max(gbt_forecast_max, f_gbt);

      q_base += vrp_qlike(f_base, proxy_var);
      q_gbt += vrp_qlike(f_gbt, proxy_var);
      q_mean += vrp_qlike(baseline.train_var_mean, proxy_var);

      test_ts.push_back(row.entry_ts_ns);
      pred_base.push_back(label_base);
      pred_gbt.push_back(label_gbt);
      realized.push_back(row.label);

      const std::size_t sym = report.panel.row_symbol[r];
      const double sd = label_stats.sd[sym];
      signal_entries.push_back(detail::SignalEntry{
          .panel_row = r,
          .pred_label = label_gbt,
          .pred_edge_norm = sd == 0.0 ? 0.0 : (label_gbt - label_stats.mean[sym]) / sd,
          .vov_63d = detail::signal_vov(report.panel, stz, r)});
    }
    const auto n_test = static_cast<double>(m.test_rows.size());
    m.qlike_baseline = q_base / n_test;
    m.qlike_gbt = q_gbt / n_test;
    m.qlike_mean_forecast = q_mean / n_test;
    m.baseline_test_forecast_max = base_forecast_max;
    m.gbt_test_forecast_min = gbt_forecast_min;
    m.gbt_test_forecast_max = gbt_forecast_max;
    m.ic_baseline = detail::mean_per_date_spearman(std::span<const std::int64_t>{test_ts},
                                                   std::span<const double>{pred_base},
                                                   std::span<const double>{realized});
    m.ic_gbt = detail::mean_per_date_spearman(std::span<const std::int64_t>{test_ts},
                                              std::span<const double>{pred_gbt},
                                              std::span<const double>{realized});

    // The fold's live-path sidecar block: everything a consumer needs to
    // score raw panel rows against this fold's serialized models.
    VrpFoldStats fs;
    fs.fold_id = m.fold_id;
    fs.n_train = m.n_train;
    fs.n_test = m.n_test;
    fs.baseline_s2 = baseline.s2;
    fs.baseline_train_mean_log = baseline.train_mean_log;
    fs.train_var_min = baseline.train_var_min;
    fs.train_var_max = baseline.train_var_max;
    fs.train_var_mean = baseline.train_var_mean;
    fs.symbols = report.panel.symbols;
    fs.feat_mean = stz.mean;
    fs.feat_sd = stz.sd;
    fs.label_mean = label_stats.mean;
    fs.label_sd = label_stats.sd;
    report.fold_stats.push_back(std::move(fs));

    last_stz = stz;
    last_baseline = baseline;
    last_gbt = gbt;
    last_label_stats = label_stats;
    report.folds.push_back(std::move(m));
  }

  if (!last_gbt.has_value() || !last_baseline.has_value()) {
    return Err(ErrorCode::InvalidArgument, "run_vrp_train: walk-forward produced no folds");
  }

  // NaN-label tail rows: predict-time rows, scored by the FINAL fold's
  // models (their dates lie after the final train window, so this is a
  // forward application, not a re-scoring of trained rows).
  for (std::size_t r = 0; r < report.panel.rows.size(); ++r) {
    const VrpPanelRow &row = report.panel.rows[r];
    if (is_labeled_row(row)) {
      continue;
    }
    const double label_gbt = detail::gbt_predict_label(report.panel, *last_stz, *last_gbt, r);
    const std::size_t sym = report.panel.row_symbol[r];
    const double sd = last_label_stats->sd[sym];
    signal_entries.push_back(detail::SignalEntry{
        .panel_row = r,
        .pred_label = label_gbt,
        .pred_edge_norm = sd == 0.0 ? 0.0 : (label_gbt - last_label_stats->mean[sym]) / sd,
        .vov_63d = detail::signal_vov(report.panel, *last_stz, r)});
  }
  std::sort(signal_entries.begin(), signal_entries.end(),
            [](const detail::SignalEntry &a, const detail::SignalEntry &b) {
              return a.panel_row < b.panel_row;
            });

  // Outputs.
  const std::filesystem::path out_dir{cfg.out_dir};
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    return Err(ErrorCode::IoError,
               "run_vrp_train: cannot create out dir '" + out_dir.string() + "'");
  }
  report.signal_path = out_dir / "vrp_signal.tsv";
  report.metrics_path = out_dir / "vrp_metrics.tsv";
  report.gbt_model_path = out_dir / "vrp_gbt_model.tsv";
  report.baseline_model_path = out_dir / "vrp_baseline_model.tsv";
  report.fold_stats_path = out_dir / "vrp_fold_stats.tsv";

  Status st = detail::write_signal_file(report.panel,
                                        std::span<const detail::SignalEntry>{signal_entries},
                                        report.signal_path);
  if (!st.has_value()) {
    return Err(st.error());
  }
  st = detail::write_metrics_file(std::span<const VrpFoldMetrics>{report.folds},
                                  report.observations, report.metrics_path);
  if (!st.has_value()) {
    return Err(st.error());
  }
  st = save_vrp_fold_stats(std::span<const VrpFoldStats>{report.fold_stats},
                           report.fold_stats_path);
  if (!st.has_value()) {
    return Err(st.error());
  }

  auto gbt_data = gbt_model_to_fair_vol_data(*last_gbt, kVrpFeatureSchemaV1);
  if (!gbt_data.has_value()) {
    return Err(gbt_data.error());
  }
  st = save_gbt_fair_vol_model_data(*gbt_data, report.gbt_model_path.string());
  if (!st.has_value()) {
    return Err(st.error());
  }

  auto lin_params = linear_model_to_fair_vol_params(
      last_baseline->model, last_baseline->train_mean_log, kVrpFeatureSchemaV1,
      std::span<const std::size_t>{detail::kBaselineFeatures});
  if (!lin_params.has_value()) {
    return Err(lin_params.error());
  }
  st = save_linear_fair_vol_params(*lin_params, report.baseline_model_path.string());
  if (!st.has_value()) {
    return Err(st.error());
  }

  return Ok(std::move(report));
}

} // namespace atx::vol::vrp
