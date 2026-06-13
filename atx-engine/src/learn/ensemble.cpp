#include "atx/engine/learn/ensemble.hpp"

#include <cmath>   // std::isfinite
#include <limits>  // std::numeric_limits (tail NaN label in meta_features_from_pool)
#include <string>  // std::string (expr_source, model hash buffer)
#include <utility> // std::move

#include "atx/core/hash.hpp"  // atx::core::hash_bytes (verdict + model hash)
#include "atx/core/macro.hpp" // ATX_CHECK

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics, compute_metrics
#include "atx/engine/library/record.hpp"  // library::Provenance

namespace atx::engine::learn {

namespace ensemble_detail {

atx::core::linalg::MatX regime_observable(const FeatureMatrix &fm) {
  const atx::usize nd = fm.n_dates;
  const atx::usize marker = (fm.n_features == 0U) ? 0U : (fm.n_features - 1U);
  atx::core::linalg::MatX obs(static_cast<Eigen::Index>(nd), 1);
  for (atx::usize d = 0; d < nd; ++d) {
    obs(static_cast<Eigen::Index>(d), 0) = 0.0;
  }
  atx::usize r = 0;
  const atx::usize nr = fm.n_rows();
  while (r < nr) {
    const atx::usize date = fm.row_date[r];
    atx::f64 sum = 0.0;
    atx::usize cnt = 0;
    while (r < nr && fm.row_date[r] == date) {
      sum += fm.X[r * fm.n_features + marker];
      ++cnt;
      ++r;
    }
    if (date < nd && cnt > 0U) {
      obs(static_cast<Eigen::Index>(date), 0) = sum / static_cast<atx::f64>(cnt);
    }
  }
  return obs;
}

std::vector<atx::u32> date_regimes(const Hmm &regime, const atx::core::linalg::MatX &obs) {
  const atx::usize nd = static_cast<atx::usize>(obs.rows());
  std::vector<atx::u32> reg(nd, 0U);
  for (atx::usize d = 0; d < nd; ++d) {
    const atx::core::linalg::VecX post = regime_posterior_at(regime, obs, d);
    atx::f64 best = post(0);
    atx::u32 arg = 0U;
    for (Eigen::Index s = 1; s < post.size(); ++s) {
      if (post(s) > best) { // strictly-greater -> lower index wins a tie (M1)
        best = post(s);
        arg = static_cast<atx::u32>(s);
      }
    }
    reg[d] = arg;
  }
  return reg;
}

FeatureMatrix regime_submatrix(const FeatureMatrix &fm, const std::vector<atx::u32> &date_reg,
                               atx::u32 which) {
  FeatureMatrix sub;
  sub.n_dates = fm.n_dates;
  sub.n_instruments = fm.n_instruments;
  sub.n_features = fm.n_features;
  sub.Y.assign(fm.Y.size(), {});
  const atx::usize p = fm.n_features;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    const atx::usize d = fm.row_date[r];
    ATX_CHECK(d < date_reg.size()); // every row's date indexes the per-date regime table
    if (date_reg[d] != which) {
      continue;
    }
    const atx::usize row = sub.push_row(d, fm.row_inst[r]);
    (void)row;
    for (atx::usize f = 0; f < p; ++f) {
      sub.X.push_back(fm.X[r * p + f]);
    }
    sub.row_valid.push_back(fm.row_valid[r]);
    for (atx::usize h = 0; h < fm.Y.size(); ++h) {
      sub.Y[h].push_back(fm.Y[h][r]);
    }
  }
  return sub;
}

LearnedModel fit_regime_nonlinear(const FeatureMatrix &meta, const Hmm &regime,
                                  const StackingCfg &cfg) {
  const atx::core::linalg::MatX obs = regime_observable(meta);
  const std::vector<atx::u32> date_reg = date_regimes(regime, obs);
  LearnedModel combined;
  combined.kind = (cfg.base == StackingCfg::Base::Gbt) ? ModelKind::Gbt : ModelKind::Linear;
  combined.trial_count = 0;
  for (atx::u32 s = 0; s < regime.n_states; ++s) {
    const FeatureMatrix sub = regime_submatrix(meta, date_reg, s);
    if (sub.n_rows() == 0U) {
      continue; // an unoccupied regime contributes no rows / no skill
    }
    const LearnedModel m = fit_flat_nonlinear(sub, cfg);
    combined.oos_score_series.insert(combined.oos_score_series.end(), m.oos_score_series.begin(),
                                     m.oos_score_series.end());
    combined.trial_count += m.trial_count;
  }
  return combined;
}

atx::u64 verdict_hash_of(const StackingVerdict &v) {
  std::vector<atx::f64> buf;
  buf.push_back(v.admitted ? 1.0 : 0.0);
  buf.push_back(v.oos_dsr_nonlinear);
  buf.push_back(v.oos_dsr_linear);
  buf.push_back(v.oos_ic_nonlinear);
  buf.push_back(v.oos_ic_linear);
  buf.push_back(static_cast<atx::f64>(static_cast<atx::u8>(v.reason)));
  // SAFETY: std::vector<f64> stores doubles contiguously; buf.data() points at
  // buf.size()*sizeof(f64) live bytes for the duration of the hash call.
  return atx::core::hash_bytes(buf.data(), buf.size() * sizeof(atx::f64));
}

} // namespace ensemble_detail

