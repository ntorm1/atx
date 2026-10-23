// Live publication of serving records (lane H1, r6 2026-10-23): serve_book.hpp is the
// contract, docs/live-query.md the protocol and its measurements.
#include "atx/vol/api/fitting/serve_book.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "fitting/slice_serve_record_detail.hpp" // build_serve_record_on (the stage's phase 2)

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] double us_since(Clock::time_point t0) noexcept {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

void raise_max(std::atomic<std::int64_t> &slot, std::int64_t value) noexcept {
  std::int64_t seen = slot.load(std::memory_order_relaxed);
  while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

// Internal enqueue outcome: Moved = the book published past the reader's pinned version
// (the reader answers from the newer version instead of queueing).
enum class Enq : std::uint8_t { Queued, Deduplicated, Full, NoRebuild, Moved };

[[nodiscard]] bool has_instants(const ServeBookEntry &e) noexcept {
  return e.expiry_ns > 0 || e.next_event_ns > 0;
}

[[nodiscard]] bool invalidated(const ServeBookEntry &e, std::int64_t now_ns) noexcept {
  return (e.expiry_ns > 0 && now_ns >= e.expiry_ns) ||
         (e.next_event_ns > 0 && now_ns >= e.next_event_ns);
}

[[nodiscard]] ServeAnswer record_answer(const SliceServeRecord &r, const ServeRequest &q) {
  return r.serve(q);
}

[[nodiscard]] ServeIvAnswer record_answer(const SliceServeRecord &r, const ServeIvRequest &q) {
  return r.implied_vol(q);
}

[[nodiscard]] bool above(ServeTier tier, ServeTier cap) noexcept {
  return static_cast<std::uint8_t>(tier) > static_cast<std::uint8_t>(cap);
}

} // namespace

const ServeBookEntry *ServeBookVersion::find(std::uint64_t expiry_id) const noexcept {
  const auto at = std::lower_bound(
      entries.begin(), entries.end(), expiry_id,
      [](const ServeBookEntry &e, std::uint64_t id) noexcept { return e.expiry_id < id; });
  return at != entries.end() && at->expiry_id == expiry_id ? &*at : nullptr;
}

struct ServeBookTransaction::Op {
  bool retire = false;
  ServeBookEntry entry{};
};

struct ServeBook::State {
  using Op = ServeBookTransaction::Op;
  using VersionPtr = std::shared_ptr<const ServeBookVersion>;

  ServeBookPolicy policy;
  ServeRebuildFn rebuild;

  // Publication: the current version (an atomic shared_ptr; readers load it only when the
  // counter moved), the counter (stored after the version, release), and the retired
  // versions awaiting their last reader (guarded by publish_mutex).
  std::atomic<VersionPtr> current;
  std::atomic<std::uint64_t> number{0};
  std::mutex publish_mutex;
  std::vector<VersionPtr> graveyard;

  // The rebuild queue: a ring of queue_capacity requests and the expiries being rebuilt,
  // both guarded by queue_mutex (the ring never reallocates).
  std::mutex queue_mutex;
  std::condition_variable_any queue_cv; // workers wait for a request
  std::condition_variable_any idle_cv;  // wait_idle waits for an empty, quiet queue
  std::vector<ServeRebuildRequest> ring;
  std::size_t head = 0;
  std::size_t size = 0;
  std::vector<std::uint64_t> inflight;
  bool stopping = false;
  std::vector<std::jthread> workers; // joined by ~ServeBook, before the state can go

  std::atomic<std::uint64_t> published{0};
  std::atomic<std::uint64_t> aborted{0};
  std::atomic<std::uint64_t> reclaimed{0};
  std::atomic<std::uint64_t> enqueued{0};
  std::atomic<std::uint64_t> deduplicated{0};
  std::atomic<std::uint64_t> rejected{0};
  std::atomic<std::uint64_t> rebuilt{0};
  std::atomic<std::uint64_t> rebuild_failed{0};
  std::atomic<std::int64_t> queue_age_max{0};
  std::atomic<std::int64_t> rebuild_max{0};
  std::atomic<std::int64_t> queue_age_sum{0};
  std::atomic<std::int64_t> rebuild_sum{0};

  // Moves the retired versions only the graveyard holds into `dead` (freed by the caller
  // after the lock: never by a reader). Caller holds publish_mutex; `dead` has capacity.
  void collect(std::vector<VersionPtr> &dead) noexcept {
    std::size_t keep = 0;
    for (std::size_t i = 0; i < graveyard.size(); ++i) {
      if (graveyard[i].use_count() == 1) {
        dead.push_back(std::move(graveyard[i]));
      } else {
        if (keep != i) graveyard[keep] = std::move(graveyard[i]);
        ++keep;
      }
    }
    graveyard.resize(keep);
  }

  // One version from the current one and `ops` (applied in order), swapped in. Strong
  // guarantee: everything that can throw happens before the store.
  [[nodiscard]] std::uint64_t commit(std::span<const Op> ops, bool replace) {
    std::vector<VersionPtr> dead;
    std::uint64_t committed = 0;
    {
      std::lock_guard<std::mutex> lock(publish_mutex);
      VersionPtr base = current.load(std::memory_order_acquire);
      auto next = std::make_shared<ServeBookVersion>();
      if (!replace) next->entries = base->entries;
      std::vector<ServeBookEntry> &e = next->entries;
      for (const Op &op : ops) {
        const auto at = std::lower_bound(
            e.begin(), e.end(), op.entry.expiry_id,
            [](const ServeBookEntry &x, std::uint64_t id) noexcept { return x.expiry_id < id; });
        const bool present = at != e.end() && at->expiry_id == op.entry.expiry_id;
        if (op.retire) {
          if (present) e.erase(at);
        } else if (present) {
          *at = op.entry;
        } else {
          e.insert(at, op.entry);
        }
      }
      next->number = base->number + 1;
      next->published_steady_ns = steady_ns();
      graveyard.reserve(graveyard.size() + 1);
      dead.reserve(graveyard.size() + 1);
      committed = next->number;
      current.store(VersionPtr(std::move(next)), std::memory_order_release);
      number.store(committed, std::memory_order_release);
      graveyard.push_back(std::move(base));
      collect(dead);
    }
    published.fetch_add(1, std::memory_order_relaxed);
    reclaimed.fetch_add(dead.size(), std::memory_order_relaxed);
    return committed; // `dead` is freed here: on the publisher, outside the lock
  }

  // A rebuild request for `id`; allocates nothing. Deduplicated against the queued AND the
  // running requests (an expiry is rebuilt once per staleness). `check_moved`: refuse
  // (Moved) when the book published past `pinned`, so the reader answers from that.
  [[nodiscard]] Enq enqueue(std::uint64_t id, ServeRebuildReason reason, std::uint64_t pinned,
                            bool check_moved) {
    if (!rebuild) return Enq::NoRebuild;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (stopping) return Enq::NoRebuild;
      bool known = false;
      for (std::size_t i = 0; i < size && !known; ++i) {
        known = ring[(head + i) % ring.size()].expiry_id == id;
      }
      for (std::size_t i = 0; i < inflight.size() && !known; ++i) known = inflight[i] == id;
      if (known) {
        deduplicated.fetch_add(1, std::memory_order_relaxed);
        return Enq::Deduplicated;
      }
      if (check_moved && pinned != number.load(std::memory_order_acquire)) return Enq::Moved;
      if (size == ring.size()) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return Enq::Full;
      }
      ring[(head + size) % ring.size()] = ServeRebuildRequest{id, reason, steady_ns()};
      ++size;
      enqueued.fetch_add(1, std::memory_order_relaxed);
    }
    queue_cv.notify_one();
    return Enq::Queued;
  }

  // Caller holds queue_mutex and size > 0.
  [[nodiscard]] ServeRebuildRequest pop_locked() {
    const ServeRebuildRequest q = ring[head];
    head = (head + 1) % ring.size();
    --size;
    inflight.push_back(q.expiry_id);
    return q;
  }

  // One rebuild: the owner's function, then its entry published - or nothing, counted (a
  // publisher that dies publishes nothing: the old version stays live). The expiry leaves
  // the running set only after its version is published.
  void run(const ServeRebuildRequest &q) {
    const std::int64_t age = steady_ns() - q.enqueued_steady_ns;
    raise_max(queue_age_max, age);
    queue_age_sum.fetch_add(age, std::memory_order_relaxed);
    bool ok = false;
    try {
      std::optional<ServeBookEntry> e = rebuild(q);
      if (e && e->record) {
        e->expiry_id = q.expiry_id;
        const std::array<Op, 1> op{Op{false, std::move(*e)}};
        (void)commit(op, false);
        ok = true;
      }
    } catch (...) {
      ok = false;
    }
    if (ok) {
      const std::int64_t done = steady_ns() - q.enqueued_steady_ns;
      raise_max(rebuild_max, done);
      rebuild_sum.fetch_add(done, std::memory_order_relaxed);
      rebuilt.fetch_add(1, std::memory_order_relaxed);
    } else {
      rebuild_failed.fetch_add(1, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      const auto at = std::find(inflight.begin(), inflight.end(), q.expiry_id);
      if (at != inflight.end()) inflight.erase(at);
    }
    idle_cv.notify_all();
  }

  void work(std::stop_token stop) {
    for (;;) {
      ServeRebuildRequest q;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (!queue_cv.wait(lock, stop, [this] { return size > 0 || stopping; })) return;
        if (stopping) return;
        q = pop_locked();
      }
      run(q);
    }
  }
};

