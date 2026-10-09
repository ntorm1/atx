#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_sign_verb.hpp"

// P9 lane SIGN (ruling C3-11): `atx-equity-strategy-ic sign`, the wave's PM7-35 sign rule on its
// screen rows. The verdicts are the literal decision table of
// atx-engine/tests/research/research_admission_sign_test.cpp (the wave column) and the reasons
// are scripts/wave_rules.py sign_pm7_35's words, both written out here rather than recomputed,
// so the verb is checked against the ruling and not against itself.
namespace {
namespace st = atx::impl::strategy;
namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Directory {
  fs::path path;
  Directory() {
    static std::atomic<std::uint64_t> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           ("atx_sign_verb_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!fs::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};

struct VerbRun {
  int code{};
  std::string out, err;
};

VerbRun run_verb(std::vector<std::string> args) {
  args.insert(args.begin(), "sign");
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  std::ostringstream out, err;
  const int code = st::dispatch_sign(static_cast<int>(argv.size()), argv.data(), out, err);
  return {code, out.str(), err.str()};
}

void write_text(const fs::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) throw std::runtime_error("fixture write");
}

std::string read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Json row(const std::string& id, const std::string& kind, int prior, const std::string& status,
         int runner) {
  return Json{{"id", id}, {"kind", kind}, {"prior_sign", prior}, {"status", status},
              {"runner_sign", runner}};
}

// Writes {"rows": rows} and runs `sign --rule pm7-35` on it into dir/receipt.json.
VerbRun sign_rows(const Directory& dir, const Json& rows, Json& receipt) {
  write_text(dir.path / "rows.json", Json{{"rows", rows}}.dump());
  const VerbRun run = run_verb({"--rule", "pm7-35", "--rows", (dir.path / "rows.json").string(),
                            "--output", (dir.path / "receipt.json").string()});
  if (run.code == 0) receipt = Json::parse(read_text(dir.path / "receipt.json"));
  return run;
}

// research_admission_sign_test.cpp:56-69, the wave column (SignRule::WaveZeroKeptV1), prior +1.
struct TableRow {
  const char* kind;
  bool admitted;
  int runner;
  const char* wave;
};
constexpr std::array<TableRow, 12> kDecisionTable{{
    {"add", false, 1, "keep"},
    {"add", false, 0, "keep"},
    {"add", false, -1, "keep"},
    {"add", true, 1, "keep"},
    {"add", true, 0, "keep"},
    {"add", true, -1, "drop"},
    {"replace", false, 1, "drop"},
    {"replace", false, 0, "drop"},
    {"replace", false, -1, "drop"},
    {"replace", true, 1, "keep"},
    {"replace", true, 0, "drop"},
    {"replace", true, -1, "drop"},
}};

// Every row of the decision table through the verb, at prior +1 and mirrored at prior -1 (runner
// sign negated, so "runner = prior" and "runner against the prior" keep their meaning): verdicts,
// the echoed row, kept / dropped in wave order and the console line.
TEST(SignVerb, DecisionTableThroughTheVerb) {
  const Directory dir;
  Json rows = Json::array();
  std::vector<std::string> want, kept, dropped;
  for (const int prior : {1, -1}) {
    for (const TableRow& t : kDecisionTable) {
      const std::string id = "c" + std::to_string(rows.size());
      rows.push_back(row(id, t.kind, prior, t.admitted ? "admitted" : "reject_redundant",
                         t.runner * prior));
      want.emplace_back(t.wave);
      (std::string_view(t.wave) == "keep" ? kept : dropped).push_back(id);
    }
  }
  Json receipt;
  const VerbRun run = sign_rows(dir, rows, receipt);
  ASSERT_EQ(run.code, 0) << run.err;
  ASSERT_EQ(receipt.at("rows").size(), rows.size());
  for (std::size_t k = 0; k < rows.size(); ++k) {
    const Json& got = receipt.at("rows")[k];
    EXPECT_EQ(got.at("verdict"), want[k]) << rows[k].dump();
    for (const char* key : {"id", "kind", "prior_sign", "status", "runner_sign"}) {
      EXPECT_EQ(got.at(key), rows[k].at(key)) << key << ' ' << rows[k].dump();
    }
  }
  EXPECT_EQ(receipt.at("kept"), Json(kept));
  EXPECT_EQ(receipt.at("dropped"), Json(dropped));
  EXPECT_EQ(kept.size(), 12U);
  EXPECT_EQ(run.out, "sign: pm7-35 (wave-zero-kept-v1): 24 rows, kept 12, dropped 12 -> " +
                         (dir.path / "receipt.json").string() + "\n");
}

// The reasons are wave_rules.sign_pm7_35's (scripts/wave_rules.py:44-57), word for word: the wave
// log and wave-result.json read the same text as under the Python rule.
TEST(SignVerb, ReasonsAreTheWaveRulesWords) {
  const Directory dir;
  const Json rows = Json::array({row("a", "add", 1, "admitted", 1),
                                 row("b", "add", 1, "admitted", 0),
                                 row("c", "add", 1, "admitted", -1),
                                 row("d", "add", -1, "reject_veto", 1),
                                 row("e", "replace", 1, "admitted", 1),
                                 row("f", "replace", -1, "admitted", 1)});
  Json receipt;
  const VerbRun run = sign_rows(dir, rows, receipt);
  ASSERT_EQ(run.code, 0) << run.err;
  const std::array<const char*, 6> reasons{
      "addition admitted with the prior sign",
      "addition admitted, runner sign 0 (R-2 precedent)",
      "addition admitted with runner sign -1 against prior +1: dropped from the wave",
      "addition not admitted (status reject_veto): stays at weight 0",
      "replacement admitted with the prior sign",
      "replacement not kept (status admitted, runner sign 1, prior -1): the replaced member "
      "keeps its string at 0 trials"};
  for (std::size_t k = 0; k < reasons.size(); ++k) {
    EXPECT_EQ(receipt.at("rows")[k].at("reason"), reasons[k]) << k;
  }
  EXPECT_EQ(receipt.at("kept"), Json::array({"a", "b", "d", "e"}));
  EXPECT_EQ(receipt.at("dropped"), Json::array({"c", "f"}));
}

// The receipt names its schema, rule and the predicate's sign rule and pins the rows file (its
// path as given, the SHA-256 of its bytes). Keys beyond the five a row needs are ignored, so the
// screen block of a wave result (rows carrying a recorded decision and reason) is accepted as
// it stands; a recorded decision never decides.
TEST(SignVerb, ReceiptPinsTheRowsFileAndIgnoresOtherKeys) {
  const Directory dir;
  Json a = row("alpha_a", "add", 1, "admitted", -1);
  a["decision"] = "keep"; // a recorded decision that disagrees: ignored
  a["reason"] = "recorded";
  a["s_k"] = 1;
  const Json block{{"spec", "scripts/specs/v8/lib-w1.json"}, {"gate_exit", 0},
                   {"rows", Json::array({a, row("alpha_b", "add", 1, "admitted", 1)})},
                   {"kept", Json::array({"alpha_a", "alpha_b"})}, {"sign_rule", "pm7-35"}};
  const std::string text = block.dump(2) + "\n";
  write_text(dir.path / "screen.json", text);
  const std::string rows_path = (dir.path / "screen.json").string();
  const VerbRun run = run_verb({"--rule", "pm7-35", "--rows", rows_path, "--output",
                            (dir.path / "out.json").string()});
  ASSERT_EQ(run.code, 0) << run.err;
  const std::string bytes = read_text(dir.path / "out.json");
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(bytes.back(), '\n');
  const Json receipt = Json::parse(bytes);
  EXPECT_EQ(receipt.at("schema"), std::string(st::kSignReceiptSchema));
  EXPECT_EQ(receipt.at("schema"), "atx.sign-receipt/v1");
  EXPECT_EQ(receipt.at("rule"), "pm7-35");
  EXPECT_EQ(receipt.at("sign_rule"), "wave-zero-kept-v1");
  EXPECT_EQ(receipt.at("rows_file"), rows_path);
  EXPECT_EQ(receipt.at("rows_sha256"), *atx::core::sha256_hex(std::string_view(text)));
  const std::set<std::string> keys{"id", "kind", "prior_sign", "status", "runner_sign",
                                   "verdict", "reason"};
  for (const Json& got : receipt.at("rows")) {
    std::set<std::string> have;
    for (auto it = got.begin(); it != got.end(); ++it) have.insert(it.key());
    EXPECT_EQ(have, keys);
  }
  EXPECT_EQ(receipt.at("rows")[0].at("verdict"), "drop");
  EXPECT_EQ(receipt.at("kept"), Json::array({"alpha_b"}));
  EXPECT_EQ(receipt.at("dropped"), Json::array({"alpha_a"}));
}

struct Bad {
  const char* name;
  std::string text;
  const char* needle;
};

std::string rows_text(const std::string& rows) { return "{\"rows\": [" + rows + "]}"; }

const std::string kGood =
    R"({"id": "a", "kind": "add", "prior_sign": 1, "status": "admitted", "runner_sign": 1})";

std::string good_with(const std::string& key, const std::string& value) {
  Json j = Json::parse(kGood);
  if (value.empty()) {
    j.erase(key);
  } else {
    j[key] = Json::parse(value);
  }
  return j.dump();
}

std::vector<Bad> bad_rows() {
  std::vector<Bad> out{
      {"not JSON", "{\"rows\": [", "the rows file is not a JSON object"},
      {"array root", "[" + kGood + "]", "the rows file is not a JSON object"},
      {"no rows", "{\"candidates\": [" + kGood + "]}", "needs a non-empty rows array"},
      {"rows an object", "{\"rows\": {}}", "needs a non-empty rows array"},
      {"empty rows", rows_text(""), "needs a non-empty rows array"},
      {"row not an object", rows_text("1"), "row 1 is not an object"},
      {"no id", rows_text(good_with("id", "")), "row 1 needs a non-empty string id"},
      {"empty id", rows_text(good_with("id", "\"\"")), "row 1 needs a non-empty string id"},
      {"no kind", rows_text(good_with("kind", "")), "row 1 (a) needs a string kind"},
      {"unknown kind", rows_text(good_with("kind", "\"rescreen\"")),
       "row 1 (a): unknown kind \"rescreen\" (add | replace)"},
      {"prior 0", rows_text(good_with("prior_sign", "0")), "needs an integer prior_sign 1 or -1"},
      {"prior 2", rows_text(good_with("prior_sign", "2")), "needs an integer prior_sign 1 or -1"},
      {"prior text", rows_text(good_with("prior_sign", "\"1\"")),
       "needs an integer prior_sign 1 or -1"},
      {"prior bool", rows_text(good_with("prior_sign", "true")),
       "needs an integer prior_sign 1 or -1"},
      {"prior float", rows_text(good_with("prior_sign", "1.0")),
       "needs an integer prior_sign 1 or -1"},
      {"no status", rows_text(good_with("status", "")),
       "row 1 (a) has no admission row (status, runner_sign): the screen did not score this "
       "string (a stop, not a drop)"},
      {"no runner sign", rows_text(good_with("runner_sign", "")),
       "row 1 (a) has no admission row (status, runner_sign)"},
      {"null status", rows_text(good_with("status", "null")), "needs a non-empty string status"},
      {"empty status", rows_text(good_with("status", "\"\"")), "needs a non-empty string status"},
      {"runner 2", rows_text(good_with("runner_sign", "2")),
       "needs an integer runner_sign -1, 0 or 1"},
      {"runner -2", rows_text(good_with("runner_sign", "-2")),
       "needs an integer runner_sign -1, 0 or 1"},
      {"runner huge", rows_text(good_with("runner_sign", "18446744073709551615")),
       "needs an integer runner_sign -1, 0 or 1"},
      {"runner bool", rows_text(good_with("runner_sign", "false")),
       "needs an integer runner_sign -1, 0 or 1"},
      {"duplicate id", rows_text(kGood + ", " + kGood), "duplicate id a"},
      {"second row",
       rows_text(kGood + ", " +
                 R"({"id": "b", "kind": "keep", "prior_sign": 1, "status": "admitted", )"
                 R"("runner_sign": 1})"),
       "row 2 (b): unknown kind \"keep\""},
  };
  std::string many;
  for (int k = 0; k <= 20000; ++k) {
    many += std::string(k ? "," : "") + R"({"id": "r)" + std::to_string(k) +
            R"(", "kind": "add", "prior_sign": 1, "status": "admitted", "runner_sign": 1})";
  }
  out.push_back({"too many rows", rows_text(many), "more than 20000 rows"});
  out.push_back(
      {"over 16 MiB", std::string((16U << 20) + 1U, ' '), "is unreadable or over 16 MiB"});
  return out;
}

