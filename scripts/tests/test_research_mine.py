"""The v9 mined campaign cell (lane MINE-RUN): the spec template scripts/specs/v9/mine-c1.json and research_mine.py.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_mine.py

Synthetic inputs only: stand-in role / fields / pool files and a fake bounded runner + verb (an injected executor);
the registration's numbers are pinned against the C++ they mirror (strategy_mine.cpp, strategy_mine_pool.hpp), the
research window module and the ledger module. No real data, nothing dated 2024-01-01 or later.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(REPO / "atx-impl" / "tools"))
sys.path.insert(0, str(REPO / "atx-engine" / "tools"))
import backtest_integrity as BI  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_ledger  # noqa: E402
import research_mine as M  # noqa: E402
import research_window as RW  # noqa: E402
from test_research_ledger import grow_registry  # noqa: E402

# The registered template as committed before the lock (`23b52a5d`): the tests below pin the registration on it.
# The live spec is the campaign as locked and run in X batch 2 (v8x prereg section 9 A2-A4), pinned by
# test_the_live_spec_is_the_campaign_as_locked.
TEMPLATE = HERE / "fixtures" / "mine-c1.registered.json"
LIVE = REPO / "scripts" / "specs" / "v9" / "mine-c1.json"
VERB_CPP = REPO / "atx-impl" / "src" / "strategy_mine.cpp"
POOL_HPP = REPO / "atx-impl" / "src" / "strategy_mine_pool.hpp"
# The registration's field rule (prereg item 4): fields v9 rows that no registry alpha reads, minus these classes.
EXCLUDED_BY_CLASS = {
    "lt": "USD level (scale)", "noa_lag4": "USD level (scale)", "grp_sic2": "categorical code",
    "ea_time_of_day": "categorical code", "ea_window_pre5": "0/1 indicator", "ea_window_post3": "0/1 indicator",
    "ins_cluster_buy": "0/1 indicator", "k8_item_material_21": "0/1 indicator",
    "inst_n_holders": "count of 13F holders (scales with size)"}
HELD_BY_V8_LIBRARY = ("ea_days_since", "inst_own_share")   # library-v8-draft E1 (R-2): read by the book once accepted


def template() -> dict:
    return json.loads(TEMPLATE.read_text(encoding="utf-8"))


def verb_options() -> set[str]:
    """Every option dispatch_mine parses (`key == "--x"`)."""
    return set(re.findall(r'key == "(--[a-z0-9-]+)"', VERB_CPP.read_text(encoding="utf-8")))


# ------------------------------------------------------------------ the registration as data
def test_template_is_the_registration():
    """mine-c1.json loads and validates; it is not runnable as committed (OD-7 and the other preconditions are open,
    the memory cap and the frozen cell are to fill); stage 1 only, racing off, budget = capacity = 11 x 12 within the
    ceiling in force; the windows are the research window's TRAIN bounds split at 2023-01-01; the cap leaves the
    runner's headroom."""
    spec = M.load(TEMPLATE)
    assert spec["campaign_id"] == "v9-mine-c1" and (REPO / spec["registration"]).name == \
        "2026-10-01-v9-mine-campaign-prereg.md"
    assert any(r.startswith("OD-7") for r in spec["requires"]) and len(spec["requires"]) == 4
    fills = M.research_spec.fills(spec)
    assert len(fills) == 4 and spec["max_memory_mib"] in fills
    assert spec["search"]["stage2_generations"] == 0 and spec["search"]["race_strides"] == "none"
    assert M.PER_FIELD == 11 and M.capacity(spec) == 11 * len(spec["fields"]) == spec["budget"] == 132
    assert spec["budget"] <= BI.MINED_MAX_BUDGET
    assert M.windows(spec) == {"discover": (RW.TRAIN_BEGIN_DATE, "2023-01-01"),
                               "confirm": ("2023-01-01", RW.TRAIN_END_DATE)}
    assert spec["windows"]["discover"][0] == "{train_begin}" and spec["windows"]["confirm"][1] == "{train_end}"
    assert spec["runner"]["max_rss_mib"] == M.RUNNER_MAX_RSS_MIB and spec["runner"]["seconds"] == M.RUNNER_MAX_SECONDS
    assert spec["rule"] == {"min_names": 1000, "min_dates": 128, "max_promotions": 16}
    assert spec["registry"] == {"path": f"{spec['output']}/registry.atxtrg", "head": None}
    assert round(M.bonferroni_z(132), 4) == 3.5544