// ── Transactions ─────────────────────────────────────────────────────────────────────────

ServeBookTransaction::ServeBookTransaction(ServeBook &book) : book_(&book) {}

ServeBookTransaction::ServeBookTransaction(ServeBookTransaction &&other) noexcept
    : book_(other.book_), ops_(std::move(other.ops_)), replace_(other.replace_),
      done_(other.done_) {
  other.done_ = true;
  other.ops_.clear();
}

ServeBookTransaction::~ServeBookTransaction() {
  if (!done_ && book_ != nullptr && (replace_ || !ops_.empty())) {
    book_->state_->aborted.fetch_add(1, std::memory_order_relaxed);
  }
}

Status ServeBookTransaction::put(ServeBookEntry entry) {
  if (done_) return Err(ErrorCode::InvalidArgument, "serve book: transaction already committed");
  if (!entry.record) {
    return Err(ErrorCode::InvalidArgument, "serve book: an entry without a record");
  }
  ops_.push_back(Op{false, std::move(entry)});
  return Ok();
}

void ServeBookTransaction::retire(std::uint64_t expiry_id) {
  if (done_) return;
  Op op;
  op.retire = true;
  op.entry.expiry_id = expiry_id;
  ops_.push_back(std::move(op));
}

void ServeBookTransaction::retire_all() {
  if (done_) return;
  replace_ = true;
  ops_.clear(); // staged puts before it would be retired with the rest
}

