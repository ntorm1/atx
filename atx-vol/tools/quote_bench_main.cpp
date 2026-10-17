// Offline orchestration only: model input is a closed, label-free CSV schema.
// Numerical work lives in reusable fitting/pricing APIs, independent of data files.
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/marketdata/dividend_schedule.hpp"
#include "atx/vol/api/pricing/dividend.hpp"
#include "quote_bench_policy.hpp"
#include "quote_bench_common.hpp"
#include "quote_bench_progress.hpp"
#include "quote_bench_data.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace atx::vol;
using Clock = std::chrono::steady_clock;
using quote_bench_policy::Json;
using quote_bench::InputRow;
using quote_bench::Prediction;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Arguments {
  std::filesystem::path input;
  std::filesystem::path output;
  std::filesystem::path report;
  std::filesystem::path progress;
  std::string curve;
  std::string policy{"auto"};
  std::string model{"quote-slice"};
  std::string common_policy{"auto"};
  std::string dividends{"funding-pv"};
  std::size_t max_rows{2000000U};
  QuoteSliceQualityConfig quality;
  std::size_t min_quotes{6};
  bool help_requested{false};
};

template <class T> T number(std::string_view value, std::string_view column) {
  T out{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::runtime_error("Invalid numeric field: " + std::string(column));
  }
  return out;
}

double real(std::string_view value, std::string_view column) {
  return value.empty() ? kNaN : number<double>(value, column);
}

bool same_file(const std::filesystem::path &left, const std::filesystem::path &right) {
  if (std::filesystem::exists(left) && std::filesystem::exists(right) &&
      std::filesystem::equivalent(left, right)) {
    return true; // Includes hardlinks, which canonical path strings do not resolve.
  }
  auto a = std::filesystem::weakly_canonical(left).native();
  auto b = std::filesystem::weakly_canonical(right).native();
#ifdef _WIN32
  const auto lower = [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); };
  std::transform(a.begin(), a.end(), a.begin(), lower);
  std::transform(b.begin(), b.end(), b.begin(), lower);
#endif
  return a == b;
}

Arguments arguments(int argc, char **argv) {
  Arguments args;
  std::set<std::string_view> seen;
  bool quality_specified = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key{argv[i]};
    if (!seen.insert(key).second) {
      throw std::runtime_error("Duplicate argument: " + std::string(key));
    }
    if (key == "--help") {
      std::cout << "atx-vol-quote-bench --input quotes.csv --output predictions.csv "
                   "[--report timing.json] [--model quote-slice|common-markov] "
                   "[--policy auto|policy.json | --curve CURVE] "
                   "[--common-policy auto|common-policy.json] "
                   "[--progress fresh-progress.jsonl] "
                   "[--dividends funding-pv|escrowed-pv|none] [--max-rows 2000000] "
                   "[--min-quotes 6] [--max-spread-to-mid 0.6] [--max-spread-vol 0.05] "
                   "[--max-weight 1000]\n"
                   "Defaults: --policy auto --dividends funding-pv. "
                   "Quality/min-quotes flags require explicit --curve.\n";
      args.help_requested = true;
      return args;
    }
    if (i + 1 == argc) throw std::runtime_error("Missing value for " + std::string(key));
    const std::string value{argv[++i]};
    if (key == "--input") args.input = value;
    else if (key == "--output") args.output = value;
    else if (key == "--report") args.report = value;
    else if (key == "--progress") args.progress = value;
    else if (key == "--curve") args.curve = value;
    else if (key == "--policy") args.policy = value;
    else if (key == "--model") args.model = value;
    else if (key == "--common-policy") args.common_policy = value;
    else if (key == "--dividends") args.dividends = value;
    else if (key == "--max-rows") args.max_rows = number<std::size_t>(value, key);
    else if (key == "--min-quotes") args.min_quotes = number<std::size_t>(value, key);
    else if (key == "--max-spread-to-mid") args.quality.max_spread_to_mid = real(value, key);
    else if (key == "--max-spread-vol") args.quality.max_spread_vol = real(value, key);
    else if (key == "--max-weight") args.quality.max_weight = real(value, key);
    else throw std::runtime_error("Unknown argument: " + std::string(key));
    quality_specified = quality_specified || key == "--min-quotes" ||
        key == "--max-spread-to-mid" || key == "--max-spread-vol" || key == "--max-weight";
  }
  if (seen.contains("--policy") && seen.contains("--curve")) {
    throw std::runtime_error("--policy and --curve are mutually exclusive");
  }
  if (args.model != "quote-slice" && args.model != "common-markov") {
    throw std::runtime_error("--model must be quote-slice or common-markov");
  }
  if (args.common_policy.empty()) throw std::runtime_error("Common policy must not be empty");
  if (args.model == "common-markov" &&
      (seen.contains("--policy") || seen.contains("--curve") || quality_specified)) {
    throw std::runtime_error("common-markov requires --common-policy; quote-slice policy, "
                             "curve and quality flags are incompatible");
  }
  if (args.model == "quote-slice" && seen.contains("--common-policy")) {
    throw std::runtime_error("--common-policy requires --model common-markov");
  }
  if (seen.contains("--progress") &&
      (args.model != "common-markov" || args.progress.empty())) {
    throw std::runtime_error("--progress requires common-markov and a fresh output path");
  }
  if ((seen.contains("--curve") && args.curve.empty()) || args.policy.empty()) {
    throw std::runtime_error("Policy and curve names must not be empty");
  }
  if (quality_specified && args.curve.empty()) {
    throw std::runtime_error("Global quality/min-quotes flags require explicit --curve; "
                             "configure policy profiles instead");
  }
  if (args.input.empty() || args.output.empty() || args.max_rows == 0U) {
    throw std::runtime_error("--input, --output and a positive --max-rows are required");
  }
  if (args.dividends != "funding-pv" && args.dividends != "escrowed-pv" &&
      args.dividends != "none") {
    throw std::runtime_error("--dividends must be funding-pv, escrowed-pv or none");
  }
  if (args.min_quotes < 2 || !std::isfinite(args.quality.max_weight) ||
      args.quality.max_weight <= 0.0 ||
      !std::isfinite(args.quality.max_spread_to_mid) || args.quality.max_spread_to_mid < 0.0 ||
      !std::isfinite(args.quality.max_spread_vol) || args.quality.max_spread_vol < 0.0) {
    throw std::runtime_error("Invalid quote quality configuration");
  }
  if (same_file(args.input, args.output)) {
    throw std::runtime_error("Input and output must differ");
  }
  if (!args.report.empty() &&
      (same_file(args.report, args.input) || same_file(args.report, args.output))) {
    throw std::runtime_error("Timing report must differ from input and predictions");
  }
  if (args.curve.empty() && args.policy != "auto" &&
      (same_file(args.policy, args.input) || same_file(args.policy, args.output) ||
       (!args.report.empty() && same_file(args.policy, args.report)))) {
    throw std::runtime_error("Policy file must differ from input, predictions and report");
  }
  if (args.model == "common-markov" && args.common_policy != "auto" &&
      (same_file(args.common_policy, args.input) || same_file(args.common_policy, args.output) ||
       (!args.report.empty() && same_file(args.common_policy, args.report)))) {
    throw std::runtime_error("Common policy file must differ from input, predictions and report");
  }
  if (!args.progress.empty() &&
      (std::filesystem::exists(args.progress) || same_file(args.progress, args.input) ||
       same_file(args.progress, args.output) ||
       (!args.report.empty() && same_file(args.progress, args.report)) ||
       (args.common_policy != "auto" && same_file(args.progress, args.common_policy)))) {
    throw std::runtime_error("Progress journal must use a new path distinct from all other files");
  }
  return args;
}

// RFC 4180 escaping within one record; embedded newlines are deliberately refused.
std::vector<std::string> csv_record(std::string_view line) {
  std::vector<std::string> result;
  std::string field;
  bool quoted = false;
  bool closed = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c != '"') field.push_back(c);
      else if (i + 1U < line.size() && line[i + 1U] == '"') {
        field.push_back('"');
        ++i;
      } else {
        quoted = false;
        closed = true;
      }
    } else if (c == ',') {
      result.push_back(std::move(field));
      field.clear();
      closed = false;
    } else if (c == '"' && field.empty() && !closed) {
      quoted = true;
    } else if (closed || c == '"') {
      throw std::runtime_error("Malformed CSV quoting");
    } else {
      field.push_back(c);
    }
  }
  if (quoted) throw std::runtime_error("Unterminated CSV quote or embedded newline");
  result.push_back(std::move(field));
  return result;
}

using Header = std::map<std::string, std::size_t, std::less<>>;

