#include "execution_stock_transition_internal.hpp"
#include <cmath>
#include <string_view>
namespace atx::engine::factory::execution_stock_transition_detail {
namespace co=atx::core;
namespace {
bool digest(std::string_view s) noexcept {
  if (s.size()!=64) return false;
  bool nonzero=false;
  for (char c:s) {
    if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
    nonzero|=c!='0';
  }
  return nonzero;
}
}
co::Status validate(const ExecutionStockTransitionEvent& e) {
  if (e.event_id.empty() || e.event_id.size()>128 || e.revision==0 ||
      e.predecessor_id==0 || e.successor_id==0 || e.predecessor_id==e.successor_id ||
      e.security_id_namespace.empty() || e.security_id_namespace.size()>128 ||
      e.predecessor_identity.empty() || e.predecessor_identity.size()>256 ||
      e.successor_identity.empty() || e.successor_identity.size()>256 ||
      !digest(e.panel_source_sha256) || !digest(e.predecessor_identity_evidence_sha256) ||
      !digest(e.successor_identity_evidence_sha256) || !digest(e.completion_evidence_sha256) ||
      !digest(e.basis_evidence_sha256) ||
      (e.evidence!=ExecutionCashClaimEvidence::SyntheticFixtureV1 &&
       e.evidence!=ExecutionCashClaimEvidence::ReconstructedPublicationV1) ||
      !e.stock_and_cash_excluded_from_adjusted_close ||
      e.fraction_rule!=ExecutionStockFractionRule::ContinuousResearchUnitsV1 ||
      e.borrow_rule!=ExecutionStockBorrowRule::ModeledSuccessorNoDischargeV1 ||
      e.stock_ratio_numerator==0 || e.stock_ratio_denominator==0 ||
      e.stock_ratio_numerator>1'000'000'000 || e.stock_ratio_denominator>1'000'000'000 ||
      !std::isfinite(e.reference_raw_close) || e.reference_raw_close<=0 ||
      !std::isfinite(e.reference_adjusted_close) || e.reference_adjusted_close<=0 ||
      !std::isfinite(e.successor_raw_close) || e.successor_raw_close<=0 ||
      !std::isfinite(e.successor_adjusted_close) || e.successor_adjusted_close<=0 ||
      !std::isfinite(e.fixed_cash_usd_per_raw_share) || e.fixed_cash_usd_per_raw_share<0)
    return co::Err(co::ErrorCode::InvalidArgument,"stock transition: identity/evidence/basis/ratio/policy");
  if (e.reference_mark_ns<=0 || e.reference_mark_ns>e.effective_after_ns ||
      e.effective_after_ns>=e.effective_by_ns || e.available_at_ns<e.effective_by_ns ||
      e.effective_by_ns>=e.recognition_mark_ns || e.available_at_ns>=e.recognition_mark_ns)
    return co::Err(co::ErrorCode::InvalidArgument,"stock transition: completion/publication/mark clock");
  return co::Ok();
}
co::Result<ExecutionStockTransitionRecognition> recognize(const ExecutionStockTransitionEvent& e,
    atx::usize event_index,atx::usize predecessor,atx::usize successor,atx::usize period,
    atx::f64 held) {
  const auto old_units=held/e.reference_raw_close;
  const auto ratio=static_cast<atx::f64>(e.stock_ratio_numerator)/
                   static_cast<atx::f64>(e.stock_ratio_denominator);
  const auto units=old_units*ratio;
  const auto dollars=units*e.successor_raw_close;
  const auto claim=old_units*e.fixed_cash_usd_per_raw_share;
  const auto bridge=(dollars+claim)-held;
  if (!std::isfinite(held) || !std::isfinite(old_units) || !std::isfinite(units) ||
      !std::isfinite(dollars) || !std::isfinite(claim) || !std::isfinite(bridge) ||
      (held!=0 && (old_units==0 || units==0 || dollars==0 ||
                  (e.fixed_cash_usd_per_raw_share!=0 && claim==0))))
    return co::Err(co::ErrorCode::OutOfRange,"stock transition: signed conversion overflow/underflow");
  return co::Ok(ExecutionStockTransitionRecognition{event_index,predecessor,successor,period,
      e.predecessor_id,e.successor_id,e.recognition_mark_ns,held,old_units,units,dollars,claim,bridge,0});
}
} // namespace atx::engine::factory::execution_stock_transition_detail
