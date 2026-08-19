// atx-vol-oracle-bench — SpiderRock parity benchmark, Mode A
// (bench/oracle/CHARTER.md stage 2).
//
// Input: one or more cohorts (bench/oracle/cohorts/README.md schema) selecting
// partitions of the parquet oracle store. Only the cohort-named
// date=<d>/bucket_et=<b> dirs are opened (oracle_cohort_reader.cpp — pruning
// by construction), with an exact undSecKey_tk predicate pushed into each
// file scan. Mode A prices every admitted row through the PUBLIC atx-vol
// American API using SpiderRock's own inputs (uPrc, rate, sdiv, ddiv, years;
// vol = srVol) and compares vol identity + price + greeks to srVol / srPrc /
// de ga th ve rh ph vo va deDecay under the oracle_conventions.* unit map.
//
// TWO OUTPUT SHAPES, selected by --aggregate-only:
//   default        the charter cell-key scorecard JSON to --out; rows/s and
//                  the opened partitions to stderr.
//   --aggregate-only  ONLY run-level aggregates (the eleven charter target
//                  metrics, or rows/s under --benchmark-speed) to --out, or
//                  to stdout when --out is absent. NOTHING per-cell, no cell
//                  keys, no partition list, no underlier or date — because the
//                  downstream analyst stage is deliberately tool-less and must
//                  never receive frozen holdout MEMBERSHIP. That confidentiality
//                  boundary is the reason the flag exists; treat any new field
//                  added under it as membership until proven otherwise.
//
// ACCESS MECHANISM FOR THE GATE TEST: tests/oracle_bench_test.cpp reaches
// run_oracle_bench/parse_bench_args by `#define ATX_ORACLE_BENCH_NO_MAIN` +
// a textual #include of THIS file (the bev_label_factory_gate_test.cpp
// pattern — a CLI-only driver TU with no reuse value beyond this binary does
// not warrant a header). That test TU is the only place this file is
// #included, so there is no ODR hazard.

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp" // Mode B: AMERICAN vol inversion
#include "oracle_cohort_reader.hpp"
#include "oracle_convention_sweep.hpp"
#include "oracle_conventions.hpp"
#include "oracle_host_probe.hpp"
#include "oracle_scorecard.hpp"

namespace atx::vol::oracle {

using atx::core::Ok; // Err resolves via ADL (Error argument); Ok's arguments
                     // are not atx::core types, so the explicit using is
                     // needed — same convention as track_store.cpp.

namespace {
constexpr std::string_view kUsage =
    "usage: atx-vol-oracle-bench --cohort <name[,name...]|cohort.json> [--mode A|B]\n"
    "                            [--scorecard] [--benchmark-speed] [--aggregate-only]\n"
    "                            [--preset NAME] [--quiet-host] [--store <store-root>]\n"
    "                            [--out <out.json>] [--iter N] [--git-sha SHA]\n"
    "   or: atx-vol-oracle-bench --convention-sweep --smoke <smoke.json>\n"
    "                            --tune <tune.json> --store <store-root>\n"
    "                            --out <sweep.json> --git-sha <SHA>\n"
    "\n"
    "--store defaults to the licensed aggregate store; --out defaults to stdout.\n"
    "Both stay REQUIRED without --aggregate-only.\n";

// The licensed aggregate store every oracle gate reads. Single source with
// $script:OracleStoreRoot in scripts/oracle-targeted-gate.ps1 — a second value
// here would silently benchmark a different corpus than the gate validates.
constexpr std::string_view kDefaultStoreRoot = "C:\\atx-cache\\oracle\\spiderrock";

// Where a bare --cohort NAME resolves, relative to the repository root.
constexpr std::string_view kCohortDirSuffix = "atx-vol/bench/oracle/cohorts";

// Bound on the upward walk that finds the repository root. The deepest real
// caller is <repo>/build-rel-avx2/bin/x.exe (3), so this is slack, not a limit
// anyone reaches — and it makes the loop statically bounded (JPL rule 2).
constexpr int kMaxRepoDepth = 12;
} // namespace

enum class BenchMode : std::uint8_t { A, B };

[[nodiscard]] inline std::string_view mode_token(BenchMode mode) noexcept {
  return mode == BenchMode::A ? "A" : "B";
}

// Lower-cased mode key used for scorecard cell prefixes and metric ids.
[[nodiscard]] inline std::string_view mode_key(BenchMode mode) noexcept {
  return mode == BenchMode::A ? "a" : "b";
}

struct BenchArgs {
  // Resolved cohort manifest paths, in command-line order. A --cohort list
  // ("smoke,tune") contributes one entry per element; the run is ONE pass over
  // their concatenated rows.
  std::vector<std::string> cohort_paths;
  std::string smoke_path;
  std::string tune_path;
  std::string store_root;
  std::string out_path;
  std::string git_sha = "unknown";
  // Build preset the caller claims to be measuring. Provenance only — this
  // binary cannot verify how it was compiled, so it records the claim and never
  // acts on it, except to name the speed metric (speed_metric_id).
  std::string preset;
  std::int64_t iter = 0;
  BenchMode mode = BenchMode::A;
  bool convention_sweep = false;
  bool scorecard = false;
  bool aggregate_only = false;
  bool benchmark_speed = false;
  bool quiet_host = false;
};

// A --cohort element is a NAME when it is a bare token: no directory separator,
// no drive colon, no ".json" suffix. Everything else is a PATH used VERBATIM,
// which is what keeps the Stage 3 gates' absolute --cohort arguments working
// byte-for-byte.
[[nodiscard]] inline bool is_cohort_name(std::string_view spec) noexcept {
  return !spec.empty() && spec.find_first_of("/\\:") == std::string_view::npos &&
         !spec.ends_with(".json");
}

// The directory holding the named cohort manifests, found by walking up from
// `start`. Empty when no ancestor carries atx-vol/bench/oracle/cohorts.
[[nodiscard]] inline std::filesystem::path find_cohort_dir(const std::filesystem::path &start) {
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::absolute(start, ec);
  if (ec) {
    return {};
  }
  for (int depth = 0; depth < kMaxRepoDepth && !dir.empty(); ++depth) {
    const std::filesystem::path candidate = dir / kCohortDirSuffix;
    if (std::filesystem::is_directory(candidate, ec)) {
      return candidate;
    }
    const std::filesystem::path parent = dir.parent_path();
    if (parent == dir) {
      break; // filesystem root: the walk is over, not merely bounded
    }
    dir = parent;
  }
  return {};
}

// The cohort manifest directory for a real invocation: the executable's own
// tree first (a gate that resolves the binary off PATH has an arbitrary CWD),
// then the working directory. Empty when neither carries one — bare names then
// fail at parse with a message that says so.
[[nodiscard]] inline std::filesystem::path discover_cohort_dir(std::string_view argv0) {
  if (!argv0.empty()) {
    const std::filesystem::path exe{argv0};
    const std::filesystem::path from_exe = find_cohort_dir(exe.parent_path());
    if (!from_exe.empty()) {
      return from_exe;
    }
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path{} : find_cohort_dir(cwd);
}

[[nodiscard]] inline Result<std::string> resolve_cohort_spec(std::string_view spec,
                                                             const std::filesystem::path &dir) {
  if (!is_cohort_name(spec)) {
    return Ok(std::string{spec});
  }
  if (dir.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "--cohort: '" + std::string{spec} +
                   "' is a cohort NAME but " + std::string{kCohortDirSuffix} +
                   " was not found from the executable or the working directory");
  }
  const std::filesystem::path manifest = dir / (std::string{spec} + ".json");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(manifest, ec)) {
    return Err(ErrorCode::InvalidArgument,
               "--cohort: unknown cohort name '" + std::string{spec} + "': no " +
                   manifest.string());
  }
  return Ok(manifest.string());
}

