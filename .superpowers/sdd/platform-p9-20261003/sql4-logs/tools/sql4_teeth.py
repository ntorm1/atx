"""SQL4 C++ teeth (TEST-1: one teeth build, every refusal disabled at once; TEETH-RESTORE, TEETH-CRLF).

One run: assert a clean tree; keep the pre-mutation bytes (and their raw SHA-256) of the mutated files; apply every
mutation (each anchor, LF-normalised, must match exactly once); build Debug tag p9-sql4-d through
scripts/research-build.ps1 (targets atx-equity-strategy-ic, atx-impl-strategy-ic-tests,
atx-engine-research-catalog-tests); run the lane's named filters on the mutated binaries; then, whatever happened,
write the kept bytes back and assert `git status --porcelain` is empty, every mutated file's `git hash-object`
equals `git rev-parse HEAD:<path>` and the raw SHA-256 of its working bytes equals the kept copy's. Never git
checkout / restore / stash. The binaries this leaves under build-equity/bin are teeth binaries: never evidence.

`--check`: verify that every anchor matches exactly once and the tree is clean; mutate and build nothing.

Mutations (each must turn a named test red):
  M1  dispatch_ic: sqlite without DIR/index.sqlite refused (exit 2)        IcCacheIndex.FlagWithoutIndexRefused
  M2  run_ic: --cache-report refused with sqlite                            IcCacheIndex.FlagWithoutIndexRefused
  M3  dispatch_ic: --candidate-cache-index without --candidate-cache        IcCacheIndex.FlagWithoutIndexRefused
  M4  row_hit: a row whose key parts differ refuses                         IcCacheIndex.KeyMismatchRefuses
  M5  row_hit: a row whose payload differs refuses                          IcCacheIndex.KeyMismatchRefuses
  M6  intact_row: an IC-result row failing its self-hash is never served    IcResultIndex.TamperedRecordIsMiss
  M7  put_pairs: a present pair with other bits refuses                     MarginalPairIndex.ConflictingBitsRefused
  M8  open_indexed: a key row of another key text refuses                   MarginalPairIndex.ConflictingBitsRefused
  M9  dispatch_marginal_ic: sqlite without DIR/index.sqlite refused         MarginalPairIndex.FlagAbsentBlockUnchanged
  M10 verify_ledger_heads: a recorded head outside every prefix refuses     ResearchCatalog.RecordedHeadsArePrefixHeads
  M11 verify_ledger_heads: ledger_state.lines != rows refuses               ResearchCatalog.RecordedHeadsArePrefixHeads
  S1  default_ledger_head: the seam bound to B2's chain_head                ResearchCatalog.LedgerHeadFromResearchLedger,
                                                                            .LedgerHeadSeamReceivesLinesInFileOrder
  S2  run_ic: the index opened by file presence (no flag)                   IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary
"""
import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-32")
LOGS = ROOT / ".superpowers/sdd/platform-p9-20261003/sql4-logs"
TAG = "p9-sql4-d"
TARGETS = "atx-equity-strategy-ic,atx-impl-strategy-ic-tests,atx-engine-research-catalog-tests"
IC_FILTER = "IcCacheIndex.*:IcResultIndex.*:MarginalPairIndex.*:StrategyIcRunner.*:MarginalIc.*"
CATALOG_FILTER = "ResearchCatalog.*"
CRLF, LF = "\r\n", "\n"

RUNNER = "atx-impl/src/strategy_ic_runner.cpp"
SIGNAL = "atx-impl/src/strategy_ic_signal_cache.cpp"
RESULT = "atx-impl/src/strategy_ic_result_cache.cpp"
INDEX = "atx-impl/src/strategy_ic_cache_index.cpp"
PAIRS = "atx-impl/src/strategy_marginal_pair_cache.cpp"
MARGINAL = "atx-impl/src/strategy_marginal_ic.cpp"
LEDGER = "atx-engine/src/research/store/catalog/ingest_ledger.cpp"

MUTATIONS = {
    RUNNER: [
        ("M1 sqlite without index", "    if (cfg.candidate_cache_index==CacheIndexMode::Sqlite &&\n"
                                    "        !cache_index_present(cfg.candidate_cache_directory))\n",
         "    if (false && cfg.candidate_cache_index==CacheIndexMode::Sqlite &&\n"
         "        !cache_index_present(cfg.candidate_cache_directory))\n"),
        ("M2 cache report", "    if (indexed && cfg.cache_report)\n", "    if (false && indexed && cfg.cache_report)\n"),
        ("M3 index without cache", "    if (cache_index_given && cfg.candidate_cache_directory.empty())\n",
         "    if (false && cache_index_given && cfg.candidate_cache_directory.empty())\n"),
        ("S2 open by presence", "    if (indexed) {\n      const auto access=",
         "    if (indexed || cache_index_present(cfg.candidate_cache_directory)) {\n      const auto access="),
    ],
    SIGNAL: [
        ("M4 row key parts", "      row.eval_mode!=key.eval_mode || row.payload_bytes!=payload_bytes(key))\n"
                             "    return mismatch();\n",
         "      row.eval_mode!=key.eval_mode || row.payload_bytes!=payload_bytes(key))\n"
         "    (void)0;\n"),
        ("M5 row payload", "  if (hit.payload_sha!=row.payload_sha256 || payload!=row.payload) return mismatch();\n",
         "  if (false && (hit.payload_sha!=row.payload_sha256 || payload!=row.payload)) return mismatch();\n"),
    ],
    RESULT: [
        ("M6 self-hash", "  if (parsed.defect!=EntryDefect::None || row.key_sha256!=key_sha ||\n",
         "  if ((false && parsed.defect!=EntryDefect::None) || row.key_sha256!=key_sha ||\n"),
    ],
    INDEX: [
        ("M7 pair bits", "      if (!held || held->sum_bits != row.sum_bits || held->dates != row.dates)\n",
         "      if (false && (!held || held->sum_bits != row.sum_bits || held->dates != row.dates))\n"),
    ],
    PAIRS: [
        ("M8 key text", "  if (key->version != static_cast<i64>(cb::kPairwiseRowCorrelationVersion) ||\n",
         "  if (false && key->version != static_cast<i64>(cb::kPairwiseRowCorrelationVersion) &&\n"),
    ],
    MARGINAL: [
        ("M9 sqlite without index", "    if (cfg.pair_cache_index == CacheIndexMode::Sqlite &&\n"
                                    "        !cache_index_present(cfg.pair_cache_directory))\n",
         "    if (false && cfg.pair_cache_index == CacheIndexMode::Sqlite &&\n"
         "        !cache_index_present(cfg.pair_cache_directory))\n"),
    ],
    LEDGER: [
        ("M10 prefix head", "      if (at != prefixes[i].end()) {\n", "      if (true) {\n"),
        ("M11 state lines", "  if (stored_lines != lines) {\n", "  if (false && stored_lines != lines) {\n"),
        ("S1 seam", "LedgerHeadFn default_ledger_head() {\n  return [](",
         "LedgerHeadFn default_ledger_head() {\n  if (true) return {};\n  return [](")
    ],
}


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True,
                          text=True).stdout


