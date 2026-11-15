#include <gtest/gtest.h>

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include "SourceCachePolicy.h"

using namespace VideoEditor;

TEST(SourceCachePolicy, EvictsLeastRecentlyUsedUnusedSources)
{
    const QHash<QString, quint64> lastUsed{
        {QStringLiteral("a"), 1},
        {QStringLiteral("b"), 2},
        {QStringLiteral("c"), 5},
        {QStringLiteral("d"), 3},
        {QStringLiteral("e"), 4},
    };
    const QSet<QString> used{QStringLiteral("c")};

    // 5 decoders, bound 3: drop the two LRU sources not on screen.
    const QStringList evicted = evictSourceKeys(lastUsed, used, 3);
    ASSERT_EQ(evicted.size(), 2);
    EXPECT_EQ(evicted.at(0), QStringLiteral("a"));
    EXPECT_EQ(evicted.at(1), QStringLiteral("b"));
}

TEST(SourceCachePolicy, NeverEvictsSourcesUsedByTheCurrentFrame)
{
    const QHash<QString, quint64> lastUsed{
        {QStringLiteral("a"), 1},
        {QStringLiteral("b"), 2},
        {QStringLiteral("c"), 3},
    };
    const QSet<QString> used{QStringLiteral("a"), QStringLiteral("b"),
                             QStringLiteral("c")};

    EXPECT_TRUE(evictSourceKeys(lastUsed, used, 1).isEmpty());
}

TEST(SourceCachePolicy, DoesNothingWhenWithinBudget)
{
    const QHash<QString, quint64> lastUsed{
        {QStringLiteral("a"), 1},
        {QStringLiteral("b"), 2},
    };
    EXPECT_TRUE(evictSourceKeys(lastUsed, {}, 4).isEmpty());
}

TEST(SourceCachePolicy, EvictsAllUnusedCandidatesWhenNeeded)
{
    const QHash<QString, quint64> lastUsed{
        {QStringLiteral("a"), 1},
        {QStringLiteral("b"), 2},
        {QStringLiteral("c"), 3},
    };
    const QSet<QString> used{QStringLiteral("c")};

    const QStringList evicted = evictSourceKeys(lastUsed, used, 0);
    ASSERT_EQ(evicted.size(), 2);
    EXPECT_EQ(evicted.at(0), QStringLiteral("a"));
    EXPECT_EQ(evicted.at(1), QStringLiteral("b"));
}
