// ATXVFA typed records: the writer, the zero-copy typed views, the owned
// decoders and FitArchive::validate_records (the record pass of open()).
// Spec: atx-vol/docs/storage/ARCHIVE_FORMAT.md sections 5-8.

#include "atx/vol/api/storage/fit_archive_records.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/c8.hpp"
#include "atx/vol/api/fitting/dense_slice.hpp"
#include "atx/vol/api/fitting/spline_curve.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/fitting/vol_surface.hpp"
#include "storage/surface_archive_payload.hpp" // write_archive_payload (padding-free param blits)

namespace atx::vol {
namespace {

using Type = FitArchiveSectionType;

// ── Pins: library layouts the format depends on ─────────────────────────────

static_assert(std::is_trivially_copyable_v<EssviParams> && sizeof(EssviParams) == kFitCurveEssviWords * 8);
static_assert(std::is_trivially_copyable_v<SviParams> && sizeof(SviParams) == kFitCurveSviWords * 8);
static_assert(std::is_trivially_copyable_v<C8Params> && sizeof(C8Params) == kFitCurveC8Words * 8);
static_assert(offsetof(C8Params, bumps_active) == 168 && sizeof(bool) == 1);
static_assert(offsetof(EssviParams, resid_basis_kind) == 240);

// ExpiryRecord object representation (ARCHIVE_FORMAT.md section 7). The member
// offsets below follow from the declaration order in expiry_record.hpp: 192 + 12
// + 12 coefficient doubles (1536 + 96 + 96), then T_, r_, q_, growth_, df_,
// k_min_, k_max_, s_min_, s_max_, cert_err_ (doubles), cert_pts_,
// cert_refused_rows_ (u32), side_, populated_, american_, has_edge_ (bytes).
static_assert(sizeof(ExpiryRecord) == kFitArchiveExpiryRecordBytes);
static_assert(alignof(ExpiryRecord) == 8);
static_assert(std::is_trivially_copyable_v<ExpiryRecord> && std::is_standard_layout_v<ExpiryRecord>);
constexpr std::size_t kErGrowth = 1752;
constexpr std::size_t kErDf = 1760;
constexpr std::size_t kErScalarsEnd = 1816; // k_min_ .. cert_refused_rows_ end
constexpr std::size_t kErSide = 1816;
constexpr std::size_t kErFlagsEnd = kFitArchiveExpiryRecordValueBytes;

// Enum maxima (stored as their u8 underlying value; range-checked on open).
template <class E> constexpr std::uint8_t ev(E e) noexcept { return static_cast<std::uint8_t>(e); }
constexpr std::uint8_t kMaxFitStatus = ev(QuoteSliceFitStatus::ArbitrageFailure);
constexpr std::uint8_t kMaxDividendModel = ev(QuoteSliceDividendModel::DatedCash);
constexpr std::uint8_t kMaxScheme = ev(DiscreteDivScheme::EscrowedLeisenReimer);
constexpr std::uint8_t kMaxGrade = ev(QuoteSliceGrade::Refused);
constexpr std::uint8_t kMaxCurveKind = ev(VolCurveKind::SplineVol);
constexpr std::uint8_t kMaxQuoteStatus = ev(QuoteSliceQuoteStatus::LowVegaWeight);
constexpr std::uint8_t kMaxEvaluation = ev(QuoteSliceEvaluationStatus::PricingFailure);
constexpr std::uint8_t kMaxRepairOutcome = ev(QuoteSliceRepairOutcome::NotCertified);
constexpr std::uint8_t kMaxTermination = ev(FitTermination::Stalled);
constexpr std::uint8_t kMaxProjection = ev(SliceProjection::Moved);
constexpr std::uint8_t kMaxCarryMode = ev(QuoteSliceCarryMode::ImpliedForward);
constexpr std::uint8_t kMaxCarryFallback = ev(QuoteSliceCarryFallback::SliceRefused);
constexpr std::uint8_t kMaxForwardStatus = ev(ImpliedForwardStatus::Invalid);
constexpr std::uint8_t kMaxSide = ev(Side::Put);
constexpr std::uint8_t kMaxRebuildOrder = ev(SpotLadderRebuildOrder::NearestAtmFirst);
constexpr std::uint8_t kMaxDiffusion = ev(AmericanMarkovDiffusionMethod::VarianceResolvent);
constexpr std::uint8_t kMaxTimeShape = ev(AmericanMarkovFitTimeShape::VarianceTimeKnots);
constexpr std::uint8_t kMaxResidualBasis = ev(ResidualBasisKind::Fengler);
constexpr std::uint8_t kMaxMovedNode = 3;

// ── Small helpers ────────────────────────────────────────────────────────────

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] std::uint64_t bits_of(double v) noexcept { return std::bit_cast<std::uint64_t>(v); }

template <class T> [[nodiscard]] bool zero_bytes(const T &value) noexcept {
  std::array<std::byte, sizeof(T)> raw{};
  std::memcpy(raw.data(), &value, sizeof(T));
  return std::all_of(raw.begin(), raw.end(), [](std::byte b) { return b == std::byte{0}; });
}

template <class T> [[nodiscard]] bool ref_in(FitArchiveArrayRef ref, std::span<const T> pool) noexcept {
  return ref.first <= pool.size() && ref.count <= pool.size() - ref.first;
}

[[nodiscard]] bool string_in(FitArchiveStringRef ref, std::size_t blob) noexcept {
  return ref.offset <= blob && ref.length <= blob - ref.offset;
}

template <class T> [[nodiscard]] std::span<const T> slice_of(std::span<const T> pool, FitArchiveArrayRef ref) noexcept {
  if (!ref_in(ref, pool)) return {};
  return pool.subspan(ref.first, ref.count);
}

[[nodiscard]] std::uint32_t expected_curve_words(std::uint8_t kind, std::uint32_t n) noexcept {
  switch (static_cast<VolCurveKind>(kind)) {
  case VolCurveKind::ConvexDense: return kFitCurveConvexFixedWords + 2u * n;
  case VolCurveKind::Essvi: return kFitCurveEssviWords;
  case VolCurveKind::Svi: return kFitCurveSviWords;
  case VolCurveKind::LinearVariance: return 2u * n;
  case VolCurveKind::C8: return kFitCurveC8Words;
  case VolCurveKind::SplineVol: return kFitCurveSplineFixedWords + 2u * n;
  }
  return std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool kind_has_nodes(std::uint8_t kind) noexcept {
  const auto k = static_cast<VolCurveKind>(kind);
  return k == VolCurveKind::ConvexDense || k == VolCurveKind::LinearVariance ||
         k == VolCurveKind::SplineVol;
}

// QuoteSliceCarryResolution::reason is a `const char *` with static lifetime in
// the library; a decoded reason is interned once per distinct text so it keeps
// that lifetime. Bounded by the (small, fixed) set of reasons the fitter emits.
[[nodiscard]] const char *intern_reason(std::string_view text) {
  static std::mutex mu;
  static std::set<std::string, std::less<>> pool;
  const std::scoped_lock lock(mu);
  auto it = pool.find(text);
  if (it == pool.end()) it = pool.emplace(text).first;
  return it->c_str();
}

// The default-constructed ExpiryRecord's value bytes must match the layout the
// adapter assumes (zero coefficients and scalars, growth_ = df_ = 1.0, side_ =
// Put, flags false). Computed once; a mismatch refuses ExpiryData instead of
// misreading it.
[[nodiscard]] bool expiry_layout_ok() noexcept {
  static const bool ok = [] {
    const ExpiryRecord probe{};
    std::array<std::byte, sizeof(ExpiryRecord)> raw{};
    std::memcpy(raw.data(), &probe, sizeof(ExpiryRecord));
    const auto read_double = [&](std::size_t at) {
      double v = 0.0;
      std::memcpy(&v, raw.data() + at, sizeof v);
      return v;
    };
    for (std::size_t i = 0; i < kErScalarsEnd; ++i) {
      if (i >= kErGrowth && i < kErDf + 8) continue;
      if (raw[i] != std::byte{0}) return false;
    }
    return read_double(kErGrowth) == 1.0 && read_double(kErDf) == 1.0 &&
           raw[kErSide] == std::byte{ev(Side::Put)} && raw[kErSide + 1] == std::byte{0} &&
           raw[kErSide + 2] == std::byte{0} && raw[kErSide + 3] == std::byte{0} &&
           ExpiryRecord{}.side() == Side::Put;
  }();
  return ok;
}

// ── Encoding ─────────────────────────────────────────────────────────────────

[[nodiscard]] FitArchiveRepair encode_repair(const QuoteSliceRepairRecord &r) noexcept {
  FitArchiveRepair o{};
  o.nodes = r.nodes;
  o.material_nodes = r.material_nodes;
  o.unidentified = r.unidentified;
  o.max_price_move = r.max_price_move;
  o.max_vol_move = r.max_vol_move;
  o.max_unquoted_vol_move = r.max_unquoted_vol_move;
  o.outcome = ev(r.outcome);
  return o;
}

[[nodiscard]] QuoteSliceRepairRecord decode_repair(const FitArchiveRepair &o) noexcept {
  QuoteSliceRepairRecord r;
  r.outcome = static_cast<QuoteSliceRepairOutcome>(o.outcome);
  r.nodes = static_cast<std::size_t>(o.nodes);
  r.material_nodes = static_cast<std::size_t>(o.material_nodes);
  r.max_price_move = o.max_price_move;
  r.max_vol_move = o.max_vol_move;
  r.max_unquoted_vol_move = o.max_unquoted_vol_move;
  r.unidentified = static_cast<std::size_t>(o.unidentified);
  return r;
}

[[nodiscard]] FitArchiveAlOpts encode_al(const AlOpts &a) noexcept {
  return FitArchiveAlOpts{a.tol, a.n_collocation, a.n_quadrature, a.n_quad_price, a.max_newton_iter};
}

[[nodiscard]] AlOpts decode_al(const FitArchiveAlOpts &o) noexcept {
  AlOpts a;
  a.n_collocation = o.n_collocation;
  a.n_quadrature = o.n_quadrature;
  a.n_quad_price = o.n_quad_price;
  a.max_newton_iter = o.max_newton_iter;
  a.tol = o.tol;
  return a;
}

[[nodiscard]] FitArchiveDiagnostics encode_diagnostics(const QuoteSliceDiagnostics &d) noexcept {
  FitArchiveDiagnostics o{};
  o.quotes_seen = d.quotes_seen;
  o.iv_attempted = d.iv_attempted;
  o.quotes_admitted = d.quotes_admitted;
  o.distinct_strikes = d.distinct_strikes;
  o.prices_produced = d.prices_produced;
  o.weights_capped = d.weights_capped;
  o.forward = d.forward;
  o.discount = d.discount;
  o.effective_rate = d.effective_rate;
  o.effective_yield = d.effective_yield;
  const FitDiag &f = d.curve_fit;
  o.curve_fit.rmse_vol_vega_weighted = f.rmse_vol_vega_weighted;
  o.curve_fit.max_residual_vol = f.max_residual_vol;
  o.curve_fit.final_grad_norm = f.final_grad_norm.value_or(0.0);
  o.curve_fit.n_quotes_used = f.n_quotes_used;
  o.curve_fit.n_butterfly_viol = f.n_butterfly_viol;
  o.curve_fit.n_calendar_viol = f.n_calendar_viol;
  o.curve_fit.outer_iters = f.outer_iters;
  o.curve_fit.inner_iters_total = f.inner_iters_total;
  o.curve_fit.termination = ev(f.termination);
  o.curve_fit.projection = ev(f.projection);
  o.curve_fit.flags = static_cast<std::uint8_t>((f.reverted_to_seed ? kFitDiagRevertedToSeed : 0u) |
                                                (f.warm_started ? kFitDiagWarmStarted : 0u) |
                                                (f.final_grad_norm ? kFitDiagHasGradNorm : 0u));
  const QuoteSliceChannelDiagnostics &c = d.channel;
  o.channel.channel_quotes = c.channel_quotes;
  o.channel.mid_quotes = c.mid_quotes;
  o.channel.mid_strikes = c.mid_strikes;
  o.channel.violations = c.violations;
  o.channel.iterations = c.iterations;
  o.channel.repaired_nodes = c.repaired_nodes;
  o.channel.max_violation_half_widths = c.max_violation_half_widths;
  o.channel.prior_strength = c.prior_strength;
  o.channel.solve_seconds = c.solve_seconds;
  o.channel.repair_max_price = c.repair_max_price;
  o.channel.repair_max_half_spreads = c.repair_max_half_spreads;
  o.channel.flags = static_cast<std::uint8_t>((c.used ? kChannelUsed : 0u) |
                                              (c.term_prior ? kChannelTermPrior : 0u) |
                                              (c.converged ? kChannelConverged : 0u));
  o.repair = encode_repair(d.repair);
  return o;
}

[[nodiscard]] QuoteSliceDiagnostics decode_diagnostics(const FitArchiveDiagnostics &o) noexcept {
  QuoteSliceDiagnostics d;
  d.quotes_seen = static_cast<std::size_t>(o.quotes_seen);
  d.iv_attempted = static_cast<std::size_t>(o.iv_attempted);
  d.quotes_admitted = static_cast<std::size_t>(o.quotes_admitted);
  d.distinct_strikes = static_cast<std::size_t>(o.distinct_strikes);
  d.prices_produced = static_cast<std::size_t>(o.prices_produced);
  d.weights_capped = static_cast<std::size_t>(o.weights_capped);
  d.forward = o.forward;
  d.discount = o.discount;
  d.effective_rate = o.effective_rate;
  d.effective_yield = o.effective_yield;
  FitDiag &f = d.curve_fit;
  f.rmse_vol_vega_weighted = o.curve_fit.rmse_vol_vega_weighted;
  f.max_residual_vol = o.curve_fit.max_residual_vol;
  f.outer_iters = o.curve_fit.outer_iters;
  f.inner_iters_total = o.curve_fit.inner_iters_total;
  f.n_quotes_used = o.curve_fit.n_quotes_used;
  f.n_butterfly_viol = o.curve_fit.n_butterfly_viol;
  f.n_calendar_viol = o.curve_fit.n_calendar_viol;
  f.termination = static_cast<FitTermination>(o.curve_fit.termination);
  f.projection = static_cast<SliceProjection>(o.curve_fit.projection);
  f.reverted_to_seed = (o.curve_fit.flags & kFitDiagRevertedToSeed) != 0u;
  f.warm_started = (o.curve_fit.flags & kFitDiagWarmStarted) != 0u;
  if ((o.curve_fit.flags & kFitDiagHasGradNorm) != 0u) f.final_grad_norm = o.curve_fit.final_grad_norm;
  QuoteSliceChannelDiagnostics &c = d.channel;
  c.used = (o.channel.flags & kChannelUsed) != 0u;
  c.channel_quotes = static_cast<std::size_t>(o.channel.channel_quotes);
  c.mid_quotes = static_cast<std::size_t>(o.channel.mid_quotes);
  c.mid_strikes = static_cast<std::size_t>(o.channel.mid_strikes);
  c.violations = static_cast<std::size_t>(o.channel.violations);
  c.max_violation_half_widths = o.channel.max_violation_half_widths;
  c.prior_strength = o.channel.prior_strength;
  c.term_prior = (o.channel.flags & kChannelTermPrior) != 0u;
  c.converged = (o.channel.flags & kChannelConverged) != 0u;
  c.iterations = static_cast<std::size_t>(o.channel.iterations);
  c.solve_seconds = o.channel.solve_seconds;
  c.repaired_nodes = static_cast<std::size_t>(o.channel.repaired_nodes);
  c.repair_max_price = o.channel.repair_max_price;
  c.repair_max_half_spreads = o.channel.repair_max_half_spreads;
  d.repair = decode_repair(o.repair);
  return d;
}

[[nodiscard]] FitArchiveCarry encode_carry_fields(const QuoteSliceCarryResolution &r) noexcept {
  FitArchiveCarry o{};
  o.supplied_forward = r.supplied_forward;
  o.supplied_yield = r.supplied_yield;
  o.implied_forward = r.implied_forward;
  o.implied_yield = r.implied_yield;
  o.implied_total_yield = r.implied_total_yield;
  o.parity_forward = r.parity_forward;
  o.forward_lower = r.forward_lower;
  o.forward_upper = r.forward_upper;
  o.dispersion_sd = r.dispersion_sd;
  o.exercise_shift_sd = r.exercise_shift_sd;
  o.atm_total_vol = r.atm_total_vol;
  o.pairs_valid = r.pairs_valid;
  o.pairs_selected = r.pairs_selected;
  o.pairs_retained = r.pairs_retained;
  o.pairs_contaminated = r.pairs_contaminated;
  o.pairs_unidentified = r.pairs_unidentified;
  o.mode = ev(r.mode);
  o.applied = r.applied ? 1u : 0u;
  o.fallback = ev(r.fallback);
  o.status = ev(r.status);
  return o;
}

[[nodiscard]] QuoteSliceCarryResolution decode_carry(const FitArchiveCarry &o, std::string_view reason) {
  QuoteSliceCarryResolution r;
  r.mode = static_cast<QuoteSliceCarryMode>(o.mode);
  r.applied = o.applied != 0u;
  r.fallback = static_cast<QuoteSliceCarryFallback>(o.fallback);
  r.status = static_cast<ImpliedForwardStatus>(o.status);
  r.reason = intern_reason(reason);
  r.supplied_forward = o.supplied_forward;
  r.supplied_yield = o.supplied_yield;
  r.implied_forward = o.implied_forward;
  r.implied_yield = o.implied_yield;
  r.implied_total_yield = o.implied_total_yield;
  r.parity_forward = o.parity_forward;
  r.forward_lower = o.forward_lower;
  r.forward_upper = o.forward_upper;
  r.dispersion_sd = o.dispersion_sd;
  r.exercise_shift_sd = o.exercise_shift_sd;
  r.atm_total_vol = o.atm_total_vol;
  r.pairs_valid = static_cast<std::size_t>(o.pairs_valid);
  r.pairs_selected = static_cast<std::size_t>(o.pairs_selected);
  r.pairs_retained = static_cast<std::size_t>(o.pairs_retained);
  r.pairs_contaminated = static_cast<std::size_t>(o.pairs_contaminated);
  r.pairs_unidentified = static_cast<std::size_t>(o.pairs_unidentified);
  return r;
}

[[nodiscard]] FitArchiveQuoteRow encode_row(const QuoteSliceRow &r) noexcept {
  FitArchiveQuoteRow o{};
  o.market_iv = r.market_iv;
  o.fitted_iv = r.fitted_iv;
  o.fitted_price = r.fitted_price;
  o.spread_vol = r.spread_vol;
  o.weight_w = r.weight_w;
  o.quote_status = ev(r.quote_status);
  o.evaluation_status = ev(r.evaluation_status);
  o.admitted = r.admitted ? 1u : 0u;
  o.weight_capped = r.weight_capped ? 1u : 0u;
  return o;
}

[[nodiscard]] QuoteSliceRow decode_row(const FitArchiveQuoteRow &o) noexcept {
  QuoteSliceRow r;
  r.quote_status = static_cast<QuoteSliceQuoteStatus>(o.quote_status);
  r.evaluation_status = static_cast<QuoteSliceEvaluationStatus>(o.evaluation_status);
  r.admitted = o.admitted != 0u;
  r.market_iv = o.market_iv;
  r.fitted_iv = o.fitted_iv;
  r.fitted_price = o.fitted_price;
  r.spread_vol = o.spread_vol;
  r.weight_w = o.weight_w;
  r.weight_capped = o.weight_capped != 0u;
  return r;
}

[[nodiscard]] FitArchiveSlice encode_slice(const QuoteSurfaceSlice &s) noexcept {
  FitArchiveSlice o{};
  o.calendar_gap = s.calendar_gap;
  o.calendar_gap_strike = s.calendar_gap_strike;
  o.calendar_repair_required = s.calendar_repair_required;
  o.point_outside_fraction = s.point_outside_fraction;
  o.point_outside_quotes = s.point_outside_quotes;
  o.rung_repair = encode_repair(s.rung_repair);
  o.calendar_channel_repair = encode_repair(s.calendar_channel_repair);
  o.grade = ev(s.grade);
  o.point_status = ev(s.point_status);
  o.flags = static_cast<std::uint8_t>(
      (s.channel_refit ? kSliceChannelRefit : 0u) | (s.calendar_retry ? kSliceCalendarRetry : 0u) |
      (s.calendar_repaired ? kSliceCalendarRepaired : 0u) | (s.repair_rung ? kSliceRepairRung : 0u) |
      (s.calendar_channel_repaired ? kSliceCalendarChannelRepaired : 0u) |
      (s.point_gated ? kSlicePointGated : 0u) | (s.point_fallback ? kSlicePointFallback : 0u) |
      (s.calendar_readmitted ? kSliceCalendarReadmitted : 0u));
  o.present = 1u;
  return o;
}

constexpr std::size_t kU32Max = std::numeric_limits<std::uint32_t>::max();

} // namespace

// ── Writer ───────────────────────────────────────────────────────────────────

struct FitArchiveWriter::Impl {
  FitArchiveFileInfo info{};
  std::vector<std::string> surface_symbol, fit_symbol, expiry_symbol, ladder_symbol, markov_symbol;
  std::vector<FitArchiveSurfaceRecord> surfaces;
  std::vector<FitArchiveFitRecord> fits;
  std::vector<FitArchiveExpiryMeta> expiry;
  std::vector<std::byte> expiry_data;
  std::vector<FitArchiveLadderRecord> ladders;
  std::vector<FitArchiveMarkovRecord> markov;
  std::vector<AmericanPriceNode> price_nodes;
  std::vector<FitArchiveQuoteRow> rows;
  std::vector<std::uint8_t> moved;
  std::vector<CashDividend> dividends;
  std::vector<QuoteSliceCarryPoint> carry_points;
  std::vector<std::uint64_t> curve_words;
  std::vector<FitArchiveLadderContract> ladder_contracts;
  std::vector<FitArchiveMarkovInterval> markov_intervals;
  std::vector<AmericanMarkovLocalVolNode> markov_local_vol;
  std::vector<double> markov_words;
  std::string strings;
  std::vector<std::byte> provenance;
  bool overflow{false};

