#pragma once

// atx::engine::parallel — lockstep grids over one shared input (platform v8 D-1).
//
// A grid runs L independent lanes (the scenario books of one construction, the
// construction variants of a sweep, the candidates of a mining pass) through one
// sequence of steps over ONE loaded input. Each step's shared work (a decision's
// desired target, price exposures, borrow tiers) is done once by the caller; every lane
// then advances on its own state. Two generic pieces live here, JSON-free and
// strategy-agnostic, so the NAV construction grid, mining and frontier sweeps share them:
//
//   1. GridVariant / validate_grid: a variant is "the base run plus these overrides",
//      (key, value) pairs in the client's own flag spelling. The engine knows no key:
//      each client passes its allowed set and applies the overrides with its own
//      parser, so a variant is by construction the standalone run with those flags.
//   2. for_each_lane: one per-lane phase of a step, sequential or on a DetPool, with a
//      deterministic error.
//
// DETERMINISM (DetPool's contract): a lane body writes only lane-owned state and reads
// shared state that no lane writes during the phase. Then every lane's bits are the
// same for any worker count, and the reported failure is the lowest failing lane's, the
// one a sequential loop reports.

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx::engine::parallel {

// One variant of a grid: its id (a file-system-safe label) and its overrides of the base
// configuration, in the spec's order.
struct GridVariant {
  std::string id;
  std::vector<std::pair<std::string, std::string>> overrides;
};

// What a client admits: at most max_variants variants, override keys from allowed_keys.
struct GridRules {
  atx::usize max_variants{16};
  std::span<const std::string_view> allowed_keys{};
};

// A grid id: 1..64 characters of [a-z0-9-] (a directory name on every file system).
[[nodiscard]] inline bool valid_grid_id(std::string_view id) noexcept {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}

// Ok iff 1 <= variants <= max_variants, every id is valid and unique, and every
// override key is allowed and appears at most once in its variant. InvalidArgument
// naming the first offending variant otherwise. O(V^2 + sum of overrides x allowed).
[[nodiscard]] inline atx::core::Status validate_grid(std::span<const GridVariant> variants,
                                                     const GridRules& rules) {
  if (variants.empty() || variants.size() > rules.max_variants)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "grid: 1.." + std::to_string(rules.max_variants) + " variants");
  for (atx::usize v = 0; v < variants.size(); ++v) {
    const auto& variant = variants[v];
    if (!valid_grid_id(variant.id))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "grid: variant ids are 1..64 of [a-z0-9-]");
    for (atx::usize u = 0; u < v; ++u)
      if (variants[u].id == variant.id)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "grid: duplicate variant id " + variant.id);
    for (atx::usize k = 0; k < variant.overrides.size(); ++k) {
      const auto& key = variant.overrides[k].first;
      const bool allowed = std::find(rules.allowed_keys.begin(), rules.allowed_keys.end(),
                                     std::string_view{key}) != rules.allowed_keys.end();
      if (!allowed)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "grid: variant " + variant.id + " may not set " + key);
      for (atx::usize j = 0; j < k; ++j)
        if (variant.overrides[j].first == key)
          return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                "grid: variant " + variant.id + " sets " + key + " twice");
    }
  }
  return atx::core::Ok();
}

// Runs body(lane) -> Status for every lane in [0, lanes).
//  - No pool, one worker or one lane: sequentially in lane order on this thread,
//    stopping at the first failure (the loop it replaces, operation for operation).
//  - Otherwise on the pool: every lane runs; after the barrier the status of the LOWEST
//    failing lane is returned (the sequential one). An exception thrown by a body is
//    rethrown by DetPool (lowest lane first).
// Allocates one Status per lane on the parallel path only.
template <class Body>
[[nodiscard]] atx::core::Status for_each_lane(DetPool* pool, atx::usize lanes, Body&& body) {
  if (pool == nullptr || pool->n_workers() < 2 || lanes < 2) {
    for (atx::usize lane = 0; lane < lanes; ++lane) {
      atx::core::Status status = body(lane);
      if (!status) return status;
    }
    return atx::core::Ok();
  }
  std::vector<atx::core::Status> statuses(lanes);
  // SAFETY (data races): lane i writes only statuses[i] and its own lane state (the
  // body's contract); parallel_for's barrier orders every write before the scan below.
  pool->parallel_for(lanes, [&](atx::usize lane, atx::usize /*worker*/) {
    statuses[lane] = body(lane);
  });
  for (auto& status : statuses)
    if (!status) return std::move(status);
  return atx::core::Ok();
}

} // namespace atx::engine::parallel
