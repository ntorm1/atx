#include "strategy_price_exposures.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
constexpr usize kCols = kPriceExposureCount;
constexpr usize kParams = kPriceExposureCount + 1; // intercept + exposures
using Vec = std::array<f64, kParams>;
using Mat = std::array<f64, kParams * kParams>;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kMaxUsize = std::numeric_limits<usize>::max();
constexpr usize kMaxWindow = 4096;
constexpr usize kMaxReturnCells = usize{1} << 26; // 512 MiB of f64 return scratch
// Same adjacent-interval guard and operation order as the target replay's rough
// return (strategy_target_replay.cpp): an adjusted move beyond |log| 1.5, or one
// the raw close does not corroborate within 0.10 log, is a data artifact.
constexpr f64 kGuardAbsLog = 1.5, kGuardExcessLog = .10;
// Floor on each Cholesky pivot of the unit-diagonal (Jacobi-equilibrated) normal
// matrix, i.e. on 1 - R^2 of a column against its predecessors. It trips only on
// numerically exact collinearity; real beta/vol/ADV correlations sit far above.
constexpr f64 kMinPivot = 1e-8;
// An exposure whose SD is below this fraction of its magnitude is constant up to
// rounding; z-scoring it would promote rounding noise to a regressor.
constexpr f64 kRelativeSdFloor = 1e-12;
// A residual this small relative to the entry gross is rounding noise, not a
// target: rescaling it to the entry gross would publish amplified noise.
constexpr f64 kMinResidualFraction = 1e-9;

[[nodiscard]] co::Status validate_config(const PriceExposureConfig& c) {
  const bool windows = c.beta_window >= 2 && c.beta_window <= kMaxWindow &&
                       c.vol_window >= 2 && c.vol_window <= kMaxWindow &&
                       c.adv_window >= 1 && c.adv_window <= kMaxWindow &&
                       c.min_return_pairs >= 2 && c.min_return_pairs <= c.beta_window;
  if (!windows || c.min_names < kParams + 1 || !std::isfinite(c.clip_z) || !(c.clip_z > 0))
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: invalid config");
  return co::Ok();
}
template <class T> [[nodiscard]] co::Status grow(std::vector<T>& v, usize size) {
  try {
    if (v.size() < size) v.resize(size);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "price exposures: scratch allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "price exposures: scratch extent");
  }
  return co::Ok();
}

