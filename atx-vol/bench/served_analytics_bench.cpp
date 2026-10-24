// Served-analytics bench - lane A1 (r6, 2026-10-23): the master plan's A1 row.
//
// Question: on a FITTED SPY expiry served from the live book, what does a scenario matrix
// cost by Taylor and by exact repricing, how far apart are the two (the error domain), what
// does each cell leave out (failed exposure), what does it hold in memory, and how do the
// Taylor / exact values behave next to the exercise boundary and across an ex-dividend date?
//
// ── Input (environment; no input, no rows) ──────────────────────────────────────────────
//   ATX_VOL_SERVE_RECORD_SPY23=<csv>  S,K,T,r,q,is_call,bid,ask of one SPY expiry, fitted
//       exactly as serve_record_bench's `spy23` dataset (fit_quote_surface, the first library
//       profile that serves it, funding-PV dividends), its record built with the default
//       ServeRecordPolicy and published in a ServeBook (no instants).
//
// ── Workloads (fixed before anything is timed; printed as `workload` lines) ────────────
//   chain       every node side of the record, quantity 1, multiplier 100;
//   p300        300 legs on the 150 node sides nearest the money (two legs each; quantities
//               from a fixed LCG in [-10, 10] \ {0}), multiplier 100;
//   matrix      spot_rel {-2, -1, 0, +1, +2} % x vol_abs {-1, 0, +1} vol pts x time {0, 1 d}
//               (1 / 365 of the variance clock) = 30 cells; base spot S_fit x 1.0021 (a
//               moved spot: no answer is the certified node bytes); Taylor box (3 %, 3 vol
//               pts, 7 d); cold kernel preset = the fit's AlOpts; tiers Ladder..Family.
//   boundary    the chain's puts with base edge_sd < 1 or Exercised, spot_rel -3..+3 % by 1
//               %, vol {0}, time {0}: the near-boundary table;
//   exdiv       the 12 node sides nearest the money at their record sigma, on D1's
//               production lattice with a DECLARED (synthetic) cash dividend of 1.80 USD at
//               half the expiry: spot_rel {-1, 0, +1} %, time {0, T / 4, 3 T / 4}.
//
// ── Rows a1/spy23/<row> (Iterations(1); every number is printed and set as a counter) ──
//   chain        one Both run (keep_legs): the scenario table, the error domain |Taylor -
//                exact| per (contract, cell) by cell class, the remainder coverage and the
//                failed exposure per reason; then per-contract latency (a one-leg portfolio
//                per unique, Taylor and Exact) p50 / p99 / max, and whole-matrix wall time
//                (7 runs each); memory (result bytes, record bytes, working set).
//   p300         the same for the 300-leg portfolio, plus bit-identity across workers 1/2/4.
//   boundary     the near-boundary table.
//   exdiv        the ex-dividend table (D1 route).
// Timings use steady_clock per call on the bench main thread (bench_main pins it to a P
// core). They are development rows on a shared host unless the run records a quiet one.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

#include "atx/vol/api/analytics/served_analytics.hpp"
#include "atx/vol/api/fitting/quote_fit_policy.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

namespace {

using namespace atx::vol;
using Clock = std::chrono::steady_clock;

constexpr double kSpotMove = 1.0021;
constexpr std::int64_t kNow = 1'800'000'000'000'000'000;

[[nodiscard]] std::string env_text(const char *name) {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, name) != 0 || raw == nullptr) return {};
  std::string s(raw);
  std::free(raw);
  return s;
#else
  const char *raw = std::getenv(name);
  return raw != nullptr ? std::string(raw) : std::string();
#endif
}

[[nodiscard]] double working_set_mb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
  }
#endif
  return std::nan("");
}

[[nodiscard]] double us_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

struct Stats {
  std::size_t n = 0;
  double p50 = std::nan("");
  double p99 = std::nan("");
  double max = std::nan("");
  double mean = std::nan("");
};

