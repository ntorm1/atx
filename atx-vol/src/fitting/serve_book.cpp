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
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "fitting/serve_book_detail.hpp"
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
  struct Generation {
    std::uint64_t expiry_id;
    std::uint64_t value;
    std::uint64_t input_revision = 0;
  };
  // Includes tombstones for absent/retired expiries. A whole-book replacement
  // advances clear_generation, invalidating even requests for never-present IDs.
  std::vector<Generation> generations; // sorted by expiry_id, publish_mutex
  std::uint64_t clear_generation = 0;

  // The rebuild queue: a ring of queue_capacity requests and the expiries being rebuilt,
  // both guarded by queue_mutex (the ring never reallocates).
  std::mutex queue_mutex;
  std::condition_variable_any queue_cv; // workers wait for a request
  std::condition_variable_any idle_cv;  // wait_idle waits for an empty, quiet queue
  std::vector<ServeRebuildRequest> ring;
  std::size_t head = 0;
  std::size_t size = 0;
  struct Inflight {
    ServeRebuildRequest active;
    std::optional<ServeRebuildRequest> next;
    bool running = true;
  };
  // One follow-up slot per active expiry survives even when the ring is full.
  // Completed slots with a follow-up are popped before the ring, without growing it.
  std::vector<Inflight> inflight;
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
  std::atomic<std::uint64_t> rebuild_superseded{0};
  std::atomic<std::uint64_t> rebuild_followups{0};
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

  // Caller holds publish_mutex. No allocation on request/read paths.
  [[nodiscard]] Generation token(std::uint64_t id) const noexcept {
    const auto at = std::lower_bound(generations.begin(), generations.end(), id,
        [](const Generation &g, std::uint64_t key) { return g.expiry_id < key; });
    return at != generations.end() && at->expiry_id == id
        ? *at : Generation{id, clear_generation, 0};
  }

  // Explicit owner intent is an input change even without a published record.
  // Only this owner path may allocate a new token; reader enqueue never does.
  [[nodiscard]] std::uint64_t advance_input(std::uint64_t id) {
    const auto at = std::lower_bound(generations.begin(), generations.end(), id,
        [](const Generation &g, std::uint64_t key) { return g.expiry_id < key; });
    if (at == generations.end() || at->expiry_id != id) {
      generations.insert(at, Generation{id, clear_generation, 1});
      return 1;
    }
    if (at->input_revision == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("serve book: input revision exhausted");
    }
    return ++at->input_revision;
  }

  // One version from the current one and `ops` (applied in order), swapped in. Strong
  // guarantee: everything that can throw happens before the store.
  // A conditional commit returns zero if superseded, without publishing a version.
  [[nodiscard]] std::uint64_t commit(std::span<const Op> ops, bool replace,
      const ServeRebuildRequest *expected = nullptr) {
    std::vector<VersionPtr> dead;
    std::uint64_t committed = 0;
    {
      std::lock_guard<std::mutex> lock(publish_mutex);
      if (expected != nullptr) {
        // Owner requests advance input_revision under this same lock. Comparing
        // both token halves and publishing are indivisible with newer intent.
        const auto current_token = token(expected->expiry_id);
        if (current_token.value != expected->generation ||
            current_token.input_revision != expected->input_revision) return 0;
      }
      VersionPtr base = current.load(std::memory_order_acquire);
      if (base->number == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("serve book: generation exhausted");
      }
      auto next = std::make_shared<ServeBookVersion>();
      next->number = base->number + 1;
      std::vector<Generation> next_generations;
      if (!replace) next_generations = generations;
      if (!replace) next->entries = base->entries;
      std::vector<ServeBookEntry> &e = next->entries;
      for (const Op &op : ops) {
        if (!replace) {
          const auto token = std::lower_bound(next_generations.begin(), next_generations.end(),
              op.entry.expiry_id,
              [](const Generation &g, std::uint64_t id) { return g.expiry_id < id; });
          if (token != next_generations.end() && token->expiry_id == op.entry.expiry_id) {
            token->value = next->number;
            token->input_revision = 0;
          } else {
            next_generations.insert(token, Generation{op.entry.expiry_id, next->number});
          }
        }
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
      next->published_steady_ns = steady_ns();
      graveyard.reserve(graveyard.size() + 1);
      dead.reserve(graveyard.size() + 1);
      committed = next->number;
      generations.swap(next_generations);
      if (replace) clear_generation = committed;
      current.store(VersionPtr(std::move(next)), std::memory_order_release);
      number.store(committed, std::memory_order_release);
      graveyard.push_back(std::move(base));
      collect(dead);
    }
    published.fetch_add(1, std::memory_order_relaxed);
    reclaimed.fetch_add(dead.size(), std::memory_order_relaxed);
    return committed; // `dead` is freed here: on the publisher, outside the lock
  }

  // A reader rebuild request for `id` allocates nothing. Same-token reader requests
  // deduplicate. A new generation or explicit owner intent replaces queued work or
  // records one follow-up for running work. `check_moved`: refuse
  // (Moved) when the book published past `pinned`, so the reader answers from that.
  [[nodiscard]] Enq enqueue(std::uint64_t id, ServeRebuildReason reason, std::uint64_t pinned,
                            bool check_moved, bool owner_request = false) {
    if (!rebuild) return Enq::NoRebuild;
    {
      std::lock_guard<std::mutex> published_lock(publish_mutex);
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (stopping) return Enq::NoRebuild;
      if (check_moved && pinned != number.load(std::memory_order_acquire)) return Enq::Moved;
      const auto current_token = token(id);
      ServeRebuildRequest fresh{id, reason, steady_ns(), current_token.value,
                                 current_token.input_revision};
      const auto owner_intent = [&] {
        if (owner_request) fresh.input_revision = advance_input(id);
      };
      const auto changed = [&](const ServeRebuildRequest &old) {
        return old.generation != fresh.generation || old.input_revision != fresh.input_revision;
      };
      for (std::size_t i = 0; i < size; ++i) {
        auto &queued = ring[(head + i) % ring.size()];
        if (queued.expiry_id != id) continue;
        owner_intent();
        if (changed(queued)) queued = fresh;
        deduplicated.fetch_add(1, std::memory_order_relaxed);
        return Enq::Deduplicated;
      }
      for (auto &active : inflight) {
        if (active.active.expiry_id != id) continue;
        owner_intent();
        if (changed(active.active)) {
          if (active.running) active.next = fresh;
          else active.active = fresh;
        }
        deduplicated.fetch_add(1, std::memory_order_relaxed);
        return Enq::Deduplicated;
      }
      if (size == ring.size()) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return Enq::Full;
      }
      owner_intent(); // a refused/full request must not invalidate existing work
      ring[(head + size) % ring.size()] = fresh;
      ++size;
      enqueued.fetch_add(1, std::memory_order_relaxed);
    }
    queue_cv.notify_one();
    return Enq::Queued;
  }

  // Caller holds queue_mutex.
  [[nodiscard]] bool pending() const noexcept {
    return size > 0 || std::any_of(inflight.begin(), inflight.end(),
                                   [](const Inflight &x) { return !x.running; });
  }

  // Caller holds queue_mutex and pending() is true.
  [[nodiscard]] ServeRebuildRequest pop_locked() {
    for (auto &active : inflight) {
      if (!active.running) {
        active.running = true;
        return active.active;
      }
    }
    const ServeRebuildRequest q = ring[head];
    inflight.push_back(Inflight{q, std::nullopt, true});
    head = (head + 1) % ring.size();
    --size;
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
    bool superseded = false;
    try {
      std::optional<ServeBookEntry> e = rebuild(q);
      if (e && e->record) {
        e->expiry_id = q.expiry_id;
        const std::array<Op, 1> op{Op{false, std::move(*e)}};
        ok = commit(op, false, &q) != 0;
        superseded = !ok;
      }
    } catch (...) {
      ok = false;
    }
    if (ok) {
      const std::int64_t done = steady_ns() - q.enqueued_steady_ns;
      raise_max(rebuild_max, done);
      rebuild_sum.fetch_add(done, std::memory_order_relaxed);
      rebuilt.fetch_add(1, std::memory_order_relaxed);
    } else if (superseded) {
      rebuild_superseded.fetch_add(1, std::memory_order_relaxed);
    } else {
      rebuild_failed.fetch_add(1, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      const auto at = std::find_if(inflight.begin(), inflight.end(), [&](const Inflight &x) {
        return x.active.expiry_id == q.expiry_id;
      });
      if (at != inflight.end()) {
        if (at->next && !stopping) {
          at->active = *at->next;
          at->next.reset();
          at->running = false;
          enqueued.fetch_add(1, std::memory_order_relaxed);
          rebuild_followups.fetch_add(1, std::memory_order_relaxed);
        } else {
          inflight.erase(at);
        }
      }
    }
    queue_cv.notify_one();
    idle_cv.notify_all();
  }

  void work(std::stop_token stop) {
    for (;;) {
      ServeRebuildRequest q;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (!queue_cv.wait(lock, stop, [this] { return pending() || stopping; })) return;
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
  switch (state_->enqueue(expiry_id, reason, 0, false, true)) {
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
      if (!s.pending()) break;
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
  c.rebuild_superseded = s.rebuild_superseded.load();
  c.rebuild_followups = s.rebuild_followups.load();
  {
    std::lock_guard<std::mutex> lock(s.queue_mutex);
    c.queue_depth = s.size;
    c.queue_depth += static_cast<std::uint64_t>(std::count_if(s.inflight.begin(), s.inflight.end(),
        [](const State::Inflight &x) { return !x.running || x.next.has_value(); }));
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
    const Enq q = state_->enqueue(expiry_id, reason, version_->number, true);
    if (q == Enq::Moved) {
      if (!pinned && attempt == 0) continue; // one refresh; a chain keeps its pinned version
      return a; // never enqueue stale reader intent against a newer input token
    }
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
                                    std::span<ServedChainStatement> statements) {
  const std::size_t n = chain.size();
  if (!chain.consistent() || !out.consistent() || out.size() != n || statements.size() != n ||
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
    ServedChainStatement &st = statements[k];
    st.certificate_usd = a.certificate_usd;
    st.map_certificate_usd = a.map_certificate_usd;
    st.iv_lower = a.iv_lower;
    st.iv_upper = a.iv_upper;
    st.status = a.status;
    st.route = a.route;
    st.tier = a.tier;
    st.certificate_class = a.certificate_class;
    st.map_certificate_class = a.map_certificate_class;
    st.price_only_reason = a.price_only_reason;
    st.sampled = a.sampled;
    // review-H1 F1: a Sampled answer is never Ok on the status span.
    out.status[k] = a.status == ServeStatus::Ok && !a.sampled ? TickStatus::Ok
                    : a.status == ServeStatus::Exercised      ? TickStatus::Exercised
                    : std::isnan(a.price)                     ? TickStatus::Unpriceable
                                                              : TickStatus::Fallback;
  }
  return Ok();
}

// ── The serve_build stage ────────────────────────────────────────────────────────────────

struct detail::ServeBookAccess {
  static ServeRebuildRequest capture(const ServeBook &book, std::uint64_t id) {
    auto &state = *book.state_;
    std::lock_guard lock(state.publish_mutex);
    const auto token = state.token(id);
    return ServeRebuildRequest{id, ServeRebuildReason::Requested, steady_ns(),
                               token.value, token.input_revision};
  }
  static Result<std::uint64_t> publish(ServeBook &book, ServeBookEntry entry,
                                      const ServeRebuildRequest &expected) {
    const std::array<ServeBookTransaction::Op, 1> ops{
        ServeBookTransaction::Op{false, std::move(entry)}};
    const auto version = book.state_->commit(ops, false, &expected);
    if (version == 0) return Err(ErrorCode::Unavailable, "serve build: superseded expiry input");
    return Ok(version);
  }
};

Result<ServeBuildStage> serve_build(ServeBook &book, const ServeBookEntry &key,
                                    const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
                                    bool two_phase) {
  return detail::serve_build_observed(book, key, fit, policy, two_phase, {});
}

Result<ServeBuildStage> detail::serve_build_observed(ServeBook &book, const ServeBookEntry &key,
    const QuoteSliceFit &fit, const ServeRecordPolicy &policy, bool two_phase,
    const std::function<void()> &after_first) {
  const auto t0 = Clock::now();
  ServeRebuildRequest expected = detail::ServeBookAccess::capture(book, key.expiry_id);
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
    const Result<std::uint64_t> v1 = detail::ServeBookAccess::publish(book, e, expected);
    if (!v1) return Err(v1.error());
    out.first_version = *v1;
    expected.generation = *v1;
    expected.input_revision = 0;
    out.first_usable_us = us_since(t0);
    if (after_first) after_first();
  }
  Result<std::shared_ptr<const SliceServeRecord>> r2 =
      first ? detail::build_serve_record_on(fit, policy, *first) : build_serve_record(fit, policy);
  if (!r2) return Err(r2.error()); // phase 1, when published, stays live
  e.record = *r2;
  const Result<std::uint64_t> v2 = detail::ServeBookAccess::publish(book, e, expected);
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