// ---------------------------------------------------------------- exposures
[[nodiscard]] co::Status validate_input(const PriceExposureInput& in, usize d, usize block,
                                        std::span<const f64> out, std::span<const u8> ok) {
  if (!in.dates || !in.instruments || in.dates > kMaxUsize / in.instruments)
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: dimensions");
  if (in.instruments > kMaxReturnCells / block)
    return co::Err(co::ErrorCode::OutOfRange, "price exposures: working size");
  const usize cells = in.dates * in.instruments;
  if (in.close.size() != cells || in.raw_close.size() != cells || in.volume.size() != cells ||
      in.present.size() != cells || out.size() != in.instruments * kCols ||
      ok.size() != in.instruments)
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: span geometry");
  if (d >= in.dates)
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: decision outside panel");
  return co::Ok();
}
// Interval t spans sessions (t-1, t]. The return block is the trailing
// max(beta_window, vol_window) intervals ending at d, clipped at the panel start.
struct Windows {
  usize first{}, intervals{};     // block = intervals [first, first + intervals)
  usize beta_rows{}, vol_rows{};  // trailing block rows each estimator reads
  bool adv_full{};                // the whole ADV window lies inside the panel
  usize first_session{};          // earliest session any estimator reads
};
[[nodiscard]] Windows windows_at(const PriceExposureConfig& cfg, usize d) {
  const usize block = std::max(cfg.beta_window, cfg.vol_window);
  Windows w;
  w.first = d >= block ? d + 1 - block : 1;
  w.intervals = d >= w.first ? d + 1 - w.first : 0;
  w.beta_rows = std::min(cfg.beta_window, w.intervals);
  w.vol_rows = std::min(cfg.vol_window, w.intervals);
  w.adv_full = d + 1 >= cfg.adv_window;
  w.first_session = w.intervals ? w.first - 1 : d;
  if (w.adv_full) w.first_session = std::min(w.first_session, d + 1 - cfg.adv_window);
  return w;
}
[[nodiscard]] co::Status check_presence(const PriceExposureInput& in, usize first_session,
                                        usize d) {
  for (usize k = first_session * in.instruments; k < (d + 1) * in.instruments; ++k)
    if (in.present[k] > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: presence byte");
  return co::Ok();
}
[[nodiscard]] co::Status reserve_scratch(PriceExposureScratch& s, usize n, usize block) {
  ATX_TRY_VOID(grow(s.returns, n * block));
  ATX_TRY_VOID(grow(s.market, block));
  ATX_TRY_VOID(grow(s.logs, 4 * n));
  return grow(s.dollars, n);
}
// Log adjusted (first half) and raw (second half) close of one session; NaN
// unless the row is present with finite positive close and raw_close.
void load_logs(const PriceExposureInput& in, usize session, std::span<f64> logs) {
  const usize n = in.instruments, base = session * n;
  for (usize i = 0; i < n; ++i) {
    const f64 close = in.close[base + i], raw = in.raw_close[base + i];
    const bool priced = in.present[base + i] != 0 && std::isfinite(close) &&
                        std::isfinite(raw) && close > 0 && raw > 0;
    logs[i] = priced ? std::log(close) : kNaN;
    logs[n + i] = priced ? std::log(raw) : kNaN;
  }
}
// Valid interval returns (instrument-major, stride w.intervals; NaN if invalid)
// and the equal-weight market return per interval. Each session is logged once.
void fill_returns(const PriceExposureInput& in, const Windows& w, PriceExposureScratch& s) {
  const usize n = in.instruments, rows = w.intervals;
  if (!rows) return;
  std::span<f64> prev(s.logs.data(), 2 * n), cur(s.logs.data() + 2 * n, 2 * n);
  load_logs(in, w.first - 1, prev);
  for (usize j = 0; j < rows; ++j) {
    const usize b = (w.first + j) * n, a = b - n;
    load_logs(in, w.first + j, cur);
    f64 sum = 0;
    usize count = 0;
    for (usize i = 0; i < n; ++i) {
      f64 r = kNaN;
      if (!std::isnan(prev[i]) && !std::isnan(cur[i])) {
        r = in.close[b + i] / in.close[a + i] - 1;
        const f64 log_return = cur[i] - prev[i], raw_log_return = cur[n + i] - prev[n + i];
        if (!std::isfinite(r) || std::abs(log_return) > kGuardAbsLog ||
            std::abs(log_return) > std::abs(raw_log_return) + kGuardExcessLog)
          r = kNaN;
      }
      s.returns[i * rows + j] = r;
      if (!std::isnan(r)) {
        sum += r;
        ++count;
      }
    }
    s.market[j] = count ? sum / static_cast<f64>(count) : kNaN;
    std::swap(prev, cur);
  }
}
// Dollar volume summed over sessions [first_session, d]; unusable sessions add 0.
void sum_dollars(const PriceExposureInput& in, usize first_session, usize d,
                 std::span<f64> sum) {
  std::fill(sum.begin(), sum.end(), 0.0);
  for (usize t = first_session; t < d + 1; ++t) {
    const usize base = t * in.instruments;
    for (usize i = 0; i < in.instruments; ++i) {
      const f64 raw = in.raw_close[base + i], shares = in.volume[base + i];
      const f64 dollars = raw * shares;
      if (in.present[base + i] != 0 && std::isfinite(raw) && raw > 0 &&
          std::isfinite(shares) && shares >= 0 && std::isfinite(dollars))
        sum[i] += dollars;
    }
  }
}
// Two-pass sample cov(r, m)/var(m) over pairs with both sides valid.
[[nodiscard]] f64 beta_of(std::span<const f64> r, std::span<const f64> m, usize min_pairs) {
  usize n = 0;
  f64 sum_r = 0, sum_m = 0;
  for (usize j = 0; j < r.size(); ++j) {
    if (std::isnan(r[j]) || std::isnan(m[j])) continue;
    ++n;
    sum_r += r[j];
    sum_m += m[j];
  }
  if (n < min_pairs) return kNaN;
  const f64 mean_r = sum_r / static_cast<f64>(n), mean_m = sum_m / static_cast<f64>(n);
  f64 cov = 0, var = 0;
  for (usize j = 0; j < r.size(); ++j) {
    if (std::isnan(r[j]) || std::isnan(m[j])) continue;
    const f64 dm = m[j] - mean_m;
    cov += (r[j] - mean_r) * dm;
    var += dm * dm;
  }
  return var > 0 ? cov / var : kNaN;
}
// Two-pass sample SD over valid returns; min_count >= 2.
[[nodiscard]] f64 vol_of(std::span<const f64> r, usize min_count) {
  usize n = 0;
  f64 sum = 0;
  for (const f64 x : r) {
    if (std::isnan(x)) continue;
    ++n;
    sum += x;
  }
  if (n < min_count) return kNaN;
  const f64 mean = sum / static_cast<f64>(n);
  f64 squares = 0;
  for (const f64 x : r)
    if (!std::isnan(x)) squares += (x - mean) * (x - mean);
  return std::sqrt(squares / static_cast<f64>(n - 1));
}

// ------------------------------------------------------------- neutralization
[[nodiscard]] co::Status validate_neutralize(std::span<const f64> target,
                                             std::span<const u8> member,
                                             std::span<const f64> exposures,
                                             std::span<const u8> ok) {
  const usize n = target.size();
  if (!n || member.size() != n || ok.size() != n || n > kMaxUsize / kCols ||
      exposures.size() != n * kCols)
    return co::Err(co::ErrorCode::InvalidArgument, "price neutralize: span geometry");
  for (usize i = 0; i < n; ++i) {
    if (member[i] > 1 || ok[i] > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "price neutralize: flag byte");
    if (member[i] ? !std::isfinite(target[i]) : target[i] != 0)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "price neutralize: members finite, nonmembers exactly zero");
    for (usize k = 0; ok[i] && k < kCols; ++k)
      if (!std::isfinite(exposures[i * kCols + k]))
        return co::Err(co::ErrorCode::InvalidArgument,
                       "price neutralize: ok row with non-finite exposure");
  }
  return co::Ok();
}
[[nodiscard]] co::Status reserve_scratch(NeutralizeScratch& s, usize n) {
  ATX_TRY_VOID(grow(s.rows, n));
  ATX_TRY_VOID(grow(s.z, n * kCols));
  return grow(s.residual, n);
}

