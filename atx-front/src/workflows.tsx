import { useCallback, useEffect, useRef, useState } from "react";
import { ApiError, request } from "./api";
import type { AlphaSummary } from "./alpha-library";
import type { Job, ResearchRequest } from "./types";
import { usePermissions } from "./session";
type Source = {
  request?: ResearchRequest;
  alpha_id?: string;
  alpha_revision?: string;
  label: string;
};
export type WorkflowDraft = {
  name: string;
  description: string;
  interval_value: string;
  interval_unit: "minutes" | "seconds" | "milliseconds";
  max_runs: number;
  first_run: string;
  source: Source | null;
};
export const emptyWorkflowDraft: WorkflowDraft = {
  name: "",
  description: "",
  interval_value: "60",
  interval_unit: "minutes",
  max_runs: 10,
  first_run: "",
  source: null,
};
type WorkflowRecord = {
  id: string;
  name: string;
  description: string;
  request: ResearchRequest | null;
  request_sha256: string;
  alpha_id: string;
  alpha_revision: string;
  actor_id: string;
  interval_ms: string;
  max_runs: number;
  first_run_at_ms: string;
  next_run_at_ms: string;
  revision: string;
  enabled: boolean;
  state: string;
  pause_reason: string;
  last_dispatch_error: string;
  created_at_ms: string;
  updated_at_ms: string;
  admitted_runs: string;
  queued: string;
  running: string;
  succeeded: string;
  failed: string;
  cancelled: string;
  missed_slots: string;
  skipped_slots: string;
  active_job_id: string;
  last_job_id: string;
};
const clock = (milliseconds: string) =>
  Number(milliseconds)
    ? new Date(Number(milliseconds))
        .toISOString()
        .replace("T", " ")
        .replace(".000Z", "Z")
    : "—";
