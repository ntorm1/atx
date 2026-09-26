#pragma once

// D3 V4 admission only. Legacy interval V2/V3 types and numerical readers are
// unchanged. Input bytes must already be hash-validated against their manifest;
// source-declared evidence is not authenticated merely by decoding this TSV.
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data::fundamentals {
struct PitRecord;
enum class FundamentalClockAdmission : atx::u8 {
  ObservedPublicV1 = 1,
  AllowModeledClockV2 = 2 // never relaxes fact-vintage qualification
};
struct QualifiedIntervalConfig {
  FundamentalClockAdmission admission{FundamentalClockAdmission::ObservedPublicV1};
  atx::usize max_rows{1'000'000};
  atx::u64 max_working_bytes{256ULL * 1024ULL * 1024ULL};
};
struct FundamentalClockAudit {
  atx::usize rows{}, markers{}, admitted_rows{}, withheld_rows{};
  atx::usize observed_public_rows{}, modeled_clock_rows{}, unqualified_vintage_rows{};
  atx::usize unqualified_knowledge_clock_rows{}, absent_axis_rows{};
};
[[nodiscard]] std::string_view fundamental_clock_admission_name(FundamentalClockAdmission rule) noexcept;
// Numeric values on unqualified rows become NaN while independent identity
// intervals remain. Malformed clocks/geometry/numerics are errors, not missingness.
// Audit is assigned only on success. The budget estimates retained payload and
// allocator slack, not RSS. Include fundamental_fields.hpp to use PitRecord.
[[nodiscard]] atx::core::Result<std::vector<PitRecord>> decode_qualified_interval_points(
    std::string_view text, std::span<const std::string> axis_ids,
    const QualifiedIntervalConfig& config = {}, FundamentalClockAudit* audit = nullptr);
} // namespace atx::engine::data::fundamentals
