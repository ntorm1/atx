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

#include <array>         // std::array (per-genome multi-objective vector, S4.1)
#include <bit>           // std::popcount (canonical-structure novelty distance)
#include <memory>        // std::unique_ptr (per-worker Engine vector, Tier 4)
#include <span>          // std::span
#include <string>        // std::string (seed-expression source input)
#include <string_view>   // std::string_view (field-swap candidate names)
#include <unordered_map> // std::unordered_map (per-run fitness cache, F6 throughput)
#include <vector>

#include "atx/core/error.hpp"  // Result, Ok, Err
#include "atx/core/random.hpp" // atx::core::Xoshiro256pp
#include "atx/core/types.hpp"  // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/panel.hpp"  // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp" // alpha::parse_expr, alpha::Library

#include "atx/engine/combine/store.hpp"      // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/parallel/det_pool.hpp" // parallel::DetPool (wired S2 handle)

#include "atx/engine/factory/behavior.hpp" // factory::BehavioralArchive, behavioral_distance (S4.2)
#include "atx/engine/factory/canonical.hpp"  // factory::canonical_hash, CanonSet
#include "atx/engine/factory/crossover.hpp"  // factory::subtree_crossover
#include "atx/engine/factory/fitness.hpp"    // factory::pool_aware_fitness, kMaxObjectives
#include "atx/engine/factory/generate.hpp"   // factory::generate_genome, GenConfig (S3.5 wire)
#include "atx/engine/factory/genome.hpp"     // factory::Genome
#include "atx/engine/factory/mutation.hpp"   // factory::op_swap/field_swap/jitter_const
#include "atx/engine/factory/op_catalog.hpp" // factory::OpCatalog
#include "atx/engine/factory/pareto.hpp"     // factory::ObjMatrix, NSGA-II primitives (S4.1)
#include "atx/engine/factory/search_state.hpp"    // factory::CachedScore (fitness-cache value)
#include "atx/engine/factory/search_progress.hpp" // factory::SearchProgressSink, SearchResumeState (resumable-discover)

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;

// =========================================================================
//  ObjectiveMode — how the search ranks a generation (S4.1).
//
//  ScalarRaw     : the pre-S4 behavior — collapse every candidate to the scalar
//                  `raw = wq * diversify * robust` and maximize it (total order by
//                  raw fitness + canonical tie-break). This is the boundary-pin
//                  path; with novelty off + a fixed seed it reproduces the frozen
//                  pre-S4 digest BYTE-IDENTICALLY.
//  MultiObjective: NSGA-II over the FitnessReport.objectives vector (front rank
//                  asc, crowding desc, canonical tie-break) — a high-wq/low-
//                  diversify and a low-wq/high-diversify candidate can BOTH survive.
// =========================================================================
enum class ObjectiveMode : atx::u8 { ScalarRaw, MultiObjective };

