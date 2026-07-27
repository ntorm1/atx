#include "atx/vol/priced_surface_view.hpp"

#include "atx/vol/surface_archive.hpp" // ArchiveV2SurfaceHeader + v2 layout

#include "atx/vol/american.hpp"       // american_price / greeks / delta / vega (free fns)
#include "atx/vol/american_batch.hpp" // ResolvedAmericanPriceBatchRequest, american_price_batch_resolved
#include "atx/vol/c8.hpp"            // C8Params, c8_slice_w
#include "atx/vol/dense_slice.hpp"   // ConvexSliceFit
#include "atx/vol/spline_curve.hpp"  // SplineVolParams
#include "atx/vol/vol_curve.hpp"     // Convex/Spline/Essvi/Svi/C8/LinearVariance curves, VolCurveKind
#include "atx/vol/vol_surface.hpp"   // EssviParams, SviParams, essvi_total_w, svi_total_w
#include "laned_greek_run.hpp"      // WS-P1v: the shared laned analytic-Greek batch driver
#include "term_carry.hpp"            // interpolate_positive_log, coherent_q_eff

#include "atx/core/error.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// PricedSurfaceView reproduces PricedSurface's COLD served path bit-for-bit. Its
// surface-level arithmetic (interp_forward / bracket / surface-w interpolation /
// resolve) is a faithful transcription of priced_surface.cpp + vol_curve.cpp over
// the mapped ATXVSA2 columns; its price/greeks call the identical american_*
// free functions with the identical arguments and the accelerator branch removed
// (the view carries no QueryAccelerator). Any deviation would break the S2
// bit-equality gate — so DO NOT "improve" the math here; mirror the source.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Mirrors priced_surface.cpp: a df within this relative tolerance of exp(-rT) is
// treated as the scalar rate, absorbing serialization/libm roundoff.
constexpr double kFlatDiscountRelativeTolerance = 1.0e-12;

[[nodiscard]] bool discount_matches_scalar_rate(double df, double T, double rate) noexcept {
  if (!(df > 0.0) || !std::isfinite(df) || !(T > 0.0) || !std::isfinite(T)) {
    return false;
  }
  const double expected = std::exp(-rate * T);
  if (!(expected > 0.0) || !std::isfinite(expected)) {
    return false;
  }
  const double scale = std::max(std::abs(df), std::abs(expected));
  return std::abs(df - expected) <= kFlatDiscountRelativeTolerance * scale;
}

[[nodiscard]] bool valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

void poison(AmericanGreeks &greeks) noexcept {
  greeks.delta = greeks.gamma = greeks.vega = greeks.theta = greeks.rho = greeks.vanna =
      greeks.volga = greeks.charm = greeks.price = kNaN;
}

// View instance ids are drawn from a HIGH, disjoint range so they can never
// collide with PricedSurface's (which start at 1 and stay small) — the
// PortfolioPricer retained-cache ABA guard compares ids within one SurfaceSet,
// and disjoint id spaces keep a mixed set unambiguous during the wave-2 cutover.
[[nodiscard]] std::uint64_t allocate_view_instance_id() noexcept {
  static std::atomic<std::uint64_t> next{0x4000'0000'0000'0001ull};
  std::uint64_t candidate = next.load();
  for (;;) {
    if (candidate == std::numeric_limits<std::uint64_t>::max()) {
      std::terminate();
    }
    if (next.compare_exchange_weak(candidate, candidate + 1)) {
      return candidate;
    }
  }
}

template <class Input, class Output>
[[nodiscard]] bool spans_overlap(std::span<Input> input, std::span<Output> output) noexcept {
  if (input.empty() || output.empty()) {
    return false;
  }
  const std::uintptr_t input_begin = reinterpret_cast<std::uintptr_t>(input.data());
  const std::uintptr_t output_begin = reinterpret_cast<std::uintptr_t>(output.data());
  if (input_begin <= output_begin) {
    return (output_begin - input_begin) < input.size_bytes();
  }
  return (input_begin - output_begin) < output.size_bytes();
}

// Typed scalar read out of mapped bytes via memcpy (no strict-aliasing / unaligned
// hazard). Used for POD slice params; the columnar f64/u64 arrays are naturally
// 8-B aligned by the writer, so they are indexed directly (Arrow-style).
template <class T> [[nodiscard]] T load_pod(const std::byte *p) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

constexpr char kSurfaceRecordMagic[8] = {'A', 'T', 'X', 'V', 'S', 'R', '2', '0'};

} // namespace

// ── construction / lifetime ──────────────────────────────────────────────────

PricedSurfaceView::~PricedSurfaceView() = default;

PricedSurfaceView::PricedSurfaceView(PricedSurfaceView &&other) noexcept
    : record_(other.record_), col_kind_(other.col_kind_), col_T_(other.col_T_),
      col_forward_(other.col_forward_), col_qeff_(other.col_qeff_), col_df_(other.col_df_),
      col_borrow_(other.col_borrow_), col_payload_off_(other.col_payload_off_),
      col_node_count_(other.col_node_count_), n_slices_(other.n_slices_), pricing_(other.pricing_),
      term_rates_(other.term_rates_), query_pricing_tier_(other.query_pricing_tier_),
      heavy_curves_(std::move(other.heavy_curves_)),
      instance_id_(std::exchange(other.instance_id_, allocate_view_instance_id())) {}

PricedSurfaceView &PricedSurfaceView::operator=(PricedSurfaceView &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  record_ = other.record_;
  col_kind_ = other.col_kind_;
  col_T_ = other.col_T_;
  col_forward_ = other.col_forward_;
  col_qeff_ = other.col_qeff_;
  col_df_ = other.col_df_;
  col_borrow_ = other.col_borrow_;
  col_payload_off_ = other.col_payload_off_;
  col_node_count_ = other.col_node_count_;
  n_slices_ = other.n_slices_;
  pricing_ = other.pricing_;
  term_rates_ = other.term_rates_;
  query_pricing_tier_ = other.query_pricing_tier_;
  heavy_curves_ = std::move(other.heavy_curves_);
  instance_id_ = std::exchange(other.instance_id_, allocate_view_instance_id());
  return *this;
}

