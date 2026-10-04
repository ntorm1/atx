// Lane AL-SIG: the Russell-proxy reader and field russell_recon -- snapshots from constituents rows
// (other indexes, the reader-side seal and unusable rows counted; undeclared index ids refused),
// the boundary moves in closed form, the event window, and the planted-leak probe with teeth: a
// change planted in a reconstitution's snapshot reaches the field at the first session whose
// previous-session mark passes the rank date's availability and never earlier, and the same probe
// flags a same-session read.
#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/engine/research/fields/russell_recon_field.hpp"
#include "atx/engine/research/fields/sources/russell_proxy.hpp"
#include "research/research_fields_al_sig_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using atx::f64;
using atx::i64;
using atx::usize;

namespace {

i64 day(i64 y, atx::u32 m, atx::u32 d) { return support::civil_day(y, m, d); }

i64 mark_of(i64 d) { return d * fields::kDayNs + fields::kSessionMarkNs; }

const i64 kRank1 = day(2020, 5, 8);
const i64 kRecon1 = day(2020, 6, 26);
const i64 kRank2 = day(2021, 5, 7);
const i64 kRecon2 = day(2021, 6, 25);
const i64 kRank3 = day(2024, 4, 30);
const i64 kRecon3 = day(2024, 6, 28);

fields::RussellRow row(std::string index, i64 sid, i64 rank, i64 recon) {
  return fields::RussellRow{std::move(index), sid, rank, recon, mark_of(rank)};
}

// 2020: R1000P {1, 3}, R2000P {2, 4, 6}. 2021: R1000P {2, 3, 6}, R2000P {1, 4, 5, 6} (6 in both).
// 2024: sealed. Plus other indexes and unusable rows.
std::vector<fields::RussellRow> base_rows() {
  std::vector<fields::RussellRow> rows{
      row("R1000P", 1, kRank1, kRecon1), row("R1000P", 3, kRank1, kRecon1),
      row("R2000P", 2, kRank1, kRecon1), row("R2000P", 4, kRank1, kRecon1),
      row("R2000P", 6, kRank1, kRecon1), row("R3000P", 1, kRank1, kRecon1),
      row("R1000P", 2, kRank2, kRecon2), row("R1000P", 3, kRank2, kRecon2),
      row("R1000P", 6, kRank2, kRecon2), row("R2000P", 1, kRank2, kRecon2),
      row("R2000P", 4, kRank2, kRecon2), row("R2000P", 5, kRank2, kRecon2),
      row("R2000P", 6, kRank2, kRecon2), row("SP500P", 3, kRank2, kRecon2),
      row("R1000P", 1, kRank3, kRecon3),          // available after the seal
      row("R2000P", -1, kRank2, kRecon2),         // null security_id
      row("R1000P", 7, kRank2, fields::kNullDay)}; // null recon_date
  fields::RussellRow never = row("R2000P", 8, kRank2, kRecon2);
  never.available_ns = fields::kNeverNs; // null available_at
  rows.push_back(never);
  return rows;
}

} // namespace

TEST(ResearchFieldsAlSigRussell, SnapshotsFromRows) {
  const auto rows = base_rows();
  const auto s = fields::russell_snapshots(rows);
  ASSERT_TRUE(s.has_value()) << s.error().message();
  EXPECT_EQ(s->counts.rows_total, rows.size());
  EXPECT_EQ(s->counts.rows_other_index, 2U);
  EXPECT_EQ(s->counts.rows_sealed, 1U);
  EXPECT_EQ(s->counts.rows_unusable, 3U);
  EXPECT_EQ(s->index_ids, (std::vector<std::string>{"R1000P", "R2000P", "R3000P", "SP500P"}));
  ASSERT_EQ(s->recons.size(), 2U); // the sealed 2024 reconstitution is not one
  const auto &r1 = s->recons[0];
  EXPECT_EQ(r1.recon_day, kRecon1);
  EXPECT_EQ(r1.rank_day, kRank1);
  EXPECT_EQ(r1.visible_ns, mark_of(kRank1));
  EXPECT_EQ(r1.r1000, (std::vector<i64>{1, 3}));
  EXPECT_EQ(r1.r2000, (std::vector<i64>{2, 4, 6}));
  EXPECT_EQ(s->recons[1].r1000, (std::vector<i64>{2, 3, 6}));
  EXPECT_EQ(s->recons[1].r2000, (std::vector<i64>{1, 4, 5, 6}));
  const auto checks = fields::russell_checks(*s);
  EXPECT_EQ(checks.at("rows_sealed_dropped"), 1);
  EXPECT_EQ(checks.at("reconstitutions").size(), 2U);
}

