// history_store_main.cpp — atx-vol-history-store: build / inspect / bench the lane-P2
// history formats (design wave3-persist D12 / D13; spec
// docs/storage/PERSIST_HISTORY_FORMAT.md, usage docs/persist-history.md).
//
//   atx-vol-history-store timeline build   --db <SurfaceDb root> --out <store root>
//                                          [--keys k1,k2,...] [--created-unix-ns N]
//   atx-vol-history-store timeline inspect <store root>
//   atx-vol-history-store timeline bench   [--out <dir>] [--uids 100] [--days 22]
//                                          [--buckets 11] [--json <file>]
//   atx-vol-history-store riskrun  inspect <run dir>
//   atx-vol-history-store riskrun  bench   [--out <dir>] [--legs 10000] [--steps 64]
//                                          [--json <file>]
//
// Bench outputs are written under --out (default C:\atx-cache\persist\p2); a path under
// C:\atx-data is refused (that store is read-only). Every bench JSON carries
// schema_version, the host CPU busy fraction sampled over the run and the --preset
// label the caller passes (the binary cannot know its own preset). Timings are wall
// clock (steady_clock); synthetic inputs are deterministic.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/backtest/priced_surface_view.hpp"
#include "atx/vol/api/fitting/dense_slice.hpp"
#include "atx/vol/api/fitting/surface_parity.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"
#include "atx/vol/api/marketdata/spiderrock_store.hpp"
#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/risk_run_archive.hpp"
#include "atx/vol/api/persist/surface_timeline.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/strike_coord.hpp"
#include "atx/vol/api/risk/theo_contract.hpp"
#include "atx/vol/api/storage/surface_archive.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "persist/risk_run_format.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace atx::vol; // NOLINT(google-build-using-namespace): tool main
using Clock = std::chrono::steady_clock;

constexpr int kSchemaVersion = 1;
constexpr std::string_view kDefaultOut = "C:/atx-cache/persist/p2";

[[nodiscard]] double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

// p-quantile (nearest rank) of `v` (sorted in place).
[[nodiscard]] double quantile(std::vector<double> &v, double p) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v.begin(), v.end());
  const auto at = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size()))) ;
  return v[std::min(v.size() - 1, at == 0 ? 0 : at - 1)];
}

// ── Host load (Windows: system-wide CPU busy fraction between two samples) ─────

struct CpuSample {
  std::uint64_t idle{0};
  std::uint64_t total{0};
};

[[nodiscard]] CpuSample cpu_sample() {
#if defined(_WIN32)
  FILETIME idle{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetSystemTimes(&idle, &kernel, &user) == 0) return {};
  const auto u64 = [](const FILETIME &f) {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32u) | f.dwLowDateTime;
  };
  // Kernel time includes idle time.
  return CpuSample{u64(idle), u64(kernel) + u64(user)};
#else
  return {};
#endif
}

[[nodiscard]] double busy_fraction(const CpuSample &a, const CpuSample &b) {
  const std::uint64_t total = b.total - a.total;
  if (total == 0) return std::numeric_limits<double>::quiet_NaN();
  return 1.0 - static_cast<double>(b.idle - a.idle) / static_cast<double>(total);
}

// ── Arguments ─────────────────────────────────────────────────────────────────

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> named;

  [[nodiscard]] std::string get(const std::string &k, std::string_view fallback) const {
    const auto it = named.find(k);
    return it == named.end() ? std::string(fallback) : it->second;
  }
  [[nodiscard]] long long num(const std::string &k, long long fallback) const {
    const auto it = named.find(k);
    return it == named.end() ? fallback : std::stoll(it->second);
  }
};

[[nodiscard]] Args parse(int argc, char **argv, int from) {
  Args a;
  for (int i = from; i < argc; ++i) {
    const std::string_view s{argv[i]};
    if (s.size() > 2 && s.substr(0, 2) == "--" && i + 1 < argc) {
      a.named[std::string(s.substr(2))] = argv[++i];
    } else {
      a.positional.emplace_back(s);
    }
  }
  return a;
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string &s) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= s.size()) {
    const std::size_t comma = s.find(',', at);
    const std::size_t end = comma == std::string::npos ? s.size() : comma;
    if (end > at) out.push_back(s.substr(at, end - at));
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return out;
}

// Refuse any output under the read-only licensed store (lane rule, persist D11).
[[nodiscard]] bool under_data_root(const fs::path &p) {
  std::string s = fs::absolute(p).lexically_normal().generic_string();
  std::transform(s.begin(), s.end(), s.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return s.rfind("c:/atx-data", 0) == 0;
}

[[nodiscard]] int fail(const char *what, const PersistError &e) {
  std::fprintf(stderr, "error: %s: %s (section %u, detail %llu)\n", what, to_string(e.status),
               e.section, static_cast<unsigned long long>(e.detail));
  return 1;
}

void write_text(const fs::path &path, const std::string &text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}

[[nodiscard]] std::uint64_t dir_bytes(const fs::path &dir) {
  std::uint64_t n = 0;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec)) n += e.file_size(ec);
  }
  return n;
}

// ── Civil dates (UTC) ─────────────────────────────────────────────────────────

[[nodiscard]] std::int64_t days_from_civil(std::int64_t y, std::int64_t m, std::int64_t d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146'097 + doe - 719'468;
}

[[nodiscard]] std::string civil_string(std::int64_t z) {
  z += 719'468;
  const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
  const std::int64_t doe = z - era * 146'097;
  const std::int64_t yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  std::array<char, 16> buf{};
  std::snprintf(buf.data(), buf.size(), "%04lld-%02lld-%02lld", static_cast<long long>(y),
                static_cast<long long>(m), static_cast<long long>(d));
  return std::string(buf.data());
}

