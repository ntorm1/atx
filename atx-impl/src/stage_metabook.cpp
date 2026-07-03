#include "stage_metabook.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile_batch, alpha::Program
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel
#include "atx/engine/alpha/parser.hpp"   // alpha::Library (DSL registry)
#include "atx/engine/alpha/streams.hpp"  // alpha::extract_streams, alpha::AlphaStreams
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine
#include "atx/engine/combine/gate.hpp"   // combine::GateConfig (read-only library open)
#include "atx/engine/library/library.hpp"
#include "atx/engine/loop/weight_policy.hpp" // atx::engine::WeightPolicy
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/multi_horizon.hpp"

#include "artifacts.hpp"      // to_hex16
#include "diag_risk.hpp"      // diagonal_risk_model (the shared S5/S6 diagonal model)
#include "research_sim.hpp"   // frictionless_sim (shared atx-impl helper)
#include "serialize_panel.hpp" // read_panel / write_panel

namespace atx::impl {

namespace alpha = atx::engine::alpha;
namespace combine = atx::engine::combine;
namespace risk = atx::engine::risk;
using atx::engine::WeightPolicy;
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

// ===========================================================================
//  S2-2 — the two-pass drive producer.
// ===========================================================================

// Evaluate one sleeve's member alphas over `research` and locally combine them into
// ONE per-period signal (D*M flat, ascending period then instrument) via an
// UNWEIGHTED, NaN-skipping cross-sectional mean of each member's position
// (target-weight) stream -- the SAME per-alpha representation stage_combine.cpp
// blends (streams.positions(a,t), stage_combine.cpp:753-765), but combined LOCALLY
// per sleeve with EQUAL weights (documented: NOT the calibrated
// stage_combine::AlphaCombiner fit -- Sprint-3-owned, not re-derived by S2). Used
// ONLY by multi-sleeve assignment modes; SingleSleeve-with-no-library instead reads
// the pre-existing combo panel directly (build_metabook_result).
atx::core::Result<std::vector<atx::f64>>
evaluate_sleeve_signal(const library::Library &lib, const std::vector<AlphaId> &members,
                       const alpha::Panel &research) {
  if (members.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "evaluate_sleeve_signal: sleeve has no members");
  }
  std::vector<std::string> dsl;
  dsl.reserve(members.size());
  for (const AlphaId id : members) {
    dsl.push_back(lib.get(id).provenance.expr_source);
  }
  alpha::Library dsl_lib{};
  const std::vector<std::string_view> views(dsl.begin(), dsl.end());
  ATX_TRY(auto program, alpha::compile_batch(std::span<const std::string_view>{views}, dsl_lib));
  alpha::Engine engine{research};
  ATX_TRY(auto signals, engine.evaluate(program));
  const WeightPolicy policy{};
  const auto sim = frictionless_sim();
  ATX_TRY(auto streams,
         alpha::extract_streams(signals, policy, research, sim, std::span<const atx::u32>{}));