// ------------------------------------------------------- within-group demeaning
constexpr usize kUnknownSlot = kMaxGroupId + 1, kFallbackSlot = kMaxGroupId + 2;
constexpr usize kSlotSums = kCols + 1; // the target, then every z column
static_assert(kFallbackSlot + 1 == kGroupSlots, "slots: every id, unknown, fallback");
[[nodiscard]] co::Status reserve_group_scratch(NeutralizeScratch& s, usize n) {
  ATX_TRY_VOID(grow(s.slot, n));
  ATX_TRY_VOID(grow(s.slot_count, kGroupSlots));
  return grow(s.slot_sum, kGroupSlots * kSlotSums);
}
// Slot of every used row (s.rows[0, stats.used)): its integer id, kUnknownSlot for
// NaN, then kFallbackSlot when that slot holds fewer than kMinGroupNames used rows.
// Fills stats.groups / unknown_group_names / fallback_names.
[[nodiscard]] co::Status assign_slots(std::span<const f64> group, NeutralizeScratch& s,
                                      NeutralizeStats& stats) {
  const usize n = stats.used;
  std::fill(s.slot_count.begin(), s.slot_count.end(), usize{0});
  for (usize r = 0; r < n; ++r) {
    const f64 id = group[s.rows[r]];
    usize slot = kUnknownSlot;
    if (!std::isnan(id)) {
      // The negated range also refuses +-inf; floor rejects a fractional id.
      if (!(id >= 0 && id <= static_cast<f64>(kMaxGroupId)) || id != std::floor(id))
        return co::Err(co::ErrorCode::InvalidArgument, "price neutralize: group id");
      slot = static_cast<usize>(id);
    }
    s.slot[r] = slot;
    ++s.slot_count[slot];
  }
  for (usize r = 0; r < n; ++r) {
    if (s.slot[r] == kUnknownSlot) ++stats.unknown_group_names;
    if (s.slot_count[s.slot[r]] >= kMinGroupNames) continue;
    s.slot[r] = kFallbackSlot;
    ++stats.fallback_names;
  }
  for (usize slot = 0; slot < kFallbackSlot; ++slot)
    stats.groups += s.slot_count[slot] >= kMinGroupNames ? 1U : 0U;
  stats.groups += stats.fallback_names ? 1U : 0U;
  return co::Ok();
}
// FWL step: each row's target e[r] and z columns minus its slot's means over the used
// rows (sums in ascending row order, so equal slots give equal bits whatever the id).
// False when a z column is spanned by the slots: its within-slot sum of squares is
// at most kMinPivot x its sum of squares (1 - R^2 on the slot indicators), the grouped
// twin of the Cholesky pivot floor, so a between-group-only exposure never enters the
// fit as rounding noise.
[[nodiscard]] bool demean_within_groups(NeutralizeScratch& s, std::span<f64> z,
                                        std::span<f64> e) {
  std::fill(s.slot_count.begin(), s.slot_count.end(), usize{0});
  std::fill(s.slot_sum.begin(), s.slot_sum.end(), 0.0);
  std::array<f64, kCols> total{}, within{};
  for (usize r = 0; r < e.size(); ++r) {
    const usize base = s.slot[r] * kSlotSums;
    ++s.slot_count[s.slot[r]];
    s.slot_sum[base] += e[r];
    for (usize k = 0; k < kCols; ++k) {
      const f64 x = z[r * kCols + k];
      s.slot_sum[base + k + 1] += x;
      total[k] += x * x;
    }
  }
  for (usize r = 0; r < e.size(); ++r) {
    const usize base = s.slot[r] * kSlotSums;
    const f64 count = static_cast<f64>(s.slot_count[s.slot[r]]);
    e[r] -= s.slot_sum[base] / count;
    for (usize k = 0; k < kCols; ++k) {
      f64& x = z[r * kCols + k];
      x -= s.slot_sum[base + k + 1] / count;
      within[k] += x * x;
    }
  }
  for (usize k = 0; k < kCols; ++k)
    if (!(within[k] > kMinPivot * total[k])) return false;
  return true;
}
// Column k of z over rows: (x - mean) / sample SD, clipped to +-clip. False if
// the column is constant up to rounding (or not finite).
[[nodiscard]] bool standardize(std::span<const f64> exposures, std::span<const usize> rows,
                               usize k, f64 clip, std::span<f64> z) {
  f64 sum = 0, magnitude = 0;
  for (const usize i : rows) {
    const f64 x = exposures[i * kCols + k];
    sum += x;
    magnitude = std::max(magnitude, std::abs(x));
  }
  const f64 mean = sum / static_cast<f64>(rows.size());
  f64 squares = 0;
  for (const usize i : rows) {
    const f64 dx = exposures[i * kCols + k] - mean;
    squares += dx * dx;
  }
  const f64 sd = std::sqrt(squares / static_cast<f64>(rows.size() - 1));
  if (!std::isfinite(sd) || !(sd > kRelativeSdFloor * magnitude)) return false;
  for (usize r = 0; r < rows.size(); ++r)
    z[r * kCols + k] = std::clamp((exposures[rows[r] * kCols + k] - mean) / sd, -clip, clip);
  return true;
}
[[nodiscard]] Vec design_row(std::span<const f64> z, usize r) {
  Vec x{};
  x[0] = 1;
  for (usize k = 0; k < kCols; ++k) x[k + 1] = z[r * kCols + k];
  return x;
}
[[nodiscard]] Mat normal_matrix(std::span<const f64> z, usize rows) {
  Mat a{};
  for (usize r = 0; r < rows; ++r) {
    const Vec x = design_row(z, r);
    for (usize p = 0; p < kParams; ++p)
      for (usize q = 0; q <= p; ++q) a[p * kParams + q] += x[p] * x[q];
  }
  for (usize p = 0; p < kParams; ++p)
    for (usize q = p + 1; q < kParams; ++q) a[p * kParams + q] = a[q * kParams + p];
  return a;
}
// X'y over the regressed rows.
[[nodiscard]] Vec cross(std::span<const f64> z, std::span<const f64> y) {
  Vec b{};
  for (usize r = 0; r < y.size(); ++r) {
    const Vec x = design_row(z, r);
    for (usize p = 0; p < kParams; ++p) b[p] += x[p] * y[r];
  }
  return b;
}
void subtract_fit(std::span<const f64> z, const Vec& coef, std::span<f64> e) {
  for (usize r = 0; r < e.size(); ++r) {
    const Vec x = design_row(z, r);
    f64 fit = 0;
    for (usize p = 0; p < kParams; ++p) fit += x[p] * coef[p];
    e[r] -= fit;
  }
}
// Jacobi-equilibrated Cholesky: C = S A S with S = diag(A_kk^-1/2), C = L L'.
// C has a unit diagonal, so the pivot floor is scale-free.
struct Cholesky {
  Mat l{};
  Vec scale{};
};
[[nodiscard]] bool factor(const Mat& a, Cholesky& f) {
  for (usize k = 0; k < kParams; ++k) {
    const f64 diag = a[k * kParams + k];
    if (!std::isfinite(diag) || !(diag > 0)) return false;
    f.scale[k] = 1 / std::sqrt(diag);
  }
  for (usize j = 0; j < kParams; ++j) {
    f64 pivot = a[j * kParams + j] * f.scale[j] * f.scale[j];
    for (usize p = 0; p < j; ++p) pivot -= f.l[j * kParams + p] * f.l[j * kParams + p];
    if (!(pivot > kMinPivot)) return false;
    const f64 root = std::sqrt(pivot);
    f.l[j * kParams + j] = root;
    for (usize i = j + 1; i < kParams; ++i) {
      f64 v = a[i * kParams + j] * f.scale[i] * f.scale[j];
      for (usize p = 0; p < j; ++p) v -= f.l[i * kParams + p] * f.l[j * kParams + p];
      f.l[i * kParams + j] = v / root;
    }
  }
  return true;
}
// Solves A x = b as C (S^-1 x) = S b.
[[nodiscard]] Vec solve(const Cholesky& f, const Vec& b) {
  Vec y{};
  for (usize i = 0; i < kParams; ++i) {
    f64 v = b[i] * f.scale[i];
    for (usize p = 0; p < i; ++p) v -= f.l[i * kParams + p] * y[p];
    y[i] = v / f.l[i * kParams + i];
  }
  Vec x{};
  for (usize i = kParams; i-- > 0;) {
    f64 v = y[i];
    for (usize p = i + 1; p < kParams; ++p) v -= f.l[p * kParams + i] * x[p];
    x[i] = v / f.l[i * kParams + i];
  }
  for (usize k = 0; k < kParams; ++k) x[k] *= f.scale[k];
  return x;
}
// OLS residual of target on [1, z] over the used rows into s.residual; grouped: of
// the within-slot demeaned target on [1, demeaned z] (assign_slots ran). Touches
// only scratch and stats, so a refusal leaves the caller's target unmodified.
// Ungrouped, every arithmetic operation and its order is the price-risk-v1 one (the
// target copy into s.residual precedes the factorization, which never reads it).
[[nodiscard]] co::Status fit_residual(std::span<const f64> target,
                                      std::span<const f64> exposures, f64 clip, bool grouped,
                                      NeutralizeScratch& s, NeutralizeStats& stats) {
  const usize n = stats.used;
  const std::span<const usize> rows(s.rows.data(), n);
  const std::span<f64> z(s.z.data(), n * kCols), e(s.residual.data(), n);
  for (usize k = 0; k < kCols; ++k)
    if (!standardize(exposures, rows, k, clip, z))
      return co::Err(co::ErrorCode::Unavailable, "price neutralize: constant exposure");
  for (usize r = 0; r < n; ++r) e[r] = target[rows[r]];
  // The intercept column stays: after demeaning it is orthogonal to every other
  // column, so its coefficient is rounding noise and the residual is the FWL one.
  if (grouped && !demean_within_groups(s, z, e))
    return co::Err(co::ErrorCode::Unavailable, "price neutralize: exposure spanned by groups");
  Cholesky f;
  if (!factor(normal_matrix(z, n), f))
    return co::Err(co::ErrorCode::Unavailable, "price neutralize: ill-conditioned exposures");
  Vec coef = solve(f, cross(z, e));
  subtract_fit(z, coef, e);
  // One refinement step drives X'e from normal-equation error to rounding level.
  const Vec delta = solve(f, cross(z, e));
  subtract_fit(z, delta, e);
  for (usize p = 0; p < kParams; ++p) coef[p] += delta[p];
  f64 gross = 0;
  for (const f64 v : e) gross += std::abs(v);
  if (!std::isfinite(gross) || !(gross > kMinResidualFraction * stats.gross))
    return co::Err(co::ErrorCode::Unavailable, "price neutralize: target spanned by exposures");
  stats.residual_gross = gross;
  stats.coefficients = coef;
  return co::Ok();
}
// neutralize_target (group empty) and neutralize_target_within_groups (group sized by
// the caller): one body, so the ungrouped path is price-risk-v1 operation for operation.
[[nodiscard]] co::Status neutralize(std::span<f64> target, std::span<const u8> member,
                                    std::span<const f64> exposures, std::span<const u8> ok,
                                    std::span<const f64> group, const PriceExposureConfig& cfg,
                                    NeutralizeScratch& scratch, NeutralizeStats& stats) {
  const bool grouped = !group.empty();
  ATX_TRY_VOID(validate_config(cfg));
  ATX_TRY_VOID(validate_neutralize(target, member, exposures, ok));
  ATX_TRY_VOID(reserve_scratch(scratch, target.size()));
  if (grouped) ATX_TRY_VOID(reserve_group_scratch(scratch, target.size()));
  for (usize i = 0; i < target.size(); ++i) {
    if (!member[i]) continue;
    stats.gross += std::abs(target[i]);
    if (ok[i]) {
      scratch.rows[stats.used++] = i;
    } else {
      ++stats.excluded;
      stats.excluded_gross += std::abs(target[i]);
    }
  }
  if (stats.used < cfg.min_names)
    return co::Err(co::ErrorCode::Unavailable, "price neutralize: too few usable names");
  if (grouped) ATX_TRY_VOID(assign_slots(group, scratch, stats));
  if (stats.gross == 0) return co::Ok(); // flat stays flat: nothing to remove or rescale
  ATX_TRY_VOID(fit_residual(target, exposures, cfg.clip_z, grouped, scratch, stats));
  const f64 scale = stats.gross / stats.residual_gross;
  for (usize i = 0; i < target.size(); ++i)
    if (member[i] && !ok[i]) target[i] = 0;
  for (usize r = 0; r < stats.used; ++r) target[scratch.rows[r]] = scratch.residual[r] * scale;
  return co::Ok();
}
// neutralize_price_risk (group empty) and its within-groups twin.
[[nodiscard]] co::Status price_risk(const PriceExposureInput& in, const PriceExposureConfig& cfg,
                                    usize d, std::span<f64> target, std::span<const u8> member,
                                    std::span<const f64> group, PriceRiskScratch& scratch,
                                    NeutralizeStats& stats) {
  const usize n = in.instruments;
  if (target.size() != n || member.size() != n || n > kMaxReturnCells)
    return co::Err(co::ErrorCode::InvalidArgument, "price risk: target geometry");
  ATX_TRY_VOID(grow(scratch.exposures, n * kCols));
  ATX_TRY_VOID(grow(scratch.ok, n));
  const std::span<f64> exposures(scratch.exposures.data(), n * kCols);
  const std::span<u8> ok(scratch.ok.data(), n);
  ATX_TRY_VOID(compute_price_exposures(in, cfg, d, scratch.exposure, exposures, ok));
  return neutralize(target, member, exposures, ok, group, cfg, scratch.neutralize, stats);
}
} // namespace

