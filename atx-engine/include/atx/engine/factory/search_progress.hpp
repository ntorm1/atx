#pragma once

// atx::engine::factory — SearchProgressSink + SearchResumeState (resumable-discover).
//
// An abstract progress sink the genetic search loop calls ONCE per completed
// generation (after that generation's population is scored + ranked, before the
// next generation is reproduced) so a long-running discover can checkpoint the
// generation-input population and resume from the last completed generation after
// a crash. The sink is OPTIONAL: SearchDriver::run defaults it to nullptr and the
// nullptr path is byte-identical to the legacy loop (F1/F2). The engine never does
// I/O or wall-clock work itself — the sink is the only seam through which a caller
// may persist state, and it returns a Status so a real I/O failure (or an injected
// test crash) cleanly aborts the run with a well-formed partial result.

#include <algorithm> // std::sort (deterministic canon / cache key ordering)
#include <array>
#include <bit>     // std::bit_cast (bit-exact f64 <-> u64 serialization)
#include <cstdint> // std::uint64_t
#include <cstdio>  // std::snprintf (fixed-width hex codec)
#include <string>
#include <utility> // std::move
#include <vector>

#include "atx/core/error.hpp" // atx::core::Status, Result, Ok, Err
#include "atx/core/types.hpp" // atx::usize, atx::f64, atx::u64, atx::u8

#include "atx/engine/factory/behavior.hpp"  // factory::BehavioralArchive (archive contents)
#include "atx/engine/factory/canonical.hpp" // factory::CanonSet (dedup set)
#include "atx/engine/factory/fitness.hpp"   // factory::kMaxObjectives
#include "atx/engine/factory/search_state.hpp" // factory::CachedScore (cache value)

namespace atx::engine::factory {

// ===========================================================================
//  Bit-exact, deterministic serialization of the cross-generation ACCUMULATED
//  search state (resumable-discover, Task F1).
//
//  WHY: SearchDriver::run accumulates state ACROSS generations — the canon dedup
//  set, the per-canon fitness_cache, the behavioral-novelty archive, the folded
//  run digest, and the candidates_generated / best_fitness_per_gen counters. The
//  population checkpoint alone is insufficient: a resumed run that starts those
//  structures EMPTY diverges (different ranking -> selection -> reproduction ->
//  admission, and a different folded digest / trial_count). To make an
//  (uninterrupted) run and a (crash + resume) run BYTE-IDENTICAL, the checkpoint
//  must persist + restore this state losslessly.
//
//  DETERMINISM (F1 — non-negotiable):
//    * f64 is serialized BIT-EXACT: bit_cast to u64 then FIXED-WIDTH 16-char hex
//      (NEVER decimal to_string — a decimal round-trip is not guaranteed lossless).
//    * Collections serialize in a DETERMINISTIC order: canon as sorted u64s;
//      fitness_cache sorted by canon-hash key; the behavior_archive in its exact
//      ring/insertion order (so novelty() neighbourhoods match bit-for-bit).
//    * The codec is LOSSLESS: deserialize(serialize(x)) reproduces x exactly.
// ===========================================================================

// One u64 as a fixed-width 16-char lowercase hex token (zero-padded). The inverse
// of hex_to_u64; the pair is a lossless bijection for the full u64 range.
[[nodiscard]] inline std::string u64_to_hex(atx::u64 v) {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return std::string(b, 16);
}

// Parse a 16-char hex token back to u64. Returns false on a malformed token.
[[nodiscard]] inline bool hex_to_u64(const std::string &s, atx::u64 &out) {
  if (s.size() != 16) {
    return false;
  }
  unsigned long long v = 0;
  for (char c : s) {
    unsigned d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<unsigned>(c - 'a') + 10U;
    } else {
      return false; // not a lowercase hex digit
    }
    v = (v << 4U) | d;
  }
  out = static_cast<atx::u64>(v);
  return true;
}

// f64 -> 16-char hex of its IEEE-754 bit pattern (bit-exact; NaN/inf/±0 preserved).
[[nodiscard]] inline std::string f64_to_hex(atx::f64 x) {
  return u64_to_hex(std::bit_cast<atx::u64>(x));
}

