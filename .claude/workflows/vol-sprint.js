export const meta = {
  name: 'vol-sprint',
  description: 'Broker-only atx-vol improvement sprint: frozen base, scoped lanes, fresh exact-SHA review, isolated integration gate',
  whenToUse: 'Multi-lane atx-vol feature/refactor work, and the Improve stage of the oracle RSI loop. Args: { task: string, base?: string, run_key?: string }',
  phases: [
    { title: 'Freeze', detail: 'resolve the requested base through the broker to one immutable SHA' },
    { title: 'Plan', detail: 'decompose into 1-4 file-disjoint lanes with a closed workflow-owned gate closure' },
    { title: 'Build', detail: 'one broker lane per plan lane: open, patch, gate, commit, fresh review, release' },
    { title: 'Gate', detail: 'one broker integration lane: integrate the exact reviewed SHAs, run the closed gate set, release' },
  ],
}

if (!args || !args.task) throw new Error('vol-sprint needs args: { task: "<what to build>", base?: "<main|canonical|exact SHA>", run_key?: "<caller identity>" }')
const BASE_REF = (args && args.base) || 'main'

// -- Holdout confinement -----------------------------------------------------
//
// vol-sprint is a TUNING stage. It must never see the holdout cohort: not its
// membership, not its rows, not its digest, and not its scores. Three
// independent layers hold that:
//
//   1. This taint filter, applied to the caller-supplied task before any agent
//      is dispatched, so no caller can smuggle holdout identity into a lane
//      brief even when vol-sprint is invoked outside the oracle workflow.
//   2. SPRINT_GATE_REGISTRY below is a CLOSED set of broker gate IDs that omits
//      every holdout gate, every Measure gate, and the holdout digest preflight.
//      Nothing else can be derived, requested, or accepted.
//   3. The trusted broker itself refuses `holdout_mode_a`, `holdout_mode_b` and
//      `rel_avx2_speed` outside operation_id=ratchet and `measure_*` outside
//      operation_id=measure, and its repository reader never serves cohort
//      membership JSON or Parquet rows to any agent.
//
// Layer 1 fails closed on the words a leak would have to use; the two lower
// layers do not depend on any string matching at all.
const HOLDOUT_TAINT_RULES = Object.freeze([
  [/holdout/i, 'names the holdout cohort'],
  [/\bcohorts?[\\/]/i, 'names a cohort membership path'],
  [/\bmembership\b/i, 'names cohort membership'],
  [/\.(?:parquet|zip)\b/i, 'names a row-data artifact'],
  [/\b[0-9a-f]{32,}\b/, 'carries a raw digest or blob identity'],
  [/\bnbbo\b|\bcell_id\b/i, 'carries per-cell market data'],
])

function holdoutTaintError(text) {
  const value = String(text === null || text === undefined ? '' : text)
  for (const rule of HOLDOUT_TAINT_RULES) if (rule[0].test(value)) return `payload ${rule[1]}`
  return null
}

function failClosed(failure, extra = {}) {
  return {
    passed: false, blocked: [failure], failure,
    integration_branch: null, integration_sha: null, integration_tree: null,
    gate_ids: [], gate_evidence: [], lanes: [], run_id: null, base_sha: null, ...extra,
  }
}

const taskTaint = holdoutTaintError(args.task)
if (taskTaint) return failClosed(`vol-sprint task ${taskTaint}; a tuning stage may not reference holdout`)

function deterministicToken(value) {
  let hash = 0xcbf29ce484222325n
  for (const character of String(value || '')) {
    hash ^= BigInt(character.codePointAt(0))
    hash = BigInt.asUintN(64, hash * 0x100000001b3n)
  }
  return hash.toString(16).padStart(16, '0')
}

function sprintRunId(baseSha, task, runKey) {
  const identity = JSON.stringify([String(runKey || '').trim() || null, String(task || '').trim()])
  return `vol-sprint-${baseSha}-${deterministicToken(identity)}`
}

// Mirror of OPERATION_REGISTRY.sprint_build in scripts/oracle-lane-broker.mjs.
// The broker is the enforcing copy; this one exists so an out-of-scope plan is
// rejected before a lease is ever taken.
// scripts/tests/workflow-contracts.test.mjs asserts the two stay identical.
const SPRINT_SCOPE = Object.freeze({
  exact: Object.freeze(['atx-vol/CHANGELOG.md', 'atx-vol/bench/oracle/CHARTER.md']),
  prefixes: Object.freeze(['atx-vol/src/pricing/', 'atx-vol/include/atx/vol/api/pricing/', 'atx-vol/tests/']),
})

