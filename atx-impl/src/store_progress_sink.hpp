#pragma once

// atx::impl — StoreProgressSink + the discover run fingerprint (resumable-discover, Task 7).
//
// StoreProgressSink adapts the engine's abstract factory::SearchProgressSink onto a
// store::PipelineRecorder: each completed generation is checkpointed (population blob +
// telemetry) in one Transaction. The sink is the ONLY seam through which the gated
// discover stage persists generation state — the engine itself never does I/O or reads
// the wall clock (now_unix lives here, impl-side, and is fed in as a parameter).
//
// compute_discover_fingerprint is a PURE function of the resumable inputs (no time / no
// RNG): same RunConfig -> same fingerprint -> a re-launch resumes the same run. fp_hex
// renders a u64 as the 16-char lowercase hex pipeline_run_id.

#include <string>

#include "atx/core/error.hpp" // atx::core::Status
#include "atx/core/types.hpp" // atx::i64, atx::u64

#include "atx/engine/factory/search_progress.hpp"   // factory::SearchProgressSink, GenerationSnapshot
#include "atx/engine/store/pipeline_progress.hpp"    // store::PipelineRecorder

#include "config.hpp" // RunConfig

namespace atx::impl {

// Unix seconds (impl-side wall clock; NEVER used in any engine determinism path).
[[nodiscard]] atx::i64 now_unix();

// A store-backed progress sink: forwards each completed generation's snapshot to a
// PipelineRecorder as a checkpoint + iteration + events (one atomic Transaction).
class StoreProgressSink final : public atx::engine::factory::SearchProgressSink {
 public:
  explicit StoreProgressSink(atx::engine::store::PipelineRecorder& rec) : rec_{rec} {}

  [[nodiscard]] atx::core::Status
  on_generation(const atx::engine::factory::GenerationSnapshot& s) override;

 private:
  atx::engine::store::PipelineRecorder& rec_;
};

// Deterministic FNV-1a64 over the resumable inputs. Same config -> same fp; a change to
// seed, panel, population, generations, seed_exprs (order-independent: sorted), gate
// floors, or the oos knobs changes it.
[[nodiscard]] atx::u64 compute_discover_fingerprint(const RunConfig& cfg);

// 16-char lowercase hex of a u64 (for pipeline_run_id).
[[nodiscard]] std::string fp_hex(atx::u64 v);

}  // namespace atx::impl
