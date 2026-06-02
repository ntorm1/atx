#pragma once

// atx::core::stats — online (streaming) statistics primitives.
//
// Value-type accumulators that consume samples one at a time in O(1) and
// expose running summaries without storing the full history (except the
// fixed window of RunningMinMax).  All are Rule-of-Zero value types.
//
//   RunningMean      — numerically stable incremental mean; supports remove()
//                      so it can drive a rolling window.
//   RunningVariance  — Welford's online algorithm (mean + M2) plus the
//                      reverse-Welford remove() for rolling windows.  Reports
//                      population variance, sample variance, and std-dev.
//   Ewma             — exponentially weighted moving average,
//                      value = alpha*x + (1-alpha)*value.
//   RunningMinMax<W> — extremes of the last W samples in amortized O(1) using
//                      two monotonic deques.  NB: the project RingBuffer is a
//                      strict FIFO (front-only pop); a monotonic deque needs
//                      double-ended popping, so a small zero-allocation deque is
//                      supplied here rather than modifying that module.
//
// Why Welford, not sum-of-squares:
//   The naive sum(x^2) - n*mean^2 formula catastrophically cancels for large
//   means with small variance.  Welford keeps M2 (sum of squared deviations
//   from the running mean) and is stable.  See Knuth TAOCP Vol.2 §4.2.2 and
//   B. P. Welford (1962), "Note on a method for calculating corrected sums of
//   squares and products", Technometrics 4(3).
//
// Thread-safety: NONE.  Synchronise externally if shared between threads.

#include <array> // std::array
#include <cmath> // std::sqrt