def test_the_live_spec_is_the_campaign_as_locked():
    """scripts/specs/v9/mine-c1.json as run (X batch 2): the registered template with A3's fields (the rule applied to
    the registry at H-F: the X members' fields left the list), budget 11 x fields, H-F's role / fields / pool source,
    every pin locked, the probe's cap at its workers, no open requires and nothing to fill; everything else is the
    template's."""
    live, reg = M.load(LIVE), template()
    registry = json.loads((REPO / "atx-impl" / "strategies" / "alphas" / "registry.json").read_text(encoding="utf-8"))
    read_x = set()
    for a in registry["alphas"]:
        if str(a.get("added_in", "")).startswith("v8x"):
            read_x.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", a["dsl"]))
    assert live["fields"] == [f for f in reg["fields"] if f not in read_x] and len(live["fields"]) == 10
    assert live["budget"] == M.capacity(live) == 11 * len(live["fields"]) == 110
    assert live["requires"] == [] and M.research_spec.fills(live) == []
    assert live["inputs"]["role"]["path"] == "build-equity/train-2020-2023-lo3/manifest.json"
    assert all(re.fullmatch(r"[0-9a-f]{64}", v["sha256"] or "") for s in ("inputs", "pool_source") for v in live[s].values())
    assert isinstance(live["max_memory_mib"], int) and live["max_memory_mib"] % 64 == 0 and \
        live["max_memory_mib"] <= 7680 and live["search"]["workers"] in (1, 2, 4)
    same = ("campaign_id", "registration", "python", "build", "exe", "runner", "windows", "rule", "registry", "output",
            "ledger")
    assert {k: live[k] for k in same} == {k: reg[k] for k in same}
    assert {k: v for k, v in live["search"].items() if k != "workers"} == \
        {k: v for k, v in reg["search"].items() if k != "workers"}


def test_fields_are_the_rule_applied_to_the_registry():
    """Prereg item 4: the mined fields are the fields v9 rows (the committed base-lo1 list) that no registry alpha
    reads, less the excluded classes and the two fields the v8.0 library wave adds; every one is a fields v9 row."""
    v9 = json.loads((REPO / "scripts" / "specs" / "v8" / "base-lo1.json").read_text(encoding="utf-8"))["fields"]["list"]
    registry = json.loads((REPO / "atx-impl" / "strategies" / "alphas" / "registry.json").read_text(encoding="utf-8"))
    read, read_x = set(), set()
    for a in registry["alphas"]:                # v8x prereg A3: the X members' fields leave the list at the lock (H-F)
        (read_x if str(a.get("added_in", "")).startswith("v8x") else read).update(
            re.findall(r"[A-Za-z_][A-Za-z0-9_]*", a["dsl"]))
    rule = [f for f in v9 if f not in read and f not in EXCLUDED_BY_CLASS and f not in HELD_BY_V8_LIBRARY]
    assert template()["fields"] == rule and len(rule) == 12
    assert [f for f in rule if f not in read_x] == [f for f in rule if f not in read | read_x]   # X only removes
    assert set(EXCLUDED_BY_CLASS) <= set(v9) - read
    assert set(HELD_BY_V8_LIBRARY) <= set(v9)            # R-2 was accepted: the book reads them (no longer unread)


def test_constants_mirror_the_verb():
    """The template windows, the per-field count and the pool's member bound are the C++'s."""
    cpp = VERB_CPP.read_text(encoding="utf-8")
    windows = re.search(r"kTemplateWindows\{([0-9, ]+)\}", cpp).group(1)
    assert tuple(int(x) for x in windows.split(",")) == M.TEMPLATE_WINDOWS
    assert re.search(r"kMaxMinePoolMembers = (\d+);", POOL_HPP.read_text(encoding="utf-8")).group(1) == \
        str(M.MAX_POOL_MEMBERS)
    assert f'"{M.POOL_SCHEMA}"' in (REPO / "atx-impl" / "src" / "strategy_mine_pool.cpp").read_text(encoding="utf-8")
    runner = (REPO / "scripts" / "run_bounded_research.py").read_text(encoding="utf-8")
    assert f"0 < args.seconds <= {M.RUNNER_MAX_SECONDS}" in runner and \
        f"32 <= args.max_rss_mib <= {M.RUNNER_MAX_RSS_MIB}" in runner


