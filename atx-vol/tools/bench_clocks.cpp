#include "bench_clocks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "atx/vol/api/simd/cpu.hpp"
#include "core/parallel_for.hpp"
#include "fitting/carry_memo.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <powrprof.h>
#endif

namespace atx::vol::bench {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
namespace ft = counters::fit_timing;
constexpr auto bucket(ft::Bucket b) noexcept { return static_cast<std::size_t>(b); }

[[nodiscard]] double order_statistic(const std::vector<double> &sorted, double p) noexcept {
  const double location = p * static_cast<double>(sorted.size() - 1U);
  const auto lo = static_cast<std::size_t>(location);
  const std::size_t hi = std::min(lo + 1U, sorted.size() - 1U);
  return sorted[lo] + (location - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}

[[nodiscard]] Json finite_or_null(double value) {
  return std::isfinite(value) ? Json(value) : Json(nullptr);
}

[[nodiscard]] std::uint64_t saturating_minus(std::uint64_t a, std::uint64_t b) noexcept {
  return a >= b ? a - b : 0U;
}

[[nodiscard]] std::string hex_mask(std::uint64_t mask) {
  std::ostringstream out;
  out << "0x" << std::hex << mask;
  return out.str();
}

[[nodiscard]] const char *isa_name(simd::SimdIsa isa) noexcept {
  switch (isa) {
  case simd::SimdIsa::Auto:
    return "auto";
  case simd::SimdIsa::ForceScalar:
    return "force_scalar";
  case simd::SimdIsa::ForceAvx2:
    return "force_avx2";
  }
  return "unknown";
}

[[nodiscard]] Json fit_workers_json() {
  Json env = nullptr;
#if defined(_MSC_VER)
  char *value = nullptr;
  std::size_t length = 0;
  if (::_dupenv_s(&value, &length, "ATX_VOL_FIT_WORKERS") == 0 && value != nullptr) {
    env = std::string(value);
    std::free(value);
  }
#else
  if (const char *value = std::getenv("ATX_VOL_FIT_WORKERS")) env = std::string(value);
#endif
  return {{"env_ATX_VOL_FIT_WORKERS", env}, {"resolved_auto", atx_auto_worker_count()}};
}

} // namespace

ClockSummary summarize(std::span<const double> values) {
  ClockSummary out;
  out.n = values.size();
  if (values.empty()) {
    out.min = out.median = out.p95 = out.p99 = out.max = kNaN;
    return out;
  }
  std::vector<double> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  out.min = sorted.front();
  out.max = sorted.back();
  out.median = order_statistic(sorted, 0.50);
  out.p95 = order_statistic(sorted, 0.95);
  out.p99 = order_statistic(sorted, 0.99);
  return out;
}

Json summary_json(const ClockSummary &s) {
  return {{"n", s.n}, {"min", finite_or_null(s.min)}, {"median", finite_or_null(s.median)},
          {"p95", finite_or_null(s.p95)}, {"p99", finite_or_null(s.p99)},
          {"max", finite_or_null(s.max)}};
}

void PhaseClock::start(BenchClock::time_point t) noexcept {
  if (!armed_) return;
  seconds_.fill(0.0);
  conflict_ = 0.0;
  mark_ = t;
  started_ = true;
}

double PhaseClock::take(BenchClock::time_point t) noexcept {
  const double interval = std::max(0.0, seconds_between(mark_, t));
  mark_ = t;
  return interval;
}

void PhaseClock::lap(Stage stage, BenchClock::time_point t) noexcept {
  if (!armed_ || !started_) return;
  seconds_[static_cast<std::size_t>(stage)] += take(t);
}

void PhaseClock::lap(Stage stage) noexcept {
  if (!armed_ || !started_) return;
  lap(stage, BenchClock::now());
}

void PhaseClock::lap_split(BenchClock::time_point t, std::span<const StagePart> parts,
                           Stage residual) noexcept {
  if (!armed_ || !started_) return;
  double remaining = take(t);
  for (const StagePart &part : parts) {
    const double claim = std::max(0.0, part.seconds);
    const double charged = std::min(claim, remaining);
    seconds_[static_cast<std::size_t>(part.stage)] += charged;
    remaining -= charged;
    conflict_ += claim - charged;
  }
  seconds_[static_cast<std::size_t>(residual)] += remaining;
}

void PhaseClock::add_conflict(double seconds) noexcept {
  if (armed_ && seconds > 0.0) conflict_ += seconds;
}

double PhaseClock::seconds(Stage stage) const noexcept {
  return seconds_[static_cast<std::size_t>(stage)];
}

double PhaseClock::total() const noexcept {
  double sum = 0.0;
  for (const double s : seconds_) sum += s;
  return sum;
}

void add_fit_work(FitWork &work, const ft::FitTimingCounters &c) noexcept {
  ++work.chains;
  work.wall_us += c.wall_us;
  const std::array<std::uint64_t, ft::kBucketCount> us = {
      c.other_us, c.deam_us, c.solve_us, c.project_us, c.price_us, c.certify_us, c.calendar_us};
  static_assert(ft::kBucketCount == 7, "FitWork mirrors the seven fit_timing buckets");
  for (std::size_t b = 0; b < ft::kBucketCount; ++b) {
    work.bucket_us[b] += us[b];
    work.al_by_bucket[b] += c.al_by_bucket[b];
  }
  work.al_price_calls += c.al_price_calls;
  work.al_greeks_calls += c.al_greeks_calls;
  work.iv_newton_iters += c.iv_newton_iters;
  work.certification_passes += c.certification_passes;
  work.channel_solves += c.channel_solves;
  work.jacobian_evals += c.jacobian_evals;
  work.rung_attempts += c.rung_attempts;
  work.calendar_pairs += c.calendar_pairs;
  work.projections += c.projections;
  work.repair_rung_visits += c.repair_rung_visits;
  work.deam_inversions += c.deam_inversions;
  work.deam_inversion_al += c.deam_inversion_al;
  work.deam_premium += c.deam_premium;
  work.deam_premium_fallbacks += c.deam_premium_fallbacks;
  for (std::size_t r = 0; r < work.deam_fallback_by_reason.size(); ++r) {
    work.deam_fallback_by_reason[r] += c.deam_premium_fallback_by_reason[r];
  }
  for (std::size_t r = 0; r < work.deam_al_by_reason.size(); ++r) {
    work.deam_al_by_reason[r] += c.deam_al_by_reason[r];
  }
  work.deam_confirms += c.deam_confirms;
  work.deam_confirms_skipped += c.deam_confirms_skipped;
  work.deam_ambiguous_edges += c.deam_ambiguous_edges;
  work.deam_seed_routed += c.deam_seed_routed;
}

FitWork fit_work_delta(const FitWork &a, const FitWork &b) noexcept {
  FitWork d;
  d.chains = saturating_minus(a.chains, b.chains);
  d.wall_us = saturating_minus(a.wall_us, b.wall_us);
  for (std::size_t i = 0; i < d.bucket_us.size(); ++i) {
    d.bucket_us[i] = saturating_minus(a.bucket_us[i], b.bucket_us[i]);
    d.al_by_bucket[i] = saturating_minus(a.al_by_bucket[i], b.al_by_bucket[i]);
  }
  d.al_price_calls = saturating_minus(a.al_price_calls, b.al_price_calls);
  d.al_greeks_calls = saturating_minus(a.al_greeks_calls, b.al_greeks_calls);
  d.iv_newton_iters = saturating_minus(a.iv_newton_iters, b.iv_newton_iters);
  d.certification_passes = saturating_minus(a.certification_passes, b.certification_passes);
  d.channel_solves = saturating_minus(a.channel_solves, b.channel_solves);
  d.jacobian_evals = saturating_minus(a.jacobian_evals, b.jacobian_evals);
  d.rung_attempts = saturating_minus(a.rung_attempts, b.rung_attempts);
  d.calendar_pairs = saturating_minus(a.calendar_pairs, b.calendar_pairs);
  d.projections = saturating_minus(a.projections, b.projections);
  d.repair_rung_visits = saturating_minus(a.repair_rung_visits, b.repair_rung_visits);
  d.deam_inversions = saturating_minus(a.deam_inversions, b.deam_inversions);
  d.deam_inversion_al = saturating_minus(a.deam_inversion_al, b.deam_inversion_al);
  d.deam_premium = saturating_minus(a.deam_premium, b.deam_premium);
  d.deam_premium_fallbacks = saturating_minus(a.deam_premium_fallbacks, b.deam_premium_fallbacks);
  for (std::size_t r = 0; r < d.deam_fallback_by_reason.size(); ++r) {
    d.deam_fallback_by_reason[r] =
        saturating_minus(a.deam_fallback_by_reason[r], b.deam_fallback_by_reason[r]);
  }
  for (std::size_t r = 0; r < d.deam_al_by_reason.size(); ++r) {
    d.deam_al_by_reason[r] = saturating_minus(a.deam_al_by_reason[r], b.deam_al_by_reason[r]);
  }
  d.deam_confirms = saturating_minus(a.deam_confirms, b.deam_confirms);
  d.deam_confirms_skipped = saturating_minus(a.deam_confirms_skipped, b.deam_confirms_skipped);
  d.deam_ambiguous_edges = saturating_minus(a.deam_ambiguous_edges, b.deam_ambiguous_edges);
  d.deam_seed_routed = saturating_minus(a.deam_seed_routed, b.deam_seed_routed);
  return d;
}

std::array<StagePart, 6> fit_parts(const FitWork &delta, std::uint64_t endpoint_ns,
                                   std::uint64_t exception_ns, double &excess) noexcept {
  const std::array<std::uint64_t, 7> &us = delta.bucket_us;
  const std::uint64_t deam_ns = us[bucket(ft::Bucket::Deam)] * 1000U;
  const std::uint64_t endpoints = std::min(endpoint_ns, deam_ns);
  const std::uint64_t exceptions = std::min(exception_ns, deam_ns - endpoints);
  excess = 1.0e-9 * static_cast<double>((endpoint_ns - endpoints) + (exception_ns - exceptions));
  const auto s_us = [&us](std::initializer_list<ft::Bucket> buckets) {
    std::uint64_t sum = 0;
    for (const ft::Bucket b : buckets) sum += us[bucket(b)];
    return 1.0e-6 * static_cast<double>(sum);
  };
  return {StagePart{Stage::Endpoints, 1.0e-9 * static_cast<double>(endpoints)},
          StagePart{Stage::ReferenceExceptions, 1.0e-9 * static_cast<double>(exceptions)},
          StagePart{Stage::Inverses,
                    1.0e-9 * static_cast<double>(deam_ns - endpoints - exceptions)},
          StagePart{Stage::CurveRepair, s_us({ft::Bucket::Solve, ft::Bucket::Project})},
          StagePart{Stage::FinalGrid,
                    s_us({ft::Bucket::Price, ft::Bucket::Certify, ft::Bucket::Calendar})},
          StagePart{Stage::FitOther, s_us({ft::Bucket::Other})}};
}

Json fit_work_json(const FitWork &w) {
  Json bucket_us = Json::object();
  Json al_by_bucket = Json::object();
  for (std::size_t b = 0; b < ft::kBucketCount; ++b) {
    bucket_us[ft::kBucketNames[b]] = w.bucket_us[b];
    al_by_bucket[ft::kBucketNames[b]] = w.al_by_bucket[b];
  }
  Json al_by_reason = Json::object();
  for (std::size_t r = 0; r < ft::kDeamReasonCount; ++r) {
    al_by_reason[ft::kDeamReasonNames[r]] = w.deam_al_by_reason[r];
  }
  return {{"timed_chains", w.chains}, {"chain_wall_us", w.wall_us}, {"bucket_us", bucket_us},
          {"al_boundary_solves_by_bucket", al_by_bucket},
          {"grid_prices_al_solves", w.al_by_bucket[bucket(ft::Bucket::Price)]},
          {"repaired_node_al_solves", w.al_by_bucket[bucket(ft::Bucket::Certify)]},
          {"al_boundary_solves", w.al_price_calls}, {"al_greeks_bundles", w.al_greeks_calls},
          {"iv_newton_iters", w.iv_newton_iters},
          {"certification_passes", w.certification_passes},
          {"channel_solves", w.channel_solves}, {"jacobian_evals", w.jacobian_evals},
          {"rung_attempts", w.rung_attempts}, {"calendar_pairs", w.calendar_pairs},
          {"projections", w.projections}, {"repair_rung_visits", w.repair_rung_visits},
          {"inversions", w.deam_inversions}, {"inversion_al_solves", w.deam_inversion_al},
          {"premium_inversions", w.deam_premium},
          {"premium_fallbacks", w.deam_premium_fallbacks},
          {"premium_fallbacks_by_reason", w.deam_fallback_by_reason},
          {"inversion_al_solves_by_reason", al_by_reason}, {"confirms", w.deam_confirms},
          {"confirms_skipped", w.deam_confirms_skipped},
          {"ambiguous_edges", w.deam_ambiguous_edges}, {"seed_routed", w.deam_seed_routed}};
}

Json stage_clock_json(const DeAmStageClock &c) {
  std::uint64_t fallback_ns = 0;
  for (const std::uint64_t ns : c.premium_fallback_ns) fallback_ns += ns;
  const auto s = [](std::uint64_t ns) { return 1.0e-9 * static_cast<double>(ns); };
  return {{"endpoint_bands", c.endpoint_bands}, {"endpoint_s", s(c.endpoint_ns)},
          {"premium_fallback_s", s(fallback_ns)},
          {"premium_fallback_ns_by_reason", c.premium_fallback_ns},
          {"fast_redos", c.fast_redos}, {"fast_redo_s", s(c.fast_redo_ns)},
          {"premium_fallback_reason_index", "IvPremiumFallback: 1 not applicable, 2 range edge "
                                            "or non-monotone band, 3 no European seed, 4 not "
                                            "increasing, 5 vol bound, 6 pricer failure, 7 no "
                                            "convergence"}};
}

Json stages_json(const PhaseClock &clock, double cold, bool family_built) {
  Json stages = Json::object();
  for (std::size_t s = 0; s < kStageCount; ++s) {
    const auto stage = static_cast<Stage>(s);
    if (stage == Stage::FamilyBuildValidation && !family_built) {
      stages[kStageNames[s]] = {{"seconds", nullptr}, {"status", "not_built"},
                                {"reason", "no family build or family validation on this fit "
                                           "route (family kernels are pricing-only here)"}};
      continue;
    }
    stages[kStageNames[s]] = {{"seconds", clock.seconds(stage)}, {"status", "timed"}};
  }
  const double sum = clock.total();
  const double residual = cold - sum;
  return {{"stages", std::move(stages)}, {"sum_s", sum}, {"cold_calibration_s", cold},
          {"unattributed_s", residual},
          {"unattributed_fraction", cold > 0.0 ? residual / cold : 0.0},
          {"reconciles_within_1pct", std::fabs(residual) <= 0.01 * cold},
          {"attribution_conflict_s", clock.conflict_seconds()}};
}

Json build_info_json() {
  Json out = Json::object();
#if defined(__VERSION__)
  out["compiler"] = __VERSION__;
#elif defined(_MSC_VER)
  out["compiler"] = "MSVC " + std::to_string(_MSC_VER);
#endif
#if defined(ATX_VOL_BENCH_CXX_FLAGS)
  out["cxx_flags"] = ATX_VOL_BENCH_CXX_FLAGS;
  out["flags_source"] = "embedded at compile time (CMAKE_CXX_FLAGS + per-config flags)";
#else
  out["cxx_flags"] = nullptr;
  out["flags_source"] = "unavailable: not embedded; read the configure cache";
#endif
#if defined(ATX_VOL_BENCH_BUILD_TYPE)
  out["cmake_build_type"] = ATX_VOL_BENCH_BUILD_TYPE;
#endif
#if defined(NDEBUG)
  out["build_type"] = "release";
#else
  out["build_type"] = "debug";
#endif
#if defined(__AVX2__)
  out["isa_compiled"] = "avx2";
#elif defined(__AVX__)
  out["isa_compiled"] = "avx";
#else
  out["isa_compiled"] = "sse2";
#endif
#if defined(__FMA__)
  out["fma_compiled"] = true;
#else
  out["fma_compiled"] = false;
#endif
#if defined(__FAST_MATH__) || defined(_M_FP_FAST) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
  out["fast_math"] = true;
#else
  out["fast_math"] = false;
#endif
  out["iec559_double"] = std::numeric_limits<double>::is_iec559;
  out["simd_runtime"] = {{"have_avx2", simd::have_avx2()}, {"use_avx2", simd::use_avx2()},
                         {"isa_override", isa_name(simd::simd_isa_override())}};
  out["exact_counters_compiled"] = counters::counters_enabled();
  return out;
}

std::size_t deam_memo_entries() noexcept { return deam_iv_memo_size(); }

Json cold_reset() {
  const std::size_t deam_entries = deam_iv_memo_size();
  deam_iv_memo_clear();
  carry_memo::reset();
  Json cleared = Json::array();
  cleared.push_back({{"state", "de-Am IV thread memo (deam_iv_cache.cpp t_memo)"},
                     {"scope", "calling thread: the fit worker at ATX_VOL_FIT_WORKERS=1; a "
                               "wider fan-out runs each pass on fresh jthreads whose memo "
                               "dies at the join"},
                     {"entries_before", deam_entries}, {"entries_after", deam_iv_memo_size()}});
  cleared.push_back({{"state", "carry memo (carry_memo.hpp, process-wide ring)"},
                     {"scope", "process"}, {"entries_before", "unavailable: no size probe"}});
  Json not_reset = Json::array();
  not_reset.push_back({{"state", "per-expiry rung memo (rung_memo.hpp)"},
                       {"why", "a member of each fit_quote_surface call (SurfaceFitter "
                               "memos_); never crosses fits"}});
  not_reset.push_back({{"state", "American IV thread ALO slot (american_iv.cpp t_alo_slot)"},
                       {"why", "allocation scratch re-initialised by every inversion; no "
                               "market value crosses calls (src/pricing is out of scope)"}});
  not_reset.push_back({{"state", "static quadrature / differentiation / calendar tables"},
                       {"why", "market-independent constants built on first use; only the "
                               "fresh-process runner pays them"}});
  not_reset.push_back({{"state", "solve ledger, lightweight sampler"},
                       {"why", "telemetry only; reps read deltas"}});
  not_reset.push_back({{"state", "OS file cache, CPU caches and predictors, heap"},
                       {"why", "not controllable in-process"}});
  return {{"applied", true}, {"cleared", std::move(cleared)},
          {"not_reset", std::move(not_reset)}};
}

#if defined(_WIN32)
namespace {

// The documented layout of SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION (winternl.h).
struct ProcessorPerformance {
  LARGE_INTEGER idle;
  LARGE_INTEGER kernel; // includes idle
  LARGE_INTEGER user;
  LARGE_INTEGER reserved1[2];
  ULONG reserved2;
};
constexpr int kSystemProcessorPerformanceInformation = 8;
using NtQuerySystemInformationFn = LONG(WINAPI *)(int, void *, ULONG, ULONG *);
using NtQueryTimerResolutionFn = LONG(WINAPI *)(ULONG *, ULONG *, ULONG *);

// The documented PROCESSOR_POWER_INFORMATION record of CallNtPowerInformation.
struct ProcessorPower {
  ULONG number;
  ULONG max_mhz;
  ULONG current_mhz;
  ULONG mhz_limit;
  ULONG max_idle_state;
  ULONG current_idle_state;
};

template <class Fn> [[nodiscard]] Fn ntdll_export(const char *name) noexcept {
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return nullptr;
  const FARPROC proc = ::GetProcAddress(ntdll, name);
  if (proc == nullptr) return nullptr;
  // SAFETY: GetProcAddress returns the export as a generic FARPROC; Fn is the export's
  // documented signature, so calling through Fn is a call through the function's own
  // type. The round trip via void (*)() is the conversion clang exempts from
  // -Wcast-function-type.
  return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(proc));
}

struct Core {
  std::uint64_t mask{0};
  unsigned efficiency_class{0};
  bool smt{false};
};

// Group-0 physical cores (GetLogicalProcessorInformationEx RelationProcessorCore).
[[nodiscard]] std::vector<Core> physical_cores() {
  std::vector<Core> cores;
  DWORD length = 0;
  ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (length == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return cores;
  std::vector<std::uint8_t> buffer(length);
  // SAFETY: the API writes a sequence of SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX records
  // into the byte buffer (operator new alignment), each starting at the previous record's
  // offset + Size; the reads below stay inside `length` (the pattern of
  // fit_scheduler.cpp / pricing_executor.cpp).
  auto *first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
  if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length)) return cores;
  for (DWORD offset = 0; offset < length;) {
    const auto *info =
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    if (info->Size == 0) break;
    if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1 &&
        info->Processor.GroupMask[0].Group == 0) {
      cores.push_back({static_cast<std::uint64_t>(info->Processor.GroupMask[0].Mask),
                       info->Processor.EfficiencyClass,
                       (info->Processor.Flags & LTP_PC_SMT) != 0});
    }
    offset += info->Size;
  }
  return cores;
}

