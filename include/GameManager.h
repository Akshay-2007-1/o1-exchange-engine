#pragma once

#include "Database.h"
#include "GameTypes.h"
#include "MarketMakerJudge.h"
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Orchestrates Market Making Practice game sessions: picks a scenario,
// judges quotes via judge::judge_quote(), and persists session/round state
// to Database.h's game_sessions/game_rounds tables. Deliberately never
// calls reserve_cash/reserve_shares/settle_trade/release_cash/release_shares
// or touches the real leaderboard - game P&L is entirely synthetic.
//
// Every public method returns the ordered list of JSON messages the caller
// should send back over the WebSocket, or throws GameError on a rejectable
// request (unknown/foreign session, crossed quote, no hints left, ...).

class GameError : public std::runtime_error
{
public:
    explicit GameError(const std::string &msg) : std::runtime_error(msg) {}
};

struct GameConfig
{
    int default_max_rounds = 5;
    double hint_penalty = 5.0;
    double unit_size = 1.0;
    double noise_epsilon = 0.0;
};

class GameManager
{
public:
    GameManager(Database &db, std::vector<Scenario> scenarios, GameConfig config = {})
        : db_(db), scenarios_(std::move(scenarios)), config_(config), rng_(std::random_device{}()) {}

    std::vector<nlohmann::json> start_session(int64_t user_id, const std::string &category, bool force_new)
    {
        if (!force_new)
        {
            auto active = db_.get_active_game_session(user_id);
            if (active.has_value())
                return {session_started_json(*active)};
        }
        else
        {
            auto active = db_.get_active_game_session(user_id);
            if (active.has_value())
                db_.end_game_session(active->id);
        }

        const Scenario &scenario = pick_scenario(category);
        auto created = db_.create_game_session(user_id, scenario.id, config_.default_max_rounds);
        if (!created.ok)
            throw GameError(created.error);

        auto fresh = db_.get_game_session(created.id, user_id);
        if (!fresh.has_value())
            throw GameError("Failed to load newly created session");

        return {session_started_json(*fresh)};
    }

    std::vector<nlohmann::json> submit_quote(int64_t user_id, int64_t session_id, double bid, double ask)
    {
        auto row = require_active_session(session_id, user_id);
        const Scenario &scenario = find_scenario(row.scenario_id);

        double noise_draw = unit_dist_(rng_);
        judge::JudgeResult result = judge::judge_quote({bid, ask}, scenario.true_value, config_.noise_epsilon, noise_draw);

        if (result.verdict == judge::Verdict::INVALID)
            throw GameError("Crossed quote: bid must be <= ask");

        double new_position = row.position;
        double new_cash = row.cash;
        bool has_fill = result.verdict != judge::Verdict::PASS;
        if (result.verdict == judge::Verdict::BUY)
        {
            // Interviewer buys at the player's ask -> the player is the seller.
            new_position -= config_.unit_size;
            new_cash += result.fill_price * config_.unit_size;
        }
        else if (result.verdict == judge::Verdict::SELL)
        {
            // Interviewer sells at the player's bid -> the player is the buyer.
            new_position += config_.unit_size;
            new_cash -= result.fill_price * config_.unit_size;
        }

        int new_round_number = row.current_round + 1;
        std::string verdict_str = verdict_to_string(result.verdict);

        db_.append_game_round(session_id, new_round_number, bid, ask, verdict_str,
                               has_fill ? std::optional<double>(result.fill_price) : std::nullopt);
        db_.update_game_session_state(session_id, new_round_number, new_position, new_cash, row.hints_revealed_csv);

        bool game_over = new_round_number >= row.max_rounds;
        if (game_over)
            db_.end_game_session(session_id);

        nlohmann::json round_result{
            {"type", "game_round_result"},
            {"session_id", session_id},
            {"round_number", new_round_number},
            {"quote", {{"bid", bid}, {"ask", ask}}},
            {"verdict", verdict_str},
            {"fill_price", has_fill ? nlohmann::json(result.fill_price) : nlohmann::json(nullptr)},
            {"position", new_position},
            {"cash", new_cash},
            {"rounds_remaining", row.max_rounds - new_round_number},
            {"status", game_over ? "ended" : "active"},
        };

        if (!game_over)
            return {round_result};

        Database::GameSessionRow updated_row = row;
        updated_row.current_round = new_round_number;
        updated_row.position = new_position;
        updated_row.cash = new_cash;
        updated_row.status = "ended";
        return {round_result, game_ended_json(updated_row, scenario, "max_rounds_reached")};
    }

