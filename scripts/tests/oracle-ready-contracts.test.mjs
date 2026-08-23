// Contracts for the broker-native ready-state oracle transaction: Measure ->
// Attribute -> Improve -> Ratchet.
//
// Two kinds of assertion live here. The first kind pins every workflow-owned
// mirror of a broker registry against the broker's own copy, so drift between
// the two fails a test instead of failing a live iteration. The second kind
// exercises the pure validators directly, including the two failures the loop's
// safety rests on: a holdout-tainted Improve payload must be REJECTED, and a
// prepare agent that disagrees with computeRatchetVerdict must fail closed.
import test from 'node:test'
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { spawnSync } from 'node:child_process'
import vm from 'node:vm'
import { GATE_REGISTRY, OPERATION_REGISTRY, OracleLaneBroker, brokerGateOutputSha256, brokerGateReceiptId, isRatchetMemoryPath, operationReadsRatchetMemory } from '../oracle-lane-broker.mjs'

const read = relative => readFileSync(resolve(process.cwd(), relative), 'utf8')
const oracleSource = read('.claude/workflows/vol-oracle-iter.js')
const sprintSource = read('.claude/workflows/vol-sprint.js')

// Evaluates the declaration region of a workflow (everything between two
// anchors) in a fresh context and hands back the named bindings. The region is
// pure declarations, so nothing dispatches and every registry keeps its real
// value instead of a stub.
function loadScope(source, startAnchor, endAnchor, names) {
  const start = source.indexOf(startAnchor)
  const end = source.indexOf(endAnchor)
  assert.ok(start >= 0 && end > start, `could not slice ${startAnchor} .. ${endAnchor}`)
  const body = source.slice(start, end)
  // `args` is the workflow's caller payload. A clean task keeps vol-sprint's
  // top-level taint refusal from returning out of the slice.
  return vm.runInNewContext(`(() => {\n${body}\nreturn { ${names.join(', ')} }\n})()`, { args: { task: 'load declarations only' } })
}

const O = loadScope(oracleSource, 'function oracleRunId(', "phase('Capability')", [
  'CANONICAL_REF', 'TARGET_REGISTRY', 'AGGREGATE_REGISTRY', 'SPEED_METRIC_ID', 'CONVENTION_MAP',
  'READY_MEASURE_GATES', 'READY_MEASURE_COMMANDS', 'RATCHET_GATE_COMMANDS', 'RATCHET_GATE_IDS',
  'BOOTSTRAP_GATE_COMMANDS', 'SPRINT_GATE_IDS', 'RATCHET_MEMORY_PATHS', 'RATCHET_SCORECARD_PREFIXES',
  'REGRESSION_BOUND_MULTIPLIER', 'ORACLE_BENCH_TEST_COUNT',
  'holdoutTaintError', 'improveRequestError', 'sprintResultError', 'iterationCommandError', 'validSuccessEvidence',
  'sha256HexUtf8', 'ratchetDigestReceiptError', 'computeRatchetVerdict', 'expectedRatchetMetrics',
  'ratchetPrepareContractError', 'measureContractError', 'attributionPayloadFromMeasure', 'aggregatePayloadError',
])
const S = loadScope(sprintSource, 'const HOLDOUT_TAINT_RULES', "phase('Freeze')", [
  'SPRINT_SCOPE', 'SPRINT_GATE_REGISTRY', 'FIXED_INTEGRATION_GATE_IDS', 'holdoutTaintError', 'iterationCommandError',
  'derivedGateIds', 'closureError', 'scopePathError', 'sprintRunId',
])

// ── registry mirrors ────────────────────────────────────────────────────────

test('vol-sprint mirrors the broker sprint_build scope exactly', () => {
  assert.deepEqual([...S.SPRINT_SCOPE.exact], [...OPERATION_REGISTRY.sprint_build.exact])
  assert.deepEqual([...S.SPRINT_SCOPE.prefixes], [...OPERATION_REGISTRY.sprint_build.prefixes])
  assert.equal(S.scopePathError('atx-vol/src/pricing/american.cpp'), null)
  assert.match(S.scopePathError('atx-vol/bench/oracle/cohorts/holdout.json'), /outside the sprint build scope/)
  assert.match(S.scopePathError('../etc/passwd'), /unsafe repository path/)
})

test('the oracle Ratchet mirrors the broker ratchet write scope exactly', () => {
  assert.deepEqual([...O.RATCHET_MEMORY_PATHS], [...OPERATION_REGISTRY.ratchet.exact])
  assert.deepEqual([...O.RATCHET_SCORECARD_PREFIXES], [...OPERATION_REGISTRY.ratchet.prefixes])
})

test('every frozen workflow command equals the broker gate display it is run by', () => {
  for (const [gateId, command] of Object.entries(O.READY_MEASURE_GATES)) assert.equal(command, GATE_REGISTRY[gateId].display, gateId)
  for (const [gateId, command] of Object.entries(O.RATCHET_GATE_COMMANDS)) assert.equal(command, GATE_REGISTRY[gateId].display, gateId)
  for (const [gateId, command] of Object.entries(O.BOOTSTRAP_GATE_COMMANDS)) assert.equal(command, GATE_REGISTRY[gateId].display, gateId)
  for (const [gateId, command] of Object.entries(S.SPRINT_GATE_REGISTRY)) assert.equal(command, GATE_REGISTRY[gateId].display, gateId)
})

test('the closed sprint gate set is identical on both sides and contains no holdout or Measure gate', () => {
  assert.deepEqual([...O.SPRINT_GATE_IDS].sort(), Object.keys(S.SPRINT_GATE_REGISTRY).sort())
  const forbidden = [...O.RATCHET_GATE_IDS, ...Object.keys(O.READY_MEASURE_GATES), 'holdout_digest']
  for (const gateId of forbidden) {
    assert.equal(O.SPRINT_GATE_IDS.includes(gateId), false, `${gateId} reachable from the oracle sprint allowlist`)
    assert.equal(Object.hasOwn(S.SPRINT_GATE_REGISTRY, gateId), false, `${gateId} reachable from the vol-sprint registry`)
  }
  // The broker is the enforcing layer: it refuses these outside their operation.
  const broker = readFileSync(resolve(process.cwd(), 'scripts/oracle-lane-broker.mjs'), 'utf8')
  assert.match(broker, /holdout gate is restricted to Ratchet/)
  assert.match(broker, /Measure gate used outside Measure/)
})

test('every command the loop can publish as evidence survives iterationCommandError', () => {
  const commands = [
    ...O.READY_MEASURE_COMMANDS, ...Object.values(O.RATCHET_GATE_COMMANDS),
    ...Object.values(S.SPRINT_GATE_REGISTRY), O.BOOTSTRAP_GATE_COMMANDS.holdout_digest,
  ]
  for (const command of commands) assert.equal(O.iterationCommandError(command), null, command)
  // and the filter still bans what it is there to ban
  assert.match(O.iterationCommandError('ctest -L atx_vol_fast'), /label\/full regression/)
  assert.match(O.iterationCommandError('a && b'), /chained command/)
  assert.match(O.iterationCommandError('ctest -R atx_vol'), /broad or unanchored ctest/)
  assert.match(O.iterationCommandError('powershell scripts\\atx-build.ps1 build all'), /broad build target/)
  assert.match(O.iterationCommandError('run_all_gates'), /label\/full regression/)
  assert.match(O.iterationCommandError('build/atx-vol-tests.exe'), /direct test executable/)
})

test('the two workflows carry byte-identical copies of iterationCommandError', () => {
  const extract = source => {
    const start = source.indexOf('function iterationCommandError(')
    let depth = 0
    for (let index = source.indexOf('{', start); index < source.length; index += 1) {
      if (source[index] === '{') depth += 1
      if (source[index] === '}') depth -= 1
      if (depth === 0) return source.slice(start, index + 1)
    }
    return null
  }
  assert.equal(extract(oracleSource), extract(sprintSource))
})

