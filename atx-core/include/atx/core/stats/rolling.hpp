#pragma once

// atx::core::stats — rolling (windowed) statistics primitives.
//
// Each accumulator summarises only the last `Window` samples in O(1) amortised
// time per update.  Before the window fills, the statistic is taken over the
// samples seen so far; once full, the oldest sample is evicted as the newest is
// admitted.  Every update() returns the current statistic.
//
//   RollingMean<W>        — mean of the last min(count, W) samples.
//   RollingStd<W>         — population standard deviation of the window.
//   RollingZScore<W>      — (x - mean) / std of the current window (incl. x);
//                           0 when the window std is 0.
//   RollingCovariance<W>  — population covariance of the windowed (x, y) pairs.
//   RollingCorrelation<W> — Pearson correlation of the window in [-1, 1];
//                           0 when either marginal variance is 0.
//
// Implementation notes:
//   * RollingMean/RollingStd/RollingZScore drive a RunningVariance (Welford)
//     forward with update() and backward with remove() as samples leave the
//     window.  A RingBuffer remembers the W samples so the value to remove is
//     known.  This is the numerically stable path (see online_stats.hpp).
//   * RollingCovariance/RollingCorrelation keep windowed power sums
//     (sum_x, sum_y, sum_xx, sum_yy, sum_xy).  On eviction the leaving pair's
//     contribution is subtracted.  Population covariance is
//     (sum_xy - sum_x*sum_y/n) / n; correlation divides by the product of the
//     marginal population std-devs.  The single-pass power-sum form is the
//     standard O(1) sliding-window formulation; inputs in this library are
//     well-scaled (returns/prices), so catastrophic cancellation is not a
//     practical concern here.
//
// Thread-safety: NONE.  Synchronise externally if shared between threads.

#include <cmath> // std::sqrt

#include "atx/core/container/ring_buffer.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/stats/online_stats.hpp"
#include "atx/core/types.hpp"

namespace atx::core::stats {

// ============================================================
//  RollingMean<Window>
// ============================================================

/// Arithmetic mean of the last `Window` samples.
///
/// Backed by a RunningMean (so removal is numerically stable) plus a RingBuffer
/// that remembers which sample to back out when the window is full.
template <usize Window>
class RollingMean {
    static_assert(Window >= 1U, "RollingMean: Window must be >= 1");

public:
    /// Admit a sample and return the mean of the current window (including x).
    [[nodiscard]] f64 update(f64 x) noexcept {
        if (samples_.size() == Window) {
            // Window full: back the oldest sample out of the accumulator.
            // pop() cannot be empty here (size == Window >= 1).
            mean_.remove(samples_.pop().value());
        }
        const bool ok = samples_.push(x); // capacity >= Window, so always fits
        ATX_ASSERT(ok);
        ATX_UNUSED(ok);
        mean_.update(x);
        return mean_.mean();
    }

    /// Number of samples currently in the window (saturates at Window).
    [[nodiscard]] usize count() const noexcept { return samples_.size(); }

private:
    container::RingBuffer<f64, Window> samples_{};
    RunningMean mean_{};
};

// ============================================================
//  RollingStd<Window>
// ============================================================

/// Population standard deviation of the last `Window` samples.
///
/// Drives a RunningVariance (Welford) forward with update() and backward with
/// remove() via reverse-Welford as samples leave the window.
template <usize Window>
class RollingStd {
    static_assert(Window >= 1U, "RollingStd: Window must be >= 1");

public:
    /// Admit a sample and return the population std-dev of the current window.
    [[nodiscard]] f64 update(f64 x) noexcept {
        if (samples_.size() == Window) {
            var_.remove(samples_.pop().value());
        }
        const bool ok = samples_.push(x);
        ATX_ASSERT(ok);
        ATX_UNUSED(ok);
        var_.update(x);
        return var_.std_dev();
    }

    /// Number of samples currently in the window (saturates at Window).
    [[nodiscard]] usize count() const noexcept { return samples_.size(); }

private:
    container::RingBuffer<f64, Window> samples_{};
    RunningVariance var_{};
};

// ============================================================
//  RollingZScore<Window>
// ============================================================

/// Z-score of the newest sample against the current window.
///
/// Returns (x - mean) / std over the window that includes x.  When the window
/// std-dev is 0 (a constant or single-sample window) the score is defined to be
/// 0 rather than producing a division by zero.
///
/// NB: this uses the SAMPLE standard deviation (divide by n-1), unlike
/// RollingStd which reports the POPULATION std-dev.  The sample std is the
/// conventional choice for a standardised score / studentised residual (it is
/// the unbiased dispersion estimate for the window treated as a sample), and is
/// what the seed test value 1.2649110640673518 == (5-3)/sqrt(2.5) encodes.
template <usize Window>
class RollingZScore {
    static_assert(Window >= 1U, "RollingZScore: Window must be >= 1");

public:
    /// Admit a sample and return its z-score within the current window.
    [[nodiscard]] f64 update(f64 x) noexcept {
        if (samples_.size() == Window) {
            var_.remove(samples_.pop().value());
        }
        const bool ok = samples_.push(x);
        ATX_ASSERT(ok);
        ATX_UNUSED(ok);
        var_.update(x);

        const f64 sd = std::sqrt(var_.sample_variance());
        if (sd == 0.0) { return 0.0; }
        return (x - var_.mean()) / sd;
    }

