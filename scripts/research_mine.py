"""research_cycle.py mine: one mined campaign (atx-equity-strategy-mine, rule mined-v1) as a pinned cell (v9, lane
MINE-RUN). The registration is docs/plans/2026-10-01-v9-mine-campaign-prereg.md; the runbook is
docs/plans/2026-10-01-v9-mine-campaign-runbook.md.

  research_cycle.py mine lock  SPEC [--write] [--relock] [--root R]
  research_cycle.py mine pool  SPEC [--root R]
  research_cycle.py mine probe SPEC [--root R] [--workers N ...]
  research_cycle.py mine plan  SPEC [--root R]
  research_cycle.py mine run   SPEC [--root R] [--date YYYY-MM-DD]
  research_cycle.py mine wave  SPEC --parent LIB --name LIB2 --parent-spec CELL_SPEC [--root R]

SPEC (schema atx.mine-campaign-spec/v1, e.g. scripts/specs/v9/mine-c1.json) is one campaign's registration as data:
  campaign_id, registration (the prereg file), requires (open preconditions: run refuses while any is listed), python,
  build (research_cycle BUILDS key) and exe (the verb; a bare name lives in the build's bin dir), runner {script,
  seconds, max_rss_mib, min_free_mib}, inputs {role, fields, pool} and pool_source {combined, weights, summary}, each
  {path, sha256 (null until `lock`)}, fields (the mined field names), windows {discover, confirm} ([begin, end) dates;
  "{train_begin}" and "{train_end}" are the research window's TRAIN bounds, never typed), budget, search {seed,
  workers, stage2_seeds, stage2_population, stage2_generations, race_strides ("none" or a list), race_keep}, rule
  {min_names, min_dates, max_promotions}, max_memory_mib (the verb's --max-memory-mib), registry {path, head}, output
  and ledger. A "<fill:...>" value is one root fills before `run` (research_spec.FILL).

lock   fills every null sha256 pin (inputs, pool_source) from its file; a path still to fill or a file not built yet (the
       pool before `pool`) is listed and left null; a pin that differs from its file is refused unless --relock.
pool   writes the atx.mine-pool/v1 manifest the verb reads (strategy_mine_pool.hpp) into dirname(inputs.pool.path): the
       regressor `book` is the source cell's saved combined signal (the K6 book composite, pool_source.combined) and the
       members are every library candidate with a positive weight in pool_source.weights (the pool's composition
       weights), each the candidate-cache payload that pool_source.summary (the IC pass's summary.json) names, in its
       order. Payloads are hard-linked (copied when a link is impossible) and re-hashed. Manifests and payload bytes
       only: nothing here reads or prints a statistic.
probe  runs the verb with --max-memory-mib 64 once per worker count: it refuses before any payload and names its
       required_bytes (run_mine), the campaign's footprint under the built memory model (lanes MINE-MEM, MINE-JOIN).
plan   prints the pins, the registration's numbers (capacity, budget, Bonferroni z of the budget, the overlap factor F
       of its band and the raw discover t it implies, the confirm factor Fc, windows resolved) and the exact run and
       ledger lines; executes nothing.
run    refuses an open `requires`, a value to fill, an unlocked or different pin, an existing output or a dirty tree;
       runs the verb through the bounded runner; then, before anything prints a statistic, appends the campaign's
       ledger line (research_ledger.campaign_record, Ruling E-33) and checks and prints the mechanics only (counts and
       the registry identity with its rung-failed status, registry, rule constants -- hurdle z, F of the budget's band,
       both factor tables, Fc of every confirm read -- and trial status / reason counts). A mechanics miss is a hard
       stop (exit 4). Promotions, mined_members.json and the numeric columns of trials.csv are read afterwards, in the
       runbook's order.
wave   prints (never runs) the one add-alpha wave of the admitted members (prereg item 11): theme mined, tier C+,
       origin mined, prior sign 1 with the discover sign embedded in the DSL ("(-1 * (dsl))" for a sign of -1).
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
from pathlib import Path
import re
import shutil
import sys
from statistics import NormalDist
from typing import NoReturn

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_cycle as RC  # noqa: E402
import research_ledger  # noqa: E402
import research_spec  # noqa: E402
import research_tree  # noqa: E402

SCHEMA = "atx.mine-campaign-spec/v1"
POOL_SCHEMA = "atx.mine-pool/v1"                       # strategy_mine_pool.hpp
COMBINED_SCHEMA = "atx.dsl-combined-signal/v1"         # strategy_ic_runner.cpp save_combined_artifact
WEIGHTS_SCHEMAS = ("atx.dsl-composition-weights/v1", "atx.dsl-composition-weights/v2")
SIGNAL_SCHEMA = "atx.dsl-candidate-signal/v2"          # the candidate cache sidecar (layout v2)
TEMPLATE_WINDOWS = (5, 21, 63, 126, 252)               # strategy_mine.cpp kTemplateWindows (pinned by the test)
PER_FIELD = 1 + 2 * len(TEMPLATE_WINDOWS)              # rank(f), rank(ts_mean(f, w)), rank(delta(f, w))
MAX_POOL_MEMBERS = 80                                  # kMaxMinePoolMembers (PM7-13): the rho check meets each
BOOK = "book"                                          # the pool's one regressor: the book composite
RUNNER_MAX_SECONDS, RUNNER_MAX_RSS_MIB = 600, 8192     # run_bounded_research.py's hard bounds
RSS_HEADROOM_MIB = 512                                 # runner cap minus the verb's cap: what the model does not count
PROBE_MIB = 64                                         # the verb's smallest --max-memory-mib: refuses before payload
FAMILY_ALPHA = 0.05                                    # strategy_mine_rule.hpp kMinedFamilyAlpha
CONFIRM_T = 2.0                                        # strategy_mine_rule.hpp kMinedConfirmT
INPUTS = ("role", "fields", "pool")
SOURCES = ("combined", "weights", "summary")
SEARCH_KEYS = ("seed", "workers", "stage2_seeds", "stage2_population", "stage2_generations", "race_strides",
               "race_keep")
RULE_KEYS = ("min_names", "min_dates", "max_promotions")
# campaign.json trials: the registry identity n_raw = the sum of these (review MINE-16 added rung_failed).
STATUS_KEYS = ("evaluated", "screen_rejected", "racing_rejected", "rung_failed", "failed")
REQUIRED = {"schema", "name", "campaign_id", "registration", "python", "build", "exe", "runner", "inputs",
            "pool_source", "fields", "windows", "budget", "search", "rule", "max_memory_mib", "registry", "output",
            "ledger"}
OPTIONAL = {"description", "requires"}
CAMPAIGN_RE = re.compile(r"[a-z0-9_-]{1,64}")          # strategy_mine.cpp safe_id
FIELD_RE = re.compile(r"[a-z_][a-z0-9_]{0,63}")        # ic_detail::field_identifier
SHA_RE = re.compile(r"[0-9a-f]{64}")
REQUIRED_BYTES = re.compile(r"required_bytes=(\d+)")


def fail(message: str, code: int = RC.EXIT_USAGE) -> NoReturn:
    raise RC.CycleError(f"mine: {message}", code)


def is_fill(value) -> bool:
    return isinstance(value, str) and value.startswith(research_spec.FILL)


def integer(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


# ------------------------------------------------------------------ spec
def window_tokens() -> dict:
    """{"{train_begin}": D, "{train_end}": D} from the research window module (task W0-1): never typed in a spec."""
    tools = str(research_tree.REPO / "atx-engine" / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import research_window as rw  # noqa: PLC0415
    return {"{train_begin}": rw.TRAIN_BEGIN_DATE, "{train_end}": rw.TRAIN_END_DATE}


def windows(spec: dict) -> dict:
    """{"discover": (begin, end), "confirm": (begin, end)} with the TRAIN tokens resolved."""
    tokens = window_tokens()
    return {k: tuple(tokens.get(d, d) for d in spec["windows"][k]) for k in ("discover", "confirm")}


def capacity(spec: dict) -> int:
    """mine_trial_capacity: the templates plus, when stage 2 runs, its population times its generations."""
    s = spec["search"]
    stage2 = s["stage2_seeds"] > 0 and s["stage2_generations"] > 0
    return PER_FIELD * len(spec["fields"]) + (s["stage2_population"] * s["stage2_generations"] if stage2 else 0)


def bonferroni_z(budget: int) -> float:
    """mined_hurdle(N) = -norm_ppf(.05 / (2 N)); the verb reads every discover f2 as f2 / F against it."""
    return -NormalDist().inv_cdf(FAMILY_ALPHA / (2.0 * budget))


def max_budget() -> int:
    """The budget ceiling in force (backtest_integrity.MINED_MAX_BUDGET, the twin of kMinedMaxBudget)."""
    return research_ledger.backtest_integrity().MINED_MAX_BUDGET


def factor_tables() -> tuple[tuple, tuple]:
    """(overlap bands, confirm bands), each ((top, factor), ...): mined-v1's label-overlap tables as
    atx-impl/tools/mine_overlap_factor.py pins them (OVERLAP_BANDS, CONFIRM_BANDS; its test pins them to
    strategy_mine_rule.hpp kMinedOverlapBands and kMinedConfirmBands). Imported on first use, as backtest_integrity."""
    tools = str(research_ledger.TOOLS)
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import mine_overlap_factor as MOF  # noqa: PLC0415
    return MOF.OVERLAP_BANDS, MOF.CONFIRM_BANDS


def band_factor(bands, count: int) -> float | None:
    """strategy_mine_rule.cpp band_factor: the factor of the first band whose top is at or above `count`; None for 0
    or a count above the last top (the verb reads NaN there, which no hurdle passes)."""
    if count >= 1:
        for top, factor in bands:
            if count <= top:
                return factor
    return None


def overlap_factor(budget: int) -> float | None:
    """F of the budget's band (mined_overlap_factor): the discover hurdle reads f2 / F >= z(budget)."""
    return band_factor(factor_tables()[0], budget)


