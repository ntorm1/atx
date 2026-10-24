// Offline orchestration only: model input is a closed, label-free CSV schema.
// Numerical work lives in reusable fitting/pricing APIs, independent of data files.
#include "atx/vol/api/core/vol_time.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"         // lane H1: graded.serve_records
#include "atx/vol/api/fitting/slice_serve_record.hpp" // lane H1: graded.serve_records
#include "atx/vol/api/marketdata/dividend_schedule.hpp"
#include "atx/vol/api/pricing/american.hpp" // lane H1: andersen_lake (fast-path audit)
#include "atx/vol/api/pricing/dividend.hpp"
#include "bench_clocks.hpp"
#include "core/parallel_for.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "fitting/slice_serve_record_detail.hpp" // lane H1: the fast-path audit's contract
#include "pricing/american_iv_stream.hpp" // lane K2: --inverse-replay
#include "quote_bench_policy.hpp"
#include "quote_bench_common.hpp"
#include "quote_bench_progress.hpp"
#include "quote_bench_data.hpp"
#include "quote_bench_acceptance.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <span>
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
  bool timing_detail{false}; // opt-in per-chain fit timing counters (graded admission)
  bool deam_audit{false};    // opt-in: every Premium de-Am inversion also runs Search (L2e)
  bool acceptance_audit{false}; // offline actual-API proof; timings are not qualifying
  // Lane B0 benchmark harness (bench_clocks.hpp, docs/benchmark-clocks.md).
  std::size_t reps{1};              // in-process repetitions of the quote-slice run
  bool cold_reset{false};           // reset every market-dependent memo before each rep
  std::optional<unsigned> affinity; // pin the process and the fit worker to one logical CPU
  bool require_pcore{false};        // refuse unless the pinned CPU is a performance core
  // Lane K2 (r5): record the fit's Premium inverse request stream and replay it through
  // iv_batch::invert_many after the run (src/pricing/american_iv_stream.hpp).
  std::optional<std::string> inverse_replay; // auto | scalar | avx2
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
                   "[--max-weight 1000] [--timing-detail] [--deam-audit] [--reps 1] "
                   "[--cold-reset] [--affinity CPU [--require-pcore]] "
                   "[--inverse-replay auto|scalar|avx2]\n"
                   "[--acceptance-audit] (offline final API proof; not a timing run)\n"
                   "Defaults: --policy auto --dividends funding-pv. "
                   "Quality/min-quotes flags require explicit --curve. "
                   "--timing-detail (quote-slice only) adds per-chain fit timing counters to "
                   "graded_stats.slices[].timing and real per-chain graded latencies. "
                   "--deam-audit (with --timing-detail) also runs the Search inversion beside "
                   "every graded.deam_inversion premium one and reports the pair statistics "
                   "(the predictions are the premium ones). "
                   "--reps N (quote-slice) repeats the run in-process and reports min / median "
                   "/ p95 / p99 / max of every clock; --cold-reset clears every market-"
                   "dependent memo before each rep; --affinity CPU pins the process and the "
                   "fit worker to one logical CPU (requires ATX_VOL_FIT_WORKERS=1); "
                   "--require-pcore refuses a CPU that is not on a performance core. Clock "
                   "and receipt definitions: atx-vol/docs/benchmark-clocks.md. "
                   "--inverse-replay ISA (quote-slice, one rep) records every Premium "
                   "inverse request of the run and replays the stream through the batch "
                   "inversion after it, reporting bench.inverse_replay "
                   "(atx-vol/docs/american-iv-batch.md).\n";
      args.help_requested = true;
      return args;
    }
    if (key == "--timing-detail") {
      args.timing_detail = true;
      continue;
    }
    if (key == "--acceptance-audit") {
      args.acceptance_audit = true;
      continue;
    }
    if (key == "--deam-audit") {
      args.deam_audit = true;
      continue;
    }
    if (key == "--cold-reset") {
      args.cold_reset = true;
      continue;
    }
    if (key == "--require-pcore") {
      args.require_pcore = true;
      continue;
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
    else if (key == "--reps") args.reps = number<std::size_t>(value, key);
    else if (key == "--affinity") args.affinity = number<unsigned>(value, key);
    else if (key == "--inverse-replay") args.inverse_replay = value;
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
  if (args.acceptance_audit && (args.model != "quote-slice" || !args.curve.empty())) {
    throw std::runtime_error("--acceptance-audit requires quote-slice graded policy");
  }
  if (args.timing_detail && args.model != "quote-slice") {
    throw std::runtime_error("--timing-detail requires --model quote-slice");
  }
  if (args.deam_audit && !args.timing_detail) {
    throw std::runtime_error("--deam-audit requires --timing-detail");
  }
  if (args.model != "quote-slice" &&
      (seen.contains("--reps") || args.cold_reset || args.affinity || args.require_pcore)) {
    throw std::runtime_error("--reps, --cold-reset, --affinity and --require-pcore require "
                             "--model quote-slice");
  }
  if (args.reps == 0U || args.reps > 1000U) throw std::runtime_error("--reps must be 1..1000");
  if (args.inverse_replay &&
      (args.model != "quote-slice" || args.reps != 1U ||
       (*args.inverse_replay != "auto" && *args.inverse_replay != "scalar" &&
        *args.inverse_replay != "avx2"))) {
    throw std::runtime_error("--inverse-replay must be auto, scalar or avx2 and requires "
                             "--model quote-slice with one rep");
  }
  if (args.require_pcore && !args.affinity) {
    throw std::runtime_error("--require-pcore requires --affinity");
  }
  if (args.affinity && atx_auto_worker_count() != 1U) {
    throw std::runtime_error("--affinity requires ATX_VOL_FIT_WORKERS=1: one fit worker and no "
                             "nested fan-out on the pinned CPU");
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
  if (header.contains("observation_ns")) {
    // Never throws (InputRow::observation_ns): only the opt-in lane C0 keys read it.
    const auto cell = get("observation_ns");
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(cell.data(), cell.data() + cell.size(), value);
    if (error == std::errc{} && end == cell.data() + cell.size()) row.observation_ns = value;
  }
  return row;
}

// Lane B0 process sub-timers of the input: read_s is the stream (open, header, every
// getline), decode_s the per-record parse and admission checks between the reads.
struct ReadTimes {
  double read_s{0.0};
  double decode_s{0.0};
};

std::vector<InputRow> read_rows(const Arguments &args, ReadTimes &times) {
  const auto read_start = Clock::now();
  std::ifstream stream{args.input};
  if (!stream) throw std::runtime_error("Cannot open input CSV");
  const Header header = read_header(stream);
  std::vector<InputRow> rows;
  std::set<std::uint64_t> ids;
  std::string line;
  while (std::getline(stream, line)) {
    const auto decode_start = Clock::now();
    if (rows.size() >= args.max_rows) {
      throw std::runtime_error("Input exceeds --max-rows; shard the cohort or raise the limit");
    }
    if (line.size() > 1048576U) throw std::runtime_error("CSV record exceeds 1 MiB");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto row = convert_row(header, csv_record(line));
    if (!ids.insert(row.id).second) throw std::runtime_error("Duplicate row_id");
    rows.push_back(std::move(row));
    times.decode_s += bench::seconds_between(decode_start, Clock::now());
  }
  times.read_s = bench::seconds_between(read_start, Clock::now()) - times.decode_s;
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
  bool graded_ladder{false};
  Json graded_stats = Json::object();
  bool acceptance_audit{false};
  // Opt-in policy key `price_only`: rows served as ok_price_only (never in `served`).
  bool price_only{false};
  std::size_t price_only_rows{};
  // Lane C0 opt-in policy keys (`serve`, `snapshot`); absent = the historical path. The
  // report blocks they fill (identifiability, one_spot) are written only when opted in.
  bool floor_price_only{false};
  bool metadata{false};
  quote_bench_policy::SnapshotOptions snapshot{};
  Json identifiability = Json::object();
  Json one_spot = Json::object();
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
  // Opt-in --timing-detail: graded chain latency is each chain's own fit wall
  // (counters::fit_timing) instead of its surface wall divided by its expiries.
  bool timing_detail{false};
  // Lane B0 (bench_clocks.hpp): the cold-calibration clock, the policy load outside it,
  // its exclusive stage laps and fit-route work (armed with --timing-detail), and the
  // de-Am stage clock armed on the fitting thread for the run; `bench` is the report's
  // bench object, filled by main.
  double cold_calibration_seconds{};
  double policy_load_seconds{};
  bench::PhaseClock phases;
  bench::FitWork fit_work;
  DeAmStageClock deam_clock{};
  DeAmStageClock deam_clock_mark{};
  Json bench = nullptr;
  // Lane H1 (graded.serve_records, opt-in): the serve_build stage's book and clocks and the
  // evaluate_quote_slice fast-path audit, reported in graded_stats.serve_records.
  quote_bench_policy::ServeRecordsOptions serve_records{};
  std::shared_ptr<ServeBook> serve_book;
  double serve_build_seconds{};
  std::vector<double> serve_first_usable_us;
  std::vector<double> serve_total_us;
  std::vector<double> serve_mid_off_ns;
  std::vector<double> serve_mid_on_ns;
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
  QuoteSliceSnapshotInfo snapshot{}; // lane C0 provenance (clocks, spot / carry versions)
};
using SnapshotIdentity = std::array<std::string, 3>;
using ChainRows = std::map<std::uint64_t, std::vector<std::size_t>>;

void count_into(Json &counts, const std::string &key, std::size_t count = 1U) {
  if (counts.is_null()) counts = Json::object();
  counts[key] = counts.value(key, std::size_t{}) + count;
}

// ── Lane C0: one authoritative spot per snapshot (policy key `snapshot`) ──────────
//
// S* of one underlying snapshot (underlier, snapshot_ns), from its rows' own spot
// observations: `nearest` takes the median spot observed at the observation instant
// nearest snapshot_ns (earlier on a tie), `median` (and nearest without any
// observation_ns) the median spot of every row.
struct SnapshotSpot {
  double spot{kNaN};
  std::int64_t underlying_ns{0}; // observation instant of S*; 0 under the median rule
  std::size_t observations{0};   // spot observations S* is the median of
  std::string rule;              // nearest | median | median_fallback
};
using SpotKey = std::array<std::string, 2>; // underlier, snapshot_ns

SnapshotSpot choose_spot(const std::vector<std::pair<std::int64_t, double>> &seen,
                         std::int64_t snapshot_ns, quote_bench_policy::OneSpot mode) {
  using quote_bench_policy::OneSpot;
  SnapshotSpot out;
  std::int64_t best = 0;
  std::uint64_t gap = std::numeric_limits<std::uint64_t>::max();
  for (const auto &[ns, spot] : seen) {
    if (mode != OneSpot::Nearest || ns <= 0 || !(std::isfinite(spot) && spot > 0.0)) continue;
    const auto distance = ns >= snapshot_ns
        ? static_cast<std::uint64_t>(ns) - static_cast<std::uint64_t>(snapshot_ns)
        : static_cast<std::uint64_t>(snapshot_ns) - static_cast<std::uint64_t>(ns);
    if (distance < gap || (distance == gap && ns < best)) {
      gap = distance;
      best = ns;
    }
  }
  std::vector<double> spots;
  for (const auto &[ns, spot] : seen) {
    if (std::isfinite(spot) && spot > 0.0 && (best == 0 || ns == best)) spots.push_back(spot);
  }
  if (spots.empty()) return out;
  out.observations = spots.size();
  out.spot = median(std::move(spots));
  out.underlying_ns = best;
  out.rule = best != 0 ? "nearest" : mode == OneSpot::Nearest ? "median_fallback" : "median";
  return out;
}

std::map<SpotKey, SnapshotSpot> choose_snapshot_spots(const std::vector<InputRow> &rows,
                                                      const ChainRows &chains,
                                                      RunStatistics &stats) {
  std::map<SpotKey, std::vector<std::pair<std::int64_t, double>>> seen;
  for (const auto &[chain_id, indices] : chains) {
    static_cast<void>(chain_id);
    for (const auto i : indices) {
      const auto &row = rows[i];
      seen[{row.identity[0], row.identity[3]}].emplace_back(row.observation_ns, row.market.spot);
    }
  }
  std::map<SpotKey, SnapshotSpot> out;
  Json spots = Json::array();
  for (const auto &[key, observations] : seen) {
    std::int64_t snapshot_ns = 0;
    static_cast<void>(std::from_chars(key[1].data(), key[1].data() + key[1].size(), snapshot_ns));
    const SnapshotSpot choice = choose_spot(observations, snapshot_ns, stats.snapshot.one_spot);
    spots.push_back({{"underlier", key[0]}, {"snapshot_ns", key[1]},
                     {"spot", finite_or_null(choice.spot)},
                     {"underlying_ns", choice.underlying_ns}, {"rule", choice.rule},
                     {"observations", choice.observations}});
    out.emplace(key, choice);
  }
  stats.one_spot["mode"] = quote_bench_policy::to_string(stats.snapshot.one_spot);
  stats.one_spot["stale_spot_bps"] = stats.snapshot.stale_spot_bps;
  stats.one_spot["stale_spot"] = stats.snapshot.refuse_stale ? "refuse" : "flag";
  stats.one_spot["spot_version"] = 1U;
  stats.one_spot["carry_version"] = 1U;
  stats.one_spot["carry_convention"] = "forward_preserving";
  stats.one_spot["snapshots"] = std::move(spots);
  return out;
}

// Re-expresses one chain on its snapshot's S*: the market and every contract yield
// override move by the forward-preserving shift (rebase_quote_slice_spot), and the raw
// spot, versions, clock and stale flag go to the chain's provenance. Returns the refusal
// status of the chain, empty when it is kept.
std::string apply_one_spot(const SnapshotSpot &choice, QuoteSliceDividendModel model,
                           std::uint64_t chain_id, PreparedChain &chain, RunStatistics &stats) {
  const double raw = chain.market.spot;
  Json record{{"chain_id", chain_id}, {"raw_spot", raw}, {"spot", finite_or_null(choice.spot)}};
  const auto rebased = rebase_quote_slice_spot(chain.market, choice.spot, model);
  if (!rebased) {
    record["status"] = "invalid_snapshot_spot";
    record["error"] = rebased.error().to_string();
    stats.one_spot["chains"].push_back(std::move(record));
    count_into(stats.one_spot["chain_status_counts"], "invalid_snapshot_spot");
    return "invalid_snapshot_spot";
  }
  const double bps = 10000.0 * std::abs(raw - choice.spot) / choice.spot;
  const bool stale = bps > stats.snapshot.stale_spot_bps;
  record["spot_shift_bps"] = bps;
  record["yield_shift"] = rebased->yield_shift;
  record["forward"] = rebased->forward;
  record["stale"] = stale;
  stats.one_spot["max_spot_shift_bps"] =
      std::max(stats.one_spot.value("max_spot_shift_bps", 0.0), bps);
  stats.one_spot["max_abs_yield_shift"] =
      std::max(stats.one_spot.value("max_abs_yield_shift", 0.0), std::abs(rebased->yield_shift));
  if (stale && stats.snapshot.refuse_stale) {
    record["status"] = "stale_spot";
    stats.one_spot["chains"].push_back(std::move(record));
    count_into(stats.one_spot["chain_status_counts"], "stale_spot");
    return "stale_spot";
  }
  const std::string status = stale ? "rebased_stale" : "rebased";
  chain.market = rebased->market;
  for (auto &quote : chain.quotes) {
    if (quote.continuous_yield) *quote.continuous_yield += rebased->yield_shift;
  }
  if (stats.snapshot.carry_price_tolerance > 0.0) {
    const auto normalized = coalesce_quote_slice_carry(
        chain.market, chain.quotes, model, stats.snapshot.carry_price_tolerance);
    if (!normalized) {
      record["status"] = "carry_conflict";
      record["error"] = normalized.error().to_string();
      stats.one_spot["chains"].push_back(std::move(record));
      count_into(stats.one_spot["chain_status_counts"], "carry_conflict");
      return "carry_conflict";
    }
    record["carry_normalization"] = {
        {"strikes_changed", normalized->strikes_changed},
        {"max_abs_yield_change", normalized->max_abs_yield_change},
        {"max_price_change_bound", normalized->max_price_change_bound},
        {"price_tolerance", stats.snapshot.carry_price_tolerance}};
  }
  record["status"] = status;
  stats.one_spot["chains"].push_back(std::move(record));
  count_into(stats.one_spot["chain_status_counts"], status);
  QuoteSliceSnapshotInfo &info = chain.snapshot;
  info.underlying_ns = choice.underlying_ns;
  info.spot_version = 1U;
  // The rule that actually chose S*: a nearest request without observation instants is
  // the median rule (choice.rule median_fallback).
  info.spot_rule =
      choice.rule == "nearest" ? QuoteSliceSpotRule::Nearest : QuoteSliceSpotRule::Median;
  info.carry_version = 1U;
  info.carry_convention = QuoteSliceCarryConvention::ForwardPreserving;
  info.raw_spot = raw;
  info.spot_stale = stale;
  return {};
}

Json repair_json(const QuoteSliceRepairRecord &r) {
  return {{"outcome", to_string(r.outcome)}, {"nodes", r.nodes},
          {"material_nodes", r.material_nodes}, {"max_price_move", r.max_price_move},
          {"max_vol_move", r.max_vol_move}, {"max_unquoted_vol_move", r.max_unquoted_vol_move},
          {"unidentified", r.unidentified}, {"terminal_floor_asks", r.terminal_floor_asks},
          {"terminal_floor_max_ask_raise", r.terminal_floor_max_ask_raise}};
}

// A published volatility that is identified: finite with a volatility convention (an ok
// row whose volatility query_quote_slice identifies; stamp_metadata sets the convention).
bool vol_identified(const Prediction &prediction) {
  return std::isfinite(prediction.vol) && prediction.vol_convention != "none";
}

// Lane C0 metadata of one published row (written only under serve.metadata): the
// convention of the served volatility (none unless the row is served ok with a volatility
// query_quote_slice identifies), the identification reason and IV interval of a served
// slice's Success / PriceOnly row (query_quote_slice's own; none for every other row),
// the fit's model and dividend convention, and the chain's snapshot provenance.
void stamp_metadata(const QuoteSliceFit &fit, const QuoteSliceRow &row,
                    const QuoteSliceSnapshotInfo &snapshot, bool slice_served,
                    Prediction &prediction) {
  prediction.vol_convention =
      prediction.status == "ok" ? to_string(vol_convention(row)) : "none";
  if (slice_served && (row.evaluation_status == QuoteSliceEvaluationStatus::Success ||
                       row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly)) {
    prediction.price_only_reason = to_string(row.price_only_reason);
    prediction.iv_lower = row.iv_lower;
    prediction.iv_upper = row.iv_upper;
  }
  prediction.model_kind = to_string(model_kind(fit));
  prediction.dividend_convention = to_string(fit.dividend_model);
  prediction.snapshot = snapshot;
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

// Knot-first ladder provenance of one graded slice (emitted only with the ladder).
void add_ladder_fields(const QuoteSurfaceSlice &slice, Json &entry) {
  entry["method"] = to_string(slice.method);
  entry["ladder_fallback"] = to_string(slice.ladder_fallback);
  entry["knot_status"] = to_string(slice.knot_status);
  entry["knot_error"] = slice.knot_error;
  entry["point_error"] = slice.point_error;
  entry["repair_rung"] = slice.repair_rung;
  entry["outside_fraction"] = slice.outside_fraction;
  entry["outside_quotes"] = slice.outside_quotes;
}

// --timing-detail: one chain's fit timing counters (exclusive wall per bucket, plus
// the inclusive repair-rung cross-cut).
Json timing_json(const counters::fit_timing::FitTimingCounters &t) {
  namespace ft = counters::fit_timing;
  Json al_by_bucket = Json::object();
  for (unsigned b = 0; b < ft::kBucketCount; ++b) {
    al_by_bucket[ft::kBucketNames[b]] = t.al_by_bucket[b];
  }
  Json deam_al_by_reason = Json::object();
  for (unsigned r = 0; r < ft::kDeamReasonCount; ++r) {
    deam_al_by_reason[ft::kDeamReasonNames[r]] = t.deam_al_by_reason[r];
  }
  Json reimply_stages = Json::object();
  for (unsigned s = 0; s < ft::kReimplyStageCount; ++s) {
    reimply_stages[ft::kReimplyStageNames[s]] = {
        {"calls", t.reimply_stage_calls[s]},
        {"boundary_solves", t.reimply_stage_al[s]},
        {"iv_iterations", t.reimply_stage_iv[s]},
        {"elapsed_ns", t.reimply_stage_ns[s]}};
  }
  return {{"wall_us", t.wall_us}, {"deam_us", t.deam_us}, {"solve_us", t.solve_us},
          {"project_us", t.project_us}, {"price_us", t.price_us},
          {"certify_us", t.certify_us}, {"calendar_us", t.calendar_us},
          {"other_us", t.other_us}, {"al_price_calls", t.al_price_calls},
          {"al_greeks_calls", t.al_greeks_calls},
          {"reimply_native_attempts", t.reimply_native_attempts},
          {"reimply_native_roots", t.reimply_native_roots},
          {"reimply_native_fallbacks", t.reimply_native_fallbacks},
          {"reimply_native_fast_confirms", t.reimply_native_fast_confirms},
          {"reimply_price_family_checks", t.reimply_price_family_checks},
          {"reimply_price_enclosure_hits", t.reimply_price_enclosure_hits},
          {"reimply_price_cold_checks", t.reimply_price_cold_checks},
          {"reimply_price_rejections", t.reimply_price_rejections},
          {"reimply_fast_attempts", t.reimply_fast_attempts},
          {"reimply_fast_roots", t.reimply_fast_roots},
          {"reimply_seed_attempts", t.reimply_seed_attempts},
          {"reimply_seed_roots", t.reimply_seed_roots},
          {"reimply_economic_attempts", t.reimply_economic_attempts},
          {"reimply_economic_roots", t.reimply_economic_roots},
          {"reimply_economic_ns", t.reimply_economic_ns},
          {"reimply_native_max_bound_iv", t.reimply_native_max_bound_iv},
          {"reimply_cache_hits", t.reimply_cache_hits},
          {"reimply_uncached", t.reimply_uncached},
          {"reimply_no_context", t.reimply_no_context},
          {"reimply_outside_q", t.reimply_outside_q},
          {"reimply_outside_window", t.reimply_outside_window},
          {"reimply_search_nonmonotone", t.reimply_search_nonmonotone},
          {"reimply_plateau_searches", t.reimply_plateau_searches},
          {"reimply_envelope_covered", t.reimply_envelope_covered},
          {"reimply_accept_by_kind", t.reimply_accept_by_kind},
          {"reimply_accept_by_reason", t.reimply_accept_by_reason},
          {"reimply_stages", std::move(reimply_stages)},
          {"reimply_batch_calls", t.reimply_batch_calls},
          {"reimply_batch_lanes", t.reimply_batch_lanes},
          {"reimply_batch_avx2_calls", t.reimply_batch_avx2_calls},
          {"reimply_batch_hits", t.reimply_batch_hits},
          {"reimply_batch_confirm_hits", t.reimply_batch_confirm_hits},
          {"reimply_batch_family_prices", t.reimply_batch_family_prices},
          {"reimply_batch_family_retry_prices", t.reimply_batch_family_retry_prices},
          {"deam_batch_family_prices", t.deam_batch_family_prices},
          {"deam_batch_family_retry_prices", t.deam_batch_family_retry_prices},
          {"reimply_batch_lane_ns", t.reimply_batch_lane_ns},
          {"reimply_batch_family_ns", t.reimply_batch_family_ns},
          {"reimply_batch_confirm_ns", t.reimply_batch_confirm_ns},
          {"certification_passes", t.certification_passes},
          {"channel_solves", t.channel_solves}, {"jacobian_evals", t.jacobian_evals},
          {"rung_attempts", t.rung_attempts}, {"calendar_pairs", t.calendar_pairs},
          {"projections", t.projections}, {"iv_newton_iters", t.iv_newton_iters},
          {"al_by_bucket", std::move(al_by_bucket)},
          {"repair_rung_visits", t.repair_rung_visits}, {"repair_rung_us", t.repair_rung_us},
          {"deam_inversions", t.deam_inversions}, {"deam_inversion_al", t.deam_inversion_al},
          {"deam_inversion_al_max", t.deam_inversion_al_max}, {"deam_premium", t.deam_premium},
          {"deam_premium_fallbacks", t.deam_premium_fallbacks},
          {"deam_premium_fallback_by_reason", t.deam_premium_fallback_by_reason},
          {"deam_premium_max_residual_usd", t.deam_premium_max_residual_usd},
          {"deam_audit_pairs", t.deam_audit_pairs},
          {"deam_audit_status_mismatch", t.deam_audit_status_mismatch},
          {"deam_audit_max_dsigma", t.deam_audit_max_dsigma},
          {"deam_audit_dsigma_decades", t.deam_audit_dsigma_decades},
          {"deam_audit_max_newton_premium", t.deam_audit_max_newton_premium},
          {"deam_audit_max_newton_search", t.deam_audit_max_newton_search},
          {"deam_audit_search_farther", t.deam_audit_search_farther},
          {"deam_al_by_reason", std::move(deam_al_by_reason)},
          {"deam_confirms", t.deam_confirms},
          {"deam_confirms_skipped", t.deam_confirms_skipped},
          {"deam_ambiguous_edges", t.deam_ambiguous_edges},
          {"deam_seed_routed", t.deam_seed_routed},
          {"deam_gate_max_bound_vol", t.deam_gate_max_bound_vol},
          {"deam_audit_skipped_pairs", t.deam_audit_skipped_pairs},
          {"deam_audit_max_newton_skipped", t.deam_audit_max_newton_skipped},
          {"deam_family_builds", t.deam_family_builds},
          {"deam_family_build_us", t.deam_family_build_us},
          {"deam_family_prices", t.deam_family_prices},
          {"deam_family_refused", t.deam_family_refused},
          {"deam_family_grid_prices", t.deam_family_grid_prices},
          {"deam_family_grid_cold", t.deam_family_grid_cold},
          {"deam_accept_by_kind", t.deam_accept_by_kind},
          {"deam_accept_by_reason", t.deam_accept_by_reason},
          {"deam_native_status", t.deam_native_status},
          {"deam_route_outcome", t.deam_route_outcome},
          {"deam_native_carried", t.deam_native_carried},
          {"deam_reference_requests", t.deam_reference_requests},
          {"deam_reference_evaluations", t.deam_reference_evaluations},
          {"deam_reference_budget_refused", t.deam_reference_budget_refused},
          {"deam_edges_bound_decided", t.deam_edges_bound_decided},
          {"deam_edges_priced", t.deam_edges_priced},
          {"deam_family_sides_requested", t.deam_family_sides_requested},
          {"deam_family_refused_by_reason", t.deam_family_refused_by_reason},
          {"deam_family_grid_max_bound_usd", t.deam_family_grid_max_bound_usd},
          {"deam_sub_request_ns", t.deam_sub_request_ns},
          {"deam_sub_lane_ns", t.deam_sub_ns[0]},
          {"deam_sub_family_accept_ns", t.deam_sub_ns[1]},
          {"deam_sub_confirm_ns", t.deam_sub_ns[2]},
          {"deam_sub_leg_ns", t.deam_sub_ns[3]},
          {"deam_sub_reference_ns", t.deam_sub_ns[4]},
          {"deam_sub_endpoint_ns", t.deam_sub_endpoint_ns},
          {"deam_sub_prefill_ns", t.deam_sub_prefill_ns},
          {"deam_batch_lanes", t.deam_batch_lanes},
          {"deam_batch_lane_hits", t.deam_batch_lane_hits},
          {"deam_batch_confirm_hits", t.deam_batch_confirm_hits},
          {"deam_sub_grid_family_ns", t.deam_sub_grid_family_ns},
          {"deam_sub_grid_kernel_ns", t.deam_sub_grid_kernel_ns}};
}

// Lane B0: charges the fit lap that just ended to its exclusive stages
// (bench_clocks.hpp fit_parts): the fit_timing buckets of the lap's chains, with the
// family build wall and the de-Am stage clock's endpoint and exception time of the lap
// carved out of their deam bucket. Whatever the lap held beyond what the chains
// measured is charged to no stage (PhaseClock unclaimed): it shows as unattributed_s.
void charge_fit_lap(RunStatistics &stats,
                    std::span<const counters::fit_timing::FitTimingCounters> chains) {
  if (!stats.phases.armed()) return;
  bench::FitWork lap;
  for (const auto &chain : chains) {
    bench::add_fit_work(lap, chain);
    bench::add_fit_work(stats.fit_work, chain);
  }
  const DeAmStageClock &now = stats.deam_clock;
  DeAmStageClock &mark = stats.deam_clock_mark;
  const std::uint64_t endpoint_ns = now.endpoint_ns - mark.endpoint_ns;
  std::uint64_t exception_ns = now.fast_redo_ns - mark.fast_redo_ns;
  exception_ns += now.reference_ns - mark.reference_ns; // K4: the bounded reference route
  for (std::size_t r = 0; r < now.premium_fallback_ns.size(); ++r) {
    exception_ns += now.premium_fallback_ns[r] - mark.premium_fallback_ns[r];
  }
  mark = now;
  double excess = 0.0;
  const auto parts = bench::fit_parts(lap, endpoint_ns, exception_ns, excess);
  stats.phases.lap_split(Clock::now(), parts);
  stats.phases.add_conflict(excess);
}

// ── Lane H1: the serve_build stage and the fast-path audit (graded.serve_records) ───────

[[nodiscard]] ServeRecordPolicy serve_record_policy_of(quote_bench_policy::ServeRecordProfile p) {
  using quote_bench_policy::ServeRecordProfile;
  if (p == ServeRecordProfile::Tick) return serve_record_policy_tick();
  ServeRecordPolicy policy;
  policy.ladders = p != ServeRecordProfile::NoLadders;
  return policy;
}

// Two evaluate_quote_slice answers agree: both values bit for bit, or the same refusal.
[[nodiscard]] bool same_value(const Result<QuoteSliceValue> &a, const Result<QuoteSliceValue> &b) {
  if (a.has_value() != b.has_value()) return false;
  if (!a) return a.error().code() == b.error().code();
  return std::bit_cast<std::uint64_t>(a->volatility) ==
             std::bit_cast<std::uint64_t>(b->volatility) &&
         std::bit_cast<std::uint64_t>(a->price) == std::bit_cast<std::uint64_t>(b->price);
}

[[nodiscard]] Json value_json(const Result<QuoteSliceValue> &v) {
  if (!v) return {{"error", v.error().to_string()}};
  return {{"volatility", v->volatility}, {"price", v->price}};
}

// The Newton distance of `sigma` to the accurate kernel's root of `price` on the record's
// contract: (A(sigma) - price) / vega_A(sigma), A = andersen_lake on the fit's kernel
// options, vega by a central difference (h = 1e-5 sigma). NaN when a price fails.
[[nodiscard]] double newton_distance(const SliceServeRecord &record, double strike, Side side,
                                     double sigma, double price) {
  const auto c = detail::serve_record_contract(record, strike, side);
  if (!c || !std::isfinite(sigma) || !(sigma > 0.0)) return kNaN;
  const double h = 1.0e-5 * sigma;
  const auto at = [&](double s) {
    const Result<double> v =
        andersen_lake(c->spot, strike, c->tau, s, c->rate, c->q, side, c->al_opts);
    return v ? *v : kNaN;
  };
  const double p0 = at(sigma);
  const double vega = (at(sigma + h) - at(sigma - h)) / (2.0 * h);
  if (!std::isfinite(p0) || !(std::abs(vega) > 0.0)) return kNaN;
  return (p0 - price) / vega;
}

// The serve_build stage of one served slice: its record built and published into the run's
// book (two-phase unless no_ladders) under the chain id, then the fast-path audit - every
// served row and every node midpoint answered without and with the record attached to the
// fit (QuoteSliceFit::serve_record; detached again before returning). Returns, per row, the
// record's value where it differs from the record-free answer (published instead).
std::vector<std::optional<QuoteSliceValue>>
serve_build_slice(RunStatistics &stats, std::uint64_t chain, QuoteSliceFit &fit,
                  std::span<const QuoteSliceQuote> quotes) {
  std::vector<std::optional<QuoteSliceValue>> out(quotes.size());
  Json &block = stats.graded_stats["serve_records"];
  if (!stats.serve_book) {
    ServeBookPolicy book_policy;
    book_policy.build_workers = 0; // the stage publishes inline; nothing is rebuilt
    stats.serve_book = std::make_shared<ServeBook>(book_policy);
  }
  stats.phases.lap(bench::Stage::Publication);
  const auto t0 = Clock::now();
  ServeBookEntry key;
  key.expiry_id = chain;
  key.tag = chain;
  const auto profile = stats.serve_records.profile;
  const Result<ServeBuildStage> stage =
      serve_build(*stats.serve_book, key, fit, serve_record_policy_of(profile),
                  profile != quote_bench_policy::ServeRecordProfile::NoLadders);
  const auto t1 = Clock::now();
  stats.serve_build_seconds += std::chrono::duration<double>(t1 - t0).count();
  stats.phases.lap(bench::Stage::ServeBuild, t1);
  if (!stage) {
    count_into(block["build_errors"], stage.error().to_string());
    return out;
  }
  count_into(block, "records");
  stats.serve_first_usable_us.push_back(stage->first_usable_us);
  stats.serve_total_us.push_back(stage->total_us);
  const ServeSupport &support = stage->record->support();
  for (const ServeSideSupport *side : {&support.call, &support.put}) {
    if (side->family) {
      const char *origin = side->origin == ServeFamilyOrigin::Handed  ? "handed"
                           : side->origin == ServeFamilyOrigin::Built ? "built"
                                                                      : "other";
      count_into(block["family_origin"], origin);
    }
    count_into(block, "ladders", side->ladders);
    count_into(block, "ladders_validated", side->ladders_validated);
    count_into(block, "ladders_sampled", side->ladders_sampled);
  }
  // The audit is untimed (its record-free answers run the cold inversion): skipped.
  const auto answer = [&fit, &stage](double strike, Side side, bool with) {
    fit.serve_record = with ? stage->record : nullptr;
    Result<QuoteSliceValue> v = evaluate_quote_slice(fit, strike, side);
    fit.serve_record = nullptr;
    return v;
  };
  double max_dvol = block.value("rows_max_dvol", 0.0);
  for (std::size_t j = 0; j < quotes.size() && j < fit.rows.size(); ++j) {
    if (fit.rows[j].evaluation_status != QuoteSliceEvaluationStatus::Success) continue;
    const auto off = answer(quotes[j].strike, quotes[j].side, false);
    const auto on = answer(quotes[j].strike, quotes[j].side, true);
    count_into(block, "rows_compared");
    if (same_value(off, on)) {
      count_into(block, "rows_identical");
      continue;
    }
    count_into(block, "rows_differ");
    const double dvol = off && on ? std::abs(on->volatility - off->volatility) : kNaN;
    if (std::isfinite(dvol)) max_dvol = std::max(max_dvol, dvol);
    if (on) out[j] = *on;
    const double nd_off =
        off ? newton_distance(*stage->record, quotes[j].strike, quotes[j].side,
                              off->volatility, off->price)
            : kNaN;
    const double nd_on =
        on ? newton_distance(*stage->record, quotes[j].strike, quotes[j].side, on->volatility,
                             on->price)
           : kNaN;
    if (std::isfinite(nd_on)) {
      block["rows_max_newton_on"] =
          std::max(block.value("rows_max_newton_on", 0.0), std::abs(nd_on));
    }
    if (std::isfinite(nd_off)) {
      block["rows_max_newton_off"] =
          std::max(block.value("rows_max_newton_off", 0.0), std::abs(nd_off));
    }
    if (std::abs(nd_on) > 1.0e-7) count_into(block, "rows_on_newton_above_1e-7");
    if (std::abs(nd_off) > 1.0e-7) count_into(block, "rows_off_newton_above_1e-7");
    if (block["differences"].size() < 5000U) {
      block["differences"].push_back({{"chain_id", chain}, {"strike", quotes[j].strike},
                                      {"side", quotes[j].side == Side::Call ? "call" : "put"},
                                      {"off", value_json(off)}, {"on", value_json(on)},
                                      {"abs_dvol", finite_or_null(dvol)},
                                      {"newton_off", finite_or_null(nd_off)},
                                      {"newton_on", finite_or_null(nd_on)}});
    }
  }
  block["rows_max_dvol"] = max_dvol;
  const auto nodes = fit.prices->nodes();
  double mid_max = block.value("midpoints_max_dvol", 0.0);
  for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
    const double K = 0.5 * (nodes[i].strike + nodes[i + 1].strike);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto a0 = Clock::now();
      const auto off = answer(K, side, false);
      const auto a1 = Clock::now();
      const auto on = answer(K, side, true);
      const auto a2 = Clock::now();
      stats.serve_mid_off_ns.push_back(std::chrono::duration<double, std::nano>(a1 - a0).count());
      stats.serve_mid_on_ns.push_back(std::chrono::duration<double, std::nano>(a2 - a1).count());
      count_into(block, "midpoints_compared");
      if (same_value(off, on)) {
        count_into(block, "midpoints_identical");
        continue;
      }
      count_into(block, "midpoints_differ");
      if (off && !on) count_into(block, "midpoints_on_refused_off_ok");
      if (off && on) {
        mid_max = std::max(mid_max, std::abs(on->volatility - off->volatility));
        const double nd_off = newton_distance(*stage->record, K, side, off->volatility,
                                              off->price);
        const double nd_on = newton_distance(*stage->record, K, side, on->volatility, on->price);
        if (std::isfinite(nd_on)) {
          block["midpoints_max_newton_on"] =
              std::max(block.value("midpoints_max_newton_on", 0.0), std::abs(nd_on));
        }
        if (std::isfinite(nd_off)) {
          block["midpoints_max_newton_off"] =
              std::max(block.value("midpoints_max_newton_off", 0.0), std::abs(nd_off));
        }
        if (std::abs(nd_on) > 1.0e-7) count_into(block, "midpoints_on_newton_above_1e-7");
        if (std::abs(nd_off) > 1.0e-7) count_into(block, "midpoints_off_newton_above_1e-7");
        if (block["midpoint_worst"].size() < 200U && std::abs(nd_on) > 1.0e-7) {
          block["midpoint_worst"].push_back(
              {{"chain_id", chain}, {"strike", K}, {"side", side == Side::Call ? "call" : "put"},
               {"off", value_json(off)}, {"on", value_json(on)},
               {"newton_off", finite_or_null(nd_off)}, {"newton_on", finite_or_null(nd_on)}});
        }
      }
    }
  }
  block["midpoints_max_dvol"] = mid_max;
  stats.phases.skip(Clock::now());
  return out;
}

