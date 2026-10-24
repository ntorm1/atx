// Simultaneous quote-query load (lane U0, vol-sota r6, 2026-10-23; master plan Sprint 6 U0).
//
// Question: while a universe fit runs elsewhere on the host, what does a hot quote query cost on
// published H0-A serve records (api/fitting/slice_serve_record.hpp), how often does it miss, and
// does any query ever see a torn or stale version?
//
// One process, two threads:
//   * publisher: fits one underlier's board (fit_quote_surface, P0's pool at the AUTO width capped
//     by ATX_VOL_FIT_WORKERS), builds a SliceServeRecord per served expiry, precomputes every
//     query's expected answer, and publishes the version through one
//     std::atomic<std::shared_ptr>. It alternates two input variants (the quotes as given, and
//     every bid / ask scaled by 1 + perturb) so consecutive versions answer differently. Retired
//     versions are held for kRetireDepth publications, so a reader never drops the last
//     reference (deferred reclamation).
//   * querier: open loop at --rate queries per second for --seconds. Each query reads the
//     publication sequence, loads the current version and calls serve() (tier Ladder up to Family,
//     four Greeks, spot moved by +21 bp). Timed with the TSC around load + serve. Outside the timed
//     region it checks the answer bit for bit against the version's expected answer (torn), and the
//     version against the sequence read before the load (stale).
//
// Reported: service time (load + serve) and response time (completion minus the scheduled send
// time, so a backlog is not hidden) p50 / p90 / p99 / p99.9 / max, overall and per 10-s window;
// miss rate (status not Ok, or answered above Family); deadline misses; torn and stale counts;
// rebuild clocks; and P0's pool queue ages on the publisher's re-fits (pass A: each task's start
// minus the stage's first start, from the expiry_schedule.hpp task hook, a test seam this tool
// installs).
//
// The tool scores nothing: the input is a label-free quote CSV in the quote-bench layout.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include <immintrin.h>

#include <nlohmann/json.hpp>

#include "atx/vol/api/fitting/quote_fit_policy.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "fitting/expiry_schedule.hpp" // detail::ScopedExpiryTaskHook (pass-A queue ages)

namespace {

using namespace atx::vol;
using Json = nlohmann::json;

constexpr double kSpotMove = 1.0021;
constexpr std::size_t kRetireDepth = 3;
constexpr std::size_t kMinQuotes = 6;
constexpr std::uint64_t kWindowSeconds = 10;

struct Options {
  std::string input;
  std::string report;
  std::string underlier;
  double rate = 20000.0;
  double seconds = 300.0;
  double perturb = 1.0e-3;
  double deadline_us = 1000.0;
  double min_publish_ms = 1000.0;
  std::size_t max_expiries = 8;
  bool publish = true;    // false: the first version only (a diagnostic baseline, no rebuilds)
  int pin_query_cpu = -1; // >= 0: pin the query thread to that logical CPU (diagnostic)
};

[[nodiscard]] Options parse_args(int argc, char **argv) {
  Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--input") o.input = value;
    else if (key == "--report") o.report = value;
    else if (key == "--underlier") o.underlier = value;
    else if (key == "--rate") o.rate = std::stod(value);
    else if (key == "--seconds") o.seconds = std::stod(value);
    else if (key == "--perturb") o.perturb = std::stod(value);
    else if (key == "--deadline-us") o.deadline_us = std::stod(value);
    else if (key == "--min-publish-ms") o.min_publish_ms = std::stod(value);
    else if (key == "--max-expiries") o.max_expiries = static_cast<std::size_t>(std::stoul(value));
    else if (key == "--publish") o.publish = value != "0";
    else if (key == "--pin-query-cpu") o.pin_query_cpu = std::stoi(value);
    else throw std::runtime_error("unknown argument " + std::string(key));
  }
  if (o.input.empty() || o.report.empty() || !(o.rate > 0.0) || !(o.seconds > 0.0) ||
      o.max_expiries == 0) {
    throw std::runtime_error("usage: --input quotes.csv --report out.json [--rate 20000] "
                             "[--seconds 300] [--underlier X] [--max-expiries 8] [--perturb 1e-3]");
  }
  return o;
}

// ── TSC clock ─────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::uint64_t tsc() noexcept {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

[[nodiscard]] double ticks_per_ns() {
  const auto c0 = std::chrono::steady_clock::now();
  const std::uint64_t t0 = tsc();
  while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(200)) _mm_pause();
  const std::uint64_t t1 = tsc();
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count();
  return static_cast<double>(t1 - t0) / ns;
}

