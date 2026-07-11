// Correctness-first volatility-surface pipeline benchmark.
//
// This is intentionally a small Release-oriented executable rather than a
// Google Benchmark registration. Admission can reject individual builds, and a
// rejected build must never enter a latency distribution. The executable owns
// that filtering explicitly, reports the rejected population beside the timing
// population, and exits non-zero if any requested sample was not admitted.
//
// Output is one JSON document suitable for release-gate ingestion. All latency
// percentiles are computed only from freshly admitted candidate generations.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol::bench {
namespace {

using Clock = std::chrono::steady_clock;

struct CliOptions {
  std::size_t samples{10};
  std::size_t warmup{1};
};

struct Distribution {
  std::vector<double> values_ms;

  void add(double value_ms) {
    if (std::isfinite(value_ms) && value_ms >= 0.0) {
      values_ms.push_back(value_ms);
    }
  }
};

struct ModeReport {
  FitQualityMode mode{FitQualityMode::Balanced};
  std::size_t cold_admitted{};
  std::size_t cold_rejected{};
  std::size_t incremental_admitted{};
  std::size_t incremental_rejected{};
  std::uint32_t cold_rejection_reasons{};
  std::uint32_t incremental_rejection_reasons{};
  Distribution cold_wall;
  Distribution pipeline_total;
  Distribution mark_build;
  Distribution risk_build;
  Distribution validation;
  Distribution one_expiry_incremental;
};

[[nodiscard]] std::optional<std::size_t> parse_count(std::string_view text) {
  std::size_t value{};
  const char *const begin = text.data();
  const char *const end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0u || value > 1000u) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<CliOptions> parse_cli(int argc, char **argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if ((arg == "--samples" || arg == "--warmup") && i + 1 < argc) {
      const std::optional<std::size_t> value = parse_count(argv[++i]);
      if (!value.has_value()) {
        return std::nullopt;
      }
      if (arg == "--samples") {
        options.samples = *value;
      } else {
        options.warmup = *value;
      }
    } else {
      return std::nullopt;
    }
  }
  return options;
}

// A generic liquid-board fixture, deliberately not a ticker-specific policy
// special case. It reuses the library's deterministic American panel generator
// and an arbitrage-free flat-vol S3 truth. Benchmark configuration pins only the
// candidate risk family so comparisons isolate quality-mode work budgets.
[[nodiscard]] SynthPanelSpec make_liquid_fixture() {
  SynthPanelSpec spec;
  spec.uid = "SYNTH_LIQUID_ETF";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.04;
  spec.borrow = 0.0;
  spec.half_spread_frac = 0.01;
  spec.min_half_spread = 0.02;

  constexpr std::string_view expiries[]{"2026-07-17", "2026-08-21", "2026-09-18", "2026-12-18",
                                        "2027-03-19"};
  for (const std::string_view expiry : expiries) {
    SynthExpiry slice;
    slice.expiry_iso = std::string(expiry);
    slice.T = year_fraction(spec.snapshot_iso, expiry);
    slice.truth = S3Params{0.30, 0.0, 0.0};
    spec.expiries.push_back(std::move(slice));
  }

  constexpr std::size_t kStrikeCount = 65;
  spec.strikes.reserve(kStrikeCount);
  for (std::size_t i = 0; i < kStrikeCount; ++i) {
    const double fraction = static_cast<double>(i) / static_cast<double>(kStrikeCount - 1u);
    const double log_moneyness = -0.80 + 1.60 * fraction;
    spec.strikes.push_back(spec.spot * std::exp(log_moneyness));
  }
  return spec;
}

[[nodiscard]] Result<OptionChain> make_chain(const SynthPanelSpec &spec) {
  Result<SynthPanel> panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return atx::core::Err(std::move(panel).error());
  }
  return OptionChain::from_frame(panel->frame, spec.r, spec.spot);
}

[[nodiscard]] PricerConfig config_for(FitQualityMode mode) {
  PricerConfig config;
  config.quality_mode = mode;
  config.outputs = SurfaceOutputs::MarketMarkAndRisk;
  config.risk_admission = RiskAdmission::Required;
  // A benchmark must expose rejection, never hide it behind a prior generation.
  config.fallback = SurfaceFallback::None;
  CurveConfig risk_curve;
  risk_curve.kind = VolCurveKind::Essvi;
  config.curve = risk_curve;
  return config;
}

[[nodiscard]] bool freshly_admitted(const SurfaceBundle &bundle) noexcept {
  return bundle.market_mark != nullptr && bundle.risk != nullptr &&
         bundle.market_mark_health.state == SurfaceState::Healthy &&
         bundle.risk_health.state == SurfaceState::Healthy &&
         bundle.risk_health.validation.admitted() &&
         bundle.risk_health.candidate_generation == bundle.candidate_generation &&
         bundle.risk_health.served_generation == bundle.candidate_generation &&
         !bundle.risk_health.using_fallback();
}

