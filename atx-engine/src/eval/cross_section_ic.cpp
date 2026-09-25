#include "atx/engine/eval/cross_section_ic.hpp"

#include <algorithm> // std::sort
#include <cmath>     // std::isfinite, std::sqrt, std::fabs, std::ceil
#include <cstddef>   // std::ptrdiff_t
#include <limits>    // std::numeric_limits (the dates*instruments overflow guard)
#include <numeric>   // std::iota
#include <string>    // std::to_string (counts carried in error messages)
#include <utility>   // std::move

#include "atx/core/random.hpp"              // Xoshiro256pp
#include "atx/core/stats/cross_section.hpp" // core::stats::rank (scratch overload, §3.5)

namespace atx::engine::eval {

using atx::core::ErrorCode;
using atx::core::Err;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

// ---------------------------------------------------------------------------
//  Everything below is TU-private (MIN-5): an anonymous namespace, not
//  `eval::detail`, so nothing here is exported from `atx-engine.lib` and a
//  later `validate` or `check_cell_span` in another `eval/*.cpp` shadows rather
//  than collides. The two shared, frozen predicates — `detail::block_len` and
//  `detail::series_reportable` — live in the header instead (ruling RR-3),
//  because T3's bootstrap and T7a's oracle both call them.
// ---------------------------------------------------------------------------
namespace {

// ---------------------------------------------------------------------------
//  strictly_increasing — the session-key / horizon monotonicity predicate.
//  Vacuously true for fewer than two elements.
// ---------------------------------------------------------------------------
template <class T>
[[nodiscard]] bool strictly_increasing(std::span<const T> values) noexcept {
  for (atx::usize i = 1U; i < values.size(); ++i) {
    if (!(values[i - 1U] < values[i])) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
//  finite_non_negative / finite_positive — the scalar contracts of §6.2. A NaN
//  fails both comparisons, so the `!` spelling rejects it without a separate
//  isnan branch; `std::isfinite` additionally rejects +/-inf.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool finite_non_negative(atx::f64 v) noexcept {
  return std::isfinite(v) && v >= 0.0;
}

[[nodiscard]] inline bool finite_positive(atx::f64 v) noexcept {
  return std::isfinite(v) && v > 0.0;
}

// ---------------------------------------------------------------------------
//  check_cell_span — every panel span is date-major of length dates*instruments
//  (§3, storage layout). A mis-sized span is OutOfRange, not InvalidArgument:
//  the shape disagrees with the declared extents rather than the value being
//  out of contract.
// ---------------------------------------------------------------------------
template <class T>
[[nodiscard]] Status check_cell_span(std::span<const T> values, atx::usize expected,
                                     const char *name) {
  if (values.size() != expected) {
    return Err(ErrorCode::OutOfRange, std::string{"cross_section_ic: "} + name + " has " +
                                          std::to_string(values.size()) + " elements, expected " +
                                          std::to_string(expected));
  }
  return Ok();
}

// ---------------------------------------------------------------------------
//  validate — every §6.2 branch, in one place, run by plan_cross_section_ic so
//  that a configuration which survives the plan is one compute may assume.
//
//  Order is deliberate: extents first (so `dates * instruments` is meaningful),
//  then shapes, then the horizon list (so `dates - h` below cannot underflow),
//  then scalars, and only then the draws-versus-reportability rule, which
//  depends on all of the above.
// ---------------------------------------------------------------------------
[[nodiscard]] Status validate(const CrossSectionIcInput &in, const CrossSectionIcConfig &cfg) {
  // --- rejectable admission enums: `Unknown` is never a working default ------
  switch (cfg.forward_variant) {
  case ForwardReturnVariant::Unknown:
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: forward_variant is Unknown; name DropMissingForward or "
               "IncludeAuditedTerminalV1 explicitly");
  case ForwardReturnVariant::DropMissingForward:
  case ForwardReturnVariant::IncludeAuditedTerminalV1:
    break;
  }
  switch (cfg.ties) {
  case IcTieHandling::Unknown:
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: ties is Unknown; name AverageRanksV1 explicitly");
  case IcTieHandling::AverageRanksV1:
    break;
  }
  // W0-E0a: the two versioned numeric rules. A value outside the named enumerators (a
  // cast from a wider integer) matches no case and is rejected exactly like `Unknown`.
  bool block_rule_ok = false;
  switch (cfg.block_len_rule) {
  case BlockLenRule::HalfHorizonV1:
  case BlockLenRule::TwoHorizonV2:
  case BlockLenRule::PolitisWhiteV2:
    block_rule_ok = true;
    break;
  case BlockLenRule::Unknown:
    break;
  }
  if (!block_rule_ok) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: block_len_rule must be HalfHorizonV1, TwoHorizonV2 or "
               "PolitisWhiteV2");
  }
  bool hac_rule_ok = false;
  switch (cfg.hac_rule) {
  case IcHacRule::HansenHodrickV1:
  case IcHacRule::NeweyWestV1:
    hac_rule_ok = true;
    break;
  case IcHacRule::Unknown:
    break;
  }
  if (!hac_rule_ok) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: hac_rule must be HansenHodrickV1 or NeweyWestV1");
  }
  if (cfg.execution_delay > kMaxIcExecutionDelay) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: execution_delay " + std::to_string(cfg.execution_delay) +
                   " exceeds kMaxIcExecutionDelay " + std::to_string(kMaxIcExecutionDelay));
  }

  // --- extents and the E-08 runtime budget ----------------------------------------
  // Ruling RR-1 still holds — every extent is bounded before any sizing, so no
  // `.assign()` in `plan_cross_section_ic` can reach a throwing size — but the binding
  // limit is now the runtime working-set budget, not a 4,096 constant (E-08). The
  // preflight also carries the dates*instruments overflow guard that makes `cells`
  // below a defined quantity.
  ATX_TRY(const IcSizing sizing, preflight_cross_section_ic(in.dates, in.instruments, cfg));
  const atx::usize cells = sizing.cells;

  // --- span shapes -----------------------------------------------------------
  ATX_TRY_VOID(check_cell_span<atx::f64>(in.signal, cells, "signal"));
  ATX_TRY_VOID(check_cell_span<atx::f64>(in.price, cells, "price"));
  ATX_TRY_VOID(check_cell_span<atx::f64>(in.raw_price, cells, "raw_price"));
  ATX_TRY_VOID(check_cell_span<atx::u8>(in.mask, cells, "mask"));
  ATX_TRY_VOID(check_cell_span<atx::u8>(in.terminal, cells, "terminal"));
  ATX_TRY_VOID(check_cell_span<atx::u8>(in.terminal_evidenced, cells, "terminal_evidenced"));
  ATX_TRY_VOID(check_cell_span<atx::f64>(in.terminal_value, cells, "terminal_value"));
  ATX_TRY_VOID(check_cell_span<atx::u8>(in.excluded_audited, cells, "excluded_audited"));
  ATX_TRY_VOID(check_cell_span<atx::i64>(in.session_keys, in.dates, "session_keys"));
  if (!in.aux.empty() && in.aux.size() != cells) {
    return Err(ErrorCode::OutOfRange, "cross-section ic: aux extent");
  }

  // Strictly increasing session keys are load-bearing twice over: the seal
  // counts against them (§5.2) and the borrow term divides their differences
  // into calendar days (§3.12), which a non-increasing pair would make negative.
  if (!strictly_increasing<atx::i64>(in.session_keys)) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: session_keys must be strictly increasing");
  }

  // --- horizon list ----------------------------------------------------------
  if (cfg.horizons.empty()) {
    return Err(ErrorCode::InvalidArgument, "cross_section_ic: horizons is empty");
  }
  // horizons.size() <= kMaxIcHorizons was enforced by the preflight above.
  if (cfg.horizons.front() == 0U) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: horizon 0 is not a forward return; each horizon must be >= 1");
  }
  if (!strictly_increasing<atx::usize>(cfg.horizons)) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: horizons must be strictly increasing");
  }
  // horizons.back() + execution_delay >= dates leaves no date with a forward mark at
  // all; the series would be empty rather than short, so it is a shape error. Both
  // terms are bounded (kMaxIcDates, kMaxIcExecutionDelay), so the sum cannot wrap.
  if (cfg.horizons.back() >= in.dates ||
      cfg.horizons.back() + cfg.execution_delay >= in.dates) {
    return Err(ErrorCode::OutOfRange,
               "cross_section_ic: horizon " + std::to_string(cfg.horizons.back()) +
                   " plus execution_delay " + std::to_string(cfg.execution_delay) +
                   " is at or past dates " + std::to_string(in.dates));
  }

  // --- scalars ---------------------------------------------------------------
  if (cfg.quantiles < 2U || cfg.quantiles > kMaxIcQuantiles) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: quantiles " + std::to_string(cfg.quantiles) +
                   " outside [2, " + std::to_string(kMaxIcQuantiles) + "]");
  }
  if (cfg.min_names_per_date < 2U) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: min_names_per_date must be >= 2 (a one-name cross-section "
               "has no correlation)");
  }
  if (cfg.block_len_floor == 0U) {
    return Err(ErrorCode::InvalidArgument, "cross_section_ic: block_len_floor must be >= 1");
  }
  // bootstrap_draws <= kMaxBootstrapDraws was enforced by the preflight above.
  if (!finite_non_negative(cfg.trade_bps)) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: trade_bps must be finite and non-negative");
  }
  if (!finite_non_negative(cfg.annual_borrow_bps)) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: annual_borrow_bps must be finite and non-negative");
  }
  if (!finite_positive(cfg.short_leg_gross)) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: short_leg_gross must be finite and positive");
  }
  if (cfg.day_basis != 360 && cfg.day_basis != 365) {
    return Err(ErrorCode::InvalidArgument, "cross_section_ic: day_basis must be 360 or 365 (got " +
                                               std::to_string(cfg.day_basis) + ")");
  }
  if (cfg.common_sample_dates > in.dates) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: common_sample_dates " + std::to_string(cfg.common_sample_dates) +
                   " exceeds dates " + std::to_string(in.dates));
  }

  // §3.10 / ruling R-C: each stream-key field owns ONE byte, which is what keeps
  // the XOR injective. A field above 255 would alias its neighbour's byte and
  // silently merge two bootstrap streams — reproducible, and wrong. Rejected
  // rather than masked: masking would publish an interval drawn from a stream the
  // receipt could not name.
  {
    const atx::u64 stream_fields[] = {cfg.stream_signal_index, cfg.stream_variant_id,
                                      cfg.stream_restriction_id, cfg.stream_sample_id};
    for (const atx::u64 field : stream_fields) {
      if (field > 255ULL) {
        return Err(ErrorCode::InvalidArgument,
                   "cross_section_ic: every stream_* field must fit in one byte (got " +
                       std::to_string(field) + "); §3.10's key gives each field its own byte");
      }
    }
  }

  // --- draws == 0 is a whole-run choice, never a silent degradation ----------
  // The maximum emittable series length at horizon h is dates - h (every date
  // emits). If ANY horizon would clear the n >= 20 and floor(n/L) >= 10 bars but
  // for draws == 0, the caller has disabled intervals it was about to be given;
  // reject rather than emit a run whose unreportable_reason is 1 everywhere.
  // Under PolitisWhiteV2 the rule's lower bound is used: the data-driven term can only
  // lengthen L, so a horizon this bound calls unreportable stays unreportable.
  if (cfg.bootstrap_draws == 0U) {
    for (const atx::usize h : cfg.horizons) {
      // safe: horizons.back() + execution_delay < dates, checked above
      const atx::usize n = in.dates - h - cfg.execution_delay;
      if (detail::series_reportable(
              n, detail::block_len_for_rule(cfg.block_len_rule, h, cfg.block_len_floor), 1U)) {
        return Err(ErrorCode::InvalidArgument,
                   "cross_section_ic: bootstrap_draws == 0 while horizon " + std::to_string(h) +
                       " would otherwise be reportable; disabling intervals must be an explicit "
                       "whole-run choice");
      }
    }
  }
  return Ok();
}

