"""A synthetic root for research_wave.py tests: a git repository holding a parent cell, its NAV, a fields manifest, a
chained ledger and a committed wave manifest, plus FakeCycle, a process executor that plays research_cycle.py,
add-alpha, the bounded runner (running wave_readers.py in process) and nav_summ --bundle on synthetic files.

Every number here is synthetic (sessions 2020-2021, inside TRAIN); no data of the repository is read.
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402
import research_ledger  # noqa: E402
import research_spec  # noqa: E402
import research_tree  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_readers  # noqa: E402

BI = research_ledger.backtest_integrity()
LEDGER = "out/trials.jsonl"
PARENT = "scripts/specs/v8/lib-p0.json"
PARENT_NAV = "out/nav-p0-L1.1474"
FIELDS = "fields-v1"
MANIFEST = "scripts/specs/v8/waves/w1.json"
DAY_NS = 86_400_000_000_000


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unrooted(argv: list[str]) -> list[str]:
    """argv without its ``--root ROOT`` (every research_cycle / add-alpha call carries the wave's root)."""
    if "--root" not in argv:
        return list(argv)
    k = argv.index("--root")
    return list(argv[:k]) + list(argv[k + 2:])


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True, text=True).stdout


def cell_spec(name: str, nav_out: str, *, reference_nav: str | None = None, leverage: str = "1.1474",
              marginal_of: str | None = None) -> dict:
    """A synthetic plain cell spec; ``marginal_of`` (the parent's name) adds add-alpha's marginal section on the
    parent's combined signal and theme weights, with its runner cap."""
    s = _cell_spec(name, nav_out, reference_nav, leverage)
    if marginal_of:
        s["inputs"]["reference_combined"] = {"path": f"out/w-{marginal_of}-1/train_combined.json", "sha256": None}
        s["inputs"]["reference_weights"] = {"path": f"out/fit-{marginal_of}/composition_weights.json", "sha256": None}
        s["marginal"] = {"output": f"out/u-{name}-marginal", "pool": "reference_combined", "themes": "reference_weights"}
        s["runner"]["phases"] = {"marginal": {"seconds": 360}}
    return s


def _cell_spec(name: str, nav_out: str, reference_nav: str | None, leverage: str) -> dict:
    s = {"schema": RC.SCHEMA, "name": name, "description": f"synthetic cell {name}", "python": sys.executable,
         "runner": {"script": "scripts/run_bounded_research.py", "seconds": 60, "max_rss_mib": 512, "min_free_mib": 256},
         "exes": {"ic": "bin/ic.exe", "nav": "bin/nav.exe"},
         "inputs": {"library": {"path": f"lib/{name}.json", "sha256": None},
                    "role": {"path": "role/manifest.json", "sha256": None}},
         "fields": {"output": FIELDS, "manifest_sha256": None, "list": ["a", "b"]},
         "ic": {"u_output": f"out/u-{name}", "w_output": f"out/w-{name}", "flags": []},
         "fit": {"script": "fit.py", "output": f"out/fit-{name}", "flags": []},
         "card": {"script": "card.py", "output": f"out/card-{name}"},
         "gate": {"admitted": [], "require": "any"},
         "nav": {"output": nav_out, "rule": "aim-partial-v5", "leverage": leverage,
                 "flags": ["--aim-leverage", "{leverage}"]},
         "summ": {"script": "nav_summ.py", "dsr_n": "ledger+1", "ledger": LEDGER, "origin": "prior"},
         "verdict": True}
    if reference_nav:
        s["inputs"]["reference_cell"] = {"dir": reference_nav, "path": f"{reference_nav}/summary.json", "sha256": None}
    return s


def write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")


def write_json(root: Path, rel: str, doc) -> None:
    write(root, rel, json.dumps(doc, indent=2) + "\n")


def sessions(n: int) -> list[int]:
    out, d = [], dt.date(2020, 1, 2)
    while len(out) < n:
        if d.weekday() < 5:
            out.append((d - dt.date(1970, 1, 1)).days * DAY_NS)
        d += dt.timedelta(days=1)
    return out


