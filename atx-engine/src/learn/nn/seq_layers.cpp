#include "atx/engine/learn/nn/seq_layers.hpp"

#include "atx/engine/learn/nn/layers.hpp" // Conv skip uses Conv1dCausal; block uses LayerNorm/ReLU/Dropout
#include "atx/engine/learn/nn/module.hpp"

#include <algorithm> // std::fill (attention adjoint scratch reset)
#include <cmath>     // std::tanh, std::exp, std::sqrt
#include <limits>    // std::numeric_limits (attention causal -inf mask)
#include <memory>    // std::make_unique
#include <vector>    // std::vector (attention per-sample adjoint scratch)

#include <Eigen/Dense> // Eigen::Index

#include "atx/core/macro.hpp" // ATX_CHECK, ATX_ASSERT
#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::learn::nn {

// Casts kept local so every size_t<->Eigen::Index boundary is explicit (/Wconversion).
namespace {
[[nodiscard]] Eigen::Index idx(atx::usize n) noexcept { return static_cast<Eigen::Index>(n); }

[[nodiscard]] atx::f64 sigmoid(atx::f64 v) noexcept { return 1.0 / (1.0 + std::exp(-v)); }

// Reshape time-major (B, T*C) -> (B*T, C): row r, cols [t*C, t*C+C) becomes row
// (r*T + t). Lets the per-row LayerNorm normalise over the CHANNEL axis at each
// time step (a row of C features per (sample, time)). The inverse undoes it. Both
// are ordered scalar copies (R1) — no reductions, just a deterministic gather.
[[nodiscard]] lin::MatX to_time_rows(const lin::MatX &x, atx::usize T, atx::usize C) {
  const Eigen::Index B = x.rows();
  lin::MatX out(B * idx(T), idx(C));
  for (Eigen::Index r = 0; r < B; ++r) {
    for (atx::usize t = 0; t < T; ++t) {
      const Eigen::Index dst = r * idx(T) + idx(t);
      for (atx::usize c = 0; c < C; ++c) {
        out(dst, idx(c)) = x(r, idx(t * C + c));
      }
    }
  }
  return out;
}

[[nodiscard]] lin::MatX from_time_rows(const lin::MatX &x, Eigen::Index B, atx::usize T,
                                       atx::usize C) {
  lin::MatX out(B, idx(T * C));
  for (Eigen::Index r = 0; r < B; ++r) {
    for (atx::usize t = 0; t < T; ++t) {
      const Eigen::Index src = r * idx(T) + idx(t);
      for (atx::usize c = 0; c < C; ++c) {
        out(r, idx(t * C + c)) = x(src, idx(c));
      }
    }
  }
  return out;
}
} // namespace

// =====================================================================
//  Conv1dCausal
// =====================================================================

Conv1dCausal::Conv1dCausal(atx::usize time_steps, atx::usize in_channels, atx::usize out_channels,
                           atx::usize kernel, atx::usize dilation, bool bias)
    : T_{time_steps}, c_in_{in_channels}, c_out_{out_channels}, k_{kernel}, dil_{dilation},
      bias_{bias}, w_count_{kernel * in_channels * out_channels},
      n_params_{kernel * in_channels * out_channels + (bias ? out_channels : 0)} {
  ATX_CHECK(time_steps > 0);
  ATX_CHECK(in_channels > 0);
  ATX_CHECK(out_channels > 0);
  ATX_CHECK(kernel > 0);
  ATX_CHECK(dilation > 0);
}

atx::usize Conv1dCausal::w_index(atx::usize k, atx::usize ci, atx::usize co) const noexcept {
  return (k * c_in_ + ci) * c_out_ + co; // tap k slowest, input ci, output co fastest
}

void Conv1dCausal::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_ = params;
  grads_ = grads;
}

lin::MatX Conv1dCausal::forward(const lin::MatX &x) {
  ATX_ASSERT(static_cast<atx::usize>(x.cols()) == T_ * c_in_);
  x_cache_ = x; // B x T*C_in, needed for dL/dW
  const Eigen::Index B = x.rows();
  lin::MatX y(B, idx(T_ * c_out_));
  for (Eigen::Index r = 0; r < B; ++r) {
    for (atx::usize t = 0; t < T_; ++t) {
      for (atx::usize co = 0; co < c_out_; ++co) {
        // Ascending scalar fold (R1): bias, then taps current->past, channels.
        atx::f64 acc = bias_ ? params_[w_count_ + co] : 0.0;
        for (atx::usize k = 0; k < k_; ++k) {
          const atx::usize back = k * dil_;
          if (back > t) {
            break; // larger k only reaches further into the past (all padded)
          }
          const atx::usize ts = t - back;
          for (atx::usize ci = 0; ci < c_in_; ++ci) {
            acc += params_[w_index(k, ci, co)] * x(r, idx(ts * c_in_ + ci));
          }
        }
        y(r, idx(t * c_out_ + co)) = acc;
      }
    }
  }
  return y;
}