// The run's serve_records block: the profile, the stage's clock, the first-usable and final
// publication latencies per record, the midpoint evaluation costs and the book's versions.
void finish_serve_records(RunStatistics &stats) {
  Json &block = stats.graded_stats["serve_records"];
  block["profile"] = quote_bench_policy::to_string(stats.serve_records.profile);
  block["serve_build_s"] = stats.serve_build_seconds;
  block["first_usable_us"] = bench::summary_json(bench::summarize(stats.serve_first_usable_us));
  block["final_us"] = bench::summary_json(bench::summarize(stats.serve_total_us));
  block["midpoint_eval_off_ns"] = bench::summary_json(bench::summarize(stats.serve_mid_off_ns));
  block["midpoint_eval_on_ns"] = bench::summary_json(bench::summarize(stats.serve_mid_on_ns));
  block["book_versions"] = stats.serve_book ? stats.serve_book->version() : 0U;
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
  const bool joint_cold = surface.cold_fit != QuoteSurfaceColdFit::ExistingPolicy;
  if (joint_cold) graded["cold_fit"]["requested"] = to_string(surface.cold_fit);
  for (const auto &[identity, ids] : snapshot_chains) {
    stats.phases.lap(bench::Stage::Publication); // B0: the previous snapshot's rows
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
      if (joint_cold) count_into(graded["cold_fit"]["fallback_chains"], "expiry_cap", members.size());
      continue;
    }
    const auto &profile = *prepared.at(members.front()).profile;
    std::vector<QuoteSurfaceExpiry> expiries;
    expiries.reserve(members.size());
    for (const auto id : members) {
      const auto &chain = prepared.at(id);
      expiries.push_back({chain.market, chain.quotes, {}, chain.snapshot});
      count_into(stats.per_profile_counts[profile.name], "attempted_chains");
      if (chain.market.dividend_pv > 0.0) ++stats.dividend_chains;
    }
    stats.phases.lap(bench::Stage::Normalization); // B0: this snapshot's worklist
    std::optional<counters::fit_timing::SurfaceTrace> trace;
    if (stats.timing_detail) trace.emplace();
    // Lane C3 (serve.call_floor, opt-in): the snapshot's shared call-floor lines.
    const bool call_floor = profile.config.call_floor.mode != QuoteSliceCallFloor::Legacy;
    std::optional<QuoteSliceConfig> floored;
    if (call_floor) {
      floored.emplace(profile.config);
      floored->call_floor.lines = quote_surface_call_floor_lines(expiries);
    }
    const auto start = Clock::now();
    // H1: audit attaches (so not const); C3: the floored config when serve.call_floor is on.
    auto fitted = fit_quote_surface(expiries, floored ? *floored : profile.config, surface);
    const double surface_us =
        std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    const double per_chain = surface_us / static_cast<double>(members.size());
    const bool timed = trace && trace->chains.size() == members.size();
    if (trace) {
      using Chains = std::span<const counters::fit_timing::FitTimingCounters>;
      charge_fit_lap(stats, timed ? Chains(trace->chains) : Chains{});
    }
    if (timed) {
      std::uint64_t chain_us = 0;
      for (const auto &t : trace->chains) chain_us += t.wall_us;
      count_into(graded["timing_detail"], "surfaces");
      graded["timing_detail"]["surface_wall_us"] =
          graded["timing_detail"].value("surface_wall_us", 0.0) + surface_us;
      count_into(graded["timing_detail"], "chain_wall_us", chain_us);
    }
    for (std::size_t s = 0; s < members.size(); ++s) {
      stats.chain_microseconds.push_back(timed ? static_cast<double>(trace->chains[s].wall_us)
                                               : per_chain);
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
      if (channel_repair || profile.config.repair.terminal_floor_recovery) {
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
      const bool ladder = surface.ladder == QuoteSurfaceLadder::KnotFirst;
      const std::string method = to_string(slice.method);
      if (joint_cold) {
        entry["method"] = method;
        entry["cold_fit"] = {{"requested", to_string(surface.cold_fit)},
                              {"fallback", to_string(slice.cold_fit_fallback)},
                              {"final_price_observations", d.final_price_observations},
                              {"final_price_identified_nodes", d.final_price_identified_nodes}};
        count_into(graded["cold_fit"]["method_chains"], method);
        count_into(graded["cold_fit"]["fallback_chains"], to_string(slice.cold_fit_fallback));
      }
      stats.graded_ladder = stats.graded_ladder || ladder;
      if (ladder) {
        add_ladder_fields(slice, entry);
        count_into(graded["method_chains"][method], grade);
        count_into(graded["ladder_fallback_chains"][to_string(slice.ladder_fallback)], grade);
      }
      if (timed) entry["timing"] = timing_json(trace->chains[s]);
      if (call_floor) {
        // Opt-in call-floor provenance (lane C3); absent when legacy.
        entry["call_floor"] = {{"lines", d.call_floor_lines.size()},
                               {"lifted_nodes", d.call_floor_lifted},
                               {"refused", d.call_floor_refused}};
        count_into(graded["call_floor"], "lifted_nodes", d.call_floor_lifted);
        if (d.call_floor_lifted > 0) count_into(graded["call_floor"], "lifted_chains");
        if (d.call_floor_refused) count_into(graded["call_floor"], "refused_chains");
      }
      if (profile.config.repair.inbox_projection || profile.config.repair.terminal_floor_recovery) {
        // Opt-in in-box projection provenance (lane C4, graded.inbox_projection): the last
        // box rung run of the served fit (QuoteSliceDiagnostics::box); absent when off.
        const QuoteSliceInboxRecord &r = d.box.inbox;
        entry["inbox_projection"] = {
            {"outcome", to_string(r.outcome)},
            {"box_outcome", to_string(d.box.outcome)},
            {"box_refusal", to_string(r.box_refusal)},
            {"conflict", to_string(r.conflict)},
            {"conflict_side", r.conflict_side == Side::Call ? "call" : "put"},
            {"conflict_node", r.conflict_node},
            {"conflict_strike", r.conflict_strike},
            {"conflict_excess", r.conflict_excess},
            {"free_sides", r.free_sides},
            {"fitted_outside", r.fitted_outside},
            {"constraints", r.constraints},
            {"phase1_steps", r.phase1_steps},
            {"phase2_steps", r.phase2_steps},
            {"moved_nodes", d.box.nodes},
            {"max_price_move", d.box.max_price_move},
            {"terminal_floor_asks", d.box.terminal_floor_asks},
            {"terminal_floor_max_ask_raise", d.box.terminal_floor_max_ask_raise},
            {"max_move_half_spreads", r.max_move_half_spreads},
            {"max_mid_half_spreads", r.max_mid_half_spreads},
            {"elapsed_us", r.elapsed_us}};
        if (r.outcome != QuoteSliceInboxOutcome::NotAttempted) {
          count_into(graded["inbox_projection"], to_string(r.outcome));
          if (served) count_into(graded["inbox_projection"], "served_chains");
        }
      }
      if (profile.config.repair.market_distance != QuoteSliceMarketDistance::Off) {
        // Opt-in distance-to-market provenance (lane L3q); absent when off.
        entry["market_distance"] = {
            {"checked_rows", slice.market_checked_rows},
            {"gated_rows", slice.market_gated_rows},
            {"max_spreads", slice.market_max_spreads},
            {"max_spreads_after", slice.market_max_spreads_after},
            {"repair", repair_json(slice.market_repair)},
            {"calendar_joint_repaired", slice.calendar_joint_repaired},
            {"calendar_last_channel_repair", repair_json(slice.calendar_channel_repair)},
            {"calendar_joint_partner_moves", slice.calendar_joint_partner_moves}};
        count_into(graded["market_distance"], "gated_rows", slice.market_gated_rows);
        if (slice.market_repair.outcome == QuoteSliceRepairOutcome::Applied) {
          count_into(graded["market_distance"], "repaired_chains");
        }
        if (slice.calendar_joint_repaired) {
          count_into(graded["market_distance"], "calendar_joint_chains");
        }
      }
      if (surface.publication != QuoteSurfacePublication::Off || joint_cold) {
        // Opt-in publication provenance (lane C2, graded.publication); absent when off.
        const std::array<std::pair<const char *, std::size_t>, 9> publication{
            {{"checks", slice.publication_checks},
             {"rollbacks", slice.publication_rollbacks},
             {"violations", slice.publication_violations},
             {"label_violations", slice.publication_label_violations},
             {"withheld_rows", slice.withheld_rows},
             {"joint_feasible", slice.joint_feasible},
             {"joint_infeasible", slice.joint_infeasible},
             {"joint_nonconverged", slice.joint_nonconverged},
             {"gaps", quote_slice_gaps(slice.fit).size()}}};
        Json counters = Json::object();
        for (const auto &[key, value] : publication) {
          counters[key] = value;
          count_into(graded["publication"], key, value);
        }
        entry["publication"] = std::move(counters);
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
      // Lane H1 (graded.serve_records): the serve_build stage and the fast-path audit.
      std::vector<std::optional<QuoteSliceValue>> fast;
      if (served && stats.serve_records.on && fit.prices) {
        fast = serve_build_slice(stats, members[s], (*fitted)[s].fit,
                                 prepared.at(members[s]).quotes);
      }
      for (std::size_t j = 0; j < indices.size(); ++j) {
        const auto &result = fit.rows[j];
        auto &prediction = predictions[indices[j]];
        if (fit.curve) prediction.actual_curve = to_string(fit.curve->kind());
        prediction.quote_status = to_string(result.quote_status);
        prediction.admitted = result.admitted;
        if (served && result.evaluation_status == QuoteSliceEvaluationStatus::Success) {
          prediction.price = result.fitted_price;
          prediction.vol = result.fitted_iv;
          if (j < fast.size() && fast[j]) { // the record's answer where it differs (H1)
            prediction.price = fast[j]->price;
            prediction.vol = fast[j]->volatility;
          }
          prediction.status = "ok";
          prediction.grade = grade;
          count_into(graded["served_rows"], grade);
          if (ladder) count_into(graded["method_rows"], method);
          ++stats.served;
        } else if (!served) {
          prediction.status = slice.refusal;
        } else if (result.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) {
          if (publish_price_only(result, stats.price_only, prediction)) {
            prediction.grade = grade;
            count_into(graded["price_only_rows"], grade);
            if (ladder) count_into(graded["method_price_only_rows"], method);
            ++stats.price_only_rows;
          }
        } else {
          prediction.status = to_string(result.evaluation_status);
        }
        stamp_metadata(fit, result, prepared.at(members[s]).snapshot, served, prediction);
      }
    }
    if (stats.acceptance_audit && fitted) {
      graded["acceptance_audit"]["schema"] = "atx-vol-final-api-audit-v1";
      graded["acceptance_audit"]["timing_qualifying"] = false;
      graded["acceptance_audit"]["representation"] = "certified_piecewise_linear";
      graded["acceptance_audit"]["snapshots"].push_back(
          quote_bench_acceptance::surface(*fitted, members));
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
  stats.acceptance_audit = args.acceptance_audit;
  const auto dividends = args.dividends == "funding-pv"
      ? QuoteSliceDividendModel::AggregateFundingPresentValue
      : args.dividends == "escrowed-pv" ? QuoteSliceDividendModel::AggregatePresentValue
                                        : QuoteSliceDividendModel::ContinuousYield;
  const auto policy_start = Clock::now(); // B0: configuration, outside the cold clock
  std::optional<quote_bench_policy::LoadedPolicy> policy;
  std::shared_ptr<const QuoteFitProfile> fixed;
  if (args.curve.empty()) {
    policy.emplace(quote_bench_policy::load(args.policy, dividends));
    stats.policy_definition = policy->definition;
    stats.price_only = policy->price_only; // implied by serve.floor_price_only (lane C0)
    stats.floor_price_only = policy->floor_price_only;
    stats.metadata = policy->metadata;
    stats.snapshot = policy->snapshot;
  stats.serve_records = policy->serve_records;
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
  // B0 cold-calibration clock: the resident snapshot is handed to the fit here.
  const auto cold_start = Clock::now();
  stats.policy_load_seconds = bench::seconds_between(policy_start, cold_start);
  stats.phases.start(cold_start);
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
  const bool one_spot = stats.snapshot.one_spot != quote_bench_policy::OneSpot::Off;
  const auto snapshot_spots = one_spot ? choose_snapshot_spots(rows, chains, stats)
                                       : std::map<SpotKey, SnapshotSpot>{};
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
    PreparedChain chain{market, {}, {}, {}, {}};
    chain.quotes.reserve(indices.size());
    for (const auto i : indices) {
      chain.quotes.push_back(rows[i].quote);
      // Quote clock: the chain's latest quote observation; its own spot rides on its quotes.
      chain.snapshot.quote_ns = std::max(chain.snapshot.quote_ns, rows[i].observation_ns);
    }
    chain.snapshot.underlying_ns = chain.snapshot.quote_ns;
    const auto &identity = rows[indices.front()].identity;
    if (one_spot) {
      const auto refused = apply_one_spot(snapshot_spots.at({identity[0], identity[3]}),
                                          dividends, chain_id, chain, stats);
      if (!refused.empty()) {
        for (const auto i : indices) predictions[i].status = refused;
        continue;
      }
    }
    prepared.emplace(chain_id, std::move(chain));
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
  stats.phases.lap(bench::Stage::Normalization);
  if (policy && policy->graded) {
    stats.graded = true;
    // Expiry fan-out of every graded surface fit at the library's AUTO width
    // (QuoteSurfaceConfig::fit_workers = 0). ATX_VOL_FIT_WORKERS=N caps it to
    // N workers (1 = serial); 0, unset or unparseable means
    // hardware_concurrency. Speed only: predictions are byte-identical for any
    // width.
    QuoteSurfaceConfig surface = policy->surface;
    surface.fit_workers = 0U;
    fit_graded(chains, prepared, snapshot_chains, ambiguous_expiries, surface, predictions, stats,
               fitted_chains, static_fallback);
    stats.phases.lap(bench::Stage::Publication);
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
    stats.phases.lap(bench::Stage::Publication); // B0: the previous chain's rows
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
    // B0: under --timing-detail a static fit is timed by its own fit_timing recorder.
    std::optional<counters::fit_timing::ChainRecorder> recorder;
    if (stats.phases.armed()) recorder.emplace();
    stats.phases.lap(bench::Stage::Normalization);
    const auto fit = [&] {
      const counters::fit_timing::ChainScope timing(recorder ? &*recorder : nullptr);
      return schedule == schedules.end()
          ? fit_quote_slice(market, chain.quotes, static_config)
          : fit_with_schedule(chain_id, first, indices.size(), chain, schedule->second, stats);
    }();
    if (recorder) {
      const counters::fit_timing::FitTimingCounters timed = recorder->result();
      charge_fit_lap(stats, {&timed, 1U});
    }
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
        stamp_metadata(*fit, result, chain.snapshot,
                       fit->fit_status == QuoteSliceFitStatus::Success, prediction);
      }
    }
    stats.chain_microseconds.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - chain_start).count());
  }
  stats.phases.lap(bench::Stage::Publication);
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
  stats.phases.lap(bench::Stage::FinalGrid); // B0: the static calendar validation
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
      predictions[index].vol_convention = "none";
      predictions[index].price_only_reason = "none";
      predictions[index].iv_lower = kNaN;
      predictions[index].iv_upper = kNaN;
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
  if (stats.floor_price_only || stats.metadata) {
    // Lane C0: price coverage (ok | ok_price_only) and identified-IV coverage (ok with a
    // volatility the library query identifies, vol_convention not none) are separate; an
    // ok row at the floor is a floor value served with a volatility (zero under
    // serve.floor_price_only), never counted as identified.
    auto &id = stats.identifiability;
    id = {{"floor_price_only", stats.floor_price_only}, {"served_price_rows", 0U},
          {"served_vol_rows", 0U}, {"identified_vol_rows", 0U}, {"price_only_rows", 0U},
          {"identified_floor_vol_rows", 0U}, {"price_only_reason_counts", Json::object()},
          {"vol_convention_counts", Json::object()}};
    for (const auto &prediction : predictions) {
      const bool ok = prediction.status == "ok";
      const bool price_only = prediction.status == "ok_price_only";
      if (ok || price_only) add_count(id, "served_price_rows");
      if (ok && std::isfinite(prediction.vol)) add_count(id, "served_vol_rows");
      if (ok && vol_identified(prediction)) add_count(id, "identified_vol_rows");
      if (ok && prediction.vol <= kIvMin) add_count(id, "identified_floor_vol_rows");
      if (ok) add_count(id["vol_convention_counts"], prediction.vol_convention);
      if (price_only) {
        add_count(id, "price_only_rows");
        add_count(id["price_only_reason_counts"], prediction.price_only_reason);
      }
    }
  }
  const auto cold_end = Clock::now();
  stats.phases.lap(bench::Stage::Publication, cold_end);
  stats.cold_calibration_seconds = bench::seconds_between(cold_start, cold_end);
  stats.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  if (stats.serve_records.on) finish_serve_records(stats);
  return predictions;
}