// ═════════════════════════════════════════════════════════════════════════════
// timeline
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] int timeline_build(const Args &a) {
  const std::string db_root = a.get("db", "");
  const std::string out = a.get("out", "");
  if (db_root.empty() || out.empty()) {
    std::fprintf(stderr, "usage: timeline build --db <SurfaceDb root> --out <store root> [--keys ...]\n");
    return 2;
  }
  if (under_data_root(out)) {
    std::fprintf(stderr, "error: refusing to write under C:\\atx-data\n");
    return 2;
  }
  auto db = SurfaceDb::open(db_root);
  if (!db) {
    std::fprintf(stderr, "error: open SurfaceDb: %s\n", db.error().to_string().c_str());
    return 1;
  }
  std::vector<std::string> keys = split_csv(a.get("keys", ""));
  if (keys.empty()) {
    for (const DbPartitionInfo &p : db->partitions()) keys.push_back(p.key);
  }
  const auto t0 = Clock::now();
  auto n = SurfaceTimelineStore::build_from_surface_db(*db, keys, out, a.num("created-unix-ns", 0));
  if (!n) return fail("build", n.error());
  std::printf("timeline build: %zu partitions -> %zu entries in %.3f s, %llu bytes under %s\n",
              keys.size(), *n, seconds_since(t0), static_cast<unsigned long long>(dir_bytes(out)),
              out.c_str());
  return 0;
}

[[nodiscard]] int timeline_inspect(const Args &a) {
  if (a.positional.empty()) {
    std::fprintf(stderr, "usage: timeline inspect <store root>\n");
    return 2;
  }
  const std::string root = a.positional.front();
  auto d = PersistDirectory::open(root);
  if (!d) return fail("open manifest", d.error());
  std::printf("SurfaceTimeline store %s: %zu period file(s), schema fingerprint %016llx\n",
              root.c_str(), d->entries().size(),
              static_cast<unsigned long long>(surface_timeline_fingerprint()));
  std::printf("%-8s %-28s %12s %8s %6s %s\n", "period", "file", "bytes", "entries", "uids",
              "ts range (ns)");
  for (const PersistManifestEntry &e : d->entries()) {
    auto tl = SurfaceTimeline::open(d->path(e));
    if (!tl) return fail("open period", tl.error());
    std::size_t entries = 0;
    for (const std::uint32_t uid : tl->uids()) {
      entries += tl->range(uid, std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max())
                     .size();
    }
    std::printf("%-8s %-28s %12llu %8zu %6zu [%lld, %lld]\n", e.key.data(), e.file_name.data(),
                static_cast<unsigned long long>(e.file_size), entries, tl->uids().size(),
                static_cast<long long>(e.ts_min_ns), static_cast<long long>(e.ts_max_ns));
  }
  return 0;
}

// A synthetic fitted surface (modeled on bench/support/synth_book.hpp and
// tests/support/risk_synthetic_market.hpp): every 5th uid on ConvexDense slices (the
// heavy, lazily-materialized record kind), the rest on eSSVI; spot, level and skew
// drift deterministically with the snapshot index.
[[nodiscard]] PricedSurface synth_surface(std::uint32_t uid, std::int64_t ts_ns, int snapshot) {
  const double x = static_cast<double>(snapshot);
  const double u = static_cast<double>(uid);
  const double S = (40.0 + 7.5 * static_cast<double>(uid % 37)) * (1.0 + 0.01 * std::sin(0.37 * x + u));
  const double r = 0.043;
  const double q = 0.01 + 0.0005 * static_cast<double>(uid % 9);
  const bool heavy = uid % 5 == 0;
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < 4; ++i) {
    const double T = 0.05 + 0.12 * static_cast<double>(i);
    const double F = S * std::exp((r - q) * T);
    const double df = std::exp(-r * T);
    const double vol = 0.20 + 0.10 * std::exp(-T / 0.15) + 0.01 * std::sin(0.21 * x + u);
    const double skew = -0.3 + 0.05 * std::cos(0.13 * x + u);
    if (heavy) {
      constexpr int kNodes = 24;
      ConvexSliceFit fit;
      fit.T = T;
      fit.F = F;
      fit.df = df;
      fit.rmse_price = 0.3;
      fit.n_obs = static_cast<std::size_t>(kNodes);
      fit.n_active = 4;
      fit.u.resize(kNodes);
      fit.C.resize(kNodes);
      for (int j = 0; j < kNodes; ++j) {
        const double K = F * (0.6 + 0.9 * static_cast<double>(j) / (kNodes - 1));
        fit.u[static_cast<std::size_t>(j)] = K;
        fit.C[static_cast<std::size_t>(j)] =
            black76_price(F, K, T, vol + 0.2 * skew * std::log(K / F), df, Side::Call);
      }
      cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    } else {
      EssviParams e{};
      e.theta = vol * vol * T;
      e.phi = 1.0 / std::sqrt(e.theta);
      e.rho = skew;
      e.psi = 0.5;
      e.p = 0.5;
      e.lambda = 0.5;
      e.T = T;
      e.F = F;
      cs.push(std::make_unique<EssviCurve>(e, df));
    }
    ctx.push_back(SliceContext{T, F, 0.0, q, 200, 0});
  }
  PricingContext pc;
  pc.S = S;
  pc.r = r;
  pc.now_ts_ns = ts_ns;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return PricedSurface::create(std::move(cs), std::move(ctx), pc).value();
}

[[nodiscard]] std::string symbol_of(std::uint32_t uid) {
  std::array<char, 16> buf{};
  std::snprintf(buf.data(), buf.size(), "U%04u", uid);
  return std::string(buf.data());
}

struct SynthDb {
  std::vector<std::string> keys;
  std::vector<std::int64_t> ts;
  double write_s{0.0};
  std::uint64_t partition_bytes{0};
};

