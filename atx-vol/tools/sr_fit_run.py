"""Run the whole SpiderRock fit-and-score pipeline for one binary, over N buckets.

The comparison this repo cares about spans four artefacts and two stores, and
doing it by hand invites the one mistake that silently invalidates everything:
scoring a chain built by binary A with a label claiming binary B, or against a
bucket other than the one it was fitted from. So the binary path, the bucket, and
the label travel together through a single call.

    transcode(bucket) -> hive + underlier
      -> <binary> chain-export --snapshot-suffix <bucket instant>
      -> sr_fit_scorecard --bucket <same bucket>

THE SNAPSHOT STAMP IS A CLAIM, NOT A FILTER. `--snapshot-suffix` is checked
against the transcoded file's own `ts` (opra_panel.cpp:804) and a disagreement is
a hard InvalidArgument, never a silent re-valuation at the wrong minute. This
derives the stamp from the bucket rather than accepting it as an argument, so the
two cannot drift apart.

Transcodes are cached: a bucket's hive is rebuilt only when absent, because the
transcode is a pure function of the store partition and re-running it per binary
would be wasted minutes and an opportunity for the two arms to differ in their
INPUT rather than in the thing under test.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import pathlib
import subprocess
import sys
from zoneinfo import ZoneInfo

TOOLS = pathlib.Path(__file__).resolve().parent
ET = ZoneInfo("America/New_York")


def snapshot_suffix(date: str, bucket: str) -> str:
    """The bucket instant in UTC, in the exact shape chain-export demands."""
    et = dt.datetime.strptime(f"{date} {bucket}", "%Y-%m-%d %H%M").replace(tzinfo=ET)
    return et.astimezone(dt.UTC).strftime("T%H:%M:%SZ")


def run(cmd: list[str], what: str) -> None:
    print(f"  $ {what}", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-4000:] + "\n" + r.stderr[-4000:] + "\n")
        raise SystemExit(f"FAILED ({r.returncode}): {what}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="fit + score one binary over N buckets")
    ap.add_argument("--binary", required=True, type=pathlib.Path,
                    help="atx-vol-chain-export.exe to test")
    ap.add_argument("--label", required=True, help="carried into every receipt")
    ap.add_argument("--tag", required=True, help="short slug for output filenames")
    ap.add_argument("--date", default="2026-08-14")
    ap.add_argument("--buckets", default="1030", help="comma list of HHMM")
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--store", type=pathlib.Path,
                    default=pathlib.Path("C:/atx-cache/oracle/spiderrock"))
    ap.add_argument("--db", default="C:/atx-data/surface-db/prodv1")
    ap.add_argument("--rate", default="0.0432432")
    ap.add_argument("--work", type=pathlib.Path, default=pathlib.Path("C:/atx-cache/srfit"))
    ap.add_argument("--extra", default="",
                    help="extra chain-export args, space separated (e.g. "
                         "'--time-convention voltime')")
    a = ap.parse_args(argv)

    a.work.mkdir(parents=True, exist_ok=True)

    # THE HIVE CACHE KEY MUST INCLUDE THE SYMBOL SET. A hive is a function of
    # (date, bucket, symbols); keying it on (date, bucket) alone means a later
    # run over a WIDER cohort silently reuses a NARROWER hive, and the score
    # join then drops every symbol the first run did not transcode -- without a
    # word, so the receipt carries a real number for the wrong population. This
    # cost one 140-name breadth run that came back scoring 22.
    sym_key = hashlib.sha1(
        ",".join(sorted(s.strip().upper()
                        for s in a.symbols.split(",") if s.strip())).encode()
    ).hexdigest()[:8]

    receipts = []
    for bucket in [b.strip() for b in a.buckets.split(",") if b.strip()]:
        print(f"\n=== {a.tag} / bucket {bucket} ===", flush=True)
        hive = a.work / f"hive-{a.date}-{bucket}-{sym_key}"
        und = a.work / f"und-{a.date}-{bucket}-{sym_key}"
        if not (hive / f"date={a.date}" / "data.parquet").exists():
            run([sys.executable, str(TOOLS / "spiderrock_to_opra_hive.py"),
                 "--store", str(a.store), "--date", a.date, "--bucket-et", bucket,
                 "--symbols", a.symbols,
                 "--out-hive", str(hive), "--out-underlier", str(und)],
                f"transcode {bucket}")
        else:
            print(f"  (hive cached: {hive})")

        chain = a.work / f"chain-{a.tag}-{a.date}-{bucket}.parquet"
        cmd = [str(a.binary), "--hive", str(hive), "--underlier", str(und),
               "--db", a.db, "--date", a.date, "--symbols", a.symbols,
               "--snapshot-suffix", snapshot_suffix(a.date, bucket),
               "--r", a.rate, "--out", str(chain)]
        if a.extra:
            cmd += a.extra.split()
        run(cmd, f"chain-export {bucket} {a.extra}")

        receipt = a.work / f"score-{a.tag}-{a.date}-{bucket}.json"
        run([sys.executable, str(TOOLS / "sr_fit_scorecard.py"),
             "--chain", str(chain), "--store", str(a.store),
             "--date", a.date, "--bucket", bucket,
             "--label", f"{a.label} [{bucket}]", "--out", str(receipt)],
            f"score {bucket}")
        receipts.append(receipt)

    print("\nreceipts:")
    for r in receipts:
        print(f"  {r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