test('frozen numeric contracts have not drifted', () => {
  assert.equal(O.REGRESSION_BOUND_MULTIPLIER, 1.01)
  // Deliberately a BARE LITERAL, not derived: this test's whole job is to make
  // ORACLE_BENCH_TEST_COUNT unmovable without a human editing this line, so
  // recomputing it from the workflow (or from the gate registry the next
  // assertion already cross-checks) would delete the review step. The message
  // names WHAT last moved it so the reviewer can tell an intended widening from
  // a case someone deleted to make a red suite go green.
  //   49 -> 53  ca493cfe  Mode B, five raw-NBBO cases
  //   53 -> 57  6950132f  Mode B European leg (iter-002), four cases:
  //                       EuropeanRowsInvertAgainstTheEuropeanLeg,
  //                       RefusesAEuropeanMidAtTheDiscountedForwardIntrinsic,
  //                       DeepItmEuropeanPutBelowIntrinsicStillInverts,
  //                       AmericanRowsKeepTheAmericanBoundsAndInverter
  assert.equal(O.ORACLE_BENCH_TEST_COUNT, 57,
    'ORACLE_BENCH_TEST_COUNT drifted. It is 57 because 6950132f pinned the four OracleBenchModeB European-leg ' +
    'cases (iter-002) on top of the 53 that ca493cfe left. Adding or removing an OracleBench gtest case moves ' +
    'FOUR definition sites in one commit: the TEST() macros in atx-vol/tests/oracle_bench_test.cpp, ' +
    '$script:OracleBenchTestIds in scripts/oracle-targeted-gate.ps1, the mode_b_targeted_tests ' +
    'PASS_REGULAR_EXPRESSION in atx-vol/tests/CMakeLists.txt, and ORACLE_BENCH_TEST_COUNT in ' +
    '.claude/workflows/vol-oracle-iter.js. Update this literal only after confirming the case list moved on purpose.')
  const gate = read('scripts/oracle-targeted-gate.ps1')
  const open = gate.indexOf('@(', gate.indexOf('$script:OracleBenchTestIds'))
  assert.ok(open > 0, '$script:OracleBenchTestIds is missing')
  let depth = 0
  let close = -1
  for (let index = open + 1; index < gate.length; index += 1) {
    if (gate[index] === '(') depth += 1
    if (gate[index] === ')') { depth -= 1; if (depth === 0) { close = index; break } }
  }
  assert.ok(close > open, '$script:OracleBenchTestIds is unterminated')
  // One quoted gtest ID per line; anchored so a stray apostrophe elsewhere in
  // the block cannot pair across two entries.
  const ids = [...gate.slice(open, close).matchAll(/^\s*'([A-Za-z][A-Za-z0-9_]*\.[A-Za-z][A-Za-z0-9_]*)'/gm)].map(match => match[1])
  assert.equal(new Set(ids).size, ids.length, '$script:OracleBenchTestIds contains a duplicate')
  assert.equal(ids.length, O.ORACLE_BENCH_TEST_COUNT, 'ORACLE_BENCH_TEST_COUNT and $script:OracleBenchTestIds must move together')
  // The bounded no-regression rule and its two-direction cross-check are intact.
  assert.match(oracleSource, /item\.value > \(baselineSymmetricById\.get\(item\.metric_id\) \|\| \{\}\)\.value \* REGRESSION_BOUND_MULTIPLIER/)
  assert.match(oracleSource, /accepted\.length !== wantedRegressions\.length/)
})

test('the hand-rolled workflow sha256 agrees with node crypto', () => {
  for (const value of ['', 'a', 'abc', 'x'.repeat(64), `${'a'.repeat(40)}:${'b'.repeat(64)}`, 'unicode \u00e9\u4e2d\ud83d\ude80']) {
    assert.equal(O.sha256HexUtf8(value), createHash('sha256').update(Buffer.from(value, 'utf8')).digest('hex'), JSON.stringify(value))
  }
})

// ── holdout confinement ─────────────────────────────────────────────────────

const CLEAN_TASK = 'Oracle RSI iter-001; typed aggregate hypotheses:\n[{"id":"H-DELTA-1","target_metric_ids":["mode_a_delta_rel"],"mechanism":"scale","prediction":"drops","blast_radius":"pricing","effort":"S"}]\nHard cutover; CHANGELOG BREAKING; no flags/shims/licensed rows.'

test('holdoutTaintError rejects every shape holdout identity could travel in', () => {
  assert.equal(O.holdoutTaintError(CLEAN_TASK), null)
  const tainted = [
    'target the holdout cohort next',
    'read atx-vol/bench/oracle/cohorts/holdout.json',
    'the cohort membership list moved',
    'load rows from store/part-0001.parquet',
    `digest ${'a'.repeat(64)}`,
    'use the nbbo midpoint for cell_id 41',
  ]
  for (const value of tainted) assert.notEqual(O.holdoutTaintError(value), null, value)
  // both workflows apply the identical filter
  for (const value of [CLEAN_TASK, ...tainted]) assert.equal(O.holdoutTaintError(value), S.holdoutTaintError(value), value)
})

test('a holdout-tainted Improve payload is REJECTED before any lane is opened', () => {
  const clean = { task: CLEAN_TASK, base: 'a'.repeat(40), run_key: 'vol-oracle-run-improve' }
  assert.equal(O.improveRequestError(clean), null)
  assert.match(O.improveRequestError({ ...clean, task: `${CLEAN_TASK}\nholdout mode_a_price_mae is 0.83` }), /names the holdout cohort/)
  assert.match(O.improveRequestError({ ...clean, task: `${CLEAN_TASK}\nmembership sha is pinned` }), /names cohort membership/)
  assert.match(O.improveRequestError({ ...clean, task: `${CLEAN_TASK}\n${'f'.repeat(64)}` }), /raw digest or blob identity/)
  assert.match(O.improveRequestError({ ...clean, base: 'main' }), /not an exact lowercase commit/)
  assert.match(O.improveRequestError({ ...clean, extra: 1 }), /keys invalid/)
  assert.match(O.improveRequestError(null), /request missing/)
})

test('an Improve result that touched holdout is REJECTED however it reports it', () => {
  const evidence = [{ command: 'atx-vol-oracle-bench --cohort smoke,tune --test american-rsi --aggregate-only', exit_code: 0, output: '{"status":"PASS"}' }]
  const ok = {
    passed: true, integration_branch: 'integration/oracle-improve-run', integration_sha: 'c'.repeat(40), integration_tree: 'd'.repeat(40),
    base_sha: 'a'.repeat(40), gate_ids: ['oracle-test:american-rsi'], gate_evidence: evidence,
  }
  assert.equal(O.sprintResultError(ok, { base_sha: 'a'.repeat(40) }), null)
  assert.match(O.sprintResultError({ ...ok, gate_ids: ['holdout_mode_a'] }, { base_sha: 'a'.repeat(40) }), /outside the closed sprint registry/)
  assert.match(O.sprintResultError({ ...ok, gate_ids: ['measure_mode_a'] }, { base_sha: 'a'.repeat(40) }), /outside the closed sprint registry/)
  assert.match(O.sprintResultError({
    ...ok, gate_evidence: [{ command: 'atx-vol-oracle-bench --cohort holdout --mode A --aggregate-only', exit_code: 0, output: 'x' }],
  }, { base_sha: 'a'.repeat(40) }), /names the holdout cohort/)
  assert.match(O.sprintResultError({ ...ok, base_sha: 'b'.repeat(40) }, { base_sha: 'a'.repeat(40) }), /did not build on the frozen canonical base/)
  assert.match(O.sprintResultError({ ...ok, integration_branch: 'lane/oracle-improve-run' }, { base_sha: 'a'.repeat(40) }), /not an integration lane/)
  assert.match(O.sprintResultError({ passed: false, failure: 'BOOM' }, { base_sha: 'a'.repeat(40) }), /Improve failed: BOOM/)
})

