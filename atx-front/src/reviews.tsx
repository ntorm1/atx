import { useCallback, useEffect, useRef, useState } from "react";
import { ApiError, request } from "./api";
import type { AlphaSummary } from "./alpha-library";
import type { Job } from "./types";
import { usePermissions } from "./session";
type Review = {
  id: string;
  alpha_id: string;
  alpha_revision: string;
  request_sha256: string;
  requester_actor_id: string;
  author_actor_id: string;
  state: string;
  revision: string;
  note: string;
  evidence?: Record<string, unknown>;
  created_at_ms: string;
  updated_at_ms: string;
  current: boolean;
};
type Decision = {
  id: string;
  revision: string;
  state: string;
  actor_id: string;
  rationale: string;
  created_at_ms: string;
};
export function ReviewsPanel({
  token,
  selectJob,
}: {
  token: string;
  selectJob: (job: Job) => void;
}) {
  const { session, canResearch, canReview } = usePermissions();
  const [alphas, setAlphas] = useState<AlphaSummary[]>([]);
  const [alphaId, setAlphaId] = useState(""),
    [alphaRevision, setAlphaRevision] = useState("");
  const [revisions, setRevisions] = useState<AlphaSummary[]>([]);
  const [jobs, setJobs] = useState<Job[]>([]),
    [jobIds, setJobIds] = useState<string[]>([]);
  const [note, setNote] = useState(""),
    [rationale, setRationale] = useState("");
  const [reviews, setReviews] = useState<Review[]>([]),
    [selected, setSelected] = useState<Review | null>(null);
  const [decisions, setDecisions] = useState<Decision[]>([]);
  const [filter, setFilter] = useState(""),
    [offset, setOffset] = useState(0),
    [total, setTotal] = useState(0);
  const [error, setError] = useState(""),
    [listError, setListError] = useState("");
  const [busy, setBusy] = useState(false),
    [conflict, setConflict] = useState(false);
  const [refreshKey, setRefreshKey] = useState(0);
  const detailActive = useRef<AbortController | null>(null),
    writeActive = useRef<AbortController | null>(null);
  const detailVersion = useRef(0),
    writeVersion = useRef(0),
    selectedId = useRef("");
  const idempotency = useRef<{ signature: string; key: string } | null>(null);
  const load = useCallback(
    async (id: string) => {
      detailActive.current?.abort();
      const controller = new AbortController();
      detailActive.current = controller;
      const version = ++detailVersion.current;
      selectedId.current = id;
      setSelected(null);
      setDecisions([]);
      try {
        const [review, history] = await Promise.all([
          request<Review>(`/reviews/${id}`, token, {
            signal: controller.signal,
          }),
          request<{ decisions: Decision[] }>(
            `/reviews/${id}/decisions?limit=100`,
            token,
            { signal: controller.signal },
          ),
        ]);
        if (controller.signal.aborted || version !== detailVersion.current)
          return;
        setSelected(review);
        setDecisions(history.decisions);
        setConflict(false);
        setError("");
      } catch (failure) {
        if (!controller.signal.aborted && version === detailVersion.current)
          setError((failure as Error).message);
      }
    },
    [token],
  );
  useEffect(() => {
    const controller = new AbortController();
    request<{ alphas: AlphaSummary[] }>("/alphas?limit=100", token, {
      signal: controller.signal,
    })
      .then((page) => {
        if (!controller.signal.aborted) {
          setAlphas(page.alphas);
          setAlphaId((current) =>
            page.alphas.some((alpha) => alpha.id === current)
              ? current
              : (page.alphas[0]?.id ?? ""),
          );
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) setListError(failure.message);
      });
    return () => controller.abort();
  }, [token, refreshKey]);
  useEffect(() => {
    const controller = new AbortController();
    setRevisions([]);
    setJobs([]);
    setJobIds([]);
    setAlphaRevision("");
    if (!alphaId) return () => controller.abort();
    request<{ revisions: AlphaSummary[] }>(
      `/alphas/${alphaId}/revisions?limit=100`,
      token,
      { signal: controller.signal },
    )
      .then((page) => {
        if (!controller.signal.aborted) {
          setRevisions(page.revisions);
          setAlphaRevision(page.revisions[0]?.revision ?? "");
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) setError(failure.message);
      });
    return () => controller.abort();
  }, [token, alphaId, refreshKey]);
  useEffect(() => {
    const controller = new AbortController();
    setJobs([]);
    setJobIds([]);
    if (!alphaId || !alphaRevision) return () => controller.abort();
    const find = async () => {
      const matches: Job[] = [];
      // Bound browser discovery to 1,000 recent successes; evidence IDs are still
      // validated and pinned by the native review transaction.
      for (let position = 0; position < 1000; position += 100) {
        const page = await request<{ jobs: Job[]; total: string }>(
          `/jobs?limit=100&offset=${position}&status=succeeded`,
          token,
          { signal: controller.signal },
        );
        matches.push(
          ...page.jobs.filter(
            (job) =>
              job.alpha_id === alphaId && job.alpha_revision === alphaRevision,
          ),
        );
        if (position + 100 >= Number(page.total) || !page.jobs.length) break;
      }
      if (!controller.signal.aborted) setJobs(matches);
    };
    void find().catch((failure: Error) => {
      if (!controller.signal.aborted) setError(failure.message);
    });
    return () => controller.abort();
  }, [token, alphaId, alphaRevision]);
  useEffect(() => {
    const controller = new AbortController();
    request<{ reviews: Review[]; total: string }>(
      `/reviews?limit=50&offset=${offset}&state=${filter}`,
      token,
      { signal: controller.signal },
    )
      .then((page) => {
        if (!controller.signal.aborted) {
          setReviews(page.reviews);
          setTotal(Number(page.total));
          setListError("");
        }
      })
      .catch((failure: Error) => {
        if (!controller.signal.aborted) setListError(failure.message);
      });
    return () => controller.abort();
  }, [token, refreshKey, offset, filter]);
  useEffect(
    () => () => {
      detailActive.current?.abort();
      writeActive.current?.abort();
      detailVersion.current++;
      writeVersion.current++;
    },
    [token],
  );
  const send = async (state?: string) => {
    if (
      (!state &&
        (!canResearch || !alphaId || !alphaRevision || !jobIds.length)) ||
      (state && !selected)
    )
      return;
    const body = state
      ? { expected_revision: selected!.revision, state, rationale }
      : {
          alpha_id: alphaId,
          alpha_revision: alphaRevision,
          job_ids: jobIds,
          note,
        };
    const signature = JSON.stringify(body);
    if (!state && idempotency.current?.signature !== signature)
      idempotency.current = { signature, key: crypto.randomUUID() };
    writeActive.current?.abort();
    const controller = new AbortController();
    writeActive.current = controller;
    const version = ++writeVersion.current;
    const target = selected?.id;
    setBusy(true);
    setError("");
    setConflict(false);
    try {
      const record = await request<Review>(
        state ? `/reviews/${target}/decision` : "/reviews",
        token,
        {
          method: "POST",
          signal: controller.signal,
          body: JSON.stringify(
            state
              ? body
              : { ...body, idempotency_key: idempotency.current!.key },
          ),
        },
      );
      if (controller.signal.aborted || version !== writeVersion.current) return;
      if (!state) {
        idempotency.current = null;
        void load(record.id);
      } else if (selectedId.current === target) {
        setSelected(record);
        void load(record.id);
      }
      setRefreshKey((key) => key + 1);
    } catch (failure) {
      if (!controller.signal.aborted && version === writeVersion.current) {
        setError((failure as Error).message);
        setConflict(failure instanceof ApiError && failure.status === 409);
      }
    } finally {
      if (version === writeVersion.current) setBusy(false);
    }
  };
  const actor = session?.principal;
  const independent =
    !session?.team_mode ||
    Boolean(
      actor &&
      selected &&
      ![
        selected.requester_actor_id,
        selected.author_actor_id,
        selected.evidence?.revision_actor_id,
        ...(Array.isArray(selected.evidence?.recipe_actor_ids)
          ? selected.evidence.recipe_actor_ids
          : []),
      ].includes(actor.id),
    );
  const submitted = selected?.state === "submitted";
  const withdraw = Boolean(
    selected &&
    ["submitted", "approved"].includes(selected.state) &&
    actor &&
    (actor.role === "admin" || actor.id === selected.requester_actor_id),
  );
  return (
    <div className="research-tool-panel reviews-panel">
      <div className="panel-toolbar">
        <strong>Reviews / frozen evidence</strong>
        <button onClick={() => setRefreshKey((key) => key + 1)}>
          Refresh reviews
        </button>
        <label>
          State{" "}
          <select
            aria-label="Review state filter"
            value={filter}
            onChange={(e) => {
              setFilter(e.target.value);
              setOffset(0);
            }}
          >
            <option value="">All</option>
            {["submitted", "approved", "rejected", "withdrawn"].map((state) => (
              <option key={state}>{state}</option>
            ))}
          </select>
        </label>
        <span>{total} records</span>
      </div>
      {(error || listError) && (
        <div className="notice error" role="alert">
          {error || listError}
          {conflict && (
            <button
              onClick={() =>
                selectedId.current && void load(selectedId.current)
              }
            >
              Reload review revision
            </button>
          )}
        </div>
      )}
      <div className="reviews-body">
        <section className="review-composer">
          <div className="panel-toolbar">
            Submit immutable alpha revision evidence
          </div>
          <label>
            Saved alpha{" "}
            <select
              aria-label="Review alpha"
              value={alphaId}
              onChange={(e) => setAlphaId(e.target.value)}
            >
              <option value="">Select saved alpha</option>
              {alphas.map((alpha) => (
                <option key={alpha.id} value={alpha.id}>
                  {alpha.name}
                </option>
              ))}
            </select>
          </label>
          <label>
            Exact revision{" "}
            <select
              aria-label="Review alpha revision"
              value={alphaRevision}
              onChange={(e) => setAlphaRevision(e.target.value)}
            >
              {revisions.map((alpha) => (
                <option key={alpha.revision} value={alpha.revision}>
                  r{alpha.revision} / {alpha.request_sha256.slice(0, 12)}
                </option>
              ))}
            </select>
          </label>
          <div className="panel-toolbar">
            Linked successful jobs · choose 1–12
          </div>
          <div className="review-job-picker">
            {jobs.map((job) => (
              <label key={job.id}>
                <input
                  type="checkbox"
                  checked={jobIds.includes(job.id)}
                  disabled={!jobIds.includes(job.id) && jobIds.length >= 12}
                  onChange={(e) =>
                    setJobIds((current) =>
                      e.target.checked
                        ? [...current, job.id]
                        : current.filter((id) => id !== job.id),
                    )
                  }
                />
                <span title={job.id}>{job.label || job.id.slice(0, 12)}</span>
                <button onClick={() => selectJob(job)}>
                  Inspect evidence run
                </button>
              </label>
            ))}
            {!jobs.length && (
              <div className="muted">
                No linked successes among the latest 1,000 successful jobs. Run
                the exact saved revision first.
              </div>
            )}
          </div>
          <label>
            Submission note{" "}
            <textarea
              aria-label="Review submission note"
              value={note}
              maxLength={4096}
              onChange={(e) => setNote(e.target.value)}
            />
          </label>
          <button
            disabled={busy || !canResearch || !jobIds.length || !alphaRevision}
            onClick={() => void send()}
          >
            Submit review
          </button>
          <div className="notice">
            Evidence freezes the exact recipe and selected native results.
            Approval is a team acknowledgement, not qualification of historical
            data, capacity or future returns.
          </div>
        </section>
        <section className="review-inspector">
          <div className="table-scroll review-list">
            <table>
              <thead>
                <tr>
                  <th>State</th>
                  <th>Alpha revision</th>
                  <th>Requester</th>
                  <th>Current head</th>
                  <th>Review</th>
                </tr>
              </thead>
              <tbody>
                {reviews.map((review) => (
                  <tr
                    key={review.id}
                    className={
                      selectedId.current === review.id ? "selected" : ""
                    }
                    onClick={() => {
                      setRationale("");
                      setError("");
                      void load(review.id);
                    }}
                  >
                    <td>{review.state}</td>
                    <td title={review.alpha_id}>
                      {review.alpha_id.slice(0, 8)} / r{review.alpha_revision}
                    </td>
                    <td>{review.requester_actor_id}</td>
                    <td>{review.current ? "Current" : "Superseded"}</td>
                    <td>{review.id.slice(0, 8)}</td>
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
              Previous reviews page
            </button>
            <button
              disabled={offset + 50 >= total}
              onClick={() => setOffset(offset + 50)}
            >
              Next reviews page
            </button>
          </div>
          {selected ? (
            <>
              <div className="panel-toolbar">
                <strong>
                  {selected.state} / r{selected.revision}
                </strong>
                <span>
                  {selected.current
                    ? "Exact current alpha head"
                    : "Alpha head changed"}
                </span>
                <button onClick={() => void load(selected.id)}>
                  Reload selected review
                </button>
              </div>
              <dl className="review-properties">
                <dt>Requester / original author</dt>
                <dd>
                  {selected.requester_actor_id} / {selected.author_actor_id}
                </dd>
                <dt>Recipe SHA-256</dt>
                <dd>{selected.request_sha256}</dd>
                <dt>Submission note</dt>
                <dd>{selected.note || "—"}</dd>
              </dl>
              <label>
                Decision rationale{" "}
                <textarea
                  aria-label="Review decision rationale"
                  value={rationale}
                  maxLength={4096}
                  onChange={(e) => setRationale(e.target.value)}
                />
              </label>
              <div className="panel-toolbar">
                <button
                  disabled={
                    busy ||
                    !canReview ||
                    !independent ||
                    !submitted ||
                    !selected.current ||
                    !rationale.trim()
                  }
                  onClick={() => void send("approved")}
                >
                  Approve evidence
                </button>
                <button
                  disabled={
                    busy ||
                    !canReview ||
                    !independent ||
                    !submitted ||
                    !rationale.trim()
                  }
                  onClick={() => void send("rejected")}
                >
                  Reject evidence
                </button>
                <button
                  disabled={busy || !withdraw || !rationale.trim()}
                  onClick={() => void send("withdrawn")}
                >
                  Withdraw review
                </button>
              </div>
              {session?.team_mode && !independent && (
                <div className="notice">
                  Approval and rejection require a reviewer distinct from the
                  requester, original author and all recorded authors of the
                  submitted recipe.
                </div>
              )}
              <div className="table-scroll review-decisions">
                <table>
                  <thead>
                    <tr>
                      <th>Decision revision</th>
                      <th>State</th>
                      <th>Actor</th>
                      <th>Rationale</th>
                    </tr>
                  </thead>
                  <tbody>
                    {decisions.map((decision) => (
                      <tr key={decision.id}>
                        <td>{decision.revision}</td>
                        <td>{decision.state}</td>
                        <td>{decision.actor_id}</td>
                        <td title={decision.rationale}>{decision.rationale}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              <details className="review-evidence">
                <summary>Frozen native evidence JSON</summary>
                <pre>{JSON.stringify(selected.evidence, null, 2)}</pre>
              </details>
            </>
          ) : (
            <div className="muted">
              Select a review to inspect its exact evidence and decision
              history.
            </div>
          )}
        </section>
      </div>
    </div>
  );
}