// Splits one --cohort value on ',' and appends each resolved manifest path.
// Rejects an empty element and a repeat: a cohort counted twice would double
// its weight in every aggregate without saying so.
[[nodiscard]] inline Status append_cohort_specs(std::string_view value,
                                                const std::filesystem::path &dir,
                                                std::vector<std::string> &out) {
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t comma = value.find(',', begin);
    const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
    const std::string_view spec = value.substr(begin, end - begin);
    if (spec.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "--cohort: empty element in list: " + std::string{value});
    }
    ATX_TRY(std::string path, resolve_cohort_spec(spec, dir));
    for (const std::string &seen : out) {
      if (seen == path) {
        return Err(ErrorCode::InvalidArgument, "--cohort: named twice: " + path);
      }
    }
    out.push_back(std::move(path));
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1;
  }
  return Ok();
}

// `cohort_dir` resolves bare --cohort NAMES; pass an empty path to admit only
// filesystem paths (what the unit tests do when they are not exercising name
// resolution). Callers that want the repository's own manifests pass
// discover_cohort_dir(argv[0]).
[[nodiscard]] Result<BenchArgs> parse_bench_args(std::span<const std::string> args,
                                                 const std::filesystem::path &cohort_dir) {
  BenchArgs out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string &flag = args[i];
    const bool has_value = i + 1 < args.size();
    auto take = [&]() -> const std::string & { return args[++i]; };
    if (flag == "--cohort" && has_value) {
      ATX_TRY_VOID(append_cohort_specs(take(), cohort_dir, out.cohort_paths));
    } else if (flag == "--mode" && has_value) {
      const std::string &value = take();
      if (value == "A") {
        out.mode = BenchMode::A;
      } else if (value == "B") {
        out.mode = BenchMode::B;
      } else {
        return Err(ErrorCode::InvalidArgument, "--mode: expected A or B, got: " + value);
      }
    } else if (flag == "--scorecard") {
      out.scorecard = true;
    } else if (flag == "--aggregate-only") {
      out.aggregate_only = true;
    } else if (flag == "--benchmark-speed") {
      out.benchmark_speed = true;
    } else if (flag == "--quiet-host") {
      out.quiet_host = true;
    } else if (flag == "--preset" && has_value) {
      out.preset = take();
    } else if (flag == "--smoke" && has_value) {
      out.smoke_path = take();
    } else if (flag == "--tune" && has_value) {
      out.tune_path = take();
    } else if (flag == "--convention-sweep") {
      out.convention_sweep = true;
    } else if (flag == "--store" && has_value) {
      out.store_root = take();
    } else if (flag == "--out" && has_value) {
      out.out_path = take();
    } else if (flag == "--git-sha" && has_value) {
      out.git_sha = take();
    } else if (flag == "--iter" && has_value) {
      const std::string &v = take();
      std::int64_t iter = 0;
      const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), iter);
      if (ec != std::errc{} || ptr != v.data() + v.size()) {
        return Err(ErrorCode::InvalidArgument, "--iter: not an integer: " + v);
      }
      out.iter = iter;
    } else {
      return Err(ErrorCode::InvalidArgument, "unknown or valueless flag: " + flag);
    }
  }
  if (out.convention_sweep) {
    if (out.mode != BenchMode::A || out.scorecard || out.aggregate_only || out.benchmark_speed ||
        out.quiet_host || !out.preset.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "--convention-sweep admits none of --mode/--scorecard/--aggregate-only/"
                 "--benchmark-speed/--quiet-host/--preset");
    }
    if (!out.cohort_paths.empty() || out.smoke_path.empty() || out.tune_path.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "convention sweep requires --smoke and --tune, without --cohort");
    }
  } else {
    if (out.cohort_paths.empty() || !out.smoke_path.empty() || !out.tune_path.empty()) {
      return Err(ErrorCode::InvalidArgument, "scorecard mode requires only --cohort");
    }
    if (out.scorecard && out.benchmark_speed) {
      return Err(ErrorCode::InvalidArgument,
                 "--scorecard and --benchmark-speed publish different things; pick one");
    }
  }
  // --store/--out become optional ONLY on an aggregate-only run: that shape
  // defaults the store to the licensed root and writes to stdout. The two
  // LEGACY shapes (per-cell scorecard file, convention sweep) still require
  // both, so a caller cannot silently lose the artifact they expect on disk.
  if (!out.aggregate_only && (out.store_root.empty() || out.out_path.empty())) {
    return Err(ErrorCode::InvalidArgument, "--store and --out are required");
  }
  if (out.store_root.empty()) {
    out.store_root = std::string{kDefaultStoreRoot};
  }
  return Ok(std::move(out));
}

// ── Aggregates ───────────────────────────────────────────────────────────
//
// The run-level form of the eleven charter target metrics. Definitions are the
// convention sweep's, reused rather than restated (oracle_convention_sweep.hpp):
// price and vol are ABSOLUTE mean errors, the nine Greeks are the REPORTED
// relative error |m - o| / max(|o|, kSelectionAbsFloor). A second definition of
// "price MAE" in this file is exactly how the sweep receipt and the aggregate
// receipt would come to disagree about the same name.
struct AggregateMetrics {
  Accumulator price; // oracle price units
  Accumulator vol;   // decimal vol
  std::array<Accumulator, 9> greeks{};
};