// Mirror of the sprint-reachable subset of GATE_REGISTRY in the broker, keyed by
// the exact fixed gate ID and carrying the exact display command the broker
// reports. Holdout, Measure and holdout-digest gate IDs are deliberately absent
// and the same test asserts that absence.
const SPRINT_GATE_REGISTRY = Object.freeze({
  'unit-build:atx-vol-tests': 'powershell scripts\\atx-build.ps1 -Preset dev build atx-vol-tests',
  'pch-off:atx-vol-tests': 'powershell scripts\\atx-build.ps1 -Preset hygiene build atx-vol-tests',
  'unit-test:^AmericanGreeks.Delta_MatchesFd_Put$': 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_american_greeks_delta_put',
  'unit-test:^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$': 'powershell scripts\\oracle-targeted-gate.ps1 -Gate sprint_adjusted_greeks_flat_smile',
  'oracle-test:american-rsi': 'atx-vol-oracle-bench --cohort smoke,tune --test american-rsi --aggregate-only',
  'oracle-test:american-price-rsi': 'atx-vol-oracle-bench --cohort smoke,tune --test american-price-rsi --aggregate-only',
  'oracle-test:american-greeks-rsi': 'atx-vol-oracle-bench --cohort smoke,tune --test american-greeks-rsi --aggregate-only',
  'oracle-test:american-simd-rsi': 'atx-vol-oracle-bench --cohort smoke,tune --test american-simd-rsi --aggregate-only',
  'scorecard:mode_a_smoke_tune': 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only',
  'scorecard:mode_b_smoke_tune': 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only',
  'speed:rel_avx2_quiet': 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only',
})
const FIXED_INTEGRATION_GATE_IDS = Object.freeze(['scorecard:mode_a_smoke_tune', 'scorecard:mode_b_smoke_tune', 'speed:rel_avx2_quiet'])
const UNIT_TEST_GATE_REGISTRY = Object.freeze({
  '^AmericanGreeks.Delta_MatchesFd_Put$': 'unit-test:^AmericanGreeks.Delta_MatchesFd_Put$',
  '^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$': 'unit-test:^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$',
})
const PATH_GATE_REGISTRY = Object.freeze([
  {
    owner: 'american-engine', exact_paths: ['atx-vol/src/pricing/american.cpp', 'atx-vol/include/atx/vol/api/pricing/american.hpp'], path_patterns: [],
    unit_targets: ['atx-vol-tests'], suite_prefixes: ['American', 'AndersenLake', 'Baw', 'CallGreeks', 'Alo', 'AlBulk', 'FitPreset', 'AdjointGreeks', 'AdjustedGreeks', 'NegRate', 'ExerciseBoundary', 'ResolvedAmerican', 'GaussLegendre'],
    oracle_tests: ['american-rsi', 'american-price-rsi', 'american-greeks-rsi'], pch_off_targets: ['atx-vol-tests'],
    mandatory_unit_targets: ['atx-vol-tests'], mandatory_unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], mandatory_oracle_tests: ['american-rsi'], mandatory_pch_off_targets: ['atx-vol-tests'],
  },
  {
    owner: 'american-support', exact_paths: [], path_patterns: ['^atx-vol/(?:src/pricing|include/atx/vol/api/pricing)/american_(?:batch|iv|boundary|detail)(?:\\.[^/]+)?$'],
    unit_targets: ['atx-vol-tests'], suite_prefixes: ['American', 'ResolvedAmerican', 'ExerciseBoundary'], oracle_tests: ['american-rsi', 'american-price-rsi', 'american-greeks-rsi', 'american-simd-rsi'], pch_off_targets: ['atx-vol-tests'],
    mandatory_unit_targets: ['atx-vol-tests'], mandatory_unit_regexes: ['^AmericanGreeks.Delta_MatchesFd_Put$'], mandatory_oracle_tests: ['american-rsi'], mandatory_pch_off_targets: ['atx-vol-tests'],
  },
  {
    owner: 'adjusted-greeks', exact_paths: ['atx-vol/src/pricing/adjusted_greeks.cpp', 'atx-vol/include/atx/vol/api/pricing/adjusted_greeks.hpp'], path_patterns: [],
    unit_targets: ['atx-vol-tests'], suite_prefixes: ['AdjustedGreeks', 'American'], oracle_tests: ['american-greeks-rsi'], pch_off_targets: ['atx-vol-tests'],
    mandatory_unit_targets: ['atx-vol-tests'], mandatory_unit_regexes: ['^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$'], mandatory_oracle_tests: ['american-greeks-rsi'], mandatory_pch_off_targets: ['atx-vol-tests'],
  },
  {
    owner: 'oracle-tests', exact_paths: [], path_patterns: ['^atx-vol/tests/[a-z0-9_]+\\.cpp$'],
    unit_targets: ['atx-vol-tests'], suite_prefixes: ['American', 'AdjustedGreeks', 'ResolvedAmerican', 'ExerciseBoundary'], oracle_tests: ['american-rsi'], pch_off_targets: ['atx-vol-tests'],
    mandatory_unit_targets: ['atx-vol-tests'], mandatory_unit_regexes: [], mandatory_oracle_tests: [], mandatory_pch_off_targets: [],
  },
  {
    owner: 'oracle-docs', exact_paths: ['atx-vol/changelog.md', 'atx-vol/bench/oracle/charter.md'],
    path_patterns: [], unit_targets: [], suite_prefixes: [], oracle_tests: [], pch_off_targets: [],
    mandatory_unit_targets: [], mandatory_unit_regexes: [], mandatory_oracle_tests: [], mandatory_pch_off_targets: [],
  },
])

