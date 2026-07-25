import { useState } from "react";

function formatNumber(value) {
  if (value === null || value === undefined || Number.isNaN(value)) return "--";
  return Number(value).toLocaleString(undefined, { maximumFractionDigits: 2 });
}

function VerdictBadge({ verdict }) {
  const cls = verdict === "BUY" ? "sell" : verdict === "SELL" ? "buy" : "";
  // Note the flip: the *interviewer* buys/sells, meaning the player is on
  // the opposite side - a "BUY" verdict means the player sold, so it's
  // colored like a sell fill from the player's own point of view.
  return <span className={`game-verdict-badge ${cls}`}>{verdict}</span>;
}

function RoundHistoryTable({ rounds }) {
  if (!rounds || rounds.length === 0) {
    return <div className="empty-state">No rounds yet</div>;
  }
  return (
    <div className="game-history">
      <div className="game-history-header">
        <span>#</span>
        <span>Bid</span>
        <span>Ask</span>
        <span>Verdict</span>
        <span>Fill</span>
      </div>
      {rounds.map(r => (
        <div className="game-history-row" key={r.round_number}>
          <span>{r.round_number}</span>
          <span>{formatNumber(r.bid)}</span>
          <span>{formatNumber(r.ask)}</span>
          <VerdictBadge verdict={r.verdict} />
          <span>{r.fill_price === null || r.fill_price === undefined ? "--" : formatNumber(r.fill_price)}</span>
        </div>
      ))}
    </div>
  );
}

export default function MarketMakingGame({ send, token, connected, gameState }) {
  const [bid, setBid] = useState("");
  const [ask, setAsk] = useState("");

  const canPlay = connected && !!token;

  // No category picker: every stub scenario is tagged "estimation" right
  // now, so filtering by category has nothing to filter and previously just
  // dead-ended on any other input ("No scenarios available for category
  // '...'"). Bring this back once there's more than one category to choose
  // from.
  const startGame = (forceNew = false) => {
    if (!canPlay) return;
    send({ type: "game_start", token, force_new: forceNew });
  };

  const submitQuote = event => {
    event.preventDefault();
    if (!gameState || !canPlay) return;
    const bidNum = parseFloat(bid);
    const askNum = parseFloat(ask);
    if (Number.isNaN(bidNum) || Number.isNaN(askNum)) return;
    send({ type: "game_quote", token, session_id: gameState.sessionId, bid: bidNum, ask: askNum });
    setBid("");
    setAsk("");
  };

  const requestHint = () => {
    if (!gameState || !canPlay) return;
    send({ type: "game_hint", token, session_id: gameState.sessionId });
  };

  const endSession = () => {
    if (!gameState || !canPlay) return;
    send({ type: "game_end", token, session_id: gameState.sessionId });
  };

  const isActive = gameState && gameState.status === "active";
  const isEnded = gameState && gameState.status === "ended";

  return (
    <section className="panel game-panel">
      <div className="panel-heading">
        <h2>Market Making Practice</h2>
        <span>{gameState ? gameState.scenario.category : "Make Me a Market"}</span>
      </div>

      <div className="game-body">
        {!gameState && (
          <div className="game-start-row">
            <button
              type="button"
              className="submit-order buy"
              onClick={() => startGame(false)}
              disabled={!canPlay}
            >
              Start Practice Round
            </button>
          </div>
        )}

        {gameState && (
          <>
            <div className="game-scenario">
              <p className="game-prompt">{gameState.scenario.prompt}</p>
              <div className="game-scenario-meta">
                <span>Round {gameState.currentRound} / {gameState.maxRounds}</span>
                <span>Unit: {gameState.scenario.unit}</span>
              </div>
            </div>

            {gameState.hintsRevealed.length > 0 && (
              <ul className="game-hints">
                {gameState.hintsRevealed.map((hint, i) => (
                  <li key={i}>{hint}</li>
                ))}
              </ul>
            )}

            <div className="stress-test-stats">
              <div className="stress-stat">
                <span className="stress-stat-label">Position</span>
                <span className="stress-stat-value">{formatNumber(gameState.position)}</span>
              </div>
              <div className="stress-stat">
                <span className="stress-stat-label">Cash (synthetic)</span>
                <span className="stress-stat-value">{formatNumber(gameState.cash)}</span>
              </div>
              <div className="stress-stat">
                <span className="stress-stat-label">Status</span>
                <span className="stress-stat-value">{gameState.status}</span>
              </div>
            </div>

            {isActive && (
              <>
                {gameState.lastRoundResult && (
                  <p className="game-last-result">
                    Round {gameState.lastRoundResult.round_number}: interviewer{" "}
                    <VerdictBadge verdict={gameState.lastRoundResult.verdict} />
                    {gameState.lastRoundResult.fill_price !== null &&
                      ` at ${formatNumber(gameState.lastRoundResult.fill_price)}`}
                  </p>
                )}

                <form className="game-quote-form" onSubmit={submitQuote}>
                  <label>
                    Bid
                    <input
                      type="number"
                      step="any"
                      value={bid}
                      onChange={e => setBid(e.target.value)}
                      placeholder="e.g. 100"
                      required
                    />
                  </label>
                  <label>
                    Ask
                    <input
                      type="number"
                      step="any"
                      value={ask}
                      onChange={e => setAsk(e.target.value)}
                      placeholder="e.g. 120"
                      required
                    />
                  </label>
                  <button type="submit" className="submit-order buy">Submit Quote</button>
                </form>

                <div className="stress-test-controls">
                  <button type="button" onClick={requestHint}>Request Hint</button>
                  <button type="button" onClick={endSession}>End Session</button>
                </div>
              </>
            )}

            {isEnded && gameState.reveal && (
              <div className="game-reveal">
                <p className="game-reveal-headline">
                  True value: <strong>{formatNumber(gameState.reveal.true_value)}</strong>{" "}
                  ({gameState.reveal.reason === "max_rounds_reached" ? "rounds exhausted" : "you closed the session"})
                </p>
                <div className="stress-test-stats">
                  <div className="stress-stat">
                    <span className="stress-stat-label">Final Position</span>
                    <span className="stress-stat-value">{formatNumber(gameState.reveal.final_position)}</span>
                  </div>
                  <div className="stress-stat">
                    <span className="stress-stat-label">Final Cash</span>
                    <span className="stress-stat-value">{formatNumber(gameState.reveal.final_cash)}</span>
                  </div>
                  <div className="stress-stat">
                    <span className="stress-stat-label">Mark-to-Model P&amp;L</span>
                    <span className="stress-stat-value">{formatNumber(gameState.reveal.mark_to_model_pnl)}</span>
                  </div>
                </div>
                <button type="button" className="submit-order buy" onClick={() => startGame(true)}>
                  Play Again
                </button>
              </div>
            )}

            <RoundHistoryTable rounds={gameState.roundHistory} />
          </>
        )}
      </div>
    </section>
  );
}