Header read_header(std::istream &stream) {
  std::string line;
  if (!std::getline(stream, line)) throw std::runtime_error("CSV input is empty");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const std::set<std::string, std::less<>> allowed{
      "row_id", "chain_id", "snapshot_ns", "observation_ns", "underlier", "root",
      "expiry_days", "strike", "is_call", "exercise", "spot_bid", "spot_ask", "spot",
      "bid", "ask", "bid_size", "ask_size", "years", "years_calendar", "rate", "sdiv",
      "ddiv", "ddiv_pv", "split", "input_status"};
  const auto fields = csv_record(line);
  Header header;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (!allowed.contains(fields[i])) {
      throw std::runtime_error("Column not in quote-only input schema: " + fields[i]);
    }
    if (!header.emplace(fields[i], i).second) throw std::runtime_error("Duplicate CSV column");
  }
  for (const auto key : {"row_id", "chain_id", "strike", "is_call", "spot", "bid", "ask",
                         "years", "years_calendar", "rate", "sdiv", "ddiv_pv",
                         "input_status", "exercise", "underlier", "root", "expiry_days",
                         "snapshot_ns"}) {
    if (!header.contains(key)) throw std::runtime_error("Missing column: " + std::string(key));
  }
  return header;
}

InputRow convert_row(const Header &header, const std::vector<std::string> &fields) {
  if (fields.size() != header.size()) throw std::runtime_error("CSV field count mismatch");
  const auto get = [&](std::string_view key) -> std::string_view {
    return fields[header.find(key)->second];
  };
  InputRow row;
  row.id = number<std::uint64_t>(get("row_id"), "row_id");
  row.chain = number<std::uint64_t>(get("chain_id"), "chain_id");
  const std::array<std::string_view, 4> identity_keys{
      "underlier", "root", "expiry_days", "snapshot_ns"};
  for (std::size_t i = 0; i < identity_keys.size(); ++i) {
    if (header.contains(identity_keys[i])) row.identity[i] = get(identity_keys[i]);
  }
  // Canonical numeric identity prevents alternate spellings (e.g. 01 and 1)
  // from bypassing same-snapshot and duplicate-expiry admission.
  if (!row.identity[2].empty()) {
    row.identity[2] = std::to_string(number<std::int32_t>(row.identity[2], "expiry_days"));
  }
  if (!row.identity[3].empty()) {
    row.identity[3] = std::to_string(number<std::int64_t>(row.identity[3], "snapshot_ns"));
  }
  row.input_status = get("input_status");
  if (row.input_status != "ok") return row;
  if (std::any_of(row.identity.begin(), row.identity.end(),
                  [](const std::string &value) { return value.empty(); })) {
    throw std::runtime_error("Eligible contract lacks calendar/underlier identity");
  }
  // OCC numeric suffixes 1-9 identify adjusted, nonstandard deliverables.
  // The imported schema has no share ratio/cash basket: treating these as
  // ordinary contracts fabricates intrinsic value and exercise economics.
  const auto &root = row.identity[1];
  if (!root.empty() && root.back() >= '1' && root.back() <= '9') {
    row.input_status = "unsupported_deliverable";
    return row;
  }
  if (get("exercise") != "American") {
    row.input_status = "unsupported_exercise";
    return row;
  }
  row.market = {real(get("spot"), "spot"), real(get("years"), "years"),
                real(get("years_calendar"), "years_calendar"), real(get("rate"), "rate"),
                real(get("sdiv"), "sdiv"), real(get("ddiv_pv"), "ddiv_pv")};
  const auto side = number<unsigned>(get("is_call"), "is_call");
  if (side > 1U) throw std::runtime_error("is_call must be zero or one");
  row.quote = {real(get("strike"), "strike"), side == 1U ? Side::Call : Side::Put,
               real(get("bid"), "bid"), real(get("ask"), "ask")};
  row.quote.continuous_yield = row.market.continuous_yield;
  if (header.contains("ddiv")) {
    // Never throws (InputRow::dividend_sum): only the opt-in schedule reads it.
    const auto cell = get("ddiv");
    double value = kNaN;
    const auto [end, error] = std::from_chars(cell.data(), cell.data() + cell.size(), value);
    if (error == std::errc{} && end == cell.data() + cell.size()) {
      row.dividend_sum = value;
    } else {
      row.dividend_sum_malformed = !cell.empty();
    }
  }
  return row;
}

std::vector<InputRow> read_rows(const Arguments &args) {
  std::ifstream stream{args.input};
  if (!stream) throw std::runtime_error("Cannot open input CSV");
  const Header header = read_header(stream);
  std::vector<InputRow> rows;
  std::set<std::uint64_t> ids;
  std::string line;
  while (std::getline(stream, line)) {
    if (rows.size() >= args.max_rows) {
      throw std::runtime_error("Input exceeds --max-rows; shard the cohort or raise the limit");
    }
    if (line.size() > 1048576U) throw std::runtime_error("CSV record exceeds 1 MiB");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto row = convert_row(header, csv_record(line));
    if (!ids.insert(row.id).second) throw std::runtime_error("Duplicate row_id");
    rows.push_back(std::move(row));
  }
  if (!stream.eof()) throw std::runtime_error("Input CSV read failed");
  if (rows.empty()) throw std::runtime_error("Input contains no rows");
  return rows;
}

double median(std::vector<double> values) {
  if (values.empty()) return kNaN;
  if (!std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); })) {
    return kNaN;
  }
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2U;
  return values.size() % 2U ? values[mid] : 0.5 * values[mid - 1U] + 0.5 * values[mid];
}

struct RunStatistics {
  std::size_t chains{};
  std::size_t fitted{};
  std::size_t served{};
  std::size_t admitted{};
  std::size_t dividend_chains{};
  double max_spot_range_bps{};
  double elapsed_seconds{};
  std::vector<double> chain_microseconds;
  std::map<std::string, std::size_t> fit_failures;
  std::size_t calendar_pairs{};
  std::size_t calendar_rejected_chains{};
  std::size_t ambiguous_expiry_chains{};
  Json policy_definition;
  Json policy_decisions = Json::array();
  Json per_profile_counts = Json::object();
  // Per-expiry carry decisions; empty unless a profile opts out of supplied carry.
  Json carry_resolutions = Json::array();
  Json carry_status_counts = Json::object();
  bool graded{false};
  bool graded_channel_repair{false}; // opt-in graded keys seen in this run
  bool graded_dominance{false};
  Json graded_stats = Json::object();
  // Opt-in policy key `price_only`: rows served as ok_price_only (never in `served`).
  bool price_only{false};
  std::size_t price_only_rows{};
  // Inferred dividend schedules and per-chain routes; empty unless a profile
  // sets the policy key `dividends` to an inferred-schedule mode.
  Json dividend_schedules = Json::array();
  Json dividend_routes = Json::array();
  Json schedule_status_counts = Json::object();
  Json schedule_reason_counts = Json::object();
  Json event_kind_counts = Json::object();
  Json route_chain_counts = Json::object();
  Json route_contract_counts = Json::object();
  Json route_reason_counts = Json::object();
  // Opted-in rows whose ddiv cell is not a number (their snapshot's schedule
  // is refused as non_finite_input).
  std::size_t malformed_ddiv_rows{};
  std::vector<double> lattice_chain_microseconds;
  std::vector<double> escrow_chain_microseconds;
};

void bump(Json &counts, const std::string &key, std::size_t count = 1U) {
  counts[key] = counts.value(key, std::size_t{}) + count;
}

Json finite_or_null(double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); }

Json carry_resolution_json(const QuoteSliceCarryResolution &r, std::uint64_t chain,
                           const InputRow &row, const QuoteSliceMarket &market) {
  return {{"chain_id", chain}, {"underlier", row.identity[0]}, {"root", row.identity[1]},
          {"expiry_days", row.identity[2]}, {"calendar_years", market.calendar_years},
          {"variance_years", market.variance_years}, {"mode", to_string(r.mode)},
          {"applied", r.applied}, {"fallback_cause", to_string(r.fallback)},
          {"status", to_string(r.status)}, {"reason", r.reason},
          {"supplied_forward", finite_or_null(r.supplied_forward)},
          {"supplied_yield", finite_or_null(r.supplied_yield)},
          {"implied_forward", finite_or_null(r.implied_forward)},
          {"implied_yield", finite_or_null(r.implied_yield)},
          {"implied_total_yield", finite_or_null(r.implied_total_yield)},
          {"parity_forward", finite_or_null(r.parity_forward)},
          {"forward_lower", finite_or_null(r.forward_lower)},
          {"forward_upper", finite_or_null(r.forward_upper)},
          {"dispersion_sd", finite_or_null(r.dispersion_sd)},
          {"exercise_shift_sd", finite_or_null(r.exercise_shift_sd)},
          {"atm_total_vol", finite_or_null(r.atm_total_vol)},
          {"pairs_valid", r.pairs_valid}, {"pairs_selected", r.pairs_selected},
          {"pairs_retained", r.pairs_retained}, {"pairs_contaminated", r.pairs_contaminated},
          {"pairs_unidentified", r.pairs_unidentified}};
}

