"""Run the tiny-world fake wave (test_wave_driver.queued_root + FakeCycle) at a fixed root; print every file's digest
(JSON files: sha256 of canonical time_free JSON; others: sha256 of bytes), excluding .git. Usage: fakewave.py REPO ROOT"""
import hashlib, json, os, shutil, sys
from pathlib import Path
repo, root = Path(sys.argv[1]), Path(sys.argv[2])
os.environ["GIT_AUTHOR_DATE"] = os.environ["GIT_COMMITTER_DATE"] = "2026-10-01T00:00:00+00:00"
sys.path.insert(0, str(repo / "scripts" / "tests")); sys.path.insert(0, str(repo / "scripts"))
import test_wave_driver as T
import wave_fixture as F
import research_wave
from wave_context import stage_chain as SC
if root.exists():
    shutil.rmtree(root, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
r = T.queued_root(root, driver={"receipt_digest": "content", "record_date": "2026-10-01"})
code = research_wave.main(["run", F.MANIFEST, "--root", str(r)], executor=F.FakeCycle(r, T.ADMIT_ALL), log=lambda s: None)
print("exit", code)
for p in sorted(r.rglob("*")):
    rel = p.relative_to(r).as_posix()
    if rel.startswith(".git/") or rel == ".git" or not p.is_file():
        continue
    data = p.read_bytes()
    if p.suffix == ".json":
        try:
            doc = SC.time_free(json.loads(data))
            data = SC.canonical(doc).encode()
            kind = "json"
        except ValueError:
            kind = "bytes"
    else:
        kind = "bytes"
    print(rel, kind, hashlib.sha256(data).hexdigest())
