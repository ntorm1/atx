// Serving-record benchmark - lane H0-A (r6, 2026-10-23), deliverable 5.
//
// Question: on a FITTED surface, what does one served contract cost per tier of the
// immutable serving record (api/fitting/slice_serve_record.hpp), what does a 300-contract
// served chain cost, how fast is the tick-rate implied volatility, what does a record
// cost to build and to hold, and do its prices agree with the certified slice?
//
// ── Inputs (environment; a row whose input is unset is not registered) ──────────────
//   ATX_VOL_SERVE_RECORD_ARCHIVE=<file.atxvfa>  an ATXVFA archive; every served expiry of
//       every surface in it is built into a record (dataset `spy_surface`: the SPY surface
//       the root's graded-v1 default fits, produced by atx-vol-fit-archive-bench).
//   ATX_VOL_SERVE_RECORD_SPY23=<csv>            S,K,T,r,q,is_call,bid,ask of one expiry
//       (dataset `spy23`), fitted here by fit_quote_surface (one expiry, graded: pass A,
//       then the channel refits) under the library's "dense" profile with
//       channel.repair_half_spreads 0.5, surface calendar_repair_half_spreads 0.5 and the
//       funding-PV dividend model.
//
// ── Rows serve/<dataset>/<row> ─────────────────────────────────────────────────────────
//   tier0_price4     serve(), Ladder only, price + delta, gamma, vega, theta
//   tier1_price      serve(), Family only, price (no Greeks requested)
//   tier1_four       serve(), Family only, + delta, gamma, vega, theta
//   tier1_all        serve(), Family only, + phi, rho as well
//   tier2            serve(), Certified only (family + one AlProfile::Fast solve)
//   iv_record        implied_vol(), Ladder only
//   iv_family        implied_vol(), Family only
//   chain300_tier<t> one pass over the 300 node sides nearest the money (largest expiry)
//   build            one rebuild of every record (construction clocks, bytes, memory)
//   consistency      untimed agreement of the records with their fits
// Per-contract rows time every call on its own with the TSC (lfence-fenced rdtsc,
// calibrated against steady_clock once per process; the empty-pair overhead is the
// `timer_overhead_ns` counter and is NOT subtracted) and report p50 / p99 / p99.9 in ns;
// the spot is S_fit x (1 + 0.0021) (a moved spot inside every ladder window), so no answer
// is the certified slice price. Chain rows time the whole pass (UseManualTime). Timings
// are unqualified unless the host is quiet: the run script records the load line.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
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
#include <immintrin.h>

#include "atx/vol/api/fitting/quote_fit_policy.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "bench_util.hpp"

namespace {

using namespace atx::vol;
using atx::vol::bench::apply_common;

constexpr double kSpotMove = 1.0021;
constexpr std::size_t kChain = 300;

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

// ── TSC timing ────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::uint64_t tsc() noexcept {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

[[nodiscard]] double ns_per_tick() {
  static const double value = [] {
    const auto c0 = std::chrono::steady_clock::now();
    const std::uint64_t t0 = tsc();
    for (;;) {
      if (std::chrono::steady_clock::now() - c0 >= std::chrono::milliseconds(200)) break;
    }
    const std::uint64_t t1 = tsc();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() -
                                                               c0)
                          .count();
    return ns / static_cast<double>(t1 - t0);
  }();
  return value;
}

[[nodiscard]] double timer_overhead_ns() {
  static const double value = [] {
    std::vector<std::uint64_t> d(20000);
    for (std::uint64_t &x : d) {
      const std::uint64_t a = tsc();
      x = tsc() - a;
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[d.size() / 2]) * ns_per_tick();
  }();
  return value;
}

[[nodiscard]] double quantile(std::vector<std::uint64_t> &v, double q) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const auto at = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
  return static_cast<double>(v[at]) * ns_per_tick();
}

[[nodiscard]] double peak_working_set_mb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
#endif
  return std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] double working_set_mb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
  }
#endif
  return std::numeric_limits<double>::quiet_NaN();
}

// ── Datasets ──────────────────────────────────────────────────────────────────────────

struct Contract {
  std::size_t record = 0;
  double strike = 0.0;
  Side side = Side::Call;
  std::uint8_t tiers = 0;
};

