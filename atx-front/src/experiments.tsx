import { useCallback, useEffect, useRef, useState } from "react";
import { request } from "./api";
import { usePermissions } from "./session";
import type { Job, ResearchRequest } from "./types";
import type { AlphaSummary } from "./alpha-library";
type Member = {
  label: string;
  request?: ResearchRequest;
  alpha_id?: string;
  alpha_revision?: string;
};
export type ExperimentDraft = {
  name: string;
  description: string;
  schedule: string;
  expressions: string;
  simulations: Member[];
};
export const emptyExperimentDraft: ExperimentDraft = {
  name: "",
  description: "",
  schedule: "",
  expressions: "",
  simulations: [],
};
type Experiment = {
  id: string;
  name: string;
  description: string;
  created_at_ms: string;
  scheduled_at_ms: string;
  status: string;
  progress: number;
  total: number;
  queued: number;
  running: number;
  succeeded: number;
  failed: number;
  cancelled: number;
  jobs: Job[];
};
const terminal = (status: string) =>
  ["succeeded", "failed", "cancelled", "completed", "partial"].includes(status);
export function ExperimentsPanel({
  token,
  draft,
  composer,
  setComposer,
  selectJob,
  onSubmitted,
}: {
  token: string;
  draft: ResearchRequest;
  composer: ExperimentDraft;
  setComposer: (next: ExperimentDraft) => void;
  selectJob: (job: Job) => void;
  onSubmitted: () => void;
}) {
  const { canResearch } = usePermissions();
  const [records, setRecords] = useState<Experiment[]>([]);
  const [total, setTotal] = useState(0),
    [offset, setOffset] = useState(0);
  const [selected, setSelected] = useState<Experiment | null>(null);
  const [alphas, setAlphas] = useState<AlphaSummary[]>([]),
    [alphaId, setAlphaId] = useState("");
  const [busy, setBusy] = useState(false),
    [error, setError] = useState(""),
    [listError, setListError] = useState("");
  const [alphaListError, setAlphaListError] = useState("");
  const active = useRef<AbortController | null>(null),
    listActive = useRef<AbortController | null>(null),
    generation = useRef(0),
    selectedId = useRef("");
  const writeActive = useRef<AbortController | null>(null),
    writeGeneration = useRef(0);
  const idempotency = useRef<{ signature: string; key: string } | null>(null);
  const update = (key: keyof ExperimentDraft, value: unknown) =>
    setComposer({ ...composer, [key]: value });
  const load = useCallback(
    async (id: string) => {
      active.current?.abort();
      const controller = new AbortController();
      active.current = controller;
      const version = ++generation.current;
      selectedId.current = id;
      try {
        const record = await request<Experiment>(`/experiments/${id}`, token, {
          signal: controller.signal,
        });
        if (
          version === generation.current &&
          !controller.signal.aborted &&
          selectedId.current === id
        )
          setSelected(record);
      } catch (failure) {
        if (version === generation.current && !controller.signal.aborted)
          setError((failure as Error).message);
      }
    },
    [token],
  );
  const refresh = useCallback(async () => {
    listActive.current?.abort();
    const controller = new AbortController();
    listActive.current = controller;
    try {
      const list = await request<{ experiments: Experiment[]; total: string }>(
        `/experiments?limit=50&offset=${offset}`,
        token,
        { signal: controller.signal },
      );
      if (controller.signal.aborted) return;
      setRecords(list.experiments);
      setTotal(Number(list.total));
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
      active.current?.abort();
      generation.current++;
    };
  }, [refresh]);
  useEffect(() => {
    setBusy(false);
    return () => {
      writeActive.current?.abort();
      writeGeneration.current++;
    };
  }, [token]);
  useEffect(() => {
    const controller = new AbortController();
    request<{ alphas: AlphaSummary[] }>("/alphas?limit=100", token, {
      signal: controller.signal,
    })
      .then((page) => {
        if (!controller.signal.aborted) {
          setAlphas(page.alphas);
          setAlphaListError("");
          setAlphaId((current) =>
            page.alphas.some((a) => a.id === current)
              ? current
              : (page.alphas[0]?.id ?? ""),
          );
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted)
          setAlphaListError(`Saved alpha list unavailable: ${failure.message}`);
      });
    return () => controller.abort();
  }, [token, records]);
  const add = (members: Member[]) => {
    if (composer.simulations.length + members.length > 32) {
      setError("An experiment supports at most 32 simulations.");
      return;
    }
    setComposer({
      ...composer,
      simulations: [...composer.simulations, ...members],
    });
    setError("");
  };
  const addExpressions = () => {
    const expressions = composer.expressions
      .split("\n")
      .map((text) => text.trim())
      .filter(Boolean);
    if (!expressions.length) {
      setError("Enter one expression per line.");
      return;
    }
    if (!["synthetic_demo", "real_dsl"].includes(draft.mode)) {
      setError("Expression variants require an editable DSL research recipe.");
      return;
    }
    add(
      expressions.map((strategy_dsl, index) => ({
        label: `Variant ${composer.simulations.length + index + 1}`,
        request: { ...structuredClone(draft), strategy_dsl },
      })),
    );
  };
  const submit = async () => {
    if (!canResearch) return;
    const scheduled_at_ms = composer.schedule
      ? new Date(composer.schedule).getTime()
      : 0;
    if (!composer.name.trim() || !composer.simulations.length) {
      setError("Name the experiment and add at least one simulation.");
      return;
    }
    if (
      composer.schedule &&
      (!Number.isFinite(scheduled_at_ms) || scheduled_at_ms <= Date.now())
    ) {
      setError("Choose a future schedule time.");
      return;
    }
    const body = {
      name: composer.name.trim(),
      description: composer.description,
      simulations: composer.simulations,
      scheduled_at_ms,
    };
    const signature = JSON.stringify(body);
    if (idempotency.current?.signature !== signature)
      idempotency.current = { signature, key: crypto.randomUUID() };
    writeActive.current?.abort();
    const controller = new AbortController();
    writeActive.current = controller;
    const version = ++writeGeneration.current;
    setBusy(true);
    setError("");
    try {
      const record = await request<Experiment>("/experiments", token, {
        method: "POST",
        signal: controller.signal,
        body: JSON.stringify({
          ...body,
          idempotency_key: idempotency.current.key,
        }),
      });
      if (version !== writeGeneration.current || controller.signal.aborted)
        return;
      idempotency.current = null;
      selectedId.current = record.id;
      setSelected(record);
      void refresh();
      onSubmitted();
    } catch (failure) {
      if (version === writeGeneration.current && !controller.signal.aborted)
        setError((failure as Error).message);
    } finally {
      if (version === writeGeneration.current) setBusy(false);
    }
  };
  const cancel = async () => {
    if (!selected || !canResearch) return;
    writeActive.current?.abort();
    const controller = new AbortController();
    writeActive.current = controller;
    const version = ++writeGeneration.current;
    setBusy(true);
    setError("");
    try {
      const record = await request<Experiment>(
        `/experiments/${selected.id}/cancel`,
        token,
        { method: "POST", signal: controller.signal },
      );
      if (version !== writeGeneration.current || controller.signal.aborted)
        return;
      if (selectedId.current === record.id) setSelected(record);
      void refresh();
    } catch (failure) {
      if (version === writeGeneration.current && !controller.signal.aborted)
        setError((failure as Error).message);
    } finally {
      if (version === writeGeneration.current) setBusy(false);
    }
  };
  return (
    <div className="experiments-panel research-tool-panel">
      <div className="panel-toolbar">
        <strong>Experiments</strong>
        <span>Atomic cohorts · 1–32 simulations</span>
        <button onClick={() => void refresh()}>Refresh experiments</button>
        <span>{total} records</span>
      </div>
      <div className="experiments-body">
        <div className="experiment-composer">
          <div className="panel-toolbar">
            Draft cohort · frozen recipes / immutable alpha revisions
          </div>
          <label>
            Name
            <input
              aria-label="Experiment name"
              value={composer.name}
              maxLength={160}
              onChange={(e) => update("name", e.target.value)}
            />
          </label>
          <label>
            Description
            <input
              aria-label="Experiment description"
              value={composer.description}
              maxLength={512}
              onChange={(e) => update("description", e.target.value)}
            />
          </label>
          <label>
            Schedule
            <input
              aria-label="Experiment schedule"
              type="datetime-local"
              value={composer.schedule}
              onChange={(e) => update("schedule", e.target.value)}
            />
          </label>
          <div className="panel-toolbar">
            <button
              disabled={busy || composer.simulations.length >= 32}
              onClick={() =>
                add([
                  {
                    label: `Draft ${composer.simulations.length + 1}`,
                    request: structuredClone(draft),
                  },
                ])
              }
            >
              Add current recipe
            </button>
            <select
              aria-label="Experiment saved alpha"
              value={alphaId}
              onChange={(e) => setAlphaId(e.target.value)}
            >
              {!alphas.length && <option value="">No saved alpha</option>}
              {alphas.map((alpha) => (
                <option key={alpha.id} value={alpha.id}>
                  {alpha.name} r{alpha.revision}
                </option>
              ))}
            </select>
            <button
              disabled={busy || !alphaId || composer.simulations.length >= 32}
              onClick={() => {
                const alpha = alphas.find((a) => a.id === alphaId);
                if (alpha)
                  add([
                    {
                      label: alpha.name,
                      alpha_id: alpha.id,
                      alpha_revision: alpha.revision,
                    },
                  ]);
              }}
            >
              Add saved revision
            </button>
          </div>
          <textarea
            aria-label="Experiment expression variants"
            placeholder="One DSL expression per line; each captures the current dataset, costs and execution settings"
            value={composer.expressions}
            onChange={(e) => update("expressions", e.target.value)}
          />
          <div className="panel-toolbar">
            <button
              disabled={
                busy || !["real_dsl", "synthetic_demo"].includes(draft.mode)
              }
              onClick={addExpressions}
            >
              Add expression variants
            </button>
            <span>{composer.simulations.length}/32 members</span>
            <button
              disabled={busy}
              onClick={() => setComposer({ ...composer, simulations: [] })}
            >
              Clear cohort
            </button>
          </div>
          <table aria-label="Draft experiment simulations">
            <thead>
              <tr>
                <th>Label</th>
                <th>Frozen source</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {composer.simulations.map((member, index) => (
                <tr key={index}>
                  <td>
                    <input
                      aria-label={`Simulation label ${index + 1}`}
                      value={member.label}
                      onChange={(e) =>
                        setComposer({
                          ...composer,
                          simulations: composer.simulations.map((current, i) =>
                            i === index
                              ? { ...current, label: e.target.value }
                              : current,
                          ),
                        })
                      }
                    />
                  </td>
                  <td title={JSON.stringify(member.request ?? member)}>
                    {member.request?.strategy_dsl ??
                      member.request?.mode ??
                      `Alpha ${member.alpha_id?.slice(0, 8)} r${member.alpha_revision}`}
                  </td>
                  <td>
                    <button
                      aria-label={`Remove simulation ${index + 1}`}
                      onClick={() =>
                        setComposer({
                          ...composer,
                          simulations: composer.simulations.filter(
                            (_, i) => i !== index,
                          ),
                        })
                      }
                    >
                      ×
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          <div className="panel-toolbar">
            <button
              disabled={
                busy ||
                !canResearch ||
                !composer.name.trim() ||
                !composer.simulations.length
              }
              onClick={() => void submit()}
            >
              {busy
                ? "Submitting…"
                : composer.schedule
                  ? "Schedule experiment"
                  : "Run experiment"}
            </button>
            <span>
              Blank schedule starts now. Admission is atomic; each job retains
              its lineage.
            </span>
          </div>
        </div>
        <div className="experiment-history">
          <div className="table-scroll">
            <table aria-label="Experiment history">
              <thead>
                <tr>
                  <th>Name</th>
                  <th>Status</th>
                  <th>Members / done</th>
                </tr>
              </thead>
              <tbody>
                {records.map((record) => (
                  <tr
                    key={record.id}
                    className={record.id === selected?.id ? "selected-row" : ""}
                    onClick={() => {
                      setError("");
                      void load(record.id);
                    }}
                  >
                    <td>
                      <button
                        className="table-link"
                        onClick={(e) => {
                          e.stopPropagation();
                          void load(record.id);
                        }}
                      >
                        {record.name}
                      </button>
                    </td>
                    <td>{record.status}</td>
                    <td>
                      {record.total}/
                      {record.succeeded + record.failed + record.cancelled}
                    </td>
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
              Previous experiments
            </button>
            <span>
              {Math.min(offset + 1, total)}–{Math.min(offset + 50, total)}/
              {total}
            </span>
            <button
              disabled={offset + 50 >= total}
              onClick={() => setOffset(offset + 50)}
            >
              Next experiments
            </button>
          </div>
          {selected && (
            <>
              <div className="panel-toolbar">
                <strong>{selected.name}</strong>
                <span>
                  {selected.status} · {Math.round(selected.progress * 100)}%
                </span>
                <button
                  disabled={
                    busy ||
                    !canResearch ||
                    terminal(selected.status) ||
                    selected.queued + selected.running === 0
                  }
                  onClick={() => void cancel()}
                >
                  Cancel experiment
                </button>
              </div>
              <div className="help">
                Queued {selected.queued} · running {selected.running} ·
                succeeded {selected.succeeded} · failed {selected.failed} ·
                cancelled {selected.cancelled}
              </div>
              <div className="table-scroll">
                <table aria-label="Experiment member jobs">
                  <thead>
                    <tr>
                      <th>Run / lineage</th>
                      <th>Status</th>
                      <th>Progress</th>
                      <th />
                    </tr>
                  </thead>
                  <tbody>
                    {selected.jobs.map((job) => (
                      <tr key={job.id}>
                        <td title={job.id}>
                          {job.label || job.id.slice(0, 10)}
                          {job.alpha_id && (
                            <small>
                              Alpha {job.alpha_id.slice(0, 8)} r
                              {job.alpha_revision}
                            </small>
                          )}
                        </td>
                        <td>{job.status}</td>
                        <td>{Math.round(job.progress * 100)}%</td>
                        <td>
                          <button onClick={() => selectJob(job)}>
                            Inspect run
                          </button>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </>
          )}
        </div>
      </div>
      {(error || listError || alphaListError) && (
        <div className="error" role="alert">
          {error || listError || alphaListError}
        </div>
      )}
    </div>
  );
}
