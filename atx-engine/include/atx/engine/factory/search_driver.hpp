#pragma once

// atx::engine::factory — SearchDriver: the deterministic evolutionary population
// loop (S3-5, plan §4.7). This is the INTEGRATION unit — it wires S3-1…S3-4 + S2
// into one seeded, deflated, pool-aware search and is the home of the F1
// byte-identical-replay contract.
//
// ===========================================================================
//  THE LOOP (§4.7) — init -> {evaluate, select, reproduce, elitism, novelty}
// ===========================================================================
//   init_population : seed expressions parsed + analyze-validated into Genomes
//                     (all F5-valid in-grammar ASTs); padded to `population`.
//   each generation :
//     (a) fresh := [g in pop if !canon.contains(g.canon_hash)]  — dedup (F6).
//     (b) compile each fresh genome (its cached Analysis) into a single-root
//         Program; parallel::parallel_evaluate the batch over a DetPool and fold
//         signal_set_digest into the run digest (F2 — worker-count-invariant).
//     (c) score each fresh genome via pool_aware_fitness (S3-4); canon.insert.
//     (d) SELECT in CANONICAL-ID order: sort the population by (canon_hash, ast
//         size, root id) BEFORE any RNG draw, then tournament-select with a
//         Xoshiro256pp seeded by (master_seed, gen).
//     (e) REPRODUCE: for child i, rng := Xoshiro256pp(seed_for(master_seed, gen,
//         i)); bernoulli(p_cross) ? subtree_crossover(pick2) : mutate_one(pick1).
//         An Err child (F5 reject) is dropped. The seed is derived from
//         (master_seed, gen, i) ONLY — never worker/thread/time (F1).
//     (f) elitism: carry the top `elites` by RAW fitness (the maximized search
//         signal), so the single best-raw genome ALWAYS survives — and because its
//         raw fitness is cached by canon_hash, it re-scores to the identical value
//         next gen, making best_fitness_per_gen non-decreasing BY CONSTRUCTION.
//     (g) novelty pressure: novelty_penalize subtracts a deterministic
//         canonical-structure distance term so the population does not collapse.
//
// ===========================================================================
//  F1 — SEED BY ID. Every RNG draw comes from a Xoshiro256pp whose seed is a pure
//  function of (master_seed, generation, candidate_index) via seed_for (a fixed
//  SplitMix-style mix, no std::hash, no platform width dependence). Same cfg/pool
//  => byte-identical digest / trial_count / all_scored. The DetPool worker count
//  NEVER enters seed_for or any result — it only sizes the eval fan-out, and
//  parallel_evaluate is byte-identical across worker counts by construction.
//
//  F2 — CANONICAL-ID ORDER + WORKER INVARIANCE. Selection and reproduction iterate
//  the population in a value-based canonical order (sort_by_canon: by canon_hash,
//  then ast node count, then root ExprId — a total, insertion-order-independent
//  key) established BEFORE any RNG draw. The per-generation eval digest is folded
//  from parallel_evaluate's SignalSet, whose digest equals the single-thread path
//  and is invariant across {1,2,4,8} workers (S2's load-bearing contract).
//
// ===========================================================================
//  EVAL WIRING (the §0.8 switch, as-built). There is NO unparse(Ast) in the
//  codebase, so the spec's `compile_batch(unparse(g.ast))` is realized directly:
//  each genome already carries its (ast, analysis), so alpha::compile(ast,
//  analysis) yields its single-root Program with ZERO re-parse. The fresh batch is
//  one Program per candidate — EXACTLY parallel_evaluate's "one program per work
//  item" model (batch_eval.hpp) — so digest(parallel) == digest(single-thread) and
//  is worker-count-invariant. The run digest is this per-generation SignalSet
//  digest folded with the generation index (hash_combine). Scoring itself uses
//  pool_aware_fitness, whose internal single-thread eval is the correct fitness
//  oracle (the parallel path is the DIGEST/throughput path; F2 is about the digest).
//
// Header-only; every function inline. The loop is COLD relative to the VM hot path
// (one rebuild/compile/eval per distinct candidate), so std::vector / hash-map /
// per-generation Program allocation is acceptable (F8: no allocation on the VM hot
// loop; the cold compile path may allocate, documented).

#include <bit>       // std::popcount (canonical-structure novelty distance)
#include <span>      // std::span
#include <string>    // std::string (seed-expression source input)
#include <string_view> // std::string_view (field-swap candidate names)
#include <unordered_map> // std::unordered_map (per-run fitness cache, F6 throughput)
#include <vector>

#include "atx/core/error.hpp"  // Result, Ok, Err
#include "atx/core/random.hpp" // atx::core::Xoshiro256pp
#include "atx/core/types.hpp"  // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"   // alpha::parse_expr, alpha::Library

