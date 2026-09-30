# Task E-25 report: `nav --label-role` (cell B0c)

Lane R45, pool-11, branch `feat/platform-v8-r45-20260929`.

Commits:
- `985fad57`: merge of root (integration 4).
- `3ba92081`: the feature.
- `ec7356a4` (`wip:`): the membership-clearing rule.

Nothing was compiled or run in C++ (lane rule). The Python tests were run on synthetic data and pass.

**The work stopped on an owner stop.** Read "STOPPED HERE" at the end before running B0c on a real role.

## What was built

### The NAV verb: `--label-role MANIFEST --label-role-sha256 SHA`

The two flags go together; either one alone is a usage error, exit 2. The flags live in `NavExecutionOptions::label_role` (`NavLabelRolePin`). The grid, the capacity pass and the holdings stream use the same load.

**What the label role supplies.** The books are MARKED by the label role. This is every read MARK makes:
- the presence test;
- drift and gap P&L;
- the guard;
- the stale carry;
- the K-session write-off, whose exposure is the label-marked dollars;
- reprints.

Code: `Ctx::mark` (a `NavMarkPrices`, the new `NavReplayInput::mark`), read by `mark_flat`, `mark_session` and `realize` in `strategy_nav_replay.cpp`. With the flag off, `mark` is the role's own `close`, `raw_close` and `present` spans, so the reads are the same as before.

**What stays on `--role`.** Everything else:
- the signal;
- membership;
- the fields;
- the construction (tied ranks, hold band, ADV cap, the neutralization session ring);
- borrow tiers;
- the liquidity and ADV windows (volume);
- execution tradability (`present` in `execute_orders`).

**Decision: what a decision at t may read.** No role payload of the label role enters DECIDE or EXECUTE. The imputed return therefore never feeds a signal, a membership, a construction input, a tier, or a liquidity, ADV or participation limit on the same session.

The book's own NAV and drifted weights do include the realized terminal return from T on, as they include every realized return. That is the return series B0c is meant to change. Two consequences:
- Order dollars after T scale with that NAV.
- Under `--rate per-name-v1`, theta reads the book NAV. The winning cell runs the fixed rate.

**Decision: execution prices come from `--role`.** No order fills on a cell that only the label role prices. Such a cell is an imputed price with volume 0, and its cause may be classified up to 30 days after T. Every fill happens where `--role` is present, and there the label close equals `--role`'s bit for bit (checked at load). The fill prices are therefore identical in both roles.

**Effect on a terminated long position.** It realizes r at T. Its exit order stays blocked. It is carried at the label mark and written off K sessions later: one session later than without the label, at `h(1+r)(1+eta)`.

### The refusals

Refusals are by name, with the prefix `nav replay: --label-role refused:`.

**From the two pinned manifests, before any payload is read.** This runs in `admit_and_load`, before the fields and the blend load. A label role is refused when:
- its `score_end_ns` (when declared) is past the research seal (`research_window.hpp`, never hard-coded);
- any manifest key other than `files` and `universe` differs from `--role`'s;
- the file set or any extent differs;
- any file SHA-256 differs, except `close.f64`, `raw_close.f64`, `volume.f64` and `present.u8`. Those four are the payloads `--delisting-returns` patches (prepare_recent_research.py `DELISTING_RETURN_RULE`, task-F-0 report). A difference in `sessions.i64` is refused as "the dates", in `ids.u64` as "the instruments";
- `member.u8` differs, unless the label role declares the delisting clearing (the wip rule, see below);
- the `universe` block is present in one role only, or does not pin the same:
  - id;
  - `base_role.manifest_sha256` and `base_role.member_sha256`;
  - identity bridge;
  - SIC events;
  - delisting stage, when `--role` has one.

**From the payloads.** These checks run after the blend loads and before the marks are used:
- no role session may be at or after the seal (checked before any label payload is opened);
- the label payloads are pinned by SHA and extent;
- presence and price contract;
- every `--role`-present cell must be present at the same close and raw close, bits equal;
- `member & present & close > 0` must equal the blend's membership on every cell.

`validate_nav_input` repeats the marks contract at the library level. The marks are charged against `--max-bytes`: 18 B per cell plus 8 MiB.

### Records

These keys are written only when the flag is on:
- recipe: `label_role {manifest_sha256, rule}`, with the full rule text;
- summary: `label_role {manifest_sha256, label_only_present_cells, label_only_present_cells_scored, basis}`;
- grid manifest: `label_role_sha256`;
- one console line.

