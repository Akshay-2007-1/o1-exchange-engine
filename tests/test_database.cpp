// Covers the wallet/settlement invariants behind several of the fixed
// github-issues bugs: reservation checks (#2 pool-exhaustion-funds-stolen),
// refund-on-cancel (#4 partial-fill-cancel-no-refund), and leaderboard
// ordering. Each test opens a fresh in-memory SQLite database
// (":memory:") so tests never share state and need no file cleanup.
#include <gtest/gtest.h>
#include <sodium.h>
#include "Database.h"

class DatabaseTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_EQ(sodium_init() >= 0, true);
    }
};

TEST_F(DatabaseTest, CreateUserAndLogin)
{
    Database db(":memory:");

    auto created = db.create_user("alice", "hunter2");
    ASSERT_TRUE(created.ok);
    EXPECT_GT(created.id, 0);

    auto dup = db.create_user("alice", "different");
    EXPECT_FALSE(dup.ok);

    auto bad_login = db.login("alice", "wrong-password");
    EXPECT_FALSE(bad_login.ok);

    auto good_login = db.login("alice", "hunter2");
    ASSERT_TRUE(good_login.ok);
    EXPECT_EQ(good_login.error.size(), 32u);

    auto session = db.validate_session(good_login.error);
    EXPECT_TRUE(session.ok);
    EXPECT_EQ(session.id, created.id);

    auto bad_session = db.validate_session("not-a-real-token");
    EXPECT_FALSE(bad_session.ok);
}

TEST_F(DatabaseTest, NewUserStartsWithDefaultCashAndNoShares)
{
    Database db(":memory:");
    auto user = db.create_user("bob", "pw");
    ASSERT_TRUE(user.ok);

    auto profile = db.get_user_profile(user.id);
    EXPECT_DOUBLE_EQ(profile.cash, 10000.0);
    EXPECT_TRUE(profile.portfolio.empty());
}

TEST_F(DatabaseTest, ReserveCashRejectsInsufficientFunds)
{
    Database db(":memory:");
    auto user = db.create_user("carol", "pw");
    ASSERT_TRUE(user.ok);

    auto over = db.reserve_cash(user.id, 20000.0);
    EXPECT_FALSE(over.ok);
    EXPECT_DOUBLE_EQ(db.get_user_profile(user.id).cash, 10000.0); // unchanged

    auto ok = db.reserve_cash(user.id, 5000.0);
    EXPECT_TRUE(ok.ok);
    EXPECT_DOUBLE_EQ(db.get_user_profile(user.id).cash, 5000.0);
}

TEST_F(DatabaseTest, ReserveSharesRejectsWhenUserHoldsNone)
{
    Database db(":memory:");
    auto user = db.create_user("dave", "pw");
    ASSERT_TRUE(user.ok);

    auto check = db.reserve_shares(user.id, /*company_id=*/1, /*quantity=*/10);
    EXPECT_FALSE(check.ok);
}

// Guards issue #4 (partial-fill-cancel-no-refund): cancelling/rejecting an
// order must return the reserved cash to the wallet it came from.
TEST_F(DatabaseTest, ReleaseCashRefundsExactAmountReserved)
{
    Database db(":memory:");
    auto user = db.create_user("erin", "pw");
    ASSERT_TRUE(user.ok);

    auto reserve = db.reserve_cash(user.id, 3000.0);
    ASSERT_TRUE(reserve.ok);
    EXPECT_DOUBLE_EQ(db.get_user_profile(user.id).cash, 7000.0);

    db.release_cash(user.id, 3000.0);
    EXPECT_DOUBLE_EQ(db.get_user_profile(user.id).cash, 10000.0);
}

TEST_F(DatabaseTest, ReleaseSharesRefundsExactQuantityReserved)
{
    Database db(":memory:");
    auto buyer = db.create_user("frank", "pw");
    auto seller = db.create_user("grace", "pw");
    ASSERT_TRUE(buyer.ok);
    ASSERT_TRUE(seller.ok);

    // Give frank 100 shares of company 1 via a settlement.
    db.settle_trade(buyer.id, seller.id, /*company_id=*/1, /*quantity=*/100, /*price=*/10000);
    auto profile = db.get_user_profile(buyer.id);
    ASSERT_EQ(profile.portfolio.size(), 1u);
    EXPECT_EQ(profile.portfolio[0].second, 100u);

    auto reserve = db.reserve_shares(buyer.id, 1, 40);
    ASSERT_TRUE(reserve.ok);
    EXPECT_EQ(db.get_user_profile(buyer.id).portfolio[0].second, 60u);

    db.release_shares(buyer.id, 1, 40);
    EXPECT_EQ(db.get_user_profile(buyer.id).portfolio[0].second, 100u);
}

TEST_F(DatabaseTest, SettleTradeCreditsBuyerSharesAndSellerCash)
{
    Database db(":memory:");
    auto buyer = db.create_user("heidi", "pw");
    auto seller = db.create_user("ivan", "pw");
    ASSERT_TRUE(buyer.ok);
    ASSERT_TRUE(seller.ok);

    db.settle_trade(buyer.id, seller.id, 1, /*quantity=*/50, /*price=*/10000); // $100.00/share

    auto buyer_profile = db.get_user_profile(buyer.id);
    ASSERT_EQ(buyer_profile.portfolio.size(), 1u);
    EXPECT_EQ(buyer_profile.portfolio[0].second, 50u);

    auto seller_profile = db.get_user_profile(seller.id);
    EXPECT_DOUBLE_EQ(seller_profile.cash, 10000.0 + 5000.0); // 50 * $100
}

TEST_F(DatabaseTest, SettleTradeRefundsBuyerOnPriceImprovement)
{
    Database db(":memory:");
    auto buyer = db.create_user("judy", "pw");
    auto seller = db.create_user("mallory", "pw");
    ASSERT_TRUE(buyer.ok);
    ASSERT_TRUE(seller.ok);

    // Buyer's limit was $110, but it filled at $100 — buyer should be
    // refunded the $10/share difference on top of the settlement.
    db.settle_trade(buyer.id, seller.id, 1, /*quantity=*/20, /*price=*/10000, /*buyer_limit_price=*/11000);

    auto buyer_profile = db.get_user_profile(buyer.id);
    EXPECT_DOUBLE_EQ(buyer_profile.cash, 10000.0 + 200.0); // 20 shares * $10 improvement
}

TEST_F(DatabaseTest, LeaderboardOrdersByCashDescending)
{
    Database db(":memory:");
    auto rich = db.create_user("rich", "pw");
    auto poor = db.create_user("poor", "pw");
    auto mid = db.create_user("mid", "pw");
    ASSERT_TRUE(rich.ok && poor.ok && mid.ok);

    db.reserve_cash(poor.id, 9000.0);  // poor: $1,000
    db.reserve_cash(mid.id, 4000.0);   // mid: $6,000
    // rich stays at $10,000

    auto board = db.get_leaderboard(10);
    ASSERT_GE(board.size(), 3u);

    // Find our three users' rank order within the board.
    auto rank_of = [&](int64_t id)
    {
        for (size_t i = 0; i < board.size(); ++i)
            if (board[i].user_id == id) return static_cast<int>(i);
        return -1;
    };

    int rich_rank = rank_of(rich.id);
    int mid_rank = rank_of(mid.id);
    int poor_rank = rank_of(poor.id);
    ASSERT_NE(rich_rank, -1);
    ASSERT_NE(mid_rank, -1);
    ASSERT_NE(poor_rank, -1);
    EXPECT_LT(rich_rank, mid_rank);
    EXPECT_LT(mid_rank, poor_rank);
}