  const atx::usize D = streams.n_periods();
  const atx::usize M = streams.n_instruments();
  const atx::usize na = streams.n_alphas();
  std::vector<atx::f64> out(D * M, std::numeric_limits<atx::f64>::quiet_NaN());
  for (atx::usize t = 0; t < D; ++t) {
    for (atx::usize i = 0; i < M; ++i) {
      atx::f64 sum = 0.0;
      atx::usize n = 0;
      for (atx::usize a = 0; a < na; ++a) {
        const atx::f64 v = streams.positions(a, t)[i];
        if (!std::isnan(v)) {
          sum += v;
          ++n;
        }
      }
      out[t * M + i] =
          (n == 0) ? std::numeric_limits<atx::f64>::quiet_NaN() : sum / static_cast<atx::f64>(n);
    }
  }
  return atx::core::Ok(std::move(out));
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

atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg_in) {
  if (cfg.panel.empty() || cfg.combo.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: --panel and --combo required");
  }
  ATX_TRY(auto research, read_panel(cfg.panel));
  ATX_TRY(auto combo, read_panel(cfg.combo));
  if (combo.num_fields() < 1) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: combo panel must have at least one field");
  }
  if (combo.instruments() != research.instruments()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: combo and research instrument counts differ");
  }
  if (combo.dates() != research.dates()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: combo and research date counts differ");
  }

  const atx::usize M = research.instruments();
  const atx::usize D = research.dates();

  // Schedule: EXACTLY stage_optimize.cpp's construction (same --rebalance validation, same
  // step derivation) -- load-bearing for the R7 stage-boundary pin (SingleSleeve, no library).
  if (!cfg.rebalance.empty() && cfg.rebalance != "daily" && cfg.rebalance != "weekly") {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: --rebalance must be 'daily' or 'weekly' (got: " +
                              cfg.rebalance + ")");
  }
  const atx::usize step = (cfg.rebalance == "daily") ? 1U : 5U; // default weekly
  risk::RebalanceSchedule sched;
  for (atx::usize d = 0; d < D; d += step) {
    sched.periods.push_back(d);
  }

  // Resolve gross/name_cap/risk_aversion EXACTLY as stage_optimize.cpp resolves
  // gross_val/name_cap_val/risk_aversion, so a SingleSleeve sleeve's mh matches the
  // deployed MVO's OptimizerConfig field-for-field (the R7 pin).
  MetaBookStageConfig scfg = scfg_in;
  scfg.gross = cfg.gross > 0.0 ? cfg.gross : 1.0;
  scfg.name_cap = cfg.name_cap > 0.0 ? cfg.name_cap : 1.0;
  scfg.risk_aversion = cfg.set_flags.count("risk-aversion") ? cfg.risk_aversion : 1.0;

  // Resolve sleeves + their per-period signal. SingleSleeve with NO --library-dir (the true
  // inert default) needs no library: one sleeve, sourced directly from the combo panel's
  // "alpha" field (the SAME slice stage_optimize.cpp's alpha_at reads). Any other invocation
  // requires a library (multi-sleeve assignment must partition an admitted pool; SingleSleeve
  // WITH an explicit library re-evaluates that whole pool locally instead of reading combo --
  // see the header doc for why that variant does not claim byte-identity to stage_optimize).
  const bool single_no_lib =
      (scfg.assignment == SleeveAssignment::SingleSleeve) && cfg.library_dir.empty();

  std::vector<fund::SleeveConfig> sleeve_cfgs;
  std::optional<library::Library> lib_holder;
  if (single_no_lib) {
    fund::SleeveConfig sc;
    sc.mh = sleeve_mh_config(scfg);
    sc.tag = fund::SleeveTag{"US", "all"};
    sc.capacity_gross = 1e9;
    sleeve_cfgs.push_back(std::move(sc)); // members left empty: sourced from combo, not re-evaluated
  } else {
    if (cfg.library_dir.empty()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "metabook: --library-dir required for this SleeveAssignment");
    }
    lib_holder.emplace(library::Library::open(cfg.library_dir, combine::GateConfig{}, {}));
    ATX_TRY(auto assigned, assign_sleeves(*lib_holder, scfg));
    sleeve_cfgs = std::move(assigned);
  }
  const atx::usize n_sleeves = sleeve_cfgs.size();

  // Per-sleeve per-period signal, flat D*M ascending date then instrument (the RAW DATE
  // index convention every fund/risk callback in this codebase uses -- confirmed at
  // src/fund/meta_book.cpp: `returns_at(sched.periods[p])`, `model_at(sched.periods[n-1])`).
  // NOTE (S2-5 wiring fix): the combo panel's "alpha" field is read ONLY on the single_no_lib
  // path -- the multi-sleeve / library path never touches combo's field contents (it
  // re-evaluates each sleeve's own members from the library instead), so looking up
  // "alpha" unconditionally would spuriously require a field the library path never needs.
  std::vector<std::vector<atx::f64>> sleeve_signal(n_sleeves);
  if (single_no_lib) {
    ATX_TRY(const auto alpha_fid, combo.field_id("alpha"));
    sleeve_signal[0].assign(D * M, 0.0);
    for (atx::usize t = 0; t < D; ++t) {
      const auto cs = combo.field_cross_section(alpha_fid, t);
      std::copy(cs.begin(), cs.end(),
               sleeve_signal[0].begin() + static_cast<std::ptrdiff_t>(t * M));
    }
  } else {
    for (atx::usize j = 0; j < n_sleeves; ++j) {
      ATX_TRY(auto sig, evaluate_sleeve_signal(*lib_holder, sleeve_cfgs[j].members, research));
      sleeve_signal[j] = std::move(sig);
    }
  }

  // model_at: diag_risk.hpp's diagonal_risk_model(research) -- the SAME model
  // stage_optimize's Diagonal path uses (stage_optimize.cpp:202/243-245). No Factor-model
  // variant is wired here (S1/S5 seam; recorded in the ledger).
  ATX_TRY(auto model, diagonal_risk_model(research));

  // returns_at: realized per-instrument simple return from research's "close" field, the
  // SAME TRI-return convention diag_risk.hpp computes. Drives Ω + the report, NOT the books
  // (meta_book.hpp:35-40). Period 0 has no prior close -> stays the structural zero.
  ATX_TRY(const auto close_fid, research.field_id("close"));
  const auto close = research.field_all(close_fid);
  std::vector<std::vector<atx::f64>> returns(D, std::vector<atx::f64>(M, 0.0));
  for (atx::usize t = 1; t < D; ++t) {
    for (atx::usize i = 0; i < M; ++i) {
      const atx::f64 p0 = close[(t - 1) * M + i];
      const atx::f64 p1 = close[t * M + i];
      returns[t][i] =
          (!std::isnan(p0) && !std::isnan(p1) && p0 != 0.0) ? (p1 / p0 - 1.0) : 0.0;
    }
  }

  atx::engine::book::CostInputs cost;
  cost.kappa = cfg.turnover_penalty;
  cost.round_trip_cost_bps = cfg.set_flags.count("cost-bps") ? cfg.cost_bps : 0.0;

  // The R7 boundary override: whenever the resolved partition is EXACTLY one sleeve (whether
  // by an explicit SingleSleeve assignment or a multi-sleeve mode's documented degenerate
  // fallback -- assign_sleeves' own "a one-sleeve partition IS the inert path" contract),
  // fractional_kelly=1.0 yields c==[1.0] every period (target_vol=0 and max_gross=4 are
  // already the engine defaults and never bind at S=1) -- the boundary config
  // fund_meta_book_integration_test.cpp's R7 test pins. Combined with Sleeve::run's pure
  // delegation and one-sleeve netting (net==gross, no crossing, structural), the fund book
  // then equals that ONE sleeve's own MultiHorizonResult.books byte-for-byte.
  fund::MetaBookConfig meta = scfg.meta;
  if (n_sleeves == 1) {
    meta.alloc.fractional_kelly = 1.0;
  }

  fund::MetaBook mb;
  mb.cfg = meta;
  mb.sleeves.reserve(n_sleeves);
  for (auto &sc : sleeve_cfgs) {
    mb.sleeves.push_back(fund::Sleeve{std::move(sc)});
  }

  const auto sources_at = [&](atx::usize sleeve, atx::usize period) -> risk::HorizonSources {
    risk::HorizonSources hs;
    const std::span<const atx::f64> row{sleeve_signal[sleeve].data() + period * M, M};
    hs.pairs.emplace_back(row, risk::SignalHorizon::identity());
    return hs;
  };
  const auto model_at = [&](atx::usize) -> const risk::FactorModel & { return model; };
  const auto returns_at = [&](atx::usize period) -> std::span<const atx::f64> {
    return std::span<const atx::f64>{returns[period]};
  };

  return mb.run(sched, sources_at, model_at, returns_at, cost);
}

