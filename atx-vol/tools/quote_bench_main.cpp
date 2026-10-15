// Offline orchestration only: model input is a closed, label-free CSV schema.
// Numerical work lives in quote_slice, independently of any data vendor or file.
#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
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
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Arguments {
  std::filesystem::path input;
  std::filesystem::path output;
  std::filesystem::path report;
  std::string curve{"essvi"};
  std::string dividends{"aggregate-pv"};
  std::size_t max_rows{2000000U};
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

Arguments arguments(int argc, char **argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key{argv[i]};
    if (key == "--help") {
      std::cout << "atx-vol-quote-bench --input quotes.csv --output predictions.csv "
                   "[--report timing.json] [--curve essvi|svi|convex-dense|spline-vol] "
                   "[--dividends aggregate-pv|none] [--max-rows 2000000]\n";
      return args;
    }
    if (i + 1 == argc) throw std::runtime_error("Missing value for " + std::string(key));
    const std::string value{argv[++i]};
    if (key == "--input") args.input = value;
    else if (key == "--output") args.output = value;
    else if (key == "--report") args.report = value;
    else if (key == "--curve") args.curve = value;
    else if (key == "--dividends") args.dividends = value;
    else if (key == "--max-rows") args.max_rows = number<std::size_t>(value, key);
    else throw std::runtime_error("Unknown argument: " + std::string(key));
  }
  if (args.input.empty() || args.output.empty() || args.max_rows == 0U) {
    throw std::runtime_error("--input, --output and a positive --max-rows are required");
  }
  if (args.dividends != "aggregate-pv" && args.dividends != "none") {
    throw std::runtime_error("--dividends must be aggregate-pv or none");
  }
  if (std::filesystem::weakly_canonical(args.input) ==
      std::filesystem::weakly_canonical(args.output)) {
    throw std::runtime_error("Input and output must differ");
  }
  if (!args.report.empty() &&
      (std::filesystem::weakly_canonical(args.report) ==
           std::filesystem::weakly_canonical(args.input) ||
       std::filesystem::weakly_canonical(args.report) ==
           std::filesystem::weakly_canonical(args.output))) {
    throw std::runtime_error("Timing report must differ from input and predictions");
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
                         "input_status", "exercise"}) {
    if (!header.contains(key)) throw std::runtime_error("Missing column: " + std::string(key));
  }
  return header;
}

struct InputRow {
  std::uint64_t id{};
  std::uint64_t chain{};
  QuoteSliceMarket market{};
  QuoteSliceQuote quote{};
  std::string input_status;
};

