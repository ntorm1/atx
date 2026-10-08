import type { Point, Result } from "./types";
import { useEffect, useRef, useState } from "react";

export function useChartDimensions(initialWidth: number, active: boolean) {
  const ref = useRef<SVGSVGElement>(null);
  const [size, setSize] = useState({ width: initialWidth, height: 208 });
  useEffect(() => {
    if (!ref.current) return;
    const observer = new ResizeObserver(([entry]) => {
      if (entry.contentRect.width > 0 && entry.contentRect.height > 0)
        setSize({
          width: entry.contentRect.width,
          height: entry.contentRect.height,
        });
    });
    observer.observe(ref.current);
    return () => observer.disconnect();
  }, [active]);
  return { ref, ...size };
}
export const number = (value: unknown, digits = 2) =>
  typeof value === "number" && Number.isFinite(value)
    ? value.toLocaleString(undefined, {
        maximumFractionDigits: digits,
        minimumFractionDigits: digits,
      })
    : "—";
export const percent = (value: unknown) =>
  typeof value === "number" ? `${number(value * 100)}%` : "—";
export function Empty({
  children = "Select a completed run to inspect research results.",
}: {
  children?: React.ReactNode;
}) {
  return (
    <div className="empty">
      <span>{children}</span>
    </div>
  );
}
export function LineChart({
  points = [],
  drawdown = false,
}: {
  points?: Point[];
  drawdown?: boolean;
}) {
  const { ref, width, height } = useChartDimensions(600, points.length > 0);
  if (!points.length) return <Empty />;
  const values = points.map((p) => p.value);
  const min = drawdown ? Math.min(...values, -0.01) : Math.min(...values);
  const max = drawdown ? 0 : Math.max(...values);
  const spread = max - min || 1;
  const left = drawdown ? 44 : 72;
  const right = Math.max(left + 1, width - 16);
  const top = 15;
  const bottom = Math.max(top + 1, height - 29);
  const xy = (p: Point, i: number) =>
    `${left + (i / Math.max(points.length - 1, 1)) * (right - left)},${top + ((max - p.value) / spread) * (bottom - top)}`;
  const path = points.map((p, i) => `${i ? "L" : "M"}${xy(p, i)}`).join(" ");
  return (
    <div className="chart">
      <svg
        ref={ref}
        viewBox={`0 0 ${width} ${height}`}
        role="img"
        aria-label={
          drawdown ? "Peak to trough drawdown" : "Net asset value curve"
        }
        preserveAspectRatio="none"
      >
        {[0, 1, 2, 3].map((i) => (
          <g key={i}>
            <line
              className="grid-line"
              x1={left}
              x2={right}
              y1={top + (i * (bottom - top)) / 3}
              y2={top + (i * (bottom - top)) / 3}
            />
            <text
              x={left - 6}
              y={top + 4 + (i * (bottom - top)) / 3}
              textAnchor="end"
            >
              {drawdown
                ? percent(max - (spread * i) / 3)
                : number(max - (spread * i) / 3, 0)}
            </text>
          </g>
        ))}
        {drawdown && (
          <path
            d={`${path} L${right},${top} L${left},${top} Z`}
            fill="#bf5b4d"
            opacity=".22"
          />
        )}
        <path
          d={path}
          fill="none"
          stroke={drawdown ? "#aa4b40" : "#2767a0"}
          strokeWidth="1.8"
          vectorEffect="non-scaling-stroke"
        />
        {(width < 320 ? [0, 1] : [0, 0.5, 1]).map((v) => (
          <text
            key={v}
            x={left + v * (right - left)}
            y={height - 10}
            textAnchor={v === 0 ? "start" : v === 1 ? "end" : "middle"}
          >
            {points[Math.floor(v * (points.length - 1))].date}
          </text>
        ))}
      </svg>
      <div className="chart-caption">
        {drawdown
          ? "Daily drawdown from prior peak"
          : "Daily net asset value · costs included where supplied"}
      </div>
    </div>
  );
}
export function Histogram({
  bins = [],
}: {
  bins?: Result["returns_histogram"];
}) {
  const { ref, width, height } = useChartDimensions(420, bins.length > 0);
  if (!bins.length) return <Empty />;
  const maximum = Math.max(...bins.map((b) => b.count), 1);
  const left = 30;
  const right = Math.max(left + 1, width - 16);
  const top = 15;
  const bottom = Math.max(top + 1, height - 29);
  return (
    <div className="chart">
      <svg
        ref={ref}
        viewBox={`0 0 ${width} ${height}`}
        role="img"
        aria-label="Distribution of daily returns"
        preserveAspectRatio="none"
      >
        {[0, 1, 2, 3].map((i) => (
          <g key={i}>
            <line
              className="grid-line"
              x1={left}
              x2={right}
              y1={top + (i * (bottom - top)) / 3}
              y2={top + (i * (bottom - top)) / 3}
            />
            <text
              x={left - 6}
              y={top + 4 + (i * (bottom - top)) / 3}
              textAnchor="end"
            >
              {Math.round(maximum * (1 - i / 3))}
            </text>
          </g>
        ))}
        {bins.map((b, i) => (
          <rect
            key={i}
            x={left + (i / bins.length) * (right - left)}
            y={bottom - (b.count / maximum) * (bottom - top)}
            width={Math.max(1, (right - left) / bins.length - 1)}
            height={(b.count / maximum) * (bottom - top)}
            fill="#507fa6"
          >
            <title>
              {percent(b.lower)} to {percent(b.upper)}: {b.count} sessions
            </title>
          </rect>
        ))}
        <text x={left} y={height - 10}>
          {percent(bins[0].lower)}
        </text>
        <text x={right} y={height - 10} textAnchor="end">
          {percent(bins[bins.length - 1].upper)}
        </text>
      </svg>
      <div className="chart-caption">Daily return · frequency</div>
    </div>
  );
}
export function Heatmap({
  months = [],
}: {
  months?: Result["monthly_returns"];
}) {
  if (!months.length) return <Empty />;
  const years = [...new Set(months.map((m) => m.month.slice(0, 4)))]
    .sort()
    .reverse();
  const lookup = new Map(months.map((m) => [m.month, m.value]));
  return (
    <div className="table-scroll">
      <table className="heatmap">
        <thead>
          <tr>
            <th>Year</th>
            {[
              "Jan",
              "Feb",
              "Mar",
              "Apr",
              "May",
              "Jun",
              "Jul",
              "Aug",
              "Sep",
              "Oct",
              "Nov",
              "Dec",
            ].map((m) => (
              <th key={m}>{m}</th>
            ))}
            <th>YTD</th>
          </tr>
        </thead>
        <tbody>
          {years.map((y) => (
            <tr key={y}>
              <th>{y}</th>
              {Array.from({ length: 12 }, (_, i) => {
                const v = lookup.get(`${y}-${String(i + 1).padStart(2, "0")}`);
                return (
                  <td
                    key={i}
                    style={
                      v === undefined
                        ? {}
                        : {
                            background: `rgba(${v >= 0 ? "83,140,107" : "187,89,74"},${Math.min(0.65, 0.13 + Math.abs(v) * 8)})`,
                          }
                    }
                    title={
                      v === undefined
                        ? "No observation"
                        : `${y}-${i + 1}: ${percent(v)}`
                    }
                  >
                    {v === undefined ? "—" : number(v * 100, 1)}
                  </td>
                );
              })}
              <td>
                {number(
                  (months
                    .filter((m) => m.month.startsWith(y))
                    .reduce((v, m) => v * (1 + m.value), 1) -
                    1) *
                    100,
                  1,
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="chart-caption">
        Monthly returns (%) · compounded from observed sessions
      </div>
    </div>
  );
}
