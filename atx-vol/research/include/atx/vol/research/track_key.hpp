#pragma once

// TrackKey -- content-addressed identity for one backtest "track" (Task D1,
// backtest-production-lakehouse sprint). This is the identity cornerstone the
// rest of the lakehouse (D2-D6, C3) keys on: two runs that would produce the
// SAME economics get the SAME key regardless of which host, thread count, or
// snapshot-pool topology produced them; any run that would produce DIFFERENT
// economics gets a different key.
//
// ## What goes into the key
//
//   TrackKey = SHA-256(len(canonical_config) || canonical_config
//                       || len(engine_id) || engine_id
//                       || data_snapshot_id)
//
// `canonical_config` -- a DETERMINISTIC BYTE ENCODING (not a full RunArchive
// serialization; see "Why not RunArchive" below) of:
//   * `BacktestStrategyTemplate`'s existing economic fingerprint
//     (`fingerprint_backtest_template`, backtest_template.cpp) -- every leg,
//     entry cadence, holding rule, hedge, projection settings, the
//     TEMPLATE-level `FrictionModel`, and settlement rule. Catalog id/name are
//     already excluded there; reused verbatim rather than re-derived so there
//     is exactly one definition of "the template's economics."
//   * The RunConfig ECONOMICS fields enumerated below, field-by-field,
//     defaults materialized (a default-constructed `RunConfig` always
//     encodes, never "whatever the caller happened to leave unset"), doubles
//     bit-patterned via `std::bit_cast<std::uint64_t>` (signed zero
//     normalized to +0.0 so `-0.0` and `0.0` cannot collide with two
//     different keys for the same economic value), enums by their declared
//     underlying integer, little-endian throughout, no padding bytes ever
//     touch the hash (every field is written individually -- struct-memcpy
//     of RunConfig/BacktestStrategyTemplate is never used here).
//
// `engine_id` -- ATX_VOL_VERSION_STRING + kBacktestEconomicsRev + the
// RunArchive schema hash (`ra_schema_hash()`), via `make_engine_id()`. This is
// the "which build of the engine, at which economics revision, against which
// on-disk schema" component: unlike canonical_config it is not a property of
// one run's settings, it is a property of the BINARY that ran it.
// `make_track_key` takes it as caller-supplied data rather than reading
// `kBacktestEconomicsRev` itself, which is what lets a test exercise "the rev
// changed" without recompiling against a different constant.
//
// `data_snapshot_id` -- SHA-256 over the sorted per-date SurfaceDb partition
// content identities the run actually consumed (already recorded as BacktestDb
// source lineage: `BacktestSourcePartition` / `ArchiveContentIdentity` in
// backtest_db.hpp). Computed by the CALLER, not this header: track_key.hpp is
// research-tier and must not depend upward on backtest_db.hpp (which itself
// depends on this tier's run_archive.hpp) or on SurfaceDb; accepting a
// pre-computed 32-byte digest keeps the layering one-directional.
//
// ## Why not full RunArchive reuse for canonical_config
//
// RunArchive sections are built through an arena-backed writer
// (`RaSectionData` / `RaSectionArena`, see run_archive.hpp) that expects a
// live archive-write pipeline -- reasonable for something that is actually
// persisted, heavyweight for a byte buffer that exists only to be hashed and
// discarded. The brief explicitly permits a deterministic canonical byte
// encoding as an alternative when full reuse is heavyweight; this file takes
// that option. If canonical_config ever needs to be READ BACK (not just
// hashed), promoting it to a real RunArchive section is the natural next step
// and does not change `make_track_key`'s signature or hashing contract.
//
// ## Reused SHA-256
//
// `research_db.cpp` (ResearchDb) does not carry its own SHA-256 -- it already
// depends on atx-core's canonical `atx::core::Sha256` (atx-core/include/atx/
// core/sha256.hpp). track_key.cpp adopts that SAME dependency directly rather
// than hoisting a new atx-vol-local `detail/sha256.hpp` wrapper: there is
// exactly one SHA-256 implementation in the tree either way, and adding a
// pass-through header here would only be a second name for the same code.
//
// ## Economics vs execution -- the RunConfig field-by-field split
//
// RunConfig has exactly TWENTY-TWO fields (drift-pinned in backtest.hpp).
// THIRTEEN are economics (change what a run COMPUTES, or -- reconcile_nav's
// fail-closed abort -- whether it produces a result AT ALL) and NINE are
// execution (change how fast / on what topology it computes the SAME thing,
// or are non-deterministic runtime handles that cannot be hashed at all).
// Getting this split wrong in the DANGEROUS direction (omitting an economics field)
// silently serves a wrong cached result; getting it wrong in the SAFE
// direction (including an execution field) only costs a cache hit. Every call
// below is documented so a reviewer can re-derive it from RunConfig's own doc
// comments without re-deriving the engine's determinism invariants from
// scratch.
//
// INCLUDED -- canonical_config_bytes() encodes these, in RunConfig
// declaration order:
//
//   query_pricing_tier          "an explicit backtest-level accuracy/latency
//                                choice" (backtest.hpp): changes computed
//                                marks, not just their cost.
//   query_cache_build_policy    ReuseOnly resolves a fast-tier miss to a
//                                separately-keyed COLD snapshot instead of
//                                building; snapshot_pool.hpp calls the same
//                                knob "history-dependent pricing" -- capable
//                                of moving a computed mark.
//   frictions (FrictionModel)   named explicitly by the brief. Encodes
//                                spread_kind, half_spread_bps, vol_tick,
//                                impact_fraction, per_contract_cost,
//                                hedge_slippage_bps, crossing_fraction_single
//                                / _complex, and whether quote_lookup is set
//                                (the callable itself cannot be hashed, but
//                                set-vs-unset changes QuoteSide fill
//                                behaviour -- see FrictionModel's own doc).
//   financing (FinancingConfig) named explicitly by the brief. Encodes
//                                borrow_rate, finance_premium, shares_carry,
//                                initial_cash, share_dividends (in order:
//                                uid, ex_ts_ns, amount), reference_uid, and
//                                flat_r (presence + value).
//   unpriced                    UnpricedLotPolicy: ExcludeAndReport vs Error
//                                changes which lots' P&L reaches NAV.
//   surface_provenance_policy   gates which archived surfaces admit at all;
//                                changes the priced universe.
//   reconcile_nav /              FIX-ROUND 1 (post-review correction -- see
//   reconcile_nav_tol           task-D1-report.md): reconcile_row
//                                (backtest.cpp:3415-3429), called via
//                                ATX_TRY_VOID at backtest.cpp:3804/:4168,
//                                returns Err and ABORTS THE RUN -- no
//                                BacktestResult at all -- the first recorded
//                                row whose (NAV - independently-recomputed
//                                liquidation) drift exceeds reconcile_nav_tol,
//                                but ONLY when reconcile_nav is true. Originally
//                                misclassified as execution ("gates a post-hoc
//                                assertion... does not change the NAV/cash
//                                walk"), which is true of the WALK but ignores
//                                the fail-closed abort -- exactly the
//                                clock_gaps/margin_breach pattern below. A run
//                                cached under reconcile_nav=false (completes,
//                                undetected drift and all) served to a caller
//                                requesting reconcile_nav=true would silently
//                                bypass the validation guarantee that caller
//                                asked for.
//   book_entry_fill_slippage    "the gap is charged into cost (hence into
//                                NAV and, exactly once, into cash)"
//                                (backtest.hpp): directly moves NAV.
//   swap_fixing_cadence         wrong cadence "silently overstated ... by
//                                roughly the gap factor" (backtest.hpp):
//                                directly moves swap accrual.
//   clock_gaps                  Accept vs Error: Error refuses to run at all
//                                rather than compute a number -- a cache keyed
//                                as if Accept would silently SERVE a result
//                                for a config that must fail closed.
//   margin_breach                Halt aborts the run on a margin shortfall
//                                instead of completing it; same "would
//                                silently serve a result for a config that
//                                must fail closed" reasoning as clock_gaps.
//   exercise_policy               Simulate (unlike the Advisory default)
//                                mutates the book/cash/hedge ledger on an
//                                early exercise: a direct NAV change.
//
// EXCLUDED -- execution; must NOT enter the key, or identical economics on
// different topologies gets different keys and the whole cache stops
// deduplicating:
//
//   price (PriceOptions)          n_threads: "Output is bit-identical to any
//                                thread count" (portfolio_pricer.hpp).
//                                analytic_greeks: price and every Greek
//                                EXCEPT theta/charm stay bit-identical to the
//                                FD path; theta/charm become the exact PDE
//                                value. KNOWN LIMITATION, documented rather
//                                than silently accepted: a track's theta/
//                                charm COLUMNS are compute-path-dependent and
//                                are NOT covered by this identity, which is
//                                scoped (per the brief) to economics (NAV /
//                                cash / P&L), not to every column. A future
//                                consumer that needs theta/charm
//                                reproducibility from the cache needs its own
//                                key component for PriceOptions -- it must not
//                                be folded silently into "economics".
//   record_every_n                downsamples which STEPS get RECORDED; the
//                                NAV/cash walk itself is computed identically
//                                every step regardless. Changes the shape of
//                                the persisted series, not any value in it.
//   step_observer                  a caller-supplied std::function; not
//                                serializable, not part of persisted state.
//   cancel                         a non-owning cooperative-cancellation flag.
//   snapshot_cache / snapshot_pool the brief's own named example: "the bytes
//                                are identical either way, so every column is
//                                bit-identical" (RunConfig::snapshot_pool
//                                doc).
//   prefetch_snapshots /            "OUTPUT IS UNAFFECTED AT ANY DEPTH"
//   prefetch_depth                 (RunConfig::prefetch_depth doc); the
//                                brief's own named example.
//   settlement_mark_memo            "ON vs OFF is bit-for-bit identical
//                                output" (RunConfig doc, verbatim).
//
// If a reviewer disagrees with any EXCLUDED call above, the fix is additive
// (move one field's encoding from the execution list into
// `canonical_config_bytes`): it never requires touching `make_track_key`'s
// signature or hex format, only widening what canonical_config covers.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"          // RunConfig
#include "atx/vol/backtest_template.hpp" // BacktestStrategyTemplate
#include "atx/vol/types.hpp"             // Result, ErrorCode