[[nodiscard]] std::string cpu_brand() {
  std::array<int, 4> regs{};
  __cpuid(regs.data(), static_cast<int>(0x80000000U));
  if (static_cast<unsigned>(regs[0]) < 0x80000004U) return "unknown";
  std::array<char, 49> brand{};
  for (unsigned leaf = 0; leaf < 3; ++leaf) {
    __cpuid(regs.data(), static_cast<int>(0x80000002U + leaf));
    for (std::size_t r = 0; r < 4; ++r) {
      for (std::size_t b = 0; b < 4; ++b) {
        brand[16U * leaf + 4U * r + b] =
            static_cast<char>((static_cast<unsigned>(regs[r]) >> (8U * b)) & 0xffU);
      }
    }
  }
  std::string out(brand.data());
  const auto begin = out.find_first_not_of(' ');
  return begin == std::string::npos ? std::string{} : out.substr(begin);
}

[[nodiscard]] double busy_pct(const CpuSample &before, const CpuSample &after, std::size_t cpu) {
  if (cpu >= before.busy.size() || cpu >= after.busy.size()) return kNaN;
  const std::uint64_t total = saturating_minus(after.total[cpu], before.total[cpu]);
  if (total == 0U) return kNaN;
  return 100.0 * static_cast<double>(saturating_minus(after.busy[cpu], before.busy[cpu])) /
         static_cast<double>(total);
}