// Registry order of AggregateMetrics::greeks. The "mode_<a|b>_" prefix is
// applied at emit time; the ids below are the workflow's TARGET_REGISTRY
// suffixes verbatim (.claude/workflows/vol-oracle-iter.js).
constexpr std::array<std::string_view, 9> kGreekMetricSuffix{
    "delta_rel", "gamma_rel", "theta_rel", "vega_rel",  "rho_rel",
    "phi_rel",   "volga_rel", "vanna_rel", "delta_decay_rel"};

// The scorecard cell metric token for the same slot, in the SAME order. Two
// arrays rather than one table of pairs would be a way for the aggregate and
// the cell to drift apart, so OracleBenchAggregate pins the correspondence.
constexpr std::array<std::string_view, 9> kGreekCellMetric{"de", "ga", "th", "ve", "rh",
                                                           "ph", "vo", "va", "deDecay"};

namespace {
// Derived from the scorecard's own tick size so a retuned tick moves the
// published ticks number instead of leaving a stale literal behind.
constexpr double kTicksPerPriceUnit = 1.0 / kPriceTick;
constexpr double kBpPerVolUnit = 10000.0;

void append_json_string(std::string &out, std::string_view text) {
  out.push_back('"');
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(ch));
      out.append(buffer);
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
}

void append_json_int(std::string &out, std::int64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
  out.append(buffer);
}

// PRECONDITION: `value` is finite. Every caller below screens it first, because
// `%.17g` renders inf/nan as bare tokens that are not JSON — a defect that
// would surface three layers away as "receipt is not JSON".
void append_json_double(std::string &out, double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%.17g", value);
  out.append(buffer);
}

struct AggregateMetric {
  std::string metric_id;
  double value = 0.0;
  std::int64_t count = 0;
  std::string_view unit;
};

void append_metric(std::string &out, const AggregateMetric &metric, bool first) {
  if (!first) {
    out.push_back(',');
  }
  out.append("\n    {\"metric_id\": ");
  append_json_string(out, metric.metric_id);
  out.append(", \"value\": ");
  append_json_double(out, metric.value);
  out.append(", \"count\": ");
  append_json_int(out, metric.count);
  out.append(", \"unit\": ");
  append_json_string(out, metric.unit);
  out.push_back('}');
}
} // namespace

// `rel-avx2` -> `rel_avx2_rows_per_second`, the id every layer above already
// names (SPEED_METRIC_ID in .claude/workflows/vol-oracle-iter.js,
// ConvertFrom-OracleSpeed in scripts/oracle-targeted-gate.ps1). DERIVED from
// --preset rather than hard-coded so a run built with a different preset cannot
// publish its number under rel-avx2's id.
[[nodiscard]] inline std::string speed_metric_id(std::string_view preset) {
  if (preset.empty()) {
    return "rows_per_second";
  }
  std::string id;
  id.reserve(preset.size() + 17);
  for (const char ch : preset) {
    const bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= 'A' && ch <= 'Z');
    id.push_back(alnum ? ch : '_');
  }
  id.append("_rows_per_second");
  return id;
}

// The eleven target metrics for one mode, or Err NAMING the first that admitted
// no observation: an empty Accumulator means infinity, and a receipt carrying a
// bare `inf` is not JSON.
[[nodiscard]] inline Result<std::vector<AggregateMetric>>
target_metrics(BenchMode mode, const AggregateMetrics &agg) {
  const std::string prefix = "mode_" + std::string{mode_key(mode)} + "_";
  std::vector<AggregateMetric> metrics;
  metrics.reserve(11);
  metrics.push_back({prefix + "price_mae", agg.price.mean() * kTicksPerPriceUnit,
                     agg.price.count, "ticks"});
  metrics.push_back({prefix + "vol_mae", agg.vol.mean() * kBpPerVolUnit, agg.vol.count, "bp"});
  for (std::size_t i = 0; i < kGreekMetricSuffix.size(); ++i) {
    metrics.push_back({prefix + std::string{kGreekMetricSuffix[i]}, agg.greeks[i].mean(),
                       agg.greeks[i].count, "relative"});
  }
  for (const AggregateMetric &metric : metrics) {
    if (metric.count <= 0 || !std::isfinite(metric.value)) {
      return Err(ErrorCode::Internal, "aggregate metric observed no row: " + metric.metric_id);
    }
  }
  return Ok(std::move(metrics));
}

// ── Mode seam ────────────────────────────────────────────────────────────
//
// A mode maps the cohort's admitted rows to model outputs and records
// per-metric observations into the scorecard under its own <mode> key prefix,
// plus the run-level aggregates --aggregate-only publishes. This interface IS
// the Mode B boundary: Mode B (fit from raw NBBO per underlier x expiry x
// bucket, a later charter stage) implements this same contract — it is
// batch-level ON PURPOSE, because Mode B needs cross-row fitting context a
// per-row hook could not provide. Per charter mandate there is NO Mode B stub
// here; the seam is the contract alone, and --mode B fails at RUN time rather
// than returning a number the loop could ratchet on.
class ModeRunner {
public:
  virtual ~ModeRunner() = default;
  ModeRunner(const ModeRunner &) = delete;
  ModeRunner &operator=(const ModeRunner &) = delete;
  ModeRunner(ModeRunner &&) = delete;
  ModeRunner &operator=(ModeRunner &&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  // Prices/evaluates every row, adding observations to `card` and `agg`, and
  // row accounting to `stats` (rows_priced / rows_engine_error; the reader-side
  // skip counts are already in `stats` when this is called).
  [[nodiscard]] virtual Status run(std::span<const OracleRow> rows, Scorecard &card,
                                   ModeStats &stats, AggregateMetrics &agg) = 0;

protected:
  ModeRunner() = default;
};

// The Andersen-Lake accuracy rung Mode A prices with: the fast preset
// (~1e-4 price accuracy) sits two orders below the 1-tick price tolerance
// while keeping the smoke cohort at seconds. Iteration 0 may retune this when
// it measures the convention-residual floor.
[[nodiscard]] inline std::optional<AlOpts> mode_a_al_opts() { return al_fast_opts(); }

class ModeARunner final : public ModeRunner {
public:
  ModeARunner() = default;

  [[nodiscard]] std::string_view name() const noexcept override { return "a"; }

