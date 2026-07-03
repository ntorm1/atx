#include "stage_metabook.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/library/library.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/multi_horizon.hpp"

namespace atx::impl {

namespace risk = atx::engine::risk;
using library::AlphaId;

namespace {

// ===========================================================================
//  S2-1 — sleeve assignment (the SleeveSpec seam). See stage_metabook.hpp's doc
//  block for the contract; this file is the sole producer of std::vector<SleeveConfig>.
// ===========================================================================

// The per-sleeve MultiHorizonConfig: H=1, ONE identity-horizon source (assembled by
// S2-2's sources_at), MINIMAL GrossNet+PositionCap constraints -- the dispatch path
// that reduces risk::MultiHorizonOptimizer to risk::MultiPeriodOptimizer byte-for-byte
// (the R7 pin; risk_multi_horizon_integration_test.cpp's
// R7_DegenerateReducesToMultiPeriodByteIdentical, already landed and GREEN). EVERY
// sleeve in EVERY assignment mode gets this SAME shape -- only `members` differ.
risk::MultiHorizonConfig sleeve_mh_config(const MetaBookStageConfig &cfg) {
  risk::MultiHorizonConfig mh;
  mh.risk_aversion = cfg.risk_aversion;
  mh.constraints.gross.gross_leverage = cfg.gross;
  mh.constraints.gross.dollar_neutral = true;
  mh.constraints.pos = risk::PositionCap{cfg.name_cap};
  mh.horizon = 1;
  mh.trade_rate = 1.0;
  mh.stacked_mpc = false;
  mh.prox_max_iters = 64;
  mh.capacity_bound_gross = true;
  return mh;
}

fund::SleeveConfig make_sleeve(std::vector<AlphaId> members, fund::SleeveTag tag,
                               const MetaBookStageConfig &cfg) {
  fund::SleeveConfig sc;
  sc.mh = sleeve_mh_config(cfg);
  sc.members = std::move(members);
  sc.tag = std::move(tag);
  sc.capacity_gross = 1e9; // large sentinel (sleeve.hpp's own default) -- never binds the R7 pin
  return sc;
}

// SingleSleeve: ALL admitted AlphaIds ascending, ONE sleeve (mirrors stage_combine.cpp's
// library-enumeration order, :401-405 -- the R7 boundary pin's partition).
std::vector<fund::SleeveConfig> single_sleeve(atx::u64 n, const MetaBookStageConfig &cfg) {
  std::vector<AlphaId> members;
  members.reserve(static_cast<atx::usize>(n));
  for (atx::u64 a = 0; a < n; ++a) {
    members.push_back(AlphaId{static_cast<atx::u32>(a)});
  }
  std::vector<fund::SleeveConfig> out;
  out.push_back(make_sleeve(std::move(members), fund::SleeveTag{"US", "all"}, cfg));
  return out;
}

// Cap a first-appearance-ordered group list at `max_groups`: keep groups [0, max_groups-1)
// as-is and fold every group at/after (max_groups-1) into ONE trailing bucket (ascending
// AlphaId within it) -- deterministic, documented (overflow groups are merged, never
// dropped). A no-op when groups.size() <= max_groups already.
std::vector<std::vector<AlphaId>> cap_groups(std::vector<std::vector<AlphaId>> groups,
                                             atx::u32 max_groups) {
  if (max_groups == 0 || groups.size() <= static_cast<atx::usize>(max_groups)) {
    return groups;
  }
  std::vector<std::vector<AlphaId>> capped(groups.begin(),
                                           groups.begin() + static_cast<std::ptrdiff_t>(max_groups - 1));
  std::vector<AlphaId> rest;
  for (atx::usize g = max_groups - 1; g < groups.size(); ++g) {
    rest.insert(rest.end(), groups[g].begin(), groups[g].end());
  }
  std::sort(rest.begin(), rest.end(), [](AlphaId a, AlphaId b) { return a.value < b.value; });
  capped.push_back(std::move(rest));
  return capped;
}

// ByLibraryGroup: group admitted AlphaIds by the sealed-segment batch they fall in.
// `Library::segment_crc_per_alpha` (library.hpp:461-475, the exact per-alpha segment CRC) is
// a PRIVATE accessor -- library.hpp is NOT an S2-owned file (fund/*.hpp is; library/*.hpp is
// not), so S2 cannot reach into it. `Library::kFlushBatch` (the public flush-batch size every
// `admits/kFlushBatch`-th alpha seals a new segment at) is the public proxy for "which segment
// an alpha lands in": `AlphaId / kFlushBatch` is constant within a sealed segment and
// increments at exactly the same boundaries `segment_crc_per_alpha` would report, so this is
// a faithful (if coarser -- it does not distinguish two segments that happen to share a CRC)
// public substitute. First-appearance order across groups, ascending AlphaId within a group.
// A library smaller than kFlushBatch collapses into ONE group (batch 0 for everyone), so this
// mode naturally (and correctly) falls back to SingleSleeve for a small library (assign_sleeves'
// documented `< 2 groups` rule handles it) -- exactly the behavior a genuinely-unsealed,
// single-segment library should produce.
std::vector<std::vector<AlphaId>> group_by_segment(const library::Library &lib, atx::u32 max_groups) {
  const atx::u64 n = lib.n_alphas();
  std::vector<std::vector<AlphaId>> groups;
  std::vector<atx::u64> group_key; // group_key[g] == the batch index that opened group g
  for (atx::u64 a = 0; a < n; ++a) {
    const atx::u64 batch = a / static_cast<atx::u64>(library::Library::kFlushBatch);
    atx::usize g = group_key.size();
    for (atx::usize k = 0; k < group_key.size(); ++k) {
      if (group_key[k] == batch) {
        g = k;
        break;
      }
    }
    if (g == group_key.size()) {
      group_key.push_back(batch);
      groups.emplace_back();
    }
    groups[g].push_back(AlphaId{static_cast<atx::u32>(a)});
  }
  return cap_groups(std::move(groups), max_groups);
}

// Canonical DSL family label: the outermost token up to (not including) the first '(' --
// or the whole trimmed expression when it has no '(' (a bare literal/identifier alpha).
// E.g. "ts_rank(close, 20)" -> "ts_rank"; "rank(sub(close, open))" -> "rank".
std::string family_of(const std::string &expr) {
  const auto paren = expr.find('(');
  const std::string tok = (paren == std::string::npos) ? expr : expr.substr(0, paren);
  atx::usize b = 0;
  while (b < tok.size() && std::isspace(static_cast<unsigned char>(tok[b])) != 0) {
    ++b;
  }
  atx::usize e = tok.size();
  while (e > b && std::isspace(static_cast<unsigned char>(tok[e - 1])) != 0) {
    --e;
  }
  return tok.substr(b, e - b);
}

// BySignalFamily: one group per canonical family label (rec.provenance.expr_source's
// outermost op token, mirroring stage_combine.cpp's rec.provenance.expr_source read,
// :417), first-appearance order, ascending AlphaId within a group.
std::vector<std::vector<AlphaId>> group_by_family(const library::Library &lib, atx::u32 max_groups) {
  const atx::u64 n = lib.n_alphas();
  std::vector<std::vector<AlphaId>> groups;
  std::vector<std::string> group_key;
  for (atx::u64 a = 0; a < n; ++a) {
    const auto rec = lib.get(AlphaId{static_cast<atx::u32>(a)});
    const std::string fam = family_of(rec.provenance.expr_source);
    atx::usize g = group_key.size();
    for (atx::usize k = 0; k < group_key.size(); ++k) {
      if (group_key[k] == fam) {
        g = k;
        break;
      }
    }
    if (g == group_key.size()) {
      group_key.push_back(fam);
      groups.emplace_back();
    }
    groups[g].push_back(AlphaId{static_cast<atx::u32>(a)});
  }
  return cap_groups(std::move(groups), max_groups);
}

// Pearson correlation of two equal-length PnL series (population moments; library-admitted
// PnL is dense -- no NaN handling needed here, unlike the trailing-window sleeve_return_cov
// which must be NaN-aware for a partial history). A degenerate (zero-variance) series
// correlates as 0 (no linear relationship is definable), never NaN/Inf.
atx::f64 pearson(std::span<const atx::f64> a, std::span<const atx::f64> b) {
  const atx::usize n = a.size();
  atx::f64 ma = 0.0;
  atx::f64 mb = 0.0;
  for (atx::usize t = 0; t < n; ++t) {
    ma += a[t];
    mb += b[t];
  }
  ma /= static_cast<atx::f64>(n);
  mb /= static_cast<atx::f64>(n);
  atx::f64 cov = 0.0;
  atx::f64 va = 0.0;
  atx::f64 vb = 0.0;
  for (atx::usize t = 0; t < n; ++t) {
    const atx::f64 da = a[t] - ma;
    const atx::f64 db = b[t] - mb;
    cov += da * db;
    va += da * da;
    vb += db * db;
  }
  if (va <= 0.0 || vb <= 0.0) {
    return 0.0;
  }
  return cov / std::sqrt(va * vb);
}

// Below this single-linkage similarity, two clusters are considered structurally unrelated
// and merging stops (unless the cluster count is still over the cap). Documented constant:
// distance = 1 - corr, so a 0.3 cutoff merges anything with corr > 0.3 while capped clusters
// still merge past it if needed to respect max_sleeves.
constexpr atx::f64 kClusterCorrCutoff = 0.3;

// ByCorrCluster: deterministic single-linkage agglomerative clustering of the admitted-pool
// PnL correlation matrix. Distance = 1 - corr; each step merges the pair of clusters with
// the LARGEST single-linkage similarity (max pairwise corr across members) -- i.e. the
// smallest distance. Stops when the best remaining cross-cluster similarity is <=
// kClusterCorrCutoff AND the cluster count is already <= max_clusters (whichever binds).
// Deterministic tie-break: cluster pairs are scanned in ascending (i, j) order and the
// FIRST strict maximum wins, so a tie always resolves to the lowest-indexed pair -- stable
// across repeated calls on the same input (no RNG, no hashing, no unordered_*). O(n^3): the
// admission pool is a cold, run-scale collection (not a hot per-period path).
std::vector<std::vector<AlphaId>> corr_clusters(const library::Library &lib, atx::u32 max_clusters) {
  const atx::u64 n = lib.n_alphas();
  std::vector<std::vector<AlphaId>> clusters;
  clusters.reserve(static_cast<atx::usize>(n));
  for (atx::u64 a = 0; a < n; ++a) {
    clusters.push_back({AlphaId{static_cast<atx::u32>(a)}});
  }
  if (n < 2) {
    return clusters;
  }

  std::vector<std::vector<atx::f64>> corr(static_cast<atx::usize>(n),
                                          std::vector<atx::f64>(static_cast<atx::usize>(n), 0.0));
  for (atx::u64 a = 0; a < n; ++a) {
    corr[a][a] = 1.0;
    for (atx::u64 b = a + 1; b < n; ++b) {
      const atx::f64 c = pearson(lib.pnl(AlphaId{static_cast<atx::u32>(a)}),
                                 lib.pnl(AlphaId{static_cast<atx::u32>(b)}));
      corr[a][b] = c;
      corr[b][a] = c;
    }
  }

  while (clusters.size() > 1) {
    atx::usize best_i = 0;
    atx::usize best_j = 0;
    atx::f64 best_sim = -2.0; // below any valid correlation ([-1, 1])
    for (atx::usize i = 0; i < clusters.size(); ++i) {
      for (atx::usize j = i + 1; j < clusters.size(); ++j) {
        atx::f64 sim = -2.0;
        for (const AlphaId &x : clusters[i]) {
          for (const AlphaId &y : clusters[j]) {
            sim = std::max(sim, corr[x.value][y.value]);
          }
        }
        if (sim > best_sim) {
          best_sim = sim;
          best_i = i;
          best_j = j;
        }
      }
    }
    const bool over_cap = clusters.size() > static_cast<atx::usize>(max_clusters);
    if (!over_cap && best_sim <= kClusterCorrCutoff) {
      break; // no more natural structure to merge, and already within the cap
    }
    clusters[best_i].insert(clusters[best_i].end(), clusters[best_j].begin(), clusters[best_j].end());
    std::sort(clusters[best_i].begin(), clusters[best_i].end(),
             [](AlphaId a, AlphaId b) { return a.value < b.value; });
    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(best_j));
  }