[[nodiscard]] std::uint32_t reason_bits(const SurfaceBundle &bundle) noexcept {
  return static_cast<std::uint32_t>(bundle.risk_health.reasons) |
         static_cast<std::uint32_t>(bundle.risk_health.validation.failures);
}

void record_admitted_cold(ModeReport &report, const SurfaceBundle &bundle, double wall_ms) {
  ++report.cold_admitted;
  report.cold_wall.add(wall_ms);
  report.pipeline_total.add(bundle.timings.total_ms);
  report.mark_build.add(bundle.timings.market_mark_build_ms);
  report.risk_build.add(bundle.timings.risk_build_ms);
  report.validation.add(bundle.timings.risk_validation_ms);
}

void run_cold_samples(const OptionChain &chain, FitQualityMode mode, const CliOptions &options,
                      ModeReport &report) {
  for (std::size_t i = 0; i < options.warmup; ++i) {
    PricerFitter warmup(config_for(mode));
    const Status ignored = warmup.fit(chain);
    (void)ignored;
  }

  for (std::size_t i = 0; i < options.samples; ++i) {
    PricerFitter fitter(config_for(mode));
    const auto start = Clock::now();
    const Status status = fitter.fit(chain);
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const SurfaceBundle bundle = fitter.bundle();
    if (status.has_value() && freshly_admitted(bundle)) {
      record_admitted_cold(report, bundle, wall_ms);
    } else {
      ++report.cold_rejected;
      report.cold_rejection_reasons |= reason_bits(bundle);
    }
  }
}

struct ExpiryUpdate {
  std::vector<OptionId> ids;
  std::vector<double> mids;
  std::vector<double> half_spreads;
};

[[nodiscard]] std::optional<ExpiryUpdate> front_expiry_update(const OptionChain &chain) {
  const std::vector<OptionId> all_ids = chain.ids();
  if (all_ids.empty()) {
    return std::nullopt;
  }
  std::int64_t front_expiry = std::numeric_limits<std::int64_t>::max();
  for (const OptionId id : all_ids) {
    const Result<OptionRef> option = chain.at(id);
    if (option.has_value()) {
      front_expiry = std::min(front_expiry, option->expiry_ns);
    }
  }
  if (front_expiry == std::numeric_limits<std::int64_t>::max()) {
    return std::nullopt;
  }

  ExpiryUpdate update;
  for (const OptionId id : all_ids) {
    const Result<OptionRef> option = chain.at(id);
    if (!option.has_value() || option->expiry_ns != front_expiry) {
      continue;
    }
    update.ids.push_back(id);
    update.mids.push_back(option->mid);
    update.half_spreads.push_back(0.5 * (option->ask - option->bid));
  }
  return update.ids.empty() ? std::nullopt : std::optional<ExpiryUpdate>(std::move(update));
}

void run_incremental_samples(OptionChain &chain, FitQualityMode mode, const CliOptions &options,
                             ModeReport &report) {
  PricerFitter fitter(config_for(mode));
  if (!fitter.fit(chain).has_value() || !freshly_admitted(fitter.bundle())) {
    report.incremental_rejected += options.samples;
    report.incremental_rejection_reasons |= reason_bits(fitter.bundle());
    return;
  }
  const std::optional<ExpiryUpdate> update = front_expiry_update(chain);
  if (!update.has_value()) {
    report.incremental_rejected += options.samples;
    report.incremental_rejection_reasons |=
        static_cast<std::uint32_t>(ValidationFailure::InsufficientData);
    return;
  }

  std::vector<double> bids(update->ids.size());
  std::vector<double> asks(update->ids.size());
  for (std::size_t sample = 0; sample < options.samples; ++sample) {
    // Change one expiry's quote uncertainty while preserving every midpoint and
    // therefore put/call parity. This is a real quote update without injecting a
    // synthetic calendar or carry violation.
    const double spread_scale = (sample & 1u) == 0u ? 1.05 : 0.95;
    for (std::size_t i = 0; i < update->ids.size(); ++i) {
      const double half_spread = update->half_spreads[i] * spread_scale;
      bids[i] = std::max(0.0, update->mids[i] - half_spread);
      asks[i] = update->mids[i] + half_spread;
    }

    const auto start = Clock::now();
    const Status updated =
        chain.update_quotes(std::span<const OptionId>(update->ids), std::span<const double>(bids),
                            std::span<const double>(asks));
    const Status fitted = updated.has_value() ? fitter.fit(chain) : updated;
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const SurfaceBundle bundle = fitter.bundle();
    if (fitted.has_value() && freshly_admitted(bundle)) {
      ++report.incremental_admitted;
      report.one_expiry_incremental.add(wall_ms);
    } else {
      ++report.incremental_rejected;
      report.incremental_rejection_reasons |= reason_bits(bundle);
    }
  }
}

