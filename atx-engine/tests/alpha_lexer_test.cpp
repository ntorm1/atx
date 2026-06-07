// atx::engine::alpha — Lexer unit tests (P3-1).
//
// Covers the plan's test list verbatim:
//   * each token kind produced correctly;
//   * multi-char vs single-char operators (</<= , !=/!, &&, ||);
//   * number forms: integer, leading-zero decimal, scientific notation;
//   * $close sigil → Dollar token + separate Ident token;
//   * span correctness — token text == src.substr(begin, end-begin);
//   * IndClass.sector → single Ident token (interior dot allowed);
//   * unknown byte → ParseError with correct offset in message;
//   * empty source → exactly one End token;
//   * trailing whitespace → just the preceding tokens + End;
//   * adjacent operators (a<=-b) lexed correctly.
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/lexer.hpp"

namespace {

using atx::core::ErrorCode;
using atx::engine::alpha::lex;
using atx::engine::alpha::Token;
using atx::engine::alpha::TokenKind;

// ---- helpers ----------------------------------------------------------------

// Lex and assert success; return the token vector.
[[nodiscard]] std::vector<Token> lex_ok(std::string_view src) {
  auto result = lex(src);
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message());
  return result.value_or(std::vector<Token>{});
}

// Assert that a token matches kind and that its span text equals expected_text.
void check_token(std::string_view src, const Token &tok, TokenKind kind,
                 std::string_view expected_text) {
  EXPECT_EQ(tok.kind, kind);
  const std::string_view actual_text = src.substr(tok.span.begin, tok.span.end - tok.span.begin);
  EXPECT_EQ(actual_text, expected_text);
}

// ---- each token kind --------------------------------------------------------

TEST(AlphaLexer_SingleToken, Number_ProducesNumberToken) {
  const auto toks = lex_ok("42");
  ASSERT_EQ(toks.size(), 2U); // Number + End
  EXPECT_EQ(toks[0].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[0].number, 42.0);
  EXPECT_EQ(toks[1].kind, TokenKind::End);
}

TEST(AlphaLexer_SingleToken, Ident_ProducesIdentToken) {
  const auto toks = lex_ok("close");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks[1].kind, TokenKind::End);
}

TEST(AlphaLexer_SingleToken, Dollar_ProducesDollarToken) {
  const auto toks = lex_ok("$");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Dollar);
  EXPECT_EQ(toks[1].kind, TokenKind::End);
}

TEST(AlphaLexer_SingleToken, Plus_ProducesPlusToken) {
  const auto toks = lex_ok("+");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Plus);
}

TEST(AlphaLexer_SingleToken, Minus_ProducesMinusToken) {
  const auto toks = lex_ok("-");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Minus);
}

TEST(AlphaLexer_SingleToken, Star_ProducesStarToken) {
  const auto toks = lex_ok("*");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Star);
}

TEST(AlphaLexer_SingleToken, Slash_ProducesSlashToken) {
  const auto toks = lex_ok("/");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Slash);
}

TEST(AlphaLexer_SingleToken, Caret_ProducesCaretToken) {
  const auto toks = lex_ok("^");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Caret);
}

TEST(AlphaLexer_SingleToken, Question_ProducesQuestionToken) {
  const auto toks = lex_ok("?");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Question);
}

TEST(AlphaLexer_SingleToken, Colon_ProducesColonToken) {
  const auto toks = lex_ok(":");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Colon);
}

TEST(AlphaLexer_SingleToken, LParen_ProducesLParenToken) {
  const auto toks = lex_ok("(");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::LParen);
}

TEST(AlphaLexer_SingleToken, RParen_ProducesRParenToken) {
  const auto toks = lex_ok(")");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::RParen);
}

TEST(AlphaLexer_SingleToken, Comma_ProducesCommaToken) {
  const auto toks = lex_ok(",");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Comma);
}

TEST(AlphaLexer_SingleToken, Assign_ProducesAssignToken) {
  const auto toks = lex_ok("=");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Assign);
}

TEST(AlphaLexer_SingleToken, Lt_ProducesLtToken) {
  const auto toks = lex_ok("<");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Lt);
}

TEST(AlphaLexer_SingleToken, Gt_ProducesGtToken) {
  const auto toks = lex_ok(">");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Gt);
}

