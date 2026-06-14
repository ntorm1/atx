#pragma once

// atx::engine::learn::nn — sequence-specific layers (S5-2b-i): Conv1dCausal,
// TcnResidualBlock, GruCell, MguCell, SeqLastStep. Each is a Module with a
// forward and a HAND-WRITTEN backward; the finite-difference gradient check in
// the test is the correctness proof (R3). These extend the S5-2a substrate
// (Module / Sequential / Residual / Linear / LayerNorm / ReLU / Dropout) with the
// time-axis machinery the four deep-learning alphas need. PURE framework — no
// alpha types, no LearnedModel here.
//
// =====================================================================
//  The sequence-tensor encoding convention (decide once, keep forever)
// =====================================================================
//  A sequence SAMPLE is (time T x channels C). The Module interface is
//  forward(MatX)->MatX over "samples x features", so a BATCH of B samples is
//  encoded as a MatX with **B rows**, each row a flattened window of T*C values:
//
//        row r, column (t * C + c)  ==  channel c at time step t of sample r.
//
//  This is TIME-MAJOR within a row — column index runs c fastest, then t — which
//  is exactly the S5-1 Seq3 per-sample layout idx(t,f) = t*F + f (tensor.hpp).
//  Every sequence layer carries its own T / C_in / C_out and reshapes each row
//  internally; nothing outside this file needs to know the packing.
//
//  Per-layer shapes (B = batch rows):
//    - Conv1dCausal : forward  MatX(B, T*C_in)  -> MatX(B, T*C_out)
//                     backward  MatX(B, T*C_out) -> MatX(B, T*C_in)
//    - TcnResidualBlock : MatX(B, T*C) -> MatX(B, T*C)  (channels preserved)
//    - GruCell / MguCell : MatX(B, T*C_in) -> MatX(B, hidden)  (FINAL hidden state)
//    - SeqLastStep  : MatX(B, T*C) -> MatX(B, C)  (the trailing step t = T-1)
//
//  The alpha head in S5-2b-ii is a plain Linear mapping (B, C) -> (B, horizons).
//
// =====================================================================
//  Causality (R2 — the structural look-ahead guard)
// =====================================================================
//  Conv1dCausal output at time t depends ONLY on inputs at times
//  t, t - dilation, t - 2*dilation, ... that are >= 0 (LEFT zero-pad; the kernel
//  NEVER reads t+1). A stack of causal convs is causal, so TcnResidualBlock is
//  causal too. The recurrent cells consume the window strictly ascending in time,
//  returning the final hidden state — also causal. SeqLastStep reads only t=T-1.
//
// =====================================================================
//  Parameter contract (matches module.hpp / layers.hpp exactly)
// =====================================================================
//  Leaves DO NOT own their parameter bytes: param_count() declares the scalar
//  budget; bind_params(p, g) aliases caller-owned contiguous storage. The flat
//  layout is FIXED and ascending, and serialize (state_to/from) walks the SAME
//  order so the determinism digest == ascending param order (R1).
//
//  Conv1dCausal flat layout (ascending):
//    [ W : K*C_in*C_out scalars, index ((k*C_in + ci)*C_out + co) ]
//    [ b : C_out scalars (when biased) ]
//  i.e. weight is addressed (kernel tap k slowest, then input channel ci, then
//  output channel co fastest). This explicit order is used for BOTH bind and
//  serialize.
//
//  GruCell / MguCell stack their gate matrices in a fixed ascending order; the
//  per-cell doc below the class fixes that order. Containers (TcnResidualBlock)
//  own one flat buffer and bind children in ascending sub-module order, exactly
//  like Sequential.
//
//  Every explicit reduction (conv accumulation, BPTT adjoint folds) is an
//  ASCENDING SCALAR FOLD (R1) — never simd::*. Matmuls would be Eigen `*`, but the
//  conv/recurrence math here is written as ordered scalar loops by construction.

#include <memory> // std::unique_ptr
#include <span>
#include <vector>

#include "atx/core/types.hpp" // f64, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/learn/nn/module.hpp" // Module, lin

