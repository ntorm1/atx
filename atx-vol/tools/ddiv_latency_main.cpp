// atx-vol-ddiv-latency: per-contract latency and coverage of the production cash-dividend
// path (lane D1, vol-sota r6, 2026-10-23; docs/american-cash-dividend-reference.md
// section 11, docs/hft-support-matrix.md).
//
// WHAT IS TIMED, per contract (one thread, pinned with --affinity; B0's harness,
// tools/bench_clocks.hpp, supplies the clock, the order statistics, the pinning and the
// affinity / build receipts):
//   cold_price   american_discrete_div_production_price: route + step map + rollback,
//                nothing prepared (the P1 shape: a standalone cold price);
//   price        DiscreteDivBatch::price on the prepared group (P2 price);
//   iv           DiscreteDivBatch::implied_vol of the contract's quote (P2 IV);
//   bundle       DiscreteDivBatch::greeks, one-day event-aware roll (P3 bundle);
// and per GROUP (one (S, T, r, q, schedule): an expiry of an underlier):
//   setup        DiscreteDivBatch::prepare of that group alone. It is charged to B0's
//                `family_build_validation` stage of a cold group-calibration clock whose
//                other stage is `inverses` (the group's implied_vol calls), and the stage
//                table (bench_clocks.hpp stages_json) reconciles the two against the
//                clock's own start / end stamps. Pass 1 runs that clock over every
//                group in one contiguous span; pass 2 re-prepares each group (untimed)
//                and times the prepared queries and the cold price.
// The contract's sigma for price / bundle / cold_price is its identified root, else
// --sigma. Coverage counts every contract's implied_vol outcome (priced, price_only,
// refused by reason; no_quote when the input carries none) and every price outcome.
//
// INPUT: CSV `group,S,K,T,r,q,side,quote,events` (events `tau:amount|...`, empty for
// none; quote empty for none). Groups are the distinct (group, S, T, r, q, events) keys,
// in input order. --fill-quote-sigma X replaces a missing quote by the route's price at
// X (untimed, before measurement): the synthetic ladder's identifiable quotes.
//
// OUTPUT: --output JSON (receipts, rows by event bucket x tenor bucket, totals, stage
// table) and the same rows as a markdown table on stdout.
//
// Class: measurement only; no library code path changes.

#include "bench_clocks.hpp"

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_batch.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace bench = atx::vol::bench;
using atx::vol::CashDividend;
using atx::vol::DiscreteDivAccuracy;
using atx::vol::DiscreteDivBatch;
using atx::vol::DiscreteDivBatchContract;
using atx::vol::DiscreteDivBatchGroup;
using atx::vol::DiscreteDivBatchOutcome;
using atx::vol::DiscreteDivBatchResult;
using atx::vol::DiscreteDivIvOpts;
using atx::vol::DiscreteDivProductionGreeks;
using atx::vol::DiscreteDivProductionOpts;
using atx::vol::DiscreteDivWorkspace;
using atx::vol::ExerciseStyle;
using atx::vol::Side;
using Json = bench::Json;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kRollHorizon = 1.0 / 252.0;

struct Arguments {
  std::string input;
  std::string output;
  std::optional<unsigned> affinity;
  bool require_pcore = false;
  DiscreteDivAccuracy accuracy = DiscreteDivAccuracy::Standard;
  double sigma = 0.30;
  std::optional<double> fill_quote_sigma;
};

[[nodiscard]] Arguments arguments(int argc, char **argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(key + " needs a value");
      }
      return argv[++i];
    };
    if (key == "--input") {
      args.input = value();
    } else if (key == "--output") {
      args.output = value();
    } else if (key == "--affinity") {
      args.affinity = static_cast<unsigned>(std::stoul(value()));
    } else if (key == "--require-pcore") {
      args.require_pcore = true;
    } else if (key == "--accuracy") {
      const std::string a = value();
      if (a == "standard") {
        args.accuracy = DiscreteDivAccuracy::Standard;
      } else if (a == "refined") {
        args.accuracy = DiscreteDivAccuracy::Refined;
      } else {
        throw std::runtime_error("--accuracy must be standard or refined");
      }
    } else if (key == "--sigma") {
      args.sigma = std::stod(value());
    } else if (key == "--fill-quote-sigma") {
      args.fill_quote_sigma = std::stod(value());
    } else {
      throw std::runtime_error("unknown argument " + key +
                               " (--input CSV --output JSON [--affinity CPU [--require-pcore]] "
                               "[--accuracy standard|refined] [--sigma X] "
                               "[--fill-quote-sigma X])");
    }
  }
  if (args.input.empty() || args.output.empty()) {
    throw std::runtime_error("--input and --output are required");
  }
  if (args.require_pcore && !args.affinity) {
    throw std::runtime_error("--require-pcore requires --affinity");
  }
  return args;
}

