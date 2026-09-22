"""Checkpoint 14 / T7b: the one bounded native `equity-ic` real-data run.

Parent-executed. This script does NOT build, configure or test anything: the
wrapper-driven configure / check / build / ctest sequence is design section 10.1
and is run by hand before this. Here the already-built `atx-impl.exe` is invoked
once, under a working-set monitor, on the frozen 2013 context and the published
baseline directory.

Contract, from the checkpoint-11/12 runner precedent and design section 10.3:
  * only THIS attempt's own output paths are refused when they already exist:
    --attempt N versions every audit file AND the data output directory
    (..._attemptN), so a failed earlier attempt's evidence is kept, not
    overwritten and not in the way (design section 11.9, ruling I-5);
  * the on-disk design note must still hash to the digest the stage embedded, or
    the run is refused before anything is launched (ruling I-2);
  * inputs, sources and the executable are SHA-256 pinned before and after, and
    a run whose pins moved is not accepted;
  * the trial ledger is verified independently here, in Python, before and after
    the run: the stage appends its own pre-registration and completion lines
    (design section 8, T5), so this script asserts the chain gained exactly two
    links rather than writing one itself;
  * the exact oracle is recorded, NOT executed against this run. The comparator
    compares the ENGINE TEST log's CROSS_SECTION_IC_MEASUREMENT lines with the
    oracle document; it has no bearing on real-data numbers. Both are pinned
    here so the receipt can say which oracle version the engine gate used.

Nothing in this file claims investment quality. A 189-observation information
coefficient is sign-and-shape evidence only.

  python build-equity/audits/iteration14_run_equity_ic.py --help
  python build-equity/audits/iteration14_run_equity_ic.py --selftest
  python build-equity/audits/iteration14_run_equity_ic.py --dry-run
  python build-equity/audits/iteration14_run_equity_ic.py
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path("C:/atx/.worktrees/equity-platform")
SHARED = Path("C:/atx")
AUDITS = ROOT / "build-equity/audits"
EXE = ROOT / "build-equity/bin/atx-impl.exe"
CONTEXT = SHARED / "data/tickerhistory_training_native_20260919/context.bin"
BASELINE = SHARED / "data/equity_baseline_training_2013_20260919"
SOURCE_AUDIT = SHARED / "data/equity_source_reconciliation_2013_20260919/manifest.json"
OUTPUT_STEM = "equity_ic_training_2013_20260920"
LEDGER = ROOT / "atx-engine/reviews/trial-ledger.jsonl"
LEDGER_SIDECAR = ROOT / "atx-engine/reviews/trial-ledger.manifest.json"
DESIGN_NOTE = ROOT / "atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md"
# Design section 11.9, ruling I-2: the stage embeds this digest as
# kDesignNoteSha256 and never reads the note. This preflight is the other half of
# that binding -- it refuses the run unless the on-disk note still hashes to the
# embedded value, so the pre-registration provably binds to the revision that
# froze it. A later edit to the design breaks this check BY DESIGN: a changed
# design is a new freeze and the constant must be re-embedded deliberately.
DESIGN_NOTE_SHA256 = "888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25"
ORACLE_V2 = AUDITS / "iteration14-cross-section-oracle-v2.json"
ORACLE_V1 = AUDITS / "iteration14-cross-section-oracle.json"
COMPARATOR = AUDITS / "iteration14_native_comparator.py"

EVALUATION_START = "2013-04-04"
EVALUATION_END = "2014-01-01"
LIMIT = 3_000_000_000
SAMPLE_SECONDS = 0.1
GENESIS = "0" * 64
EXPECTED_OBSERVATIONS = 189
EXPECTED_REQUIRED_MARK_IDS = 34
EVIDENCED_IDS = (37648, 35715, 39970)
PCS_ID = 146189

EXPECTED = {
    "context_artifact_id":
        "ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079",
    "context_payload_sha256":
        "c219dc237a58574e4b55df8ee51e9f239ef9d5963903e54aa253bd07eafb85ef",
    "baseline_evaluation_id":
        "57b7c21c9320e98c1dee6c97e5f3b8cde211c5efdd688c2daf6fb28daa3cd475",
    "baseline_combo_id":
        "def31e5ca03dc2329bb05a2526c4470e77028fea060d0c3743634d6b4f86a692",
    "source_audit_id":
        "7dda241005f6f4f4549bba3acfab1e5982aeff0d71c9bc66443fada7d06965ea",
}

SOURCE_NAMES = (
    "atx-impl/src/stage_equity_ic.hpp",
    "atx-impl/src/stage_equity_ic.cpp",
    "atx-impl/src/trial_ledger.hpp",
    "atx-impl/src/trial_ledger.cpp",
    "atx-impl/src/config.hpp",
    "atx-impl/src/config.cpp",
    "atx-impl/src/dispatch.cpp",
    "atx-impl/CMakeLists.txt",
    "atx-engine/include/atx/engine/eval/cross_section_ic.hpp",
    "atx-engine/src/eval/cross_section_ic.cpp",
    "atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md",
)

PUBLISHED = ("request.json", "seal.json", "coverage.csv", "ic.csv", "ic_decay.csv",
             "signal_autocorr.csv", "quantile_spread.csv", "ic_summary.json", "manifest.json")


# ---------------------------------------------------------------------------
#  Pure helpers (all covered by --selftest)
# ---------------------------------------------------------------------------
def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def pin(path: Path) -> dict:
    payload = path.read_bytes()
    return {"path": str(path), "sha256": sha256_bytes(payload), "bytes": len(payload)}


def suffixed(name: str, attempt: int) -> str:
    """Attempt 1 keeps the plain name; a retry versions itself, never overwrites."""
    if attempt <= 1:
        return name
    stem, dot, extension = name.partition(".")
    return f"{stem}-attempt{attempt}{dot}{extension}"


def output_root(attempt: int) -> Path:
    """The DATA output directory, versioned per attempt (design section 11.9, I-5).

    The stage reserves --out before doing any work, so a failed attempt 1 leaves
    that directory on disk carrying .pending and failure.json -- and the handoff
    forbids deleting it. Without a per-attempt directory every retry would die in
    preflight on evidence it is required to keep.
    """
    if attempt <= 1:
        return SHARED / "data" / OUTPUT_STEM
    return SHARED / "data" / f"{OUTPUT_STEM}_attempt{attempt}"


def verify_ledger(ledger: Path, sidecar: Path) -> dict:
    """Independent Python walk of the append-only chain (design section 4.5).

    Each line's prev_sha256 is the SHA-256 of the previous line's bytes INCLUDING
    its LF; the first line carries 64 zeros. The sidecar anchors the tail, which
    the chain alone cannot: deleting the newest lines leaves a consistent chain.
    """
    if not ledger.exists():
        return {"lines": 0, "head_sha256": GENESIS, "anchored": False, "present": False}
    payload = ledger.read_bytes()
    if payload and not payload.endswith(b"\n"):
        raise ValueError("trial ledger has a torn final line")
    lines = payload.split(b"\n")[:-1] if payload else []
    previous = GENESIS
    trials = []
    for index, raw in enumerate(lines):
        if not raw:
            raise ValueError(f"trial ledger line {index} is empty")
        if b"\r" in raw:
            raise ValueError(f"trial ledger line {index} carries a CR")
        entry = json.loads(raw.decode("utf-8"))
        if entry.get("prev_sha256") != previous:
            raise ValueError(f"trial ledger chain breaks at line {index}")
        trials.append({"trial_id": entry.get("trial_id"), "status": entry.get("status"),
                       "checkpoint": entry.get("checkpoint"),
                       "trial_count_declared": entry.get("trial_count_declared"),
                       "outcome": (entry.get("result") or {}).get("outcome"),
                       "line_sha256": sha256_bytes(raw + b"\n")})
        previous = sha256_bytes(raw + b"\n")
    anchored = False
    if sidecar.exists():
        recorded = json.loads(sidecar.read_text(encoding="utf-8"))
        if recorded.get("lines") != len(lines) or recorded.get("head_sha256") != previous:
            raise ValueError("trial ledger sidecar disagrees with the walked chain")
        anchored = True
    elif lines:
        raise ValueError("trial ledger sidecar is missing beside a non-empty ledger")
    return {"lines": len(lines), "head_sha256": previous, "anchored": anchored,
            "present": True, "trials": trials}


def required_mark_ids(audit: dict) -> list:
    """The unique security ids the source-reconciliation audit's gaps name."""
    ids = set()
    for gap in audit.get("gaps", []):
        ids.add(int(gap["required_gap"]["security_id"]))
    return sorted(ids)


