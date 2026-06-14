#include "atx/engine/learn/tcn_alpha.hpp"

#include <algorithm> // std::sort, std::unique
#include <cmath>     // std::isfinite
#include <functional> // std::function (the factory-builder seam)
#include <memory>    // std::unique_ptr, std::make_unique
#include <span>      // std::span
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX

#include "atx/core/error.hpp"  // Result, Ok, Err, ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp (seeded param init)
#include "atx/core/types.hpp"  // f64, u16, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/eval/cpcv.hpp"        // eval::CpcvConfig, eval::cpcv_folds, eval::LabelSpan
#include "atx/engine/learn/latent.hpp"     // detail::pearson (reused, order-fixed)
#include "atx/engine/learn/nn/layers.hpp"  // nn::Linear
#include "atx/engine/learn/nn/loss.hpp"    // nn::MseLoss
#include "atx/engine/learn/nn/module.hpp"  // nn::Module, nn::Sequential
#include "atx/engine/learn/nn/optimizer.hpp"  // nn::Adam
#include "atx/engine/learn/nn/seq_layers.hpp" // nn::TcnResidualBlock, nn::GruCell, nn::SeqLastStep
#include "atx/engine/learn/nn/trainer.hpp"    // nn::train, nn::ensemble_mean_predict, nn::ModelFactory
#include "atx/engine/learn/train.hpp"         // seed_for

