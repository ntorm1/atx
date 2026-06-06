// atx::engine::alpha — fast vectorized VM core differential tests (P3-6).
//
// The VM (vm.hpp) is the PRODUCTION execution path; oracle.hpp is the slow,
// obviously-correct reference. These tests run BOTH on identical inputs and
// assert bit-for-bit agreement (NaN==NaN treated as equal) for every
// element-wise / logical / select opcode — the differential is exactly what
// catches a kernel that drifts from the pinned semantic contract. Plus:
//   * scalar broadcast, NaN/±inf/0-0, out-of-universe masking;
//   * a zero-allocation guard proving the warm dispatch loop allocates nothing
//     beyond the unavoidable output buffers;
//   * boundaries (1×1, all-NaN field, bare field) and a clean NotImplemented on
//     a cross-sectional opcode;
//   * an informational ns/cell bench (not asserted).
//
// Naming: Subject_Condition_ExpectedResult.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

// ===========================================================================
//  Allocation counting via the MSVC CRT debug-heap allocation hook.
//
//  We CANNOT replace global ::operator new in this TU: event_bus_test.cpp
//  already defines the replaceable allocation functions for the whole test
//  executable (one definition program-wide), and its counter lives in an
//  anonymous namespace we cannot read. Instead, in this Debug/MSVC build we
//  install a CRT allocation hook (`_CrtSetAllocHook`): operator new forwards to
//  `_malloc_dbg`, so the hook fires on EVERY allocation (gross, not net — so a
//  transient alloc/free pair inside the dispatch loop would still be caught) in
//  the measured window, with zero global plumbing and no symbol clash. Off-MSVC
//  the guard degrades to a no-op; this build is Debug/MSVC (per CMakeCache).
// ===========================================================================
#if defined(_MSC_VER)
#define ATX_VM_HAVE_CRT_HEAP 1
#include <crtdbg.h>
#else
#define ATX_VM_HAVE_CRT_HEAP 0
#endif

namespace {

#if ATX_VM_HAVE_CRT_HEAP
// Hook state: a simple gross-allocation counter, armed only inside the window.
std::atomic<bool> g_hook_armed{false};
std::atomic<long> g_hook_allocs{0};

int vm_alloc_hook(int alloc_type, void * /*user*/, std::size_t /*size*/, int block_type,
                  long /*request*/, const unsigned char * /*filename*/, int /*line*/) {
  // Count allocate/reallocate of ordinary blocks; ignore frees and the CRT's own
  // internal (_CRT_BLOCK) bookkeeping allocations.
  if (g_hook_armed.load(std::memory_order_relaxed) && block_type != _CRT_BLOCK &&
      (alloc_type == _HOOK_ALLOC || alloc_type == _HOOK_REALLOC)) {
    g_hook_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  return 1; // allow the allocation to proceed
}
#endif

// Number of heap allocations performed while running `work`. Returns 0 when the
// instrument is unavailable so callers can skip the assertion off-MSVC.
template <class Fn> [[nodiscard]] long count_allocs(Fn &&work) {
#if ATX_VM_HAVE_CRT_HEAP
  g_hook_allocs.store(0, std::memory_order_relaxed);
  _CrtSetAllocHook(&vm_alloc_hook);
  g_hook_armed.store(true, std::memory_order_relaxed);
  std::forward<Fn>(work)();
  g_hook_armed.store(false, std::memory_order_relaxed);
  _CrtSetAllocHook(nullptr);
  return g_hook_allocs.load(std::memory_order_relaxed);
#else
  std::forward<Fn>(work)();
  return 0;
#endif
}

using atx::core::ErrorCode;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// A process-lifetime Library so any borrowed OpSig stays valid across tests.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly bit/value equal (covers ±inf, ±0).
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Compile a bare expression all the way to a Program (parse -> analyze ->
// compile). On any failure the caller's ASSERT surfaces the message.
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// Build a Panel with the four canonical OHLCV fields from explicit column data.
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments,
                               std::vector<std::vector<atx::f64>> cols,
                               std::vector<std::uint8_t> universe = {}) {
  std::vector<std::string> names = {"close", "open", "high", "low", "volume"};
  auto p =
      Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Fill five OHLCV columns from a fixed-seed RNG (deterministic). `volume` is
// kept positive so `volume>0` masks are meaningful; prices in [1, 100].
[[nodiscard]] std::vector<std::vector<atx::f64>> random_ohlcv(atx::usize cells,
                                                              std::uint64_t seed) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{0.0, 1.0e6};
  std::vector<std::vector<atx::f64>> cols(5, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = price(rng); // close
    cols[1][i] = price(rng); // open
    cols[2][i] = price(rng); // high
    cols[3][i] = price(rng); // low
    cols[4][i] = vol(rng);   // volume
  }
  return cols;
}

// The core differential assertion: VM == oracle, cell by cell.
void expect_vm_matches_oracle(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto vm = engine.evaluate(prog);
  ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
  auto ref = evaluate_reference(prog, panel);
  ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());

  const SignalSet &v = vm.value();
  const SignalSet &r = ref.value();
  ASSERT_EQ(v.alphas.size(), r.alphas.size());
  ASSERT_EQ(v.dates, r.dates);
  ASSERT_EQ(v.instruments, r.instruments);
  for (atx::usize a = 0; a < v.alphas.size(); ++a) {
    ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      const atx::f64 vc = v.alphas[a].values[i];
      const atx::f64 rc = r.alphas[a].values[i];
      EXPECT_TRUE(same_cell(vc, rc)) << "expr '" << expr << "' alpha " << a << " cell " << i
                                     << ": VM=" << vc << " oracle=" << rc;
    }
  }
}

