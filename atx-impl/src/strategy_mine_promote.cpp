// mined-v1 applied to a campaign's evaluated trials (platform v8 H-3). The rule's arithmetic is in
// strategy_mine_rule.cpp; this file reads the signals it needs. Contracts in
// strategy_mine_detail.hpp.
#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "strategy_mine_detail.hpp"

namespace atx::impl::strategy::mine_detail {
namespace {
namespace al = atx::engine::alpha;
namespace cb = atx::engine::combine;

// The shortlist's signals, evaluated as the search evaluated them: the role's DSL panel with the
// decision membership as the cross-section mask.
co::Result<std::vector<std::vector<f64>>>
evaluate_signals(const al::Panel &panel, std::span<const u8> mask,
                 const std::vector<const ex::Genome *> &genomes) {
  al::Engine engine{panel};
  ATX_TRY_VOID(engine.set_cross_section_mask(std::vector<u8>(mask.begin(), mask.end())));
  std::vector<std::vector<f64>> out;
  for (const ex::Genome *g : genomes) {
    ATX_TRY(const auto program, al::compile(g->ast, g->analysis));
    ATX_TRY(auto signals, engine.evaluate(program));
    if (signals.alphas.empty())
      return co::Err(fail(co::ErrorCode::Internal, "a shortlisted expression gave no signal"));
    out.push_back(std::move(signals.alphas.front().values));
  }
  return co::Ok(std::move(out));
}

// Rows: the pool members, the candidates kept so far (`held`), then `batch`; each ranked over the
// decision members on every discover decision row. The batch is checked greedily against every
// earlier row (mined_rho_select), at most `slots` of it passing. Lane MINE-JOIN: the members are
// streamed date by date from their pinned payloads (stream_mine_pool_members, every payload
// verified), so one date of each is held and never a whole panel; each discover date is ranked and
// added exactly as by the in-memory pass -- the same row bytes, the same rows in the same order,
// the dates ascending -- so every pair's sums are bit for bit the same.
co::Result<std::vector<MinedRho>> rho_check(const PromotionContext &context,
                                            const std::vector<std::vector<f64>> &held,
                                            const std::vector<std::vector<f64>> &batch,
                                            usize slots) {
  const usize members = context.pool->members.size();
  const usize fixed = members + held.size();
  const usize rows = fixed + batch.size();
  const usize names = context.role->panel().instruments();
  const std::span<const u8> membership = context.role->member();
  cb::PairwiseRowCorrelation rho(rows, context.min_names);
  std::vector<std::vector<f64>> ranks(rows, std::vector<f64>(names));
  std::vector<std::span<const f64>> views;
  for (const auto &rank : ranks) views.emplace_back(rank);
  std::vector<std::pair<f64, usize>> sorted;
  // Candidate row r >= members on date d: the kept signals first, then the batch.
  const auto candidate_row = [&held, &batch, members, fixed, names](usize r, usize d) {
    const std::vector<f64> &signal = r < fixed ? held[r - members] : batch[r - fixed];
    return std::span<const f64>(signal).subspan(d * names, names);
  };
  const auto on_date = [&ranks, &rho, &views, &sorted, &candidate_row, membership, names, members,
                        rows](usize d, std::span<const std::span<const f64>> member_rows)
      -> co::Status {
    const std::span<const u8> eligible = membership.subspan(d * names, names);
    for (usize r = 0; r < rows; ++r) {
      const std::span<const f64> source = r < members ? member_rows[r] : candidate_row(r, d);
      ATX_TRY_VOID(cb::centred_tied_ranks(source, eligible, ranks[r], sorted));
    }
    return rho.add_date(views);
  };
  ATX_TRY_VOID(stream_mine_pool_members(*context.pool, context.discover->begin,
                                        context.discover->end, on_date));
  return co::Ok(mined_rho_select(rho, fixed, batch.size(), context.min_dates, slots));
}

// The candidates that reach the confirm read, in shortlist order, with their signals.
struct Kept {
  std::vector<usize> positions; // into the shortlist
  std::vector<std::vector<f64>> signals;
};

// Review MINE-14 (Ruling PM5-9): the greedy rho step over the whole shortlist, then the cap of
// context.max_promotions. Read in batches that, with the candidates kept so far, never hold more
// than max_promotions signals (what the memory admission counts): each batch is checked against
// the pool members and every kept candidate, and the step stops once max_promotions have passed.
// A pair's rho reads only its own two rows, so the batches give the values one pass over the
// whole list would; a list that fits the cap is one batch, the pre-PM5-9 pass. A candidate the
// step never reaches keeps rho.read false. Rows are renumbered to the campaign's: the pool
// members, then the shortlist (promotions_json names them). Lane MINE-JOIN: context.rho_batch (a
// test hook; 0 in the verb) bounds a batch instead of the free slots -- smaller batches check the
// same rows against the same earlier rows, and a batch wider than the free slots stops reading
// once the cap fills (mined_rho_select), as one pass over the whole list does.
co::Result<Kept> rho_step(const std::vector<MinedTrial> &trials, const PromotionContext &context,
                          std::vector<Promotion> &shortlist) {
  const usize cap = context.max_promotions;
  const usize members = context.pool->members.size();
  Kept kept;
  usize next = 0;
  while (next < shortlist.size() && kept.positions.size() < cap) {
    const usize held = kept.positions.size();
    const usize slots = cap - held;
    const usize width = context.rho_batch == 0U ? slots : context.rho_batch;
    const usize batch = std::min(width, shortlist.size() - next);
    std::vector<const ex::Genome *> genomes;
    for (usize j = 0; j < batch; ++j) genomes.push_back(trials[shortlist[next + j].trial].genome);
    ATX_TRY(auto signals,
            evaluate_signals(context.role->panel(), context.role->member(), genomes));
    ATX_TRY(const auto rho, rho_check(context, kept.signals, signals, slots));
    const auto campaign_row = [&kept, members, held, next](usize row) -> usize {
      if (row == kMinedNoRow || row < members) return row;
      if (row < members + held) return members + kept.positions[row - members];
      return members + next + (row - members - held);
    };
    for (usize j = 0; j < batch; ++j) {
      MinedRho r = rho[j];
      r.against = campaign_row(r.against);
      r.undefined = campaign_row(r.undefined);
      shortlist[next + j].rho = r;
      if (!r.pass) continue;
      kept.positions.push_back(next + j);
      kept.signals.push_back(std::move(signals[j]));
    }
    next += batch;
  }
  return co::Ok(std::move(kept));
}

struct ConfirmReads {
  std::vector<ex::ResearchIcRead> reads;
  usize label_rows{}; // the confirm window's mature h 21 label rows
};

// One confirm read per signal: the IC runner's recipe and the marginal term on the confirm
// window, against the same regressors.
co::Result<ConfirmReads> confirm_reads(const PromotionContext &context,
                                       const std::vector<std::vector<f64>> &signals) {
  const MineWindow &confirm = *context.confirm;
  const ex::ResearchIcWindow window{confirm.begin, confirm.end, context.min_names,
                                    context.min_dates, context.max_cache_bytes};
  std::vector<std::span<const f64>> regressors(context.regressors.begin(),
                                               context.regressors.end());
  const ResearchRole &role = *context.role;
  ATX_TRY(auto scorer, ex::ResearchIcScorer::prepare(role.panel(), window, role.member(),
                                                     role.guard(), std::move(regressors), true));
  ATX_TRY_VOID(scorer.bind(1U));
  ConfirmReads out;
  out.label_rows = scorer.label_rows();
  for (const std::vector<f64> &signal : signals) {
    ATX_TRY(const auto read, scorer.read(signal, true, 0U));
    out.reads.push_back(read);
  }
  return co::Ok(std::move(out));
}
} // namespace

co::Result<std::vector<Promotion>> promote(const std::vector<MinedTrial> &trials, f64 hurdle,
                                           const PromotionContext &context) {
  std::vector<usize> evaluated;
  std::vector<MinedRead> reads;
  for (usize i = 0; i < trials.size(); ++i) {
    if (trials[i].status != TrialStatus::Evaluated) continue;
    evaluated.push_back(i);
    reads.push_back(MinedRead{trials[i].canon_hash, ex::research_ic_f2(trials[i].read->read)});
  }
  // Ruling PM5-9: every trial above the hurdle; the cap applies after the rho step (rho_step).
  const std::vector<usize> shortlist = mined_shortlist(reads, hurdle, context.overlap_factor);
  std::vector<Promotion> out(shortlist.size());
  for (usize k = 0; k < shortlist.size(); ++k) out[k].trial = evaluated[shortlist[k]];
  ATX_TRY(const auto kept, rho_step(trials, context, out));
  const std::vector<usize> &passed = kept.positions;
  if (passed.empty()) return co::Ok(std::move(out));
  ATX_TRY(const auto confirms, confirm_reads(context, kept.signals));
  // Sign frozen from the discover window; a read short of its full window (review MINE-2) has no
  // t and is unconfirmed. The m reads are the candidates kept (BY and the confirm factor).
  std::vector<f64> oriented;
  for (usize j = 0; j < passed.size(); ++j) {
    Promotion &p = out[passed[j]];
    const ex::ResearchIcRead &c = confirms.reads[j];
    p.confirm_rows = confirms.label_rows;
    p.confirm_defined =
        mined_confirm_defined(c.ic_defined, c.ic_dates, c.marginal_dates, confirms.label_rows);
    const int sign = trials[p.trial].read->read.sign;
    oriented.push_back(p.confirm_defined ? static_cast<f64>(sign) * c.marginal_t
                                         : std::numeric_limits<f64>::quiet_NaN());
  }
  const std::vector<MinedConfirm> decisions = mined_confirm(oriented);
  for (usize j = 0; j < passed.size(); ++j) {
    Promotion &p = out[passed[j]];
    p.confirm_read = true;
    p.confirm = confirms.reads[j];
    p.decision = decisions[j];
    p.admitted = decisions[j].confirmed;
  }
  return co::Ok(std::move(out));
}

Json promotions_json(const std::vector<MinedTrial> &trials,
                     const std::vector<Promotion> &promotions, const MinePool &pool,
                     f64 overlap_factor) {
  // The rho rows by name: the pool members, then the shortlist.
  std::vector<std::string> rows;
  for (const auto &member : pool.members) rows.push_back("pool:" + member.name);
  for (const Promotion &p : promotions) rows.push_back(trials[p.trial].dsl);
  Json out = Json::array();
  for (const Promotion &p : promotions) {
    const MinedTrial &t = trials[p.trial];
    const ex::ResearchIcRead &r = t.read->read;
    const f64 confirm_ic_t = p.confirm_read ? static_cast<f64>(r.sign) * p.confirm.ic_t
                                            : std::numeric_limits<f64>::quiet_NaN();
    const Json against = p.rho.against < rows.size() ? Json(rows[p.rho.against]) : Json(nullptr);
    const Json undefined =
        p.rho.undefined < rows.size() ? Json(rows[p.rho.undefined]) : Json(nullptr);
    out.push_back(Json{{"canon_hash", hex16(t.canon_hash)}, {"dsl", t.dsl}, {"sign", r.sign},
                       {"f1", finite_or_null(ex::research_ic_f1(r))},
                       {"f2", finite_or_null(ex::research_ic_f2(r))},
                       {"f2_corrected", finite_or_null(mined_overlap_corrected(
                                            ex::research_ic_f2(r), overlap_factor))},
                       {"rho_read", p.rho.read}, // Ruling PM5-9: the step stops at the cap
                       {"max_abs_rho", finite_or_null(p.rho.max_abs)}, {"max_rho_row", against},
                       {"rho_undefined_row", undefined}, // Ruling PM5-8: an undefined pair fails
                       {"rho_pass", p.rho.pass}, {"confirm_read", p.confirm_read},
                       {"confirm_defined", p.confirm_defined},
                       {"confirm_rows", p.confirm_rows},
                       {"confirm_ic_dates", p.confirm.ic_dates},
                       {"confirm_marginal_dates", p.confirm.marginal_dates},
                       {"confirm_ic_t", finite_or_null(confirm_ic_t)},
                       {"confirm_marginal_t", finite_or_null(p.decision.t)},
                       {"confirm_factor", finite_or_null(p.decision.factor)},
                       {"confirm_t_corrected", finite_or_null(p.decision.t_corrected)},
                       {"p", finite_or_null(p.decision.p)},
                       {"p_by", finite_or_null(p.decision.p_by)},
                       {"confirmed", p.decision.confirmed}, {"admitted", p.admitted}});
  }
  return out;
}

Json members_json(const std::vector<MinedTrial> &trials,
                  const std::vector<Promotion> &promotions) {
  Json out = Json::array();
  const std::string theme(kMinedTheme);
  for (const Promotion &p : promotions) {
    if (!p.admitted) continue;
    const MinedTrial &t = trials[p.trial];
    out.push_back(Json{{"id", "mined_" + hex16(t.canon_hash)}, {"dsl", t.dsl},
                       {"sign", t.read->read.sign}, {"theme", theme}, {"family", theme},
                       {"origin", "mined"}, {"canon_hash", hex16(t.canon_hash)},
                       {"discover_f2", finite_or_null(ex::research_ic_f2(t.read->read))},
                       {"confirm_marginal_t", finite_or_null(p.decision.t)},
                       {"confirm_factor", finite_or_null(p.decision.factor)},
                       {"confirm_t_corrected", finite_or_null(p.decision.t_corrected)}});
  }
  return out;
}

} // namespace atx::impl::strategy::mine_detail
