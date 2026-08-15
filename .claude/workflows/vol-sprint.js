export const meta = {
  name: 'vol-sprint',
  description: 'atx-vol DAG: frozen-base plan, isolated lanes, fresh review/fix/re-review, isolated integration gate',
  whenToUse: 'Multi-lane atx-vol feature/refactor work. Args: { task: string, base?: string, run_key?: string }',
  phases: [
    { title: 'Freeze', detail: 'resolve the requested base ref to one immutable SHA' },
    { title: 'Plan', detail: 'decompose task into 1-4 disjoint mandatory lanes' },
    { title: 'Build', detail: 'one builder per lane in a run-owned pool lease' },
    { title: 'Review', detail: 'fresh-context review; every fix is reviewed again' },
    { title: 'Release', detail: 'release all approved lane leases before integration' },
    { title: 'Gate', detail: 'new isolated integration lease, gates, ledger, release' },
  ],
}

if (!args || !args.task) throw new Error('vol-sprint needs args: { task: "<what to build>", base?: "<ref, default main>", run_key?: "<resume-stable caller identity>" }')
const BASE_REF = (args && args.base) || 'main'

function deterministicToken(value) {
  let hash = 0xcbf29ce484222325n
  for (const character of String(value || '')) {
    hash ^= BigInt(character.codePointAt(0))
    hash = BigInt.asUintN(64, hash * 0x100000001b3n)
  }
  return hash.toString(16).padStart(16, '0')
}

function sprintRunId(baseSha, task, runKey) {
  const caller = String(runKey || '').trim()
  const identity = caller ? `caller:${caller}` : `task:${task}`
  return `vol-sprint-${baseSha}-${deterministicToken(identity)}`
}
const FIXED_ORACLE_GATES = Object.freeze([
  { gate_id: 'scorecard:mode_a_smoke_tune', command: 'atx-vol-oracle-bench --cohort smoke,tune --mode A --scorecard --aggregate-only' },
  { gate_id: 'scorecard:mode_b_smoke_tune', command: 'atx-vol-oracle-bench --cohort smoke,tune --mode B --scorecard --aggregate-only' },
  { gate_id: 'speed:rel_avx2_quiet', command: 'atx-vol-oracle-bench --cohort tune --benchmark-speed --preset rel-avx2 --quiet-host --aggregate-only' },
])
const UNIT_TEST_GATE_REGISTRY = Object.freeze({
  '^AmericanGreeks.Delta_MatchesFd_Put$': { adapter_gate: 'sprint_american_greeks_delta_put', source: 'atx-vol/tests/american_test.cpp' },
  '^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$': { adapter_gate: 'sprint_adjusted_greeks_flat_smile', source: 'atx-vol/tests/adjusted_greeks_test.cpp' },
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
    owner: 'oracle-docs', exact_paths: ['atx-vol/changelog.md', 'atx-vol/bench/oracle/charter.md'],
    path_patterns: [], unit_targets: [], suite_prefixes: [], oracle_tests: [], pch_off_targets: [],
    mandatory_unit_targets: [], mandatory_unit_regexes: [], mandatory_oracle_tests: [], mandatory_pch_off_targets: [],
  },
])

