// Platform P9 B2 (migration slice 8): the trial-counting rules of research/ledger equal the
// Python readers of record (backtest_integrity, dsr_total.trial_breakdown, era_pool) on the
// committed fixture (fixtures/research_ledger; expected.json pinned by its own pytest).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/research/ledger/era_pool.hpp"
#include "atx/engine/research/ledger/ledger_file.hpp"
#include "atx/engine/research/ledger/trial_counts.hpp"

namespace {

namespace fs = std::filesystem;
namespace lg = atx::engine::research::ledger;
using atx::f64;
using atx::i64;
using atx::usize;
using Json = nlohmann::json;

std::string read_text(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const Json &values() {
  static const Json doc =
      Json::parse(read_text(fs::path(ATX_RESEARCH_LEDGER_FIXTURE) / "expected.json"));
  return doc["values"];
}

const lg::LedgerFile &ledger() {
  static const lg::LedgerFile file = [] {
    auto r = lg::read_ledger(fs::path(ATX_RESEARCH_LEDGER_FIXTURE) / "ledger.jsonl",
                             lg::MissingLedger::Refuse);
    EXPECT_TRUE(r.has_value()) << r.error().to_string();
    return r.has_value() ? *r : lg::LedgerFile{};
  }();
  return file;
}

// numpy's ddof-1 variance against Python's value at the P12 tolerance (1e-12 relative).
void expect_close(const Json &want, const std::optional<f64> &got, const char *what) {
  if (want.is_null()) {
    EXPECT_FALSE(got.has_value()) << what;
    return;
  }
  ASSERT_TRUE(got.has_value()) << what;
  const f64 w = want.get<f64>();
  EXPECT_LE(std::fabs(*got - w), 1e-12 * std::fabs(w)) << what << " " << *got << " vs " << w;
}

TEST(ResearchLedger, TrialCountsEqualPython) {
  const Json &v = values();
  const lg::Records recs = ledger().records;
  ASSERT_EQ(recs.size(), v["lines"].get<usize>());
  const auto counts = lg::trial_counts(recs);
  ASSERT_TRUE(counts.has_value()) << counts.error().to_string();
  EXPECT_EQ(*counts, v["trial_counts"].get<std::vector<i64>>());

  std::set<std::string> invalid;
  for (const Json &id : v["invalid_ids"]) {
    invalid.insert(Json(id.get<std::string>()).dump()); // the library keys ids by dumped JSON
  }
  EXPECT_EQ(lg::invalid_ids(recs), invalid);
  EXPECT_EQ(lg::history_read_lines(recs), v["history_read_lines"].get<std::vector<usize>>());
  EXPECT_EQ(lg::excluded_lines(recs), v["excluded_lines"].get<std::vector<usize>>());
  const auto campaigns = lg::campaign_registry_count(recs);
  ASSERT_TRUE(campaigns.has_value());
  EXPECT_EQ(*campaigns, v["campaign_registry_count"].get<i64>());
}

TEST(ResearchLedger, LedgerNEqualsPython) {
  const Json &n = values()["ledger_n"];
  const lg::Records recs = ledger().records;
  EXPECT_EQ(*lg::ledger_n(recs, true), n["construction_in"].get<i64>());
  EXPECT_EQ(*lg::ledger_n(recs, false), n["construction_out"].get<i64>());
  EXPECT_EQ(*lg::ledger_n(recs, true, "admission"), n["admission_in"].get<i64>());
}

TEST(ResearchLedger, NTotBreakdownEqualsPython) {
  const Json &b = values()["trial_breakdown"];
  const auto got = lg::trial_breakdown(ledger().records);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  ASSERT_EQ(got->by_kind.size(), b["by_kind"].size());
  for (const auto &[kind, trials] : got->by_kind) {
    ASSERT_TRUE(b["by_kind"].contains(kind)) << kind;
    EXPECT_EQ(trials, b["by_kind"][kind].get<i64>()) << kind;
  }
  EXPECT_EQ(got->ledger_trials, b["ledger_trials"].get<i64>());
  EXPECT_EQ(got->campaign_registry, b["campaign_registry"].get<i64>());
  EXPECT_EQ(got->n_tot, b["n_tot"].get<i64>());
}

TEST(ResearchLedger, WindowVarianceEqualsPython) {
  for (const auto &[window, want] : values()["dsr_variance"].items()) {
    const auto got = lg::dsr_variance(ledger().records, window);
    ASSERT_TRUE(got.has_value()) << got.error().to_string();
    EXPECT_EQ(got->n, want["n"].get<i64>()) << window;
    EXPECT_EQ(got->window_id, want["window_id"].get<std::string>());
    EXPECT_EQ(got->cells, want["cells"].get<usize>()) << window;
    EXPECT_EQ(got->legacy_cells, want["legacy_cells"].get<usize>()) << window;
    EXPECT_EQ(got->variance_source, want["variance_source"].get<std::string>());
    expect_close(want["variance_sr"], got->variance_sr, "variance_sr");
    expect_close(want["legacy_variance_sr"], got->legacy_variance_sr, "legacy_variance_sr");
  }
}

TEST(ResearchLedger, ConstructionIdsAndPrefixesEqualPython) {
  const Json &v = values();
  const lg::Records recs = ledger().records;
  const std::set<std::string> ids = lg::construction_ids(recs);
  EXPECT_EQ(std::vector<std::string>(ids.begin(), ids.end()),
            v["construction_ids"].get<std::vector<std::string>>());
  for (const auto &[trial, want] : v["prefix_length"].items()) {
    const std::optional<usize> k = lg::construction_prefix_length(recs, trial);
    if (want.is_null()) {
      EXPECT_FALSE(k.has_value()) << trial;
    } else {
      ASSERT_TRUE(k.has_value()) << trial;
      EXPECT_EQ(*k, want.get<usize>()) << trial;
    }
  }
}

TEST(ResearchLedger, TrialIdentitiesEqualPython) {
  const Json &id = values()["identities"];
  const std::string a(64, 'a');
  const std::string b(64, 'b');
  EXPECT_EQ(*lg::trial_id("construction", a), id["trial_id_construction_a"].get<std::string>());
  EXPECT_EQ(*lg::trial_id("admission", b), id["trial_id_admission_b"].get<std::string>());
  const std::vector<std::string> one = {a};
  const std::vector<std::string> two = {a, b};
  EXPECT_EQ(*lg::pooled_sha256(one), id["pooled_sha256_one"].get<std::string>());
  EXPECT_EQ(*lg::pooled_sha256(two), id["pooled_sha256_two"].get<std::string>());
  EXPECT_EQ(*lg::pooled_trial_id("construction", two),
            id["pooled_trial_id_two"].get<std::string>());
  const std::vector<std::string> dirs = {"navs/e1", "navs\\e2"};
  EXPECT_EQ(lg::pool_label(dirs), id["pool_label"].get<std::string>());
}

} // namespace