def confirm_factor(reads: int) -> float | None:
    """Fc of m confirm reads (mined_confirm_factor): the confirm read is t / Fc, BY over the m reads."""
    return band_factor(factor_tables()[1], reads)


def raw_hurdle(budget: int) -> float:
    """The raw marginal HAC t the discover hurdle asks for: z(budget) x F(budget)."""
    factor = overlap_factor(budget)
    if factor is None:
        fail(f"budget {budget} is outside the overlap table (1..{max_budget()})")
    return bonferroni_z(budget) * factor


def check_item(where: str, item, fill_ok: bool) -> None:
    if not isinstance(item, dict) or set(item) != {"path", "sha256"} or not isinstance(item["path"], str) or \
            not item["path"] or (is_fill(item["path"]) and not fill_ok):
        fail(f"{where} must be {{path, sha256}} with a path")
    if item["sha256"] is not None and not (isinstance(item["sha256"], str) and SHA_RE.fullmatch(item["sha256"])):
        fail(f"{where}.sha256 must be null or a SHA-256 hex digest")


def validate(spec: dict) -> dict:
    """The spec, checked as the verb and the bounded runner would check its values (refused before anything runs)."""
    if not isinstance(spec, dict) or spec.get("schema") != SCHEMA:
        fail(f"schema must be {SCHEMA}")
    missing, unknown = REQUIRED - set(spec), set(spec) - REQUIRED - OPTIONAL
    if missing or unknown:
        fail(f"keys: missing {sorted(missing)}, unknown {sorted(unknown)}")
    if not CAMPAIGN_RE.fullmatch(str(spec["campaign_id"])):
        fail("campaign_id must match [a-z0-9_-]{1,64}")
    if spec["build"] not in RC.BUILDS:
        fail(f"build must be one of {', '.join(RC.BUILDS)}")
    if not isinstance(spec.get("requires", []), list) or not all(isinstance(r, str) for r in spec.get("requires", [])):
        fail("requires must be a list of strings")
    if not isinstance(spec["inputs"], dict) or set(spec["inputs"]) != set(INPUTS):
        fail(f"inputs must be exactly {', '.join(INPUTS)}")
    for key in INPUTS:
        check_item(f"inputs.{key}", spec["inputs"][key], fill_ok=False)
    if not isinstance(spec["pool_source"], dict) or set(spec["pool_source"]) != set(SOURCES):
        fail(f"pool_source must be exactly {', '.join(SOURCES)}")
    for key in SOURCES:
        check_item(f"pool_source.{key}", spec["pool_source"][key], fill_ok=True)
    fields = spec["fields"]
    if not isinstance(fields, list) or not 1 <= len(fields) <= 64 or len(set(fields)) != len(fields) or \
            not all(isinstance(f, str) and FIELD_RE.fullmatch(f) for f in fields):
        fail("fields must be 1..64 distinct field identifiers")
    s, r = spec["search"], spec["rule"]
    if not isinstance(s, dict) or set(s) != set(SEARCH_KEYS) or not all(integer(s[k]) for k in SEARCH_KEYS[:5]):
        fail(f"search must be exactly {', '.join(SEARCH_KEYS)} (integers but race_strides, race_keep)")
    strides = s["race_strides"]
    if not (strides == "none" or (isinstance(strides, list) and 1 <= len(strides) <= 2 and
                                  all(integer(x) and 1 <= x <= 1024 for x in strides))):
        fail('search.race_strides must be "none" or 1..2 strides in 1..1024')
    if not (s["seed"] >= 0 and 1 <= s["workers"] <= 64 and 2 <= s["stage2_population"] <= 4096 and
            0 <= s["stage2_seeds"] <= s["stage2_population"] and 0 <= s["stage2_generations"] <= 256 and
            isinstance(s["race_keep"], (int, float)) and 0 < s["race_keep"] <= 1):
        fail("search: workers 1..64, stage2_seeds <= stage2_population (2..4096), stage2_generations 0..256, "
             "race_keep in (0, 1]")
    if not isinstance(r, dict) or set(r) != set(RULE_KEYS) or not all(integer(r[k]) for k in RULE_KEYS) or \
            not (r["min_names"] >= 3 and r["min_dates"] >= 8 and 1 <= r["max_promotions"] <= 256):
        fail("rule: min_names >= 3, min_dates >= 8, max_promotions 1..256")
    budget, cap, ceiling = spec["budget"], capacity(spec), max_budget()
    if not integer(budget) or not cap <= budget <= ceiling:
        fail(f"budget {budget!r} must cover the trial capacity {cap} (templates {PER_FIELD} x {len(fields)} fields "
             f"plus stage 2) and be at most the ceiling in force {ceiling} (kMinedMaxBudget)")
    w = spec["windows"]
    if not isinstance(w, dict) or set(w) != {"discover", "confirm"} or \
            not all(isinstance(w[k], list) and len(w[k]) == 2 for k in w):
        fail("windows must be {discover: [begin, end], confirm: [begin, end]}")
    tokens = window_tokens()
    try:
        d0, d1, c0, c1 = (dt.date.fromisoformat(tokens.get(x, x)) for x in w["discover"] + w["confirm"])
        lo, hi = (dt.date.fromisoformat(tokens[k]) for k in ("{train_begin}", "{train_end}"))
    except (TypeError, ValueError):
        fail("window dates must be YYYY-MM-DD or {train_begin} / {train_end}")
    if not lo <= d0 < d1 <= c0 < c1 <= hi:
        fail(f"windows must be chronological, non-overlapping and inside TRAIN [{lo}, {hi})")
    run = spec["runner"]
    if not isinstance(run, dict) or set(run) != {"script", "seconds", "max_rss_mib", "min_free_mib"} or \
            not integer(run["seconds"]) or not 1 <= run["seconds"] <= RUNNER_MAX_SECONDS or \
            not integer(run["min_free_mib"]) or not 64 <= run["min_free_mib"] <= RUNNER_MAX_RSS_MIB or \
            not (is_fill(run["max_rss_mib"]) or (integer(run["max_rss_mib"]) and
                                                  32 <= run["max_rss_mib"] <= RUNNER_MAX_RSS_MIB)):
        fail(f"runner: script, seconds 1..{RUNNER_MAX_SECONDS}, max_rss_mib 32..{RUNNER_MAX_RSS_MIB}, "
             f"min_free_mib 64..{RUNNER_MAX_RSS_MIB} (run_bounded_research.py's bounds)")
    mem = spec["max_memory_mib"]
    if not is_fill(mem):
        if not integer(mem) or not PROBE_MIB <= mem <= RUNNER_MAX_RSS_MIB - RSS_HEADROOM_MIB:
            fail(f"max_memory_mib must be {PROBE_MIB}..{RUNNER_MAX_RSS_MIB - RSS_HEADROOM_MIB} (the runner's cap "
                 f"less {RSS_HEADROOM_MIB} MiB the model does not count)")
        if integer(run["max_rss_mib"]) and run["max_rss_mib"] < mem + RSS_HEADROOM_MIB:
            fail(f"runner.max_rss_mib must be at least max_memory_mib + {RSS_HEADROOM_MIB}")
    reg = spec["registry"]
    if not isinstance(reg, dict) or set(reg) != {"path", "head"} or not isinstance(reg["path"], str) or \
            not (reg["head"] is None or isinstance(reg["head"], str)):
        fail("registry must be {path, head (null for a new registry, else the previous campaign's "
             "registry_head.txt)}")
    return spec


