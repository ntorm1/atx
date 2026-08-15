#include "pricing/theo.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"  // american_price, AlOpts, AmericanMethod
#include "atx/vol/api/analytics/event_vol.hpp" // censored_total_variance, event_recombined_vol, count_events_at
#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface
#include "atx/vol/api/backtest/query_pricing.hpp"  // QueryPricingTier
#include "analytics/realized_vol.hpp"   // RvPanel

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr auto kModelMissing = static_cast<std::uint32_t>(TheoFlagBits::ModelMissing);
} // namespace

TheoEngine::TheoEngine(std::vector<std::unique_ptr<ITheoOverlay>> ovs, const TheoConfig &c)
    : overlays_(std::move(ovs)), cfg_(c) {}

Result<TheoEngine> TheoEngine::create(std::vector<std::unique_ptr<ITheoOverlay>> overlays,
                                      const TheoConfig &cfg) {
  for (const std::unique_ptr<ITheoOverlay> &overlay : overlays) {
    if (overlay == nullptr) {
      return Err(ErrorCode::InvalidArgument, "TheoEngine::create: null overlay");
    }
  }
  if (!(cfg.max_abs_dvol > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::create: max_abs_dvol must be > 0");
  }
  if (!(cfg.band_floor_vol >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::create: band_floor_vol must be >= 0");
  }
  return Ok(TheoEngine(std::move(overlays), cfg));
}

Result<TheoValue> TheoEngine::value(const TheoContext &ctx, const TheoQuery &q) const {
  const std::array<TheoQuery, 1> qs{q};
  std::array<TheoValue, 1> out{};
  const Status st = value_into(ctx, qs, out);
  if (!st.has_value()) {
    return Err(st.error());
  }
  return Ok(out[0]);
}

Status TheoEngine::value_into(const TheoContext &ctx, std::span<const TheoQuery> qs,
                              std::span<TheoValue> out) const {
  if (out.size() != qs.size()) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::value_into: out span size != query count");
  }
  if (ctx.surface == nullptr) {
    return Err(ErrorCode::InvalidArgument, "TheoEngine::value_into: ctx.surface is null");
  }
  const PricedSurface &surface = *ctx.surface;
  // Read ONCE per call (not per query/chunk): whether the surface serves its
  // market mark through a fast-tier cached route. Drives FastTierRoute below.
  const QueryPricingTier tier = surface.query_pricing_tier();
  const bool fast_tier_surface =
      tier == QueryPricingTier::RepresentativeFast || tier == QueryPricingTier::CarryBank;
  // M2 (Task 10 perf pass), read ONCE per call, not per chunk: with zero
  // overlays engaged, theo_vol == market_vol for EVERY query by construction
  // (nothing ever adds to it) -- the whole overlay-scratch apparatus below (the
  // fixed `OverlayAdjust` buffer, its per-chunk clear, the band_sq_sum
  // accumulator) exists only to feed that addition, so this flag lets the loop
  // below skip allocating and clearing it for nothing rather than pay that
  // cost every chunk just to add zero.
  const bool has_overlays = !overlays_.empty();

  std::size_t begin = 0;
  while (begin < qs.size()) {
    const std::size_t n = std::min(kTheoMaxBatch, qs.size() - begin);
    const std::span<const TheoQuery> chunk_q = qs.subspan(begin, n);
    const std::span<TheoValue> chunk_out = out.subspan(begin, n);

    // Baseline: one FUSED surface resolve per query (M1, Task 10 perf pass).
    // `evaluate(Iv|Price)` replaces the former two independent reads -- iv()
    // then fair_value(), each re-doing the SAME (K,T) resolution -- with a
    // single resolve; documented bit-identical to the separate calls
    // (priced_surface.hpp's evaluate() doc: "iv and price match iv(K,T) /
    // fair_value(K,T,side).value()"). theo_vol seeds at market_vol so a chunk
    // with no engaged overlays is bit-for-bit the served mark already (the
    // identity contract) -- nothing below mutates it further.
    for (std::size_t i = 0; i < n; ++i) {
      const TheoQuery &q = chunk_q[i];
      const PricedSurface::FusedResult fused =
          surface.evaluate(q.strike, q.tenor_years, q.side,
                           PricedSurface::EvalField::Iv | PricedSurface::EvalField::Price,
                           /*analytic=*/false);
      if (!fused.status.has_value()) {
        return Err(fused.status.error());
      }
      TheoValue &v = chunk_out[i];
      v = TheoValue{};
      v.market_vol = fused.iv;
      v.market_price = fused.price;
      v.theo_vol = v.market_vol;
    }

    if (!has_overlays) {
      // M2: no overlays => no scratch clearing at all. theo_vol == market_vol
      // for this whole chunk already (set above); finalize the identity path
      // directly -- edge_vol is trivially 0.0 (identical doubles subtract
      // exactly), band_vol is the floor (an empty sum-of-squares floored is
      // the floor), theo_price reuses market_price bit-for-bit (the identity
      // contract), and FastTierRoute never applies (it is gated on a nonzero
      // net dvol, which cannot happen on this branch) -- exactly the values
      // the general path below would also produce for a zero-dvol query, just
      // without ever touching the overlay scratch to get there.
      for (std::size_t i = 0; i < n; ++i) {
        TheoValue &v = chunk_out[i];
        v.edge_vol = 0.0;
        v.band_vol = cfg_.band_floor_vol;
        v.theo_price = v.market_price;
      }
      begin += n;
      continue;
    }

    // Fixed-capacity, no-allocation overlay scratch -- reused across every
    // overlay in this chunk (and across chunks). `band_sq_sum` is a LOCAL
    // accumulator (M5): the running sum-of-squares is never written into the
    // public `TheoValue::band_vol` field mid-batch -- that field is written
    // exactly once, in the finalize loop below, already in its documented
    // half-width-band units. A partial sum-of-squares is not a band and has
    // no business being observable, even transiently, in a caller-owned span.
    std::array<OverlayAdjust, kTheoMaxBatch> scratch{};
    const std::span<OverlayAdjust> scratch_span{scratch.data(), n};
    std::array<double, kTheoMaxBatch> band_sq_sum{};

    std::size_t overlay_index = 0;
    for (const std::unique_ptr<ITheoOverlay> &overlay : overlays_) {
      // M2: `scratch`'s own declaration above already zeroed [0, n) for the
      // FIRST overlay -- re-clearing it here would be a redundant second
      // clear. Overlay index 1+ still needs a fresh scratch: the PREVIOUS
      // overlay's dvol/band is sitting there and would otherwise leak into
      // this overlay's read (each overlay call is expected to see a clean
      // slate to write its OWN adjustment into, per ITheoOverlay::adjust's
      // "fill out[i] for every i" contract).
      if (overlay_index != 0) {
        for (std::size_t i = 0; i < n; ++i) {
          scratch_span[i] = OverlayAdjust{};
        }
      }
      const Status st = overlay->adjust(ctx, chunk_q, scratch_span);
      if (!st.has_value()) {
        return Err(st.error());
      }
      for (std::size_t i = 0; i < n; ++i) {
        const double raw_dvol = scratch_span[i].dvol;
        const double band = scratch_span[i].band;
        // Task 8: OR the overlay's own flags (e.g. ModelMissing on a
        // gracefully-degraded query) into the query's TheoValue -- independent
        // of the clamp check below, which only ever ADDS OverlayClamped.
        chunk_out[i].flags |= scratch_span[i].flags;
        // M3: a broken overlay returning a non-finite adjustment is a BUG in
        // that overlay, not a data condition -- clamping would silently fold
        // the NaN/Inf into theo_vol (poisoning the downstream American
        // reprice with an opaque solver error far from the actual cause) or,
        // worse, mark it OverlayClamped as if it were an ordinary out-of-band
        // value. Fail loud, naming exactly which overlay and which query.
        if (!std::isfinite(raw_dvol) || !std::isfinite(band)) {
          return Err(ErrorCode::Internal,
                     "TheoEngine::value_into: overlay index " + std::to_string(overlay_index) +
                         " ('" + std::string(overlay->name()) +
                         "') returned a non-finite dvol/band for query index " +
                         std::to_string(begin + i));
        }
        const double clamped_dvol = std::clamp(raw_dvol, -cfg_.max_abs_dvol, cfg_.max_abs_dvol);
        if (clamped_dvol != raw_dvol) {
          chunk_out[i].flags |= static_cast<std::uint32_t>(TheoFlagBits::OverlayClamped);
        }
        chunk_out[i].theo_vol += clamped_dvol;
        band_sq_sum[i] += band * band;
      }
      ++overlay_index;
    }

    // Finalize: edge_vol, band_vol (sum-of-squares -> floored sqrt, written
    // once), FastTierRoute, and theo_price (identity reuse, skipped reprice,
    // or a fresh cold American solve -- see the header banner for why a
    // fast-tier surface's theo_price is only a cold-route FAMILY member, not
    // bit-identical to market_price, once dvol != 0).
    for (std::size_t i = 0; i < n; ++i) {
      TheoValue &v = chunk_out[i];
      v.edge_vol = v.market_vol - v.theo_vol;
      v.band_vol = std::max(cfg_.band_floor_vol, std::sqrt(band_sq_sum[i]));

      if (v.theo_vol == v.market_vol) {
        // Zero net overlay adjustment: reuse the already-computed market
        // price bit-for-bit rather than a second, independently-rounded
        // American solve at the "same" vol -- the identity contract.
        v.theo_price = v.market_price;
        continue;
      }
      if (fast_tier_surface) {
        v.flags |= static_cast<std::uint32_t>(TheoFlagBits::FastTierRoute);
      }
      if (!cfg_.price_theo) {
        v.theo_price = kNaN; // vol-space-only sheet: no reprice at a shifted vol
        continue;
      }
      const TheoQuery &q = chunk_q[i];
      const Result<double> theo_price = american_price(
          surface.pricing().S, q.strike, q.tenor_years, v.theo_vol, surface.rate_at(q.tenor_years),
          surface.q_eff_at(q.tenor_years), q.side, surface.pricing().method,
          std::optional<AlOpts>{surface.pricing().al_opts});
      if (!theo_price.has_value()) {
        return Err(theo_price.error());
      }
      v.theo_price = *theo_price;
    }

    begin += n;
  }
  return Ok();
}

Result<std::vector<TheoValue>> compute_theo_sheet(const TheoContext &ctx, const TheoEngine &engine,
                                                  std::span<const TheoQuery> queries) {
  std::vector<TheoValue> out(queries.size());
  const Status st = engine.value_into(ctx, queries, out);
  if (!st.has_value()) {
    return Err(st.error());
  }
  return Ok(std::move(out));
}

// ── RV-blend fair vol (Task 8) ──────────────────────────────────────────────

namespace {

// theo vol level lean: pull ATM level toward an RV-anchored forecast, damped
// in tenor. See `RvBlendConfig`'s doc (theo.hpp) for the model. `market_vol`
// and `tenor_years` are read straight from `ctx`/`query` here, never from a
// caller-supplied `TheoValue` -- overlays run BEFORE the engine has a
// `TheoValue` for this query, so `ctx.surface->iv` is the only source (and
// the engine reads the identical value for its own baseline, by
// construction bit-for-bit consistent).
class RvBlendOverlay final : public ITheoOverlay {
public:
  explicit RvBlendOverlay(const RvBlendConfig &cfg) : cfg_(cfg) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "rv_blend"; }

  [[nodiscard]] Status adjust(const TheoContext &ctx, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = adjust_one(ctx, queries[i]);
    }
    return Ok();
  }

private:
  // Degrades to a zeroed, ModelMissing-flagged adjustment (never non-finite
  // math) when: `ctx.rv` is null; `rv_window_idx` is out of range for
  // `RvPanel::vol`; the selected RV slot is not finite (a window shorter
  // than 2 available bars -- `RvPanel`'s own doc); `market_vol` is not
  // finite or not > 0 (surface extrapolation); `tenor_years` is not finite;
  // or the computed `dvol` itself is somehow still not finite (defense in
  // depth -- construction already rejects the one config combination,
  // `tenor_damp_years <= 0`, that could otherwise divide-by-zero here).
  [[nodiscard]] OverlayAdjust adjust_one(const TheoContext &ctx, const TheoQuery &q) const {
    constexpr OverlayAdjust kMissing{.dvol = 0.0, .band = 0.0, .flags = kModelMissing};
    if (ctx.rv == nullptr) {
      return kMissing;
    }
    if (cfg_.rv_window_idx >= ctx.rv->vol.size()) {
      return kMissing;
    }
    const double rv_anchor = ctx.rv->vol[cfg_.rv_window_idx];
    if (!std::isfinite(rv_anchor)) {
      return kMissing;
    }
    if (!std::isfinite(q.tenor_years)) {
      return kMissing;
    }
    const double market_vol = ctx.surface->iv(q.strike, q.tenor_years);
    if (!std::isfinite(market_vol) || !(market_vol > 0.0)) {
      return kMissing;
    }
    const double w_t = cfg_.weight * std::exp(-q.tenor_years / cfg_.tenor_damp_years);
    const double dvol = w_t * (rv_anchor - market_vol);
    if (!std::isfinite(dvol)) {
      return kMissing;
    }
    return OverlayAdjust{.dvol = dvol, .band = 0.0};
  }

  RvBlendConfig cfg_;
};

} // namespace

