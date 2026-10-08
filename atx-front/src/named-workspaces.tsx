import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type RefObject,
} from "react";
import { ApiError, request } from "./api";
import { usePermissions } from "./session";
import {
  validateWorkspaceDocument,
  type LayoutPreset,
  type SavedWorkspace,
  type SavedWorkspaceSummary,
  type WorkspaceDocument,
} from "./workspace-document";

export type WorkspacePorts = {
  capture: () => WorkspaceDocument;
  apply: (document: WorkspaceDocument) => void;
  template: (preset: LayoutPreset) => void;
};
type Target =
  { kind: "saved"; id: string } | { kind: "template"; preset: LayoutPreset };
type Dialog = "save_as" | "rename" | "delete" | null;
const path = (id: string) => `/workspaces/${encodeURIComponent(id)}`;

export function useNamedWorkspaces(
  token: string,
  ports: RefObject<WorkspacePorts | null>,
  canResearch = true,
) {
  const [records, setRecords] = useState<SavedWorkspaceSummary[]>([]);
  const [active, setActive] = useState<SavedWorkspace | null>(null);
  const [preset, setPreset] = useState<LayoutPreset | null>("research");
  const [dirty, setDirty] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [listError, setListError] = useState("");
  const [conflict, setConflict] = useState(false);
  const [pending, setPending] = useState<Target | null>(null);
  const [dialog, setDialog] = useState<Dialog>(null);
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [menu, setMenu] = useState(false);
  const [ready, setReady] = useState(false);
  const changes = useRef(0);
  const activeRef = useRef(active);
  activeRef.current = active;
  const dirtyRef = useRef(dirty);
  dirtyRef.current = dirty;
  const pendingRef = useRef(pending);
  pendingRef.current = pending;
  const operation = useRef(0);
  const requestController = useRef<AbortController | null>(null);
  const listController = useRef<AbortController | null>(null);
  const listGeneration = useRef(0);
  const loadedLink = useRef(false);

  const markChanged = useCallback(() => {
    changes.current++;
    dirtyRef.current = true;
    setDirty(true);
  }, []);
  const begin = useCallback(() => {
    requestController.current?.abort();
    const controller = new AbortController();
    requestController.current = controller;
    const generation = ++operation.current;
    setBusy(true);
    setError("");
    setConflict(false);
    return { controller, generation };
  }, []);
  const refreshList = useCallback(async () => {
    listController.current?.abort();
    const controller = new AbortController();
    listController.current = controller;
    const generation = ++listGeneration.current;
    try {
      const found: SavedWorkspaceSummary[] = [];
      let total = 1;
      for (let offset = 0; offset < Math.min(total, 256); offset += 100) {
        const page = await request<{
          workspaces: SavedWorkspaceSummary[];
          total: string;
        }>(`/workspaces?limit=100&offset=${offset}`, token, {
          signal: controller.signal,
        });
        if (controller.signal.aborted || generation !== listGeneration.current)
          return;
        total = Number(page.total);
        found.push(...page.workspaces);
        if (!page.workspaces.length) break;
      }
      if (generation !== listGeneration.current) return;
      setRecords([
        ...new Map(found.map((record) => [record.id, record])).values(),
      ]);
      setListError("");
    } catch (failure) {
      if (!controller.signal.aborted && generation === listGeneration.current)
        setListError((failure as Error).message);
    }
  }, [token]);

  const replaceUrl = (id: string | null) => {
    const url = new URL(window.location.href);
    if (id) url.searchParams.set("workspace", id);
    else url.searchParams.delete("workspace");
    window.history.replaceState(null, "", url);
  };
  const performSwitch = useCallback(
    async (target: Target) => {
      if (!ports.current) return false;
      const { controller, generation } = begin();
      const version = changes.current;
      try {
        if (target.kind === "template") {
          ports.current.template(target.preset);
          activeRef.current = null;
          setActive(null);
          setPreset(target.preset);
          replaceUrl(null);
        } else {
          const record = await request<SavedWorkspace>(path(target.id), token, {
            signal: controller.signal,
          });
          if (generation !== operation.current || controller.signal.aborted)
            return false;
          validateWorkspaceDocument(record.document);
          if (version !== changes.current) {
            setPending(target);
            setError(
              "You edited the working workspace while loading. Those changes have been kept.",
            );
            return false;
          }
          ports.current.apply(record.document);
          activeRef.current = record;
          setActive(record);
          setPreset(null);
          replaceUrl(record.id);
        }
        changes.current++;
        dirtyRef.current = false;
        setDirty(false);
        setPending(null);
        setDialog(null);
        setMenu(false);
        return true;
      } catch (failure) {
        if (!controller.signal.aborted && generation === operation.current)
          setError((failure as Error).message);
        return false;
      } finally {
        if (generation === operation.current) setBusy(false);
      }
    },
    [token, ports, begin],
  );

  const switchTo = useCallback(
    (target: Target) => {
      setMenu(false);
      if (dirtyRef.current) {
        setPending(target);
        return;
      }
      void performSwitch(target);
    },
    [performSwitch],
  );
  const openDialog = (kind: Dialog, duplicate = false) => {
    setMenu(false);
    setError("");
    setDialog(kind);
    setName(
      duplicate
        ? `${activeRef.current?.name ?? "Workspace"} copy`
        : (activeRef.current?.name ?? ""),
    );
    setDescription(activeRef.current?.description ?? "");
  };
  const save = useCallback(async () => {
    if (!canResearch) {
      setError("Your current role cannot save shared workspaces.");
      return false;
    }
    const current = activeRef.current;
    if (!current || !ports.current) return false;
    const { controller, generation } = begin();
    const version = changes.current;
    try {
      const document = ports.current.capture();
      validateWorkspaceDocument(document);
      const saved = await request<SavedWorkspace>(path(current.id), token, {
        method: "PUT",
        signal: controller.signal,
        body: JSON.stringify({
          name: current.name,
          description: current.description,
          expected_revision: current.revision,
          document,
        }),
      });
      if (generation !== operation.current || controller.signal.aborted)
        return false;
      activeRef.current = saved;
      setActive(saved);
      const clean = version === changes.current;
      dirtyRef.current = !clean;
      setDirty(!clean);
      if (!clean)
        setError(
          "The submitted state was saved. Newer edits are still unsaved.",
        );
      void refreshList();
      return clean;
    } catch (failure) {
      if (generation === operation.current && !controller.signal.aborted) {
        if (failure instanceof ApiError && failure.status === 409) {
          setConflict(true);
          setError(
            "A newer server revision exists. Your working changes are preserved; reload it or save a copy.",
          );
        } else setError((failure as Error).message);
      }
      return false;
    } finally {
      if (generation === operation.current) setBusy(false);
    }
  }, [token, ports, begin, refreshList, canResearch]);

  const saveAndSwitch = async () => {
    if (!activeRef.current) {
      openDialog("save_as");
      return;
    }
    if (await save()) {
      const target = pendingRef.current;
      if (target) await performSwitch(target);
    }
  };
  const submitDialog = async () => {
    if (!canResearch) {
      setError("Your current role cannot edit shared workspaces.");
      return;
    }
    if (!ports.current || !dialog) return;
    const current = activeRef.current;
    const version = changes.current;
    const { controller, generation } = begin();
    try {
      if (dialog === "delete") {
        if (!current) return;
        await request<void>(path(current.id), token, {
          method: "DELETE",
          signal: controller.signal,
          body: JSON.stringify({ expected_revision: current.revision }),
        });
        if (generation !== operation.current || controller.signal.aborted)
          return;
        activeRef.current = null;
        setActive(null);
        setPreset(null);
        replaceUrl(null);
        dirtyRef.current = true;
        setDirty(true);
        setDialog(null);
        void refreshList();
        return;
      }
      const trimmed = name.trim();
      if (
        !trimmed ||
        [...trimmed].length > 80 ||
        new TextEncoder().encode(trimmed).byteLength > 320
      )
        throw new Error("Name must contain 1 to 80 characters.");
      if (new TextEncoder().encode(description).byteLength > 512)
        throw new Error("Description exceeds 512 UTF-8 bytes.");
      const document =
        dialog === "rename" && current
          ? current.document
          : ports.current.capture();
      validateWorkspaceDocument(document);
      const body = {
        name: trimmed,
        description,
        document,
        ...(dialog === "rename" && current
          ? { expected_revision: current.revision }
          : {}),
      };
      const saved = await request<SavedWorkspace>(
        dialog === "rename" && current ? path(current.id) : "/workspaces",
        token,
        {
          method: dialog === "rename" ? "PUT" : "POST",
          signal: controller.signal,
          body: JSON.stringify(body),
        },
      );
      if (generation !== operation.current || controller.signal.aborted) return;
      activeRef.current = saved;
      setActive(saved);
      setPreset(null);
      setDialog(null);
      replaceUrl(saved.id);
      if (dialog === "save_as") {
        const clean = version === changes.current;
        dirtyRef.current = !clean;
        setDirty(!clean);
        const target = pendingRef.current;
        if (target && clean) await performSwitch(target);
        else if (!clean)
          setError(
            "The submitted state was saved. Newer edits are still unsaved.",
          );
      }
      void refreshList();
    } catch (failure) {
      if (generation === operation.current && !controller.signal.aborted) {
        if (failure instanceof ApiError && failure.status === 409)
          setConflict(true);
        setError(
          failure instanceof ApiError && failure.status === 409
            ? "The record changed or its name is already in use. Working changes are preserved."
            : (failure as Error).message,
        );
      }
    } finally {
      if (generation === operation.current) setBusy(false);
    }
  };
  useEffect(() => {
    requestController.current?.abort();
    operation.current++;
    setBusy(false);
    void refreshList();
    const timer = setInterval(() => void refreshList(), 30_000);
    return () => {
      clearInterval(timer);
      requestController.current?.abort();
      listController.current?.abort();
      operation.current++;
      listGeneration.current++;
    };
  }, [token, refreshList]);
  useEffect(() => {
    if (!ready || loadedLink.current) return;
    const id = new URL(window.location.href).searchParams.get("workspace");
    if (id && /^[a-zA-Z0-9_-]{1,128}$/.test(id))
      void performSwitch({ kind: "saved", id }).then((loaded) => {
        if (loaded) loadedLink.current = true;
      });
  }, [ready, token, performSwitch]);
  return {
    records,
    active,
    preset,
    dirty,
    busy,
    error,
    listError,
    conflict,
    pending,
    dialog,
    name,
    description,
    menu,
    setName,
    setDescription,
    setMenu,
    setDialog,
    setPending,
    markChanged,
    refreshList,
    switchTo,
    performSwitch,
    save,
    openDialog,
    saveAndSwitch,
    submitDialog,
    initialize: (legacy: boolean) => {
      if (legacy) setPreset(null);
      setReady(true);
    },
  };
}
export type NamedWorkspaceManager = ReturnType<typeof useNamedWorkspaces>;
export function WorkspaceSelector({
  manager: m,
}: {
  manager: NamedWorkspaceManager;
}) {
  const { canResearch } = usePermissions();
  const current = m.active
    ? `saved:${m.active.id}`
    : m.preset
      ? `template:${m.preset}`
      : "local";
  const remote =
    m.active &&
    m.records.find((record) => record.id === m.active?.id)?.revision;
  return (
    <div className="workspace-tools">
      <label>
        Workspace{" "}
        <select
          aria-label="Workspace"
          value={current}
          disabled={m.busy}
          onChange={(e) => {
            const value = e.target.value;
            if (value === current) return;
            if (value.startsWith("template:"))
              m.switchTo({
                kind: "template",
                preset: value.slice(9) as LayoutPreset,
              });
            else if (value.startsWith("saved:"))
              m.switchTo({ kind: "saved", id: value.slice(6) });
          }}
        >
          {!m.active && !m.preset && (
            <option value="local">Recovered / unsaved layout</option>
          )}
          <optgroup label="Local templates">
            <option value="template:research">Research template</option>
            <option value="template:database">Database template</option>
            <option value="template:operations">Operations template</option>
          </optgroup>
          <optgroup label="Saved on server">
            {m.active &&
              !m.records.some((record) => record.id === m.active?.id) && (
                <option value={`saved:${m.active.id}`}>{m.active.name}</option>
              )}
            {m.records.map((record) => (
              <option key={record.id} value={`saved:${record.id}`}>
                {record.name}
              </option>
            ))}
          </optgroup>
        </select>
      </label>
      <button
        disabled={m.busy || !m.active || !m.dirty || !canResearch}
        onClick={() => void m.save()}
      >
        Save
      </button>
      <div className="menu-anchor">
        <button
          aria-label="Workspace actions"
          aria-expanded={m.menu}
          onClick={() => m.setMenu(!m.menu)}
        >
          Workspace ▾
        </button>
        {m.menu && (
          <div className="dropdown">
            <button
              disabled={m.busy || !canResearch}
              onClick={() => m.openDialog("save_as")}
            >
              Save as…
            </button>
            <button
              disabled={m.busy || !canResearch}
              onClick={() => m.openDialog("save_as", true)}
            >
              Duplicate…
            </button>
            <button
              disabled={!m.active || m.busy || !canResearch}
              onClick={() => m.openDialog("rename")}
            >
              Rename…
            </button>
            <button
              disabled={!m.active || m.busy || !canResearch}
              onClick={() => m.openDialog("delete")}
            >
              Delete…
            </button>
            <button
              disabled={!m.active || m.busy}
              onClick={() =>
                m.active && m.switchTo({ kind: "saved", id: m.active.id })
              }
            >
              Reload saved
            </button>
            <button
              onClick={() => {
                m.setMenu(false);
                void m.refreshList();
              }}
            >
              Refresh workspace list
            </button>
          </div>
        )}
      </div>
      <span className="workspace-status">
        {m.busy
          ? "Working…"
          : m.dirty
            ? "* Unsaved"
            : m.active
              ? `r${m.active.revision}`
              : "Not saved"}
        {remote && remote !== m.active?.revision ? " / server changed" : ""}
      </span>
    </div>
  );
}
export function WorkspaceMessages({
  manager: m,
}: {
  manager: NamedWorkspaceManager;
}) {
  const { canResearch } = usePermissions();
  return (
    <>
      {m.pending && (
        <div
          className="workspace-prompt"
          role="region"
          aria-label="Unsaved workspace changes"
        >
          <span>Unsaved workspace changes. Save before switching?</span>
          <button
            disabled={m.busy || !canResearch}
            onClick={() => void m.saveAndSwitch()}
          >
            Save and switch
          </button>
          <button
            disabled={m.busy}
            onClick={() => m.pending && void m.performSwitch(m.pending)}
          >
            Discard and switch
          </button>
          <button disabled={m.busy} onClick={() => m.setPending(null)}>
            Keep editing
          </button>
        </div>
      )}
      {m.dialog && (
        <form
          className="workspace-dialog"
          aria-label="Workspace action"
          onSubmit={(event) => {
            event.preventDefault();
            void m.submitDialog();
          }}
        >
          {m.dialog === "delete" ? (
            <span>
              Delete “{m.active?.name}” from the server? The working layout and
              edits will remain as an unsaved copy.
            </span>
          ) : (
            <>
              <label>
                Workspace name{" "}
                <input
                  aria-label="Workspace name"
                  value={m.name}
                  onChange={(e) => m.setName(e.target.value)}
                  required
                  maxLength={160}
                />
              </label>
              <label>
                Description{" "}
                <input
                  aria-label="Workspace description"
                  value={m.description}
                  onChange={(e) => m.setDescription(e.target.value)}
                  maxLength={512}
                />
              </label>
            </>
          )}
          <button type="submit" disabled={m.busy || !canResearch}>
            {m.dialog === "delete"
              ? "Delete saved workspace"
              : m.dialog === "rename"
                ? "Apply name"
                : "Create workspace"}
          </button>
          <button
            type="button"
            disabled={m.busy}
            onClick={() => m.setDialog(null)}
          >
            Cancel workspace action
          </button>
        </form>
      )}
      {(m.error || m.listError) && (
        <div className="workspace-error" role="alert">
          <span>
            {m.error || `Saved workspace list unavailable: ${m.listError}`}
          </span>
          {m.conflict && (
            <>
              <button
                disabled={m.busy || !m.active}
                onClick={() =>
                  m.active && m.switchTo({ kind: "saved", id: m.active.id })
                }
              >
                Reload latest
              </button>
              <button
                disabled={m.busy || !canResearch}
                onClick={() => m.openDialog("save_as", true)}
              >
                Save as copy
              </button>
            </>
          )}
        </div>
      )}
    </>
  );
}
