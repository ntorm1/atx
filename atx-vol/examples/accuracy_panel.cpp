// Deterministic real-OPRA regression oracle for fit quality and query-tier economics.

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/opra_batch.hpp"
#include "atx/vol/opra_panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/query_pricing.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"

namespace {

using atx::vol::ChainSnapshot;
using atx::vol::CorpusBoard;
using atx::vol::ErrorCode;
using atx::vol::FitDecision;
using atx::vol::FitPreset;
using atx::vol::OpraLoadSpec;
using atx::vol::OptionChain;
using atx::vol::OutputField;
using atx::vol::PricedSurface;
using atx::vol::PricerConfig;
using atx::vol::PricerFitter;
using atx::vol::QueryExecution;
using atx::vol::QueryPricingRoute;
using atx::vol::QueryPricingTier;
using atx::vol::SessionDiagnostics;
using atx::vol::Side;
using atx::vol::SurfacePurpose;
using atx::vol::ValidationFailure;

constexpr std::string_view kUniverseDate{"2026-07-01"};
constexpr std::string_view kUniverseSnapshot{"2026-07-01T14:00:00Z"};
constexpr double kDefaultRate{0.043};
constexpr double kDefaultOptionTick{0.01};
constexpr double kEconomicVegaFraction{0.1 * 1.0e-4};
constexpr std::size_t kCommitSampleLimit{64u};

enum class PanelMode : std::uint8_t {
  Commit = 0,
  Exhaustive = 1,
};

struct SpyFixture {
  std::string_view filename;
  std::string_view snapshot;
};

constexpr std::array<SpyFixture, 10> kSpyFixtures{{
    {"SPY_2026-02-12T1435Z.parquet", "2026-02-12T14:35:00Z"},
    {"SPY_2026-02-12T1700Z.parquet", "2026-02-12T17:00:00Z"},
    {"SPY_2026-02-12T1955Z.parquet", "2026-02-12T19:55:00Z"},
    {"SPY_2026-03-09T1335Z.parquet", "2026-03-09T13:35:00Z"},
    {"SPY_2026-03-09T1600Z.parquet", "2026-03-09T16:00:00Z"},
    {"SPY_2026-03-09T1955Z.parquet", "2026-03-09T19:55:00Z"},
    {"SPY_2026-05-27T1335Z.parquet", "2026-05-27T13:35:00Z"},
    {"SPY_2026-05-27T1600Z.parquet", "2026-05-27T16:00:00Z"},
    {"SPY_2026-05-27T1955Z.parquet", "2026-05-27T19:55:00Z"},
    {"SPY_2026-06-05T1955Z.parquet", "2026-06-05T19:55:00Z"},
}};

struct CliArgs {
  std::filesystem::path spy_root{"data/spy_fit_slices"};
  std::filesystem::path universe_root{"data/opra_universe"};
  std::filesystem::path symbols_path{"data/universe/smoke100.txt"};
  std::filesystem::path output_path{"accuracy_panel.csv"};
  FitPreset preset{FitPreset::Robust};
  double rate{kDefaultRate};
  double option_tick{kDefaultOptionTick};
  std::size_t spy_limit{0u};
  std::size_t universe_limit{0u};
  PanelMode mode{PanelMode::Commit};
  bool include_spy{true};
  bool full_chain_valuation{false};
};

struct InputSpec {
  std::string symbol;
  std::string input;
  std::string date;
  std::string snapshot;
  std::filesystem::path path;
};

struct FastDiagnostics {
  std::size_t candidates{};
  std::size_t representative_routes{};
  std::size_t cold_fallbacks{};
  std::size_t scored{};
  std::size_t failures{};
  std::size_t greek_scored{};
  std::size_t greek_failures{};
  std::size_t price_sign_flips{};
  std::size_t delta_sign_flips{};
  std::size_t theta_sign_flips{};
  std::size_t vega_sign_flips{};
  std::size_t fast_negative_prices{};
  std::size_t half_spread_passes{};
  std::size_t half_tick_passes{};
  std::size_t economic_gate_passes{};
  std::optional<double> price_error_p50{};
  std::optional<double> price_error_p95{};
  std::optional<double> price_error_max{};
  std::optional<double> delta_error_p50{};
  std::optional<double> delta_error_p95{};
  std::optional<double> delta_error_max{};
  std::optional<double> gamma_error_p50{};
  std::optional<double> gamma_error_p95{};
  std::optional<double> gamma_error_max{};
  std::optional<double> theta_error_p50{};
  std::optional<double> theta_error_p95{};
  std::optional<double> theta_error_max{};
  std::optional<double> vega_error_p50{};
  std::optional<double> vega_error_p95{};
  std::optional<double> vega_error_max{};
  std::optional<double> half_spread_pass_fraction{};
  std::optional<double> half_tick_pass_fraction{};
  std::optional<double> economic_gate_fraction{};
  std::optional<double> route_fraction{};
  std::optional<double> screen_safe_fraction{};
  std::optional<double> sample_coverage_fraction{};
};

struct Row {
  std::string symbol;
  std::string input;
  std::string status{"pending"};
  std::string error;
  std::string effective_preset;
  std::string curve_kind;
  std::string primary_curve_kind;
  std::string resolved_config;
  std::optional<bool> used_fallback{};
  std::optional<std::size_t> scored_quotes{};
  std::optional<double> market_in_band_fraction{};
  std::optional<double> market_worst_in_band_fraction{};
  std::optional<double> market_reduced_chi_square{};
  std::optional<double> market_vol_rmse{};
  std::optional<std::size_t> market_bid_misses{};
  std::optional<std::size_t> market_ask_misses{};
  std::optional<double> market_max_price_error{};
  std::optional<std::size_t> calendar_violations{};
  std::optional<bool> calendar_arb_free{};
  std::optional<std::uint32_t> validation_mask{};
  std::optional<std::uint32_t> admission_mask{};
  std::optional<std::uint32_t> arbitrage_mask{};
  std::optional<std::size_t> valued_options{};
  FastDiagnostics fast{};
};

[[nodiscard]] std::string_view preset_name(FitPreset preset) noexcept {
  switch (preset) {
  case FitPreset::Fast:
    return "fast";
  case FitPreset::Accurate:
    return "accurate";
  case FitPreset::Robust:
    return "robust";
  case FitPreset::Hft:
    return "hft";
  case FitPreset::Populate:
    return "populate";
  case FitPreset::Bulk:
    return "bulk";
  }
  return "unknown";
}

[[nodiscard]] std::string_view mode_name(PanelMode mode) noexcept {
  switch (mode) {
  case PanelMode::Commit:
    return "commit";
  case PanelMode::Exhaustive:
    return "exhaustive";
  }
  return "unknown";
}

[[nodiscard]] std::optional<PanelMode> parse_mode(std::string_view value) noexcept {
  if (value == "commit")
    return PanelMode::Commit;
  if (value == "exhaustive")
    return PanelMode::Exhaustive;
  return std::nullopt;
}

[[nodiscard]] std::optional<FitPreset> parse_preset(std::string_view value) noexcept {
  if (value == "fast")
    return FitPreset::Fast;
  if (value == "accurate")
    return FitPreset::Accurate;
  if (value == "robust")
    return FitPreset::Robust;
  if (value == "hft")
    return FitPreset::Hft;
  return std::nullopt;
}

[[nodiscard]] bool parse_size(std::string_view text, std::size_t &value) noexcept {
  if (text.empty())
    return false;
  std::size_t parsed{};
  const char *const begin = text.data();
  const char *const end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end)
    return false;
  value = parsed;
  return true;
}