// 16-char hex -> the exact f64 whose bit pattern it encodes. Returns false on a
// malformed token.
[[nodiscard]] inline bool hex_to_f64(const std::string &s, atx::f64 &out) {
  atx::u64 bits = 0;
  if (!hex_to_u64(s, bits)) {
    return false;
  }
  out = std::bit_cast<atx::f64>(bits);
  return true;
}

namespace detail {

// Split `s` on `sep` into tokens (empty `s` -> a single empty token, matching the
// natural inverse of join-with-sep). Deterministic; no allocation beyond the out vec.
[[nodiscard]] inline std::vector<std::string> split_on(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

} // namespace detail

// ===========================================================================
//  AccumulatedState — the full cross-generation search state ENTERING a generation,
//  carried through a checkpoint so a resumed run restores it byte-identically.
//
//  Captured at the TOP of the generation loop (BEFORE evaluate_generation runs for
//  that generation) so it is consistent with the population snapshot (which is also
//  the generation INPUT). Restoring this on resume makes gens [K..N) replay the
//  exact same trajectory + fold the exact same digest as an uninterrupted run.
// ===========================================================================
struct AccumulatedState {
  std::vector<atx::u64> canon;                   // dedup set, serialized as sorted u64s
  std::vector<atx::u64> cache_keys;              // fitness_cache canon-hash keys (sorted)
  std::vector<CachedScore> cache_vals;           // parallel to cache_keys (same order)
  std::vector<std::vector<atx::f64>> archive;    // behavior archive, exact ring order
  std::vector<atx::f64> best_fitness_per_gen;    // res.best_fitness_per_gen so far
  atx::u64 digest{0};                            // res.digest folded over gens [0..gen)
  atx::usize candidates_generated{0};            // res.candidates_generated so far
};

// ---- per-structure serialize / deserialize (bit-exact, deterministic order) ----
//
// Each emits a single self-delimiting std::string blob; the deserialize is the exact
// inverse (lossless round-trip — tested by AccumulatedStateRoundTrip). All f64 go
// through f64_to_hex (bit pattern), never decimal.

// canon: space-joined sorted u64 hex tokens. Sorting makes the blob independent of
// the unordered_set's iteration order (which is platform/allocation dependent).
[[nodiscard]] inline std::string serialize_canon(const CanonSet &canon) {
  std::vector<atx::u64> keys;
  keys.reserve(canon.size());
  for (atx::u64 h : canon.seen) {
    keys.push_back(h);
  }
  std::sort(keys.begin(), keys.end());
  std::string out;
  for (atx::usize i = 0; i < keys.size(); ++i) {
    if (i != 0) {
      out += ' ';
    }
    out += u64_to_hex(keys[i]);
  }
  return out;
}

[[nodiscard]] inline atx::core::Result<std::vector<atx::u64>>
deserialize_canon(const std::string &blob) {
  std::vector<atx::u64> out;
  if (blob.empty()) {
    return atx::core::Ok(std::move(out));
  }
  for (const std::string &tok : detail::split_on(blob, ' ')) {
    atx::u64 v = 0;
    if (!hex_to_u64(tok, v)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_canon: bad u64 token");
    }
    out.push_back(v);
  }
  return atx::core::Ok(std::move(out));
}

// One CachedScore as a fixed field sequence (all f64 bit-exact). Layout per record:
//   key raw n_objectives obj0..obj_{kMaxObjectives-1} n_desc desc0..desc_{n-1}
// Fields are space-joined; records are '\n'-joined. The key (canon_hash) is stored
// INLINE so a record is self-describing; the cache is emitted in sorted-key order.
[[nodiscard]] inline std::string serialize_cache(const std::vector<atx::u64> &keys,
                                                 const std::vector<CachedScore> &vals) {
  std::string out;
  for (atx::usize r = 0; r < keys.size(); ++r) {
    if (r != 0) {
      out += '\n';
    }
    const CachedScore &cs = vals[r];
    out += u64_to_hex(keys[r]);
    out += ' ';
    out += f64_to_hex(cs.raw);
    out += ' ';
    out += u64_to_hex(static_cast<atx::u64>(cs.n_objectives));
    for (atx::usize o = 0; o < kMaxObjectives; ++o) {
      out += ' ';
      out += f64_to_hex(cs.objectives[o]);
    }
    out += ' ';
    out += u64_to_hex(static_cast<atx::u64>(cs.descriptor.size()));
    for (atx::f64 d : cs.descriptor) {
      out += ' ';
      out += f64_to_hex(d);
    }
  }
  return out;
}

[[nodiscard]] inline atx::core::Status
deserialize_cache(const std::string &blob, std::vector<atx::u64> &keys,
                  std::vector<CachedScore> &vals) {
  keys.clear();
  vals.clear();
  if (blob.empty()) {
    return atx::core::Ok();
  }
  for (const std::string &line : detail::split_on(blob, '\n')) {
    const std::vector<std::string> f = detail::split_on(line, ' ');
    // Minimum fields: key, raw, n_objectives, kMaxObjectives objs, n_desc.
    const atx::usize fixed = 3 + kMaxObjectives + 1;
    if (f.size() < fixed) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: short record");
    }
    atx::usize idx = 0;
    atx::u64 key = 0;
    if (!hex_to_u64(f[idx++], key)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: bad key");
    }
    CachedScore cs;
    if (!hex_to_f64(f[idx++], cs.raw)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: bad raw");
    }
    atx::u64 nobj = 0;
    if (!hex_to_u64(f[idx++], nobj)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: bad n_objectives");
    }
    cs.n_objectives = static_cast<atx::u8>(nobj);
    for (atx::usize o = 0; o < kMaxObjectives; ++o) {
      if (!hex_to_f64(f[idx++], cs.objectives[o])) {
        return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: bad objective");
      }
    }
    atx::u64 ndesc = 0;
    if (!hex_to_u64(f[idx++], ndesc)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: bad n_desc");
    }
    if (f.size() != fixed + static_cast<atx::usize>(ndesc)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: desc count mismatch");
    }
    cs.descriptor.reserve(static_cast<atx::usize>(ndesc));
    for (atx::u64 d = 0; d < ndesc; ++d) {
      atx::f64 v = 0.0;
      if (!hex_to_f64(f[idx++], v)) {
        return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_cache: bad desc value");
      }
      cs.descriptor.push_back(v);
    }
    keys.push_back(key);
    vals.push_back(std::move(cs));
  }
  return atx::core::Ok();
}