test('vol-sprint can never derive a gate ID outside its closed registry', () => {
  const rogue = [{
    file: 'atx-vol/src/pricing/american.cpp',
    unit_targets: ['atx-vol-tests', 'holdout_mode_a'], unit_regexes: [], oracle_tests: ['american-rsi', 'holdout-rsi'],
    pch_off_targets: [],
  }]
  const derived = [...S.derivedGateIds(rogue, true)]
  for (const id of derived) assert.ok(Object.hasOwn(S.SPRINT_GATE_REGISTRY, id), id)
  assert.deepEqual(derived.filter(id => /holdout|measure_/.test(id)), [])
  assert.deepEqual(derived, ['oracle-test:american-rsi', 'scorecard:mode_a_smoke_tune', 'scorecard:mode_b_smoke_tune', 'speed:rel_avx2_quiet', 'unit-build:atx-vol-tests'])
})

// ── ratchet contracts ───────────────────────────────────────────────────────

const SHA = { base: 'a'.repeat(40), baseTree: 'b'.repeat(40), integration: 'c'.repeat(40), integrationTree: 'd'.repeat(40), ratchet: 'e'.repeat(40), ratchetTree: 'f'.repeat(40) }
const DIGEST = '4'.repeat(64)
const WORKTREE = 'C:\\atx-wt\\pool-7'

function guard(canonical) {
  return {
    main_sha: '9'.repeat(40), canonical_sha: canonical,
    index_sha256: '1'.repeat(64), tracked_sha256: '2'.repeat(64), untracked_sha256: '3'.repeat(64), raw_sha256: '5'.repeat(64),
  }
}

function evidence(operation, { cwd = WORKTREE, command = 'fixed broker operation', output = 'PASS', beforeCanonical = SHA.base, afterCanonical = beforeCanonical } = {}) {
  return {
    logical_operation: operation, physical_cwd: cwd, command, exit_code: 0, output,
    raw_output_sha256: brokerGateOutputSha256(output), root_guard_before: guard(beforeCanonical), root_guard_after: guard(afterCanonical),
  }
}

// Builds a broker-shaped gate receipt exactly the way buildBrokerGateReceipt in
// scripts/oracle-lane-broker.mjs would, so the workflow's recomputation of the
// receipt digest is a real check and not a restatement of the fixture.
function gateReceipt(operationId, gateId, command, result, sha, tree, cwd = WORKTREE) {
  const output = JSON.stringify(result)
  const brokerEvidence = evidence(`gate:${gateId}`, { cwd, command, output })
  const receipt = { gate_id: gateId, tested_sha: sha, tested_tree: tree, command, exit_code: 0, output, result, broker_evidence: brokerEvidence }
  return { receipt_id: brokerGateReceiptId(operationId, { ...receipt, raw_output_sha256: brokerEvidence.raw_output_sha256 }), ...receipt }
}

function metric(metricId, value, pin) {
  const unit = metricId === O.SPEED_METRIC_ID ? 'rows_per_second'
    : [...O.TARGET_REGISTRY, ...O.AGGREGATE_REGISTRY].find(item => item.metric_id === metricId).unit
  return pin === undefined ? { metric_id: metricId, value, count: 1000, unit } : { metric_id: metricId, value, count: 1000, unit, pin }
}

const CONVENTIONS = Object.fromEntries(O.CONVENTION_MAP.required.map(key => [key, 'fixed']))
const BASELINE = 0.5
const SPEED_BASELINE = 2000000
const SPEED_PIN = 1800000

function measureGateReceipts() {
  const modeA = [...O.TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => metric(item.metric_id, BASELINE)), metric('mode_a_aggregate_error', BASELINE)]
  const modeB = [...O.TARGET_REGISTRY.filter(item => item.mode === 'B').map(item => metric(item.metric_id, BASELINE)), metric('mode_b_aggregate_error', BASELINE)]
  const speed = [metric(O.SPEED_METRIC_ID, SPEED_BASELINE, SPEED_PIN)]
  return [['measure_mode_a', modeA], ['measure_mode_b', modeB], ['measure_speed', speed]].map(([gateId, metrics]) =>
    gateReceipt('measure', gateId, O.READY_MEASURE_GATES[gateId],
      { schema_version: 1, status: 'PASS', command_id: gateId, observations: 1000, metrics }, SHA.base, SHA.baseTree))
}

function measureReport(acquire, expected) {
  const receipts = measureGateReceipts()
  return {
    status: 'ok', iter: expected.iter, tested_sha: SHA.base, tested_tree: SHA.baseTree, branch: expected.branch, base_sha: SHA.base,
    worktree: acquire.worktree, lease_name: acquire.lease_name, lease_run_id: expected.run_id, heartbeat_id: expected.heartbeat_id,
    keeper_pid: acquire.keeper_pid, keeper_process_started_utc: acquire.keeper_process_started_utc,
    acquisition_receipt: acquire.acquisition_receipt,
    analysis_context: { prior_refuted_ids: [], oracle_suspect_cells: [7], conventions: CONVENTIONS },
    gate_receipts: receipts,
    evidence: receipts.map(receipt => ({ command: receipt.command, exit_code: 0, output: receipt.output })),
    broker_evidence: [evidence('lane_open', { cwd: acquire.worktree })],
  }
}

function ratchetGateReceipts(scale) {
  const build = (gateId, metrics) => gateReceipt('ratchet', gateId, O.RATCHET_GATE_COMMANDS[gateId],
    { schema_version: 1, status: 'PASS', command_id: gateId, observations: 1000, metrics }, SHA.integration, SHA.integrationTree)
  return [
    build('holdout_mode_a', [...O.TARGET_REGISTRY.filter(item => item.mode === 'A').map(item => metric(item.metric_id, BASELINE * scale)), metric('mode_a_aggregate_error', BASELINE * scale)]),
    build('holdout_mode_b', [...O.TARGET_REGISTRY.filter(item => item.mode === 'B').map(item => metric(item.metric_id, BASELINE * scale)), metric('mode_b_aggregate_error', BASELINE * scale)]),
    build('rel_avx2_speed', [metric(O.SPEED_METRIC_ID, SPEED_BASELINE * 1.1)]),
  ]
}

function digestReceipt(sha = SHA.integration, digest = DIGEST) {
  const result = { schema_version: 1, status: 'PASS', observations: 1, command_id: 'holdout_digest', raw_output_sha256: O.sha256HexUtf8(`${sha}:${digest}`) }
  return gateReceipt('ratchet', 'holdout_digest', O.BOOTSTRAP_GATE_COMMANDS.holdout_digest, result, SHA.integration, SHA.integrationTree)
}