// days x buckets ATXVSA2 partitions of `uids` surfaces in a fresh SurfaceDb.
[[nodiscard]] std::optional<SynthDb> make_synth_db(const fs::path &root, std::uint32_t uids, int days,
                                                   int buckets) {
  std::error_code ec;
  fs::remove_all(root, ec);
  auto db = SurfaceDb::create(root.string());
  if (!db) return std::nullopt;
  SynthDb out;
  const std::int64_t day0 = days_from_civil(2026, 9, 21); // spans September / October
  const auto t0 = Clock::now();
  int snapshot = 0;
  for (int d = 0; d < days; ++d) {
    const std::string date = civil_string(day0 + d);
    for (int b = 0; b < buckets; ++b, ++snapshot) {
      std::array<char, 8> hhmm{};
      std::snprintf(hhmm.data(), hhmm.size(), "%02d%02d", 10 + b / 2, (b % 2) * 30);
      auto ts = spiderrock_bucket_instant_ns(date, hhmm.data());
      if (!ts) return std::nullopt;
      std::vector<PricedSurface> surfaces;
      surfaces.reserve(uids);
      std::vector<std::string> symbols;
      symbols.reserve(uids);
      std::vector<SurfaceArchiveItem> items;
      for (std::uint32_t u = 1; u <= uids; ++u) {
        surfaces.push_back(synth_surface(u, *ts, snapshot));
        symbols.push_back(symbol_of(u));
      }
      for (std::uint32_t u = 0; u < uids; ++u) {
        items.push_back(SurfaceArchiveItem{symbols[u], &surfaces[u], std::nullopt});
      }
      const std::string key = date + "T" + hhmm.data();
      if (!db->write_partition(key, items)) return std::nullopt;
      out.keys.push_back(key);
      out.ts.push_back(*ts);
    }
  }
  out.write_s = seconds_since(t0);
  out.partition_bytes = dir_bytes(root / std::string(kSurfaceDbPartitionDir));
  return out;
}

[[nodiscard]] bool same_theo(const ResolvedTheo &a, const ResolvedTheo &b) {
  const auto eq = [](double x, double y) {
    return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y);
  };
  return a.status == b.status && eq(a.leg.K, b.leg.K) && eq(a.iv, b.iv) && eq(a.mark, b.mark) &&
         eq(a.delta, b.delta) && eq(a.greeks.gamma, b.greeks.gamma) && eq(a.greeks.vega, b.greeks.vega);
}