@pytest.mark.parametrize("edit, needle", [
    (lambda s: s.update(budget=131), "must cover the trial capacity 132"),
    (lambda s: s.update(budget=BI.MINED_MAX_BUDGET + 1), "at most the ceiling in force"),
    (lambda s: s["search"].update(stage2_generations=4), "must cover the trial capacity 228"),
    (lambda s: s["windows"].update(confirm=["2022-12-01", "{train_end}"]), "non-overlapping"),
    (lambda s: s["windows"].update(confirm=["2023-01-01", "2024-02-01"]), "inside TRAIN"),
    (lambda s: s["windows"].update(discover=["2019-12-01", "2023-01-01"]), "inside TRAIN"),
    (lambda s: s.update(max_memory_mib=7681), "max_memory_mib must be"),
    (lambda s: s.update(max_memory_mib=4096) or s["runner"].update(max_rss_mib=4096), "at least max_memory_mib"),
    (lambda s: s["runner"].update(seconds=601), "runner:"),
    (lambda s: s.update(fields=s["fields"] + ["iv_atm_63d"]), "distinct"),
    (lambda s: s.update(campaign_id="V9 C1"), "campaign_id"),
    (lambda s: s.update(extra=1), "unknown ['extra']"),
    (lambda s: s["inputs"].update(role={"path": "<fill:x>", "sha256": None}), "inputs.role"),
    (lambda s: s["rule"].update(max_promotions=0), "rule:"),
])
def test_validate_refuses_before_anything_runs(edit, needle):
    spec = template()
    edit(spec)
    with pytest.raises(RC.CycleError) as err:
        M.validate(spec)
    assert needle in str(err.value) and err.value.code == RC.EXIT_USAGE


def test_verb_argv_is_the_verbs_full_cli():
    """Every option the verb parses is passed exactly once with a value, except --registry-head (a new registry);
    the values are the registration's."""
    spec = template()
    spec["max_memory_mib"] = 7000
    pinned = {k: (spec["inputs"][k]["path"], c * 64, "x") for k, c in zip(M.INPUTS, "abc")}
    argv = M.verb_argv(spec, pinned)
    opts = argv[1::2]
    assert argv[0] == "build-equity/bin/atx-equity-strategy-mine.exe" and len(argv) % 2 == 1
    assert len(opts) == len(set(opts)) and set(opts) == verb_options() - {"--help", "--registry-head"}
    val = dict(zip(argv[1::2], argv[2::2]))
    assert val["--fields"] == ",".join(spec["fields"]) and val["--budget"] == "132" and val["--race-strides"] == "none"
    assert val["--stage2-generations"] == "0" and val["--min-names"] == "1000" and val["--max-memory-mib"] == "7000"
    assert val["--role-fields"] == "build-equity/train-2020-2023-lo1-fields-v9" and val["--role-fields-sha256"] == "b" * 64
    assert (val["--discover-begin"], val["--confirm-end"]) == (RW.TRAIN_BEGIN_DATE, RW.TRAIN_END_DATE)
    usage = re.search(r"kUsage =(.*?);\n", VERB_CPP.read_text(encoding="utf-8"), re.S).group(1)
    assert all(o in usage for o in opts)
    spec["registry"]["head"] = "build-equity/prev/registry_head.txt"
    assert "--registry-head" in M.verb_argv(spec, pinned)
    assert M.verb_argv(spec, pinned, workers=1, memory_mib=64)[-1] == "64"


# ------------------------------------------------------------------ a synthetic root
def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write(root: Path, rel: str, data) -> str:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    raw = data if isinstance(data, bytes) else (json.dumps(data, indent=2) + "\n").encode()
    p.write_bytes(raw)
    return sha(raw)


DATES, NAMES = 3, 2
CELL = DATES * NAMES * 8


def frozen_cell(root: Path, members: dict, role_sha: str) -> dict:
    """A frozen book cell as the IC runner and the fitter leave it: the w pass's saved combined signal and summary,
    the candidate cache v2 entries, the fit's composition weights. Returns {name: rel path}."""
    lib = "e" * 64
    weights_rel = "cell/fit/composition_weights.json"
    wsha = write(root, weights_rel, {"schema": "atx.dsl-composition-weights/v2", "library_sha256": lib,
                                     "weights": members})
    signal = bytes(range(CELL))
    combined = {"schema": M.COMBINED_SCHEMA, "status": "complete", "role": "train", "dates": DATES,
                "instruments": NAMES, "role_manifest_sha256": role_sha, "library_sha256": lib,
                "composition_weights_sha256": wsha,
                "files": {"train_combined.f64": {"bytes": CELL, "sha256": write(root, "cell/wt-1/train_combined.f64",
                                                                                  signal)}}}
    write(root, "cell/wt-1/train_combined.json", combined)
    entries = []
    for k, cid in enumerate(members):
        payload = bytes([k + 1]) * CELL
        stem = f"cache/role/fp_1/{cid}.{'0' * 16}"
        psha = write(root, f"{stem}.f64", payload)
        write(root, f"{stem}.json", {"schema": M.SIGNAL_SCHEMA, "candidate_id": cid, "role_manifest_sha256": role_sha,
                                     "dates": DATES, "instruments": NAMES, "bytes": CELL, "payload_sha256": psha})
        entries.append({"id": cid, "layout": "v2", "sidecar": f"{stem}.json", "payload": f"{stem}.f64",
                        "payload_sha256": psha})
    write(root, "cell/wt-1/summary.json", {"status": "complete", "roles": [{"candidate_cache": {"entries": entries}}]})
    return {"combined": "cell/wt-1/train_combined.json", "weights": weights_rel, "summary": "cell/wt-1/summary.json"}


