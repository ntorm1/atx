# Task DLRET report: delisting returns for linked-operating-v1 (Ruling E-39, B0c)

Lane DLRET, pool 4, branch `feat/platform-v8-dlret-20260930` from `fd2ff7a8`.

Commit: `5db4ce53` (tests and fixture). The report is committed separately.

Nothing was compiled (lane rule). No real data was read: every role in this lane is synthetic.

## Finding: the lo1 path already exists

The premise of the brief (runbook R15: "only linked-operating-v2/v3 take `--delisting` / `--delisting-returns`") is
stale. Lane F task F-0 added the path to linked-operating-v1:
- `872b7125`: the code;
- `33742f7a`: the merge into the v8 integration branch;
- report: `task-F-0-report.md`.

At `fd2ff7a8`, `prepare_recent_research.py` does the following for v1:
- `restrict_role` accepts `delisting` / `delisting_sha256` / `delisting_returns`. The stage is optional for v1 and
  required for v2 / v3.
- Marking and applying run through the one `_delisting` function for every universe (`:757-761`). There is no copy.
- The CLI accepts `--delisting --delisting-sha256 --delisting-returns` with `--universe linked-operating-v1`
  (`:977-989`).

So each item of the brief already holds:
- **Returns corrected exactly as v3 does:** the same function, `DELISTING_MARK_RULE` and `DELISTING_RETURN_RULE`.
- **Membership:** the lo1 restriction is unchanged, apart from v3's own clearing of lagged members on the
  termination session (see deviation 2).
- **Every other payload unchanged:** `sessions.i64` and `ids.u64` are copied byte for byte.
- **The manifest declares the build:**
  - `universe.delisting.returns_applied: true`;
  - `universe.delisting.applied {terminations, members_cleared_on_termination_session, skipped}`;
  - the delisting events digest. `universe.inputs.delisting.manifest_sha256` pins the stage manifest, and
    `universe.inputs.delisting.sources[1].sha256` is the SHA-256 of `events.parquet`.

No change to the tool was needed, and none was made: `git diff fd2ff7a8 5db4ce53 -- atx-engine/tools/prepare_recent_research.py`
is empty.

One acceptance item was missing: no test showed that the E-25 check admits an lo1 pair. The existing C++ tests use
hand-written manifests in the lo3 shape, where the decision role carries a marks-only delisting block. A real lo1
decision role has no delisting block at all. This lane adds that proof on the tool's own output.

## What was built

### `atx-engine/tools/test_lo1_delisting.py` (F-0's test file)

- `tokenised_manifest(case, out) -> str`:
  - The tokenising half of F-0's `identity_tokens`, split out. `identity_tokens` now calls it.
  - The token map gains the delisting stage manifest SHA and the `events.parquet` SHA. A flag-off manifest contains
    neither, so `LO1_GOLDEN_MANIFEST` is unchanged; the test passes.