  // Sizes of every staging vector, for all-or-nothing adds.
  struct Mark {
    std::array<std::size_t, 21> n{};
  };
  [[nodiscard]] Mark mark() const noexcept {
    return Mark{{surface_symbol.size(), fit_symbol.size(), expiry_symbol.size(), ladder_symbol.size(),
                 markov_symbol.size(), surfaces.size(), fits.size(), expiry.size(),
                 expiry_data.size(), ladders.size(), markov.size(), price_nodes.size(), rows.size(),
                 moved.size(), dividends.size(), carry_points.size(), curve_words.size(),
                 ladder_contracts.size(), markov_intervals.size(), markov_local_vol.size(),
                 markov_words.size()}};
  }
  void rollback(const Mark &m, std::size_t string_size) {
    surface_symbol.resize(m.n[0]);
    fit_symbol.resize(m.n[1]);
    expiry_symbol.resize(m.n[2]);
    ladder_symbol.resize(m.n[3]);
    markov_symbol.resize(m.n[4]);
    surfaces.resize(m.n[5]);
    fits.resize(m.n[6]);
    expiry.resize(m.n[7]);
    expiry_data.resize(m.n[8]);
    ladders.resize(m.n[9]);
    markov.resize(m.n[10]);
    price_nodes.resize(m.n[11]);
    rows.resize(m.n[12]);
    moved.resize(m.n[13]);
    dividends.resize(m.n[14]);
    carry_points.resize(m.n[15]);
    curve_words.resize(m.n[16]);
    ladder_contracts.resize(m.n[17]);
    markov_intervals.resize(m.n[18]);
    markov_local_vol.resize(m.n[19]);
    markov_words.resize(m.n[20]);
    strings.resize(string_size);
  }