lin::MatX Conv1dCausal::backward(const lin::MatX &grad_out) {
  ATX_ASSERT(static_cast<atx::usize>(grad_out.cols()) == T_ * c_out_);
  const Eigen::Index B = grad_out.rows();
  lin::MatX dx = lin::MatX::Zero(B, idx(T_ * c_in_));
  for (Eigen::Index r = 0; r < B; ++r) {
    for (atx::usize t = 0; t < T_; ++t) {
      for (atx::usize co = 0; co < c_out_; ++co) {
        const atx::f64 go = grad_out(r, idx(t * c_out_ + co));
        if (bias_) {
          grads_[w_count_ + co] += go; // dL/db[co] += Σ_t gout[t,co]
        }
        for (atx::usize k = 0; k < k_; ++k) {
          const atx::usize back = k * dil_;
          if (back > t) {
            break;
          }
          const atx::usize ts = t - back;
          for (atx::usize ci = 0; ci < c_in_; ++ci) {
            const atx::usize wi = w_index(k, ci, co);
            // dL/dW += gout * x[t-k*dil] ; dL/dx[t-k*dil] += gout * W.
            grads_[wi] += go * x_cache_(r, idx(ts * c_in_ + ci));
            dx(r, idx(ts * c_in_ + ci)) += go * params_[wi];
          }
        }
      }
    }
  }
  return dx;
}

void Conv1dCausal::state_to(std::vector<atx::f64> &out) const {
  out.insert(out.end(), params_.begin(), params_.end()); // ascending == bind order
}
void Conv1dCausal::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  for (atx::usize i = 0; i < n_params_; ++i) {
    params_[i] = in[i];
  }
}

// =====================================================================
//  TcnResidualBlock
// =====================================================================

TcnResidualBlock::TcnResidualBlock(atx::usize time_steps, atx::usize in_channels,
                                   atx::usize out_channels, atx::usize kernel, atx::usize dilation,
                                   atx::f64 dropout, atx::u64 seed)
    : T_{time_steps}, c_in_{in_channels}, c_out_{out_channels} {
  ATX_CHECK(time_steps > 0);
  ATX_CHECK(in_channels > 0);
  ATX_CHECK(out_channels > 0);
  // Stage 1 maps c_in -> c_out; stage 2 maps c_out -> c_out. LayerNorm/ReLU/Dropout
  // act on the per-time channel vector (features = channels), applied at every step
  // via the time-major row encoding (LayerNorm normalises each C-block per row).
  conv1_ = std::make_unique<Conv1dCausal>(time_steps, in_channels, out_channels, kernel, dilation,
                                          /*bias=*/true);
  ln1_ = std::make_unique<LayerNorm>(out_channels);
  relu1_ = std::make_unique<ReLU>();
  drop1_ = std::make_unique<Dropout>(dropout, seed);
  conv2_ = std::make_unique<Conv1dCausal>(time_steps, out_channels, out_channels, kernel, dilation,
                                          /*bias=*/true);
  ln2_ = std::make_unique<LayerNorm>(out_channels);
  relu2_ = std::make_unique<ReLU>();
  drop2_ = std::make_unique<Dropout>(dropout, seed ^ 0xD1B54A32D192ED03ULL); // distinct mask stream
  if (in_channels != out_channels) {
    // 1x1 causal conv projects the skip to the output channel width.
    proj_ = std::make_unique<Conv1dCausal>(time_steps, in_channels, out_channels, /*kernel=*/1,
                                           /*dilation=*/1, /*bias=*/false);
  }
  n_params_ = conv1_->param_count() + ln1_->param_count() + conv2_->param_count() +
              ln2_->param_count() + (proj_ ? proj_->param_count() : 0);
}

void TcnResidualBlock::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_view_ = params;
  grads_view_ = grads;
  // Ascending sub-module order: conv1, ln1, conv2, ln2, [proj].
  atx::usize off = 0;
  auto bind = [&](Module &m) {
    const atx::usize n = m.param_count();
    m.bind_params(params.subspan(off, n), grads.subspan(off, n));
    off += n;
  };
  bind(*conv1_);
  bind(*ln1_);
  bind(*conv2_);
  bind(*ln2_);
  if (proj_) {
    bind(*proj_);
  }
}

lin::MatX TcnResidualBlock::forward(const lin::MatX &x) {
  const Eigen::Index B = x.rows();
  // Stage 1: conv -> LN(per time-step over channels) -> ReLU -> dropout. The LN
  // sees a (B*T, C_out) reshape so it normalises each step's channel vector.
  lin::MatX h = conv1_->forward(x);
  h = from_time_rows(ln1_->forward(to_time_rows(h, T_, c_out_)), B, T_, c_out_);
  h = relu1_->forward(h);
  h = drop1_->forward(h);
  // Stage 2: conv -> LN -> ReLU -> dropout.
  h = conv2_->forward(h);
  h = from_time_rows(ln2_->forward(to_time_rows(h, T_, c_out_)), B, T_, c_out_);
  h = relu2_->forward(h);
  h = drop2_->forward(h);
  // Residual skip (identity, or 1x1 conv projection when channels differ).
  const lin::MatX skip = proj_ ? proj_->forward(x) : x;
  return h + skip;
}

lin::MatX TcnResidualBlock::backward(const lin::MatX &grad_out) {
  // y = body(x) + skip(x); dL/d(body out) = dL/d(skip out) = grad_out.
  // Thread grad back through the two stages, then add the skip-path input grad.
  // The reshape is a permutation, so its adjoint is just the inverse gather: a
  // (B, T*C) grad re-rows to (B*T, C) for ln.backward, then un-rows afterwards.
  const Eigen::Index B = grad_out.rows();
  lin::MatX g = drop2_->backward(grad_out);
  g = relu2_->backward(g);
  g = from_time_rows(ln2_->backward(to_time_rows(g, T_, c_out_)), B, T_, c_out_);
  g = conv2_->backward(g);
  g = drop1_->backward(g);
  g = relu1_->backward(g);
  g = from_time_rows(ln1_->backward(to_time_rows(g, T_, c_out_)), B, T_, c_out_);
  lin::MatX dx = conv1_->backward(g); // dL/dx through the body
  if (proj_) {
    dx += proj_->backward(grad_out); // skip-path grad through the 1x1 conv
  } else {
    dx += grad_out; // identity skip: local grad = 1
  }
  return dx;
}