  [[nodiscard]] Status run(std::span<const OracleRow> rows, Scorecard &card, ModeStats &stats,
                           AggregateMetrics &agg) override {
    for (const OracleRow &row : rows) {
      // ONE entry point for inputs, exercise-style routing, Greeks and the
      // carry leg — the same `mode_a_price_row` the Stage 3 sweep resolves the
      // map with, so production cannot price with a different pricer than the
      // one the published floor was measured on.
      const Result<ModeAPricing> priced =
          mode_a_price_row(row, winning_convention(), mode_a_al_opts());
      if (!priced.has_value()) {
        // Expected on corner regimes (double-continuation, degenerate T/sigma
        // the reader could not know about): counted, not fatal.
        ++stats.rows_engine_error;
        continue;
      }
      const EnginePricingInputs &in = priced->inputs;
      const AmericanGreeks &greeks = priced->greeks;
      ++stats.rows_priced;

      const MoneynessBand mband = moneyness_band(row.strike / row.uprc, in.side);
      const DteBand dband = dte_band(dte_days(row.years));
      auto observe = [&](std::string_view metric, double model, double oracle_val, double tol) {
        const double err = model - oracle_val;
        if (!std::isfinite(err)) {
          return; // belt-and-suspenders: a non-finite comparison poisons no cell
        }
        card.observe(name(), metric, mband, dband, in.side, err, std::abs(err) <= tol);
      };

      const double model_price = price_to_oracle_units(greeks.price);
      observe("price", model_price, row.sr_prc, price_tolerance(row.bid_prc, row.ask_prc));
      agg.price.absolute(model_price, row.sr_prc);
      // Mode A deliberately prices at SpiderRock's own vol. Record that
      // identity as a first-class aggregate instead of letting the target
      // registry claim vol coverage without scorecard provenance.
      observe("vol", in.sigma, row.sr_vol, vol_tolerance());
      agg.vol.absolute(in.sigma, row.sr_vol);

      // ph rides the carry-greeks route; on the American leg its corner-regime
      // failures leave dp_dq non-finite and skip ONLY the ph observation (cell
      // n per metric reflects it), never the row.
      const OracleUnitGreeks g = to_oracle_units(greeks, priced->dp_dq);
      // Slot order is kGreekMetricSuffix's / kGreekCellMetric's.
      const std::array<std::pair<double, double>, 9> greek_pairs{
          std::pair{g.de, row.de}, std::pair{g.ga, row.ga}, std::pair{g.th, row.th},
          std::pair{g.ve, row.ve}, std::pair{g.rh, row.rh}, std::pair{g.ph, row.ph},
          std::pair{g.vo, row.vo}, std::pair{g.va, row.va},
          std::pair{g.de_decay, row.de_decay}};
      for (std::size_t i = 0; i < greek_pairs.size(); ++i) {
        const auto [model, oracle_val] = greek_pairs[i];
        observe(kGreekCellMetric[i], model, oracle_val, greek_tolerance(oracle_val));
        agg.greeks[i].relative(model, oracle_val); // NaN ph self-skips
      }
    }
    return Ok();
  }
};

// ── Mode B: volatility MEASURED from raw NBBO ────────────────────────────
//
// Mode A prices AT srVol, so the vol it reports back is the vol it was handed
// and mode_a_vol_mae is 0 by identity — worthless as evidence. Mode B is where
// vol becomes a real measurement: for each `underlier x expiry x bucket` group
// it inverts the group's own NBBO mids to volatility, then prices price + all
// nine Greeks FROM that fitted vol through the same pinned convention map Mode
// A uses. The only difference between the two modes is where sigma comes from.
//
// WHAT IS INVERTED, AND WHY IT IS NOT "LET'S BE RATIONAL". These are AMERICAN
// equity options. Jäckel's four-branch/Householder(3) inversion is exact and
// fast for the BLACK forward map and is the right tool there, but running it
// against an American premium inverts the wrong function: the early-exercise
// premium is charged to volatility, so it returns a plausible, biased-low
// sigma that re-prices to the wrong number. Every layer downstream would then
// inherit a fiction that looks like a measurement. This uses the library's
// `american_implied_vol` (american_iv.hpp) instead — a safeguarded Newton
// (rtsafe) whose forward map is the SAME Andersen-Lake pricer, at the SAME
// `al_fast_opts` rung, that this file prices with. Because forward and inverse
// share one map, the round trip is self-consistent by construction rather than
// by assumption, and it is asserted per row below.
//
// WHY THE SEAM IS BATCH-LEVEL. The group is the unit of work, not the row: it
// pins one snapshot's pricing context, it orders the chain by strike so each
// inversion warm-starts from its neighbour's converged root (documented to
// move only the Newton PATH, never the root), and it is the scope the charter
// states the fit in. A per-row hook could provide none of that.
//
// NO SMILE MODEL, DELIBERATELY. The vol published per row is the vol inverted
// from THAT row's own mid — not a smoothed value read off a curve fitted to
// the group. A smoothed vol would be a modelled quantity reported as a
// measurement, and it would let a bad row borrow credibility from its
// neighbours. Iteration 1+ may add a smile; iteration 0 must be able to say
// exactly what each number is.
//
// EXPIRY KEY: the exact `years` double, compared by IEEE BIT PATTERN. Rows
// carry no expiry date. Within one (date, bucket) partition `years` is computed
// from a single snapshot timestamp to the contract's expiry, so it is discrete
// per expiry and byte-identical across every contract on that expiry. Bit
// equality is therefore exact where an epsilon compare would be a guess in both
// directions — wide enough to merge two nearby expiries, narrow enough to split
// one. If ingest ever perturbed `years` per row this key would over-split, which
// shows up honestly as more groups and fewer rows each, never as a wrong fit.
struct ModeBStats {
  // Groups fitted: distinct (date, bucket_et, underlier, years) keys. A COUNT,
  // never the keys themselves — partition names are membership.
  std::int64_t groups_fitted = 0;
  // ── Rows that yielded NO volatility, by reason. Every one of these is an
  // honest refusal that is counted and published, never a silent 0 or NaN: a
  // fabricated vol is strictly worse than a missing one, because the oracle
  // loop ratchets on whatever it is given.
  std::int64_t rows_no_quote = 0;          // NBBO absent, crossed, or non-finite
  std::int64_t rows_below_lo_bound = 0;    // mid at/under the zero-vol American floor
  std::int64_t rows_above_up_bound = 0;    // mid at/over the no-arbitrage ceiling
  std::int64_t rows_iv_no_solution = 0;    // inverter returned Err (incl. T <= 0)
  std::int64_t rows_iv_at_floor = 0;       // inverter reported kIvMin: the clamp, not a root
  std::int64_t rows_vega_below_floor = 0;  // quote cannot resolve a vol at all
  std::int64_t rows_round_trip_failed = 0; // re-pricing the fitted vol missed the mid

