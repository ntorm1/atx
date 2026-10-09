// dispersion_realism_flags_test.cpp — Task E1 (backtest-lakehouse sprint).
//
// `dispersion_realism_flags.hpp` (examples/) is the shared friction/financing
// /policy CLI flag seam `mag7_dispersion_backtest.cpp` and
// `spy_dispersion_pnl.cpp` both include -- the fix for
// docs/reviews/2026-07-21-pipeline-sota-review/review-backtest.md:77
// ("wired by zero examples"). This is the unit-testable half of that fix
// (mirrors `surface_db_build_test.cpp`'s inclusion of
// `tools/surface_db_build_cli.hpp` for the same reason: the CLI binaries
// themselves are ATX_BUILD_EXAMPLES-gated and this suite tests the header's
// pure functions directly, not either binary).
//
// Coverage: PARSE (argv-shaped tokens -> `RealismArgs`) -> DUMP
// (`apply_realism_args` -> `RunConfig`) -> COMPARE (every field this header
// owns reaches the RunConfig it claims to, and nothing else moves), plus the
// enum-rejection and default-is-a-no-op halves of the contract.

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp"
#include "dispersion_realism_flags.hpp"

using namespace atx::vol;

namespace {

// Mirrors each CLI's own `parse_args` argv loop exactly enough to exercise
// `parse_realism_flag` the way `mag7_dispersion_backtest.cpp` /
// `spy_dispersion_pnl.cpp` actually call it: a mutable index into `argv`, a
// `nv()` that returns the NEXT token and advances the index, called at most
// once per flag.
[[nodiscard]] RealismArgs parse_all(const std::vector<std::string> &argv) {
  RealismArgs a;
  std::size_t i = 0;
  while (i < argv.size()) {
    const std::string_view arg = argv[i];
    const std::function<const char *()> nv = [&]() -> const char * {
      return (i + 1 < argv.size()) ? argv[++i].c_str() : "";
    };
    EXPECT_TRUE(parse_realism_flag(arg, nv, a)) << "unrecognized flag: " << arg;
    ++i;
  }
  return a;
}

} // namespace

// ── PARSE -> DUMP -> COMPARE: every flag this header owns reaches RunConfig ─
TEST(DispersionRealismFlags, EveryFlagReachesRunConfig) {
  const std::vector<std::string> argv = {
      "--unpriced",
      "error",
      "--exercise-policy",
      "simulate",
      "--margin-breach",
      "halt",
      "--friction-spread-kind",
      "quote_side",
      "--half-spread-bps",
      "7.5",
      "--vol-tick",
      "0.02",
      "--impact-fraction",
      "0.001",
      "--per-contract-cost",
      "0.65",
      "--hedge-slippage-bps",
      "3.0",
      "--crossing-fraction-single",
      "0.8",
      "--crossing-fraction-complex",
      "0.6",
      "--borrow-rate",
      "0.017",
      "--finance-premium",
      "--shares-carry",
      "--initial-cash",
      "100000",
      "--financing-reference-uid",
      "42",
      "--financing-flat-r",
      "0.043",
  };
  const RealismArgs a = parse_all(argv);

  RunConfig rc; // RunConfig{}'s own defaults, per the CLIs' own construction.
  std::string err;
  ASSERT_TRUE(apply_realism_args(a, rc, err)) << err;

  EXPECT_EQ(rc.unpriced, UnpricedLotPolicy::Error);
  EXPECT_EQ(rc.exercise_policy, ExercisePolicy::Simulate);
  EXPECT_EQ(rc.margin_breach, MarginBreachPolicy::Halt);

  EXPECT_EQ(rc.frictions.spread_kind, FrictionModel::SpreadKind::QuoteSide);
  EXPECT_DOUBLE_EQ(rc.frictions.half_spread_bps, 7.5);
  EXPECT_DOUBLE_EQ(rc.frictions.vol_tick, 0.02);
  EXPECT_DOUBLE_EQ(rc.frictions.impact_fraction, 0.001);
  EXPECT_DOUBLE_EQ(rc.frictions.per_contract_cost, 0.65);
  EXPECT_DOUBLE_EQ(rc.frictions.hedge_slippage_bps, 3.0);
  EXPECT_DOUBLE_EQ(rc.frictions.crossing_fraction_single, 0.8);
  EXPECT_DOUBLE_EQ(rc.frictions.crossing_fraction_complex, 0.6);

  EXPECT_DOUBLE_EQ(rc.financing.borrow_rate, 0.017);
  EXPECT_TRUE(rc.financing.finance_premium);
  EXPECT_TRUE(rc.financing.shares_carry);
  EXPECT_DOUBLE_EQ(rc.financing.initial_cash, 100000.0);
  EXPECT_EQ(rc.financing.reference_uid, 42u);
  ASSERT_TRUE(rc.financing.flat_r.has_value());
  EXPECT_DOUBLE_EQ(*rc.financing.flat_r, 0.043);
}

