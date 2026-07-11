// Corpus builder + manifest serialization. See corpus.hpp for the design.

#include "atx/vol/corpus.hpp"

#include <algorithm> // std::sort, std::min, std::max, std::find
#include <array>
#include <bit>
#include <charconv> // std::to_chars, std::from_chars
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error> // std::error_code
#include <thread>       // std::jthread, std::thread::hardware_concurrency
#include <utility>      // std::move
#include <vector>

#include "atx/core/hash.hpp"
#include "atx/vol/dispersion.hpp"     // with_uid
#include "atx/vol/priced_surface.hpp" // PricedSurface
#include "atx/vol/session.hpp"        // VolaSession::to_priced_surface
#include "atx/vol/universe.hpp"       // uid_for_symbol
#include "corpus_board_fit.hpp"       // FitSlot, fit_board (T5-extracted shared fit path)

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

const char *to_string(CorpusFitStatus status) noexcept {
  switch (status) {
  case CorpusFitStatus::Ok:
    return "Ok";
  case CorpusFitStatus::Failed:
    return "Failed";
  case CorpusFitStatus::Skipped:
    return "Skipped";
  }
  return "Unrecognized"; // unreachable for valid enumerators
}

const char *to_string(CorpusDisposition disposition) noexcept {
  switch (disposition) {
  case CorpusDisposition::Admitted:
    return "Admitted";
  case CorpusDisposition::Quarantined:
    return "Quarantined";
  case CorpusDisposition::SourceFailed:
    return "SourceFailed";
  case CorpusDisposition::FitFailed:
    return "FitFailed";
  case CorpusDisposition::Empty:
    return "Empty";
  }
  return "Unrecognized";
}

const char *to_string(CorpusAdmissionReason reason) noexcept {
  switch (reason) {
  case CorpusAdmissionReason::None:
    return "None";
  case CorpusAdmissionReason::MissingSource:
    return "MissingSource";
  case CorpusAdmissionReason::InvalidSourceSchema:
    return "InvalidSourceSchema";
  case CorpusAdmissionReason::AmbiguousSourceIdentity:
    return "AmbiguousSourceIdentity";
  case CorpusAdmissionReason::EmptyBoard:
    return "EmptyBoard";
  case CorpusAdmissionReason::FitError:
    return "FitError";
  case CorpusAdmissionReason::SourceProvenanceUnavailable:
    return "SourceProvenanceUnavailable";
  case CorpusAdmissionReason::QualityUnavailable:
    return "QualityUnavailable";
  case CorpusAdmissionReason::NonFiniteMetric:
    return "NonFiniteMetric";
  case CorpusAdmissionReason::InvalidRule:
    return "InvalidRule";
  case CorpusAdmissionReason::TooFewQuotes:
    return "TooFewQuotes";
  case CorpusAdmissionReason::TooFewSlices:
    return "TooFewSlices";
  case CorpusAdmissionReason::TooFewHoldouts:
    return "TooFewHoldouts";
  case CorpusAdmissionReason::CalendarArbitrage:
    return "CalendarArbitrage";
  case CorpusAdmissionReason::InBandBelowFloor:
    return "InBandBelowFloor";
  case CorpusAdmissionReason::OosInBandBelowFloor:
    return "OosInBandBelowFloor";
  case CorpusAdmissionReason::OosVegaWeightedBelowFloor:
    return "OosVegaWeightedBelowFloor";
  case CorpusAdmissionReason::VolRmseAboveCeiling:
    return "VolRmseAboveCeiling";
  case CorpusAdmissionReason::ReducedChi2AboveCeiling:
    return "ReducedChi2AboveCeiling";
  case CorpusAdmissionReason::RoundTripMismatch:
    return "RoundTripMismatch";
  case CorpusAdmissionReason::MetricOutOfRange:
    return "MetricOutOfRange";
  case CorpusAdmissionReason::Count:
    return "Count";
  }
  return "Unrecognized";
}

namespace {

static_assert(static_cast<unsigned>(CorpusAdmissionReason::Count) < 32u,
              "CorpusAdmissionReason no longer fits its uint32 failure mask");

[[nodiscard]] constexpr CorpusAdmissionFailureMask
admission_reason_mask(CorpusAdmissionReason reason) noexcept {
  const unsigned bit = static_cast<unsigned>(reason);
  return (reason == CorpusAdmissionReason::None || reason == CorpusAdmissionReason::Count)
             ? 0u
             : (CorpusAdmissionFailureMask{1u} << bit);
}

void record_failure(CorpusAdmissionFailureMask &mask, CorpusAdmissionReason reason) noexcept {
  mask |= admission_reason_mask(reason);
}

[[nodiscard]] bool non_finite(const std::optional<double> &value) noexcept {
  return value.has_value() && !std::isfinite(*value);
}

[[nodiscard]] bool outside_unit_interval(const std::optional<double> &value) noexcept {
  return value.has_value() && std::isfinite(*value) && (*value < 0.0 || *value > 1.0);
}

[[nodiscard]] bool negative_measurement(const std::optional<double> &value) noexcept {
  return value.has_value() && std::isfinite(*value) && *value < 0.0;
}

[[nodiscard]] bool valid_ratio_threshold(const std::optional<double> &value) noexcept {
  return !value.has_value() || (std::isfinite(*value) && *value >= 0.0 && *value <= 1.0);
}

[[nodiscard]] bool valid_nonnegative_threshold(const std::optional<double> &value) noexcept {
  return !value.has_value() || (std::isfinite(*value) && *value >= 0.0);
}

[[nodiscard]] bool valid_rule(const CorpusAdmissionRule &rule) noexcept {
  return valid_ratio_threshold(rule.min_fit_in_band) &&
         valid_ratio_threshold(rule.min_oos_in_band) &&
         valid_ratio_threshold(rule.min_oos_vega_weighted) &&
         valid_nonnegative_threshold(rule.max_mean_vol_rmse) &&
         valid_nonnegative_threshold(rule.max_mean_reduced_chi2) &&
         std::isfinite(rule.calendar_abs_k) && rule.calendar_abs_k > 0.0 &&
         rule.calendar_abs_k <= 3.0;
}

[[nodiscard]] bool required_metric_missing(const CorpusQualityMetrics &metrics,
                                           const CorpusAdmissionRule &rule) noexcept {
  return (rule.min_fit_in_band.has_value() && !metrics.fit_in_band.has_value()) ||
         (rule.min_oos_in_band.has_value() && !metrics.oos_in_band.has_value()) ||
         (rule.min_oos_vega_weighted.has_value() && !metrics.oos_vega_weighted.has_value()) ||
         (rule.max_mean_vol_rmse.has_value() && !metrics.mean_vol_rmse.has_value()) ||
         (rule.max_mean_reduced_chi2.has_value() && !metrics.mean_reduced_chi2.has_value()) ||
         (rule.require_calendar_arb_free && !metrics.calendar_violations.has_value());
}

[[nodiscard]] bool quality_has_non_finite(const CorpusQualityMetrics &metrics) noexcept {
  return non_finite(metrics.fit_in_band) || non_finite(metrics.oos_in_band) ||
         non_finite(metrics.oos_vega_weighted) || non_finite(metrics.oos_vega_weight_in_band) ||
         non_finite(metrics.oos_vega_weight_total) || non_finite(metrics.mean_vol_rmse) ||
         non_finite(metrics.mean_reduced_chi2);
}

[[nodiscard]] bool ratio_evidence_missing(const std::optional<double> &ratio,
                                          std::uint32_t numerator,
                                          std::uint32_t denominator) noexcept {
  return !ratio.has_value() && (numerator != 0u || denominator != 0u);
}

[[nodiscard]] bool ratio_evidence_invalid(const std::optional<double> &ratio,
                                          std::uint32_t numerator,
                                          std::uint32_t denominator) noexcept {
  if (!ratio.has_value()) {
    return false;
  }
  if (denominator == 0u || numerator > denominator || !std::isfinite(*ratio)) {
    return true;
  }
  const double expected = static_cast<double>(numerator) / static_cast<double>(denominator);
  return std::fabs(*ratio - expected) > 8.0 * std::numeric_limits<double>::epsilon();
}

[[nodiscard]] bool weighted_evidence_missing(const CorpusQualityMetrics &metrics) noexcept {
  const bool in = metrics.oos_vega_weight_in_band.has_value();
  const bool total = metrics.oos_vega_weight_total.has_value();
  return in != total || (!in && metrics.oos_vega_weighted.has_value()) ||
         (in && *metrics.oos_vega_weight_total > 0.0 && !metrics.oos_vega_weighted.has_value());
}

[[nodiscard]] bool weighted_evidence_invalid(const CorpusQualityMetrics &metrics) noexcept {
  if (!metrics.oos_vega_weight_in_band.has_value() || !metrics.oos_vega_weight_total.has_value()) {
    return false;
  }
  const double in = *metrics.oos_vega_weight_in_band;
  const double total = *metrics.oos_vega_weight_total;
  if (!std::isfinite(in) || !std::isfinite(total) || in < 0.0 || total < 0.0 || in > total) {
    return true;
  }
  if (total == 0.0) {
    return metrics.oos_vega_weighted.has_value();
  }
  return !metrics.oos_vega_weighted.has_value() || !std::isfinite(*metrics.oos_vega_weighted) ||
         std::fabs(*metrics.oos_vega_weighted - in / total) >
             8.0 * std::numeric_limits<double>::epsilon();
}

[[nodiscard]] bool quality_out_of_range(const CorpusQualityMetrics &metrics) noexcept {
  return outside_unit_interval(metrics.fit_in_band) || outside_unit_interval(metrics.oos_in_band) ||
         outside_unit_interval(metrics.oos_vega_weighted) ||
         negative_measurement(metrics.mean_vol_rmse) ||
         negative_measurement(metrics.mean_reduced_chi2);
}

constexpr std::array<CorpusAdmissionReason, 15> kAdmissionPriority{
    CorpusAdmissionReason::InvalidRule,         CorpusAdmissionReason::SourceProvenanceUnavailable,
    CorpusAdmissionReason::QualityUnavailable,  CorpusAdmissionReason::NonFiniteMetric,
    CorpusAdmissionReason::MetricOutOfRange,    CorpusAdmissionReason::TooFewQuotes,
    CorpusAdmissionReason::TooFewSlices,        CorpusAdmissionReason::TooFewHoldouts,
    CorpusAdmissionReason::CalendarArbitrage,   CorpusAdmissionReason::InBandBelowFloor,
    CorpusAdmissionReason::OosInBandBelowFloor, CorpusAdmissionReason::OosVegaWeightedBelowFloor,
    CorpusAdmissionReason::VolRmseAboveCeiling, CorpusAdmissionReason::ReducedChi2AboveCeiling,
    CorpusAdmissionReason::RoundTripMismatch,
};

[[nodiscard]] CorpusAdmissionReason primary_reason(CorpusAdmissionFailureMask failures) noexcept {
  for (const CorpusAdmissionReason reason : kAdmissionPriority) {
    if ((failures & admission_reason_mask(reason)) != 0u) {
      return reason;
    }
  }
  return CorpusAdmissionReason::None;
}

} // namespace