def campaign_root(tmp_path: Path, members: dict | None = None) -> tuple[Path, Path, dict]:
    """A root with stand-in role and fields manifests, a frozen cell and a copy of the template pointing at them."""
    root = tmp_path / "root"
    spec = template()
    role_sha = write(root, spec["inputs"]["role"]["path"], {"schema": "stand-in role manifest"})
    write(root, spec["inputs"]["fields"]["path"], {"schema": "stand-in fields manifest"})
    src = frozen_cell(root, members or {"q_a": 0.5, "q_b": 0.0, "q_c": 0.5}, role_sha)
    for key, rel in src.items():
        spec["pool_source"][key]["path"] = rel
    sp = root / "scripts" / "specs" / "v9" / "mine-c1.json"
    write(root, "scripts/specs/v9/mine-c1.json", spec)
    return root, sp, spec


def lock_write(sp: Path, root: Path) -> dict:
    spec, _ = M.lock(sp, root)
    sp.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
    return spec


def verb_reads_pool(manifest: Path, role_sha: str) -> list[str]:
    """read_mine_pool_manifest + load_mine_pool's checks (strategy_mine_pool.cpp), in Python: what the verb accepts."""
    j = json.loads(manifest.read_text(encoding="utf-8"))
    assert j["schema"] == M.POOL_SCHEMA and j["status"] == "complete" and j["role_manifest_sha256"] == role_sha
    names = []
    for key, cap in (("regressors", 11), ("members", M.MAX_POOL_MEMBERS)):
        rows = j[key]
        assert len(rows) <= cap and len({r["name"] for r in rows}) == len(rows)
        for r in rows:
            assert M.FIELD_RE.fullmatch(r["name"]) and r["file"] == r["name"] + ".f64"
            assert r["bytes"] == j["dates"] * j["instruments"] * 8
            assert sha((manifest.parent / r["file"]).read_bytes()) == r["sha256"]
            names.append(f"{key}:{r['name']}")
    return names


def test_pool_is_the_frozen_books_composite_and_weighted_members(tmp_path, capsys):
    """`mine pool`: regressor book = the saved combined signal, members = the positive-weight candidates in the
    summary's order, each its cache payload; the verb's own manifest checks pass; nothing is overwritten."""
    root, sp, _ = campaign_root(tmp_path)
    spec = lock_write(sp, root)
    assert spec["inputs"]["pool"]["sha256"] is None and all(spec["pool_source"][k]["sha256"] for k in M.SOURCES)
    manifest = M.assemble_pool(spec, root, log=lambda *_: None)
    assert verb_reads_pool(manifest, spec["inputs"]["role"]["sha256"]) == ["regressors:book", "members:q_a",
                                                                          "members:q_c"]
    assert (manifest.parent / "book.f64").read_bytes() == (root / "cell/wt-1/train_combined.f64").read_bytes()
    assert RC.main(["mine", "lock", str(sp), "--root", str(root), "--write"]) == 0
    assert json.loads(sp.read_text())["inputs"]["pool"]["sha256"] == sha(manifest.read_bytes())
    with pytest.raises(RC.CycleError, match="never overwritten"):
        M.assemble_pool(M.load(sp), root)


@pytest.mark.parametrize("break_it, needle", [
    (lambda root, s: s["pool_source"]["weights"].update(sha256="0" * 64), "pins"),
    (lambda root, s: write(root, "cell/fit/composition_weights.json", {"schema": "atx.dsl-composition-weights/v2",
                                                                       "library_sha256": "e" * 64,
                                                                       "weights": {"q_a": 1.0}})
     and s["pool_source"]["weights"].update(sha256=sha((root / "cell/fit/composition_weights.json").read_bytes())),
     "not blended with pool_source.weights"),
    (lambda root, s: s["inputs"]["role"].update(sha256=None), "lock inputs.role first"),
    (lambda root, s: write(root, "cache/role/fp_1/q_c.0000000000000000.f64", bytes(CELL)), "does not hash"),
])
def test_pool_refuses_what_is_not_the_frozen_book(tmp_path, break_it, needle):
    root, sp, _ = campaign_root(tmp_path)
    spec = lock_write(sp, root)
    break_it(root, spec)
    with pytest.raises(RC.CycleError, match=needle):
        M.assemble_pool(spec, root, log=lambda *_: None)


def test_pool_refuses_a_member_without_its_cache_entry(tmp_path):
    root, sp, _ = campaign_root(tmp_path)
    summary = json.loads((root / "cell/wt-1/summary.json").read_text())
    summary["roles"][0]["candidate_cache"]["entries"].pop()                 # q_c, weighted, has no entry now
    write(root, "cell/wt-1/summary.json", summary)
    with pytest.raises(RC.CycleError, match=r"without a cache entry in the summary: \['q_c'\]"):
        M.assemble_pool(lock_write(sp, root), root, log=lambda *_: None)