namespace atx::engine::learn::nn {

// ===========================================================================
//  Conv1dCausal — dilated causal 1-D convolution along time.
//
//    y[t, co] = b[co] + Σ_{k=0}^{K-1} Σ_{ci} W[k, ci, co] * x[t - k*dilation, ci]
//
//  with x[.] = 0 for negative time (LEFT zero-pad). Output length == input length
//  T (the causal "same"-length convolution). The k=0 tap reads the current step;
//  larger k reach further into the PAST only (R2).
//
//  Flat param layout (ascending): W of K*C_in*C_out with index
//  ((k*C_in + ci)*C_out + co), then bias of C_out (when biased). See header doc.
// ===========================================================================
class Conv1dCausal final : public Module {
public:
  Conv1dCausal(atx::usize time_steps, atx::usize in_channels, atx::usize out_channels,
               atx::usize kernel, atx::usize dilation, bool bias = true);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return n_params_; }
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_; }

  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize time_steps() const noexcept { return T_; }
  [[nodiscard]] atx::usize in_channels() const noexcept { return c_in_; }
  [[nodiscard]] atx::usize out_channels() const noexcept { return c_out_; }
  [[nodiscard]] atx::usize kernel() const noexcept { return k_; }
  [[nodiscard]] atx::usize dilation() const noexcept { return dil_; }
  [[nodiscard]] bool has_bias() const noexcept { return bias_; }

private:
  // Flat index of weight tap (k, ci, co) into params_/grads_ (ascending order).
  [[nodiscard]] atx::usize w_index(atx::usize k, atx::usize ci, atx::usize co) const noexcept;

  atx::usize T_;
  atx::usize c_in_;
  atx::usize c_out_;
  atx::usize k_;
  atx::usize dil_;
  bool bias_;
  atx::usize w_count_;  // K*C_in*C_out
  atx::usize n_params_; // w_count_ + (bias ? C_out : 0)
  std::span<atx::f64> params_;
  std::span<atx::f64> grads_;
  lin::MatX x_cache_; // last forward input (B x T*C_in), for backward
};

// ===========================================================================
//  TcnResidualBlock — the canonical TCN block over `channels` (in == out):
//
//    h1 = Dropout(ReLU(LayerNorm(Conv1dCausal(x))))
//    h2 = Dropout(ReLU(LayerNorm(Conv1dCausal(h1))))
//    y  = h2 + skip(x)
//
//  where skip is identity when in/out channels match (the common case here, since
//  the block preserves channel count), else a 1x1 Conv1dCausal projecting the
//  channels. The two LayerNorms normalise over the CHANNEL axis at each time step
//  (per (sample, time) row of C values), matching the time-major encoding.
//
//  Composed from Conv1dCausal + the S5-2a LayerNorm / ReLU / Dropout. The block
//  owns one flat parameter buffer and binds its sub-modules in ascending order
//  (conv1, ln1, conv2, ln2, [proj]); backward threads dL/dx back through the two
//  conv/norm/relu/dropout stages and ADDS the skip-path gradient (R2/R3).
// ===========================================================================
class TcnResidualBlock final : public Module {
public:
  // `in_channels`/`out_channels` are the block's channel widths; when they differ
  // a 1x1 causal conv projects the skip. dropout is the DROP probability; seed
  // bases the two dropout mask streams (distinct per stage).
  TcnResidualBlock(atx::usize time_steps, atx::usize in_channels, atx::usize out_channels,
                   atx::usize kernel, atx::usize dilation, atx::f64 dropout, atx::u64 seed);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return n_params_; }
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_view_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_view_; }

  void train(bool on) noexcept override;
  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize in_channels() const noexcept { return c_in_; }
  [[nodiscard]] atx::usize out_channels() const noexcept { return c_out_; }
  [[nodiscard]] bool projects_skip() const noexcept { return proj_ != nullptr; }