// Configured fit family as reported in predictions and decisions.
std::string configured_curve(const QuoteFitProfile &profile) {
  const auto &c = profile.config;
  if (c.method != QuoteSliceFitMethod::ChannelSpline) return to_string(c.curve.kind);
  return c.channel.family ? "channel-" + std::string{to_string(*c.channel.family)}
                          : std::string{"knot-spline"};
}

// Contract-level underlying quotes can differ inside one recorded chain snapshot.
// The median is robust to isolated stale legs; dispersion is explicitly reported.
QuoteSliceMarket chain_market(const std::vector<InputRow> &rows,
                              const std::vector<std::size_t> &indices,
                              RunStatistics &stats) {
  QuoteSliceMarket market;
  const auto column = [&](double QuoteSliceMarket::*member) {
    std::vector<double> values;
    values.reserve(indices.size());
    for (const auto i : indices) values.push_back(rows[i].market.*member);
    return median(std::move(values));
  };
  market.spot = column(&QuoteSliceMarket::spot);
  market.variance_years = column(&QuoteSliceMarket::variance_years);
  market.calendar_years = column(&QuoteSliceMarket::calendar_years);
  market.rate = column(&QuoteSliceMarket::rate);
  market.continuous_yield = column(&QuoteSliceMarket::continuous_yield);
  market.dividend_pv = column(&QuoteSliceMarket::dividend_pv);
  if (std::isfinite(market.spot) && market.spot > 0.0) {
    double lo = market.spot;
    double hi = market.spot;
    for (const auto i : indices) {
      lo = std::min(lo, rows[i].market.spot);
      hi = std::max(hi, rows[i].market.spot);
    }
    stats.max_spot_range_bps = std::max(stats.max_spot_range_bps,
                                      10000.0 * (hi - lo) / market.spot);
  }
  return market;
}

bool consistent_chain(const std::vector<InputRow> &rows,
                      const std::vector<std::size_t> &indices) {
  const auto &first = rows[indices.front()];
  std::set<std::pair<double, Side>> contracts;
  for (const auto i : indices) {
    const auto &row = rows[i];
    if (row.identity != first.identity || !std::isfinite(row.quote.strike)) return false;
    if (!contracts.emplace(row.quote.strike, row.quote.side).second) return false;
    // Variance/funding clocks define an expiry; averaging distinct expiries
    // would silently create a contract that was never supplied by the caller.
    for (const auto member : {&QuoteSliceMarket::variance_years,
                              &QuoteSliceMarket::calendar_years}) {
      const double a = row.market.*member;
      const double b = first.market.*member;
      if (!std::isfinite(a) || !std::isfinite(b) ||
          std::abs(a - b) > 1.0e-10 * std::max({1.0, std::abs(a), std::abs(b)})) return false;
    }
  }
  return true;
}

struct PreparedChain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
  std::shared_ptr<const QuoteFitProfile> profile;
  std::string source;
};
using SnapshotIdentity = std::array<std::string, 3>;
using ChainRows = std::map<std::uint64_t, std::vector<std::size_t>>;

void count_into(Json &counts, const std::string &key, std::size_t count = 1U) {
  if (counts.is_null()) counts = Json::object();
  counts[key] = counts.value(key, std::size_t{}) + count;
}

Json repair_json(const QuoteSliceRepairRecord &r) {
  return {{"outcome", to_string(r.outcome)}, {"nodes", r.nodes},
          {"material_nodes", r.material_nodes}, {"max_price_move", r.max_price_move},
          {"max_vol_move", r.max_vol_move}, {"max_unquoted_vol_move", r.max_unquoted_vol_move},
          {"unidentified", r.unidentified}};
}

// A served fit's PriceOnly row: a certified price without an identifiable
// American volatility. Its vol_status records that the volatility was evaluated
// and is unidentifiable. With the policy key `price_only` the price is served
// (status ok_price_only, blank vol; returns true); otherwise the row keeps the
// historical status invalid_volatility and no price.
bool publish_price_only(const QuoteSliceRow &row, bool serve, Prediction &prediction) {
  prediction.vol_status = "unidentifiable";
  if (!serve) {
    prediction.status = "invalid_volatility";
    return false;
  }
  prediction.price = row.fitted_price;
  prediction.status = "ok_price_only";
  return true;
}

// Graded admission: one surface fit per exact snapshot. Routing has already
// refused ambiguous expiries and resolved ONE profile per snapshot; calendar
// consistency is enforced inside fit_quote_surface on the final prices. A
// snapshot with more than kQuoteSurfaceMaxExpiries expiries is left to the
// static per-expiry path (`static_fallback`) rather than refused wholesale.
void fit_graded(const ChainRows &chains, const std::map<std::uint64_t, PreparedChain> &prepared,
                const std::map<SnapshotIdentity, std::vector<std::uint64_t>> &snapshot_chains,
                const std::set<std::uint64_t> &ambiguous, const QuoteSurfaceConfig &surface,
                std::vector<Prediction> &predictions, RunStatistics &stats,
                std::set<std::uint64_t> &fitted_chains, std::set<std::uint64_t> &static_fallback) {
  auto &graded = stats.graded_stats;
  for (const auto &[identity, ids] : snapshot_chains) {
    static_cast<void>(identity);
    std::vector<std::uint64_t> members;
    for (const auto id : ids) {
      if (!ambiguous.contains(id) && prepared.at(id).profile) members.push_back(id);
    }
    if (members.empty()) continue;
    if (members.size() > kQuoteSurfaceMaxExpiries) {
      static_fallback.insert(members.begin(), members.end());
      count_into(graded["static_fallback"], "snapshots");
      count_into(graded["static_fallback"], "chains", members.size());
      continue;
    }
    const auto &profile = *prepared.at(members.front()).profile;
    std::vector<QuoteSurfaceExpiry> expiries;
    expiries.reserve(members.size());
    for (const auto id : members) {
      const auto &chain = prepared.at(id);
      expiries.push_back({chain.market, chain.quotes, {}});
      count_into(stats.per_profile_counts[profile.name], "attempted_chains");
      if (chain.market.dividend_pv > 0.0) ++stats.dividend_chains;
    }
    const auto start = Clock::now();
    const auto fitted = fit_quote_surface(expiries, profile.config, surface);
    const double per_chain = std::chrono::duration<double, std::micro>(Clock::now() - start)
        .count() / static_cast<double>(members.size());
    for (std::size_t s = 0; s < members.size(); ++s) {
      stats.chain_microseconds.push_back(per_chain);
      const auto &indices = chains.at(members[s]);
      if (!fitted) {
        ++stats.fit_failures[fitted.error().to_string()];
        for (const auto i : indices) predictions[i].status = "invalid_market";
        continue;
      }
      const auto &slice = (*fitted)[s];
      if (slice.refusal == "invalid_input") {
        ++stats.fit_failures[slice.error];
        for (const auto i : indices) predictions[i].status = "invalid_market";
        continue;
      }
      const auto &fit = slice.fit;
      const bool served = slice.grade != QuoteSliceGrade::Refused;
      const std::string grade = served ? to_string(slice.grade) : slice.refusal;
      const auto &d = fit.diagnostics;
      Json entry{
          {"chain_id", members[s]}, {"profile", profile.name},
          {"variance_years", fit.market.variance_years}, {"grade", grade},
          {"error", slice.error}, {"point_status", to_string(slice.point_status)},
          {"admitted", d.quotes_admitted}, {"distinct_strikes", d.distinct_strikes},
          {"channel_refit", slice.channel_refit}, {"channel_quotes", d.channel.channel_quotes},
          {"mid_strikes", d.channel.mid_strikes}, {"violations", d.channel.violations},
          {"max_violation_half_widths", d.channel.max_violation_half_widths},
          {"prior_strength", d.channel.prior_strength}, {"term_prior", d.channel.term_prior},
          {"converged", d.channel.converged}, {"iterations", d.channel.iterations},
          {"solve_us", d.channel.solve_seconds * 1.0e6},
          {"repaired_nodes", d.channel.repaired_nodes},
          {"repair_max_price", d.channel.repair_max_price},
          {"repair_max_half_spreads", std::isfinite(d.channel.repair_max_half_spreads)
              ? Json(d.channel.repair_max_half_spreads) : Json("inf")},
          {"calendar_retry", slice.calendar_retry},
          {"calendar_repaired", slice.calendar_repaired}, {"calendar_gap", slice.calendar_gap},
          {"calendar_repair_required", std::isfinite(slice.calendar_repair_required)
              ? Json(slice.calendar_repair_required) : Json("inf")},
          {"calendar_gap_strike", slice.calendar_gap_strike}};
      // Opt-in provenance only, so every pre-existing graded report is unchanged.
      const bool channel_repair = profile.config.repair.mode == QuoteSliceRepairMode::Channel;
      const bool dominance = profile.config.channel.point_max_outside_fraction.has_value();
      stats.graded_channel_repair = stats.graded_channel_repair || channel_repair;
      stats.graded_dominance = stats.graded_dominance || dominance;
      if (channel_repair || dominance) entry["point_error"] = slice.point_error;
      if (channel_repair) {
        entry["repair"] = repair_json(d.repair);
        entry["repair_rung"] = slice.repair_rung;
        entry["rung_repair"] = repair_json(slice.rung_repair);
        entry["calendar_channel_repaired"] = slice.calendar_channel_repaired;
        entry["calendar_channel_repair"] = repair_json(slice.calendar_channel_repair);
      }
      if (dominance) {
        entry["point_outside_fraction"] = slice.point_outside_fraction;
        entry["point_outside_quotes"] = slice.point_outside_quotes;
        entry["calendar_readmitted"] = slice.calendar_readmitted;
        entry["point_gated"] = slice.point_gated;
        entry["point_fallback"] = slice.point_fallback;
      }
      graded["slices"].push_back(std::move(entry));
      count_into(graded["chains"], grade);
      if (slice.channel_refit) count_into(graded["channel_refit_chains"], grade);
      if (slice.calendar_retry) count_into(graded["calendar_retry_chains"], grade);
      if (slice.calendar_repaired) count_into(graded["calendar_repaired_chains"], grade);
      if (served && d.repair.outcome == QuoteSliceRepairOutcome::Applied) {
        count_into(graded["channel_repaired_chains"], grade);
      }
      if (slice.repair_rung) count_into(graded["repair_rung_chains"], grade);
      if (slice.calendar_channel_repaired) {
        count_into(graded["calendar_channel_repaired_chains"], grade);
      }
      if (slice.point_gated) count_into(graded["point_gated_chains"], grade);
      if (slice.point_fallback) count_into(graded["point_fallback_chains"], grade);
      if (slice.point_gated && slice.channel_refit) {
        count_into(graded["point_refit_chains"], grade);
      }
      if (slice.calendar_readmitted) count_into(graded["calendar_readmitted_chains"], grade);
      if (served) {
        ++stats.fitted;
        fitted_chains.insert(members[s]);
      } else {
        ++stats.fit_failures[slice.error.empty() ? slice.refusal : slice.error];
      }
      stats.admitted += fit.diagnostics.quotes_admitted;
      for (std::size_t j = 0; j < indices.size(); ++j) {
        const auto &result = fit.rows[j];
        auto &prediction = predictions[indices[j]];
        if (fit.curve) prediction.actual_curve = to_string(fit.curve->kind());
        prediction.quote_status = to_string(result.quote_status);
        prediction.admitted = result.admitted;
        if (served && result.evaluation_status == QuoteSliceEvaluationStatus::Success) {
          prediction.price = result.fitted_price;
          prediction.vol = result.fitted_iv;
          prediction.status = "ok";
          prediction.grade = grade;
          count_into(graded["served_rows"], grade);
          ++stats.served;
        } else if (!served) {
          prediction.status = slice.refusal;
        } else if (result.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) {
          if (publish_price_only(result, stats.price_only, prediction)) {
            prediction.grade = grade;
            count_into(graded["price_only_rows"], grade);
            ++stats.price_only_rows;
          }
        } else {
          prediction.status = to_string(result.evaluation_status);
        }
      }
    }
  }
}