[[nodiscard]] int timeline_bench(const Args &a) {
  const fs::path out = a.get("out", kDefaultOut);
  if (under_data_root(out)) {
    std::fprintf(stderr, "error: refusing to write under C:\\atx-data\n");
    return 2;
  }
  const auto uids = static_cast<std::uint32_t>(a.num("uids", 100));
  const auto days = static_cast<int>(a.num("days", 22));
  const auto buckets = static_cast<int>(a.num("buckets", 11));
  const CpuSample cpu0 = cpu_sample();
  const fs::path db_root = out / "timeline-bench-db";
  const fs::path store = out / "timeline-bench-store";
  auto synth = make_synth_db(db_root, uids, days, buckets);
  if (!synth) {
    std::fprintf(stderr, "error: synthetic SurfaceDb build failed\n");
    return 1;
  }
  auto db = SurfaceDb::open(db_root.string());
  if (!db) return 1;
  std::error_code ec;
  fs::remove_all(store, ec);
  const auto tb = Clock::now();
  auto built = SurfaceTimelineStore::build_from_surface_db(*db, synth->keys, store.string(),
                                                           1792000000000000000LL);
  const double build_s = seconds_since(tb);
  if (!built) return fail("build", built.error());
  const std::uint64_t store_bytes = dir_bytes(store);

  // Open the store and one period file directly (range / view are per-file calls).
  const auto to = Clock::now();
  auto st = SurfaceTimelineStore::open(store.string());
  const double open_s = seconds_since(to);
  if (!st) return fail("open store", st.error());
  auto dir = PersistDirectory::open(store.string());
  if (!dir) return fail("manifest", dir.error());
  auto tl = SurfaceTimeline::open(dir->path(dir->entries().front()));
  if (!tl) return fail("open period", tl.error());

  // range(): per-call cost, timed in batches of 64 lookups.
  std::uint64_t lcg = 0x9E3779B97F4A7C15ull;
  const auto next = [&lcg]() {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    return lcg >> 33u;
  };
  const auto all = tl->range(1, std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max());
  std::vector<double> range_ns;
  std::size_t sink = 0;
  for (int s = 0; s < 4000; ++s) {
    const auto t0 = Clock::now();
    for (int k = 0; k < 64; ++k) {
      const auto uid = static_cast<std::uint32_t>(1 + next() % uids);
      const std::int64_t lo = all[next() % all.size()].ts_ns;
      sink += tl->range(uid, lo, lo + 86'400'000'000'000LL).size();
    }
    range_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 64.0);
  }
  // view(): per call, parametric (eSSVI) and heavy (ConvexDense) records apart.
  std::vector<double> view_param_us;
  std::vector<double> view_heavy_us;
  for (int s = 0; s < 4000; ++s) {
    const auto uid = static_cast<std::uint32_t>(1 + next() % uids);
    const auto r = tl->range(uid, std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max());
    const TimelineEntry &e = r[next() % r.size()];
    const auto t0 = Clock::now();
    auto v = tl->view(e);
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    if (!v) return fail("view", v.error());
    sink += v->n_slices();
    (uid % 5 == 0 ? view_heavy_us : view_param_us).push_back(us);
  }

  // project_series over every snapshot of one uid: timeline views vs owned surfaces.
  struct ProjectRow {
    std::uint32_t uid;
    std::size_t refs;
    double timeline_s;
    double owned_s;
    bool bit_equal;
  };
  std::vector<ProjectRow> project;
  for (const std::uint32_t uid : {1u, 5u}) {
    std::vector<PricedSurfaceView> views;
    std::vector<SurfaceRef> refs;
    std::vector<std::int64_t> ts;
    auto n = st->collect(uid, std::numeric_limits<std::int64_t>::min(),
                         std::numeric_limits<std::int64_t>::max(), views, refs, ts);
    if (!n) return fail("collect", n.error());
    std::vector<PricedSurface> owned;
    owned.reserve(synth->keys.size());
    for (const std::string &key : synth->keys) {
      auto s = db->load_surface(key, symbol_of(uid));
      if (!s) return 1;
      owned.push_back(std::move(*s));
    }
    std::vector<SurfaceRef> owned_refs;
    for (const PricedSurface &s : owned) owned_refs.emplace_back(&s);
    const TheoContract spec{uid, MaturitySpec::years_of(0.25), StrikeCoord::log_moneyness(-0.05),
                            Side::Put, 100.0};
    std::vector<ResolvedTheo> rt(refs.size());
    std::vector<ResolvedTheo> ro(refs.size());
    std::vector<double> tt;
    std::vector<double> too;
    for (int rep = 0; rep < 5; ++rep) {
      auto t0 = Clock::now();
      if (!project_series(spec, refs, ts, ProjectionPolicy{}, rt)) return 1;
      tt.push_back(seconds_since(t0));
      t0 = Clock::now();
      if (!project_series(spec, owned_refs, ts, ProjectionPolicy{}, ro)) return 1;
      too.push_back(seconds_since(t0));
    }
    bool equal = true;
    for (std::size_t k = 0; k < rt.size(); ++k) equal = equal && same_theo(rt[k], ro[k]);
    project.push_back(ProjectRow{uid, refs.size(), quantile(tt, 0.5), quantile(too, 0.5), equal});
  }
  const double busy = busy_fraction(cpu0, cpu_sample());

  std::string json = "{\n  \"schema_version\": " + std::to_string(kSchemaVersion) +
                     ",\n  \"kind\": \"persist-history-timeline-synthetic\",\n";
  std::array<char, 2048> b{};
  std::snprintf(
      b.data(), b.size(),
      "  \"preset\": \"%s\",\n  \"host_cpu_busy\": %.3f,\n  \"uids\": %u, \"days\": %d, "
      "\"buckets\": %d, \"partitions\": %zu, \"entries\": %zu,\n  \"partition_bytes\": %llu, "
      "\"partition_write_s\": %.3f,\n  \"store_bytes\": %llu, \"periods\": %zu, \"build_s\": %.3f, "
      "\"build_mb_s\": %.1f,\n  \"store_open_ms\": %.3f,\n  \"range_ns_p50\": %.1f, "
      "\"range_ns_p99\": %.1f,\n  \"view_param_us_p50\": %.2f, \"view_param_us_p99\": %.2f,\n"
      "  \"view_heavy_us_p50\": %.2f, \"view_heavy_us_p99\": %.2f,\n",
      a.get("preset", "unknown").c_str(), busy, uids, days, buckets, synth->keys.size(), *built,
      static_cast<unsigned long long>(synth->partition_bytes), synth->write_s,
      static_cast<unsigned long long>(store_bytes), dir->entries().size(), build_s,
      static_cast<double>(store_bytes) / 1e6 / build_s, open_s * 1e3, quantile(range_ns, 0.5),
      quantile(range_ns, 0.99), quantile(view_param_us, 0.5), quantile(view_param_us, 0.99),
      quantile(view_heavy_us, 0.5), quantile(view_heavy_us, 0.99));
  json += b.data();
  json += "  \"project_series\": [";
  for (std::size_t i = 0; i < project.size(); ++i) {
    const ProjectRow &p = project[i];
    std::snprintf(b.data(), b.size(),
                  "%s\n    {\"uid\": %u, \"kind\": \"%s\", \"refs\": %zu, \"timeline_ms\": %.3f, "
                  "\"owned_ms\": %.3f, \"ratio\": %.3f, \"bit_equal\": %s}",
                  i == 0 ? "" : ",", p.uid, p.uid % 5 == 0 ? "convex-dense" : "essvi", p.refs,
                  p.timeline_s * 1e3, p.owned_s * 1e3, p.timeline_s / p.owned_s,
                  p.bit_equal ? "true" : "false");
    json += b.data();
  }
  json += "\n  ],\n  \"sink\": " + std::to_string(sink % 7) + "\n}\n";
  std::fputs(json.c_str(), stdout);
  write_text(a.get("json", (out / "bench-timeline-synthetic.json").string()), json);
  return 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// riskrun
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] int riskrun_inspect(const Args &a) {
  if (a.positional.empty()) {
    std::fprintf(stderr, "usage: riskrun inspect <run dir>\n");
    return 2;
  }
  auto r = risk_run::Reader::open(a.positional.front());
  if (!r) return fail("open run", r.error());
  std::printf("RiskRun %s: steps %zu, segments %zu, var results %zu, legs %zu, book fp %016llx, "
              "schema fingerprint %016llx\n",
              a.positional.front().c_str(), r->n_steps(), r->n_segments(), r->n_var(),
              r->legs().size(), static_cast<unsigned long long>(r->meta(0).book_fingerprint),
              static_cast<unsigned long long>(risk_run_fingerprint()));
  std::printf("%-4s %-10s %-10s %8s %8s %12s %-6s\n", "seg", "first", "steps", "frames", "rows",
              "bytes", "codec");
  for (std::size_t s = 0; s < r->n_segments(); ++s) {
    const risk_run::RiskRunMeta &m = r->meta(s);
    const PersistSectionEntry *price = r->segment(s).entry(
        risk_run::type_of(risk_run::Section::FrameF64) + static_cast<std::uint32_t>(RiskFrameColumn::Price));
    std::printf("%-4zu %-10llu %-10llu %8llu %8llu %12llu %-6s\n", s,
                static_cast<unsigned long long>(m.first_step), static_cast<unsigned long long>(m.n_steps),
                static_cast<unsigned long long>(m.n_frames), static_cast<unsigned long long>(m.n_explain_rows),
                static_cast<unsigned long long>(r->segment(s).header().file_size),
                price != nullptr && price->codec != 0u ? "zstd" : "none");
  }
  return 0;
}

