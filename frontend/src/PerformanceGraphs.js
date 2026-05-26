import React, { useState, useEffect, useRef } from 'react';
import {
  LineChart, Line, BarChart, Bar, AreaChart, Area,
  ComposedChart, XAxis, YAxis, CartesianGrid, Tooltip, Legend, ResponsiveContainer
} from 'recharts';
import './PerformanceGraphs.css';

export function PerformanceGraphs({ engineMode, isRunning }) {
  const [latencyData, setLatencyData] = useState([]);
  const [throughputData, setThroughputData] = useState([]);
  const [memoryData, setMemoryData] = useState([]);
  const [orderStatusData, setOrderStatusData] = useState([]);

  const latencyDataRef = useRef([]);
  const throughputDataRef = useRef([]);
  const memoryDataRef = useRef([]);
  const orderStatusDataRef = useRef([]);
  const metricsIntervalRef = useRef(null);
  const timestampRef = useRef(0);

  // Determine color scheme based on engine mode
  const getThemeColor = () => engineMode === 'current' 
    ? { primary: '#27ae60', secondary: '#229954' }
    : { primary: '#e74c3c', secondary: '#c0392b' };

  const theme = getThemeColor();

  // Simulate metrics updates (in real app, this would come from WebSocket)
  useEffect(() => {
    if (!isRunning) {
      clearInterval(metricsIntervalRef.current);
      return;
    }

    metricsIntervalRef.current = setInterval(() => {
      timestampRef.current += 1;
      const time = timestampRef.current;

      // Generate realistic metrics data
      const baseLatency = engineMode === 'current' ? 0.5 : 50;
      const variability = engineMode === 'current' ? 0.2 : 15;

      const latency_p50 = baseLatency + (Math.random() - 0.5) * variability;
      const latency_p99 = baseLatency * 3 + (Math.random() - 0.5) * variability * 2;

      // Latency data
      latencyDataRef.current.push({
        time: time,
        p50: Math.max(0, latency_p50),
        p99: Math.max(0, latency_p99),
      });

      // Throughput data (orders/sec)
      const baseThroughput = engineMode === 'current' ? 8000 : 100;
      const throughput = baseThroughput + (Math.random() - 0.5) * baseThroughput * 0.1;

      throughputDataRef.current.push({
        time: time,
        throughput: Math.max(0, throughput),
      });

      // Memory data (MB)
      const baseMemory = engineMode === 'current' ? 50 : 120;
      const memory = baseMemory + (time * 0.2) + (Math.random() - 0.5) * 5;

      memoryDataRef.current.push({
        time: time,
        memory: Math.max(0, memory),
      });

      // Order status data
      const basematched = engineMode === 'current' ? 500 : 20;
      const matched = basematched * time + (Math.random() - 0.5) * basematched * 0.1;
      const queueDepth = Math.max(0, 100 - matched * 0.01);

      orderStatusDataRef.current.push({
        time: time,
        matched: Math.max(0, matched),
        queueDepth: Math.max(0, queueDepth),
      });

      // Keep only last 60 data points
      if (latencyDataRef.current.length > 60) {
        latencyDataRef.current.shift();
      }
      if (throughputDataRef.current.length > 60) {
        throughputDataRef.current.shift();
      }
      if (memoryDataRef.current.length > 60) {
        memoryDataRef.current.shift();
      }
      if (orderStatusDataRef.current.length > 60) {
        orderStatusDataRef.current.shift();
      }

      setLatencyData([...latencyDataRef.current]);
      setThroughputData([...throughputDataRef.current]);
      setMemoryData([...memoryDataRef.current]);
      setOrderStatusData([...orderStatusDataRef.current]);
    }, 500);  // Update every 500ms

    return () => clearInterval(metricsIntervalRef.current);
  }, [isRunning, engineMode]);

  // Reset data when engine mode changes
  useEffect(() => {
    if (!isRunning) {
      timestampRef.current = 0;
      latencyDataRef.current = [];
      throughputDataRef.current = [];
      memoryDataRef.current = [];
      orderStatusDataRef.current = [];
      setLatencyData([]);
      setThroughputData([]);
      setMemoryData([]);
      setOrderStatusData([]);
    }
  }, [engineMode, isRunning]);

  const CustomTooltip = ({ active, payload, label }) => {
    if (active && payload && payload.length) {
      return (
        <div className="custom-tooltip">
          <p className="label">{`Time: ${label}s`}</p>
          {payload.map((entry, index) => (
            <p key={index} style={{ color: entry.color }}>
              {`${entry.name}: ${entry.value.toFixed(2)}`}
            </p>
          ))}
        </div>
      );
    }
    return null;
  };

  return (
    <div className="performance-graphs-container">
      <div className={`graphs-wrapper ${engineMode}-mode`}>
        <h3>Real-Time Performance Metrics - {engineMode === 'current' ? 'Current (Optimized)' : 'Legacy (Baseline)'}</h3>

        <div className="graph-grid">
          {/* Latency Timeline */}
          <div className="graph-card">
            <h4>Latency Over Time (ms)</h4>
            <ResponsiveContainer width="100%" height={300}>
              <LineChart data={latencyData}>
                <CartesianGrid strokeDasharray="3 3" stroke="#e0e6ed" />
                <XAxis dataKey="time" label={{ value: 'Time (sec)', position: 'insideBottomRight', offset: -5 }} />
                <YAxis label={{ value: 'Latency (ms)', angle: -90, position: 'insideLeft' }} />
                <Tooltip content={<CustomTooltip />} />
                <Legend />
                <Line
                  type="monotone"
                  dataKey="p50"
                  stroke={theme.primary}
                  strokeWidth={2}
                  dot={false}
                  isAnimationActive={true}
                  animationDuration={300}
                  name="P50"
                />
                <Line
                  type="monotone"
                  dataKey="p99"
                  stroke={theme.secondary}
                  strokeWidth={2}
                  dot={false}
                  isAnimationActive={true}
                  animationDuration={300}
                  name="P99"
                  strokeDasharray="5 5"
                />
              </LineChart>
            </ResponsiveContainer>
          </div>

          {/* Throughput */}
          <div className="graph-card">
            <h4>Throughput (orders/sec)</h4>
            <ResponsiveContainer width="100%" height={300}>
              <BarChart data={throughputData}>
                <CartesianGrid strokeDasharray="3 3" stroke="#e0e6ed" />
                <XAxis dataKey="time" label={{ value: 'Time (sec)', position: 'insideBottomRight', offset: -5 }} />
                <YAxis label={{ value: 'Orders/sec', angle: -90, position: 'insideLeft' }} />
                <Tooltip content={<CustomTooltip />} />
                <Legend />
                <Bar
                  dataKey="throughput"
                  fill={theme.primary}
                  isAnimationActive={true}
                  animationDuration={300}
                  name="Throughput"
                />
              </BarChart>
            </ResponsiveContainer>
          </div>

          {/* Memory Usage */}
          <div className="graph-card">
            <h4>Memory Usage (MB)</h4>
            <ResponsiveContainer width="100%" height={300}>
              <AreaChart data={memoryData}>
                <defs>
                  <linearGradient id="memoryGradient" x1="0" y1="0" x2="0" y2="1">
                    <stop offset="5%" stopColor={theme.primary} stopOpacity={0.8} />
                    <stop offset="95%" stopColor={theme.primary} stopOpacity={0.1} />
                  </linearGradient>
                </defs>
                <CartesianGrid strokeDasharray="3 3" stroke="#e0e6ed" />
                <XAxis dataKey="time" label={{ value: 'Time (sec)', position: 'insideBottomRight', offset: -5 }} />
                <YAxis label={{ value: 'Memory (MB)', angle: -90, position: 'insideLeft' }} />
                <Tooltip content={<CustomTooltip />} />
                <Legend />
                <Area
                  type="monotone"
                  dataKey="memory"
                  stroke={theme.primary}
                  fill="url(#memoryGradient)"
                  isAnimationActive={true}
                  animationDuration={300}
                  name="Heap Usage"
                />
              </AreaChart>
            </ResponsiveContainer>
          </div>

          {/* Order Status */}
          <div className="graph-card">
            <h4>Order Processing Status</h4>
            <ResponsiveContainer width="100%" height={300}>
              <ComposedChart data={orderStatusData}>
                <defs>
                  <linearGradient id="orderGradient" x1="0" y1="0" x2="0" y2="1">
                    <stop offset="5%" stopColor={theme.primary} stopOpacity={0.8} />
                    <stop offset="95%" stopColor={theme.primary} stopOpacity={0.1} />
                  </linearGradient>
                </defs>
                <CartesianGrid strokeDasharray="3 3" stroke="#e0e6ed" />
                <XAxis dataKey="time" label={{ value: 'Time (sec)', position: 'insideBottomRight', offset: -5 }} />
                <YAxis yAxisId="left" label={{ value: 'Orders Matched', angle: -90, position: 'insideLeft' }} />
                <YAxis yAxisId="right" orientation="right" label={{ value: 'Queue Depth', angle: 90, position: 'insideRight' }} />
                <Tooltip content={<CustomTooltip />} />
                <Legend />
                <Area
                  yAxisId="left"
                  type="monotone"
                  dataKey="matched"
                  stroke={theme.primary}
                  fill="url(#orderGradient)"
                  isAnimationActive={true}
                  animationDuration={300}
                  name="Matched Orders"
                />
                <Line
                  yAxisId="right"
                  type="monotone"
                  dataKey="queueDepth"
                  stroke={theme.secondary}
                  strokeWidth={2}
                  dot={false}
                  isAnimationActive={true}
                  animationDuration={300}
                  name="Queue Depth"
                  strokeDasharray="5 5"
                />
              </ComposedChart>
            </ResponsiveContainer>
          </div>
        </div>

        {!isRunning && latencyData.length === 0 && (
          <div className="empty-state">
            <p>Run a stress test to see performance metrics</p>
          </div>
        )}
      </div>
    </div>
  );
}

export default PerformanceGraphs;