// ---------------------------------------------------------------------------
//  check_scratch — the caller-contract branch of §6.2. A scratch smaller than
//  `plan_cross_section_ic` sized would be an out-of-bounds write, so it is
//  `Internal` (a contract violation) rather than a value error.
// ---------------------------------------------------------------------------
[[nodiscard]] Status check_scratch(const CrossSectionIcInput &in, const CrossSectionIcConfig &cfg,
                                   const CrossSectionIcScratch &s) {
  const atx::usize inst = in.instruments;
  if (s.x.size() < inst || s.r.size() < inst || s.rx.size() < inst || s.rr.size() < inst ||
      s.buf.size() < inst || s.order.size() < inst || s.perm.size() < inst ||
      s.w_prev.size() < inst || s.w_curr.size() < inst || s.draw.size() < in.dates ||
      s.stat.size() < cfg.bootstrap_draws) {
    return Err(ErrorCode::Internal,
               "cross_section_ic: scratch is smaller than plan_cross_section_ic requires "
               "(pass the scratch the plan returned for this exact shape)");
  }
  return Ok();
}

// ---------------------------------------------------------------------------
//  priced — §3.3's mark predicate. A NaN fails both comparisons and a
//  non-positive price is NOT repaired; it is a missing forward return.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool priced(atx::f64 p) noexcept { return std::isfinite(p) && p > 0.0; }

// One date's coverage counters (§3.7) alongside the gathered cross-section.
struct DateGather {
  atx::usize n_eligible{};
  atx::usize n_signal_finite{};
  atx::usize n_with_forward{};
  atx::usize n_terminal_applied{};
  atx::usize n_terminal_unevidenced{};
  atx::usize n_excluded_audited{};
  atx::usize n_used{};
};

// ---------------------------------------------------------------------------
//  last_valid_mark — §3.8's `t_last`, searched in the HALF-OPEN window (t, t+h]
//  only (M-8), scanning backwards so the first hit is the last mark. Returns the
//  absolute row index, or 0 for "none" — 0 can never be a valid answer because
//  every candidate satisfies u > t >= 0. Under an execution delay `t` here is the
//  ENTRY row (signal row + delay), so the window is the one the position is held over.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::usize last_valid_mark(const CrossSectionIcInput &in, atx::usize t,
                                         atx::usize h, atx::usize i) noexcept {
  for (atx::usize u = t + h; u > t; --u) {
    if (priced(in.price[u * in.instruments + i])) {
      return u;
    }
  }
  return 0U;
}

