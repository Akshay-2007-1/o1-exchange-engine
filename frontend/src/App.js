import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import "./App.css";

// const WS_URL = process.env.REACT_APP_WS_URL || "ws://20.205.25.160:9001";
const WS_URL = "ws://127.0.0.1:9001";

const currency = new Intl.NumberFormat("en-US", {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2
});

function formatPrice(cents) {
  const value = Number(cents) / 100;
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

function Wallet({ cash, portfolio, companies }) {
  const getCompanySymbol = (id) => {
    return companies.find(c => c.id === id)?.symbol || `#${id}`;
  };

  return (
    <section className="panel wallet-panel">
      <div className="panel-heading">
        <h2>Your Account</h2>
        <span className="cash-balance">${currency.format(cash)}</span>
      </div>
      <div className="portfolio-list">
        <h3>Portfolio</h3>
        {portfolio.length === 0 ? (
          <div className="empty-state">No shares held</div>
        ) : (
          portfolio.map(item => (
            <div className="portfolio-row" key={item.company_id}>
              <span className="symbol">{getCompanySymbol(item.company_id)}</span>
              <span className="shares">{formatQuantity(item.shares)}</span>
            </div>
          ))
        )}
      </div>
    </section>
  );
}

function LoginForm({ onLogin, onSwitch, error }) {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");

  const handleSubmit = (e) => {
    e.preventDefault();
    onLogin(username, password);
  };

  return (
    <section className="auth-card">
      <h2>Welcome Back</h2>
      <p>Log in to your O(1) Exchange account</p>
      <form onSubmit={handleSubmit}>
        <label>
          Username
          <input type="text" value={username} onChange={e => setUsername(e.target.value)} required />
        </label>
        <label>
          Password
          <input type="password" value={password} onChange={e => setPassword(e.target.value)} required />
        </label>
        {error && <p className="error-text">{error}</p>}
        <button type="submit" className="submit-order buy">Login</button>
      </form>
      <button className="text-button" onClick={onSwitch}>Need an account? Register</button>
    </section>
  );
}

function RegisterForm({ onRegister, onSwitch, error, success }) {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");

  const handleSubmit = (e) => {
    e.preventDefault();
    onRegister(username, password);
  };

  return (
    <section className="auth-card">
      <h2>Create Account</h2>
      <p>Join the future of high-frequency trading</p>
      <form onSubmit={handleSubmit}>
        <label>
          Username
          <input type="text" value={username} onChange={e => setUsername(e.target.value)} required />
        </label>
        <label>
          Password
          <input type="password" value={password} onChange={e => setPassword(e.target.value)} required minLength={6} />
        </label>
        {error && <p className="error-text">{error}</p>}
        {success && <p className="success-text">{success}</p>}
        <button type="submit" className="submit-order buy">Register</button>
      </form>
      <button className="text-button" onClick={onSwitch}>Already have an account? Login</button>
    </section>
  );
}

// OrderTable is now read-only for the public depth
function OrderTable({ title, side, rows }) {
  const maxQuantity = Math.max(...rows.map(row => Number(row.quantity || 0)), 1);
  const hasOrderIds = rows.some(row => row.id !== undefined);

  return (
    <section className="panel depth-panel">
      <div className="panel-heading">
        <h2>{title}</h2>
        <span>{rows.length} {hasOrderIds ? "open" : "levels"}</span>
      </div>

      <div className="depth-header">
        <span>Price</span>
        <span>Qty</span>
        <span>{hasOrderIds ? "Order" : "Orders"}</span>
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
                className={`depth-row ${side.toLowerCase()}`}
                key={`${side}-${rowKey}-${row.timestamp || ""}`}
              >
                <div className="depth-bar" style={{ width }} />
                <span className="price">${formatPrice(row.price)}</span>
                <span>{formatQuantity(row.quantity)}</span>
                <span className="order-id">{row.id !== undefined ? `#${row.id}` : row.orders}</span>
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}

// Dedicated secure cancellation panel
function MyOrdersPanel({ orders, onCancel, companies }) {
  const getCompanySymbol = (id) => companies.find(c => c.id === id)?.symbol || `#${id}`;

  return (
     <section className="panel bottom-panel">
       <div className="panel-heading">
         <h2>My Open Orders</h2>
         <span>{orders.length} Active</span>
       </div>
       <div className="my-orders-header">
         <span>Side</span>
         <span>Symbol</span>
         <span>Price</span>
         <span>Quantity</span>
         <span>Order ID</span>
         <span />
       </div>
       <div className="my-orders-list">
         {orders.length === 0 ? <div className="empty-state">No open orders</div> : (
            orders.map(o => (
               <div className="my-order-row" key={o.id}>
                  <span className={`my-order-side ${o.side.toLowerCase()}`}>{o.side}</span>
                  <span className="my-order-symbol">{getCompanySymbol(o.company_id)}</span>
                  <span>${formatPrice(o.price)}</span>
                  <span>{formatQuantity(o.quantity)}</span>
                  <span className="my-order-id">#{o.id}</span>
                  <div className="my-order-action">
                    <button 
                      className="cancel-btn" 
                      onClick={() => onCancel(o.side, o)}
                    >
                      Cancel
                    </button>
                  </div>
               </div>
            ))
         )}
       </div>
     </section>
  );
}

function TradeFeed({ trades, companies }) {
  const getCompanyIdentity = (id) => {
    const company = companies.find(c => c.id === id);
    return company ? company.symbol : `ID:${id}`;
  };

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
              <span className="trade-price">${formatPrice(trade.price)}</span>
              <span className="trade-qty">{formatQuantity(trade.quantity)}</span>
              <div className="trade-meta">
                <span className="trade-symbol">{getCompanyIdentity(trade.company_id)}</span>
                <time>{trade.time}</time>
              </div>
            </div>
          ))
        )}
      </div>
    </section>
  );
}

