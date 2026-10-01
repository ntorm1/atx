// atx::engine::factory — the research IC fitness of a mined signal (platform v8 H-3). Contracts
// live in factory/research_ic_fitness.hpp.

#include "atx/engine/factory/research_ic_fitness.hpp"

#include <algorithm> // std::any_of, std::max, std::min, std::stable_sort
#include <cmath>     // std::abs, std::isfinite
#include <limits>    // std::numeric_limits
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

namespace atx::engine::factory {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
} // namespace

atx::f64 research_ic_f1(const ResearchIcRead &read) noexcept {
  return read.ic_defined ? std::abs(read.ic_t) : kNaN;
}

atx::f64 research_ic_f2(const ResearchIcRead &read) noexcept {
  return read.sign != 0 && std::isfinite(read.marginal_t)
             ? static_cast<atx::f64>(read.sign) * read.marginal_t
             : kNaN;
}

ResearchIcScreen research_ic_screen(const ResearchIcRead &read) noexcept {
  if (!read.ic_defined) {
    return ResearchIcScreen::IcUndefined;
  }
  if (read.reason == IcScreenReason::PracticalNull) {
    return ResearchIcScreen::PracticalNull;
  }
  if (!std::isfinite(read.marginal_t)) {
    return ResearchIcScreen::MarginalUndefined;
  }
  if (read.sign == 0) {
    return ResearchIcScreen::NoSign;
  }
  return ResearchIcScreen::None;
}

std::string_view research_ic_screen_name(ResearchIcScreen screen) noexcept {
  switch (screen) {
  case ResearchIcScreen::None:
    return "none";
  case ResearchIcScreen::IcUndefined:
    return "ic-undefined";
  case ResearchIcScreen::PracticalNull:
    return "practical-null";
  case ResearchIcScreen::MarginalUndefined:
    return "marginal-undefined";
  case ResearchIcScreen::NoSign:
    return "no-sign";
  }
  return "unknown";
}

// ---- ResearchIcScorer ---------------------------------------------------------------------------

atx::core::Result<ResearchIcScorer>
ResearchIcScorer::prepare(const alpha::Panel &panel, const ResearchIcWindow &window,
                          std::span<const atx::u8> member, std::span<const atx::u32> guard,
                          std::vector<std::span<const atx::f64>> regressors, bool marginal) {
  const atx::usize cells = panel.cells();
  const bool ragged =
      std::any_of(regressors.begin(), regressors.end(),
                  [cells](std::span<const atx::f64> column) { return column.size() != cells; });
  if ((!member.empty() && member.size() != cells) || (!guard.empty() && guard.size() != cells) ||
      ragged || regressors.size() > combine::kMaxMarginalRegressors ||
      (marginal && member.empty())) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC scorer: membership, guard and regressors are panel cells "
                          "each (at most 11 regressors; the marginal term needs the membership)");
  }
  if (window.begin >= window.end || window.end > panel.dates() || window.min_names < 3U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC scorer: window rows [begin, end) inside the panel, "
                          "min_names >= 3");
  }
  ResearchIcScorer out;
  const IcScreenConfig config = research_window_ic_config(
      window.begin, window.end, window.min_names, window.min_dates, window.max_cache_bytes);
  ATX_TRY(out.cache_, prepare_research_ic(panel, config, ResearchIcOptions{3, true, 1}, member,
                                          guard));
  out.min_names_ = window.min_names;
  out.marginal_ = marginal;
  if (marginal) {
    const atx::usize n = panel.instruments();
    const auto rows = member.subspan(out.cache_.first_date() * n, out.label_rows() * n);
    out.member_rows_.assign(rows.begin(), rows.end());
    out.regressors_ = std::move(regressors);
  }
  return out;
}

atx::core::Status ResearchIcScorer::bind(atx::usize workers) {
  if (workers == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC scorer: at least one worker");
  }
  const atx::usize n = cache_.instruments();
  while (workspaces_.size() < workers) {
    ResearchIcWorkspace work;
    ATX_TRY(work.ic, prepare_research_ic_scratch(cache_));
    work.rank_row.assign(n, kNaN);
    work.daily.assign(label_rows(), kNaN);
    work.regressor_rows.resize(regressors_.size());
    workspaces_.push_back(std::move(work));
  }
  return atx::core::Ok();
}

