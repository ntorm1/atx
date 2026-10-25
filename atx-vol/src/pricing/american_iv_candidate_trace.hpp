#pragma once

// Private diagnostic seam. Observation never supplies a candidate or proof.
// All records own values; correction caches are represented only by presence.
#include <array>
#include <cstdint>
#include <optional>

#include "pricing/american_iv_search.hpp"

namespace atx::vol::research {

enum class IvCandidatePhase : std::uint8_t {
  ScreenProposal, ScreenProof, FastProposal, FastProof, CompleteFast, RetryProof
};
enum class IvCandidateOutcome : std::uint8_t {
  Incomplete, InvalidControls, Accepted, Refused, AllocationFailure
};
enum class IvCandidatePhaseOutcome : std::uint8_t {
  NotVisited, Value, Refused, Error, AllocationFailure
};
// Always-on ledger deltas: boundary solves, premium evaluations, IV iterations.
using IvCandidateWork = std::array<std::uint64_t, 3>;

struct IvCandidateRequest {
  double price = 0., S = 0., K = 0., T = 0., r = 0., q = 0., budget = 0.;
  Side side = Side::Put;
};
struct IvCandidateOptions {
  AmericanMethod method = AmericanMethod::AndersenLake;
  double tol = 0.;
  std::uint16_t max_iter = 0;
  std::optional<AlOpts> al_opts;
  bool has_correction = false;
  double warm_start = 0., price_tol = 0.;
  bool refuse_non_monotone = false;
  IvStrategy strategy = IvStrategy::Search;
  std::optional<AlProfile> seed_profile;
};
struct IvCandidatePhaseTrace {
  bool visited = false, complete = false, stopped_early = false;
  IvCandidatePhaseOutcome outcome = IvCandidatePhaseOutcome::NotVisited;
  std::optional<double> sigma, seed_sigma;
  std::optional<ErrorCode> error;
  std::optional<IvStatus> typed_status;
  std::optional<AlOpts> al_opts;
  std::uint64_t elapsed_ns = 0;
  IvCandidateWork work{};
  std::uint64_t floor_preparations = 0, floor_hits = 0;
};
struct IvPriceCandidateTrace {
  bool complete = false, floor_cache_present = false;
  IvCandidateRequest request;
  IvCandidateOptions options;
  IvPriceProposalKernel kernel = IvPriceProposalKernel::ScreenThenFast;
  IvCandidateOutcome outcome = IvCandidateOutcome::Incomplete;
  IvPriceCandidateStats stats;
  std::optional<double> sigma;
  // Fixed enum order; skipped phases remain untouched defaults.
  std::array<IvCandidatePhaseTrace, 6> phases;
  std::uint64_t elapsed_ns = 0;
  IvCandidateWork work{};
  std::uint64_t floor_preparations = 0, floor_hits = 0;
  IvPriceSeedFloorSnapshot floor_before, floor_after;
};

// Runs precisely the original conditional proposal/proof chain with owned
// observations. Invalid controls reset the record and perform no numerical work.
// Phase clocks are nested in the inclusive clock. No extra inverse, price, floor,
// heap-owning trace storage or global collector is introduced by this seam.
// The normal per-thread solve-ledger registration must precede any allocation
// measurement. Only bad_alloc escaping a numerical operation is distinguishable;
// a helper's contained optional refusal remains a refusal, without an invented
// cause. The ordinary candidate instantiates the same body with observation
// compiled out.
[[nodiscard]] std::optional<double> american_implied_vol_price_candidate_traced(
    double price, double S, double K, double T, double r, double q, Side side,
    const IvOpts &opts, double max_price_error, IvPriceCandidateTrace &trace,
    IvPriceProposalKernel kernel = IvPriceProposalKernel::ScreenThenFast,
    IvPriceSeedFloorCache *floor_cache = nullptr) noexcept;

} // namespace atx::vol::research
