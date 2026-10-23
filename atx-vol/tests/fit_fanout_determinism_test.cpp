// Lane P0 (r6): the graded surface fit is a pure function of its inputs - byte-identical
// for any fan-out width, any task-to-thread schedule and any core type.
//
// The defect this file pins (lane C2 report D3, ledger 2026-10-23 L692): keys-ON repair
// arms moved rows on 2 SPY chains by <= 5.1e-13 at random, "only at fan-out width >= 2".
// Root cause: Eigen 3.4 sizes its blocked GEMM / TRSM kernels from the L1 / L2 / L3 cache
// sizes it reads with CPUID ONCE per process (the first blocked product), and a hybrid
// CPU answers per core (i7-1260P: P core 48 KiB / 1.25 MiB, E core 32 KiB / 2 MiB). The
// dense convex slice's normal matrix H = 2 B'WB has depth = distinct strikes, so above
// the E core's kc (248) the summation order - and the served bits - followed whichever
// core type ran the process's first blocked product. The build now disables the query
// (EIGEN_NO_CPUID, root CMakeLists.txt) and pins the three sizes, so every core blocks
// the same way.
//
// The same suite pins the scheduler's contract (src/fitting/expiry_pool.hpp): the
// per-expiry stages run on the one persistent pool (a bounded set of threads however
// many fits run) and a pricing-pool dispatch inside a fit task runs inline.
//
// FitFanoutDeterminismChild.* is the child half of the cross-process test below; it
// skips unless the parent set kPinProbeEnv.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <Eigen/Core>
#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_fit_policy.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "fitting/expiry_schedule.hpp" // ScopedExpiryTaskHook (per-task state pollution)

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h> // _spawnl, _P_WAIT
#include <windows.h> // affinity, logical processor information
#endif