  [[nodiscard]] std::int64_t rows_unfitted() const noexcept {
    return rows_no_quote + rows_below_lo_bound + rows_above_up_bound + rows_iv_no_solution +
           rows_iv_at_floor + rows_vega_below_floor + rows_round_trip_failed;
  }
};

namespace {
// Inversion tolerance in VOL units. 1e-6 is 0.01 bp — two orders below the 5 bp
// vol tolerance the scorecard reports against, so it cannot move a published
// digit, and materially cheaper than the entry point's 1e-7 default across
// hundreds of thousands of cold Andersen-Lake inversions.
constexpr double kModeBIvTol = 1.0e-6;
constexpr std::uint16_t kModeBIvMaxIter = 64;

// A fitted vol this close to the inverter's reported floor IS the floor clamp.
// american_implied_vol returns Ok(kIvMin) — a SUCCESS — both for a price at or
// below intrinsic and for a quote whose root sits under the floor. Taking that
// at face value would publish 0.005 as a measured volatility for every dead
// deep-wing quote in the cohort. It is refused and counted instead.
constexpr double kModeBVolFloorSlack = 1.0e-9;

// IV IDENTIFIABILITY. A quote pins vol only as tightly as vega converts price
// into vol: moving the mid by half a tick moves the fitted vol by about
// (tick/2)/vega. Where that exceeds kModeBMaxVolResolution the returned root is
// an artifact of the quote's last digit rather than a measurement of anything,
// which is what "IV is not identified where vega -> 0" means numerically. The
// bound is deliberately loose (5 vol POINTS of resolution per half tick): it is
// here to refuse the hopeless, not to grade accuracy.
constexpr double kModeBMaxVolResolution = 0.05;

// Self-consistency of the round trip. The inverse and forward maps are the same
// Andersen-Lake rung, so re-pricing at the fitted vol must land back on the mid
// to within the solver's own tolerance; half a tick is slack of two orders. A
// row that misses it did not actually invert — commonly a collapsed-vega corner
// the screens above did not catch — and is refused rather than published.
constexpr double kModeBRoundTripTicks = 0.5;

// The ZERO-VOL American price: the correct lower bracket for an American
// inversion, and NOT the same thing as immediate intrinsic. At sigma = 0 the
// spot follows the deterministic path S_t = S e^{(r-q)t} and the holder may
// exercise at any t in [0, T], so the value is max over t of the discounted
// deterministic payoff. Both endpoints are evaluated:
//   t = 0  immediate intrinsic,      max(0, S - K)      / max(0, K - S)
//   t = T  discounted forward value, max(0, S e^{-qT} - K e^{-rT}) / mirrored
// The t = T leg is what plain intrinsic misses: for a call that is out of the
// money on spot but in the money on the FORWARD (the ordinary case under
// positive rates, and the everyday case on the hard-to-borrow names in this
// cohort where q < 0), the forward leg is strictly the larger, and a quote
// between the two admits no volatility at all. The library's own band screens
// only immediate intrinsic, so a mid in that gap would reach the solver, find
// no root, and come back clamped to the vol floor as a SUCCESS.
//
// The interior of [0, T] can hold the maximum for some (r, q) sign
// combinations; ignoring it leaves this a slightly LOOSE lower bound, which is
// the safe direction — a row it wrongly admits is caught by the floor-clamp and
// round-trip screens rather than published.
[[nodiscard]] double mode_b_lo_bound(double spot, double strike, double years, double rate,
                                     double q, Side side) noexcept {
  const double spot_leg = spot * std::exp(-q * years);
  const double strike_leg = strike * std::exp(-rate * years);
  const double now = side == Side::Call ? spot - strike : strike - spot;
  const double fwd = side == Side::Call ? spot_leg - strike_leg : strike_leg - spot_leg;
  return std::max(0.0, std::max(now, fwd));
}

// The no-arbitrage ceiling the American price cannot reach: the underlying for
// a call, the strike for a put. Mirrors american_iv.cpp's own upper band.
[[nodiscard]] double mode_b_up_bound(double spot, double strike, Side side) noexcept {
  return side == Side::Call ? spot : strike;
}

// Stable ordering key for one fit group. The three string members are VIEWS
// into the rows the runner was handed, which outlive the grouping.
struct ModeBGroupKey {
  std::string_view date;
  std::string_view bucket_et;
  std::string_view underlier;
  std::uint64_t years_bits = 0; // exact IEEE pattern; see the banner above
  [[nodiscard]] auto operator<=>(const ModeBGroupKey &) const = default;
};
} // namespace

class ModeBRunner final : public ModeRunner {
public:
  explicit ModeBRunner(ModeBStats &fit_stats) noexcept : fit_{&fit_stats} {}

  [[nodiscard]] std::string_view name() const noexcept override { return "b"; }