FeatureMatrix meta_features_from_pool(const combine::AlphaStore &pool,
                                      std::span<const atx::f64> forward_returns_flat,
                                      std::span<const atx::u16> horizons) {
  FeatureMatrix fm;
  fm.n_dates = pool.n_periods();
  fm.n_instruments = pool.n_instruments();
  fm.n_features = pool.n_alphas();
  fm.Y.assign(horizons.size(), {});
  const atx::usize ni = fm.n_instruments;
  for (atx::usize d = 0; d < fm.n_dates; ++d) {
    for (atx::usize i = 0; i < ni; ++i) {
      const atx::usize row = fm.push_row(d, i);
      (void)row;
      bool all_finite = true;
      for (atx::usize f = 0; f < fm.n_features; ++f) {
        const std::span<const atx::f64> cs = pool.positions(combine::AlphaId{static_cast<atx::u32>(f)}, d);
        const atx::f64 v = cs[i];
        fm.X.push_back(v);
        all_finite = all_finite && std::isfinite(v);
      }
      fm.row_valid.push_back(static_cast<atx::u8>(all_finite ? 1 : 0));
      for (atx::usize h = 0; h < horizons.size(); ++h) {
        const atx::usize ahead = d + static_cast<atx::usize>(horizons[h]);
        if (ahead >= fm.n_dates) {
          fm.Y[h].push_back(std::numeric_limits<atx::f64>::quiet_NaN());
        } else {
          fm.Y[h].push_back(forward_returns_flat[d * ni + i]); // PIT-aligned by caller
        }
      }
    }
  }
  return fm;
}

StackingVerdict fit_stack(const FeatureMatrix &meta, const Hmm *regime, const StackingCfg &cfg) {
  const LatentAugmentation empty_aug; // PURE linear combination (no interaction aug)

  // The LINEAR benchmark: elastic-net over the raw alpha columns.
  const LearnedModel lin_model = fit_linear(meta, empty_aug, ensemble_detail::linear_cfg_of(cfg));

  // The NONLINEAR base: flat, or regime-conditional (a per-regime base whose
  // out-of-fold series are unioned — see fit_regime_nonlinear).
  const LearnedModel nl_model = (regime != nullptr)
                                    ? ensemble_detail::fit_regime_nonlinear(meta, *regime, cfg)
                                    : ensemble_detail::fit_flat_nonlinear(meta, cfg);

  StackingVerdict v;
  // SAME metric, SAME meta: both models scored by oos_deflated_sharpe / oos_ic.
  v.oos_dsr_linear = oos_deflated_sharpe(lin_model, meta);
  v.oos_dsr_nonlinear = oos_deflated_sharpe(nl_model, meta);
  v.oos_ic_linear = oos_ic(lin_model, meta);
  v.oos_ic_nonlinear = oos_ic(nl_model, meta);

  // §0.4 gate: admit only if the nonlinear base survives deflation AND beats the
  // linear combination's OOS IC. Else RejectFitness ("no ML-for-ML's-sake").
  v.admitted = (v.oos_dsr_nonlinear > 0.0) && (v.oos_ic_nonlinear > v.oos_ic_linear);
  v.reason = v.admitted ? AdmitKind::Accept : AdmitKind::RejectFitness;
  v.verdict_hash = ensemble_detail::verdict_hash_of(v);
  return v;
}

