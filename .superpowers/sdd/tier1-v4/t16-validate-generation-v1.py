"""Guarded explicit-generation validation; never relies on the current pointer."""
import argparse
import json
import re
import uuid
from pathlib import Path

from atx_db.alpha_panel import common as C, release_inputs as R, universe_release as U


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generation', required=True)
    parser.add_argument('--manifest-sha256', required=True)
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.generation):
        parser.error('generation must be a simple explicit identifier')
    if not re.fullmatch(r'[0-9a-f]{64}', args.manifest_sha256):
        parser.error('manifest-sha256 must be the independently pinned lowercase SHA256')
    root = C.build_root().resolve()
    parent = root / U.STAGE / 'generations'
    generation = (parent / args.generation).resolve()
    if generation.parent != parent.resolve():
        raise RuntimeError('Generation escaped explicit release root')
    manifest_path = generation / 'manifest.json'
    before = R.identity(manifest_path)
    if before['sha256'] != args.manifest_sha256:
        raise RuntimeError('Manifest differs from caller-pinned generation')
    con = C.connect(memory='380MB', threads=1, db_file=f'explicit-release-check-{uuid.uuid4().hex}.duckdb')
    try:
        U.validate_generation(root, generation, con)
    finally:
        con.close()
    manifest = C.read_json(manifest_path)
    if R.identity(manifest_path) != before:
        raise RuntimeError('Manifest changed during full validation')
    print(json.dumps({'status': 'GENERATION_VALIDATED_NOT_PRODUCTION_READY',
                      'generation': str(generation), 'manifest': before,
                      'gate_status': manifest['gate_status'],
                      'readiness': manifest['readiness'],
                      'source_quality_certified': manifest['source_quality_certified'],
                      'helper': R.identity(Path(__file__))}, sort_keys=True), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