def load(path: Path) -> dict:
    try:
        return validate(json.loads(Path(path).read_text(encoding="utf-8")))
    except (OSError, ValueError) as exc:
        fail(f"spec {path}: {exc}")


def exe_path(spec: dict) -> str:
    exe = spec["exe"]
    return exe if "/" in exe or "\\" in exe else f"{RC.BUILDS[spec['build']]['bin']}/{exe}"


def launchable(argv: list[str], root: Path) -> list[str]:
    """`argv` for a direct launch of the verb (probe, --help): argv[0] resolved against `root`. Windows CreateProcess
    does not find a relative path written with '/' (WinError 2, found at the v8x campaign's probe); the bounded runner
    resolves its command itself (shutil.which), so the run's argv and every printed line keep the spec's spelling."""
    exe = Path(argv[0])
    return [str(exe if exe.is_absolute() else Path(root) / exe)] + list(argv[1:])


def env(spec: dict) -> dict:
    out = dict(os.environ)
    out["PATH"] = os.pathsep.join([str(Path(p)) for p in RC.BUILDS[spec["build"]]["path"]] + [out.get("PATH", "")])
    return out


# ------------------------------------------------------------------ pins
def pins(spec: dict, res: RC.Resolver, section: str) -> dict:
    """{key: (path, sha or None, state)}; a locked pin must equal its file (else exit 3)."""
    out: dict[str, tuple] = {}
    for key, item in spec[section].items():
        if is_fill(item["path"]):
            out[key] = (item["path"], None, "TO FILL")
            continue
        got = res.sha(item["path"])
        if item["sha256"] is None:
            out[key] = (item["path"], got, "UNLOCKED" if got else "MISSING")
        elif got is None:
            fail(f"{section}.{key} missing: {item['path']}", RC.EXIT_PIN)
        elif got != item["sha256"]:
            fail(f"PIN MISMATCH {section}.{key}: {item['path']} is {got}, spec pins {item['sha256']}", RC.EXIT_PIN)
        else:
            out[key] = (item["path"], got, "locked, verified")
    return out