TEST(AlphaLexer_SingleToken, Bang_ProducesBangToken) {
  const auto toks = lex_ok("!");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Bang);
}

// ---- multi-char operators vs single-char ------------------------------------

TEST(AlphaLexer_MultiCharOp, Le_LessThanEqual_ProducesLeToken) {
  const auto toks = lex_ok("<=");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Le);
}

TEST(AlphaLexer_MultiCharOp, Lt_Alone_ProducesLtNotLe) {
  const auto toks = lex_ok("< ");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Lt);
}

TEST(AlphaLexer_MultiCharOp, Ge_GreaterThanEqual_ProducesGeToken) {
  const auto toks = lex_ok(">=");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ge);
}

TEST(AlphaLexer_MultiCharOp, Gt_Alone_ProducesGtNotGe) {
  const auto toks = lex_ok("> ");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Gt);
}

TEST(AlphaLexer_MultiCharOp, EqEq_DoubleEqual_ProducesEqEqToken) {
  const auto toks = lex_ok("==");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::EqEq);
}

TEST(AlphaLexer_MultiCharOp, Assign_SingleEqual_ProducesAssignNotEqEq) {
  const auto toks = lex_ok("= ");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Assign);
}

TEST(AlphaLexer_MultiCharOp, BangEq_NotEqual_ProducesBangEqToken) {
  const auto toks = lex_ok("!=");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::BangEq);
}

TEST(AlphaLexer_MultiCharOp, Bang_Alone_ProducesBangNotBangEq) {
  const auto toks = lex_ok("! ");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Bang);
}

TEST(AlphaLexer_MultiCharOp, AmpAmp_LogicalAnd_ProducesAmpAmpToken) {
  const auto toks = lex_ok("&&");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::AmpAmp);
}

TEST(AlphaLexer_MultiCharOp, PipePipe_LogicalOr_ProducesPipePipeToken) {
  const auto toks = lex_ok("||");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::PipePipe);
}

TEST(AlphaLexer_MultiCharOp, LoneAmpersand_ProducesParseError) {
  const auto result = lex("&");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
  EXPECT_NE(result.error().message().find('0'), std::string::npos);
}

TEST(AlphaLexer_MultiCharOp, LonePipe_ProducesParseError) {
  const auto result = lex("|");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
  EXPECT_NE(result.error().message().find('0'), std::string::npos);
}

// ---- number forms -----------------------------------------------------------

TEST(AlphaLexer_Numbers, Integer_LexesCorrectly) {
  const auto toks = lex_ok("5");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[0].number, 5.0);
}

TEST(AlphaLexer_Numbers, LeadingZeroDecimal_LexesCorrectly) {
  const auto toks = lex_ok("0.5");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[0].number, 0.5);
}

TEST(AlphaLexer_Numbers, ScientificLower_LexesCorrectly) {
  const auto toks = lex_ok("1e3");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[0].number, 1000.0);
}

TEST(AlphaLexer_Numbers, ScientificUpper_LexesCorrectly) {
  const auto toks = lex_ok("1E3");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[0].number, 1000.0);
}

TEST(AlphaLexer_Numbers, NegativeExponent_LexesCorrectly) {
  const auto toks = lex_ok("2.5e-2");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[0].number, 0.025);
}

TEST(AlphaLexer_Numbers, LargeInteger_LexesCorrectly) {
  const auto toks = lex_ok("12345");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_DOUBLE_EQ(toks[0].number, 12345.0);
}

// ---- $ sigil ----------------------------------------------------------------

TEST(AlphaLexer_DollarSigil, DollarClose_ProducesDollarThenIdent) {
  const std::string_view src = "$close";
  const auto toks = lex_ok(src);
  ASSERT_EQ(toks.size(), 3U); // Dollar + Ident + End
  EXPECT_EQ(toks[0].kind, TokenKind::Dollar);
  EXPECT_EQ(toks[1].kind, TokenKind::Ident);
  // Span of Dollar is just "$"
  EXPECT_EQ(src.substr(toks[0].span.begin, toks[0].span.end - toks[0].span.begin), "$");
  // Span of Ident is "close"
  EXPECT_EQ(src.substr(toks[1].span.begin, toks[1].span.end - toks[1].span.begin), "close");
}

// ---- span correctness -------------------------------------------------------

