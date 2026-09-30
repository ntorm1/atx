# Task F-0 report: `--delisting` / `--delisting-returns` on linked-operating-v1

Lane F, pool-7, branch `feat/platform-v8-f-20260929`. Python only; synthetic tests only.

## Dependency answer

There is no v3-only dependency. `_delisting` (prepare_recent_research.py) reads three things:

- the pinned delisting stage (`events.parquet`, rule `delisting-rule-r-v1`);
- the base role's axes and payloads (`close`, `raw_close`, `volume`, `present`);
- the restricted member mask `kept`.

Stage rows map to role columns by `security_id` through `role.columns_of`, so the inputs are keyed by instrument. The identity bridge (v2-pit), the class rule and the SIC stage are never read. The universe rule reaches the block only through `kept`, in two per-role read-outs: `kept_member_at_last_session`, and members cleared on the termination session.

## What was built

`atx-engine/tools/prepare_recent_research.py`. The seal constant lines and every rule, limit and reason constant are unchanged.

- `restrict_role` accepts `delisting` / `delisting_sha256` / `delisting_returns` for v1.
  - For v1 the stage is optional. v2 and v3 still require it, with the same refusal.
  - The stage and its pin go together.
  - `delisting_returns` needs the stage.
  - Marking and applying now run on `delisting is not None` instead of on `v2`. The two manifest blocks (`universe.delisting`, `universe.inputs.delisting`) are written when a stage was given.
  - The v1 `rule`, `limits`, `class_status_required` and `reasons` stay the v1 values. The delisting block carries its own `rule`, `return_rule` and `stage_rule`, identical to v3's.
- CLI (`main`):
  - removed the v1 refusal of `--delisting` / `--delisting-returns`;
  - added "`--delisting` and `--delisting-sha256` go together" and "`--delisting-returns` needs `--delisting`". Before, a v1 `--delisting-returns` without `--delisting` was refused only by the removed check; now it gets its own error instead of reaching `delisting_dir(None)`;
  - kept the rule that `--delisting-returns` must name the `--delisting` stage.
- Updated the help text, the `restrict_role` docstring and a module-doc paragraph.

`atx-engine/tools/test_lo1_delisting.py` (new, 3 tests, on the LinkedOperatingV3 fixture):

- `test_lo1_delisting_off_is_byte_identical`:
  - Checks the lo1 flag-off output against goldens computed with the unmodified tool at base commit c560b8c8: the SHA-256 of every payload file, and the SHA-256 of the manifest's **raw bytes**, so key order and formatting count.
  - Only three things are tokenised: the temp directory, the fixture's input pins, and the two `code_identity` values. The goldens were computed twice in two temp directories and matched.
  - Asserts the code block equals the executed files' live `code_identity`, and that no `delisting` key appears.
  - Re-checks the pre-existing W5b v1 golden (`V1_GOLDEN_MANIFEST` / `V1_GOLDEN_MEMBER`).
- `test_lo1_delisting_returns_match_lo3_semantics`:
  - Marking alone (no returns) changes no payload byte and no manifest value except the two added blocks.
  - lo1 and lo3, for marks and for returns: `inputs.delisting` is identical, and the delisting block is identical except for the per-role membership read-outs.
  - The patched `close`, `raw_close`, `volume` and `present` of lo1 and lo3 are byte-identical. `sessions` and `ids` are unchanged.
  - Line 3 realises -0.55 on STOP+1.
  - For both roles, the per-role read-outs are checked against each role's own plain membership: `kept_member_at_last_session`, exactly the cleared (T, line) cells and nothing else, `members_cleared_on_termination_session`, `kept_member_counts` and `score_member_counts`.
  - The v1 restriction fields (`id`, `rule`, `limits`, class rule, `dropped_by_reason`, `base_member_cells`) equal the flag-off role's.
- `test_lo1_delisting_cli_and_refusals`:
  - API refusals: unpinned stage, returns without stage, wrong pin.
  - CLI refusals: `--delisting` without the SHA, returns without the stage, returns naming another stage. None leaves an output directory.
  - A CLI run of v1 with `--delisting --delisting-returns` is byte-identical to the API run.

