// risk_types — names of the risk engine's route / status / flag vocabulary.

#include "atx/vol/api/risk/risk_types.hpp"

#include <cstdint>

namespace atx::vol {

const char *to_string(RiskRoute value) noexcept {
  switch (value) {
  case RiskRoute::None:
    return "None";
  case RiskRoute::Family:
    return "Family";
  case RiskRoute::ColdAnalytic:
    return "ColdAnalytic";
  case RiskRoute::ColdFd:
    return "ColdFd";
  case RiskRoute::Exercised:
    return "Exercised";
  case RiskRoute::Share:
    return "Share";
  case RiskRoute::Refused:
    return "Refused";
  }
  return "Unrecognized"; // unreachable for valid enumerators
}

const char *to_string(RiskLaneStatus value) noexcept {
  switch (value) {
  case RiskLaneStatus::Ok:
    return "Ok";
  case RiskLaneStatus::ModelUnavailable:
    return "ModelUnavailable";
  case RiskLaneStatus::NumericError:
    return "NumericError";
  case RiskLaneStatus::InvalidContract:
    return "InvalidContract";
  case RiskLaneStatus::Rolled:
    return "Rolled";
  case RiskLaneStatus::Refused:
    return "Refused";
  }
  return "Unrecognized"; // unreachable for valid enumerators
}

const char *to_string(RiskFlag value) noexcept {
  // A RiskFlag value may carry several bits when a caller casts an OR-ed RiskFlags
  // back; the contract is the LOWEST set bit's name.
  const auto bits = static_cast<std::uint32_t>(value);
  if (bits == 0u) {
    return "";
  }
  const std::uint32_t lowest = bits & (~bits + 1u);
  switch (static_cast<RiskFlag>(lowest)) {
  case RiskFlag::None:
    return "";
  case RiskFlag::TenorExtrapolated:
    return "TenorExtrapolated";
  case RiskFlag::MoneynessExtrapolated:
    return "MoneynessExtrapolated";
  case RiskFlag::NearExerciseEdge:
    return "NearExerciseEdge";
  case RiskFlag::MarkMissing:
    return "MarkMissing";
  case RiskFlag::SurfaceMissing:
    return "SurfaceMissing";
  case RiskFlag::OutsideFamilyBox:
    return "OutsideFamilyBox";
  case RiskFlag::Rolled:
    return "Rolled";
  case RiskFlag::PivotFromMarks:
    return "PivotFromMarks";
  case RiskFlag::VendorParityAvailable:
    return "VendorParityAvailable";
  case RiskFlag::ColdFallback:
    return "ColdFallback";
  case RiskFlag::CarryFromSurface:
    return "CarryFromSurface";
  case RiskFlag::FamilyUneconomic:
    return "FamilyUneconomic";
  case RiskFlag::ShortTenor:
    return "ShortTenor";
  }
  return "Unknown"; // a bit no enumerator names
}

} // namespace atx::vol