[[nodiscard]] bool parse_finite_double(std::string_view text, double &value) noexcept {
  if (text.empty())
    return false;
  const std::string copy{text};
  char *end{};
  errno = 0;
  const double parsed = std::strtod(copy.c_str(), &end);
  if (errno != 0 || end != copy.c_str() + copy.size() || !std::isfinite(parsed)) {
    return false;
  }
  value = parsed;
  return true;
}

[[nodiscard]] bool next_value(int argc, char **argv, int &index, std::string_view &value,
                              std::string &error) {
  if (index + 1 >= argc) {
    error = "missing value for " + std::string{argv[index]};
    return false;
  }
  ++index;
  value = argv[index];
  return true;
}

void print_usage() {
  std::fputs("usage: accuracy_panel [--spy-root DIR] [--universe-root DIR] "
             "[--symbols FILE] [--out FILE] [--preset robust|fast|accurate|hft] "
             "[--rate R] [--option-tick TICK] [--spy-limit N] [--universe-limit N] "
             "[--mode commit|exhaustive] [--full-chain-valuation] [--no-spy]\n",
             stderr);
}

[[nodiscard]] std::optional<CliArgs> parse_args(int argc, char **argv, std::string &error) {
  CliArgs args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option{argv[i]};
    if (option == "--help") {
      print_usage();
      return std::nullopt;
    }
    if (option == "--no-spy") {
      args.include_spy = false;
      continue;
    }
    if (option == "--full-chain-valuation") {
      args.full_chain_valuation = true;
      continue;
    }
    std::string_view value;
    if (!next_value(argc, argv, i, value, error))
      return std::nullopt;
    if (option == "--spy-root")
      args.spy_root = std::string{value};
    else if (option == "--universe-root")
      args.universe_root = std::string{value};
    else if (option == "--symbols")
      args.symbols_path = std::string{value};
    else if (option == "--out")
      args.output_path = std::string{value};
    else if (option == "--preset") {
      const auto parsed = parse_preset(value);
      if (!parsed.has_value()) {
        error = "invalid --preset: " + std::string{value};
        return std::nullopt;
      }
      args.preset = *parsed;
    } else if (option == "--rate") {
      if (!parse_finite_double(value, args.rate)) {
        error = "invalid --rate: " + std::string{value};
        return std::nullopt;
      }
    } else if (option == "--option-tick") {
      if (!parse_finite_double(value, args.option_tick) || !(args.option_tick > 0.0)) {
        error = "invalid --option-tick: " + std::string{value};
        return std::nullopt;
      }
    } else if (option == "--spy-limit") {
      if (!parse_size(value, args.spy_limit)) {
        error = "invalid --spy-limit: " + std::string{value};
        return std::nullopt;
      }
    } else if (option == "--universe-limit") {
      if (!parse_size(value, args.universe_limit)) {
        error = "invalid --universe-limit: " + std::string{value};
        return std::nullopt;
      }
    } else if (option == "--mode") {
      const auto parsed = parse_mode(value);
      if (!parsed.has_value()) {
        error = "invalid --mode: " + std::string{value};
        return std::nullopt;
      }
      args.mode = *parsed;
    } else {
      error = "unknown argument: " + std::string{option};
      return std::nullopt;
    }
  }
  return args;
}

