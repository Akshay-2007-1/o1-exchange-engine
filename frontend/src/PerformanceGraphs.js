import { useMemo } from "react";

function average(points) {
  if (points.length === 0) return 0;
  return points.reduce((sum, m) => sum + m.throughput_ops_sec, 0) / points.length;
}

function summarize(metrics) {
  const currentPoints = metrics.filter(m => m.engine_mode === "CURRENT" && m.orders_processed > 0);
  const legacyPoints = metrics.filter(m => m.engine_mode === "LEGACY" && m.orders_processed > 0);
  const currentAvg = average(currentPoints);
  const legacyAvg = average(legacyPoints);
  return {
    hasBoth: currentPoints.length > 0 && legacyPoints.length > 0,
    speedup: legacyAvg > 0 ? currentAvg / legacyAvg : null
  };
}

function Sparkline({ points, color, height = 56 }) {
  if (points.length < 2 || points.every(v => v === 0)) {
    return <div className="perf-empty">Waiting for engine activity…</div>;
  }

  const W = 400;
  const H = height;
  const pad = 6;
  const max = Math.max(...points, 1);

  const pts = points
    .map((v, i) => {
      const x = pad + (i / (points.length - 1)) * (W - pad * 2);
      const y = H - pad - (v / max) * (H - pad * 2);
      return `${x},${y}`;
    })
    .join(" ");

  return (
    <svg viewBox={`0 0 ${W} ${H}`} style={{ width: "100%", height, display: "block" }}>
      <polyline points={pts} fill="none" stroke={color} strokeWidth="2" strokeLinejoin="round" />
    </svg>
  );
}

export default function PerformanceGraphs({ metrics }) {
  const summary = useMemo(() => summarize(metrics), [metrics]);
  const recent = useMemo(() => metrics.slice(-60), [metrics]);
  const latest = recent[recent.length - 1];
  // Prefer the most recent tick that actually processed orders — otherwise a
  // trailing idle tick (0 ops/sec) right after a burst makes the summary
  // numbers look stale/wrong next to a sparkline still showing the peak.
  const latestActive = [...recent].reverse().find(m => m.orders_processed > 0) || latest;
  const color = latest?.engine_mode === "LEGACY" ? "var(--sell, #ff6262)" : "var(--buy, #25c18c)";

  const throughputPoints = recent.map(m => m.throughput_ops_sec || 0);
  const p50Points = recent.map(m => m.latency_p50_us || 0);
  const p99Points = recent.map(m => m.latency_p99_us || 0);

  return (
    <section className="panel perf-graphs-panel">
      <div className="panel-heading">
        <h2>Live Engine Metrics</h2>
        <span>{latest ? `${latest.engine_mode} engine` : "No data yet"}</span>
      </div>

      <div className="perf-graphs-body">
        {summary.hasBoth && summary.speedup !== null && (
          <p className="perf-speedup-badge">
            CURRENT is <strong>{summary.speedup.toFixed(1)}×</strong> faster than LEGACY, averaged over this session
          </p>
        )}

        <div className="perf-chart-block">
          <div className="perf-chart-label">
            <span>Matching Throughput</span>
            <span>{latestActive ? `${Math.round(latestActive.throughput_ops_sec).toLocaleString()} ops/sec` : "--"}</span>
          </div>
          <Sparkline points={throughputPoints} color={color} />
        </div>

        <div className="perf-chart-block">
          <div className="perf-chart-label">
            <span>Match Latency p50</span>
            <span>{latestActive ? `${latestActive.latency_p50_us.toFixed(1)} µs` : "--"}</span>
          </div>
          <Sparkline points={p50Points} color={color} />
        </div>

        <div className="perf-chart-block">
          <div className="perf-chart-label">
            <span>Match Latency p99</span>
            <span>{latestActive ? `${latestActive.latency_p99_us.toFixed(1)} µs` : "--"}</span>
          </div>
          <Sparkline points={p99Points} color="var(--gold, #f3c96b)" />
        </div>

        <p className="stress-test-note">
          Latency measures only the matching call itself (process_buy_order/process_sell_order) — not
          WebSocket or database time — so CURRENT vs LEGACY reflects the engine, not network conditions.
        </p>
      </div>
    </section>
  );
}
