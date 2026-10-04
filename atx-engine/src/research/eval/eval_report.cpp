#include "atx/engine/research/eval/eval_report.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <new>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#include "atx/engine/eval/deflated_sharpe_house.hpp"
#include "atx/engine/research/eval/conditional.hpp"
#include "atx/engine/research/eval/cscv_report.hpp"
#include "atx/engine/research/eval/eval_series.hpp"
#include "atx/engine/research/eval/house_moments.hpp"
#include "atx/engine/research/eval/onc_house.hpp"
#include "atx/engine/research/ledger/chain.hpp"
#include "atx/engine/research/ledger/ledger_file.hpp"
#include "atx/engine/research/ledger/trial_counts.hpp"

namespace atx::engine::research::eval {
namespace {

namespace lg = atx::engine::research::ledger;
using Json = nlohmann::json;

// ------------------------------------------------------------------ JSON values
// A float as the Python rows hold it: NaN / infinity print as null (json.dumps would write
// NaN; the nav_summ adapter turns the documented keys back into nan).
[[nodiscard]] Json num(f64 v) { return std::isfinite(v) ? Json(v) : Json(nullptr); }
[[nodiscard]] Json opt(const std::optional<f64> &v) { return v ? num(*v) : Json(nullptr); }

[[nodiscard]] core::Error invalid(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "atx-research-eval: " + std::move(message));
}

[[nodiscard]] Json moments_json(const HouseMoments &m) {
  return Json{{"sessions", m.sessions},
              {"sr_daily", opt(m.sr_daily)},
              {"skew", opt(m.skew)},
              {"kurtosis", opt(m.kurtosis)}};
}

[[nodiscard]] std::optional<f64> annual(const std::optional<f64> &daily) {
  return daily ? std::optional<f64>(*daily * std::sqrt(kAnnualization)) : std::nullopt;
}

// backtest_integrity.psr_report.
[[nodiscard]] Json psr_json(const PsrReport &r) {
  Json out = moments_json(r.moments);
  out["sr_annual"] = opt(r.sr_annual);
  out["alpha"] = r.alpha;
  Json rows = Json::array();
  for (const PsrBenchmark &b : r.benchmarks) {
    rows.push_back(Json{{"sr_star_annual", b.sr_star_annual},
                        {"sr_star_daily", b.sr_star_daily},
                        {"psr", opt(b.psr)},
                        {"min_trl_sessions", opt(b.min_trl_sessions)},
                        {"min_trl_years", opt(b.min_trl_years)}});
  }
  out["benchmarks"] = std::move(rows);
  return out;
}

// K-P9-5 min_trl: the MinTRL of each benchmark.
[[nodiscard]] Json min_trl_json(const PsrReport &r) {
  Json rows = Json::array();
  for (const PsrBenchmark &b : r.benchmarks) {
    rows.push_back(Json{{"sr_star_annual", b.sr_star_annual},
                        {"sessions", opt(b.min_trl_sessions)},
                        {"years", opt(b.min_trl_years)}});
  }
  return rows;
}

// nav_summ.dsr_rows' row.
[[nodiscard]] Json cross_json(const DsrRow &row, const HouseMoments &m) {
  return Json{{"n", row.n},
              {"sessions", m.sessions},
              {"sr_daily", opt(m.sr_daily)},
              {"skew", opt(m.skew)},
              {"kurtosis", opt(m.kurtosis)},
              {"sr_annual", opt(annual(m.sr_daily))},
              {"dsr", opt(row.dsr)},
              {"sr0_daily", opt(row.sr0_daily)},
              {"sr0_annual", opt(row.sr0_annual)},
              {"variance_sr", opt(row.variance_sr)},
              {"variance_source", row.variance_source}};
}

// backtest_integrity.lo_null_dsr's row.
[[nodiscard]] Json lo_json(const DsrRow &row) {
  return Json{{"n", row.n},
              {"dsr", opt(row.dsr)},
              {"sr0_daily", opt(row.sr0_daily)},
              {"sr0_annual", opt(row.sr0_annual)},
              {"variance_sr", opt(row.variance_sr)},
              {"variance_source", row.variance_source}};
}

// dsr_total.dsr_at with the ledger's integer N (an N below 2 leaves the row undefined).
[[nodiscard]] Json dsr_at_json(const HouseMoments &m, i64 n, const std::optional<f64> &variance) {
  Json row{{"n", n}, {"sr0_daily", nullptr}, {"sr0_annual", nullptr}, {"dsr", nullptr}};
  if (n < 2) {
    return row;
  }
  const DsrRow r = dsr_at(m, static_cast<usize>(n), variance);
  row["sr0_daily"] = opt(r.sr0_daily);
  row["sr0_annual"] = opt(r.sr0_annual);
  row["dsr"] = opt(r.dsr);
  return row;
}

// ------------------------------------------------------------------ ledger rows
struct LedgerState {
  std::string path;
  bool exists{};
  lg::LedgerFile file;
  std::set<std::string> ledgered; // construction trial ids
};

// nav_summ.ledger_dsr: DSR house_v1 (N = construction trials, V = the window cell variance; the
// legacy variance beside it).
[[nodiscard]] core::Result<Json> house_v1_json(const HouseMoments &m, lg::Records records,
                                               bool in_ledger, bool history,
                                               const std::string &window_id) {
  ATX_TRY(const lg::DsrVariance v, lg::dsr_variance(records, window_id));
  ATX_TRY(const i64 n, lg::ledger_n(records, in_ledger || history));
  std::string rule = "construction trials of the ledger by the defect rule";
  if (history) {
    rule += " (a history read adds no trial, Ruling E-41)";
  } else if (!in_ledger) {
    rule += " + 1 (this cell, not yet ledgered)";
  }
  Json row{{"n", n},
           {"window_id", v.window_id},
           {"variance_sr", opt(v.variance_sr)},
           {"cells", v.cells},
           {"variance_source", v.variance_source},
           {"legacy_variance_sr", opt(v.legacy_variance_sr)},
           {"legacy_cells", v.legacy_cells},
           {"n_rule", rule},
           {"dsr", nullptr},
           {"sr0_daily", nullptr},
           {"sr0_annual", nullptr},
           {"legacy_dsr", nullptr},
           {"legacy_sr0_annual", nullptr}};
  if (!m.defined() || m.sessions < 2U || n < 2) {
    return core::Ok(std::move(row));
  }
  if (v.variance_sr) {
    const DsrRow r = dsr_at(m, static_cast<usize>(n), v.variance_sr);
    row["sr0_annual"] = opt(r.sr0_annual);
    row["dsr"] = opt(r.dsr);
    row["sr0_daily"] = opt(r.sr0_daily);
  }
  if (v.legacy_variance_sr) {
    const DsrRow r = dsr_at(m, static_cast<usize>(n), v.legacy_variance_sr);
    row["legacy_sr0_annual"] = opt(r.sr0_annual);
    row["legacy_dsr"] = opt(r.dsr);
  }
  return core::Ok(std::move(row));
}

// dsr_total.ledger_rows: the tot (N_tot) and v8 (construction N) rows with the same V.
[[nodiscard]] core::Result<Json> ledger_rows_json(const HouseMoments &m, lg::Records records,
                                                  bool in_ledger, const std::string &window_id) {
  ATX_TRY(const lg::DsrVariance v, lg::dsr_variance(records, window_id));
  ATX_TRY(const lg::TrialBreakdown parts, lg::trial_breakdown(records));
  ATX_TRY(const i64 n_c, lg::ledger_n(records, in_ledger));
  const i64 n_tot = parts.n_tot + (in_ledger ? 0 : 1);
  Json by_kind = Json::object();
  for (const auto &[kind, trials] : parts.by_kind) {
    by_kind[kind] = trials;
  }
  return core::Ok(Json{{"window_id", window_id},
                       {"in_ledger", in_ledger},
                       {"variance_sr", opt(v.variance_sr)},
                       {"cells_in_v", v.cells},
                       {"variance_source", v.variance_source},
                       {"parts",
                        Json{{"by_kind", std::move(by_kind)},
                             {"ledger_trials", parts.ledger_trials},
                             {"campaign_registry", parts.campaign_registry},
                             {"n_tot", parts.n_tot}}},
                       {"tot", dsr_at_json(m, n_tot, v.variance_sr)},
                       {"v8", dsr_at_json(m, n_c, v.variance_sr)}});
}

[[nodiscard]] core::Result<LedgerState> load_ledger(const EvalRequest &rq) {
  LedgerState s;
  s.path = *rq.ledger;
  std::error_code ec;
  s.exists = std::filesystem::is_regular_file(std::filesystem::path(s.path), ec);
  if (!s.exists && rq.ledger_missing == lg::MissingLedger::Refuse) {
    return core::Err(invalid("ledger " + s.path +
                             " does not exist (N_tot and V are read from it; an absent ledger "
                             "would read as N 0)"));
  }
  ATX_TRY(lg::LedgerFile file, lg::read_ledger(std::filesystem::path(s.path), rq.ledger_missing));
  s.file = std::move(file);
  s.ledgered = lg::construction_ids(s.file.records);
  return core::Ok(std::move(s));
}

[[nodiscard]] Json ledger_doc(const LedgerState &s, const std::string &window_id) {
  return Json{{"path", s.path},
              {"exists", s.exists},
              {"lines", s.file.lines.size()},
              {"chain_head", s.file.head.head_sha256},
              {"chain_rule", s.file.head.rule},
              {"window_id", window_id}};
}

// ------------------------------------------------------------------ paired
[[nodiscard]] Json paired_json(const PairedStats &p, const std::string &reference) {
  Json lw{{"dsr_population", num(p.lw.dsr_population)},
          {"se", num(p.lw.se)},
          {"ci95", Json::array({opt(p.lw.ci_lo), opt(p.lw.ci_hi)})},
          {"p_value", opt(p.lw.p_value)},
          {"valid_draws", p.lw.valid_draws}};
  if (p.config.one_sided) {
    lw["p_one_sided"] = opt(p.lw.p_one_sided);
  }
  return Json{{"sessions", p.sessions},
              {"sr", num(p.sr)},
              {"sr_reference", num(p.sr_reference)},
              {"dsr", num(p.dsr)},
              {"rho", num(p.rho)},
              {"one_minus_rho", num(p.one_minus_rho)},
              {"degenerate", p.degenerate},
              {"memmel_se", num(p.memmel_se)},
              {"t", opt(p.t)},
              {"cbb_ci95", Json::array({num(p.cbb_lo), num(p.cbb_hi)})},
              {"cbb_valid_draws", p.cbb_valid_draws},
              {"lw", std::move(lw)},
              {"draws", p.config.draws},
              {"block", p.config.block},
              {"seed", p.config.seed},
              // K-P9-5: se is the studentized bootstrap's SE (Python lw["se"]: the delta-method
              // SE with the Bartlett HAC, lag block - 1) and p_one / p_two its p-values, the
              // test the bundle verdict reads; memmel_se is listed beside it.
              {"se", num(p.lw.se)},
              {"p_one", opt(p.lw.p_one_sided)},
              {"p_two", opt(p.lw.p_value)},
              {"reference", reference}};
}

// The series a reference path names, read once.
class SeriesCache {
public:
  [[nodiscard]] core::Result<const NetSeries *> get(const std::string &path) {
    const auto it = cache_.find(path);
    if (it != cache_.end()) {
      return core::Ok(static_cast<const NetSeries *>(&it->second));
    }
    ATX_TRY(NetSeries s, read_net_series(path, std::filesystem::path(path)));
    const auto pos = cache_.emplace(path, std::move(s)).first;
    return core::Ok(static_cast<const NetSeries *>(&pos->second));
  }

private:
  std::map<std::string, NetSeries> cache_;
};

// ------------------------------------------------------------------ request checks
[[nodiscard]] core::Status check_request(const EvalRequest &rq) {
  if (rq.daily.empty()) {
    return core::Err(invalid("no --daily series"));
  }
  std::set<std::string> labels;
  for (const LabeledPath &c : rq.daily) {
    if (c.label.empty() || !labels.insert(c.label).second) {
      return core::Err(invalid("--daily labels must be distinct and non-empty: '" + c.label + "'"));
    }
  }
  for (const auto *group : {&rq.paired_of, &rq.trials}) {
    for (const LabeledPath &p : *group) {
      if (!labels.contains(p.label)) {
        return core::Err(invalid("unknown cell label '" + p.label + "'"));
      }
    }
  }
  for (const std::string &h : rq.history) {
    if (!labels.contains(h)) {
      return core::Err(invalid("unknown --history label '" + h + "'"));
    }
  }
  if (rq.dsr_n < 2U) {
    return core::Err(invalid("--dsr-n must be >= 2"));
  }
  if (!(rq.psr_alpha > 0.0 && rq.psr_alpha < 0.5)) {
    return core::Err(invalid("--psr-alpha in (0, 0.5)"));
  }
  if (!(rq.conditional_alpha > 0.0 && rq.conditional_alpha < 1.0) ||
      !std::isfinite(rq.threshold)) {
    return core::Err(invalid("--conditional-alpha in (0, 1) and a finite --threshold"));
  }
  if (rq.onc_init < 1U) {
    return core::Err(invalid("--onc-init must be >= 1"));
  }
  if (!rq.hands.empty() && !rq.ledger) {
    return core::Err(invalid("--hand needs --ledger"));
  }
  return core::Ok();
}

[[nodiscard]] const std::string *find_label(const std::vector<LabeledPath> &rows,
                                            const std::string &label) {
  for (const LabeledPath &r : rows) {
    if (r.label == label) {
      return &r.path;
    }
  }
  return nullptr;
}

// The series of a multi-series block: the listed ones, else the cells.
[[nodiscard]] core::Result<std::vector<NetSeries>>
block_series(const std::vector<LabeledPath> &listed, const std::vector<NetSeries> &cells) {
  if (listed.empty()) {
    return core::Ok(cells);
  }
  std::vector<NetSeries> out;
  out.reserve(listed.size());
  for (const LabeledPath &p : listed) {
    ATX_TRY(NetSeries s, read_net_series(p.label, std::filesystem::path(p.path)));
    out.push_back(std::move(s));
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] Aligned align_series(const std::vector<NetSeries> &series) {
  std::vector<const NetSeries *> ptrs;
  ptrs.reserve(series.size());
  for (const NetSeries &s : series) {
    ptrs.push_back(&s);
  }
  return align_many(ptrs);
}

// ------------------------------------------------------------------ blocks
struct Cell {
  NetSeries series;
  HouseMoments moments;
  PsrReport psr;
  Json dsr = Json::object();
  Json paired = nullptr;
  std::optional<std::string> paired_note;
  Json conditional = nullptr;
};

[[nodiscard]] Json effective_json(const EffectiveTrials &eff,
                                  const std::vector<NetSeries> &series) {
  Json clusters = Json::array();
  for (const std::vector<usize> &cl : eff.clusters) {
    Json names = Json::array();
    for (const usize k : cl) {
      names.push_back(series[k].label);
    }
    clusters.push_back(std::move(names));
  }
  Json srs = Json::array();
  for (const std::optional<f64> &s : eff.cluster_sr_daily) {
    srs.push_back(opt(s));
  }
  return Json{{"n_trials", eff.n_trials},
              {"n_eff", eff.n_eff},
              {"sessions", eff.sessions},
              {"clusters", std::move(clusters)},
              {"cluster_sr_daily", std::move(srs)},
              {"variance_sr", opt(eff.variance_sr)},
              {"n_init", eff.n_init},
              {"seed", eff.seed},
              {"mean_offdiag_corr", num(eff.mean_offdiag_corr)}};
}

[[nodiscard]] Json effective_n_json(const EffectiveNDsr &e) {
  return Json{{"n_eff", e.n_eff},
              {"n_trials", e.n_trials},
              {"variance_sr", opt(e.variance_sr)},
              {"dsr", opt(e.dsr)},
              {"sr0_daily", opt(e.sr0_daily)},
              {"sr0_annual", opt(e.sr0_annual)},
              {"variance_source", e.variance_source}};
}

[[nodiscard]] core::Result<Json> onc_block(const EvalRequest &rq, std::vector<Cell> &cells,
                                           const std::vector<NetSeries> &cell_series) {
  ATX_TRY(const std::vector<NetSeries> series, block_series(rq.onc_series, cell_series));
  const Aligned a = align_series(series);
  if (a.sessions.size() < 3U) {
    return core::Err(invalid("--onc needs >= 3 sessions common to its series, got " +
                             std::to_string(a.sessions.size())));
  }
  ATX_TRY(const EffectiveTrials eff, effective_trials(a.matrix, a.sessions.size(), a.columns,
                                                       rq.onc_init, rq.onc_seed));
  for (Cell &c : cells) {
    c.dsr["onc"] = effective_n_json(effective_n_dsr(c.moments, eff));
  }
  return core::Ok(effective_json(eff, series));
}

[[nodiscard]] Json pbo_json(const CscvReport &r, const std::vector<NetSeries> &series,
                            const Aligned &a) {
  Json ranks = Json::array();
  Json logits_k = Json::array();
  Json splits_k = Json::array();
  for (usize k = 0; k < r.n_candidates; ++k) {
    ranks.push_back(k);
    logits_k.push_back(num(r.rank_logits[k]));
    splits_k.push_back(r.rank_splits[k]);
  }
  Json labels = Json::array();
  for (const NetSeries &s : series) {
    labels.push_back(s.label);
  }
  Json logits = Json::array();
  for (const f64 v : r.logits) {
    logits.push_back(num(v));
  }
  const std::array<const char *, 5> names = {"p5", "p25", "p50", "p75", "p95"};
  Json quantiles = Json::object();
  for (usize i = 0; i < names.size(); ++i) {
    quantiles[names[i]] = num(r.logit_quantiles[i]);
  }
  return Json{{"pbo", num(r.pbo)},
              {"n_candidates", r.n_candidates},
              {"sessions", r.sessions},
              {"blocks", r.blocks},
              {"block_width", r.block_width},
              {"dropped_tail_sessions", r.dropped_tail_sessions},
              {"splits", r.splits},
              {"splits_total", r.splits_total},
              {"exhaustive", true},
              {"seed", nullptr},
              {"logit_mean", num(r.logit_mean)},
              {"logit_quantiles", std::move(quantiles)},
              {"logit_distribution",
               Json{{"oos_rank", std::move(ranks)},
                    {"logit", std::move(logits_k)},
                    {"splits", std::move(splits_k)}}},
              {"winner_is_sr_annual_mean", num(r.winner_is_sr_annual_mean)},
              {"winner_oos_sr_annual_mean", num(r.winner_oos_sr_annual_mean)},
              {"prob_winner_oos_loss", num(r.prob_winner_oos_loss)},
              {"degradation_slope", opt(r.degradation_slope)},
              {"winner_counts", r.winner_counts},
              {"logits", std::move(logits)},
              {"labels", std::move(labels)},
              {"common_sessions_ns",
               Json::array({a.sessions.front(), a.sessions.back()})},
              {"engine",
               Json{{"rule", "pbo_cscv_traced CachedMomentsV2"},
                    {"cached_evaluations", r.cached_evaluations},
                    {"reference_evaluations", r.reference_evaluations},
                    {"ambiguous_comparisons", r.ambiguous_comparisons},
                    {"constant_candidates", r.constant_candidates}}}};
}

[[nodiscard]] core::Result<Json> pbo_block(const EvalRequest &rq,
                                           const std::vector<NetSeries> &cell_series) {
  ATX_TRY(const std::vector<NetSeries> series, block_series(rq.pbo_series, cell_series));
  if (series.size() < kPboMinCells) {
    return core::Err(invalid("--pbo needs a grid of >= " + std::to_string(kPboMinCells) +
                             " cells, got " + std::to_string(series.size())));
  }
  const Aligned a = align_series(series);
  if (a.sessions.empty()) {
    return core::Err(invalid("--pbo: the series share no session"));
  }
  ATX_TRY(const CscvReport r, cscv_report(a.matrix, a.sessions.size(), a.columns, rq.pbo_blocks));
  return core::Ok(pbo_json(r, series, a));
}

[[nodiscard]] Json conditional_row(const ConditionalEstimate &e, const std::string &name) {
  return Json{{"name", name},
              {"raw", num(e.raw)},
              {"se", num(e.se)},
              {"threshold", num(e.threshold)},
              {"alpha", num(e.alpha)},
              {"selected", e.selected},
              {"median_unbiased", opt(e.median_unbiased)},
              {"ci_lo", opt(e.ci_lo)},
              {"ci_hi", opt(e.ci_hi)},
              {"null_bias", num(e.null_bias)},
              {"clamped", e.clamped}};
}

// The lineage: the given steps, then every cell with a finite paired dSR and a positive SE.
[[nodiscard]] core::Result<Json> conditional_block(const EvalRequest &rq,
                                                   std::vector<Cell> &cells) {
  std::vector<LineageCell> lineage;
  for (const LineageStep &s : rq.steps) {
    lineage.push_back(LineageCell{s.label, s.raw, s.se, rq.threshold});
  }
  std::vector<usize> owner(lineage.size(), cells.size());
  for (usize i = 0; i < cells.size(); ++i) {
    const Json &p = cells[i].paired;
    if (p.is_null() || !p["dsr"].is_number() || !p["memmel_se"].is_number()) {
      continue;
    }
    const f64 se = p["memmel_se"].get<f64>();
    if (!(se > 0.0)) {
      continue;
    }
    lineage.push_back(LineageCell{cells[i].series.label, p["dsr"].get<f64>(), se, rq.threshold});
    owner.push_back(i);
  }
  ATX_TRY(const CumulativeConditional cum, cumulative_conditional(lineage, rq.conditional_alpha));
  Json rows = Json::array();
  for (usize k = 0; k < cum.rows.size(); ++k) {
    Json row = conditional_row(cum.rows[k], lineage[k].name);
    row["source"] = owner[k] < cells.size() ? "cell" : "step";
    if (owner[k] < cells.size()) {
      cells[owner[k]].conditional = row;
    }
    rows.push_back(std::move(row));
  }
  return core::Ok(Json{{"threshold", rq.threshold},
                       {"alpha", rq.conditional_alpha},
                       {"se_basis", "memmel_se (Memmel 2003) of the paired dSR"},
                       {"estimator", "Andrews, Kitagawa & McCloskey (2024) median-unbiased, "
                                     "equal-tailed conditional interval; printed only"},
                       {"rows", std::move(rows)},
                       {"cumulative",
                        Json{{"cells", cum.cells},
                             {"raw_sum", num(cum.raw_sum)},
                             {"adjusted_sum", opt(cum.adjusted_sum)},
                             {"null_bias_sum", num(cum.null_bias_sum)}}}});
}

[[nodiscard]] core::Status paired_blocks(const EvalRequest &rq, std::vector<Cell> &cells) {
  SeriesCache refs;
  for (Cell &c : cells) {
    const std::string *of = find_label(rq.paired_of, c.series.label);
    const std::string *ref = of != nullptr ? of : (rq.paired ? &*rq.paired : nullptr);
    if (ref == nullptr) {
      continue;
    }
    ATX_TRY(const NetSeries *r, refs.get(*ref));
    const std::array<const NetSeries *, 2> pair = {&c.series, r};
    const Aligned a = align_many(pair);
    if (a.sessions.size() < 3U) {
      c.paired_note = "paired skipped: " + std::to_string(a.sessions.size()) +
                      " session(s) in common with the reference (3 needed)";
      continue;
    }
    std::vector<f64> x(a.sessions.size());
    std::vector<f64> y(a.sessions.size());
    for (usize t = 0; t < a.sessions.size(); ++t) {
      x[t] = a.matrix[t * 2U];
      y[t] = a.matrix[t * 2U + 1U];
    }
    ATX_TRY(const PairedStats p, paired_stats(x, y, rq.paired_config));
    c.paired = paired_json(p, *ref);
  }
  return core::Ok();
}

[[nodiscard]] core::Status ledger_blocks(const EvalRequest &rq, const LedgerState &s,
                                         std::vector<Cell> &cells) {
  const std::set<std::string> history(rq.history.begin(), rq.history.end());
  for (Cell &c : cells) {
    const std::string *tid = find_label(rq.trials, c.series.label);
    const bool in_ledger = tid != nullptr && s.ledgered.contains(*tid);
    const bool hist = history.contains(c.series.label);
    ATX_TRY(Json house, house_v1_json(c.moments, s.file.records, in_ledger, hist, rq.window_id));
    ATX_TRY(Json tot, ledger_rows_json(c.moments, s.file.records, in_ledger, rq.window_id));
    c.dsr["house_v1"] = std::move(house);
    c.dsr["tot"] = std::move(tot);
  }
  return core::Ok();
}

// dsr_total.hand_row: the book's rows on the ledger prefix ending at its own construction line.
[[nodiscard]] core::Result<Json> hand_block(const EvalRequest &rq, const LedgerState &s) {
  Json out = Json::array();
  for (const HandBook &h : rq.hands) {
    ATX_TRY(const NetSeries series, read_net_series(h.trial_id, std::filesystem::path(h.path)));
    const HouseMoments m = house_moments(series.nets);
    const std::optional<usize> k = lg::construction_prefix_length(s.file.records, h.trial_id);
    if (!k) {
      return core::Err(invalid("--dsr-hand: trial " + h.trial_id +
                               " has no construction line in the ledger (DSR_hand is read on the "
                               "ledger prefix that ends at the book's own line; ledger the book "
                               "first)"));
    }
    const lg::Records prefix = std::span<const lg::Record>(s.file.records).first(*k);
    ATX_TRY(Json row, ledger_rows_json(m, prefix, true, rq.window_id));
    ATX_TRY(const lg::ChainHead head,
            lg::chain_head(std::span<const std::string>(s.file.lines).first(*k)));
    row["trial_id"] = h.trial_id;
    row["prefix_lines"] = *k;
    row["prefix_chain_head"] = head.head_sha256;
    row["path"] = h.path;
    row["net_moments"] = moments_json(m);
    out.push_back(std::move(row));
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] Json config_json(const EvalRequest &rq) {
  return Json{{"protocol", rq.protocol},
              {"draws", rq.paired_config.draws},
              {"block", rq.paired_config.block},
              {"seed", rq.paired_config.seed},
              {"one_sided", rq.paired_config.one_sided},
              {"dsr_n", rq.dsr_n},
              {"psr_benchmarks", rq.psr_benchmarks},
              {"psr_alpha", rq.psr_alpha},
              {"onc", rq.onc},
              {"onc_init", rq.onc_init},
              {"onc_seed", rq.onc_seed},
              {"pbo", rq.pbo},
              {"pbo_blocks", rq.pbo_blocks},
              {"window_id", rq.window_id},
              {"threshold", rq.threshold},
              {"conditional_alpha", rq.conditional_alpha}};
}

[[nodiscard]] Json cell_json(const Cell &c) {
  Json out{{"label", c.series.label},
           {"path", c.series.path},
           {"net_moments", moments_json(c.moments)},
           {"psr", psr_json(c.psr)},
           {"min_trl", min_trl_json(c.psr)},
           {"dsr", c.dsr},
           {"paired", c.paired},
           {"conditional", c.conditional}};
  if (c.paired_note) {
    out["paired_note"] = *c.paired_note;
  }
  return out;
}

[[nodiscard]] core::Result<Json> evaluate_checked(const EvalRequest &rq) {
  ATX_TRY_VOID(check_request(rq));
  std::vector<Cell> cells;
  std::vector<NetSeries> cell_series;
  cells.reserve(rq.daily.size());
  for (const LabeledPath &d : rq.daily) {
    ATX_TRY(NetSeries s, read_net_series(d.label, std::filesystem::path(d.path)));
    Cell c;
    c.moments = house_moments(s.nets);
    c.psr = psr_report(s.nets, rq.psr_benchmarks, rq.psr_alpha);
    cell_series.push_back(s);
    c.series = std::move(s);
    cells.push_back(std::move(c));
  }
  std::vector<HouseMoments> moments;
  moments.reserve(cells.size());
  for (const Cell &c : cells) {
    moments.push_back(c.moments);
  }
  const std::vector<DsrRow> cross = dsr_rows(moments, rq.dsr_n);
  for (usize i = 0; i < cells.size(); ++i) {
    cells[i].dsr["cross"] = cross_json(cross[i], cells[i].moments);
    cells[i].dsr["lo"] = lo_json(lo_null_dsr(cells[i].moments, rq.dsr_n));
  }
  Json doc{{"schema", std::string(kEvalSchema)}, {"config", config_json(rq)}, {"ledger", nullptr},
           {"onc", nullptr},        {"pbo", nullptr},             {"hand", Json::array()}};
  ATX_TRY_VOID(paired_blocks(rq, cells));
  if (rq.ledger) {
    ATX_TRY(const LedgerState s, load_ledger(rq));
    doc["ledger"] = ledger_doc(s, rq.window_id);
    ATX_TRY_VOID(ledger_blocks(rq, s, cells));
    ATX_TRY(Json hands, hand_block(rq, s));
    doc["hand"] = std::move(hands);
  }
  if (rq.onc) {
    ATX_TRY(Json onc, onc_block(rq, cells, cell_series));
    doc["onc"] = std::move(onc);
  }
  if (rq.pbo) {
    ATX_TRY(Json pbo, pbo_block(rq, cell_series));
    doc["pbo"] = std::move(pbo);
  }
  ATX_TRY(Json conditional, conditional_block(rq, cells));
  doc["conditional"] = std::move(conditional);
  Json rows = Json::array();
  for (const Cell &c : cells) {
    rows.push_back(cell_json(c));
  }
  doc["cells"] = std::move(rows);
  const Json &first = doc["cells"][0];
  doc["psr"] = first["psr"];
  doc["min_trl"] = first["min_trl"];
  doc["dsr"] = first["dsr"];
  doc["paired"] = first["paired"];
  return core::Ok(std::move(doc));
}

// ------------------------------------------------------------------ text
[[nodiscard]] std::string fmt(const Json &v, const char *spec) {
  if (!v.is_number()) {
    return "na";
  }
  std::array<char, 64> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), spec, v.get<f64>());
  return n > 0 ? std::string(buf.data(), static_cast<usize>(n)) : std::string("na");
}

void cell_text(std::string &out, const Json &c) {
  const Json &m = c["net_moments"];
  out += "== " + c["label"].get<std::string>() + " (" + c["path"].get<std::string>() +
         "): T " + std::to_string(m["sessions"].get<usize>()) + " SR " +
         fmt(c["psr"]["sr_annual"], "%+.3f") + " ann, skew " + fmt(m["skew"], "%+.3f") +
         " kurtosis " + fmt(m["kurtosis"], "%.3f") + "\n";
  for (const Json &b : c["psr"]["benchmarks"]) {
    out += "   PSR vs SR* " + fmt(b["sr_star_annual"], "%+.2f") + " ann: " +
           fmt(b["psr"], "%.4f") + " | MinTRL " + fmt(b["min_trl_sessions"], "%.0f") +
           " sessions (" + fmt(b["min_trl_years"], "%.2f") + " y)\n";
  }
  const Json &d = c["dsr"];
  out += "   DSR cross (N=" + std::to_string(d["cross"]["n"].get<usize>()) + ") " +
         fmt(d["cross"]["dsr"], "%.4f") + " | lo " + fmt(d["lo"]["dsr"], "%.4f");
  if (d.contains("house_v1")) {
    out += " | house_v1 (N=" + std::to_string(d["house_v1"]["n"].get<i64>()) + ") " +
           fmt(d["house_v1"]["dsr"], "%.4f") + " | tot (N_tot=" +
           std::to_string(d["tot"]["tot"]["n"].get<i64>()) + ") " +
           fmt(d["tot"]["tot"]["dsr"], "%.4f");
  }
  if (d.contains("onc")) {
    out += " | onc (N_eff=" + std::to_string(d["onc"]["n_eff"].get<usize>()) + ") " +
           fmt(d["onc"]["dsr"], "%.4f");
  }
  out += "\n";
  const Json &p = c["paired"];
  if (!p.is_null()) {
    out += "   paired vs " + p["reference"].get<std::string>() + ": dSR " +
           fmt(p["dsr"], "%+.3f") + " Memmel SE " + fmt(p["memmel_se"], "%.3f") + " | LW SE " +
           fmt(p["se"], "%.3f") + " p two-sided " + fmt(p["p_two"], "%.4f") + " one-sided " +
           fmt(p["p_one"], "%.4f") + " | CBB 95% [" + fmt(p["cbb_ci95"][0], "%+.3f") + ", " +
           fmt(p["cbb_ci95"][1], "%+.3f") + "]\n";
    if (p["degenerate"].get<bool>()) {
      out += "   paired: degenerate pair (1 - rho " + fmt(p["one_minus_rho"], "%.3e") +
             " < 1e-06): SE, t and p refused (ruling B2-R1)\n";
    }
  } else if (c.contains("paired_note")) {
    out += "   " + c["paired_note"].get<std::string>() + "\n";
  }
  const Json &q = c["conditional"];
  if (!q.is_null()) {
    out += "   conditional (printed only): raw " + fmt(q["raw"], "%+.3f") +
           " median-unbiased " + fmt(q["median_unbiased"], "%+.3f") + " CI [" +
           fmt(q["ci_lo"], "%+.3f") + ", " + fmt(q["ci_hi"], "%+.3f") + "] null bias " +
           fmt(q["null_bias"], "%.3f") + "\n";
  }
}

} // namespace