[[nodiscard]] Json power_json(std::optional<unsigned> cpu) {
  Json out = Json::object();
  GUID *scheme = nullptr;
  if (::PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme != nullptr) {
    const GUID &g = *scheme;
    std::array<char, 40> text{};
    std::snprintf(text.data(), text.size(), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                  g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    out["active_scheme_guid"] = std::string(text.data());
    ::LocalFree(scheme);
  } else {
    out["active_scheme_guid"] = nullptr;
  }
  const unsigned logical = std::max(1U, std::thread::hardware_concurrency());
  std::vector<ProcessorPower> power(logical);
  const auto bytes = static_cast<ULONG>(power.size() * sizeof(ProcessorPower));
  if (::CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(), bytes) == 0) {
    const std::size_t at = cpu.value_or(::GetCurrentProcessorNumber());
    if (at < power.size()) {
      out["cpu_mhz"] = {{"cpu", at}, {"max", power[at].max_mhz},
                        {"current", power[at].current_mhz}, {"limit", power[at].mhz_limit}};
    }
  }
  return out;
}

[[nodiscard]] Json timer_json() {
  Json out = Json::object();
  LARGE_INTEGER frequency{};
  out["qpc_frequency_hz"] =
      ::QueryPerformanceFrequency(&frequency) ? Json(frequency.QuadPart) : Json(nullptr);
  if (const auto query = ntdll_export<NtQueryTimerResolutionFn>("NtQueryTimerResolution")) {
    ULONG coarsest = 0;
    ULONG finest = 0;
    ULONG current = 0;
    if (query(&coarsest, &finest, &current) >= 0) {
      out["timer_resolution_100ns"] = {
          {"coarsest", coarsest}, {"finest", finest}, {"current", current}};
    }
  }
  return out;
}

} // namespace