struct Dataset {
  std::string name;
  std::vector<QuoteSliceFit> fits; // kept for the consistency row and rebuilds
  std::vector<std::shared_ptr<const SliceServeRecord>> records;
  std::vector<Contract> contracts; // every node side of every record
  std::vector<Contract> chain;     // the 300 node sides nearest the money, largest expiry
  double build_seconds = 0.0;
  double working_set_before_mb = 0.0;
  double peak_after_mb = 0.0;
  bool ok = false;
  std::string error = "dataset unavailable";
  std::string fit_label; // how the dataset was fitted (spy23)
};

void index_contracts(Dataset &d) {
  std::size_t largest = 0;
  for (std::size_t r = 0; r < d.records.size(); ++r) {
    if (d.records[r]->nodes().size() > d.records[largest]->nodes().size()) largest = r;
    for (const ServedNode &n : d.records[r]->nodes()) {
      d.contracts.push_back({r, n.strike, Side::Call, n.call.tiers});
      d.contracts.push_back({r, n.strike, Side::Put, n.put.tiers});
    }
  }
  if (d.records.empty()) return;
  std::vector<Contract> big;
  for (const Contract &c : d.contracts) {
    if (c.record == largest) big.push_back(c);
  }
  const double S = d.records[largest]->spot();
  std::stable_sort(big.begin(), big.end(), [S](const Contract &a, const Contract &b) {
    return std::abs(std::log(a.strike / S)) < std::abs(std::log(b.strike / S));
  });
  big.resize(std::min(big.size(), kChain));
  d.chain = std::move(big);
}

void build_records(Dataset &d) {
  d.working_set_before_mb = working_set_mb();
  const auto t0 = std::chrono::steady_clock::now();
  for (const QuoteSliceFit &fit : d.fits) {
    auto built = build_serve_record(fit);
    if (built) d.records.push_back(std::move(*built));
  }
  d.build_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  d.peak_after_mb = peak_working_set_mb();
  index_contracts(d);
  d.ok = !d.records.empty();
}

const Dataset &spy_surface() {
  static const Dataset d = [] {
    Dataset out;
    out.name = "spy_surface";
    const std::string path = env_text("ATX_VOL_SERVE_RECORD_ARCHIVE");
    if (path.empty()) return out;
    auto archive = FitArchive::open(path, {FitArchiveOpenMode::Read, FitArchiveVerify::Paranoid});
    if (!archive) return out;
    for (const FitArchiveSurfaceRecord &surface : archive->surfaces()) {
      auto slices = decode_surface(*archive, surface);
      if (!slices) continue;
      for (QuoteSurfaceSlice &slice : *slices) {
        if (slice.grade != QuoteSliceGrade::Refused && slice.fit.prices) {
          out.fits.push_back(std::move(slice.fit));
        }
      }
    }
    build_records(out);
    return out;
  }();
  return d;
}

[[nodiscard]] std::vector<double> csv_numbers(const std::string &line) {
  std::vector<double> out;
  std::stringstream ss(line);
  std::string cell;
  while (std::getline(ss, cell, ',')) out.push_back(std::strtod(cell.c_str(), nullptr));
  return out;
}

const Dataset &spy23() {
  static const Dataset d = [] {
    Dataset out;
    out.name = "spy23";
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
    // The graded admission of one expiry (pass A, then the channel refits with their
    // repair), surface settings of graded-v1; the first library profile (dense, ordinary,
    // sparse), without then with the opt-in channel repair, that serves the expiry.
    QuoteSurfaceConfig surface;
    surface.calendar_repair_half_spreads = 0.5;
    const std::array<QuoteSurfaceExpiry, 1> expiries{QuoteSurfaceExpiry{market, quotes, {}, {}}};
    out.error = "spy23: no profile serves the expiry";
    for (const QuoteSliceRepairMode repair : {QuoteSliceRepairMode::Off,
                                              QuoteSliceRepairMode::Channel}) {
      for (const QuoteFitProfile &profile : default_quote_fit_profiles()) {
        QuoteSliceConfig config = profile.config;
        config.channel.repair_half_spreads = 0.5;
        config.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
        config.repair.mode = repair;
        auto fitted = fit_quote_surface(expiries, config, surface);
        if (!fitted) continue;
        QuoteSurfaceSlice &slice = fitted->front();
        if (slice.grade == QuoteSliceGrade::Refused || !slice.fit.prices) {
          out.error += "; " + profile.name + (repair == QuoteSliceRepairMode::Channel
                                                  ? "+channel_repair: "
                                                  : ": ") +
                       slice.refusal;
          continue;
        }
        out.fit_label = profile.name + (repair == QuoteSliceRepairMode::Channel
                                            ? "+channel_repair"
                                            : "") +
                        " grade " + to_string(slice.grade);
        out.fits.push_back(std::move(slice.fit));
        build_records(out);
        return out;
      }
    }
    return out;
  }();
  return d;
}

