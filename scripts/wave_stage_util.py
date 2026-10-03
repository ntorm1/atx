"""Shared pieces of the wave stages (wave_stages.py assembles the chain): the stage names and constants, the
research_cycle argv on the wave's root, skips, the write-once spec writer, the phase receipts, the reader
digests and the predecessor-digest check every stage's inputs use.
"""
from __future__ import annotations

import json
from pathlib import Path

import wave_steps as WS
from wave_context import Wave, stage_chain

StageError = stage_chain.StageError
Stage = stage_chain.Stage
EXIT_PIN = 3
SPECS_V8 = "scripts/specs/v8"
ROW_KEYS = ("id", "status", "runner_sign", "s_k", "sign_agrees", "failed_checks", "redundant_with")
MARGINAL_KEYS = ("id", "ic21", "ic21_hac_t", "marginal_ic21", "marginal_hac_t", "max_abs_rho", "max_rho_member")


def lib_spec(name: str) -> str:
    return f"{SPECS_V8}/lib-{name}.json"


def cyc(w: Wave, verb: str, spec: str, *extra: str) -> list[str]:
    """research_cycle.py VERB SPEC ... --root <the wave's root> (it writes under that root, the root the driver
    commits); a ``run`` carries the manifest's driver options (WS.driver_flags; none without a driver block)."""
    flags = WS.driver_flags(w.manifest.get("driver")) if verb == "run" else []
    return WS.cycle_argv(w.python, verb, spec, *extra, *flags, root=w.root)


def timings_on(w: Wave) -> bool:
    """driver.timings (P9 OR section 5): the stages record complete timings (processes, the screen's phase rows) and
    wave-result.json carries them (stage seconds, readers, bundle)."""
    return bool((w.manifest.get("driver") or {}).get("timings"))


def fold_processes(rows: list[dict]) -> list[dict]:
    """Process rows {what, seconds, exit_code} folded by what, in first-seen order: [{what, calls, seconds}]."""
    out: dict[str, dict] = {}
    for r in rows:
        t = out.setdefault(r["what"], {"what": r["what"], "calls": 0, "seconds": 0.0})
        t["calls"] += 1
        t["seconds"] = round(t["seconds"] + r["seconds"], 3)
    return list(out.values())


def lock_exes(w: Wave) -> bool:
    """driver.lock_exes (P9 OR-2): the wave pins the exes of every cell spec it writes (`lock --exes --write`)."""
    return bool((w.manifest.get("driver") or {}).get("lock_exes"))


def pin_exes(w: Wave, spec: str) -> None:
    """`lock SPEC --exes --write` before the spec's commit, under driver.lock_exes; nothing without it."""
    if lock_exes(w):
        w.run(cyc(w, "lock", spec, "--exes", "--write"), f"lock --exes {spec}")


def verdict_file(w: Wave, cycle_dir: str, mode: str) -> str:
    """The cycle verdict a stage reads and pins: <cycle dir>/cycle_verdict.json, as before; under driver.keep_verdicts
    the newest per-run copy <cycle dir>/verdicts/<mode>-<k>.json that the research_cycle run the stage just ran wrote
    (never overwritten, so the stage's pin never dangles: P9 OR section 3; a missing copy is a stop)."""
    if not (w.manifest.get("driver") or {}).get("keep_verdicts"):
        return f"{cycle_dir}/cycle_verdict.json"
    d, n = w.path(f"{cycle_dir}/verdicts"), len(mode) + 1
    ks = sorted(int(p.stem[n:]) for p in d.glob(f"{mode}-*.json") if p.stem[n:].isdigit()) if d.is_dir() else []
    if not ks:
        raise StageError(f"driver.keep_verdicts: no {mode} verdict copy under {cycle_dir}/verdicts (research_cycle "
                         "--keep-verdicts writes one per run)", EXIT_PIN)
    return f"{cycle_dir}/verdicts/{mode}-{ks[-1]}.json"


def library_wave(w: Wave) -> bool:
    return "candidates" in w.manifest


