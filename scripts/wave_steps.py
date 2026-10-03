"""The commands and spec files a research wave writes (research_wave.py): pure builders, no process runs here.

Commands (each an argv list, run from the root with no shell; given the wave's root, research_cycle.py and add-alpha
get ``--root ROOT`` and every tool path is root-relative when the tool exists under the root, else this repository's
absolute path):
  add_alpha_argv   research_cycle.py add-alpha for one frozen candidate (its registration fields verbatim, the wave's
                   parent, name, fields dir and --save-plan for the K1 plan of record)
  cycle_argv       research_cycle.py run SPEC [--screen | --stop-after nav] / lock SPEC
  bounded_argv     scripts/run_bounded_research.py with the wave readers' caps (180 s / 1,536 MiB / 512 MiB free, the
                   integrator's caps for the bundle and the mechanics reads) around one command
  reader_argv      wave_readers.py mechanics | book under the bounded runner
  bundle_argv      nav_summ.py --protocol v8 --bundle PARENT CELL --bundle-json OUT under the bounded runner (PM5-23)
  commit_argvs     git add -- PATHS ; git commit -q -m MSG -- PATHS (exactly the paths the stage wrote)
Spec files:
  gm_doc           the gross-matched copy of a cell spec (Ruling PM6-6): name + "-gm", nav.leverage L', nav.output with
                   the leverage token L<old> -> L<new> (else "-L<new>" appended) and, for a plain spec with a ref
                   phase, ref.leverage kept at the old L (the reference construction reproduces the parent); a
                   template copy gets the same keys in change.set
  rule_cell_doc    a cell template on the wave's parent: parent set (relative to the template's directory when both
                   lie in one), the registered constants merged into change.set / change.flags, the cell's name
                   (rule_cell.name, else the template's name + "-" + the wave id)
"""
from __future__ import annotations

import copy
import os
from pathlib import Path
import re

import research_spec
import research_tree

RCY = "scripts/research_cycle.py"
RUNNER = "scripts/run_bounded_research.py"
READERS = "scripts/wave_readers.py"
NAV_SUMM = "atx-impl/tools/nav_summ.py"
READER_CAPS = {"seconds": "180", "max_rss_mib": "1536", "min_free_mib": "512"}
OPTIONAL_FLAGS = ("prior_sign_source", "form", "formula", "domain", "deviation")


def tool(rel: str, root) -> str:
    """A tool's argv path: root-relative when ``root`` is None or the tool exists under it (the wave's own checkout),
    else the absolute path in this scripts' repository (research_tree.REPO)."""
    if root is None or (Path(root) / rel).is_file():
        return rel
    return (research_tree.REPO / rel).as_posix()


def rooted(argv: list[str], root) -> list[str]:
    """``--root ROOT`` appended (research_cycle.py and add-alpha write under it), none without a root."""
    return argv if root is None else argv + ["--root", Path(root).as_posix()]


def add_alpha_argv(py: str, c: dict, *, parent: str, parent_spec: str, name: str, fields_dir: str,
                   plan_out: str, root=None) -> list[str]:
    argv = [py, tool(RCY, root), "add-alpha", "--id", c["id"], "--dsl", c["dsl"], "--theme", c["theme"],
            "--tier", c["tier"], "--prior-sign", str(c["prior_sign"]), "--citation", c["citation"],
            "--origin", c["origin"], "--parent", parent, "--name", name, "--parent-spec", parent_spec, "--fields", fields_dir]
    for rid in c.get("replaces", []):
        argv += ["--replaces", rid]
    if c.get("rescreen"):
        argv.append("--rescreen")
    for rid in c.get("removes", []):
        argv += ["--removes", rid]
    ex = c.get("exception")
    if ex:
        for key, value in ex["limits"].items():
            argv += ["--exception", f"{key}={value}"]
        argv += ["--exception-basis", ex["basis"]]
    for key in OPTIONAL_FLAGS:
        if key in c:
            argv += [f"--{key.replace('_', '-')}", c[key]]
    return rooted(argv + ["--save-plan", plan_out], root)


def cycle_argv(py: str, verb: str, spec: str, *extra: str, root=None) -> list[str]:
    return rooted([py, tool(RCY, root), verb, spec, *extra], root)


def driver_flags(driver: dict | None) -> list[str]:
    """research_cycle run options of a manifest's driver block (wave_manifest.DRIVER_KEYS; none without one):
    auto_attempt -> --auto-attempt (P9 OR-4), admission_wait_seconds -> --admission-wait N (F-5 (a)),
    host_budget_mib -> --host-budget-mib N (OR section 5)."""
    d = driver or {}
    out = ["--auto-attempt"] if d.get("auto_attempt") else []
    for key, flag in (("admission_wait_seconds", "--admission-wait"), ("host_budget_mib", "--host-budget-mib")):
        if key in d:
            out += [flag, str(d[key])]
    return out


