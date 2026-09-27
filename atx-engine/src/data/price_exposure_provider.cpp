#include "atx/engine/data/price_exposure_provider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/history_panel.hpp"
#include "atx/engine/data/panel_store.hpp"

namespace atx::engine::data {
namespace {
using namespace atx;
namespace co = atx::core;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kLong = 252, kCloseRing = 253, kLiquid = 63;
constexpr f64 kEps = std::numeric_limits<f64>::epsilon();

struct Budget {
  u64 limit{}, used{};
  bool add(u64 count, u64 width = 1) noexcept {
    if (used > limit || (width && count > (limit - used) / width)) return false;
    used += count * width; return true;
  }
};
bool hash_ok(std::string_view v) {
  return v.size() == 64 && std::all_of(v.begin(), v.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool increasing(std::span<const i64> a) {
  return !a.empty() && a.front() > 0 &&
      std::adjacent_find(a.begin(), a.end(), std::greater_equal<i64>{}) == a.end();
}
bool parent(const ExposurePanelConfig& c, std::string_view role) {
  return std::any_of(c.parents.begin(), c.parents.end(), [&](const auto& p) {
    return p.role == role && hash_ok(p.sha256);
  });
}
bool parent_is(const ExposurePanelConfig& c, std::string_view role, std::string_view sha) {
  return std::count_if(c.parents.begin(), c.parents.end(), [&](const auto& p) {
    return p.role == role && p.sha256 == sha;
  }) == 1;
}
bool known(i64 available, u8 qualified, i64 decision) {
  return qualified == 1 && available > 0 && available < decision;
}
bool identity_valid(const ExposureInterval& v, i64 decision) {
  return v.qualified && v.valid_from_ns > 0 && v.valid_from_ns <= decision &&
      v.available_at_ns > 0 && v.available_at_ns < decision &&
      !(v.valid_to_ns != std::numeric_limits<i64>::max() &&
        v.end_available_at_ns > 0 && v.end_available_at_ns < decision &&
        decision >= v.valid_to_ns);
}
bool cap_valid(const ExposureObservation& v, i64 decision) {
  return v.qualified && v.observed_through_ns > 0 &&
      v.observed_through_ns <= v.available_at_ns && v.available_at_ns < decision &&
      std::isfinite(v.value) && v.value > 0;
}
co::Status validate(const PriceExposureConfig& c) {
  const auto& o = c.output;
  const auto t = o.session_keys.size(), n = o.instrument_ids.size();
  if (!t || t > 8192 || !n || n > 32768 || !increasing(o.session_keys) ||
      !increasing(o.instrument_ids) || !increasing(o.decision_times_ns) ||
      o.decision_times_ns.size() != t || c.mark_times_ns.size() != t ||
      !increasing(c.mark_times_ns) || o.instrument_namespace.empty() ||
      o.instrument_namespace.size() > 128 || o.parents.size() > 32 ||
      (o.provenance != ExposureProvenance::SyntheticDeclaredV1 &&
       o.provenance != ExposureProvenance::PitDeclaredV1) ||
      o.normalization != ExposureNormalizationRule::RawWinsor16CapCenterV1 ||
      c.rule != PriceExposureRule::PriorKnownCapPriceV1 ||
      c.close_basis != ExposureCloseBasis::AdjustedCloseRatioV1 ||
      c.dollar_basis != ExposureDollarBasis::RawUsdCloseTimesRawSharesV1 ||
      !hash_ok(c.price_source_sha256) || !parent_is(o, "prices", c.price_source_sha256) ||
      (c.use_return_guard && !parent(o, "return_guard")))
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: axes/basis/parents/rule");
  // Reject oversized or conflicting metadata before copying output into the
  // provider/builder. The fixed role-count bound alone does not bound strings.
  for (usize i = 0; i < o.parents.size(); ++i) {
    const auto& p = o.parents[i];
    if (p.role.empty() || p.role.size() > 128 || !hash_ok(p.sha256))
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: parent metadata");
    for (usize j = 0; j < i; ++j) if (p.role == o.parents[j].role)
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: duplicate parent role");
  }
  for (auto name : {std::string_view(c.close_field), std::string_view(c.raw_close_field),
                    std::string_view(c.volume_field)})
    if (name.empty() || name.size() > 128)
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: field name");
  if (c.close_field == c.raw_close_field || c.close_field == c.volume_field ||
      c.raw_close_field == c.volume_field)
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: distinct field roles");
  for (const auto& s : o.descriptor_recipes) if (!s.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: provider owns recipes");
  for (usize d = 0; d < t; ++d)
    if (!(c.mark_times_ns[d] < o.decision_times_ns[d]) ||
        (d + 1 < t && !(o.decision_times_ns[d] < c.mark_times_ns[d + 1])))
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: mark/decision chronology");
  return co::Ok();
}

struct Reader {
  virtual ~Reader() = default;
  u64 mapped_bytes{}, metadata_bytes{};
  virtual co::Status read(usize d, std::span<f64> close, std::span<f64> raw,
      std::span<f64> volume, std::span<u8> present, std::span<u8> member,
      i64& member_clock) = 0;
  virtual bool has_membership() const noexcept { return false; }
};
struct PanelReader final : Reader {
  const alpha::Panel& panel;
  alpha::FieldId close_id{}, raw_id{}, volume_id{};
  explicit PanelReader(const alpha::Panel& p) : panel(p) {}
  co::Status read(usize d, std::span<f64> close, std::span<f64> raw,
      std::span<f64> volume, std::span<u8> present, std::span<u8>, i64&) override {
    const std::array ids{close_id, raw_id, volume_id};
    const std::array outputs{close, raw, volume};
    for (usize k = 0; k < ids.size(); ++k) {
      const auto input = panel.field_cross_section(ids[k], d);
      for (usize i = 0; i < input.size(); ++i)
        outputs[k][i] = panel.in_universe(d, i) ? input[i] : kNaN;
    }
    for (usize i = 0; i < present.size(); ++i)
      present[i] = static_cast<u8>(panel.in_universe(d, i));
    return co::Ok();
  }
};
struct StoreReader final : Reader {
  const PanelStore& store;
  usize raw_id{}, volume_id{}, active_index{std::numeric_limits<usize>::max()};
  std::optional<PanelStoreChunk> chunk;
  explicit StoreReader(const PanelStore& s) : store(s) {}
  bool has_membership() const noexcept override { return true; }
  co::Status read(usize d, std::span<f64> close, std::span<f64> raw,
      std::span<f64> volume, std::span<u8> present, std::span<u8> member,
      i64& member_clock) override {
    const auto index = d / store.config().chunk_dates;
    if (index != active_index) {
      chunk.reset(); // release old mapping BEFORE acquiring the next
      ATX_TRY(auto next, store.open_chunk(index));
      chunk.emplace(std::move(next)); active_index = index;
    }
    const auto local = d - chunk->begin_date();
    ATX_TRY_VOID(chunk->read_exact_close_row(local, close));
    ATX_TRY_VOID(chunk->read_field_row(raw_id, local, raw));
    ATX_TRY_VOID(chunk->read_field_row(volume_id, local, volume));
    ATX_TRY(auto p, chunk->present(local)); ATX_TRY(auto m, chunk->tradable(local));
    ATX_TRY(auto clock, chunk->membership_decision_key(local));
    std::copy(p.begin(), p.end(), present.begin());
    std::copy(m.begin(), m.end(), member.begin()); member_clock = clock;
    return co::Ok();
  }
};

template<class T> bool row_shape(std::span<const T> v, usize n) {
  return v.empty() || v.size() == n;
}
co::Status check_row(const PriceExposureEvidenceRow& r, usize n, bool guard) {
  const auto& e = r.evidence;
  if (!e.raw_descriptors.empty() || !e.source_present.empty() ||
      !row_shape(e.market_cap_usd, n) || !row_shape(e.industry, n) ||
      !row_shape(e.identity, n) || !row_shape(e.member, n) ||
      !row_shape(e.membership_available_ns, n) || !row_shape(e.membership_qualified, n) ||
      !row_shape(e.presence_available_ns, n) || !row_shape(e.presence_qualified, n) ||
      !row_shape(r.bar_available_ns, n) || !row_shape(r.bar_qualified, n) ||
      (guard ? r.bad_return.size() != n : !r.bad_return.empty()))
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: evidence row shape");
  for (auto bits : {e.member, e.membership_qualified, e.presence_qualified,
                    r.bar_qualified, r.bad_return})
    for (auto v : bits) if (v > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: nonbinary evidence");
  return co::Ok();
}

struct Sum {
  f64 value{}, correction{}; usize count{}; bool reseed{};
  void add(f64 v) {
    const f64 y = v - correction, next = value + y;
    correction = (next - value) - y; value = next;
  }
  void replace(f64 old, f64 next) {
    if (std::isfinite(old)) {
      if (old > 0 && std::abs(value - old) <=
          128 * kEps * std::max(std::abs(value), old)) reseed = true;
      add(-old); --count;
    }
    if (std::isfinite(next)) { add(next); ++count; }
  }
};
struct Moments {
  usize count{};
  f64 x{}, y{}, xx{}, yy{}, xy{};
  bool reseed{};
  void add(f64 a, f64 b) {
    ++count; const f64 dx = a - x, dy = b - y;
    x += dx / static_cast<f64>(count); y += dy / static_cast<f64>(count);
    xx += dx * (a - x); yy += dy * (b - y); xy += dx * (b - y);
  }
  void remove(f64 a, f64 b) {
    if (count <= 1) { *this = {}; return; }
    const f64 dx = a - x, dy = b - y;
    --count; x -= dx / static_cast<f64>(count); y -= dy / static_cast<f64>(count);
    const f64 tx = dx * (a - x), ty = dy * (b - y);
    const f64 old_xx = xx, old_yy = yy;
    xx -= tx; yy -= ty; xy -= dx * (b - y);
    if (xx <= 128 * kEps * (std::abs(old_xx) + std::abs(tx)) ||
        yy <= 128 * kEps * (std::abs(old_yy) + std::abs(ty))) reseed = true;
  }
  bool stable() const {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(xx) &&
        std::isfinite(yy) && std::isfinite(xy) && xx >= 0 && yy >= 0;
  }
};
struct Weights {
  std::vector<f64> cap;
  PriceExposureDate proof;
  explicit Weights(usize n) : cap(n, kNaN) {}
};
void freeze_weights(Weights& w, const ExposureRowInput& e, i64 decision,
                    const ExposurePanelConfig& c) {
  w.proof = {}; std::fill(w.cap.begin(), w.cap.end(), kNaN);
  const bool members = parent(c, "membership") && !e.member.empty() &&
      !e.membership_available_ns.empty() && !e.membership_qualified.empty();
  const bool caps = parent(c, "cap") && !e.market_cap_usd.empty();
  const bool ids = parent(c, "identity") && !e.identity.empty();
  for (usize i = 0; i < w.cap.size(); ++i) {
    if (!members || !known(e.membership_available_ns[i], e.membership_qualified[i], decision)) {
      ++w.proof.unknown_membership; continue;
    }
    if (!e.member[i]) continue;
    ++w.proof.prior_known_members;
    const bool cap = caps && cap_valid(e.market_cap_usd[i], decision);
    const bool identity = ids && identity_valid(e.identity[i], decision);
    if (!cap) ++w.proof.unknown_cap;
    if (!identity) ++w.proof.unknown_identity;
    if (cap && identity) {
      w.cap[i] = e.market_cap_usd[i].value; ++w.proof.qualified_prior_caps;
    }
  }
}
PriceExposureDate market(const Weights& w, std::span<const f64> returns) {
  auto out = w.proof;
  if (out.unknown_membership) out.market_reasons |= PriceMarketUnknownMembership;
  if (out.unknown_cap) out.market_reasons |= PriceMarketUnknownCap;
  if (out.unknown_identity) out.market_reasons |= PriceMarketUnknownIdentity;
  if (out.prior_known_members < 2) out.market_reasons |= PriceMarketTooFewNames;
  f64 scale = 0;
  for (auto cap : w.cap) if (std::isfinite(cap)) scale = std::max(scale, cap);
  Sum denominator, admitted, numerator;
  for (usize i = 0; i < w.cap.size(); ++i) if (std::isfinite(w.cap[i])) {
    const auto weight = w.cap[i] / scale;
    if (!(weight > 0)) out.market_reasons |= PriceMarketNumericalRange;
    denominator.add(weight);
    if (!std::isfinite(returns[i])) { ++out.missing_market_returns; continue; }
    ++out.observed_market_returns; admitted.add(weight); numerator.add(weight * returns[i]);
  }
  if (out.missing_market_returns) out.market_reasons |= PriceMarketMissingReturn;
  if (!out.unknown_membership && !out.unknown_cap && !out.unknown_identity &&
      std::isfinite(denominator.value) && denominator.value > 0)
    out.admitted_cap_weight_fraction = admitted.value / denominator.value;
  if (!out.market_reasons) {
    const f64 value = numerator.value / denominator.value;
    if (std::isfinite(value)) out.market_return = value;
    else out.market_reasons |= PriceMarketNumericalRange;
  }
  return out;
}

std::array<std::string, kExposureRawDescriptorCount> recipes(const PriceExposureConfig& c) {
  const std::string common = "prior-known-cap-price-v1;on-time-own-decision;strict-full-support;"
      "original-calendar;f64-accumulation;guard=" + std::to_string(c.use_return_guard) +
      ";close=" + c.close_field + ";raw=" + c.raw_close_field + ";volume=" + c.volume_field +
      ";adjusted-ratio-v1;raw-usd-times-raw-shares-v1;";
  return {common + "log-mean-dollar-volume63;zero-volume-observed",
      common + "mean-abs-simple-return-per-usd63;zero-dollar-volume-unavailable",
      common + "intercept-beta252;prior-decision-cap-market;centered-moments-reseed32-v1",
      common + "population-daily-residual-sd252;intercept;centered-reseed32-v1",
      common + "log-close[d-21]-log-close[d-252];valid-interior-returns",
      common + "negative-log-close[d]/close[d-21];valid-interior-returns"};
}

co::Result<PriceExposureResult> build(Reader& reader, PriceExposureEvidenceSource& evidence,
                                     const PriceExposureConfig& c) {
  ATX_TRY_VOID(validate(c));
  const auto t = c.output.session_keys.size(), n = c.output.instrument_ids.size();
  Budget budget{c.max_working_bytes};
  // All provider vectors, temporary metadata copies, diagnostic retention and
  // allocator slack coexist with the builder. Ring geometry is fixed, not T*N*252.
  if (!budget.add(evidence.retained_bytes()) || !budget.add(reader.mapped_bytes) ||
      !budget.add(reader.metadata_bytes) || !budget.add(128ULL * 1024) ||
      !budget.add(t, sizeof(PriceExposureDate) + 256) ||
      !budget.add(n, (kCloseRing + kLong + 2 * kLiquid) * sizeof(f64) +
                        kCloseRing * sizeof(u32) + 16 * sizeof(f64) +
                        8 * sizeof(u8) + 4 * sizeof(i64) + sizeof(Moments) +
                        2 * sizeof(Sum) + 6 * sizeof(ExposureObservation)))
    return co::Err(co::ErrorCode::OutOfRange, "price exposures: combined provider budget");
  const u64 provider_bytes = budget.used;
  auto output = c.output; output.descriptor_recipes = recipes(c);
  output.max_working_bytes = c.max_working_bytes - provider_bytes;
  ATX_TRY(auto builder, ExposurePanelBuilder::create(output));
  std::vector<f64> closes(kCloseRing * n, kNaN), returns(kLong * n, kNaN);
  std::vector<f64> dollars(kLiquid * n, kNaN), amihuds(kLiquid * n, kNaN);
  std::vector<f64> markets(kLong, kNaN), close(n), raw(n), volume(n), step(n, kNaN);
  std::vector<u32> bad_prefix(kCloseRing * n), bad_count(n);
  std::vector<u8> present(n), source_member(n), effective_present(n);
  std::vector<i64> presence_clock(n), member_clock(n), input_clock(n);
  std::vector<Sum> dollar_sum(n), amihud_sum(n);
  std::vector<Moments> moments(n);
  std::vector<ExposureObservation> descriptors(6 * n);
  Weights weights(n);
  PriceExposureResult result; result.dates.reserve(t);
  result.provider_working_bytes = provider_bytes;
  result.mapped_chunk_bound_bytes = reader.mapped_bytes;
  for (usize d = 0; d < t; ++d) {
    ATX_TRY(auto row, evidence.read_date(d));
    ATX_TRY_VOID(check_row(row, n, c.use_return_guard));
    auto e = row.evidence;
    i64 source_member_clock{};
    ATX_TRY_VOID(reader.read(d, close, raw, volume, present, source_member, source_member_clock));
    const i64 decision = output.decision_times_ns[d], mark = c.mark_times_ns[d];
    if (reader.has_membership() && source_member_clock >= decision)
      return co::Err(co::ErrorCode::InvalidArgument, "price exposures: store membership clock");
    const bool have_presence = !e.presence_available_ns.empty() && !e.presence_qualified.empty();
    const bool have_bar = !row.bar_available_ns.empty() && !row.bar_qualified.empty();
    for (usize i = 0; i < n; ++i) {
      if (reader.has_membership() && !e.member.empty() && !e.membership_available_ns.empty() &&
          !e.membership_qualified.empty() &&
          known(e.membership_available_ns[i], e.membership_qualified[i], decision) &&
          e.member[i] != source_member[i])
        return co::Err(co::ErrorCode::InvalidArgument, "price exposures: store membership identity");
      // D6's historical decision is an additional floor, not a replacement for
      // the separately qualified evidence clock. Zero-member source rows may use 0.
      if (!e.membership_available_ns.empty())
        member_clock[i] = reader.has_membership() ?
            std::max(e.membership_available_ns[i], source_member_clock) : e.membership_available_ns[i];
      const bool seen = present[i] && have_presence &&
          known(e.presence_available_ns[i], e.presence_qualified[i], decision);
      if (have_bar && row.bar_qualified[i] && row.bar_available_ns[i] > 0 &&
          row.bar_available_ns[i] < mark)
        return co::Err(co::ErrorCode::InvalidArgument, "price exposures: bar before observed mark");
      const bool bar = seen && have_bar && row.bar_available_ns[i] >= mark &&
          known(row.bar_available_ns[i], row.bar_qualified[i], decision);
      input_clock[i] = bar ? std::max(row.bar_available_ns[i], e.presence_available_ns[i]) : 0;
      effective_present[i] = static_cast<u8>(seen);
      presence_clock[i] = have_presence ? e.presence_available_ns[i] : 0;
      const f64 value = bar && std::isfinite(close[i]) && close[i] > 0 ? close[i] : kNaN;
      const f64 previous = d ? closes[((d - 1) % kCloseRing) * n + i] : kNaN;
      step[i] = d && std::isfinite(value) && std::isfinite(previous) &&
          (!c.use_return_guard || !row.bad_return[i]) ? value / previous - 1 : kNaN;
      if (!std::isfinite(step[i])) { step[i] = kNaN; ++bad_count[i]; }
      closes[(d % kCloseRing) * n + i] = value;
      bad_prefix[(d % kCloseRing) * n + i] = bad_count[i];
      const f64 dv = bar && std::isfinite(raw[i]) && raw[i] > 0 &&
          std::isfinite(volume[i]) && volume[i] >= 0 ? raw[i] * volume[i] : kNaN;
      const f64 good_dv = std::isfinite(dv) && (dv > 0 || volume[i] == 0) ? dv : kNaN;
      const f64 a = good_dv > 0 && std::isfinite(step[i]) ? std::abs(step[i]) / good_dv : kNaN;
      const f64 good_a = std::isfinite(a) && (a > 0 || step[i] == 0) ? a : kNaN;
      const auto slot = (d % kLiquid) * n + i;
      dollar_sum[i].replace(dollars[slot], good_dv);
      amihud_sum[i].replace(amihuds[slot], good_a);
      dollars[slot] = good_dv; amihuds[slot] = good_a;
      if (d % 32 == 0 || dollar_sum[i].reseed || amihud_sum[i].reseed ||
          !std::isfinite(dollar_sum[i].value) ||
          !std::isfinite(amihud_sum[i].value) || dollar_sum[i].value < 0 ||
          amihud_sum[i].value < 0) {
        dollar_sum[i] = {}; amihud_sum[i] = {};
        const usize begin = d + 1 > kLiquid ? d + 1 - kLiquid : 0;
        for (usize s = begin; s <= d; ++s) {
          const auto index = (s % kLiquid) * n + i;
          dollar_sum[i].replace(kNaN, dollars[index]); amihud_sum[i].replace(kNaN, amihuds[index]);
        }
      }
    }
    if (!e.membership_available_ns.empty()) e.membership_available_ns = member_clock;
    PriceExposureDate diagnostics;
    if (d) diagnostics = market(weights, step);
    else diagnostics.market_reasons = PriceMarketNoPriorDate;
    if (std::isfinite(diagnostics.market_return))
      for (usize i = 0; i < n; ++i) if (std::isfinite(weights.cap[i]))
        diagnostics.market_available_at_ns = std::max(
            diagnostics.market_available_at_ns, input_clock[i]);
    const auto old_market = markets[d % kLong];
    markets[d % kLong] = diagnostics.market_return;
    for (usize i = 0; i < n; ++i) {
      const auto slot = (d % kLong) * n + i;
      auto& m = moments[i];
      if (std::isfinite(returns[slot]) && std::isfinite(old_market)) m.remove(returns[slot], old_market);
      if (std::isfinite(step[i]) && std::isfinite(diagnostics.market_return))
        m.add(step[i], diagnostics.market_return);
      returns[slot] = step[i];
      if (d % 32 == 0 || m.reseed || !m.stable()) {
        m = {}; const usize begin = d + 1 > kLong ? d + 1 - kLong : 0;
        for (usize s = begin; s <= d; ++s) {
          const auto a = returns[(s % kLong) * n + i], b = markets[s % kLong];
          if (std::isfinite(a) && std::isfinite(b)) m.add(a, b);
        }
      }
      std::array<f64, 6> values; values.fill(kNaN);
      if (dollar_sum[i].count == kLiquid && dollar_sum[i].value > 0 &&
          std::isfinite(dollar_sum[i].value))
        values[0] = std::log(dollar_sum[i].value / static_cast<f64>(kLiquid));
      if (amihud_sum[i].count == kLiquid && std::isfinite(amihud_sum[i].value))
        values[1] = amihud_sum[i].value / static_cast<f64>(kLiquid);
      if (m.count == kLong && m.stable() && m.yy > 0) {
        const f64 beta = m.xy / m.yy;
        const f64 explained = beta * m.xy;
        f64 residual = m.xx - explained;
        // Cancellation near a perfect fit uses the chronological centered
        // residuals directly; never clamp a negative subtraction to zero.
        if (!std::isfinite(residual) || residual <= 128 * kEps *
            std::max(std::abs(m.xx), std::abs(explained))) {
          Sum direct;
          for (usize s = d + 1 - kLong; s <= d; ++s) {
            const auto a = returns[(s % kLong) * n + i], b = markets[s % kLong];
            const f64 r = (a - m.x) - beta * (b - m.y); direct.add(r * r);
          }
          residual = direct.value;
        }
        if (std::isfinite(beta) && std::isfinite(residual) && residual >= 0) {
          values[2] = beta; values[3] = std::sqrt(residual / static_cast<f64>(kLong));
        }
      }
      const auto endpoint = [&](usize s) { return closes[(s % kCloseRing) * n + i]; };
      const auto prefix = [&](usize s) { return bad_prefix[(s % kCloseRing) * n + i]; };
      if (d >= kLong && prefix(d - 21) == prefix(d - kLong) &&
          std::isfinite(endpoint(d - 21)) && std::isfinite(endpoint(d - kLong)))
        values[4] = std::log(endpoint(d - 21)) - std::log(endpoint(d - kLong));
      if (d >= 21 && prefix(d) == prefix(d - 21) &&
          std::isfinite(endpoint(d)) && std::isfinite(endpoint(d - 21)))
        values[5] = std::log(endpoint(d - 21)) - std::log(endpoint(d));
      const bool clock = input_clock[i] > 0;
      for (usize k = 0; k < 6; ++k) {
        descriptors[k * n + i] = {};
        if (clock && std::isfinite(values[k])) {
          const auto available = k == 2 || k == 3 ?
              std::max(input_clock[i], diagnostics.market_available_at_ns) : input_clock[i];
          descriptors[k * n + i] = {values[k], mark, available, true};
          ++diagnostics.finite_raw_descriptors[k];
        }
      }
    }
    freeze_weights(weights, e, decision, output);
    e.raw_descriptors = descriptors; e.source_present = effective_present;
    if (!e.presence_available_ns.empty()) e.presence_available_ns = presence_clock;
    ATX_TRY_VOID(builder.append_date(d, e));
    result.dates.push_back(diagnostics);
  }
  ATX_TRY(auto panel, builder.finish()); result.panel = std::move(panel);
  return co::Ok(std::move(result));
}
} // namespace

co::Result<PriceExposureResult> build_price_exposures(const alpha::Panel& panel,
    PriceExposureEvidenceSource& evidence, const PriceExposureConfig& c) {
  ATX_TRY_VOID(validate(c));
  if (panel.dates() != c.output.session_keys.size() || panel.instruments() != c.output.instrument_ids.size())
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: Panel shape");
  PanelReader reader(panel);
  ATX_TRY(auto close, panel.field_id(c.close_field)); reader.close_id = close;
  ATX_TRY(auto raw, panel.field_id(c.raw_close_field)); reader.raw_id = raw;
  ATX_TRY(auto volume, panel.field_id(c.volume_field)); reader.volume_id = volume;
  // Numeric axes/basis/source SHA are caller assertions for plain Panel. The
  // output content hash binds produced values; no durable source authentication.
  return build(reader, evidence, c);
}

co::Result<PriceExposureResult> build_price_exposures(const PanelStore& store,
    PriceExposureEvidenceSource& evidence, const PriceExposureConfig& c) {
  ATX_TRY_VOID(validate(c));
  const auto& s = store.config();
  if (store.manifest_sha256() != c.price_source_sha256 || s.session_keys != c.output.session_keys ||
      s.instrument_ids != c.output.instrument_ids ||
      s.instrument_namespace != c.output.instrument_namespace ||
      !parent_is(c.output, "membership", s.membership_sha256) || c.close_field != "close")
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: D6 identity/axes");
  StoreReader reader(store);
  bool close_found = false, raw_found = false, volume_found = false;
  for (usize i = 0; i < s.fields.size(); ++i) {
    const auto& f = s.fields[i];
    if (f.name == c.close_field) close_found = f.basis == LevelBasis::AdjustedLevel;
    if (f.name == c.raw_close_field) { raw_found = f.basis == LevelBasis::Raw; reader.raw_id = i; }
    if (f.name == c.volume_field) { volume_found = f.basis == LevelBasis::Raw; reader.volume_id = i; }
  }
  if (!close_found || !raw_found || !volume_found)
    return co::Err(co::ErrorCode::InvalidArgument, "price exposures: D6 field bases");
  ATX_TRY(auto sizing, preflight_panel_store(s));
  reader.mapped_bytes = sizing.largest_chunk_bytes;
  reader.metadata_bytes = sizing.metadata_bound_bytes * 4 + 65536;
  return build(reader, evidence, c);
}
} // namespace atx::engine::data
