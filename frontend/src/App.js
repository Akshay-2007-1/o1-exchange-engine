import { useState, useEffect, useRef } from "react";

// const WS_URL = "ws://localhost:9001";
const WS_URL = "ws://20.205.25.160:9001";

export default function App() {
  const [connected, setConnected]   = useState(false);
  const [trades, setTrades]         = useState([]);
  const [bids, setBids]             = useState([]);
  const [asks, setAsks]             = useState([]);
  const [form, setForm]             = useState({
    side: "BUY", price: "", quantity: ""
  });
  const ws  = useRef(null);
  const oid = useRef(1);

  // ── Connect to WebSocket on mount ──
  useEffect(() => {
    const socket = new WebSocket(WS_URL);

    socket.onopen = () => {
      setConnected(true);
      console.log("Connected to exchange engine");
    };

    socket.onmessage = (event) => {
      const msg = JSON.parse(event.data);

      if (msg.type === "trade") {
        setTrades(prev => [{
          id:       `${msg.buy_order_id}x${msg.sell_order_id}`,
          price:    msg.price,
          quantity: msg.quantity,
          time:     new Date().toLocaleTimeString()
        }, ...prev].slice(0, 20));  // keep last 20 trades
      }
    };

    socket.onclose = () => setConnected(false);
    ws.current = socket;

    return () => socket.close();
  }, []);

  // ── Submit an order ────────────────
  const submitOrder = () => {
    if (!ws.current || !form.price || !form.quantity) return;

    const order = {
      id:        oid.current++,
      side:      form.side,
      price:     parseFloat(form.price),
      quantity:  parseInt(form.quantity),
      timestamp: Date.now()
    };

    ws.current.send(JSON.stringify(order));
    setForm(f => ({ ...f, price: "", quantity: "" }));
  };

  return (
    <div style={styles.app}>
      <div style={styles.header}>
        <h1 style={styles.title}>O(1) Exchange</h1>
        <span style={{
          ...styles.badge,
          background: connected ? "#1D9E75" : "#993C1D"
        }}>
          {connected ? "● LIVE" : "● DISCONNECTED"}
        </span>
      </div>

      <div style={styles.grid}>

        {/* ── Order Entry ── */}
        <div style={styles.card}>
          <h2 style={styles.cardTitle}>Place Order</h2>

          <div style={styles.toggle}>
            {["BUY", "SELL"].map(side => (
              <button
                key={side}
                onClick={() => setForm(f => ({ ...f, side }))}
                style={{
                  ...styles.toggleBtn,
                  background: form.side === side
                    ? (side === "BUY" ? "#1D9E75" : "#993C1D")
                    : "#2a2a2a",
                  color: form.side === side ? "#fff" : "#888"
                }}
              >
                {side}
              </button>
            ))}
          </div>

          <input
            style={styles.input}
            type="number"
            placeholder="Price (e.g. 102.50)"
            value={form.price}
            onChange={e => setForm(f => ({ ...f, price: e.target.value }))}
          />
          <input
            style={styles.input}
            type="number"
            placeholder="Quantity (e.g. 100)"
            value={form.quantity}
            onChange={e => setForm(f => ({ ...f, quantity: e.target.value }))}
          />
          <button
            onClick={submitOrder}
            style={{
              ...styles.submitBtn,
              background: form.side === "BUY" ? "#1D9E75" : "#993C1D"
            }}
          >
            Submit {form.side} Order
          </button>
        </div>

        {/* ── Trade Feed ── */}
        <div style={styles.card}>
          <h2 style={styles.cardTitle}>Recent Trades</h2>
          {trades.length === 0
            ? <p style={styles.empty}>No trades yet — submit matching orders</p>
            : trades.map(t => (
              <div key={t.id} style={styles.tradeRow}>
                <span style={styles.tradePrice}>${t.price.toFixed(2)}</span>
                <span style={styles.tradeQty}>{t.quantity} shares</span>
                <span style={styles.tradeTime}>{t.time}</span>
              </div>
            ))
          }
        </div>

      </div>
    </div>
  );
}

const styles = {
  app:        { background: "#111", minHeight: "100vh", padding: "24px", fontFamily: "monospace", color: "#eee" },
  header:     { display: "flex", alignItems: "center", gap: "16px", marginBottom: "24px" },
  title:      { fontSize: "24px", fontWeight: "bold", margin: 0 },
  badge:      { fontSize: "12px", padding: "4px 10px", borderRadius: "20px", color: "#fff" },
  grid:       { display: "grid", gridTemplateColumns: "1fr 1fr", gap: "16px" },
  card:       { background: "#1a1a1a", borderRadius: "12px", padding: "20px" },
  cardTitle:  { fontSize: "14px", fontWeight: "600", marginBottom: "16px", color: "#aaa", textTransform: "uppercase", letterSpacing: "0.05em" },
  toggle:     { display: "flex", gap: "8px", marginBottom: "12px" },
  toggleBtn:  { flex: 1, padding: "10px", borderRadius: "8px", border: "none", cursor: "pointer", fontWeight: "600", fontSize: "14px" },
  input:      { width: "100%", padding: "10px", marginBottom: "10px", borderRadius: "8px", border: "1px solid #333", background: "#111", color: "#eee", fontSize: "14px", boxSizing: "border-box" },
  submitBtn:  { width: "100%", padding: "12px", borderRadius: "8px", border: "none", cursor: "pointer", fontWeight: "700", fontSize: "15px", color: "#fff" },
  tradeRow:   { display: "flex", justifyContent: "space-between", padding: "8px 0", borderBottom: "1px solid #222" },
  tradePrice: { color: "#1D9E75", fontWeight: "600" },
  tradeQty:   { color: "#aaa", fontSize: "13px" },
  tradeTime:  { color: "#555", fontSize: "12px" },
  empty:      { color: "#555", fontSize: "13px" }
};