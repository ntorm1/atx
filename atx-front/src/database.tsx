import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { request } from "./api";
import {
  emptyDatabaseContext,
  type DatabaseContext,
} from "./workspace-document";

type Capabilities = {
  browse: boolean;
  query: boolean;
  max_rows: number;
  max_offset: number;
  max_query_bytes: number;
  timeout_ms: number;
};
type Source = {
  id: string;
  name: string;
  provider: string;
  read_only: boolean;
  capabilities: Capabilities;
};
type Table = { catalog: string; schema: string; name: string; kind: string };
type Catalog = { source_id: string; tables: Table[]; truncated: boolean };
type Cell = { text: string; is_null: boolean; truncated: boolean };
type Column = { name: string; type: string };
type DataResult = {
  source_id: string;
  columns: Column[];
  rows: { cells: Cell[] }[];
  has_more: boolean;
  truncated: boolean;
  elapsed_ms: string;
  warnings: string[];
};
const keyOf = (table: Table) =>
  JSON.stringify([table.catalog, table.schema, table.name]);
const schemaOf = (table: Table) =>
  JSON.stringify([table.catalog, table.schema]);
const quote = (identifier: string) => `"${identifier.replaceAll('"', '""')}"`;
const tableSql = (table: Table) =>
  [table.catalog, table.schema, table.name]
    .filter(Boolean)
    .map(quote)
    .join(".");
const tableLabel = (table: Table) =>
  [table.catalog, table.schema, table.name].filter(Boolean).join(".");
const numeric =
  /^(?:U?BIGINT|U?HUGEINT|U?INTEGER|U?SMALLINT|U?TINYINT|INT\d*|DECIMAL|NUMERIC|FLOAT|DOUBLE|REAL)/i;