struct Contract {
  std::size_t group = 0;
  double K = 0.0;
  Side side = Side::Call;
  double quote = kNaN;
};

struct Group {
  std::string key;
  double S = 0.0;
  double T = 0.0;
  double r = 0.0;
  double q = 0.0;
  std::vector<CashDividend> dividends;
};

[[nodiscard]] std::vector<std::string> split(const std::string &text, char sep) {
  std::vector<std::string> out;
  std::string field;
  std::istringstream in(text);
  while (std::getline(in, field, sep)) {
    out.push_back(field);
  }
  if (!text.empty() && text.back() == sep) {
    out.emplace_back();
  }
  return out;
}

[[nodiscard]] std::vector<CashDividend> parse_events(const std::string &text) {
  std::vector<CashDividend> out;
  if (text.empty()) {
    return out;
  }
  for (const std::string &event : split(text, '|')) {
    const auto colon = event.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("event without ':': " + event);
    }
    out.push_back({std::stod(event.substr(0, colon)), std::stod(event.substr(colon + 1))});
  }
  return out;
}

void load(const std::string &path, std::vector<Group> &groups,
          std::vector<Contract> &contracts) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open " + path);
  }
  std::string line;
  std::getline(in, line);
  if (line.rfind("group,S,K,T,r,q,side,quote,events", 0) != 0) {
    throw std::runtime_error("unexpected header: " + line);
  }
  std::map<std::string, std::size_t> index;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const std::vector<std::string> f = split(line, ',');
    if (f.size() != 9U) {
      throw std::runtime_error("expected 9 fields: " + line);
    }
    const std::string key = f[0] + "," + f[1] + "," + f[3] + "," + f[4] + "," + f[5] + "," + f[8];
    auto it = index.find(key);
    if (it == index.end()) {
      Group g;
      g.key = f[0];
      g.S = std::stod(f[1]);
      g.T = std::stod(f[3]);
      g.r = std::stod(f[4]);
      g.q = std::stod(f[5]);
      g.dividends = parse_events(f[8]);
      it = index.emplace(key, groups.size()).first;
      groups.push_back(std::move(g));
    }
    Contract c;
    c.group = it->second;
    c.K = std::stod(f[2]);
    c.side = (f[6] == "C") ? Side::Call : Side::Put;
    c.quote = f[7].empty() ? kNaN : std::stod(f[7]);
    contracts.push_back(c);
  }
}

// Event-count and tenor buckets of the table (events in life, from the group's route).
constexpr std::array<const char *, 7> kEventBuckets = {"0", "1", "2", "3-4", "5-8", "9-12",
                                                       ">12"};
constexpr std::array<const char *, 4> kTenorBuckets = {"<=0.25y", "0.25-1y", "1-2y", ">2y"};

[[nodiscard]] std::size_t event_bucket(int events) noexcept {
  if (events <= 2) {
    return static_cast<std::size_t>(std::max(events, 0));
  }
  if (events <= 4) {
    return 3U;
  }
  if (events <= 8) {
    return 4U;
  }
  return events <= 12 ? 5U : 6U;
}

[[nodiscard]] std::size_t tenor_bucket(double T) noexcept {
  if (T <= 0.25) {
    return 0U;
  }
  if (T <= 1.0) {
    return 1U;
  }
  return T <= 2.0 ? 2U : 3U;
}

constexpr std::size_t kOutcomes = 6; // DiscreteDivBatchOutcome
constexpr std::array<const char *, kOutcomes> kOutcomeNames = {
    "priced", "price_only", "refused_input", "refused_range", "refused_budget", "refused_group"};

struct Cell {
  std::size_t groups = 0;
  std::size_t contracts = 0;
  std::vector<double> setup_us;
  std::vector<double> cold_price_us;
  std::vector<double> price_us;
  std::vector<double> iv_us;
  std::vector<double> bundle_us;
  std::array<std::size_t, kOutcomes> iv_outcomes{};
  std::array<std::size_t, kOutcomes> price_outcomes{};
  std::size_t no_quote = 0;
  std::size_t cold_price_refused = 0;
  std::size_t bundle_refused = 0;
};

[[nodiscard]] double micros(bench::BenchClock::time_point a, bench::BenchClock::time_point b) {
  return bench::seconds_between(a, b) * 1.0e6;
}

[[nodiscard]] Json summary_us(const std::vector<double> &v) {
  return bench::summary_json(bench::summarize(v));
}