function PriceChart({ prices, symbol }) {
  if (!prices || prices.length < 2) {
    return (
      <section className="panel price-chart-panel">
        <div className="panel-heading">
          <h2>Price Chart</h2>
          <span>{symbol}</span>
        </div>
        <div className="empty-state" style={{ padding: "2rem 0" }}>Waiting for trades...</div>
      </section>
    );
  }

  const W = 400;
  const H = 120;
  const pad = 8;
  const minP = Math.min(...prices);
  const maxP = Math.max(...prices);
  const range = maxP - minP || 1;

  const pts = prices.map((p, i) => {
    const x = pad + (i / (prices.length - 1)) * (W - pad * 2);
    const y = H - pad - ((p - minP) / range) * (H - pad * 2);
    return `${x},${y}`;
  }).join(" ");

  const last = prices[prices.length - 1];
  const first = prices[0];
  const up = last >= first;

  return (
    <section className="panel price-chart-panel">
      <div className="panel-heading">
        <h2>Price Chart</h2>
        <span style={{ color: up ? "var(--buy)" : "var(--sell)" }}>
          {symbol} · ${formatPrice(last)}
        </span>
      </div>
      <svg viewBox={`0 0 ${W} ${H}`} style={{ width: "100%", height: H, display: "block" }}>
        <polyline
          points={pts}
          fill="none"
          stroke={up ? "var(--buy, #26a69a)" : "var(--sell, #ef5350)"}
          strokeWidth="2"
          strokeLinejoin="round"
        />
      </svg>
      <div style={{ display: "flex", justifyContent: "space-between", fontSize: "0.7rem", color: "var(--muted)", padding: "0 8px 4px" }}>
        <span>${formatPrice(minP)}</span>
        <span>{prices.length} trades</span>
        <span>${formatPrice(maxP)}</span>
      </div>
    </section>
  );
}