private:
  atx::usize T_;
  atx::usize c_in_;
  atx::usize c_out_;
  atx::usize n_params_;
  // Sub-modules in ascending param order: conv1, ln1, conv2, ln2, [proj].
  std::unique_ptr<Module> conv1_;
  std::unique_ptr<Module> ln1_;
  std::unique_ptr<Module> relu1_;
  std::unique_ptr<Module> drop1_;
  std::unique_ptr<Module> conv2_;
  std::unique_ptr<Module> ln2_;
  std::unique_ptr<Module> relu2_;
  std::unique_ptr<Module> drop2_;
  std::unique_ptr<Module> proj_; // 1x1 causal conv on the skip, or null (identity)
  std::span<atx::f64> params_view_;
  std::span<atx::f64> grads_view_;
};

// ===========================================================================
//  GruCell — a (slightly reduced) GRU recurrent over the T steps of the window,
//  returning the FINAL hidden state. Standard GRU equations, h_{-1} = 0:
//
//    z_t = σ(x_t W_xz + h_{t-1} W_hz + b_z)        (update gate)
//    r_t = σ(x_t W_xr + h_{t-1} W_hr + b_r)        (reset gate)
//    n_t = tanh(x_t W_xn + (r_t ⊙ h_{t-1}) W_hn + b_n)   (candidate)
//    h_t = (1 - z_t) ⊙ n_t + z_t ⊙ h_{t-1}
//
//  forward: MatX(B, T*C_in) -> MatX(B, hidden) (h_{T-1}). backward is BPTT through
//  the window: iterate time DESCENDING, threading dL/dh_t and the gate adjoints
//  (this is the hardest gradient check; the adjoints are the load-bearing math).
//
//  Flat param layout (ascending), with C = C_in, H = hidden:
//    [ W_xz (C*H) ][ W_hz (H*H) ][ b_z (H) ]
//    [ W_xr (C*H) ][ W_hr (H*H) ][ b_r (H) ]
//    [ W_xn (C*H) ][ W_hn (H*H) ][ b_n (H) ]
//  each input/recurrent matrix is row-major (in-index slowest, out-index fastest):
//    W_x*[i*H + o], W_h*[i*H + o].
// ===========================================================================
class GruCell final : public Module {
public:
  GruCell(atx::usize time_steps, atx::usize in_channels, atx::usize hidden);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return n_params_; }
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_; }

  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize time_steps() const noexcept { return T_; }
  [[nodiscard]] atx::usize in_channels() const noexcept { return c_in_; }
  [[nodiscard]] atx::usize hidden() const noexcept { return h_; }

private:
  // BPTT step helpers (one window step of GruCell::backward, split at the natural
  // boundary). gru_step_adjoints: given the incoming dh, compute the three gate
  // pre-activation adjoints (daz/dar/dan) and seed dhprev with the direct + reset
  // paths. gru_step_accumulate: fold those adjoints into the param grads + dx and
  // finish dhprev via the z/r recurrent matrices. Pure extraction — no math change.
  void gru_step_adjoints(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &dh,
                         std::vector<atx::f64> &daz, std::vector<atx::f64> &dar,
                         std::vector<atx::f64> &dan, std::vector<atx::f64> &dhprev) const;
  void gru_step_accumulate(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &daz,
                           const std::vector<atx::f64> &dar, const std::vector<atx::f64> &dan,
                           std::vector<atx::f64> &dhprev, lin::MatX &dx);

  atx::usize T_;
  atx::usize c_in_;
  atx::usize h_;
  atx::usize n_params_;
  std::span<atx::f64> params_;
  std::span<atx::f64> grads_;
  // Caches for BPTT (filled per forward; sized in ctor where possible, R6).
  lin::MatX x_cache_;  // B x T*C_in
  lin::MatX h_seq_;    // (T+1) blocks of B x H stacked col-wise: h_seq_ col (t*H + j) = h_{t-1}[.,j]
  lin::MatX z_seq_;    // B x T*H : update gate per step
  lin::MatX r_seq_;    // B x T*H : reset gate per step
  lin::MatX n_seq_;    // B x T*H : candidate per step
};