namespace atx::engine::learn {

namespace lin = atx::core::linalg;

namespace detail {

// ---------------------------------------------------------------------------
//  A FactoryBuilder turns the window shape (L, F) into a Trainer ModelFactory.
//  fit_tcn / fit_gru supply one each; everything below is arch-agnostic. The
//  built factory is a PURE function of its member seed (R1): same seed ->
//  byte-identical initial params.
// ---------------------------------------------------------------------------
using FactoryBuilder = std::function<nn::ModelFactory(atx::usize L, atx::usize F)>;

// Seed-initialise a freshly built network's params with small normals from a
// generator seeded by `seed` (the column-major / ascending param order the flat
// buffer exposes). Mirrors the substrate tests' init path (R1). 0.05 scale keeps
// the deep TCN stable at init.
void seed_init(nn::Module &net, atx::u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  for (atx::f64 &p : net.params()) {
    p = 0.05 * rng.normal();
  }
}

// The TCN factory: Sequential{ TcnResidualBlock x blocks (dilation 1,2,4,...,
// first block F->channels, rest channels->channels) -> SeqLastStep -> Linear
// (channels->1) }. The block's own seed bases its dropout masks; the member seed
// seed-inits the whole flat param buffer.
[[nodiscard]] nn::ModelFactory tcn_factory(atx::usize L, atx::usize F, atx::usize blocks,
                                           atx::usize kernel, atx::usize channels,
                                           atx::f64 dropout) {
  return [L, F, blocks, kernel, channels, dropout](atx::u64 seed) -> std::unique_ptr<nn::Module> {
    auto seq = std::make_unique<nn::Sequential>();
    for (atx::usize b = 0; b < blocks; ++b) {
      const atx::usize cin = (b == 0U) ? F : channels;
      const atx::usize dil = static_cast<atx::usize>(1) << b; // 1,2,4,...
      const atx::u64 blk_seed = seed_for(seed, "tcn-block", b, 0U);
      seq->add(std::make_unique<nn::TcnResidualBlock>(L, cin, channels, kernel, dil, dropout,
                                                      blk_seed));
    }
    seq->add(std::make_unique<nn::SeqLastStep>(L, channels));
    seq->add(std::make_unique<nn::Linear>(channels, static_cast<atx::usize>(1), /*bias=*/true));
    seq->build();
    seed_init(*seq, seed);
    return seq;
  };
}

// The GRU-lite factory: Sequential{ GruCell(F->hidden) -> Linear(hidden->1) }.
// GruCell returns the FINAL hidden state, so no SeqLastStep is needed.
[[nodiscard]] nn::ModelFactory gru_factory(atx::usize L, atx::usize F, atx::usize hidden) {
  return [L, F, hidden](atx::u64 seed) -> std::unique_ptr<nn::Module> {
    auto seq = std::make_unique<nn::Sequential>();
    seq->add(std::make_unique<nn::GruCell>(L, F, hidden));
    seq->add(std::make_unique<nn::Linear>(hidden, static_cast<atx::usize>(1), /*bias=*/true));
    seq->build();
    seed_init(*seq, seed);
    return seq;
  };
}

// ---------------------------------------------------------------------------
//  Sequence CPCV plumbing — the SAMPLE-keyed analog of train.hpp's date axis +
//  expand_date_folds. Keys on seq.date_of, valid samples only, ascending.
// ---------------------------------------------------------------------------

// The ascending list of distinct dates over VALID samples (the CPCV date axis).
// A CPCV fold's index `o` is an ordinal into THIS list -> used_dates[o] is the
// actual anchor date.
[[nodiscard]] std::vector<atx::usize> seq_used_dates(const SequenceTensor &seq) {
  std::vector<atx::usize> dates;
  dates.reserve(seq.n_samples);
  for (atx::usize s = 0; s < seq.n_samples; ++s) {
    if (seq.sample_valid[s] != 0U) {
      dates.push_back(seq.date_of[s]);
    }
  }
  std::sort(dates.begin(), dates.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
  return dates;
}

// One half-open forward-return label span per used date, for horizon h: {date,
// min(date + horizon, max_anchor_date + 1)}. max_anchor is the largest used date
// (the date axis is ascending, so it is the last element). The used_dates.back()+1
// cap is equivalent to fit_linear's n_dates cap: the CPCV observations span only
// the USED dates (the date axis cpcv_folds runs over), so clamping the label
// window to one past the last used anchor is the same firewall, just keyed to the
// observed axis rather than the panel's nominal date count.
[[nodiscard]] std::vector<eval::LabelSpan>
seq_label_spans(const std::vector<atx::usize> &used_dates, atx::u16 horizon) {
  std::vector<eval::LabelSpan> spans;
  spans.reserve(used_dates.size());
  if (used_dates.empty()) {
    return spans;
  }
  const atx::usize cap = used_dates.back() + 1U; // one past the last anchor date
  for (const atx::usize d : used_dates) {
    atx::usize t1 = d + static_cast<atx::usize>(horizon);
    if (t1 > cap) {
      t1 = cap;
    }
    spans.push_back(eval::LabelSpan{d, t1});
  }
  return spans;
}

// Map a fold's date ordinals (into used_dates) to the VALID sample indices whose
// anchor date is one of those dates, ascending by sample. `date_in_set[date] ==
// true` marks a fold-side date; built by the caller from the ordinal list.
[[nodiscard]] std::vector<atx::usize> samples_for_dates(const SequenceTensor &seq,
                                                        const std::vector<bool> &date_in_set) {
  std::vector<atx::usize> out;
  for (atx::usize s = 0; s < seq.n_samples; ++s) {
    if (seq.sample_valid[s] == 0U) {
      continue;
    }
    const atx::usize d = seq.date_of[s];
    if (d < date_in_set.size() && date_in_set[d]) {
      out.push_back(s);
    }
  }
  return out;
}

// Build the per-date membership table (sized to max_date+1) from a fold's date
// ordinals into used_dates.
[[nodiscard]] std::vector<bool> date_membership(const std::vector<atx::usize> &used_dates,
                                                std::span<const atx::usize> ordinals) {
  std::vector<bool> in_set;
  if (!used_dates.empty()) {
    in_set.assign(used_dates.back() + 1U, false);
  }
  for (const atx::usize o : ordinals) {
    if (o < used_dates.size()) {
      in_set[used_dates[o]] = true;
    }
  }
  return in_set;
}

// One CPCV fold expanded to ascending TRAIN / TEST sample-index sets.
struct SampleFold {
  std::vector<atx::usize> train_samples;
  std::vector<atx::usize> test_samples;
};

[[nodiscard]] std::vector<SampleFold>
expand_seq_folds(const std::vector<eval::CpcvFold> &dfolds, const SequenceTensor &seq,
                 const std::vector<atx::usize> &used_dates) {
  std::vector<SampleFold> out;
  out.reserve(dfolds.size());
  for (const eval::CpcvFold &f : dfolds) {
    const std::vector<bool> tr_set =
        date_membership(used_dates, std::span<const atx::usize>{f.train_idx});
    const std::vector<bool> te_set =
        date_membership(used_dates, std::span<const atx::usize>{f.test_idx});
    SampleFold sf;
    sf.train_samples = samples_for_dates(seq, tr_set);
    sf.test_samples = samples_for_dates(seq, te_set);
    out.push_back(std::move(sf));
  }
  return out;
}

// ---------------------------------------------------------------------------
//  Design assembly — copy each selected sample's flat (L*F) window into a MatX
//  row (the Trainer's time-major (B, T*C) encoding, which is byte-identical to
//  the SequenceTensor per-sample layout idx(t,f) = t*F + f). The label is the
//  per-sample value `label_of(s)`; samples with a non-finite label are SKIPPED.
//  `kept_out` (when non-null) receives the sample index of each emitted row, in
//  row order, so a caller can map a prediction back to its sample (OOF series).
// ---------------------------------------------------------------------------
using LabelFn = std::function<atx::f64(atx::usize sample)>;

[[nodiscard]] atx::usize build_seq_design(const SequenceTensor &seq,
                                          std::span<const atx::usize> samples,
                                          const LabelFn &label_of, lin::MatX &x_out,
                                          lin::MatX &y_out, std::vector<atx::usize> *kept_out) {
  const atx::usize wlen = seq.lookback * seq.n_features; // L*F columns per row
  // First pass: count usable (finite-label) samples so the matrix is sized once.
  std::vector<atx::usize> usable;
  usable.reserve(samples.size());
  for (const atx::usize s : samples) {
    if (std::isfinite(label_of(s))) {
      usable.push_back(s);
    }
  }
  x_out.resize(static_cast<Eigen::Index>(usable.size()), static_cast<Eigen::Index>(wlen));
  y_out.resize(static_cast<Eigen::Index>(usable.size()), 1);
  if (kept_out != nullptr) {
    kept_out->clear();
    kept_out->reserve(usable.size());
  }
  atx::usize w = 0;
  for (const atx::usize s : usable) {
    const atx::usize base = s * wlen;
    for (atx::usize j = 0; j < wlen; ++j) {
      x_out(static_cast<Eigen::Index>(w), static_cast<Eigen::Index>(j)) = seq.x[base + j];
    }
    y_out(static_cast<Eigen::Index>(w), 0) = label_of(s);
    if (kept_out != nullptr) {
      kept_out->push_back(s);
    }
    ++w;
  }
  return w;
}

// The per-date out-of-fold IC series, sample-keyed (the analog of
// linear_alpha's detail::oof_ic_series). For each used date (ascending), gather
// the covered (cnt>0), finite-label samples at that date, form the
// cross-sectional Pearson of (OOF pred, horizon-0 label), and emit it when >= 2
// such samples exist. `oof_sum[s]` / `oof_cnt[s]` accumulate the average-across-
// folds OOF prediction for sample s.
[[nodiscard]] std::vector<atx::f64>
seq_oof_ic_series(const SequenceTensor &seq, const std::vector<atx::usize> &used_dates,
                  std::span<const atx::f64> oof_sum, std::span<const atx::u32> oof_cnt) {
  std::vector<atx::f64> series;
  series.reserve(used_dates.size());
  const std::vector<atx::f64> &y0 = seq.y[0];
  for (const atx::usize date : used_dates) {
    std::vector<atx::f64> pv;
    std::vector<atx::f64> lv;
    for (atx::usize s = 0; s < seq.n_samples; ++s) {
      if (seq.sample_valid[s] == 0U || seq.date_of[s] != date || oof_cnt[s] == 0U) {
        continue;
      }
      const atx::f64 label = y0[s];
      if (!std::isfinite(label)) {
        continue;
      }
      pv.push_back(oof_sum[s] / static_cast<atx::f64>(oof_cnt[s]));
      lv.push_back(label);
    }
    if (pv.size() >= 2U) {
      series.push_back(pearson(std::span<const atx::f64>{pv}, std::span<const atx::f64>{lv}));
    }
  }
  return series;
}

// The shared CPCV / OOF / blend / deploy core. fit_tcn / fit_gru differ ONLY in
// `build` (the FactoryBuilder) and the arch dims/params recorded on m.nn. `kind`
// tags the model; `horizons`, `cpcv`, `train` are the per-arm knobs.
[[nodiscard]] atx::core::Result<LearnedModel>
fit_seq_alpha(const SequenceTensor &seq, ModelKind kind, const FactoryBuilder &build,
              std::vector<atx::usize> arch_dims, std::vector<atx::f64> arch_params,
              const std::vector<atx::u16> &horizons, const eval::CpcvConfig &cpcv,
              const nn::TrainConfig &train) {
  if (seq.n_samples == 0U || seq.n_features == 0U || seq.lookback == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_seq_alpha: empty / zero-feature / zero-lookback tensor");
  }
  const atx::usize L = seq.lookback;
  const atx::usize F = seq.n_features;
  const atx::usize wlen = L * F;
  const nn::ModelFactory factory = build(L, F);

  LearnedModel m;
  m.kind = kind;
  m.n_base_features = static_cast<atx::u32>(wlen); // augmented_dim() == L*F (aug empty)
  m.horizons = horizons;
  m.trial_count = 0;
  // No explicit standardization: the net's LayerNorm normalises internally and
  // the firewall is the CPCV fold structure (train trailing folds, predict OOS),
  // not a per-element transform. Identity feat_mean/feat_sd make
  // build_augmented_row pass the verbatim flattened window through (M2: the same
  // bytes at train and predict). feat_sd == 1 (not 0) so the column is not zeroed.
  m.feat_mean.assign(wlen, 0.0);
  m.feat_sd.assign(wlen, 1.0);

  const std::vector<atx::usize> used_dates = seq_used_dates(seq);

  std::vector<atx::f64> oof_pred_sum(seq.n_samples, 0.0);
  std::vector<atx::u32> oof_pred_cnt(seq.n_samples, 0U);

  std::vector<atx::f64> oos_ic(horizons.size(), 0.0);

  for (atx::usize h = 0; h < horizons.size(); ++h) {
    const auto label_h = [&seq, h](atx::usize s) noexcept -> atx::f64 { return seq.y[h][s]; };

    const std::vector<eval::LabelSpan> spans = seq_label_spans(used_dates, horizons[h]);
    const std::vector<eval::CpcvFold> dfolds =
        eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cpcv);
    const std::vector<SampleFold> folds = expand_seq_folds(dfolds, seq, used_dates);

    std::vector<atx::f64> oos_pred;
    std::vector<atx::f64> oos_label;
    for (const SampleFold &f : folds) {
      lin::MatX xtr;
      lin::MatX ytr;
      const atx::usize ntr = build_seq_design(seq, std::span<const atx::usize>{f.train_samples},
                                              label_h, xtr, ytr, nullptr);
      lin::MatX xte;
      lin::MatX yte;
      std::vector<atx::usize> te_samples;
      const atx::usize nte = build_seq_design(seq, std::span<const atx::usize>{f.test_samples},
                                              label_h, xte, yte, &te_samples);
      if (ntr == 0U || nte == 0U) {
        continue; // no usable train OR no OOS test rows in this fold
      }
      nn::Adam opt{0.01};
      nn::MseLoss loss;
      const auto states = nn::train(factory, opt, loss, xtr, ytr, xte, yte, train);
      if (!states.has_value()) {
        continue; // a degenerate fold design -> no fit (no trial counted)
      }
      ++m.trial_count; // one distinct fold fit -> one deflation trial (§0.3)
      const auto pred = nn::ensemble_mean_predict(factory, *states, xte);
      if (!pred.has_value()) {
        continue;
      }
      for (atx::usize i = 0; i < nte; ++i) {
        const atx::f64 p = (*pred)(static_cast<Eigen::Index>(i), 0);
        oos_pred.push_back(p);
        oos_label.push_back(yte(static_cast<Eigen::Index>(i), 0));
        if (h == 0U) {
          oof_pred_sum[te_samples[i]] += p;
          oof_pred_cnt[te_samples[i]] += 1U;
        }
      }
    }
    oos_ic[h] = pearson(std::span<const atx::f64>{oos_pred}, std::span<const atx::f64>{oos_label});
  }

  // §0.6 horizon blend: normalize(max(oos_IC_h, 0)); uniform if all non-positive.
  m.blend_w.assign(horizons.size(), 0.0);
  atx::f64 wsum = 0.0;
  for (atx::usize h = 0; h < horizons.size(); ++h) {
    const atx::f64 w = (oos_ic[h] > 0.0) ? oos_ic[h] : 0.0;
    m.blend_w[h] = w;
    wsum += w;
  }
  if (wsum > 0.0) {
    for (atx::f64 &w : m.blend_w) {
      w /= wsum;
    }
  } else {
    const atx::f64 u =
        horizons.empty() ? 0.0 : 1.0 / static_cast<atx::f64>(horizons.size());
    for (atx::f64 &w : m.blend_w) {
      w = u;
    }
  }

  // The genuine per-date OOS skill series from the horizon-0 OOF predictions
  // (fold-local-fit, OOS-only -> no look-ahead), frozen for the deflation gate.
  m.oos_score_series = seq_oof_ic_series(seq, used_dates, std::span<const atx::f64>{oof_pred_sum},
                                         std::span<const atx::u32>{oof_pred_cnt});

  // DEPLOYED model: a SINGLE seed-ensemble refit on ALL valid samples (the full
  // trailing window) against the BLEND-WEIGHTED target Σ_h blend_w[h]·y[h][s].
  // Baking the §0.6 blend into the target keeps inference a single ascending-
  // member-mean forward (predict_nn), so the NN inherits build_augmented_row /
  // predict_blended cleanly without a per-horizon blend at eval time. Only
  // samples whose blended target is finite (every used horizon's label finite)
  // join the deployed fit.
  const auto blended_label = [&seq, &m, &horizons](atx::usize s) noexcept -> atx::f64 {
    atx::f64 acc = 0.0;
    for (atx::usize h = 0; h < horizons.size(); ++h) {
      // Skip ZERO-weight horizons before multiplying: max(IC,0) normalization can
      // set blend_w[h] == 0, and that horizon's label may legitimately be NaN (a
      // valid sample whose horizon-h forward return runs off the panel end). With
      // 0.0 * NaN == NaN, including the zero-weight term would poison the whole
      // blended target and silently drop a sample whose *contributing* (nonzero-
      // weight) labels are all finite — shrinking the deployed training set on
      // live data. Skipping zero-weight horizons fixes that; a NONZERO-weight
      // horizon with a NaN label still propagates NaN here, which correctly drops
      // the sample (its blended target is genuinely unknowable).
      if (m.blend_w[h] == 0.0) {
        continue;
      }
      acc += m.blend_w[h] * seq.y[h][s];
    }
    return acc; // non-finite iff any contributing (nonzero-weight) label is non-finite
  };
  std::vector<atx::usize> all_valid;
  all_valid.reserve(seq.n_samples);
  for (atx::usize s = 0; s < seq.n_samples; ++s) {
    if (seq.sample_valid[s] != 0U) {
      all_valid.push_back(s);
    }
  }
  lin::MatX xfull;
  lin::MatX yfull;
  const atx::usize nfull = build_seq_design(seq, std::span<const atx::usize>{all_valid},
                                            blended_label, xfull, yfull, nullptr);
  m.nn.lookback = L;
  m.nn.n_seq_features = F;
  m.nn.arch_dims = std::move(arch_dims);
  m.nn.arch_params = std::move(arch_params);
  if (nfull > 0U) {
    nn::Adam opt{0.01};
    nn::MseLoss loss;
    // INTENTIONAL: the deployed refit trains on ALL valid samples (the full
    // trailing window) and passes xfull/yfull as BOTH train and val, so the
    // Trainer's checkpoint-at-best selects on TRAINING loss — there is no
    // held-out carve-out here (unlike the CPCV folds above, which is where the
    // genuine OOS skill / deflation gate is established). Deployment legitimately
    // uses every observation; the firewall already ran in the fold loop.
    const auto states = nn::train(factory, opt, loss, xfull, yfull, xfull, yfull, train);
    if (states.has_value()) {
      m.nn.member_states = *states;
    }
  }
  return atx::core::Ok(std::move(m));
}

// Whether the deployed NN payload carries enough arch scalars to rebuild the
// factory. predict_nn is a PUBLIC path over the POD NnPayload, so a too-short
// arch_dims / arch_params (e.g. after a future load-from-disk / IPC that did not
// round-trip the full payload) must be caught BEFORE indexing — an OOB vector
// read is UB. The kind decides the required shape (Tcn: 3 dims + 1 param; Gru: 1
// dim). On violation predict_nn returns its 0.0 "no opinion" path.
[[nodiscard]] bool payload_arch_ok(const LearnedModel &m) noexcept {
  if (m.kind == ModelKind::Tcn) {
    return m.nn.arch_dims.size() >= 3U && !m.nn.arch_params.empty();
  }
  // ModelKind::Gru.
  return !m.nn.arch_dims.empty();
}

// Rebuild a ModelFactory from a deployed NN payload (kind decides the arch). The
// arch dims/params are exactly what fit_* recorded; the factory is a pure
// function of its member seed, so reloading + averaging is byte-deterministic.
// PRECONDITION: payload_arch_ok(m) (the caller, predict_nn, checks it first).
[[nodiscard]] nn::ModelFactory factory_from_payload(const LearnedModel &m) {
  const atx::usize L = m.nn.lookback;
  const atx::usize F = m.nn.n_seq_features;
  if (m.kind == ModelKind::Tcn) {
    // arch_dims = {blocks, kernel, channels}; arch_params = {dropout}.
    const atx::usize blocks = m.nn.arch_dims[0];
    const atx::usize kernel = m.nn.arch_dims[1];
    const atx::usize channels = m.nn.arch_dims[2];
    const atx::f64 dropout = m.nn.arch_params[0];
    return tcn_factory(L, F, blocks, kernel, channels, dropout);
  }
  // ModelKind::Gru — arch_dims = {hidden}.
  const atx::usize hidden = m.nn.arch_dims[0];
  return gru_factory(L, F, hidden);
}

} // namespace detail

atx::core::Result<LearnedModel> fit_tcn(const SequenceTensor &seq, const TcnAlphaCfg &cfg) {
  detail::FactoryBuilder build = [&cfg](atx::usize L, atx::usize F) -> nn::ModelFactory {
    return detail::tcn_factory(L, F, cfg.blocks, cfg.kernel, cfg.channels, cfg.dropout);
  };
  std::vector<atx::usize> arch_dims{cfg.blocks, cfg.kernel, cfg.channels};
  std::vector<atx::f64> arch_params{cfg.dropout};
  // Thread the advertised L2 knob into the Trainer (decoupled weight-decay).
  nn::TrainConfig train = cfg.train;
  train.l2 = cfg.l2;
  return detail::fit_seq_alpha(seq, ModelKind::Tcn, build, std::move(arch_dims),
                               std::move(arch_params), cfg.horizons, cfg.cpcv, train);
}

atx::core::Result<LearnedModel> fit_gru(const SequenceTensor &seq, const GruAlphaCfg &cfg) {
  detail::FactoryBuilder build = [&cfg](atx::usize L, atx::usize F) -> nn::ModelFactory {
    return detail::gru_factory(L, F, cfg.hidden);
  };
  std::vector<atx::usize> arch_dims{cfg.hidden};
  std::vector<atx::f64> arch_params{cfg.dropout};
  // Thread the advertised L2 knob into the Trainer (decoupled weight-decay).
  nn::TrainConfig train = cfg.train;
  train.l2 = cfg.l2;
  return detail::fit_seq_alpha(seq, ModelKind::Gru, build, std::move(arch_dims),
                               std::move(arch_params), cfg.horizons, cfg.cpcv, train);
}

atx::f64 predict_nn(const LearnedModel &m, std::span<const atx::f64> window_row) {
  if (m.nn.member_states.empty() || !detail::payload_arch_ok(m)) {
    // An undeployed model (no member states) OR a malformed payload (too-short
    // arch_dims / arch_params — e.g. a partial load-from-disk / IPC) emits no
    // opinion (0). Guarding here keeps factory_from_payload's unchecked indexing
    // safe (its precondition) on this public path.
    return 0.0;
  }
  const atx::usize wlen = m.nn.lookback * m.nn.n_seq_features;
  lin::MatX x(1, static_cast<Eigen::Index>(wlen));
  for (atx::usize j = 0; j < wlen; ++j) {
    x(0, static_cast<Eigen::Index>(j)) = window_row[j];
  }
  const nn::ModelFactory factory = detail::factory_from_payload(m);
  const auto pred = nn::ensemble_mean_predict(factory, m.nn.member_states, x);
  if (!pred.has_value()) {
    return 0.0;
  }
  return (*pred)(0, 0);
}

} // namespace atx::engine::learn