- `test_lo1_label_role_pair_for_the_e25_check` builds two roles on the synthetic LinkedOperatingV3 fixture: lo1
  with no option (the decision role, B0a's shape), and lo1 with `--delisting --delisting-returns` (the label role).
  It checks three things.
  - **Declarations:**
    - the decision role has no `universe.delisting`;
    - the label role has `returns_applied` true;
    - the stage manifest SHA and the `events.parquet` SHA are pinned in `universe.inputs.delisting`;
    - `members_cleared_on_termination_session` is greater than 0, so the pair exercises the declared-clearing path.
  - **Payloads**, against the rule that `nav --label-role` applies at load (`load_label_role`,
    `check_declared_clearing`):
    - sessions and ids are byte-identical;
    - every cell present in the decision role is present in the label role, with close and raw close equal bit
      for bit;
    - presence is added only on the applied termination cells;
    - `member & present & close > 0` is equal in both roles;
    - `member.u8` differs on exactly N cells, the declared count. Each such cell is (decision member 1, decision
      present 0, label member 0, label present 1).
  - **Manifests:** both tokenised manifests equal the committed fixture byte for byte (CRLF normalised).
    `ATX_WRITE_LO1_LABEL_ROLE_PAIR=1` rewrites the fixture after an intended change to the tool's manifest.

### `atx-impl/tests/fixtures/lo1_label_role_pair/{decision,label}/manifest.json` (new)

The tool's two manifests, about 21 KB and 26 KB. The temporary directory, input pins and code identities are
replaced by tokens. The C++ rule compares those values only for equality. The payload SHAs, axes, counts and rule
texts are the tool's real values.

### `atx-impl/tests/strategy_target_replay_test.cpp` (appended block)

Helpers `lo1_pair_manifest` and `with_role`, plus three tests:
- `NavLabelRoleLo1.AdmitsTheToolsDelistingReturnsPair`: `detail::check_label_role(decision, label)` is Ok. The test
  also asserts the fixture's declarations: id lo1, `returns_applied` true, N > 0, and different `member.u8` files.
- `NavLabelRoleLo1.RefusesTheDelistingReturnsRoleAsDecisionRole`: with the pair swapped, the review B-3 refusal
  rejects it. The message contains "was built with --delisting-returns" and the label's path.
- `NavLabelRoleLo1.RefusesTheClearingUndeclared`: the tool's label role with N set to 0 is refused with "the
  membership (member.u8) differs from --role's".

## How root verifies

1. Python (this lane, 28 passed):
   ```
   cd atx-engine/tools
   "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_lo1_delisting.py test_linked_operating_v2.py test_linked_operating_v3.py test_prepare_recent_research.py
   ```
2. C++: build `atx-impl-strategy-target-tests`, then run `--gtest_filter=NavLabelRoleLo1.*:NavLabelRole.*`. That is
   3 new tests plus R45's 6. The same source is also globbed into `atx-impl-tests`.
3. **Flag-absent identity:**
   - The tool is untouched: `git diff fd2ff7a8 <merge> -- atx-engine/tools/prepare_recent_research.py` must be empty.
   - `test_lo1_delisting_off_is_byte_identical` passes. It checks the lo1 manifest raw bytes (tokenised) and every
     payload against goldens taken at c560b8c8, before F-0.
   - No production C++ changed. The only C++ change is an appended test block.
4. Brief acceptance mapping:
   - **(a) flag-absent identity:** `test_lo1_delisting_off_is_byte_identical` (F-0).
   - **(b) corrected returns equal v3's on the same synthetic events:** `test_lo1_delisting_returns_match_lo3_semantics`
     (F-0). It checks that the patched `close`, `raw_close`, `volume` and `present` of lo1 and lo3 are byte-identical,
     that the delisting blocks are equal apart from the per-role membership read-outs, and that line 3 realises -0.55.
   - **(c) the E-25 check admits the pair:** `NavLabelRoleLo1.*` on the tool's manifests, plus the payload half in
     `test_lo1_label_role_pair_for_the_e25_check`.

### Building B0c's lo1 label role (when lo1 wins B0b)

This is R7's command with the stage added. It is the same command as `task-E25-report.md` "Building the label roles".
Use the same `$BASE`, `$R4`, `$FE` and `$FEV` pins and the same tool commit as `train-2020-2023-lo1`:

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

Then pass `--label-role build-equity/train-2020-2023-lo1-dlret/manifest.json --label-role-sha256 <its sha>` to the
NAV only, with spec key `inputs.label_role`. Signals, fields and `--role` stay on `train-2020-2023-lo1`.

## Registration (v8-prereg, E-30 precedent)

There is no free parameter. The rule is v3's `DELISTING_RETURN_RULE` verbatim (Shumway imputation from the stage:
M&A and non-common 0, performance -30% for the NYSE family and -55% for Nasdaq). The stage is the atx-db `delisting`
stage, rule `delisting-rule-r-v1`, pinned by `$DL`, the same stage as lo3. Nothing was read from 2020-2023.

## Deviations, with reasons

1. **No tool change.** The path the brief asks for has existed since F-0. Adding it again would have copied
   `_delisting`, which the brief forbids. This lane delivers the missing acceptance proof (c) instead.
2. **"Membership unchanged" is read as "the lo1 restriction unchanged".** v3's rule clears a lagged member on the
   termination session (`member[T] = 0`). E-25 needs that clearing: its payload rule requires the label's
   `member & present & close > 0` to equal the decision role's. At T the label role prices the name (present 1),
   and the decision role has it absent. Keeping the member would therefore be refused. The clearing is declared, as
   E-25 requires, and is verified cell by cell.

## Cross-lane edits

- `atx-engine/tools/test_lo1_delisting.py` (lane F's file): one function split into two, two tokens added, one test
  added. F-0's goldens are untouched and still pass.
- `atx-impl/tests/strategy_target_replay_test.cpp`: an appended block. No existing test changed.

## Open risks

- **The stage is not cross-checked for an lo1 pair.** For lo1, E-25 cannot compare the label's delisting stage with
  the decision role's: the decision role has none, and the rule skips the comparison (correctly, by design). The
  stage is pinned only through the label manifest's SHA in the spec. The B0c registration should therefore name
  `$DL`, the same stage as lo3.
- **The membership must match the decision role's.** The label role must be built with the same tool rule and pins
  as `train-2020-2023-lo1`, or E-25 refuses it by count or by cell. The lo1 rule has not changed since F-0, which
  checked flag-off bytes against c560b8c8. `--check-fields` is optional: `fields_crosscheck` is not compared.
- **Runbook R15 is stale.** It says there is no lo1 rule with delisting returns; the command above replaces that
  statement. This is a PM document, so I did not edit it.
- **Nothing was compiled.** The C++ block copies the file's idioms: `read_json`, `write_json`, `Directory`,
  `co::sha256_file`, `st::detail::`. Lines are at most 100 columns, and there are no unused names and no
  conversions.
- **The fixture is a golden.** Any change to the tool's lo1 manifest text, such as a rule string or the seal date,
  fails the Python comparison until it is rewritten with `ATX_WRITE_LO1_LABEL_ROLE_PAIR=1`. The gtests must then be
  re-run on the new bytes.
