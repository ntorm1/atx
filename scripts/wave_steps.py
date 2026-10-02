"""The commands and spec files a research wave writes (research_wave.py): pure builders, no process runs here.

Commands (each an argv list, run from the root with no shell):
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

RCY = "scripts/research_cycle.py"
RUNNER = "scripts/run_bounded_research.py"
READERS = "scripts/wave_readers.py"
NAV_SUMM = "atx-impl/tools/nav_summ.py"
READER_CAPS = {"seconds": "180", "max_rss_mib": "1536", "min_free_mib": "512"}
OPTIONAL_FLAGS = ("prior_sign_source", "form", "formula", "domain", "deviation")


def add_alpha_argv(py: str, c: dict, *, parent: str, parent_spec: str, name: str, fields_dir: str,
                   plan_out: str) -> list[str]:
    argv = [py, RCY, "add-alpha", "--id", c["id"], "--dsl", c["dsl"], "--theme", c["theme"], "--tier", c["tier"],
            "--prior-sign", str(c["prior_sign"]), "--citation", c["citation"], "--origin", c["origin"],
            "--parent", parent, "--name", name, "--parent-spec", parent_spec, "--fields", fields_dir]
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
    return argv + ["--save-plan", plan_out]


def cycle_argv(py: str, verb: str, spec: str, *extra: str) -> list[str]:
    return [py, RCY, verb, spec, *extra]


def bounded_argv(py: str, run_dir: str, binds: list[str], command: list[str]) -> list[str]:
    argv = [py, RUNNER, "--seconds", READER_CAPS["seconds"], "--max-rss-mib", READER_CAPS["max_rss_mib"],
            "--min-free-mib", READER_CAPS["min_free_mib"], "--output", run_dir]
    for b in binds:
        argv += ["--bind", b]
    return argv + ["--", *command]


def reader_argv(py: str, kind: str, navs: dict, out: str, run_dir: str) -> list[str]:
    cmd = [py, READERS, kind]
    for name, d in navs.items():
        cmd += ["--nav", f"{name}={d}"]
    cmd += ["--output", out]
    return bounded_argv(py, run_dir, [READERS] + [f"{d}/summary.json" for d in navs.values()], cmd)


def bundle_argv(py: str, base_nav: str, cell_nav: str, out: str, run_dir: str) -> list[str]:
    cmd = [py, NAV_SUMM, "--protocol", "v8", "--bundle", base_nav, cell_nav, "--bundle-json", out]
    return bounded_argv(py, run_dir, [NAV_SUMM, f"{base_nav}/summary.json", f"{cell_nav}/summary.json"], cmd)


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
    screen's marginal rows instead of a second pass (speed.reuse_screen_marginal; the marginal decides nothing)."""
    out = copy.deepcopy(doc)
    out.pop("marginal", None)
    phases = (out.get("runner") or {}).get("phases")
    if isinstance(phases, dict) and "marginal" in phases:
        phases.pop("marginal")
        if not phases:
            out["runner"].pop("phases")
    return out


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