// Lane C0 metadata columns (serve.metadata): blank where a value is not supplied.
// vol_valid: the published volatility is identified (vol_identified).
void write_metadata(std::ostream &stream, const Prediction &p) {
  const auto number_or_blank = [&](double value) {
    stream << ',';
    if (std::isfinite(value)) stream << value;
  };
  const auto instant = [&](std::int64_t ns) {
    stream << ',';
    if (ns != 0) stream << ns;
  };
  const auto &s = p.snapshot;
  stream << ',' << p.price_only_reason;
  number_or_blank(p.iv_lower);
  number_or_blank(p.iv_upper);
  stream << ',' << p.vol_convention << ',' << p.model_kind << ',' << p.dividend_convention;
  instant(s.quote_ns);
  instant(s.underlying_ns);
  stream << ',' << s.spot_version << ',' << to_string(s.spot_rule) << ',' << s.carry_version << ','
         << (p.model_kind.empty() ? "" : to_string(s.carry_convention));
  number_or_blank(s.raw_spot);
  stream << ',' << (s.spot_stale ? 1 : 0) << ',' << (std::isfinite(p.price) ? 1 : 0) << ','
         << (vol_identified(p) ? 1 : 0);
}

void write_predictions(const std::filesystem::path &path, const std::vector<InputRow> &rows,
                       const std::vector<Prediction> &predictions, bool graded = false,
                       bool metadata = false) {
  std::ofstream stream{path};
  if (!stream) throw std::runtime_error("Cannot open prediction output");
  stream << "row_id,predicted_price,predicted_vol,status,chain_id,quote_status,admitted,"
            "resolved_profile,policy_source,configured_curve,actual_curve,vol_status,model"
         << (graded ? ",grade" : "")
         << (metadata ? ",price_only_reason,iv_lower,iv_upper,vol_convention,model_kind,"
                        "dividend_convention,quote_ns,underlying_ns,spot_version,spot_rule,"
                        "carry_version,carry_convention,raw_spot,spot_stale,price_valid,vol_valid"
                      : "")
         << '\n';
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
    if (metadata) write_metadata(stream, prediction);
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
  if (stats.timing_detail) {
    report["chain_latency_scope"] = "--timing-detail: a graded chain's latency is its own fit "
        "wall (sum of its exclusive fit_timing buckets; surface-level work such as term-prior "
        "construction is in graded_stats.timing_detail surface_wall_us - chain_wall_us); a "
        "static-path chain's latency is its own wall as without the flag";
    report["timing_detail_semantics"] = "graded_stats.slices[].timing: EXCLUSIVE (not "
        "inclusive) wall per bucket in microseconds: a nested scope's time is charged only to "
        "its own bucket, so the buckets partition the chain wall (deam = American IV "
        "inversions of the quotes and the channel "
        "floor/cap prices; solve = point curve solve or channel native/family solve; project = "
        "project_in_channel; price = certification grid reprice; certify = AmericanPriceSlice "
        "certificate, its repairs and row re-implication; calendar = calendar pair checks and "
        "calendar repairs; other = the rest charged to the chain); wall_us is their sum; "
        "al_price_calls / al_by_bucket / iv_newton_iters / al_greeks_calls are the fitting "
        "thread's solve-ledger deltas (AL boundary solves, IV iterations, Greek bundles); "
        "repair_rung_visits / repair_rung_us are a cross-cut, not a bucket: the surface "
        "fitter's repair-rung visits and their INCLUSIVE wall, overlapping the buckets their "
        "refits charge and not part of wall_us; "
        "deam_* (graded.fit_space vol only): the de-Am IV cache inversions, the AL boundary "
        "solves inside them (deam_inversion_al, most per inversion _max), the "
        "graded.deam_inversion premium attempts, their fallbacks to Search by reason (index = "
        "IvPremiumFallback: 1 not applicable, 2 range edge or non-monotone band, 3 no seed, "
        "4 not increasing, 5 vol bound, 6 pricer failure, 7 no convergence) and the largest "
        "last-solve residual in USD of an accepted premium root; deam_audit_* (--deam-audit) "
        "compare each premium answer with Search (fallbacks count as exact pairs): decade "
        "histogram of |dsigma|, bin 0 = 0, bin b = [1e-(17-b), 1e-(16-b)); for differing "
        "pairs the largest accurate-map Newton distance of each answer and the count with "
        "Search farther from the root; "
        "instrumented, timing unqualified unless the host is quiet";
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
    if (stats.graded_stats.contains("cold_fit")) {
      report["graded_semantics"] = report["graded_semantics"].get<std::string>() +
          "; cold_fit joint_feasible_mid: jointly project midpoints of the original boxes "
          "intersected with proven price floors, identify final prices once, then publish grade B "
          "with ProjectedPrice provenance; any failed attempt falls back for the whole surface "
          "with its actual method and typed reason. The reported original quote boxes remain unchanged";
    }
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
    if (stats.graded_ladder) {
      report["graded_semantics"] = report["graded_semantics"].get<std::string>() +
          "; ladder knot-first: pass A serves the term-prior-free channel (knot) fit when it "
          "certifies, is inside the market and has own point evidence, else the point fit "
          "when it certifies and is inside the market (method channel/point, ladder_fallback "
          "certificate/knot_failure/containment), both graded by the unchanged A rule; other "
          "expiries take the channel refits (channel_refit); ladder_fallback none means the knot "
          "slice served, or certified inside the market without own point evidence (the point "
          "fit shares its admission and is refused too) and the channel refits decide; the "
          "repair rung tries the knot fit, then the point fit; no rung serves prices outside "
          "bid/ask on more than "
          "ladder_max_outside_fraction (point fits) or ladder_channel_max_outside_fraction "
          "(channel fits), and at least ladder_min_outside_quotes, of the strictly two-sided "
          "quotes (a refit outside is refused outside_market); calendar: one visit per slice "
          "in the order inside the market under the smaller limit first, grade tier, own or "
          "convexity-repaired prices before channel-repaired or repair-rung prices, more "
          "strictly two-sided quotes, nearer maturity, each "
          "resolved against the higher-confidence slices only: own bounded-move repair, the "
          "in-market point fit of a knot-served slice clean then repaired (ladder_fallback "
          "calendar), the stronger-prior retry, the channel calendar repairs, else refused; "
          "the dominance gate does not run";
    }
  }
  if (stats.price_only) {
    report["price_only_rows"] = stats.price_only_rows;
    report["price_only_semantics"] = "policy price_only: a served row whose certified price "
        "has no identifiable American volatility under its own carry (a repaired node) is "
        "status ok_price_only with that price, a blank vol and vol_status unidentifiable; it is "
        "never counted in served_rows (price-served = ok | ok_price_only, vol-served = ok) and "
        "never becomes a vol row (a later calendar repair refreshes its price only); without "
        "the key such a row is refused invalid_volatility";
  }
  if (stats.floor_price_only || stats.metadata) {
    report["identifiability"] = stats.identifiability;
    report["identifiability_semantics"] = "serve.floor_price_only: every profile's "
        "QuoteSliceConfig::floor_policy is PriceOnly, so a row whose volatility would sit at "
        "the floor (kIvMin) or on a flat exercise-region plateau, or would re-imply only with "
        "no vega, is a PriceOnly row (reason floor_vol, flat_exercise_region, low_vega or "
        "unidentified_interval; IV interval where known) served as ok_price_only with its "
        "certified price and no volatility; served_price_rows = ok | ok_price_only, "
        "served_vol_rows = ok with a volatility, identified_vol_rows = ok with a volatility "
        "query_quote_slice identifies (vol_convention not none), identified_floor_vol_rows "
        "= ok rows at or below kIvMin (a floor value served with a volatility under the "
        "Legacy policy, its vol_convention none; 0 under the key). serve.floor_price_only "
        "implies price_only (policy_definition.price_only true)";
  }
  if (stats.metadata) {
    report["output_conventions"] = {
        {"columns", "price_only_reason,iv_lower,iv_upper,vol_convention,model_kind,"
            "dividend_convention,quote_ns,underlying_ns,spot_version,spot_rule,carry_version,"
            "carry_convention,raw_spot,spot_stale,price_valid,vol_valid"},
        {"vol_convention", "predicted_vol of an ok row: fitted_curve (the fitted American "
            "IV curve at an unmoved certified node) or reimplied_repaired (the certified price "
            "re-implied under the row's own carry and American model); none otherwise, "
            "including an ok row whose volatility query_quote_slice does not identify (the "
            "Legacy floor policy keeps its historical value; price_only_reason and the IV "
            "interval then say why). No row is american_quoting or european_equivalent"},
        {"price_only_reason", "query_quote_slice's reason (and iv_lower / iv_upper) for an "
            "ok_price_only row, or an ok row whose volatility it does not identify; none on "
            "every other row"},
        {"model_kind", "andersen_lake, or discrete_dividend_lattice for a DatedCash route"},
        {"dividend_convention", "QuoteSliceDividendModel of the fit"},
        {"quote_clock", "quote_ns: the chain's latest observation_ns (blank without the "
            "column)"},
        {"underlying_clock", "underlying_ns: under snapshot.one_spot the observation "
            "instant of S* (blank under the median rule); otherwise the chain's latest quote "
            "observation_ns, NOT the spot's own instant (the chain spot is the median over "
            "rows that may be observed at several instants)"},
        {"versions", "spot_version 0 = the chain's own median spot, 1 = the snapshot spot S* "
            "(snapshot.one_spot); spot_rule: the rule that chose the fit's spot - own, "
            "nearest (S* at the observation instant nearest snapshot_ns) or median (every "
            "observation; also a nearest request without observation instants); "
            "carry_version 0 = supplied carry, 1 = re-derived by the forward-preserving "
            "rebase (carry_convention forward_preserving)"},
        {"validity", "raw_spot: the chain's own spot before a rebase; spot_stale: raw_spot "
            "beyond snapshot.stale_spot_bps of S*; price_valid: a published price; vol_valid: "
            "a published volatility that is identified (vol_convention not none)"}};
  }
  if (stats.snapshot.one_spot != quote_bench_policy::OneSpot::Off) {
    report["one_spot"] = stats.one_spot;
    report["one_spot"]["semantics"] = "snapshot.one_spot: every expiry of one underlying "
        "snapshot (underlier, snapshot_ns) is re-expressed on one spot S* "
        "(rebase_quote_slice_spot): spot = S*, and the expiry and every contract yield move by "
        "the same calendar-clock shift so each forward under the run's dividend model is "
        "unchanged (continuous / funding PV ln(S*/S)/T_cal, escrowed PV "
        "ln((S*-PV)/(S-PV))/T_cal). FORWARD-preserving only: American prices are NOT "
        "preserved (early exercise depends on the spot itself). Raw per-chain spots are kept "
        "(chains[].raw_spot); spot_version 1, spot_rule nearest | median (snapshots[].rule; "
        "median_fallback is the median rule), carry_version 1; a chain beyond stale_spot_bps "
        "is flagged (rebased_stale, served) or refused (stale_spot)";
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
  if (!stats.bench.is_null()) report["bench"] = stats.bench; // B0 clocks and receipts
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

// ── Lane B0 benchmark harness (bench_clocks.hpp, docs/benchmark-clocks.md) ─────────

bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Every published field of two prediction sets, prices and vols bit for bit.
bool same_predictions(const std::vector<Prediction> &a, const std::vector<Prediction> &b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const Prediction &x = a[i];
    const Prediction &y = b[i];
    if (!same_bits(x.price, y.price) || !same_bits(x.vol, y.vol) || x.status != y.status ||
        x.quote_status != y.quote_status || x.admitted != y.admitted ||
        x.resolved_profile != y.resolved_profile || x.actual_curve != y.actual_curve ||
        x.vol_status != y.vol_status || x.grade != y.grade) {
      return false;
    }
  }
  return true;
}

Json ledger_json(const counters::ledger::Counts &work) {
  Json out = Json::object();
  for (unsigned i = 0; i < counters::ledger::kCount; ++i) {
    out[counters::ledger::kNames[i]] = work.v[i];
  }
  return out;
}

// SIMD vector invocations and scalar-equivalent lanes come from the exact counter plane,
// compiled out of the shipping build: there they are unavailable, never 0.
Json simd_json(const counters::Snapshot &before, const counters::Snapshot &after) {
  if (!after.enabled) {
    const Json missing = "unavailable: exact counters compiled out (ATX_VOL_COUNTERS off)";
    return {{"vector_pack_dispatches", missing}, {"scalar_equivalent_lanes", missing}};
  }
  using counters::Counter;
  const auto delta = [&](Counter c) { return after.get(c) - before.get(c); };
  return {{"vector_pack_dispatches", delta(Counter::AmericanAvxPackDispatches)},
          {"scalar_equivalent_lanes", delta(Counter::AmericanWrapperKnownScalarLanes) +
                                          delta(Counter::ScalarFallbackLanes)}};
}

// The rows counters of one run (input, admitted, served, price-only, identified IV).
Json rows_json(const std::vector<InputRow> &rows, const std::vector<Prediction> &predictions,
               const RunStatistics &stats) {
  std::size_t eligible = 0;
  std::size_t iv_rows = 0;
  Json by_status = Json::object();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].input_status == "ok") ++eligible;
    const Prediction &p = predictions[i];
    if (p.status == "ok" && std::isfinite(p.vol)) ++iv_rows;
    bump(by_status, p.status);
  }
  return {{"input_rows", rows.size()}, {"supported_input_rows", eligible},
          {"admitted_quotes", stats.admitted}, {"served_rows", stats.served},
          {"price_only_rows", stats.price_only_rows},
          {"served_price_rows", stats.served + stats.price_only_rows},
          {"served_iv_rows", iv_rows}, {"rows_by_status", std::move(by_status)},
          {"fitted_chains", stats.fitted}, {"chains", stats.chains},
          {"publication", {{"rows_published", predictions.size()},
                           {"graded_slice_records", stats.graded_stats.contains("slices")
                               ? stats.graded_stats["slices"].size() : 0U}}}};
}

