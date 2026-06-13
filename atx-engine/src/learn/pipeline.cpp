#include "atx/engine/learn/pipeline.hpp"

#include <vector> // std::vector (digest buffer)

#include <Eigen/Dense> // Eigen::Index (coeff / forest / hmm element access)

#include "atx/core/hash.hpp" // atx::core::hash_bytes

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/learn/ensemble.hpp"       // StackingVerdict, fit_stack, regime_observable
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"            // GbtForest/Tree/Node, fit_gbt
#include "atx/engine/learn/hmm.hpp"            // Hmm, HmmCfg, baum_welch, forward_log, Gaussian
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel
#include "atx/engine/learn/linear_alpha.hpp"   // fit_linear

namespace atx::engine::learn {

namespace pipeline_detail {

// Fold a LearnedModel's DECIDED parameters (M1) into the f64 digest buffer:
// kind, trial_count, blend weights, the frozen out-of-fold series, the linear
// coefficients, and every GBT forest's node bytes. All deterministic from the
// seeded fit; NO clock / map / filesystem input. Order-fixed ascending walks.
void fold_model(std::vector<atx::f64> &buf, const LearnedModel &m) {
  buf.push_back(static_cast<atx::f64>(static_cast<atx::u8>(m.kind)));
  buf.push_back(static_cast<atx::f64>(m.trial_count));
  buf.insert(buf.end(), m.blend_w.begin(), m.blend_w.end());
  buf.insert(buf.end(), m.oos_score_series.begin(), m.oos_score_series.end());
  for (const atx::core::linalg::VecX &c : m.coeffs) {
    for (Eigen::Index j = 0; j < c.size(); ++j) {
      buf.push_back(c(j));
    }
  }
  for (const GbtForest &forest : m.forests) {
    buf.push_back(forest.base);
    for (const GbtTree &tree : forest.trees) {
      buf.push_back(static_cast<atx::f64>(tree.nodes.size()));
      for (const GbtNode &node : tree.nodes) {
        buf.push_back(static_cast<atx::f64>(node.feature));
        buf.push_back(node.threshold);
        buf.push_back(node.leaf_value);
        buf.push_back(static_cast<atx::f64>(node.left));
        buf.push_back(static_cast<atx::f64>(node.right));
        buf.push_back(node.is_leaf ? 1.0 : 0.0);
      }
    }
  }
}

} // namespace pipeline_detail

[[nodiscard]] atx::u64 full_pipeline_digest(const FeatureMatrix &fm, const PipelineCfg &cfg) {
  const LatentAugmentation empty_aug; // the pipeline exercises the raw-feature path

  const LearnedModel lin = fit_linear(fm, empty_aug, pipeline_detail::linear_cfg_of(cfg));
  const LearnedModel gbt = fit_gbt(fm, empty_aug, pipeline_detail::gbt_cfg_of(cfg));

  std::vector<atx::f64> buf;
  pipeline_detail::fold_model(buf, lin);
  pipeline_detail::fold_model(buf, gbt);

  // HMM on the derived marker observable: fold its log-parameters + an independent
  // forward log-likelihood (all deterministic from the seeded init + order-fixed EM).
  const atx::core::linalg::MatX obs = ensemble_detail::regime_observable(fm);
  HmmCfg hcfg;
  hcfg.n_states = 2U;
  hcfg.master_seed = cfg.master_seed;
  const Hmm hmm = baum_welch(obs, hcfg);
  for (Eigen::Index i = 0; i < hmm.logA.rows(); ++i) {
    for (Eigen::Index j = 0; j < hmm.logA.cols(); ++j) {
      buf.push_back(hmm.logA(i, j));
    }
  }
  for (Eigen::Index s = 0; s < hmm.logpi.size(); ++s) {
    buf.push_back(hmm.logpi(s));
  }
  for (const Gaussian &g : hmm.emit) {
    for (Eigen::Index j = 0; j < g.mean.size(); ++j) {
      buf.push_back(g.mean(j));
    }
    for (Eigen::Index j = 0; j < g.var.size(); ++j) {
      buf.push_back(g.var(j));
    }
  }
  buf.push_back(forward_log(hmm, obs).loglik);

  // SAFETY: std::vector<f64> stores doubles contiguously; buf.data() points at
  // buf.size()*sizeof(f64) live bytes for the duration of the hash call.
  const atx::u64 model_digest = atx::core::hash_bytes(buf.data(), buf.size() * sizeof(atx::f64));

  // The stacking verdict_hash (already a deterministic digest of the §0.4 gate's
  // decided numeric fields) folds in losslessly: hash the {model_digest,
  // verdict_hash} u64 pair by bytes (order-fixed, no f64 precision loss).
  const StackingVerdict v = fit_stack(fm, /*regime=*/nullptr, pipeline_detail::stack_cfg_of(cfg));
  const atx::u64 pair[2] = {model_digest, v.verdict_hash};
  // SAFETY: `pair` is a 2-element u64 array on the stack; &pair[0] is valid for
  // 2*sizeof(u64) bytes for the duration of the hash call.
  return atx::core::hash_bytes(&pair[0], 2U * sizeof(atx::u64));
}

} // namespace atx::engine::learn