atx::core::Result<ResearchIcRead> ResearchIcScorer::read(std::span<const atx::f64> signal,
                                                         bool marginal, atx::usize worker) {
  if (worker >= workspaces_.size() || (marginal && !marginal_)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC scorer: unbound worker, or no marginal term prepared");
  }
  ResearchIcWorkspace &work = workspaces_[worker];
  ATX_TRY(const auto result, evaluate_research_ic(signal, cache_, work.ic));
  ResearchIcRead out;
  out.reason = result.screen.reason;
  const IcScreenEstimate &rank = result.screen.horizons[kResearchIcHorizon].rank;
  out.ic_defined = rank.defined;
  out.ic_dates = rank.valid_dates;
  if (rank.defined) {
    out.ic_mean = rank.mean;
    out.ic_se = rank.standard_error;
    out.ic_t = rank.mean / rank.standard_error;
    out.sign = rank.mean > 0.0 ? 1 : (rank.mean < 0.0 ? -1 : 0);
  }
  if (marginal) {
    ATX_TRY_VOID(read_marginal(signal, work, out));
  } else {
    std::fill(work.daily.begin(), work.daily.end(), kNaN);
  }
  return out;
}

// Row by row over the label rows: the signal's centred tied rank over the decision members, the
// regressor rows, the h 21 label row (the cache's own labels, so both reads share one label).
atx::core::Status ResearchIcScorer::read_marginal(std::span<const atx::f64> signal,
                                                  ResearchIcWorkspace &work,
                                                  ResearchIcRead &out) const {
  const atx::usize n = cache_.instruments();
  const atx::usize rows = label_rows();
  const atx::usize first = cache_.first_date();
  const std::span<const atx::f64> labels = cache_.labels(kResearchIcHorizon);
  const std::span<const atx::u8> members{member_rows_};
  atx::usize spanned = 0;
  for (atx::usize row = 0; row < rows; ++row) {
    const atx::usize d = first + row;
    ATX_TRY_VOID(combine::centred_tied_ranks(signal.subspan(d * n, n), members.subspan(row * n, n),
                                             work.rank_row, work.sorted));
    for (atx::usize k = 0; k < regressors_.size(); ++k) {
      work.regressor_rows[k] = regressors_[k].subspan(d * n, n);
    }
    ATX_TRY(const auto day,
            combine::marginal_rank_ic_day(work.rank_row, work.regressor_rows,
                                          labels.subspan(row * n, n), min_names_, work.marginal));
    work.daily[row] = day.marginal_ic;
    spanned += day.spanned;
  }
  const combine::RankIcSummary summary =
      combine::summarize_rank_ic(work.daily, kResearchIcHacLag, work.compact);
  out.marginal_mean = summary.mean;
  out.marginal_t = summary.hac_t;
  out.marginal_dates = summary.dates;
  out.spanned_dates = spanned;
  return atx::core::Ok();
}

std::span<const atx::f64> ResearchIcScorer::daily_rank_ic(atx::usize worker) const noexcept {
  return worker < workspaces_.size() ? workspaces_[worker].ic.rank_series(kResearchIcHorizon)
                                     : std::span<const atx::f64>{};
}

std::span<const atx::f64> ResearchIcScorer::daily_marginal_ic(atx::usize worker) const noexcept {
  return worker < workspaces_.size() ? std::span<const atx::f64>{workspaces_[worker].daily}
                                     : std::span<const atx::f64>{};
}

// ---- ResearchIcFitness --------------------------------------------------------------------------

ResearchIcFitness::ResearchIcFitness(ResearchIcFitnessInputs inputs, ResearchIcScorer full)
    : inputs_{std::move(inputs)}, full_{std::move(full)} {}

atx::core::Result<ResearchIcFitness>
ResearchIcFitness::prepare(ResearchIcFitnessInputs inputs) {
  if (inputs.panel == nullptr) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC fitness: no search panel");
  }
  ATX_TRY(auto full, ResearchIcScorer::prepare(*inputs.panel, inputs.window, inputs.member,
                                               inputs.guard, inputs.regressors, true));
  return ResearchIcFitness{std::move(inputs), std::move(full)};
}

atx::core::Status ResearchIcFitness::bind(const SignalFitnessBinding &binding) {
  const alpha::Panel &panel = *inputs_.panel;
  if (binding.workers == 0U || binding.dates != panel.dates() ||
      binding.instruments != panel.instruments()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC fitness: the run's panel or worker count differs from "
                          "the prepared one");
  }
  ATX_TRY_VOID(prepare_rungs(binding.low_rungs));
  ATX_TRY_VOID(full_.bind(binding.workers));
  for (RungScorer &rung : rungs_) {
    ATX_TRY_VOID(rung.scorer.bind(binding.workers));
  }
  if (trials_.size() < binding.workers) {
    trials_.resize(binding.workers);
  }
  return atx::core::Ok();
}