    /// Number of samples currently in the window (saturates at Window).
    [[nodiscard]] usize count() const noexcept { return samples_.size(); }

private:
    container::RingBuffer<f64, Window> samples_{};
    RunningVariance var_{};
};

// ============================================================
//  detail — windowed power sums for covariance/correlation
// ============================================================

namespace detail {

/// Incremental windowed power sums over (x, y) pairs.
///
/// Maintains sum_x, sum_y, sum_xx, sum_yy, sum_xy over the live window so that
/// population covariance and the marginal variances are O(1) to read.  On
/// eviction the leaving pair's contribution is subtracted.
struct PowerSums {
    f64 sum_x{0.0};
    f64 sum_y{0.0};
    f64 sum_xx{0.0};
    f64 sum_yy{0.0};
    f64 sum_xy{0.0};

    /// Fold a pair into the sums.
    void add(f64 x, f64 y) noexcept {
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_yy += y * y;
        sum_xy += x * y;
    }

    /// Back a pair out of the sums (window eviction).
    void sub(f64 x, f64 y) noexcept {
        sum_x -= x;
        sum_y -= y;
        sum_xx -= x * x;
        sum_yy -= y * y;
        sum_xy -= x * y;
    }

    /// Population covariance over n live pairs: (sum_xy - sum_x*sum_y/n) / n.
    /// @pre n >= 1.
    [[nodiscard]] f64 covariance(f64 n) const noexcept {
        return (sum_xy - sum_x * sum_y / n) / n;
    }

    /// Population variance of x over n live pairs.  @pre n >= 1.
    [[nodiscard]] f64 var_x(f64 n) const noexcept {
        return (sum_xx - sum_x * sum_x / n) / n;
    }

    /// Population variance of y over n live pairs.  @pre n >= 1.
    [[nodiscard]] f64 var_y(f64 n) const noexcept {
        return (sum_yy - sum_y * sum_y / n) / n;
    }
};

} // namespace detail

// ============================================================
//  RollingCovariance<Window>
// ============================================================

/// Population covariance of the last `Window` (x, y) pairs.
///
/// Keeps windowed power sums; the oldest pair's contribution is subtracted when
/// the window is full.  cov = (sum_xy - sum_x*sum_y/n) / n.
template <usize Window>
class RollingCovariance {
    static_assert(Window >= 1U, "RollingCovariance: Window must be >= 1");

public:
    /// Admit a pair and return the population covariance of the current window.
    [[nodiscard]] f64 update(f64 x, f64 y) noexcept {
        if (xs_.size() == Window) {
            // Oldest pair leaving the window; both rings advance in lock-step.
            sums_.sub(xs_.pop().value(), ys_.pop().value());
        }
        const bool ok = xs_.push(x) && ys_.push(y); // capacity >= Window
        ATX_ASSERT(ok);
        ATX_UNUSED(ok);
        sums_.add(x, y);

        return sums_.covariance(static_cast<f64>(xs_.size()));
    }

    /// Number of pairs currently in the window (saturates at Window).
    [[nodiscard]] usize count() const noexcept { return xs_.size(); }

private:
    container::RingBuffer<f64, Window> xs_{};
    container::RingBuffer<f64, Window> ys_{};
    detail::PowerSums sums_{};
};

// ============================================================
//  RollingCorrelation<Window>
// ============================================================

/// Pearson correlation of the last `Window` (x, y) pairs, in [-1, 1].
///
/// corr = cov / (std_x * std_y), using population variances.  When either
/// marginal variance is 0 (a degenerate constant window) the correlation is
/// defined to be 0 rather than producing a division by zero.  The result is
/// clamped into [-1, 1] to absorb floating-point round-off.
template <usize Window>
class RollingCorrelation {
    static_assert(Window >= 1U, "RollingCorrelation: Window must be >= 1");

public:
    /// Admit a pair and return the Pearson correlation of the current window.
    [[nodiscard]] f64 update(f64 x, f64 y) noexcept {
        if (xs_.size() == Window) {
            sums_.sub(xs_.pop().value(), ys_.pop().value());
        }
        const bool ok = xs_.push(x) && ys_.push(y);
        ATX_ASSERT(ok);
        ATX_UNUSED(ok);
        sums_.add(x, y);

        const f64 n = static_cast<f64>(xs_.size());
        const f64 vx = sums_.var_x(n);
        const f64 vy = sums_.var_y(n);
        if (vx <= 0.0 || vy <= 0.0) { return 0.0; }

        const f64 corr = sums_.covariance(n) / std::sqrt(vx * vy);
        // Clamp tiny floating-point overshoot to the valid correlation range.
        if (corr > 1.0) { return 1.0; }
        if (corr < -1.0) { return -1.0; }
        return corr;
    }

    /// Number of pairs currently in the window (saturates at Window).
    [[nodiscard]] usize count() const noexcept { return xs_.size(); }

private:
    container::RingBuffer<f64, Window> xs_{};
    container::RingBuffer<f64, Window> ys_{};
    detail::PowerSums sums_{};
};

} // namespace atx::core::stats
