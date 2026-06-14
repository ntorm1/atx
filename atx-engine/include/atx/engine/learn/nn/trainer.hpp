#pragma once

// atx::engine::learn::nn — the deterministic Trainer (S5-2a).
//
// =====================================================================
//  What train(...) does (R1 / R6 / R7)
// =====================================================================
//  Trains ONE architecture as a SEED ENSEMBLE and returns the per-member best-
//  validation serialized states (the NN payload a later unit folds into an
//  alpha). The whole pipeline is a pure function of the inputs and cfg.master_seed:
//
//    for member m in [0, ensemble_size):
//      seed_m = seed_for(master, "nn-ensemble", m, 0)
//      model  = make_model(seed_m)        // factory builds + seed-inits the net
//      opt.reset()                        // fresh optimizer state per member
//      best_val = +inf ; best_state = init state
//      for epoch in [0, epochs):
//        order = deterministic shuffle of train rows, pure fn of
//                seed_for(master, "nn-shuffle", member?, epoch)  (see .cpp)
//        for each minibatch (order-fixed, fixed batch_size):
//          zero grads ; forward ; loss.grad ; backward ; opt.step
//        every ckpt_every epochs (and the last): eval val loss; if < best_val,
//          snapshot best_state = model.state_to (checkpoint-at-best, R1)
//      ensemble[m] = best_state
//
//  Determinism: the only RNG is the seeded init inside make_model and the seeded
//  shuffle; both derive from seed_for (SplitMix). No wall-clock / address / map
//  order. Two runs with the same master_seed produce byte-identical ensembles.
//
//  proto/clone (the factory mechanism): the Trainer needs a FRESH reseeded model
//  per member. The caller supplies a factory `make_model(seed) -> unique_ptr<Module>`
//  that BOTH constructs the architecture AND seed-initialises its parameters from
//  the given member seed. The Trainer owns no knowledge of layer shapes — it only
//  drives forward/backward/step/serialize. This keeps the substrate generic.

#include <functional>
#include <memory>
#include <vector>

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/learn/nn/loss.hpp"      // Loss
#include "atx/engine/learn/nn/module.hpp"    // Module
#include "atx/engine/learn/nn/optimizer.hpp" // Optimizer

namespace atx::engine::learn::nn {

// ===========================================================================
//  TrainConfig — fixed budget knobs. All deterministic; no early stopping on a
//  wall-clock or stochastic criterion.
// ===========================================================================
struct TrainConfig {
  atx::usize epochs = 200;
  atx::usize batch_size = 256;
  atx::usize ckpt_every = 10;   // eval + maybe-checkpoint every N epochs
  atx::usize ensemble_size = 5; // number of independently-seeded members
  atx::u64 master_seed = 0;
  // L2 weight-decay coefficient (DECOUPLED convention): each minibatch step, after
  // backward fills grads, grad[i] += l2 * param[i] is added in ascending param
  // order, then opt.step runs. 0.0 (the default) is a no-op, so a config that does
  // not set it trains exactly as before (R1: deterministic ascending fold, no RNG).
  atx::f64 l2 = 0.0;
};

// The model factory: build + seed-initialise a fresh architecture for `seed`.
// Called once per ensemble member. MUST be a pure function of `seed` (same seed
// => byte-identical initial params) for the R1 determinism proof to hold.
using ModelFactory = std::function<std::unique_ptr<Module>(atx::u64 seed)>;

// ===========================================================================
//  train — deterministic seed-ensemble training of ONE architecture.
//
//  Inputs are tabular designs: x_* are samples x in_features, y_* are
//  samples x out_features (rows aligned). Returns one serialized best-val state
//  per ensemble member (member order ascending). Use Module::state_from to
//  reload a member.
//
//  Errors (Result): empty design, x/y row mismatch, x_train/x_val feature
//  mismatch, batch_size == 0, ensemble_size == 0 -> InvalidArgument.
// ===========================================================================
[[nodiscard]] atx::core::Result<std::vector<std::vector<atx::f64>>>
train(const ModelFactory &make_model, Optimizer &opt, Loss &loss, const lin::MatX &x_train,
      const lin::MatX &y_train, const lin::MatX &x_val, const lin::MatX &y_val,
      const TrainConfig &cfg);

// ===========================================================================
//  ensemble_mean_predict — average the predictions of every ensemble member.
//
//  Rebuilds each member from its serialized state (via make_model + state_from)
//  in eval mode, predicts on x, and folds the member predictions with an
//  ASCENDING-member scalar mean (R1). Convenience for callers; the Trainer's
//  own determinism does not depend on it.
// ===========================================================================
[[nodiscard]] atx::core::Result<lin::MatX>
ensemble_mean_predict(const ModelFactory &make_model,
                      const std::vector<std::vector<atx::f64>> &states, const lin::MatX &x);

} // namespace atx::engine::learn::nn
