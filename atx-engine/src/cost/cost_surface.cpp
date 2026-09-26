#include "atx/engine/cost/cost_surface.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"

namespace atx::engine::cost {
namespace cost_surface_detail {
struct Data {
  CostSurfaceRecipe recipe;
  CostSurfaceIdentity identity;
  std::vector<CostSurfaceRow> rows;
  std::string recipe_hash;
  std::string snapshot_hash;
};
} // namespace cost_surface_detail
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

bool nonnegative(atx::f64 x) noexcept { return std::isfinite(x) && x >= 0.0; }
bool positive(atx::f64 x) noexcept { return std::isfinite(x) && x > 0.0; }
atx::f64 impact_base(const CostSurfaceRecipe& r, const CostSurfaceRow& row) noexcept {
  const auto base = r.impact_y * row.daily_vol;
  return r.rule == CostSurfaceRule::ModeledInputsV2 ? base * row.impact_multiplier : base;
}
bool valid_fill(CostFillRule fill) noexcept {
  return fill == CostFillRule::FullRequest || fill == CostFillRule::ParticipationCapped;
}

void word(std::string& out, atx::u64 x) {
  for (atx::usize i = 0U; i < 8U; ++i) {
    out.push_back(static_cast<char>(x & 0xffU));
    x >>= 8U;
  }
}
void number(std::string& out, atx::f64 x) {
  word(out, std::bit_cast<atx::u64>(x == 0.0 ? 0.0 : x)); // canonical signed zero
}
void text_field(std::string& out, std::string_view x) {
  word(out, static_cast<atx::u64>(x.size()));
  out.append(x);
}

Status validate(const CostSurfaceRecipe& r, const CostSurfaceIdentity& id,
                std::span<const CostSurfaceRow> rows, atx::u64 budget) {
  if ((r.rule != CostSurfaceRule::SqrtOneWayV1 && r.rule != CostSurfaceRule::ModeledInputsV2) ||
      !nonnegative(r.impact_y) ||
      !nonnegative(r.commission_bps) || !nonnegative(r.spread_scale) ||
      std::isnan(r.max_participation) || r.max_participation <= 0.0)
    return Err(ErrorCode::InvalidArgument, "cost surface: invalid one-way recipe");
  if (id.decision_time_ns <= 0 || id.source_sha256.size() != 64U ||
      id.liquidity_recipe.empty() || id.liquidity_recipe.size() > 4096U ||
      id.calibration_identity.empty() || id.calibration_identity.size() > 4096U)
    return Err(ErrorCode::InvalidArgument, "cost surface: missing or oversized identity");
  for (const char c : id.source_sha256)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return Err(ErrorCode::InvalidArgument, "cost surface: source SHA256 must be hexadecimal");
  // Includes owned rows, duplicate-ID scratch, canonical serialization, identities
  // and fixed object/hash overhead. Check before any population-sized allocation.
  constexpr atx::u64 fixed = 32768U;
  constexpr atx::u64 per_name = sizeof(CostSurfaceRow) + sizeof(atx::u64) + 160U;
  if (rows.empty() || budget < fixed || rows.size() > (budget - fixed) / per_name ||
      rows.size() > (std::numeric_limits<atx::usize>::max() - fixed) / per_name)
    return Err(ErrorCode::InvalidArgument, "cost surface: empty geometry or working budget exceeded");
  for (const auto& row : rows) {
    if (r.rule == CostSurfaceRule::ModeledInputsV2) {
      if (row.borrow_state != CostInputState::Unavailable &&
          (row.borrow_state != CostInputState::Available || row.borrow_available_at_ns <= 0 ||
           row.borrow_available_at_ns >= id.decision_time_ns ||
           !nonnegative(row.borrow_annual_fraction)))
        return Err(ErrorCode::InvalidArgument, "cost surface: invalid modeled borrow row");
      if (row.state == CostInputState::Available &&
          (!nonnegative(row.impact_multiplier) || row.available_at_ns <= 0))
        return Err(ErrorCode::InvalidArgument, "cost surface: invalid modeled impact input");
    }
    if (row.state == CostInputState::Unavailable) continue;
    if (row.state != CostInputState::Available || row.available_at_ns >= id.decision_time_ns ||
        !positive(row.adv_dollars) || !nonnegative(row.daily_vol) || !nonnegative(row.full_spread))
      return Err(ErrorCode::InvalidArgument, "cost surface: invalid or not-yet-available liquidity row");
    const auto spread = (0.5 * row.full_spread) * r.spread_scale;
    const auto impact = impact_base(r, row) / std::sqrt(row.adv_dollars);
    if (!nonnegative(spread) || !nonnegative(impact) ||
        !nonnegative(spread + r.commission_bps * 1e-4))
      return Err(ErrorCode::InvalidArgument, "cost surface: nonfinite derived coefficient");
  }
  return Ok();
}

Result<std::string> recipe_hash(const CostSurfaceRecipe& r, const CostSurfaceIdentity& id) {
  std::string body;
  text_field(body, r.rule == CostSurfaceRule::SqrtOneWayV1 ?
      "atx-cost-surface-recipe-v1/one-way/USD/fraction/delta=0.5" :
      "atx-cost-surface-recipe-v2/modeled/one-way/USD/fraction/delta=0.5/borrow-separate");
  word(body, static_cast<atx::u64>(r.rule));
  number(body, r.impact_y); number(body, r.commission_bps);
  number(body, r.spread_scale); number(body, r.max_participation);
  text_field(body, id.liquidity_recipe); text_field(body, id.calibration_identity);
  return atx::core::sha256_hex(body);
}

Result<std::string> snapshot_hash(const cost_surface_detail::Data& data) {
  std::string body;
  body.reserve(256U + data.rows.size() * 96U);
  const auto modeled = data.recipe.rule == CostSurfaceRule::ModeledInputsV2;
  text_field(body, modeled ? "atx-cost-surface-snapshot-v2" : "atx-cost-surface-snapshot-v1");
  text_field(body, data.recipe_hash); text_field(body, data.identity.source_sha256);
  word(body, std::bit_cast<atx::u64>(data.identity.decision_time_ns));
  word(body, static_cast<atx::u64>(data.rows.size()));
  for (const auto& row : data.rows) {
    word(body, row.instrument_id); word(body, static_cast<atx::u64>(row.state));
    if (row.state == CostInputState::Available) {
      word(body, std::bit_cast<atx::u64>(row.available_at_ns));
      number(body, row.adv_dollars); number(body, row.daily_vol); number(body, row.full_spread);
      if (modeled) number(body, row.impact_multiplier);
    }
    if (modeled) {
      word(body, static_cast<atx::u64>(row.borrow_state));
      if (row.borrow_state == CostInputState::Available) {
        word(body, std::bit_cast<atx::u64>(row.borrow_available_at_ns));
        number(body, row.borrow_annual_fraction);
      }
    }
  }
  return atx::core::sha256_hex(body);
}
} // namespace

