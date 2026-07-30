// Entry point for every atx-vol Google Benchmark target. The per-suite
// translation unit (american_pricing_bench.cpp / portfolio_throughput_bench.cpp
// / e2e_hotpath_bench.cpp / ...) registers its cases via BENCHMARK()/Apply and is
// linked into this single executable.
//
// ── M3 (infra-measure) — quiet-window bench protocol · class: tooling ────────
// This host is an i7-1260P: a P+E hybrid with no CPU-frequency pinning, so raw
// benchmark numbers are only citable under a disciplined protocol. Instead of the
// bare BENCHMARK_MAIN() this custom main() wraps the standard run with:
//
//   (a) P-CORE PINNING — configure_pricing_executor(Topology::PerformanceCores)
//       before the first pool use, so the pricing executor's workers land on the
//       performance cores (best-effort on Windows; falls back to Auto sizing if
//       P-core discovery or the affinity API fails — affinity is a latency prior,
//       never a correctness gate). NOTE: this pins the *pricing* executor pool; the
//       fitter's own parallel_for pool shares the same core budget/env cap but is
//       not pinned by this call, so a fit-heavy row (fit/e2e/*) is only partially
//       covered — cap its fan-out with ATX_VOL_FIT_WORKERS and lease the box.
//   (b) TURBO/THERMAL PREAMBLE + WARMUP — a stderr note reminding the operator to
//       quiesce the box, plus a brief active warmup that spins the CPU into turbo
//       before the first measured case (each case additionally carries the
//       apply_common() >=0.5 s MinWarmUpTime). Skip the warmup with
//       ATX_BENCH_NO_WARMUP=1.
//   (c) CV GATE — a run is only trustworthy when its coefficient of variation
//       (stddev/mean over the 5 repetitions apply_common() records) is <= 5%.
//       cv_gate_accepts() mechanizes that threshold; the operator takes best-of-N
//       and rejects/flags any kept run whose row is NOISY (compare_baseline.py
//       reads the same `cv` statistic and never gates a CV>5% row).
//   (d) PER-ISA BASELINE NAMING — this exe knows its own build ISA (__AVX2__) and
//       REFUSES to write a --benchmark_out that lands under a baselines/ directory
//       unless the filename is tagged with this build's ISA (and not the other's),
//       so an AVX2 run can never overwrite an SSE2 baseline (or vice-versa).
//       Convention: i7-1260p-clang18-{sse2,avx2}-<bench>.json.
//
// `--atx-self-check` runs the (c)+(d) self-tests (CV gate rejects a synthetic
// high-variance sample / accepts a low-variance one; naming enforcement accepts
// this-ISA and rejects other-ISA / ISA-less names) and exits without touching
// Google Benchmark. It is the in-tree verification for M3.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/detail/pricing_executor.hpp"

#include "bench_util.hpp"

