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
//      (label interval [t, t+21 label bars]; label_end is the EMITTED-AXIS
//      end -- the symbol's own 21st emitted successor row's timestamp, a
//      provable UPPER bound on the true bar-axis t+21 end, because emitted
//      rows are a subset of the symbol's label-generation bars; the fix-2
//      review demonstrated that a pooled-axis t+21 end UNDERSTATES bar-holey
//      symbols' windows and admits leaking train rows) and build a purged +
//      embargoed anchored walk-forward via make_purged_walk_forward_plan
//      (research_validation.hpp). Trainability is recovered by a SPAN CAP
//      instead of a shorter end: a row whose emitted-axis window spans more
//      than cfg.max_label_span_sessions POOLED sessions (default 42 = 2x
//      the horizon) is REJECTED AND COUNTED, so one sparse symbol can no
//      longer stretch the global-max embargo to months and purge the whole
//      corpus. embargo_ns is derived as the MAXIMUM ADMITTED wall-clock
//      span -- >= 21 sessions, <= the cap (plus calendar gaps). The t+21
//      same-symbol-row invariant is enforced PER ROW (round-2 F1): a
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
//   3b. OPTIONAL round-3 level recalibration (--recalibrate isotonic,
//      default OFF; research-vrp-costs digest Q4 [18][19]): per fold, the
//      trailing --recalib-window ADMITTED train sessions (capped at half
//      the fold's train sessions) become a temporal-holdout calibration
//      window. A CALIBRATION GBT fit on the earlier train rows produces
//      genuinely out-of-sample forecasts on the window; PAVA isotonic
//      regression maps those forecasts onto realized labels, and the
//      monotone map is applied to the PRODUCTION model's raw test
//      forecasts. Every fit row is an admitted train row, so the plan's
//      own purge already bounds its label window at the fold's test start
//      -- the fit uses only data strictly before the test window and the
//      fold plan itself is untouched (the leak adjudicator stays PASS).
//      Monotone => rank-preserving by construction; QLIKE + Mincer-
//      Zarnowitz slope/intercept + rank IC are reported before AND after
//      per fold (QLIKE alone can favor positively biased forecasts,
//      digest [15]). Behind the flag the recalibrated values flow through
//      the EXISTING pred_label/pred_edge_norm columns (schema unchanged).
//      A second flag (--retransform smearing, default jensen) swaps the
//      baseline's exp(s2/2) lognormal retransform for the Duan smearing
//      factor mean(exp(resid)) (digest [16][17]); trainer-side only, the
//      sidecar contract stays jensen-based.
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
#include <functional>
#include <limits>
#include <map>
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
// vrp_panel_v2 is a strict PREFIX EXTENSION of v1: the same 18 columns then
// `iv_atmf_21d`, the ATM-FORWARD implied vol a straddle is actually struck and
// marked at (`iv_fair_21d` is the variance-swap fair strike K_var, ~4.24 vol
// points above it on 99.5% of rows). Round 5's vol-change target is built on
// the ATMF leg because that is the leg a vega position is marked on; a v1 panel
// still loads and carries a NaN `iv_atmf_21d`, which makes every iv-change axis
// undefined rather than silently substituting the strip strike.
inline constexpr std::string_view kVrpPanelSchemaValueV2 = "vrp_panel_v2";
inline constexpr std::size_t kVrpPanelColumnCountV2 = 19;
inline constexpr std::string_view kVrpPanelColumnIvAtmf = "iv_atmf_21d";
inline constexpr std::size_t kVrpHorizonSessions = 21;
inline constexpr double kVrpHorizonYears = 21.0 / 252.0;
// Default --max-label-span: reject-and-count rows whose emitted-axis label
// window spans more than this many POOLED sessions (2x the 21-session
// horizon). SP100 survey (fix round 2): spans p50=22 p95=35 p99=64 max=169;
// 42 keeps 97.5% of the coverage-complete rows while capping the DUK-class
// 100+ session spans that poisoned the global-max embargo.
inline constexpr std::size_t kVrpDefaultMaxLabelSpanSessions = 2 * kVrpHorizonSessions;

// Round-3 forecast-level recalibration (research-vrp-costs digest Q4).
// Off keeps every signal/model/sidecar byte identical to the fix-2 trainer.
enum class VrpRecalMode : std::uint8_t { Off, Isotonic };
// Baseline log-target retransformation: Jensen = exp(s2/2) (the round-1
// path), Smearing = Duan's nonparametric mean(exp(resid)) factor.
enum class VrpRetransformMode : std::uint8_t { Jensen, Smearing };
// Default --recalib-window: trailing admitted-train calibration sessions
// (digest suggestion 60-90; capped per fold at half the train sessions).
inline constexpr std::size_t kVrpDefaultRecalibWindowSessions = 63;

// ROUND-4 F1: the basis pred_edge_norm standardizes on. THIS IS THE HIGHEST-
// MEASURED-VALUE SWITCH IN THE SPRINT -- audit-gross-negative S1/S4 replayed
// the identical signal file, dates and 21d horizon through an equal-weighted
// decile long/short book with all sizing stripped out and measured
//   PerSymbol    (round-1..3 default): -1.63 vol pts/cycle, 29% of phases > 0
//   CrossSection (round-4 default):    +1.74 vol pts/cycle, 76% of phases > 0
// a ~3.4 vol pt/cycle (~$210k/cycle) swing from ONE column. The per-symbol
// z-score (label_gbt - label_mean[sym]) / label_sd[sym] demeans away the
// persistent cross-sectional VRP -- the exact quantity being harvested -- and
// then divides by a label_sd spanning three orders of magnitude (long-decile
// mean 0.0039 against short-decile 0.4551), which SWAPS which names land on
// which side. CrossSection standardizes WITHIN each date across symbols, an
// order-preserving affine map of pred_label, so the book ranks on the axis the
// IC is measured on. PerSymbol stays reachable so the round-3 comparison (and
// byte-identical round-3 artifacts) remain reproducible.
enum class VrpEdgeNormMode : std::uint8_t { CrossSection, PerSymbol };

// ROUND-4 F4: features are read from the row's own session (lag 0, the
// round-1..3 behaviour) or from its k-th same-symbol predecessor. Lag 2 puts
// the whole feature set at t-2 so a stale quote cannot manufacture IC.
inline constexpr std::size_t kVrpDefaultFeatureLag = 0;
// Bounded so a typo cannot silently blank the corpus (every lag row without a
// k-th predecessor loses its features).
inline constexpr std::size_t kVrpMaxFeatureLag = kVrpHorizonSessions;

// Named feature slots this file reads directly out of VrpPanelRow::f.
// f2_log_rv21 = ln(rv_trail_21d^2); f5_hv_iv_gap = ln(rv_trail_21d/iv_fair_21d).
inline constexpr std::size_t kVrpFeatLogRv21 = 2;
// f4_term_slope = iv_fair_63d - iv_fair_21d, IDENTICAL to the quantity the
// Vasquez benchmark wants (vrp_panel.hpp:637). ROUND 6 reads the BENCHMARK out
// of the feature vector rather than off the target-side row, which is what
// makes it move under --feature-lag; see the bench_term_slope push_back.
inline constexpr std::size_t kVrpFeatTermSlope = 4;
inline constexpr std::size_t kVrpFeatHvIvGap = 5;

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
  // vrp_panel_v2 only; NaN on a v1 panel. TARGET-SIDE: never feature-lagged.
  double iv_atmf_21d{std::numeric_limits<double>::quiet_NaN()};
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
//
// NaN is canonicalized to the single spelling "nan" BEFORE to_chars sees it.
// The UCRT/MSVC to_chars distinguishes NaN payloads and prints the x87
// "indefinite" quiet NaN (sign bit set, zero payload -- exactly what 0.0/0.0
// produces) as "-nan(ind)", and a signalling one as "nan(snan)". Which of
// those a degenerate statistic yields depends on the instruction that made it,
// so leaving it through would make the artifacts non-reproducible across
// compilers for a value that carries no information beyond "undefined".
// Verified: no round-3 artifact contains a "nan(" spelling, so this
// canonicalization leaves every anchored byte unchanged.
[[nodiscard]] inline std::string fmt_double(double v) {
  if (std::isnan(v)) {
    return "nan";
  }
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

// v2 == v1's header then one appended column. Spelled as an extension of the
// v1 string rather than a second literal so the two can never drift apart.
[[nodiscard]] inline std::string expected_panel_header_v2() {
  return expected_panel_header() + '\t' + std::string{kVrpPanelColumnIvAtmf};
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
  bool panel_is_v2 = false;
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
      if (!schema_value.has_value() ||
          (*schema_value != kVrpPanelSchemaValue && *schema_value != kVrpPanelSchemaValueV2)) {
        return Err(ErrorCode::ParseError, "load_vrp_panel: schema comment mismatch (expected "
                                          "'# schema=vrp_panel_v1' or '# schema=vrp_panel_v2')");
      }
      panel_is_v2 = (*schema_value == kVrpPanelSchemaValueV2);
      if (!horizon_days.has_value() ||
          *horizon_days != static_cast<std::int64_t>(kVrpHorizonSessions)) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_panel: horizon_days comment mismatch (expected 21)");
      }
      if (line != (panel_is_v2 ? detail::expected_panel_header_v2()
                               : detail::expected_panel_header())) {
        return Err(ErrorCode::ParseError,
                   "load_vrp_panel: header row does not match the frozen panel column order");
      }
      header_seen = true;
      continue;
    }
    const std::size_t want_fields = panel_is_v2 ? kVrpPanelColumnCountV2 : kVrpPanelColumnCount;
    detail::split_tabs(line, fields);
    if (fields.size() != want_fields) {
      return Err(ErrorCode::ParseError,
                 "load_vrp_panel: expected " + std::to_string(want_fields) +
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
    if (ok && panel_is_v2) {
      ok = parse_field(kVrpPanelColumnCount, row.iv_atmf_21d);
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

// ── Feature lagging (round-4 F4) ────────────────────────────────────────────

// Shift every row's FEATURE VECTOR to that row's `lag`-th same-symbol
// predecessor, i.e. score session t on the information set of session t-lag.
// ROUND4-PLAN Phase 2: at lag 0 a feature derived from the same session's
// quote can manufacture IC out of a stale quote that already knows part of the
// outcome; lag 2 removes that channel. Applied ONCE, immediately after the
// panel parse, so every downstream consumer -- per-asset standardization, the
// baseline, the GBT, signal_vov and the hv_iv_gap benchmark -- faces the same
// shifted information set with no further plumbing.
//
// The TARGET side (label, rv_fwd_21d, iv_fair_21d, spot, entry_ts_ns) is NEVER
// shifted: iv_fair_21d at t is already known at t, and shifting the label would
// silently change the forecasting problem.
//
// A row whose symbol has no `lag`-th predecessor (the per-symbol warmup) gets
// an all-NaN feature vector and is COUNTED, not dropped: NaN features already
// route through the documented per-asset z = 0 imputation, and the returned
// count is published so the attrition is never invisible. lag 0 is the
// identity and returns 0. Deterministic: rows arrive in canonical
// (entry_ts_ns, symbol) order, so each symbol's row list is ascending in time.
[[nodiscard]] inline std::size_t apply_vrp_feature_lag(VrpPanel &panel, std::size_t lag) {
  if (lag == 0) {
    return 0;
  }
  std::vector<std::vector<std::size_t>> by_symbol(panel.symbols.size());
  for (std::size_t r = 0; r < panel.rows.size(); ++r) {
    by_symbol[panel.row_symbol[r]].push_back(r);
  }
  std::vector<std::array<double, kVrpFeatureCount>> src(panel.rows.size());
  for (std::size_t r = 0; r < panel.rows.size(); ++r) {
    src[r] = panel.rows[r].f;
  }
  std::array<double, kVrpFeatureCount> blank{};
  blank.fill(std::numeric_limits<double>::quiet_NaN());
  std::size_t n_unavailable = 0;
  for (const std::vector<std::size_t> &rows : by_symbol) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (i < lag) {
        panel.rows[rows[i]].f = blank;
        ++n_unavailable;
      } else {
        panel.rows[rows[i]].f = src[rows[i - lag]];
      }
    }
  }
  return n_unavailable;
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
  // Rejection accounting (surfaced in the metrics meta lines + CLI stderr):
  std::size_t n_labeled_rows{0};           // labeled panel rows seen
  std::size_t n_rows_rejected_no_t21{0};   // labeled rows lacking a t+21 row
  std::size_t n_rows_rejected_span_cap{0}; // rows whose label span exceeds the cap
  std::size_t n_symbols_fully_rejected{0}; // symbols whose EVERY labeled row was rejected
};

// One ResearchObservation per USABLE labeled panel row. decision =
// entry_ts_ns; label_end is the EMITTED-AXIS end: the symbol's own 21st
// emitted successor row's timestamp. The panel builder labels over 21 bars
// of the SYMBOL'S OWN bar axis, and emitted rows are a SUBSET of those bars
// (an emitted row requires the bar to exist plus a usable surface), so the
// 21st emitted successor is the k-th bar for some k >= 21 and this end is
// >= the true bar-axis t+21 end BY CONSTRUCTION -- it may over-purge, it
// can never understate. The fix-2 review demonstrated the alternative
// (pooled-axis t+21) UNDERSTATES the window of any symbol with in-window
// bar holes (77 of 102 SP100 names) and admitted train rows whose true
// label windows crossed their fold's test start -- reverted here.
//
// Trainability under the conservative end comes from the SPAN CAP: a row
// whose emitted-axis window spans more than max_label_span_sessions POOLED
// sessions is rejected AND COUNTED (n_rows_rejected_span_cap), so a sparse
// symbol's stretched windows can no longer poison make_vrp_plan's global-
// max embargo (round-1 SP100: one 250-day span embargoed ~70% of the
// corpus and zeroed every fold). Precondition: max_label_span_sessions >=
// kVrpHorizonSessions (every window spans >= the horizon; fail closed).
//
// The t+21 invariant stays PER ROW (F1): a labeled row whose symbol lacks
// an emitted row 21 positions later (interior surface holes inside the
// final horizon) loses only itself -- rejected and counted, with fully-
// rejected symbols counted separately -- never the symbol or the run. The
// mirrored check fails CLOSED: an unlabeled (NaN-label) row that DOES have
// a t+21 successor is not a tail row, and silently scoring it with the
// final fold's models would hand it a hindsight prediction (review minor,
// round 1). uid = symbol index + 1 (uid 0 is rejected upstream).
[[nodiscard]] inline Result<VrpObservations> build_vrp_observations(
    const VrpPanel &panel,
    std::size_t max_label_span_sessions = kVrpDefaultMaxLabelSpanSessions) {
  if (max_label_span_sessions < kVrpHorizonSessions) {
    return Err(ErrorCode::InvalidArgument,
               "build_vrp_observations: max_label_span_sessions " +
                   std::to_string(max_label_span_sessions) + " is below the " +
                   std::to_string(kVrpHorizonSessions) +
                   "-session horizon (every label window spans >= the horizon)");
  }
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
    // The row's 21st same-symbol emitted successor: the label_end source.
    const std::size_t t21_row = sym_rows[s][p + kVrpHorizonSessions];
    // Span cap: the window's width in POOLED sessions. Within-symbol rows
    // occupy strictly increasing pooled session indices, so the span is
    // always >= kVrpHorizonSessions; interior emitted gaps (surface holes
    // or partition absence) stretch it. Rows past the cap lose only
    // themselves -- rejected and counted, never the symbol or the run.
    const std::size_t span_sessions = row_session[t21_row] - row_session[r];
    if (span_sessions > max_label_span_sessions) {
      ++out.n_rows_rejected_span_cap;
      ++sym_rejected[s];
      continue;
    }
    ResearchObservation ob;
    ob.uid = static_cast<std::uint32_t>(s + 1);
    ob.observed_ts_ns = row.entry_ts_ns;
    ob.available_ts_ns = row.entry_ts_ns;
    ob.decision_ts_ns = row.entry_ts_ns;
    ob.execution_ts_ns = row.entry_ts_ns + 1; // strictly after the decision
    // EMITTED-AXIS end: >= the true bar-axis t+21 end (see the contract
    // comment above) -- conservative by construction, bounded by the cap.
    ob.label_end_ts_ns = panel.rows[t21_row].entry_ts_ns;
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
                   std::to_string(out.n_rows_rejected_no_t21) + ", rejected_span_cap=" +
                   std::to_string(out.n_rows_rejected_span_cap) + ")");
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
// embargo_ns = max ADMITTED [t, label_end] wall-clock span, so the embargo
// always covers >= 21 sessions regardless of weekends/holidays. The span
// cap in build_vrp_observations bounds every admitted span at
// max_label_span_sessions pooled sessions (plus calendar gaps), so a
// sparse symbol can no longer stretch the embargo to months.
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

// Duan (1983) smearing factor: mean(exp(residual)) over the train-fold log
// residuals -- the nonparametric retransformation alternative to exp(s2/2),
// safer when residuals are non-Gaussian (digest [16][17]). NaN on empty.
[[nodiscard]] inline double vrp_smearing_factor(std::span<const double> residuals) noexcept {
  if (residuals.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double sum = 0.0;
  for (const double r : residuals) {
    sum += std::exp(r);
  }
  return sum / static_cast<double>(residuals.size());
}

// Smearing retransform + the SAME insanity clip as the Jensen path.
[[nodiscard]] inline double vrp_smearing_retransform_clip(double log_var_forecast,
                                                          double smear_factor,
                                                          double train_var_min,
                                                          double train_var_max) noexcept {
  return std::clamp(std::exp(log_var_forecast) * smear_factor, train_var_min, train_var_max);
}

// ── Isotonic recalibration (PAVA; digest [18][19]) ──────────────────────────

// Deterministic pool-adjacent-violators isotonic fit of a non-decreasing map
// x -> y, evaluated with piecewise-linear interpolation between the fitted
// points and CONSTANT extrapolation outside the fitted x range (recalibrated
// levels stay bounded by observed calibration targets). The evaluated map is
// monotone non-decreasing by construction: q1 <= q2 => eval(q1) <= eval(q2)
// and equal inputs map to equal outputs -- applying it to a forecast can
// repair the LEVEL but can never reorder the ranks (rank statistics are
// invariant under non-decreasing maps, up to pooled-block ties).
struct VrpIsotonicMap {
  std::vector<double> x; // strictly increasing fitted abscissae
  std::vector<double> y; // non-decreasing fitted values (parallel to x)
};

// PAVA over the finite (x, y) pairs; non-finite pairs carry no level
// information and are excluded. Err(InvalidArgument) on size mismatch or
// when no finite pair survives. Deterministic and RNG-free: pairs sort by
// (x, y), exact x-ties pool to their mean first, then adjacent violators
// merge on weighted means.
[[nodiscard]] inline Result<VrpIsotonicMap> fit_vrp_isotonic(std::vector<double> x,
                                                             std::vector<double> y) {
  if (x.size() != y.size()) {
    return Err(ErrorCode::InvalidArgument, "fit_vrp_isotonic: x/y size mismatch");
  }
  std::vector<std::pair<double, double>> pts;
  pts.reserve(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::isfinite(x[i]) && std::isfinite(y[i])) {
      pts.emplace_back(x[i], y[i]);
    }
  }
  if (pts.empty()) {
    return Err(ErrorCode::InvalidArgument, "fit_vrp_isotonic: no finite (x, y) pairs");
  }
  std::sort(pts.begin(), pts.end());
  struct Block {
    double sum_wy{0.0};
    double w{0.0};
    double x_first{0.0};
    double x_last{0.0};
    [[nodiscard]] double value() const noexcept { return sum_wy / w; }
  };
  std::vector<Block> blocks;
  blocks.reserve(pts.size());
  std::size_t i = 0;
  // Bounded: each outer iteration consumes >= 1 point; merges only shrink
  // the block stack.
  while (i < pts.size()) {
    // Pool exact x-ties into one weighted point.
    std::size_t j = i;
    double sum = 0.0;
    while (j < pts.size() && pts[j].first == pts[i].first) {
      sum += pts[j].second;
      ++j;
    }
    blocks.push_back(
        Block{sum, static_cast<double>(j - i), pts[i].first, pts[i].first});
    // PAVA: merge while the previous block violates non-decreasing order.
    while (blocks.size() >= 2 &&
           blocks[blocks.size() - 2].value() > blocks.back().value()) {
      const Block last = blocks.back();
      blocks.pop_back();
      blocks.back().sum_wy += last.sum_wy;
      blocks.back().w += last.w;
      blocks.back().x_last = last.x_last;
    }
    i = j;
  }
  VrpIsotonicMap map;
  map.x.reserve(blocks.size() * 2);
  map.y.reserve(blocks.size() * 2);
  for (const Block &b : blocks) {
    map.x.push_back(b.x_first);
    map.y.push_back(b.value());
    if (b.x_last > b.x_first) { // flat block interior: two boundary points
      map.x.push_back(b.x_last);
      map.y.push_back(b.value());
    }
  }
  return Ok(std::move(map));
}

// Piecewise-linear evaluation of a fitted map, constant outside the fitted
// range. Non-finite inputs and an empty (unfitted) map pass q through
// unchanged -- the caller's forecast stays exactly unrecalibrated.
[[nodiscard]] inline double vrp_isotonic_eval(const VrpIsotonicMap &m, double q) noexcept {
  if (!std::isfinite(q) || m.x.empty()) {
    return q;
  }
  if (q <= m.x.front()) {
    return m.y.front();
  }
  if (q >= m.x.back()) {
    return m.y.back();
  }
  const auto it = std::upper_bound(m.x.begin(), m.x.end(), q);
  const auto hi = static_cast<std::size_t>(it - m.x.begin()); // first x > q; >= 1
  const std::size_t lo = hi - 1;
  const double t = (q - m.x[lo]) / (m.x[hi] - m.x[lo]); // x strictly increasing
  return m.y[lo] + t * (m.y[hi] - m.y[lo]);
}

// ── Mincer-Zarnowitz level diagnostic ───────────────────────────────────────

// OLS of realized on forecast: realized = intercept + slope * forecast.
// Level-honesty target: slope -> 1, intercept -> 0 (digest Q4 -- QLIKE alone
// can favor positively biased forecasts [15]). Non-finite pairs are
// excluded; slope/intercept stay NaN with fewer than 2 finite pairs or a
// degenerate (zero-variance) forecast -- reported as-is, never fabricated.
struct VrpMzFit {
  double slope{std::numeric_limits<double>::quiet_NaN()};
  double intercept{std::numeric_limits<double>::quiet_NaN()};
};

[[nodiscard]] inline VrpMzFit vrp_mincer_zarnowitz(std::span<const double> forecast,
                                                   std::span<const double> realized) noexcept {
  VrpMzFit fit;
  if (forecast.size() != realized.size()) {
    return fit;
  }
  double sx = 0.0;
  double sy = 0.0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < forecast.size(); ++i) {
    if (std::isfinite(forecast[i]) && std::isfinite(realized[i])) {
      sx += forecast[i];
      sy += realized[i];
      ++n;
    }
  }
  if (n < 2) {
    return fit;
  }
  const double mx = sx / static_cast<double>(n);
  const double my = sy / static_cast<double>(n);
  double sxx = 0.0;
  double sxy = 0.0;
  for (std::size_t i = 0; i < forecast.size(); ++i) {
    if (std::isfinite(forecast[i]) && std::isfinite(realized[i])) {
      const double dx = forecast[i] - mx;
      sxx += dx * dx;
      sxy += dx * (realized[i] - my);
    }
  }
  if (sxx == 0.0) {
    return fit;
  }
  fit.slope = sxy / sxx;
  fit.intercept = my - fit.slope * mx;
  return fit;
}

