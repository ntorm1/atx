"""ABS C++ teeth (TEST-1: one teeth build, every refusal disabled at once; TEETH-RESTORE).

One run: assert a clean tree; keep the pre-mutation bytes of the mutated files; apply every
mutation (each anchor must match exactly once); build Debug tag p9-abs-b through
scripts/research-build.ps1 (targets atx-equity-strategy-ic, atx-impl-strategy-ic-tests); run the
AbsentMembers gtests on the mutated binary; then, whatever happened, write the kept bytes back and
assert `git status --porcelain` is empty and every mutated file's `git hash-object` equals HEAD's
blob. Never git checkout / restore / stash. The binaries this leaves in build-equity/bin are teeth
binaries: never evidence (EXE-CUR).

Mutations: R1..R14 disable the refusals of contract section 5 (strategy_ic_absent.cpp, and
dispatch_ic's repeated-flag refusal); S1 removes the IC-result cache bypass of a listed member
(strategy_ic_runner.cpp), which SharedCacheNeverCrossServesRunsWithAndWithoutTheList must catch.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-28")
LOGS = ROOT / ".superpowers/sdd/platform-p9-20261003/abs-logs"
TAG = "p9-abs-c"
TARGETS = "atx-equity-strategy-ic,atx-impl-strategy-ic-tests"
ABSENT = "atx-impl/src/strategy_ic_absent.cpp"
RUNNER = "atx-impl/src/strategy_ic_runner.cpp"
CRLF, LF = "\r\n", "\n"

MUTATIONS = {
    ABSENT: [
        ("R1 pair", "  if (path != pin)\n", "  if (false && path != pin)\n"),
        ("R2 no-composition", "  if (cfg.no_composition)\n", "  if (false && cfg.no_composition)\n"),
        ("R3 pin hex", "  if (!ic_detail::hash_valid(cfg.absent_members_sha256))\n",
         "  if (false && !ic_detail::hash_valid(cfg.absent_members_sha256))\n"),
        ("R4 file read", "  ATX_TRY(const auto text, ic_detail::metadata_text(cfg.absent_members_path));\n",
         "  const auto text = ic_detail::metadata_text(cfg.absent_members_path).value_or(std::string{});\n"),
        ("R5 sha mismatch", "  if (actual != cfg.absent_members_sha256)\n",
         "  if (false && actual != cfg.absent_members_sha256)\n"),
        ("R6 not JSON", "  if (doc.is_discarded()) return refuse(",
         "  if (false && doc.is_discarded()) return refuse("),
        ("R7 not an object", "  if (!doc.is_object()) return refuse(",
         "  if (false && !doc.is_object()) return refuse("),
        ("R8a schema", "  if (schema == doc.end() || !schema->is_string() ||\n",
         "  if (false && (schema == doc.end() || !schema->is_string() ||\n"),
        ("R8b schema", "      schema->get_ref<const std::string&>() != absent_members_schema)\n",
         "      schema->get_ref<const std::string&>() != absent_members_schema))\n"),
        ("R9 members array", "    return refuse(\"members must be a non-empty array of candidate ids\");\n",
         "    return co::Ok(IcAbsentMembers{sha256, {}, std::vector<atx::u8>(lib.candidates.size(), 0)});\n"),
        ("R10 member string", "    if (!entry.is_string()) return refuse(\"member must be a string\");\n",
         "    if (!entry.is_string()) continue;\n"),
        ("R11 member id", "    if (!member_id(id)) return refuse(\"member id must match [A-Za-z0-9_]+: \" + id);\n",
         "    if (!member_id(id)) continue;\n"),
        ("R12 not a candidate",
         "    if (at == lib.candidates.end()) return refuse(\"member is not a candidate of --library: \" + id);\n",
         "    if (at == lib.candidates.end()) continue;\n"),
        ("R13 listed twice", "    if (listed != 0) return refuse(\"member listed twice: \" + id);\n",
         "    if (listed != 0) continue;\n"),
    ],
    RUNNER: [
        ("R14 CLI repeated flag", "        if (std::exchange(absent_given[pin?1U:0U],true))\n",
         "        if (false && std::exchange(absent_given[pin?1U:0U],true))\n"),
        ("S1 IC cache bypass", "    const bool ic_cached=ic_scope.has_value() && !listed;\n"
                               "    const bool legacy_cached=legacy_scope.has_value() && !listed;\n",
         "    const bool ic_cached=ic_scope.has_value();\n"
         "    const bool legacy_cached=legacy_scope.has_value();\n"),
    ],
}


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True,
                          text=True).stdout


def clean_and_at_head(paths) -> list[str]:
    problems = []
    status = git("status", "--porcelain")
    if status.strip():
        problems.append("git status --porcelain not empty:\n" + status)
    for rel in paths:
        work = git("hash-object", rel).strip()
        head = git("rev-parse", f"HEAD:{rel}").strip()
        if work != head:
            problems.append(f"{rel}: working blob {work} != HEAD blob {head}")
    return problems


def main() -> int:
    log = open(LOGS / f"teeth-{TAG}.log", "w", encoding="utf-8")

    def say(text: str) -> None:
        print(text)
        log.write(text + "\n")
        log.flush()

    before = clean_and_at_head(MUTATIONS)
    if before:
        say("REFUSED: tree not clean before mutation\n" + "\n".join(before))
        return 2
    kept = {rel: (ROOT / rel).read_bytes() for rel in MUTATIONS}
    code = 1
    try:
        for rel, edits in MUTATIONS.items():
            raw = kept[rel].decode("utf-8")
            crlf = raw.count(CRLF) == raw.count(LF)  # a checked-out file; a new one is LF
            assert crlf or CRLF not in raw, rel
            text = raw.replace(CRLF, LF)
            for name, old, new in edits:
                count = text.count(old)
                assert count == 1, (rel, name, count)
                text = text.replace(old, new)
                say(f"mutated {rel}: {name}")
            (ROOT / rel).write_bytes((text.replace(LF, CRLF) if crlf else text).encode("utf-8"))
        say(f"build {TAG} ...")
        build = subprocess.run(["powershell", "scripts\\research-build.ps1", "-Tag", TAG,
                                "-Targets", TARGETS], cwd=ROOT, capture_output=True, text=True)
        say(build.stdout[-4000:] + build.stderr[-2000:])
        say(f"build exit {build.returncode}")
        if build.returncode == 0:
            exe = ROOT / "build-equity/bin/atx-impl-strategy-ic-tests.exe"
            run = subprocess.run([str(exe), "--gtest_filter=AbsentMembers.*"], cwd=ROOT,
                                 capture_output=True, text=True)
            (LOGS / f"teeth-{TAG}-gtest.log").write_text(run.stdout + run.stderr, encoding="utf-8")
            say(f"gtest exit {run.returncode} (log teeth-{TAG}-gtest.log)")
            code = 0
    finally:
        for rel, data in kept.items():
            (ROOT / rel).write_bytes(data)
        after = clean_and_at_head(MUTATIONS)
        if after:
            say("RESTORE FAILED: STOP, commit nothing\n" + "\n".join(after))
            code = 3
        else:
            say("restored: git status --porcelain empty; every mutated file's blob == HEAD's")
    log.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