CpuSample sample_cpus() {
  CpuSample out;
  const auto query = ntdll_export<NtQuerySystemInformationFn>("NtQuerySystemInformation");
  if (query == nullptr) return out;
  const unsigned logical = std::max(1U, std::thread::hardware_concurrency());
  std::vector<ProcessorPerformance> perf(logical);
  ULONG returned = 0;
  const auto bytes = static_cast<ULONG>(perf.size() * sizeof(ProcessorPerformance));
  if (query(kSystemProcessorPerformanceInformation, perf.data(), bytes, &returned) < 0) {
    return out;
  }
  const std::size_t n =
      std::min<std::size_t>(perf.size(), returned / sizeof(ProcessorPerformance));
  for (std::size_t i = 0; i < n; ++i) {
    const auto idle = static_cast<std::uint64_t>(perf[i].idle.QuadPart);
    const auto total = static_cast<std::uint64_t>(perf[i].kernel.QuadPart) +
                       static_cast<std::uint64_t>(perf[i].user.QuadPart);
    out.total.push_back(total);
    out.busy.push_back(saturating_minus(total, idle));
  }
  return out;
}

void pin_to_logical_cpu(unsigned cpu) {
  if (cpu >= 64U) {
    throw std::runtime_error("--affinity: logical CPU must be below 64 (group 0)");
  }
  DWORD_PTR process_mask = 0;
  DWORD_PTR system_mask = 0;
  if (!::GetProcessAffinityMask(::GetCurrentProcess(), &process_mask, &system_mask)) {
    throw std::runtime_error("--affinity: GetProcessAffinityMask failed");
  }
  const DWORD_PTR want = DWORD_PTR{1} << cpu;
  if ((system_mask & want) == 0U) {
    throw std::runtime_error("--affinity: logical CPU " + std::to_string(cpu) +
                             " is not in the system affinity mask " + hex_mask(system_mask));
  }
  if (!::SetProcessAffinityMask(::GetCurrentProcess(), want)) {
    throw std::runtime_error("--affinity: SetProcessAffinityMask failed");
  }
  if (::SetThreadAffinityMask(::GetCurrentThread(), want) == 0U) {
    throw std::runtime_error("--affinity: SetThreadAffinityMask failed");
  }
  ::SwitchToThread(); // let the scheduler move this thread onto the pinned CPU now
}