Result<std::unique_ptr<ITheoOverlay>> make_rv_blend_overlay(RvBlendConfig cfg) {
  if (!std::isfinite(cfg.weight)) {
    return Err(ErrorCode::InvalidArgument, "make_rv_blend_overlay: weight must be finite");
  }
  if (!std::isfinite(cfg.tenor_damp_years) || !(cfg.tenor_damp_years > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "make_rv_blend_overlay: tenor_damp_years must be finite and > 0");
  }
  std::unique_ptr<ITheoOverlay> overlay = std::make_unique<RvBlendOverlay>(cfg);
  return Ok(std::move(overlay));
}

// ── Event variance swap (Task 8) ────────────────────────────────────────────

namespace {

// Strips the market's implied event move out of the served ATM level and
// re-injects the caller's own forecast. See `EventVarConfig`'s doc (theo.hpp)
// for the model; `censored_total_variance`/`event_recombined_vol` are
// event_vol.hpp's existing FLEX censoring/recombination machinery, unchanged
// here.
class EventVarOverlay final : public ITheoOverlay {
public:
  explicit EventVarOverlay(const EventVarConfig &cfg) : cfg_(cfg) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "event_var"; }

  [[nodiscard]] Status adjust(const TheoContext &ctx, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = adjust_one(ctx, queries[i]);
    }
    return Ok();
  }