TEST(AlphaLexer_Spans, AllTokens_SpanTextMatchesSource) {
  const std::string_view src = "a + 3.14";
  const auto toks = lex_ok(src);
  ASSERT_GE(toks.size(), 3U);
  check_token(src, toks[0], TokenKind::Ident, "a");
  check_token(src, toks[1], TokenKind::Plus, "+");
  check_token(src, toks[2], TokenKind::Number, "3.14");
}

TEST(AlphaLexer_Spans, EndToken_SpanIsLenLen) {
  const std::string_view src = "x";
  const auto toks = lex_ok(src);
  ASSERT_GE(toks.size(), 2U);
  const Token &end = toks.back();
  EXPECT_EQ(end.kind, TokenKind::End);
  EXPECT_EQ(end.span.begin, static_cast<atx::u32>(src.size()));
  EXPECT_EQ(end.span.end, static_cast<atx::u32>(src.size()));
}

TEST(AlphaLexer_Spans, MultiCharOp_SpanCoversFullOperator) {
  const std::string_view src = "<=";
  const auto toks = lex_ok(src);
  ASSERT_GE(toks.size(), 1U);
  check_token(src, toks[0], TokenKind::Le, "<=");
}

TEST(AlphaLexer_Spans, Number_SpanCoversFullNumberText) {
  const std::string_view src = "1e3";
  const auto toks = lex_ok(src);
  ASSERT_GE(toks.size(), 1U);
  check_token(src, toks[0], TokenKind::Number, "1e3");
}

// ---- IndClass.sector — interior dot in ident --------------------------------

TEST(AlphaLexer_Ident, IndClassSector_LexesAsIdentDotIdent) {
  // B4: interior dots are no longer consumed by scan_ident; '.' is a Dot token.
  // "IndClass.sector" → Ident("IndClass"), Dot, Ident("sector"), End.
  const std::string_view src = "IndClass.sector";
  const auto toks = lex_ok(src);
  ASSERT_EQ(toks.size(), 4U); // Ident + Dot + Ident + End
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(src.substr(toks[0].span.begin, toks[0].span.end - toks[0].span.begin), "IndClass");
  EXPECT_EQ(toks[1].kind, TokenKind::Dot);
  EXPECT_EQ(toks[2].kind, TokenKind::Ident);
  EXPECT_EQ(src.substr(toks[2].span.begin, toks[2].span.end - toks[2].span.begin), "sector");
  EXPECT_EQ(toks[3].kind, TokenKind::End);
}

TEST(AlphaLexer_Ident, UnderscoreStart_IsValidIdent) {
  const auto toks = lex_ok("_foo");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
}

TEST(AlphaLexer_Ident, AlphanumericAndUnderscore_IsValidIdent) {
  const auto toks = lex_ok("foo_bar2");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
}

TEST(AlphaLexer_Ident, TrailingDot_ProducesIdentThenDot) {
  // "foo." — the '.' is now a standalone Dot token (B4); no longer an error.
  const auto toks = lex_ok("foo.");
  ASSERT_EQ(toks.size(), 3U); // Ident("foo"), Dot, End
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks[1].kind, TokenKind::Dot);
  EXPECT_EQ(toks[2].kind, TokenKind::End);
}

// ---- unknown byte → ParseError ----------------------------------------------

TEST(AlphaLexer_ParseError, UnknownByte_ProducesParseError) {
  const auto result = lex("@");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
}

TEST(AlphaLexer_ParseError, UnknownByteAtOffset7_MessageContainsOffset) {
  // "abcdefg@" — '@' is at offset 7
  const auto result = lex("abcdefg@");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
  EXPECT_NE(result.error().message().find('7'), std::string::npos);
}

TEST(AlphaLexer_ParseError, UnknownByteAtOffset3_MessageContainsOffset) {
  // "1 + #" — '#' is at offset 4
  const auto result = lex("1 + #");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
  EXPECT_NE(result.error().message().find('4'), std::string::npos);
}

// ---- boundary: empty source -------------------------------------------------

TEST(AlphaLexer_Boundary, EmptySource_ProducesJustEndToken) {
  const auto toks = lex_ok("");
  ASSERT_EQ(toks.size(), 1U);
  EXPECT_EQ(toks[0].kind, TokenKind::End);
  EXPECT_EQ(toks[0].span.begin, 0U);
  EXPECT_EQ(toks[0].span.end, 0U);
}

// ---- boundary: trailing whitespace ------------------------------------------