std::optional<bool> is_performance_cpu(unsigned cpu) {
  const std::vector<Core> cores = physical_cores();
  if (cores.empty() || cpu >= 64U) return std::nullopt;
  unsigned max_class = 0;
  for (const Core &core : cores) max_class = std::max(max_class, core.efficiency_class);
  for (const Core &core : cores) {
    if ((core.mask >> cpu) & 1U) return core.efficiency_class == max_class;
  }
  return std::nullopt;
}

Json affinity_json(std::optional<unsigned> requested, bool require_pcore,
                   const CpuSample &before, const CpuSample &after) {
  Json out = Json::object();
  out["requested_cpu"] = requested ? Json(*requested) : Json(nullptr);
  out["pinned"] = requested.has_value();
  out["require_pcore"] = require_pcore;
  out["cpu_model"] = cpu_brand();
  out["logical_processors"] = std::thread::hardware_concurrency();
  const std::vector<Core> cores = physical_cores();
  unsigned max_class = 0;
  unsigned min_class = std::numeric_limits<unsigned>::max();
  for (const Core &core : cores) {
    max_class = std::max(max_class, core.efficiency_class);
    min_class = std::min(min_class, core.efficiency_class);
  }
  out["physical_cores"] = cores.size();
  out["heterogeneous"] = !cores.empty() && max_class != min_class;
  out["max_efficiency_class"] = max_class;
  const unsigned current = ::GetCurrentProcessorNumber();
  out["current_cpu_at_receipt"] = current;
  const unsigned subject = requested.value_or(current);
  Json siblings = Json::array();
  out["efficiency_class"] = nullptr;
  out["is_pcore"] = nullptr;
  for (const Core &core : cores) {
    if (((core.mask >> subject) & 1U) == 0U) continue;
    out["efficiency_class"] = core.efficiency_class;
    out["is_pcore"] = core.efficiency_class == max_class;
    out["core_mask"] = hex_mask(core.mask);
    out["smt"] = core.smt;
    for (unsigned b = 0; b < 64U; ++b) {
      if (b != subject && ((core.mask >> b) & 1U)) siblings.push_back(b);
    }
  }
  out["smt_siblings"] = siblings;
  DWORD_PTR process_mask = 0;
  DWORD_PTR system_mask = 0;
  if (::GetProcessAffinityMask(::GetCurrentProcess(), &process_mask, &system_mask)) {
    out["process_affinity_mask"] = hex_mask(process_mask);
    out["system_affinity_mask"] = hex_mask(system_mask);
  }
  GROUP_AFFINITY thread_affinity{};
  if (::GetThreadGroupAffinity(::GetCurrentThread(), &thread_affinity)) {
    out["thread_affinity_mask"] = hex_mask(thread_affinity.Mask);
  }
  Json busy = {{"source", "NtQuerySystemInformation per-CPU kernel+user-idle, best effort"},
               {"subject_cpu_pct", finite_or_null(busy_pct(before, after, subject))}};
  Json sibling_pct = Json::array();
  bool sibling_idle = true;
  for (const auto &s : siblings) {
    const double pct = busy_pct(before, after, s.get<std::size_t>());
    sibling_pct.push_back(finite_or_null(pct));
    sibling_idle = sibling_idle && std::isfinite(pct) && pct < 5.0;
  }
  busy["smt_sibling_pct"] = sibling_pct;
  busy["smt_sibling_idle_below_5pct"] = sibling_idle;
  double system_busy = 0.0;
  double system_total = 0.0;
  for (std::size_t i = 0; i < before.busy.size() && i < after.busy.size(); ++i) {
    system_busy += static_cast<double>(saturating_minus(after.busy[i], before.busy[i]));
    system_total += static_cast<double>(saturating_minus(after.total[i], before.total[i]));
  }
  busy["system_pct"] =
      system_total > 0.0 ? Json(100.0 * system_busy / system_total) : Json(nullptr);
  out["busy"] = std::move(busy);
  out["fit_workers"] = fit_workers_json();
  out["nested_pools"] = {{"surface_expiry_fanout", atx_auto_worker_count()},
                         {"pricing_executor", "not on the quote-slice fit route"},
                         {"rule", "every nested compute pool must be 1 on a pinned run"}};
  out["power"] = power_json(requested);
  out["timer"] = timer_json();
  out["process_priority_class"] = ::GetPriorityClass(::GetCurrentProcess());
  out["thread_priority"] = ::GetThreadPriority(::GetCurrentThread());
  return out;
}