def choose_oracle(explicit, v2: Path, v1: Path) -> Path:
    if explicit is not None:
        return Path(explicit)
    return v2 if v2.exists() else v1


# ---------------------------------------------------------------------------
#  Working-set monitor (PROCESS_MEMORY_COUNTERS_EX via psapi)
# ---------------------------------------------------------------------------
class _Counters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]


def read_memory(process) -> dict:
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, process.pid)
    if not handle:
        return {}
    try:
        counters = _Counters()
        counters.cb = ctypes.sizeof(_Counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb):
            return {}
        names = [field[0] for field in _Counters._fields_]
        return {name: int(getattr(counters, name)) for name in names
                if name not in ("cb", "PageFaultCount")}
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


# ---------------------------------------------------------------------------
#  Preconditions the parent must be able to read back from the receipt
# ---------------------------------------------------------------------------
def preflight(oracle: Path, output: Path) -> dict:
    checks = {}
    for label, path in (("executable", EXE), ("context", CONTEXT),
                        ("context_manifest", Path(str(CONTEXT) + ".manifest.json")),
                        ("baseline_evaluation_manifest",
                         BASELINE / "evaluation.bin.manifest.json"),
                        ("baseline_combo_manifest", BASELINE / "combo.bin.manifest.json"),
                        ("source_audit", SOURCE_AUDIT), ("design_note", DESIGN_NOTE),
                        ("oracle", oracle), ("comparator", COMPARATOR)):
        if not path.exists():
            raise FileNotFoundError(f"precondition failed, missing {label}: {path}")
        checks[label] = pin(path)
    # The other half of the embedded-constant binding (section 11.9, I-2).
    if checks["design_note"]["sha256"] != DESIGN_NOTE_SHA256:
        raise ValueError(
            "precondition failed, the design note has changed since the stage embedded its "
            f"digest: on disk {checks['design_note']['sha256']}, embedded {DESIGN_NOTE_SHA256}. "
            "A changed design is a new freeze: re-embed kDesignNoteSha256 in "
            "atx-impl/src/stage_equity_ic.cpp and this constant, rebuild, then re-run.")
    # Only THIS attempt's own output root is refused; earlier attempts are
    # evidence the handoff requires be kept.
    if output.exists():
        raise FileExistsError(f"refusing to overwrite an existing output root: {output}")

    context_manifest = json.loads(
        Path(str(CONTEXT) + ".manifest.json").read_text(encoding="utf-8"))
    identities = {
        "context_artifact_id": context_manifest["artifact_id"],
        "context_payload_sha256": context_manifest["payload"]["sha256"],
        "baseline_evaluation_id": json.loads(
            (BASELINE / "evaluation.bin.manifest.json").read_text(
                encoding="utf-8"))["artifact_id"],
        "baseline_combo_id": json.loads(
            (BASELINE / "combo.bin.manifest.json").read_text(
                encoding="utf-8"))["artifact_id"],
    }
    audit = json.loads(SOURCE_AUDIT.read_text(encoding="utf-8"))
    identities["source_audit_id"] = audit["audit_id"]
    for key, expected in EXPECTED.items():
        if identities[key] != expected:
            raise ValueError(f"precondition failed, {key} is {identities[key]}, expected "
                             f"{expected}")

    ids = required_mark_ids(audit)
    if len(ids) != EXPECTED_REQUIRED_MARK_IDS:
        raise ValueError(f"required-mark audit names {len(ids)} ids, expected "
                         f"{EXPECTED_REQUIRED_MARK_IDS}")
    for member in (*EVIDENCED_IDS, PCS_ID):
        if member not in ids:
            raise ValueError(f"required-mark audit does not name id {member}")

    ledger_before = verify_ledger(LEDGER, LEDGER_SIDECAR)
    return {"files": checks, "identities": identities, "required_mark_ids": ids,
            "ledger_before": ledger_before, "output_root": str(output),
            "design_note_sha256_matches_embedded_constant": True,
            "oracle_scope": "the oracle and comparator bind the ENGINE TEST log, not this "
                            "real-data run; neither is executed here",
            "evaluation_start": EVALUATION_START, "evaluation_end_exclusive": EVALUATION_END,
            "expected_observations": EXPECTED_OBSERVATIONS,
            "working_memory_limit_bytes": LIMIT}