atx::core::Result<StageResult> run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg) {
  if (cfg.panel.empty() || cfg.combo.empty() || cfg.books_out.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: --panel, --combo, and --out required");
  }
  ATX_TRY(auto result, build_metabook_result(cfg, scfg));

  const atx::usize S = result.fund_books.size();
  const atx::usize M = (S > 0) ? result.fund_books[0].size() : 0U;
  std::vector<atx::f64> flat;
  flat.reserve(S * M);
  for (const auto &book : result.fund_books) {
    flat.insert(flat.end(), book.begin(), book.end());
  }

  std::vector<std::uint8_t> uni(S * M, 1U);
  ATX_TRY(auto cpanel, alpha::Panel::create(S, M, {"weight"}, {flat}, uni));
  ATX_TRY(auto digest, write_panel(cpanel, cfg.books_out));

  // Sidecar .meta.txt: a drop-in for stage_report per the sprint's run_all seam note, plus
  // the S2-3 netting telemetry per period (surfaced here too for offline inspection; the
  // authoritative telemetry surface is StageResult::kvs below).
  {
    const std::string sidecar = cfg.books_out + ".meta.txt";
    std::ofstream mf{sidecar};
    if (!mf.is_open()) {
      return atx::core::Err(atx::core::ErrorCode::IoError,
                            "metabook: cannot write sidecar: " + sidecar);
    }
    mf << "periods=" << S << '\n';
    mf << "instruments=" << M << '\n';
    mf << "sleeves=" << result.sleeve_results.size() << '\n';
    for (atx::usize s = 0; s < S; ++s) {
      mf << "s=" << s << " turnover_net=" << result.report.turnover_net[s]
         << " turnover_gross=" << result.report.turnover_gross[s]
         << " crossing_benefit_bps=" << result.report.crossing_benefit_bps[s] << '\n';
    }
  }

  // S2-3: netting telemetry -- the crossing win, aggregated over the whole schedule. The
  // naive baseline (sleeves traded SEPARATELY, no crossing) IS turnover_gross
  // (netting.hpp:63); turnover_net is the fund's ACTUAL traded book. Assert the R3 triangle
  // holds in aggregate too (a wiring regression -- e.g. accidentally summing the wrong
  // field -- would be caught here, not just inside the frozen net_fund_book). Telemetry
  // ONLY: these sums never enter `digest` above (mirrors the combine breadth/capacity kvs
  // convention, stage_combine.cpp:735-736).
  atx::f64 turnover_net_total = 0.0;
  atx::f64 turnover_gross_total = 0.0;
  atx::f64 crossing_benefit_total = 0.0;
  for (atx::usize s = 0; s < S; ++s) {
    turnover_net_total += result.report.turnover_net[s];
    turnover_gross_total += result.report.turnover_gross[s];
    crossing_benefit_total += result.report.crossing_benefit_bps[s];
  }
  ATX_ASSERT(turnover_net_total <= turnover_gross_total + 1e-6); // R3, aggregate sanity net
  const atx::f64 crossed_fraction_total =
      (turnover_gross_total > 0.0)
          ? (turnover_gross_total - turnover_net_total) / turnover_gross_total
          : 0.0;

  // S2-4: Euler attribution-by-sleeve + Meucci effective-bets, surfaced as stage telemetry
  // (comma-joined in sleeve order; never folded into `digest`). `result.report.attribution`
  // is computed inside MetaBook::run (the FROZEN driver) with the R4 sum-identity guarantees
  // (Sigma return_contrib == R_fund; Sigma risk_contrib == sqrt(c^T Omega c); Sigma
  // crossing_credit == the total crossing benefit) -- summed again HERE so a wiring
  // regression that mis-copies/mis-orders the attribution vector is caught by the
  // *_contrib_sum kvs, independent of MetaBook's own internal guarantee.
  const auto join_csv = [](const std::vector<atx::f64> &v) {
    std::string s;
    for (atx::usize i = 0; i < v.size(); ++i) {
      if (i > 0) {
        s += ",";
      }
      s += std::to_string(v[i]);
    }
    return s;
  };
  atx::f64 return_contrib_sum = 0.0;
  atx::f64 risk_contrib_sum = 0.0;
  atx::f64 crossing_credit_sum = 0.0;
  for (const auto v : result.report.attribution.return_contrib) {
    return_contrib_sum += v;
  }
  for (const auto v : result.report.attribution.risk_contrib) {
    risk_contrib_sum += v;
  }
  for (const auto v : result.report.attribution.crossing_credit) {
    crossing_credit_sum += v;
  }

  StageResult sr;
  sr.digest = digest;
  sr.kvs = {
      {"periods", std::to_string(S)},
      {"instruments", std::to_string(M)},
      {"sleeves", std::to_string(result.sleeve_results.size())},
      {"books", to_hex16(digest)},
      // S2-3 netting telemetry.
      {"fund_turnover_net", std::to_string(turnover_net_total)},
      {"fund_turnover_gross", std::to_string(turnover_gross_total)},
      {"crossing_benefit_bps", std::to_string(crossing_benefit_total)},
      {"crossed_fraction", std::to_string(crossed_fraction_total)},
      // S2-4 report telemetry.
      {"sleeve_return_contrib", join_csv(result.report.attribution.return_contrib)},
      {"sleeve_risk_contrib", join_csv(result.report.attribution.risk_contrib)},
      {"sleeve_crossing_credit", join_csv(result.report.attribution.crossing_credit)},
      {"fund_effective_bets", std::to_string(result.report.effective_bets)},
      {"fund_sharpe", std::to_string(result.report.fund_metrics.sharpe)},
      {"return_contrib_sum", std::to_string(return_contrib_sum)},
      {"risk_contrib_sum", std::to_string(risk_contrib_sum)},
      {"crossing_credit_sum", std::to_string(crossing_credit_sum)},
  };
  return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