def runner_launch_flags(driver: dict | None) -> list[str]:
    """The bounded runner's launch admission options of a manifest's driver block (the readers' and the bundle's
    runs; none without the keys): --admission-wait-seconds N, --host-budget-mib N."""
    d, out = driver or {}, []
    for key, flag in (("admission_wait_seconds", "--admission-wait-seconds"), ("host_budget_mib", "--host-budget-mib")):
        if key in d:
            out += [flag, str(d[key])]
    return out


def bounded_argv(py: str, run_dir: str, binds: list[str], command: list[str], root=None,
                 driver: dict | None = None) -> list[str]:
    """``command`` under the bounded runner with the readers' caps (and the driver block's launch options)."""
    argv = [py, tool(RUNNER, root), "--seconds", READER_CAPS["seconds"], "--max-rss-mib", READER_CAPS["max_rss_mib"],
            "--min-free-mib", READER_CAPS["min_free_mib"], *runner_launch_flags(driver), "--output", run_dir]
    for b in binds:
        argv += ["--bind", b]
    return argv + ["--", *command]


def reader_argv(py: str, kind: str, navs: dict, out: str, run_dir: str, root=None,
                files: list | None = None, driver: dict | None = None) -> list[str]:
    """``files``: the NAVs' series the reader reads (daily_<scen>.csv, capacity_curve.csv), bound with summary.json."""
    readers = tool(READERS, root)
    cmd = [py, readers, kind]
    for name, d in navs.items():
        cmd += ["--nav", f"{name}={d}"]
    cmd += ["--output", out]
    return bounded_argv(py, run_dir, [readers] + [f"{d}/summary.json" for d in navs.values()] + list(files or []),
                        cmd, root, driver)


def bundle_argv(py: str, base_nav: str, cell_nav: str, out: str, run_dir: str, root=None,
                files: list | None = None, driver: dict | None = None) -> list[str]:
    """``files``: the two NAVs' daily series (daily_<scen>.csv), bound with summary.json (bundle_once checks them)."""
    summ = tool(NAV_SUMM, root)
    cmd = [py, summ, "--protocol", "v8", "--bundle", base_nav, cell_nav, "--bundle-json", out]
    return bounded_argv(py, run_dir, [summ, f"{base_nav}/summary.json", f"{cell_nav}/summary.json"] + list(files or []),
                        cmd, root, driver)


def commit_argvs(paths: list[str], message: str) -> list[list[str]]:
    return [["git", "add", "--", *paths], ["git", "commit", "-q", "-m", message, "--", *paths]]


def fmt_argv(argv: list[str]) -> str:
    """One command line as research_cycle prints it: an argument holding a space double-quoted."""
    return " ".join(f'"{a}"' if " " in a else a for a in argv)


# ------------------------------------------------------------------ spec files
def renamed_output(output: str, old: str, new: str) -> str:
    """nav.output with the leverage token L<old> replaced by L<new> (the -loc-L1.1474 convention), else -L<new>."""
    token = re.compile(rf"(?<![0-9.])L{re.escape(old)}(?![0-9])")
    return token.sub(f"L{new}", output) if token.search(output) else f"{output}-L{new}"


def gm_doc(doc: dict, resolved: dict, new: str, note: str) -> dict:
    """The gross-matched copy of a cell spec file ``doc`` (``resolved``: its resolved spec, for a template)."""
    out = copy.deepcopy(doc)
    old = str(resolved["nav"]["leverage"])
    out["name"] = f"{doc['name']}-gm"
    out["description"] = (doc.get("description", "") + " " + note).strip()
    nav_out = renamed_output(resolved["nav"]["output"], old, new)
    if research_spec.is_template(doc):
        sets = out.setdefault("change", {}).setdefault("set", {})
        sets["nav.output"], sets["nav.leverage"] = nav_out, new
        return out
    out["nav"] = dict(out["nav"], leverage=new, output=nav_out)
    if "ref" in out and "leverage" not in out["ref"]:
        out["ref"] = dict(out["ref"], leverage=old)
    return out


def without_marginal(doc: dict) -> dict:
    """A plain cell spec without its marginal phase (and that phase's runner cap): the wave's b library carries the
    screen's per-row marginal fields instead of a second pass of the same mode (speed.reuse_screen_marginal; the
    marginal decides nothing)."""
    out = copy.deepcopy(doc)
    out.pop("marginal", None)
    phases = (out.get("runner") or {}).get("phases")
    if isinstance(phases, dict) and "marginal" in phases:
        phases.pop("marginal")
        if not phases:
            out["runner"].pop("phases")
    return out