# ------------------------------------------------------------------ probe, plan, run
def done(code: int, out: str = "", err: str = "") -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess([], code, out, err)


def ready(tmp_path: Path) -> tuple[Path, Path, dict]:
    """The campaign root with the pool built and locked, the fills filled and the preconditions met (a granted spec)."""
    root, sp, _ = campaign_root(tmp_path)
    M.assemble_pool(lock_write(sp, root), root, log=lambda *_: None)
    spec = lock_write(sp, root)
    spec["requires"], spec["max_memory_mib"] = [], 7000
    sp.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
    return root, sp, M.load(sp)


def test_probe_reads_the_verbs_required_bytes(tmp_path):
    root, _, spec = ready(tmp_path)
    calls = []

    def verb(argv, cwd, env, capture):
        calls.append(argv)
        w = int(argv[argv.index("--workers") + 1])
        return done(1, err=f"mine: required_bytes={w * (1 << 30) + 1} exceeds --max-memory-mib before any payload load")
    assert M.probe(spec, root, [4, 1], executor=verb, log=lambda *_: None) == {4: 4097, 1: 1025}
    assert all(a[a.index("--max-memory-mib") + 1] == "64" for a in calls)
    with pytest.raises(RC.CycleError, match="did not refuse"):
        M.probe(spec, root, [4], executor=lambda *a: done(0, out="mine: wrote"), log=lambda *_: None)
    # the direct launch resolves the verb against the root (Windows does not find "build-equity/bin/x.exe" itself)
    assert all(Path(a[0]) == root / "build-equity" / "bin" / "atx-equity-strategy-mine.exe" for a in calls)


def test_launchable_resolves_a_relative_verb_and_keeps_an_absolute_one(tmp_path):
    argv = ["build-equity/bin/atx-equity-strategy-mine.exe", "--help"]
    assert M.launchable(argv, tmp_path) == [str(tmp_path / "build-equity" / "bin" / "atx-equity-strategy-mine.exe"),
                                            "--help"]
    absolute = [str(tmp_path / "v.exe"), "--x"]
    assert M.launchable(absolute, Path("C:/elsewhere")) == absolute


