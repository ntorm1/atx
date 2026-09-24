"""One native equity-book trial; coordinator runs only after releasing the executable."""
from __future__ import annotations

import ctypes
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import time


ROOT = Path("C:/atx/.worktrees/equity-platform")
SHARED = Path("C:/atx")
AUDITS = ROOT / "build-equity/audits"
EXE = ROOT / "build-equity/bin/atx-impl.exe"
CONTEXT = SHARED / "data/tickerhistory_training_native_20260919/context.bin"
BASELINE = SHARED / "data/equity_baseline_training_2013_20260919"
OUTPUT = SHARED / "data/equity_book_training_2013_20260920"
MEASUREMENT = AUDITS / "iteration8-equity-book-measurement.json"
PREFLIGHT = AUDITS / "iteration8-equity-book-preflight.json"
STDOUT = AUDITS / "iteration8-equity-book-native.stdout.log"
STDERR = AUDITS / "iteration8-equity-book-native.stderr.log"
SAMPLES = AUDITS / "iteration8-equity-book-memory-samples.jsonl"
LIMIT = 3_000_000_000
SAMPLE_SECONDS = 0.1
HELPERS = SHARED / "build-equity/audits"
IMPORTED_HELPER_PINS = {}


def load_helper(name: str, path: Path):
    # Compile the captured bytes, avoiding a stale bytecode cache. Main guards in
    # both helpers prevent their native launch paths from running during import.
    captured = path.read_bytes()
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None:
        raise RuntimeError(f"cannot import measurement helper: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    exec(compile(captured, str(path), "exec"), module.__dict__)
    IMPORTED_HELPER_PINS[str(path)] = hashlib.sha256(captured).hexdigest()
    return module


memory = load_helper("iteration6_build_training_context",
                     HELPERS / "iteration6_build_training_context.py")
baseline_helper = load_helper("iteration8_prior_baseline_helper",
                              HELPERS / "iteration6_run_training_baseline.py")
sha, require, save_new = memory.sha, memory.require, memory.save_new
decode_json, canonical = memory.decode_json, memory.canonical

EXPECTED_PANELS = {
    CONTEXT: ("ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079",
              "e9d53a8932b242750f44f8744a46856b535a0b1c27bd91dcf8dda60555e5f68a"),
    BASELINE / "evaluation.bin": (
        "57b7c21c9320e98c1dee6c97e5f3b8cde211c5efdd688c2daf6fb28daa3cd475",
        "25158e69ed35a86424a3958359541817f2330534851eabe58068915caf9e797b"),
    BASELINE / "combo.bin": (
        "def31e5ca03dc2329bb05a2526c4470e77028fea060d0c3743634d6b4f86a692",
        "05f0426590676a2bbf1ab1a7055a1d4175da92e03978e217a507c41c9114af6c"),
    BASELINE / "books.bin": (
        "615019e7c83c34adc950bf41bda2e28923960249fbef0fccd8b84e14c251eb88",
        "a67930ee992b5b2dfd6e74710ec1971c2d08c26cd3c8471f9b317dd42418ac26"),
}
SOURCE_NAMES = [
    "CMakeLists.txt", "CMakePresets.json", "atx-engine/CMakeLists.txt",
    "atx-impl/CMakeLists.txt", "atx-impl/src/main.cpp", "atx-impl/src/stages.hpp",
    "atx-impl/src/config.cpp", "atx-impl/src/config.hpp", "atx-impl/src/dispatch.cpp",
    "atx-impl/src/stage_equity_book.cpp", "atx-impl/src/stage_equity_book.hpp",
    "atx-impl/src/equity_allocation.cpp", "atx-impl/src/equity_allocation.hpp",
    "atx-impl/src/equity_baseline_views.cpp", "atx-impl/src/equity_baseline_views.hpp",
    "atx-impl/src/stage_report.cpp", "atx-impl/src/replay_report.cpp",
    "atx-impl/src/replay_report.hpp", "atx-impl/src/replay_diagnostics.cpp",
    "atx-impl/src/replay_diagnostics.hpp", "atx-impl/src/panel_artifact.cpp",
    "atx-impl/src/panel_artifact.hpp", "atx-impl/src/panel_pipeline.cpp",
    "atx-impl/src/panel_pipeline.hpp", "atx-impl/src/serialize_panel.cpp",
    "atx-impl/src/serialize_panel.hpp", "atx-impl/src/stage_data_provenance.cpp",
    "atx-engine/src/book/replay.cpp", "atx-engine/include/atx/engine/book/replay.hpp",
    "atx-engine/include/atx/engine/risk/constraints.hpp",
    "atx-engine/include/atx/engine/risk/qp_solver.hpp",
    "atx-engine/include/atx/engine/risk/qp_augment.hpp",
    "atx-engine/include/atx/engine/risk/kkt_ldl.hpp",
    "atx-engine/include/atx/engine/risk/factor_model.hpp",
    "atx-engine/src/risk/factor_model.cpp",
]


def pin(path: Path) -> dict:
    return {"sha256": sha(path), "size_bytes": path.stat().st_size}


def prepare() -> dict:
    require(sys.platform == "win32" and ctypes.sizeof(ctypes.c_void_p) == 8,
            "64-bit Windows Python monitor required")
    require(ctypes.sizeof(memory.ProcessMemoryCountersEx) == 80, "unexpected PSAPI ABI")
    panels = {}
    inputs = {}
    for path, (expected_id, expected_manifest_sha) in EXPECTED_PANELS.items():
        manifest_path = Path(str(path) + ".manifest.json")
        raw = manifest_path.read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected_manifest_sha,
                f"frozen input manifest changed: {manifest_path}")
        document = decode_json(raw)
        body = dict(document)
        artifact_id = body.pop("artifact_id")
        require(artifact_id == expected_id and
                hashlib.sha256(b"atx-panel-artifact-v1\n" + canonical(body)).hexdigest() == artifact_id,
                f"invalid input artifact identity: {path}")
        payload = pin(path)
        require(payload["sha256"] == document["payload"]["sha256"] and
                payload["size_bytes"] == int(document["payload"]["size_bytes"]),
                f"input payload binding mismatch: {path}")
        inputs[str(path)] = payload
        inputs[str(manifest_path)] = pin(manifest_path)
        panels[path.name] = document
    evaluation = panels["evaluation.bin"]
    recipe = decode_json(evaluation["recipe"])
    require(recipe["evaluation_start"] == "2013-04-04" and
            recipe["evaluation_end_exclusive"] == "2014-01-01" and
            evaluation["shape"]["dates"] == "189" and
            evaluation["shape"]["instruments"] == "1661", "original evaluation window changed")
    context_keys = panels["context.bin"]["axes"]["session_keys"]
    require(evaluation["axes"]["session_keys"] == context_keys[256:],
            "evaluation is not the full original context suffix")
    require(panels["books.bin"]["axes"]["session_keys"] ==
            evaluation["axes"]["session_keys"][::5], "original weekly preference schedule changed")
    # Preserve every original trial file, including its failure evidence. Do not
    # require a completed baseline: the fixed-target trial intentionally failed.
    for path in sorted(BASELINE.rglob("*")):
        require(path.resolve().is_relative_to(BASELINE.resolve()), "baseline path escapes root")
        if path.is_file() and str(path) not in inputs:
            inputs[str(path)] = pin(path)
    for path in (CONTEXT.parent / "segments/_ingestion.manifest.json",
                 SHARED / "data/tickerhistory_training_20120326_20131231_20260919/manifest.json"):
        inputs[str(path)] = pin(path)
    sources = {str(ROOT / name): pin(ROOT / name) for name in SOURCE_NAMES}
    sources[str(Path(__file__).resolve())] = pin(Path(__file__).resolve())
    for name, loaded_sha in IMPORTED_HELPER_PINS.items():
        sources[name] = pin(Path(name))
        require(sources[name]["sha256"] == loaded_sha, "measurement helper changed during import")
    executable = pin(EXE)
    args = [str(EXE), "equity-book", "--panel", str(CONTEXT), "--baseline-dir", str(BASELINE),
            "--out", str(OUTPUT), "--max-working-bytes", str(LIMIT),
            "--report-aum", "100000000", "--replay-execution-delay", "1",
            "--replay-trade-bps", "5", "--replay-annual-borrow-bps", "365",
            "--replay-day-basis", "365"]
    return {"schema": "atx.iteration8-equity-book-preflight-v1", "args": args,
            "command_line": subprocess.list2cmdline(args), "cwd": str(ROOT),
            "executable": {"path": str(EXE), **executable}, "input_pins": inputs,
            "source_pins": sources, "source_pin_scope": "selected first-party implementation and build configuration",
            "panel_artifact_ids": {name: value["artifact_id"] for name, value in panels.items()},
            "evaluation_start": "2013-04-04", "evaluation_end_exclusive": "2014-01-01",
            "evaluation_observations": 189, "intervals_if_complete": 188,
            "scheduled_decisions": 38, "evaluation_session_keys": evaluation["axes"]["session_keys"],
            "evaluation_instrument_ids": evaluation["axes"]["instrument_ids"],
            "original_baseline_complete": (BASELINE / "manifest.json").exists(),
            "baseline_failure_preserved": (BASELINE / "failure.json").exists(),
            "working_memory_limit_bytes": LIMIT,
            "window_policy": "full original evaluation; no date trimming, security exclusion or imputation",
            "memory_admission": "native allocator/replay preflight plus monitored child high-water guard",
            "source_archive_and_segments_rehashed": False,
            "source_economic_adjustments": "unverified; prior source contradictions remain",
            "investment_qualification": "unknown-no-promotion"}