private:
  // Degrades to a zeroed, ModelMissing-flagged adjustment (never non-finite
  // math) when: `ctx.events` is null; `market_vol`/`tenor_years` are not
  // finite and > 0 (event_recombined_vol's own domain requires `T > 0`, and a
  // non-positive/non-finite market_vol has no physical total-variance
  // reading to strip); or the computed `dvol` itself is somehow still not
  // finite (defense in depth).
  [[nodiscard]] OverlayAdjust adjust_one(const TheoContext &ctx, const TheoQuery &q) const {
    constexpr OverlayAdjust kMissing{.dvol = 0.0, .band = 0.0, .flags = kModelMissing};
    if (ctx.events == nullptr) {
      return kMissing;
    }
    const double T = q.tenor_years;
    if (!std::isfinite(T) || !(T > 0.0)) {
      return kMissing;
    }
    const double market_vol = ctx.surface->iv(q.strike, T);
    if (!std::isfinite(market_vol) || !(market_vol > 0.0)) {
      return kMissing;
    }
    const std::size_t n = count_events_at(*ctx.events, ctx.surface->pricing().now_ts_ns, T);
    const double w_total = market_vol * market_vol * T;
    const double w_cen = censored_total_variance(w_total, n, cfg_.emove_market);
    const double atm_cen = std::sqrt(w_cen / T);
    const double recombined = event_recombined_vol(atm_cen, T, n, cfg_.emove_forecast);
    const double dvol = recombined - market_vol;
    if (!std::isfinite(dvol)) {
      return kMissing;
    }
    return OverlayAdjust{.dvol = dvol, .band = 0.0};
  }

  EventVarConfig cfg_;
};

} // namespace

Result<std::unique_ptr<ITheoOverlay>> make_event_var_overlay(EventVarConfig cfg) {
  if (!std::isfinite(cfg.emove_forecast) || cfg.emove_forecast < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "make_event_var_overlay: emove_forecast must be finite and >= 0");
  }
  if (!std::isfinite(cfg.emove_market) || cfg.emove_market < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "make_event_var_overlay: emove_market must be finite and >= 0");
  }
  std::unique_ptr<ITheoOverlay> overlay = std::make_unique<EventVarOverlay>(cfg);
  return Ok(std::move(overlay));
}

// ── ML seam: linear v1 fair-vol model (Task 9) ──────────────────────────────

namespace {

// Linear model: y = b0 + sum_i b_i * x_i on a schema-parameterized width
// (v1: 8 features; VRP schema 2: 10 features). The width IS
// `coefs_.size() == fair_vol_feature_count(schema_)`, validated at
// construction by `make_linear_fair_vol_model` -- predict never re-derives it.
class LinearFairVolModel final : public IFairVolModel {
public:
  LinearFairVolModel(std::uint32_t schema, double intercept, std::vector<double> coefs)
      : schema_(schema), intercept_(intercept), coefs_(std::move(coefs)) {}

  [[nodiscard]] std::uint32_t feature_schema() const noexcept override { return schema_; }