void trim_ascii(std::string &value) {
  const auto is_space = [](char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!value.empty() && is_space(value.back()))
    value.pop_back();
  const auto first = std::find_if_not(value.begin(), value.end(), is_space);
  value.erase(value.begin(), first);
}

[[nodiscard]] std::optional<std::vector<std::string>>
read_symbols(const std::filesystem::path &path, std::string &error) {
  std::ifstream input{path};
  if (!input) {
    error = "cannot open symbols file: " + path.generic_string();
    return std::nullopt;
  }
  input.imbue(std::locale::classic());
  std::vector<std::string> symbols;
  std::string line;
  while (std::getline(input, line)) {
    trim_ascii(line);
    if (!line.empty())
      symbols.push_back(std::move(line));
  }
  if (input.bad()) {
    error = "failed reading symbols file: " + path.generic_string();
    return std::nullopt;
  }
  if (symbols.empty()) {
    error = "symbols file is empty: " + path.generic_string();
    return std::nullopt;
  }
  return symbols;
}

[[nodiscard]] std::vector<InputSpec> build_inputs(const CliArgs &args,
                                                  std::vector<std::string> symbols) {
  const std::size_t spy_count =
      !args.include_spy ? 0u
                        : (args.spy_limit == 0u ? kSpyFixtures.size()
                                                : std::min(args.spy_limit, kSpyFixtures.size()));
  if (args.universe_limit > 0u && symbols.size() > args.universe_limit) {
    symbols.resize(args.universe_limit);
  }
  std::vector<InputSpec> inputs;
  inputs.reserve(spy_count + symbols.size());
  for (std::size_t i = 0u; i < spy_count; ++i) {
    const SpyFixture &fixture = kSpyFixtures[i];
    inputs.push_back(InputSpec{"SPY", "spy/" + std::string{fixture.filename},
                               std::string{fixture.snapshot.substr(0u, 10u)},
                               std::string{fixture.snapshot},
                               args.spy_root / std::string{fixture.filename}});
  }
  for (std::string &symbol : symbols) {
    const std::string input = "universe/" + symbol + "/" + std::string{kUniverseDate} + ".parquet";
    inputs.push_back(InputSpec{
        std::move(symbol), input, std::string{kUniverseDate}, std::string{kUniverseSnapshot}, {}});
    InputSpec &added = inputs.back();
    added.path = args.universe_root / added.symbol / (std::string{kUniverseDate} + ".parquet");
  }
  return inputs;
}

void record_decision(Row &row, const std::optional<FitDecision> &decision) {
  if (!decision.has_value())
    return;
  row.effective_preset = std::string{preset_name(decision->preset)};
  row.curve_kind = atx::vol::to_string(decision->curve.kind);
  row.primary_curve_kind = atx::vol::to_string(decision->primary_curve.kind);
  row.used_fallback = decision->used_fallback;
}

[[nodiscard]] std::string resolved_config(const PricerFitter &fitter) {
  const auto &inputs = fitter.surface()->session().inputs();
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "curve=" << atx::vol::to_string(inputs.curve.kind)
      << ";prep=" << static_cast<unsigned>(inputs.fit_prep_policy)
      << ";score=" << (inputs.score_parity ? 1 : 0)
      << ";calendar_floor=" << (inputs.enforce_calendar_floor ? 1 : 0)
      << ";query_tier=" << static_cast<unsigned>(inputs.query_pricing_tier)
      << ";deam_cache=" << (inputs.use_deam_cache_for_fit ? 1 : 0);
  return out.str();
}

void record_market_diagnostics(Row &row, const PricerFitter &fitter) {
  const SessionDiagnostics &diag = fitter.surface()->diagnostics();
  row.scored_quotes = diag.n_quotes;
  row.market_in_band_fraction = diag.mean_frac_within_bidask;
  row.market_worst_in_band_fraction = diag.worst_frac_within_bidask;
  row.market_reduced_chi_square = diag.mean_chi2_reduced;
  row.market_vol_rmse = diag.mean_rmse_vol;
  row.market_bid_misses = diag.n_bid_miss;
  row.market_ask_misses = diag.n_ask_miss;
  row.market_max_price_error = diag.max_prc_err;
  row.calendar_violations = diag.n_calendar_viol_pre;
  row.calendar_arb_free = diag.calendar_arb_free;
  row.resolved_config = resolved_config(fitter);

  const auto bundle = fitter.bundle();
  const auto purpose = fitter.surface()->purpose();
  const auto &health =
      purpose == SurfacePurpose::MarketMark ? bundle.market_mark_health : bundle.risk_health;
  const std::uint32_t validation = static_cast<std::uint32_t>(health.reasons);
  constexpr std::uint32_t arb_bits =
      static_cast<std::uint32_t>(ValidationFailure::StrikeMonotonicity) |
      static_cast<std::uint32_t>(ValidationFailure::Butterfly) |
      static_cast<std::uint32_t>(ValidationFailure::Calendar) |
      static_cast<std::uint32_t>(ValidationFailure::Wing);
  row.validation_mask = validation;
  std::uint32_t arbitration = validation & arb_bits;
  if (!diag.calendar_arb_free) {
    arbitration |= static_cast<std::uint32_t>(ValidationFailure::Calendar);
  }
  row.arbitrage_mask = arbitration;

  const auto &report = fitter.published_report();
  if (report.has_value()) {
    for (auto attempt = report->attempts.rbegin(); attempt != report->attempts.rend(); ++attempt) {
      if (attempt->build_succeeded && attempt->admission.admitted) {
        row.admission_mask = attempt->admission.failed_checks;
        break;
      }
    }
  }
}

