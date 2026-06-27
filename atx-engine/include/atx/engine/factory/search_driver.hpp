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
//     (g) novelty pressure: the behavioral-novelty objective (S4.2) drives
//         phenotypic diversity when enable_behavioral_novelty is active.
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
  atx::u16 max_lookback{250}; // crossover/jitter window cap (in-grammar rail)
  atx::usize n_workers{1};    // DetPool fan-out; affects digest SPEED, never bits
  // S4.2 behavioral / phenotypic diversity. The behavioral novelty objective
  // (objectives[3]) is ACTIVE iff objective_mode == MultiObjective AND
  // enable_behavioral_novelty. This is the ONLY novelty mechanism: the old
  // structural canonical-hash penalty (a Hamming distance over hashes) was unsound
  // and is removed. `behavior_metric` selects the profile distance (PnlCorr default;
  // RankIc fork). `behavior_archive_cap` is the FIFO capacity C of the past-elite
  // descriptor archive; `behavior_k` is the k-nearest count for the novelty mean.
  bool enable_behavioral_novelty{true};
  BehaviorMetric behavior_metric{BehaviorMetric::PnlCorr};
  atx::usize behavior_archive_cap{64}; // C: past-elite behavioral descriptor ring
  atx::usize behavior_k{3};            // k-nearest neighbours for the novelty mean
  // S4.1 selection mode. Defaults to MultiObjective (the new behavior); the
  // boundary-pin / replay tests set ScalarRaw to reproduce the frozen pre-S4 path.
  ObjectiveMode objective_mode{ObjectiveMode::MultiObjective};
  // S3.5 generation wire (plan §0.5/§0.6). When ON (the new default), init_population
  // fills any population slot the seed expressions do NOT cover via ramped grammar
  // sampling (varied depth across [init_min_depth, gen_cfg.max_depth]) with
  // canon-hash dedup, so gen-0 starts diverse. The boundary pin / ScalarRaw golden
  // test sets this to false to reproduce the pre-Task-3 cycle-fill EXACTLY (the
  // digest stays byte-identical). `gen_cfg` is the sampler config.
  bool seed_from_grammar{true};
  // Ramped init: the grammar fill samples tree depth across [init_min_depth,
  // gen_cfg.max_depth] per slot (a ramped-half-and-half analogue) and resamples on
  // a canon-hash collision (bounded retries) so gen-0 is maximally distinct.
  atx::usize init_min_depth{2};
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
  // Parsimony pressure: when ON (MultiObjective only), objectives[kObjParsimony] =
  // -node_count makes a smaller, equally-fit tree Pareto-dominate a larger one.
  // ScalarRaw ignores objectives, so this never perturbs the boundary pin.
  // Default OFF: with ~5 objectives most genomes land in Pareto front 0 and the
  // smallest-tree genome gets +inf crowding distance, becoming a tournament magnet
  // that floods the population with degenerate trivial alphas. Keep configurable.
  bool enable_parsimony{false};
  // Diversity insurance: replace the worst min(n_immigrants, n_children) non-elite
  // child slots each generation with fresh grammar genomes (seed_for-seeded -> F1).
  // 0 disables (legacy).
  atx::usize n_immigrants{2};
  // Stagnation early-stop: stop when BOTH best_raw AND mean_raw fitness have
  // not improved by more than epsilon over the last `patience` generations.
  // Requiring BOTH prevents premature termination on healthy elitist populations
  // where best_raw is non-decreasing by construction (elitism carries the best
  // genome verbatim). 0 disables (run the full budget; legacy behavior).
  atx::usize stagnation_patience{8};
  // Adaptive operator selection: bias each generation's mutation-operator
  // distribution toward operators that produced fitness gains last generation.
  // OFF reproduces the fixed-uniform (% 3) draw bit-for-bit (boundary pin).
  bool adaptive_operators{true};
  // Jitter annealing: scale jitter_const's sigma by jitter_anneal_decay^gen
  // (coarse early, fine late). OFF keeps the constant JitterCfg.sigma.
  // 0.97 halves sigma only by ~gen 23 (vs 0.9 which halved by gen 7), keeping
  // local exploration alive when escape from a plateau is most needed.
  bool jitter_anneal{true};
  atx::f64 jitter_anneal_decay{0.97};
  // R4: opt-in deflated-Sharpe selection pressure. When ON, evaluate_generation
  // captures canon.size() BEFORE the parallel_for as the per-generation deflation
  // N (worker-order-independent), feeds it into a per-generation FitnessCfg so
  // pool_aware_fitness computes dsr with that N, then at the score_slot seam:
  //   (1) objectives[kObjDeflation] = rep->dsr    (new NSGA objective, maximized)
  //   (2) score_slot[j].raw *= rep->dsr            (raw haircut for elitism/ScalarRaw)
  // OFF (the default): gen_fit == cfg.fitness exactly — zero new computation, the
  // F1 search digest, admission digest, ScalarRaw boundary pin, and MultiObjective
  // default digest are ALL byte-identical to the pre-R4 path.
  // CACHING NOTE: a genome first scored in generation g caches its haircut raw +
  // objectives[kObjDeflation] computed with N = canon.size() at gen g; a later
  // reuse keeps that first-evaluation deflation. Fully deterministic (cache order
  // is serial) — the accepted semantic.
  bool deflate_selection{false};  // R4: deflated-Sharpe enters search selection
  // W1b: the wrap_in_op mutation — wrap a subtree in a conditioning op (zscore/
  // signedpower/rank/winsorize/group_neutralize) so the GA can CREATE in-expression
  // conditioning structure (the manual-alpha lift signedpower(zscore(raw), p)).
  // DEFAULT FALSE: when off, mutate_one's operator draw, modulus, op_weights, and
  // the entire RNG stream are BYTE-IDENTICAL to the pre-W1b path — zero new RNG
  // draws (the wrap selection is a post-draw bernoulli guarded entirely behind this
  // flag). The kGoldenDigest boundary pin proves the disabled path is unchanged.
  bool enable_wrap_in_op{false};
  // Probability that a drawn mutation is REPLACED by a wrap_in_op attempt, sampled
  // AFTER the legacy operator draw and ONLY when enable_wrap_in_op is true (so the
  // disabled path draws nothing extra). A failed wrap (Err) falls through to the
  // originally-drawn operator, so the population never stalls.
  atx::f64 wrap_in_op_prob{0.25};

  // S3-2: opt-in seed-elite protection.
  //
  // protect_seed_elites (NEW, default false): when true, the top-ranked from_seed
  // genome is force-inserted into the next generation's population for every
  // generation where `gen < protect_until_gen`.  The insertion is POST-selection
  // (after the canonical sort that establishes F2 canonical order) so it never
  // perturbs the F2 determinism proof.  No RNG change; byte-identical when false.
  //
  // protect_until_gen (NEW, default 3): exclusive upper bound: protection active
  // while `gen < protect_until_gen` (gens 0..protect_until_gen-1).  With the
  // default 3, seed elites are force-inserted during gens 0, 1, 2 (NOT gen 3).
  // Only read when protect_seed_elites is true.
  //
  // mutate_seed_copies (NEW, default false): when true AND seed_from_grammar=false,
  // each cycled clone slot in init_population receives ONE seeded mutation via
  // detail::seed_for(master_seed, kMutateSeedAxis, i) instead of being an
  // identical copy.  Slot 0 always gets the seed original (unmutated).  The new
  // RNG axis (kMutateSeedAxis) is distinct from all existing axes so the default
  // path's draw sequence is unchanged — byte-identical when false.
  bool protect_seed_elites{false};
  atx::usize protect_until_gen{3};
  bool mutate_seed_copies{false};
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

} // namespace detail