def clean_and_at_head(paths, kept=None) -> list[str]:
    """TEETH-CRLF: hash-object (clean filter) vs HEAD's blob; raw SHA-256 vs the kept copy's."""
    problems = []
    status = git("status", "--porcelain")
    if status.strip():
        problems.append("git status --porcelain not empty:\n" + status)
    for rel in paths:
        work = git("hash-object", rel).strip()
        head = git("rev-parse", f"HEAD:{rel}").strip()
        if work != head:
            problems.append(f"{rel}: working blob {work} != HEAD blob {head}")
        if kept is not None:
            raw = hashlib.sha256((ROOT / rel).read_bytes()).hexdigest()
            if raw != hashlib.sha256(kept[rel]).hexdigest():
                problems.append(f"{rel}: working bytes differ from the kept copy")
    return problems


def mutated_text(raw: str, rel: str, edits, say) -> str:
    crlf = raw.count(CRLF) == raw.count(LF)  # a checked-out file (CRLF) or a new LF one
    assert crlf or CRLF not in raw, rel
    text = raw.replace(CRLF, LF)
    for name, old, new in edits:
        count = text.count(old)
        assert count == 1, (rel, name, count)
        text = text.replace(old, new)
        say(f"anchor ok {rel}: {name}")
    return text.replace(LF, CRLF) if crlf else text


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    log = open(LOGS / (f"teeth-{TAG}-check.log" if check_only else f"teeth-{TAG}.log"), "w",
               encoding="utf-8")

    def say(text: str) -> None:
        print(text)
        log.write(text + "\n")
        log.flush()

    before = clean_and_at_head(MUTATIONS)
    if before:
        say("REFUSED: tree not clean before mutation\n" + "\n".join(before))
        return 2
    kept = {rel: (ROOT / rel).read_bytes() for rel in MUTATIONS}
    if check_only:
        for rel, edits in MUTATIONS.items():
            mutated_text(kept[rel].decode("utf-8"), rel, edits, say)
        say("check only: nothing written")
        log.close()
        return 0
    code = 1
    try:
        for rel, edits in MUTATIONS.items():
            (ROOT / rel).write_bytes(mutated_text(kept[rel].decode("utf-8"), rel, edits, say).encode("utf-8"))
            say(f"mutated {rel}")
        say(f"build {TAG} ...")
        build = subprocess.run(["powershell", "-NoProfile", "-File", "scripts\\research-build.ps1", "-Tag", TAG,
                                "-Targets", TARGETS], cwd=ROOT, capture_output=True, text=True)
        say(build.stdout[-4000:] + build.stderr[-2000:])
        say(f"build exit {build.returncode}")
        if build.returncode == 0:
            for exe_name, flt in (("atx-impl-strategy-ic-tests", IC_FILTER),
                                  ("atx-engine-research-catalog-tests", CATALOG_FILTER)):
                exe = ROOT / "build-equity/bin" / f"{exe_name}.exe"
                run = subprocess.run([str(exe), f"--gtest_filter={flt}", "--gtest_brief=1"], cwd=ROOT,
                                     capture_output=True, text=True)
                out = LOGS / f"teeth-{TAG}-{exe_name}.log"
                out.write_text(run.stdout + run.stderr, encoding="utf-8")
                failed = [line.strip() for line in (run.stdout + run.stderr).splitlines()
                          if line.startswith("[  FAILED  ]") and "," not in line and "listed below" not in line]
                say(f"{exe_name}: gtest exit {run.returncode}; failed {len(failed)}: " + "; ".join(failed))
            code = 0
    finally:
        for rel, data in kept.items():
            (ROOT / rel).write_bytes(data)
        after = clean_and_at_head(MUTATIONS, kept)
        if after:
            say("RESTORE FAILED: STOP, commit nothing\n" + "\n".join(after))
            code = 3
        else:
            say("restored: git status --porcelain empty; every mutated file's hash-object == HEAD's blob and its "
                "raw SHA-256 == the kept copy's")
    log.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