def marginal_mode(doc) -> str | None:
    """A plain cell spec's marginal mode: "themes" (the pool and the parent's theme weights), "pool-only" (PM6-8 (i)),
    or None (no marginal phase)."""
    m = doc.get("marginal") if isinstance(doc, dict) else None
    return None if not isinstance(m, dict) else "themes" if "themes" in m else "pool-only"


def pool_only_marginal(doc: dict) -> dict:
    """Ruling PM6-8 (i), confirmed for replacing waves by PM7-32: a library that replaces a parent member runs the
    marginal on the pool only (the parent's theme weights hold the replaced member, which the verb refuses):
    marginal.themes deleted and marginal.output suffixed -poolonly (the integrator's X-2 / X-4 spec fix)."""
    out = copy.deepcopy(doc)
    m = out.get("marginal")
    if isinstance(m, dict) and "themes" in m:
        m.pop("themes")
        if not m["output"].endswith("-poolonly"):
            m["output"] = f"{m['output']}-poolonly"
    return out


def marginal_ruled(doc: dict, rule: dict) -> dict:
    """The manifest's "marginal" ruling (e.g. PM8-15: an add-alpha wave on a theme-erc parent with more themes than
    the marginal verb takes runs it on the pool only, PM6-8 (i); the phase's time cap): pool_only applies
    pool_only_marginal, seconds sets runner.phases.marginal.seconds; a spec without a marginal phase is unchanged. A
    cap above the bounded runner's maximum (research_tree.RUNNER_MAX_SECONDS) is refused (ValueError, P9 OR-1)."""
    refusal = research_tree.seconds_cap_refusal("marginal.seconds", rule.get("seconds"))
    if refusal:
        raise ValueError(f"wave manifest {refusal}")
    out = pool_only_marginal(doc) if rule.get("pool_only") else copy.deepcopy(doc)
    if "seconds" in rule and isinstance(out.get("marginal"), dict):
        phases = out.setdefault("runner", {}).setdefault("phases", {})
        phases["marginal"] = dict(phases.get("marginal") or {}, seconds=rule["seconds"])
    return out


def unpinned(doc: dict) -> dict:
    """A cell template without the pins `lock --write` writes into it (research_spec.lock_template: the "locked"
    block, change.inputs.*.sha256, the fields manifest pin in change.set; `lock --exes`: change.set.exes_sha256): a
    resumed rule cell compares on this."""
    out = copy.deepcopy(doc)
    out.pop("locked", None)
    change: dict = out["change"] if isinstance(out.get("change"), dict) else {}
    for item in (change.get("inputs") or {}).values():
        if isinstance(item, dict) and "sha256" in item:
            item["sha256"] = None
    sets = change.get("set") or {}
    sets.pop("exes_sha256", None)               # `lock --exes --write` (P9 OR-2, driver.lock_exes)
    if "fields.manifest_sha256" in sets:
        sets["fields.manifest_sha256"] = None
    if isinstance(sets.get("fields"), dict) and "manifest_sha256" in sets["fields"]:
        sets["fields"]["manifest_sha256"] = None
    return out


def gm_path(spec_path: str) -> str:
    p = Path(spec_path)
    return (p.parent / f"{p.stem}-gm{p.suffix}").as_posix()


def rule_cell_doc(template: dict, template_path: str, parent_spec: str, rule_cell: dict, wave: str) -> dict:
    """The rule cell's template file: the template with parent set, constants merged and its name (rule_cell.name,
    else the template's name + "-" + the wave id: the cell's cycle dir is its own)."""
    out = copy.deepcopy(template)
    tdir, pspec = Path(template_path).parent, Path(parent_spec)
    out["parent"] = pspec.name if pspec.parent == tdir else os.path.relpath(pspec, tdir).replace("\\", "/")
    out["name"] = rule_cell.get("name") or f"{template['name']}-{wave}"
    const = rule_cell.get("constants") or {}
    change = out.setdefault("change", {})
    if const.get("set"):
        change["set"] = dict(change.get("set") or {}, **const["set"])
    for section, ops in (const.get("flags") or {}).items():
        flags = change.setdefault("flags", {})
        flags[section] = dict(flags.get(section) or {}, **ops)
    return out


def rule_cell_path(template_path: str, rule_cell: dict, wave: str) -> str:
    p = Path(template_path)
    return (p.parent / f"{rule_cell.get('name') or f'{p.stem}-{wave}'}.json").as_posix()