namespace {

// Bounds + natural-alignment check for one column of `count` elements of size
// `elem` at record-relative offset `off` inside a record of `record_size` bytes.
[[nodiscard]] bool column_in_bounds(std::uint64_t off, std::uint64_t elem, std::uint64_t count,
                                    std::uint64_t record_size, std::uint64_t align) noexcept {
  if ((off % align) != 0u) {
    return false;
  }
  if (off > record_size) {
    return false;
  }
  const std::uint64_t bytes = elem * count; // count bounded by n_slices (u32) -> no overflow
  return bytes <= record_size - off;
}

// Is `k[0..n)` a legal `std::lower_bound` key? The LinearVariance node axis is
// searched on the `noexcept` query path (`slice_w`, and `LinearVarianceCurve::w`
// on the owned path) and the bracket it returns is used as `lo = hi - 1`.
//
// The two wing guards there (`k_log <= k[0]`, `k_log >= k[n-1]`) already pin the
// returned index into [1, n-1] for any ORDERED axis: `first` can only reach 0 by
// probing index 0 and finding `k[0] >= k_log`, and can only reach n by probing
// index n-1 and finding `k[n-1] < k_log` — both excluded by the guards. A NaN
// node breaks exactly that: it compares false in every direction, so the guards
// fall through AND lower_bound walks to the front and returns 0, making
// `lo = hi - 1` == SIZE_MAX and `k[lo]`/`w[lo]` an out-of-bounds read.
//
// Ascendance is required NON-STRICTLY: sorted-with-duplicates is exactly
// lower_bound's precondition, `w()` already handles a zero-width bracket
// (`!(span > 0.0)` -> `w[lo]`), and demanding strictness could reject an archive
// a fitter legitimately wrote with two coincident nodes.
[[nodiscard]] bool linear_nodes_searchable(const double *k, std::uint64_t n) noexcept {
  for (std::uint64_t i = 0; i < n; ++i) {
    if (!std::isfinite(k[i]) || (i > 0 && k[i] < k[i - 1])) {
      return false;
    }
  }
  return true;
}

} // namespace

