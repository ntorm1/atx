export const meta = {
  name: 'aes-wave',
  description: 'Alpha-engine swarm wave batch: lanes implement -> sync -> adversarial review -> fix/re-review (<=2) -> serialized merge; resumable per lane',
  phases: [
    { title: 'Implement', detail: 'one implementer per lane in its leased pool' },
    { title: 'Sync', detail: 'merge the integration branch into the lane, re-run suites' },
    { title: 'Review', detail: 'fresh adversarial reviewer per lane' },
    { title: 'Fix', detail: 'fix passes + fix-only re-reviews (max 2)' },
    { title: 'Merge', detail: 'serialized merge into the integration branch' },
  ],
}

// args: { wave:'w0', waveTitle:'Truth', base:<sha>, integBranch:'feat/w0-integration', maxLanes:5,
//   lanes:[{ id, pool, branch, title, start:'implement'|'fix'|'final-sync', heavy?:bool,
//            reviewedSha?, rounds?, summary?, note?, mergeAfter?:[ids] }] }
const A = args
const W = A.wave
const WU = W.toUpperCase()
const BASE = A.base
const IB = A.integBranch
const INTEG = 'C:\\atx-wt\\pool-1'
const SDD = `.superpowers\\sdd\\${W}`
const SDDF = `.superpowers/sdd/${W}`
const COAUTH = 'Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>'

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    outcome: { type: 'string', enum: ['DONE', 'PARTIAL', 'BLOCKED'] },
    head_sha: { type: 'string' },
    summary: { type: 'string' },
    acceptance: { type: 'array', items: { type: 'object', properties: {
      item: { type: 'string' }, evidence: { type: 'string' },
      status: { type: 'string', enum: ['MET', 'UNMET', 'PARTIAL', 'DEFERRED'] } }, required: ['item', 'status', 'evidence'] } },
    defects: { type: 'array', items: { type: 'object', properties: {
      id: { type: 'string' }, status: { type: 'string', enum: ['CLOSED', 'DEFERRED', 'OPEN'] }, note: { type: 'string' } },
      required: ['id', 'status', 'note'] } },
    targets_green: { type: 'string' },
    key_numbers: { type: 'string' },
    integration_notes: { type: 'string' },
    blockers: { type: 'string' },
  },
  required: ['outcome', 'head_sha', 'summary', 'acceptance', 'defects', 'targets_green'],
}
const SYNC_SCHEMA = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['UP_TO_DATE', 'MERGED', 'CONFLICT', 'FAILED'] },
    head_sha: { type: 'string' }, tests: { type: 'string' }, notes: { type: 'string' },
  },
  required: ['status', 'head_sha', 'tests', 'notes'],
}
const REVIEW_SCHEMA = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] },
    reviewed_sha: { type: 'string' },
    findings: { type: 'array', items: { type: 'object', properties: {
      severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
      location: { type: 'string' }, problem: { type: 'string' }, required_fix: { type: 'string' } },
      required: ['severity', 'location', 'problem', 'required_fix'] } },
    acceptance_verified: { type: 'string' },
    defects_verified: { type: 'string' },
    waiver_needed: { type: 'string' },
    ownership_ok: { type: 'boolean' },
    tests_weakened: { type: 'boolean' },
    evidence: { type: 'string' },
  },
  required: ['verdict', 'reviewed_sha', 'findings', 'ownership_ok', 'tests_weakened', 'evidence'],
}
const FIX_SCHEMA = {
  type: 'object',
  properties: {
    head_sha: { type: 'string' },
    fixed: { type: 'array', items: { type: 'string' } },
    not_fixed: { type: 'array', items: { type: 'string' } },
    tests: { type: 'string' },
  },
  required: ['head_sha', 'fixed', 'not_fixed', 'tests'],
}
const MERGE_SCHEMA = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['MERGED', 'CONFLICT', 'REFUSED'] },
    integration_sha: { type: 'string' }, lane_head: { type: 'string' },
    release_output: { type: 'string' }, notes: { type: 'string' },
  },
  required: ['status', 'integration_sha', 'lane_head', 'notes'],
}