void TcnResidualBlock::train(bool on) noexcept {
  training_ = on;
  conv1_->train(on);
  ln1_->train(on);
  relu1_->train(on);
  drop1_->train(on);
  conv2_->train(on);
  ln2_->train(on);
  relu2_->train(on);
  drop2_->train(on);
  if (proj_) {
    proj_->train(on);
  }
}

void TcnResidualBlock::state_to(std::vector<atx::f64> &out) const {
  conv1_->state_to(out); // ascending sub-module order == ascending param order
  ln1_->state_to(out);
  conv2_->state_to(out);
  ln2_->state_to(out);
  if (proj_) {
    proj_->state_to(out);
  }
}

void TcnResidualBlock::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  atx::usize off = 0;
  auto restore = [&](Module &m) {
    const atx::usize n = m.param_count();
    m.state_from(in.subspan(off, n));
    off += n;
  };
  restore(*conv1_);
  restore(*ln1_);
  restore(*conv2_);
  restore(*ln2_);
  if (proj_) {
    restore(*proj_);
  }
}

// =====================================================================
//  GruCell — BPTT over the window. Param block layout (ascending):
//    z: [W_xz (C*H)][W_hz (H*H)][b_z (H)]
//    r: [W_xr (C*H)][W_hr (H*H)][b_r (H)]
//    n: [W_xn (C*H)][W_hn (H*H)][b_n (H)]
//  W_x*[i*H + o], W_h*[i*H + o] (in-index slowest, out-index fastest).
// =====================================================================

namespace {
// Per-gate slice offsets into the GRU flat param block.
struct GruLayout {
  atx::usize c;
  atx::usize h;
  atx::usize wx;  // C*H
  atx::usize wh;  // H*H
  atx::usize gate; // wx + wh + h
  [[nodiscard]] atx::usize wxz() const noexcept { return 0; }
  [[nodiscard]] atx::usize whz() const noexcept { return wx; }
  [[nodiscard]] atx::usize bz() const noexcept { return wx + wh; }
  [[nodiscard]] atx::usize wxr() const noexcept { return gate; }
  [[nodiscard]] atx::usize whr() const noexcept { return gate + wx; }
  [[nodiscard]] atx::usize br() const noexcept { return gate + wx + wh; }
  [[nodiscard]] atx::usize wxn() const noexcept { return 2 * gate; }
  [[nodiscard]] atx::usize whn() const noexcept { return 2 * gate + wx; }
  [[nodiscard]] atx::usize bn() const noexcept { return 2 * gate + wx + wh; }
};
[[nodiscard]] GruLayout gru_layout(atx::usize c, atx::usize h) noexcept {
  return GruLayout{c, h, c * h, h * h, c * h + h * h + h};
}
} // namespace

GruCell::GruCell(atx::usize time_steps, atx::usize in_channels, atx::usize hidden)
    : T_{time_steps}, c_in_{in_channels}, h_{hidden},
      n_params_{3 * (in_channels * hidden + hidden * hidden + hidden)} {
  ATX_CHECK(time_steps > 0);
  ATX_CHECK(in_channels > 0);
  ATX_CHECK(hidden > 0);
}

void GruCell::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_ = params;
  grads_ = grads;
}

lin::MatX GruCell::forward(const lin::MatX &x) {
  ATX_ASSERT(static_cast<atx::usize>(x.cols()) == T_ * c_in_);
  x_cache_ = x;
  const Eigen::Index B = x.rows();
  const GruLayout L = gru_layout(c_in_, h_);
  // h_seq_ col (t*H + j) holds h_{t-1}[.,j]; block t=0 is h_{-1}=0, block T is h_{T-1}.
  h_seq_ = lin::MatX::Zero(B, idx((T_ + 1) * h_));
  z_seq_.resize(B, idx(T_ * h_));
  r_seq_.resize(B, idx(T_ * h_));
  n_seq_.resize(B, idx(T_ * h_));

  for (Eigen::Index b = 0; b < B; ++b) {
    for (atx::usize t = 0; t < T_; ++t) {
      for (atx::usize o = 0; o < h_; ++o) {
        // z/r gate pre-activations (ascending scalar fold: bias, x terms, h terms).
        atx::f64 az = params_[L.bz() + o];
        atx::f64 ar = params_[L.br() + o];
        for (atx::usize ci = 0; ci < c_in_; ++ci) {
          const atx::f64 xv = x(b, idx(t * c_in_ + ci));
          az += params_[L.wxz() + ci * h_ + o] * xv;
          ar += params_[L.wxr() + ci * h_ + o] * xv;
        }
        for (atx::usize j = 0; j < h_; ++j) {
          const atx::f64 hp = h_seq_(b, idx(t * h_ + j));
          az += params_[L.whz() + j * h_ + o] * hp;
          ar += params_[L.whr() + j * h_ + o] * hp;
        }
        const atx::f64 z = sigmoid(az);
        const atx::f64 r = sigmoid(ar);
        z_seq_(b, idx(t * h_ + o)) = z;
        r_seq_(b, idx(t * h_ + o)) = r;
      }
      // Candidate uses the freshly-computed reset gate: n = tanh(x W_xn + (r⊙h)W_hn + b_n).
      for (atx::usize o = 0; o < h_; ++o) {
        atx::f64 an = params_[L.bn() + o];
        for (atx::usize ci = 0; ci < c_in_; ++ci) {
          an += params_[L.wxn() + ci * h_ + o] * x(b, idx(t * c_in_ + ci));
        }
        for (atx::usize j = 0; j < h_; ++j) {
          const atx::f64 rh = r_seq_(b, idx(t * h_ + j)) * h_seq_(b, idx(t * h_ + j));
          an += params_[L.whn() + j * h_ + o] * rh;
        }
        const atx::f64 n = std::tanh(an);
        n_seq_(b, idx(t * h_ + o)) = n;
        const atx::f64 z = z_seq_(b, idx(t * h_ + o));
        const atx::f64 hprev = h_seq_(b, idx(t * h_ + o));
        h_seq_(b, idx((t + 1) * h_ + o)) = (1.0 - z) * n + z * hprev;
      }
    }
  }
  // Final hidden state h_{T-1} lives in block T.
  lin::MatX out(B, idx(h_));
  for (Eigen::Index b = 0; b < B; ++b) {
    for (atx::usize o = 0; o < h_; ++o) {
      out(b, idx(o)) = h_seq_(b, idx(T_ * h_ + o));
    }
  }
  return out;
}

