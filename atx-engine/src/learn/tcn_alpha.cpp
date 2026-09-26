#include "atx/engine/learn/tcn_alpha.hpp"

#include <algorithm> // std::sort, std::unique
#include <cmath>     // std::isfinite, std::ceil (inner block / embargo length)
#include <cstddef>   // std::ptrdiff_t (inner block slice)
#include <functional> // std::function (the factory-builder seam)
#include <limits>    // std::numeric_limits (trace NaN for uncovered samples)
#include <memory>    // std::unique_ptr, std::make_unique
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX

#include "atx/core/error.hpp"  // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp"  // ATX_CHECK
#include "atx/core/random.hpp" // Xoshiro256pp (seeded param init)
#include "atx/core/types.hpp"  // f64, u16, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/eval/cpcv_date.hpp"        // eval::CpcvConfig, eval::cpcv_folds, eval::LabelSpan
#include "atx/engine/learn/latent.hpp"     // detail::pearson (reused, order-fixed)
#include "atx/engine/learn/nn/layers.hpp"  // nn::Linear, nn::Dropout
#include "atx/engine/learn/nn/loss.hpp"    // nn::MseLoss
#include "atx/engine/learn/nn/module.hpp"  // nn::Module, nn::Sequential
#include "atx/engine/learn/nn/optimizer.hpp"  // nn::Adam
#include "atx/engine/learn/nn/seq_layers.hpp" // nn::TcnResidualBlock, nn::GruCell, nn::SeqLastStep, nn::Attention1Head
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