def write_nav(root: Path, nav: str, gross: float, *, drift: float = 2e-4, net_lev: float = 0.001, rows: int = 300,
              x4: float = 1.1, accounting: float = 1e-14) -> None:
    """A synthetic NAV output dir: summary.json (primary s2), daily_s2.csv, capacity_curve.csv."""
    cols = ["session_ns", "return_observation", "executed", "traded_dollars", "pretrade_gross_dollars",
            "one_way_turnover_gmv", "gross_leverage", "net_leverage", "trade_cost_dollars", "pretrade_nav",
            "held_names", "net_return"]
    buf = io.StringIO()
    wr = csv.writer(buf, lineterminator="\n")
    wr.writerow(cols)
    for k, s in enumerate(sessions(rows)):
        wobble = 0.01 * math.sin(k / 7.0)
        wr.writerow([s, 1, 1, 1e6, 1e8, 0.02 + 0.002 * math.cos(k / 5.0), gross + wobble, net_lev, 120.0, 1e8, 100,
                     drift + 0.003 * math.sin(k * 1.3)])
    write(root, f"{nav}/daily_s2.csv", buf.getvalue())
    scen = {"scenario": "s2", "net_sharpe": 1.2, "gross_sharpe": 1.6, "ann_mean": 0.05, "cagr": 0.051,
            "ann_vol": 0.035, "max_drawdown": 0.03, "observations": rows - 1,
            "costs": {"summed_trade_cost_return": 0.01, "summed_borrow_return": 0.005},
            "financing": {"summed_long_financing_return": 0.002},
            "accounting_checks": {"max_return_identity_error": accounting, "max_cash_book_relative_error": 1e-15,
                                  "tolerance": 1e-9},
            "meets_daily_turnover_mean": True, "meets_daily_turnover_p95": True,
            "construction": {"v5": {"aim_leverage": 1.0}}, "daily_turnover_gmv": {"mean": 0.02}}
    write_json(root, f"{nav}/summary.json", {"primary_scenario": "s2", "rule": "aim-partial-v5", "status": "complete",
                                             "scenarios": [scen]})
    cap = "multiple,book,net_sharpe\n" + "".join(f"{m},s2,{x4 if m == 4 else 1.0}\n" for m in (0.5, 1, 2, 4, 8))
    write(root, f"{nav}/capacity_curve.csv", cap)


def construction_line(cell: str, daily_sha: str, sr: float) -> dict:
    return {"schema": BI.LEDGER_SCHEMA, "kind": "construction", "count": 1, "cell": cell, "s2_net_sr": sr,
            "trial_id": BI.trial_id("construction", daily_sha), "window_id": BI.window_id(), "origin": "prior"}


def admission_line(cycle: str, cid: str, *, role_sha: str = "", status: str = "admitted", origin: str = "prior",
                   dsl_sha: str | None = None) -> dict:
    """One admission line in the ledger's real layout (cycle_admission.admission_lines), its trial_id by the same rule."""
    import wave_stages  # noqa: PLC0415
    dsl_sha = dsl_sha or candidate(cid)["dsl_sha256"]
    return {"schema": BI.LEDGER_SCHEMA, "kind": "admission", "count": 1, "candidate": cid, "cycle": cycle,
            "status": status, "origin": origin, "window_id": BI.window_id(),
            "window": {"label": "TRAIN", "first_session": "2020-01-02", "last_session": "2023-12-29"},
            "pins": {"library_sha256": sha(cycle.encode()), "role_sha256": role_sha, "admission_sha256": sha(b"a")},
            "dsl_sha256": dsl_sha, "trial_id": wave_stages.admission_trial_id(cid, dsl_sha, role_sha)}


def role_sha(root: Path) -> str:
    return sha((root / "role" / "manifest.json").read_bytes())


def candidate(cid: str, *, kind: str = "add", prior: int = 1, **extra) -> dict:
    dsl = f"rank(ts_mean({cid}_field, 20))"
    c = {"id": cid, "dsl": dsl, "dsl_sha256": WM.dsl_sha256(dsl), "theme": "value", "tier": "B", "prior_sign": prior,
         "citation": "Synthetic 2026", "origin": "prior", "hypothesis": f"h-{cid}", "kind": kind}
    c.update(extra)
    return c