[[nodiscard]] double nearest_rank(std::span<const double> sorted, double probability) noexcept {
  if (sorted.empty())
    return std::numeric_limits<double>::quiet_NaN();
  const double rank = std::ceil(probability * static_cast<double>(sorted.size()));
  const std::size_t one_based = static_cast<std::size_t>(std::max(1.0, rank));
  return sorted[std::min(one_based - 1u, sorted.size() - 1u)];
}

[[nodiscard]] bool economically_relevant_sign_flip(double lhs, double rhs) noexcept {
  return (lhs < 0.0) != (rhs < 0.0);
}

struct FastErrors {
  std::vector<double> price;
  std::vector<double> delta;
  std::vector<double> gamma;
  std::vector<double> theta;
  std::vector<double> vega;
};

void summarize_errors(std::vector<double> errors, std::optional<double> &p50,
                      std::optional<double> &p95, std::optional<double> &maximum) {
  if (errors.empty())
    return;
  std::sort(errors.begin(), errors.end());
  p50 = nearest_rank(errors, 0.50);
  p95 = nearest_rank(errors, 0.95);
  maximum = errors.back();
}

void finish_fast_diagnostics(FastDiagnostics &diag, FastErrors errors) {
  summarize_errors(errors.price, diag.price_error_p50, diag.price_error_p95, diag.price_error_max);
  summarize_errors(errors.delta, diag.delta_error_p50, diag.delta_error_p95, diag.delta_error_max);
  summarize_errors(errors.gamma, diag.gamma_error_p50, diag.gamma_error_p95, diag.gamma_error_max);
  summarize_errors(errors.theta, diag.theta_error_p50, diag.theta_error_p95, diag.theta_error_max);
  summarize_errors(errors.vega, diag.vega_error_p50, diag.vega_error_p95, diag.vega_error_max);
  if (!errors.price.empty()) {
    const double denominator = static_cast<double>(errors.price.size());
    diag.half_spread_pass_fraction = static_cast<double>(diag.half_spread_passes) / denominator;
    diag.half_tick_pass_fraction = static_cast<double>(diag.half_tick_passes) / denominator;
    diag.economic_gate_fraction = static_cast<double>(diag.economic_gate_passes) / denominator;
  }
  if (diag.candidates > 0u) {
    const double denominator = static_cast<double>(diag.candidates);
    diag.route_fraction = static_cast<double>(diag.representative_routes) / denominator;
    diag.screen_safe_fraction = static_cast<double>(diag.economic_gate_passes) / denominator;
  }
  if (diag.representative_routes > 0u) {
    const double denominator = static_cast<double>(diag.representative_routes);
    diag.sample_coverage_fraction = static_cast<double>(diag.scored + diag.failures) / denominator;
  }
}

[[nodiscard]] bool is_fast_candidate(const ChainSnapshot &snapshot, std::size_t index) noexcept {
  const double strike = snapshot.strike[index];
  const double maturity = snapshot.T[index];
  const double bid = snapshot.bid[index];
  const double ask = snapshot.ask[index];
  return std::isfinite(strike) && std::isfinite(maturity) && std::isfinite(bid) &&
         std::isfinite(ask) && strike > 0.0 && maturity > 0.0 && bid > 0.0 && ask > bid;
}

[[nodiscard]] std::vector<std::size_t> evenly_spaced_positions(std::size_t size,
                                                               std::size_t limit) {
  if (limit == 0u || size <= limit) {
    std::vector<std::size_t> positions(size);
    for (std::size_t i = 0u; i < size; ++i)
      positions[i] = i;
    return positions;
  }
  std::vector<std::size_t> positions;
  positions.reserve(limit);
  if (limit == 1u) {
    positions.push_back(size / 2u);
    return positions;
  }
  for (std::size_t i = 0u; i < limit; ++i) {
    positions.push_back(i * (size - 1u) / (limit - 1u));
  }
  return positions;
}

[[nodiscard]] std::size_t nearest_target(double value, std::span<const double> targets) noexcept {
  std::size_t best{};
  double distance = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0u; i < targets.size(); ++i) {
    const double candidate = std::fabs(value - targets[i]);
    if (candidate < distance) {
      best = i;
      distance = candidate;
    }
  }
  return best;
}