// A synthetic 10k-leg-class book: uids x expiries x strikes x {call, put} unique
// contracts, one leg each, plus one share leg per uid.
[[nodiscard]] RiskBook synth_book(std::size_t n_legs) {
  constexpr std::uint32_t kUids = 20;
  constexpr std::int64_t kT0 = 1792000000000000000LL;
  constexpr std::int64_t kYearNs = 31'536'000'000'000'000LL;
  const std::size_t per = (n_legs + kUids * 8u * 2u - 1u) / (kUids * 8u * 2u);
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (std::uint32_t u = 1; u <= kUids; ++u) {
    for (int e = 0; e < 8; ++e) {
      const std::int64_t expiry = kT0 + (e + 1) * (kYearNs / 12);
      for (std::size_t k = 0; k < per; ++k) {
        for (const Side side : {Side::Call, Side::Put}) {
          const double K = 50.0 + static_cast<double>(u) * 5.0 + static_cast<double>(k) * 0.5;
          legs.push_back(OptionLeg{id, u, K, expiry, side, (id % 2 == 0 ? -1.0 : 1.0) * static_cast<double>(id % 7 + 1), 100.0});
          ++id;
        }
      }
    }
  }
  std::vector<ShareLeg> shares;
  for (std::uint32_t u = 1; u <= kUids; ++u) shares.push_back(ShareLeg{1'000'000 + u, u, -100.0});
  return RiskBook::create(legs, shares).value();
}

// Deterministic, engine-shaped values (prices / Greeks with fine structure, so the
// compression ratios are not trivially high).
[[nodiscard]] double wiggle(std::size_t i, std::size_t step, double scale) {
  const double x = static_cast<double>(i) * 0.618034 + static_cast<double>(step) * 0.1;
  return scale * (1.0 + 0.3 * std::sin(x) + 1e-7 * std::cos(37.0 * x));
}

void fill_frame(RiskFrame &f, const PreparedRiskBook &p, std::size_t step, std::int64_t ts) {
  f.reserve(p.n_unique(), p.uids().size(), p.book().n_share_legs());
  f.ts_ns = ts;
  f.book_logical_id = p.book_logical_id();
  for (std::size_t c = 0; c < kRiskFrameColumnCount; ++c) {
    std::vector<double> &col = f.*risk_run::kFrameMembers[c];
    for (std::size_t i = 0; i < col.size(); ++i) col[i] = wiggle(i + 977 * c, step, 0.01 * static_cast<double>(c + 1));
  }
  std::fill(f.route.begin(), f.route.end(), RiskRoute::Family);
  std::fill(f.flags.begin(), f.flags.end(), RiskFlags{0});
  std::fill(f.status.begin(), f.status.end(), RiskLaneStatus::Ok);
  for (std::size_t i = 0; i < f.uid_spot.size(); ++i) f.uid_spot[i] = wiggle(i, step, 100.0);
  std::fill(f.uid_flat_rate.begin(), f.uid_flat_rate.end(), 0.043);
  std::fill(f.uid_status.begin(), f.uid_status.end(), RiskLaneStatus::Ok);
  for (std::size_t i = 0; i < f.share_mark.size(); ++i) f.share_mark[i] = wiggle(i, step, 100.0);
  std::fill(f.share_status.begin(), f.share_status.end(), RiskLaneStatus::Ok);
}

void fill_explain(ExplainResult &r, const PreparedRiskBook &p, std::size_t step, std::int64_t base,
                  std::int64_t shifted) {
  const RiskBook &b = p.book();
  r.reserve(b.n_option_legs(), b.n_share_legs(), p.uids().size(), 8);
  r.base_ts_ns = base;
  r.shifted_ts_ns = shifted;
  r.dt_years = 1.0 / 365.0;
  for (std::size_t i = 0; i < b.n_option_legs(); ++i) {
    r.id[i] = b.option_legs()[i].id;
    r.uid[i] = b.option_legs()[i].uid;
    r.kind[i] = ExplainLegKind::Option;
  }
  for (std::size_t j = 0; j < b.n_share_legs(); ++j) {
    const std::size_t i = b.n_option_legs() + j;
    r.id[i] = b.share_legs()[j].id;
    r.uid[i] = b.share_legs()[j].uid;
    r.kind[i] = ExplainLegKind::Share;
  }
  for (std::size_t c = 0; c < kRiskExplainColumnCount; ++c) {
    std::vector<double> &col = r.*risk_run::kExplainMembers[c];
    for (std::size_t i = 0; i < col.size(); ++i) col[i] = wiggle(i + 131 * c, step, 0.5 * static_cast<double>(c + 1));
  }
  std::fill(r.route_base.begin(), r.route_base.end(), RiskRoute::Family);
  std::fill(r.route_shifted.begin(), r.route_shifted.end(), RiskRoute::Family);
  std::fill(r.status.begin(), r.status.end(), RiskLaneStatus::Ok);
  r.by_uid.clear();
  for (const std::uint32_t u : p.uids()) r.by_uid.push_back(ExplainBucket{u, 0, ExplainTotals{}});
  r.total.pnl_total = wiggle(step, 0, 1000.0);
}

struct SealSplit {
  std::uint64_t bytes{0};
  double build_ms{0.0};
  double publish_ms{0.0};
};

// Re-frame one segment's sections (uncompressed) and publish the image to a scratch
// path: what a seal costs, split into its in-memory and its durable halves.
[[nodiscard]] SealSplit seal_split(const PersistFile &f, const fs::path &scratch) {
  SealSplit s;
  std::vector<PersistSectionInput> in;
  for (const PersistSectionEntry &e : f.sections()) {
    auto bytes = f.section(e.type);
    if (!bytes) return s;
    in.push_back(PersistSectionInput{e.type, e.type_major, e.type_minor, static_cast<PersistDType>(e.dtype),
                                     e.element_stride, true, PersistCodec::None, PersistFilter::None, 3,
                                     *bytes});
  }
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = f.header().schema_fingerprint;
  const auto t0 = Clock::now();
  auto image = build_persist_file(in, info);
  s.build_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  if (!image) return s;
  s.bytes = image->size();
  std::error_code ec;
  fs::remove(scratch, ec);
  const auto t1 = Clock::now();
  const bool ok = publish_persist_file(scratch.string(), *image).has_value();
  s.publish_ms = ok ? std::chrono::duration<double, std::milli>(Clock::now() - t1).count() : -1.0;
  fs::remove(scratch, ec);
  return s;
}