Result<std::uint64_t> ServeBookTransaction::commit() {
  if (done_ || book_ == nullptr) {
    return Err(ErrorCode::InvalidArgument, "serve book: transaction already committed");
  }
  if (!replace_ && ops_.empty()) {
    done_ = true;
    return Ok(book_->version());
  }
  const std::uint64_t n = book_->state_->commit(ops_, replace_);
  done_ = true;
  ops_.clear();
  return Ok(n);
}

// ── The book ─────────────────────────────────────────────────────────────────────────────

ServeBook::ServeBook(const ServeBookPolicy &policy, ServeRebuildFn rebuild)
    : state_(std::make_shared<State>()) {
  State &s = *state_;
  s.policy = policy;
  s.policy.queue_capacity = std::max<std::size_t>(policy.queue_capacity, 1);
  s.rebuild = std::move(rebuild);
  s.current.store(std::make_shared<const ServeBookVersion>(), std::memory_order_release);
  s.ring.resize(s.policy.queue_capacity);
  s.inflight.reserve(static_cast<std::size_t>(policy.build_workers) + 8);
  s.workers.reserve(policy.build_workers);
  for (unsigned i = 0; i < policy.build_workers; ++i) {
    s.workers.emplace_back([state = state_.get()](std::stop_token stop) { state->work(stop); });
  }
}

