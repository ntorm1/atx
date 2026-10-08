// Isolated browser regression fixture. This is never used by normal launches.
import { EventEmitter } from "node:events";
import { createHash } from "node:crypto";
import { createApp } from "../gateway/app.mjs";
let reads = 0;
let healthStatus = "SERVING";
let workspaces = new Map();
let workspaceCounter = 0;
let workspaceLoadDelay = 0;
let workspaceSaveDelay = 0;
let alphas = new Map();
let alphaCounter = 0;
let experiments = new Map();
let reviews = new Map();
let reviewDecisions = new Map();
let workflows = new Map();
let workflowRuns = new Map();
let workflowLoseResponse = false;
let fixturePrincipal = {
  id: "local",
  name: "Fixture operator",
  role: "admin",
  expires_at_ms: "0",
};
let fixtureTeamMode = false;
const rpcError = (code, details) =>
  Object.assign(new Error(details), { code, details });
const jobs = ["a", "b"].map((letter, i) => {
  const value = i ? 0.35 : 0.125;
  return {
    id: letter.repeat(32),
    status: "succeeded",
    label: i ? "" : "Fixture A",
    request_json: JSON.stringify({
      mode: "synthetic_demo",
      strategy_id: "synthetic-long-short-v1",
    }),
    result_json: JSON.stringify({
      schema_version: 1,
      strategy_id: "synthetic-long-short-v1",
      metrics: { total_return: value, sessions: 10 },
      equity_curve: [
        { date: "2020-01-01", value: 100 },
        { date: "2020-01-02", value: 100 * (1 + value) },
      ],
      drawdown: [{ date: "2020-01-01", value: 0 }],
      returns_histogram: [{ lower: 0, upper: 0.01, count: 10 }],
      monthly_returns: [{ month: "2020-01", value }],
      trades: [],
      positions: [],
      factor_exposures: [],
      provenance: {
        synthetic: true,
        evaluation: "synthetic_test",
        data_source: "Browser test fixture; not research evidence",
        warnings: [],
      },
    }),
    progress: 1,
    error: "",
    created_at_ms: "1700000000000",
    scheduled_at_ms: "0",
    started_at_ms: "1700000000000",
    finished_at_ms: "1700000000001",
  };
});
const databaseColumns = [
  { name: "id", type: "BIGINT" },
  { name: "amount", type: "DECIMAL(38, 12)" },
  { name: "note", type: "VARCHAR" },
];
const cell = (text, is_null = false, truncated = false) => ({
  text,
  is_null,
  truncated,
});
const databaseResult = (source_id, page = false) => ({
  source_id,
  columns: databaseColumns,
  rows: [
    {
      cells: [
        cell(page ? "200" : "9223372036854775807"),
        cell("12345678901234567890.123456789012"),
        cell("", true),
      ],
    },
    { cells: [cell("2"), cell("0.000000000001"), cell("NULL")] },
  ],
  has_more: !page,
  truncated: false,
  elapsed_ms: "3",
  warnings: [],
});
const native = {
  async unary(method, body) {
    if (method === "listWorkflows")
      return {
        workflows: [...workflows.values()]
          .slice(body.offset, body.offset + body.limit)
          .map(({ request_json, ...record }) => ({
            ...record,
            request_json: "",
          })),
        total: String(workflows.size),
      };
    if (method === "getWorkflow") {
      const record = workflows.get(body.id);
      if (!record) throw rpcError(5, "Workflow does not exist");
      return record;
    }
    if (method === "listWorkflowRuns") {
      const list = workflowRuns.get(body.id) ?? [];
      return {
        jobs: list.slice(body.offset, body.offset + body.limit),
        total: String(list.length),
      };
    }
    if (method === "createWorkflow") {
      const id = createHash("md5").update(body.idempotency_key).digest("hex");
      if (workflows.has(id)) {
        const previous = workflows.get(id);
        if (previous.first_run_at_ms !== body.first_run_at_ms)
          throw rpcError(10, "Idempotent first slot changed");
        return previous;
      }
      const alpha = body.alpha_id
        ? alphas
            .get(body.alpha_id)
            ?.find((record) => record.revision === body.alpha_revision)
        : null;
      const recipe = body.request_json || alpha?.request_json;
      if (!recipe) throw rpcError(3, "Frozen recipe required");
      const record = {
        id,
        name: body.name,
        description: body.description,
        request_json: recipe,
        request_sha256: createHash("sha256").update(recipe).digest("hex"),
        alpha_id: body.alpha_id ?? "",
        alpha_revision: body.alpha_revision ?? "0",
        actor_id: fixturePrincipal.id,
        interval_ms: body.interval_ms,
        max_runs: body.max_runs,
        first_run_at_ms: body.first_run_at_ms,
        next_run_at_ms: body.first_run_at_ms,
        revision: "1",
        enabled: true,
        state: "active",
        pause_reason: "",
        last_dispatch_error: "",
        created_at_ms: String(Date.now()),
        updated_at_ms: String(Date.now()),
        admitted_runs: "0",
        queued: "0",
        running: "0",
        succeeded: "0",
        failed: "0",
        cancelled: "0",
        missed_slots: "0",
        skipped_slots: "0",
        active_job_id: "",
        last_job_id: "",
      };
      workflows.set(id, record);
      if (workflowLoseResponse) {
        workflowLoseResponse = false;
        throw rpcError(14, "Fixture response lost after durable admission");
      }
      return record;
    }
    if (method === "setWorkflowPaused") {
      const record = workflows.get(body.id);
      if (!record) throw rpcError(5, "Workflow does not exist");
      if (body.expected_revision !== record.revision)
        throw rpcError(10, "Workflow revision changed; composer kept");
      const changed = {
        ...record,
        enabled: !body.paused,
        state: body.paused ? "paused" : "active",
        revision: String(Number(record.revision) + 1),
        pause_reason: body.paused ? "operator_paused" : "",
      };
      workflows.set(record.id, changed);
      return changed;
    }
    if (method === "cancelJob") {
      const job = jobs.find((record) => record.id === body.id);
      if (!job) throw rpcError(5, "Job does not exist");
      job.status = "cancelled";
      return job;
    }
    if (method === "getSession")
      return {
        principal: fixturePrincipal,
        team_mode: fixtureTeamMode,
        team_name: "Fixture team",
        project_id: "local",
      };
    if (method === "listTeam")
      return {
        principals: [
          fixturePrincipal,
          {
            id: "author",
            name: "Original author",
            role: "researcher",
            expires_at_ms: "0",
          },
        ],
        team_mode: fixtureTeamMode,
        team_name: "Fixture team",
        project_id: "local",
      };
    if (method === "listAudit")
      return {
        records: [...reviews.values()].map((review, i) => ({
          sequence: String(i + 1),
          entity_type: "review",
          entity_id: review.id,
          action: "review.submitted",
          actor_id: review.requester_actor_id,
          revision: "1",
          metadata_json: JSON.stringify({ alpha_id: review.alpha_id }),
          created_at_ms: review.created_at_ms,
        })),
        total: String(reviews.size),
      };
    if (method === "submitReview") {
      const alpha = alphas
        .get(body.alpha_id)
        ?.find((record) => record.revision === body.alpha_revision);
      if (!alpha) throw rpcError(5, "Alpha revision does not exist");
      const id = createHash("md5").update(body.idempotency_key).digest("hex");
      if (reviews.has(id)) return reviews.get(id);
      const record = {
        id,
        alpha_id: body.alpha_id,
        alpha_revision: body.alpha_revision,
        request_sha256: alpha.request_sha256,
        requester_actor_id: fixturePrincipal.id,
        author_actor_id: "author",
        state: "submitted",
        revision: "1",
        note: body.note,
        evidence_json: JSON.stringify({
          schema_version: 1,
          job_ids: body.job_ids,
          recipe_sha256: alpha.request_sha256,
          qualification: "Fixture only",
        }),
        created_at_ms: String(Date.now()),
        updated_at_ms: String(Date.now()),
        current: true,
      };
      reviews.set(id, record);
      return record;
    }
    if (method === "listReviews") {
      const list = [...reviews.values()].filter(
        (record) => !body.state || record.state === body.state,
      );
      return {
        reviews: list.slice(body.offset, body.offset + body.limit),
        total: String(list.length),
      };
    }
    if (method === "getReview") {
      const record = reviews.get(body.id);
      if (!record) throw rpcError(5, "Review does not exist");
      return record;
    }
    if (method === "listReviewDecisions") {
      const list = reviewDecisions.get(body.id) ?? [];
      return {
        decisions: list.slice(body.offset, body.offset + body.limit),
        total: String(list.length),
      };
    }
    if (method === "decideReview") {
      const record = reviews.get(body.id);
      if (!record) throw rpcError(5, "Review does not exist");
      if (body.expected_revision !== record.revision)
        throw rpcError(10, "Review revision changed; rationale preserved");
      if (
        body.state !== "withdrawn" &&
        fixtureTeamMode &&
        [record.requester_actor_id, record.author_actor_id].includes(
          fixturePrincipal.id,
        )
      )
        throw rpcError(7, "Independent reviewer required");
      const updated = {
        ...record,
        state: body.state,
        revision: String(Number(record.revision) + 1),
        updated_at_ms: String(Date.now()),
      };
      reviews.set(record.id, updated);
      const history = reviewDecisions.get(record.id) ?? [];
      history.push({
        id: `${record.id}-${updated.revision}`,
        review_id: record.id,
        revision: updated.revision,
        state: body.state,
        actor_id: fixturePrincipal.id,
        rationale: body.rationale,
        created_at_ms: updated.updated_at_ms,
      });
      reviewDecisions.set(record.id, history);
      return updated;
    }
    if (method === "listExperiments")
      return {
        experiments: [...experiments.values()]
          .slice(body.offset, body.offset + body.limit)
          .map(({ jobs, ...record }) => record),
        total: String(experiments.size),
      };
    if (method === "getExperiment") {
      const record = experiments.get(body.id);
      if (!record) throw rpcError(5, "Experiment not found");
      return { ...record, jobs: record.jobs.map((job) => ({ ...job })) };
    }
    if (method === "createExperiment") {
      const id = createHash("md5").update(body.idempotency_key).digest("hex");
      if (experiments.has(id)) return experiments.get(id);
      const members = body.simulations.map((member, index) => {
        const alpha = member.alpha_id
          ? alphas
              .get(member.alpha_id)
              ?.find((record) => record.revision === member.alpha_revision)
          : null;
        if (member.alpha_id && !alpha)
          throw rpcError(5, "Alpha revision not found");
        return {
          ...jobs[0],
          id: createHash("md5").update(`${id}/${index}`).digest("hex"),
          label: member.label,
          request_json: member.request_json || alpha.request_json,
          result_json: "",
          status: "queued",
          progress: 0,
          alpha_id: member.alpha_id,
          alpha_revision: member.alpha_revision,
          experiment_id: id,
          scheduled_at_ms: String(body.scheduled_at_ms),
          created_at_ms: String(Date.now()),
        };
      });
      jobs.push(...members);
      const record = {
        id,
        name: body.name,
        description: body.description,
        created_at_ms: String(Date.now()),
        scheduled_at_ms: String(body.scheduled_at_ms),
        status: "queued",
        progress: 0,
        total: members.length,
        queued: members.length,
        running: 0,
        succeeded: 0,
        failed: 0,
        cancelled: 0,
        jobs: members,
      };
      experiments.set(id, record);
      return record;
    }
    if (method === "cancelExperiment") {
      const record = experiments.get(body.id);
      if (!record) throw rpcError(5, "Experiment not found");
      record.jobs.forEach((job) => (job.status = "cancelled"));
      record.status = "cancelled";
      record.cancelled = record.total;
      record.queued = 0;
      record.progress = 1;
      return record;
    }
    if (method === "validateExpression") {
      if (body.dataset_id && body.dataset_sha256 !== "c".repeat(64))
        throw rpcError(
          3,
          "Dataset revision SHA256 does not match registered pin",
        );
      if (body.dataset_id && body.strategy_dsl.includes("open"))
        throw rpcError(
          3,
          "Field open is unavailable in the registered dataset",
        );
      if (body.strategy_dsl.includes("future"))
        throw rpcError(3, "Unsupported function future");
      if (body.strategy_dsl.includes("ts_mean"))
        await new Promise((resolve) => setTimeout(resolve, 600));
      return {
        analysis_json: JSON.stringify({
          valid: true,
          required_lookback: 5,
          fields: ["close"],
          unique_nodes: 3,
          peak_live_slots: 2,
          instruction_count: 3,
          expression_sha256: createHash("sha256")
            .update(body.strategy_dsl)
            .digest("hex"),
          scope: "Fixture syntax validation; not research evidence",
        }),
      };
    }
    if (method === "compareJobs") {
      const selected = body.job_ids.map((id) =>
        jobs.find((job) => job.id === id),
      );
      if (selected.some((job) => !job || job.status !== "succeeded"))
        throw rpcError(3, "Comparison requires successful runs");
      return {
        comparison_json: JSON.stringify({
          schema_version: "atx.research-comparison/v1",
          alignment: "intersection_of_exact_return_periods",
          observations: 10,
          start_date: "2020-01-01",
          end_date: "2020-01-15",
          mixed_evidence: false,
          has_gaps: false,
          chart_downsampled: false,
          correlation: [
            [1, 0.8],
            [0.8, 1],
          ],
          runs: selected.map((job) => {
            const result = JSON.parse(job.result_json);
            return {
              job_id: job.id,
              label: job.label,
              synthetic: true,
              evaluation: "synthetic_test",
              dataset_sha256: "",
              available_observations: 10,
              excluded_observations: 0,
              metrics: {
                ...result.metrics,
                annualized_volatility: 0.15,
                sharpe_ratio: 1.2,
                max_drawdown: -0.05,
              },
              curve: result.equity_curve,
              yearly_returns: [
                { year: "2020", value: result.metrics.total_return },
              ],
            };
          }),
          warnings: ["Fixture comparison; not research evidence"],
        }),
      };
    }
    if (method === "listAlphas") {
      const records = [...alphas.values()]
        .map((revisions) => revisions.at(-1))
        .filter(
          (record) =>
            (body.include_archived || record.stage !== "archived") &&
            (!body.search ||
              record.name.toLowerCase().includes(body.search.toLowerCase())) &&
            (!body.tag || record.tags.includes(body.tag)),
        );
      return {
        alphas: records
          .slice(body.offset, body.offset + body.limit)
          .map(({ request_json, hypothesis, ...record }) => record),
        total: String(records.length),
      };
    }
    if (method === "getAlpha") {
      const revisions = alphas.get(body.id);
      const record =
        body.revision === "0"
          ? revisions?.at(-1)
          : revisions?.find((record) => record.revision === body.revision);
      if (!record) throw rpcError(5, "Alpha revision does not exist");
      return { ...record };
    }
    if (method === "listAlphaRevisions") {
      const revisions = alphas.get(body.id);
      if (!revisions) throw rpcError(5, "Alpha does not exist");
      return {
        revisions: [...revisions]
          .reverse()
          .slice(body.offset, body.offset + body.limit)
          .map(({ request_json, hypothesis, ...record }) => record),
        total: String(revisions.length),
      };
    }
    if (["createAlpha", "updateAlpha", "setAlphaArchived"].includes(method)) {
      const previous = alphas.get(body.id)?.at(-1);
      if (method !== "createAlpha" && !previous)
        throw rpcError(5, "Alpha does not exist");
      if (previous && previous.revision !== body.expected_revision)
        throw rpcError(10, "Alpha revision changed; working changes preserved");
      const record = {
        id:
          previous?.id ??
          createHash("md5")
            .update(`fixture-alpha-${++alphaCounter}`)
            .digest("hex"),
        project_id: "local",
        name: body.name ?? previous.name,
        description: body.description ?? previous.description,
        tags: body.tags ?? previous.tags,
        hypothesis: body.hypothesis ?? previous.hypothesis,
        request_json: body.request_json ?? previous.request_json,
        request_sha256: createHash("sha256")
          .update(body.request_json ?? previous.request_json)
          .digest("hex"),
        source_job_id: body.source_job_id ?? previous.source_job_id,
        stage:
          method === "setAlphaArchived"
            ? body.archived
              ? "archived"
              : "research"
            : (previous?.stage ?? "research"),
        revision: String(Number(previous?.revision ?? 0) + 1),
        created_at_ms: previous?.created_at_ms ?? String(Date.now()),
        updated_at_ms: String(Date.now()),
      };
      const history = alphas.get(record.id) ?? [];
      history.push(record);
      alphas.set(record.id, history);
      return { ...record };
    }
    if (method === "submitJob") {
      const alpha = body.alpha_id
        ? alphas
            .get(body.alpha_id)
            ?.find((record) => record.revision === body.alpha_revision)
        : null;
      if (body.alpha_id && !alpha)
        throw rpcError(5, "Saved alpha revision does not exist");
      const job = {
        ...jobs[0],
        id: createHash("md5").update(body.idempotency_key).digest("hex"),
        label: body.label ?? "",
        request_json: body.request_json || alpha.request_json,
        result_json: "",
        status: "queued",
        progress: 0,
        alpha_id: body.alpha_id ?? "",
        alpha_revision: body.alpha_revision ?? "0",
        created_at_ms: String(Date.now()),
      };
      jobs.push(job);
      return job;
    }
    if (method === "listWorkspaces") {
      const records = [...workspaces.values()];
      return {
        workspaces: records
          .slice(body.offset, body.offset + body.limit)
          .map(({ document_json, ...record }) => record),
        total: String(records.length),
      };
    }
    if (method === "getWorkspace") {
      const record = workspaces.get(body.id);
      if (workspaceLoadDelay)
        await new Promise((resolve) => setTimeout(resolve, workspaceLoadDelay));
      if (!record) throw rpcError(5, "Workspace does not exist");
      return { ...record };
    }
    if (
      ["createWorkspace", "updateWorkspace", "deleteWorkspace"].includes(method)
    ) {
      if (workspaceSaveDelay)
        await new Promise((resolve) => setTimeout(resolve, workspaceSaveDelay));
      const previous = workspaces.get(body.id);
      if (method !== "createWorkspace" && !previous)
        throw rpcError(5, "Workspace does not exist");
      if (previous && body.expected_revision !== previous.revision)
        throw rpcError(
          10,
          "Workspace changed on the server; reload or save as a copy",
        );
      if (method === "deleteWorkspace") {
        workspaces.delete(body.id);
        return {};
      }
      if (
        [...workspaces.values()].some(
          (w) => w.name === body.name && w.id !== body.id,
        )
      )
        throw rpcError(6, "Workspace name already exists");
      const record = {
        id: previous?.id ?? `workspace-${++workspaceCounter}`,
        name: body.name,
        description: body.description,
        document_json: body.document_json,
        revision: String(Number(previous?.revision ?? 0) + 1),
        created_at_ms: previous?.created_at_ms ?? String(Date.now()),
        updated_at_ms: String(Date.now()),
      };
      workspaces.set(record.id, record);
      return { ...record };
    }
    if (method === "listDataSources")
      return {
        sources: [
          {
            id: "fixture-db",
            name: "Fixture database",
            provider: "duckdb",
            read_only: true,
            capabilities: {
              browse: true,
              query: true,
              max_rows: 500,
              max_offset: 1000000,
              max_query_bytes: 65536,
              timeout_ms: 5000,
            },
          },
          {
            id: "archive",
            name: "Fixture archive",
            provider: "postgresql",
            read_only: true,
            capabilities: {
              browse: true,
              query: true,
              max_rows: 25,
              max_offset: 1000000,
              max_query_bytes: 65536,
              timeout_ms: 5000,
            },
          },
        ],
      };
    if (method === "getDataCatalog")
      return {
        source_id: body.source_id,
        truncated: false,
        tables: [
          {
            catalog: "fixture",
            schema: "main",
            name: "prices",
            kind: "BASE TABLE",
          },
          {
            catalog: "fixture",
            schema: "main",
            name: "trades",
            kind: "BASE TABLE",
          },
          { catalog: "fixture", schema: "audit", name: "events", kind: "VIEW" },
        ],
      };
    if (method === "previewData")
      return databaseResult(body.source_id, body.offset > 0);
    if (method === "queryData") {
      if (!body.sql.trim().toUpperCase().startsWith("SELECT"))
        throw Object.assign(new Error("Only read-only SELECT is allowed"), {
          code: 3,
          details: "Only read-only SELECT is allowed",
        });
      if (body.sql.includes("delayed"))
        await new Promise((resolve) => setTimeout(resolve, 600));
      if (body.sql.includes("virtual"))
        return {
          source_id: body.source_id,
          columns: Array.from({ length: 256 }, (_, i) => ({
            name: `column_${i}`,
            type: "VARCHAR",
          })),
          rows: Array.from(
            { length: Math.min(500, body.max_rows) },
            (_, row) => ({
              cells: Array.from({ length: 256 }, (_, column) =>
                cell(`row_${row}_column_${column}`),
              ),
            }),
          ),
          has_more: false,
          truncated: false,
          elapsed_ms: "4",
          warnings: [],
        };
      return databaseResult(body.source_id);
    }
    if (method === "getHealth")
      return { status: healthStatus, cache_hits: String(reads) };
    if (method === "listStrategies")
      return {
        strategies_json: JSON.stringify({
          strategies: [
            {
              id: "synthetic-long-short-v1",
              name: "Fixture strategy",
              mode: "synthetic_demo",
              synthetic: true,
            },
            {
              id: "real-data-long-short-v1",
              name: "Fixture TRAIN market-data alpha",
              mode: "real_dsl",
              synthetic: false,
              enabled: true,
            },
          ],
          datasets: [
            {
              id: "fixture-train",
              name: "Fixture TRAIN dataset",
              manifest_sha256: "c".repeat(64),
              evaluation: "TRAIN",
              start_date: "2018-01-01",
              end_date: "2022-12-31",
              fields: [
                "close",
                "raw_close",
                "volume",
                "returns",
                "dollar_volume",
                "adv63",
              ],
              max_instruments: 512,
              historical_vintage_verified: false,
              common_stock_history_verified: false,
              field_contract: { close: "Fixture data, not evidence" },
            },
          ],
          operators: {
            operators: [
              {
                name: "rank",
                category: "cross-sectional",
                example: "rank(close)",
                description: "Ranks available observations",
                window_argument: false,
                causal: true,
              },
              {
                name: "delta",
                category: "time-series",
                example: "delta(close,5)",
                description: "Causal trailing difference",
                window_argument: true,
                causal: true,
              },
              {
                name: "ts_mean",
                category: "time-series",
                example: "ts_mean(close,20)",
                description: "Trailing mean",
                window_argument: true,
                causal: true,
              },
            ],
            limits: { max_bytes: 1024, max_window: 252 },
            arithmetic: "+ - * / ^",
            missing_values: "No implicit zero fill",
          },
        }),
      };
    if (method === "listJobs")
      return {
        jobs: jobs
          .filter((job) => !body.status || job.status === body.status)
          .slice(body.offset, body.offset + body.limit)
          .map((j) => ({ ...j, request_json: "", result_json: "" })),
        total: String(
          jobs.filter((job) => !body.status || job.status === body.status)
            .length,
        ),
      };
    if (method === "getJob") {
      reads++;
      return jobs.find((j) => j.id === body.id);
    }
    throw new Error("Unsupported fixture request");
  },
  watch() {
    return Object.assign(new EventEmitter(), { cancel() {} });
  },
};
// Regression pages intentionally share loopback and exceed a normal user's budget.
const app = await createApp({ native, requestsPerMinute: 100000 });
app.post("/fixture/health", async (req) => {
  healthStatus = req.body.status;
  return { status: healthStatus };
});
app.post("/fixture/workspaces", async (req) => {
  if (req.body.reset) {
    workspaces.clear();
    workspaceCounter = 0;
  }
  workspaceLoadDelay = req.body.load_delay_ms ?? 0;
  workspaceSaveDelay = req.body.save_delay_ms ?? 0;
  return { total: workspaces.size };
});
app.post("/fixture/library", async () => {
  alphas.clear();
  alphaCounter = 0;
  jobs.splice(2);
  experiments.clear();
  return { total: 0 };
});
app.post("/fixture/team", async (req) => {
  fixtureTeamMode = req.body.team_mode ?? false;
  fixturePrincipal = {
    id: req.body.actor_id ?? "local",
    name: req.body.name ?? "Fixture operator",
    role: req.body.role ?? "admin",
    expires_at_ms: String(req.body.expires_at_ms ?? 0),
  };
  if (req.body.seed) {
    alphas.clear();
    reviews.clear();
    reviewDecisions.clear();
    jobs.splice(2);
    const recipe = JSON.stringify({
      mode: "synthetic_demo",
      strategy_id: "synthetic-long-short-v1",
      strategy_dsl: "rank(close)",
    });
    const alpha = {
      id: "d".repeat(32),
      project_id: "local",
      name: "Review fixture alpha",
      description: "",
      tags: [],
      hypothesis: "Fixture only",
      request_json: recipe,
      request_sha256: createHash("sha256").update(recipe).digest("hex"),
      source_job_id: "",
      stage: "research",
      revision: "1",
      created_at_ms: String(Date.now()),
      updated_at_ms: String(Date.now()),
    };
    alphas.set(alpha.id, [alpha]);
    jobs.push({
      ...jobs[0],
      id: "e".repeat(32),
      label: "Linked fixture evidence",
      alpha_id: alpha.id,
      alpha_revision: "1",
      request_json: recipe,
    });
  }
  return fixturePrincipal;
});
app.post("/fixture/review-conflict", async () => {
  for (const [id, record] of reviews)
    reviews.set(id, {
      ...record,
      revision: String(Number(record.revision) + 1),
    });
  return { total: reviews.size };
});
app.post("/fixture/workflows", async (req) => {
  if (req.body.reset) {
    workflows.clear();
    workflowRuns.clear();
    workflowLoseResponse = false;
  }
  if (req.body.lose_response) workflowLoseResponse = true;
  if (req.body.seed) {
    const recipe = jobs[0].request_json;
    const record = {
      id: "f".repeat(32),
      name: "Fixture recurring workflow",
      description: "",
      request_json: recipe,
      request_sha256: createHash("sha256").update(recipe).digest("hex"),
      alpha_id: "",
      alpha_revision: "0",
      actor_id: "local",
      interval_ms: "3600000",
      max_runs: 3,
      first_run_at_ms: String(Date.now()),
      next_run_at_ms: String(Date.now() + 3600000),
      revision: "1",
      enabled: true,
      state: "active",
      pause_reason: "",
      last_dispatch_error: "",
      created_at_ms: String(Date.now()),
      updated_at_ms: String(Date.now()),
      admitted_runs: "0",
      queued: "0",
      running: "0",
      succeeded: "0",
      failed: "0",
      cancelled: "0",
      missed_slots: "0",
      skipped_slots: "0",
      active_job_id: "",
      last_job_id: "",
    };
    workflows.set(record.id, record);
  }
  if (req.body.conflict)
    for (const [id, record] of workflows)
      workflows.set(id, {
        ...record,
        revision: String(Number(record.revision) + 1),
      });
  if (req.body.active_id) {
    const record = workflows.get(req.body.active_id);
    const job = {
      ...jobs[0],
      id: createHash("md5").update(`workflow:${record.id}`).digest("hex"),
      status: "running",
      result_json: "",
      request_json: record.request_json,
      workflow_id: record.id,
      workflow_slot_ms: record.first_run_at_ms,
    };
    jobs.push(job);
    workflowRuns.set(record.id, [job]);
    workflows.set(record.id, {
      ...record,
      active_job_id: job.id,
      last_job_id: job.id,
      admitted_runs: "1",
      running: "1",
    });
  }
  return { total: workflows.size };
});
await app.listen({ host: "127.0.0.1", port: 3002 });
const stop = async () => {
  await app.close();
  process.exit(0);
};
process.once("SIGINT", stop);
process.once("SIGTERM", stop);
if (process.platform === "win32") process.once("SIGBREAK", stop);