def command(output: Path) -> list:
    return [str(EXE), "equity-ic", "--panel", str(CONTEXT), "--baseline-dir", str(BASELINE),
            "--out", str(output), "--evaluation-start", EVALUATION_START,
            "--evaluation-end", EVALUATION_END, "--max-working-bytes", str(LIMIT),
            "--trial-ledger", str(LEDGER)]


def run_native(args, stdout_log: Path, stderr_log: Path, samples_log: Path) -> dict:
    maxima: dict = {}
    count = 0
    started = time.perf_counter()
    with stdout_log.open("xb") as out, stderr_log.open("xb") as err, \
            samples_log.open("x", encoding="utf-8", newline="\n") as samples:
        process = subprocess.Popen(args, cwd=str(ROOT), stdin=subprocess.DEVNULL,
                                   stdout=out, stderr=err)
        breached = None
        while True:
            sample = read_memory(process)
            count += 1
            for key, value in sample.items():
                maxima[key] = max(maxima.get(key, 0), value)
            sample["elapsed_seconds"] = time.perf_counter() - started
            samples.write(json.dumps(sample, sort_keys=True) + "\n")
            samples.flush()
            code = process.poll()
            breached = {k: v for k, v in maxima.items()
                        if k in ("WorkingSetSize", "PeakWorkingSetSize", "PrivateUsage",
                                 "PagefileUsage", "PeakPagefileUsage") and v > LIMIT}
            if breached:
                if code is None:
                    process.terminate()  # only this Popen-owned child, never another process
                    code = process.wait(timeout=30)
                break
            if code is not None:
                break
            time.sleep(SAMPLE_SECONDS)
    return {"exit_code": code, "wall_seconds": time.perf_counter() - started,
            "sample_count": count, "sampled_os_maxima_bytes": maxima,
            "budget_breach": breached or None}