def lock(spec_path: Path, root: Path, relock: bool = False) -> tuple[dict, list[str]]:
    raw = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    validate(raw)
    res, notes = RC.Resolver(root), []
    for section in ("inputs", "pool_source"):
        for key, item in raw[section].items():
            if is_fill(item["path"]):
                notes.append(f"to fill: {section}.{key} {item['path']}")
                continue
            got = res.sha(item["path"])
            if got is None:
                notes.append(f"missing (lock again once built): {section}.{key} {item['path']}")
            elif item["sha256"] is None:
                item["sha256"] = got
                notes.append(f"locked {section}.{key}: {item['path']} {got}")
            elif item["sha256"] != got:
                if not relock:
                    fail(f"lock: {section}.{key} pin {item['sha256']} differs from the file ({got}); --relock to "
                         "replace", RC.EXIT_PIN)
                notes.append(f"RELOCKED {section}.{key}: {item['path']} {item['sha256']} -> {got}")
                item["sha256"] = got
    return raw, notes


# ------------------------------------------------------------------ pool
def read_pinned_json(res: RC.Resolver, item: dict, what: str) -> dict:
    if item["sha256"] is None or is_fill(item["path"]):
        fail(f"pool: {what} is not locked (fill its path, then `mine lock --write`)", RC.EXIT_PIN)
    got = res.sha(item["path"])
    if got != item["sha256"]:
        fail(f"pool: {what} {item['path']} is {got}, the spec pins {item['sha256']}", RC.EXIT_PIN)
    doc = res.read_json(item["path"])
    if not isinstance(doc, dict):
        fail(f"pool: {what} {item['path']} is not a JSON object")
    return doc


def summary_entries(summary: dict) -> list[dict]:
    """The candidate-cache entries an IC pass names (summary.json roles[].candidate_cache.entries): one role."""
    roles = [r for r in summary.get("roles") or [] if isinstance(r, dict) and "candidate_cache" in r]
    if summary.get("status") != "complete" or len(roles) != 1:
        fail("pool: the summary must be a complete IC pass with one role naming its candidate cache")
    entries = roles[0]["candidate_cache"].get("entries")
    if not isinstance(entries, list):
        fail("pool: the summary's candidate_cache has no entries list")
    return entries


def link_or_copy(src: Path, dst: Path) -> str:
    try:
        os.link(src, dst)
        return "linked"
    except OSError:
        shutil.copyfile(src, dst)
        return "copied"


