# P9-X exact preparation and hand read plan

Preparation only under R15-X-PREP / resume15-ledger-26, at HEAD
`3c699d4051a856266ea1427864a2a11f14d0b961`. No X spec, measurement or trial exists.
MV is REJECTED; W LAPSED (0), with no invented dSR. Final unlevered P9-F0 =
P9-F0h = accepted H. PRE30-MVX requires a new X trial; PRE-34's old-Y1
zero-trial agreement branch is inapplicable. No V work is authorized here.

## Exact registration and frozen prerequisites

The companion `p9x-leverage-proposal.json` is normative for the complete proposed
template, its LF serialization, exact resolved NAV argv, bounded reader argv,
all binding paths, and numeric limits. Proposed file:
`scripts/specs/p9/templates/p9x-leverage.json`; SHA-256
`e8a308b593b67c16111f3e4792c92983e9c5cfd412c7e42d485ecfe089b549db`.
Parent `theme-cov-hedge-v1-attempt2-gm.json` SHA-256
`1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`.
Closed scientific delta: leverage 2.0 and `--vol-target vol-target-v1` with
`--risk-model build-equity/p9-m2i-dcov-v11` and its manifest SHA. Other changes
are own name/description/NAV and monitor output paths, explicit existing exe
pins and locked reference hashes. Gross_match is none, a protocol property;
no unsupported v1 JSON key, calibration, v2 leverage wave or MV target/store.
Inherit completed H u/fit/card/w, aim-partial-v5, fixed cadence 1, trade fraction
.05, exact flag-absent fit, v15, and each print opt-in exactly once. H's existing
NAV turnover flags remain inherited; B.7's scaled reader limits below decide.

Current preparation streamed and matched the H template, registered H mechanics
file and primary daily hash, all three candidate exe pins, v11 manifest/container
and role pin. COV-9 `universe.delisting.returns_applied=false`. The companion
contains full hashes/root/bytes; v11 manifest `39736647...`, model `cd48d574...`,
container root `b39a1b77...`. Frozen eval B2-11 is
`e2b48c46055c6aefcda0b992f8c0341f7274793bc30fbb07791f7a58d962c`.
Reuse the completed MV frozen-input evidence at `3c699d40` and scratch26:
all 54 exes, CRT, 126 reference files, v15, both D-COV manifests, cache and
absence checks. No new broad audit or test was needed. Registration will commit
the complete diff/pins/flag binding before any run, then adjacent B.7/Appendix IV
notes and the precise read filter before launch, preserving standing text.

## Scaled mechanics and decision

G_P = 0.986241893721804 is the existing H reader's
`/navs/cell/mean_gross_leverage_all_rows`, not native scenario exposure mean.
Its file/hash and underlying daily hash are in the companion. L_P = 1.1674;
r = 2/L_P = 1.7132088401576153. Require all six values finite:

| Registered measure | Formula and exact limit |
|---|---|
| Mean gross, all rows | [G_P/L_P-.005, 2*G_P/L_P+.005] = [0.8398191654289909, 1.6946383308579818] |
| Absolute mean net, all rows | <= .02*r = 0.03426417680315231 |
| Mean tau_GMV | <= .20*r = 0.3426417680315231 |
| p95 tau_GMV | <= .30*r = 0.5139626520472845 |
| Maximum return identity error | <= 1e-9 |
| Maximum cash/book relative error | <= 1e-9 |

Own X verdict: net annual return X > H AND net Sharpe X >= H-.100 AND all
six scaled mechanics. Paired statistics are nondeciding. C.11/B2-13 applies:
if 1-rho < 1e-6, paired SE/t/p are n/a; the nonpaired decision is unchanged.
Boundary/inconsistent encoding stops for PM; no new inequality after a read.

OD5-7/C3-4's 2.0 cap concerns realised gross exposure. Native
`summary.json /scenarios/*/exposure/max_gross_leverage` and
`mean_gross_leverage` come from actual book exposure (`nav_summarize.cpp`,
`nav_write.cpp`), separate from all-rows mechanics. Check these exact fields for
the five main scenarios, identifying primary `modeled-1bn-stale5-v1+swap-fin-v1`.
`vol_target_leverage` clips L_t to [1,2]; this parameter clip alone is not proof
that realised gross is capped. The partial-aim implementation does not itself
establish that equivalence. Report any observed cap breach or schema conflict
to PM before return reads; do not silently turn it into a seventh decision test
or loosen the cap. This interpretation is proposed prospectively for PM ruling.

## Run and source boundary

After exact PM release, invoke the companion's cycle command once:
`Python312 -u scripts/research_cycle.py run scripts/specs/p9/templates/p9x-leverage.json --stop-after nav`.
The supported stop occurs before final write_verdict; summ copies the ledger,
so monitor/summ completion requires a later PM release after DERIV agrees.
Reuse checks must hold for completed H phases. NAV caps 180s/1536MiB, floor
512MiB; each registered reader uses the same caps with driver `{}`. Immediate
R15-RAM-NOW admission, C: >=15GB; no wait/host-budget options, automatic retry,
build/tests/index, extra diagnostic, calibration or alternative. Actual stops
are reported to PM. Record outer wall time and actual receipt time/peak/caps;
separate reused work and award no G-P1 without evidence.