def output_evidence(output: Path) -> dict:
    files = {name: pin(output / name) for name in PUBLISHED if (output / name).exists()}
    missing = [name for name in PUBLISHED if name not in files]
    manifest_bound = None
    if "manifest.json" in files:
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        recorded = {entry["filename"]: entry["sha256"] for entry in manifest.get("files", [])}
        manifest_bound = all(recorded.get(name) == files[name]["sha256"]
                             for name in recorded if name in files)
        observations = None
        if "ic_summary.json" in files:
            observations = json.loads(
                (output / "ic_summary.json").read_text(encoding="utf-8")).get("observations")
        return {"files": files, "missing": missing, "manifest_files_match": manifest_bound,
                "ic_id": manifest.get("ic_id"),
                "observations": observations,
                "observations_expected": EXPECTED_OBSERVATIONS,
                "observations_match": observations == EXPECTED_OBSERVATIONS,
                "runtime": manifest.get("runtime"),
                "predictions_confirmed": manifest.get("predictions_confirmed"),
                "terminal_evidence": manifest.get("terminal_evidence"),
                "pending_marker_present": (output / ".pending").exists()}
    return {"files": files, "missing": missing, "manifest_files_match": manifest_bound,
            "failure_present": (output / "failure.json").exists(),
            "pending_marker_present": (output / ".pending").exists()}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--attempt", type=int, default=1,
                        help="retry ordinal; >1 versions every output, never overwriting")
    parser.add_argument("--oracle", default=None,
                        help="exact oracle document; defaults to the v2 file when present, "
                             "else v1. Recorded, never executed against this run.")
    parser.add_argument("--dry-run", action="store_true",
                        help="run the preconditions and print them; launch nothing")
    parser.add_argument("--selftest", action="store_true", help="exercise the pure helpers")
    options = parser.parse_args(argv)
    if options.selftest:
        return selftest()

    oracle = choose_oracle(options.oracle, ORACLE_V2, ORACLE_V1)
    output = output_root(options.attempt)
    destination = AUDITS / suffixed("iteration14-equity-ic-measurement.json", options.attempt)
    stdout_log = AUDITS / suffixed("iteration14-ic-run.stdout.log", options.attempt)
    stderr_log = AUDITS / suffixed("iteration14-ic-run.stderr.log", options.attempt)
    samples_log = AUDITS / suffixed("iteration14-ic-memory-samples.jsonl", options.attempt)
    for path in (destination, stdout_log, stderr_log, samples_log):
        if path.exists():
            raise FileExistsError(f"immutable attempt-{options.attempt} output exists: {path}")

    plan = preflight(oracle, output)
    if options.dry_run:
        print(json.dumps({"dry_run": True, "command": command(output),
                          "preflight": plan}, indent=2))
        return 0

    sources = {name: pin(ROOT / name) for name in SOURCE_NAMES}
    sources[str(Path(__file__).resolve())] = pin(Path(__file__).resolve())
    before = {**{entry["path"]: entry for entry in plan["files"].values()}, **sources}

    report = {"schema": "atx-equity-ic-native-measurement-v1", "attempt": options.attempt,
              "started_utc": datetime.now(timezone.utc).isoformat(),
              "command": command(output),
              "command_line": subprocess.list2cmdline(command(output)),
              "output_root": str(output),
              "cwd": str(ROOT), "preflight": plan,
              "oracle": pin(oracle), "comparator": pin(COMPARATOR),
              "oracle_executed_against_this_run": False,
              "monitor": {"api": "GetProcessMemoryInfo/PROCESS_MEMORY_COUNTERS_EX",
                          "sampling_interval_seconds": SAMPLE_SECONDS,
                          "limit_bytes": LIMIT,
                          "sampling_limit": "polling and termination are not an instantaneous "
                                            "hard memory cap"},
              "source_and_input_pins_before": before}
    try:
        report.update(run_native(command(output), stdout_log, stderr_log, samples_log))
    except BaseException as error:  # the evidence is written either way
        report["error"] = f"{type(error).__name__}: {error}"
    # Re-pin by each entry's recorded absolute path, not by the dict key: keys are
    # relative source names, and pin() records the path it was handed, so keying
    # the re-pin off the name flipped "path" absolute -> relative and reported a
    # spurious change on attempt 1 (SHA-256 and byte counts were identical).
    after = {key: pin(Path(entry["path"])) for key, entry in before.items()}
    report["source_and_input_pins_after"] = after
    report["pins_unchanged"] = before == after
    report["ledger_after"] = verify_ledger(LEDGER, LEDGER_SIDECAR)
    gained = report["ledger_after"]["lines"] - plan["ledger_before"]["lines"]
    report["ledger_lines_gained"] = gained
    report["ledger_two_lines_per_run"] = gained == 2
    report["output_evidence"] = output_evidence(output)
    report["logs"] = {"stdout": str(stdout_log), "stderr": str(stderr_log),
                      "samples": str(samples_log)}
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    report["accepted"] = bool(report.get("exit_code") == 0 and report["pins_unchanged"]
                              and report["ledger_two_lines_per_run"]
                              and not report["output_evidence"]["missing"]
                              and report.get("budget_breach") is None)
    report["qualification"] = ("Bounded software measurement of a pre-registered forecast "
                               "statistic on 189 training observations. Sign-and-shape "
                               "evidence only: not accepted alpha, not a Sharpe, not capacity.")
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"accepted": report["accepted"], "exit_code": report.get("exit_code"),
                      "wall_seconds": report.get("wall_seconds"),
                      "ledger_lines_gained": gained,
                      "measurement": str(destination)}, indent=2))
    return 0 if report["accepted"] else 1