def assemble_pool(spec: dict, root: Path, log=print) -> Path:
    """The atx.mine-pool/v1 manifest of the source cell's book (see the module doc); returns its path."""
    res, src = RC.Resolver(root), spec["pool_source"]
    if spec["inputs"]["role"]["sha256"] is None:
        fail("pool: lock inputs.role first (the pool is bound to it)", RC.EXIT_PIN)
    role_pin = pins(spec, res, "inputs")["role"][1]
    combined = read_pinned_json(res, src["combined"], "pool_source.combined")
    weights = read_pinned_json(res, src["weights"], "pool_source.weights")
    summary = read_pinned_json(res, src["summary"], "pool_source.summary")
    axes = (combined.get("dates"), combined.get("instruments"))
    if combined.get("schema") != COMBINED_SCHEMA or combined.get("status") != "complete" or \
            not all(integer(x) and x > 0 for x in axes):
        fail(f"pool: pool_source.combined is not a complete {COMBINED_SCHEMA} manifest")
    dates, names = int(axes[0]), int(axes[1])
    if combined.get("role_manifest_sha256") != role_pin:
        fail("pool: the combined signal is bound to another role than inputs.role", RC.EXIT_PIN)
    if combined.get("composition_weights_sha256") != src["weights"]["sha256"]:
        fail("pool: the combined signal was not blended with pool_source.weights (composition_weights_sha256)",
             RC.EXIT_PIN)
    if weights.get("schema") not in WEIGHTS_SCHEMAS or weights.get("library_sha256") != combined.get("library_sha256") \
            or not isinstance(weights.get("weights"), dict):
        fail("pool: pool_source.weights is not the combined signal's composition weights file")
    cells_bytes = dates * names * 8
    files = combined.get("files") or {}
    signal = [n for n in files if n.endswith("_combined.f64")]
    if len(signal) != 1 or (files[signal[0]] or {}).get("bytes") != cells_bytes:
        fail("pool: the combined manifest names no single D x N f64 signal")
    base = res.path(src["combined"]["path"]).parent
    rows = [("regressors", BOOK, base / signal[0], files[signal[0]]["sha256"])]
    weighted = {k for k, w in weights["weights"].items() if isinstance(w, (int, float)) and w > 0}
    seen = set()
    for entry in summary_entries(summary):
        cid = entry.get("id")
        if cid not in weighted:
            continue
        if entry.get("layout") != "v2" or not FIELD_RE.fullmatch(str(cid)) or cid in seen:
            fail(f"pool: member {cid!r}: a v2 cache entry with a field-identifier id, once")
        sidecar = json.loads(res.path(entry["sidecar"]).read_text(encoding="utf-8"))
        if sidecar.get("schema") != SIGNAL_SCHEMA or sidecar.get("candidate_id") != cid or \
                sidecar.get("role_manifest_sha256") != role_pin or sidecar.get("dates") != dates or \
                sidecar.get("instruments") != names or sidecar.get("bytes") != cells_bytes or \
                sidecar.get("payload_sha256") != entry.get("payload_sha256"):
            fail(f"pool: the cache sidecar of {cid} does not describe its D x N signal on inputs.role")
        seen.add(cid)
        rows.append(("members", cid, res.path(entry["payload"]), entry["payload_sha256"]))
    if seen != weighted:
        fail(f"pool: weighted members without a cache entry in the summary: {sorted(weighted - seen)}")
    if not 1 <= len(seen) <= MAX_POOL_MEMBERS:
        fail(f"pool: {len(seen)} weighted members; mined-v1 checks rho against every member and the verb holds "
             f"1..{MAX_POOL_MEMBERS}")
    manifest_path = res.path(spec["inputs"]["pool"]["path"])
    out = manifest_path.parent
    if out.exists():
        fail(f"pool: {out} exists (never overwritten)", RC.EXIT_STOP)
    out.mkdir(parents=True)
    doc = {"schema": POOL_SCHEMA, "status": "complete", "role_manifest_sha256": role_pin, "dates": dates,
           "instruments": names, "regressors": [], "members": [],
           "source": {k: dict(src[k]) for k in SOURCES}}
    for section, name, path, sha in rows:
        dst = out / f"{name}.f64"
        how = link_or_copy(path, dst)
        got = RC.sha256_file(dst)
        if got != sha or dst.stat().st_size != cells_bytes:
            fail(f"pool: {path} does not hash to its pin {sha} (or its extent differs)", RC.EXIT_PIN)
        doc[section].append({"name": name, "file": dst.name, "sha256": sha, "bytes": cells_bytes})
        log(f"   {section[:-1]} {name}: {how} {path}")
    manifest_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    log(f"pool: wrote {manifest_path} (1 regressor, {len(doc['members'])} members); `mine lock --write` pins it")
    return manifest_path


# ------------------------------------------------------------------ argv
def verb_argv(spec: dict, pinned: dict, workers: int | None = None, memory_mib: int | None = None) -> list[str]:
    """The verb's whole command line (strategy_mine.cpp dispatch_mine) from the spec; `pinned` = pins(spec, inputs)."""
    w, s, r = windows(spec), spec["search"], spec["rule"]
    fields = spec["inputs"]["fields"]["path"]
    strides = s["race_strides"]

    def pin(key: str) -> str:
        return pinned[key][1] or f"<sha256:{key} missing>"
    argv = [exe_path(spec),
            "--role", spec["inputs"]["role"]["path"], "--role-sha256", pin("role"),
            "--role-fields", Path(fields).parent.as_posix(), "--role-fields-sha256", pin("fields"),
            "--fields", ",".join(spec["fields"]),
            "--discover-begin", w["discover"][0], "--discover-end", w["discover"][1],
            "--confirm-begin", w["confirm"][0], "--confirm-end", w["confirm"][1],
            "--registry", spec["registry"]["path"]]
    if spec["registry"]["head"]:
        argv += ["--registry-head", spec["registry"]["head"]]
    argv += ["--campaign-id", spec["campaign_id"], "--output", spec["output"], "--budget", str(spec["budget"]),
             "--pool", spec["inputs"]["pool"]["path"], "--pool-sha256", pin("pool"),
             "--seed", str(s["seed"]), "--workers", str(s["workers"] if workers is None else workers),
             "--stage2-seeds", str(s["stage2_seeds"]), "--stage2-population", str(s["stage2_population"]),
             "--stage2-generations", str(s["stage2_generations"]),
             "--race-strides", "none" if strides == "none" else ",".join(str(x) for x in strides),
             "--race-keep", repr(float(s["race_keep"])),
             "--min-names", str(r["min_names"]), "--min-dates", str(r["min_dates"]),
             "--max-promotions", str(r["max_promotions"]),
             "--max-memory-mib", str(spec["max_memory_mib"] if memory_mib is None else memory_mib)]
    return argv


def receipt_dir(spec: dict) -> str:
    return f"{spec['output']}-run"


def runner_argv(spec: dict, spec_path: Path, verb: list[str]) -> list[str]:
    run = spec["runner"]
    argv = [spec["python"], run["script"], "--seconds", str(run["seconds"]), "--max-rss-mib", str(run["max_rss_mib"]),
            "--min-free-mib", str(run["min_free_mib"]), "--output", receipt_dir(spec)]
    for key in INPUTS:
        argv += ["--bind", spec["inputs"][key]["path"]]
    argv += ["--bind", str(Path(spec_path).resolve())]
    return argv + ["--"] + verb


def ledger_argv(spec: dict, date: str | None = None) -> list[str]:
    argv = [spec["python"], "scripts/research_cycle.py", "ledger-campaign", "--ledger", spec["ledger"], "--campaign",
            spec["output"]]
    return argv + (["--date", date] if date else [])


