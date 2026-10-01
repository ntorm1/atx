# Alpha lanes (owner directive 2026-09-30: more parallel lanes on alpha generation in atx-engine + atx-impl)

Every lane: read `lane-rules.md` first (binding; never build C++, never run real data, never dispatch subagents,
Write/Edit tools for files, commit per item with the Opus trailer, report file committed with `git add -f`).
Then `.agents/cpp/agent.md` for C++. Generic machinery in `atx-engine`, strategy-specific code in `atx-impl`, every
change behind a flag or provably value-preserving with the flag absent; state in the report how root verifies
identity (gtest filter, identity argv). Pre-registration (`v8-prereg.md`) binds: one variant per hypothesis, every
parameter fixed blind by the lane and written in the report as the registration (precedent: Ruling E-30); nothing
is read from the 2020-2023 window. Rulings E-37, E-38, E-39 in `progress.md` bind the slots.

## Lane RISK (pool 9, branch `feat/platform-v8-risk-20260930` from `28051c4c` in pool 7's history -- fetch is not
needed, the commit is in the shared repository): task R-8, ex-ante risk target
Plan `docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md`, task R-8 (section "Task R-8 (optional)"), verbatim:
`L_t = clip(sigma_star / (b x sigma_hat_t), .8 L, 1.25 L)`, sigma_star 5.0%, b 1.15, sigma_hat from `book_variance`
on the gross-1 current book, updated every 21 sessions; files `strategy_nav_v7.cpp:312-384`. Acceptance (report
only here): realised volatility inside [.8, 1.2] x sigma_star each year AND dSR not lower by more than one SE.
Build: the scaler as a reusable atx-engine component (input: current weights, factor covariance from the risk
store, cadence; output: leverage multiplier), the NAV / spo flag `--risk-target 0.05` with the bias constant and
the cadence as flags defaulting to the registered values, spo-v3 compatible (the aim's leverage is scaled before
tracking), manifest and summary fields (`risk_target`, per-session `sigma_hat`, `L_t`), a cell template
`scripts/specs/v8/r8.json` following the A2 templates (`task-A2-report.md` section 3) with the year table and
E-29 capacity curve. Flag absent: byte-identical. gtests: scaler on a synthetic covariance (closed-form), clip
bounds, cadence, flag-absent identity on the pinned bench.
Report: `task-R-8-report.md`.

## Lane DLRET (pool 4, branch `feat/platform-v8-dlret-20260930` from `fd2ff7a8`): delisting returns for
linked-operating-v1 (B0c critical path, Ruling E-39)
`w0-2-runbook.md` R15: only linked-operating-v2/v3 take `--delisting` / `--delisting-returns`
(`prepare_recent_research.py:665-670, 962-966`); lo1 has no such path. Add `--delisting-returns` to
linked-operating-v1: returns on terminated members are corrected exactly as v3 does (same function, no copy),
membership and every other payload unchanged, manifest records `returns_applied: true` and the delisting events
digest so FIX-AB's B-3 refusal and R45's E-25 check (`check_label_manifests`, declared clearing cells) accept it as
a label role for an lo1 signal role. Flag absent: byte-identical role. Tests on synthetic roles: (a) flag absent
identity, (b) the corrected returns equal v3's on the same synthetic events, (c) the E-25 check admits the pair.
Report: `task-DLRET-report.md`.

## Lane LIB2 (pool 8, branch `feat/platform-v8-lib2-20260930` from `fd2ff7a8`): library v8.2 candidates for the
three remaining admission trials (Ruling E-38)
Read `library-v8-draft.md` sections 0, 4, 5, 8, 10 and `task-F-3-report.md`. Trial accounting: 15 admission trials
at most; v8.0 7 + v8.1 5 = 12; three remain. Choose, blind, three candidates from section 10 ("not included") or
from the literature, under the draft's conventions (section 0: canonical definition, literature sign, one DSL
string, tier, citation), preferring themes with the fewest library members and signals orthogonal in construction
to the roster (different data source, different horizon). Any candidate that needs a field not in fields v11 gets
its builder in `atx-engine/tools/research_fields_*.py` (point-in-time: every value available at the session it is
stamped on, reader-side seal from `research_window`, reuse inputs complete per findings B-1 and N-1), a fields
v12 spec entry, and tests on synthetic data (oracle values computed in the test, a look-ahead probe that fails,
the reuse fingerprint). No atx-db edits. Write the three `add-alpha` command lines (strings frozen) and the
registry rows in the report; they are the registration of R-12's screen set.
Report: `task-LIB2-report.md`.

## Lane COMB2 (pool 11, branch `feat/platform-v8-comb2-20260930` from `fd2ff7a8`): composition rule
`ic-shrink-v1` (cell R-10, slot per Ruling E-38)
Read `task-R-1-report.md` (composition v8, ew-theme-std-v1, its five rules in C++ `strategy_ic_composition.cpp`
and Python `fit_composition_weights.py`), Ruling E-27, and `review-w1-T.md` T-1. Hypothesis: inside each theme,
member weights proportional to a shrunk estimate of each member's IC are a better combination than equal tier
weights. Rule (fix every constant blind, write it in the report as the registration): the fitter's IC estimate of
each member over the fit window the parent fitter already uses (no new window, no new read), James-Stein shrinkage
toward the theme's equal weight with a fixed shrinkage intensity, negative shrunk weights floored at 0, theme share
1 / T and the member cap 1 / (2T) as in ew-theme-std-v1, standardisation unchanged. C++ rule id `ic-shrink-v1` in
its own file beside the existing rules, registered in the rule table; Python fitter path; cell template
`scripts/specs/v8/r10.json` (parent = last accepted). Tests: C++ against values computed in the test from the
written rule (non-degenerate fixture per T-1), Python fitter equals C++ on the same fixture, flag absent identity.
Report: `task-R-10-report.md`.

## Lane ORTH (pool 10, branch `feat/platform-v8-orth-20260930` from `fd2ff7a8`): composition rule
`theme-resid-v1` (cell R-11, slot per Ruling E-38)
Same reading as COMB2. Hypothesis: theme composites residualised against the preceding themes (registered theme
order, cross-sectional least squares per session, intercept included, on the standardised composites) carry
uncorrelated alpha and combine better than raw composites. Rule (constants blind, written as the registration):
residualise in registered order, re-standardise each residual composite, then combine with the parent's theme
shares; member weights inside each theme unchanged. C++ rule id `theme-resid-v1` in its own file; Python fitter
path; cell template `scripts/specs/v8/r11.json`. Tests: closed-form two-theme fixture (orthogonality of the
residual to the preceding composite to 1e-12), Python equals C++, flag absent identity.
Report: `task-R-11-report.md`.
