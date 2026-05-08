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
    companyId: msg.company_id,
    companyName: msg.company_name,
    companySymbol: msg.company_symbol,
    totalShares: msg.total_shares,
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
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
                      <line x1="18" y1="6" x2="6" y2="18" />
                      <line x1="6" y1="6" x2="18" y2="18" />
                    </svg>
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
                <span>{trade.company_name}</span>
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
  const [companies, setCompanies] = useState([]);
  const [selectedCompanyId, setSelectedCompanyId] = useState(null);
  const [selectedCompany, setSelectedCompany] = useState(null);
  const [bids, setBids] = useState([]);
  const [asks, setAsks] = useState([]);
  const [buyOrders, setBuyOrders] = useState([]);
  const [sellOrders, setSellOrders] = useState([]);
  const [form, setForm] = useState({ side: "BUY", price: "", quantity: "" });
  const [priceError, setPriceError] = useState("");
  const [quantityError, setQuantityError] = useState("");
  const [lastError, setLastError] = useState("");
  const [marketReady, setMarketReady] = useState(false);

  const ws = useRef(null);
  const tradeSeq = useRef(1);
  const selectedCompanyIdRef = useRef(null);
  const reconnectTimeoutRef = useRef(null);
  const reconnectAttemptsRef = useRef(0);
  const shouldReconnectRef = useRef(true);

  const makeTradeRow = useCallback((trade, time = new Date().toLocaleTimeString()) => ({
    ...trade,
    rowId: `${trade.buy_order_id}-${trade.sell_order_id}-${tradeSeq.current++}`,
    time
  }), []);

  
  const applyBookSnapshot = useCallback(msg => {
    const book = normalizedBook(msg);

    setSelectedCompany({
      id: book.companyId,
      name: book.companyName,
      symbol: book.companySymbol,
      totalShares: book.totalShares
    });
    selectedCompanyIdRef.current = book.companyId;
    setSelectedCompanyId(book.companyId);
    setBids(book.bids);
    setAsks(book.asks);
    setBuyOrders(book.buyOrders);
    setSellOrders(book.sellOrders);
    setMarketReady(true);
  }, []);

  const applyTradeHistory = useCallback(trades => {
    setTrades(trades.slice(0, 20).map(trade => makeTradeRow(trade, "Earlier")));
  }, [makeTradeRow]);

  const connectWebSocket = useCallback(() => {
    if (reconnectTimeoutRef.current !== null) {
      clearTimeout(reconnectTimeoutRef.current);
      reconnectTimeoutRef.current = null;
    }

    if (
      ws.current &&
      (ws.current.readyState === WebSocket.OPEN || ws.current.readyState === WebSocket.CONNECTING)
    ) {
      return;
    }

    const socket = new WebSocket(WS_URL);

    socket.onopen = () => {
      setConnected(true);
      setMarketReady(false);
      setLastError("");
      reconnectAttemptsRef.current = 0;
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
          if (
            selectedCompanyIdRef.current === null ||
            Number(msg.company_id) === Number(selectedCompanyIdRef.current)
          ) {
            applyBookSnapshot(msg);
          }
        }

        if (msg.type === "snapshot") {
          if (Array.isArray(msg.companies)) {
            setCompanies(msg.companies);
          }
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
      if (ws.current === socket) {
        ws.current = null;
      }

      if (shouldReconnectRef.current && reconnectTimeoutRef.current === null) {
        const delay = Math.min(1000 * 2 ** reconnectAttemptsRef.current, 30000);
        reconnectAttemptsRef.current += 1;
        reconnectTimeoutRef.current = setTimeout(() => {
          reconnectTimeoutRef.current = null;
          connectWebSocket();
        }, delay);
      }
    };

    ws.current = socket;
  }, [applyBookSnapshot, applyTradeHistory, makeTradeRow]);

  useEffect(() => {
    shouldReconnectRef.current = true;
    connectWebSocket();

    return () => {
      shouldReconnectRef.current = false;
      if (ws.current) {
        ws.current.close();
        ws.current = null;
      }
      if (reconnectTimeoutRef.current !== null) {
        clearTimeout(reconnectTimeoutRef.current);
        reconnectTimeoutRef.current = null;
      }
    };
  }, [connectWebSocket]);

  const requestSnapshot = useCallback(companyId => {
    if (!ws.current || ws.current.readyState !== WebSocket.OPEN) {
      return;
    }

    setMarketReady(false);
    ws.current.send(JSON.stringify({
      type: "snapshot",
      company_id: companyId
    }));
  }, []);

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
      validatePrice(form.price);
      validateQuantity(form.quantity);
      setLastError("Enter both price and quantity.");
      return;
    }

    // Validate using inline validation states
    const priceValid = validatePrice(form.price);
    const quantityValid = validateQuantity(form.quantity);

    if (!priceValid || !quantityValid) {
      return;
    }

    const order = {
      type: "order",
      company_id: selectedCompanyId,
      side: form.side,
      price: parseFloat(form.price),
      quantity: parseInt(form.quantity, 10),
      timestamp: Date.now()
    };

    ws.current.send(JSON.stringify(order));
    setForm(current => ({ ...current, price: "", quantity: "" }));
    setPriceError("");
    setQuantityError("");
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
      company_id: selectedCompanyId,
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

  const validatePrice = value => {
    if (value === "") {
      setPriceError("");
      setLastError("");
      return false;
    }
    const num = parseFloat(value);
    if (isNaN(num)) {
      setPriceError("Must be a valid number");
      setLastError("Enter a valid price and quantity.");
      return false;
    }
    if (num <= 0) {
      setPriceError("Must be greater than 0");
      setLastError("Enter a valid price and quantity.");
      return false;
    }
    if (num > 1000000) {
      setPriceError("Must be less than 1,000,000");
      setLastError("Enter a valid price and quantity.");
      return false;
    }
    setPriceError("");
    if (!quantityError) {
      setLastError("");
    }
    return true;
  };

  const validateQuantity = value => {
    if (value === "") {
      setQuantityError("");
      setLastError("");
      return false;
    }
    const num = parseInt(value, 10);
    if (isNaN(num)) {
      setQuantityError("Must be a valid integer");
      setLastError("Enter a valid price and quantity.");
      return false;
    }
    if (num < 1) {
      setQuantityError("Must be at least 1");
      setLastError("Enter a valid price and quantity.");
      return false;
    }
    if (num > 1000000) {
      setQuantityError("Must be less than 1,000,000");
      setLastError("Enter a valid price and quantity.");
      return false;
    }
    setQuantityError("");
    if (!priceError) {
      setLastError("");
    }
    return true;
  };

  const handleCompanyChange = event => {
    const companyId = Number(event.target.value);
    selectedCompanyIdRef.current = companyId;
    setSelectedCompanyId(companyId);
    requestSnapshot(companyId);
  };

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
        <section className="market-cluster">
          <div className="cluster-header">
            <div>
              <p className="cluster-label">Selected Market</p>
              <h2>{selectedCompany ? `${selectedCompany.name} (${selectedCompany.symbol})` : "Loading market"}</h2>
            </div>
            <span>{marketReady ? "Instrument-specific order flow" : "Refreshing book"}</span>
          </div>

          <div className="cluster-body">
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
                Company
                <select value={selectedCompanyId ?? ""} onChange={handleCompanyChange}>
                  {companies.map(company => (
                    <option key={company.id} value={company.id}>
                      {company.name} ({company.symbol})
                    </option>
                  ))}
                </select>
              </label>

              <label>
                Price
                <input
                  type="number"
                  min="0.01"
                  max="1000000"
                  step="0.01"
                  inputMode="decimal"
                  placeholder="102.50"
                  value={form.price}
                  onChange={event => {
                    const value = event.target.value;
                    setForm(current => ({ ...current, price: value }));
                    validatePrice(value);
                  }}
                />
                {priceError && <span className="error-text">{priceError}</span>}
              </label>

              <label>
                Quantity
                <input
                  type="number"
                  min="1"
                  max="1000000"
                  step="1"
                  inputMode="numeric"
                  placeholder="150"
                  value={form.quantity}
                  onChange={event => {
                    const value = event.target.value;
                    setForm(current => ({ ...current, quantity: value }));
                    validateQuantity(value);
                  }}
                />
                {quantityError && <span className="error-text">{quantityError}</span>}
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
          </div>
        </section>

        <TradeFeed trades={trades} />
      </div>
    </main>
  );
}
