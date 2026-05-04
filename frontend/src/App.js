import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import "./App.css";

// const WS_URL = process.env.REACT_APP_WS_URL || "ws://20.205.25.160:9001";
const WS_URL = "ws://localhost:9001";

const currency = new Intl.NumberFormat("en-US", {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2
});

function formatPrice(price) {
  const value = Number(price);
  return Number.isFinite(value) ? currency.format(value) : "--";
}

function formatQuantity(quantity) {
  const value = Number(quantity);
  return Number.isFinite(value) ? value.toLocaleString("en-US") : "--";
}

function depthTotal(levels) {
  return levels.reduce((sum, level) => sum + Number(level.quantity || 0), 0);
}

function normalizedBook(msg) {
  const bids = msg.bids || [];
  const asks = msg.asks || [];
  const hasOrderSnapshots = Array.isArray(msg.buy_orders) && Array.isArray(msg.sell_orders);

  return {
    bids,
    asks,
    buyOrders: hasOrderSnapshots ? msg.buy_orders : bids,
    sellOrders: hasOrderSnapshots ? msg.sell_orders : asks
  };
}

function Stat({ label, value, tone }) {
  return (
    <div className="stat">
      <span>{label}</span>
      <strong className={tone ? `tone-${tone}` : ""}>{value}</strong>
    </div>
  );
}