`decide` recomputes the recipe without a label role. A labelled run is therefore never a deploy pin: it gets "pin mismatch nav.recipe_sha256".

### Spec side

`scripts/research_cycle.py`:
- `INPUT_KEYS` gains `label_role {dir, path, sha256}`.
- The nav phase only (never `ref`, which is an identity check against the unlabelled parent) appends `--label-role PATH --label-role-sha256 PIN` and adds `--bind PATH` to the runner line.
- A spec without the key produces the same argv as before.

### Tests

In `atx-impl/tests/strategy_live_test.cpp`, gtest target `atx-impl-strategy-target-tests`:
- `NavLabelRole.FlagOffIsByteIdentical`
- `NavLabelRole.SameRoleIsIdentity`
- `NavLabelRole.TerminalReturnReachesThePnlAndNoDecisionInput`
  - (1) Plant at the final session: every row, holding, target and order before it is bit-identical, and mark P&L = off + (h·close_T/close_L − h) bitwise.
  - (2) Plant mid-window: rows before T are identical; EXECUTE at T fills the same dollars; the construction record, members, tier census, tiers and desired weights are identical at every later decision; the write-offs are h at T+4 and the realized value at T+5.
  - (3) A role pair on disk through the verb.
- `NavLabelRole.RefusesMismatchedAxes`

Python: `scripts/tests/test_research_cycle_label_role.py`, 2 tests. Run here together with `test_research_cycle.py` and `test_cycle_e2e.py`: 96 passed, 4 skipped.

## How root verifies

1. Build `atx-impl-strategy-target-tests` and `atx-equity-strategy-targets`, then run `-R "NavLabelRole|NavV6|StrategyLive|HoldBand|AdvHold"`. `NavV6.OrderBasisTargetAndExitRateOneAreBitIdentical` pins the flag-off recipe SHAs.
2. **Flag off.** Run the accepted v7.1 NAV argv (receipt `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247-run`) with the old exe and with the new exe into two new directories. Every file must be byte-identical.
3. **Label equal to `--role`.** Run the same argv plus `--label-role <role manifest> --label-role-sha256 <role sha>`. Every daily and events CSV must be byte-identical. The recipe and summary differ only by `label_role`, and the summary also by `recipe_sha256`.
4. Run the Python tests: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle_label_role.py scripts/tests/test_research_cycle.py`

## B0c

### argv delta

Relative to the winning cell's NAV argv:

```
--warm-start-sessions 60 --label-role build-equity/train-2020-2023-lo{1|3}-dlret/manifest.json --label-role-sha256 <sha of that manifest>
```

`--combined`, `--role` and `--fields` stay the winner's. Spec form: `inputs.label_role {dir, path, sha256}`, and add `--warm-start-sessions 60` to `nav.flags`.

### Building the label roles

Each is the runbook's decision-role command (w0-2-runbook R7 or R8) with the delisting-return flags added. The same `$BASE`, `$R4`/`$V2PIT`, `$FE`/`$FEV`, `$SIC`, `$DL` and `$V1` pins as the decision role must be used, or the manifest check refuses the label role.

**lo1** (R7 plus the delisting stage and returns; `--check-fields` is optional because it is not compared):
```
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo1-dlret-run --bind build-equity/train-2020-2023-base/manifest.json \
  --bind build-equity/identity-bridge-r4-v2/manifest.json --bind $FE/manifest.json --bind $V1/delisting/manifest.json -- \
  "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v1 \
  --base-role build-equity/train-2020-2023-base --base-role-sha256 $BASE \
  --identity-bridge build-equity/identity-bridge-r4-v2 --identity-bridge-sha256 $R4 \
  --sic-events $FE --sic-events-sha256 $FEV \
  --delisting $V1/delisting --delisting-sha256 $DL --delisting-returns $V1/delisting \
  --out build-equity/train-2020-2023-lo1-dlret --memory-mib 1024 --max-seconds 170