[[nodiscard]] Stats stats_of(std::vector<double> v) {
  Stats s;
  s.n = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  const auto at = [&v](double q) {
    const double pos = q * static_cast<double>(v.size() - 1);
    return v[static_cast<std::size_t>(std::llround(pos))];
  };
  s.p50 = at(0.50);
  s.p99 = at(0.99);
  s.max = v.back();
  double sum = 0.0;
  for (const double x : v) sum += x;
  s.mean = sum / static_cast<double>(v.size());
  return s;
}

// ── The dataset (serve_record_bench's spy23 fit) ─────────────────────────────────────────

struct Spy23 {
  bool ok = false;
  std::string error = "ATX_VOL_SERVE_RECORD_SPY23 unset";
  std::string label;
  QuoteSliceFit fit;
  std::shared_ptr<const SliceServeRecord> record;
  std::unique_ptr<ServeBook> book;
  double fit_s = 0.0;
  double build_s = 0.0;
};

[[nodiscard]] std::vector<double> csv_numbers(const std::string &line) {
  std::vector<double> out;
  std::stringstream ss(line);
  std::string cell;
  while (std::getline(ss, cell, ',')) out.push_back(std::strtod(cell.c_str(), nullptr));
  return out;
}

void fit_spy23(Spy23 &d, const QuoteSliceMarket &market, std::span<const QuoteSliceQuote> quotes) {
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 0.5;
  // QuoteSurfaceExpiry borrows the quotes (a span): `quotes` outlives the fit.
  const std::array<QuoteSurfaceExpiry, 1> expiries{QuoteSurfaceExpiry{market, quotes, {}, {}}};
  d.error = "spy23: no profile serves the expiry";
  const auto t0 = Clock::now();
  for (const QuoteSliceRepairMode repair :
       {QuoteSliceRepairMode::Off, QuoteSliceRepairMode::Channel}) {
    for (const QuoteFitProfile &profile : default_quote_fit_profiles()) {
      QuoteSliceConfig config = profile.config;
      config.channel.repair_half_spreads = 0.5;
      config.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
      config.repair.mode = repair;
      auto fitted = fit_quote_surface(expiries, config, surface);
      if (!fitted) continue;
      QuoteSurfaceSlice &slice = fitted->front();
      if (slice.grade == QuoteSliceGrade::Refused || !slice.fit.prices) continue;
      d.label = profile.name + (repair == QuoteSliceRepairMode::Channel ? "+channel_repair" : "") +
                " grade " + to_string(slice.grade);
      d.fit = std::move(slice.fit);
      d.fit_s = us_since(t0) * 1e-6;
      d.ok = true;
      return;
    }
  }
}

[[nodiscard]] const Spy23 &spy23() {
  static const std::unique_ptr<Spy23> d = [] {
    auto out = std::make_unique<Spy23>();
    const std::string path = env_text("ATX_VOL_SERVE_RECORD_SPY23");
    if (path.empty()) return out;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line); // header S,K,T,r,q,is_call,bid,ask
    std::vector<QuoteSliceQuote> quotes;
    std::vector<double> yields;
    QuoteSliceMarket market{};
    while (std::getline(in, line)) {
      const std::vector<double> v = csv_numbers(line);
      if (v.size() < 8) continue;
      market = QuoteSliceMarket{v[0], v[2], v[2], v[3], 0.0, 0.0};
      quotes.push_back({v[1], v[5] != 0.0 ? Side::Call : Side::Put, v[6], v[7], v[4]});
      yields.push_back(v[4]);
    }
    if (quotes.empty()) return out;
    std::sort(yields.begin(), yields.end());
    market.continuous_yield = yields[yields.size() / 2];
    fit_spy23(*out, market, quotes);
    if (!out->ok) return out;
    const auto t0 = Clock::now();
    auto built = build_serve_record(out->fit, ServeRecordPolicy{});
    out->build_s = us_since(t0) * 1e-6;
    if (!built) {
      out->ok = false;
      out->error = "spy23: build_serve_record refused";
      return out;
    }
    out->record = *built;
    ServeBookPolicy policy;
    policy.build_workers = 0;
    out->book = std::make_unique<ServeBook>(policy);
    ServeBookEntry e;
    e.expiry_id = 23;
    e.record = out->record;
    e.tag = 2023;
    if (!out->book->publish_expiry(e)) {
      out->ok = false;
      out->error = "spy23: publish refused";
    }
    return out;
  }();
  return *d;
}

