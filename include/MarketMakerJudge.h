#pragma once

#include <cmath>

// Standalone market-making "interviewer" judge. No boost/json/sqlite/Server
// dependency on purpose - this is the piece of the practice-game feature
// that most benefits from being a pure function tested in total isolation.
//
// The judge never seeds or calls an RNG itself: callers (GameManager) draw
// noise_draw from their own RNG and pass it in, so every branch here is
// deterministically reproducible in a unit test.
namespace judge
{

enum class Verdict
{
    BUY,    // interviewer buys at the player's ask (player is the seller)
    SELL,   // interviewer sells at the player's bid (player is the buyer)
    PASS,   // true_value falls inside [bid, ask] - no trade
    INVALID // crossed quote (bid > ask)
};

struct Quote
{
    double bid;
    double ask;
};

struct JudgeResult
{
    Verdict verdict;
    double fill_price = 0.0; // meaningful only when verdict is BUY or SELL
};

// noise_epsilon: half-width of the band around true_value within which a
// quote sitting exactly on the boundary gets its outcome flipped by a coin
// toss instead of the deterministic PASS. Pass 0.0 (the default) to disable
// noise entirely - the boundary then stays perfectly deterministic.
// noise_draw: caller-supplied value in [0,1); only consulted when the
// noise band is enabled and a quote lands inside it.
inline JudgeResult judge_quote(const Quote &q, double true_value,
                                double noise_epsilon = 0.0, double noise_draw = 0.0)
{
    if (q.bid > q.ask)
        return {Verdict::INVALID, 0.0};

    // Strict inequalities: true_value landing exactly on bid or ask is a
    // PASS, not a trade - the interviewer only trades when it has a real
    // edge, never at a break-even price.
    double ask_edge = true_value - q.ask; // > 0 means buying at ask is profitable for the interviewer
    double bid_edge = q.bid - true_value; // > 0 means selling at bid is profitable for the interviewer

    if (noise_epsilon > 0.0)
    {
        bool near_ask = std::fabs(ask_edge) <= noise_epsilon;
        bool near_bid = std::fabs(bid_edge) <= noise_epsilon;
        if (near_ask || near_bid)
        {
            if (noise_draw < 0.5)
                return {Verdict::PASS, 0.0};
            // Coin flip landed on "trade" - pick whichever side is actually
            // in-the-money (or nearer to it) for the interviewer.
            if (near_ask && ask_edge >= bid_edge)
                return {Verdict::BUY, q.ask};
            if (near_bid)
                return {Verdict::SELL, q.bid};
            return {Verdict::PASS, 0.0};
        }
    }

    // A single price can never be simultaneously below and above
    // true_value, so ask_edge and bid_edge can't both be strictly positive
    // at once - the >= tie-break below is defensive, not reachable in
    // normal operation, but keeps the function total.
    if (ask_edge > 0.0 && ask_edge >= bid_edge)
        return {Verdict::BUY, q.ask};
    if (bid_edge > 0.0)
        return {Verdict::SELL, q.bid};
    return {Verdict::PASS, 0.0};
}

} // namespace judge