#include "atx/engine/combine/store.hpp" // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/parallel/det_pool.hpp" // parallel::DetPool (wired S2 handle)

#include "atx/engine/factory/canonical.hpp" // factory::canonical_hash, CanonSet
#include "atx/engine/factory/crossover.hpp" // factory::subtree_crossover
#include "atx/engine/factory/fitness.hpp"   // factory::pool_aware_fitness
#include "atx/engine/factory/genome.hpp"    // factory::Genome
#include "atx/engine/factory/mutation.hpp"  // factory::op_swap/field_swap/jitter_const
#include "atx/engine/factory/op_catalog.hpp" // factory::OpCatalog

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;

// =========================================================================
//  SearchConfig — the §4.7 knobs.
// =========================================================================
struct SearchConfig {
  atx::u64 master_seed{0};   // the F1 root entropy; the recorded artifact key
  atx::usize population{16}; // genomes per generation
  atx::usize generations{5}; // search depth (budget = generations * population)
  atx::usize elites{2};      // top-k carried verbatim each generation
  atx::usize k_tournament{3};// tournament size for parent selection
  atx::f64 p_cross{0.5};     // P(crossover) vs P(mutation) per child
  atx::f64 novelty_w{0.1};   // weight of the anti-collapse novelty penalty
  atx::u16 max_lookback{250};// crossover/jitter window cap (in-grammar rail)
  atx::usize n_workers{1};   // DetPool fan-out; affects digest SPEED, never bits
  FitnessCfg fitness{};      // CPCV geometry + trial-count base for deflation
  // op_swap is ENABLED (S3.4 fixed the root cause). The original defect — a swap
  // could rebuild an analyze-VALID genome that corrupted the VM SlotPool — is
  // closed by (a) analyze's validate_node_contract (materialized operand-arity +
  // hparam-count rail: analyze-valid ⟹ VM-safe for ALL mutation) and (b) the
  // OpCatalog buckets keying on materialized operand arity + group role and
  // skipping hparam/record ops, so a swap only ever samples a contract-compatible
  // op. The alpha_op_swap_stress harness proves no abort across every bucket.
  bool enable_op_swap{true};
};

// =========================================================================
//  SearchResult — the §4.7 return value (fields the verbatim tests read).
// =========================================================================
struct SearchResult {
  atx::u64 digest{0};              // F1/F2 byte-identical run fingerprint
  atx::usize trial_count{0};       // distinct candidates scored (canon.size())
  atx::usize candidates_generated{0}; // total genomes produced across the run
  atx::f64 dedup_pct{0.0};         // 1 - trial_count/candidates_generated (F6)
  std::vector<atx::f64> best_fitness_per_gen; // best RAW fitness per gen (the
                                              // maximized search signal; elites
                                              // carry it forward so this is
                                              // non-decreasing by construction)
  std::vector<Genome> all_scored;  // every distinct genome that was scored (F5)
  std::vector<Genome> admitted_candidates; // top survivors of the final gen
  atx::u64 seed{0};                // == cfg.master_seed (artifact key)
};

namespace detail {

// seed_for — the F1 per-(gen, idx) seed derivation. A fixed SplitMix-style mix of
// (master_seed, gen, idx); pure, portable (no std::hash / platform-width hash),
// and depends on NOTHING else (never worker/thread/time/address). The golden-ratio
// avalanche makes (gen, idx) streams well-separated.
[[nodiscard]] inline atx::u64 seed_for(atx::u64 master, atx::u64 gen, atx::u64 idx) noexcept {
  auto mix = [](atx::u64 x) noexcept -> atx::u64 {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
  };
  atx::u64 h = mix(master);
  h = mix(h ^ (gen + 0x9E3779B97F4A7C15ULL));
  h = mix(h ^ (idx + 0x632BE59BD9B4E019ULL));
  return h;
}

// Canonical-order comparator (F2): a TOTAL, value-based, insertion-independent
// order over genomes. Primary key canon_hash; ties broken by ast node count then
// root ExprId so two distinct genomes that (improbably) share a hash still order
// deterministically. NOT population/insertion/completion order.
[[nodiscard]] inline bool canon_less(const Genome &a, const Genome &b) noexcept {
  if (a.canon_hash != b.canon_hash) {
    return a.canon_hash < b.canon_hash;
  }
  const atx::usize an = a.ast.nodes().size();
  const atx::usize bn = b.ast.nodes().size();
  if (an != bn) {
    return an < bn;
  }
  return a.ast.roots().front().root < b.ast.roots().front().root;
}

// canonical_distance — a deterministic behavioral-distance proxy: the Hamming
// distance of two genomes' canonical hashes (popcount of the XOR), normalized to
// [0,1] over 64 bits. Two structurally-identical genomes => 0; genuinely-different
// structures => a positive, value-based, RNG-free distance. Used by
// novelty_penalize to keep the population diverse without any extra evaluation.
[[nodiscard]] inline atx::f64 canonical_distance(atx::u64 a, atx::u64 b) noexcept {
  const int bits = std::popcount(a ^ b);
  return static_cast<atx::f64>(bits) / 64.0;
}

} // namespace detail