// ── Workloads ────────────────────────────────────────────────────────────────────────────

[[nodiscard]] std::vector<ServedPosition> chain_workload(const SliceServeRecord &r) {
  std::vector<ServedPosition> p;
  std::uint64_t id = 1;
  for (const ServedNode &n : r.nodes()) {
    p.push_back({id++, 23, n.strike, Side::Call, 1.0, 100.0});
    p.push_back({id++, 23, n.strike, Side::Put, 1.0, 100.0});
  }
  return p;
}

[[nodiscard]] std::vector<ServedPosition> p300_workload(const SliceServeRecord &r) {
  std::vector<ServedPosition> sides = chain_workload(r);
  const double S = r.spot();
  const auto nearer = [S](const ServedPosition &a, const ServedPosition &b) {
    return std::abs(std::log(a.strike / S)) < std::abs(std::log(b.strike / S));
  };
  std::stable_sort(sides.begin(), sides.end(), nearer);
  sides.resize(std::min<std::size_t>(sides.size(), 150));
  std::uint64_t state = 0x2545F4914F6CDD1Dull;
  const auto qty = [&state]() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const int q = static_cast<int>((state >> 33) % 21) - 10;
    return q == 0 ? 1.0 : static_cast<double>(q);
  };
  std::vector<ServedPosition> p;
  std::uint64_t id = 1;
  for (int pass = 0; pass < 2; ++pass) {
    for (const ServedPosition &s : sides) {
      ServedPosition leg = s;
      leg.leg_id = id++;
      leg.quantity = qty();
      p.push_back(leg);
    }
  }
  return p;
}

[[nodiscard]] ScenarioSpec matrix_spec(const Spy23 &d) {
  ScenarioSpec s;
  s.spot_rel = {-0.02, -0.01, 0.0, 0.01, 0.02};
  s.vol_abs = {-0.01, 0.0, 0.01};
  s.time_years = {0.0, 1.0 / 365.0};
  s.spot = d.record->spot() * kSpotMove;
  s.now_ns = kNow;
  s.cold_opts = d.fit.al_opts;
  s.box = TaylorBox{0.03, 0.03, 7.0 / 365.0};
  return s;
}

void print_workload(const char *name, const ScenarioResult &r) {
  const ServedWorkload &w = r.workload;
  std::printf("workload %s: contracts %zu unique %zu greek_mask 0x%02x cells %zu (%zu spot x %zu "
              "vol x %zu time) expiries %zu events_in_horizon %zu book_version %llu\n",
              name, w.contracts, w.unique_contracts, static_cast<unsigned>(w.greek_mask.bits),
              w.n_cells, w.n_spot, w.n_vol, w.n_time, w.expiries, w.events_in_horizon,
              static_cast<unsigned long long>(r.book_version));
}

