#include "atx/engine/research/fields/gia_13f_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/fields/clock.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// The declared text; atx-engine/tools/field_registry.json's gia_13f row carries it verbatim as
// spec_text (formula fingerprint pinned by the registry check and the AL-SIG gtests).
[[nodiscard]] FieldSpec make_gia_13f_spec() {
  FieldSpec s;
  s.name = "gia_13f";
  s.group = "13f_skill";
  s.formula_id = "gia-13f-skill-wavg-v1";
  s.revision = 1;
  s.min_history = "8 filing quarters before the anchor quarter (the stage holds 2013q2 on)";
  FieldDefinition &d = s.definition;
  d.units = "skill score of the line's 13F holders: the portfolio-weight-weighted mean of their "
            "centered skill ranks (decimal in (-0.5, 0.5))";
  d.clock = "13f-quarter-skill-asof-v1: V(q) = the maximum available_at (stage "
            "13f-filed-plus-46h-v1: filing_date 00:00 UTC + 46 h) over every 13F filing of "
            "quarter q filed by its deadline (effective or not, notices included); the value of "
            "quarter P reads the positions of quarters P-8..P only and is visible at V*(P) = the "
            "maximum V(q) over the quarters q <= P with positions; session t reads the latest "
            "quarter P with V*(P) < date(t-1) 22:00 UTC, P <= date(t) and date(t) - P <= 150 days "
            "in which the line has a mapped 13F position (forward fill across a quarter without "
            "one); session 0 -> NaN";
  d.staleness = "date(t) - P > 150 days -> NaN (stage staleness_days 150); no visible fresh "
                "quarter with a mapped position of the line -> NaN; a NaN value at the anchor "
                "quarter stays NaN (no skip-back); session 0 -> NaN";
  d.source_columns = {"filings.accession",     "filings.filer_cik",
                      "filings.period_q",      "filings.filing_date",
                      "filings.submission_type", "filings.amendment_type",
                      "filings.available_at",  "filings.deadline",
                      "filings.source_period", "holdings.accession",
                      "holdings.cusip",        "holdings.shares",
                      "holdings.sshprnamt_type", "holdings.put_call",
                      "holdings.value_usd",    "filing_checks.unit_factor",
                      "cusip_map_pit.security_id"};
  d.definition =
      "Skill-weighted 13F holdings (after Wermers, Yao and Zhao 2012, RFS 25(12)). Positions of "
      "quarter q: the 13f-asof45-effective-v1 filings of q; holdings rows with sshprnamt_type SH, "
      "no put/call, shares > 0 and a finite value_usd >= 0, value x filing_checks.unit_factor (1 "
      "when unlisted, not finite or not positive), CUSIP -> security_id by cusip_map_pit at q "
      "(unmapped rows dropped); a priced row whose implied price value/shares lies outside [1/10, "
      "10] x its security's median implied price is dropped; (filer CIK, security) pairs summed; "
      "p_q(i) = the median implied price of i's kept priced rows; a filer's total = the sum of "
      "its kept values. Share basis a_q(i) = 13f-basis-change-v1 (the median N_q / N_(q-1) of "
      "i's holders in both quarters when there are at least 5 of them, it lies outside [1/1.2, "
      "1.2] and the price offsets it within [1/1.5, 1.5], else 1). Holdings return of filer k "
      "over quarter q: the value-weighted mean over k's pairs of q-1 with value > 0 of r_q(i) = "
      "a_q(i) p_q(i) / p_(q-1)(i) - 1 (a price ratio outside [1/10, 10] or a missing price: no "
      "return for i), NaN when those pairs hold less than half of k's total of q-1; the "
      "aggregate 13F book's return likewise over every pair. Skill s_k(P) = the mean of k's "
      "excess return (filer minus aggregate) over quarters P-7..P, NaN with fewer than 6. u_k = "
      "the centered average rank of s_k among the filers of P with a skill, (rank - (m + 1) / 2) "
      "/ m, undefined when m < 3. Value of security i at P = the sum of w_ki u_k over the sum of "
      "w_ki over the filers k of P with a skill holding i, w_ki = k's value of i / k's total; "
      "NaN with fewer than 5 such holders. Formula id gia-13f-skill-wavg-v1.";
  d.point_in_time = true;
  d.domain = std::array<f64, 2>{-0.5, 0.5};
  s.caveats = {
      "managers are 13F filers (CIK), not funds; holdings returns use 13F implied quarter-end "
      "prices (no dividends, no trading inside the quarter), not reported fund returns",
      "the skill weights are cross-sectional ranks of trailing excess holdings returns, not "
      "Wermers-Yao-Zhao's generalized-inverse alpha of the fund-weight matrix",
      "the 13F clock is the filing date + 46 h (the data sets carry no acceptance time); "
      "quarters are published only after their 45-day deadline, so a value is 47-150 days old",
      "13F-HR/A amendments filed after the deadline (late confidential-treatment disclosures) "
      "are not used"};
  return s;
}

