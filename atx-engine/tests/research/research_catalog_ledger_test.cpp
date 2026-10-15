// P9 lane SQL4 (rulings SQL-7 / SQL-9; sql-design 3.11): the catalog's trial-ledger index head
// comes from B2's research/ledger chain_head through the bound seam, and `verify --ledger`
// checks every head a catalogued verdict or wave result records against the prefix heads of the
// catalogued ledger (B2's prefix_heads): a tail edit refuses, an appended line does not, an
// inconsistent ledger_state row refuses. The JSONL file stays the authority.

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/ledger/chain.hpp"
#include "atx/engine/research/store/catalog/catalog.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/store_cli.hpp"
#include "atx/engine/research/store/store.hpp"
#include "research/research_catalog_test_support.hpp"

namespace cat = atx::engine::research::store::catalog;
namespace lg = atx::engine::research::ledger;
namespace t = atx::engine::research::store::catalog::test;

namespace {

constexpr std::string_view kLedger = "build-equity/trials.jsonl";
constexpr std::string_view kVerdict = "build-equity/cycle-fx-base/cycle_verdict.json";

// The non-blank lines of a ledger file, without line ends (read independently of the catalog).
std::vector<std::string> file_lines(const std::filesystem::path &path) {
  std::vector<std::string> out;
  std::istringstream in{t::read_bytes(path)};
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!lg::is_blank_line(line)) {
      out.push_back(line);
    }
  }
  return out;
}

// The synthetic tree catalogued with the bound seam (the CLI's catalog and ingest use it).
atx::core::db::Database catalogued(const std::filesystem::path &tree) {
  auto db = t::open_new(t::temp_dir("db") / "catalog.sqlite");
  cat::CatalogOptions opt = t::options(tree);
  opt.ledger_head = cat::default_ledger_head();
  (void)t::run(db, opt);
  return db;
}

struct CliRun {
  int code{};
  std::string out;
  std::string err;
};

CliRun cli(std::initializer_list<std::string> args) {
  const std::vector<std::string> argv(args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  CliRun r;
  r.code = cat::run_store_cli(argv, out, err);
  r.out = out.str();
  r.err = err.str();
  return r;
}

cat::LedgerVerifyReport verified(atx::core::db::Database &db,
                                 std::initializer_list<std::string> ledgers) {
  const std::vector<std::string> paths(ledgers.begin(), ledgers.end());
  auto report = cat::verify_ledger_heads(db, paths);
  EXPECT_TRUE(report) << report.error().to_string();
  return report ? *report : cat::LedgerVerifyReport{};
}

} // namespace

// The bound seam fills ledger_state with B2's chain head of the ledger's lines (and the rule's
// name): the head the fixture's verdicts recorded (make_store_tree.py took it from
// backtest_integrity.ledger_head). The CLI's catalog fills it the same way.
TEST(ResearchCatalog, LedgerHeadFromResearchLedger) {
  const auto tree = t::tree_copy();
  auto db = catalogued(tree);
  const auto lines = file_lines(tree / std::string{kLedger});
  ASSERT_EQ(lines.size(), 5U);
  auto head = lg::chain_head(lines);
  ASSERT_TRUE(head) << head.error().to_string();
  EXPECT_EQ(t::scalar_text(db, "SELECT head_sha256 FROM ledger_state;"), head->head_sha256);
  EXPECT_EQ(t::scalar_text(db, "SELECT head_rule FROM ledger_state;"), std::string{lg::kChainRule});
  EXPECT_EQ(t::scalar_text(db, "SELECT ledger_head FROM cycle_verdict WHERE path = '" +
                                   std::string{kVerdict} + "';"),
            head->head_sha256);
  const std::string path = (t::temp_dir("cli") / "catalog.sqlite").string();
  ASSERT_EQ(cli({"init", "--catalog", path}).code, cat::kExitOk);
  const CliRun run = cli({"catalog", "--catalog", path, "--root", tree.string(), "--classes",
                          ATX_RESEARCH_STORE_CLASSES_FILE});
  ASSERT_EQ(run.code, cat::kExitOk) << run.err;
  auto cli_db = cat::open_catalog(path, atx::engine::research::store::StoreOpen::Existing);
  ASSERT_TRUE(cli_db) << cli_db.error().to_string();
  EXPECT_EQ(t::scalar_text(*cli_db, "SELECT head_sha256 FROM ledger_state;"), head->head_sha256);
}

