import { useCallback, useEffect, useRef, useState } from "react";
import { ApiError, request } from "./api";
import { usePermissions } from "./session";
import type { Job, ResearchRequest } from "./types";
export type AlphaSummary = {
  id: string;
  project_id: string;
  name: string;
  description: string;
  tags: string[];
  stage: string;
  revision: string;
  request_sha256: string;
  source_job_id: string;
  created_at_ms: string;
  updated_at_ms: string;
};
export type AlphaSnapshot = AlphaSummary & {
  request: ResearchRequest;
  hypothesis: string;
};
type Edit = {
  name: string;
  description: string;
  tags: string;
  hypothesis: string;
  request: ResearchRequest;
  source_job_id: string;
};
const fromSnapshot = (snapshot: AlphaSnapshot): Edit => ({
  name: snapshot.name,
  description: snapshot.description,
  tags: snapshot.tags.join(", "),
  hypothesis: snapshot.hypothesis,
  request: snapshot.request,
  source_job_id: snapshot.source_job_id,
});
export function AlphaLibrary({
  token,
  draft,
  setDraft,
  selected,
  runAlpha,
  onUnsavedChange,
}: {
  token: string;
  draft: ResearchRequest;
  setDraft: (draft: ResearchRequest) => void;
  selected: Job | null;
  runAlpha: (id: string, revision: string, label: string) => Promise<void>;
  onUnsavedChange: (dirty: boolean) => void;
}) {
  const { canResearch } = usePermissions();
  const [records, setRecords] = useState<AlphaSummary[]>([]);
  const [total, setTotal] = useState(0);
  const [offset, setOffset] = useState(0);
  const [search, setSearch] = useState("");
  const [tag, setTag] = useState("");
  const [archived, setArchived] = useState(false);
  const [snapshot, setSnapshot] = useState<AlphaSnapshot | null>(null);
  const [history, setHistory] = useState<AlphaSummary[]>([]);
  const [edit, setEdit] = useState<Edit | null>(null);
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [listError, setListError] = useState("");
  const [conflict, setConflict] = useState(false);
  const [pending, setPending] = useState<(() => void) | null>(null);
  const active = useRef<AbortController | null>(null);
  const listActive = useRef<AbortController | null>(null);
  const generation = useRef(0);
  const versions = useRef(0);
  const dirtyRef = useRef(false);
  dirtyRef.current = dirty;
  const owner = useRef(onUnsavedChange);
  owner.current = onUnsavedChange;
  const setChanged = (value: boolean) => {
    dirtyRef.current = value;
    setDirty(value);
    owner.current(value);
  };
  const refresh = useCallback(async () => {
    listActive.current?.abort();
    const controller = new AbortController();
    listActive.current = controller;
    try {
      const page = await request<{ alphas: AlphaSummary[]; total: string }>(
        `/alphas?${new URLSearchParams({ limit: "50", offset: String(offset), search, tag, include_archived: String(archived), project_id: "local" })}`,
        token,
        { signal: controller.signal },
      );
      if (!controller.signal.aborted) {
        setRecords(page.alphas);
        setTotal(Number(page.total));
        setListError("");
      }
    } catch (failure) {
      if (!controller.signal.aborted) setListError((failure as Error).message);
    }
  }, [token, offset, search, tag, archived]);
  useEffect(() => {
    const timer = setTimeout(() => void refresh(), 250);
    return () => clearTimeout(timer);
  }, [refresh]);
  useEffect(() => {
    const timer = setInterval(() => void refresh(), 30000);
    return () => clearInterval(timer);
  }, [refresh]);
  useEffect(
    () => () => {
      active.current?.abort();
      listActive.current?.abort();
      generation.current++;
      owner.current(false);
    },
    [],
  );
  useEffect(() => {
    active.current?.abort();
    generation.current++;
    setBusy(false);
  }, [token]);
  const load = async (id: string, revision = "0") => {
    active.current?.abort();
    const controller = new AbortController();
    active.current = controller;
    const version = ++generation.current;
    const editing = versions.current;
    setBusy(true);
    setError("");
    setConflict(false);
    try {
      const result = await request<AlphaSnapshot>(
        `/alphas/${id}?revision=${revision}`,
        token,
        { signal: controller.signal },
      );
      if (version !== generation.current || controller.signal.aborted) return;
      if (editing !== versions.current) {
        setError(
          "New edits were made while loading; your working alpha was preserved.",
        );
        return;
      }
      setSnapshot(result);
      setEdit(fromSnapshot(result));
      setChanged(false);
      setPending(null);
      const revisions = await request<{ revisions: AlphaSummary[] }>(
        `/alphas/${id}/revisions?limit=100`,
        token,
        { signal: controller.signal },
      );
      if (version === generation.current && !controller.signal.aborted)
        setHistory(revisions.revisions);
    } catch (failure) {
      if (version === generation.current && !controller.signal.aborted)
        setError((failure as Error).message);
    } finally {
      if (version === generation.current) setBusy(false);
    }
  };
  const choose = (action: () => void) => {
    if (dirtyRef.current) setPending(() => action);
    else action();
  };
  const create = (recipe: ResearchRequest, source_job_id = "") => {
    active.current?.abort();
    generation.current++;
    setBusy(false);
    setSnapshot(null);
    setHistory([]);
    setEdit({
      name: "",
      description: "",
      tags: "",
      hypothesis: "",
      request: structuredClone(recipe),
      source_job_id,
    });
    setError("");
    setConflict(false);
    versions.current++;
    setChanged(true);
  };
  const change = (key: keyof Edit, value: string) => {
    if (!edit) return;
    setEdit({ ...edit, [key]: value });
    versions.current++;
    setChanged(true);
  };
  const save = async (copy = false) => {
    if (!edit || !canResearch) return;
    const tags = edit.tags
      .split(",")
      .map((value) => value.trim())
      .filter(Boolean);
    if (tags.length > 16 || new Set(tags).size !== tags.length) {
      setError("Use at most 16 unique comma-separated tags.");
      return;
    }
    if (!edit.name.trim() || [...edit.name.trim()].length > 80) {
      setError("Name must contain 1 to 80 characters.");
      return;
    }
    active.current?.abort();
    const controller = new AbortController();
    active.current = controller;
    const version = ++generation.current;
    const editing = versions.current;
    setBusy(true);
    setError("");
    setConflict(false);
    const body = {
      ...edit,
      name: edit.name.trim(),
      tags,
      ...(!copy && snapshot
        ? { expected_revision: snapshot.revision }
        : { project_id: "local" }),
    };
    try {
      const saved = await request<AlphaSnapshot>(
        !copy && snapshot ? `/alphas/${snapshot.id}` : "/alphas",
        token,
        {
          method: !copy && snapshot ? "PUT" : "POST",
          signal: controller.signal,
          body: JSON.stringify(body),
        },
      );
      if (version !== generation.current || controller.signal.aborted) return;
      setSnapshot(saved);
      const unchanged = editing === versions.current;
      if (unchanged) setEdit(fromSnapshot(saved));
      setChanged(!unchanged);
      if (!unchanged)
        setError(
          "The submitted revision was saved; newer edits remain unsaved.",
        );
      void refresh();
      const revisions = await request<{ revisions: AlphaSummary[] }>(
        `/alphas/${saved.id}/revisions?limit=100`,
        token,
        { signal: controller.signal },
      );
      if (version === generation.current && !controller.signal.aborted)
        setHistory(revisions.revisions);
    } catch (failure) {
      if (version === generation.current && !controller.signal.aborted) {
        setConflict(failure instanceof ApiError && failure.status === 409);
        setError((failure as Error).message);
      }
    } finally {
      if (version === generation.current) setBusy(false);
    }
  };
  const archive = async () => {
    if (!snapshot || dirtyRef.current || !canResearch) return;
    active.current?.abort();
    const controller = new AbortController();
    active.current = controller;
    const version = ++generation.current;
    setBusy(true);
    setError("");
    try {
      const result = await request<AlphaSnapshot>(
        `/alphas/${snapshot.id}/archive`,
        token,
        {
          method: "POST",
          signal: controller.signal,
          body: JSON.stringify({
            expected_revision: snapshot.revision,
            archived: snapshot.stage !== "archived",
          }),
        },
      );
      if (version !== generation.current || controller.signal.aborted) return;
      setSnapshot(result);
      setEdit(fromSnapshot(result));
      void refresh();
    } catch (failure) {
      if (version === generation.current && !controller.signal.aborted) {
        setConflict(failure instanceof ApiError && failure.status === 409);
        setError((failure as Error).message);
      }
    } finally {
      if (version === generation.current) setBusy(false);
    }
  };
  const run = async () => {
    if (!snapshot || !canResearch) return;
    setBusy(true);
    setError("");
    try {
      await runAlpha(
        snapshot.id,
        snapshot.revision,
        `${snapshot.name} r${snapshot.revision}`,
      );
    } catch (failure) {
      setError((failure as Error).message);
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="alpha-library research-tool-panel">
      <div className="panel-toolbar">
        <input
          aria-label="Search alpha library"
          placeholder="Search saved alphas…"
          value={search}
          onChange={(e) => {
            setSearch(e.target.value);
            setOffset(0);
          }}
        />
        <input
          className="alpha-tag-filter"
          aria-label="Filter alpha tag"
          placeholder="Tag"
          value={tag}
          onChange={(e) => {
            setTag(e.target.value);
            setOffset(0);
          }}
        />
        <label>
          <input
            type="checkbox"
            checked={archived}
            onChange={(e) => {
              setArchived(e.target.checked);
              setOffset(0);
            }}
          />{" "}
          Archived
        </label>
        <button onClick={() => void refresh()}>Refresh library</button>
        <span>{total} records</span>
      </div>
      <div className="panel-toolbar">
        <span>Shared local project · immutable revisions</span>
        <button disabled={busy} onClick={() => choose(() => create(draft))}>
          Save draft as alpha
        </button>
        <button
          disabled={busy || !selected?.request}
          onClick={() =>
            selected?.request &&
            choose(() => create(selected.request!, selected.id))
          }
        >
          Save selected run recipe
        </button>
      </div>
      {listError && (
        <div className="error" role="alert">
          Library unavailable: {listError}
        </div>
      )}
      {pending && (
        <div className="workspace-prompt">
          <span>Unsaved alpha edits.</span>
          <button
            onClick={() => {
              const action = pending;
              setPending(null);
              action();
            }}
          >
            Discard alpha edits
          </button>
          <button onClick={() => setPending(null)}>Keep alpha editing</button>
        </div>
      )}
      <div className="alpha-library-body">
        <div className="alpha-list">
          <div className="table-scroll">
            <table aria-label="Saved alpha library">
              <thead>
                <tr>
                  <th>Name / tags</th>
                  <th>Revision / stage</th>
                </tr>
              </thead>
              <tbody>
                {records.map((record) => (
                  <tr
                    key={record.id}
                    className={record.id === snapshot?.id ? "selected-row" : ""}
                    onClick={() => choose(() => void load(record.id))}
                  >
                    <td>
                      <button
                        className="table-link"
                        onClick={(event) => {
                          event.stopPropagation();
                          choose(() => void load(record.id));
                        }}
                      >
                        {record.name}
                      </button>
                      <small>{record.tags.join(", ")}</small>
                    </td>
                    <td>
                      r{record.revision}
                      <br />
                      {record.stage}
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
              Previous alphas
            </button>
            <span>
              {Math.min(offset + 1, total)}–{Math.min(offset + 50, total)}/
              {total}
            </span>
            <button
              disabled={offset + 50 >= total}
              onClick={() => setOffset(offset + 50)}
            >
              Next alphas
            </button>
          </div>
        </div>
        <div className="alpha-inspector">
          {edit ? (
            <>
              <div className="panel-toolbar">
                <strong>
                  {snapshot
                    ? `${snapshot.name} / r${snapshot.revision}`
                    : "New alpha"}
                </strong>
                <span>{dirty ? "* Unsaved" : snapshot?.stage}</span>
                {snapshot && (
                  <select
                    aria-label="Alpha revision"
                    value={snapshot.revision}
                    disabled={busy}
                    onChange={(e) =>
                      choose(() => void load(snapshot.id, e.target.value))
                    }
                  >
                    {!history.some((h) => h.revision === snapshot.revision) && (
                      <option value={snapshot.revision}>
                        r{snapshot.revision}
                      </option>
                    )}
                    {history.map((h) => (
                      <option key={h.revision} value={h.revision}>
                        r{h.revision} / {h.stage}
                      </option>
                    ))}
                  </select>
                )}
              </div>
              <label>
                Name
                <input
                  aria-label="Alpha name"
                  value={edit.name}
                  maxLength={160}
                  onChange={(e) => change("name", e.target.value)}
                />
              </label>
              <label>
                Description
                <input
                  aria-label="Alpha description"
                  value={edit.description}
                  maxLength={512}
                  onChange={(e) => change("description", e.target.value)}
                />
              </label>
              <label>
                Tags
                <input
                  aria-label="Alpha tags"
                  value={edit.tags}
                  placeholder="Comma-separated;16 maximum"
                  onChange={(e) => change("tags", e.target.value)}
                />
              </label>
              <label>
                Hypothesis
                <textarea
                  aria-label="Alpha hypothesis"
                  value={edit.hypothesis}
                  maxLength={4096}
                  onChange={(e) => change("hypothesis", e.target.value)}
                />
              </label>
              <div className="alpha-recipe" title={snapshot?.request_sha256}>
                <code>{JSON.stringify(edit.request, null, 2)}</code>
              </div>
              <div className="help">
                {snapshot
                  ? `Recipe SHA-256 ${snapshot.request_sha256}`
                  : "The current recipe is captured for this new record."}
                {edit.source_job_id && ` · source run ${edit.source_job_id}`}
              </div>
              <div className="panel-toolbar">
                <button
                  disabled={busy || !dirty || !canResearch}
                  onClick={() => void save(false)}
                >
                  {snapshot ? "Save new revision" : "Create alpha"}
                </button>
                <button
                  disabled={busy || !snapshot || !canResearch}
                  onClick={() => void save(true)}
                >
                  Save alpha copy
                </button>
                <button
                  disabled={busy || dirty || !snapshot}
                  onClick={() => setDraft(snapshot!.request)}
                >
                  Load recipe into draft
                </button>
                <button
                  disabled={busy || dirty || !snapshot || !canResearch}
                  onClick={() => void run()}
                >
                  Run saved revision
                </button>
                <button
                  disabled={busy || dirty || !snapshot || !canResearch}
                  onClick={() => void archive()}
                >
                  {snapshot?.stage === "archived"
                    ? "Restore alpha"
                    : "Archive alpha"}
                </button>
                {dirty && (
                  <button
                    onClick={() => {
                      active.current?.abort();
                      generation.current++;
                      versions.current++;
                      setBusy(false);
                      setEdit(snapshot ? fromSnapshot(snapshot) : null);
                      setChanged(false);
                    }}
                  >
                    Discard alpha form
                  </button>
                )}
              </div>
            </>
          ) : (
            <div className="help">
              Select a saved alpha or capture the current draft. Metadata and
              recipes create immutable server revisions.
            </div>
          )}
        </div>
      </div>
      {error && (
        <div className="error" role="alert">
          {error}
          {conflict && snapshot && (
            <>
              <button onClick={() => choose(() => void load(snapshot.id))}>
                Reload alpha head
              </button>
              <button onClick={() => void save(true)}>
                Save conflict copy
              </button>
            </>
          )}
        </div>
      )}
    </div>
  );
}
