// Platform P9 B2 (carry SQL-7): the chain-head function of research/ledger on any prefix of a
// ledger, the reader's verification walk, and the Python's values on the committed fixture
// (fixtures/research_ledger: backtest_integrity.chain_head of every prefix and ledger_head).

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/research/ledger/chain.hpp"
#include "atx/engine/research/ledger/ledger_file.hpp"

namespace {

namespace fs = std::filesystem;
namespace lg = atx::engine::research::ledger;
using atx::usize;
using Json = nlohmann::json;

std::string read_text(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const Json &expected() {
  static const Json doc =
      Json::parse(read_text(fs::path(ATX_RESEARCH_LEDGER_FIXTURE) / "expected.json"));
  return doc;
}

fs::path ledger_path() { return fs::path(ATX_RESEARCH_LEDGER_FIXTURE) / "ledger.jsonl"; }

// The fixture's non-blank lines.
std::vector<std::string> fixture_lines() {
  std::vector<std::string> out;
  for (const std::string &line : lg::split_lines(read_text(ledger_path()))) {
    if (!lg::is_blank_line(line)) {
      out.push_back(line);
    }
  }
  return out;
}

std::string legacy(int k) {
  return R"({"schema":"atx.trial-ledger/v1","kind":"construction","trial_id":"t)" +
         std::to_string(k) + R"(","count":1})";
}

std::string chained(int k, const std::string &prev) {
  return R"({"schema":"atx.trial-ledger/v1","kind":"construction","trial_id":"c)" +
         std::to_string(k) + R"(","prev_sha256":")" + prev + R"("})";
}

// Two legacy lines then three chained ones, each naming the head before it.
std::vector<std::string> small_ledger() {
  std::vector<std::string> lines = {legacy(1), legacy(2)};
  for (int k = 0; k < 3; ++k) {
    const auto head = lg::chain_head(lines);
    EXPECT_TRUE(head.has_value());
    lines.push_back(chained(k, head->head_sha256));
  }
  return lines;
}

TEST(ResearchLedgerChain, EmptyPrefixIsTheGenesisUnderTheNamedRule) {
  const auto head = lg::chain_head(std::vector<std::string>{});
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(head->head_sha256, std::string(lg::kChainGenesis));
  EXPECT_EQ(head->lines, 0U);
  EXPECT_EQ(head->rule, "atx-ledger-chain-fold/v1");
}

TEST(ResearchLedgerChain, LegacyLinesFoldAndAChainedLineIsItsOwnHash) {
  const std::vector<std::string> lines = small_ledger();
  const auto one = lg::chain_head(std::span<const std::string>(lines).first(1));
  ASSERT_TRUE(one.has_value());
  const auto fold = lg::fold_head(lg::kChainGenesis, lines[0]);
  ASSERT_TRUE(fold.has_value());
  EXPECT_EQ(one->head_sha256, *fold);
  const auto all = lg::chain_head(lines);
  const auto last = lg::line_sha256(lines.back());
  ASSERT_TRUE(all.has_value() && last.has_value());
  EXPECT_EQ(all->head_sha256, *last);
  EXPECT_EQ(all->lines, lines.size());
}

TEST(ResearchLedgerChain, PrefixHeadsAreTheHeadOfEveryPrefix) {
  const std::vector<std::string> lines = small_ledger();
  const auto heads = lg::prefix_heads(lines);
  ASSERT_TRUE(heads.has_value());
  ASSERT_EQ(heads->size(), lines.size());
  for (usize k = 1; k <= lines.size(); ++k) {
    const auto head = lg::chain_head(std::span<const std::string>(lines).first(k));
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ((*heads)[k - 1U], head->head_sha256) << k;
  }
}

TEST(ResearchLedgerChain, LineEndsAndBlankLinesDoNotMoveTheHead) {
  const std::vector<std::string> lines = small_ledger();
  std::vector<std::string> crlf;
  for (const std::string &l : lines) {
    crlf.push_back(l + "\r\n");
    crlf.emplace_back("  \t");
  }
  const auto a = lg::chain_head(lines);
  const auto b = lg::chain_head(crlf);
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_EQ(a->head_sha256, b->head_sha256);
  EXPECT_EQ(b->lines, lines.size());
}

