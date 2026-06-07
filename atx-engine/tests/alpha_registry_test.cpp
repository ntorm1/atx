// atx::engine::alpha — operator registry unit tests (P3-2 / P3b-1).
//
// Covers the Library catalogue and the table-driven shape signatures:
//   * built-ins registered (find returns a row; unknown → nullptr);
//   * name → opcode / arity / out_dtype mapping (Appendix A);
//   * all built-ins lookahead_safe;
//   * shape_of broadcast rules per op family (§4);
//   * register_op: success, duplicate, empty-name, null-shape rejection.
//   * P3b-1: arity range [min_arity, max_arity] (fixed ops min==max; scale 1..2).
//
// Naming: Subject_Condition_ExpectedResult.

#include <array>
#include <cmath>
#include <span>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace {

using atx::core::ErrorCode;
using atx::engine::alpha::DType;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::Shape;

// ---- catalogue presence -----------------------------------------------------

TEST(AlphaRegistry_Find, KnownBuiltin_ReturnsRow) {
  const Library lib;
  const OpSig *sig = lib.find("ts_mean");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->name, "ts_mean");
  EXPECT_EQ(sig->min_arity, 2U);
  EXPECT_EQ(sig->max_arity, 2U);
  EXPECT_EQ(sig->opcode, OpCode::TsMean);
}

TEST(AlphaRegistry_Find, UnknownName_ReturnsNullptr) {
  const Library lib;
  EXPECT_EQ(lib.find("no_such_op"), nullptr);
}

TEST(AlphaRegistry_Find, EmptyName_ReturnsNullptr) {
  const Library lib;
  EXPECT_EQ(lib.find(""), nullptr);
}

// ---- name → opcode mapping (Appendix A spot checks) -------------------------

TEST(AlphaRegistry_Mapping, Correlation_MapsToTsCorr_Arity3) {
  const Library lib;
  const OpSig *sig = lib.find("correlation");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::TsCorr);
  EXPECT_EQ(sig->min_arity, 3U);
  EXPECT_EQ(sig->max_arity, 3U);
}

TEST(AlphaRegistry_Mapping, Stddev_MapsToTsStd) {
  const Library lib;
  const OpSig *sig = lib.find("stddev");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::TsStd);
}

TEST(AlphaRegistry_Mapping, SignedPower_MapsToSpow) {
  const Library lib;
  const OpSig *sig = lib.find("signedpower");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::Spow);
  EXPECT_EQ(sig->min_arity, 2U);
  EXPECT_EQ(sig->max_arity, 2U);
}

TEST(AlphaRegistry_Mapping, IndNeutralize_MapsToCsDemeanG) {
  const Library lib;
  const OpSig *sig = lib.find("indneutralize");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::CsDemeanG);
}

TEST(AlphaRegistry_Mapping, GroupNeutralize_MapsToCsNeutG) {
  const Library lib;
  const OpSig *sig = lib.find("group_neutralize");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::CsNeutG);
}

TEST(AlphaRegistry_Mapping, Rank_MapsToCsRank_Arity1) {
  const Library lib;
  const OpSig *sig = lib.find("rank");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::CsRank);
  EXPECT_EQ(sig->min_arity, 1U);
  EXPECT_EQ(sig->max_arity, 1U);
}

// ---- out_dtype + lookahead rail ---------------------------------------------

TEST(AlphaRegistry_Dtype, AbsFunction_OutDtypeIsF64) {
  const Library lib;
  const OpSig *sig = lib.find("abs");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->out_dtype, DType::F64);
}

TEST(AlphaRegistry_Lookahead, AllBuiltins_AreLookaheadSafe) {
  const Library lib;
  static constexpr std::array<const char *, 6> kNames = {"rank",  "ts_mean",     "correlation",
                                                         "delay", "signedpower", "decay_linear"};
  for (const char *name : kNames) {
    const OpSig *sig = lib.find(name);
    ASSERT_NE(sig, nullptr) << name;
    EXPECT_TRUE(sig->lookahead_safe) << name;
  }
}

// ---- shape_of broadcast rules (§4) ------------------------------------------

TEST(AlphaRegistry_Shape, MinFunction_BroadcastsToMaxShape) {
  const Library lib;
  const OpSig *sig = lib.find("min");
  ASSERT_NE(sig, nullptr);
  const std::array<Shape, 2> panel_scalar = {Shape::Panel, Shape::Scalar};
  EXPECT_EQ(sig->shape_of(panel_scalar), Shape::Panel);
  const std::array<Shape, 2> scalar_scalar = {Shape::Scalar, Shape::Scalar};
  EXPECT_EQ(sig->shape_of(scalar_scalar), Shape::Scalar);
}

TEST(AlphaRegistry_Shape, AbsFunction_PreservesArgShape) {
  const Library lib;
  const OpSig *sig = lib.find("abs");
  ASSERT_NE(sig, nullptr);
  const std::array<Shape, 1> panel = {Shape::Panel};
  EXPECT_EQ(sig->shape_of(panel), Shape::Panel);
  const std::array<Shape, 1> cs = {Shape::CrossSection};
  EXPECT_EQ(sig->shape_of(cs), Shape::CrossSection);
}

