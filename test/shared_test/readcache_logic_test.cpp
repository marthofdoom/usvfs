#include <gtest/gtest.h>

#include "../../src/usvfs_dll/readcache_logic.h"

using namespace usvfs::readcache::logic;

namespace
{

HandleAttrs goodAttrs()
{
  HandleAttrs a;
  a.hasEventOrApc   = false;
  a.queriesOk       = true;
  a.deviceType      = kDeviceTypeDisk;
  a.directory       = false;
  a.mode            = kModeSynchronousNonAlert;
  a.access          = kAccessReadData | 0x100000 /* SYNCHRONIZE */;
  a.endOfFile       = 4096;
  a.firstReadLength = 3100;
  return a;
}

}  // namespace

// ---- eligibility -----------------------------------------------------------

TEST(ReadCacheEligibility, GoodHandleIsEligible)
{
  EXPECT_EQ(Eligibility::Eligible, evaluateEligibility(goodAttrs(), Limits{}));
}

TEST(ReadCacheEligibility, SynchronousAlertAlsoCounts)
{
  auto a = goodAttrs();
  a.mode = kModeSynchronousAlert;
  EXPECT_EQ(Eligibility::Eligible, evaluateEligibility(a, Limits{}));
}

TEST(ReadCacheEligibility, Rejections)
{
  Limits l;
  {
    auto a = goodAttrs();
    a.hasEventOrApc = true;
    EXPECT_EQ(Eligibility::EventOrApc, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.queriesOk = false;
    EXPECT_EQ(Eligibility::QueryFailed, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.deviceType = 0x12;  // network file system
    EXPECT_EQ(Eligibility::NotDisk, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.directory = true;
    EXPECT_EQ(Eligibility::Directory, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.mode = 0;  // overlapped / asynchronous handle
    EXPECT_EQ(Eligibility::NotSynchronous, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.mode |= kModeNoIntermediateBuffering;
    EXPECT_EQ(Eligibility::NoBuffering, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.access = 0x100000;
    EXPECT_EQ(Eligibility::NotReadable, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.access |= kAccessWriteData;
    EXPECT_EQ(Eligibility::Writable, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.access |= kAccessAppendData;
    EXPECT_EQ(Eligibility::Writable, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.endOfFile = l.maxFileBytes + 1;
    EXPECT_EQ(Eligibility::TooLarge, evaluateEligibility(a, l));
  }
  {
    auto a = goodAttrs();
    a.firstReadLength = kSmallReadLimit;
    EXPECT_EQ(Eligibility::BigRead, evaluateEligibility(a, l));
  }
}

TEST(ReadCacheEligibility, FileLimitBoundaryAndBudget)
{
  Limits l;
  l.maxFileBytes = 1000;
  l.budgetBytes  = 500;

  auto a = goodAttrs();
  a.endOfFile = 500;
  EXPECT_EQ(Eligibility::Eligible, evaluateEligibility(a, l));
  a.endOfFile = 501;  // above the budget, even though below the file cap
  EXPECT_EQ(Eligibility::TooLarge, evaluateEligibility(a, l));

  l.budgetBytes = 5000;
  a.endOfFile = 1000;
  EXPECT_EQ(Eligibility::Eligible, evaluateEligibility(a, l));
  a.endOfFile = 1001;
  EXPECT_EQ(Eligibility::TooLarge, evaluateEligibility(a, l));
}

TEST(ReadCacheEligibility, SmallReadBoundary)
{
  auto a = goodAttrs();
  a.firstReadLength = kSmallReadLimit - 1;
  EXPECT_EQ(Eligibility::Eligible, evaluateEligibility(a, Limits{}));
}

// ---- read planning ---------------------------------------------------------

TEST(ReadCachePlan, SequentialReadsUseShadowPosition)
{
  ReadPlan p = planRead(1000, 0, false, 0, 100, false);
  EXPECT_EQ(ReadAction::Copy, p.action);
  EXPECT_EQ(0u, p.offset);
  EXPECT_EQ(100u, p.count);
  EXPECT_EQ(100u, p.newPos);

  p = planRead(1000, p.newPos, false, 0, 100, false);
  EXPECT_EQ(ReadAction::Copy, p.action);
  EXPECT_EQ(100u, p.offset);
  EXPECT_EQ(200u, p.newPos);
}

TEST(ReadCachePlan, UseFilePointerSentinel)
{
  ReadPlan p = planRead(1000, 300, true, kUseFilePointerPosition, 50, false);
  EXPECT_EQ(ReadAction::Copy, p.action);
  EXPECT_EQ(300u, p.offset);
  EXPECT_EQ(350u, p.newPos);
}

TEST(ReadCachePlan, ExplicitOffsetMovesShadowPosition)
{
  ReadPlan p = planRead(1000, 300, true, 10, 50, false);
  EXPECT_EQ(ReadAction::Copy, p.action);
  EXPECT_EQ(10u, p.offset);
  EXPECT_EQ(50u, p.count);
  EXPECT_EQ(60u, p.newPos);
}

TEST(ReadCachePlan, ClampsAtEndOfFile)
{
  ReadPlan p = planRead(1000, 0, true, 990, 50, false);
  EXPECT_EQ(ReadAction::Copy, p.action);
  EXPECT_EQ(10u, p.count);
  EXPECT_EQ(1000u, p.newPos);
}

TEST(ReadCachePlan, AtOrPastEndOfFile)
{
  ReadPlan p = planRead(1000, 1000, false, 0, 50, false);
  EXPECT_EQ(ReadAction::EndOfFile, p.action);
  EXPECT_EQ(1000u, p.newPos);

  p = planRead(1000, 0, true, 5000, 50, false);
  EXPECT_EQ(ReadAction::EndOfFile, p.action);
  EXPECT_EQ(0u, p.newPos);  // explicit EOF read leaves the position alone
}

TEST(ReadCachePlan, PassthroughCases)
{
  EXPECT_EQ(ReadAction::Passthrough, planRead(1000, 0, false, 0, 50, true).action);
  EXPECT_EQ(ReadAction::Passthrough, planRead(1000, 0, false, 0, 0, false).action);
  // FILE_WRITE_TO_END_OF_FILE (-1) and other negative offsets
  EXPECT_EQ(ReadAction::Passthrough, planRead(1000, 0, true, -1, 50, false).action);
  EXPECT_EQ(ReadAction::Passthrough, planRead(1000, 0, true, -3, 50, false).action);
}

TEST(ReadCachePlan, EmptyFile)
{
  EXPECT_EQ(ReadAction::EndOfFile, planRead(0, 0, false, 0, 10, false).action);
}

// ---- LRU eviction ----------------------------------------------------------

TEST(ReadCacheLru, NothingToDoWithinBudget)
{
  std::vector<EvictCandidate> c{{100, false}, {100, false}};
  EXPECT_TRUE(selectEvictions(c, 200, 200).empty());
  EXPECT_TRUE(selectEvictions(c, 200, 1000).empty());
}

TEST(ReadCacheLru, EvictsOldestFirstUntilWithinBudget)
{
  std::vector<EvictCandidate> c{{100, false}, {100, false}, {100, false}, {100, false}};
  auto r = selectEvictions(c, 400, 250);
  ASSERT_EQ(2u, r.size());
  EXPECT_EQ(0u, r[0]);
  EXPECT_EQ(1u, r[1]);
}

TEST(ReadCacheLru, SkipsPinnedEntries)
{
  std::vector<EvictCandidate> c{{100, true}, {100, false}, {100, true}, {100, false}};
  auto r = selectEvictions(c, 400, 150);
  ASSERT_EQ(2u, r.size());
  EXPECT_EQ(1u, r[0]);
  EXPECT_EQ(3u, r[1]);
}

TEST(ReadCacheLru, AllPinnedMayExceedBudget)
{
  std::vector<EvictCandidate> c{{100, true}, {100, true}};
  EXPECT_TRUE(selectEvictions(c, 200, 50).empty());
}

TEST(ReadCacheLru, StopsAsSoonAsBudgetIsMet)
{
  std::vector<EvictCandidate> c{{300, false}, {100, false}, {100, false}};
  auto r = selectEvictions(c, 500, 250);
  ASSERT_EQ(1u, r.size());
  EXPECT_EQ(0u, r[0]);
}

// ---- configuration ---------------------------------------------------------

TEST(ReadCacheConfig, ParseNumber)
{
  EXPECT_EQ(7u, parseNumber(nullptr, 7));
  EXPECT_EQ(7u, parseNumber("", 7));
  EXPECT_EQ(7u, parseNumber("abc", 7));
  EXPECT_EQ(7u, parseNumber("12x", 7));
  EXPECT_EQ(7u, parseNumber("-3", 7));
  EXPECT_EQ(0u, parseNumber("0", 7));
  EXPECT_EQ(1024u, parseNumber("1024", 7));
  EXPECT_EQ(7u, parseNumber("99999999999999999999999", 7));
}

TEST(ReadCacheConfig, ParseFlag)
{
  EXPECT_TRUE(parseFlag(nullptr, true));
  EXPECT_FALSE(parseFlag(nullptr, false));
  EXPECT_FALSE(parseFlag("0", true));
  EXPECT_FALSE(parseFlag("off", true));
  EXPECT_FALSE(parseFlag("FALSE", true));
  EXPECT_TRUE(parseFlag("1", false));
  EXPECT_TRUE(parseFlag("yes", false));
}
