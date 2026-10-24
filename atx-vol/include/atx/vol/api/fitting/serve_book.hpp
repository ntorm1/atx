#pragma once

// Live publication of serving records (lane H1, r6 2026-10-23; master plan Sprint 5 H1,
// Gate G6): the book a quoting process reads its expiries' SliceServeRecords from while a
// fit, a rebuild or a refit publishes new ones. docs/live-query.md is the protocol.
//
// ## Versions
//
// A ServeBookVersion is an immutable set of entries (one per expiry: its record, the
// instants after which its geometry no longer applies, and the caller's tag) with a
// version number. Publication is copy-on-write: a ServeBookTransaction collects puts and
// retirements, commit() builds the WHOLE next version off to the side, then swaps it in
// with one atomic store and bumps the book's version counter (release). A publisher that
// dies before commit() returns (an exception, an abandoned transaction) publishes nothing:
// the old version stays live, whole. Readers therefore see version N or N + 1, never a
// mix (no torn version), and version numbers only grow (no ABA: a reader compares numbers,
// never addresses).
//
// ## Reclamation (deferred, publisher-side)
//
// Readers never free a version or a record. The book keeps every retired version in a
// graveyard until no reader holds it (its use count is the graveyard's own), and frees
// those on the publisher's thread (every commit, or reclaim()). A record shared by
// consecutive versions lives as long as any of them.
//
// ## Readers
//
// A ServeBookReader is one thread's handle (not itself thread-safe; make one per thread).
// Each query reads the book's version counter (one acquire load); only when it moved does
// the reader re-pin the current version (an atomic shared_ptr load). Queries then go to
// the expiry's record. A reader never runs a solve the primary tier does not allow:
// ServeBookPolicy::primary_tier_max (Family by default) caps every request, so a contract
// no allowed tier can answer is a typed refusal with its route (ColdPath / Rebuild /
// Refit), never a synchronous cold solve on the quoting thread. Normal queries allocate
// nothing (LivePublication.*Alloc*).
//
// ## Typed responses (ServeStatus) added by the book
//
//   Invalidated     now_ns at or after the entry's expiry_ns or next_event_ns (expiry,
//                   ex-dividend or any geometry event): route Refit; a rebuild request
//                   (reason Invalidated) is queued for the owner.
//   Stale           the entry has instants (expiry_ns / next_event_ns > 0) or the record
//                   a max_age_ns, and the request carries no clock (now_ns <= 0): ColdPath.
//   BudgetExhausted a refusal routed Rebuild (a Stale record, a missing expiry with a
//                   rebuild function) whose rebuild request found the queue full: nothing
//                   was queued; the caller retries or takes the cold path.
// Every record status (OutOfDomain, PriceOnly, SigmaOutOfBox, TierUnavailable, Stale,
// NonMonotoneBand, Unconfirmed, ...) passes through unchanged.
//
// ## Async rebuild queue
//
// A bounded ring (ServeBookPolicy::queue_capacity, allocated once) of rebuild requests,
// one per expiry at a time. Same-token reader requests deduplicate; newer input
// replaces queued intent or retains one follow-up beside an in-flight request.
// ServeBookPolicy::build_workers std::jthread workers (never the pricing pool) pop them,
// call the owner's ServeRebuildFn and conditionally publish what it returns;
// replacement/retirement or a newer owner request discards stale work. Unrelated
// expiry publication does not invalidate it. build_workers = 0 leaves
// the queue to run_pending() on the owner's thread. A rebuild that throws or returns
// nothing publishes nothing (the old version stays live) and is counted.
//
// ## Threads
//
// ServeBook: any number of publishers (commits serialize on one mutex) and readers.
// The book must outlive its transactions; readers keep the book's shared state alive.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp" // ChainTickOutputs, TickStatus

namespace atx::vol {

// One expiry of a version.
struct ServeBookEntry {
  std::uint64_t expiry_id = 0; // the caller's key (unique within a version)
  std::shared_ptr<const SliceServeRecord> record;
  std::int64_t expiry_ns = 0;     // Unix ns; > 0: at or after it the entry is Invalidated
  std::int64_t next_event_ns = 0; // Unix ns; > 0: an event (ex-dividend, ...) that ends it
  std::uint64_t tag = 0;          // the caller's provenance stamp, echoed
};

struct ServeBookVersion {
  std::uint64_t number = 0;              // 0: the empty book; then 1, 2, ... per commit
  std::int64_t published_steady_ns = 0;  // steady clock at commit
  std::vector<ServeBookEntry> entries;   // sorted by expiry_id, unique