// ---------------------------------------------------------------------------
//  gather_date — one cross-section: §3.1 admission, §3.3 forward returns with
//  §3.8's two variants and ruling A-6's precedence, and every §3.7 counter.
//
//  E-09: admission (mask, _ex34) and the signal are read at the signal row t; the
//  forward return runs from the ENTRY row e = t + execution_delay to e + h, and the
//  terminal triple is read at e (it describes the window (e, e + h]). At delay 0 every
//  read is the pre-W0 one.
//
//  Writes the used set into scratch.x (signal), scratch.r (forward return) and
//  scratch.perm (instrument index), in ASCENDING instrument index — the fixed
//  accumulation order §3.4 requires for bit-reproducibility, and the order that
//  makes §3.12's "(signal desc, instrument index asc)" tie-break expressible as a
//  tie-break on gathered position. Allocates nothing.
// ---------------------------------------------------------------------------
[[nodiscard]] DateGather gather_date(const CrossSectionIcInput &in,
                                     const CrossSectionIcConfig &cfg, atx::usize t, atx::usize h,
                                     CrossSectionIcScratch &scratch) noexcept {
  DateGather g{};
  const atx::usize inst = in.instruments;
  const bool restrict_ex34 = (cfg.stream_restriction_id != 0U);
  const bool variant_b = (cfg.forward_variant == ForwardReturnVariant::IncludeAuditedTerminalV1);
  const atx::usize base = t * inst;
  // Validation guarantees t + execution_delay + h < dates for every evaluated t.
  const atx::usize entry_row = t + cfg.execution_delay;
  const atx::usize entry = entry_row * inst;
  const atx::usize fwd = (entry_row + h) * inst;

  for (atx::usize i = 0; i < inst; ++i) {
    const atx::usize cell = base + i;
    const atx::usize entry_cell = entry + i;
    const bool audited = (in.excluded_audited[cell] != 0U);
    if (restrict_ex34 && audited) {
      continue; // §3.8 / AR-7: the _ex34 sub-universe, applied before eligibility
    }
    if (in.mask[cell] == 0U) {
      continue; // §3.1: the mask is the admission truth, whatever the value is
    }
    ++g.n_eligible;
    if (audited) {
      ++g.n_excluded_audited; // ELIGIBLE basis (ruling A-2)
    }
    const atx::f64 xv = in.signal[cell];
    if (!std::isfinite(xv)) {
      continue; // §3.6: excluded and counted, never imputed
    }
    ++g.n_signal_finite;

    const atx::f64 p0 = in.price[entry_cell];
    const atx::f64 p1 = in.price[fwd + i];
    atx::f64 rv = 0.0;
    bool used = false;
    if (priced(p0) && priced(p1)) {
      // Ruling A-6: a live close at t+h ALWAYS wins. The terminal leg is a
      // fallback for an undefined ordinary return, never an overlay on a live
      // price — applying both would double-count the consideration.
      //
      // The quotient is re-checked for finiteness exactly as the terminal branch
      // below is (M-1): `priced` guarantees both operands are finite and
      // positive but not that `p1 / p0` is — `p0 = 5e-324, p1 = 1e308` overflows
      // to +inf, which would reach `detail::pearson` as a NaN. Unreachable with
      // real closes; the symmetry is free.
      const atx::f64 candidate = p1 / p0 - 1.0;
      if (std::isfinite(candidate)) {
        rv = candidate;
        ++g.n_with_forward;
        used = true;
      }
    } else if (in.terminal[entry_cell] != 0U) {
      if (in.terminal_evidenced[entry_cell] == 0U) {
        // §2.3 / ruling AR-1: the PCS shape. Dropped and counted; the engine
        // never derives a terminal return from an unevidenced gap.
        ++g.n_terminal_unevidenced;
      } else if (variant_b && priced(p0)) {
        const atx::usize u_last = last_valid_mark(in, entry_row, h, i);
        if (u_last != 0U) {
          const atx::f64 raw_last = in.raw_price[u_last * inst + i];
          if (priced(raw_last)) {
            // The leg is priced against the RAW close: the consideration is a
            // cash amount per as-traded share (§3.8).
            const atx::f64 leg = in.terminal_value[entry_cell] / raw_last - 1.0;
            const atx::f64 candidate = (in.price[u_last * inst + i] / p0) * (1.0 + leg) - 1.0;
            if (std::isfinite(candidate)) {
              rv = candidate;
              ++g.n_terminal_applied;
              used = true;
            }
          }
        }
      }
    }
    if (used) {
      scratch.x[g.n_used] = xv;
      scratch.r[g.n_used] = rv;
      scratch.perm[g.n_used] = i;
      ++g.n_used;
    }
  }
  return g;
}

// Iterator distance for a scratch vector, spelled once so no call site narrows.
[[nodiscard]] inline std::vector<atx::usize>::difference_type idx_diff(atx::usize n) noexcept {
  return static_cast<std::vector<atx::usize>::difference_type>(n);
}

// ---------------------------------------------------------------------------
//  order_by_signal_desc — §3.12 step 2's total order: signal DESCENDING, ties
//  broken by ascending gathered position, which is ascending instrument index
//  because `gather_date` walks instruments in order. A total order makes the
//  bucket assignment deterministic under ties.
// ---------------------------------------------------------------------------
void order_by_signal_desc(CrossSectionIcScratch &scratch, atx::usize n) {
  std::iota(scratch.order.begin(), scratch.order.begin() + idx_diff(n), atx::usize{0});
  const std::vector<atx::f64> &x = scratch.x;
  std::sort(scratch.order.begin(), scratch.order.begin() + idx_diff(n),
            [&x](atx::usize a, atx::usize b) noexcept {
              if (x[b] < x[a]) {
                return true;
              }
              if (x[a] < x[b]) {
                return false;
              }
              return a < b;
            });
}

// ---------------------------------------------------------------------------
//  decile_spread — §3.12 steps 2-5. Bucket q = floor(p * Q / n) over the ordered
//  positions, equal weight inside each bucket, spread = mean_0 - mean_{Q-1}.
//  Also accumulates this date into the per-horizon bucket table. Requires n >= Q,
//  which makes every bucket non-empty. When `in.aux` is present, each bucket's
//  mean finite aux over its members is also accumulated (never enters the spread).
// ---------------------------------------------------------------------------
[[nodiscard]] atx::f64 decile_spread(const CrossSectionIcInput &in, atx::usize date,
                                     CrossSectionIcScratch &scratch, atx::usize n, atx::usize q_ct,
                                     std::vector<QuantileBucketStat> &buckets) {
  order_by_signal_desc(scratch, n);
  // `buf` holds the forward returns in ORDERED position, so the bucket sums walk
  // contiguous memory instead of chasing `order` twice. The summation order is
  // unchanged — `buf[p] == r[order[p]]` by construction — so no result moves.
  for (atx::usize p = 0; p < n; ++p) {
    scratch.buf[p] = scratch.r[scratch.order[p]];
  }
  atx::f64 top = 0.0;
  atx::f64 bottom = 0.0;
  atx::usize p = 0;
  while (p < n) {
    const atx::usize q = (p * q_ct) / n;
    atx::f64 total = 0.0;
    atx::usize members = 0;
    atx::f64 aux_total = 0.0;
    atx::usize aux_members = 0;
    while (p < n && ((p * q_ct) / n) == q) {
      total += scratch.buf[p];
      ++members;
      if (!in.aux.empty()) {
        const atx::f64 av = in.aux[date * in.instruments + scratch.perm[scratch.order[p]]];
        if (std::isfinite(av)) {
          aux_total += av;
          ++aux_members;
        }
      }
      ++p;
    }
    const atx::f64 mean = total / static_cast<atx::f64>(members);
    buckets[q].mean_forward_return += mean;
    buckets[q].mean_names += static_cast<atx::f64>(members);
    ++buckets[q].n_dates;
    if (aux_members > 0U) {
      buckets[q].mean_aux += aux_total / static_cast<atx::f64>(aux_members);
      ++buckets[q].n_aux_dates;
    }
    if (q == 0U) {
      top = mean;
    }
    if (q == q_ct - 1U) {
      bottom = mean;
    }
  }
  return top - bottom;
}

// ---------------------------------------------------------------------------
//  fill_decile_weights — the GROSS-2.0 book of ruling AR-11: +1/n_top across
//  decile 0 (totalling +1.0) and -1/n_bottom across decile Q-1 (totalling -1.0),
//  zero elsewhere. A COMPLETE turnover of both legs therefore reads
//  `oneway == 2.0`, not 1.0; weights of +/-0.5/n are the exact pre-AR-11 bug and
//  halve every net spread. Returns false when the date emits no deciles.
// ---------------------------------------------------------------------------
[[nodiscard]] bool fill_decile_weights(const CrossSectionIcInput &in,
                                       const CrossSectionIcConfig &cfg, atx::usize t, atx::usize h,
                                       CrossSectionIcScratch &scratch, std::span<atx::f64> w) {
  for (atx::f64 &value : w) {
    value = 0.0;
  }
  const DateGather g = gather_date(in, cfg, t, h, scratch);
  if (g.n_used < cfg.quantiles) {
    return false;
  }
  order_by_signal_desc(scratch, g.n_used);
  const atx::usize n = g.n_used;
  const atx::usize q_ct = cfg.quantiles;
  atx::usize n_top = 0;
  atx::usize n_bottom = 0;
  for (atx::usize p = 0; p < n; ++p) {
    const atx::usize q = (p * q_ct) / n;
    if (q == 0U) {
      ++n_top;
    } else if (q == q_ct - 1U) {
      ++n_bottom;
    }
  }
  const atx::f64 long_leg = 1.0 / static_cast<atx::f64>(n_top);
  const atx::f64 short_leg = 1.0 / static_cast<atx::f64>(n_bottom);
  for (atx::usize p = 0; p < n; ++p) {
    const atx::usize q = (p * q_ct) / n;
    const atx::usize inst = scratch.perm[scratch.order[p]];
    if (q == 0U) {
      w[inst] += long_leg;
    } else if (q == q_ct - 1U) {
      w[inst] -= short_leg;
    }
  }
  return true;
}