bool CorpusAdmissionDecision::failed(CorpusAdmissionReason reason) const noexcept {
  return (failed_checks & admission_reason_mask(reason)) != 0u;
}

CorpusAdmissionDecision evaluate_corpus_admission(const CorpusQualityMetrics &metrics,
                                                  const CorpusAdmissionRule &rule) noexcept {
  CorpusAdmissionFailureMask failures = 0u;

  if (!valid_rule(rule)) {
    record_failure(failures, CorpusAdmissionReason::InvalidRule);
  }
  if (rule.require_source_provenance && !metrics.provenance_complete) {
    record_failure(failures, CorpusAdmissionReason::SourceProvenanceUnavailable);
  }
  if (required_metric_missing(metrics, rule)) {
    record_failure(failures, CorpusAdmissionReason::QualityUnavailable);
  }
  if (ratio_evidence_missing(metrics.fit_in_band, metrics.n_fit_in_band, metrics.n_fit_scorable) ||
      ratio_evidence_missing(metrics.oos_in_band, metrics.n_oos_in_band, metrics.n_holdout) ||
      weighted_evidence_missing(metrics)) {
    record_failure(failures, CorpusAdmissionReason::QualityUnavailable);
  }
  if (quality_has_non_finite(metrics)) {
    record_failure(failures, CorpusAdmissionReason::NonFiniteMetric);
  }
  if (quality_out_of_range(metrics)) {
    record_failure(failures, CorpusAdmissionReason::MetricOutOfRange);
  }
  if (ratio_evidence_invalid(metrics.fit_in_band, metrics.n_fit_in_band, metrics.n_fit_scorable) ||
      ratio_evidence_invalid(metrics.oos_in_band, metrics.n_oos_in_band, metrics.n_holdout) ||
      weighted_evidence_invalid(metrics)) {
    record_failure(failures, CorpusAdmissionReason::MetricOutOfRange);
  }
  if (!metrics.final_kind_consistent) {
    record_failure(failures, CorpusAdmissionReason::RoundTripMismatch);
  }
  if (metrics.n_two_sided < rule.min_quotes) {
    record_failure(failures, CorpusAdmissionReason::TooFewQuotes);
  }
  if (metrics.n_slices < rule.min_slices) {
    record_failure(failures, CorpusAdmissionReason::TooFewSlices);
  }
  if (metrics.n_holdout < rule.min_holdout) {
    record_failure(failures, CorpusAdmissionReason::TooFewHoldouts);
  }
  if (rule.require_calendar_arb_free && metrics.calendar_violations.has_value() &&
      *metrics.calendar_violations > 0u) {
    record_failure(failures, CorpusAdmissionReason::CalendarArbitrage);
  }
  if (rule.min_fit_in_band.has_value() && metrics.fit_in_band.has_value() &&
      *metrics.fit_in_band < *rule.min_fit_in_band) {
    record_failure(failures, CorpusAdmissionReason::InBandBelowFloor);
  }
  if (rule.min_oos_in_band.has_value() && metrics.oos_in_band.has_value() &&
      *metrics.oos_in_band < *rule.min_oos_in_band) {
    record_failure(failures, CorpusAdmissionReason::OosInBandBelowFloor);
  }
  if (rule.min_oos_vega_weighted.has_value() && metrics.oos_vega_weighted.has_value() &&
      *metrics.oos_vega_weighted < *rule.min_oos_vega_weighted) {
    record_failure(failures, CorpusAdmissionReason::OosVegaWeightedBelowFloor);
  }
  if (rule.max_mean_vol_rmse.has_value() && metrics.mean_vol_rmse.has_value() &&
      *metrics.mean_vol_rmse > *rule.max_mean_vol_rmse) {
    record_failure(failures, CorpusAdmissionReason::VolRmseAboveCeiling);
  }
  if (rule.max_mean_reduced_chi2.has_value() && metrics.mean_reduced_chi2.has_value() &&
      *metrics.mean_reduced_chi2 > *rule.max_mean_reduced_chi2) {
    record_failure(failures, CorpusAdmissionReason::ReducedChi2AboveCeiling);
  }

  if (failures == 0u) {
    return CorpusAdmissionDecision{CorpusDisposition::Admitted, CorpusAdmissionReason::None, 0u};
  }
  return CorpusAdmissionDecision{CorpusDisposition::Quarantined, primary_reason(failures),
                                 failures};
}

namespace {

// `FitSlot` + `fit_board` (the ONE per-board fit pipeline: OptionChain::from_frame
// -> PricerFitter::fit -> VolaSession::to_priced_surface, incl. uid-eligible
// curve/quality bookkeeping) moved to corpus_board_fit.{hpp,cpp} (T5) so
// `populate_surface_db` reuses the EXACT same path instead of duplicating it.
// See corpus_board_fit.hpp for the extracted contract; NOTE (why not
// calibrate_pool) lives there too.

// The per-date archive file path for `date` under `out_dir` (deterministic,
// forward-slash normalized).
[[nodiscard]] std::string archive_path_for(std::string_view out_dir, std::string_view date) {
  return (std::filesystem::path(out_dir) / (std::string(date) + ".atxvsa")).generic_string();
}

} // namespace

namespace {

void fingerprint_append_u64(std::string &out, std::uint64_t value) {
  char text[32];
  const auto [ptr, ec] = std::to_chars(text, text + sizeof text, value);
  (void)ec;
  out.append(text, static_cast<std::size_t>(ptr - text));
  out.push_back('|');
}

void fingerprint_append_text(std::string &out, std::string_view value) {
  fingerprint_append_u64(out, value.size());
  out.append(value);
  out.push_back('|');
}

void fingerprint_append_double(std::string &out, double value) {
  fingerprint_append_u64(out, std::bit_cast<std::uint64_t>(value));
}

void fingerprint_append_optional_double(std::string &out, const std::optional<double> &value) {
  fingerprint_append_u64(out, value.has_value() ? 1u : 0u);
  if (value.has_value()) {
    fingerprint_append_double(out, *value);
  }
}

void fingerprint_append_fit_context(std::string &out, const FitContext &context) {
  fingerprint_append_u64(out, context.profile_override.has_value() ? 1u : 0u);
  if (context.profile_override.has_value()) {
    fingerprint_append_u64(out, static_cast<std::uint64_t>(*context.profile_override));
  }
  fingerprint_append_u64(out, static_cast<std::uint64_t>(context.session_phase));
  fingerprint_append_u64(out, static_cast<std::uint64_t>(context.event_phase));
  fingerprint_append_u64(out, context.event_distance_days.has_value() ? 1u : 0u);
  if (context.event_distance_days.has_value()) {
    fingerprint_append_u64(out, *context.event_distance_days);
  }
  fingerprint_append_optional_double(out, context.forward_dispersion_bp);
  fingerprint_append_optional_double(out, context.median_q_eff);
  fingerprint_append_u64(out, context.htb.has_value() ? 1u : 0u);
  if (context.htb.has_value()) {
    fingerprint_append_u64(out, *context.htb ? 1u : 0u);
  }
  fingerprint_append_u64(out, context.vol_product ? 1u : 0u);
}

[[nodiscard]] std::uint64_t fingerprint_bytes(std::string_view bytes) noexcept {
  const std::uint64_t hash = atx::core::hash_bytes(bytes.data(), bytes.size());
  return hash == 0u ? 1u : hash;
}

[[nodiscard]] std::uint64_t fingerprint_corpus_inputs(std::span<const CorpusBoard> boards) {
  std::vector<std::size_t> order(boards.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&boards](std::size_t lhs, std::size_t rhs) {
    return boards[lhs].date != boards[rhs].date ? boards[lhs].date < boards[rhs].date
                                                : boards[lhs].symbol < boards[rhs].symbol;
  });
  std::string bytes;
  for (const std::size_t index : order) {
    const CorpusBoard &board = boards[index];
    fingerprint_append_text(bytes, board.date);
    fingerprint_append_text(bytes, board.symbol);
    fingerprint_append_u64(bytes, board.source_schema_version);
    fingerprint_append_u64(bytes, board.source_fingerprint);
    fingerprint_append_u64(bytes, board.market_input_fingerprint);
    fingerprint_append_u64(bytes, board.source_provenance_complete ? 1u : 0u);
    fingerprint_append_u64(bytes, board.curve.has_value() ? 1u : 0u);
    if (board.curve.has_value()) {
      fingerprint_append_u64(bytes, static_cast<std::uint64_t>(board.curve->kind));
    }
    fingerprint_append_fit_context(bytes, board.fit_context);
    fingerprint_append_text(bytes, board.frame.snapshot_iso);
    fingerprint_append_u64(bytes, static_cast<std::uint64_t>(board.frame.snapshot_ts_ns));
    fingerprint_append_double(bytes, board.frame.spot);
    for (std::size_t i = 0; i < board.frame.yc_pillar_t.size(); ++i) {
      fingerprint_append_double(bytes, board.frame.yc_pillar_t[i]);
      if (i < board.frame.yc_pillar_r.size()) {
        fingerprint_append_double(bytes, board.frame.yc_pillar_r[i]);
      }
    }
    for (const DividendEvent &dividend : board.frame.divs) {
      fingerprint_append_u64(bytes, static_cast<std::uint64_t>(dividend.ex_date_ns));
      fingerprint_append_double(bytes, dividend.amount);
    }
    for (const QuoteRow &row : board.frame.rows) {
      fingerprint_append_text(bytes, row.uid);
      fingerprint_append_text(bytes, row.expiry_iso);
      fingerprint_append_double(bytes, row.strike);
      fingerprint_append_u64(bytes, static_cast<std::uint64_t>(row.side));
      fingerprint_append_double(bytes, row.bid);
      fingerprint_append_double(bytes, row.ask);
      fingerprint_append_u64(bytes, static_cast<std::uint64_t>(row.bid_size));
      fingerprint_append_u64(bytes, static_cast<std::uint64_t>(row.ask_size));
      fingerprint_append_u64(bytes, static_cast<std::uint64_t>(row.ts_ns));
    }
  }
  return fingerprint_bytes(bytes);
}

