"""The context one research wave runs in (research_wave.py): root, manifest, the process executor and the git and
spec helpers every stage shares. Nothing here decides; the stages (wave_stages.py) do.

Processes go through ``executor(argv, root, env) -> CompletedProcess`` (default: subprocess.run with captured output,
cwd = the root), so a test drives every stage with a fake and ``--dry-run`` prints the same argv. Git queries
(clean checks, HEAD) read the repository directly; commits are commands and go through the executor.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_cycle as RC  # noqa: E402
import research_spec  # noqa: E402
import research_tree  # noqa: E402

ENGINE_TOOLS = str(research_tree.REPO / "atx-engine" / "tools")   # the generic stage chain lives with the engine tools
if ENGINE_TOOLS not in sys.path:
    sys.path.insert(0, ENGINE_TOOLS)
import stage_chain  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_steps as WS  # noqa: E402


def execute(argv: list[str], root: Path, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=root, env=env, capture_output=True, text=True)


class Wave:
    def __init__(self, manifest_path: Path, root: Path, *, executor=execute, log=print):
        self.root = Path(root).resolve()
        p = Path(manifest_path)
        self.manifest_file = (p if p.is_absolute() else self.root / p).resolve()
        self.manifest_rel = self.rel(self.manifest_file)
        self.manifest, self.manifest_sha = WM.load(self.manifest_file)
        self.executor, self.log = executor, log
        self.out_dir = self.manifest["out_dir"].rstrip("/")
        self._py: str | None = None

    # -------------------------------------------------------------- paths
    def rel(self, path: Path) -> str:
        p = Path(path).resolve()
        return p.relative_to(self.root).as_posix() if p.is_relative_to(self.root) else p.as_posix()

    def path(self, rel: str) -> Path:
        p = Path(rel)
        return p if p.is_absolute() else self.root / p

    def exists(self, rel: str) -> bool:
        return self.path(rel).exists()

    def sha(self, rel: str) -> str | None:
        p = self.path(rel)
        return stage_chain.sha256_file(p) if p.is_file() else None

    def read_json(self, rel: str):
        p = self.path(rel)
        return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None

    def write_json(self, rel: str, doc, *, exclusive: bool = False) -> str:
        p = self.path(rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        text = json.dumps(doc, indent=2, allow_nan=False) + "\n"
        with p.open("x" if exclusive else "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return stage_chain.sha256_file(p)

    def wave_path(self, *parts: str) -> str:
        return "/".join([self.out_dir, *parts])

    def free_run_dir(self, base: str) -> str:
        """<base>-run<k>, k the first number whose dir does not exist (the bounded runner never reuses a dir)."""
        k = 1
        while self.exists(f"{base}-run{k}"):
            k += 1
        return f"{base}-run{k}"

    # -------------------------------------------------------------- specs
    @property
    def python(self) -> str:
        """The interpreter of the parent spec (every cell of the chain runs under it), else this one."""
        if self._py is None:
            try:
                self._py = self.load_spec(self.manifest["parent"]["spec"]).get("python") or sys.executable
            except stage_chain.StageError:
                self._py = sys.executable
        return str(self._py)

    def load_spec(self, rel: str) -> dict:
        try:
            return RC.load_spec(self.path(rel))
        except RC.CycleError as exc:
            raise stage_chain.StageError(f"spec {rel}: {exc}", exc.code) from exc

    def spec_digest(self, rel: str) -> str:
        return research_spec.spec_digest(self.path(rel), research_tree.REPO)

    def outputs(self, rel: str) -> dict:
        """The resolved outputs of a cell spec file: nav, fit and cycle dirs, nav leverage, ledger, paired reference."""
        return self.outputs_of(self.load_spec(rel), rel, resolved=True)

    def outputs_of(self, spec: dict, rel: str, resolved: bool = False) -> dict:
        """``outputs`` of a spec document not written yet (a template is resolved as if it were the file ``rel``)."""
        if not resolved and research_spec.is_template(spec):
            try:
                spec = research_spec.resolve(spec, self.path(rel), RC.load_spec, research_tree.REPO)
            except (research_spec.TemplateError, RC.CycleError) as exc:
                raise stage_chain.StageError(f"spec {rel}: {exc}") from exc
        c = RC.Cycle(spec, RC.Resolver(self.root), verify=False)
        return {"spec": rel, "name": spec["name"], "nav": c.out(spec["nav"]["output"]),
                "fit": c.out(spec["fit"]["output"], keyed=False), "cycle_dir": c.cycle_dir(),
                "leverage": str(spec["nav"].get("leverage")), "ledger": (spec.get("summ") or {}).get("ledger"),
                "reference_nav": (spec["inputs"].get("reference_cell") or {}).get("dir")}

    # -------------------------------------------------------------- processes
    def run(self, argv: list[str], what: str, ok=(0,)) -> subprocess.CompletedProcess:
        self.log(WS.fmt_argv(argv))
        done = self.executor(argv, self.root, self.env())
        if done.returncode not in ok:
            tail = ((done.stderr or "") + (done.stdout or ""))[-600:]
            raise stage_chain.StageError(f"{what}: exit {done.returncode} ({WS.fmt_argv(argv)[:200]}) {tail}")
        return done

    def env(self) -> dict:
        return dict(os.environ)

    # -------------------------------------------------------------- git
    def git(self, *args: str) -> str:
        done = subprocess.run(["git", *args], cwd=self.root, capture_output=True, text=True)
        if done.returncode != 0:
            raise stage_chain.StageError(f"git {' '.join(args)}: exit {done.returncode}: {done.stderr.strip()[:300]}")
        return done.stdout

    def dirty_code(self) -> list[str]:
        """Dirty paths inside the code pathspec (research_tree.CODE_PATHSPEC): what the bounded runner refuses."""
        try:
            return research_tree.dirty_paths(self.root)[0]
        except (OSError, subprocess.CalledProcessError) as exc:
            raise stage_chain.StageError(f"git status failed: {exc}") from exc

    def committed(self, rel: str) -> str | None:
        """The last commit of a tracked, unmodified file; None when it is untracked or modified."""
        if self.git("status", "--porcelain", "--", rel).strip():
            return None
        sha = self.git("log", "-1", "--format=%H", "--", rel).strip()
        return sha or None

    def head(self) -> str:
        return self.git("rev-parse", "HEAD").strip()

    def commit_dirty(self, message: str) -> str | None:
        """Commit exactly the dirty code-pathspec paths a stage wrote (none: nothing to commit, None)."""
        paths = self.dirty_code()
        if not paths:
            return None
        for argv in WS.commit_argvs(paths, message):
            self.run(argv, "commit")
        if self.dirty_code():
            raise stage_chain.StageError(f"commit left dirty code paths: {', '.join(self.dirty_code()[:8])}")
        return self.head()