// ── Round-4 honest metrics: correlations, t-stats, decile tails ─────────────
//
// ROUND4-PLAN Phase 2 exists because three rounds believed a +0.22 IC that was
// an artifact. Three defects this block makes impossible:
//   * an IC / P&L aggregate quoted with no t-stat (the whole round-1..3 gross
//     book was t = -0.96 and nobody noticed) -- every aggregate below carries
//     one, and a SECOND one that pays the overlap haircut;
//   * Spearman substituted for Pearson in Grinold's alpha = IC * sigma_y * z
//     (research digest Q6.2: that substitution is where the LEVEL error dies)
//     -- Pearson is the SIZING IC, Spearman is reported strictly beside it and
//     is never the sizing input;
//   * a whole-cross-section IC quoted as if it described the TAILS the book
//     actually trades (audit S3: pred_edge_norm scored +0.042 pooled while its
//     decile tails were inverted by -2.5 vol pts) -- hence the decile block.
//
// Every statistic here is UNDEFINED-honest: fewer than 2 usable pairs, or a
// degenerate (zero-dispersion) input, yields NaN and never 0.0. A zero
// correlation is a measurement; an unmeasurable one must not impersonate it.

enum class VrpCorrKind : std::uint8_t { Pearson, Spearman };

// Bartlett bandwidth for the per-date IC series. The label spans 21 sessions
// and is sampled daily, so consecutive dates share ~21 sessions of outcome
// (audit S7: effective independent sample ~6-7 cycles against 111 dates). The
// i.i.d. t over dates is inflated by construction; the NW t at this lag is the
// textbook correction for h-period overlap.
inline constexpr std::size_t kVrpOverlapLag = kVrpHorizonSessions - 1;

inline constexpr std::size_t kVrpDecileCount = 10;
// A date needs at least one name per decile before "top decile" means
// anything; thinner dates contribute nothing rather than a fabricated tail.
inline constexpr std::size_t kVrpDecileMinNames = kVrpDecileCount;

// Pearson correlation over the pairs where BOTH values are finite. NaN (never
// 0.0) below 2 such pairs or on a constant input -- the engine kernel's
// all-zero convention is right for feature screening and wrong for a reported
// IC, so the degenerate cases are intercepted here before delegating.
[[nodiscard]] inline double vrp_pearson(std::span<const double> a,
                                        std::span<const double> b) {
  if (a.size() != b.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::vector<double> fa;
  std::vector<double> fb;
  fa.reserve(a.size());
  fb.reserve(b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::isfinite(a[i]) && std::isfinite(b[i])) {
      fa.push_back(a[i]);
      fb.push_back(b[i]);
    }
  }
  if (fa.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const bool a_flat = std::adjacent_find(fa.begin(), fa.end(),
                                         std::not_equal_to<>{}) == fa.end();
  const bool b_flat = std::adjacent_find(fb.begin(), fb.end(),
                                         std::not_equal_to<>{}) == fb.end();
  if (a_flat || b_flat) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return learn::detail::pearson(std::span<const double>{fa}, std::span<const double>{fb});
}

// Spearman rank correlation with the SAME finite-pair filter and undefined
// contract as vrp_pearson. Ranking math is the engine's (ties averaged).
[[nodiscard]] inline double vrp_spearman(std::span<const double> a,
                                         std::span<const double> b) {
  if (a.size() != b.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::vector<double> fa;
  std::vector<double> fb;
  fa.reserve(a.size());
  fb.reserve(b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::isfinite(a[i]) && std::isfinite(b[i])) {
      fa.push_back(a[i]);
      fb.push_back(b[i]);
    }
  }
  if (fa.size() < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const bool a_flat = std::adjacent_find(fa.begin(), fa.end(),
                                         std::not_equal_to<>{}) == fa.end();
  const bool b_flat = std::adjacent_find(fb.begin(), fb.end(),
                                         std::not_equal_to<>{}) == fb.end();
  if (a_flat || b_flat) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return learn::detail::spearman(std::span<const double>{fa}, std::span<const double>{fb});
}

[[nodiscard]] inline double vrp_corr(std::span<const double> a, std::span<const double> b,
                                     VrpCorrKind kind) {
  return kind == VrpCorrKind::Pearson ? vrp_pearson(a, b) : vrp_spearman(a, b);
}

// Mean of a per-date statistic series with BOTH t-stats. `t_iid` treats the
// dates as independent (it is not -- see kVrpOverlapLag); `t_nw` is the
// Newey-West/Bartlett HAC t at `nw_lag`. Quote t_nw; t_iid is published only
// so the size of the overlap haircut is visible rather than assumed.
struct VrpStatAgg {
  double mean{std::numeric_limits<double>::quiet_NaN()};
  double t_iid{std::numeric_limits<double>::quiet_NaN()};
  double t_nw{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n{0};
};

[[nodiscard]] inline VrpStatAgg vrp_aggregate_series(std::span<const double> values,
                                                     std::size_t nw_lag) {
  VrpStatAgg agg;
  std::vector<double> x;
  x.reserve(values.size());
  for (const double v : values) {
    if (std::isfinite(v)) {
      x.push_back(v);
    }
  }
  agg.n = x.size();
  if (x.empty()) {
    return agg;
  }
  double sum = 0.0;
  for (const double v : x) {
    sum += v;
  }
  const auto n = static_cast<double>(x.size());
  agg.mean = sum / n;
  if (x.size() < 2) {
    return agg;
  }
  double g0 = 0.0;
  for (const double v : x) {
    const double d = v - agg.mean;
    g0 += d * d;
  }
  g0 /= n;
  if (g0 == 0.0) {
    return agg; // a constant series has no sampling error to divide by
  }
  // Unbiased sample sd for the i.i.d. t: sqrt(n/(n-1)) rescales the population
  // second moment already accumulated above.
  const double se_iid = std::sqrt(g0 * n / (n - 1.0) / n);
  agg.t_iid = agg.mean / se_iid;
  const std::size_t lag = std::min(nw_lag, x.size() - 1);
  double s = g0;
  for (std::size_t k = 1; k <= lag; ++k) {
    double gk = 0.0;
    for (std::size_t t = k; t < x.size(); ++t) {
      gk += (x[t] - agg.mean) * (x[t - k] - agg.mean);
    }
    gk /= n;
    const double w = 1.0 - static_cast<double>(k) / static_cast<double>(lag + 1);
    s += 2.0 * w * gk;
  }
  if (s <= 0.0) {
    return agg; // small-sample HAC can go non-positive: undefined, not faked
  }
  agg.t_nw = agg.mean / std::sqrt(s / n);
  return agg;
}

// Per-DATE decile buckets of `realized` by `score` rank -- the tail statement a
// pooled IC cannot make. Buckets pool ACROSS dates after a WITHIN-date ranking,
// which is exactly what a per-date long-top/short-bottom book experiences.
//
// `spread` is the harvestability test of audit-gross-negative S1 run inside the
// trainer: top-decile mean minus bottom-decile mean of the REALIZED label, i.e.
// an equal-weighted decile long/short book with every piece of sizing stripped
// out. It is a P&L aggregate, so it carries t-stats -- the round-1..3 gross
// book was t = -0.96 and shipped three times because none was reported.
// `ic_traded` is the Grinold sizing IC on the rows the book would actually
// hold: PEARSON corr(score, realized) restricted per date to the top and bottom
// decile. A whole-cross-section IC is not a statement about the tails
// (pred_edge_norm scored +0.042 pooled while its tails were inverted).
struct VrpDecileStats {
  std::array<double, kVrpDecileCount> bucket_mean{};
  std::array<std::size_t, kVrpDecileCount> bucket_n{};
  double spread{std::numeric_limits<double>::quiet_NaN()}; // top - bottom, pooled
  double spread_t{std::numeric_limits<double>::quiet_NaN()};
  double spread_t_nw{std::numeric_limits<double>::quiet_NaN()};
  double rho{std::numeric_limits<double>::quiet_NaN()};    // Spearman(idx, mean)
  double ic_traded{std::numeric_limits<double>::quiet_NaN()};
  double ic_traded_t{std::numeric_limits<double>::quiet_NaN()};
  double ic_traded_t_nw{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_dates{0};
};

// `ts` parallels `score`/`realized` and arrives ascending (the caller pools
// fold test rows in canonical panel order).
[[nodiscard]] inline VrpDecileStats vrp_decile_stats(std::span<const std::int64_t> ts,
                                                     std::span<const double> score,
                                                     std::span<const double> realized) {
  VrpDecileStats out;
  out.bucket_mean.fill(std::numeric_limits<double>::quiet_NaN());
  if (ts.size() != score.size() || ts.size() != realized.size()) {
    return out;
  }
  std::array<double, kVrpDecileCount> sums{};
  std::vector<double> per_date_spread;
  std::vector<double> per_date_ic_traded;
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    std::vector<std::pair<double, double>> pairs; // (score, realized)
    pairs.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(score[i]) && std::isfinite(realized[i])) {
        pairs.emplace_back(score[i], realized[i]);
      }
    }
    begin = end;
    if (pairs.size() < kVrpDecileMinNames) {
      continue;
    }
    std::sort(pairs.begin(), pairs.end());
    ++out.n_dates;
    double top_sum = 0.0;
    double bot_sum = 0.0;
    std::size_t top_n = 0;
    std::size_t bot_n = 0;
    std::vector<double> traded_x;
    std::vector<double> traded_y;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      const std::size_t d = i * kVrpDecileCount / pairs.size();
      sums[d] += pairs[i].second;
      ++out.bucket_n[d];
      if (d == 0) {
        bot_sum += pairs[i].second;
        ++bot_n;
      } else if (d == kVrpDecileCount - 1) {
        top_sum += pairs[i].second;
        ++top_n;
      }
      if (d == 0 || d == kVrpDecileCount - 1) {
        traded_x.push_back(pairs[i].first);
        traded_y.push_back(pairs[i].second);
      }
    }
    if (top_n > 0 && bot_n > 0) {
      per_date_spread.push_back(top_sum / static_cast<double>(top_n) -
                                bot_sum / static_cast<double>(bot_n));
    }
    per_date_ic_traded.push_back(
        vrp_pearson(std::span<const double>{traded_x}, std::span<const double>{traded_y}));
  }
  if (out.n_dates == 0) {
    return out;
  }
  std::vector<double> idx;
  std::vector<double> mean;
  for (std::size_t d = 0; d < kVrpDecileCount; ++d) {
    if (out.bucket_n[d] > 0) {
      out.bucket_mean[d] = sums[d] / static_cast<double>(out.bucket_n[d]);
      idx.push_back(static_cast<double>(d));
      mean.push_back(out.bucket_mean[d]);
    }
  }
  if (out.bucket_n[kVrpDecileCount - 1] > 0 && out.bucket_n[0] > 0) {
    out.spread = out.bucket_mean[kVrpDecileCount - 1] - out.bucket_mean[0];
  }
  out.rho = vrp_spearman(std::span<const double>{idx}, std::span<const double>{mean});
  const VrpStatAgg sp =
      vrp_aggregate_series(std::span<const double>{per_date_spread}, kVrpOverlapLag);
  out.spread_t = sp.t_iid;
  out.spread_t_nw = sp.t_nw;
  const VrpStatAgg tr =
      vrp_aggregate_series(std::span<const double>{per_date_ic_traded}, kVrpOverlapLag);
  out.ic_traded = tr.mean;
  out.ic_traded_t = tr.t_iid;
  out.ic_traded_t_nw = tr.t_nw;
  return out;
}

// Mean squared error over finite pairs; NaN when nothing is comparable.
[[nodiscard]] inline double vrp_mse(std::span<const double> forecast,
                                    std::span<const double> realized) noexcept {
  if (forecast.size() != realized.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < forecast.size(); ++i) {
    if (std::isfinite(forecast[i]) && std::isfinite(realized[i])) {
      const double d = forecast[i] - realized[i];
      sum += d * d;
      ++n;
    }
  }
  return n == 0 ? std::numeric_limits<double>::quiet_NaN() : sum / static_cast<double>(n);
}

// What a scored column IS, which is what decides whether the gate may be
// judged against it. Benchmark == a ZERO-PARAMETER rule computable from the
// panel with nothing fitted: it costs nothing, cannot overfit, and a model
// that does not beat it has produced no evidence it should be deployed.
// Baseline == the fitted log-HAR comparator: informative, but not free, so it
// is reported and never used as the pass bar.
// Ranked == the column the BOOK sorts on (pred_edge_norm), which is not
// necessarily the column the model was scored on. Rounds 1-3 measured the IC
// on pred_label and traded pred_edge_norm, a ~0.6-correlated transform whose
// decile tails were inverted; scoring both, side by side, on the same rows is
// what makes that class of break impossible to miss again.
// Contaminated == a column that is algebraically inside the composite label
// and therefore CANNOT be a benchmark, however well it scores. Round 4 graded
// the model against one of these and the verdict it produced is void; see the
// kVrpScoreContamNegIv block below for the algebra. Scored and published so
// the evidence survives, structurally unable to decide a verdict.
enum class VrpScoreKind : std::uint8_t { Model, Baseline, Benchmark, Ranked, Contaminated };

// The three targets every score column is graded on. Round 4 graded exactly
// ONE -- the composite label -- and that single choice is what voided its
// verdict (audit-benchmark-contamination.md).
//
//   RvFwd  = rv_fwd_21d, the label's REALIZED leg alone. THE PRIMARY GATE
//            TARGET. Rank-identical to rv_fwd_21d^2 and to
//            H*(rv_fwd^2 - E_t[iv_fair^2]), since rv_fwd > 0 on labeled rows.
//   VolChg = ln(rv_fwd_21d / rv_trail_21d). The CLEANEST axis in the set: no
//            iv_fair anywhere in it and no trivial trailing-vol persistence.
//            On SP100 it is the only axis on which the GBT is the sole
//            positive predictor (+0.0613, t_nw +3.83).
//   Label  = the composite (rv_fwd^2 - iv_fair^2)*H. CONTAMINATED: its rank
//            ordering is anti-correlated with realized-vol forecasting skill
//            (the best free forecast, rv_trail^2, scores -0.1240 against it).
//            Reported for continuity with rounds 1-4; never gated on.
//   IvChgRaw  = 100*(iv_atmf_21d[t+H] - iv_atmf_21d[t]), vol points. ROUND 5's
//            TRADEABLE axis: the mark-to-market of one unit of vega. Reported
//            but NOT the money axis, because it is not a holdable P&L -- see
//            IvChgRoll and vrp_build_iv_chg below.
//   IvChgRoll = IvChgRaw minus the term-structure roll a constant-maturity
//            21d vega position pays over the same H sessions. THE ONLY
//            IV-CHANGE AXIS THE MONEY IS GRADED ON.
enum class VrpTargetAxis : std::uint8_t { RvFwd, VolChg, Label, IvChgRaw, IvChgRoll };

// The artifact key fragment for each axis. Every gate meta line names its
// target: a bare `ic_spearman` is exactly how a label-contaminated number came
// to be read as a forecast.
[[nodiscard]] inline std::string_view vrp_target_axis_key(VrpTargetAxis axis) noexcept {
  switch (axis) {
  case VrpTargetAxis::RvFwd:
    return "rv_fwd_21d";
  case VrpTargetAxis::VolChg:
    return "vol_chg_21d";
  case VrpTargetAxis::Label:
    return "label_contaminated";
  case VrpTargetAxis::IvChgRaw:
    return "iv_chg_21d_raw";
  case VrpTargetAxis::IvChgRoll:
    return "iv_chg_21d_roll";
  }
  return "unknown";
}

// ── Round-5 target: the vega P&L per unit of vega held ──────────────────────
//
// `iv_chg_raw = 100*(iv_atmf_21d[t+H] - iv_atmf_21d[t])`, vol points, where the
// exit row is the SAME SYMBOL's row exactly H pooled sessions later. A symbol
// with no row at that exact offset (a surface gap) is UNDEFINED, never
// interpolated: an exit mark that does not exist is not a fill.
//
// WHY THE RAW DIFFERENCE IS NOT A P&L, AND WHY THE ROLL LEG EXISTS.
// `iv_atmf_21d` is a CONSTANT-MATURITY index. The 21d option bought at t has
// expired by t+H, so nobody is marked at `iv_atmf_21d[t+H]`; what a vega holder
// is marked at is the SHORTER-maturity vol of the option they still hold. To
// first order in tenor the two differ by the term-structure roll, and with the
// panel's 21d/63d pair the 21d point's slope is (iv_fair_63d - iv_fair_21d)/42
// per session, so over H = 21 sessions:
//
//     roll = 100 * (iv_fair_63d[t] - iv_fair_21d[t]) / 2
//     iv_chg_roll = iv_chg_raw - roll
//
// MEASURED, NOT ASSERTED: on the SP100 OOS rows `f4_term_slope` scores rank IC
// +0.4764 (t_nw +17.51) against iv_chg_raw and +0.2038 (t_nw +5.62) against
// iv_chg_roll -- more than half of the strongest apparent predictor of the raw
// axis IS the roll accounting. `iv_fair_63d - iv_atmf_21d` runs +0.5202
// (t_nw +19.63) raw against +0.4007 roll-adjusted. A gate on the raw axis would
// crown a carry identity exactly as round 4's crowned an algebraic one.
//
// The roll uses iv_fair_63d/iv_fair_21d -- both TARGET-SIDE columns, never
// feature-lagged -- and NOT f4_term_slope, which apply_vrp_feature_lag shifts.
// The entry-date curve is what the position actually pays.
//
// KNOWN APPROXIMATION, stated rather than buried: the roll is a linear-in-tenor
// read of a K_var (strip) curve applied to an ATM-forward point. The panel
// carries no 42d leg, so the exact mark `iv_atmf_42d[t+H]` cannot be formed
// here. Both axes are therefore published and the contest is reported on both.
struct VrpIvChgTargets {
  std::vector<double> raw;  // parallel to panel.rows; vol points
  std::vector<double> roll; // raw - roll leg; NaN when the 63d leg is missing
  std::size_t n_rows_with_exit{0};
  std::size_t n_rows_no_exit{0};
  std::size_t n_rows_roll_undefined{0};
};

// Dense pooled-session ordinals over a canonically ordered panel: rows are
// ascending in entry_ts_ns, so equal-timestamp runs form one session each.
[[nodiscard]] inline std::vector<std::size_t> vrp_session_ordinals(const VrpPanel &panel) {
  std::vector<std::size_t> ord(panel.rows.size(), 0);
  std::size_t session = 0;
  for (std::size_t i = 0; i < panel.rows.size(); ++i) {
    if (i != 0 && panel.rows[i].entry_ts_ns != panel.rows[i - 1].entry_ts_ns) {
      ++session;
    }
    ord[i] = session;
  }
  return ord;
}

// ROUND 6, `entry_lag`: read the target's ENTRY leg from the row's
// `entry_lag`-th same-symbol predecessor session, leaving the exit leg at
// session[i] + horizon. This is round 5's decisive errors-in-variables swap,
// promoted from a one-off table to a runnable diagnostic (`--eiv-target-entry-
// lag 2`). The point is not realism -- a t-2 entry with a t+21 exit is a
// 23-session hold, and the report says so -- it is discrimination: a predictor
// read at t that scores only because the TARGET also contains the t read will
// hand its advantage to the t-2 read when the target's entry moves, whereas a
// predictor with a real forecast will keep it. A row whose symbol has no
// `entry_lag`-th predecessor is UNDEFINED, never substituted.
[[nodiscard]] inline VrpIvChgTargets vrp_build_iv_chg(const VrpPanel &panel,
                                                      std::size_t horizon = kVrpHorizonSessions,
                                                      std::size_t entry_lag = 0) {
  VrpIvChgTargets out;
  const std::size_t n = panel.rows.size();
  out.raw.assign(n, std::numeric_limits<double>::quiet_NaN());
  out.roll.assign(n, std::numeric_limits<double>::quiet_NaN());
  const std::vector<std::size_t> session = vrp_session_ordinals(panel);
  // (symbol index, session ordinal) -> row. std::map keeps the lookup total and
  // ordering-independent; the panel is ~20k rows so the log factor is free.
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> at;
  for (std::size_t i = 0; i < n; ++i) {
    at.emplace(std::pair<std::size_t, std::size_t>{panel.row_symbol[i], session[i]}, i);
  }
  const auto row_at = [&](std::size_t i, std::size_t ordinal) -> const VrpPanelRow * {
    const auto it = at.find(std::pair<std::size_t, std::size_t>{panel.row_symbol[i], ordinal});
    return it == at.end() ? nullptr : &panel.rows[it->second];
  };
  for (std::size_t i = 0; i < n; ++i) {
    const VrpPanelRow *exit_row = row_at(i, session[i] + horizon);
    if (exit_row == nullptr) {
      ++out.n_rows_no_exit;
      continue;
    }
    ++out.n_rows_with_exit;
    // The entry leg: this row at lag 0, else the lag-th predecessor session.
    // Session ordinals are dense and >= 0, so the subtraction is guarded.
    const VrpPanelRow *entry_row = &panel.rows[i];
    if (entry_lag != 0) {
      entry_row = session[i] < entry_lag ? nullptr : row_at(i, session[i] - entry_lag);
      if (entry_row == nullptr) {
        continue; // no lagged entry mark: undefined, never the unlagged one
      }
    }
    const VrpPanelRow &entry = *entry_row;
    const VrpPanelRow &exit = *exit_row;
    if (!(std::isfinite(entry.iv_atmf_21d) && std::isfinite(exit.iv_atmf_21d))) {
      continue; // v1 panel, or an unavailable ATMF read: undefined, not zero
    }
    out.raw[i] = 100.0 * (exit.iv_atmf_21d - entry.iv_atmf_21d);
    if (!(std::isfinite(entry.iv_fair_63d) && std::isfinite(entry.iv_fair_21d))) {
      ++out.n_rows_roll_undefined;
      continue;
    }
    const double roll = 100.0 * (entry.iv_fair_63d - entry.iv_fair_21d) / 2.0;
    out.roll[i] = out.raw[i] - roll;
  }
  return out;
}