namespace atx::vol {
namespace {

namespace fs = std::filesystem;

// The pinned Eigen blocking sizes (root CMakeLists.txt, target `eigen`): the i7-1260P's
// P-core answers, so every hash recorded on this host's P cores is unchanged.
constexpr std::ptrdiff_t kEigenL1 = 49152;
constexpr std::ptrdiff_t kEigenL2 = 1310720;
constexpr std::ptrdiff_t kEigenL3 = 18874368;

// Set by the parent to "<out file>|<affinity mask, decimal>"; the child pins itself to
// the mask, fits the dense fixture and writes its served-row hash to the file.
constexpr const char *kPinProbeEnv = "ATX_VOL_P0_PIN_PROBE";

// ── Fixtures ─────────────────────────────────────────────────────────────────

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// American quotes on sigma(k) = atm + skew * k (k = ln K / S), priced with the engine the
// fitter inverts; the half-spread is relative with a one-cent floor.
Chain smile_chain(double spot, double years, double atm, double skew, double half_spread,
                  double lo, double hi, double step) {
  Chain out;
  out.market = QuoteSliceMarket{spot, years, years, 0.03, 0.01, 0.0};
  const auto count = static_cast<int>(std::lround((hi - lo) / step));
  for (int s = 0; s <= count; ++s) {
    const double strike = lo + step * static_cast<double>(s);
    const double sigma = atm + skew * std::log(strike / spot);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(spot, strike, years, sigma, 0.03, 0.01, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double spread = std::max(0.01, *price * half_spread);
      out.quotes.push_back({strike, side, std::max(*price - spread, 0.0), *price + spread});
    }
  }
  return out;
}

// The shipped dense profile (ConvexDense, node cap 48) under the C2 keys-ON repair arm:
// box certificate, market-distance repair at one spread, bounded channel repair.
QuoteSliceConfig dense_repair_config() {
  QuoteSliceConfig config;
  for (const QuoteFitProfile &profile : default_quote_fit_profiles()) {
    if (profile.name == "dense") config = profile.config;
  }
  config.channel.repair_half_spreads = 0.5;
  config.repair.certificate = QuoteSliceCertificate::Box;
  config.repair.market_distance = QuoteSliceMarketDistance::Repair;
  config.repair.market_distance_spreads = 1.0;
  return config;
}

// 601 distinct strikes: the dense normal matrix's depth (601) is past both cores' Eigen
// max kc (E 248, P 376), and the two split it into different k blocks (E 208, P 304). The
// B-spline normal matrix is banded, so only basis pairs whose support straddles a block
// edge can change bits: 301 strikes (one E-only edge) happened to round alike, 601 do not.
Chain wide_dense_chain() {
  return smile_chain(1000.0, 0.75, 0.24, -0.12, 0.004, 700.0, 1300.0, 1.0);
}

// A small surface that reaches every fanned-out stage and the serial passes behind them:
// four dense-ish expiries, the third quoted at a vol that crosses its neighbours.
std::vector<QuoteSurfaceExpiry> surface_expiries(std::vector<Chain> &storage) {
  storage = {smile_chain(100.0, 0.25, 0.22, -0.10, 0.01, 80.0, 120.0, 1.0),
             smile_chain(100.0, 0.50, 0.24, -0.08, 0.01, 75.0, 125.0, 1.0),
             smile_chain(100.0, 0.75, 0.15, -0.05, 0.01, 75.0, 125.0, 2.5),
             smile_chain(100.0, 1.25, 0.27, -0.05, 0.01, 70.0, 130.0, 1.0)};
  std::vector<QuoteSurfaceExpiry> out;
  for (const Chain &c : storage) out.push_back({c.market, c.quotes, {}});
  return out;
}

// ── Hashes ───────────────────────────────────────────────────────────────────

struct Fnv {
  std::uint64_t h = 0xcbf29ce484222325ull;
  void word(std::uint64_t v) noexcept { h = (h ^ v) * 0x100000001b3ull; }
  void real(double v) noexcept { word(std::bit_cast<std::uint64_t>(v)); }
};

// Every served field of one fit, doubles by bit pattern.
void hash_fit(Fnv &f, const QuoteSliceFit &fit) {
  f.word(static_cast<std::uint64_t>(fit.fit_status));
  if (fit.prices) {
    for (const AmericanPriceNode &node : fit.prices->nodes()) {
      f.real(node.strike);
      f.real(node.call);
      f.real(node.put);
    }
  }
  for (const QuoteSliceRow &row : fit.rows) {
    f.word(static_cast<std::uint64_t>(row.evaluation_status));
    f.real(row.fitted_price);
    f.real(row.fitted_iv);
  }
}

[[nodiscard]] std::uint64_t hash_surface(const std::vector<QuoteSurfaceSlice> &slices) {
  Fnv f;
  for (const QuoteSurfaceSlice &slice : slices) {
    f.word(static_cast<std::uint64_t>(slice.grade));
    hash_fit(f, slice.fit);
  }
  return f.h;
}

// ── Windows core discovery and pinning ───────────────────────────────────────

#ifdef _WIN32
// Logical-CPU masks (group 0) of the highest-EfficiencyClass cores (P) and of every
// lower class (E). E is empty on a homogeneous host.
struct CoreMasks {
  std::uint64_t p = 0;
  std::uint64_t e = 0;
};

[[nodiscard]] CoreMasks core_masks() {
  CoreMasks out;
  DWORD len = 0;
  ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0) return out;
  std::vector<std::uint8_t> buf(len);
  auto *const first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
  if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, first, &len)) return out;
  BYTE top = 0;
  for (DWORD off = 0; off < len;) {
    const auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    top = std::max(top, info->Processor.EfficiencyClass);
    off += info->Size;
  }
  for (DWORD off = 0; off < len;) {
    const auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    if (info->Processor.GroupCount >= 1 && info->Processor.GroupMask[0].Group == 0) {
      const auto mask = static_cast<std::uint64_t>(info->Processor.GroupMask[0].Mask);
      (info->Processor.EfficiencyClass == top ? out.p : out.e) |= mask;
    }
    off += info->Size;
  }
  return out;
}

// Pins the calling thread to `cpu` and waits until it runs there; false if it cannot.
[[nodiscard]] bool run_on(unsigned cpu) {
  if (::SetThreadAffinityMask(::GetCurrentThread(), DWORD_PTR{1} << cpu) == 0) return false;
  for (int spin = 0; spin < 64 && ::GetCurrentProcessorNumber() != cpu; ++spin) {
    ::SwitchToThread();
  }
  return ::GetCurrentProcessorNumber() == cpu;
}