// ── Opt-in inferred dividend schedule (policy key `dividends`) ──────────────
//
// The aggregate-PV models fold every cash dividend into one continuous yield,
// which cannot place an early-exercise decision at an ex-date. When a profile
// opts in, each snapshot's (ddiv, ddiv_pv, rate) expiry ladder becomes a dated
// schedule (api/marketdata/dividend_schedule.hpp) and every expiry whose life
// holds an inferred ex-date goes through the shared discrete-dividend route
// (discrete_div_route, Lattice) into the quote slice's DatedCash lattice under
// the profile's `dividend_lattice` scheme (spliced V&N unless set).
// Every other expiry keeps the historical aggregate path bit for bit. Day
// numbering: the UTC day of snapshot_ns is the valuation date (US listed
// sessions end before 00:00 UTC); expiry_days is already a day number.
using quote_bench_policy::DividendRouting;

struct ScheduleRecord {
  DividendScheduleResult schedule;
  DividendRouting routing{DividendRouting::Aggregate};
};

std::int32_t valuation_day(std::string_view snapshot_ns) {
  const auto ns = number<std::int64_t>(snapshot_ns, "snapshot_ns");
  if (ns < 0) throw std::runtime_error("snapshot_ns precedes the epoch");
  return static_cast<std::int32_t>(ns / kDividendScheduleNsPerDay);
}

// Proleptic Gregorian date of a day number (days since 1970-01-01), for the
// readable schedule provenance only (Hinnant's civil_from_days).
std::string iso_date(std::int32_t day_number) {
  const std::int64_t z = static_cast<std::int64_t>(day_number) + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  const auto pad = [](std::int64_t value, std::size_t width) {
    auto text = std::to_string(value);
    return std::string(text.size() < width ? width - text.size() : 0U, '0') + text;
  };
  return pad(year, 4U) + "-" + pad(month, 2U) + "-" + pad(day, 2U);
}

Json schedule_json(const SnapshotIdentity &id, const ScheduleRecord &record) {
  const auto &s = record.schedule;
  Json events = Json::array();
  for (const auto &ev : s.view()) {
    events.push_back({{"ex_date", iso_date(ev.ex_day)}, {"days", ev.days},
                      {"amount", ev.amount}, {"kind", to_string(ev.kind)},
                      {"increment_expiry", iso_date(ev.increment_expiry_day)},
                      {"increment_events", ev.increment_events}});
  }
  return {{"underlier", id[0]}, {"root", id[1]}, {"snapshot_ns", id[2]},
          {"routing", to_string(record.routing)}, {"valuation_date", iso_date(s.valuation_day)},
          {"status", to_string(s.status)}, {"reason", to_string(s.reason)},
          {"expiries", s.expiries}, {"resolved_through", iso_date(s.resolved_through_day)},
          {"failed_expiry", s.failed_expiry_day == 0 ? Json(nullptr)
                                                     : Json(iso_date(s.failed_expiry_day))},
          {"increments", s.increments}, {"exact_increments", s.exact_increments},
          {"split_increments", s.split_increments},
          {"non_integer_increments", s.non_integer_increments},
          {"unit_amount", s.unit_amount}, {"cadence_days", s.cadence_days},
          {"max_pv_relative_residual", s.max_pv_relative_residual},
          {"max_sum_abs_residual", s.max_sum_abs_residual},
          {"pv_residual_exceedances", s.pv_residual_exceedances}, {"events", std::move(events)}};
}

// One schedule per snapshot whose resolved profile opted in. Chains without a
// profile (ambiguous expiries, policy-resolution failures) are not in the ladder.
std::map<SnapshotIdentity, ScheduleRecord>
infer_schedules(const std::vector<InputRow> &rows,
                const std::map<std::uint64_t, std::vector<std::size_t>> &chains,
                const std::map<SnapshotIdentity, std::vector<std::uint64_t>> &snapshot_chains,
                const std::map<std::uint64_t, PreparedChain> &prepared,
                const quote_bench_policy::LoadedPolicy &policy, RunStatistics &stats) {
  std::map<SnapshotIdentity, ScheduleRecord> schedules;
  bool any_cell = false;
  for (const auto &[identity, ids] : snapshot_chains) {
    std::vector<DividendLadderExpiry> ladder;
    auto routing = DividendRouting::Aggregate;
    for (const auto id : ids) {
      const auto &chain = prepared.at(id);
      if (!chain.profile) continue;
      const auto found = policy.dividend_routing.find(chain.profile->name);
      if (found == policy.dividend_routing.end()) continue;
      routing = found->second;
      const auto &indices = chains.at(id);
      std::vector<double> sums;
      sums.reserve(indices.size());
      for (const auto i : indices) {
        sums.push_back(rows[i].dividend_sum);
        if (rows[i].dividend_sum_malformed) ++stats.malformed_ddiv_rows;
        // A present cell, readable or not, shows the column exists.
        any_cell = any_cell || std::isfinite(rows[i].dividend_sum) ||
                   rows[i].dividend_sum_malformed;
      }
      const double sum = median(std::move(sums));
      ladder.push_back({number<std::int32_t>(rows[indices.front()].identity[2], "expiry_days"),
                        chain.market.calendar_years, chain.market.rate, sum,
                        chain.market.dividend_pv});
    }
    if (ladder.empty()) continue;
    ScheduleRecord record{infer_dividend_schedule(valuation_day(identity[2]), ladder), routing};
    const auto &s = record.schedule;
    bump(stats.schedule_status_counts, to_string(s.status));
    if (s.reason != DividendScheduleReason::None) {
      bump(stats.schedule_reason_counts, to_string(s.reason));
    }
    for (const auto &ev : s.view()) bump(stats.event_kind_counts, to_string(ev.kind));
    stats.dividend_schedules.push_back(schedule_json(identity, record));
    schedules.emplace(identity, std::move(record));
  }
  if (!schedules.empty() && !any_cell) {
    throw std::runtime_error("inferred-schedule dividends require the ddiv column");
  }
  return schedules;
}

