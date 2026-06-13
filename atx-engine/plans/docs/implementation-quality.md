# Implementation Quality Standard

**Purpose:** Define the coding standard that sprint orchestrators must pass to
subagent implementers. This is the guardrail for production-quality ATS code:
complete behavior, clear APIs, focused tests, and comments that explain real
complexity.

## Reference Style

Use [`../../include/ats_orderbook.h`](../../include/ats_orderbook.h) as the positive
style reference:

- The module-level comment explains what the component does and what it does
  not own.
- Constants, return codes, state structs, lifecycle APIs, apply APIs, and query
  APIs are grouped into readable sections.
- Public structs and functions document ownership, valid inputs, side effects,
  and return behavior.
- Comments explain invariants, domain semantics, and control-flow hazards
  rather than restating obvious assignments.

Do not copy weaker header patterns where a file exposes many constants,
structs, and prototypes without enough lifecycle or error-contract context.
When a provider-facing API must be broad, the comments and grouping need to be
stronger, not thinner.

## Public API Standard

Every public header touched by a sprint should answer these questions in the
header itself:

- What is this module responsible for?
- What owns memory, file handles, arenas, buffers, or table state?
- Which objects are borrowed, and which are destroyed by the API?
- What ordering, partitioning, crash, or replay invariants must callers honor?
- What are the valid inputs and sentinel values?
- What do return codes mean, and which failures are fail-closed?
- Which functions are hot-path APIs versus test/debug/operator helpers?

Prefer small, named structs over loosely related argument lists once an API has
state that must stay consistent. Keep names domain-accurate and consistent with
nearby ATS code.

## End-To-End Completion

A unit is not done when a symbol compiles. It is done when the useful behavior
works end to end:

- Public API, implementation, tests, docs or ledger update, and build/test gate
  are all present in the same unit.
- No fake success paths, placeholder stubs, unused exported APIs, or TODO-based
  behavior claims.
- Error paths clean up owned resources and leave durable state safe to reopen.
- Tests cover success, important failure modes, and domain edge cases.
- Benchmarks record commands, rows/bytes, timing scope, host/build context, and
  validation method when performance is part of the claim.

Prefer a smaller complete slice over a larger partial skeleton.

## Comment Standard

Comments should be intelligent and concise.

Good comments explain:

- Ownership and lifecycle.
- Non-obvious invariants.
- Replay, storage, ordering, crash, or concurrency semantics.
- Why a fail-closed branch exists.
- Why a performance-sensitive structure is shaped a certain way.

Avoid comments that merely narrate code:

- "Assign x to y."
- "Loop over rows."
- "Return success."
- Section art that overwhelms the API contract.

When logic is complex, a short orienting comment before the block is better
than many line-by-line comments inside it.

## Test Standard

Tests should prove behavior, not just helper functions:

- Use the existing `ATS_TEST(...)` framework.
- Add focused tests in the same commit as the code.
- Include negative tests for invalid arguments, schema mismatch, bad ordering,
  fail-closed provider semantics, and crash/recovery windows when relevant.
- Prefer tiny deterministic fixtures for unit tests.
- Use real fixture evidence only in committed docs or benchmark summaries; do
  not commit licensed or large provider data.

## Subagent Handoff Block

Paste this into coding subagent briefs:

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level intent, grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and concise comments that explain invariants, non-obvious control flow, or domain semantics. Do not follow weaker patterns that expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering, crash/recovery semantics, and tricky domain rules. Do not comment obvious assignments or wrap every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

## Reviewer Gate

Reviewers should reject a unit, even if it compiles, when:

- A public API lacks ownership/lifecycle/error-contract clarity.
- The implementation is a partial stub or fake pass-through.
- The code adds an abstraction without removing real complexity.
- Comments are noisy but miss the hard invariants.
- Tests do not exercise the user-facing behavior.
- Performance claims are not backed by recorded bench evidence.