// ── board loading (quote-bench CSV layout; label-free) ────────────────────────────────────────

struct ExpiryInput {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
  std::int64_t expiry_days = 0;
};

[[nodiscard]] std::vector<std::string> split_csv(std::string line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::vector<std::string> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t comma = line.find(',', start);
    std::string cell = line.substr(start, comma == std::string::npos ? std::string::npos
                                                                     : comma - start);
    if (cell.size() >= 2 && cell.front() == '"' && cell.back() == '"') {
      cell = cell.substr(1, cell.size() - 2);
    }
    out.push_back(std::move(cell));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

[[nodiscard]] double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? std::nan("") : v[v.size() / 2];
}

struct RowAcc {
  std::vector<double> spot, years, calendar, rate, sdiv, ddiv_pv;
  std::vector<QuoteSliceQuote> quotes;
};

// Eligible two-sided American rows of one underlier, grouped by expiry; the `max_expiries` expiries
// with the most quotes, in expiry order. Market fields are per-expiry medians.
[[nodiscard]] std::vector<ExpiryInput> load_board(const Options &o, std::string &underlier) {
  std::ifstream in(o.input);
  if (!in) throw std::runtime_error("cannot open " + o.input);
  std::string line;
  std::getline(in, line);
  const std::vector<std::string> header = split_csv(line);
  std::unordered_map<std::string, std::size_t> col;
  for (std::size_t i = 0; i < header.size(); ++i) col[header[i]] = i;
  for (const char *need :
       {"underlier", "expiry_days", "strike", "is_call", "exercise", "spot", "bid", "ask", "years",
        "years_calendar", "rate", "sdiv", "ddiv_pv", "input_status"}) {
    if (!col.contains(need)) throw std::runtime_error(std::string("missing column ") + need);
  }
  std::map<std::int64_t, RowAcc> by_expiry;
  while (std::getline(in, line)) {
    const std::vector<std::string> f = split_csv(line);
    if (f.size() != header.size()) throw std::runtime_error("ragged CSV row");
    if (underlier.empty()) underlier = f[col["underlier"]];
    if (f[col["underlier"]] != underlier || f[col["input_status"]] != "ok" ||
        f[col["exercise"]] != "American") {
      continue;
    }
    const double bid = std::stod(f[col["bid"]]);
    const double ask = std::stod(f[col["ask"]]);
    if (!(bid > 0.0) || !(ask >= bid) || !std::isfinite(ask)) continue;
    RowAcc &acc = by_expiry[std::stoll(f[col["expiry_days"]])];
    acc.spot.push_back(std::stod(f[col["spot"]]));
    acc.years.push_back(std::stod(f[col["years"]]));
    acc.calendar.push_back(std::stod(f[col["years_calendar"]]));
    acc.rate.push_back(std::stod(f[col["rate"]]));
    acc.sdiv.push_back(std::stod(f[col["sdiv"]]));
    acc.ddiv_pv.push_back(std::stod(f[col["ddiv_pv"]]));
    acc.quotes.push_back({std::stod(f[col["strike"]]),
                          f[col["is_call"]] == "1" ? Side::Call : Side::Put, bid, ask, {}});
  }
  std::vector<ExpiryInput> board;
  for (auto &[days, acc] : by_expiry) {
    if (acc.quotes.size() < kMinQuotes) continue;
    QuoteSliceMarket m{median(acc.spot), median(acc.years), median(acc.calendar),
                       median(acc.rate), median(acc.sdiv), median(acc.ddiv_pv)};
    board.push_back({m, std::move(acc.quotes), days});
  }
  std::stable_sort(board.begin(), board.end(), [](const ExpiryInput &a, const ExpiryInput &b) {
    return a.quotes.size() > b.quotes.size();
  });
  if (board.size() > o.max_expiries) board.resize(o.max_expiries);
  std::sort(board.begin(), board.end(), [](const ExpiryInput &a, const ExpiryInput &b) {
    return a.expiry_days < b.expiry_days;
  });
  return board;
}

// ── pass-A queue ages (P0 pool) via the expiry task hook ───────────────────────────────────────

