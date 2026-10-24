// fit_risk_probe.cpp — RED stub (TDD): every entry point refuses NotImplemented.

#include "fit_risk_probe.hpp"

#include <limits>

namespace atx::vol::fit_probe {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

Result<std::vector<QuoteRow>> parse_quotes_csv(std::string_view) {
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}
Result<std::vector<PredictionRow>> parse_predictions_csv(std::string_view) {
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}
Result<std::vector<LabelRow>> parse_labels_csv(std::string_view) {
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}
Result<std::vector<ChainSpot>> parse_basis_csv(std::string_view) {
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}
Status labels_allowed(std::string_view) { return atx::core::Err(ErrorCode::NotImplemented); }
double shadow_rate(const QuoteSliceMarket &) noexcept { return kNaN; }
double shadow_yield(const QuoteSliceMarket &, double, QuoteSliceDividendModel) noexcept {
  return kNaN;
}
std::int64_t shadow_expiry_ns(std::int64_t, double, std::int64_t) noexcept { return 0; }
std::uint8_t greek_sanity(Side, double, double, double) noexcept { return 0; }
bool cp_delta_violation(double, double, double, double) noexcept { return false; }
const char *to_string(ProbeKernel) noexcept { return ""; }
const char *to_string(RowState) noexcept { return ""; }
const char *to_string(SanityBit) noexcept { return ""; }
void CompensatedSum::add(double) noexcept {}
void accumulate(ProbeCounts &, const ProbeRow &) noexcept {}
Result<ProbeReport> run_fit_risk_probe(std::span<const QuoteRow>, std::span<const PredictionRow>,
                                       std::span<const LabelRow>, std::span<const ChainSpot>,
                                       const ProbeOptions &, std::vector<ProbeRow> *) {
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}
double vendor_delta_mae(const ProbeCounts &) noexcept { return 0.0; }
double vendor_vega_sym_rel(const ProbeCounts &) noexcept { return 0.0; }
std::string probe_report_to_json(const ProbeReport &) { return {}; }
Result<ProbeReport> probe_report_from_json(std::string_view) {
  return atx::core::Err(ErrorCode::NotImplemented, "stub");
}
std::string probe_rows_to_csv(std::span<const ProbeRow>) { return {}; }

} // namespace atx::vol::fit_probe