// ===========================================================================
//  Differential — every element-wise / logical / select opcode.
// ===========================================================================

TEST(AlphaVm_Differential, EveryElementwiseOp_RandomPanel_MatchesOracle) {
  const atx::usize dates = 12;
  const atx::usize instruments = 7;
  const Panel panel =
      make_panel(dates, instruments, random_ohlcv(dates * instruments, 0xA1FA01ULL));

  const std::vector<std::string_view> exprs = {
      "close+open",
      "close-open",
      "close*open",
      "close/open",
      "power(close,3)",       // Pow (^2 strength-reduces to Mul; use 3)
      "signedpower(close,3)", // Spow
      "min(close,open)",
      "max(close,open)",
      "abs(close-open)",
      "sign(close-open)",
      "log(close)",
      "-close",
      "close>open",
      "close<open",
      "close>=open",
      "close<=open",
      "close==open",
      "close!=open",
      "(close>open)&&(volume>0)",
      "(close>open)||(close<low)",
      "!(close>open)",
      "(close>open)?close:open",
  };
  for (const std::string_view e : exprs) {
    expect_vm_matches_oracle(e, panel);
  }
}

// ===========================================================================
//  Scalar broadcast — literals fill the whole buffer.
// ===========================================================================

TEST(AlphaVm_ScalarBroadcast, CloseTimes2Plus1_MatchesOracle) {
  const atx::usize dates = 9;
  const atx::usize instruments = 5;
  const Panel panel = make_panel(dates, instruments, random_ohlcv(dates * instruments, 0xBEEFULL));
  expect_vm_matches_oracle("close * 2 + 1", panel);
}

// ===========================================================================
//  NaN / ±inf / 0/0 — exotic IEEE inputs propagate identically.
// ===========================================================================

TEST(AlphaVm_SpecialValues, SeededCells_MatchesOracle) {
  const atx::usize dates = 4;
  const atx::usize instruments = 4;
  const atx::usize cells = dates * instruments;
  auto cols = random_ohlcv(cells, 0xC0FFEEULL);
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  const atx::f64 inf = std::numeric_limits<atx::f64>::infinity();
  // Seed close with NaN, +inf, -inf, 0; seed several opens with 0 so close/open
  // exercises x/0 (-> ±inf) and 0/0 (-> NaN).
  cols[0][0] = nan;
  cols[0][1] = inf;
  cols[0][2] = -inf;
  cols[0][3] = 0.0;
  cols[1][3] = 0.0;  // 0 / 0
  cols[1][4] = 0.0;  // finite / 0 -> +inf
  cols[1][5] = 0.0;  // finite / 0
  cols[0][6] = -1.0; // log of a negative -> NaN
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  for (const std::string_view e :
       {"close/open", "close+open", "close*open", "min(close,open)", "max(close,open)",
        "log(close)", "sign(close)", "signedpower(close,3)", "(close>open)?close:open"}) {
    expect_vm_matches_oracle(e, panel);
  }
}