void add_context_once() {
  static const bool done = [] {
    benchmark::AddCustomContext("serve_record_tsc_ns_per_tick", std::to_string(ns_per_tick()));
    benchmark::AddCustomContext("serve_record_timer_overhead_ns",
                                std::to_string(timer_overhead_ns()));
    benchmark::AddCustomContext("serve_record_archive", env_text("ATX_VOL_SERVE_RECORD_ARCHIVE"));
    benchmark::AddCustomContext("serve_record_spy23", env_text("ATX_VOL_SERVE_RECORD_SPY23"));
    return true;
  }();
  static_cast<void>(done);
}

// ── Per-contract rows ─────────────────────────────────────────────────────────────────

enum class Op : std::uint8_t { Serve, Iv };

struct RowSpec {
  Op op = Op::Serve;
  ServeTier tier = ServeTier::Ladder;
  ServeGreekMask greeks = ServeGreekMask::four();
  std::uint8_t needs = kServeNodeLadder; // ServedNodeSide::tiers bit a contract must carry
};

struct Prepared {
  std::vector<ServeRequest> serve;
  std::vector<ServeIvRequest> iv;
  std::vector<std::size_t> record;
};

[[nodiscard]] Prepared prepare(const Dataset &d, const std::vector<Contract> &set,
                               const RowSpec &spec) {
  Prepared p;
  for (const Contract &c : set) {
    if ((c.tiers & spec.needs) == 0u) continue;
    const SliceServeRecord &rec = *d.records[c.record];
    ServeRequest r;
    r.strike = c.strike;
    r.side = c.side;
    r.spot = rec.spot() * kSpotMove;
    r.tier = spec.tier;
    r.tier_max = spec.tier;
    r.greeks = spec.greeks;
    if (spec.op == Op::Iv) {
      // A target 2e-4 above the tier's own price at the served sigma (outside the band).
      const ServeAnswer a = rec.serve(r);
      if (a.status != ServeStatus::Ok) continue;
      p.iv.push_back({c.strike, c.side, r.spot, a.price * (1.0 + 2.0e-4), spec.tier, spec.tier});
    } else {
      if (rec.serve(r).status != ServeStatus::Ok) continue;
      p.serve.push_back(r);
    }
    p.record.push_back(c.record);
  }
  return p;
}

