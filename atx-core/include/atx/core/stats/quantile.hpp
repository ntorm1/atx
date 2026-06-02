#pragma once

// atx::core::stats — streaming quantile estimation (P² algorithm).
//
// P2Quantile estimates an arbitrary p-quantile (e.g. the median, the 90th or
// 99th percentile) from a stream of samples in CONSTANT memory and O(1) per
// update.  It is the Jain & Chlamtac "P²" (Piecewise-Parabolic) algorithm: it
// keeps just five markers — order statistics that track the running min, the
// p/2-, p-, (1+p)/2- and max-quantiles — and on each sample nudges the markers
// toward their desired positions using a parabolic (and, as a fallback, linear)
// prediction.  It never stores the full history, so it is suited to unbounded
// streams and latency histograms.
//
// Accuracy: this is an ESTIMATOR, not an exact percentile.  For well-behaved
// distributions it converges to within a percent or so of the true quantile;
// the error is largest in the transient before ~100 samples and for heavily
// multimodal data.  Callers needing exactness must buffer and sort.
//
// Before five samples have arrived the markers are not yet initialised; value()
// falls back to a linear-interpolated order statistic over the buffered
// samples so the estimate is still sane (e.g. the middle of three samples for
// the median).  The fifth sample sorts the buffer to seed the marker heights.
//
// Thread-safety: NONE.  Synchronise externally if shared between threads.
//
// Reference:
//   R. Jain and I. Chlamtac (1985), "The P² Algorithm for Dynamic Calculation
//   of Quantiles and Histograms Without Storing Observations", Communications
//   of the ACM 28(10), 1076-1085.