Result<PricedSurfaceView>
PricedSurfaceView::create_over_record(std::span<const std::byte> record) {
  if (record.size() < sizeof(ArchiveV2SurfaceHeader)) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: record smaller than header");
  }
  // The f64/u64 columns are indexed via reinterpret_cast (Arrow mmap idiom), which
  // requires the record base to be >= 8-B aligned (columns are 8-B aligned RELATIVE
  // to it). The archive guarantees this (records are 64-B aligned in-file, backing
  // base is >= 16-B), but a corrupt/hand-rolled file or a misaligned open_borrowed
  // span could break it — reject rather than risk an unaligned/UB typed read (§11.3).
  if ((reinterpret_cast<std::uintptr_t>(record.data()) % kArchiveV2ColumnAlign) != 0) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: record base not 8-B aligned");
  }
  ArchiveV2SurfaceHeader h;
  std::memcpy(&h, record.data(), sizeof h);
  if (std::memcmp(h.magic, kSurfaceRecordMagic, 8) != 0) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: bad record magic");
  }
  if (h.record_size != record.size()) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: record size mismatch");
  }
  if (h.n_slices == 0) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: zero slices");
  }
  const std::uint64_t n = h.n_slices;
  const std::uint64_t rs = record.size();

  // Validate every column's bounds + natural alignment (byte offsets, no fix-up).
  const bool ok =
      column_in_bounds(h.col_kind_off, 1, n, rs, 1) &&
      column_in_bounds(h.col_T_off, 8, n, rs, 8) &&
      column_in_bounds(h.col_forward_off, 8, n, rs, 8) &&
      column_in_bounds(h.col_qeff_off, 8, n, rs, 8) &&
      column_in_bounds(h.col_df_off, 8, n, rs, 8) &&
      column_in_bounds(h.col_borrow_off, 8, n, rs, 8) &&
      // col_nused/col_ndropped are not dereferenced by the view, but reconstruct
      // validates all ten columns; keep the view reconstruct-equivalent so a
      // CRC-unchecked offset past the record extent is rejected here too (SE-P1-3).
      column_in_bounds(h.col_nused_off, 8, n, rs, 8) &&
      column_in_bounds(h.col_ndropped_off, 8, n, rs, 8) &&
      column_in_bounds(h.col_nodecount_off, 4, n, rs, 4) &&
      column_in_bounds(h.col_payload_off_off, 8, n, rs, 8);
  if (!ok) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: column out of bounds / misaligned");
  }

  PricedSurfaceView v;
  v.record_ = record;
  v.n_slices_ = static_cast<std::size_t>(n);
  const std::byte *base = record.data();
  v.col_kind_ = reinterpret_cast<const std::uint8_t *>(base + h.col_kind_off);
  // The f64/u64 columns are 8-B aligned by the writer + the record's 64-B file
  // alignment, so a direct typed view is well-defined (Arrow mmap idiom).
  v.col_T_ = reinterpret_cast<const double *>(base + h.col_T_off);
  v.col_forward_ = reinterpret_cast<const double *>(base + h.col_forward_off);
  v.col_qeff_ = reinterpret_cast<const double *>(base + h.col_qeff_off);
  v.col_df_ = reinterpret_cast<const double *>(base + h.col_df_off);
  v.col_borrow_ = reinterpret_cast<const double *>(base + h.col_borrow_off);
  v.col_node_count_ = reinterpret_cast<const std::uint32_t *>(base + h.col_nodecount_off);
  v.col_payload_off_ = reinterpret_cast<const std::uint64_t *>(base + h.col_payload_off_off);

  // Decode the pricing scalars (mirror ArchivePricingRecord -> PricingContext).
  v.pricing_.S = h.S;
  v.pricing_.r = h.r;
  v.pricing_.now_ts_ns = h.now_ts_ns;
  v.pricing_.uid = h.uid;
  v.pricing_.method = static_cast<AmericanMethod>(h.method);
  v.pricing_.al_opts.n_collocation = h.al_n_collocation;
  v.pricing_.al_opts.n_quadrature = h.al_n_quadrature;
  v.pricing_.al_opts.max_newton_iter = h.al_max_newton_iter;
  v.pricing_.al_opts.n_quad_price = h.al_n_quad_price; // C2 (SE-P1-2); 0 -> tied
  v.pricing_.al_opts.tol = h.al_tol;

  // Semantic validation, mirroring PricedSurface::create (priced_surface.cpp):
  // reject exactly the data the OWNED reconstruct path rejects so the zero-copy
  // view is not a weaker parser than reconstruct (SE-P1-1). Under the lazy-CRC
  // design the view is the de-facto untrusted-input parser, and without these a
  // NaN/non-ascending T drives interp_forward's upper_bound off the column end
  // (a one-element OOB read). O(n) at open, off the hot query path.
  if (!(h.S > 0.0) || !std::isfinite(h.r)) {
    return Err(ErrorCode::ParseError, "PricedSurfaceView: non-positive spot or non-finite rate");
  }
  for (std::size_t i = 0; i < v.n_slices_; ++i) {
    const double Ti = v.col_T_[i];
    if (!(Ti > 0.0) || !std::isfinite(Ti) || !(v.col_forward_[i] > 0.0) ||
        !std::isfinite(v.col_forward_[i]) || !std::isfinite(v.col_qeff_[i])) {
      return Err(ErrorCode::ParseError, "PricedSurfaceView: invalid slice carry context");
    }
    if (i > 0 && !(Ti > v.col_T_[i - 1])) {
      return Err(ErrorCode::ParseError, "PricedSurfaceView: slice T's not strictly ascending");
    }
  }

  // term_rates_ mirrors PricedSurface::create: true iff ANY slice's df departs
  // from exp(-rT) beyond tolerance.
  bool term_rates = false;
  for (std::size_t i = 0; i < v.n_slices_; ++i) {
    if (!discount_matches_scalar_rate(v.col_df_[i], v.col_T_[i], v.pricing_.r)) {
      term_rates = true;
      break;
    }
  }
  v.term_rates_ = term_rates;

  // Validate EVERY slice's payload extent (the view reads parametric params in
  // place, lazily, so their bounds must be checked on open too) and eagerly
  // materialize the two derived-state kinds (ConvexDense / SplineVol) whose
  // concrete evaluators (bit-identical to reconstruct) back slice_w. Parametric-
  // only surfaces keep heavy_curves_ empty -> zero heap.
  bool any_heavy = false;
  for (std::size_t i = 0; i < v.n_slices_; ++i) {
    const auto kind = static_cast<VolCurveKind>(v.col_kind_[i]);
    if (kind == VolCurveKind::ConvexDense || kind == VolCurveKind::SplineVol) {
      any_heavy = true;
      break;
    }
  }
  if (any_heavy) {
    v.heavy_curves_.resize(v.n_slices_);
  }
  // Payload extents must also be MONOTONE and DISJOINT (SE-2.2). Each extent was
  // only ever bounds-checked in isolation, so aliasing every slice onto ONE
  // offset passed every check against the same bytes and let an n-slice record
  // demand n x its own size in node vectors (a ~1 MB record has room for ~19k
  // slice columns, each able to claim ~1 MB of nodes). The writer lays payloads
  // out in slice order, each align_up'd past the previous extent, so requiring
  // `poff >= end(previous)` restores exactly the writer's geometry — and makes
  // the record's size the allocation bound again. Checked BEFORE this slice's
  // own materialization, so at most one slice's allocation precedes a rejection.
  std::uint64_t prev_payload_end = 0;
  for (std::size_t i = 0; i < v.n_slices_; ++i) {
    const auto kind = static_cast<VolCurveKind>(v.col_kind_[i]);
    const std::uint64_t poff = v.col_payload_off_[i];
    const std::uint64_t nc = v.col_node_count_[i];
    if ((poff % kArchiveV2ColumnAlign) != 0u || poff > rs) {
      return Err(ErrorCode::ParseError, "PricedSurfaceView: slice payload misaligned/out of bounds");
    }
    if (poff < prev_payload_end) {
      return Err(ErrorCode::ParseError,
                 "PricedSurfaceView: slice payload extents overlap / not ascending");
    }
    const std::uint64_t avail = rs - poff;
    const std::byte *p = base + poff;
    // Payload bytes this slice's kind claims; `need <= avail` is enforced per
    // case below, so `poff + need <= rs` never overflows.
    std::uint64_t need = 0;
    switch (kind) {
    case VolCurveKind::Essvi:
      need = sizeof(EssviParams);
      if (need > avail) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: essvi payload out of bounds");
      }
      break;
    case VolCurveKind::Svi:
      need = sizeof(SviParams);
      if (need > avail) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: svi payload out of bounds");
      }
      break;
    case VolCurveKind::C8:
      need = sizeof(C8Params);
      if (need > avail) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: c8 payload out of bounds");
      }
      break;
    case VolCurveKind::LinearVariance: {
      need = 2ull * nc * sizeof(double);
      if (nc == 0 || need > avail) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: linear payload out of bounds");
      }
      if (!linear_nodes_searchable(reinterpret_cast<const double *>(p), nc)) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: linear node k's not ascending/finite");
      }
      break;
    }
    case VolCurveKind::ConvexDense: {
      // rmse_price f64 | n_obs u64 | n_active u64 | u[n] f64 | C[n] f64.
      need = 24ull + 2ull * nc * sizeof(double);
      if (nc == 0 || need > avail) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: convex payload out of bounds");
      }
      ConvexSliceFit fit;
      fit.T = v.col_T_[i];
      fit.F = v.col_forward_[i];
      fit.df = v.col_df_[i];
      fit.rmse_price = load_pod<double>(p + 0);
      fit.n_obs = static_cast<std::size_t>(load_pod<std::uint64_t>(p + 8));
      fit.n_active = static_cast<std::size_t>(load_pod<std::uint64_t>(p + 16));
      fit.u.resize(static_cast<std::size_t>(nc));
      fit.C.resize(static_cast<std::size_t>(nc));
      const std::size_t nb = static_cast<std::size_t>(nc) * sizeof(double);
      std::memcpy(fit.u.data(), p + 24, nb);
      std::memcpy(fit.C.data(), p + 24 + nb, nb);
      v.heavy_curves_[i] = std::make_unique<ConvexDenseCurve>(std::move(fit));
      break;
    }
    case VolCurveKind::SplineVol: {
      // atm_vol,z_lo,z_hi f64x3 | n u32 | pad u32 | z[n] f64 | mult[n] f64 |
      // mult_cap f64 | w_offset f64 | viol u32.  mult_cap (served-multiple clamp)
      // and w_offset (calendar-cone additive lift) are LIVE in SplineVolCurve::w();
      // dropping them silently misprices (review C1).
      need = 52ull + 16ull * nc;
      if (need > avail) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: spline payload out of bounds");
      }
      SplineVolParams sp;
      sp.atm_vol = load_pod<double>(p + 0);
      sp.z_lo_valid = load_pod<double>(p + 8);
      sp.z_hi_valid = load_pod<double>(p + 16);
      if (load_pod<std::uint32_t>(p + 24) != static_cast<std::uint32_t>(nc)) {
        return Err(ErrorCode::ParseError, "PricedSurfaceView: spline node count mismatch");
      }
      sp.z.resize(static_cast<std::size_t>(nc));
      sp.mult.resize(static_cast<std::size_t>(nc));
      const std::size_t nb = static_cast<std::size_t>(nc) * sizeof(double);
      std::memcpy(sp.z.data(), p + 32, nb);
      std::memcpy(sp.mult.data(), p + 32 + nb, nb);
      sp.mult_cap = load_pod<double>(p + 32 + 2 * nb);
      sp.w_offset = load_pod<double>(p + 40 + 2 * nb);
      sp.n_butterfly_viol = load_pod<std::uint32_t>(p + 48 + 2 * nb);
      v.heavy_curves_[i] =
          std::make_unique<SplineVolCurve>(std::move(sp), v.col_T_[i], v.col_forward_[i], v.col_df_[i]);
      break;
    }
    default:
      return Err(ErrorCode::ParseError, "PricedSurfaceView: unknown curve kind");
    }
    prev_payload_end = poff + need;
  }

  v.instance_id_ = allocate_view_instance_id();
  return Ok(std::move(v));
}