// Which per-date value a series is taken from.
enum class SeriesField : atx::u8 { PearsonIc, RankIc, SpreadGross, SpreadNet };

// ---------------------------------------------------------------------------
//  gather_series — compact one per-date field into `dst`, in ascending date
//  order, over the emitted dates of the requested sample. Returns its length.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::usize gather_series(const std::vector<IcDatePoint> &series, SeriesField field,
                                       bool common_only, std::vector<atx::f64> &dst) noexcept {
  atx::usize n = 0;
  for (const IcDatePoint &p : series) {
    if (common_only && p.in_common_sample == 0U) {
      continue;
    }
    const bool is_ic = (field == SeriesField::PearsonIc || field == SeriesField::RankIc);
    if (is_ic ? (p.emitted == 0U) : (p.spread_emitted == 0U)) {
      continue;
    }
    switch (field) {
    case SeriesField::PearsonIc:
      dst[n] = p.pearson_ic;
      break;
    case SeriesField::RankIc:
      dst[n] = p.rank_ic;
      break;
    case SeriesField::SpreadGross:
      dst[n] = p.spread_gross;
      break;
    case SeriesField::SpreadNet:
      dst[n] = p.spread_net;
      break;
    }
    ++n;
  }
  return n;
}

// §3.9's four series statistics, with the explicit n == 0 and n == 1 branches:
// the (n-1) divisor is undefined at n == 1 and 0/0 would leak a NaN that the
// `sd == 0.0` test does not catch (I-11).
struct SeriesStats {
  atx::f64 mean{};
  atx::f64 sd{};
  atx::f64 ir{};
  atx::f64 naive_t{};
};

[[nodiscard]] SeriesStats series_stats(std::span<const atx::f64> s) noexcept {
  SeriesStats out{};
  const atx::usize n = s.size();
  if (n == 0U) {
    return out;
  }
  if (n == 1U) {
    out.mean = s[0];
    return out;
  }
  atx::f64 total = 0.0;
  for (atx::usize k = 0; k < n; ++k) {
    total += s[k];
  }
  out.mean = total / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (atx::usize k = 0; k < n; ++k) {
    const atx::f64 d = s[k] - out.mean;
    ss += d * d;
  }
  out.sd = std::sqrt(ss / static_cast<atx::f64>(n - 1U));
  out.ir = (out.sd == 0.0) ? 0.0 : out.mean / out.sd;
  out.naive_t = out.ir * std::sqrt(static_cast<atx::f64>(n));
  return out;
}

// ---------------------------------------------------------------------------
//  series_reason — §3.10's frozen five-code enum for ONE series, with ruling
//  A-3's lowest-nonzero-wins precedence. The single spelling of the decision
//  that reaches the output: `make_interval` and the I-6 spread gate both call
//  it, so the two cannot drift (M-5).
//
//  `consider(3, …)` short-circuits on `len == 0`, so `n / len` is never
//  evaluated at zero.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::u8 series_reason(atx::usize n, atx::usize len, atx::usize draws,
                                    bool common_prefix_gap) noexcept {
  atx::u8 reason = 0;
  const auto consider = [&reason](atx::u8 code, bool holds) noexcept {
    if (holds && (reason == 0U || code < reason)) {
      reason = code;
    }
  };
  consider(atx::u8{1}, n < 20U);
  consider(atx::u8{2}, common_prefix_gap);
  consider(atx::u8{3}, len == 0U || (n / len) < 10U);
  consider(atx::u8{4}, draws == 0U);
  return reason;
}

// Which statistic a bootstrap draw recomputes.
enum class DrawStatistic : atx::u8 { Mean, InfoRatio };

// ---------------------------------------------------------------------------
//  resample_statistic — one circular-block resample of §3.10 and its statistic.
//
//  `b = ceil(n / L)` start indices are drawn by consecutive `draw_below` calls;
//  block j contributes S[(s_j + k) mod n] for k = 0..L-1; the concatenation is
//  truncated to exactly n elements — which is why the innermost loop stops at
//  `filled == n` and the final block consumes fewer than L elements.
//
//  The resample is never materialized. The information ratio needs §3.9's
//  two-pass mean-then-sd, so the draw's generator is COPIED before the first
//  pass and replayed for the second: `Xoshiro256pp` has value semantics and no
//  global state, so the replay walks the identical index sequence. Fallbacks are
//  counted on the first pass only, so the replay cannot double-count them.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::f64 resample_statistic(atx::core::Xoshiro256pp &rng,
                                          std::span<const atx::f64> s, atx::usize len,
                                          DrawStatistic kind, atx::usize &fallbacks) noexcept {
  const atx::usize n = s.size();
  const atx::core::Xoshiro256pp saved = rng;
  atx::f64 total = 0.0;
  atx::usize filled = 0;
  while (filled < n) {
    const atx::u64 start = detail::draw_below(rng, static_cast<atx::u64>(n), fallbacks);
    for (atx::usize k = 0; k < len && filled < n; ++k, ++filled) {
      total += s[(static_cast<atx::usize>(start) + k) % n];
    }
  }
  const atx::f64 mean = total / static_cast<atx::f64>(n);
  if (kind == DrawStatistic::Mean) {
    return mean;
  }
  atx::core::Xoshiro256pp replay = saved;
  atx::usize ignored = 0;
  atx::f64 ss = 0.0;
  filled = 0;
  while (filled < n) {
    const atx::u64 start = detail::draw_below(replay, static_cast<atx::u64>(n), ignored);
    for (atx::usize k = 0; k < len && filled < n; ++k, ++filled) {
      const atx::f64 d = s[(static_cast<atx::usize>(start) + k) % n] - mean;
      ss += d * d;
    }
  }
  const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(n - 1U));
  return (sd == 0.0) ? 0.0 : mean / sd;
}

// ---------------------------------------------------------------------------
//  draw_percentiles — the §3.10 draw loop and percentile step for one interval,
//  shared by `make_interval` and the public `bootstrap_mean_interval`.
//
//  splitmix64_next advances the key FIRST and returns the mixed value of the advanced
//  state; the RETURN VALUE seeds the generator and the mutated state is discarded
//  (§3.10). `draw_stats` has exactly `draws` elements.
// ---------------------------------------------------------------------------
void draw_percentiles(BootstrapInterval &iv, atx::u64 key, std::span<const atx::f64> s,
                      atx::usize len, DrawStatistic kind, atx::usize draws,
                      std::span<atx::f64> draw_stats) {
  atx::u64 state = key;
  const atx::u64 seed_x = detail::splitmix64_next(state);
  atx::core::Xoshiro256pp rng{seed_x};
  for (atx::usize d = 0; d < draws; ++d) {
    draw_stats[d] = resample_statistic(rng, s, len, kind, iv.modulo_fallbacks);
  }
  std::sort(draw_stats.begin(), draw_stats.begin() + static_cast<std::ptrdiff_t>(draws));
  const std::span<const atx::f64> sorted{draw_stats.data(), draws};
  iv.lo = detail::quantile_sorted_asc(sorted, 0.025);
  iv.hi = detail::quantile_sorted_asc(sorted, 0.975);
}

// ---------------------------------------------------------------------------
//  make_interval — §3.10's percentile interval, or a well-defined unreportable
//  shell. The reason is the LOWEST NONZERO code that applies (ruling A-3); when
//  it is nonzero `lo`/`hi` are left at 0.0 and MUST serialize as null / "".
// ---------------------------------------------------------------------------
[[nodiscard]] BootstrapInterval make_interval(BootstrapStatisticId statistic, DrawStatistic kind,
                                              std::span<const atx::f64> s, atx::usize len,
                                              const CrossSectionIcConfig &cfg,
                                              atx::usize horizon_index, atx::u64 sample_id,
                                              bool common_prefix_gap, atx::f64 point,
                                              std::vector<atx::f64> &draw_stats) {
  BootstrapInterval iv{};
  const atx::usize n = s.size();
  iv.point = point;
  iv.draws = cfg.bootstrap_draws;
  iv.block_len = len;
  iv.blocks = (len == 0U || n == 0U) ? 0U : ((n + len - 1U) / len);
  iv.series_len = n;

  const atx::u8 reason = series_reason(n, len, cfg.bootstrap_draws, common_prefix_gap);
  iv.unreportable_reason = reason;
  if (reason != 0U) {
    return iv;
  }
  iv.reportable = atx::u8{1};

  const atx::u64 key = detail::bootstrap_stream_key(
      statistic, static_cast<atx::u64>(horizon_index), cfg.stream_signal_index,
      cfg.stream_variant_id, cfg.stream_restriction_id, sample_id, cfg.bootstrap_seed);
  draw_percentiles(iv, key, s, len, kind, cfg.bootstrap_draws,
                   std::span<atx::f64>{draw_stats.data(), cfg.bootstrap_draws});
  return iv;
}