// Gate-adjoint block of one BPTT step: from the incoming dh, produce the gate
// pre-activation adjoints daz/dar/dan and seed dhprev (direct + reset paths).
void GruCell::gru_step_adjoints(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &dh,
                                std::vector<atx::f64> &daz, std::vector<atx::f64> &dar,
                                std::vector<atx::f64> &dan,
                                std::vector<atx::f64> &dhprev) const {
  const GruLayout L = gru_layout(c_in_, h_);
  for (atx::usize o = 0; o < h_; ++o) {
    const atx::f64 z = z_seq_(b, idx(t * h_ + o));
    const atx::f64 n = n_seq_(b, idx(t * h_ + o));
    const atx::f64 hprev = h_seq_(b, idx(t * h_ + o));
    const atx::f64 g = dh[o];
    // h_t = (1-z) n + z hprev.
    const atx::f64 dz = g * (hprev - n);
    const atx::f64 dn = g * (1.0 - z);
    daz[o] = dz * z * (1.0 - z); // through sigmoid
    dan[o] = dn * (1.0 - n * n); // through tanh
    dhprev[o] += g * z;          // direct hprev path
  }
  // Candidate's recurrent term g = r⊙hprev fed through W_hn:
  //   d(r_j hprev_j) = Σ_o dan[o] * W_hn[j,o]  (=: dg_j)
  //   dr_j += dg_j * hprev_j ; dhprev_j += dg_j * r_j.
  for (atx::usize j = 0; j < h_; ++j) {
    const atx::f64 r = r_seq_(b, idx(t * h_ + j));
    const atx::f64 hprev = h_seq_(b, idx(t * h_ + j));
    atx::f64 dg = 0.0;
    for (atx::usize o = 0; o < h_; ++o) {
      dg += dan[o] * params_[L.whn() + j * h_ + o];
    }
    const atx::f64 dr = dg * hprev;
    dar[j] = dr * r * (1.0 - r); // through sigmoid
    dhprev[j] += dg * r;
  }
}

// Accumulation block of one BPTT step: fold daz/dar/dan into the param grads and
// dx, and finish dhprev via the z/r recurrent matrices (n's path already folded).
void GruCell::gru_step_accumulate(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &daz,
                                  const std::vector<atx::f64> &dar, const std::vector<atx::f64> &dan,
                                  std::vector<atx::f64> &dhprev, lin::MatX &dx) {
  const GruLayout L = gru_layout(c_in_, h_);
  // Accumulate parameter grads (ascending: bias, then W_x, then W_h).
  for (atx::usize o = 0; o < h_; ++o) {
    grads_[L.bz() + o] += daz[o];
    grads_[L.br() + o] += dar[o];
    grads_[L.bn() + o] += dan[o];
  }
  for (atx::usize ci = 0; ci < c_in_; ++ci) {
    const atx::f64 xv = x_cache_(b, idx(t * c_in_ + ci));
    for (atx::usize o = 0; o < h_; ++o) {
      grads_[L.wxz() + ci * h_ + o] += xv * daz[o];
      grads_[L.wxr() + ci * h_ + o] += xv * dar[o];
      grads_[L.wxn() + ci * h_ + o] += xv * dan[o];
      // dx_t += daz W_xzᵀ + dar W_xrᵀ + dan W_xnᵀ.
      dx(b, idx(t * c_in_ + ci)) += daz[o] * params_[L.wxz() + ci * h_ + o] +
                                    dar[o] * params_[L.wxr() + ci * h_ + o] +
                                    dan[o] * params_[L.wxn() + ci * h_ + o];
    }
  }
  for (atx::usize j = 0; j < h_; ++j) {
    const atx::f64 hp = h_seq_(b, idx(t * h_ + j));
    const atx::f64 rh = r_seq_(b, idx(t * h_ + j)) * hp;
    for (atx::usize o = 0; o < h_; ++o) {
      grads_[L.whz() + j * h_ + o] += hp * daz[o];
      grads_[L.whr() + j * h_ + o] += hp * dar[o];
      grads_[L.whn() + j * h_ + o] += rh * dan[o];
      // dhprev via the z/r gate recurrent matrices (n's path already in dhprev).
      dhprev[j] += daz[o] * params_[L.whz() + j * h_ + o] + dar[o] * params_[L.whr() + j * h_ + o];
    }
  }
}

