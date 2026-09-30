"""Render checks of the v8 pitch config (docs/plans/mega-alpha-v8-pitch.config.json) on synthetic inputs.

The config is read as committed; only its root moves to a short temp directory (Windows 260-character path limit), where
``test_mega_report_v8.world`` writes a synthetic file at every input path the config names. No real data is read: the
tiny_world fixture yields no NAV or nav_summ output without the built executables, so every input is synthetic JSON
(the tracked pre-registration and literature documents are read in one test, as the pitch reads them).

- every input present: 0 unavailable blocks, every placeholder resolved, every input listed as read;
- each input missing: exactly one unavailable block, its owner's, naming the path;
- the planning value, the seal check on a sealed path, and the v7 config untouched.

Run: python -m pytest atx-impl/tools/test_mega_report_v8_render.py -q
"""
import copy
import hashlib
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_mega_report_v8 as T  # noqa: E402
from mega_report import data as D  # noqa: E402
from mega_report import pitch as P  # noqa: E402
from mega_report import v8 as V  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
V8_CONFIG = REPO / 'docs/plans/mega-alpha-v8-pitch.config.json'
V7_CONFIG = REPO / 'docs/plans/mega-alpha-v7-pitch.config.json'
V7_CONFIG_SHA256 = '73f80583b14d93a602f0934a315e3d9c9cb808c36e65022fb03e8d017e975a3b'  # LF bytes at 39926caa
PLANNING = 'live net Sharpe .7 to 1.0 [est] against the TRAIN figure'


def v8_config() -> dict:
    cfg = json.loads(V8_CONFIG.read_text(encoding='utf-8'))
    cfg['root'] = '.'  # the temp root the config is written into
    return cfg


INPUTS = V.inputs(v8_config())


@pytest.fixture()
def root():
    d = Path(tempfile.mkdtemp(prefix='v8p'))
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)


def text_of(html: str) -> str:
    return re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', ' ', html))


# ============================================================================================ the config itself
def test_v8_config_uses_registered_blocks_and_names_every_input():
    cfg = v8_config()
    types = [b if isinstance(b, str) else b['type'] for sec in cfg['layout'] for b in sec['blocks']]
    assert all(t in P.BLOCKS for t in types)
    assert set(V.BLOCKS) <= set(types)  # every v8 section is in the pitch
    assert [k for _, k, _ in INPUTS] == [
        'v8.summ', *[f'v8.cells[{k}].paired' for k in ('B0b', 'R-1', 'R-2', 'R-3', 'R-4', 'R-5', 'R-6', 'R-7')],
        'v8.bundle', 'v8.diagnostics', 'v8.member_horizon.card_index', 'v8.member_horizon.admission',
        'v8.trial_ledger', 'v8.prereg', 'v8.literature']
    assert len({p for _, _, p in INPUTS}) == len(INPUTS)
    assert not any(D.path_is_sealed(p) for _, _, p in INPUTS)  # no input is refused by name
    assert cfg['v8']['re_screens'] == 8 and cfg['v8']['final'] in {c['key'] for c in cfg['v8']['cells']}


def test_v7_config_is_untouched_and_has_no_v8_block():
    raw = V7_CONFIG.read_bytes().replace(b'\r\n', b'\n')
    assert hashlib.sha256(raw).hexdigest() == V7_CONFIG_SHA256
    cfg = json.loads(raw)
    assert 'v8' not in cfg
    assert not [b for sec in cfg['layout'] for b in sec['blocks']
                if (b if isinstance(b, str) else b['type']).startswith('v8_')]


# ============================================================================================ render
def test_v8_pitch_renders_every_block_when_every_input_exists(root):
    cfg = v8_config()
    T.world(root, cfg)
    html = T.build(root, cfg)
    assert T.unavailable(html) == []
    assert 'unresolved' not in html and '>n/a<' not in html.split('id="summary"')[1].split('id="window"')[0]
    for tid in ('t-v8-bundle', 't-v8-bundle-years', 't-v8-ladder', 't-v8-year-matrix', 't-v8-year-R-7',
                't-v8-diagnostics', 't-v8-g1a-members', 't-v8-member-horizon', 't-v8-contradictions',
                't-appendix-files'):
        assert f'id="{tid}"' in html, tid
    for _, _, rel in INPUTS:  # each input read (seal check passed) and listed with its SHA-256
        sha = hashlib.sha256((root / rel).read_bytes()).hexdigest()
        assert f'data-sort="{rel}">{rel}</td><td class="t" data-sort="read">read</td>' in html, rel
        assert sha in html, rel
    assert html.count('<tr', html.index('id="t-v8-ladder"'), html.index('</table>', html.index('id="t-v8-ladder"'))) == 11


def test_v8_pitch_states_the_planning_value_and_the_re_screens(root):
    cfg = v8_config()
    T.world(root, cfg)
    txt = text_of(T.build(root, cfg))
    assert f'Plan on a {PLANNING} +1.400 (S2, 2020-2023)' in txt
    assert 'admission trials this sprint 7 plus 8 re-screens' in txt
    assert 'Freeze gate met: yes' in txt and 'OD-1 window disclosure (v8-prereg item 1)' in txt


@pytest.mark.parametrize('block,key,rel', INPUTS, ids=[k for _, k, _ in INPUTS])
def test_each_missing_input_is_one_named_unavailable_block(root, block, key, rel):
    cfg = v8_config()
    T.world(root, cfg, skip=(rel,))
    na = T.unavailable(T.build(root, cfg))
    assert [n for n, _ in na] == [block]
    assert f'{rel}: ' in na[0][1] and 'missing' in na[0][1]


def test_a_sealed_input_path_is_refused_before_it_is_opened(root):
    cfg = v8_config()
    T.world(root, cfg)
    sealed = 'build-equity/mega-nav-v8-summ-2024.json'
    shutil.copy(root / cfg['v8']['summ'], root / sealed)
    cfg['v8']['summ'] = sealed
    html = T.build(root, cfg)
    na = T.unavailable(html)
    assert [n for n, _ in na] == ['v8_year_table'] and 'refused (sealed)' in na[0][1]
    assert f'data-sort="{sealed}">{sealed}</td><td class="t" data-sort="refused (sealed)">' in html


def test_v8_pitch_on_the_tracked_documents(root):
    """The pre-registration and the literature review as committed: item 1 and the contradictions table parse."""
    cfg = v8_config()
    T.world(root, cfg)
    for key in ('prereg', 'literature'):
        rel = V._rel(cfg['v8'][key])
        src = REPO / rel
        if not src.exists():
            pytest.skip(f'{rel} not in this checkout')
        shutil.copy(src, root / rel)
    txt = text_of(T.build(root, copy.deepcopy(cfg)))
    assert '1. Window. TRAIN is [2020-01-01, 2024-01-01). Hidden: 2024-01-01 and later.' in txt
    m = re.search(r'Where the v8 literature notes contradict or update v6 and v7 \((\d+) rows: (\d+) contradicts', txt)
    assert m and int(m.group(1)) >= 20 and int(m.group(2)) >= 5
