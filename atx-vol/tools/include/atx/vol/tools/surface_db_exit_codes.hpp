#pragma once

// ── The ONE exit vocabulary shared by both surface-db CLIs ───────────────────
//
// REV-R5 (review I-4). `atx-vol-surface-db-build` and `atx-vol-surface-db` are
// run back to back by the same wrapper scripts, so one number must mean one
// thing across both binaries: a caller reads either tool's status without a
// per-tool lookup table. That contract is why the build CLI's refusal verdict is
// 5 and not the obvious 4 — 4 was already spent on `verify`'s ABSENT verdict.
//
// UNTIL THIS HEADER EXISTED THE CONTRACT WAS ENFORCED BY COMMENT ONLY. The build
// CLI's constants lived in `surface_db_build.hpp` and were pinned by
// `SurfaceDbBuildExitCode.TheCodesThemselvesAreAContract`; `verify`'s 4 was a
// file-local `constexpr` in `tools/surface_db_main.cpp` with no shared header and
// no test — so the two halves of a cross-binary invariant were never in scope
// together, and nothing failed if either moved onto the other. Three comments
// asserted the invariant; nothing checked it. This file is the check: both
// vocabularies are declared here, the `static_assert`s below fail the BUILD on a
// collision, and `SurfaceDbExitVocabulary.*` pins the same relations as a test.
//
// This header is deliberately dependency-free (no includes, no types): every
// consumer — both `main()`s and the test — can take it without taking anything
// else, so there is never a reason to restate a code locally.
//
// ADDING A CODE: put it here, give it a tool prefix, and extend the asserts. A
// code that appears in one tool's `main()` as a literal is outside the contract
// and will not be caught by anything.