TEST(ResearchFieldsAlSigRussell, UndeclaredIndexIdsAreRefused) {
  EXPECT_EQ(fields::kRussell1000Id, "R1000P"); // the house constituents ids (atx-db indexes.md)
  EXPECT_EQ(fields::kRussell2000Id, "R2000P");
  // The membership file's flag column names (r1000, r2000) are not constituents index ids.
  const std::vector<fields::RussellRow> other{row("r1000", 1, kRank1, kRecon1),
                                              row("r2000", 2, kRank1, kRecon1),
                                              row("russell2000", 3, kRank1, kRecon1)};
  const auto refused = fields::russell_snapshots(other);
  ASSERT_FALSE(refused.has_value());
  EXPECT_NE(refused.error().message().find("r1000, r2000, russell2000"), std::string::npos)
      << refused.error().message();
  EXPECT_FALSE(fields::russell_snapshots({}).has_value());
  // Declared ids whose rows are all sealed: no reconstitution, not a refusal.
  const auto sealed = fields::russell_snapshots(
      std::vector<fields::RussellRow>{row("R1000P", 1, kRank3, kRecon3)});
  ASSERT_TRUE(sealed.has_value());
  EXPECT_TRUE(sealed->recons.empty());
}

TEST(ResearchFieldsAlSigRussell, IndexesStageRule) {
  // The fixture manifest carries the house stage's rule list; the contract needs russell-proxy-v1.
  const fields::StageRequest fixture = support::al_sig_stage("indexes");
  EXPECT_EQ(fields::kIndexesStage.rule, "russell-proxy-v1");
  const auto opened = fields::PinnedStage::open(fixture, fields::kIndexesStage);
  ASSERT_TRUE(opened.has_value()) << opened.error().message();
  EXPECT_TRUE(opened->manifest().at("rule").is_array());
  fields::StageContract other = fields::kIndexesStage;
  other.rule = "russell-proxy-v2";
  EXPECT_FALSE(fields::PinnedStage::open(fixture, other).has_value());
  // A manifest whose rule is one string: equal passes, another rule or no rule is refused.
  const auto dir = support::scratch("al_sig_indexes_rule");
  const auto with_rule = [&dir](const nlohmann::json &rule) {
    nlohmann::json m{{"schema", "atx.alpha-panel.indexes/v1"},
                     {"status", "complete"},
                     {"files", nlohmann::json::object()}};
    if (!rule.is_null()) {
      m["rule"] = rule;
    }
    const std::string text = m.dump();
    support::write_bytes(dir / "manifest.json", text);
    const fields::StageRequest request{dir, support::sha256_of(text)};
    return fields::PinnedStage::open(request, fields::kIndexesStage).has_value();
  };
  EXPECT_TRUE(with_rule("russell-proxy-v1"));
  EXPECT_FALSE(with_rule("sp500-proxy-v1"));
  EXPECT_FALSE(with_rule(nlohmann::json::array({"sp500-proxy-v1"})));
  EXPECT_FALSE(with_rule(nullptr));
}

TEST(ResearchFieldsAlSigRussell, MovesClosedForm) {
  const auto s = fields::russell_snapshots(base_rows()).value();
  const std::vector<i64> ids{1, 2, 3, 4, 5, 6, 9};
  const auto moves = fields::recon_moves(s, ids);
  ASSERT_EQ(moves.size(), 1U); // the first reconstitution has no prior
  const auto &m = moves[0];
  EXPECT_EQ(m.recon_day, kRecon2);
  EXPECT_EQ(m.visible_ns, mark_of(kRank2));
  EXPECT_EQ(m.value[0], 1.0);  // r1000 -> r2000: predicted R2000 addition
  EXPECT_EQ(m.value[1], -1.0); // r2000 -> r1000
  EXPECT_EQ(m.value[2], 0.0);  // stays in r1000
  EXPECT_EQ(m.value[3], 0.0);  // stays in r2000
  EXPECT_TRUE(std::isnan(m.value[4])); // new to the snapshots
  EXPECT_TRUE(std::isnan(m.value[5])); // in both indexes of one snapshot
  EXPECT_TRUE(std::isnan(m.value[6])); // never a member
  EXPECT_EQ(m.down, 1U);
  EXPECT_EQ(m.up, 1U);
  EXPECT_EQ(m.conflict, 1U);
}

