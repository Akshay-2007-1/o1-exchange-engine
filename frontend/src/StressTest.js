import React, { useState, useRef, useCallback, useEffect } from 'react';
import './StressTest.css';
import PerformanceGraphs from './PerformanceGraphs';

export function StressTest({ ws, userSession }) {
  const [engineMode, setEngineMode] = useState('current');  // 'current' or 'legacy'
  const [numOrders, setNumOrders] = useState(10000);
  const [ordersPerSecond, setOrdersPerSecond] = useState(1000);
  const [isRunning, setIsRunning] = useState(false);
  
  // Metrics for Current engine
  const [currentMetrics, setCurrentMetrics] = useState({
    submitted: 0,
    matched: 0,
    latencyP50: 0,
    latencyP99: 0,
    renderTime: 0,
    totalTime: 0,
    throughput: 0,
    errors: 0,
  });

  // Metrics for Legacy engine
  const [legacyMetrics, setLegacyMetrics] = useState({
    submitted: 0,
    matched: 0,
    latencyP50: 0,
    latencyP99: 0,
    renderTime: 0,
    totalTime: 0,
    throughput: 0,
    errors: 0,
  });

  // Track pending orders for latency calculation
  const pendingOrdersRef = useRef(new Map());
  const latenciesRef = useRef([]);
  const startTimeRef = useRef(null);
  const testInProgressRef = useRef(false);

  // Calculate speedup ratio
  const speedupRatio = currentMetrics.totalTime > 0 && legacyMetrics.totalTime > 0
    ? (legacyMetrics.totalTime / currentMetrics.totalTime).toFixed(2)
    : null;

  const handleWebSocketMessage = useCallback((event) => {
    try {
      const data = JSON.parse(event.data);

      if (data.type === 'trade') {
        const buyKey = data.buy_order_id;
        const sellKey = data.sell_order_id;
        const orderKey = pendingOrdersRef.current.has(buyKey)
          ? buyKey
          : pendingOrdersRef.current.has(sellKey)
            ? sellKey
            : null;

        if (orderKey !== null && pendingOrdersRef.current.has(orderKey)) {
          const sentTime = pendingOrdersRef.current.get(orderKey);
          const latency = Date.now() - sentTime;
          latenciesRef.current.push(latency);
          pendingOrdersRef.current.delete(orderKey);

          if (engineMode === 'current') {
            setCurrentMetrics(prev => ({
              ...prev,
              matched: prev.matched + 1,
            }));
          } else {
            setLegacyMetrics(prev => ({
              ...prev,
              matched: prev.matched + 1,
            }));
          }
        }
      }
    } catch (error) {
      console.error('Error parsing WebSocket message:', error);
    }
  }, [engineMode]);

  // Setup WebSocket listener
  useEffect(() => {
    if (ws) {
      ws.addEventListener('message', handleWebSocketMessage);
      return () => ws.removeEventListener('message', handleWebSocketMessage);
    }
  }, [ws, handleWebSocketMessage]);

  const calculatePercentile = (arr, percentile) => {
    if (arr.length === 0) return 0;
    const sorted = [...arr].sort((a, b) => a - b);
    const index = Math.ceil((percentile / 100) * sorted.length) - 1;
    return sorted[Math.max(0, index)];
  };

  const generateRandomOrder = (orderId) => {
    const companies = [1, 2, 3];
    const side = Math.random() < 0.5 ? 'BUY' : 'SELL';
    // Price range: $50-$100 per share (5000-10000 cents)
    const basePrice = side === 'BUY' ? 7500 : 7500;  // $75 baseline
    const range = side === 'BUY' ? 2500 : 2500;  // ±$25 variance
    const crossover = Math.random() < 0.40;  // 40% crossover for natural matching

    const price = crossover
      ? side === 'BUY'
        ? 8000 + Math.floor(Math.random() * 3001)  // Can go higher when crossing
        : 7000 + Math.floor(Math.random() * 3001)  // Can go lower when crossing
      : basePrice + (Math.floor(Math.random() * range) - Math.floor(range / 2));

    return {
      type: 'order',
      company_id: companies[Math.floor(Math.random() * companies.length)],
      id: orderId,
      side,
      price: Math.max(100, price),  // Ensure positive price in cents
      quantity: Math.floor(Math.random() * 4) + 1,  // 1-5 shares instead of 10-500
      timestamp: Date.now(),
    };
  };

  const submitOrder = (order) => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      // Track order latency
      pendingOrdersRef.current.set(order.id, Date.now());

      ws.send(JSON.stringify({ ...order, token: userSession.token }));

      if (engineMode === 'current') {
        setCurrentMetrics(prev => ({
          ...prev,
          submitted: prev.submitted + 1,
        }));
      } else {
        setLegacyMetrics(prev => ({
          ...prev,
          submitted: prev.submitted + 1,
        }));
      }

      return true;
    }
    return false;
  };

  const runStressTest = async () => {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      alert('WebSocket not connected');
      return;
    }

    setIsRunning(true);
    testInProgressRef.current = true;
    startTimeRef.current = Date.now();
    latenciesRef.current = [];
    pendingOrdersRef.current.clear();

    // Reset metrics
    if (engineMode === 'current') {
      setCurrentMetrics({
        submitted: 0,
        matched: 0,
        latencyP50: 0,
        latencyP99: 0,
        renderTime: 0,
        totalTime: 0,
        throughput: 0,
        errors: 0,
      });
    } else {
      setLegacyMetrics({
        submitted: 0,
        matched: 0,
        latencyP50: 0,
        latencyP99: 0,
        renderTime: 0,
        totalTime: 0,
        throughput: 0,
        errors: 0,
      });
    }

    const delayBetweenOrders = 1000 / ordersPerSecond;  // milliseconds
    const startTime = Date.now();

    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: 'set_engine_mode',
        mode: engineMode,
      }));
    }

    // Periodic balance refresh during test
    const balanceRefreshInterval = setInterval(() => {
      if (ws && ws.readyState === WebSocket.OPEN && testInProgressRef.current) {
        ws.send(JSON.stringify({
          type: 'snapshot',
          token: userSession.token
        }));
      }
    }, 200);  // Update balance every 200ms

    for (let i = 0; i < numOrders; i++) {
      if (!testInProgressRef.current) break;

      const order = generateRandomOrder(i);
      const submitted = submitOrder(order);

      if (!submitted) {
        if (engineMode === 'current') {
          setCurrentMetrics(prev => ({ ...prev, errors: prev.errors + 1 }));
        } else {
          setLegacyMetrics(prev => ({ ...prev, errors: prev.errors + 1 }));
        }
      }

      // Throttle submission
      if ((i + 1) % 100 === 0) {
        await new Promise(resolve => setTimeout(resolve, delayBetweenOrders * 100));
      }
    }

    // Wait for pending orders to settle
    await new Promise(resolve => setTimeout(resolve, 1000));

    clearInterval(balanceRefreshInterval);  // Stop periodic balance updates

    const endTime = Date.now();
    const totalTime = (endTime - startTime) / 1000;  // seconds
    const p50 = calculatePercentile(latenciesRef.current, 50);
    const p99 = calculatePercentile(latenciesRef.current, 99);
    const throughput = numOrders / totalTime;

    if (engineMode === 'current') {
      setCurrentMetrics(prev => ({
        ...prev,
        latencyP50: p50.toFixed(2),
        latencyP99: p99.toFixed(2),
        totalTime: totalTime.toFixed(2),
        throughput: throughput.toFixed(0),
      }));
    } else {
      setLegacyMetrics(prev => ({
        ...prev,
        latencyP50: p50.toFixed(2),
        latencyP99: p99.toFixed(2),
        totalTime: totalTime.toFixed(2),
        throughput: throughput.toFixed(0),
      }));
    }

    testInProgressRef.current = false;
    setIsRunning(false);
  };

  const setScale = (scale) => {
    setNumOrders(scale);
  };

  const handleStop = () => {
    testInProgressRef.current = false;
    setIsRunning(false);
  };

  const renderMetricsPanel = (title, metrics) => (
    <div className="metrics-panel">
      <h3>{title}</h3>
      <div className="metric-grid">
        <div className="metric">
          <label>Orders Submitted</label>
          <div className="value">{metrics.submitted}</div>
        </div>
        <div className="metric">
          <label>Orders Matched</label>
          <div className="value">{metrics.matched}</div>
        </div>
        <div className="metric">
          <label>Latency P50 (ms)</label>
          <div className="value">{metrics.latencyP50}</div>
        </div>
        <div className="metric">
          <label>Latency P99 (ms)</label>
          <div className="value">{metrics.latencyP99}</div>
        </div>
        <div className="metric">
          <label>Throughput (orders/sec)</label>
          <div className="value">{metrics.throughput}</div>
        </div>
        <div className="metric">
          <label>Total Time (sec)</label>
          <div className="value">{metrics.totalTime}</div>
        </div>
        {metrics.errors > 0 && (
          <div className="metric error">
            <label>Errors</label>
            <div className="value">{metrics.errors}</div>
          </div>
        )}
      </div>
    </div>
  );

  return (
    <div className="stress-test-container">
      <h2>Order Book Stress Test</h2>

      {/* Engine Mode Toggle */}
      <div className="engine-selector">
        <label>Select Engine Mode:</label>
        <div className="radio-group">
          <label>
            <input
              type="radio"
              value="current"
              checked={engineMode === 'current'}
              onChange={(e) => setEngineMode(e.target.value)}
              disabled={isRunning}
            />
            Current Engine (Optimized)
          </label>
          <label>
            <input
              type="radio"
              value="legacy"
              checked={engineMode === 'legacy'}
              onChange={(e) => setEngineMode(e.target.value)}
              disabled={isRunning}
            />
            Legacy Engine (Baseline)
          </label>
        </div>
      </div>

      {/* Input Controls */}
      <div className="controls">
        <div className="control-group">
          <label>Number of Orders:</label>
          <input
            type="number"
            value={numOrders}
            onChange={(e) => setNumOrders(parseInt(e.target.value))}
            disabled={isRunning}
            min="100"
            step="100"
          />
        </div>

        <div className="control-group">
          <label>Orders Per Second:</label>
          <input
            type="number"
            value={ordersPerSecond}
            onChange={(e) => setOrdersPerSecond(parseInt(e.target.value))}
            disabled={isRunning}
            min="100"
            step="100"
          />
        </div>

        {/* Scale Presets */}
        <div className="scale-presets">
          <label>Quick Scale:</label>
          <div className="button-group">
            {[100, 1000, 10000, 100000].map(scale => (
              <button
                key={scale}
                onClick={() => setScale(scale)}
                disabled={isRunning}
                className={numOrders === scale ? 'active' : ''}
              >
                {scale >= 1000000 ? `${(scale / 1000000).toFixed(1)}M` : 
                 scale >= 1000 ? `${scale / 1000}K` : 
                 scale}
              </button>
            ))}
          </div>
        </div>

        {/* Action Buttons */}
        <div className="action-buttons">
          <button
            onClick={runStressTest}
            disabled={isRunning || !ws || ws.readyState !== WebSocket.OPEN}
            className="btn-start"
          >
            {isRunning ? 'Running...' : 'Start Stress Test'}
          </button>
          {isRunning && (
            <button onClick={handleStop} className="btn-stop">
              Stop Test
            </button>
          )}
        </div>
      </div>

      {/* Side-by-Side Metrics */}
      <div className="metrics-comparison">
        <div className="legacy-panel">
          {renderMetricsPanel('Legacy Engine (Baseline)', legacyMetrics)}
        </div>

        {/* Speedup Display */}
        <div className="speedup-display">
          {speedupRatio && (
            <>
              <div className="speedup-ratio">
                <h3>Speedup</h3>
                <div className="ratio-value">
                  {speedupRatio}×
                </div>
                <div className="ratio-label">
                  Current is {speedupRatio}× faster
                </div>
              </div>
            </>
          )}
        </div>
        
        <div className="current-panel">
          {renderMetricsPanel('Current Engine (Optimized)', currentMetrics)}
        </div>
      </div>

      <PerformanceGraphs engineMode={engineMode} isRunning={isRunning} />
    </div>
  );
}

export default StressTest;