  [[nodiscard]] Status predict(std::span<const double> features_row_major, std::size_t n_rows,
                               std::span<double> log_ratio_out) const override {
    const std::size_t width = coefs_.size();
    if (features_row_major.size() != n_rows * width) {
      return Err(ErrorCode::InvalidArgument,
                 "LinearFairVolModel::predict: features span size != n_rows * feature width");
    }
    if (log_ratio_out.size() != n_rows) {
      return Err(ErrorCode::InvalidArgument,
                 "LinearFairVolModel::predict: log_ratio_out span size != n_rows");
    }
    for (std::size_t row = 0; row < n_rows; ++row) {
      log_ratio_out[row] = dot(features_row_major.subspan(row * width, width));
    }
    return Ok();
  }

private:
  // noexcept: pure arithmetic over a fixed-size, already-validated feature
  // row -- the per-row hot loop `predict` drives, no error path to report.
  [[nodiscard]] double dot(std::span<const double> features) const noexcept {
    double y = intercept_;
    for (std::size_t i = 0; i < coefs_.size(); ++i) {
      y += coefs_[i] * features[i];
    }
    return y;
  }

  std::uint32_t schema_{kFairVolFeatureSchemaV1};
  double intercept_{0.0};
  std::vector<double> coefs_;
};

[[nodiscard]] std::string_view rstrip_cr(std::string_view v) noexcept {
  if (!v.empty() && v.back() == '\r') {
    v.remove_suffix(1);
  }
  return v;
}

[[nodiscard]] std::string_view trim(std::string_view v) noexcept {
  const std::size_t start = v.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    return {};
  }
  const std::size_t end = v.find_last_not_of(" \t");
  return v.substr(start, end - start + 1);
}

// Appends every whitespace-separated token in `line` to `tokens` (copied out
// as owned strings -- `line` is a view into a getline buffer reused on the
// next iteration by the caller). Bounded by line.size().
void split_ws_append(std::string_view line, std::vector<std::string> &tokens) {
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
      ++i;
    }
    if (i > start) {
      tokens.emplace_back(line.substr(start, i - start));
    }
  }
}

// Parses "schema=<uint>" (already stripped of the leading '#' and any
// whitespace). nullopt if `after_hash` doesn't match that shape -- the
// caller keeps scanning subsequent comment lines rather than treating a
// non-schema comment as a hard parse error.
[[nodiscard]] std::optional<std::uint32_t> parse_schema_comment(std::string_view after_hash) {
  constexpr std::string_view kPrefix = "schema=";
  const std::string_view body = trim(after_hash);
  if (body.size() <= kPrefix.size() || body.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  const std::string_view digits = body.substr(kPrefix.size());
  std::uint32_t value = 0;
  const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

Result<LinearFairVolParams> load_linear_fair_vol_params(std::string_view coef_tsv_path) {
  std::ifstream in{std::string{coef_tsv_path}, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError,
               "load_linear_fair_vol_model: cannot open '" + std::string{coef_tsv_path} + "'");
  }

  std::optional<std::uint32_t> schema;
  std::vector<std::string> tokens;
  std::string raw_line;
  // Bounded by the file's own line count -- std::getline terminates at EOF.
  while (std::getline(in, raw_line)) {
    const std::string_view line = trim(rstrip_cr(raw_line));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '#') {
      if (!schema.has_value()) {
        schema = parse_schema_comment(line.substr(1));
      }
      continue;
    }
    split_ws_append(line, tokens);
  }

  if (!schema.has_value()) {
    return Err(ErrorCode::ParseError,
               "load_linear_fair_vol_model: missing '# schema=<n>' header line");
  }
  // The schema decides the width; an unknown schema is a fail-closed
  // ParseError (never a guessed width).
  const std::size_t width = fair_vol_feature_count(*schema);
  if (width == 0) {
    return Err(ErrorCode::ParseError,
               "load_linear_fair_vol_model: unsupported feature schema " + std::to_string(*schema));
  }
  const std::size_t expected_values = width + 1;
  if (tokens.size() != expected_values) {
    return Err(ErrorCode::ParseError, "load_linear_fair_vol_model: expected " +
                                          std::to_string(expected_values) +
                                          " whitespace-separated values (intercept + " +
                                          std::to_string(width) + " coefficients), got " +
                                          std::to_string(tokens.size()));
  }

  std::vector<double> values(expected_values, 0.0);
  for (std::size_t i = 0; i < expected_values; ++i) {
    const std::string &tok = tokens[i];
    double v = 0.0;
    const auto r =
        std::from_chars(tok.data(), tok.data() + tok.size(), v, std::chars_format::general);
    if (r.ec != std::errc{} || r.ptr != tok.data() + tok.size() || !std::isfinite(v)) {
      return Err(ErrorCode::ParseError,
                 "load_linear_fair_vol_model: unparseable coefficient token '" + tok + "'");
    }
    values[i] = v;
  }

  LinearFairVolParams params;
  params.feature_schema = *schema;
  params.intercept = values[0];
  params.coefficients.assign(values.begin() + 1, values.end());
  return Ok(std::move(params));
}

namespace {

// Shared validation for the linear plain-data form (make + save).
[[nodiscard]] Status validate_linear_fair_vol_params(const LinearFairVolParams &params,
                                                     std::string_view who) {
  const std::size_t width = fair_vol_feature_count(params.feature_schema);
  if (width == 0) {
    return Err(ErrorCode::InvalidArgument, std::string(who) + ": unsupported feature schema " +
                                               std::to_string(params.feature_schema));
  }
  if (params.coefficients.size() != width) {
    return Err(ErrorCode::InvalidArgument,
               std::string(who) + ": coefficient count " +
                   std::to_string(params.coefficients.size()) + " != schema width " +
                   std::to_string(width));
  }
  if (!std::isfinite(params.intercept)) {
    return Err(ErrorCode::InvalidArgument, std::string(who) + ": non-finite intercept");
  }
  for (const double c : params.coefficients) {
    if (!std::isfinite(c)) {
      return Err(ErrorCode::InvalidArgument, std::string(who) + ": non-finite coefficient");
    }
  }
  return Ok();
}

// std::to_chars shortest round-trip form -- the CANONICAL double spelling
// every model-file writer below uses, so save(load(f)) of a save-produced
// file is byte-identical.
[[nodiscard]] std::string format_double_shortest(double v) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  // SAFETY: 64 bytes always suffices for the shortest form of a double; the
  // only to_chars failure mode is an undersized buffer.
  return std::string(buf.data(), r.ptr);
}

} // namespace