TEST(AlphaRegistry_Shape, Rank_AlwaysCrossSection) {
  const Library lib;
  const OpSig *sig = lib.find("rank");
  ASSERT_NE(sig, nullptr);
  const std::array<Shape, 1> panel = {Shape::Panel};
  EXPECT_EQ(sig->shape_of(panel), Shape::CrossSection);
}

TEST(AlphaRegistry_Shape, TsMean_AlwaysPanel) {
  const Library lib;
  const OpSig *sig = lib.find("ts_mean");
  ASSERT_NE(sig, nullptr);
  const std::array<Shape, 2> panel_scalar = {Shape::Panel, Shape::Scalar};
  EXPECT_EQ(sig->shape_of(panel_scalar), Shape::Panel);
}

// ---- register_op ------------------------------------------------------------

TEST(AlphaRegistry_Register, NewOp_Succeeds) {
  Library lib;
  const OpSig sig{"my_op",    1,    1,  OpCode::Abs,
                  DType::F64, true, {}, &atx::engine::alpha::shape_unary};
  const auto status = lib.register_op(sig);
  ASSERT_TRUE(status.has_value()) << (status ? "" : status.error().message());
  EXPECT_NE(lib.find("my_op"), nullptr);
}

TEST(AlphaRegistry_Register, DuplicateName_Fails) {
  Library lib;
  const auto status = lib.register_op(OpSig{
      "ts_mean", 2, 2, OpCode::TsMean, DType::F64, true, {}, &atx::engine::alpha::shape_panel});
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::AlreadyExists);
}

TEST(AlphaRegistry_Register, EmptyName_Fails) {
  Library lib;
  const auto status = lib.register_op(
      OpSig{"", 1, 1, OpCode::Abs, DType::F64, true, {}, &atx::engine::alpha::shape_unary});
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

TEST(AlphaRegistry_Register, NullShapeOf_Fails) {
  Library lib;
  const auto status =
      lib.register_op(OpSig{"bad_op", 1, 1, OpCode::Abs, DType::F64, true, {}, nullptr});
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

// ---- P3b-1: arity range [min_arity, max_arity] ------------------------------

TEST(AlphaRegistry_Arity, FixedOp_MinEqualsMax) {
  const Library lib;
  const OpSig *sig = lib.find("abs");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->min_arity, sig->max_arity);
  EXPECT_EQ(sig->min_arity, 1U);
}

TEST(AlphaRegistry_Arity, Scale_HasOptionalTrailingArg) {
  // scale is the proof of finite-default fill: 1 required + 1 optional (default 1.0).
  const Library lib;
  const OpSig *sig = lib.find("scale");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->opcode, OpCode::CsScale);
  EXPECT_EQ(sig->min_arity, 1U);
  EXPECT_EQ(sig->max_arity, 2U);
  // defaults[0] supplies argument (min_arity + 0) == arg index 1 when omitted.
  EXPECT_DOUBLE_EQ(sig->defaults[0], 1.0);
}

TEST(AlphaRegistry_Arity, GroupNeutralize_StaysFixedArity2) {
  // P3b-1 pins group_neutralize fixed at 2 (optional cap deferred to P3b-4).
  const Library lib;
  const OpSig *sig = lib.find("group_neutralize");
  ASSERT_NE(sig, nullptr);
  EXPECT_EQ(sig->min_arity, 2U);
  EXPECT_EQ(sig->max_arity, 2U);
}

TEST(AlphaRegistry_Register, VariadicWithFiniteDefault_Succeeds) {
  Library lib;
  const OpSig sig{"opt_op",   1,    2,     OpCode::CsScale,
                  DType::F64, true, {2.5}, &atx::engine::alpha::shape_cross_section};
  const auto status = lib.register_op(sig);
  ASSERT_TRUE(status.has_value()) << (status ? "" : status.error().message());
  const OpSig *found = lib.find("opt_op");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->min_arity, 1U);
  EXPECT_EQ(found->max_arity, 2U);
  EXPECT_DOUBLE_EQ(found->defaults[0], 2.5);
}

TEST(AlphaRegistry_Register, OptionalCountExceedsDefaultsCapacity_Fails) {
  // (max_arity - min_arity) > kMaxDefaults would index OpSig::defaults out of
  // bounds during parser default-fill — register_op must reject at the boundary.
  Library lib;
  const OpSig sig{"too_many_opts", 1,    5,     OpCode::CsScale,
                  DType::F64,      true, {1.0}, &atx::engine::alpha::shape_cross_section};
  const auto status = lib.register_op(sig);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

TEST(AlphaRegistry_Register, MaxArityBelowMinArity_Fails) {
  // An inverted arity range is malformed; reject it at the boundary.
  Library lib;
  const OpSig sig{"inverted", 3,    2,  OpCode::CsScale,
                  DType::F64, true, {}, &atx::engine::alpha::shape_cross_section};
  const auto status = lib.register_op(sig);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

// ---- B3: PinSig + OpSig.pins + split2 builtin -------------------------------

TEST(AlphaRegistry, Split2IsRecordWithTwoPins) {
  Library lib;
  const OpSig *s = lib.find("split2");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->min_arity, 1u);
  ASSERT_EQ(s->pins.size(), 2u);
  EXPECT_EQ(s->pins[0].name, "hi");
  EXPECT_EQ(s->pins[1].name, "lo");
}

} // namespace