namespace atx::vol::bench {
namespace {

// ── Build ISA identity (compile-time) ────────────────────────────────────────
// The `rel` preset is x64-default SSE2; `rel-avx2` adds global /arch:AVX2 which
// makes clang-cl define __AVX2__. That single macro distinguishes the two ISA
// builds whose baselines must never collide.
#if defined(__AVX2__)
constexpr std::string_view kBuildIsa = "avx2";
constexpr std::string_view kSelfIsaTag = "-avx2-";
constexpr std::string_view kOtherIsaTag = "-sse2-";
#else
constexpr std::string_view kBuildIsa = "sse2";
constexpr std::string_view kSelfIsaTag = "-sse2-";
constexpr std::string_view kOtherIsaTag = "-avx2-";
#endif

// The CV acceptance threshold. A kept run must sit at or under this to be citable;
// compare_baseline.py uses the same 5% line to decide NOISY vs comparable.
constexpr double kCvGateThreshold = 0.05;

[[nodiscard]] bool cv_gate_accepts(double cv) noexcept { return cv <= kCvGateThreshold; }

// A baseline filename is valid for THIS build iff it carries this build's ISA tag
// and not the other build's — so an ISA-less or wrong-ISA name is rejected.
[[nodiscard]] bool baseline_name_matches_build_isa(std::string_view filename) noexcept {
  return filename.find(kSelfIsaTag) != std::string_view::npos &&
         filename.find(kOtherIsaTag) == std::string_view::npos;
}

// Enforce per-ISA naming for a --benchmark_out target. Only files that land under
// a canonical `baselines/` directory are gated (scratch outputs elsewhere are
// left alone). Returns 0 to proceed, non-zero to refuse.
[[nodiscard]] int enforce_baseline_path_isa(std::string_view out_path) {
  if (out_path.empty()) {
    return 0;
  }
  std::string norm(out_path);
  std::replace(norm.begin(), norm.end(), '\\', '/');
  if (norm.find("baselines/") == std::string::npos) {
    return 0; // not a canonical baseline write — nothing to enforce.
  }
  const std::size_t slash = norm.find_last_of('/');
  const std::string filename =
      (slash == std::string::npos) ? norm : norm.substr(slash + 1);
  if (baseline_name_matches_build_isa(filename)) {
    return 0;
  }
  std::fprintf(stderr,
               "[atx-vol bench] REFUSING baseline write: '%s'\n"
               "  This is an ISA=%.*s build; a baselines/ file must contain '%.*s' and not '%.*s'\n"
               "  so sse2 and avx2 baselines never overwrite each other.\n"
               "  Rename to e.g. i7-1260p-clang18-%.*s-<bench>.json and re-run.\n",
               filename.c_str(), static_cast<int>(kBuildIsa.size()), kBuildIsa.data(),
               static_cast<int>(kSelfIsaTag.size()), kSelfIsaTag.data(),
               static_cast<int>(kOtherIsaTag.size()), kOtherIsaTag.data(),
               static_cast<int>(kBuildIsa.size()), kBuildIsa.data());
  return 2;
}

// The --benchmark_out=<path> (or two-token --benchmark_out <path>) value, if any.
[[nodiscard]] std::string find_benchmark_out(int argc, char **argv) {
  constexpr std::string_view key = "--benchmark_out=";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg.size() >= key.size() && arg.substr(0, key.size()) == key) {
      return std::string(arg.substr(key.size()));
    }
    if (arg == "--benchmark_out" && (i + 1) < argc) {
      return std::string(argv[i + 1]);
    }
  }
  return {};
}

// True iff the named environment variable is present and non-empty. Uses
// _dupenv_s under MSVC/clang-cl because plain std::getenv trips /WX
// (-Wdeprecated-declarations) — the same read parallel_for.hpp / e2e bench use.
[[nodiscard]] bool env_flag_set(const char *name) noexcept {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&raw, &n, name) != 0 || raw == nullptr) {
    return false;
  }
  const bool set = raw[0] != '\0';
  std::free(raw);
  return set;
#else
  const char *raw = std::getenv(name);
  return raw != nullptr && raw[0] != '\0';
#endif
}

void print_quiet_window_preamble() {
  std::fprintf(stderr,
               "[atx-vol bench] quiet-window protocol (M3, tooling) | build ISA=%.*s\n"
               "  * pricing executor pinned to P-cores (best-effort; fitter pool shares the\n"
               "    core budget — cap fan-out with ATX_VOL_FIT_WORKERS and lease the box)\n"
               "  * i7-1260P is a P/E hybrid with NO freq pinning: quiesce all background load;\n"
               "    turbo/thermal drift is real — take best-of-N and REJECT any kept row CV>5%%\n"
               "  * warmup: brief turbo prime here + >=0.5 s MinWarmUpTime per case\n"
               "  * baselines are per-ISA: i7-1260p-clang18-{sse2,avx2}-<bench>.json (enforced)\n",
               static_cast<int>(kBuildIsa.size()), kBuildIsa.data());
}

// A brief active warmup so the first measured case does not eat the cold-clock /
// pre-turbo transient. Bounded to ~300 ms of real work; opt out with
// ATX_BENCH_NO_WARMUP=1. The volatile-fed accumulator keeps the loop from being
// optimized away.
void turbo_warmup() {
  if (env_flag_set("ATX_BENCH_NO_WARMUP")) {
    return;
  }
  using Clock = std::chrono::steady_clock;
  const Clock::time_point start = Clock::now();
  volatile double seed = 1.000001;
  double acc = 0.0;
  while (std::chrono::duration<double>(Clock::now() - start).count() < 0.30) {
    for (int i = 0; i < 20000; ++i) {
      acc += std::sqrt(seed + static_cast<double>(i));
    }
    seed = acc; // keep the result live across iterations
  }
  benchmark::DoNotOptimize(acc);
}

