// atx::engine::data — adapt_signal unit tests (P2-S6.5).
//
// Suite: DataAdaptSignal
//
// S6.5 wires an EXTERNAL precomputed-signal Dataset into TWO paths:
//   (1) signal-as-feature — a Signal Dataset merges as named panel columns via the
//       existing merge_features_into_panel (no new code); covered by
//       SignalAsFeatureReferenceable.
//   (2) signal-as-library-admission — signal_to_candidates aligns the signal onto
//       the price/panel axis, realizes per-column pnl/position streams via the
//       EXISTING alpha::extract_streams, and synthesizes one library::AlphaCandidate
//       per column (metrics via combine::compute_metrics). The CALLER admits each
//       candidate through the EXISTING library gate — NO new admission logic.
//
// Tests:
//   * SignalColumnAdmitsThroughGate          — a strong planted signal Accepts; a
//       pure-noise column is Rejected by the real deflated/floor gate.
//   * AdmitVerdictMatchesExtractStreamsPath  — DIFFERENTIAL: the adapter's candidate
//       admits identically to a hand-built extract_streams + compute_metrics path.
//   * ProvenanceFlagsExternalSource          — expr_source records dataset+column;
//       parent_hashes empty.
//   * CanonHashDistinctFromMinedGenome       — deterministic, distinct per column,
//       and distinct from a representative mined-genome canonical_hash.
//   * SignalAsFeatureReferenceable           — path (1): a merged signal column
//       resolves as a panel field by name.

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp" // parse_expr, alpha::Library (mined-hash probe)
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp" // analyze (mined-hash probe)
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/data/adapt_feature.hpp"
#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/adapt_signal.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash (mined-hash probe)
#include "atx/engine/factory/genome.hpp"    // factory::Genome (mined-hash probe)
#include "atx/engine/library/library.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_data_adapt_signal_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::extract_streams;
using atx::engine::alpha::Panel;
using atx::engine::alpha::SignalSet;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::compute_metrics;
using atx::engine::combine::GateConfig;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::merge_features_into_panel;
using atx::engine::data::price_to_panel;
using atx::engine::data::Role;
using atx::engine::data::signal_to_candidates;
using atx::engine::data::SignalAdmission;
using atx::engine::exec::ExecutionSimulator;

namespace lib = atx::engine::library;