ServeBook::~ServeBook() {
  State &s = *state_;
  {
    std::lock_guard<std::mutex> lock(s.queue_mutex);
    s.stopping = true;
  }
  for (std::jthread &w : s.workers) w.request_stop();
  s.queue_cv.notify_all();
  for (std::jthread &w : s.workers) {
    if (w.joinable()) w.join();
  }
  s.workers.clear();
}

ServeBookTransaction ServeBook::transaction() { return ServeBookTransaction(*this); }

Result<std::uint64_t> ServeBook::publish_expiry(ServeBookEntry entry) {
  ServeBookTransaction tx = transaction();
  const Status staged = tx.put(std::move(entry));
  if (!staged) return Err(staged.error());
  return tx.commit();
}

Result<std::uint64_t> ServeBook::publish(std::span<const ServeBookEntry> entries) {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].record) {
      return Err(ErrorCode::InvalidArgument, "serve book: an entry without a record");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (entries[j].expiry_id == entries[i].expiry_id) {
        return Err(ErrorCode::InvalidArgument, "serve book: an expiry published twice");
      }
    }
  }
  ServeBookTransaction tx = transaction();
  tx.retire_all();
  for (const ServeBookEntry &e : entries) {
    const Status staged = tx.put(e);
    if (!staged) return Err(staged.error());
  }
  return tx.commit();
}

std::uint64_t ServeBook::version() const noexcept {
  return state_->number.load(std::memory_order_acquire);
}

std::shared_ptr<const ServeBookVersion> ServeBook::current() const {
  return state_->current.load(std::memory_order_acquire);
}

ServeBookReader ServeBook::reader() const { return ServeBookReader(state_); }

const ServeBookPolicy &ServeBook::policy() const noexcept { return state_->policy; }

ServeEnqueue ServeBook::request_rebuild(std::uint64_t expiry_id, ServeRebuildReason reason) {
  switch (state_->enqueue(expiry_id, reason, 0, false)) {
  case Enq::Queued:
    return ServeEnqueue::Queued;
  case Enq::Deduplicated:
    return ServeEnqueue::Deduplicated;
  case Enq::Full:
    return ServeEnqueue::Full;
  case Enq::NoRebuild:
  case Enq::Moved:
    break;
  }
  return ServeEnqueue::NoRebuild;
}

std::size_t ServeBook::run_pending(std::size_t max) {
  State &s = *state_;
  std::size_t ran = 0;
  while (ran < max) {
    ServeRebuildRequest q;
    {
      std::lock_guard<std::mutex> lock(s.queue_mutex);
      if (s.size == 0) break;
      q = s.pop_locked();
    }
    s.run(q);
    ++ran;
  }
  return ran;
}

bool ServeBook::wait_idle(std::chrono::milliseconds timeout) {
  State &s = *state_;
  std::unique_lock<std::mutex> lock(s.queue_mutex);
  return s.idle_cv.wait_for(lock, timeout, [&s] { return s.size == 0 && s.inflight.empty(); });
}

std::size_t ServeBook::reclaim() {
  State &s = *state_;
  std::vector<State::VersionPtr> dead;
  {
    std::lock_guard<std::mutex> lock(s.publish_mutex);
    dead.reserve(s.graveyard.size());
    s.collect(dead);
  }
  s.reclaimed.fetch_add(dead.size(), std::memory_order_relaxed);
  return dead.size();
}

