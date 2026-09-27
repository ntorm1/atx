#include "atx/engine/data/exposure_panel.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <utility>

#include "atx/core/sha256.hpp"

namespace atx::engine::data {
namespace {
using namespace atx;
namespace co = atx::core;
constexpr auto kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr auto kOpenEnd = std::numeric_limits<i64>::max();
constexpr usize kColumns = kExposureDescriptorCount;
constexpr u64 kSlack = 64U * 1024U;

struct Budget {
  u64 maximum{}, used{};
  bool add(u64 count, u64 width) noexcept {
    if (used > maximum || (width && count > (maximum - used) / width)) return false;
    used += count * width;
    return true;
  }
};
struct Hash {
  co::Sha256 state;
  co::Status word(u64 v) {
    std::array<std::byte, 8> bytes{};
    for (usize i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::byte>((v >> (8U * i)) & 255U);
    return state.update(bytes);
  }
  co::Status text(std::string_view s) {
    ATX_TRY_VOID(word(s.size()));
    return state.update(std::as_bytes(std::span(s.data(), s.size())));
  }
  co::Status number(f64 value) { return word(std::bit_cast<u64>(value)); }
  co::Result<std::string> finish() {
    ATX_TRY(auto bytes, state.finalize());
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (auto byte : bytes) {
      const auto value = std::to_integer<unsigned>(byte);
      result.push_back(hex[value >> 4U]); result.push_back(hex[value & 15U]);
    }
    return co::Ok(std::move(result));
  }
};
bool valid_hash(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool increasing(std::span<const i64> values) {
  return !values.empty() && values.front() > 0 &&
      std::adjacent_find(values.begin(), values.end(), std::greater_equal<i64>{}) == values.end();
}
bool has_parent(const ExposurePanelConfig& cfg, std::string_view role) {
  return std::any_of(cfg.parents.begin(), cfg.parents.end(), [&](const auto& p) { return p.role == role; });
}
co::Result<u64> validate_config(const ExposurePanelConfig& cfg) {
  const auto t = cfg.session_keys.size(), n = cfg.instrument_ids.size();
  if (t > 8192 || n > 32768 || !increasing(cfg.session_keys) || !increasing(cfg.instrument_ids) ||
      cfg.decision_times_ns.size() != t || !increasing(cfg.decision_times_ns) ||
      cfg.instrument_namespace.empty() || cfg.instrument_namespace.size() > 128 ||
      cfg.parents.size() > 32 ||
      (cfg.provenance != ExposureProvenance::SyntheticDeclaredV1 &&
       cfg.provenance != ExposureProvenance::PitDeclaredV1) ||
      cfg.normalization != ExposureNormalizationRule::RawWinsor16CapCenterV1)
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: axis/configuration");
  for (usize i = 0; i < cfg.parents.size(); ++i) {
    const auto& p = cfg.parents[i];
    if (p.role.empty() || p.role.size() > 128 || !valid_hash(p.sha256))
      return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: parent identity");
    for (usize j = 0; j < i; ++j) if (cfg.parents[j].role == p.role)
      return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: duplicate parent role");
  }
  Budget b{cfg.max_working_bytes};
  // Fixed numeric vectors, strings/metadata, control blocks and allocator slack.
  if (!b.add(kSlack, 1) || !b.add(t, 2 * sizeof(i64) + sizeof(ExposureDateSummary) + 128) ||
      !b.add(n, sizeof(i64)) || !b.add(t * n, kColumns * (sizeof(f64) + sizeof(u32)) +
                                    sizeof(f64) + sizeof(u32) + 2 * sizeof(u8)))
    return co::Err(co::ErrorCode::OutOfRange, "exposure panel: retained payload budget");
  for (const auto& recipe : cfg.descriptor_recipes) {
    if (recipe.empty() || recipe.size() > 16384)
      return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: descriptor recipe required");
    if (!b.add(recipe.size(), 2))
      return co::Err(co::ErrorCode::OutOfRange, "exposure panel: recipe budget");
  }
  for (const auto& p : cfg.parents) if (!b.add(p.role.size() + p.sha256.size() + 128, 2))
    return co::Err(co::ErrorCode::OutOfRange, "exposure panel: parent budget");
  const auto retained = b.used;
  // Reusable one-date transaction/normalization scratch and bounded hash state.
  if (!b.add(n, kColumns * (sizeof(f64) + sizeof(u32)) + sizeof(f64) + sizeof(u32) +
                    2 * sizeof(u8) + 4 * sizeof(f64)) || !b.add(kSlack, 1))
    return co::Err(co::ErrorCode::OutOfRange, "exposure panel: append scratch budget");
  return co::Ok(retained);
}
co::Result<std::string> axis_identity(const ExposurePanelConfig& cfg) {
  Hash h; ATX_TRY_VOID(h.text("exposure-axes-v1")); ATX_TRY_VOID(h.text(cfg.instrument_namespace));
  for (auto axis : {std::span(cfg.session_keys), std::span(cfg.decision_times_ns),
                    std::span(cfg.instrument_ids)}) {
    ATX_TRY_VOID(h.word(axis.size()));
    for (auto value : axis) ATX_TRY_VOID(h.word(static_cast<u64>(value)));
  }
  return h.finish();
}
co::Result<std::string> recipe_identity(const ExposurePanelConfig& cfg) {
  Hash h;
  ATX_TRY_VOID(h.text("exposure-panel-v1;scaled-moments-v1;equal-raw-winsor16-3sd;cap-center-equal-sd;no-final-clip"));
  ATX_TRY_VOID(h.word(static_cast<u64>(cfg.provenance)));
  ATX_TRY_VOID(h.word(static_cast<u64>(cfg.normalization)));
  for (const auto& recipe : cfg.descriptor_recipes) ATX_TRY_VOID(h.text(recipe));
  // Parent order is canonical by role; caller vector ordering cannot alter identity.
  std::vector<const ExposureParent*> ordered; ordered.reserve(cfg.parents.size());
  for (const auto& p : cfg.parents) ordered.push_back(&p);
  std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->role < b->role; });
  ATX_TRY_VOID(h.word(ordered.size()));
  for (const auto* p : ordered) { ATX_TRY_VOID(h.text(p->role)); ATX_TRY_VOID(h.text(p->sha256)); }
  return h.finish();
}
u32 interval_reason(const ExposureInterval& v, i64 decision) {
  if (!v.qualified) return ExposureUnqualified;
  if (v.available_at_ns <= 0 || v.valid_from_ns <= 0) return ExposureOutsideValidity;
  if (v.available_at_ns >= decision || v.valid_from_ns > decision) return ExposureNotYetAvailable;
  if (v.valid_to_ns != kOpenEnd && v.end_available_at_ns > 0 &&
      v.end_available_at_ns < decision && decision >= v.valid_to_ns) return ExposureOutsideValidity;
  return 0;
}
co::Status hash_interval(Hash& hash, const ExposureInterval& v, i64 decision, u32 reason) {
  ATX_TRY_VOID(hash.word(reason));
  if (reason) return co::Ok();
  ATX_TRY_VOID(hash.word(static_cast<u64>(v.valid_from_ns)));
  ATX_TRY_VOID(hash.word(static_cast<u64>(v.available_at_ns)));
  const bool end_known = v.end_available_at_ns > 0 && v.end_available_at_ns < decision;
  ATX_TRY_VOID(hash.word(static_cast<u64>(end_known ? v.valid_to_ns : kOpenEnd)));
  return hash.word(static_cast<u64>(end_known ? v.end_available_at_ns : kOpenEnd));
}
u32 observation_reason(const ExposureObservation& value, i64 decision) {
  if (!value.qualified) return ExposureUnqualified;
  if (value.available_at_ns <= 0 || value.observed_through_ns <= 0) return ExposureUnqualified;
  if (value.observed_through_ns > value.available_at_ns) return ExposureInvalidValue;
  if (value.available_at_ns >= decision || value.observed_through_ns >= decision)
    return ExposureNotYetAvailable;
  return std::isfinite(value.value) ? 0U : ExposureInvalidValue;
}
co::Status hash_observation(Hash& hash, const ExposureObservation& v, u32 reason) {
  ATX_TRY_VOID(hash.word(reason));
  if (reason) return co::Ok();
  ATX_TRY_VOID(hash.number(v.value));
  ATX_TRY_VOID(hash.word(static_cast<u64>(v.observed_through_ns)));
  return hash.word(static_cast<u64>(v.available_at_ns));
}
// No cap fallback. Scale raw values and caps first so valid finite economic
// inputs cannot overflow moment sums. Scaling is part of this new recipe.
co::Status normalize(std::span<f64> values, std::span<const f64> caps,
                     ExposureColumnSummary& summary) {
  f64 scale = 0, cap_scale = 0;
  for (usize i = 0; i < values.size(); ++i) if (std::isfinite(values[i])) {
    ++summary.finite_members;
    scale = std::max(scale, std::abs(values[i])); cap_scale = std::max(cap_scale, caps[i]);
  }
  if (!summary.finite_members) return co::Ok();
  if (!(cap_scale > 0) || !std::isfinite(cap_scale))
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: normalization cap");
  if (scale > 0) for (auto& v : values) if (std::isfinite(v)) v /= scale;
  f64 equal_mean = 0, cap_mean = 0, sd = 0;
  const auto moments = [&]() {
    f64 total = 0, weighted = 0, weights = 0;
    for (usize i = 0; i < values.size(); ++i) if (std::isfinite(values[i])) {
      const auto weight = caps[i] / cap_scale;
      total += values[i]; weighted += weight * values[i]; weights += weight;
    }
    equal_mean = total / static_cast<f64>(summary.finite_members); cap_mean = weighted / weights;
    f64 squares = 0;
    for (auto value : values) if (std::isfinite(value)) {
      const auto delta = value - equal_mean; squares += delta * delta;
    }
    sd = std::sqrt(squares / static_cast<f64>(summary.finite_members));
  };
  for (u32 pass = 0; pass < 16; ++pass) {
    moments(); if (!(sd > 0)) break;
    const auto low = equal_mean - 3 * sd, high = equal_mean + 3 * sd;
    bool clipped = false;
    for (auto& value : values) if (std::isfinite(value)) {
      const auto bounded = std::clamp(value, low, high);
      clipped = clipped || value != bounded; value = bounded;
    }
    if (!clipped) break;
    ++summary.winsor_passes;
  }
  moments();
  if (!std::isfinite(equal_mean) || !std::isfinite(cap_mean) || !std::isfinite(sd))
    return co::Err(co::ErrorCode::OutOfRange, "exposure panel: normalization overflow");
  summary.degenerate = !(sd > 0) || summary.finite_members <= 1;
  for (auto& value : values) if (std::isfinite(value)) {
    value = summary.degenerate ? 0 : (value - cap_mean) / sd;
    if (!std::isfinite(value))
      return co::Err(co::ErrorCode::OutOfRange, "exposure panel: standardized value overflow");
  }
  return co::Ok();
}
template<class T> bool optional_shape(std::span<const T> values, usize n) {
  return values.empty() || values.size() == n;
}
co::Status validate_row(const ExposureRowInput& row, usize n) {
  if (!optional_shape(row.raw_descriptors, kExposureRawDescriptorCount * n) ||
      !optional_shape(row.market_cap_usd, n) || !optional_shape(row.industry, n) ||
      !optional_shape(row.identity, n) || !optional_shape(row.source_present, n) ||
      !optional_shape(row.member, n) || !optional_shape(row.presence_available_ns, n) ||
      !optional_shape(row.membership_available_ns, n) || !optional_shape(row.presence_qualified, n) ||
      !optional_shape(row.membership_qualified, n))
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: row geometry");
  for (auto mask : {row.source_present, row.member, row.presence_qualified, row.membership_qualified})
    for (auto value : mask) if (value > 1)
      return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: nonbinary mask");
  const auto interval_valid = [](const ExposureInterval& v) {
    return v.valid_to_ns == kOpenEnd ||
        (v.valid_to_ns > v.valid_from_ns && v.end_available_at_ns > 0);
  };
  for (const auto& v : row.identity) if (!interval_valid(v))
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: identity validity geometry");
  for (const auto& v : row.industry) if (!interval_valid(v.validity))
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: industry validity geometry");
  return co::Ok();
}
u32 mask_reason(std::span<const u8> mask, std::span<const i64> clocks,
                std::span<const u8> qualified, usize i, i64 decision, bool parent) {
  if (!parent || mask.empty() || clocks.empty() || qualified.empty() || !qualified[i] || clocks[i] <= 0)
    return ExposureUnqualified;
  return clocks[i] < decision ? 0U : ExposureNotYetAvailable;
}
} // namespace

struct ExposurePanel::Impl {
  ExposurePanelConfig cfg;
  std::string axis_hash, content_hash, recipe_hash;
  std::vector<f64> values, caps; // date-major; values descriptor-major within date
  std::vector<u32> reasons, industry_reasons;
  std::vector<u8> membership, industry; // membership0=knownfalse,1=knowntrue,2=unknown
  std::vector<ExposureDateSummary> summaries;
  std::vector<std::string> evidence_hashes;
  u64 retained_bytes{};
};
struct ExposurePanelBuilder::Impl {
  std::shared_ptr<ExposurePanel::Impl> panel;
  std::vector<f64> values, caps;
  std::vector<u32> reasons, industry_reasons;
  std::vector<u8> membership, industry;
  usize next{};
};

std::string_view exposure_descriptor_name(ExposureDescriptor field) noexcept {
  constexpr std::array<std::string_view, kColumns> names{
      "log_size", "log_adv63", "amihud63", "beta252", "residual_vol252",
      "momentum252_skip21", "short_reversal21"};
  const auto index = static_cast<usize>(field);
  return index < names.size() ? names[index] : std::string_view{};
}
ExposurePanel::ExposurePanel(std::shared_ptr<const Impl> p) : impl_(std::move(p)) {}
usize ExposurePanel::dates() const noexcept { return impl_ ? impl_->cfg.session_keys.size() : 0; }
usize ExposurePanel::instruments() const noexcept { return impl_ ? impl_->cfg.instrument_ids.size() : 0; }
std::span<const i64> ExposurePanel::session_keys() const noexcept {
  return impl_ ? std::span<const i64>(impl_->cfg.session_keys) : std::span<const i64>{};
}
std::span<const i64> ExposurePanel::decision_times_ns() const noexcept {
  return impl_ ? std::span<const i64>(impl_->cfg.decision_times_ns) : std::span<const i64>{};
}
std::span<const i64> ExposurePanel::instrument_ids() const noexcept {
  return impl_ ? std::span<const i64>(impl_->cfg.instrument_ids) : std::span<const i64>{};
}
std::string_view ExposurePanel::instrument_namespace() const noexcept {
  return impl_ ? std::string_view(impl_->cfg.instrument_namespace) : std::string_view{};
}
std::span<const ExposureParent> ExposurePanel::parents() const noexcept {
  return impl_ ? std::span<const ExposureParent>(impl_->cfg.parents) : std::span<const ExposureParent>{};
}
std::string_view ExposurePanel::axis_sha256() const noexcept { return impl_ ? impl_->axis_hash : std::string_view{}; }
std::string_view ExposurePanel::content_sha256() const noexcept { return impl_ ? impl_->content_hash : std::string_view{}; }
std::string_view ExposurePanel::recipe_sha256() const noexcept { return impl_ ? impl_->recipe_hash : std::string_view{}; }
u64 ExposurePanel::bytes() const noexcept { return impl_ ? impl_->retained_bytes : 0; }
co::Result<ExposureDateSummary> ExposurePanel::date_summary(usize date) const {
  if (!impl_ || date >= dates()) return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: date bound");
  return co::Ok(impl_->summaries[date]);
}
ExposurePanelBuilder::ExposurePanelBuilder(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
ExposurePanelBuilder::ExposurePanelBuilder(ExposurePanelBuilder&&) noexcept = default;
ExposurePanelBuilder& ExposurePanelBuilder::operator=(ExposurePanelBuilder&&) noexcept = default;
ExposurePanelBuilder::~ExposurePanelBuilder() = default;
co::Result<ExposurePanelBuilder> ExposurePanelBuilder::create(const ExposurePanelConfig& cfg) {
  ATX_TRY(auto bytes, validate_config(cfg));
  ATX_TRY(auto axis, axis_identity(cfg)); ATX_TRY(auto recipe, recipe_identity(cfg));
  auto impl = std::make_unique<Impl>(); impl->panel = std::make_shared<ExposurePanel::Impl>();
  auto& p = *impl->panel; p.cfg = cfg; p.retained_bytes = bytes;
  p.axis_hash = std::move(axis); p.recipe_hash = std::move(recipe);
  const auto t = cfg.session_keys.size(), n = cfg.instrument_ids.size(), cells = t * n;
  p.values.assign(cells * kColumns, kNaN); p.caps.assign(cells, kNaN);
  p.reasons.resize(cells * kColumns); p.industry_reasons.resize(cells);
  p.membership.resize(cells); p.industry.resize(cells); p.summaries.resize(t); p.evidence_hashes.resize(t);
  impl->values.resize(n * kColumns); impl->caps.resize(n); impl->reasons.resize(n * kColumns);
  impl->industry_reasons.resize(n); impl->membership.resize(n); impl->industry.resize(n);
  return co::Ok(ExposurePanelBuilder(std::move(impl)));
}

co::Status ExposurePanelBuilder::append_date(usize date, const ExposureRowInput& row) {
  if (!impl_ || !impl_->panel || date != impl_->next || date >= impl_->panel->cfg.session_keys.size())
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: append order/state");
  auto& b = *impl_; auto& p = *b.panel; const auto& cfg = p.cfg;
  const auto n = cfg.instrument_ids.size(); const auto decision = cfg.decision_times_ns[date];
  ATX_TRY_VOID(validate_row(row, n));
  std::fill(b.values.begin(), b.values.end(), kNaN); std::fill(b.caps.begin(), b.caps.end(), kNaN);
  std::fill(b.industry.begin(), b.industry.end(), u8{0});
  ExposureDateSummary summary; Hash evidence; ATX_TRY_VOID(evidence.text("dated-exposure-evidence-v1"));
  const bool cap_parent = has_parent(cfg, "cap"), industry_parent = has_parent(cfg, "industry");
  const bool identity_parent = has_parent(cfg, "identity"), prices_parent = has_parent(cfg, "prices");
  const bool member_parent = has_parent(cfg, "membership");
  for (usize i = 0; i < n; ++i) {
    const auto member_reason = mask_reason(row.member, row.membership_available_ns,
        row.membership_qualified, i, decision, member_parent);
    b.membership[i] = member_reason ? 2 : row.member[i];
    ATX_TRY_VOID(evidence.word(member_reason)); ATX_TRY_VOID(evidence.word(b.membership[i]));
    if (!member_reason) ATX_TRY_VOID(evidence.word(static_cast<u64>(row.membership_available_ns[i])));
    if (member_reason) ++summary.unknown_membership_names;
    else if (b.membership[i]) ++summary.known_member_names;
    u32 common = member_reason ? (ExposureUnknownMembership | member_reason) :
        (b.membership[i] ? 0U : ExposureNotMember);
    const auto identity_reason = !identity_parent || row.identity.empty() ? ExposureUnqualified :
        interval_reason(row.identity[i], decision);
    if (identity_reason) common |= ExposureMissingIdentity | identity_reason;
    if (row.identity.empty()) ATX_TRY_VOID(evidence.word(identity_reason));
    else ATX_TRY_VOID(hash_interval(evidence, row.identity[i], decision, identity_reason));
    auto presence_reason = mask_reason(row.source_present, row.presence_available_ns,
        row.presence_qualified, i, decision, prices_parent);
    if (!presence_reason && !row.source_present[i]) presence_reason = ExposureMissingPresence;
    if (presence_reason) common |= ExposureMissingPresence | presence_reason;
    ATX_TRY_VOID(evidence.word(presence_reason));
    if (!presence_reason) ATX_TRY_VOID(evidence.word(static_cast<u64>(row.presence_available_ns[i])));
    auto cap_reason = !cap_parent || row.market_cap_usd.empty() ? ExposureUnqualified :
        observation_reason(row.market_cap_usd[i], decision);
    if (!cap_reason && !(row.market_cap_usd[i].value > 0)) cap_reason = ExposureInvalidValue;
    if (cap_reason) common |= ExposureMissingCap | cap_reason;
    if (row.market_cap_usd.empty()) ATX_TRY_VOID(evidence.word(cap_reason));
    else ATX_TRY_VOID(hash_observation(evidence, row.market_cap_usd[i], cap_reason));
    if (!common) b.caps[i] = row.market_cap_usd[i].value;
    b.reasons[i] = common;
    if (!common) b.values[i] = std::log(b.caps[i]);
    for (usize f = 1; f < kColumns; ++f) {
      const auto raw_index = (f - 1) * n + i;
      auto raw_reason = row.raw_descriptors.empty() ? ExposureUnqualified :
          observation_reason(row.raw_descriptors[raw_index], decision);
      if (!raw_reason && (f == static_cast<usize>(ExposureDescriptor::Amihud63) ||
                          f == static_cast<usize>(ExposureDescriptor::ResidualVol252)) &&
          row.raw_descriptors[raw_index].value < 0) raw_reason = ExposureInvalidValue;
      b.reasons[f * n + i] = common | (raw_reason ? ExposureMissingDescriptor | raw_reason : 0U);
      if (!b.reasons[f * n + i]) b.values[f * n + i] = row.raw_descriptors[raw_index].value;
      if (row.raw_descriptors.empty()) ATX_TRY_VOID(evidence.word(raw_reason));
      else ATX_TRY_VOID(hash_observation(evidence, row.raw_descriptors[raw_index], raw_reason));
    }
    auto industry_reason = !industry_parent || row.industry.empty() ? ExposureUnqualified :
        interval_reason(row.industry[i].validity, decision);
    if (!industry_reason && (row.industry[i].ff49 == 0 || row.industry[i].ff49 > 49))
      industry_reason = ExposureInvalidValue;
    b.industry_reasons[i] = common | (industry_reason ? ExposureMissingIndustry | industry_reason : 0U);
    if (!b.industry_reasons[i]) b.industry[i] = row.industry[i].ff49;
    if (row.industry.empty()) ATX_TRY_VOID(evidence.word(industry_reason));
    else {
      ATX_TRY_VOID(hash_interval(evidence, row.industry[i].validity, decision, industry_reason));
      if (!industry_reason) ATX_TRY_VOID(evidence.word(row.industry[i].ff49));
    }
  }
  for (usize f = 0; f < kColumns; ++f)
    ATX_TRY_VOID(normalize(std::span(b.values).subspan(f * n, n), b.caps, summary.columns[f]));
  bool complete = summary.unknown_membership_names == 0;
  for (usize i = 0; i < n; ++i) if (b.membership[i] == 1) {
    if (b.industry_reasons[i]) complete = false;
    for (usize f = 0; f < kColumns; ++f) if (b.reasons[f * n + i]) complete = false;
  }
  summary.completeness = !summary.known_member_names ? ExposureCompleteness::NoKnownMembers :
      (complete ? ExposureCompleteness::Complete : ExposureCompleteness::Partial);
  ATX_TRY(auto evidence_hash, evidence.finish());
  // Commit only after all validation, hashing and numeric checks succeeded.
  const auto offset = date * n;
  std::copy(b.values.begin(), b.values.end(), p.values.begin() + static_cast<std::ptrdiff_t>(offset * kColumns));
  std::copy(b.reasons.begin(), b.reasons.end(), p.reasons.begin() + static_cast<std::ptrdiff_t>(offset * kColumns));
  std::copy(b.caps.begin(), b.caps.end(), p.caps.begin() + static_cast<std::ptrdiff_t>(offset));
  std::copy(b.membership.begin(), b.membership.end(), p.membership.begin() + static_cast<std::ptrdiff_t>(offset));
  std::copy(b.industry.begin(), b.industry.end(), p.industry.begin() + static_cast<std::ptrdiff_t>(offset));
  std::copy(b.industry_reasons.begin(), b.industry_reasons.end(), p.industry_reasons.begin() + static_cast<std::ptrdiff_t>(offset));
  p.summaries[date] = summary; p.evidence_hashes[date] = std::move(evidence_hash); ++b.next;
  return co::Ok();
}
co::Result<ExposurePanel> ExposurePanelBuilder::finish() {
  if (!impl_ || !impl_->panel || impl_->next != impl_->panel->cfg.session_keys.size())
    return co::Err(co::ErrorCode::InvalidArgument, "exposure panel: unfinished builder");
  auto& p = *impl_->panel; Hash h;
  ATX_TRY_VOID(h.text("immutable-exposure-panel-v1")); ATX_TRY_VOID(h.text(p.axis_hash));
  ATX_TRY_VOID(h.text(p.recipe_hash));
  for (const auto& value : p.evidence_hashes) ATX_TRY_VOID(h.text(value));
  for (auto value : p.values) ATX_TRY_VOID(h.number(value));
  for (auto value : p.caps) ATX_TRY_VOID(h.number(value));
  for (auto value : p.reasons) ATX_TRY_VOID(h.word(value));
  for (auto value : p.industry_reasons) ATX_TRY_VOID(h.word(value));
  for (auto value : p.membership) ATX_TRY_VOID(h.word(value));
  for (auto value : p.industry) ATX_TRY_VOID(h.word(value));
  ATX_TRY(p.content_hash, h.finish());
  std::shared_ptr<const ExposurePanel::Impl> frozen = std::move(impl_->panel);
  impl_.reset(); return co::Ok(ExposurePanel(std::move(frozen)));
}

co::Result<ExposureCrossSection> extract_exposure_date(const ExposurePanel& panel, usize date,
    std::string_view axis_hash, std::string_view content_hash, i64 decision,
    const ExposureExtractConfig& cfg) {
  if (!panel.impl_ || date >= panel.dates() || panel.axis_sha256() != axis_hash ||
      panel.content_sha256() != content_hash || panel.decision_times_ns()[date] != decision ||
      cfg.descriptors.empty() || cfg.descriptors.size() > kColumns || !cfg.min_names ||
      !std::isfinite(cfg.min_member_fraction) || cfg.min_member_fraction < 0 || cfg.min_member_fraction > 1 ||
      (cfg.missing != MissingExposurePolicy::RefuseIncomplete &&
       cfg.missing != MissingExposurePolicy::DropUnavailableNames))
    return co::Err(co::ErrorCode::InvalidArgument, "exposure extraction: identity/clock/configuration");
  for (usize j = 0; j < cfg.descriptors.size(); ++j)
    if (static_cast<usize>(cfg.descriptors[j]) >= kColumns ||
        (j && cfg.descriptors[j] <= cfg.descriptors[j - 1]))
      return co::Err(co::ErrorCode::InvalidArgument, "exposure extraction: descriptor order");
  const auto& p = *panel.impl_; const auto n = panel.instruments(), offset = date * n;
  if (!cfg.allow_synthetic && p.cfg.provenance == ExposureProvenance::SyntheticDeclaredV1)
    return co::Err(co::ErrorCode::Unavailable, "exposure extraction: synthetic evidence not admitted");
  Budget budget{cfg.max_working_bytes, panel.bytes()};
  if (!budget.add(kSlack, 1) || !budget.add(n, cfg.descriptors.size() * sizeof(f64) +
      sizeof(usize) + sizeof(i64) + sizeof(f64) + sizeof(u8)))
    return co::Err(co::ErrorCode::OutOfRange, "exposure extraction: retained plus result budget");
  const auto& summary = p.summaries[date];
  const auto reasons_for = [&](usize i) {
    u32 reasons = p.reasons[offset * kColumns + i]; // cap/identity/presence/membership always required
    for (auto field : cfg.descriptors)
      reasons |= p.reasons[offset * kColumns + static_cast<usize>(field) * n + i];
    if (cfg.require_ff49) reasons |= p.industry_reasons[offset + i];
    return reasons;
  };
  usize admitted = 0;
  for (usize i = 0; i < n; ++i) if (p.membership[offset + i] == 1 && !reasons_for(i)) ++admitted;
  if (admitted < cfg.min_names || !summary.known_member_names ||
      static_cast<f64>(admitted) < cfg.min_member_fraction * static_cast<f64>(summary.known_member_names) ||
      (cfg.missing == MissingExposurePolicy::RefuseIncomplete &&
       (admitted != summary.known_member_names || summary.unknown_membership_names)))
    return co::Err(co::ErrorCode::Unavailable, "exposure extraction: incomplete prerequisite/support");
  ExposureCrossSection result;
  result.original_slots.reserve(admitted); result.security_ids.reserve(admitted);
  result.values.reserve(admitted * cfg.descriptors.size()); result.market_cap_usd.reserve(admitted);
  result.ff49.reserve(admitted); result.descriptors = cfg.descriptors;
  result.known_member_names = summary.known_member_names;
  result.unknown_membership_names = summary.unknown_membership_names;
  result.decision_ns = decision; result.axis_sha256 = p.axis_hash;
  result.content_sha256 = p.content_hash; result.recipe_sha256 = p.recipe_hash;
  result.completeness = admitted == summary.known_member_names && !summary.unknown_membership_names ?
      ExposureCompleteness::Complete : ExposureCompleteness::Partial;
  for (auto field : cfg.descriptors) if (summary.columns[static_cast<usize>(field)].degenerate)
    result.degenerate_columns.push_back(field);
  for (usize i = 0; i < n; ++i) {
    const auto reasons = reasons_for(i);
    if (p.membership[offset + i] != 1 || reasons) {
      // Known nonmembers are outside the exposure denominator, not missing names.
      if (p.membership[offset + i] == 0) continue;
      for (usize bit = 0; bit < kExposureMissingReasonCount; ++bit)
        if (reasons & (u32{1} << bit)) ++result.omission_counts[bit];
      continue;
    }
    result.original_slots.push_back(i); result.security_ids.push_back(p.cfg.instrument_ids[i]);
    result.market_cap_usd.push_back(p.caps[offset + i]); result.ff49.push_back(p.industry[offset + i]);
    for (auto field : cfg.descriptors)
      result.values.push_back(p.values[offset * kColumns + static_cast<usize>(field) * n + i]);
  }
  return co::Ok(std::move(result));
}
} // namespace atx::engine::data