  // Empty strings and arrays are the canonical {0, 0} reference, so a record's
  // bytes do not depend on how much pool data preceded it (copy_from relies on it).
  [[nodiscard]] FitArchiveStringRef put_string(std::string_view s) {
    if (s.empty()) return {};
    if (strings.size() + s.size() > kU32Max) {
      overflow = true;
      return {};
    }
    const FitArchiveStringRef ref{static_cast<std::uint32_t>(strings.size()),
                                  static_cast<std::uint32_t>(s.size())};
    strings.append(s);
    return ref;
  }

  template <class P, class T> [[nodiscard]] FitArchiveArrayRef put(std::vector<P> &pool, std::span<const T> v) {
    if (v.empty()) return {};
    if (pool.size() + v.size() > kU32Max) {
      overflow = true;
      return {};
    }
    const FitArchiveArrayRef ref{static_cast<std::uint32_t>(pool.size()), static_cast<std::uint32_t>(v.size())};
    pool.insert(pool.end(), v.begin(), v.end());
    return ref;
  }

  [[nodiscard]] FitArchiveArrayRef put_words(std::span<const std::uint64_t> v) { return put(curve_words, v); }

  [[nodiscard]] ArchiveOutcome encode_curve(const IVolCurve &c, FitArchiveCurve &o);
  [[nodiscard]] ArchiveOutcome encode_fit(const QuoteSliceFit &fit, FitArchiveFitRecord &r);
  void push_expiry(std::string_view underlier, const FitArchiveKey &key, const ExpiryRecord &rec,
                   std::uint32_t ladder);
};

namespace {

[[nodiscard]] bool symbol_ok(std::string_view s) noexcept {
  return !s.empty() && s.size() <= kFitArchiveSymbolMax;
}

template <class T> void append_native(std::vector<std::uint64_t> &words, const T &native) {
  static_assert(sizeof(T) % 8 == 0);
  std::array<std::byte, sizeof(T)> raw{};
  detail::write_archive_payload(raw.data(), native); // zeroes padding, writes named fields
  const std::size_t at = words.size();
  words.resize(at + sizeof(T) / 8);
  std::memcpy(words.data() + at, raw.data(), sizeof(T));
}

} // namespace

ArchiveOutcome FitArchiveWriter::Impl::encode_curve(const IVolCurve &c, FitArchiveCurve &o) {
  o.present = 1u;
  o.kind = ev(c.kind());
  o.T = c.T();
  o.F = c.F();
  o.df = c.df();
  std::vector<std::uint64_t> w;
  switch (c.kind()) {
  case VolCurveKind::ConvexDense: {
    // SAFETY: kind() identifies the final concrete class (vol_curve.hpp).
    const ConvexSliceFit &f = static_cast<const ConvexDenseCurve &>(c).fit();
    if (f.C.size() != f.u.size() || f.u.size() > kU32Max / 4u) {
      return archive_error(ArchiveStatus::InvalidArgument, Type::CurveWords);
    }
    o.node_count = static_cast<std::uint32_t>(f.u.size());
    w = {bits_of(f.T), bits_of(f.F), bits_of(f.df), bits_of(f.rmse_price), f.n_obs, f.n_active,
         f.qp_iterations, bits_of(f.qp_stationarity), bits_of(f.qp_primal_violation),
         bits_of(f.qp_complementarity), bits_of(f.qp_dual_violation), bits_of(f.effective_lambda),
         bits_of(f.noise_scale)};
    for (const double v : f.u) w.push_back(bits_of(v));
    for (const double v : f.C) w.push_back(bits_of(v));
    break;
  }
  case VolCurveKind::Essvi: append_native(w, static_cast<const EssviCurve &>(c).slice()); break;
  case VolCurveKind::Svi: append_native(w, static_cast<const SviCurve &>(c).slice()); break;
  case VolCurveKind::C8: append_native(w, static_cast<const C8Curve &>(c).slice()); break;
  case VolCurveKind::LinearVariance: {
    const auto &lv = static_cast<const LinearVarianceCurve &>(c);
    if (lv.k_nodes().size() != lv.w_nodes().size() || lv.k_nodes().size() > kU32Max / 4u) {
      return archive_error(ArchiveStatus::InvalidArgument, Type::CurveWords);
    }
    o.node_count = static_cast<std::uint32_t>(lv.k_nodes().size());
    for (const double v : lv.k_nodes()) w.push_back(bits_of(v));
    for (const double v : lv.w_nodes()) w.push_back(bits_of(v));
    break;
  }
  case VolCurveKind::SplineVol: {
    const SplineVolParams &p = static_cast<const SplineVolCurve &>(c).params();
    if (p.z.size() != p.mult.size() || p.z.size() > kU32Max / 4u) {
      return archive_error(ArchiveStatus::InvalidArgument, Type::CurveWords);
    }
    o.node_count = static_cast<std::uint32_t>(p.z.size());
    w = {bits_of(p.atm_vol), bits_of(p.z_lo_valid), bits_of(p.z_hi_valid), bits_of(p.mult_cap),
         bits_of(p.w_offset), p.n_butterfly_viol};
    for (const double v : p.z) w.push_back(bits_of(v));
    for (const double v : p.mult) w.push_back(bits_of(v));
    break;
  }
  }
  if (w.size() != expected_curve_words(o.kind, o.node_count)) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::CurveWords, o.kind);
  }
  o.payload = put_words(w);
  return {};
}

ArchiveOutcome FitArchiveWriter::Impl::encode_fit(const QuoteSliceFit &fit, FitArchiveFitRecord &r) {
  // moved_nodes is stored as the fit holds it, with no tie to `prices`: a slice
  // stripped after a repair (quote_surface.cpp strip) keeps its flags with no
  // prices, and consumers bound-check the index (quote_slice.cpp).
  r.market = FitArchiveMarket{fit.market.spot, fit.market.variance_years, fit.market.calendar_years,
                              fit.market.rate, fit.market.continuous_yield, fit.market.dividend_pv};
  r.diagnostics = encode_diagnostics(fit.diagnostics);
  r.carry = encode_carry_fields(fit.carry_resolution);
  r.carry.reason = put_string(fit.carry_resolution.reason ? fit.carry_resolution.reason : "");
  if (fit.curve) {
    if (ArchiveOutcome ok = encode_curve(*fit.curve, r.curve); !ok) return ok;
  }
  if (fit.prices) {
    const AmericanPriceBounds &b = fit.prices->bounds();
    r.prices.present = 1u;
    r.prices.spot = b.spot;
    r.prices.discount_min = b.discount_min;
    r.prices.discount_max = b.discount_max;
    r.prices.prepaid_min = b.prepaid_min;
    r.prices.prepaid_max = b.prepaid_max;
    r.prices.nodes = put(price_nodes, fit.prices->nodes());
  }
  if (fit.al_opts) {
    r.has_al_opts = 1u;
    r.al_opts = encode_al(*fit.al_opts);
  }
  std::vector<FitArchiveQuoteRow> encoded(fit.rows.size());
  std::transform(fit.rows.begin(), fit.rows.end(), encoded.begin(), encode_row);
  r.rows = put(rows, std::span<const FitArchiveQuoteRow>{encoded});
  r.moved_nodes = put(moved, std::span<const std::uint8_t>{fit.moved_nodes});
  r.dividends = put(dividends, std::span<const CashDividend>{fit.variance_clock_dividends});
  r.carry_curve = put(carry_points, std::span<const QuoteSliceCarryPoint>{fit.carry_curve});
  r.fit_error = put_string(fit.fit_error);
  r.discrete_div_steps = fit.discrete_div_steps;
  r.fit_status = ev(fit.fit_status);
  r.dividend_model = ev(fit.dividend_model);
  r.discrete_div_scheme = ev(fit.discrete_div_scheme);
  return {};
}

void FitArchiveWriter::Impl::push_expiry(std::string_view underlier, const FitArchiveKey &key,
                                         const ExpiryRecord &rec, std::uint32_t ladder) {
  FitArchiveExpiryMeta m{};
  m.key = key;
  m.expiry = rec.expiry();
  m.rate = rec.rate();
  m.carry = rec.carry();
  m.k_min = rec.k_min();
  m.k_max = rec.k_max();
  m.sigma_min = rec.sigma_min();
  m.sigma_max = rec.sigma_max();
  m.forward_factor = rec.forward_factor();
  m.certified_max_abs_error_fwd = rec.certified_max_abs_error_fwd();
  m.certified_points = rec.certified_points();
  m.uncertified_rows = rec.uncertified_rows();
  m.side = ev(rec.side());
  m.populated = rec.populated() ? 1u : 0u;
  m.american = rec.american() ? 1u : 0u;
  m.has_exercise_edge = rec.has_exercise_edge() ? 1u : 0u;
  m.ladder = ladder;
  m.data_index = static_cast<std::uint32_t>(expiry.size());
  expiry.push_back(m);
  expiry_symbol.emplace_back(underlier);
  const std::size_t at = expiry_data.size();
  expiry_data.resize(at + sizeof(ExpiryRecord));
  // Object representation, then the tail padding zeroed (ARCHIVE_FORMAT.md 7).
  std::memcpy(expiry_data.data() + at, &rec, sizeof(ExpiryRecord));
  std::fill(expiry_data.begin() + static_cast<std::ptrdiff_t>(at + kErFlagsEnd),
            expiry_data.begin() + static_cast<std::ptrdiff_t>(at + sizeof(ExpiryRecord)), std::byte{0});
}

FitArchiveWriter::FitArchiveWriter(FitArchiveFileInfo info) : impl_(std::make_unique<Impl>()) {
  impl_->info = info;
}
FitArchiveWriter::~FitArchiveWriter() = default;
FitArchiveWriter::FitArchiveWriter(FitArchiveWriter &&) noexcept = default;
FitArchiveWriter &FitArchiveWriter::operator=(FitArchiveWriter &&) noexcept = default;

std::size_t FitArchiveWriter::surface_count() const noexcept { return impl_->surfaces.size(); }
std::size_t FitArchiveWriter::fit_count() const noexcept { return impl_->fits.size(); }
std::size_t FitArchiveWriter::expiry_count() const noexcept { return impl_->expiry.size(); }
std::size_t FitArchiveWriter::ladder_count() const noexcept { return impl_->ladders.size(); }
std::size_t FitArchiveWriter::markov_count() const noexcept { return impl_->markov.size(); }

