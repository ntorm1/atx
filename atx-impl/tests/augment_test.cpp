// atx-impl — production augment function tests (S5-0, Task 1).
//
// Tests the new `atx::engine::alpha::with_alpha101_fields` production function
// and pins that it is byte-identical to the test helper `augment_for_alpha101`
// (DelegationIdentity) before Task 2 makes the helper delegate.
//
// All tests use `atx_impl_test::make_synth_orats_panel()` — a deterministic,
// env-independent fixture (300d x 24i, 6 sectors, all in-universe). No real
// data, no env-gating, always runs in CI.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring> // std::memcpy (bitwise NaN-safe cell comparisons)
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

// The production function under test (does not exist yet — RED compile failure
// is the expected first-build outcome).
#include "atx/engine/alpha/augment.hpp"

// Synth-panel fixture + reference helper (for DelegationIdentity).
#include "alpha101_support.hpp"

namespace {

using atx::engine::alpha::FieldId;
using atx::engine::alpha::Panel;

// Thin helpers ---------------------------------------------------------

// Find field index by name; returns (usize)-1 if absent.
[[nodiscard]] atx::usize field_idx(const Panel &p, std::string_view name) noexcept {
  for (atx::usize f = 0; f < p.num_fields(); ++f) {
    if (p.field_name(f) == name) {
      return f;
    }
  }
  return static_cast<atx::usize>(-1);
}

// True iff `name` appears exactly once in the panel field dictionary.
[[nodiscard]] bool field_unique(const Panel &p, std::string_view name) noexcept {
  atx::usize count = 0;
  for (atx::usize f = 0; f < p.num_fields(); ++f) {
    if (p.field_name(f) == name) {
      ++count;
    }
  }
  return count == 1u;
}

// Cell accessor: value at (date d, instrument n) for field named `name`.
// Precondition: field exists.
[[nodiscard]] atx::f64 cell(const Panel &p, std::string_view name, atx::usize d,
                            atx::usize n) noexcept {
  const atx::usize fi = field_idx(p, name);
  const std::span<const atx::f64> col = p.field_all(static_cast<FieldId>(fi));
  return col[d * p.instruments() + n];
}

// Hand-rolled trailing mean of `src` over `window` bars ending at date `d`,
// instrument `n` (date-major stride = instruments). Returns NaN if any bar in
// the window is NaN or if d+1 < window.
[[nodiscard]] atx::f64 trailing_mean(std::span<const atx::f64> src, atx::usize instruments,
                                     atx::usize d, atx::usize n, atx::usize window) noexcept {
  if (d + 1 < window) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 sum = 0.0;
  for (atx::usize k = d + 1 - window; k <= d; ++k) {
    const atx::f64 v = src[k * instruments + n];
    if (std::isnan(v)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    sum += v;
  }
  return sum / static_cast<atx::f64>(window);
}

// Build a synth panel WITHOUT the market_cap column (drop it to exercise the
// cap-fallback branch). Copies all columns from `src` except market_cap.
[[nodiscard]] Panel drop_market_cap(const Panel &src) {
  const atx::usize D = src.dates();
  const atx::usize I = src.instruments();
  const atx::usize cells = D * I;

  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> data;
  names.reserve(src.num_fields());
  data.reserve(src.num_fields());

  for (atx::usize f = 0; f < src.num_fields(); ++f) {
    if (src.field_name(f) == "market_cap") {
      continue;
    }
    names.emplace_back(src.field_name(f));
    const std::span<const atx::f64> col = src.field_all(static_cast<FieldId>(f));
    data.emplace_back(col.begin(), col.end());
  }

  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  for (atx::usize d = 0; d < D; ++d) {
    for (atx::usize n = 0; n < I; ++n) {
      universe[d * I + n] = src.in_universe(d, n) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }

  auto p = Panel::create(D, I, std::move(names), std::move(data), std::move(universe));
  return std::move(p).value(); // can't be ragged — copied from a valid Panel
}

} // namespace

// ============================================================================
//  Test 1: AddExpectedColumns
// ============================================================================

TEST(WithAlpha101Fields, AddsExpectedColumns) {
  const Panel base = atx_impl_test::make_synth_orats_panel();
  const std::vector<atx::u16> windows = {20};

  auto result = atx::engine::alpha::with_alpha101_fields(base, windows);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const Panel &aug = result.value();

  // Must contain ALL base fields plus the 8 derived ones.
  static constexpr std::string_view kExpected[] = {
      "open", "high", "low", "close", "volume", "sector", "market_cap",
      "returns", "cap", "IndClass.sector", "IndClass.industry", "IndClass.subindustry",
      "dollar_volume", "vwap", "adv20"};

  for (std::string_view name : kExpected) {
    EXPECT_NE(field_idx(aug, name), static_cast<atx::usize>(-1))
        << "missing field: " << name;
    EXPECT_TRUE(field_unique(aug, name)) << "duplicate field: " << name;
  }

  // Exactly 15 fields (7 base + 8 derived).
  EXPECT_EQ(aug.num_fields(), 15u);
}

// ============================================================================
//  Test 2: Idempotent
// ============================================================================

TEST(WithAlpha101Fields, Idempotent) {
  const Panel base = atx_impl_test::make_synth_orats_panel();
  const std::vector<atx::u16> windows = {20};

  auto r1 = atx::engine::alpha::with_alpha101_fields(base, windows);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();

  // Feed the augmented panel back in — no new columns, same values.
  auto r2 = atx::engine::alpha::with_alpha101_fields(r1.value(), windows);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();

  const Panel &p1 = r1.value();
  const Panel &p2 = r2.value();

  ASSERT_EQ(p1.num_fields(), p2.num_fields()) << "idempotent call changed field count";

  // Same field names in same order.
  for (atx::usize f = 0; f < p1.num_fields(); ++f) {
    EXPECT_EQ(p1.field_name(f), p2.field_name(f)) << "field name differs at index " << f;
  }

  // Spot-check a few cells bitwise-equal (returns, cap, adv20).
  const atx::usize D = p1.dates();
  const atx::usize I = p1.instruments();
  // returns at (5, 3), (100, 11), (D-1, I-1)
  for (auto [d, n] : std::initializer_list<std::pair<atx::usize, atx::usize>>{
           {5, 3}, {100, 11}, {D - 1, I - 1}}) {
    const atx::f64 v1 = cell(p1, "returns", d, n);
    const atx::f64 v2 = cell(p2, "returns", d, n);
    // NaN-equal by bit pattern.
    std::uint64_t b1{};
    std::uint64_t b2{};
    std::memcpy(&b1, &v1, sizeof(b1));
    std::memcpy(&b2, &v2, sizeof(b2));
    EXPECT_EQ(b1, b2) << "returns[" << d << "," << n << "] differs after second call";

    const atx::f64 c1 = cell(p1, "cap", d, n);
    const atx::f64 c2 = cell(p2, "cap", d, n);
    std::uint64_t bc1{};
    std::uint64_t bc2{};
    std::memcpy(&bc1, &c1, sizeof(bc1));
    std::memcpy(&bc2, &c2, sizeof(bc2));
    EXPECT_EQ(bc1, bc2) << "cap[" << d << "," << n << "] differs after second call";
  }
}

// ============================================================================
//  Test 3: ReturnsCorrectValues
// ============================================================================

TEST(WithAlpha101Fields, ReturnsCorrectValues) {
  const Panel base = atx_impl_test::make_synth_orats_panel();
  const std::vector<atx::u16> windows = {20};

  auto result = atx::engine::alpha::with_alpha101_fields(base, windows);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const Panel &aug = result.value();

  const atx::usize I = aug.instruments();

  // Day 0: all instruments must have NaN returns (no prior day).
  for (atx::usize n = 0; n < I; ++n) {
    EXPECT_TRUE(std::isnan(cell(aug, "returns", 0, n)))
        << "returns[0," << n << "] should be NaN (no prior day)";
  }

  // Interior cells: returns[d,n] == close[d,n]/close[d-1,n] - 1
  // (all in-universe on the synth panel, so NaN condition won't fire).
  // Check instruments 0, 5, 12, 23 at date 50.
  for (atx::usize n : {atx::usize{0}, atx::usize{5}, atx::usize{12}, atx::usize{23}}) {
    const atx::usize d = 50;
    const atx::f64 c = cell(aug, "close", d, n);
    const atx::f64 pc = cell(aug, "close", d - 1, n);
    const atx::f64 expected = (pc != 0.0) ? (c / pc - 1.0) : std::numeric_limits<atx::f64>::quiet_NaN();
    const atx::f64 actual = cell(aug, "returns", d, n);
    EXPECT_DOUBLE_EQ(actual, expected) << "returns[" << d << "," << n << "] mismatch";
  }
}

// ============================================================================
//  Test 4: CapFallback
// ============================================================================

TEST(WithAlpha101Fields, CapFallback) {
  const std::vector<atx::u16> windows = {20};

  // Branch (a): panel WITH market_cap -> cap == market_cap cellwise.
  {
    const Panel base = atx_impl_test::make_synth_orats_panel();
    auto result = atx::engine::alpha::with_alpha101_fields(base, windows);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const Panel &aug = result.value();

    const atx::usize D = aug.dates();
    const atx::usize I = aug.instruments();
    // Spot-check every 10th cell.
    for (atx::usize d = 0; d < D; d += 10) {
      for (atx::usize n = 0; n < I; n += 4) {
        const atx::f64 mc = cell(aug, "market_cap", d, n);
        const atx::f64 cap_val = cell(aug, "cap", d, n);
        EXPECT_DOUBLE_EQ(cap_val, mc) << "cap != market_cap at [" << d << "," << n << "]";
      }
    }
  }

  // Branch (b): panel WITHOUT market_cap -> cap == close*1e8 for in-universe cells.
  {
    const Panel base_no_mc = drop_market_cap(atx_impl_test::make_synth_orats_panel());
    auto result = atx::engine::alpha::with_alpha101_fields(base_no_mc, windows);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const Panel &aug = result.value();

    const atx::usize D = aug.dates();
    const atx::usize I = aug.instruments();
    for (atx::usize d = 0; d < D; d += 10) {
      for (atx::usize n = 0; n < I; n += 4) {
        if (aug.in_universe(d, n)) {
          const atx::f64 c = cell(aug, "close", d, n);
          const atx::f64 cap_val = cell(aug, "cap", d, n);
          EXPECT_DOUBLE_EQ(cap_val, c * 1.0e8)
              << "cap fallback != close*1e8 at [" << d << "," << n << "]";
        }
      }
    }
  }
}

// ============================================================================
//  Test 5: MultiAdv
// ============================================================================

TEST(WithAlpha101Fields, MultiAdv) {
  const Panel base = atx_impl_test::make_synth_orats_panel();
  const std::vector<atx::u16> windows = {5, 20, 60};

  auto result = atx::engine::alpha::with_alpha101_fields(base, windows);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const Panel &aug = result.value();

  // All three adv columns must be present.
  EXPECT_NE(field_idx(aug, "adv5"), static_cast<atx::usize>(-1));
  EXPECT_NE(field_idx(aug, "adv20"), static_cast<atx::usize>(-1));
  EXPECT_NE(field_idx(aug, "adv60"), static_cast<atx::usize>(-1));

  const atx::usize I = aug.instruments();

  // adv{d} == ts_mean(dollar_volume, d). dollar_volume = close * volume
  // (all in-universe, so no NaN masking).
  const atx::usize fi_dv = field_idx(aug, "dollar_volume");
  ASSERT_NE(fi_dv, static_cast<atx::usize>(-1));
  const std::span<const atx::f64> dv = aug.field_all(static_cast<FieldId>(fi_dv));

  // Spot-check 3+ cells per window.
  const std::vector<std::pair<atx::usize, atx::usize>> spots = {
      {100, 0}, {200, 5}, {250, 11}};

  for (const auto &[d, n] : spots) {
    // adv5
    {
      const atx::f64 expected = trailing_mean(dv, I, d, n, 5);
      const atx::f64 actual = cell(aug, "adv5", d, n);
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << "adv5[" << d << "," << n << "] should be NaN";
      } else {
        EXPECT_DOUBLE_EQ(actual, expected) << "adv5[" << d << "," << n << "] mismatch";
      }
    }
    // adv20
    {
      const atx::f64 expected = trailing_mean(dv, I, d, n, 20);
      const atx::f64 actual = cell(aug, "adv20", d, n);
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << "adv20[" << d << "," << n << "] should be NaN";
      } else {
        EXPECT_DOUBLE_EQ(actual, expected) << "adv20[" << d << "," << n << "] mismatch";
      }
    }
    // adv60
    {
      const atx::f64 expected = trailing_mean(dv, I, d, n, 60);
      const atx::f64 actual = cell(aug, "adv60", d, n);
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << "adv60[" << d << "," << n << "] should be NaN";
      } else {
        EXPECT_DOUBLE_EQ(actual, expected) << "adv60[" << d << "," << n << "] mismatch";
      }
    }
  }

  // Incomplete-window cell: adv60 at date 3, instrument 0 -> NaN (3+1 < 60).
  EXPECT_TRUE(std::isnan(cell(aug, "adv60", 3, 0)))
      << "adv60[3,0] should be NaN (incomplete window)";
}

