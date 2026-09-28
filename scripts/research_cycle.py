#!/usr/bin/env python3
"""One research cycle driven by a spec file (platform v7 lane L2): replaces the hand-copied vNN_train.sh ladders.

  research_cycle.py plan   SPEC [--root R] [--suffix S] [--attempt PHASE=N ...] [--reuse-fields DIR] [--ledger PATH]
                                [--lines-only]
  research_cycle.py run    SPEC [same options] [--stop-after PHASE]
  research_cycle.py status SPEC [same options]
  research_cycle.py lock   SPEC [--root R] [--relock] [--write]

SPEC is a JSON file (``atx.research-cycle-spec/v1``; a relative SPEC not found from the current directory is looked
up next to this script, so ``specs/v61.json`` works from the worktree root). Paths inside it are relative to --root
(default: this script's worktree). Phases, in order (a phase the spec leaves out is skipped):

  fields  prepare_research_fields.py (direct: the builder carries its own RSS/time caps), optional --reuse; then the
          fields check (baseline fields byte-identical, the expected fields added, short-volume file-set pin)
  check   the library static check (direct)
  u       IC runner, unweighted pass            (bounded runner; output U-<attempt>, receipt U-run<attempt>)
  fit     fit_composition_weights.py            (bounded; output W, receipt W-run<pass>; exit 3 = documented
          "incomplete, rerun resumes" protocol -> the next pass runs, up to fit.max_passes; anything else stops)
  gate    admission read-out of admission.json (internal; exit 10 when a required candidate is not admitted with
          its prior sign)
  w       IC runner, weighted pass              (bounded; output WT-<attempt>, receipt WT-run<attempt>)
  nav     NAV replay                            (bounded; output N, receipt N-run)
  summ    nav_summ.py vs the reference cell with DSR N (direct)

Pins: every input (library, recipe, baseline library, role, identity bridge, fundamental events, baseline fields,
reference admission, reference cell) is pinned in the spec by ``lock``, which computes the SHA-256 from the file
(never hand-typed); ``plan``/``run`` re-hash and stop on any mismatch (exit 3). Every intermediate pin (fields
manifest, orientations, runner summary, weights, combined signal) is computed from the file the previous phase wrote.

``plan`` prints the exact command lines with every pin resolved (``# `` lines are annotations; --lines-only prints
the commands alone) and executes nothing. ``run`` executes phase by phase through scripts/run_bounded_research.py
(spec caps: 180 s / 1536 MiB / 512 MiB free), resumes from the first phase whose output is missing, never overwrites
an output (a failed attempt stays on disk: rerun with ``--attempt PHASE=N+1`` or a fresh ``--suffix``), reads every
receipt and HARD-STOPS (exit 4) on a non-zero exit or any refusal (prelaunch-memory-refusal, rss/time/system-memory
limit, runner error). The tree must be clean (the runner refuses otherwise). ``status`` shows each phase's state and
receipt. ``--suffix S`` appends ``-S`` to every output name (fields, cache, U, fit work, W, WT, N) for an identity
re-run; ``--reuse-fields DIR`` passes ``--reuse DIR --reuse-sha256 <pin>`` to the fields builder; ``--ledger PATH``
passes ``--ledger PATH --ledger-kind <summ.ledger_kind>`` to nav_summ.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

SCHEMA = "atx.research-cycle-spec/v1"
PHASES = ("fields", "check", "u", "fit", "gate", "w", "nav", "summ")
ATTEMPT_PHASES = ("u", "fit", "w", "nav")
EXIT_OK, EXIT_USAGE, EXIT_PIN, EXIT_STOP, EXIT_GATE = 0, 2, 3, 4, 10
FIT_INCOMPLETE = 3                     # fit_composition_weights.py: "incomplete (rerun resumes)"
MAX_ATTEMPTS = 9
REQUIRED = {"schema", "name", "python", "runner", "inputs"}
INPUT_KEYS = ("library", "recipe", "baseline_library", "role", "identity_bridge", "fund_events", "baseline_fields",
              "reference_admission", "reference_cell")
SOURCE_FLAGS = (("finra", "--finra"), ("tickerhistory", "--tickerhistory"), ("lake", "--lake"),
                ("finra_short_volume", "--finra-short-volume"))


class CycleError(Exception):
    def __init__(self, message: str, code: int = EXIT_STOP):
        super().__init__(message)
        self.code = code


def fmt_argv(argv: list[str]) -> str:
    """One command line: arguments joined by a space, an argument holding a space double-quoted (bash DRY format)."""
    return " ".join(f'"{a}"' if " " in a else a for a in argv)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------ spec
def find_spec(arg: str) -> Path:
    p = Path(arg)
    if p.exists():
        return p
    alt = Path(__file__).resolve().parent / arg
    if alt.exists():
        return alt
    raise CycleError(f"spec not found: {arg}", EXIT_USAGE)


def load_spec(path: Path) -> dict:
    try:
        spec = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CycleError(f"spec {path}: {exc}", EXIT_USAGE) from exc
    validate_spec(spec)
    return spec


def validate_spec(spec: dict) -> None:
    if not isinstance(spec, dict) or spec.get("schema") != SCHEMA:
        raise CycleError(f"spec schema must be {SCHEMA}", EXIT_USAGE)
    missing = sorted(REQUIRED - set(spec))
    if missing:
        raise CycleError(f"spec misses {', '.join(missing)}", EXIT_USAGE)
    unknown = sorted(set(spec["inputs"]) - set(INPUT_KEYS))
    if unknown:
        raise CycleError(f"spec inputs: unknown key(s) {', '.join(unknown)} (known: {', '.join(INPUT_KEYS)})",
                         EXIT_USAGE)
    for key, item in spec["inputs"].items():
        if not isinstance(item, dict) or "path" not in item or "sha256" not in item:
            raise CycleError(f"spec inputs.{key} needs path and sha256 (null until `lock`)", EXIT_USAGE)
    r = spec["runner"]
    if not all(k in r for k in ("script", "seconds", "max_rss_mib", "min_free_mib")):
        raise CycleError("spec runner needs script, seconds, max_rss_mib, min_free_mib", EXIT_USAGE)
    need = {"fields": ("output", "builder", "list"), "static_check": ("script",), "ic": ("u_output", "cache", "flags"),
            "fit": ("script", "output", "work_dir", "flags"), "gate": ("admitted",), "nav": ("output", "rule", "flags"),
            "summ": ("script", "dsr_n")}
    for section, keys in need.items():
        if section in spec and not all(k in spec[section] for k in keys):
            raise CycleError(f"spec {section} needs {', '.join(keys)}", EXIT_USAGE)
    if "nav" in spec and "w_output" not in spec.get("ic", {}):
        raise CycleError("spec nav needs ic.w_output (the weighted pass feeds the NAV)", EXIT_USAGE)
    if ("fit" in spec and "ic" not in spec) or ("gate" in spec and "fit" not in spec) or \
            (("ic" in spec or "static_check" in spec) and "fields" not in spec):
        raise CycleError("spec: fit needs ic; gate needs fit; ic and static_check need fields", EXIT_USAGE)
    if ("ic" in spec and "ic" not in spec.get("exes", {})) or ("nav" in spec and "nav" not in spec.get("exes", {})):
        raise CycleError("spec exes needs ic (for ic) and nav (for nav)", EXIT_USAGE)
    for key in ("role", "library"):
        if "ic" in spec and key not in spec["inputs"]:
            raise CycleError(f"spec inputs needs {key}", EXIT_USAGE)
    if "summ" in spec and "nav" not in spec:
        raise CycleError("spec summ needs nav", EXIT_USAGE)
    if "static_check" in spec and "baseline_library" not in spec["inputs"]:
        raise CycleError("spec static_check needs inputs.baseline_library", EXIT_USAGE)


def parse_attempts(items: list[str]) -> dict:
    out = {}
    for item in items or []:
        phase, _, n = item.partition("=")
        if phase not in ATTEMPT_PHASES or not n.isdigit() or not 1 <= int(n) <= MAX_ATTEMPTS:
            raise CycleError(f"--attempt {item}: expected PHASE=N, PHASE in {', '.join(ATTEMPT_PHASES)}, "
                             f"1 <= N <= {MAX_ATTEMPTS}", EXIT_USAGE)
        out[phase] = int(n)
    return out


# ------------------------------------------------------------------ files under the root
class Resolver:
    """Root-relative files: existence, SHA-256 (cached) and JSON. ``known`` = {relpath: sha256} stands in for files
    that are not on disk (hash-only mode for fixtures: content checks are skipped and flagged)."""

    def __init__(self, root: Path, known: dict | None = None):
        self.root = Path(root)
        self.known = dict(known or {})
        self._sha: dict = {}

    def path(self, rel: str) -> Path:
        p = Path(rel)
        return p if p.is_absolute() else self.root / p

    def exists(self, rel: str) -> bool:
        return rel in self.known or self.path(rel).is_file()

    def exists_dir(self, rel: str) -> bool:
        pre = rel.rstrip("/") + "/"
        return self.path(rel).is_dir() or any(k.startswith(pre) for k in self.known)

    def sha(self, rel: str) -> str | None:
        if rel in self.known:
            return self.known[rel]
        if rel not in self._sha:  # only existing files are cached: outputs appear while a cycle runs
            p = self.path(rel)
            if not p.is_file():
                return None
            self._sha[rel] = sha256_file(p)
        return self._sha[rel]

    def read_json(self, rel: str):
        p = self.path(rel)
        if not p.is_file():
            return None
        return json.loads(p.read_text(encoding="utf-8"))

    def hash_only(self, rel: str) -> bool:
        return rel in self.known and not self.path(rel).is_file()


def placeholder(rel: str) -> str:
    return f"<sha256:{rel}>"


# ------------------------------------------------------------------ the cycle
class Step:
    def __init__(self, phase: str, kind: str, argv=None, output=None, run_dir=None, attempt=None, state="pending",
                 note=""):
        self.phase, self.kind, self.argv, self.output, self.run_dir = phase, kind, argv, output, run_dir
        self.attempt, self.state, self.note = attempt, state, note

    @property
    def done(self) -> bool:
        return self.state == "done"


class Cycle:
    def __init__(self, spec: dict, res: Resolver, *, suffix: str | None = None, attempts: dict | None = None,
                 reuse_fields: str | None = None, ledger: str | None = None, spec_path: Path | None = None):
        self.spec, self.res, self.suffix = spec, res, suffix
        self.attempts = dict(attempts or {})
        self.reuse_fields, self.ledger, self.spec_path = reuse_fields, ledger, spec_path
        self.py = spec["python"]
        self.pins = self.verify_inputs()

    # -------------------------------------------------------------- names and pins
    def out(self, name: str) -> str:
        return f"{name}-{self.suffix}" if self.suffix else name

    def verify_inputs(self) -> dict:
        """{key: (path, sha, how)}; a locked pin must equal the file's SHA-256 (else exit 3)."""
        pins = {}
        for key, item in self.spec["inputs"].items():
            rel, want = item["path"], item.get("sha256")
            got = self.res.sha(rel)
            if got is None:
                raise CycleError(f"input {key} missing: {rel}", EXIT_PIN)
            if want is None:
                pins[key] = (rel, got, "UNLOCKED (computed now; run `lock --write`)")
            elif want != got:
                raise CycleError(f"PIN MISMATCH {key}: {rel} is {got}, spec pins {want}", EXIT_PIN)
            else:
                pins[key] = (rel, got, "locked, verified" if not self.res.hash_only(rel) else "locked (hash-only)")
        role = self.spec["inputs"].get("role")
        if role and role.get("universe"):
            doc = self.res.read_json(role["path"])
            if doc is not None:
                got = (doc.get("universe") or {}).get("id")
                if got != role["universe"]:
                    raise CycleError(f"role universe is {got!r}, spec declares {role['universe']!r}", EXIT_PIN)
        return pins

    def pin(self, key: str) -> str:
        return self.pins[key][1]

    def ipath(self, key: str) -> str:
        return self.spec["inputs"][key]["path"]

    def idir(self, key: str) -> str:
        item = self.spec["inputs"][key]
        return item.get("dir") or str(Path(item["path"]).parent).replace("\\", "/")

    def rt_sha(self, rel: str) -> str:
        """An intermediate pin: the SHA-256 of a file an earlier phase wrote (placeholder until it exists)."""
        return self.res.sha(rel) or placeholder(rel)

    def runner(self, run_dir: str, binds: list[str]) -> list[str]:
        r = self.spec["runner"]
        argv = [self.py, r["script"], "--seconds", str(r["seconds"]), "--max-rss-mib", str(r["max_rss_mib"]),
                "--min-free-mib", str(r["min_free_mib"]), "--output", run_dir]
        for b in binds:
            argv += ["--bind", b]
        return argv + ["--"]

    # -------------------------------------------------------------- phase states
    def receipt(self, run_dir: str | None) -> dict | None:
        if not run_dir:
            return None
        return self.res.read_json(f"{run_dir}/receipt.json")

    def complete_summary(self, out: str) -> bool:
        rel = f"{out}/summary.json"
        if not self.res.exists(rel):
            return False
        if self.res.hash_only(rel):
            return True
        doc = self.res.read_json(rel)
        return isinstance(doc, dict) and doc.get("status") == "complete"

    def ic_attempt(self, phase: str, base: str) -> tuple[int, str, str]:
        """(attempt, state, note) of an IC pass: the explicit --attempt, else the lowest complete attempt, else a
        fresh attempt 1; a failed attempt on disk without an explicit --attempt is a stop (never auto-retried)."""
        if phase in self.attempts:
            n = self.attempts[phase]
            if self.complete_summary(f"{base}-{n}"):
                return n, "done", ""
            if self.res.exists_dir(f"{base}-{n}") or self.res.exists_dir(f"{base}-run{n}"):
                return n, "failed", self.failure_note(f"{base}-run{n}")
            return n, "pending", ""
        tried = 0
        for n in range(1, MAX_ATTEMPTS + 1):
            if self.complete_summary(f"{base}-{n}"):
                return n, "done", ""
            if self.res.exists_dir(f"{base}-{n}") or self.res.exists_dir(f"{base}-run{n}"):
                tried = n
        if tried:
            return tried, "failed", (self.failure_note(f"{base}-run{tried}") +
                                     f"; never overwritten: rerun with --attempt {phase}={tried + 1}")
        return 1, "pending", ""

    def failure_note(self, run_dir: str) -> str:
        r = self.receipt(run_dir)
        if r is None:
            return f"attempt left {run_dir} without a receipt"
        return f"receipt {run_dir}: outcome {r.get('outcome')}, exit_code {r.get('exit_code')}"

    # -------------------------------------------------------------- the steps, with every pin resolved
    def steps(self) -> list[Step]:
        s, out = self.spec, []
        role_m, role_sha = self.ipath("role"), self.pin("role")
        lib, lib_sha = self.ipath("library"), self.pin("library")
        fd = self.out(s["fields"]["output"]) if "fields" in s else ""   # validate_spec: ic/check need fields
        fdm = f"{fd}/manifest.json" if fd else ""
        if "fields" in s:
            out.append(self.fields_step(fd, fdm, role_sha))
        if "static_check" in s:
            sc = s["static_check"]
            argv = [self.py, sc["script"], "--manifest", fdm, "--library", lib, "--baseline",
                    self.ipath("baseline_library"), *sc.get("args", [])]
            if sc.get("expect_added"):
                argv += ["--expect-added", ",".join(sc["expect_added"])]
            out.append(Step("check", "direct", argv, state="always", note="library static check"))
        u_out = ""
        if "ic" in s:
            ic = s["ic"]
            base = self.out(ic["u_output"])
            n, state, note = self.ic_attempt("u", base)
            u_out, run_dir = f"{base}-{n}", f"{base}-run{n}"
            argv = self.runner(run_dir, [s["exes"]["ic"], lib, role_m, fdm]) + [
                s["exes"]["ic"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
                role_sha, "--train-fields", fd, "--train-fields-sha256", self.rt_sha(fdm), "--output", u_out,
                *ic["flags"], "--candidate-cache", self.out(ic["cache"])]
            out.append(Step("u", "bounded", argv, u_out, run_dir, n, state, note))
        w_dir = ""
        if "fit" in s:
            step, w_dir = self.fit_step(u_out, lib, lib_sha, role_m, role_sha)
            out.append(step)
        if "gate" in s:
            out.append(Step("gate", "internal", None, state="always",
                            note=f"admission read-out of {w_dir}/admission.json"))
        wt_out = ""
        if "ic" in s and "w_output" in s["ic"]:
            ic = s["ic"]
            base = self.out(ic["w_output"])
            n, state, note = self.ic_attempt("w", base)
            wt_out, run_dir = f"{base}-{n}", f"{base}-run{n}"
            weights = f"{w_dir}/composition_weights.json"
            argv = self.runner(run_dir, [s["exes"]["ic"], role_m, fdm, lib, weights]) + [
                s["exes"]["ic"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
                role_sha, "--train-fields", fd, "--train-fields-sha256", self.rt_sha(fdm), "--output", wt_out,
                *ic["flags"], "--candidate-cache", self.out(ic["cache"]), "--composition-weights", weights,
                "--composition-weights-sha256", self.rt_sha(weights)]
            out.append(Step("w", "bounded", argv, wt_out, run_dir, n, state, note))
        n_out = ""
        if "nav" in s:
            nav = s["nav"]
            n_out = self.out(nav["output"])
            k = self.attempts.get("nav", 1)
            run_dir = f"{n_out}-run" if k == 1 else f"{n_out}-run{k}"
            comb = f"{wt_out}/train_combined.json"
            flags = [str(nav.get("leverage")) if f == "{leverage}" else f for f in nav["flags"]]
            argv = self.runner(run_dir, [s["exes"]["nav"], comb, fdm]) + [
                s["exes"]["nav"], "nav", "--combined", comb, "--combined-sha256", self.rt_sha(comb), "--role", role_m,
                "--role-sha256", role_sha, "--fields", fdm, "--fields-sha256", self.rt_sha(fdm), "--output", n_out,
                "--rule", nav["rule"], *flags]
            if self.res.exists(f"{n_out}/summary.json"):
                state, note = "done", ""
            elif self.res.exists_dir(n_out) or self.res.exists_dir(run_dir):
                state, note = "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
            else:
                state, note = "pending", ""
            out.append(Step("nav", "bounded", argv, n_out, run_dir, k, state, note))
        if "summ" in s:
            sm = s["summ"]
            argv = [self.py, sm["script"]]
            if w_dir:
                argv += ["--weights", f"{w_dir}/composition_weights.json"]
            if "reference_cell" in s["inputs"]:
                argv += ["--reference", self.idir("reference_cell")]
            argv += ["--dsr-n", str(sm["dsr_n"]), *sm.get("extra", [])]
            if self.ledger:
                argv += ["--ledger", self.ledger, "--ledger-kind", sm.get("ledger_kind", "construction")]
            argv.append(n_out)
            out.append(Step("summ", "direct", argv, state="always", note="nav_summ vs the reference cell"))
        return out

    def fields_step(self, fd: str, fdm: str, role_sha: str) -> Step:
        f, s = self.spec["fields"], self.spec
        argv = [self.py, f["builder"], "--role", self.idir("role"), "--role-sha256", role_sha, "--output", fd,
                "--fields", ",".join(f["list"])]
        for key, flag in SOURCE_FLAGS:
            if key in f.get("sources", {}):
                argv += [flag, f["sources"][key]]
        if "identity_bridge" in s["inputs"]:
            argv += ["--identity-bridge", self.idir("identity_bridge"), "--identity-bridge-sha256",
                     self.pin("identity_bridge")]
        if "fund_events" in s["inputs"]:
            argv += ["--fund-events", self.idir("fund_events"), "--fund-events-sha256", self.pin("fund_events")]
        if "fund_lag_sessions" in f:
            argv += ["--fund-lag-sessions", str(f["fund_lag_sessions"])]
        argv += ["--max-rss-mib", str(f.get("max_rss_mib", 1536)), "--max-seconds", str(f.get("max_seconds", 1800))]
        if self.reuse_fields:
            rel = f"{self.reuse_fields.rstrip('/')}/manifest.json"
            sha = self.res.sha(rel)
            if sha is None:
                raise CycleError(f"--reuse-fields: no manifest at {rel}", EXIT_PIN)
            argv += ["--reuse", self.reuse_fields.rstrip("/"), "--reuse-sha256", sha]
        if self.res.exists(fdm):
            state, note = "done", ""
        elif self.res.exists_dir(fd):
            state, note = "failed", f"partial fields dir exists (never overwritten): {fd}; use a fresh --suffix"
        else:
            state, note = "pending", ""
        return Step("fields", "direct", argv, fd, None, None, state, note)

    def fit_step(self, u_out: str, lib: str, lib_sha: str, role_m: str, role_sha: str) -> tuple[Step, str]:
        s, fit = self.spec, self.spec["fit"]
        w_dir = self.out(fit["output"])
        o, sm = f"{u_out}/orientations.json", f"{u_out}/summary.json"
        max_passes = int(fit.get("max_passes", 3))
        runs = [j for j in range(1, MAX_ATTEMPTS + 1) if self.res.exists_dir(f"{w_dir}-run{j}")]
        if "fit" in self.attempts:
            j = self.attempts["fit"]
        elif self.res.exists(f"{w_dir}/composition_weights.json"):
            j = runs[-1] if runs else 1
        elif runs:
            last = self.receipt(f"{w_dir}-run{runs[-1]}")
            j = runs[-1] + 1 if last and last.get("exit_code") == FIT_INCOMPLETE else runs[-1]
        else:
            j = 1
        run_dir = f"{w_dir}-run{j}"
        if self.res.exists(f"{w_dir}/composition_weights.json"):
            state, note = "done", ""
        elif self.res.exists_dir(run_dir):
            state, note = "failed", self.failure_note(run_dir) + "; never overwritten"
        elif j > max_passes:
            state, note = "failed", f"fit still incomplete after {max_passes} passes"
        else:
            state, note = "pending", "" if j == 1 else f"resume pass {j} (previous pass exited {FIT_INCOMPLETE})"
        argv = self.runner(run_dir, [lib, self.ipath("recipe"), role_m, o, sm]) + [
            self.py, fit["script"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
            role_sha, "--orientations", o, "--orientations-sha256", self.rt_sha(o), "--runner-summary", sm,
            "--runner-summary-sha256", self.rt_sha(sm), *fit["flags"]]
        if "recipe" in s["inputs"]:
            argv += ["--recipe", self.ipath("recipe"), "--recipe-sha256", self.pin("recipe")]
        argv += ["--work-dir", self.out(fit["work_dir"])]
        if "max_seconds" in fit:
            argv += ["--max-seconds", str(fit["max_seconds"])]
        argv += ["--output", w_dir]
        return Step("fit", "bounded", argv, w_dir, run_dir, j, state, note), w_dir


# ------------------------------------------------------------------ internal checks
def fields_check(cycle: Cycle, fdm: str, log=print) -> None:
    chk = cycle.spec["fields"].get("check")
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
    ok = True
    for cid in g["admitted"]:
        r = rows.get(cid)
        if r is None:
            log(f"gate {g.get('name', 'gate')} {cid}: not in admission.json")
            ok = False
            continue
        log(f"gate {g.get('name', 'gate')} {cid}: status {r['status']} failed {r.get('failed_checks')}; runner sign "
            f"{r.get('runner_sign')} vs prior {r.get('s_k')} (agrees {r.get('sign_agrees')}); HAC t {fmt(r.get('hac_t'))}; "
            f"tau {fmt(r.get('tau'))} (limit {adm.get('rules', {}).get('tau_limit')}); max |rho| "
            f"{fmt(r.get('max_abs_rho'))} with {r.get('max_abs_rho_with')}; redundant_with {r.get('redundant_with')}; "
            f"train days {r.get('train_days')}")
        ok = ok and r["status"] == "admitted" and (not g.get("sign_agrees", True) or r.get("sign_agrees") is True)
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
    log(f"gate {g.get('name', 'gate')} " + ("PASS" if ok else "FAIL: stop (no later phase runs)"))
    if not ok:
        raise CycleError("gate failed", EXIT_GATE)


# ------------------------------------------------------------------ plan / status / run
def header(cycle: Cycle) -> list[str]:
    spec_sha = sha256_file(cycle.spec_path) if cycle.spec_path else None
    lines = [f"# research_cycle {cycle.spec['name']}: spec {cycle.spec_path} sha256 {spec_sha}; root {cycle.res.root}; "
             f"suffix {cycle.suffix or 'none'}; attempts {cycle.attempts or 'auto'}"]
    for key, (rel, sha, how) in cycle.pins.items():
        lines.append(f"# pin {key}: {rel} {sha} [{how}]")
    return lines


def plan_lines(cycle: Cycle, lines_only: bool = False) -> list[str]:
    out = [] if lines_only else header(cycle)
    for st in cycle.steps():
        if not lines_only:
            where = f" -> {st.output}" if st.output else ""
            rd = f" (receipt {st.run_dir})" if st.run_dir else ""
            out.append(f"# phase {st.phase} [{st.kind}; {st.state}{'; ' + st.note if st.note else ''}]{where}{rd}")
        if st.argv:
            out.append(fmt_argv(st.argv))
    return out


def status_lines(cycle: Cycle) -> list[str]:
    out = header(cycle)
    for st in cycle.steps():
        line = f"{st.phase:6s} {st.state:8s}"
        if st.output:
            line += f" {st.output}"
        r = cycle.receipt(st.run_dir)
        if r is not None:
            peak = (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20
            line += (f" | receipt {st.run_dir}: {r.get('outcome')} exit {r.get('exit_code')} "
                     f"{r.get('wall_seconds', 0):.1f} s peak {peak} MiB")
        if st.note:
            line += f" | {st.note}"
        out.append(line)
        if st.phase == "nav" and st.done:
            summ = cycle.res.read_json(f"{st.output}/summary.json") or {}
            scen = summ.get("primary_scenario")
            csv = f"{st.output}/daily_{scen}.csv"
            if scen and cycle.res.exists(csv):
                out.append(f"       primary daily CSV {csv} sha256 {cycle.res.sha(csv)}")
    return out


def git_clean(root: Path) -> bool:
    try:
        done = subprocess.run(["git", "status", "--porcelain"], cwd=root, capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    return not done.stdout.strip()


def execute(argv: list[str], root: Path, env: dict, capture: bool) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=root, env=env, capture_output=capture, text=True)


def run_cycle(cycle: Cycle, *, stop_after: str | None = None, log=print, executor=execute,
              clean=git_clean) -> int:
    root = cycle.res.root
    if not clean(root):
        raise CycleError(f"tree at {root} is not clean: commit first (the bounded runner refuses a dirty tree)")
    env = dict(os.environ)
    pre = cycle.spec.get("env_path_prepend") or []
    if pre:
        env["PATH"] = os.pathsep.join([str(Path(p)) for p in pre] + [env.get("PATH", "")])
    for line in header(cycle):
        log(line)
    phase_idx = 0
    while True:
        steps = cycle.steps()           # re-resolved after every phase: later pins come from the files just written
        if phase_idx >= len(steps):
            break
        st = steps[phase_idx]
        if st.state == "failed":
            raise CycleError(f"HARD-STOP [{st.phase}]: {st.note}")
        if st.done:
            log(f"== {st.phase}: done ({st.output})")
            if st.phase == "fields":
                fields_check(cycle, f"{st.output}/manifest.json", log)
        elif st.kind == "internal":
            w_dir = next(x.output for x in steps if x.phase == "fit")
            gate(cycle, w_dir, log)
        else:
            if "<sha256:" in " ".join(st.argv):
                raise CycleError(f"HARD-STOP [{st.phase}]: an upstream pin is unresolved (upstream output missing)")
            if st.output and st.state == "pending" and st.phase != "fit" and cycle.res.exists_dir(st.output):
                raise CycleError(f"HARD-STOP [{st.phase}]: output {st.output} exists (never overwritten)")
            log(f"== {st.phase}" + (f" (attempt {st.attempt})" if st.attempt else ""))
            log(fmt_argv(st.argv))
            done = executor(st.argv, root, env, st.kind == "bounded")
            if st.kind == "bounded":
                r = cycle.receipt(st.run_dir)
                if r is None:
                    tail = (done.stderr or "")[-400:]
                    raise CycleError(f"HARD-STOP [{st.phase}]: no receipt in {st.run_dir} (runner exit "
                                     f"{done.returncode}) {tail}")
                peak = (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20
                log(f"   receipt: outcome {r.get('outcome')} exit {r.get('exit_code')} "
                    f"{r.get('wall_seconds', 0):.1f} s peak {peak} MiB")
                if st.phase == "fit" and r.get("exit_code") == FIT_INCOMPLETE and r.get("outcome") == "process-error":
                    if st.attempt >= int(cycle.spec["fit"].get("max_passes", 3)):
                        raise CycleError(f"HARD-STOP [fit]: still incomplete after {st.attempt} passes")
                    log("   fit incomplete (exit 3, the documented resume protocol): next pass resumes")
                    continue
                if r.get("outcome") != "completed" or r.get("exit_code") != 0:
                    raise CycleError(f"HARD-STOP [{st.phase}]: receipt {st.run_dir}: outcome {r.get('outcome')}, "
                                     f"exit_code {r.get('exit_code')}{', ' + r['error'] if r.get('error') else ''}")
            else:
                if st.kind == "direct" and done.returncode != 0:
                    raise CycleError(f"HARD-STOP [{st.phase}]: exit {done.returncode}")
            post = cycle.steps()[phase_idx]
            if st.phase in ("u", "w", "fit", "nav", "fields") and not post.done:
                raise CycleError(f"HARD-STOP [{st.phase}]: exit 0 but its output is incomplete ({st.output})")
            if st.phase == "fields":
                fields_check(cycle, f"{st.output}/manifest.json", log)
        if stop_after == st.phase:
            log(f"== stopped after {st.phase} (--stop-after)")
            return EXIT_OK
        phase_idx += 1
    log("== cycle complete")
    return EXIT_OK


# ------------------------------------------------------------------ lock
def lock(spec_path: Path, root: Path, relock: bool = False) -> tuple[dict, list[str]]:
    spec = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    validate_spec(spec)
    res = Resolver(root)
    notes = []
    for key, item in spec["inputs"].items():
        got = res.sha(item["path"])
        if got is None:
            raise CycleError(f"lock: input {key} missing: {item['path']}", EXIT_PIN)
        if item.get("sha256") in (None, got):
            if item.get("sha256") is None:
                notes.append(f"locked {key}: {item['path']} {got}")
            item["sha256"] = got
        elif relock:
            notes.append(f"RELOCKED {key}: {item['path']} {item['sha256']} -> {got}")
            item["sha256"] = got
        else:
            raise CycleError(f"lock: {key} pin {item['sha256']} differs from the file ({got}); --relock to replace",
                             EXIT_PIN)
    return spec, notes


# ------------------------------------------------------------------ CLI
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("verb", choices=("plan", "run", "status", "lock"))
    ap.add_argument("spec")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--suffix", default=None)
    ap.add_argument("--attempt", action="append", default=[], help="PHASE=N (u, fit, w, nav)")
    ap.add_argument("--reuse-fields", default=None, help="prior fields dir for prepare_research_fields --reuse")
    ap.add_argument("--ledger", default=None, help="trial ledger passed to nav_summ --ledger")
    ap.add_argument("--lines-only", action="store_true", help="plan: the command lines only")
    ap.add_argument("--stop-after", choices=PHASES, default=None)
    ap.add_argument("--relock", action="store_true", help="lock: replace pins that differ from the files")
    ap.add_argument("--write", action="store_true", help="lock: write the pins back into SPEC")
    a = ap.parse_args(argv)
    try:
        spec_path = find_spec(a.spec)
        if a.verb == "lock":
            spec, notes = lock(spec_path, a.root, a.relock)
            for n in notes:
                print(n)
            text = json.dumps(spec, indent=2) + "\n"
            if a.write:
                spec_path.write_text(text, encoding="utf-8", newline="\n")
                print(f"wrote {spec_path}")
            else:
                print(text, end="")
            return EXIT_OK
        if a.suffix is not None and (not a.suffix or any(c in a.suffix for c in "/\\ ")):
            raise CycleError("--suffix must be a non-empty name without separators or spaces", EXIT_USAGE)
        cycle = Cycle(load_spec(spec_path), Resolver(a.root), suffix=a.suffix, attempts=parse_attempts(a.attempt),
                      reuse_fields=a.reuse_fields, ledger=a.ledger, spec_path=spec_path)
        if a.verb == "plan":
            print("\n".join(plan_lines(cycle, a.lines_only)))
            return EXIT_OK
        if a.verb == "status":
            print("\n".join(status_lines(cycle)))
            return EXIT_OK
        return run_cycle(cycle, stop_after=a.stop_after)
    except CycleError as exc:
        print(f"research_cycle: {exc}", file=sys.stderr)
        return exc.code


if __name__ == "__main__":
    sys.exit(main())