struct TaskStart {
  std::uint64_t tsc = 0;
  std::uint32_t expiry = 0;
};
constexpr std::size_t kTraceCap = 1u << 14;
// SAFETY: the hook writes distinct slots (fetch_add index) from pool threads during one fit; the
// publisher reads them only after fit_quote_surface returned, i.e. after the pool's join.
std::array<TaskStart, kTraceCap> g_trace{};
std::atomic<std::size_t> g_trace_n{0};

void trace_hook(std::size_t expiry) {
  const std::size_t i = g_trace_n.fetch_add(1, std::memory_order_relaxed);
  if (i < kTraceCap) g_trace[i] = {tsc(), static_cast<std::uint32_t>(expiry)};
}

// Pass A covers each expiry once and joins before any later stage starts, so the trace prefix up
// to the first repeated expiry is exactly pass A; its earliest start is the stage dispatch (the
// caller runs a task first, as context 0).
[[nodiscard]] std::vector<double> pass_a_ages_us(double tpn) {
  const std::size_t n = std::min(g_trace_n.load(), kTraceCap);
  std::vector<bool> seen(1024, false);
  std::vector<std::uint64_t> starts;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t e = g_trace[i].expiry;
    if (e >= seen.size() || seen[e]) break;
    seen[e] = true;
    starts.push_back(g_trace[i].tsc);
  }
  std::vector<double> ages;
  if (starts.empty()) return ages;
  const std::uint64_t base = *std::min_element(starts.begin(), starts.end());
  for (const std::uint64_t s : starts) ages.push_back(static_cast<double>(s - base) / tpn / 1e3);
  return ages;
}

// ── versions ──────────────────────────────────────────────────────────────────────────────────

struct Query {
  std::uint32_t record = 0;
  ServeRequest request;
};

struct Expected {
  std::array<std::uint64_t, 5> bits{};
  ServeStatus status = ServeStatus::TierUnavailable;
  ServeTier tier = ServeTier::Cold;
};

struct Version {
  std::uint64_t seq = 0;
  int variant = 0;
  std::vector<std::shared_ptr<const SliceServeRecord>> records;
  std::vector<Query> queries;
  std::vector<Expected> expected;
  std::size_t expiries_served = 0;
  double fit_ms = 0.0, records_ms = 0.0, expected_ms = 0.0;
  std::vector<double> queue_ages_us;
};

[[nodiscard]] Expected expect(const ServeAnswer &a) {
  return {{std::bit_cast<std::uint64_t>(a.price), std::bit_cast<std::uint64_t>(a.delta),
           std::bit_cast<std::uint64_t>(a.gamma), std::bit_cast<std::uint64_t>(a.vega),
           std::bit_cast<std::uint64_t>(a.theta)},
          a.status,
          a.tier};
}

[[nodiscard]] bool same(const Expected &a, const Expected &b) {
  return a.bits == b.bits && a.status == b.status && a.tier == b.tier;
}

[[nodiscard]] double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

[[nodiscard]] std::vector<QuoteSurfaceExpiry>
surface_inputs(const std::vector<ExpiryInput> &board,
               std::vector<std::vector<QuoteSliceQuote>> &store, double scale) {
  store.clear();
  for (const ExpiryInput &e : board) {
    std::vector<QuoteSliceQuote> q = e.quotes;
    for (QuoteSliceQuote &x : q) {
      x.bid *= scale;
      x.ask *= scale;
    }
    store.push_back(std::move(q));
  }
  std::vector<QuoteSurfaceExpiry> out;
  for (std::size_t i = 0; i < board.size(); ++i) out.push_back({board[i].market, store[i], {}, {}});
  return out;
}