// Everything the gate says about ONE score column against ONE target axis on
// ONE row set. `ic_pearson` is THE sizing IC (Grinold alpha = IC * sigma_y *
// z); the Spearman fields exist to be REPORTED, never to be substituted into
// it. `mse` / `mz_*` are populated only on the Label axis, and only for scores
// that live in LABEL units -- a pure ranking axis (a benchmark, a z-score) has
// no level claim, and neither does any forecast measured against rv_fwd_21d or
// against a log vol ratio. A fabricated MSE would be a category error of
// exactly the kind QLIKE on a signed spread was.
struct VrpScoreReport {
  std::string name;
  VrpScoreKind kind{VrpScoreKind::Benchmark};
  VrpTargetAxis target{VrpTargetAxis::RvFwd};
  double ic_pearson{std::numeric_limits<double>::quiet_NaN()};
  double ic_pearson_t{std::numeric_limits<double>::quiet_NaN()};
  double ic_pearson_t_nw{std::numeric_limits<double>::quiet_NaN()};
  double ic_spearman{std::numeric_limits<double>::quiet_NaN()};
  double ic_spearman_t{std::numeric_limits<double>::quiet_NaN()};
  double ic_spearman_t_nw{std::numeric_limits<double>::quiet_NaN()};
  double ic_pearson_pooled{std::numeric_limits<double>::quiet_NaN()};
  double ic_spearman_pooled{std::numeric_limits<double>::quiet_NaN()};
  // Grinold sizing IC restricted to the decile tails a book would hold.
  double ic_pearson_traded{std::numeric_limits<double>::quiet_NaN()};
  double ic_pearson_traded_t{std::numeric_limits<double>::quiet_NaN()};
  double ic_pearson_traded_t_nw{std::numeric_limits<double>::quiet_NaN()};
  // Equal-weight decile long/short P&L proxy, with its t-stats.
  double decile_spread{std::numeric_limits<double>::quiet_NaN()};
  double decile_spread_t{std::numeric_limits<double>::quiet_NaN()};
  double decile_spread_t_nw{std::numeric_limits<double>::quiet_NaN()};
  double decile_rho{std::numeric_limits<double>::quiet_NaN()};
  double mse{std::numeric_limits<double>::quiet_NaN()};
  double mz_slope{std::numeric_limits<double>::quiet_NaN()};
  double mz_intercept{std::numeric_limits<double>::quiet_NaN()};
  // Dates that produced a DEFINED per-date Pearson IC (>= 2 finite pairs and
  // both inputs non-degenerate), not the raw date count -- the mean is over
  // these and nothing else. n_rows is every row handed in, including the ones
  // the finite filter dropped, so the two together show the attrition.
  std::size_t n_dates{0};
  std::size_t n_rows{0};
};

// Score one column against ONE target axis on one row set. `label_units`
// gates the MSE / Mincer-Zarnowitz block (see the struct contract above) and
// is meaningful only on VrpTargetAxis::Label -- a forecast of the composite
// label has no level claim against rv_fwd_21d or against a log vol ratio.
[[nodiscard]] inline VrpScoreReport vrp_score_report(std::string name, VrpScoreKind kind,
                                                     VrpTargetAxis target,
                                                     std::span<const std::int64_t> ts,
                                                     std::span<const double> score,
                                                     std::span<const double> realized,
                                                     bool label_units) {
  VrpScoreReport rep;
  rep.name = std::move(name);
  rep.kind = kind;
  rep.target = target;
  if (ts.size() != score.size() || ts.size() != realized.size()) {
    return rep;
  }
  rep.n_rows = ts.size();
  std::vector<double> per_date_p;
  std::vector<double> per_date_s;
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    const auto sub = [&](std::span<const double> v) {
      return v.subspan(begin, end - begin);
    };
    per_date_p.push_back(vrp_pearson(sub(score), sub(realized)));
    per_date_s.push_back(vrp_spearman(sub(score), sub(realized)));
    begin = end;
  }
  const VrpStatAgg ap = vrp_aggregate_series(std::span<const double>{per_date_p},
                                             kVrpOverlapLag);
  const VrpStatAgg as = vrp_aggregate_series(std::span<const double>{per_date_s},
                                             kVrpOverlapLag);
  rep.ic_pearson = ap.mean;
  rep.ic_pearson_t = ap.t_iid;
  rep.ic_pearson_t_nw = ap.t_nw;
  rep.n_dates = ap.n;
  rep.ic_spearman = as.mean;
  rep.ic_spearman_t = as.t_iid;
  rep.ic_spearman_t_nw = as.t_nw;
  rep.ic_pearson_pooled = vrp_pearson(score, realized);
  rep.ic_spearman_pooled = vrp_spearman(score, realized);
  const VrpDecileStats dec = vrp_decile_stats(ts, score, realized);
  rep.decile_spread = dec.spread;
  rep.decile_spread_t = dec.spread_t;
  rep.decile_spread_t_nw = dec.spread_t_nw;
  rep.decile_rho = dec.rho;
  rep.ic_pearson_traded = dec.ic_traded;
  rep.ic_pearson_traded_t = dec.ic_traded_t;
  rep.ic_pearson_traded_t_nw = dec.ic_traded_t_nw;
  // Enforced, not merely requested: a level loss is only defined when the
  // forecast and the target share units, which happens on the Label axis
  // alone. Fabricating one against rv_fwd_21d would be the same category error
  // as QLIKE on a signed spread.
  if (label_units && target == VrpTargetAxis::Label) {
    rep.mse = vrp_mse(score, realized);
    const VrpMzFit mz = vrp_mincer_zarnowitz(score, realized);
    rep.mz_slope = mz.slope;
    rep.mz_intercept = mz.intercept;
  }
  return rep;
}

// ── Money: carry per unit of GROSS VEGA, and the floor it has to clear ──────
//
// IC is not money. Every P&L figure in this gate is the hold-to-horizon carry
// of a daily-delta-hedged ATM straddle carrying ONE unit of vega per name:
//
//   ppv (vol points) = 100 * (rv_fwd_21d^2 - iv_fair_21d^2) / (2 * iv_fair_21d)
//
// A long-top / short-bottom book spends TWO units of gross vega for its quoted
// leg spread, so every figure below is halved and quoted per 1u GROSS VEGA.
// That is the only unit in which a selection book is comparable to the
// zero-selection floor it has to clear.
//
// WINSORIZATION IS NOT COSMETIC HERE. The SP100 panel still carries the
// unadjusted-split rows of audit-gross-negative.md S6: three symbols own
// 78.21% of sum|ppv| across 284 of 9110 rows, with single rows at +16465 vol
// points. A mean over those is not a measurement. |ppv| is capped at
// kVrpPpvWinsorAbs and the number of capped rows is PUBLISHED, so the data
// defect stays visible instead of being averaged into a headline.
inline constexpr double kVrpPpvWinsorAbs = 60.0;
inline constexpr std::size_t kVrpIvNeutralQuintiles = 5;
// A date needs at least two names per IV quintile before "top 20% of this
// quintile" means anything; thinner dates contribute nothing.
inline constexpr std::size_t kVrpIvNeutralMinNames = 2 * kVrpIvNeutralQuintiles;

// Raw, UNWINSORIZED carry of one row. NaN when the row cannot be priced --
// iv_fair_21d is the denominator and load_vrp_panel already enforces it
// positive on labeled rows, so a non-positive one here means a tail row.
[[nodiscard]] inline double vrp_ppv_raw(double rv_fwd, double iv_fair) noexcept {
  if (!(std::isfinite(rv_fwd) && std::isfinite(iv_fair)) || !(iv_fair > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return 100.0 * (rv_fwd * rv_fwd - iv_fair * iv_fair) / (2.0 * iv_fair);
}

struct VrpPpvSeries {
  std::vector<double> ppv;     // winsorized, parallel to the input rows
  std::size_t n_priced{0};     // rows that produced a finite raw carry
  std::size_t n_winsorized{0}; // rows the |ppv| cap actually moved
};

[[nodiscard]] inline VrpPpvSeries vrp_build_ppv(std::span<const double> rv_fwd,
                                                std::span<const double> iv_fair) {
  VrpPpvSeries out;
  if (rv_fwd.size() != iv_fair.size()) {
    return out;
  }
  out.ppv.reserve(rv_fwd.size());
  for (std::size_t i = 0; i < rv_fwd.size(); ++i) {
    const double raw = vrp_ppv_raw(rv_fwd[i], iv_fair[i]);
    if (!std::isfinite(raw)) {
      out.ppv.push_back(std::numeric_limits<double>::quiet_NaN());
      continue;
    }
    ++out.n_priced;
    const double w = std::clamp(raw, -kVrpPpvWinsorAbs, kVrpPpvWinsorAbs);
    if (w != raw) {
      ++out.n_winsorized;
    }
    out.ppv.push_back(w);
  }
  return out;
}

// THE ZERO-SELECTION FLOOR. Short the entire cross-section, one unit of vega
// per name, no model, no ranking, no fit. Shorting earns -ppv, so the floor is
// the negated per-date cross-sectional mean carry, and it already stands per
// 1u gross vega.
//
// It exists because short-vol BETA was mistaken for selection SKILL for three
// rounds. Measured here on the round-4 SP100 OOS rows it is +3.706 vol pts/
// cycle (t_nw +2.89) and on clean25 +3.843 (t_nw +2.70). At the decile book
// -iv_fair_21d earns +1.836 and the GBT +2.704 -- BOTH LESS THAN DOING NOTHING
// -- and only hv_iv_gap (+3.951) clears it, by +0.245 at t_nw +0.24. After
// IV-neutralisation NOTHING in the set clears it on either corpus. It is
// computed from the run's own rows and is NEVER a hardcoded constant: it is a
// property of the corpus and the period, not of this code.
struct VrpPnlFloor {
  double mean{std::numeric_limits<double>::quiet_NaN()};
  double t_iid{std::numeric_limits<double>::quiet_NaN()};
  double t_nw{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_dates{0};
  // One entry per date GROUP of `ts`, in encounter order, NaN where the date
  // priced nothing. Books are aligned to this vector index-for-index so the
  // excess-over-floor is a PAIRED per-date difference and carries its own t.
  std::vector<double> per_date;
};

[[nodiscard]] inline VrpPnlFloor vrp_short_everything_floor(std::span<const std::int64_t> ts,
                                                            std::span<const double> ppv) {
  VrpPnlFloor out;
  if (ts.size() != ppv.size()) {
    return out;
  }
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(ppv[i])) {
        sum += ppv[i];
        ++n;
      }
    }
    out.per_date.push_back(n == 0 ? std::numeric_limits<double>::quiet_NaN()
                                  : -sum / static_cast<double>(n));
    begin = end;
  }
  const VrpStatAgg agg =
      vrp_aggregate_series(std::span<const double>{out.per_date}, kVrpOverlapLag);
  out.mean = agg.mean;
  out.t_iid = agg.t_iid;
  out.t_nw = agg.t_nw;
  out.n_dates = agg.n;
  return out;
}

// A book's realized carry and -- PERMANENTLY ATTACHED -- its excess over the
// short-everything floor. The two travel together in one struct because they
// were once quoted apart, and a +3.7 vol-pt short-vol beta then read as
// selection skill. `mean` on its own is not a claim about skill and must never
// be published without `excess` beside it.
struct VrpPnlAgg {
  double mean{std::numeric_limits<double>::quiet_NaN()}; // vol pts / 1u gross vega / cycle
  double t_iid{std::numeric_limits<double>::quiet_NaN()};
  double t_nw{std::numeric_limits<double>::quiet_NaN()};
  double excess{std::numeric_limits<double>::quiet_NaN()}; // mean - floor, same units
  double excess_t{std::numeric_limits<double>::quiet_NaN()};
  double excess_t_nw{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_dates{0};
};

[[nodiscard]] inline VrpPnlAgg vrp_pnl_agg(std::span<const double> per_date_book,
                                           std::span<const double> per_date_floor) {
  VrpPnlAgg out;
  const VrpStatAgg raw = vrp_aggregate_series(per_date_book, kVrpOverlapLag);
  out.mean = raw.mean;
  out.t_iid = raw.t_iid;
  out.t_nw = raw.t_nw;
  out.n_dates = raw.n;
  if (per_date_book.size() != per_date_floor.size()) {
    return out; // misaligned inputs: undefined, never faked
  }
  std::vector<double> diff(per_date_book.size(),
                           std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < diff.size(); ++i) {
    diff[i] = per_date_book[i] - per_date_floor[i];
  }
  const VrpStatAgg ex = vrp_aggregate_series(std::span<const double>{diff}, kVrpOverlapLag);
  out.excess = ex.mean;
  out.excess_t = ex.t_iid;
  out.excess_t_nw = ex.t_nw;
  return out;
}

// Per-date long-top-decile / short-bottom-decile carry, per 1u gross vega.
// One entry per date GROUP of `ts` (NaN on dates below the one-name-per-decile
// floor), so the result aligns index-for-index with VrpPnlFloor::per_date.
[[nodiscard]] inline std::vector<double>
vrp_decile_book_per_date(std::span<const std::int64_t> ts, std::span<const double> score,
                         std::span<const double> ppv) {
  std::vector<double> out;
  if (ts.size() != score.size() || ts.size() != ppv.size()) {
    return out;
  }
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    std::vector<std::pair<double, double>> pairs; // (score, ppv)
    pairs.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(score[i]) && std::isfinite(ppv[i])) {
        pairs.emplace_back(score[i], ppv[i]);
      }
    }
    begin = end;
    if (pairs.size() < kVrpDecileMinNames) {
      out.push_back(std::numeric_limits<double>::quiet_NaN());
      continue;
    }
    std::sort(pairs.begin(), pairs.end());
    double top = 0.0;
    double bot = 0.0;
    std::size_t top_n = 0;
    std::size_t bot_n = 0;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      const std::size_t d = i * kVrpDecileCount / pairs.size();
      if (d == 0) {
        bot += pairs[i].second;
        ++bot_n;
      } else if (d == kVrpDecileCount - 1) {
        top += pairs[i].second;
        ++top_n;
      }
    }
    // Halved: the quoted leg spread costs 2 units of gross vega, 1 per leg.
    out.push_back(top_n == 0 || bot_n == 0
                      ? std::numeric_limits<double>::quiet_NaN()
                      : 0.5 * (top / static_cast<double>(top_n) -
                               bot / static_cast<double>(bot_n)));
  }
  return out;
}

// IV-QUINTILE-NEUTRALISED carry -- audit-benchmark-contamination.md S6b's
// method verbatim: split the date into 5 quintiles by iv_fair_21d, rank INSIDE
// each quintile, long the top 20% and short the bottom 20% of each, average the
// five. Same per-date alignment contract as vrp_decile_book_per_date.
//
// This is the transform that separates selection from a static vol-LEVEL tilt.
// A book ranked on implied cheapness shorts names at 62 vol and buys names at
// 22.5 vol; 90% of -iv_fair_21d's P&L evaporates under it (+3.567 -> +0.355,
// t_nw 0.45) while the GBT's survives largely intact (+2.807 -> +2.157) because
// its legs were IV-balanced to begin with (0.396 vs 0.386).
[[nodiscard]] inline std::vector<double>
vrp_iv_neutral_book_per_date(std::span<const std::int64_t> ts, std::span<const double> score,
                             std::span<const double> ppv, std::span<const double> iv_fair) {
  std::vector<double> out;
  if (ts.size() != score.size() || ts.size() != ppv.size() || ts.size() != iv_fair.size()) {
    return out;
  }
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    // (iv, score, ppv); sorted by iv first, so ties break on score then ppv --
    // total and deterministic, which a reproducible artifact requires.
    std::vector<std::array<double, 3>> rows;
    rows.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(score[i]) && std::isfinite(ppv[i]) && std::isfinite(iv_fair[i])) {
        rows.push_back({iv_fair[i], score[i], ppv[i]});
      }
    }
    begin = end;
    if (rows.size() < kVrpIvNeutralMinNames) {
      out.push_back(std::numeric_limits<double>::quiet_NaN());
      continue;
    }
    std::sort(rows.begin(), rows.end());
    double q_sum = 0.0;
    std::size_t q_n = 0;
    for (std::size_t q = 0; q < kVrpIvNeutralQuintiles; ++q) {
      const std::size_t lo = q * rows.size() / kVrpIvNeutralQuintiles;
      const std::size_t hi = (q + 1) * rows.size() / kVrpIvNeutralQuintiles;
      const std::size_t m = hi - lo;
      if (m < 2) {
        continue;
      }
      std::vector<std::pair<double, double>> in_q; // (score, ppv)
      in_q.reserve(m);
      for (std::size_t i = lo; i < hi; ++i) {
        in_q.emplace_back(rows[i][1], rows[i][2]);
      }
      std::sort(in_q.begin(), in_q.end());
      // "Top/bottom 20%", floored at one name per side so a 2..4-name quintile
      // still expresses a long and a short rather than being dropped.
      const std::size_t side = std::max<std::size_t>(1, m / kVrpIvNeutralQuintiles);
      double top = 0.0;
      double bot = 0.0;
      for (std::size_t k = 0; k < side; ++k) {
        bot += in_q[k].second;
        top += in_q[m - 1 - k].second;
      }
      q_sum += 0.5 * (top - bot) / static_cast<double>(side);
      ++q_n;
    }
    out.push_back(q_n == 0 ? std::numeric_limits<double>::quiet_NaN()
                           : q_sum / static_cast<double>(q_n));
  }
  return out;
}

// Everything this gate says in MONEY about one score column on one row set.
// Both books carry their excess over the same short-everything floor.
struct VrpPnlReport {
  std::string name;
  VrpScoreKind kind{VrpScoreKind::Benchmark};
  VrpPnlAgg decile;     // long top decile / short bottom decile
  VrpPnlAgg iv_neutral; // 5 IV quintiles, top/bottom 20% inside each
};

[[nodiscard]] inline VrpPnlReport
vrp_pnl_report(std::string name, VrpScoreKind kind, std::span<const std::int64_t> ts,
               std::span<const double> score, std::span<const double> ppv,
               std::span<const double> iv_fair, const VrpPnlFloor &floor) {
  VrpPnlReport rep;
  rep.name = std::move(name);
  rep.kind = kind;
  const std::vector<double> dec = vrp_decile_book_per_date(ts, score, ppv);
  const std::vector<double> ivn = vrp_iv_neutral_book_per_date(ts, score, ppv, iv_fair);
  rep.decile = vrp_pnl_agg(std::span<const double>{dec},
                           std::span<const double>{floor.per_date});
  rep.iv_neutral = vrp_pnl_agg(std::span<const double>{ivn},
                               std::span<const double>{floor.per_date});
  return rep;
}

// ── The VEGA book: money on the iv-change axis ──────────────────────────────
//
// A vol-CHANGE forecast is not harvested by a 21-day variance position. It is
// harvested by HOLDING VEGA: long where implied is predicted to rise, short
// where it is predicted to fall, marked at the horizon. Everything below is in
// the same unit as the variance block -- VOL POINTS PER 1 UNIT OF GROSS VEGA
// PER CYCLE -- so the two are directly comparable and so is the floor.
//
// COSTS. research-vrp-premise.md Q2.1 gives the conversion that makes an option
// spread commensurable with a vol-point P&L:
//
//     one-way cost in VOL POINTS = (one-way cost as a fraction of premium) * sigma
//
// because Price ~ 0.4*S*sigma*sqrt(T) and Vega ~ 0.4*S*sqrt(T). The fraction is
// Christoffersen-Goyenko-Jacobs-Karoui (RFS 2018) intraday EFFECTIVE relative
// spread for S&P 500 constituent ATM calls, 6.41% of price, halved to a one-way
// charge. That is a MEASURED effective spread on the exact universe, not a
// quoted one and not an optimistic algo bound. Charged on BOTH legs of the
// round trip (entry and exit), per Zhan-Han-Cao-Tong (RFS 2022), whose backtest
// convention is explicitly costs at initiation AND close.
//
// WHAT THIS COST NUMBER DOES NOT COVER, stated because it flatters the book:
// a literally constant-maturity 21d vega position is re-struck every session
// and would pay ~21 round trips, not one. One round trip prices the honest
// alternative -- buy a longer-dated straddle once and let its maturity drift --
// which is also what the iv_chg_roll axis is constructed to mark.
//
// ── ROUND 6: THE CROSSING FRACTION, MADE REACHABLE ─────────────────────────
//
// The audit `audit-cost-model.md` (defect #2) found the engine's option charge
// crossing 100% of an assumed QUOTED spread while the repo's own ORATS
// calibration (`crossing_fraction_complex = 0.53`, api/backtest/backtest.hpp)
// sat unreachable under `SpreadKind::VolTicks`. That defect is REAL and it is
// an ENGINE defect. It is NOT a defect of the number above, and the difference
// decides whether costs go up or down -- so it is spelled out rather than
// asserted:
//
//   * 6.41% (Christoffersen et al.) is an EFFECTIVE relative spread, measured
//     from intraday trade prints against the prevailing mid. The crossing
//     fraction is ALREADY INSIDE IT. Multiplying it by 0.53 would count the
//     same discount twice and hand the book a free 47% cost cut.
//   * The engine's `cost_half_spread_vol_pts` is an operator-supplied width
//     with no measurement behind it, crossed at 1.00. THAT is where 0.53
//     belongs -- and applying it there without also restating the width as a
//     QUOTED one is exactly the flattering half of the correction.
//
// So the model is restated as an explicit two-factor product, with the quoted
// width DERIVED from the two measurements rather than asserted:
//
//     one-way frac of premium  =  quoted_half_spread_frac  x  f_cross
//     quoted_half_spread_frac  =  3.205%  /  0.55  =  5.827% of premium
//
// where 0.55 is Zhan-Han-Cao-Tong (RFS 2022) realized effective/quoted measured
// on ACTUAL OPRA prints 2003-16 -- the best-measured ratio in the digest, and
// the one the sprint's research doc names as the honest central case. Two
// independent roads then agree to within 4%: the ORATS complex-order constant
// 0.53 on the same derived quoted width gives 3.09% one-way (0.93 vol pts at
// sigma = 30%) against Christoffersen's 3.205% (0.96 vol pts).
//
// The charge is computed as `effective x (f_cross / 0.55)` rather than
// `quoted x f_cross` so that the DEFAULT returns the measured effective spread
// BIT-EXACTLY (x/x is exactly 1.0), i.e. this restatement is not a silent
// re-pricing of round 5. `--cost-crossing-fraction 1.00` is the stress corner
// and reproduces a full-quoted-spread crossing; 0.38 is Muravyev-Pearson's
// patient-algo bound; anything below 0.30 is unsupported by any published
// measurement (research-vrp-premise.md, "Do not tune costs down").
inline constexpr double kVrpVegaEffectiveOneWayFracOfPremium = 0.0641 / 2.0;
inline constexpr double kVrpVegaCrossingFractionMeasured = 0.55;
inline constexpr double kVrpVegaQuotedOneWayFracOfPremium =
    kVrpVegaEffectiveOneWayFracOfPremium / kVrpVegaCrossingFractionMeasured;