void fingerprint_append_admission_rule(std::string &out, const CorpusAdmissionRule &rule) {
  fingerprint_append_u64(out, rule.min_quotes);
  fingerprint_append_u64(out, rule.min_slices);
  fingerprint_append_u64(out, rule.min_holdout);
  fingerprint_append_optional_double(out, rule.min_fit_in_band);
  fingerprint_append_optional_double(out, rule.min_oos_in_band);
  fingerprint_append_optional_double(out, rule.min_oos_vega_weighted);
  fingerprint_append_optional_double(out, rule.max_mean_vol_rmse);
  fingerprint_append_optional_double(out, rule.max_mean_reduced_chi2);
  fingerprint_append_u64(out, rule.require_calendar_arb_free ? 1u : 0u);
  fingerprint_append_double(out, rule.calendar_abs_k);
  fingerprint_append_u64(out, rule.require_source_provenance ? 1u : 0u);
}

[[nodiscard]] std::uint64_t fingerprint_corpus_policy(const QualifiedCorpusConfig &cfg) {
  std::string bytes;
  fingerprint_append_u64(bytes, cfg.admission.enabled ? 1u : 0u);
  for (const CorpusAdmissionRule &rule : cfg.admission.by_profile) {
    fingerprint_append_admission_rule(bytes, rule);
  }
  const PricerConfig &fit = cfg.build.fit_template;
  fingerprint_append_u64(bytes, static_cast<std::uint64_t>(fit.preset));
  fingerprint_append_u64(bytes, fit.curve.has_value() ? 1u : 0u);
  if (fit.curve.has_value()) {
    fingerprint_append_u64(bytes, static_cast<std::uint64_t>(fit.curve->kind));
  }
  fingerprint_append_u64(bytes, static_cast<std::uint64_t>(fit.policy.mode));
  fingerprint_append_double(bytes, fit.policy.min_direct_confidence);
  fingerprint_append_u64(bytes, fit.policy.validate_ambiguous ? 1u : 0u);
  fingerprint_append_u64(bytes, fit.policy.sparse_validation_floor);
  fingerprint_append_u64(bytes, fit.policy.dense_node_cap);
  fingerprint_append_u64(bytes, cfg.build.write_opts.flags);
  fingerprint_append_u64(bytes, cfg.build.write_opts.lookup_load_pct);
  fingerprint_append_u64(bytes, cfg.build.write_opts.blob_alignment);
  fingerprint_append_u64(bytes, cfg.build.write_opts.array_alignment);
  fingerprint_append_u64(bytes, static_cast<std::uint64_t>(cfg.build.write_opts.created_ts_ns));
  return fingerprint_bytes(bytes);
}

[[nodiscard]] std::string canonical_corpus_symbol(std::string_view symbol) {
  while (!symbol.empty() && symbol.front() == ' ') {
    symbol.remove_prefix(1u);
  }
  while (!symbol.empty() && symbol.back() == ' ') {
    symbol.remove_suffix(1u);
  }
  std::string canonical;
  canonical.reserve(std::min<std::size_t>(symbol.size(), 32u));
  for (const char ch : symbol.substr(0u, 32u)) {
    canonical.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - ('a' - 'A')) : ch);
  }
  return canonical;
}

[[nodiscard]] bool valid_source_failure_reason(CorpusAdmissionReason reason) noexcept {
  return reason == CorpusAdmissionReason::MissingSource ||
         reason == CorpusAdmissionReason::InvalidSourceSchema ||
         reason == CorpusAdmissionReason::AmbiguousSourceIdentity;
}

struct CorpusBuildArtifacts {
  CorpusManifest manifest{};
  std::optional<CorpusQualityReport> quality{};
  std::uint32_t peak_live_fitted_surfaces{0};
};

void count_disposition(CorpusQualityReport &report, CorpusDisposition disposition) noexcept;

[[nodiscard]] bool admitted_surface(const FitSlot &slot, bool qualified) noexcept {
  return slot.status == CorpusFitStatus::Ok &&
         (!qualified || slot.admission.disposition == CorpusDisposition::Admitted);
}

[[nodiscard]] Result<CorpusBuildArtifacts>
build_corpus_core(std::span<const CorpusBoard> boards, std::string_view out_dir,
                  const CorpusConfig &cfg, const CorpusAdmissionPolicy *admission,
                  std::uint64_t input_fingerprint, std::uint64_t policy_fingerprint,
                  bool write_sidecars) {
  if (boards.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_corpus: empty boards");
  }
  if (out_dir.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_corpus: empty out_dir");
  }

  {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(out_dir), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "build_corpus: cannot create out_dir");
    }
  }

  const std::size_t n = boards.size();
  if (n > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Err(ErrorCode::InvalidArgument, "build_corpus: too many boards");
  }
  const bool qualified = admission != nullptr;

  // ── Fan out board fits; each worker writes its own disjoint slot ───────────
  std::vector<FitSlot> slots(n);
  const auto run_range = [&boards, &slots, &cfg, admission](std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; ++i) {
      slots[i] = fit_board(boards[i], cfg.fit_template, admission);
    }
  };

  std::size_t n_workers = (cfg.n_threads != 0u)
                              ? static_cast<std::size_t>(cfg.n_threads)
                              : std::max<std::size_t>(1u, std::thread::hardware_concurrency());
  n_workers = std::min(n_workers, n); // n >= 1 here
  {
    std::vector<std::jthread> workers;
    workers.reserve(n_workers - 1u);
    const std::size_t base = n / n_workers;
    const std::size_t rem = n % n_workers;
    std::size_t pos = 0u;
    for (std::size_t w = 0u; w < n_workers; ++w) {
      const std::size_t sz = base + (w < rem ? std::size_t{1u} : std::size_t{0u});
      const std::size_t start = pos;
      const std::size_t end = pos + sz;
      pos = end;
      if (w + 1u < n_workers) {
        workers.emplace_back([&run_range, start, end] { run_range(start, end); });
      } else {
        run_range(start, end); // last chunk on the calling thread
      }
    }
    // workers join here (jthread RAII) before we read `slots`.
  }

  const std::size_t live_surfaces = static_cast<std::size_t>(std::count_if(
      slots.begin(), slots.end(), [](const FitSlot &slot) { return slot.surface.has_value(); }));

  // ── Deterministic output order: (date asc, symbol asc, board index) ────────
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&boards](std::size_t a, std::size_t b) noexcept {
    if (boards[a].date != boards[b].date) {
      return boards[a].date < boards[b].date;
    }
    if (boards[a].symbol != boards[b].symbol) {
      return boards[a].symbol < boards[b].symbol;
    }
    return a < b;
  });

  // ── Group by date: build entries, write one archive per date with Ok boards ─
  CorpusManifest man{};
  man.entries.reserve(n);
  man.n_boards = static_cast<std::uint32_t>(n);
  CorpusQualityReport quality{};
  if (qualified) {
    quality.input_fingerprint = input_fingerprint;
    quality.policy_fingerprint = policy_fingerprint;
    quality.entries.reserve(n);
    quality.n_planned = static_cast<std::uint32_t>(n);
  }

  std::size_t i = 0;
  while (i < n) {
    const std::string &date = boards[order[i]].date;
    std::size_t j = i;
    while (j < n && boards[order[j]].date == date) {
      ++j;
    }
    // group == order[i, j); already symbol-ascending within the date.
    const std::string apath = archive_path_for(out_dir, date);

    bool date_has_ok = false;
    for (std::size_t k = i; k < j; ++k) {
      if (admitted_surface(slots[order[k]], qualified)) {
        date_has_ok = true;
        break;
      }
    }

    man.dates.push_back(date);

    for (std::size_t k = i; k < j; ++k) {
      const std::size_t idx = order[k];
      const FitSlot &s = slots[idx];
      CorpusEntry e{};
      e.date = boards[idx].date;
      e.symbol = boards[idx].symbol;
      const bool admitted = admitted_surface(s, qualified);
      if (qualified && s.status == CorpusFitStatus::Ok && !admitted) {
        e.status = CorpusFitStatus::Failed;
        e.error_code = ErrorCode::Unavailable;
      } else {
        e.status = s.status;
        e.chosen_kind = s.chosen_kind;
        e.n_slices = s.n_slices;
        e.oos_in_band = s.oos_in_band;
        e.error_code = s.error_code;
      }
      if (admitted && date_has_ok) {
        e.archive_path = apath;
      }
      switch (e.status) {
      case CorpusFitStatus::Ok:
        ++man.n_ok;
        break;
      case CorpusFitStatus::Failed:
        ++man.n_failed;
        break;
      case CorpusFitStatus::Skipped:
        ++man.n_skipped;
        break;
      }
      man.entries.push_back(std::move(e));

      if (qualified) {
        QualifiedCorpusEntry qe{};
        qe.date = boards[idx].date;
        qe.symbol = boards[idx].symbol;
        qe.disposition = s.admission.disposition;
        qe.primary_reason = s.admission.primary_reason;
        qe.failed_checks = s.admission.failed_checks;
        qe.source_or_fit_error = s.error_code;
        qe.quality = s.quality;
        if (admitted && date_has_ok) {
          qe.archive_path = apath;
        }
        count_disposition(quality, qe.disposition);
        quality.entries.push_back(std::move(qe));
      }
    }

    if (date_has_ok) {
      // Distinct per-symbol uid at write (S1-1 — the multi-name northstar
      // blocker). Each board fits in its own single-symbol `Universe`, so
      // `slots[idx].surface`'s in-memory uid is ALWAYS 1 (universe.cpp:71 — the
      // sole interned ticker of a fresh Universe). Left unstamped, a date with
      // more than one Ok symbol would archive every surface at uid=1 and
      // `MarketSnapshot::load`'s `SurfaceSet::create` would reject the archive
      // ("duplicate uid", portfolio_pricer.cpp). Stamp a symbol-derived uid
      // (`uid_for_symbol`) onto an ARCHIVED COPY — `with_uid` deep-clones
      // curves + context, a one-time cost at corpus-write time, not the pricing
      // hot path — so the in-memory `slots[idx].surface` (and any live session
      // built from the same board) is untouched: single-symbol served/session
      // pricing keeps uid=1 exactly as before.
      std::vector<PricedSurface> restamped; // owns this date's uid-corrected copies
      restamped.reserve(j - i);
      // Non-owning items into `restamped` (reserved above, so no reallocation
      // invalidates the pointers taken below) + the still-live `boards` storage
      // (outlives this write). Symbol-ascending, matching the archive's own
      // directory sort.
      std::vector<SurfaceArchiveItem> items;
      items.reserve(j - i);
      for (std::size_t k = i; k < j; ++k) {
        const std::size_t idx = order[k];
        if (admitted_surface(slots[idx], qualified)) {
          const std::uint32_t uid = uid_for_symbol(boards[idx].symbol);
          Result<PricedSurface> stamped = with_uid(slots[idx].surface.value(), uid);
          if (!stamped) {
            return Err(stamped.error());
          }
          restamped.push_back(std::move(*stamped));
          items.push_back(SurfaceArchiveItem{boards[idx].symbol, &restamped.back()});
        }
      }
      const Status w = write_surface_archive_file(apath, items, cfg.write_opts);
      if (!w) {
        return Err(w.error()); // propagate IoError / AlreadyExists
      }
    }

    i = j;
  }

  // ── Manifest file ──────────────────────────────────────────────────────────
  if (write_sidecars) {
    const std::string mpath = (std::filesystem::path(out_dir) / "manifest.tsv").generic_string();
    ATX_TRY_VOID(write_manifest_file(mpath, man));
  }

  CorpusBuildArtifacts artifacts;
  artifacts.manifest = std::move(man);
  artifacts.peak_live_fitted_surfaces = saturated_u32(live_surfaces);
  if (qualified) {
    if (write_sidecars) {
      const std::string qpath = (std::filesystem::path(out_dir) / "quality.tsv").generic_string();
      ATX_TRY_VOID(write_quality_report_file(qpath, quality));
    }
    artifacts.quality = std::move(quality);
  }
  return Ok(std::move(artifacts));
}