// =========================================================================
//  Scored — a genome paired with its selection fitness (the search signal).
// =========================================================================
struct Scored {
  Genome genome;
  atx::f64 fitness{0.0};   // pool_aware_fitness raw (the maximized signal)
  atx::f64 selection{0.0}; // equals fitness (ScalarRaw tournament ranking; no structural penalty)
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
  //
  // W4a: `weak_panel` (OPTIONAL borrow; default nullptr) is the §0.8 alternate-
  // universe Panel for the robustness re-eval — when non-null, each candidate's
  // fitness multiplies in robust = clamp(wq_on(weak_panel)/wq, 0, 1); when nullptr
  // (the default) robust stays the constant 1.0 and raw == wq*diversify EXACTLY as
  // before (byte-identical — the kGoldenDigest boundary pin proves it). The pointee
  // (if any) MUST outlive the driver and every run() call (owned by the caller).
  //
  // R1 field-type discipline: `numeric_excluded_fields` / `extra_group_fields` are
  // OPTIONAL lists (both defaulted empty = byte-identical to the pre-R1 path).
  // When non-empty, the ctor's partition loop tightens:
  //   - A field in numeric_excluded_fields is excluded from numeric_field_views_
  //     even if !is_group_field(f) (e.g. binary earnFlag, low-cardinality counts).
  //   - A field in extra_group_fields is added to group_field_views_ iff it was not
  //     already classified there by is_group_field(f) (e.g. a raw gics integer col).
  //   - panel_field_views_ (the full list for eval-field validity) is UNCHANGED.
  // DETERMINISM: empty lists -> identical partition -> byte-identical digest (F1).
  SearchDriver(const alpha::Library &lib, const alpha::Panel &panel, const WeightPolicy &policy,
               const exec::ExecutionSimulator &sim, std::vector<std::string> seed_exprs,
               std::vector<std::string> panel_fields,
               const alpha::Panel *weak_panel = nullptr,
               std::vector<std::string> numeric_excluded_fields = {},
               std::vector<std::string> extra_group_fields = {});

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