// ── surface-level math (bit-for-bit transcription of priced_surface/vol_curve) ─

double PricedSurfaceView::slice_rate(std::size_t index) const noexcept {
  // Reproduces PricedSurface::create's per-slice slice_rates_[index] AND
  // interp_forward's `term_rates_ ? slice_rates_[index] : r`.
  if (!term_rates_) {
    return pricing_.r;
  }
  const double df = col_df_[index];
  const double T = col_T_[index];
  if (discount_matches_scalar_rate(df, T, pricing_.r)) {
    return pricing_.r;
  }
  return (T > 0.0 && df > 0.0 && std::isfinite(df)) ? -std::log(df) / T : pricing_.r;
}

PricedSurfaceView::ForwardCarry PricedSurfaceView::interp_forward(double T) const noexcept {
  const std::size_t n = n_slices_;
  const double firstT = col_T_[0];
  const double lastT = col_T_[n - 1];
  if (T <= firstT) {
    const double rate = slice_rate(0u);
    if (T == firstT) {
      return ForwardCarry{col_forward_[0], col_qeff_[0], rate};
    }
    const double forward = pricing_.S * std::exp((rate - col_qeff_[0]) * T);
    return ForwardCarry{forward, col_qeff_[0], rate};
  }
  if (T >= lastT) {
    const double rate = slice_rate(n - 1u);
    if (T == lastT) {
      return ForwardCarry{col_forward_[n - 1], col_qeff_[n - 1], rate};
    }
    const double forward = pricing_.S * std::exp((rate - col_qeff_[n - 1]) * T);
    return ForwardCarry{forward, col_qeff_[n - 1], rate};
  }
  // Interior: hi = first index with col_T_[hi] > T (== the upper_bound the
  // scan/upper_bound in priced_surface.cpp selects). ascending T guaranteed.
  const double *upper = std::upper_bound(col_T_, col_T_ + n, T);
  const std::size_t hi = static_cast<std::size_t>(upper - col_T_);
  const std::size_t lo = hi - 1;
  const double aT = col_T_[lo];
  const double bT = col_T_[hi];
  if (T == aT) {
    return ForwardCarry{col_forward_[lo], col_qeff_[lo], slice_rate(lo)};
  }
  const double span = bT - aT;
  const double alpha = (span > 0.0) ? (T - aT) / span : 0.0;
  const double rate_lo = slice_rate(lo);
  const double rate_hi = slice_rate(hi);
  const double forward = interpolate_positive_log(col_forward_[lo], col_forward_[hi], alpha);
  double rate = pricing_.r;
  if (term_rates_) {
    const double log_df_lo = -rate_lo * aT;
    const double log_df_hi = -rate_hi * bT;
    rate = -(log_df_lo + alpha * (log_df_hi - log_df_lo)) / T;
  }
  const double q_eff = coherent_q_eff(pricing_.S, forward, T, rate);
  return ForwardCarry{forward, q_eff, rate};
}