inline constexpr double kVrpDefaultCrossingFraction = kVrpVegaCrossingFractionMeasured;
// Retained name for the DEFAULT one-way charge, so the meta line and every
// round-5 reader keep meaning the same thing.
inline constexpr double kVrpVegaOneWayCostFracOfPremium = kVrpVegaEffectiveOneWayFracOfPremium;

// ASYMMETRIC OBJECTIVE (user directive; research-vrp-premise.md Q3.3).
// Long vega and short vega are not worth the same per unit of measured edge:
// short vol carries unbounded single-name gap risk, negative skew and left-tail
// correlation across names, and its Sharpe is the payoff Goetzmann-Ingersoll-
// Spiegel-Welch (RFS 2007) prove is MAXIMISED by option selling that adds no
// value; Faias-Santa-Clara (JFQA 2017) exhibit a Sharpe-0.47 option book whose
// certainty equivalent is -100%. Long vega has bounded loss and positive
// convexity. So a short-vega gain is discounted and a short-vega LOSS is not:
// the haircut applies only when the short leg's aggregate edge is positive.
//
// 0.50 IS A JUDGEMENT, NOT A MEASUREMENT, and is stated as one: it requires a
// short-vega edge to be twice a long-vega edge before the two count equally.
// The knob exists so a reader can set it to 0 and see the undiscounted book.
inline constexpr double kVrpDefaultShortVegaHaircut = 0.50;

// One-way cost of one unit of vega on a name marked at `iv` (a DECIMAL vol),
// in vol points, crossing `crossing` of the derived QUOTED width. NaN in, NaN
// out -- an unpriceable leg is not a free one; a non-finite or negative
// crossing fraction is also NaN, so a bad knob cannot silently trade free.
//
// Note the identity the research digest supplies (Q2.1): cost in vol points is
// (frac of premium) * sigma, so the charge SCALES WITH THE NAME'S OWN IV. A
// flat vol-point charge -- what the engine's `cost_half_spread_vol_pts` is --
// systematically undercharges high-vol names and overcharges low-vol ones.
[[nodiscard]] inline double vrp_vega_cost_one_way(double iv, double crossing) noexcept {
  if (!std::isfinite(iv) || !(iv > 0.0) || !std::isfinite(crossing) || crossing < 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double frac =
      kVrpVegaEffectiveOneWayFracOfPremium * (crossing / kVrpVegaCrossingFractionMeasured);
  return frac * 100.0 * iv;
}

[[nodiscard]] inline double vrp_vega_cost_one_way(double iv) noexcept {
  return vrp_vega_cost_one_way(iv, kVrpDefaultCrossingFraction);
}

// Per-date series of one long/short vega book, all per 1u GROSS vega except
// the two leg series, which are per 1u of THEIR OWN vega so a leg can be read
// on its own terms. One entry per date GROUP of `ts`, NaN where the date could
// not fill both legs -- same alignment contract as VrpPnlFloor::per_date.
struct VrpVegaLegs {
  std::vector<double> gross;     // before costs, per 1u gross vega
  std::vector<double> cost;      // round-trip charge actually applied
  std::vector<double> net;       // gross - cost
  std::vector<double> long_leg;  // net, per 1u LONG vega
  std::vector<double> short_leg; // net, per 1u SHORT vega
  // ROUND 6 IMPLEMENTABILITY COLUMNS. A net figure says nothing about whether
  // the book can be held: 4 names a day is not a strategy however good its t.
  std::vector<double> names;    // long members + short members on this date
  std::vector<double> turnover; // fraction of GROSS replaced vs the previous
                                // formation date; 1.0 = complete replacement,
                                // 0.0 = identical book. NaN on the first date.
  // Signed per-1u-gross weights of this date's book, kept only long enough to
  // difference against the next date. Empty where the date could not fill.
  std::vector<std::map<std::size_t, double>> weights;
};

namespace detail {

// Signed weights per 1 unit of GROSS vega: each of `L` long names carries
// +1/(2L) and each of `S` short names -1/(2S), so sum|w| == 1 and the series
// is in the same unit as the P&L above. A symbol appearing on both sides
// (impossible for a decile book, possible in principle for the quintile-
// neutral one) nets, which is the economically correct thing to trade.
inline void add_weight(std::map<std::size_t, double> &w, std::size_t sym, double v) {
  w[sym] += v;
}

// One-way fraction of GROSS replaced going from `prev` to `cur`, in [0, 1].
// Half the L1 distance, because sum|w| == 1 on each side and a complete
// replacement moves 1 unit out and 1 unit in.
[[nodiscard]] inline double weight_turnover(const std::map<std::size_t, double> &prev,
                                            const std::map<std::size_t, double> &cur) noexcept {
  double l1 = 0.0;
  for (const auto &[sym, w] : cur) {
    const auto it = prev.find(sym);
    l1 += std::abs(w - (it == prev.end() ? 0.0 : it->second));
  }
  for (const auto &[sym, w] : prev) {
    if (cur.find(sym) == cur.end()) {
      l1 += std::abs(w);
    }
  }
  return 0.5 * l1;
}

// Fill `legs.turnover` from `legs.weights`: NaN wherever either endpoint has no
// book, so a gap never reads as a zero-turnover hold.
inline void fill_turnover(VrpVegaLegs &legs) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  legs.turnover.assign(legs.weights.size(), nan);
  for (std::size_t i = 1; i < legs.weights.size(); ++i) {
    if (!legs.weights[i].empty() && !legs.weights[i - 1].empty()) {
      legs.turnover[i] = weight_turnover(legs.weights[i - 1], legs.weights[i]);
    }
  }
}

// Worst peak-to-trough of the cumulative per-date series, as a POSITIVE
// magnitude in the series' own unit. NaN dates are skipped (the curve holds
// flat), so a missing date never manufactures a drawdown. NaN when no finite
// entry exists at all -- an unmeasurable path has no measured drawdown.
[[nodiscard]] inline double max_drawdown(std::span<const double> per_date) noexcept {
  double cum = 0.0;
  double peak = 0.0;
  double worst = 0.0;
  bool any = false;
  for (const double x : per_date) {
    if (!std::isfinite(x)) {
      continue;
    }
    any = true;
    cum += x;
    peak = std::max(peak, cum);
    worst = std::max(worst, peak - cum);
  }
  return any ? worst : std::numeric_limits<double>::quiet_NaN();
}

// Mean of the finite entries of `v`, NaN when there are none.
[[nodiscard]] inline double mean_finite(std::span<const double> v) noexcept {
  double s = 0.0;
  std::size_t n = 0;
  for (const double x : v) {
    if (std::isfinite(x)) {
      s += x;
      ++n;
    }
  }
  return n == 0 ? std::numeric_limits<double>::quiet_NaN() : s / static_cast<double>(n);
}

} // namespace detail

// Rank by `score` inside each date, long the top decile / short the bottom
// decile, hold to horizon, mark at `target` (the iv-change axis, vol points per
// 1u vega), charge a round trip on each leg at that leg's own entry IV.
[[nodiscard]] inline VrpVegaLegs
vrp_vega_book_per_date(std::span<const std::int64_t> ts, std::span<const double> score,
                       std::span<const double> target, std::span<const double> iv,
                       std::span<const std::size_t> sym, double crossing, bool charge_costs) {
  VrpVegaLegs out;
  if (ts.size() != score.size() || ts.size() != target.size() || ts.size() != iv.size() ||
      ts.size() != sym.size()) {
    return out;
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto empty_date = [&](VrpVegaLegs &o) {
    o.gross.push_back(nan);
    o.cost.push_back(nan);
    o.net.push_back(nan);
    o.long_leg.push_back(nan);
    o.short_leg.push_back(nan);
    o.names.push_back(nan);
    o.weights.emplace_back();
  };
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    // (score, target, iv, symbol) -- symbol last so the ordering of the first
    // three, and therefore every round-5 figure, is unchanged.
    std::vector<std::array<double, 4>> rows;
    rows.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(score[i]) && std::isfinite(target[i]) && std::isfinite(iv[i])) {
        rows.push_back({score[i], target[i], iv[i], static_cast<double>(sym[i])});
      }
    }
    begin = end;
    if (rows.size() < kVrpDecileMinNames) {
      empty_date(out);
      continue;
    }
    std::sort(rows.begin(), rows.end());
    double top_pnl = 0.0;
    double bot_pnl = 0.0;
    double top_cost = 0.0;
    double bot_cost = 0.0;
    std::size_t top_n = 0;
    std::size_t bot_n = 0;
    std::vector<std::size_t> top_sym;
    std::vector<std::size_t> bot_sym;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const std::size_t d = i * kVrpDecileCount / rows.size();
      const double rt = charge_costs ? 2.0 * vrp_vega_cost_one_way(rows[i][2], crossing) : 0.0;
      if (d == 0) {
        bot_pnl += rows[i][1];
        bot_cost += rt;
        ++bot_n;
        bot_sym.push_back(static_cast<std::size_t>(rows[i][3]));
      } else if (d == kVrpDecileCount - 1) {
        top_pnl += rows[i][1];
        top_cost += rt;
        ++top_n;
        top_sym.push_back(static_cast<std::size_t>(rows[i][3]));
      }
    }
    if (top_n == 0 || bot_n == 0) {
      empty_date(out);
      continue;
    }
    std::map<std::size_t, double> w;
    for (const std::size_t s : top_sym) {
      detail::add_weight(w, s, 0.5 / static_cast<double>(top_n));
    }
    for (const std::size_t s : bot_sym) {
      detail::add_weight(w, s, -0.5 / static_cast<double>(bot_n));
    }
    out.names.push_back(static_cast<double>(top_n + bot_n));
    out.weights.push_back(std::move(w));
    const auto avg = [](double s, std::size_t k) { return s / static_cast<double>(k); };
    // LONG vega earns +target; SHORT vega earns -target. Costs are paid by both.
    const double lg_gross = avg(top_pnl, top_n);
    const double sh_gross = -avg(bot_pnl, bot_n);
    const double lg_cost = avg(top_cost, top_n);
    const double sh_cost = avg(bot_cost, bot_n);
    out.long_leg.push_back(lg_gross - lg_cost);
    out.short_leg.push_back(sh_gross - sh_cost);
    // The pair spends 2 units of gross vega, so the quoted figure is halved --
    // the same convention the variance block uses, and the only one in which
    // this book is comparable to a zero-selection floor.
    out.gross.push_back(0.5 * (lg_gross + sh_gross));
    out.cost.push_back(0.5 * (lg_cost + sh_cost));
    out.net.push_back(0.5 * (lg_gross + sh_gross - lg_cost - sh_cost));
  }
  detail::fill_turnover(out);
  return out;
}

// THE ZERO-SELECTION VEGA FLOOR. The variance block's floor is "short the whole
// cross-section" because a variance book is structurally short vol. A vega book
// is not: its zero-selection alternatives are LONG everything and SHORT
// everything, and which of the two is the bar is a property of the sample, not
// of the code. Both are computed, both are published, and the bar is whichever
// earned more over the whole run -- picked ONCE and then applied to every date,
// never per-date hindsight.
//
// This matters here more than it did for the variance book: on the round-5
// SP100 rows the cross-section's mean 21d ATMF implied vol ROSE, so long-vega
// beta is this sample's free money exactly as short-vol beta was rounds 1-4's.
struct VrpVegaFloor {
  std::vector<double> per_date_long;  // long 1u vega on every name, net of cost
  std::vector<double> per_date_short; // short 1u vega on every name, net of cost
  std::vector<double> per_date_best;  // the higher-mean of the two, uniformly
  VrpStatAgg long_all;
  VrpStatAgg short_all;
  bool best_is_long{true};
};

[[nodiscard]] inline VrpVegaFloor vrp_vega_floor(std::span<const std::int64_t> ts,
                                                 std::span<const double> target,
                                                 std::span<const double> iv, double crossing,
                                                 bool charge_costs) {
  VrpVegaFloor out;
  if (ts.size() != target.size() || ts.size() != iv.size()) {
    return out;
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    double pnl = 0.0;
    double cost = 0.0;
    std::size_t n = 0;
    for (std::size_t i = begin; i < end; ++i) {
      const double rt = charge_costs ? 2.0 * vrp_vega_cost_one_way(iv[i], crossing) : 0.0;
      if (std::isfinite(target[i]) && std::isfinite(rt)) {
        pnl += target[i];
        cost += rt;
        ++n;
      }
    }
    begin = end;
    if (n == 0) {
      out.per_date_long.push_back(nan);
      out.per_date_short.push_back(nan);
      continue;
    }
    const double m = pnl / static_cast<double>(n);
    const double c = cost / static_cast<double>(n);
    out.per_date_long.push_back(m - c);
    out.per_date_short.push_back(-m - c);
  }
  out.long_all = vrp_aggregate_series(std::span<const double>{out.per_date_long}, kVrpOverlapLag);
  out.short_all =
      vrp_aggregate_series(std::span<const double>{out.per_date_short}, kVrpOverlapLag);
  // NaN-hostile: an unmeasurable alternative never becomes the bar by default.
  out.best_is_long = !(out.short_all.mean > out.long_all.mean);
  out.per_date_best = out.best_is_long ? out.per_date_long : out.per_date_short;
  return out;
}

// Everything the gate says in money about one score column's VEGA book.
// `objective` is the asymmetric figure: the short leg's contribution is scaled
// by (1 - haircut) when its aggregate edge is positive and left alone when it
// is negative. The multiplier is decided ONCE from the whole-sample short-leg
// mean and then applied to every date, so the objective has a well-defined
// per-date series and a real t-stat rather than a per-date clipping artifact.
struct VrpVegaAgg {
  VrpStatAgg gross;
  VrpStatAgg cost;
  VrpStatAgg net;
  VrpStatAgg long_leg;
  VrpStatAgg short_leg;
  VrpStatAgg objective;
  double excess{std::numeric_limits<double>::quiet_NaN()}; // net - floor, paired
  double excess_t{std::numeric_limits<double>::quiet_NaN()};
  double excess_t_nw{std::numeric_limits<double>::quiet_NaN()};
  // The legs' own excesses over the same-direction zero-selection alternative:
  // long leg vs long-everything, short leg vs short-everything. This is what
  // separates leg SELECTION from leg BETA.
  double long_excess{std::numeric_limits<double>::quiet_NaN()};
  double long_excess_t_nw{std::numeric_limits<double>::quiet_NaN()};
  double short_excess{std::numeric_limits<double>::quiet_NaN()};
  double short_excess_t_nw{std::numeric_limits<double>::quiet_NaN()};
  double haircut{kVrpDefaultShortVegaHaircut};
  double short_multiplier{1.0};
  // ROUND 6: can this book be held at all? Reported for every column, because
  // "excess over the floor" is a claim about money and these are the claims
  // about implementability that a money number cannot make on its own.
  VrpStatAgg names_per_day;
  VrpStatAgg turnover; // fraction of GROSS replaced per formation date, [0, 1]
  double max_drawdown{std::numeric_limits<double>::quiet_NaN()};
  double floor_max_drawdown{std::numeric_limits<double>::quiet_NaN()};
  // Per-leg drawdown against the same-direction zero-selection alternative,
  // because a long-only reading of this book is a different instrument from
  // the paired one and must not borrow the pair's risk statistics.
  double long_max_drawdown{std::numeric_limits<double>::quiet_NaN()};
  double short_max_drawdown{std::numeric_limits<double>::quiet_NaN()};
  double floor_long_max_drawdown{std::numeric_limits<double>::quiet_NaN()};
  double floor_short_max_drawdown{std::numeric_limits<double>::quiet_NaN()};
};