  std::sort(clusters.begin(), clusters.end(),
           [](const std::vector<AlphaId> &a, const std::vector<AlphaId> &b) {
             return a.front().value < b.front().value;
           });
  return clusters;
}

std::vector<fund::SleeveConfig> sleeves_from_groups(std::vector<std::vector<AlphaId>> groups,
                                                    const std::string &family_prefix,
                                                    const MetaBookStageConfig &cfg) {
  std::vector<fund::SleeveConfig> out;
  out.reserve(groups.size());
  for (atx::usize g = 0; g < groups.size(); ++g) {
    fund::SleeveTag tag{"US", family_prefix + std::to_string(g)};
    out.push_back(make_sleeve(std::move(groups[g]), std::move(tag), cfg));
  }
  return out;
}

} // namespace

atx::core::Result<std::vector<fund::SleeveConfig>>
assign_sleeves(const library::Library &lib, const MetaBookStageConfig &cfg) {
  const atx::u64 n = lib.n_alphas();
  if (n == 0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "assign_sleeves: library has no admitted alphas");
  }

  switch (cfg.assignment) {
  case SleeveAssignment::SingleSleeve:
    return atx::core::Ok(single_sleeve(n, cfg));
  case SleeveAssignment::ByLibraryGroup: {
    auto groups = group_by_segment(lib, cfg.max_sleeves);
    if (groups.size() < 2) {
      return atx::core::Ok(single_sleeve(n, cfg));
    }
    return atx::core::Ok(sleeves_from_groups(std::move(groups), "group_", cfg));
  }
  case SleeveAssignment::ByCorrCluster: {
    auto clusters = corr_clusters(lib, cfg.max_sleeves);
    if (clusters.size() < 2) {
      return atx::core::Ok(single_sleeve(n, cfg));
    }
    return atx::core::Ok(sleeves_from_groups(std::move(clusters), "cluster_", cfg));
  }
  case SleeveAssignment::BySignalFamily: {
    auto fams = group_by_family(lib, cfg.max_sleeves);
    if (fams.size() < 2) {
      return atx::core::Ok(single_sleeve(n, cfg));
    }
    return atx::core::Ok(sleeves_from_groups(std::move(fams), "family_", cfg));
  }
  }
  // Unreachable given SleeveAssignment's fixed 4 enumerators; kept only to satisfy
  // -Wreturn-type (no real code path reaches this).
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        "assign_sleeves: unknown SleeveAssignment");
}

} // namespace atx::impl
