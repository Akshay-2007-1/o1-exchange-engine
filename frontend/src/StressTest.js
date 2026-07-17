import { useEffect, useRef, useState } from "react";
import PerformanceGraphs from "./PerformanceGraphs";

const ORDER_PRESETS = [100, 1000, 10000, 100000];
const TICK_MS = 50; // 20 ticks/sec - batches orders within a tick to hit the target rate
const SYNTHETIC_TRADER_COUNT = 50; // spreads fills across many fake user_ids so
// self-trade prevention doesn't constantly trip against a single real account

function randomOrder(companies, companyFilter) {
  const pool = companyFilter === "ALL" ? companies : companies.filter(c => String(c.id) === String(companyFilter));
  const company = pool.length ? pool[Math.floor(Math.random() * pool.length)] : null;
  if (!company) return null;

  const side = Math.random() < 0.5 ? "BUY" : "SELL";
  // Centered around $100 with a spread wide enough that buys/sells routinely
  // cross, so the stress test actually exercises the matching path rather
  // than just resting orders.
  const price = 10000 + Math.floor((Math.random() - 0.5) * 400);
  const quantity = 10 + Math.floor(Math.random() * 490);
  // Negative IDs never collide with a real (positive) DB user_id, so the
  // server skips wallet reservation/settlement for these entirely.
  const syntheticUserId = -(1 + Math.floor(Math.random() * SYNTHETIC_TRADER_COUNT));

  return {
    type: "order",
    stress: true,
    synthetic_user_id: syntheticUserId,
    company_id: company.id,
    side,
    price,
    quantity,
    timestamp: Date.now()
  };
}

export default function StressTest({ send, engineMode, companies, token, connected, metrics = [] }) {
  const [numOrders, setNumOrders] = useState(1000);
  const [rate, setRate] = useState(200);
  const [companyFilter, setCompanyFilter] = useState("ALL");
  const [running, setRunning] = useState(false);
  const [submitted, setSubmitted] = useState(0);
  const [elapsedMs, setElapsedMs] = useState(0);

  const intervalRef = useRef(null);
  const submittedRef = useRef(0);
  const startRef = useRef(0);

  const stop = () => {
    if (intervalRef.current !== null) {
      clearInterval(intervalRef.current);
      intervalRef.current = null;
    }
    setRunning(false);
  };

  const start = () => {
    if (!connected || !token || companies.length === 0) return;
    stop();
    submittedRef.current = 0;
    setSubmitted(0);
    setElapsedMs(0);
    startRef.current = performance.now();
    setRunning(true);

    const perTick = Math.max(1, Math.round((rate * TICK_MS) / 1000));

    intervalRef.current = setInterval(() => {
      const remaining = numOrders - submittedRef.current;
      if (remaining <= 0) {
        stop();
        return;
      }
      const batch = Math.min(perTick, remaining);
      for (let i = 0; i < batch; i++) {
        const order = randomOrder(companies, companyFilter);
        if (order) send({ ...order, token });
      }
      submittedRef.current += batch;
      setSubmitted(submittedRef.current);
      setElapsedMs(performance.now() - startRef.current);
    }, TICK_MS);
  };

  const reset = () => {
    stop();
    submittedRef.current = 0;
    setSubmitted(0);
    setElapsedMs(0);
  };

  useEffect(() => stop, []);

  const elapsedSec = elapsedMs / 1000;
  const sendRate = elapsedSec > 0 ? Math.round(submitted / elapsedSec) : 0;

  return (
    <section className="panel stress-test-panel">
      <div className="panel-heading">
        <h2>Stress Test</h2>
        <span>CURRENT vs LEGACY engine comparison</span>
      </div>

      <div className="stress-test-body">
        <div className="stress-test-row">
          <span className="stress-test-label">Engine</span>
          <div className="side-toggle engine-toggle">
            <button
              type="button"
              className={engineMode === "CURRENT" ? "buy" : ""}
              onClick={() => send({ type: "switch_engine", mode: "CURRENT" })}
            >
              CURRENT (O(1) bitmap)
            </button>
            <button
              type="button"
              className={engineMode === "LEGACY" ? "sell" : ""}
              onClick={() => send({ type: "switch_engine", mode: "LEGACY" })}
            >
              LEGACY (std::map)
            </button>
          </div>
        </div>

        <div className="stress-test-row">
          <span className="stress-test-label">Orders</span>
          <div className="preset-group">
            {ORDER_PRESETS.map(preset => (
              <button
                key={preset}
                type="button"
                className={numOrders === preset ? "active" : ""}
                disabled={running}
                onClick={() => setNumOrders(preset)}
              >
                {preset.toLocaleString()}
              </button>
            ))}
          </div>
        </div>

        <div className="stress-test-row">
          <label className="stress-test-label" htmlFor="stress-rate">Rate (orders/sec)</label>
          <input
            id="stress-rate"
            type="number"
            min="1"
            max="20000"
            value={rate}
            disabled={running}
            onChange={event => setRate(Math.max(1, Number(event.target.value) || 1))}
          />
        </div>

        <div className="stress-test-row">
          <label className="stress-test-label" htmlFor="stress-company">Company</label>
          <select
            id="stress-company"
            value={companyFilter}
            disabled={running}
            onChange={event => setCompanyFilter(event.target.value)}
          >
            <option value="ALL">All companies</option>
            {companies.map(company => (
              <option key={company.id} value={company.id}>{company.symbol}</option>
            ))}
          </select>
        </div>

        <div className="stress-test-controls">
          <button type="button" className="submit-order buy" onClick={start} disabled={running || !connected}>
            Start
          </button>
          <button type="button" className="submit-order sell" onClick={stop} disabled={!running}>
            Stop
          </button>
          <button type="button" onClick={reset} disabled={running}>
            Reset
          </button>
        </div>

        <div className="stress-test-stats">
          <div className="stress-stat">
            <span className="stress-stat-label">Submitted</span>
            <span className="stress-stat-value">{submitted.toLocaleString()} / {numOrders.toLocaleString()}</span>
          </div>
          <div className="stress-stat">
            <span className="stress-stat-label">Elapsed</span>
            <span className="stress-stat-value">{(elapsedMs / 1000).toFixed(1)}s</span>
          </div>
          <div className="stress-stat">
            <span className="stress-stat-label">Client Send Rate</span>
            <span className="stress-stat-value">{sendRate.toLocaleString()}/s</span>
          </div>
        </div>
        <p className="stress-test-note">
          "Client Send Rate" is how fast the browser can dispatch WebSocket messages, not the
          engine's matching throughput - see live engine metrics below for that.
        </p>
      </div>

      <PerformanceGraphs metrics={metrics} />
    </section>
  );
}