// One version: fit (pass-A queue ages traced), records, the query list and its expected answers.
[[nodiscard]] std::shared_ptr<const Version> build_version(const std::vector<ExpiryInput> &board,
                                                           std::uint64_t seq, int variant,
                                                           double perturb, double tpn) {
  auto v = std::make_shared<Version>();
  v->seq = seq;
  v->variant = variant;
  std::vector<std::vector<QuoteSliceQuote>> store;
  const std::vector<QuoteSurfaceExpiry> inputs =
      surface_inputs(board, store, variant == 0 ? 1.0 : 1.0 + perturb);
  QuoteSliceConfig config = default_quote_fit_profiles().front().config; // "dense"
  config.channel.repair_half_spreads = 0.5;
  config.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  QuoteSurfaceConfig surface;
  surface.calendar_repair_half_spreads = 0.5;
  surface.fit_workers = 0; // AUTO: P0's pool, capped by ATX_VOL_FIT_WORKERS
  auto t0 = std::chrono::steady_clock::now();
  g_trace_n.store(0);
  auto fitted = [&] {
    const detail::ScopedExpiryTaskHook hook(&trace_hook);
    return fit_quote_surface(inputs, config, surface);
  }();
  v->fit_ms = ms_since(t0);
  v->queue_ages_us = pass_a_ages_us(tpn);
  if (!fitted) throw std::runtime_error("fit_quote_surface: " + fitted.error().to_string());
  t0 = std::chrono::steady_clock::now();
  for (const QuoteSurfaceSlice &slice : *fitted) {
    if (slice.grade == QuoteSliceGrade::Refused || !slice.fit.prices) continue;
    auto built = build_serve_record(slice.fit);
    if (built) v->records.push_back(std::move(*built));
  }
  v->expiries_served = v->records.size();
  v->records_ms = ms_since(t0);
  t0 = std::chrono::steady_clock::now();
  for (std::uint32_t r = 0; r < v->records.size(); ++r) {
    const SliceServeRecord &rec = *v->records[r];
    for (const ServedNode &n : rec.nodes()) {
      for (const auto &[side, tiers] : {std::pair{Side::Call, n.call.tiers},
                                        std::pair{Side::Put, n.put.tiers}}) {
        if ((tiers & (kServeNodeLadder | kServeNodeFamily)) == 0u) continue;
        ServeRequest q;
        q.strike = n.strike;
        q.side = side;
        q.spot = rec.spot() * kSpotMove;
        q.tier = ServeTier::Ladder;
        q.tier_max = ServeTier::Family;
        q.greeks = ServeGreekMask::four();
        v->queries.push_back({r, q});
        v->expected.push_back(expect(rec.serve(q)));
      }
    }
  }
  v->expected_ms = ms_since(t0);
  if (v->queries.empty()) throw std::runtime_error("no servable query in the version");
  return v;
}

// ── statistics ────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] Json quantiles(std::vector<double> v) {
  if (v.empty()) return Json{{"n", 0}};
  std::sort(v.begin(), v.end());
  const auto q = [&v](double p) {
    const auto at = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1;
    return v[std::min(v.size() - 1, at)];
  };
  return {{"n", v.size()}, {"p50", q(0.50)}, {"p90", q(0.90)}, {"p99", q(0.99)},
          {"p999", q(0.999)}, {"p9999", q(0.9999)}, {"max", v.back()}};
}

struct QueryLog {
  std::vector<float> service_ns;
  std::array<std::vector<float>, 3> service_by_tier; // answered at Ladder, Family, other
  std::vector<float> response_ns;
  std::uint64_t misses = 0, torn = 0, stale = 0, deadline_misses = 0;
  std::vector<std::uint64_t> window_misses, window_torn;
  Json first_torn = Json::array();
};

struct Shared {
  std::atomic<std::shared_ptr<const Version>> current;
  std::atomic<std::uint64_t> published{0};
  std::atomic<bool> stop{false};
};

void query_loop(const Options &o, Shared &shared, double tpn, QueryLog &log) {
  const auto total = static_cast<std::uint64_t>(o.rate * o.seconds);
  const auto per_window = static_cast<std::uint64_t>(o.rate) * kWindowSeconds;
  const double ticks_per_query = tpn * 1e9 / o.rate;
  const double deadline_ticks = o.deadline_us * 1e3 * tpn;
  log.service_ns.reserve(total);
  log.response_ns.reserve(total);
  log.window_misses.assign(total / per_window + 1, 0);
  log.window_torn.assign(total / per_window + 1, 0);
  const std::uint64_t t0 = tsc();
  for (std::uint64_t k = 0; k < total; ++k) {
    const auto due = t0 + static_cast<std::uint64_t>(static_cast<double>(k) * ticks_per_query);
    while (tsc() < due) _mm_pause();
    const std::uint64_t a = tsc();
    const std::uint64_t seq_before = shared.published.load();
    const std::shared_ptr<const Version> v = shared.current.load();
    const std::size_t i = static_cast<std::size_t>(k % v->queries.size());
    const Query &q = v->queries[i];
    const ServeAnswer ans = v->records[q.record]->serve(q.request);
    const std::uint64_t b = tsc();
    log.service_ns.push_back(static_cast<float>(static_cast<double>(b - a) / tpn));
    const std::size_t tier = ans.tier == ServeTier::Ladder   ? 0u
                             : ans.tier == ServeTier::Family ? 1u
                                                             : 2u;
    log.service_by_tier[tier].push_back(log.service_ns.back());
    log.response_ns.push_back(static_cast<float>(static_cast<double>(b - due) / tpn));
    if (static_cast<double>(b - due) > deadline_ticks) ++log.deadline_misses;
    const Expected got = expect(ans);
    if (v->seq < seq_before) ++log.stale;
    if (ans.status != ServeStatus::Ok || ans.tier > ServeTier::Family) {
      ++log.misses;
      ++log.window_misses[k / per_window];
    }
    if (!same(got, v->expected[i])) {
      ++log.torn;
      ++log.window_torn[k / per_window];
      if (log.first_torn.size() < 10) {
        log.first_torn.push_back({{"k", k}, {"seq", v->seq}, {"i", i}});
      }
    }
  }
}

