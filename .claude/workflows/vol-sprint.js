export const meta = {
  name: 'vol-sprint',
  description: 'atx-vol DAG: frozen-base plan, isolated lanes, fresh review/fix/re-review, isolated integration gate',
  whenToUse: 'Multi-lane atx-vol feature/refactor work. Args: { task: string, base?: string }',
  phases: [
    { title: 'Freeze', detail: 'resolve the requested base ref to one immutable SHA' },
    { title: 'Plan', detail: 'decompose task into 1-4 disjoint mandatory lanes' },
    { title: 'Build', detail: 'one builder per lane in a run-owned pool lease' },
    { title: 'Review', detail: 'fresh-context review; every fix is reviewed again' },
    { title: 'Release', detail: 'release all approved lane leases before integration' },
    { title: 'Gate', detail: 'new isolated integration lease, gates, ledger, release' },
  ],
}

if (!args || !args.task) throw new Error('vol-sprint needs args: { task: "<what to build>", base?: "<ref, default main>" }')
const BASE_REF = (args && args.base) || 'main'
const RUN_ID = `vol-sprint-${Date.now()}-${Math.random().toString(16).slice(2)}`
const RUN_SLUG = RUN_ID.replace(/[^A-Za-z0-9._-]/g, '-')

const EVIDENCE_ITEM = {
  type: 'object', required: ['command', 'exit_code', 'output'],
  properties: {
    command: { type: 'string' },
    exit_code: { type: 'integer' },
    output: { type: 'string', description: 'verbatim output excerpt; empty output is invalid' },
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
  type: 'object',
  required: ['id', 'title', 'goal', 'branch', 'files_in_scope', 'files_forbidden', 'check_targets', 'build_targets', 'suites', 'done_criteria', 'out_of_scope'],
  properties: {
    id: { type: 'string' }, title: { type: 'string' }, goal: { type: 'string' },
    branch: { type: 'string' },
    files_in_scope: { type: 'array', items: { type: 'string' } },
    files_forbidden: { type: 'array', items: { type: 'string' } },
    check_targets: { type: 'array', items: { type: 'string' } },
    build_targets: { type: 'array', items: { type: 'string' } },
    suites: { type: 'array', items: { type: 'string' } },
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
  required: ['lane_id', 'outcome', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'files_changed', 'evidence', 'deviations', 'ledger_candidates'],
  properties: {
    lane_id: { type: 'string' }, outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] },
    branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' },
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
  type: 'object', required: ['passed', 'released', 'evidence'],
  properties: {
    passed: { type: 'boolean' },
    released: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const GATE = {
  type: 'object',
  required: ['passed', 'base_sha', 'lease_run_id', 'integration_branch', 'sha', 'integrated_shas', 'integration_worktree', 'integration_lease', 'integration_heartbeat_id', 'gate_results', 'leases_released', 'ledger_appended'],
  properties: {
    passed: { type: 'boolean' }, base_sha: { type: 'string' }, lease_run_id: { type: 'string' },
    integration_branch: { type: 'string' }, sha: { type: 'string' },
    integrated_shas: { type: 'array', items: { type: 'string' } },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' },
    integration_heartbeat_id: { type: 'string' },
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

function laneHeartbeatId(lane) {
  return `${RUN_SLUG}-lane-${String(lane.id).replace(/[^A-Za-z0-9._-]/g, '-')}`
}

function evidenceReferencesTarget(evidence, target) {
  const wanted = String(target || '').trim().replace(/\\/g, '/').toLowerCase()
  if (!wanted) return true
  return evidence.some(item => {
    const command = String(item.command || '').replace(/\\/g, '/').toLowerCase()
    return /(?:^|\s)(?:powershell(?:\.exe)?\s+)?(?:[^\s]*atx-build\.ps1|cmake|ctest|node|python|pytest|invoke-pester)(?:\s|$)/i.test(command) &&
      command.includes(wanted)
  })
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
  if (!validSuccessEvidence(report.evidence)) return 'missing successful command/output evidence'
  const requiredReferences = [...lane.check_targets, ...lane.build_targets, ...lane.suites]
  const missing = requiredReferences.filter(target => !evidenceReferencesTarget(report.evidence, target))
  if (missing.length) return `evidence does not reference required checks: ${missing.join(', ')}`
  return null
}

function reviewContractError(review, report) {
  if (!review) return 'review agent returned no result'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== report.sha) return `reviewed stale SHA ${review.reviewed_sha}; expected ${report.sha}`
  if (!validSuccessEvidence(review.evidence)) return 'review has no successful command/output evidence'
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
  if (gate.base_sha !== expected.base_sha || gate.lease_run_id !== expected.run_id ||
      gate.integration_branch !== expected.branch || !/^[0-9a-f]{40}$/i.test(gate.sha || '')) return 'integration identity mismatch'
  if (!Array.isArray(gate.integrated_shas) || gate.integrated_shas.length !== expected.reviewed_shas.length ||
      !gate.integrated_shas.every((sha, index) => sha === expected.reviewed_shas[index])) return 'integrated SHA list mismatch'
  if (!/^pool-[0-9]+$/.test(gate.integration_lease || '') || gate.integration_heartbeat_id !== expected.heartbeat_id ||
      !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(gate.integration_worktree || '') ||
      !gate.integration_worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${gate.integration_lease.toLowerCase()}`)) return 'integration lease/worktree mismatch'
  if (!Array.isArray(gate.leases_released) || !gate.leases_released.includes(gate.integration_lease)) return 'integration lease not released'
  return null
}

phase('Freeze')
const freeze = await agent(
  `Read-only preflight for vol-sprint run_id=${RUN_ID}. Resolve ${BASE_REF}^{commit} once with git rev-parse. Do not edit, lease, build, merge, or switch branches. Return the full SHA and pasted command output.`,
  { schema: FREEZE, label: 'freeze-base' },
)
if (!freeze || freeze.base_ref !== BASE_REF || !/^[0-9a-f]{40}$/i.test(freeze.base_sha || '') || !validSuccessEvidence(freeze.evidence)) {
  throw new Error('could not freeze base ref with command evidence')
}
const BASE_SHA = freeze.base_sha

phase('Plan')
const plan = await agent(
  `Task: ${args.task}\nFrozen base: ${BASE_REF} -> ${BASE_SHA}\nRun id: ${RUN_ID}\n\nDecompose into 1-4 file-disjoint lanes. Every listed lane is mandatory. Branches must be lane/<id> and the integration branch must be integration/<task>. Reserve shared files for exactly one lane or the integration gate. Do not edit, lease, build, or inspect licensed oracle row data.`,
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
if (new Set(lanes.map(lane => lane.id)).size !== lanes.length ||
    new Set(lanes.map(lane => lane.branch)).size !== lanes.length) {
  throw new Error('lane ids/derived run-unique branches must be unique')
}

const results = await pipeline(
  lanes,
  lane => agent(
    `Mandatory lane brief (JSON):\n${JSON.stringify(lane, null, 2)}\n\nFrozen base SHA: ${BASE_SHA}. Harness run_id: ${RUN_ID}. Lease only with: powershell scripts\\lease-worktree.ps1 -Branch ${lane.branch} -Base ${BASE_SHA} -Agent vol-builder-${lane.id} -RunId ${RUN_ID} -HeartbeatId ${laneHeartbeatId(lane)} -MaxPool 20. Pulse the reported pool before and after every long command with -Pulse <pool-N> -RunId ${RUN_ID}. Never work in C:\\atx. Implement, run every named check/build/suite, commit explicit paths, keep the lease held, and return only exit_code=0 commands under evidence; failed diagnostics belong under diagnostics.`,
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
      `Fix mandatory lane ${lane.id} at ${state.report.worktree} on ${state.report.branch}. Frozen base SHA: ${BASE_SHA}; run_id=${RUN_ID}. Address exactly these blockers:\n${JSON.stringify(blockers, null, 2)}\nRun every named lane check/build/suite again, commit explicit paths, keep the same lease, and report the NEW commit SHA with pasted output.`,
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
  let cleanup = { passed: heldLeases.length === 0, released: [], evidence: [] }
  if (heldLeases.length) {
    cleanup = await agent(
      `ABORT before integration. Mandatory lane failures:\n${failures.join('\n')}\nRelease only these run-owned leases, without editing, merging, building, or using C:\\atx: ${JSON.stringify(heldLeases.map(r => ({ lease: r.lease_name, run_id: RUN_ID, worktree: r.worktree })))}. For each use powershell scripts\\lease-worktree.ps1 -Release <pool-N> -RunId ${RUN_ID}. Return pasted output.`,
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
  `All mandatory lanes are DONE and freshly APPROVED. Before any integration lease is acquired, release these lane leases: ${JSON.stringify(heldLeases.map(r => ({ lease: r.lease_name, run_id: RUN_ID, worktree: r.worktree })))}. Use powershell scripts\\lease-worktree.ps1 -Release <pool-N> -RunId ${RUN_ID}; do not edit, merge, build, or use C:\\atx. Return pasted output for every release.`,
  { agentType: 'vol-verifier', schema: CLEANUP, label: 'release-lanes' },
)
const expectedReleased = heldLeases.map(report => report.lease_name)
const releaseComplete = release && release.passed && validSuccessEvidence(release.evidence) &&
  expectedReleased.every(name => release.released.includes(name)) &&
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

phase('Gate')
const gate = await agent(
  `Gate this vol-sprint in a NEW isolated pool lease. Frozen base SHA: ${BASE_SHA}. Integration branch: ${INTEGRATION_BRANCH}. Harness run_id: ${RUN_ID}. Lane commits in brief order:\n${JSON.stringify(results.map(state => ({ lane: state.report.lane_id, branch: state.report.branch, sha: state.report.sha, reviewed_sha: state.review.reviewed_sha, verdict: state.review.verdict })))}\nShared-files ownership: ${plan.shared_files_note}\n\nFirst acquire with powershell scripts\\lease-worktree.ps1 -Branch ${INTEGRATION_BRANCH} -Base ${BASE_SHA} -Agent vol-verifier -RunId ${RUN_ID} -HeartbeatId ${RUN_SLUG}-integration -MaxPool 20. Pulse before/after long gates. The returned C:\\atx-wt\\pool-N path is the ONLY place integration, builds, tests, and ledger append may occur; never use C:\\atx. Merge exact reviewed SHAs in the listed order and report them unchanged as integrated_shas, along with base_sha=${BASE_SHA} and lease_run_id=${RUN_ID}. Run required gates with pasted output, commit gate-owned memory if changed, then release the integration lease with -RunId ${RUN_ID}. A conflict or gate failure is passed=false but still releases the integration lease. Successful gate_results contain only exit_code=0 evidence; failures belong in diagnostics.`,
  { agentType: 'vol-verifier', schema: GATE, label: 'gate' },
)

const gateError = gateContractError(gate, {
  base_sha: BASE_SHA, run_id: RUN_ID, branch: INTEGRATION_BRANCH,
  heartbeat_id: `${RUN_SLUG}-integration`, reviewed_shas: results.map(state => state.review.reviewed_sha),
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