// The reference forward of the aggregate model the profile would have used,
// for the forward-shift provenance of a lattice-routed chain.
double aggregate_forward(const QuoteSliceMarket &m, QuoteSliceDividendModel model) {
  const double growth = std::exp((m.rate - m.continuous_yield) * m.calendar_years);
  if (model == QuoteSliceDividendModel::AggregatePresentValue) {
    return (m.spot - m.dividend_pv) * growth;
  }
  if (model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
    return m.spot * growth - m.dividend_pv * std::exp(m.rate * m.calendar_years);
  }
  return m.spot * growth;
}

// Route one chain. Lattice when the shared route admits an inferred ex-date in
// the option's life (and, under the -exact mode, every such event is exactly
// dated); otherwise the historical aggregate fit. Two lattice outcomes fall back
// to the aggregate fit, recorded per chain as escrow_fallback with the reason:
// a shared input refusal (Err), and a refused price certification
// (ArbitrageFailure). The second is structural, measured on dev200 ASML: a
// strike-by-strike binomial price is piecewise linear in strike between
// terminal nodes, so pricing each strike at its own fitted vol leaves ~1e-5
// concave kinks that the certification's fixed 1e-9-relative tolerance refuses
// at 301, 601, 1201 and 2401 steps alike. The certification is mandatory and is
// not relaxed here; the next certified model serves the chain instead.
Result<QuoteSliceFit> fit_with_schedule(std::uint64_t chain_id, const InputRow &first,
                                        std::size_t contracts, const PreparedChain &chain,
                                        const ScheduleRecord &record, RunStatistics &stats) {
  const auto start = Clock::now();
  const auto &market = chain.market;
  const auto &schedule = record.schedule;
  const auto expiry_day = number<std::int32_t>(first.identity[2], "expiry_days");
  std::array<CashDividend, kDividendScheduleMaxEvents> events{};
  const auto life =
      dividend_schedule_expiry_events(schedule, expiry_day, market.calendar_years, events);
  std::array<DividendEvent, kDividendScheduleMaxEvents> instants{};
  const auto n_instants = dividend_schedule_instants(schedule, instants);
  // Valuation and expiry on the events' own UTC day grid, so the route's
  // instant window admits exactly the events in `life`.
  const std::int64_t now_ns =
      static_cast<std::int64_t>(schedule.valuation_day) * kDividendScheduleNsPerDay;
  const std::int64_t expiry_ns = static_cast<std::int64_t>(expiry_day) * kDividendScheduleNsPerDay;
  const auto route = discrete_div_route(
      std::span<const DividendEvent>(instants.data(), n_instants), expiry_ns, now_ns,
      static_cast<double>(expiry_ns - now_ns) / kCalendarYearNs, market.rate,
      DiscreteDivPolicy::Lattice);
  std::string reason;
  // The count comparison runs BEFORE applies(): an empty route with events in
  // the life (dividend_schedule_instants' overflow guard returns 0, or a route
  // refusal) is a window mismatch, never "no ex-date".
  if (life.coverage != DividendExpiryCoverage::Covered) {
    reason = std::string("schedule_") + to_string(life.coverage);
  } else if (route.schedule.size() != life.count || route.n_outside_tau_window != 0U) {
    reason = "route_window_mismatch";
  } else if (!route.applies()) {
    reason = "no_ex_date_in_life";
  } else if (record.routing == DividendRouting::InferredScheduleExact && life.ambiguous > 0U) {
    reason = "ambiguous_events_excluded";
  }
  std::string route_name{"escrow"};
  std::string reason_key = reason;
  std::optional<Result<QuoteSliceFit>> fit;
  // The lattice scheme and step count are provenance only where the lattice ran.
  const bool lattice_attempted = reason.empty();
  if (lattice_attempted) {
    QuoteSliceMarket dated = market;
    dated.dividend_pv = 0.0; // the dated cash replaces the aggregate PV, never both
    QuoteSliceConfig config = chain.profile->config;
    config.dividend_model = QuoteSliceDividendModel::DatedCash;
    auto lattice = fit_quote_slice(dated, chain.quotes, config,
                                   std::span<const CashDividend>(events.data(), life.count));
    if (!lattice) {
      route_name = "escrow_fallback";
      reason_key = "lattice_refused";
      reason = "lattice_refused: " + lattice.error().to_string();
    } else if (lattice->fit_status == QuoteSliceFitStatus::ArbitrageFailure) {
      route_name = "escrow_fallback";
      reason_key = "lattice_certification_refused";
      reason = "lattice_certification_refused: " + lattice->fit_error;
    } else {
      route_name = "lattice";
      reason = reason_key = "ex_date_in_life";
      fit.emplace(std::move(lattice));
    }
  }
  if (!fit) fit.emplace(fit_quote_slice(market, chain.quotes, chain.profile->config));
  const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  (route_name == "lattice" ? stats.lattice_chain_microseconds
                           : stats.escrow_chain_microseconds).push_back(us);
  bump(stats.route_chain_counts, route_name);
  bump(stats.route_contract_counts, route_name, contracts);
  bump(stats.route_reason_counts, reason_key);
  const double reference = aggregate_forward(market, chain.profile->config.dividend_model);
  const double used = *fit ? (*fit)->diagnostics.forward : kNaN;
  stats.dividend_routes.push_back(
      {{"chain_id", chain_id}, {"underlier", first.identity[0]}, {"root", first.identity[1]},
       {"expiry", iso_date(expiry_day)}, {"calendar_years", market.calendar_years},
       {"contracts", contracts}, {"route", route_name}, {"reason", reason},
       {"coverage", to_string(life.coverage)}, {"events_in_life", life.count},
       {"ambiguous_events_in_life", life.ambiguous}, {"cash_in_life", life.cash},
       {"route_events", route.schedule.size()}, {"route_pv_calendar", route.pv},
       {"n_outside_tau_window", route.n_outside_tau_window},
       {"aggregate_pv", market.dividend_pv}, {"aggregate_forward", finite_or_null(reference)},
       {"forward", finite_or_null(used)},
       {"forward_shift_bp", finite_or_null(1.0e4 * (used / reference - 1.0))},
       {"fit_status", *fit ? Json(to_string((*fit)->fit_status)) : Json("error")},
       {"lattice_steps", lattice_attempted ? Json(chain.profile->config.discrete_div_steps)
                                           : Json(nullptr)},
       {"lattice_scheme", lattice_attempted
                              ? Json(to_string(chain.profile->config.discrete_div_scheme))
                              : Json(nullptr)},
       {"chain_us", us}});
  return std::move(*fit);
}

