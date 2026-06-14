// atx::engine::data — Alt-data subsumption proof (P2-S6.7, TEST-ONLY).
//
// Suite: DataAltdataSubsumption
//
// ZERO production code is added. These tests PROVE that three alt-data shapes
// (fundamental, news-sentiment, analyst) ingest through the existing S6.1–S6.6
// machinery using only Dataset::create / register_dataset + a schema.
//
// Tests:
//   * FundamentalIngestsAsReferenceDataset   — fundamental Dataset (Role::Reference,
//       market_cap + group_id, restatement as-of versioning). reference_spans()
//       as-of a date that is BEFORE a later restatement row is invisible (PIT /
//       no-look-ahead proof). Resulting FactorComponents == hand-built call.
//   * NewsSentimentGatesViaTradeWhen         — news-sentiment Dataset (Role::Feature,
//       sentiment column). merge_features_into_panel → DSL formula with trade_when
//       gates the base signal on sentiment threshold. Output == hand-computed expected.
//   * AnalystFeatureFeedsFeatureMatrix       — analyst Dataset (Role::Feature,
//       analyst_eps_revision column). merge_features_into_panel → FeatureSpec
//       raw_fields → build_features. Analyst column values reach the FeatureMatrix.
//
// Residual (schema boundary): DatasetSchema carries only f64/I64/Category columns
// in a strictly-ascending date × instrument grid. It CANNOT express:
//   (a) text/blob payloads (e.g. raw news article body),
//   (b) ragged event timestamps (intra-day tick timestamps with irregular cadence).
// These shapes are recorded here as the abstraction's explicit boundary; they are
// "explicitly NOT" in-scope for the current S6 contract.

#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/data/adapt_feature.hpp"
#include "atx/engine/data/adapt_panel.hpp"
#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/learn/feature_matrix.hpp"
#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/risk/exposures.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atxtest_data_altdata_subsumption_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::combine::AlphaStore;
using atx::engine::data::ColumnDType;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetCatalog;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::merge_features_into_panel;
using atx::engine::data::price_to_panel;
using atx::engine::data::reference_spans;
using atx::engine::data::RefSpans;
using atx::engine::data::Role;
using atx::engine::learn::build_features;
using atx::engine::learn::FeatureSpec;
using atx::engine::risk::FactorComponents;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ---------------------------------------------------------------------------
//  Shared DSL helpers (mirror alpha_trade_when_test pattern).
// ---------------------------------------------------------------------------

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] SignalSet eval_panel(const Program &prog, const Panel &panel) {
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

// ---------------------------------------------------------------------------
//  PanelFixture for FactorModelBuilder — minimal ring-buffer PanelView builder
//  (replicated from data_adapt_factor_test pattern, self-contained).
// ---------------------------------------------------------------------------

class PanelFixture {
public:
  PanelFixture(usize n_rows, usize n_inst, const std::vector<std::vector<f64>> &close,
               const std::vector<std::vector<f64>> &volume)
      : n_rows_{n_rows}, n_inst_{n_inst}, cap_{pow2_ceil(n_rows)},
        mask_words_{(n_inst + 63U) / 64U} {
    universe_.reserve(n_inst);
    for (usize i = 0; i < n_inst; ++i) {
      universe_.push_back(InstrumentId{atx::core::domain::Symbol{static_cast<u32>(i + 1U)}});
    }
    fields_.assign(kPanelFieldCount * cap_ * n_inst_, kNaN);
    mask_.assign(cap_ * mask_words_, 0ULL);
    for (usize r = 0; r < n_rows_; ++r) {
      const usize phys = (n_rows_ - 1U) - r;
      for (usize i = 0; i < n_inst_; ++i) {
        const f64 c = close[r][i];
        const f64 v = volume[r][i];
        set(PanelField::Open, phys, i, c);
        set(PanelField::High, phys, i, c);
        set(PanelField::Low, phys, i, c);
        set(PanelField::Close, phys, i, c);
        set(PanelField::Volume, phys, i, v);
        if (!std::isnan(c)) {
          mask_[phys * mask_words_ + (i >> 6U)] |= (1ULL << (i & 63U));
        }
      }
    }
  }

  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           head_(),      n_rows_,
                     mask_words_};
  }