[[nodiscard]] core::Error refused(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "atx-research-fields spec: " + std::move(message));
}

[[nodiscard]] Json counts_json(const GiaQuarterCounts &c) {
  return Json{{"quarter", iso_date(c.period_day)},
              {"positions", c.positions},
              {"filers", c.filers},
              {"pairs", c.pairs},
              {"rows_mapped", c.rows_mapped},
              {"rows_price_outlier", c.rows_price_outlier},
              {"filers_with_return", c.filers_with_return},
              {"managers_ranked", c.managers_ranked},
              {"securities_scored", c.securities_scored}};
}

[[nodiscard]] Json params_json(const GiaParams &p) {
  return Json{{"skill_quarters", p.skill_quarters},
              {"skill_min_quarters", p.skill_min_quarters},
              {"min_holders", p.min_holders},
              {"min_managers", p.min_managers},
              {"min_covered", p.min_covered},
              {"max_return_ratio", p.max_return_ratio}};
}

} // namespace

const FieldSpec &gia_13f_spec() {
  static const FieldSpec spec = make_gia_13f_spec();
  return spec;
}

GiaAccumulator::GiaAccumulator(std::span<const i64> ids, const GiaParams &params)
    : ids_(ids.begin(), ids.end()), params_(params) {
  panel_.columns = ids_.size();
}

std::vector<f64> GiaAccumulator::filer_scores(const QuarterPositions &positions, usize q,
                                              GiaQuarterCounts &counts) const {
  std::vector<f64> skill(positions.filers.size(), kNaN);
  std::vector<usize> ranked;
  std::vector<f64> ranked_skill;
  for (usize k = 0; k < positions.filers.size(); ++k) {
    const auto it = excess_.find(positions.filers[k]);
    if (it == excess_.end()) {
      continue;
    }
    f64 sum = 0.0;
    usize used = 0;
    for (const auto &[quarter, excess] : it->second) { // chronological
      if (quarter <= q && quarter + params_.skill_quarters > q) {
        sum += excess;
        ++used;
      }
    }
    if (used >= params_.skill_min_quarters) {
      skill[k] = sum / static_cast<f64>(used);
      ranked.push_back(k);
      ranked_skill.push_back(skill[k]);
    }
  }
  std::vector<f64> out(positions.filers.size(), kNaN);
  if (ranked.size() < params_.min_managers) {
    return out;
  }
  counts.managers_ranked = static_cast<u64>(ranked.size());
  const std::vector<f64> u = centered_ranks(ranked_skill);
  for (usize r = 0; r < ranked.size(); ++r) {
    out[ranked[r]] = u[r];
  }
  return out;
}