std::vector<Prediction> run(const Arguments &args, const std::vector<InputRow> &rows,
                            RunStatistics &stats) {
  const auto dividends = args.dividends == "funding-pv"
      ? QuoteSliceDividendModel::AggregateFundingPresentValue
      : args.dividends == "escrowed-pv" ? QuoteSliceDividendModel::AggregatePresentValue
                                        : QuoteSliceDividendModel::ContinuousYield;
  std::optional<quote_bench_policy::LoadedPolicy> policy;
  std::shared_ptr<const QuoteFitProfile> fixed;
  if (args.curve.empty()) {
    policy.emplace(quote_bench_policy::load(args.policy, dividends));
    stats.policy_definition = policy->definition;
    stats.price_only = policy->price_only;
  } else {
    const auto kind = parse_curve_kind(args.curve);
    if (!kind) throw std::runtime_error(kind.error().to_string());
    QuoteFitProfile profile{"fixed-" + args.curve, ProfileKind::OrdinarySingleName, {}};
    profile.config.curve.kind = *kind;
    profile.config.curve.convex.bound_slope_below = true;
    profile.config.quality = args.quality;
    profile.config.min_quotes = args.min_quotes;
    profile.config.dividend_model = dividends;
    fixed = std::make_shared<const QuoteFitProfile>(std::move(profile));
    stats.policy_definition = {{"schema_version", 1},
        {"profiles", Json::array({quote_bench_policy::profile_json(*fixed)})},
        {"symbol_overrides", Json::array()}, {"regimes", nullptr}};
  }
  std::vector<Prediction> predictions(rows.size());
  std::map<std::uint64_t, std::vector<std::size_t>> chains;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].input_status == "unsupported_deliverable") {
      predictions[i].status = "unsupported_deliverable";
    } else if (rows[i].input_status != "ok") predictions[i].status = "input_rejected";
    else chains[rows[i].chain].push_back(i);
  }
  stats.chains = chains.size();
  std::map<std::uint64_t, PreparedChain> prepared;
  std::map<SnapshotIdentity, std::vector<std::uint64_t>> snapshot_chains;
  const auto start = Clock::now();
  for (const auto &[chain_id, indices] : chains) {
    if (!consistent_chain(rows, indices)) {
      for (const auto i : indices) predictions[i].status = "inconsistent_chain";
      continue;
    }
    auto market = chain_market(rows, indices, stats);
    if (!(std::isfinite(market.spot) && market.spot > 0.0 &&
          std::isfinite(market.calendar_years) && market.calendar_years > 0.0 &&
          std::isfinite(market.variance_years) && market.variance_years > 0.0 &&
          std::isfinite(market.rate) && std::isfinite(market.continuous_yield) &&
          std::isfinite(market.dividend_pv) && market.dividend_pv >= 0.0)) {
      for (const auto i : indices) predictions[i].status = "invalid_market";
      continue;
    }
    PreparedChain chain{market, {}, {}, {}};
    chain.quotes.reserve(indices.size());
    for (const auto i : indices) chain.quotes.push_back(rows[i].quote);
    prepared.emplace(chain_id, std::move(chain));
    const auto &identity = rows[indices.front()].identity;
    snapshot_chains[{identity[0], identity[1], identity[3]}].push_back(chain_id);
  }
  const auto add_count = [](Json &counts, const std::string &key, std::size_t count = 1U) {
    if (counts.is_null()) counts = Json::object();
    counts[key] = counts.value(key, std::size_t{}) + count;
  };
  std::set<std::uint64_t> ambiguous_expiries;
  for (const auto &[identity, ids] : snapshot_chains) {
    // Resolve ambiguity BEFORE fitting/routing: a failed duplicate must not
    // inflate liquidity or let its sibling acquire a different policy.
    std::map<std::string, std::vector<std::uint64_t>> expiries;
    std::map<double, std::vector<std::uint64_t>> maturities;
    for (const auto id : ids) {
      expiries[rows[chains.at(id).front()].identity[2]].push_back(id);
      maturities[prepared.at(id).market.calendar_years].push_back(id);
    }
    const auto reject_duplicates = [&](const auto &groups) {
      for (const auto &[key, group] : groups) {
        static_cast<void>(key);
        if (group.size() > 1U) ambiguous_expiries.insert(group.begin(), group.end());
      }
    };
    reject_duplicates(expiries);
    reject_duplicates(maturities);
    Json decision{{"underlier", identity[0]}, {"root", identity[1]}, {"snapshot_ns", identity[2]},
                  {"eligible_chain_ids", Json::array()}, {"ambiguous_chain_ids", Json::array()}};
    std::vector<QuoteFitExpirySnapshot> snapshots;
    std::vector<std::uint64_t> eligible;
    for (const auto id : ids) {
      if (ambiguous_expiries.contains(id)) {
        for (const auto i : chains.at(id)) predictions[i].status = "ambiguous_expiry";
        decision["ambiguous_chain_ids"].push_back(id);
      } else {
        const auto &chain = prepared.at(id);
        snapshots.push_back({chain.market, chain.quotes});
        eligible.push_back(id);
        decision["eligible_chain_ids"].push_back(id);
      }
    }
    if (eligible.empty()) {
      decision["status"] = "no_unambiguous_expiries";
      stats.policy_decisions.push_back(std::move(decision));
      continue;
    }
    std::shared_ptr<const QuoteFitProfile> profile = fixed;
    std::string source{"fixed-curve"};
    if (policy) {
      const auto resolved = policy->catalog.resolve(identity[0], snapshots);
      if (!resolved) {
        for (const auto id : eligible) {
          for (const auto i : chains.at(id)) predictions[i].status = "policy_resolution_failure";
        }
        decision["status"] = "policy_resolution_failure";
        decision["error"] = resolved.error().to_string();
        stats.policy_decisions.push_back(std::move(decision));
        continue;
      }
      profile = resolved->profile;
      source = to_string(resolved->source);
      decision["features"] = quote_bench_policy::features_json(resolved->features);
    } else {
      decision["features"] = nullptr; // Fixed experiments do not classify.
    }
    decision["status"] = "resolved";
    decision["resolved_profile"] = profile->name;
    decision["policy_source"] = source;
    decision["configured_curve"] = configured_curve(*profile);
    if (!stats.per_profile_counts.contains(profile->name)) {
      stats.per_profile_counts[profile->name] = {
          {"resolved_snapshots", 0U}, {"attempted_chains", 0U}, {"fitted_chains", 0U},
          {"rows", 0U}, {"admitted_quotes", 0U}, {"served_rows", 0U}, {"no_curve_rows", 0U},
          {"configured_curve", configured_curve(*profile)},
          {"actual_curve_rows", Json::object()}, {"row_status_counts", Json::object()}};
    }
    add_count(stats.per_profile_counts[profile->name], "resolved_snapshots");
    for (const auto id : eligible) {
      prepared.at(id).profile = profile;
      prepared.at(id).source = source;
      for (const auto i : chains.at(id)) {
        predictions[i].resolved_profile = profile->name;
        predictions[i].policy_source = source;
        predictions[i].configured_curve = configured_curve(*profile);
      }
    }
    stats.policy_decisions.push_back(std::move(decision));
  }
  // Empty unless a profile opted into an inferred dividend schedule.
  const auto schedules = policy && !policy->dividend_routing.empty()
      ? infer_schedules(rows, chains, snapshot_chains, prepared, *policy, stats)
      : std::map<SnapshotIdentity, ScheduleRecord>{};
  struct PublishedSlice {
    std::uint64_t chain;
    double years;
    std::string expiry;
    AmericanPriceSlice prices;
  };
  // Same underlying, deliverable root and EXACT valuation snapshot. Calendar
  // inequalities concern prices at the same strike, not total variance after
  // independently chosen carry transformations.
  std::map<SnapshotIdentity, std::vector<PublishedSlice>> surfaces;
  std::set<std::uint64_t> fitted_chains;
  std::set<std::uint64_t> static_fallback;
  if (policy && policy->graded) {
    stats.graded = true;
    fit_graded(chains, prepared, snapshot_chains, ambiguous_expiries, policy->surface,
               predictions, stats, fitted_chains, static_fallback);
  }
  // Grade of a static-fallback expiry under graded admission: the pass-A rule
  // without a term prior (own point evidence and unmoved prices => A).
  const auto fallback_grade = [&](const QuoteSliceFit &fit, const QuoteSliceConfig &config) {
    const auto &d = fit.diagnostics;
    const bool evidence = d.quotes_admitted >= config.min_quotes &&
                          d.distinct_strikes >= config.min_quotes;
    if (evidence) return std::string{fit.moved_nodes.empty() ? "A" : "B"};
    return std::string{d.channel.mid_strikes >= policy->surface.grade_b_min_mid_strikes ? "B"
                                                                                       : "C"};
  };
  for (const auto &[chain_id, chain] : prepared) {
    // Graded admission fitted and published every snapshot above except the
    // static-fallback ones, which take this path and the static calendar rule.
    if (stats.graded && !static_fallback.contains(chain_id)) continue;
    if (!chain.profile) continue;
    const auto &indices = chains.at(chain_id);
    const auto chain_start = Clock::now();
    add_count(stats.per_profile_counts[chain.profile->name], "attempted_chains");
    const auto &market = chain.market;
    if (market.dividend_pv > 0.0) ++stats.dividend_chains;
    const auto &first = rows[indices.front()];
    const auto schedule = schedules.find({first.identity[0], first.identity[1], first.identity[3]});
    // The graded channel repair is a rung of fit_quote_surface only: the
    // static(-fallback) path keeps its historical certificate unrepaired.
    QuoteSliceConfig static_config = chain.profile->config;
    static_config.repair.mode = QuoteSliceRepairMode::Off;
    const auto fit = schedule == schedules.end()
        ? fit_quote_slice(market, chain.quotes, static_config)
        : fit_with_schedule(chain_id, first, indices.size(), chain, schedule->second, stats);
    if (!fit) {
      ++stats.fit_failures[fit.error().to_string()];
      for (const auto i : indices) predictions[i].status = "invalid_market";
    } else {
      if (fit->carry_resolution.mode != QuoteSliceCarryMode::Supplied) {
        const auto &carry = fit->carry_resolution;
        stats.carry_resolutions.push_back(
            carry_resolution_json(carry, chain_id, rows[indices.front()], market));
        // An estimator refusal is named by its status; every other fallback
        // also names its typed cause (a post-estimate fallback has status ok).
        std::string key = std::string(carry.applied ? "applied:" : "fallback:") +
                          to_string(carry.status);
        if (!carry.applied && carry.fallback != QuoteSliceCarryFallback::EstimateRefused) {
          key += std::string(":") + to_string(carry.fallback);
        }
        stats.carry_status_counts[key] = stats.carry_status_counts.value(key, std::size_t{}) + 1U;
      }
      if (fit->fit_status == QuoteSliceFitStatus::Success) {
        ++stats.fitted;
        fitted_chains.insert(chain_id);
        const auto &identity = rows[indices.front()].identity;
        surfaces[{identity[0], identity[1], identity[3]}].push_back(
            {chain_id, market.calendar_years, identity[2], *fit->prices});
      }
      else ++stats.fit_failures[fit->fit_error];
      stats.admitted += fit->diagnostics.quotes_admitted;
      for (std::size_t j = 0; j < indices.size(); ++j) {
        const auto &result = fit->rows[j];
        auto &prediction = predictions[indices[j]];
        if (fit->curve) prediction.actual_curve = to_string(fit->curve->kind());
        prediction.quote_status = to_string(result.quote_status);
        prediction.admitted = result.admitted;
        if (result.evaluation_status == QuoteSliceEvaluationStatus::Success) {
          prediction.price = result.fitted_price;
          prediction.vol = result.fitted_iv;
          prediction.status = "ok";
          if (stats.graded) prediction.grade = fallback_grade(*fit, chain.profile->config);
          ++stats.served;
        } else if (fit->fit_status != QuoteSliceFitStatus::Success) {
          prediction.status = to_string(fit->fit_status);
        } else if (result.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) {
          if (publish_price_only(result, stats.price_only, prediction)) {
            if (stats.graded) prediction.grade = fallback_grade(*fit, chain.profile->config);
            ++stats.price_only_rows;
          }
        } else {
          prediction.status = to_string(result.evaluation_status);
        }
      }
    }
    stats.chain_microseconds.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - chain_start).count());
  }
  std::set<std::uint64_t> calendar_failures;
  for (auto &[identity, slices] : surfaces) { // graded admission: static-fallback snapshots only
    static_cast<void>(identity);
    std::sort(slices.begin(), slices.end(),
              [](const auto &a, const auto &b) { return a.years < b.years; });
    // Every pair matters when domains are nested differently; adjacent expiry
    // checks alone can miss a crossing outside the intervening slice's domain.
    for (std::size_t i = 0; i < slices.size(); ++i) {
      for (std::size_t j = i + 1; j < slices.size(); ++j) {
        ++stats.calendar_pairs;
        if (slices[i].expiry == slices[j].expiry || slices[i].years == slices[j].years) {
          calendar_failures.insert(slices[i].chain);
          calendar_failures.insert(slices[j].chain);
          ambiguous_expiries.insert(slices[i].chain);
          ambiguous_expiries.insert(slices[j].chain);
          continue; // Duplicate surfaces are ambiguous, even with identical marks.
        }
        const auto accepted = check_american_calendar(slices[i].prices, slices[j].prices);
        if (!accepted) {
          calendar_failures.insert(slices[i].chain);
          calendar_failures.insert(slices[j].chain);
        }
      }
    }
  }
  stats.ambiguous_expiry_chains = ambiguous_expiries.size();
  stats.calendar_rejected_chains = 0U;
  for (const auto id : calendar_failures) {
    if (!ambiguous_expiries.contains(id)) ++stats.calendar_rejected_chains;
  }
  for (const auto chain : calendar_failures) {
    --stats.fitted;
    fitted_chains.erase(chain);
    for (const auto index : chains.at(chain)) {
      if (predictions[index].status == "ok") --stats.served;
      if (predictions[index].status == "ok_price_only") --stats.price_only_rows;
      predictions[index].price = kNaN;
      predictions[index].vol = kNaN;
      predictions[index].vol_status = "not_evaluated"; // the whole expiry is refused
      predictions[index].grade.clear();
      predictions[index].status = ambiguous_expiries.contains(chain)
          ? "ambiguous_expiry" : "calendar_arbitrage";
    }
  }
  if (stats.calendar_rejected_chains > 0U) {
    stats.fit_failures["American calendar price crossing"] = stats.calendar_rejected_chains;
  }
  if (!ambiguous_expiries.empty()) {
    stats.fit_failures["Ambiguous duplicate expiry"] = ambiguous_expiries.size();
  }
  for (const auto id : fitted_chains) {
    add_count(stats.per_profile_counts[prepared.at(id).profile->name], "fitted_chains");
  }
  for (const auto &prediction : predictions) {
    if (prediction.resolved_profile.empty()) continue;
    auto &counts = stats.per_profile_counts[prediction.resolved_profile];
    add_count(counts, "rows");
    add_count(counts, "admitted_quotes", prediction.admitted ? 1U : 0U);
    add_count(counts, "served_rows", prediction.status == "ok" ? 1U : 0U);
    if (stats.price_only) {
      add_count(counts, "price_only_rows", prediction.status == "ok_price_only" ? 1U : 0U);
    }
    if (prediction.actual_curve.empty()) add_count(counts, "no_curve_rows");
    else add_count(counts["actual_curve_rows"], prediction.actual_curve);
    add_count(counts["row_status_counts"], prediction.status);
  }
  stats.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return predictions;
}