// ---------------------------------------------------------------------------
//  make_hac — the W0-E0a HAC inference of one IC-family mean (E-03).
//
//  The lag covers the MA(h-1) overlap of an h-day forward return and never drops below
//  the Newey-West rule of thumb (so h = 1 still gets a serial-correlation allowance):
//    HansenHodrickV1  uniform kernel, L = max(h - 1, floor(4 (n/100)^(2/9)))
//    NeweyWestV1      Bartlett kernel, L = max(h - 1, Newey-West 1994 automatic lag)
//  No small-sample correction (statsmodels' OLS default). On a common-prefix gap the
//  block is void, exactly like the summary means (§3.13 / NEW-1).
// ---------------------------------------------------------------------------
[[nodiscard]] HacInterval make_hac(std::span<const atx::f64> s, IcHacRule rule,
                                   atx::usize horizon, bool common_prefix_gap) noexcept {
  HacInterval out{};
  out.n = s.size();
  const atx::usize overlap = horizon - 1U; // horizon >= 1 is validated
  hac::Kernel kernel = hac::Kernel::UniformV1;
  atx::usize lag = overlap;
  if (rule == IcHacRule::NeweyWestV1) {
    kernel = hac::Kernel::BartlettV1;
    const atx::usize auto_lag = hac::newey_west_auto_lag(s);
    lag = (auto_lag > overlap) ? auto_lag : overlap;
  } else {
    const atx::usize thumb = hac::newey_west_rule_of_thumb_lag(out.n);
    lag = (thumb > overlap) ? thumb : overlap;
  }
  out.kernel = kernel;
  out.lag = lag;
  if (common_prefix_gap) {
    out.unreportable_reason = (out.n < 20U) ? atx::u8{1} : atx::u8{2};
    return out;
  }
  const hac::MeanInference mi = hac::mean_inference(s, kernel, lag, false);
  out.point = mi.mean;
  out.kernel = mi.kernel;
  out.fell_back = mi.fell_back;
  if (out.n >= 2U) {
    out.lag = mi.lag;
  }
  atx::u8 reason = 0;
  if (out.n < 20U) {
    reason = atx::u8{1};
  } else if ((out.n / (out.lag + 1U)) < 10U) {
    reason = atx::u8{3};
  } else if (mi.defined == 0U) {
    reason = atx::u8{5};
  }
  out.unreportable_reason = reason;
  if (reason != 0U) {
    return out;
  }
  out.se = mi.se;
  out.t = mi.t;
  out.lo = mi.mean - kHacZ975 * mi.se;
  out.hi = mi.mean + kHacZ975 * mi.se;
  out.reportable = atx::u8{1};
  return out;
}

// ---------------------------------------------------------------------------
//  fill_sample — one (horizon, sample) block: §3.9 summaries for the IC, rank-IC
//  and spread families plus their six §3.10 intervals.
//
//  §3.13 / NEW-1: on the COMMON block a single prefix gap voids the ENTIRE block
//  — means included. A point estimate over an incomplete prefix is exactly the
//  number a reader would mistake for a common-sample result, so the statistics
//  are zeroed here and the serializer emits null / "". The full block for the
//  same horizon is untouched.
// ---------------------------------------------------------------------------
void fill_sample(IcSampleStats &s, bool common, atx::usize gaps,
                 const std::vector<IcDatePoint> &series, atx::usize block,
                 const CrossSectionIcConfig &cfg, atx::usize horizon_index, atx::usize horizon,
                 CrossSectionIcScratch &scratch, atx::u8 &spread_reason_out) {
  s = IcSampleStats{};
  s.common_prefix_gaps = common ? gaps : 0U;
  const bool gap = common && (gaps > 0U);
  // Ruling R-C: the common block draws from its OWN stream. Without the sample
  // field the two blocks of one configuration would share a stream and produce
  // maximally correlated intervals.
  const atx::u64 sample_id = common ? atx::u64{1} : cfg.stream_sample_id;

  // --- IC family ------------------------------------------------------------
  atx::usize n = gather_series(series, SeriesField::PearsonIc, common, scratch.draw);
  s.dates_emitted = n;
  atx::u8 reason = 0;
  if (n < 2U) {
    reason = atx::u8{1}; // §3.9's degenerate end of the same short-series condition
  }
  if (gap && (reason == 0U || reason > 2U)) {
    reason = atx::u8{2};
  }
  s.summary_reportable = (reason == 0U) ? atx::u8{1} : atx::u8{0};
  s.unreportable_reason = reason;

  {
    const std::span<const atx::f64> ic{scratch.draw.data(), n};
    const SeriesStats st = gap ? SeriesStats{} : series_stats(ic);
    s.ic_mean = st.mean;
    s.ic_sd = st.sd;
    s.icir = st.ir;
    s.naive_t = st.naive_t;
    s.ic_mean_hac = make_hac(ic, cfg.hac_rule, horizon, gap);
    s.ic_mean_ci = make_interval(BootstrapStatisticId::IcMean, DrawStatistic::Mean, ic, block, cfg,
                                 horizon_index, sample_id, gap, st.mean, scratch.stat);
    s.icir_ci = make_interval(BootstrapStatisticId::Icir, DrawStatistic::InfoRatio, ic, block, cfg,
                              horizon_index, sample_id, gap, st.ir, scratch.stat);
  }
  {
    n = gather_series(series, SeriesField::RankIc, common, scratch.draw);
    const std::span<const atx::f64> rk{scratch.draw.data(), n};
    const SeriesStats st = gap ? SeriesStats{} : series_stats(rk);
    s.rank_ic_mean = st.mean;
    s.rank_ic_sd = st.sd;
    s.rank_icir = st.ir;
    s.rank_naive_t = st.naive_t;
    s.rank_ic_mean_hac = make_hac(rk, cfg.hac_rule, horizon, gap);
    s.rank_ic_mean_ci = make_interval(BootstrapStatisticId::RankIcMean, DrawStatistic::Mean, rk,
                                      block, cfg, horizon_index, sample_id, gap, st.mean,
                                      scratch.stat);
    s.rank_icir_ci = make_interval(BootstrapStatisticId::RankIcir, DrawStatistic::InfoRatio, rk,
                                   block, cfg, horizon_index, sample_id, gap, st.ir, scratch.stat);
  }
  // --- spread families, gated on the SPREAD series' own length (ruling I-6) --
  // The spread emits on `n >= quantiles`, a strictly smaller date set than the
  // IC family's `n >= min_names_per_date`, so `summary_reportable` says nothing
  // about it. Its own reason is computed once here and returned, so the caller
  // can publish it beside the IC one.
  {
    const atx::usize n_spread = gather_series(series, SeriesField::SpreadGross, common,
                                              scratch.draw);
    spread_reason_out = series_reason(n_spread, block, cfg.bootstrap_draws, gap);
    const bool spread_ok = (spread_reason_out == 0U);
    const std::span<const atx::f64> sg{scratch.draw.data(), n_spread};
    const SeriesStats st = spread_ok ? series_stats(sg) : SeriesStats{};
    s.spread_gross_mean = st.mean;
    s.spread_gross_sd = st.sd;
    s.spread_gross_ci = make_interval(BootstrapStatisticId::SpreadGross, DrawStatistic::Mean, sg,
                                      block, cfg, horizon_index, sample_id, gap, st.mean,
                                      scratch.stat);
    n = gather_series(series, SeriesField::SpreadNet, common, scratch.draw);
    const std::span<const atx::f64> sn{scratch.draw.data(), n};
    const SeriesStats net = spread_ok ? series_stats(sn) : SeriesStats{};
    s.spread_net_mean = net.mean;
    s.spread_net_sd = net.sd;
    s.spread_net_ci = make_interval(BootstrapStatisticId::SpreadNet, DrawStatistic::Mean, sn, block,
                                    cfg, horizon_index, sample_id, gap, net.mean, scratch.stat);
  }
}

} // namespace