function ratchetReport(acquire, payload, expected, { scale = 0.9, verdict = 'ACCEPT', overrides = {} } = {}) {
  const receipts = ratchetGateReceipts(scale)
  const digest = digestReceipt()
  const byGate = new Map(receipts.map((receipt, index) => [receipt.gate_id, index]))
  const metrics = O.expectedRatchetMetrics(payload).map(item => {
    const gateId = item.metric_id === O.SPEED_METRIC_ID ? 'rel_avx2_speed' : item.mode === 'A' ? 'holdout_mode_a' : 'holdout_mode_b'
    const candidate = receipts[byGate.get(gateId)].result.metrics.find(entry => entry.metric_id === item.metric_id).value
    return { ...item, candidate, delta: candidate - item.baseline, evidence_index: byGate.get(gateId) }
  })
  return {
    tested_branch: expected.tested_branch, tested_sha: SHA.integration, tested_tree: SHA.integrationTree, base_sha: SHA.integration,
    ratchet_branch: expected.ratchet_branch, ratchet_sha: SHA.ratchet, ratchet_tree: SHA.ratchetTree,
    worktree: acquire.worktree, lease_name: acquire.lease_name, lease_run_id: expected.run_id, heartbeat_id: expected.heartbeat_id,
    keeper_pid: acquire.keeper_pid, keeper_process_started_utc: acquire.keeper_process_started_utc,
    acquisition_receipt: acquire.acquisition_receipt, digest_receipt: digest,
    applicable_modes: ['A', 'B'], metrics, gate_receipts: receipts, oracle_suspects_excluded: [],
    files_changed: ['atx-vol/bench/oracle/scorecards/iter-001.json', 'atx-vol/docs/LEDGER.md', 'atx-vol/docs/oracle/NORTHSTAR.md'],
    memory_verdict: verdict, holdout_summary: 'holdout improved on every target',
    hypotheses_confirmed: ['H-DELTA-1'], hypotheses_refuted: [],
    ledger_appended: ['2026-08-18 | oracle | iter-001 accepted'], northstar_updated: true,
    evidence: [digest, ...receipts].map(receipt => ({ command: receipt.command, exit_code: 0, output: receipt.output })),
    broker_evidence: [evidence('lane_commit', { cwd: acquire.worktree })],
    ...overrides,
  }
}

function leaseReceipt(identity, action) {
  const output = action === 'acquire'
    ? `LEASED pool=${identity.lease_name} path=${identity.worktree} branch=${identity.branch} base_sha=${identity.base_sha} run_id=${identity.run_id} owner_kind=heartbeat heartbeat_id=${identity.heartbeat_id} keeper_pid=${identity.keeper_pid} keeper_started_utc=${identity.keeper_process_started_utc} keeper_ready_utc=${identity.keeper_ready_utc}`
    : `RELEASE ${identity.lease_name} run_id=${identity.run_id}`
  return { action, ...identity, exit_code: 0, output }
}

function acquireFor(expected, leaseName = 'pool-7', keeperPid = 4242) {
  const identity = {
    lease_name: leaseName, run_id: expected.run_id, branch: expected.branch, base_sha: expected.base_sha,
    worktree: `C:\\atx-wt\\${leaseName}`, heartbeat_id: expected.heartbeat_id, keeper_pid: keeperPid,
    keeper_process_started_utc: '2026-08-18T09:00:00.0000000Z', keeper_ready_utc: '2026-08-18T09:00:01.0000000Z',
  }
  return {
    capability: '6'.repeat(64), operation_id: expected.operation_id, stage: expected.stage, ...identity,
    lease_start_sha: expected.base_sha, recovery_replay: false,
    acquisition_receipt: leaseReceipt(identity, 'acquire'),
    broker_evidence: evidence('lane_open', { cwd: 'C:\\atx' }),
  }
}

const RUN_ID = `vol-oracle-${SHA.base}-ready-iter-001`
const MEASURE_EXPECTED = {
  operation_id: 'measure', stage: 'measure', run_id: RUN_ID,
  branch: `lane/oracle-measure-iter-001-${RUN_ID}`, base_sha: SHA.base, heartbeat_id: `${RUN_ID}-measure`,
}
const RATCHET_EXPECTED = {
  operation_id: 'ratchet', stage: 'ratchet', run_id: RUN_ID,
  branch: `lane/oracle-ratchet-${RUN_ID}`, base_sha: SHA.integration, heartbeat_id: `${RUN_ID}-ratchet`,
}

test('Measure binds every gate receipt to the frozen canonical tree and the workflow-owned lane', () => {
  const acquire = acquireFor(MEASURE_EXPECTED)
  const expected = { iter: 'iter-001', branch: MEASURE_EXPECTED.branch, base_sha: SHA.base, base_tree: SHA.baseTree, run_id: RUN_ID, heartbeat_id: MEASURE_EXPECTED.heartbeat_id }
  const report = measureReport(acquire, expected)
  assert.equal(O.measureContractError(report, acquire, expected), null)
  assert.match(O.measureContractError({ ...report, tested_sha: SHA.integration }, acquire, expected), /frozen canonical base tree/)
  assert.match(O.measureContractError({ ...report, lease_name: 'pool-9' }, acquire, expected), /not the workflow-owned lane/)
  assert.match(O.measureContractError({ ...report, status: 'failed' }, acquire, expected), /did not complete/)
  // a receipt whose output was edited after the fact no longer matches its digest
  const tampered = JSON.parse(JSON.stringify(report))
  tampered.gate_receipts[0].output = `${tampered.gate_receipts[0].output} `
  assert.match(O.measureContractError(tampered, acquire, expected), /gate receipt invalid: measure_mode_a/)
})

test('the Ratchet proves the holdout digest without ever transporting it', () => {
  const binding = { operation_id: 'ratchet', sha: SHA.integration, tree: SHA.integrationTree, worktree: WORKTREE, holdout_digest: DIGEST }
  assert.equal(O.ratchetDigestReceiptError(digestReceipt(), binding), null)
  assert.equal(JSON.stringify(digestReceipt()).includes(DIGEST), false, 'the digest must not appear anywhere in the receipt')
  assert.match(O.ratchetDigestReceiptError(digestReceipt(SHA.integration, '7'.repeat(64)), binding), /does not match the frozen membership digest/)
  assert.match(O.ratchetDigestReceiptError(digestReceipt(SHA.base, DIGEST), binding), /does not match the frozen membership digest/)
  assert.match(O.ratchetDigestReceiptError(null, binding), /gate receipt invalid/)
})

function ratchetExpected(payload) {
  const acquire = acquireFor(RATCHET_EXPECTED)
  return {
    acquire,
    expected: {
      tested_sha: SHA.integration, tested_tree: SHA.integrationTree, tested_branch: 'integration/oracle-improve-run',
      ratchet_branch: RATCHET_EXPECTED.branch, holdout_digest: DIGEST, run_id: RUN_ID, heartbeat_id: RATCHET_EXPECTED.heartbeat_id,
      lease_name: acquire.lease_name, worktree: acquire.worktree, keeper_pid: acquire.keeper_pid,
      keeper_process_started_utc: acquire.keeper_process_started_utc,
      applicable_modes: ['A', 'B'], baseline_contract: payload, hypothesis_ids: ['H-DELTA-1', 'H-VEGA-2'],
    },
  }
}

function baselinePayload() {
  const acquire = acquireFor(MEASURE_EXPECTED)
  const expected = { iter: 'iter-001', branch: MEASURE_EXPECTED.branch, base_sha: SHA.base, base_tree: SHA.baseTree, run_id: RUN_ID, heartbeat_id: MEASURE_EXPECTED.heartbeat_id }
  return O.attributionPayloadFromMeasure(measureReport(acquire, expected))
}

test('the aggregate attribution payload Measure produces is schema-clean and holdout-free', () => {
  const payload = baselinePayload()
  assert.equal(O.aggregatePayloadError(payload), null)
  assert.equal(O.holdoutTaintError(JSON.stringify(payload)), null)
  assert.equal(payload.target_metrics.length, O.TARGET_REGISTRY.length)
  assert.equal(payload.aggregate_metrics.length, O.AGGREGATE_REGISTRY.length)
})

test('a well-formed broker-native Ratchet prepare satisfies the contract', () => {
  const payload = baselinePayload()
  const { acquire, expected } = ratchetExpected(payload)
  const report = ratchetReport(acquire, payload, { tested_branch: expected.tested_branch, ratchet_branch: expected.ratchet_branch, run_id: RUN_ID, heartbeat_id: expected.heartbeat_id })
  assert.equal(O.ratchetPrepareContractError(report, expected), null)
  assert.equal(O.computeRatchetVerdict(report), 'ACCEPT')
})