void FitArchiveWriter::set_provenance(std::span<const std::byte> bytes) {
  impl_->provenance.assign(bytes.begin(), bytes.end());
}

ArchiveOutcome FitArchiveWriter::add_surface(const FitArchiveSurfaceInput &in) {
  Impl &w = *impl_;
  const std::size_t n = in.slices.size();
  if (!symbol_ok(in.underlier) || in.expiry_days.size() != n || w.fits.size() + n > kU32Max) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::SurfaceRecords);
  }
  std::vector<std::size_t> order(n);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return in.expiry_days[a] < in.expiry_days[b]; });
  for (std::size_t i = 1; i < n; ++i) {
    if (in.expiry_days[order[i - 1]] == in.expiry_days[order[i]]) {
      return archive_error(ArchiveStatus::DuplicateKey, Type::FitRecords, i);
    }
  }
  const Impl::Mark m = w.mark();
  const std::size_t string_size = w.strings.size();
  FitArchiveSurfaceRecord s{};
  s.key.snapshot_ns = in.snapshot_ns;
  s.term_prior_weight = in.config.term_prior_weight;
  s.prior_decay_log_time = in.config.prior_decay_log_time;
  s.prior_quote_halflife = in.config.prior_quote_halflife;
  s.channel_node_weight = in.config.channel_node_weight;
  s.calendar_retry_factor = in.config.calendar_retry_factor;
  s.calendar_repair_half_spreads = in.config.calendar_repair_half_spreads;
  s.grade_b_min_mid_strikes = in.config.grade_b_min_mid_strikes;
  s.policy_digest = in.policy_digest;
  s.profile = w.put_string(in.profile);
  s.policy_source = w.put_string(in.policy_source);
  s.first_fit = static_cast<std::uint32_t>(w.fits.size());
  s.fit_count = static_cast<std::uint32_t>(n);
  const auto surface_index = static_cast<std::uint32_t>(w.surfaces.size());
  for (const std::size_t i : order) {
    const QuoteSurfaceSlice &slice = in.slices[i];
    FitArchiveFitRecord r{};
    r.key.expiry_day = in.expiry_days[i];
    r.key.snapshot_ns = in.snapshot_ns;
    if (ArchiveOutcome ok = w.encode_fit(slice.fit, r); !ok) {
      w.rollback(m, string_size);
      return ok;
    }
    r.slice = encode_slice(slice);
    r.slice.refusal = w.put_string(slice.refusal);
    r.slice.error = w.put_string(slice.error);
    r.slice.point_error = w.put_string(slice.point_error);
    r.surface = surface_index;
    if (r.slice.grade > kMaxGrade) {
      w.rollback(m, string_size);
      return archive_error(ArchiveStatus::InvalidArgument, Type::FitRecords, i);
    }
    ++s.grade_counts[r.slice.grade];
    w.fits.push_back(r);
    w.fit_symbol.emplace_back(in.underlier);
  }
  w.surfaces.push_back(s);
  w.surface_symbol.emplace_back(in.underlier);
  return {};
}

ArchiveOutcome FitArchiveWriter::add_fit(const FitArchiveFitInput &in) {
  Impl &w = *impl_;
  if (!symbol_ok(in.underlier) || in.fit == nullptr || w.fits.size() >= kU32Max) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::FitRecords);
  }
  const Impl::Mark m = w.mark();
  const std::size_t string_size = w.strings.size();
  FitArchiveFitRecord r{};
  r.key.expiry_day = in.expiry_day;
  r.key.snapshot_ns = in.snapshot_ns;
  r.key.sub = in.sub;
  if (ArchiveOutcome ok = w.encode_fit(*in.fit, r); !ok) {
    w.rollback(m, string_size);
    return ok;
  }
  r.surface = kFitArchiveNone;
  w.fits.push_back(r);
  w.fit_symbol.emplace_back(in.underlier);
  return {};
}

ArchiveOutcome FitArchiveWriter::add_expiry_record(const FitArchiveExpiryInput &in) {
  Impl &w = *impl_;
  if (!symbol_ok(in.underlier) || in.record == nullptr || !in.record->populated() ||
      w.expiry.size() >= kU32Max) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::ExpiryMeta);
  }
  FitArchiveKey key{};
  key.expiry_day = in.expiry_day;
  key.snapshot_ns = in.snapshot_ns;
  key.sub = (std::uint32_t{ev(in.record->side())} << 16) | in.ordinal;
  w.push_expiry(in.underlier, key, *in.record, kFitArchiveNone);
  return {};
}

ArchiveOutcome FitArchiveWriter::add_chain_records(const FitArchiveChainInput &in) {
  Impl &w = *impl_;
  if (!symbol_ok(in.underlier) || in.records == nullptr || !in.chain.consistent()) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::Ladders);
  }
  const ExpiryRecordBank &bank = in.records->bank();
  const auto map = in.records->record_of_contract();
  const std::size_t n = in.chain.size();
  if (map.size() != n || (!in.record_expiry_days.empty() && in.record_expiry_days.size() != bank.size()) ||
      w.expiry.size() + bank.size() > kU32Max) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::Ladders);
  }
  for (std::size_t i = 0; i < bank.size(); ++i) {
    if (bank.at(i) == nullptr || !bank.at(i)->populated()) {
      return archive_error(ArchiveStatus::InvalidArgument, Type::ExpiryMeta, i);
    }
  }
  std::vector<FitArchiveLadderContract> contracts(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (map[i] >= bank.size() || ev(in.chain.side[i]) > kMaxSide) {
      return archive_error(ArchiveStatus::InvalidArgument, Type::LadderContracts, i);
    }
    FitArchiveLadderContract &c = contracts[i];
    c.strike = in.chain.strike[i];
    c.expiry = in.chain.expiry[i];
    c.sigma = in.chain.sigma[i];
    c.rate = in.chain.rate[i];
    c.carry = in.chain.carry[i];
    c.record = map[i];
    c.side = ev(in.chain.side[i]);
  }
  FitArchiveLadderRecord l{};
  l.key.expiry_day = in.expiry_day;
  l.key.snapshot_ns = in.snapshot_ns;
  l.key.sub = in.sub;
  l.spot = in.spot;
  l.spot_move = in.record_policy.spot_move;
  l.sigma_lo_mult = in.record_policy.sigma_lo_mult;
  l.sigma_hi_mult = in.record_policy.sigma_hi_mult;
  if (in.record_policy.sample_opts) {
    l.has_sample_opts = 1u;
    l.sample_opts = encode_al(*in.record_policy.sample_opts);
  }
  l.certify = in.record_policy.certify ? 1u : 0u;
  l.build_workers = in.record_policy.build_workers;
  l.window_sigmas = in.ladder_policy.window_sigmas;
  l.window_min = in.ladder_policy.window_min;
  l.window_max = in.ladder_policy.window_max;
  l.max_cert_usd = in.ladder_policy.max_cert_usd;
  l.max_cert_rel = in.ladder_policy.max_cert_rel;
  l.max_rebuilds_per_tick = in.ladder_policy.max_rebuilds_per_tick;
  l.rebuild_on_exit = in.ladder_policy.rebuild_on_exit ? 1u : 0u;
  l.rebuild_order = ev(in.ladder_policy.rebuild_order);
  l.worst_certified_error_fwd = in.records->worst_certified_error_fwd();
  l.first_record = static_cast<std::uint32_t>(w.expiry.size());
  l.record_count = static_cast<std::uint32_t>(bank.size());
  const Impl::Mark m = w.mark();
  l.contracts = w.put(w.ladder_contracts, std::span<const FitArchiveLadderContract>{contracts});
  const auto ladder_index = static_cast<std::uint32_t>(w.ladders.size());
  std::vector<std::pair<std::pair<std::int32_t, std::uint8_t>, std::uint32_t>> ordinals;
  for (std::size_t i = 0; i < bank.size(); ++i) {
    const ExpiryRecord &rec = *bank.at(i);
    const std::int32_t day = in.record_expiry_days.empty() ? in.expiry_day : in.record_expiry_days[i];
    const auto group = std::make_pair(day, ev(rec.side()));
    auto it = std::find_if(ordinals.begin(), ordinals.end(), [&](const auto &e) { return e.first == group; });
    if (it == ordinals.end()) {
      ordinals.emplace_back(group, std::uint32_t{in.ordinal_base});
      it = ordinals.end() - 1;
    }
    if (it->second > 0xFFFFu) {
      w.rollback(m, w.strings.size());
      return archive_error(ArchiveStatus::InvalidArgument, Type::ExpiryMeta, i);
    }
    FitArchiveKey key{};
    key.expiry_day = day;
    key.snapshot_ns = in.snapshot_ns;
    key.sub = (std::uint32_t{group.second} << 16) | it->second++;
    w.push_expiry(in.underlier, key, rec, ladder_index);
  }
  w.ladders.push_back(l);
  w.ladder_symbol.emplace_back(in.underlier);
  return {};
}

ArchiveOutcome FitArchiveWriter::add_markov(const FitArchiveMarkovInput &in) {
  Impl &w = *impl_;
  if (!symbol_ok(in.underlier) || in.spec == nullptr) {
    return archive_error(ArchiveStatus::InvalidArgument, Type::MarkovRecords);
  }
  const AmericanMarkovSpec &spec = *in.spec;
  const AmericanMarkovConfig &c = spec.config;
  const Impl::Mark m = w.mark();
  const std::size_t string_size = w.strings.size();
  FitArchiveMarkovRecord r{};
  r.key.snapshot_ns = in.snapshot_ns;
  r.key.sub = in.sub;
  r.spot = spec.spot;
  r.upper_spot_multiple = c.upper_spot_multiple;
  r.max_calendar_step = c.max_calendar_step;
  r.max_variance_step = c.max_variance_step;
  r.cfl_fraction = c.cfl_fraction;
  r.grid_nodes = c.grid_nodes;
  r.max_states = c.max_states;
  r.max_transitions = c.max_transitions;
  r.max_stored_rows = c.max_stored_rows;
  r.max_stored_volatility_values = c.max_stored_volatility_values;
  r.max_transition_work = c.max_transition_work;
  r.diffusion_method = ev(c.diffusion_method);
  r.state_grid = w.put(w.markov_words, spec.state_grid);
  std::vector<FitArchiveMarkovInterval> intervals(spec.intervals.size());
  for (std::size_t i = 0; i < spec.intervals.size(); ++i) {
    const AmericanMarkovInterval &s = spec.intervals[i];
    intervals[i] = FitArchiveMarkovInterval{s.calendar_end, s.variance_end, s.rate, s.continuous_yield,
                                            s.volatility, s.cash_dividend_at_end,
                                            w.put(w.markov_local_vol, s.local_volatility)};
  }
  r.intervals = w.put(w.markov_intervals, std::span<const FitArchiveMarkovInterval>{intervals});
  if (in.fit != nullptr) {
    r.has_fit = 1u;
    r.time_shape = ev(in.fit->time_shape());
    r.sigmas = w.put(w.markov_words, in.fit->sigmas());
    r.log_state_knots = w.put(w.markov_words, in.fit->log_state_knots());
    r.variance_time_knots = w.put(w.markov_words, in.fit->variance_time_knots());
    r.quote_prices = w.put(w.markov_words, in.fit->quote_prices());
  }
  r.label = w.put_string(in.label);
  if (w.overflow) {
    w.rollback(m, string_size);
    return archive_error(ArchiveStatus::InvalidArgument, Type::MarkovRecords);
  }
  w.markov.push_back(r);
  w.markov_symbol.emplace_back(in.underlier);
  return {};
}