core::Result<nlohmann::json> evaluate(const EvalRequest &request) {
  try {
    return evaluate_checked(request);
  } catch (const nlohmann::json::exception &e) {
    return core::Err(core::ErrorCode::ParseError, std::string("atx-research-eval: ") + e.what());
  } catch (const std::bad_alloc &) {
    return core::Err(core::ErrorCode::OutOfRange, "atx-research-eval: allocation failed");
  } catch (const std::exception &e) {
    return core::Err(core::ErrorCode::Internal, std::string("atx-research-eval: ") + e.what());
  }
}

std::string eval_text(const nlohmann::json &doc) {
  std::string out;
  if (!doc["ledger"].is_null()) {
    const Json &l = doc["ledger"];
    out += "== ledger " + l["path"].get<std::string>() + ": " +
           std::to_string(l["lines"].get<usize>()) + " lines, chain head " +
           l["chain_head"].get<std::string>().substr(0, 16) + " (" +
           l["chain_rule"].get<std::string>() + "), window " +
           l["window_id"].get<std::string>() + "\n";
  }
  for (const Json &c : doc["cells"]) {
    cell_text(out, c);
  }
  const Json &cum = doc["conditional"]["cumulative"];
  out += "== conditional (Andrews-Kitagawa-McCloskey; printed only): " +
         std::to_string(cum["cells"].get<usize>()) + " selected cell(s), raw sum " +
         fmt(cum["raw_sum"], "%+.3f") + " -> adjusted " + fmt(cum["adjusted_sum"], "%+.3f") +
         " (null bias " + fmt(cum["null_bias_sum"], "%.3f") + ")\n";
  if (!doc["onc"].is_null()) {
    const Json &o = doc["onc"];
    out += "== effective N (house ONC): " + std::to_string(o["n_trials"].get<usize>()) +
           " series, " + std::to_string(o["sessions"].get<usize>()) + " sessions -> N_eff " +
           std::to_string(o["n_eff"].get<usize>()) + "; V[SR_k] " +
           fmt(o["variance_sr"], "%.3e") + "\n";
  }
  if (!doc["pbo"].is_null()) {
    const Json &p = doc["pbo"];
    out += "== CSCV PBO over " + std::to_string(p["n_candidates"].get<usize>()) + " cells: PBO " +
           fmt(p["pbo"], "%.4f") + " | " + std::to_string(p["splits"].get<usize>()) +
           " splits | logit mean " + fmt(p["logit_mean"], "%+.3f") + "\n";
  }
  for (const Json &h : doc["hand"]) {
    out += "== DSR_hand " + h["trial_id"].get<std::string>() + ": prefix " +
           std::to_string(h["prefix_lines"].get<usize>()) + " lines (chain head " +
           h["prefix_chain_head"].get<std::string>().substr(0, 16) + "), N_tot " +
           std::to_string(h["tot"]["n"].get<i64>()) + " DSR " + fmt(h["tot"]["dsr"], "%.4f") +
           "\n";
  }
  return out;
}

} // namespace atx::engine::research::eval