const EVIDENCE_ITEM = {
  type: 'object', required: ['command', 'exit_code', 'output'],
  properties: {
    command: { type: 'string' },
    exit_code: { type: 'integer' },
    output: { type: 'string', description: 'verbatim output excerpt; empty output is invalid' },
  },
}
const LEASE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['action', 'lease_name', 'run_id', 'branch', 'base_sha', 'worktree', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc', 'exit_code', 'output'],
  properties: {
    action: { type: 'string', enum: ['acquire', 'release'] }, lease_name: { type: 'string' },
    run_id: { type: 'string' }, branch: { type: 'string' }, base_sha: { type: 'string' },
    worktree: { type: 'string' }, heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' },
    keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const HEAD_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['ref', 'sha', 'command', 'exit_code', 'output'],
  properties: {
    ref: { type: 'string' }, sha: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const INTEGRATION_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['reviewed_sha', 'head_after', 'command', 'exit_code', 'output'],
  properties: {
    reviewed_sha: { type: 'string' }, head_after: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const GATE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['gate_id', 'tested_sha', 'command', 'exit_code', 'output'],
  properties: {
    gate_id: { type: 'string' }, tested_sha: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const FREEZE = {
  type: 'object', required: ['base_ref', 'base_sha', 'evidence'],
  properties: {
    base_ref: { type: 'string' },
    base_sha: { type: 'string', description: 'full 40-character commit SHA' },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const LANE = {
  type: 'object', additionalProperties: false,
  required: ['id', 'title', 'goal', 'branch', 'files_in_scope', 'files_forbidden', 'gate_closure', 'done_criteria', 'out_of_scope'],
  properties: {
    id: { type: 'string' }, title: { type: 'string' }, goal: { type: 'string' },
    branch: { type: 'string' },
    files_in_scope: { type: 'array', items: { type: 'string' } },
    files_forbidden: { type: 'array', items: { type: 'string' } },
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
  type: 'object', required: ['lanes', 'integration_branch', 'shared_files_note'],
  properties: {
    lanes: { type: 'array', items: LANE },
    integration_branch: { type: 'string' },
    shared_files_note: { type: 'string' },
  },
}
const REPORT = {
  type: 'object',
  required: ['lane_id', 'outcome', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'files_changed', 'evidence', 'deviations', 'ledger_candidates'],
  properties: {
    lane_id: { type: 'string' }, outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] },
    branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT,
    files_changed: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    deviations: { type: 'string' },
    ledger_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const REVIEW = {
  type: 'object', required: ['verdict', 'reviewed_sha', 'evidence', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] },
    reviewed_sha: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    findings: { type: 'array', items: {
      type: 'object', required: ['location', 'severity', 'problem', 'fix'],
      properties: {
        location: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
        problem: { type: 'string' }, fix: { type: 'string' },
      },
    } },
  },
}
const CLEANUP = {
  type: 'object', required: ['passed', 'released', 'release_receipts', 'evidence'],
  properties: {
    passed: { type: 'boolean' },
    released: { type: 'array', items: { type: 'string' } },
    release_receipts: { type: 'array', items: LEASE_RECEIPT },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const GATE = {
  type: 'object',
  required: ['passed', 'base_sha', 'lease_run_id', 'integration_branch', 'sha', 'integrated_shas', 'integration_worktree', 'integration_lease', 'integration_heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'integration_receipts', 'head_receipt', 'gate_receipts', 'release_receipt', 'gate_results', 'leases_released', 'ledger_appended'],
  properties: {
    passed: { type: 'boolean' }, base_sha: { type: 'string' }, lease_run_id: { type: 'string' },
    integration_branch: { type: 'string' }, sha: { type: 'string' },
    integrated_shas: { type: 'array', items: { type: 'string' } },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' },
    integration_heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT, integration_receipts: { type: 'array', items: INTEGRATION_RECEIPT },
    head_receipt: HEAD_RECEIPT, gate_receipts: { type: 'array', items: GATE_RECEIPT },
    release_receipt: LEASE_RECEIPT,
    gate_results: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    leases_released: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } },
  },
}

function validSuccessEvidence(evidence) {
  return Array.isArray(evidence) && evidence.length > 0 && evidence.every(item =>
    item && typeof item.command === 'string' && item.command.trim() &&
    item.exit_code === 0 && typeof item.output === 'string' && item.output.trim())
}

function validLeaseReceipt(receipt, expected, action) {
  if (!receipt || receipt.action !== action || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return false
  if (receipt.lease_name !== expected.lease_name || receipt.run_id !== expected.run_id ||
      receipt.branch !== expected.branch || receipt.base_sha !== expected.base_sha ||
      receipt.worktree !== expected.worktree || receipt.heartbeat_id !== expected.heartbeat_id ||
      receipt.keeper_pid !== expected.keeper_pid || receipt.keeper_process_started_utc !== expected.keeper_process_started_utc) return false
  if (!Number.isInteger(receipt.keeper_pid) || receipt.keeper_pid <= 0 || !/^\d{4}-/.test(receipt.keeper_process_started_utc || '') || !/^\d{4}-/.test(receipt.keeper_ready_utc || '')) return false
  return receipt.output.includes(receipt.lease_name) && receipt.output.includes(receipt.run_id) &&
    (action === 'release' || (receipt.output.includes(String(receipt.keeper_pid)) && receipt.output.includes(receipt.heartbeat_id) && receipt.output.includes(receipt.keeper_ready_utc)))
}

function validHeadReceipt(receipt, ref, sha) {
  return !!receipt && receipt.ref === ref && receipt.sha === sha && receipt.exit_code === 0 &&
    receipt.command.trim() === `git rev-parse ${ref}` && receipt.output.trim() === sha
}

function validIntegrationCommand(receipt, reviewedSha) {
  return !!receipt && receipt.reviewed_sha === reviewedSha && receipt.exit_code === 0 &&
    new RegExp(`^git\\s+(?:merge(?:\\s+--(?:ff-only|no-ff|no-edit))*|cherry-pick(?:\\s+--(?:ff|no-commit))*)\\s+${reviewedSha}$`, 'i').test(String(receipt.command || '').trim())
}

function laneHeartbeatId(lane) {
  return `${RUN_SLUG}-lane-${String(lane.id).replace(/[^A-Za-z0-9._-]/g, '-')}`
}

function changedHeader(path) {
  return /\.(?:h|hh|hpp|hxx|inl)$/i.test(String(path || ''))
}

function changedCode(path) {
  return /\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|inl)$/i.test(String(path || ''))
}

function safeTarget(value) {
  return /^[A-Za-z0-9][A-Za-z0-9_.+-]{1,63}$/.test(String(value || '')) && String(value).toLowerCase() !== 'all'
}

function safeUnitRegex(value) {
  const text = String(value || '')
  return /^\^[A-Za-z][A-Za-z0-9_.-]{3,63}\$$/.test(text) && !/atx_vol/i.test(text)
}

function safeOracleTest(value) {
  return /^[a-z0-9][a-z0-9_-]{2,63}$/.test(String(value || ''))
}

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
  const normalizedScope = lane.files_in_scope.map(path => String(path).replace(/\\/g, '/').toLowerCase())
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
        !item.oracle_tests.every(test => owner.oracle_tests.includes(test)) || !item.unit_regexes.every(regex => owner.suite_prefixes.some(prefix => regexSuiteName(regex).startsWith(prefix)) && UNIT_TEST_GATE_REGISTRY[regex])) return `gate closure does not match workflow path owner: ${item.file}`
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

function derivedGateRegistry(entries, includeFixed = false) {
  const gates = []
  const add = (gate_id, command) => { if (!gates.some(item => item.gate_id === gate_id)) gates.push({ gate_id, command }) }
  for (const entry of entries) {
    for (const target of entry.unit_targets) add(`unit-build:${target}`, `powershell scripts\\atx-build.ps1 -Preset dev build ${target}`)
    for (const regex of entry.unit_regexes) add(`unit-test:${regex}`, `powershell scripts\\oracle-targeted-gate.ps1 -Gate ${UNIT_TEST_GATE_REGISTRY[regex].adapter_gate}`)
    for (const test of entry.oracle_tests) add(`oracle-test:${test}`, `atx-vol-oracle-bench --cohort smoke,tune --test ${test} --aggregate-only`)
    if (changedHeader(entry.file)) for (const target of entry.pch_off_targets) add(`pch-off:${target}`, `powershell scripts\\atx-build.ps1 -Preset hygiene build ${target}`)
  }
  if (includeFixed) for (const gate of FIXED_ORACLE_GATES) add(gate.gate_id, gate.command)
  return gates.sort((left, right) => left.gate_id.localeCompare(right.gate_id))
}

function sprintUnitEvidenceError(evidence, expectedGate) {
  if (!expectedGate.gate_id.startsWith('unit-test:')) return null
  let result
  try { result = JSON.parse(String(evidence && evidence.output || '')) } catch { return 'unit-test evidence is not typed adapter JSON' }
  const regex = expectedGate.gate_id.slice('unit-test:'.length)
  const adapter = UNIT_TEST_GATE_REGISTRY[regex]
  const exactKeys = ['schema_version', 'status', 'observations', 'command_id', 'gate_kind', 'tests_executed', 'tests_passed', 'rows_processed', 'metric_ids', 'audit_summary', 'raw_output_sha256'].sort()
  if (!adapter || !result || Object.keys(result).sort().join(',') !== exactKeys.join(',') || result.schema_version !== 1 || result.status !== 'PASS' ||
      result.command_id !== adapter.adapter_gate || result.gate_kind !== 'ctest' || !Number.isInteger(result.observations) || result.observations <= 0 ||
      !Number.isInteger(result.tests_executed) || result.tests_executed <= 0 || result.tests_passed !== result.tests_executed || result.rows_processed !== 0 ||
      !Array.isArray(result.metric_ids) || result.metric_ids.length !== 0 || result.audit_summary !== `tests_executed=${result.tests_executed} tests_passed=${result.tests_passed}` ||
      !/^[0-9a-f]{64}$/.test(result.raw_output_sha256 || '')) return 'unit-test evidence reports invalid or zero semantic work'
  return null
}

function oracleLoopCommandError(command) {
  const text = String(command || '').trim()
  if (!text) return 'empty command'
  if (/[;&|`]/.test(text)) return 'chained command forbidden in oracle loop'
  if (/(?:^|\s)-L(?:\s|=)/i.test(text) || /run_all_gates/i.test(text)) return 'label/full regression command forbidden in oracle loop'
  if (/(?:^|\s)(?:ctest|-Ctest)(?:\s|$)/i.test(text)) {
    const match = text.match(/(?:^|\s)-R\s+(\S+)/i)
    if (!match || !safeUnitRegex(match[1])) return 'broad or unanchored ctest forbidden in oracle loop'
  }
  const tokens = text.split(/\s+/)
  for (let index = 0; index < tokens.length; index += 1) {
    if (/(?:^|[\\/])[^\\/\s]*(?:test|tests)(?:\.exe)?$/i.test(tokens[index]) && !/^-/i.test(tokens[index]) &&
        !['build', '--target'].includes(String(tokens[index - 1] || '').toLowerCase())) return 'direct test executable forbidden in oracle loop'
  }
  if (/\bpytest\b/i.test(text) || /\bnode\s+--test\b/i.test(text)) return 'unscoped external test runner forbidden in oracle loop'
  if (/cmake\s+--build/i.test(text) && !/--target\s+[A-Za-z0-9_.+-]+/i.test(text)) return 'broad cmake build forbidden in oracle loop'
  if (/atx-build\.ps1[^\r\n]*\bbuild(?:\s*$|\s+all(?:\s|$))/i.test(text) || /(?:--target|\bbuild\s+)\s+all(?:\s|$)/i.test(text)) return 'broad build target forbidden in oracle loop'
  return null
}

function reportContractError(report, lane, baseSha) {
  if (!report) return 'agent returned no report'
  if (report.outcome !== 'DONE') return `lane outcome ${report.outcome || 'missing'}`
  if (report.lane_id !== lane.id) return `lane_id mismatch: ${report.lane_id}`
  if (report.branch !== lane.branch) return `branch mismatch: ${report.branch}`
  if (report.base_sha !== baseSha) return `base SHA mismatch: ${report.base_sha}`
  if (!/^[0-9a-f]{40}$/i.test(report.sha || '')) return 'missing full commit SHA'
  if (report.lease_run_id !== RUN_ID) return `lease run_id mismatch: ${report.lease_run_id}`
  if (!/^pool-[0-9]+$/.test(report.lease_name || '')) return `invalid lease name: ${report.lease_name}`
  if (report.heartbeat_id !== laneHeartbeatId(lane)) return `heartbeat mismatch: ${report.heartbeat_id}`
  if (!validLeaseReceipt(report.acquisition_receipt, {
    lease_name: report.lease_name, run_id: RUN_ID, branch: lane.branch, base_sha: baseSha,
    worktree: report.worktree, heartbeat_id: report.heartbeat_id, keeper_pid: report.keeper_pid,
    keeper_process_started_utc: report.keeper_process_started_utc,
  }, 'acquire')) return 'lane acquisition receipt invalid'
  if (!validSuccessEvidence(report.evidence)) return 'missing successful command/output evidence'
  const commandError = [...(report.evidence || []), ...(report.diagnostics || [])].map(item => oracleLoopCommandError(item && item.command)).find(Boolean)
  if (commandError) return commandError
  if (!Array.isArray(report.files_changed) || !report.files_changed.length || new Set(report.files_changed).size !== report.files_changed.length) return 'changed-file report missing/duplicated'
  const scope = new Set(lane.files_in_scope.map(path => String(path).replace(/\\/g, '/').toLowerCase()))
  if (!report.files_changed.every(path => scope.has(String(path).replace(/\\/g, '/').toLowerCase()))) return 'changed file outside lane scope'
  const requiredGates = derivedGateRegistry(closureEntries(lane, report.files_changed))
  if (!requiredGates.length) return 'changed dependency closure produced no targeted gates'
  if (report.evidence.length !== requiredGates.length || new Set(report.evidence.map(item => item.command)).size !== requiredGates.length) return 'lane evidence must equal exact changed-closure command set'
  const missing = requiredGates.filter(gate => !report.evidence.some(item => item.command === gate.command && item.exit_code === 0 && String(item.output || '').trim() && !sprintUnitEvidenceError(item, gate)))
  if (missing.length) return `evidence missing exact changed-closure gates: ${missing.map(gate => gate.gate_id).join(', ')}`
  return null
}

function reviewContractError(review, report) {
  if (!review) return 'review agent returned no result'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== report.sha) return `reviewed stale SHA ${review.reviewed_sha}; expected ${report.sha}`
  if (!validSuccessEvidence(review.evidence)) return 'review has no successful command/output evidence'
  const commandError = [...(review.evidence || []), ...(review.diagnostics || [])].map(item => oracleLoopCommandError(item && item.command)).find(Boolean)
  if (commandError) return `review used invalid oracle-loop command: ${commandError}`
  if (review.verdict === 'APPROVE' && review.findings.some(f => f.severity === 'blocker')) {
    return 'APPROVE verdict contains a blocker finding'
  }
  if (review.verdict === 'BLOCK' && !review.findings.some(f => f.severity === 'blocker')) {
    return 'BLOCK verdict has no blocker finding'
  }
  return null
}

function laneFailureReason(state, lane) {
  if (!state) return `${lane.id}: incomplete pipeline`
  if (state.contract_error) return `${lane.id}: ${state.contract_error}`
  if (!state.report || state.report.outcome !== 'DONE') return `${lane.id}: not DONE`
  if (!state.review || state.review.verdict !== 'APPROVE') return `${lane.id}: final review not APPROVE`
  if (state.review.reviewed_sha !== state.report.sha) return `${lane.id}: final review is stale`
  return null
}

function gateContractError(gate, expected) {
  if (!gate || !gate.passed) return 'integration gate missing/failed'
  if (!validSuccessEvidence(gate.gate_results)) return 'integration gate lacks successful evidence'
  const commandError = [...(gate.gate_results || []), ...(gate.gate_receipts || []), ...(gate.diagnostics || [])].map(item => oracleLoopCommandError(item && item.command)).find(Boolean)
  if (commandError) return commandError
  if (gate.base_sha !== expected.base_sha || gate.lease_run_id !== expected.run_id ||
      gate.integration_branch !== expected.branch || !/^[0-9a-f]{40}$/i.test(gate.sha || '')) return 'integration identity mismatch'
  if (!Array.isArray(gate.integrated_shas) || gate.integrated_shas.length !== expected.reviewed_shas.length ||
      !gate.integrated_shas.every((sha, index) => sha === expected.reviewed_shas[index])) return 'integrated SHA list mismatch'
  if (!/^pool-[0-9]+$/.test(gate.integration_lease || '') || gate.integration_heartbeat_id !== expected.heartbeat_id ||
      !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(gate.integration_worktree || '') ||
      !gate.integration_worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${gate.integration_lease.toLowerCase()}`)) return 'integration lease/worktree mismatch'
  if (!Array.isArray(gate.leases_released) || !gate.leases_released.includes(gate.integration_lease)) return 'integration lease not released'
  const leaseExpected = {
    lease_name: gate.integration_lease, run_id: expected.run_id, branch: expected.branch,
    base_sha: expected.base_sha, worktree: gate.integration_worktree,
    heartbeat_id: expected.heartbeat_id, keeper_pid: gate.keeper_pid,
    keeper_process_started_utc: gate.keeper_process_started_utc,
  }
  if (!validLeaseReceipt(gate.acquisition_receipt, leaseExpected, 'acquire')) return 'integration acquisition receipt invalid'
  if (!validLeaseReceipt(gate.release_receipt, leaseExpected, 'release')) return 'integration release receipt invalid'
  if (!Array.isArray(gate.integration_receipts) || gate.integration_receipts.length !== expected.reviewed_shas.length) return 'integration receipts missing'
  for (let index = 0; index < expected.reviewed_shas.length; index += 1) {
    const receipt = gate.integration_receipts[index]
    if (!validIntegrationCommand(receipt, expected.reviewed_shas[index]) ||
        !/^[0-9a-f]{40}$/i.test(receipt.head_after || '') || !String(receipt.output || '').includes(receipt.head_after)) return 'exact reviewed SHA integration receipt invalid'
  }
  if (!validHeadReceipt(gate.head_receipt, 'HEAD', gate.sha)) return 'integration HEAD receipt invalid'
  if (!Array.isArray(expected.gate_registry) || !expected.gate_registry.length || !Array.isArray(gate.gate_receipts) || gate.gate_receipts.length !== expected.gate_registry.length || new Set(gate.gate_receipts.map(receipt => receipt && receipt.gate_id)).size !== expected.gate_registry.length) return 'targeted gate receipts missing/extra/duplicated'
  for (const expectedGate of expected.gate_registry) {
    const gateId = expectedGate.gate_id
    const matches = gate.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    const receipt = matches[0]
    if (matches.length !== 1 || receipt.tested_sha !== gate.sha || receipt.command !== expectedGate.command || receipt.exit_code !== 0 || !String(receipt.output || '').trim() || sprintUnitEvidenceError(receipt, expectedGate)) return `required targeted gate receipt invalid: ${gateId}`
    if (!gate.gate_results.some(item => item.command === receipt.command && item.exit_code === 0 && item.output === receipt.output)) {
      return `required gate missing from returned evidence: ${gateId}`
    }
  }
  if (gate.gate_results.length !== expected.gate_registry.length) return 'gate evidence count does not equal exact targeted gate set'
  return null
}

phase('Freeze')
const freeze = await agent(
  `Read-only preflight for vol-sprint. Resolve ${BASE_REF}^{commit} once with git rev-parse. Do not edit, lease, build, merge, or switch branches. Return the full SHA and pasted command output.`,
  { schema: FREEZE, label: 'freeze-base' },
)
if (!freeze || freeze.base_ref !== BASE_REF || !/^[0-9a-f]{40}$/i.test(freeze.base_sha || '') || !validSuccessEvidence(freeze.evidence)) {
  throw new Error('could not freeze base ref with command evidence')
}
const BASE_SHA = freeze.base_sha
const RUN_ID = sprintRunId(BASE_SHA, args.task, args.run_key)
const RUN_SLUG = RUN_ID

phase('Plan')
const plan = await agent(
  `Task: ${args.task}\nFrozen base: ${BASE_REF} -> ${BASE_SHA}\nRun id: ${RUN_ID}\n\nDecompose into 1-4 file-disjoint lanes. Every listed lane is mandatory. Branches must be lane/<id> and the integration branch must be integration/<task>. Reserve shared files for exactly one lane or the integration gate. For EVERY files_in_scope path return one exact gate_closure entry. Changed C/C++ files require affected unit targets plus anchored non-broad unit regexes; every lane needs hypothesis-specific OracleBench test IDs; headers require owning scoped PCH-off targets. Never assign labels, broad ctest/builds, full regression/release suites, or full-repo hygiene. Do not edit, lease, build, or inspect licensed oracle row data.`,
  { agentType: 'vol-planner', schema: PLAN, label: 'plan' },
)
if (!plan || !Array.isArray(plan.lanes) || plan.lanes.length < 1 || plan.lanes.length > 4) {
  throw new Error('planner must return 1-4 mandatory lanes')
}
const lanes = plan.lanes.map(lane => ({
  ...lane,
  branch: `lane/${String(lane.id).replace(/[^A-Za-z0-9._-]/g, '-')}-${RUN_SLUG}`,
}))
const INTEGRATION_BRANCH = `integration/${RUN_SLUG}`
const planClosureErrors = lanes.map(lane => closureError(lane)).filter(Boolean)
if (planClosureErrors.length) throw new Error(`planner changed-dependency closure invalid: ${planClosureErrors.join('; ')}`)
if (new Set(lanes.map(lane => lane.id)).size !== lanes.length ||
    new Set(lanes.map(lane => lane.branch)).size !== lanes.length) {
  throw new Error('lane ids/derived run-unique branches must be unique')
}

const results = await pipeline(
  lanes,
  lane => agent(
    `Mandatory lane brief (JSON):\n${JSON.stringify(lane, null, 2)}\n\nFrozen base SHA: ${BASE_SHA}. Harness run_id: ${RUN_ID}. Lease only with: powershell scripts\\lease-worktree.ps1 -Branch ${lane.branch} -Base ${BASE_SHA} -Agent vol-builder-${lane.id} -RunId ${RUN_ID} -HeartbeatId ${laneHeartbeatId(lane)} -MaxPool 20. The lease starts an independent continuous keeper; do not substitute foreground pulses for ownership. Never work in C:\\atx. Implement and run only the planned small changed-closure commands: ${JSON.stringify(derivedGateRegistry(lane.gate_closure))}. Labels, broad ctest/builds, full suites, and full-repo hygiene are forbidden. Commit explicit paths, keep the lease held, and return the typed acquisition receipt including keeper PID/start plus only exit_code=0 commands under evidence; failed diagnostics belong under diagnostics.`,
    { agentType: 'vol-builder', schema: REPORT, phase: 'Build', label: `build:${lane.id}` },
  ),
  (report, lane) => {
    const contractError = reportContractError(report, lane, BASE_SHA)
    if (contractError) return { report, review: null, contract_error: contractError, fix_round: false }
    return agent(
      `Fresh review of mandatory lane ${lane.id}. Frozen base SHA: ${BASE_SHA}. Brief:\n${JSON.stringify(lane, null, 2)}\nBuilder report:\n${JSON.stringify(report, null, 2)}\nIndependently inspect git diff ${BASE_SHA}...${report.sha} and verify at least one relevant check with pasted output. Review exactly commit ${report.sha}.`,
      { agentType: 'vol-reviewer', schema: REVIEW, phase: 'Review', label: `review:${lane.id}` },
    ).then(review => ({ report, review, contract_error: reviewContractError(review, report), fix_round: false }))
  },
  (state, lane) => {
    if (!state || state.contract_error || !state.review || state.review.verdict === 'APPROVE') return state
    const blockers = state.review.findings.filter(f => f.severity === 'blocker')
    if (!blockers.length) return { ...state, contract_error: 'BLOCK review had no blockers' }
    return agent(
      `Fix mandatory lane ${lane.id} at ${state.report.worktree} on ${state.report.branch}. Frozen base SHA: ${BASE_SHA}; run_id=${RUN_ID}. Address exactly these blockers:\n${JSON.stringify(blockers, null, 2)}\nRerun only the exact small changed-closure commands ${JSON.stringify(derivedGateRegistry(lane.gate_closure))}; labels, broad/full suites, and full-repo hygiene remain forbidden. Commit explicit paths, keep the same lease, and report the NEW commit SHA with pasted output.`,
      { agentType: 'vol-builder', schema: REPORT, phase: 'Fix', label: `fix:${lane.id}` },
    ).then(report => ({
      report,
      review: null,
      contract_error: reportContractError(report, lane, BASE_SHA),
      fix_round: true,
    }))
  },
  (state, lane) => {
    if (!state || state.contract_error || !state.fix_round) return state
    return agent(
      `FRESH RE-REVIEW after Fix for mandatory lane ${lane.id}. Do not reuse the prior verdict. Frozen base SHA: ${BASE_SHA}; fixed commit: ${state.report.sha}. Brief:\n${JSON.stringify(lane, null, 2)}\nIndependently inspect git diff ${BASE_SHA}...${state.report.sha}, rerun a relevant check, and return a verdict for exactly ${state.report.sha} with pasted output.`,
      { agentType: 'vol-reviewer', schema: REVIEW, phase: 'Re-review', label: `rereview:${lane.id}` },
    ).then(review => ({ ...state, review, contract_error: reviewContractError(review, state.report) }))
  },
)

const failures = lanes.map((lane, index) => laneFailureReason(results[index], lane)).filter(Boolean)

const heldLeases = results.filter(Boolean).map(state => state.report).filter(report =>
  report && /^pool-[0-9]+$/.test(report.lease_name || '') && report.lease_run_id === RUN_ID)

if (failures.length) {
  phase('Abort')
  let cleanup = { passed: heldLeases.length === 0, released: [], release_receipts: [], evidence: [] }
  if (heldLeases.length) {
    cleanup = await agent(
      `ABORT before integration. Mandatory lane failures:\n${failures.join('\n')}\nRelease only these run-owned leases, without editing, merging, building, or using C:\\atx: ${JSON.stringify(heldLeases.map(r => ({ lease: r.lease_name, run_id: RUN_ID, worktree: r.worktree, branch: r.branch, base_sha: r.base_sha, heartbeat_id: r.heartbeat_id, keeper_pid: r.keeper_pid, keeper_process_started_utc: r.keeper_process_started_utc })))}. For each use powershell scripts\\lease-worktree.ps1 -Release <pool-N> -RunId ${RUN_ID}. Return one typed release receipt and pasted output per lease.`,
      { agentType: 'vol-verifier', schema: CLEANUP, label: 'abort-cleanup' },
    )
  }
  return {
    passed: false, integration: null, gate_results: [],
    lanes: results.filter(Boolean).map(state => state.report && ({ lane: state.report.lane_id, outcome: state.report.outcome, branch: `${state.report.branch}@${state.report.sha}`, verdict: state.review ? state.review.verdict : null })).filter(Boolean),
    blocked: failures, leases_released: cleanup ? cleanup.released : [], ledger: [], run_id: RUN_ID, base_sha: BASE_SHA,
  }
}

phase('Release')
const release = await agent(
  `All mandatory lanes are DONE and freshly APPROVED. Before any integration lease is acquired, release these lane leases: ${JSON.stringify(heldLeases.map(r => ({ lease: r.lease_name, run_id: RUN_ID, worktree: r.worktree, branch: r.branch, base_sha: r.base_sha, heartbeat_id: r.heartbeat_id, keeper_pid: r.keeper_pid, keeper_process_started_utc: r.keeper_process_started_utc })))}. Use powershell scripts\\lease-worktree.ps1 -Release <pool-N> -RunId ${RUN_ID}; do not edit, merge, build, or use C:\\atx. Return one typed release receipt and pasted output per lease.`,
  { agentType: 'vol-verifier', schema: CLEANUP, label: 'release-lanes' },
)
const expectedReleased = heldLeases.map(report => report.lease_name)
const releaseComplete = release && release.passed && validSuccessEvidence(release.evidence) &&
  expectedReleased.every(name => release.released.includes(name)) &&
  expectedReleased.every(name => {
    const report = heldLeases.find(item => item.lease_name === name)
    return release.release_receipts.filter(receipt => validLeaseReceipt(receipt, {
      lease_name: report.lease_name, run_id: RUN_ID, branch: report.branch, base_sha: report.base_sha,
      worktree: report.worktree, heartbeat_id: report.heartbeat_id, keeper_pid: report.keeper_pid,
      keeper_process_started_utc: report.keeper_process_started_utc,
    }, 'release')).length === 1
  }) &&
  expectedReleased.every(name => release.evidence.some(item =>
    item.command.includes(name) && item.command.includes('-RunId') && item.output.includes(name)))
if (!releaseComplete) {
  return {
    passed: false, integration: null, gate_results: [],
    lanes: results.map(state => ({ lane: state.report.lane_id, outcome: state.report.outcome, branch: `${state.report.branch}@${state.report.sha}`, verdict: state.review.verdict })),
    blocked: ['lane lease release incomplete; integration was not started'],
    leases_released: release ? release.released : [], ledger: [], run_id: RUN_ID, base_sha: BASE_SHA,
  }
}

const changedClosureEntries = results.flatMap((state, index) => closureEntries(lanes[index], state.report.files_changed))
const REQUIRED_GATE_REGISTRY = derivedGateRegistry(changedClosureEntries, true)
if (!REQUIRED_GATE_REGISTRY.some(gate => gate.gate_id.startsWith('unit-test:')) || !REQUIRED_GATE_REGISTRY.some(gate => gate.gate_id.startsWith('oracle-test:'))) {
  throw new Error('changed closure omitted affected unit or hypothesis-specific OracleBench gates')
}

phase('Gate')
const gate = await agent(
  `Gate this vol-sprint in a NEW isolated pool lease. Frozen base SHA: ${BASE_SHA}. Integration branch: ${INTEGRATION_BRANCH}. Harness run_id: ${RUN_ID}. Lane commits in brief order:\n${JSON.stringify(results.map(state => ({ lane: state.report.lane_id, branch: state.report.branch, sha: state.report.sha, reviewed_sha: state.review.reviewed_sha, verdict: state.review.verdict })))}\nWorkflow-derived changed-file closure: ${JSON.stringify(results.map(state => ({ lane: state.report.lane_id, files: state.report.files_changed })))}. Run this COMPLETE exact targeted gate registry once and omit nothing: ${JSON.stringify(REQUIRED_GATE_REGISTRY)}. It contains affected unit targets/anchored tests, hypothesis OracleBench tests, required aggregate smoke+tune scorecards, quiet pinned speed, and header-only scoped PCH-off targets. Labels, broad ctest/builds, full regression/release suites, and full-repo hygiene are forbidden.\nShared-files ownership: ${plan.shared_files_note}\n\nFirst acquire with powershell scripts\\lease-worktree.ps1 -Branch ${INTEGRATION_BRANCH} -Base ${BASE_SHA} -Agent vol-verifier -RunId ${RUN_ID} -HeartbeatId ${RUN_SLUG}-integration -MaxPool 20. The returned C:\\atx-wt\\pool-N path is the ONLY place integration, builds, tests, and ledger append may occur; never use C:\\atx. Report typed keeper-backed acquisition. Integrate each exact reviewed SHA in listed order and prove final HEAD BEFORE gates. Run every registry command exactly once against that tested_sha, return one exact receipt/evidence item per gate, commit gate-owned memory if changed, then release. A conflict or gate failure is passed=false but still releases. Failures belong in diagnostics.`,
  { agentType: 'vol-verifier', schema: GATE, label: 'gate' },
)

const gateError = gateContractError(gate, {
  base_sha: BASE_SHA, run_id: RUN_ID, branch: INTEGRATION_BRANCH,
  heartbeat_id: `${RUN_SLUG}-integration`, reviewed_shas: results.map(state => state.review.reviewed_sha),
  gate_registry: REQUIRED_GATE_REGISTRY,
})
const gateContractPassed = !gateError

return {
  passed: !!(gate && gate.passed && gateContractPassed),
  integration: gate ? `${gate.integration_branch} @ ${gate.sha}` : null,
  integration_branch: gate ? gate.integration_branch : null,
  integration_sha: gate ? gate.sha : null,
  reviewed_lane_shas: results.map(state => state.review.reviewed_sha),
  gate_evidence: gate ? gate.gate_results : [],
  integration_worktree: gate ? gate.integration_worktree : null,
  gate_results: gate ? gate.gate_results : [],
  lanes: results.map(state => ({ lane: state.report.lane_id, outcome: state.report.outcome, branch: `${state.report.branch}@${state.report.sha}`, verdict: state.review.verdict })),
  blocked: gateContractPassed ? [] : [gateError],
  leases_released: [...release.released, ...(gate ? gate.leases_released : [])],
  ledger: gate ? gate.ledger_appended : [],
  run_id: RUN_ID,
  base_sha: BASE_SHA,
}