// =========================================================================
//  Scored — a genome paired with its selection fitness (the search signal).
// =========================================================================
struct Scored {
  Genome genome;
  atx::f64 fitness{0.0}; // pool_aware_fitness raw (the maximized signal)
  atx::f64 selection{0.0}; // fitness MINUS the novelty penalty (used for ranking)
};

// =========================================================================
//  SearchDriver — the seeded population loop (§4.7).
//
//  Borrows the run-wide Library + Panel + WeightPolicy + ExecutionSimulator for
//  the duration of every run() call. The Library owns every OpSig the genomes'
//  Expr::op pointers borrow (genome.hpp SAFETY); it MUST outlive the driver.
// =========================================================================
class SearchDriver {
public:
  // SAFETY: `lib`, `panel`, `policy`, `sim` are BORROWED for the driver's lifetime
  // (and every run() call). The single run-wide Library owns the op rows every
  // genome's Expr::op aliases; it must outlive the driver and all produced genomes.
  // `seed_exprs` are the in-grammar starting templates; `panel_fields` are the
  // field-swap candidate names (the Panel exposes no field-name iterator, so the
  // caller supplies them — they MUST be the panel's actual field spellings).
  SearchDriver(const alpha::Library &lib, const alpha::Panel &panel,
               const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
               std::vector<std::string> seed_exprs, std::vector<std::string> panel_fields);

  // Run the deterministic search. Same cfg + same pool => byte-identical result
  // (F1). `pool` is borrowed read-only for the marginal-correlation fitness term.
  [[nodiscard]] SearchResult run(const SearchConfig &cfg, const combine::AlphaStore &pool);

private:
  // ----- (1) init_population -------------------------------------------------
  // Parse + analyze each seed expression into an F5-valid Genome, set its
  // canon_hash, then pad up to cfg.population by cycling the valid seeds (a
  // deterministic, in-grammar fill — every member is analyze-valid by
  // construction). At least one seed must parse; an all-invalid seed set yields an
  // empty population and an empty (but well-formed) result.
  [[nodiscard]] std::vector<Genome> init_population(const SearchConfig &cfg) const;

  // ----- (2) evaluate_generation --------------------------------------------
  // Collect the fresh (un-seen) genomes (F6), compile them to single-root Programs,
  // evaluate each (fresh Engine per program — see the EVAL-PATH NOTE below) and
  // fold its worker-invariant digest into the run digest (F2), then score each NEW
  // population member via pool_aware_fitness (dedup-hits reuse the cached score).
  // A fresh candidate that fails to compile is dropped from the digest (F5
  // backstop); every distinct structure is recorded in all_scored + the CanonSet.
  [[nodiscard]] std::vector<Scored>
  evaluate_generation(const std::vector<Genome> &pop, const SearchConfig &cfg, atx::usize gen,
                      const combine::AlphaStore &pool, CanonSet &canon,
                      std::unordered_map<atx::u64, atx::f64> &fitness_cache,
                      parallel::DetPool &det_pool, SearchResult &res);

  // ----- (3) novelty_penalize ------------------------------------------------
  // Subtract a deterministic behavioral-distance term from each genome's selection
  // fitness: the LESS novel a genome is (the smaller its mean canonical-structure
  // distance to the rest of the population), the LARGER the penalty — so the search
  // is pushed off a single collapsing motif. Distance is the normalized Hamming
  // distance of canonical hashes (RNG-free, value-based, order-independent), so
  // this is fully deterministic and F1-safe.
  void novelty_penalize(std::vector<Scored> &scored, const combine::AlphaStore &pool,
                        const SearchConfig &cfg) const;

  // ----- (4) reproduce -------------------------------------------------------
  // Produce the next population: carry the top `elites` by RAW fitness, then fill
  // the rest by id-seeded crossover/mutation over canonical-id-ordered parents
  // (F1/F2). Every child is funneled through the operators' analyze backstop, so an
  // Err child (F5) is simply dropped; if a child cannot be produced, an elite is
  // cloned in its place to hold the population size stable (still F5-valid).
  //
  // ELITISM IS STRUCTURAL: elites are ranked by RAW .fitness (the maximized search
  // signal), NOT the novelty-penalized .selection. Carrying the best-raw genome
  // verbatim — combined with the canon-keyed fitness_cache that re-scores it to the
  // SAME raw value next gen — makes best_fitness_per_gen non-decreasing BY
  // CONSTRUCTION. (Tournament parent selection below still uses .selection, the
  // explore/anti-collapse pressure — only the elite carry switches to raw.)
  [[nodiscard]] std::vector<Genome> reproduce(const std::vector<Scored> &scored,
                                              const SearchConfig &cfg, atx::usize gen,
                                              SearchResult &res);