// --reps in-process repetitions of the quote-slice run, each preceded by the cold reset
// under --cold-reset. Rep 1's statistics and predictions are the run's; every later rep
// must reproduce them bit for bit (predictions_identical_to_rep1).
std::vector<Prediction> run_reps(const Arguments &args, const std::vector<InputRow> &rows,
                                 RunStatistics &stats, Json &out) {
  std::vector<Prediction> first;
  std::vector<double> legacy;
  std::vector<double> cold;
  Json per_rep = Json::array();
  for (std::size_t rep = 0; rep < args.reps; ++rep) {
    const Json reset = args.cold_reset ? bench::cold_reset() : Json(nullptr);
    RunStatistics trial;
    trial.timing_detail = args.timing_detail;
    if (args.timing_detail) trial.phases.arm();
    const std::size_t memo_at_start = bench::deam_memo_entries();
    const auto ledger_before = counters::ledger::snapshot();
    const auto exact_before = counters::snapshot();
    auto predictions = [&] {
      const bench::StageClockArm armed(args.timing_detail ? &trial.deam_clock : nullptr);
      return run(args, rows, trial);
    }();
    const auto work = counters::ledger::snapshot() - ledger_before;
    const auto exact_after = counters::snapshot();
    legacy.push_back(trial.elapsed_seconds);
    cold.push_back(trial.cold_calibration_seconds);
    const bool identical = rep == 0U || same_predictions(first, predictions);
    per_rep.push_back({{"rep", rep + 1U}, {"legacy_fit_reprice_s", trial.elapsed_seconds},
                       {"cold_calibration_s", trial.cold_calibration_seconds},
                       {"policy_load_s", trial.policy_load_seconds},
                       {"deam_memo_entries_at_start", memo_at_start},
                       {"deam_memo_entries_at_end", bench::deam_memo_entries()},
                       {"solve_ledger", ledger_json(work)},
                       {"predictions_identical_to_rep1", identical}, {"cold_reset", reset}});
    if (rep == 0U) {
      out["counters"] = {
          {"rows", rows_json(rows, predictions, trial)}, {"solve_ledger", ledger_json(work)},
          {"families", args.timing_detail ? bench::families_json(trial.fit_work)
                                          : Json("unavailable: requires --timing-detail")},
          {"fit_route", args.timing_detail ? bench::fit_work_json(trial.fit_work)
                                           : Json("unavailable: requires --timing-detail")},
          {"deam_stage_clock", args.timing_detail ? bench::stage_clock_json(trial.deam_clock)
                                                  : Json("unavailable: requires --timing-detail")},
          {"simd", simd_json(exact_before, exact_after)}};
      out["stages"] = args.timing_detail
          ? bench::stages_json(trial.phases, trial.cold_calibration_seconds,
                               trial.fit_work.deam_family_builds > 0U, trial.serve_records.on)
          : Json("unavailable: requires --timing-detail");
      if (args.timing_detail) {
        out["stages"]["rep"] = 1;
        out["stages"]["attribution_valid"] =
            atx_auto_worker_count() == 1U && trial.phases.conflict_seconds() == 0.0;
      }
      stats = std::move(trial);
      first = std::move(predictions);
    }
  }
  out["reps"] = args.reps;
  out["cold_reset"] = args.cold_reset;
  out["per_rep"] = std::move(per_rep);
  out["clocks"] = {
      {"legacy_fit_reprice_s",
       {{"summary", bench::summary_json(bench::summarize(legacy))},
        {"scope", "the historical quote-bench timer (fit_and_reprice_seconds): preparation "
                  "after classification and grouping, routing, fit and repricing"}}},
      {"cold_calibration_s",
       {{"summary", bench::summary_json(bench::summarize(cold))},
        {"scope", "resident snapshot handed to the fit -> validated surface and predictions "
                  "published: classification, grouping, chain markets, routing, carry, "
                  "fits, inverse memories, certification, calendar validation; excludes "
                  "file I/O and the policy load"}}},
      {"ready_to_quote_s",
       stats.serve_records.on
           ? Json{{"status", "timed"},
                  {"serve_build_s", stats.serve_build_seconds},
                  {"first_usable_us",
                   bench::summary_json(bench::summarize(stats.serve_first_usable_us))},
                  {"final_us", bench::summary_json(bench::summarize(stats.serve_total_us))},
                  {"scope", "rep 1's serve_build stage (graded.serve_records): every served "
                            "slice's serving record built after its fit and published "
                            "(first usable bundle, then the laddered record); inside "
                            "cold_calibration_s, separated as the serve_build stage"}}
           : Json{{"status", "not_built"},
                  {"reason", "quote-bench builds no serving tier (records, ladders, primary "
                             "risk), so ready-to-quote is not measured; it is never reported "
                             "as zero"}}}};
  return first;
}