void run_contracts(benchmark::State &state, const Dataset &d, const RowSpec &spec) {
  if (!d.ok) {
    state.SkipWithError(d.error);
    return;
  }
  const Prepared p = prepare(d, d.contracts, spec);
  const std::size_t n = spec.op == Op::Iv ? p.iv.size() : p.serve.size();
  if (n == 0) {
    state.SkipWithError("no contract qualifies");
    return;
  }
  // Untimed: the rows inside the near-exercise-edge band (theta / gamma withheld).
  std::size_t near_edge = 0;
  double min_edge_sd = std::numeric_limits<double>::infinity();
  // Untimed (IV rows): one pass's statuses, so not_ok (summed over every timed pass) has
  // its reasons.
  std::array<std::size_t, 5> iv_status{}; // ok, unconfirmed, no_convergence, band, other
  if (spec.op == Op::Serve) {
    for (std::size_t i = 0; i < n; ++i) {
      const ServeAnswer a = d.records[p.record[i]]->serve(p.serve[i]);
      near_edge += a.near_edge ? 1u : 0u;
      if (std::isfinite(a.edge_sd)) min_edge_sd = std::min(min_edge_sd, a.edge_sd);
    }
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      const ServeStatus st = d.records[p.record[i]]->implied_vol(p.iv[i]).status;
      const std::size_t at = st == ServeStatus::Ok                ? 0u
                             : st == ServeStatus::Unconfirmed     ? 1u
                             : st == ServeStatus::NoConvergence   ? 2u
                             : st == ServeStatus::NonMonotoneBand ? 3u
                                                                  : 4u;
      ++iv_status[at];
    }
  }
  std::vector<std::uint64_t> samples;
  samples.reserve(n * static_cast<std::size_t>(state.max_iterations));
  double sink = 0.0;
  std::size_t not_ok = 0;
  for (auto _ : state) {
    std::uint64_t pass = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const SliceServeRecord &rec = *d.records[p.record[i]];
      std::uint64_t t0 = 0, t1 = 0;
      if (spec.op == Op::Iv) {
        t0 = tsc();
        const ServeIvAnswer a = rec.implied_vol(p.iv[i]);
        t1 = tsc();
        sink += a.sigma;
        not_ok += a.status != ServeStatus::Ok ? 1u : 0u;
      } else {
        t0 = tsc();
        const ServeAnswer a = rec.serve(p.serve[i]);
        t1 = tsc();
        sink += a.price + a.delta;
        not_ok += a.status != ServeStatus::Ok ? 1u : 0u;
      }
      samples.push_back(t1 - t0);
      pass += t1 - t0;
    }
    state.SetIterationTime(static_cast<double>(pass) * ns_per_tick() * 1.0e-9);
  }
  benchmark::DoNotOptimize(sink);
  state.counters["contracts"] = static_cast<double>(n);
  state.counters["not_ok"] = static_cast<double>(not_ok);
  if (spec.op == Op::Serve) {
    state.counters["near_edge_rows"] = static_cast<double>(near_edge);
    if (std::isfinite(min_edge_sd)) state.counters["min_edge_sd"] = min_edge_sd;
  } else {
    state.counters["iv_ok"] = static_cast<double>(iv_status[0]);
    state.counters["iv_unconfirmed"] = static_cast<double>(iv_status[1]);
    state.counters["iv_no_convergence"] = static_cast<double>(iv_status[2]);
    state.counters["iv_non_monotone_band"] = static_cast<double>(iv_status[3]);
    state.counters["iv_other"] = static_cast<double>(iv_status[4]);
  }
  state.counters["timer_overhead_ns"] = timer_overhead_ns();
  state.counters["p50_ns"] = quantile(samples, 0.50);
  state.counters["p99_ns"] = quantile(samples, 0.99);
  state.counters["p999_ns"] = quantile(samples, 0.999);
}

void run_chain(benchmark::State &state, const Dataset &d, const RowSpec &spec) {
  if (!d.ok) {
    state.SkipWithError(d.error);
    return;
  }
  const Prepared p = prepare(d, d.chain, spec);
  if (p.serve.empty()) {
    state.SkipWithError("no contract qualifies");
    return;
  }
  std::vector<std::uint64_t> passes;
  passes.reserve(static_cast<std::size_t>(state.max_iterations));
  double sink = 0.0;
  for (auto _ : state) {
    const std::uint64_t t0 = tsc();
    for (std::size_t i = 0; i < p.serve.size(); ++i) {
      const ServeAnswer a = d.records[p.record[i]]->serve(p.serve[i]);
      sink += a.price + a.delta;
    }
    const std::uint64_t t1 = tsc();
    passes.push_back(t1 - t0);
    state.SetIterationTime(static_cast<double>(t1 - t0) * ns_per_tick() * 1.0e-9);
  }
  benchmark::DoNotOptimize(sink);
  state.counters["contracts"] = static_cast<double>(p.serve.size());
  state.counters["chain_p50_us"] = quantile(passes, 0.50) * 1.0e-3;
  state.counters["chain_p99_us"] = quantile(passes, 0.99) * 1.0e-3;
}

// ── Build and consistency rows ────────────────────────────────────────────────────────

