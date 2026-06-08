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

#include <algorithm> // std::clamp, std::sort, std::max, std::min
#include <bit>       // std::popcount (canonical-structure novelty distance)
#include <cstddef>   // std::size_t (hash_combine seed type)
#include <span>      // std::span
#include <string>    // std::string (seed-expression source input)
#include <string_view> // std::string_view (field-swap candidate names)
#include <unordered_map> // std::unordered_map (per-run fitness cache, F6 throughput)
#include <utility>   // std::move
#include <vector>

#include "atx/core/error.hpp"  // Result, Ok, Err
#include "atx/core/hash.hpp"   // atx::core::hash_combine
#include "atx/core/random.hpp" // atx::core::Xoshiro256pp
#include "atx/core/types.hpp"  // atx::u64, atx::usize, atx::f64

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"   // alpha::parse_expr, alpha::Library
#include "atx/engine/alpha/typecheck.hpp" // alpha::analyze
#include "atx/engine/alpha/vm.hpp"        // alpha::Engine (digest eval, fresh per program)

#include "atx/engine/combine/store.hpp" // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/weight_policy.hpp" // engine::WeightPolicy

#include "atx/engine/parallel/det_pool.hpp" // parallel::DetPool (wired S2 handle)
#include "atx/engine/parallel/digest.hpp"   // parallel::signal_set_digest

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
  // op_swap (S3-1) is DISABLED by default: as-built it can rebuild an
  // analyze-VALID genome whose compiled Program corrupts the VM SlotPool at
  // evaluate (a verified abort, isolated to op_swap — field_swap / jitter_const /
  // subtree_crossover are clean). Until that S3-1 defect is fixed, the mutation
  // mix is field_swap + jitter_const (+ crossover). Flip this true only against a
  // fixed op_swap. See mutate_one().
  bool enable_op_swap{false};
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
               std::vector<std::string> seed_exprs, std::vector<std::string> panel_fields)
      : lib_{lib}, panel_{panel}, policy_{policy}, sim_{sim}, catalog_{lib},
        seed_exprs_{std::move(seed_exprs)}, panel_fields_{std::move(panel_fields)} {
    panel_field_views_.reserve(panel_fields_.size());
    for (const std::string &f : panel_fields_) {
      panel_field_views_.push_back(f);
    }
  }

  // Run the deterministic search. Same cfg + same pool => byte-identical result
  // (F1). `pool` is borrowed read-only for the marginal-correlation fitness term.
  [[nodiscard]] SearchResult run(const SearchConfig &cfg, const combine::AlphaStore &pool) {
    SearchResult res;
    res.seed = cfg.master_seed;
    res.best_fitness_per_gen.reserve(cfg.generations);

    CanonSet canon;                 // F6 dedup: distinct structures scored so far
    // Run-LOCAL fitness cache keyed by canon_hash (F6 throughput: an equivalent
    // structure is scored ONCE per run). Declared per-run (not a member) so a
    // second run() with the same seed replays from a clean slate (F1).
    std::unordered_map<atx::u64, atx::f64> fitness_cache;
    parallel::DetPool det_pool{cfg.n_workers};

    std::vector<Genome> pop = init_population(cfg);
    res.candidates_generated += pop.size();

    std::vector<Scored> scored; // current generation's scored population

    for (atx::usize gen = 0; gen < cfg.generations; ++gen) {
      // (a)-(c): evaluate the fresh (not-yet-seen) candidates of `pop`, fold the
      // determinism digest, and score each via pool_aware_fitness (cached by canon).
      scored = evaluate_generation(pop, cfg, gen, pool, canon, fitness_cache, det_pool, res);

      // (d) novelty pressure -> selection fitness (anti-collapse, deterministic).
      novelty_penalize(scored, pool, cfg);

      // Track the best RAW fitness this generation (the maximized search signal,
      // NOT the novelty-penalized selection score). A best-raw elite is carried
      // verbatim into the next gen and re-scores to the same cached raw value, so
      // this sequence is non-decreasing by construction (the ElitismKeepsBest
      // guarantee).
      res.best_fitness_per_gen.push_back(best_raw(scored));

      // (e)-(g) reproduce into the next population (skip on the final generation —
      // the last scored set is the result).
      if (gen + 1 < cfg.generations) {
        pop = reproduce(scored, cfg, gen, res);
      }
    }

    finalize(scored, canon, res);
    return res;
  }