// Process clock and receipts of a quote-slice run (bench object of the report).
struct ProcessTimes {
  double startup_s{};
  double setup_s{};
  ReadTimes input{};
  double compute_s{};
  double write_s{};
  double process_wall_s{};
};

Json process_json(const ProcessTimes &t, const RunStatistics &stats) {
  const double attributed =
      t.startup_s + t.setup_s + t.input.read_s + t.input.decode_s + t.compute_s + t.write_s;
  return {{"value", std::isfinite(t.process_wall_s) ? Json(t.process_wall_s) : Json(nullptr)},
          {"status", std::isfinite(t.startup_s) ? "measured" : "startup unavailable"},
          {"sub_timers", {{"startup_s", std::isfinite(t.startup_s) ? Json(t.startup_s)
                                                                  : Json(nullptr)},
                          {"setup_s", t.setup_s}, {"read_s", t.input.read_s},
                          {"decode_s", t.input.decode_s}, {"compute_s", t.compute_s},
                          {"policy_load_s", stats.policy_load_seconds}, {"write_s", t.write_s}}},
          {"unattributed_s", std::isfinite(t.process_wall_s) ? Json(t.process_wall_s - attributed)
                                                             : Json(nullptr)},
          {"scope", "process launch -> predictions written. startup = kernel process creation "
                    "-> main; setup = arguments and pinning; read = open, header and getline; "
                    "decode = record parse and admission; compute = every rep (policy load "
                    "included, reported separately); write = predictions CSV. The report "
                    "writes themselves are excluded (they carry this value)"}};
}