// ===========================================================================
//  plan_cross_section_ic
// ===========================================================================
Result<CrossSectionIcScratch> plan_cross_section_ic(const CrossSectionIcInput &in,
                                                    const CrossSectionIcConfig &cfg) {
  ATX_TRY_VOID(validate(in, cfg));

  // Sized ONCE, here. §6.1's budget is
  //   9 * instruments * 8   (x, r, rx, rr, buf, w_prev, w_curr as f64; order, perm as usize)
  // + dates * 8             (draw — one circular-block resample)
  // + bootstrap_draws * 8   (stat — the B draw statistics, sorted in place)
  // = 137,104 bytes at I = 1,661, T = 189, B = 2,000.
  CrossSectionIcScratch scratch;
  scratch.x.assign(in.instruments, 0.0);
  scratch.r.assign(in.instruments, 0.0);
  scratch.rx.assign(in.instruments, 0.0);
  scratch.rr.assign(in.instruments, 0.0);
  scratch.buf.assign(in.instruments, 0.0);
  scratch.order.assign(in.instruments, 0U);
  scratch.perm.assign(in.instruments, 0U);
  scratch.w_prev.assign(in.instruments, 0.0);
  scratch.w_curr.assign(in.instruments, 0.0);
  scratch.draw.assign(in.dates, 0.0);
  scratch.stat.assign(cfg.bootstrap_draws, 0.0);
  return Ok(std::move(scratch));
}

// ===========================================================================
//  preflight_cross_section_ic (E-08)
// ===========================================================================
namespace {

// Overflow-checked u64 arithmetic for the byte budget; false on wrap.
[[nodiscard]] bool mul_ok(atx::u64 a, atx::u64 b, atx::u64 &out) noexcept {
  if (a != 0U && b > (std::numeric_limits<atx::u64>::max)() / a) {
    return false;
  }
  out = a * b;
  return true;
}

[[nodiscard]] bool add_ok(atx::u64 a, atx::u64 b, atx::u64 &out) noexcept {
  if (b > (std::numeric_limits<atx::u64>::max)() - a) {
    return false;
  }
  out = a + b;
  return true;
}

} // namespace

Result<IcSizing> preflight_cross_section_ic(atx::usize dates, atx::usize instruments,
                                            const CrossSectionIcConfig &cfg) {
  if (dates == 0U || instruments == 0U) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: dates and instruments must both be non-zero (got " +
                   std::to_string(dates) + " x " + std::to_string(instruments) + ")");
  }
  // Sanity bounds (RR-1): they keep every loop bound statically obvious; the budget
  // below is the binding limit (E-08).
  if (dates > kMaxIcDates) {
    return Err(ErrorCode::OutOfRange, "cross_section_ic: dates " + std::to_string(dates) +
                                          " exceeds kMaxIcDates " + std::to_string(kMaxIcDates));
  }
  if (instruments > kMaxIcInstruments) {
    return Err(ErrorCode::OutOfRange,
               "cross_section_ic: instruments " + std::to_string(instruments) +
                   " exceeds kMaxIcInstruments " + std::to_string(kMaxIcInstruments));
  }
  // Subsumed by the two bounds on a 64-bit usize, and kept because it is the branch
  // that makes `cells` defined whatever the bounds become.
  if (dates > (std::numeric_limits<atx::usize>::max)() / instruments) {
    return Err(ErrorCode::OutOfRange,
               "cross_section_ic: dates * instruments overflows usize (" +
                   std::to_string(dates) + " x " + std::to_string(instruments) + ")");
  }
  // The two config maxima the byte count below multiplies by. `validate` reaches them
  // only through this call, so a standalone preflight enforces the same bounds.
  if (cfg.horizons.size() > kMaxIcHorizons) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: " + std::to_string(cfg.horizons.size()) +
                   " horizons exceeds kMaxIcHorizons " + std::to_string(kMaxIcHorizons));
  }
  if (cfg.bootstrap_draws > kMaxBootstrapDraws) {
    return Err(ErrorCode::OutOfRange,
               "cross_section_ic: bootstrap_draws " + std::to_string(cfg.bootstrap_draws) +
                   " exceeds kMaxBootstrapDraws " + std::to_string(kMaxBootstrapDraws));
  }
  if (cfg.max_working_bytes == 0U) {
    return Err(ErrorCode::InvalidArgument, "cross_section_ic: max_working_bytes must be > 0");
  }

  IcSizing out{};
  out.cells = dates * instruments;
  const atx::u64 d = dates;
  const atx::u64 inst = instruments;
  bool ok = true;
  // Caller-owned spans: signal, price, raw_price, terminal_value (f64); mask, terminal,
  // terminal_evidenced, excluded_audited (u8); session_keys (i64 per date).
  atx::u64 per_cell = 4U * sizeof(atx::f64) + 4U * sizeof(atx::u8);
  atx::u64 cell_bytes = 0;
  atx::u64 key_bytes = 0;
  ok = ok && mul_ok(static_cast<atx::u64>(out.cells), per_cell, cell_bytes);
  ok = ok && mul_ok(d, sizeof(atx::i64), key_bytes);
  ok = ok && add_ok(cell_bytes, key_bytes, out.input_bytes);
  // Scratch, exactly as plan_cross_section_ic sizes it: seven f64 and two usize vectors
  // of `instruments`, `draw` of `dates` f64 and `stat` of `bootstrap_draws` f64.
  per_cell = 7U * sizeof(atx::f64) + 2U * sizeof(atx::usize);
  atx::u64 inst_bytes = 0;
  atx::u64 draw_bytes = 0;
  atx::u64 stat_bytes = 0;
  ok = ok && mul_ok(inst, per_cell, inst_bytes);
  ok = ok && mul_ok(d, sizeof(atx::f64), draw_bytes);
  ok = ok && mul_ok(static_cast<atx::u64>(cfg.bootstrap_draws), sizeof(atx::f64), stat_bytes);
  ok = ok && add_ok(inst_bytes, draw_bytes, out.scratch_bytes);
  ok = ok && add_ok(out.scratch_bytes, stat_bytes, out.scratch_bytes);
  // Result: per horizon one summary, a series reserved to `dates` points and Q buckets.
  atx::u64 series_bytes = 0;
  atx::u64 bucket_bytes = 0;
  atx::u64 per_horizon = sizeof(IcHorizonSummary);
  ok = ok && mul_ok(d, sizeof(IcDatePoint), series_bytes);
  ok = ok && mul_ok(static_cast<atx::u64>(cfg.quantiles), sizeof(QuantileBucketStat),
                    bucket_bytes);
  ok = ok && add_ok(per_horizon, series_bytes, per_horizon);
  ok = ok && add_ok(per_horizon, bucket_bytes, per_horizon);
  ok = ok && mul_ok(static_cast<atx::u64>(cfg.horizons.size()), per_horizon, out.result_bytes);
  ok = ok && add_ok(out.result_bytes, sizeof(CrossSectionIcResult), out.result_bytes);
  ok = ok && add_ok(out.scratch_bytes, out.result_bytes, out.working_bytes);
  if (!ok) {
    return Err(ErrorCode::OutOfRange, "cross_section_ic: working-set byte count overflows u64");
  }
  if (out.working_bytes > cfg.max_working_bytes) {
    return Err(ErrorCode::OutOfRange,
               "cross_section_ic: working set " + std::to_string(out.working_bytes) +
                   " bytes exceeds max_working_bytes " + std::to_string(cfg.max_working_bytes));
  }
  return Ok(out);
}