StackCandidate stack_to_candidate(const StackingVerdict & /*verdict*/, const FeatureMatrix &meta,
                                  const StackingCfg &cfg) {
  const atx::usize np = meta.n_dates;
  const atx::usize ni = meta.n_instruments;
  StackCandidate out;
  out.pnl.assign(np, 0.0);
  out.pos_flat.assign(np * ni, 0.0);

  const LearnedModel deployed = ensemble_detail::deploy_nonlinear(meta, cfg);

  // Per date, predict the cross-section over the date's VALID rows (predict_at
  // emits one score per emitted in-universe instrument, in row order) and scatter
  // each score back to its instrument slot; pnl[d] = Σ position * forward return.
  for (atx::usize d = 0; d < np; ++d) {
    const atx::core::linalg::VecX scores = predict_at(deployed, meta, d);
    // Walk the date's rows in order to map each emitted score to its instrument and
    // its horizon-0 label (predict_at emits valid in-universe rows in row order).
    atx::usize si = 0;
    atx::f64 pnl_d = 0.0;
    for (atx::usize r = 0; r < meta.n_rows(); ++r) {
      if (meta.row_date[r] != d || meta.row_valid[r] == 0) {
        continue;
      }
      ATX_CHECK(si < static_cast<atx::usize>(scores.size())); // one score per emitted valid row
      const atx::f64 pos = scores(static_cast<Eigen::Index>(si));
      const atx::usize inst = meta.row_inst[r];
      ATX_CHECK(inst < ni); // the row's instrument indexes the cross-section
      out.pos_flat[d * ni + inst] = pos;
      const atx::f64 fwd = meta.Y[0][r];
      if (std::isfinite(fwd)) {
        pnl_d += pos * fwd;
      }
      ++si;
    }
    out.pnl[d] = pnl_d;
  }

  // Metrics over the synthesized streams (book_size 1.0 — the scores are already a
  // gross cross-section, not normalized notionals; turnover is then in score units,
  // which is fine for the M6 dedup-distinctness check the test asserts).
  const combine::AlphaMetrics metrics =
      combine::compute_metrics(std::span<const atx::f64>{out.pnl},
                               std::span<const atx::f64>{out.pos_flat}, ni, /*book_size=*/1.0);

  // canon_hash: a deterministic digest over the synthesized streams (distinct
  // streams -> distinct hash, so a fresh stack does not dedup against an empty
  // library). Order-fixed byte hash, no map / clock input (M1).
  std::vector<atx::f64> hbuf;
  hbuf.insert(hbuf.end(), out.pnl.begin(), out.pnl.end());
  hbuf.insert(hbuf.end(), out.pos_flat.begin(), out.pos_flat.end());
  // SAFETY: std::vector<f64> stores doubles contiguously; hbuf.data() points at
  // hbuf.size()*sizeof(f64) live bytes for the duration of the hash call.
  const atx::u64 canon =
      atx::core::hash_bytes(hbuf.data(), hbuf.size() * sizeof(atx::f64));

  library::Provenance prov;
  prov.expr_source =
      std::string("learned:stack:") + (cfg.base == StackingCfg::Base::Gbt ? "gbt" : "elasticnet");
  prov.seed = cfg.master_seed;

  out.candidate.canon_hash = canon;
  out.candidate.pnl = std::span<const atx::f64>{out.pnl};
  out.candidate.pos_flat = std::span<const atx::f64>{out.pos_flat};
  out.candidate.metrics = metrics;
  out.candidate.prov = std::move(prov);
  out.candidate.as_of = (np == 0U) ? 0U : (np - 1U);
  out.candidate.source = nullptr; // no live re-eval handle (deferred — see header)
  return out;
}

} // namespace atx::engine::learn