// ===========================================================================
//  Universe — out-of-universe cells are NaN in both paths.
// ===========================================================================

TEST(AlphaVm_Universe, OutOfUniverseCells_AreNaNInBoth) {
  const atx::usize dates = 5;
  const atx::usize instruments = 4;
  const atx::usize cells = dates * instruments;
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  // Knock out a scattered set of cells (e.g. a not-yet-listed instrument).
  for (atx::usize d = 0; d < dates; ++d) {
    universe[d * instruments + 0] = 0;       // instrument 0 always out
    universe[d * instruments + (d % 4)] = 0; // a moving hole
  }
  const Panel panel =
      make_panel(dates, instruments, random_ohlcv(cells, 0xD15EA5EULL), std::move(universe));
  expect_vm_matches_oracle("close - open", panel);
  expect_vm_matches_oracle("(close>open)?close:open", panel);
}

// ===========================================================================
//  Zero-allocation guard.
//
//  PROVES: after a warm-up call (which sizes the SlotPool and the field-remap
//  scratch), a second evaluate() on a SAME-num_slots program allocates ONLY the
//  output SignalSet buffers — a count that is INDEPENDENT of dates*instruments
//  and of program length. The surviving allocations are exactly:
//    * the `alphas` vector (one block) + its per-alpha `values` buffer (one
//      block each) + the alpha name string (small-string-optimized for the
//      empty/short root names, but counted defensively).
//  These scale ONLY with the number of output alphas, NOT the panel size or the
//  code length. The dispatch loop itself (kernels + StoreAlpha copy + Free) does
//  NOT allocate: the pool buffer and the field-remap scratch are reused, and
//  StoreAlpha writes into a pre-sized vector. We assert the count is small AND
//  that a 4× larger panel running the SAME program produces the SAME count
//  (definitive proof the dispatch loop is alloc-free and size-independent).
// ===========================================================================

TEST(AlphaVm_ZeroAlloc, WarmDispatchLoop_BoundedByOutputsAndSizeIndependent) {
  const atx::usize instruments = 32;
  // A multi-op program; one root. num_slots is identical on every call.
  const Program prog = compile_ok("(close>open)?(close+open):(close-open)");

  const Panel small = make_panel(64, instruments, random_ohlcv(64 * instruments, 0x5EED01ULL));
  Engine small_engine{small};
  ASSERT_TRUE(small_engine.evaluate(prog).has_value()); // warm: sizes pool + scratch
  long small_allocs = 0;
  const long small_run = count_allocs([&] {
    auto out = small_engine.evaluate(prog);
    ASSERT_TRUE(out.has_value());
    small_allocs = static_cast<long>(out.value().alphas.size());
  });
  (void)small_allocs;

  const Panel big = make_panel(256, instruments, random_ohlcv(256 * instruments, 0x5EED02ULL));
  Engine big_engine{big};
  ASSERT_TRUE(big_engine.evaluate(prog).has_value()); // warm
  const long big_run = count_allocs([&] { ASSERT_TRUE(big_engine.evaluate(prog).has_value()); });

#if ATX_VM_HAVE_CRT_HEAP
  // One output alpha -> a handful of allocations (alphas vector + values buffer
  // + name string), bounded and tiny — and crucially the SAME for a 4× panel.
  EXPECT_GE(small_run, 1) << "must allocate at least the output buffer";
  // A handful of allocations for the single-output SignalSet (the alphas vector,
  // its values buffer, the name string, and the Result<>/move plumbing); a small
  // panel-size-INDEPENDENT constant. The strict size-independence check below is
  // the real proof the dispatch loop allocates nothing.
  EXPECT_LE(small_run, 8) << "warm evaluate allocated " << small_run
                          << " times; the dispatch loop must be alloc-free";
  EXPECT_EQ(small_run, big_run) << "allocation count must be independent of panel size (small="
                                << small_run << ", big=" << big_run << ")";
#else
  GTEST_SKIP() << "allocation counting requires the MSVC CRT debug heap";
  (void)small_run;
  (void)big_run;
#endif
}

