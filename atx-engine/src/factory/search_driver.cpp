#include "atx/engine/factory/search_driver.hpp"

#include <algorithm>     // std::clamp, std::sort, std::max, std::min
#include <cmath>         // std::isfinite (mean_raw telemetry, NaN/inf-safe)
#include <cstddef>       // std::size_t (hash_combine seed type)
#include <cstdint>       // std::uint8_t (compiled[] flag vector)
#include <memory>        // std::unique_ptr, std::make_unique
#include <optional>      // std::optional (per-child single-writer reproduce slots, Tier 5)
#include <span>          // std::span
#include <unordered_map> // std::unordered_map (score_j_of_ptr: Genome* -> j)
#include <unordered_set> // std::unordered_set (seen_this_gen dedup)
#include <utility>       // std::move
#include <vector>

#include "atx/core/hash.hpp" // atx::core::hash_combine

#include "atx/engine/alpha/bytecode.hpp"  // alpha::compile, alpha::Program
#include "atx/engine/alpha/typecheck.hpp" // alpha::analyze
#include "atx/engine/alpha/unparse.hpp"   // alpha::unparse (population serialization)
#include "atx/engine/alpha/vm.hpp"        // alpha::Engine (digest eval, fresh per program)

#include "atx/engine/parallel/digest.hpp"    // parallel::signal_set_digest
#include "atx/engine/parallel/scheduler.hpp" // parallel::Scheduler, ShardId (Tier 1 LPT dispatch)