[[nodiscard]] std::string env(const char *name) {
  char buf[4096];
  const DWORD n = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof buf));
  return n == 0 || n >= sizeof buf ? std::string{} : std::string(buf, n);
}

[[nodiscard]] fs::path this_image() {
  char buf[4096];
  const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
  return fs::path(std::string(buf, n));
}

// What a pinned child reports: its served-row hash and the L1 size its Eigen blocked with.
struct ChildAnswer {
  std::string hash; // "" when the child wrote none
  std::string l1;
  int code = -1;
};

// Runs this test binary's child test pinned to `mask` and reads its answer.
[[nodiscard]] ChildAnswer run_pinned_child(std::uint64_t mask, const fs::path &out) {
  std::error_code ec;
  fs::remove(out, ec);
  const std::string probe = out.string() + "|" + std::to_string(mask);
  ::_putenv_s(kPinProbeEnv, probe.c_str());
  const std::string exe = this_image().string();
  const std::string argv0 = "\"" + exe + "\"";
  ChildAnswer answer;
  answer.code = static_cast<int>(::_spawnl(_P_WAIT, exe.c_str(), argv0.c_str(),
                                           "--gtest_filter=FitFanoutDeterminismChild.*",
                                           "--gtest_brief=1", nullptr));
  ::_putenv_s(kPinProbeEnv, "");
  std::ifstream in(out, std::ios::binary);
  std::getline(in, answer.hash);
  std::getline(in, answer.l1);
  return answer;
}
#endif // _WIN32

// ── Tests ────────────────────────────────────────────────────────────────────

// Root cause, at its source: Eigen's cache-size query must answer the same on every
// logical CPU of the host (the build disables it, so every core answers "unknown" and the
// pinned build constants decide). On the root build a hybrid host fails here: P cores
// answer 48 KiB, E cores 32 KiB.
TEST(FitFanoutDeterminism, EigenCacheQueryIsTheSameOnEveryCore) {
#ifdef _WIN32
  const DWORD_PTR saved = ::SetThreadAffinityMask(::GetCurrentThread(), ~DWORD_PTR{0});
  ASSERT_NE(saved, 0u);
  DWORD_PTR process = 0;
  DWORD_PTR system = 0;
  ASSERT_TRUE(::GetProcessAffinityMask(::GetCurrentProcess(), &process, &system));
  std::vector<std::array<int, 4>> answers; // cpu, l1, l2, l3
  for (unsigned cpu = 0; cpu < 64; ++cpu) {
    if (((process >> cpu) & 1u) == 0u || !run_on(cpu)) continue;
    int l1 = 0;
    int l2 = 0;
    int l3 = 0;
    Eigen::internal::queryCacheSizes(l1, l2, l3);
    answers.push_back({static_cast<int>(cpu), l1, l2, l3});
  }
  ::SetThreadAffinityMask(::GetCurrentThread(), saved);
  ASSERT_GE(answers.size(), 1u);
  for (const auto &a : answers) {
    EXPECT_EQ(a[1], answers.front()[1]) << "cpu " << a[0] << " L1 vs cpu " << answers.front()[0];
    EXPECT_EQ(a[2], answers.front()[2]) << "cpu " << a[0] << " L2 vs cpu " << answers.front()[0];
    EXPECT_EQ(a[3], answers.front()[3]) << "cpu " << a[0] << " L3 vs cpu " << answers.front()[0];
  }
#else
  GTEST_SKIP() << "per-core pinning is Windows-only here";
#endif
}

// The blocking every product uses is the build's pinned triple, whatever core first
// asked (the documented constants of docs/all-core-scheduling.md).
TEST(FitFanoutDeterminism, EigenBlockingUsesThePinnedCacheSizes) {
  EXPECT_EQ(Eigen::l1CacheSize(), kEigenL1);
  EXPECT_EQ(Eigen::l2CacheSize(), kEigenL2);
  EXPECT_EQ(Eigen::l3CacheSize(), kEigenL3);
}