struct CorpusDateCheckpoint {
  CorpusManifest manifest{};
  CorpusQualityReport quality{};
};

[[nodiscard]] std::filesystem::path checkpoint_path(std::string_view out_dir, std::string_view date,
                                                    std::string_view suffix) {
  return std::filesystem::path(out_dir) / ".checkpoints" /
         (std::string(date) + std::string(suffix));
}

[[nodiscard]] Result<std::optional<CorpusDateCheckpoint>>
read_date_checkpoint(std::string_view out_dir, std::string_view date,
                     std::span<const std::string> expected_symbols, std::uint64_t input_fingerprint,
                     std::uint64_t policy_fingerprint) {
  const std::filesystem::path manifest_path = checkpoint_path(out_dir, date, ".manifest.tsv");
  const std::filesystem::path quality_path = checkpoint_path(out_dir, date, ".quality.tsv");
  std::error_code manifest_error;
  std::error_code quality_error;
  const bool manifest_exists = std::filesystem::exists(manifest_path, manifest_error);
  const bool quality_exists = std::filesystem::exists(quality_path, quality_error);
  if (manifest_error || quality_error) {
    return Err(ErrorCode::IoError, "CorpusBuildSession: cannot inspect date checkpoint");
  }
  if (!manifest_exists && !quality_exists) {
    return Ok(std::optional<CorpusDateCheckpoint>{});
  }
  if (manifest_exists != quality_exists) {
    return Err(ErrorCode::AlreadyExists, "CorpusBuildSession: incomplete date checkpoint");
  }

  ATX_TRY(CorpusManifest manifest, read_manifest_file(manifest_path.generic_string()));
  ATX_TRY(CorpusQualityReport quality, read_quality_report_file(quality_path.generic_string()));
  if (quality.input_fingerprint != input_fingerprint ||
      quality.policy_fingerprint != policy_fingerprint) {
    return Err(ErrorCode::AlreadyExists,
               "CorpusBuildSession: date checkpoint fingerprint mismatch");
  }
  if (manifest.dates != std::vector<std::string>{std::string(date)} ||
      manifest.entries.size() != expected_symbols.size() ||
      quality.entries.size() != expected_symbols.size() ||
      manifest.n_boards != expected_symbols.size() ||
      quality.n_planned != expected_symbols.size()) {
    return Err(ErrorCode::ParseError, "CorpusBuildSession: date checkpoint shape mismatch");
  }

  std::uint32_t admitted = 0u;
  const std::filesystem::path expected_archive =
      std::filesystem::path(out_dir) / (std::string(date) + ".atxvsa");
  for (std::size_t i = 0u; i < expected_symbols.size(); ++i) {
    const CorpusEntry &legacy = manifest.entries[i];
    const QualifiedCorpusEntry &qualified = quality.entries[i];
    if (legacy.date != date || qualified.date != date || legacy.symbol != expected_symbols[i] ||
        qualified.symbol != expected_symbols[i] || legacy.symbol != qualified.symbol) {
      return Err(ErrorCode::ParseError, "CorpusBuildSession: date checkpoint key mismatch");
    }
    const bool is_admitted = qualified.disposition == CorpusDisposition::Admitted;
    if (is_admitted != (legacy.status == CorpusFitStatus::Ok)) {
      return Err(ErrorCode::ParseError, "CorpusBuildSession: date checkpoint disposition mismatch");
    }
    if (is_admitted) {
      ++admitted;
      if (std::filesystem::path(legacy.archive_path) != expected_archive ||
          std::filesystem::path(qualified.archive_path) != expected_archive) {
        return Err(ErrorCode::ParseError, "CorpusBuildSession: date checkpoint archive mismatch");
      }
    } else if (!legacy.archive_path.empty() || !qualified.archive_path.empty()) {
      return Err(ErrorCode::ParseError,
                 "CorpusBuildSession: rejected checkpoint row names an archive");
    }
  }

  std::error_code archive_error;
  const bool archive_exists = std::filesystem::exists(expected_archive, archive_error);
  if (archive_error || archive_exists != (admitted != 0u)) {
    return Err(ErrorCode::AlreadyExists, "CorpusBuildSession: date checkpoint archive unavailable");
  }
  if (archive_exists) {
    ATX_TRY(SurfaceArchive archive, SurfaceArchive::open_file(expected_archive.generic_string()));
    if (archive.count() != admitted) {
      return Err(ErrorCode::ParseError,
                 "CorpusBuildSession: date checkpoint archive count mismatch");
    }
    ATX_TRY(std::vector<PricedSurface> mapped, archive.map_all());
    if (mapped.size() != admitted) {
      return Err(ErrorCode::ParseError, "CorpusBuildSession: date checkpoint archive map mismatch");
    }
    for (const CorpusEntry &entry : manifest.entries) {
      if (entry.status == CorpusFitStatus::Ok && !archive.find(entry.symbol)) {
        return Err(ErrorCode::ParseError,
                   "CorpusBuildSession: date checkpoint archive symbol mismatch");
      }
    }
  }

  return Ok(std::optional<CorpusDateCheckpoint>{
      CorpusDateCheckpoint{std::move(manifest), std::move(quality)}});
}

[[nodiscard]] Status write_date_checkpoint(std::string_view out_dir, std::string_view date,
                                           std::uint64_t input_fingerprint,
                                           std::uint64_t policy_fingerprint,
                                           std::span<const CorpusEntry> manifest_entries,
                                           std::span<const QualifiedCorpusEntry> quality_entries) {
  CorpusManifest manifest;
  manifest.dates.emplace_back(date);
  manifest.entries.assign(manifest_entries.begin(), manifest_entries.end());
  manifest.n_boards = saturated_u32(manifest.entries.size());
  for (const CorpusEntry &entry : manifest.entries) {
    switch (entry.status) {
    case CorpusFitStatus::Ok:
      ++manifest.n_ok;
      break;
    case CorpusFitStatus::Failed:
      ++manifest.n_failed;
      break;
    case CorpusFitStatus::Skipped:
      ++manifest.n_skipped;
      break;
    }
  }

  CorpusQualityReport quality;
  quality.input_fingerprint = input_fingerprint;
  quality.policy_fingerprint = policy_fingerprint;
  quality.entries.assign(quality_entries.begin(), quality_entries.end());
  quality.n_planned = saturated_u32(quality.entries.size());
  for (const QualifiedCorpusEntry &entry : quality.entries) {
    count_disposition(quality, entry.disposition);
  }

  const std::filesystem::path manifest_path = checkpoint_path(out_dir, date, ".manifest.tsv");
  const std::filesystem::path quality_path = checkpoint_path(out_dir, date, ".quality.tsv");
  const std::filesystem::path manifest_pending = manifest_path.string() + ".pending";
  const std::filesystem::path quality_pending = quality_path.string() + ".pending";
  ATX_TRY_VOID(write_manifest_file(manifest_pending.generic_string(), manifest));
  const Status quality_write = write_quality_report_file(quality_pending.generic_string(), quality);
  if (!quality_write) {
    std::error_code cleanup;
    std::filesystem::remove(manifest_pending, cleanup);
    return Err(quality_write.error());
  }

  std::error_code rename_error;
  std::filesystem::rename(manifest_pending, manifest_path, rename_error);
  if (rename_error) {
    std::error_code cleanup;
    std::filesystem::remove(manifest_pending, cleanup);
    std::filesystem::remove(quality_pending, cleanup);
    return Err(ErrorCode::IoError, "CorpusBuildSession: manifest checkpoint commit failed");
  }
  std::filesystem::rename(quality_pending, quality_path, rename_error);
  if (rename_error) {
    std::error_code cleanup;
    std::filesystem::remove(quality_pending, cleanup);
    std::filesystem::remove(manifest_path, cleanup);
    return Err(ErrorCode::IoError, "CorpusBuildSession: quality checkpoint commit failed");
  }
  return Ok();
}

} // namespace

