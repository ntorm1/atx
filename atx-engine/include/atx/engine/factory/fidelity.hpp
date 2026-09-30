#pragma once

// atx::engine::factory — multi-fidelity racing (L3 search throughput).
//
// Most GP candidates are bad, and a bad candidate is as expensive to score at
// full fidelity (every date x every instrument x every CPCV fold) as a good one.
// Successive halving (Jamieson & Talwalkar 2016; the synchronous core of
// Hyperband / ASHA) spends a cheap budget on everyone and the full budget on the
// few that survive:
//
//   rung r = 0 .. R-1 : score every live candidate at rung r's fidelity;
//                        for r < R-1 keep the best ceil(eta * live) (>= min_keep),
//                        ordered by (score desc, canon_hash asc, index asc);
//                        a NaN score is a rejection at that rung.
//
// A Rung subsamples the panel (every `date_stride`-th date, every
// `inst_stride`-th instrument) and optionally overrides the CPCV fold count
// (`n_folds`, 0 = keep). Every rung evaluation is a TRIAL: the caller must add
// `RaceResult::n_evals` (and the rejected candidates) to its multiple-testing
// count — a candidate killed at rung 0 was still looked at.
//
// DETERMINISM: the evaluator is invoked per (candidate, rung) into a pre-sized
// slot (optionally on a DetPool, single-writer slots); promotion is a serial
// sort with a total, value-based key. The result is independent of worker count
// and of the order the evaluator calls complete.

#include <algorithm> // std::max (strided_cells)
#include <array>
#include <functional>
#include <span>
#include <utility> // std::move (strided_cells)
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/parallel/det_pool.hpp"

#include "atx/engine/factory/genome.hpp"

namespace atx::engine::factory {

using GenomeId = atx::u32;

struct Rung {
  atx::u32 date_stride{1};
  atx::u32 inst_stride{1};
  atx::u32 n_folds{0}; // CPCV n_groups override at this rung; 0 = keep the run's

  [[nodiscard]] bool full() const noexcept { return date_stride <= 1 && inst_stride <= 1; }
};

struct FidelityCfg {
  bool enabled{false};
  // Default ladder: 1/16 of the cells, then 1/4, then full fidelity. With
  // eta = 1/3 the expected cell budget is 1/16 + 1/12 + 1/9 ~= 0.26 of a full
  // pass per candidate (factory_throughput_bench mode 4 measured 4.4x genomes/s
  // single-worker at equal best raw fitness vs the no-fidelity baseline).
  std::array<Rung, 3> rungs{{Rung{4, 4, 0}, Rung{2, 2, 0}, Rung{1, 1, 0}}};
  atx::f64 eta{1.0 / 3.0};  // promoted fraction per rung
  atx::usize min_keep{2};   // never promote fewer than this many (if available)
  atx::usize min_batch{6};  // race only batches at least this large
};

// Score of candidate `g` at rung `rung_idx` (higher is better; NaN = reject).
// `wid` is the DetPool worker running the call (0 when serial) so an evaluator
// can reuse per-worker state (an Engine bound to the rung's sub-panel).
using RungEvaluator =
    std::function<atx::f64(const Genome &g, atx::usize rung_idx, const Rung &, atx::usize wid)>;

struct RaceResult {
  std::vector<GenomeId> survivors;        // alive after the last rung, ascending id
  std::vector<atx::f64> last_score;       // per input: score at the last rung it reached
  std::vector<atx::u8> last_rung;         // per input: index of the last rung it reached
  atx::usize n_evals{0};                  // total evaluator calls == trials consumed
  std::array<atx::usize, 3> evals_per_rung{};
  atx::usize n_rejected{0};               // inputs not in `survivors`
};

// Number of candidates promoted out of a rung with `live` candidates.
[[nodiscard]] atx::usize promote_count(atx::usize live, const FidelityCfg &cfg) noexcept;

// Race `cands` through rungs [0, n_rungs) (n_rungs <= 3). `pool` (optional)
// fans each rung's evaluations out; nullptr runs them serially.
// `promote_after_last` also applies the eta cut after the final raced rung —
// the driver races only the LOW rungs and hands the promoted set to its own
// full-fidelity pass.
[[nodiscard]] RaceResult race(std::span<const Genome> cands, const FidelityCfg &cfg,
                              const RungEvaluator &eval, atx::usize n_rungs = 3,
                              parallel::DetPool *pool = nullptr,
                              bool promote_after_last = false);

// Index of the first full-fidelity rung (the driver treats it and everything
// after it as its normal full pass). Returns 3 when no rung is full.
[[nodiscard]] atx::usize first_full_rung(const FidelityCfg &cfg) noexcept;

// A strided copy of `panel`: dates {0, s_d, 2 s_d, ...}, instruments
// {0, s_i, 2 s_i, ...}, every field and the universe mask subsampled alike.
[[nodiscard]] atx::core::Result<alpha::Panel> strided_panel(const alpha::Panel &panel,
                                                            atx::u32 date_stride,
                                                            atx::u32 inst_stride);

// ---- platform v8 H-3: racing on instrument strides only -------------------------------------
//
// A date subset changes every time-series operator (delay(x, 1) becomes delay(x, stride)) and
// lets a candidate be judged on dates it was not meant to see; an instrument subset changes only
// the cross-sections. A research search therefore races on instrument strides alone.

// The rung ladder for `strides` (1 or 2 strides, strictly decreasing, each >= 2): one low rung
// per stride keeping every date, then the full-fidelity rung. Err otherwise.
[[nodiscard]] atx::core::Result<std::array<Rung, 3>>
instrument_rungs(std::span<const atx::u32> strides);

// True iff every rung before the first full-fidelity rung keeps every date (date_stride == 1)
// and keeps the run's CPCV folds (n_folds == 0).
[[nodiscard]] bool instrument_only(const FidelityCfg &cfg) noexcept;

// A strided copy of a date-major per-cell array (dates x instruments), in strided_panel's
// geometry: dates {0, s_d, 2 s_d, ...} x instruments {0, s_i, 2 s_i, ...}. Err when `cells` is
// not dates x instruments. A cumulative per-instrument series (a return guard) stays valid under
// an instrument stride.
template <class T>
[[nodiscard]] atx::core::Result<std::vector<T>>
strided_cells(std::span<const T> cells, atx::usize dates, atx::usize instruments,
              atx::u32 date_stride, atx::u32 inst_stride) {
  if (cells.size() != dates * instruments) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "strided_cells: cell count is not dates x instruments");
  }
  const atx::usize sd = std::max<atx::u32>(date_stride, 1U);
  const atx::usize si = std::max<atx::u32>(inst_stride, 1U);
  const atx::usize out_dates = (dates + sd - 1U) / sd;
  const atx::usize out_insts = (instruments + si - 1U) / si;
  std::vector<T> out(out_dates * out_insts);
  for (atx::usize d = 0; d < out_dates; ++d) {
    for (atx::usize j = 0; j < out_insts; ++j) {
      out[d * out_insts + j] = cells[(d * sd) * instruments + j * si];
    }
  }
  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::factory
