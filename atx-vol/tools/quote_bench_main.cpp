// Offline orchestration only: model input is a closed, label-free CSV schema.
// Numerical work lives in reusable fitting/pricing APIs, independent of data files.
#include "atx/vol/api/fitting/quote_slice.hpp"
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
};

Json finite_or_null(double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); }

Json carry_resolution_json(const QuoteSliceCarryResolution &r, std::uint64_t chain,
                           const InputRow &row, const QuoteSliceMarket &market) {
  return {{"chain_id", chain}, {"underlier", row.identity[0]}, {"root", row.identity[1]},
          {"expiry_days", row.identity[2]}, {"calendar_years", market.calendar_years},
          {"variance_years", market.variance_years}, {"mode", to_string(r.mode)},
          {"applied", r.applied}, {"status", to_string(r.status)}, {"reason", r.reason},
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
  struct PreparedChain {
    QuoteSliceMarket market;
    std::vector<QuoteSliceQuote> quotes;
    std::shared_ptr<const QuoteFitProfile> profile;
    std::string source;
  };
  std::map<std::uint64_t, PreparedChain> prepared;
  using SnapshotIdentity = std::array<std::string, 3>;
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
    decision["configured_curve"] = to_string(profile->config.curve.kind);
    if (!stats.per_profile_counts.contains(profile->name)) {
      stats.per_profile_counts[profile->name] = {
          {"resolved_snapshots", 0U}, {"attempted_chains", 0U}, {"fitted_chains", 0U},
          {"rows", 0U}, {"admitted_quotes", 0U}, {"served_rows", 0U}, {"no_curve_rows", 0U},
          {"configured_curve", to_string(profile->config.curve.kind)},
          {"actual_curve_rows", Json::object()}, {"row_status_counts", Json::object()}};
    }
    add_count(stats.per_profile_counts[profile->name], "resolved_snapshots");
    for (const auto id : eligible) {
      prepared.at(id).profile = profile;
      prepared.at(id).source = source;
      for (const auto i : chains.at(id)) {
        predictions[i].resolved_profile = profile->name;
        predictions[i].policy_source = source;
        predictions[i].configured_curve = to_string(profile->config.curve.kind);
      }
    }
    stats.policy_decisions.push_back(std::move(decision));
  }
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
  for (const auto &[chain_id, chain] : prepared) {
    if (!chain.profile) continue;
    const auto &indices = chains.at(chain_id);
    const auto chain_start = Clock::now();
    add_count(stats.per_profile_counts[chain.profile->name], "attempted_chains");
    const auto &market = chain.market;
    if (market.dividend_pv > 0.0) ++stats.dividend_chains;
    const auto fit = fit_quote_slice(market, chain.quotes, chain.profile->config);
    if (!fit) {
      ++stats.fit_failures[fit.error().to_string()];
      for (const auto i : indices) predictions[i].status = "invalid_market";
    } else {
      if (fit->carry_resolution.mode != QuoteSliceCarryMode::Supplied) {
        const auto &carry = fit->carry_resolution;
        stats.carry_resolutions.push_back(
            carry_resolution_json(carry, chain_id, rows[indices.front()], market));
        const std::string key = std::string(carry.applied ? "applied:" : "fallback:") +
                                to_string(carry.status);
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
          ++stats.served;
        } else if (fit->fit_status != QuoteSliceFitStatus::Success) {
          prediction.status = to_string(fit->fit_status);
        } else {
          prediction.status = to_string(result.evaluation_status);
        }
      }
    }
    stats.chain_microseconds.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - chain_start).count());
  }
  std::set<std::uint64_t> calendar_failures;
  for (auto &[identity, slices] : surfaces) {
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
      predictions[index].price = kNaN;
      predictions[index].vol = kNaN;
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
    if (prediction.actual_curve.empty()) add_count(counts, "no_curve_rows");
    else add_count(counts["actual_curve_rows"], prediction.actual_curve);
    add_count(counts["row_status_counts"], prediction.status);
  }
  stats.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return predictions;
}

void write_predictions(const std::filesystem::path &path, const std::vector<InputRow> &rows,
                       const std::vector<Prediction> &predictions) {
  std::ofstream stream{path};
  if (!stream) throw std::runtime_error("Cannot open prediction output");
  stream << "row_id,predicted_price,predicted_vol,status,chain_id,quote_status,admitted,"
            "resolved_profile,policy_source,configured_curve,actual_curve,vol_status,model\n";
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
                   ? "ok" : prediction.vol_status) << ',' << prediction.model << '\n';
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
    write_predictions(args.output, rows, predictions);
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