co::Status compute_price_exposures(const PriceExposureInput& in, const PriceExposureConfig& cfg,
                                   usize d, PriceExposureScratch& scratch,
                                   std::span<f64> out, std::span<u8> ok) {
  ATX_TRY_VOID(validate_config(cfg));
  const usize block = std::max(cfg.beta_window, cfg.vol_window);
  ATX_TRY_VOID(validate_input(in, d, block, out, ok));
  const Windows w = windows_at(cfg, d);
  ATX_TRY_VOID(check_presence(in, w.first_session, d));
  ATX_TRY_VOID(reserve_scratch(scratch, in.instruments, block));
  fill_returns(in, w, scratch);
  const std::span<f64> dollars(scratch.dollars.data(), in.instruments);
  if (w.adv_full) sum_dollars(in, d + 1 - cfg.adv_window, d, dollars);
  const usize vol_count = std::max<usize>(2, (cfg.vol_window + 1) / 2);
  const std::span<const f64> market(scratch.market.data() + (w.intervals - w.beta_rows),
                                    w.beta_rows);
  for (usize i = 0; i < in.instruments; ++i) {
    const std::span<const f64> r(scratch.returns.data() + i * w.intervals, w.intervals);
    const f64 beta = beta_of(r.last(w.beta_rows), market, cfg.min_return_pairs);
    const f64 vol = vol_of(r.last(w.vol_rows), vol_count);
    const f64 mean_dollars = w.adv_full ? dollars[i] / static_cast<f64>(cfg.adv_window) : 0;
    const f64 log_adv = mean_dollars > 0 ? std::log(mean_dollars) : kNaN;
    out[i * kCols + kExposureBeta] = beta;
    out[i * kCols + kExposureVol] = vol;
    out[i * kCols + kExposureLogAdv] = log_adv;
    ok[i] = static_cast<u8>(std::isfinite(beta) && std::isfinite(vol) && std::isfinite(log_adv));
  }
  return co::Ok();
}