const EVIDENCE_ITEM = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output'],
  properties: { command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
}
const BROKER_ROOT_GUARD = {
  type: 'object', additionalProperties: false,
  required: ['main_sha', 'canonical_sha', 'index_sha256', 'tracked_sha256', 'untracked_sha256', 'raw_sha256'],
  properties: {
    main_sha: { type: 'string' }, canonical_sha: { anyOf: [{ type: 'string' }, { type: 'null' }] },
    index_sha256: { type: 'string' }, tracked_sha256: { type: 'string' }, untracked_sha256: { type: 'string' }, raw_sha256: { type: 'string' },
  },
}
const BROKER_EVIDENCE = {
  type: 'object', additionalProperties: false,
  required: ['logical_operation', 'physical_cwd', 'command', 'exit_code', 'output', 'raw_output_sha256', 'root_guard_before', 'root_guard_after'],
  properties: {
    logical_operation: { type: 'string' }, physical_cwd: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' },
    output: { type: 'string' }, raw_output_sha256: { type: 'string' }, root_guard_before: BROKER_ROOT_GUARD, root_guard_after: BROKER_ROOT_GUARD,
  },
}
const LEASE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['action', 'lease_name', 'run_id', 'branch', 'base_sha', 'worktree', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc', 'exit_code', 'output'],
  properties: {
    action: { type: 'string', enum: ['acquire', 'release', 'quarantine'] }, lease_name: { type: 'string' }, run_id: { type: 'string' },
    branch: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' }, heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const BROKER_ACQUIRE = {
  type: 'object', additionalProperties: false,
  required: ['capability', 'operation_id', 'stage', 'lease_name', 'run_id', 'branch', 'base_sha', 'lease_start_sha', 'recovery_replay', 'worktree', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc', 'acquisition_receipt', 'broker_evidence'],
  properties: {
    capability: { type: 'string' }, operation_id: { type: 'string' }, stage: { type: 'string' }, lease_name: { type: 'string' }, run_id: { type: 'string' },
    branch: { type: 'string' }, base_sha: { type: 'string' }, lease_start_sha: { type: 'string' }, recovery_replay: { type: 'boolean' }, worktree: { type: 'string' },
    heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT, broker_evidence: BROKER_EVIDENCE,
  },
}
const BROKER_RELEASE = {
  type: 'object', additionalProperties: false,
  required: ['released', 'lease_name', 'sha', 'tree', 'finalize_capability', 'release_receipt', 'broker_evidence'],
  properties: {
    released: { type: 'boolean' }, lease_name: { type: 'string' }, sha: { type: 'string' }, tree: { type: 'string' }, finalize_capability: { type: 'string' },
    release_receipt: LEASE_RECEIPT, broker_evidence: BROKER_EVIDENCE,
  },
}
const FREEZE = {
  type: 'object', additionalProperties: false, required: ['ref', 'sha', 'evidence'],
  properties: { ref: { type: 'string' }, sha: { type: 'string' }, evidence: BROKER_EVIDENCE },
}
const LANE = {
  type: 'object', additionalProperties: false,
  required: ['id', 'title', 'goal', 'files_in_scope', 'gate_closure', 'done_criteria', 'out_of_scope'],
  properties: {
    id: { type: 'string' }, title: { type: 'string' }, goal: { type: 'string' },
    files_in_scope: { type: 'array', items: { type: 'string' } },
    gate_closure: { type: 'array', items: {
      type: 'object', additionalProperties: false,
      required: ['file', 'unit_targets', 'unit_regexes', 'oracle_tests', 'pch_off_targets'],
      properties: {
        file: { type: 'string' },
        unit_targets: { type: 'array', items: { type: 'string' } },
        unit_regexes: { type: 'array', items: { type: 'string' } },
        oracle_tests: { type: 'array', items: { type: 'string' } },
        pch_off_targets: { type: 'array', items: { type: 'string' } },
      },
    } },
    done_criteria: { type: 'string' }, out_of_scope: { type: 'string' },
  },
}
const PLAN = {
  type: 'object', additionalProperties: false, required: ['lanes', 'shared_files_note'],
  properties: { lanes: { type: 'array', items: LANE }, shared_files_note: { type: 'string' } },
}
const LANE_GATE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['receipt_id', 'gate_id', 'tested_sha', 'tested_tree', 'command', 'exit_code', 'output', 'broker_evidence'],
  properties: {
    receipt_id: { type: 'string' }, gate_id: { type: 'string', enum: Object.keys(SPRINT_GATE_REGISTRY) },
    tested_sha: { type: 'string' }, tested_tree: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
  },
}
const LANE_REPORT = {
  type: 'object', additionalProperties: false,
  required: ['lane_id', 'outcome', 'branch', 'sha', 'tree', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'files_changed', 'gate_receipts', 'evidence', 'broker_evidence', 'deviations'],
  properties: {
    lane_id: { type: 'string' }, outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] }, branch: { type: 'string' },
    sha: { type: 'string' }, tree: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' },
    lease_name: { type: 'string' }, lease_run_id: { type: 'string' }, heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, acquisition_receipt: LEASE_RECEIPT,
    files_changed: { type: 'array', items: { type: 'string' } },
    gate_receipts: { type: 'array', items: LANE_GATE_RECEIPT },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    broker_evidence: { type: 'array', minItems: 1, items: BROKER_EVIDENCE },
    blockers: { type: 'array', items: { type: 'string' } }, deviations: { type: 'string' },
  },
}
const FINDING = {
  type: 'object', additionalProperties: false, required: ['location', 'severity', 'problem', 'fix'],
  properties: {
    location: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
    problem: { type: 'string' }, fix: { type: 'string' },
  },
}
const REVIEW = {
  type: 'object', additionalProperties: false, required: ['verdict', 'reviewed_sha', 'evidence', 'broker_evidence', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] }, reviewed_sha: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    broker_evidence: { type: 'array', minItems: 1, items: BROKER_EVIDENCE }, findings: { type: 'array', items: FINDING },
  },
}
const INTEGRATION_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['reviewed_sha', 'reviewed_tree', 'head_after', 'tree_after', 'command', 'exit_code', 'output'],
  properties: {
    reviewed_sha: { type: 'string' }, reviewed_tree: { type: 'string' }, head_after: { type: 'string' }, tree_after: { type: 'string' },
    command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
  },
}
const HEAD_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'sha', 'command', 'exit_code', 'output'],
  properties: {
    ref: { type: 'string' }, sha: { type: 'string' }, tree: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' }, broker_evidence: BROKER_EVIDENCE,
  },
}
const GATE = {
  type: 'object', additionalProperties: false,
  required: ['passed', 'base_sha', 'lease_run_id', 'integration_branch', 'integration_sha', 'integration_tree', 'integration_worktree', 'integration_lease', 'integration_heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'integrated_shas', 'integration_receipts', 'head_receipt', 'gate_receipts', 'broker_evidence'],
  properties: {
    passed: { type: 'boolean' }, base_sha: { type: 'string' }, lease_run_id: { type: 'string' },
    integration_branch: { type: 'string' }, integration_sha: { type: 'string' }, integration_tree: { type: 'string' },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' }, integration_heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    integrated_shas: { type: 'array', items: { type: 'string' } },
    integration_receipts: { type: 'array', items: INTEGRATION_RECEIPT }, head_receipt: HEAD_RECEIPT,
    gate_receipts: { type: 'array', items: LANE_GATE_RECEIPT },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM }, broker_evidence: { type: 'array', minItems: 1, items: BROKER_EVIDENCE },
  },
}