lin::MatX GruCell::backward(const lin::MatX &grad_out) {
  const Eigen::Index B = grad_out.rows();
  lin::MatX dx = lin::MatX::Zero(B, idx(T_ * c_in_));

  for (Eigen::Index b = 0; b < B; ++b) {
    // dh for the current step; seeded at h_{T-1} by the output grad.
    std::vector<atx::f64> dh(h_, 0.0);
    for (atx::usize o = 0; o < h_; ++o) {
      dh[o] = grad_out(b, idx(o));
    }
    for (atx::usize tt = T_; tt-- > 0;) { // time descending (BPTT)
      const atx::usize t = tt;
      std::vector<atx::f64> daz(h_, 0.0);
      std::vector<atx::f64> dar(h_, 0.0);
      std::vector<atx::f64> dan(h_, 0.0);
      std::vector<atx::f64> dhprev(h_, 0.0);
      gru_step_adjoints(b, t, dh, daz, dar, dan, dhprev);
      gru_step_accumulate(b, t, daz, dar, dan, dhprev, dx);
      dh = dhprev; // carry to step t-1
    }
  }
  return dx;
}

void GruCell::state_to(std::vector<atx::f64> &out) const {
  out.insert(out.end(), params_.begin(), params_.end());
}
void GruCell::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  for (atx::usize i = 0; i < n_params_; ++i) {
    params_[i] = in[i];
  }
}

// =====================================================================
//  MguCell — Minimal Gated Unit BPTT. Param block layout (ascending):
//    f: [W_xf (C*H)][W_hf (H*H)][b_f (H)]
//    n: [W_xn (C*H)][W_hn (H*H)][b_n (H)]
// =====================================================================

namespace {
struct MguLayout {
  atx::usize c;
  atx::usize h;
  atx::usize wx;
  atx::usize wh;
  atx::usize gate;
  [[nodiscard]] atx::usize wxf() const noexcept { return 0; }
  [[nodiscard]] atx::usize whf() const noexcept { return wx; }
  [[nodiscard]] atx::usize bf() const noexcept { return wx + wh; }
  [[nodiscard]] atx::usize wxn() const noexcept { return gate; }
  [[nodiscard]] atx::usize whn() const noexcept { return gate + wx; }
  [[nodiscard]] atx::usize bn() const noexcept { return gate + wx + wh; }
};
[[nodiscard]] MguLayout mgu_layout(atx::usize c, atx::usize h) noexcept {
  return MguLayout{c, h, c * h, h * h, c * h + h * h + h};
}
} // namespace

MguCell::MguCell(atx::usize time_steps, atx::usize in_channels, atx::usize hidden)
    : T_{time_steps}, c_in_{in_channels}, h_{hidden},
      n_params_{2 * (in_channels * hidden + hidden * hidden + hidden)} {
  ATX_CHECK(time_steps > 0);
  ATX_CHECK(in_channels > 0);
  ATX_CHECK(hidden > 0);
}

void MguCell::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_ = params;
  grads_ = grads;
}

lin::MatX MguCell::forward(const lin::MatX &x) {
  ATX_ASSERT(static_cast<atx::usize>(x.cols()) == T_ * c_in_);
  x_cache_ = x;
  const Eigen::Index B = x.rows();
  const MguLayout L = mgu_layout(c_in_, h_);
  h_seq_ = lin::MatX::Zero(B, idx((T_ + 1) * h_));
  f_seq_.resize(B, idx(T_ * h_));
  n_seq_.resize(B, idx(T_ * h_));

  for (Eigen::Index b = 0; b < B; ++b) {
    for (atx::usize t = 0; t < T_; ++t) {
      for (atx::usize o = 0; o < h_; ++o) {
        atx::f64 af = params_[L.bf() + o];
        for (atx::usize ci = 0; ci < c_in_; ++ci) {
          af += params_[L.wxf() + ci * h_ + o] * x(b, idx(t * c_in_ + ci));
        }
        for (atx::usize j = 0; j < h_; ++j) {
          af += params_[L.whf() + j * h_ + o] * h_seq_(b, idx(t * h_ + j));
        }
        f_seq_(b, idx(t * h_ + o)) = sigmoid(af);
      }
      for (atx::usize o = 0; o < h_; ++o) {
        atx::f64 an = params_[L.bn() + o];
        for (atx::usize ci = 0; ci < c_in_; ++ci) {
          an += params_[L.wxn() + ci * h_ + o] * x(b, idx(t * c_in_ + ci));
        }
        for (atx::usize j = 0; j < h_; ++j) {
          const atx::f64 fh = f_seq_(b, idx(t * h_ + j)) * h_seq_(b, idx(t * h_ + j));
          an += params_[L.whn() + j * h_ + o] * fh;
        }
        const atx::f64 n = std::tanh(an);
        n_seq_(b, idx(t * h_ + o)) = n;
        const atx::f64 f = f_seq_(b, idx(t * h_ + o));
        const atx::f64 hprev = h_seq_(b, idx(t * h_ + o));
        h_seq_(b, idx((t + 1) * h_ + o)) = (1.0 - f) * hprev + f * n;
      }
    }
  }
  lin::MatX out(B, idx(h_));
  for (Eigen::Index b = 0; b < B; ++b) {
    for (atx::usize o = 0; o < h_; ++o) {
      out(b, idx(o)) = h_seq_(b, idx(T_ * h_ + o));
    }
  }
  return out;
}

