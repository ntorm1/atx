# atx-vol Python scripts

This directory is a standalone Python-utility tier that operates on the
outputs of the C++ atx-vol build (e.g. `bev_label_factory`'s label TSVs) --
it is **not** part of the CMake build, is not linked against `atxvol`, and
has no C++ compile-time relationship to the rest of `atx-vol/`. Scripts here
are stdlib-only (`argparse`, `json`, `subprocess`, `pathlib`, `csv`, `math`,
...); the only allowed test dependency is `pytest`. Tests are self-contained
(tmp-dir fixtures, no network, no real corpus/driver dependency -- driver
invocations are exercised against small Python stub scripts, never the real
binary). Run the suite from the repo root:

```
python -m pytest atx-vol/scripts/ -q
```

## Scripts

- `bev_corpus_run.py` -- manifest-driven fan-out of `bev_label_factory`
  across a (run x tenor) grid, sequential (the driver is already internally
  threaded via its own `--threads`). See the module docstring for the CLI
  and manifest JSON schema.
- `bev_label_qa.py` -- markdown QA report over one or more `bev_label_factory`
  label TSVs: row accounting by flag/snapped, `log_ratio` distribution by
  tenor x delta bucket, feature-column NaN coverage, cross-file
  duplicate-key detection, and a report-only leakage tripwire. See the
  module docstring for the CLI and exit codes.
