"""research_cycle.py wave: one research wave, one command (platform v8 lane YINFRA).

  research_cycle.py wave plan   MANIFEST [--root R]                 the exact argv of every stage; nothing runs
  research_cycle.py wave run    MANIFEST [--root R] [--until STAGE] [--dry-run] [--seal-allow TOKEN=RULING ...]
  research_cycle.py wave status MANIFEST [--root R]

MANIFEST is a committed atx.research-wave/v1 file (wave_manifest.py), e.g. scripts/specs/v8/waves/<wave>.json. The
stages (wave_stages.py) run as a resumable chain (atx-engine/tools/stage_chain.py) whose receipts live in
<manifest out_dir>/receipts/: preflight -> register -> screen -> spec -> run -> match -> verify -> judge -> record.
Each stage runs only after the previous stage's ok receipt, and only while every input it recorded is unchanged; a
re-run resumes after the last good receipt; a failed stage leaves a .failed-<k> receipt and is retried by the next run.
``--dry-run`` (= ``plan``) prints every stage's argv without running anything. The stages that read data (screen,
run, match, verify, judge) are for root only; the tests drive them with fakes.

Exit codes: 0 done (or stopped after --until), 2 usage / manifest, 3 a pin or a stale receipt, 4 a hard stop of a stage.
Nothing here changes an existing command: research_cycle.py gains the verb ``wave``, add-alpha the opt-in
``--save-plan``.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_tree  # noqa: E402
from wave_context import Wave, execute, stage_chain  # noqa: E402
import wave_manifest as WM  # noqa: E402
import wave_stages  # noqa: E402

CHAIN = "atx.research-wave"


def chain_of(w: Wave) -> stage_chain.Chain:
    """The wave's stage chain; driver.receipt_digest "content" chains the receipts' content digests (P9 OR section 3:
    no started_utc / seconds in a chain hash), absent the file digests of before."""
    return stage_chain.Chain(CHAIN, wave_stages.STAGES, w.path(w.out_dir),
                             digest=WM.driver(w.manifest, "receipt_digest", "file"))


def main(argv=None, *, executor=execute, log=print) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py wave", description=__doc__.split("\n", 1)[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("verb", choices=("plan", "run", "status"))
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--root", type=Path, default=research_tree.REPO)
    ap.add_argument("--until", default=None, choices=[s.name for s in wave_stages.STAGES],
                    help="run: stop after this stage")
    ap.add_argument("--dry-run", action="store_true", help="run: print every stage's argv, run nothing (= plan)")
    ap.add_argument("--seal-allow", action="append", default=[], metavar="TOKEN=RULING",
                    help="run: a seal-scan token that is not a data date, allowed by the named ruling (recorded in the "
                         "verify receipt and the result; repeatable)")
    a = ap.parse_args(argv)
    allow = {}
    for item in a.seal_allow:
        token, _, ruling = item.partition("=")
        if not token.strip() or not ruling.strip():
            print(f"research_cycle wave: --seal-allow {item!r}: TOKEN=RULING, both non-empty", file=sys.stderr)
            return 2
        allow[token.strip()] = ruling.strip()
    try:
        w = Wave(a.manifest, a.root, executor=executor, log=log)
        w.seal_allow = allow
        chain = chain_of(w)
        if a.verb == "plan" or a.dry_run:
            log(f"# wave {w.manifest['wave']}: manifest {w.manifest_rel} sha256 {w.manifest_sha}; state "
                f"{w.out_dir}/receipts; root {w.root}")
            for line in chain.plan(w):
                log(line)
            return 0
        if a.verb == "status":
            for row in chain.state(w):
                log(f"{row['stage']:10s} {row['state']:8s} {row['receipt'] or ''} {row['why']}".rstrip())
            return 0
        chain.run(w, until=a.until, log=log)
        return 0
    except WM.WaveError as exc:
        print(f"research_cycle wave: {exc}", file=sys.stderr)
        return 2
    except (stage_chain.ChainError, stage_chain.StageError) as exc:
        print(f"research_cycle wave: {exc}", file=sys.stderr)
        return exc.code


if __name__ == "__main__":
    sys.modules.setdefault("research_wave", sys.modules[__name__])
    sys.exit(main())
