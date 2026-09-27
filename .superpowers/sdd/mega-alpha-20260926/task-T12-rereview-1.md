# Task T12 re-review, fix round 1 (2b00345b..0e94be59)

Package `review-T12-fix1.diff`: 1 commit (`0e94be59`), 2 files (+299 / -52). R = `atx-impl/tools/repair_role_factor_breaks.py` at 0e94be59 (blob `b2e883f0`; base blob `4f5502a2` = the blob recorded in TRAIN role v2's `repair.tool`). T = `atx-impl/tools/test_repair_role_factor_breaks.py` at 0e94be59. The report names the suite (16/16 OK, 2.4 s); not re-run.

### Finding Verdicts

**Important 1: the 50-cell mass threshold's margin is too thin; add a `factor-break-v2` unexplained-step detector for future roles** — ADDRESSED.
- **The v2 predicate matches the repaired class exactly.**
  - `unexplained(s, r)` = `(s < -NOISE | s >= ln 1.25) & ~raw_followed` (R:247-257).
  - Case by case, this is the classifier's REPAIRED predicate (|s| > noise, gap <= 10, not follow, not 0 < s < ln 1.25), including the s == ln 1.25 edge.
  - It is counted on `step` (gap <= 10) at R:290.
  - `crossing_steps` now calls the same `raw_followed` (R:309). That function is arithmetically identical to the old inline `follow`.
- **Detector:** `max(20, ceil(1% x steps))` (R:319-327), declared with a structural baseline argument that uses no role counts (R docstring).
- **v2 post-repair gate:** no unexplained step on a repaired session and no v2 mass session (R:384-388). It holds by construction, because every unexplained step ending at b is a repaired crossing step.
- **Docstring margin claim corrected** (R:42-48: "about 6%, not the 2x first claimed"). The original report §2.1 text stays as written (append-only) and is retracted in the fix section.
- **Fixtures:**
  - a 60-name dividend cluster is a false MASS under v1 and refused at the post-repair gate, and CLEAN with exit 3 under v2 (T:384);
  - a 30-name re-anchoring is CLEAN under v1 and MASS under v2, repaired to 2e-15 (T:406);
  - threshold arithmetic (T:433);
  - v1 and v2 give identical bytes on the main scenario (T:437).
- **Real run (root):** validation is CLEAN under v2 (max 1 unexplained step per session, threshold about 44). TRAIN v1 is MASS-v2 only on 2021-01-04: its 2114 unexplained steps equal v1's repaired count there (no gap-spanning repaired steps) against a threshold of 48, and every other session has at most 1. The margin is now >= 20x on the legitimate side, against 1.06x before.
- **v1 byte identity, requirement met with one caveat.**
  - Every v1 output path is unchanged: `RULE` / `RULE_STATEMENT` / `PARAMETERS`, the `mass_sessions` keys (v2 keys only under `rule == "v2"`, R:559 and R:584), the jump detector and the jump gate.
  - `--rule` defaults to v1 (R:601).
  - T:454 runs `git show 99421a5f:` of the old tool against the new one: payloads and `repair_cells.csv` are byte-identical, and the manifest is equal apart from `repair.tool`. I confirmed `99421a5f:atx-impl/tools/repair_role_factor_breaks.py` = blob `4f5502a2` in pool-2.
  - **Caveat:** TRAIN role v2's exact manifest bytes (`210fff96…`) are reproducible only by running the recorded blob `4f5502a2`, not HEAD, because `repair.tool` records the code identity by design. HEAD `--rule v1` reproduces every payload and the sidecar.

**Minor 1: the `kept_distribution` premise ignores combined later actions** — ADDRESSED. The limit is documented, with the TRAIN separation (repaired increases have k >= 1.95, kept distributions k <= 1.030) and row 35139 named for a vendor `returnFactor` check (R:34-37).

**Minor 2: declared residual, same-day dividend plus artifact repaired as a whole, no fixture** — ADDRESSED as an accepted residual. It still exists. The review requested no change, and the report explains why close/raw cannot separate the two cases.

**Minor 3: docstring inconsistency (ln 1.2 vs "smallest common split 1.25")** — ADDRESSED.
- CELL_STEP is now "below any split or stock distribution of 6:5 (ln 1.2) or more" (R:43-44).
- SPLIT_RATIO no longer claims to be the smallest split.
- The added 6:5 and 5:6 statements are correct under the classifier: 6:5 forward gives s = 0.182 < ln 1.25, so it is kept_distribution; 5:6 consolidation gives r = 0.182 < 0.223, so it is repaired.
- A grep for "smallest common" or "far below any split" finds no remaining text.

**Minor 4: `TypeError` / `AttributeError` escaped as a traceback** — ADDRESSED. They are now caught and exit 2 (R:639). Fixture: `dates: null` is refused with `TypeError` in stderr (T:483).

**Minor 5: `close_basis` kept verbatim** — ADDRESSED as disclosed and listed. `repair.close_basis_note` is unchanged and bound by the role sha; the review requested no change.

### New Breakage in the Fix Diff

No Critical or Important.

- **Minor:** `test_v1_reproduces_the_committed_tool` skips silently when `99421a5f` is absent, for example in a shallow or other checkout (T:460-466). The byte-identity guarantee is then untested there. It is present and passes in pool-2.
- **Checked, no problem:**
  - The scan output format changed: the single `verdict:` line became two, and the CSV column `mass` became `mass_v1`/`mass_v2`. A repo-wide grep found no programmatic consumer of either.
  - With `--expect-sessions` and `--allow-noop`, the tool uses the selected rule's `res["mass"]`, so a v2 repair is gated on v2 sessions.

### Out-of-Scope Observations

- **The v2 detector does not reach the field producer.** `prepare_research_fields.py` (T6 fix 2, P:84-90, P:615) ports the v1 detector and its 50-cell threshold. Its fail-closed binding compares its v1 mass sessions with the role's `repair.mass_sessions` without regard to `repair.rule`. So a future role repaired with `--rule v2` whose break has fewer than 50 jump cells would be refused by the producer. The producer's validation manifest also shows the same 47-cell legitimate peak. This is ledgered as N1 in `task-T6-rereview-2.md`.

### Verdict

**Fix round:** All findings addressed, no new Critical/Important breakage (6 ADDRESSED: 1 Important + 5 Minor; 0 NOT ADDRESSED; 1 new Minor). v1 logic and payload bytes are unchanged. Reproducing role v2's manifest sha `210fff96…` itself requires the recorded blob `4f5502a2` (retrievable at `99421a5f`).