void print_cells(const char *name, const ScenarioResult &r) {
  std::printf("scenario %s: spot_rel vol_abs dt | taylor_usd exact_usd (each over its own "
              "legs) exact_cert_usd | taylor_legs exact_legs | M matched_legs taylor_usd "
              "exact_usd diff_usd remainder_usd (legs in BOTH totals: the Taylor error) | routes "
              "rec/cold/settle | excluded taylor(n,usd) exact(n,usd) by reason\n",
              name);
  for (const ScenarioCell &c : r.cell) {
    std::printf("cell %s %+.3f %+.3f %.5f | %.2f %.2f %.4f | %u %u | M %u %.2f %.2f %.4f %.4f | "
                "%u/%u/%u |",
                name, c.spot_rel, c.vol_abs, c.time_years, c.taylor_pnl, c.exact_pnl,
                c.exact_certificate_usd, c.taylor_legs, c.exact_legs, c.matched_legs,
                c.matched_taylor_pnl, c.matched_exact_pnl,
                c.matched_taylor_pnl - c.matched_exact_pnl, c.matched_remainder,
                c.exact_routes[1], c.exact_routes[2], c.exact_routes[3]);
    for (std::size_t k = 1; k < kScenarioExclusionCount; ++k) {
      if (c.taylor_excluded[k] == 0 && c.exact_excluded[k] == 0) continue;
      std::printf(" %s T(%u,%.0f) E(%u,%.0f)", to_string(static_cast<ScenarioExclusion>(k)),
                  c.taylor_excluded[k], c.taylor_excluded_usd[k], c.exact_excluded[k],
                  c.exact_excluded_usd[k]);
    }
    std::printf("\n");
  }
}

// The error domain: |Taylor - exact| per share per (unique, cell) where both are included,
// by cell class, and how often the remainder estimate covers it (with a rounding allowance
// of 1e-12 x (1 + |exact|): an exercised leg's Taylor and exact values agree to rounding
// against a zero estimate).
struct Uncovered {
  double excess = 0.0; // |taylor - exact| - remainder (per share)
  std::size_t unique = 0;
  std::size_t cell = 0;
  double err = 0.0;
  double remainder = 0.0;
};

void error_domain(const char *name, const ScenarioResult &r, std::span<const ServedPosition> p,
                  benchmark::State &state) {
  const std::size_t nc = r.cell.size();
  std::array<std::vector<Uncovered>, 4> miss;
  const char *classes[] = {"spot_only", "vol_only", "time_only", "mixed"};
  std::array<std::vector<double>, 4> err;
  std::array<std::vector<double>, 4> rel;
  std::array<std::size_t, 4> covered{};
  for (std::size_t u = 0; u * nc < r.cells.size(); ++u) {
    for (std::size_t c = 0; c < nc; ++c) {
      const ScenarioLegCell &lc = r.cells[u * nc + c];
      if (lc.taylor_exclusion != ScenarioExclusion::None ||
          lc.exact_exclusion != ScenarioExclusion::None) {
        continue;
      }
      const ScenarioCell &cell = r.cell[c];
      if (cell.spot_rel == 0.0 && cell.vol_abs == 0.0 && cell.time_years == 0.0) continue;
      const int axes = (cell.spot_rel != 0.0) + (cell.vol_abs != 0.0) + (cell.time_years != 0.0);
      const std::size_t k = axes > 1 ? 3 : cell.spot_rel != 0.0 ? 0 : cell.vol_abs != 0.0 ? 1 : 2;
      const double e = std::abs(lc.taylor - lc.exact);
      err[k].push_back(e);
      rel[k].push_back(e / std::max(std::abs(lc.exact), 1e-2));
      if (e <= lc.remainder + 1e-12 * (1.0 + std::abs(lc.exact))) {
        ++covered[k];
      } else {
        miss[k].push_back(Uncovered{e - lc.remainder, u, c, e, lc.remainder});
      }
    }
  }
  for (std::size_t k = 0; k < 4; ++k) {
    const Stats s = stats_of(err[k]);
    const Stats q = stats_of(rel[k]);
    std::printf("error_domain %s %s: n %zu |taylor-exact| per share p50 %.3e p99 %.3e max %.3e "
                "(rel to max(|exact|,0.01): p50 %.3e p99 %.3e) remainder (+1e-12 rounding) "
                "covers %zu/%zu\n",
                name, classes[k], s.n, s.p50, s.p99, s.max, q.p50, q.p99, covered[k], s.n);
    state.counters[std::string("err_") + classes[k] + "_p99"] = s.p99;
    state.counters[std::string("err_") + classes[k] + "_max"] = s.max;
    // Where the remainder estimate (Sampled) does not cover: the six worst by excess.
    std::sort(miss[k].begin(), miss[k].end(),
              [](const Uncovered &a, const Uncovered &b) { return a.excess > b.excess; });
    for (std::size_t i = 0; i < std::min<std::size_t>(6, miss[k].size()); ++i) {
      const Uncovered &m = miss[k][i];
      std::size_t l = 0;
      while (l < r.legs.size() && r.legs[l].unique_index != m.unique) ++l;
      if (l == r.legs.size()) continue;
      const ServedLegProvenance &leg = r.legs[l];
      const ScenarioCell &cell = r.cell[m.cell];
      std::printf("uncovered %s %s: K %.1f %s edge_sd %.3f tier %s sigma %.4f | cell %+.3f %+.3f "
                  "%.5f | err %.4e remainder %.4e\n",
                  name, classes[k], p[l].strike, p[l].side == Side::Call ? "C" : "P",
                  leg.edge_sd, to_string(leg.tier), leg.sigma, cell.spot_rel, cell.vol_abs,
                  cell.time_years, m.err, m.remainder);
    }
  }
}

