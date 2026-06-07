#pragma once

// atx::engine::factory — formulaic alpha factory forward declarations (Sprint S3).
//
// A lightweight header other factory headers include to NAME the factory-layer
// types without pulling in the genome, mutation, canonicalization, search, or
// fitness machinery behind them.
//
// WHAT THIS LAYER IS: the engine already EVALUATES thousands of overlapping DSL
// expressions cheaply (hash-consed DAG + cross-alpha CSE + S2 parallel fan-out).
// S3 is the seeded, deflated, pool-aware SEARCH that sits on top — it DISCOVERS
// alphas instead of merely scoring them: a deterministic evolutionary search over
// the Phase-3 alpha DSL (mutation + crossover + fractional-constant param search)
// whose fitness is a candidate's MARGINAL contribution to a diversified pool (not
// standalone Sharpe), with canonical-hash dedup so structurally-equivalent
// expressions are never re-evaluated, and S1 deflation so a pure-noise population
// admits NOTHING.
//
// THREE LOAD-BEARING CONTRACTS (every unit is checked against them):
//   F1 SEED-BY-ID DETERMINISM: every RNG draw comes from atx::core::Xoshiro256pp
//      seeded by (master_seed, generation, candidate_index) — NEVER worker/thread/
//      time. A seeded run replays BYTE-IDENTICALLY; the seed is in the artifact.
//   F4 DEFLATION KILLS SNOOPING: the running trial count N feeds
//      eval::deflated_sharpe(...,N,...)/eval::pbo_cscv. A pure-noise population must
//      admit NOTHING after deflation (the non-vacuous anti-snooping proof).
//   F6 CANONICAL-DEDUP SOUNDNESS: hash-equal => VM evaluates bit-identical;
//      commutative reorder => same hash; genuinely-different expr => different hash.
//
// THE GENOME IS A REBUILT alpha::Ast, NOT an edited tree: the Ast is a flat,
// index-addressed, relocatable arena with no in-place mutator — every mutation /
// crossover constructs a NEW Ast through the public builder (add/intern/add_root)
// and re-runs alpha::analyze() as the validity oracle. Expr::op borrows a
// const alpha::OpSig* from the Library, which MUST outlive every genome and is
// shared across the whole run.
//
// Full definitions live in (added per unit):
//   factory/genome.hpp        — Genome (Ast + cached Analysis + canon_hash),       (S3-1)
//                               clone/clone_subtree/rebuild_with/validate
//   factory/op_catalog.hpp    — OpCatalog (op candidates by (Shape,DType,arity)    (S3-1)
//                               + declared-commutative set; Library has no iterator)
//   factory/mutation.hpp      — op_swap, field_swap, jitter_const (window/scale/    (S3-1)
//                               hparam literal classifier); analyze-validated
//   factory/crossover.hpp     — subtree_crossover at TypeInfo-compatible cuts       (S3-2)
//   factory/canonical.hpp     — canonical_hash (commutative-sort + fold + stable    (S3-2)
//                               byte layout, field-NAME keyed), CanonSet dedup
//   factory/param_search.hpp  — ParamSpace + optimize_params (Grid/Random/          (S3-3)
//                               SepCmaEs; diagonal-covariance, no eigendecomp)
//   factory/fitness.hpp       — corr_to_pool + pool_aware_fitness (WQ fitness ×     (S3-4)
//                               diversification × robustness, OOS via CPCV, deflated)
//   factory/search_driver.hpp — SearchDriver (seeded population loop:               (S3-5)
//                               init→batch-eval→dedup→select→reproduce→elitism)
//   factory/factory.hpp       — Factory::mine (mine→gate→admit over AlphaStore +    (S3-6)
//                               AlphaGate; trial accounting; dedup/CSE telemetry)
//
// EVAL SWITCH POINT: a generation is scored via the S2 parallel path —
//   parallel::parallel_evaluate(progs, panel, pool)   // Pool = parallel::Pool
// whose signal_set_digest is byte-identical to the single-thread
//   alpha::Engine(panel).evaluate(prog)
// path and invariant across worker counts (S2's load-bearing contract). The
// single-thread Engine::evaluate call is the correct, slower fallback; the
// factory consumes the parallel path through that one call site.

namespace atx::engine::factory {

// The genome: a value-owned alpha::Ast + its cached alpha::Analysis + the stable
// canonical-dedup key. Rebuilt (never edited) by every search operator (S3-1).
struct Genome;

// Precomputed op-candidate lists grouped by (result Shape, out DType, arity) plus
// the declared-commutative subset — built once from the OpCode set + the Library's
// named rows, because alpha::Library exposes no iterator and no commutative flag (S3-1).
class OpCatalog;

// The free fractional constants of a template genome (window / scale literals) with
// per-dimension bounds — the search space the parameter optimizer ranges over (S3-3).
struct ParamSpace;

// One candidate's scored result: WQ fitness, redundancy/diversification, sub-universe
// robustness, the raw search signal, and the DEFLATED admission statistic (S3-4).
struct FitnessReport;

// The seeded, deterministic evolutionary population loop (S3-5).
class SearchDriver;

// The mine → gate → admit driver over the P4 AlphaStore + AlphaGate (S3-6).
class Factory;

} // namespace atx::engine::factory