test('the Ratchet fails closed on suspect exclusion, foreign memory paths and unattributed hypotheses', () => {
  const payload = baselinePayload()
  const { acquire, expected } = ratchetExpected(payload)
  const base = { tested_branch: expected.tested_branch, ratchet_branch: expected.ratchet_branch, run_id: RUN_ID, heartbeat_id: expected.heartbeat_id }
  const withOverride = overrides => O.ratchetPrepareContractError(ratchetReport(acquire, payload, base, { overrides }), expected)
  assert.match(withOverride({ oracle_suspects_excluded: [7] }), /suspect exclusion is retired/)
  assert.match(withOverride({ files_changed: ['atx-vol/src/pricing/american.cpp'] }), /outside its broker scope/)
  assert.match(withOverride({ files_changed: ['atx-vol/docs/LEDGER.md', 'atx-vol/docs/oracle/NORTHSTAR.md'] }), /did not commit the ledger, north star and scorecard/)
  assert.match(withOverride({ hypotheses_confirmed: ['H-UNKNOWN'] }), /disjoint subset of the attributed hypotheses/)
  assert.match(withOverride({ hypotheses_confirmed: ['H-DELTA-1'], hypotheses_refuted: ['H-DELTA-1'] }), /disjoint subset of the attributed hypotheses/)
  assert.match(withOverride({ northstar_updated: false }), /memory\/summary incomplete/)
  assert.match(withOverride({ ratchet_sha: SHA.integration }), /exact integration\/base identity mismatch/)
  // evidence must be exactly the four frozen commands, once each
  const good = ratchetReport(acquire, payload, base)
  assert.match(O.ratchetPrepareContractError({ ...good, evidence: good.evidence.slice(1) }, expected), /exact frozen gate command set/)
  assert.match(O.ratchetPrepareContractError({
    ...good, evidence: [...good.evidence, { command: 'atx-vol-oracle-bench --cohort holdout --market-check 7 --aggregate-only', exit_code: 0, output: 'x' }],
  }, expected), /exact frozen gate command set/)
})

test('a Ratchet candidate that is not the value in its cited gate receipt fails closed', () => {
  const payload = baselinePayload()
  const { acquire, expected } = ratchetExpected(payload)
  const base = { tested_branch: expected.tested_branch, ratchet_branch: expected.ratchet_branch, run_id: RUN_ID, heartbeat_id: expected.heartbeat_id }
  const report = ratchetReport(acquire, payload, base)
  const forged = JSON.parse(JSON.stringify(report))
  const target = forged.metrics.find(item => item.metric_id === 'mode_a_price_mae')
  target.candidate = 0.0001
  target.delta = target.candidate - target.baseline
  assert.match(O.ratchetPrepareContractError(forged, expected), /candidate is not bound to typed gate output/)
  const reindexed = JSON.parse(JSON.stringify(report))
  reindexed.metrics.find(item => item.metric_id === 'mode_a_price_mae').evidence_index = 2
  assert.match(O.ratchetPrepareContractError(reindexed, expected), /does not cite the receipt it was measured from/)
  // The Ratchet must benchmark the SEALED integration commit, not its own memory
  // commit: a receipt stamped with the post-commit HEAD is rejected.
  const gatedAfterCommit = JSON.parse(JSON.stringify(report))
  for (const receipt of gatedAfterCommit.gate_receipts) { receipt.tested_sha = SHA.ratchet; receipt.tested_tree = SHA.ratchetTree }
  assert.match(O.ratchetPrepareContractError(gatedAfterCommit, expected), /required gate receipt invalid/)
})

test('computeRatchetVerdict owns the verdict and rejects a regressing candidate', () => {
  const payload = baselinePayload()
  const { acquire, expected } = ratchetExpected(payload)
  const base = { tested_branch: expected.tested_branch, ratchet_branch: expected.ratchet_branch, run_id: RUN_ID, heartbeat_id: expected.heartbeat_id }
  const worse = ratchetReport(acquire, payload, base, { scale: 1.5, verdict: 'REJECT' })
  assert.equal(O.ratchetPrepareContractError(worse, expected), null)
  assert.equal(O.computeRatchetVerdict(worse), 'REJECT')
  const better = ratchetReport(acquire, payload, base, { scale: 0.9, verdict: 'ACCEPT' })
  assert.equal(O.computeRatchetVerdict(better), 'ACCEPT')
  // the prepare agent's own claim is never consulted by the computation
  const lying = ratchetReport(acquire, payload, base, { scale: 1.5, verdict: 'ACCEPT' })
  assert.equal(O.computeRatchetVerdict(lying), 'REJECT')
  assert.notEqual(lying.memory_verdict, O.computeRatchetVerdict(lying))
})

// ── end-to-end mocked ready transaction ─────────────────────────────────────

function compileWorkflow(source) {
  return new Function('args', 'agent', 'phase', 'workflow', 'pipeline', `return (async () => {\n${source.replace('export const meta', 'const meta')}\n})()`)
}

const HYPOTHESES = [
  { id: 'H-DELTA-1', target_metric_ids: ['mode_a_delta_rel'], mechanism: 'delta scale is off by the contract multiplier', prediction: 'mode A delta relative error falls', blast_radius: 'pricing only', effort: 'S' },
  { id: 'H-VEGA-2', target_metric_ids: ['mode_b_vega_rel'], mechanism: 'vega uses the wrong year fraction', prediction: 'mode B vega relative error falls', blast_radius: 'pricing only', effort: 'M' },
]

function sprintResult() {
  return {
    passed: true, blocked: [], failure: null,
    integration_branch: 'integration/oracle-improve-run', integration_sha: SHA.integration, integration_tree: SHA.integrationTree,
    gate_ids: ['oracle-test:american-rsi', 'unit-build:atx-vol-tests'],
    gate_evidence: [
      { command: 'atx-vol-oracle-bench --cohort smoke,tune --test american-rsi --aggregate-only', exit_code: 0, output: '{"status":"PASS"}' },
      { command: 'powershell scripts\\atx-build.ps1 -Preset dev build atx-vol-tests', exit_code: 0, output: 'build ok' },
    ],
    run_id: 'vol-sprint-run', base_sha: SHA.base,
  }
}

