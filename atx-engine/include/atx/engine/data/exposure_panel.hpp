#pragma once

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

// Fixed continuous column identities; FF49 is a separate categorical axis.
enum class ExposureDescriptor : atx::u8 {
  LogSize, LogAdv63, Amihud63, Beta252, ResidualVol252,
  Momentum252Skip21, ShortReversal21, Count
};
inline constexpr atx::usize kExposureDescriptorCount = 7;
inline constexpr atx::usize kExposureRawDescriptorCount = 6;
[[nodiscard]] std::string_view exposure_descriptor_name(ExposureDescriptor) noexcept;

enum class ExposureProvenance : atx::u8 { SyntheticDeclaredV1 = 1, PitDeclaredV1 = 2 };
enum class ExposureNormalizationRule : atx::u8 { RawWinsor16CapCenterV1 = 1 };
enum class MissingExposurePolicy : atx::u8 { RefuseIncomplete = 1, DropUnavailableNames = 2 };
enum class ExposureCompleteness : atx::u8 { Complete = 1, Partial = 2, NoKnownMembers = 3 };
enum ExposureMissingReason : atx::u32 {
  ExposureUnknownMembership = 1U, ExposureNotMember = 2U,
  ExposureMissingIdentity = 4U, ExposureMissingPresence = 8U,
  ExposureMissingCap = 16U, ExposureMissingDescriptor = 32U,
  ExposureMissingIndustry = 64U, ExposureNotYetAvailable = 128U,
  ExposureUnqualified = 256U, ExposureInvalidValue = 512U,
  ExposureOutsideValidity = 1024U
};
inline constexpr atx::usize kExposureMissingReasonCount = 11;