void run_build(benchmark::State &state, const Dataset &d) {
  if (!d.ok) {
    state.SkipWithError(d.error);
    return;
  }
  ServeBuildClocks sum{};
  ServeRecordBytes bytes{};
  std::size_t nodes = 0, ladders = 0, ineligible = 0, families = 0, max_bytes = 0;
  double max_total_us = 0.0;
  for (auto _ : state) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<const SliceServeRecord>> rebuilt;
    for (const QuoteSliceFit &fit : d.fits) {
      auto r = build_serve_record(fit);
      if (r) rebuilt.push_back(std::move(*r));
    }
    state.SetIterationTime(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    sum = ServeBuildClocks{};
    bytes = ServeRecordBytes{};
    nodes = ladders = ineligible = families = max_bytes = 0;
    max_total_us = 0.0;
    for (const auto &r : rebuilt) {
      const ServeBuildClocks &c = r->clocks();
      sum.identify_us += c.identify_us;
      sum.family_us += c.family_us;
      sum.ladder_us += c.ladder_us;
      sum.audit_us += c.audit_us;
      sum.digest_us += c.digest_us;
      sum.total_us += c.total_us;
      sum.lazy_family_builds += c.lazy_family_builds;
      sum.family_evaluations += c.family_evaluations;
      sum.reimplied_nodes += c.reimplied_nodes;
      max_total_us = std::max(max_total_us, c.total_us);
      const ServeRecordBytes &b = r->bytes();
      bytes.nodes += b.nodes;
      bytes.ladders += b.ladders;
      bytes.families += b.families;
      bytes.geometries += b.geometries;
      bytes.fixed += b.fixed;
      max_bytes = std::max(max_bytes, b.total());
      nodes += r->nodes().size();
      ladders += r->support().call.ladders + r->support().put.ladders;
      ineligible += r->support().call.ladders_ineligible + r->support().put.ladders_ineligible;
      families += (r->support().call.family ? 1u : 0u) + (r->support().put.family ? 1u : 0u);
    }
  }
  const double n = static_cast<double>(d.records.size());
  state.counters["expiries"] = n;
  state.counters["nodes"] = static_cast<double>(nodes);
  state.counters["ladders"] = static_cast<double>(ladders);
  state.counters["ladders_ineligible"] = static_cast<double>(ineligible);
  state.counters["families"] = static_cast<double>(families);
  state.counters["identify_ms"] = sum.identify_us * 1.0e-3;
  state.counters["family_ms"] = sum.family_us * 1.0e-3;
  state.counters["ladder_ms"] = sum.ladder_us * 1.0e-3;
  state.counters["audit_ms"] = sum.audit_us * 1.0e-3;
  state.counters["digest_ms"] = sum.digest_us * 1.0e-3;
  state.counters["total_ms"] = sum.total_us * 1.0e-3;
  state.counters["max_expiry_ms"] = max_total_us * 1.0e-3;
  state.counters["lazy_family_builds"] = static_cast<double>(sum.lazy_family_builds);
  state.counters["family_evaluations"] = static_cast<double>(sum.family_evaluations);
  state.counters["reimplied_nodes"] = static_cast<double>(sum.reimplied_nodes);
  state.counters["bytes_symbol"] = static_cast<double>(bytes.total());
  state.counters["bytes_expiry_mean"] = static_cast<double>(bytes.total()) / n;
  state.counters["bytes_expiry_max"] = static_cast<double>(max_bytes);
  state.counters["bytes_nodes"] = static_cast<double>(bytes.nodes);
  state.counters["bytes_ladders"] = static_cast<double>(bytes.ladders);
  state.counters["bytes_families"] = static_cast<double>(bytes.families);
  state.counters["bytes_geometries"] = static_cast<double>(bytes.geometries);
  state.counters["bytes_fixed"] = static_cast<double>(bytes.fixed);
  // The layout: one ServedNode (both sides) and one ladder (bytes.ladders less the
  // two int32 ladder slots per node).
  state.counters["sizeof_served_node"] = static_cast<double>(sizeof(ServedNode));
  if (ladders > 0) {
    state.counters["bytes_per_ladder"] =
        static_cast<double>(bytes.ladders - 2 * nodes * sizeof(std::int32_t)) /
        static_cast<double>(ladders);
  }
  state.counters["first_build_s"] = d.build_seconds;
  state.SetLabel(d.fit_label);
  state.counters["ws_before_mb"] = d.working_set_before_mb;
  state.counters["peak_after_first_build_mb"] = d.peak_after_mb;
}