```

**lo3** (R8 plus `--delisting-returns`):
```
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/train-2020-2023-lo3-dlret-run --bind build-equity/train-2020-2023-base/manifest.json \
  --bind $V1/export/identity-bridge-v2-pit/manifest.json --bind $V1/fundamentals/manifest.json \
  --bind $V1/delisting/manifest.json -- \
  "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v3 \
  --out build-equity/train-2020-2023-lo3-dlret --base-role build-equity/train-2020-2023-base --base-role-sha256 $BASE \
  --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT \
  --sic-events $V1/fundamentals --sic-events-sha256 $SIC \
  --delisting $V1/delisting --delisting-sha256 $DL --delisting-returns $V1/delisting --memory-mib 1024 --max-seconds 170
```

### Checks before the NAV

- The label manifest must stay under 1 MiB (C++ cap). The lo3 manifest is about 560 KB.
- Its `universe.delisting.applied` gives the cleared-member count.

## Cross-lane edits

- `scripts/research_cycle.py`: `INPUT_KEYS` and `nav_step` only. Lane A2 also edits this file, so a merge touches those two spots.
- `atx-impl/src/strategy_target_replay.cpp` and `_detail.hpp`: new anonymous-namespace helpers and two new `detail` functions. No existing function changed.
- `atx-impl/tests/strategy_live_test.cpp`: an appended block.

## Risks

- **S3 double-counts the terminal loss.** In S3 (adverse, K=1) a label-priced termination realizes r at T and then takes the adverse haircut at T+1. The S2 primary has eta 0.
- **Terminal P&L reaches order sizing.** From T on, the book NAV includes the realized r, so decision-NAV dollars include it. No role input does.
- **The tool's rule may have drifted since the decision role was built.** If `prepare_recent_research.py`'s lo rule changed since the decision role was built, the label role's membership differs and is refused by name. Rebuild both roles with the same code.
- **None of this has been compiled.** The code is written to be clean first time under clang-cl `/W4 /WX`.

## STOPPED HERE (owner stop)

### Done (committed)

- Everything above.
- `ec7356a4` (wip). A real lo role's membership is lagged: the base role sets `member[t]` from t−1 data, so a name that delists at T = L+1 can be a member at T while absent. `--delisting-returns` clears those cells (`members_cleared_on_termination_session`). The label role's `member.u8`, and so `score_member_counts`, then legitimately differ from `--role`'s. Under `3ba92081` that would have refused the real B0c label role on "the membership (member.u8)".
- `ec7356a4` fixes this. The manifest check now admits a different `member.u8` (and then `score_member_counts`) only when the label manifest declares `universe.delisting.returns_applied: true` with `applied.members_cleared_on_termination_session` N > 0 (`declared_cleared`, `member_differs` in `strategy_target_replay.cpp`). Any other membership difference is still refused before any payload.
- The payload stage still checks effective membership against the blend on every cell. That check alone already forces the label's member to equal `--role`'s at every present cell, and to be 0 at label-only cells.

### Not done

1. **An exact payload check in `detail::load_label_role`.**
   - When `member_differs(role, label)`, also load `--role`'s `member.u8` (payload helper, `--role`'s receipts).
   - Require every differing cell to be a label-only present cell with label member 0 and role member 1.
   - Require the count of those cells to equal the declared N.
   - Raise `label_role_cell_bytes` to `2 * sizeof(f64) + 3` for the extra byte per cell.
2. **Text.** The docs, help and recipe still say "sessions, ids and member.u8 are shared". Update them to "member.u8 shared, or differing only by the declared delisting-return clearing (and then score_member_counts)" in these places:
   - `strategy_target_replay_detail.hpp` (check_label_role comment);
   - `strategy_nav_replay.hpp` (execution.label_role paragraph);
   - `label_role_declaration` and the `--help` text in `strategy_nav_replay.cpp`.
3. **A test of the declared-clearing path.**
   - Give `write_artifact` two trailing defaulted parameters: a role-manifest edit, and a role `member.u8` override.
   - Build a decision role whose `member.u8` is 1 at the planted absent cell (the blend's member stays 0), with a universe block on both roles.
   - The label role clears that member and declares N = 1: it runs.
   - Undeclared: refused on "the membership (member.u8)".
   - Declared N = 2: refused at the payload count (after item 1).
4. **Nothing compiled.** Root builds, and fixes any `/W4 /WX` finding.

### Decisions to keep unchanged when finishing

- MARK only from the label role.
- Execution and all decision inputs from `--role`.
- Book NAV includes the realized return.
- Refusal prefix and names as tested.
- Recipe and summary keys only when on.
- The ref phase never takes the label.