namespace detail {

// Paired per-date difference of two aligned series, then aggregated.
[[nodiscard]] inline VrpStatAgg paired_diff(std::span<const double> a,
                                            std::span<const double> b) {
  if (a.size() != b.size()) {
    return VrpStatAgg{};
  }
  std::vector<double> d(a.size(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < d.size(); ++i) {
    d[i] = a[i] - b[i];
  }
  return vrp_aggregate_series(std::span<const double>{d}, kVrpOverlapLag);
}

} // namespace detail

[[nodiscard]] inline VrpVegaAgg vrp_vega_agg(const VrpVegaLegs &legs, const VrpVegaFloor &floor,
                                             double haircut) {
  VrpVegaAgg out;
  out.haircut = haircut;
  out.gross = vrp_aggregate_series(std::span<const double>{legs.gross}, kVrpOverlapLag);
  out.cost = vrp_aggregate_series(std::span<const double>{legs.cost}, kVrpOverlapLag);
  out.net = vrp_aggregate_series(std::span<const double>{legs.net}, kVrpOverlapLag);
  out.long_leg = vrp_aggregate_series(std::span<const double>{legs.long_leg}, kVrpOverlapLag);
  out.short_leg = vrp_aggregate_series(std::span<const double>{legs.short_leg}, kVrpOverlapLag);
  // Discount a positive short-vega edge; never discount a short-vega loss.
  out.short_multiplier = (out.short_leg.mean > 0.0) ? (1.0 - haircut) : 1.0;
  std::vector<double> obj(legs.net.size(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < obj.size(); ++i) {
    obj[i] = 0.5 * (legs.long_leg[i] + out.short_multiplier * legs.short_leg[i]);
  }
  out.objective = vrp_aggregate_series(std::span<const double>{obj}, kVrpOverlapLag);
  const VrpStatAgg ex =
      detail::paired_diff(std::span<const double>{legs.net},
                          std::span<const double>{floor.per_date_best});
  out.excess = ex.mean;
  out.excess_t = ex.t_iid;
  out.excess_t_nw = ex.t_nw;
  const VrpStatAgg lex =
      detail::paired_diff(std::span<const double>{legs.long_leg},
                          std::span<const double>{floor.per_date_long});
  out.long_excess = lex.mean;
  out.long_excess_t_nw = lex.t_nw;
  const VrpStatAgg sex =
      detail::paired_diff(std::span<const double>{legs.short_leg},
                          std::span<const double>{floor.per_date_short});
  out.short_excess = sex.mean;
  out.short_excess_t_nw = sex.t_nw;
  out.names_per_day = vrp_aggregate_series(std::span<const double>{legs.names}, kVrpOverlapLag);
  out.turnover = vrp_aggregate_series(std::span<const double>{legs.turnover}, kVrpOverlapLag);
  out.max_drawdown = detail::max_drawdown(std::span<const double>{legs.net});
  out.floor_max_drawdown = detail::max_drawdown(std::span<const double>{floor.per_date_best});
  out.long_max_drawdown = detail::max_drawdown(std::span<const double>{legs.long_leg});
  out.short_max_drawdown = detail::max_drawdown(std::span<const double>{legs.short_leg});
  out.floor_long_max_drawdown =
      detail::max_drawdown(std::span<const double>{floor.per_date_long});
  out.floor_short_max_drawdown =
      detail::max_drawdown(std::span<const double>{floor.per_date_short});
  return out;
}

// One column's whole vega statement on one row set: the decile book and the
// IV-quintile-neutralised book, each net of costs and each with its excess over
// the zero-selection vega floor.
struct VrpVegaReport {
  std::string name;
  VrpScoreKind kind{VrpScoreKind::Benchmark};
  VrpVegaAgg decile;
  VrpVegaAgg iv_neutral;
};

// IV-quintile-neutralised vega book: the variance block's method verbatim
// (5 quintiles by iv_fair_21d, rank inside each, long/short the top/bottom 20%,
// average the five), carried onto the vega P&L unit and net of the same costs.
[[nodiscard]] inline VrpVegaLegs
vrp_vega_iv_neutral_per_date(std::span<const std::int64_t> ts, std::span<const double> score,
                             std::span<const double> target, std::span<const double> iv,
                             std::span<const double> iv_fair, std::span<const std::size_t> sym,
                             double crossing, bool charge_costs) {
  VrpVegaLegs out;
  if (ts.size() != score.size() || ts.size() != target.size() || ts.size() != iv.size() ||
      ts.size() != iv_fair.size() || ts.size() != sym.size()) {
    return out;
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto empty_date = [&](VrpVegaLegs &o) {
    o.gross.push_back(nan);
    o.cost.push_back(nan);
    o.net.push_back(nan);
    o.long_leg.push_back(nan);
    o.short_leg.push_back(nan);
    o.names.push_back(nan);
    o.weights.emplace_back();
  };
  std::size_t begin = 0;
  while (begin < ts.size()) {
    std::size_t end = begin;
    while (end < ts.size() && ts[end] == ts[begin]) {
      ++end;
    }
    std::vector<std::array<double, 5>> rows; // (iv_fair, score, target, iv, symbol)
    rows.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(score[i]) && std::isfinite(target[i]) && std::isfinite(iv[i]) &&
          std::isfinite(iv_fair[i])) {
        rows.push_back({iv_fair[i], score[i], target[i], iv[i], static_cast<double>(sym[i])});
      }
    }
    begin = end;
    if (rows.size() < kVrpIvNeutralMinNames) {
      empty_date(out);
      continue;
    }
    std::sort(rows.begin(), rows.end());
    double lg = 0.0;
    double sh = 0.0;
    double lg_c = 0.0;
    double sh_c = 0.0;
    std::size_t q_n = 0;
    std::size_t n_long = 0;
    std::size_t n_short = 0;
    // Weights accumulate as (per-quintile 1/side) / q_n, so sum|w| == 1 across
    // the whole book; q_n is only known after the loop, hence the rescale.
    std::map<std::size_t, double> w;
    for (std::size_t q = 0; q < kVrpIvNeutralQuintiles; ++q) {
      const std::size_t lo = q * rows.size() / kVrpIvNeutralQuintiles;
      const std::size_t hi = (q + 1) * rows.size() / kVrpIvNeutralQuintiles;
      const std::size_t m = hi - lo;
      if (m < 2) {
        continue;
      }
      std::vector<std::array<double, 4>> in_q; // (score, target, iv, symbol)
      in_q.reserve(m);
      for (std::size_t i = lo; i < hi; ++i) {
        in_q.push_back({rows[i][1], rows[i][2], rows[i][3], rows[i][4]});
      }
      std::sort(in_q.begin(), in_q.end());
      const std::size_t side = std::max<std::size_t>(1, m / kVrpIvNeutralQuintiles);
      double top = 0.0;
      double bot = 0.0;
      double top_c = 0.0;
      double bot_c = 0.0;
      for (std::size_t k = 0; k < side; ++k) {
        bot += in_q[k][1];
        top += in_q[m - 1 - k][1];
        if (charge_costs) {
          bot_c += 2.0 * vrp_vega_cost_one_way(in_q[k][2], crossing);
          top_c += 2.0 * vrp_vega_cost_one_way(in_q[m - 1 - k][2], crossing);
        }
        detail::add_weight(w, static_cast<std::size_t>(in_q[m - 1 - k][3]),
                           0.5 / static_cast<double>(side));
        detail::add_weight(w, static_cast<std::size_t>(in_q[k][3]),
                           -0.5 / static_cast<double>(side));
      }
      n_long += side;
      n_short += side;
      const auto sd = static_cast<double>(side);
      lg += top / sd;
      sh += -bot / sd;
      lg_c += top_c / sd;
      sh_c += bot_c / sd;
      ++q_n;
    }
    if (q_n == 0) {
      empty_date(out);
      continue;
    }
    const auto qn = static_cast<double>(q_n);
    for (auto &[unused_sym, wv] : w) {
      (void)unused_sym;
      wv /= qn;
    }
    out.names.push_back(static_cast<double>(n_long + n_short));
    out.weights.push_back(std::move(w));
    out.long_leg.push_back((lg - lg_c) / qn);
    out.short_leg.push_back((sh - sh_c) / qn);
    out.gross.push_back(0.5 * (lg + sh) / qn);
    out.cost.push_back(0.5 * (lg_c + sh_c) / qn);
    out.net.push_back(0.5 * (lg + sh - lg_c - sh_c) / qn);
  }
  detail::fill_turnover(out);
  return out;
}

[[nodiscard]] inline VrpVegaReport
vrp_vega_report(std::string name, VrpScoreKind kind, std::span<const std::int64_t> ts,
                std::span<const double> score, std::span<const double> target,
                std::span<const double> iv, std::span<const double> iv_fair,
                std::span<const std::size_t> sym, const VrpVegaFloor &floor, double haircut,
                double crossing) {
  VrpVegaReport rep;
  rep.name = std::move(name);
  rep.kind = kind;
  rep.decile = vrp_vega_agg(vrp_vega_book_per_date(ts, score, target, iv, sym, crossing, true),
                            floor, haircut);
  rep.iv_neutral = vrp_vega_agg(
      vrp_vega_iv_neutral_per_date(ts, score, target, iv, iv_fair, sym, crossing, true), floor,
      haircut);
  return rep;
}

// ── The benchmark gate (round-4 F2, corrected in round 5) ───────────────────
//
// This exists because three rounds shipped on a +0.22 IC that no free rule was
// ever asked to beat. Round 4 built the gate and then graded it against the
// WRONG TARGET and one contaminated benchmark, so its verdict was void too.
// Round 5 keeps the machinery and corrects what it grades:
//
//   1. THE TARGET IS THE REALIZED LEG. Every pass condition is measured
//      against rv_fwd_21d. The composite label is still reported, labelled
//      contaminated, and never gated on.
//   2. `-iv_fair_21d` IS NOT A BENCHMARK (see kVrpScoreContamNegIv below).
//   3. MONEY IS A PASS CONDITION, not a footnote: the IV-quintile-neutralised
//      book, quoted as EXCESS OVER THE SHORT-EVERYTHING FLOOR.
//
// PASS requires all of the following, strictly and simultaneously, against
// EVERY surviving zero-parameter benchmark:
//   * mean per-date Pearson IC vs rv_fwd_21d (the Grinold sizing IC), and
//   * mean per-date Spearman IC vs rv_fwd_21d (the rank statement), and
//   * IV-neutralised carry in excess of the short-everything floor,
// plus the model's own excess over that floor must itself be positive -- a
// book that earns less per unit of gross vega than shorting the universe
// blind has not selected anything, whatever its IC says.
//
// Fail closed: a NaN anywhere is a FAIL, because an unmeasurable comparison is
// not a won one. Beating on rank while losing on level is the state that
// produced a book with rank skill and no currency edge, so no single-metric
// pass bar is offered.
struct VrpGateVerdict {
  bool pass{false};
  std::string model;          // the column on trial
  std::string best_benchmark; // benchmark with the highest rv_fwd_21d Spearman IC
  double model_ic_pearson{std::numeric_limits<double>::quiet_NaN()};
  double model_ic_spearman{std::numeric_limits<double>::quiet_NaN()};
  double best_benchmark_ic_pearson{std::numeric_limits<double>::quiet_NaN()};
  double best_benchmark_ic_spearman{std::numeric_limits<double>::quiet_NaN()};
  // IV-neutralised carry MINUS the short-everything floor, vol pts / 1u gross
  // vega / cycle. Never the raw carry: raw carry is mostly short-vol beta.
  double model_pnl_excess{std::numeric_limits<double>::quiet_NaN()};
  double best_benchmark_pnl_excess{std::numeric_limits<double>::quiet_NaN()};
  double pnl_floor{std::numeric_limits<double>::quiet_NaN()};
  std::size_t n_benchmarks{0};
};

namespace detail {

// The IV-neutralised excess of the named column, NaN when it was not scored.
[[nodiscard]] inline double pnl_excess_of(std::span<const VrpPnlReport> pnl,
                                          std::string_view name) noexcept {
  for (const VrpPnlReport &p : pnl) {
    if (p.name == name) {
      return p.iv_neutral.excess;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace detail

// `scores` holds exactly one VrpScoreKind::Model entry PER TARGET AXIS; only
// the rv_fwd_21d ones are gating. No model entry, no benchmark entry, or no
// money for either, is a FAIL -- an ungraded run must never read as a passing
// one. VrpScoreKind::Contaminated entries are skipped here BY CONSTRUCTION and
// cannot influence the verdict however well they score.
[[nodiscard]] inline VrpGateVerdict vrp_gate_verdict(std::span<const VrpScoreReport> scores,
                                                     std::span<const VrpPnlReport> pnl,
                                                     const VrpPnlFloor &floor) {
  VrpGateVerdict v;
  v.pnl_floor = floor.mean;
  const VrpScoreReport *model = nullptr;
  for (const VrpScoreReport &s : scores) {
    if (s.kind == VrpScoreKind::Model && s.target == VrpTargetAxis::RvFwd) {
      model = &s;
      break;
    }
  }
  if (model == nullptr) {
    return v;
  }
  v.model = model->name;
  v.model_ic_pearson = model->ic_pearson;
  v.model_ic_spearman = model->ic_spearman;
  v.model_pnl_excess = detail::pnl_excess_of(pnl, model->name);
  bool beats_all = true;
  for (const VrpScoreReport &s : scores) {
    if (s.kind != VrpScoreKind::Benchmark || s.target != VrpTargetAxis::RvFwd) {
      continue;
    }
    ++v.n_benchmarks;
    const double bench_excess = detail::pnl_excess_of(pnl, s.name);
    // Strict, and NaN-hostile: `!(a > b)` is true when either side is NaN.
    if (!(model->ic_pearson > s.ic_pearson) || !(model->ic_spearman > s.ic_spearman) ||
        !(v.model_pnl_excess > bench_excess)) {
      beats_all = false;
    }
    // "Best" = strongest rank benchmark, so the report names the one the model
    // most has to answer for. A measured benchmark always outranks an
    // unmeasurable one, whatever order they arrive in -- otherwise a leading
    // NaN would latch and the report would name the wrong bar.
    const bool first = v.best_benchmark.empty();
    const bool held_is_nan = std::isnan(v.best_benchmark_ic_spearman);
    const bool beats_held = s.ic_spearman > v.best_benchmark_ic_spearman;
    if (first || beats_held || (held_is_nan && !std::isnan(s.ic_spearman))) {
      v.best_benchmark = s.name;
      v.best_benchmark_ic_pearson = s.ic_pearson;
      v.best_benchmark_ic_spearman = s.ic_spearman;
      v.best_benchmark_pnl_excess = bench_excess;
    }
  }
  // The zero-selection floor is an absolute bar, not a relative one: beating
  // every benchmark while still earning less than shorting the universe blind
  // is not evidence of selection.
  v.pass = beats_all && v.n_benchmarks > 0 && v.model_pnl_excess > 0.0;
  return v;
}

// ── The VEGA gate (round 5) ─────────────────────────────────────────────────
//
// Identical in shape to the variance gate and deliberately no softer, because
// the whole point of round 5 is that a different instrument does not get a
// different standard of proof. PASS requires, against EVERY zero-parameter
// benchmark, simultaneously:
//   * mean per-date Pearson IC vs iv_chg_21d_roll, and
//   * mean per-date Spearman IC vs iv_chg_21d_roll, and
//   * IV-neutralised vega carry NET OF COSTS in excess of the zero-selection
//     vega floor,
// plus the model's own net excess over that floor must itself be positive.
// Fail closed on NaN, on a missing model, and on zero benchmarks.
//
// The gated axis is iv_chg_21d_roll, never iv_chg_21d_raw: the raw axis's
// strongest free predictor is the term-structure roll a real position pays.
struct VrpVegaGateVerdict {
  bool pass{false};
  std::string model;
  std::string best_benchmark;
  double model_ic_pearson{std::numeric_limits<double>::quiet_NaN()};
  double model_ic_spearman{std::numeric_limits<double>::quiet_NaN()};
  double best_benchmark_ic_pearson{std::numeric_limits<double>::quiet_NaN()};
  double best_benchmark_ic_spearman{std::numeric_limits<double>::quiet_NaN()};
  double model_pnl_excess{std::numeric_limits<double>::quiet_NaN()};
  double best_benchmark_pnl_excess{std::numeric_limits<double>::quiet_NaN()};
  double vega_floor{std::numeric_limits<double>::quiet_NaN()};
  bool vega_floor_is_long{true};
  std::size_t n_benchmarks{0};
};

namespace detail {

[[nodiscard]] inline double vega_excess_of(std::span<const VrpVegaReport> vega,
                                           std::string_view name) noexcept {
  for (const VrpVegaReport &v : vega) {
    if (v.name == name) {
      return v.iv_neutral.excess;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace detail

[[nodiscard]] inline VrpVegaGateVerdict
vrp_vega_gate_verdict(std::span<const VrpScoreReport> scores,
                      std::span<const VrpVegaReport> vega, const VrpVegaFloor &floor) {
  VrpVegaGateVerdict v;
  v.vega_floor = floor.best_is_long ? floor.long_all.mean : floor.short_all.mean;
  v.vega_floor_is_long = floor.best_is_long;
  const VrpScoreReport *model = nullptr;
  for (const VrpScoreReport &s : scores) {
    if (s.kind == VrpScoreKind::Model && s.target == VrpTargetAxis::IvChgRoll) {
      model = &s;
      break;
    }
  }
  if (model == nullptr) {
    return v;
  }
  v.model = model->name;
  v.model_ic_pearson = model->ic_pearson;
  v.model_ic_spearman = model->ic_spearman;
  v.model_pnl_excess = detail::vega_excess_of(vega, model->name);
  bool beats_all = true;
  for (const VrpScoreReport &s : scores) {
    if (s.kind != VrpScoreKind::Benchmark || s.target != VrpTargetAxis::IvChgRoll) {
      continue;
    }
    ++v.n_benchmarks;
    const double bench_excess = detail::vega_excess_of(vega, s.name);
    if (!(model->ic_pearson > s.ic_pearson) || !(model->ic_spearman > s.ic_spearman) ||
        !(v.model_pnl_excess > bench_excess)) {
      beats_all = false;
    }
    const bool first = v.best_benchmark.empty();
    const bool held_is_nan = std::isnan(v.best_benchmark_ic_spearman);
    const bool beats_held = s.ic_spearman > v.best_benchmark_ic_spearman;
    if (first || beats_held || (held_is_nan && !std::isnan(s.ic_spearman))) {
      v.best_benchmark = s.name;
      v.best_benchmark_ic_pearson = s.ic_pearson;
      v.best_benchmark_ic_spearman = s.ic_spearman;
      v.best_benchmark_pnl_excess = bench_excess;
    }
  }
  v.pass = beats_all && v.n_benchmarks > 0 && v.model_pnl_excess > 0.0;
  return v;
}

// The named score columns of the gate. `bench_*` entries are ZERO-PARAMETER:
// computable from the panel row alone, nothing fitted, no seed, no folds.
//   bench_hv_iv_gap = f5_hv_iv_gap, the Goyal-Saretto (2009) HV-IV classic.
//     Positive gap = realized above implied = long vol pays, same sign as the
//     label. KEPT, and kept at full strength: it is only PARTIALLY exposed to
//     the contamination below (+0.0730 t_nw +1.95 against rv_fwd_21d on SP100,
//     +5.00 vol pts IV-neutralised at t_nw +4.88, decile rho +0.539) and on
//     SP100 it currently BEATS the model on every honest axis, including being
//     the only axis in the set that clears the short-everything floor. A
//     benchmark is not weakened to flatter a model. Note the authors' own 2025
//     retraction (RV-IV 2.87% raw -> 0.34% alpha): it is here as a bar to
//     clear, not as an endorsement.
//
// ── DELETED AS A BENCHMARK: -iv_fair_21d. DO NOT REINSTATE IT. ──────────────
//
// Round 4 scored `-iv_fair_21d` as a zero-parameter benchmark, it posted a
// +0.4225 rank IC against the composite label, and the gate concluded "the free
// rule beats the GBT by 12x, do not ship". That number is an ALGEBRAIC
// IDENTITY, not a measurement:
//
//     label = (rv_fwd_21d^2 - iv_fair_21d^2) * 21/252
//
// so iv_fair_21d sits INSIDE the label with a negative sign, and
// load_vrp_panel enforces iv_fair_21d > 0 on every labeled row. On that domain
// x -> -x^2 is strictly monotone, so -iv_fair_21d is a PERFECT rank transform
// of the label's implied leg: rank IC exactly +1.0000, on any dataset, in any
// regime, forever. Nothing measured; nothing that can fail to be true.
//
// Against the leg that is actually forecastable it is a strong ANTI-forecaster:
// rank IC -0.6128 (t_nw -22.93) vs rv_fwd_21d on SP100 and -0.7870
// (t_nw -53.28) on clean25. The published +0.4225 is a +1.0000 identity blended
// with a -0.6128 anti-forecast. There is zero forecasting content in it.
//
// The FORM is named here, not just the column, because the defect generalises:
// ANY score shaped (variance_forecast - iv_fair^2) inherits the free component.
// A zero-parameter, zero-skill (rv_trail^2 - iv^2)*H scores +0.3501 against the
// composite label, and `baseline_log_har` -- the same functional form, fitted --
// scores +0.4124, statistically indistinguishable from the "benchmark" it was
// supposed to be independent of. Decisive: the best free forecast of realized
// volatility, rv_trail_21d^2 (genuine +0.5177 against rv_fwd_21d), scores
// -0.1240 against the composite label. THE LABEL'S RANK ORDERING IS
// ANTI-CORRELATED WITH FORECASTING SKILL, which is why the target moved to the
// realized leg and why no future column may be admitted as a benchmark on a
// composite-label score alone.
//
// It survives as VrpScoreKind::Contaminated -- scored, published, and
// structurally incapable of deciding a verdict -- because deleting the number
// would delete the evidence. Full derivation:
// .superpowers/sdd/2026-08-15-vrp-ml/audit-benchmark-contamination.md
inline constexpr std::string_view kVrpScoreModel = "gbt";
inline constexpr std::string_view kVrpScoreBaseline = "baseline_log_har";
inline constexpr std::string_view kVrpScoreBenchHvIv = "bench_hv_iv_gap";
inline constexpr std::string_view kVrpScoreRanked = "ranked_pred_edge_norm";
inline constexpr std::string_view kVrpScoreContamNegIv = "contaminated_neg_iv_fair_21d";
// ── ROUND-5 additions: the free rules the VEGA book has to beat ─────────────
//
//   bench_neg_iv_atmf_21d = -iv_atmf_21d[t]. Short high implied, long low
//     implied: the zero-parameter cross-sectional vol MEAN-REVERSION rule, and
//     the natural free bar for any forecast of iv change. It is NOT a
//     contaminated column in round 4's sense -- it is a perfect rank transform
//     of the iv-change target's KNOWN leg (rank IC +1.0000 against
//     E_t[iv_fwd] - iv_t by algebra, exactly as -iv_fair was against the old
//     label's implied leg), but against the target ITSELF it scores a MEASURED
//     +0.2537 that would be zero under a cross-sectional random walk in
//     implied vol and is negative (-0.7078) against the forecastable leg. That
//     is mean reversion, which can be wrong, not an identity, which cannot.
//     READ THE EIV CAVEAT ON kVrpScoreContamIvSlope BEFORE TRUSTING ITS SIZE.
//   bench_term_slope = iv_fair_63d - iv_fair_21d, the Vasquez (JFQA 2017)
//     term-structure slope. Zero-parameter and, on the roll-adjusted axis,
//     the strongest honest free rule measured here.
//
// ── CONTAMINATED ON THE IV-CHANGE AXES: iv_fair_63d - iv_atmf_21d ──────────
// It contains -iv_atmf_21d[t], the iv-change target's own entry mark, so it
// inherits that mark's measurement error with the same sign the target does.
// MEASURED: read at t its rank IC against iv_chg_raw is +0.5202 (t_nw +19.63);
// read at t-2, with only the surface read changed, it falls to +0.3203. The
// same test on -iv_atmf_21d halves it (+0.2537 -> +0.1182), and rebuilding the
// TARGET with a t-2 entry leg moves the advantage across to -iv_atmf_21d[t-2]
// (+0.2535) from -iv_atmf_21d[t] (+0.1068). Whichever surface read sits inside
// the target is the one that scores: that is Duarte-Jones-Wang errors-in-
// variables, not forecasting, and it is a property of THE TARGET.
inline constexpr std::string_view kVrpScoreBenchNegIvAtmf = "bench_neg_iv_atmf_21d";
inline constexpr std::string_view kVrpScoreBenchTermSlope = "bench_term_slope";
inline constexpr std::string_view kVrpScoreContamIvSlope =
    "contaminated_iv63_minus_iv_atmf21";
// gbt, baseline_log_har, bench_hv_iv_gap, ranked_pred_edge_norm, the round-4
// contaminated appendix, and the three round-5 iv-change columns -- the fixed
// column set every fold and the pooled row set are graded on.
inline constexpr std::size_t kVrpGateColumnCount = 8;

// The gate's whole finding for one run: every score column on every fold and
// on the pooled OOS rows, plus the verdict, plus the corpus label. The corpus
// is carried because the widely quoted +0.22 IC was a clean-25 number while
// every traded config ran on SP100 and no artifact recorded the difference.
// One fold's scored columns, carrying their own fold id. A struct rather than
// a second vector kept parallel to the first: a fold whose id lives somewhere
// else is a state that can go wrong, and mislabelled per-fold ICs are one of
// the breaks this gate exists to prevent.
struct VrpGateFold {
  std::uint32_t fold_id{0};
  std::vector<VrpScoreReport> scores; // kVrpGateColumnCount columns x 5 axes
  std::vector<VrpPnlReport> pnl;      // one per column, same column order
  std::vector<VrpVegaReport> vega;    // one per column, same column order
  VrpPnlFloor floor;                  // this fold's own short-everything floor
  VrpVegaFloor vega_floor;            // this fold's own zero-selection vega bar
};

struct VrpGateReport {
  std::string corpus;
  std::vector<VrpGateFold> per_fold;
  std::vector<VrpScoreReport> pooled; // same column/axis order as each fold
  std::vector<VrpPnlReport> pooled_pnl;
  std::vector<VrpVegaReport> pooled_vega;
  VrpPnlFloor pooled_floor;
  VrpVegaFloor pooled_vega_floor;
  VrpGateVerdict verdict;          // computed on `pooled` + `pooled_pnl`
  VrpVegaGateVerdict vega_verdict; // computed on `pooled` + `pooled_vega`
  // Round-5 iv-change coverage: rows whose exit mark exists at exactly the
  // horizon, and rows that lost the roll leg with the 63d strip.
  std::size_t n_iv_chg_rows{0};
  std::size_t n_iv_chg_rows_no_exit{0};
  std::size_t n_iv_chg_rows_roll_undefined{0};
  double short_vega_haircut{kVrpDefaultShortVegaHaircut};
  // ROUND 6: the cost parameterisation and the EIV target this run graded on,
  // both stamped into the artifact because a cost figure whose crossing
  // fraction is not published is not a reproducible cost figure.
  double cost_crossing_fraction{kVrpDefaultCrossingFraction};
  std::size_t eiv_target_entry_lag{0};
  // The data defect the P&L block has to survive: unadjusted-split rows whose
  // raw carry runs to +16465 vol points. Published, not hidden.
  std::size_t n_ppv_rows_priced{0};
  std::size_t n_ppv_rows_winsorized{0};
  // Coverage honesty: signal rows with no realized label were TRADED in
  // rounds 1-3 and reported as if validated (27% of the round-2 run).
  std::size_t n_signal_rows{0};
  std::size_t n_signal_rows_unlabeled{0};
  [[nodiscard]] double frac_unlabeled() const noexcept {
    return n_signal_rows == 0 ? std::numeric_limits<double>::quiet_NaN()
                              : static_cast<double>(n_signal_rows_unlabeled) /
                                    static_cast<double>(n_signal_rows);
  }
};

// ── Grading one row set: every column, every target axis, plus the money ─────

// The three grading targets and the two P&L inputs on one row set, all
// parallel to `ts` and all TARGET-SIDE: none of them is ever feature-lagged.
struct VrpGradeRows {
  std::vector<std::int64_t> ts;
  std::vector<double> rv_fwd;  // primary target axis + P&L input
  std::vector<double> vol_chg; // ln(rv_fwd_21d / rv_trail_21d)
  std::vector<double> label;   // composite, CONTAMINATED
  std::vector<double> iv_fair; // P&L input (straddle vega denominator)
  // Round 5: the tradeable vega axes and the ATM-forward mark the vega book
  // enters at (and pays its spread on).
  std::vector<double> iv_chg_raw;
  std::vector<double> iv_chg_roll;
  std::vector<double> iv_atmf;
  // ROUND 6: the panel symbol index of each row, so a book can be differenced
  // against the previous formation date and report its own turnover.
  std::vector<std::size_t> symbol;
};

// One column to grade. `score` is a non-owning view over storage the caller
// keeps alive for the duration of the vrp_grade call.
struct VrpGradeColumn {
  std::string name;
  VrpScoreKind kind{VrpScoreKind::Benchmark};
  std::span<const double> score;
  bool label_units{false};
};

// The gate's whole statement about one row set.
struct VrpGraded {
  std::vector<VrpScoreReport> scores; // column-major: one per axis, per column
  std::vector<VrpPnlReport> pnl;      // variance carry, one per column
  std::vector<VrpVegaReport> vega;    // vega book on iv_chg_roll, one per column
  VrpPnlFloor floor;
  VrpVegaFloor vega_floor;
  std::size_t n_ppv_rows_priced{0};
  std::size_t n_ppv_rows_winsorized{0};
};

// The canonical target-axis order every artifact key and every reader relies
// on. rv_fwd_21d FIRST because it is the one that gates the variance book;
// the two iv-change axes are APPENDED so every round-4/5 key keeps its place.
inline constexpr std::array<VrpTargetAxis, 5> kVrpTargetAxes{
    VrpTargetAxis::RvFwd, VrpTargetAxis::VolChg, VrpTargetAxis::Label,
    VrpTargetAxis::IvChgRaw, VrpTargetAxis::IvChgRoll};

[[nodiscard]] inline VrpGraded vrp_grade(const VrpGradeRows &rows,
                                         std::span<const VrpGradeColumn> cols,
                                         double short_vega_haircut = kVrpDefaultShortVegaHaircut,
                                         double crossing = kVrpDefaultCrossingFraction) {
  VrpGraded out;
  const std::span<const std::int64_t> ts{rows.ts};
  const std::span<const std::size_t> sym{rows.symbol};
  const VrpPpvSeries ppv = vrp_build_ppv(std::span<const double>{rows.rv_fwd},
                                         std::span<const double>{rows.iv_fair});
  out.n_ppv_rows_priced = ppv.n_priced;
  out.n_ppv_rows_winsorized = ppv.n_winsorized;
  out.floor = vrp_short_everything_floor(ts, std::span<const double>{ppv.ppv});
  // The vega book is graded on iv_chg_roll -- the holdable axis -- because
  // iv_chg_raw's strongest free predictor is the term-structure roll itself.
  out.vega_floor = vrp_vega_floor(ts, std::span<const double>{rows.iv_chg_roll},
                                  std::span<const double>{rows.iv_atmf}, crossing, true);
  out.scores.reserve(cols.size() * kVrpTargetAxes.size());
  out.pnl.reserve(cols.size());
  out.vega.reserve(cols.size());
  for (const VrpGradeColumn &c : cols) {
    for (const VrpTargetAxis axis : kVrpTargetAxes) {
      std::span<const double> target;
      switch (axis) {
      case VrpTargetAxis::RvFwd:
        target = std::span<const double>{rows.rv_fwd};
        break;
      case VrpTargetAxis::VolChg:
        target = std::span<const double>{rows.vol_chg};
        break;
      case VrpTargetAxis::Label:
        target = std::span<const double>{rows.label};
        break;
      case VrpTargetAxis::IvChgRaw:
        target = std::span<const double>{rows.iv_chg_raw};
        break;
      case VrpTargetAxis::IvChgRoll:
        target = std::span<const double>{rows.iv_chg_roll};
        break;
      }
      out.scores.push_back(
          vrp_score_report(c.name, c.kind, axis, ts, c.score, target, c.label_units));
    }
    out.pnl.push_back(vrp_pnl_report(c.name, c.kind, ts, c.score,
                                     std::span<const double>{ppv.ppv},
                                     std::span<const double>{rows.iv_fair}, out.floor));
    out.vega.push_back(vrp_vega_report(
        c.name, c.kind, ts, c.score, std::span<const double>{rows.iv_chg_roll},
        std::span<const double>{rows.iv_atmf}, std::span<const double>{rows.iv_fair}, sym,
        out.vega_floor, short_vega_haircut, crossing));
  }
  return out;
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
  // Reject-and-count cap on each row's label-window span in POOLED sessions
  // (build_vrp_observations; the CLI's --max-label-span). Bounds the
  // embargo, which bounds purge/embargo attrition.
  std::size_t max_label_span_sessions{kVrpDefaultMaxLabelSpanSessions};
  // Round-3 level recalibration (--recalibrate; default Off keeps the
  // signal/model/sidecar artifacts byte-identical to the fix-2 trainer).
  VrpRecalMode recalibrate{VrpRecalMode::Off};
  // Trailing admitted-train calibration window in sessions (--recalib-window;
  // capped per fold at half the fold's admitted train sessions, and the
  // effective value is reported per fold). Must be >= 1 under Isotonic.
  std::size_t recalib_window_sessions{kVrpDefaultRecalibWindowSessions};
  // Baseline log-target retransformation (--retransform; trainer-side only,
  // the sidecar score-from-files contract stays jensen-based).
  VrpRetransformMode retransform{VrpRetransformMode::Jensen};
  // ROUND-4 F1 (--edge-norm): the basis pred_edge_norm standardizes on.
  // CrossSection is the DEFAULT and changes the VALUES in that column;
  // PerSymbol reproduces the round-3 artifacts byte for byte. See
  // VrpEdgeNormMode for the measured swing this default is worth.
  VrpEdgeNormMode edge_norm{VrpEdgeNormMode::CrossSection};
  // ROUND-4 F4 (--feature-lag): read features from the row's `lag`-th
  // same-symbol predecessor. 0 = the round-1..3 behaviour (reproducible for
  // regression); 2 is the recommended round-4 setting.
  std::size_t feature_lag{kVrpDefaultFeatureLag};
  // ROUND-4 F3 (--corpus): the corpus label stamped into the metrics output.
  // Empty => derived from the panel file stem. Exists because the widely
  // quoted +0.22 IC came from clean-25 while every traded config ran on SP100
  // (audit S3 break 1) and nothing in the artifacts said so.
  std::string corpus;
  // ROUND-5 (--short-vega-haircut): the asymmetric objective's discount on a
  // POSITIVE short-vega edge. See kVrpDefaultShortVegaHaircut -- 0.50 is a
  // stated judgement, and 0 reproduces the undiscounted book.
  double short_vega_haircut{kVrpDefaultShortVegaHaircut};
  // ROUND-6 (--cost-crossing-fraction): the fraction of the derived QUOTED
  // option spread the book actually crosses. The default returns the measured
  // Christoffersen effective spread bit-exactly; see the block above
  // kVrpVegaEffectiveOneWayFracOfPremium for why applying the ORATS 0.53 to
  // an already-effective number would have been a free 47% cost cut.
  double cost_crossing_fraction{kVrpDefaultCrossingFraction};
  // ROUND-6 (--eiv-target-entry-lag): rebuild the iv-change TARGET with its
  // ENTRY leg read from the row's `n`-th same-symbol predecessor, exit leg
  // unmoved. 0 is the round-5 target. This is round 5's decisive
  // errors-in-variables swap made runnable: whichever surface read sits inside
  // the target is the one that scores, so a predictor whose edge is real must
  // keep it when the target's entry read moves away from the predictor's.
  std::size_t eiv_target_entry_lag{0};
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
  // Round-3 metrics honesty + isotonic recalibration (digest Q4): Mincer-
  // Zarnowitz level diagnostics of the raw GBT always, and of the
  // recalibrated forecast when cfg.recalibrate is on; the raw/recalibrated
  // per-test-row label forecasts (parallel to test_rows; recal == raw when
  // recalibration is off or not applied); the isotonic fit accounting --
  // recal_fit_rows are ADMITTED TRAIN rows from the trailing
  // recal_window_effective train sessions, strictly before the fold's test
  // start by the plan's own purge (pinned by test).
  double mz_slope_raw{std::numeric_limits<double>::quiet_NaN()};
  double mz_intercept_raw{std::numeric_limits<double>::quiet_NaN()};
  double mz_slope_recal{std::numeric_limits<double>::quiet_NaN()};
  double mz_intercept_recal{std::numeric_limits<double>::quiet_NaN()};
  double qlike_gbt_recal{std::numeric_limits<double>::quiet_NaN()};
  double ic_gbt_recal{std::numeric_limits<double>::quiet_NaN()};
  double smear_factor{1.0}; // baseline Duan factor (computed in every mode)
  bool recal_applied{false};
  std::size_t recal_window_effective{0};
  std::size_t recal_n_fit{0};
  std::vector<std::size_t> recal_fit_rows; // panel rows the isotonic fit used
  std::vector<double> test_pred_raw;       // raw GBT label forecast per test row
  std::vector<double> test_pred_recal;     // recalibrated (== raw when off)
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
  // ROUND-4: the benchmark gate's finding for this run, and the per-symbol
  // warmup rows the feature lag could not fill (0 at lag 0).
  VrpGateReport gate;
  std::size_t feature_lag_rows_unavailable{0};
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
  double smear_factor{1.0};    // Duan smearing factor: mean(exp(resid))
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

  // Train residual variance in log space (for the exp(s2/2) retransform)
  // plus the Duan smearing factor mean(exp(resid)) over the same residuals
  // (the --retransform smearing alternative). Predictions omit the mean
  // (no-intercept kernel over zero-mean columns), so the residual is
  // measured against pred + ybar.
  double sq = 0.0;
  std::vector<double> resids(train_rows.size(), 0.0);
  std::array<double, 3> base{};
  for (std::size_t i = 0; i < train_rows.size(); ++i) {
    for (std::size_t j = 0; j < kBaselineFeatures.size(); ++j) {
      base[j] = standardized_feature(panel, stz, train_rows[i], kBaselineFeatures[j]);
    }
    const double pred = predict_model(fit.model, std::span<const double>{base});
    const double resid = y_log[i] - (pred + fit.train_mean_log);
    resids[i] = resid;
    sq += resid * resid;
  }
  fit.s2 = sq / n;
  fit.smear_factor = vrp_smearing_factor(std::span<const double>{resids});
  return fit;
}

// The clipped baseline VARIANCE forecast for one panel row, retransformed
// per `mode`: Jensen exp(s2/2) (round-1 path, the sidecar contract) or the
// Duan smearing factor (--retransform smearing; digest [16][17]).
[[nodiscard]] inline double baseline_forecast_var(const VrpPanel &panel,
                                                  const VrpStandardization &stz,
                                                  const BaselineFit &fit, std::size_t row,
                                                  VrpRetransformMode mode) {
  std::array<double, 3> base{};
  for (std::size_t j = 0; j < kBaselineFeatures.size(); ++j) {
    base[j] = standardized_feature(panel, stz, row, kBaselineFeatures[j]);
  }
  const double pred = predict_model(fit.model, std::span<const double>{base});
  const double mu = pred + fit.train_mean_log;
  if (mode == VrpRetransformMode::Smearing) {
    return vrp_smearing_retransform_clip(mu, fit.smear_factor, fit.train_var_min,
                                         fit.train_var_max);
  }
  return vrp_retransform_clip(mu, fit.s2, fit.train_var_min, fit.train_var_max);
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

// ROUND-4 F1: rewrite every entry's pred_edge_norm as the WITHIN-DATE
// cross-sectional z-score of pred_label, across the symbols scored on that
// date. Applied as a final pass over the completed, panel-row-sorted entry
// list, so fold test rows and the NaN-label tail rows are standardized by the
// same rule and no date is split across two conventions.
//
// Why this is the default (audit-gross-negative S1/S4, measured): the
// round-1..3 per-symbol z-score (pred_label - label_mean[sym]) / label_sd[sym]
// SUBTRACTS each name's own average variance premium -- the persistent
// cross-sectional VRP that the book exists to harvest -- and then divides by a
// per-name label_sd spanning three orders of magnitude, which reorders the
// cross-section rather than rescaling it. Equal-weighted decile long/short,
// same signal file, same dates, same 21d horizon: per-symbol -1.63 vol
// pts/cycle (29% of phase offsets positive), pred_label +1.74 (76% positive).
//
// Within a date this is an affine map with a POSITIVE scale, so it is exactly
// order-preserving on pred_label -- the book then ranks on the axis the IC is
// measured on, which is the whole point. Degenerate dates (fewer than two
// finite forecasts, or zero cross-sectional dispersion) emit 0.0, matching the
// existing sd == 0 convention of the per-symbol path; a non-finite pred_label
// emits 0.0 for the same reason. The frozen vrp_signal_v1 loader fail-closes
// on a non-finite column, so this pass must never introduce one.
//
// `entries` arrives sorted by panel_row, i.e. canonical (entry_ts_ns, symbol)
// order, so equal-timestamp runs are contiguous and one linear scan suffices.
inline void apply_cross_section_edge_norm(const VrpPanel &panel,
                                          std::span<SignalEntry> entries) {
  std::size_t begin = 0;
  while (begin < entries.size()) {
    const std::int64_t ts = panel.rows[entries[begin].panel_row].entry_ts_ns;
    std::size_t end = begin;
    while (end < entries.size() && panel.rows[entries[end].panel_row].entry_ts_ns == ts) {
      ++end;
    }
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = begin; i < end; ++i) {
      if (std::isfinite(entries[i].pred_label)) {
        sum += entries[i].pred_label;
        ++n;
      }
    }
    double mean = 0.0;
    double sd = 0.0;
    if (n >= 2) {
      mean = sum / static_cast<double>(n);
      double sq = 0.0;
      for (std::size_t i = begin; i < end; ++i) {
        if (std::isfinite(entries[i].pred_label)) {
          const double d = entries[i].pred_label - mean;
          sq += d * d;
        }
      }
      sd = std::sqrt(sq / static_cast<double>(n)); // population, matches LabelStats
    }
    for (std::size_t i = begin; i < end; ++i) {
      const double p = entries[i].pred_label;
      entries[i].pred_edge_norm =
          (sd > 0.0 && std::isfinite(p)) ? (p - mean) / sd : 0.0;
    }
    begin = end;
  }
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

// One score column's statistics on ONE target axis, as
// `# <prefix><name>_<axis>_<stat>=<value>` meta lines. Meta lines are the
// sanctioned additive surface: the tabular block below keeps its round-1
// shape, so a round-3 reader is unaffected.
//
// The axis token is NOT optional and is not a suffix that may be dropped. A
// bare `..._ic_spearman` is precisely the key that let a +1.0000 algebraic
// identity against the label's own implied leg be read as a forecast for a
// whole round.
inline void append_score_meta(std::string &body, const std::string &prefix,
                              const VrpScoreReport &s) {
  const std::string p =
      prefix + s.name + "_" + std::string{vrp_target_axis_key(s.target)} + "_";
  const auto num = [&](std::string_view stat, double v) {
    body += p;
    body += stat;
    body += '=';
    body += fmt_double(v);
    body += '\n';
  };
  num("ic_pearson", s.ic_pearson);
  num("ic_pearson_t", s.ic_pearson_t);
  num("ic_pearson_t_nw", s.ic_pearson_t_nw);
  num("ic_spearman", s.ic_spearman);
  num("ic_spearman_t", s.ic_spearman_t);
  num("ic_spearman_t_nw", s.ic_spearman_t_nw);
  num("ic_pearson_pooled_rows", s.ic_pearson_pooled);
  num("ic_spearman_pooled_rows", s.ic_spearman_pooled);
  num("ic_pearson_traded", s.ic_pearson_traded);
  num("ic_pearson_traded_t", s.ic_pearson_traded_t);
  num("ic_pearson_traded_t_nw", s.ic_pearson_traded_t_nw);
  num("decile_spread", s.decile_spread);
  num("decile_spread_t", s.decile_spread_t);
  num("decile_spread_t_nw", s.decile_spread_t_nw);
  num("decile_rho", s.decile_rho);
  num("mse", s.mse);
  num("mz_slope", s.mz_slope);
  num("mz_intercept", s.mz_intercept);
  body += p + "n_dates=" + std::to_string(s.n_dates) + "\n";
  body += p + "n_rows=" + std::to_string(s.n_rows) + "\n";
}

// One book's money, in vol points per 1u GROSS VEGA per cycle. `excess_over_
// floor` is emitted on the SAME key stem as the raw figure and immediately
// after it, so no reader can quote one without seeing the other: short-vol
// beta was mistaken for selection skill three times, and the pairing is the
// structural fix.
inline void append_pnl_book_meta(std::string &body, const std::string &stem,
                                 const VrpPnlAgg &a) {
  const auto num = [&](std::string_view stat, double v) {
    body += stem;
    body += stat;
    body += '=';
    body += fmt_double(v);
    body += '\n';
  };
  num("vol_pts_gross_vega", a.mean);
  num("vol_pts_gross_vega_t", a.t_iid);
  num("vol_pts_gross_vega_t_nw", a.t_nw);
  num("excess_over_floor", a.excess);
  num("excess_over_floor_t", a.excess_t);
  num("excess_over_floor_t_nw", a.excess_t_nw);
  body += stem + "n_dates=" + std::to_string(a.n_dates) + "\n";
}

inline void append_pnl_meta(std::string &body, const std::string &prefix,
                            const VrpPnlReport &p) {
  append_pnl_book_meta(body, prefix + p.name + "_decile_", p.decile);
  append_pnl_book_meta(body, prefix + p.name + "_iv_neutral_", p.iv_neutral);
}

inline void append_pnl_floor_meta(std::string &body, const std::string &prefix,
                                  const VrpPnlFloor &f) {
  const std::string stem = prefix + "floor_short_everything_";
  body += stem + "vol_pts_gross_vega=" + fmt_double(f.mean) + "\n";
  body += stem + "vol_pts_gross_vega_t=" + fmt_double(f.t_iid) + "\n";
  body += stem + "vol_pts_gross_vega_t_nw=" + fmt_double(f.t_nw) + "\n";
  body += stem + "n_dates=" + std::to_string(f.n_dates) + "\n";
}

// One VEGA book, in vol points per 1u GROSS VEGA per cycle on the
// iv_chg_21d_roll axis. gross / cost / net are emitted CONSECUTIVELY and the
// excess over the zero-selection vega floor sits immediately after net, for the
// same reason the variance block pairs them: a gross figure quoted alone is not
// a claim about anything. The leg split is emitted with its own t-stats and its
// own same-direction excess, because a net number can hide a dead long leg.
inline void append_vega_book_meta(std::string &body, const std::string &stem,
                                  const VrpVegaAgg &a) {
  const auto num = [&](std::string_view stat, double v) {
    body += stem;
    body += stat;
    body += '=';
    body += fmt_double(v);
    body += '\n';
  };
  const auto agg = [&](std::string_view stat, const VrpStatAgg &s) {
    num(stat, s.mean);
    num(std::string{stat} + "_t", s.t_iid);
    num(std::string{stat} + "_t_nw", s.t_nw);
  };
  agg("gross_vol_pts", a.gross);
  agg("cost_vol_pts", a.cost);
  agg("net_vol_pts", a.net);
  num("excess_over_vega_floor", a.excess);
  num("excess_over_vega_floor_t", a.excess_t);
  num("excess_over_vega_floor_t_nw", a.excess_t_nw);
  agg("long_leg_net_vol_pts", a.long_leg);
  num("long_leg_excess_over_long_everything", a.long_excess);
  num("long_leg_excess_over_long_everything_t_nw", a.long_excess_t_nw);
  agg("short_leg_net_vol_pts", a.short_leg);
  num("short_leg_excess_over_short_everything", a.short_excess);
  num("short_leg_excess_over_short_everything_t_nw", a.short_excess_t_nw);
  num("short_vega_haircut", a.haircut);
  num("short_leg_multiplier_applied", a.short_multiplier);
  agg("haircut_objective_vol_pts", a.objective);
  // ROUND 6 IMPLEMENTABILITY. maxDD is a PATH statistic -- it has no per-date
  // series, so it gets no t-stat and is not given a fabricated one; the floor's
  // own maxDD is emitted beside it so the number can be read as a comparison
  // rather than as an absolute.
  agg("names_per_day", a.names_per_day);
  agg("turnover_frac_of_gross", a.turnover);
  num("max_drawdown_vol_pts", a.max_drawdown);
  num("floor_max_drawdown_vol_pts", a.floor_max_drawdown);
  num("long_leg_max_drawdown_vol_pts", a.long_max_drawdown);
  num("short_leg_max_drawdown_vol_pts", a.short_max_drawdown);
  num("floor_long_everything_max_drawdown_vol_pts", a.floor_long_max_drawdown);
  num("floor_short_everything_max_drawdown_vol_pts", a.floor_short_max_drawdown);
  body += stem + "n_dates=" + std::to_string(a.net.n) + "\n";
}

inline void append_vega_meta(std::string &body, const std::string &prefix,
                             const VrpVegaReport &v) {
  append_vega_book_meta(body, prefix + v.name + "_vega_decile_", v.decile);
  append_vega_book_meta(body, prefix + v.name + "_vega_iv_neutral_", v.iv_neutral);
}

inline void append_vega_floor_meta(std::string &body, const std::string &prefix,
                                   const VrpVegaFloor &f) {
  const auto one = [&](std::string_view which, const VrpStatAgg &s) {
    const std::string stem = prefix + "vega_floor_" + std::string{which} + "_everything_";
    body += stem + "net_vol_pts=" + fmt_double(s.mean) + "\n";
    body += stem + "net_vol_pts_t=" + fmt_double(s.t_iid) + "\n";
    body += stem + "net_vol_pts_t_nw=" + fmt_double(s.t_nw) + "\n";
    body += stem + "n_dates=" + std::to_string(s.n) + "\n";
  };
  one("long", f.long_all);
  one("short", f.short_all);
  body += prefix + "vega_floor_binding=" +
          (f.best_is_long ? "long_everything" : "short_everything") + "\n";
}

[[nodiscard]] inline Status write_metrics_file(std::span<const VrpFoldMetrics> folds,
                                               const VrpObservations &observations,
                                               const VrpTrainConfig &cfg,
                                               const VrpGateReport &gate,
                                               std::size_t feature_lag_unavailable,
                                               const std::filesystem::path &path) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  if (!out) {
    return Err(ErrorCode::IoError,
               "write_metrics_file: cannot open '" + path.string() + "' for writing");
  }
  const bool recal_on = cfg.recalibrate == VrpRecalMode::Isotonic;
  std::string body = "# schema=vrp_train_metrics_v1\n";
  // Rejection accounting as meta lines (columns below stay round-1 shaped).
  body += "# n_labeled_rows=" + std::to_string(observations.n_labeled_rows) + "\n";
  body += "# n_rows_rejected_no_t21=" + std::to_string(observations.n_rows_rejected_no_t21) +
          "\n";
  body += "# n_rows_rejected_span_cap=" +
          std::to_string(observations.n_rows_rejected_span_cap) + "\n";
  body += "# n_symbols_fully_rejected=" +
          std::to_string(observations.n_symbols_fully_rejected) + "\n";
  // Round-3 run-level mode lines (value semantics documented in CHANGELOG).
  body += std::string("# recalibrate=") + (recal_on ? "isotonic" : "off") + "\n";
  if (recal_on) {
    body += "# recalib_window=" + std::to_string(cfg.recalib_window_sessions) + "\n";
  }
  body += std::string("# retransform=") +
          (cfg.retransform == VrpRetransformMode::Smearing ? "smearing" : "jensen") + "\n";
  // ── Round-4 run-level mode + honesty lines ────────────────────────────────
  body += std::string("# edge_norm=") +
          (cfg.edge_norm == VrpEdgeNormMode::PerSymbol ? "per_symbol" : "cross_section") + "\n";
  body += "# feature_lag=" + std::to_string(cfg.feature_lag) + "\n";
  body += "# feature_lag_rows_unavailable=" + std::to_string(feature_lag_unavailable) + "\n";
  // The corpus the numbers below belong to. Round 1-3 quoted a clean-25 IC
  // beside an SP100 book and nothing in the artifacts made that visible.
  body += "# corpus=" + gate.corpus + "\n";
  // QLIKE is a VARIANCE loss: L(F,P) = P/F - ln(P/F) - 1 needs F > 0 and P > 0.
  // The label is a SIGNED variance spread, negative about half the time, which
  // is what produced the round-1 1e6..3e7 readings. The round-4 gate scores MSE
  // and Mincer-Zarnowitz instead and never reads the legacy column below; it is
  // retained only so round-3 artifacts stay comparable.
  body += "# qlike_status=deprecated_undefined_on_signed_label_not_scored_by_gate\n";
  // Coverage: signal rows whose label never realized were traded in rounds 1-3
  // and reported as if out-of-sample validated (27% of the round-2 run).
  body += "# n_signal_rows=" + std::to_string(gate.n_signal_rows) + "\n";
  body += "# n_signal_rows_unlabeled_tail=" +
          std::to_string(gate.n_signal_rows_unlabeled) + "\n";
  body += "# frac_signal_rows_unlabeled_tail=" + fmt_double(gate.frac_unlabeled()) + "\n";
  // ── The benchmark gate verdict (round-4 F2, corrected in round 5) ─────────
  body += std::string("# gate_verdict=") + (gate.verdict.pass ? "PASS" : "FAIL") + "\n";
  body += "# gate_rule=model_beats_every_zero_parameter_benchmark_on_rv_fwd_21d_"
          "pearson_and_spearman_ic_and_on_iv_neutralised_pnl_excess_over_the_"
          "short_everything_floor_which_must_itself_be_positive\n";
  body += "# gate_primary_target=rv_fwd_21d\n";
  // The corrections that make the verdict above mean something, stamped on
  // every artifact so a stale reader cannot mistake this file for a round-4
  // one. See atx-vol/tools/vrp_train.hpp (kVrpScoreContamNegIv) and
  // .superpowers/sdd/2026-08-15-vrp-ml/audit-benchmark-contamination.md.
  body += "# gate_contaminated_target=label_composite_rank_ordering_is_anti_correlated_"
          "with_realized_vol_forecasting_skill_reported_never_gated\n";
  body += "# gate_deleted_benchmark=neg_iv_fair_21d_rank_ic_exactly_plus_1.0000_vs_the_"
          "labels_own_implied_leg_and_minus_0.6128_t_nw_minus_22.93_vs_rv_fwd_21d\n";
  body += "# gate_deleted_benchmark_reference=.superpowers/sdd/2026-08-15-vrp-ml/"
          "audit-benchmark-contamination.md\n";
  body += "# gate_model=" + gate.verdict.model + "\n";
  body += "# gate_n_benchmarks=" + std::to_string(gate.verdict.n_benchmarks) + "\n";
  body += "# gate_best_benchmark=" + gate.verdict.best_benchmark + "\n";
  body += "# gate_model_ic_pearson=" + fmt_double(gate.verdict.model_ic_pearson) + "\n";
  body += "# gate_model_ic_spearman=" + fmt_double(gate.verdict.model_ic_spearman) + "\n";
  body += "# gate_best_benchmark_ic_pearson=" +
          fmt_double(gate.verdict.best_benchmark_ic_pearson) + "\n";
  body += "# gate_best_benchmark_ic_spearman=" +
          fmt_double(gate.verdict.best_benchmark_ic_spearman) + "\n";
  body += "# gate_model_pnl_iv_neutral_excess_over_floor=" +
          fmt_double(gate.verdict.model_pnl_excess) + "\n";
  body += "# gate_best_benchmark_pnl_iv_neutral_excess_over_floor=" +
          fmt_double(gate.verdict.best_benchmark_pnl_excess) + "\n";
  body += "# gate_pnl_floor_short_everything=" + fmt_double(gate.verdict.pnl_floor) + "\n";
  // ── Money (round-5): definition, the data defect it survives, the floor ───
  body += "# pnl_definition=hold_to_horizon_daily_delta_hedged_atm_straddle_"
          "100x(rv_fwd_21d^2-iv_fair_21d^2)/(2*iv_fair_21d)_vol_pts_per_1u_gross_vega\n";
  body += "# pnl_floor_contract=every_pnl_figure_is_published_with_its_excess_over_the_"
          "short_everything_floor_computed_from_this_runs_own_rows\n";
  body += "# pnl_ppv_winsor_abs=" + fmt_double(kVrpPpvWinsorAbs) + "\n";
  body += "# pnl_n_rows_priced=" + std::to_string(gate.n_ppv_rows_priced) + "\n";
  body += "# pnl_n_rows_winsorized=" + std::to_string(gate.n_ppv_rows_winsorized) + "\n";
  // ── The VEGA gate (round 5): the same standard on the tradeable axis ──────
  body += std::string("# vega_gate_verdict=") + (gate.vega_verdict.pass ? "PASS" : "FAIL") +
          "\n";
  body += "# vega_gate_rule=model_beats_every_zero_parameter_benchmark_on_iv_chg_21d_roll_"
          "pearson_and_spearman_ic_and_on_iv_neutralised_net_of_cost_vega_pnl_excess_over_"
          "the_zero_selection_vega_floor_which_must_itself_be_positive\n";
  body += "# vega_gate_primary_target=iv_chg_21d_roll\n";
  body += "# vega_target_definition=100x(iv_atmf_21d[t+21]-iv_atmf_21d[t])_minus_"
          "100x(iv_fair_63d[t]-iv_fair_21d[t])/2_vol_pts_per_1u_vega_held\n";
  body += "# vega_target_raw_axis_is_not_a_pnl=iv_chg_21d_raw_omits_the_term_structure_roll_"
          "a_constant_maturity_vega_position_pays_and_its_strongest_free_predictor_is_that_"
          "roll_reported_never_gated\n";
  body += "# vega_target_eiv_caveat=both_legs_of_the_target_are_fitted_surface_reads_so_any_"
          "predictor_built_from_the_same_entry_read_inherits_its_error_with_the_targets_own_"
          "sign_use_feature_lag_2_before_trusting_any_level_ranked_column\n";
  body += "# vega_cost_model=christoffersen_goyenko_jacobs_karoui_rfs_2018_atm_call_effective_"
          "relative_spread_6.41pct_of_premium_halved_one_way_charged_at_entry_and_exit\n";
  body += "# vega_cost_one_way_frac_of_premium=" +
          fmt_double(kVrpVegaOneWayCostFracOfPremium * (gate.cost_crossing_fraction /
                                                        kVrpVegaCrossingFractionMeasured)) +
          "\n";
  // ROUND 6: the crossing fraction, made reachable. The measured input is the
  // EFFECTIVE spread; the quoted width is DERIVED from it by dividing out
  // Zhan-Han-Cao-Tong's realized effective/quoted, so a reader can re-cross it
  // at any fraction without double-counting the discount already inside the
  // Christoffersen measurement.
  body += "# vega_cost_crossing_fraction=" + fmt_double(gate.cost_crossing_fraction) + "\n";
  body += "# vega_cost_crossing_fraction_default=" + fmt_double(kVrpDefaultCrossingFraction) +
          "\n";
  body += "# vega_cost_quoted_one_way_frac_of_premium=" +
          fmt_double(kVrpVegaQuotedOneWayFracOfPremium) + "\n";
  body += "# vega_cost_effective_one_way_frac_of_premium_measured=" +
          fmt_double(kVrpVegaEffectiveOneWayFracOfPremium) + "\n";
  body += "# vega_cost_crossing_provenance=quoted_width_is_derived_by_dividing_the_measured_"
          "christoffersen_effective_spread_by_zhan_han_cao_tong_rfs_2022_realized_effective_"
          "over_quoted_0.55_measured_on_actual_opra_prints_2003_2016_so_the_default_returns_"
          "the_measured_effective_charge_and_the_orats_complex_order_0.53_at_backtest_hpp_863_"
          "must_never_be_applied_on_top_of_it\n";
  body += "# vega_cost_scales_with_iv=one_way_vol_points_equals_frac_of_premium_times_100x_iv_"
          "so_a_flat_vol_point_charge_undercharges_high_vol_names\n";
  body += "# vega_book_turnover_unit=fraction_of_gross_replaced_between_consecutive_formation_"
          "dates_1.0_is_a_complete_replacement_and_the_cost_column_charges_one_round_trip_per_"
          "cohort_not_per_formation_date\n";
  body += "# eiv_target_entry_lag=" + std::to_string(gate.eiv_target_entry_lag) + "\n";
  body += "# vega_short_haircut=" + fmt_double(gate.short_vega_haircut) + "\n";
  body += "# vega_short_haircut_contract=applied_to_the_short_legs_aggregate_edge_only_when_"
          "that_edge_is_positive_never_to_a_short_vega_loss\n";
  body += "# vega_gate_model=" + gate.vega_verdict.model + "\n";
  body += "# vega_gate_n_benchmarks=" + std::to_string(gate.vega_verdict.n_benchmarks) + "\n";
  body += "# vega_gate_best_benchmark=" + gate.vega_verdict.best_benchmark + "\n";
  body += "# vega_gate_model_ic_pearson=" + fmt_double(gate.vega_verdict.model_ic_pearson) +
          "\n";
  body += "# vega_gate_model_ic_spearman=" + fmt_double(gate.vega_verdict.model_ic_spearman) +
          "\n";
  body += "# vega_gate_best_benchmark_ic_pearson=" +
          fmt_double(gate.vega_verdict.best_benchmark_ic_pearson) + "\n";
  body += "# vega_gate_best_benchmark_ic_spearman=" +
          fmt_double(gate.vega_verdict.best_benchmark_ic_spearman) + "\n";
  body += "# vega_gate_model_excess_over_vega_floor=" +
          fmt_double(gate.vega_verdict.model_pnl_excess) + "\n";
  body += "# vega_gate_best_benchmark_excess_over_vega_floor=" +
          fmt_double(gate.vega_verdict.best_benchmark_pnl_excess) + "\n";
  body += "# vega_gate_floor=" + fmt_double(gate.vega_verdict.vega_floor) + "\n";
  body += std::string("# vega_gate_floor_binding=") +
          (gate.vega_verdict.vega_floor_is_long ? "long_everything" : "short_everything") + "\n";
  body += "# vega_n_rows_with_exit_mark=" + std::to_string(gate.n_iv_chg_rows) + "\n";
  body += "# vega_n_rows_no_exit_mark=" + std::to_string(gate.n_iv_chg_rows_no_exit) + "\n";
  body += "# vega_n_rows_roll_undefined=" +
          std::to_string(gate.n_iv_chg_rows_roll_undefined) + "\n";
  append_pnl_floor_meta(body, "# gate_pooled_pnl_", gate.pooled_floor);
  for (const VrpPnlReport &p : gate.pooled_pnl) {
    append_pnl_meta(body, "# gate_pooled_pnl_", p);
  }
  append_vega_floor_meta(body, "# gate_pooled_", gate.pooled_vega_floor);
  for (const VrpVegaReport &v : gate.pooled_vega) {
    append_vega_meta(body, "# gate_pooled_", v);
  }
  for (const VrpScoreReport &s : gate.pooled) {
    append_score_meta(body, "# gate_pooled_", s);
  }
  for (const VrpGateFold &f : gate.per_fold) {
    const std::string prefix = "# gate_fold_" + std::to_string(f.fold_id) + "_";
    append_pnl_floor_meta(body, prefix + "pnl_", f.floor);
    for (const VrpPnlReport &p : f.pnl) {
      append_pnl_meta(body, prefix + "pnl_", p);
    }
    append_vega_floor_meta(body, prefix, f.vega_floor);
    for (const VrpVegaReport &v : f.vega) {
      append_vega_meta(body, prefix, v);
    }
    for (const VrpScoreReport &s : f.scores) {
      append_score_meta(body, prefix, s);
    }
  }
  // Per-fold accounting meta lines (round-2 review majors 2 + 3): the
  // purge/embargo train-row losses and the GBT QLIKE-path insanity-clip
  // count + post-clip extrema, via the same `# key=value` mechanism as the
  // F1 counters. Round 3 adds the Mincer-Zarnowitz level diagnostics (raw
  // always; recalibrated behind the flag, together with the fit-window
  // accounting) and the baseline smearing factor. Additive only -- the
  // tabular columns below stay round-1 shaped.
  for (const VrpFoldMetrics &m : folds) {
    const std::string p = "# fold_" + std::to_string(m.fold_id) + "_";
    body += p + "n_train_purged=" + std::to_string(m.n_train_purged) + "\n";
    body += p + "n_train_embargoed=" + std::to_string(m.n_train_embargoed) + "\n";
    body += p + "n_gbt_forecast_clipped=" + std::to_string(m.n_gbt_forecast_clipped) + "\n";
    body += p + "gbt_test_forecast_min=" + fmt_double(m.gbt_test_forecast_min) + "\n";
    body += p + "gbt_test_forecast_max=" + fmt_double(m.gbt_test_forecast_max) + "\n";
    body += p + "mz_slope_raw=" + fmt_double(m.mz_slope_raw) + "\n";
    body += p + "mz_intercept_raw=" + fmt_double(m.mz_intercept_raw) + "\n";
    body += p + "smear_factor=" + fmt_double(m.smear_factor) + "\n";
    if (recal_on) {
      body += p + "recal_applied=" + std::to_string(m.recal_applied ? 1 : 0) + "\n";
      body += p + "recal_window_effective=" + std::to_string(m.recal_window_effective) + "\n";
      body += p + "recal_n_fit=" + std::to_string(m.recal_n_fit) + "\n";
      body += p + "qlike_gbt_recal=" + fmt_double(m.qlike_gbt_recal) + "\n";
      body += p + "mz_slope_recal=" + fmt_double(m.mz_slope_recal) + "\n";
      body += p + "mz_intercept_recal=" + fmt_double(m.mz_intercept_recal) + "\n";
      body += p + "ic_gbt_recal=" + fmt_double(m.ic_gbt_recal) + "\n";
    }
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
  if (cfg.recalibrate == VrpRecalMode::Isotonic && cfg.recalib_window_sessions == 0) {
    return Err(ErrorCode::InvalidArgument,
               "run_vrp_train: --recalib-window must be >= 1 under --recalibrate isotonic");
  }
  if (cfg.feature_lag > kVrpMaxFeatureLag) {
    return Err(ErrorCode::InvalidArgument,
               "run_vrp_train: --feature-lag " + std::to_string(cfg.feature_lag) +
                   " exceeds the " + std::to_string(kVrpMaxFeatureLag) + "-session cap");
  }
  // FAIL CLOSED ON THE COST KNOB. A crossing fraction outside (0, 1] is not a
  // sensitivity, it is a free lunch or a sign error, and five rounds of this
  // sprint died on cost. Zero is excluded explicitly: "costs off" is what the
  // gross column already reports, and it must never be reachable by a knob a
  // reader would mistake for a calibration.
  if (!(cfg.cost_crossing_fraction > 0.0) || !(cfg.cost_crossing_fraction <= 1.0)) {
    return Err(ErrorCode::InvalidArgument,
               "run_vrp_train: --cost-crossing-fraction must lie in (0, 1]; the published "
               "measurements bracket 0.38 (patient algo) to 1.00 (full quoted cross)");
  }
  if (cfg.eiv_target_entry_lag > kVrpMaxFeatureLag) {
    return Err(ErrorCode::InvalidArgument,
               "run_vrp_train: --eiv-target-entry-lag " +
                   std::to_string(cfg.eiv_target_entry_lag) + " exceeds the " +
                   std::to_string(kVrpMaxFeatureLag) + "-session cap");
  }
  auto panel_r = load_vrp_panel(cfg.panel_path);
  if (!panel_r.has_value()) {
    return Err(panel_r.error());
  }
  VrpTrainReport report;
  report.panel = std::move(*panel_r);
  // ROUND-5: the vol-CHANGE target needs trailing realized vol, and a TARGET is
  // never feature-lagged (F4's contract, without which a lag-to-lag comparison
  // is two experiments). So ln(rv_trail_21d) is captured from the RAW panel
  // here, before the shift below can touch it. f2_log_rv21 = ln(rv_trail^2),
  // hence ln(rv_trail) = f2/2; NaN on warmup rows, which the finite-pair filter
  // then drops rather than imputing.
  std::vector<double> log_rv_trail(report.panel.rows.size(),
                                   std::numeric_limits<double>::quiet_NaN());
  for (std::size_t r = 0; r < report.panel.rows.size(); ++r) {
    log_rv_trail[r] = 0.5 * report.panel.rows[r].f[kVrpFeatLogRv21];
  }
  // ROUND-5: the tradeable iv-change target and its roll leg, both built from
  // TARGET-SIDE columns on the RAW panel for the same reason as above -- the
  // roll a position pays is the ENTRY date's curve, not a lagged read of it.
  const VrpIvChgTargets iv_chg =
      vrp_build_iv_chg(report.panel, kVrpHorizonSessions, cfg.eiv_target_entry_lag);
  report.gate.n_iv_chg_rows = iv_chg.n_rows_with_exit;
  report.gate.n_iv_chg_rows_no_exit = iv_chg.n_rows_no_exit;
  report.gate.n_iv_chg_rows_roll_undefined = iv_chg.n_rows_roll_undefined;
  report.gate.short_vega_haircut = cfg.short_vega_haircut;
  report.gate.cost_crossing_fraction = cfg.cost_crossing_fraction;
  report.gate.eiv_target_entry_lag = cfg.eiv_target_entry_lag;
  // ROUND-4 F4: shift the feature vectors before ANYTHING reads them, so the
  // standardization, both models, signal_vov and the HV-IV benchmark all face
  // one information set. Targets are untouched, so the fold plan below is
  // identical at every lag and lag-to-lag comparisons stay like for like.
  report.feature_lag_rows_unavailable = apply_vrp_feature_lag(report.panel, cfg.feature_lag);
  // The corpus label every gate number is stamped with. Derived from the panel
  // file stem unless the caller names it: an unlabeled IC is how a clean-25
  // number came to be quoted for an SP100 book for three rounds.
  report.gate.corpus =
      cfg.corpus.empty() ? std::filesystem::path{cfg.panel_path}.stem().string() : cfg.corpus;

  auto obs_r = build_vrp_observations(report.panel, cfg.max_label_span_sessions);
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
  VrpIsotonicMap last_recal_map; // empty when off / not applied
  bool last_recal_applied = false;

  // Gate accumulators: the OOS test rows of every fold, held per fold and
  // graded AFTER the walk-forward completes. Grading is deferred because one
  // of the scored columns -- the ranking column the book actually sorts on --
  // is a WITHIN-DATE cross-sectional transform and does not exist until every
  // fold's forecasts are in hand. Anchored walk-forward with step >= test makes
  // the test windows disjoint and increasing, so the concatenation is already
  // ascending in entry_ts_ns -- which is what the per-date grouping in
  // vrp_score_report requires. That ordering is RE-ESTABLISHED below rather
  // than relied on.
  struct FoldGradeInputs {
    std::uint32_t fold_id{0};
    std::vector<std::size_t> panel_rows;
    VrpGradeRows targets;
    std::vector<double> pred_model;
    std::vector<double> pred_baseline;
    std::vector<double> bench_hv_iv;
    std::vector<double> contam_neg_iv;
    std::vector<double> bench_neg_iv_atmf;
    std::vector<double> bench_term_slope;
    std::vector<double> contam_iv_slope;
  };
  std::vector<FoldGradeInputs> fold_inputs;
  fold_inputs.reserve(report.plan.folds.size());

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

    // Round-3 isotonic recalibration (digest Q4 [18][19]): fit a monotone
    // map from raw GBT label forecast to realized label on the TRAILING
    // window of the fold's ADMITTED train sessions, using genuinely
    // out-of-sample forecasts from a CALIBRATION model fit on the earlier
    // train rows (temporal-holdout calibration). Fit rows are admitted
    // train rows, so the plan's own purge already bounds every fit row's
    // recorded (upper-bound) label end at the fold's test start -- the fit
    // uses only data strictly before the test window, and the fold plan is
    // untouched by the flag (the leak adjudicator stays PASS). The map is
    // applied to the PRODUCTION model's raw test forecasts; the production
    // fit itself (full train fold, same seed) is bit-identical to flag-off.
    VrpIsotonicMap recal_map;
    if (cfg.recalibrate == VrpRecalMode::Isotonic) {
      // Distinct train decision sessions, ascending (train_rows arrive in
      // canonical ascending panel order).
      std::vector<std::int64_t> train_sessions;
      for (const std::size_t r : m.train_rows) {
        const std::int64_t ts = report.panel.rows[r].entry_ts_ns;
        if (train_sessions.empty() || train_sessions.back() != ts) {
          train_sessions.push_back(ts);
        }
      }
      // Never let calibration swallow the fold: cap the window at half the
      // admitted train sessions (the effective value is reported).
      const std::size_t eff =
          std::min(cfg.recalib_window_sessions, train_sessions.size() / 2);
      m.recal_window_effective = eff;
      if (eff >= 1) {
        const std::int64_t cutoff = train_sessions[train_sessions.size() - eff];
        std::vector<std::size_t> core_rows;
        std::vector<std::size_t> calib_rows;
        for (const std::size_t r : m.train_rows) {
          (report.panel.rows[r].entry_ts_ns < cutoff ? core_rows : calib_rows).push_back(r);
        }
        // eff <= n_sessions/2 guarantees both splits are non-empty.
        const VrpStandardization stz_core = compute_asset_standardization(
            report.panel, std::span<const std::size_t>{core_rows});
        std::vector<double> core_labels(core_rows.size(), 0.0);
        for (std::size_t ci = 0; ci < core_rows.size(); ++ci) {
          core_labels[ci] = report.panel.rows[core_rows[ci]].label;
        }
        const learn::FeatureMatrix fm_core = detail::build_feature_matrix(
            report.panel, stz_core, std::span<const std::size_t>{core_rows},
            std::span<const std::size_t>{detail::kAllFeatures},
            std::span<const double>{core_labels});
        const learn::LearnedModel gbt_core =
            learn::fit_gbt(fm_core, learn::LatentAugmentation{}, gbt_cfg);
        std::vector<double> fit_x;
        std::vector<double> fit_y;
        for (const std::size_t r : calib_rows) {
          const double pred =
              detail::gbt_predict_label(report.panel, stz_core, gbt_core, r);
          if (std::isfinite(pred)) { // NaN forecast carries no level info
            fit_x.push_back(pred);
            fit_y.push_back(report.panel.rows[r].label);
            m.recal_fit_rows.push_back(r);
          }
        }
        if (fit_x.size() >= 2) { // < 2 points cannot shape a level map
          auto map_r = fit_vrp_isotonic(std::move(fit_x), std::move(fit_y));
          if (!map_r.has_value()) {
            return Err(map_r.error());
          }
          recal_map = std::move(*map_r);
          m.recal_applied = true;
        } else {
          m.recal_fit_rows.clear();
        }
      }
      m.recal_n_fit = m.recal_fit_rows.size();
    }

    double q_base = 0.0;
    double q_gbt = 0.0;
    double q_recal = 0.0;
    double q_mean = 0.0;
    double base_forecast_max = -std::numeric_limits<double>::infinity();
    double gbt_forecast_min = std::numeric_limits<double>::infinity();
    double gbt_forecast_max = -std::numeric_limits<double>::infinity();
    std::vector<std::int64_t> test_ts;
    std::vector<double> pred_base;
    std::vector<double> pred_gbt;
    std::vector<double> pred_recal;
    std::vector<double> realized;
    // The surviving ZERO-PARAMETER benchmark and the contaminated appendix, on
    // this fold's exact test rows.
    std::vector<double> bench_hv_iv;
    std::vector<double> contam_neg_iv;
    // The other two target axes and the P&L inputs, all target-side.
    std::vector<double> tgt_rv_fwd;
    std::vector<double> tgt_vol_chg;
    std::vector<double> row_iv_fair;
    // ROUND-5: the two iv-change axes, the ATM-forward mark the vega book
    // enters at, and the three free/contaminated columns that axis is judged
    // against.
    std::vector<double> tgt_iv_chg_raw;
    std::vector<double> tgt_iv_chg_roll;
    std::vector<double> row_iv_atmf;
    std::vector<std::size_t> row_sym;
    std::vector<double> bench_neg_iv_atmf;
    std::vector<double> bench_term_slope;
    std::vector<double> contam_iv_slope;
    test_ts.reserve(m.test_rows.size());
    for (const std::size_t r : m.test_rows) {
      const VrpPanelRow &row = report.panel.rows[r];
      const double proxy_var = row.rv_fwd_21d * row.rv_fwd_21d;
      const double iv_var = row.iv_fair_21d * row.iv_fair_21d;

      const double f_base =
          detail::baseline_forecast_var(report.panel, stz, baseline, r, cfg.retransform);
      base_forecast_max = std::max(base_forecast_max, f_base);
      const double label_base = (f_base - iv_var) * kVrpHorizonYears;

      const double label_gbt = detail::gbt_predict_label(report.panel, stz, gbt, r);
      // Monotone post-map: identity when recalibration is off / not applied.
      const double label_recal =
          m.recal_applied ? vrp_isotonic_eval(recal_map, label_gbt) : label_gbt;
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
      // The recalibrated forecast's QLIKE goes through the SAME insanity
      // clip (F > 0 precondition; the map's constant extrapolation already
      // bounds it by observed calibration labels, but a small iv_var can
      // still push the implied variance under train_var_min).
      const double f_recal = std::clamp(label_recal / kVrpHorizonYears + iv_var,
                                        baseline.train_var_min, baseline.train_var_max);

      q_base += vrp_qlike(f_base, proxy_var);
      q_gbt += vrp_qlike(f_gbt, proxy_var);
      q_recal += vrp_qlike(f_recal, proxy_var);
      q_mean += vrp_qlike(baseline.train_var_mean, proxy_var);

      test_ts.push_back(row.entry_ts_ns);
      pred_base.push_back(label_base);
      pred_gbt.push_back(label_gbt);
      pred_recal.push_back(label_recal);
      realized.push_back(row.label);
      // The three target axes. rv_fwd_21d is the PRIMARY one; the composite
      // label above is retained and reported as contaminated.
      tgt_rv_fwd.push_back(row.rv_fwd_21d);
      tgt_vol_chg.push_back(std::log(row.rv_fwd_21d) - log_rv_trail[r]);
      row_iv_fair.push_back(row.iv_fair_21d);
      bench_hv_iv.push_back(row.f[kVrpFeatHvIvGap]); // Goyal-Saretto HV-IV
      // -iv_fair_21d: NOT a benchmark. It is a perfect rank transform of the
      // composite label's own implied leg (IC exactly +1.0000 by algebra) and
      // an anti-forecast of realized vol (-0.6128, t_nw -22.93). Scored and
      // published so the evidence survives; VrpScoreKind::Contaminated makes it
      // structurally unable to decide a verdict. See kVrpScoreContamNegIv.
      contam_neg_iv.push_back(-row.iv_fair_21d);
      tgt_iv_chg_raw.push_back(iv_chg.raw[r]);
      tgt_iv_chg_roll.push_back(iv_chg.roll[r]);
      row_iv_atmf.push_back(row.iv_atmf_21d);
      row_sym.push_back(report.panel.row_symbol[r]);
      // Zero-parameter free rules for the iv-change axes.
      //
      // ROUND 6 EIV FIX. `bench_term_slope` used to be read off the TARGET-SIDE
      // row (`row.iv_fair_63d - row.iv_fair_21d`), so --feature-lag did not
      // move it and its "lag 2" figure still saw the entry session's own
      // surface read. Round 5 flagged that and did not fix it. It is now read
      // from `f4_term_slope`, which apply_vrp_feature_lag shifts and which the
      // panel builds as EXACTLY the same difference -- so the lag-0 ranking,
      // and therefore every round-5 number, is unchanged, while lag 2 finally
      // means what it says. A free rule cannot be gated on a lag it never took.
      //
      // `bench_neg_iv_atmf` has NO feature-side twin (the panel carries no
      // lagged iv_atmf_21d), so it remains target-side and its lag-2 figure
      // remains optimistic. It is reported, and it is not what anything ships
      // on.
      bench_neg_iv_atmf.push_back(-row.iv_atmf_21d);
      bench_term_slope.push_back(row.f[kVrpFeatTermSlope]);
      // Contaminated on the iv-change axes: contains the target's own entry
      // mark, hence its measurement error with the target's own sign.
      contam_iv_slope.push_back(row.iv_fair_63d - row.iv_atmf_21d);
      m.test_pred_raw.push_back(label_gbt);
      m.test_pred_recal.push_back(label_recal);

      // Behind the flag the recalibrated value flows through the EXISTING
      // pred_label / pred_edge_norm columns (schema unchanged; value
      // semantics in CHANGELOG). label_recal == label_gbt when off.
      const std::size_t sym = report.panel.row_symbol[r];
      const double sd = label_stats.sd[sym];
      signal_entries.push_back(detail::SignalEntry{
          .panel_row = r,
          .pred_label = label_recal,
          .pred_edge_norm = sd == 0.0 ? 0.0 : (label_recal - label_stats.mean[sym]) / sd,
          .vov_63d = detail::signal_vov(report.panel, stz, r)});
    }
    const auto n_test = static_cast<double>(m.test_rows.size());
    m.qlike_baseline = q_base / n_test;
    m.qlike_gbt = q_gbt / n_test;
    m.qlike_gbt_recal = q_recal / n_test;
    m.qlike_mean_forecast = q_mean / n_test;
    m.baseline_test_forecast_max = base_forecast_max;
    m.gbt_test_forecast_min = gbt_forecast_min;
    m.gbt_test_forecast_max = gbt_forecast_max;
    m.smear_factor = baseline.smear_factor;
    m.ic_baseline = detail::mean_per_date_spearman(std::span<const std::int64_t>{test_ts},
                                                   std::span<const double>{pred_base},
                                                   std::span<const double>{realized});
    m.ic_gbt = detail::mean_per_date_spearman(std::span<const std::int64_t>{test_ts},
                                              std::span<const double>{pred_gbt},
                                              std::span<const double>{realized});
    m.ic_gbt_recal = detail::mean_per_date_spearman(std::span<const std::int64_t>{test_ts},
                                                    std::span<const double>{pred_recal},
                                                    std::span<const double>{realized});
    // Mincer-Zarnowitz level diagnostics in LABEL units (realized on
    // forecast; target slope 1, intercept 0) -- raw always, recal alongside.
    const VrpMzFit mz_raw = vrp_mincer_zarnowitz(std::span<const double>{pred_gbt},
                                                 std::span<const double>{realized});
    m.mz_slope_raw = mz_raw.slope;
    m.mz_intercept_raw = mz_raw.intercept;
    const VrpMzFit mz_recal = vrp_mincer_zarnowitz(std::span<const double>{pred_recal},
                                                   std::span<const double>{realized});
    m.mz_slope_recal = mz_recal.slope;
    m.mz_intercept_recal = mz_recal.intercept;

    // ROUND-4 F2/F3 + ROUND-5: retain THIS fold's rows -- same rows, same
    // dates, same horizon -- for the grading pass below. `pred_recal` is the
    // column the signal actually carries (== raw when recalibration is off), so
    // the gate grades the shipped forecast, not an internal one.
    fold_inputs.push_back(
        FoldGradeInputs{.fold_id = m.fold_id,
                        .panel_rows = m.test_rows,
                        .targets = VrpGradeRows{.ts = test_ts,
                                                .rv_fwd = tgt_rv_fwd,
                                                .vol_chg = tgt_vol_chg,
                                                .label = realized,
                                                .iv_fair = row_iv_fair,
                                                .iv_chg_raw = tgt_iv_chg_raw,
                                                .iv_chg_roll = tgt_iv_chg_roll,
                                                .iv_atmf = row_iv_atmf,
                                                .symbol = row_sym},
                        .pred_model = pred_recal,
                        .pred_baseline = pred_base,
                        .bench_hv_iv = bench_hv_iv,
                        .contam_neg_iv = contam_neg_iv,
                        .bench_neg_iv_atmf = bench_neg_iv_atmf,
                        .bench_term_slope = bench_term_slope,
                        .contam_iv_slope = contam_iv_slope});

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
    last_recal_map = std::move(recal_map);
    last_recal_applied = m.recal_applied;
    report.folds.push_back(std::move(m));
  }

  if (!last_gbt.has_value() || !last_baseline.has_value()) {
    return Err(ErrorCode::InvalidArgument, "run_vrp_train: walk-forward produced no folds");
  }

  // NaN-label tail rows: predict-time rows, scored by the FINAL fold's
  // models (their dates lie after the final train window, so this is a
  // forward application, not a re-scoring of trained rows). Behind the
  // recalibration flag the final fold's monotone map applies here too.
  for (std::size_t r = 0; r < report.panel.rows.size(); ++r) {
    const VrpPanelRow &row = report.panel.rows[r];
    if (is_labeled_row(row)) {
      continue;
    }
    const double label_gbt = detail::gbt_predict_label(report.panel, *last_stz, *last_gbt, r);
    const double label_out =
        last_recal_applied ? vrp_isotonic_eval(last_recal_map, label_gbt) : label_gbt;
    const std::size_t sym = report.panel.row_symbol[r];
    const double sd = last_label_stats->sd[sym];
    signal_entries.push_back(detail::SignalEntry{
        .panel_row = r,
        .pred_label = label_out,
        .pred_edge_norm = sd == 0.0 ? 0.0 : (label_out - last_label_stats->mean[sym]) / sd,
        .vov_63d = detail::signal_vov(report.panel, *last_stz, r)});
  }
  std::sort(signal_entries.begin(), signal_entries.end(),
            [](const detail::SignalEntry &a, const detail::SignalEntry &b) {
              return a.panel_row < b.panel_row;
            });
  // ROUND-4 F1: the DEFAULT ranking column. Overwrites the per-symbol z-score
  // computed above with the within-date cross-sectional one; PerSymbol leaves
  // the round-3 bytes exactly as they were.
  if (cfg.edge_norm == VrpEdgeNormMode::CrossSection) {
    detail::apply_cross_section_edge_norm(report.panel,
                                          std::span<detail::SignalEntry>{signal_entries});
  }
  // Coverage honesty: how much of what ships was never validated by an
  // out-of-sample outcome, because 27% of the round-2 run was not.
  report.gate.n_signal_rows = signal_entries.size();
  for (const detail::SignalEntry &e : signal_entries) {
    if (!is_labeled_row(report.panel.rows[e.panel_row])) {
      ++report.gate.n_signal_rows_unlabeled;
    }
  }

  // ── THE GRADING PASS ──────────────────────────────────────────────────────
  //
  // Every column on every target axis, plus the money, per fold and pooled.
  // Deferred to here because the SHIPPED RANKING COLUMN is a within-date
  // cross-sectional transform that does not exist until all folds are in:
  // rounds 1-3 measured pred_label and traded pred_edge_norm, and on the traded
  // universe that transform cut the IC by 62% and inverted the decile tails
  // (+0.042 pooled IC while the tails ran -2.5 vol pts). Scoring it beside the
  // forecast is what turns that from a forensic finding into a reported one.
  {
    std::vector<double> edge_of(report.panel.rows.size(),
                                std::numeric_limits<double>::quiet_NaN());
    for (const detail::SignalEntry &e : signal_entries) {
      edge_of[e.panel_row] = e.pred_edge_norm;
    }
    VrpGradeRows pooled_rows;
    std::vector<double> pool_model;
    std::vector<double> pool_baseline;
    std::vector<double> pool_hv_iv;
    std::vector<double> pool_ranked;
    std::vector<double> pool_contam;
    std::vector<double> pool_neg_iv_atmf;
    std::vector<double> pool_term_slope;
    std::vector<double> pool_contam_slope;
    // The fixed column set, in the canonical order every artifact and reader
    // relies on: the model on trial, the fitted (and itself contaminated)
    // baseline, the surviving zero-parameter benchmarks, the column the BOOK
    // ranks on, and the contaminated appendices last.
    const auto columns = [](const FoldGradeInputs &in, std::span<const double> model,
                            std::span<const double> ranked) {
      return std::array<VrpGradeColumn, kVrpGateColumnCount>{
          VrpGradeColumn{std::string{kVrpScoreModel}, VrpScoreKind::Model, model, true},
          VrpGradeColumn{std::string{kVrpScoreBaseline}, VrpScoreKind::Baseline,
                         in.pred_baseline, true},
          VrpGradeColumn{std::string{kVrpScoreBenchHvIv}, VrpScoreKind::Benchmark,
                         in.bench_hv_iv, false},
          VrpGradeColumn{std::string{kVrpScoreBenchNegIvAtmf}, VrpScoreKind::Benchmark,
                         in.bench_neg_iv_atmf, false},
          VrpGradeColumn{std::string{kVrpScoreBenchTermSlope}, VrpScoreKind::Benchmark,
                         in.bench_term_slope, false},
          VrpGradeColumn{std::string{kVrpScoreRanked}, VrpScoreKind::Ranked, ranked, false},
          VrpGradeColumn{std::string{kVrpScoreContamNegIv}, VrpScoreKind::Contaminated,
                         in.contam_neg_iv, false},
          VrpGradeColumn{std::string{kVrpScoreContamIvSlope}, VrpScoreKind::Contaminated,
                         in.contam_iv_slope, false}};
    };
    report.gate.per_fold.reserve(fold_inputs.size());
    for (const FoldGradeInputs &fi : fold_inputs) {
      std::vector<double> ranked;
      ranked.reserve(fi.panel_rows.size());
      for (const std::size_t r : fi.panel_rows) {
        ranked.push_back(edge_of[r]);
      }
      const std::array<VrpGradeColumn, kVrpGateColumnCount> cols =
          columns(fi, fi.pred_model, ranked);
      VrpGraded g = vrp_grade(fi.targets, std::span<const VrpGradeColumn>{cols},
                              cfg.short_vega_haircut, cfg.cost_crossing_fraction);
      report.gate.per_fold.push_back(VrpGateFold{.fold_id = fi.fold_id,
                                                 .scores = std::move(g.scores),
                                                 .pnl = std::move(g.pnl),
                                                 .vega = std::move(g.vega),
                                                 .floor = std::move(g.floor),
                                                 .vega_floor = std::move(g.vega_floor)});
      const auto cat = [](std::vector<double> &dst, const std::vector<double> &src) {
        dst.insert(dst.end(), src.begin(), src.end());
      };
      pooled_rows.ts.insert(pooled_rows.ts.end(), fi.targets.ts.begin(), fi.targets.ts.end());
      cat(pooled_rows.rv_fwd, fi.targets.rv_fwd);
      cat(pooled_rows.vol_chg, fi.targets.vol_chg);
      cat(pooled_rows.label, fi.targets.label);
      cat(pooled_rows.iv_fair, fi.targets.iv_fair);
      cat(pooled_rows.iv_chg_raw, fi.targets.iv_chg_raw);
      cat(pooled_rows.iv_chg_roll, fi.targets.iv_chg_roll);
      cat(pooled_rows.iv_atmf, fi.targets.iv_atmf);
      pooled_rows.symbol.insert(pooled_rows.symbol.end(), fi.targets.symbol.begin(),
                                fi.targets.symbol.end());
      cat(pool_model, fi.pred_model);
      cat(pool_baseline, fi.pred_baseline);
      cat(pool_hv_iv, fi.bench_hv_iv);
      cat(pool_ranked, ranked);
      cat(pool_contam, fi.contam_neg_iv);
      cat(pool_neg_iv_atmf, fi.bench_neg_iv_atmf);
      cat(pool_term_slope, fi.bench_term_slope);
      cat(pool_contam_slope, fi.contam_iv_slope);
    }
    // The per-date grouping needs ascending timestamps; anchored folds already
    // deliver that, but the order is RE-ESTABLISHED rather than assumed -- a
    // silently mis-grouped pooled statistic is exactly the class of error this
    // whole gate exists to stop. std::stable_sort keeps the within-date row
    // order, so the result is deterministic either way.
    {
      std::vector<std::size_t> ord(pooled_rows.ts.size());
      for (std::size_t i = 0; i < ord.size(); ++i) {
        ord[i] = i;
      }
      std::stable_sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) {
        return pooled_rows.ts[a] < pooled_rows.ts[b];
      });
      const auto permute = [&ord](std::vector<double> &v) {
        std::vector<double> tmp(v.size());
        for (std::size_t i = 0; i < ord.size(); ++i) {
          tmp[i] = v[ord[i]];
        }
        v.swap(tmp);
      };
      std::vector<std::int64_t> ts_tmp(pooled_rows.ts.size());
      for (std::size_t i = 0; i < ord.size(); ++i) {
        ts_tmp[i] = pooled_rows.ts[ord[i]];
      }
      pooled_rows.ts.swap(ts_tmp);
      permute(pooled_rows.rv_fwd);
      permute(pooled_rows.vol_chg);
      permute(pooled_rows.label);
      permute(pooled_rows.iv_fair);
      permute(pooled_rows.iv_chg_raw);
      permute(pooled_rows.iv_chg_roll);
      permute(pooled_rows.iv_atmf);
      {
        std::vector<std::size_t> tmp(pooled_rows.symbol.size());
        for (std::size_t i = 0; i < ord.size(); ++i) {
          tmp[i] = pooled_rows.symbol[ord[i]];
        }
        pooled_rows.symbol.swap(tmp);
      }
      permute(pool_model);
      permute(pool_baseline);
      permute(pool_hv_iv);
      permute(pool_ranked);
      permute(pool_contam);
      permute(pool_neg_iv_atmf);
      permute(pool_term_slope);
      permute(pool_contam_slope);
    }
    FoldGradeInputs pooled_in;
    pooled_in.pred_baseline = pool_baseline;
    pooled_in.bench_hv_iv = pool_hv_iv;
    pooled_in.contam_neg_iv = pool_contam;
    pooled_in.bench_neg_iv_atmf = pool_neg_iv_atmf;
    pooled_in.bench_term_slope = pool_term_slope;
    pooled_in.contam_iv_slope = pool_contam_slope;
    const std::array<VrpGradeColumn, kVrpGateColumnCount> pooled_cols =
        columns(pooled_in, pool_model, pool_ranked);
    VrpGraded pg = vrp_grade(pooled_rows, std::span<const VrpGradeColumn>{pooled_cols},
                             cfg.short_vega_haircut, cfg.cost_crossing_fraction);
    report.gate.pooled = std::move(pg.scores);
    report.gate.pooled_pnl = std::move(pg.pnl);
    report.gate.pooled_vega = std::move(pg.vega);
    report.gate.pooled_floor = std::move(pg.floor);
    report.gate.pooled_vega_floor = std::move(pg.vega_floor);
    report.gate.n_ppv_rows_priced = pg.n_ppv_rows_priced;
    report.gate.n_ppv_rows_winsorized = pg.n_ppv_rows_winsorized;
    report.gate.verdict =
        vrp_gate_verdict(std::span<const VrpScoreReport>{report.gate.pooled},
                         std::span<const VrpPnlReport>{report.gate.pooled_pnl},
                         report.gate.pooled_floor);
    report.gate.vega_verdict =
        vrp_vega_gate_verdict(std::span<const VrpScoreReport>{report.gate.pooled},
                              std::span<const VrpVegaReport>{report.gate.pooled_vega},
                              report.gate.pooled_vega_floor);
  }

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
                                  report.observations, cfg, report.gate,
                                  report.feature_lag_rows_unavailable, report.metrics_path);
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