// ===========================================================================
//  MguCell — the Minimal Gated Unit (Zhou et al. 2016): a single forget gate.
//
//    f_t = σ(x_t W_xf + h_{t-1} W_hf + b_f)                  (forget gate)
//    n_t = tanh(x_t W_xn + (f_t ⊙ h_{t-1}) W_hn + b_n)       (candidate)
//    h_t = (1 - f_t) ⊙ h_{t-1} + f_t ⊙ n_t
//
//  with h_{-1} = 0; forward returns h_{T-1}. backward is BPTT (descending time).
//
//  Flat param layout (ascending), C = C_in, H = hidden:
//    [ W_xf (C*H) ][ W_hf (H*H) ][ b_f (H) ]
//    [ W_xn (C*H) ][ W_hn (H*H) ][ b_n (H) ]
//  matrices row-major (W*[i*H + o]).
// ===========================================================================
class MguCell final : public Module {
public:
  MguCell(atx::usize time_steps, atx::usize in_channels, atx::usize hidden);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return n_params_; }
  void bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) override;
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return params_; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return grads_; }

  void state_to(std::vector<atx::f64> &out) const override;
  void state_from(std::span<const atx::f64> in) override;

  [[nodiscard]] atx::usize time_steps() const noexcept { return T_; }
  [[nodiscard]] atx::usize in_channels() const noexcept { return c_in_; }
  [[nodiscard]] atx::usize hidden() const noexcept { return h_; }

private:
  // BPTT step helpers (one window step of MguCell::backward, split at the natural
  // boundary, mirroring GruCell). mgu_step_adjoints: from dh, produce the forget
  // and candidate pre-activation adjoints (daf/dan) and seed dhprev. accumulate:
  // fold them into the param grads + dx and finish dhprev. Pure extraction.
  void mgu_step_adjoints(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &dh,
                         std::vector<atx::f64> &daf, std::vector<atx::f64> &dan,
                         std::vector<atx::f64> &dhprev) const;
  void mgu_step_accumulate(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &daf,
                           const std::vector<atx::f64> &dan, std::vector<atx::f64> &dhprev,
                           lin::MatX &dx);

  atx::usize T_;
  atx::usize c_in_;
  atx::usize h_;
  atx::usize n_params_;
  std::span<atx::f64> params_;
  std::span<atx::f64> grads_;
  lin::MatX x_cache_; // B x T*C_in
  lin::MatX h_seq_;   // col (t*H + j) = h_{t-1}[.,j]; (T+1) blocks
  lin::MatX f_seq_;   // B x T*H : forget gate per step
  lin::MatX n_seq_;   // B x T*H : candidate per step
};

// ===========================================================================
//  SeqLastStep — temporal pool taking the trailing/current step t = T-1.
//
//    forward  : MatX(B, T*C) -> MatX(B, C), y[.,c] = x[., (T-1)*C + c]
//    backward : scatter dL/dy back to the t=T-1 block, zeros for all earlier
//               steps (the discarded past has no gradient path).
//
//  Parameter-free. The alpha reads the newest step, so this is the pool that
//  hands the per-channel current state to the Linear head (R2: only t=T-1).
// ===========================================================================
class SeqLastStep final : public Module {
public:
  SeqLastStep(atx::usize time_steps, atx::usize channels);

  [[nodiscard]] lin::MatX forward(const lin::MatX &x) override;
  [[nodiscard]] lin::MatX backward(const lin::MatX &grad_out) override;

  [[nodiscard]] atx::usize param_count() const noexcept override { return 0; }
  void bind_params(std::span<atx::f64>, std::span<atx::f64>) override {}
  [[nodiscard]] std::span<atx::f64> params() noexcept override { return {}; }
  [[nodiscard]] std::span<atx::f64> grads() noexcept override { return {}; }
  void state_to(std::vector<atx::f64> &) const override {}
  void state_from(std::span<const atx::f64>) override {}

  [[nodiscard]] atx::usize time_steps() const noexcept { return T_; }
  [[nodiscard]] atx::usize channels() const noexcept { return c_; }

private:
  atx::usize T_;
  atx::usize c_;
};

} // namespace atx::engine::learn::nn
