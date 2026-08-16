// Per-expiry PCP borrow memo — see carry_memo.hpp for the rationale and for why
// the reductions in the key are exact.

#include "fitting/carry_memo.hpp"

#include <cstdlib>
#include <cstring>
#include <shared_mutex>

namespace atx::vol::carry_memo {

namespace {

// Bounded ring (JPL Rule 3). Sized so every board in flight keeps its whole
// expiry strip resident: the outer fit fan-out is capped at the P-core count, a
// board carries a few dozen expiries, and a rung re-resolves them all. Overflow
// overwrites the OLDEST entry, so the memo degrades to extra solves, never to a
// wrong answer.
constexpr std::size_t kCapacity = 1024;

// FNV-1a over raw bytes. Used ONLY to pre-filter buckets — a hit still requires
// the full element-wise `same_key` compare below, so a collision costs a
// comparison, never a wrong carry.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] std::uint64_t hash_bytes(std::uint64_t h, const void *data, std::size_t n) noexcept {
  const auto *p = static_cast<const unsigned char *>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= static_cast<std::uint64_t>(p[i]);
    h *= kFnvPrime;
  }
  return h;
}

template <typename T>
[[nodiscard]] std::uint64_t hash_span(std::uint64_t h, std::span<const T> s) noexcept {
  const std::size_t n = s.size();
  h = hash_bytes(h, &n, sizeof(n));
  return s.empty() ? h : hash_bytes(h, s.data(), n * sizeof(T));
}

// Field-by-field, never `memcmp` over the struct: hashing an aggregate's padding
// bytes would make two equal keys hash differently and silently cost a re-solve.
[[nodiscard]] std::uint64_t hash_al_opts(std::uint64_t h, const AlOpts &o) noexcept {
  h = hash_bytes(h, &o.n_collocation, sizeof(o.n_collocation));
  h = hash_bytes(h, &o.n_quadrature, sizeof(o.n_quadrature));
  h = hash_bytes(h, &o.n_quad_price, sizeof(o.n_quad_price));
  h = hash_bytes(h, &o.max_newton_iter, sizeof(o.max_newton_iter));
  return hash_bytes(h, &o.tol, sizeof(o.tol));
}

[[nodiscard]] std::uint64_t hash_key(const Key &k) noexcept {
  std::uint64_t h = kFnvOffset;
  h = hash_bytes(h, &k.T, sizeof(k.T));
  h = hash_bytes(h, &k.forward_base, sizeof(k.forward_base));
  h = hash_bytes(h, &k.S, sizeof(k.S));
  h = hash_bytes(h, &k.r, sizeof(k.r));
  h = hash_bytes(h, &k.now_ts_ns, sizeof(k.now_ts_ns));
  h = hash_bytes(h, &k.method, sizeof(k.method));
  h = hash_bytes(h, &k.has_carry_al_opts, sizeof(k.has_carry_al_opts));
  h = hash_al_opts(h, k.carry_al_opts);
  h = hash_bytes(h, &k.selected_pairs, sizeof(k.selected_pairs));
  h = hash_bytes(h, &k.warm_start_carry, sizeof(k.warm_start_carry));
  h = hash_span(h, k.strikes);
  h = hash_span(h, k.bids);
  h = hash_span(h, k.asks);
  h = hash_span(h, k.mids);
  h = hash_span(h, k.flags);
  h = hash_span(h, k.ts_ns);
  return h;
}

// A key with its quote arrays owned, so an entry outlives the Chain it was
// solved from.
struct OwnedKey {
  double T{0.0};
  double forward_base{0.0};
  double S{0.0};
  double r{0.0};
  std::int64_t now_ts_ns{0};
  AmericanMethod method{AmericanMethod::AndersenLake};
  bool has_carry_al_opts{false};
  AlOpts carry_al_opts{};
  std::size_t selected_pairs{0};
  bool warm_start_carry{false};
  std::vector<double> strikes;
  std::vector<double> bids;
  std::vector<double> asks;
  std::vector<double> mids;
  std::vector<std::uint8_t> flags;
  std::vector<std::int64_t> ts_ns;
};

// EXACT equality — bitwise on the doubles, because that is what "the same market
// state" means here. `==` on double is deliberate and correct: these are stored
// quote values copied verbatim, never arithmetic results being compared for
// nearness. NaN cannot appear in a memoized key, because a chain carrying a
// non-finite quote fails `leg_quote_valid` upstream and a non-finite
// forward_base / S / r / T is refused before the memo is consulted.
template <typename T>
[[nodiscard]] bool same_seq(const std::vector<T> &a, std::span<const T> b) noexcept {
  return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(),
                                                           a.size() * sizeof(T)) == 0);
}

[[nodiscard]] bool same_al_opts(const AlOpts &a, const AlOpts &b) noexcept {
  return a.n_collocation == b.n_collocation && a.n_quadrature == b.n_quadrature &&
         a.n_quad_price == b.n_quad_price && a.max_newton_iter == b.max_newton_iter &&
         std::memcmp(&a.tol, &b.tol, sizeof(a.tol)) == 0;
}