struct PublishLog {
  std::vector<Json> versions;
  std::vector<double> fit_ms, records_ms, expected_ms, total_ms, queue_ages_us;
  std::uint64_t distinguishable = 0, compared = 0;
  std::string error;
};

void publish_loop(const Options &o, const std::vector<ExpiryInput> &board, Shared &shared,
                  std::shared_ptr<const Version> first, double tpn, double start_ms,
                  std::chrono::steady_clock::time_point origin, PublishLog &log) {
  std::vector<std::shared_ptr<const Version>> retired{std::move(first)};
  for (std::uint64_t seq = 2; !shared.stop.load(); ++seq) {
    const auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<const Version> v;
    try {
      v = build_version(board, seq, static_cast<int>(seq % 2 == 0 ? 1 : 0), o.perturb, tpn);
    } catch (const std::exception &e) {
      log.error = e.what();
      return;
    }
    const double total = ms_since(t0);
    const Version &prev = *retired.back();
    const std::size_t n = std::min(prev.expected.size(), v->expected.size());
    for (std::size_t i = 0; i < n; ++i) {
      log.distinguishable += same(prev.expected[i], v->expected[i]) ? 0u : 1u;
    }
    log.compared += n;
    shared.current.store(v);
    shared.published.store(v->seq);
    log.fit_ms.push_back(v->fit_ms);
    log.records_ms.push_back(v->records_ms);
    log.expected_ms.push_back(v->expected_ms);
    log.total_ms.push_back(total);
    log.queue_ages_us.insert(log.queue_ages_us.end(), v->queue_ages_us.begin(),
                             v->queue_ages_us.end());
    log.versions.push_back({{"seq", v->seq},
                            {"variant", v->variant},
                            {"queries", v->queries.size()},
                            {"records", v->records.size()},
                            {"published_at_ms", ms_since(origin) - start_ms},
                            {"build_ms", total}});
    retired.push_back(std::move(v));
    if (retired.size() > kRetireDepth) retired.erase(retired.begin());
    const double rest = o.min_publish_ms - total;
    for (double waited = 0.0; waited < rest && !shared.stop.load(); waited += 10.0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

void pin_current_thread(int cpu) {
#if defined(_WIN32)
  if (cpu >= 0 && cpu < 64) {
    static_cast<void>(SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu));
  }
#else
  static_cast<void>(cpu);
#endif
}

[[nodiscard]] double peak_working_set_mb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
#endif
  return std::nan("");
}

[[nodiscard]] std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::milliseconds>(now));
}

[[nodiscard]] Json tier_quantiles(const QueryLog &log, std::size_t tier) {
  const std::vector<float> &v = log.service_by_tier[tier];
  return quantiles(std::vector<double>(v.begin(), v.end()));
}

[[nodiscard]] Json answer_histogram(const Version &v) {
  std::map<std::string, std::size_t> counts;
  for (const Expected &e : v.expected) {
    ++counts[std::string(to_string(e.status)) + "@" + to_string(e.tier)];
  }
  return counts;
}

[[nodiscard]] Json window_table(const QueryLog &log, const Options &o) {
  const auto per_window = static_cast<std::size_t>(o.rate) * kWindowSeconds;
  Json rows = Json::array();
  for (std::size_t w = 0; w * per_window < log.service_ns.size(); ++w) {
    const std::size_t lo = w * per_window;
    const std::size_t hi = std::min(log.service_ns.size(), lo + per_window);
    std::vector<double> s(log.service_ns.begin() + static_cast<std::ptrdiff_t>(lo),
                          log.service_ns.begin() + static_cast<std::ptrdiff_t>(hi));
    std::vector<double> r(log.response_ns.begin() + static_cast<std::ptrdiff_t>(lo),
                          log.response_ns.begin() + static_cast<std::ptrdiff_t>(hi));
    rows.push_back({{"window_s", w * kWindowSeconds}, {"service_ns", quantiles(std::move(s))},
                    {"response_ns", quantiles(std::move(r))}, {"misses", log.window_misses[w]},
                    {"torn", log.window_torn[w]}});
  }
  return rows;
}