async function runReady({ scale = 0.9, verdict = 'ACCEPT', sprintOverrides = {}, ratchetOverrides = {} } = {}) {
  const measureAcquire = acquireFor(MEASURE_EXPECTED)
  const ratchetAcquire = acquireFor(RATCHET_EXPECTED)
  const measureExpected = { iter: 'iter-001', branch: MEASURE_EXPECTED.branch, base_sha: SHA.base, base_tree: SHA.baseTree, run_id: RUN_ID, heartbeat_id: MEASURE_EXPECTED.heartbeat_id }
  const ratchetBase = { tested_branch: 'integration/oracle-improve-run', ratchet_branch: RATCHET_EXPECTED.branch, run_id: RUN_ID, heartbeat_id: RATCHET_EXPECTED.heartbeat_id }
  const payload = O.attributionPayloadFromMeasure(measureReport(measureAcquire, measureExpected))
  const labels = []
  const prompts = new Map()
  const sprintCalls = []
  const releaseReceipt = acquire => ({
    released: true, lease_name: acquire.lease_name, sha: null, tree: null, finalize_capability: '',
    release_receipt: leaseReceipt({
      lease_name: acquire.lease_name, run_id: acquire.run_id, branch: acquire.branch, base_sha: acquire.base_sha,
      worktree: acquire.worktree, heartbeat_id: acquire.heartbeat_id, keeper_pid: acquire.keeper_pid,
      keeper_process_started_utc: acquire.keeper_process_started_utc, keeper_ready_utc: acquire.keeper_ready_utc,
    }, 'release'),
    broker_evidence: evidence('lane_release', { cwd: 'C:\\atx' }),
  })
  const agent = async (prompt, options) => {
    labels.push(options.label)
    prompts.set(options.label, prompt)
    switch (options.label) {
      case 'capability': return {
        state: 'ready', canonical_ref: O.CANONICAL_REF, canonical_exists: true, base_ref: O.CANONICAL_REF,
        base_sha: SHA.base, base_tree: SHA.baseTree, holdout_digest_receipt: DIGEST, next_iter: 'iter-001',
        evidence: [{ command: 'powershell scripts\\oracle-capability.ps1', exit_code: 0, output: `state=ready canonical_exists=true base_ref=${O.CANONICAL_REF} base_sha=${SHA.base} base_tree=${SHA.baseTree}` }],
        broker_evidence: evidence('capability_probe', { cwd: 'C:\\atx' }),
      }
      case 'measure-acquire': return measureAcquire
      case 'measure': return measureReport(measureAcquire, measureExpected)
      case 'measure-release': return { ...releaseReceipt(measureAcquire), sha: SHA.base, tree: SHA.baseTree }
      case 'attribute': return { hypotheses: HYPOTHESES, new_suspect_candidates: [] }
      case 'ratchet-acquire': return ratchetAcquire
      case 'ratchet-prepare': return ratchetReport(ratchetAcquire, payload, ratchetBase, { scale, verdict, overrides: ratchetOverrides })
      case 'ratchet-release': return { ...releaseReceipt(ratchetAcquire), sha: SHA.ratchet, tree: SHA.ratchetTree, finalize_capability: '8'.repeat(64) }
      case 'ratchet-finalize-discard': return {
        discarded: true, ref: O.CANONICAL_REF, new_sha: SHA.ratchet, operation_id: 'ratchet',
        command: 'discard-finalizer:ratchet', exit_code: 0, output: `DISCARDED ratchet ${SHA.ratchet}`,
        broker_evidence: evidence('canonical_discard', { cwd: 'C:\\atx', command: 'discard-finalizer:ratchet', output: `DISCARDED ratchet ${SHA.ratchet}` }),
      }
      case 'ratchet-cas-finalizer': return {
        ref: O.CANONICAL_REF, new_sha: SHA.ratchet, new_tree: SHA.ratchetTree, expected_old_sha: SHA.base,
        command: `git update-ref ${O.CANONICAL_REF} ${SHA.ratchet} ${SHA.base}`, exit_code: 0, output: SHA.ratchet,
        broker_evidence: evidence('canonical_finalize', { cwd: 'C:\\atx', afterCanonical: SHA.ratchet }),
      }
      case 'ratchet-post-decision-audit': {
        const landed = labels.includes('ratchet-cas-finalizer') ? SHA.ratchet : SHA.base
        return {
          ref: O.CANONICAL_REF, sha: landed, command: `git rev-parse ${O.CANONICAL_REF}`, exit_code: 0, output: landed,
          broker_evidence: evidence('canonical_audit', { cwd: 'C:\\atx', beforeCanonical: landed }),
        }
      }
      default: throw new Error(`unexpected oracle label: ${options.label}`)
    }
  }
  const nested = async (name, nestedArgs) => {
    assert.equal(name, 'vol-sprint')
    sprintCalls.push(nestedArgs)
    return { ...sprintResult(), ...sprintOverrides }
  }
  const result = await compileWorkflow(oracleSource)({}, agent, () => {}, nested, async () => {})
  return { result, labels, prompts, sprintCalls }
}

test('the mocked ready transaction runs Measure, Attribute, Improve and Ratchet and lands once', async () => {
  const { result, labels, sprintCalls } = await runReady()
  assert.equal(result.failure, null)
  assert.equal(result.verdict, 'ACCEPT')
  assert.equal(result.landing_status, 'COMMITTED')
  assert.equal(result.canonical_after, SHA.ratchet)
  assert.equal(result.iteration, 'iter-001')
  assert.deepEqual([...labels], [
    'capability', 'measure-acquire', 'measure', 'measure-release', 'attribute',
    'ratchet-acquire', 'ratchet-prepare', 'ratchet-release', 'ratchet-cas-finalizer', 'ratchet-post-decision-audit',
  ])
  // exactly one canonical mutation, and it happens after the audit-bearing CAS
  assert.equal(labels.filter(label => label === 'ratchet-cas-finalizer').length, 1)
  // Improve was reached exactly once, on the frozen canonical base, with a
  // holdout-free typed request
  assert.equal(sprintCalls.length, 1)
  assert.equal(sprintCalls[0].base, SHA.base)
  assert.equal(O.improveRequestError(sprintCalls[0]), null)
  // `base` and `run_key` are workflow-owned identity and legitimately carry the
  // frozen SHA; the analyst-authored task is the only field that could smuggle
  // holdout content, and it is the field the taint filter guards.
  assert.equal(O.holdoutTaintError(sprintCalls[0].task), null)
  assert.equal(sprintCalls[0].task.includes('H-DELTA-1'), true)
})

test('a REJECT verdict leaves canonical untouched and never reaches the finalizer', async () => {
  const { result, labels } = await runReady({ scale: 1.5, verdict: 'REJECT' })
  assert.equal(result.verdict, 'REJECT')
  assert.equal(result.landing_status, 'UNCHANGED_REJECT')
  assert.equal(result.canonical_after, SHA.base)
  assert.equal(labels.includes('ratchet-cas-finalizer'), false, 'a REJECT must never consume the finalize capability')
  assert.equal(labels.includes('ratchet-finalize-discard'), true, 'a REJECT must destroy the unused finalize capability')
  assert.equal(result.failure, null)
  // A completed REJECT still publishes its holdout summary to the caller; what
  // it must never do is move canonical or increment the iteration.
  assert.equal(typeof result.holdout.summary, 'string')
  assert.equal(result.canonical_after, result.base_sha)
})

test('a prepare agent whose verdict disagrees with the workflow fails the transaction closed', async () => {
  const { result, labels } = await runReady({ scale: 1.5, verdict: 'ACCEPT' })
  assert.equal(result.verdict, 'FAILED')
  assert.equal(result.failure, 'prepared memory verdict disagrees with workflow computation')
  assert.equal(result.canonical_after, null)
  assert.equal(result.ratchet, null)
  assert.equal(labels.includes('ratchet-cas-finalizer'), false)
  assert.equal(labels.includes('ratchet-post-decision-audit'), false)
})

test('an Improve result that reports a holdout gate stops the transaction before the Ratchet lane', async () => {
  const { result, labels } = await runReady({ sprintOverrides: { gate_ids: ['holdout_mode_a'] } })
  assert.equal(result.verdict, 'FAILED')
  assert.match(result.failure, /outside the closed sprint registry/)
  assert.equal(labels.includes('ratchet-acquire'), false, 'no holdout gate may run after a tainted Improve')
  assert.equal(result.holdout, null)
})

// ── end-to-end mocked Improve sprint ────────────────────────────────────────

const SPRINT_LANE = {
  id: 'american-delta', title: 'fix the Mode A delta scale', goal: 'correct the delta multiplier',
  files_in_scope: ['atx-vol/src/pricing/american.cpp'],
  gate_closure: [{
    file: 'atx-vol/src/pricing/american.cpp', unit_targets: ['atx-vol-tests'],
    unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], oracle_tests: ['american-rsi'], pch_off_targets: [],
  }],
  done_criteria: 'the anchored unit test and the hypothesis OracleBench test pass',
  out_of_scope: 'anything outside the one scoped file',
}
const LANE_SHA = '1'.repeat(40)
const LANE_TREE = '2'.repeat(40)

