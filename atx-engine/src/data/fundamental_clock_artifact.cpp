#include "atx/engine/data/fundamental_clock_artifact.hpp"
#include "atx/engine/data/fundamental_fields.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace atx::engine::data::fundamentals {
namespace {
constexpr atx::i64 kSeal = 1'577'836'800'000'000'000LL;
constexpr atx::i64 kSecond = 1'000'000'000LL;
constexpr std::string_view kHeader =
    "sr_id\towner_id\tlink_id\tavailable_ns\tperiod_end_ns\tidentity_valid_from_ns\t"
    "identity_valid_to_ns\tlink_available_ns\tidentity_only\tlink_priority\t"
    "identity_retired_from_ns\tidentity_retired_available_ns\tfiled_ns\tclock_kind\t"
    "accepted_ns\tpublished_ns\trevision_available_ns\tfact_vintage_qualified\t"
    "knowledge_clock_qualified\tclock_policy";
struct Budget {
  atx::u64 maximum{}, used{};
  bool add(atx::u64 count, atx::u64 width) {
    if (used > maximum || (width && count > (maximum - used) / width)) return false;
    used += count * width;
    return true;
  }
};
bool integer(std::string_view s, atx::i64& value) {
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  return ec == std::errc{} && end == s.data() + s.size();
}
bool finite_clock(atx::i64 value) { return value >= 0 && value < kSeal; }
} // namespace

std::string_view fundamental_clock_admission_name(FundamentalClockAdmission rule) noexcept {
  switch (rule) {
  case FundamentalClockAdmission::ObservedPublicV1: return "observed-public-v1";
  case FundamentalClockAdmission::AllowModeledClockV2: return "allow-modeled-clock-v2";
  }
  return "invalid";
}

