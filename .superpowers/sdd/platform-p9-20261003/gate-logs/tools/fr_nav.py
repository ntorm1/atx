"""root FLAGS agent (RESUME 14; rulings FLAGS-ROUTE, FLAGS-ROUTE-2, FLAGS-SEQ (1); GATE-REV part 2 RQ-1): ONE bounded
Release NAV re-run of Y-F0 through the SPEC ROUTE, compared with the P9-B0 reference `build-equity/p9-b0g-yf0-nav`.

The spec route: the NAV argv the driver plans (scripts/cycle/phases.py nav_step, :824-832) for the lineage parent
`scripts/specs/v8/lib-v8ysb-gm.json` at HEAD (its leverage 1.1828, its `nav.flags` now carrying `--ann-mean-gross
--info-clock-print`), with only `nav.output` moved to the NEW dir `build-equity/p9-fr-yf0-nav`. The parent alone cannot
be built by the cycle (its own `inputs.baseline_fields` is the absent fields v13: FIELDS-RISK), so the argv is taken
from the cycle of the minimal NAV-only template over the parent FILE (the way every P9 cell is built, FLAGS-ROUTE-2
(a); research_spec.resolve drops the parent's own identity inputs): {"parent": PARENT, "change": {"set": {"nav.output":
NEW}}}, resolved in memory (no template file is written; the anchor path below only locates the parent), every input
pin verified (`Cycle(..., verify=True)`). Token 0 is the spec's `exes.nav` resolved against the pool root (the same
file; the form of the reference receipt and of b0_nav.py). The run is exe-direct through the bounded runner (no driver:
no seal scan, so no `--seal-allow`, SEAL-REF; timings off; no cache flag, SQL-11).

usage (cwd anywhere; the pool root is derived from this file's place):
  python fr_nav.py plan        print the planned argv, its token diff against the reference receipt of record, the
                               pre-run checks, and the planned NAV flags of X-10 / Y-1 (verify's argv digests); runs
                               nothing, writes nothing
  python fr_nav.py printspec   `<exe> nav <argv> --print-spec` (the NAV exe's flag adapter; "nothing runs",
                               book/nav_spec.hpp:51) for the planned argv and for the reference command with its
                               --output set to the new dir; stdout -> gate-logs/flags-navspec-{plan,ref}.json; the two
                               NavSpec documents compared by JSON path (paths only)
  python fr_nav.py run         the ONE bounded run (scripts/run_bounded_research.py --seconds 600 --max-rss-mib 2048),
                               then the record line (gate-logs/flags-run.txt), the receipt copy, the per-file SHA-256
                               listing (gate-logs/flags-files-p9-fr-yf0-nav.txt, the b0-files form)
  python fr_nav.py cmp         the new dir vs the reference by file SHA-256 and JSON leaf path (cmp.py / b0_cmp.py form;
                               allowed differing leaves: `summary.json` `/producer/*` ONLY, FLAGS-ROUTE), plus the
                               presence of the opt-in leaves on both sides; never prints a value
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
LOGS = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "gate-logs"
PY = sys.executable
PARENT = "scripts/specs/v8/lib-v8ysb-gm.json"
NEW = "build-equity/p9-fr-yf0-nav"
RUN_DIR = NEW + "-run"
REF_DIR = "build-equity/p9-b0g-yf0-nav"
REF_RECEIPT = "build-equity/p9-b0g-yf0-nav-run/receipt.json"
EXE_RECEIPT = "build-equity/mega-p9-3f-receipt.json"
FLAGS = ("--ann-mean-gross", "--info-clock-print")
CHILDREN = ("scripts/specs/v8/x-leverage-L2.0.json", "scripts/specs/v8/y-vol-target-y-1.json")
PROBE = {"schema": "atx.research-cycle-template/v1", "name": "fr-yf0-nav",
         "description": "root FLAGS agent probe: Y-F0 itself, NAV output moved to a new dir (never written to disk)",
         "parent": PARENT, "change": {"set": {"nav.output": NEW}}}

sys.path.insert(0, str(REPO / "scripts"))
os.chdir(REPO)


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    return h.hexdigest()


def argv_digest(args: list[str]) -> str:
    import research_tree  # noqa: PLC0415
    return research_tree.argv_sha256(args)


def nav_args(st) -> list[str]:
    return st.argv[st.argv.index("--") + 1:]


def planned() -> tuple[list[str], list[str], dict]:
    """(the driver's NAV argv after the runner's "--", the argv of record with token 0 resolved, the resolved spec)."""
    import research_cycle as RC  # noqa: PLC0415
    import research_spec  # noqa: PLC0415
    import research_tree  # noqa: PLC0415
    from cycle.resolver import Resolver  # noqa: PLC0415
    from cycle.spec import validate_spec  # noqa: PLC0415
    anchor = LOGS / "tools" / "fr-yf0-nav.template.json"   # never written: research_spec looks the parent up from it
    spec = research_spec.resolve(json.loads(json.dumps(PROBE)), anchor, lambda p: RC.load_spec(p), research_tree.REPO)
    validate_spec(spec)
    cyc = RC.Cycle(spec, Resolver(REPO))                  # verify=True: every input pin checked against disk
    st = next(s for s in cyc.steps() if s.phase == "nav")
    if st.output != NEW or st.state != "pending":
        raise SystemExit(f"STOP: the planned NAV step is {st.state} {st.output} (want pending {NEW})")
    args = nav_args(st)
    if Path(args[0]) != Path(spec["exes"]["nav"]):
        raise SystemExit(f"STOP: token 0 {args[0]} is not the spec's exes.nav {spec['exes']['nav']}")
    return args, [str(REPO / args[0]), *args[1:]], spec


def ref_command() -> list[str]:
    ref = json.loads((REPO / REF_RECEIPT).read_text(encoding="utf-8-sig"))["command"]
    copy = json.loads((LOGS / "receipt-p9-b0g-yf0-nav.json").read_text(encoding="utf-8-sig"))["command"]
    if ref != copy:
        raise SystemExit("STOP: the reference receipt differs from its gate-logs copy")
    return ref


def pairs(argv: list[str]) -> list[tuple[str, str | None]]:
    out, i = [], 2                                       # after <exe> nav
    while i < len(argv):
        t = argv[i]
        if not t.startswith("--"):
            raise SystemExit(f"STOP: stray token {t!r} at {i}")
        if i + 1 < len(argv) and not argv[i + 1].startswith("--"):
            out.append((t, argv[i + 1]))
            i += 2
        else:
            out.append((t, None))
            i += 1
    return out


def token_report(ref: list[str], cmd: list[str]) -> list[str]:
    lines = [f"reference receipt of record {REF_RECEIPT} command: {len(ref)} tokens; argv of record: {len(cmd)} tokens"]
    pos = [(i, ref[i] if i < len(ref) else None, cmd[i] if i < len(cmd) else None)
           for i in range(max(len(ref), len(cmd))) if (ref[i] if i < len(ref) else None) != (cmd[i] if i < len(cmd) else None)]
    lines.append(f"positional token differences ({len(pos)}):")
    lines += [f"  [{i}] reference {a!r} | argv of record {b!r}" for i, a, b in pos]
    pr, pc = pairs(ref), pairs(cmd)
    dr, dc = dict(pr), dict(pc)
    if len(dr) != len(pr) or len(dc) != len(pc):
        raise SystemExit("STOP: an option occurs twice")
    opts = sorted(set(dr) | set(dc))
    vd = [o for o in opts if o not in dr or o not in dc or dr[o] != dc[o]]
    lines.append(f"option / value pairs: reference {len(pr)}, argv of record {len(pc)}; options on one side only: "
                 f"{sorted(set(dr) ^ set(dc))}; options whose value differs: {vd}")
    for o in vd:
        lines.append(f"  {o}: reference {dr.get(o)!r} | argv of record {dc.get(o)!r}")
    order_r, order_c = [o for o, _ in pr], [o for o, _ in pc]
    k = next((i for i, (a, b) in enumerate(zip(order_r, order_c)) if a != b), None)
    if k is None:
        lines.append("option order: identical")
    else:
        lines.append(f"option order: identical up to option #{k}; from there reference {order_r[k:]} | argv of record "
                     f"{order_c[k:]} (the same options: {sorted(order_r[k:]) == sorted(order_c[k:])})")
    return lines


def checks(cmd: list[str]) -> list[str]:
    out, bad = [], []
    cache = [t for t in cmd if t.startswith("--candidate-cache") or t.endswith("-cache-index")]
    (bad if cache else out).append(f"no --candidate-cache and no cache-index flag (GATE-IDX / SQL-11): {not cache}")
    for f in FLAGS:
        n = cmd.count(f)
        (bad if n != 1 else out).append(f"{f} occurs {n} time(s) (want 1)")
    for f in ("--seal-allow", "--stage-timers"):
        (bad if f in cmd else out).append(f"{f} absent (exe-direct: no driver seal scan, SEAL-REF; timings off): "
                                          f"{f not in cmd}")
    comb = cmd[cmd.index("--combined") + 1]
    pin = cmd[cmd.index("--combined-sha256") + 1]
    got = sha_file(REPO / comb)
    (bad if got != pin else out).append(f"--combined {comb} re-hashed {got} {'=' if got == pin else '!='} "
                                        f"--combined-sha256 {pin}")
    want = json.loads((REPO / EXE_RECEIPT).read_text(encoding="utf-8-sig"))["Executables"]
    want = want["atx-equity-strategy-targets"]["Sha256"]
    exe = sha_file(Path(cmd[0]))
    (bad if exe != want else out).append(f"exe {cmd[0]} sha256 {exe} {'=' if exe == want else '!='} p9-3f receipt")
    for d in (NEW, RUN_DIR):
        (bad if (REPO / d).exists() else out).append(f"{d} absent before the run: {not (REPO / d).exists()}")
    return out, bad


def cmd_plan() -> int:
    import research_cycle as RC  # noqa: PLC0415
    from cycle.cli import make_cycle  # noqa: PLC0415
    from cycle.resolver import Resolver  # noqa: PLC0415
    head = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True).stdout.strip()
    print(f"# fr_nav.py plan {dt.datetime.now(dt.timezone.utc):%Y-%m-%dT%H:%M:%SZ} head {head}; parent {PARENT} sha256 "
          f"{sha_file(REPO / PARENT)}")
    args, cmd, spec = planned()
    print(f"probe template (in memory): {json.dumps(PROBE['change'])} over {PARENT}; resolved nav.rule "
          f"{spec['nav']['rule']}, nav.leverage {spec['nav']['leverage']}, nav.flags ({len(spec['nav']['flags'])}): "
          f"{json.dumps(spec['nav']['flags'])}")
    print(f"driver NAV argv after the runner's '--' (phases.py nav_step), {len(args)} tokens:\n  {json.dumps(args)}")
    print(f"argv of record (token 0 = exes.nav resolved against the pool root), {len(cmd)} tokens:\n  {json.dumps(cmd)}")
    print(f"argv sha256 (research_tree.argv_sha256 of command[1:]; the receipt's argv_sha256 and a C-13 binding's "
          f"digest): {argv_digest(cmd[1:])}")
    for line in token_report(ref_command(), cmd):
        print(line)
    ok, bad = checks(cmd)
    for line in ok:
        print("  check ok: " + line)
    for line in bad:
        print("  CHECK FAILS: " + line)
    for child in CHILDREN:
        c = make_cycle(Resolver(REPO), spec_path=REPO / child)
        st = next(s for s in c.steps() if s.phase == "nav")
        a = nav_args(st)
        fl = a[a.index("--rule") + 2:]
        print(f"{child}: planned NAV flags after --rule ({len(fl)}): {json.dumps(fl)}; " +
              "; ".join(f"{f} x{a.count(f)}" for f in FLAGS) + f"; argv sha256 {argv_digest(a[1:])}")
    return 1 if bad else 0


def json_leaves(o, p=""):
    if isinstance(o, dict):
        if not o:
            yield p + "/{}", None
        for k, v in o.items():
            yield from json_leaves(v, f"{p}/{k}")
    elif isinstance(o, list):
        if not o:
            yield p + "/[]", None
        for i, v in enumerate(o):
            yield from json_leaves(v, f"{p}/{i}")
    else:
        yield p, o


def jdiff(x, y) -> list[str]:
    lx, ly = dict(json_leaves(x)), dict(json_leaves(y))
    return sorted(k for k in set(lx) | set(ly)
                  if k not in lx or k not in ly or lx[k] != ly[k] or type(lx[k]) is not type(ly[k]))


def cmd_printspec() -> int:
    _, cmd, _ = planned()
    ref = list(ref_command())
    ref[ref.index("--output") + 1] = NEW
    docs, rc = {}, 0
    for tag, argv in (("plan", cmd), ("ref", ref)):
        done = subprocess.run([*argv, "--print-spec"], cwd=REPO, capture_output=True)
        out = LOGS / f"flags-navspec-{tag}.json"
        out.write_bytes(done.stdout)
        print(f"print-spec {tag}: exit {done.returncode}, stderr bytes {len(done.stderr)}, stdout {len(done.stdout)} "
              f"bytes sha256 {hashlib.sha256(done.stdout).hexdigest()} -> {out.name}")
        rc = max(rc, done.returncode)
        docs[tag] = json.loads(done.stdout.decode("utf-8")) if done.returncode == 0 else None
    print(f"{NEW} created by print-spec: {(REPO / NEW).exists()}; {RUN_DIR}: {(REPO / RUN_DIR).exists()}")
    if docs["plan"] is not None and docs["ref"] is not None:
        d = jdiff(docs["ref"], docs["plan"])
        print(f"NavSpec of the argv of record vs NavSpec of the reference command (--output set to {NEW}): "
              f"{len(d)} differing leaf path(s): {d}")
        for f in ("ann_mean_gross", "info_clock_print"):
            print(f"  options.{f}: present in both {all(f in (docs[t].get('options') or {}) for t in docs)}, "
                  f"equal {docs['plan'].get('options', {}).get(f) == docs['ref'].get('options', {}).get(f)}")
    return rc


def free_mib() -> int:
    import psutil  # noqa: PLC0415  (the runner requires it too)
    return psutil.virtual_memory().available // 2**20


def cmd_run() -> int:
    args, cmd, _ = planned()
    ref = ref_command()
    ok, bad = checks(cmd)
    if bad:
        raise SystemExit("STOP before the run: " + "; ".join(bad))
    before = sha_file(Path(cmd[0]))
    disk = shutil.disk_usage("C:/").free / 2**30
    if disk < 15:
        raise SystemExit(f"BLOCKED-HOST: C: {disk:.1f} GB free (< 15)")
    t0 = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    ram = free_mib()
    runner = [PY, "scripts/run_bounded_research.py", "--output", RUN_DIR, "--seconds", "600", "--max-rss-mib", "2048",
              "--build-type", "Release", "--admission-wait-seconds", "1800", "--", *cmd]
    print("runner argv: " + json.dumps(runner))
    with (LOGS / "runner-p9-fr-yf0-nav.log").open("wb") as fh:
        rc = subprocess.run(runner, cwd=REPO, stdout=fh, stderr=subprocess.STDOUT,
                            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}).returncode
    after = sha_file(Path(cmd[0]))
    line = (f"{t0} p9-fr-yf0-nav: exe sha256 {before} (p9-3f receipt =); free RAM before {ram} MiB; C: free "
            f"{disk:.1f} GB; runner-exit {rc}; exe after {'=' if after == before else '!= ' + after}")
    rec = REPO / RUN_DIR / "receipt.json"
    if rec.is_file():
        shutil.copyfile(rec, LOGS / "receipt-p9-fr-yf0-nav.json")
        r = json.loads(rec.read_text(encoding="utf-8"))
        s_path = REPO / RUN_DIR / "start.json"
        s = json.loads(s_path.read_text(encoding="utf-8")) if s_path.is_file() else {}
        line += (f" :: outcome {r.get('outcome')} exit {r.get('exit_code')} wall_s {r.get('wall_seconds')} peak_MiB "
                 f"{round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20)} receipt-exe "
                 f"{r.get('executable_sha256')} source {r.get('source_sha')} build_type {r.get('build_type')} "
                 f"argv_sha256 {r.get('argv_sha256')} (planned {argv_digest(cmd[1:])}) git {r.get('git')!r} "
                 f"store-block receipt {'store' in r} start {'store' in s}; receipt command = argv of record "
                 f"{r.get('command') == cmd}; positional differences vs the reference receipt "
                 f"{sum(1 for a, b in zip(ref, r.get('command') or []) if a != b)}")
        if r.get("exit_code") == 3221225794:
            line += " | ENV-1: exit 3221225794 (0xC0000142): VOID; re-run once on a quiet host"
    od = REPO / NEW
    if od.is_dir():
        files = sorted(p for p in od.rglob("*") if p.is_file())
        rows = [f"{p.relative_to(od).as_posix()}\t{p.stat().st_size}\t{sha_file(p)}" for p in files]
        text = "\n".join(rows) + "\n"
        (LOGS / "flags-files-p9-fr-yf0-nav.txt").write_text(text, encoding="utf-8", newline="\n")
        line += f" | outputs {len(rows)} files, listing sha256 {hashlib.sha256(text.encode()).hexdigest()}"
    print(line)
    with (LOGS / "flags-run.txt").open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")
    return rc


PRODUCER = re.compile(r"^/producer/")
OPTIN = re.compile(r"^/scenarios/\d+/(ann_mean_gross$|info_clock(/|$))")


def files_of(root: Path) -> dict[str, Path]:
    return {p.relative_to(root).as_posix(): p for p in sorted(root.rglob("*")) if p.is_file()}


def cmd_cmp() -> int:
    ref, new = REPO / REF_DIR, REPO / NEW
    fa, fb = files_of(ref), files_of(new)
    print(f"# fr_nav.py cmp {dt.datetime.now(dt.timezone.utc):%Y-%m-%dT%H:%M:%SZ}: reference {REF_DIR} ({len(fa)} files) "
          f"vs new {NEW} ({len(fb)} files); allowed differing leaves: summary.json /producer/* ONLY (FLAGS-ROUTE)")
    same = differ = outside = 0
    for rel in sorted(set(fa) | set(fb)):
        if rel not in fa or rel not in fb:
            print(f"ONE-SIDED {rel} ({'new only' if rel in fb else 'reference only'}) OUTSIDE")
            outside += 1
            continue
        sa, sb = sha_file(fa[rel]), sha_file(fb[rel])
        if sa == sb:
            same += 1
            print(f"same   {rel} {sa}")
            continue
        differ += 1
        if not rel.endswith(".json"):
            print(f"DIFF   {rel} {sa[:16]} vs {sb[:16]} (not JSON) OUTSIDE")
            outside += 1
            continue
        da = json.loads(fa[rel].read_text(encoding="utf-8"))
        db = json.loads(fb[rel].read_text(encoding="utf-8"))
        d = jdiff(da, db)
        allowed = [q for q in d if rel == "summary.json" and PRODUCER.search(q)]
        out = [q for q in d if q not in allowed]
        print(f"DIFF   {rel} {sa[:16]} vs {sb[:16]}: {len(d)} differing leaf path(s), {len(allowed)} allowed, "
              f"{len(out)} outside")
        for q in allowed:
            print(f"         ALLOWED {q}")
        for q in out:
            print(f"         OUTSIDE {q}")
        outside += len(out)
    sj = {t: json.loads((root / "summary.json").read_text(encoding="utf-8")) for t, root in (("ref", ref), ("new", new))
          if (root / "summary.json").is_file()}
    if len(sj) == 2:
        la, lb = dict(json_leaves(sj["ref"])), dict(json_leaves(sj["new"]))
        oa = sorted(k for k in la if OPTIN.search(k))
        ob = sorted(k for k in lb if OPTIN.search(k))
        kinds = lambda ks: dict(Counter("ann_mean_gross" if k.endswith("/ann_mean_gross") else "info_clock" for k in ks))  # noqa: E731
        eq = oa == ob and all(la[k] == lb[k] and type(la[k]) is type(lb[k]) for k in oa)
        print(f"opt-in leaves (/scenarios/<i>/ann_mean_gross, /scenarios/<i>/info_clock/**): reference {len(oa)} "
              f"{kinds(oa)}, new {len(ob)} {kinds(ob)}; same paths and equal values on both sides: {eq}")
        print(f"producer leaves: reference {sorted(k for k in la if PRODUCER.search(k))}; new "
              f"{sorted(k for k in lb if PRODUCER.search(k))}")
    print(f"files byte-identical {same}, differing {differ}; TOTAL outside the allowed list: {outside}")
    print("VERDICT: " + ("HOLDS (identical outside /producer/*)" if outside == 0 and set(fa) == set(fb) else "STOP"))
    return 0 if outside == 0 else 1


if __name__ == "__main__":
    verb = sys.argv[1] if len(sys.argv) > 1 else ""
    fn = {"plan": cmd_plan, "printspec": cmd_printspec, "run": cmd_run, "cmp": cmd_cmp}.get(verb)
    if fn is None:
        raise SystemExit(__doc__)
    raise SystemExit(fn())