  [[nodiscard]] Status run(std::span<const OracleRow> rows, Scorecard &card, ModeStats &stats,
                           AggregateMetrics &agg) override {
    // GROUPING. An ordered map keyed by the four coordinates, holding row
    // INDICES: the fit is per `underlier x expiry x bucket`, and the flat span
    // this seam receives has already lost those boundaries (which is why
    // OracleRow now carries date/bucket_et at all).
    std::map<ModeBGroupKey, std::vector<std::uint32_t>> groups;
    for (std::uint32_t i = 0; i < rows.size(); ++i) {
      const OracleRow &row = rows[i];
      const ModeBGroupKey key{row.date, row.bucket_et, row.underlier,
                              std::bit_cast<std::uint64_t>(row.years)};
      groups[key].push_back(i);
    }
    fit_->groups_fitted = static_cast<std::int64_t>(groups.size());

    const double tick_engine = price_from_oracle_units(kPriceTick);
    const double half_tick = 0.5 * tick_engine;
    const double min_vega = half_tick / kModeBMaxVolResolution;
    const double round_trip_tol = kModeBRoundTripTicks * tick_engine;

    for (auto &[key, members] : groups) {
      // Strike-then-side order makes the warm-start chain meaningful: adjacent
      // strikes in one expiry carry near-equal vols, so each inversion seeds
      // from its neighbour's converged root. The library documents this as
      // moving the Newton PATH only — the root, and therefore every number
      // published below, is unchanged.
      std::sort(members.begin(), members.end(),
                [&rows](std::uint32_t l, std::uint32_t r) {
                  const OracleRow &a = rows[l];
                  const OracleRow &b = rows[r];
                  if (a.strike != b.strike) {
                    return a.strike < b.strike;
                  }
                  return static_cast<int>(a.side) < static_cast<int>(b.side);
                });

      double warm_start = 0.0; // 0 == "no warm start; use the European seed"
      for (const std::uint32_t index : members) {
        const OracleRow &row = rows[index];
        EnginePricingInputs in = mode_a_inputs(row); // same pinned map as Mode A
        // ...everything except sigma, which Mode B MEASURES rather than reads.
        in.sigma = 0.0;
        // SCOPE, stated rather than left to be discovered: the map's
        // `exercise_style` key is NOT honoured below. Mode B inverts and
        // re-prices through the AMERICAN rung unconditionally, so a
        // European-routed root is inverted against the wrong functional here
        // even though Mode A now prices it correctly. Closing that needs a
        // European inverter plus its own Mode B gate evidence, and it is
        // deliberately not smuggled in on the back of a Mode A price fix — the
        // Mode B numbers this file publishes are unchanged by that axis, which
        // is why they stay comparable across it.

        // ── Quote admission. A mid needs two live, uncrossed sides.
        if (!(row.bid_prc > 0.0) || !(row.ask_prc > 0.0) || !(row.ask_prc >= row.bid_prc)) {
          ++fit_->rows_no_quote;
          continue;
        }
        const double mid = price_from_oracle_units(0.5 * (row.bid_prc + row.ask_prc));
        if (!std::isfinite(mid) || !(in.years > 0.0) || !(in.spot > 0.0) || !(in.strike > 0.0)) {
          ++fit_->rows_no_quote;
          continue;
        }

        // ── Identifiability brackets (see mode_b_lo_bound's banner).
        const double lo = mode_b_lo_bound(in.spot, in.strike, in.years, in.rate, in.carry, in.side);
        if (mid <= lo + half_tick) {
          ++fit_->rows_below_lo_bound;
          continue;
        }
        if (mid >= mode_b_up_bound(in.spot, in.strike, in.side) - half_tick) {
          ++fit_->rows_above_up_bound;
          continue;
        }

        // ── The inversion. AMERICAN, through the same Andersen-Lake rung this
        // file prices with; `in.carry` is the engine's q slot.
        const Result<double> fitted = american_implied_vol(
            mid, in.spot, in.strike, in.years, in.rate, in.carry, in.side,
            AmericanMethod::AndersenLake, kModeBIvTol, kModeBIvMaxIter, mode_a_al_opts(),
            /*correction=*/nullptr, warm_start);
        if (!fitted.has_value() || !std::isfinite(*fitted)) {
          ++fit_->rows_iv_no_solution;
          continue;
        }
        if (*fitted <= kIvMin + kModeBVolFloorSlack) {
          ++fit_->rows_iv_at_floor; // the documented clamp, not a measured root
          continue;
        }
        in.sigma = *fitted;

        const Result<AmericanGreeks> greeks = american_greeks_al(
            in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, mode_a_al_opts());
        if (!greeks.has_value()) {
          ++stats.rows_engine_error; // same meaning as Mode A's: the pricer refused
          continue;
        }
        if (!std::isfinite(greeks->vega) || greeks->vega < min_vega) {
          ++fit_->rows_vega_below_floor;
          continue;
        }
        if (!std::isfinite(greeks->price) || std::abs(greeks->price - mid) > round_trip_tol) {
          ++fit_->rows_round_trip_failed;
          continue;
        }
        // Only a row that survived every screen seeds its neighbour.
        warm_start = in.sigma;
        ++stats.rows_priced;

        const MoneynessBand mband = moneyness_band(row.strike / row.uprc, in.side);
        const DteBand dband = dte_band(dte_days(row.years));
        auto observe = [&](std::string_view metric, double model, double oracle_val, double tol) {
          const double err = model - oracle_val;
          if (!std::isfinite(err)) {
            return;
          }
          card.observe(name(), metric, mband, dband, in.side, err, std::abs(err) <= tol);
        };

        const double model_price = price_to_oracle_units(greeks->price);
        observe("price", model_price, row.sr_prc, price_tolerance(row.bid_prc, row.ask_prc));
        agg.price.absolute(model_price, row.sr_prc);
        // THE measurement this whole mode exists for: a vol fitted from the
        // market, compared against SpiderRock's. Unlike Mode A's, this is not
        // an identity — the two numbers were produced independently.
        observe("vol", in.sigma, row.sr_vol, vol_tolerance());
        agg.vol.absolute(in.sigma, row.sr_vol);

        double dp_dq = std::numeric_limits<double>::quiet_NaN();
        const Result<CarryGreeks> carry = american_carry_greeks_al(
            in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, mode_a_al_opts());
        if (carry.has_value()) {
          dp_dq = carry->dP_dq;
        }
        const OracleUnitGreeks g = to_oracle_units(*greeks, dp_dq);
        const std::array<std::pair<double, double>, 9> greek_pairs{
            std::pair{g.de, row.de}, std::pair{g.ga, row.ga}, std::pair{g.th, row.th},
            std::pair{g.ve, row.ve}, std::pair{g.rh, row.rh}, std::pair{g.ph, row.ph},
            std::pair{g.vo, row.vo}, std::pair{g.va, row.va},
            std::pair{g.de_decay, row.de_decay}};
        for (std::size_t i = 0; i < greek_pairs.size(); ++i) {
          const auto [model, oracle_val] = greek_pairs[i];
          observe(kGreekCellMetric[i], model, oracle_val, greek_tolerance(oracle_val));
          agg.greeks[i].relative(model, oracle_val);
        }
      }
    }
    return Ok();
  }

private:
  ModeBStats *fit_; // non-owning; outlives this runner (a BenchRun member)
};

// One completed bench run. Returned whole so the gate test can assert on typed
// cell stats AND on the aggregates instead of re-parsing JSON.
struct BenchRun {
  Scorecard card;
  AggregateMetrics aggregate;
  ModeStats stats;
  // Mode B's fit accounting. Left all-zero on a Mode A run and emitted only
  // under --mode B, so Mode A's aggregate receipt keeps its exact key set.
  ModeBStats mode_b;
  // Cohort NAMES as the manifests declare them, in --cohort order. Names, never
  // membership: they are already on the command line.
  std::vector<std::string> cohort_names;
};

[[nodiscard]] inline std::string join(std::span<const std::string> parts, char sep) {
  std::string out;
  for (const std::string &part : parts) {
    if (!out.empty()) {
      out.push_back(sep);
    }
    out.append(part);
  }
  return out;
}

// Run-level aggregates ONLY. Every field here is a whole-run number: no cell
// key, no partition, no date, no underlier, no per-cell count. Adding anything
// row-addressable would breach the --aggregate-only confidentiality boundary
// the file banner describes.
[[nodiscard]] inline Result<std::string> aggregate_json(const BenchArgs &args,
                                                        const BenchRun &run) {
  std::vector<AggregateMetric> metrics;
  if (args.benchmark_speed) {
    if (run.stats.rows_priced <= 0 || !(run.stats.wall_seconds > 0.0)) {
      return Err(ErrorCode::Internal, "--benchmark-speed measured no priced work");
    }
    metrics.push_back({speed_metric_id(args.preset),
                       static_cast<double>(run.stats.rows_priced) / run.stats.wall_seconds,
                       run.stats.rows_priced, "rows_per_second"});
  } else {
    ATX_TRY(metrics, target_metrics(args.mode, run.aggregate));
  }

  const double priced = static_cast<double>(run.stats.rows_priced);
  const double rows_per_s =
      run.stats.wall_seconds > 0.0 ? priced / run.stats.wall_seconds : 0.0;
  std::string out;
  out.reserve(2048);
  out.append("{\n  \"schema_version\": 1");
  out.append(",\n  \"kind\": \"oracle_aggregate\"");
  out.append(",\n  \"mode\": ");
  append_json_string(out, mode_token(args.mode));
  out.append(",\n  \"cohorts\": [");
  for (std::size_t i = 0; i < run.cohort_names.size(); ++i) {
    if (i != 0) {
      out.append(", ");
    }
    append_json_string(out, run.cohort_names[i]);
  }
  out.append("]");
  out.append(",\n  \"iter\": ");
  append_json_int(out, args.iter);
  out.append(",\n  \"git_sha\": ");
  append_json_string(out, args.git_sha);
  out.append(",\n  \"preset\": ");
  append_json_string(out, args.preset);
  out.append(",\n  \"quiet_host\": ");
  out.append(args.quiet_host ? "true" : "false");
  out.append(",\n  \"rows_total\": ");
  append_json_int(out, run.stats.rows_total);
  out.append(",\n  \"rows_priced\": ");
  append_json_int(out, run.stats.rows_priced);
  out.append(",\n  \"rows_null_sentinel\": ");
  append_json_int(out, run.stats.rows_null_sentinel);
  out.append(",\n  \"rows_bad_input\": ");
  append_json_int(out, run.stats.rows_bad_input);
  out.append(",\n  \"rows_engine_error\": ");
  append_json_int(out, run.stats.rows_engine_error);
  if (args.mode == BenchMode::B) {
    // Mode B's REFUSALS, published rather than absorbed. Without this block the
    // eleven metrics would silently describe whatever subset of the cohort
    // happened to yield a vol, and the difference would be invisible. Every
    // field is a whole-run COUNT — no cell key, band, date, bucket or underlier
    // — so it stays inside the --aggregate-only confidentiality boundary. Only
    // emitted under --mode B, which keeps Mode A's receipt key set exact.
    const ModeBStats &fit = run.mode_b;
    out.append(",\n  \"mode_b_fit\": {");
    out.append("\n    \"groups_fitted\": ");
    append_json_int(out, fit.groups_fitted);
    out.append(",\n    \"rows_unfitted\": ");
    append_json_int(out, fit.rows_unfitted());
    out.append(",\n    \"rows_no_quote\": ");
    append_json_int(out, fit.rows_no_quote);
    out.append(",\n    \"rows_below_lo_bound\": ");
    append_json_int(out, fit.rows_below_lo_bound);
    out.append(",\n    \"rows_above_up_bound\": ");
    append_json_int(out, fit.rows_above_up_bound);
    out.append(",\n    \"rows_iv_no_solution\": ");
    append_json_int(out, fit.rows_iv_no_solution);
    out.append(",\n    \"rows_iv_at_floor\": ");
    append_json_int(out, fit.rows_iv_at_floor);
    out.append(",\n    \"rows_vega_below_floor\": ");
    append_json_int(out, fit.rows_vega_below_floor);
    out.append(",\n    \"rows_round_trip_failed\": ");
    append_json_int(out, fit.rows_round_trip_failed);
    out.append("\n  }");
  }
  out.append(",\n  \"wall_seconds\": ");
  append_json_double(out, run.stats.wall_seconds);
  out.append(",\n  \"rows_per_second\": ");
  append_json_double(out, rows_per_s);
  out.append(",\n  \"metrics\": [");
  for (std::size_t i = 0; i < metrics.size(); ++i) {
    append_metric(out, metrics[i], i == 0);
  }
  out.append("\n  ]\n}\n");
  return Ok(std::move(out));
}

// Loads every named cohort, scans ONLY their named partitions, runs the mode,
// and returns the populated scorecard + aggregates. Writes nothing: emission is
// the caller's, because the two output shapes differ in what they may reveal.
[[nodiscard]] Result<BenchRun> run_oracle_bench_core(const BenchArgs &args) {
  // The Stage-4 run-time refusal that used to stand here is GONE because a real
  // Mode B now stands behind it (ModeBRunner above). It was never a stub: a
  // Mode B returning plausible numbers would have been ratcheted on by the
  // oracle loop, which is strictly worse than a run that refuses. The same
  // principle now lives one level down, per row — every row Mode B cannot fit
  // is counted in ModeBStats and published, never guessed at.
  if (args.quiet_host) {
    ATX_TRY_VOID(enforce_quiet_host());
  }

  BenchRun run;
  std::vector<OracleRow> rows;
  for (const std::string &path : args.cohort_paths) {
    ATX_TRY(const Cohort cohort, load_cohort_json(path));
    ATX_TRY(const CohortScan scan, read_cohort_rows(cohort, args.store_root));
    run.cohort_names.push_back(cohort.name);
    run.stats.rows_null_sentinel += scan.rows_null_sentinel;
    run.stats.rows_bad_input += scan.rows_bad_input;
    rows.insert(rows.end(), scan.rows.begin(), scan.rows.end());
    if (args.aggregate_only) {
      continue; // partition names ARE membership — never under --aggregate-only
    }
    std::fprintf(stderr,
                 "oracle-bench: cohort '%s': %zu partition dir(s) opened, %zu row(s) admitted, "
                 "%lld null-sentinel + %lld bad-input row(s) skipped\n",
                 cohort.name.c_str(), scan.partitions_opened.size(), scan.rows.size(),
                 static_cast<long long>(scan.rows_null_sentinel),
                 static_cast<long long>(scan.rows_bad_input));
    for (const std::string &p : scan.partitions_opened) {
      std::fprintf(stderr, "oracle-bench: partition %s\n", p.c_str());
    }
  }

  ModeARunner mode_a;
  ModeBRunner mode_b{run.mode_b};
  // ONE runner per invocation, selected by --mode. The loop below publishes
  // run.stats and the scorecard's mode stats from the runner it ran, so running
  // both in one pass would leave the scorecard describing the second mode and
  // the aggregate describing a mixture of the two.
  ModeRunner *const selected = args.mode == BenchMode::A
                                   ? static_cast<ModeRunner *>(&mode_a)
                                   : static_cast<ModeRunner *>(&mode_b);
  ModeRunner *const runners[] = {selected};
  for (ModeRunner *const runner : runners) {
    ModeStats stats;
    stats.rows_null_sentinel = run.stats.rows_null_sentinel;
    stats.rows_bad_input = run.stats.rows_bad_input;
    stats.rows_total = static_cast<std::int64_t>(rows.size()) + stats.rows_null_sentinel +
                       stats.rows_bad_input;
    const auto t0 = std::chrono::steady_clock::now();
    ATX_TRY_VOID(runner->run(rows, run.card, stats, run.aggregate));
    stats.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    run.card.set_mode_stats(runner->name(), stats);
    run.stats = stats;
    const double rows_per_s = stats.wall_seconds > 0.0
                                  ? static_cast<double>(stats.rows_priced) / stats.wall_seconds
                                  : 0.0;
    std::fprintf(stderr, "oracle-bench: mode %.*s: %lld row(s) priced in %.3f s (%.0f rows/s)\n",
                 static_cast<int>(runner->name().size()), runner->name().data(),
                 static_cast<long long>(stats.rows_priced), stats.wall_seconds, rows_per_s);
    if (args.mode == BenchMode::B) {
      // Scalar COUNTS only — safe under --aggregate-only for the same reason
      // rows/s is: a count is not membership. Named on stderr so a run whose
      // vol coverage collapsed says so while it is running, instead of only in
      // the receipt someone reads later.
      const ModeBStats &fit = run.mode_b;
      std::fprintf(stderr,
                   "oracle-bench: mode b fit: %lld group(s); %lld row(s) unfitted "
                   "(no-quote %lld, below-lo-bound %lld, above-up-bound %lld, no-solution %lld, "
                   "at-vol-floor %lld, vega-below-floor %lld, round-trip %lld)\n",
                   static_cast<long long>(fit.groups_fitted),
                   static_cast<long long>(fit.rows_unfitted()),
                   static_cast<long long>(fit.rows_no_quote),
                   static_cast<long long>(fit.rows_below_lo_bound),
                   static_cast<long long>(fit.rows_above_up_bound),
                   static_cast<long long>(fit.rows_iv_no_solution),
                   static_cast<long long>(fit.rows_iv_at_floor),
                   static_cast<long long>(fit.rows_vega_below_floor),
                   static_cast<long long>(fit.rows_round_trip_failed));
    }
  }
  return Ok(std::move(run));
}

[[nodiscard]] Status write_text_file(const std::string &path, std::string_view text) {
  const std::filesystem::path out{path};
  if (out.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "create output dir: " + ec.message());
    }
  }
  std::ofstream file{out, std::ios::binary | std::ios::trunc};
  if (!file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
    return Err(ErrorCode::IoError, "write output: " + path);
  }
  return Ok();
}