// Verbatim copy of iterationCommandError in .claude/workflows/vol-oracle-iter.js.
// Every command this workflow publishes as evidence has to survive the oracle
// loop's own filter, so the filter lives on both sides of the boundary and
// scripts/tests/workflow-contracts.test.mjs asserts the two agree byte for byte.
function iterationCommandError(command) {
  const text = String(command || '').trim()
  if (!text) return 'empty command'
  if (/[;&|`]/.test(text)) return 'chained command forbidden in oracle loop'
  if (/\s+\((?:cwd|working directory)\s+[^)]*\)\s*$/i.test(text)) return 'annotated command forbidden in oracle loop'
  if (/(?:^|\s)-L(?:\s|=)/i.test(text) || /run_all_gates/i.test(text)) return 'label/full regression command forbidden in oracle loop'
  if (/(?:^|\s)(?:ctest|-Ctest)(?:\s|$)/i.test(text)) {
    const match = text.match(/(?:^|\s)-R\s+(\S+)/i)
    if (!match || !/^\^[A-Za-z][A-Za-z0-9_.-]{3,63}\$$/.test(match[1]) || /atx_vol/i.test(match[1])) return 'broad or unanchored ctest forbidden in oracle loop'
  }
  const tokens = text.split(/\s+/)
  for (let index = 0; index < tokens.length; index += 1) {
    if (/(?:^|[\\/])[^\\/\s]*(?:test|tests)(?:\.exe)?$/i.test(tokens[index]) && !/^-/i.test(tokens[index]) &&
        !['build', '--target'].includes(String(tokens[index - 1] || '').toLowerCase())) return 'direct test executable forbidden in oracle loop'
  }
  if (/atx-build\.ps1[^\r\n]*\bbuild(?:\s*$|\s+all(?:\s|$))/i.test(text) || /(?:--target|\bbuild\s+)\s+all(?:\s|$)/i.test(text)) return 'broad build target forbidden in oracle loop'
  if (/\bpytest\b/i.test(text) || /\bnode\s+--test\b/i.test(text) || /cmake\s+--build/i.test(text) && !/--target\s+[A-Za-z0-9_.+-]+/i.test(text)) return 'broad runner/build forbidden in oracle loop'
  return null
}

function validSuccessEvidence(evidence) {
  return Array.isArray(evidence) && evidence.length > 0 && evidence.every(item => item && typeof item.command === 'string' && item.command.trim() &&
    !iterationCommandError(item.command) && item.exit_code === 0 && typeof item.output === 'string' && item.output.trim())
}

function diagnosticsUseForbiddenCommand(diagnostics) {
  return Array.isArray(diagnostics) && diagnostics.some(item => iterationCommandError(item && item.command))
}

function validLeaseReceipt(receipt, expected, action) {
  if (!receipt || receipt.action !== action || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return false
  if (receipt.lease_name !== expected.lease_name || receipt.run_id !== expected.run_id || receipt.branch !== expected.branch || receipt.base_sha !== expected.base_sha ||
      receipt.worktree !== expected.worktree || receipt.heartbeat_id !== expected.heartbeat_id || receipt.keeper_pid !== expected.keeper_pid ||
      receipt.keeper_process_started_utc !== expected.keeper_process_started_utc) return false
  if (!Number.isInteger(receipt.keeper_pid) || receipt.keeper_pid <= 0 || !/^\d{4}-/.test(receipt.keeper_process_started_utc || '') || !/^\d{4}-/.test(receipt.keeper_ready_utc || '')) return false
  return receipt.output.includes(receipt.lease_name) && receipt.output.includes(receipt.run_id) &&
    (action !== 'acquire' || (receipt.output.includes(String(receipt.keeper_pid)) && receipt.output.includes(receipt.keeper_ready_utc) &&
      receipt.output.includes(` heartbeat_id=${receipt.heartbeat_id}`)))
}

function validBrokerEvidence(receipt, logicalOperation = null, physicalCwd = null) {
  if (!receipt || !Number.isInteger(receipt.exit_code) || receipt.exit_code !== 0 || typeof receipt.command !== 'string' || !receipt.command.trim() ||
      typeof receipt.output !== 'string' || !/^[0-9a-f]{64}$/.test(receipt.raw_output_sha256 || '') ||
      (logicalOperation && receipt.logical_operation !== logicalOperation)) return false
  if (physicalCwd && String(receipt.physical_cwd || '').replace(/\//g, '\\').toLowerCase() !== String(physicalCwd).replace(/\//g, '\\').toLowerCase()) return false
  const before = receipt.root_guard_before; const after = receipt.root_guard_after
  if (!before || !after || !/^[0-9a-f]{40}$/.test(before.main_sha || '') || before.main_sha !== after.main_sha) return false
  for (const key of ['index_sha256', 'tracked_sha256', 'untracked_sha256', 'raw_sha256']) if (!/^[0-9a-f]{64}$/.test(before[key] || '') || !/^[0-9a-f]{64}$/.test(after[key] || '')) return false
  for (const key of ['index_sha256', 'tracked_sha256', 'untracked_sha256']) if (before[key] !== after[key]) return false
  // A sprint lane may never move the oracle canonical ref. Only the oracle
  // workflow's Ratchet transaction may, and only through its own audited CAS.
  return before.canonical_sha === after.canonical_sha
}

function brokerAcquireError(acquire, expected) {
  if (!acquire || !/^[0-9a-f]{64}$/.test(acquire.capability || '') || acquire.operation_id !== expected.operation_id || acquire.stage !== expected.stage ||
      acquire.run_id !== expected.run_id || acquire.branch !== expected.branch || acquire.base_sha !== expected.base_sha ||
      acquire.heartbeat_id !== expected.heartbeat_id) return 'broker acquisition identity invalid'
  if (acquire.recovery_replay !== false || !/^[0-9a-f]{40}$/.test(acquire.lease_start_sha || '') || acquire.lease_start_sha !== acquire.base_sha) return 'broker acquisition replay identity invalid'
  if (!/^pool-[0-9]+$/.test(acquire.lease_name || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(acquire.worktree || '') ||
      !acquire.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${acquire.lease_name.toLowerCase()}`)) return 'broker acquisition path invalid'
  if (!validLeaseReceipt(acquire.acquisition_receipt, { ...expected, lease_name: acquire.lease_name, worktree: acquire.worktree, keeper_pid: acquire.keeper_pid, keeper_process_started_utc: acquire.keeper_process_started_utc }, 'acquire')) return 'broker acquisition receipt invalid'
  return validBrokerEvidence(acquire.broker_evidence, 'lane_open') ? null : 'broker acquisition root evidence invalid'
}

function brokerReleaseError(release, acquire) {
  if (!release || !release.released || release.lease_name !== acquire.lease_name || !/^[0-9a-f]{40}$/.test(release.sha || '') ||
      !/^[0-9a-f]{40}$/.test(release.tree || '')) return 'broker release identity invalid'
  // sprint_build and sprint_integration both carry finalize:false in the broker
  // operation registry, so a finalize capability here would mean a tuning lane
  // had been handed the ability to move the canonical ref. Refuse it.
  if (release.finalize_capability !== '') return 'sprint lane must not be issued a canonical finalize capability'
  if (!validLeaseReceipt(release.release_receipt, acquire, 'release')) return 'broker release receipt invalid'
  return validBrokerEvidence(release.broker_evidence, 'lane_release') ? null : 'broker release root evidence invalid'
}

function scopePathError(path) {
  const rel = String(path || '')
  if (!rel || rel !== rel.trim() || rel.includes('\\') || rel.startsWith('/') || rel.includes('..') || !/^[A-Za-z0-9._/-]+$/.test(rel)) return `unsafe repository path: ${path}`
  if (SPRINT_SCOPE.exact.includes(rel) || SPRINT_SCOPE.prefixes.some(prefix => rel.startsWith(prefix))) return null
  return `path is outside the sprint build scope: ${rel}`
}

function changedHeader(path) { return /\.(?:h|hh|hpp|hxx|inl)$/i.test(String(path || '')) }
function changedCode(path) { return /\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|inl)$/i.test(String(path || '')) }
function safeTarget(value) { return /^[A-Za-z0-9][A-Za-z0-9_.+-]{1,63}$/.test(String(value || '')) && String(value).toLowerCase() !== 'all' }
function safeUnitRegex(value) { return /^\^[A-Za-z][A-Za-z0-9_.-]{3,63}\$$/.test(String(value || '')) && !/atx_vol/i.test(String(value || '')) }
function safeOracleTest(value) { return /^[a-z0-9][a-z0-9_-]{2,63}$/.test(String(value || '')) }

function pathGateOwner(path) {
  const normalized = String(path || '').replace(/\\/g, '/').toLowerCase()
  return PATH_GATE_REGISTRY.find(owner => (owner.exact_paths || []).includes(normalized) ||
    (owner.path_patterns || []).some(pattern => new RegExp(pattern).test(normalized))) || null
}

function regexSuiteName(value) {
  const body = String(value || '').slice(1, -1)
  return body.includes('.') ? body.slice(0, body.indexOf('.')) : body
}

