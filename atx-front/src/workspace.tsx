import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
} from "react";
import {
  DockviewReact,
  type DockviewApi,
  type DockviewReadyEvent,
  type IDockviewPanelProps,
  type SerializedDockview,
  Orientation,
} from "dockview-react";
import { ApiError, events, request } from "./api";
import { DatabasePanel } from "./database";
import { AlphaLibrary } from "./alpha-library";
import { ComparisonPanel } from "./comparison";
import { SessionContext, usePermissions, useSession } from "./session";
import { TeamPanel } from "./team";
import { ReviewsPanel } from "./reviews";
import { PortfolioPanel, portfolioRequest } from "./portfolio";
import {
  WorkflowsPanel,
  emptyWorkflowDraft,
  type WorkflowDraft,
} from "./workflows";
import {
  ExperimentsPanel,
  emptyExperimentDraft,
  type ExperimentDraft,
} from "./experiments";
import {
  ExpressionEditor,
  OperatorsPanel,
  RealDatasetFields,
  realDatasetRequest,
} from "./research-tools";
import {
  useNamedWorkspaces,
  WorkspaceSelector,
  WorkspaceMessages,
  type WorkspacePorts,
} from "./named-workspaces";
import {
  emptyDatabaseContext,
  validateWorkspaceDocument,
  type DatabaseContext,
  type LayoutPreset,
} from "./workspace-document";
import {
  Empty,
  Heatmap,
  Histogram,
  LineChart,
  number,
  percent,
} from "./charts";
import type {
  Health,
  Job,
  ResearchRequest,
  Strategy,
  WorkspaceEvent,
  Dataset,
  OperatorCatalog,
} from "./types";

const initial: ResearchRequest = {
  mode: "synthetic_demo",
  strategy_id: "synthetic-long-short-v1",
  strategy_dsl: "rank(delta(close,5))",
  sessions: 504,
  initial_capital: 1000000,
  trade_bps: 6,
  annual_borrow_bps: 300,
};
const strategyRequest = (
  s: Strategy,
  datasets: Dataset[] = [],
): ResearchRequest =>
  s.mode === "synthetic_demo"
    ? { ...initial, strategy_id: s.id }
    : s.mode === "real_dsl"
      ? realDatasetRequest(s.id, datasets[0])
      : s.mode === "real_portfolio"
        ? portfolioRequest(s.id, datasets[0])
        : s.mode === "real_nav"
          ? {
              mode: "real_nav",
              strategy_id: s.id,
              combined_path: "",
              combined_sha256: "",
              role_path: "",
              role_sha256: "",
              rule: "baseline-v1",
              cadence: 5,
              trade_fraction: 0.25,
              monthly_budget: 0.3,
            }
          : { mode: "import_nav", strategy_id: s.id, artifact_path: "" };
const titles: Record<string, string> = {
  workspace: "Workspace",
  configuration: "Draft configuration",
  editor: "Draft request",
  database: "Database viewer",
  metrics: "Results summary",
  equity: "Equity curve",
  drawdown: "Drawdown",
  distribution: "Return distribution",
  monthly: "Monthly returns (%)",
  jobs: "Jobs / scheduler",
  trades: "Trades",
  positions: "Positions",
  factors: "Factor analysis",
  provenance: "Provenance",
  logs: "Message channels",
  operators: "Alpha operators",
  alphas: "Alpha library",
  comparison: "Run comparison",
  experiments: "Experiments",
  team: "Team / audit",
  reviews: "Research reviews",
  portfolio: "Portfolio construction",
  workflows: "Recurring workflows",
};
const terminal = (status: string) =>
  ["succeeded", "failed", "cancelled"].includes(status);
const date = (ms: string | number) =>
  Number(ms) ? new Date(Number(ms)).toLocaleString() : "—";