void GiaAccumulator::add(i64 period_day, i64 visible_ns,
                         std::optional<QuarterPositions> positions) {
  const usize q = panel_.period_day.size();
  const usize n = ids_.size();
  GiaQuarterCounts counts;
  counts.period_day = period_day;
  counts.positions = positions.has_value();
  if (positions && prev_) {
    const HoldingsReturns r = holdings_returns(*prev_, *positions, params_);
    for (usize k = 0; k < r.filer.size() && std::isfinite(r.aggregate); ++k) {
      if (std::isfinite(r.filer[k])) {
        excess_[prev_->filers[k]].emplace_back(q, r.filer[k] - r.aggregate);
        ++counts.filers_with_return;
      }
    }
  }
  panel_.period_day.push_back(period_day);
  panel_.value.resize((q + 1) * n, kNaN);
  panel_.has.resize((q + 1) * n, 0);
  if (positions) {
    visible_so_far_ = std::max(visible_so_far_, visible_ns);
    panel_.visible_ns.push_back(visible_so_far_);
    counts.filers = static_cast<u64>(positions->filers.size());
    counts.pairs = static_cast<u64>(positions->pair_sid.size());
    counts.rows_mapped = positions->rows_mapped;
    counts.rows_price_outlier = positions->rows_price_outlier;
    const std::vector<f64> scores = filer_scores(*positions, q, counts);
    const SecurityScores v = holder_scores(*positions, scores, params_);
    for (usize s = 0; s < v.sid.size(); ++s) {
      const auto col = std::lower_bound(ids_.begin(), ids_.end(), v.sid[s]);
      if (col == ids_.end() || *col != v.sid[s]) {
        continue;
      }
      const usize cell = q * n + static_cast<usize>(col - ids_.begin());
      panel_.has[cell] = 1;
      panel_.value[cell] = v.value[s];
      counts.securities_scored += std::isfinite(v.value[s]) ? 1U : 0U;
    }
  } else {
    panel_.visible_ns.push_back(kNeverNs);
  }
  panel_.counts.push_back(counts);
  for (auto it = excess_.begin(); it != excess_.end();) { // keep the next quarter's window only
    auto &entries = it->second;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [this, q](const std::pair<usize, f64> &e) {
                                   return e.first + params_.skill_quarters <= q + 1;
                                 }),
                  entries.end());
    it = entries.empty() ? excess_.erase(it) : std::next(it);
  }
  prev_ = std::move(positions);
}

void gia_row(const GiaPanel &panel, i64 day, i64 mark, std::span<f64> out, std::span<u8> fresh) {
  const usize n = panel.columns;
  std::fill(out.begin(), out.end(), kNaN);
  std::fill(fresh.begin(), fresh.end(), u8{0});
  for (usize q = 0; q < panel.period_day.size(); ++q) { // ascending: a later quarter wins
    const i64 period = panel.period_day[q];
    if (!(panel.visible_ns[q] < mark) || period > day || day - period > kThirteenfStaleDays) {
      continue;
    }
    for (usize j = 0; j < n; ++j) {
      if (panel.has[q * n + j] != 0) {
        out[j] = panel.value[q * n + j];
        if (!fresh.empty()) {
          fresh[j] = 1;
        }
      }
    }
  }
}