const poolOf = (L) => `C:\\atx-wt\\pool-${L.pool}`
const ID = (L) => L.id.toUpperCase()

function hdr(L, role) {
  const P = poolOf(L)
  const heavy = L.heavy
    ? `\n- This lane is RAM:HEAVY. Before any real-panel / warehouse / large-data run, acquire the heavy lock: create C:\\atx\\data\\.heavy-run.lock exclusively (PowerShell: [IO.File]::Open('C:\\atx\\data\\.heavy-run.lock','CreateNew').Close(); write your lane id + UTC into it via a second call is optional). If it already exists, wait (Start-Sleep 120, re-check, up to 60 times) — never delete another holder's lock. Delete it yourself as soon as your heavy run ends (also on failure). Creating/deleting that one file is the ONLY write allowed under C:\\atx. The warehouse (atx-db/data/warehouse.duckdb) may be opened ONLY with read_only=True; never write it, never kill a process holding it. Never read data dated >= 2020-01-01.`
    : ''
  return `You are the ${role} for lane ${WU}-${ID(L)} ("${L.title}") of the atx alpha-engine swarm (plan v2, wave ${WU} "${A.waveTitle}").
Worktree: ${P} (branch ${L.branch}; lease run id aes-${W}-${L.id} is already held for you — never run lease-worktree.ps1).
${WU} base commit: ${BASE}. Integration branch: ${IB} (worktree ${INTEG} — never modify it).

NON-NEGOTIABLE SAFETY (a breach is a lane failure):
- C:\\atx is ANOTHER SESSION'S LIVE CHECKOUT. Never run git or builds in it and never Edit/Write any path under C:\\atx\\ (except the heavy lock file if your lane is heavy). Your shell cwd resets to C:\\atx after EVERY tool call, so: every git command is \`git -C ${P} ...\`; every build/test command begins \`Set-Location ${P};\` inside the same PowerShell call; every file path you Read/Edit/Write starts with \`${P}\\\`.
- Never touch other pools, main, or ${IB}; never git push/stash/rebase/reset --hard/worktree/branch -D; never kill processes you did not start; never write atx-db/data/warehouse.duckdb; never read anything dated >= 2020-01-01.${heavy}
- The owner has WAIVED test-driven development for this series: do NOT invoke the test-driven-development skill; implement first, then add tests that prove every acceptance item. Tests are still mandatory and must never be weakened, skipped or deleted to go green.
- The owner wants REAL engine improvements and TRADEABLE alpha: production-quality code on the real research path (wired, not just a library function nobody calls, unless the lane scope says library-only), honest measured numbers, no toy stand-ins.
- RAM is tight (16 GB shared with other lanes and another session): every build call sets $env:CMAKE_BUILD_PARALLEL_LEVEL='2' and waits for >= 2 GB free RAM first (RULES §1). Build preset is equity-dev (build-equity\\); the dev preset is broken on this base.
Binding lane rules: ${P}\\${SDD}\\RULES.md. Brief: ${P}\\${SDD}\\lane-${L.id}-brief.md. House style: ${P}\\.agents\\cpp\\agent.md (§10 checklist). Plan: ${P}\\docs\\plans\\2026-09-24-alpha-engine-production-swarm.md. Findings: ${P}\\docs\\plans\\2026-09-24-alpha-engine-review-findings.md. Contracts: ${P}\\.agents\\harness\\TEMPLATES.md.${L.note ? '\nORCHESTRATOR NOTE FOR THIS LANE: ' + L.note : ''}`
}

