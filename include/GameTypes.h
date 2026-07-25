#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Domain types for the Market Making Practice game mode. Kept dependency-free
// (no boost/json/sqlite) so GameManager.h and Server.h are the only places
// that need to know about the transport/persistence layers.

struct Scenario
{
    int id;
    std::string category;
    std::string prompt;
    std::string unit; // display-only, e.g. "windows", "minutes", "taxis"
    double true_value; // hidden from the client
    std::vector<std::string> hints; // revealed progressively, cheapest/vaguest first
};

enum class GameStatus
{
    ACTIVE,
    ENDED
};

enum class RoundVerdict
{
    BUY,
    SELL,
    PASS
};

struct Round
{
    int round_number;
    double bid;
    double ask;
    RoundVerdict verdict;
    bool has_fill; // false when verdict == PASS
    double fill_price; // meaningful only when has_fill is true
    int64_t ts;
};

struct GameSession
{
    int64_t id;
    int64_t user_id;
    int scenario_id;
    int max_rounds;
    int current_round = 0; // rounds completed so far
    double position = 0.0; // net long(+)/short(-) synthetic units
    double cash = 0.0; // synthetic P&L, never touches the real wallet
    std::vector<int> hints_revealed; // indices into scenario.hints
    GameStatus status = GameStatus::ACTIVE;
    std::vector<Round> rounds;
};

inline const char *round_verdict_to_string(RoundVerdict v)
{
    switch (v)
    {
    case RoundVerdict::BUY:
        return "BUY";
    case RoundVerdict::SELL:
        return "SELL";
    case RoundVerdict::PASS:
        return "PASS";
    }
    return "PASS";
}

inline RoundVerdict round_verdict_from_string(const std::string &s)
{
    if (s == "BUY")
        return RoundVerdict::BUY;
    if (s == "SELL")
        return RoundVerdict::SELL;
    return RoundVerdict::PASS;
}