Status save_linear_fair_vol_params(const LinearFairVolParams &params,
                                   std::string_view coef_tsv_path) {
  const Status valid = validate_linear_fair_vol_params(params, "save_linear_fair_vol_params");
  if (!valid.has_value()) {
    return valid;
  }
  std::ofstream out{std::string{coef_tsv_path}, std::ios::binary | std::ios::trunc};
  if (!out) {
    return Err(ErrorCode::IoError, "save_linear_fair_vol_params: cannot open '" +
                                       std::string{coef_tsv_path} + "' for writing");
  }
  std::string body = "# schema=" + std::to_string(params.feature_schema) + "\n";
  body += format_double_shortest(params.intercept);
  for (const double c : params.coefficients) {
    body += '\t';
    body += format_double_shortest(c);
  }
  body += '\n';
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    return Err(ErrorCode::IoError,
               "save_linear_fair_vol_params: write failed for '" + std::string{coef_tsv_path} +
                   "'");
  }
  return Ok();
}

Result<std::unique_ptr<IFairVolModel>> make_linear_fair_vol_model(LinearFairVolParams params) {
  const Status valid = validate_linear_fair_vol_params(params, "make_linear_fair_vol_model");
  if (!valid.has_value()) {
    return Err(valid.error());
  }
  std::unique_ptr<IFairVolModel> model = std::make_unique<LinearFairVolModel>(
      params.feature_schema, params.intercept, std::move(params.coefficients));
  return Ok(std::move(model));
}

Result<std::unique_ptr<IFairVolModel>> load_linear_fair_vol_model(std::string_view coef_tsv_path) {
  auto params = load_linear_fair_vol_params(coef_tsv_path);
  if (!params.has_value()) {
    return Err(params.error());
  }
  return make_linear_fair_vol_model(std::move(*params));
}

// ── ML seam: flat-array GBT scorer (vrp-ml sprint, schema-2 model files) ────

namespace {

// Structural contract validation shared by make_gbt_fair_vol_model and
// save_gbt_fair_vol_model_data. Everything predict() relies on for bounded,
// in-range walks is proven HERE, once -- see GbtFairVolModelData's doc
// (theo.hpp) for the contract being enforced.
[[nodiscard]] Status validate_gbt_fair_vol_data(const GbtFairVolModelData &d,
                                                std::string_view who) {
  const std::string prefix{who};
  const std::size_t width = fair_vol_feature_count(d.feature_schema);
  if (width == 0) {
    return Err(ErrorCode::InvalidArgument, prefix + ": unsupported feature schema " +
                                               std::to_string(d.feature_schema));
  }
  if (!std::isfinite(d.base)) {
    return Err(ErrorCode::InvalidArgument, prefix + ": non-finite base");
  }
  const std::size_t n = d.feature_idx.size();
  if (d.threshold.size() != n || d.left.size() != n || d.right.size() != n ||
      d.leaf_value.size() != n) {
    return Err(ErrorCode::InvalidArgument, prefix + ": node array lengths disagree");
  }
  const std::size_t n_trees = d.tree_first_node.size();
  if (n_trees == 0) {
    if (n != 0) {
      return Err(ErrorCode::InvalidArgument, prefix + ": nodes present but no trees");
    }
    return Ok(); // empty model: predicts base
  }
  if (n == 0) {
    return Err(ErrorCode::InvalidArgument, prefix + ": trees present but no nodes");
  }
  if (d.tree_first_node.front() != 0) {
    return Err(ErrorCode::InvalidArgument, prefix + ": first tree must start at node 0");
  }
  for (std::size_t t = 0; t < n_trees; ++t) {
    const std::size_t begin = d.tree_first_node[t];
    const std::size_t end = (t + 1 < n_trees) ? d.tree_first_node[t + 1] : n;
    if (begin >= n || end <= begin || end > n) {
      return Err(ErrorCode::InvalidArgument,
                 prefix + ": tree offsets not strictly increasing/in range");
    }
    for (std::size_t i = begin; i < end; ++i) {
      if (!std::isfinite(d.threshold[i]) || !std::isfinite(d.leaf_value[i])) {
        return Err(ErrorCode::InvalidArgument,
                   prefix + ": non-finite threshold/leaf_value at node " + std::to_string(i));
      }
      const std::int32_t l = d.left[i];
      const std::int32_t r = d.right[i];
      const bool left_leaf = l < 0;
      const bool right_leaf = r < 0;
      if (left_leaf != right_leaf) {
        return Err(ErrorCode::InvalidArgument,
                   prefix + ": half-leaf node " + std::to_string(i) +
                       " (exactly one child is -1)");
      }
      if (left_leaf) {
        if (l != -1 || r != -1) {
          return Err(ErrorCode::InvalidArgument,
                     prefix + ": leaf sentinel must be exactly -1 at node " + std::to_string(i));
        }
        continue;
      }
      if (d.feature_idx[i] >= width) {
        return Err(ErrorCode::InvalidArgument,
                   prefix + ": feature index " + std::to_string(d.feature_idx[i]) +
                       " out of schema width at node " + std::to_string(i));
      }
      const auto lu = static_cast<std::size_t>(l);
      const auto ru = static_cast<std::size_t>(r);
      // Children strictly AFTER the parent and inside this tree: the
      // topological order that makes every predict walk provably terminate.
      if (lu <= i || lu >= end || ru <= i || ru >= end) {
        return Err(ErrorCode::InvalidArgument,
                   prefix + ": child index out of order/range at node " + std::to_string(i));
      }
    }
  }
  return Ok();
}

// Flat-array GBT scorer over a schema-parameterized feature row. Predict is
// allocation-free: a bounded index walk per (row, tree) over the validated
// SoA node arrays -- construction (make_gbt_fair_vol_model) proved children
// strictly increase within each tree, so a walk takes < tree-size steps.
class GbtFairVolModel final : public IFairVolModel {
public:
  explicit GbtFairVolModel(GbtFairVolModelData data)
      : data_(std::move(data)), width_(fair_vol_feature_count(data_.feature_schema)) {}

  [[nodiscard]] std::uint32_t feature_schema() const noexcept override {
    return data_.feature_schema;
  }