`atx-engine/tools/test_linked_operating_v2.py` (existing): one CLI case in `test_refusals_and_cli` asserted the old v1 refusal of `--delisting`. It now asserts v1 `--delisting-returns` without `--delisting` is refused, with a comment. The API case `x2` (v1 returns without a stage) still refuses and still matches `--delisting-returns`.

## How root verifies

```
cd atx-engine/tools
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_lo1_delisting.py test_linked_operating_v2.py test_linked_operating_v3.py test_prepare_recent_research.py
```

Result here: 27 passed. That includes the unchanged v1, v2 and v3 goldens: `V1_GOLDEN_*`, `V2_GOLDEN`, `V2_GOLDEN_RETURNS_CLOSE`.

Building the baseline lo1 role with terminal returns reuses the existing lo1 inputs (pins read from the pool-2 manifests, input pins only) and adds lo3's delisting stage:

```
python atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v1
  --base-role build-equity/recent-fast-train-2020-2022-v2 --base-role-sha256 210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
  --identity-bridge build-equity/identity-bridge-r4-v1 --identity-bridge-sha256 ddf9716459a1116b85f713ca9cb788c3db753a6e1fea8eba335ed34320baebaa
  --sic-events build-equity/fundamental-events-v2 --sic-events-sha256 74ed9a50ea686e0b0842ff9b09e78d6653ddeedd0d42f37893873ce269e3dd71
  [--check-fields <the lo1 build's fields dir> --check-fields-sha256 32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8]
  --delisting C:/atx/atx-db/data/alpha_panel/v1/delisting --delisting-sha256 1b1166b61e5a77d8dbe007f2de3261392424fb86a59c1118028862abc264c37f
  --delisting-returns C:/atx/atx-db/data/alpha_panel/v1/delisting
  --out build-equity/<new lo1 role dir>
```

## Deviations, with reasons

1. **"Manifest included" byte identity holds for every byte except the tool's own code identity.**
   - `universe.inputs.code.prepare_recent_research` holds the SHA-256 of the raw bytes, the SHA-256 of the LF bytes and the git blob of the executing source file. Any edit to the file changes those three hex values, by design, in every role it writes.
   - The test therefore compares the raw manifest bytes with those values tokenised, and separately asserts they equal the executed file's identity.
   - The existing lo1 role on disk is not affected: nothing here rebuilds it. A flag-off rebuild with this code would differ from the existing manifest only in those three values, plus the `prepare_research_fields` values if that file has changed since. It would therefore get a new manifest SHA.
2. The error messages for the stage/pin pairing and for returns-without-stage were reworded, because v1 is now allowed. The substrings the existing tests match (`--delisting`, `--delisting-returns`) are kept.

## Cross-lane edits

- `atx-engine/tools/prepare_recent_research.py` (W0E owns the seal constant lines): the seal lines are not touched; all edits are in the module doc, `restrict_role` and `main`. Root merges this with W0-1.
- `atx-engine/tools/test_linked_operating_v2.py`: one CLI refusal case replaced, as described above.

## Open risks

- **A new role SHA needs new downstream bindings.** A lo1 role built with `--delisting-returns` has a new manifest SHA, from the patched payloads, the members cleared on termination sessions and the added blocks. Fields (for example lo1 fields-v9), IC caches and pools bound to the current lo1 SHA will not bind to it; they must be rebuilt, or the baseline cell must bind them another way.
- **Look-ahead if fields are built on the patched role.** `DELISTING_RETURN_RULE` says the imputed return "must not feed a T-dated signal": the cause can be classified up to 30 days after T. Recommendation: build signals and fields on the unpatched existing lo1 role. A marks-only lo1 has payloads and membership byte-identical to it, but its manifest SHA differs. Use the patched role only for labels and NAV.
- The lo1 membership differs from lo3's, so the per-role read-outs differ. Each follows its own role, as tested. The terminations, returns and patched payloads are identical.