// Every task's thread state is polluted before its body runs: a retained American IV
// solve (the thread's ALO slot), a blocked Eigen product and a recycled NaN-filled heap
// block. A fit that read any of it would differ from the clean serial reference.
void pollute_thread_state(std::size_t expiry) {
  const auto iv = american_implied_vol(7.25 + static_cast<double>(expiry), 103.0, 97.0, 0.9,
                                       0.04, 0.0, Side::Put);
  static_cast<void>(iv);
  const Eigen::MatrixXd a = Eigen::MatrixXd::Constant(40, 300, 0.5 + static_cast<double>(expiry));
  const Eigen::MatrixXd ata = a * a.transpose();
  static_cast<void>(ata.sum());
  std::vector<double> poison(8192, std::numeric_limits<double>::quiet_NaN());
  static_cast<void>(poison.data());
}

// The brief's ctest: the same surface (box certificate + market-distance repair, every
// fanned-out stage and the serial calendar / market passes) at fit_workers 1 / 2 / 4 and
// AUTO, with and without per-task state pollution, serves byte-identical rows.
TEST(FitFanoutDeterminism, SurfaceRowsAreByteIdenticalAcrossWidthsAndSchedules) {
  std::vector<Chain> storage;
  const std::vector<QuoteSurfaceExpiry> chains = surface_expiries(storage);
  const QuoteSliceConfig config = dense_repair_config();
  QuoteSurfaceConfig serial;
  serial.fit_workers = 1;
  serial.calendar_repair_half_spreads = 0.5;
  const auto reference = fit_quote_surface(chains, config, serial);
  ASSERT_TRUE(reference) << reference.error().to_string();
  const std::uint64_t want = hash_surface(*reference);
  std::size_t served = 0;
  for (const QuoteSurfaceSlice &slice : *reference) {
    if (slice.grade != QuoteSliceGrade::Refused) ++served;
  }
  ASSERT_GE(served, 3u) << "the fixture must serve most expiries to mean anything";
  for (const bool polluted : {false, true}) {
    for (const unsigned workers : {1u, 2u, 4u, 0u}) {
      QuoteSurfaceConfig surface = serial;
      surface.fit_workers = workers;
      const auto result = [&] {
        if (!polluted) return fit_quote_surface(chains, config, surface);
        const detail::ScopedExpiryTaskHook hook(&pollute_thread_state);
        return fit_quote_surface(chains, config, surface);
      }();
      ASSERT_TRUE(result) << result.error().to_string();
      EXPECT_EQ(hash_surface(*result), want) << "workers " << workers << " polluted " << polluted;
    }
  }
}

// ── Bounded pool and the nested-dispatch rule ───────────────────────────────

std::atomic<unsigned> g_task_threads{0}; // distinct OS threads that ever ran a fit task
std::atomic<unsigned> g_foreign_blocks{0}; // pricing blocks run off their fit task's thread

// Counts each OS thread the first time it runs a fit task. A thread_local sentinel, not
// a thread id: Windows recycles ids, a new thread always gets fresh thread-local state.
void count_task_thread(std::size_t) {
  thread_local const bool counted = (g_task_threads.fetch_add(1u), true);
  static_cast<void>(counted);
}

// Issues a plain pricing-pool dispatch from inside the fit task and counts the blocks
// that ran on another thread: a fit task must never fan the pricing pool out again.
void dispatch_pricing_from_task(std::size_t) {
  const std::thread::id self = std::this_thread::get_id();
  pricing_executor().run_blocks(64u, 0u, [self](std::size_t) {
    if (std::this_thread::get_id() != self) g_foreign_blocks.fetch_add(1u);
  });
}

// Deliverable 1: the per-expiry stages run on the one persistent pool, so however many
// fits run, their tasks stay on at most pool size + 1 threads (the workers and the
// caller). The fresh-jthread-per-stage fan-out started up to fit_workers new threads per
// stage per fit, so enough fits always exceed that bound.
TEST(FitFanoutDeterminism, FitsRunOnABoundedSetOfThreads) {
  std::vector<Chain> storage;
  const std::vector<QuoteSurfaceExpiry> chains = surface_expiries(storage);
  const QuoteSliceConfig config = dense_repair_config();
  QuoteSurfaceConfig surface;
  surface.fit_workers = 4;
  const unsigned bound = pricing_executor().size() + 1u;
  const unsigned fits = bound / 4u + 2u; // fresh threads would add >= 4 per fit
  g_task_threads.store(0u);
  {
    const detail::ScopedExpiryTaskHook hook(&count_task_thread);
    for (unsigned fit = 0; fit < fits; ++fit) {
      ASSERT_TRUE(fit_quote_surface(chains, config, surface));
    }
  }
  EXPECT_LE(g_task_threads.load(), bound) << fits << " fits started threads beyond the pool";
}