ServeBookCounters ServeBook::counters() const {
  State &s = *state_;
  ServeBookCounters c;
  c.published = s.published.load();
  c.aborted = s.aborted.load();
  c.reclaimed = s.reclaimed.load();
  {
    std::lock_guard<std::mutex> lock(s.publish_mutex);
    c.retired_held = s.graveyard.size();
  }
  c.enqueued = s.enqueued.load();
  c.deduplicated = s.deduplicated.load();
  c.rejected = s.rejected.load();
  c.rebuilt = s.rebuilt.load();
  c.rebuild_failed = s.rebuild_failed.load();
  {
    std::lock_guard<std::mutex> lock(s.queue_mutex);
    c.queue_depth = s.size;
  }
  c.queue_age_max_ns = s.queue_age_max.load();
  c.rebuild_max_ns = s.rebuild_max.load();
  c.queue_age_sum_ns = s.queue_age_sum.load();
  c.rebuild_sum_ns = s.rebuild_sum.load();
  return c;
}

// ── Readers ──────────────────────────────────────────────────────────────────────────────

ServeBookReader::ServeBookReader(std::shared_ptr<ServeBook::State> state)
    : state_(std::move(state)), version_(state_->current.load(std::memory_order_acquire)) {}

bool ServeBookReader::refresh() noexcept {
  const std::uint64_t n = state_->number.load(std::memory_order_acquire);
  if (n == version_->number) return false;
  // The version dropped here is the current one or a retired one the graveyard holds:
  // this assignment is never its last reference (readers never free).
  version_ = state_->current.load(std::memory_order_acquire);
  return true;
}

template <class Answer, class Request>
Answer ServeBookReader::answer(std::uint64_t expiry_id, const Request &request, bool pinned) {
  const ServeTier cap = state_->policy.primary_tier_max;
  for (int attempt = 0;; ++attempt) {
    if (!pinned) refresh();
    const ServeBookEntry *e = version_->find(expiry_id);
    Answer a{};
    bool queue = false;
    ServeRebuildReason reason = ServeRebuildReason::Requested;
    if (e == nullptr) {
      a.status = ServeStatus::TierUnavailable; // no record: today's path
      a.route = ServeRoute::ColdPath;
      queue = true;
      reason = ServeRebuildReason::Missing;
    } else if (has_instants(*e) && request.now_ns <= 0) {
      a.status = ServeStatus::Stale; // no clock to tell whether the geometry still applies
      a.route = ServeRoute::ColdPath;
    } else if (invalidated(*e, request.now_ns)) {
      a.status = ServeStatus::Invalidated;
      a.route = ServeRoute::Refit;
      queue = true;
      reason = ServeRebuildReason::Invalidated;
    } else if (above(request.tier, cap)) {
      a.status = ServeStatus::TierUnavailable; // above the primary tier: a route, no solve
      a.route = ServeRoute::ColdPath;
    } else {
      Request capped = request;
      if (above(capped.tier_max, cap)) capped.tier_max = cap;
      a = record_answer(*e->record, capped);
      if (a.route == ServeRoute::Rebuild) {
        queue = true;
        reason = ServeRebuildReason::Stale;
      }
    }
    if (!queue) return a;
    const Enq q = state_->enqueue(expiry_id, reason, version_->number, !pinned && attempt == 0);
    if (q == Enq::Moved) continue; // a newer version exists: answer from it
    if (q == Enq::Full && a.route == ServeRoute::Rebuild) a.status = ServeStatus::BudgetExhausted;
    return a;
  }
}

ServeAnswer ServeBookReader::serve(std::uint64_t expiry_id, const ServeRequest &request) {
  return answer<ServeAnswer>(expiry_id, request, false);
}