private:
  [[nodiscard]] usize head_() const noexcept { return (n_rows_ == 0U) ? 0U : n_rows_ - 1U; }

  static usize pow2_ceil(usize n) noexcept {
    usize p = 1U;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    const usize block = static_cast<usize>(f) * cap_ * n_inst_;
    fields_[block + phys * n_inst_ + inst] = v;
  }

  usize n_rows_;
  usize n_inst_;
  usize cap_;
  usize mask_words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<u64> mask_;
};

[[nodiscard]] PanelFixture make_panel_fixture(usize n_rows, usize n_inst) {
  std::vector<std::vector<f64>> close(n_rows, std::vector<f64>(n_inst));
  std::vector<std::vector<f64>> volume(n_rows, std::vector<f64>(n_inst, 1000.0));
  for (usize r = 0; r < n_rows; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      close[r][i] = 1.0 + 0.01 * static_cast<f64>((n_rows - r) * n_inst + i);
    }
  }
  return PanelFixture{n_rows, n_inst, close, volume};
}

[[nodiscard]] FactorModelConfig single_sector_cfg() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0x00;
  return cfg;
}

// ---------------------------------------------------------------------------
//  Price Dataset / Panel helper (used by Test 3 — AnalystFeatureFeedsFeatureMatrix).
// ---------------------------------------------------------------------------

