"""GATE-0 (RESUME 11) STORE-S S-1 teeth (TEETH-RESTORE, TEETH-CRLF form): the hook's catch-all re-raises.

Precondition: `git hash-object scripts/research_store_hook.py` equals `git rev-parse HEAD:scripts/research_store_hook.py`.
Keeps the working bytes in memory and as a copy beside this tool's log dir, writes the mutant (record's
`except Exception` body `warn(...)` replaced by `raise`), runs the hook test file once (pytest, own basetemp), restores
the bytes in `finally`, then asserts the raw SHA-256 equals the kept copy's and hash-object equals HEAD's blob again.
usage: teeth_hook_catchall.py REPO BASETEMP LOG
"""
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path

REL = "scripts/research_store_hook.py"
OLD = ('    except Exception as exc:                 # noqa: BLE001  (the JSON stands whatever the hook meets)\n'
       '        warn(cls, rel, f"{type(exc).__name__}: {exc}")\n')
NEW = ('    except Exception as exc:                 # noqa: BLE001  (the JSON stands whatever the hook meets)\n'
       '        raise\n')


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


def main() -> int:
    repo, bt, log = Path(sys.argv[1]).resolve(), Path(sys.argv[2]), Path(sys.argv[3])
    path = repo / REL
    head_blob = git(repo, "rev-parse", f"HEAD:{REL}")
    if git(repo, "hash-object", REL) != head_blob:
        print("precondition: the working file differs from HEAD; nothing mutated")
        return 2
    kept = path.read_bytes()
    kept_sha = hashlib.sha256(kept).hexdigest()
    copy = log.parent / "teeth-hook-catchall.kept.bin"
    copy.write_bytes(kept)
    text = kept.decode("utf-8")
    nl = "\r\n" if "\r\n" in text else "\n"
    old, new = OLD.replace("\n", nl), NEW.replace("\n", nl)
    if text.count(old) != 1:
        print("the catch-all text was not found exactly once; nothing mutated")
        return 2
    rc = None
    try:
        path.write_bytes(text.replace(old, new).encode("utf-8"))
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
        with log.open("w", encoding="utf-8") as out:
            rc = subprocess.run([sys.executable, "-m", "pytest", "-q", "-rf", "-p", "no:cacheprovider", "--basetemp",
                                 str(bt), "scripts/tests/test_research_store_hook.py"], cwd=repo, env=env,
                                stdout=out, stderr=subprocess.STDOUT, timeout=600).returncode
    finally:
        path.write_bytes(kept)
    raw_ok = hashlib.sha256(path.read_bytes()).hexdigest() == kept_sha
    blob_ok = git(repo, "hash-object", REL) == head_blob
    copy.unlink()
    print(f"mutant pytest exit {rc}")
    print(f"restore raw sha256 {kept_sha[:16]} equal {raw_ok}; hash-object = HEAD blob {head_blob[:16]} {blob_ok}")
    return 0 if (raw_ok and blob_ok) else 3


if __name__ == "__main__":
    sys.exit(main())