// Every malformed rows file is refused (exit 1, the reason on stderr) before any output.
TEST(SignVerb, RefusesMalformedRowsBeforeAnyOutput) {
  for (const Bad& bad : bad_rows()) {
    const Directory dir;
    write_text(dir.path / "rows.json", bad.text);
    const VerbRun run = run_verb({"--rule", "pm7-35", "--rows", (dir.path / "rows.json").string(),
                              "--output", (dir.path / "receipt.json").string()});
    EXPECT_EQ(run.code, 1) << bad.name << ": " << run.err;
    EXPECT_NE(run.err.find(bad.needle), std::string::npos) << bad.name << ": " << run.err;
    EXPECT_FALSE(fs::exists(dir.path / "receipt.json")) << bad.name;
    EXPECT_TRUE(run.out.empty()) << bad.name;
  }
}

// A missing rows file is refused (exit 1, nothing written); an existing output is never
// overwritten (exit 1, its bytes unchanged), even beside a valid rows file.
TEST(SignVerb, RefusesAMissingRowsFileAndAnExistingOutput) {
  const Directory dir;
  const std::string rows = (dir.path / "rows.json").string();
  const std::string receipt = (dir.path / "receipt.json").string();
  VerbRun run = run_verb({"--rule", "pm7-35", "--rows", rows, "--output", receipt});
  EXPECT_EQ(run.code, 1);
  EXPECT_NE(run.err.find("cannot open the rows file"), std::string::npos) << run.err;
  EXPECT_FALSE(fs::exists(receipt));
  write_text(rows, rows_text(kGood));
  write_text(receipt, "kept\n");
  run = run_verb({"--rule", "pm7-35", "--rows", rows, "--output", receipt});
  EXPECT_EQ(run.code, 1);
  EXPECT_NE(run.err.find("the output must not exist (never overwritten)"), std::string::npos)
      << run.err;
  EXPECT_EQ(read_text(receipt), "kept\n");
  run = run_verb({"--rule", "pm7-35", "--rows", rows, "--output", rows});
  EXPECT_EQ(run.code, 1);
  EXPECT_EQ(read_text(rows), rows_text(kGood));
}