function implPrompt(L) {
  const P = poolOf(L)
  return `${hdr(L, 'IMPLEMENTER')}

TASK — implement the whole lane:
1. \`git -C ${P} merge --no-ff ${IB} -m "${W}-${L.id}: merge ${IB}"\` first ("Already up to date" is fine). Then read RULES.md and your brief in full; read the plan §5 and the findings rows you close; read ${SDD}\\*integration-notes*.md if present. Read the code at every cited file:line before changing anything.
2. If build-equity\\CMakeCache.txt ATX_TEST_GROUPS differs from the brief's groups, reconfigure once (RULES §1).
3. Implement every "Build" item of your lane section, closing each cited defect ID (or deferring it with a concrete reason and target lane). Owned files only; versioned enums for any changed numeric behaviour; CMakeLists edits / new src .cpp only if RULES.md or your brief allows them.
4. Add tests (suite names from the brief) that prove EACH acceptance item with real measured numbers. Loop: \`check\` for fast type-checks, then \`build\` your owning target(s), then anchored \`-Ctest -Preset equity-dev -R '^Suite'\` runs, then each owning test executable whole once (build-equity\\bin\\<target>.exe --gtest_brief=1) — all green.
5. Write ${SDDF}/lane-${L.id}-report.md (RULES §4: acceptance table, defect table, verbatim evidence with exit codes and test counts, golden-digest old->new table, deviations, integration notes, ledger candidates) in clear normal English; \`git -C ${P} add -f\` it; commit everything (messages "${W}-${L.id}: ...", body ending with "${COAUTH}"). The tree must be clean at the end.
Work through to completion — do not stop at a plan or a partial. If an item is genuinely impossible, finish all others and record it UNMET/DEFERRED honestly with the reason.
Return the structured result; head_sha = \`git -C ${P} rev-parse HEAD\` after your final commit.`
}

function syncPrompt(L, why) {
  const P = poolOf(L)
  return `${hdr(L, 'SYNC AGENT')}

TASK — sync the lane with the integration branch (${why}):
1. \`git -C ${P} status --porcelain\` must be empty and no merge may be in progress; if leftover lane work is present, commit it with a clear message and say so; if a MERGE_HEAD exists with nothing to commit, run \`git -C ${P} merge --quit\`.
2. If \`git -C ${P} merge-base --is-ancestor ${IB} HEAD\` succeeds, the lane already contains integration: skip to step 4 only if the brief's suites were never run on this exact head (check the report's last "Post-merge sync" block); otherwise return status UP_TO_DATE with the current head.
3. Otherwise \`git -C ${P} merge --no-ff ${IB} -m "${W}-${L.id}: merge ${IB}" -m "${COAUTH}"\`. Conflicts inside your owned files (brief scope) may be resolved keeping both sides' intent; any conflict in a non-owned file -> \`git -C ${P} merge --abort\` and return CONFLICT listing the files.
4. Rebuild the owning target(s), re-run the lane suites (anchored) and each owning test executable whole (--gtest_brief=1). If something now fails because of the merged code, fix it inside owned files if the fix is small and clearly correct; otherwise return FAILED with details.
5. Append a "Post-merge sync" block (head sha, commands, exit codes, test counts) to ${SDDF}/lane-${L.id}-report.md, \`git add -f\`, commit. Return the structured result (head_sha after commit).`
}