[[nodiscard]] std::size_t result_bytes(const ScenarioResult &r) {
  return sizeof(ScenarioResult) + r.cell.capacity() * sizeof(ScenarioCell) +
         r.legs.capacity() * sizeof(ServedLegProvenance) +
         r.cells.capacity() * sizeof(ScenarioLegCell);
}

// Per-contract latency: a one-leg portfolio per unique contract of `p`, by method.
void per_contract(const char *name, const Spy23 &d, std::span<const ServedPosition> p,
                  ScenarioSpec spec, benchmark::State &state) {
  std::vector<ServedPosition> uniq;
  for (const ServedPosition &leg : p) {
    const bool seen = std::any_of(uniq.begin(), uniq.end(), [&leg](const ServedPosition &x) {
      return x.strike == leg.strike && x.side == leg.side && x.expiry_id == leg.expiry_id;
    });
    if (!seen) uniq.push_back(leg);
  }
  for (const ScenarioMethod method : {ScenarioMethod::Taylor, ScenarioMethod::Exact}) {
    spec.method = method;
    spec.keep_legs = false;
    std::vector<double> us;
    us.reserve(uniq.size());
    for (const ServedPosition &leg : uniq) {
      const auto t0 = Clock::now();
      Result<ScenarioResult> r = served_scenarios(*d.book, std::span(&leg, 1), spec);
      us.push_back(us_since(t0));
      benchmark::DoNotOptimize(r);
    }
    const Stats s = stats_of(us);
    const char *m = method == ScenarioMethod::Taylor ? "taylor" : "exact";
    std::printf("latency %s per_contract_%s (30 cells, base answer + setup included): n %zu p50 "
                "%.1f us p99 %.1f us max %.1f us mean %.1f us\n",
                name, m, s.n, s.p50, s.p99, s.max, s.mean);
    state.counters[std::string(m) + "_contract_p50_us"] = s.p50;
    state.counters[std::string(m) + "_contract_p99_us"] = s.p99;
  }
}

void whole_matrix(const char *name, const Spy23 &d, std::span<const ServedPosition> p,
                  ScenarioSpec spec, benchmark::State &state) {
  for (const ScenarioMethod method : {ScenarioMethod::Taylor, ScenarioMethod::Exact}) {
    spec.method = method;
    spec.keep_legs = false;
    std::vector<double> ms;
    std::size_t cold = 0;
    for (int run = 0; run < 7; ++run) {
      const auto t0 = Clock::now();
      const Result<ScenarioResult> r = served_scenarios(*d.book, p, spec);
      ms.push_back(us_since(t0) * 1e-3);
      if (r) cold = r->cold_solves;
    }
    const Stats s = stats_of(ms);
    const char *m = method == ScenarioMethod::Taylor ? "taylor" : "exact";
    std::printf("latency %s matrix_%s (7 runs, workers 1): median %.2f ms max %.2f ms min-run "
                "cold solves %zu\n",
                name, m, s.p50, s.max, cold);
    state.counters[std::string(m) + "_matrix_ms"] = s.p50;
  }
}