// ---------------------------------------------------------------------------
//  Planted-signal price + signal fixture.
//
//  N == 4 instruments, T dates with a PERSISTENT cross-sectional return structure:
//  instrument i carries a fixed base return rank (inst 0 best ... inst 3 worst) plus
//  a small deterministic per-period wiggle (so realized Sharpe is large but FINITE,
//  never the degenerate std==0 case). A planted "foresight" signal that ranks
//  instruments by that base structure builds a near-CONSTANT long-top / short-bottom
//  book (low turnover) whose realized return is consistently positive -> a Sharpe and
//  fitness well above the floor and turnover well below the cap: it ADMITS.
//
//  A "noise" signal whose ranking ROTATES every period (uncorrelated with the fixed
//  base structure) builds a book that is long different names each period: its
//  realized mean is ~0 (it does not capture the persistent spread) AND it churns the
//  book every period (high turnover). Either way it does NOT clear the gate.
//
//  pnl alignment (no look-ahead): positions[t] (from the date-t signal) earn
//  ret[t+1]. The persistent structure makes "rank by base order" the right signal at
//  EVERY date t (the next period's structure is the same), so foresight is a fixed
//  per-instrument score (constant in time) — the book it implies is constant after
//  warm-up, so turnover -> ~0.
// ---------------------------------------------------------------------------
namespace {

constexpr usize kT = 40; // dates (>= ~30 gives a stable Sharpe well above the floor)
constexpr usize kN = 4;  // four instruments (graded long/short book)

// Instrument i's base expected return rank: inst 0 = +0.03 ... inst 3 = -0.03 (a
// fixed, persistent cross-sectional spread the foresight signal can capture).
[[nodiscard]] f64 base_ret(usize i) { return 0.03 - 0.02 * static_cast<f64>(i); }

// A small deterministic per-period wiggle (zero-mean across the wiggle, period-
// dependent) so the realized pnl has nonzero variance -> finite Sharpe (no std==0
// degenerate). Tiny relative to base_ret so it never reorders the cross-section.
[[nodiscard]] f64 wiggle(usize t, usize i) {
  const f64 phase = static_cast<f64>((t * 7U + i * 13U) % 5U) - 2.0; // in {-2..2}
  return 0.001 * phase;
}

// Instrument i's simple return booked AT period t (t >= 1): base_ret(i) + wiggle.
// r[0] is unused (no prior price; pnl[0] == 0 structurally).
[[nodiscard]] f64 ret_at(usize t, usize i) {
  if (t == 0U) {
    return 0.0;
  }
  return base_ret(i) + wiggle(t, i);
}

// A guaranteed-coherent trivial Dataset, returned by a fixture helper when its
// (always-true) Dataset::create precondition unexpectedly fails — so a failed
// precondition records ADD_FAILURE and returns a VALID object rather than calling
// .value() on an error Result (which is UB; EXPECT does not stop the function).
[[nodiscard]] Dataset valid_default_dataset() {
  DatasetSchema s;
  s.columns = {"close", "volume"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Price;
  std::vector<std::vector<f64>> data = {{1.0}, {1.0}}; // 1 date x 1 instrument
  auto res = Dataset::create(std::move(s), /*dates=*/{1}, /*instruments=*/{0u}, std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test:default", ""});
  // This construction is unconditionally coherent; ATX-assert via gtest if not.
  EXPECT_TRUE(res.has_value());
  return std::move(res).value();
}

// A price Dataset whose close column realizes ret_at(t,i): close[0]=100, then
// close[t]=close[t-1]*(1+ret_at(t,i)). open/high/low/volume are filled positive so
// with_datafields (vwap/dollar_volume) is well-formed. Date keys are 1..kT (strictly
// ascending, as align_onto requires). Instruments {10,20,30,40}.
[[nodiscard]] Dataset make_price_dataset() {
  DatasetSchema s;
  s.columns = {"open", "high", "low", "close", "volume"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::F64};
  s.role = Role::Price;

  const usize cells = kT * kN;
  std::vector<std::vector<f64>> data(5, std::vector<f64>(cells, 0.0));
  // close (field index 3).
  for (usize i = 0; i < kN; ++i) {
    f64 px = 100.0;
    for (usize t = 0; t < kT; ++t) {
      px *= (1.0 + ret_at(t, i));
      data[3][t * kN + i] = px;
    }
  }
  // open/high/low track close; volume a positive constant.
  for (usize t = 0; t < kT; ++t) {
    for (usize i = 0; i < kN; ++i) {
      const f64 c = data[3][t * kN + i];
      data[0][t * kN + i] = c;        // open
      data[1][t * kN + i] = c * 1.01; // high
      data[2][t * kN + i] = c * 0.99; // low
      data[4][t * kN + i] = 1000.0;   // volume
    }
  }
  std::vector<DateKey> dates(kT);
  for (usize t = 0; t < kT; ++t) {
    dates[t] = static_cast<DateKey>(t + 1);
  }
  auto res = Dataset::create(std::move(s), std::move(dates), {10u, 20u, 30u, 40u}, std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test:prices", "planted"});
  if (!res.has_value()) {
    ADD_FAILURE() << "make_price_dataset: " << res.error().message();
    return valid_default_dataset();
  }
  return std::move(res).value();
}

// A Signal Dataset over the SAME axis carrying two columns:
//   "foresight" — value = the instrument's base-return rank (HIGHER score = higher
//                 expected return). Constant in time (the structure is persistent) ->
//                 a near-constant long-top / short-bottom book that captures the
//                 spread: positive mean, low turnover -> ADMITS.
//   "noise"     — a ranking that ROTATES every period: score = ((i + t) mod kN). It
//                 is uncorrelated with the persistent base structure (zero-mean
//                 realized pnl) and churns the book every period (high turnover) ->
//                 does NOT clear the gate.
[[nodiscard]] Dataset make_signal_dataset(const std::string &source = "external:sentiment") {
  DatasetSchema s;
  s.columns = {"foresight", "noise"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Signal;

  const usize cells = kT * kN;
  std::vector<std::vector<f64>> data(2, std::vector<f64>(cells, 0.0));
  for (usize t = 0; t < kT; ++t) {
    for (usize i = 0; i < kN; ++i) {
      // foresight: higher score for the higher-base-return instrument (inst 0 best).
      data[0][t * kN + i] = base_ret(i);
      // noise: a per-period rotating rank, uncorrelated with the fixed base order.
      data[1][t * kN + i] = static_cast<f64>((i + t) % kN);
    }
  }
  std::vector<DateKey> dates(kT);
  for (usize t = 0; t < kT; ++t) {
    dates[t] = static_cast<DateKey>(t + 1);
  }
  auto res = Dataset::create(std::move(s), std::move(dates), {10u, 20u, 30u, 40u}, std::move(data),
                             /*mask=*/{}, DatasetProvenance{source, "planted-signal"});
  if (!res.has_value()) {
    ADD_FAILURE() << "make_signal_dataset: " << res.error().message();
    return valid_default_dataset();
  }
  return std::move(res).value();
}

// Panel has no public default ctor (built only via price_to_panel here), so on an
// unexpected failure we fall back to a panel over the guaranteed-coherent default
// price Dataset rather than calling .value() on an error Result (UB).
[[nodiscard]] Panel make_price_panel(const Dataset &price) {
  const std::vector<atx::u16> adv{};
  auto res = price_to_panel(price, std::span<const atx::u16>{adv});
  if (!res.has_value()) {
    ADD_FAILURE() << "make_price_panel: " << res.error().message();
    const Dataset fallback = valid_default_dataset();
    auto def = price_to_panel(fallback, std::span<const atx::u16>{adv});
    EXPECT_TRUE(def.has_value());
    return std::move(def).value();
  }
  return std::move(res).value();
}

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::string base = std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s6_5" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

constexpr u64 kMasterSeed = 424242;

} // namespace

// ---------------------------------------------------------------------------
//  Path (2): signal-as-library-admission.
// ---------------------------------------------------------------------------

// The strong planted "foresight" column clears the real gate (Accept); the
// "noise" column fails it (a non-Accept verdict). Same fixture, same gate.
TEST(DataAdaptSignal, SignalColumnAdmitsThroughGate) {
  const Dataset price = make_price_dataset();
  const Dataset signal = make_signal_dataset();
  const Panel panel = make_price_panel(price);
  const ExecutionSimulator sim; // frictionless default -> pure analytic streams
  const WeightPolicy policy;    // defaults: rank, dollar-neutral, gross 1.0

  auto adm = signal_to_candidates(signal, price, panel, sim, policy, /*as_of=*/kT - 1U);
  ASSERT_TRUE(adm.has_value()) << (adm.has_value() ? "" : adm.error().message());
  const SignalAdmission &sa = adm.value();
  ASSERT_EQ(sa.candidates.size(), usize{2}); // foresight, noise

  lib::Library facade = lib::Library::open(tmpdir("gate"), GateConfig{}, {kMasterSeed});
  const AlphaGate gate{GateConfig{}};

  // foresight (column 0): a strong, low-variance positive pnl -> Accept.
  const auto v_fore = facade.admit(sa.candidates[0], gate);
  EXPECT_EQ(v_fore.kind, lib::AdmitKind::Accept)
      << "foresight verdict kind=" << static_cast<int>(v_fore.kind)
      << " sharpe=" << sa.candidates[0].metrics.sharpe
      << " fitness=" << sa.candidates[0].metrics.fitness;

  // noise (column 1): ~zero-mean pnl -> NOT admitted (fails Sharpe/fitness floor).
  const auto v_noise = facade.admit(sa.candidates[1], gate);
  EXPECT_NE(v_noise.kind, lib::AdmitKind::Accept)
      << "noise should be rejected; sharpe=" << sa.candidates[1].metrics.sharpe
      << " fitness=" << sa.candidates[1].metrics.fitness;
}

// DIFFERENTIAL: the adapter's candidate (column 0) is byte-for-byte the same
// realized stream + metrics as a hand-built extract_streams over an equivalent
// SignalSet + compute_metrics. Admit both into two fresh libraries with identical
// cfg+seeds; the AdmitVerdicts (kind) must match.
TEST(DataAdaptSignal, AdmitVerdictMatchesExtractStreamsPath) {
  const Dataset price = make_price_dataset();
  const Dataset signal = make_signal_dataset();
  const Panel panel = make_price_panel(price);
  const ExecutionSimulator sim;
  const WeightPolicy policy;

  // --- Path A: the adapter ---
  auto adm = signal_to_candidates(signal, price, panel, sim, policy, /*as_of=*/kT - 1U);
  ASSERT_TRUE(adm.has_value()) << (adm.has_value() ? "" : adm.error().message());
  const SignalAdmission &sa = adm.value();
  ASSERT_GE(sa.candidates.size(), usize{1});

  // --- Path B: hand-built SignalSet -> extract_streams -> compute_metrics ---
  // The aligned "foresight" column over the panel axis equals the signal Dataset's
  // column verbatim here (same axis, full coverage), so we build the SignalSet
  // directly from the signal column.
  SignalSet ss;
  ss.dates = panel.dates();
  ss.instruments = panel.instruments();
  SignalSet::Alpha a;
  a.name = "foresight";
  auto col = signal.column_by_name("foresight");
  ASSERT_TRUE(col.has_value());
  a.values.assign(col.value().begin(), col.value().end());
  ss.alphas.push_back(std::move(a));

  auto hand_streams = extract_streams(ss, policy, panel, sim);
  ASSERT_TRUE(hand_streams.has_value()) << (hand_streams ? "" : hand_streams.error().message());
  const AlphaStreams &hs = hand_streams.value();
  const auto hand_pnl = hs.pnl(0);
  const usize np = hs.n_periods();
  const usize ni = hs.n_instruments();
  std::span<const f64> hand_pos{hs.pos_flat.data(), np * ni}; // alpha 0 is the first block
  const auto hand_metrics = compute_metrics(hand_pnl, hand_pos, ni, /*book_size=*/1.0);

  // The adapter's column-0 streams must equal the hand path bit-for-bit.
  const auto adp_pnl = sa.candidates[0].pnl;
  ASSERT_EQ(adp_pnl.size(), hand_pnl.size());
  for (usize t = 0; t < adp_pnl.size(); ++t) {
    EXPECT_DOUBLE_EQ(adp_pnl[t], hand_pnl[t]) << "pnl mismatch t=" << t;
  }
  EXPECT_DOUBLE_EQ(sa.candidates[0].metrics.sharpe, hand_metrics.sharpe);
  EXPECT_DOUBLE_EQ(sa.candidates[0].metrics.fitness, hand_metrics.fitness);

  // Admit both into two FRESH libraries with identical gate + seeds; verdicts equal.
  lib::Library lib_a = lib::Library::open(tmpdir("a"), GateConfig{}, {kMasterSeed});
  lib::Library lib_b = lib::Library::open(tmpdir("b"), GateConfig{}, {kMasterSeed});
  const AlphaGate gate{GateConfig{}};

  lib::AlphaCandidate hand_cand{};
  hand_cand.canon_hash = sa.candidates[0].canon_hash ^ 0x1ULL; // distinct key (avoid dedup)
  hand_cand.pnl = hand_pnl;
  hand_cand.pos_flat = hand_pos;
  hand_cand.metrics = hand_metrics;
  hand_cand.prov = sa.candidates[0].prov;
  hand_cand.as_of = sa.candidates[0].as_of;
  hand_cand.source = nullptr;

  const auto va = lib_a.admit(sa.candidates[0], gate);
  const auto vb = lib_b.admit(hand_cand, gate);
  EXPECT_EQ(va.kind, vb.kind) << "adapter verdict (" << static_cast<int>(va.kind)
                              << ") != hand verdict (" << static_cast<int>(vb.kind) << ")";
}

// Provenance records the external dataset source + column name; parent_hashes empty.
TEST(DataAdaptSignal, ProvenanceFlagsExternalSource) {
  const Dataset price = make_price_dataset();
  const Dataset signal = make_signal_dataset("external:my_sentiment_v3");
  const Panel panel = make_price_panel(price);
  const ExecutionSimulator sim;
  const WeightPolicy policy;

  auto adm = signal_to_candidates(signal, price, panel, sim, policy, /*as_of=*/kT - 1U);
  ASSERT_TRUE(adm.has_value()) << (adm.has_value() ? "" : adm.error().message());
  const SignalAdmission &sa = adm.value();
  ASSERT_EQ(sa.candidates.size(), usize{2});

  for (const auto &c : sa.candidates) {
    EXPECT_NE(c.prov.expr_source.find("external"), std::string::npos)
        << "expr_source must mark external: " << c.prov.expr_source;
    EXPECT_NE(c.prov.expr_source.find("my_sentiment_v3"), std::string::npos)
        << "expr_source must carry the dataset source: " << c.prov.expr_source;
    EXPECT_TRUE(c.prov.parent_hashes.empty());
    EXPECT_EQ(c.prov.mutation_op, atx::u16{0});
    EXPECT_EQ(c.prov.seed, u64{0});
  }
  // Column name appears in its own candidate's tag.
  EXPECT_NE(sa.candidates[0].prov.expr_source.find("foresight"), std::string::npos);
  EXPECT_NE(sa.candidates[1].prov.expr_source.find("noise"), std::string::npos);
}

// canon_hash is deterministic (same inputs -> same hash), distinct across columns,
// distinct across dataset sources, AND concretely distinct from a representative
// mined-genome factory::canonical_hash ("rank(close)") — the external tag guarantees
// it cannot collide with the genome-hash space.
TEST(DataAdaptSignal, CanonHashDistinctFromMinedGenome) {
  const Dataset price = make_price_dataset();
  const Dataset signal = make_signal_dataset("external:src_x");
  const Panel panel = make_price_panel(price);
  const ExecutionSimulator sim;
  const WeightPolicy policy;

  auto adm1 = signal_to_candidates(signal, price, panel, sim, policy, /*as_of=*/kT - 1U);
  auto adm2 = signal_to_candidates(signal, price, panel, sim, policy, /*as_of=*/kT - 1U);
  ASSERT_TRUE(adm1.has_value() && adm2.has_value());
  const SignalAdmission &a1 = adm1.value();
  const SignalAdmission &a2 = adm2.value();
  ASSERT_EQ(a1.candidates.size(), usize{2});

  // Deterministic: same inputs -> identical hashes.
  EXPECT_EQ(a1.candidates[0].canon_hash, a2.candidates[0].canon_hash);
  EXPECT_EQ(a1.candidates[1].canon_hash, a2.candidates[1].canon_hash);

  // Distinct across columns (foresight vs noise).
  EXPECT_NE(a1.candidates[0].canon_hash, a1.candidates[1].canon_hash);

  // A different dataset source yields a different hash for the same column.
  const Dataset signal_y = make_signal_dataset("external:src_y");
  auto adm3 = signal_to_candidates(signal_y, price, panel, sim, policy, /*as_of=*/kT - 1U);
  ASSERT_TRUE(adm3.has_value());
  EXPECT_NE(a1.candidates[0].canon_hash, adm3.value().candidates[0].canon_hash);

  EXPECT_NE(a1.candidates[0].canon_hash, u64{0});

  // CONCRETE genome-hash disjointness: build a representative mined genome
  // ("rank(close)") and compute its factory::canonical_hash (the persisted mined
  // dedup key) the same way the search driver does. The external canon_hash must
  // differ from it. (Structurally it must: canonical_hash folds an Ast and never
  // hashes the "external" tag; this pins the guarantee with a real mined key.)
  atx::engine::alpha::Library dsl; // built-ins (rank, etc.) pre-registered
  auto parsed = atx::engine::alpha::parse_expr("rank(close)", dsl);
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  auto info = atx::engine::alpha::analyze(*parsed);
  ASSERT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  const atx::engine::factory::Genome g{std::move(*parsed), std::move(*info), 0};
  const u64 mined_hash = atx::engine::factory::canonical_hash(g);
  EXPECT_NE(a1.candidates[0].canon_hash, mined_hash);
  EXPECT_NE(a1.candidates[1].canon_hash, mined_hash);
}

// ---------------------------------------------------------------------------
//  Path (1): signal-as-feature (covered by the EXISTING merge_features_into_panel).
// ---------------------------------------------------------------------------

// A Signal Dataset merges as panel columns and the column resolves by name.
TEST(DataAdaptSignal, SignalAsFeatureReferenceable) {
  const Dataset price = make_price_dataset();
  const Dataset signal = make_signal_dataset();
  const Panel panel = make_price_panel(price);

  auto res = merge_features_into_panel(panel, price, signal);
  ASSERT_TRUE(res.has_value()) << res.error().message();
  const Panel merged = std::move(res).value();

  // Both signal columns resolve as panel fields by name.
  auto fore = merged.field_id("foresight");
  auto noise = merged.field_id("noise");
  ASSERT_TRUE(fore.has_value()) << "merged panel missing 'foresight' field";
  ASSERT_TRUE(noise.has_value()) << "merged panel missing 'noise' field";

  // The merged field carries the aligned signal value (full coverage on this axis).
  const auto fore_col = merged.field_all(fore.value());
  ASSERT_EQ(fore_col.size(), kT * kN);
  // foresight is the per-instrument base-return rank (constant in time). At any date,
  // inst0 carries the highest score (best base return), inst3 the lowest.
  EXPECT_DOUBLE_EQ(fore_col[0 * kN + 0], base_ret(0));
  EXPECT_DOUBLE_EQ(fore_col[0 * kN + 3], base_ret(3));
  EXPECT_GT(fore_col[0 * kN + 0], fore_col[0 * kN + 3]);
}

} // namespace atxtest_data_adapt_signal_test