// ── M3 self-check (class: tooling) ───────────────────────────────────────────
// Proves the CV gate rejects a synthetic high-variance sample and accepts a
// low-variance one, and that per-ISA baseline naming is enforced. Returns 0 on
// success (all checks PASS), 1 otherwise.
[[nodiscard]] int run_self_check() {
  int failures = 0;
  const auto check = [&](const char *what, bool ok) {
    std::fprintf(stderr, "[atx-bench self-check] %-52s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
      ++failures;
    }
  };

  // (c) CV gate — a tight cluster (~0.3% CV) accepts; a wild spread (~26% CV) rejects.
  const std::vector<double> low_variance{100.0, 100.4, 99.7, 100.2, 99.9};
  const std::vector<double> high_variance{80.0, 120.0, 70.0, 130.0, 95.0};
  const double cv_low = stat_cv(low_variance);
  const double cv_high = stat_cv(high_variance);
  check("CV gate accepts low-variance sample (CV<=5%)", cv_gate_accepts(cv_low));
  check("CV gate rejects high-variance sample (CV>5%)", !cv_gate_accepts(cv_high));
  std::fprintf(stderr, "   cv_low=%.4f%%  cv_high=%.4f%%  threshold=%.1f%%\n", cv_low * 100.0,
               cv_high * 100.0, kCvGateThreshold * 100.0);

  // (d) Per-ISA baseline naming.
  const std::string self_name =
      std::string("i7-1260p-clang18-") + std::string(kBuildIsa) + "-e2e-hotpath.json";
  const std::string other_isa(kBuildIsa == "avx2" ? "sse2" : "avx2");
  const std::string other_name =
      std::string("i7-1260p-clang18-") + other_isa + "-e2e-hotpath.json";
  const std::string isa_less = "i7-1260p-clang18-e2e-hotpath.json";
  check("naming accepts THIS-ISA baseline name", baseline_name_matches_build_isa(self_name));
  check("naming rejects OTHER-ISA baseline name", !baseline_name_matches_build_isa(other_name));
  check("naming rejects ISA-less baseline name", !baseline_name_matches_build_isa(isa_less));
  // End-to-end path enforcement (only gates under baselines/).
  check("enforce refuses OTHER-ISA under baselines/",
        enforce_baseline_path_isa("bench/baselines/" + other_name) != 0);
  check("enforce allows THIS-ISA under baselines/",
        enforce_baseline_path_isa("bench/baselines/" + self_name) == 0);
  check("enforce ignores scratch output (not under baselines/)",
        enforce_baseline_path_isa("scratch/e2e.json") == 0);

  std::fprintf(stderr, "[atx-bench self-check] %d failure(s) | build ISA=%.*s\n", failures,
               static_cast<int>(kBuildIsa.size()), kBuildIsa.data());
  return failures == 0 ? 0 : 1;
}

} // namespace
} // namespace atx::vol::bench

int main(int argc, char **argv) {
  namespace b = atx::vol::bench;

  // (1) tooling self-check mode — verify the CV gate + per-ISA naming, then exit
  //     before Google Benchmark ever initializes.
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--atx-self-check") {
      return b::run_self_check();
    }
  }

  // (2) per-ISA naming enforcement — refuse a mislabeled baseline write up front
  //     (before a long run is wasted).
  if (const int rc = b::enforce_baseline_path_isa(b::find_benchmark_out(argc, argv)); rc != 0) {
    return rc;
  }

  // (3) quiet-window protocol — preamble note, P-core pinning, turbo warmup.
  b::print_quiet_window_preamble();
  atx::vol::configure_pricing_executor(
      atx::vol::ExecutorConfig{atx::vol::Topology::PerformanceCores});
  b::turbo_warmup();

  // (4) standard Google Benchmark run (mirrors BENCHMARK_MAIN()).
  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