#include "atx/core/bit.hpp" // next_pow2
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core::stats {

// ============================================================
//  RunningMean
// ============================================================

/// Numerically stable incremental arithmetic mean.
///
/// Uses the update mean += (x - mean) / n rather than accumulating a raw sum,
/// which avoids the precision loss of a large running total.  remove() inverts
/// the same recurrence so the accumulator can back out the oldest sample of a
/// rolling window.
class RunningMean {
public:
    /// Fold a new sample into the running mean.
    void update(f64 x) noexcept {
        ++count_;
        // n is at least 1 here, so the division is well-defined.
        mean_ += (x - mean_) / static_cast<f64>(count_);
    }

    /// Remove a previously-added sample (rolling window support).
    ///
    /// @pre count() >= 1 and x is a value that was previously update()d.
    void remove(f64 x) noexcept {
        ATX_ASSERT(count_ >= 1U);
        --count_;
        if (count_ == 0U) {
            mean_ = 0.0;
            return;
        }
        // Invert mean_n = mean_{n-1} + (x - mean_{n-1}) / n  for mean_{n-1}.
        mean_ -= (x - mean_) / static_cast<f64>(count_);
    }

    /// Current mean.  Returns 0 when no samples have been added.
    [[nodiscard]] f64 mean() const noexcept { return mean_; }

    /// Number of samples currently folded in.
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    f64 mean_{0.0};
    usize count_{0U};
};

// ============================================================
//  RunningVariance
// ============================================================

/// Welford's online mean/variance, with reverse-Welford removal.
///
/// Maintains the running mean and M2 (sum of squared deviations from the
/// current mean).  Population variance is M2 / n; sample variance is
/// M2 / (n - 1).  Variance of fewer than two samples is defined to be 0.
class RunningVariance {
public:
    /// Fold a new sample in (forward Welford).
    void update(f64 x) noexcept {
        ++count_;
        const f64 n = static_cast<f64>(count_);
        const f64 delta = x - mean_;
        mean_ += delta / n;
        const f64 delta2 = x - mean_;
        m2_ += delta * delta2;
    }

    /// Remove a previously-added sample (reverse Welford).
    ///
    /// @pre count() >= 1 and x was previously update()d.
    void remove(f64 x) noexcept {
        ATX_ASSERT(count_ >= 1U);
        --count_;
        if (count_ == 0U) {
            mean_ = 0.0;
            m2_ = 0.0;
            return;
        }
        const f64 n = static_cast<f64>(count_);
        // Recover the pre-sample mean, then back out the M2 contribution.
        const f64 prev_mean = (mean_ * (n + 1.0) - x) / n;
        m2_ -= (x - mean_) * (x - prev_mean);
        mean_ = prev_mean;
        // Clamp tiny negative residue from floating-point round-off.
        if (m2_ < 0.0) { m2_ = 0.0; }
    }

    /// Current mean.  Returns 0 when no samples have been added.
    [[nodiscard]] f64 mean() const noexcept { return mean_; }

    /// Population variance (divide by n).  0 for fewer than two samples.
    [[nodiscard]] f64 variance() const noexcept {
        if (count_ < 2U) { return 0.0; }
        return m2_ / static_cast<f64>(count_);
    }

    /// Sample variance (divide by n - 1).  0 for fewer than two samples.
    [[nodiscard]] f64 sample_variance() const noexcept {
        if (count_ < 2U) { return 0.0; }
        return m2_ / static_cast<f64>(count_ - 1U);
    }

    /// Population standard deviation (sqrt of population variance).
    [[nodiscard]] f64 std_dev() const noexcept { return std::sqrt(variance()); }

    /// Number of samples currently folded in.
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    f64 mean_{0.0};
    f64 m2_{0.0}; // sum of squared deviations from the running mean
    usize count_{0U};
};

// ============================================================
//  Ewma
// ============================================================

/// Exponentially weighted moving average.
///
/// value = alpha*x + (1 - alpha)*value.  The first sample initialises value to
/// itself (there is no prior estimate to blend with).  A larger alpha weights
/// recent samples more heavily; alpha == 1 tracks the latest sample exactly.
class Ewma {
public:
    /// @param alpha  Smoothing factor in (0, 1].
    /// @pre 0 < alpha <= 1.
    explicit Ewma(f64 alpha) noexcept : alpha_{alpha} {
        ATX_ASSERT(alpha_ > 0.0 && alpha_ <= 1.0);
    }

    /// Blend a new sample into the running average.
    void update(f64 x) noexcept {
        if (count_ == 0U) {
            value_ = x; // First sample seeds the estimate.
        } else {
            value_ = alpha_ * x + (1.0 - alpha_) * value_;
        }
        ++count_;
    }

    /// Current smoothed value.  Returns 0 before any sample.
    [[nodiscard]] f64 value() const noexcept { return value_; }

    /// Number of samples folded in.
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    f64 alpha_;
    f64 value_{0.0};
    usize count_{0U};
};

// ============================================================
//  RunningMinMax<Window>
// ============================================================

namespace detail {

/// Fixed-capacity, double-ended monotonic deque of (value, seq) candidates.
///
/// This is the data structure a sliding-window-extremum needs: it must pop
/// from the FRONT (expire old candidates) and from the BACK (discard dominated
/// candidates).  The project RingBuffer is a strict FIFO — it only pops from
/// the front — so it cannot back this directly without modifying that module,
/// which is out of scope here.  We therefore implement a tiny self-contained
/// circular deque over an inline array (no dynamic allocation), with Capacity
/// rounded to the next power of two so indices mask instead of modulo.
///
/// @tparam Capacity  Maximum live candidates; the actual capacity is the next
///                   power of two >= Capacity.
template <usize Capacity>
class MonotonicDeque {
    static_assert(Capacity >= 1U, "MonotonicDeque: Capacity must be >= 1");
    static constexpr usize kCap = atx::core::next_pow2(Capacity);
    static constexpr usize kMask = kCap - 1U;

public:
    struct Tagged {
        f64 value;
        u64 seq; // global sample index; used to expire stale candidates
    };

    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] usize size() const noexcept { return size_; }

    /// Oldest (front) candidate — the current window extreme.
    /// @pre !empty().
    [[nodiscard]] const Tagged& front() const noexcept {
        ATX_ASSERT(size_ >= 1U);
        return buf_[head_];
    }

    /// Newest (back) candidate.
    /// @pre !empty().
    [[nodiscard]] const Tagged& back() const noexcept {
        ATX_ASSERT(size_ >= 1U);
        return buf_[(head_ + size_ - 1U) & kMask];
    }

    /// Append a candidate at the back.
    /// @pre size() < capacity().
    void push_back(const Tagged& t) noexcept {
        ATX_ASSERT(size_ < kCap);
        buf_[(head_ + size_) & kMask] = t;
        ++size_;
    }

    /// Drop the back candidate.
    /// @pre !empty().
    void pop_back() noexcept {
        ATX_ASSERT(size_ >= 1U);
        --size_;
    }

    /// Drop the front candidate.
    /// @pre !empty().
    void pop_front() noexcept {
        ATX_ASSERT(size_ >= 1U);
        head_ = (head_ + 1U) & kMask;
        --size_;
    }

private:
    // Tagged is trivial, so a plain inline array (no placement-new) suffices.
    std::array<Tagged, kCap> buf_{};
    usize head_{0U}; // physical index of the front candidate
    usize size_{0U};
};

} // namespace detail