def fake_campaign(root: Path, spec: dict, pinned: dict, distinct: int = 132, edit=None) -> None:
    """The verb's output directory as strategy_mine.cpp writes it, for a stage-1-only campaign of `distinct` trials
    on a new registry inside it (campaign.json, trials.csv, registry_head.txt, ledger_line.json). The recipe, the
    hurdle and the promotions carry lane MINE-STAT's keys (the factor tables, F of the budget's band, Fc per confirm
    read) and the trial counts MINE-16's rung-failed status; `edit(campaign)` breaks one for a test."""
    out, w = root / spec["output"], M.windows(spec)
    head, size = grow_registry(root, spec["registry"]["path"], distinct)
    recipe = {"schema": "atx.mine-trial/v1", "rule": "mined-v1", "role_manifest_sha256": pinned["role"][1],
              "fields_manifest_sha256": pinned["fields"][1], "pool_sha256": pinned["pool"][1],
              "window_id": BI.window_id(),
              "discover": {"begin": w["discover"][0], "end": w["discover"][1], "label_rows": 734},
              "confirm": {"begin": w["confirm"][0], "end": w["confirm"][1], "label_rows": 228},
              "overlap_bands": [[100, 1.47], [1000, 1.54], [10000, 1.63]],
              "confirm_bands": [[16, 1.77], [64, 1.96], [256, 2.15]], "max_budget": BI.MINED_MAX_BUDGET}
    chain = "0123456789abcdef"
    campaign = {"schema": "atx.mine-campaign/v1", "status": "complete", "campaign_id": spec["campaign_id"],
                "rule": "mined-v1", "budget": spec["budget"], "research_window": {"id": BI.window_id()},
                "inputs": {"fields": {"names": spec["fields"]}}, "recipe_sha256": None, "recipe": recipe,
                "search": {"capacity": M.capacity(spec), "stage2": None, "required_bytes": 7 << 30},
                "trials": {"distinct": distinct, "evaluated": distinct - 2, "screen_rejected": 2,
                           "racing_rejected": 0, "rung_failed": 0, "failed": 0},
                "registry": {"path": spec["registry"]["path"], "records": distinct, "chain": chain, "head": head,
                             "bytes": size, "n_raw": distinct, "new_records": distinct, "anchor": None},
                "hurdle": {"budget": spec["budget"], "t": M.bonferroni_z(spec["budget"]), "overlap_factor": 1.54,
                           "max_budget": BI.MINED_MAX_BUDGET, "reads": "f2 / overlap_factor"},
                "promotions": [{"dsl": "NOT TO BE PRINTED", "rho_read": True, "rho_pass": True, "confirm_read": True,
                                "confirm_factor": 1.77, "confirm_marginal_t": 9.87},
                               {"dsl": "NOT TO BE PRINTED EITHER", "rho_read": True, "rho_pass": False,
                                "confirm_read": False, "confirm_factor": None, "confirm_marginal_t": None}],
                "admitted": 1, "seconds": 300.0}
    if edit is not None:
        edit(campaign)
    recipe_sha = campaign["recipe_sha256"] = research_ledger.recipe_sha256(campaign["recipe"])  # as the verb hashes it
    write(root, f"{spec['output']}/campaign.json", campaign)
    rows = ["canon_hash,stage,status,reason,ic_mean,ic_t,f1,marginal_mean,marginal_t,f2,sign,dsl"]
    rows += [f"{k:016x},1,evaluated,,0.123,9.87,9.87,0.1,8.8,8.8,1,\"rank(x)\"" for k in range(distinct - 2)]
    rows += [f"{k:016x},1,screen-rejected,ic-undefined,,,,,,,,\"rank(y)\"" for k in range(2)]
    (out / "trials.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    (out / "registry_head.txt").write_text(f"ATXTRGH1 {distinct:x} {int(chain, 16):x} 1f\n", encoding="utf-8")
    line = BI.campaign_line(spec["campaign_id"], spec["registry"]["path"], head, distinct, registry_total=distinct,
                            registry_bytes=size, budget=spec["budget"], recipe_sha256=recipe_sha,
                            confirm={"begin": w["confirm"][0], "end": w["confirm"][1]}, research_window_id=BI.window_id())
    (out / "ledger_line.json").write_text(json.dumps(line, sort_keys=True, separators=(",", ":")) + "\n",
                                          encoding="utf-8")


def fake_tools(root: Path, spec: dict, distinct: int = 132, outcome: str = "completed", edit=None):
    """An executor standing in for the verb's --help, the bounded runner and the verb it runs."""
    calls = []
    usage = " ".join(sorted(verb_options()))

    def executor(argv, cwd, env, capture):
        calls.append(argv)
        if argv[1:] == ["--help"]:
            return done(0, out=usage)
        k = argv.index("--")
        run_dir = root / argv[argv.index("--output") + 1]
        run_dir.mkdir(parents=True)
        verb = argv[k + 1:]
        pinned = {key: (verb[verb.index(opt) + 1], verb[verb.index(opt + "-sha256") + 1], "") for key, opt in
                  (("role", "--role"), ("fields", "--role-fields"), ("pool", "--pool"))}
        if outcome == "completed":
            fake_campaign(root, spec, pinned, distinct, edit)
        (run_dir / "receipt.json").write_text(json.dumps({"outcome": outcome, "exit_code": 0 if outcome ==
                                                          "completed" else None, "wall_seconds": 300.0,
                                                          "sampled_peak_tree_rss_bytes": 7 << 30}))
        return done(0 if outcome == "completed" else 1)
    return executor, calls


def test_plan_prints_the_registration_and_the_lines(tmp_path, capsys):
    root, sp, _ = ready(tmp_path)
    assert RC.main(["mine", "plan", str(sp), "--root", str(root)]) == 0
    out = capsys.readouterr().out
    assert "budget 132" in out and "Bonferroni z 3.5544" in out and "stage 2 off" in out
    assert "ceiling in force 10000" in out and "F 1.54" in out and "raw discover t 5.4738" in out
    assert "Fc by m (m 1..16: 1.77; cap 16)" in out
    assert "== run (bounded)" in out and "run_bounded_research.py" in out and "ledger-campaign" in out
    assert "[locked, verified]" in out and "UNLOCKED" not in out


def test_run_refuses_until_granted_filled_and_locked(tmp_path):
    root, sp, _ = campaign_root(tmp_path)
    spec = M.load(sp)
    executor, calls = fake_tools(root, spec)
    with pytest.raises(RC.CycleError) as err:
        M.run(spec, sp, root, executor=executor, clean=lambda r: ([], []), log=lambda *_: None)
    text = str(err.value)
    assert err.value.code == RC.EXIT_PIN and "requires: OD-7" in text and "unfilled value <fill:" in text
    assert "inputs.role UNLOCKED" in text and "inputs.pool MISSING" in text and not calls
    root, sp, spec = ready(tmp_path / "granted")
    executor, calls = fake_tools(root, spec)
    with pytest.raises(RC.CycleError, match="not clean"):
        M.run(spec, sp, root, executor=executor, clean=lambda r: (["atx-impl/src/x.cpp"], []), log=lambda *_: None)
    assert not calls


def test_run_ledgers_the_campaign_then_prints_mechanics_only(tmp_path):
    """A granted run: the verb runs through the bounded runner with the registration's argv; the campaign line is
    appended (count 0, registry count 132) before anything else is read; the log carries counts and constants but no
    statistic; a second run is refused (the output exists)."""
    root, sp, spec = ready(tmp_path)
    executor, calls = fake_tools(root, spec)
    log: list[str] = []
    assert M.run(spec, sp, root, date="2026-10-02", executor=executor, clean=lambda r: ([], []),
                 log=log.append) == RC.EXIT_OK
    runner = calls[-1]
    assert runner[1] == "scripts/run_bounded_research.py" and runner[runner.index("--max-rss-mib") + 1] == "8192"
    assert runner[runner.index("--output") + 1] == "build-equity/mine-v9-c1-run"
    assert runner[runner.index("--") + 1:] == M.verb_argv(spec, M.pins(spec, RC.Resolver(root), "inputs"))
    records = BI.ledger_read(root / spec["ledger"])
    assert len(records) == 1 and records[0]["kind"] == "mining-campaign" and records[0]["count"] == 0
    assert records[0]["registry"]["count"] == 132 and records[0]["budget"] == 132 and records[0]["date"] == "2026-10-02"
    assert BI.trial_counts(records) == [0] and BI.campaign_registry_count(records) == 132
    text = "\n".join(log)
    assert "== ledger: appended" in text and \
        "distinct 132 = evaluated 130 + screen_rejected 2 + racing_rejected 0 + rung_failed 0 + failed 0" in text
    assert "status screen-rejected / ic-undefined: 2" in text and "hurdle z 3.554" in text
    assert "overlap factor 1.54, ceiling 10000" in text
    assert "NOT TO BE PRINTED" not in text and "9.87" not in text and "admitted" not in text
    # m (the reads reaching the confirm) and the rho step are results, read in the runbook's step 11
    assert "confirm_read" not in text and "rho_pass" not in text and "reads 1" not in text
    assert text.index("== ledger") < text.index("== mechanics")
    with pytest.raises(RC.CycleError, match="exists"):
        M.run(spec, sp, root, executor=executor, clean=lambda r: ([], []), log=lambda *_: None)


def test_run_stops_on_a_mechanics_miss_and_on_a_failed_receipt(tmp_path):
    root, sp, spec = ready(tmp_path / "a")
    executor, _ = fake_tools(root, spec, distinct=131)          # a stage-1 campaign missing a template
    with pytest.raises(RC.CycleError) as err:
        M.run(spec, sp, root, executor=executor, clean=lambda r: ([], []), log=lambda *_: None)
    assert err.value.code == RC.EXIT_STOP and "distinct trials != templates" in str(err.value)
    assert BI.ledger_read(root / spec["ledger"])[0]["registry"]["count"] == 131   # ledgered first, all the same
    root, sp, spec = ready(tmp_path / "b")
    executor, _ = fake_tools(root, spec, outcome="time-limit")
    with pytest.raises(RC.CycleError, match="outcome time-limit"):
        M.run(spec, sp, root, executor=executor, clean=lambda r: ([], []), log=lambda *_: None)
    assert not (root / spec["ledger"]).exists()


@pytest.mark.parametrize("edit, needle", [
    (lambda c: c["trials"].pop("rung_failed"), "trial statuses are not"),           # a verb before MINE-16
    (lambda c: c["trials"].update(rung_failed=1, evaluated=129), "racing-rejected or rung-failed"),
    (lambda c: c["hurdle"].update(overlap_factor=1.55), "overlap_factor is not F"),  # the pre-MINE-STAT constant
    (lambda c: c["recipe"].update(confirm_bands=[[16, 1.55]]), "recipe factor tables"),
    (lambda c: c["promotions"][0].update(confirm_factor=1.96), "Fc of the reads"),
    (lambda c: c["promotions"][1].update(confirm_factor=1.77), "Fc of the reads"),
    (lambda c: c["hurdle"].update(max_budget=1000), "budget ceiling"),
])
def test_run_stops_when_the_rule_constants_are_not_mined_v1s(tmp_path, edit, needle):
    """Lanes MINE-STAT and MINE-MEM as merged: the mechanics read hurdle.t, hurdle.overlap_factor (F of the budget's
    band), the recipe's factor tables and ceiling, Fc of every confirm read, and the five-status registry identity
    with rung-failed 0 when racing is off; a miss stops the run after the ledger line, before any statistic."""
    root, sp, spec = ready(tmp_path)
    executor, _ = fake_tools(root, spec, edit=edit)
    with pytest.raises(RC.CycleError) as err:
        M.run(spec, sp, root, executor=executor, clean=lambda r: ([], []), log=lambda *_: None)
    assert err.value.code == RC.EXIT_STOP and needle in str(err.value)
    assert "NOT TO BE PRINTED" not in str(err.value)


def test_factors_and_hurdle_of_the_registration():
    """Prereg items 5 and 7(d) at the merged head: F 1.54 at budget 132 (band 101..1,000), so the raw discover t is
    3.5544 x 1.54 = 5.4738; Fc 1.77 for every m the cap of 16 allows (raw confirm t 3.54); the ceiling 10,000; a band's
    top belongs to it and a count outside the table has no factor."""
    assert M.factor_tables() == (((100, 1.47), (1000, 1.54), (10000, 1.63)), ((16, 1.77), (64, 1.96), (256, 2.15)))
    assert M.max_budget() == 10000 and M.overlap_factor(132) == 1.54
    assert round(M.raw_hurdle(132), 4) == 5.4738 and round(M.raw_hurdle(228), 4) == 5.6913
    assert {M.confirm_factor(m) for m in range(1, 17)} == {1.77} and M.confirm_factor(17) == 1.96
    assert [M.overlap_factor(n) for n in (0, 1, 100, 101, 1000, 1001, 10000, 10001)] == \
        [None, 1.47, 1.47, 1.54, 1.54, 1.63, 1.63, None]
    assert M.confirm_factor(0) is None and M.confirm_factor(257) is None
    assert M.confirm_bands_line(16) == "m 1..16: 1.77" and M.confirm_bands_line(20) == "m 1..16: 1.77, m 17..20: 1.96"


def test_run_refuses_a_stale_verb(tmp_path):
    root, sp, spec = ready(tmp_path)
    executor, calls = fake_tools(root, spec)

    def stale(argv, cwd, env, capture):
        return done(0, out="--role --fields") if argv[1:] == ["--help"] else executor(argv, cwd, env, capture)
    with pytest.raises(RC.CycleError, match="does not offer"):
        M.run(spec, sp, root, executor=stale, clean=lambda r: ([], []), log=lambda *_: None)
    assert not calls


def test_wave_prints_one_add_alpha_line_per_admitted_member(tmp_path, capsys):
    """Prereg item 11: theme mined, tier C+, origin mined, prior sign 1 with the discover sign in the DSL; the ledger
    trial in the citation; no member, no wave."""
    root, sp, spec = ready(tmp_path)
    executor, _ = fake_tools(root, spec)
    M.run(spec, sp, root, executor=executor, clean=lambda r: ([], []), log=lambda *_: None)
    tid = BI.ledger_read(root / spec["ledger"])[0]["trial_id"]
    members = [{"id": "mined_00000000000000aa", "dsl": "rank(ts_mean(k8_count_63, 63))", "sign": 1},
               {"id": "mined_00000000000000bb", "dsl": "rank(delta(inst_own_chg_q, 21))", "sign": -1}]
    write(root, f"{spec['output']}/mined_members.json", {"schema": "atx.mined-members/v1", "campaign_id": "v9-mine-c1",
                                                         "rule": "mined-v1", "theme": "mined", "members": members})
    argv = ["mine", "wave", str(sp), "--root", str(root), "--parent", "v81", "--name", "v81m1", "--parent-spec",
            "scripts/specs/v8/lib-v81.json"]
    assert RC.main(argv) == 0
    lines = capsys.readouterr().out.strip().splitlines()
    assert len(lines) == 2 and all("add-alpha" in x and "--theme mined --tier C+ --prior-sign 1 --origin mined" in x
                                   and f"(ledger trial {tid})" in x and "--name v81m1" in x for x in lines)
    assert '--dsl "rank(ts_mean(k8_count_63, 63))"' in lines[0]
    assert '--dsl "(-1 * (rank(delta(inst_own_chg_q, 21))))"' in lines[1]
    write(root, f"{spec['output']}/mined_members.json", {"campaign_id": "v9-mine-c1", "theme": "mined", "members": []})
    assert RC.main(argv) == 0 and "admitted no member" in capsys.readouterr().out


def test_lock_fills_what_exists_and_refuses_a_changed_pin(tmp_path, capsys):
    root, sp, _ = campaign_root(tmp_path)
    assert RC.main(["mine", "lock", str(sp), "--root", str(root), "--write"]) == 0
    out = capsys.readouterr().out
    assert "locked inputs.role" in out and "missing (lock again once built): inputs.pool" in out
    write(root, "build-equity/train-2020-2023-lo1/manifest.json", {"schema": "edited"})
    assert RC.main(["mine", "lock", str(sp), "--root", str(root)]) == RC.EXIT_PIN
    assert RC.main(["mine", "lock", str(sp), "--root", str(root), "--relock"]) == 0
    assert "RELOCKED inputs.role" in capsys.readouterr().out
    spec = copy.deepcopy(json.loads(sp.read_text()))
    with pytest.raises(RC.CycleError, match="PIN MISMATCH inputs.role"):
        M.pins(spec, RC.Resolver(root), "inputs")