  [[nodiscard]] Status predict(std::span<const double> features_row_major, std::size_t n_rows,
                               std::span<double> log_ratio_out) const override {
    if (features_row_major.size() != n_rows * width_) {
      return Err(ErrorCode::InvalidArgument,
                 "GbtFairVolModel::predict: features span size != n_rows * feature width");
    }
    if (log_ratio_out.size() != n_rows) {
      return Err(ErrorCode::InvalidArgument,
                 "GbtFairVolModel::predict: log_ratio_out span size != n_rows");
    }
    const std::size_t n_nodes = data_.feature_idx.size();
    for (std::size_t row = 0; row < n_rows; ++row) {
      const std::span<const double> x = features_row_major.subspan(row * width_, width_);
      double acc = data_.base;
      for (const std::uint32_t root : data_.tree_first_node) {
        std::size_t idx = root;
        // Bounded loop: construction validated child > parent inside the
        // tree, so idx strictly increases; n_nodes iterations is a hard
        // ceiling (defense in depth against a memory-corrupted model).
        std::size_t steps = 0;
        while (data_.left[idx] >= 0) {
          // NaN feature: IEEE `<` is false on NaN -> routes RIGHT,
          // deterministically (documented in theo.hpp).
          idx = (x[data_.feature_idx[idx]] < data_.threshold[idx])
                    ? static_cast<std::size_t>(data_.left[idx])
                    : static_cast<std::size_t>(data_.right[idx]);
          if (++steps > n_nodes) {
            return Err(ErrorCode::Internal,
                       "GbtFairVolModel::predict: tree walk exceeded node count");
          }
        }
        acc += data_.leaf_value[idx];
      }
      log_ratio_out[row] = acc;
    }
    return Ok();
  }

private:
  GbtFairVolModelData data_;
  std::size_t width_{0};
};

// Parses "<key><uint>" from an already-'#'-stripped comment body (same shape
// as parse_schema_comment above, generalized to the GBT file's format-version
// key). nullopt when the comment is not this key.
[[nodiscard]] std::optional<std::uint32_t> parse_u32_comment_key(std::string_view after_hash,
                                                                 std::string_view key) {
  const std::string_view body = trim(after_hash);
  if (body.size() <= key.size() || body.substr(0, key.size()) != key) {
    return std::nullopt;
  }
  const std::string_view digits = body.substr(key.size());
  std::uint32_t value = 0;
  const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

template <typename T> [[nodiscard]] bool parse_int_token(std::string_view tok, T &out) {
  const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), out);
  return r.ec == std::errc{} && r.ptr == tok.data() + tok.size();
}

[[nodiscard]] bool parse_double_token(std::string_view tok, double &out) {
  const auto r =
      std::from_chars(tok.data(), tok.data() + tok.size(), out, std::chars_format::general);
  return r.ec == std::errc{} && r.ptr == tok.data() + tok.size();
}

} // namespace

Result<GbtFairVolModelData> load_gbt_fair_vol_model_data(std::string_view path) {
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) {
    return Err(ErrorCode::IoError,
               "load_gbt_fair_vol_model: cannot open '" + std::string{path} + "'");
  }

  std::optional<std::uint32_t> format_version;
  std::optional<std::uint32_t> schema;
  std::vector<std::vector<std::string>> content; // tokenized non-comment lines, file order
  std::string raw_line;
  // Bounded by the file's own line count -- std::getline terminates at EOF.
  while (std::getline(in, raw_line)) {
    const std::string_view line = trim(rstrip_cr(raw_line));
    if (line.empty()) {
      continue;
    }
    if (line.front() == '#') {
      if (!format_version.has_value()) {
        format_version = parse_u32_comment_key(line.substr(1), "gbt_fair_vol=");
      }
      if (!schema.has_value()) {
        schema = parse_schema_comment(line.substr(1));
      }
      continue;
    }
    std::vector<std::string> tokens;
    split_ws_append(line, tokens);
    content.push_back(std::move(tokens));
  }

  if (!format_version.has_value() || *format_version != 1) {
    return Err(ErrorCode::ParseError,
               "load_gbt_fair_vol_model: missing/unsupported '# gbt_fair_vol=<v>' format line");
  }
  if (!schema.has_value()) {
    return Err(ErrorCode::ParseError,
               "load_gbt_fair_vol_model: missing '# schema=<n>' header line");
  }
  if (fair_vol_feature_count(*schema) == 0) {
    return Err(ErrorCode::ParseError,
               "load_gbt_fair_vol_model: unsupported feature schema " + std::to_string(*schema));
  }

  GbtFairVolModelData data;
  data.feature_schema = *schema;
  std::size_t at = 0;
  const auto tagged_line = [&](std::string_view tag,
                               std::string &value_out) -> Status {
    if (at >= content.size() || content[at].size() != 2 || content[at][0] != tag) {
      return Err(ErrorCode::ParseError, "load_gbt_fair_vol_model: expected '" + std::string(tag) +
                                            "\\t<value>' at content line " + std::to_string(at));
    }
    value_out = content[at][1];
    ++at;
    return Ok();
  };

  std::string value;
  Status st = tagged_line("base", value);
  if (!st.has_value()) {
    return Err(st.error());
  }
  if (!parse_double_token(value, data.base)) {
    return Err(ErrorCode::ParseError, "load_gbt_fair_vol_model: unparseable base '" + value + "'");
  }

  st = tagged_line("trees", value);
  if (!st.has_value()) {
    return Err(st.error());
  }
  std::size_t n_trees = 0;
  if (!parse_int_token(value, n_trees)) {
    return Err(ErrorCode::ParseError,
               "load_gbt_fair_vol_model: unparseable tree count '" + value + "'");
  }
  data.tree_first_node.reserve(n_trees);
  for (std::size_t t = 0; t < n_trees; ++t) {
    st = tagged_line("tree", value);
    if (!st.has_value()) {
      return Err(st.error());
    }
    std::uint32_t first = 0;
    if (!parse_int_token(value, first)) {
      return Err(ErrorCode::ParseError,
                 "load_gbt_fair_vol_model: unparseable tree offset '" + value + "'");
    }
    data.tree_first_node.push_back(first);
  }

  st = tagged_line("nodes", value);
  if (!st.has_value()) {
    return Err(st.error());
  }
  std::size_t n_nodes = 0;
  if (!parse_int_token(value, n_nodes)) {
    return Err(ErrorCode::ParseError,
               "load_gbt_fair_vol_model: unparseable node count '" + value + "'");
  }
  data.feature_idx.reserve(n_nodes);
  data.threshold.reserve(n_nodes);
  data.left.reserve(n_nodes);
  data.right.reserve(n_nodes);
  data.leaf_value.reserve(n_nodes);
  for (std::size_t i = 0; i < n_nodes; ++i) {
    if (at >= content.size() || content[at].size() != 5) {
      return Err(ErrorCode::ParseError,
                 "load_gbt_fair_vol_model: expected a 5-field node line at content line " +
                     std::to_string(at));
    }
    const std::vector<std::string> &f = content[at];
    std::uint32_t feature = 0;
    double threshold = 0.0;
    std::int32_t left = 0;
    std::int32_t right = 0;
    double leaf = 0.0;
    if (!parse_int_token(f[0], feature) || !parse_double_token(f[1], threshold) ||
        !parse_int_token(f[2], left) || !parse_int_token(f[3], right) ||
        !parse_double_token(f[4], leaf)) {
      return Err(ErrorCode::ParseError,
                 "load_gbt_fair_vol_model: unparseable node line at content line " +
                     std::to_string(at));
    }
    data.feature_idx.push_back(feature);
    data.threshold.push_back(threshold);
    data.left.push_back(left);
    data.right.push_back(right);
    data.leaf_value.push_back(leaf);
    ++at;
  }
  if (at != content.size()) {
    return Err(ErrorCode::ParseError,
               "load_gbt_fair_vol_model: trailing content after the declared node count");
  }
  return Ok(std::move(data));
}

