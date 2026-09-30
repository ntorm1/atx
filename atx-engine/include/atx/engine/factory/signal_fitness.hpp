#pragma once

// atx::engine::factory — SignalFitness: a caller-supplied score of an evaluated signal
// (platform v8 H-3).
//
// The SearchDriver's default fitness backtests a candidate against a pool (pool_aware_fitness).
// A research search scores the signal itself instead: its forward-return IC, its marginal IC
// against a book, a screen. SignalFitness is that seam. With SearchConfig::signal_fitness set,
// the driver evaluates each distinct candidate once on its own panel (the full pass) and on each
// racing rung's instrument-strided panel, and hands the signal to score(); nothing else scores.
//
// Contract:
//   * bind() is called serially by SearchDriver::run() before any score(), once per run, with the
//     run's worker count, panel geometry and low racing rungs. It sizes per-worker state; it may
//     be called again for a later run and must keep what the caller has not yet collected.
//   * score() is called concurrently from the driver's workers; `worker` < the bound worker count
//     names the only state the call may write (worker-local state; no locks). A call must depend
//     only on (genome, signal, level) so results are worker-count invariant.
//   * score() returns Err only for a hard failure (binding, shapes); the driver then stops the
//     run (SearchResult::signal_path_invalid). A signal without a usable score is `rejected`: a
//     trial, never selected (the driver files it with the IC-screen rejections). A non-finite
//     `raw` on an accepted score is treated as unscored.

#include <array>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/factory/fidelity.hpp" // Rung
#include "atx/engine/factory/fitness.hpp"  // kMaxObjectives
#include "atx/engine/factory/genome.hpp"

namespace atx::engine::factory {

// Where a signal was evaluated: the full pass (the driver's panel) or low racing rung `rung`,
// whose panel keeps every date and every inst_stride-th instrument (strided_panel(panel, 1, s)).
struct SignalLevel {
  atx::usize rung{};
  atx::u32 inst_stride{1};
  bool full{true};
};

// raw is the maximized search signal (elitism, ScalarRaw ranking, rung promotion); objectives
// are the NSGA-II columns (the driver adds parsimony when enabled).
struct SignalScore {
  atx::f64 raw{};
  std::array<atx::f64, kMaxObjectives> objectives{};
  atx::u8 n_objectives{};
  bool rejected{};
};

struct SignalFitnessBinding {
  atx::usize workers{};
  atx::usize dates{};
  atx::usize instruments{};
  std::span<const Rung> low_rungs{}; // the rungs raced before the full pass (may be empty)
};

class SignalFitness {
public:
  virtual ~SignalFitness() = default;
  SignalFitness(const SignalFitness &) = delete;
  SignalFitness &operator=(const SignalFitness &) = delete;

  [[nodiscard]] virtual atx::core::Status bind(const SignalFitnessBinding &binding) = 0;
  // `signal` is date-major (dates x instruments of the level's panel).
  [[nodiscard]] virtual atx::core::Result<SignalScore>
  score(const Genome &genome, std::span<const atx::f64> signal, const SignalLevel &level,
        atx::usize worker) = 0;

protected:
  SignalFitness() = default;
  SignalFitness(SignalFitness &&) noexcept = default;
  SignalFitness &operator=(SignalFitness &&) noexcept = default;
};

} // namespace atx::engine::factory