// Gate-adjoint block of one MGU BPTT step: from dh, produce the forget/candidate
// pre-activation adjoints daf/dan (daf already through the sigmoid) and seed
// dhprev (direct + forget-recurrent paths).
void MguCell::mgu_step_adjoints(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &dh,
                                std::vector<atx::f64> &daf, std::vector<atx::f64> &dan,
                                std::vector<atx::f64> &dhprev) const {
  const MguLayout L = mgu_layout(c_in_, h_);
  std::vector<atx::f64> df(h_, 0.0);
  for (atx::usize o = 0; o < h_; ++o) {
    const atx::f64 f = f_seq_(b, idx(t * h_ + o));
    const atx::f64 n = n_seq_(b, idx(t * h_ + o));
    const atx::f64 hprev = h_seq_(b, idx(t * h_ + o));
    const atx::f64 g = dh[o];
    // h_t = (1-f) hprev + f n.
    df[o] = g * (n - hprev);
    const atx::f64 dn = g * f;
    dan[o] = dn * (1.0 - n * n);
    dhprev[o] += g * (1.0 - f);
  }
  // Candidate recurrent term gg = f⊙hprev through W_hn.
  for (atx::usize j = 0; j < h_; ++j) {
    const atx::f64 f = f_seq_(b, idx(t * h_ + j));
    const atx::f64 hprev = h_seq_(b, idx(t * h_ + j));
    atx::f64 dg = 0.0;
    for (atx::usize o = 0; o < h_; ++o) {
      dg += dan[o] * params_[L.whn() + j * h_ + o];
    }
    df[j] += dg * hprev;
    dhprev[j] += dg * f;
  }
  for (atx::usize o = 0; o < h_; ++o) {
    const atx::f64 f = f_seq_(b, idx(t * h_ + o));
    daf[o] = df[o] * f * (1.0 - f); // through sigmoid
  }
}

// Accumulation block of one MGU BPTT step: fold daf/dan into the param grads and
// dx, and finish dhprev via the forget recurrent matrix.
void MguCell::mgu_step_accumulate(Eigen::Index b, atx::usize t, const std::vector<atx::f64> &daf,
                                  const std::vector<atx::f64> &dan, std::vector<atx::f64> &dhprev,
                                  lin::MatX &dx) {
  const MguLayout L = mgu_layout(c_in_, h_);
  for (atx::usize o = 0; o < h_; ++o) {
    grads_[L.bf() + o] += daf[o];
    grads_[L.bn() + o] += dan[o];
  }
  for (atx::usize ci = 0; ci < c_in_; ++ci) {
    const atx::f64 xv = x_cache_(b, idx(t * c_in_ + ci));
    for (atx::usize o = 0; o < h_; ++o) {
      grads_[L.wxf() + ci * h_ + o] += xv * daf[o];
      grads_[L.wxn() + ci * h_ + o] += xv * dan[o];
      dx(b, idx(t * c_in_ + ci)) += daf[o] * params_[L.wxf() + ci * h_ + o] +
                                    dan[o] * params_[L.wxn() + ci * h_ + o];
    }
  }
  for (atx::usize j = 0; j < h_; ++j) {
    const atx::f64 hp = h_seq_(b, idx(t * h_ + j));
    const atx::f64 fh = f_seq_(b, idx(t * h_ + j)) * hp;
    for (atx::usize o = 0; o < h_; ++o) {
      grads_[L.whf() + j * h_ + o] += hp * daf[o];
      grads_[L.whn() + j * h_ + o] += fh * dan[o];
      dhprev[j] += daf[o] * params_[L.whf() + j * h_ + o];
    }
  }
}

lin::MatX MguCell::backward(const lin::MatX &grad_out) {
  const Eigen::Index B = grad_out.rows();
  lin::MatX dx = lin::MatX::Zero(B, idx(T_ * c_in_));

  for (Eigen::Index b = 0; b < B; ++b) {
    std::vector<atx::f64> dh(h_, 0.0);
    for (atx::usize o = 0; o < h_; ++o) {
      dh[o] = grad_out(b, idx(o));
    }
    for (atx::usize tt = T_; tt-- > 0;) {
      const atx::usize t = tt;
      std::vector<atx::f64> daf(h_, 0.0);
      std::vector<atx::f64> dan(h_, 0.0);
      std::vector<atx::f64> dhprev(h_, 0.0);
      mgu_step_adjoints(b, t, dh, daf, dan, dhprev);
      mgu_step_accumulate(b, t, daf, dan, dhprev, dx);
      dh = dhprev;
    }
  }
  return dx;
}

void MguCell::state_to(std::vector<atx::f64> &out) const {
  out.insert(out.end(), params_.begin(), params_.end());
}
void MguCell::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  for (atx::usize i = 0; i < n_params_; ++i) {
    params_[i] = in[i];
  }
}

// =====================================================================
//  SeqLastStep — temporal pool (step T-1)
// =====================================================================

SeqLastStep::SeqLastStep(atx::usize time_steps, atx::usize channels)
    : T_{time_steps}, c_{channels} {
  ATX_CHECK(time_steps > 0);
  ATX_CHECK(channels > 0);
}

lin::MatX SeqLastStep::forward(const lin::MatX &x) {
  ATX_ASSERT(static_cast<atx::usize>(x.cols()) == T_ * c_);
  const Eigen::Index B = x.rows();
  lin::MatX y(B, idx(c_));
  const atx::usize base = (T_ - 1) * c_;
  for (Eigen::Index r = 0; r < B; ++r) {
    for (atx::usize c = 0; c < c_; ++c) {
      y(r, idx(c)) = x(r, idx(base + c));
    }
  }
  return y;
}

lin::MatX SeqLastStep::backward(const lin::MatX &grad_out) {
  ATX_ASSERT(static_cast<atx::usize>(grad_out.cols()) == c_);
  const Eigen::Index B = grad_out.rows();
  lin::MatX dx = lin::MatX::Zero(B, idx(T_ * c_));
  const atx::usize base = (T_ - 1) * c_;
  for (Eigen::Index r = 0; r < B; ++r) {
    for (atx::usize c = 0; c < c_; ++c) {
      dx(r, idx(base + c)) = grad_out(r, idx(c)); // scatter to t=T-1, zeros elsewhere
    }
  }
  return dx;
}