// =========================================================================
//  SearchConfig — the §4.7 knobs.
// =========================================================================
struct SearchConfig {
  atx::u64 master_seed{0};    // the F1 root entropy; the recorded artifact key
  atx::usize population{16};  // genomes per generation
  atx::usize generations{5};  // search depth (budget = generations * population)
  atx::usize elites{2};       // top-k carried verbatim each generation
  atx::usize k_tournament{3}; // tournament size for parent selection
  atx::f64 p_cross{0.5};      // P(crossover) vs P(mutation) per child
  atx::f64 novelty_w{0.1};    // weight of the anti-collapse novelty penalty
  atx::u16 max_lookback{250}; // crossover/jitter window cap (in-grammar rail)
  atx::usize n_workers{1};    // DetPool fan-out; affects digest SPEED, never bits
  // S4.2 behavioral / phenotypic diversity knobs. The behavioral novelty objective
  // (objectives[3]) is ACTIVE only when objective_mode == MultiObjective AND
  // novelty_w > 0 (the same lever that gates the ScalarRaw canonical-hash penalty;
  // the pinned ScalarRaw path sets novelty_w == 0, so neither fires and the
  // boundary pin holds). `behavior_metric` selects the profile distance (PnlCorr
  // default; RankIc fork). `behavior_archive_cap` is the FIFO capacity C of the
  // past-elite descriptor archive; `behavior_k` is the k-nearest count for the
  // mean-distance novelty. Defaults are a documented, deterministic starting point.
  BehaviorMetric behavior_metric{BehaviorMetric::PnlCorr};
  atx::usize behavior_archive_cap{64}; // C: past-elite behavioral descriptor ring
  atx::usize behavior_k{3};            // k-nearest neighbours for the novelty mean
  // S4.1 selection mode. Defaults to MultiObjective (the new behavior); the
  // boundary-pin / replay tests set ScalarRaw to reproduce the frozen pre-S4 path.
  ObjectiveMode objective_mode{ObjectiveMode::MultiObjective};
  // S3.5 generation wire (plan §0.5/§0.6). When ON, init_population fills any
  // population slot the seed expressions do NOT cover via generate_genome (the
  // type-correct grammar sampler), and the immigration path reuses it. GATED OFF
  // for the pinned ScalarRaw path so gen-0 reproduces the pre-S4 seed-cycle fill
  // EXACTLY (the digest stays byte-identical). `gen_cfg` is the sampler config.
  bool seed_from_grammar{false};
  GenConfig gen_cfg{};  // grammar-sampler knobs (max_lookback/depth, fields)
  FitnessCfg fitness{}; // CPCV geometry + trial-count base for deflation
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
  atx::u64 digest{0};                         // F1/F2 byte-identical run fingerprint
  atx::usize trial_count{0};                  // distinct candidates scored (canon.size())
  atx::usize candidates_generated{0};         // total genomes produced across the run
  atx::f64 dedup_pct{0.0};                    // 1 - trial_count/candidates_generated (F6)
  std::vector<atx::f64> best_fitness_per_gen; // best RAW fitness per gen (the
                                              // maximized search signal; elites
                                              // carry it forward so this is
                                              // non-decreasing by construction)
  std::vector<Genome> all_scored;             // every distinct genome that was scored (F5)
  std::vector<Genome> admitted_candidates;    // top survivors of the final gen
  atx::u64 seed{0};                           // == cfg.master_seed (artifact key)
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
  atx::f64 fitness{0.0};   // pool_aware_fitness raw (the maximized signal)
  atx::f64 selection{0.0}; // fitness MINUS the novelty penalty (used for ranking)
  // S4.1 multi-objective fields. `objectives` is the cached FitnessReport vector
  // {wq, diversify, robust, ...}; `rank`/`crowding` are filled ONCE per generation
  // by assign_pareto_ranks (NSGA-II over `objectives`) BEFORE any reproduction RNG
  // draw. In ScalarRaw mode rank/crowding collapse to a total order by `fitness`.
  std::array<atx::f64, kMaxObjectives> objectives{};
  atx::u8 n_objectives{0}; // live leading entries of `objectives`
  atx::u16 rank{0};        // NSGA-II non-dominated front (0 == best)
  atx::f64 crowding{0.0};  // crowding distance within the rank (larger == better)
  // S4.2 behavioral descriptor (the candidate's realized OOS PnL profile / phenotype,
  // copied from the cached FitnessReport). Read by the per-generation behavioral-
  // novelty pass to compute objectives[3] = mean k-nearest behavioral distance over
  // population ∪ archive. Owned by value (canon-cacheable; novelty itself is not).
  std::vector<atx::f64> descriptor{};
};