// Commit mode covers both sides, front/middle/back tenor, ATM/intermediate/wing
// moneyness, and each side's observed tails before filling the remaining budget
// evenly in canonical expiry/strike/side order. Every tie breaks by input order.
[[nodiscard]] std::vector<std::size_t>
stratified_commit_positions(const ChainSnapshot &snapshot, const PricedSurface &surface,
                            std::span<const std::size_t> representative_indices) {
  if (representative_indices.size() <= kCommitSampleLimit) {
    return evenly_spaced_positions(representative_indices.size(), 0u);
  }
  constexpr std::array<double, 3> tenor_targets{0.0, 0.5, 1.0};
  constexpr std::array<double, 5> moneyness_targets{-0.75, -0.25, 0.0, 0.25, 0.75};
  constexpr std::size_t kStrata = 2u * tenor_targets.size() * moneyness_targets.size();
  struct Best {
    std::size_t position{std::numeric_limits<std::size_t>::max()};
    double score{std::numeric_limits<double>::infinity()};
  };
  std::array<Best, kStrata> strata{};
  std::array<Best, 2> lower_tail{};
  std::array<Best, 2> upper_tail{};
  double min_t = std::numeric_limits<double>::infinity();
  double max_t = 0.0;
  for (const std::size_t index : representative_indices) {
    min_t = std::min(min_t, snapshot.T[index]);
    max_t = std::max(max_t, snapshot.T[index]);
  }
  const double tenor_span = max_t > min_t ? max_t - min_t : 1.0;
  double cached_maturity = std::numeric_limits<double>::quiet_NaN();
  double cached_forward = std::numeric_limits<double>::quiet_NaN();
  for (std::size_t position = 0u; position < representative_indices.size(); ++position) {
    const std::size_t index = representative_indices[position];
    const double maturity = snapshot.T[index];
    if (maturity != cached_maturity) {
      cached_maturity = maturity;
      cached_forward = surface.forward_at(maturity);
    }
    if (!(cached_forward > 0.0) || !std::isfinite(cached_forward))
      continue;
    const double k_log = std::log(snapshot.strike[index] / cached_forward);
    if (!std::isfinite(k_log))
      continue;
    const double tenor = (maturity - min_t) / tenor_span;
    const std::size_t side = static_cast<std::size_t>(snapshot.side[index]);
    const std::size_t tenor_bucket = nearest_target(tenor, tenor_targets);
    const std::size_t moneyness_bucket = nearest_target(k_log, moneyness_targets);
    const std::size_t cell = side * tenor_targets.size() * moneyness_targets.size() +
                             tenor_bucket * moneyness_targets.size() + moneyness_bucket;
    const double tenor_error = tenor - tenor_targets[tenor_bucket];
    const double moneyness_error = k_log - moneyness_targets[moneyness_bucket];
    const double score = tenor_error * tenor_error + moneyness_error * moneyness_error;
    if (score < strata[cell].score)
      strata[cell] = Best{position, score};
    if (k_log < lower_tail[side].score)
      lower_tail[side] = Best{position, k_log};
    if (-k_log < upper_tail[side].score)
      upper_tail[side] = Best{position, -k_log};
  }
  std::vector<bool> selected(representative_indices.size(), false);
  std::vector<std::size_t> positions;
  positions.reserve(kCommitSampleLimit);
  const auto select = [&](std::size_t position) {
    if (position < selected.size() && !selected[position] &&
        positions.size() < kCommitSampleLimit) {
      selected[position] = true;
      positions.push_back(position);
    }
  };
  for (const Best &best : strata)
    select(best.position);
  for (const Best &best : lower_tail)
    select(best.position);
  for (const Best &best : upper_tail)
    select(best.position);
  for (const std::size_t position :
       evenly_spaced_positions(representative_indices.size(), kCommitSampleLimit)) {
    select(position);
  }
  for (std::size_t position = 0u;
       position < representative_indices.size() && positions.size() < kCommitSampleLimit;
       ++position) {
    select(position);
  }
  std::sort(positions.begin(), positions.end());
  return positions;
}