function closureError(lane) {
  if (!lane || !Array.isArray(lane.files_in_scope) || !lane.files_in_scope.length || !Array.isArray(lane.gate_closure)) return 'lane closure missing'
  const scopeError = lane.files_in_scope.map(scopePathError).find(Boolean)
  if (scopeError) return scopeError
  const normalizedScope = lane.files_in_scope.map(path => path.toLowerCase())
  const normalizedClosure = lane.gate_closure.map(item => item && String(item.file || '').replace(/\\/g, '/').toLowerCase())
  if (new Set(normalizedScope).size !== normalizedScope.length || new Set(normalizedClosure).size !== normalizedClosure.length ||
      normalizedScope.length !== normalizedClosure.length || !normalizedScope.every(path => normalizedClosure.includes(path))) return 'gate closure must map every scoped file exactly once'
  let oracleTests = 0
  for (const item of lane.gate_closure) {
    if (!item || !['unit_targets', 'unit_regexes', 'oracle_tests', 'pch_off_targets'].every(key => Array.isArray(item[key]))) return 'gate closure arrays missing'
    if (!item.unit_targets.every(safeTarget) || !item.unit_regexes.every(safeUnitRegex) || !item.oracle_tests.every(safeOracleTest) || !item.pch_off_targets.every(safeTarget)) return 'gate closure identifier invalid/broad'
    const owner = pathGateOwner(item.file)
    if (!owner) return `gate closure path is not in workflow registry: ${item.file}`
    if (!item.unit_targets.every(target => owner.unit_targets.includes(target)) || !item.pch_off_targets.every(target => owner.pch_off_targets.includes(target)) ||
        !item.oracle_tests.every(test => owner.oracle_tests.includes(test)) ||
        !item.unit_regexes.every(regex => owner.suite_prefixes.some(prefix => regexSuiteName(regex).startsWith(prefix)) && UNIT_TEST_GATE_REGISTRY[regex])) return `gate closure does not match workflow path owner: ${item.file}`
    if (!owner.mandatory_unit_targets.every(target => item.unit_targets.includes(target)) || !owner.mandatory_unit_regexes.every(regex => item.unit_regexes.includes(regex)) ||
        !owner.mandatory_oracle_tests.every(test => item.oracle_tests.includes(test)) ||
        changedHeader(item.file) && !owner.mandatory_pch_off_targets.every(target => item.pch_off_targets.includes(target))) return `gate closure omitted mandatory workflow gates for: ${item.file}`
    if (changedCode(item.file) && (!item.unit_targets.length || !item.unit_regexes.length)) return 'changed code requires affected unit target and anchored unit regex'
    if (changedHeader(item.file) && !item.pch_off_targets.length) return 'changed header requires scoped PCH-off target'
    if (!changedCode(item.file) && (item.unit_targets.length || item.unit_regexes.length || item.oracle_tests.length || item.pch_off_targets.length)) return 'non-code path cannot assert code gates'
    oracleTests += item.oracle_tests.length
  }
  return oracleTests > 0 ? null : 'lane requires a hypothesis-specific OracleBench test'
}

function closureEntries(lane, files) {
  const wanted = new Set(files.map(path => String(path).replace(/\\/g, '/').toLowerCase()))
  return lane.gate_closure.filter(item => wanted.has(String(item.file).replace(/\\/g, '/').toLowerCase()))
}

// Derives the CLOSED broker gate-ID set a changed-file closure requires. Every
// candidate ID is looked up in SPRINT_GATE_REGISTRY before it is admitted, so an
// ID outside the closed sprint set cannot be constructed even from a plan that
// is trying to reach one.
function derivedGateIds(entries, includeFixed = false) {
  const ids = []
  const add = id => { if (Object.prototype.hasOwnProperty.call(SPRINT_GATE_REGISTRY, id) && !ids.includes(id)) ids.push(id) }
  for (const entry of entries) {
    for (const target of entry.unit_targets) add(`unit-build:${target}`)
    for (const regex of entry.unit_regexes) add(UNIT_TEST_GATE_REGISTRY[regex] || '')
    for (const test of entry.oracle_tests) add(`oracle-test:${test}`)
    if (changedHeader(entry.file)) for (const target of entry.pch_off_targets) add(`pch-off:${target}`)
  }
  if (includeFixed) for (const id of FIXED_INTEGRATION_GATE_IDS) add(id)
  return ids.sort((left, right) => left.localeCompare(right))
}

function gateReceiptError(receipt, gateId, expected) {
  if (!receipt || receipt.gate_id !== gateId) return `gate receipt missing: ${gateId}`
  if (!Object.prototype.hasOwnProperty.call(SPRINT_GATE_REGISTRY, gateId)) return `gate is outside the closed sprint registry: ${gateId}`
  if (receipt.command !== SPRINT_GATE_REGISTRY[gateId]) return `gate command is not the broker-registered command: ${gateId}`
  if (iterationCommandError(receipt.command) || holdoutTaintError(receipt.command)) return `gate command is forbidden in the oracle loop: ${gateId}`
  if (receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return `gate did not pass with pasted output: ${gateId}`
  if (!/^[0-9a-f]{64}$/.test(receipt.receipt_id || '') || receipt.tested_sha !== expected.sha || receipt.tested_tree !== expected.tree) return `gate receipt is not bound to the tested SHA/tree: ${gateId}`
  // This workflow does not recompute the broker's receipt digest; what it does
  // bind is every field that digest is taken over plus the broker's own
  // canonical-root guard, so a fabricated receipt cannot agree with the broker
  // evidence it is required to carry.
  if (!validBrokerEvidence(receipt.broker_evidence, `gate:${gateId}`, expected.worktree) || receipt.broker_evidence.command !== receipt.command ||
      receipt.broker_evidence.output !== receipt.output || receipt.broker_evidence.exit_code !== receipt.exit_code) return `gate broker evidence is not bound to the receipt: ${gateId}`
  return null
}

function gateReceiptSetError(receipts, gateIds, expected) {
  if (!Array.isArray(receipts) || receipts.length !== gateIds.length) return 'gate receipt set does not equal the derived closure'
  if (new Set(receipts.map(receipt => receipt && receipt.receipt_id)).size !== gateIds.length) return 'gate receipt IDs are duplicated'
  for (const gateId of gateIds) {
    const matches = receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1) return `gate receipt set does not contain exactly one ${gateId}`
    const error = gateReceiptError(matches[0], gateId, expected)
    if (error) return error
  }
  return null
}

