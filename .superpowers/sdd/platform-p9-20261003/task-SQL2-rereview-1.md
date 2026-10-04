# Lane SQL2 re-review 1 (fix round 1)

## Verdict
**APPROVE.** There are 0 Required findings.
- S1-S6 are all fixed. None of the fixes adds a problem that blocks merge.
- There is 1 new Suggested finding (S7: what the root seal check scans) and 3 notes for root.

## Scope and how the review was done
- Tree: `C:/atx-wt/pool-23`, branch `feat/p9-sql2-20261003`.
- Fix diff: `e835b434..cfe2de87`. The code commit is `bedd090a`; `cfe2de87` adds the report section.
  - 17 files, +683 / -38.
  - Apart from the two sdd docs, every file is in the lane's owned `atx-engine` research-store set.
  - Nothing under `atx-impl/`, `scripts/` or `atx-db/`, and no CMake file, was touched.
- I reviewed from source only, with no build. The lane deleted its object tree, so the gtests and the fixture chain could not
  be re-run here. The evidence I accept from the report:
  - Build `p9-sql2-d`. Receipt `ExitCode 0`, `CompiledTUs 26`.
    - The receipt's `Source` is `1c993ab5` with `DirtyEntries 15`, so this binary was built from the uncommitted fix
      tree, not from the commit `bedd090a`. Root's R2 rebuild settles this.
    - The log holds exactly 7 `warning` lines. All 7 are clang-cl's unused `/MP` argument, and there are no errors. I
      checked this in `build-equity/mega-p9-sql2-d-build.log`.
  - 27/27 gtests.
  - 43 pytest passes under seeds 0 and 1, with no skips.
  - Fixture chain: `d32f7655...4e69` from both a relative and an absolute `--root`.
- I ran one scratch probe of the seal regex (since deleted). The regex is the backstop's own; `SealGuard.HasSealedYearMatchesTheBackstopRegex` pins the C++ to it.

## Per-finding status
| # | status | evidence |
|---|---|---|
| S1 | **fixed** | `shared_literal_problems` (`test_research_store_classes.py:82-108`) requires each glob of a class that shares a literal to end in a literal last segment, with no `*`, `?` or `[`. It also requires those leaf names, lower-cased, to be pairwise disjoint across the classes.<br>`glob_match` (`classify.cpp:158-189`) has only `**`, `*`, `?` and `[...]`: no braces and no escapes. So a path matches such a glob only when its leaf equals that literal segment. Under this rule two sharing classes can never match the same path, which is the invariant the comment claims.<br>The probe set covers all four of my original probes plus case folding, `?`, `[`, a mixed literal + catch-all, and empty globs. `registers` literals stay in scope, as before. |
| S2 | **fixed** | `sealed_root` = `has_sealed_year(utf8_path(normal_root(root)))` (`seal_guard.cpp:120-127`). It is checked in:<br>- `run_catalog` (`catalog.cpp:739`) and `ingest_one` (`catalog.cpp:752`);<br>- the `catalog` CLI verb, before `--rebuild` and the registry read (`store_cli.cpp:266`). So a sealed root can neither delete an old catalog nor create a new one;<br>- the `ingest` CLI verb (`store_cli.cpp:317`).<br>The control in `StoreCli.SealedRootRefusedBeforeAnythingIsRead` (clean tree exits 4 on its broken registry, sealed tree exits 3) proves the ordering. Scope is judged under S7. |
| S3 | **fixed** | `import_legacy_records` refuses a sealed DIR (`cache_index.cpp:226`). A partition or kind directory with a token is skipped unlisted and counted in `sealed` (`cache_index.cpp:81-93`).<br>`children()` admits only `*.json` files, and `legacy_row` rejects a non-64-hex stem before opening it (`:111`). So the only free text below DIR is the partition and kind names, and both are checked. A 64-hex name that happens to contain a year token is a content address, not a seal name (R5), and is still opened. That is correct.<br>The CLI refuses before `create_cache` (`store_cli.cpp:464`). `cache init` without `--import` still creates `index.sqlite` inside a sealed DIR. It opens nothing there, and the test pins this as intended. |
| S4 | **fixed** (behaviour change; see N1, N3) | `detail::claim_key` (`catalog.cpp:49-71`) runs inside the write closure, before the `candidate` upsert (`ingest_spec.cpp:161`) and the `build_receipt` upsert (`ingest_build.cpp:60`). It reads on the same connection inside the segment's transaction, so a second claimant in the same segment sees the first.<br>The refusal names both paths in sorted order, so it reads the same in every walk order. `held == path` keeps re-ingest idempotent, and `ingest_one` resolves the on-disk spelling first (`catalog.cpp:766-767`).<br>I checked every record table's key (`tables_records.hpp`). `candidate.id` and `build_receipt.tag` are the only keys taken from document values; every other key comes from the path. So the general header claim at `catalog.hpp:24-26` is complete. |
| S5 | **fixed** | `files_seen = verified + declared + skipped - unparsed` (`catalog.cpp:380-383`). This is exact, for three reasons:<br>- `skips_` is a map keyed by `path_key`, and `emplace` never overwrites.<br>- `visit` adds the `unparsed` skip and then always reaches `++verified_` (`:604-630`).<br>- No other skip reason is also verified or declared.<br>The meaning is documented at `catalog.hpp:71-73`. |
| S6 | **fixed** | `remove_catalog`'s error now goes through `open_failure` (`store_cli.cpp:273-275`):<br>- a foreign store (InvalidArgument) gives 3;<br>- in use (PermissionDenied) gives 3, unchanged;<br>- `cannot remove` (IoError) gives 4, unchanged.<br>A non-store text file is NOTADB, which maps to IoError and so to 4 in every verb, `digest` included. The test pins `--rebuild` and `digest` to the same code. |

