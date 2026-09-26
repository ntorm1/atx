#include "execution_cash_claim_internal.hpp"
#include <cmath>
#include <string_view>

namespace atx::engine::factory::execution_cash_claim_detail {
namespace co=atx::core;
namespace {
bool digest(std::string_view s) noexcept {
  if (s.size()!=64) return false;
  bool nonzero=false;
  for (const char c:s) {
    if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
    nonzero |= c!='0';
  }
  return nonzero;
}
}
co::Status validate(const ExecutionCashClaimEvent& e) {
  if (e.event_id.empty() || e.event_id.size()>128 || e.revision==0 || e.instrument_id==0 ||
      e.security_id_namespace.empty() || e.security_id_namespace.size()>128 ||
      e.historical_identity.empty() || e.historical_identity.size()>256 ||
      !digest(e.panel_source_sha256) || !digest(e.identity_evidence_sha256) ||
      !digest(e.completion_evidence_sha256) || !digest(e.basis_evidence_sha256) ||
      (e.evidence!=ExecutionCashClaimEvidence::SyntheticFixtureV1 &&
       e.evidence!=ExecutionCashClaimEvidence::ReconstructedPublicationV1) ||
      !e.cash_excluded_from_adjusted_close ||
      !std::isfinite(e.reference_raw_close) || e.reference_raw_close<=0 ||
      !std::isfinite(e.reference_adjusted_close) || e.reference_adjusted_close<=0 ||
      !std::isfinite(e.cash_usd_per_raw_share) || e.cash_usd_per_raw_share<=0)
    return co::Err(co::ErrorCode::InvalidArgument,"cash claim: identity/evidence/basis/amount");
  if (e.reference_mark_ns<=0 || e.reference_mark_ns>e.effective_after_ns ||
      e.effective_after_ns>=e.effective_by_ns || e.available_at_ns<e.effective_by_ns ||
      e.effective_by_ns>=e.recognition_mark_ns || e.available_at_ns>=e.recognition_mark_ns)
    return co::Err(co::ErrorCode::InvalidArgument,"cash claim: completion/publication/mark clock");
  return co::Ok();
}
co::Result<ExecutionCashClaimRecognition> recognize(const ExecutionCashClaimEvent& e,
    atx::usize event_index,atx::usize instrument_index,atx::usize period,atx::f64 held) {
  // Preparation validated the scalar contract. The marked-dollar book is a
  // research reinvested-share equivalent, not a broker physical-share ledger.
  const auto units=held/e.reference_raw_close;
  const auto claim=units*e.cash_usd_per_raw_share;
  const auto bridge=claim-held;
  if (!std::isfinite(held) || !std::isfinite(units) || !std::isfinite(claim) ||
      !std::isfinite(bridge) || (held!=0 && (units==0 || claim==0)))
    return co::Err(co::ErrorCode::OutOfRange,"cash claim: signed conversion overflow/underflow");
  return co::Ok(ExecutionCashClaimRecognition{event_index,instrument_index,e.instrument_id,
      period,e.recognition_mark_ns,held,units,claim,bridge,0});
}
} // namespace atx::engine::factory::execution_cash_claim_detail
