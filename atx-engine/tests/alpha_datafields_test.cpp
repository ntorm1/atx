// atx::engine::alpha — derived datafields (S3.3): vwap / dollar_volume / adv{d}.
//
// Datafields are PANEL INPUTS, not opcodes: with_datafields() derives them once
// at construction so a formula loads `adv20` like `close`. This suite proves:
//   * dollar_volume = close·volume and vwap = (high+low+close)/3 (typical proxy),
//   * adv{d} equals ts_mean(dollar_volume, d) BIT-FOR-BIT through the engine
//     (the conformance that lets a formula reference adv{d} as a column),
//   * the PIT/universe discipline (out-of-universe -> NaN, never imputed),
//   * a supplied `vwap` column overrides the proxy, and adv name parsing.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_datafields_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
namespace df = atx::engine::alpha::datafields;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Compile + run one alpha through the VM, returning its date-major values.
[[nodiscard]] std::vector<atx::f64> vm_values(std::string_view expr, const Panel &panel) {
  auto ast = parse_expr(expr, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  Engine engine{panel};
  auto out = engine.evaluate(prog.value_or(Program{}));
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

// Read a derived field column straight off the Panel (length == cells).
[[nodiscard]] std::vector<atx::f64> column(const Panel &p, std::string_view name) {
  auto id = p.field_id(name);
  EXPECT_TRUE(id.has_value()) << (id ? "" : id.error().message());
  const std::span<const atx::f64> col = p.field_all(id.value_or(0));
  return std::vector<atx::f64>(col.begin(), col.end());
}

// A small deterministic OHLCV panel (so dollar_volume/vwap are hand-checkable).
[[nodiscard]] std::vector<std::vector<atx::f64>> ohlcv(atx::usize dates, atx::usize instruments) {
  const atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols(4, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    const auto fi = static_cast<atx::f64>(i);
    cols[0][i] = 10.0 + fi;       // close
    cols[1][i] = 100.0 + 2.0 * fi; // volume
    cols[2][i] = 12.0 + fi;       // high
    cols[3][i] = 8.0 + fi;        // low
  }
  return cols;
}

[[nodiscard]] Panel build(atx::usize dates, atx::usize instruments,
                          std::vector<std::vector<atx::f64>> cols,
                          std::vector<std::uint8_t> universe, std::vector<atx::u16> adv) {
  std::vector<std::string> names = {"close", "volume", "high", "low"};
  auto p = df::with_datafields(dates, instruments, std::move(names), std::move(cols),
                               std::move(universe), adv);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// ===========================================================================
//  parse_adv_field — "adv{d}" name -> window.
// ===========================================================================

TEST(ParseAdvField, RecognizesPositiveWindows) {
  atx::u16 w = 0;
  EXPECT_TRUE(df::parse_adv_field("adv20", w));
  EXPECT_EQ(w, 20);
  EXPECT_TRUE(df::parse_adv_field("adv1", w));
  EXPECT_EQ(w, 1);
  EXPECT_TRUE(df::parse_adv_field("adv65535", w));
  EXPECT_EQ(w, 65535);
}

TEST(ParseAdvField, RejectsNonAdvAndDegenerate) {
  atx::u16 w = 7;
  EXPECT_FALSE(df::parse_adv_field("adv", w));       // no digits
  EXPECT_FALSE(df::parse_adv_field("advx", w));      // non-numeric suffix
  EXPECT_FALSE(df::parse_adv_field("adv0", w));      // zero window
  EXPECT_FALSE(df::parse_adv_field("adv20x", w));    // trailing junk
  EXPECT_FALSE(df::parse_adv_field("close", w));     // unrelated field
  EXPECT_FALSE(df::parse_adv_field("adv65536", w));  // overflows u16
  EXPECT_EQ(w, 7);                                   // unchanged on failure
}

// ===========================================================================
//  dollar_volume / vwap derivation.
// ===========================================================================

TEST(Datafields, DollarVolumeAndVwap_HandComputed) {
  const atx::usize dates = 3;
  const atx::usize instruments = 2;
  const Panel p = build(dates, instruments, ohlcv(dates, instruments), {}, {});
  const std::vector<atx::f64> dvol = column(p, "dollar_volume");
  const std::vector<atx::f64> vwap = column(p, "vwap");
  ASSERT_EQ(dvol.size(), dates * instruments);
  for (atx::usize i = 0; i < dvol.size(); ++i) {
    const auto fi = static_cast<atx::f64>(i);
    const atx::f64 close = 10.0 + fi;
    const atx::f64 volume = 100.0 + 2.0 * fi;
    const atx::f64 high = 12.0 + fi;
    const atx::f64 low = 8.0 + fi;
    EXPECT_DOUBLE_EQ(dvol[i], close * volume) << "cell " << i;
    EXPECT_DOUBLE_EQ(vwap[i], (high + low + close) / 3.0) << "cell " << i;
  }
}

// A caller-supplied `vwap` column is kept verbatim (no proxy override).
TEST(Datafields, SuppliedVwap_OverridesProxy) {
  const atx::usize dates = 2;
  const atx::usize instruments = 2;
  const atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols = ohlcv(dates, instruments);
  std::vector<atx::f64> true_vwap(cells);
  for (atx::usize i = 0; i < cells; ++i) {
    true_vwap[i] = 99.0 + static_cast<atx::f64>(i);
  }
  cols.push_back(true_vwap);
  std::vector<std::string> names = {"close", "volume", "high", "low", "vwap"};
  auto p = df::with_datafields(dates, instruments, std::move(names), std::move(cols), {}, {});
  ASSERT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  const std::vector<atx::f64> vwap = column(p.value(), "vwap");
  for (atx::usize i = 0; i < cells; ++i) {
    EXPECT_DOUBLE_EQ(vwap[i], 99.0 + static_cast<atx::f64>(i)) << "cell " << i;
  }
}

// ===========================================================================
//  adv{d} == ts_mean(dollar_volume, d) bit-for-bit (the load-bearing identity).
// ===========================================================================

TEST(Datafields, Adv_EqualsTsMeanOfDollarVolume) {
  const atx::usize dates = 8;
  const atx::usize instruments = 3;
  const Panel p = build(dates, instruments, ohlcv(dates, instruments), {}, {3, 5});
  for (const std::string &adv : {std::string{"adv3"}, std::string{"adv5"}}) {
    const std::vector<atx::f64> stored = column(p, adv);
    const std::string window = adv.substr(3);
    const std::vector<atx::f64> via_ts =
        vm_values("ts_mean(dollar_volume, " + window + ")", p);
    ASSERT_EQ(stored.size(), via_ts.size()) << adv;
    for (atx::usize i = 0; i < stored.size(); ++i) {
      EXPECT_TRUE(same_cell(stored[i], via_ts[i]))
          << adv << " cell " << i << ": stored=" << stored[i] << " ts_mean=" << via_ts[i];
    }
  }
}

// ===========================================================================
//  PIT / universe — out-of-universe cells are NaN, never imputed.
// ===========================================================================

TEST(Datafields, OutOfUniverse_DerivedCellsAreNaN) {
  const atx::usize dates = 4;
  const atx::usize instruments = 2;
  const atx::usize cells = dates * instruments;
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  universe[2 * instruments + 0] = 0; // (date 2, inst 0) out of universe
  const Panel p = build(dates, instruments, ohlcv(dates, instruments), universe, {3});
  const std::vector<atx::f64> dvol = column(p, "dollar_volume");
  const std::vector<atx::f64> vwap = column(p, "vwap");
  EXPECT_TRUE(std::isnan(dvol[2 * instruments + 0]));
  EXPECT_TRUE(std::isnan(vwap[2 * instruments + 0]));
  // adv3 for inst 0 needs dates {1,2,3}; date 2 is NaN, so adv3 at date 3 is NaN.
  const std::vector<atx::f64> adv3 = column(p, "adv3");
  EXPECT_TRUE(std::isnan(adv3[3 * instruments + 0]));
  // inst 1 is fully in-universe; adv3 at date 3 is the mean of its 3 dvols.
  EXPECT_FALSE(std::isnan(adv3[3 * instruments + 1]));
}

// ===========================================================================
//  Error — a missing required base field is reported, not silently skipped.
// ===========================================================================

TEST(Datafields, MissingVolume_IsError) {
  const atx::usize dates = 2;
  const atx::usize instruments = 2;
  const atx::usize cells = dates * instruments;
  std::vector<std::string> names = {"close", "high", "low"}; // no volume
  std::vector<std::vector<atx::f64>> cols(3, std::vector<atx::f64>(cells, 1.0));
  auto p = df::with_datafields(dates, instruments, std::move(names), std::move(cols), {}, {});
  EXPECT_FALSE(p.has_value());
}


}  // namespace atxtest_alpha_datafields_test