namespace {

[[nodiscard]] bool rebase(std::uint32_t &first, std::size_t base) noexcept {
  if (base + first > kU32Max) return false;
  first = static_cast<std::uint32_t>(base + first);
  return true;
}

// Zero-length references become the canonical {0, 0} (see Impl::put_string).
[[nodiscard]] bool rebase(FitArchiveArrayRef &ref, std::size_t base) noexcept {
  if (ref.count == 0u) {
    ref.first = 0u;
    return true;
  }
  return rebase(ref.first, base);
}

[[nodiscard]] bool rebase(FitArchiveStringRef &ref, std::size_t base) noexcept {
  if (ref.length == 0u) {
    ref.offset = 0u;
    return true;
  }
  return rebase(ref.offset, base);
}

} // namespace

ArchiveOutcome FitArchiveWriter::copy_from(const FitArchive &src) {
  Impl &w = *impl_;
  const Impl::Mark m = w.mark();
  const std::size_t string_base = w.strings.size();
  const auto fail = [&](Type t) {
    w.rollback(m, string_base);
    return archive_error(ArchiveStatus::InvalidArgument, t);
  };
  const std::size_t nodes_base = w.price_nodes.size(), rows_base = w.rows.size(),
                    moved_base = w.moved.size(), div_base = w.dividends.size(),
                    carry_base = w.carry_points.size(), words_base = w.curve_words.size(),
                    contracts_base = w.ladder_contracts.size(), intervals_base = w.markov_intervals.size(),
                    local_base = w.markov_local_vol.size(), mwords_base = w.markov_words.size(),
                    surface_base = w.surfaces.size(), fit_base = w.fits.size(),
                    expiry_base = w.expiry.size(), ladder_base = w.ladders.size();
  const auto blob = src.section_bytes(Type::Strings);
  if (w.strings.size() + blob.size() > kU32Max) return fail(Type::Strings);
  // SAFETY: std::byte -> char view of the string blob (char may alias any object).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  w.strings.append(reinterpret_cast<const char *>(blob.data()), blob.size());
  static_cast<void>(w.put(w.price_nodes, src.view<AmericanPriceNode>(Type::PriceNodes)));
  static_cast<void>(w.put(w.rows, src.view<FitArchiveQuoteRow>(Type::QuoteRows)));
  static_cast<void>(w.put(w.moved, src.view<std::uint8_t>(Type::MovedNodes)));
  static_cast<void>(w.put(w.dividends, src.view<CashDividend>(Type::Dividends)));
  static_cast<void>(w.put(w.carry_points, src.view<QuoteSliceCarryPoint>(Type::CarryPoints)));
  static_cast<void>(w.put(w.curve_words, src.view<std::uint64_t>(Type::CurveWords)));
  static_cast<void>(w.put(w.ladder_contracts, src.view<FitArchiveLadderContract>(Type::LadderContracts)));
  static_cast<void>(w.put(w.markov_local_vol, src.view<AmericanMarkovLocalVolNode>(Type::MarkovLocalVol)));
  static_cast<void>(w.put(w.markov_words, src.view<double>(Type::MarkovWords)));
  if (w.overflow) return fail(Type::Strings);
  const auto str = [&](FitArchiveStringRef &ref) { return rebase(ref, string_base); };
  for (FitArchiveMarkovInterval iv : src.view<FitArchiveMarkovInterval>(Type::MarkovIntervals)) {
    if (!rebase(iv.local_volatility, local_base)) return fail(Type::MarkovIntervals);
    w.markov_intervals.push_back(iv);
  }
  for (FitArchiveSurfaceRecord s : src.surfaces()) {
    const std::string_view name = src.underlier_name(s.key.underlier);
    s.key.underlier = 0;
    if (!str(s.profile) || !str(s.policy_source) || !rebase(s.first_fit, fit_base)) {
      return fail(Type::SurfaceRecords);
    }
    w.surfaces.push_back(s);
    w.surface_symbol.emplace_back(name);
  }
  for (FitArchiveFitRecord f : src.fits()) {
    const std::string_view name = src.underlier_name(f.key.underlier);
    f.key.underlier = 0;
    const bool ok = rebase(f.rows, rows_base) && rebase(f.moved_nodes, moved_base) &&
                    rebase(f.dividends, div_base) && rebase(f.carry_curve, carry_base) &&
                    rebase(f.curve.payload, words_base) && rebase(f.prices.nodes, nodes_base) &&
                    str(f.fit_error) && str(f.carry.reason) && str(f.slice.refusal) &&
                    str(f.slice.error) && str(f.slice.point_error) &&
                    (f.surface == kFitArchiveNone || rebase(f.surface, surface_base));
    if (!ok) return fail(Type::FitRecords);
    w.fits.push_back(f);
    w.fit_symbol.emplace_back(name);
  }
  const auto data = src.section_bytes(Type::ExpiryData);
  w.expiry_data.insert(w.expiry_data.end(), data.begin(), data.end());
  for (FitArchiveExpiryMeta e : src.expiry_meta()) {
    const std::string_view name = src.underlier_name(e.key.underlier);
    e.key.underlier = 0;
    if (!rebase(e.data_index, expiry_base) || (e.ladder != kFitArchiveNone && !rebase(e.ladder, ladder_base))) {
      return fail(Type::ExpiryMeta);
    }
    w.expiry.push_back(e);
    w.expiry_symbol.emplace_back(name);
  }
  for (FitArchiveLadderRecord l : src.ladders()) {
    const std::string_view name = src.underlier_name(l.key.underlier);
    l.key.underlier = 0;
    if (!rebase(l.contracts, contracts_base) || !rebase(l.first_record, expiry_base)) {
      return fail(Type::Ladders);
    }
    w.ladders.push_back(l);
    w.ladder_symbol.emplace_back(name);
  }
  for (FitArchiveMarkovRecord r : src.markov()) {
    const std::string_view name = src.underlier_name(r.key.underlier);
    r.key.underlier = 0;
    const bool ok = rebase(r.intervals, intervals_base) && rebase(r.state_grid, mwords_base) &&
                    rebase(r.sigmas, mwords_base) && rebase(r.log_state_knots, mwords_base) &&
                    rebase(r.variance_time_knots, mwords_base) && rebase(r.quote_prices, mwords_base) &&
                    str(r.label);
    if (!ok) return fail(Type::MarkovRecords);
    w.markov.push_back(r);
    w.markov_symbol.emplace_back(name);
  }
  return {};
}

namespace {

template <class T>
[[nodiscard]] FitArchiveSectionInput section_of(Type type, const std::vector<T> &v, std::uint32_t align,
                                                std::uint32_t flags = kFitArchiveSectionRequired) {
  return FitArchiveSectionInput{type, 1, 0, static_cast<std::uint32_t>(sizeof(T)), align, flags,
                                std::as_bytes(std::span<const T>{v})};
}

// Assign symbol ids and build one keyed record section + its sorted index.
template <class R>
[[nodiscard]] ArchiveOutcome finish_keyed(Type records_type, std::vector<R> &rows,
                                          const std::vector<std::string> &names,
                                          const std::vector<std::string> &symbols,
                                          std::vector<FitArchiveIndexEntry> &index) {
  index.resize(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto it = std::lower_bound(symbols.begin(), symbols.end(), names[i]);
    rows[i].key.underlier = static_cast<std::uint32_t>(it - symbols.begin());
    const FitArchiveKey &k = rows[i].key;
    index[i] = FitArchiveIndexEntry{k.underlier, k.expiry_day, k.snapshot_ns, k.sub,
                                    static_cast<std::uint32_t>(i)};
  }
  std::sort(index.begin(), index.end(), [](const FitArchiveIndexEntry &a, const FitArchiveIndexEntry &b) {
    return fit_archive_key_less(a, b);
  });
  for (std::size_t i = 1; i < index.size(); ++i) {
    if (!fit_archive_key_less(index[i - 1], index[i])) {
      return archive_error(ArchiveStatus::DuplicateKey, records_type, index[i].record);
    }
  }
  return {};
}

[[nodiscard]] Type index_for(Type records) noexcept {
  switch (records) {
  case Type::SurfaceRecords: return Type::SurfaceIndex;
  case Type::FitRecords: return Type::FitIndex;
  case Type::ExpiryMeta: return Type::ExpiryIndex;
  case Type::Ladders: return Type::LadderIndex;
  case Type::MarkovRecords: return Type::MarkovIndex;
  default: return records;
  }
}

} // namespace

ArchiveResult<std::vector<std::byte>> FitArchiveWriter::finish() const { return finish(impl_->info); }

ArchiveResult<std::vector<std::byte>> FitArchiveWriter::finish(const FitArchiveFileInfo &info) const {
  const Impl &w = *impl_;
  if (w.overflow) return archive_error(ArchiveStatus::InvalidArgument, Type::Strings);
  std::vector<std::string> symbols;
  for (const auto *names : {&w.surface_symbol, &w.fit_symbol, &w.expiry_symbol, &w.ladder_symbol,
                            &w.markov_symbol}) {
    symbols.insert(symbols.end(), names->begin(), names->end());
  }
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
  std::vector<FitArchiveSymbol> symbol_rows(symbols.size());
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    symbol_rows[i].length = static_cast<std::uint32_t>(symbols[i].size());
    std::copy(symbols[i].begin(), symbols[i].end(), symbol_rows[i].name.begin());
  }
  auto surfaces = w.surfaces;
  auto fits = w.fits;
  auto expiry = w.expiry;
  auto ladders = w.ladders;
  auto markov = w.markov;
  std::vector<FitArchiveIndexEntry> si, fi, ei, li, mi;
  if (auto ok = finish_keyed(Type::SurfaceRecords, surfaces, w.surface_symbol, symbols, si); !ok) return tl::unexpected(ok.error());
  if (auto ok = finish_keyed(Type::FitRecords, fits, w.fit_symbol, symbols, fi); !ok) return tl::unexpected(ok.error());
  if (auto ok = finish_keyed(Type::ExpiryMeta, expiry, w.expiry_symbol, symbols, ei); !ok) return tl::unexpected(ok.error());
  if (auto ok = finish_keyed(Type::Ladders, ladders, w.ladder_symbol, symbols, li); !ok) return tl::unexpected(ok.error());
  if (auto ok = finish_keyed(Type::MarkovRecords, markov, w.markov_symbol, symbols, mi); !ok) return tl::unexpected(ok.error());

  std::vector<FitArchiveSectionInput> s;
  s.push_back(section_of(Type::Symbols, symbol_rows, 8));
  s.push_back(FitArchiveSectionInput{Type::Strings, 1, 0, 1, 1, kFitArchiveSectionRequired,
                                     std::as_bytes(std::span<const char>{w.strings})});
  if (!w.provenance.empty()) {
    s.push_back(FitArchiveSectionInput{Type::Provenance, 1, 0, 1, 1, 0u, w.provenance});
  }
  const auto keyed = [&](Type t, const auto &rows, const std::vector<FitArchiveIndexEntry> &index) {
    if (rows.empty()) return;
    s.push_back(section_of(t, rows, 8));
    s.push_back(section_of(index_for(t), index, 8));
  };
  const auto pool = [&](Type t, const auto &v, std::uint32_t align) {
    if (!v.empty()) s.push_back(section_of(t, v, align));
  };
  keyed(Type::SurfaceRecords, surfaces, si);
  keyed(Type::FitRecords, fits, fi);
  pool(Type::PriceNodes, w.price_nodes, 8);
  pool(Type::QuoteRows, w.rows, 8);
  pool(Type::MovedNodes, w.moved, 1);
  pool(Type::Dividends, w.dividends, 8);
  pool(Type::CarryPoints, w.carry_points, 8);
  pool(Type::CurveWords, w.curve_words, 8);
  keyed(Type::ExpiryMeta, expiry, ei);
  if (!w.expiry_data.empty()) {
    s.push_back(FitArchiveSectionInput{Type::ExpiryData, 1, 0, kFitArchiveExpiryRecordBytes, 8,
                                       kFitArchiveSectionRequired, w.expiry_data});
  }
  keyed(Type::Ladders, ladders, li);
  pool(Type::LadderContracts, w.ladder_contracts, 8);
  keyed(Type::MarkovRecords, markov, mi);
  pool(Type::MarkovIntervals, w.markov_intervals, 8);
  pool(Type::MarkovLocalVol, w.markov_local_vol, 8);
  pool(Type::MarkovWords, w.markov_words, 8);
  return build_fit_archive(s, info);
}