function OrderTable({ title, side, rows, onCancel }) {
  const maxQuantity = Math.max(...rows.map(row => Number(row.quantity || 0)), 1);
  const hasOrderIds = rows.some(row => row.id !== undefined);
  const canCancel = hasOrderIds && typeof onCancel === "function";

  return (
    <section className="panel depth-panel">
      <div className="panel-heading">
        <h2>{title}</h2>
        <span>{rows.length} {hasOrderIds ? "open" : "levels"}</span>
      </div>

      <div className={`depth-header ${canCancel ? "with-cancel" : ""}`}>
        <span>Price</span>
        <span>Qty</span>
        <span>{hasOrderIds ? "Order" : "Orders"}</span>
        {canCancel && <span />}
      </div>

      <div className="depth-list">
        {rows.length === 0 ? (
          <div className="empty-state">No resting {side.toLowerCase()} orders</div>
        ) : (
          rows.map((row, index) => {
            const width = `${(Number(row.quantity || 0) / maxQuantity) * 100}%`;
            const rowKey = row.id ?? `${row.price}-${index}`;

            return (
              <div
                className={`depth-row ${side.toLowerCase()} ${canCancel ? "with-cancel" : ""}`}
                key={`${side}-${rowKey}-${row.timestamp || ""}`}
              >
                <div className="depth-bar" style={{ width }} />
                <span className="price">${formatPrice(row.price)}</span>
                <span>{formatQuantity(row.quantity)}</span>
                <span className="order-id">{row.id !== undefined ? `#${row.id}` : row.orders}</span>
                {canCancel && (
                  <button
                    type="button"
                    className="cancel-order"
                    aria-label={`Cancel ${side.toLowerCase()} order ${row.id}`}
                    title={`Cancel order #${row.id}`}
                    onClick={() => onCancel(side, row)}
                  >
                    ×
                  </button>
                )}
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}

function TradeFeed({ trades }) {
  return (
    <section className="panel trade-panel">
      <div className="panel-heading">
        <h2>Trade Tape</h2>
        <span>Last {trades.length}</span>
      </div>

      <div className="trade-list">
        {trades.length === 0 ? (
          <div className="empty-state">No executions yet</div>
        ) : (
          trades.map(trade => (
            <div className="trade-row" key={trade.rowId}>
              <div>
                <strong>${formatPrice(trade.price)}</strong>
                <span>{formatQuantity(trade.quantity)} shares</span>
              </div>
              <div className="trade-meta">
                <span>B{trade.buy_order_id} / S{trade.sell_order_id}</span>
                <time>{trade.time}</time>
              </div>
            </div>
          ))
        )}
      </div>
    </section>
  );
}

export default function App() {
  const [connected, setConnected] = useState(false);
  const [trades, setTrades] = useState([]);
  const [bids, setBids] = useState([]);
  const [asks, setAsks] = useState([]);
  const [buyOrders, setBuyOrders] = useState([]);
  const [sellOrders, setSellOrders] = useState([]);
  const [form, setForm] = useState({ side: "BUY", price: "", quantity: "" });
  const [lastError, setLastError] = useState("");
  const [marketReady, setMarketReady] = useState(false);

  const ws = useRef(null);
  const tradeSeq = useRef(1);

  const makeTradeRow = useCallback((trade, time = new Date().toLocaleTimeString()) => ({
    ...trade,
    rowId: `${trade.buy_order_id}-${trade.sell_order_id}-${tradeSeq.current++}`,
    time
  }), []);

  const applyBookSnapshot = useCallback(msg => {
    const book = normalizedBook(msg);

    setBids(book.bids);
    setAsks(book.asks);
    setBuyOrders(book.buyOrders);
    setSellOrders(book.sellOrders);
    setMarketReady(true);
  }, []);

  const applyTradeHistory = useCallback(trades => {
    setTrades(trades.slice(0, 20).map(trade => makeTradeRow(trade, "Earlier")));
  }, [makeTradeRow]);

  useEffect(() => {
    const socket = new WebSocket(WS_URL);

    socket.onopen = () => {
      setConnected(true);
      setMarketReady(false);
      setLastError("");
      socket.send(JSON.stringify({ type: "snapshot" }));
    };

    socket.onmessage = event => {
      try {
        const msg = JSON.parse(event.data);

        if (msg.type === "trade") {
          setTrades(prev => [makeTradeRow(msg), ...prev].slice(0, 20));
        }

        if (msg.type === "history") {
          applyTradeHistory(msg.trades || []);
        }

        if (msg.type === "book") {
          applyBookSnapshot(msg);
        }

        if (msg.type === "snapshot") {
          applyTradeHistory(msg.trades || []);
          applyBookSnapshot(msg);
        }
      } catch (error) {
        setLastError("Received an invalid exchange message.");
      }
    };

    socket.onerror = () => {
      setLastError("WebSocket error. Check the engine URL or server status.");
    };

    socket.onclose = () => {
      setConnected(false);
      setMarketReady(false);
    };

    ws.current = socket;

    return () => socket.close();
  }, [applyBookSnapshot, applyTradeHistory, makeTradeRow]);

  const submitOrder = event => {
    event.preventDefault();

    if (!ws.current || ws.current.readyState !== WebSocket.OPEN) {
      setLastError("Connect to the exchange before sending an order.");
      return;
    }

    if (!marketReady) {
      setLastError("Waiting for the market snapshot before sending an order.");
      return;
    }

    if (!form.price || !form.quantity) {
      setLastError("Enter both price and quantity.");
      return;
    }

    const order = {
      side: form.side,
      price: parseFloat(form.price),
      quantity: parseInt(form.quantity, 10),
      timestamp: Date.now()
    };

    ws.current.send(JSON.stringify(order));
    setForm(current => ({ ...current, price: "", quantity: "" }));
    setLastError("");
  };

  const cancelOrder = (side, row) => {
    if (!ws.current || ws.current.readyState !== WebSocket.OPEN) {
      setLastError("Connect to the exchange before cancelling an order.");
      return;
    }

    if (!marketReady) {
      setLastError("Waiting for the market snapshot before cancelling an order.");
      return;
    }

    if (row.id === undefined) {
      setLastError("Cannot cancel a price level without a specific order id.");
      return;
    }

    ws.current.send(JSON.stringify({
      type: "cancel",
      side,
      order_id: row.id,
      price: Number(row.price)
    }));
    setLastError("");
  };

  const stats = useMemo(() => {
    const bestBid = bids[0]?.price;
    const bestAsk = asks[0]?.price;
    const spread = Number.isFinite(Number(bestBid)) && Number.isFinite(Number(bestAsk))
      ? Number(bestAsk) - Number(bestBid)
      : null;

    return {
      bestBid,
      bestAsk,
      spread,
      bidVolume: depthTotal(bids),
      askVolume: depthTotal(asks)
    };
  }, [bids, asks]);

  return (
    <main className="exchange-app">
      <header className="topbar">
        <div>
          <p className="eyebrow">NUS Orbital 2026 · Apollo</p>
          <h1>O(1) Exchange</h1>
        </div>
        <div className="connection-stack">
          <span className={`status-pill ${connected ? "live" : "offline"}`}>
            <span />
            {connected ? "LIVE" : "DISCONNECTED"}
          </span>
          <small>{WS_URL.replace("ws://", "")}</small>
        </div>
      </header>

      <section className="market-strip">
        <Stat label="Best Bid" value={`$${formatPrice(stats.bestBid)}`} tone="buy" />
        <Stat label="Best Ask" value={`$${formatPrice(stats.bestAsk)}`} tone="sell" />
        <Stat label="Spread" value={stats.spread === null ? "--" : `$${formatPrice(stats.spread)}`} />
        <Stat label="Bid Volume" value={formatQuantity(stats.bidVolume)} tone="buy" />
        <Stat label="Ask Volume" value={formatQuantity(stats.askVolume)} tone="sell" />
      </section>

      <div className="workspace">
        <section className="panel order-ticket">
          <div className="panel-heading">
            <h2>Order Ticket</h2>
            <span>Limit order</span>
          </div>

          <form onSubmit={submitOrder}>
            <div className="side-toggle" role="group" aria-label="Order side">
              {["BUY", "SELL"].map(side => (
                <button
                  type="button"
                  key={side}
                  className={form.side === side ? side.toLowerCase() : ""}
                  onClick={() => setForm(current => ({ ...current, side }))}
                >
                  {side}
                </button>
              ))}
            </div>

            <label>
              Price
              <input
                type="number"
                min="0"
                step="0.01"
                inputMode="decimal"
                placeholder="102.50"
                value={form.price}
                onChange={event => setForm(current => ({ ...current, price: event.target.value }))}
              />
            </label>

            <label>
              Quantity
              <input
                type="number"
                min="1"
                step="1"
                inputMode="numeric"
                placeholder="150"
                value={form.quantity}
                onChange={event => setForm(current => ({ ...current, quantity: event.target.value }))}
              />
            </label>

            {lastError && <p className="error-text">{lastError}</p>}

            <button className={`submit-order ${form.side.toLowerCase()}`} type="submit">
              Send {form.side} Order
            </button>
          </form>
        </section>

        <div className="book-grid">
          <OrderTable title="Buy Orders" side="BUY" rows={buyOrders} onCancel={cancelOrder} />
          <OrderTable title="Sell Orders" side="SELL" rows={sellOrders} onCancel={cancelOrder} />
        </div>

        <TradeFeed trades={trades} />
      </div>
    </main>
  );
}