// behavior archive: one entry per line (entries in ring order, oldest first); within
// a line, a leading count then the descriptor f64 hex tokens. An empty archive -> "".
[[nodiscard]] inline std::string
serialize_archive(const std::vector<std::vector<atx::f64>> &entries) {
  std::string out;
  for (atx::usize e = 0; e < entries.size(); ++e) {
    if (e != 0) {
      out += '\n';
    }
    out += u64_to_hex(static_cast<atx::u64>(entries[e].size()));
    for (atx::f64 d : entries[e]) {
      out += ' ';
      out += f64_to_hex(d);
    }
  }
  return out;
}

[[nodiscard]] inline atx::core::Result<std::vector<std::vector<atx::f64>>>
deserialize_archive(const std::string &blob) {
  std::vector<std::vector<atx::f64>> out;
  if (blob.empty()) {
    return atx::core::Ok(std::move(out));
  }
  for (const std::string &line : detail::split_on(blob, '\n')) {
    const std::vector<std::string> f = detail::split_on(line, ' ');
    if (f.empty()) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_archive: empty line");
    }
    atx::u64 n = 0;
    if (!hex_to_u64(f[0], n)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_archive: bad count");
    }
    if (f.size() != 1 + static_cast<atx::usize>(n)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_archive: count mismatch");
    }
    std::vector<atx::f64> entry;
    entry.reserve(static_cast<atx::usize>(n));
    for (atx::u64 i = 0; i < n; ++i) {
      atx::f64 v = 0.0;
      if (!hex_to_f64(f[1 + static_cast<atx::usize>(i)], v)) {
        return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_archive: bad value");
      }
      entry.push_back(v);
    }
    out.push_back(std::move(entry));
  }
  return atx::core::Ok(std::move(out));
}

