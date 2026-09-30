#pragma once

// atx::engine::factory — the research IC fitness of a mined signal (platform v8 H-3).
//
// ResearchIcScorer reads one signal on one window of a research role with the IC runner's recipe
// and the K6 marginal kernel, so a mined expression is judged exactly as a library candidate is:
//   IC read        evaluate_research_ic on research_window_ic_config(window) with
//                  ResearchIcOptions{3, true, 1} (horizons 5/21/63, strict observed endpoints, the
//                  role's decision membership and return guard): the h 21 tied-rank IC mean, its
//                  conservative HAC standard error, t = mean / se, and the equivalence screen.
//   marginal read  per label row d: combine::marginal_rank_ic_day(the signal's centred tied rank
//                  over the decision members of d, the regressor rows at d, the h 21 label row),
//                  then combine::summarize_rank_ic of the daily marginal ICs at Bartlett lag 21.
//
// ResearchIcFitness is the SignalFitness of a research search built on two kinds of scorer:
//   full pass  screened (SignalScore::rejected) when the h 21 IC is undefined, the equivalence
//              screen calls the signal a practical null, the marginal t is undefined or the IC
//              sign is 0; otherwise f1 = |IC t|, f2 = sign x marginal t, raw = min(f1, f2) and the
//              NSGA-II objectives are {f1, f2}.
//   rung       the IC read on the rung's instrument-strided copy of the role (strided_panel,
//              strided_cells; min_names scaled by 1 / stride): raw = |IC t|, rejected when
//              undefined. The rung reads no marginal term.
// Every full-pass read is kept per worker (ResearchIcTrial) and handed back by take_trials()
// sorted by canonical hash, so the trial log does not depend on the worker count.

#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/factory/fidelity.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/ic_research.hpp"
#include "atx/engine/factory/signal_fitness.hpp"

namespace atx::engine::factory {

inline constexpr atx::usize kResearchIcHorizon = 1; // index of h 21 in the horizons {5, 21, 63}
inline constexpr atx::usize kResearchIcHacLag = 21; // the marginal read's Bartlett lag (K6)

// Decision rows [begin, end) of the panel; every label matures inside the window. min_names
// (>= 3) and min_dates are the IC recipe's and the marginal kernel's admission floors.
struct ResearchIcWindow {
  atx::usize begin{};
  atx::usize end{};
  atx::usize min_names{};
  atx::usize min_dates{};
  atx::u64 max_cache_bytes{};
};

// One read of a signal. The IC fields are NaN (and sign 0) when the h 21 estimate is undefined;
// the marginal fields are NaN when no marginal term was read or it is undefined.
struct ResearchIcRead {
  IcScreenReason reason{IcScreenReason::Disabled};
  bool ic_defined{};
  atx::f64 ic_mean = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 ic_se = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 ic_t = std::numeric_limits<atx::f64>::quiet_NaN();
  int sign{};
  atx::f64 marginal_mean = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 marginal_t = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::usize marginal_dates{};
  atx::usize spanned_dates{};
};

// f1 = |IC t| and f2 = sign x marginal t (NaN when undefined).
[[nodiscard]] atx::f64 research_ic_f1(const ResearchIcRead &read) noexcept;
[[nodiscard]] atx::f64 research_ic_f2(const ResearchIcRead &read) noexcept;

// Why a full-pass read is screened (None: a scored trial).
enum class ResearchIcScreen : atx::u8 {
  None,
  IcUndefined,
  PracticalNull,
  MarginalUndefined,
  NoSign,
};
[[nodiscard]] ResearchIcScreen research_ic_screen(const ResearchIcRead &read) noexcept;
[[nodiscard]] std::string_view research_ic_screen_name(ResearchIcScreen screen) noexcept;

// One worker's reusable buffers (sized by ResearchIcScorer::bind; never shared).
struct ResearchIcWorkspace {
  ResearchIcScratch ic;
  combine::MarginalRankIcScratch marginal;
  std::vector<std::pair<atx::f64, atx::usize>> sorted;
  std::vector<atx::f64> rank_row;
  std::vector<atx::f64> daily;
  std::vector<atx::f64> compact;
  std::vector<std::span<const atx::f64>> regressor_rows;
};

class ResearchIcScorer {
public:
  // Prepares the recipe on `panel` (its "close" is the label price) with the decision membership
  // and return guard (panel cells each; empty: none). With `marginal`, `member` is required and
  // copied for the window's label rows, and `regressors` (panel cells each, at most
  // combine::kMaxMarginalRegressors) are BORROWED: they must outlive the scorer. `panel` and
  // `guard` need not outlive the call (the cache owns its labels).
  [[nodiscard]] static atx::core::Result<ResearchIcScorer>
  prepare(const alpha::Panel &panel, const ResearchIcWindow &window,
          std::span<const atx::u8> member, std::span<const atx::u32> guard,
          std::vector<std::span<const atx::f64>> regressors, bool marginal);

