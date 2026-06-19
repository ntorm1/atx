#include "store_progress_sink.hpp"

#include <algorithm> // std::sort
#include <bit>       // std::bit_cast
#include <chrono>    // std::chrono::system_clock
#include <cstdio>    // std::snprintf
#include <string>
#include <vector>

#include "atx/engine/store/fingerprint.hpp" // store::fingerprint::{kFnvOffset, fold_string, fold_u64}

namespace atx::impl {

atx::i64 now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string fp_hex(atx::u64 v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return std::string(b);
}

atx::core::Status StoreProgressSink::on_generation(
    const atx::engine::factory::GenerationSnapshot& s) {
  const std::string blob = atx::engine::store::join_population(s.population);
  // Resumable-discover (Task F1): persist the FULL accumulated state ENTERING this
  // generation alongside the population so a resume restores it byte-identically.
  atx::engine::store::CheckpointState state;
  state.canon_blob           = s.canon_blob;
  state.cache_blob           = s.cache_blob;
  state.archive_blob         = s.archive_blob;
  state.best_per_gen_blob    = s.best_per_gen_blob;
  state.digest               = s.digest;
  state.candidates_generated = static_cast<atx::i64>(s.candidates_generated);
  return rec_.save_checkpoint(
      static_cast<atx::i64>(s.generation), blob,
      static_cast<atx::i64>(s.population.size()), s.best_fitness, s.mean_fitness,
      static_cast<atx::i64>(s.n_evaluated), static_cast<atx::i64>(s.n_unique),
      /*wall_ms*/ 0, now_unix(), state);
}

atx::u64 compute_discover_fingerprint(const RunConfig& cfg) {
  namespace fp = atx::engine::store::fingerprint;
  atx::u64 h = fp::kFnvOffset;
  h = fp::fold_string(h, cfg.panel);
  h = fp::fold_u64(h, static_cast<atx::u64>(cfg.seed));
  h = fp::fold_u64(h, static_cast<atx::u64>(cfg.population));
  h = fp::fold_u64(h, static_cast<atx::u64>(cfg.generations));
  // Order-independent over the seed templates: sort a copy before folding.
  std::vector<std::string> seeds = cfg.seed_exprs;
  std::sort(seeds.begin(), seeds.end());
  for (const auto& e : seeds) h = fp::fold_string(h, e);
  // Gate floors + oos knobs are doubles: fold the exact bit pattern.
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.min_sharpe));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.min_fitness));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.max_turnover));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.max_pool_corr));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.min_dsr));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.oos_fraction));
  h = fp::fold_u64(h, std::bit_cast<atx::u64>(cfg.oos_embargo));
  return h;
}

}  // namespace atx::impl