CostSurface::CostSurface(std::shared_ptr<const cost_surface_detail::Data> data) noexcept
    : data_{std::move(data)} {}

Result<CostSurface> CostSurface::create(const CostSurfaceRecipe& recipe,
    const CostSurfaceIdentity& identity, std::span<const CostSurfaceRow> rows,
    atx::u64 max_working_bytes) {
  ATX_TRY_VOID(validate(recipe, identity, rows, max_working_bytes));
  std::vector<atx::u64> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) ids.push_back(row.instrument_id);
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
    return Err(ErrorCode::InvalidArgument, "cost surface: duplicate instrument identity");
  auto data = std::make_shared<cost_surface_detail::Data>();
  data->recipe = recipe; data->identity = identity;
  for (char& c : data->identity.source_sha256)
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c + ('a' - 'A'));
  data->rows.assign(rows.begin(), rows.end());
  for (auto& row : data->rows) {
    if (row.state == CostInputState::Unavailable) {
      row.available_at_ns = 0; row.adv_dollars = 0.0; row.daily_vol = 0.0; row.full_spread = 0.0;
      row.impact_multiplier = 1.0;
    }
    if (recipe.rule == CostSurfaceRule::SqrtOneWayV1) {
      row.impact_multiplier = 1.0; row.borrow_state = CostInputState::Unavailable;
    }
    if (row.borrow_state == CostInputState::Unavailable) {
      row.borrow_available_at_ns = 0; row.borrow_annual_fraction = 0.0;
    }
  }
  ATX_TRY(data->recipe_hash, recipe_hash(recipe, data->identity));
  ATX_TRY(data->snapshot_hash, snapshot_hash(*data));
  return Ok(CostSurface{std::move(data)});
}