double PricedSurfaceView::forward_at(double T) const noexcept {
  if (!(T > 0.0) || !std::isfinite(T) || n_slices_ == 0) {
    return 0.0;
  }
  return interp_forward(T).forward;
}

double PricedSurfaceView::q_eff_at(double T) const noexcept {
  if (!(T > 0.0) || !std::isfinite(T) || n_slices_ == 0) {
    return 0.0;
  }
  return interp_forward(T).q_eff;
}

double PricedSurfaceView::rate_at(double T) const noexcept {
  if (!(T > 0.0) || !std::isfinite(T) || n_slices_ == 0) {
    return 0.0;
  }
  return interp_forward(T).rate;
}

namespace {
// Local mirror of CurveSurface::Bracket (which is private).
struct Bracket {
  std::size_t lo{0};
  std::size_t hi{0};
  double upper_weight{0.0};
  [[nodiscard]] bool is_single_slice() const noexcept { return lo == hi; }
};
} // namespace

double PricedSurfaceView::slice_w(std::size_t i, double k_log) const noexcept {
  const auto kind = static_cast<VolCurveKind>(col_kind_[i]);
  const std::byte *p = record_.data() + col_payload_off_[i];
  switch (kind) {
  case VolCurveKind::Essvi:
    return essvi_total_w(load_pod<EssviParams>(p), k_log);
  case VolCurveKind::Svi:
    return svi_total_w(load_pod<SviParams>(p), k_log);
  case VolCurveKind::C8:
    return c8_slice_w(load_pod<C8Params>(p), k_log);
  case VolCurveKind::LinearVariance: {
    // Replicates LinearVarianceCurve::w over the mapped k[]/w[] node arrays.
    const std::uint32_t nc = col_node_count_[i];
    if (nc == 0 || !std::isfinite(k_log)) {
      return kNaN;
    }
    const double *k = reinterpret_cast<const double *>(p);
    const double *w = reinterpret_cast<const double *>(p + static_cast<std::size_t>(nc) * sizeof(double));
    if (k_log <= k[0]) {
      return w[0];
    }
    if (k_log >= k[nc - 1]) {
      return w[nc - 1];
    }
    const double *it = std::lower_bound(k, k + nc, k_log);
    const std::size_t hi = static_cast<std::size_t>(it - k);
    const std::size_t lo = hi - 1;
    const double span = k[hi] - k[lo];
    if (!(span > 0.0)) {
      return w[lo];
    }
    const double a = (k_log - k[lo]) / span;
    return w[lo] + a * (w[hi] - w[lo]);
  }
  case VolCurveKind::ConvexDense:
  case VolCurveKind::SplineVol:
    // Derived-state kinds: evaluated through the eager-materialized concrete curve
    // (bit-identical to reconstruct).
    return heavy_curves_[i] ? heavy_curves_[i]->w(k_log) : kNaN;
  }
  return kNaN;
}

double PricedSurfaceView::surface_w(double k_log, double T) const noexcept {
  // CurveSurface::w(k_log, T) == w(k_log, T, bracket(T)).
  if (n_slices_ == 0 || !(T > 0.0)) {
    return kNaN;
  }
  const std::size_t n = n_slices_;
  // bracket(T) (CurveSurface::bracket).
  Bracket br;
  if (!(T > col_T_[0])) {
    br = Bracket{0, 0, 0.0};
  } else if (T >= col_T_[n - 1]) {
    br = Bracket{n - 1, n - 1, 0.0};
  } else {
    const double *upper = std::lower_bound(col_T_ + 1, col_T_ + n, T);
    const std::size_t hi = static_cast<std::size_t>(upper - col_T_);
    if (*upper == T) {
      br = Bracket{hi, hi, 0.0};
    } else {
      const std::size_t lo = hi - 1;
      const double span = col_T_[hi] - col_T_[lo];
      const double uw = span > 0.0 ? (T - col_T_[lo]) / span : 0.0;
      br = Bracket{lo, hi, uw};
    }
  }
  // CurveSurface::w(k_log, T, resolved).
  const double T_front = col_T_[0];
  if (T < T_front) {
    const double w_front = slice_w(0, k_log);
    return std::isfinite(w_front) ? w_front * (T / T_front) : kNaN;
  }
  const double wlo = slice_w(br.lo, k_log);
  if (br.is_single_slice()) {
    return wlo;
  }
  const double whi = slice_w(br.hi, k_log);
  if (!std::isfinite(wlo) || !std::isfinite(whi)) {
    return kNaN;
  }
  return (1.0 - br.upper_weight) * wlo + br.upper_weight * whi;
}

PricedSurfaceView::ResolvedSurfacePoint
PricedSurfaceView::resolve_with_carry(double K, double T, ForwardCarry fc) const noexcept {
  ResolvedSurfacePoint p;
  p.K = K;
  p.T = T;
  if (!(std::isfinite(K) && (K > 0.0))) {
    return p; // valid == false
  }
  p.forward = fc.forward;
  p.q_eff = fc.q_eff;
  p.rate = fc.rate;
  p.k_log = std::log(K / fc.forward);
  const double w = surface_w(p.k_log, T);
  p.sigma = (w > 0.0 && T > 0.0) ? std::sqrt(w / T) : kNaN; // == CurveSurface::iv
  p.valid = true;
  return p;
}

PricedSurfaceView::ResolvedSurfacePoint PricedSurfaceView::resolve(double K,
                                                                   double T) const noexcept {
  ResolvedSurfacePoint p;
  p.K = K;
  p.T = T;
  if (!valid_query(K, T)) {
    return p;
  }
  return resolve_with_carry(K, T, interp_forward(T));
}

double PricedSurfaceView::iv(double K, double T) const noexcept {
  const ResolvedSurfacePoint p = resolve(K, T);
  return p.valid ? p.sigma : kNaN;
}

double PricedSurfaceView::total_variance(double K, double T) const noexcept {
  const ResolvedSurfacePoint p = resolve(K, T);
  return p.valid ? surface_w(p.k_log, T) : kNaN;
}

