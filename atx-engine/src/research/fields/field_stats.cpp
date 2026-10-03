#include "atx/engine/research/fields/field_stats.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <system_error>
#include <utility>

namespace atx::engine::research::fields {

namespace {
constexpr usize kUnroll = 8;          // numpy's eight accumulators
constexpr usize kPairwiseBlock = 128; // numpy PW_BLOCKSIZE

// ---- ndarray.min() / max(): simd_reduce_c_{min,max}_f64 on AVX2 (4 f64 lanes) -----------------
constexpr usize kLanes = 4;
constexpr usize kBlock = 8 * kLanes; // eight vectors per unrolled step
using Lanes = std::array<f64, kLanes>;

// x86 MINPD / MAXPD on finite operands: the second operand when they compare equal.
[[nodiscard]] f64 x86_min(f64 a, f64 b) noexcept { return a < b ? a : b; }
[[nodiscard]] f64 x86_max(f64 a, f64 b) noexcept { return a > b ? a : b; }

template <class Op> [[nodiscard]] Lanes fold(const Lanes &a, const Lanes &b, Op op) noexcept {
  Lanes out{};
  for (usize j = 0; j < kLanes; ++j) {
    out[j] = op(a[j], b[j]);
  }
  return out;
}

[[nodiscard]] Lanes load(std::span<const f64> values, usize at) noexcept {
  Lanes out{};
  for (usize j = 0; j < kLanes; ++j) {
    out[j] = values[at + j];
  }
  return out;
}

template <class Op> [[nodiscard]] f64 numpy_reduce(std::span<const f64> values, Op op) noexcept {
  assert(!values.empty());
  // numpy seeds the output with the first element and reduces the rest into it.
  const std::span<const f64> rest = values.subspan(1);
  const usize len = rest.size();
  if (len == 0) {
    return values.front();
  }
  Lanes acc{};
  acc.fill(values.front());
  usize i = 0;
  for (; len - i >= kBlock; i += kBlock) {
    const Lanes r01 = fold(load(rest, i), load(rest, i + kLanes), op);
    const Lanes r23 = fold(load(rest, i + 2 * kLanes), load(rest, i + 3 * kLanes), op);
    const Lanes r45 = fold(load(rest, i + 4 * kLanes), load(rest, i + 5 * kLanes), op);
    const Lanes r67 = fold(load(rest, i + 6 * kLanes), load(rest, i + 7 * kLanes), op);
    acc = fold(acc, fold(fold(r01, r23, op), fold(r45, r67, op), op), op);
  }
  for (; len - i >= kLanes; i += kLanes) {
    acc = fold(acc, load(rest, i), op);
  }
  // npyv_reduce: (lane 0, lane 2) and (lane 1, lane 3), then the pair.
  f64 r = op(op(acc[0], acc[2]), op(acc[1], acc[3]));
  for (; i < len; ++i) {
    r = op(r, rest[i]);
  }
  return r;
}

// ---- np.partition: numpy 1.26 selection.cpp introselect_ (float64, no argsort) ----------------
constexpr usize kMaxPivots = 50; // NPY_MAX_PIVOT_STACK

struct PivotStack {
  std::array<isize, kMaxPivots> index{};
  usize size{};
};

void store_pivot(isize pivot, isize kth, PivotStack *pivots) noexcept {
  if (pivots == nullptr) {
    return;
  }
  if (pivot == kth && pivots->size == kMaxPivots) {
    pivots->index[kMaxPivots - 1] = pivot;
  } else if (pivot >= kth && pivots->size < kMaxPivots) {
    pivots->index[pivots->size] = pivot;
    ++pivots->size;
  }
}

// npy_get_msb: floor(log2(n)) for n >= 1.
[[nodiscard]] int msb(isize n) noexcept {
  auto unum = static_cast<usize>(n);
  int depth = 0;
  while ((unum >>= 1U) != 0U) {
    ++depth;
  }
  return depth;
}

void median3_swap(f64 *v, isize low, isize mid, isize high) noexcept {
  if (v[high] < v[mid]) {
    std::swap(v[high], v[mid]);
  }
  if (v[high] < v[low]) {
    std::swap(v[high], v[low]);
  }
  if (v[low] < v[mid]) { // the median moves to low
    std::swap(v[low], v[mid]);
  }
  std::swap(v[mid], v[low + 1]); // the 3-lowest moves to low + 1
}

// The index (0..4) of the median of v[0..4], sorting pairs on the way as numpy does.
[[nodiscard]] isize median5(f64 *v) noexcept {
  if (v[1] < v[0]) {
    std::swap(v[1], v[0]);
  }
  if (v[4] < v[3]) {
    std::swap(v[4], v[3]);
  }
  if (v[3] < v[0]) {
    std::swap(v[3], v[0]);
  }
  if (v[4] < v[1]) {
    std::swap(v[4], v[1]);
  }
  if (v[2] < v[1]) {
    std::swap(v[2], v[1]);
  }
  if (v[3] < v[2]) {
    return v[3] < v[1] ? 1 : 3;
  }
  return 2;
}

// O(n * kth) selection for a kth close to low.
void dumb_select(f64 *v, isize num, isize kth) noexcept {
  for (isize i = 0; i <= kth; ++i) {
    isize min_index = i;
    f64 min_value = v[i];
    for (isize k = i + 1; k < num; ++k) {
      if (v[k] < min_value) {
        min_index = k;
        min_value = v[k];
      }
    }
    std::swap(v[i], v[min_index]);
  }
}

void introselect(f64 *v, isize num, isize kth, PivotStack *pivots) noexcept;

// The median of the medians of blocks of five; recursion (through introselect, without a pivot
// stack) shrinks the range five-fold per level, so its depth is at most log5(n) < 28.
[[nodiscard]] isize median_of_median5(f64 *v, isize num) noexcept {
  const isize count = num / 5;
  for (isize i = 0, sub_left = 0; i < count; ++i, sub_left += 5) {
    const isize m = median5(v + sub_left);
    std::swap(v[sub_left + m], v[i]);
  }
  if (count > 2) {
    introselect(v, count, count / 2, nullptr);
  }
  return count / 2;
}

void introselect(f64 *v, isize num, isize kth, PivotStack *pivots) noexcept {
  isize low = 0;
  isize high = num - 1;
  while (pivots != nullptr && pivots->size > 0) {
    const isize top = pivots->index[pivots->size - 1];
    if (top > kth) {
      high = top - 1; // a larger pivot bounds the range from above
      break;
    }
    if (top == kth) {
      return; // found by a previous kth
    }
    low = top + 1;
    --pivots->size;
  }
  if (kth - low < 3) {
    dumb_select(v + low, high - low + 1, kth - low);
    store_pivot(kth, kth, pivots);
    return;
  }
  if (kth == num - 1) { // inexact types: a max scan (numpy's NaN probe partition(d, (x, -1)))
    isize max_index = low;
    f64 max_value = v[low];
    for (isize k = low + 1; k < num; ++k) {
      if (!(v[k] < max_value)) {
        max_index = k;
        max_value = v[k];
      }
    }
    std::swap(v[kth], v[max_index]);
    return;
  }
  int depth_limit = msb(num) * 2;
  while (low + 1 < high) {
    isize ll = low + 1;
    isize hh = high;
    if (depth_limit > 0 || hh - ll < 5) {
      median3_swap(v, low, low + (high - low) / 2, high);
    } else {
      const isize mid = ll + median_of_median5(v + ll, hh - ll);
      std::swap(v[mid], v[low]);
      --ll; // the larger partition of a median-of-medians pivot
      ++hh;
    }
    --depth_limit;
    const f64 pivot = v[low];
    for (;;) { // unguarded: the median-of-3 swaps bound both scans
      do {
        ++ll;
      } while (v[ll] < pivot);
      do {
        --hh;
      } while (pivot < v[hh]);
      if (hh < ll) {
        break;
      }
      std::swap(v[ll], v[hh]);
    }
    std::swap(v[low], v[hh]);
    if (hh != kth) {
      store_pivot(hh, kth, pivots);
    }
    if (hh >= kth) {
      high = hh - 1;
    }
    if (hh <= kth) {
      low = ll;
    }
  }
  if (high == low + 1 && v[high] < v[low]) {
    std::swap(v[high], v[low]);
  }
  store_pivot(kth, kth, pivots);
}

// numpy's _lerp(a, b, gamma).
[[nodiscard]] f64 lerp(f64 a, f64 b, f64 gamma) noexcept {
  const f64 diff = b - a;
  if (gamma >= 0.5) {
    const f64 complement = 1.0 - gamma;
    const f64 step = diff * complement;
    return b - step;
  }
  const f64 step = diff * gamma;
  return a + step;
}

struct Neighbours {
  isize previous{}; // numpy's previous index: -1 when v >= n - 1
  isize next{};
  f64 virtual_index{};
};

[[nodiscard]] Neighbours neighbours(usize n, f64 q) noexcept {
  const f64 last = static_cast<f64>(n - 1);
  Neighbours out;
  out.virtual_index = last * q;
  if (out.virtual_index >= last) {
    out.previous = -1;
    out.next = -1;
  } else {
    out.previous = static_cast<isize>(std::floor(out.virtual_index));
    out.next = out.previous + 1;
  }
  return out;
}

[[nodiscard]] f64 interpolate(std::span<const f64> v, const Neighbours &at) noexcept {
  const auto n = static_cast<isize>(v.size());
  const isize lo = at.previous < 0 ? at.previous + n : at.previous;
  const isize hi = at.next < 0 ? at.next + n : at.next;
  const f64 gamma = at.virtual_index - static_cast<f64>(at.previous);
  return lerp(v[static_cast<usize>(lo)], v[static_cast<usize>(hi)], gamma);
}

} // namespace

f64 numpy_pairwise_sum(std::span<const f64> values) noexcept {
  const usize n = values.size();
  if (n < kUnroll) {
    f64 res = 0.0;
    for (const f64 x : values) {
      res += x;
    }
    return res;
  }
  if (n <= kPairwiseBlock) {
    std::array<f64, kUnroll> r{};
    for (usize j = 0; j < kUnroll; ++j) {
      r[j] = values[j];
    }
    usize i = kUnroll;
    for (; i < n - (n % kUnroll); i += kUnroll) {
      for (usize j = 0; j < kUnroll; ++j) {
        r[j] += values[i + j];
      }
    }
    const f64 left = (r[0] + r[1]) + (r[2] + r[3]);
    const f64 right = (r[4] + r[5]) + (r[6] + r[7]);
    f64 res = left + right;
    for (; i < n; ++i) {
      res += values[i];
    }
    return res;
  }
  // Recursion depth is at most log2(n) (each level at least halves n), so it is bounded by 64.
  usize n2 = n / 2;
  n2 -= n2 % kUnroll;
  const f64 head = numpy_pairwise_sum(values.first(n2));
  const f64 tail = numpy_pairwise_sum(values.subspan(n2));
  return head + tail;
}

f64 numpy_reduce_min(std::span<const f64> values) noexcept { return numpy_reduce(values, x86_min); }

f64 numpy_reduce_max(std::span<const f64> values) noexcept { return numpy_reduce(values, x86_max); }

void numpy_quantiles(std::span<f64> values, std::span<const f64> probabilities,
                     std::span<f64> out) noexcept {
  assert(!values.empty() && out.size() == probabilities.size() &&
         probabilities.size() <= kMaxQuantiles);
  const usize n = values.size();
  const auto count = static_cast<isize>(n);
  std::array<Neighbours, kMaxQuantiles> at{};
  // np.unique([0, -1, previous..., next...]) on the raw indexes, then made non-negative and sorted
  // (partition_prep_kth_array): -1 and n - 1 may both survive unique and meet after the shift.
  std::array<isize, 2 + 2 * kMaxQuantiles> raw{};
  usize kth_count = 0;
  raw[kth_count++] = 0;
  raw[kth_count++] = -1;
  for (usize k = 0; k < probabilities.size(); ++k) {
    assert(probabilities[k] >= 0.0 && probabilities[k] <= 1.0);
    at[k] = neighbours(n, probabilities[k]);
    raw[kth_count++] = at[k].previous;
    raw[kth_count++] = at[k].next;
  }
  const auto raw_end = raw.begin() + static_cast<isize>(kth_count);
  std::sort(raw.begin(), raw_end);
  const auto unique_end = std::unique(raw.begin(), raw_end);
  for (auto it = raw.begin(); it != unique_end; ++it) {
    *it = *it < 0 ? *it + count : *it;
  }
  std::sort(raw.begin(), unique_end);
  PivotStack pivots;
  for (auto it = raw.begin(); it != unique_end; ++it) {
    introselect(values.data(), count, *it, &pivots);
  }
  for (usize k = 0; k < probabilities.size(); ++k) {
    out[k] = interpolate(values, at[k]);
  }
}

f64 numpy_linear_quantile(std::span<const f64> sorted, f64 q) noexcept {
  return interpolate(sorted, neighbours(sorted.size(), q));
}

std::optional<f64> rounded_fraction(u64 finite, u64 member) {
  if (member == 0) {
    return std::nullopt;
  }
  const f64 x = static_cast<f64>(finite) / static_cast<f64>(member);
  std::array<char, 64> buf{};
  const auto printed =
      std::to_chars(buf.data(), buf.data() + buf.size(), x, std::chars_format::fixed, 6);
  if (printed.ec != std::errc{}) {
    return std::nullopt;
  }
  f64 out = 0.0;
  const auto parsed = std::from_chars(buf.data(), printed.ptr, out);
  if (parsed.ec != std::errc{}) {
    return std::nullopt;
  }
  return out;
}

} // namespace atx::engine::research::fields