TEST(ResearchLedgerChain, VerifyAcceptsTheChainAndRefusesEachBreak) {
  const std::vector<std::string> lines = small_ledger();
  const auto ok = lg::verify_chain(lines);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();

  std::vector<std::string> broken = lines;
  broken[3] = chained(1, std::string(64, 'f'));
  const auto link = lg::verify_chain(broken);
  ASSERT_FALSE(link.has_value());
  EXPECT_NE(link.error().message().find("line 4: hash chain broken"), std::string::npos)
      << link.error().message();

  std::vector<std::string> tail = lines;
  tail.push_back(legacy(9));
  EXPECT_FALSE(lg::verify_chain(tail).has_value());

  std::vector<std::string> schema = lines;
  schema[0] = R"({"schema":"atx.other/v1","kind":"construction"})";
  EXPECT_FALSE(lg::verify_chain(schema).has_value());

  std::vector<std::string> edited = lines;
  edited[0] = legacy(7);
  EXPECT_FALSE(lg::verify_chain(edited).has_value());
}

// Review B2 S2: a legacy line written with "prev_sha256": null is unchained for the chain-head
// function as for the reader's walk, so the head a verdict records and the verified head agree.
TEST(ResearchLedgerChain, ANullPrevLineFoldsInEveryRule) {
  std::vector<std::string> lines = {
      legacy(1), R"({"schema":"atx.trial-ledger/v1","kind":"construction","trial_id":"t2",)"
                 R"("count":1,"prev_sha256":null})"};
  const auto mid = lg::chain_head(lines);
  ASSERT_TRUE(mid.has_value());
  const auto fold1 = lg::fold_head(lg::kChainGenesis, lines[0]);
  ASSERT_TRUE(fold1.has_value());
  const auto fold2 = lg::fold_head(*fold1, lines[1]);
  ASSERT_TRUE(fold2.has_value());
  EXPECT_EQ(mid->head_sha256, *fold2);
  const auto link = lg::is_chained_line(lines[1]);
  ASSERT_TRUE(link.has_value());
  EXPECT_FALSE(*link);
  lines.push_back(chained(0, mid->head_sha256));
  const auto verified = lg::verify_chain(lines);
  ASSERT_TRUE(verified.has_value()) << verified.error().to_string();
  const auto head = lg::chain_head(lines);
  const auto heads = lg::prefix_heads(lines);
  ASSERT_TRUE(head.has_value() && heads.has_value());
  EXPECT_EQ(head->head_sha256, verified->head_sha256);
  EXPECT_EQ(heads->back(), verified->head_sha256);
  EXPECT_EQ((*heads)[1], *fold2);
}

TEST(ResearchLedgerChain, ANonObjectLineIsAParseError) {
  const std::vector<std::string> lines = {legacy(1), "not json"};
  const auto head = lg::chain_head(lines);
  ASSERT_FALSE(head.has_value());
  EXPECT_EQ(head.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(ResearchLedgerChain, FixturePrefixHeadsEqualPython) {
  const Json &v = expected()["values"];
  const std::vector<std::string> lines = fixture_lines();
  ASSERT_EQ(lines.size(), v["lines"].get<usize>());
  const auto heads = lg::prefix_heads(lines);
  ASSERT_TRUE(heads.has_value()) << heads.error().to_string();
  ASSERT_EQ(heads->size(), v["prefix_heads"].size());
  for (usize k = 0; k < heads->size(); ++k) {
    EXPECT_EQ((*heads)[k], v["prefix_heads"][k].get<std::string>()) << k;
  }
  const auto file = lg::read_ledger(ledger_path(), lg::MissingLedger::Refuse);
  ASSERT_TRUE(file.has_value()) << file.error().to_string();
  EXPECT_EQ(file->head.head_sha256, v["ledger_head"].get<std::string>());
  EXPECT_EQ(file->lines.size(), lines.size());
  EXPECT_EQ(file->records.size(), lines.size());
}

TEST(ResearchLedgerChain, AnAbsentLedgerIsEmptyOrRefused) {
  const fs::path absent = fs::temp_directory_path() / "atx_research_ledger_absent.jsonl";
  std::error_code ec;
  fs::remove(absent, ec);
  const auto empty = lg::read_ledger(absent, lg::MissingLedger::Empty);
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->records.empty());
  EXPECT_EQ(empty->head.head_sha256, std::string(lg::kChainGenesis));
  const auto refused = lg::read_ledger(absent, lg::MissingLedger::Refuse);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), atx::core::ErrorCode::NotFound);
}

} // namespace