def confirm_bands_line(cap: int) -> str:
    """The confirm factors a campaign capped at `cap` reads can meet (m = the reads reaching the confirm <= cap)."""
    out, low = [], 1
    for top, factor in factor_tables()[1]:
        if low > cap:
            break
        out.append(f"m {low}..{min(top, cap)}: {factor}")
        low = top + 1
    return ", ".join(out)


def header(spec: dict, spec_path: Path, res: RC.Resolver) -> list[str]:
    w, budget = windows(spec), spec["budget"]
    cap = spec["rule"]["max_promotions"]
    lines = [f"# mine campaign {spec['campaign_id']} ({spec_path}); registration {spec['registration']}",
             f"# fields {len(spec['fields'])}: {', '.join(spec['fields'])}",
             f"# capacity {capacity(spec)} (templates {PER_FIELD} x {len(spec['fields'])}"
             f"{' + stage 2' if capacity(spec) > PER_FIELD * len(spec['fields']) else ', stage 2 off'}); budget "
             f"{budget} (ceiling in force {max_budget()}); Bonferroni z {bonferroni_z(budget):.4f} = mined_hurdle("
             f"{budget}), read on f2 / F, F {overlap_factor(budget)} (kMinedOverlapBands at {budget}): raw discover t "
             f"{raw_hurdle(budget):.4f}",
             f"# confirm: t / Fc >= {CONFIRM_T} and BY p <= .10 over the m reads, Fc by m ({confirm_bands_line(cap)}; "
             f"cap {cap})",
             f"# discover [{w['discover'][0]}, {w['discover'][1]}), confirm [{w['confirm'][0]}, {w['confirm'][1]})",
             f"# memory: --max-memory-mib {spec['max_memory_mib']}, runner {spec['runner']['max_rss_mib']} MiB / "
             f"{spec['runner']['seconds']} s; registry {spec['registry']['path']} "
             f"({'anchored ' + spec['registry']['head'] if spec['registry']['head'] else 'new'}); ledger {spec['ledger']}"]
    for section in ("inputs", "pool_source"):
        for key, (path, sha, state) in pins(spec, res, section).items():
            lines.append(f"# {section}.{key}: {path} {sha or '-'} [{state}]")
    lines += [f"# requires (run refuses while listed): {r}" for r in spec.get("requires", [])]
    lines += [f"# fill (root fills before `run`): {x}" for x in research_spec.fills(spec)]
    return lines


def plan_lines(spec: dict, spec_path: Path, root: Path) -> list[str]:
    res = RC.Resolver(root)
    pinned = pins(spec, res, "inputs")
    return header(spec, spec_path, res) + [
        "== probe (metadata only; the verb refuses before any payload and names required_bytes)",
        RC.fmt_argv(verb_argv(spec, pinned, memory_mib=PROBE_MIB)),
        "== run (bounded)", RC.fmt_argv(runner_argv(spec, spec_path, verb_argv(spec, pinned))),
        "== ledger (run appends it before any statistic is printed)", RC.fmt_argv(ledger_argv(spec))]