void run_consistency(benchmark::State &state, const Dataset &d) {
  if (!d.ok) {
    state.SkipWithError(d.error);
    return;
  }
  std::size_t node_sides = 0, node_bits_equal = 0, node_answers = 0;
  std::size_t between = 0, between_within = 0;
  double between_ratio_max = 0.0, delta_family_max = 0.0, delta_ladder_max = 0.0;
  double cert_max = 0.0;
  struct Worst {
    double delta = 0.0, strike = 0.0;
    bool call = false;
    double price = 0.0, logm = 0.0, sigma = 0.0, tau = 0.0;
  } worst;
  std::vector<double> deltas;
  for (auto _ : state) {
    for (std::size_t r = 0; r < d.records.size(); ++r) {
      const SliceServeRecord &rec = *d.records[r];
      const QuoteSliceFit &fit = d.fits[r];
      const auto slice = fit.prices->nodes();
      for (std::size_t i = 0; i < slice.size(); ++i) {
        for (const Side side : {Side::Call, Side::Put}) {
          ++node_sides;
          const double certified = side == Side::Call ? slice[i].call : slice[i].put;
          const ServedNodeSide &s = side == Side::Call ? rec.nodes()[i].call : rec.nodes()[i].put;
          if (std::isfinite(s.ladder_certificate_usd)) {
            cert_max = std::max(cert_max, s.ladder_certificate_usd);
          }
          if (std::isfinite(s.family_price_delta)) {
            deltas.push_back(std::abs(s.family_price_delta));
            if (std::abs(s.family_price_delta) > worst.delta) {
              worst = {std::abs(s.family_price_delta), slice[i].strike, side == Side::Call,
                       certified, std::log(slice[i].strike / rec.spot()), s.sigma, rec.tau()};
            }
          }
          for (const ServeTier t : {ServeTier::Ladder, ServeTier::Family, ServeTier::Certified}) {
            ServeRequest q;
            q.strike = slice[i].strike;
            q.side = side;
            q.spot = rec.spot();
            q.tier = t;
            q.tier_max = t;
            const ServeAnswer a = rec.serve(q);
            if (!std::isfinite(a.price)) continue;
            ++node_answers;
            node_bits_equal += std::bit_cast<std::uint64_t>(a.price) ==
                                       std::bit_cast<std::uint64_t>(certified)
                                   ? 1u
                                   : 0u;
            if (t == ServeTier::Family && std::isfinite(a.price_map_delta)) {
              delta_family_max = std::max(delta_family_max, a.price_map_delta);
            }
            if (t == ServeTier::Ladder && a.status == ServeStatus::Ok) {
              delta_ladder_max = std::max(delta_ladder_max, a.price_map_delta);
            }
          }
          if (i + 1 == slice.size()) continue;
          const ServeSideSupport &sup = side == Side::Call ? rec.support().call
                                                           : rec.support().put;
          for (int m = 1; m <= 7; ++m) {
            ServeRequest q;
            q.strike = slice[i].strike + (slice[i + 1].strike - slice[i].strike) * m / 8.0;
            q.side = side;
            q.spot = rec.spot();
            q.tier = ServeTier::Family;
            q.tier_max = ServeTier::Family;
            const ServeAnswer a = rec.serve(q);
            if (!std::isfinite(a.price_map_delta) || !(sup.between_node_bound_usd > 0.0)) continue;
            ++between;
            between_within += a.price_map_delta <= sup.between_node_bound_usd ? 1u : 0u;
            between_ratio_max =
                std::max(between_ratio_max, a.price_map_delta / sup.between_node_bound_usd);
          }
        }
      }
    }
  }
  state.counters["node_sides"] = static_cast<double>(node_sides);
  state.counters["node_answers"] = static_cast<double>(node_answers);
  state.counters["node_bits_equal"] = static_cast<double>(node_bits_equal);
  state.counters["between_points"] = static_cast<double>(between);
  state.counters["between_within_bound"] = static_cast<double>(between_within);
  state.counters["between_ratio_max"] = between_ratio_max;
  state.counters["price_map_delta_family_max"] = delta_family_max;
  state.counters["price_map_delta_ladder_max"] = delta_ladder_max;
  state.counters["ladder_cert_max_usd"] = cert_max;
  std::sort(deltas.begin(), deltas.end());
  if (!deltas.empty()) {
    state.counters["family_delta_p50_usd"] = deltas[deltas.size() / 2];
    state.counters["family_delta_p99_usd"] = deltas[deltas.size() * 99 / 100];
  }
  state.counters["family_delta_worst_strike"] = worst.strike;
  state.counters["family_delta_worst_is_call"] = worst.call ? 1.0 : 0.0;
  state.counters["family_delta_worst_price"] = worst.price;
  state.counters["family_delta_worst_logm"] = worst.logm;
  state.counters["family_delta_worst_sigma"] = worst.sigma;
  state.counters["family_delta_worst_tau"] = worst.tau;
}