[[nodiscard]] bool score_fast_vs_cold(const OptionChain &chain, const PricerFitter &fitter,
                                      double option_tick, PanelMode mode, FastDiagnostics &diag,
                                      std::string &error) {
  auto priced = fitter.surface()->session().to_priced_surface();
  if (!priced.has_value()) {
    error = priced.error().to_string();
    return false;
  }
  auto prepared = std::move(*priced).with_query_pricing(QueryPricingTier::RepresentativeFast);
  if (!prepared.has_value()) {
    error = prepared.error().to_string();
    return false;
  }
  const PricedSurface surface = std::move(*prepared);
  const ChainSnapshot snapshot = chain.snapshot();
  std::vector<std::size_t> representative_indices;
  representative_indices.reserve(snapshot.size());
  std::size_t incompatible_routes{};
  for (std::size_t i = 0u; i < snapshot.size(); ++i) {
    if (!is_fast_candidate(snapshot, i))
      continue;
    ++diag.candidates;
    switch (surface.query_pricing_route(snapshot.strike[i], snapshot.T[i], snapshot.side[i])) {
    case QueryPricingRoute::RepresentativeFast:
      representative_indices.push_back(i);
      break;
    case QueryPricingRoute::ColdFallback:
      ++diag.cold_fallbacks;
      break;
    case QueryPricingRoute::ColdReference:
    case QueryPricingRoute::CarryBank:
      ++incompatible_routes;
      break;
    }
  }
  if (incompatible_routes > 0u) {
    error = "representative-fast surface returned " + std::to_string(incompatible_routes) +
            " incompatible query routes";
    return false;
  }
  diag.representative_routes = representative_indices.size();
  std::sort(representative_indices.begin(), representative_indices.end(),
            [&](std::size_t lhs, std::size_t rhs) {
              return std::tie(snapshot.T[lhs], snapshot.strike[lhs], snapshot.side[lhs], lhs) <
                     std::tie(snapshot.T[rhs], snapshot.strike[rhs], snapshot.side[rhs], rhs);
            });
  const std::vector<std::size_t> positions =
      mode == PanelMode::Commit
          ? stratified_commit_positions(snapshot, surface, representative_indices)
          : evenly_spaced_positions(representative_indices.size(), 0u);
  FastErrors errors;
  errors.price.reserve(positions.size());
  errors.delta.reserve(positions.size());
  errors.gamma.reserve(positions.size());
  errors.theta.reserve(positions.size());
  errors.vega.reserve(positions.size());
  for (const std::size_t position : positions) {
    const std::size_t i = representative_indices[position];
    const double strike = snapshot.strike[i];
    const double maturity = snapshot.T[i];
    const double bid = snapshot.bid[i];
    const double ask = snapshot.ask[i];
    const Side side = snapshot.side[i];
    const auto fast = surface.greeks_analytic(strike, maturity, side);
    const auto cold =
        surface.greeks_analytic(strike, maturity, side, QueryExecution::ColdReference);
    const auto finite_bundle = [](const auto &bundle) noexcept {
      return std::isfinite(bundle.price) && std::isfinite(bundle.delta) &&
             std::isfinite(bundle.gamma) && std::isfinite(bundle.theta) &&
             std::isfinite(bundle.vega);
    };
    if (!fast.has_value() || !cold.has_value() || !finite_bundle(*fast) || !finite_bundle(*cold)) {
      ++diag.failures;
      ++diag.greek_failures;
      continue;
    }
    ++diag.scored;
    ++diag.greek_scored;
    const double absolute_error = std::fabs(fast->price - cold->price);
    const double half_spread = 0.5 * (ask - bid);
    const double half_tick = 0.5 * option_tick;
    const double vega_bound = kEconomicVegaFraction * std::fabs(cold->vega);
    const bool inside_half_spread = absolute_error < half_spread;
    const bool inside_half_tick = absolute_error <= half_tick;
    const bool price_flip = economically_relevant_sign_flip(fast->price, cold->price);
    const bool delta_flip = economically_relevant_sign_flip(fast->delta, cold->delta);
    const bool theta_flip = economically_relevant_sign_flip(fast->theta, cold->theta);
    const bool vega_flip = economically_relevant_sign_flip(fast->vega, cold->vega);
    if (inside_half_spread)
      ++diag.half_spread_passes;
    if (inside_half_tick)
      ++diag.half_tick_passes;
    if (inside_half_spread && absolute_error <= std::min(half_tick, vega_bound) && !price_flip &&
        !delta_flip && !theta_flip && !vega_flip && !(fast->price < 0.0)) {
      ++diag.economic_gate_passes;
    }
    if (price_flip)
      ++diag.price_sign_flips;
    if (delta_flip)
      ++diag.delta_sign_flips;
    if (theta_flip)
      ++diag.theta_sign_flips;
    if (vega_flip)
      ++diag.vega_sign_flips;
    if (fast->price < 0.0)
      ++diag.fast_negative_prices;
    errors.price.push_back(absolute_error);
    errors.delta.push_back(std::fabs(fast->delta - cold->delta));
    errors.gamma.push_back(std::fabs(fast->gamma - cold->gamma));
    errors.theta.push_back(std::fabs(fast->theta - cold->theta));
    errors.vega.push_back(std::fabs(fast->vega - cold->vega));
  }
  finish_fast_diagnostics(diag, std::move(errors));
  return true;
}

[[nodiscard]] Row evaluate_input(const InputSpec &input, const CliArgs &args) {
  Row row;
  row.symbol = input.symbol;
  row.input = input.input;
  row.effective_preset = std::string{preset_name(args.preset)};

  OpraLoadSpec load;
  load.path = input.path.generic_string();
  load.underlying = input.symbol;
  load.snapshot_iso = input.snapshot;
  load.r = args.rate;
  auto panel = atx::vol::load_opra_cbbo_parquet(load);
  if (!panel.has_value()) {
    row.status = panel.error().code() == ErrorCode::NotFound ? "load_missing" : "load_error";
    row.error = panel.error().to_string();
    return row;
  }

  CorpusBoard board = atx::vol::corpus_board_from_opra(input.date, input.symbol, std::move(*panel));
  auto chain = OptionChain::from_frame(board.frame, board.env);
  if (!chain.has_value()) {
    row.status = "chain_error";
    row.error = chain.error().to_string();
    return row;
  }

  PricerConfig config;
  config.preset = args.preset;
  // This executable is the economic oracle for the same legacy production
  // route, not a mark-latency benchmark. Preserve that route while explicitly
  // retaining the parity diagnostics W1.5 may elide for ordinary mark callers.
  config.score_parity = true;
  config.context = board.fit_context;
  config.n_threads = 1u;
  config.fit_workers = 1u;
  PricerFitter fitter{config};
  const auto fit = fitter.fit(*chain);
  record_decision(row, fitter.candidate_decision());
  if (!fit.has_value()) {
    row.status = "fit_error";
    row.error = fit.error().to_string();
    return row;
  }
  record_decision(row, fitter.decision());
  record_market_diagnostics(row, fitter);

  row.valued_options = chain->size();
  if (args.full_chain_valuation) {
    const auto valuation = fitter.value_chain(*chain, OutputField::Prices, 1u);
    if (!valuation.has_value()) {
      row.status = "value_error";
      row.error = valuation.error().to_string();
      return row;
    }
    row.valued_options = valuation->size();
  }
  if (!score_fast_vs_cold(*chain, fitter, args.option_tick, args.mode, row.fast, row.error)) {
    row.status = "fast_diagnostic_error";
    return row;
  }
  row.status = "ok";
  return row;
}