[[nodiscard]] Json cell_json(const Cell &c) {
  Json iv = Json::object();
  Json price = Json::object();
  for (std::size_t o = 0; o < kOutcomes; ++o) {
    iv[kOutcomeNames[o]] = c.iv_outcomes[o];
    price[kOutcomeNames[o]] = c.price_outcomes[o];
  }
  iv["no_quote"] = c.no_quote;
  return {{"groups", c.groups},
          {"contracts", c.contracts},
          {"setup_us", summary_us(c.setup_us)},
          {"cold_price_us", summary_us(c.cold_price_us)},
          {"price_us", summary_us(c.price_us)},
          {"iv_us", summary_us(c.iv_us)},
          {"bundle_us", summary_us(c.bundle_us)},
          {"iv_coverage", iv},
          {"price_coverage", price},
          {"cold_price_refused", c.cold_price_refused},
          {"bundle_refused", c.bundle_refused}};
}

void merge(Cell &into, const Cell &from) {
  into.groups += from.groups;
  into.contracts += from.contracts;
  const auto append = [](std::vector<double> &a, const std::vector<double> &b) {
    a.insert(a.end(), b.begin(), b.end());
  };
  append(into.setup_us, from.setup_us);
  append(into.cold_price_us, from.cold_price_us);
  append(into.price_us, from.price_us);
  append(into.iv_us, from.iv_us);
  append(into.bundle_us, from.bundle_us);
  for (std::size_t o = 0; o < kOutcomes; ++o) {
    into.iv_outcomes[o] += from.iv_outcomes[o];
    into.price_outcomes[o] += from.price_outcomes[o];
  }
  into.no_quote += from.no_quote;
  into.cold_price_refused += from.cold_price_refused;
  into.bundle_refused += from.bundle_refused;
}

[[nodiscard]] std::string fmt_us(const std::vector<double> &v) {
  if (v.empty()) {
    return "-";
  }
  const bench::ClockSummary s = bench::summarize(v);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.1f / %.1f", s.median, s.p99);
  return buf;
}

void print_row(const std::string &events, const std::string &tenor, const Cell &c) {
  const std::size_t quoted = c.contracts - c.no_quote;
  std::printf("| %s | %s | %zu | %zu | %s | %s | %s | %s | %s | %zu / %zu / %zu / %zu / %zu | "
              "%zu |\n",
              events.c_str(), tenor.c_str(), c.groups, c.contracts, fmt_us(c.setup_us).c_str(),
              fmt_us(c.cold_price_us).c_str(), fmt_us(c.price_us).c_str(),
              fmt_us(c.iv_us).c_str(), fmt_us(c.bundle_us).c_str(), quoted, c.iv_outcomes[0],
              c.iv_outcomes[1], c.iv_outcomes[2] + c.iv_outcomes[3] + c.iv_outcomes[5],
              c.iv_outcomes[4], c.price_outcomes[0]);
}

using Grid = std::array<std::array<Cell, kTenorBuckets.size()>, kEventBuckets.size()>;

// Pass 1 for one group: prepare it alone (charged to family_build_validation), then
// invert each quoted contract (charged to inverses), recording the setup and per-call
// IV times, the IV outcomes and the identified roots. Returns the group's cell index
// (event bucket x tenor bucket, flattened).
[[nodiscard]] std::size_t cold_group(const Group &g, const std::vector<std::size_t> &members,
                                     const std::vector<Contract> &contracts, double sigma,
                                     DiscreteDivBatch &batch, bench::PhaseClock &clock,
                                     Grid &cells, std::vector<double> &sigma_of) {
  const DiscreteDivBatchGroup in{g.S, g.T, g.r, g.q, g.dividends};
  const auto t0 = bench::BenchClock::now();
  const std::size_t refused = batch.prepare(std::span<const DiscreteDivBatchGroup>(&in, 1));
  const auto t1 = bench::BenchClock::now();
  clock.lap(bench::Stage::FamilyBuildValidation, t1);
  const int events = (refused == 0U) ? batch.route(0).events : 0;
  const std::size_t e = event_bucket(events);
  const std::size_t t = tenor_bucket(g.T);
  Cell &cell = cells[e][t];
  ++cell.groups;
  cell.setup_us.push_back(micros(t0, t1));
  const DiscreteDivIvOpts iv_opts;
  std::array<DiscreteDivBatchResult, 1> one{};
  for (const std::size_t ci : members) {
    const Contract &c = contracts[ci];
    ++cell.contracts;
    if (!std::isfinite(c.quote)) {
      ++cell.no_quote;
      continue;
    }
    const DiscreteDivBatchContract bc{0U, c.K, c.side, sigma, c.quote};
    const auto a = bench::BenchClock::now();
    const atx::vol::Status ok =
        batch.implied_vol(std::span<const DiscreteDivBatchContract>(&bc, 1), iv_opts, one);
    const auto b = bench::BenchClock::now();
    if (!ok) {
      throw std::runtime_error("implied_vol: " + ok.error().to_string());
    }
    cell.iv_us.push_back(micros(a, b));
    ++cell.iv_outcomes[static_cast<std::size_t>(one[0].outcome)];
    if (one[0].outcome == DiscreteDivBatchOutcome::Priced) {
      sigma_of[ci] = one[0].value;
    }
  }
  clock.lap(bench::Stage::Inverses, bench::BenchClock::now());
  return e * kTenorBuckets.size() + t;
}