// ===========================================================================
//  bootstrap_mean_interval
// ===========================================================================
Result<BootstrapInterval> bootstrap_mean_interval(std::span<const atx::f64> series,
                                                  atx::usize block_len, atx::usize draws,
                                                  atx::u64 stream_key,
                                                  std::span<atx::f64> draw_stats) {
  if (draws > kMaxBootstrapDraws) {
    return Err(ErrorCode::InvalidArgument,
               "bootstrap_mean_interval: draws " + std::to_string(draws) +
                   " exceeds kMaxBootstrapDraws " + std::to_string(kMaxBootstrapDraws));
  }
  if (draw_stats.size() < draws) {
    return Err(ErrorCode::InvalidArgument,
               "bootstrap_mean_interval: draw_stats is smaller than draws");
  }
  BootstrapInterval iv{};
  const atx::usize n = series.size();
  iv.point = series_stats(series).mean;
  iv.draws = draws;
  iv.block_len = block_len;
  iv.blocks = (block_len == 0U || n == 0U) ? 0U : ((n + block_len - 1U) / block_len);
  iv.series_len = n;
  iv.unreportable_reason = series_reason(n, block_len, draws, false);
  if (iv.unreportable_reason != 0U) {
    return Ok(iv);
  }
  iv.reportable = atx::u8{1};
  draw_percentiles(iv, stream_key, series, block_len, DrawStatistic::Mean, draws,
                   draw_stats.first(draws));
  return Ok(iv);
}

// ===========================================================================
//  compute_cross_section_ic
//
//  Every §6.2 branch is re-checked here rather than assumed: the function is
//  then TOTAL in its arguments, which is what makes "pure in (in, cfg)" a
//  statement about every caller and not only about well-planned ones.
// ===========================================================================
Result<CrossSectionIcResult> compute_cross_section_ic(const CrossSectionIcInput &in,
                                                     const CrossSectionIcConfig &cfg,
                                                     CrossSectionIcScratch &scratch) {
  ATX_TRY_VOID(validate(in, cfg));
  ATX_TRY_VOID(check_scratch(in, cfg, scratch));

  CrossSectionIcResult out{};
  out.variant = cfg.forward_variant;
  out.horizons.resize(cfg.horizons.size());

  for (atx::usize hi = 0; hi < cfg.horizons.size(); ++hi) {
    const atx::usize h = cfg.horizons[hi];
    IcHorizonSummary &sum = out.horizons[hi];
    sum.horizon = h;
    sum.execution_delay = cfg.execution_delay;
    sum.embargo = label_embargo(h, cfg.execution_delay);
    sum.block_len_rule = cfg.block_len_rule;
    // PolitisWhiteV2 may lengthen this after the IC series exists (below).
    sum.block_len = detail::block_len_for_rule(cfg.block_len_rule, h, cfg.block_len_floor);

    // The only success-path allocation, and it happens ONCE per horizon, before
    // the date loop: the per-date body below touches scratch only (§6.1).
    sum.buckets.assign(cfg.quantiles, QuantileBucketStat{});
    for (atx::usize q = 0; q < cfg.quantiles; ++q) {
      sum.buckets[q].quantile = q;
    }
    sum.series.clear();
    sum.series.reserve(in.dates);

    // `horizons.back() + execution_delay < dates` is validated, so this cannot
    // underflow and every t below has an entry row t + delay and a forward row
    // t + delay + h (E-09).
    const atx::usize delay = cfg.execution_delay;
    const atx::usize evaluable = in.dates - h - delay;

    for (atx::usize t = 0; t < evaluable; ++t) {
      IcDatePoint p{};
      p.date = t;
      p.session_key = in.session_keys[t];
      p.in_common_sample = (t < cfg.common_sample_dates) ? atx::u8{1} : atx::u8{0};
      // §3.12: ACTUAL calendar days from the panel's own session keys, integer
      // division, over the HOLDING window (entry to exit). Strictly increasing keys
      // are validated, so this is positive.
      p.days_forward =
          (in.session_keys[t + delay + h] - in.session_keys[t + delay]) / kNanosPerDay;
      const atx::f64 year_fraction =
          static_cast<atx::f64>(p.days_forward) / static_cast<atx::f64>(cfg.day_basis);
      p.borrow_drag = (cfg.annual_borrow_bps / 1e4) * year_fraction * cfg.short_leg_gross;

      const DateGather g = gather_date(in, cfg, t, h, scratch);
      p.n_eligible = g.n_eligible;
      p.n_signal_finite = g.n_signal_finite;
      p.n_with_forward = g.n_with_forward;
      // GROSS, never netted against n_terminal_applied (ruling A-1): a
      // terminal-applied cell has no §3.3 forward return by construction.
      p.n_dropped_missing_forward = g.n_signal_finite - g.n_with_forward;
      p.n_terminal_applied = g.n_terminal_applied;
      p.n_terminal_unevidenced = g.n_terminal_unevidenced;
      p.n_excluded_audited = g.n_excluded_audited;
      p.n_used = g.n_used;

      if (g.n_used >= cfg.min_names_per_date) {
        p.emitted = atx::u8{1};
        const std::span<const atx::f64> xs{scratch.x.data(), g.n_used};
        const std::span<const atx::f64> rs{scratch.r.data(), g.n_used};
        p.pearson_ic = detail::pearson(xs, rs);
        // §3.5: ranks are taken over U(t,h) ONLY, never over the full
        // cross-section with missing cells held out afterwards.
        atx::core::stats::rank(xs, std::span<atx::f64>{scratch.rx.data(), g.n_used},
                               std::span<atx::usize>{scratch.order.data(), g.n_used});
        atx::core::stats::rank(rs, std::span<atx::f64>{scratch.rr.data(), g.n_used},
                               std::span<atx::usize>{scratch.order.data(), g.n_used});
        p.rank_ic = detail::pearson(std::span<const atx::f64>{scratch.rx.data(), g.n_used},
                                    std::span<const atx::f64>{scratch.rr.data(), g.n_used});
      } else {
        ++sum.dates_below_min_names; // counted, never silently skipped (§3.4)
      }

      if (g.n_used >= cfg.quantiles) {
        p.spread_emitted = atx::u8{1};
        p.spread_gross = decile_spread(in, t, scratch, g.n_used, cfg.quantiles, sum.buckets);
      } else {
        ++sum.dates_below_quantile_count;
      }
      sum.series.push_back(p);
    }

    // --- §3.12 decile-membership turnover at lag h --------------------------
    // Measured on the actual decile portfolios at the gross-2.0 weights the
    // spread earns, not assumed. Both dates are re-gathered rather than cached:
    // w(t-h) is h dates back, and a ring buffer of h weight vectors is the one
    // thing the §6.1 scratch budget does not have room for.
    {
      atx::f64 oneway_total = 0.0;
      atx::usize oneway_dates = 0;
      for (atx::usize t = h; t < evaluable; ++t) {
        const bool prev_ok =
            fill_decile_weights(in, cfg, t - h, h, scratch, std::span<atx::f64>{scratch.w_prev});
        const bool curr_ok =
            fill_decile_weights(in, cfg, t, h, scratch, std::span<atx::f64>{scratch.w_curr});
        if (!prev_ok || !curr_ok) {
          continue; // emitted iff BOTH t and t-h emit deciles
        }
        atx::f64 l1 = 0.0;
        for (atx::usize i = 0; i < in.instruments; ++i) {
          l1 += std::fabs(scratch.w_curr[i] - scratch.w_prev[i]);
        }
        oneway_total += 0.5 * l1;
        ++oneway_dates;
      }
      sum.decile_one_way_turnover =
          (oneway_dates == 0U) ? 0.0 : oneway_total / static_cast<atx::f64>(oneway_dates);
    }
    // `2 *` converts a one-way turnover to full-L1 traded dollars: a round trip
    // touches both the exited and the entered side (§3.12).
    sum.trade_drag = (cfg.trade_bps / 1e4) * 2.0 * sum.decile_one_way_turnover;

    // --- §3.12 per-date net spread (ruling AR-3: no rebalance grid) ---------
    // The net series has exactly the same length and the same emitted-date set
    // as the gross series, so no net interval is lost that a gross one keeps.
    {
      atx::f64 borrow_total = 0.0;
      atx::f64 cost_total = 0.0;
      atx::f64 days_total = 0.0;
      for (IcDatePoint &p : sum.series) {
        p.cost_drag = sum.trade_drag + p.borrow_drag;
        if (p.spread_emitted != 0U) {
          p.spread_net = p.spread_gross - p.cost_drag;
        }
        borrow_total += p.borrow_drag;
        cost_total += p.cost_drag;
        days_total += static_cast<atx::f64>(p.days_forward);
      }
      const atx::usize rows = sum.series.size();
      if (rows > 0U) {
        const atx::f64 denom = static_cast<atx::f64>(rows);
        sum.mean_borrow_drag = borrow_total / denom;
        sum.mean_cost_drag = cost_total / denom;
        sum.mean_days_forward = days_total / denom;
      }
    }

    for (atx::usize q = 0; q < cfg.quantiles; ++q) {
      QuantileBucketStat &b = sum.buckets[q];
      if (b.n_dates > 0U) {
        const atx::f64 denom = static_cast<atx::f64>(b.n_dates);
        b.mean_forward_return /= denom;
        b.mean_names /= denom;
      }
      if (b.n_aux_dates > 0U) {
        b.mean_aux /= static_cast<atx::f64>(b.n_aux_dates);
      }
    }

    // --- §3.13 common-prefix gaps, ONE counter shared by both families ------
    // A prefix date that emits an IC but not a spread is a gap for the whole
    // block (NEW-1): one counter cannot express a per-family split, and the two
    // families must never be computed over different date sets. A prefix date
    // past the evaluable range is a gap too — it cannot emit at this horizon.
    atx::usize gaps = 0;
    for (atx::usize t = 0; t < cfg.common_sample_dates; ++t) {
      if (t >= evaluable) {
        ++gaps;
        continue;
      }
      const IcDatePoint &p = sum.series[t];
      if (p.emitted == 0U || p.spread_emitted == 0U) {
        ++gaps;
      }
    }

    // Ruling I-6 / design §11.8: the horizon-level spread reportability is the
    // FULL sample's, measured on the spread series' own emitted-date count. The
    // common block gates its own means internally (adding code 2 on a gap).
    // E-02 / BlockLenRule::PolitisWhiteV2: raise L to the Politis-White automatic
    // circular block length of this horizon's full-sample IC series when that is longer
    // than max(floor, 2h). One L per horizon, shared by every family and both samples,
    // so all intervals of a horizon are drawn under the same dependence allowance.
    if (cfg.block_len_rule == BlockLenRule::PolitisWhiteV2) {
      const atx::usize n_ic = gather_series(sum.series, SeriesField::PearsonIc, false,
                                            scratch.draw);
      const hac::BlockLength pw =
          hac::politis_white(std::span<const atx::f64>{scratch.draw.data(), n_ic});
      if (pw.defined != 0U) {
        // circular <= ceil(min(3 sqrt(n), n / 3)) < dates, so the cast is exact.
        const auto pw_len = static_cast<atx::usize>(std::ceil(pw.circular));
        if (pw_len > sum.block_len) {
          sum.block_len = pw_len;
        }
      }
    }

    atx::u8 spread_reason = 0;
    atx::u8 common_spread_reason = 0;
    fill_sample(sum.full, false, 0U, sum.series, sum.block_len, cfg, hi, h, scratch,
                spread_reason);
    fill_sample(sum.common, true, gaps, sum.series, sum.block_len, cfg, hi, h, scratch,
                common_spread_reason);
    sum.spread_unreportable_reason = spread_reason;
    sum.spread_reportable = (spread_reason == 0U) ? atx::u8{1} : atx::u8{0};
  }

  // --- §3.11 signal autocorrelation and the implied-turnover proxy ----------
  // Lag 1, the whole pre-registered lag set. `rho_pearson` is REPORTED ONLY;
  // `rho_rank` alone defines the proxy (ruling AR-5), because the deployed
  // preference is a rank-space object.
  out.autocorr.lag = 1U;
  {
    const bool restrict_ex34 = (cfg.stream_restriction_id != 0U);
    atx::f64 rho_pearson_total = 0.0;
    atx::f64 rho_rank_total = 0.0;
    atx::usize pairs = 0;
    for (atx::usize t = 1; t < in.dates; ++t) {
      atx::usize m = 0;
      for (atx::usize i = 0; i < in.instruments; ++i) {
        const atx::usize prev = (t - 1U) * in.instruments + i;
        const atx::usize curr = t * in.instruments + i;
        if (restrict_ex34 &&
            (in.excluded_audited[prev] != 0U || in.excluded_audited[curr] != 0U)) {
          continue;
        }
        if (in.mask[prev] == 0U || in.mask[curr] == 0U) {
          continue;
        }
        const atx::f64 a = in.signal[prev];
        const atx::f64 b = in.signal[curr];
        if (!std::isfinite(a) || !std::isfinite(b)) {
          continue;
        }
        scratch.x[m] = a;
        scratch.r[m] = b;
        ++m;
      }
      if (m < 2U) {
        continue; // emitted iff |A| >= 2
      }
      const std::span<const atx::f64> lag_prev{scratch.x.data(), m};
      const std::span<const atx::f64> lag_curr{scratch.r.data(), m};
      rho_pearson_total += detail::pearson(lag_prev, lag_curr);
      atx::core::stats::rank(lag_prev, std::span<atx::f64>{scratch.rx.data(), m},
                             std::span<atx::usize>{scratch.order.data(), m});
      atx::core::stats::rank(lag_curr, std::span<atx::f64>{scratch.rr.data(), m},
                             std::span<atx::usize>{scratch.order.data(), m});
      rho_rank_total += detail::pearson(std::span<const atx::f64>{scratch.rx.data(), m},
                                        std::span<const atx::f64>{scratch.rr.data(), m});
      ++pairs;
    }
    out.autocorr.pairs_emitted = pairs;
    if (pairs > 0U) {
      const atx::f64 denom = static_cast<atx::f64>(pairs);
      out.autocorr.rho_pearson = rho_pearson_total / denom;
      out.autocorr.rho_rank = rho_rank_total / denom;
      out.autocorr.implied_one_way_turnover = 1.0 - out.autocorr.rho_rank;
    }
    // pairs == 0 leaves all three at 0.0 rather than publishing `1 - 0 == 1`,
    // which would read as complete turnover measured from no observation at all.
  }

  return Ok(std::move(out));
}