# ------------------------------------------------------------------ probe
def probe(spec: dict, root: Path, workers: list[int], executor=RC.execute, log=print) -> dict:
    """{workers: required MiB} from the verb's own refusal at --max-memory-mib 64 (no payload is opened)."""
    pinned = pins(spec, RC.Resolver(root), "inputs")
    if any(sha is None for _, sha, _ in pinned.values()):
        fail("probe: inputs.role, inputs.fields and inputs.pool must exist (build the pool first)", RC.EXIT_PIN)
    if RC.Resolver(root).path(spec["output"]).exists():
        fail(f"probe: output {spec['output']} exists (the verb refuses it first)", RC.EXIT_STOP)
    out = {}
    for w in workers:
        done = executor(launchable(verb_argv(spec, pinned, workers=w, memory_mib=PROBE_MIB), root), root, env(spec),
                        True)
        text = (done.stdout or "") + (done.stderr or "")
        m = REQUIRED_BYTES.search(text)
        if done.returncode == 0 or m is None:
            fail(f"probe: the verb did not refuse with required_bytes at workers {w} (exit {done.returncode}): "
                 f"{text.strip()[-400:]}", RC.EXIT_STOP)
        out[w] = -(-int(m.group(1)) // (1 << 20))
        log(f"probe: workers {w}: required_bytes {m.group(1)} = {out[w]} MiB")
    return out


# ------------------------------------------------------------------ run
def run_refusal(spec: dict, root: Path) -> list[str]:
    res, why = RC.Resolver(root), []
    why += [f"requires: {r}" for r in spec.get("requires", [])]
    why += [f"unfilled value {x}" for x in research_spec.fills(spec)]
    for section in ("inputs", "pool_source"):
        why += [f"{section}.{k} {state}: {p}" for k, (p, _, state) in pins(spec, res, section).items()
                if state != "locked, verified"]
    for rel in (spec["output"], receipt_dir(spec)):
        if res.path(rel).exists():
            why.append(f"output {rel} exists (never overwritten)")
    reg, head = res.path(spec["registry"]["path"]).exists(), spec["registry"]["head"]
    if reg != bool(head):
        why.append("registry: an existing registry needs registry.head (its registry_head.txt), a new one none")
    return why


def exe_offers(spec: dict, root: Path, argv: list[str], executor) -> None:
    """The built verb's --help names every option the spec passes (a stale exe is refused before the run)."""
    done = executor(launchable([argv[0], "--help"], root), root, env(spec), True)
    text = (done.stdout or "") + (done.stderr or "")
    absent = [a for a in argv[1:] if a.startswith("--") and a not in text]
    if done.returncode != 0 or absent:
        fail(f"the verb {argv[0]} does not offer {absent or '--help'} (exit {done.returncode}): rebuild it",
             RC.EXIT_PIN)


def status_counts(path: Path) -> dict:
    """{(status, reason): count} from trials.csv, reading those two columns only (mechanics, no statistic)."""
    counts: dict = {}
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            key = (row["status"], row["reason"])
            counts[key] = counts.get(key, 0) + 1
    return counts


def mechanics(spec: dict, pinned: dict, out_dir: Path) -> tuple[list[str], list[str]]:
    """(lines, problems): the campaign's counts and constants checked against the registration; no statistic."""
    c = json.loads((out_dir / "campaign.json").read_text(encoding="utf-8"))
    t, reg, hur, recipe, search = c["trials"], c["registry"], c["hurdle"], c["recipe"], c["search"]
    w, problems = windows(spec), []

    def need(ok: bool, what: str) -> None:
        if not ok:
            problems.append(what)
    need(c.get("status") == "complete" and c.get("campaign_id") == spec["campaign_id"], "status / campaign id")
    need(c.get("budget") == spec["budget"] == hur.get("budget"), "budget differs from the registration")
    # The registry identity as the verb codes it (review MINE-16): n_raw = evaluated + screen-rejected +
    # racing-rejected + rung-failed + failed; a verb without the rung-failed count is not the registered verb.
    need(set(t) == {"distinct", *STATUS_KEYS}, f"trial statuses are not {', '.join(STATUS_KEYS)} (MINE-16)")
    statuses = {k: t.get(k, 0) for k in STATUS_KEYS}
    need(t["distinct"] == sum(statuses.values()), "distinct != the sum of the trial statuses")
    need(t["distinct"] <= spec["budget"] and search.get("capacity") == capacity(spec), "trials above the budget or "
         "capacity differs")
    stage2 = capacity(spec) > PER_FIELD * len(spec["fields"])
    if not stage2:
        need(t["distinct"] == capacity(spec) and search.get("stage2") is None, "stage-1-only campaign: distinct "
             "trials != templates, or a stage 2 ran")
    if spec["search"]["race_strides"] == "none":
        need(statuses["racing_rejected"] == 0 and statuses["rung_failed"] == 0,
             "racing off but trials were racing-rejected or rung-failed")
    need(reg["new_records"] == t["distinct"] and (spec["registry"]["head"] or reg["n_raw"] == reg["new_records"]),
         "registry records differ from the distinct trials")
    # Rule constants as lane MINE-STAT codes them: hurdle.t the Bonferroni z of the budget, hurdle.overlap_factor F of
    # the budget's band, both tables and the ceiling in the recipe, and every confirm read at Fc of the m reads that
    # reached the confirm (m is checked, never printed: it is a result, read in the runbook's step 11).
    overlap, confirm = factor_tables()
    need(abs(float(hur["t"]) - bonferroni_z(spec["budget"])) < 1e-8, "hurdle t is not mined_hurdle(budget)")
    need(hur.get("overlap_factor") == overlap_factor(spec["budget"]), "hurdle overlap_factor is not F of the "
         "budget's band")
    need(hur.get("max_budget") == recipe.get("max_budget") == max_budget(), "budget ceiling differs from the one in "
         "force")
    need(recipe.get("overlap_bands") == [list(b) for b in overlap] and
         recipe.get("confirm_bands") == [list(b) for b in confirm], "recipe factor tables differ from mined-v1's")
    reads = [p for p in c.get("promotions", []) if p.get("confirm_read")]
    fc = confirm_factor(len(reads))
    need(all(p.get("confirm_factor") == fc for p in reads) and
         all(p.get("confirm_factor") is None for p in c.get("promotions", []) if not p.get("confirm_read")),
         "a confirm read's factor is not Fc of the reads that reached the confirm")
    need(recipe["role_manifest_sha256"] == pinned["role"][1] and recipe["fields_manifest_sha256"] ==
         pinned["fields"][1] and recipe["pool_sha256"] == pinned["pool"][1], "recipe pins differ from the spec's")
    need((recipe["discover"]["begin"], recipe["discover"]["end"]) == w["discover"] and
         (recipe["confirm"]["begin"], recipe["confirm"]["end"]) == w["confirm"], "windows differ from the registration")
    need(c["inputs"]["fields"]["names"] == spec["fields"], "fields differ from the registration")
    lines = [f"   trials: distinct {t['distinct']} = " + " + ".join(f"{k} {v}" for k, v in statuses.items()) +
             f" (budget {c['budget']}, capacity {search.get('capacity')})",
             f"   registry: new {reg['new_records']}, n_raw {reg['n_raw']}, bytes {reg['bytes']}, head {reg['head']}",
             f"   rule constants: hurdle z {hur['t']}, overlap factor {hur.get('overlap_factor')}, ceiling "
             f"{hur.get('max_budget')}; confirm factors by m {recipe.get('confirm_bands')}; label rows discover "
             f"{recipe['discover']['label_rows']}, confirm {recipe['confirm']['label_rows']}",
             f"   footprint: required_bytes {search.get('required_bytes')}, seconds {c.get('seconds')}"]
    for (status, reason), n in sorted(status_counts(out_dir / "trials.csv").items()):
        lines.append(f"   status {status}{' / ' + reason if reason else ''}: {n}")
    return lines, problems


def run(spec: dict, spec_path: Path, root: Path, *, date: str | None = None, executor=RC.execute,
        clean=RC.git_scoped, log=print) -> int:
    why = run_refusal(spec, root)
    if why:
        fail("run refused: " + "; ".join(why), RC.EXIT_PIN)
    blocking, _ = clean(root)
    if blocking:
        fail(f"tree at {root} is not clean in the code pathspec: {', '.join(blocking[:8])}: commit first",
             RC.EXIT_STOP)
    res = RC.Resolver(root)
    pinned = pins(spec, res, "inputs")
    verb = verb_argv(spec, pinned)
    exe_offers(spec, root, verb, executor)
    for line in header(spec, spec_path, res):
        log(line)
    argv = runner_argv(spec, spec_path, verb)
    log(RC.fmt_argv(argv))
    done = executor(argv, root, env(spec), True)
    r = res.read_json(f"{receipt_dir(spec)}/receipt.json")
    if r is None:
        fail(f"HARD-STOP: no receipt in {receipt_dir(spec)} (runner exit {done.returncode}) "
             f"{(done.stderr or '')[-400:]}", RC.EXIT_STOP)
    log(f"   receipt: outcome {r.get('outcome')} exit {r.get('exit_code')} {r.get('wall_seconds', 0):.1f} s peak "
        f"{(r.get('sampled_peak_tree_rss_bytes') or 0) >> 20} MiB")
    if r.get("outcome") != "completed" or r.get("exit_code") != 0:
        fail(f"HARD-STOP: receipt {receipt_dir(spec)}: outcome {r.get('outcome')}, exit_code {r.get('exit_code')} "
             "(no campaign.json is read; see the runbook's void rules)", RC.EXIT_STOP)
    out_dir = res.path(spec["output"])
    try:   # Ruling E-33: ledgered before anything prints a statistic, whatever the campaign found
        rec = research_ledger.campaign_record(out_dir, date, root)
        written = research_ledger.append(res.path(spec["ledger"]), rec)
    except (OSError, ValueError, LookupError, TypeError) as exc:
        fail(f"HARD-STOP [ledger]: the campaign line was not appended: {exc}", RC.EXIT_STOP)
    log(f"== ledger: {'appended' if written else 'already present'} trial {rec['trial_id']} (count 0; registry "
        f"count {rec['registry']['count']}, total {rec['registry']['total']}; budget {rec['budget']})")
    lines, problems = mechanics(spec, pinned, out_dir)
    log("== mechanics (counts and constants only)")
    for line in lines:
        log(line)
    if problems:
        fail("HARD-STOP [mechanics]: " + "; ".join(problems) + " -- read no promotion before this is resolved",
             RC.EXIT_STOP)
    log("== campaign complete and ledgered; read the results in the runbook's order (promotions, then "
        "mined_members.json, then trials.csv numbers)")
    return RC.EXIT_OK


# ------------------------------------------------------------------ wave
def signed_dsl(member: dict) -> str:
    """The registry DSL of a mined member (prior_sign 1, the discover sign embedded, as the registry's -1 * forms)."""
    if member.get("sign") not in (1, -1):
        fail(f"wave: member {member.get('id')!r} has no discover sign +1 / -1")
    return member["dsl"] if member["sign"] == 1 else f"(-1 * ({member['dsl']}))"


def wave_lines(spec: dict, root: Path, parent: str, name: str, parent_spec: str) -> list[str]:
    """The add-alpha lines of the one mined wave (prereg item 11), from mined_members.json and the ledger line; printed,
    never executed (root runs them after the registry gains the theme `mined`)."""
    out = RC.Resolver(root).path(spec["output"])
    members = json.loads((out / "mined_members.json").read_text(encoding="utf-8"))
    line = json.loads((out / "ledger_line.json").read_text(encoding="utf-8"))
    if members.get("campaign_id") != spec["campaign_id"] or members.get("theme") != "mined":
        fail("wave: mined_members.json is not this campaign's")
    lines = []
    for m in members["members"]:
        lines.append(RC.fmt_argv([
            spec["python"], "scripts/research_cycle.py", "add-alpha", "--id", m["id"], "--dsl", signed_dsl(m),
            "--theme", "mined", "--tier", "C+", "--prior-sign", "1", "--origin", "mined",
            "--citation", f"mined-v1 campaign {spec['campaign_id']} (ledger trial {line['trial_id']})",
            "--prior-sign-source", f"mined-v1 discover sign, campaign {spec['campaign_id']}",
            "--form", "mined stage-1 template", "--parent", parent, "--name", name, "--parent-spec", parent_spec]))
    return lines or [f"# {spec['campaign_id']} admitted no member: no wave, no cell, no admission trial"]


# ------------------------------------------------------------------ CLI
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py mine", description=__doc__.split("\n", 1)[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("verb", choices=("lock", "pool", "probe", "plan", "run", "wave"))
    ap.add_argument("spec")
    ap.add_argument("--root", type=Path, default=research_tree.REPO)
    ap.add_argument("--write", action="store_true", help="lock: write the pins back into SPEC")
    ap.add_argument("--relock", action="store_true", help="lock: replace pins that differ from the files")
    ap.add_argument("--workers", type=int, action="append", default=None, help="probe: worker counts (default 4 2 1)")
    ap.add_argument("--date", default=None, help="run: YYYY-MM-DD of the campaign for its ledger line")
    ap.add_argument("--parent", help="wave: the parent library id (the last accepted cell's)")
    ap.add_argument("--name", help="wave: the wave's library name, e.g. <parent>m1")
    ap.add_argument("--parent-spec", help="wave: the last accepted cell's spec")
    a = ap.parse_args(argv)
    try:
        spec_path = RC.find_spec(a.spec)
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
            return RC.EXIT_OK
        spec = load(spec_path)
        if a.verb == "pool":
            assemble_pool(spec, a.root)
        elif a.verb == "probe":
            probe(spec, a.root, a.workers or [4, 2, 1])
        elif a.verb == "plan":
            print("\n".join(plan_lines(spec, spec_path, a.root)))
        elif a.verb == "wave":
            if not (a.parent and a.name and a.parent_spec):
                fail("wave needs --parent, --name and --parent-spec")
            print("\n".join(wave_lines(spec, a.root, a.parent, a.name, a.parent_spec)))
        else:
            if a.date is not None:
                dt.date.fromisoformat(a.date)
            return run(spec, spec_path, a.root, date=a.date)
        return RC.EXIT_OK
    except RC.CycleError as exc:
        print(f"research_cycle mine: {exc}", file=sys.stderr)
        return exc.code
    except ValueError as exc:
        print(f"research_cycle mine: {exc}", file=sys.stderr)
        return RC.EXIT_USAGE