// Pass 2 for one group: prepare again (untimed), then per contract the prepared price,
// the prepared Greek bundle and the cold standalone price, each at the contract's root
// (or the default sigma).
void query_group(const Group &g, const std::vector<std::size_t> &members,
                 const std::vector<Contract> &contracts, const std::vector<double> &sigma_of,
                 const DiscreteDivProductionOpts &opts, DiscreteDivBatch &batch,
                 DiscreteDivWorkspace &cold_ws, Cell &cell) {
  const DiscreteDivBatchGroup in{g.S, g.T, g.r, g.q, g.dividends};
  static_cast<void>(batch.prepare(std::span<const DiscreteDivBatchGroup>(&in, 1)));
  std::array<DiscreteDivBatchResult, 1> one{};
  std::array<DiscreteDivProductionGreeks, 1> bundle{};
  for (const std::size_t ci : members) {
    const Contract &c = contracts[ci];
    const DiscreteDivBatchContract bc{0U, c.K, c.side, sigma_of[ci], c.quote};
    const std::span<const DiscreteDivBatchContract> just(&bc, 1);
    auto a = bench::BenchClock::now();
    const atx::vol::Status priced = batch.price(just, ExerciseStyle::American, one);
    auto b = bench::BenchClock::now();
    if (!priced) {
      throw std::runtime_error("price: " + priced.error().to_string());
    }
    cell.price_us.push_back(micros(a, b));
    ++cell.price_outcomes[static_cast<std::size_t>(one[0].outcome)];
    a = bench::BenchClock::now();
    const atx::vol::Status bundled =
        batch.greeks(just, ExerciseStyle::American, kRollHorizon, bundle);
    b = bench::BenchClock::now();
    if (!bundled) {
      throw std::runtime_error("greeks: " + bundled.error().to_string());
    }
    cell.bundle_us.push_back(micros(a, b));
    cell.bundle_refused += std::isfinite(bundle[0].price) ? 0U : 1U;
    a = bench::BenchClock::now();
    const auto cold = atx::vol::american_discrete_div_production_price(
        g.S, c.K, g.T, sigma_of[ci], g.r, g.q, c.side, g.dividends, ExerciseStyle::American,
        opts, cold_ws);
    b = bench::BenchClock::now();
    cell.cold_price_us.push_back(micros(a, b));
    cell.cold_price_refused += cold ? 0U : 1U;
  }
}