function laneReportError(report, lane, acquire, expected) {
  if (!report || typeof report !== 'object' || Array.isArray(report)) return `${lane.id}: lane worker returned no typed report`
  if (report.outcome !== 'DONE') return `${lane.id}: ${report.outcome === 'BLOCKED' ? `blocked: ${(report.blockers || []).join('; ') || 'no blocker stated'}` : 'lane incomplete'}`
  if (report.lane_id !== lane.id || report.branch !== expected.branch || report.base_sha !== expected.base_sha ||
      report.lease_run_id !== expected.run_id || report.heartbeat_id !== expected.heartbeat_id) return `${lane.id}: lane identity mismatch`
  if (report.lease_name !== acquire.lease_name || report.worktree !== acquire.worktree || report.keeper_pid !== acquire.keeper_pid ||
      report.keeper_process_started_utc !== acquire.keeper_process_started_utc) return `${lane.id}: report is not the workflow-owned lane`
  if (!validLeaseReceipt(report.acquisition_receipt, { ...expected, lease_name: acquire.lease_name, worktree: acquire.worktree, keeper_pid: acquire.keeper_pid, keeper_process_started_utc: acquire.keeper_process_started_utc }, 'acquire')) return `${lane.id}: lane acquisition receipt invalid`
  if (!/^[0-9a-f]{40}$/.test(report.sha || '') || !/^[0-9a-f]{40}$/.test(report.tree || '') || report.sha === expected.base_sha) return `${lane.id}: lane did not commit an exact new SHA/tree`
  if (typeof report.deviations !== 'string') return `${lane.id}: lane deviations missing`
  if (!Array.isArray(report.broker_evidence) || !report.broker_evidence.length ||
      !report.broker_evidence.every(item => validBrokerEvidence(item, null, report.worktree))) return `${lane.id}: lane broker evidence invalid`
  if (!Array.isArray(report.files_changed) || !report.files_changed.length || new Set(report.files_changed).size !== report.files_changed.length) return `${lane.id}: changed-file report missing/duplicated`
  const outOfScope = report.files_changed.find(path => !lane.files_in_scope.includes(path))
  if (outOfScope) return `${lane.id}: changed file outside the leased lane scope: ${outOfScope}`
  const requiredGateIds = derivedGateIds(closureEntries(lane, report.files_changed))
  if (!requiredGateIds.length) return `${lane.id}: changed dependency closure produced no targeted gates`
  const gateError = gateReceiptSetError(report.gate_receipts, requiredGateIds, { sha: report.sha, tree: report.tree, worktree: report.worktree })
  if (gateError) return `${lane.id}: ${gateError}`
  if (!validSuccessEvidence(report.evidence) || diagnosticsUseForbiddenCommand(report.diagnostics)) return `${lane.id}: lane evidence missing/failed/forbidden`
  const evidenceCommands = report.evidence.map(item => item.command).sort((left, right) => left.localeCompare(right))
  const receiptCommands = report.gate_receipts.map(receipt => receipt.command).sort((left, right) => left.localeCompare(right))
  if (evidenceCommands.length !== receiptCommands.length || evidenceCommands.some((command, index) => command !== receiptCommands[index])) return `${lane.id}: lane evidence is not exactly the broker gate receipt set`
  return null
}

function reviewContractError(review, expectedSha) {
  if (!review) return 'review agent returned no result'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== expectedSha) return `review is stale: ${review.reviewed_sha}`
  if (!validSuccessEvidence(review.evidence) || diagnosticsUseForbiddenCommand(review.diagnostics) || !Array.isArray(review.broker_evidence) ||
      !review.broker_evidence.length || !review.broker_evidence.every(item => validBrokerEvidence(item))) return 'review has invalid/broad broker evidence'
  const blockers = review.findings.filter(finding => finding.severity === 'blocker')
  if (review.verdict === 'APPROVE' && blockers.length) return 'APPROVE contains blocker'
  if (review.verdict === 'BLOCK' && !blockers.length) return 'BLOCK lacks blocker'
  return null
}

function validIntegrationCommand(receipt, reviewedSha, reviewedTree) {
  return !!receipt && receipt.reviewed_sha === reviewedSha && receipt.reviewed_tree === reviewedTree && receipt.exit_code === 0 &&
    new RegExp(`^git\\s+merge(?:\\s+--(?:ff-only|no-ff|no-edit))*\\s+${reviewedSha}$`, 'i').test(String(receipt.command || '').trim())
}

function validHeadReceipt(receipt, sha, tree) {
  return !!receipt && receipt.ref === 'HEAD' && receipt.sha === sha && receipt.tree === tree && receipt.exit_code === 0 &&
    receipt.command.trim() === 'git rev-parse HEAD' && receipt.output.trim() === sha
}

function gateContractError(gate, acquire, expected) {
  if (!gate || typeof gate !== 'object' || Array.isArray(gate)) return 'integration gate returned no typed result'
  if (!gate.passed) return 'integration gate did not pass'
  if (gate.base_sha !== expected.base_sha || gate.lease_run_id !== expected.run_id || gate.integration_branch !== expected.branch ||
      gate.integration_heartbeat_id !== expected.heartbeat_id) return 'integration identity mismatch'
  if (gate.integration_lease !== acquire.lease_name || gate.integration_worktree !== acquire.worktree ||
      gate.keeper_pid !== acquire.keeper_pid || gate.keeper_process_started_utc !== acquire.keeper_process_started_utc) return 'integration gate is not the workflow-owned lane'
  if (!/^[0-9a-f]{40}$/.test(gate.integration_sha || '') || !/^[0-9a-f]{40}$/.test(gate.integration_tree || '')) return 'integration SHA/tree is not exact'
  if (!Array.isArray(gate.broker_evidence) || !gate.broker_evidence.length ||
      !gate.broker_evidence.every(item => validBrokerEvidence(item, null, gate.integration_worktree))) return 'integration broker evidence invalid'
  if (diagnosticsUseForbiddenCommand(gate.diagnostics)) return 'integration diagnostics used a forbidden command'
  if (!Array.isArray(gate.integrated_shas) || gate.integrated_shas.length !== expected.reviewed.length ||
      gate.integrated_shas.some((sha, index) => sha !== expected.reviewed[index].sha)) return 'integrated SHA list is not the reviewed list in order'
  if (!Array.isArray(gate.integration_receipts) || gate.integration_receipts.length !== expected.reviewed.length) return 'integration receipts missing'
  for (let index = 0; index < expected.reviewed.length; index += 1) {
    const receipt = gate.integration_receipts[index]
    if (!validIntegrationCommand(receipt, expected.reviewed[index].sha, expected.reviewed[index].tree) ||
        !/^[0-9a-f]{40}$/.test(receipt.head_after || '') || !String(receipt.output || '').includes(receipt.head_after)) return 'exact reviewed SHA integration receipt invalid'
  }
  if (gate.integration_receipts[gate.integration_receipts.length - 1].head_after !== gate.integration_sha) return 'final integration receipt does not carry the sealed integration SHA'
  if (!validHeadReceipt(gate.head_receipt, gate.integration_sha, gate.integration_tree)) return 'integration HEAD/tree receipt invalid'
  return gateReceiptSetError(gate.gate_receipts, expected.gate_ids, { sha: gate.integration_sha, tree: gate.integration_tree, worktree: gate.integration_worktree })
}

async function releaseLane(capability, acquire, label) {
  let release = null
  let thrown = null
  try {
    release = await agent(`Call broker lane_release exactly once with capability=${capability}; return the result unchanged.`,
      { agentType: 'vol-lane-releaser', schema: BROKER_RELEASE, label })
  } catch (error) { thrown = String(error) }
  return { release, error: thrown || brokerReleaseError(release, acquire) }
}