int run(const Options &o) {
  std::string underlier = o.underlier;
  const std::vector<ExpiryInput> board = load_board(o, underlier);
  if (board.empty()) throw std::runtime_error("no expiry with enough two-sided quotes");
  const double tpn = ticks_per_ns();
  const auto origin = std::chrono::steady_clock::now();
  Shared shared;
  auto first = build_version(board, 1, 0, o.perturb, tpn);
  shared.current.store(first);
  shared.published.store(1);
  const std::string start_utc = utc_now();
  const double start_ms = ms_since(origin);
  PublishLog plog;
  QueryLog qlog;
  {
    std::jthread publisher([&] {
      if (o.publish) publish_loop(o, board, shared, first, tpn, start_ms, origin, plog);
    });
    pin_current_thread(o.pin_query_cpu);
    query_loop(o, shared, tpn, qlog);
    shared.stop.store(true);
  }
  std::vector<double> service(qlog.service_ns.begin(), qlog.service_ns.end());
  std::vector<double> response(qlog.response_ns.begin(), qlog.response_ns.end());
  const auto n = static_cast<double>(qlog.service_ns.size());
  Json out{{"tool", "atx-vol-quote-query-load"}, {"input", o.input}, {"underlier", underlier},
           {"start_utc", start_utc}, {"end_utc", utc_now()}, {"rate_per_s", o.rate},
           {"seconds", o.seconds}, {"perturb", o.perturb}, {"deadline_us", o.deadline_us},
           {"expiries_loaded", board.size()}, {"first_version_queries", first->queries.size()},
           {"first_version_records", first->records.size()}, {"tsc_ticks_per_ns", tpn},
           {"query_path", "SliceServeRecord::serve tier Ladder..Family, four Greeks, spot +21 bp"},
           {"queries", qlog.service_ns.size()}, {"service_ns", quantiles(std::move(service))},
           {"response_ns", quantiles(std::move(response))}, {"misses", qlog.misses},
           {"miss_rate", static_cast<double>(qlog.misses) / n},
           {"deadline_misses", qlog.deadline_misses},
           {"torn", qlog.torn}, {"stale", qlog.stale}, {"first_torn", qlog.first_torn},
           {"service_ns_by_answer_tier",
            {{"ladder", tier_quantiles(qlog, 0)},
             {"family", tier_quantiles(qlog, 1)},
             {"other", tier_quantiles(qlog, 2)}}},
           {"expected_answers_first_version", answer_histogram(*first)},
           {"publish", o.publish}, {"pin_query_cpu", o.pin_query_cpu},
           {"versions_published", plog.versions.size() + 1}, {"publisher_error", plog.error},
           {"rebuild_ms", {{"fit", quantiles(plog.fit_ms)}, {"records", quantiles(plog.records_ms)},
                           {"expected", quantiles(plog.expected_ms)},
                           {"total", quantiles(plog.total_ms)}}},
           {"pass_a_queue_age_us", quantiles(plog.queue_ages_us)},
           {"consecutive_versions_distinguishable", plog.compared == 0 ? 0.0
               : static_cast<double>(plog.distinguishable) / static_cast<double>(plog.compared)},
           {"versions", plog.versions}, {"windows", window_table(qlog, o)},
           {"peak_working_set_mb", peak_working_set_mb()}};
  std::ofstream(o.report) << out.dump(2) << '\n';
  std::printf("queries=%zu torn=%llu stale=%llu misses=%llu versions=%zu\n", qlog.service_ns.size(),
              static_cast<unsigned long long>(qlog.torn),
              static_cast<unsigned long long>(qlog.stale),
              static_cast<unsigned long long>(qlog.misses), plog.versions.size() + 1);
  return qlog.torn == 0 && qlog.stale == 0 && plog.error.empty() ? 0 : 3;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(parse_args(argc, argv));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "atx-vol-quote-query-load: %s\n", e.what());
    return 1;
  }
}