  // Produce one child from the canonical-id-ordered parent pool with a single
  // id-seeded rng: bernoulli(p_cross) ? crossover(two tournament picks) :
  // mutate(one tournament pick). The operators' analyze backstop guarantees an
  // Ok child is F5-valid; an Err propagates (the caller substitutes an elite).
  [[nodiscard]] atx::core::Result<Genome> make_child(const std::vector<Scored> &scored,
                                                     const std::vector<atx::usize> &canon_order,
                                                     const SearchConfig &cfg, Xoshiro256pp &rng);

  // Pick one type-safe mutation by a seeded draw, fallback-cascading so a
  // degenerate genome (e.g. no literal to jitter) still yields a child when ANY
  // operator applies. Each operator self-validates (analyze backstop, F5).
  //
  // op_swap is gated behind cfg.enable_op_swap (default ON since S3.4 fixed the
  // root cause). The seeded draw uses a FIXED modulus (3) regardless of the gate
  // so the RNG stream — and therefore the replay (F1) — does not shift when the
  // gate flips; a drawn-but-disabled op_swap simply falls through to jitter_const.
  [[nodiscard]] atx::core::Result<Genome> mutate_one(const Genome &g, const SearchConfig &cfg,
                                                     Xoshiro256pp &rng);

  // ----- selection helpers ---------------------------------------------------

  // Indices of `scored` in canonical-id order (value-based, RNG-free). Established
  // before any draw so the seeded tournament is replayable (F2).
  [[nodiscard]] static std::vector<atx::usize>
  canon_ordered_indices(const std::vector<Scored> &scored);

  // Indices of `scored` ranked by DESCENDING selection fitness (penalized score);
  // ties broken by canonical order so the rank is deterministic (F1). NOTE: this is
  // the .selection ranking — it is no longer used for elitism (which switched to
  // raw_ordered_indices, below); retained for any selection-pressure consumer.
  [[nodiscard]] static std::vector<atx::usize>
  elite_ordered_indices(const std::vector<Scored> &scored);

  // Indices of `scored` ranked by DESCENDING RAW fitness (the maximized search
  // signal); ties broken by canonical order so the rank is deterministic (F1).
  // This is the elitism + admitted-candidate ordering: carrying the best-raw genome
  // verbatim each gen (and re-scoring it from the canon-keyed cache to the SAME raw
  // value) makes best_fitness_per_gen non-decreasing BY CONSTRUCTION.
  [[nodiscard]] static std::vector<atx::usize>
  raw_ordered_indices(const std::vector<Scored> &scored);

  // k-tournament over the canonical-id-ordered parent pool: draw k indices into
  // `canon_order` with the seeded rng, return the one with the highest selection
  // fitness (ties -> the earlier canonical-id slot, deterministic). The candidate
  // set is iterated in fixed canonical order, so the draw is fully replayable (F2).
  [[nodiscard]] static const Genome &tournament_pick(const std::vector<Scored> &scored,
                                                     const std::vector<atx::usize> &canon_order,
                                                     atx::usize k, Xoshiro256pp &rng);

  // ----- result assembly -----------------------------------------------------

  // Best RAW fitness in the scored set (the maximized search signal, NOT the
  // novelty-penalized .selection). This is what best_fitness_per_gen tracks; the
  // structural elite carry guarantees it is non-decreasing across generations.
  [[nodiscard]] static atx::f64 best_raw(const std::vector<Scored> &scored);

  // dedup_pct + admitted candidates (top survivors of the final generation by
  // RAW fitness — same ordering as the structural elite carry, so the run's
  // reported best matches what was preserved across generations). trial_count ==
  // the distinct structures scored (CanonSet).
  void finalize(const std::vector<Scored> &scored, const CanonSet &canon, SearchResult &res) const;

  // SAFETY: each member borrows a const OpSig* from `lib_`; `lib_`/`panel_` etc.
  // are borrowed for the driver's lifetime and must outlive every produced genome.
  const alpha::Library &lib_;
  const alpha::Panel &panel_;
  const WeightPolicy &policy_;
  const exec::ExecutionSimulator &sim_;
  OpCatalog catalog_;
  std::vector<std::string> seed_exprs_;
  // Field-swap candidate names (owned) + their string_views. SAFETY: the views
  // alias panel_fields_'s owned strings, valid for the driver's lifetime.
  std::vector<std::string> panel_fields_;
  std::vector<std::string_view> panel_field_views_;
};

} // namespace atx::engine::factory