def manifest(drop: tuple = (), **over) -> dict:
    m = {"schema": WM.SCHEMA, "wave": "w1", "description": "synthetic wave",
         "parent": {"spec": PARENT, "library": "p0"},
         "fields": {"dir": FIELDS, "manifest_sha256": None}, "library": "w1",
         "candidates": [candidate("alpha_a"), candidate("alpha_b"), candidate("alpha_c")],
         "acceptance": {"rule": "pm7-34", "printed": ["turnover-per-gross-not-higher", "capacity-4x-higher"]},
         "sign_rule": "pm7-35", "gross_match": "pm6-6",
         "budget": {"id": "syn", "admission_cap": 10, "admission_cycle_prefix": "w", "construction_cap": 20},
         "ledger": LEDGER, "expect": {"n_before": 2}, "out_dir": "out/waves/w1",
         "record": {"copy_to": "sprint/waves"}}
    m.update(over)
    return {k: v for k, v in m.items() if k not in drop}


def build(root: Path, drop: tuple = (), **over) -> Path:
    """The synthetic root; returns it. ``over`` overrides manifest keys, ``drop`` removes them (fields.manifest_sha256
    is filled in)."""
    root.mkdir(parents=True, exist_ok=True)
    git(root, "init", "-q")
    git(root, "config", "user.email", "wave@test")
    git(root, "config", "user.name", "wave test")
    write_json(root, PARENT, cell_spec("p0", PARENT_NAV, reference_nav="out/nav-base"))
    write_json(root, "atx-impl/strategies/libraries/p0.json", {"members": ["m1", "m2"]})
    write_json(root, f"{FIELDS}/manifest.json", {"status": "complete", "seal": {"exclusive_end": "2024-01-01"},
                                                 "fields": [{"name": "a"}, {"name": "b"}]})
    write_json(root, "role/manifest.json", {"dates": 300})
    write_nav(root, PARENT_NAV, 0.9862)
    BI.ledger_append(root / LEDGER, [construction_line("out/nav-base", sha(b"base"), 1.0),
                                     construction_line(PARENT_NAV, sha((root / PARENT_NAV / "daily_s2.csv").read_bytes()),
                                                       1.2)], chain=True)
    m = manifest(drop, **over)
    if m["fields"].get("manifest_sha256") is None:
        m["fields"] = dict(m["fields"], manifest_sha256=sha((root / FIELDS / "manifest.json").read_bytes()))
    write_json(root, MANIFEST, m)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "synthetic root")
    return root