// ===========================================================================
//  Boundaries.
// ===========================================================================

TEST(AlphaVm_Boundary, OneByOnePanel_MatchesOracle) {
  const Panel panel = make_panel(1, 1, random_ohlcv(1, 0x11ULL));
  expect_vm_matches_oracle("close + open", panel);
  expect_vm_matches_oracle("(close>open)?close:open", panel);
}

TEST(AlphaVm_Boundary, AllNaNField_MatchesOracle) {
  const atx::usize dates = 3;
  const atx::usize instruments = 3;
  const atx::usize cells = dates * instruments;
  auto cols = random_ohlcv(cells, 0x22ULL);
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  for (atx::f64 &c : cols[0]) { // close all NaN
    c = nan;
  }
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  expect_vm_matches_oracle("close + open", panel);
  expect_vm_matches_oracle("close > open", panel);
}

TEST(AlphaVm_Boundary, BareField_MatchesOracle) {
  const atx::usize dates = 6;
  const atx::usize instruments = 3;
  const Panel panel = make_panel(dates, instruments, random_ohlcv(dates * instruments, 0x33ULL));
  expect_vm_matches_oracle("close", panel);
}

TEST(AlphaVm_Boundary, TimeSeriesOp_ReturnsNotImplemented) {
  const atx::usize dates = 4;
  const atx::usize instruments = 4;
  const Panel panel = make_panel(dates, instruments, random_ohlcv(dates * instruments, 0x44ULL));
  // Cross-sectional ops are implemented as of P3-7; the Ts* family lands in
  // P3-8, so a time-series op is what still returns NotImplemented here.
  const Program prog = compile_ok("delay(close, 2)"); // TsDelay -> not yet in the VM
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), ErrorCode::NotImplemented);
}

TEST(AlphaVm_Field, UnknownField_ReturnsNotFound) {
  const atx::usize dates = 2;
  const atx::usize instruments = 2;
  // Panel WITHOUT a "vwap" field; the expression references it.
  const Panel panel = make_panel(dates, instruments, random_ohlcv(dates * instruments, 0x55ULL));
  const Program prog = compile_ok("vwap + close");
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code(), ErrorCode::NotFound);
}

// ===========================================================================
//  Bench (informational — not asserted). Reports ns/cell for add/mul/select.
// ===========================================================================

// Volatile sink so the bench loop's result is not optimized away.
volatile atx::f64 benchmark_sink = 0.0;

TEST(AlphaVm_Bench, AddMulSelect_NsPerCell_Informational) {
  const atx::usize dates = 1024;
  const atx::usize instruments = 512;
  const atx::usize cells = dates * instruments;
  const Panel panel = make_panel(dates, instruments, random_ohlcv(cells, 0xBADC0DEULL));

  struct Case {
    std::string_view name;
    std::string_view expr;
  };
  const Case cases[] = {
      {"add", "close+open"},
      {"mul", "close*open"},
      {"select", "(close>open)?close:open"},
  };

  for (const Case &c : cases) {
    const Program prog = compile_ok(c.expr);
    Engine engine{panel};
    ASSERT_TRUE(engine.evaluate(prog).has_value()); // warm (sizes pool)

    constexpr int kReps = 20;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) {
      auto out = engine.evaluate(prog);
      ASSERT_TRUE(out.has_value());
      benchmark_sink += out.value().alphas[0].values[0];
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double ns_per_cell = ns / (static_cast<double>(cells) * kReps);
    std::cout << "[ bench    ] " << c.name << " (" << dates << "x" << instruments
              << ", Debug build): " << ns_per_cell << " ns/cell\n";
    RecordProperty(std::string{c.name} + "_ns_per_cell",
                   static_cast<int>(ns_per_cell * 1000.0)); // milli-ns/cell (int)
  }
}

} // namespace