function sprintGateReceipts(gateIds, sha, tree, worktree) {
  return gateIds.map(gateId => {
    const command = S.SPRINT_GATE_REGISTRY[gateId]
    const output = `{"gate":"${gateId}","status":"PASS"}`
    const brokerEvidence = evidence(`gate:${gateId}`, { cwd: worktree, command, output })
    const receipt = { gate_id: gateId, tested_sha: sha, tested_tree: tree, command, exit_code: 0, output, broker_evidence: brokerEvidence }
    return { receipt_id: brokerGateReceiptId('sprint_build', { ...receipt, raw_output_sha256: brokerEvidence.raw_output_sha256 }), ...receipt }
  })
}

async function runSprint({ laneGatesBeforeCommit = false } = {}) {
  const task = 'Oracle RSI iter-001; typed aggregate hypotheses:\n[]\nHard cutover; CHANGELOG BREAKING.'
  const runId = S.sprintRunId(SHA.base, task, 'caller-key')
  const laneBranch = `lane/oracle-improve-american-delta-${runId}`
  const integrationBranch = `integration/oracle-improve-${runId}`
  const laneExpected = { operation_id: 'sprint_build', stage: 'improve', run_id: runId, branch: laneBranch, base_sha: SHA.base, heartbeat_id: `${runId}-lane-american-delta` }
  const integrationExpected = { operation_id: 'sprint_integration', stage: 'improve-gate', run_id: runId, branch: integrationBranch, base_sha: SHA.base, heartbeat_id: `${runId}-integration` }
  const laneAcquire = acquireFor(laneExpected, 'pool-3', 1111)
  const integrationAcquire = acquireFor(integrationExpected, 'pool-4', 2222)
  const laneGateIds = [...S.derivedGateIds(SPRINT_LANE.gate_closure)]
  const integrationGateIds = [...S.derivedGateIds(SPRINT_LANE.gate_closure, true)]
  // The whole point of the ordering rule: gate receipts carry the lane HEAD the
  // broker stamped them with, which is the committed SHA only when lane_commit
  // ran first.
  const laneReceipts = laneGatesBeforeCommit
    ? sprintGateReceipts(laneGateIds, SHA.base, SHA.baseTree, laneAcquire.worktree)
    : sprintGateReceipts(laneGateIds, LANE_SHA, LANE_TREE, laneAcquire.worktree)
  const labels = []
  const agent = async (_prompt, options) => {
    labels.push(options.label)
    switch (options.label) {
      case 'freeze-base': return { ref: SHA.base, sha: SHA.base, evidence: evidence('ref_resolve', { cwd: 'C:\\atx', command: `git rev-parse --verify ${SHA.base}^{commit}`, output: SHA.base }) }
      case 'plan': return { lanes: [SPRINT_LANE], shared_files_note: 'one lane owns the only shared file' }
      case 'lane-acquire:american-delta': return laneAcquire
      case 'build:american-delta': return {
        lane_id: 'american-delta', outcome: 'DONE', branch: laneBranch, sha: LANE_SHA, tree: LANE_TREE, base_sha: SHA.base,
        worktree: laneAcquire.worktree, lease_name: laneAcquire.lease_name, lease_run_id: runId, heartbeat_id: laneExpected.heartbeat_id,
        keeper_pid: laneAcquire.keeper_pid, keeper_process_started_utc: laneAcquire.keeper_process_started_utc,
        acquisition_receipt: laneAcquire.acquisition_receipt, files_changed: ['atx-vol/src/pricing/american.cpp'],
        gate_receipts: laneReceipts,
        evidence: laneReceipts.map(receipt => ({ command: receipt.command, exit_code: 0, output: receipt.output })),
        broker_evidence: [evidence('lane_commit', { cwd: laneAcquire.worktree })], deviations: '',
      }
      case 'review:american-delta': return {
        verdict: 'APPROVE', reviewed_sha: LANE_SHA, findings: [],
        evidence: [{ command: `git diff --no-ext-diff ${SHA.base}...${LANE_SHA}`, exit_code: 0, output: 'reviewed the scoped diff' }],
        broker_evidence: [evidence('commit_inspect', { cwd: 'C:\\atx' })],
      }
      case 'lane-release:american-delta': return {
        released: true, lease_name: laneAcquire.lease_name, sha: LANE_SHA, tree: LANE_TREE, finalize_capability: '',
        release_receipt: leaseReceipt({
          lease_name: laneAcquire.lease_name, run_id: runId, branch: laneBranch, base_sha: SHA.base, worktree: laneAcquire.worktree,
          heartbeat_id: laneExpected.heartbeat_id, keeper_pid: laneAcquire.keeper_pid,
          keeper_process_started_utc: laneAcquire.keeper_process_started_utc, keeper_ready_utc: laneAcquire.keeper_ready_utc,
        }, 'release'),
        broker_evidence: evidence('lane_release', { cwd: 'C:\\atx' }),
      }
      case 'integration-acquire': return integrationAcquire
      case 'integration-gate': {
        const integrationReceipt = {
          reviewed_sha: LANE_SHA, reviewed_tree: LANE_TREE, head_after: SHA.integration, tree_after: SHA.integrationTree,
          command: `git merge --no-ff ${LANE_SHA}`, exit_code: 0, output: `merged\n${SHA.integration}`,
          broker_evidence: evidence('lane_integrate', { cwd: integrationAcquire.worktree }),
        }
        return {
          passed: true, base_sha: SHA.base, lease_run_id: runId, integration_branch: integrationBranch,
          integration_sha: SHA.integration, integration_tree: SHA.integrationTree,
          integration_worktree: integrationAcquire.worktree, integration_lease: integrationAcquire.lease_name,
          integration_heartbeat_id: integrationExpected.heartbeat_id, keeper_pid: integrationAcquire.keeper_pid,
          keeper_process_started_utc: integrationAcquire.keeper_process_started_utc,
          integrated_shas: [LANE_SHA], integration_receipts: [integrationReceipt],
          head_receipt: { ref: 'HEAD', sha: SHA.integration, tree: SHA.integrationTree, command: 'git rev-parse HEAD', exit_code: 0, output: SHA.integration },
          gate_receipts: sprintGateReceipts(integrationGateIds, SHA.integration, SHA.integrationTree, integrationAcquire.worktree),
          broker_evidence: [integrationReceipt.broker_evidence],
        }
      }
      case 'integration-release': return {
        released: true, lease_name: integrationAcquire.lease_name, sha: SHA.integration, tree: SHA.integrationTree, finalize_capability: '',
        release_receipt: leaseReceipt({
          lease_name: integrationAcquire.lease_name, run_id: runId, branch: integrationBranch, base_sha: SHA.base,
          worktree: integrationAcquire.worktree, heartbeat_id: integrationExpected.heartbeat_id, keeper_pid: integrationAcquire.keeper_pid,
          keeper_process_started_utc: integrationAcquire.keeper_process_started_utc, keeper_ready_utc: integrationAcquire.keeper_ready_utc,
        }, 'release'),
        broker_evidence: evidence('lane_release', { cwd: 'C:\\atx' }),
      }
      default: throw new Error(`unexpected sprint label: ${options.label}`)
    }
  }
  const result = await compileWorkflow(sprintSource)({ task, base: SHA.base, run_key: 'caller-key' }, agent, () => {}, async () => { throw new Error('nested workflow reached') }, async () => {})
  return { result, labels, integrationGateIds }
}