// Lane K2 (r5): the replayed inverse stream (bench.inverse_replay). Five replays of the
// same recorded stream: replay 1 is the stop/go value (a fresh scratch, the tables not yet
// touched by a replay), replays 2-5 show the spread.
Json inverse_replay_json(const iv_batch::InverseStream &stream, const std::string &isa_name) {
  namespace ib = iv_batch;
  const ib::InverseIsa isa = isa_name == "scalar" ? ib::InverseIsa::Scalar
                             : isa_name == "avx2" ? ib::InverseIsa::Avx2
                                                  : ib::InverseIsa::Auto;
  constexpr int kReplays = 5;
  std::vector<double> totals;
  Json runs = Json::array();
  ib::InverseReplay first;
  for (int i = 0; i < kReplays; ++i) {
    const ib::InverseReplay r = ib::replay_inverse_stream(stream, isa);
    totals.push_back(r.total_seconds);
    runs.push_back({{"total_s", r.total_seconds}, {"seed_s", r.seed_seconds},
                    {"batch_s", r.batch_seconds}});
    if (i == 0) first = r;
  }
  const ib::InverseBatchWork &w = first.work;
  Json by_status = Json::object();
  Json fit_by_status = Json::object();
  for (std::size_t s = 0; s < ib::kInverseStatusCount; ++s) {
    const char *name = ib::inverse_status_name(static_cast<ib::InverseStatus>(s));
    by_status[name] = w.by_status[s];
    fit_by_status[name] = first.fit_by_status[s];
  }
  const double slots = static_cast<double>(w.vector_steps) * ib::kInverseLaneWidth;
  const auto ratio = [](double a, double b) { return b > 0.0 ? Json(a / b) : Json(nullptr); };
  return {
      {"isa_requested", isa_name},
      {"avx2", w.avx2},
      {"requests", first.requests},
      {"with_family", first.with_family},
      {"without_family", first.without_family},
      {"tables", stream.tables.size()},
      {"table_failures", stream.table_failures},
      {"record_failures", stream.record_failures},
      {"groups", first.groups},
      {"max_group", first.max_group},
      {"anchored", first.anchored},
      {"seeds", first.seeds},
      {"seed_failures", first.seed_failures},
      {"aggregate_inverse_s", first.total_seconds},
      {"seed_s", first.seed_seconds},
      {"batch_s", first.batch_seconds},
      {"replays", runs},
      {"replay_total_summary", bench::summary_json(bench::summarize(totals))},
      {"work", {{"active_lanes", w.active_lanes}, {"evaluations", w.evaluations},
                {"vector_steps", w.vector_steps}, {"vector_lane_slots", w.vector_lane_slots},
                {"scalar_evaluations", w.scalar_evaluations},
                {"q_contractions", w.q_contractions}, {"tile_reuses", w.tile_reuses},
                {"seed_failed_lanes", w.seed_failed}, {"seed_clamped_lanes", w.seed_clamped},
                {"anchored_lanes", w.anchored}}},
      {"lane_utilization", ratio(static_cast<double>(w.vector_lane_slots), slots)},
      {"evaluations_per_request", ratio(static_cast<double>(w.evaluations),
                                        static_cast<double>(w.active_lanes))},
      {"evaluations_per_converged_root",
       ratio(static_cast<double>(w.evaluations),
             static_cast<double>(w.by_status[static_cast<std::size_t>(
                 ib::InverseStatus::Converged)]))},
      {"us_per_request", ratio(1.0e6 * first.total_seconds,
                               static_cast<double>(first.with_family))},
      {"by_status", by_status},
      {"fit_native", {{"by_status", fit_by_status}, {"compared", first.fit_compared},
                      {"status_equal", first.fit_status_equal},
                      {"both_converged", first.fit_both_converged},
                      {"max_abs_dsigma", first.fit_max_dsigma}}},
      {"scope", "every Premium inverse request the run made (refusals included), recorded "
                "in arrival order; the native-eligible ones (a family-native family) are "
                "replayed per family through iv_batch::invert_many with the bid / ask "
                "anchored to their mid; aggregate_inverse_s = the Black-76 seeds plus the "
                "invert_many calls of replay 1 (steady clock), grouping and tables excluded"}};
}