function DatabaseResultTable({ result }: { result: DataResult }) {
  const scroll = useRef<HTMLDivElement>(null);
  const [scrollTop, setScrollTop] = useState(0);
  const [height, setHeight] = useState(120);
  useEffect(() => {
    const element = scroll.current;
    if (!element) return;
    const observer = new ResizeObserver(([entry]) =>
      setHeight(entry.contentRect.height),
    );
    observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    if (scroll.current) scroll.current.scrollTop = 0;
    setScrollTop(0);
  }, [result]);
  const widths = useMemo(
    () =>
      result.columns.map((column, index) => {
        const longest = result.rows.reduce(
          (length, row) =>
            Math.max(
              length,
              row.cells[index]?.is_null
                ? 4
                : (row.cells[index]?.text.length ?? 0),
            ),
          Math.max(column.name.length, column.type.length),
        );
        return Math.min(280, Math.max(85, longest * 7 + 15));
      }),
    [result],
  );
  const rowHeight = 23;
  const start = Math.max(
    0,
    Math.min(result.rows.length, Math.floor((scrollTop - 36) / rowHeight) - 8),
  );
  const end = Math.min(
    result.rows.length,
    start + Math.ceil(height / rowHeight) + 17,
  );
  const spacer = (rows: number, key: string) =>
    rows > 0 && (
      <tr key={key} aria-hidden="true" role="presentation">
        <td
          colSpan={result.columns.length || 1}
          style={{ height: rows * rowHeight, padding: 0, border: 0 }}
        />
      </tr>
    );
  return (
    <div
      className="table-scroll database-result-scroll"
      ref={scroll}
      onScroll={(e) => setScrollTop(e.currentTarget.scrollTop)}
    >
      <table
        aria-label="Database result"
        aria-rowcount={result.rows.length + 1}
        style={{
          tableLayout: "fixed",
          minWidth: "100%",
          width: widths.reduce((sum, width) => sum + width, 0),
        }}
      >
        <colgroup>
          {widths.map((width, i) => (
            <col key={i} style={{ width }} />
          ))}
        </colgroup>
        <thead>
          <tr>
            {result.columns.map((column, i) => (
              <th key={i} title={`${column.name} / ${column.type}`}>
                {column.name}
                <small className="database-column-type">{column.type}</small>
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {spacer(start, "before")}
          {result.rows.slice(start, end).map((row, i) => (
            <tr key={start + i} aria-rowindex={start + i + 2}>
              {result.columns.map((column, j) => {
                const cell = row.cells[j];
                return (
                  <td
                    key={j}
                    className={`${numeric.test(column.type) ? "numeric" : ""} ${cell?.is_null ? "database-null" : ""}`}
                    title={
                      cell?.truncated
                        ? `${cell.text} [Native cell text truncated]`
                        : cell?.text
                    }
                  >
                    {cell ? (
                      cell.is_null ? (
                        <i>NULL</i>
                      ) : (
                        cell.text
                      )
                    ) : (
                      <span className="muted">unavailable</span>
                    )}
                    {cell?.truncated && <span title="Truncated">…</span>}
                  </td>
                );
              })}
            </tr>
          ))}
          {spacer(result.rows.length - end, "after")}
        </tbody>
      </table>
      {!result.rows.length && (
        <div className="help">Query returned no rows.</div>
      )}
    </div>
  );
}

export function DatabasePanel({
  token,
  context = emptyDatabaseContext,
  onContextChange,
}: {
  token: string;
  context?: DatabaseContext;
  onContextChange?: (context: DatabaseContext, userChanged: boolean) => void;
}) {
  const restore = useRef<DatabaseContext | null>(context);
  const userChange = useRef(false);
  const lastSource = useRef("");
  const [sources, setSources] = useState<Source[]>([]);
  const [sourceId, setSourceId] = useState(context.source_id);
  const [catalog, setCatalog] = useState<Catalog | null>(null);
  const [tableKey, setTableKey] = useState(
    context.table
      ? JSON.stringify([context.catalog, context.schema, context.table])
      : "",
  );
  const [filter, setFilter] = useState(context.filter);
  const [sql, setSql] = useState(context.sql);
  const [limit, setLimit] = useState(context.page_size);
  const [offset, setOffset] = useState(context.offset);
  const [previousOffsets, setPreviousOffsets] = useState<number[]>(
    context.previous_offsets ?? [],
  );
  const [result, setResult] = useState<DataResult | null>(null);
  const [identity, setIdentity] = useState("");
  const [mode, setMode] = useState<"preview" | "query">(context.mode);
  const [loading, setLoading] = useState(false);
  const [catalogLoading, setCatalogLoading] = useState(false);
  const [error, setError] = useState("");
  const [epoch, setEpoch] = useState(0);
  const [previewEpoch, setPreviewEpoch] = useState(0);
  const generation = useRef(0);
  const active = useRef<AbortController | null>(null);
  const source = sources.find((s) => s.id === sourceId);
  const tableParts = tableKey
    ? (JSON.parse(tableKey) as string[])
    : ["", "", ""];
  const publicContext: DatabaseContext = {
    source_id: sourceId,
    catalog: tableParts[0],
    schema: tableParts[1],
    table: tableParts[2],
    sql,
    page_size: limit,
    offset,
    filter,
    mode,
    previous_offsets: previousOffsets,
  };
  const contextRef = useRef(publicContext);
  contextRef.current = publicContext;
  const publish = useRef(onContextChange);
  publish.current = onContextChange;
  useEffect(() => {
    publish.current?.(contextRef.current, userChange.current);
    userChange.current = false;
  }, [
    sourceId,
    tableKey,
    filter,
    sql,
    limit,
    offset,
    mode,
    previousOffsets,
    loading,
  ]);
  const table = catalog?.tables.find((t) => keyOf(t) === tableKey);
  const schemas = useMemo(
    () => [...new Set(catalog?.tables.map(schemaOf) ?? [])],
    [catalog],
  );
  const maxRows = Math.min(source?.capabilities.max_rows || 500, 500);
  const maxOffset = Math.min(
    source?.capabilities.max_offset ?? 1_000_000,
    1_000_000,
  );
  const sqlBytes = new TextEncoder().encode(sql).byteLength;
  const sqlLimit = Math.min(
    source?.capabilities.max_query_bytes || 65536,
    65536,
  );
  const clear = useCallback(() => {
    generation.current++;
    active.current?.abort();
    active.current = null;
    setResult(null);
    setIdentity("");
    setError("");
    setLoading(false);
  }, []);
  useEffect(() => {
    setLimit((current) => Math.min(current, maxRows));
  }, [maxRows]);

  useEffect(() => {
    const controller = new AbortController();
    request<{ sources: Source[] }>("/data/sources", token, {
      signal: controller.signal,
    })
      .then((data) => {
        if (controller.signal.aborted) return;
        setSources(data.sources);
        const current = contextRef.current.source_id;
        if (current && !data.sources.some((s) => s.id === current))
          setError(
            "The saved database source is unavailable. Select a configured source to continue.",
          );
        setSourceId((value) => value || data.sources[0]?.id || "");
      })
      .catch((e: Error) => {
        if (!controller.signal.aborted) setError(e.message);
      });
    return () => controller.abort();
  }, [token, epoch]);

  useEffect(() => {
    clear();
    setCatalog(null);
    setCatalogLoading(false);
    if (!sourceId || !source) return;
    const saved =
      restore.current?.source_id === sourceId
        ? restore.current
        : lastSource.current === sourceId
          ? contextRef.current
          : null;
    lastSource.current = sourceId;
    if (!saved) {
      setTableKey("");
      setOffset(0);
      setPreviousOffsets([]);
      setSql("");
    }
    const controller = new AbortController();
    setCatalogLoading(true);
    request<Catalog>(
      `/data/catalog?${new URLSearchParams({ source_id: sourceId })}`,
      token,
      { signal: controller.signal },
    )
      .then((data) => {
        if (controller.signal.aborted) return;
        setCatalog(data);
        const intended = saved?.table
          ? data.tables.find(
              (t) =>
                t.catalog === saved.catalog &&
                t.schema === saved.schema &&
                t.name === saved.table,
            )
          : data.tables[0];
        restore.current = null;
        if (intended) {
          setTableKey(keyOf(intended));
          setSql(saved ? saved.sql : `SELECT * FROM ${tableSql(intended)}`);
          setMode(saved?.mode ?? "preview");
          setOffset(saved?.offset ?? 0);
          setPreviousOffsets(saved?.previous_offsets ?? []);
        } else if (saved?.table) {
          setError(
            "The saved table is no longer available. Your SQL draft has been preserved; select another table.",
          );
        }
      })
      .catch((e: Error) => {
        if (!controller.signal.aborted) setError(e.message);
      })
      .finally(() => {
        if (!controller.signal.aborted) setCatalogLoading(false);
      });
    return () => controller.abort();
  }, [sourceId, source?.id, token, epoch, clear]);

  const execute = useCallback(
    async (query: boolean) => {
      if ((query ? "query" : "preview") !== mode) userChange.current = true;
      clear();
      if (!sourceId || (!query && !table)) return;
      const current = ++generation.current;
      const controller = new AbortController();
      active.current = controller;
      setLoading(true);
      setMode(query ? "query" : "preview");
      try {
        const data = query
          ? await request<DataResult>("/data/query", token, {
              method: "POST",
              signal: controller.signal,
              body: JSON.stringify({
                source_id: sourceId,
                sql,
                max_rows: limit,
              }),
            })
          : await request<DataResult>(
              `/data/preview?${new URLSearchParams({
                source_id: sourceId,
                catalog: table!.catalog,
                schema: table!.schema,
                table: table!.name,
                limit: String(limit),
                offset: String(offset),
              })}`,
              token,
              { signal: controller.signal },
            );
        if (current !== generation.current) return;
        setResult(data);
        setIdentity(
          query
            ? `Query / ${source?.name ?? sourceId}`
            : `${tableLabel(table!)} / offset ${offset}`,
        );
      } catch (e) {
        if (current === generation.current && !controller.signal.aborted)
          setError((e as Error).message);
      } finally {
        if (current === generation.current) {
          setLoading(false);
          active.current = null;
        }
      }
    },
    [clear, sourceId, table, token, sql, limit, offset, source?.name, mode],
  );

  // Changes to browsing identity request a fresh preview; editing SQL alone
  // invalidates query evidence without submitting or running a different query.
  useEffect(() => {
    if (mode === "preview" && table && source?.capabilities.browse)
      void execute(false);
    return () => {
      generation.current++;
      active.current?.abort();
    };
    // SQL edits must not trigger preview requests or implicit execution.
  }, [
    tableKey,
    offset,
    limit,
    token,
    catalog,
    source?.capabilities.browse,
    previewEpoch,
  ]);

  const selectTable = (t: Table) => {
    userChange.current = true;
    restore.current = null;
    clear();
    setTableKey(keyOf(t));
    setOffset(0);
    setPreviousOffsets([]);
    setMode("preview");
    setPreviewEpoch((current) => current + 1);
    setSql(`SELECT * FROM ${tableSql(t)}`);
  };
  const queryAllowed = Boolean(
    source?.capabilities.query &&
    sql.trim() &&
    sqlBytes <= sqlLimit &&
    !loading,
  );

  return (
    <div className="database-panel">
      <div className="panel-toolbar database-toolbar">
        <label>
          Source{" "}
          <select
            aria-label="Database source"
            value={sourceId}
            onChange={(e) => {
              if (e.target.value === sourceId) return;
              userChange.current = true;
              restore.current = null;
              clear();
              setSourceId(e.target.value);
            }}
          >
            {!sources.length && <option value="">No configured sources</option>}
            {sourceId && !source && (
              <option value={sourceId}>Unavailable: {sourceId}</option>
            )}
            {sources.map((s) => (
              <option key={s.id} value={s.id}>
                {s.name} / {s.provider}
              </option>
            ))}
          </select>
        </label>
        <button
          onClick={() => {
            clear();
            setEpoch((n) => n + 1);
          }}
        >
          Refresh catalog
        </button>
        <span className="spacer" />
        <span>Read only</span>
      </div>
      <div className="database-body">
        <aside className="database-catalog">
          <label className="database-field">
            Schema
            <select
              aria-label="Database schema"
              value={table ? schemaOf(table) : ""}
              disabled={!schemas.length}
              onChange={(e) => {
                const next = catalog?.tables.find(
                  (t) => schemaOf(t) === e.target.value,
                );
                if (next) selectTable(next);
              }}
            >
              {!schemas.length && (
                <option value="">
                  {catalogLoading ? "Loading…" : "No schemas"}
                </option>
              )}
              {schemas.map((s) => (
                <option key={s} value={s}>
                  {(JSON.parse(s) as string[]).filter(Boolean).join(".") ||
                    "Default namespace"}
                </option>
              ))}
            </select>
          </label>
          <input
            aria-label="Filter database tables"
            placeholder="Filter tables…"
            value={filter}
            onChange={(e) => {
              userChange.current = true;
              setFilter(e.target.value);
            }}
          />
          <div
            className="database-table-list"
            role="list"
            aria-label="Database tables"
          >
            {catalog?.tables
              .filter(
                (t) =>
                  (!table || schemaOf(t) === schemaOf(table)) &&
                  `${t.name} ${t.kind}`
                    .toLowerCase()
                    .includes(filter.toLowerCase()),
              )
              .map((t) => (
                <button
                  role="listitem"
                  key={keyOf(t)}
                  className={tableKey === keyOf(t) ? "selected" : ""}
                  title={`${tableLabel(t)} / ${t.kind}`}
                  onClick={() => selectTable(t)}
                >
                  <span>{t.name}</span>
                  <small>{t.kind.toLowerCase()}</small>
                </button>
              ))}
            {catalog && !catalog.tables.length && (
              <div className="help">No visible tables.</div>
            )}
          </div>
          {catalog?.truncated && (
            <div className="notice">
              Catalog limit reached; some tables are not listed.
            </div>
          )}
          <div className="database-limits">
            {maxRows} rows / page
            <br />
            {source
              ? `${source.capabilities.timeout_ms / 1000}s query deadline`
              : "Native provider not configured"}
          </div>
        </aside>
        <section className="database-results">
          <form
            className="database-query"
            onSubmit={(e) => {
              e.preventDefault();
              if (queryAllowed) void execute(true);
            }}
          >
            <label htmlFor="database-sql">
              Read-only SELECT query <span>Ctrl+Enter</span>
            </label>
            <textarea
              id="database-sql"
              aria-label="Database SQL query"
              spellCheck={false}
              value={sql}
              maxLength={65536}
              onChange={(e) => {
                userChange.current = true;
                if (mode === "query") clear();
                setSql(e.target.value);
              }}
              onKeyDown={(e) => {
                if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
                  e.preventDefault();
                  if (queryAllowed) void execute(true);
                }
              }}
            />
            <div className="panel-toolbar">
              <button type="submit" disabled={!queryAllowed}>
                Run SELECT
              </button>
              <button
                type="button"
                disabled={!table || loading || !source?.capabilities.browse}
                title="Preview order is not guaranteed; use SELECT with ORDER BY for reproducible pages. Database operations are read-only."
                onClick={() => void execute(false)}
              >
                Table preview
              </button>
              {loading && (
                <button type="button" onClick={clear}>
                  Cancel request
                </button>
              )}
              <label>
                Rows{" "}
                <select
                  aria-label="Database page size"
                  value={limit}
                  onChange={(e) => {
                    if (Number(e.target.value) === limit) return;
                    userChange.current = true;
                    clear();
                    setOffset(0);
                    setPreviousOffsets([]);
                    setLimit(Number(e.target.value));
                  }}
                >
                  {[...new Set([50, 100, 200, 500, maxRows])]
                    .sort((a, b) => a - b)
                    .filter((n) => n <= maxRows)
                    .map((n) => (
                      <option key={n}>{n}</option>
                    ))}
                </select>
              </label>
              <span className="spacer" />
              <span>
                {sqlBytes} / {sqlLimit} bytes
              </span>
            </div>
          </form>
          {error && (
            <div className="error" role="alert">
              {error}
            </div>
          )}
          {sqlBytes > sqlLimit && (
            <div className="error">
              Query exceeds the provider's UTF-8 byte limit.
            </div>
          )}
          <div className="database-result-identity">
            {loading
              ? "Reading native database…"
              : identity || "No query result selected"}
          </div>
          {result ? (
            <DatabaseResultTable result={result} />
          ) : (
            <div className="table-scroll">
              <div className="help">
                {source
                  ? "Select a table or run a read-only SELECT query."
                  : "Configure a native database source to browse data."}
              </div>
            </div>
          )}
          {result?.warnings.map((warning, i) => (
            <div className="notice" key={i}>
              {warning}
            </div>
          ))}
          <div className="panel-toolbar database-pagination">
            <span>
              {result
                ? `${result.rows.length} rows / ${result.elapsed_ms} ms${result.has_more ? (mode === "query" && result.rows.length >= limit ? " / row limit reached" : " / more rows available") : ""}${result.truncated ? " / data truncated" : ""}`
                : "No rows loaded"}
            </span>
            <span className="spacer" />
            <button
              disabled={
                mode !== "preview" || !previousOffsets.length || loading
              }
              onClick={() => {
                userChange.current = true;
                clear();
                setOffset(previousOffsets[previousOffsets.length - 1]);
                setPreviousOffsets((history) => history.slice(0, -1));
              }}
            >
              Previous page
            </button>
            <button
              disabled={
                mode !== "preview" ||
                !result?.has_more ||
                !result.rows.length ||
                offset + result.rows.length > maxOffset ||
                loading
              }
              onClick={() => {
                userChange.current = true;
                clear();
                setPreviousOffsets((history) => [
                  ...history.slice(-999),
                  offset,
                ]);
                setOffset(offset + result!.rows.length);
              }}
            >
              Next page
            </button>
          </div>
        </section>
      </div>
    </div>
  );
}