atx::core::Result<std::vector<PitRecord>> decode_qualified_interval_points(
    std::string_view text, std::span<const std::string> axis_ids,
    const QualifiedIntervalConfig& config, FundamentalClockAudit* audit) {
  namespace co = atx::core;
  const auto error = [] { return co::Err(co::ErrorCode::ParseError,
                                         "fundamentals: invalid qualified interval-v4 TSV"); };
  const auto budget_error = [] { return co::Err(co::ErrorCode::OutOfRange,
                                                "fundamentals: interval-v4 resource budget"); };
  if (config.admission != FundamentalClockAdmission::ObservedPublicV1 &&
      config.admission != FundamentalClockAdmission::AllowModeledClockV2)
    return co::Err(co::ErrorCode::InvalidArgument, "fundamentals: unknown clock admission rule");
  if (text.size() > 256ULL * 1024ULL * 1024ULL || config.max_rows == 0 ||
      config.max_rows > 1'000'000 || axis_ids.size() > 1'000'000) return budget_error();
  Budget budget{config.max_working_bytes, 0};
  if (!budget.add(1, 64U * 1024U) || !budget.add(text.size(), 1) ||
      !budget.add(axis_ids.size(), 128)) return budget_error();
  try {
    const auto line = [&text]() {
      const auto end = text.find('\n');
      const auto out = text.substr(0, end);
      text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
      return out;
    };
    if (line() != "ATX-FUNDAMENTAL-INTERVALS\t4") return error();
    std::string expected{kHeader};
    for (auto field : kRawFieldNames) { expected += '\t'; expected += field; }
    if (line() != expected) return error();
    std::unordered_map<std::string_view, atx::usize> axes;
    for (atx::usize i = 0; i < axis_ids.size(); ++i)
      if (axis_ids[i].empty() || !axes.emplace(axis_ids[i], i).second) return error();
    FundamentalClockAudit stats;
    std::vector<PitRecord> result;
    while (!text.empty()) {
      const auto row = line();
      if (row.empty() || row.size() > 8192 || ++stats.rows > config.max_rows) return error();
      std::array<std::string_view, 20 + kRawFieldCount> cells;
      auto rest = row;
      for (atx::usize c = 0; c < cells.size(); ++c) {
        const auto end = rest.find('\t');
        if ((c + 1 < cells.size()) != (end != std::string_view::npos)) return error();
        cells[c] = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
      }
      // Parse all rows, including absent axes, before deciding admission.
      std::array<atx::i64, 16> number{}; // columns 3..18
      for (atx::usize i = 0; i < number.size(); ++i)
        if (!integer(cells[i + 3], number[i])) return error();
      const auto marker = number[5], priority = number[6], filed = number[9], kind = number[10];
      const auto accepted = number[11], published = number[12], revision = number[13];
      const auto vintage = number[14], knowledge = number[15];
      if (cells[0].empty() || cells[1].empty() || cells[2].empty() ||
          marker < 0 || marker > 1 || priority < 0 || priority > 1 ||
          vintage < 0 || vintage > 1 || knowledge < 0 || knowledge > 1 ||
          number[2] < 0 || number[4] < 0 ||
          number[2] >= number[3] || number[1] > number[0] ||
          number[7] < 0 || number[8] < 0 ||
          ((number[7] == std::numeric_limits<atx::i64>::max()) !=
           (number[8] == std::numeric_limits<atx::i64>::max()))) return error();
      if (!finite_clock(number[0]) || !finite_clock(number[1]) ||
          !finite_clock(filed) || !finite_clock(accepted) ||
          !finite_clock(published) || !finite_clock(revision)) return error();
      if (marker) {
        if (number[0] || number[1] || filed || kind || accepted || published || revision ||
            vintage || knowledge || !cells[19].empty()) return error();
        ++stats.markers;
      } else {
        if (!filed || filed % kNanosPerDay != 0 || !number[1] ||
            (published && accepted && published < accepted) ||
            (revision && accepted && revision < accepted) ||
            (revision && published && revision < published)) return error();
        atx::i64 base{};
        if (kind == 1) {
          if (!accepted || !published || cells[19] != "accepted-public-v2") return error();
          base = published;
          ++stats.observed_public_rows;
        } else if (kind == 2) {
          constexpr std::string_view prefix = "acceptance-plus", suffix = "s-modeled-v2";
          const auto policy = cells[19];
          atx::i64 seconds{};
          if (!accepted || published || knowledge || !policy.starts_with(prefix) ||
              !policy.ends_with(suffix) || policy.size() <= prefix.size() + suffix.size() ||
              !integer(policy.substr(prefix.size(), policy.size() - prefix.size() - suffix.size()), seconds) ||
              seconds < 0 || seconds > 7 * 86400) return error();
          base = accepted + seconds * kSecond;
          ++stats.modeled_clock_rows;
        } else if (kind == 3) {
          if (accepted || knowledge || cells[19] != "filed-plus46h-modeled-v2") return error();
          base = filed + 46 * 3600 * kSecond;
          ++stats.modeled_clock_rows;
        } else return error();
        if (number[0] != std::max({base, published, revision})) return error();
        if (!vintage) ++stats.unqualified_vintage_rows;
        if (!knowledge) ++stats.unqualified_knowledge_clock_rows;
      }
      const bool admitted = !marker && vintage &&
          (config.admission == FundamentalClockAdmission::AllowModeledClockV2 || (kind == 1 && knowledge));
      PitRecord rec;
      rec.identity_rule = IdentityRule::DatedLinksV2;
      rec.available_ns = number[0]; rec.period_end_ns = number[1];
      rec.identity_valid_from_ns = number[2]; rec.identity_valid_to_ns = number[3];
      rec.link_available_ns = number[4]; rec.identity_only = marker != 0;
      rec.link_priority = static_cast<atx::u8>(priority);
      rec.identity_retired_from_ns = number[7]; rec.identity_retired_available_ns = number[8];
      rec.values.fill(std::numeric_limits<atx::f64>::quiet_NaN());
      for (atx::usize f = 0; f < kRawFieldCount; ++f) {
        const auto value = cells[20 + f];
        if (value.empty()) continue;
        atx::f64 parsed{};
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || end != value.data() + value.size() ||
            !std::isfinite(parsed) || marker) return error();
        if (admitted) rec.values[f] = parsed;
      }
      if (!marker) { if (admitted) ++stats.admitted_rows; else ++stats.withheld_rows; }
      const auto axis = axes.find(cells[0]);
      if (axis == axes.end()) { ++stats.absent_axis_rows; continue; }
      if (!budget.add(2, sizeof(PitRecord) + 128) ||
          !budget.add(cells[1].size() + cells[2].size(), 2)) return budget_error();
      rec.instrument = axis->second; rec.owner_id = cells[1]; rec.link_id = cells[2];
      result.push_back(std::move(rec));
    }
    if (audit) *audit = stats;
    return co::Ok(std::move(result));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::Unavailable, "fundamentals: interval-v4 allocation failed");
  }
}
} // namespace atx::engine::data::fundamentals