ServeIvAnswer ServeBookReader::implied_vol(std::uint64_t expiry_id,
                                           const ServeIvRequest &request) {
  return answer<ServeIvAnswer>(expiry_id, request, false);
}

Status ServeBookReader::serve_chain(double spot, std::int64_t now_ns,
                                    const ServedChainView &chain, ServeTier tier,
                                    ServeTier tier_max, const ChainTickOutputs &out,
                                    std::span<ServeStatus> served) {
  const std::size_t n = chain.size();
  if (!chain.consistent() || !out.consistent() || out.size() != n || served.size() != n ||
      !std::isfinite(spot) || !(spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "serve_chain: column lengths or spot");
  }
  refresh(); // ONE version for the whole chain
  ServeRequest q;
  q.spot = spot;
  q.tier = tier;
  q.tier_max = tier_max;
  q.greeks = ServeGreekMask::four();
  q.now_ns = now_ns;
  q.family_bound = false;
  for (std::size_t k = 0; k < n; ++k) {
    q.strike = chain.strike[k];
    q.side = chain.side[k];
    const ServeAnswer a = answer<ServeAnswer>(chain.expiry_id[k], q, true);
    out.price[k] = a.price;
    out.delta[k] = a.delta;
    out.gamma[k] = a.gamma;
    out.vega[k] = a.vega;
    out.theta[k] = a.theta;
    served[k] = a.status;
    out.status[k] = a.status == ServeStatus::Ok          ? TickStatus::Ok
                    : a.status == ServeStatus::Exercised ? TickStatus::Exercised
                    : std::isnan(a.price)                ? TickStatus::Unpriceable
                                                         : TickStatus::Fallback;
  }
  return Ok();
}

// ── The serve_build stage ────────────────────────────────────────────────────────────────

Result<ServeBuildStage> serve_build(ServeBook &book, const ServeBookEntry &key,
                                    const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
                                    bool two_phase) {
  const auto t0 = Clock::now();
  ServeBuildStage out;
  ServeBookEntry e = key;
  std::shared_ptr<const SliceServeRecord> first;
  if (two_phase && policy.ladders) {
    ServeRecordPolicy tiers12 = policy;
    tiers12.ladders = false;
    Result<std::shared_ptr<const SliceServeRecord>> r1 = build_serve_record(fit, tiers12);
    if (!r1) return Err(r1.error());
    first = *r1;
    e.record = first;
    const Result<std::uint64_t> v1 = book.publish_expiry(e);
    if (!v1) return Err(v1.error());
    out.first_version = *v1;
    out.first_usable_us = us_since(t0);
  }
  Result<std::shared_ptr<const SliceServeRecord>> r2 =
      first ? detail::build_serve_record_on(fit, policy, *first) : build_serve_record(fit, policy);
  if (!r2) return Err(r2.error()); // phase 1, when published, stays live
  e.record = *r2;
  const Result<std::uint64_t> v2 = book.publish_expiry(e);
  if (!v2) return Err(v2.error());
  out.final_version = *v2;
  out.total_us = us_since(t0);
  if (out.first_version == 0) {
    out.first_version = out.final_version;
    out.first_usable_us = out.total_us;
  }
  out.record = *r2;
  return Ok(std::move(out));
}

const char *to_string(ServeRebuildReason value) noexcept {
  switch (value) {
  case ServeRebuildReason::Requested:
    return "requested";
  case ServeRebuildReason::Stale:
    return "stale";
  case ServeRebuildReason::Invalidated:
    return "invalidated";
  case ServeRebuildReason::Missing:
    return "missing";
  }
  return "unknown";
}

const char *to_string(ServeEnqueue value) noexcept {
  switch (value) {
  case ServeEnqueue::Queued:
    return "queued";
  case ServeEnqueue::Deduplicated:
    return "deduplicated";
  case ServeEnqueue::Full:
    return "full";
  case ServeEnqueue::NoRebuild:
    return "no_rebuild";
  }
  return "unknown";
}

} // namespace atx::vol