namespace atx::vol {

// ── Codes 0-2: the SAME meaning in both tools ────────────────────────────────
//
// These three are common ground and the asserts below pin that they agree
// numerically. A script may branch on them without knowing which tool it ran.

// The tool did what was asked.
inline constexpr int kSurfaceDbBuildExitOk = 0;
// The tool or the db broke and there is no report to read — OR (the only shape
// `build_exit_code` can return it for) the run was otherwise fine and the single
// thing that failed is the `--report` CSV the operator asked for. It is the
// LOWEST-priority verdict: `main` has already printed the full report to stdout.
inline constexpr int kSurfaceDbBuildExitReportWriteFailed = 1;
// 2 is a usage error. It is decided before the db is opened, so no report exists
// and `build_exit_code` can never return it; named here only so the vocabulary
// reads as a whole.
inline constexpr int kSurfaceDbBuildExitUsage = 2;

// ── 3 and 5: `atx-vol-surface-db-build` only ─────────────────────────────────

// The build ran to completion and produced NOTHING — at INGEST (every present
// file in the window unreadable), at CONFIG SELECTION, or at the FIT. ONE code
// for all three stages: a script asks "did this run produce anything?", and the
// stderr diagnostic names which stage swallowed it.
inline constexpr int kSurfaceDbBuildExitTotalFitFailure = 3;
// REV-R3 (review C-02/F-02). At least one date was REFUSED because committing its
// rewrite would have destroyed a stored surface. Not exit 3: the refusal is the
// tool WORKING, other dates were built normally, and nothing was lost. The
// operator's next action differs in kind — 3 says "fix your inputs and re-run",
// this says "your inputs would have deleted data; decide first".
//
// 4 IS DELIBERATELY SKIPPED — see `kSurfaceDbVerifyExitAbsentOverLimit` below,
// which owns it, and the `static_assert` that now makes the skip mandatory
// rather than customary.
inline constexpr int kSurfaceDbBuildExitCoverageRegression = 5;

// ── `atx-vol-surface-db`, and the 4 the build CLI steps around ───────────────

// The admin CLI's spellings of the common codes. Same numbers, and the asserts
// below are what keeps them the same numbers; they exist so `run_verify` and its
// siblings can name what they return instead of writing a bare literal.
inline constexpr int kSurfaceDbVerifyExitOk = 0;
// A runtime failure, OR `verify` found cells that failed a gate (`verdict
// FAILED`). Both are "this database is not serving what you asked of it", and
// the verdict on stdout separates them.
inline constexpr int kSurfaceDbVerifyExitFailed = 1;
inline constexpr int kSurfaceDbVerifyExitUsage = 2;
// `verify` found more ABSENT cells than `--max-absent` allows (`verdict ABSENT`).
// Its own code, and deliberately not 1: absence is a coverage answer over an
// otherwise intact database, and a script that treats "the database is damaged"
// and "the database is missing two more cells than last month" identically will
// act wrongly on one of them. Never reached without the flag — a converged
// production database is permanently non-zero on `cells_absent`, so an
// unconditional non-zero here would rebuild the permanently-red verdict that
// whole change removes (the same argument `is_carry_masked_fit_failure` records
// for keeping the build CLI at exit 0 — surface_db_build.hpp).
inline constexpr int kSurfaceDbVerifyExitAbsentOverLimit = 4;
// FINAL-REVIEW I1. `band-audit --fail-on-flagged` scored ZERO expiries: the gate
// ran and measured NOTHING (hive root moved/renamed, a `--from/--to` window
// matching no partition, or every model price non-finite). Its own code, and
// deliberately not 3: 3 says "the audit measured the database and found rows
// below the floor" — an answer — while this says "there is no answer here at
// all", and the operator's next action differs in kind (fix the invocation or
// the inputs, not the surfaces). Not 0 either, which is the fail-open bug it
// exists to close: a CI gate that passes while auditing nothing is not a gate.
//
// 6, NOT 4, AND NOT 5. Both were already spent — 4 by `verify`'s ABSENT verdict
// in THIS SAME binary (so a wrapper reading 4 off `atx-vol-surface-db` would
// have to know which subcommand ran to interpret it) and 5 by the build CLI's
// coverage-regression refusal. 6 is the first free number under the rule this
// header exists to enforce. Reached ONLY with `--fail-on-flagged`: without the
// flag the same emptiness prints the same loud warning and exits 0, because
// interactive inspection of an empty window is a legitimate thing to do.
inline constexpr int kSurfaceDbBandAuditExitScoredNothing = 6;

// ── The invariant, checked at COMPILE TIME in every TU that includes this ────
//
// A collision is now a build failure in both CLIs and in the test binary, which
// is strictly stronger than a test: a change that renumbers either tool cannot
// reach a reviewer, let alone an operator.

static_assert(kSurfaceDbBuildExitOk == kSurfaceDbVerifyExitOk,
              "0 must mean 'the tool did what was asked' in BOTH surface-db CLIs");
static_assert(kSurfaceDbBuildExitReportWriteFailed == kSurfaceDbVerifyExitFailed,
              "1 must mean 'runtime failure / no usable answer' in BOTH surface-db CLIs");
static_assert(kSurfaceDbBuildExitUsage == kSurfaceDbVerifyExitUsage,
              "2 must mean 'usage error' in BOTH surface-db CLIs");

// The tool-specific codes are the ones that can collide, and this is the whole
// point of the file: 3 and 5 belong to the build CLI, 4 belongs to verify, and
// no tool-specific code may reuse a common one.
static_assert(kSurfaceDbBuildExitTotalFitFailure != kSurfaceDbVerifyExitAbsentOverLimit,
              "atx-vol-surface-db verify owns 4 for `verdict ABSENT`; the build CLI's "
              "total-failure code must not take it");
static_assert(kSurfaceDbBuildExitCoverageRegression != kSurfaceDbVerifyExitAbsentOverLimit,
              "atx-vol-surface-db verify owns 4 for `verdict ABSENT`; the build CLI's "
              "coverage-regression refusal must not be 'tidied' down onto it");
static_assert(kSurfaceDbBuildExitTotalFitFailure != kSurfaceDbBuildExitCoverageRegression,
              "the build CLI's two verdicts must stay distinguishable");
static_assert(kSurfaceDbBandAuditExitScoredNothing != kSurfaceDbVerifyExitAbsentOverLimit,
              "atx-vol-surface-db verify owns 4 for `verdict ABSENT`; band-audit's "
              "measured-nothing code must not take it — they share a binary");
static_assert(kSurfaceDbBandAuditExitScoredNothing != kSurfaceDbBuildExitTotalFitFailure &&
                  kSurfaceDbBandAuditExitScoredNothing != kSurfaceDbBuildExitCoverageRegression,
              "band-audit's measured-nothing code must not collide with either build "
              "CLI verdict");
static_assert(kSurfaceDbBuildExitTotalFitFailure > kSurfaceDbBuildExitUsage &&
                  kSurfaceDbBuildExitCoverageRegression > kSurfaceDbBuildExitUsage &&
                  kSurfaceDbVerifyExitAbsentOverLimit > kSurfaceDbBuildExitUsage &&
                  kSurfaceDbBandAuditExitScoredNothing > kSurfaceDbBuildExitUsage,
              "a tool-specific verdict must not reuse one of the three common codes");

} // namespace atx::vol
