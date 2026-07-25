#include <gtest/gtest.h>
#include "MarketMakerJudge.h"

using judge::judge_quote;
using judge::Quote;
using judge::Verdict;

TEST(MarketMakerJudge, Buy_WhenAskBelowTrueValue)
{
    auto result = judge_quote(Quote{95.0, 99.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::BUY);
    EXPECT_DOUBLE_EQ(result.fill_price, 99.0);
}

TEST(MarketMakerJudge, Sell_WhenBidAboveTrueValue)
{
    auto result = judge_quote(Quote{101.0, 105.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::SELL);
    EXPECT_DOUBLE_EQ(result.fill_price, 101.0);
}

TEST(MarketMakerJudge, Pass_WhenTrueValueStrictlyInsideSpread)
{
    auto result = judge_quote(Quote{95.0, 105.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::PASS);
}

TEST(MarketMakerJudge, Pass_WhenTrueValueExactlyEqualsAsk)
{
    auto result = judge_quote(Quote{90.0, 100.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::PASS);
}

TEST(MarketMakerJudge, Pass_WhenTrueValueExactlyEqualsBid)
{
    auto result = judge_quote(Quote{100.0, 110.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::PASS);
}

TEST(MarketMakerJudge, DegenerateSpread_Buy_WhenPriceBelowTrueValue)
{
    auto result = judge_quote(Quote{90.0, 90.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::BUY);
    EXPECT_DOUBLE_EQ(result.fill_price, 90.0);
}

TEST(MarketMakerJudge, DegenerateSpread_Sell_WhenPriceAboveTrueValue)
{
    auto result = judge_quote(Quote{110.0, 110.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::SELL);
    EXPECT_DOUBLE_EQ(result.fill_price, 110.0);
}

TEST(MarketMakerJudge, DegenerateSpread_Pass_WhenPriceEqualsTrueValue)
{
    auto result = judge_quote(Quote{100.0, 100.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::PASS);
}

TEST(MarketMakerJudge, Invalid_WhenQuoteCrossed)
{
    auto result = judge_quote(Quote{105.0, 95.0}, 100.0);
    EXPECT_EQ(result.verdict, Verdict::INVALID);
}

TEST(MarketMakerJudge, NegativeTrueValue_BehavesLikeShiftedPositiveCase)
{
    // Same relative geometry as Sell_WhenBidAboveTrueValue, shifted by -200.
    auto result = judge_quote(Quote{-99.0, -95.0}, -100.0);
    EXPECT_EQ(result.verdict, Verdict::SELL);
    EXPECT_DOUBLE_EQ(result.fill_price, -99.0);
}

TEST(MarketMakerJudge, ZeroTrueValue_NoDivisionErrors)
{
    auto pass = judge_quote(Quote{-5.0, 5.0}, 0.0);
    EXPECT_EQ(pass.verdict, Verdict::PASS);

    auto buy = judge_quote(Quote{-5.0, -1.0}, 0.0);
    EXPECT_EQ(buy.verdict, Verdict::BUY);
    EXPECT_DOUBLE_EQ(buy.fill_price, -1.0);

    auto sell = judge_quote(Quote{1.0, 5.0}, 0.0);
    EXPECT_EQ(sell.verdict, Verdict::SELL);
    EXPECT_DOUBLE_EQ(sell.fill_price, 1.0);
}

TEST(MarketMakerJudge, NoiseBand_SameBoundaryQuote_DifferentDrawsProduceDifferentVerdicts)
{
    // true_value sits exactly on the ask, well inside a wide noise band.
    Quote q{90.0, 100.0};
    double true_value = 100.0;
    double epsilon = 2.0;

    auto passed = judge_quote(q, true_value, epsilon, /*noise_draw=*/0.1);
    auto traded = judge_quote(q, true_value, epsilon, /*noise_draw=*/0.9);

    EXPECT_EQ(passed.verdict, Verdict::PASS);
    EXPECT_EQ(traded.verdict, Verdict::BUY);
    EXPECT_DOUBLE_EQ(traded.fill_price, 100.0);
}

TEST(MarketMakerJudge, NoiseDisabled_ByDefault_BoundaryStillDeterministic)
{
    Quote q{90.0, 100.0};
    // noise_epsilon defaults to 0.0 - noise_draw is irrelevant and must not
    // leak into the outcome even when it would trigger a trade above.
    auto a = judge_quote(q, 100.0, 0.0, 0.9);
    auto b = judge_quote(q, 100.0, 0.0, 0.1);
    EXPECT_EQ(a.verdict, Verdict::PASS);
    EXPECT_EQ(b.verdict, Verdict::PASS);
}
