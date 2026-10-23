#pragma once

// K2 (r5, 2026-10-22): the recorded Premium inverse request stream and its replay through
// iv_batch::invert_many — the lane's stop/go measurement. Bench only: armed by
// atx-vol-quote-bench --inverse-replay; disarmed (the default) it costs one relaxed load
// per Premium request and changes nothing.
//
// RECORDING. While a stream is armed, every Premium request american_iv.cpp's premium_root
// receives is appended in arrival order — refusals included (a price at the range edges,
// an invalid input): price, contract, tolerance, the family-native family when the request
// was native-eligible (snapshotted ONCE per family as a K1 PremiumTable, so the replay
// never touches a family the fit has since freed), the request whose native carry started
// it (the quote's mid behind its bid / ask) and the fit's own native outcome. Appends are
// serialised by the stream's mutex (one fit worker in the timed runs; safe with more).
// A family is identified by its prepared state's address plus its box, node counts, side
// and anchor; two families that agree on all of them while alive at different times are
// the same chain's family rebuilt identically (a rung re-fit), which the snapshot equals.
//
// REPLAY. Requests are grouped by family (and S, tolerance); per group ONE invert_many
// call takes every request as a lane — a bid / ask anchored to its mid — after the
// unanchored lanes get the fit's own Black-76 seed. Untimed: grouping, the SoA arrays,
// the scratch. Timed (steady clock): the seeds and the invert_many calls. The fit's own
// native outcomes are compared lane by lane (status, root distance).
//
// NOT a public API.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_iv_batch.hpp"

namespace atx::vol::iv_batch {

inline constexpr std::uint32_t kNoStreamId = 0xFFFFFFFFu;

struct InverseStreamRequest {
  double price = kNaN;
  double S = kNaN;
  double K = kNaN;
  double T = kNaN;
  double r = kNaN;
  double q = kNaN;
  double tol = kNaN;
  Side side = Side::Put;
  std::int32_t family = -1; // InverseStream::tables index (-1: not native-eligible)
  std::int32_t anchor = -1; // the request whose native carry started this one (-1: none)
  // The fit's own family-native attempt (Inactive: it did not run).
  InverseStatus fit_status = InverseStatus::Inactive;
  std::uint16_t fit_evaluations = 0;
  double fit_sigma = kNaN;
};

struct InverseStreamFamilyKey {
  const void *state = nullptr;
  double sigma_lo = kNaN;
  double sigma_hi = kNaN;
  double q_lo = kNaN;
  double q_hi = kNaN;
  double anchor = kNaN;
  std::uint16_t n_sigma = 0;
  std::uint16_t n_q = 0;
  Side side = Side::Put;
};

struct InverseStream {
  std::mutex mutex;
  std::vector<InverseStreamRequest> requests;
  // Held by unique_ptr and never moved or rebuilt (a moved-from PremiumTable must not be
  // reused: K1 review, 2026-10-22).
  std::vector<std::unique_ptr<sigma_q_family_detail::PremiumTable>> tables;
  std::vector<InverseStreamFamilyKey> keys;
  std::uint64_t table_failures = 0; // a snapshot refused / failed: recorded without family
  std::uint64_t record_failures = 0; // std::bad_alloc while appending: request dropped
};

// Process-wide arm (nullptr disarms). The stream must outlive the armed period.
void inverse_stream_arm(InverseStream *stream) noexcept;
[[nodiscard]] InverseStream *inverse_stream_armed() noexcept;

// Append one request (family snapshotted on first sight; nullptr: not native-eligible).
// `carried_from` is the stream id of the request whose native carry this request starts
// from (kNoStreamId: none). Returns the request's id, kNoStreamId when unarmed or dropped.
[[nodiscard]] std::uint32_t inverse_stream_record(const InverseStreamRequest &request,
                                                  const AlSigmaQFamily *native_family,
                                                  std::uint32_t carried_from) noexcept;
// The fit's own native outcome of request `id` (no-op for kNoStreamId / unarmed).
void inverse_stream_annotate(std::uint32_t id, InverseStatus status, std::uint16_t evaluations,
                             double sigma) noexcept;

struct InverseReplay {
  std::uint64_t requests = 0;
  std::uint64_t with_family = 0;    // replayed through invert_many
  std::uint64_t without_family = 0; // not native-eligible in the fit (no family to replay)
  std::uint64_t groups = 0;         // invert_many calls
  std::uint64_t max_group = 0;      // lanes in the largest call
  std::uint64_t anchored = 0;       // lanes anchored to their mid
  std::uint64_t seeds = 0;          // Black-76 seeds computed
  std::uint64_t seed_failures = 0;  // ... of which failed (the lane starts from the bracket)
  InverseBatchWork work;
  double seed_seconds = 0.0;  // timed: the Black-76 seeds
  double batch_seconds = 0.0; // timed: the invert_many calls
  double total_seconds = 0.0; // timed region (seeds + batches + loop overhead)
  // Agreement with the fit's own native attempts on the same requests.
  std::uint64_t fit_compared = 0;     // requests the fit ran natively
  std::uint64_t fit_status_equal = 0; // ... with the replay's status
  std::uint64_t fit_both_converged = 0;
  double fit_max_dsigma = 0.0;        // max |σ_replay − σ_fit| over both converged
  std::array<std::uint64_t, kInverseStatusCount> fit_by_status{};
};

// Replay `stream` (contract at the top). Allocates (grouping) outside its timed region.
[[nodiscard]] InverseReplay replay_inverse_stream(const InverseStream &stream, InverseIsa isa);

} // namespace atx::vol::iv_batch