void write_predictions(const std::filesystem::path &path, const std::vector<InputRow> &rows,
                       const std::vector<Prediction> &predictions, bool graded = false) {
  std::ofstream stream{path};
  if (!stream) throw std::runtime_error("Cannot open prediction output");
  stream << "row_id,predicted_price,predicted_vol,status,chain_id,quote_status,admitted,"
            "resolved_profile,policy_source,configured_curve,actual_curve,vol_status,model"
         << (graded ? ",grade\n" : "\n");
  stream << std::setprecision(17);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &prediction = predictions[i];
    stream << rows[i].id << ',';
    if (std::isfinite(prediction.price)) stream << prediction.price;
    stream << ',';
    if (std::isfinite(prediction.vol)) stream << prediction.vol;
    stream << ',' << prediction.status << ',' << rows[i].chain << ','
           << prediction.quote_status << ',' << (prediction.admitted ? 1 : 0) << ','
           << prediction.resolved_profile << ',' << prediction.policy_source << ','
           << prediction.configured_curve << ','
           << prediction.actual_curve << ','
           << (prediction.model == "quote-slice" && std::isfinite(prediction.vol)
                   ? "ok" : prediction.vol_status) << ',' << prediction.model;
    if (graded) stream << ',' << prediction.grade;
    stream << '\n';
  }
  stream.close();
  if (!stream) throw std::runtime_error("Prediction output write failed");
}

double quantile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double location = p * static_cast<double>(values.size() - 1U);
  const auto lo = static_cast<std::size_t>(location);
  const auto hi = std::min(lo + 1U, values.size() - 1U);
  return values[lo] + (location - static_cast<double>(lo)) * (values[hi] - values[lo]);
}