// One scorer per low rung on the instrument-strided role, prepared again only when the strides
// change (a campaign's stages share them).
atx::core::Status ResearchIcFitness::prepare_rungs(std::span<const Rung> low) {
  for (const Rung &rung : low) {
    if (rung.date_stride != 1U || rung.n_folds != 0U || rung.inst_stride < 2U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "research IC fitness: racing rungs keep every date and fold "
                            "(instrument strides >= 2 only)");
    }
  }
  bool same = low.size() == rungs_.size();
  for (atx::usize r = 0; same && r < low.size(); ++r) {
    same = low[r].inst_stride == rungs_[r].stride;
  }
  if (same) {
    return atx::core::Ok();
  }
  rungs_.clear();
  const alpha::Panel &panel = *inputs_.panel;
  for (const Rung &rung : low) {
    ATX_TRY(auto sub, strided_panel(panel, 1U, rung.inst_stride));
    std::vector<atx::u8> member;
    if (!inputs_.member.empty()) {
      ATX_TRY(member, strided_cells(inputs_.member, panel.dates(), panel.instruments(), 1U,
                                    rung.inst_stride));
    }
    std::vector<atx::u32> guard;
    if (!inputs_.guard.empty()) {
      ATX_TRY(guard, strided_cells(inputs_.guard, panel.dates(), panel.instruments(), 1U,
                                   rung.inst_stride));
    }
    ResearchIcWindow window = inputs_.window;
    window.min_names =
        std::max<atx::usize>(3U, (window.min_names + rung.inst_stride - 1U) / rung.inst_stride);
    ATX_TRY(auto scorer, ResearchIcScorer::prepare(sub, window, member, guard, {}, false));
    rungs_.push_back(RungScorer{rung.inst_stride, std::move(scorer)});
  }
  return atx::core::Ok();
}

atx::core::Result<SignalScore> ResearchIcFitness::score(const Genome &genome,
                                                        std::span<const atx::f64> signal,
                                                        const SignalLevel &level,
                                                        atx::usize worker) {
  if (worker >= trials_.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC fitness: worker not bound");
  }
  if (!level.full) {
    return score_rung(signal, level, worker);
  }
  ATX_TRY(auto read, full_.read(signal, true, worker));
  ResearchIcTrial trial;
  trial.canon_hash = genome.canon_hash;
  trial.screen = research_ic_screen(read);
  const std::span<const atx::f64> daily = full_.daily_rank_ic(worker);
  trial.daily_rank_ic.assign(daily.begin(), daily.end());
  SignalScore out;
  out.rejected = trial.screen != ResearchIcScreen::None;
  if (!out.rejected) {
    const atx::f64 f1 = research_ic_f1(read);
    const atx::f64 f2 = research_ic_f2(read);
    out.raw = std::min(f1, f2);
    out.objectives[0] = f1;
    out.objectives[1] = f2;
    out.n_objectives = 2;
  }
  trial.read = read;
  trials_[worker].push_back(std::move(trial));
  return out;
}

atx::core::Result<SignalScore> ResearchIcFitness::score_rung(std::span<const atx::f64> signal,
                                                             const SignalLevel &level,
                                                             atx::usize worker) {
  if (level.rung >= rungs_.size() || rungs_[level.rung].stride != level.inst_stride) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "research IC fitness: rung not bound for this stride");
  }
  ATX_TRY(const auto read, rungs_[level.rung].scorer.read(signal, false, worker));
  SignalScore out;
  out.rejected = !read.ic_defined;
  if (!out.rejected) {
    out.raw = std::abs(read.ic_t);
    out.objectives[0] = out.raw;
    out.n_objectives = 1;
  }
  return out;
}

std::vector<ResearchIcTrial> ResearchIcFitness::take_trials() {
  std::vector<ResearchIcTrial> out;
  for (std::vector<ResearchIcTrial> &pending : trials_) {
    for (ResearchIcTrial &trial : pending) {
      out.push_back(std::move(trial));
    }
    pending.clear();
  }
  std::stable_sort(out.begin(), out.end(), [](const ResearchIcTrial &a, const ResearchIcTrial &b) {
    return a.canon_hash < b.canon_hash;
  });
  return out;
}

} // namespace atx::engine::factory
