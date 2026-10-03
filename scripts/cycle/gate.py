"""The cycle's internal checks and ledger hooks (research_cycle.py, split move-only into scripts/cycle/ by P9 lane
E2): the fields check, the admission gate, the gate's admission trials, cycle_verdict.json with the ledger state it
records (review C-6, F-3), and the summ's ledger copy."""
from __future__ import annotations

import json
from pathlib import Path
import shutil

import cycle_admission
import cycle_resume
import research_ledger
from cycle.phases import Cycle
from cycle.spec import EXIT_GATE, SUMM_V8, CycleError, as_built, summ_protocol
from cycle_verdict import VERDICT, write_verdict as _write_verdict
from research_common import sha256_file


# ------------------------------------------------------------------ internal checks
def fields_check(cycle: Cycle, fdm: str, log=print) -> None:
    f = cycle.spec["fields"]
    if as_built(f) and "list" in f:  # as built: the pinned manifest's rows are the declared list
        doc = cycle.res.read_json(fdm) or {}
        names = [row.get("name") for row in doc.get("fields", [])]
        log(f"fields check: pinned {fdm} sha256 {f['manifest_sha256'] or cycle.res.sha(fdm)}: {len(names)} rows"
            f"{' == the spec list' if names == f['list'] else ' DIFFER from the spec list'}")
        if names != f["list"]:
            raise CycleError("fields check FAILED (the pinned manifest's field rows differ from fields.list)")
    chk = f.get("check")
    if not chk:
        return
    res = cycle.res
    b = res.read_json(fdm)
    if b is None:
        raise CycleError(f"fields check: no manifest {fdm}")
    base_key = chk.get("baseline", "baseline_fields")
    a = res.read_json(cycle.ipath(base_key)) if base_key in cycle.spec["inputs"] else None
    added, bad = [], []
    if a is not None:
        names = [f["name"] for f in a["fields"]]
        bad = [n for n in names if a["files"][n + ".f64"] != b["files"].get(n + ".f64")]
        added = sorted({f["name"] for f in b["fields"]} - set(names))
        log(f"fields check: {len(names) - len(bad)} of {len(names)} baseline fields byte-identical "
            f"{bad or ''}; added {added}")
    ok = not bad and (a is None or "expect_added" not in chk or added == sorted(chk["expect_added"]))
    want_sv = chk.get("short_volume_files_sha256")
    if want_sv:
        sv = next((f for f in b["fields"] if f["name"] == "sv_ratio126"), None)
        files = ((sv or {}).get("short_volume") or {}).get("files") or {}
        log(f"fields check: sv_ratio126 files {files.get('files_read')} {files.get('first_file_date')}.."
            f"{files.get('last_file_date')} sha {files.get('files_sha256')}")
        ok = ok and files.get("files_sha256") == want_sv
    reuse = b.get("reuse")
    if reuse:
        log(f"fields reuse: reused {len(reuse['reused'])}, computed {reuse['computed']} (from {reuse['from']})")
    if not ok:
        raise CycleError("fields check FAILED (baseline fields changed, unexpected added set or short-volume pin)")


def gate(cycle: Cycle, w_dir: str, log=print) -> None:
    g = cycle.spec["gate"]
    adm = cycle.res.read_json(f"{w_dir}/admission.json")
    if adm is None:
        raise CycleError(f"gate: no admission.json in {w_dir} (run fit first)")
    rows = {c["id"]: c for c in adm["candidates"]}

    def fmt(x):
        return "None" if x is None else f"{x:.4f}" if isinstance(x, float) else str(x)
    passed = []
    for cid in g["admitted"]:
        r = rows.get(cid)
        if r is None:
            log(f"gate {g.get('name', 'gate')} {cid}: not in admission.json")
            passed.append(False)
            continue
        log(f"gate {g.get('name', 'gate')} {cid}: status {r['status']} failed {r.get('failed_checks')}; runner sign "
            f"{r.get('runner_sign')} vs prior {r.get('s_k')} (agrees {r.get('sign_agrees')}); HAC t {fmt(r.get('hac_t'))}; "
            f"tau {fmt(r.get('tau'))} (limit {adm.get('rules', {}).get('tau_limit')}); max |rho| "
            f"{fmt(r.get('max_abs_rho'))} with {r.get('max_abs_rho_with')}; redundant_with {r.get('redundant_with')}; "
            f"train days {r.get('train_days')}")
        passed.append(r["status"] == "admitted" and (not g.get("sign_agrees", True) or r.get("sign_agrees") is True))
    for cid in g.get("report", []):
        if cid in rows:
            log(f"  {cid}: status {rows[cid]['status']}, max |rho| {fmt(rows[cid].get('max_abs_rho'))} with "
                f"{rows[cid].get('max_abs_rho_with')}")
    if "reference_admission" in cycle.spec["inputs"]:
        ref = cycle.res.read_json(cycle.ipath("reference_admission"))
        if ref is not None:
            prev = {c["id"]: c["status"] for c in ref["candidates"]}
            moved = {k: (prev[k], rows[k]["status"]) for k in prev if k in rows and rows[k]["status"] != prev[k]}
            log(f"  reference members vs {cycle.ipath('reference_admission')}: {len(moved)} status changes {moved or ''}")
    require = g.get("require", "all")
    ok = all(passed) if require == "all" else any(passed)
    if require != "all":
        log(f"gate {g.get('name', 'gate')}: {sum(passed)} of {len(passed)} listed candidates admitted with the prior "
            f"sign (require {require})")
    log(f"gate {g.get('name', 'gate')} " + ("PASS" if ok else "FAIL: stop (no later phase runs)"))
    if not ok:
        raise CycleError("gate failed", EXIT_GATE)


