// End-to-end coverage of GameManager against a real (in-memory) Database,
// following the same TEST_F/:memory: convention as test_database.cpp.
#include <gtest/gtest.h>
#include <sodium.h>
#include "GameManager.h"

namespace
{
std::vector<Scenario> test_scenarios()
{
    return {
        Scenario{1, "test", "Test prompt A", "units", 100.0, {"hint0", "hint1"}},
        Scenario{2, "other", "Test prompt B", "units", 50.0, {"only-hint"}},
    };
}
} // namespace

class GameManagerTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_EQ(sodium_init() >= 0, true);
    }
};

TEST_F(GameManagerTest, StartSession_CreatesActiveSession)
{
    Database db(":memory:");
    auto user = db.create_user("alice", "pw");
    ASSERT_TRUE(user.ok);
    GameManager gm(db, test_scenarios());

    auto msgs = gm.start_session(user.id, "", false);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0]["type"], "game_started");
    EXPECT_EQ(msgs[0]["status"], "active");
    EXPECT_EQ(msgs[0]["current_round"], 0);
    EXPECT_TRUE(msgs[0]["round_history"].empty());
}

TEST_F(GameManagerTest, StartSession_ResumesExistingActiveSession)
{
    Database db(":memory:");
    auto user = db.create_user("bob", "pw");
    GameManager gm(db, test_scenarios());

    auto first = gm.start_session(user.id, "", false);
    int64_t sid1 = first[0]["session_id"].get<int64_t>();

    auto second = gm.start_session(user.id, "", false);
    int64_t sid2 = second[0]["session_id"].get<int64_t>();

    EXPECT_EQ(sid1, sid2);
}

TEST_F(GameManagerTest, StartSession_ForceNewEndsPriorSession)
{
    Database db(":memory:");
    auto user = db.create_user("carol", "pw");
    GameManager gm(db, test_scenarios());

    auto first = gm.start_session(user.id, "", false);
    int64_t sid1 = first[0]["session_id"].get<int64_t>();

    auto second = gm.start_session(user.id, "", true);
    int64_t sid2 = second[0]["session_id"].get<int64_t>();

    EXPECT_NE(sid1, sid2);
    auto row = db.get_game_session(sid1, user.id);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->status, "ended");
}

TEST_F(GameManagerTest, SubmitQuote_UpdatesPositionAndCashOnTrade)
{
    Database db(":memory:");
    auto user = db.create_user("dave", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(user.id, "test", false); // true_value = 100
    int64_t sid = started[0]["session_id"].get<int64_t>();

    // ask below true_value -> interviewer BUYs -> player sells.
    auto result = gm.submit_quote(user.id, sid, 90.0, 95.0);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0]["verdict"], "BUY");
    EXPECT_DOUBLE_EQ(result[0]["fill_price"].get<double>(), 95.0);
    EXPECT_DOUBLE_EQ(result[0]["position"].get<double>(), -1.0);
    EXPECT_DOUBLE_EQ(result[0]["cash"].get<double>(), 95.0);
}