[[nodiscard]] double percentile(const Distribution &distribution, double p) {
  if (distribution.values_ms.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::vector<double> sorted = distribution.values_ms;
  std::sort(sorted.begin(), sorted.end());
  const double rank = p * static_cast<double>(sorted.size() - 1u);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = static_cast<std::size_t>(std::ceil(rank));
  const double fraction = rank - static_cast<double>(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * fraction;
}

void print_number_or_null(double value) {
  if (std::isfinite(value)) {
    std::cout << value;
  } else {
    std::cout << "null";
  }
}

void print_distribution(std::string_view name, const Distribution &distribution,
                        bool trailing_comma) {
  std::cout << "      \"" << name << "\": {\"samples\": " << distribution.values_ms.size()
            << ", \"p50_ms\": ";
  print_number_or_null(percentile(distribution, 0.50));
  std::cout << ", \"p95_ms\": ";
  print_number_or_null(percentile(distribution, 0.95));
  std::cout << "}" << (trailing_comma ? "," : "") << "\n";
}

void print_report(const ModeReport &report, bool trailing_comma) {
  std::cout << "    {\n"
            << "      \"mode\": \"" << to_string(report.mode) << "\",\n"
            << "      \"cold_admitted\": " << report.cold_admitted << ",\n"
            << "      \"cold_rejected\": " << report.cold_rejected << ",\n"
            << "      \"cold_rejection_reason_bits\": " << report.cold_rejection_reasons << ",\n"
            << "      \"incremental_admitted\": " << report.incremental_admitted << ",\n"
            << "      \"incremental_rejected\": " << report.incremental_rejected << ",\n"
            << "      \"incremental_rejection_reason_bits\": "
            << report.incremental_rejection_reasons << ",\n"
            << "      \"timings\": {\n";
  print_distribution("cold_wall", report.cold_wall, true);
  print_distribution("pipeline_total", report.pipeline_total, true);
  print_distribution("market_mark_build", report.mark_build, true);
  print_distribution("risk_build", report.risk_build, true);
  print_distribution("risk_validation", report.validation, true);
  print_distribution("one_expiry_update_to_admission", report.one_expiry_incremental, false);
  std::cout << "      }\n"
            << "    }" << (trailing_comma ? "," : "") << "\n";
}

} // namespace
} // namespace atx::vol::bench

int main(int argc, char **argv) {
  using namespace atx::vol;
  using namespace atx::vol::bench;

  const std::optional<CliOptions> options = parse_cli(argc, argv);
  if (!options.has_value()) {
    std::cerr << "usage: atx-vol-surface-v2-bench [--samples 1..1000] "
                 "[--warmup 1..1000]\n";
    return 64;
  }

  const SynthPanelSpec fixture = make_liquid_fixture();
  Result<OptionChain> cold_chain = make_chain(fixture);
  if (!cold_chain.has_value()) {
    std::cerr << "surface_v2_bench: fixture construction failed: " << cold_chain.error().message()
              << "\n";
    return 2;
  }

  constexpr FitQualityMode modes[]{FitQualityMode::Latency, FitQualityMode::Balanced,
                                   FitQualityMode::Accuracy};
  std::vector<ModeReport> reports;
  reports.reserve(std::size(modes));
  bool all_admitted = true;
  for (const FitQualityMode mode : modes) {
    ModeReport report;
    report.mode = mode;
    run_cold_samples(*cold_chain, mode, *options, report);

    Result<OptionChain> incremental_chain = make_chain(fixture);
    if (incremental_chain.has_value()) {
      run_incremental_samples(*incremental_chain, mode, *options, report);
    } else {
      report.incremental_rejected = options->samples;
      report.incremental_rejection_reasons =
          static_cast<std::uint32_t>(ValidationFailure::InsufficientData);
    }
    all_admitted = all_admitted && report.cold_rejected == 0u &&
                   report.incremental_rejected == 0u && report.cold_admitted == options->samples &&
                   report.incremental_admitted == options->samples;
    reports.push_back(std::move(report));
  }

  std::cout << std::fixed << std::setprecision(6) << "{\n"
            << "  \"benchmark\": \"surface_v2\",\n"
#ifdef NDEBUG
            << "  \"build\": \"release\",\n"
#else
            << "  \"build\": \"non_release\",\n"
#endif
            << "  \"fixture\": \"generic_synthetic_liquid_board\",\n"
            << "  \"timing_sample_policy\": "
               "\"freshly_admitted_candidates_only\",\n"
            << "  \"incremental_api\": "
               "\"one_expiry_quote_update_then_current_fit_api\",\n"
            << "  \"requested_samples_per_mode\": " << options->samples << ",\n"
            << "  \"modes\": [\n";
  for (std::size_t i = 0; i < reports.size(); ++i) {
    print_report(reports[i], i + 1u < reports.size());
  }
  std::cout << "  ],\n"
            << "  \"all_samples_admitted\": " << (all_admitted ? "true" : "false") << "\n"
            << "}\n";

  // A release latency gate is meaningless if any build was rejected or served
  // from fallback. The JSON above remains complete for diagnosis.
  return all_admitted ? 0 : 3;
}
