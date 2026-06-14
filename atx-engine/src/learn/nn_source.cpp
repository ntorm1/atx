#include "atx/engine/learn/nn_source.hpp"

#include <cmath>   // std::isfinite
#include <limits>  // std::numeric_limits (NaN "no opinion" sentinel)
#include <span>    // std::span
#include <string>  // std::string (expr_source, kind tag)
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/hash.hpp"  // atx::core::hash_bytes (canon_hash over the streams)
#include "atx/core/macro.hpp" // ATX_CHECK

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics, compute_metrics
#include "atx/engine/library/record.hpp"  // library::Provenance

namespace atx::engine::learn {

namespace {

constexpr atx::f64 kNoOpinion = std::numeric_limits<atx::f64>::quiet_NaN();

// True iff `kind` is one of the sequence-NN arms predict_nn serves (L*F window).
[[nodiscard]] bool is_seq_nn_kind(ModelKind kind) noexcept {
  return kind == ModelKind::Tcn || kind == ModelKind::Gru || kind == ModelKind::Attn;
}

// Map a raw field name to its PanelField storage slot. ABORTS on an unknown name
// (a construction-time deployment error, not a per-evaluate runtime path) —
// mirrors LearnedSignalSource::panel_field_of so the two adapters resolve fields
// identically.
[[nodiscard]] PanelField panel_field_of(const std::string &name) {
  if (name == "open") {
    return PanelField::Open;
  }
  if (name == "high") {
    return PanelField::High;
  }
  if (name == "low") {
    return PanelField::Low;
  }
  if (name == "close") {
    return PanelField::Close;
  }
  if (name == "volume") {
    return PanelField::Volume;
  }
  ATX_CHECK(false); // unknown raw field name for the live PanelView adapter
  return PanelField::Close;
}

// The expr_source kind tag for a learned NN model: Tcn->"tcn", Gru->"gru",
// Attn->"attn", Autoencoder->"ae". The non-NN kinds are not bridged here.
[[nodiscard]] std::string kind_tag(ModelKind kind) {
  switch (kind) {
  case ModelKind::Tcn:
    return "tcn";
  case ModelKind::Gru:
    return "gru";
  case ModelKind::Attn:
    return "attn";
  case ModelKind::Autoencoder:
    return "ae";
  case ModelKind::Linear:
  case ModelKind::Gbt:
    break;
  }
  return "nn"; // non-NN kinds are not synthesized through nn_to_candidate
}

// The deterministic canon_hash over the synthesized streams: an order-fixed byte
// digest of (pnl ++ pos_flat). Distinct streams -> distinct hash, so a fresh NN
// candidate does not dedup against an empty library (mirrors stack_to_candidate).
[[nodiscard]] atx::u64 hash_streams(const std::vector<atx::f64> &pnl,
                                    const std::vector<atx::f64> &pos_flat) {
  std::vector<atx::f64> hbuf;
  hbuf.reserve(pnl.size() + pos_flat.size());
  hbuf.insert(hbuf.end(), pnl.begin(), pnl.end());
  hbuf.insert(hbuf.end(), pos_flat.begin(), pos_flat.end());
  // SAFETY: std::vector<f64> stores doubles contiguously; hbuf.data() points at
  // hbuf.size()*sizeof(f64) live bytes for the duration of the hash call.
  return atx::core::hash_bytes(hbuf.data(), hbuf.size() * sizeof(atx::f64));
}

// Predict one sample's score for a fitted NN model. Sequence kinds (Tcn|Gru|Attn)
// run predict_nn over the L*F window; the Autoencoder runs predict_ae over the
// trailing-step F-dim vector (the AE input is the newest cross-sectional feature
// row, NOT the flattened window). `seq.x` is the time-major store
// idx(s,l,f) = (s*L + l)*F + f, so the window is x[s*L*F .. +L*F) and the trailing
// step is x[(s*L + (L-1))*F .. +F).
[[nodiscard]] atx::f64 predict_sample(const LearnedModel &m, const SequenceTensor &seq,
                                      atx::usize s) {
  const atx::usize L = seq.lookback;
  const atx::usize F = seq.n_features;
  const atx::usize wlen = L * F;
  if (m.kind == ModelKind::Autoencoder) {
    // The AE consumes the F-dim newest-step cross-sectional feature vector.
    const atx::usize off = (s * L + (L - 1U)) * F;
    return predict_ae(m, std::span<const atx::f64>{seq.x.data() + off, F});
  }
  // Sequence-NN window (Tcn|Gru|Attn).
  return predict_nn(m, std::span<const atx::f64>{seq.x.data() + s * wlen, wlen});
}

} // namespace

// ---------------------------------------------------------------------------
//  SeqLearnedSignalSource
// ---------------------------------------------------------------------------

SeqLearnedSignalSource::SeqLearnedSignalSource(LearnedModel model,
                                               std::vector<std::string> raw_fields,
                                               atx::usize lookback, atx::usize universe_size)
    : model_{std::move(model)}, lookback_{lookback}, universe_{universe_size},
      n_features_{raw_fields.size()} {
  // The live adapter serves ONLY the sequence-NN arms (predict_nn over an L*F
  // window). A non-NN (Linear/Gbt) or AE model is unrepresentable here.
  ATX_CHECK(is_seq_nn_kind(model_.kind));
  // Train/eval feature parity: F must match the deployed payload, L*F the base dim,
  // and L the deployed window depth (so the live window is the exact shape the
  // ensemble was trained on).
  ATX_CHECK(n_features_ == model_.nn.n_seq_features);
  ATX_CHECK(lookback_ == model_.nn.lookback);
  ATX_CHECK(lookback_ > 0U && n_features_ > 0U);
  ATX_CHECK(lookback_ * n_features_ == static_cast<atx::usize>(model_.n_base_features));

  // Resolve each raw field name -> PanelField (aborts on an unknown name).
  fields_.reserve(raw_fields.size());
  for (const std::string &name : raw_fields) {
    fields_.push_back(panel_field_of(name));
  }

  // Pre-size the adapter's OWN scratch so evaluate() reuses it (the adapter adds no
  // per-call allocation). NOTE: predict_nn's per-instrument forward still allocates
  // its activations (MatX by value) — a substrate-level residual, not the adapter's
  // (see the class header note); evaluate() is therefore not globally alloc-free.
  window_.assign(lookback_ * n_features_, 0.0);
  out_.assign(universe_, kNoOpinion);
}

atx::usize SeqLearnedSignalSource::max_lookback() const noexcept {
  // The window spans L rows (the current row + L-1 of history); the loop provisions
  // L-1 trailing rows BELOW the current one. lookback_ >= 1 by construction.
  return lookback_ - 1U;
}

atx::f64 SeqLearnedSignalSource::read_field(PanelView panel, PanelField f, atx::usize row,
                                            atx::usize inst) {
  switch (f) {
  case PanelField::Open:
    return panel.open(row, inst);
  case PanelField::High:
    return panel.high(row, inst);
  case PanelField::Low:
    return panel.low(row, inst);
  case PanelField::Close:
    return panel.close(row, inst);
  case PanelField::Volume:
    return panel.volume(row, inst);
  }
  return kNoOpinion; // unreachable: every PanelField handled (no default).
}

bool SeqLearnedSignalSource::fill_window(PanelView panel, atx::usize inst) {
  bool all_finite = true;
  // Window position l=0 = OLDEST (date t-L+1), l=L-1 = NEWEST (current). PanelView
  // row 0 = NEWEST, so step l reads row (L-1)-l — the REVERSAL that aligns the
  // newest-first ring storage with the oldest-first training convention (S5-1). The
  // time-major flat index is l*F + f (idx(l,f), matching the SequenceTensor store).
  for (atx::usize l = 0; l < lookback_; ++l) {
    const atx::usize row = (lookback_ - 1U) - l; // l=L-1 -> row 0 (newest); l=0 -> row L-1
    if (!panel.present(row, inst)) {
      all_finite = false; // a universe gap mid-window -> "no opinion" (never zero-fill)
    }
    for (atx::usize f = 0; f < n_features_; ++f) {
      const atx::f64 v = read_field(panel, fields_[f], row, inst);
      window_[l * n_features_ + f] = v;
      all_finite = all_finite && std::isfinite(v);
    }
  }
  return all_finite;
}

atx::core::Result<SignalView> SeqLearnedSignalSource::evaluate(PanelView panel) {
  const atx::usize inst = panel.instruments();
  const atx::usize n = (inst < universe_) ? inst : universe_;
  // Reset every slot to "no opinion" in place (out_ is pre-sized — no adapter-level
  // allocation here; the per-instrument predict_nn forward below still allocates).
  for (atx::usize i = 0; i < universe_; ++i) {
    out_[i] = kNoOpinion;
  }
  // Insufficient trailing history -> the whole cross-section is "no opinion".
  if (panel.rows() < lookback_) {
    return atx::core::Ok(SignalView{std::span<const atx::f64>{out_}});
  }
  for (atx::usize i = 0; i < n; ++i) {
    if (fill_window(panel, i)) {
      out_[i] = predict_nn(model_, std::span<const atx::f64>{window_});
    }
    // else: a non-present / non-finite window cell leaves out_[i] == NaN.
  }
  return atx::core::Ok(SignalView{std::span<const atx::f64>{out_}});
}

// ---------------------------------------------------------------------------
//  nn_to_candidate — the NN -> library bridge (the stack_to_candidate analog).
// ---------------------------------------------------------------------------

NnCandidate nn_to_candidate(const LearnedModel &model, const SequenceTensor &seq,
                            atx::usize n_instruments, atx::u64 seed) {
  // n_dates = max(date_of)+1 (0 for an empty tensor). The streams are date-major
  // with n_instruments-wide cross-sections, exactly the library's pos_flat layout.
  atx::usize n_dates = 0;
  for (const atx::usize d : seq.date_of) {
    if (d + 1U > n_dates) {
      n_dates = d + 1U;
    }
  }

  NnCandidate out;
  out.pnl.assign(n_dates, 0.0);
  out.pos_flat.assign(n_dates * n_instruments, 0.0);

  // Per valid sample (ascending), predict its score, scatter it to its (date, inst)
  // slot, and accumulate pnl[date] += score * forward_return (the horizon-0 label,
  // read directly as stack_to_candidate reads meta.Y[0][r] — present iff seq carries
  // labels; a label-less tensor leaves pnl at 0, a benign degenerate stream).
  const bool has_labels = !seq.y.empty();
  for (atx::usize s = 0; s < seq.n_samples; ++s) {
    if (seq.sample_valid[s] == 0U) {
      continue; // an incomplete / non-finite window contributes no position
    }
    const atx::usize d = seq.date_of[s];
    const atx::usize i = seq.inst_of[s];
    ATX_CHECK(d < n_dates && i < n_instruments); // the sample indexes the cross-section
    const atx::f64 score = predict_sample(model, seq, s);
    out.pos_flat[d * n_instruments + i] = score;
    if (has_labels) {
      const atx::f64 fwd = seq.y[0][s];
      if (std::isfinite(fwd)) {
        out.pnl[d] += score * fwd;
      }
    }
  }

  // Metrics over the synthesized streams (book_size 1.0 — the scores are a gross
  // cross-section, not normalized notionals; the same convention stack_to_candidate
  // uses for the dedup-distinctness check).
  const combine::AlphaMetrics metrics =
      combine::compute_metrics(std::span<const atx::f64>{out.pnl},
                               std::span<const atx::f64>{out.pos_flat}, n_instruments,
                               /*book_size=*/1.0);

  library::Provenance prov;
  prov.expr_source = std::string("learned:") + kind_tag(model.kind);
  prov.seed = seed;

  out.candidate.canon_hash = hash_streams(out.pnl, out.pos_flat);
  out.candidate.pnl = std::span<const atx::f64>{out.pnl};
  out.candidate.pos_flat = std::span<const atx::f64>{out.pos_flat};
  out.candidate.metrics = metrics;
  out.candidate.prov = std::move(prov);
  out.candidate.as_of = (n_dates == 0U) ? 0U : (n_dates - 1U);
  out.candidate.source = nullptr; // no live re-eval handle wired here (tests pass nullptr)
  return out;
}

} // namespace atx::engine::learn
