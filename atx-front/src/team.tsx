import { useEffect, useState } from "react";
import { request } from "./api";
import { usePermissions, type Principal } from "./session";
type Audit = {
  sequence: string;
  entity_type: string;
  entity_id: string;
  action: string;
  actor_id: string;
  revision: string;
  metadata: Record<string, unknown>;
  created_at_ms: string;
};
export function TeamPanel({
  token,
  active,
}: {
  token: string;
  active: boolean;
}) {
  const {
    session,
    error: sessionError,
    refresh: refreshSession,
  } = usePermissions();
  const [members, setMembers] = useState<Principal[]>([]);
  const [records, setRecords] = useState<Audit[]>([]);
  const [offset, setOffset] = useState(0),
    [total, setTotal] = useState(0);
  const [actor, setActor] = useState(""),
    [entity, setEntity] = useState("");
  const [error, setError] = useState("");
  const [memberError, setMemberError] = useState("");
  const [refreshKey, setRefreshKey] = useState(0);
  useEffect(() => {
    const controller = new AbortController();
    request<{ principals: Principal[] }>("/team", token, {
      signal: controller.signal,
    })
      .then((page) => {
        if (!controller.signal.aborted) {
          setMembers(page.principals);
          setMemberError("");
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) setMemberError(failure.message);
      });
    return () => controller.abort();
  }, [token, refreshKey, active]);
  useEffect(() => {
    const controller = new AbortController();
    request<{ records: Audit[]; total: string }>(
      `/audit?${new URLSearchParams({ limit: "50", offset: String(offset), actor_id: actor, entity_type: entity })}`,
      token,
      { signal: controller.signal },
    )
      .then((page) => {
        if (!controller.signal.aborted) {
          setRecords(page.records);
          setTotal(Number(page.total));
          setError("");
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) {
          setRecords([]);
          setError(failure.message);
        }
      });
    return () => controller.abort();
  }, [token, refreshKey, actor, entity, offset, active]);
  return (
    <div className="research-tool-panel team-panel">
      <div className="panel-toolbar">
        <strong>{session?.team_name || "Team / audit"}</strong>
        <span title="One shared research project. Roles are operator configured; this does not create private project boundaries.">
          Shared project: {session?.project_id || "unavailable"}
        </span>
        <button
          onClick={() => {
            setRefreshKey((key) => key + 1);
            refreshSession();
          }}
        >
          Refresh team
        </button>
      </div>
      {(error || memberError || sessionError) && (
        <div className="notice error" role="alert">
          {error || memberError || sessionError}
        </div>
      )}
      <div className="table-scroll team-members">
        <table>
          <thead>
            <tr>
              <th>Member</th>
              <th>Actor ID</th>
              <th>Role</th>
              <th>Credential expiry</th>
            </tr>
          </thead>
          <tbody>
            {members.map((member) => (
              <tr key={member.id}>
                <td>{member.name}</td>
                <td>{member.id}</td>
                <td>{member.role}</td>
                <td>
                  {Number(member.expires_at_ms)
                    ? new Date(Number(member.expires_at_ms)).toLocaleString()
                    : "No configured expiry"}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <div className="panel-toolbar">
        <strong>Durable audit</strong>
        <label>
          Actor{" "}
          <select
            aria-label="Audit actor"
            value={actor}
            onChange={(e) => {
              setActor(e.target.value);
              setOffset(0);
            }}
          >
            <option value="">All actors</option>
            {members.map((member) => (
              <option key={member.id} value={member.id}>
                {member.name}
              </option>
            ))}
          </select>
        </label>
        <label>
          Entity{" "}
          <select
            aria-label="Audit entity"
            value={entity}
            onChange={(e) => {
              setEntity(e.target.value);
              setOffset(0);
            }}
          >
            <option value="">All entities</option>
            {["alpha", "workspace", "job", "experiment", "review"].map(
              (value) => (
                <option key={value}>{value}</option>
              ),
            )}
          </select>
        </label>
        <span>{total} records</span>
        <button
          disabled={!offset}
          onClick={() => setOffset(Math.max(0, offset - 50))}
        >
          Previous audit page
        </button>
        <button
          disabled={offset + 50 >= total}
          onClick={() => setOffset(offset + 50)}
        >
          Next audit page
        </button>
      </div>
      <div className="table-scroll">
        <table>
          <thead>
            <tr>
              <th>Sequence</th>
              <th>Time</th>
              <th>Actor</th>
              <th>Action</th>
              <th>Entity</th>
              <th>Revision</th>
              <th>Metadata</th>
            </tr>
          </thead>
          <tbody>
            {records.map((record) => (
              <tr key={record.sequence}>
                <td>{record.sequence}</td>
                <td>
                  {new Date(Number(record.created_at_ms)).toLocaleString()}
                </td>
                <td>{record.actor_id}</td>
                <td>{record.action}</td>
                <td title={record.entity_id}>
                  {record.entity_type} / {record.entity_id.slice(0, 8)}
                </td>
                <td>{record.revision}</td>
                <td title={JSON.stringify(record.metadata)}>
                  {JSON.stringify(record.metadata)}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