ArchiveOutcome FitArchiveWriter::write_file(std::string_view path) const {
  auto image = finish();
  if (!image) return tl::unexpected(image.error());
  return publish_fit_archive(path, *image);
}

// ── Typed views ──────────────────────────────────────────────────────────────

std::span<const AmericanPriceNode> price_nodes(const FitArchive &a, const FitArchiveFitRecord &fit) noexcept {
  return slice_of(a.view<AmericanPriceNode>(Type::PriceNodes), fit.prices.nodes);
}

std::span<const CashDividend> fit_dividends(const FitArchive &a, const FitArchiveFitRecord &fit) noexcept {
  return slice_of(a.view<CashDividend>(Type::Dividends), fit.dividends);
}

std::span<const QuoteSliceCarryPoint> fit_carry_curve(const FitArchive &a,
                                                      const FitArchiveFitRecord &fit) noexcept {
  return slice_of(a.view<QuoteSliceCarryPoint>(Type::CarryPoints), fit.carry_curve);
}

std::span<const ExpiryRecord> expiry_records(const FitArchive &a) noexcept {
  return a.view<ExpiryRecord>(Type::ExpiryData);
}

const ExpiryRecord *expiry_record(const FitArchive &a, const FitArchiveExpiryMeta &meta) noexcept {
  const auto records = expiry_records(a);
  return meta.data_index < records.size() ? &records[meta.data_index] : nullptr;
}

std::span<const AmericanMarkovLocalVolNode> markov_local_vol(const FitArchive &a,
                                                             const FitArchiveMarkovInterval &iv) noexcept {
  return slice_of(a.view<AmericanMarkovLocalVolNode>(Type::MarkovLocalVol), iv.local_volatility);
}

// ── Owned decoders ───────────────────────────────────────────────────────────

ArchiveResult<std::unique_ptr<IVolCurve>> decode_curve(const FitArchive &a, const FitArchiveFitRecord &fit) {
  const FitArchiveCurve &c = fit.curve;
  const auto words = a.curve_payload(fit);
  if (c.present == 0u || words.size() != expected_curve_words(c.kind, c.node_count)) {
    return archive_error(ArchiveStatus::RecordInvalid, Type::FitRecords);
  }
  const std::size_t n = c.node_count;
  const auto u64_at = [&](std::size_t i) { return static_cast<std::size_t>(bits_of(words[i])); };
  std::unique_ptr<IVolCurve> curve;
  switch (static_cast<VolCurveKind>(c.kind)) {
  case VolCurveKind::ConvexDense: {
    ConvexSliceFit f;
    f.T = words[0];
    f.F = words[1];
    f.df = words[2];
    f.rmse_price = words[3];
    f.n_obs = u64_at(4);
    f.n_active = u64_at(5);
    f.qp_iterations = u64_at(6);
    f.qp_stationarity = words[7];
    f.qp_primal_violation = words[8];
    f.qp_complementarity = words[9];
    f.qp_dual_violation = words[10];
    f.effective_lambda = words[11];
    f.noise_scale = words[12];
    f.u.assign(words.begin() + 13, words.begin() + static_cast<std::ptrdiff_t>(13 + n));
    f.C.assign(words.begin() + static_cast<std::ptrdiff_t>(13 + n),
               words.begin() + static_cast<std::ptrdiff_t>(13 + 2 * n));
    curve = std::make_unique<ConvexDenseCurve>(std::move(f));
    break;
  }
  case VolCurveKind::Essvi: {
    EssviParams p{};
    std::memcpy(&p, words.data(), sizeof p);
    curve = std::make_unique<EssviCurve>(p, c.df);
    break;
  }
  case VolCurveKind::Svi: {
    SviParams p{};
    std::memcpy(&p, words.data(), sizeof p);
    curve = std::make_unique<SviCurve>(p, c.df);
    break;
  }
  case VolCurveKind::C8: {
    C8Params p{};
    std::memcpy(&p, words.data(), sizeof p);
    curve = std::make_unique<C8Curve>(p, c.df);
    break;
  }
  case VolCurveKind::LinearVariance:
    curve = std::make_unique<LinearVarianceCurve>(
        c.T, c.F, c.df, std::vector<double>(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(n)),
        std::vector<double>(words.begin() + static_cast<std::ptrdiff_t>(n), words.end()));
    break;
  case VolCurveKind::SplineVol: {
    SplineVolParams p;
    p.atm_vol = words[0];
    p.z_lo_valid = words[1];
    p.z_hi_valid = words[2];
    p.mult_cap = words[3];
    p.w_offset = words[4];
    p.n_butterfly_viol = u64_at(5);
    p.z.assign(words.begin() + 6, words.begin() + static_cast<std::ptrdiff_t>(6 + n));
    p.mult.assign(words.begin() + static_cast<std::ptrdiff_t>(6 + n), words.end());
    curve = std::make_unique<SplineVolCurve>(std::move(p), c.T, c.F, c.df);
    break;
  }
  }
  if (!curve || ev(curve->kind()) != c.kind || !same_bits(curve->T(), c.T) || !same_bits(curve->F(), c.F) ||
      !same_bits(curve->df(), c.df)) {
    return archive_error(ArchiveStatus::RecordInvalid, Type::CurveWords);
  }
  return curve;
}

ArchiveResult<QuoteSliceFit> decode_fit(const FitArchive &a, const FitArchiveFitRecord &r) {
  QuoteSliceFit fit;
  fit.fit_status = static_cast<QuoteSliceFitStatus>(r.fit_status);
  fit.fit_error = std::string(a.string(r.fit_error));
  fit.diagnostics = decode_diagnostics(r.diagnostics);
  if (r.curve.present != 0u) {
    auto curve = decode_curve(a, r);
    if (!curve) return tl::unexpected(curve.error());
    fit.curve = std::move(*curve);
  }
  if (r.prices.present != 0u) {
    const AmericanPriceBounds bounds{r.prices.spot, r.prices.discount_min, r.prices.discount_max,
                                     r.prices.prepaid_min, r.prices.prepaid_max};
    auto prices = AmericanPriceSlice::create(price_nodes(a, r), bounds);
    if (!prices) return archive_error(ArchiveStatus::RecordInvalid, Type::PriceNodes);
    fit.prices = std::move(*prices);
  }
  const auto moved = a.moved_nodes(r);
  fit.moved_nodes.assign(moved.begin(), moved.end());
  const auto rows = a.quote_rows(r);
  fit.rows.resize(rows.size());
  std::transform(rows.begin(), rows.end(), fit.rows.begin(), decode_row);
  fit.market = QuoteSliceMarket{r.market.spot, r.market.variance_years, r.market.calendar_years,
                                r.market.rate, r.market.continuous_yield, r.market.dividend_pv};
  fit.dividend_model = static_cast<QuoteSliceDividendModel>(r.dividend_model);
  if (r.has_al_opts != 0u) fit.al_opts = decode_al(r.al_opts);
  fit.discrete_div_steps = r.discrete_div_steps;
  fit.discrete_div_scheme = static_cast<DiscreteDivScheme>(r.discrete_div_scheme);
  const auto divs = fit_dividends(a, r);
  fit.variance_clock_dividends.assign(divs.begin(), divs.end());
  const auto carry = fit_carry_curve(a, r);
  fit.carry_curve.assign(carry.begin(), carry.end());
  fit.carry_resolution = decode_carry(r.carry, a.string(r.carry.reason));
  return fit;
}

ArchiveResult<QuoteSurfaceSlice> decode_surface_slice(const FitArchive &a, const FitArchiveFitRecord &r) {
  auto fit = decode_fit(a, r);
  if (!fit) return tl::unexpected(fit.error());
  QuoteSurfaceSlice s;
  s.fit = std::move(*fit);
  if (r.slice.present == 0u) return s;
  const FitArchiveSlice &o = r.slice;
  s.grade = static_cast<QuoteSliceGrade>(o.grade);
  s.refusal = std::string(a.string(o.refusal));
  s.error = std::string(a.string(o.error));
  s.point_status = static_cast<QuoteSliceFitStatus>(o.point_status);
  s.channel_refit = (o.flags & kSliceChannelRefit) != 0u;
  s.calendar_retry = (o.flags & kSliceCalendarRetry) != 0u;
  s.calendar_repaired = (o.flags & kSliceCalendarRepaired) != 0u;
  s.calendar_gap = o.calendar_gap;
  s.calendar_gap_strike = o.calendar_gap_strike;
  s.calendar_repair_required = o.calendar_repair_required;
  s.point_error = std::string(a.string(o.point_error));
  s.repair_rung = (o.flags & kSliceRepairRung) != 0u;
  s.rung_repair = decode_repair(o.rung_repair);
  s.calendar_channel_repaired = (o.flags & kSliceCalendarChannelRepaired) != 0u;
  s.calendar_channel_repair = decode_repair(o.calendar_channel_repair);
  s.point_outside_fraction = o.point_outside_fraction;
  s.point_outside_quotes = static_cast<std::size_t>(o.point_outside_quotes);
  s.point_gated = (o.flags & kSlicePointGated) != 0u;
  s.point_fallback = (o.flags & kSlicePointFallback) != 0u;
  s.calendar_readmitted = (o.flags & kSliceCalendarReadmitted) != 0u;
  return s;
}

ArchiveResult<std::vector<QuoteSurfaceSlice>> decode_surface(const FitArchive &a,
                                                             const FitArchiveSurfaceRecord &surface) {
  std::vector<QuoteSurfaceSlice> out;
  const auto fits = a.fits_of(surface);
  out.reserve(fits.size());
  for (const FitArchiveFitRecord &r : fits) {
    auto s = decode_surface_slice(a, r);
    if (!s) return tl::unexpected(s.error());
    out.push_back(std::move(*s));
  }
  return out;
}

QuoteSurfaceConfig decode_surface_config(const FitArchiveSurfaceRecord &s) noexcept {
  QuoteSurfaceConfig c;
  c.term_prior_weight = s.term_prior_weight;
  c.prior_decay_log_time = s.prior_decay_log_time;
  c.prior_quote_halflife = s.prior_quote_halflife;
  c.channel_node_weight = s.channel_node_weight;
  c.calendar_retry_factor = s.calendar_retry_factor;
  c.grade_b_min_mid_strikes = static_cast<std::size_t>(s.grade_b_min_mid_strikes);
  c.calendar_repair_half_spreads = s.calendar_repair_half_spreads;
  return c;
}