def admission_trials(cycle, w_dir: str, log) -> None:
    """Review C-7: the gate of a v8 cycle with a ledger of record (--ledger, else summ.ledger) first ledgers the
    admission trials it reads, one chained line per listed candidate (cycle_admission.py); a v7 cycle writes none."""
    rel = cycle.ledger or (cycle.spec.get("summ") or {}).get("ledger")
    if not rel or summ_protocol(cycle.spec) != SUMM_V8:
        return
    try:
        cycle_admission.ledger_admissions(cycle, rel, w_dir, log)
    except ValueError as exc:
        raise CycleError(f"HARD-STOP [gate]: admission trials not ledgered: {exc}") from exc


def write_verdict(cycle: Cycle, timings: dict, log) -> dict:
    try:
        return _write_verdict(cycle, timings, cycle_resume.spec_digest(cycle), log,   # F-9: the template chain's
                              ledger_state(cycle), keep=bool(getattr(cycle, "keep_verdicts", False)))
    except ValueError as exc:     # review C-1: no verdict DSR from a cell count; C-6: a broken ledger chain
        raise CycleError(f"HARD-STOP [verdict]: {exc}") from exc


def ledger_state(cycle) -> dict | None:
    """{path, head, lines} of the sprint ledger of record (--ledger, else summ.ledger) when it exists: the chain head
    (backtest_integrity.ledger_head, the chain verified) every verdict records (review C-6). None without one."""
    rel = cycle.ledger or (cycle.spec.get("summ") or {}).get("ledger")
    if not rel or not cycle.res.path(rel).is_file():
        return None
    bi = research_ledger.backtest_integrity()
    p = cycle.res.path(rel)
    check_recorded_heads(cycle, p, bi)
    return {"path": rel, "head": bi.ledger_head(p), "lines": len(bi.ledger_read(p))}


def check_recorded_heads(cycle, p: Path, bi) -> None:
    """Review F-3: every earlier verdict of this ledger under the cycle's out base (<out base>/cycle-*/
    cycle_verdict.json, ledger.path resolving to ``p``) recorded {head, lines}; the ledger's first ``lines`` lines must
    still fold to that head (backtest_integrity.chain_head), so an edit of any line an earlier verdict read, the tail
    included, stops the verdict (ValueError: HARD-STOP [verdict])."""
    texts = [line for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]
    for vp in sorted(cycle.res.path(cycle.out_base()).glob(f"cycle-*/{VERDICT}")):
        try:
            led = json.loads(vp.read_text(encoding="utf-8")).get("ledger")
        except (OSError, ValueError, AttributeError) as exc:
            raise ValueError(f"verdict {vp} is unreadable ({exc}): its recorded ledger head cannot be checked") from exc
        if not isinstance(led, dict) or not isinstance(led.get("path"), str) or \
                cycle.res.path(led["path"]).resolve() != p.resolve():
            continue
        n, head = led.get("lines"), led.get("head")
        if type(n) is not int or n > len(texts) or bi.chain_head(texts[:n]) != head:
            raise ValueError(f"ledger {led['path']}: its first {n} line(s) no longer fold to the chain head {head} that "
                             f"{vp} recorded (a line an earlier verdict read was edited or removed)")


def copy_ledger(cycle: Cycle, log) -> None:
    """summ.ledger_copy: the trial ledger copied (e.g. into the sprint directory) after the cycle's summ, with its chain
    head logged (review C-6)."""
    sm = cycle.spec["summ"]
    if not sm.get("ledger_copy"):
        return
    src, dst = cycle.res.path(cycle.ledger or sm["ledger"]), cycle.res.path(sm["ledger_copy"])
    if not src.is_file():
        raise CycleError(f"HARD-STOP [summ]: summ.ledger_copy: no ledger at {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    try:
        head = research_ledger.backtest_integrity().ledger_head(dst)
    except ValueError as exc:
        raise CycleError(f"HARD-STOP [summ]: summ.ledger_copy: {exc}") from exc
    log(f"   ledger copied: {src} -> {sm['ledger_copy']} (sha256 {sha256_file(dst)}, chain head {head})")