  // ----- (3b) behavioral_novelty_pass (S4.2) ---------------------------------
  // Compute the population-relative BEHAVIORAL novelty for every Scored and write
  // it into objectives[3], bumping n_objectives to 4 — but ONLY when the objective
  // is ACTIVE (objective_mode == MultiObjective && enable_behavioral_novelty). Runs AFTER all
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
  // activation gate, referenced by every S4.2 seam). MultiObjective && enable_behavioral_novelty.
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
  // Task 5: `op_weights` are the FIXED per-generation operator weights (computed
  // serially by the caller before this call, so every child draws its operator
  // against the same weights — F1-safe). `child_ops` is filled (sized n_children)
  // with each child slot's chosen mutation-operator id (0=op_swap, 1=field_swap,
  // 2=jitter; 0xFF for a non-mutation child — crossover, immigrant, or elite-clone
  // fallback) so the caller can credit operators by realized fitness gain next gen.
  // Each slot is written by the single shard that produced it (no race). Both are
  // inert (uniform weights, ignored ids) when cfg.adaptive_operators is false.
  [[nodiscard]] std::vector<Genome> reproduce(const std::vector<Scored> &scored,
                                              const SearchConfig &cfg, atx::usize gen,
                                              parallel::DetPool &det_pool, SearchResult &res,
                                              const std::array<atx::f64, 3> &op_weights,
                                              std::vector<atx::u8> &child_ops);

  // Produce one child from the canonical-id-ordered parent pool with a single
  // id-seeded rng: bernoulli(p_cross) ? crossover(two tournament picks) :
  // mutate(one tournament pick). The operators' analyze backstop guarantees an
  // Ok child is F5-valid; an Err propagates (the caller substitutes an elite).
  [[nodiscard]] atx::core::Result<Genome> make_child(const std::vector<Scored> &scored,
                                                     const std::vector<atx::usize> &canon_order,
                                                     const SearchConfig &cfg, Xoshiro256pp &rng,
                                                     const std::array<atx::f64, 3> &op_w,
                                                     atx::usize gen, atx::u8 &op_used);

  // Pick one type-safe mutation by a seeded draw, fallback-cascading so a
  // degenerate genome (e.g. no literal to jitter) still yields a child when ANY
  // operator applies. Each operator self-validates (analyze backstop, F5).
  //
  // op_swap is gated behind cfg.enable_op_swap (default ON since S3.4 fixed the
  // root cause). The seeded draw uses a FIXED modulus (3) regardless of the gate
  // so the RNG stream — and therefore the replay (F1) — does not shift when the
  // gate flips; a drawn-but-disabled op_swap simply falls through to jitter_const.
  //
  // Task 5: under cfg.adaptive_operators the operator is drawn weighted by `op_w`
  // (still ONE rng word, flag-branched so OFF keeps the literal % 3 stream); under
  // cfg.jitter_anneal the jitter sigma is scaled by jitter_anneal_decay^gen. The
  // selected operator's id (0=op_swap,1=field_swap,2=jitter) is reported via
  // `op_used` for the per-generation credit accumulator (caller ignores it when
  // adaptation is off).
  [[nodiscard]] atx::core::Result<Genome> mutate_one(const Genome &g, const SearchConfig &cfg,
                                                     Xoshiro256pp &rng,
                                                     const std::array<atx::f64, 3> &op_w,
                                                     atx::usize gen, atx::u8 &op_used);

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
  // W4a: the OPTIONAL §0.8 weak/holdout sub-universe Panel for the robustness re-eval
  // (nullptr -> robust factor inert at 1.0, byte-identical). Borrowed; the pointee
  // (if any) is owned by the caller and must outlive the driver.
  const alpha::Panel *weak_panel_;
  const WeightPolicy &policy_;
  const exec::ExecutionSimulator &sim_;
  OpCatalog catalog_;
  std::vector<std::string> seed_exprs_;
  // Field-swap candidate names (owned) + their string_views. SAFETY: the views
  // alias panel_fields_'s owned strings, valid for the driver's lifetime.
  std::vector<std::string> panel_fields_;
  std::vector<std::string_view> panel_field_views_;
  // Task 3.1: dtype-partitioned views into panel_fields_. Built in the ctor.
  // numeric_field_views_: fields where !is_group_field (F64 leaves).
  // group_field_views_: fields where is_group_field (Group classifiers).
  std::vector<std::string_view> numeric_field_views_;
  std::vector<std::string_view> group_field_views_;
};

} // namespace atx::engine::factory