co::Status neutralize_target(std::span<f64> target, std::span<const u8> member,
                             std::span<const f64> exposures, std::span<const u8> ok,
                             const PriceExposureConfig& cfg, NeutralizeScratch& scratch,
                             NeutralizeStats& stats) {
  stats = {};
  return neutralize(target, member, exposures, ok, {}, cfg, scratch, stats);
}

co::Status neutralize_price_risk(const PriceExposureInput& in, const PriceExposureConfig& cfg,
                                 usize d, std::span<f64> target, std::span<const u8> member,
                                 PriceRiskScratch& scratch, NeutralizeStats& stats) {
  stats = {};
  return price_risk(in, cfg, d, target, member, {}, scratch, stats);
}

co::Status neutralize_target_within_groups(std::span<f64> target, std::span<const u8> member,
                                           std::span<const f64> exposures,
                                           std::span<const u8> ok, std::span<const f64> group,
                                           const PriceExposureConfig& cfg,
                                           NeutralizeScratch& scratch, NeutralizeStats& stats) {
  stats = {};
  // An empty group span would silently select the ungrouped path.
  if (target.empty() || group.size() != target.size())
    return co::Err(co::ErrorCode::InvalidArgument, "price neutralize: group geometry");
  return neutralize(target, member, exposures, ok, group, cfg, scratch, stats);
}

co::Status neutralize_price_risk_within_groups(const PriceExposureInput& in,
                                               const PriceExposureConfig& cfg, usize d,
                                               std::span<f64> target,
                                               std::span<const u8> member,
                                               std::span<const f64> group,
                                               PriceRiskScratch& scratch, NeutralizeStats& stats) {
  stats = {};
  if (!in.instruments || group.size() != in.instruments)
    return co::Err(co::ErrorCode::InvalidArgument, "price risk: group geometry");
  return price_risk(in, cfg, d, target, member, group, scratch, stats);
}
} // namespace atx::impl::strategy