# ---------------------------------------------------------------------------
#  --selftest: the pure helpers only. Nothing native, nothing written outside a
#  temporary directory, no repository or data path touched.
# ---------------------------------------------------------------------------
def selftest() -> int:
    assert suffixed("a-run.json", 1) == "a-run.json"
    assert suffixed("a-run.json", 3) == "a-run-attempt3.json"
    assert suffixed("iteration14-ic-run.stdout.log", 2) == \
        "iteration14-ic-run-attempt2.stdout.log"
    assert sha256_bytes(b"") == (
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")

    audit = {"audit_id": "x", "gaps": [{"required_gap": {"security_id": "35715"}},
                                       {"required_gap": {"security_id": "35715"}},
                                       {"required_gap": {"security_id": "146189"}}]}
    assert required_mark_ids(audit) == [35715, 146189]

    with tempfile.TemporaryDirectory() as temporary:
        room = Path(temporary)
        ledger, sidecar = room / "t.jsonl", room / "t.manifest.json"
        assert verify_ledger(ledger, sidecar)["head_sha256"] == GENESIS
        first = json.dumps({"prev_sha256": GENESIS, "trial_id": "a", "status": "pre-registered",
                            "checkpoint": 14, "trial_count_declared": 30,
                            "result": {"outcome": "pending"}}, sort_keys=True).encode("utf-8")
        head = sha256_bytes(first + b"\n")
        second = json.dumps({"prev_sha256": head, "trial_id": "a", "status": "completed",
                             "checkpoint": 14, "trial_count_declared": 30,
                             "result": {"outcome": "completed"}},
                            sort_keys=True).encode("utf-8")
        ledger.write_bytes(first + b"\n" + second + b"\n")
        sidecar.write_text(json.dumps({"lines": 2, "head_sha256": sha256_bytes(second + b"\n"),
                                       "created_utc": "2026-09-20T00:00:00Z"}),
                           encoding="utf-8")
        walked = verify_ledger(ledger, sidecar)
        assert walked["lines"] == 2 and walked["anchored"] is True
        assert [t["status"] for t in walked["trials"]] == ["pre-registered", "completed"]

        ledger.write_bytes(first + b"\n" + first + b"\n")  # second link now lies
        try:
            verify_ledger(ledger, sidecar)
            raise AssertionError("a broken chain must not verify")
        except ValueError as error:
            assert "chain breaks at line 1" in str(error)

        ledger.write_bytes(first)  # no trailing LF
        try:
            verify_ledger(ledger, sidecar)
            raise AssertionError("a torn final line must not verify")
        except ValueError as error:
            assert "torn final line" in str(error)

        missing_sidecar = room / "u.manifest.json"
        ledger.write_bytes(first + b"\n")
        try:
            verify_ledger(ledger, missing_sidecar)
            raise AssertionError("an unanchored tail must not verify")
        except ValueError as error:
            assert "sidecar is missing" in str(error)

        present, absent = room / "v2.json", room / "v1.json"
        present.write_text("{}", encoding="utf-8")
        assert choose_oracle(None, present, absent) == present
        assert choose_oracle(None, room / "nope.json", absent) == absent
        assert choose_oracle(str(absent), present, absent) == absent

    # Design section 11.9, I-5: attempt 1 keeps the plain data directory; every
    # retry gets its own, so a failed attempt's evidence is never overwritten.
    assert output_root(1).name == OUTPUT_STEM
    assert output_root(2).name == OUTPUT_STEM + "_attempt2"
    assert output_root(3) != output_root(2) != output_root(1)
    assert output_root(1).parent == output_root(4).parent
    assert command(output_root(2))[command(output_root(2)).index("--out") + 1] ==         str(output_root(2))
    assert "--trial-ledger" in command(output_root(2))
    # Ruling I-2: the embedded digest is a 64-hex constant, and the preflight
    # compares the on-disk note against exactly it.
    assert len(DESIGN_NOTE_SHA256) == 64
    assert all(c in "0123456789abcdef" for c in DESIGN_NOTE_SHA256)
    print(json.dumps({"selftest": "passed", "checks": 24}))
    return 0


if __name__ == "__main__":
    if os.name != "nt" and "--selftest" not in sys.argv and "--help" not in sys.argv:
        raise SystemExit("this runner drives a Windows build of atx-impl.exe")
    raise SystemExit(main())