// ===========================================================================
//  apply_calendar_seal
// ===========================================================================
Result<SealReport> apply_calendar_seal(std::span<const atx::i64> session_keys,
                                       const CalendarSeal &seal) {
  if (!strictly_increasing<atx::i64>(session_keys)) {
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: seal session_keys must be strictly increasing");
  }

  SealReport report{};
  report.dates_total = session_keys.size();
  // Counted ALWAYS, including when both are zero: the receipt must be able to
  // state that the gate ran and this input did not trip it, which is the whole
  // "non-vacuous by code, vacuous by data" claim of §5.2.
  atx::usize first_sealed = report.dates_total;
  for (atx::usize t = 0U; t < session_keys.size(); ++t) {
    if (session_keys[t] >= seal.validation_begin_ns) {
      ++report.dates_at_or_after_validation;
    }
    if (session_keys[t] >= seal.sealed_begin_ns) {
      ++report.dates_at_or_after_sealed;
      if (first_sealed == report.dates_total) {
        first_sealed = t;
      }
    }
  }

  // `embargo_len`, `content_address` and `used_reserve_window` stay zero: this
  // function has no Panel, so it never calls eval::reserve_window, and it never
  // calls eval::reserve_lockbox at all — its index-fraction carve would reserve
  // the terminal 20% of the EVALUATED dates, which on a 2013-only panel is 2013
  // and not the 2023-2025 sealed period (ruling R-2).
  switch (seal.policy) {
  case SealPolicy::Unknown:
    return Err(ErrorCode::InvalidArgument,
               "cross_section_ic: seal policy is Unknown; name RejectSealedV1 or MaskSealedV1 "
               "explicitly");
  case SealPolicy::RejectSealedV1:
    if (report.dates_at_or_after_sealed > 0U) {
      return Err(ErrorCode::PermissionDenied,
                 "cross_section_ic: " + std::to_string(report.dates_at_or_after_sealed) +
                     " input observations at or after the sealed boundary");
    }
    report.dates_visible = report.dates_total;
    break;
  case SealPolicy::MaskSealedV1:
    // The CALLER truncates to [0, dates_visible). Not used in checkpoint 14; it
    // exists so the enum has a rejectable alternative rather than a silent one.
    report.dates_visible = first_sealed;
    break;
  }
  return Ok(report);
}

} // namespace atx::engine::eval