## Extra checks
1. **Duplicate rule extended to build tags: sound.**
   - `build_receipt` is keyed by `tag`, and `build_exe` by (`tag`, `target`). Before the fix, a second receipt with the
     same `Tag` silently deleted and replaced the first one's `build_exe` rows. Which receipt won depended on walk order,
     and the exe SHA -> tag -> source SHA chain broke.
   - So the rule is the same defect, fixed the same way.
   - It is effectively unreachable from current tooling. `research-build.ps1` writes receipts only to `build-equity/`
     (`$receiptDir`, line 69) and refuses a tag whose receipt already exists (lines 11-12).
   - Small wording fix to the report: "a duplicate can only arise across `build-equity/` and `build-equity-rel/`" is not
     quite right. `Tag` is read from the document (`ingest_build.cpp:20`), not from the file name, so a hand-copied or
     renamed `build-equity/mega-X-receipt.json` that still carries `Tag: Y` collides too. A legacy `build-equity-rel/`
     receipt, which no current writer produces, is the other route. Either way the result is a refusal, which is the
     safe outcome.
2. **Root seal check reads the whole absolute path: no misfire on any path used in this sprint (Suggested S7 below).**
   I probed the backstop regex `(^|\D)20(2[4-9]|[3-9]\d)(\D|$)` against this sprint's paths. None of the following matches:
   - `C:/atx`;
   - `C:/atx-wt/pool-N` (including `pool-20`);
   - the checkout fixture path;
   - `.../platform-p9-20261003` and `feat/p9-sql2-20261003`. A compact 8-digit date never matches, because the 4-digit
     run must end at a non-digit;
   - gtest roots `%TEMP%/atx_research_catalog_tests/<Suite>_<Name>/<leaf>`. They are deterministic, and no test or leaf
     name carries a token other than the intended `v8-2024-oos` / `fit-work-2024`;
   - `%TEMP%` itself, which is `C:\Users\natha\AppData\Local\Temp` here.

   `FixtureChain` roots at the committed tree inside the checkout. The Python identity tests use `tempfile` directories
   only for the Python checker, which has no root seal check.

   Legitimate paths that would misfire, all fail-closed with exit 3 and a message naming the root:
   - an ISO dash date in any ancestor (`C:/atx-wt/sprint-2026-10-03`);
   - pytest `tmp_path` once the per-user basetemp counter reaches `pytest-2024`. It is at about 207 here, and no current
     test passes a `tmp_path` root to the C++;
   - a UUID-named directory such as a Claude scratchpad: 0.46% per random UUID in a 200k-sample probe;
   - a Python `tempfile` name: 0.01%;
   - a home, runner or checkout directory with a year in it (`D:/a/_work/2025/...`).

   The gtests also now assume a token-free checkout and `%TEMP%`. `SealGuard.SealedRootNeverOpened` asserts
   `!sealed_root(fixture("tree"))`.

   The sealed data this guard protects lives inside the research tree. Directories above the repository top are
   environment, not data. So yes: the token rule should apply only to the portion at or below the repo root. This is not
   merge-blocking, for three reasons:
   - (a) nothing in this sprint misfires;
   - (b) a misfire can only refuse, never open, so it cannot breach the seal;
   - (c) the whole-path rule is what my own S2 fix text prescribed.
