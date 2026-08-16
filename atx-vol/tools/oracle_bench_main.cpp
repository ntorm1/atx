// atx-vol-oracle-bench — SpiderRock parity benchmark, Mode A
// (bench/oracle/CHARTER.md stage 2).
//
// Input: a cohort JSON (bench/oracle/cohorts/README.md schema) selecting
// partitions of the parquet oracle store. Only the cohort-named
// date=<d>/bucket_et=<b> dirs are opened (oracle_cohort_reader.cpp — pruning
// by construction), with an exact undSecKey_tk predicate pushed into each
// file scan. Mode A prices every admitted row through the PUBLIC atx-vol
// American API using SpiderRock's own inputs (uPrc, rate, sdiv, ddiv, years;
// vol = srVol) and compares vol identity + price + greeks to srVol / srPrc /
// de ga th ve rh ph vo va deDecay under the oracle_conventions.* unit map. Output: the charter
// cell-key scorecard JSON to --out; rows/s and the opened partitions to
// stderr.
//
// ACCESS MECHANISM FOR THE GATE TEST: tests/oracle_bench_test.cpp reaches
// run_oracle_bench/parse_bench_args by `#define ATX_ORACLE_BENCH_NO_MAIN` +
// a textual #include of THIS file (the bev_label_factory_gate_test.cpp
// pattern — a CLI-only driver TU with no reuse value beyond this binary does
// not warrant a header). That test TU is the only place this file is
// #included, so there is no ODR hazard.

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "oracle_cohort_reader.hpp"
#include "oracle_convention_sweep.hpp"
#include "oracle_conventions.hpp"
#include "oracle_scorecard.hpp"