// Both clocks are supplied evidence, not inferred from a session label.
// Require observed_through <= available_at < decision.
// qualified is a caller assertion; this in-memory builder authenticates no feed.
struct ExposureObservation {
  atx::f64 value{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::i64 observed_through_ns{};
  atx::i64 available_at_ns{};
  bool qualified{false};
};
struct ExposureInterval {
  atx::i64 valid_from_ns{};
  atx::i64 available_at_ns{};
  atx::i64 valid_to_ns{std::numeric_limits<atx::i64>::max()};
  // A subsequently learned closure cannot rewrite a past snapshot. Apply a
  // finite valid_to only after this independent knowledge clock is reached.
  atx::i64 end_available_at_ns{std::numeric_limits<atx::i64>::max()};
  bool qualified{false};
};
struct ExposureIndustryObservation {
  atx::u8 ff49{}; // 1..49; zero means absent, never an "other" industry
  ExposureInterval validity;
};
struct ExposureParent { std::string role, sha256; };
struct ExposurePanelConfig {
  std::vector<atx::i64> session_keys; // complete increasing date labels
  std::vector<atx::i64> decision_times_ns; // increasing, same date-axis length
  std::vector<atx::i64> instrument_ids; // complete sorted positive security IDs
  std::string instrument_namespace;
  // Exact recipes for the six supplied raw descriptors, in enum order after
  // LogSize: log USD ADV63; Amihud63 (return/USD); cap-market beta252;
  // daily residual sd252; log-return252 excluding latest21; negative log-return21.
  std::array<std::string, kExposureRawDescriptorCount> descriptor_recipes;
  std::vector<ExposureParent> parents; // completeness needs identity,membership,prices,cap,industry roles
  ExposureProvenance provenance{ExposureProvenance::SyntheticDeclaredV1};
  ExposureNormalizationRule normalization{ExposureNormalizationRule::RawWinsor16CapCenterV1};
  atx::u64 max_working_bytes{256ULL * 1024 * 1024};
};

// Each optional span is empty (unavailable prerequisite) or its exact stated
// size. No broadcasting. Membership/presence are independent known-at-decision
// masks, with clocks and qualifications. Future mask values never affect rows.
struct ExposureRowInput {
  std::span<const ExposureObservation> raw_descriptors{}; // descriptor-major [6*N]
  std::span<const ExposureObservation> market_cap_usd{}; // [N], true PIT cap only
  std::span<const ExposureIndustryObservation> industry{}; // [N]
  std::span<const ExposureInterval> identity{}; // [N], security/issuer link evidence
  std::span<const atx::u8> source_present{}, member{}; // [N], 0/1
  std::span<const atx::i64> presence_available_ns{}, membership_available_ns{}; // [N]
  std::span<const atx::u8> presence_qualified{}, membership_qualified{}; // [N],0/1
};
struct ExposureColumnSummary {
  atx::usize finite_members{};
  atx::u32 winsor_passes{};
  bool degenerate{};
};
struct ExposureDateSummary {
  ExposureCompleteness completeness{ExposureCompleteness::NoKnownMembers};
  atx::usize known_member_names{}, unknown_membership_names{};
  std::array<ExposureColumnSummary, kExposureDescriptorCount> columns{};
};
struct ExposureExtractConfig {
  std::vector<ExposureDescriptor> descriptors{
      ExposureDescriptor::LogSize, ExposureDescriptor::LogAdv63,
      ExposureDescriptor::Amihud63, ExposureDescriptor::Beta252,
      ExposureDescriptor::ResidualVol252, ExposureDescriptor::Momentum252Skip21,
      ExposureDescriptor::ShortReversal21};
  bool require_ff49{true};
  MissingExposurePolicy missing{MissingExposurePolicy::RefuseIncomplete};
  atx::usize min_names{2};
  atx::f64 min_member_fraction{1};
  bool allow_synthetic{false};
  atx::u64 max_working_bytes{256ULL * 1024 * 1024}; // panel retained bytes + owned result
};
struct ExposureCrossSection {
  std::vector<atx::usize> original_slots;
  std::vector<atx::i64> security_ids;
  std::vector<ExposureDescriptor> descriptors;
  std::vector<atx::f64> values; // row-major [admitted][descriptors.size()]
  std::vector<atx::f64> market_cap_usd; // finite positive, raw USD
  std::vector<atx::u8> ff49; // categorical, no implicit contrasts/intercept
  atx::usize known_member_names{}, unknown_membership_names{};
  std::array<atx::usize, kExposureMissingReasonCount> omission_counts{}; // joint reasons
  std::vector<ExposureDescriptor> degenerate_columns;
  atx::i64 decision_ns{};
  std::string axis_sha256, content_sha256, recipe_sha256;
  ExposureCompleteness completeness{ExposureCompleteness::NoKnownMembers};
};

// Cheap immutable shared ownership. Retaining a copy keeps accessor spans alive.
// Default/moved-from objects expose empty axes and are refused by extraction.
class ExposurePanel {
public:
  ExposurePanel() = default;
  [[nodiscard]] atx::usize dates() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  [[nodiscard]] std::span<const atx::i64> session_keys() const noexcept;
  [[nodiscard]] std::span<const atx::i64> decision_times_ns() const noexcept;
  [[nodiscard]] std::span<const atx::i64> instrument_ids() const noexcept;
  [[nodiscard]] std::string_view instrument_namespace() const noexcept;
  [[nodiscard]] std::span<const ExposureParent> parents() const noexcept;
  [[nodiscard]] std::string_view axis_sha256() const noexcept;
  [[nodiscard]] std::string_view content_sha256() const noexcept;
  [[nodiscard]] std::string_view recipe_sha256() const noexcept;
  [[nodiscard]] atx::u64 bytes() const noexcept; // retained payload + declared allocation slack, not RSS
  [[nodiscard]] atx::core::Result<ExposureDateSummary> date_summary(atx::usize date) const;
private:
  struct Impl;
  explicit ExposurePanel(std::shared_ptr<const Impl>);
  std::shared_ptr<const Impl> impl_;
  friend class ExposurePanelBuilder;
  friend atx::core::Result<ExposureCrossSection> extract_exposure_date(
      const ExposurePanel&, atx::usize, std::string_view, std::string_view,
      atx::i64, const ExposureExtractConfig&);
};

class ExposurePanelBuilder {
public:
  [[nodiscard]] static atx::core::Result<ExposurePanelBuilder> create(const ExposurePanelConfig&);
  ExposurePanelBuilder(ExposurePanelBuilder&&) noexcept;
  ExposurePanelBuilder& operator=(ExposurePanelBuilder&&) noexcept;
  ~ExposurePanelBuilder();
  ExposurePanelBuilder(const ExposurePanelBuilder&) = delete;
  ExposurePanelBuilder& operator=(const ExposurePanelBuilder&) = delete;
  // Append exactly once in axis order; validation/normalization completes before
  // committing a row. Missing evidence is retained as canonical NaN plus reason.
  [[nodiscard]] atx::core::Status append_date(atx::usize date, const ExposureRowInput&);
  [[nodiscard]] atx::core::Result<ExposurePanel> finish(); // requires all dates
private:
  struct Impl;
  explicit ExposurePanelBuilder(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};

// Hash/axis/decision identity must agree on EVERY extraction. No static-date
// broadcast. Common finite requested support; positive qualified cap mandatory.
// Missing support/prerequisites -> Unavailable; identity/config mismatch ->
// InvalidArgument; byte limits -> OutOfRange. Supplied evidence stays caller-declared.
// Raw winsorizing uses equal mean/population SD for <=16 passes, then cap-weighted
// mean/equal SD. Final centered z may exceed +/-3; zero spread is flagged zero.
// Missing policy and coverage are explicit; unknown membership has its own
// denominator and prevents RefuseIncomplete from claiming a complete date.
[[nodiscard]] atx::core::Result<ExposureCrossSection> extract_exposure_date(
    const ExposurePanel&, atx::usize date, std::string_view expected_axis_sha256,
    std::string_view expected_content_sha256, atx::i64 exact_decision_ns,
    const ExposureExtractConfig& = {});

} // namespace atx::engine::data