namespace atx::vol {

// Bumped ONLY when a correctness/behavior change moves the golden NAV pin
// (`kGolden82SessionFinalNav`, golden_pin.hpp). Participates in `engine_id`
// (via `make_engine_id()`) and therefore in every `TrackKey`: bumping it
// invalidates every previously-cached track, which is the point -- a track
// computed under the OLD economics must never be served for the NEW ones.
inline constexpr int kBacktestEconomicsRev = 1;

// Stable content identity for one backtest track. `sha256` is the raw digest;
// `hex()` is its lowercase 64-character hex rendering, stable across process
// restarts (pinned by TrackKeyTest.GoldenHexPinIsStableAcrossRestarts).
struct TrackKey {
  std::array<std::uint8_t, 32> sha256{};

  [[nodiscard]] std::string hex() const;

  [[nodiscard]] bool operator==(const TrackKey &) const noexcept = default;
};

// The inverse of `TrackKey::hex()` (Task D5). Needed because a `TrackKey` is
// not always available where a track's identity is: `TrackStore::compact()`
// (track_store.cpp) reads only the hex string back out of a staged file's
// Feather metadata (the original `TrackKey` the writer held is long gone by
// then), so a caller that wants to act on that identity -- e.g. calling
// `Catalog::mark_compacted`, which takes a `TrackKey`, not a hex string --
// needs to parse it back. Err(InvalidArgument) unless `hex` is EXACTLY 64
// lowercase hex characters -- `hex()` only ever emits lowercase
// (track_key.cpp's `kHexDigits`), so a string that did not round-trip
// through `hex()` (wrong length, uppercase, or a non-hex character) is
// rejected rather than silently canonicalized.
[[nodiscard]] Result<TrackKey> track_key_from_hex(std::string_view hex);

// Deterministic canonical byte encoding of the economics-relevant subset of
// (strategy_template, run_config) -- see the field-by-field enumeration
// above. Two calls with field-for-field-equal inputs produce byte-identical
// output; any single economics field difference produces different output.
[[nodiscard]] std::vector<std::uint8_t>
canonical_config_bytes(const BacktestStrategyTemplate &strategy_template,
                       const RunConfig &run_config);

// ATX_VOL_VERSION_STRING + kBacktestEconomicsRev + ra_schema_hash(), joined by
// '|'. Deterministic within one build (every input is a compile-time
// constant), so stable across process restarts; changes only when the
// library version, the economics revision, or the RunArchive schema moves.
[[nodiscard]] std::string make_engine_id();

// TrackKey = SHA-256 over the length-prefixed concatenation of
// canonical_config and engine_id, followed by the fixed-width 32-byte
// data_snapshot_id. Length-prefixing the two variable-length inputs makes the
// concatenation unambiguous (no boundary shift between adjacent fields can
// collide two different (config, id) pairs onto the same bytes).
[[nodiscard]] TrackKey make_track_key(std::span<const std::uint8_t> canonical_config,
                                      std::string_view engine_id,
                                      std::span<const std::uint8_t, 32> data_snapshot_id);

} // namespace atx::vol
