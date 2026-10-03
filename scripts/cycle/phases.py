"""The steps of a cycle (research_cycle.py, split move-only into scripts/cycle/ by P9 lane E2): Step, the IC exe's
capabilities, and Cycle, which resolves every phase's argv with its pins (output names, runner caps, attempt
sub-dirs, inherited outputs and the twelve phase builders)."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess

import cycle_resume
import research_ledger
import research_spec
import research_tree
from cycle.resolver import Resolver, placeholder
from cycle.spec import (CAP_KEYS, CAPACITY_FILES, CAPACITY_FLAG, COMPARE_SUFFIX, DEFAULT_OUT_ROOT, DSR_FROM_LEDGER,
                        ERA_PHASES, EXES_PIN, EXIT_PIN, EXIT_USAGE, FIT_INCOMPLETE, HOLDINGS_INPUTS,
                        MARGINAL_CANDIDATES, MAX_ATTEMPTS, OPERAND, OUT_PHASES, PHASE_OUTPUTS, RUNNER_PHASE_RULES,
                        SEC_STAGE_INPUTS, SOURCE_FLAGS, SUMM_V8, BUILDS, CycleError, as_built, effective_exes,
                        input_dir, load_spec, option_value, summ_protocol)
from cycle_verdict import PBO_JSON, SUMM_JSON
from research_common import free_run_dir, run_dir_of


# ------------------------------------------------------------------ the cycle
class Step:
    def __init__(self, phase: str, kind: str, argv=None, output=None, run_dir=None, attempt=None, state="pending",
                 note="", checks=None):
        self.phase, self.kind, self.argv, self.output, self.run_dir = phase, kind, argv, output, run_dir
        self.attempt, self.state, self.note = attempt, state, note
        self.checks = checks or []  # compare steps: the resolved comparisons
        self.role, self.cycle = None, None  # a roles: cycle's era id and era Cycle (H-1; step_key, fields_check)
        self.writes: dict[str, str] = {}   # {repo path: UTF-8 text} written when the step starts (start_step)

    @property
    def done(self) -> bool:
        return self.state == "done"


# K-P9-6 (lane D1): the IC exe's rule table and capabilities envelope, `--list-rules --json`
CAPABILITY_ARGV, CAPABILITY_SCHEMA = ("--list-rules", "--json"), "atx.composition-rules/v1"


def exe_capabilities(exe: str, root: Path, env: dict | None = None) -> frozenset:
    """What the IC exe offers beyond the v7 CLI: the "capabilities" of its `--list-rules --json` envelope (K-P9-6,
    CAPABILITY_SCHEMA; P9 lane E2: no longer probed from the --help text, OR section 3 / DS review section 4), e.g.
    "no-composition" (B-1's u-pass flag) and "marginal" (F-2's marginal IC verb, contract K6). A missing or failing
    exe, or output that is not the envelope (an exe before lane D1), offers nothing (never a hard stop)."""
    p = Path(exe) if Path(exe).is_absolute() else Path(root) / exe
    try:
        done = subprocess.run([str(p), *CAPABILITY_ARGV], cwd=root, env=env, capture_output=True, text=True,
                              timeout=30)
        doc = json.loads(done.stdout or "")
    except (OSError, subprocess.SubprocessError, ValueError):
        return frozenset()
    caps = doc.get("capabilities") if isinstance(doc, dict) and doc.get("schema") == CAPABILITY_SCHEMA else None
    if done.returncode != 0 or not isinstance(caps, list):
        return frozenset()
    return frozenset(c for c in caps if isinstance(c, str))


class Cycle:
    def __init__(self, spec: dict, res: Resolver, *, suffix: str | None = None, attempts: dict | None = None,
                 reuse_fields: str | None = None, ledger: str | None = None, spec_path: Path | None = None,
                 keep_fields: bool = False, runner_overrides: dict | None = None, no_git: bool = False,
                 screen: bool = False, capabilities=None, verify: bool = True, role_key: str | None = None,
                 auto_attempt: bool = False, launch: dict | None = None, keep_verdicts: bool = False):
        if keep_fields and reuse_fields:
            raise CycleError("--keep-fields and --reuse-fields exclude each other", EXIT_USAGE)
        if reuse_fields and as_built(spec.get("fields")):
            raise CycleError("--reuse-fields: the spec pins an as-built fields dir (fields.manifest_sha256); nothing "
                             "is built", EXIT_USAGE)
        if reuse_fields and "reuse_fields" in spec["inputs"]:
            raise CycleError("--reuse-fields: the spec pins inputs.reuse_fields already", EXIT_USAGE)
        if no_git and research_tree.no_git_refusal(res.root):
            raise CycleError(research_tree.no_git_refusal(res.root), EXIT_USAGE)
        spec = json.loads(json.dumps(spec))
        spec["runner"].update(runner_overrides or {})
        if "build" in spec:
            spec["exes"] = effective_exes(spec)
            spec.setdefault("env_path_prepend", list(BUILDS[spec["build"]]["path"]))
        self.spec, self.res, self.suffix, self.keep_fields = spec, res, suffix, keep_fields
        self.runner_overrides = dict(runner_overrides or {})
        self.attempts = dict(attempts or {})
        self.reuse_fields, self.ledger, self.spec_path = reuse_fields, ledger, spec_path
        self.no_git, self.screen = no_git, screen
        self.auto_attempt = auto_attempt    # P9 OR-4: a refused attempt with no output advances to attempt-k
        # P9 F-5 (a) / OR section 5: {admission_wait_seconds, host_budget_mib} for every bounded process ({}: none)
        self.launch = {k: v for k, v in (launch or {}).items() if v is not None}
        self.keep_verdicts = keep_verdicts  # P9 OR section 3: per-run verdict copies (cycle_verdict.keep_copy)
        self._capabilities = capabilities   # None: read the IC exe's --list-rules --json when a step needs it
        self.py = spec["python"]
        # task H-1 (research_roles.py): an era of a roles: cycle keys its outputs and receipts by role_key; the
        # anchor era's fit and summ pool the others (fit_pool: (argv, binds); summ_pool: [(id, nav dir, weights)])
        self.role_key, self.weights_name = role_key, "composition_weights.json"
        self.fit_pool: tuple[list[str], list[str]] | None = None
        self.summ_pool: list[tuple[str, str, str]] | None = None
        # Ruling E-41: the pooled summ is a history read (its series begins before TRAIN, research_roles): N takes no + 1
        self.summ_history = False
        # verify=False: names only (add-alpha reads a parent spec's outputs); no pin, no step
        self.pins = self.verify_inputs() if verify else {}
        self.exe_pins = self.verify_exes() if verify else {}     # P9 OR-2: {} without exes_sha256

    # -------------------------------------------------------------- names and pins
    def out(self, name: str, keyed: bool = True) -> str:
        """An output name: placed under out_root (when set, for a relative name), keyed by the era (H-1: ``-<role>``,
        per-era outputs only), then suffixed."""
        name = self.placed(name)
        if keyed and self.role_key:
            name = f"{name}-{self.role_key}"
        return f"{name}-{self.suffix}" if self.suffix else name

    def placed(self, name: str) -> str:
        root = self.spec.get("out_root")
        return f"{root.rstrip('/')}/{name}" if root and not Path(name).is_absolute() else name

    def out_base(self) -> str:
        return self.spec.get("out_root") or DEFAULT_OUT_ROOT

    def cycle_dir(self) -> str:
        """Where the cycle's own files go: the always-run phases' receipts and cycle_verdict.json."""
        name = f"{self.out_base().rstrip('/')}/cycle-{self.spec['name']}"
        return f"{name}-{self.suffix}" if self.suffix else name

    def derived_root(self, kind: str) -> str:
        """A shared, content-keyed store derived from the role pin and the research window (never suffixed)."""
        try:
            wid = window_id()
        except LookupError as exc:
            raise CycleError(f"spec omits the {kind} root and it cannot be derived: {exc}", EXIT_USAGE) from exc
        role = self.pin("role") if "role" in self.pins else (      # verify=False: the spec's pin, else the file's
            self.spec["inputs"]["role"].get("sha256") or self.res.sha(self.ipath("role")))
        if not role:
            raise CycleError(f"spec omits the {kind} root and the role pin is unknown", EXIT_USAGE)
        return f"{self.out_base().rstrip('/')}/{kind}/{role[:16]}-{wid}"

    def cache_dir(self) -> str:
        ic = self.spec["ic"]
        return self.out(ic["cache"], keyed=False) if "cache" in ic else self.derived_root("candidate-cache")

    def fit_work_dir(self) -> str:
        fit = self.spec["fit"]
        return self.out(fit["work_dir"], keyed=False) if "work_dir" in fit else self.derived_root("fit-work")

    def tool(self, rel: str) -> str:
        """A spec script path; with --no-git a relative one absent under the root is this worktree's copy."""
        if self.no_git and not Path(rel).is_absolute() and not self.res.path(rel).exists() and \
                (research_tree.REPO / rel).is_file():
            return (research_tree.REPO / rel).as_posix()
        return rel

    def capabilities(self) -> frozenset:
        if self._capabilities is None:
            self._capabilities = exe_capabilities(self.spec["exes"]["ic"], self.res.root, self.env())
        return frozenset(self._capabilities)

    def env(self) -> dict:
        env = dict(os.environ)
        pre = self.spec.get("env_path_prepend") or []
        if pre:
            env["PATH"] = os.pathsep.join([str(Path(p)) for p in pre] + [env.get("PATH", "")])
        return env

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

    def verify_exes(self) -> dict:
        """P9 OR-2: {exe key: (path, sha)} of the exes the spec pins (exes_sha256, `lock --exes`); each must hash to its
        pin (else exit 3: a rebuilt exe is re-pinned by `lock --exes --write`). {} without the key, as before."""
        out = {}
        for key, want in (self.spec.get(EXES_PIN) or {}).items():
            rel = (self.spec.get("exes") or {}).get(key)
            got = self.res.sha(rel) if rel else None
            if got is None:
                raise CycleError(f"exe {key} missing: {rel} (spec {EXES_PIN} pins {want})", EXIT_PIN)
            if got != want:
                raise CycleError(f"PIN MISMATCH exe {key}: {rel} is {got}, spec {EXES_PIN} pins {want} (a rebuilt exe "
                                 "is re-pinned by `lock --exes --write`; outputs the old exe made are then refused on "
                                 "resume: move them aside or run under a fresh --suffix)", EXIT_PIN)
            out[key] = (rel, got)
        return out

    def template_chain(self) -> list[dict]:
        """The resolved specs up this cycle's template chain, nearest first ([parent, grandparent, ...]; [] for a plain
        spec, or without a spec file); loaded once. Ruling E1-REUSE-a2 with review E1-N3: an output a template keeps
        is judged by the pins of the ancestor whose run made it, however many generations up."""
        if not hasattr(self, "_template_chain"):
            chain, path = [], self.spec_path
            while path is not None:
                try:
                    doc = json.loads(Path(path).read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    break
                if not (isinstance(doc, dict) and research_spec.is_template(doc)):
                    break
                path, _ = research_spec.parent_of(doc, Path(path), research_tree.REPO)
                chain.append(load_spec(path))      # load_spec refuses a chain whose parents form a cycle
            self._template_chain = chain
        return self._template_chain

    def parent_spec(self) -> dict | None:
        """The resolved spec of this cycle's template parent (None for a plain spec, or without a spec file)."""
        chain = self.template_chain()
        return chain[0] if chain else None

    def introducer(self, phase: str) -> tuple[dict, int]:
        """(the resolved spec whose run made this cycle's ``phase`` output, how many templates up it is: 0 = this
        spec). Review E1-N3: walk up the template chain while the phase's output name (PHASE_OUTPUTS) and out_root
        stay the next ancestor's, the cycle running without --suffix (research_spec: every output a template keeps is
        the parent's and resumes as done); the last such ancestor made it."""
        keys = PHASE_OUTPUTS.get(phase)
        if keys is None or self.suffix:
            return self.spec, 0
        owner, depth = self.spec, 0
        for ancestor in self.template_chain():
            name = (owner.get(keys[0]) or {}).get(keys[1])
            if not (isinstance(name, str) and name == (ancestor.get(keys[0]) or {}).get(keys[1]) and
                    owner.get("out_root") == ancestor.get("out_root")):
                break
            owner, depth = ancestor, depth + 1
        return owner, depth

    def inherited(self, phase: str) -> bool:
        """Whether this cycle's ``phase`` output is an ancestor's (``introducer``: its template parent's, or one further
        up the chain whose output the parent kept in turn)."""
        return self.introducer(phase)[1] > 0

    def reuse_pin(self, st: Step) -> tuple[str | None, str | None, str]:
        """(exe key, its exes_sha256 pin or None, whose pin) that a done bounded step's receipt is judged by (ruling
        E1-REUSE-a2, review E1-N3): the step's executable (its argv after the runner's ``--``) as a key of exes; the pin
        of the resolved spec of the ancestor that made an inherited output (``introducer``), else this spec's. No pin:
        not refused."""
        exe = st.argv[st.argv.index("--") + 1]
        key = next((k for k, v in (self.spec.get("exes") or {}).items() if v == exe), None)
        owner, depth = self.introducer(st.phase)
        pins = owner.get(EXES_PIN) or {}
        whose = ("the spec's" if depth == 0 else "the parent's" if depth == 1 else
                 f"the ancestor's ({depth} templates up)")
        return key, pins.get(key) if key else None, whose

    def pin(self, key: str) -> str:
        return self.pins[key][1]

    def ipath(self, key: str) -> str:
        return self.spec["inputs"][key]["path"]

    def idir(self, key: str) -> str:
        return input_dir(self.spec["inputs"][key])

    def rt_sha(self, rel: str) -> str:
        """An intermediate pin: the SHA-256 of a file an earlier phase wrote (placeholder until it exists)."""
        return self.res.sha(rel) or placeholder(rel)

    def role_counts(self) -> dict:
        """{role_dates, scored_sessions} from the role manifest ("dates", "score_begin", "score_end"); {} when the
        manifest is not on disk (hash-only mode) or lacks them."""
        doc = self.res.read_json(self.ipath("role")) if "role" in self.spec["inputs"] else None
        if not isinstance(doc, dict) or type(doc.get("dates")) is not int:
            return {}
        out = {"role_dates": doc["dates"]}
        if type(doc.get("score_begin")) is int:
            out["scored_sessions"] = doc.get("score_end", doc["dates"]) - doc["score_begin"]
        return out

    def phase_caps(self, phase: str) -> dict:
        """The runner caps of one phase: the runner's own, then the spec's runner.phases entry (else the first
        matching RUNNER_PHASE_RULES rule), then the command line's --runner-override (which wins)."""
        r = self.spec["runner"]
        caps = {k: r[k] for k in CAP_KEYS}
        spec_caps = (r.get("phases") or {}).get(phase)
        if spec_caps is None:
            counts = None
            for rule in RUNNER_PHASE_RULES:
                if phase not in rule["phases"]:
                    continue
                counts = self.role_counts() if counts is None else counts
                if all(counts.get(key[:-len("_over")], -1) > limit for key, limit in rule["when"].items()):
                    spec_caps = rule["caps"]
                    break
        caps.update(spec_caps or {})
        caps.update({k: v for k, v in self.runner_overrides.items() if k in CAP_KEYS})
        return caps

    def runner(self, run_dir: str, binds: list[str], phase: str | None = None) -> list[str]:
        r = self.spec["runner"]
        caps = self.phase_caps(phase) if phase else {k: r[k] for k in CAP_KEYS}
        argv = [self.py, self.tool(r["script"])]
        if self.no_git:
            argv += ["--root", str(self.res.root), "--no-git"]
        argv += ["--seconds", str(caps["seconds"]), "--max-rss-mib", str(caps["max_rss_mib"]),
                 "--min-free-mib", str(caps["min_free_mib"])]
        if self.role_key and phase in ERA_PHASES:   # H-1: an era's own run (the pooled fit and summ have none)
            argv += ["--role-id", self.role_key]
        if self.spec.get("build") in research_tree.BUILD_TYPES:   # K-P9-10: the exes' build type in the receipt
            argv += ["--build-type", research_tree.BUILD_TYPES[self.spec["build"]]]
        argv += self.launch_flags(caps, phase)
        argv += ["--output", run_dir]
        for b in binds:
            argv += ["--bind", b]
        return argv + ["--"]

    def launch_flags(self, caps: dict, phase: str | None) -> list[str]:
        """The runner's launch admission options (none without --admission-wait / --host-budget-mib); a phase whose
        declared cap is above the host budget would never be admitted: refused when planned."""
        out = []
        if "admission_wait_seconds" in self.launch:
            out += ["--admission-wait-seconds", format(self.launch["admission_wait_seconds"], "g")]
        budget = self.launch.get("host_budget_mib")
        if budget is not None:
            if caps["max_rss_mib"] > budget:
                raise CycleError(f"--host-budget-mib {budget} is below the {phase or 'runner'} cap max_rss_mib "
                                 f"{caps['max_rss_mib']}: that process would never be admitted", EXIT_USAGE)
            out += ["--host-budget-mib", str(budget)]
        return out

    # -------------------------------------------------------------- attempt sub-dirs (P9 OR-4)
    def retarget(self, st: Step, run_dir: str, attempt: int) -> None:
        """Point a bounded step's runner at ``run_dir`` (an attempt-k sub-dir of its run dir) as attempt ``attempt``:
        the runner's --output and --attempt before its "--" (the command after it is unchanged)."""
        k = st.argv.index("--")
        head, tail = list(st.argv[:k]), st.argv[k:]
        i = head.index("--output")
        head[i + 1] = run_dir
        if "--attempt" in head:
            head[head.index("--attempt") + 1] = str(attempt)
        else:
            head[i:i] = ["--attempt", str(attempt)]
        st.argv, st.run_dir = head + tail, run_dir

    def resolve_attempts(self, st: Step) -> None:
        """Attempt sub-dirs of a bounded step (P9 OR-4): attempt 1 is the step's run dir, attempt k >= 2 is
        <run dir>/attempt-k. A step with sub-dirs on disk is pointed at the latest one (where its receipt and NAV
        binding are). A failed step whose output is absent and whose latest attempt the runner refused for host
        memory (cycle_resume.AUTO_OUTCOMES: nothing ran to the end, nothing was written) advances to the next
        attempt-k under --auto-attempt (no spec edit, no --suffix); without the flag it stays a stop, as before."""
        if st.kind != "bounded" or not st.run_dir or st.state not in ("done", "failed") or \
                st.phase in ("check", "summ"):
            return
        base = st.run_dir
        dirs = cycle_resume.attempt_dirs(self.res, base, MAX_ATTEMPTS)
        if len(dirs) > 1:
            self.retarget(st, dirs[-1], len(dirs))
            if st.state == "failed":       # the latest attempt's receipt, not the first one's
                st.note = f"{self.failure_note(dirs[-1])} (attempt {len(dirs)}); never overwritten"
        if st.state != "failed" or (st.output and self.res.exists_dir(st.output)):
            return
        r = self.receipt(dirs[-1])
        if not (isinstance(r, dict) and r.get("outcome") in cycle_resume.AUTO_OUTCOMES):
            return
        nxt = f"{base}/{cycle_resume.ATTEMPT_PREFIX}{len(dirs) + 1}"
        if len(dirs) >= MAX_ATTEMPTS:
            st.note += f"; refused with nothing written, and the {MAX_ATTEMPTS} attempts are spent"
        elif self.auto_attempt:
            self.retarget(st, nxt, len(dirs) + 1)
            st.state = "pending"
            st.note = (f"attempt {len(dirs) + 1} (--auto-attempt): {dirs[-1]} was refused ({r.get('outcome')}) and "
                       f"{st.output} is absent")
        else:
            st.note += (f"; refused with nothing written: a resume with --auto-attempt runs attempt {len(dirs) + 1} in "
                        f"{nxt}")

    def phase_bases(self) -> dict:
        """{phase: output base} of a cell spec's bounded phases: u, fit, w, nav, then card, marginal, ref and monitor
        when the spec runs them, then check and summ (<cycle dir>/<phase>); each run dir is research_common.run_dir_of
        (<base>-run[<k>]) with attempt sub-dirs under it. The wave's timings and seal scan read these (OR section 4:
        the cycle's own naming, so a rename here reaches the scan)."""
        s = self.spec
        bases = {"u": self.out(s["ic"]["u_output"]), "fit": self.out(s["fit"]["output"], keyed=False),
                 "w": self.out(s["ic"]["w_output"]), "nav": self.out(s["nav"]["output"])}
        for key in ("card", "marginal", "ref", "monitor"):
            if key in s:
                bases[key] = self.out(s[key]["output"])
        for phase in ("check", "summ"):
            bases[phase] = f"{self.cycle_dir()}/{phase}"
        return bases

    def always_run_dir(self, phase: str) -> str:
        """A fresh receipt dir of an always-run phase (check, summ): <cycle dir>/<phase>-run<k>, k the first unused."""
        return free_run_dir(f"{self.cycle_dir()}/{phase}", self.res.exists_dir)

    def every_phase(self) -> bool:
        return self.spec.get("receipts") == "every-phase"

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
            if self.res.exists_dir(f"{base}-{n}") or self.res.exists_dir(run_dir_of(base, n)):
                return n, "failed", self.failure_note(run_dir_of(base, n))
            return n, "pending", ""
        tried = 0
        for n in range(1, MAX_ATTEMPTS + 1):
            if self.complete_summary(f"{base}-{n}"):
                return n, "done", ""
            if self.res.exists_dir(f"{base}-{n}") or self.res.exists_dir(run_dir_of(base, n)):
                tried = n
        if tried:
            return tried, "failed", (self.failure_note(run_dir_of(base, tried)) +
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
        fd = ""                                  # validate_spec: ic and the static check need fields
        if "fields" in s:
            f_out = s["fields"]["output"]
            if as_built(s["fields"]):  # a pinned input dir: never placed, suffixed or rebuilt
                fd = f_out
            else:
                fd = self.placed(f_out) if self.keep_fields else self.out(f_out)
        fdm = f"{fd}/manifest.json" if fd else ""
        if "fields" in s:
            out.append(self.fields_step(fd, fdm, role_sha))
        if "static_check" in s:
            sc = s["static_check"]
            argv = [self.py, self.tool(sc["script"]), "--manifest", fdm, "--library", lib, "--baseline",
                    self.ipath("baseline_library"), *sc.get("args", [])]
            if sc.get("expect_added"):
                argv += ["--expect-added", ",".join(sc["expect_added"])]
            out.append(self.always_step("check", argv, [self.tool(sc["script"])], "library static check"))
        if "ref" in s:
            out.append(self.ref_step(fdm, role_m, role_sha))
        u_out = ""
        if "ic" in s:
            ic = s["ic"]
            base = self.out(ic["u_output"])
            n, state, note = self.ic_attempt("u", base)
            u_out, run_dir = f"{base}-{n}", run_dir_of(base, n)
            flags = list(ic["flags"])
            if self.screen and "no-composition" in self.capabilities() and "--no-composition" not in flags:
                # B-1: a screen skips the u-pass blend nobody reads (and so has no combined signal to save)
                flags = [x for x in flags if x != "--save-combined"] + ["--no-composition"]
            themes, theme_binds = self.theme_registry()
            argv = self.runner(run_dir, [s["exes"]["ic"], lib, role_m, fdm, *theme_binds], "u") + [
                s["exes"]["ic"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
                role_sha, "--train-fields", fd, "--train-fields-sha256", self.rt_sha(fdm), "--output", u_out,
                *flags, "--candidate-cache", self.cache_dir(), *themes]
            out.append(Step("u", "bounded", argv, u_out, run_dir, n, state, note))
        w_dir = ""
        if "fit" in s:
            step, w_dir = self.fit_step(u_out, lib, lib_sha, role_m, role_sha)
            out.append(step)
        card_out = ""
        if "card" in s:
            step = self.card_step(u_out, w_dir, role_m, role_sha)
            card_out = step.output
            out.append(step)
        if "marginal" in s:
            out.append(self.marginal_step(lib, fd))
        if "gate" in s:
            out.append(Step("gate", "internal", None, state="always",
                            note=f"admission read-out of {w_dir}/admission.json"))
        wt_out = ""
        if "ic" in s and "w_output" in s["ic"]:
            ic = s["ic"]
            base = self.out(ic["w_output"])
            n, state, note = self.ic_attempt("w", base)
            wt_out, run_dir = f"{base}-{n}", run_dir_of(base, n)
            weights = f"{w_dir}/{self.weights_name}"
            w_flags = research_spec.apply_flags(ic["flags"], ic.get("w_flags") or {}, "ic.w_flags")   # E-28
            themes, theme_binds = self.theme_registry()
            argv = self.runner(run_dir, [s["exes"]["ic"], role_m, fdm, lib, weights, *theme_binds], "w") + [
                s["exes"]["ic"], "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
                role_sha, "--train-fields", fd, "--train-fields-sha256", self.rt_sha(fdm), "--output", wt_out,
                *w_flags, "--candidate-cache", self.cache_dir(), "--composition-weights", weights,
                "--composition-weights-sha256", self.rt_sha(weights), *themes]
            out.append(Step("w", "bounded", argv, wt_out, run_dir, n, state, note))
        n_out = ""
        if "nav" in s:
            n_out = self.out(s["nav"]["output"])
            comb = f"{wt_out}/train_combined.json"
            out.append(self.nav_step("nav", n_out, comb, self.rt_sha(comb), s["nav"], fdm, role_m, role_sha))
        if "monitor" in s:
            out.append(self.monitor_step(u_out, w_dir, card_out))
        if "summ" in s:
            out.append(self.summ_step(w_dir, n_out))
        for st in out:
            self.resolve_attempts(st)
        steps = self.with_compares(out)
        if self.screen:  # u -> fit -> card -> marginal -> gate; the identity NAV and the cell wait for the full run
            for st in steps:
                if st.phase in ("ref", "ref" + COMPARE_SUFFIX, "w", "nav", "monitor", "summ") or \
                        (st.phase.endswith(COMPARE_SUFFIX) and st.phase[:-len(COMPARE_SUFFIX)] in ("w", "nav")):
                    st.kind, st.state, st.note = "skipped", "skipped", "screen: runs with the full `run`"
        return steps

    def theme_registry(self) -> tuple[list[str], list[str]]:
        """(the IC passes' --theme-registry PATH --theme-registry-sha256 PIN, its bind) of inputs.theme_registry (P9
        lane E2, D1's theme table, K-P9-6); ([], []) without the input: the argv of before."""
        if "theme_registry" not in self.spec["inputs"]:
            return [], []
        path = self.ipath("theme_registry")
        return ["--theme-registry", path, "--theme-registry-sha256", self.pin("theme_registry")], [path]

    def always_step(self, phase: str, argv: list[str], binds: list[str], note: str) -> Step:
        """An always-run process phase (check, summ): direct, or bounded with a fresh receipt dir (every-phase)."""
        if not self.every_phase():
            return Step(phase, "direct", argv, state="always", note=note)
        run_dir = self.always_run_dir(phase)
        return Step(phase, "bounded", self.runner(run_dir, binds, phase) + argv, None, run_dir, None, "always", note)

    def ref_step(self, fdm: str, role_m: str, role_sha: str) -> Step:
        s, ref = self.spec, self.spec["ref"]
        nav = dict(s["nav"], **{k: ref[k] for k in ("rule", "leverage", "flags") if k in ref})
        step = self.nav_step("ref", self.out(ref["output"]), self.ipath(ref["combined"]), self.pin(ref["combined"]),
                             nav, fdm, role_m, role_sha)
        if "baseline_fields" in s["inputs"] and step.state != "done" and \
                self.res.sha(fdm) == self.pin("baseline_fields"):
            cell_exe, parent_exe = self.nav_exe_shas()
            if cell_exe and parent_exe and cell_exe != parent_exe:   # P9 OR-2: equal fields, another NAV exe
                step.note = (f"fields manifest {fdm} equals the parent's (inputs.baseline_fields) but the NAV exe "
                             f"differs (sha256 {cell_exe}, the parent NAV's {parent_exe}): the reference "
                             "construction runs")
                return step
            step.kind, step.state = "skipped", "skipped"
            step.note = (f"fields manifest {fdm} equals the parent's (inputs.baseline_fields): the reference "
                         "construction is identical by construction")
        return step

    def nav_exe_shas(self) -> tuple[str | None, str | None]:
        """(the SHA-256 of the NAV exe this cycle runs, the executable_sha256 of the parent NAV's last completed run
        receipt, inputs.reference_cell): None where unknown (no file, no reference cell, no receipt recording it)."""
        ref = self.idir("reference_cell") if "reference_cell" in self.spec["inputs"] else None
        return (self.res.sha(self.spec["exes"]["nav"]),
                cycle_resume.completed_exe_sha256(self.res, ref, MAX_ATTEMPTS) if ref else None)

    def marginal_step(self, lib: str, fd: str) -> Step:
        """F-2's marginal IC verb (contract K6) over the u pass's candidate cache and fields dir, on this cycle's role:
        each candidate's IC and its IC residualised on the pool (inputs.<marginal.pool>, the parent's combined signal)
        and, with marginal.themes, on the pool's theme composites. The argv carries every option the verb requires
        (MARGINAL_REQUIRED) and every pin the spec has; skipped with a note when the IC exe does not offer the verb."""
        s, m = self.spec, self.spec["marginal"]
        ic_exe, role_m = s["exes"]["ic"], self.ipath("role")
        m_out = self.out(m["output"])
        run_dir = run_dir_of(m_out)
        pool_key, themes = m.get("pool", "reference_combined"), m.get("themes")
        pool = self.ipath(pool_key)
        self.check_marginal_bindings(pool_key, themes)
        argv = [ic_exe, "marginal", "--candidate-cache", self.cache_dir(), "--library", lib, "--library-sha256",
                self.pin("library"), "--pool", pool, "--pool-sha256", self.pin(pool_key), "--role", role_m]
        binds = [ic_exe, lib, pool, role_m]
        if themes:
            argv += ["--themes", self.ipath(themes)]
            binds.append(self.ipath(themes))
        argv += ["--fields", fd]
        writes = {}
        if m.get("candidates"):           # P9 ruling P4: only the wave's ids, one per line (written when it starts)
            text = "".join(f"{x}\n" for x in m["candidates"])
            path = f"{self.cycle_dir()}/marginal-candidates-{hashlib.sha256(text.encode()).hexdigest()[:12]}.txt"
            argv += [MARGINAL_CANDIDATES, path]
            binds.append(path)
            writes[path] = text
        flags = list(m.get("flags", []))
        min_names = option_value(s["ic"]["flags"], "--min-names")
        if "--min-names" not in flags[::2] and min_names is not None:   # the u pass's name floor
            flags += ["--min-names", min_names]
        argv = self.runner(run_dir, binds + [f"{fd}/manifest.json"], "marginal") + argv + flags + ["--output", m_out]
        if "marginal" not in self.capabilities():
            return Step("marginal", "skipped", argv, m_out, run_dir, None, "skipped",
                        "the IC exe offers no marginal verb (contract K6, lane F): skipped")
        st = Step("marginal", "bounded", argv, m_out, run_dir, None, *self.single_state(
            m_out, run_dir, f"{m_out}/marginal_ic.json"))
        st.writes = writes
        return st

    def check_marginal_bindings(self, pool_key: str, themes: str | None) -> None:
        """The verb's bindings of the files this spec pins, checked when the step is planned (exit 3 before any phase
        runs): the pool manifest's role_manifest_sha256 is inputs.role's pin and, with themes, its
        composition_weights_sha256 is the pin of inputs.<themes>. Skipped for a pool not on disk (hash-only)."""
        rel = self.ipath(pool_key)
        try:
            doc = self.res.read_json(rel)
        except ValueError as exc:
            raise CycleError(f"marginal: pool {rel} is not a combined-signal manifest ({exc})", EXIT_PIN) from exc
        if doc is None:
            return
        doc = doc if isinstance(doc, dict) else {}
        if doc.get("role_manifest_sha256") != self.pin("role"):
            raise CycleError(f"marginal: pool {rel} was blended on role {doc.get('role_manifest_sha256')}, not on "
                             f"inputs.role {self.pin('role')} (the verb binds --role to the pool)", EXIT_PIN)
        if themes and doc.get("composition_weights_sha256") != self.pin(themes):
            raise CycleError(f"marginal: pool {rel} names composition weights {doc.get('composition_weights_sha256')}, "
                             f"not inputs.{themes} {self.pin(themes)} (the verb binds --themes to the pool)", EXIT_PIN)

    def single_state(self, out: str, run_dir: str, marker: str) -> tuple[str, str]:
        """(state, note) of a single-attempt output: done iff its marker file exists; a partial output is a stop."""
        if self.res.exists(marker):
            return "done", ""
        if self.res.exists_dir(out) or self.res.exists_dir(run_dir):
            return "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
        return "pending", ""

    def summ_step(self, w_dir: str, n_out: str) -> Step:
        s, sm = self.spec, self.spec["summ"]
        argv = [self.py, self.tool(sm["script"])]
        if w_dir:
            argv += ["--weights", f"{w_dir}/composition_weights.json"]
        if "reference_cell" in s["inputs"]:
            argv += ["--reference", self.idir("reference_cell")]
        pool = self.summ_pool                # H-1: the eras' NAV dirs pooled by nav_summ --pool (anchor era)
        for _, _, weights in pool or []:
            if weights != f"{w_dir}/composition_weights.json":
                argv += ["--weights", weights]
        from_ledger = sm.get("cells_from_ledger") or sm["dsr_n"] == DSR_FROM_LEDGER
        prior, ledger_n = self.ledger_cells(n_out) if from_ledger else (None, None)
        cells = prior if sm.get("cells_from_ledger") else (sm.get("cells") or [])
        if cells:  # one positional block (argparse), before the options: a trailing nargs-* --pbo takes none
            argv += [*cells] + ([] if pool else [n_out])
        argv += ["--dsr-n", str(self.dsr_n(ledger_n)), *sm.get("extra", [])]
        ledger = self.ledger or sm.get("ledger")
        if summ_protocol(s) == SUMM_V8:
            argv += self.v8_summ_flags(ledger)
        if s.get("verdict"):
            argv += ["--json", f"{self.cycle_dir()}/{SUMM_JSON}"]
            if "--pbo" in sm.get("extra", []):
                argv += ["--pbo-json", f"{self.cycle_dir()}/{PBO_JSON}"]
        if ledger:
            argv += ["--ledger", ledger, "--ledger-kind", sm.get("ledger_kind", "construction")]
        if s.get("verdict"):   # review C-1: the verdict's DSR is the ledger's (N and V[SR] of v8-prereg item 3)
            if not ledger:
                raise CycleError("spec verdict: the verdict's DSR needs the sprint ledger of record (summ.ledger or "
                                 "--ledger) for nav_summ --dsr-ledger (v8-prereg item 3)", EXIT_USAGE)
            argv += ["--dsr-ledger", ledger]
        if pool:
            argv += ["--pool", *[nav for _, nav, _ in pool], "--pool-ids", ",".join(i for i, _, _ in pool)]
        elif not cells:
            argv.append(n_out)
        return self.always_step("summ", argv, [self.tool(sm["script"])], "nav_summ vs the reference cell")

    def v8_summ_flags(self, ledger: str | None) -> list[str]:
        """Review C-2: a v8 scoring step always carries nav_summ --protocol v8 (the pre-registered bootstrap seed and
        draw count, the year table, origin + window_id + chain in the ledger line), added unless summ.extra has it;
        with a ledger also --origin summ.origin (contract K5), which nav_summ requires there."""
        sm = self.spec["summ"]
        extra = sm.get("extra", [])
        out = [] if option_value(extra, "--protocol") == SUMM_V8 else ["--protocol", SUMM_V8]
        if ledger and "--origin" not in extra:
            if not sm.get("origin"):
                raise CycleError("spec summ: a v8 scoring step ledgers the cell with its origin class: set summ.origin "
                                 "(prior | grid | mined, contract K5)", EXIT_USAGE)
            out += ["--origin", sm["origin"]]
        return out

    def dsr_n(self, ledger_n: int | None) -> int:
        """summ.dsr_n: the declared integer, or "ledger+1" = the ledger's N (research_ledger.ledger_n: the defect-rule
        construction trials + 1 for this cell when not yet ledgered, nav_summ's N; resolved at scoring time)."""
        n = self.spec["summ"]["dsr_n"]
        return ledger_n if n == DSR_FROM_LEDGER else n

    def with_compares(self, steps: list[Step]) -> list[Step]:
        """The spec's compare items as one internal "<after>-compare" step right after each named phase (skipped with
        it when that phase is skipped)."""
        items = self.spec.get("compare") or []
        if not items:
            return steps
        outs = {st.phase: st.output for st in steps if st.phase in OUT_PHASES and st.output}

        def operand(text: str) -> str:
            return OPERAND.sub(lambda m: self.ipath(m[2]) if m[1] == "input" else outs[m[2]], text)
        done = []
        for st in steps:
            done.append(st)
            mine = [dict(c, **{s: operand(c[s]) for s in ("a", "b", "keys", "keys_in") if s in c})
                    for c in items if c["after"] == st.phase]
            if mine:
                note = "identity: " + "; ".join(f"{c['name']} [{c['mode']}]" for c in mine) + " (a miss hard-stops)"
                if st.state == "skipped":
                    done.append(Step(st.phase + COMPARE_SUFFIX, "skipped", None, state="skipped",
                                     note=f"{st.phase} skipped: {note}", checks=mine))
                else:
                    done.append(Step(st.phase + COMPARE_SUFFIX, "compare", None, state="always", note=note,
                                     checks=mine))
        return done

    def nav_step(self, phase: str, n_out: str, comb: str, comb_sha: str, nav: dict, fdm: str, role_m: str,
                 role_sha: str) -> Step:
        """A NAV replay (the cycle's cell, or the reference construction for the ref phase)."""
        s = self.spec
        k = self.attempts.get("nav", 1) if phase == "nav" else 1
        run_dir = run_dir_of(n_out) if k == 1 else run_dir_of(n_out, k)
        flags = [str(nav.get("leverage")) if f == "{leverage}" else f for f in nav["flags"]]
        binds, label = [s["exes"]["nav"], comb, fdm], []
        if "label_role" in s["inputs"]:   # v8 E-25: the label role marks every replayed book (review F-8: ref too)
            binds.append(self.ipath("label_role"))
            label = ["--label-role", self.ipath("label_role"), "--label-role-sha256", self.pin("label_role")]
        argv = self.runner(run_dir, binds, phase) + [
            s["exes"]["nav"], "nav", "--combined", comb, "--combined-sha256", comb_sha, "--role", role_m,
            "--role-sha256", role_sha, "--fields", fdm, "--fields-sha256", self.rt_sha(fdm), "--output", n_out,
            "--rule", nav["rule"], *flags, *label]
        missing = self.capacity_missing(n_out, flags)
        if self.res.exists(f"{n_out}/summary.json") and not missing:
            state, note = "done", ""
        elif missing:
            state, note = "failed", (f"{n_out}/summary.json exists but {', '.join(missing)} of {CAPACITY_FLAG} do not "
                                     "(the NAV verb writes summary.json before its capacity pass and extras: that pass "
                                     "did not finish; P9 NV-4); never overwritten: use a fresh --suffix")
        elif self.res.exists_dir(n_out) or self.res.exists_dir(run_dir):
            state, note = "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
        else:
            state, note = "pending", "" if phase == "nav" else "reference construction on this cycle's fields"
        return Step(phase, "bounded", argv, n_out, run_dir, k, state, note)

    def capacity_missing(self, n_out: str, flags: list) -> list[str]:
        """The files a NAV whose argv carries --capacity-curve still lacks beside its summary.json (CAPACITY_FILES:
        written after the summary, P9 NV-4); empty without the flag, without a summary.json, or for a summary known
        by hash only (content checks skipped, as complete_summary skips them)."""
        rel = f"{n_out}/summary.json"
        if CAPACITY_FLAG not in flags or not self.res.exists(rel) or self.res.hash_only(rel):
            return []
        return [name for name in CAPACITY_FILES if not self.res.exists(f"{n_out}/{name}")]

    def ledger_cells(self, n_out: str) -> tuple[list[str], int]:
        """summ.cells_from_ledger / dsr_n "ledger+1": (every trial line's cell, in ledger order, this cycle's own cell
        excluded (protocol lines are no trial: research_ledger), the ledger's N); N is backtest_integrity.ledger_n
        (the defect rule, + 1 for this cell when not yet ledgered: nav_summ's N; a history read adds 0, Ruling E-41)
        and an integer dsr_n must equal it."""
        sm = self.spec["summ"]
        rel = self.ledger or sm["ledger"]
        p = self.res.path(rel)
        if not p.is_file():
            raise CycleError(f"summ.cells_from_ledger: no trial ledger at {rel}", EXIT_PIN)
        navs = [nav for _, nav, _ in self.summ_pool or []]   # H-1: the pooled cell, by its pooled trial_id
        try:
            cells = research_ledger.cells(p)
            n = (research_ledger.ledger_n(p, research_ledger.pool_label(navs), pool_dirs=[self.res.path(d) for d in navs],
                                          history=self.summ_history)
                 if navs else research_ledger.ledger_n(p, n_out, self.res.path(n_out)))
        except research_ledger.LedgerError as exc:
            raise CycleError(f"summ.cells_from_ledger: {exc}", EXIT_PIN) from exc
        prior = [c for c in cells if c != n_out]
        if len(set(prior)) != len(prior):
            raise CycleError(f"summ.cells_from_ledger: {rel} lists a cell twice", EXIT_PIN)
        if sm["dsr_n"] != DSR_FROM_LEDGER and n != sm["dsr_n"]:
            raise CycleError(f"summ.cells_from_ledger: {rel} lists {len(prior)} prior cells, so cross-cell N = "
                             f"{n}, but the spec declares summ.dsr_n {sm['dsr_n']}: set summ.dsr_n to {n}", EXIT_PIN)
        return prior, n

    def fields_step(self, fd: str, fdm: str, role_sha: str) -> Step:
        f, s = self.spec["fields"], self.spec
        if as_built(f):  # as built: an input, never (re)built by the cycle
            pin, got = f["manifest_sha256"], self.res.sha(fdm)
            if got is None:
                state, note = "failed", (f"pinned fields manifest missing: {fdm} (as built; the cycle never builds a "
                                         "pinned fields dir)")
            elif pin is None:
                state, note = "done", f"as built, manifest sha256 {got} (UNLOCKED: computed now; run `lock --write`)"
            elif got != pin:
                raise CycleError(f"PIN MISMATCH fields: {fdm} is {got}, spec pins {pin}", EXIT_PIN)
            else:
                state, note = "done", f"as built, manifest sha256 {pin} (pinned)"
            return Step("fields", "pinned", None, fd, None, None, state, note)
        argv = [self.py, self.tool(f["builder"]), "--role", self.idir("role"), "--role-sha256", role_sha, "--output",
                fd, "--fields", ",".join(f["list"])]
        for key, flag in SOURCE_FLAGS:
            if key in f.get("sources", {}):
                argv += [flag, f["sources"][key]]
        if "identity_bridge" in s["inputs"]:
            argv += ["--identity-bridge", self.idir("identity_bridge"), "--identity-bridge-sha256",
                     self.pin("identity_bridge")]
        if "fund_events" in s["inputs"]:
            argv += ["--fund-events", self.idir("fund_events"), "--fund-events-sha256", self.pin("fund_events")]
        if "sic_events" in s["inputs"]:  # U2: the grp_* SIC table = the role's (atx-db fundamentals stage manifest)
            argv += ["--sic-events", self.idir("sic_events"), "--sic-events-sha256", self.pin("sic_events")]
        argv += self.stage_flags()
        if "fund_lag_sessions" in f:
            argv += ["--fund-lag-sessions", str(f["fund_lag_sessions"])]
        argv += ["--max-rss-mib", str(f.get("max_rss_mib", 1536)), "--max-seconds", str(f.get("max_seconds", 1800))]
        if "reuse_fields" in s["inputs"]:  # a pinned prior fields dir (L9 design)
            argv += ["--reuse", self.idir("reuse_fields"), "--reuse-sha256", self.pin("reuse_fields")]
            if f.get("reuse_hardlink"):
                argv.append("--reuse-hardlink")
        elif self.reuse_fields:
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
        if self.every_phase():
            run_dir = run_dir_of(fd)
            if state == "pending" and self.res.exists_dir(run_dir):
                state, note = "failed", self.failure_note(run_dir) + "; never overwritten: use a fresh --suffix"
            return Step("fields", "bounded", self.runner(run_dir, [self.tool(f["builder"]), self.ipath("role")], "fields") + argv,
                        fd, run_dir, None, state, note)
        return Step("fields", "direct", argv, fd, None, None, state, note)

    def stage_flags(self) -> list[str]:
        """The L9 stage inputs as fields-builder flags: the four SEC inputs (--sec-stages = the three stage dirs'
        common parent, --sec-identity-bridge, and one pin per stage) and the five holdings stages (--<key> DIR
        --<key>-sha256 PIN), in INPUT_KEYS order."""
        inputs, argv = self.spec["inputs"], []
        if "sec_identity_bridge" in inputs:
            argv += ["--sec-stages", str(Path(self.idir(SEC_STAGE_INPUTS[0])).parent).replace("\\", "/"),
                     "--sec-identity-bridge", self.idir("sec_identity_bridge"), "--sec-identity-bridge-sha256",
                     self.pin("sec_identity_bridge")]
            for key in SEC_STAGE_INPUTS:
                argv += [f"--{key.replace('_', '-')}-sha256", self.pin(key)]
        for key in HOLDINGS_INPUTS:
            if key in inputs:
                flag = f"--{key.replace('_', '-')}"
                argv += [flag, self.idir(key), f"{flag}-sha256", self.pin(key)]
        return argv

    def card_step(self, u_out: str, w_dir: str, role_m: str, role_sha: str) -> Step:
        c = self.spec["card"]
        c_out = self.out(c["output"])
        run_dir = run_dir_of(c_out)
        adm = f"{w_dir}/admission.json"
        argv = self.runner(run_dir, [self.tool(c["script"]), role_m, f"{u_out}/summary.json", adm], "card") + [
            self.py, self.tool(c["script"]), "--u-pass", u_out, "--train", role_m, "--train-sha256", role_sha,
            "--admission", adm, "--admission-sha256", self.rt_sha(adm), *c.get("flags", []), "--output", c_out]
        return Step("card", "bounded", argv, c_out, run_dir, None, *self.single_state(c_out, run_dir,
                                                                                      f"{c_out}/index.json"))

    def monitor_step(self, u_out: str, w_dir: str, card_out: str) -> Step:
        m = self.spec["monitor"]
        m_out = self.out(m["output"])
        argv = [self.py, self.tool(m["script"]), "--baseline", "--daily-ic", f"{u_out}/train_daily_ic.csv",
                "--admission", f"{w_dir}/admission.json", "--fit-work", self.fit_work_dir()]
        if card_out:
            argv += ["--sleeve-daily", f"{card_out}/daily_sleeve.csv"]
        for key, flag in (("holdings_days", "--holdings-days"), ("bias", "--bias"), ("decide", "--decide")):
            if m.get(key):
                argv += [flag, m[key]]
        argv += [*m.get("flags", []), "--output", m_out]
        if self.every_phase():
            run_dir = run_dir_of(m_out)
            return Step("monitor", "bounded", self.runner(run_dir, [self.tool(m["script"])], "monitor") + argv, m_out, run_dir,
                        None, *self.single_state(m_out, run_dir, f"{m_out}/monitor.json"))
        if self.res.exists(f"{m_out}/monitor.json"):
            state, note = "done", ""
        elif self.res.exists_dir(m_out):
            state, note = "failed", f"partial monitor dir exists (never overwritten): {m_out}; use a fresh --suffix"
        else:
            state, note = "pending", ""
        return Step("monitor", "direct", argv, m_out, None, None, state, note)

    def fit_step(self, u_out: str, lib: str, lib_sha: str, role_m: str, role_sha: str) -> tuple[Step, str]:
        s, fit = self.spec, self.spec["fit"]
        w_dir = self.out(fit["output"], keyed=False)   # H-1: one pooled fit for every era
        o, sm = f"{u_out}/orientations.json", f"{u_out}/summary.json"
        max_passes = int(fit.get("max_passes", 3))
        runs = [j for j in range(1, MAX_ATTEMPTS + 1) if self.res.exists_dir(run_dir_of(w_dir, j))]
        if "fit" in self.attempts:
            j = self.attempts["fit"]
        elif self.res.exists(f"{w_dir}/composition_weights.json"):
            j = runs[-1] if runs else 1
        elif runs:   # the pass's last attempt (an attempt-k sub-dir after a refusal, P9 OR-4) decides the next pass
            last = self.receipt(cycle_resume.attempt_dirs(self.res, run_dir_of(w_dir, runs[-1]), MAX_ATTEMPTS)[-1])
            j = runs[-1] + 1 if last and last.get("exit_code") == FIT_INCOMPLETE else runs[-1]
        else:
            j = 1
        run_dir = run_dir_of(w_dir, j)
        if self.res.exists(f"{w_dir}/composition_weights.json"):
            state, note = "done", ""
        elif self.res.exists_dir(run_dir):
            state, note = "failed", self.failure_note(run_dir) + "; never overwritten"
        elif j > max_passes:
            state, note = "failed", f"fit still incomplete after {max_passes} passes"
        else:
            state, note = "pending", "" if j == 1 else f"resume pass {j} (previous pass exited {FIT_INCOMPLETE})"
        pool_argv, pool_binds = self.fit_pool or ([], [])   # H-1: --era ... --era-id of the anchor era
        resid_parent = "reference_resid_parent" in s["inputs"] and not self.fit_pool   # v8 R-11 (R6B-O-5)
        parent_binds = [self.ipath("reference_resid_parent")] if resid_parent else []
        argv = self.runner(run_dir, [lib, self.ipath("recipe"), role_m, o, sm, *pool_binds, *parent_binds], "fit") + [
            self.py, self.tool(fit["script"]), "--library", lib, "--library-sha256", lib_sha, "--train", role_m, "--train-sha256",
            role_sha, "--orientations", o, "--orientations-sha256", self.rt_sha(o), "--runner-summary", sm,
            "--runner-summary-sha256", self.rt_sha(sm), *fit["flags"]]
        if resid_parent:
            argv += ["--theme-resid-parent", self.ipath("reference_resid_parent"), "--theme-resid-parent-sha256",
                     self.pin("reference_resid_parent")]
        if "recipe" in s["inputs"]:
            argv += ["--recipe", self.ipath("recipe"), "--recipe-sha256", self.pin("recipe")]
        argv += ["--work-dir", self.fit_work_dir()]
        if "max_seconds" in fit:
            argv += ["--max-seconds", str(fit["max_seconds"])]
        argv += [*pool_argv, "--output", w_dir]
        return Step("fit", "bounded", argv, w_dir, run_dir, j, state, note), w_dir


def window_id() -> str:
    return research_tree.window_id()
