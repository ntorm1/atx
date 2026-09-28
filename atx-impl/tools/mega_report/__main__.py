"""CLI: ``python atx-impl/tools/mega_report --config CONFIG.json --out REPORT.html`` (or ``python -m mega_report``
with ``atx-impl/tools`` on PYTHONPATH). ``--root`` overrides the config's inputs root; ``--stamp`` fixes the
generated-at text (reproducible output); ``--self-test`` runs the component self-tests only."""
import argparse
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = 'mega_report'

from mega_report import components, report  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog='mega_report', description=__doc__)
    ap.add_argument('--config')
    ap.add_argument('--out')
    ap.add_argument('--root')
    ap.add_argument('--stamp')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        print(components.self_test())
        return 0
    if not a.config or not a.out:
        ap.error('--config and --out are required')
    html = report.build(a.config, root=a.root, stamp=a.stamp)
    out = Path(a.out)
    out.write_text(html, encoding='utf-8', newline='\n')
    print(f'{out} {out.stat().st_size} bytes; n/a markers {html.count(chr(62) + components.NA_TEXT + chr(60))}; '
          f'unavailable blocks {html.count("class=" + chr(34) + "unavailable")}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