// =====================================================================
//  Attention1Head — single-head causal scaled-dot-product attention. Flat param
//  layout (ascending): [Wq (C*D)][Wk (C*D)][Wv (C*D)][bq (D)][bk (D)][bv (D)],
//  each projection matrix row-major W[ci*D + o]. See the header for the math.
// =====================================================================

Attention1Head::Attention1Head(atx::usize time_steps, atx::usize in_channels, atx::usize d_model,
                               bool bias)
    : T_{time_steps}, c_in_{in_channels}, d_{d_model}, bias_{bias} {
  ATX_CHECK(time_steps > 0);
  ATX_CHECK(in_channels > 0);
  ATX_CHECK(d_model > 0);
  const atx::usize cd = in_channels * d_model; // one projection block (C*D)
  wq_ = 0;
  wk_ = cd;
  wv_ = 2 * cd;
  bq_ = 3 * cd;
  bk_ = 3 * cd + d_model;
  bv_ = 3 * cd + 2 * d_model;
  n_params_ = 3 * cd + (bias ? 3 * d_model : 0);
}

atx::usize Attention1Head::w_index(atx::usize ci, atx::usize o) const noexcept {
  return ci * d_ + o; // input ci slowest, output o fastest (row-major, sibling style)
}

void Attention1Head::bind_params(std::span<atx::f64> params, std::span<atx::f64> grads) {
  ATX_ASSERT(params.size() == n_params_);
  ATX_ASSERT(grads.size() == n_params_);
  params_ = params;
  grads_ = grads;
}

// Project sample b's window into Q/K/V caches (each T×D), ascending C_in fold.
void Attention1Head::project_qkv(Eigen::Index b, const lin::MatX &x) {
  for (atx::usize t = 0; t < T_; ++t) {
    for (atx::usize o = 0; o < d_; ++o) {
      atx::f64 q = bias_ ? params_[bq_ + o] : 0.0;
      atx::f64 k = bias_ ? params_[bk_ + o] : 0.0;
      atx::f64 v = bias_ ? params_[bv_ + o] : 0.0;
      for (atx::usize ci = 0; ci < c_in_; ++ci) {
        const atx::f64 xv = x(b, idx(t * c_in_ + ci));
        const atx::usize wi = w_index(ci, o);
        q += params_[wq_ + wi] * xv;
        k += params_[wk_ + wi] * xv;
        v += params_[wv_ + wi] * xv;
      }
      q_cache_(b, idx(t * d_ + o)) = q;
      k_cache_(b, idx(t * d_ + o)) = k;
      v_cache_(b, idx(t * d_ + o)) = v;
    }
  }
}

// Causal masked-softmax attention for sample b: fill a_cache_ (T×T) and write the
// output O (T×D) into `out`. Row i attends only to j <= i; the softmax fold is
// ascending with the standard max-subtract for stability (R1, no simd).
void Attention1Head::attend(Eigen::Index b, lin::MatX &out) {
  const atx::f64 inv_scale = 1.0 / std::sqrt(static_cast<atx::f64>(d_));
  for (atx::usize i = 0; i < T_; ++i) {
    // 1) raw scores S[i,j] = (Q_i · K_j) / sqrt(D) for j <= i; find the row max.
    atx::f64 smax = -std::numeric_limits<atx::f64>::infinity();
    for (atx::usize j = 0; j <= i; ++j) {
      atx::f64 s = 0.0;
      for (atx::usize o = 0; o < d_; ++o) {
        s += q_cache_(b, idx(i * d_ + o)) * k_cache_(b, idx(j * d_ + o));
      }
      s *= inv_scale;
      a_cache_(b, idx(i * T_ + j)) = s; // stash raw score; normalised below
      if (s > smax) {
        smax = s;
      }
    }
    // 2) exp(S - max) and the ascending normaliser sum over the unmasked range.
    atx::f64 denom = 0.0;
    for (atx::usize j = 0; j <= i; ++j) {
      const atx::f64 e = std::exp(a_cache_(b, idx(i * T_ + j)) - smax);
      a_cache_(b, idx(i * T_ + j)) = e;
      denom += e;
    }
    // 3) normalise to weights A[i,j]; masked (j > i) entries are exactly zero.
    for (atx::usize j = 0; j <= i; ++j) {
      a_cache_(b, idx(i * T_ + j)) /= denom;
    }
    for (atx::usize j = i + 1; j < T_; ++j) {
      a_cache_(b, idx(i * T_ + j)) = 0.0; // future positions carry no weight (R2)
    }
    // 4) output O[i,o] = Σ_{j<=i} A[i,j] · V[j,o] (ascending j fold).
    for (atx::usize o = 0; o < d_; ++o) {
      atx::f64 acc = 0.0;
      for (atx::usize j = 0; j <= i; ++j) {
        acc += a_cache_(b, idx(i * T_ + j)) * v_cache_(b, idx(j * d_ + o));
      }
      out(b, idx(i * d_ + o)) = acc;
    }
  }
}

lin::MatX Attention1Head::forward(const lin::MatX &x) {
  ATX_ASSERT(static_cast<atx::usize>(x.cols()) == T_ * c_in_);
  x_cache_ = x;
  const Eigen::Index B = x.rows();
  q_cache_.resize(B, idx(T_ * d_));
  k_cache_.resize(B, idx(T_ * d_));
  v_cache_.resize(B, idx(T_ * d_));
  a_cache_.resize(B, idx(T_ * T_));
  lin::MatX out(B, idx(T_ * d_));
  for (Eigen::Index b = 0; b < B; ++b) {
    project_qkv(b, x);
    attend(b, out);
  }
  return out;
}