// 3 dates × 2 instruments price Dataset with OHLCV.
[[nodiscard]] Dataset make_price_dataset() {
  // dates: {10, 20, 30}; instruments: {100u, 200u}
  // All fields set to a simple deterministic value; "close" matters most.
  DatasetSchema s;
  s.columns = {"open", "high", "low", "close", "volume"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
              ColumnDType::F64};
  s.role = Role::Price;
  const usize cells = 3U * 2U; // 3 dates × 2 instruments
  std::vector<std::vector<f64>> data(5U, std::vector<f64>(cells));
  // Fill: field f, date d, inst i → f*100 + d*10 + i + 1 (all positive, distinct)
  for (usize f = 0; f < 5U; ++f) {
    for (usize d = 0U; d < 3U; ++d) {
      for (usize i = 0U; i < 2U; ++i) {
        data[f][d * 2U + i] = static_cast<f64>(f * 100U + d * 10U + i + 1U);
      }
    }
  }
  auto res = Dataset::create(std::move(s), {10, 20, 30}, {100u, 200u}, std::move(data),
                             /*mask=*/{}, DatasetProvenance{"test:price", "altdata subsumption"});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// ===========================================================================
//  Test 1: FundamentalIngestsAsReferenceDataset
//
//  A fundamental Dataset carries market_cap + group_id with RESTATEMENT:
//   * Original row at date 50:  inst 100 → mc=1e9, grp=1;  inst 200 → mc=2e9, grp=2
//   * Restatement row at date 90: inst 100 → mc=1.5e9, grp=1; inst 200 → mc=2.5e9, grp=2
//
//  as_of_date = 70 (after original, BEFORE restatement):
//   → as-of resolution picks row at date 50 (the restatement at 90 is INVISIBLE).
//   → PIT / no-look-ahead proof: future restatement never colours the past.
//
//  Then feed spans into FactorModelBuilder::build_components and assert the result
//  == a hand-built call with the SAME spans (same dataset picked at same date).
// ===========================================================================
TEST(DataAltdataSubsumption, FundamentalIngestsAsReferenceDataset) {
  // ------------------------------------------------------------------
  // Build fundamental Dataset: Role::Reference, market_cap + group_id,
  // 2 date rows (original at 50, restatement at 90), 2 instruments.
  // ------------------------------------------------------------------
  DatasetSchema fund_schema;
  fund_schema.columns = {"market_cap", "group_id"};
  fund_schema.dtypes = {ColumnDType::F64, ColumnDType::Category};
  fund_schema.role = Role::Reference;
  fund_schema.as_of.effective_dated = true; // restatement versioning declared

  // Date-major layout: cells = 2 dates × 2 instruments = 4 cells.
  // cell index = date_row * 2 + inst_col
  //   date 50 (row 0): inst100 → mc=1e9, grp=1;  inst200 → mc=2e9, grp=2
  //   date 90 (row 1): inst100 → mc=1.5e9, grp=1; inst200 → mc=2.5e9, grp=2
  const std::vector<f64> mc_col = {1e9, 2e9, 1.5e9, 2.5e9};
  const std::vector<f64> grp_col = {1.0, 2.0, 1.0, 2.0};

  auto fund_res =
      Dataset::create(fund_schema, {50, 90}, {100u, 200u}, {mc_col, grp_col}, /*mask=*/{},
                      DatasetProvenance{"external:fundamental", "market_cap+group_id"});
  ASSERT_TRUE(fund_res.has_value()) << fund_res.error().message();
  const Dataset fundamental = std::move(fund_res).value();

  // Register via catalog — the ONLY new-code-free ingestion path.
  DatasetCatalog catalog;
  auto reg_status = catalog.register_dataset("fundamental", fundamental);
  ASSERT_TRUE(reg_status.has_value()) << reg_status.error().message();

  // Resolve back — proves register_dataset round-trips faithfully.
  auto resolved = catalog.resolve("fundamental");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message();
  EXPECT_EQ(resolved->get().num_dates(), usize{2});

  // Build a minimal price Dataset (the axis reference_spans aligns onto).
  DatasetSchema price_schema;
  price_schema.columns = {"close"};
  price_schema.dtypes = {ColumnDType::F64};
  price_schema.role = Role::Price;
  const std::vector<f64> close_col = {100.0, 200.0}; // 1 date × 2 instruments
  auto price_res = Dataset::create(price_schema, {70}, {100u, 200u}, {{close_col}}, /*mask=*/{},
                                   DatasetProvenance{"test:price", ""});
  ASSERT_TRUE(price_res.has_value()) << price_res.error().message();
  const Dataset price = std::move(price_res).value();

  // ------------------------------------------------------------------
  // as-of resolution at date 70 (BEFORE restatement row at 90).
  // The restatement row must be INVISIBLE — PIT no-look-ahead proof.
  // ------------------------------------------------------------------
  constexpr DateKey kAsOfDate = 70;
  auto spans_res = reference_spans(fundamental, price, kAsOfDate);
  ASSERT_TRUE(spans_res.has_value()) << spans_res.error().message();
  const RefSpans &spans = spans_res.value();

  ASSERT_EQ(spans.market_cap.size(), usize{2});
  ASSERT_EQ(spans.group_id.size(), usize{2});

  // Row at date 50 (the pre-restatement value): mc=1e9, grp=1 for inst 100;
  //                                              mc=2e9, grp=2 for inst 200.
  // The row at date 90 (mc=1.5e9, mc=2.5e9) must NOT appear here.
  EXPECT_DOUBLE_EQ(spans.market_cap[0], 1e9)
      << "PIT FAIL: future restatement (1.5e9) leaked into as-of=70 lookup";
  EXPECT_DOUBLE_EQ(spans.market_cap[1], 2e9)
      << "PIT FAIL: future restatement (2.5e9) leaked into as-of=70 lookup";
  EXPECT_EQ(spans.group_id[0], u32{1});
  EXPECT_EQ(spans.group_id[1], u32{2});

  // ------------------------------------------------------------------
  // Confirm the RESTATEMENT is visible at a later as-of date (date 95).
  // ------------------------------------------------------------------
  auto spans_post_res = reference_spans(fundamental, price, /*as_of_date=*/95);
  ASSERT_TRUE(spans_post_res.has_value()) << spans_post_res.error().message();
  const RefSpans &spans_post = spans_post_res.value();
  EXPECT_DOUBLE_EQ(spans_post.market_cap[0], 1.5e9) << "Restatement should be visible at as-of=95";
  EXPECT_DOUBLE_EQ(spans_post.market_cap[1], 2.5e9) << "Restatement should be visible at as-of=95";

  // ------------------------------------------------------------------
  // Feed spans into FactorModelBuilder::build_components.
  // Assert result == a hand-built call with the SAME span values.
  // ------------------------------------------------------------------
  const usize window = 4U;
  const usize n_rows = window + 1U;
  const PanelFixture fx = make_panel_fixture(n_rows, /*n_inst=*/2);

  const std::vector<f64> mkt_caps = {spans.market_cap[0], spans.market_cap[1]};
  const std::vector<u32> group_ids = {spans.group_id[0], spans.group_id[1]};

  FactorModelBuilder builder;
  builder.cfg = single_sector_cfg();

  auto comp_res = builder.build_components(fx.view(), window, std::span<const f64>{mkt_caps},
                                           std::span<const u32>{group_ids});
  ASSERT_TRUE(comp_res.has_value()) << comp_res.error().message();
  const FactorComponents &comp = comp_res.value();

  // Hand-built call with identical span values — must produce byte-identical X/F/D.
  // NOTE: feeding the SAME literal spans, this byte-compare proves only that the
  // adapter→builder path is deterministic (no hidden state). The PIT/no-look-ahead
  // proof is the as-of EXPECTs above (future restatement invisible at as_of=70).
  const std::vector<f64> hand_mc = {1e9, 2e9}; // same as spans (pre-restatement)
  const std::vector<u32> hand_grp = {1U, 2U};
  auto hand_res = builder.build_components(fx.view(), window, std::span<const f64>{hand_mc},
                                           std::span<const u32>{hand_grp});
  ASSERT_TRUE(hand_res.has_value()) << hand_res.error().message();
  const FactorComponents &hand = hand_res.value();

  // Shape equality.
  ASSERT_EQ(comp.X.rows(), hand.X.rows());
  ASSERT_EQ(comp.X.cols(), hand.X.cols());
  ASSERT_EQ(comp.F.rows(), hand.F.rows());
  ASSERT_EQ(comp.F.cols(), hand.F.cols());
  ASSERT_EQ(comp.D.size(), hand.D.size());
  EXPECT_EQ(comp.fit_end, hand.fit_end);

  // Value equality (byte-identical via u64 reinterpret).
  for (Eigen::Index r = 0; r < comp.X.rows(); ++r) {
    for (Eigen::Index c = 0; c < comp.X.cols(); ++c) {
      u64 ba = 0, bb = 0;
      static_assert(sizeof(f64) == sizeof(u64));
      __builtin_memcpy(&ba, &comp.X(r, c), sizeof(u64));
      __builtin_memcpy(&bb, &hand.X(r, c), sizeof(u64));
      EXPECT_EQ(ba, bb) << "X(" << r << "," << c << ") differs";
    }
  }
  for (Eigen::Index i = 0; i < comp.D.size(); ++i) {
    u64 ba = 0, bb = 0;
    __builtin_memcpy(&ba, &comp.D[i], sizeof(u64));
    __builtin_memcpy(&bb, &hand.D[i], sizeof(u64));
    EXPECT_EQ(ba, bb) << "D[" << i << "] differs";
  }
}

// ===========================================================================
//  Test 2: NewsSentimentGatesViaTradeWhen
//
//  A news-sentiment Dataset (Role::Feature, "sentiment" column) is merged into
//  a price Panel via merge_features_into_panel.
//
//  This test EXERCISES trade_when's defining STATE-LATCHING behavior: the
//  sentiment series carries NEUTRAL (0.0) days where neither the enter trigger
//  (sentiment > 0) nor the exit trigger (sentiment < 0) fires. On such a day the
//  operator must HOLD out[t-1] (the latched prior signal), NOT recompute from
//  the current alpha. A strictly-alternating-sign series would degenerate into a
//  stateless pointwise gate and never test this — so we include hold days.
//
//  Setup (1 instrument, 6 dates):
//    date:       0     1     2     3     4     5
//    close:      10    20    30    40    50    60
//    sentiment:  0.8   0.0   0.0  -0.6   0.0   0.7
//
//  DSL formula:
//    "a = trade_when(sentiment > 0, close, sentiment < 0)\n"
//
//  Semantics of trade_when(trigger, alpha, exit) — from the op definition:
//    out[t] = NaN      if exit[t]  > 0         (sentiment < 0: close position)
//           = alpha[t] elif trigger[t] > 0     (sentiment > 0: (re)enter w/ close[t])
//           = out[t-1] else                    (NEUTRAL: HOLD the latched prior)
//    out[0] = (trigger[0]>0 && !(exit[0]>0)) ? alpha[0] : NaN
//
//  Hand-computed expected (the LATCH is what the neutral days prove):
//    d0: sent=0.8>0      → enter            → close[0]=10
//    d1: sent=0.0 neutral → HOLD out[0]     → 10   (LATCH: NOT close[1]=20)
//    d2: sent=0.0 neutral → HOLD out[1]     → 10   (LATCH: still 10, NOT 30)
//    d3: sent=-0.6<0     → exit fires       → NaN
//    d4: sent=0.0 neutral → HOLD out[3]     → NaN  (LATCH: stays flat/closed)
//    d5: sent=0.7>0      → re-enter         → close[5]=60
//  ⇒ expected = [10, 10, 10, NaN, NaN, 60]
//
//  Assert: gated output == this hand-computed STATEFUL expected (and bit-equal
//  to a hand-built Panel path).
// ===========================================================================
TEST(DataAltdataSubsumption, NewsSentimentGatesViaTradeWhen) {
  // ------------------------------------------------------------------
  // Build price Dataset (6 dates, 1 instrument).
  // ------------------------------------------------------------------
  DatasetSchema price_schema;
  price_schema.columns = {"open", "high", "low", "close", "volume"};
  price_schema.dtypes = {ColumnDType::F64, ColumnDType::F64, ColumnDType::F64, ColumnDType::F64,
                         ColumnDType::F64};
  price_schema.role = Role::Price;
  const std::vector<DateKey> dates = {1, 2, 3, 4, 5, 6};
  const std::vector<InstKey> insts = {10u};
  const std::vector<f64> close_vals = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  std::vector<std::vector<f64>> price_data = {
      close_vals,                                 // open (reuse close for simplicity)
      close_vals,                                 // high
      close_vals,                                 // low
      close_vals,                                 // close
      {100.0, 100.0, 100.0, 100.0, 100.0, 100.0}, // volume
  };
  auto price_res = Dataset::create(price_schema, dates, insts, std::move(price_data), /*mask=*/{},
                                   DatasetProvenance{"test:price", "sentiment gate"});
  ASSERT_TRUE(price_res.has_value()) << price_res.error().message();
  const Dataset price = std::move(price_res).value();

  // ------------------------------------------------------------------
  // Build news-sentiment Dataset (Role::Feature, "sentiment" column).
  // Same 6 dates, same 1 instrument. NEUTRAL (0.0) days at d1,d2,d4 are the
  // hold days that exercise trade_when's state latch.
  // ------------------------------------------------------------------
  DatasetSchema sent_schema;
  sent_schema.columns = {"sentiment"};
  sent_schema.dtypes = {ColumnDType::F64};
  sent_schema.role = Role::Feature;
  const std::vector<f64> sentiment = {0.8, 0.0, 0.0, -0.6, 0.0, 0.7};
  auto sent_res = Dataset::create(sent_schema, dates, insts, {{sentiment}}, /*mask=*/{},
                                  DatasetProvenance{"external:news_sentiment", "sentiment column"});
  ASSERT_TRUE(sent_res.has_value()) << sent_res.error().message();
  const Dataset sentiment_ds = std::move(sent_res).value();

  // Register in catalog (the no-new-code ingestion path).
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("news_sentiment", sentiment_ds).has_value());
  ASSERT_TRUE(catalog.register_dataset("price", price).has_value());
  EXPECT_EQ(catalog.role_of("news_sentiment").value(), Role::Feature);

  // ------------------------------------------------------------------
  // Build price Panel and merge sentiment feature.
  // ------------------------------------------------------------------
  const std::vector<atx::u16> adv_windows{};
  auto panel_res = price_to_panel(price, std::span<const atx::u16>{adv_windows});
  ASSERT_TRUE(panel_res.has_value()) << panel_res.error().message();
  const Panel panel_in = std::move(panel_res).value();

  auto merge_res = merge_features_into_panel(panel_in, price, sentiment_ds);
  ASSERT_TRUE(merge_res.has_value()) << merge_res.error().message();
  const Panel merged = std::move(merge_res).value();

  // Confirm "sentiment" resolves in the merged Panel.
  auto sent_fid = merged.field_id("sentiment");
  ASSERT_TRUE(sent_fid.has_value()) << "merged Panel missing 'sentiment' field";

  // ------------------------------------------------------------------
  // DSL formula: trade_when(sentiment > 0, close, sentiment < 0)
  //   trigger = sentiment > 0  (enter when bullish)
  //   exit    = sentiment < 0  (exit when bearish)
  //   alpha   = close
  // ------------------------------------------------------------------
  const Program prog = compile_ok("a = trade_when(sentiment > 0, close, sentiment < 0)\n");
  const SignalSet result = eval_panel(prog, merged);
  ASSERT_EQ(result.alphas.size(), usize{1});
  const std::vector<f64> &v = result.alphas[0].values;
  ASSERT_EQ(v.size(), usize{6}); // 6 dates × 1 instrument

  // Hand-computed STATEFUL expected = [10, 10, 10, NaN, NaN, 60] (see header).
  // The d1/d2 holds at 10 (NOT close[1]=20 / close[2]=30) and the d4 hold at NaN
  // are the LATCH — they only pass if trade_when carries out[t-1] across a
  // non-trigger day rather than gating pointwise.
  EXPECT_DOUBLE_EQ(v[0], 10.0) << "d0: bullish trigger → enter → close=10";
  EXPECT_DOUBLE_EQ(v[1], 10.0) << "d1: neutral → HOLD latched 10 (NOT close[1]=20)";
  EXPECT_DOUBLE_EQ(v[2], 10.0) << "d2: neutral → HOLD latched 10 (NOT close[2]=30)";
  EXPECT_TRUE(std::isnan(v[3])) << "d3: bearish exit fires → NaN";
  EXPECT_TRUE(std::isnan(v[4])) << "d4: neutral → HOLD latched NaN (stays flat)";
  EXPECT_DOUBLE_EQ(v[5], 60.0) << "d5: bullish re-enter → close=60";

  // ------------------------------------------------------------------
  // DIFFERENTIAL equality: gated output == hand-built Panel fed directly.
  //
  // Build a vanilla alpha::Panel (not via Dataset bridge) with the same
  // 'close' and 'sentiment' columns and evaluate the same formula.
  // The outputs must be bit-identical — the Dataset-merge path is a
  // transparent pass-through with no numeric transformation.
  // ------------------------------------------------------------------
  auto hand_panel_res = Panel::create(
      /*dates=*/6, /*instruments=*/1, std::vector<std::string>{"close", "sentiment"},
      std::vector<std::vector<f64>>{close_vals, sentiment}, std::vector<u8>(6, u8{1}));
  ASSERT_TRUE(hand_panel_res.has_value()) << hand_panel_res.error().message();
  const Panel hand_panel = std::move(hand_panel_res).value();

  const SignalSet hand_result = eval_panel(prog, hand_panel);
  ASSERT_EQ(hand_result.alphas.size(), usize{1});
  const std::vector<f64> &hv = hand_result.alphas[0].values;
  ASSERT_EQ(hv.size(), v.size());

  for (usize i = 0; i < v.size(); ++i) {
    const bool both_nan = std::isnan(v[i]) && std::isnan(hv[i]);
    EXPECT_TRUE(both_nan || v[i] == hv[i]) << "merged-path vs hand-panel output differs at cell "
                                           << i << ": merged=" << v[i] << " hand=" << hv[i];
  }
}