function MyTradesPanel({ trades, userId, companies }) {
  const getSymbol = id => companies.find(c => c.id === id)?.symbol || `#${id}`;

  return (
    <section className="panel bottom-panel">
      <div className="panel-heading">
        <h2>My Trade History</h2>
        <span>{trades.length} fills</span>
      </div>
      <div className="my-orders-header">
        <span>Side</span>
        <span>Symbol</span>
        <span>Price</span>
        <span>Qty</span>
        <span>Time</span>
      </div>
      <div className="my-orders-list">
        {trades.length === 0 ? (
          <div className="empty-state">No fills yet</div>
        ) : (
          trades.map((t, i) => {
            const isBuy = Number(t.buyer_id) === Number(userId);
            return (
              <div className="my-order-row" key={i}>
                <span className={`my-order-side ${isBuy ? "buy" : "sell"}`}>{isBuy ? "BUY" : "SELL"}</span>
                <span className="my-order-symbol">{getSymbol(t.company_id)}</span>
                <span>${formatPrice(t.price)}</span>
                <span>{formatQuantity(t.quantity)}</span>
                <span style={{ fontSize: "0.75rem", color: "var(--muted)" }}>
                  {new Date(t.ts * 1000).toLocaleTimeString()}
                </span>
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}

export default function App() {
  const [view, setView] = useState("LOADING"); // LOADING, LOGIN, REGISTER, DASHBOARD
  const [user, setUser] = useState(null);
  const userRef = useRef(null);
  const [cash, setCash] = useState(0);
  const [portfolio, setPortfolio] = useState([]);
  const [authError, setAuthError] = useState("");
  const [authSuccess, setAuthSuccess] = useState("");

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

  const [myOrdersByCompany, setMyOrdersByCompany] = useState({});
  const [priceHistory, setPriceHistory] = useState({});
  const [myTrades, setMyTrades] = useState([]);

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

    if (userRef.current) {
      const buys = book.buyOrders
        .filter(o => Number(o.user_id) === Number(userRef.current.userId))
        .map(o => ({ ...o, side: "BUY", company_id: book.companyId }));
      const sells = book.sellOrders
        .filter(o => Number(o.user_id) === Number(userRef.current.userId))
        .map(o => ({ ...o, side: "SELL", company_id: book.companyId }));
      setMyOrdersByCompany(prev => ({
        ...prev,
        [book.companyId]: { buys, sells }
      }));
    }
  }, []);

  const applyTradeHistory = useCallback(trades => {
    setTrades(trades.slice(0, 20).map(trade => makeTradeRow(trade, "Earlier")));
  }, []);

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
    // Crucial: Instruct browser to not convert blobs, giving us raw binary access
    socket.binaryType = "arraybuffer";

    socket.onopen = () => {
      setConnected(true);
      setMarketReady(false);
      setLastError("");
      setPriceHistory({});
      reconnectAttemptsRef.current = 0;

      const savedUser = localStorage.getItem("exchange_user");
      if (savedUser) {
        const parsed = JSON.parse(savedUser);
        userRef.current = parsed;
        setUser(parsed);
        setView("DASHBOARD");
        socket.send(JSON.stringify({ type: "snapshot", token: parsed.token }));
        socket.send(JSON.stringify({ type: "my_trades", token: parsed.token }));
      } else {
        setView("LOGIN");
      }
    };

    socket.onmessage = event => {
      // Hot-path binary parsing
      if (event.data instanceof ArrayBuffer) {
        const view = new DataView(event.data);
        const msgType = view.getUint8(0);
        
        if (msgType === 1) { // BinaryTradeMsg
          const trade = {
            type: "trade",
            company_id: view.getUint16(1, true),
            price: view.getUint32(3, true),
            quantity: view.getUint32(7, true),
            buy_order_id: Number(view.getBigUint64(11, true)),
            sell_order_id: Number(view.getBigUint64(19, true))
          };
          setTrades(prev => [makeTradeRow(trade), ...prev].slice(0, 20));
          setPriceHistory(prev => {
            const cid = trade.company_id;
            const existing = prev[cid] || [];
            return { ...prev, [cid]: [...existing, trade.price].slice(-60) };
          });
        }
        return;
      }

      // JSON Control-Path Parsing
      try {
        const msg = JSON.parse(event.data);

        if (msg.type === "error") {
          setAuthError(msg.message);
          setLastError(msg.message);
        }

        if (msg.type === "registered") {
          setAuthSuccess(msg.message);
          setAuthError("");
          setTimeout(() => setView("LOGIN"), 1500);
        }

        if (msg.type === "logged_in") {
          const userData = {
            token: msg.token,
            userId: msg.user_id,
            username: msg.username
          };
          localStorage.setItem("exchange_user", JSON.stringify(userData));
          userRef.current = userData;
          setUser(userData);
          setAuthError("");
          setView("DASHBOARD");
          socket.send(JSON.stringify({ type: "snapshot", token: userData.token }));
          socket.send(JSON.stringify({ type: "my_trades", token: userData.token }));
        }

        if (msg.type === "user_update") {
          setCash(msg.cash);
          setPortfolio(msg.portfolio || []);
          const token = userRef.current?.token;
          if (token && socket.readyState === WebSocket.OPEN)
            socket.send(JSON.stringify({ type: "my_trades", token }));
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
          if (Array.isArray(msg.trades) && msg.trades.length > 0) {
            const cid = msg.company_id;
            const prices = [...msg.trades].reverse().map(t => t.price);
            setPriceHistory(prev => ({ ...prev, [cid]: prices.slice(-60) }));
          }
        }

        if (msg.type === "my_trades") {
          setMyTrades(msg.trades || []);
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
  }, [applyBookSnapshot, applyTradeHistory]);

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

  const makeTradeRow = (trade, time = new Date().toLocaleTimeString()) => ({
    ...trade,
    rowId: `${trade.buy_order_id}-${trade.sell_order_id}-${tradeSeq.current++}`,
    time
  });

  const handleLogin = (username, password) => {
    if (ws.current?.readyState === WebSocket.OPEN) {
      ws.current.send(JSON.stringify({ type: "login", username, password }));
    }
  };

  const handleRegister = (username, password) => {
    if (ws.current?.readyState === WebSocket.OPEN) {
      ws.current.send(JSON.stringify({ type: "register", username, password }));
    }
  };

  const handleLogout = () => {
    localStorage.removeItem("exchange_user");
    userRef.current = null;
    setUser(null);
    setCash(0);
    setPortfolio([]);
    setMyOrdersByCompany({});
    setView("LOGIN");
  };

  const requestSnapshot = useCallback(companyId => {
    if (!ws.current || ws.current.readyState !== WebSocket.OPEN) {
      return;
    }

    setMarketReady(false);
    ws.current.send(JSON.stringify({
      type: "snapshot",
      company_id: companyId,
      token: user?.token
    }));
  }, [user]);

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

    const priceValid = validatePrice(form.price);
    const quantityValid = validateQuantity(form.quantity);

    if (!priceValid || !quantityValid) {
      return;
    }

    const order = {
      type: "order",
      token: user?.token,
      company_id: selectedCompanyId,
      side: form.side,
      price: Math.round(parseFloat(form.price) * 100),
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

    ws.current.send(JSON.stringify({
      type: "cancel",
      token: user?.token,
      company_id: row.company_id || selectedCompanyId, 
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

  const myOpenOrders = useMemo(() => {
    if (!user) return [];
    return Object.values(myOrdersByCompany)
      .flatMap(c => [...(c.buys || []), ...(c.sells || [])])
      .sort((a, b) => b.timestamp - a.timestamp);
  }, [myOrdersByCompany, user]);

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
    if (num > 999.99) {
      setPriceError("Must be less than $1,000.00");
      setLastError("Price limit exceeded.");
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

  if (view === "LOADING") {
    return (
      <div className="loading-screen">
        <div className="spinner" />
        <p>Connecting to O(1) Exchange...</p>
      </div>
    );
  }

  if (view === "LOGIN") {
    return (
      <main className="auth-page">
        <header className="topbar">
          <div>
            <p className="eyebrow">NUS Orbital 2026 · Apollo</p>
            <h1>O(1) Exchange</h1>
          </div>
        </header>
        <LoginForm 
          onLogin={handleLogin} 
          onSwitch={() => { setView("REGISTER"); setAuthError(""); }} 
          error={authError} 
        />
      </main>
    );
  }

  if (view === "REGISTER") {
    return (
      <main className="auth-page">
        <header className="topbar">
          <div>
            <p className="eyebrow">NUS Orbital 2026 · Apollo</p>
            <h1>O(1) Exchange</h1>
          </div>
        </header>
        <RegisterForm 
          onRegister={handleRegister} 
          onSwitch={() => { setView("LOGIN"); setAuthError(""); setAuthSuccess(""); }} 
          error={authError}
          success={authSuccess}
        />
      </main>
    );
  }

  return (
    <main className="exchange-app">
      <header className="topbar">
        <div>
          <p className="eyebrow">NUS Orbital 2026 · Apollo</p>
          <h1>O(1) Exchange</h1>
        </div>
        <div className="user-profile">
          <div className="user-info">
            <span className="username">{user?.username}</span>
            <button className="logout-button" onClick={handleLogout}>Logout</button>
          </div>
          <div className="connection-stack">
            <span className={`status-pill ${connected ? "live" : "offline"}`}>
              <span />
              {connected ? "LIVE" : "DISCONNECTED"}
            </span>
            <small>{WS_URL.replace("ws://", "")}</small>
          </div>
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
        <div className="top-row">
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
                    max="999.99"
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
                <OrderTable title="Buy Orders" side="BUY" rows={buyOrders} />
                <OrderTable title="Sell Orders" side="SELL" rows={sellOrders} />
              </div>
            </div>
          </section>

          <div className="sidebar">
            <Wallet cash={cash} portfolio={portfolio} companies={companies} />
            <PriceChart
              prices={priceHistory[selectedCompanyId] || []}
              symbol={selectedCompany?.symbol || ""}
            />
            <TradeFeed trades={trades} companies={companies} />
          </div>
        </div>
        
        <MyOrdersPanel orders={myOpenOrders} onCancel={cancelOrder} companies={companies} />
        <MyTradesPanel trades={myTrades} userId={user?.userId} companies={companies} />
      </div>
    </main>
  );
}