void pin_fit_worker(const Arguments &args) {
  bench::pin_to_logical_cpu(*args.affinity);
  if (!args.require_pcore) return;
  const auto performance = bench::is_performance_cpu(*args.affinity);
  if (!performance.has_value()) {
    throw std::runtime_error("--require-pcore: the core topology is unavailable");
  }
  if (!*performance) {
    throw std::runtime_error("--require-pcore: logical CPU " + std::to_string(*args.affinity) +
                             " is not on a performance core");
  }
}

} // namespace

int main(int argc, char **argv) {
  // B0 process clock: kernel creation -> here is the startup sub-timer; every later one is
  // on the monotonic clock from main entry.
  ProcessTimes times;
  times.startup_s = bench::seconds_since_process_creation();
  const auto main_entry = Clock::now();
  try {
    const auto args = arguments(argc, argv);
    if (args.help_requested) return 0;
    if (args.affinity) pin_fit_worker(args);
    times.setup_s = bench::seconds_between(main_entry, Clock::now());
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
    const auto rows = read_rows(args, times.input);
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
    stats.timing_detail = args.timing_detail;
    counters::fit_timing::arm_deam_audit(args.deam_audit); // before any fit starts
    Json bench_report = {{"schema", "atx-vol-bench-clocks-v2"},
                         {"definitions", "atx-vol/docs/benchmark-clocks.md"}};
    const auto cpus_before = bench::sample_cpus();
    iv_batch::InverseStream inverse_stream; // lane K2: armed only under --inverse-replay
    if (args.inverse_replay) iv_batch::inverse_stream_arm(&inverse_stream);
    const auto compute_start = Clock::now();
    const auto predictions = run_reps(args, rows, stats, bench_report);
    times.compute_s = bench::seconds_between(compute_start, Clock::now());
    iv_batch::inverse_stream_arm(nullptr);
    if (args.inverse_replay) {
      bench_report["inverse_replay"] = inverse_replay_json(inverse_stream, *args.inverse_replay);
    }
    const auto cpus_after = bench::sample_cpus();
    const auto write_start = Clock::now();
    write_predictions(args.output, rows, predictions, stats.graded, stats.metadata);
    const auto outputs_ready = Clock::now();
    times.write_s = bench::seconds_between(write_start, outputs_ready);
    times.process_wall_s = times.startup_s + bench::seconds_between(main_entry, outputs_ready);
    bench_report["clocks"]["process_wall_s"] = process_json(times, stats);
    bench_report["affinity"] =
        bench::affinity_json(args.affinity, args.require_pcore, cpus_before, cpus_after);
    bench_report["build_info"] = bench::build_info_json();
    bench_report["process_identity"] = bench::process_identity_json();
    stats.bench = std::move(bench_report);
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