void run_workload(benchmark::State &state, const char *name,
                  std::vector<ServedPosition> (*make)(const SliceServeRecord &)) {
  const Spy23 &d = spy23();
  if (!d.ok) {
    state.SkipWithError(d.error.c_str());
    return;
  }
  for (auto _ : state) {
    const std::vector<ServedPosition> p = make(*d.record);
    ScenarioSpec spec = matrix_spec(d);
    spec.keep_legs = true;
    const double ws0 = working_set_mb();
    const auto t0 = Clock::now();
    const Result<ScenarioResult> r = served_scenarios(*d.book, p, spec);
    const double both_ms = us_since(t0) * 1e-3;
    const double ws1 = working_set_mb();
    if (!r) {
      state.SkipWithError("served_scenarios refused");
      return;
    }
    std::printf("dataset spy23: %s, fit %.2f s, record build %.2f s, record bytes %zu, nodes "
                "%zu, S_fit %.4f T %.6f\n",
                d.label.c_str(), d.fit_s, d.build_s, d.record->bytes().total(),
                d.record->nodes().size(), d.record->spot(), d.record->tau());
    print_workload(name, *r);
    std::printf("run %s Both keep_legs: %.2f ms, base serves %zu, record serves %zu, cold solves "
                "%zu, result bytes %zu, working set %.1f -> %.1f MB, status %s attempts %u\n",
                name, both_ms, r->base_serves, r->exact_record_serves, r->cold_solves,
                result_bytes(*r), ws0, ws1, to_string(r->status), r->attempts);
    std::size_t by_class[4] = {0, 0, 0, 0};
    for (const ServedLegProvenance &leg : r->legs) ++by_class[static_cast<int>(leg.input_class)];
    std::printf("inputs %s: certified %zu estimate %zu price_only %zu refused %zu (legs)\n", name,
                by_class[0], by_class[1], by_class[2], by_class[3]);
    print_cells(name, *r);
    error_domain(name, *r, p, state);
    per_contract(name, d, p, matrix_spec(d), state);
    whole_matrix(name, d, p, matrix_spec(d), state);
    state.counters["result_bytes"] = static_cast<double>(result_bytes(*r));
    state.counters["record_bytes"] = static_cast<double>(d.record->bytes().total());
    state.counters["unique"] = static_cast<double>(r->workload.unique_contracts);
    state.counters["contracts"] = static_cast<double>(r->workload.contracts);
  }
}

// Bit identity of the 300-leg matrix across workers 1 / 2 / 4.
void p300_determinism(const Spy23 &d) {
  const std::vector<ServedPosition> p = p300_workload(*d.record);
  std::vector<ScenarioResult> runs;
  for (const unsigned w : {1u, 2u, 4u}) {
    ScenarioSpec spec = matrix_spec(d);
    spec.workers = w;
    Result<ScenarioResult> r = served_scenarios(*d.book, p, spec);
    if (r) runs.push_back(std::move(*r));
  }
  std::size_t differ = 0;
  for (std::size_t w = 1; w < runs.size(); ++w) {
    for (std::size_t c = 0; c < runs[0].cell.size(); ++c) {
      const ScenarioCell &a = runs[0].cell[c];
      const ScenarioCell &b = runs[w].cell[c];
      const auto same = [](double x, double y) {
        return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y);
      };
      if (!same(a.taylor_pnl, b.taylor_pnl) || !same(a.exact_pnl, b.exact_pnl) ||
          !same(a.taylor_remainder, b.taylor_remainder) || a.taylor_legs != b.taylor_legs ||
          a.exact_legs != b.exact_legs) {
        ++differ;
      }
    }
  }
  std::printf("determinism p300 workers 1/2/4: runs %zu, cells differing bitwise %zu\n",
              runs.size(), differ);
}