// ── price / greeks (cold; no accelerator) ────────────────────────────────────

Result<double> PricedSurfaceView::price_resolved(const ResolvedSurfacePoint &p, Side side) const {
  return american_price(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<AmericanGreeks> PricedSurfaceView::greeks_resolved(const ResolvedSurfacePoint &p, Side side,
                                                          bool analytic, GreekNeeds needs) const {
  if (analytic && pricing_.method == AmericanMethod::AndersenLake) {
    // K4 first-order tier, identical mapping to PricedSurface::greeks_resolved's cold
    // AL lane: a reduced request skips whole boundary solves and zeroes the columns it
    // did not ask for. Default GreekNeeds{} (all true) is the full 5-solve bundle and
    // reproduces the pre-WS-ZC1 maskless call exactly.
    return american_greeks_al(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                              std::optional<AlOpts>{pricing_.al_opts}, needs.vega, needs.rho,
                              needs.charm);
  }
  return american_greeks_fd(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                            std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurfaceView::delta_resolved(const ResolvedSurfacePoint &p, Side side) const {
  return american_delta(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                        std::optional<AlOpts>{pricing_.al_opts});
}

Result<double> PricedSurfaceView::vega_resolved(const ResolvedSurfacePoint &p, Side side) const {
  if (pricing_.method == AmericanMethod::AndersenLake) {
    return american_vega_al(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side,
                            std::optional<AlOpts>{pricing_.al_opts});
  }
  const Result<AmericanGreeks> greeks =
      american_greeks_fd(pricing_.S, p.K, p.T, p.sigma, p.rate, p.q_eff, side, pricing_.method,
                         std::optional<AlOpts>{pricing_.al_opts});
  if (!greeks.has_value()) {
    return Err(greeks.error());
  }
  return Ok(greeks->vega);
}

Result<double> PricedSurfaceView::fair_value(double K, double T, Side side,
                                             QueryExecution /*execution*/) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::fair_value: non-finite or non-positive K/T");
  }
  return price_resolved(p, side);
}

Result<AmericanGreeks> PricedSurfaceView::greeks(double K, double T, Side side,
                                                 QueryExecution /*execution*/) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::greeks: non-finite or non-positive K/T");
  }
  return greeks_resolved(p, side, false);
}

Result<AmericanGreeks> PricedSurfaceView::greeks_analytic(double K, double T, Side side,
                                                          QueryExecution /*execution*/,
                                                          GreekNeeds needs) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::greeks_analytic: non-finite or non-positive K/T");
  }
  return greeks_resolved(p, side, true, needs);
}

Result<double> PricedSurfaceView::delta(double K, double T, Side side,
                                        QueryExecution /*execution*/) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::delta: non-finite or non-positive K/T");
  }
  return delta_resolved(p, side);
}

Result<double> PricedSurfaceView::vega(double K, double T, Side side,
                                       QueryExecution /*execution*/) const {
  const ResolvedSurfacePoint p = resolve(K, T);
  if (!p.valid) {
    return Err(ErrorCode::InvalidArgument, "PricedSurfaceView::vega: non-finite or non-positive K/T");
  }
  return vega_resolved(p, side);
}

PricedSurfaceView::FusedResult
PricedSurfaceView::evaluate_resolved(const ResolvedSurfacePoint &p, Side side, EvalField fields,
                                     bool analytic, GreekNeeds needs) const {
  FusedResult r;
  if (!p.valid) {
    r.iv = kNaN;
    r.price = kNaN;
    poison(r.greeks);
    r.status = Err(ErrorCode::InvalidArgument,
                   "PricedSurfaceView::evaluate: non-finite or non-positive K/T");
    return r;
  }
  r.iv = p.sigma;
  if (has_field(fields, EvalField::Delta)) {
    r.greeks.delta = kNaN;
  }
  if (has_field(fields, EvalField::Vega)) {
    r.greeks.vega = kNaN;
  }
  const bool want_greeks =
      has_field(fields, EvalField::FirstOrder) || has_field(fields, EvalField::SecondOrder);
  if (want_greeks) {
    Result<AmericanGreeks> g = greeks_resolved(p, side, analytic, needs);
    if (!g.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(g.error());
      return r;
    }
    // FIX-3/F3-A: identical stamp semantics to PricedSurface::evaluate_resolved and to
    // the shared laned driver (FIX-2/F2-B c601504; FIX-1 740b040 / 9c3e1d0). The view
    // is the type the archive / SurfaceDb replay path serves, so leaving it on an
    // unconditional Ok would have made a REPLAYED book disagree with the freshly-fit
    // book it is supposed to reproduce, on ISA alone. Guard the REQUESTED set only and
    // normalize an unrequested non-finite slot to its canonical unmaterialized 0.0.
    AmericanGreeks gg = *g;
    detail::normalize_unrequested_greeks(gg, needs);
    r.greeks = gg;
    r.price = gg.price;
    if (!detail::requested_greeks_finite(gg, needs)) {
      r.status = Err(ErrorCode::Internal,
                     "PricedSurfaceView::evaluate: non-finite price or requested Greek");
    }
    return r;
  }
  if (has_field(fields, EvalField::Price)) {
    Result<double> fv = price_resolved(p, side);
    if (!fv.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(fv.error());
      return r;
    }
    r.price = *fv;
  }
  if (has_field(fields, EvalField::Delta)) {
    const Result<double> d = delta_resolved(p, side);
    if (!d.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(d.error());
      return r;
    }
    r.greeks.delta = *d;
  }
  if (has_field(fields, EvalField::Vega)) {
    Result<double> vega_result = vega_resolved(p, side);
    if (!vega_result.has_value()) {
      r.iv = kNaN;
      r.price = kNaN;
      poison(r.greeks);
      r.status = Err(vega_result.error());
      return r;
    }
    r.greeks.vega = *vega_result;
  }
  return r;
}