def skipped(why: str) -> dict:
    return {"skipped": why}


def no_cell(done: dict) -> bool:
    return not (done.get("spec") or {}).get("cell_spec")


# ------------------------------------------------------------------ the write-once spec writer
def write_spec_file(w: Wave, rel: str, doc: dict, same=None) -> None:
    """Write a cell spec once: an existing file must hold exactly these bytes (a resumed stage), or with ``same`` (a
    normalizer, e.g. WS.unpinned after `lock --write`) the same document under it; else a stop."""
    text = json.dumps(doc, indent=2) + "\n"
    p = w.path(rel)
    if p.is_file():
        have = p.read_text(encoding="utf-8")
        if have != text and (same is None or same(json.loads(have)) != same(doc)):
            raise StageError(f"{rel} exists with other content: never overwritten (rename the cell)", EXIT_PIN)
        return
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")


# ------------------------------------------------------------------ phase receipts, reader digests
def phase_rows(w: Wave, spec_rel: str) -> list[dict]:
    """The bounded-runner receipts of a cell's phases (timings: seconds, peak MiB, outcome; the executable_sha256 the
    runner recorded, P9 OR-2), every attempt."""
    rows = []
    for phase, base in w.phase_bases(spec_rel).items():
        rows += run_rows(w, phase, base)
    return rows


def exe_notes(w: Wave, rows: list[dict]) -> list[str]:
    """Ruling E1-REUSE-a2: one note per completed phase row whose K-P9-10 receipt (argv_sha256) names another
    executable SHA than its executable (the receipt's command[0]) has on disk now: an output research_cycle reused on
    a rebuilt exe without an exes_sha256 pin (a pinned mismatch stops the cycle). Empty for receipts before K-P9-10."""
    out = []
    for row in rows:
        r = w.read_json(f"{row['run_dir']}/receipt.json") or {}
        cmd, made = r.get("command"), r.get("executable_sha256")
        if r.get("outcome") != "completed" or not isinstance(r.get("argv_sha256"), str) or not isinstance(made, str) \
                or not (isinstance(cmd, list) and cmd and isinstance(cmd[0], str)):
            continue
        now = w.sha(cmd[0])
        if now is not None and now != made:
            out.append(f"{row['phase']} {row['run_dir']}: made by executable sha256 {made[:12]}, "
                       f"{Path(cmd[0]).name} is sha256 {now[:12]} now")
    return out


def run_rows(w: Wave, phase: str, base: str) -> list[dict]:
    """One phase row per bounded run dir of the output ``base`` (``<base>-run[<k>]``) holding a receipt."""
    rows = []
    for d in w.run_dirs(base):
        r = w.read_json(f"{d}/receipt.json")
        if isinstance(r, dict):
            rows.append({"phase": phase, "run_dir": d, "outcome": r.get("outcome"), "exit_code": r.get("exit_code"),
                         "seconds": r.get("wall_seconds"),
                         "peak_mib": (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20,
                         "executable_sha256": r.get("executable_sha256")})
    return rows


def reader_digests(w: Wave, names: list[str]) -> dict:
    return {n: w.sha(w.wave_path("readers", f"{n}.json")) for n in names}


# ------------------------------------------------------------------ predecessor digests (stage inputs)
# Each stage's inputs are the digests its predecessor recorded of the files it hands on, recomputed now (only files no
# later stage rewrites): a moved file is stale (exit 3) both before the stage runs (against the predecessor's record)
# and on every resume after (against the stage's own receipt). Preflight pins the manifest, parent and fields itself.
def pinned(stage: str, recorded: dict, now: dict) -> dict:
    out = {}
    for key, value in now.items():
        if value != recorded.get(key):
            raise StageError(f"STALE: {stage} recorded {key} {str(recorded.get(key))[:80]}; it is {str(value)[:80]} "
                             "now (a file a done stage recorded changed: restore it, or start a new state dir)",
                             stage_chain.EXIT_STALE)
        out[f"{stage}.{key}"] = value
    return out