[[nodiscard]] std::string format_double(double value) {
  if (std::isnan(value))
    return "nan";
  if (std::isinf(value))
    return value > 0.0 ? "inf" : "-inf";
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return out.str();
}

template <typename Integer>
[[nodiscard]] std::string format_integer(const std::optional<Integer> &value) {
  return value.has_value() ? std::to_string(*value) : std::string{};
}

[[nodiscard]] std::string format_optional_double(const std::optional<double> &value) {
  return value.has_value() ? format_double(*value) : std::string{};
}

[[nodiscard]] std::string format_bool(const std::optional<bool> &value) {
  return value.has_value() ? (*value ? "1" : "0") : std::string{};
}

[[nodiscard]] std::string csv_escape(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos)
    return std::string{value};
  std::string escaped;
  escaped.reserve(value.size() + 2u);
  escaped.push_back('"');
  for (const char c : value) {
    if (c == '"')
      escaped.push_back('"');
    escaped.push_back(c);
  }
  escaped.push_back('"');
  return escaped;
}

constexpr std::array<std::string_view, 66> kColumns{{
    "symbol",
    "input",
    "status",
    "error",
    "effective_preset",
    "curve_kind",
    "primary_curve_kind",
    "resolved_config",
    "used_fallback",
    "scored_quotes",
    "market_in_band_fraction",
    "market_worst_in_band_fraction",
    "market_reduced_chi_square",
    "market_vol_rmse",
    "market_bid_misses",
    "market_ask_misses",
    "market_max_price_error",
    "calendar_violations",
    "calendar_arb_free",
    "validation_mask",
    "admission_mask",
    "arbitrage_mask",
    "valued_options",
    "fast_candidate_quotes",
    "fast_representative_routes",
    "fast_cold_fallbacks",
    "fast_vs_cold_scored",
    "fast_vs_cold_failures",
    "fast_vs_cold_greek_scored",
    "fast_vs_cold_greek_failures",
    "fast_vs_cold_price_error_p50",
    "fast_vs_cold_price_error_p95",
    "fast_vs_cold_price_error_max",
    "fast_vs_cold_delta_error_p50",
    "fast_vs_cold_delta_error_p95",
    "fast_vs_cold_delta_error_max",
    "fast_vs_cold_gamma_error_p50",
    "fast_vs_cold_gamma_error_p95",
    "fast_vs_cold_gamma_error_max",
    "fast_vs_cold_theta_error_p50",
    "fast_vs_cold_theta_error_p95",
    "fast_vs_cold_theta_error_max",
    "fast_vs_cold_vega_error_p50",
    "fast_vs_cold_vega_error_p95",
    "fast_vs_cold_vega_error_max",
    "fast_vs_cold_price_sign_flips",
    "fast_vs_cold_delta_sign_flips",
    "fast_vs_cold_theta_sign_flips",
    "fast_vs_cold_vega_sign_flips",
    "fast_negative_prices",
    "fast_vs_cold_half_spread_passes",
    "fast_vs_cold_half_tick_passes",
    "fast_vs_cold_economic_gate_passes",
    "fast_vs_cold_half_spread_pass_fraction",
    "fast_vs_cold_half_tick_pass_fraction",
    "fast_vs_cold_economic_gate_fraction",
    "fast_representative_route_fraction",
    "fast_screen_safe_fraction",
    "fast_gate_option_tick",
    "fast_gate_half_tick",
    "fast_gate_vega_fraction",
    "fast_point_limit",
    "fast_greek_sample_limit",
    "mode",
    "full_chain_valuation",
    "fast_sample_coverage_fraction",
}};

