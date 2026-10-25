// atx-vol-fit-risk-probe — argv and file I/O around tools/fit_risk_probe.cpp (lane F,
// wave 3). CLI and report schema: atx-vol/docs/universe-fit-bench.md.
//
//   atx-vol-fit-risk-probe --quotes quotes.csv --predictions p.csv --out probe.json
//       [--rows probe-rows.csv] [--kernel family|cold|both] [--threads N]
//       [--basis basis.csv] [--carry-tolerance X] [--dividends funding-pv|escrowed-pv|none]
//       [--labels labels.csv --manifest manifest.json]
//
// Exit codes: 0 ok, 2 usage / input error, 3 label governance refusal.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fit_risk_probe.hpp"

namespace {

namespace fp = atx::vol::fit_probe;

struct Args {
  std::string quotes;
  std::string predictions;
  std::string out;
  std::string rows;
  std::string basis;
  std::string labels;
  std::string manifest;
  fp::ProbeOptions options{};
};

[[nodiscard]] std::optional<std::string> slurp(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[nodiscard]] bool spill(const std::string &path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

int usage(const char *why) {
  std::fprintf(stderr,
               "atx-vol-fit-risk-probe: %s\nusage: atx-vol-fit-risk-probe --quotes q.csv "
               "--predictions p.csv --out probe.json [--rows rows.csv] "
               "[--kernel family|cold|both] [--threads N] [--basis basis.csv] "
               "[--carry-tolerance X] [--dividends funding-pv|escrowed-pv|none] "
               "[--labels labels.csv --manifest manifest.json]\n",
               why);
  return 2;
}

// Returns an error text, or empty on success.
[[nodiscard]] std::string parse_args(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) return "missing value for " + std::string(flag);
    const std::string value = argv[++i];
    if (flag == "--quotes") {
      a.quotes = value;
    } else if (flag == "--predictions") {
      a.predictions = value;
    } else if (flag == "--out") {
      a.out = value;
    } else if (flag == "--rows") {
      a.rows = value;
    } else if (flag == "--basis") {
      a.basis = value;
    } else if (flag == "--labels") {
      a.labels = value;
    } else if (flag == "--manifest") {
      a.manifest = value;
    } else if (flag == "--kernel") {
      if (value == "family") {
        a.options.kernel = fp::ProbeKernel::Family;
      } else if (value == "cold") {
        a.options.kernel = fp::ProbeKernel::Cold;
      } else if (value == "both") {
        a.options.kernel = fp::ProbeKernel::Both;
      } else {
        return "unknown --kernel " + value;
      }
    } else if (flag == "--threads") {
      const unsigned long n = std::stoul(value);
      if (n > 256UL) return "--threads must be <= 256";
      a.options.threads = static_cast<unsigned>(n);
    } else if (flag == "--carry-tolerance") {
      a.options.carry_price_tolerance = std::stod(value);
      if (!(a.options.carry_price_tolerance >= 0.0)) return "--carry-tolerance must be >= 0";
    } else if (flag == "--dividends") {
      if (value == "funding-pv") {
        a.options.dividend_model = atx::vol::QuoteSliceDividendModel::AggregateFundingPresentValue;
      } else if (value == "escrowed-pv") {
        a.options.dividend_model = atx::vol::QuoteSliceDividendModel::AggregatePresentValue;
      } else if (value == "none") {
        a.options.dividend_model = atx::vol::QuoteSliceDividendModel::ContinuousYield;
      } else {
        return "unknown --dividends " + value;
      }
    } else {
      return "unknown flag " + std::string(flag);
    }
  }
  if (a.quotes.empty() || a.predictions.empty() || a.out.empty()) {
    return "--quotes, --predictions and --out are required";
  }
  if (!a.labels.empty() && a.manifest.empty()) {
    return "--labels requires --manifest (label governance)";
  }
  return {};
}

int run(int argc, char **argv) {
  Args a;
  if (const std::string why = parse_args(argc, argv, a); !why.empty()) return usage(why.c_str());
  const auto started = std::chrono::steady_clock::now();
  std::vector<fp::LabelRow> labels;
  if (!a.labels.empty()) {
    const auto manifest = slurp(a.manifest);
    if (!manifest) return usage("cannot read --manifest");
    const atx::vol::Status allowed = fp::labels_allowed(*manifest);
    if (!allowed) {
      std::fprintf(stderr, "atx-vol-fit-risk-probe: labels refused: %s\n",
                   allowed.error().to_string().c_str());
      return 3;
    }
    a.options.labels_allowed = true;
    const auto text = slurp(a.labels);
    if (!text) return usage("cannot read --labels");
    auto parsed = fp::parse_labels_csv(*text);
    if (!parsed) return usage(parsed.error().to_string().c_str());
    labels = std::move(*parsed);
  }
  std::vector<fp::ChainSpot> basis;
  if (!a.basis.empty()) {
    const auto text = slurp(a.basis);
    if (!text) return usage("cannot read --basis");
    auto parsed = fp::parse_basis_csv(*text);
    if (!parsed) return usage(parsed.error().to_string().c_str());
    basis = std::move(*parsed);
  }
  std::vector<fp::QuoteRow> quotes;
  {
    const auto text = slurp(a.quotes);
    if (!text) return usage("cannot read --quotes");
    auto parsed = fp::parse_quotes_csv(*text);
    if (!parsed) return usage(parsed.error().to_string().c_str());
    quotes = std::move(*parsed);
  }
  std::vector<fp::PredictionRow> predictions;
  {
    const auto text = slurp(a.predictions);
    if (!text) return usage("cannot read --predictions");
    auto parsed = fp::parse_predictions_csv(*text);
    if (!parsed) return usage(parsed.error().to_string().c_str());
    predictions = std::move(*parsed);
  }
  const auto loaded = std::chrono::steady_clock::now();
  std::vector<fp::ProbeRow> rows;
  const auto report = fp::run_fit_risk_probe(quotes, predictions, labels, basis, a.options,
                                             a.rows.empty() ? nullptr : &rows);
  if (!report) return usage(report.error().to_string().c_str());
  const auto evaluated = std::chrono::steady_clock::now();
  if (!spill(a.out, fp::probe_report_to_json(*report))) return usage("cannot write --out");
  if (!a.rows.empty() && !spill(a.rows, fp::probe_rows_to_csv(rows))) {
    return usage("cannot write --rows");
  }
  const auto seconds = [](auto from, auto to) {
    return std::chrono::duration<double>(to - from).count();
  };
  const auto &t = report->totals;
  std::printf("{\"rows\":%llu,\"evaluated\":%llu,\"price_only\":%llu,\"reprice_mismatch\":%llu,"
              "\"load_s\":%.3f,\"probe_s\":%.3f,\"timing_qualified\":false}\n",
              static_cast<unsigned long long>(t.rows),
              static_cast<unsigned long long>(
                  t.state[static_cast<std::size_t>(fp::RowState::Evaluated)]),
              static_cast<unsigned long long>(
                  t.state[static_cast<std::size_t>(fp::RowState::PriceOnly)]),
              static_cast<unsigned long long>(t.reprice_mismatch), seconds(started, loaded),
              seconds(loaded, evaluated));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  // The boundary: std::stoul / std::stod on a malformed flag and allocation failure are
  // the only throwing paths; report them as a usage error instead of terminating.
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    return usage(e.what());
  }
}