ArchiveResult<FitArchiveChainSource> decode_chain_source(const FitArchive &a,
                                                         const FitArchiveLadderRecord &l) {
  FitArchiveChainSource out;
  out.spot = l.spot;
  const auto contracts = a.ladder_contracts(l);
  const std::size_t n = contracts.size();
  out.strike.reserve(n);
  out.expiry.reserve(n);
  out.sigma.reserve(n);
  out.rate.reserve(n);
  out.carry.reserve(n);
  out.side.reserve(n);
  out.record_of_contract.reserve(n);
  for (const FitArchiveLadderContract &c : contracts) {
    out.strike.push_back(c.strike);
    out.expiry.push_back(c.expiry);
    out.sigma.push_back(c.sigma);
    out.rate.push_back(c.rate);
    out.carry.push_back(c.carry);
    out.side.push_back(static_cast<Side>(c.side));
    out.record_of_contract.push_back(c.record);
  }
  out.record_policy.spot_move = l.spot_move;
  out.record_policy.sigma_lo_mult = l.sigma_lo_mult;
  out.record_policy.sigma_hi_mult = l.sigma_hi_mult;
  if (l.has_sample_opts != 0u) out.record_policy.sample_opts = decode_al(l.sample_opts);
  out.record_policy.certify = l.certify != 0u;
  out.record_policy.build_workers = l.build_workers;
  out.ladder_policy.window_sigmas = l.window_sigmas;
  out.ladder_policy.window_min = l.window_min;
  out.ladder_policy.window_max = l.window_max;
  out.ladder_policy.max_cert_usd = l.max_cert_usd;
  out.ladder_policy.max_cert_rel = l.max_cert_rel;
  out.ladder_policy.max_rebuilds_per_tick = l.max_rebuilds_per_tick;
  out.ladder_policy.rebuild_on_exit = l.rebuild_on_exit != 0u;
  out.ladder_policy.rebuild_order = static_cast<SpotLadderRebuildOrder>(l.rebuild_order);
  out.worst_certified_error_fwd = l.worst_certified_error_fwd;
  return out;
}

ArchiveResult<ExpiryRecordBank> decode_expiry_bank(const FitArchive &a, const FitArchiveLadderRecord &l) {
  const auto metas = a.ladder_records(l);
  ExpiryRecordBank bank(metas.size());
  for (const FitArchiveExpiryMeta &m : metas) {
    const ExpiryRecord *rec = expiry_record(a, m);
    if (rec == nullptr || !bank.insert(*rec)) return archive_error(ArchiveStatus::RecordInvalid, Type::ExpiryData);
  }
  return bank;
}

ArchiveResult<FitArchiveMarkovSpec> decode_markov_spec(const FitArchive &a, const FitArchiveMarkovRecord &r) {
  FitArchiveMarkovSpec out;
  out.spot = r.spot;
  AmericanMarkovConfig &c = out.config;
  c.grid_nodes = static_cast<std::size_t>(r.grid_nodes);
  c.upper_spot_multiple = r.upper_spot_multiple;
  c.max_calendar_step = r.max_calendar_step;
  c.max_variance_step = r.max_variance_step;
  c.cfl_fraction = r.cfl_fraction;
  c.max_states = static_cast<std::size_t>(r.max_states);
  c.max_transitions = static_cast<std::size_t>(r.max_transitions);
  c.max_stored_rows = static_cast<std::size_t>(r.max_stored_rows);
  c.max_stored_volatility_values = static_cast<std::size_t>(r.max_stored_volatility_values);
  c.diffusion_method = static_cast<AmericanMarkovDiffusionMethod>(r.diffusion_method);
  c.max_transition_work = static_cast<std::size_t>(r.max_transition_work);
  const auto grid = a.markov_words(r.state_grid);
  out.state_grid.assign(grid.begin(), grid.end());
  for (const FitArchiveMarkovInterval &iv : a.markov_intervals(r)) {
    AmericanMarkovInterval s;
    s.calendar_end = iv.calendar_end;
    s.variance_end = iv.variance_end;
    s.rate = iv.rate;
    s.continuous_yield = iv.continuous_yield;
    s.volatility = iv.volatility;
    s.cash_dividend_at_end = iv.cash_dividend_at_end;
    s.local_volatility = markov_local_vol(a, iv);
    out.intervals.push_back(s);
  }
  return out;
}

// ── Record validation (the record pass of FitArchive::open) ─────────────────

namespace {

[[nodiscard]] ArchiveOutcome validate_symbols(const FitArchive &a) noexcept {
  const auto syms = a.symbols();
  for (std::size_t i = 0; i < syms.size(); ++i) {
    const FitArchiveSymbol &s = syms[i];
    if (s.length == 0u || s.length > kFitArchiveSymbolMax || s.reserved != 0u ||
        !std::all_of(s.name.begin() + s.length, s.name.end(), [](char ch) { return ch == '\0'; })) {
      return archive_error(ArchiveStatus::RecordInvalid, Type::Symbols, i);
    }
    if (i == 0) continue;
    const FitArchiveSymbol &p = syms[i - 1];
    const int c = std::memcmp(p.name.data(), s.name.data(), std::min(p.length, s.length));
    if (!(c < 0 || (c == 0 && p.length < s.length))) {
      return archive_error(ArchiveStatus::IndexUnsorted, Type::Symbols, i);
    }
  }
  return {};
}

template <class R>
[[nodiscard]] ArchiveOutcome validate_keyed(const FitArchive &a, Type records_type, std::span<const R> rows) noexcept {
  const Type index_type = index_for(records_type);
  const bool has_rows = a.section(records_type) != nullptr;
  const bool has_index = a.section(index_type) != nullptr;
  if (has_rows != has_index) return archive_error(ArchiveStatus::MissingSection, has_rows ? index_type : records_type);
  const auto idx = a.index_of(records_type);
  if (idx.size() != rows.size()) return archive_error(ArchiveStatus::IndexBounds, index_type, idx.size());
  const std::size_t symbols = a.symbols().size();
  for (std::size_t i = 0; i < idx.size(); ++i) {
    const FitArchiveIndexEntry &e = idx[i];
    if (i > 0 && !fit_archive_key_less(idx[i - 1], e)) return archive_error(ArchiveStatus::IndexUnsorted, index_type, i);
    if (e.record >= rows.size() || !fit_archive_key_equal(rows[e.record].key, e)) {
      return archive_error(ArchiveStatus::IndexBounds, index_type, i);
    }
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].key.reserved != 0u) return archive_error(ArchiveStatus::RecordInvalid, records_type, i);
    if (rows[i].key.underlier >= symbols) return archive_error(ArchiveStatus::RecordBounds, records_type, i);
  }
  return {};
}

[[nodiscard]] bool repair_ok(const FitArchiveRepair &r) noexcept {
  return r.outcome <= kMaxRepairOutcome && zero_bytes(r.reserved);
}

[[nodiscard]] bool diagnostics_ok(const FitArchiveDiagnostics &d) noexcept {
  const FitArchiveFitDiag &f = d.curve_fit;
  const bool grad_ok = (f.flags & kFitDiagHasGradNorm) != 0u || bits_of(f.final_grad_norm) == 0u;
  return f.termination <= kMaxTermination && f.projection <= kMaxProjection && f.flags <= 7u &&
         f.reserved0 == 0u && f.reserved1 == 0u && grad_ok && d.channel.flags <= 7u &&
         zero_bytes(d.channel.reserved) && repair_ok(d.repair);
}

// Curve payload shape, plus the bytes of native params that must be valid
// values before the payload may be copied into a library struct.
[[nodiscard]] bool curve_ok(const FitArchiveCurve &c, std::span<const double> words) noexcept {
  if (!zero_bytes(c.reserved)) return false;
  if (c.present == 0u) {
    return c.kind == 0u && c.node_count == 0u && c.payload.count == 0u && bits_of(c.T) == 0u &&
           bits_of(c.F) == 0u && bits_of(c.df) == 0u;
  }
  if (c.present != 1u || c.kind > kMaxCurveKind) return false;
  if (!kind_has_nodes(c.kind) && c.node_count != 0u) return false;
  if (words.size() != expected_curve_words(c.kind, c.node_count)) return false;
  const auto raw = std::as_bytes(words);
  if (c.kind == ev(VolCurveKind::C8) && raw[offsetof(C8Params, bumps_active)] > std::byte{1}) return false;
  if (c.kind == ev(VolCurveKind::Essvi) &&
      raw[offsetof(EssviParams, resid_basis_kind)] > std::byte{kMaxResidualBasis}) {
    return false;
  }
  return true;
}

[[nodiscard]] ArchiveOutcome validate_fits(const FitArchive &a) noexcept {
  const auto fits = a.fits();
  const auto rows = a.view<FitArchiveQuoteRow>(Type::QuoteRows);
  const auto moved = a.view<std::uint8_t>(Type::MovedNodes);
  const auto divs = a.view<CashDividend>(Type::Dividends);
  const auto carry = a.view<QuoteSliceCarryPoint>(Type::CarryPoints);
  const auto words = a.view<double>(Type::CurveWords);
  const auto nodes = a.view<AmericanPriceNode>(Type::PriceNodes);
  const std::size_t blob = a.section_bytes(Type::Strings).size();
  const auto surfaces = a.surfaces();
  for (std::size_t i = 0; i < fits.size(); ++i) {
    const FitArchiveFitRecord &r = fits[i];
    const bool refs = ref_in(r.rows, rows) && ref_in(r.moved_nodes, moved) && ref_in(r.dividends, divs) &&
                      ref_in(r.carry_curve, carry) && ref_in(r.curve.payload, words) &&
                      ref_in(r.prices.nodes, nodes) && string_in(r.fit_error, blob) &&
                      string_in(r.carry.reason, blob) && string_in(r.slice.refusal, blob) &&
                      string_in(r.slice.error, blob) && string_in(r.slice.point_error, blob);
    if (!refs) return archive_error(ArchiveStatus::RecordBounds, Type::FitRecords, i);
    const FitArchiveCarry &c = r.carry;
    const FitArchivePrices &p = r.prices;
    const FitArchiveSlice &s = r.slice;
    const bool prices_ok = p.present == 1u ? p.nodes.count >= 2u
                                           : p.present == 0u && zero_bytes(p);
    const bool slice_ok = s.present == 1u
                              ? s.grade <= kMaxGrade && s.point_status <= kMaxFitStatus &&
                                    repair_ok(s.rung_repair) && repair_ok(s.calendar_channel_repair) &&
                                    s.reserved == 0u && r.surface < surfaces.size()
                              : s.present == 0u && zero_bytes(s) && r.surface == kFitArchiveNone;
    const bool ok =
        r.fit_status <= kMaxFitStatus && r.dividend_model <= kMaxDividendModel &&
        r.discrete_div_scheme <= kMaxScheme && r.has_al_opts <= 1u &&
        (r.has_al_opts == 1u || zero_bytes(r.al_opts)) && zero_bytes(r.reserved) &&
        diagnostics_ok(r.diagnostics) && c.mode <= kMaxCarryMode && c.applied <= 1u &&
        c.fallback <= kMaxCarryFallback && c.status <= kMaxForwardStatus && c.reserved == 0u &&
        curve_ok(r.curve, slice_of(words, r.curve.payload)) && prices_ok && slice_ok;
    if (!ok) return archive_error(ArchiveStatus::RecordInvalid, Type::FitRecords, i);
    if (r.surface != kFitArchiveNone) {
      const FitArchiveSurfaceRecord &owner = surfaces[r.surface];
      if (i < owner.first_fit || i - owner.first_fit >= owner.fit_count) {
        return archive_error(ArchiveStatus::RecordInvalid, Type::FitRecords, i);
      }
    }
  }
  for (std::size_t j = 0; j < rows.size(); ++j) {
    const FitArchiveQuoteRow &q = rows[j];
    if (q.quote_status > kMaxQuoteStatus || q.evaluation_status > kMaxEvaluation || q.admitted > 1u ||
        q.weight_capped > 1u || q.reserved != 0u) {
      return archive_error(ArchiveStatus::RecordInvalid, Type::QuoteRows, j);
    }
  }
  for (std::size_t j = 0; j < moved.size(); ++j) {
    if (moved[j] > kMaxMovedNode) return archive_error(ArchiveStatus::RecordInvalid, Type::MovedNodes, j);
  }
  return {};
}