CorpusBuildSession::CorpusBuildSession(std::string out_dir, QualifiedCorpusConfig cfg)
    : out_dir_{std::move(out_dir)}, cfg_{std::move(cfg)} {
  quality_.input_fingerprint = cfg_.input_fingerprint;
  quality_.policy_fingerprint =
      cfg_.policy_fingerprint != 0u ? cfg_.policy_fingerprint : fingerprint_corpus_policy(cfg_);
}

Result<CorpusBuildSession> CorpusBuildSession::create(std::string_view out_dir,
                                                      const QualifiedCorpusConfig &cfg) {
  if (out_dir.empty()) {
    return Err(ErrorCode::InvalidArgument, "CorpusBuildSession::create: empty out_dir");
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(out_dir), ec);
  if (ec) {
    return Err(ErrorCode::IoError, "CorpusBuildSession::create: cannot create out_dir");
  }
  return Ok(CorpusBuildSession{std::string(out_dir), cfg});
}

Status CorpusBuildSession::append_date(std::string_view date,
                                       std::span<const CorpusCellInput> cells) {
  if (finished_) {
    return Err(ErrorCode::InvalidArgument,
               "CorpusBuildSession::append_date: session already finished");
  }
  if (date.empty() || cells.empty() || (!last_date_.empty() && date <= last_date_)) {
    return Err(ErrorCode::InvalidArgument,
               "CorpusBuildSession::append_date: dates must be nonempty and strictly ascending");
  }

  std::vector<CorpusBoard> boards;
  std::vector<CorpusSourceFailure> source_failures;
  std::vector<std::string> symbols;
  std::vector<std::pair<std::string, std::string>> fingerprint_cells;
  std::string date_fingerprint_material;
  boards.reserve(cells.size());
  source_failures.reserve(cells.size());
  symbols.reserve(cells.size());
  for (const CorpusCellInput &cell : cells) {
    if (const CorpusBoard *board = std::get_if<CorpusBoard>(&cell)) {
      if (board->date != date) {
        return Err(ErrorCode::InvalidArgument,
                   "CorpusBuildSession::append_date: board date mismatch");
      }
      CorpusBoard copy = *board;
      copy.symbol = canonical_corpus_symbol(copy.symbol);
      if (copy.symbol.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "CorpusBuildSession::append_date: empty canonical symbol");
      }
      symbols.push_back(copy.symbol);
      const std::uint64_t cell_fingerprint =
          fingerprint_corpus_inputs(std::span<const CorpusBoard>(&copy, 1u));
      std::string material;
      fingerprint_append_text(material, date);
      fingerprint_append_text(material, copy.symbol);
      fingerprint_append_u64(material, cell_fingerprint);
      fingerprint_cells.emplace_back(copy.symbol, std::move(material));
      boards.push_back(std::move(copy));
    } else {
      CorpusSourceFailure failure = std::get<CorpusSourceFailure>(cell);
      if (failure.date != date || !valid_source_failure_reason(failure.reason)) {
        return Err(ErrorCode::InvalidArgument,
                   "CorpusBuildSession::append_date: invalid source-failure cell");
      }
      failure.symbol = canonical_corpus_symbol(failure.symbol);
      if (failure.symbol.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "CorpusBuildSession::append_date: empty canonical symbol");
      }
      symbols.push_back(failure.symbol);
      std::string material;
      fingerprint_append_text(material, date);
      fingerprint_append_text(material, failure.symbol);
      fingerprint_append_u64(material, static_cast<std::uint64_t>(failure.reason));
      fingerprint_append_u64(material, failure.source_schema_version);
      fingerprint_append_u64(material, failure.source_fingerprint);
      fingerprint_append_u64(material, failure.market_input_fingerprint);
      fingerprint_cells.emplace_back(failure.symbol, std::move(material));
      source_failures.push_back(std::move(failure));
    }
  }
  std::sort(symbols.begin(), symbols.end());
  if (std::adjacent_find(symbols.begin(), symbols.end()) != symbols.end()) {
    return Err(ErrorCode::AlreadyExists,
               "CorpusBuildSession::append_date: duplicate canonical symbol");
  }
  std::sort(fingerprint_cells.begin(), fingerprint_cells.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  for (const auto &[symbol, material] : fingerprint_cells) {
    (void)symbol;
    date_fingerprint_material.append(material);
  }
  const std::uint64_t date_input_fingerprint = fingerprint_bytes(date_fingerprint_material);
  if (cells.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      static_cast<std::uint64_t>(manifest_.n_boards) + cells.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return Err(ErrorCode::InvalidArgument,
               "CorpusBuildSession::append_date: too many planned cells");
  }

  std::vector<CorpusEntry> manifest_entries;
  std::vector<QualifiedCorpusEntry> quality_entries;
  manifest_entries.reserve(cells.size());
  quality_entries.reserve(cells.size());
  std::uint32_t date_peak = 0u;
  ATX_TRY(std::optional<CorpusDateCheckpoint> checkpoint,
          read_date_checkpoint(out_dir_, date, symbols, date_input_fingerprint,
                               quality_.policy_fingerprint));
  if (checkpoint.has_value()) {
    manifest_entries = std::move(checkpoint->manifest.entries);
    quality_entries = std::move(checkpoint->quality.entries);
  } else {
    if (!boards.empty()) {
      ATX_TRY(CorpusBuildArtifacts artifacts,
              build_corpus_core(boards, out_dir_, cfg_.build, &cfg_.admission,
                                quality_.input_fingerprint, quality_.policy_fingerprint, false));
      date_peak = artifacts.peak_live_fitted_surfaces;
      manifest_entries = std::move(artifacts.manifest.entries);
      if (!artifacts.quality.has_value()) {
        return Err(ErrorCode::Internal, "CorpusBuildSession::append_date: quality unavailable");
      }
      quality_entries = std::move(artifacts.quality->entries);
    }

    for (const CorpusSourceFailure &failure : source_failures) {
      CorpusEntry legacy;
      legacy.date = failure.date;
      legacy.symbol = failure.symbol;
      legacy.status = CorpusFitStatus::Failed;
      legacy.error_code = failure.error_code;
      manifest_entries.push_back(std::move(legacy));

      QualifiedCorpusEntry quality;
      quality.date = failure.date;
      quality.symbol = failure.symbol;
      quality.disposition = CorpusDisposition::SourceFailed;
      quality.primary_reason = failure.reason;
      quality.failed_checks = admission_reason_mask(failure.reason);
      quality.source_or_fit_error = failure.error_code;
      quality.quality.source_schema_version = failure.source_schema_version;
      quality.quality.source_fingerprint = failure.source_fingerprint;
      quality.quality.market_input_fingerprint = failure.market_input_fingerprint;
      quality_entries.push_back(std::move(quality));
    }

    std::sort(
        manifest_entries.begin(), manifest_entries.end(),
        [](const CorpusEntry &lhs, const CorpusEntry &rhs) { return lhs.symbol < rhs.symbol; });
    std::sort(quality_entries.begin(), quality_entries.end(),
              [](const QualifiedCorpusEntry &lhs, const QualifiedCorpusEntry &rhs) {
                return lhs.symbol < rhs.symbol;
              });
    ATX_TRY_VOID(write_date_checkpoint(out_dir_, date, date_input_fingerprint,
                                       quality_.policy_fingerprint, manifest_entries,
                                       quality_entries));
  }

  manifest_.dates.emplace_back(date);
  for (CorpusEntry &entry : manifest_entries) {
    switch (entry.status) {
    case CorpusFitStatus::Ok:
      ++manifest_.n_ok;
      break;
    case CorpusFitStatus::Failed:
      ++manifest_.n_failed;
      break;
    case CorpusFitStatus::Skipped:
      ++manifest_.n_skipped;
      break;
    }
    manifest_.entries.push_back(std::move(entry));
  }
  for (QualifiedCorpusEntry &entry : quality_entries) {
    count_disposition(quality_, entry.disposition);
    quality_.entries.push_back(std::move(entry));
  }
  manifest_.n_boards += saturated_u32(cells.size());
  quality_.n_planned += saturated_u32(cells.size());
  peak_live_fitted_surfaces_ = std::max(peak_live_fitted_surfaces_, date_peak);
  input_fingerprint_material_.append(date_fingerprint_material);
  last_date_ = std::string(date);
  return Ok();
}