void write_report(std::ostream &stream, const Arguments &args, std::size_t rows,
                  const RunStatistics &stats) {
  Json report{
      {"schema_version", 2}, {"model", "quote-slice"}, {"rows", rows}, {"chains", stats.chains},
      {"fitted_chains", stats.fitted}, {"served_rows", stats.served},
      {"admitted_quotes", stats.admitted}, {"cash_dividend_chains", stats.dividend_chains},
      {"calendar_pairs_checked", stats.calendar_pairs},
      {"calendar_rejected_chains", stats.calendar_rejected_chains},
      {"ambiguous_expiry_chains", stats.ambiguous_expiry_chains},
      {"policy_mode", !args.curve.empty() ? "fixed-curve" :
          args.policy == "auto" ? "auto" : "file"},
      {"curve", args.curve.empty() ? Json(nullptr) : Json(args.curve)},
      {"policy_definition_format", "expanded-runtime-configuration-v1"},
      {"policy_definition", stats.policy_definition}, {"policy_decisions", stats.policy_decisions},
      {"per_profile_counts", stats.per_profile_counts}, {"dividend_model", args.dividends},
      {"dividend_convention_assumption", args.dividends == "funding-pv"
          ? "funding-discounted cash PV; continuous-yield approximation without ex-dates"
          : args.dividends == "escrowed-pv"
              ? "escrowed cash PV; continuous-yield approximation without ex-dates"
              : "continuous yield only; nonzero cash PV is refused"},
      {"chain_market_aggregation", "median"}, {"contract_carry", "per-strike supplied yield"},
      {"serving_representation", "bounded convex American price interpolation"},
      {"maximum_spot_range_bps", stats.max_spot_range_bps},
      {"fit_and_reprice_seconds", stats.elapsed_seconds},
      {"served_rows_per_second", stats.elapsed_seconds > 0.0 ?
          static_cast<double>(stats.served) / stats.elapsed_seconds : 0.0},
      {"chain_latency_us_p50", quantile(stats.chain_microseconds, 0.50)},
      {"chain_latency_us_p95", quantile(stats.chain_microseconds, 0.95)},
      {"chain_latency_us_p99", quantile(stats.chain_microseconds, 0.99)},
      {"timing_scope", "serial preparation, raw policy routing, fit and repricing; "
          "excludes CSV IO and policy parsing; chain latency excludes preparation/routing"},
      {"performance_claim", "diagnostic; requires quiet repeated release runs"},
      {"fit_failure_counts", stats.fit_failures},
      {"mandatory_admission", "quote validity, fitted American price bounds/strike shape/"
          "cross-side and calendar checks remain enabled"}};
  if (!stats.carry_resolutions.empty()) {
    report["contract_carry"] = "per-expiry implied forward where the estimator accepts; "
                               "otherwise per-strike supplied yield";
    report["carry_status_counts"] = stats.carry_status_counts;
    report["carry_resolutions"] = stats.carry_resolutions;
  }
  if (stats.graded) {
    report["admission"] = "graded";
    report["graded_stats"] = stats.graded_stats;
    report["graded_semantics"] = "A: own admitted point evidence >= the profile minimum, served "
        "by the profile's own method (point fit, or the term-prior-free channel fit of a "
        "knot-spline/channel.family profile, under the flat default shape) with no term prior and "
        "no served price moved by a repair; B: a pass-A slice whose prices a bounded-move repair "
        "moved, or a channel refit (under the leave-one-out term prior when another expiry "
        "supplies a node, else the flat default shape) with >= grade_b_min_mid_strikes mid "
        "strikes; C: other channel refits; calendar crossings refuse only the lower-confidence "
        "slice after a bounded-move repair and one retry; snapshots above the expiry cap use the "
        "static path (static_fallback; its B/C are term-prior-free channel fits)";
    if (stats.graded_channel_repair) {
      report["graded_semantics"] = report["graded_semantics"].get<std::string>() +
          "; repair channel: a failed certificate is repaired by lowering the offending nodes "
          "onto their convex envelope, else by a least-squares projection onto the convex, "
          "slope- and bound-constrained prices inside each node's own bid/ask channel widened "
          "to its fitted price (calendar: also inside the neighbours' prices), accepted only if "
          "no moved node ends farther outside its channel, at most repair_max_nodes strikes "
          "move by more than repair_immaterial_price, and unquoted node-sides move at most "
          "repair_max_vol in IV (quoted sides are bounded only by their channel), then "
          "re-certified unchanged; it runs where a check already failed (channel refits, the "
          "pass-A fit of a still-refused expiry, then calendar crossings that the bounded-move "
          "repair and the retry cannot fix); every channel-repaired slice is at best B";
    }
    if (stats.graded_dominance) {
      report["graded_semantics"] = report["graded_semantics"].get<std::string>() +
          "; channel.point_max_outside_fraction / point_min_outside_quotes (per profile), "
          "after every other rung: a final point fit (served, or refused for a calendar "
          "crossing) outside bid/ask on more than that share and at least that many of its "
          "two-sided quotes is replaced by a channel refit only when the final served set is "
          "calendar-consistent with it; otherwise it is kept (point_fallback, A becomes B); "
          "after an adoption, calendar refusals are re-checked against the final served set "
          "(calendar_readmitted, B/C); the gate never refuses an expiry";
    }
  }
  if (stats.price_only) {
    report["price_only_rows"] = stats.price_only_rows;
    report["price_only_semantics"] = "policy price_only: a served row whose certified price "
        "has no identifiable American volatility under its own carry (a repaired node) is "
        "status ok_price_only with that price, a blank vol and vol_status unidentifiable; it is "
        "never counted in served_rows (price-served = ok | ok_price_only, vol-served = ok)";
  }
  if (!stats.dividend_schedules.empty()) {
    report["dividend_schedule"] = {
        {"policy_key", "dividends"},
        {"inference", "infer_dividend_schedule (api/marketdata/dividend_schedule.hpp): "
            "per-expiry ddiv increments dated by the ddiv_pv identity "
            "PV = sum D_i exp(-rate_T days_i / 365)"},
        {"split_rule", "an increment holding n >= 2 unit dividends (n = round(increment / "
            "median exactly dated amount), capped by its window at half the cadence) is split "
            "into n equal amounts at equally spaced times (spacing = cadence, else window/n, "
            "shrunk to fit), first time solved so the n events reproduce the increment's PV "
            "exactly; flagged sum_split, never silent. An increment holding more units than "
            "its window holds apart (room for two or more) is kept as one event at its "
            "PV-exact time (clamped to the window), flagged non_integer_day"},
        {"day_numbering", "valuation date = UTC day of snapshot_ns; ex-dates in whole days"},
        {"clock_mapping", "tau = years_calendar * days / (expiry_day - valuation_day); an "
            "ex-date on the expiry date lands at tau = T"},
        {"route", "discrete_div_route(Lattice) admits an ex-date in (valuation, expiry] -> "
            "QuoteSliceDividendModel::DatedCash lattice for IV inversion and repricing "
            "(dividend_pv = 0, per-strike sdiv kept as residual yield): the profile's "
            "dividend_lattice scheme at dividend_lattice_steps (301 unless set; per route: "
            "lattice_scheme, lattice_steps, null where the lattice did not run). spliced "
            "(default) is the V&N spliced CRR on spot; spliced-bbs adds a Broadie-Detemple "
            "analytic final step, spliced-bbsr that plus Richardson in the step count; "
            "escrowed-bbs (CRR + BBS) and escrowed-lr (Leisen-Reimer) are the ESCROWED-dividend "
            "model, whose implied and reported vols are the escrowed stock's (spot net of the "
            "discounted cash in the option's life), not spot vols. Otherwise the --dividends "
            "aggregate model unchanged"},
        {"route_granularity", "per chain (one expiry); every contract of a chain shares its "
            "route, joinable on chain_id"},
        {"schedule_status_counts", stats.schedule_status_counts},
        {"schedule_reason_counts", stats.schedule_reason_counts},
        {"malformed_ddiv_rows", stats.malformed_ddiv_rows},
        {"event_kind_counts", stats.event_kind_counts},
        {"route_chain_counts", stats.route_chain_counts},
        {"route_contract_counts", stats.route_contract_counts},
        {"route_reason_counts", stats.route_reason_counts},
        {"lattice_chains_seconds", std::accumulate(stats.lattice_chain_microseconds.begin(),
            stats.lattice_chain_microseconds.end(), 0.0) * 1.0e-6},
        {"lattice_chain_latency_us_p50", quantile(stats.lattice_chain_microseconds, 0.50)},
        {"lattice_chain_latency_us_p95", quantile(stats.lattice_chain_microseconds, 0.95)},
        {"escrow_chain_latency_us_p50", quantile(stats.escrow_chain_microseconds, 0.50)},
        {"escrow_chain_latency_us_p95", quantile(stats.escrow_chain_microseconds, 0.95)},
        {"timing_scope", "diagnostic wall time of the routed fit and repricing per chain, "
            "shared host"},
        {"schedules", stats.dividend_schedules},
        {"routes", stats.dividend_routes}};
  }
#ifdef __VERSION__
  report["compiler"] = __VERSION__;
#elif defined(_MSC_VER)
  report["compiler"] = "MSVC " + std::to_string(_MSC_VER);
#else
  report["compiler"] = "unknown";
#endif
#ifdef NDEBUG
  report["build_type"] = "release";
#else
  report["build_type"] = "debug";
#endif
  stream << report.dump(2) << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto args = arguments(argc, argv);
    if (args.help_requested) return 0;
    Json provenance = Json::object();
    if (!args.progress.empty()) {
      provenance = {{"input", std::filesystem::weakly_canonical(args.input).string()},
          {"input_sha256", quote_bench::file_sha256(args.input)},
          {"policy_selection", args.common_policy},
          {"policy_sha256", args.common_policy == "auto" ? Json(nullptr)
              : Json(quote_bench::file_sha256(args.common_policy))},
          {"executable_sha256", std::filesystem::exists(argv[0])
              ? Json(quote_bench::file_sha256(argv[0])) : Json(nullptr)},
          {"executable_hash_scope", "argv[0] if directly resolvable; wrapper start receipt "
              "always binds the absolute invoked executable"}};
    }
    const auto rows = read_rows(args);
    if (!args.progress.empty() &&
        provenance["input_sha256"] != quote_bench::file_sha256(args.input)) {
      throw std::runtime_error("Input changed while reading progress-bound cohort");
    }
    if (args.model == "common-markov") {
      auto result = quote_bench::run_common_markov(rows, args.common_policy, args.dividends,
                                                  args.progress, provenance);
      result.report["input"] = std::filesystem::weakly_canonical(args.input).string();
      write_predictions(args.output, rows, result.predictions);
      std::cout << result.report.dump(2) << '\n';
      if (!args.report.empty()) {
        std::ofstream report{args.report};
        if (!report) throw std::runtime_error("Cannot open timing report");
        report << result.report.dump(2) << '\n';
        report.close();
        if (!report) throw std::runtime_error("Timing report write failed");
      }
      return 0;
    }
    RunStatistics stats;
    const auto predictions = run(args, rows, stats);
    write_predictions(args.output, rows, predictions, stats.graded);
    write_report(std::cout, args, rows.size(), stats);
    if (!args.report.empty()) {
      std::ofstream report{args.report};
      if (!report) throw std::runtime_error("Cannot open timing report");
      write_report(report, args, rows.size(), stats);
      report.close();
      if (!report) throw std::runtime_error("Timing report write failed");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "quote-bench: " << error.what() << '\n';
    return 1;
  }
}