  // The entry of `expiry_id`, nullptr when the version has none.
  [[nodiscard]] const ServeBookEntry *find(std::uint64_t expiry_id) const noexcept;
};

enum class ServeRebuildReason : std::uint8_t {
  Requested = 0,   // the owner asked (request_rebuild)
  Stale = 1,       // a reader met a Stale record (route Rebuild)
  Invalidated = 2, // a reader met an Invalidated entry (route Refit)
  Missing = 3,     // a reader asked for an expiry the version does not hold
};

struct ServeRebuildRequest {
  std::uint64_t expiry_id = 0;
  ServeRebuildReason reason = ServeRebuildReason::Requested;
  std::int64_t enqueued_steady_ns = 0;
  std::uint64_t generation = 0; // book-owned publication/tombstone generation
  std::uint64_t input_revision = 0; // explicit owner requests within that generation
};

// The owner's rebuild: the entry to publish for the request (record, instants, tag), or
// nullopt when it has none. Runs on a worker (or run_pending()'s caller); may throw.
using ServeRebuildFn = std::function<std::optional<ServeBookEntry>(const ServeRebuildRequest &)>;

struct ServeBookPolicy {
  // The dearest tier a reader query may use (no reader-triggered cold solve above it:
  // Certified's Fast confirm and Cold are routes only unless this allows them).
  ServeTier primary_tier_max = ServeTier::Family;
  std::size_t queue_capacity = 64; // rebuild requests held at once (>= 1)
  // Rebuild workers (std::jthread, never the pricing pool); 0: the owner drains the queue
  // with run_pending().
  unsigned build_workers = 1;
};

struct ServeBookCounters {
  std::uint64_t published = 0;       // versions committed
  std::uint64_t aborted = 0;         // transactions with work destroyed without commit
  std::uint64_t reclaimed = 0;       // retired versions freed (publisher-side)
  std::uint64_t retired_held = 0;    // retired versions a reader still holds (now)
  std::uint64_t enqueued = 0;        // rebuild requests queued
  std::uint64_t deduplicated = 0;    // ... already queued for that expiry
  std::uint64_t rejected = 0;        // ... refused: the queue was full (BudgetExhausted)
  std::uint64_t rebuilt = 0;         // rebuilds published
  std::uint64_t rebuild_failed = 0;  // rebuilds that threw or returned nothing
  std::uint64_t queue_depth = 0;     // ring plus retained follow-ups waiting (now)
  std::int64_t queue_age_max_ns = 0; // enqueue -> a worker took it
  std::int64_t rebuild_max_ns = 0;   // enqueue -> published
  std::int64_t queue_age_sum_ns = 0;
  std::int64_t rebuild_sum_ns = 0;
  std::uint64_t rebuild_superseded = 0; // completed work discarded after newer input/publication
  std::uint64_t rebuild_followups = 0;  // coalesced newer requests scheduled after running work
};

enum class ServeEnqueue : std::uint8_t { Queued = 0, Deduplicated = 1, Full = 2, NoRebuild = 3 };

class ServeBook;
class ServeBookReader;
namespace detail {
struct ServeBookAccess;
}

// Collects puts and retirements; commit() publishes them as ONE new version applied to
// the version current at commit. Destroyed without commit(): nothing is published.
class ServeBookTransaction {
public:
  ~ServeBookTransaction();
  ServeBookTransaction(ServeBookTransaction &&) noexcept;
  ServeBookTransaction &operator=(ServeBookTransaction &&) = delete;
  ServeBookTransaction(const ServeBookTransaction &) = delete;
  ServeBookTransaction &operator=(const ServeBookTransaction &) = delete;