PricedSurfaceView::FusedResult PricedSurfaceView::evaluate(double K, double T, Side side,
                                                           EvalField fields, bool analytic,
                                                           QueryExecution /*execution*/,
                                                           GreekNeeds needs) const {
  return evaluate_resolved(resolve(K, T), side, fields, analytic, needs);
}

Result<FullGreekSeed> PricedSurfaceView::full_greek_seed(double K, double T, Side side, bool analytic,
                                                         QueryExecution execution) const {
  using EF = EvalField;
  constexpr EF kSeedFields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;
  // WS-ZC1: mirror PricedSurface::full_greek_seed EXACTLY — produce the seed through a
  // ONE-ELEMENT evaluate_batch, not the scalar evaluate().
  //
  // WHY THIS MATTERS. WS-P1 moved PricedSurface's seed onto evaluate_batch so the seed
  // rides the same LANED analytic-Greek kernel the batch uses (the kernels are
  // pack-composition invariant, so a 1-lane pack returns exactly what a 4-lane pack
  // returns). The view was left on the scalar route. That was invisible while the view
  // never backed a priced book — but once the replay path borrows views (WS-ZC1) it
  // meant seeds were solved SCALAR while the book around them was solved LANED, so
  // `seeded == fresh` degraded from bit-identity to the documented ~1e-13 AVX2-vs-scalar
  // delta, which the P&L then amplified to ~1e-8 in the run output. It also cost ~4 extra
  // boundary solves per seed. Routing both types through the same batch seam restores
  // exact owned==borrowed agreement.
  const std::array<double, 1> Ks{K};
  const std::array<double, 1> Ts{T};
  const std::array<Side, 1> sides{side};
  std::array<double, 1> seed_iv{};
  std::array<double, 1> seed_px{};
  std::array<AmericanGreeks, 1> seed_gk{};
  std::array<Status, 1> seed_st{};
  const Status rc = evaluate_batch(Ks, Ts, sides, kSeedFields, analytic,
                                   EvaluationSoA{seed_iv, seed_px, seed_gk, seed_st, {}, {}},
                                   simd::SimdIsa::Auto, execution);
  if (!rc.has_value()) {
    return Err(rc.error());
  }
  if (!seed_st[0].has_value()) {
    return Err(seed_st[0].error());
  }
  return FullGreekSeed{uid(),    K,         T,          side, instance_id_,
                       analytic, execution, seed_iv[0], seed_gk[0]};
}