#include <algorithm> // std::sort, std::lower_bound
#include <array>     // std::array
#include <cmath>     // std::floor

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core::stats {

/// Single-quantile P² streaming estimator (5 markers, constant memory).
///
/// Construct with the target probability p in (0, 1), feed samples via
/// update(), and read the current estimate with value().  Rule of Zero: holds
/// only fixed arrays and scalars, so the compiler-generated special members are
/// correct and the type is trivially copyable.
class P2Quantile {
public:
    /// @param p  Target probability in the open interval (0, 1).
    /// @pre 0 < p < 1.
    explicit P2Quantile(f64 p) noexcept : p_{p} {
        ATX_ASSERT(p_ > 0.0 && p_ < 1.0);
        // Desired-position increments per sample (Jain-Chlamtac dn vector).
        dn_ = {0.0, p_ / 2.0, p_, (1.0 + p_) / 2.0, 1.0};
        // Initial desired positions n'[i] = 1 + 4*dn[i].
        for (usize i = 0U; i < kMarkers; ++i) {
            np_[i] = 1.0 + 4.0 * dn_[i];
        }
    }

    /// Consume one sample, updating the marker estimates.
    void update(f64 x) noexcept {
        if (count_ < kMarkers) {
            init_buf_[count_] = x;
            ++count_;
            if (count_ == kMarkers) {
                seed_markers();
            }
            return;
        }
        ++count_;
        const usize k = locate_cell(x);
        advance_positions(k);
        adjust_markers();
    }

    /// Current estimate of the p-quantile.
    ///
    /// Before five samples have arrived this interpolates over the buffered
    /// samples; afterwards it returns the p-marker height q[2].
    [[nodiscard]] f64 value() const noexcept {
        if (count_ < kMarkers) {
            return interpolate_buffer();
        }
        return q_[2];
    }

    /// Number of samples consumed so far.
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    static constexpr usize kMarkers = 5U;

    /// Seed the five marker heights from the first five samples (sorted) and
    /// set the marker positions to 1..5.  Called exactly once, on sample five.
    void seed_markers() noexcept {
        std::sort(init_buf_.begin(), init_buf_.end());
        for (usize i = 0U; i < kMarkers; ++i) {
            q_[i] = init_buf_[i];
            n_[i] = static_cast<f64>(i + 1U); // 1-based marker positions
        }
    }

    /// Find the cell k such that the new sample sits in [q[k], q[k+1]),
    /// clamping the extreme markers outward to cover x.  Returns k in [0, 3].
    [[nodiscard]] usize locate_cell(f64 x) noexcept {
        if (x < q_[0]) {
            q_[0] = x;
            return 0U;
        }
        if (x >= q_[kMarkers - 1U]) {
            q_[kMarkers - 1U] = x;
            return kMarkers - 2U; // == 3
        }
        usize k = 0U;
        // Bounded scan over the 4 interior cells; q is sorted so this is cheap.
        for (usize i = 0U; i + 1U < kMarkers; ++i) {
            if (q_[i] <= x && x < q_[i + 1U]) {
                k = i;
                break;
            }
        }
        return k;
    }

    /// Increment the actual positions of all markers above the hit cell, and
    /// advance every marker's desired position by its increment.
    void advance_positions(usize k) noexcept {
        for (usize i = k + 1U; i < kMarkers; ++i) {
            n_[i] += 1.0;
        }
        for (usize i = 0U; i < kMarkers; ++i) {
            np_[i] += dn_[i];
        }
    }

    /// Nudge each interior marker (i = 1..3) toward its desired position using
    /// the parabolic prediction, falling back to linear when parabola breaks
    /// monotonicity.  Moves a marker at most one position per call.
    void adjust_markers() noexcept {
        for (usize i = 1U; i + 1U < kMarkers; ++i) {
            const f64 d = np_[i] - n_[i];
            if ((d >= 1.0 && (n_[i + 1U] - n_[i]) > 1.0) ||
                (d <= -1.0 && (n_[i - 1U] - n_[i]) < -1.0)) {
                const f64 s = (d >= 0.0) ? 1.0 : -1.0;
                f64 qi = parabolic(i, s);
                if (qi <= q_[i - 1U] || qi >= q_[i + 1U]) {
                    qi = linear(i, s);
                }
                q_[i] = qi;
                n_[i] += s;
            }
        }
    }

    /// Piecewise-parabolic (P²) prediction of q[i] when it shifts by s (+/-1).
    [[nodiscard]] f64 parabolic(usize i, f64 s) const noexcept {
        const f64 nm = n_[i - 1U];
        const f64 ni = n_[i];
        const f64 np = n_[i + 1U];
        const f64 qm = q_[i - 1U];
        const f64 qi = q_[i];
        const f64 qp = q_[i + 1U];
        return qi + (s / (np - nm)) *
                        ((ni - nm + s) * (qp - qi) / (np - ni) +
                         (np - ni - s) * (qi - qm) / (ni - nm));
    }

    /// Linear fallback for q[i] when the parabolic value leaves (q[i-1], q[i+1]).
    [[nodiscard]] f64 linear(usize i, f64 s) const noexcept {
        const usize j = (s >= 0.0) ? (i + 1U) : (i - 1U);
        return q_[i] + s * (q_[j] - q_[i]) / (n_[j] - n_[i]);
    }

    /// Order-statistic estimate over the buffered (< 5) samples, by linear
    /// interpolation at rank p*(m-1).  m == count_ here, and m >= 1.
    [[nodiscard]] f64 interpolate_buffer() const noexcept {
        ATX_ASSERT(count_ >= 1U);
        std::array<f64, kMarkers> tmp = init_buf_;
        std::sort(tmp.begin(), tmp.begin() + static_cast<isize>(count_));
        if (count_ == 1U) {
            return tmp[0];
        }
        const f64 rank = p_ * static_cast<f64>(count_ - 1U);
        const f64 floor_rank = std::floor(rank);
        const auto lo = static_cast<usize>(floor_rank);
        const f64 frac = rank - floor_rank;
        if (lo + 1U >= count_) {
            return tmp[count_ - 1U];
        }
        return tmp[lo] + frac * (tmp[lo + 1U] - tmp[lo]);
    }

    f64 p_;                                   // target probability in (0, 1)
    std::array<f64, kMarkers> q_{};           // marker heights (order statistics)
    std::array<f64, kMarkers> n_{};           // actual marker positions
    std::array<f64, kMarkers> np_{};          // desired marker positions
    std::array<f64, kMarkers> dn_{};          // desired-position increments
    std::array<f64, kMarkers> init_buf_{};    // first 5 samples, pre-seed
    usize count_{0U};                         // samples consumed
};

} // namespace atx::core::stats