function reviewPrompt(L, sha) {
  const P = poolOf(L)
  return `${hdr(L, 'FRESH ADVERSARIAL REVIEWER')}

ROLE — you did not write this code; assume it is wrong until evidence proves otherwise. You are READ-ONLY on code: never edit source or test files. You MAY build and run tests in ${P} (RULES §1 commands). The ONLY file you write is ${P}\\${SDD}\\lane-${L.id}-review.md (TEMPLATES.md "Review" format: Verdict, Reviewed SHA, Evidence, Findings table path:line | severity | problem | required fix, Checked list), committed via \`git -C ${P} add -f\` + commit (body ending "${COAUTH}").
Review target: lane head ${sha}; lane changes = \`git -C ${P} diff ${IB}...HEAD\`; the lane report ${SDDF}/lane-${L.id}-report.md.
Check and record each:
1. .agents/cpp/agent.md §10 checklist on the diff: UB, bounds, narrowing, lifetimes, error paths, exhaustive switches, contracts, /W4 /WX cleanliness.
2. EVERY plan acceptance item in the brief against REAL output: re-run the tests that claim it (anchored -Ctest -R or the exe with --gtest_filter) AND read the test code — does it actually prove the item (right fixture, right tolerance, non-vacuous, would fail on the old code)?
3. EVERY cited defect ID: really closed at the cited file:line (read the code), or explicitly and justifiably deferred?
4. File ownership: \`git -C ${P} diff --name-only ${IB}...HEAD\` must be within the brief scope (+ new test files + the lane's sdd files).
5. No acceptance or existing test weakened: inspect diffs of pre-existing test files — loosened tolerances, deleted assertions, DISABLED_/GTEST_SKIP additions, expectation changes not tied to a cited defect ID.
6. Changed numeric behaviour keeps the old behaviour behind a versioned enum; golden-digest re-baselines carry an old->new table tied to defect IDs.
7. Causality-harness registration where RULES.md requires it (from W1 on).
8. The work is real: wired into the path the plan names, not a stub or a toy; the owning test executables pass whole (run them, --gtest_brief=1).
Severity: blocker = wrong result / UB / the leak or defect still present / acceptance claimed MET but not proven / test weakened / ownership breach; major = significant correctness or test gap, or an UNMET acceptance item that is achievable within the lane scope; minor = small issue. An item that is genuinely infeasible in this repo and honestly reported goes in waiver_needed, not as a blocker. Verdict APPROVE only with zero blocker and zero major findings. Correctness-scoped only — no style or formatting nits.`
}

function fixPrompt(L, n, findings) {
  const P = poolOf(L)
  const list = findings
    ? JSON.stringify(findings, null, 2)
    : `(read them from the Findings table of ${P}\\${SDD}\\lane-${L.id}-review.md — the latest verdict section)`
  return `${hdr(L, 'FIXER (fix pass ' + n + ')')}

TASK — fix pass ${n} for the reviewer's findings (full review: ${P}\\${SDD}\\lane-${L.id}-review.md). Findings to address:
${list}
Fix every blocker and major; fix minors when in scope and cheap, else state why not. Owned files only. Never weaken tests. Rebuild, re-run the lane suites (anchored) and each owning executable whole. Add a "Fix pass ${n}" section to ${SDDF}/lane-${L.id}-report.md (per finding: what changed, evidence) and commit (body ending "${COAUTH}"). Tree clean at the end. Return the structured result.`
}

function rereviewPrompt(L, n, prevSha, sha, findings) {
  const P = poolOf(L)
  const list = findings
    ? JSON.stringify(findings, null, 2)
    : `(the findings in the Findings table of ${P}\\${SDD}\\lane-${L.id}-review.md — the latest verdict section)`
  return `${hdr(L, 'FRESH FIX-ONLY RE-REVIEWER (round ' + n + ')')}

ROLE — adversarial and READ-ONLY on code (you may build/run tests). Verify ONLY that each finding below is correctly fixed at lane head ${sha}, and that the fix commits (\`git -C ${P} log --oneline ${prevSha}..HEAD\`, \`git -C ${P} diff ${prevSha} HEAD\`) introduce no regression and weaken no test. Re-run the affected tests and the owning executables whole. Findings under re-review:
${list}
Append a "## Re-review ${n}" section (verdict, per-finding FIXED/NOT FIXED with evidence) to ${P}\\${SDD}\\lane-${L.id}-review.md; commit it (git add -f; body ending "${COAUTH}"). Verdict APPROVE only if every blocker/major is fixed and nothing regressed. Report new findings only if the fix itself introduced them. Return the structured result (findings = only still-open or newly-introduced issues).`
}