struct RunMeasure {
  double first_append_ms{0.0};
  std::vector<double> append_ms;
  std::vector<double> seal_ms;
  double finish_ms{0.0};
  std::size_t segments{0};
  std::uint64_t bytes{0};
};

[[nodiscard]] std::optional<RunMeasure> write_synth_run(const fs::path &dir, const PreparedRiskBook &p,
                                                         std::size_t steps) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  auto w = RiskRunWriter::create(dir.string(), p, RiskRunOptions{});
  if (!w) return std::nullopt;
  RunMeasure m;
  RiskFrame frame;
  ExplainResult ex;
  constexpr std::int64_t kT0 = 1792000000000000000LL;
  constexpr std::int64_t kStep = 1'800'000'000'000LL; // 30 min
  std::size_t listed = 0;
  for (std::size_t k = 0; k < steps; ++k) {
    const std::int64_t ts = kT0 + static_cast<std::int64_t>(k) * kStep;
    fill_frame(frame, p, k, ts);
    if (k > 0) fill_explain(ex, p, k, ts - kStep, ts);
    const auto t0 = Clock::now();
    if (!w->append_frame(frame)) return std::nullopt;
    if (k > 0 && !w->append_explain(ex)) return std::nullopt;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    auto d = PersistDirectory::open(dir.string());
    const std::size_t now = d ? d->entries().size() : 0u;
    if (k == 0) {
      m.first_append_ms = ms; // the one-off staging reservation (pages pre-faulted)
    } else {
      (now > listed ? m.seal_ms : m.append_ms).push_back(ms);
    }
    listed = now;
  }
  const auto tf = Clock::now();
  if (!w->finish()) return std::nullopt;
  m.finish_ms = std::chrono::duration<double, std::milli>(Clock::now() - tf).count();
  auto d = PersistDirectory::open(dir.string());
  m.segments = d ? d->entries().size() : 0u;
  m.bytes = dir_bytes(dir);
  return m;
}

// Total size of `segment` re-framed with every section through zstd at `level`, with
// or without the byte shuffle (informational: the ± shuffle question of persist D9).
[[nodiscard]] std::uint64_t reframed_size(const PersistFile &f, int level, PersistFilter filter) {
  std::vector<PersistSectionInput> in;
  for (const PersistSectionEntry &e : f.sections()) {
    auto bytes = f.section(e.type);
    if (!bytes) return 0;
    in.push_back(PersistSectionInput{e.type, e.type_major, e.type_minor, static_cast<PersistDType>(e.dtype),
                                     e.element_stride, true, PersistCodec::Zstd, filter, level, *bytes});
  }
  PersistFileInfo info;
  info.kind = PersistKind::RiskRun;
  info.schema_fingerprint = f.header().schema_fingerprint;
  auto image = build_persist_file(in, info, 4);
  return image ? image->size() : 0u;
}

struct CompactMeasure {
  std::string label;
  std::uint64_t bytes{0};
  double compact_s{0.0};
  double reopen_ms{0.0};
};

[[nodiscard]] std::optional<CompactMeasure> compact_copy(const fs::path &src, const fs::path &dst,
                                                         PersistCodec codec, int level, std::string label) {
  std::error_code ec;
  fs::remove_all(dst, ec);
  fs::copy(src, dst, fs::copy_options::recursive, ec);
  if (ec) return std::nullopt;
  const auto t0 = Clock::now();
  auto n = compact_risk_run(dst.string(), codec, level);
  const double s = seconds_since(t0);
  if (!n) return std::nullopt;
  auto d = PersistDirectory::open(dst.string());
  if (!d) return std::nullopt;
  const auto t1 = Clock::now();
  auto a = RiskRunArchive::open(dst.string());
  const double reopen = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
  if (!a) return std::nullopt;
  return CompactMeasure{std::move(label), d->entries().front().file_size, s, reopen};
}

// The 11-state RiskStream over a synthetic SpiderRock day, recorded: recording
// overhead per step and the bit-equality of the reopened totals.
struct StreamMeasure {
  std::size_t legs{0};
  std::size_t states{0};
  double step_ms_p50{0.0};
  double record_ms_p50{0.0};
  bool totals_bit_equal{false};
  std::uint64_t run_bytes{0};
  std::vector<CompactMeasure> compact;
};