// ── No flags passed -> strict no-op (the determinism invariant both CLIs'
//    "default behavior with no new flags produces byte-identical artifacts"
//    claim rests on). ──────────────────────────────────────────────────────
TEST(DispersionRealismFlags, DefaultArgsAreANoOpOnRunConfig) {
  const RealismArgs a{}; // nothing set, matching argv with none of these flags.
  RunConfig before;
  before.frictions.half_spread_bps = 3.0; // an already-set field (e.g. --frictions preset)
  RunConfig after = before;
  std::string err;
  ASSERT_TRUE(apply_realism_args(a, after, err)) << err;

  EXPECT_EQ(after.unpriced, before.unpriced);
  EXPECT_EQ(after.exercise_policy, before.exercise_policy);
  EXPECT_EQ(after.margin_breach, before.margin_breach);
  EXPECT_EQ(after.frictions.spread_kind, before.frictions.spread_kind);
  EXPECT_DOUBLE_EQ(after.frictions.half_spread_bps, before.frictions.half_spread_bps);
  EXPECT_DOUBLE_EQ(after.frictions.hedge_slippage_bps, before.frictions.hedge_slippage_bps);
  EXPECT_DOUBLE_EQ(after.frictions.crossing_fraction_single, before.frictions.crossing_fraction_single);
  EXPECT_DOUBLE_EQ(after.frictions.crossing_fraction_complex, before.frictions.crossing_fraction_complex);
  EXPECT_FALSE(after.financing.finance_premium);
  EXPECT_FALSE(after.financing.shares_carry);
  EXPECT_EQ(after.financing.reference_uid, before.financing.reference_uid);
  EXPECT_EQ(after.financing.flat_r.has_value(), before.financing.flat_r.has_value());
}

// ── A --frictions-style preset applied first is refined, not replaced, by an
//    explicit fine-grained flag (the two CLIs' own "preset first, explicit
//    keys refine" call order). ───────────────────────────────────────────
TEST(DispersionRealismFlags, ExplicitFlagRefinesAPriorPreset) {
  RunConfig rc;
  // The --frictions shortcut both CLIs apply before this header's flags.
  rc.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  rc.frictions.half_spread_bps = 5.0;
  rc.frictions.per_contract_cost = 0.65;

  const RealismArgs a = parse_all({"--half-spread-bps", "9.0"});
  std::string err;
  ASSERT_TRUE(apply_realism_args(a, rc, err)) << err;

  EXPECT_EQ(rc.frictions.spread_kind, FrictionModel::SpreadKind::PriceBps); // untouched
  EXPECT_DOUBLE_EQ(rc.frictions.half_spread_bps, 9.0);                      // refined
  EXPECT_DOUBLE_EQ(rc.frictions.per_contract_cost, 0.65);                   // untouched
}

// ── Bad enum spellings are rejected (loud usage error), not silently ignored ─
TEST(DispersionRealismFlags, UnrecognizedEnumSpellingIsRejected) {
  RunConfig rc;
  std::string err;

  {
    const RealismArgs a = parse_all({"--unpriced", "bogus"});
    EXPECT_FALSE(apply_realism_args(a, rc, err));
    EXPECT_NE(err.find("--unpriced"), std::string::npos) << err;
  }
  {
    const RealismArgs a = parse_all({"--exercise-policy", "bogus"});
    EXPECT_FALSE(apply_realism_args(a, rc, err));
    EXPECT_NE(err.find("--exercise-policy"), std::string::npos) << err;
  }
  {
    const RealismArgs a = parse_all({"--margin-breach", "bogus"});
    EXPECT_FALSE(apply_realism_args(a, rc, err));
    EXPECT_NE(err.find("--margin-breach"), std::string::npos) << err;
  }
  {
    const RealismArgs a = parse_all({"--friction-spread-kind", "bogus"});
    EXPECT_FALSE(apply_realism_args(a, rc, err));
    EXPECT_NE(err.find("--friction-spread-kind"), std::string::npos) << err;
  }
}

// ── An unrecognized flag is not swallowed by this dispatcher -- it must fall
//    through so each CLI's own arg loop can print "unknown arg" and exit 2. ──
TEST(DispersionRealismFlags, UnknownFlagIsNotConsumed) {
  RealismArgs a;
  std::size_t i = 0;
  const std::vector<std::string> argv = {"--not-a-real-flag"};
  const std::function<const char *()> nv = [&]() -> const char * {
    return (i + 1 < argv.size()) ? argv[++i].c_str() : "";
  };
  EXPECT_FALSE(parse_realism_flag(argv[i], nv, a));
}

// ── FrictionRegime -> string, printed into every emitted artifact's meta
//    block and console summary by both CLIs. ────────────────────────────
TEST(DispersionRealismFlags, FrictionRegimeToStringCoversEveryEnumerator) {
  EXPECT_EQ(to_string(FrictionRegime::Frictionless), "frictionless");
  EXPECT_EQ(to_string(FrictionRegime::Modeled), "modeled");
  EXPECT_EQ(to_string(FrictionRegime::QuoteSide), "quote_side");
}