phase('Freeze')
let freeze = null
let freezeThrown = null
try {
  freeze = await agent(`Call broker ref_resolve exactly once with ${JSON.stringify({ ref_id: BASE_REF })}; return the typed result unchanged. Do not open a lane, patch, gate, or finalize.`,
    { agentType: 'vol-ref-auditor', schema: FREEZE, label: 'freeze-base' })
} catch (error) { freezeThrown = String(error) }
if (freezeThrown || !freeze || !/^[0-9a-f]{40}$/.test(freeze.sha || '') || !validBrokerEvidence(freeze.evidence, 'ref_resolve') ||
    freeze.evidence.output.trim() !== freeze.sha) return failClosed(`vol-sprint could not freeze ${BASE_REF} through the broker: ${freezeThrown || 'invalid typed resolve receipt'}`)
const BASE_SHA = freeze.sha
const RUN_ID = sprintRunId(BASE_SHA, args.task, args.run_key)
const RUN_SLUG = RUN_ID
const INTEGRATION_BRANCH = `integration/oracle-improve-${RUN_SLUG}`
const INTEGRATION_HEARTBEAT = `${RUN_SLUG}-integration`

phase('Plan')
let plan = null
let planThrown = null
try {
  plan = await agent(
    `Task: ${args.task}\nFrozen base SHA: ${BASE_SHA}\nRun id: ${RUN_ID}\n\nRead only through broker repo_search/repo_read. Decompose into 1-4 file-disjoint MANDATORY lanes. Every files_in_scope path must be repository-relative with forward slashes and inside the fixed sprint build scope ${JSON.stringify(SPRINT_SCOPE)}; nothing else can be leased. For EVERY files_in_scope path return exactly one gate_closure entry. Changed C/C++ files require affected unit targets plus anchored non-broad unit regexes; every lane needs at least one hypothesis-specific OracleBench test ID; headers require the owning scoped PCH-off target. Reserve a shared file for exactly one lane. This is a TUNING stage scored on the smoke and tune cohorts only: never plan, request, name, or quote reserved evaluation data, cohort files, row artifacts, or digests of any kind. Keep every field to plain prose about source files and gates.`,
    { agentType: 'vol-planner', schema: PLAN, label: 'plan' },
  )
} catch (error) { planThrown = String(error) }
if (planThrown || !plan || !Array.isArray(plan.lanes) || plan.lanes.length < 1 || plan.lanes.length > 4) return failClosed(`vol-sprint planner must return 1-4 mandatory lanes: ${planThrown || 'invalid plan'}`)
// The plan is agent-authored text that becomes a lane brief, so it gets the same
// filter the task did. The planner prompt above deliberately does not name the
// reserved cohort, so a plan that names it did not learn it from this workflow.
const planTaint = holdoutTaintError(JSON.stringify(plan))
if (planTaint) return failClosed(`vol-sprint plan ${planTaint}; a tuning stage may not reference holdout`)
const lanes = plan.lanes.map(lane => ({ ...lane, branch: `lane/oracle-improve-${String(lane.id).replace(/[^A-Za-z0-9._-]/g, '-')}-${RUN_SLUG}` }))
const planErrors = lanes.map(lane => closureError(lane)).filter(Boolean)
if (planErrors.length) return failClosed(`vol-sprint plan closure invalid: ${planErrors.join('; ')}`)
if (new Set(lanes.map(lane => lane.id)).size !== lanes.length || new Set(lanes.map(lane => lane.branch)).size !== lanes.length) return failClosed('vol-sprint lane ids/branches must be unique')
const claimed = lanes.flatMap(lane => lane.files_in_scope)
if (new Set(claimed).size !== claimed.length) return failClosed('vol-sprint lanes are not file-disjoint')

phase('Build')
const completed = []
const failures = []
for (const lane of lanes) {
  if (failures.length) break
  const heartbeat = `${RUN_SLUG}-lane-${String(lane.id).replace(/[^A-Za-z0-9._-]/g, '-')}`
  const expected = { operation_id: 'sprint_build', stage: 'improve', run_id: RUN_ID, branch: lane.branch, base_sha: BASE_SHA, heartbeat_id: heartbeat }
  let acquire = null
  let acquireThrown = null
  try {
    acquire = await agent(`Call broker lane_open exactly once with ${JSON.stringify({ ...expected, scope_paths: lane.files_in_scope })}; return the broker result unchanged.`,
      { agentType: 'vol-lane-opener', schema: BROKER_ACQUIRE, label: `lane-acquire:${lane.id}` })
  } catch (error) { acquireThrown = String(error) }
  const acquireError = acquireThrown || brokerAcquireError(acquire, expected)
  if (acquireError) { failures.push(`${lane.id}: ${acquireError}`); break }
  const requiredGateIds = derivedGateIds(lane.gate_closure)
  const brief = `ONE preleased broker lane; no planner, no lease, no release. Immutable acquisition: ${JSON.stringify(acquire)}. Use only capability=${acquire.capability}. Lane brief:\n${JSON.stringify(lane, null, 2)}\nFrozen base SHA ${BASE_SHA}. Read with repo_search/repo_read, then change source only with patch_apply and only inside files_in_scope. ORDER MATTERS: lane_commit with message_id=sprint_lane FIRST, then run ONLY these fixed broker gate IDs, each exactly once: ${JSON.stringify(requiredGateIds)}. The broker stamps every gate receipt with the lane HEAD it ran against, and this workflow requires that to be the committed SHA/tree, so a gate run before the commit is rejected. Requesting any gate ID outside that list is refused by the broker and fails this workflow closed. Return the typed report with lease identity copied from the acquisition, files_changed exactly as lane_commit returned them, every broker gate receipt unchanged (receipt_id, tested_sha, tested_tree, command, exit_code, output, broker_evidence), evidence containing exactly one command/exit/output triple per gate receipt, and every broker evidence object under broker_evidence.`
  let report = null
  let buildThrown = null
  try { report = await agent(brief, { agentType: 'vol-builder', schema: LANE_REPORT, label: `build:${lane.id}` }) } catch (error) { buildThrown = String(error) }
  let laneError = buildThrown ? `${lane.id}: build failed: ${buildThrown}` : laneReportError(report, lane, acquire, expected)
  let review = null
  if (!laneError) {
    phase('Review')
    try {
      review = await agent(`Fresh exact-SHA broker review of lane ${lane.id}. Call commit_inspect with base_sha=${BASE_SHA}, candidate_sha=${report.sha} and capability=${acquire.capability}. Brief:\n${JSON.stringify(lane, null, 2)}\nVerify the diff implements the lane goal, changes nothing outside files_in_scope, and adds no flag, shim, or bypass. This is a TUNING lane: the broker withholds ratchet-memory paths from your diff and from repo_search/repo_read, and you must not try to reach them through any other commit range. Return the verdict for exactly ${report.sha} with command evidence and every broker evidence object unchanged.`,
        { agentType: 'vol-reviewer', schema: REVIEW, label: `review:${lane.id}` })
      const reviewError = reviewContractError(review, report.sha)
      if (reviewError) laneError = `${lane.id}: ${reviewError}`
    } catch (error) { laneError = `${lane.id}: review failed: ${String(error)}` }
  }
  if (!laneError && review.verdict === 'BLOCK') {
    phase('Fix')
    try {
      report = await agent(`Fix exactly blockers ${JSON.stringify(review.findings.filter(finding => finding.severity === 'blocker'))} using only workflow-held capability=${acquire.capability}; never acquire, release, or widen scope. patch_apply, then lane_commit with message_id=sprint_lane, then rerun only gate IDs ${JSON.stringify(requiredGateIds)} against that new commit, and return a NEW typed report with the new SHA/tree and fresh broker gate receipts bound to it.`,
        { agentType: 'vol-builder', schema: LANE_REPORT, label: `fix:${lane.id}` })
      laneError = laneReportError(report, lane, acquire, expected)
    } catch (error) { laneError = `${lane.id}: fix failed: ${String(error)}` }
    if (!laneError) {
      phase('Re-review')
      try {
        review = await agent(`FRESH post-Fix broker re-review of lane ${lane.id}; never reuse the prior verdict. Call commit_inspect with base_sha=${BASE_SHA} and candidate_sha=${report.sha} and return a verdict for exactly ${report.sha}.`,
          { agentType: 'vol-reviewer', schema: REVIEW, label: `rereview:${lane.id}` })
        const reReviewError = reviewContractError(review, report.sha)
        if (reReviewError) laneError = `${lane.id}: ${reReviewError}`
      } catch (error) { laneError = `${lane.id}: re-review failed: ${String(error)}` }
    }
  }
  if (!laneError && review.verdict !== 'APPROVE') laneError = `${lane.id}: final review is not APPROVE`
  const released = await releaseLane(acquire.capability, acquire, `lane-release:${lane.id}`)
  if (!laneError && released.error) laneError = `${lane.id}: ${released.error}`
  if (!laneError && (released.release.sha !== report.sha || released.release.tree !== report.tree)) laneError = `${lane.id}: released lane SHA/tree differs from the reviewed report`
  if (laneError) { failures.push(laneError); break }
  completed.push({ lane, report, review, release: released.release })
}
if (failures.length) {
  return failClosed(`vol-sprint lane failed before integration: ${failures.join('; ')}`,
    { run_id: RUN_ID, base_sha: BASE_SHA, lanes: completed.map(item => ({ lane: item.lane.id, sha: item.report.sha, verdict: item.review.verdict })) })
}