def output_evidence() -> dict:
    result = {"root_exists": OUTPUT.exists(), "files": [], "pending_directories": []}
    if not OUTPUT.exists():
        return result
    for path in sorted(OUTPUT.rglob("*")):
        require(path.resolve().is_relative_to(OUTPUT.resolve()), "native output path escapes root")
        relative = path.relative_to(OUTPUT).as_posix()
        if path.is_file():
            result["files"].append({"filename": relative, **pin(path)})
        elif path.is_dir() and path.name.startswith("."):
            result["pending_directories"].append(relative)
    for name in ("failure.json", "request.json", "allocation_certificates.json"):
        path = OUTPUT / name
        if path.is_file():
            require(path.stat().st_size <= 16 * 1024 * 1024, "oversized output metadata")
            result[name] = decode_json(path.read_bytes())
    result["complete_manifest_exists"] = (OUTPUT / "manifest.json").exists()
    return result


def validate_completion(plan: dict) -> dict:
    require(not (OUTPUT / ".pending").exists(), "equity-book completion still pending")
    completed = baseline_helper.verify_native_manifest(
        OUTPUT / "manifest.json", "book_id", b"atx-equity-book-v1\n")
    replay = baseline_helper.verify_native_manifest(
        OUTPUT / "report/manifest.json", "report_id", b"atx-replay-report-v1\n")
    require(completed["status"] == "complete" and replay["status"] == "complete" and
            completed["report_id"] == replay["report_id"], "completion/report identity mismatch")
    for document in (completed, replay):
        require(document["recipe"]["producer_executable_sha256"] == plan["executable"]["sha256"],
                "output producer differs from executed binary")
    require(replay["axes"]["session_keys"] == plan["evaluation_session_keys"] and
            replay["axes"]["instrument_ids"] == plan["evaluation_instrument_ids"],
            "completed report shortened or changed original evaluation axes")
    summary = decode_json((OUTPUT / "report/summary.json").read_bytes())
    require(summary["full"]["observed_intervals"] == 188 and
            summary["accepted_allocation_count"] == 38, "incomplete trial coverage")
    require((OUTPUT / "report/allocations.csv").is_file(), "accepted allocations are missing")
    return {"book_id": completed["book_id"], "report_id": replay["report_id"],
            "qualification": completed["qualification"], "summary": summary}