// Runs the bench and emits the shape --aggregate-only selected: run-level
// aggregates (to --out, or stdout when absent), or the charter cell-key
// scorecard file. Returns the run so the gate test asserts on typed values.
[[nodiscard]] Result<BenchRun> run_oracle_bench(const BenchArgs &args) {
  ATX_TRY(BenchRun run, run_oracle_bench_core(args));
  if (args.aggregate_only) {
    ATX_TRY(const std::string json, aggregate_json(args, run));
    if (args.out_path.empty()) {
      if (std::fwrite(json.data(), 1, json.size(), stdout) != json.size()) {
        return Err(ErrorCode::IoError, "write aggregate JSON to stdout");
      }
      return Ok(std::move(run));
    }
    ATX_TRY_VOID(write_text_file(args.out_path, json));
    return Ok(std::move(run));
  }
  const ScorecardHeader header{args.iter, args.git_sha, join(run.cohort_names, ',')};
  ATX_TRY_VOID(write_text_file(args.out_path, run.card.to_json(header)));
  return Ok(std::move(run));
}

[[nodiscard]] Status run_oracle_convention_sweep(const BenchArgs &args) {
  ATX_TRY(const Cohort smoke_cohort, load_cohort_json(args.smoke_path));
  ATX_TRY(const Cohort tune_cohort, load_cohort_json(args.tune_path));
  if (smoke_cohort.name != "smoke" || tune_cohort.name != "tune") {
    return Err(ErrorCode::InvalidArgument, "convention sweep admits only named smoke+tune");
  }
  ATX_TRY(const CohortScan smoke, read_cohort_rows(smoke_cohort, args.store_root));
  ATX_TRY(const CohortScan tune, read_cohort_rows(tune_cohort, args.store_root));
  ATX_TRY(const ConventionSweepResult result, run_convention_sweep(smoke.rows, tune.rows));
  std::fprintf(stderr,
               "oracle-conventions: smoke=%zu tune=%zu priced=%lld errors=%lld %.0f rows/s "
               "diagnostic-only\n",
               smoke.rows.size(), tune.rows.size(), static_cast<long long>(result.rows_priced),
               static_cast<long long>(result.engine_errors), result.diagnostic_rows_per_second);
  return write_text_file(args.out_path, convention_sweep_json(result, args.git_sha));
}

} // namespace atx::vol::oracle

#ifndef ATX_ORACLE_BENCH_NO_MAIN
int main(int argc, char **argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  const std::string_view argv0 = argc > 0 && argv[0] != nullptr ? argv[0] : std::string_view{};
  const auto parsed =
      atx::vol::oracle::parse_bench_args(args, atx::vol::oracle::discover_cohort_dir(argv0));
  if (!parsed.has_value()) {
    std::fprintf(stderr, "atx-vol-oracle-bench: %s\n%.*s", parsed.error().to_string().c_str(),
                 static_cast<int>(atx::vol::oracle::kUsage.size()),
                 atx::vol::oracle::kUsage.data());
    return 2;
  }
  if (parsed->convention_sweep) {
    const auto run = atx::vol::oracle::run_oracle_convention_sweep(*parsed);
    if (!run.has_value()) {
      std::fprintf(stderr, "atx-vol-oracle-bench: %s\n", run.error().to_string().c_str());
      return 1;
    }
    return 0;
  }
  const auto run = atx::vol::oracle::run_oracle_bench(*parsed);
  if (!run.has_value()) {
    std::fprintf(stderr, "atx-vol-oracle-bench: %s\n", run.error().to_string().c_str());
    return 1;
  }
  return 0;
}
#endif