// ── Registration ──────────────────────────────────────────────────────────────────────

void register_dataset(const char *name, const Dataset &(*get)()) {
  const std::string base = std::string("serve/") + name + "/";
  const struct {
    const char *row;
    RowSpec spec;
    benchmark::IterationCount iterations;
  } rows[] = {
      {"tier0_price4", {Op::Serve, ServeTier::Ladder, ServeGreekMask::four(), kServeNodeLadder},
       200},
      {"tier1_price", {Op::Serve, ServeTier::Family, ServeGreekMask::none(), kServeNodeFamily},
       20},
      {"tier1_four", {Op::Serve, ServeTier::Family, ServeGreekMask::four(), kServeNodeFamily},
       20},
      {"tier1_all", {Op::Serve, ServeTier::Family, ServeGreekMask::all(), kServeNodeFamily}, 20},
      {"tier2", {Op::Serve, ServeTier::Certified, ServeGreekMask::four(), kServeNodeFamily}, 2},
      {"iv_record", {Op::Iv, ServeTier::Ladder, ServeGreekMask::none(), kServeNodeLadder}, 200},
      {"iv_family", {Op::Iv, ServeTier::Family, ServeGreekMask::none(), kServeNodeFamily}, 10},
  };
  for (const auto &row : rows) {
    const RowSpec spec = row.spec;
    apply_common(benchmark::RegisterBenchmark((base + row.row).c_str(),
                                              [get, spec](benchmark::State &state) {
                                                run_contracts(state, get(), spec);
                                              }))
        ->UseManualTime()
        ->Iterations(row.iterations)
        ->Unit(benchmark::kMicrosecond);
  }
  const struct {
    const char *row;
    RowSpec spec;
    benchmark::IterationCount iterations;
  } chains[] = {
      {"chain300_tier0",
       {Op::Serve, ServeTier::Ladder, ServeGreekMask::four(), kServeNodeLadder}, 2000},
      {"chain300_tier1",
       {Op::Serve, ServeTier::Family, ServeGreekMask::four(), kServeNodeFamily}, 50},
      {"chain300_tier2",
       {Op::Serve, ServeTier::Certified, ServeGreekMask::four(), kServeNodeFamily}, 3},
  };
  for (const auto &row : chains) {
    const RowSpec spec = row.spec;
    apply_common(benchmark::RegisterBenchmark((base + row.row).c_str(),
                                              [get, spec](benchmark::State &state) {
                                                run_chain(state, get(), spec);
                                              }))
        ->UseManualTime()
        ->Iterations(row.iterations)
        ->Unit(benchmark::kMicrosecond);
  }
  benchmark::RegisterBenchmark((base + "build").c_str(),
                               [get](benchmark::State &state) { run_build(state, get()); })
      ->UseManualTime()
      ->Iterations(1)
      ->Repetitions(1)
      ->Unit(benchmark::kMillisecond);
  benchmark::RegisterBenchmark((base + "consistency").c_str(),
                               [get](benchmark::State &state) { run_consistency(state, get()); })
      ->Iterations(1)
      ->Repetitions(1)
      ->Unit(benchmark::kMillisecond);
}

const bool kRegistered = [] {
  add_context_once();
  if (!env_text("ATX_VOL_SERVE_RECORD_ARCHIVE").empty()) {
    register_dataset("spy_surface", &spy_surface);
  }
  if (!env_text("ATX_VOL_SERVE_RECORD_SPY23").empty()) register_dataset("spy23", &spy23);
  return true;
}();

} // namespace