// The ATTENTION-LITE factory (p2 S5-3a): Sequential{ Attention1Head(F->d_model)
// [-> Dropout] -> SeqLastStep(T=L, C=d_model) -> Linear(d_model->1) }. The single
// causal head maps each window to a (L, d_model) sequence; SeqLastStep pools the
// trailing step (the newest causal summary, R2) and the Linear head scores it. The
// Dropout (when dropout > 0) bases its mask stream on a per-build seed (tag
// "attn-drop"); the member seed seed-inits the whole flat param buffer.
[[nodiscard]] nn::ModelFactory attn_factory(atx::usize L, atx::usize F, atx::usize d_model,
                                            atx::f64 dropout) {
  return [L, F, d_model, dropout](atx::u64 seed) -> std::unique_ptr<nn::Module> {
    auto seq = std::make_unique<nn::Sequential>();
    seq->add(std::make_unique<nn::Attention1Head>(L, F, d_model, /*bias=*/true));
    if (dropout > 0.0) {
      seq->add(std::make_unique<nn::Dropout>(dropout, seed_for(seed, "attn-drop", 0U, 0U)));
    }
    seq->add(std::make_unique<nn::SeqLastStep>(L, d_model));
    seq->add(std::make_unique<nn::Linear>(d_model, static_cast<atx::usize>(1), /*bias=*/true));
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

// The VALID samples anchored at the dates named by `ordinals` (ordinals into
// used_dates), ascending by sample.
[[nodiscard]] std::vector<atx::usize> samples_for_ordinals(const SequenceTensor &seq,
                                                           const std::vector<atx::usize> &used_dates,
                                                           std::span<const atx::usize> ordinals) {
  return samples_for_dates(seq, date_membership(used_dates, ordinals));
}

InnerSplit inner_purged_split(std::span<const eval::LabelSpan> spans,
                              std::span<const atx::usize> train_ord, atx::f64 frac,
                              atx::usize embargo_len) {
  InnerSplit out;
  const atx::usize n = train_ord.size();
  if (n == 0U || !(frac > 0.0) || frac > 0.5) {
    return out; // nothing to split, or an out-of-contract fraction: degenerate
  }
  atx::usize n_val = static_cast<atx::usize>(std::ceil(frac * static_cast<atx::f64>(n)));
  n_val = (n_val == 0U) ? 1U : ((n_val > n) ? n : n_val);
  const atx::usize first_val = n - n_val;
  out.val.assign(train_ord.begin() + static_cast<std::ptrdiff_t>(first_val), train_ord.end());
  // Reuse the eval::cpcv purge + embargo rule with the block as the "test" set: it
  // returns every ordinal of the axis that survives; keep those that are inner-train
  // candidates (the fold's train ordinals before the block).
  std::vector<bool> is_val(spans.size(), false);
  for (const atx::usize o : out.val) {
    ATX_CHECK(o < spans.size());
    is_val[o] = true;
  }
  const std::vector<atx::usize> survivors = eval::detail::purged_embargoed_train(
      spans, std::span<const atx::usize>{out.val}, is_val, embargo_len);
  std::vector<bool> keep(spans.size(), false);
  for (const atx::usize o : survivors) {
    keep[o] = true;
  }
  for (atx::usize i = 0; i < first_val; ++i) {
    const atx::usize o = train_ord[i];
    ATX_CHECK(o < spans.size());
    if (keep[o]) {
      out.inner_train.push_back(o);
    }
  }
  return out;
}

[[nodiscard]] atx::core::Result<InnerSplit> inner_split_checked(
    std::span<const eval::LabelSpan> spans, std::span<const atx::usize> train_ord,
    atx::f64 frac, atx::usize embargo, eval::CpcvRule rule) {
  if (rule == eval::CpcvRule::ObservationV1)
    return atx::core::Ok(inner_purged_split(spans, train_ord, frac, embargo));
  InnerSplit out;
  if (train_ord.empty()) return atx::core::Ok(std::move(out));
  const auto n_val = std::max<atx::usize>(1U, static_cast<atx::usize>(
      std::ceil(frac * static_cast<atx::f64>(train_ord.size()))));
  const auto boundary = train_ord.size() - n_val;
  out.val.assign(train_ord.begin() + static_cast<std::ptrdiff_t>(boundary), train_ord.end());
  ATX_TRY(auto kept, eval::cpcv_date_train(spans, train_ord.first(boundary), out.val, embargo));
  out.inner_train = std::move(kept);
  return atx::core::Ok(std::move(out));
}

[[nodiscard]] atx::core::Result<std::vector<eval::LabelSpan>> seq_spans_checked(
    const std::vector<atx::usize>& dates, atx::u16 horizon, eval::CpcvRule rule) {
  if (rule == eval::CpcvRule::ObservationV1)
    return atx::core::Ok(seq_label_spans(dates, horizon));
  return date_label_spans_v2(dates, horizon);
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

// Flatten a trained seed-ensemble (member states, ascending member order) into a
// LearnFitTrace artifact: member count, then each member's size + params.
[[nodiscard]] std::vector<atx::f64>
states_artifact(const std::vector<std::vector<atx::f64>> &states) {
  std::vector<atx::f64> a;
  a.push_back(static_cast<atx::f64>(states.size()));
  for (const std::vector<atx::f64> &s : states) {
    a.push_back(static_cast<atx::f64>(s.size()));
    a.insert(a.end(), s.begin(), s.end());
  }
  return a;
}

// Whether a protocol needs a valid inner-validation fraction.
[[nodiscard]] bool uses_inner_split(const SeqFitProtocol &p) noexcept {
  return p.validation == SeqValidationRule::InnerPurgedV2 || p.deploy == SeqDeployRule::InnerValV2;
}

// The CPCV embargo length over an n-date axis — the same ceil(embargo * n) cpcv_folds
// applies, reused for the inner block so the inner split is as strict as the outer.
[[nodiscard]] atx::usize embargo_len_of(const eval::CpcvConfig &cpcv, atx::usize n) {
  return static_cast<atx::usize>(std::ceil(cpcv.embargo * static_cast<atx::f64>(n)));
}

// The shared CPCV / OOF / blend / deploy core. fit_tcn / fit_gru differ ONLY in
// `build` (the FactoryBuilder) and the arch dims/params recorded on m.nn. `kind`
// tags the model; `horizons`, `cpcv`, `train`, `proto` are the per-arm knobs.
[[nodiscard]] atx::core::Result<LearnedModel>
fit_seq_alpha(const SequenceTensor &seq, ModelKind kind, const FactoryBuilder &build,
              std::vector<atx::usize> arch_dims, std::vector<atx::f64> arch_params,
              const std::vector<atx::u16> &horizons, const eval::CpcvConfig &cpcv,
              const nn::TrainConfig &train, const SeqFitProtocol &proto,
              LearnFitTrace *trace) {
  if (seq.n_samples == 0U || seq.n_features == 0U || seq.lookback == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_seq_alpha: empty / zero-feature / zero-lookback tensor");
  }
  if (uses_inner_split(proto) && !(proto.inner_val_frac > 0.0 && proto.inner_val_frac <= 0.5)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_seq_alpha: protocol.inner_val_frac must be in (0, 0.5]");
  }
  // The embargo fraction is turned into a date count below (embargo_len_of); a NaN or
  // out-of-range value would make that float->usize conversion undefined.
  if (cpcv.rule == eval::CpcvRule::ObservationV1 &&
      !(cpcv.embargo >= 0.0 && cpcv.embargo <= 1.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_seq_alpha: cpcv.embargo must be in [0, 1]");
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
  if (trace != nullptr) {
    *trace = LearnFitTrace{}; // a reused trace starts empty
  }

  const std::vector<atx::usize> used_dates = seq_used_dates(seq);
  if (cpcv.rule != eval::CpcvRule::ObservationV1 &&
      (cpcv.rule != eval::CpcvRule::DateV2 || horizons.empty() ||
       used_dates.size() > cpcv.max_working_bytes / 128U))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "sequence DateV2: rule/horizon/span budget");

  const atx::usize embargo_len = cpcv.rule == eval::CpcvRule::DateV2
      ? cpcv.embargo_dates : embargo_len_of(cpcv, used_dates.size());

  std::vector<atx::f64> oof_pred_sum(seq.n_samples, 0.0);
  std::vector<atx::u32> oof_pred_cnt(seq.n_samples, 0U);

  std::vector<atx::f64> oos_ic(horizons.size(), 0.0);
  atx::usize n_fold_fits = 0; // successful fold fits (TrialCountRule input, L-08)

  for (atx::usize h = 0; h < horizons.size(); ++h) {
    const auto label_h = [&seq, h](atx::usize s) noexcept -> atx::f64 { return seq.y[h][s]; };

    ATX_TRY(auto spans, seq_spans_checked(used_dates, horizons[h], cpcv.rule));
    ATX_TRY(auto plan, eval::cpcv_plan(spans, cpcv));
    const auto& dfolds = plan.folds;
    if (cpcv.rule == eval::CpcvRule::DateV2)
      m.cpcv_metadata.push_back(std::move(plan.metadata));

    std::vector<atx::f64> oos_pred;
    std::vector<atx::f64> oos_label;
    std::vector<atx::f64> oof_sum_h(seq.n_samples, 0.0); // horizon-h OOF (MeanDateIcV2)
    std::vector<atx::u32> oof_cnt_h(seq.n_samples, 0U);
    for (atx::usize fold_idx = 0; fold_idx < dfolds.size(); ++fold_idx) {
      const eval::CpcvFold &df = dfolds[fold_idx];
      const std::vector<atx::usize> test_samples =
          samples_for_ordinals(seq, used_dates, std::span<const atx::usize>{df.test_idx});
      // The checkpoint-selection split (L-01). TestFoldV1 (legacy) trains on every
      // train date and validates on the TEST fold; InnerPurgedV2 carves a purged inner
      // validation block out of the TRAIN dates so the test fold is only predicted.
      std::vector<atx::usize> fit_samples;
      std::vector<atx::usize> val_samples;
      switch (proto.validation) {
      case SeqValidationRule::TestFoldV1:
        fit_samples =
            samples_for_ordinals(seq, used_dates, std::span<const atx::usize>{df.train_idx});
        val_samples = test_samples;
        break;
      case SeqValidationRule::InnerPurgedV2: {
        ATX_TRY(auto split, inner_split_checked(std::span<const eval::LabelSpan>{spans},
                               std::span<const atx::usize>{df.train_idx},
                               proto.inner_val_frac, embargo_len, cpcv.rule));
        fit_samples = samples_for_ordinals(seq, used_dates,
                                           std::span<const atx::usize>{split.inner_train});
        val_samples =
            samples_for_ordinals(seq, used_dates, std::span<const atx::usize>{split.val});
        break;
      }
      }
      lin::MatX xtr;
      lin::MatX ytr;
      const atx::usize ntr = build_seq_design(seq, std::span<const atx::usize>{fit_samples},
                                              label_h, xtr, ytr, nullptr);
      lin::MatX xval;
      lin::MatX yval;
      const atx::usize nval = build_seq_design(seq, std::span<const atx::usize>{val_samples},
                                               label_h, xval, yval, nullptr);
      lin::MatX xte;
      lin::MatX yte;
      std::vector<atx::usize> te_samples;
      const atx::usize nte = build_seq_design(seq, std::span<const atx::usize>{test_samples},
                                              label_h, xte, yte, &te_samples);
      // No usable train rows, no OOS test rows, or (degenerate split) no validation
      // rows -> no fit: an empty validation set would silently fall back to
      // selecting on training loss.
      if (ntr == 0U || nte == 0U || nval == 0U) {
        continue;
      }
      nn::Adam opt{0.01};
      nn::MseLoss loss;
      const auto states = nn::train(factory, opt, loss, xtr, ytr, xval, yval, train);
      if (!states.has_value()) {
        continue; // a degenerate fold design -> no fit (no trial counted)
      }
      ++n_fold_fits;
      const auto pred = nn::ensemble_mean_predict(factory, *states, xte);
      if (!pred.has_value()) {
        continue;
      }
      LearnFoldRecord rec;
      for (atx::usize i = 0; i < nte; ++i) {
        const atx::f64 p = (*pred)(static_cast<Eigen::Index>(i), 0);
        oos_pred.push_back(p);
        oos_label.push_back(yte(static_cast<Eigen::Index>(i), 0));
        oof_sum_h[te_samples[i]] += p;
        oof_cnt_h[te_samples[i]] += 1U;
        if (h == 0U) {
          oof_pred_sum[te_samples[i]] += p;
          oof_pred_cnt[te_samples[i]] += 1U;
        }
      }
      if (trace != nullptr) {
        rec.horizon_idx = h;
        rec.fold_idx = fold_idx;
        rec.test_keys = te_samples;
        rec.test_pred.reserve(nte);
        for (atx::usize i = 0; i < nte; ++i) {
          rec.test_pred.push_back((*pred)(static_cast<Eigen::Index>(i), 0));
        }
        rec.artifact = states_artifact(*states);
        rec.fit_keys = fit_samples;
        rec.val_keys = val_samples;
        trace->folds.push_back(std::move(rec));
      }
    }
    oos_ic[h] = (proto.common.blend_ic == HorizonBlendIc::PooledPearsonV1)
                    ? pearson(std::span<const atx::f64>{oos_pred},
                              std::span<const atx::f64>{oos_label})
                    : oof_mean_date_ic(std::span<const atx::usize>{seq.date_of},
                                       std::span<const atx::f64>{seq.y[h]},
                                       std::span<const atx::f64>{oof_sum_h},
                                       std::span<const atx::u32>{oof_cnt_h});
  }
  m.trial_count = protocol_trial_count(proto.common.trials, n_fold_fits);

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
  if (trace != nullptr) {
    trace->oof_cnt = oof_pred_cnt;
    trace->oof_pred.assign(seq.n_samples, std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize s = 0; s < seq.n_samples; ++s) {
      if (oof_pred_cnt[s] > 0U) {
        trace->oof_pred[s] = oof_pred_sum[s] / static_cast<atx::f64>(oof_pred_cnt[s]);
      }
    }
  }

  // DEPLOYED model: a SINGLE seed-ensemble refit over the full trailing window
  // against the BLEND-WEIGHTED target Σ_h blend_w[h]·y[h][s]. Baking the §0.6 blend
  // into the target keeps inference a single ascending-member-mean forward
  // (predict_nn), so the NN inherits build_augmented_row / predict_blended cleanly
  // without a per-horizon blend at eval time. Only samples whose blended target is
  // finite (every used horizon's label finite) join the deployed fit.
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
  m.nn.lookback = L;
  m.nn.n_seq_features = F;
  m.nn.arch_dims = std::move(arch_dims);
  m.nn.arch_params = std::move(arch_params);

  // The deployed fit/validation split (L-07). TrainLossV1 (legacy) fits every valid
  // sample and passes the same design as validation, so the checkpoint is chosen on
  // TRAINING loss. InnerValV2 carves the window's inner validation block (the latest
  // dates, purged at the longest horizon the blend actually uses) and checkpoints on
  // it — the protocol the CPCV folds above evaluated.
  std::vector<atx::usize> fit_samples;
  std::vector<atx::usize> val_samples;
  switch (proto.deploy) {
  case SeqDeployRule::TrainLossV1:
    for (atx::usize s = 0; s < seq.n_samples; ++s) {
      if (seq.sample_valid[s] != 0U) {
        fit_samples.push_back(s);
      }
    }
    val_samples = fit_samples;
    break;
  case SeqDeployRule::InnerValV2: {
    atx::u16 h_dep = 0;
    for (atx::usize h = 0; h < horizons.size(); ++h) {
      if (m.blend_w[h] != 0.0 && horizons[h] > h_dep) {
        h_dep = horizons[h];
      }
    }
    if (cpcv.rule == eval::CpcvRule::DateV2)
      for (const auto horizon : horizons) h_dep = std::max(h_dep, horizon);
    ATX_TRY(auto spans, seq_spans_checked(used_dates, h_dep, cpcv.rule));
    std::vector<atx::usize> all_ord(used_dates.size());
    for (atx::usize o = 0; o < all_ord.size(); ++o) {
      all_ord[o] = o;
    }
    ATX_TRY(auto split, inner_split_checked(std::span<const eval::LabelSpan>{spans},
                           std::span<const atx::usize>{all_ord}, proto.inner_val_frac,
                           embargo_len, cpcv.rule));
    fit_samples =
        samples_for_ordinals(seq, used_dates, std::span<const atx::usize>{split.inner_train});
    val_samples = samples_for_ordinals(seq, used_dates, std::span<const atx::usize>{split.val});
    break;
  }
  }
  if (trace != nullptr) {
    trace->deploy_fit_keys = fit_samples;
    trace->deploy_val_keys = val_samples;
  }
  lin::MatX xfit;
  lin::MatX yfit;
  const atx::usize nfit = build_seq_design(seq, std::span<const atx::usize>{fit_samples},
                                           blended_label, xfit, yfit, nullptr);
  lin::MatX xval;
  lin::MatX yval;
  const atx::usize nval = build_seq_design(seq, std::span<const atx::usize>{val_samples},
                                           blended_label, xval, yval, nullptr);
  // A degenerate deploy split (no fit or no validation rows) leaves the model
  // undeployed (predict_nn -> 0, "no opinion") instead of selecting on training loss.
  if (nfit > 0U && nval > 0U) {
    nn::Adam opt{0.01};
    nn::MseLoss loss;
    const auto states = nn::train(factory, opt, loss, xfit, yfit, xval, yval, train);
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
  if (m.kind == ModelKind::Attn) {
    // arch_dims = {d_model}; arch_params = {dropout}.
    return !m.nn.arch_dims.empty() && !m.nn.arch_params.empty();
  }
  // ModelKind::Gru — arch_dims = {hidden}.
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
  if (m.kind == ModelKind::Attn) {
    // arch_dims = {d_model}; arch_params = {dropout}.
    const atx::usize d_model = m.nn.arch_dims[0];
    const atx::f64 dropout = m.nn.arch_params[0];
    return attn_factory(L, F, d_model, dropout);
  }
  // ModelKind::Gru — arch_dims = {hidden}.
  const atx::usize hidden = m.nn.arch_dims[0];
  return gru_factory(L, F, hidden);
}

} // namespace detail

atx::core::Result<LearnedModel> fit_tcn(const SequenceTensor &seq, const TcnAlphaCfg &cfg) {
  return fit_tcn(seq, cfg, nullptr);
}

atx::core::Result<LearnedModel> fit_tcn(const SequenceTensor &seq, const TcnAlphaCfg &cfg,
                                         LearnFitTrace *trace) {
  detail::FactoryBuilder build = [&cfg](atx::usize L, atx::usize F) -> nn::ModelFactory {
    return detail::tcn_factory(L, F, cfg.blocks, cfg.kernel, cfg.channels, cfg.dropout);
  };
  std::vector<atx::usize> arch_dims{cfg.blocks, cfg.kernel, cfg.channels};
  std::vector<atx::f64> arch_params{cfg.dropout};
  // Thread the advertised L2 knob into the Trainer (decoupled weight-decay).
  nn::TrainConfig train = cfg.train;
  train.l2 = cfg.l2;
  return detail::fit_seq_alpha(seq, ModelKind::Tcn, build, std::move(arch_dims),
                               std::move(arch_params), cfg.horizons, cfg.cpcv, train,
                               cfg.protocol, trace);
}

atx::core::Result<LearnedModel> fit_gru(const SequenceTensor &seq, const GruAlphaCfg &cfg) {
  return fit_gru(seq, cfg, nullptr);
}

atx::core::Result<LearnedModel> fit_gru(const SequenceTensor &seq, const GruAlphaCfg &cfg,
                                         LearnFitTrace *trace) {
  detail::FactoryBuilder build = [&cfg](atx::usize L, atx::usize F) -> nn::ModelFactory {
    return detail::gru_factory(L, F, cfg.hidden);
  };
  std::vector<atx::usize> arch_dims{cfg.hidden};
  std::vector<atx::f64> arch_params{cfg.dropout};
  // Thread the advertised L2 knob into the Trainer (decoupled weight-decay).
  nn::TrainConfig train = cfg.train;
  train.l2 = cfg.l2;
  return detail::fit_seq_alpha(seq, ModelKind::Gru, build, std::move(arch_dims),
                               std::move(arch_params), cfg.horizons, cfg.cpcv, train,
                               cfg.protocol, trace);
}

atx::core::Result<LearnedModel> fit_attn(const SequenceTensor &seq, const AttnAlphaCfg &cfg) {
  return fit_attn(seq, cfg, nullptr);
}

atx::core::Result<LearnedModel> fit_attn(const SequenceTensor &seq, const AttnAlphaCfg &cfg,
                                         LearnFitTrace *trace) {
  detail::FactoryBuilder build = [&cfg](atx::usize L, atx::usize F) -> nn::ModelFactory {
    return detail::attn_factory(L, F, cfg.d_model, cfg.dropout);
  };
  // arch_dims = {d_model}, arch_params = {dropout} (factory_from_payload rebuild).
  std::vector<atx::usize> arch_dims{cfg.d_model};
  std::vector<atx::f64> arch_params{cfg.dropout};
  // Thread the advertised L2 knob into the Trainer (decoupled weight-decay).
  nn::TrainConfig train = cfg.train;
  train.l2 = cfg.l2;
  return detail::fit_seq_alpha(seq, ModelKind::Attn, build, std::move(arch_dims),
                               std::move(arch_params), cfg.horizons, cfg.cpcv, train,
                               cfg.protocol, trace);
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