phase('Gate')
const REQUIRED_GATE_IDS = derivedGateIds(completed.flatMap(item => closureEntries(item.lane, item.report.files_changed)), true)
if (!REQUIRED_GATE_IDS.some(id => id.startsWith('unit-test:')) || !REQUIRED_GATE_IDS.some(id => id.startsWith('oracle-test:'))) {
  return failClosed('vol-sprint changed closure omitted affected unit or hypothesis-specific OracleBench gates', { run_id: RUN_ID, base_sha: BASE_SHA })
}
const integrationExpected = { operation_id: 'sprint_integration', stage: 'improve-gate', run_id: RUN_ID, branch: INTEGRATION_BRANCH, base_sha: BASE_SHA, heartbeat_id: INTEGRATION_HEARTBEAT }
let integrationAcquire = null
let integrationAcquireThrown = null
try {
  integrationAcquire = await agent(`Call broker lane_open exactly once with ${JSON.stringify(integrationExpected)}; return the result unchanged.`,
    { agentType: 'vol-lane-opener', schema: BROKER_ACQUIRE, label: 'integration-acquire' })
} catch (error) { integrationAcquireThrown = String(error) }
const integrationAcquireError = integrationAcquireThrown || brokerAcquireError(integrationAcquire, integrationExpected)
if (integrationAcquireError) return failClosed(`vol-sprint integration lane unavailable: ${integrationAcquireError}`, { run_id: RUN_ID, base_sha: BASE_SHA })

const reviewed = completed.map(item => ({ sha: item.report.sha, tree: item.report.tree }))
let gate = null
let gateThrown = null
try {
  gate = await agent(
    `Use only preleased capability=${integrationAcquire.capability}. Call lane_integrate exactly once with reviewed_shas=${JSON.stringify(reviewed.map(item => item.sha))} in that order, relay every integration receipt and the HEAD receipt unchanged, then run each of these fixed broker gate IDs exactly once against the sealed integrated SHA/tree: ${JSON.stringify(REQUIRED_GATE_IDS)}. Requesting any gate ID outside that list is refused by the broker and fails this workflow closed. You cannot patch, commit, acquire, release, or finalize. Return identities copied from ${JSON.stringify(integrationAcquire)}, base_sha=${BASE_SHA}, integration_branch=${INTEGRATION_BRANCH}, the sealed integration SHA and tree, every gate receipt unchanged, and all broker evidence.`,
    { agentType: 'vol-verifier', schema: GATE, label: 'integration-gate' },
  )
} catch (error) { gateThrown = String(error) }
const integrationRelease = await releaseLane(integrationAcquire.capability, integrationAcquire, 'integration-release')
const gateError = gateThrown ||
  gateContractError(gate, integrationAcquire, { base_sha: BASE_SHA, run_id: RUN_ID, branch: INTEGRATION_BRANCH, heartbeat_id: INTEGRATION_HEARTBEAT, reviewed, gate_ids: REQUIRED_GATE_IDS }) ||
  integrationRelease.error ||
  (integrationRelease.release.sha !== gate.integration_sha || integrationRelease.release.tree !== gate.integration_tree ? 'integration release differs from the sealed integration SHA/tree' : null)
if (gateError) {
  return failClosed(`vol-sprint integration gate failed: ${gateError}`,
    { run_id: RUN_ID, base_sha: BASE_SHA, lanes: completed.map(item => ({ lane: item.lane.id, sha: item.report.sha, verdict: item.review.verdict })) })
}

const gateEvidence = gate.gate_receipts.map(receipt => ({ command: receipt.command, exit_code: receipt.exit_code, output: receipt.output }))
if (!validSuccessEvidence(gateEvidence)) return failClosed('vol-sprint gate evidence does not satisfy the oracle-loop command filter', { run_id: RUN_ID, base_sha: BASE_SHA })

return {
  passed: true, blocked: [], failure: null,
  integration_branch: INTEGRATION_BRANCH, integration_sha: gate.integration_sha, integration_tree: gate.integration_tree,
  gate_ids: REQUIRED_GATE_IDS, gate_evidence: gateEvidence,
  reviewed_lane_shas: reviewed.map(item => item.sha),
  lanes: completed.map(item => ({ lane: item.lane.id, branch: item.lane.branch, sha: item.report.sha, verdict: item.review.verdict, files_changed: item.report.files_changed })),
  run_id: RUN_ID, base_sha: BASE_SHA,
}