/// Sliding-window minimum and maximum over the last `Window` samples.
///
/// Complexity: amortized O(1) per update().  Two monotonic deques (one
/// non-increasing for the max, one non-decreasing for the min) hold candidate
/// extremes tagged with the sequence index at which each entered.  Each sample
/// is pushed and popped from each deque at most once across its lifetime, so
/// the amortized cost is constant; the worst-case single update() is O(Window)
/// when a long run of dominated candidates is discarded at once.
///
/// The deques are inline, fixed-capacity, zero-allocation (see
/// detail::MonotonicDeque).  At most Window candidates are ever simultaneously
/// live in either deque.
template <usize Window>
class RunningMinMax {
    static_assert(Window >= 1U, "RunningMinMax: Window must be >= 1");

    using Deque = detail::MonotonicDeque<Window>;
    using Tagged = typename Deque::Tagged;

public:
    /// Slide the window forward by one sample.
    void update(f64 x) noexcept {
        const u64 seq = next_seq_++;

        // Expire the front candidate of each deque once it leaves the window
        // [seq - Window + 1, seq].  A candidate at index s is in-window iff
        // s + Window > seq; addition avoids unsigned underflow.
        while (!max_dq_.empty() && max_dq_.front().seq + Window <= seq) {
            max_dq_.pop_front();
        }
        while (!min_dq_.empty() && min_dq_.front().seq + Window <= seq) {
            min_dq_.pop_front();
        }

        // Max deque: keep values non-increasing front→back.  Any tail candidate
        // that x dominates (>=) can never again be the window maximum.
        while (!max_dq_.empty() && max_dq_.back().value <= x) {
            max_dq_.pop_back();
        }
        max_dq_.push_back(Tagged{x, seq});

        // Min deque: keep values non-decreasing front→back.
        while (!min_dq_.empty() && min_dq_.back().value >= x) {
            min_dq_.pop_back();
        }
        min_dq_.push_back(Tagged{x, seq});

        if (count_ < Window) { ++count_; }
    }

    /// Current window maximum.
    /// @pre count() >= 1.
    [[nodiscard]] f64 max() const noexcept {
        ATX_ASSERT(count_ >= 1U);
        return max_dq_.front().value;
    }

    /// Current window minimum.
    /// @pre count() >= 1.
    [[nodiscard]] f64 min() const noexcept {
        ATX_ASSERT(count_ >= 1U);
        return min_dq_.front().value;
    }

    /// Number of samples currently in the window (saturates at Window).
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    Deque max_dq_{}; // front = current window maximum (non-increasing values)
    Deque min_dq_{}; // front = current window minimum (non-decreasing values)
    u64 next_seq_{0U};
    usize count_{0U};
};

} // namespace atx::core::stats
