// atx::engine::alpha — SubtreeCache + structural subtree hashing (Lane 2).
#include "atx/engine/alpha/subtree_cache.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "atx/core/hash.hpp"
#include "atx/core/macro.hpp"

namespace atx::engine::alpha {

namespace {

// splitmix64 finalizer: a bijective avalanche mix (every input bit affects every
// output bit with ~1/2 probability), used as the per-step compression function.
[[nodiscard]] constexpr atx::u64 fmix64(atx::u64 x) noexcept {
  x ^= x >> 30U;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27U;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31U;
  return x;
}

// Two independently seeded / pre-whitened 64-bit lanes folded in lock-step.
struct Hasher {
  atx::u64 lo{0x243F6A8885A308D3ULL};
  atx::u64 hi{0x13198A2E03707344ULL};

  void add(atx::u64 v) noexcept {
    lo = fmix64(lo ^ (v + 0x9e3779b97f4a7c15ULL + (lo << 6U) + (lo >> 2U)));
    hi = fmix64(hi + fmix64(v ^ 0xA4093822299F31D0ULL) * 0xff51afd7ed558ccdULL);
  }
  void add(const SubtreeHash &h) noexcept {
    add(h.lo);
    add(h.hi);
  }
  [[nodiscard]] SubtreeHash done() const noexcept { return SubtreeHash{lo, hi}; }
};

[[nodiscard]] atx::u64 f64_bits(atx::f64 v) noexcept {
  atx::u64 b = 0;
  static_assert(sizeof(b) == sizeof(v));
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

[[nodiscard]] bool is_compute(OpCode op) noexcept {
  return op != OpCode::Free && op != OpCode::StoreAlpha;
}

} // namespace

// ---------------------------------------------------------------------------
//  SubtreeKeyHash
// ---------------------------------------------------------------------------
std::size_t detail::SubtreeKeyHash::operator()(const SubtreeKey &k) const noexcept {
  return static_cast<std::size_t>(
      fmix64(k.node_hash.lo ^ fmix64(k.panel_digest + static_cast<atx::u64>(k.mode))));
}

// ---------------------------------------------------------------------------
//  SubtreeCache
// ---------------------------------------------------------------------------
std::shared_ptr<const PanelBuf> SubtreeCache::find(const SubtreeKey &key) const {
  const std::lock_guard<std::mutex> lock{mtx_};
  const auto it = map_.find(key);
  if (it == map_.end()) {
    ++stats_.misses;
    return nullptr;
  }
  ++stats_.hits;
  // Move to MRU. splice never invalidates list iterators, so the stored lru_pos
  // stays valid.
  lru_.splice(lru_.begin(), lru_, it->second.lru_pos);
  return it->second.buf;
}

bool SubtreeCache::contains(const SubtreeKey &key) const {
  const std::lock_guard<std::mutex> lock{mtx_};
  return map_.find(key) != map_.end();
}

void SubtreeCache::publish(const SubtreeKey &key, PanelBuf &&buf) {
  const std::size_t bytes = buf.size() * sizeof(atx::f64);
  // Build the immutable shared value OUTSIDE the lock (allocation + move only).
  auto value = std::make_shared<const PanelBuf>(std::move(buf));
  const std::lock_guard<std::mutex> lock{mtx_};
  if (bytes > budget_ || map_.find(key) != map_.end()) {
    ++stats_.rejected;
    return;
  }
  // Evict LRU entries until the new value fits. Bounded: each iteration removes
  // one entry, and an empty cache always fits (bytes <= budget_ checked above).
  while (stats_.bytes + bytes > budget_ && !lru_.empty()) {
    const SubtreeKey victim = lru_.back();
    const auto vit = map_.find(victim);
    ATX_ASSERT(vit != map_.end());
    stats_.bytes -= vit->second.buf->size() * sizeof(atx::f64);
    map_.erase(vit);
    lru_.pop_back();
    ++stats_.evictions;
  }
  lru_.push_front(key);
  map_.emplace(key, Entry{std::move(value), lru_.begin()});
  stats_.bytes += bytes;
  ++stats_.publishes;
  stats_.entries = map_.size();
}

CacheStats SubtreeCache::stats() const {
  const std::lock_guard<std::mutex> lock{mtx_};
  CacheStats s = stats_;
  s.entries = map_.size();
  return s;
}

void SubtreeCache::clear() {
  const std::lock_guard<std::mutex> lock{mtx_};
  map_.clear();
  lru_.clear();
  stats_ = CacheStats{};
}

// ---------------------------------------------------------------------------
//  panel_digest
// ---------------------------------------------------------------------------
atx::u64 panel_content_digest(const Panel &panel) {
  Hasher h;
  h.add(panel.dates());
  h.add(panel.instruments());
  h.add(panel.num_fields());
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    const std::string &name = panel.field_name(f);
    h.add(atx::core::hash_bytes(name.data(), name.size()));
    const std::span<const atx::f64> col = panel.field_all(static_cast<FieldId>(f));
    h.add(atx::core::hash_bytes(col.data(), col.size() * sizeof(atx::f64)));
  }
  // Universe mask, one date row at a time (bounded scratch, no per-cell hashing).
  std::vector<std::uint8_t> row(panel.instruments());
  for (atx::usize d = 0; d < panel.dates(); ++d) {
    for (atx::usize j = 0; j < panel.instruments(); ++j) {
      row[j] = panel.in_universe(static_cast<DateIdx>(d), j) ? std::uint8_t{1} : std::uint8_t{0};
    }
    h.add(atx::core::hash_bytes(row.data(), row.size()));
  }
  const SubtreeHash out = h.done();
  return out.lo ^ fmix64(out.hi);
}

// ---------------------------------------------------------------------------
//  subtree_hashes
// ---------------------------------------------------------------------------
void subtree_hashes(const Program &prog, std::vector<SubtreeHash> &out,
                    std::vector<SubtreeHash> &slot_hash) {
  out.assign(prog.code.size(), SubtreeHash{});
  slot_hash.assign(static_cast<atx::usize>(prog.num_slots) + 1U, SubtreeHash{});
  for (atx::usize i = 0; i < prog.code.size(); ++i) {
    const Instr &in = prog.code[i];
    if (!is_compute(in.op)) {
      continue;
    }
    Hasher h;
    h.add(static_cast<atx::u64>(in.op));
    h.add(static_cast<atx::u64>(in.n_out));
    h.add(f64_bits(in.imm[0]));
    h.add(f64_bits(in.imm[1]));
    if (in.op == OpCode::LoadField) {
      // Field NAME, never the per-program dictionary id.
      ATX_ASSERT(in.param < prog.fields.size());
      const std::string &nm = prog.fields[in.param];
      h.add(atx::core::hash_bytes(nm.data(), nm.size()));
      h.add(nm.size());
    } else {
      h.add(static_cast<atx::u64>(in.param));
    }
    for (atx::usize k = 0; k < in.src.size(); ++k) {
      const SlotId s = in.src[k];
      if (s == kNoSlot) {
        h.add(0xDEADBEEFULL + k); // arity marker: absent operand position
        continue;
      }
      ATX_ASSERT(s < slot_hash.size());
      h.add(slot_hash[s]);
    }
    const SubtreeHash node = h.done();
    out[i] = node;
    for (atx::usize k = 0; k < in.n_out; ++k) {
      const atx::usize s = static_cast<atx::usize>(in.dst) + k;
      ATX_ASSERT(s < slot_hash.size());
      slot_hash[s] = node; // a Pin disambiguates block members by its own param
    }
  }
}

std::vector<SubtreeHash> subtree_hashes(const Program &prog) {
  std::vector<SubtreeHash> out;
  std::vector<SubtreeHash> scratch;
  subtree_hashes(prog, out, scratch);
  return out;
}

bool subtree_cacheable(const Instr &in) noexcept {
  switch (in.op) {
  case OpCode::LoadField:
  case OpCode::Const:
  case OpCode::Pin:
  case OpCode::Free:
  case OpCode::StoreAlpha:
    return false;
  default:
    return in.n_out == 1;
  }
}

} // namespace atx::engine::alpha
