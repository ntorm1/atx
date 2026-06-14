#include "atx/engine/learn/nn/trainer.hpp"

#include <limits> // std::numeric_limits
#include <numeric> // std::iota
#include <vector>

#include <Eigen/Dense> // Eigen::Index

#include "atx/core/error.hpp"  // Result, Ok, Err, ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u64, usize

#include "atx/engine/learn/train.hpp" // seed_for

namespace atx::engine::learn::nn {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

namespace {

[[nodiscard]] Eigen::Index idx(atx::usize n) noexcept { return static_cast<Eigen::Index>(n); }

// Deterministic in-place Fisher-Yates over [0, n). Pure function of `seed` (no
// wall-clock / address): the j draw is rng.next_u64() % (i+1), ascending i. The
// reverse walk gives the canonical Durstenfeld shuffle.
void seeded_shuffle(std::vector<atx::usize> &order, atx::u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  for (atx::usize i = order.size(); i-- > 1;) {
    const atx::u64 j = rng.next_u64() % static_cast<atx::u64>(i + 1);
    std::swap(order[i], order[static_cast<atx::usize>(j)]);
  }
}

// Gather rows `rows[lo..hi)` of a sample-major design into a contiguous batch.
[[nodiscard]] lin::MatX gather_rows(const lin::MatX &src, const std::vector<atx::usize> &rows,
                                    atx::usize lo, atx::usize hi) {
  const Eigen::Index b = idx(hi - lo);
  lin::MatX out(b, src.cols());
  for (atx::usize k = lo; k < hi; ++k) {
    out.row(idx(k - lo)) = src.row(idx(rows[k]));
  }
  return out;
}

// Sign of x with sgn(0) == 0 (the L1 subgradient at the origin). Pure, no RNG.
[[nodiscard]] atx::f64 sgn(atx::f64 x) noexcept {
  return (x > 0.0) ? 1.0 : ((x < 0.0) ? -1.0 : 0.0);
}

// One full-pass training epoch over the order-fixed minibatches of `order`.
void run_epoch(Module &model, Optimizer &opt, Loss &loss, const lin::MatX &x_train,
               const lin::MatX &y_train, const std::vector<atx::usize> &order,
               atx::usize batch_size, atx::f64 l2, atx::f64 l1) {
  const atx::usize n = order.size();
  for (atx::usize lo = 0; lo < n; lo += batch_size) {
    const atx::usize hi = (lo + batch_size < n) ? lo + batch_size : n;
    const lin::MatX xb = gather_rows(x_train, order, lo, hi);
    const lin::MatX yb = gather_rows(y_train, order, lo, hi);
    // Zero grads, then accumulate this minibatch's gradient (R1: caller-zeroed).
    std::span<atx::f64> g = model.grads();
    for (atx::f64 &gi : g) {
      gi = 0.0;
    }
    const lin::MatX pred = model.forward(xb);
    const lin::MatX dpred = loss.grad(pred, yb);
    static_cast<void>(model.backward(dpred));
    // L2 weight-decay (DECOUPLED convention): after backward fills the data
    // gradient and before opt.step, add l2 * param[i] to each grad in ASCENDING
    // param order (R1: deterministic scalar fold, no RNG, no reordering). l2 == 0
    // makes this a no-op, so the default config is byte-identical to before. The
    // penalty rides the SAME grad span the optimizer consumes, so it inherits the
    // optimizer's update rule (it is not a separate, reorderable path).
    if (l2 != 0.0) {
      const std::span<atx::f64> params = model.params();
      for (atx::usize i = 0; i < g.size(); ++i) {
        g[i] += l2 * params[i];
      }
    }
    // L1 (LASSO) subgradient (DECOUPLED, mirrors L2): after backward fills the data
    // gradient and before opt.step, add l1 * sgn(param[i]) to each grad in ASCENDING
    // param order (R1: deterministic scalar fold, no RNG, no reordering). sgn(0) == 0
    // so a parameter sitting exactly at the origin contributes no penalty. l1 == 0
    // makes this a no-op, so the default config is byte-identical to before.
    if (l1 != 0.0) {
      const std::span<atx::f64> params = model.params();
      for (atx::usize i = 0; i < g.size(); ++i) {
        g[i] += l1 * sgn(params[i]);
      }
    }
    opt.step(model.params(), model.grads());
  }
}

[[nodiscard]] Result<void> validate_shapes(const lin::MatX &x_train, const lin::MatX &y_train,
                                           const lin::MatX &x_val, const lin::MatX &y_val,
                                           const TrainConfig &cfg) {
  if (x_train.rows() == 0 || x_train.cols() == 0) {
    return Err(ErrorCode::InvalidArgument, "train: empty training design");
  }
  if (x_train.rows() != y_train.rows()) {
    return Err(ErrorCode::InvalidArgument, "train: x_train/y_train row mismatch");
  }
  if (x_val.rows() != y_val.rows()) {
    return Err(ErrorCode::InvalidArgument, "train: x_val/y_val row mismatch");
  }
  if (x_val.rows() > 0 && x_val.cols() != x_train.cols()) {
    return Err(ErrorCode::InvalidArgument, "train: x_train/x_val feature mismatch");
  }
  if (cfg.batch_size == 0) {
    return Err(ErrorCode::InvalidArgument, "train: batch_size must be > 0");
  }
  if (cfg.ensemble_size == 0) {
    return Err(ErrorCode::InvalidArgument, "train: ensemble_size must be > 0");
  }
  return Ok();
}

} // namespace

Result<std::vector<std::vector<atx::f64>>>
train(const ModelFactory &make_model, Optimizer &opt, Loss &loss, const lin::MatX &x_train,
      const lin::MatX &y_train, const lin::MatX &x_val, const lin::MatX &y_val,
      const TrainConfig &cfg) {
  ATX_TRY_VOID(validate_shapes(x_train, y_train, x_val, y_val, cfg));

  std::vector<std::vector<atx::f64>> ensemble;
  ensemble.reserve(cfg.ensemble_size);
  const atx::usize n_train = static_cast<atx::usize>(x_train.rows());
  const bool have_val = x_val.rows() > 0;

  for (atx::usize m = 0; m < cfg.ensemble_size; ++m) {
    // Fresh reseeded model + fresh optimizer state for this member (R1).
    const atx::u64 member_seed = seed_for(cfg.master_seed, "nn-ensemble", m, 0);
    std::unique_ptr<Module> model = make_model(member_seed);
    opt.reset();

    // best-val checkpoint state seeded with the initial params (so a member that
    // never improves still returns a well-defined state).
    std::vector<atx::f64> best_state;
    model->state_to(best_state);
    atx::f64 best_val = std::numeric_limits<atx::f64>::infinity();
    {
      model->train(false);
      const atx::f64 v0 = have_val ? loss.value(model->forward(x_val), y_val)
                                   : loss.value(model->forward(x_train), y_train);
      best_val = v0; // epoch-0 baseline is the initial checkpoint
    }

    for (atx::usize epoch = 0; epoch < cfg.epochs; ++epoch) {
      // Minibatch order is a pure function of (master_seed, member, epoch) (R1b).
      std::vector<atx::usize> order(n_train);
      std::iota(order.begin(), order.end(), atx::usize{0});
      seeded_shuffle(order, seed_for(cfg.master_seed, "nn-shuffle", m, epoch));

      model->train(true);
      run_epoch(*model, opt, loss, x_train, y_train, order, cfg.batch_size, cfg.l2, cfg.l1);

      // Checkpoint-at-best every ckpt_every epochs and always on the last epoch.
      const bool is_last = (epoch + 1 == cfg.epochs);
      const bool do_ckpt = (cfg.ckpt_every > 0 && (epoch + 1) % cfg.ckpt_every == 0) || is_last;
      if (do_ckpt) {
        model->train(false);
        const lin::MatX vp = have_val ? model->forward(x_val) : model->forward(x_train);
        const atx::f64 vl = have_val ? loss.value(vp, y_val) : loss.value(vp, y_train);
        if (vl < best_val) {
          best_val = vl;
          best_state.clear();
          model->state_to(best_state); // snapshot the improved params
        }
      }
    }
    ensemble.push_back(std::move(best_state));
  }
  return Ok(std::move(ensemble));
}

Result<lin::MatX> ensemble_mean_predict(const ModelFactory &make_model,
                                        const std::vector<std::vector<atx::f64>> &states,
                                        const lin::MatX &x) {
  if (states.empty()) {
    return Err(ErrorCode::InvalidArgument, "ensemble_mean_predict: no members");
  }
  lin::MatX acc;
  for (atx::usize m = 0; m < states.size(); ++m) {
    // The factory seed is irrelevant here (params are overwritten by state_from);
    // pass the member index so the build is still a pure function of inputs.
    std::unique_ptr<Module> model = make_model(static_cast<atx::u64>(m));
    // Explicit (release-safe) size guard: state_from's own check is a debug-only
    // ATX_ASSERT, so a mismatch would silently write out of bounds in release.
    if (states[m].size() != model->param_count()) {
      return Err(ErrorCode::InvalidArgument,
                 "ensemble_mean_predict: member state size != model param_count");
    }
    model->state_from(states[m]);
    model->train(false);
    const lin::MatX pred = model->forward(x);
    if (m == 0) {
      acc = pred;
    } else {
      acc += pred; // ascending-member fold (R1)
    }
  }
  acc /= static_cast<atx::f64>(states.size());
  return Ok(std::move(acc));
}

} // namespace atx::engine::learn::nn