[[nodiscard]] ArchiveOutcome validate_surfaces(const FitArchive &a) noexcept {
  const auto surfaces = a.surfaces();
  const auto fits = a.fits();
  const std::size_t blob = a.section_bytes(Type::Strings).size();
  for (std::size_t i = 0; i < surfaces.size(); ++i) {
    const FitArchiveSurfaceRecord &s = surfaces[i];
    if (!string_in(s.profile, blob) || !string_in(s.policy_source, blob) ||
        !ref_in(FitArchiveArrayRef{s.first_fit, s.fit_count}, fits)) {
      return archive_error(ArchiveStatus::RecordBounds, Type::SurfaceRecords, i);
    }
    bool ok = s.key.expiry_day == 0 && s.key.sub == 0u && s.flags == 0u && s.reserved0 == 0u &&
              zero_bytes(s.reserved);
    std::array<std::uint32_t, 4> counts{};
    for (std::uint32_t k = 0; ok && k < s.fit_count; ++k) {
      const FitArchiveFitRecord &f = fits[s.first_fit + k];
      ok = f.surface == i && f.slice.present == 1u && f.slice.grade <= kMaxGrade &&
           f.key.underlier == s.key.underlier &&
           f.key.snapshot_ns == s.key.snapshot_ns && f.key.sub == 0u &&
           (k == 0 || fits[s.first_fit + k - 1].key.expiry_day < f.key.expiry_day);
      if (ok) ++counts[f.slice.grade];
    }
    if (!ok || counts != s.grade_counts) return archive_error(ArchiveStatus::RecordInvalid, Type::SurfaceRecords, i);
  }
  return {};
}

[[nodiscard]] ArchiveOutcome validate_expiry(const FitArchive &a) noexcept {
  const auto metas = a.expiry_meta();
  const auto raw = a.section_bytes(Type::ExpiryData);
  const std::size_t stored = raw.size() / sizeof(ExpiryRecord);
  if ((a.section(Type::ExpiryData) != nullptr) != (a.section(Type::ExpiryMeta) != nullptr) ||
      stored != metas.size()) {
    return archive_error(ArchiveStatus::MissingSection, Type::ExpiryData);
  }
  if (metas.empty()) return {};
  if (!expiry_layout_ok()) return archive_error(ArchiveStatus::LayoutMismatch, Type::ExpiryData);
  const std::size_t ladders = a.ladders().size();
  for (std::size_t i = 0; i < metas.size(); ++i) {
    const FitArchiveExpiryMeta &m = metas[i];
    const bool ok = m.data_index == i && m.side <= kMaxSide && m.populated == 1u && m.american <= 1u &&
                    m.has_exercise_edge <= 1u && (m.key.sub >> 16) == m.side && zero_bytes(m.reserved) &&
                    (m.ladder == kFitArchiveNone || m.ladder < ladders);
    if (!ok) return archive_error(ArchiveStatus::RecordInvalid, Type::ExpiryMeta, i);
    // Flag bytes must be valid bool / Side values and the padding zero before
    // the bytes are read as an ExpiryRecord.
    const std::byte *b = raw.data() + i * sizeof(ExpiryRecord);
    for (std::size_t at = kErSide; at < kErFlagsEnd; ++at) {
      if (b[at] > std::byte{1}) return archive_error(ArchiveStatus::LayoutMismatch, Type::ExpiryData, i);
    }
    for (std::size_t at = kErFlagsEnd; at < sizeof(ExpiryRecord); ++at) {
      if (b[at] != std::byte{0}) return archive_error(ArchiveStatus::LayoutMismatch, Type::ExpiryData, i);
    }
  }
  const auto records = expiry_records(a);
  for (std::size_t i = 0; i < metas.size(); ++i) {
    const FitArchiveExpiryMeta &m = metas[i];
    const ExpiryRecord &r = records[i];
    const bool same = same_bits(r.expiry(), m.expiry) && same_bits(r.rate(), m.rate) &&
                      same_bits(r.carry(), m.carry) && same_bits(r.k_min(), m.k_min) &&
                      same_bits(r.k_max(), m.k_max) && same_bits(r.sigma_min(), m.sigma_min) &&
                      same_bits(r.sigma_max(), m.sigma_max) &&
                      same_bits(r.forward_factor(), m.forward_factor) &&
                      same_bits(r.certified_max_abs_error_fwd(), m.certified_max_abs_error_fwd) &&
                      r.certified_points() == m.certified_points &&
                      r.uncertified_rows() == m.uncertified_rows && ev(r.side()) == m.side &&
                      r.populated() && r.american() == (m.american != 0u) &&
                      r.has_exercise_edge() == (m.has_exercise_edge != 0u);
    if (!same) return archive_error(ArchiveStatus::LayoutMismatch, Type::ExpiryData, i);
  }
  return {};
}

[[nodiscard]] ArchiveOutcome validate_ladders(const FitArchive &a) noexcept {
  const auto ladders = a.ladders();
  const auto metas = a.expiry_meta();
  const auto contracts = a.view<FitArchiveLadderContract>(Type::LadderContracts);
  for (std::size_t i = 0; i < ladders.size(); ++i) {
    const FitArchiveLadderRecord &l = ladders[i];
    if (!ref_in(l.contracts, contracts) || !ref_in(FitArchiveArrayRef{l.first_record, l.record_count}, metas)) {
      return archive_error(ArchiveStatus::RecordBounds, Type::Ladders, i);
    }
    bool ok = l.has_sample_opts <= 1u && (l.has_sample_opts == 1u || zero_bytes(l.sample_opts)) &&
              l.certify <= 1u && l.rebuild_on_exit <= 1u && l.rebuild_order <= kMaxRebuildOrder &&
              zero_bytes(l.reserved);
    for (std::uint32_t k = 0; ok && k < l.record_count; ++k) ok = metas[l.first_record + k].ladder == i;
    for (const FitArchiveLadderContract &c : slice_of(contracts, l.contracts)) {
      ok = ok && c.record < l.record_count && c.side <= kMaxSide && zero_bytes(c.reserved);
    }
    if (!ok) return archive_error(ArchiveStatus::RecordInvalid, Type::Ladders, i);
  }
  for (std::size_t i = 0; i < metas.size(); ++i) {
    const std::uint32_t owner = metas[i].ladder;
    if (owner == kFitArchiveNone) continue;
    if (owner >= ladders.size()) return archive_error(ArchiveStatus::RecordInvalid, Type::ExpiryMeta, i);
    const FitArchiveLadderRecord &l = ladders[owner];
    if (i < l.first_record || i - l.first_record >= l.record_count) {
      return archive_error(ArchiveStatus::RecordInvalid, Type::ExpiryMeta, i);
    }
  }
  return {};
}

[[nodiscard]] ArchiveOutcome validate_markov(const FitArchive &a) noexcept {
  const auto records = a.markov();
  const auto intervals = a.view<FitArchiveMarkovInterval>(Type::MarkovIntervals);
  const auto local = a.view<AmericanMarkovLocalVolNode>(Type::MarkovLocalVol);
  const auto words = a.view<double>(Type::MarkovWords);
  const std::size_t blob = a.section_bytes(Type::Strings).size();
  for (std::size_t i = 0; i < records.size(); ++i) {
    const FitArchiveMarkovRecord &r = records[i];
    const bool refs = ref_in(r.intervals, intervals) && ref_in(r.state_grid, words) && ref_in(r.sigmas, words) &&
                      ref_in(r.log_state_knots, words) && ref_in(r.variance_time_knots, words) &&
                      ref_in(r.quote_prices, words) && string_in(r.label, blob);
    if (!refs) return archive_error(ArchiveStatus::RecordBounds, Type::MarkovRecords, i);
    const bool fit_ok = r.has_fit == 1u ||
                        (r.has_fit == 0u && r.time_shape == 0u && r.sigmas.count == 0u &&
                         r.log_state_knots.count == 0u && r.variance_time_knots.count == 0u &&
                         r.quote_prices.count == 0u);
    if (r.diffusion_method > kMaxDiffusion || r.time_shape > kMaxTimeShape || !fit_ok || !zero_bytes(r.reserved) ||
        r.key.expiry_day != 0) {
      return archive_error(ArchiveStatus::RecordInvalid, Type::MarkovRecords, i);
    }
    for (const FitArchiveMarkovInterval &iv : slice_of(intervals, r.intervals)) {
      if (!ref_in(iv.local_volatility, local)) return archive_error(ArchiveStatus::RecordBounds, Type::MarkovIntervals, i);
    }
  }
  return {};
}

[[nodiscard]] ArchiveOutcome validate_segments(const FitArchive &a) noexcept {
  const auto segments = a.series_segments();
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const FitArchiveSeriesSegment &s = segments[i];
    const auto end = std::find(s.file_name.begin(), s.file_name.end(), '\0');
    bool ok = end != s.file_name.begin() && end != s.file_name.end() &&
              std::all_of(end, s.file_name.end(), [](char c) { return c == '\0'; }) && s.reserved == 0u &&
              s.sequence != 0u;
    // Manifest order is read order; sequences (hence file names) are unique.
    for (std::size_t j = 0; ok && j < i; ++j) ok = segments[j].sequence != s.sequence;
    if (!ok) return archive_error(ArchiveStatus::RecordInvalid, Type::SeriesSegments, i);
  }
  return {};
}

} // namespace

ArchiveOutcome FitArchive::validate_records() const {
  const ArchiveOutcome checks[] = {
      validate_symbols(*this),
      validate_keyed(*this, Type::SurfaceRecords, surfaces()),
      validate_keyed(*this, Type::FitRecords, fits()),
      validate_keyed(*this, Type::ExpiryMeta, expiry_meta()),
      validate_keyed(*this, Type::Ladders, ladders()),
      validate_keyed(*this, Type::MarkovRecords, markov()),
  };
  for (const ArchiveOutcome &ok : checks) {
    if (!ok) return ok;
  }
  // The surface pass first: validate_fits indexes surfaces through r.surface.
  if (ArchiveOutcome ok = validate_surfaces(*this); !ok) return ok;
  if (ArchiveOutcome ok = validate_fits(*this); !ok) return ok;
  if (ArchiveOutcome ok = validate_ladders(*this); !ok) return ok;
  if (ArchiveOutcome ok = validate_expiry(*this); !ok) return ok;
  if (ArchiveOutcome ok = validate_markov(*this); !ok) return ok;
  return validate_segments(*this);
}

} // namespace atx::vol