void BM_chain(benchmark::State &state) { run_workload(state, "chain", &chain_workload); }
void BM_p300(benchmark::State &state) {
  run_workload(state, "p300", &p300_workload);
  if (spy23().ok) p300_determinism(spy23());
}

// The near-boundary table: puts within 1 sd of the edge (or exercised) on the real chain.
void BM_boundary(benchmark::State &state) {
  const Spy23 &d = spy23();
  if (!d.ok) {
    state.SkipWithError(d.error.c_str());
    return;
  }
  for (auto _ : state) {
    ServeBookReader reader = d.book->reader();
    const double S = d.record->spot() * kSpotMove;
    std::vector<ServedPosition> p;
    std::uint64_t id = 1;
    for (const ServedNode &n : d.record->nodes()) {
      HedgeGreeksRequest q;
      q.expiry_id = 23;
      q.strike = n.strike;
      q.side = Side::Put;
      q.spot = S;
      q.now_ns = kNow;
      const HedgeGreeks h = hedge_greeks(reader, q);
      const bool close = h.serve_status == ServeStatus::Exercised ||
                         (std::isfinite(h.edge_sd) && h.edge_sd < 1.0);
      if (close) p.push_back({id++, 23, n.strike, Side::Put, 1.0, 100.0});
    }
    ScenarioSpec spec = matrix_spec(d);
    spec.spot_rel = {-0.03, -0.02, -0.01, 0.0, 0.01, 0.02, 0.03};
    spec.vol_abs = {0.0};
    spec.time_years = {0.0};
    spec.keep_legs = true;
    const Result<ScenarioResult> r = served_scenarios(*d.book, p, spec);
    if (!r) {
      state.SkipWithError("served_scenarios refused");
      return;
    }
    print_workload("boundary", *r);
    std::printf("boundary: K edge_sd status tier | per spot_rel: taylor/exact (per share) or "
                "exclusion\n");
    const std::size_t nc = r->cell.size();
    for (std::size_t l = 0; l < r->legs.size(); ++l) {
      const ServedLegProvenance &leg = r->legs[l];
      std::printf("boundary K %.1f edge_sd %.3f %s %s |", p[l].strike, leg.edge_sd,
                  to_string(leg.serve_status), to_string(leg.tier));
      for (std::size_t c = 0; c < nc; ++c) {
        const ScenarioLegCell &lc = r->cells[leg.unique_index * nc + c];
        if (lc.taylor_exclusion == ScenarioExclusion::None) {
          std::printf(" %+.4f/", lc.taylor);
        } else {
          std::printf(" %s/", to_string(lc.taylor_exclusion));
        }
        if (lc.exact_exclusion == ScenarioExclusion::None) {
          std::printf("%+.4f", lc.exact);
        } else {
          std::printf("%s", to_string(lc.exact_exclusion));
        }
      }
      std::printf("\n");
    }
    print_cells("boundary", *r);
    state.counters["contracts"] = static_cast<double>(p.size());
  }
}