TEST(AlphaLexer_Boundary, TrailingWhitespace_OnlyPrecedingTokensPlusEnd) {
  const auto toks = lex_ok("x   ");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks[1].kind, TokenKind::End);
}

TEST(AlphaLexer_Boundary, LeadingWhitespace_SkippedCorrectly) {
  const auto toks = lex_ok("   y");
  ASSERT_EQ(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks[1].kind, TokenKind::End);
}

// ---- boundary: adjacent operators -------------------------------------------

TEST(AlphaLexer_Adjacent, ALessEqualMinusB_LexesFourTokens) {
  // "a<=-b" → Ident Le Minus Ident End  (5 tokens)
  const std::string_view src = "a<=-b";
  const auto toks = lex_ok(src);
  ASSERT_EQ(toks.size(), 5U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks[1].kind, TokenKind::Le);
  EXPECT_EQ(toks[2].kind, TokenKind::Minus);
  EXPECT_EQ(toks[3].kind, TokenKind::Ident);
  EXPECT_EQ(toks[4].kind, TokenKind::End);
  check_token(src, toks[1], TokenKind::Le, "<=");
}

TEST(AlphaLexer_Adjacent, EqEqThenBang_LexesCorrectly) {
  // "==!" → EqEq Bang End
  const auto toks = lex_ok("==!");
  ASSERT_EQ(toks.size(), 3U);
  EXPECT_EQ(toks[0].kind, TokenKind::EqEq);
  EXPECT_EQ(toks[1].kind, TokenKind::Bang);
}

TEST(AlphaLexer_Adjacent, AmpAmpThenPipePipe_LexesCorrectly) {
  // "&&||" → AmpAmp PipePipe End
  const auto toks = lex_ok("&&||");
  ASSERT_EQ(toks.size(), 3U);
  EXPECT_EQ(toks[0].kind, TokenKind::AmpAmp);
  EXPECT_EQ(toks[1].kind, TokenKind::PipePipe);
}

// ---- expression: full expression round-trip ---------------------------------

TEST(AlphaLexer_Expression, SimpleArithmetic_LexesAllTokens) {
  // "rank($close) / 100"
  const std::string_view src = "rank($close) / 100";
  const auto toks = lex_ok(src);
  // rank Ident, ( LParen, $ Dollar, close Ident, ) RParen, / Slash, 100 Number, End
  ASSERT_EQ(toks.size(), 8U);
  EXPECT_EQ(toks[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks[1].kind, TokenKind::LParen);
  EXPECT_EQ(toks[2].kind, TokenKind::Dollar);
  EXPECT_EQ(toks[3].kind, TokenKind::Ident);
  EXPECT_EQ(toks[4].kind, TokenKind::RParen);
  EXPECT_EQ(toks[5].kind, TokenKind::Slash);
  EXPECT_EQ(toks[6].kind, TokenKind::Number);
  EXPECT_DOUBLE_EQ(toks[6].number, 100.0);
  EXPECT_EQ(toks[7].kind, TokenKind::End);
}

TEST(AlphaLexer_Expression, TernaryExpr_LexesQuestionAndColon) {
  // "x > 0 ? 1 : -1"
  const auto toks = lex_ok("x > 0 ? 1 : -1");
  // x Ident, > Gt, 0 Number, ? Question, 1 Number, : Colon, - Minus, 1 Number, End
  ASSERT_EQ(toks.size(), 9U);
  EXPECT_EQ(toks[3].kind, TokenKind::Question);
  EXPECT_EQ(toks[5].kind, TokenKind::Colon);
}

// ---- B4: Dot token ----------------------------------------------------------

TEST(AlphaLexer, DotToken) {
  auto toks = lex("kf.beta");
  ASSERT_TRUE(toks);
  ASSERT_GE(toks.value().size(), 4u); // Ident, Dot, Ident, End
  EXPECT_EQ(toks.value()[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks.value()[1].kind, TokenKind::Dot);
  EXPECT_EQ(toks.value()[2].kind, TokenKind::Ident);
}

TEST(AlphaLexer, NumberDotNotSplit) {
  auto toks = lex("3.5");
  ASSERT_TRUE(toks);
  EXPECT_EQ(toks.value()[0].kind, TokenKind::Number); // 3.5 stays one numeric literal
}

} // namespace