  // InvalidArgument for an entry without a record (nothing is staged then).
  [[nodiscard]] Status put(ServeBookEntry entry);
  void retire(std::uint64_t expiry_id);
  void retire_all(); // the committed version holds only this transaction's puts
  // The committed version's number (the current one when nothing was staged).
  [[nodiscard]] Result<std::uint64_t> commit();

  struct Op; // one staged put or retirement (serve_book.cpp)

private:
  friend class ServeBook;
  explicit ServeBookTransaction(ServeBook &book);
  ServeBook *book_ = nullptr;
  std::vector<Op> ops_;
  bool replace_ = false;
  bool done_ = false;
};

class ServeBook {
public:
  explicit ServeBook(const ServeBookPolicy &policy = {}, ServeRebuildFn rebuild = {});
  ~ServeBook(); // stops and joins the workers; queued requests are dropped
  ServeBook(const ServeBook &) = delete;
  ServeBook &operator=(const ServeBook &) = delete;
  ServeBook(ServeBook &&) = delete;
  ServeBook &operator=(ServeBook &&) = delete;

  [[nodiscard]] ServeBookTransaction transaction();
  // One-expiry convenience: put `entry` (replacing that expiry) and commit.
  [[nodiscard]] Result<std::uint64_t> publish_expiry(ServeBookEntry entry);
  // Replace the whole book with `entries` and commit.
  [[nodiscard]] Result<std::uint64_t> publish(std::span<const ServeBookEntry> entries);

  [[nodiscard]] std::uint64_t version() const noexcept;
  [[nodiscard]] std::shared_ptr<const ServeBookVersion> current() const;
  [[nodiscard]] ServeBookReader reader() const;
  [[nodiscard]] const ServeBookPolicy &policy() const noexcept;

  // Explicit owner intent: accepted requests advance the expiry's input revision,
  // even when deduplicated into existing work. A running older build is discarded
  // and one latest follow-up is retained. May allocate a new expiry token; reader
  // requests remain allocation-free. Full/NoRebuild does not invalidate old work.
  [[nodiscard]] ServeEnqueue
  request_rebuild(std::uint64_t expiry_id,
                  ServeRebuildReason reason = ServeRebuildReason::Requested);
  // Runs up to `max` queued rebuilds on the calling thread; returns how many ran.
  std::size_t run_pending(std::size_t max = static_cast<std::size_t>(-1));
  // Until the queue is empty and no rebuild is running, or the timeout; true when idle.
  [[nodiscard]] bool wait_idle(std::chrono::milliseconds timeout);
  // Frees the retired versions no reader holds; returns how many.
  std::size_t reclaim();
  [[nodiscard]] ServeBookCounters counters() const;

  struct State; // serve_book.cpp

private:
  friend class ServeBookTransaction;
  friend struct detail::ServeBookAccess;
  std::shared_ptr<State> state_;
};

// A chain of contracts across expiries as caller-owned columns (input order).
struct ServedChainView {
  std::span<const std::uint64_t> expiry_id;
  std::span<const double> strike;
  std::span<const Side> side;

  [[nodiscard]] std::size_t size() const noexcept { return strike.size(); }
  [[nodiscard]] bool consistent() const noexcept {
    return expiry_id.size() == strike.size() && side.size() == strike.size();
  }
};

// The statement of one served chain contract, beside its TickStatus (lane H1 fix round 1,
// review-H1 F1): what the record's answer (ServeAnswer) says about the value it served.
// Fixed size, written in place: the chain path stays allocation-free.
struct ServedChainStatement {
  double certificate_usd = std::numeric_limits<double>::quiet_NaN(); // ServeAnswer's
  double map_certificate_usd = std::numeric_limits<double>::quiet_NaN();
  double iv_lower = std::numeric_limits<double>::quiet_NaN(); // a PriceOnly node's interval
  double iv_upper = std::numeric_limits<double>::quiet_NaN();
  ServeStatus status = ServeStatus::TierUnavailable;
  ServeRoute route = ServeRoute::ColdPath;
  ServeTier tier = ServeTier::Cold;
  ServeBoundClass certificate_class = ServeBoundClass::None;
  ServeBoundClass map_certificate_class = ServeBoundClass::None;
  QuoteSlicePriceOnlyReason price_only_reason = QuoteSlicePriceOnlyReason::None;
  bool sampled = false; // ServeAnswer::sampled: an estimate, never certified
  // Verbatim ServeAnswer::reason (Delta/Gamma/Vega/Theta/Rho/Phi). Its own
  // vocabulary is distinct from AmericanGreeks' cold numerical stencil reasons.
  ServeGreekReasons greek_reasons{};
};

class ServeBookReader {
public:
  ServeBookReader(const ServeBookReader &) = delete;
  ServeBookReader &operator=(const ServeBookReader &) = delete;
  ServeBookReader(ServeBookReader &&) noexcept = default;
  ServeBookReader &operator=(ServeBookReader &&) noexcept = default;
  ~ServeBookReader() = default;