def recheck_pins(plan: dict) -> dict:
    observed = {}
    for category in ("input_pins", "source_pins"):
        observed[category] = {}
        for name, before in plan[category].items():
            try:
                after = pin(Path(name))
                observed[category][name] = {"after": after, "unchanged": before == after}
            except BaseException as error:
                observed[category][name] = {"unchanged": False, "error": str(error)}
    before = {key: value for key, value in plan["executable"].items() if key != "path"}
    after = pin(EXE)
    observed["executable"] = {"after": after, "unchanged": before == after}
    observed["all_unchanged"] = observed["executable"]["unchanged"] and all(
        item["unchanged"] for category in ("input_pins", "source_pins")
        for item in observed[category].values())
    return observed


def main() -> int:
    require(sys.argv[1:] == ["--execute-approved-native"],
            "launch requires the coordinator's executable-ready release flag")
    for path in (OUTPUT, MEASUREMENT, PREFLIGHT, STDOUT, STDERR, SAMPLES):
        require(not path.exists(), f"refusing to reuse output: {path}")
    report = {"schema": "atx.iteration8-equity-book-measurement-v1", "status": "preflight",
              "accepted": False, "started_utc": datetime.now(timezone.utc).isoformat(),
              "monitor": {"api": "GetProcessMemoryInfo/PROCESS_MEMORY_COUNTERS_EX",
                  "official_source": "https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex",
                  "sampling_interval_seconds": SAMPLE_SECONDS, "limit_bytes": LIMIT,
                  "guard": "current/OS-peak working set and private commit",
                  "sampling_limit": "polling and termination are not an instantaneous hard memory cap"}}
    plan = process = None
    maxima = {}
    count = 0
    started = time.perf_counter()
    native_started = None
    try:
        plan = prepare()
        save_new(PREFLIGHT, plan)
        report.update({"preflight": plan, "preflight_sha256": sha(PREFLIGHT),
                       "args": plan["args"], "command_line": plan["command_line"]})
        read_memory = memory.memory_reader()
        with STDOUT.open("xb") as stdout, STDERR.open("xb") as stderr, \
                SAMPLES.open("x", encoding="utf-8", newline="\n") as samples:
            native_started = time.perf_counter()
            process = subprocess.Popen(plan["args"], cwd=str(ROOT), stdin=subprocess.DEVNULL,
                stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
            report.update({"status": "running", "pid": process.pid,
                           "native_started_utc": datetime.now(timezone.utc).isoformat()})
            print(json.dumps({"event": "native-equity-book-started", "pid": process.pid,
                              "command_line": plan["command_line"]}), flush=True)
            last_progress = native_started
            while True:
                sample = read_memory(process)
                count += 1
                for key, value in sample.items():
                    maxima[key] = max(maxima.get(key, 0), value)
                sample["elapsed_seconds"] = time.perf_counter() - native_started
                samples.write(json.dumps(sample, sort_keys=True) + "\n")
                samples.flush()
                code = process.poll()
                breached = {key: value for key, value in maxima.items() if key in
                    {"WorkingSetSize", "PeakWorkingSetSize", "PrivateUsage", "PagefileUsage",
                     "PeakPagefileUsage"} and value > LIMIT}
                if breached:
                    report["budget_breach"] = breached
                    if code is None:
                        process.terminate()  # Only this Popen-owned child, never another process.
                        code = process.wait(timeout=30)
                        report["terminated_child_for_memory"] = True
                    report["exit_code"] = code
                    raise RuntimeError("native equity-book exceeded declared 3 GB budget")
                if code is not None:
                    report["exit_code"] = code
                    break
                if time.perf_counter() - last_progress >= 30:
                    print(json.dumps({"event": "native-equity-book-progress", "pid": process.pid,
                        "elapsed_seconds": sample["elapsed_seconds"],
                        "os_peak_working_set_bytes": maxima["PeakWorkingSetSize"],
                        "os_peak_private_commit_bytes": maxima["PeakPagefileUsage"]}), flush=True)
                    last_progress = time.perf_counter()
                time.sleep(SAMPLE_SECONDS)
        report["native_wall_seconds"] = time.perf_counter() - native_started
        require(report["exit_code"] == 0, f"native equity-book exited {report['exit_code']}")
        report["completion"] = validate_completion(plan)
        report.update({"status": "completed", "accepted": True,
            "acceptance_scope": "full-window bounded software trial; no investment qualification"})
    except BaseException as error:
        report.update({"status": "failed", "accepted": False,
                       "error": f"{type(error).__name__}: {error}"})
        try:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=30)
                report["terminated_child_after_monitor_error"] = True
        except BaseException as termination_error:
            report["termination_error"] = f"{type(termination_error).__name__}: {termination_error}"
        if process is not None:
            report["exit_code"] = process.returncode
    finally:
        if native_started is not None:
            report.setdefault("native_wall_seconds", time.perf_counter() - native_started)
            report["native_finished_utc"] = datetime.now(timezone.utc).isoformat()
        if plan is not None:
            try:
                report["pin_recheck"] = recheck_pins(plan)
                require(report["pin_recheck"]["all_unchanged"],
                        "input, executable or source changed during native trial")
            except BaseException as error:
                report.update({"status": "failed", "accepted": False,
                               "pin_error": f"{type(error).__name__}: {error}"})
        try:
            report["output_evidence"] = output_evidence()
        except BaseException as error:
            report.update({"status": "failed", "accepted": False,
                           "evidence_error": f"{type(error).__name__}: {error}"})
        report.update({"finished_utc": datetime.now(timezone.utc).isoformat(),
            "total_wall_seconds": time.perf_counter() - started, "sample_count": count,
            "sampled_os_maxima_bytes": maxima,
            "logs": {"stdout": str(STDOUT), "stderr": str(STDERR), "samples": str(SAMPLES)}})
        save_new(MEASUREMENT, report)
    print(json.dumps({"event": "native-equity-book-finished", "status": report["status"],
        "accepted": report["accepted"], "exit_code": report.get("exit_code"),
        "error": report.get("error"), "wall_seconds": report["total_wall_seconds"],
        "os_peak_working_set_bytes": maxima.get("PeakWorkingSetSize"),
        "os_peak_private_commit_bytes": maxima.get("PeakPagefileUsage"),
        "measurement": str(MEASUREMENT)}), flush=True)
    return 0 if report["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