// Backward through O=A·V and the row-wise softmax (with the causal mask) for one
// sample b. Produces dQ/dK/dV (each length T*D, ascending t*D+o). The softmax
// Jacobian dS_i = A_i ⊙ (dA_i − (dA_i·A_i)) and the masked-entry zeroing are the
// load-bearing adjoint math (R3). All folds ascending (R1).
void Attention1Head::attend_backward(Eigen::Index b, const lin::MatX &grad_out,
                                     std::vector<atx::f64> &dQ, std::vector<atx::f64> &dK,
                                     std::vector<atx::f64> &dV) const {
  const atx::f64 inv_scale = 1.0 / std::sqrt(static_cast<atx::f64>(d_));
  std::fill(dQ.begin(), dQ.end(), 0.0);
  std::fill(dK.begin(), dK.end(), 0.0);
  std::fill(dV.begin(), dV.end(), 0.0);
  // Per output row i (only j <= i is unmasked / contributes).
  for (atx::usize i = 0; i < T_; ++i) {
    // dA[i,j] = Σ_o dO[i,o]·V[j,o]; dV[j,o] += A[i,j]·dO[i,o] (ascending folds).
    std::vector<atx::f64> dA(i + 1, 0.0);
    for (atx::usize j = 0; j <= i; ++j) {
      const atx::f64 a = a_cache_(b, idx(i * T_ + j));
      atx::f64 da = 0.0;
      for (atx::usize o = 0; o < d_; ++o) {
        const atx::f64 go = grad_out(b, idx(i * d_ + o));
        da += go * v_cache_(b, idx(j * d_ + o));
        dV[j * d_ + o] += a * go;
      }
      dA[j] = da;
    }
    // Softmax Jacobian: dot = Σ_{k<=i} dA[k]·A[i,k]; dS[i,j] = A[i,j]·(dA[j] − dot).
    atx::f64 dot = 0.0;
    for (atx::usize j = 0; j <= i; ++j) {
      dot += dA[j] * a_cache_(b, idx(i * T_ + j));
    }
    // dS[i,j] feeds the scores S=QKᵀ/√d: dQ[i,o] += dS·K[j,o]/√d (over j),
    // dK[j,o] += dS·Q[i,o]/√d. Masked j > i contribute nothing (never iterated).
    for (atx::usize j = 0; j <= i; ++j) {
      const atx::f64 a = a_cache_(b, idx(i * T_ + j));
      const atx::f64 ds = a * (dA[j] - dot) * inv_scale;
      for (atx::usize o = 0; o < d_; ++o) {
        dQ[i * d_ + o] += ds * k_cache_(b, idx(j * d_ + o));
        dK[j * d_ + o] += ds * q_cache_(b, idx(i * d_ + o));
      }
    }
  }
}

// Fold sample b's dQ/dK/dV into the param grads (+ biases) and dL/dx. Ascending.
void Attention1Head::project_backward(Eigen::Index b, const std::vector<atx::f64> &dQ,
                                      const std::vector<atx::f64> &dK,
                                      const std::vector<atx::f64> &dV, lin::MatX &dx) {
  for (atx::usize t = 0; t < T_; ++t) {
    if (bias_) {
      for (atx::usize o = 0; o < d_; ++o) {
        grads_[bq_ + o] += dQ[t * d_ + o];
        grads_[bk_ + o] += dK[t * d_ + o];
        grads_[bv_ + o] += dV[t * d_ + o];
      }
    }
    for (atx::usize ci = 0; ci < c_in_; ++ci) {
      const atx::f64 xv = x_cache_(b, idx(t * c_in_ + ci));
      atx::f64 dxv = 0.0;
      for (atx::usize o = 0; o < d_; ++o) {
        const atx::usize wi = w_index(ci, o);
        const atx::f64 dq = dQ[t * d_ + o];
        const atx::f64 dk = dK[t * d_ + o];
        const atx::f64 dv = dV[t * d_ + o];
        grads_[wq_ + wi] += xv * dq;
        grads_[wk_ + wi] += xv * dk;
        grads_[wv_ + wi] += xv * dv;
        dxv += dq * params_[wq_ + wi] + dk * params_[wk_ + wi] + dv * params_[wv_ + wi];
      }
      dx(b, idx(t * c_in_ + ci)) += dxv;
    }
  }
}

lin::MatX Attention1Head::backward(const lin::MatX &grad_out) {
  ATX_ASSERT(static_cast<atx::usize>(grad_out.cols()) == T_ * d_);
  const Eigen::Index B = grad_out.rows();
  lin::MatX dx = lin::MatX::Zero(B, idx(T_ * c_in_));
  std::vector<atx::f64> dQ(T_ * d_, 0.0);
  std::vector<atx::f64> dK(T_ * d_, 0.0);
  std::vector<atx::f64> dV(T_ * d_, 0.0);
  for (Eigen::Index b = 0; b < B; ++b) {
    attend_backward(b, grad_out, dQ, dK, dV);
    project_backward(b, dQ, dK, dV, dx);
  }
  return dx;
}

void Attention1Head::state_to(std::vector<atx::f64> &out) const {
  out.insert(out.end(), params_.begin(), params_.end()); // ascending == bind order
}
void Attention1Head::state_from(std::span<const atx::f64> in) {
  ATX_ASSERT(in.size() == n_params_);
  for (atx::usize i = 0; i < n_params_; ++i) {
    params_[i] = in[i];
  }
}

} // namespace atx::engine::learn::nn