Status save_gbt_fair_vol_model_data(const GbtFairVolModelData &data, std::string_view path) {
  const Status valid = validate_gbt_fair_vol_data(data, "save_gbt_fair_vol_model_data");
  if (!valid.has_value()) {
    return valid;
  }
  std::ofstream out{std::string{path}, std::ios::binary | std::ios::trunc};
  if (!out) {
    return Err(ErrorCode::IoError, "save_gbt_fair_vol_model_data: cannot open '" +
                                       std::string{path} + "' for writing");
  }
  std::string body = "# gbt_fair_vol=1\n# schema=" + std::to_string(data.feature_schema) + "\n";
  body += "base\t" + format_double_shortest(data.base) + "\n";
  body += "trees\t" + std::to_string(data.tree_first_node.size()) + "\n";
  for (const std::uint32_t first : data.tree_first_node) {
    body += "tree\t" + std::to_string(first) + "\n";
  }
  body += "nodes\t" + std::to_string(data.feature_idx.size()) + "\n";
  for (std::size_t i = 0; i < data.feature_idx.size(); ++i) {
    body += std::to_string(data.feature_idx[i]);
    body += '\t';
    body += format_double_shortest(data.threshold[i]);
    body += '\t';
    body += std::to_string(data.left[i]);
    body += '\t';
    body += std::to_string(data.right[i]);
    body += '\t';
    body += format_double_shortest(data.leaf_value[i]);
    body += '\n';
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    return Err(ErrorCode::IoError,
               "save_gbt_fair_vol_model_data: write failed for '" + std::string{path} + "'");
  }
  return Ok();
}

Result<std::unique_ptr<IFairVolModel>> make_gbt_fair_vol_model(GbtFairVolModelData data) {
  const Status valid = validate_gbt_fair_vol_data(data, "make_gbt_fair_vol_model");
  if (!valid.has_value()) {
    return Err(valid.error());
  }
  std::unique_ptr<IFairVolModel> model = std::make_unique<GbtFairVolModel>(std::move(data));
  return Ok(std::move(model));
}

Result<std::unique_ptr<IFairVolModel>> load_gbt_fair_vol_model(std::string_view path) {
  auto data = load_gbt_fair_vol_model_data(path);
  if (!data.has_value()) {
    return Err(data.error());
  }
  return make_gbt_fair_vol_model(std::move(*data));
}

// ── ML seam: model-driven fair vol overlay (Task 9) ─────────────────────────

namespace {

// Assembles the fixed kFairVolFeatureCount feature row from `ctx` + `query`,
// batches every eligible row in a chunk through one `model->predict` call,
// and converts the returned log-ratio to a vol-space `dvol`. Mirrors
// RvBlendOverlay/EventVarOverlay's per-row graceful degradation (Task 8): a
// row whose required context/surface/model inputs are missing or non-finite
// gets `dvol = 0` + `ModelMissing` rather than failing the whole batch.
class FairVolModelOverlay final : public ITheoOverlay {
public:
  explicit FairVolModelOverlay(std::shared_ptr<const IFairVolModel> model)
      : model_(std::move(model)) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "fair_vol_model"; }