[[nodiscard]] std::array<std::string, kColumns.size()> row_fields(const Row &row,
                                                                  const CliArgs &args) {
  return {{
      row.symbol,
      row.input,
      row.status,
      row.error,
      row.effective_preset,
      row.curve_kind,
      row.primary_curve_kind,
      row.resolved_config,
      format_bool(row.used_fallback),
      format_integer(row.scored_quotes),
      format_optional_double(row.market_in_band_fraction),
      format_optional_double(row.market_worst_in_band_fraction),
      format_optional_double(row.market_reduced_chi_square),
      format_optional_double(row.market_vol_rmse),
      format_integer(row.market_bid_misses),
      format_integer(row.market_ask_misses),
      format_optional_double(row.market_max_price_error),
      format_integer(row.calendar_violations),
      format_bool(row.calendar_arb_free),
      format_integer(row.validation_mask),
      format_integer(row.admission_mask),
      format_integer(row.arbitrage_mask),
      format_integer(row.valued_options),
      std::to_string(row.fast.candidates),
      std::to_string(row.fast.representative_routes),
      std::to_string(row.fast.cold_fallbacks),
      std::to_string(row.fast.scored),
      std::to_string(row.fast.failures),
      std::to_string(row.fast.greek_scored),
      std::to_string(row.fast.greek_failures),
      format_optional_double(row.fast.price_error_p50),
      format_optional_double(row.fast.price_error_p95),
      format_optional_double(row.fast.price_error_max),
      format_optional_double(row.fast.delta_error_p50),
      format_optional_double(row.fast.delta_error_p95),
      format_optional_double(row.fast.delta_error_max),
      format_optional_double(row.fast.gamma_error_p50),
      format_optional_double(row.fast.gamma_error_p95),
      format_optional_double(row.fast.gamma_error_max),
      format_optional_double(row.fast.theta_error_p50),
      format_optional_double(row.fast.theta_error_p95),
      format_optional_double(row.fast.theta_error_max),
      format_optional_double(row.fast.vega_error_p50),
      format_optional_double(row.fast.vega_error_p95),
      format_optional_double(row.fast.vega_error_max),
      std::to_string(row.fast.price_sign_flips),
      std::to_string(row.fast.delta_sign_flips),
      std::to_string(row.fast.theta_sign_flips),
      std::to_string(row.fast.vega_sign_flips),
      std::to_string(row.fast.fast_negative_prices),
      std::to_string(row.fast.half_spread_passes),
      std::to_string(row.fast.half_tick_passes),
      std::to_string(row.fast.economic_gate_passes),
      format_optional_double(row.fast.half_spread_pass_fraction),
      format_optional_double(row.fast.half_tick_pass_fraction),
      format_optional_double(row.fast.economic_gate_fraction),
      format_optional_double(row.fast.route_fraction),
      format_optional_double(row.fast.screen_safe_fraction),
      format_double(args.option_tick),
      format_double(0.5 * args.option_tick),
      format_double(kEconomicVegaFraction),
      std::to_string(args.mode == PanelMode::Commit ? kCommitSampleLimit : 0u),
      std::to_string(args.mode == PanelMode::Commit ? kCommitSampleLimit : 0u),
      std::string{mode_name(args.mode)},
      args.full_chain_valuation ? "1" : "0",
      format_optional_double(row.fast.sample_coverage_fraction),
  }};
}

template <typename Fields> void write_csv_fields(std::ostream &output, const Fields &fields) {
  for (std::size_t i = 0u; i < fields.size(); ++i) {
    if (i > 0u)
      output.put(',');
    output << csv_escape(fields[i]);
  }
  output.put('\n');
}

[[nodiscard]] bool write_panel(const std::filesystem::path &path, const std::vector<Row> &rows,
                               const CliArgs &args, std::string &error) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    error = "cannot open output file: " + path.generic_string();
    return false;
  }
  output.imbue(std::locale::classic());
  write_csv_fields(output, kColumns);
  for (const Row &row : rows)
    write_csv_fields(output, row_fields(row, args));
  output.flush();
  if (!output) {
    error = "failed writing output file: " + path.generic_string();
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::locale::global(std::locale::classic());
  (void)std::setlocale(LC_ALL, "C");
  std::string error;
  const auto args = parse_args(argc, argv, error);
  if (!args.has_value()) {
    if (!error.empty()) {
      std::fprintf(stderr, "accuracy_panel: %s\n", error.c_str());
      print_usage();
      return 2;
    }
    return 0;
  }
  auto symbols = read_symbols(args->symbols_path, error);
  if (!symbols.has_value()) {
    std::fprintf(stderr, "accuracy_panel: %s\n", error.c_str());
    return 2;
  }
  std::vector<InputSpec> inputs = build_inputs(*args, std::move(*symbols));
  std::vector<Row> rows;
  rows.reserve(inputs.size());
  const auto exception_row = [&](const InputSpec &input, std::string message) {
    Row row;
    row.symbol = input.symbol;
    row.input = input.input;
    row.status = "exception";
    row.error = std::move(message);
    row.effective_preset = std::string{preset_name(args->preset)};
    return row;
  };
  for (const InputSpec &input : inputs) {
    try {
      rows.push_back(evaluate_input(input, *args));
    } catch (const std::exception &exception) {
      rows.push_back(exception_row(input, exception.what()));
    } catch (...) {
      rows.push_back(exception_row(input, "unknown exception"));
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row &lhs, const Row &rhs) {
    return std::tie(lhs.symbol, lhs.input) < std::tie(rhs.symbol, rhs.input);
  });
  if (!write_panel(args->output_path, rows, *args, error)) {
    std::fprintf(stderr, "accuracy_panel: %s\n", error.c_str());
    return 1;
  }
  std::map<std::string, std::size_t> statuses;
  for (const Row &row : rows)
    ++statuses[row.status];
  std::printf("accuracy_panel rows=%zu", rows.size());
  for (const auto &[status, count] : statuses) {
    std::printf(" %s=%zu", status.c_str(), count);
  }
  std::printf(" out=%s\n", args->output_path.generic_string().c_str());
  return 0;
}