Own raw console: `cells/logs/p9x-cycle-raw.log`. Own NAV receipt directory:
`build-equity/mega-nav-p9x-leverage-run`. Reader root:
`build-equity/p9x-leverage-readers`, with `mechanics-run1`, `pair-run1`,
`book-run1`, and `mechanics.json`, `pair.json`, `book.json`. Save reader raw
consoles as `cells/logs/p9x-{mechanics,pair,book}-raw.log`. Preserve raw output
unread. Metadata filter prints only stage/status/path/elapsed/resource values.

Proposed exact direct-hand source allowance, requiring PM ruling: scan each
named own raw console plus `stdout.log`, `stderr.log`, `start.json`,
`receipt.json` in those four named receipt directories, after each producer.
Use SEAL-HAND's forms/seeds/seal-reference scan and per-hit report, with these
additional strict predicates over parsed own receipt JSON (and an identical
receipt block if echoed into its associated raw console): own `started_utc`
on 2026-10-17 only; exact five owner filenames only as string entries in
`dirty_outside_pathspec`. The closed filename set is:

- `docs/plans/2026-10-02-x5-equity-curve.png`
- `docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png`
- `docs/plans/2026-10-03-wq-explore-equity-curve.png`
- `docs/plans/2026-10-03-wq-explore-sharpe-vs-turnover.png`
- `docs/plans/2026-10-06-p9-f0-yf0-equity-curve.png`

Match only the token span within that exact field value. A filename elsewhere,
different day, other UTC field, unexpected file or any nonmatching hit stops
before result reads. Existing hand_seal.py is a first classification, not enough
alone because its A2/B predicates are broader. No general date exemption,
wave-console class D extension, M exact-file carryover or wave checker change.
Owner files are name-only: never open/hash/stage/move/delete. Extra reused-phase
receipt/log paths are covered only by their existing verified audit; unexpected
new phase activity stops. Never import sealsrc.py. No unsupported --seal-allow
is passed to the v1 cycle. PM must approve these precise predicates before run.

## Ordered read and independent derivation

1. Own source classification, freeze/input/source/pin/void checks, ledger and
   changed-path scope first. Preserve ledger 138 / SHA-256
   `fe1bc97ecf9ccef7f042d637730581c3410792798ef2ab0a4a7a7fa2bfbc05f8`.
   Run the exact bounded mechanics command; scan its own outputs before the
   six numeric mechanics values and specified exposure/L_t-cap metadata.
   Derive the scaled checks independently; do not use the reader's unscaled
   generic pass flag. Commit own mechanics derivation before any return reads.
2. Run the companion's paired bundle command once, H then X, pinned eval/v8.
   It may compute wider outputs; the agent reads only the registered projection.
   Source scan and pin checks precede `/paired/{sessions,rho,one_minus_rho,degenerate}`.
   Require integer sessions >=2, finite rho in [-1,1], compatible optional
   degeneracy encoding per C.11/R15-V2-RHO. Missing optional fields are documented
   as absent/inferred FALSE only under the established nondegenerate encoding.
   Bind the exact bundle hash, H/X daily and summary hashes, receipt/argv/source
   and frozen eval to `cells/logs/p9x-rho-first.json`; persist and commit that
   rho-first evidence before paired scalar reads. No second rho_pairs computation.
3. Then run the exact bounded book reader; source/pins first. Read only
   `/navs/{parent,cell,yf0}/{net_annual,net_sharpe,x4_net_sharpe,ann_vol,max_drawdown}`
   plus identification/hash metadata. Paired print allowlist is `/paired/dsr`,
   `/paired/lw/p_value`, `/paired/lw/p_one_sided`; SE/t are n/a under the fallback,
   with no broad lw/cbb objects or confidence intervals. Compute only the
   registered X-10 comparisons. No M/C variants or old-Y1 agreement read.
4. Mean L_t uses X's registered `vol_target.csv`, primary-book rows only, exact
   header `L_t`, arithmetic mean; require finite values in [1,2], identify
   book/row count and hash. `strategy_risk_target.cpp` is the producer. H/Y-F0
   fixed leverage parameters are labelled constants, not observed exposure
   or fabricated vol_target rows. No extra volatility/clip statistics.
5. Commit own derivation/record, source and ordered evidence, actual timings,
   input/output hash list and handoff. Stop for fresh independent DERIV. No hand
   trial write, monitor/summ, V or other next cell until explicit PM release.

DERIV's proposed exact read set: this plan/companion, own six-mechanics evidence,
the three named reader JSON files, `cells/logs/p9x-rho-first.json` (explicitly
outside NAV dirs), the four named receipt directories' metadata/source reports,
and H/X/Y-F0 `summary.json`, primary `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`
and `capacity_curve.csv`; X `vol_target.csv` for the registered mean L_t only.
Hashes are available for all named files. DERIV receives its own PM dispatch,
does not read root's verdict or a trial line, and independently derives X-10.
The H/Y-F0 NAV paths and complete commands are in the companion. Unlisted
scientific fields/files are not authorized by inclusion of their parent folder.

Appendix A carries unchanged pre-X counts: N_c67, historical admissions61/P9+0,
N_hand128, M110, N_tot238; eight historical window reruns add0. X is a prospective
new trial, not yet entered. Preserve historical exposure disclosures from the
MV record; no new hidden-session read, null imputation or exposure waiver.

Identity-bridge flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.
Absolute Sharpe / return figures are possibly overstated by survivorship;
paired relative tests carry the shared identity-bridge limitation.