TEST(ResearchFieldsAlSigRussell, EventWindowClock) {
  const auto s = fields::russell_snapshots(base_rows()).value();
  const std::vector<i64> ids{1, 2};
  const auto moves = fields::recon_moves(s, ids);
  std::vector<f64> out(2);
  const i64 visible = mark_of(kRank2);
  EXPECT_FALSE(fields::russell_recon_row(moves, kRank2 + 3, visible, out).has_value());
  EXPECT_TRUE(std::isnan(out[0]) && std::isnan(out[1])); // a mark equal to V(R) sees nothing
  ASSERT_TRUE(fields::russell_recon_row(moves, kRank2 + 3, visible + 1, out).has_value());
  EXPECT_EQ(out, (std::vector<f64>{1.0, -1.0}));
  ASSERT_TRUE(fields::russell_recon_row(moves, kRecon2 - 1, mark_of(kRecon2 - 2), out));
  EXPECT_EQ(out, (std::vector<f64>{1.0, -1.0})); // the last session before the reconstitution
  EXPECT_FALSE(fields::russell_recon_row(moves, kRecon2, mark_of(kRecon2 - 1), out).has_value());
  EXPECT_TRUE(std::isnan(out[0])); // the reconstitution day and after: outside the window
  EXPECT_FALSE(fields::russell_recon_row(moves, kRecon2 + 30, mark_of(kRecon2 + 29), out));
}

namespace {

// The first session whose row differs between two move sets under `marks`.
std::optional<usize> first_changed(const std::vector<fields::ReconMove> &a,
                                   const std::vector<fields::ReconMove> &b,
                                   const std::vector<i64> &days, const std::vector<i64> &marks) {
  std::vector<f64> ra(a.empty() ? usize{0} : a[0].value.size());
  std::vector<f64> rb(ra.size());
  for (usize t = 0; t < days.size(); ++t) {
    static_cast<void>(fields::russell_recon_row(a, days[t], marks[t], ra));
    static_cast<void>(fields::russell_recon_row(b, days[t], marks[t], rb));
    for (usize j = 0; j < ra.size(); ++j) {
      if (!support::same_bits(ra[j], rb[j])) {
        return t;
      }
    }
  }
  return std::nullopt;
}

} // namespace

TEST(ResearchFieldsAlSigRussell, PlantedLeakProbeHasTeeth) {
  // The plant: line 3 moves to R2000P in the 2021 snapshot (0 -> +1).
  auto planted_rows = base_rows();
  for (auto &r : planted_rows) {
    if (r.index_id == "R1000P" && r.security_id == 3 && r.recon_day == kRecon2) {
      r.index_id = "R2000P";
    }
  }
  const std::vector<i64> ids{1, 2, 3, 4};
  const auto base = fields::recon_moves(fields::russell_snapshots(base_rows()).value(), ids);
  const auto planted =
      fields::recon_moves(fields::russell_snapshots(planted_rows).value(), ids);
  const std::vector<i64> days = support::consecutive_days(day(2021, 4, 1), 100);
  const std::vector<i64> honest = support::honest_marks(days);
  const i64 visible = mark_of(kRank2);
  const auto changed = first_changed(base, planted, days, honest);
  ASSERT_TRUE(changed.has_value());
  EXPECT_GT(honest[*changed], visible);
  EXPECT_LE(honest[*changed - 1], visible);
  EXPECT_EQ(days[*changed], kRank2 + 2); // the rank date's 22:00 mark is passed on the next day
  const auto leaked = first_changed(base, planted, days, support::leaky_marks(days));
  ASSERT_TRUE(leaked.has_value());
  EXPECT_LT(*leaked, *changed);
  EXPECT_LE(honest[*leaked], visible); // the probe condition flags the same-session read
}