[[nodiscard]] std::optional<StreamMeasure> stream_bench(const fs::path &out) {
  const fs::path root = out / "riskrun-stream-store";
  std::error_code ec;
  fs::remove_all(root, ec);
  SyntheticSpiderRockSpec spec;
  spec.n_underliers = 20;
  spec.n_expiries = 4;
  spec.n_strikes = 10;
  spec.seed = 3;
  if (!write_synthetic_spiderrock_store(spec, root.string())) return std::nullopt;
  SpiderRockStoreSpec rspec;
  rspec.store_root = root.string();
  rspec.date = spec.date;
  UnderlierTable table;
  struct Owned {
    MarketStateStorage storage;
    MarketState state;
  };
  std::vector<std::unique_ptr<Owned>> states;
  for (const std::string &bucket : spec.buckets_et) {
    auto b = load_spiderrock_bucket(rspec, bucket, table);
    if (!b) return std::nullopt;
    auto st = market_state_from_bucket(*b, TimeSpec{});
    if (!st) return std::nullopt;
    auto o = std::make_unique<Owned>();
    o->storage = std::move(*st);
    auto ms = make_market_state(o->storage);
    if (!ms) return std::nullopt;
    o->state = std::move(*ms);
    states.push_back(std::move(o));
  }
  std::vector<OptionLeg> legs;
  std::vector<ShareLeg> shares;
  std::uint64_t id = 1;
  for (const MarketStateStorage::MarksColumns &m : states.front()->storage.marks) {
    for (std::size_t i = 0; i < m.strike.size(); ++i) {
      legs.push_back(OptionLeg{id, m.uid, m.strike[i], m.expiry_ns[i], m.side[i],
                               static_cast<double>(id % 5 + 1) * (id % 2 == 0 ? -1.0 : 1.0), 100.0});
      ++id;
    }
    shares.push_back(ShareLeg{1'000'000 + m.uid, m.uid, -50.0});
  }
  const RiskBook book = RiskBook::create(legs, shares).value();
  StreamMeasure sm;
  sm.legs = legs.size() + shares.size();
  sm.states = states.size();
  const fs::path dir = out / "riskrun-stream-run";
  fs::remove_all(dir, ec);
  RiskStream stream(book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  auto w = RiskRunWriter::create(dir.string(), stream.prepared(), RiskRunOptions{});
  if (!w || !stream.start(states[0]->state) || !w->append_frame(stream.current())) return std::nullopt;
  std::vector<double> step_ms;
  std::vector<double> rec_ms;
  std::vector<ExplainTotals> live;
  for (std::size_t k = 1; k < states.size(); ++k) {
    auto t0 = Clock::now();
    auto r = stream.step(states[k]->state);
    step_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    if (!r) return std::nullopt;
    t0 = Clock::now();
    if (!w->append_frame(stream.current()) || !w->append_explain(**r)) return std::nullopt;
    rec_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    live.push_back((*r)->total);
  }
  if (!w->finish()) return std::nullopt;
  auto a = RiskRunArchive::open(dir.string());
  if (!a) return std::nullopt;
  sm.totals_bit_equal = a->n_steps() == states.size();
  for (std::size_t k = 1; k < states.size(); ++k) {
    const ExplainTotals *t = a->totals(k);
    sm.totals_bit_equal = sm.totals_bit_equal && t != nullptr &&
                          std::memcmp(t, &live[k - 1], sizeof(ExplainTotals)) == 0;
  }
  sm.step_ms_p50 = quantile(step_ms, 0.5);
  sm.record_ms_p50 = quantile(rec_ms, 0.5);
  sm.run_bytes = dir_bytes(dir);
  for (const auto &[codec, level, label] :
       {std::tuple{PersistCodec::None, 3, "none"}, std::tuple{PersistCodec::Zstd, 1, "zstd-1+shuffle"},
        std::tuple{PersistCodec::Zstd, 3, "zstd-3+shuffle"}}) {
    auto c = compact_copy(dir, out / ("riskrun-stream-" + std::string(label)), codec, level, label);
    if (!c) return std::nullopt;
    sm.compact.push_back(*c);
  }
  return sm;
}

[[nodiscard]] int riskrun_bench(const Args &a) {
  const fs::path out = a.get("out", kDefaultOut);
  if (under_data_root(out)) {
    std::fprintf(stderr, "error: refusing to write under C:\\atx-data\n");
    return 2;
  }
  std::error_code ec;
  fs::create_directories(out, ec);
  const auto n_legs = static_cast<std::size_t>(a.num("legs", 10000));
  const auto steps = static_cast<std::size_t>(a.num("steps", 64));
  const CpuSample cpu0 = cpu_sample();
  const RiskBook book = synth_book(n_legs);
  auto prepared = PreparedRiskBook::create(book);
  if (!prepared) return 1;
  const fs::path dir = out / "riskrun-bench-run";
  auto run = write_synth_run(dir, *prepared, steps);
  if (!run) {
    std::fprintf(stderr, "error: synthetic run failed\n");
    return 1;
  }
  // Reopen (warm) and column-span cost.
  std::vector<double> reopen_ms;
  for (int i = 0; i < 7; ++i) {
    const auto t0 = Clock::now();
    auto r = RiskRunArchive::open(dir.string());
    reopen_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    if (!r) return fail("reopen", r.error());
  }
  auto archive = RiskRunArchive::open(dir.string());
  if (!archive) return fail("open", archive.error());
  auto t_first = Clock::now();
  std::size_t sink = archive->frame(RiskFrameColumn::Price, 1).size();
  const double first_touch_us = std::chrono::duration<double, std::micro>(Clock::now() - t_first).count();
  std::vector<double> span_ns;
  for (int s = 0; s < 2000; ++s) {
    const auto t0 = Clock::now();
    for (int k = 0; k < 16; ++k) {
      sink += archive->frame(RiskFrameColumn::Price, static_cast<std::size_t>(s + k) % steps).size();
      sink += archive->explain(RiskExplainColumn::PnlTotal, 1 + static_cast<std::size_t>(s + k) % (steps - 1)).size();
    }
    span_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 32.0);
  }
  std::vector<CompactMeasure> compact;
  for (const auto &[codec, level, label] :
       {std::tuple{PersistCodec::None, 3, "none"}, std::tuple{PersistCodec::Zstd, 1, "zstd-1+shuffle"},
        std::tuple{PersistCodec::Zstd, 3, "zstd-3+shuffle"}}) {
    auto c = compact_copy(dir, out / ("riskrun-bench-" + std::string(label)), codec, level, label);
    if (!c) {
      std::fprintf(stderr, "error: compaction %s failed\n", label);
      return 1;
    }
    compact.push_back(*c);
  }
  // ± shuffle on the merged uncompressed segment, and the seal attribution: framing a
  // 64 MiB-class image in memory (build_persist_file: copy + section / file CRCs) vs
  // publishing it (write + FlushFileBuffers + rename), on one of the run's own segments.
  std::array<std::uint64_t, 4> shuffle_bytes{};
  SealSplit split;
  {
    auto r = risk_run::Reader::open((out / "riskrun-bench-none").string(),
                                    PersistOpenOptions{false, PersistVerify::Full, false, 0});
    if (!r) return fail("reader", r.error());
    shuffle_bytes[0] = reframed_size(r->segment(0), 1, PersistFilter::None);
    shuffle_bytes[1] = reframed_size(r->segment(0), 1, PersistFilter::ByteShuffle);
    shuffle_bytes[2] = reframed_size(r->segment(0), 3, PersistFilter::None);
    shuffle_bytes[3] = reframed_size(r->segment(0), 3, PersistFilter::ByteShuffle);
    auto first = risk_run::Reader::open(dir.string(), PersistOpenOptions{false, PersistVerify::Full, false, 0});
    if (!first) return fail("reader", first.error());
    split = seal_split(first->segment(0), out / "seal-split.atxrr");
  }
  auto stream = stream_bench(out);
  if (!stream) {
    std::fprintf(stderr, "error: stream bench failed\n");
    return 1;
  }
  const double busy = busy_fraction(cpu0, cpu_sample());
  const double plain = static_cast<double>(compact.front().bytes);

  std::string json = "{\n  \"schema_version\": " + std::to_string(kSchemaVersion) +
                     ",\n  \"kind\": \"persist-history-riskrun-synthetic\",\n";
  std::array<char, 4096> b{};
  std::snprintf(
      b.data(), b.size(),
      "  \"preset\": \"%s\",\n  \"host_cpu_busy\": %.3f,\n  \"legs\": %zu, \"unique\": %zu, "
      "\"steps\": %zu,\n  \"append_ms_p50\": %.3f, \"append_ms_p99\": %.3f,\n  \"seal_ms\": [",
      a.get("preset", "unknown").c_str(), busy, book.n_option_legs() + book.n_share_legs(),
      prepared->n_unique(), steps, quantile(run->append_ms, 0.5), quantile(run->append_ms, 0.99));
  json += b.data();
  for (std::size_t i = 0; i < run->seal_ms.size(); ++i) {
    std::snprintf(b.data(), b.size(), "%s%.2f", i == 0 ? "" : ", ", run->seal_ms[i]);
    json += b.data();
  }
  std::snprintf(
      b.data(), b.size(),
      "], \"finish_ms\": %.2f, \"first_append_ms\": %.2f,\n  \"seal_split\": {\"segment_bytes\": %llu, "
      "\"build_ms\": %.2f, \"publish_fsync_ms\": %.2f},\n"
      "  \"segments\": %zu, \"run_bytes\": %llu, \"bytes_per_step\": %.0f,\n"
      "  \"reopen_ms_p50\": %.3f, \"column_first_touch_us\": %.1f, \"column_span_ns_p50\": %.1f,\n"
      "  \"compaction\": [",
      run->finish_ms, run->first_append_ms, static_cast<unsigned long long>(split.bytes),
      split.build_ms, split.publish_ms, run->segments, static_cast<unsigned long long>(run->bytes),
      static_cast<double>(run->bytes) / static_cast<double>(steps), quantile(reopen_ms, 0.5),
      first_touch_us, quantile(span_ns, 0.5));
  json += b.data();
  for (std::size_t i = 0; i < compact.size(); ++i) {
    const CompactMeasure &c = compact[i];
    std::snprintf(b.data(), b.size(),
                  "%s\n    {\"codec\": \"%s\", \"bytes\": %llu, \"ratio\": %.3f, \"compact_s\": %.3f, "
                  "\"reopen_ms\": %.2f}",
                  i == 0 ? "" : ",", c.label.c_str(), static_cast<unsigned long long>(c.bytes),
                  plain / static_cast<double>(c.bytes), c.compact_s, c.reopen_ms);
    json += b.data();
  }
  std::snprintf(b.data(), b.size(),
                "\n  ],\n  \"zstd_shuffle_ratio\": {\"zstd1\": %.3f, \"zstd1_shuffle\": %.3f, "
                "\"zstd3\": %.3f, \"zstd3_shuffle\": %.3f},\n",
                plain / static_cast<double>(shuffle_bytes[0]), plain / static_cast<double>(shuffle_bytes[1]),
                plain / static_cast<double>(shuffle_bytes[2]), plain / static_cast<double>(shuffle_bytes[3]));
  json += b.data();
  const double splain = static_cast<double>(stream->compact.front().bytes);
  std::snprintf(b.data(), b.size(),
                "  \"stream\": {\"states\": %zu, \"legs\": %zu, \"step_ms_p50\": %.3f, "
                "\"record_ms_p50\": %.3f, \"totals_bit_equal\": %s, \"run_bytes\": %llu, "
                "\"zstd1_ratio\": %.3f, \"zstd3_ratio\": %.3f},\n",
                stream->states, stream->legs, stream->step_ms_p50, stream->record_ms_p50,
                stream->totals_bit_equal ? "true" : "false",
                static_cast<unsigned long long>(stream->run_bytes),
                splain / static_cast<double>(stream->compact[1].bytes),
                splain / static_cast<double>(stream->compact[2].bytes));
  json += b.data();
  json += "  \"sink\": " + std::to_string(sink % 7) + "\n}\n";
  std::fputs(json.c_str(), stdout);
  write_text(a.get("json", (out / "bench-riskrun-synthetic.json").string()), json);
  return 0;
}

[[nodiscard]] int usage() {
  std::fprintf(stderr,
               "atx-vol-history-store timeline build|inspect|bench ... | riskrun inspect|bench ...\n"
               "  (see the header of tools/history_store_main.cpp)\n");
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) return usage();
  const std::string_view family{argv[1]};
  const std::string_view verb{argv[2]};
  const Args args = parse(argc, argv, 3);
  if (family == "timeline") {
    if (verb == "build") return timeline_build(args);
    if (verb == "inspect") return timeline_inspect(args);
    if (verb == "bench") return timeline_bench(args);
  } else if (family == "riskrun") {
    if (verb == "inspect") return riskrun_inspect(args);
    if (verb == "bench") return riskrun_bench(args);
  }
  return usage();
}