Status PricedSurfaceView::evaluate_batch(std::span<const double> K, std::span<const double> T,
                                         std::span<const Side> side, EvalField fields, bool analytic,
                                         EvaluationSoA out, simd::SimdIsa resolved_price_isa,
                                         QueryExecution /*execution*/, GreekNeeds needs) const {
  const std::size_t n = K.size();
  if (T.size() != n || side.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::evaluate_batch: K/T/side length mismatch");
  }
  const bool want_greeks =
      has_field(fields, EvalField::FirstOrder) || has_field(fields, EvalField::SecondOrder);
  const bool want_price = has_field(fields, EvalField::Price);
  const bool want_iv = has_field(fields, EvalField::Iv);
  const bool want_delta = has_field(fields, EvalField::Delta);
  const bool want_vega = has_field(fields, EvalField::Vega);
  const bool selective_only = !want_greeks && (want_delta || want_vega);
  const auto optional_size_ok = [n](const auto output) noexcept {
    return output.empty() || output.size() == n;
  };
  if (!selective_only && (out.iv.size() != n || out.price.size() != n || out.status.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::evaluate_batch: iv/price/status out-span size != query count");
  }
  if (!optional_size_ok(out.delta) || !optional_size_ok(out.vega) ||
      (want_delta && out.delta.size() != n) || (want_vega && out.vega.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::evaluate_batch: dedicated axis out-span size invalid");
  }
  if (selective_only &&
      (out.status.size() != n || !optional_size_ok(out.iv) || !optional_size_ok(out.price) ||
       !optional_size_ok(out.greeks) || (want_iv && out.iv.size() != n) ||
       (want_price && out.price.size() != n))) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::evaluate_batch: selective out-span size invalid");
  }
  if (want_greeks && out.greeks.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::evaluate_batch: greeks out-span size != query count");
  }
  const auto overlaps_input = [&](const auto output) noexcept {
    return spans_overlap(K, output) || spans_overlap(T, output) || spans_overlap(side, output);
  };
  const bool outputs_overlap =
      spans_overlap(out.iv, out.price) || spans_overlap(out.iv, out.greeks) ||
      spans_overlap(out.iv, out.delta) || spans_overlap(out.iv, out.vega) ||
      spans_overlap(out.iv, out.status) || spans_overlap(out.price, out.greeks) ||
      spans_overlap(out.price, out.delta) || spans_overlap(out.price, out.vega) ||
      spans_overlap(out.price, out.status) || spans_overlap(out.greeks, out.delta) ||
      spans_overlap(out.greeks, out.vega) || spans_overlap(out.greeks, out.status) ||
      spans_overlap(out.delta, out.vega) || spans_overlap(out.delta, out.status) ||
      spans_overlap(out.vega, out.status);
  if (overlaps_input(out.iv) || overlaps_input(out.price) || overlaps_input(out.greeks) ||
      overlaps_input(out.status) || overlaps_input(out.delta) || overlaps_input(out.vega) ||
      outputs_overlap) {
    return Err(ErrorCode::InvalidArgument,
               "PricedSurfaceView::evaluate_batch: query/output spans overlap");
  }

  std::size_t i = 0;
  while (i < n) {
    const double t = T[i];
    std::size_t j = i + 1;
    while (j < n && std::bit_cast<std::uint64_t>(T[j]) == std::bit_cast<std::uint64_t>(t)) {
      ++j;
    }
    const bool t_valid = std::isfinite(t) && (t > 0.0);
    const ForwardCarry fc = t_valid ? interp_forward(t) : ForwardCarry{};
    // The view is always cold (no accelerator), so the price-only resolved-batch
    // fast path is always eligible — matching PricedSurface's default-tier path.
    if (want_price && !want_greeks && !selective_only) {
      if (!t_valid) {
        for (std::size_t e = i; e < j; ++e) {
          ResolvedSurfacePoint p;
          p.K = K[e];
          p.T = t;
          const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic, needs);
          out.iv[e] = fr.iv;
          out.price[e] = fr.price;
          out.status[e] = fr.status;
        }
        i = j;
        continue;
      }

      const auto dispatch_valid = [&](std::size_t begin, std::size_t end) -> Status {
        if (begin == end) {
          return Ok();
        }
        const std::size_t run_size = end - begin;
        const ResolvedAmericanPriceBatchRequest request{
            .S = pricing_.S,
            .T = t,
            .r = fc.rate,
            .q = fc.q_eff,
            .K = K.subspan(begin, run_size),
            .sigma = out.iv.subspan(begin, run_size),
            .side = side.subspan(begin, run_size),
            .method = pricing_.method,
            .al_opts = std::optional<AlOpts>{pricing_.al_opts},
            .isa = resolved_price_isa,
            .price = out.price.subspan(begin, run_size),
            .status = out.status.subspan(begin, run_size),
            .pack_dispatch = {},
        };
        const Status batch = american_price_batch_resolved(request);
        if (!batch.has_value()) {
          return batch;
        }
        for (std::size_t e = begin; e < end; ++e) {
          if (!out.status[e].has_value()) {
            out.iv[e] = kNaN;
          }
        }
        return Ok();
      };

      std::size_t valid_begin = i;
      for (std::size_t e = i; e < j; ++e) {
        // resolve_with_carry uses the shared carry + recomputes surface_w's own
        // bracket (bit-identical to resolve(K, t)).
        const ResolvedSurfacePoint p = resolve_with_carry(K[e], t, fc);
        if (p.valid) {
          out.iv[e] = p.sigma;
          continue;
        }
        const Status batch_status = dispatch_valid(valid_begin, e);
        if (!batch_status.has_value()) {
          return batch_status;
        }
        const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic, needs);
        out.iv[e] = fr.iv;
        out.price[e] = fr.price;
        out.status[e] = fr.status;
        valid_begin = e + 1;
      }
      const Status batch_status = dispatch_valid(valid_begin, j);
      if (!batch_status.has_value()) {
        return batch_status;
      }
      i = j;
      continue;
    }
    // WS-P1v: the laned analytic-Greek route, shared verbatim with
    // PricedSurface::evaluate_batch via detail::laned_greek_run. BEFORE this change the
    // view evaluated Greeks one entry at a time through evaluate_resolved, so a book
    // priced off a mapped `.atxvsa` record paid the scalar per-contract
    // american_greeks_al fan while an identical freshly-fit PricedSurface rode the
    // 4-lane AVX2 kernel. That asymmetry — not any real modelling difference — was why
    // the SurfaceArchiveV2 `expect_batch_bit_identical` golden had to be relaxed on the
    // analytic route by WS-P1a; sharing the driver restores exact surface==view batch
    // agreement and the golden is re-tightened accordingly.
    //
    // The view carries NO QueryAccelerator (it is always the cold reference tier), so the
    // accelerator half of PricedSurface's guard is unconditionally satisfied here. It also
    // now carries the SAME GreekNeeds parameter as PricedSurface (WS-ZC1), so a reduced
    // request skips the same boundary solves on both types and the default {} full bundle
    // is exactly what the scalar greeks_resolved() it replaces computed.
    if (detail::laned_greek_route_selected(want_greeks, selective_only, want_delta, want_vega,
                                           analytic, t_valid, pricing_.method,
                                           resolved_price_isa)) {
      detail::laned_greek_run(
          pricing_.S, i, j, side, std::optional<AlOpts>{pricing_.al_opts}, resolved_price_isa,
          needs, out, [&](std::size_t e) { return resolve_with_carry(K[e], t, fc); },
          [&](std::size_t e, Side sd) {
            return evaluate_resolved(resolve_with_carry(K[e], t, fc), sd, fields, analytic, needs);
          });
      i = j;
      continue;
    }
    for (std::size_t e = i; e < j; ++e) {
      ResolvedSurfacePoint p;
      if (t_valid) {
        p = resolve_with_carry(K[e], t, fc);
      } else {
        p.K = K[e];
        p.T = t;
      }
      const FusedResult fr = evaluate_resolved(p, side[e], fields, analytic, needs);
      if (!selective_only || want_iv) {
        out.iv[e] = fr.iv;
      }
      if (!selective_only || want_price) {
        out.price[e] = fr.price;
      }
      if (want_greeks) {
        out.greeks[e] = fr.greeks;
      }
      if (want_delta) {
        out.delta[e] = fr.greeks.delta;
      }
      if (want_vega) {
        out.vega[e] = fr.greeks.vega;
      }
      out.status[e] = fr.status;
    }
    i = j;
  }
  return Ok();
}

Status PricedSurfaceView::set_cold_query_pricing_tier(QueryPricingTier tier) noexcept {
  switch (tier) {
  case QueryPricingTier::LegacyCompatible:
  case QueryPricingTier::ColdReference:
    query_pricing_tier_ = tier;
    return Ok();
  case QueryPricingTier::RepresentativeFast:
  case QueryPricingTier::CarryBank:
    break;
  }
  return Err(ErrorCode::InvalidArgument,
             "PricedSurfaceView::set_cold_query_pricing_tier: a view carries no accelerator "
             "and cannot serve a fast tier");
}

VolCurveKind PricedSurfaceView::kind_at(std::size_t i) const noexcept {
  return static_cast<VolCurveKind>(col_kind_[i]);
}

} // namespace atx::vol