# ------------------------------------------------------------------ the fake processes
class FakeCycle:
    """The process executor of a wave test. ``admission`` = {candidate id: (status, runner_sign)} for the screen: every
    row goes to the fit's admission.json (after the parent member m1's), the ``listed`` ones (gate.admitted; default
    all) to cycle_verdict.json and the ledger;
    ``gross_per_l`` = {cell spec name: c} so that a NAV at leverage L has all-rows gross c x L; ``dsr`` the paired
    dSR the cell's summ reports; ``fail`` = {argv token: exit} forces an exit."""

    def __init__(self, root: Path, admission: dict, *, gross_per_l: dict | None = None, dsr: float = 0.05,
                 gate_exit: int = 0, accounting: float = 1e-14, fail: dict | None = None, listed: set | None = None,
                 screen_log: str = "u pass 2020-01-02 .. 2023-12-29\n"):
        self.root, self.admission, self.gross_per_l, self.listed = root, admission, gross_per_l or {}, listed
        self.screen_log = screen_log
        self.dsr, self.gate_exit, self.accounting, self.fail = dsr, gate_exit, accounting, fail or {}
        self.calls: list[list[str]] = []

    def __call__(self, argv, root, env=None):
        self.calls.append(list(argv))
        for token, code in self.fail.items():
            if token in argv:
                return subprocess.CompletedProcess(argv, code, "", f"forced exit {code}")
        if argv[0] == "git":
            done = subprocess.run(argv, cwd=root, capture_output=True, text=True)
            return done
        tool = Path(argv[1]).name
        if tool == "research_cycle.py":
            return self.cycle(argv[2:])
        if tool == "run_bounded_research.py":
            return self.bounded(argv)
        raise AssertionError(f"unexpected command {argv}")

    def ok(self, argv, code=0):
        return subprocess.CompletedProcess(argv, code, "", "")

    def spec_outputs(self, rel: str) -> tuple[dict, RC.Cycle]:
        spec = RC.load_spec(self.root / rel)
        return spec, RC.Cycle(spec, RC.Resolver(self.root), verify=False)

    def cycle(self, args: list[str]):
        if args[0] == "add-alpha":
            return self.add_alpha(args)
        verb, rel = args[0], args[1]
        if verb == "lock":
            RC.load_spec(self.root / rel)
            doc = json.loads((self.root / rel).read_text())
            if "--write" in args and research_spec.is_template(doc):  # lock_template's pins, as `lock --write` writes
                doc.setdefault("locked", {})["role"] = {"path": "role/manifest.json", "sha256": role_sha(self.root)}
                write_json(self.root, rel, doc)
            return self.ok(args)
        spec, c = self.spec_outputs(rel)
        if "--screen" in args:
            rows = [{"id": cid, "status": st, "runner_sign": sg, "s_k": 1} for cid, (st, sg) in self.admission.items()]
            listed = [r for r in rows if self.listed is None or r["id"] in self.listed]     # gate.admitted
            marginal = [{"id": cid, "ic21": 0.01, "ic21_hac_t": 2.0, "marginal_ic21": 0.005, "marginal_hac_t": 1.5,
                         "max_abs_rho": 0.3, "max_rho_member": "m1"}
                        for cid in self.admission] if "marginal" in spec else []
            fit = c.out(spec["fit"]["output"], keyed=False)
            member = {"id": "m1", "status": "admitted", "runner_sign": 1, "s_k": 1}
            write_json(self.root, f"{fit}/admission.json", {"candidates": [member] + rows})
            write_json(self.root, f"{c.cycle_dir()}/cycle_verdict.json", {"admission": listed, "marginal": marginal})
            u_run = self.root / f"{c.out(spec['ic']['u_output'])}-run"
            u_run.mkdir(parents=True, exist_ok=True)
            write(self.root, f"{c.out(spec['ic']['u_output'])}-run/stdout.log", self.screen_log)
            write_json(self.root, f"{c.out(spec['ic']['u_output'])}-run/receipt.json",
                       {"outcome": "completed", "exit_code": 0, "wall_seconds": 12.0, "sampled_peak_tree_rss_bytes": 1 << 30})
            BI.ledger_append(self.root / LEDGER, [admission_line(spec["name"], r["id"], role_sha=role_sha(self.root))
                                                  for r in listed], chain=True)
            return self.ok(args, self.gate_exit)
        nav = c.out(spec["nav"]["output"])
        if "--stop-after" in args:
            base = spec["name"][:-3] if spec["name"].endswith("-gm") else spec["name"]
            gross = self.gross_per_l.get(base, 0.9862 / 1.1474) * float(spec["nav"]["leverage"])
            drift = 3e-4 + int(sha(base.encode()), 16) % 100003 * 1e-9          # each cell its own series
            write_nav(self.root, nav, gross, accounting=self.accounting, drift=drift)
            run_dir, k = f"{nav}-run", 2                                 # research_cycle: -run, then -run<k>
            while (self.root / run_dir).exists():
                run_dir, k = f"{nav}-run{k}", k + 1
            write_json(self.root, f"{run_dir}/receipt.json", {"outcome": "completed", "exit_code": 0,
                                                              "wall_seconds": 41.5, "sampled_peak_tree_rss_bytes": 586 << 20})
            write_json(self.root, f"{run_dir}/cycle_binding.json",       # cycle_resume.write_binding's layout
                       {"schema": "atx.cycle-nav-binding/v1", "output": nav, "argv_sha256": sha(nav.encode()),
                        "spec_sha256": research_spec.spec_digest(self.root / rel, research_tree.REPO),
                        "spec_rule": "spec-digest-v1"})
            write(self.root, f"{run_dir}/stdout.log", "nav replay 2020-01-02 .. 2021-02-10\n")
            return self.ok(args)
        daily = (self.root / nav / "daily_s2.csv").read_bytes()
        write_json(self.root, f"{c.cycle_dir()}/summ.json", [{"dir": nav}])
        lib = self.root / "atx-impl/strategies/libraries" / f"{spec['name'].removesuffix('-gm')}.json"
        members = json.loads(lib.read_text())["members"] if lib.is_file() else []
        marginal = [{"id": cid, "ic21": 0.01, "ic21_hac_t": 2.0, "marginal_ic21": 0.004, "marginal_hac_t": 1.2,
                     "max_abs_rho": 0.3, "max_rho_member": "m1"} for cid in self.admission if cid in members] \
            if "marginal" in spec else []
        write_json(self.root, f"{c.cycle_dir()}/cycle_verdict.json",
                   {"admission": [], "paired": {"dsr": self.dsr, "se": 0.1, "cbb_ci": [-0.1, 0.2], "lw_p": 0.3},
                    "dsr": {"n": 3, "cell_count": 0.6}, "pbo": 0.2, "marginal": marginal})
        sr = json.loads((self.root / nav / "summary.json").read_text())["scenarios"][0]["net_sharpe"]
        BI.ledger_append(self.root / LEDGER, [construction_line(nav, sha(daily), sr)], chain=True)
        return self.ok(args)

    def add_alpha(self, args: list[str]):
        a = {args[k]: args[k + 1] for k in range(len(args) - 1) if args[k].startswith("--")}
        name, cid = a["--name"], a["--id"]
        lib = self.root / "atx-impl" / "strategies" / "libraries" / f"{name}.json"
        members = json.loads(lib.read_text())["members"] if lib.is_file() else ["m1", "m2"]
        st = "atx-impl/strategies"                                  # add-alpha's file set (research_add_alpha.add_alpha)
        write_json(self.root, f"{st}/libraries/{name}.json", {"id": f"ic_{name}", "members": members + [cid]})
        reg = self.root / st / "alphas" / "registry.json"
        alphas = json.loads(reg.read_text())["alphas"] if reg.is_file() else []
        alphas = [x for x in alphas if x["id"] != cid] + [{"id": cid, "dsl": a["--dsl"], "origin": a["--origin"]}]
        write_json(self.root, f"{st}/alphas/registry.json", {"alphas": alphas})
        write_json(self.root, f"{st}/ic_{name}.json", {"candidates": members + [cid]})
        write_json(self.root, f"{st}/ic_{name}.recipe.v2.json", {"members": members + [cid]})
        write(self.root, f"{st}/libraries/{name}.prereg.md", f"# {name}\n")
        pspec, pc = self.spec_outputs(a["--parent-spec"])
        write_json(self.root, f"scripts/specs/v8/lib-{name}.json",
                   cell_spec(name, f"out/nav-{name}-L1.1474", reference_nav=pc.out(pspec["nav"]["output"]),
                             leverage=str(pspec["nav"]["leverage"]), marginal_of=pspec["name"]))
        write_json(self.root, a["--save-plan"], {"library": name, "candidates": [{"id": cid}]})
        return self.ok(args)

    def bounded(self, argv: list[str]):
        k = argv.index("--")
        run_dir, cmd = argv[argv.index("--output") + 1], argv[k + 1:]
        (self.root / run_dir).mkdir(parents=True)
        if Path(cmd[1]).name == "wave_readers.py":
            cwd = os.getcwd()
            os.chdir(self.root)
            try:
                code = wave_readers.main(cmd[2:])
            finally:
                os.chdir(cwd)
        else:                                                     # nav_summ --bundle BASE FINAL --bundle-json OUT
            b = cmd.index("--bundle")
            write_json(self.root, cmd[cmd.index("--bundle-json") + 1],
                       {"base": cmd[b + 1], "final": cmd[b + 2],
                        "paired": {"dsr": self.dsr, "rho": 0.95, "sessions": 299, "memmel_se": 0.1,
                                   "cbb_ci95": [-0.1, 0.2], "lw": {"p_value": 0.3, "p_one_sided": 0.15,
                                                                   "ci95": [-0.1, 0.2]}},
                        "verdict": {"pass": False}})
            code = 0
        binds = [argv[i + 1] for i in range(k) if argv[i] == "--bind"]           # the runner records each bind
        bindings = [{"path": str((self.root / b).resolve()), "sha256": sha((self.root / b).read_bytes())}
                    for b in binds if (self.root / b).is_file()]
        write_json(self.root, f"{run_dir}/receipt.json", {"outcome": "completed", "exit_code": code,
                                                          "wall_seconds": 0.8, "sampled_peak_tree_rss_bytes": 300 << 20,
                                                          "bindings": bindings})
        write(self.root, f"{run_dir}/stdout.log", "reader\n")
        return self.ok(argv, code)