[[nodiscard]] int run(const Arguments &args) {
  std::vector<Group> groups;
  std::vector<Contract> contracts;
  load(args.input, groups, contracts);
  std::vector<std::vector<std::size_t>> members(groups.size());
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    members[contracts[i].group].push_back(i);
  }

  DiscreteDivProductionOpts opts;
  opts.accuracy = args.accuracy;
  DiscreteDivWorkspace cold_ws(atx::vol::kDiscreteDivMaxRouteSteps,
                               atx::vol::kDiscreteDivMaxPreSteps);
  if (args.fill_quote_sigma) {
    for (Contract &c : contracts) {
      if (std::isfinite(c.quote)) {
        continue;
      }
      const Group &g = groups[c.group];
      const auto p = atx::vol::american_discrete_div_production_price(
          g.S, c.K, g.T, *args.fill_quote_sigma, g.r, g.q, c.side, g.dividends,
          ExerciseStyle::American, opts, cold_ws);
      c.quote = p ? p->price : kNaN;
    }
  }

  if (args.affinity) {
    bench::pin_to_logical_cpu(*args.affinity);
    if (args.require_pcore) {
      const auto performance = bench::is_performance_cpu(*args.affinity);
      if (!performance.has_value() || !*performance) {
        throw std::runtime_error("--require-pcore: logical CPU " +
                                 std::to_string(*args.affinity) + " is not a performance core");
      }
    }
  }
  const bench::CpuSample cpus_before = bench::sample_cpus();
  const auto t_all = bench::BenchClock::now();

  Grid cells{};
  // Pass 1, the cold group calibration, one contiguous span on B0's clock: per group,
  // prepare (family_build_validation) then its inversions (inverses).
  bench::PhaseClock clock;
  clock.arm();
  DiscreteDivBatch batch(opts);
  std::vector<double> sigma_of(contracts.size(), args.sigma);
  std::vector<std::size_t> bucket_of(groups.size(), 0U);
  const auto t_cold = bench::BenchClock::now();
  clock.start(t_cold);
  for (std::size_t gi = 0; gi < groups.size(); ++gi) {
    bucket_of[gi] = cold_group(groups[gi], members[gi], contracts, args.sigma, batch, clock,
                               cells, sigma_of);
  }
  const auto t_cold_end = bench::BenchClock::now();
  clock.skip(t_cold_end);
  const double cold_s = bench::seconds_between(t_cold, t_cold_end);
  // Pass 2, prepared queries and the cold standalone price (not on the cold clock).
  for (std::size_t gi = 0; gi < groups.size(); ++gi) {
    const std::size_t e = bucket_of[gi] / kTenorBuckets.size();
    const std::size_t t = bucket_of[gi] % kTenorBuckets.size();
    query_group(groups[gi], members[gi], contracts, sigma_of, opts, batch, cold_ws, cells[e][t]);
  }
  const auto t_end = bench::BenchClock::now();
  const bench::CpuSample cpus_after = bench::sample_cpus();

  std::printf("| events | tenor | groups | contracts | setup us p50 / p99 | cold price | "
              "price | IV | bundle | quoted / priced / price_only / refused / budget | "
              "prices |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
  Json rows = Json::array();
  std::array<Cell, kEventBuckets.size()> by_events{};
  Cell total;
  for (std::size_t e = 0; e < kEventBuckets.size(); ++e) {
    for (std::size_t t = 0; t < kTenorBuckets.size(); ++t) {
      const Cell &c = cells[e][t];
      if (c.groups == 0U) {
        continue;
      }
      print_row(kEventBuckets[e], kTenorBuckets[t], c);
      Json row = cell_json(c);
      row["events"] = kEventBuckets[e];
      row["tenor"] = kTenorBuckets[t];
      rows.push_back(std::move(row));
      merge(by_events[e], c);
      merge(total, c);
    }
  }
  Json event_rows = Json::array();
  for (std::size_t e = 0; e < kEventBuckets.size(); ++e) {
    if (by_events[e].groups == 0U) {
      continue;
    }
    print_row(kEventBuckets[e], "all", by_events[e]);
    Json row = cell_json(by_events[e]);
    row["events"] = kEventBuckets[e];
    row["tenor"] = "all";
    event_rows.push_back(std::move(row));
  }
  print_row("all", "all", total);

  Json report;
  report["tool"] = "atx-vol-ddiv-latency";
  report["input"] = args.input;
  report["accuracy"] = args.accuracy == DiscreteDivAccuracy::Standard ? "standard" : "refined";
  report["default_sigma"] = args.sigma;
  report["fill_quote_sigma"] = args.fill_quote_sigma ? Json(*args.fill_quote_sigma) : Json();
  report["groups"] = groups.size();
  report["contracts"] = contracts.size();
  report["rows"] = std::move(rows);
  report["rows_by_events"] = std::move(event_rows);
  report["total"] = cell_json(total);
  report["cold_group_calibration"] = bench::stages_json(clock, cold_s, /*family_built=*/true);
  report["cold_group_calibration"]["scope"] =
      "pass 1, one contiguous span over every group: DiscreteDivBatch::prepare of the group "
      "alone (family_build_validation), then its contracts' implied_vol one by one "
      "(inverses); the per-call stamps inside the span are part of the inverses lap";
  report["wall_s"] = bench::seconds_between(t_all, t_end);
  report["build"] = bench::build_info_json();
  report["affinity"] =
      bench::affinity_json(args.affinity, args.require_pcore, cpus_before, cpus_after);
  report["timing_scope"] =
      "per call: steady_clock around one entry on one contract (one thread, the workspace "
      "and batch reused); p50 / p99 over the contracts of the row";
  std::ofstream out(args.output);
  if (!out) {
    throw std::runtime_error("cannot write " + args.output);
  }
  out << report.dump(2) << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(arguments(argc, argv));
  } catch (const std::exception &error) {
    std::cerr << "ddiv-latency: " << error.what() << '\n';
    return 1;
  }
}