Result<QualifiedCorpusManifest> CorpusBuildSession::finish() {
  if (finished_ || manifest_.dates.empty()) {
    return Err(ErrorCode::InvalidArgument, "CorpusBuildSession::finish: empty or already finished");
  }
  if (quality_.input_fingerprint == 0u) {
    quality_.input_fingerprint = fingerprint_bytes(input_fingerprint_material_);
  }
  const std::string manifest_path =
      (std::filesystem::path(out_dir_) / "manifest.tsv").generic_string();
  const std::string quality_path =
      (std::filesystem::path(out_dir_) / "quality.tsv").generic_string();
  std::error_code manifest_exists_error;
  std::error_code quality_exists_error;
  const bool manifest_exists = std::filesystem::exists(manifest_path, manifest_exists_error);
  const bool quality_exists = std::filesystem::exists(quality_path, quality_exists_error);
  if (manifest_exists_error || quality_exists_error) {
    return Err(ErrorCode::IoError, "CorpusBuildSession::finish: cannot inspect final indexes");
  }
  if (manifest_exists || quality_exists) {
    if (manifest_exists != quality_exists) {
      return Err(ErrorCode::AlreadyExists,
                 "CorpusBuildSession::finish: incomplete existing final indexes");
    }
    ATX_TRY(CorpusManifest existing_manifest, read_manifest_file(manifest_path));
    ATX_TRY(CorpusQualityReport existing_quality, read_quality_report_file(quality_path));
    if (existing_manifest != manifest_ || existing_quality != quality_) {
      return Err(ErrorCode::AlreadyExists,
                 "CorpusBuildSession::finish: existing final indexes do not match");
    }
    finished_ = true;
    return Ok(QualifiedCorpusManifest{std::move(manifest_), std::move(quality_),
                                      peak_live_fitted_surfaces_});
  }
  const std::string manifest_pending = manifest_path + ".pending";
  const std::string quality_pending = quality_path + ".pending";
  ATX_TRY_VOID(write_manifest_file(manifest_pending, manifest_));
  const Status quality_write = write_quality_report_file(quality_pending, quality_);
  if (!quality_write) {
    std::error_code cleanup;
    std::filesystem::remove(manifest_pending, cleanup);
    return Err(quality_write.error());
  }
  std::error_code rename_error;
  std::filesystem::rename(manifest_pending, manifest_path, rename_error);
  if (rename_error) {
    std::error_code cleanup;
    std::filesystem::remove(manifest_pending, cleanup);
    std::filesystem::remove(quality_pending, cleanup);
    return Err(ErrorCode::IoError, "CorpusBuildSession::finish: manifest commit failed");
  }
  std::filesystem::rename(quality_pending, quality_path, rename_error);
  if (rename_error) {
    std::error_code cleanup;
    std::filesystem::remove(quality_pending, cleanup);
    std::filesystem::remove(manifest_path, cleanup);
    return Err(ErrorCode::IoError, "CorpusBuildSession::finish: quality commit failed");
  }
  finished_ = true;
  return Ok(QualifiedCorpusManifest{std::move(manifest_), std::move(quality_),
                                    peak_live_fitted_surfaces_});
}

Result<CorpusManifest> build_corpus(std::span<const CorpusBoard> boards, std::string_view out_dir,
                                    const CorpusConfig &cfg) {
  ATX_TRY(CorpusBuildArtifacts artifacts,
          build_corpus_core(boards, out_dir, cfg, nullptr, 0u, 0u, true));
  return Ok(std::move(artifacts.manifest));
}

Result<QualifiedCorpusManifest> build_qualified_corpus(std::span<const CorpusBoard> boards,
                                                       std::string_view out_dir,
                                                       const QualifiedCorpusConfig &cfg) {
  if (boards.empty() || out_dir.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_qualified_corpus: empty boards or out_dir");
  }
  QualifiedCorpusConfig resolved = cfg;
  if (resolved.input_fingerprint == 0u) {
    resolved.input_fingerprint = fingerprint_corpus_inputs(boards);
  }
  if (resolved.policy_fingerprint == 0u) {
    resolved.policy_fingerprint = fingerprint_corpus_policy(resolved);
  }
  ATX_TRY(CorpusBuildSession session, CorpusBuildSession::create(out_dir, resolved));

  std::vector<std::size_t> order(boards.size());
  for (std::size_t i = 0u; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&boards](std::size_t lhs, std::size_t rhs) {
    if (boards[lhs].date != boards[rhs].date) {
      return boards[lhs].date < boards[rhs].date;
    }
    if (boards[lhs].symbol != boards[rhs].symbol) {
      return boards[lhs].symbol < boards[rhs].symbol;
    }
    return lhs < rhs;
  });
  std::size_t first = 0u;
  while (first < order.size()) {
    const std::string &date = boards[order[first]].date;
    std::size_t last = first + 1u;
    while (last < order.size() && boards[order[last]].date == date) {
      ++last;
    }
    std::vector<CorpusCellInput> cells;
    cells.reserve(last - first);
    for (std::size_t i = first; i < last; ++i) {
      cells.emplace_back(boards[order[i]]);
    }
    ATX_TRY_VOID(session.append_date(date, cells));
    first = last;
  }
  return session.finish();
}

// ── Manifest TSV (de)serialization ──────────────────────────────────────────

namespace {

constexpr std::string_view kManifestMagic = "atx-corpus-manifest\tv1";
constexpr std::string_view kQualityReportMagic = "atx-corpus-quality\tv1";
constexpr std::string_view kQualityColumns =
    "date\tsymbol\tdisposition\tprimary_reason\tfailed_checks\terror_code\tprofile\t"
    "decision_source\tpreset\tprimary_kind\tfinal_kind\tused_fallback\tcurve_pinned\t"
    "final_kind_consistent\tprovenance_complete\tsource_schema_version\t"
    "source_fingerprint\tmarket_input_fingerprint\tdividend_treatment\t"
    "n_cash_dividends\tn_raw_quotes\tn_two_sided\tn_slices\tn_holdout\t"
    "n_fit_scorable\tn_fit_in_band\tn_oos_in_band\tfit_in_band\t"
    "oos_in_band\toos_vega_weighted\toos_vega_weight_in_band\t"
    "oos_vega_weight_total\tmean_vol_rmse\tmean_reduced_chi2\t"
    "calendar_violations\tarchive_path";
constexpr std::size_t kQualityFieldCount = 36u;

// Append an unsigned integer as decimal text.
void append_u32(std::string &out, std::uint32_t v) {
  char buf[16];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
  (void)ec; // to_chars on a 16-byte buffer for a uint32 never fails
  out.append(buf, static_cast<std::size_t>(ptr - buf));
}

void append_u64(std::string &out, std::uint64_t v) {
  char buf[32];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
  (void)ec; // 32 bytes is sufficient for a uint64
  out.append(buf, static_cast<std::size_t>(ptr - buf));
}

// Append a double at shortest round-trip precision (locale-independent).
void append_double(std::string &out, double v) {
  char buf[64];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
  (void)ec; // 64 bytes is always sufficient for a double
  out.append(buf, static_cast<std::size_t>(ptr - buf));
}

void append_optional_double(std::string &out, const std::optional<double> &v) {
  if (!v.has_value()) {
    out.append("NA");
    return;
  }
  append_double(out, *v);
}

void append_optional_u32(std::string &out, const std::optional<std::uint32_t> &v) {
  if (!v.has_value()) {
    out.append("NA");
    return;
  }
  append_u32(out, *v);
}

// Split `line` on '\t', preserving empty fields (including a trailing one).
[[nodiscard]] std::vector<std::string_view> split_tabs(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return out;
}

// Split `text` on '\n', dropping a single trailing empty line (the final '\n').
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      if (start < text.size()) {
        out.push_back(text.substr(start));
      }
      break;
    }
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

[[nodiscard]] bool parse_u32(std::string_view sv, std::uint32_t &out) noexcept {
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc() && ptr == sv.data() + sv.size();
}

[[nodiscard]] bool parse_u64(std::string_view sv, std::uint64_t &out) noexcept {
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc() && ptr == sv.data() + sv.size();
}

[[nodiscard]] bool parse_double(std::string_view sv, double &out) noexcept {
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc() && ptr == sv.data() + sv.size();
}