3. **`files_seen` 31 -> 30 follows from S5; the expectation was not fitted to the output.**
   - Counted independently: in `IngestsSyntheticTree`, `artifact` = 27 rows (25 verified + 2 declared) and
     `skipped_path` = 4.
   - One of those skips is `build-equity/fx-nav/extra_nan.json`. `UnparsedJsonCataloguedAsArtifact` shows it is also an
     artifact.
   - Distinct paths = 27 + 3 = 30. The committed-tree chain gives 25 + 0 + 4 - 1 = 28, matching the report's 29 -> 28.
   - Every other count assertion in the test is unchanged.
4. **Catalog digest unchanged: consistent by construction.**
   - `files_seen` lives in `catalog_run`, which is `volatile_table = true` (`tables_core.hpp:26`). So is `skipped_path`
     (`:62`).
   - `claim_key` only reads. S2, S3 and S6 add refusals but change no row. S1 is Python-only, and the `classes.json`
     edit changes only the `description` string, which the classifier does not read.
   - On a tree with no duplicates, every digested row is therefore byte-identical.
   - The report's re-run (`d32f7655...4e69`, both root spellings) agrees with this. I could not re-run it (no binaries).

## New findings
| # | path:line | tag (severity) | problem | fix |
|---|---|---|---|---|
| S7 | `atx-engine/src/research/store/catalog/seal_guard.cpp:120-122`; callers `catalog.cpp:739,752`, `cache_index.cpp:226`, `store_cli.cpp:266,317,464` | Suggested (minor; not blocking) | `sealed_root` scans the whole absolute path, ancestors above the checkout included. A year token in an environment directory therefore refuses every `catalog`, `ingest` and `cache init --import` run: an ISO-dated sprint dir, pytest `tmp_path` at `pytest-2024`..`2099`, about 0.5% of UUID-named directories, or a home or runner directory. The gtests then turn red as well. This fails closed and is loud, and no path in this sprint is affected (extra check 2). | Scan only the part of the root at or below the repository top. Take the nearest ancestor of `normal_root(root)` that holds a `.git` entry (a file in a worktree, a directory in a clone), using a bounded upward walk, and run `has_sealed_year` on the root-relative remainder. When no `.git` ancestor exists, keep the whole-path check. For `cache init --import` (DIR may sit outside any repo), the fallback keeps today's behaviour. At minimum, record the premise in `atx-vol/docs/LEDGER.md`, or in the store's `--help`, so a refusal on a new machine is not misread as a seal hit. |

## Notes for root (not findings)
- **N1. A refusal leaves a partial catalog.** `flush` commits each segment of up to 500 writes in its own `BEGIN
  IMMEDIATE` (`catalog.cpp:290-302`). When the second claimant of an id or tag is refused, the earlier segments stay
  committed and no `catalog_run` row is written.
  - The append-only ledger refusal behaves the same way, and that was accepted earlier.
  - After an exit 3 from a duplicate, do not read `digest` from that store. Investigate, then run `catalog --rebuild`,
    as the error text says.
- **N2. Renames refuse until `--rebuild`.** The catalog never prunes rows for vanished files. Moving a candidate file
  (or a receipt) to a new path in an existing catalog therefore refuses, because the stale row still holds the key. The
  lane documented this.
- **N3. Candidate ids must stay unique across queue dirs.** `wave_queue.py` writes `<queue dir>/<id>.json` and takes
  `--dir D`. If P9 opens a second queue dir (say `scripts/specs/p9/candidates/`) and re-queues an id that already exists
  under `scripts/specs/v8/candidates/`, the whole real-tree catalog run refuses.
  - Today the 15 tracked v8 candidates and 2 fixture candidates have no duplicate leaf names.
  - If cross-wave re-queueing becomes a workflow, `candidate` needs a path key: a schema change for a later lane, not a
    fix here.

## Checked
- [x] The fix diff only. I applied `.agents/cpp/agent.md` §10:
  - every added loop is bounded (directory entries, skip map);
  - no lifetime issues: `claim_key` takes `string_view`s into closure-owned rows;
  - errors return `Status` / `Result`;
  - includes are present (`<algorithm>`, `<utility>` in `catalog.cpp`; `<initializer_list>` in the tests);
  - no added line is longer than 100 columns.
- [x] Exit-code changes match `store_cli.hpp:2-7` and the report's "Notes for root". Each new refusal is
  PermissionDenied, which maps to 3.
- [x] No cross-lane file. Flag-absent identity is unaffected: no `atx-impl/`, `scripts/` or CMake change.
- [x] Scratch: one probe script in the session scratchpad, deleted. I created no file in pool-23 except this one. I
  started no processes.