atx::usize CostSurface::instruments() const noexcept { return data_ ? data_->rows.size() : 0U; }
atx::i64 CostSurface::decision_time_ns() const noexcept { return data_ ? data_->identity.decision_time_ns : 0; }
std::span<const CostSurfaceRow> CostSurface::rows() const noexcept {
  return data_ ? std::span<const CostSurfaceRow>{data_->rows} : std::span<const CostSurfaceRow>{};
}
std::string_view CostSurface::recipe_sha256() const noexcept { return data_ ? data_->recipe_hash : std::string_view{}; }
atx::u64 CostSurface::bytes() const noexcept {
  if (!data_) return 0;
  constexpr auto limit = std::numeric_limits<atx::u64>::max();
  atx::u64 used = sizeof(cost_surface_detail::Data) + 1024U;
  const auto add = [&](atx::u64 count, atx::u64 width) {
    if (width != 0 && count > (limit - used) / width) { used = limit; return; }
    used += count * width;
  };
  add(data_->rows.capacity(), sizeof(CostSurfaceRow));
  for (const auto* value : {&data_->identity.source_sha256, &data_->identity.liquidity_recipe,
                            &data_->identity.calibration_identity, &data_->recipe_hash,
                            &data_->snapshot_hash}) {
    add(value->capacity(), 1U);
    add(1U, 1U); // terminating character; conservative for small-string storage
  }
  return used;
}
std::string_view CostSurface::snapshot_sha256() const noexcept { return data_ ? data_->snapshot_hash : std::string_view{}; }
std::string_view CostSurface::source_sha256() const noexcept { return data_ ? data_->identity.source_sha256 : std::string_view{}; }

CostSurfaceCoefficients CostSurface::coefficients(atx::usize instrument, atx::i64 time,
                                                 atx::f64 nav) const noexcept {
  CostSurfaceCoefficients out;
  if (!positive(nav)) return out;
  if (!data_ || instrument >= data_->rows.size()) { out.status = CostQuoteStatus::OutOfRange; return out; }
  if (time != data_->identity.decision_time_ns) { out.status = CostQuoteStatus::WrongDecision; return out; }
  const auto& row = data_->rows[instrument];
  if (row.state != CostInputState::Available) { out.status = CostQuoteStatus::Unavailable; return out; }
  const auto& r = data_->recipe;
  out.spread_linear = (0.5 * row.full_spread) * r.spread_scale;
  out.commission_linear = r.commission_bps * 1e-4;
  out.impact_three_halves = (impact_base(r, row) / std::sqrt(row.adv_dollars)) * std::sqrt(nav);
  out.max_trade_weight = std::numeric_limits<atx::f64>::infinity();
  if (std::isfinite(r.max_participation)) {
    // Compute the dollar cap first, matching quote_dollars. Overflow means it
    // exceeds every finite dollar request, not that the row is unpriceable.
    const auto dollar_cap = r.max_participation * row.adv_dollars;
    out.max_trade_weight = std::isfinite(dollar_cap) ? dollar_cap / nav :
        r.max_participation * (row.adv_dollars / nav);
  }
  if (!nonnegative(out.impact_three_halves)) {
    out.status = CostQuoteStatus::NumericalOverflow; out.max_trade_weight = 0.0; return out;
  }
  out.status = CostQuoteStatus::Priced;
  return out;
}

CostBorrowRateQuote CostSurface::borrow_annual_rate(atx::usize instrument,
                                                  atx::i64 time) const noexcept {
  if (!data_ || instrument >= data_->rows.size())
    return {CostQuoteStatus::OutOfRange, std::numeric_limits<atx::f64>::quiet_NaN()};
  if (time != data_->identity.decision_time_ns)
    return {CostQuoteStatus::WrongDecision, std::numeric_limits<atx::f64>::quiet_NaN()};
  const auto& row = data_->rows[instrument];
  if (data_->recipe.rule != CostSurfaceRule::ModeledInputsV2 ||
      row.borrow_state != CostInputState::Available) return {};
  return {CostQuoteStatus::Priced, row.borrow_annual_fraction};
}