function mergePrompt(L, sha, rounds, summary) {
  const P = poolOf(L)
  return `You are the integration merger (orchestrator delegate) for the atx alpha-engine swarm, wave ${WU}.
Integration worktree: ${INTEG}, branch ${IB}. SAFETY: never touch C:\\atx (another session's live checkout; your shell cwd resets to C:\\atx after every call, so always use \`git -C ${INTEG}\` / \`git -C ${P}\` and absolute paths). Never touch main. Never push. Never resolve merge conflicts yourself.
Merge lane ${WU}-${ID(L)} ("${L.title}"): branch ${L.branch} in ${P}, expected head ${sha}.
1. Confirm \`git -C ${INTEG} status --porcelain\` is empty and \`git -C ${INTEG} rev-parse --abbrev-ref HEAD\` is ${IB}. Confirm \`git -C ${P} status --porcelain\` is empty and read \`git -C ${P} rev-parse HEAD\` (if it differs from ${sha}, merge the actual head and say so in notes). Confirm the lane branch contains ${SDDF}/lane-${L.id}-report.md and lane-${L.id}-review.md.
2. \`git -C ${INTEG} merge --no-ff ${L.branch} -m "${W}: merge lane ${ID(L)} (${L.title})" -m "${summary.replace(/"/g, "'").replace(/`/g, "'").slice(0, 400)}" -m "${COAUTH}"\`. On ANY conflict: \`git -C ${INTEG} merge --abort\` and return CONFLICT with the file list.
3. Edit ${INTEG}\\${SDD}\\progress.md: in the Lanes table set lane ${ID(L)}'s State cell to "merged @ <merge sha 8 chars>; review APPROVE after ${rounds} fix round(s)"; append one line to the end of the Log section: "- <UTC now> — ${ID(L)} merged @ <merge sha 8> (lane head <lane sha 8>): <=1-line outcome>". \`git -C ${INTEG} add -f ${SDDF}/progress.md\`; commit "${W}: progress — ${ID(L)} merged" with body "${COAUTH}".
4. Release the lane lease: \`Set-Location ${INTEG}; powershell -NoProfile -File scripts\\lease-worktree.ps1 -Release pool-${L.pool} -RunId aes-${W}-${L.id}\` and capture its output (a "HEAD is now at" line on stderr is normal). If it refuses, do NOT force or use -RecoverStale; report it.
Return the structured result (integration_sha = \`git -C ${INTEG} rev-parse HEAD\` after step 3).`
}

// ---- orchestration primitives ----
let mergeChain = Promise.resolve()
function locked(fn) {
  const p = mergeChain.then(fn)
  mergeChain = p.catch(() => {})
  return p
}
let slots = A.maxLanes || 5
const waiters = []
async function acquire() {
  if (slots > 0) { slots -= 1; return }
  await new Promise((r) => waiters.push(r))
}
function release() {
  const w = waiters.shift()
  if (w) w(); else slots += 1
}
const mergedGate = {}
const mergedResolve = {}
for (const L of A.lanes) mergedGate[L.id] = new Promise((r) => { mergedResolve[L.id] = r })

function hardProblems(f) { return (f || []).filter((x) => x.severity === 'blocker' || x.severity === 'major') }