private:
  // ----- (1) init_population -------------------------------------------------
  // Parse + analyze each seed expression into an F5-valid Genome, set its
  // canon_hash, then pad up to cfg.population by cycling the valid seeds (a
  // deterministic, in-grammar fill — every member is analyze-valid by
  // construction). At least one seed must parse; an all-invalid seed set yields an
  // empty population and an empty (but well-formed) result.
  [[nodiscard]] std::vector<Genome> init_population(const SearchConfig &cfg) const {
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
    for (atx::usize i = 0; i < cfg.population; ++i) {
      pop.push_back(seeds[i % seeds.size()].clone());
      pop.back().canon_hash = seeds[i % seeds.size()].canon_hash;
    }
    return pop;
  }

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
                      parallel::DetPool &det_pool, SearchResult &res) {
    // Fresh genomes (canonical-id ordered so the digest is order-stable).
    std::vector<const Genome *> fresh;
    for (const Genome &g : pop) {
      if (!canon.contains(g.canon_hash)) {
        fresh.push_back(&g);
      }
    }
    std::sort(fresh.begin(), fresh.end(),
              [](const Genome *a, const Genome *b) { return detail::canon_less(*a, *b); });

    // Compile each fresh candidate to a single-root Program and fold its eval
    // digest into the run digest (F2), in canonical-id order (fresh was sorted
    // above) so the digest is order-stable and replayable (F1/F2).
    //
    // EVAL-PATH NOTE (the §0.8 switch, as-built S2 reality): the spec's primary
    // path is parallel::parallel_evaluate, whose digest is byte-identical to the
    // single-thread Engine::evaluate and worker-count-invariant. parallel_evaluate
    // HAS been verified to SUCCEED on the seed grammar (rank / ts_mean / ts_std /
    // delta) with a worker-invariant digest (see the F2 test). We nonetheless take
    // the single-thread fresh-Engine FALLBACK — which the spec EXPLICITLY sanctions
    // (§4.8 tail) — because S2's per-worker Engine warm-up + reuse remains a
    // RESIDUAL concern for arbitrary EVOLVED stateful candidates produced mid-search
    // (not just the seed set): warm-up evaluates progs[0] then re-evaluates on the
    // reused Engine, and we have not exhaustively verified that every reachable
    // evolved Ts/Cs program leaves the reused SlotPool live-count clean. A FRESH
    // Engine per program (never reused) sidesteps that concern entirely: it is
    // exactly pool_aware_fitness's internal eval (which the S3-4 suite exercises
    // green), and its digest is worker-count-invariant BY CONSTRUCTION — no worker
    // count enters the math at all. det_pool is retained (the wired S2 handle) so
    // the parallel path can be reinstated once the warm-up + reuse path is verified
    // clean across the full evolved grammar.
    static_cast<void>(det_pool);
    for (const Genome *g : fresh) {
      auto prog = alpha::compile(g->ast, g->analysis);
      if (!prog.has_value()) {
        continue; // F5 backstop: a non-compilable structure is dropped
      }
      alpha::Engine engine{panel_}; // fresh Engine per program (no reuse, no warm-up)
      auto ss = engine.evaluate(*prog);
      const atx::u64 prog_digest =
          ss.has_value() ? parallel::signal_set_digest(*ss) : atx::u64{0};
      res.digest = static_cast<atx::u64>(
          atx::core::hash_combine(static_cast<std::size_t>(res.digest), gen, prog_digest));
    }

    // (c) score each member via pool_aware_fitness (its own single-thread OOS eval
    // is the fitness oracle; the digest loop above is the determinism fingerprint).
    // F6 THROUGHPUT: a candidate whose canon_hash is already in `fitness_cache_` is
    // NOT re-scored — its cached fitness is reused, so an equivalent expression
    // costs ONE eval across the whole run (the dedup lever, measured by dedup_pct).
    // A NEW structure is scored once, cached, recorded in all_scored, and inserted
    // into the CanonSet. A fresh candidate whose fitness errors (F5/eval failure)
    // scores 0 but is still counted as a distinct trial (it WAS attempted).
    std::vector<Scored> out;
    out.reserve(pop.size());
    for (const Genome &g : pop) {
      atx::f64 fit = 0.0;
      const bool is_new = !canon.contains(g.canon_hash);
      if (is_new) {
        auto rep = pool_aware_fitness(g, pool, panel_, policy_, sim_, cfg.fitness);
        if (rep.has_value()) {
          fit = rep->raw;
        }
        canon.insert(g.canon_hash);
        fitness_cache.emplace(g.canon_hash, fit);
        res.all_scored.push_back(g.clone());
        res.all_scored.back().canon_hash = g.canon_hash;
      } else {
        const auto it = fitness_cache.find(g.canon_hash);
        fit = (it != fitness_cache.end()) ? it->second : 0.0; // reuse cached score
      }
      out.push_back(Scored{g.clone(), fit, fit});
      out.back().genome.canon_hash = g.canon_hash;
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
  void novelty_penalize(std::vector<Scored> &scored, const combine::AlphaStore &pool,
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
        sum_dist += detail::canonical_distance(scored[i].genome.canon_hash,
                                               scored[j].genome.canon_hash);
      }
      const atx::f64 mean_dist = sum_dist / static_cast<atx::f64>(n - 1);
      // novelty in [0,1]; penalty = novelty_w * (1 - novelty) (redundant => bigger).
      const atx::f64 penalty = cfg.novelty_w * (1.0 - mean_dist);
      scored[i].selection = scored[i].fitness - penalty;
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
  [[nodiscard]] std::vector<Genome> reproduce(const std::vector<Scored> &scored,
                                              const SearchConfig &cfg, atx::usize gen,
                                              SearchResult &res) {
    if (scored.empty()) {
      return {};
    }
    // Canonical-id order BEFORE any RNG draw (F2). Index into this order for the
    // value-based parent pool; rank elites separately by RAW fitness.
    std::vector<atx::usize> canon_order = canon_ordered_indices(scored);
    std::vector<atx::usize> elite_order = raw_ordered_indices(scored);

    std::vector<Genome> next;
    next.reserve(cfg.population);

    // (g) elitism: carry the top-k by RAW fitness (F5-valid by construction).
    const atx::usize n_elites = std::min(cfg.elites, scored.size());
    for (atx::usize e = 0; e < n_elites; ++e) {
      next.push_back(scored[elite_order[e]].genome.clone());
      next.back().canon_hash = scored[elite_order[e]].genome.canon_hash;
    }

    // (e) reproduction: id-seeded children fill the remainder.
    for (atx::usize i = n_elites; i < cfg.population; ++i) {
      Xoshiro256pp rng{detail::seed_for(cfg.master_seed, gen, i)};
      auto child = make_child(scored, canon_order, cfg, rng);
      ++res.candidates_generated;
      if (child.has_value()) {
        child->canon_hash = canonical_hash(*child);
        next.push_back(std::move(*child));
      } else {
        // F5 reject -> hold population size with an elite clone (deterministic).
        const atx::usize fallback = elite_order[i % std::max<atx::usize>(n_elites, 1)];
        next.push_back(scored[fallback].genome.clone());
        next.back().canon_hash = scored[fallback].genome.canon_hash;
      }
    }
    return next;
  }

  // Produce one child from the canonical-id-ordered parent pool with a single
  // id-seeded rng: bernoulli(p_cross) ? crossover(two tournament picks) :
  // mutate(one tournament pick). The operators' analyze backstop guarantees an
  // Ok child is F5-valid; an Err propagates (the caller substitutes an elite).
  [[nodiscard]] atx::core::Result<Genome> make_child(const std::vector<Scored> &scored,
                                                     const std::vector<atx::usize> &canon_order,
                                                     const SearchConfig &cfg, Xoshiro256pp &rng) {
    const Genome &p1 = tournament_pick(scored, canon_order, cfg.k_tournament, rng);
    if (rng.bernoulli(cfg.p_cross)) {
      const Genome &p2 = tournament_pick(scored, canon_order, cfg.k_tournament, rng);
      return subtree_crossover(p1, p2, rng, CrossoverCfg{cfg.max_lookback});
    }
    return mutate_one(p1, cfg, rng);
  }

  // Pick one type-safe mutation by a seeded draw, fallback-cascading so a
  // degenerate genome (e.g. no literal to jitter) still yields a child when ANY
  // operator applies. Each operator self-validates (analyze backstop, F5).
  //
  // op_swap is gated behind cfg.enable_op_swap (default OFF): as-built it can emit
  // an analyze-valid genome that corrupts the VM at evaluate (an uncatchable abort
  // — an S3-1 defect; field_swap / jitter_const are clean). The seeded draw uses a
  // FIXED modulus (3) regardless of the gate so the RNG stream — and therefore the
  // replay (F1) — does not shift when the gate flips; a drawn-but-disabled op_swap
  // simply falls through to jitter_const.
  [[nodiscard]] atx::core::Result<Genome> mutate_one(const Genome &g, const SearchConfig &cfg,
                                                     Xoshiro256pp &rng) {
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
  [[nodiscard]] static std::vector<atx::usize>
  canon_ordered_indices(const std::vector<Scored> &scored) {
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
  [[nodiscard]] static std::vector<atx::usize>
  elite_ordered_indices(const std::vector<Scored> &scored) {
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
  [[nodiscard]] static std::vector<atx::usize>
  raw_ordered_indices(const std::vector<Scored> &scored) {
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

  // k-tournament over the canonical-id-ordered parent pool: draw k indices into
  // `canon_order` with the seeded rng, return the one with the highest selection
  // fitness (ties -> the earlier canonical-id slot, deterministic). The candidate
  // set is iterated in fixed canonical order, so the draw is fully replayable (F2).
  [[nodiscard]] static const Genome &tournament_pick(const std::vector<Scored> &scored,
                                                     const std::vector<atx::usize> &canon_order,
                                                     atx::usize k, Xoshiro256pp &rng) {
    const atx::usize n = canon_order.size();
    atx::usize best = canon_order[static_cast<atx::usize>(rng.next_u64() % n)];
    for (atx::usize t = 1; t < std::max<atx::usize>(k, 1); ++t) {
      const atx::usize cand = canon_order[static_cast<atx::usize>(rng.next_u64() % n)];
      if (scored[cand].selection > scored[best].selection) {
        best = cand;
      }
    }
    return scored[best].genome;
  }

  // ----- result assembly -----------------------------------------------------

  // Best RAW fitness in the scored set (the maximized search signal, NOT the
  // novelty-penalized .selection). This is what best_fitness_per_gen tracks; the
  // structural elite carry guarantees it is non-decreasing across generations.
  [[nodiscard]] static atx::f64 best_raw(const std::vector<Scored> &scored) {
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

  // dedup_pct + admitted candidates (top survivors of the final generation by
  // RAW fitness — same ordering as the structural elite carry, so the run's
  // reported best matches what was preserved across generations). trial_count ==
  // the distinct structures scored (CanonSet).
  void finalize(const std::vector<Scored> &scored, const CanonSet &canon,
                SearchResult &res) const {
    res.trial_count = canon.size();
    if (res.candidates_generated > 0) {
      res.dedup_pct = 1.0 - static_cast<atx::f64>(res.trial_count) /
                                static_cast<atx::f64>(res.candidates_generated);
      res.dedup_pct = std::clamp(res.dedup_pct, 0.0, 1.0);
    }
    std::vector<atx::usize> order = raw_ordered_indices(scored);
    for (const atx::usize i : order) {
      res.admitted_candidates.push_back(scored[i].genome.clone());
      res.admitted_candidates.back().canon_hash = scored[i].genome.canon_hash;
    }
  }

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