// Deliverable 3, the nested-dispatch rule: fit tasks are pool work, so a pricing-pool
// dispatch issued inside one runs inline on the task's thread - live fitting never
// nests the pool and never oversubscribes it.
TEST(FitFanoutDeterminism, PricingDispatchInsideAFitTaskRunsInline) {
  std::vector<Chain> storage;
  const std::vector<QuoteSurfaceExpiry> chains = surface_expiries(storage);
  QuoteSurfaceConfig surface;
  surface.fit_workers = 4;
  g_foreign_blocks.store(0u);
  {
    const detail::ScopedExpiryTaskHook hook(&dispatch_pricing_from_task);
    ASSERT_TRUE(fit_quote_surface(chains, dense_repair_config(), surface));
  }
  EXPECT_EQ(g_foreign_blocks.load(), 0u) << "a fit task fanned the pricing pool out";
}

// End to end across processes: the dense slice fitted by a process pinned to the P cores
// and by one pinned to the E cores serves the same bits. On the root build this fails on
// a hybrid host (the E-core process blocks H = 2 B'WB at kc 248 instead of 376).
TEST(FitFanoutDeterminism, DenseSliceIgnoresTheCoreTypeThatRunsIt) {
#ifdef _WIN32
  const CoreMasks masks = core_masks();
  if (masks.p == 0u || masks.e == 0u) {
    GTEST_SKIP() << "homogeneous host: no second core type to compare";
  }
  const fs::path dir = fs::temp_directory_path() / "atx_vol_p0_pin_probe";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const ChildAnswer on_p = run_pinned_child(masks.p, dir / "p.txt");
  const ChildAnswer on_e = run_pinned_child(masks.e, dir / "e.txt");
  ASSERT_EQ(on_p.code, 0);
  ASSERT_EQ(on_e.code, 0);
  ASSERT_FALSE(on_p.hash.empty()) << "the P-core child wrote no hash";
  ASSERT_FALSE(on_e.hash.empty()) << "the E-core child wrote no hash";
  EXPECT_EQ(on_p.hash, on_e.hash) << "served rows depend on the core type (Eigen blocked with "
                                  << on_p.l1 << " on P, " << on_e.l1 << " on E)";
#else
  GTEST_SKIP() << "core-type pinning is Windows-only here";
#endif
}

// Child half: pins the whole process (every thread it will start) to the parent's mask
// before anything touches Eigen, fits the wide dense chain and reports its hash.
TEST(FitFanoutDeterminismChild, PinnedDenseSliceHash) {
#ifdef _WIN32
  const std::string probe = env(kPinProbeEnv);
  if (probe.empty()) GTEST_SKIP() << "driver test; runs only when the parent sets " << kPinProbeEnv;
  const std::size_t bar = probe.rfind('|');
  ASSERT_NE(bar, std::string::npos);
  const std::uint64_t mask = std::stoull(probe.substr(bar + 1));
  ASSERT_NE(::SetProcessAffinityMask(::GetCurrentProcess(), static_cast<DWORD_PTR>(mask)), 0);
  ASSERT_NE(::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(mask)), 0u);
  ::SwitchToThread();
  const Chain chain = wide_dense_chain();
  const auto fit = fit_quote_slice(chain.market, chain.quotes, dense_repair_config());
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  Fnv f;
  hash_fit(f, *fit);
  std::ofstream out(fs::path(probe.substr(0, bar)), std::ios::binary | std::ios::trunc);
  out << std::hex << f.h << "\nl1=" << std::dec << Eigen::l1CacheSize()
      << " admitted=" << fit->diagnostics.quotes_admitted
      << " strikes=" << fit->diagnostics.distinct_strikes << "\n";
#else
  GTEST_SKIP() << "Windows-only child";
#endif
}

} // namespace
} // namespace atx::vol