double seconds_since_process_creation() noexcept {
  FILETIME creation{};
  FILETIME exit_time{};
  FILETIME kernel{};
  FILETIME user{};
  if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
    return kNaN;
  }
  FILETIME now{};
  ::GetSystemTimePreciseAsFileTime(&now);
  const auto ticks = [](const FILETIME &f) {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32U) | f.dwLowDateTime;
  };
  return 1.0e-7 * static_cast<double>(saturating_minus(ticks(now), ticks(creation)));
}

#else // !_WIN32: no topology or affinity API; every probe reports unavailable.

CpuSample sample_cpus() { return {}; }
void pin_to_logical_cpu(unsigned) {
  throw std::runtime_error("--affinity is implemented for Windows only");
}
std::optional<bool> is_performance_cpu(unsigned) { return std::nullopt; }
Json affinity_json(std::optional<unsigned> requested, bool require_pcore, const CpuSample &,
                   const CpuSample &) {
  return {{"requested_cpu", requested ? Json(*requested) : Json(nullptr)},
          {"pinned", false}, {"require_pcore", require_pcore}, {"available", false},
          {"fit_workers", fit_workers_json()}};
}
double seconds_since_process_creation() noexcept { return kNaN; }

#endif // _WIN32

} // namespace atx::vol::bench
