import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
} from "react";
import { ApiError, request } from "./api";

export type Principal = {
  id: string;
  name: string;
  role: "viewer" | "researcher" | "reviewer" | "admin";
  expires_at_ms: string;
};
export type Session = {
  principal: Principal;
  team_mode: boolean;
  team_name: string;
  project_id: string;
};
export type Permissions = {
  session: Session | null;
  canResearch: boolean;
  canReview: boolean;
  error: string;
  expired: boolean;
  refresh: () => void;
};
export const SessionContext = createContext<Permissions>({
  session: null,
  canResearch: true,
  canReview: false,
  error: "",
  expired: false,
  refresh: () => undefined,
});
export const usePermissions = () => useContext(SessionContext);
export function useSession(token: string): Permissions {
  const [session, setSession] = useState<Session | null>(null);
  const [denied, setDenied] = useState(false);
  const [error, setError] = useState("");
  const [refreshKey, setRefreshKey] = useState(0);
  const knownTeam = useRef(false);
  const refresh = useCallback(() => setRefreshKey((key) => key + 1), []);
  useEffect(() => {
    const controller = new AbortController();
    let expiryTimer: ReturnType<typeof setTimeout> | undefined;
    setSession(null);
    setDenied(knownTeam.current);
    const check = async () => {
      try {
        const response = await request<Session>("/session", token, {
          signal: controller.signal,
        });
        if (controller.signal.aborted) return;
        if (
          !response.principal ||
          !["viewer", "researcher", "reviewer", "admin"].includes(
            response.principal.role,
          )
        )
          throw new Error("The server returned an invalid session identity.");
        setSession(response);
        knownTeam.current = response.team_mode;
        setDenied(false);
        setError("");
        clearTimeout(expiryTimer);
        const expires = Number(response.principal.expires_at_ms);
        if (expires > 0)
          expiryTimer = setTimeout(
            () => {
              if (Date.now() >= expires) {
                setDenied(true);
                setError(
                  "Your session credential has expired. Connect with a valid credential.",
                );
              } else void check();
            },
            Math.max(0, Math.min(expires - Date.now(), 2147483647)),
          );
      } catch (failure) {
        if (controller.signal.aborted) return;
        if (
          knownTeam.current ||
          (failure instanceof ApiError && [401, 403].includes(failure.status))
        ) {
          setSession(null);
          setDenied(true);
        }
        // Older standalone services have no session RPC. Native authorization
        // remains authoritative; a known team identity never becomes anonymous.
        setError((failure as Error).message);
      }
    };
    void check();
    const timer = setInterval(() => void check(), 60_000);
    return () => {
      controller.abort();
      clearInterval(timer);
      clearTimeout(expiryTimer);
    };
  }, [token, refreshKey]);
  const expired = Boolean(
    session &&
    Number(session.principal.expires_at_ms) > 0 &&
    Number(session.principal.expires_at_ms) <= Date.now(),
  );
  return {
    session,
    expired,
    canResearch:
      !denied && !expired && (!session || session.principal.role !== "viewer"),
    canReview:
      !denied &&
      !expired &&
      Boolean(
        session && ["reviewer", "admin"].includes(session.principal.role),
      ),
    error,
    refresh,
  };
}