// The ex-dividend table on D1's production lattice (declared synthetic schedule).
void BM_exdiv(benchmark::State &state) {
  const Spy23 &d = spy23();
  if (!d.ok) {
    state.SkipWithError(d.error.c_str());
    return;
  }
  for (auto _ : state) {
    const SliceServeRecord &rec = *d.record;
    const double S = rec.spot();
    std::vector<const ServedNode *> nodes;
    for (const ServedNode &n : rec.nodes()) nodes.push_back(&n);
    std::stable_sort(nodes.begin(), nodes.end(), [S](const ServedNode *a, const ServedNode *b) {
      return std::abs(std::log(a->strike / S)) < std::abs(std::log(b->strike / S));
    });
    std::vector<CashDividendContract> contracts;
    const double T = rec.tau();
    for (std::size_t i = 0; i < nodes.size() && contracts.size() < 12; ++i) {
      for (const Side side : {Side::Call, Side::Put}) {
        const ServedNodeSide &ns = side == Side::Call ? nodes[i]->call : nodes[i]->put;
        if (!(ns.sigma > 0.0) || contracts.size() >= 12) continue;
        CashDividendContract c;
        c.leg_id = contracts.size() + 1;
        c.spot = S;
        c.strike = nodes[i]->strike;
        c.tau = T;
        c.sigma = ns.sigma;
        c.rate = rec.rate();
        c.yield = 0.0;
        c.side = side;
        c.dividends = {CashDividend{0.5 * T, 1.80}};
        contracts.push_back(std::move(c));
      }
    }
    CashDividendScenarioSpec spec;
    spec.spot_rel = {-0.01, 0.0, 0.01};
    spec.vol_abs = {0.0};
    spec.time_years = {0.0, 0.25 * T, 0.75 * T};
    spec.box = TaylorBox{0.03, 0.03, 0.5 * T};
    const auto t0 = Clock::now();
    const Result<CashDividendScenarioResult> r = cash_dividend_scenarios(contracts, spec);
    const double ms = us_since(t0) * 1e-3;
    if (!r) {
      state.SkipWithError("cash_dividend_scenarios refused");
      return;
    }
    std::printf("exdiv: %zu contracts (SPY 23 d node sides at their record sigma, rate %.6f, "
                "residual yield 0, DECLARED cash 1.80 USD at T/2 = %.6f), D1 opts Standard; "
                "%.1f ms, %zu lattice solves\n",
                contracts.size(), rec.rate(), 0.5 * T, ms, r->lattice_solves);
    std::printf("exdiv: K side | base price theta(reason) | per (spot_rel, dt): taylor/exact "
                "events\n");
    for (std::size_t l = 0; l < r->legs.size(); ++l) {
      const CashDividendLeg &leg = r->legs[l];
      std::printf("exdiv K %.1f %s | %.4f theta %s |", contracts[l].strike,
                  contracts[l].side == Side::Call ? "C" : "P", leg.base.price,
                  leg.base.supported(DiscreteDivGreek::Theta) ? "served" : "unsupported");
      for (std::size_t c = 0; c < leg.cells.size(); ++c) {
        const CashDividendLegCell &lc = leg.cells[c];
        if (lc.taylor_exclusion == ScenarioExclusion::None) {
          std::printf(" %+.4f/", lc.taylor);
        } else {
          std::printf(" %s/", to_string(lc.taylor_exclusion));
        }
        if (lc.exact_exclusion == ScenarioExclusion::None) {
          std::printf("%+.4f[%d]", lc.exact, lc.events_crossed);
        } else {
          std::printf("%s", to_string(lc.exact_exclusion));
        }
      }
      std::printf("\n");
    }
    for (const ScenarioCell &c : r->cell) {
      std::printf("exdiv cell %+.3f %.6f | taylor %.2f exact %.2f | taylor legs %u exact legs %u "
                  "| M %u %.2f %.2f diff %.4f remainder %.4f",
                  c.spot_rel, c.time_years, c.taylor_pnl, c.exact_pnl, c.taylor_legs,
                  c.exact_legs, c.matched_legs, c.matched_taylor_pnl, c.matched_exact_pnl,
                  c.matched_taylor_pnl - c.matched_exact_pnl, c.matched_remainder);
      for (std::size_t k = 1; k < kScenarioExclusionCount; ++k) {
        if (c.taylor_excluded[k] == 0 && c.exact_excluded[k] == 0) continue;
        std::printf(" %s T(%u) E(%u)", to_string(static_cast<ScenarioExclusion>(k)),
                    c.taylor_excluded[k], c.exact_excluded[k]);
      }
      std::printf("\n");
    }
    state.counters["exdiv_ms"] = ms;
  }
}

BENCHMARK(BM_chain)->Name("a1/spy23/chain")->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_p300)->Name("a1/spy23/p300")->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_boundary)->Name("a1/spy23/boundary")->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_exdiv)->Name("a1/spy23/exdiv")->Iterations(1)->Unit(benchmark::kMillisecond);

} // namespace