type State = {
  token: string;
  databaseContext: DatabaseContext;
  panelEpoch: number;
  setDatabaseContext: (
    context: DatabaseContext,
    userChanged: boolean,
    ownerEpoch: number,
  ) => void;
  health: Health | null;
  strategies: Strategy[];
  datasets: Dataset[];
  operatorCatalog: OperatorCatalog | null;
  jobs: Job[];
  total: number;
  selected: Job | null;
  select: (job: Job) => void;
  draft: ResearchRequest;
  setDraft: (r: ResearchRequest) => void;
  editorText: string;
  setEditorText: (text: string) => void;
  runAlpha: (id: string, revision: string, label: string) => Promise<void>;
  setAlphaUnsaved: (dirty: boolean) => void;
  experimentDraft: ExperimentDraft;
  setExperimentDraft: (draft: ExperimentDraft) => void;
  workflowDraft: WorkflowDraft;
  setWorkflowDraft: (draft: WorkflowDraft) => void;
  submit: (schedule: string) => Promise<void>;
  cancel: (id: string) => Promise<void>;
  busy: boolean;
  error: string;
  logs: WorkspaceEvent[];
  filter: string;
  setFilter: (f: string) => void;
  offset: number;
  setOffset: (o: number) => void;
  refresh: () => Promise<void>;
};
const Workspace = createContext<State | null>(null);
const useWorkspace = () => useContext(Workspace)!;
function JobBadge({ status }: { status: string }) {
  return <span className={`badge status-${status}`}>{status}</span>;
}
function DataTable({
  rows,
  columns,
}: {
  rows: Record<string, unknown>[];
  columns?: string[];
}) {
  const [filter, setFilter] = useState("");
  if (!rows.length)
    return <Empty>No observations supplied for this result.</Empty>;
  const keys = columns ?? Object.keys(rows[0]);
  const visible = rows.filter((row) =>
    JSON.stringify(row).toLowerCase().includes(filter.toLowerCase()),
  );
  return (
    <>
      <div className="panel-toolbar">
        <input
          aria-label="Filter rows"
          placeholder="Filter rows…"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
        />
        <span>
          {visible.length} / {rows.length} rows
        </span>
      </div>
      <div className="table-scroll">
        <table>
          <thead>
            <tr>
              {keys.map((k) => (
                <th key={k}>{k.replaceAll("_", " ")}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {visible.slice(0, 1000).map((r, i) => (
              <tr key={i}>
                {keys.map((k) => (
                  <td
                    key={k}
                    className={typeof r[k] === "number" ? "numeric" : ""}
                  >
                    {typeof r[k] === "number"
                      ? number(r[k])
                      : String(r[k] ?? "—")}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
        {visible.length > 1000 && (
          <div className="notice">
            Showing the first 1,000 rows. Filter to narrow the result or export
            the complete document.
          </div>
        )}
      </div>
    </>
  );
}
function BrowserPanel() {
  const { strategies, datasets, jobs, selected, select, draft, setDraft } =
    useWorkspace();
  return (
    <div className="panel-content browser-panel">
      <div className="tree-header">RESEARCH / EQUITY</div>
      <div className="tree-folder">▾ Strategies</div>
      {strategies.length ? (
        strategies.map((s) => (
          <button
            className={`tree-row ${draft.strategy_id === s.id ? "selected" : ""}`}
            key={s.id}
            disabled={s.enabled === false}
            title={s.description}
            onClick={() => setDraft(strategyRequest(s, datasets))}
          >
            <span>▧</span>
            {s.name}
            {s.synthetic && <small>DEMO</small>}
          </button>
        ))
      ) : (
        <div className="muted tree-row">Waiting for catalog…</div>
      )}
      <div className="tree-folder">▾ Run history</div>
      {jobs.slice(0, 30).map((j) => (
        <button
          key={j.id}
          className={`tree-row ${selected?.id === j.id ? "selected" : ""}`}
          onClick={() => select(j)}
          title={j.id}
        >
          <span className={`status-dot status-${j.status}`} />
          <span className="ellipsize">
            {j.label || j.request?.strategy_id || j.id}
          </span>
          <small>{j.status}</small>
        </button>
      ))}
      <div className="tree-folder">▾ Data sources</div>
      {datasets.map((d) => (
        <button
          key={d.id}
          className="tree-row"
          title={`${d.id}\nManifest ${d.manifest_sha256}\n${d.fields.join(", ")}`}
          onClick={() =>
            setDraft(realDatasetRequest("real-data-long-short-v1", d))
          }
        >
          {d.name}
          <small>TRAIN</small>
        </button>
      ))}
      <div className="tree-row muted">▤ Allowed NAV artifacts</div>
      <div className="tree-row muted">▤ Synthetic engine replay</div>
      <div className="properties">
        <div className="tree-header">SELECTED RUN</div>
        <dl>
          <dt>ID</dt>
          <dd title={selected?.id}>{selected?.id ?? "—"}</dd>
          <dt>Status</dt>
          <dd>{selected?.status ?? "—"}</dd>
          <dt>Created</dt>
          <dd>{selected ? date(selected.created_at_ms) : "—"}</dd>
          <dt>Evaluation</dt>
          <dd>
            {selected?.result?.provenance.synthetic
              ? "Synthetic demo"
              : selected?.result
                ? ["real_dsl", "real_portfolio"].includes(
                    selected.request?.mode ?? "",
                  )
                  ? "TRAIN / unverified"
                  : "Imported NAV"
                : "—"}
          </dd>
        </dl>
      </div>
    </div>
  );
}
function RealReplayFields() {
  const { draft, setDraft } = useWorkspace();
  const update = (key: string, value: unknown) =>
    setDraft({ ...draft, [key]: value });
  return (
    <>
      <div className="notice">
        Native saved-blend replay · TRAIN observations · fixed cost scenarios
        and initial NAV. Results remain research-unverified.
      </div>
      {(
        [
          ["combined_path", "Combined artifact"],
          ["combined_sha256", "Combined SHA-256"],
          ["role_path", "Role artifact"],
          ["role_sha256", "Role SHA-256"],
        ] as const
      ).map(([key, label]) => (
        <label key={key}>
          {label}
          <input
            required
            value={draft[key] ?? ""}
            pattern={key.endsWith("sha256") ? "[a-f0-9]{64}" : undefined}
            placeholder={
              key.endsWith("sha256")
                ? "64 lowercase hex characters"
                : "relative/artifact.json"
            }
            onChange={(e) => update(key, e.target.value)}
          />
        </label>
      ))}
      <label>
        Rebalance rule
        <select
          value={draft.rule ?? "baseline-v1"}
          onChange={(e) => update("rule", e.target.value)}
        >
          {["baseline-v1", "monthly-budget-v2", "aim-partial-v5"].map(
            (rule) => (
              <option key={rule}>{rule}</option>
            ),
          )}
        </select>
      </label>
      <label>
        Cadence (sessions)
        <input
          required
          type="number"
          min="1"
          max="252"
          value={draft.cadence ?? 5}
          onChange={(e) => update("cadence", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Trade fraction
        <input
          required
          type="number"
          min="0.001"
          max="1"
          step=".001"
          value={draft.trade_fraction ?? 0.25}
          onChange={(e) => update("trade_fraction", e.target.valueAsNumber)}
        />
      </label>
      <label>
        Monthly budget
        <input
          required
          type="number"
          min="0.001"
          max="2"
          step=".001"
          value={draft.monthly_budget ?? 0.3}
          onChange={(e) => update("monthly_budget", e.target.valueAsNumber)}
        />
      </label>
      {(["start_date", "end_date"] as const).map((key) => (
        <label key={key}>
          {key === "start_date" ? "Start date" : "End date"}
          <input
            type="date"
            value={draft[key] ?? ""}
            onChange={(e) => update(key, e.target.value || undefined)}
          />
        </label>
      ))}
    </>
  );
}
function Configuration() {
  const { canResearch } = usePermissions();
  const { draft, setDraft, strategies, datasets, submit, busy, error } =
    useWorkspace();
  const [schedule, setSchedule] = useState("");
  const update = (key: string, value: unknown) =>
    setDraft({ ...draft, [key]: value });
  return (
    <div className="panel-content">
      <form
        className="configuration"
        onSubmit={(e) => {
          e.preventDefault();
          void submit(schedule);
        }}
      >
        <div className="subtabs">
          <span className="active">General</span>
          <span>
            {draft.mode === "synthetic_demo"
              ? "Synthetic replay"
              : ["real_dsl", "real_portfolio"].includes(draft.mode)
                ? "Observed TRAIN replay"
                : draft.mode === "real_nav"
                  ? "Native replay"
                  : "Artifact import"}
          </span>
        </div>
        <label>
          Strategy
          <select
            value={draft.strategy_id}
            onChange={(e) => {
              const s = strategies.find((x) => x.id === e.target.value);
              if (s) setDraft(strategyRequest(s, datasets));
            }}
          >
            {!strategies.length && (
              <option value={draft.strategy_id}>{draft.strategy_id}</option>
            )}
            {strategies.map((s) => (
              <option key={s.id} value={s.id} disabled={s.enabled === false}>
                {s.name}
              </option>
            ))}
          </select>
        </label>
        {draft.mode === "synthetic_demo" ? (
          <>
            <div className="notice demo-notice">
              SYNTHETIC DEMO · Generated prices. Results are for system
              validation.
            </div>
            <label>
              Alpha expression
              <input
                required
                maxLength={1024}
                value={draft.strategy_dsl ?? "rank(delta(close,5))"}
                onChange={(e) => update("strategy_dsl", e.target.value)}
              />
            </label>
            <label>
              Sessions
              <input
                type="number"
                min="10"
                max="756"
                required
                value={draft.sessions ?? 504}
                onChange={(e) => update("sessions", e.target.valueAsNumber)}
              />
            </label>
            <label>
              Initial capital
              <input
                type="number"
                min="1"
                max="1000000000000"
                step="any"
                required
                value={draft.initial_capital ?? 1000000}
                onChange={(e) =>
                  update("initial_capital", e.target.valueAsNumber)
                }
              />
            </label>
            <label>
              Trading cost (bps)
              <input
                type="number"
                min="0"
                max="100"
                step=".1"
                required
                value={draft.trade_bps ?? 6}
                onChange={(e) => update("trade_bps", e.target.valueAsNumber)}
              />
            </label>
            <label>
              Annual borrow (bps)
              <input
                type="number"
                min="0"
                max="5000"
                required
                value={draft.annual_borrow_bps ?? 300}
                onChange={(e) =>
                  update("annual_borrow_bps", e.target.valueAsNumber)
                }
              />
            </label>
          </>
        ) : ["real_dsl", "real_portfolio"].includes(draft.mode) ? (
          <RealDatasetFields
            draft={draft}
            setDraft={setDraft}
            datasets={datasets}
          />
        ) : draft.mode === "real_nav" ? (
          <RealReplayFields />
        ) : (
          <>
            <div className="notice">
              Import an existing NAV CSV inside the server's configured artifact
              roots.
            </div>
            <label>
              Artifact path
              <input
                required
                value={draft.artifact_path ?? ""}
                placeholder="results/daily_scenario.csv"
                onChange={(e) => update("artifact_path", e.target.value)}
              />
            </label>
            <label>
              Start date
              <input
                type="date"
                value={draft.start_date ?? ""}
                onChange={(e) =>
                  update("start_date", e.target.value || undefined)
                }
              />
            </label>
            <label>
              End date
              <input
                type="date"
                value={draft.end_date ?? ""}
                onChange={(e) =>
                  update("end_date", e.target.value || undefined)
                }
              />
            </label>
            <label>
              Summary path
              <input
                value={draft.summary_path ?? ""}
                placeholder="Optional summary.json"
                onChange={(e) =>
                  update("summary_path", e.target.value || undefined)
                }
              />
            </label>
          </>
        )}
        <label>
          Schedule
          <input
            type="datetime-local"
            value={schedule}
            onChange={(e) => setSchedule(e.target.value)}
          />
        </label>
        <div className="help">
          Blank starts now. Scheduled times use your browser's local timezone.
        </div>
        <div className="form-actions">
          <button
            type="button"
            onClick={() => {
              const strategy = strategies.find(
                (s) => s.id === draft.strategy_id,
              );
              setDraft(
                strategy ? strategyRequest(strategy, datasets) : initial,
              );
              setSchedule("");
            }}
          >
            Defaults
          </button>
          <button
            className="primary"
            type="submit"
            disabled={busy || !canResearch}
          >
            {busy
              ? "Submitting…"
              : schedule
                ? "Schedule run"
                : "▶ Run research"}
          </button>
        </div>
        {error && (
          <div role="alert" className="error">
            {error}
          </div>
        )}
      </form>
    </div>
  );
}
function Editor() {
  const {
    draft,
    setDraft,
    token,
    operatorCatalog,
    editorText: text,
    setEditorText: setText,
  } = useWorkspace();
  const [error, setError] = useState("");
  return (
    <div className="editor">
      <ExpressionEditor
        draft={draft}
        setDraft={setDraft}
        token={token}
        catalog={operatorCatalog}
      />
      <div className="panel-toolbar">
        <span>research-request.json</span>
        <button
          onClick={() => {
            try {
              const d = JSON.parse(text);
              if (
                ![
                  "synthetic_demo",
                  "import_nav",
                  "real_nav",
                  "real_dsl",
                  "real_portfolio",
                ].includes(d.mode) ||
                !d.strategy_id
              )
                throw new Error("Valid mode and strategy_id are required");
              setDraft(d);
              setError("");
            } catch (e) {
              setError((e as Error).message);
            }
          }}
        >
          Apply to configuration
        </button>
      </div>
      <div className="editor-body">
        <div className="line-numbers">
          {text.split("\n").map((_, i) => (
            <div key={i}>{i + 1}</div>
          ))}
        </div>
        <textarea
          aria-label="Research request JSON editor"
          spellCheck={false}
          value={text}
          onChange={(e) => setText(e.target.value)}
        />
      </div>
      {error && (
        <div className="error" role="alert">
          {error}
        </div>
      )}
      <div className="help">
        Apply JSON edits before saving a workspace or submitting the configured
        request.
      </div>
    </div>
  );
}
const metricLabels: [string, string, boolean?][] = [
  ["total_return", "Total return", true],
  ["cagr", "CAGR", true],
  ["annualized_volatility", "Annual volatility", true],
  ["sharpe_ratio", "Sharpe ratio"],
  ["sortino_ratio", "Sortino ratio"],
  ["max_drawdown", "Max drawdown", true],
  ["calmar_ratio", "Calmar ratio"],
  ["win_rate", "Positive sessions", true],
  ["initial_nav", "Initial NAV"],
  ["final_nav", "Final NAV"],
  ["sessions", "Sessions"],
];
function Metrics() {
  const { selected } = useWorkspace();
  const result = selected?.result;
  return (
    <div className="panel-content">
      {selected && (
        <div className="notice" title={selected.id}>
          Selected run {selected.id.slice(0, 12)} / {selected.status}
        </div>
      )}
      {result?.provenance.synthetic && (
        <div className="result-banner">
          SYNTHETIC DEMO — simulated research results
        </div>
      )}
      <table>
        <thead>
          <tr>
            <th>Metric</th>
            <th className="numeric">Strategy</th>
            <th className="numeric">Benchmark</th>
          </tr>
        </thead>
        <tbody>
          {metricLabels.map(([key, label, pct]) => (
            <tr key={key}>
              <td>{label}</td>
              <td
                className={`numeric ${key === "total_return" && typeof result?.metrics[key] === "number" ? (result.metrics[key]! >= 0 ? "positive" : "negative") : ""}`}
              >
                {pct
                  ? percent(result?.metrics[key])
                  : number(result?.metrics[key], key === "sessions" ? 0 : 2)}
              </td>
              <td className="numeric muted">—</td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="help">
        Unavailable fields appear as —. Benchmark data is not supplied.
      </div>
    </div>
  );
}
function Jobs() {
  const { canResearch } = usePermissions();
  const {
    jobs,
    total,
    selected,
    select,
    cancel,
    filter,
    setFilter,
    offset,
    setOffset,
    busy,
    refresh,
  } = useWorkspace();
  return (
    <div className="panel-content">
      <div className="panel-toolbar">
        <select
          aria-label="Filter jobs by status"
          value={filter}
          onChange={(e) => {
            setFilter(e.target.value);
            setOffset(0);
          }}
        >
          <option value="">All statuses</option>
          {["queued", "running", "succeeded", "failed", "cancelled"].map(
            (s) => (
              <option key={s}>{s}</option>
            ),
          )}
        </select>
        <button onClick={() => void refresh()}>↻ Refresh</button>
        <span>{total} runs</span>
        <div className="spacer" />
        <button
          disabled={!offset}
          onClick={() => setOffset(Math.max(0, offset - 100))}
        >
          ‹
        </button>
        <span>
          {offset + (jobs.length ? 1 : 0)}–{offset + jobs.length}
        </span>
        <button
          disabled={offset + 100 >= total}
          onClick={() => setOffset(offset + 100)}
        >
          ›
        </button>
      </div>
      <div className="table-scroll">
        <table>
          <thead>
            <tr>
              <th>Run / strategy</th>
              <th>Status</th>
              <th>Progress</th>
              <th>Scheduled / created</th>
              <th>Action</th>
            </tr>
          </thead>
          <tbody>
            {jobs.map((j) => (
              <tr
                key={j.id}
                className={`clickable ${selected?.id === j.id ? "row-selected" : ""}`}
                onClick={() => select(j)}
              >
                <td title={j.id}>
                  {j.label || j.request?.strategy_id || j.id}
                  <small className="run-id">{j.id.slice(0, 12)}</small>
                </td>
                <td>
                  <JobBadge
                    status={
                      j.status === "queued" &&
                      Number(j.scheduled_at_ms) > Date.now()
                        ? "scheduled"
                        : j.status
                    }
                  />
                </td>
                <td>
                  <progress
                    value={j.progress > 1 ? j.progress / 100 : j.progress}
                    max="1"
                  />
                </td>
                <td>{date(Number(j.scheduled_at_ms) || j.created_at_ms)}</td>
                <td>
                  {!terminal(j.status) && (
                    <button
                      disabled={busy || !canResearch}
                      onClick={(e) => {
                        e.stopPropagation();
                        void cancel(j.id);
                      }}
                    >
                      Cancel
                    </button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {!jobs.length && (
          <Empty>
            No runs in this view. Submit a research request to begin.
          </Empty>
        )}
      </div>
      {selected?.error && <div className="error">{selected.error}</div>}
    </div>
  );
}
function Provenance() {
  const { selected } = useWorkspace();
  const result = selected?.result;
  if (!result) return <Empty />;
  return (
    <div className="panel-content">
      <div
        className={`result-banner ${result.provenance.synthetic ? "" : "verified"}`}
      >
        {result.provenance.synthetic
          ? "SYNTHETIC DEMO / ENGINE REPLAY"
          : ["real_dsl", "real_portfolio"].includes(
                selected?.request?.mode ?? "",
              )
            ? "OBSERVED MARKET DATA / TRAIN RESEARCH"
            : "OBSERVED NAV / RESEARCH UNVERIFIED"}
      </div>
      <table>
        <tbody>
          {Object.entries(result.provenance)
            .filter(([k]) => k !== "warnings")
            .map(([k, v]) => (
              <tr key={k}>
                <th>{k.replaceAll("_", " ")}</th>
                <td className="provenance-value">
                  {typeof v === "object" ? JSON.stringify(v) : String(v)}
                </td>
              </tr>
            ))}
        </tbody>
      </table>
      {result.provenance.warnings?.map((w, i) => (
        <div className="notice" key={i}>
          {w}
        </div>
      ))}
    </div>
  );
}
function Logs() {
  const { logs } = useWorkspace();
  const [channel, setChannel] = useState("all");
  const [query, setQuery] = useState("");
  const visible = logs.filter(
    (l) =>
      (channel === "all" || l.channel === channel) &&
      JSON.stringify(l).toLowerCase().includes(query.toLowerCase()),
  );
  return (
    <div className="panel-content">
      <div className="panel-toolbar">
        <select
          aria-label="Message channel"
          value={channel}
          onChange={(e) => setChannel(e.target.value)}
        >
          <option value="all">All channels</option>
          <option value="jobs">Jobs</option>
          <option value="system">System</option>
        </select>
        <input
          aria-label="Filter messages"
          placeholder="Filter messages…"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
        <span>{visible.length} messages</span>
      </div>
      <div className="logs">
        {visible.length ? (
          visible.map((l, i) => (
            <div key={`${l.sequence}-${i}`}>
              <time>
                [{new Date(Number(l.created_at_ms)).toLocaleTimeString()}]
              </time>{" "}
              <span className="log-type">{l.type}</span>{" "}
              <span>{l.job_id.slice(0, 10)}</span> {JSON.stringify(l.payload)}
            </div>
          ))
        ) : (
          <div className="muted">
            Waiting for messages. Stream resumes from the last event sequence.
          </div>
        )}
      </div>
    </div>
  );
}
function Panel({ params, api }: IDockviewPanelProps<{ kind: string }>) {
  const [active, setActive] = useState(api.isActive);
  useEffect(() => {
    const subscription = api.onDidActiveChange((event) =>
      setActive(event.isActive),
    );
    return () => subscription.dispose();
  }, [api]);
  const {
    selected,
    token,
    datasets,
    databaseContext,
    panelEpoch,
    setDatabaseContext,
    draft,
    setDraft,
    operatorCatalog,
    runAlpha,
    setAlphaUnsaved,
    experimentDraft,
    setExperimentDraft,
    workflowDraft,
    setWorkflowDraft,
    select,
    refresh,
  } = useWorkspace();
  const r = selected?.result;
  switch (params.kind) {
    case "workflows":
      return (
        <WorkflowsPanel
          token={token}
          draft={draft}
          composer={workflowDraft}
          setComposer={setWorkflowDraft}
          selectJob={select}
          onChanged={() => void refresh()}
        />
      );
    case "team":
      return <TeamPanel token={token} active={active} />;
    case "portfolio":
      return (
        <PortfolioPanel
          token={token}
          draft={draft}
          setDraft={setDraft}
          datasets={datasets}
          selected={selected}
        />
      );
    case "reviews":
      return <ReviewsPanel token={token} selectJob={select} />;
    case "experiments":
      return (
        <ExperimentsPanel
          token={token}
          draft={draft}
          composer={experimentDraft}
          setComposer={setExperimentDraft}
          selectJob={select}
          onSubmitted={() => void refresh()}
        />
      );
    case "alphas":
      return (
        <AlphaLibrary
          token={token}
          draft={draft}
          setDraft={setDraft}
          selected={selected}
          runAlpha={runAlpha}
          onUnsavedChange={setAlphaUnsaved}
        />
      );
    case "comparison":
      return <ComparisonPanel token={token} selected={selected} />;
    case "operators":
      return (
        <OperatorsPanel
          catalog={operatorCatalog}
          draft={draft}
          setDraft={setDraft}
        />
      );
    case "workspace":
      return <BrowserPanel />;
    case "configuration":
      return <Configuration />;
    case "editor":
      return <Editor />;
    case "database":
      return (
        <DatabasePanel
          key={panelEpoch}
          token={token}
          context={databaseContext}
          onContextChange={(next, changed) =>
            setDatabaseContext(next, changed, panelEpoch)
          }
        />
      );
    case "metrics":
      return <Metrics />;
    case "equity":
      return <LineChart points={r?.equity_curve} />;
    case "drawdown":
      return <LineChart points={r?.drawdown} drawdown />;
    case "distribution":
      return <Histogram bins={r?.returns_histogram} />;
    case "monthly":
      return <Heatmap months={r?.monthly_returns} />;
    case "jobs":
      return <Jobs />;
    case "logs":
      return <Logs />;
    case "trades":
      return (
        <div className="panel-content">
          <DataTable rows={r?.trades ?? []} />
        </div>
      );
    case "positions":
      return (
        <div className="panel-content">
          <DataTable rows={r?.positions ?? []} />
        </div>
      );
    case "factors":
      return r?.factor_exposures?.length ? (
        <DataTable rows={r.factor_exposures} />
      ) : (
        <Empty>Factor exposures are not supplied by this evaluation.</Empty>
      );
    case "provenance":
      return <Provenance />;
    default:
      return <Empty />;
  }
}
const components = { research: Panel };
const layoutKey = "atx.workspace.layout.v2";
function defaultLayout(api: DockviewApi, preset: LayoutPreset = "research") {
  // Construct the initial grid as three complete research rows so later splits
  // cannot steal width from the sidebar or collapse the configuration form.
  const width = api.width || 1530,
    height = api.height || 930;
  const compact = width < 1100,
    sidebar = compact ? 160 : 200,
    content = width - sidebar;
  type Node = SerializedDockview["grid"]["root"];
  const leaf = (views: string[], size: number): Node => ({
    type: "leaf",
    size,
    data: { id: `group-${views[0]}`, views, activeView: views[0] },
  });
  const branch = (data: Node[], size: number): Node => ({
    type: "branch",
    size,
    data,
  });
  const rows = compact
    ? [
        branch(
          [
            leaf(
              ["configuration", "editor", "database", "operators"],
              content * 0.57,
            ),
            leaf(["metrics"], content * 0.43),
          ],
          height * 0.45,
        ),
        branch(
          [leaf(["equity", "drawdown", "distribution", "monthly"], content)],
          height * 0.25,
        ),
        branch(
          [
            leaf(
              ["jobs", "trades", "positions", "factors", "provenance"],
              content * 0.62,
            ),
            leaf(["logs"], content * 0.38),
          ],
          height * 0.3,
        ),
      ]
    : [
        branch(
          [
            leaf(["editor", "database", "operators"], content * 0.38),
            leaf(["configuration"], content * 0.27),
            leaf(["metrics"], content * 0.35),
          ],
          height * 0.45,
        ),
        branch(
          [
            leaf(["equity"], content * 0.34),
            leaf(["drawdown"], content * 0.18),
            leaf(["distribution"], content * 0.18),
            leaf(["monthly"], content * 0.3),
          ],
          height * 0.25,
        ),
        branch(
          [
            leaf(
              [
                "jobs",
                "trades",
                "positions",
                "alphas",
                "experiments",
                "reviews",
              ],
              content * 0.43,
            ),
            leaf(["provenance", "factors", "comparison"], content * 0.27),
            leaf(["logs", "team"], content * 0.3),
          ],
          height * 0.3,
        ),
      ];
  const centralWidth = content * 0.72;
  const operationsConfig = Math.max(300, content * 0.3);
  const root =
    preset === "database"
      ? branch(
          [
            leaf(["workspace"], compact ? 140 : 180),
            branch(
              [
                leaf(["database", "editor", "configuration"], height * 0.76),
                leaf(["jobs", "trades", "positions"], height * 0.24),
              ],
              centralWidth,
            ),
            branch(
              [
                leaf(["provenance", "metrics", "factors"], height * 0.57),
                leaf(["logs"], height * 0.43),
              ],
              content - centralWidth,
            ),
          ],
          height,
        )
      : preset === "operations"
        ? branch(
            [
              leaf(["workspace"], sidebar),
              branch(
                [
                  branch(
                    [
                      leaf(
                        [
                          "workflows",
                          "jobs",
                          "experiments",
                          "reviews",
                          "trades",
                          "positions",
                        ],
                        content - operationsConfig,
                      ),
                      leaf(
                        [
                          "configuration",
                          "editor",
                          "database",
                          "provenance",
                          "metrics",
                        ],
                        operationsConfig,
                      ),
                    ],
                    height * 0.68,
                  ),
                  leaf(["logs"], height * 0.32),
                ],
                content,
              ),
            ],
            height,
          )
        : branch([leaf(["workspace"], sidebar), branch(rows, content)], height);
  const included = new Set<string>();
  const collect = (node: Node) => {
    if (Array.isArray(node.data)) node.data.forEach(collect);
    else node.data.views.forEach((id: string) => included.add(id));
  };
  collect(root);
  api.fromJSON({
    grid: {
      width,
      height,
      orientation: Orientation.HORIZONTAL,
      root,
    },
    panels: Object.fromEntries(
      Object.entries(titles)
        .filter(([id]) => included.has(id))
        .map(([id, title]) => [
          id,
          { id, title, contentComponent: "research", params: { kind: id } },
        ]),
    ),
    activeGroup:
      preset === "database"
        ? "group-database"
        : preset === "operations"
          ? "group-workflows"
          : "group-configuration",
  });
}
export default function App() {
  const [health, setHealth] = useState<Health | null>(null);
  const [strategies, setStrategies] = useState<Strategy[]>([]);
  const [datasets, setDatasets] = useState<Dataset[]>([]);
  const [operatorCatalog, setOperatorCatalog] =
    useState<OperatorCatalog | null>(null);
  const [jobs, setJobs] = useState<Job[]>([]);
  const [total, setTotal] = useState(0);
  const [selected, setSelected] = useState<Job | null>(null);
  const [draft, setDraftState] = useState<ResearchRequest>(initial);
  const [editorText, setEditorTextState] = useState(
    JSON.stringify(initial, null, 2),
  );
  const editorTextRef = useRef(editorText);
  editorTextRef.current = editorText;
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [connection, setConnection] = useState("Connecting");
  const [streaming, setStreaming] = useState(false);
  const [logs, setLogs] = useState<WorkspaceEvent[]>([]);
  const [filter, setFilter] = useState("");
  const [offset, setOffset] = useState(0);
  const [token, setToken] = useState("");
  const permissions = useSession(token);
  const [tokenInput, setTokenInput] = useState("");
  const [auth, setAuth] = useState(false);
  const accessOpened = useRef(false);
  const [panelMenu, setPanelMenu] = useState(false);
  const [maximized, setMaximized] = useState(false);
  const dock = useRef<DockviewApi | null>(null);
  const databaseContext = useRef<DatabaseContext>({ ...emptyDatabaseContext });
  const [panelEpoch, setPanelEpoch] = useState(0);
  const panelEpochRef = useRef(0);
  const lastLayout = useRef("");
  const lastDockDimensions = useRef({ width: 0, height: 0 });
  const draftRef = useRef(draft);
  draftRef.current = draft;
  const applyingLayout = useRef(false);
  const workspacePorts = useRef<WorkspacePorts | null>(null);
  const named = useNamedWorkspaces(
    token,
    workspacePorts,
    permissions.canResearch,
  );
  const [experimentDraft, setExperimentDraftState] =
    useState<ExperimentDraft>(emptyExperimentDraft);
  const experimentDraftRef = useRef(experimentDraft);
  experimentDraftRef.current = experimentDraft;
  const setExperimentDraft = useCallback(
    (next: ExperimentDraft) => {
      setExperimentDraftState(next);
      experimentDraftRef.current = next;
      named.markChanged();
    },
    [named.markChanged],
  );
  const alphaUnsaved = useRef(false);
  const [workflowDraft, setWorkflowDraftState] =
    useState<WorkflowDraft>(emptyWorkflowDraft);
  const workflowDraftRef = useRef(workflowDraft);
  workflowDraftRef.current = workflowDraft;
  const setWorkflowDraft = useCallback(
    (next: WorkflowDraft) => {
      setWorkflowDraftState(next);
      workflowDraftRef.current = next;
      named.markChanged();
    },
    [named.markChanged],
  );
  const setAlphaUnsaved = useCallback(
    (dirty: boolean) => {
      alphaUnsaved.current = dirty;
      if (dirty) named.markChanged();
    },
    [named.markChanged],
  );
  const setDraft = useCallback(
    (next: ResearchRequest) => {
      setDraftState(next);
      setEditorTextState(JSON.stringify(next, null, 2));
      editorTextRef.current = JSON.stringify(next, null, 2);
      if (!applyingLayout.current) named.markChanged();
    },
    [named.markChanged],
  );
  const setEditorText = useCallback(
    (next: string) => {
      setEditorTextState(next);
      editorTextRef.current = next;
      named.markChanged();
    },
    [named.markChanged],
  );
  const setDatabaseContext = useCallback(
    (next: DatabaseContext, userChanged: boolean, ownerEpoch: number) => {
      if (ownerEpoch !== panelEpochRef.current) return;
      databaseContext.current = next;
      if (userChanged && !applyingLayout.current) named.markChanged();
    },
    [named.markChanged],
  );
  const selectedId = useRef("");
  const selectedDetail = useRef<Job | null>(null);
  const detailGeneration = useRef(0);
  const sequence = useRef("0");
  const refreshTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const refreshGeneration = useRef(0);
  const pendingSubmission = useRef<{ signature: string; key: string } | null>(
    null,
  );
  const loadSelected = useCallback(
    async (id: string) => {
      const generation = ++detailGeneration.current;
      const cached = selectedDetail.current;
      // Cache only one immutable terminal document. The native wire limit bounds
      // its size; pagination summaries must never erase its research evidence.
      if (cached?.id === id && terminal(cached.status) && cached.request) {
        setSelected(cached);
        return;
      }
      try {
        const detail = await request<Job>(
          `/jobs/${encodeURIComponent(id)}`,
          token,
        );
        if (
          selectedId.current !== id ||
          generation !== detailGeneration.current
        )
          return;
        selectedDetail.current = detail;
        setSelected(detail);
      } catch (e) {
        if (
          selectedId.current === id &&
          generation === detailGeneration.current
        )
          setError((e as Error).message);
      }
    },
    [token],
  );
  const select = useCallback(
    (j: Job, userChanged = true) => {
      if (userChanged && selectedId.current !== j.id) named.markChanged();
      selectedId.current = j.id;
      if (j.request) selectedDetail.current = j;
      setSelected(
        selectedDetail.current?.id === j.id ? selectedDetail.current : j,
      );
      void loadSelected(j.id);
    },
    [loadSelected, named.markChanged],
  );
  const refresh = useCallback(async () => {
    const generation = ++refreshGeneration.current;
    try {
      const [h, catalog, list] = await Promise.all([
        request<Health>("/health", token),
        request<{
          strategies: Strategy[];
          datasets?: Dataset[];
          operators?: OperatorCatalog;
        }>("/strategies", token),
        request<{ jobs: Job[]; total: string }>(
          `/jobs?limit=100&offset=${offset}&status=${filter}`,
          token,
        ),
      ]);
      if (generation !== refreshGeneration.current) return;
      setHealth(h);
      setStrategies(catalog.strategies);
      setDatasets(catalog.datasets ?? []);
      setOperatorCatalog(catalog.operators ?? null);
      setJobs(list.jobs);
      setTotal(Number(list.total));
      setConnection(
        ["serving", "healthy", "ok", "ready"].includes(
          String(h.status).toLowerCase(),
        )
          ? "Connected"
          : "Degraded",
      );
      if (!accessOpened.current) setAuth(false);
      setError("");
      if (selectedId.current) await loadSelected(selectedId.current);
      else if (list.jobs.length)
        select(
          list.jobs.find((j) => j.status === "succeeded") ?? list.jobs[0],
          false,
        );
    } catch (e) {
      if (generation !== refreshGeneration.current) return;
      setConnection("Offline");
      setHealth(null);
      if (e instanceof ApiError && e.status === 401) setAuth(true);
      setError((e as Error).message);
    }
  }, [token, filter, offset, select, loadSelected]);
  useEffect(() => {
    void refresh();
    const timer = setInterval(() => void refresh(), 15_000);
    return () => clearInterval(timer);
  }, [refresh]);
  useEffect(() => {
    const controller = new AbortController();
    let delay = 1000;
    let retry: ReturnType<typeof setTimeout>;
    let workspaceRefresh: ReturnType<typeof setTimeout> | undefined;
    const connect = async () => {
      try {
        await events(
          token,
          sequence.current,
          controller.signal,
          (event) => {
            delay = 1000;
            setStreaming(true);
            sequence.current = event.sequence;
            setLogs((prev) => [...prev.slice(-499), event]);
            if (
              (event.type.startsWith("workspace.") || event.gap) &&
              !workspaceRefresh
            )
              workspaceRefresh = setTimeout(() => {
                workspaceRefresh = undefined;
                void named.refreshList();
              }, 1000);
            if (event.gap)
              setLogs((prev) => [
                ...prev.slice(-498),
                {
                  ...event,
                  type: "snapshot-refresh",
                  payload:
                    "Event replay gap detected; refreshing durable job state.",
                },
              ]);
            if (!refreshTimer.current)
              refreshTimer.current = setTimeout(() => {
                refreshTimer.current = null;
                void refresh();
              }, 1000);
          },
          () => setStreaming(true),
          () => {
            if (!refreshTimer.current)
              refreshTimer.current = setTimeout(() => {
                refreshTimer.current = null;
                void refresh();
              }, 1000);
          },
        );
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) setAuth(true);
      }
      setStreaming(false);
      if (!controller.signal.aborted) {
        retry = setTimeout(connect, delay);
        delay = Math.min(delay * 2, 30_000);
      }
    };
    void connect();
    return () => {
      controller.abort();
      clearTimeout(retry);
      clearTimeout(workspaceRefresh);
      if (refreshTimer.current) {
        clearTimeout(refreshTimer.current);
        refreshTimer.current = null;
      }
    };
  }, [token, refresh, named.refreshList]);
  const submit = async (schedule: string) => {
    if (!permissions.canResearch) {
      setError("Your current role cannot submit research jobs.");
      return;
    }
    setBusy(true);
    setError("");
    try {
      if (
        draft.mode === "real_portfolio" &&
        (draft.components?.some(
          (component) => !/^[0-9a-f]{64}$/.test(component.expression_sha256),
        ) ||
          !draft.components?.length)
      )
        throw new Error(
          "Validate and pin portfolio components before submitting the draft.",
        );
      const scheduled = schedule ? new Date(schedule).getTime() : 0;
      if (schedule && (!Number.isFinite(scheduled) || scheduled <= Date.now()))
        throw new Error("Choose a future schedule time");
      const signature = JSON.stringify({
        request: draft,
        scheduled_at_ms: scheduled,
      });
      if (pendingSubmission.current?.signature !== signature)
        pendingSubmission.current = { signature, key: crypto.randomUUID() };
      const j = await request<Job>("/jobs", token, {
        method: "POST",
        body: JSON.stringify({
          request: draft,
          scheduled_at_ms: scheduled,
          idempotency_key: pendingSubmission.current.key,
        }),
      });
      pendingSubmission.current = null;
      select(j);
      await refresh();
      dock.current?.getPanel("jobs")?.api.setActive();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const cancel = async (id: string) => {
    if (!permissions.canResearch) {
      setError("Your current role cannot cancel research jobs.");
      return;
    }
    setBusy(true);
    setError("");
    try {
      const j = await request<Job>(
        `/jobs/${encodeURIComponent(id)}/cancel`,
        token,
        { method: "POST" },
      );
      if (selectedId.current === id) setSelected(j);
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const runAlpha = async (
    alpha_id: string,
    alpha_revision: string,
    label: string,
  ) => {
    if (!permissions.canResearch)
      throw new Error("Your current role cannot run alpha revisions.");
    setBusy(true);
    setError("");
    const signature = JSON.stringify({ alpha_id, alpha_revision, label });
    if (pendingSubmission.current?.signature !== signature)
      pendingSubmission.current = { signature, key: crypto.randomUUID() };
    try {
      const job = await request<Job>("/jobs", token, {
        method: "POST",
        body: JSON.stringify({
          alpha_id,
          alpha_revision,
          label,
          idempotency_key: pendingSubmission.current.key,
        }),
      });
      pendingSubmission.current = null;
      select(job);
      await refresh();
      dock.current?.getPanel("jobs")?.api.setActive();
    } catch (failure) {
      setError((failure as Error).message);
      throw failure;
    } finally {
      setBusy(false);
    }
  };
  const onReady = (e: DockviewReadyEvent) => {
    dock.current = e.api;
    e.api.onDidMaximizedGroupChange(() =>
      setMaximized(e.api.hasMaximizedGroup()),
    );
    applyingLayout.current = true;
    let legacy = false;
    try {
      const saved = localStorage.getItem(layoutKey);
      if (saved) {
        e.api.fromJSON(JSON.parse(saved));
        legacy = true;
      } else defaultLayout(e.api);
    } catch {
      defaultLayout(e.api);
    }
    for (const panel of e.api.panels)
      if (titles[panel.id]) panel.api.setTitle(titles[panel.id]);
    e.api.onDidLayoutChange(() => {
      const layout = JSON.stringify(e.api.toJSON());
      const resized =
        lastDockDimensions.current.width !== e.api.width ||
        lastDockDimensions.current.height !== e.api.height;
      if (!applyingLayout.current && !resized && layout !== lastLayout.current)
        named.markChanged();
      lastLayout.current = layout;
      lastDockDimensions.current = { width: e.api.width, height: e.api.height };
      try {
        localStorage.setItem(layoutKey, layout);
      } catch {
        /* Storage can be unavailable in private browsing. */
      }
    });
    const requestedPanel = new URL(window.location.href).searchParams.get(
      "panel",
    );
    if (requestedPanel && Object.hasOwn(titles, requestedPanel))
      showPanel(requestedPanel);
    applyingLayout.current = false;
    lastLayout.current = JSON.stringify(e.api.toJSON());
    lastDockDimensions.current = { width: e.api.width, height: e.api.height };
    named.initialize(legacy);
  };
  const showPanel = (id: string) => {
    const api = dock.current;
    if (!api) return;
    const panel = api.getPanel(id);
    if (panel) panel.api.setActive();
    else
      api.addPanel({
        id,
        title: titles[id],
        component: "research",
        params: { kind: id },
        ...([
          "reviews",
          "alphas",
          "experiments",
          "comparison",
          "portfolio",
          "workflows",
        ].includes(id) && api.getPanel("jobs")
          ? {
              position: {
                referencePanel: "jobs",
                direction: "within" as const,
              },
            }
          : {}),
      });
    setPanelMenu(false);
  };
  const exportRun = () => {
    if (!selected) return;
    const url = URL.createObjectURL(
      new Blob([JSON.stringify(selected, null, 2)], {
        type: "application/json",
      }),
    );
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `atx-${selected.id}.json`;
    anchor.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  workspacePorts.current = {
    capture: () => {
      if (!dock.current) throw new Error("The docking workspace is not ready.");
      if (alphaUnsaved.current)
        throw new Error(
          "Save or discard alpha library edits before saving this workspace. Your alpha form has been kept.",
        );
      try {
        if (
          JSON.stringify(JSON.parse(editorTextRef.current)) !==
          JSON.stringify(draftRef.current)
        )
          throw new Error("Unapplied editor changes");
      } catch {
        throw new Error(
          "Apply or revert the request editor before saving. Your editor text has been kept.",
        );
      }
      return {
        schema_version: 1,
        layout: dock.current.toJSON(),
        panel_state: {
          draft_request: draftRef.current,
          selected_job_id: selectedId.current,
          database: databaseContext.current,
          experiment: experimentDraftRef.current,
          workflow: workflowDraftRef.current,
        },
      };
    },
    apply: (document) => {
      validateWorkspaceDocument(document);
      const api = dock.current;
      if (!api) throw new Error("The docking workspace is not ready.");
      const previous = api.toJSON();
      applyingLayout.current = true;
      try {
        api.fromJSON(document.layout);
      } catch (failure) {
        api.fromJSON(previous);
        throw failure;
      } finally {
        applyingLayout.current = false;
      }
      for (const panel of api.panels)
        if (titles[panel.id]) panel.api.setTitle(titles[panel.id]);
      setDraftState(document.panel_state.draft_request);
      setEditorTextState(
        JSON.stringify(document.panel_state.draft_request, null, 2),
      );
      editorTextRef.current = JSON.stringify(
        document.panel_state.draft_request,
        null,
        2,
      );
      databaseContext.current = document.panel_state.database;
      setExperimentDraftState(
        document.panel_state.experiment ?? emptyExperimentDraft,
      );
      experimentDraftRef.current =
        document.panel_state.experiment ?? emptyExperimentDraft;
      setWorkflowDraftState(
        document.panel_state.workflow ?? emptyWorkflowDraft,
      );
      workflowDraftRef.current =
        document.panel_state.workflow ?? emptyWorkflowDraft;
      setPanelEpoch(++panelEpochRef.current);
      selectedId.current = document.panel_state.selected_job_id;
      selectedDetail.current = null;
      detailGeneration.current++;
      setSelected(null);
      if (selectedId.current) void loadSelected(selectedId.current);
      const requested = new URL(window.location.href).searchParams.get("panel");
      if (requested && Object.hasOwn(titles, requested)) showPanel(requested);
      lastLayout.current = JSON.stringify(api.toJSON());
      lastDockDimensions.current = { width: api.width, height: api.height };
    },
    template: (preset) => {
      if (!dock.current) throw new Error("The docking workspace is not ready.");
      applyingLayout.current = true;
      try {
        defaultLayout(dock.current, preset);
      } finally {
        applyingLayout.current = false;
      }
      setDraftState(initial);
      setEditorTextState(JSON.stringify(initial, null, 2));
      editorTextRef.current = JSON.stringify(initial, null, 2);
      databaseContext.current = { ...emptyDatabaseContext };
      setExperimentDraftState(emptyExperimentDraft);
      experimentDraftRef.current = emptyExperimentDraft;
      setWorkflowDraftState(emptyWorkflowDraft);
      workflowDraftRef.current = emptyWorkflowDraft;
      setPanelEpoch(++panelEpochRef.current);
      lastLayout.current = JSON.stringify(dock.current.toJSON());
      lastDockDimensions.current = {
        width: dock.current.width,
        height: dock.current.height,
      };
    },
  };
  return (
    <SessionContext.Provider value={permissions}>
      <Workspace.Provider
        value={{
          token,
          databaseContext: databaseContext.current,
          panelEpoch,
          setDatabaseContext,
          health,
          strategies,
          datasets,
          operatorCatalog,
          jobs,
          total,
          selected,
          select,
          draft,
          setDraft,
          editorText,
          setEditorText,
          runAlpha,
          setAlphaUnsaved,
          experimentDraft,
          setExperimentDraft,
          workflowDraft,
          setWorkflowDraft,
          submit,
          cancel,
          busy,
          error,
          logs,
          filter,
          setFilter,
          offset,
          setOffset,
          refresh,
        }}
      >
        <main className="app-shell">
          <header className="titlebar">
            <div className="brand-mark">ATX</div>
            <strong>Research workstation</strong>
            <span className="version">EQUITY / v1</span>
            <div className="spacer" />
            {permissions.session && (
              <button
                title={`${permissions.session.team_name} / shared project ${permissions.session.project_id}`}
                onClick={() => showPanel("team")}
              >
                {permissions.session.principal.name} /{" "}
                {permissions.session.principal.role}
                {permissions.expired ? " / expired" : ""}
              </button>
            )}
            <span
              className={`connection ${connection === "Connected" ? "online" : ""}`}
            >
              <i />
              {connection}
            </span>
            <button
              onClick={() => {
                accessOpened.current = !auth;
                setAuth(!auth);
              }}
            >
              Access
            </button>
          </header>
          <nav className="menubar">
            <WorkspaceSelector manager={named} />
            <button onClick={() => void refresh()}>↻ Refresh</button>
            <button onClick={exportRun} disabled={!selected}>
              Export run
            </button>
            <button onClick={() => showPanel("database")}>Database</button>
            <button onClick={() => showPanel("reviews")}>Reviews</button>
            <button onClick={() => showPanel("team")}>Team</button>
            <div className="menu-anchor">
              <button
                aria-expanded={panelMenu}
                onClick={() => setPanelMenu(!panelMenu)}
              >
                Windows ▾
              </button>
              {panelMenu && (
                <div className="dropdown">
                  {Object.entries(titles).map(([id, title]) => (
                    <button key={id} onClick={() => showPanel(id)}>
                      {title}
                    </button>
                  ))}
                </div>
              )}
            </div>
            <button
              onClick={() => {
                if (dock.current)
                  defaultLayout(dock.current, named.preset ?? "research");
              }}
            >
              Reset layout
            </button>
            <button
              aria-label={
                maximized ? "Restore docking layout" : "Maximize active panel"
              }
              onClick={() => {
                const api = dock.current;
                if (!api) return;
                if (api.hasMaximizedGroup()) api.exitMaximizedGroup();
                else api.activePanel?.api.maximize();
              }}
            >
              {maximized ? "Restore" : "Maximize"}
            </button>
            <div className="spacer" />
            <span className="menubar-hint">
              Drag tabs to dock · drag dividers to resize
            </span>
          </nav>
          <WorkspaceMessages manager={named} />
          {auth && (
            <form
              className="authbar"
              onSubmit={(e) => {
                e.preventDefault();
                accessOpened.current = false;
                setToken(tokenInput);
                setTokenInput("");
              }}
            >
              <label>
                Workstation access token{" "}
                <input
                  autoFocus
                  type="password"
                  autoComplete="off"
                  value={tokenInput}
                  onChange={(e) => setTokenInput(e.target.value)}
                />
              </label>
              <button className="primary">Connect</button>
              <button
                type="button"
                onClick={() => {
                  accessOpened.current = false;
                  setToken("");
                  setTokenInput("");
                  setAuth(false);
                }}
              >
                Clear
              </button>
              <span>Token stays in this browser session's memory.</span>
            </form>
          )}
          {error && connection === "Offline" && (
            <div className="connection-error" role="alert">
              {error}{" "}
              <button onClick={() => void refresh()}>Retry connection</button>
            </div>
          )}
          {connection === "Degraded" && (
            <div className="connection-error" role="alert">
              Native scheduler is degraded; queued work may not start.{" "}
              <button onClick={() => void refresh()}>Refresh status</button>
            </div>
          )}
          <div className="dock-container">
            <DockviewReact
              className="dockview-theme-light atx-dock"
              components={components}
              onReady={onReady}
            />
          </div>
          <footer className="statusbar">
            <span>
              <i className={streaming ? "live-dot" : "idle-dot"} />
              {streaming ? "Channels live" : "Channels reconnecting"}
            </span>
            <span>
              Queued {health?.queued_jobs ?? "—"} · Running{" "}
              {health?.running_jobs ?? "—"}
            </span>
            <span>
              Cache {health?.cache_entries ?? "—"} entries ·{" "}
              {health
                ? number(Number(health.cache_bytes) / 1024 / 1024, 1)
                : "—"}{" "}
              MB
            </span>
            <div className="spacer" />
            <span>
              {selected?.result?.provenance.synthetic
                ? "SYNTHETIC DEMO"
                : selected?.result
                  ? ["real_dsl", "real_portfolio"].includes(
                      selected.request?.mode ?? "",
                    )
                    ? "OBSERVED TRAIN / UNVERIFIED"
                    : "IMPORTED NAV"
                  : "No result selected"}
            </span>
            <span>Server {health?.version ?? "unavailable"}</span>
          </footer>
        </main>
      </Workspace.Provider>
    </SessionContext.Provider>
  );
}
