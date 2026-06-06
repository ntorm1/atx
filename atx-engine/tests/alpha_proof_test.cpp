// atx::engine::alpha — P3-9 integration PROOF suite.
//
// This is the sprint-close evidence battery for the alpha-expression DSL. It
// exercises the WHOLE pipeline end-to-end — parse_program -> analyze -> compile
// -> evaluate (two ways) — and proves the three load-bearing rails plus an
// informational CSE/throughput bench:
//
//   1. Differential (AlphaProof_Differential): a multi-alpha Alpha101-style
//      program (element-wise + cross-sectional + time-series + nesting) compiled
//      ONCE and evaluated by BOTH the fast VM (vm.hpp::Engine) and the slow,
//      obviously-correct reference oracle (oracle.hpp::evaluate_reference). The
//      VM is bit-exact with the oracle BY DESIGN, so we assert cell-by-cell
//      bit-identity via a NaN-aware `same_cell` — any divergence is a real bug.
//
//   2. Determinism (AlphaProof_Determinism): a canonical `signal_hash` folds
//      atx::core::hash_combine over the ORDERED (alpha, date, instrument, value)
//      cell stream. Repeat-run FAST hashes match; FAST hash == ORACLE hash; and
//      — crucially NOT a vacuous pass — the hash CHANGES under three mutations
//      (a single input cell, an instrument-column reorder, a flipped universe
//      bit). NaN cells fold a fixed sentinel so the hash is canonical w.r.t. the
//      same NaN-equivalence `same_cell` uses (the differential already proves
//      bit-identity; the hash equality must not be hostage to an exotic NaN bit
//      pattern, while mutation sensitivity rides on the finite cells).
//
//   3. No-look-ahead (AlphaProof_NoLookahead): truncation invariance. Evaluating
//      a panel truncated to its first t+1 dates must reproduce, BYTE-IDENTICALLY,
//      every cell at date <= t of the full-panel result. This is the §3.3
//      causality rail: rows > t are invisible to outputs at <= t. Time-series
//      ops (the only non-trivially-causal family) are in the program.
//
//   4. Bench (AlphaProof_Bench, INFORMATIONAL — no asserts): a mined-style
//      program with heavy subexpression overlap. Reports unique/total AST nodes
//      and their ratio (the CSE-lever evidence), num_slots, and an ns/cell +
//      alphas/s throughput measurement over a sizable panel.
//
// NOTE: no parallel/multi-thread evaluator is built or exercised — it is
// explicitly deferred this sprint; determinism is proved single-thread via
// repeat-run identity plus mutation sensitivity.
//
// Naming: Subject_Condition_ExpectedResult.

#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace {

using atx::core::hash_combine;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// A process-lifetime Library: the Ast borrows OpSig pointers from it, so it must
// outlive every parse result. One shared instance keeps every test consistent.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly value-equal (covers +-inf, +-0). This
// is the bit-aware comparison the differential asserts: the VM reproduces the
// oracle EXACTLY, so equality is the right bar (not a loose tolerance).
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// ===========================================================================
//  Pipeline helper — parse_program -> analyze -> compile, asserting each stage.
// ===========================================================================

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] SignalSet eval_fast(const Program &prog, const Panel &panel) {
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

[[nodiscard]] SignalSet eval_oracle(const Program &prog, const Panel &panel) {
  auto out = evaluate_reference(prog, panel);
  EXPECT_TRUE(out.has_value()) << "oracle: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

// ===========================================================================
//  Synthetic panel — a realistic date x instrument block.
//
//  Fields (in this order): open high low close volume vwap returns plus an
//  IndClass.sector integer-label classifier (labels 0..num_sectors-1 widened to
//  f64) for the group ops. Prices are positive, `returns` is small, and the data
//  is drawn from a FIXED-SEED mt19937_64 (never time-seeded -> deterministic).
//
//  Realism the proof asks for:
//   * Delisted symbols: instruments whose universe bit goes 0 for date >=
//     delist_date (they remain present — no survivorship — but read as missing).
//   * NaN gaps: a few scattered NaN source cells.
// ===========================================================================

struct PanelData {
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols; // one column per field, date-major
  std::vector<std::uint8_t> universe;      // dates*instruments {0,1}
};

[[nodiscard]] PanelData make_panel_data(atx::usize dates, atx::usize instruments,
                                        std::uint64_t seed, int num_sectors = 4) {
  const atx::usize cells = dates * instruments;
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{10.0, 200.0};
  std::uniform_real_distribution<atx::f64> spread{0.0, 5.0};
  std::uniform_real_distribution<atx::f64> vol{1.0e4, 1.0e6};
  std::uniform_real_distribution<atx::f64> ret{-0.05, 0.05};
  std::uniform_int_distribution<int> sector{0, num_sectors - 1};

  PanelData pd;
  pd.dates = dates;
  pd.instruments = instruments;
  pd.names = {"open", "high", "low", "close", "volume", "vwap", "returns", "IndClass.sector"};
  pd.cols.assign(pd.names.size(), std::vector<atx::f64>(cells));

  for (atx::usize i = 0; i < cells; ++i) {
    const atx::f64 base = price(rng);
    const atx::f64 hi = base + spread(rng);
    const atx::f64 lo = base - spread(rng);
    pd.cols[0][i] = base;                               // open
    pd.cols[1][i] = hi;                                 // high
    pd.cols[2][i] = lo;                                 // low
    pd.cols[3][i] = lo + (hi - lo) * 0.5;               // close (inside [low, high])
    pd.cols[4][i] = vol(rng);                           // volume (positive)
    pd.cols[5][i] = (hi + lo + pd.cols[3][i]) / 3.0;    // vwap (typical price)
    pd.cols[6][i] = ret(rng);                           // returns (small)
    pd.cols[7][i] = static_cast<atx::f64>(sector(rng)); // IndClass.sector
  }

  // All-in-universe to start, then delist a few instruments mid-sample.
  pd.universe.assign(cells, std::uint8_t{1});
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_inst_a = 1;
    const atx::usize delist_inst_b = instruments - 1;
    const atx::usize delist_date = dates * 2 / 3; // last third is delisted
    for (atx::usize d = delist_date; d < dates; ++d) {
      pd.universe[d * instruments + delist_inst_a] = 0;
      pd.universe[d * instruments + delist_inst_b] = 0;
    }
  }

  // Scatter a handful of NaN source cells (data gaps) into close and volume.
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  if (cells >= 8) {
    pd.cols[3][cells / 5] = nan;     // close gap
    pd.cols[4][cells / 3] = nan;     // volume gap
    pd.cols[3][cells * 4 / 5] = nan; // a later close gap
  }
  return pd;
}

[[nodiscard]] Panel panel_from(const PanelData &pd) {
  auto p = Panel::create(pd.dates, pd.instruments, pd.names, pd.cols, pd.universe);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments, std::uint64_t seed,
                               int num_sectors = 4) {
  return panel_from(make_panel_data(dates, instruments, seed, num_sectors));
}

// ===========================================================================
//  The proven alpha battery — Alpha101-style, expressible from shipped ops.
//
//  Adjustments from the plan's draft (recorded for the ledger):
//   * `scale(x)` -> `scale(x, 1)`: the registry's `scale` is arity-2 (CsScale
//     reads its target scale `a` from the 2nd operand); a bare `scale(x)` fails
//     arity. `a=1` is the canonical unit-L1-norm scaling.
//  Every other line is shipped verbatim. The whole thing is ONE program (one
//  compile), exercising element-wise + cross-sectional + time-series + nesting.
// ===========================================================================

[[nodiscard]] std::string_view battery_src() {
  return "a1 = rank(close - open)\n"
         "a2 = -1 * correlation(rank(close), rank(volume), 5)\n"
         "a3 = scale(close - ts_mean(close, 10), 1)\n"
         "a4 = (close > open) ? rank(volume) : -rank(volume)\n"
         "a5 = ts_std(returns, 5) * indneutralize(close, IndClass.sector)\n"
         "a6 = delta(close, 2) / (ts_mean(volume, 5) + 1)\n"
         "a7 = group_rank(vwap, IndClass.sector)\n";
}

// A program whose time-series reach is real (windows up to 10) so truncation
// invariance is a genuine test, not a trivially-causal one. Reuses the battery.
[[nodiscard]] std::string_view causal_src() { return battery_src(); }

// ===========================================================================
//  signal_hash — a canonical digest of a SignalSet.
//
//  Folds atx::core::hash_combine over the ORDERED cell stream
//  (alpha_index, date, instrument, bits(value)) in canonical alpha->date->
//  instrument order. A NaN cell folds a FIXED sentinel rather than its raw bits:
//  this makes the hash canonical w.r.t. the SAME NaN-equivalence `same_cell`
//  uses, so FAST hash == ORACLE hash follows from the proven bit-identity
//  WITHOUT being hostage to an exotic NaN payload. Mutation sensitivity rides on
//  the finite cells (and on NaN-ness flips), so this loses no detection power.
// ===========================================================================

[[nodiscard]] std::size_t signal_hash(const SignalSet &ss) noexcept {
  constexpr std::uint64_t kNanSentinel = 0x7FF8'0000'0000'0001ULL; // canonical NaN tag
  std::size_t seed = 0;
  seed = hash_combine(seed, static_cast<std::size_t>(ss.alphas.size()),
                      static_cast<std::size_t>(ss.dates), static_cast<std::size_t>(ss.instruments));
  for (atx::usize a = 0; a < ss.alphas.size(); ++a) {
    const std::vector<atx::f64> &v = ss.alphas[a].values;
    for (atx::usize d = 0; d < ss.dates; ++d) {
      for (atx::usize inst = 0; inst < ss.instruments; ++inst) {
        const atx::usize idx = d * ss.instruments + inst;
        const atx::f64 cell = idx < v.size() ? v[idx] : std::numeric_limits<atx::f64>::quiet_NaN();
        const std::uint64_t bits =
            std::isnan(cell) ? kNanSentinel : std::bit_cast<std::uint64_t>(cell);
        seed = hash_combine(seed, static_cast<std::size_t>(a), static_cast<std::size_t>(d),
                            static_cast<std::size_t>(inst), static_cast<std::size_t>(bits));
      }
    }
  }
  return seed;
}

// ===========================================================================
//  1. Differential battery — FAST == ORACLE, cell-by-cell, bit-identical.
// ===========================================================================

TEST(AlphaProof_Differential, Alpha101Battery_FastEqualsOracle_BitIdentical) {
  const atx::usize dates = 24;       // > ~16: exercises the 10-bar windows
  const atx::usize instruments = 12; // > ~8: meaningful cross-sections / groups
  const Panel panel = make_panel(dates, instruments, 0xA1FA9001ULL);

  const Program prog = compile_ok(battery_src());
  ASSERT_EQ(prog.roots.size(), 7U) << "battery must compile to 7 alpha roots";

  const SignalSet fast = eval_fast(prog, panel);
  const SignalSet oracle = eval_oracle(prog, panel);

  ASSERT_EQ(fast.alphas.size(), oracle.alphas.size());
  ASSERT_EQ(fast.dates, oracle.dates);
  ASSERT_EQ(fast.instruments, oracle.instruments);
  ASSERT_EQ(fast.dates, dates);
  ASSERT_EQ(fast.instruments, instruments);

  atx::usize divergences = 0;
  for (atx::usize a = 0; a < fast.alphas.size(); ++a) {
    ASSERT_EQ(fast.alphas[a].values.size(), oracle.alphas[a].values.size());
    for (atx::usize i = 0; i < fast.alphas[a].values.size(); ++i) {
      const atx::f64 fc = fast.alphas[a].values[i];
      const atx::f64 oc = oracle.alphas[a].values[i];
      const bool agree = same_cell(fc, oc);
      if (!agree) {
        ++divergences;
      }
      EXPECT_TRUE(agree) << "alpha '" << fast.alphas[a].name << "' (idx " << a << ") cell " << i
                         << " (date " << i / instruments << ", inst " << i % instruments
                         << "): FAST=" << fc << " ORACLE=" << oc;
    }
  }
  EXPECT_EQ(divergences, 0U) << "FAST==ORACLE differential diverged in " << divergences
                             << " cells — that is a real VM/oracle bug";
}

// ===========================================================================
//  2. Determinism — repeat-run identity, FAST==ORACLE hash, mutation sensitivity.
// ===========================================================================

TEST(AlphaProof_Determinism, RepeatRunIdentical_FastEqualsOracle_MutationSensitive) {
  const atx::usize dates = 20;
  const atx::usize instruments = 10;
  const PanelData base = make_panel_data(dates, instruments, 0xDE7E11ULL);
  const Panel panel = panel_from(base);

  const Program prog = compile_ok(battery_src());

  // Repeat-run determinism: two FAST evaluations of the SAME program+panel.
  const std::size_t fast_h1 = signal_hash(eval_fast(prog, panel));
  const std::size_t fast_h2 = signal_hash(eval_fast(prog, panel));
  EXPECT_EQ(fast_h1, fast_h2) << "FAST evaluate() is not run-to-run deterministic";

  // The two engines agree at the hash level too (the differential proves it cell
  // by cell; this is the folded-digest corollary).
  const std::size_t oracle_h = signal_hash(eval_oracle(prog, panel));
  EXPECT_EQ(fast_h1, oracle_h) << "FAST and ORACLE hashes disagree — differential broken";

  // ---- Mutation sensitivity: the hash MUST change (no vacuous pass) ----

  // (a) change a single input cell value (a finite close cell -> a distinct
  //     finite value; pick one we know is finite and in-universe).
  {
    PanelData m = base;
    const atx::usize victim = 7 * instruments + 3; // date 7, inst 3
    m.universe[victim] = 1;                        // ensure visible
    m.cols[3][victim] = m.cols[3][victim] + 12.5;  // perturb close
    const std::size_t mutated = signal_hash(eval_fast(prog, panel_from(m)));
    EXPECT_NE(fast_h1, mutated) << "hash insensitive to a single input cell change";
  }

  // (b) reorder two instruments' columns (swap inst 0 and inst 2 across every
  //     date in every field) — a different panel that must hash differently.
  {
    PanelData m = base;
    const atx::usize ia = 0;
    const atx::usize ib = 2;
    for (std::vector<atx::f64> &col : m.cols) {
      for (atx::usize d = 0; d < dates; ++d) {
        std::swap(col[d * instruments + ia], col[d * instruments + ib]);
      }
    }
    for (atx::usize d = 0; d < dates; ++d) {
      std::swap(m.universe[d * instruments + ia], m.universe[d * instruments + ib]);
    }
    const std::size_t mutated = signal_hash(eval_fast(prog, panel_from(m)));
    EXPECT_NE(fast_h1, mutated) << "hash insensitive to an instrument-column reorder";
  }

  // (c) flip one universe bit (mask an extra cell that was in-universe).
  {
    PanelData m = base;
    const atx::usize victim = 4 * instruments + 5; // date 4, inst 5
    ASSERT_EQ(m.universe[victim], std::uint8_t{1}) << "victim must start in-universe";
    m.universe[victim] = 0;
    const std::size_t mutated = signal_hash(eval_fast(prog, panel_from(m)));
    EXPECT_NE(fast_h1, mutated) << "hash insensitive to a flipped universe bit";
  }
}

// ===========================================================================
//  3. No-look-ahead — truncation invariance (the §3.3 causality rail).
//
//  Evaluate the full panel, then a panel TRUNCATED to its first t+1 dates (same
//  instruments/fields, universe + columns sliced to those rows). Every cell at
//  date d <= t must be byte-identical between the two: rows > t are invisible to
//  outputs at <= t. Time-series ops (the real test) are in the program.
// ===========================================================================

TEST(AlphaProof_NoLookahead, TruncatedPanel_ReproducesEarlyRows_ByteIdentical) {
  const atx::usize dates = 22;
  const atx::usize instruments = 9;
  const PanelData full = make_panel_data(dates, instruments, 0xCA05A1ULL);
  const Panel full_panel = panel_from(full);

  const Program prog = compile_ok(causal_src());
  const SignalSet full_res = eval_fast(prog, full_panel);
  ASSERT_EQ(full_res.dates, dates);

  const atx::usize t = dates / 2; // a mid-sample cut
  const atx::usize trunc_dates = t + 1;

  // Build the truncated panel = first t+1 date-rows of every field + universe.
  PanelData tr;
  tr.dates = trunc_dates;
  tr.instruments = instruments;
  tr.names = full.names;
  tr.cols.assign(full.cols.size(), {});
  const atx::usize trunc_cells = trunc_dates * instruments;
  for (atx::usize f = 0; f < full.cols.size(); ++f) {
    tr.cols[f].assign(full.cols[f].begin(),
                      full.cols[f].begin() + static_cast<std::ptrdiff_t>(trunc_cells));
  }
  tr.universe.assign(full.universe.begin(),
                     full.universe.begin() + static_cast<std::ptrdiff_t>(trunc_cells));
  const Panel trunc_panel = panel_from(tr);

  const SignalSet trunc_res = eval_fast(prog, trunc_panel);
  ASSERT_EQ(trunc_res.dates, trunc_dates);
  ASSERT_EQ(trunc_res.instruments, instruments);
  ASSERT_EQ(trunc_res.alphas.size(), full_res.alphas.size());

  atx::usize lookahead_violations = 0;
  for (atx::usize a = 0; a < full_res.alphas.size(); ++a) {
    for (atx::usize d = 0; d <= t; ++d) {
      for (atx::usize inst = 0; inst < instruments; ++inst) {
        const atx::f64 full_cell = full_res.alphas[a].values[d * instruments + inst];
        const atx::f64 trunc_cell = trunc_res.alphas[a].values[d * instruments + inst];
        const bool agree = same_cell(full_cell, trunc_cell);
        if (!agree) {
          ++lookahead_violations;
        }
        EXPECT_TRUE(agree) << "LOOK-AHEAD: alpha '" << full_res.alphas[a].name << "' date " << d
                           << " inst " << inst
                           << " changed when later rows were truncated: full=" << full_cell
                           << " trunc=" << trunc_cell;
      }
    }
  }
  EXPECT_EQ(lookahead_violations, 0U)
      << "rows > t leaked into outputs at <= t in " << lookahead_violations << " cells";
}

// ===========================================================================
//  4. CSE + throughput bench (INFORMATIONAL — no asserts).
//
//  A mined-style program with HIGH subexpression overlap: 24 alpha lines reuse a
//  handful of subexpressions (rank(close), ts_mean(close,5), correlation(close,
//  volume,10), ...). The Program's unique/total node counts + their ratio are the
//  CSE-lever evidence; num_slots is the live-buffer peak. We time a warm
//  Engine::evaluate over a 512x256 panel and print ns/cell and alphas/s.
// ===========================================================================

// Volatile sink so the timed loop's result is not optimized away.
volatile atx::f64 g_bench_sink = 0.0;

TEST(AlphaProof_Bench, HighCseProgram_ReportsRatioAndThroughput) {
  const std::string_view mined = "m01 = rank(close)\n"
                                 "m02 = rank(close) - rank(open)\n"
                                 "m03 = rank(close) * ts_mean(close, 5)\n"
                                 "m04 = ts_mean(close, 5) - close\n"
                                 "m05 = correlation(close, volume, 10)\n"
                                 "m06 = correlation(close, volume, 10) * rank(close)\n"
                                 "m07 = ts_mean(close, 5) / (ts_mean(volume, 5) + 1)\n"
                                 "m08 = rank(close) + correlation(close, volume, 10)\n"
                                 "m09 = ts_std(close, 10) * rank(close)\n"
                                 "m10 = ts_mean(close, 5) + ts_mean(close, 5)\n"
                                 "m11 = delta(close, 2) * rank(close)\n"
                                 "m12 = correlation(close, volume, 10) - ts_mean(close, 5)\n"
                                 "m13 = rank(volume) * rank(close)\n"
                                 "m14 = scale(ts_mean(close, 5) - close, 1)\n"
                                 "m15 = ts_std(close, 10) + ts_std(close, 10)\n"
                                 "m16 = rank(close) * rank(close)\n"
                                 "m17 = correlation(close, volume, 10) / (ts_std(close, 10) + 1)\n"
                                 "m18 = ts_mean(close, 5) * ts_mean(volume, 5)\n"
                                 "m19 = rank(close - open) + ts_mean(close, 5)\n"
                                 "m20 = delta(close, 2) + delta(close, 2)\n"
                                 "m21 = rank(close) - ts_std(close, 10)\n"
                                 "m22 = correlation(close, volume, 10) * ts_mean(volume, 5)\n"
                                 "m23 = ts_mean(close, 5) - ts_mean(volume, 5)\n"
                                 "m24 = rank(close) * correlation(close, volume, 10)\n";

  const Program prog = compile_ok(mined);
  const double ratio = prog.total_ast_nodes == 0 ? 0.0
                                                 : static_cast<double>(prog.unique_nodes) /
                                                       static_cast<double>(prog.total_ast_nodes);

  const atx::usize dates = 512;
  const atx::usize instruments = 256;
  const atx::usize cells = dates * instruments;
  const Panel panel = make_panel(dates, instruments, 0xBE0C0DEULL);

  Engine engine{panel};
  auto warm = engine.evaluate(prog); // warm: sizes the SlotPool + scratch
  ASSERT_TRUE(warm.has_value()) << (warm ? "" : warm.error().message());
  const atx::usize num_alphas = warm.value().alphas.size();

  constexpr int kReps = 5;
  const auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < kReps; ++r) {
    auto out = engine.evaluate(prog);
    ASSERT_TRUE(out.has_value());
    g_bench_sink += out.value().alphas.front().values.front();
  }
  const auto t1 = std::chrono::steady_clock::now();

  const double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  const double alpha_cells =
      static_cast<double>(cells) * static_cast<double>(num_alphas) * static_cast<double>(kReps);
  const double ns_per_cell = alpha_cells > 0.0 ? total_ns / alpha_cells : 0.0;
  const double seconds = total_ns / 1.0e9;
  const double alphas_per_s =
      seconds > 0.0 ? (static_cast<double>(num_alphas) * kReps) / seconds : 0.0;

  std::cout << "[ bench    ] AlphaProof CSE/throughput (Debug build):\n"
            << "[ bench    ]   panel            = " << dates << " x " << instruments << " ("
            << cells << " cells)\n"
            << "[ bench    ]   alphas           = " << num_alphas << "\n"
            << "[ bench    ]   unique_nodes     = " << prog.unique_nodes << "\n"
            << "[ bench    ]   total_ast_nodes  = " << prog.total_ast_nodes << "\n"
            << "[ bench    ]   unique/total     = " << ratio << "\n"
            << "[ bench    ]   num_slots        = " << prog.num_slots << "\n"
            << "[ bench    ]   ns/cell          = " << ns_per_cell << "\n"
            << "[ bench    ]   alphas/s         = " << alphas_per_s << "\n";

  RecordProperty("unique_nodes", static_cast<int>(prog.unique_nodes));
  RecordProperty("total_ast_nodes", static_cast<int>(prog.total_ast_nodes));
  RecordProperty("unique_over_total_milli", static_cast<int>(ratio * 1000.0));
  RecordProperty("num_slots", static_cast<int>(prog.num_slots));
  RecordProperty("ns_per_cell_milli", static_cast<int>(ns_per_cell * 1000.0));
  RecordProperty("alphas_per_s", static_cast<int>(alphas_per_s));

  SUCCEED() << "informational bench; no thresholds asserted (machine + Debug dependent)";
}

} // namespace