test('the mocked Improve sprint opens, gates and releases every lane through the broker', async () => {
  const { result, labels, integrationGateIds } = await runSprint()
  assert.equal(result.failure, null)
  assert.equal(result.passed, true)
  assert.match(result.integration_branch, /^integration\//)
  assert.equal(result.integration_sha, SHA.integration)
  assert.deepEqual([...labels], [
    'freeze-base', 'plan', 'lane-acquire:american-delta', 'build:american-delta', 'review:american-delta',
    'lane-release:american-delta', 'integration-acquire', 'integration-gate', 'integration-release',
  ])
  // and its output satisfies the oracle loop's own acceptance contract
  assert.equal(O.sprintResultError(result, { base_sha: SHA.base }), null)
  assert.deepEqual([...result.gate_ids], integrationGateIds)
  assert.ok(integrationGateIds.includes('scorecard:mode_a_smoke_tune'))
  assert.ok(integrationGateIds.includes('unit-test:^AmericanGreeks.Delta_MatchesFd_Put$'))
})

test('a lane whose gates ran before its commit is rejected, not merged', async () => {
  const { result, labels } = await runSprint({ laneGatesBeforeCommit: true })
  assert.equal(result.passed, false)
  assert.match(result.failure, /gate receipt is not bound to the tested SHA\/tree/)
  assert.equal(labels.includes('integration-acquire'), false, 'an unbound lane must never reach integration')
  assert.ok(labels.includes('lane-release:american-delta'), 'a failed lane is still released')
})

test('no holdout command or cohort identity reaches any stage before the Ratchet lane', async () => {
  const { prompts } = await runReady()
  // Prompts legitimately carry the frozen SHA/tree the stage is pinned to, and
  // they legitimately FORBID cohort membership by name, so this checks the one
  // property that matters: no stage before the Ratchet may name the holdout
  // cohort or any per-row artifact at all.
  const holdoutShaped = /holdout|\.parquet\b|\bnbbo\b|\bcell_id\b/i
  for (const label of ['capability', 'measure-acquire', 'measure', 'measure-release', 'attribute']) {
    assert.doesNotMatch(prompts.get(label), holdoutShaped, `${label} prompt carries holdout identity`)
  }
  // the Measure prompt names only the three frozen smoke+tune commands
  for (const command of O.READY_MEASURE_COMMANDS) assert.ok(prompts.get('measure').includes(command), command)
  // and the Ratchet prompt is the only one that may name a holdout gate
  assert.match(prompts.get('ratchet-prepare'), holdoutShaped)
  for (const command of Object.values(O.RATCHET_GATE_COMMANDS)) assert.ok(prompts.get('ratchet-prepare').includes(command), command)
})

// ── holdout leak scoping (the broker's three read doors) ────────────────────
//
// Ratchet memory - the iteration scorecards, the research ledger, the north
// star and the convergence changelog - carries holdout-derived numbers, and a
// tuning stage that can read last iteration's holdout aggregates is tuning
// against the test set. The broker therefore scopes repo_search, repo_read and
// commit_inspect BY OPERATION: only a ratchet or bootstrap capability sees
// those paths, and a caller with no capability at all is the fail-closed case.
// These tests run against a real broker on a real repository, so reverting the
// scoping in scripts/oracle-lane-broker.mjs makes them fail.

const RATCHET_MEMORY_FIXTURE_PATHS = Object.freeze([
  'atx-vol/bench/oracle/scorecards/iter-001.json', 'atx-vol/docs/LEDGER.md',
  'atx-vol/docs/oracle/NORTHSTAR.md', 'atx-vol/docs/oracle/CONVERGENCE_CHANGELOG.md',
])

function leakFixture() {
  const sandbox = mkdtempSync(join(tmpdir(), 'atx-oracle-leak-scope-'))
  const root = join(sandbox, 'repo')
  mkdirSync(root, { recursive: true })
  const git = (...args) => {
    const result = spawnSync('git', args, { cwd: root, encoding: 'utf8', windowsHide: true })
    assert.equal(result.status, 0, `git ${args.join(' ')} failed: ${result.stdout || ''}${result.stderr || ''}`)
    return String(result.stdout || '').trim()
  }
  git('init', '-b', 'main')
  git('config', 'user.email', 'leak-scope-test@example.invalid')
  git('config', 'user.name', 'Leak Scope Test')
  const write = (rel, content) => {
    const target = join(root, ...rel.split('/'))
    mkdirSync(dirname(target), { recursive: true })
    writeFileSync(target, content, 'utf8')
  }
  write('atx-vol/src/pricing/american.cpp', 'int visible_pricing_marker() { return 41; }\n')
  git('add', '--', 'atx-vol')
  git('commit', '-m', 'fixture base')
  const base = git('rev-parse', 'HEAD')
  write('atx-vol/docs/LEDGER.md', 'holdout_leak_marker ledger 0.83\n')
  write('atx-vol/docs/oracle/NORTHSTAR.md', 'holdout_leak_marker northstar 0.83\n')
  write('atx-vol/docs/oracle/CONVERGENCE_CHANGELOG.md', 'holdout_leak_marker convergence 0.83\n')
  write('atx-vol/bench/oracle/scorecards/iter-001.json', '{"holdout_leak_marker":0.83}\n')
  write('atx-vol/src/pricing/visible_change.cpp', 'int visible_change_marker() { return 42; }\n')
  git('add', '--', 'atx-vol')
  git('commit', '-m', 'ratchet memory plus one visible change')
  const candidate = git('rev-parse', 'HEAD')
  const broker = new OracleLaneBroker({ root, poolRoot: join(sandbox, 'atx-wt'), testMode: true })
  return { sandbox, broker, base, candidate }
}

test('ratchet memory classification matches its writers and fails closed on the unidentified reader', () => {
  for (const path of RATCHET_MEMORY_FIXTURE_PATHS) assert.equal(isRatchetMemoryPath(path), true, path)
  assert.equal(isRatchetMemoryPath('atx-vol/src/pricing/american.cpp'), false)
  assert.equal(isRatchetMemoryPath('atx-vol/CHANGELOG.md'), false)
  for (const reader of ['ratchet', 'bootstrap_data', 'bootstrap_mode_a', 'bootstrap_conventions', 'bootstrap_mode_b', 'bootstrap_integration']) {
    assert.equal(operationReadsRatchetMemory(reader), true, reader)
    assert.ok(Object.hasOwn(OPERATION_REGISTRY, reader), `${reader} is not a registered broker operation`)
  }
  for (const tuner of ['measure', 'sprint_build', 'sprint_integration', 'unknown', '', null, undefined]) {
    assert.equal(operationReadsRatchetMemory(tuner), false, String(tuner))
  }
})

test('repo_search and repo_read withhold ratchet memory from a caller with no capability', () => {
  const { sandbox, broker } = leakFixture()
  try {
    assert.deepEqual(broker.repoSearch({ query: 'holdout_leak_marker' }).hits, [], 'a capability-less search served ratchet memory')
    const visible = broker.repoSearch({ query: 'visible_pricing_marker' })
    assert.equal(visible.hits.length, 1)
    assert.equal(visible.hits[0].path, 'atx-vol/src/pricing/american.cpp')
    const read = broker.repoRead({ file_id: visible.hits[0].file_id })
    assert.match(read.content, /visible_pricing_marker/)
    // an id the filtered listing cannot produce cannot open the read door either
    assert.throws(() => broker.repoRead({ file_id: 'f'.repeat(64) }), /unknown repository artifact id/)
  } finally { rmSync(sandbox, { recursive: true, force: true }) }
})

test('commit_inspect withholds ratchet memory by name from a caller with no capability', () => {
  const { sandbox, broker, base, candidate } = leakFixture()
  try {
    const inspection = broker.inspectCommit({ base_sha: base, candidate_sha: candidate })
    assert.deepEqual([...inspection.withheld_paths].sort(), [...RATCHET_MEMORY_FIXTURE_PATHS].sort(), 'withheld ratchet-memory paths must be named for audit')
    assert.deepEqual(inspection.paths, ['atx-vol/src/pricing/visible_change.cpp'])
    assert.match(inspection.diff, /visible_change_marker/)
    assert.doesNotMatch(inspection.diff, /holdout_leak_marker/, 'the diff leaked ratchet memory to a tuning-stage reviewer')
  } finally { rmSync(sandbox, { recursive: true, force: true }) }
})