// verify --ledger: every recorded head of the intact tree is the head of the ledger's 5-line
// prefix; one more chained line keeps them prefix heads; a tail edit before cataloguing leaves
// them outside every prefix (exit 3); an inconsistent ledger_state row and a ledger that is not
// catalogued refuse; heads naming another ledger are counted, not checked.
TEST(ResearchCatalog, RecordedHeadsArePrefixHeads) {
  const std::string ledger{kLedger};
  {
    const auto tree = t::tree_copy("intact");
    auto db = catalogued(tree);
    const cat::LedgerVerifyReport report = verified(db, {ledger});
    EXPECT_TRUE(report.ok());
    ASSERT_EQ(report.ledgers.size(), 1U);
    EXPECT_EQ(report.ledgers[0].lines, 5);
    EXPECT_FALSE(report.ledgers[0].problem.has_value())
        << report.ledgers[0].problem.value_or(std::string{});
    ASSERT_EQ(report.recorded.size(), 3U); // two cycle verdicts and the wave result
    for (const cat::RecordedLedgerHead &r : report.recorded) {
      EXPECT_EQ(r.prefix_lines, std::optional<atx::i64>{5}) << r.table << " " << r.path;
      EXPECT_EQ(r.ledger_path, std::optional<std::string>{ledger}) << r.path;
    }
    EXPECT_EQ(report.recorded.back().table, "wave_result");
    EXPECT_EQ(report.other, 0);
    // A '\' path names the same ledger; another ledger's name checks nothing and refuses.
    EXPECT_TRUE(verified(db, {"build-equity\\trials.jsonl"}).ok());
    const cat::LedgerVerifyReport elsewhere = verified(db, {"build-equity/other.jsonl"});
    EXPECT_FALSE(elsewhere.ok());
    ASSERT_EQ(elsewhere.ledgers.size(), 1U);
    EXPECT_EQ(elsewhere.ledgers[0].problem, std::optional<std::string>{"not catalogued"});
    EXPECT_EQ(elsewhere.other, 3);
    // An inconsistent index refuses: the row count, then the stored head.
    ASSERT_TRUE(db.exec("UPDATE ledger_state SET lines = 4;"));
    EXPECT_FALSE(verified(db, {ledger}).ok());
    ASSERT_TRUE(db.exec("UPDATE ledger_state SET lines = 5, head_sha256 = '" +
                        std::string(64, '0') + "';"));
    EXPECT_FALSE(verified(db, {ledger}).ok());
  }
  {
    // One more chained line (its prev_sha256 the 5-line head): the recorded heads stay prefix
    // heads of the longer ledger.
    const auto tree = t::tree_copy("appended");
    const auto path = tree / ledger;
    auto head = lg::chain_head(file_lines(path));
    ASSERT_TRUE(head);
    std::string bytes = t::read_bytes(path);
    if (!bytes.empty() && bytes.back() != '\n') {
      bytes += "\n";
    }
    t::write_bytes(path, bytes +
                             R"({"cell":"fx-cell-6","count":6,"kind":"fixture","prev_sha256":")" +
                             head->head_sha256 +
                             R"(","schema":"atx.trial-ledger/v1","trial_id":"fx-t6"})" + "\n");
    auto db = catalogued(tree);
    const cat::LedgerVerifyReport report = verified(db, {ledger});
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.ledgers[0].lines, 6);
    for (const cat::RecordedLedgerHead &r : report.recorded) {
      EXPECT_EQ(r.prefix_lines, std::optional<atx::i64>{5}) << r.path;
    }
  }
  {
    // A tail edit (the last line the verdicts' head covers) before cataloguing: exit 3 through
    // the CLI, every recorded head outside every prefix; the ledger's own index is consistent.
    const auto tree = t::tree_copy("edited");
    const auto path = tree / ledger;
    std::string bytes = t::read_bytes(path);
    bytes.replace(bytes.find("fx-cell-5"), 9, "fx-cell-X");
    t::write_bytes(path, bytes);
    const std::string db_path = (t::temp_dir("edited-db") / "catalog.sqlite").string();
    ASSERT_EQ(cli({"init", "--catalog", db_path}).code, cat::kExitOk);
    const CliRun catalog = cli({"catalog", "--catalog", db_path, "--root", tree.string(),
                                "--classes", ATX_RESEARCH_STORE_CLASSES_FILE});
    ASSERT_EQ(catalog.code, cat::kExitOk) << catalog.err;
    const CliRun refused = cli({"verify", "--catalog", db_path, "--ledger", ledger});
    EXPECT_EQ(refused.code, cat::kExitRefusal) << refused.out << refused.err;
    EXPECT_NE(refused.out.find(" index ok\n"), std::string::npos) << refused.out;
    EXPECT_NE(refused.out.find("recorded heads 3 not-a-prefix-head 3 other-ledger 0\n"),
              std::string::npos)
        << refused.out;
    // The intact tree through the CLI: exit 0, and --pins alongside keeps its report.
    const auto intact = t::tree_copy("intact-cli");
    const std::string ok_path = (t::temp_dir("intact-db") / "catalog.sqlite").string();
    ASSERT_EQ(cli({"init", "--catalog", ok_path}).code, cat::kExitOk);
    ASSERT_EQ(cli({"catalog", "--catalog", ok_path, "--root", intact.string(), "--classes",
                   ATX_RESEARCH_STORE_CLASSES_FILE})
                  .code,
              cat::kExitOk);
    const CliRun passed = cli({"verify", "--catalog", ok_path, "--ledger", ledger});
    EXPECT_EQ(passed.code, cat::kExitOk) << passed.out << passed.err;
    EXPECT_NE(passed.out.find("recorded heads 3 not-a-prefix-head 0 other-ledger 0\n"),
              std::string::npos)
        << passed.out;
    const CliRun pins = cli({"verify", "--catalog", ok_path, "--pins"});
    const CliRun both = cli({"verify", "--catalog", ok_path, "--pins", "--ledger", ledger});
    EXPECT_EQ(both.code, cat::kExitOk) << both.err;
    EXPECT_EQ(both.out.rfind(pins.out, 0), 0U) << both.out; // the pins report first, unchanged
    EXPECT_EQ(cli({"verify", "--catalog", ok_path}).code, cat::kExitUsage);
  }
}