// ============================================================================
//  Test 6: DelegationIdentity
// ============================================================================

TEST(DelegationIdentity, ProductionMatchesTestHelper) {
  const Panel base = atx_impl_test::make_synth_orats_panel();
  const std::vector<atx::u16> windows = {5, 20};

  auto prod_r = atx::engine::alpha::with_alpha101_fields(base, windows);
  ASSERT_TRUE(prod_r.has_value()) << "production: " << prod_r.error().message();

  auto ref_r = atx_impl_test::augment_for_alpha101(base, windows);
  ASSERT_TRUE(ref_r.has_value()) << "reference: " << ref_r.error().message();

  const Panel &prod = prod_r.value();
  const Panel &ref = ref_r.value();

  ASSERT_EQ(prod.num_fields(), ref.num_fields())
      << "field count differs: prod=" << prod.num_fields() << " ref=" << ref.num_fields();

  // Same field-name list, same order.
  for (atx::usize f = 0; f < prod.num_fields(); ++f) {
    EXPECT_EQ(prod.field_name(f), ref.field_name(f))
        << "field name differs at index " << f;
  }

  // Every cell bitwise-equal (NaN==NaN by bit pattern).
  ASSERT_EQ(prod.dates(), ref.dates());
  ASSERT_EQ(prod.instruments(), ref.instruments());

  const atx::usize cells = prod.dates() * prod.instruments();
  const atx::usize I = prod.instruments();
  // Per-field cell comparison wrapped in an immediately-invoked lambda so the
  // first-mismatch report can `return` out of the inner loop (no goto: see
  // .agents/cpp/agent.md §3). Each field is compared independently.
  for (atx::usize f = 0; f < prod.num_fields(); ++f) {
    const std::span<const atx::f64> pc = prod.field_all(static_cast<FieldId>(f));
    const std::span<const atx::f64> rc = ref.field_all(static_cast<FieldId>(f));
    [&] {
      for (atx::usize i = 0; i < cells; ++i) {
        std::uint64_t pb{};
        std::uint64_t rb{};
        std::memcpy(&pb, &pc[i], sizeof(pb));
        std::memcpy(&rb, &rc[i], sizeof(rb));
        if (pb != rb) {
          // Report first mismatch with context, then fail fast for this field.
          const atx::usize d = i / I;
          const atx::usize n = i % I;
          ADD_FAILURE() << "field '" << prod.field_name(f) << "' differs at [" << d << "," << n
                        << "]: prod=" << pc[i] << " ref=" << rc[i];
          return; // skip remaining cells in this field after first mismatch
        }
      }
    }();
  }
}