[[nodiscard]] bool same_key(const OwnedKey &a, const Key &b) noexcept {
  return std::memcmp(&a.T, &b.T, sizeof(a.T)) == 0 &&
         std::memcmp(&a.forward_base, &b.forward_base, sizeof(a.forward_base)) == 0 &&
         std::memcmp(&a.S, &b.S, sizeof(a.S)) == 0 &&
         std::memcmp(&a.r, &b.r, sizeof(a.r)) == 0 && a.now_ts_ns == b.now_ts_ns &&
         a.method == b.method && a.has_carry_al_opts == b.has_carry_al_opts &&
         same_al_opts(a.carry_al_opts, b.carry_al_opts) &&
         a.selected_pairs == b.selected_pairs &&
         a.warm_start_carry == b.warm_start_carry && same_seq(a.strikes, b.strikes) &&
         same_seq(a.bids, b.bids) && same_seq(a.asks, b.asks) && same_seq(a.mids, b.mids) &&
         same_seq(a.flags, b.flags) && same_seq(a.ts_ns, b.ts_ns);
}

void adopt(OwnedKey &dst, const Key &src) {
  dst.T = src.T;
  dst.forward_base = src.forward_base;
  dst.S = src.S;
  dst.r = src.r;
  dst.now_ts_ns = src.now_ts_ns;
  dst.method = src.method;
  dst.has_carry_al_opts = src.has_carry_al_opts;
  dst.carry_al_opts = src.carry_al_opts;
  dst.selected_pairs = src.selected_pairs;
  dst.warm_start_carry = src.warm_start_carry;
  dst.strikes.assign(src.strikes.begin(), src.strikes.end());
  dst.bids.assign(src.bids.begin(), src.bids.end());
  dst.asks.assign(src.asks.begin(), src.asks.end());
  dst.mids.assign(src.mids.begin(), src.mids.end());
  dst.flags.assign(src.flags.begin(), src.flags.end());
  dst.ts_ns.assign(src.ts_ns.begin(), src.ts_ns.end());
}

struct Entry {
  std::uint64_t hash{0};
  bool live{false};
  OwnedKey key{};
  Value value{};
};

struct Table {
  std::shared_mutex mutex;
  std::vector<Entry> entries;
  std::size_t next{0}; // FIFO overwrite cursor
};

[[nodiscard]] Table &table() {
  static Table t;
  return t;
}

// `ATX_VOL_CARRY_MEMO=0` is the kill switch — the A/B lever that made the memo's
// speedup measurable in deterministic call counts, and the escape hatch if a
// future change ever makes the solve impure. Anything else (including unset)
// leaves the memo on. Same `getenv_s` shape as al_probe.cpp: plain `getenv` is a
// deprecation error under /W4 /WX here.
[[nodiscard]] bool read_enabled() noexcept {
#if defined(_MSC_VER)
  char buf[8] = {};
  std::size_t sz = 0;
  if (getenv_s(&sz, buf, sizeof(buf), "ATX_VOL_CARRY_MEMO") != 0 || sz == 0) {
    return true;
  }
  return !(buf[0] == '0' && buf[1] == '\0');
#else
  const char *const v = std::getenv("ATX_VOL_CARRY_MEMO");
  return v == nullptr || !(v[0] == '0' && v[1] == '\0');
#endif
}

const bool g_enabled = read_enabled();

} // namespace

bool enabled() noexcept { return g_enabled; }

bool lookup(const Key &key, Value &out) {
  if (!g_enabled) {
    return false;
  }
  const std::uint64_t h = hash_key(key);
  Table &t = table();
  const std::shared_lock<std::shared_mutex> guard{t.mutex};
  for (const Entry &e : t.entries) {
    if (e.live && e.hash == h && same_key(e.key, key)) {
      out = e.value;
      return true;
    }
  }
  return false;
}

void store(const Key &key, const Value &value) {
  if (!g_enabled) {
    return;
  }
  const std::uint64_t h = hash_key(key);
  Table &t = table();
  const std::unique_lock<std::shared_mutex> guard{t.mutex};
  for (const Entry &e : t.entries) {
    if (e.live && e.hash == h && same_key(e.key, key)) {
      return; // another worker published the same solve first; both agree
    }
  }
  if (t.entries.size() < kCapacity) {
    t.entries.emplace_back();
    Entry &slot = t.entries.back();
    slot.hash = h;
    slot.live = true;
    adopt(slot.key, key);
    slot.value = value;
    return;
  }
  Entry &slot = t.entries[t.next];
  t.next = (t.next + 1u) % kCapacity;
  slot.hash = h;
  slot.live = true;
  adopt(slot.key, key);
  slot.value = value;
}

void reset() {
  Table &t = table();
  const std::unique_lock<std::shared_mutex> guard{t.mutex};
  t.entries.clear();
  t.next = 0;
}

} // namespace atx::vol::carry_memo