async function runLane(L) {
  const res = { id: L.id, pool: L.pool, branch: L.branch, state: 'STARTED' }
  await acquire()
  try {
    const start = L.start || 'implement'
    let sha = L.reviewedSha || ''
    let summary = L.summary || ''
    let rounds = L.rounds || 0
    let open = null
    let approved = start === 'final-sync'

    if (start === 'implement') {
      const impl = await agent(implPrompt(L), { label: `${L.id}:implement`, phase: 'Implement', schema: IMPL_SCHEMA, effort: 'high', agentType: 'general-purpose' })
      res.impl = impl
      if (!impl) { res.state = 'IMPL_DIED'; log(`${L.id}: implementer returned nothing`); return res }
      log(`${L.id}: implement ${impl.outcome} @ ${String(impl.head_sha).slice(0, 8)} — ${impl.targets_green}`)
      if (impl.outcome === 'BLOCKED') { res.state = 'IMPL_BLOCKED'; return res }
      summary = impl.summary

      const s1 = await agent(syncPrompt(L, 'pre-merge before review'), { label: `${L.id}:sync-pre-review`, phase: 'Sync', schema: SYNC_SCHEMA, effort: 'medium', model: 'sonnet', agentType: 'general-purpose' })
      res.syncPre = s1
      if (!s1 || s1.status === 'CONFLICT' || s1.status === 'FAILED') { res.state = 'SYNC_FAILED'; return res }
      sha = s1.head_sha || impl.head_sha

      const review = await agent(reviewPrompt(L, sha), { label: `${L.id}:review`, phase: 'Review', schema: REVIEW_SCHEMA, effort: 'high', agentType: 'general-purpose' })
      res.reviews = [review]
      if (!review) { res.state = 'REVIEW_DIED'; return res }
      log(`${L.id}: review ${review.verdict} (${review.findings.length} findings, ${hardProblems(review.findings).length} blocker/major)`)
      open = review.findings
      approved = review.verdict === 'APPROVE' && review.ownership_ok && !review.tests_weakened
    }

    if (start === 'implement' || start === 'fix') {
      res.fixes = []
      res.reviews = res.reviews || []
      // start==='fix': findings come from the committed review file (open === null)
      let pending = start === 'fix' ? true : open.length > 0
      while (pending && rounds < 2) {
        if (approved && hardProblems(open).length === 0 && rounds >= 1) break
        rounds += 1
        const prevSha = sha
        const fix = await agent(fixPrompt(L, rounds, open), { label: `${L.id}:fix-${rounds}`, phase: 'Fix', schema: FIX_SCHEMA, effort: 'high', agentType: 'general-purpose' })
        res.fixes.push(fix)
        if (!fix) { res.state = 'FIX_DIED'; return res }
        sha = fix.head_sha || sha
        const rr = await agent(rereviewPrompt(L, rounds, prevSha, sha, open), { label: `${L.id}:re-review-${rounds}`, phase: 'Fix', schema: REVIEW_SCHEMA, effort: 'high', agentType: 'general-purpose' })
        res.reviews.push(rr)
        if (!rr) { res.state = 'REREVIEW_DIED'; return res }
        log(`${L.id}: re-review ${rounds} ${rr.verdict} (${hardProblems(rr.findings).length} blocker/major open)`)
        approved = rr.verdict === 'APPROVE' && rr.ownership_ok && !rr.tests_weakened
        open = rr.findings
        pending = open.length > 0
        if (approved && hardProblems(open).length === 0) break
      }
      if (!approved || hardProblems(open).length > 0) {
        res.state = 'BLOCKED_AFTER_FIXES'
        res.open = open
        res.fix_rounds = rounds
        log(`${L.id}: BLOCKED after ${rounds} fix pass(es) — not merged`)
        return res
      }
    }
    res.fix_rounds = rounds

    for (const dep of (L.mergeAfter || [])) {
      if (mergedGate[dep]) {
        const d = await mergedGate[dep]
        log(`${L.id}: merge gate ${dep} passed (${d})`)
      }
    }
    const s2 = await agent(syncPrompt(L, 'final sync before the orchestrator merge; integration may have moved'), { label: `${L.id}:sync-final`, phase: 'Sync', schema: SYNC_SCHEMA, effort: 'medium', model: 'sonnet', agentType: 'general-purpose' })
    res.syncFinal = s2
    if (!s2 || s2.status === 'CONFLICT' || s2.status === 'FAILED') { res.state = 'FINAL_SYNC_FAILED'; return res }
    sha = s2.head_sha || sha

    const m = await locked(() => agent(mergePrompt(L, sha, rounds, `${summary}`.slice(0, 400)), { label: `${L.id}:merge`, phase: 'Merge', schema: MERGE_SCHEMA, effort: 'medium', model: 'sonnet', agentType: 'general-purpose' }))
    res.merge = m
    res.state = m && m.status === 'MERGED' ? 'MERGED' : 'MERGE_FAILED'
    log(`${L.id}: ${res.state} ${m ? String(m.integration_sha).slice(0, 8) : ''}`)
    return res
  } finally {
    release()
    mergedResolve[L.id](res.state)
  }
}

const results = await parallel(A.lanes.map((L) => () => runLane(L)))
return results.map((r, i) => r || { id: A.lanes[i].id, state: 'LANE_THREW' })