core::Result<Gia13fField> build_gia_13f(PinnedStage &stage, const RoleAxes &role,
                                        const std::filesystem::path &output_dir) {
  const GiaParams params{};
  const auto days = role.days();
  const std::vector<i64> quarters =
      thirteenf_quarters(days.front(), days.back(), params.skill_quarters);
  ATX_TRY(auto index, read_thirteenf_index(stage, quarters));
  GiaAccumulator accumulator(role.ids(), params);
  ThirteenfCounts read_counts;
  for (usize q = 0; q < index.quarters.size(); ++q) {
    const ThirteenfQuarter &quarter = index.quarters[q];
    std::optional<QuarterPositions> positions;
    if (quarter.has_filing && !quarter.sealed) {
      ATX_TRY(auto rows, read_quarter_rows(stage, index, q, read_counts));
      positions = quarter_positions(std::move(rows));
    }
    accumulator.add(quarter.period_day, quarter.visible_ns, std::move(positions));
  }
  index.counts.holdings_parts_not_read_sealed = read_counts.holdings_parts_not_read_sealed;
  index.counts.holdings_rows_kept = read_counts.holdings_rows_kept;
  const usize n = role.instruments();
  const std::vector<i64> marks = previous_session_marks(days);
  ATX_TRY(auto writer, FieldWriter::create(output_dir, gia_13f_spec().name, role));
  std::vector<f64> row(n);
  std::vector<u8> fresh(n);
  u64 no_fresh = 0;
  u64 anchor_nan = 0;
  for (usize t = 0; t < role.dates(); ++t) {
    gia_row(accumulator.panel(), days[t], marks[t], row, fresh);
    ATX_TRY_VOID(writer.write(row));
    const auto member = role.member_row(t);
    for (usize j = 0; j < n; ++j) {
      if (member[j] == 0) {
        continue;
      }
      no_fresh += fresh[j] == 0 ? 1U : 0U;
      anchor_nan += (fresh[j] != 0 && std::isnan(row[j])) ? 1U : 0U;
    }
  }
  ATX_TRY(auto written, writer.close());
  Json per_quarter = Json::array();
  for (const GiaQuarterCounts &c : accumulator.panel().counts) {
    per_quarter.push_back(counts_json(c));
  }
  Json stage_files = Json::array();
  for (const SourceRecord &f : stage.sources()) {
    stage_files.push_back(Json{{"path", f.path}, {"bytes", f.bytes}, {"sha256", f.sha256}});
  }
  Gia13fField out;
  out.field = std::move(written);
  out.checks = Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                    {"seal", std::string(data::kSealBeginDate)},
                    {"window", std::string(data::kResearchWindowId)},
                    {"stage", stage.check()},
                    {"stage_files", std::move(stage_files)},
                    {"thirteenf", thirteenf_checks(index)},
                    {"params", params_json(params)},
                    {"per_quarter", std::move(per_quarter)},
                    {"nan_reasons_member_cells",
                     Json{{"no_fresh_visible_quarter_row", no_fresh},
                          {"anchor_value_nan", anchor_nan}}}};
  return core::Ok(std::move(out));
}

core::Result<FieldPlan> parse_gia_13f_kind(std::string_view field, const nlohmann::json &options,
                                           const BuildSpec &spec) {
  const FieldSpec &s = gia_13f_spec();
  ATX_TRY_VOID(check_kind_request(s, field, options));
  if (!spec.thirteenf) {
    return core::Err(refused("gia_13f needs thirteenf {dir, manifest_sha256}"));
  }
  FieldPlan plan;
  plan.name = s.name;
  plan.spec = &s;
  plan.options = options;
  plan.inputs = {spec.thirteenf->dir / "manifest.json"};
  return core::Ok(std::move(plan));
}

core::Result<KindOutput> build_gia_13f_kind(const FieldPlan & /*plan*/, BuildContext &ctx) {
  if (!ctx.spec.thirteenf) {
    return core::Err(refused("gia_13f needs thirteenf {dir, manifest_sha256}"));
  }
  if (!ctx.sources.thirteenf) { // pinned once per run, shared by every kind that reads it
    ATX_TRY(auto stage, PinnedStage::open(*ctx.spec.thirteenf, kThirteenfStage));
    ctx.sources.thirteenf = std::move(stage);
  }
  PinnedStage &stage = *ctx.sources.thirteenf;
  ATX_TRY(auto built, build_gia_13f(stage, ctx.role, ctx.spec.output_dir));
  KindOutput out;
  out.field = std::move(built.field);
  out.sources = {stage.manifest_source()};
  out.blocks = Json{{"source_checks", std::move(built.checks)},
                    {"extra", Json{{"visibility_rule", std::string(kStageVisibilityRule)},
                                   {"stage_manifest_sha256", stage.manifest_source().sha256},
                                   {"seal_date", std::string(data::kSealBeginDate)}}}};
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