TEST_F(GameManagerTest, SubmitQuote_NoChangeOnPass)
{
    Database db(":memory:");
    auto user = db.create_user("erin", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(user.id, "test", false);
    int64_t sid = started[0]["session_id"].get<int64_t>();

    auto result = gm.submit_quote(user.id, sid, 90.0, 110.0); // true_value=100 inside spread
    EXPECT_EQ(result[0]["verdict"], "PASS");
    EXPECT_TRUE(result[0]["fill_price"].is_null());
    EXPECT_DOUBLE_EQ(result[0]["position"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(result[0]["cash"].get<double>(), 0.0);
}

TEST_F(GameManagerTest, SubmitQuote_RejectsCrossedQuote)
{
    Database db(":memory:");
    auto user = db.create_user("frank", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(user.id, "test", false);
    int64_t sid = started[0]["session_id"].get<int64_t>();

    EXPECT_THROW(gm.submit_quote(user.id, sid, 110.0, 90.0), GameError);
}

TEST_F(GameManagerTest, SubmitQuote_RejectsUnownedSession)
{
    Database db(":memory:");
    auto owner = db.create_user("grace", "pw");
    auto intruder = db.create_user("heidi", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(owner.id, "test", false);
    int64_t sid = started[0]["session_id"].get<int64_t>();

    EXPECT_THROW(gm.submit_quote(intruder.id, sid, 90.0, 95.0), GameError);
}

TEST_F(GameManagerTest, SubmitQuote_EndsSessionAtMaxRounds)
{
    Database db(":memory:");
    auto user = db.create_user("ivan", "pw");
    GameConfig config;
    config.default_max_rounds = 2;
    GameManager gm(db, test_scenarios(), config);
    auto started = gm.start_session(user.id, "test", false);
    int64_t sid = started[0]["session_id"].get<int64_t>();

    auto r1 = gm.submit_quote(user.id, sid, 90.0, 110.0); // PASS, round 1 of 2
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_EQ(r1[0]["status"], "active");

    auto r2 = gm.submit_quote(user.id, sid, 90.0, 110.0); // PASS, round 2 of 2 -> ends
    ASSERT_EQ(r2.size(), 2u);
    EXPECT_EQ(r2[0]["status"], "ended");
    EXPECT_EQ(r2[1]["type"], "game_ended");
    EXPECT_EQ(r2[1]["reason"], "max_rounds_reached");

    EXPECT_THROW(gm.submit_quote(user.id, sid, 90.0, 110.0), GameError);
}

TEST_F(GameManagerTest, RequestHint_DeductsCashAndReturnsNextHint)
{
    Database db(":memory:");
    auto user = db.create_user("judy", "pw");
    GameConfig config;
    config.hint_penalty = 5.0;
    GameManager gm(db, test_scenarios(), config);
    auto started = gm.start_session(user.id, "test", false); // 2 hints available
    int64_t sid = started[0]["session_id"].get<int64_t>();

    auto hint1 = gm.request_hint(user.id, sid);
    ASSERT_EQ(hint1.size(), 1u);
    EXPECT_EQ(hint1[0]["hint_index"], 0);
    EXPECT_EQ(hint1[0]["hint_text"], "hint0");
    EXPECT_DOUBLE_EQ(hint1[0]["cash"].get<double>(), -5.0);
    EXPECT_EQ(hint1[0]["hints_remaining"], 1);

    auto hint2 = gm.request_hint(user.id, sid);
    EXPECT_EQ(hint2[0]["hint_index"], 1);
    EXPECT_EQ(hint2[0]["hint_text"], "hint1");
    EXPECT_DOUBLE_EQ(hint2[0]["cash"].get<double>(), -10.0);
    EXPECT_EQ(hint2[0]["hints_remaining"], 0);
}

TEST_F(GameManagerTest, RequestHint_ErrorsWhenAllHintsRevealed)
{
    Database db(":memory:");
    auto user = db.create_user("mallory", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(user.id, "other", false); // scenario 2, 1 hint only
    int64_t sid = started[0]["session_id"].get<int64_t>();

    gm.request_hint(user.id, sid); // reveals the only hint
    EXPECT_THROW(gm.request_hint(user.id, sid), GameError);
}

TEST_F(GameManagerTest, EndSession_ComputesMarkToModelPnlCorrectly)
{
    Database db(":memory:");
    auto user = db.create_user("niaj", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(user.id, "test", false); // true_value = 100
    int64_t sid = started[0]["session_id"].get<int64_t>();

    gm.submit_quote(user.id, sid, 90.0, 95.0); // interviewer BUY at 95 -> position=-1, cash=+95

    auto ended = gm.end_session(user.id, sid);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0]["type"], "game_ended");
    EXPECT_EQ(ended[0]["reason"], "player_closed");
    EXPECT_DOUBLE_EQ(ended[0]["true_value"].get<double>(), 100.0);
    // mark_to_model = cash + position * true_value = 95 + (-1 * 100) = -5
    EXPECT_DOUBLE_EQ(ended[0]["mark_to_model_pnl"].get<double>(), -5.0);
}

TEST_F(GameManagerTest, RoundsPersistAndAreReadableDirectlyFromDatabase)
{
    Database db(":memory:");
    auto user = db.create_user("olivia", "pw");
    GameManager gm(db, test_scenarios());
    auto started = gm.start_session(user.id, "test", false);
    int64_t sid = started[0]["session_id"].get<int64_t>();

    gm.submit_quote(user.id, sid, 90.0, 95.0);

    auto rounds = db.get_game_rounds(sid);
    ASSERT_EQ(rounds.size(), 1u);
    EXPECT_EQ(rounds[0].round_number, 1);
    EXPECT_EQ(rounds[0].verdict, "BUY");
    ASSERT_TRUE(rounds[0].fill_price.has_value());
    EXPECT_DOUBLE_EQ(*rounds[0].fill_price, 95.0);

    auto session_row = db.get_game_session(sid, user.id);
    ASSERT_TRUE(session_row.has_value());
    EXPECT_EQ(session_row->current_round, 1);
    EXPECT_DOUBLE_EQ(session_row->position, -1.0);
}
