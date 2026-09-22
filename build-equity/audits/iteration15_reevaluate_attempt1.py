"""Re-evaluate the checkpoint-15 attempt-1 `equity-universe` run's acceptance WITHOUT re-running.

What happened (runner defect, cp15 attempt 1)
---------------------------------------------
`iteration15_run_equity_universe.py` launched the stage once; the stage SUCCEEDED
(`C:/atx/data/equity_universe_pit_2013_2019_20260920`, manifest status `complete`, 84
rebalances, trial-ledger lines 3-4 `pre-registered` -> `completed`). The runner then crashed
AFTER the native run at its post-run re-pin loop (`iteration15_run_equity_universe.py:768`,
`TypeError: string indices must be integers, not 'str'`): the oracle-lineage acceptance note had
been stored as a STRING inside `checks` (the pin map that becomes `source_and_input_pins_before`),
and the loop indexed every entry as a pin dict. The in-memory `run_native` result (exit code,
wall time, maxima) was never written; the memory-samples and stdout/stderr logs WERE written.

A re-run would append two more lines to the tracked trial ledger for no new information, so the
acceptance is re-derived here from what is on disk, mirroring cp14's
`iteration14_reevaluate_attempt1.py`. Nothing is launched; every path is read-only except the
single output file, opened with mode "x".

Method
------
The runner module is imported (not executed) so the SAME predicates decide: `preflight`
(design digest, stage-embedded constant, executable subcommand string, seven segment
directories with manifests / seal / order / counts, positional preparation-manifest binding,
ledger walk, no stale lock), `output_evidence` (10 published files, manifest digests,
`membership.bin` header decode, `request.json` rebalance count), `ledger_delta_ok` and
`pins_differ`. "Digests unchanged" is evaluated post hoc as: every digest the STAGE recorded at
run time (`producer_executable_sha256`, `design_note.sha256`, `parents[]`) equals the digest of
the same file NOW, and the ingestion manifests' segment digests equal the stage's recorded
segment parents. The per-`.seg` file hash the runner would have taken before/after is optional
here (`--pin-segments`, 2.3 GB) and is reported as not taken otherwise.

The output carries the runner's measurement surface (`atx-equity-universe-native-measurement-v1`
keys, so `iteration15_write_validation_receipt.py --measurement` can read it) plus a
`reevaluation` block and `runner_defect`. `exit_code` is NOT recorded anywhere on disk; it is
INFERRED (see `exit_code_inference`) and labelled as such.

  python build-equity/audits/iteration15_reevaluate_attempt1.py [--pin-segments]
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path("C:/atx/.worktrees/equity-platform")
AUDITS = ROOT / "build-equity/audits"
RUNNER = AUDITS / "iteration15_run_equity_universe.py"
OUT = AUDITS / "iteration15-equity-universe-measurement-attempt1-reeval.json"
ATTEMPT = 1
CRASH_LOG = AUDITS / "iteration15-equity-universe-run.err.log"
CRASH_STDOUT = AUDITS / "iteration15-equity-universe-run.log"
REEVAL_SCHEMA = "atx-equity-universe-measurement-reevaluation-v1"
STAGE_SUMMARY = re.compile(r"\[atx-impl\] stage=equity-universe .*?rebalances=(\d+) sessions=(\d+) "
                           r"source_ids=(\d+)")


def load_runner():
    spec = importlib.util.spec_from_file_location("iteration15_run_equity_universe", RUNNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)      # defines functions/constants only; main() is guarded
    return module


def walk_prefix(ledger: Path, count: int, sha256_bytes) -> dict:
    """Head of the chain after the first `count` lines (the state the runner saw BEFORE the run)."""
    lines = ledger.read_bytes().split(b"\n")[:-1]
    previous = "0" * 64
    trials = []
    for raw in lines[:count]:
        entry = json.loads(raw.decode("utf-8"))
        assert entry.get("prev_sha256") == previous, "chain breaks inside the prefix"
        trials.append({"trial_id": entry.get("trial_id"), "status": entry.get("status"),
                       "checkpoint": entry.get("checkpoint"), "purpose": entry.get("purpose"),
                       "trial_count_declared": entry.get("trial_count_declared"),
                       "line_sha256": sha256_bytes(raw + b"\n")})
        previous = sha256_bytes(raw + b"\n")
    return {"lines": count, "head_sha256": previous, "present": True, "anchored": None,
            "trials": trials, "reconstructed": "prefix walk of the current ledger"}


def parse_samples(path: Path, limit: int) -> dict:
    if not path.exists():
        return {"available": False, "reason": f"samples log missing: {path}"}
    maxima, count, last_elapsed = {}, 0, None
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw.strip():
            continue
        sample = json.loads(raw)
        count += 1
        last_elapsed = sample.get("elapsed_seconds", last_elapsed)
        for key, value in sample.items():
            if key != "elapsed_seconds":
                maxima[key] = max(maxima.get(key, 0), int(value))
    breached = {k: v for k, v in maxima.items()
                if k in ("WorkingSetSize", "PeakWorkingSetSize", "PrivateUsage",
                         "PagefileUsage", "PeakPagefileUsage") and v > limit}
    return {"available": True, "path": str(path), "sample_count": count,
            "sampled_os_maxima_bytes": maxima, "last_sample_elapsed_seconds": last_elapsed,
            "budget_breach": breached or None,
            "note": "wall time is the elapsed stamp of the LAST 100 ms sample (a lower bound on "
                    "the child's lifetime, within one sampling interval); the runner's own "
                    "perf_counter total was lost in the crash"}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pin-segments", action="store_true",
                        help="also SHA-256 every .seg file against its ingestion manifest (2.3 GB)")
    options = parser.parse_args(argv)
    if OUT.exists():
        raise FileExistsError(f"immutable re-evaluation already exists: {OUT}")

    R = load_runner()
    output = R.output_root(ATTEMPT)
    stdout_log = AUDITS / R.suffixed("iteration15-universe-run.stdout.log", ATTEMPT)
    stderr_log = AUDITS / R.suffixed("iteration15-universe-run.stderr.log", ATTEMPT)
    samples_log = AUDITS / R.suffixed("iteration15-universe-memory-samples.jsonl", ATTEMPT)
    reasons = []

    # 1. The runner's own preflight, post hoc. The output root now exists BY DESIGN (the run
    #    succeeded), so exactly that one problem is expected; anything else is a finding.
    plan = R.preflight(output, pin_segments=options.pin_segments)
    expected_problem = f"output root already exists: {output}"
    unexpected = [p for p in plan["problems"] if p != expected_problem]
    if expected_problem not in plan["problems"]:
        reasons.append("output root is missing: the run left no output directory")
    reasons.extend(f"preflight (post hoc): {p}" for p in unexpected)
    plan["problems_expected_post_hoc"] = [expected_problem]
    plan["problems"] = unexpected

    # 2. Current pins of everything the runner pins (sources, exe, note, oracle, comparator,
    #    manifests) -- the "after" side. The "before" side was never written, so the run-time
    #    digests come from what the STAGE itself recorded.
    sources = {name: R.pin(ROOT / name) for name in R.SOURCE_NAMES if (ROOT / name).exists()}
    sources[str(RUNNER)] = R.pin(RUNNER)
    after = {k: v for k, v in {**plan["files"], **sources}.items() if R.is_pin(v)}
    for index, entry in enumerate(plan["segment_dirs"]):
        for key in ("ingestion_manifest", "preparation_manifest_pin"):
            if key in entry:
                after[f"{key}[{index}]"] = entry[key]
        for name, seg in entry.get("segment_pins", {}).items():
            after[f"segments[{index}]/{name}"] = {"path": str(R.SEGMENT_DIRS[index] / name), **seg}

    evidence = R.output_evidence(output)
    manifest = request = None
    if "manifest.json" in evidence["files"]:
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    if "request.json" in evidence["files"]:
        request = json.loads((output / "request.json").read_text(encoding="utf-8"))
    run_time = {"source": "digests the stage recorded in manifest.json / request.json at run time"}
    if manifest is None:
        reasons.append("manifest.json missing: no run-time digests to compare against")
    else:
        parents = manifest.get("parents") or []
        parent_shas = {p.get("sha256") for p in parents if isinstance(p, dict)}
        seg_parents = {p["filename"]: p["sha256"] for p in parents
                       if isinstance(p, dict) and str(p.get("filename", "")).endswith(".seg")}
        run_time["producer_executable_sha256"] = manifest.get("producer_executable_sha256")
        run_time["design_note_sha256"] = (manifest.get("design_note") or {}).get("sha256")
        run_time["parents"] = len(parents)
        run_time["segment_parents"] = len(seg_parents)
        exe_now = after.get("executable", {}).get("sha256")
        if exe_now != run_time["producer_executable_sha256"]:
            reasons.append(f"executable changed since the run: now {exe_now}, stage recorded "
                           f"{run_time['producer_executable_sha256']}")
        note_now = after.get("design_note", {}).get("sha256")
        if note_now != run_time["design_note_sha256"] or note_now != R.DESIGN_NOTE_SHA256:
            reasons.append(f"design note digest now {note_now}, stage recorded "
                           f"{run_time['design_note_sha256']}, frozen {R.DESIGN_NOTE_SHA256}")
        for index, entry in enumerate(plan["segment_dirs"]):
            for key in ("ingestion_manifest", "preparation_manifest_pin"):
                sha = (entry.get(key) or {}).get("sha256")
                if sha and sha not in parent_shas:
                    reasons.append(f"{key}[{index}] digest {sha[:12]}... is not among the stage's "
                                   "recorded parents (changed since the run?)")
        # Segments: the stage hashed every attached .seg at run time (I-12) and the runner's
        # preflight binds each dir to its ingestion manifest -- compare the two digest lists
        # filename-by-filename without reading the 2.3 GB.
        mismatched, compared = [], 0
        for index, directory in enumerate(R.SEGMENT_DIRS):
            ingestion = directory / R.INGESTION_MANIFEST
            if not ingestion.exists():
                continue
            listed = R.manifest_digests(json.loads(ingestion.read_text(encoding="utf-8")).get("segments", []))
            for name, sha in listed.items():
                compared += 1
                if seg_parents.get(name) != sha:
                    mismatched.append(f"{index}/{name}")
        run_time["segments_compared_manifest_vs_stage_parents"] = compared
        run_time["segments_mismatched"] = mismatched[:10]
        if compared != R.EXPECTED_SESSIONS or mismatched:
            reasons.append(f"segment digests: {compared} compared (expected {R.EXPECTED_SESSIONS}), "
                           f"{len(mismatched)} mismatched between ingestion manifests and the stage's parents")
        if options.pin_segments:
            bad = [k for k, v in after.items() if k.startswith("segments[")
                   and seg_parents.get(k.split("/", 1)[1]) != v["sha256"]]
            run_time["segment_files_hashed_now"] = sum(1 for k in after if k.startswith("segments["))
            run_time["segment_files_differing_from_stage_parents"] = bad[:10]
            if bad:
                reasons.append(f"{len(bad)} .seg file(s) hash differently now than the stage recorded")
        else:
            run_time["segment_files_hashed_now"] = 0
            run_time["segment_files_note"] = ("per-file .seg hashes not taken (pass --pin-segments); "
                                              "the manifest-vs-stage-parents digest comparison above "
                                              "covers every attached segment by name")
    pins_changed = []          # by construction: `after` IS the current state; the run-time
    # comparison above is the substantive "unchanged" test, listed in `reasons` when it fails.
    pins_unchanged = not any(r.startswith(("executable changed", "design note digest",
                                           "ingestion_manifest", "preparation_manifest_pin",
                                           "segment digests", ".seg")) or "hash differently" in r
                             for r in reasons)

    # 3. Ledger: chain now, and the prefix the runner saw before the run (lines 1-2).
    ledger_after = R.verify_ledger(R.LEDGER, R.LEDGER_SIDECAR)
    ledger_before = walk_prefix(R.LEDGER, ledger_after["lines"] - 2, R.sha256_bytes) \
        if ledger_after["lines"] >= 2 else {"lines": 0, "trials": [], "head_sha256": R.GENESIS}
    gained, new, ledger_ok = R.ledger_delta_ok(ledger_before, ledger_after)
    if not ledger_ok:
        reasons.append(f"ledger delta is not exactly two cp15 lines (pre-registered -> completed): {new}")
    if manifest is not None:
        recorded = manifest.get("trial_ledger") or {}
        if new and recorded.get("trial_id") != new[0]["trial_id"]:
            reasons.append("manifest.json trial_id differs from the ledger's new lines")
        if new and recorded.get("pre_registration_line_sha256") != new[0]["line_sha256"]:
            reasons.append("manifest.json pre_registration_line_sha256 differs from ledger line 3")
    if not ledger_after.get("anchored"):
        reasons.append("ledger sidecar does not anchor the current head")

    # 4. Outputs (the runner's own predicates).
    if evidence["missing"]:
        reasons.append(f"published files missing: {evidence['missing']}")
    if evidence.get("membership_bin_ok") is not True:
        reasons.append(f"membership.bin acceptance failed: {evidence.get('membership_bin')}")
    if evidence.get("request_rebalances_match") is not True:
        reasons.append(f"request.json does not carry rebalances == {R.EXPECTED_REBALANCES}")
    if evidence.get("manifest_files_match") is not True:
        reasons.append("manifest.json file digests differ from the files on disk")
    if evidence.get("pending_marker_present"):
        reasons.append(".pending marker still present under the output root")
    if manifest is not None and manifest.get("status") != "complete":
        reasons.append(f"manifest status {manifest.get('status')!r} != 'complete'")
    seal = json.loads((output / "seal.json").read_text(encoding="utf-8")) \
        if "seal.json" in evidence["files"] else {}
    if seal.get("segments_refused") != 0 or seal.get("latest_attached") != "2019-12-31" \
            or seal.get("validation_begin") != R.VALIDATION_BEGIN:
        reasons.append(f"seal.json is not the expected sealed statement: {seal}")
    if request is not None and (request.get("rank_session_first") != R.RANK_START
                                or request.get("rank_session_last") != R.RANK_END
                                or request.get("sessions") != R.EXPECTED_SESSIONS):
        reasons.append("request.json rank window / session count differ from the pinned values")

    # 5. Runner logs that DID get written, and the stage's own runtime.
    samples = parse_samples(samples_log, R.LIMIT)
    if not samples.get("available"):
        reasons.append("peak working set unavailable: " + samples.get("reason", ""))
    elif samples.get("budget_breach"):
        reasons.append(f"sampled memory exceeded the budget: {samples['budget_breach']}")
    stdout_text = stdout_log.read_text(encoding="utf-8", errors="replace") if stdout_log.exists() else None
    stderr_text = stderr_log.read_text(encoding="utf-8", errors="replace") if stderr_log.exists() else None
    summary = STAGE_SUMMARY.search(stdout_text or "")
    stage_summary = ({"rebalances": int(summary.group(1)), "sessions": int(summary.group(2)),
                      "source_ids": int(summary.group(3)), "line": summary.group(0)} if summary else None)
    if stage_summary is None or stage_summary["rebalances"] != R.EXPECTED_REBALANCES:
        reasons.append("stage summary line with rebalances=84 not found on the run's stdout")
    if stderr_text:
        reasons.append(f"stage wrote to stderr: {stderr_text[:200]!r}")
    stage_runtime = (manifest or {}).get("runtime") or {}
    runtime_seconds = (manifest or {}).get("runtime_seconds", stage_runtime.get("runtime_seconds"))

    # 6. Exit code: never recorded (the crash discarded run_native's result). Inferred only.
    inference = {
        "recorded": None,
        "manifest_status_complete": (manifest or {}).get("status") == "complete",
        "ledger_terminal_line_completed": bool(new) and new[-1]["status"] == "completed",
        "stage_summary_on_stdout": stage_summary is not None,
        "stderr_empty": stderr_text == "",
        "note": "the stage writes manifest.json last and the ledger 'completed' line after it "
                "(design section 5.5 step 6); every later exit path is the failure branch, which "
                "writes failure.json and a 'failed' line instead. None of those exist.",
    }
    inference["inferred_exit_code"] = 0 if all(inference[k] for k in (
        "manifest_status_complete", "ledger_terminal_line_completed", "stage_summary_on_stdout",
        "stderr_empty")) else None
    if inference["inferred_exit_code"] != 0:
        reasons.append("exit code 0 cannot be inferred from the on-disk evidence")

    accepted = not reasons
    crash = CRASH_LOG.read_text(encoding="utf-8", errors="replace") if CRASH_LOG.exists() else ""
    report = {
        "schema": "atx-equity-universe-native-measurement-v1",
        "measurement_kind": "POST-HOC RE-EVALUATION of attempt 1 (not written by the runner); see "
                            "`reevaluation` and `runner_defect`",
        "reevaluation": {
            "schema": REEVAL_SCHEMA,
            "written_utc": datetime.now(timezone.utc).isoformat(),
            "this_script": R.pin(Path(__file__).resolve()),
            "runner_module_imported": R.pin(RUNNER),
            "accepted_reevaluated": accepted,
            "reasons": reasons,
            "rules_applied": ["runner.preflight (post hoc; only 'output root already exists' expected)",
                              "executable / design note / manifests digests now == stage-recorded run-time digests",
                              "ingestion-manifest segment digests == stage parents, by filename",
                              "ledger: exactly two new cp15 lines pre-registered -> completed, same trial_id, sidecar anchored",
                              "runner.output_evidence: 10 files, manifest digests, membership.bin header (84, "
                              "2012-12-31 .. 2019-11-29, effective 2019-12-02, frozen config, FNV trailer), request.json 84",
                              "seal.json sealed statement; manifest status complete; no .pending; stderr empty",
                              "memory samples below the 3,000,000,000-byte budget",
                              "exit code 0 inferred from the on-disk terminal evidence"],
            "run_time_digests": run_time,
            "exit_code_inference": inference,
            "no_rerun_reason": "a re-run would append two more ledger lines (a new trial_id) for no new "
                               "information; the attempt-1 stage run itself completed and is bound by "
                               "its own manifest and ledger lines",
        },
        "runner_defect": {
            "where": "iteration15_run_equity_universe.py:768 (post-run re-pin loop), attempt 1",
            "error": "TypeError: string indices must be integers, not 'str'",
            "cause": "the oracle-lineage acceptance note was stored as a string in `checks` (the pin "
                     "map that becomes source_and_input_pins_before); the re-pin loop indexed it as a pin",
            "effect": "run_native's in-memory result (exit code, wall seconds, sample count, maxima) was "
                      "discarded; the measurement JSON was never written; the memory-samples and "
                      "stdout/stderr logs were written; the stage run itself was unaffected",
            "fix": "runner: note moved to preflight `facts`/`notes`; `is_pin` guard; `pins_differ` and "
                   "the re-pin loop ignore non-pin entries (selftest 46 checks)",
            "crash_log": R.pin(CRASH_LOG) if CRASH_LOG.exists() else None,
            "crash_log_text": crash,
            "crash_stdout_log": R.pin(CRASH_STDOUT) if CRASH_STDOUT.exists() else None,
        },
        "attempt": ATTEMPT,
        "command": R.command(output),
        "command_line": __import__("subprocess").list2cmdline(R.command(output)),
        "output_root": str(output),
        "cwd": str(ROOT),
        "preflight": plan,
        "oracle": R.pin(R.ORACLE), "comparator": R.pin(R.COMPARATOR),
        "oracle_executed_against_this_run": False,
        "monitor": {"api": "GetProcessMemoryInfo/PROCESS_MEMORY_COUNTERS_EX",
                    "sampling_interval_seconds": R.SAMPLE_SECONDS, "limit_bytes": R.LIMIT,
                    "sampling_limit": "polling and termination are not an instantaneous hard memory cap",
                    "source": "the attempt-1 samples log the runner wrote before crashing"},
        "source_and_input_pins_before": None,
        "source_and_input_pins_before_note": "never written by the crashed runner; run-time digests are "
                                             "those the stage recorded (reevaluation.run_time_digests)",
        "source_and_input_pins_after": after,
        "pins_changed": pins_changed,
        "pins_unchanged": pins_unchanged,
        "exit_code": inference["inferred_exit_code"],
        "exit_code_source": "INFERRED (see reevaluation.exit_code_inference); not recorded by the runner",
        "wall_seconds": samples.get("last_sample_elapsed_seconds"),
        "wall_seconds_source": "last memory-sample elapsed stamp (lower bound within 0.1 s)",
        "sample_count": samples.get("sample_count"),
        "sampled_os_maxima_bytes": samples.get("sampled_os_maxima_bytes"),
        "budget_breach": samples.get("budget_breach"),
        "stage_runtime": {"runtime_seconds": runtime_seconds, "runtime": stage_runtime},
        "stage_stdout_summary": stage_summary,
        "stage_stderr": stderr_text,
        "ledger_before": ledger_before,
        "ledger_after": ledger_after,
        "ledger_lines_gained": gained,
        "ledger_new_lines": new,
        "ledger_two_cp15_lines": ledger_ok,
        "output_evidence": evidence,
        "seal": seal,
        "logs": {"stdout": str(stdout_log), "stderr": str(stderr_log), "samples": str(samples_log)},
        "finished_utc": datetime.now(timezone.utc).isoformat(),
        "accepted": accepted,
        "accepted_source": "accepted_reevaluated (post hoc); identical predicate set to the runner's, "
                           "with exit_code inferred and pins compared against the stage's own records",
        "qualification": ("Bounded software measurement of a pre-registered point-in-time universe "
                          "construction (trial_count_declared 0). Membership lists and counts only: "
                          "no alpha, no forecast, no Sharpe, no capacity; survivorship figures are "
                          "lower bounds."),
    }
    with OUT.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"accepted_reevaluated": accepted, "reasons": reasons,
                      "exit_code_inferred": inference["inferred_exit_code"],
                      "rebalances": (evidence.get("membership_bin") or {}).get("rebalance_count"),
                      "peak_working_set_bytes": (samples.get("sampled_os_maxima_bytes") or {}).get("PeakWorkingSetSize"),
                      "sample_count": samples.get("sample_count"),
                      "wall_seconds_lower_bound": samples.get("last_sample_elapsed_seconds"),
                      "stage_runtime_seconds": runtime_seconds,
                      "ledger_lines_gained": gained, "pins_unchanged": pins_unchanged,
                      "out": str(OUT), "out_sha256": hashlib.sha256(OUT.read_bytes()).hexdigest()},
                     indent=2))
    return 0 if accepted else 1


if __name__ == "__main__":
    sys.exit(main())