// CachedScore — the per-canon_hash fitness cache value (F6 throughput, S4.1) — is
// defined in search_state.hpp (extracted so search_progress.hpp can serialize the
// fitness_cache without a circular include). Re-included here below.

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
  SearchDriver(const alpha::Library &lib, const alpha::Panel &panel, const WeightPolicy &policy,
               const exec::ExecutionSimulator &sim, std::vector<std::string> seed_exprs,
               std::vector<std::string> panel_fields);

  // Run the deterministic search. Same cfg + same pool => byte-identical result
  // (F1). `pool` is borrowed read-only for the marginal-correlation fitness term.
  //
  // `sink` (optional): invoked once per completed generation with a
  // GenerationSnapshot so a caller may checkpoint progress; an Err return aborts
  // the run cleanly (well-formed partial result). `resume` (optional): when
  // well-formed, starts the loop at resume->start_generation from
  // resume->population instead of init_population. With BOTH nullptr (the default)
  // run() is the byte-identical legacy path — the only added work is two
  // null-pointer checks (F1/F2 off-path invariant, tested by OffPathByteIdentical).
  [[nodiscard]] SearchResult run(const SearchConfig &cfg, const combine::AlphaStore &pool,
                                 SearchProgressSink *sink = nullptr,
                                 const SearchResumeState *resume = nullptr);

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
  // evaluate each on a per-worker `engines[wid]` (reused across BOTH the digest and
  // fitness work of one merged pass) and fold its worker-invariant digest into the
  // run digest (F2), then score each NEW population member via pool_aware_fitness
  // (dedup-hits reuse the cached score). A fresh candidate that fails to compile is
  // dropped from the digest (F5 backstop); every distinct structure is recorded in
  // all_scored + the CanonSet. `engines` is owned by run() and reused across every
  // generation (Tier 4): Engine::evaluate is idempotent (output depends only on
  // (program, panel_)) and panel_ is constant for the whole run, so reusing the
  // engines across generations is byte-identical — the SlotPool grows ONCE to peak.
  [[nodiscard]] std::vector<Scored>
  evaluate_generation(const std::vector<Genome> &pop, const SearchConfig &cfg, atx::usize gen,
                      const combine::AlphaStore &pool, CanonSet &canon,
                      std::unordered_map<atx::u64, CachedScore> &fitness_cache,
                      parallel::DetPool &det_pool,
                      std::vector<std::unique_ptr<alpha::Engine>> &engines, SearchResult &res);

  // ----- (3) novelty_penalize ------------------------------------------------
  // Subtract a deterministic behavioral-distance term from each genome's selection
  // fitness: the LESS novel a genome is (the smaller its mean canonical-structure
  // distance to the rest of the population), the LARGER the penalty — so the search
  // is pushed off a single collapsing motif. Distance is the normalized Hamming
  // distance of canonical hashes (RNG-free, value-based, order-independent), so
  // this is fully deterministic and F1-safe.
  void novelty_penalize(std::vector<Scored> &scored, const combine::AlphaStore &pool,
                        const SearchConfig &cfg) const;

  // ----- (3b) behavioral_novelty_pass (S4.2) ---------------------------------
  // Compute the population-relative BEHAVIORAL novelty for every Scored and write
  // it into objectives[3], bumping n_objectives to 4 — but ONLY when the objective
  // is ACTIVE (objective_mode == MultiObjective && novelty_w > 0). Runs AFTER all
  // descriptors for the generation are known and BEFORE assign_pareto_ranks (so the
  // 4th objective enters NSGA-II this generation). For genome i the novelty is the
  // mean behavioral_distance to the k nearest descriptors in (population minus self)
  // ∪ archive — phenotypic diversity, DISTINCT from the marginal-corr `diversify`
  // objective (which prices distance to the admitted POOL). `scratch` is a reused
  // span buffer sized once per generation (zero hot-path alloc). RNG-free,
  // deterministic. When inactive this is a no-op and n_objectives stays 3 (the
  // boundary pin / ScalarRaw path is byte-untouched).
  void behavioral_novelty_pass(std::vector<Scored> &scored, const BehavioralArchive &archive,
                               const SearchConfig &cfg,
                               std::vector<std::span<const atx::f64>> &scratch) const;

  // Update the behavioral archive with the current generation's Pareto FRONT (the
  // rank-0 non-dominated elites) in CANONICAL-ID order, evicting oldest past C. No
  // RNG. Requires assign_pareto_ranks to have run (reads Scored.rank). Inactive
  // (objective off) -> no-op so the archive stays empty and adds no state. The
  // canonical-id insert order makes the archive contents — and the FIFO eviction
  // sequence — byte-deterministic and worker-count-invariant.
  static void update_archive(const std::vector<Scored> &scored,
                             const std::vector<atx::usize> &canon_order, const SearchConfig &cfg,
                             BehavioralArchive &archive);

  // True iff the S4.2 behavioral objective is active for this config (the single
  // activation gate, referenced by every S4.2 seam). MultiObjective && novelty_w>0.
  [[nodiscard]] static bool behavioral_active(const SearchConfig &cfg) noexcept;

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
                                              parallel::DetPool &det_pool, SearchResult &res);

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
  // This is the ScalarRaw elitism + admitted-candidate ordering: carrying the
  // best-raw genome verbatim each gen (and re-scoring it from the canon-keyed cache
  // to the SAME raw value) makes best_fitness_per_gen non-decreasing BY CONSTRUCTION.
  [[nodiscard]] static std::vector<atx::usize>
  raw_ordered_indices(const std::vector<Scored> &scored);

  // S4.1: assign NSGA-II rank + crowding to every Scored, ONCE per generation,
  // BEFORE any reproduction RNG draw. MultiObjective: builds an ObjMatrix over the
  // first n_objectives columns, runs fast_nondominated_sort (canon_order) -> rank,
  // and crowding_distance per front -> crowding. ScalarRaw: collapses to a total
  // order by RAW fitness (rank == descending-raw position, crowding 0), so the
  // pre-S4 raw ordering is reproduced exactly. `canon_order` is the value-based
  // permutation established before this call (no RNG has been drawn yet).
  static void assign_pareto_ranks(std::vector<Scored> &scored,
                                  const std::vector<atx::usize> &canon_order,
                                  const SearchConfig &cfg);

  // S4.1: indices of `scored` in NSGA-II survivor order — (rank asc, crowding desc,
  // canon_less). Elitism keeps the first front then crowding up to `elites`; the
  // admitted-candidate list reads the same order. In ScalarRaw this reduces to the
  // exact pre-S4 raw_ordered_indices (rank is the raw-descending position, crowding
  // 0, canonical tie-break). Requires assign_pareto_ranks to have run.
  [[nodiscard]] static std::vector<atx::usize>
  pareto_ordered_indices(const std::vector<Scored> &scored);

  // S4.1: the NSGA-II crowded-comparison operator <_n (Deb §III-C). `a` is BETTER
  // than `b` iff (rank_a < rank_b) OR (rank_a == rank_b AND crowding_a > crowding_b).
  // Used by both pareto_ordered_indices and tournament_pick (MultiObjective). NO
  // canonical tie-break here — the callers apply canon_less as the final key so the
  // resolution is a strict-weak-ordering. RNG-free, value-based, noexcept.
  [[nodiscard]] static bool crowded_better(const Scored &a, const Scored &b) noexcept;

  // k-tournament over the canonical-id-ordered parent pool: draw k indices into
  // `canon_order` with the seeded rng, return the BETTER candidate. MultiObjective
  // compares (rank asc, crowding desc); ScalarRaw compares .selection > (the exact
  // pre-S4 rule). Ties -> the earlier canonical-id slot (deterministic). The
  // candidate set is iterated in fixed canonical order, so the draw is replayable (F2).
  [[nodiscard]] static const Genome &tournament_pick(const std::vector<Scored> &scored,
                                                     const std::vector<atx::usize> &canon_order,
                                                     const SearchConfig &cfg, Xoshiro256pp &rng);

  // ----- result assembly -----------------------------------------------------

  // Best RAW fitness in the scored set (the maximized search signal, NOT the
  // novelty-penalized .selection). This is what best_fitness_per_gen tracks; the
  // structural elite carry guarantees it is non-decreasing across generations.
  [[nodiscard]] static atx::f64 best_raw(const std::vector<Scored> &scored);

  // Mean RAW fitness over the scored set (telemetry for the progress sink; NOT part
  // of the digest/admission). NaN/inf-safe: skips non-finite; empty -> 0.
  [[nodiscard]] static atx::f64 mean_raw(const std::vector<Scored> &scored);

  // dedup_pct + admitted candidates (top survivors of the final generation by
  // RAW fitness — same ordering as the structural elite carry, so the run's
  // reported best matches what was preserved across generations). trial_count ==
  // the distinct structures scored (CanonSet).
  void finalize(const std::vector<Scored> &scored, const CanonSet &canon, SearchResult &res) const;

  // ----- population checkpoint helpers (resumable-discover, Task 1) ----------

  // Render each genome to its canonical DSL string, emitted in canonical-id
  // order (sorted by detail::canon_less) so the blob is deterministic and
  // insertion-order-independent (F1/F2 contracts).
  [[nodiscard]] std::vector<std::string>
  serialize_population(const std::vector<Genome> &pop) const;

  // Parse + analyze each DSL string back into an F5-valid Genome with its
  // canon_hash set. Propagates the first parse/analyze error (ATX_TRY).
  [[nodiscard]] atx::core::Result<std::vector<Genome>>
  deserialize_population(const std::vector<std::string> &exprs) const;

  // Test-access friend for the population checkpoint round-trip test.
  // Unqualified: introduces SearchProgressTestAccess into atx::engine::factory.
  friend struct SearchProgressTestAccess;

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