[[nodiscard]] bool parse_optional_double(std::string_view sv, std::optional<double> &out) noexcept {
  if (sv == "NA") {
    out.reset();
    return true;
  }
  double value = 0.0;
  if (!parse_double(sv, value)) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_optional_u32(std::string_view sv,
                                      std::optional<std::uint32_t> &out) noexcept {
  if (sv == "NA") {
    out.reset();
    return true;
  }
  std::uint32_t value = 0u;
  if (!parse_u32(sv, value)) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_bool(std::string_view sv, bool &out) noexcept {
  if (sv == "0") {
    out = false;
    return true;
  }
  if (sv == "1") {
    out = true;
    return true;
  }
  return false;
}

[[nodiscard]] bool to_fit_status(std::uint32_t v, CorpusFitStatus &out) noexcept {
  switch (v) {
  case 0u:
    out = CorpusFitStatus::Ok;
    return true;
  case 1u:
    out = CorpusFitStatus::Failed;
    return true;
  case 2u:
    out = CorpusFitStatus::Skipped;
    return true;
  default:
    return false;
  }
}

// The writer emits static_cast<uint32_t>(chosen_kind) for whichever kind the
// selector picked, so this reader must accept every VolCurveKind or the manifest
// fails its own round-trip. Switching on the enum with no `default:` lets
// -Wswitch -Werror reject a new kind that forgets to update this mapping.
[[nodiscard]] bool to_curve_kind(std::uint32_t v, VolCurveKind &out) noexcept {
  if (v > 0xFFu) {
    return false;
  }
  const auto kind = static_cast<VolCurveKind>(static_cast<std::uint8_t>(v));
  switch (kind) {
  case VolCurveKind::ConvexDense:
  case VolCurveKind::Essvi:
  case VolCurveKind::Svi:
  case VolCurveKind::LinearVariance:
  case VolCurveKind::C8:
    out = kind;
    return true;
  }
  return false; // unknown kind: manifest written by a newer build
}

// ErrorCode spans 0..10 (see atx/core/error.hpp).
[[nodiscard]] bool to_error_code(std::uint32_t v, ErrorCode &out) noexcept {
  if (v > static_cast<std::uint32_t>(ErrorCode::ParseError)) {
    return false;
  }
  out = static_cast<ErrorCode>(v);
  return true;
}

[[nodiscard]] bool to_disposition(std::uint32_t v, CorpusDisposition &out) noexcept {
  if (v > static_cast<std::uint32_t>(CorpusDisposition::Empty)) {
    return false;
  }
  out = static_cast<CorpusDisposition>(v);
  return true;
}

[[nodiscard]] bool to_admission_reason(std::uint32_t v, CorpusAdmissionReason &out) noexcept {
  if (v >= static_cast<std::uint32_t>(CorpusAdmissionReason::Count)) {
    return false;
  }
  out = static_cast<CorpusAdmissionReason>(v);
  return true;
}

[[nodiscard]] bool to_profile_kind(std::uint32_t v, ProfileKind &out) noexcept {
  if (v >= kProfileKindCount) {
    return false;
  }
  out = static_cast<ProfileKind>(v);
  return true;
}

[[nodiscard]] bool to_decision_source(std::uint32_t v, FitDecisionSource &out) noexcept {
  if (v > static_cast<std::uint32_t>(FitDecisionSource::CrossValidation)) {
    return false;
  }
  out = static_cast<FitDecisionSource>(v);
  return true;
}

[[nodiscard]] bool to_fit_preset(std::uint32_t v, FitPreset &out) noexcept {
  if (v > static_cast<std::uint32_t>(FitPreset::Hft)) {
    return false;
  }
  out = static_cast<FitPreset>(v);
  return true;
}

[[nodiscard]] bool to_dividend_treatment(std::uint32_t v, CorpusDividendTreatment &out) noexcept {
  if (v != static_cast<std::uint32_t>(CorpusDividendTreatment::EscrowedForward)) {
    return false;
  }
  out = CorpusDividendTreatment::EscrowedForward;
  return true;
}

[[nodiscard]] bool valid_failure_mask(CorpusAdmissionFailureMask mask) noexcept {
  constexpr unsigned count = static_cast<unsigned>(CorpusAdmissionReason::Count);
  constexpr CorpusAdmissionFailureMask valid = (CorpusAdmissionFailureMask{1u} << count) - 1u;
  constexpr CorpusAdmissionFailureMask none_bit = 1u;
  return (mask & ~valid) == 0u && (mask & none_bit) == 0u;
}

void append_quality_entry(std::string &out, const QualifiedCorpusEntry &e) {
  out.append(e.date);
  out.push_back('\t');
  out.append(e.symbol);
  for (const std::uint32_t v :
       {static_cast<std::uint32_t>(e.disposition), static_cast<std::uint32_t>(e.primary_reason),
        e.failed_checks, static_cast<std::uint32_t>(e.source_or_fit_error),
        static_cast<std::uint32_t>(e.quality.profile),
        static_cast<std::uint32_t>(e.quality.decision_source),
        static_cast<std::uint32_t>(e.quality.preset),
        static_cast<std::uint32_t>(e.quality.primary_kind),
        static_cast<std::uint32_t>(e.quality.final_kind), e.quality.used_fallback ? 1u : 0u,
        e.quality.curve_pinned ? 1u : 0u, e.quality.final_kind_consistent ? 1u : 0u,
        e.quality.provenance_complete ? 1u : 0u}) {
    out.push_back('\t');
    append_u32(out, v);
  }
  out.push_back('\t');
  append_u32(out, e.quality.source_schema_version);
  out.push_back('\t');
  append_u64(out, e.quality.source_fingerprint);
  out.push_back('\t');
  append_u64(out, e.quality.market_input_fingerprint);
  for (const std::uint32_t v :
       {static_cast<std::uint32_t>(e.quality.dividend_treatment), e.quality.n_cash_dividends,
        e.quality.n_raw_quotes, e.quality.n_two_sided, e.quality.n_slices, e.quality.n_holdout,
        e.quality.n_fit_scorable, e.quality.n_fit_in_band, e.quality.n_oos_in_band}) {
    out.push_back('\t');
    append_u32(out, v);
  }
  for (const std::optional<double> &v :
       {e.quality.fit_in_band, e.quality.oos_in_band, e.quality.oos_vega_weighted,
        e.quality.oos_vega_weight_in_band, e.quality.oos_vega_weight_total, e.quality.mean_vol_rmse,
        e.quality.mean_reduced_chi2}) {
    out.push_back('\t');
    append_optional_double(out, v);
  }
  out.push_back('\t');
  append_optional_u32(out, e.quality.calendar_violations);
  out.push_back('\t');
  out.append(e.archive_path);
  out.push_back('\n');
}

[[nodiscard]] bool parse_quality_enums(const std::vector<std::string_view> &f,
                                       QualifiedCorpusEntry &e) noexcept {
  std::uint32_t disposition = 0u;
  std::uint32_t reason = 0u;
  std::uint32_t error = 0u;
  std::uint32_t profile = 0u;
  std::uint32_t source = 0u;
  std::uint32_t preset = 0u;
  std::uint32_t primary = 0u;
  std::uint32_t final = 0u;
  std::uint32_t dividend_treatment = 0u;
  return parse_u32(f[2], disposition) && to_disposition(disposition, e.disposition) &&
         parse_u32(f[3], reason) && to_admission_reason(reason, e.primary_reason) &&
         parse_u32(f[5], error) && to_error_code(error, e.source_or_fit_error) &&
         parse_u32(f[6], profile) && to_profile_kind(profile, e.quality.profile) &&
         parse_u32(f[7], source) && to_decision_source(source, e.quality.decision_source) &&
         parse_u32(f[8], preset) && to_fit_preset(preset, e.quality.preset) &&
         parse_u32(f[9], primary) && to_curve_kind(primary, e.quality.primary_kind) &&
         parse_u32(f[10], final) && to_curve_kind(final, e.quality.final_kind) &&
         parse_u32(f[18], dividend_treatment) &&
         to_dividend_treatment(dividend_treatment, e.quality.dividend_treatment);
}

[[nodiscard]] bool parse_quality_scalars(const std::vector<std::string_view> &f,
                                         QualifiedCorpusEntry &e) noexcept {
  return parse_u32(f[4], e.failed_checks) && valid_failure_mask(e.failed_checks) &&
         parse_bool(f[11], e.quality.used_fallback) && parse_bool(f[12], e.quality.curve_pinned) &&
         parse_bool(f[13], e.quality.final_kind_consistent) &&
         parse_bool(f[14], e.quality.provenance_complete) &&
         parse_u32(f[15], e.quality.source_schema_version) &&
         parse_u64(f[16], e.quality.source_fingerprint) &&
         parse_u64(f[17], e.quality.market_input_fingerprint) &&
         parse_u32(f[19], e.quality.n_cash_dividends) && parse_u32(f[20], e.quality.n_raw_quotes) &&
         parse_u32(f[21], e.quality.n_two_sided) && parse_u32(f[22], e.quality.n_slices) &&
         parse_u32(f[23], e.quality.n_holdout) && parse_u32(f[24], e.quality.n_fit_scorable) &&
         parse_u32(f[25], e.quality.n_fit_in_band) && parse_u32(f[26], e.quality.n_oos_in_band) &&
         parse_optional_double(f[27], e.quality.fit_in_band) &&
         parse_optional_double(f[28], e.quality.oos_in_band) &&
         parse_optional_double(f[29], e.quality.oos_vega_weighted) &&
         parse_optional_double(f[30], e.quality.oos_vega_weight_in_band) &&
         parse_optional_double(f[31], e.quality.oos_vega_weight_total) &&
         parse_optional_double(f[32], e.quality.mean_vol_rmse) &&
         parse_optional_double(f[33], e.quality.mean_reduced_chi2) &&
         parse_optional_u32(f[34], e.quality.calendar_violations);
}

[[nodiscard]] bool ratio_evidence_consistent(const std::optional<double> &ratio,
                                             std::uint32_t numerator,
                                             std::uint32_t denominator) noexcept {
  if (!ratio.has_value()) {
    return numerator == 0u && denominator == 0u;
  }
  if (denominator == 0u || numerator > denominator || !std::isfinite(*ratio)) {
    return false;
  }
  const double expected = static_cast<double>(numerator) / static_cast<double>(denominator);
  return std::fabs(*ratio - expected) <= 8.0 * std::numeric_limits<double>::epsilon();
}

[[nodiscard]] bool quality_evidence_consistent(const CorpusQualityMetrics &quality) noexcept {
  if (!ratio_evidence_consistent(quality.fit_in_band, quality.n_fit_in_band,
                                 quality.n_fit_scorable) ||
      !ratio_evidence_consistent(quality.oos_in_band, quality.n_oos_in_band, quality.n_holdout)) {
    return false;
  }
  const bool has_weight_in = quality.oos_vega_weight_in_band.has_value();
  const bool has_weight_total = quality.oos_vega_weight_total.has_value();
  if (has_weight_in != has_weight_total) {
    return false;
  }
  if (!has_weight_in) {
    return !quality.oos_vega_weighted.has_value();
  }
  const double in_band = *quality.oos_vega_weight_in_band;
  const double total = *quality.oos_vega_weight_total;
  if (!std::isfinite(in_band) || !std::isfinite(total) || in_band < 0.0 || total < 0.0 ||
      in_band > total) {
    return false;
  }
  if (total == 0.0) {
    return !quality.oos_vega_weighted.has_value();
  }
  return quality.oos_vega_weighted.has_value() && std::isfinite(*quality.oos_vega_weighted) &&
         std::fabs(*quality.oos_vega_weighted - in_band / total) <=
             8.0 * std::numeric_limits<double>::epsilon();
}

[[nodiscard]] Result<QualifiedCorpusEntry> parse_quality_entry(std::string_view line) {
  const std::vector<std::string_view> f = split_tabs(line);
  if (f.size() != kQualityFieldCount) {
    return Err(ErrorCode::ParseError, "parse_quality_report: entry row must have 36 fields");
  }

  QualifiedCorpusEntry e;
  e.date = std::string(f[0]);
  e.symbol = std::string(f[1]);
  if (e.date.empty() || e.symbol.empty() || !parse_quality_enums(f, e) ||
      !parse_quality_scalars(f, e) || !quality_evidence_consistent(e.quality)) {
    return Err(ErrorCode::ParseError, "parse_quality_report: invalid entry field");
  }
  e.archive_path = std::string(f[35]);

  const bool admitted = e.disposition == CorpusDisposition::Admitted;
  if (admitted && (e.primary_reason != CorpusAdmissionReason::None || e.failed_checks != 0u)) {
    return Err(ErrorCode::ParseError, "parse_quality_report: admitted row carries a failure");
  }
  if (!admitted && (e.primary_reason == CorpusAdmissionReason::None ||
                    (e.failed_checks & admission_reason_mask(e.primary_reason)) == 0u)) {
    return Err(ErrorCode::ParseError,
               "parse_quality_report: non-admitted row lacks primary failure");
  }
  return Ok(std::move(e));
}

void count_disposition(CorpusQualityReport &report, CorpusDisposition disposition) noexcept {
  switch (disposition) {
  case CorpusDisposition::Admitted:
    ++report.n_admitted;
    return;
  case CorpusDisposition::Quarantined:
    ++report.n_quarantined;
    return;
  case CorpusDisposition::SourceFailed:
    ++report.n_source_failed;
    return;
  case CorpusDisposition::FitFailed:
    ++report.n_fit_failed;
    return;
  case CorpusDisposition::Empty:
    ++report.n_empty;
    return;
  }
}

[[nodiscard]] bool quality_counts_match(const CorpusQualityReport &report) noexcept {
  const std::uint64_t sum = static_cast<std::uint64_t>(report.n_admitted) + report.n_quarantined +
                            report.n_source_failed + report.n_fit_failed + report.n_empty;
  return sum == report.n_planned &&
         report.entries.size() == static_cast<std::size_t>(report.n_planned);
}

} // namespace

std::string serialize_manifest(const CorpusManifest &m) {
  std::string out;
  out.append(kManifestMagic);
  out.push_back('\n');

  out.append("counts");
  for (const std::uint32_t v : {m.n_boards, m.n_ok, m.n_failed, m.n_skipped}) {
    out.push_back('\t');
    append_u32(out, v);
  }
  out.push_back('\n');

  out.append("dates");
  for (const std::string &d : m.dates) {
    out.push_back('\t');
    out.append(d);
  }
  out.push_back('\n');

  out.append("date\tsymbol\tstatus\tchosen_kind\tn_slices\toos_in_band\terror_code\tarchive_path");
  out.push_back('\n');

  for (const CorpusEntry &e : m.entries) {
    out.append(e.date);
    out.push_back('\t');
    out.append(e.symbol);
    out.push_back('\t');
    append_u32(out, static_cast<std::uint32_t>(e.status));
    out.push_back('\t');
    append_u32(out, static_cast<std::uint32_t>(e.chosen_kind));
    out.push_back('\t');
    append_u32(out, e.n_slices);
    out.push_back('\t');
    append_double(out, e.oos_in_band);
    out.push_back('\t');
    append_u32(out, static_cast<std::uint32_t>(e.error_code));
    out.push_back('\t');
    out.append(e.archive_path);
    out.push_back('\n');
  }
  return out;
}

Result<CorpusManifest> parse_manifest(std::string_view tsv) {
  const std::vector<std::string_view> lines = split_lines(tsv);
  if (lines.size() < 4) {
    return Err(ErrorCode::ParseError, "parse_manifest: truncated (need >= 4 lines)");
  }
  if (lines[0] != kManifestMagic) {
    return Err(ErrorCode::ParseError, "parse_manifest: bad magic");
  }

  CorpusManifest m{};

  // Line 1: counts.
  {
    const std::vector<std::string_view> f = split_tabs(lines[1]);
    if (f.size() != 5 || f[0] != "counts") {
      return Err(ErrorCode::ParseError, "parse_manifest: bad counts line");
    }
    if (!parse_u32(f[1], m.n_boards) || !parse_u32(f[2], m.n_ok) || !parse_u32(f[3], m.n_failed) ||
        !parse_u32(f[4], m.n_skipped)) {
      return Err(ErrorCode::ParseError, "parse_manifest: non-numeric count");
    }
  }

  // Line 2: dates.
  {
    const std::vector<std::string_view> f = split_tabs(lines[2]);
    if (f.empty() || f[0] != "dates") {
      return Err(ErrorCode::ParseError, "parse_manifest: bad dates line");
    }
    m.dates.reserve(f.size() - 1);
    for (std::size_t k = 1; k < f.size(); ++k) {
      m.dates.emplace_back(f[k]);
    }
  }

  // Line 3: column header (validated loosely). Lines 4+: entries.
  {
    const std::vector<std::string_view> h = split_tabs(lines[3]);
    if (h.empty() || h[0] != "date") {
      return Err(ErrorCode::ParseError, "parse_manifest: bad column header");
    }
  }

  m.entries.reserve(lines.size() - 4);
  for (std::size_t li = 4; li < lines.size(); ++li) {
    const std::vector<std::string_view> f = split_tabs(lines[li]);
    if (f.size() != 8) {
      return Err(ErrorCode::ParseError, "parse_manifest: entry row must have 8 fields");
    }
    CorpusEntry e{};
    e.date = std::string(f[0]);
    e.symbol = std::string(f[1]);

    std::uint32_t status_v = 0;
    std::uint32_t kind_v = 0;
    std::uint32_t err_v = 0;
    if (!parse_u32(f[2], status_v) || !to_fit_status(status_v, e.status)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad status");
    }
    if (!parse_u32(f[3], kind_v) || !to_curve_kind(kind_v, e.chosen_kind)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad chosen_kind");
    }
    if (!parse_u32(f[4], e.n_slices)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad n_slices");
    }
    if (!parse_double(f[5], e.oos_in_band)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad oos_in_band");
    }
    if (!parse_u32(f[6], err_v) || !to_error_code(err_v, e.error_code)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad error_code");
    }
    e.archive_path = std::string(f[7]);
    m.entries.push_back(std::move(e));
  }

  return Ok(std::move(m));
}

std::string serialize_quality_report(const CorpusQualityReport &report) {
  std::string out;
  out.append(kQualityReportMagic);
  out.push_back('\n');

  out.append("fingerprints\t");
  append_u64(out, report.input_fingerprint);
  out.push_back('\t');
  append_u64(out, report.policy_fingerprint);
  out.push_back('\n');

  out.append("counts");
  for (const std::uint32_t v : {report.n_planned, report.n_admitted, report.n_quarantined,
                                report.n_source_failed, report.n_fit_failed, report.n_empty}) {
    out.push_back('\t');
    append_u32(out, v);
  }
  out.push_back('\n');
  out.append(kQualityColumns);
  out.push_back('\n');

  for (const QualifiedCorpusEntry &entry : report.entries) {
    append_quality_entry(out, entry);
  }
  return out;
}

Result<CorpusQualityReport> parse_quality_report(std::string_view tsv) {
  const std::vector<std::string_view> lines = split_lines(tsv);
  if (lines.size() < 4u || lines[0] != kQualityReportMagic) {
    return Err(ErrorCode::ParseError, "parse_quality_report: truncated or bad magic");
  }

  CorpusQualityReport report;
  const std::vector<std::string_view> fingerprints = split_tabs(lines[1]);
  if (fingerprints.size() != 3u || fingerprints[0] != "fingerprints" ||
      !parse_u64(fingerprints[1], report.input_fingerprint) ||
      !parse_u64(fingerprints[2], report.policy_fingerprint)) {
    return Err(ErrorCode::ParseError, "parse_quality_report: bad fingerprints line");
  }

  const std::vector<std::string_view> counts = split_tabs(lines[2]);
  if (counts.size() != 7u || counts[0] != "counts" || !parse_u32(counts[1], report.n_planned) ||
      !parse_u32(counts[2], report.n_admitted) || !parse_u32(counts[3], report.n_quarantined) ||
      !parse_u32(counts[4], report.n_source_failed) || !parse_u32(counts[5], report.n_fit_failed) ||
      !parse_u32(counts[6], report.n_empty)) {
    return Err(ErrorCode::ParseError, "parse_quality_report: bad counts line");
  }
  if (lines[3] != kQualityColumns) {
    return Err(ErrorCode::ParseError, "parse_quality_report: bad column header");
  }

  report.entries.reserve(lines.size() - 4u);
  CorpusQualityReport observed;
  for (std::size_t i = 4u; i < lines.size(); ++i) {
    ATX_TRY(QualifiedCorpusEntry entry, parse_quality_entry(lines[i]));
    count_disposition(observed, entry.disposition);
    report.entries.push_back(std::move(entry));
  }
  if (report.entries.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return Err(ErrorCode::ParseError, "parse_quality_report: too many entry rows");
  }
  observed.n_planned = static_cast<std::uint32_t>(report.entries.size());
  if (!quality_counts_match(report) || report.n_admitted != observed.n_admitted ||
      report.n_quarantined != observed.n_quarantined ||
      report.n_source_failed != observed.n_source_failed ||
      report.n_fit_failed != observed.n_fit_failed || report.n_empty != observed.n_empty) {
    return Err(ErrorCode::ParseError, "parse_quality_report: aggregate counts do not match rows");
  }
  return Ok(std::move(report));
}

Status write_quality_report_file(std::string_view path, const CorpusQualityReport &report) {
  const std::filesystem::path dst{std::string(path)};
  if (dst.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "write_quality_report_file: cannot create parent dir");
    }
  }

  const std::string text = serialize_quality_report(report);
  std::filesystem::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      return Err(ErrorCode::IoError, "write_quality_report_file: cannot open temp file");
    }
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_quality_report_file: write failed");
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_quality_report_file: rename failed");
  }
  return Ok();
}

