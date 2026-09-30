// atx::engine::factory — multi-fidelity racing out-of-line definitions (L3).
#include "atx/engine/factory/fidelity.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace atx::engine::factory {

[[nodiscard]] atx::usize promote_count(atx::usize live, const FidelityCfg &cfg) noexcept {
  if (live == 0) {
    return 0;
  }
  const atx::f64 eta = std::clamp(cfg.eta, 0.0, 1.0);
  auto keep = static_cast<atx::usize>(std::ceil(eta * static_cast<atx::f64>(live)));
  keep = std::max(keep, cfg.min_keep);
  return std::min(keep, live);
}

[[nodiscard]] atx::usize first_full_rung(const FidelityCfg &cfg) noexcept {
  for (atx::usize r = 0; r < cfg.rungs.size(); ++r) {
    if (cfg.rungs[r].full()) {
      return r;
    }
  }
  return cfg.rungs.size();
}

atx::core::Result<std::array<Rung, 3>> instrument_rungs(std::span<const atx::u32> strides) {
  if (strides.empty() || strides.size() > 2U || strides[0] < 2U ||
      (strides.size() == 2U && (strides[1] < 2U || strides[1] >= strides[0]))) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "instrument_rungs: 1 or 2 strictly decreasing instrument strides >= 2");
  }
  std::array<Rung, 3> out{{Rung{1, 1, 0}, Rung{1, 1, 0}, Rung{1, 1, 0}}};
  for (atx::usize r = 0; r < strides.size(); ++r) {
    out[r] = Rung{1, strides[r], 0};
  }
  return atx::core::Ok(out);
}

bool instrument_only(const FidelityCfg &cfg) noexcept {
  const atx::usize n_low = first_full_rung(cfg);
  for (atx::usize r = 0; r < n_low; ++r) {
    if (cfg.rungs[r].date_stride != 1U || cfg.rungs[r].n_folds != 0U) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] RaceResult race(std::span<const Genome> cands, const FidelityCfg &cfg,
                              const RungEvaluator &eval, atx::usize n_rungs,
                              parallel::DetPool *pool, bool promote_after_last) {
  RaceResult res;
  const atx::usize n = cands.size();
  res.last_score.assign(n, 0.0);
  res.last_rung.assign(n, atx::u8{0});
  n_rungs = std::min(n_rungs, cfg.rungs.size());

  std::vector<GenomeId> live(n);
  for (atx::usize i = 0; i < n; ++i) {
    live[i] = static_cast<GenomeId>(i);
  }
  std::vector<atx::f64> slot;
  for (atx::usize r = 0; r < n_rungs && !live.empty(); ++r) {
    const Rung &rung = cfg.rungs[r];
    slot.assign(live.size(), 0.0);
    // SAFETY: shard p writes only slot[p]; cands / rung / eval are read-only
    // (the evaluator's own contract is to be reentrant).
    auto body = [&](atx::usize p, atx::usize wid) {
      slot[p] = eval(cands[live[p]], r, rung, wid);
    };
    if (pool != nullptr) {
      pool->parallel_for(live.size(), body);
    } else {
      for (atx::usize p = 0; p < live.size(); ++p) {
        body(p, 0);
      }
    }
    res.n_evals += live.size();
    res.evals_per_rung[r] = live.size();
    std::vector<GenomeId> next;
    next.reserve(live.size());
    for (atx::usize p = 0; p < live.size(); ++p) {
      res.last_score[live[p]] = slot[p];
      res.last_rung[live[p]] = static_cast<atx::u8>(r);
      if (!std::isnan(slot[p])) {
        next.push_back(live[p]);
      }
    }
    if (r + 1 < n_rungs || promote_after_last) {
      // Promotion order: score desc, canon_hash asc, input index asc (total).
      std::sort(next.begin(), next.end(), [&](GenomeId a, GenomeId b) {
        const atx::f64 sa = res.last_score[a];
        const atx::f64 sb = res.last_score[b];
        if (sa != sb) {
          return sa > sb;
        }
        if (cands[a].canon_hash != cands[b].canon_hash) {
          return cands[a].canon_hash < cands[b].canon_hash;
        }
        return a < b;
      });
      next.resize(promote_count(next.size(), cfg));
      std::sort(next.begin(), next.end());
    }
    live = std::move(next);
  }
  res.survivors = std::move(live);
  res.n_rejected = n - res.survivors.size();
  return res;
}

[[nodiscard]] atx::core::Result<alpha::Panel> strided_panel(const alpha::Panel &panel,
                                                            atx::u32 date_stride,
                                                            atx::u32 inst_stride) {
  const atx::usize sd = std::max<atx::u32>(date_stride, 1U);
  const atx::usize si = std::max<atx::u32>(inst_stride, 1U);
  const atx::usize dates = (panel.dates() + sd - 1) / sd;
  const atx::usize insts = (panel.instruments() + si - 1) / si;
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  names.reserve(panel.num_fields());
  cols.reserve(panel.num_fields());
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    names.push_back(panel.field_name(f));
    const std::span<const atx::f64> src = panel.field_all(static_cast<alpha::FieldId>(f));
    std::vector<atx::f64> col(dates * insts);
    for (atx::usize d = 0; d < dates; ++d) {
      for (atx::usize j = 0; j < insts; ++j) {
        col[d * insts + j] = src[(d * sd) * panel.instruments() + j * si];
      }
    }
    cols.push_back(std::move(col));
  }
  std::vector<std::uint8_t> universe(dates * insts);
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < insts; ++j) {
      universe[d * insts + j] = panel.in_universe(d * sd, j * si) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return alpha::Panel::create(dates, insts, std::move(names), std::move(cols), std::move(universe));
}

} // namespace atx::engine::factory
