#pragma once

// atx::engine::factory — SearchProgressSink + SearchResumeState (resumable-discover).
//
// An abstract progress sink the genetic search loop calls ONCE per completed
// generation (after that generation's population is scored + ranked, before the
// next generation is reproduced) so a long-running discover can checkpoint the
// generation-input population and resume from the last completed generation after
// a crash. The sink is OPTIONAL: SearchDriver::run defaults it to nullptr and the
// nullptr path is byte-identical to the legacy loop (F1/F2). The engine never does
// I/O or wall-clock work itself — the sink is the only seam through which a caller
// may persist state, and it returns a Status so a real I/O failure (or an injected
// test crash) cleanly aborts the run with a well-formed partial result.

#include <string>
#include <vector>

#include "atx/core/error.hpp" // atx::core::Status
#include "atx/core/types.hpp" // atx::usize, atx::f64

namespace atx::engine::factory {

// A read-only snapshot of one completed generation, handed to the sink. The
// `population` is the generation INPUT (the genomes that ENTERED generation
// `generation`), serialized as canonical DSL strings in canonical-id order — the
// exact blob a resume feeds back via SearchResumeState. The fitness fields and
// counts are pure telemetry (NOT part of the digest or the admitted set).
struct GenerationSnapshot {
  atx::usize generation{0};            // 0-based index of the generation just scored
  std::vector<std::string> population; // unparse per genome, canonical-id order (gen INPUT)
  atx::f64 best_fitness{0.0};          // best RAW fitness this generation
  atx::f64 mean_fitness{0.0};          // mean RAW fitness over finite-scored members
  atx::usize n_evaluated{0};           // distinct candidates scored so far (CanonSet size)
  atx::usize n_unique{0};              // genomes in this generation's population
};

// Abstract progress sink. `on_generation` is invoked once per completed
// generation in ascending order. Returning an Err aborts the run cleanly
// (the driver finalizes the current scored set into a well-formed partial result
// and returns). A null sink (the default) disables the seam entirely.
class SearchProgressSink {
public:
  virtual ~SearchProgressSink() = default;
  [[nodiscard]] virtual atx::core::Status on_generation(const GenerationSnapshot &) = 0;
};

// Resume directive for SearchDriver::run. When supplied (and well-formed), the
// loop starts at `start_generation` from `population` (the canonical DSL strings
// captured in a prior GenerationSnapshot) instead of init_population.
// PipelineRecorder::latest_population_blob verifies the stored checkpoint
// state_hash against population_hash(blob) before returning the blob; a mismatch
// returns Err(Internal) which aborts the resume rather than silently resuming
// from a corrupt population. The driver's deserialize is a further backstop that
// refuses an incompatible blob rather than silently restarting from generation 0.
struct SearchResumeState {
  atx::usize start_generation{0};
  std::vector<std::string> population;
};

} // namespace atx::engine::factory