// ===========================================================================
//  Test 3: AnalystFeatureFeedsFeatureMatrix
//
//  An analyst Dataset (Role::Feature, "analyst_eps_revision" column) is merged
//  via merge_features_into_panel, referenced in FeatureSpec::raw_fields, and
//  fed into build_features. Assert the analyst column reaches the FeatureMatrix
//  with the correct values — same as the S6.4b feature path, framed as analyst data.
//
//  Setup: 3 dates × 2 instruments (mirrors data_adapt_feature_test pattern).
//    Analyst dataset covers ONLY instrument 100 (inst 200 absent → NaN in matrix).
//    analyst_eps_revision(inst 100) = 0.15 at date 10 (carried forward).
// ===========================================================================
TEST(DataAltdataSubsumption, AnalystFeatureFeedsFeatureMatrix) {
  // ------------------------------------------------------------------
  // Price Dataset: 3 dates × 2 instruments, OHLCV.
  // ------------------------------------------------------------------
  const Dataset price = make_price_dataset();

  // ------------------------------------------------------------------
  // Analyst Dataset: Role::Feature, "analyst_eps_revision" column.
  // Covers only instrument 100 (inst 200 absent → NaN after alignment).
  // 1 date row at date 10 → carried forward to dates 20 and 30 via as-of.
  // ------------------------------------------------------------------
  DatasetSchema analyst_schema;
  analyst_schema.columns = {"analyst_eps_revision"};
  analyst_schema.dtypes = {ColumnDType::F64};
  analyst_schema.role = Role::Feature;
  // 1 date × 1 instrument → single cell: value 0.15
  const std::vector<f64> eps_vals = {0.15};
  auto analyst_res =
      Dataset::create(analyst_schema, /*dates=*/{10}, /*instruments=*/{100u}, {{eps_vals}},
                      /*mask=*/{}, DatasetProvenance{"external:analyst", "eps_revision"});
  ASSERT_TRUE(analyst_res.has_value()) << analyst_res.error().message();
  const Dataset analyst_ds = std::move(analyst_res).value();

  // Register via catalog — no bespoke ingestion code.
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("price", price).has_value());
  ASSERT_TRUE(catalog.register_dataset("analyst_eps", analyst_ds).has_value());
  EXPECT_EQ(catalog.role_of("analyst_eps").value(), Role::Feature);

  // ------------------------------------------------------------------
  // Build price Panel and merge analyst feature.
  // ------------------------------------------------------------------
  const std::vector<atx::u16> adv_windows{};
  auto panel_res = price_to_panel(price, std::span<const atx::u16>{adv_windows});
  ASSERT_TRUE(panel_res.has_value()) << panel_res.error().message();
  const Panel panel_in = std::move(panel_res).value();

  auto merge_res = merge_features_into_panel(panel_in, price, analyst_ds);
  ASSERT_TRUE(merge_res.has_value()) << merge_res.error().message();
  const Panel merged = std::move(merge_res).value();

  // Confirm analyst column resolves.
  auto eps_fid = merged.field_id("analyst_eps_revision");
  ASSERT_TRUE(eps_fid.has_value()) << "merged Panel missing 'analyst_eps_revision' field";

  // Verify aligned values: inst 100 (index 0) → 0.15 at all 3 dates;
  //                        inst 200 (index 1) → NaN (not in analyst dataset).
  const auto eps_all = merged.field_all(eps_fid.value());
  ASSERT_EQ(eps_all.size(), usize{6}); // 3 dates × 2 instruments
  for (usize d = 0; d < 3U; ++d) {
    EXPECT_DOUBLE_EQ(eps_all[d * 2U + 0U], 0.15) << "d=" << d << " inst100";
    EXPECT_TRUE(std::isnan(eps_all[d * 2U + 1U])) << "d=" << d << " inst200 should be NaN";
  }

  // ------------------------------------------------------------------
  // Feed merged Panel into build_features with "analyst_eps_revision"
  // listed in FeatureSpec::raw_fields.
  // ------------------------------------------------------------------
  const AlphaStore store; // empty pool
  FeatureSpec spec;
  spec.raw_fields = {"close", "analyst_eps_revision"};
  spec.pool_alphas = {};
  spec.horizons = {1};
  spec.max_lookback = 0;

  auto fm_res = build_features(merged, store, spec);
  ASSERT_TRUE(fm_res.has_value()) << fm_res.error().message();
  const auto fm = std::move(fm_res).value();

  EXPECT_EQ(fm.n_features, usize{2}); // close, analyst_eps_revision

  // ------------------------------------------------------------------
  // Assert analyst column reaches the FeatureMatrix.
  //   * Canonical instrument index 0 = InstKey 100 → value 0.15, row_valid=1
  //   * Canonical instrument index 1 = InstKey 200 → NaN, row_valid=0
  // ------------------------------------------------------------------
  bool saw_inst100 = false;
  bool saw_inst200 = false;
  for (usize r = 0; r < fm.n_rows(); ++r) {
    const usize inst = fm.row_inst[r];
    // analyst_eps_revision is feature index 1 (second in spec order after close).
    const f64 eps_val = fm.X[r * fm.n_features + 1U];
    if (inst == 0U) { // canonical index 0 == InstKey 100
      saw_inst100 = true;
      EXPECT_DOUBLE_EQ(eps_val, 0.15)
          << "analyst_eps_revision value wrong at row " << r << " (inst100)";
      EXPECT_EQ(fm.row_valid[r], u8{1})
          << "inst100 row " << r << " should be valid (all features finite)";
    }
    if (inst == 1U) { // canonical index 1 == InstKey 200
      saw_inst200 = true;
      EXPECT_TRUE(std::isnan(eps_val)) << "inst200 analyst_eps_revision should be NaN at row " << r;
      EXPECT_EQ(fm.row_valid[r], u8{0})
          << "inst200 row " << r << " should be invalid (NaN feature)";
    }
  }
  EXPECT_TRUE(saw_inst100) << "no inst-100 rows emitted by build_features";
  EXPECT_TRUE(saw_inst200) << "no inst-200 rows emitted by build_features";

  // ------------------------------------------------------------------
  // EQUALITY to hand-built path: run the same FeatureSpec against a
  // hand-constructed Panel with identical columns. Values must match
  // cell-for-cell (the Dataset-merge is a transparent bridge).
  // ------------------------------------------------------------------
  // Close column: field f=3 (OHLCV order), date-major, 3*2=6 cells.
  const auto close_fid = merged.field_id("close");
  ASSERT_TRUE(close_fid.has_value());
  const auto close_all = merged.field_all(close_fid.value());

  // Analyst column: 0.15 for inst 100, NaN for inst 200, all 3 dates.
  std::vector<f64> analyst_hand(6U);
  for (usize d = 0U; d < 3U; ++d) {
    analyst_hand[d * 2U + 0U] = 0.15;
    analyst_hand[d * 2U + 1U] = kNaN;
  }

  // Build hand Panel with the same columns.
  std::vector<f64> close_vec(close_all.begin(), close_all.end());
  auto hand_panel_res = Panel::create(
      /*dates=*/3U, /*instruments=*/2U, std::vector<std::string>{"close", "analyst_eps_revision"},
      std::vector<std::vector<f64>>{close_vec, analyst_hand}, std::vector<u8>(6U, u8{1}));
  ASSERT_TRUE(hand_panel_res.has_value()) << hand_panel_res.error().message();
  const Panel hand_panel = std::move(hand_panel_res).value();

  auto hand_fm_res = build_features(hand_panel, store, spec);
  ASSERT_TRUE(hand_fm_res.has_value()) << hand_fm_res.error().message();
  const auto hand_fm = std::move(hand_fm_res).value();

  ASSERT_EQ(fm.n_rows(), hand_fm.n_rows());
  ASSERT_EQ(fm.n_features, hand_fm.n_features);

  for (usize r = 0; r < fm.n_rows(); ++r) {
    for (usize f = 0; f < fm.n_features; ++f) {
      const f64 va = fm.X[r * fm.n_features + f];
      const f64 vb = hand_fm.X[r * hand_fm.n_features + f];
      const bool both_nan = std::isnan(va) && std::isnan(vb);
      u64 ba = 0, bb = 0;
      __builtin_memcpy(&ba, &va, sizeof(u64));
      __builtin_memcpy(&bb, &vb, sizeof(u64));
      EXPECT_TRUE(both_nan || ba == bb)
          << "FeatureMatrix X[" << r << "," << f << "] merged=" << va << " hand=" << vb;
    }
  }
}

} // namespace atxtest_data_altdata_subsumption_test