// best_fitness_per_gen: space-joined f64 hex tokens (bit-exact). Empty vector -> "".
[[nodiscard]] inline std::string serialize_f64_list(const std::vector<atx::f64> &v) {
  std::string out;
  for (atx::usize i = 0; i < v.size(); ++i) {
    if (i != 0) {
      out += ' ';
    }
    out += f64_to_hex(v[i]);
  }
  return out;
}

[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
deserialize_f64_list(const std::string &blob) {
  std::vector<atx::f64> out;
  if (blob.empty()) {
    return atx::core::Ok(std::move(out));
  }
  for (const std::string &tok : detail::split_on(blob, ' ')) {
    atx::f64 v = 0.0;
    if (!hex_to_f64(tok, v)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "deserialize_f64_list: bad token");
    }
    out.push_back(v);
  }
  return atx::core::Ok(std::move(out));
}

// A read-only snapshot of one completed generation, handed to the sink. The
// `population` is the generation INPUT (the genomes that ENTERED generation
// `generation`), serialized as canonical DSL strings in canonical-id order — the
// exact blob a resume feeds back via SearchResumeState. The fitness fields and
// counts are pure telemetry (NOT part of the digest or the admitted set). The
// `*_blob` + digest/candidates fields carry the full ACCUMULATED state ENTERING
// `generation` (bit-exact, deterministic order) so a resume restores it losslessly.
struct GenerationSnapshot {
  atx::usize generation{0};            // 0-based index of the generation just scored
  std::vector<std::string> population; // unparse per genome, canonical-id order (gen INPUT)
  atx::f64 best_fitness{0.0};          // best RAW fitness this generation
  atx::f64 mean_fitness{0.0};          // mean RAW fitness over finite-scored members
  atx::usize n_evaluated{0};           // distinct candidates scored so far (CanonSet size)
  atx::usize n_unique{0};              // genomes in this generation's population
  // Accumulated-state blobs (state ENTERING `generation`, consistent with population).
  std::string canon_blob;        // serialize_canon(canon)
  std::string cache_blob;        // serialize_cache(fitness_cache, sorted by key)
  std::string archive_blob;      // serialize_archive(behavior_archive.entries())
  std::string best_per_gen_blob; // serialize_f64_list(res.best_fitness_per_gen)
  atx::u64 digest{0};            // res.digest folded over gens [0..generation)
  atx::usize candidates_generated{0}; // res.candidates_generated ENTERING `generation`
};

// Abstract progress sink. `on_generation` is invoked once per completed
// generation in ascending order. Returning an Err aborts the run cleanly
// (the driver finalizes the current scored set into a well-formed partial result
// and returns). A null sink (the default) disables the seam entirely.
class SearchProgressSink {
public:
  virtual ~SearchProgressSink() = default;
  [[nodiscard]] virtual atx::core::Status on_generation(const GenerationSnapshot &) = 0;
};

// Resume directive for SearchDriver::run. When supplied (and well-formed), the
// loop starts at `start_generation` from `population` (the canonical DSL strings
// captured in a prior GenerationSnapshot) instead of init_population.
// PipelineRecorder::latest_population_blob verifies the stored checkpoint
// state_hash against population_hash(blob) before returning the blob; a mismatch
// returns Err(Internal) which aborts the resume rather than silently resuming
// from a corrupt population. The driver's deserialize is a further backstop that
// refuses an incompatible blob rather than silently restarting from generation 0.
struct SearchResumeState {
  atx::usize start_generation{0};
  std::vector<std::string> population;
  // Accumulated state ENTERING start_generation (the full cross-generation state the
  // checkpoint persisted). Restored into the live canon / fitness_cache /
  // behavior_archive / res counters BEFORE the loop so gens [start_generation..N)
  // replay byte-identically to an uninterrupted run. These blobs are the same
  // strings the GenerationSnapshot carried; the driver deserializes them via the
  // free functions above. Empty blobs (legacy / pre-S4 path) restore nothing.
  std::string canon_blob;
  std::string cache_blob;
  std::string archive_blob;
  std::string best_per_gen_blob;
  atx::u64 digest{0};
  atx::usize candidates_generated{0};
};

} // namespace atx::engine::factory