const interval = (milliseconds: string) => {
  try {
    const value = BigInt(milliseconds);
    return value % 60000n === 0n ? `${value / 60000n} min` : `${value} ms`;
  } catch {
    return "Unavailable";
  }
};
export function WorkflowsPanel({
  token,
  draft,
  composer,
  setComposer,
  selectJob,
  onChanged,
}: {
  token: string;
  draft: ResearchRequest;
  composer: WorkflowDraft;
  setComposer: (value: WorkflowDraft) => void;
  selectJob: (job: Job) => void;
  onChanged: () => void;
}) {
  const { canResearch } = usePermissions();
  const [records, setRecords] = useState<WorkflowRecord[]>([]),
    [selected, setSelected] = useState<WorkflowRecord | null>(null);
  const [offset, setOffset] = useState(0),
    [total, setTotal] = useState(0);
  const [runs, setRuns] = useState<Job[]>([]),
    [runOffset, setRunOffset] = useState(0),
    [runTotal, setRunTotal] = useState(0);
  const [alphas, setAlphas] = useState<AlphaSummary[]>([]),
    [alphaId, setAlphaId] = useState("");
  const [busy, setBusy] = useState(false),
    [error, setError] = useState(""),
    [listError, setListError] = useState(""),
    [conflict, setConflict] = useState(false);
  const [alphaError, setAlphaError] = useState("");
  const listActive = useRef<AbortController | null>(null),
    detailActive = useRef<AbortController | null>(null),
    writeActive = useRef<AbortController | null>(null);
  const detailVersion = useRef(0),
    writeVersion = useRef(0),
    selectedId = useRef("");
  const idempotency = useRef<{
    signature: string;
    key: string;
    first_run_at_ms: string;
  } | null>(null);
  const load = useCallback(
    async (id: string) => {
      detailActive.current?.abort();
      const controller = new AbortController();
      detailActive.current = controller;
      const version = ++detailVersion.current;
      const changed = selectedId.current !== id;
      selectedId.current = id;
      if (changed) {
        setSelected(null);
        setRuns([]);
        setRunOffset(0);
      }
      try {
        const [record, page] = await Promise.all([
          request<WorkflowRecord>(`/workflows/${id}`, token, {
            signal: controller.signal,
          }),
          request<{ jobs: Job[]; total: string }>(
            `/workflows/${id}/runs?limit=50&offset=${changed ? 0 : runOffset}`,
            token,
            { signal: controller.signal },
          ),
        ]);
        if (
          controller.signal.aborted ||
          version !== detailVersion.current ||
          selectedId.current !== id
        )
          return;
        setSelected(record);
        setRuns(page.jobs);
        setRunTotal(Number(page.total));
      } catch (failure) {
        if (!controller.signal.aborted && version === detailVersion.current)
          setError((failure as Error).message);
      }
    },
    [token, runOffset],
  );
  const refresh = useCallback(async () => {
    listActive.current?.abort();
    const controller = new AbortController();
    listActive.current = controller;
    try {
      const page = await request<{
        workflows: WorkflowRecord[];
        total: string;
      }>(`/workflows?limit=50&offset=${offset}`, token, {
        signal: controller.signal,
      });
      if (controller.signal.aborted) return;
      setRecords(page.workflows);
      setTotal(Number(page.total));
      setListError("");
      if (selectedId.current) void load(selectedId.current);
    } catch (failure) {
      if (!controller.signal.aborted) setListError((failure as Error).message);
    }
  }, [token, offset, load]);
  useEffect(() => {
    void refresh();
    const timer = setInterval(() => void refresh(), 5000);
    return () => {
      clearInterval(timer);
      listActive.current?.abort();
      detailActive.current?.abort();
      detailVersion.current++;
    };
  }, [refresh]);
  useEffect(() => {
    const controller = new AbortController();
    setBusy(false);
    request<{ alphas: AlphaSummary[] }>("/alphas?limit=100", token, {
      signal: controller.signal,
    })
      .then((page) => {
        if (!controller.signal.aborted) {
          setAlphas(page.alphas);
          setAlphaId(page.alphas[0]?.id ?? "");
          setAlphaError("");
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) setAlphaError(failure.message);
      });
    return () => {
      controller.abort();
      writeActive.current?.abort();
      writeVersion.current++;
    };
  }, [token]);
  const update = (key: keyof WorkflowDraft, value: unknown) =>
    setComposer({ ...composer, [key]: value });
  const create = async () => {
    if (!canResearch || !composer.source) return;
    const units = { minutes: 60000, seconds: 1000, milliseconds: 1 };
    const amount = Number(composer.interval_value),
      multiplier = units[composer.interval_unit],
      milliseconds = amount * multiplier,
      first = composer.first_run
        ? new Date(composer.first_run).getTime()
        : Date.now() + 1000;
    if (
      !composer.name.trim() ||
      !Number.isSafeInteger(milliseconds) ||
      milliseconds < 60000 ||
      milliseconds > 2592000000 ||
      !Number.isInteger(composer.max_runs) ||
      composer.max_runs < 1 ||
      composer.max_runs > 1000 ||
      !Number.isFinite(first) ||
      first <= 0 ||
      first > Date.now() + 365 * 86400000
    ) {
      setError(
        "Name the workflow, choose an exact interval of 60 seconds to 30 days, 1–1,000 runs, and a first slot within the next year.",
      );
      return;
    }
    const body = {
      name: composer.name.trim(),
      description: composer.description,
      interval_ms: String(milliseconds),
      max_runs: composer.max_runs,
      first_run_at_ms: String(first),
      ...(composer.source.request
        ? { request: composer.source.request }
        : {
            alpha_id: composer.source.alpha_id,
            alpha_revision: composer.source.alpha_revision,
          }),
    };
    // A blank first-slot field means an immediate slot. Retain the exact epoch
    // and key on retries so a lost response cannot admit a second recurrence.
    const signature = JSON.stringify({
      ...body,
      first_run_at_ms: composer.first_run || "immediate",
    });
    if (idempotency.current?.signature !== signature)
      idempotency.current = {
        signature,
        key: crypto.randomUUID(),
        first_run_at_ms: body.first_run_at_ms,
      };
    body.first_run_at_ms = idempotency.current.first_run_at_ms;
    const controller = new AbortController();
    writeActive.current?.abort();
    writeActive.current = controller;
    const version = ++writeVersion.current;
    setBusy(true);
    setError("");
    setConflict(false);
    try {
      const record = await request<WorkflowRecord>("/workflows", token, {
        method: "POST",
        signal: controller.signal,
        body: JSON.stringify({
          ...body,
          idempotency_key: idempotency.current.key,
        }),
      });
      if (controller.signal.aborted || version !== writeVersion.current) return;
      idempotency.current = null;
      detailActive.current?.abort();
      detailVersion.current++;
      setSelected(record);
      selectedId.current = record.id;
      setRunOffset(0);
      void refresh();
      onChanged();
    } catch (failure) {
      if (!controller.signal.aborted && version === writeVersion.current)
        setError((failure as Error).message);
    } finally {
      if (version === writeVersion.current) setBusy(false);
    }
  };
  const mutate = async (cancel = false) => {
    if (!canResearch || !selected) return;
    const target = selected.id;
    const controller = new AbortController();
    writeActive.current?.abort();
    writeActive.current = controller;
    const version = ++writeVersion.current;
    setBusy(true);
    setError("");
    setConflict(false);
    try {
      if (cancel)
        await request<Job>(`/jobs/${selected.active_job_id}/cancel`, token, {
          method: "POST",
          signal: controller.signal,
        });
      else {
        const record = await request<WorkflowRecord>(
          `/workflows/${target}/pause`,
          token,
          {
            method: "POST",
            signal: controller.signal,
            body: JSON.stringify({
              expected_revision: selected.revision,
              paused: selected.enabled,
            }),
          },
        );
        if (controller.signal.aborted || version !== writeVersion.current)
          return;
        detailActive.current?.abort();
        detailVersion.current++;
        if (selectedId.current === target) setSelected(record);
      }
      if (controller.signal.aborted || version !== writeVersion.current) return;
      void refresh();
      onChanged();
    } catch (failure) {
      if (!controller.signal.aborted && version === writeVersion.current) {
        setError((failure as Error).message);
        setConflict(failure instanceof ApiError && failure.status === 409);
      }
    } finally {
      if (version === writeVersion.current) setBusy(false);
    }
  };
  return (
    <div className="research-tool-panel workflows-panel">
      <div className="panel-toolbar">
        <strong>Recurring workflows</strong>
        <button onClick={() => void refresh()}>Refresh workflows</button>
        <span>{total} records · no overlapping slots</span>
      </div>
      {(error || listError || alphaError) && (
        <div className="notice error" role="alert">
          {error || listError || alphaError}
          {conflict && (
            <button
              onClick={() => {
                if (selectedId.current) void load(selectedId.current);
                setError("");
                setConflict(false);
              }}
            >
              Reload workflow revision
            </button>
          )}
        </div>
      )}
      <div className="workflows-body">
        <section className="workflow-composer">
          <div className="panel-toolbar">
            Draft recurrence · immutable after creation
          </div>
          <label>
            Name
            <input
              aria-label="Workflow name"
              value={composer.name}
              maxLength={160}
              onChange={(e) => update("name", e.target.value)}
            />
          </label>
          <label>
            Description
            <input
              aria-label="Workflow description"
              value={composer.description}
              maxLength={512}
              onChange={(e) => update("description", e.target.value)}
            />
          </label>
          <label>
            Interval
            <span className="workflow-interval">
              <input
                aria-label="Workflow interval"
                type="number"
                min={1}
                max={2592000000}
                step={1}
                value={composer.interval_value}
                onChange={(e) => update("interval_value", e.target.value)}
              />
              <select
                aria-label="Workflow interval unit"
                value={composer.interval_unit}
                onChange={(e) => update("interval_unit", e.target.value)}
              >
                <option value="minutes">Minutes</option>
                <option value="seconds">Seconds</option>
                <option value="milliseconds">Milliseconds</option>
              </select>
            </span>
          </label>
          <label>
            Maximum admissions
            <input
              aria-label="Workflow maximum runs"
              type="number"
              min={1}
              max={1000}
              step={1}
              value={composer.max_runs}
              onChange={(e) => update("max_runs", e.target.valueAsNumber)}
            />
          </label>
          <label>
            First slot (local time)
            <input
              aria-label="Workflow first slot"
              type="datetime-local"
              value={composer.first_run}
              onChange={(e) => update("first_run", e.target.value)}
            />
          </label>
          <div className="panel-toolbar">
            <button
              disabled={busy}
              onClick={() =>
                update("source", {
                  request: structuredClone(draft),
                  label: `${draft.strategy_id} / frozen draft`,
                })
              }
            >
              Freeze current recipe
            </button>
            <select
              aria-label="Workflow saved alpha"
              value={alphaId}
              onChange={(e) => setAlphaId(e.target.value)}
            >
              {!alphas.length && <option value="">No saved alpha</option>}
              {alphas.map((alpha) => (
                <option key={alpha.id} value={alpha.id}>
                  {alpha.name} / r{alpha.revision}
                </option>
              ))}
            </select>
            <button
              disabled={busy || !alphaId}
              onClick={() => {
                const alpha = alphas.find((record) => record.id === alphaId);
                if (alpha)
                  update("source", {
                    alpha_id: alpha.id,
                    alpha_revision: alpha.revision,
                    label: `${alpha.name} / r${alpha.revision}`,
                  });
              }}
            >
              Freeze saved revision
            </button>
          </div>
          <div className="workflow-source">
            {composer.source?.label ||
              "Freeze a recipe or saved revision explicitly."}
            {composer.source && (
              <details>
                <summary>Exact frozen source</summary>
                <pre>{JSON.stringify(composer.source, null, 2)}</pre>
              </details>
            )}
          </div>
          <button
            disabled={
              busy || !canResearch || !composer.source || !composer.name.trim()
            }
            onClick={() => void create()}
          >
            Create recurring workflow
          </button>
          <div className="notice">
            Fixed UTC intervals, not exchange-calendar or DST cron. Blank first
            slot starts now. Frozen dataset pins, dates and recipe do not roll
            forward. Missed slots coalesce; pause does not cancel an active job.
            Create a copy to change the recipe or cadence.
          </div>
        </section>
        <section className="workflow-history">
          <div className="table-scroll workflow-list">
            <table>
              <thead>
                <tr>
                  <th>Workflow</th>
                  <th>State</th>
                  <th>Admissions</th>
                  <th>Interval</th>
                  <th>Next UTC slot</th>
                  <th>Actor</th>
                </tr>
              </thead>
              <tbody>
                {records.map((record) => (
                  <tr
                    key={record.id}
                    className={
                      selectedId.current === record.id ? "selected-row" : ""
                    }
                  >
                    <td>
                      <button
                        className="table-link"
                        onClick={() => {
                          setError("");
                          setConflict(false);
                          void load(record.id);
                        }}
                      >
                        {record.name}
                      </button>
                    </td>
                    <td>{record.state}</td>
                    <td>
                      {record.admitted_runs}/{record.max_runs}
                    </td>
                    <td>{interval(record.interval_ms)}</td>
                    <td>{clock(record.next_run_at_ms)}</td>
                    <td>{record.actor_id}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <div className="panel-toolbar">
            <button
              disabled={!offset}
              onClick={() => setOffset(Math.max(0, offset - 50))}
            >
              Previous workflows
            </button>
            <button
              disabled={offset + 50 >= total}
              onClick={() => setOffset(offset + 50)}
            >
              Next workflows
            </button>
          </div>
          {selected && (
            <>
              <div className="panel-toolbar">
                <strong>
                  {selected.state} / r{selected.revision}
                </strong>
                <button
                  title={
                    selected.state === "completed"
                      ? "The finite admission budget is exhausted. Create a copy for another workflow."
                      : undefined
                  }
                  disabled={
                    busy ||
                    !canResearch ||
                    selected.state === "completed" ||
                    selected.state === "draining"
                  }
                  onClick={() => void mutate()}
                >
                  {selected.state === "completed"
                    ? "Completed"
                    : selected.enabled
                      ? "Pause workflow"
                      : "Resume workflow"}
                </button>
                <button
                  disabled={busy || !canResearch || !selected.active_job_id}
                  onClick={() => void mutate(true)}
                >
                  Cancel active workflow run
                </button>
                <button
                  onClick={() =>
                    setComposer({
                      ...composer,
                      name: `${selected.name} copy`,
                      description: selected.description,
                      interval_value: selected.interval_ms,
                      interval_unit: "milliseconds",
                      max_runs: selected.max_runs,
                      first_run: "",
                      source: selected.alpha_id
                        ? {
                            alpha_id: selected.alpha_id,
                            alpha_revision: selected.alpha_revision,
                            label: `Saved alpha ${selected.alpha_id.slice(0, 8)} / r${selected.alpha_revision}`,
                          }
                        : selected.request
                          ? {
                              request: structuredClone(selected.request),
                              label: `Frozen ${selected.request.strategy_id}`,
                            }
                          : null,
                    })
                  }
                >
                  Copy workflow configuration
                </button>
              </div>
              <div className="workflow-counts">
                Queued {selected.queued} · Running {selected.running} ·
                Succeeded {selected.succeeded} · Failed {selected.failed} ·
                Cancelled {selected.cancelled} · Missed {selected.missed_slots}{" "}
                · Skipped {selected.skipped_slots}
              </div>
              {selected.pause_reason && (
                <div className="notice">
                  Pause reason: {selected.pause_reason}
                </div>
              )}
              {selected.last_dispatch_error && (
                <div className="notice error">
                  Dispatch error: {selected.last_dispatch_error}
                </div>
              )}
              <div className="workflow-source">
                Recipe SHA-256 {selected.request_sha256} · Actor{" "}
                {selected.actor_id}
                <details>
                  <summary>Immutable workflow recipe</summary>
                  <pre>{JSON.stringify(selected.request, null, 2)}</pre>
                </details>
              </div>
              <div className="table-scroll workflow-runs">
                <table>
                  <thead>
                    <tr>
                      <th>UTC slot</th>
                      <th>Status</th>
                      <th>Job</th>
                      <th>Alpha revision</th>
                      <th />
                    </tr>
                  </thead>
                  <tbody>
                    {runs.map((job) => (
                      <tr key={job.id}>
                        <td>{clock(job.workflow_slot_ms ?? "0")}</td>
                        <td>{job.status}</td>
                        <td title={job.id}>{job.id.slice(0, 12)}</td>
                        <td>
                          {job.alpha_id
                            ? `${job.alpha_id.slice(0, 8)} / r${job.alpha_revision}`
                            : "Embedded recipe"}
                        </td>
                        <td>
                          <button onClick={() => selectJob(job)}>
                            Inspect workflow run
                          </button>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              <div className="panel-toolbar">
                <span>{runTotal} admitted jobs</span>
                <button
                  disabled={!runOffset}
                  onClick={() => setRunOffset(Math.max(0, runOffset - 50))}
                >
                  Previous workflow runs
                </button>
                <button
                  disabled={runOffset + 50 >= runTotal}
                  onClick={() => setRunOffset(runOffset + 50)}
                >
                  Next workflow runs
                </button>
              </div>
            </>
          )}
        </section>
      </div>
    </div>
  );
}
