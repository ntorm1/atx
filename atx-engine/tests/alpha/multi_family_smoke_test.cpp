// atx::engine::alpha — multi-family augmentation smoke test (p7 S2-5).
//
// Suite: MultiFamilySmoke
//
// Integration gate for the full augmentation chain
//   with_alpha101_fields(base, {20}) -> with_iv_fields -> with_liquidity_fields
// on a tiny deterministic synthetic panel. Proves the VM can actually evaluate
// at least one representative seed from each price-derived family end-to-end
// (catching wiring mismatches — wrong field name / type / dimension — that the
// per-entry-point unit tests cannot). It is NOT a discover or dev-panel run.
//
// The short-interest family (si_dtc/si_util/si_chg) is NOT smoked here: it needs
// real/synthetic parquet on disk and is covered by Augment.* (S2-1); parse +
// typecheck coverage from S2-4 is sufficient for the catalog integration gate.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/augment.hpp"
#include "atx/engine/alpha/bytecode.hpp" // compile, Program
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"    // parse_expr, Library
#include "atx/engine/alpha/typecheck.hpp" // analyze
#include "atx/engine/alpha/vm.hpp"        // Engine, SignalSet

namespace atxtest_multi_family_smoke {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::with_alpha101_fields;
using atx::engine::alpha::with_iv_fields;
using atx::engine::alpha::with_liquidity_fields;

namespace {

// 25 dates (not the plan's nominal 15): adv20 = ts_mean(dollar_volume, 20)
// requires a FULL 20-date trailing window, so a 15-date panel leaves adv20 — and
// therefore illiq, which derives from it — all-NaN, and the LiquidityFamily smoke
// (>= 1 finite illiq cell) cannot pass. 25 dates gives adv20 finite cells from
// date 19 on while keeping the panel tiny. (5 instruments as the plan specifies.)
constexpr atx::usize kD = 25;
constexpr atx::usize kN = 5;

// Base OHLCV+meta panel carrying every input the chain needs:
//   with_alpha101_fields: close (+ open/high/low/volume/market_cap/sector)
//   with_iv_fields:       atmCenI_21d, atmCenI_126d, nEarnCnt_5d (+ returns added)
//   with_liquidity_fields: adv20 (added by alpha101) + sector
// Values are deterministic and non-degenerate so each cross-section has variance.
[[nodiscard]] Panel make_base_panel() {
  const atx::usize cells = kD * kN;
  std::vector<atx::f64> open(cells);
  std::vector<atx::f64> high(cells);
  std::vector<atx::f64> low(cells);
  std::vector<atx::f64> close(cells);
  std::vector<atx::f64> volume(cells);
  std::vector<atx::f64> market_cap(cells);
  std::vector<atx::f64> sector(cells);
  std::vector<atx::f64> iv21(cells);
  std::vector<atx::f64> iv126(cells);
  std::vector<atx::f64> nearn(cells);
  for (atx::usize d = 0; d < kD; ++d) {
    for (atx::usize n = 0; n < kN; ++n) {
      const atx::usize i = d * kN + n;
      const atx::f64 c = 10.0 + 0.1 * static_cast<atx::f64>(d) + 0.5 * static_cast<atx::f64>(n);
      close[i] = c;
      open[i] = c * 0.995;
      high[i] = c * 1.01;
      low[i] = c * 0.99;
      volume[i] = 1.0e6 * (1.0 + static_cast<atx::f64>(n)) + 1.0e3 * static_cast<atx::f64>(d);
      market_cap[i] = 1.0e9 * (1.0 + static_cast<atx::f64>(n));
      sector[i] = (n < kN / 2) ? 0.0 : 1.0;
      iv21[i] = 0.05 + 0.01 * static_cast<atx::f64>(n) + 0.001 * static_cast<atx::f64>(d);
      iv126[i] = 0.07 + 0.005 * static_cast<atx::f64>(n) + 0.0005 * static_cast<atx::f64>(d);
      nearn[i] = static_cast<atx::f64>(i % 3);
    }
  }
  std::vector<std::string> names = {"open",       "high",        "low",
                                    "close",      "volume",      "market_cap",
                                    "sector",     "atmCenI_21d", "atmCenI_126d",
                                    "nEarnCnt_5d"};
  std::vector<std::vector<atx::f64>> data = {open,    high,  low,  close, volume,
                                            market_cap, sector, iv21, iv126, nearn};
  auto r = Panel::create(kD, kN, std::move(names), std::move(data), {});
  return std::move(r).value();
}

// Fully augmented panel: alpha101({20}) -> iv -> liquidity. The inputs are
// known-valid, so each step's Result must hold; .value() aborts loudly on a
// contract break (Panel has no public default ctor to fall back to). Errors are
// surfaced via the message before the abort.
[[nodiscard]] Panel make_augmented_panel() {
  const Panel base = make_base_panel();
  const std::vector<atx::u16> windows = {20};
  auto a = with_alpha101_fields(base, windows);
  EXPECT_TRUE(a.has_value()) << "with_alpha101_fields: " << (a ? "" : a.error().message());
  auto iv = with_iv_fields(a.value());
  EXPECT_TRUE(iv.has_value()) << "with_iv_fields: " << (iv ? "" : iv.error().message());
  auto liq = with_liquidity_fields(iv.value());
  EXPECT_TRUE(liq.has_value()) << "with_liquidity_fields: " << (liq ? "" : liq.error().message());
  return std::move(liq).value();
}

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Evaluate one alpha expr over `panel`; returns the date-major values (empty on
// any error, with a gtest failure recorded so the caller's ASSERTs fire).
[[nodiscard]] std::vector<atx::f64> eval_expr(std::string_view expr, const Panel &panel) {
  auto ast = parse_expr(expr, shared_lib());
  EXPECT_TRUE(ast.has_value()) << "parse: " << (ast ? "" : ast.error().message());
  if (!ast.has_value()) {
    return {};
  }
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << "analyze: " << (ana ? "" : ana.error().message());
  if (!ana.has_value()) {
    return {};
  }
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << "compile: " << (prog ? "" : prog.error().message());
  if (!prog.has_value()) {
    return {};
  }
  Engine engine{panel};
  auto out = engine.evaluate(prog.value());
  EXPECT_TRUE(out.has_value()) << "evaluate: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

[[nodiscard]] bool any_finite(const std::vector<atx::f64> &v) noexcept {
  for (const atx::f64 x : v) {
    if (std::isfinite(x)) {
      return true;
    }
  }
  return false;
}

// Order-sensitive bitwise digest over a panel's field names + cell bit patterns
// (NaN-safe: hashes the raw bits). FNV-1a over a deterministic byte stream — two
// byte-identical panels hash equal; any field/value drift changes the digest.
[[nodiscard]] atx::u64 panel_digest(const Panel &p) {
  atx::u64 h = 1469598103934665603ULL; // FNV offset basis
  auto mix = [&h](const void *data, atx::usize len) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (atx::usize i = 0; i < len; ++i) {
      h ^= bytes[i];
      h *= 1099511628211ULL; // FNV prime
    }
  };
  const atx::usize nf = p.num_fields();
  mix(&nf, sizeof(nf));
  for (atx::usize f = 0; f < nf; ++f) {
    const std::string_view nm = p.field_name(f);
    mix(nm.data(), nm.size());
    const std::span<const atx::f64> col = p.field_all(static_cast<atx::engine::alpha::FieldId>(f));
    for (const atx::f64 v : col) {
      atx::u64 bits = 0;
      std::memcpy(&bits, &v, sizeof(bits));
      mix(&bits, sizeof(bits));
    }
  }
  return h;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. price/returns family seed evaluates and yields >= 1 finite cell.
// ---------------------------------------------------------------------------
TEST(MultiFamilySmoke, PriceReturnsFamily) {
  const Panel aug = make_augmented_panel();
  const std::vector<atx::f64> out =
      eval_expr("group_neutralize(rank(ts_mean(returns, 5)), sector)", aug);
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(any_finite(out)) << "price/returns seed produced no finite cell";
}

// ---------------------------------------------------------------------------
// 2. IV-surface family seed evaluates and yields >= 1 finite cell.
// ---------------------------------------------------------------------------
TEST(MultiFamilySmoke, IvSurfaceFamily) {
  const Panel aug = make_augmented_panel();
  const std::vector<atx::f64> out = eval_expr("group_neutralize(rank(iv_term), sector)", aug);
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(any_finite(out)) << "iv-surface seed produced no finite cell";
}

// ---------------------------------------------------------------------------
// 3. liquidity family seed evaluates and yields >= 1 finite cell.
// ---------------------------------------------------------------------------
TEST(MultiFamilySmoke, LiquidityFamily) {
  const Panel aug = make_augmented_panel();
  const std::vector<atx::f64> out = eval_expr("group_neutralize(rank(illiq), sector)", aug);
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(any_finite(out)) << "liquidity seed produced no finite cell";
}

// ---------------------------------------------------------------------------
// 4. The fully augmented panel carries the expected field set (exact count).
// ---------------------------------------------------------------------------
TEST(MultiFamilySmoke, AugmentedPanelFieldCount) {
  const Panel base = make_base_panel();
  const Panel aug = make_augmented_panel();

  // with_alpha101_fields({20}) appends 8 (returns, cap, IndClass.sector/.industry/
  // .subindustry, dollar_volume, vwap, adv20); with_iv_fields appends 3 (iv_term,
  // iv_vrp, iv_lo); with_liquidity_fields appends 1 (illiq).
  const atx::usize expected = base.num_fields() + 8 + 3 + 1;
  EXPECT_EQ(aug.num_fields(), expected);
  EXPECT_GE(aug.num_fields(), 15u);

  // Every expected derived name is present exactly once.
  for (std::string_view nm : {"returns", "cap", "IndClass.sector", "IndClass.industry",
                              "IndClass.subindustry", "dollar_volume", "vwap", "adv20", "iv_term",
                              "iv_vrp", "iv_lo", "illiq"}) {
    atx::usize cnt = 0;
    for (atx::usize f = 0; f < aug.num_fields(); ++f) {
      if (aug.field_name(f) == nm) {
        ++cnt;
      }
    }
    EXPECT_EQ(cnt, 1u) << "field " << nm << " present " << cnt << " times (expected 1)";
  }
}

// ---------------------------------------------------------------------------
// 5. Off-path opt-in contract: a panel built by ONLY with_alpha101_fields(base,
//    {20}) (no IV, no liquidity) digests identically on two consecutive calls,
//    and never carries the opt-in iv/liquidity columns.
// ---------------------------------------------------------------------------
TEST(MultiFamilySmoke, OffPathDigestUnchanged) {
  const Panel base = make_base_panel();
  const std::vector<atx::u16> windows = {20};

  auto a1 = with_alpha101_fields(base, windows);
  ASSERT_TRUE(a1.has_value()) << a1.error().message();
  auto a2 = with_alpha101_fields(base, windows);
  ASSERT_TRUE(a2.has_value()) << a2.error().message();

  EXPECT_EQ(panel_digest(a1.value()), panel_digest(a2.value()))
      << "with_alpha101_fields is not deterministic across two calls";

  // The off-path panel must NOT carry any opt-in IV / liquidity column.
  for (std::string_view nm : {"iv_term", "iv_vrp", "iv_lo", "illiq"}) {
    for (atx::usize f = 0; f < a1.value().num_fields(); ++f) {
      EXPECT_NE(a1.value().field_name(f), nm)
          << "off-path panel unexpectedly carries opt-in column " << nm;
    }
  }
}

} // namespace atxtest_multi_family_smoke