  // The expiry's record answer (record.serve) at the request's tiers capped by
  // primary_tier_max, with the book's typed responses (file header). No allocation.
  [[nodiscard]] ServeAnswer serve(std::uint64_t expiry_id, const ServeRequest &request);
  [[nodiscard]] ServeIvAnswer implied_vol(std::uint64_t expiry_id, const ServeIvRequest &request);

  // Every contract of `chain` at `spot` from ONE version (pinned for the whole call):
  // price, delta, gamma, vega, theta into `out`, each answer's statement into `statements`
  // (status, route, tier, certificate class and USD, the sampled flag, a price-only
  // node's reason and IV interval). TickStatus: Ok for an Ok answer that is not Sampled;
  // Fallback for a Sampled answer (an estimate "NOT covered by any certificate") and for a
  // refusal that keeps a price (PriceOnly at the fit spot); Exercised; Unpriceable (NaN)
  // for a refusal. Ok is not a certificate claim: read `certificate_class` (None: the
  // answer claims no statement). The chain path's requests: Greeks four, family_bound
  // false. No allocation.
  // @return InvalidArgument when a column length disagrees (nothing is written).
  [[nodiscard]] Status serve_chain(double spot, std::int64_t now_ns, const ServedChainView &chain,
                                   ServeTier tier, ServeTier tier_max,
                                   const ChainTickOutputs &out,
                                   std::span<ServedChainStatement> statements);

  // The version the last query read (pinned until the next query).
  [[nodiscard]] const ServeBookVersion &pinned() const noexcept { return *version_; }
  // Re-pins when the book's counter moved; true when it did.
  bool refresh() noexcept;

private:
  friend class ServeBook;
  explicit ServeBookReader(std::shared_ptr<ServeBook::State> state);
  // One query on the pinned version (`pinned`: no re-pin, no retry - the chain path).
  template <class Answer, class Request>
  [[nodiscard]] Answer answer(std::uint64_t expiry_id, const Request &request, bool pinned);
  std::shared_ptr<ServeBook::State> state_;
  std::shared_ptr<const ServeBookVersion> version_;
};

// The serve_build stage (lane H1 deliverable 1): builds the record of a certified `fit`
// (its serve_handoff when it carries one) and publishes it under `key` (expiry_id,
// instants, tag; key.record is ignored). Two-phase by default: first the record without
// ladders (tiers 1-2, the first usable bundle), then the laddered one replacing it.
// Both publications are conditional on the expiry's captured input token; phase 2
// expects phase 1. A replacement, retirement or accepted owner rebuild request
// supersedes the build (Unavailable), preserving current state. An unrelated
// expiry change does not invalidate it. A failed phase 2 leaves phase 1 only if
// no newer owner publication has replaced/retired it.
struct ServeBuildStage {
  std::uint64_t first_version = 0; // the version that first served the expiry
  std::uint64_t final_version = 0; // the version holding the final record
  double first_usable_us = 0.0;    // stage start -> first commit
  double total_us = 0.0;           // stage start -> final commit
  std::shared_ptr<const SliceServeRecord> record; // the final record
};
[[nodiscard]] Result<ServeBuildStage> serve_build(ServeBook &book, const ServeBookEntry &key,
                                                  const QuoteSliceFit &fit,
                                                  const ServeRecordPolicy &policy = {},
                                                  bool two_phase = true);

[[nodiscard]] const char *to_string(ServeRebuildReason value) noexcept;
[[nodiscard]] const char *to_string(ServeEnqueue value) noexcept;

} // namespace atx::vol