  // Adds workspaces up to `workers` (existing ones are kept).
  [[nodiscard]] atx::core::Status bind(atx::usize workers);

  // Reads `signal` (panel cells, date-major) on `worker`'s workspace; worker < bound count.
  // Concurrent calls must use distinct workers.
  [[nodiscard]] atx::core::Result<ResearchIcRead> read(std::span<const atx::f64> signal,
                                                       bool marginal, atx::usize worker);

  // The daily h 21 rank IC / marginal IC of `worker`'s last read (label_rows() values, NaN where
  // undefined), valid until that worker's next read.
  [[nodiscard]] std::span<const atx::f64> daily_rank_ic(atx::usize worker) const noexcept;
  [[nodiscard]] std::span<const atx::f64> daily_marginal_ic(atx::usize worker) const noexcept;

  [[nodiscard]] atx::usize first_row() const noexcept { return cache_.first_date(); }
  [[nodiscard]] atx::usize label_rows() const noexcept {
    return cache_.label_rows(kResearchIcHorizon);
  }

private:
  ResearchIcScorer() = default;
  [[nodiscard]] atx::core::Status read_marginal(std::span<const atx::f64> signal,
                                                ResearchIcWorkspace &work,
                                                ResearchIcRead &out) const;

  ResearchIcCache cache_;
  std::vector<atx::u8> member_rows_; // label rows x instruments (marginal only)
  std::vector<std::span<const atx::f64>> regressors_;
  atx::usize min_names_{};
  bool marginal_{};
  std::vector<ResearchIcWorkspace> workspaces_;
};

// One full-pass read of a candidate on the fitness's window.
struct ResearchIcTrial {
  atx::u64 canon_hash{};
  ResearchIcRead read{};
  ResearchIcScreen screen{ResearchIcScreen::None};
  std::vector<atx::f64> daily_rank_ic{}; // h 21 rank IC per label row, unoriented (NaN undefined)
};

// Everything BORROWED must outlive the fitness: the search panel, the decision membership
// (panel cells), the return guard (panel cells; may be empty) and the regressor columns.
struct ResearchIcFitnessInputs {
  const alpha::Panel *panel{};
  ResearchIcWindow window{};
  std::span<const atx::u8> member{};
  std::span<const atx::u32> guard{};
  std::vector<std::span<const atx::f64>> regressors{};
};

class ResearchIcFitness final : public SignalFitness {
public:
  [[nodiscard]] static atx::core::Result<ResearchIcFitness> prepare(ResearchIcFitnessInputs inputs);

  ResearchIcFitness(ResearchIcFitness &&) noexcept = default;
  ResearchIcFitness &operator=(ResearchIcFitness &&) noexcept = default;
  ~ResearchIcFitness() override = default;

  // Checks the run's geometry, (re)prepares the rung scorers when the rung strides changed and
  // sizes every scorer's workspaces. Pending trials are kept until take_trials().
  [[nodiscard]] atx::core::Status bind(const SignalFitnessBinding &binding) override;
  [[nodiscard]] atx::core::Result<SignalScore> score(const Genome &genome,
                                                     std::span<const atx::f64> signal,
                                                     const SignalLevel &level,
                                                     atx::usize worker) override;

  // Every full-pass read since the last call, sorted by canonical hash; clears them.
  [[nodiscard]] std::vector<ResearchIcTrial> take_trials();

private:
  struct RungScorer {
    atx::u32 stride{};
    ResearchIcScorer scorer;
  };
  ResearchIcFitness(ResearchIcFitnessInputs inputs, ResearchIcScorer full);
  [[nodiscard]] atx::core::Status prepare_rungs(std::span<const Rung> low);
  [[nodiscard]] atx::core::Result<SignalScore> score_rung(std::span<const atx::f64> signal,
                                                          const SignalLevel &level,
                                                          atx::usize worker);

  ResearchIcFitnessInputs inputs_;
  ResearchIcScorer full_;
  std::vector<RungScorer> rungs_;
  std::vector<std::vector<ResearchIcTrial>> trials_; // per worker
};

} // namespace atx::engine::factory