    std::vector<nlohmann::json> request_hint(int64_t user_id, int64_t session_id)
    {
        auto row = require_active_session(session_id, user_id);
        const Scenario &scenario = find_scenario(row.scenario_id);

        std::vector<int> revealed = parse_hints_csv(row.hints_revealed_csv);
        int next_index = static_cast<int>(revealed.size());
        if (next_index >= static_cast<int>(scenario.hints.size()))
            throw GameError("No hints remaining for this scenario");

        double new_cash = row.cash - config_.hint_penalty;
        revealed.push_back(next_index);
        db_.update_game_session_state(session_id, row.current_round, row.position, new_cash, hints_to_csv(revealed));

        return {nlohmann::json{
            {"type", "game_hint_revealed"},
            {"session_id", session_id},
            {"hint_index", next_index},
            {"hint_text", scenario.hints[next_index]},
            {"cost", config_.hint_penalty},
            {"cash", new_cash},
            {"hints_remaining", static_cast<int>(scenario.hints.size()) - next_index - 1},
        }};
    }

    std::vector<nlohmann::json> end_session(int64_t user_id, int64_t session_id)
    {
        auto row = require_active_session(session_id, user_id);
        const Scenario &scenario = find_scenario(row.scenario_id);

        db_.end_game_session(session_id);
        row.status = "ended";
        return {game_ended_json(row, scenario, "player_closed")};
    }

private:
    Database &db_;
    std::vector<Scenario> scenarios_;
    GameConfig config_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> unit_dist_{0.0, 1.0};

    const Scenario &pick_scenario(const std::string &category)
    {
        std::vector<const Scenario *> pool;
        for (const auto &s : scenarios_)
            if (category.empty() || s.category == category)
                pool.push_back(&s);
        if (pool.empty())
            throw GameError("No scenarios available for category '" + category + "'");
        std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
        return *pool[pick(rng_)];
    }

    const Scenario &find_scenario(int scenario_id) const
    {
        for (const auto &s : scenarios_)
            if (s.id == scenario_id)
                return s;
        throw GameError("Unknown scenario id " + std::to_string(scenario_id));
    }

    Database::GameSessionRow require_active_session(int64_t session_id, int64_t user_id)
    {
        auto row = db_.get_game_session(session_id, user_id);
        if (!row.has_value())
            throw GameError("Session not found");
        if (row->status != "active")
            throw GameError("Session has already ended");
        return *row;
    }

    static std::string verdict_to_string(judge::Verdict v)
    {
        switch (v)
        {
        case judge::Verdict::BUY:
            return "BUY";
        case judge::Verdict::SELL:
            return "SELL";
        default:
            return "PASS";
        }
    }

    static std::vector<int> parse_hints_csv(const std::string &csv)
    {
        std::vector<int> result;
        std::stringstream ss(csv);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (!token.empty())
                result.push_back(std::stoi(token));
        }
        return result;
    }

    static std::string hints_to_csv(const std::vector<int> &hints)
    {
        std::string out;
        for (size_t i = 0; i < hints.size(); ++i)
        {
            if (i)
                out += ",";
            out += std::to_string(hints[i]);
        }
        return out;
    }

    nlohmann::json rounds_to_json(int64_t session_id)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &r : db_.get_game_rounds(session_id))
        {
            arr.push_back({
                {"round_number", r.round_number},
                {"bid", r.bid},
                {"ask", r.ask},
                {"verdict", r.verdict},
                {"fill_price", r.fill_price.has_value() ? nlohmann::json(*r.fill_price) : nlohmann::json(nullptr)},
            });
        }
        return arr;
    }

    nlohmann::json revealed_hints_to_json(const Database::GameSessionRow &row, const Scenario &scenario)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (int idx : parse_hints_csv(row.hints_revealed_csv))
            if (idx >= 0 && idx < static_cast<int>(scenario.hints.size()))
                arr.push_back(scenario.hints[idx]);
        return arr;
    }

    nlohmann::json session_started_json(const Database::GameSessionRow &row)
    {
        const Scenario &scenario = find_scenario(row.scenario_id);
        return {
            {"type", "game_started"},
            {"session_id", row.id},
            {"scenario", {{"id", scenario.id}, {"category", scenario.category}, {"prompt", scenario.prompt}, {"unit", scenario.unit}}},
            {"current_round", row.current_round},
            {"max_rounds", row.max_rounds},
            {"position", row.position},
            {"cash", row.cash},
            {"hints_revealed", revealed_hints_to_json(row, scenario)},
            {"status", row.status},
            {"round_history", rounds_to_json(row.id)},
        };
    }

    nlohmann::json game_ended_json(const Database::GameSessionRow &row, const Scenario &scenario, const std::string &reason)
    {
        double mark_to_model_pnl = row.cash + row.position * scenario.true_value;
        return {
            {"type", "game_ended"},
            {"session_id", row.id},
            {"true_value", scenario.true_value},
            {"final_position", row.position},
            {"final_cash", row.cash},
            {"mark_to_model_pnl", mark_to_model_pnl},
            {"reason", reason},
            {"rounds", rounds_to_json(row.id)},
        };
    }
};
