#pragma once

#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/engine/data/exposure_panel.hpp"

namespace atx::engine::alpha { class Panel; }
namespace atx::engine::data {
class PanelStore;

enum class PriceExposureRule : atx::u8 { PriorKnownCapPriceV1 = 1 };
enum class ExposureCloseBasis : atx::u8 { Unknown = 0, AdjustedCloseRatioV1 = 1 };
enum class ExposureDollarBasis : atx::u8 { Unknown = 0, RawUsdCloseTimesRawSharesV1 = 1 };

struct PriceExposureConfig {
  // Provider generates all six descriptor recipes; these entries must be empty.
  ExposurePanelConfig output;
  std::vector<atx::i64> mark_times_ns;
  std::string price_source_sha256;
  std::string close_field{"close"}, raw_close_field{"raw_close"}, volume_field{"volume"};
  ExposureCloseBasis close_basis{ExposureCloseBasis::Unknown};
  ExposureDollarBasis dollar_basis{ExposureDollarBasis::Unknown};
  PriceExposureRule rule{PriceExposureRule::PriorKnownCapPriceV1};
  bool use_return_guard{false}; // requires a return_guard parent and N flags per date
  atx::u64 max_working_bytes{512ULL * 1024 * 1024};
};

// All spans are N cells, no static broadcast. raw_descriptors/source_present in
// evidence must be empty: the provider supplies them from computed/source data.
// The other ExposureRowInput spans may be empty, meaning unavailable evidence.
// A missing presence clock does NOT turn a finite backing value into an observation.
struct PriceExposureEvidenceRow {
  ExposureRowInput evidence;
  std::span<const atx::i64> bar_available_ns;
  std::span<const atx::u8> bar_qualified;
  std::span<const atx::u8> bad_return; // interval ending this date; optional explicit guard
};

// Synchronous borrowed row views, valid until the next read_date call. Implementors
// must have bounded retained storage and must not read future rows to construct a
// current snapshot. retained_bytes includes any owned read/decode scratch. This
// interface validates supplied clocks but does not authenticate a data vendor.
class PriceExposureEvidenceSource {
public:
  virtual ~PriceExposureEvidenceSource() = default;
  [[nodiscard]] virtual atx::u64 retained_bytes() const noexcept = 0;
  [[nodiscard]] virtual atx::core::Result<PriceExposureEvidenceRow>
      read_date(atx::usize date) = 0;
};

enum PriceMarketReason : atx::u32 {
  PriceMarketNoPriorDate = 1U, PriceMarketUnknownMembership = 2U,
  PriceMarketUnknownCap = 4U, PriceMarketUnknownIdentity = 8U,
  PriceMarketMissingReturn = 16U, PriceMarketTooFewNames = 32U,
  PriceMarketNumericalRange = 64U
};
struct PriceExposureDate {
  atx::usize prior_known_members{}, unknown_membership{}, unknown_cap{}, unknown_identity{};
  atx::usize qualified_prior_caps{}, observed_market_returns{}, missing_market_returns{};
  // Coverage is NaN when the denominator's membership/caps/identity are unknown.
  atx::f64 admitted_cap_weight_fraction{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 market_return{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::i64 market_available_at_ns{};
  atx::u32 market_reasons{};
  std::array<atx::usize, kExposureRawDescriptorCount> finite_raw_descriptors{};
};
struct PriceExposureResult {
  ExposurePanel panel;
  std::vector<PriceExposureDate> dates;
  atx::u64 provider_working_bytes{}, mapped_chunk_bound_bytes{};
};

// Strict versioned recipe, using complete uncompressed calendar windows:
// log ADV63 (raw USD close * raw traded shares); Amihud63 (abs(simple adjusted
// return)/raw USD volume); intercept beta252 and population daily residual SD252;
// log(close[d-21])-log(close[d-252]); negative log-return21.
// Market interval ending d uses qualified cap/membership/identity at decision[d-1].
// Require all membership known, every member's cap/identity qualified, >=2 members,
// and every weighted return observed. Missing is never zero; no implicit support
// renormalization. Complete 63/252 windows are required. A late/unqualified bar is
// unavailable under on-time admission and never backfills an earlier ring slot.
// Both adapters accumulate in f64; D6 uses its original-f64 close channel.
// Output is immediately usable by extract_exposure_date / residual IC preparation.
// No stage, disk publication, cap construction or FF49 inference occurs here.
// Budget includes retained output/builder, evidence source, rolling rings, row
// scratch and one D6 mapping. Caller-owned Panel storage is borrowed, not charged.
[[nodiscard]] atx::core::Result<PriceExposureResult> build_price_exposures(
    const alpha::Panel&, PriceExposureEvidenceSource&, const PriceExposureConfig&);
[[nodiscard]] atx::core::Result<PriceExposureResult> build_price_exposures(
    const PanelStore&, PriceExposureEvidenceSource&, const PriceExposureConfig&);

} // namespace atx::engine::data