namespace atx::vol::oracle {

using atx::core::Ok; // Err resolves via ADL (Error argument); Ok's arguments
                     // are not atx::core types, so the explicit using is
                     // needed — same convention as track_store.cpp.

namespace {
constexpr std::string_view kUsage =
    "usage: atx-vol-oracle-bench --cohort <cohort.json> --store <store-root>\n"
    "                            --out <scorecard.json> [--iter N] [--git-sha SHA]\n"
    "   or: atx-vol-oracle-bench --convention-sweep --smoke <smoke.json>\n"
    "                            --tune <tune.json> --store <store-root>\n"
    "                            --out <sweep.json> --git-sha <SHA>\n";
} // namespace

struct BenchArgs {
  std::string cohort_path;
  std::string smoke_path;
  std::string tune_path;
  std::string store_root;
  std::string out_path;
  std::string git_sha = "unknown";
  std::int64_t iter = 0;
  bool convention_sweep = false;
};

[[nodiscard]] Result<BenchArgs> parse_bench_args(std::span<const std::string> args) {
  BenchArgs out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string &flag = args[i];
    const bool has_value = i + 1 < args.size();
    auto take = [&]() -> const std::string & { return args[++i]; };
    if (flag == "--cohort" && has_value) {
      out.cohort_path = take();
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
  if (out.store_root.empty() || out.out_path.empty()) {
    return Err(ErrorCode::InvalidArgument, "--store and --out are required");
  }
  if (out.convention_sweep) {
    if (!out.cohort_path.empty() || out.smoke_path.empty() || out.tune_path.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "convention sweep requires --smoke and --tune, without --cohort");
    }
  } else if (out.cohort_path.empty() || !out.smoke_path.empty() || !out.tune_path.empty()) {
    return Err(ErrorCode::InvalidArgument, "scorecard mode requires only --cohort");
  }
  return Ok(std::move(out));
}

// ── Mode seam ────────────────────────────────────────────────────────────
//
// A mode maps the cohort's admitted rows to model outputs and records
// per-metric observations into the scorecard under its own <mode> key prefix.
// This interface IS the Mode B boundary: Mode B (fit from raw NBBO per
// underlier x expiry x bucket, a later charter stage) implements this same
// contract — it is batch-level ON PURPOSE, because Mode B needs cross-row
// fitting context a per-row hook could not provide. Per charter mandate there
// is NO Mode B stub here; the seam is the contract alone.
class ModeRunner {
public:
  virtual ~ModeRunner() = default;
  ModeRunner(const ModeRunner &) = delete;
  ModeRunner &operator=(const ModeRunner &) = delete;
  ModeRunner(ModeRunner &&) = delete;
  ModeRunner &operator=(ModeRunner &&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  // Prices/evaluates every row, adding observations to `card` and row
  // accounting to `stats` (rows_priced / rows_engine_error; the reader-side
  // skip counts are already in `stats` when this is called).
  [[nodiscard]] virtual Status run(std::span<const OracleRow> rows, Scorecard &card,
                                   ModeStats &stats) = 0;

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

  [[nodiscard]] Status run(std::span<const OracleRow> rows, Scorecard &card,
                           ModeStats &stats) override {
    for (const OracleRow &row : rows) {
      const EnginePricingInputs in = mode_a_inputs(row);
      const Result<AmericanGreeks> greeks = american_greeks_al(
          in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, mode_a_al_opts());
      if (!greeks.has_value()) {
        // Expected on corner regimes (double-continuation, degenerate T/sigma
        // the reader could not know about): counted, not fatal.
        ++stats.rows_engine_error;
        continue;
      }
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

      observe("price", price_to_oracle_units(greeks->price), row.sr_prc,
              price_tolerance(row.bid_prc, row.ask_prc));
      // Mode A deliberately prices at SpiderRock's own vol. Record that
      // identity as a first-class aggregate instead of letting the target
      // registry claim vol coverage without scorecard provenance.
      observe("vol", in.sigma, row.sr_vol, vol_tolerance());

      // ph rides the carry-greeks route; its corner-regime failures skip ONLY
      // the ph observation (cell n per metric reflects it), never the row.
      double dp_dq = std::numeric_limits<double>::quiet_NaN();
      const Result<CarryGreeks> carry = american_carry_greeks_al(
          in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, mode_a_al_opts());
      if (carry.has_value()) {
        dp_dq = carry->dP_dq;
      }
      const OracleUnitGreeks g = to_oracle_units(*greeks, dp_dq);
      observe("de", g.de, row.de, greek_tolerance(row.de));
      observe("ga", g.ga, row.ga, greek_tolerance(row.ga));
      observe("th", g.th, row.th, greek_tolerance(row.th));
      observe("ve", g.ve, row.ve, greek_tolerance(row.ve));
      observe("rh", g.rh, row.rh, greek_tolerance(row.rh));
      observe("ph", g.ph, row.ph, greek_tolerance(row.ph)); // NaN ph self-skips
      observe("vo", g.vo, row.vo, greek_tolerance(row.vo));
      observe("va", g.va, row.va, greek_tolerance(row.va));
      observe("deDecay", g.de_decay, row.de_decay, greek_tolerance(row.de_decay));
    }
    return Ok();
  }
};

// Loads the cohort, scans ONLY its named partitions, runs every registered
// mode, writes the scorecard JSON to args.out_path, and reports rows/s (per
// mode) + the opened partitions on stderr. Returns the populated scorecard so
// the gate test can assert on typed cell stats instead of re-parsing JSON.
[[nodiscard]] Result<Scorecard> run_oracle_bench(const BenchArgs &args) {
  ATX_TRY(const Cohort cohort, load_cohort_json(args.cohort_path));
  ATX_TRY(const CohortScan scan, read_cohort_rows(cohort, args.store_root));

  std::fprintf(stderr,
               "oracle-bench: cohort '%s': %zu partition dir(s) opened, %zu row(s) admitted, "
               "%lld null-sentinel + %lld bad-input row(s) skipped\n",
               cohort.name.c_str(), scan.partitions_opened.size(), scan.rows.size(),
               static_cast<long long>(scan.rows_null_sentinel),
               static_cast<long long>(scan.rows_bad_input));
  for (const std::string &p : scan.partitions_opened) {
    std::fprintf(stderr, "oracle-bench: partition %s\n", p.c_str());
  }

  Scorecard card;
  ModeARunner mode_a;
  ModeRunner *const runners[] = {&mode_a}; // Mode B registers here (later stage)
  for (ModeRunner *const runner : runners) {
    ModeStats stats;
    stats.rows_null_sentinel = scan.rows_null_sentinel;
    stats.rows_bad_input = scan.rows_bad_input;
    stats.rows_total =
        static_cast<std::int64_t>(scan.rows.size()) + scan.rows_null_sentinel + scan.rows_bad_input;
    const auto t0 = std::chrono::steady_clock::now();
    ATX_TRY_VOID(runner->run(scan.rows, card, stats));
    stats.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    card.set_mode_stats(runner->name(), stats);
    const double rows_per_s = stats.wall_seconds > 0.0
                                  ? static_cast<double>(stats.rows_priced) / stats.wall_seconds
                                  : 0.0;
    std::fprintf(stderr, "oracle-bench: mode %.*s: %lld row(s) priced in %.3f s (%.0f rows/s)\n",
                 static_cast<int>(runner->name().size()), runner->name().data(),
                 static_cast<long long>(stats.rows_priced), stats.wall_seconds, rows_per_s);
  }

  const ScorecardHeader header{args.iter, args.git_sha, cohort.name};
  const std::string json = card.to_json(header);

  const std::filesystem::path out{args.out_path};
  if (out.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "create scorecard dir: " + ec.message());
    }
  }
  std::ofstream f{out, std::ios::binary | std::ios::trunc};
  if (!f.write(json.data(), static_cast<std::streamsize>(json.size()))) {
    return Err(ErrorCode::IoError, "write scorecard: " + args.out_path);
  }
  return Ok(std::move(card));
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
  const auto parsed = atx::vol::oracle::parse_bench_args(args);
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
