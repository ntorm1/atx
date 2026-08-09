#include "atx/vol/theo.hpp"

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

#include "atx/vol/american.hpp"  // american_price, AlOpts, AmericanMethod
#include "atx/vol/event_vol.hpp" // censored_total_variance, event_recombined_vol, count_events_at
#include "atx/vol/priced_surface.hpp" // PricedSurface
#include "atx/vol/query_pricing.hpp"  // QueryPricingTier
#include "atx/vol/realized_vol.hpp"   // RvPanel

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

// v1 model: y = b0 + sum_i b_i * x_i on the fixed kFairVolFeatureCount schema.
class LinearFairVolModel final : public IFairVolModel {
public:
  LinearFairVolModel(double intercept, std::array<double, kFairVolFeatureCount> coefs)
      : intercept_(intercept), coefs_(coefs) {}

  [[nodiscard]] std::uint32_t feature_schema() const noexcept override {
    return kFairVolFeatureSchemaV1;
  }

  [[nodiscard]] Status predict(std::span<const double> features_row_major, std::size_t n_rows,
                               std::span<double> log_ratio_out) const override {
    if (features_row_major.size() != n_rows * kFairVolFeatureCount) {
      return Err(
          ErrorCode::InvalidArgument,
          "LinearFairVolModel::predict: features span size != n_rows * kFairVolFeatureCount");
    }
    if (log_ratio_out.size() != n_rows) {
      return Err(ErrorCode::InvalidArgument,
                 "LinearFairVolModel::predict: log_ratio_out span size != n_rows");
    }
    for (std::size_t row = 0; row < n_rows; ++row) {
      log_ratio_out[row] =
          dot(features_row_major.subspan(row * kFairVolFeatureCount, kFairVolFeatureCount));
    }
    return Ok();
  }

private:
  // noexcept: pure arithmetic over a fixed-size, already-validated feature
  // row -- the per-row hot loop `predict` drives, no error path to report.
  [[nodiscard]] double dot(std::span<const double> features) const noexcept {
    double y = intercept_;
    for (std::size_t i = 0; i < kFairVolFeatureCount; ++i) {
      y += coefs_[i] * features[i];
    }
    return y;
  }

  double intercept_{0.0};
  std::array<double, kFairVolFeatureCount> coefs_{};
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

Result<std::unique_ptr<IFairVolModel>> load_linear_fair_vol_model(std::string_view coef_tsv_path) {
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
  if (*schema != kFairVolFeatureSchemaV1) {
    return Err(ErrorCode::ParseError, "load_linear_fair_vol_model: unsupported feature schema " +
                                          std::to_string(*schema) + " (expected " +
                                          std::to_string(kFairVolFeatureSchemaV1) + ")");
  }
  constexpr std::size_t kExpectedValues = kFairVolFeatureCount + 1;
  if (tokens.size() != kExpectedValues) {
    return Err(ErrorCode::ParseError, "load_linear_fair_vol_model: expected " +
                                          std::to_string(kExpectedValues) +
                                          " whitespace-separated values (intercept + " +
                                          std::to_string(kFairVolFeatureCount) +
                                          " coefficients), got " + std::to_string(tokens.size()));
  }

  std::array<double, kExpectedValues> values{};
  for (std::size_t i = 0; i < kExpectedValues; ++i) {
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

  std::array<double, kFairVolFeatureCount> coefs{};
  std::copy(values.begin() + 1, values.end(), coefs.begin());
  std::unique_ptr<IFairVolModel> model = std::make_unique<LinearFairVolModel>(values[0], coefs);
  return Ok(std::move(model));
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