Result<CorpusQualityReport> read_quality_report_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "read_quality_report_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "read_quality_report_file: cannot open file");
  }
  const std::streamoff size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "read_quality_report_file: cannot size file");
  }
  is.seekg(0);
  std::string text(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    is.read(text.data(), size);
    if (!is) {
      return Err(ErrorCode::IoError, "read_quality_report_file: read failed");
    }
  }
  return parse_quality_report(text);
}

Status write_manifest_file(std::string_view path, const CorpusManifest &m) {
  const std::filesystem::path dst{std::string(path)};
  if (dst.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "write_manifest_file: cannot create parent dir");
    }
  }

  const std::string text = serialize_manifest(m);
  std::filesystem::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      return Err(ErrorCode::IoError, "write_manifest_file: cannot open temp file");
    }
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_manifest_file: write failed");
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_manifest_file: rename failed");
  }
  return Ok();
}

Result<CorpusManifest> read_manifest_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "read_manifest_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "read_manifest_file: cannot open file");
  }
  const std::streamoff size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "read_manifest_file: cannot size file");
  }
  is.seekg(0);
  std::string text(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    is.read(text.data(), size);
    if (!is) {
      return Err(ErrorCode::IoError, "read_manifest_file: read failed");
    }
  }
  return parse_manifest(text);
}

} // namespace atx::vol