CostQuote CostSurface::quote_dollars(atx::usize instrument, atx::i64 time,
                                    atx::f64 signed_dollars, CostFillRule fill) const noexcept {
  CostQuote out;
  out.requested_dollars = signed_dollars;
  if (!std::isfinite(signed_dollars) || !valid_fill(fill)) return out;
  if (!data_ || instrument >= data_->rows.size()) { out.status = CostQuoteStatus::OutOfRange; return out; }
  if (time != data_->identity.decision_time_ns) { out.status = CostQuoteStatus::WrongDecision; return out; }
  if (signed_dollars == 0.0) { out.status = CostQuoteStatus::Priced; return out; }
  const auto terms = coefficients(instrument, time, 1.0);
  out.status = terms.status;
  if (!terms.priced()) return out;
  const auto requested = std::abs(signed_dollars);
  const auto quantity = fill == CostFillRule::ParticipationCapped ?
      std::min(requested, terms.max_trade_weight) : requested;
  out.spread_dollars = quantity * terms.spread_linear;
  out.commission_dollars = quantity * terms.commission_linear;
  out.impact_dollars = quantity * (terms.impact_three_halves * std::sqrt(quantity));
  out.total_dollars = out.spread_dollars + out.commission_dollars + out.impact_dollars;
  if (!nonnegative(out.total_dollars)) {
    out.status = CostQuoteStatus::NumericalOverflow; return out;
  }
  out.filled_dollars = quantity == requested ? signed_dollars : std::copysign(quantity, signed_dollars);
  return out;
}

CostWeightQuote CostSurface::quote_weight_change(atx::usize instrument, atx::i64 time,
    atx::f64 delta, atx::f64 nav, CostFillRule fill) const noexcept {
  CostWeightQuote out;
  if (!positive(nav) || !std::isfinite(delta)) return out;
  const auto dollars = delta * nav;
  if (!std::isfinite(dollars)) { out.dollars.status = CostQuoteStatus::NumericalOverflow; return out; }
  out.dollars = quote_dollars(instrument, time, dollars, fill);
  if (out.dollars.priced()) {
    out.cost_return = out.dollars.total_dollars / nav;
    if (!nonnegative(out.cost_return)) out.dollars.status = CostQuoteStatus::NumericalOverflow;
  }
  return out;
}

Result<CostPathSummary> price_trade_path(std::span<const CostSurface> snapshots,
    std::span<const CostPathTrade> trades, CostFillRule fill) {
  if (!valid_fill(fill)) return Err(ErrorCode::InvalidArgument, "cost path: unknown fill rule");
  CostPathSummary out;
  auto previous_time = std::numeric_limits<atx::i64>::min();
  for (const auto& trade : trades) {
    if (trade.snapshot >= snapshots.size() || !positive(trade.pretrade_nav) ||
        trade.decision_time_ns < previous_time)
      return Err(ErrorCode::InvalidArgument, "cost path: invalid geometry, NAV or chronology");
    previous_time = trade.decision_time_ns;
    const auto quote = snapshots[trade.snapshot].quote_dollars(
        trade.instrument, trade.decision_time_ns, trade.signed_dollars, fill);
    if (!quote.priced()) return Err(ErrorCode::Unavailable, "cost path: unpriceable supplied trade");
    out.total_dollars += quote.total_dollars;
    out.spread_return += quote.spread_dollars / trade.pretrade_nav;
    out.commission_return += quote.commission_dollars / trade.pretrade_nav;
    out.impact_return += quote.impact_dollars / trade.pretrade_nav;
    out.cost_return += quote.total_dollars / trade.pretrade_nav;
    out.one_way_turnover += std::abs(quote.filled_dollars) / trade.pretrade_nav;
    if (!nonnegative(out.total_dollars) || !nonnegative(out.cost_return) || !nonnegative(out.one_way_turnover))
      return Err(ErrorCode::InvalidArgument, "cost path: numerical overflow");
  }
  return Ok(out);
}
} // namespace atx::engine::cost