InputRow convert_row(const Header &header, const std::vector<std::string> &fields) {
  if (fields.size() != header.size()) throw std::runtime_error("CSV field count mismatch");
  const auto get = [&](std::string_view key) -> std::string_view {
    return fields[header.find(key)->second];
  };
  InputRow row;
  row.id = number<std::uint64_t>(get("row_id"), "row_id");
  row.chain = number<std::uint64_t>(get("chain_id"), "chain_id");
  row.input_status = get("input_status");
  if (row.input_status != "ok") return row;
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

struct Prediction {
  double price{kNaN};
  double vol{kNaN};
  std::string status{"not_evaluated"};
  std::string quote_status{"not_attempted"};
  bool admitted{false};
};

struct RunStatistics {
  std::size_t chains{};
  std::size_t fitted{};
  std::size_t served{};
  std::size_t admitted{};
  std::size_t dividend_chains{};
  double max_spot_range_bps{};
  double elapsed_seconds{};
  std::vector<double> chain_microseconds;
};

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

std::vector<Prediction> run(const Arguments &args, const std::vector<InputRow> &rows,
                            RunStatistics &stats) {
  QuoteSliceConfig config;
  const auto kind = parse_curve_kind(args.curve);
  if (!kind) throw std::runtime_error(kind.error().to_string());
  config.curve.kind = kind.value();
  config.dividend_model = args.dividends == "aggregate-pv"
      ? QuoteSliceDividendModel::AggregatePresentValue
      : QuoteSliceDividendModel::ContinuousYield;
  std::vector<Prediction> predictions(rows.size());
  std::map<std::uint64_t, std::vector<std::size_t>> chains;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].input_status != "ok") predictions[i].status = "input_rejected";
    else chains[rows[i].chain].push_back(i);
  }
  stats.chains = chains.size();
  const auto start = Clock::now();
  for (const auto &[chain_id, indices] : chains) {
    static_cast<void>(chain_id);
    const auto chain_start = Clock::now();
    const auto market = chain_market(rows, indices, stats);
    if (market.dividend_pv > 0.0) ++stats.dividend_chains;
    std::vector<QuoteSliceQuote> quotes;
    quotes.reserve(indices.size());
    for (const auto i : indices) quotes.push_back(rows[i].quote);
    const auto fit = fit_quote_slice(market, quotes, config);
    if (!fit) {
      for (const auto i : indices) predictions[i].status = "invalid_market";
    } else {
      if (fit->fit_status == QuoteSliceFitStatus::Success) ++stats.fitted;
      stats.admitted += fit->diagnostics.quotes_admitted;
      for (std::size_t j = 0; j < indices.size(); ++j) {
        const auto &result = fit->rows[j];
        auto &prediction = predictions[indices[j]];
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
  stats.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return predictions;
}

void write_predictions(const std::filesystem::path &path, const std::vector<InputRow> &rows,
                       const std::vector<Prediction> &predictions) {
  std::ofstream stream{path};
  if (!stream) throw std::runtime_error("Cannot open prediction output");
  stream << "row_id,predicted_price,predicted_vol,status,chain_id,quote_status,admitted\n";
  stream << std::setprecision(17);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &prediction = predictions[i];
    stream << rows[i].id << ',';
    if (std::isfinite(prediction.price)) stream << prediction.price;
    stream << ',';
    if (std::isfinite(prediction.vol)) stream << prediction.vol;
    stream << ',' << prediction.status << ',' << rows[i].chain << ','
           << prediction.quote_status << ',' << (prediction.admitted ? 1 : 0) << '\n';
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
  stream << std::setprecision(17)
         << "{\n  \"schema_version\": 1,\n  \"rows\": " << rows
         << ",\n  \"chains\": " << stats.chains
         << ",\n  \"fitted_chains\": " << stats.fitted
         << ",\n  \"served_rows\": " << stats.served
         << ",\n  \"admitted_quotes\": " << stats.admitted
         << ",\n  \"cash_dividend_chains\": " << stats.dividend_chains
         << ",\n  \"curve\": \"" << args.curve << '"'
         << ",\n  \"dividend_model\": \"" << args.dividends << '"'
         << ",\n  \"chain_market_aggregation\": \"median\""
         << ",\n  \"maximum_spot_range_bps\": " << stats.max_spot_range_bps
         << ",\n  \"fit_and_reprice_seconds\": " << stats.elapsed_seconds
         << ",\n  \"served_rows_per_second\": "
         << (stats.elapsed_seconds > 0.0
                 ? static_cast<double>(stats.served) / stats.elapsed_seconds : 0.0)
         << ",\n  \"chain_latency_us_p50\": " << quantile(stats.chain_microseconds, 0.50)
         << ",\n  \"chain_latency_us_p95\": " << quantile(stats.chain_microseconds, 0.95)
         << ",\n  \"chain_latency_us_p99\": " << quantile(stats.chain_microseconds, 0.99)
         << ",\n  \"timing_scope\": \"serial cold fit plus all contract reprices; excludes CSV IO\""
         << ",\n  \"performance_claim\": \"diagnostic; requires quiet repeated release runs\""
#ifdef __VERSION__
         << ",\n  \"compiler\": \"" << __VERSION__ << '"'
#elif defined(_MSC_VER)
         << ",\n  \"compiler\": \"MSVC " << _MSC_VER << '"'
#else
         << ",\n  \"compiler\": \"unknown\""
#endif
#ifdef NDEBUG
         << ",\n  \"build_type\": \"release\""
#else
         << ",\n  \"build_type\": \"debug\""
#endif
         << "\n}\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto args = arguments(argc, argv);
    if (args.input.empty()) return 0; // --help
    const auto rows = read_rows(args);
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