  [[nodiscard]] Status adjust(const TheoContext &ctx, std::span<const TheoQuery> queries,
                              std::span<OverlayAdjust> out) const override {
    // Sub-chunk on the same kTheoMaxBatch cap TheoEngine itself chunks on, so
    // this overlay's own scratch stays fixed-capacity (no heap allocation)
    // regardless of caller -- TheoEngine always calls with queries.size() <=
    // kTheoMaxBatch already, but a direct caller (e.g. a test) is not assumed
    // to uphold that, so this loop is a genuine bound, not decoration.
    std::size_t begin = 0;
    while (begin < queries.size()) {
      const std::size_t n = std::min(kTheoMaxBatch, queries.size() - begin);
      const Status st = adjust_chunk(ctx, queries.subspan(begin, n), out.subspan(begin, n));
      if (!st.has_value()) {
        return st;
      }
      begin += n;
    }
    return Ok();
  }

private:
  [[nodiscard]] Status adjust_chunk(const TheoContext &ctx, std::span<const TheoQuery> queries,
                                    std::span<OverlayAdjust> out) const {
    constexpr OverlayAdjust kMissingAdjust{.dvol = 0.0, .band = 0.0, .flags = kModelMissing};

    std::array<double, kTheoMaxBatch> market_vol{};
    std::array<double, kTheoMaxBatch * kFairVolFeatureCount> feature_buf{};
    std::array<std::size_t, kTheoMaxBatch> row_query_index{};
    std::size_t n_eligible = 0;

    for (std::size_t i = 0; i < queries.size(); ++i) {
      out[i] = kMissingAdjust; // default; overwritten below iff this row predicts
      double mv = 0.0;
      std::array<double, kFairVolFeatureCount> feats{};
      if (!build_features(ctx, queries[i], mv, feats)) {
        continue;
      }
      market_vol[n_eligible] = mv;
      row_query_index[n_eligible] = i;
      std::copy(feats.begin(), feats.end(),
                feature_buf.begin() + n_eligible * kFairVolFeatureCount);
      ++n_eligible;
    }

    if (n_eligible == 0) {
      return Ok(); // every row already defaulted to kMissingAdjust above
    }

    std::array<double, kTheoMaxBatch> log_ratio{};
    const std::span<const double> features_span{feature_buf.data(),
                                                n_eligible * kFairVolFeatureCount};
    const std::span<double> log_ratio_span{log_ratio.data(), n_eligible};
    const Status st = model_->predict(features_span, n_eligible, log_ratio_span);
    if (!st.has_value()) {
      return st; // a broken/mismatched model is a bug -- propagate, not a data condition
    }

    for (std::size_t r = 0; r < n_eligible; ++r) {
      const std::size_t qi = row_query_index[r];
      const double y = log_ratio[r];
      if (!std::isfinite(y)) {
        continue; // out[qi] already kMissingAdjust -- never emit NaN
      }
      const double dvol = market_vol[r] * (std::exp(y) - 1.0);
      if (!std::isfinite(dvol)) {
        continue; // out[qi] already kMissingAdjust
      }
      // Band contribution |dvol| * 0.5 -- a placeholder until the model ships
      // quantile heads (residual work, not this task's scope).
      out[qi] = OverlayAdjust{.dvol = dvol, .band = std::fabs(dvol) * 0.5, .flags = 0};
    }
    return Ok();
  }

  // Fills `market_vol_out`/`features_out` and returns true iff every input
  // the feature row needs (ctx.rv, ctx.events, a finite surface read, a
  // finite analytic delta) is present and valid. false -> the caller leaves
  // this row at its already-written kMissingAdjust default.
  [[nodiscard]] bool build_features(const TheoContext &ctx, const TheoQuery &q,
                                    double &market_vol_out,
                                    std::array<double, kFairVolFeatureCount> &features_out) const {
    if (ctx.rv == nullptr || ctx.events == nullptr) {
      return false;
    }
    if (!std::isfinite(q.tenor_years) || !(q.tenor_years > 0.0)) {
      return false;
    }
    const double market_vol = ctx.surface->iv(q.strike, q.tenor_years);
    if (!std::isfinite(market_vol) || !(market_vol > 0.0)) {
      return false;
    }
    const double forward = ctx.surface->forward_at(q.tenor_years);
    if (!std::isfinite(forward) || !(forward > 0.0)) {
      return false;
    }
    const double log_moneyness = std::log(q.strike / forward);
    if (!std::isfinite(log_moneyness)) {
      return false;
    }
    // M4: features 3/4 are contractually rv_21d/rv_63d (kFairVolFeatureSchemaV1,
    // theo.hpp), but `RvPanel::window` is a public, caller-settable field
    // (realized_vol.hpp) -- `realized_vol_panel` happens to always populate
    // the default {5,21,63,252} windows today, but nothing enforces that a
    // caller handing this overlay a `TheoContext::rv` built some other way
    // used the windows the model expects. Degrade to ModelMissing rather than
    // silently feeding the model whatever vol lives at slots 1/2.
    if (ctx.rv->window[1] != 21 || ctx.rv->window[2] != 63) {
      return false;
    }
    const double rv_21d = ctx.rv->vol[1];
    const double rv_63d = ctx.rv->vol[2];
    if (!std::isfinite(rv_21d) || !std::isfinite(rv_63d)) {
      return false;
    }
    const Result<double> delta_r = ctx.surface->delta(q.strike, q.tenor_years, q.side);
    if (!delta_r.has_value()) {
      return false;
    }
    const double delta_abs = std::fabs(*delta_r);
    if (!std::isfinite(delta_abs)) {
      return false;
    }
    const std::size_t n_events =
        count_events_at(*ctx.events, ctx.surface->pricing().now_ts_ns, q.tenor_years);

    market_vol_out = market_vol;
    features_out = {log_moneyness,
                    q.tenor_years,
                    market_vol,
                    rv_21d,
                    rv_63d,
                    market_vol - rv_21d,
                    static_cast<double>(n_events),
                    delta_abs};
    return true;
  }

  std::shared_ptr<const IFairVolModel> model_;
};

} // namespace

Result<std::unique_ptr<ITheoOverlay>>
make_fair_vol_model_overlay(std::shared_ptr<const IFairVolModel> model) {
  if (model == nullptr) {
    return Err(ErrorCode::InvalidArgument, "make_fair_vol_model_overlay: model must not be null");
  }
  // The overlay assembles exactly the kFairVolFeatureSchemaV1 layout (see
  // theo.hpp's feature-order comment) and nothing else -- a model trained
  // against a different schema must be refused here, not silently handed a
  // feature block laid out for a schema it never saw.
  const std::uint32_t schema = model->feature_schema();
  if (schema != kFairVolFeatureSchemaV1) {
    return Err(ErrorCode::InvalidArgument,
               "make_fair_vol_model_overlay: model feature_schema() == " + std::to_string(schema) +
                   ", expected " + std::to_string(kFairVolFeatureSchemaV1));
  }
  std::unique_ptr<ITheoOverlay> overlay = std::make_unique<FairVolModelOverlay>(std::move(model));
  return Ok(std::move(overlay));
}

} // namespace atx::vol
