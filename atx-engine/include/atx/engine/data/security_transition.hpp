#pragma once

#include <array>
#include <optional>

#include "atx/core/types.hpp"

namespace atx::engine::data {

// Fixed-size content references. Nonzero bytes bind supplied evidence; they do
// not authenticate it. The first planner accepts synthetic fixtures only.
using TransitionDigest = std::array<atx::u8, 32>;

enum class TransitionEvidenceNamespace : atx::u8 { Unknown, SyntheticFixture };
enum class TransitionCurrency : atx::u8 { Unknown, USD };
enum class TransitionCoverage : atx::u8 { Unknown, ExcludedFromTri, EmbeddedInTri };
enum class TransitionShareDenomination : atx::u8 { Unknown, PredecessorBeforeTransition };
enum class TransitionBoundary : atx::u8 { Unknown, BetweenValuations };
enum class TransitionEntitlementRule : atx::u8 { Unknown, SameAsConvertedPredecessor };

struct TransitionEvidence {
  TransitionEvidenceNamespace name_space{TransitionEvidenceNamespace::Unknown};
  TransitionDigest content_sha256{};
  std::optional<atx::i64> available_at_ns;
};

struct TransitionSecurity {
  atx::u64 vendor_security_id{};
  atx::u64 identity_epoch{};
  atx::usize instrument{};
  TransitionDigest mapping_sha256{};
  bool operator==(const TransitionSecurity&) const = default;
};

// Numerator and denominator must each fit an exactly representable f64 integer.
// Stock numerator must be positive; the cash numerator may be zero.
struct TransitionRatio {
  atx::u64 numerator{};
  atx::u64 denominator{};
};

struct SecurityTransition {
  atx::u64 event_id{};
  atx::u32 revision{};
  atx::u64 sequence{};
  atx::u64 expected_state_version{};
  atx::u64 cash_claim_id{};
  TransitionSecurity predecessor;
  TransitionSecurity successor;
  TransitionRatio stock_ratio;
  TransitionRatio cash_per_predecessor_share;
  TransitionCurrency currency{TransitionCurrency::Unknown};
  TransitionShareDenomination cash_denomination{TransitionShareDenomination::Unknown};
  TransitionBoundary boundary{TransitionBoundary::Unknown};
  TransitionEntitlementRule entitlement_rule{TransitionEntitlementRule::Unknown};
  std::optional<atx::i64> effective_at_ns;
  std::optional<atx::i64> entitlement_at_ns;
  TransitionDigest terms_sha256{};
  TransitionEvidence evidence;
};

struct TransitionMarkBasis {
  TransitionSecurity security;
  atx::f64 raw_price{};
  atx::f64 tri_price{};
  std::optional<atx::i64> observed_at_ns;
};

struct TransitionBasis {
  atx::u64 event_id{};
  atx::u32 revision{};
  TransitionDigest source_artifact_sha256{};
  TransitionDigest axes_sha256{};
  TransitionDigest adjustment_recipe_sha256{};
  // Availability cannot precede the successor observation described here.
  TransitionEvidence evidence;
  TransitionMarkBasis predecessor;
  TransitionMarkBasis successor;
  TransitionCoverage stock_coverage{TransitionCoverage::Unknown};
  TransitionCoverage cash_coverage{TransitionCoverage::Unknown};
  TransitionCoverage continuity_coverage{TransitionCoverage::Unknown};
  // Separate from converted quantity. This slice requires the explicit same-
  // quantity entitlement rule, within its published numerical tolerance.
  atx::f64 entitled_predecessor_shares{};
};

// A payment record describes an actual synthetic allocation, not an issuer's
// declared payable date. Evidence availability cannot precede the allocation.
// Only a full, exactly matching signed claim is supported.
struct CashClaimPayment {
  atx::u64 payment_id{};
  atx::u64 sequence{};
  atx::u64 expected_state_version{};
  atx::u64 cash_claim_id{};
  atx::u64 event_id{};
  atx::u32 event_revision{};
  TransitionCurrency currency{TransitionCurrency::Unknown};
  atx::f64 amount{};
  std::optional<atx::i64> allocated_at_ns;
  TransitionEvidence evidence;
};

} // namespace atx::engine::data