namespace atx::engine::factory {

SearchDriver::SearchDriver(const alpha::Library &lib, const alpha::Panel &panel,
                           const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                           std::vector<std::string> seed_exprs,
                           std::vector<std::string> panel_fields)
    : lib_{lib}, panel_{panel}, policy_{policy}, sim_{sim}, catalog_{lib},
      seed_exprs_{std::move(seed_exprs)}, panel_fields_{std::move(panel_fields)} {
  panel_field_views_.reserve(panel_fields_.size());
  for (const std::string &f : panel_fields_) {
    panel_field_views_.push_back(f);
  }
}

[[nodiscard]] SearchResult SearchDriver::run(const SearchConfig &cfg,
                                             const combine::AlphaStore &pool,
                                             SearchProgressSink *sink,
                                             const SearchResumeState *resume) {
  SearchResult res;
  res.seed = cfg.master_seed;
  res.best_fitness_per_gen.reserve(cfg.generations);

  CanonSet canon; // F6 dedup: distinct structures scored so far
  // Run-LOCAL fitness cache keyed by canon_hash (F6 throughput: an equivalent
  // structure is scored ONCE per run). Declared per-run (not a member) so a
  // second run() with the same seed replays from a clean slate (F1). Caches the
  // raw scalar AND the S4.1 multi-objective vector (CachedScore).
  std::unordered_map<atx::u64, CachedScore> fitness_cache;
  parallel::DetPool det_pool{cfg.n_workers};

  // One reusable Engine per worker, bound to panel_, built ONCE per run() and reused
  // across EVERY generation (Tier 4). Engine holds a const Panel& (not move-assignable)
  // -> unique_ptr so the vector never assigns on growth (mirrors parallel_evaluate,
  // batch_eval.cpp:89-93). Engine::evaluate is idempotent (output depends only on
  // (program, panel_)) and panel_ is constant for the whole run, so reusing each engine
  // across generations is byte-identical to a fresh-per-generation engine — the SlotPool
  // simply grows ONCE to its peak and is reused, eliminating per-generation reallocation.
  // Run-LOCAL (not a member) so a second run() with the same seed replays from a clean
  // slate (F1) — the same lifecycle discipline as det_pool / fitness_cache / behavior_archive.
  std::vector<std::unique_ptr<alpha::Engine>> engines;
  engines.reserve(det_pool.n_workers());
  for (atx::usize w = 0; w < det_pool.n_workers(); ++w) {
    engines.push_back(std::make_unique<alpha::Engine>(panel_));
  }

  // S4.2 behavioral archive: a per-RUN ring of past-elite descriptors (declared
  // here, not a member, so a second run() with the same seed replays from a clean
  // slate — F1). Empty + unused when the behavioral objective is inactive. `nbr`
  // is the per-generation k-nearest population scratch, sized once and reused (no
  // hot-path alloc in the inner generation loop).
  BehavioralArchive behavior_archive{cfg.behavior_archive_cap};
  std::vector<std::span<const atx::f64>> nbr;

  // Initial population + start generation. Off-path (resume == nullptr) this is
  // EXACTLY the legacy `pop = init_population(cfg)` path. On a well-formed resume
  // the loop starts at resume->start_generation from the checkpoint population
  // (the canonical DSL strings captured in a prior GenerationSnapshot). The
  // resume->start_generation must be a real interior generation [1, generations);
  // a corrupt/incompatible blob (deserialize Err) returns an empty well-formed
  // result rather than silently restarting from gen 0. The impl-side validates the
  // blob hash before calling, so a valid resume always succeeds (deserialize Ok).
  std::vector<Genome> pop;
  atx::usize gen_start = 0;
  if (resume != nullptr && resume->start_generation > 0 &&
      resume->start_generation < cfg.generations && !resume->population.empty()) {
    auto restored = deserialize_population(resume->population);
    if (!restored) { // corrupt/incompatible checkpoint -> fail loud, do NOT silently restart
      SearchResult err_res;
      err_res.seed = cfg.master_seed;
      return err_res; // empty, well-formed result
    }
    pop = std::move(*restored);
    gen_start = resume->start_generation;
  } else {
    pop = init_population(cfg);
  }
  res.candidates_generated += pop.size();

  std::vector<Scored> scored; // current generation's scored population

  for (atx::usize gen = gen_start; gen < cfg.generations; ++gen) {
    // (a)-(c): evaluate the fresh (not-yet-seen) candidates of `pop`, fold the
    // determinism digest, and score each via pool_aware_fitness (cached by canon).
    scored = evaluate_generation(pop, cfg, gen, pool, canon, fitness_cache, det_pool, engines, res);

    // (d) novelty pressure -> selection fitness (anti-collapse, deterministic).
    novelty_penalize(scored, pool, cfg);

    // (d2) S4.2 behavioral-novelty pass: write the population-relative phenotypic
    // novelty into objectives[3] (n_objectives -> 4) BEFORE ranking, but ONLY when
    // the objective is active (MultiObjective && novelty_w > 0). A no-op otherwise
    // -> n_objectives stays 3 and the boundary pin is byte-untouched.
    behavioral_novelty_pass(scored, behavior_archive, cfg, nbr);

    // (d') NSGA-II rank + crowding (S4.1), assigned ONCE per generation in
    // canonical-id order BEFORE any reproduction RNG draw (F1/F2). In ScalarRaw
    // this collapses to a total order by RAW fitness (the pre-S4 path); in
    // MultiObjective it is the Pareto front rank + per-front crowding distance.
    // Reads n_objectives dynamically -> the 4th (behavioral) column enters here
    // automatically when active.
    const std::vector<atx::usize> canon_order = canon_ordered_indices(scored);
    assign_pareto_ranks(scored, canon_order, cfg);

    // (d3) S4.2 archive update: insert this generation's Pareto FRONT (rank-0
    // elites) in canonical-id order, evict oldest past C. After ranks are known,
    // before reproduction. No-op when the objective is inactive.
    update_archive(scored, canon_order, cfg, behavior_archive);

    // Track the best RAW fitness this generation (the maximized search signal,
    // NOT the novelty-penalized selection score). A best-raw elite is carried
    // verbatim into the next gen and re-scores to the same cached raw value, so
    // this sequence is non-decreasing by construction (the ElitismKeepsBest
    // guarantee).
    res.best_fitness_per_gen.push_back(best_raw(scored));

    // Progress sink (resumable-discover). Off-path (sink == nullptr) this is a
    // single null-pointer check — no work, byte-identical legacy loop. When set,
    // hand the sink a snapshot of the population that ENTERED this generation
    // (`pop` is NOT mutated by evaluate_generation — it scores into
    // scored/canon/fitness_cache only — so serializing it here is correct without
    // a loop-top copy). An Err return (real I/O failure or an injected test crash)
    // aborts cleanly: finalize the current scored set into a well-formed partial
    // result and return. canon.size() is the distinct-scored count (the CanonSet
    // local). This call runs BEFORE reproduce, so the snapshot population is the
    // exact blob a resume feeds back via SearchResumeState.
    if (sink != nullptr) {
      GenerationSnapshot snap;
      snap.generation = gen;
      snap.population = serialize_population(pop); // population that ENTERED gen `gen`
      snap.best_fitness = best_raw(scored);
      snap.mean_fitness = mean_raw(scored);
      snap.n_evaluated = canon.size();
      snap.n_unique = pop.size();
      auto st = sink->on_generation(snap);
      if (!st) { // sink-requested abort -> clean stop with a well-formed partial result
        finalize(scored, canon, res);
        return res;
      }
    }

    // (e)-(g) reproduce into the next population (skip on the final generation —
    // the last scored set is the result).
    if (gen + 1 < cfg.generations) {
      pop = reproduce(scored, cfg, gen, det_pool, res);
    }
  }

  finalize(scored, canon, res);
  return res;
}

// ----- (1) init_population -------------------------------------------------
// Parse + analyze each seed expression into an F5-valid Genome, set its
// canon_hash, then pad up to cfg.population by cycling the valid seeds (a
// deterministic, in-grammar fill — every member is analyze-valid by
// construction). At least one seed must parse; an all-invalid seed set yields an
// empty population and an empty (but well-formed) result.
[[nodiscard]] std::vector<Genome> SearchDriver::init_population(const SearchConfig &cfg) const {
  std::vector<Genome> seeds;
  for (const std::string &src : seed_exprs_) {
    auto ast = alpha::parse_expr(src, lib_);
    if (!ast.has_value()) {
      continue;
    }
    auto info = alpha::analyze(*ast);
    if (!info.has_value()) {
      continue;
    }
    Genome g{std::move(*ast), std::move(*info), 0};
    g.canon_hash = canonical_hash(g);
    seeds.push_back(std::move(g));
  }
  std::vector<Genome> pop;
  pop.reserve(cfg.population);
  if (seeds.empty()) {
    return pop; // degenerate: no valid seed -> empty (well-formed) run
  }

  // S3.5 GENERATION WIRE (plan §0.5/§0.6), GATED. With seed_from_grammar OFF (the
  // boundary-pin / ScalarRaw path) gen-0 is filled EXACTLY as pre-S4 — cycle the
  // valid seeds over every slot — so the frozen digest is byte-identical. With it
  // ON, the seeds fill the first slots and any REMAINING slots are sampled from
  // the type-correct grammar (generate_genome); a generation failure (Err — a
  // sampler bug, ~never) falls back to the cyclic seed fill for that slot, so the
  // population size is always held and every member stays analyze-valid.
  if (!cfg.seed_from_grammar) {
    for (atx::usize i = 0; i < cfg.population; ++i) {
      pop.push_back(seeds[i % seeds.size()].clone());
      pop.back().canon_hash = seeds[i % seeds.size()].canon_hash;
    }
    return pop;
  }

  const atx::usize n_seed_slots = std::min(cfg.population, seeds.size());
  for (atx::usize i = 0; i < n_seed_slots; ++i) {
    pop.push_back(seeds[i].clone());
    pop.back().canon_hash = seeds[i].canon_hash;
  }
  // Fill the remainder from the grammar. The per-slot seed is a pure function of
  // (master_seed, kGenSeedAxis, slot) — a fixed, gen-independent axis disjoint
  // from the per-generation reproduction streams (seed_for uses gen in [0,
  // generations)), so generation entropy never collides with reproduction (F1).
  constexpr atx::u64 kGenSeedAxis = 0xFFFFFFFFFFFFFFFFULL;
  for (atx::usize i = n_seed_slots; i < cfg.population; ++i) {
    Xoshiro256pp rng{detail::seed_for(cfg.master_seed, kGenSeedAxis, i)};
    auto gen = generate_genome(cfg.gen_cfg, lib_, rng);
    if (gen.has_value()) {
      gen->canon_hash = canonical_hash(*gen);
      pop.push_back(std::move(*gen));
    } else {
      pop.push_back(seeds[i % seeds.size()].clone());
      pop.back().canon_hash = seeds[i % seeds.size()].canon_hash;
    }
  }
  return pop;
}

// ----- (2) evaluate_generation --------------------------------------------
// Collect the fresh (un-seen) genomes (F6), compile them to single-root Programs,
// evaluate each on its per-worker `engines[wid]` (owned by run(), reused across
// every generation — see the EVAL-PATH NOTE below) and fold its worker-invariant
// digest into the run digest (F2), then score each NEW population member via
// pool_aware_fitness (dedup-hits reuse the cached score). A fresh candidate that
// fails to compile is dropped from the digest (F5 backstop); every distinct
// structure is recorded in all_scored + the CanonSet.
[[nodiscard]] std::vector<Scored>
SearchDriver::evaluate_generation(const std::vector<Genome> &pop, const SearchConfig &cfg,
                                  atx::usize gen, const combine::AlphaStore &pool, CanonSet &canon,
                                  std::unordered_map<atx::u64, CachedScore> &fitness_cache,
                                  parallel::DetPool &det_pool,
                                  std::vector<std::unique_ptr<alpha::Engine>> &engines,
                                  SearchResult &res) {
  // -----------------------------------------------------------------------
  // Phase 1 (serial): dedup + plan
  //
  // `fresh` = all population members not yet in canon, sorted by canonical-id
  // order so the digest fold is order-stable and replayable (F1/F2). Intra-
  // generation duplicates are kept: the current serial path folded them too, so
  // their digest contribution is preserved.
  //
  // `to_score` = pointers to the DISTINCT-new genomes in population first-
  // occurrence order (the order they are inserted into canon/fitness_cache and
  // pushed to all_scored).
  // -----------------------------------------------------------------------
  std::vector<const Genome *> fresh;
  for (const Genome &g : pop) {
    if (!canon.contains(g.canon_hash)) {
      fresh.push_back(&g);
    }
  }
  std::sort(fresh.begin(), fresh.end(),
            [](const Genome *a, const Genome *b) { return detail::canon_less(*a, *b); });

  std::vector<const Genome *> to_score;
  {
    std::unordered_set<atx::u64> seen_this_gen;
    for (const Genome &g : pop) {
      if (!canon.contains(g.canon_hash) && seen_this_gen.insert(g.canon_hash).second) {
        to_score.push_back(&g);
      }
    }
  }

  // -----------------------------------------------------------------------
  // Phase 2+3 (PARALLEL): single merged eval pass — digest + fitness share
  // ONE evaluate per genome.
  //
  // Each fresh[k] is compiled and evaluated ONCE. The digest is folded as
  // before (serial, canonical order). When fresh[k] is a to_score
  // representative (exact pointer match via score_j_of_ptr), fitness is
  // computed from the SAME SignalSet produced by that evaluate — eliminating
  // the redundant second evaluate that Phase 3 previously performed.
  //
  // SAFETY: `digest_slot`, `compiled`, and `score_slot` are pre-sized before
  // the parallel region; shard k writes ONLY digest_slot[k] and compiled[k]
  // (disjoint single-writer slots). When fresh[k] matches to_score[j], shard
  // k ALSO writes score_slot[j] — each j has EXACTLY ONE matching k (a pop
  // object's address appears in fresh at most once, and to_score contains only
  // one entry per canon_hash), so score_slot[j] is also a disjoint
  // single-writer slot. `panel_` is shared CONST (never mutated). `fresh[k]`
  // is a const pointer to a const Genome (read-only). `alpha::compile` and
  // `alpha::Engine::evaluate` are reentrant (no static/thread_local mutable
  // state). Worker `wid` touches ONLY `engines[wid]` (disjoint single-owner):
  // no cross-worker Engine access. No shared-mutable write occurs inside the
  // parallel region. The serial fold below reads digest_slot + compiled in
  // fixed canonical index order, so the accumulated digest is byte-identical
  // across all worker counts — worker scheduling CANNOT affect the fold order
  // or any slot value.
  //
  // EVAL-PATH NOTE: each worker reuses its own `engines[wid]` Engine across
  // all programs it processes (Engine::evaluate is IDEMPOTENT — output depends
  // ONLY on (program, panel_), never on prior engine state: field_remap_
  // reassigned per call, output buffers re-assigned per call, SlotPool slots
  // written-before-read, recurrence state_ seeded at t==0 on every call). The
  // SlotPool inside each Engine therefore grows monotonically to the peak
  // allocation for that worker and is reused thereafter. The digest is worker-
  // count-invariant BY CONSTRUCTION: slot k's value depends only on
  // (fresh[k], panel_), and the fold is serial in fixed canonical order.
  // No cross-genome substitution: fresh[k] digests from its OWN eval; fitness
  // for to_score[j] uses that SAME genome's SignalSet.
  // -----------------------------------------------------------------------
  const atx::usize n_fresh = fresh.size();
  std::vector<atx::u64> digest_slot(n_fresh, atx::u64{0});
  std::vector<std::uint8_t> compiled(n_fresh, std::uint8_t{0});

  // One stateless Scheduler for the merged parallel region's Tier 1 LPT
  // dispatch order. Default-constructed: the single-node fallback topology, NO
  // OS query; `dispatch_order` reads ONLY its cost-hint argument (a PURE
  // function — no clock, no thread id, no topology), so it cannot touch a
  // result bit.
  const parallel::Scheduler lpt{};

  // -----------------------------------------------------------------------
  // Tier 1 LPT (longest-processing-time-first) dispatch order for the merged
  // Phase 2+3 pass.
  //
  // `order_fresh` is a PURE permutation of [0, n_fresh) sorted by DESCENDING
  // genome size — the AST node count is a finite, NaN-free monotone proxy for
  // compile+eval cost. Dispatching the heaviest candidates first overlaps their
  // long tail with the many short ones, shrinking the generation's makespan.
  //
  // DETERMINISM: the permutation only remaps WHICH claim-position p runs WHICH
  // canonical slot k (k == order_fresh[p]). Slot k is still written by exactly
  // one shard, and the serial fold below reads digest_slot in canonical k-order,
  // so the accumulated digest is byte-identical to ANY dispatch order (the
  // Scheduler §4.4 "no bit contact" proof + the DetPool single-writer contract).
  // A uniform-cost population degenerates to the ascending-id identity order, i.e.
  // exactly the prior Tier 0 dispatch.
  // -----------------------------------------------------------------------
  std::vector<atx::f64> cost_fresh(n_fresh);
  for (atx::usize k = 0; k < n_fresh; ++k) {
    cost_fresh[k] = static_cast<atx::f64>(fresh[k]->ast.nodes().size());
  }
  const std::vector<parallel::ShardId> order_fresh = lpt.dispatch_order(cost_fresh);

  // `engines` (one reusable Engine per worker, bound to panel_) is owned by run()
  // and passed in: it is built ONCE per run and reused across every generation
  // (Tier 4), so its SlotPool grows once to peak instead of being reallocated each
  // generation. engines.size() == det_pool.n_workers() (run() sizes it from the
  // same pool). Worker `wid` touches ONLY engines[wid] (disjoint single-owner).
  // Reuse is byte-identical because Engine::evaluate is idempotent (output depends
  // only on (program, panel_)) and panel_ is constant across the run -> slot k's
  // value is independent of which worker ran it or what it evaluated before, so
  // worker-count + Tier 1 LPT-order invariance hold.

  // Pointer -> to_score index map (serial, built after to_score is finalized).
  // Each to_score[j] is the SAME pop object as exactly one fresh[k] (a pop
  // object's address is unique -> appears in `fresh` at most once). This lets
  // the single merged pass score a representative from the SAME SignalSet it
  // evaluated for the digest, eliminating Phase 3's redundant evaluate.
  // score_slot[j] therefore has exactly one writer (the unique matching shard)
  // -> the disjoint single-writer contract holds.
  std::unordered_map<const Genome *, atx::usize> score_j_of_ptr;
  score_j_of_ptr.reserve(to_score.size());
  for (atx::usize j = 0; j < to_score.size(); ++j) {
    score_j_of_ptr.emplace(to_score[j], j);
  }

  const atx::usize n_to_score = to_score.size();
  std::vector<CachedScore> score_slot(n_to_score);

  det_pool.parallel_for(n_fresh, [&](atx::usize p, atx::usize wid) {
    const atx::usize k = order_fresh[p]; // LPT remap -> canonical slot k
    auto prog = alpha::compile(fresh[k]->ast, fresh[k]->analysis);
    if (!prog.has_value()) {
      return; // compiled[k] stays 0 (F5 backstop): no digest, no fitness for this k
    }
    auto ss = engines[wid]->evaluate(*prog); // reused per-worker engine (idempotent)
    digest_slot[k] = ss.has_value() ? parallel::signal_set_digest(*ss) : atx::u64{0};
    compiled[k] = std::uint8_t{1};
    // Representative? Score it from the SAME SignalSet (no second evaluate). On
    // eval-failure (!ss) leave score_slot[j] default — matches the prior code where
    // Phase 3's own eval would fail to a default score.
    const auto it = score_j_of_ptr.find(fresh[k]);
    if (it != score_j_of_ptr.end() && ss.has_value()) {
      const atx::usize j = it->second;
      auto rep = pool_aware_fitness(*to_score[j], pool, panel_, policy_, sim_, cfg.fitness,
                                   /*weak_panel=*/nullptr, /*engine=*/engines[wid].get(),
                                   /*signals=*/&*ss);
      if (rep.has_value()) {
        score_slot[j].raw = rep->raw;
        score_slot[j].objectives = rep->objectives; // S4.1: cache the objectives
        score_slot[j].n_objectives = rep->n_objectives;
        score_slot[j].descriptor = std::move(rep->descriptor); // S4.2: canon-cache phenotype
      }
      // On Err: score_slot[j] stays default-constructed (raw=0, empty descriptor),
      // matching the prior code's behaviour for a fitness-error candidate.
    }
  });

  // Serial fold in canonical-id index order (0..n_fresh): skip non-compilable
  // genomes (compiled[k]==0), exactly matching the prior sequential loop's
  // `continue` on compile failure. This reproduces the byte-identical digest.
  // Each genome's digest comes from its OWN eval (no cross-genome substitution);
  // the serial fold order is the same as before — golden digest preserved.
  for (atx::usize k = 0; k < n_fresh; ++k) {
    if (compiled[k] != std::uint8_t{0}) {
      res.digest = static_cast<atx::u64>(
          atx::core::hash_combine(static_cast<std::size_t>(res.digest), gen, digest_slot[k]));
    }
  }

  // -----------------------------------------------------------------------
  // Phase 4 (serial): merge scores into canon/fitness_cache/all_scored, then
  // assemble the Scored output vector in population order.
  //
  // score_slot[j] was written inside the merged Phase 2+3 pass (above) by the
  // unique shard that evaluated the corresponding fresh[k] representative;
  // fitness was computed from that same evaluate's SignalSet. Merge is in
  // to_score order (== pop first-occurrence order), identical to the prior
  // sequential implementation's per-pop-member insertion sequence.
  // -----------------------------------------------------------------------
  for (atx::usize j = 0; j < n_to_score; ++j) {
    const atx::u64 hash = to_score[j]->canon_hash;
    canon.insert(hash);
    fitness_cache.emplace(hash, std::move(score_slot[j]));
    res.all_scored.push_back(to_score[j]->clone());
    res.all_scored.back().canon_hash = hash;
  }

  // Assemble output in population order (every hash is now present in
  // fitness_cache after the merge above; cached hits reuse the stored score).
  std::vector<Scored> out;
  out.reserve(pop.size());
  for (const Genome &g : pop) {
    CachedScore cs{};
    const auto it = fitness_cache.find(g.canon_hash);
    if (it != fitness_cache.end()) {
      cs = it->second; // reuse cached score (raw + objectives + descriptor), F6 dedup
    }
    Scored s{g.clone(), cs.raw, cs.raw};
    s.objectives = cs.objectives;
    s.n_objectives = cs.n_objectives;
    s.descriptor = std::move(cs.descriptor); // S4.2: phenotype for the novelty pass
    s.genome.canon_hash = g.canon_hash;
    out.push_back(std::move(s));
  }
  return out;
}

// ----- (3) novelty_penalize ------------------------------------------------
// Subtract a deterministic behavioral-distance term from each genome's selection
// fitness: the LESS novel a genome is (the smaller its mean canonical-structure
// distance to the rest of the population), the LARGER the penalty — so the search
// is pushed off a single collapsing motif. Distance is the normalized Hamming
// distance of canonical hashes (RNG-free, value-based, order-independent), so
// this is fully deterministic and F1-safe.
void SearchDriver::novelty_penalize(std::vector<Scored> &scored, const combine::AlphaStore &pool,
                                    const SearchConfig &cfg) const {
  // DIVISION OF LABOR: distance-to-POOL is already priced into each candidate's
  // fitness by pool_aware_fitness's `diversify` term (1 − mean|corr-to-pool|, F7),
  // so a pool-redundant candidate enters here ALREADY discounted. This pass adds
  // the orthogonal anti-collapse pressure the fitness score lacks: distance to the
  // rest of the POPULATION (so the search does not pile onto one motif even when
  // that motif is pool-diversifying). `pool` is therefore unused here.
  static_cast<void>(pool);
  const atx::usize n = scored.size();
  if (n <= 1 || cfg.novelty_w == 0.0) {
    return;
  }
  for (atx::usize i = 0; i < n; ++i) {
    atx::f64 sum_dist = 0.0;
    for (atx::usize j = 0; j < n; ++j) {
      if (i == j) {
        continue;
      }
      sum_dist +=
          detail::canonical_distance(scored[i].genome.canon_hash, scored[j].genome.canon_hash);
    }
    const atx::f64 mean_dist = sum_dist / static_cast<atx::f64>(n - 1);
    // novelty in [0,1]; penalty = novelty_w * (1 - novelty) (redundant => bigger).
    const atx::f64 penalty = cfg.novelty_w * (1.0 - mean_dist);
    scored[i].selection = scored[i].fitness - penalty;
  }
}

// ----- (3b) behavioral_novelty_pass (S4.2) ---------------------------------

// The single S4.2 activation gate (referenced by every S4.2 seam): the behavioral
// objective is live ONLY in MultiObjective mode with a positive novelty weight.
// ScalarRaw, or novelty_w == 0 -> off -> n_objectives stays 3 -> boundary pin holds.
[[nodiscard]] bool SearchDriver::behavioral_active(const SearchConfig &cfg) noexcept {
  return cfg.objective_mode == ObjectiveMode::MultiObjective && cfg.novelty_w > 0.0;
}

// Write each genome's population-relative behavioral novelty into objectives[3]
// and bump n_objectives to 4 — phenotypic diversity, DISTINCT from the marginal-
// corr `diversify` objective (objectives[1]). For genome i: novelty = mean
// behavioral_distance to the k nearest descriptors in (population minus self) ∪
// archive. `nbr` is the reused population-span scratch (sized once per generation;
// no inner-loop alloc). Deterministic, RNG-free. No-op when inactive (the boundary
// pin / ScalarRaw path is byte-untouched).
void SearchDriver::behavioral_novelty_pass(std::vector<Scored> &scored,
                                           const BehavioralArchive &archive,
                                           const SearchConfig &cfg,
                                           std::vector<std::span<const atx::f64>> &nbr) const {
  if (!behavioral_active(cfg)) {
    return; // gate closed: objectives[3] untouched, n_objectives stays at 3
  }
  const atx::usize n = scored.size();
  nbr.reserve(n); // size the scratch once; cleared + refilled per genome (no realloc)
  for (atx::usize i = 0; i < n; ++i) {
    // Population neighbourhood = every OTHER genome's descriptor (exclude self so a
    // genome's own 0-distance never collapses its novelty). Reuse `nbr`'s capacity.
    nbr.clear();
    for (atx::usize j = 0; j < n; ++j) {
      if (j == i) {
        continue;
      }
      nbr.push_back(std::span<const atx::f64>{scored[j].descriptor});
    }
    const atx::f64 nov = archive.novelty(std::span<const atx::f64>{scored[i].descriptor},
                                         std::span<const std::span<const atx::f64>>{nbr},
                                         cfg.behavior_k, cfg.behavior_metric);
    scored[i].objectives[3] = nov; // the 4th NSGA-II objective (maximization)
    // Fixed-slot bookkeeping (S4.3): novelty owns slot 3; cost owns slot 4. Bump
    // n_objectives to COVER slot 3 without CLOBBERING a higher active slot — when
    // the cost objective is on, finish_report already set n_objectives to 5, and
    // max() preserves it so assign_pareto_ranks keeps reading all 5 columns. With
    // cost off this is exactly 4 (the pre-S4.3 behavior).
    scored[i].n_objectives = static_cast<atx::u8>(std::max<atx::usize>(scored[i].n_objectives, 4U));
  }
}

// Insert this generation's Pareto FRONT (rank-0 elites) into the behavioral archive
// in CANONICAL-ID order, evicting oldest past C. After assign_pareto_ranks (reads
// .rank), before reproduction. No-op when the objective is inactive (archive stays
// empty). The canonical-id insert order makes the archive contents + FIFO eviction
// byte-deterministic and worker-count-invariant.
void SearchDriver::update_archive(const std::vector<Scored> &scored,
                                  const std::vector<atx::usize> &canon_order,
                                  const SearchConfig &cfg, BehavioralArchive &archive) {
  if (!behavioral_active(cfg)) {
    return;
  }
  for (const atx::usize i : canon_order) {
    if (scored[i].rank == 0) { // the non-dominated front: the generation's elites
      archive.insert(std::span<const atx::f64>{scored[i].descriptor});
    }
  }
}

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
[[nodiscard]] std::vector<Genome> SearchDriver::reproduce(const std::vector<Scored> &scored,
                                                          const SearchConfig &cfg, atx::usize gen,
                                                          parallel::DetPool &det_pool,
                                                          SearchResult &res) {
  if (scored.empty()) {
    return {};
  }
  // Canonical-id order BEFORE any RNG draw (F2). Index into this order for the
  // value-based parent pool; rank elites by NSGA-II survivor order (S4.1) — in
  // ScalarRaw this is exactly the pre-S4 raw-descending order. Both are pure,
  // RNG-free permutations of `scored`, computed ONCE here and read-only thereafter.
  const std::vector<atx::usize> canon_order = canon_ordered_indices(scored);
  const std::vector<atx::usize> elite_order = pareto_ordered_indices(scored);

  const atx::usize n_elites = std::min(cfg.elites, scored.size());
  const atx::usize n_children = (cfg.population > n_elites) ? cfg.population - n_elites : 0;

  // (e) reproduction (PARALLEL): id-seeded children fill the non-elite slots.
  //
  // SAFETY: child slot p (population index i = n_elites + p) is written by EXACTLY
  // one shard -> disjoint single-writer slots (the DetPool contract proven for
  // evaluate_generation). Its RNG is Xoshiro256pp{seed_for(master_seed, gen, i)} —
  // a PURE function of (master_seed, gen, i), never worker/thread/claim-order (F1).
  // The make_child path (tournament_pick + crossover/mutation + analyze +
  // canonical_hash) and the F5-reject elite-clone fallback (elite_order[i %
  // max(n_elites,1)] — pure in i, NO shared counter / NO order dependence) read
  // ONLY shared-CONST state (scored, canon_order, elite_order, cfg, catalog_,
  // panel_field_views_, lib_) plus per-call locals — audited reentrant, with no
  // static/thread_local/mutable/shared-scratch state. So child_slot[p] depends only
  // on (i, scored, cfg), independent of worker count and claim order -> byte-identical
  // across {1,2,4} workers. No LPT remap: per-child cost is not known a priori (the
  // tournament parent is drawn inside make_child), and determinism rests on the
  // single-writer slots + per-index seed, NOT on dispatch order.
  std::vector<std::optional<Genome>> child_slot(n_children);
  det_pool.parallel_for(n_children, [&](atx::usize p, atx::usize /*wid*/) {
    const atx::usize i = n_elites + p;
    Xoshiro256pp rng{detail::seed_for(cfg.master_seed, gen, i)};
    auto child = make_child(scored, canon_order, cfg, rng);
    if (child.has_value()) {
      child->canon_hash = canonical_hash(*child);
      child_slot[p] = std::move(*child);
    } else {
      // F5 reject -> hold population size with an elite clone (deterministic, pure in i).
      const atx::usize fallback = elite_order[i % std::max<atx::usize>(n_elites, 1)];
      Genome clone = scored[fallback].genome.clone();
      clone.canon_hash = scored[fallback].genome.canon_hash;
      child_slot[p] = std::move(clone);
    }
  });

  // candidates_generated is order-INDEPENDENT: exactly n_children children are
  // produced regardless of which shard ran which slot. Folded as a closed-form add
  // OUTSIDE the parallel region (the old per-child `++` was a shared-counter write).
  res.candidates_generated += n_children;

  // Serial assembly in POPULATION order (elites first, then children in slot order)
  // — byte-identical to the prior sequential push_back sequence. Elite clones are
  // cheap and stay serial (no parallel benefit; avoids cloning into the slot vector).
  std::vector<Genome> next;
  next.reserve(cfg.population);
  for (atx::usize e = 0; e < n_elites; ++e) {
    next.push_back(scored[elite_order[e]].genome.clone());
    next.back().canon_hash = scored[elite_order[e]].genome.canon_hash;
  }
  for (atx::usize p = 0; p < n_children; ++p) {
    next.push_back(std::move(*child_slot[p]));
  }
  return next;
}

// Produce one child from the canonical-id-ordered parent pool with a single
// id-seeded rng: bernoulli(p_cross) ? crossover(two tournament picks) :
// mutate(one tournament pick). The operators' analyze backstop guarantees an
// Ok child is F5-valid; an Err propagates (the caller substitutes an elite).
[[nodiscard]] atx::core::Result<Genome>
SearchDriver::make_child(const std::vector<Scored> &scored,
                         const std::vector<atx::usize> &canon_order, const SearchConfig &cfg,
                         Xoshiro256pp &rng) {
  const Genome &p1 = tournament_pick(scored, canon_order, cfg, rng);
  if (rng.bernoulli(cfg.p_cross)) {
    const Genome &p2 = tournament_pick(scored, canon_order, cfg, rng);
    return subtree_crossover(p1, p2, rng, CrossoverCfg{cfg.max_lookback});
  }
  return mutate_one(p1, cfg, rng);
}

// Pick one type-safe mutation by a seeded draw, fallback-cascading so a
// degenerate genome (e.g. no literal to jitter) still yields a child when ANY
// operator applies. Each operator self-validates (analyze backstop, F5).
//
// op_swap is gated behind cfg.enable_op_swap (default ON since S3.4 fixed the
// root cause — analyze's validate_node_contract + the materialized-arity buckets
// make analyze-valid ⟹ VM-safe). The seeded draw uses a FIXED modulus (3)
// regardless of the gate so the RNG stream — and therefore the replay (F1) —
// does not shift when the gate flips; a drawn-but-disabled op_swap simply falls
// through to jitter_const.
[[nodiscard]] atx::core::Result<Genome>
SearchDriver::mutate_one(const Genome &g, const SearchConfig &cfg, Xoshiro256pp &rng) {
  const atx::u64 which = rng.next_u64() % 3;
  JitterCfg jc;
  jc.max_lookback = cfg.max_lookback;
  if (which == 0 && cfg.enable_op_swap) {
    auto r = op_swap(g, catalog_, rng);
    if (r.has_value()) {
      return r;
    }
  } else if (which == 1) {
    auto r = field_swap(g, std::span<const std::string_view>{panel_field_views_}, rng);
    if (r.has_value()) {
      return r;
    }
  }
  // Default / fallback: jitter a literal (the most broadly-applicable mutation).
  return jitter_const(g, rng, jc);
}

// ----- selection helpers ---------------------------------------------------

// Indices of `scored` in canonical-id order (value-based, RNG-free). Established
// before any draw so the seeded tournament is replayable (F2).
[[nodiscard]] std::vector<atx::usize>
SearchDriver::canon_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// Indices of `scored` ranked by DESCENDING selection fitness (penalized score);
// ties broken by canonical order so the rank is deterministic (F1). NOTE: this is
// the .selection ranking — it is no longer used for elitism (which switched to
// raw_ordered_indices, below); retained for any selection-pressure consumer.
[[nodiscard]] std::vector<atx::usize>
SearchDriver::elite_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    if (scored[a].selection != scored[b].selection) {
      return scored[a].selection > scored[b].selection;
    }
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// Indices of `scored` ranked by DESCENDING RAW fitness (the maximized search
// signal); ties broken by canonical order so the rank is deterministic (F1).
// This is the elitism + admitted-candidate ordering: carrying the best-raw genome
// verbatim each gen (and re-scoring it from the canon-keyed cache to the SAME raw
// value) makes best_fitness_per_gen non-decreasing BY CONSTRUCTION.
[[nodiscard]] std::vector<atx::usize>
SearchDriver::raw_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    if (scored[a].fitness != scored[b].fitness) {
      return scored[a].fitness > scored[b].fitness;
    }
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// S4.1: assign NSGA-II rank + crowding to every Scored, ONCE per generation, in
// canonical-id order, BEFORE any reproduction RNG draw (F1/F2).
//
//  ScalarRaw (the pre-S4 path): collapse to a TOTAL order by RAW fitness — rank ==
//  the position in raw_ordered_indices (raw desc + canon tie-break), crowding == 0.
//  Then pareto_ordered_indices (sort by rank asc) reproduces raw_ordered_indices
//  EXACTLY, and the tournament's .selection comparison is untouched, so the
//  boundary pin stays byte-identical.
//
//  MultiObjective: build an ObjMatrix over the first n_objectives columns (in
//  scored index order), run fast_nondominated_sort(canon_order) -> per-genome
//  rank, then for each front run crowding_distance(front_members, canon_order) ->
//  per-genome crowding. All ordering inside pareto.hpp is canonical-id stable.
void SearchDriver::assign_pareto_ranks(std::vector<Scored> &scored,
                                       const std::vector<atx::usize> &canon_order,
                                       const SearchConfig &cfg) {
  const atx::usize n = scored.size();
  if (n == 0) {
    return;
  }
  if (cfg.objective_mode == ObjectiveMode::ScalarRaw) {
    const std::vector<atx::usize> raw_order = raw_ordered_indices(scored);
    for (atx::usize pos = 0; pos < raw_order.size(); ++pos) {
      scored[raw_order[pos]].rank = static_cast<atx::u16>(pos);
      scored[raw_order[pos]].crowding = 0.0;
    }
    return;
  }

  // MultiObjective: how many objective columns are live (uniform across a run; a
  // 0-objective scored set degenerates to a single front, crowding 0).
  atx::usize k = 0;
  for (const Scored &s : scored) {
    k = std::max<atx::usize>(k, s.n_objectives);
  }
  if (k == 0) {
    for (Scored &s : scored) {
      s.rank = 0;
      s.crowding = 0.0;
    }
    return;
  }

  // Flat row-major [n * k] objective buffer, sized once (cold per-generation path).
  std::vector<atx::f64> flat(n * k);
  for (atx::usize i = 0; i < n; ++i) {
    for (atx::usize o = 0; o < k; ++o) {
      flat[i * k + o] = scored[i].objectives[o];
    }
  }
  const ObjMatrix obj{flat, n, k};
  const std::vector<atx::u16> front_of = fast_nondominated_sort(obj, canon_order);
  for (atx::usize i = 0; i < n; ++i) {
    scored[i].rank = front_of[i];
    scored[i].crowding = 0.0;
  }
  // Per-front crowding distance. Group ids by front (in canon_order so each
  // front's member list is canonical-id stable), then score each group.
  atx::u16 max_front = 0;
  for (const atx::u16 f : front_of) {
    max_front = std::max(max_front, f);
  }
  for (atx::u16 f = 0; f <= max_front; ++f) {
    std::vector<atx::usize> members;
    for (const atx::usize i : canon_order) {
      if (front_of[i] == f) {
        members.push_back(i);
      }
    }
    if (members.empty()) {
      continue;
    }
    const std::vector<atx::f64> cd = crowding_distance(obj, members, canon_order);
    for (const atx::usize i : members) {
      scored[i].crowding = cd[i];
    }
  }
}

// S4.1: indices of `scored` in NSGA-II survivor order — (rank asc, crowding desc,
// canon_less). Elitism keeps the first front then crowding up to `elites`; the
// admitted-candidate list reads the same order. In ScalarRaw this reduces to the
// exact pre-S4 raw_ordered_indices (rank == raw-descending position, crowding 0,
// canonical tie-break). Requires assign_pareto_ranks to have run.
[[nodiscard]] std::vector<atx::usize>
SearchDriver::pareto_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    if (scored[a].rank != scored[b].rank) {
      return scored[a].rank < scored[b].rank; // lower front first
    }
    if (scored[a].crowding != scored[b].crowding) {
      return scored[a].crowding > scored[b].crowding; // larger crowding first
    }
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// k-tournament over the canonical-id-ordered parent pool: draw k indices into
// `canon_order` with the seeded rng, return the BETTER candidate. MultiObjective
// compares (rank asc, crowding desc) — the NSGA-II crowded operator; ScalarRaw
// compares .selection > (the EXACT pre-S4 rule). Ties -> the earlier draw / lower
// canonical-id slot (deterministic). The candidate set is iterated in fixed
// canonical order and the RNG DRAW SEQUENCE is identical in both modes (only the
// comparison differs), so the draw is fully replayable (F1/F2).
[[nodiscard]] const Genome &
SearchDriver::tournament_pick(const std::vector<Scored> &scored,
                              const std::vector<atx::usize> &canon_order, const SearchConfig &cfg,
                              Xoshiro256pp &rng) {
  const atx::usize n = canon_order.size();
  const atx::usize k = cfg.k_tournament;
  atx::usize best = canon_order[static_cast<atx::usize>(rng.next_u64() % n)];
  for (atx::usize t = 1; t < std::max<atx::usize>(k, 1); ++t) {
    const atx::usize cand = canon_order[static_cast<atx::usize>(rng.next_u64() % n)];
    const bool better = (cfg.objective_mode == ObjectiveMode::MultiObjective)
                            ? crowded_better(scored[cand], scored[best])
                            : (scored[cand].selection > scored[best].selection);
    if (better) {
      best = cand;
    }
  }
  return scored[best].genome;
}

// The NSGA-II crowded-comparison operator <_n (Deb §III-C): `a` is better than
// `b` iff it has a strictly lower front rank, or an equal rank with strictly
// greater crowding distance. No canonical tie-break here (the ordered-index +
// tournament callers iterate in fixed canonical order, so equal (rank, crowding)
// resolves to the earlier slot deterministically). RNG-free, noexcept.
[[nodiscard]] bool SearchDriver::crowded_better(const Scored &a, const Scored &b) noexcept {
  if (a.rank != b.rank) {
    return a.rank < b.rank;
  }
  return a.crowding > b.crowding;
}

// ----- result assembly -----------------------------------------------------

// Best RAW fitness in the scored set (the maximized search signal, NOT the
// novelty-penalized .selection). This is what best_fitness_per_gen tracks; the
// structural elite carry guarantees it is non-decreasing across generations.
[[nodiscard]] atx::f64 SearchDriver::best_raw(const std::vector<Scored> &scored) {
  atx::f64 best = 0.0;
  bool any = false;
  for (const Scored &s : scored) {
    if (!any || s.fitness > best) {
      best = s.fitness;
      any = true;
    }
  }
  return best;
}

// Mean RAW fitness over the scored set (telemetry for the progress sink; NOT part
// of the digest or admission). NaN/inf-safe: skips non-finite members; empty -> 0.
[[nodiscard]] atx::f64 SearchDriver::mean_raw(const std::vector<Scored> &scored) {
  atx::f64 sum = 0.0;
  atx::usize n = 0;
  for (const auto &s : scored) {
    if (std::isfinite(s.fitness)) {
      sum += s.fitness;
      ++n;
    }
  }
  return n ? sum / static_cast<atx::f64>(n) : 0.0;
}

// dedup_pct + admitted candidates (top survivors of the final generation by
// RAW fitness — same ordering as the structural elite carry, so the run's
// reported best matches what was preserved across generations). trial_count ==
// the distinct structures scored (CanonSet).
void SearchDriver::finalize(const std::vector<Scored> &scored, const CanonSet &canon,
                            SearchResult &res) const {
  res.trial_count = canon.size();
  if (res.candidates_generated > 0) {
    res.dedup_pct = 1.0 - static_cast<atx::f64>(res.trial_count) /
                              static_cast<atx::f64>(res.candidates_generated);
    res.dedup_pct = std::clamp(res.dedup_pct, 0.0, 1.0);
  }
  // Admitted in NSGA-II survivor order (S4.1): first front then crowding; in
  // ScalarRaw this is exactly the pre-S4 raw-descending order, so the boundary-pin
  // result's admitted list is byte-identical.
  std::vector<atx::usize> order = pareto_ordered_indices(scored);
  for (const atx::usize i : order) {
    res.admitted_candidates.push_back(scored[i].genome.clone());
    res.admitted_candidates.back().canon_hash = scored[i].genome.canon_hash;
  }
}

// ----- population checkpoint helpers (resumable-discover, Task 1) ------------

std::vector<std::string>
SearchDriver::serialize_population(const std::vector<Genome> &pop) const {
  std::vector<atx::usize> order(pop.size());
  for (atx::usize i = 0; i < pop.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(),
            [&](atx::usize a, atx::usize b) { return detail::canon_less(pop[a], pop[b]); });
  std::vector<std::string> out;
  out.reserve(pop.size());
  for (atx::usize i : order) {
    out.push_back(alpha::unparse(pop[i].ast));
  }
  return out;
}

atx::core::Result<std::vector<Genome>>
SearchDriver::deserialize_population(const std::vector<std::string> &exprs) const {
  std::vector<Genome> out;
  out.reserve(exprs.size());
  for (const auto &src : exprs) {
    ATX_TRY(auto ast, alpha::parse_expr(src, lib_));
    ATX_TRY(auto g, analyze_into(std::move(ast)));
    g.canon_hash = canonical_hash(g);
    out.push_back(std::move(g));
  }
  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::factory