// Usage errors exit 2 and write nothing: an unknown rule id (a predicate name is not a wave rule
// id), an unknown, repeated or value-less flag, a missing required flag. --help exits 0.
TEST(SignVerb, UsageErrorsWriteNothing) {
  const Directory dir;
  const std::string rows = (dir.path / "rows.json").string();
  const std::string receipt = (dir.path / "receipt.json").string();
  write_text(rows, rows_text(kGood));
  const std::vector<std::pair<std::vector<std::string>, const char*>> cases{
      {{"--rule", "pm7-36", "--rows", rows, "--output", receipt},
       "unknown --rule pm7-36 (known: pm7-35)"},
      {{"--rule", "wave-zero-kept-v1", "--rows", rows, "--output", receipt},
       "unknown --rule wave-zero-kept-v1"},
      {{"--rule", "pm7-35", "--row", rows, "--output", receipt}, "unknown flag --row"},
      {{"--rule", "pm7-35", "--rule", "pm7-35", "--rows", rows, "--output", receipt},
       "missing value or repeated flag --rule"},
      {{"--rule", "pm7-35", "--rows", rows}, "--rule, --rows and --output are required"},
      {{"--rule", "pm7-35", "--output", receipt, "--rows"},
       "missing value or repeated flag --rows"},
  };
  for (const auto& [args, needle] : cases) {
    const VerbRun run = run_verb(args);
    EXPECT_EQ(run.code, 2) << needle << ": " << run.err;
    EXPECT_NE(run.err.find(needle), std::string::npos) << needle << ": " << run.err;
    EXPECT_NE(run.err.find("usage: atx-equity-strategy-ic sign --rule pm7-35"), std::string::npos)
        << needle;
    EXPECT_FALSE(fs::exists(receipt)) << needle;
  }
  const VerbRun help = run_verb({"--help"});
  EXPECT_EQ(help.code, 0);
  EXPECT_NE(help.out.find("usage: atx-equity-strategy-ic sign"), std::string::npos);
}
} // namespace
