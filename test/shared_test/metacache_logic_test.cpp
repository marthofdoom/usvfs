#include <gtest/gtest.h>

#include "../../src/usvfs_dll/metacache_logic.h"

using namespace usvfs::metacache::logic;

// ---- paths -----------------------------------------------------------------

TEST(MetaCachePath, NormalizeFoldsCaseAndSlashes)
{
  EXPECT_EQ(LR"(\??\C:\GAMES\DATA\A.ESP)", normalizePath(LR"(\??\c:\Games/Data\a.esp)"));
  EXPECT_EQ(LR"(\??\C:\DIR)", normalizePath(LR"(\??\C:\dir\)"));
  EXPECT_EQ(LR"(\??\C:\)", normalizePath(LR"(\??\C:\)"));  // root keeps its slash
}

TEST(MetaCachePath, ParentAndPrefix)
{
  EXPECT_EQ(LR"(\??\C:\A)", parentPath(LR"(\??\C:\A\B)"));
  EXPECT_EQ(L"", parentPath(L"NOSLASH"));

  EXPECT_TRUE(isUnderPrefix(LR"(\??\C:\OVERWRITE\X)", LR"(\??\C:\OVERWRITE)"));
  EXPECT_TRUE(isUnderPrefix(LR"(\??\C:\OVERWRITE)", LR"(\??\C:\OVERWRITE)"));
  EXPECT_FALSE(isUnderPrefix(LR"(\??\C:\OVERWRITE2\X)", LR"(\??\C:\OVERWRITE)"));
  EXPECT_FALSE(isUnderPrefix(LR"(\??\C:\A)", L""));
}

// ---- masks -----------------------------------------------------------------

TEST(MetaCacheMask, Normalization)
{
  EXPECT_EQ(L"*", normalizeMask(L""));
  EXPECT_EQ(L"*", normalizeMask(L"*"));
  EXPECT_EQ(L"*.*", normalizeMask(L"*.*"));  // not folded into "*"
  EXPECT_EQ(L"FOO.ESP", normalizeMask(L"foo.esp"));
}

TEST(MetaCacheMask, Classification)
{
  EXPECT_TRUE(isLiteralMask(L"FOO.ESP"));
  EXPECT_FALSE(isLiteralMask(L"*.ESP"));
  EXPECT_FALSE(isLiteralMask(L"A?C"));
  EXPECT_TRUE(isCacheableMask(L"*.esp"));
  EXPECT_FALSE(isCacheableMask(L"*<"));
  EXPECT_FALSE(isCacheableMask(L"\"x"));
}

TEST(MetaCacheMask, Matching)
{
  EXPECT_TRUE(maskMatches(L"*", L"anything"));
  EXPECT_TRUE(maskMatches(L"*.esp", L"Skyrim.ESP"));
  EXPECT_FALSE(maskMatches(L"*.esp", L"Skyrim.esm"));
  EXPECT_TRUE(maskMatches(L"sky*.es?", L"SkyrimSE.esm"));
  EXPECT_TRUE(maskMatches(L"a?c", L"abc"));
  EXPECT_FALSE(maskMatches(L"a?c", L"ac"));
  EXPECT_FALSE(maskMatches(L"a?c", L"abbc"));
  EXPECT_TRUE(maskMatches(L"*a*b*", L"xxaxxbxx"));
  EXPECT_FALSE(maskMatches(L"*a*b*", L"xxbxxaxx"));
  EXPECT_TRUE(maskMatches(L"foo.esp", L"FOO.ESP"));
  EXPECT_FALSE(maskMatches(L"foo.esp", L"foo.esp2"));
  EXPECT_TRUE(maskMatches(L"**", L""));
  EXPECT_FALSE(maskMatches(L"?", L""));
}

// ---- record packing --------------------------------------------------------

TEST(MetaCachePack, AlignedSize)
{
  EXPECT_EQ(0u, alignedSize(0));
  EXPECT_EQ(8u, alignedSize(1));
  EXPECT_EQ(8u, alignedSize(8));
  EXPECT_EQ(16u, alignedSize(9));
}

TEST(MetaCachePack, CountFitting)
{
  const std::vector<size_t> r{10, 20, 30};  // aligned: 16, 24, 32
  // offsets: 0, 16, 40; ends: 10, 36, 70
  EXPECT_EQ(0u, countFitting(r, 0, 9, false));    // not even one fits
  EXPECT_EQ(1u, countFitting(r, 0, 10, false));
  EXPECT_EQ(1u, countFitting(r, 0, 35, false));
  EXPECT_EQ(2u, countFitting(r, 0, 36, false));
  EXPECT_EQ(2u, countFitting(r, 0, 69, false));
  EXPECT_EQ(3u, countFitting(r, 0, 70, false));
  EXPECT_EQ(1u, countFitting(r, 0, 1000, true));  // single entry
  EXPECT_EQ(1u, countFitting(r, 2, 30, false));   // starting in the middle
  EXPECT_EQ(0u, countFitting(r, 3, 1000, false)); // nothing left
}

TEST(MetaCachePack, PackedSize)
{
  const std::vector<size_t> r{10, 20, 30};
  EXPECT_EQ(0u, packedSize(r, 0, 0));
  EXPECT_EQ(10u, packedSize(r, 0, 1));
  EXPECT_EQ(16u + 20u, packedSize(r, 0, 2));
  EXPECT_EQ(16u + 24u + 30u, packedSize(r, 0, 3));
  EXPECT_EQ(24u + 30u, packedSize(r, 1, 2));
}

// ---- store: attributes, invalidation, eviction -------------------------------

namespace
{
Attr existing(int64_t eof = 100, bool full = true)
{
  Attr a;
  a.status    = 0;
  a.full      = full;
  a.endOfFile = eof;
  return a;
}

Attr missing()
{
  Attr a;
  a.status = static_cast<int32_t>(0xC0000034);
  return a;
}
}  // namespace

TEST(MetaCacheStore, HitMissAndNegative)
{
  Store s(1 << 20);
  Attr out;
  EXPECT_FALSE(s.lookupAttr(L"A", true, out));

  s.putAttr(L"A", existing(42));
  ASSERT_TRUE(s.lookupAttr(L"A", true, out));
  EXPECT_EQ(42, out.endOfFile);

  s.putAttr(L"B", missing());
  ASSERT_TRUE(s.lookupAttr(L"B", true, out));
  EXPECT_LT(out.status, 0);

  const StoreStats st = s.stats();
  EXPECT_EQ(1u, st.attrHits);
  EXPECT_EQ(1u, st.attrNegativeHits);
  EXPECT_EQ(1u, st.attrMisses);
}

TEST(MetaCacheStore, BasicEntryDoesNotSatisfyFullQuery)
{
  Store s(1 << 20);
  s.putAttr(L"A", existing(0, false));
  Attr out;
  EXPECT_TRUE(s.lookupAttr(L"A", false, out));
  EXPECT_FALSE(s.lookupAttr(L"A", true, out));

  // a full entry then serves both and is not downgraded by a basic result
  s.putAttr(L"A", existing(7, true));
  EXPECT_TRUE(s.lookupAttr(L"A", true, out));
  s.putAttr(L"A", existing(0, false));
  ASSERT_TRUE(s.lookupAttr(L"A", true, out));
  EXPECT_EQ(7, out.endOfFile);
}

TEST(MetaCacheStore, InvalidateRemovesEntryAndParentListing)
{
  Store s(1 << 20);
  s.putAttr(L"\\D\\F", existing());
  auto l = std::make_shared<Listing>();
  l->add(std::string(8, 'x'), L"F");
  s.putListing(L"\\D", 37, L"*", l);
  s.putListing(L"\\OTHER", 37, L"*", l);

  s.invalidatePath(L"\\D\\F");

  Attr out;
  EXPECT_FALSE(s.lookupAttr(L"\\D\\F", true, out));
  EXPECT_EQ(nullptr, s.lookupListing(L"\\D", 37, L"*"));
  EXPECT_NE(nullptr, s.lookupListing(L"\\OTHER", 37, L"*"));
  EXPECT_EQ(1u, s.stats().invalidations);
}

TEST(MetaCacheStore, WrittenPathsAndTheirDirectoryAreNeverCachedAgain)
{
  Store s(1 << 20);
  s.invalidatePath(L"\\D\\F");
  EXPECT_TRUE(s.isVolatile(L"\\D\\F"));
  EXPECT_TRUE(s.isVolatile(L"\\D"));

  s.putAttr(L"\\D\\F", existing());
  Attr out;
  EXPECT_FALSE(s.lookupAttr(L"\\D\\F", true, out));

  auto l = std::make_shared<Listing>();
  s.putListing(L"\\D", 37, L"*", l);
  EXPECT_EQ(nullptr, s.lookupListing(L"\\D", 37, L"*"));

  // unrelated files in the same directory are still fine
  s.putAttr(L"\\D\\G", existing());
  EXPECT_TRUE(s.lookupAttr(L"\\D\\G", true, out));
}

TEST(MetaCacheStore, InvalidateTreeDropsEverythingBelow)
{
  Store s(1 << 20);
  s.putAttr(L"\\D\\SUB\\F", existing());
  s.putAttr(L"\\D\\SUB2\\F", existing());
  auto l = std::make_shared<Listing>();
  s.putListing(L"\\D\\SUB", 37, L"*", l);

  s.invalidateTree(L"\\D\\SUB");

  Attr out;
  EXPECT_FALSE(s.lookupAttr(L"\\D\\SUB\\F", true, out));
  EXPECT_EQ(nullptr, s.lookupListing(L"\\D\\SUB", 37, L"*"));
  s.putAttr(L"\\D\\SUB2\\F", existing());  // sibling with a similar prefix is untouched
  EXPECT_TRUE(s.lookupAttr(L"\\D\\SUB2\\F", true, out));
}

TEST(MetaCacheStore, NoCachePrefix)
{
  Store s(1 << 20);
  s.addNoCachePrefix(L"\\OVERWRITE");
  s.putAttr(L"\\OVERWRITE\\A\\B", existing());
  Attr out;
  EXPECT_FALSE(s.lookupAttr(L"\\OVERWRITE\\A\\B", true, out));
  auto l = std::make_shared<Listing>();
  s.putListing(L"\\OVERWRITE\\A", 37, L"*", l);
  EXPECT_EQ(nullptr, s.lookupListing(L"\\OVERWRITE\\A", 37, L"*"));
  s.putAttr(L"\\OVERWRITE2\\A", existing());
  EXPECT_TRUE(s.lookupAttr(L"\\OVERWRITE2\\A", true, out));
}

TEST(MetaCacheStore, LruEvictsLeastRecentlyUsedWithinCap)
{
  const std::wstring p1 = L"\\P\\1", p2 = L"\\P\\2", p3 = L"\\P\\3";
  // each entry costs path*2 + 160 bytes; room for two
  Store s(2 * (p1.size() * 2 + 160) + 10);
  s.putAttr(p1, existing());
  s.putAttr(p2, existing());
  Attr out;
  ASSERT_TRUE(s.lookupAttr(p1, true, out));  // p1 is now the most recent
  s.putAttr(p3, existing());                 // evicts p2

  EXPECT_TRUE(s.lookupAttr(p1, true, out));
  EXPECT_FALSE(s.lookupAttr(p2, true, out));
  EXPECT_TRUE(s.lookupAttr(p3, true, out));
  EXPECT_LE(s.bytes(), 2 * (p1.size() * 2 + 160) + 10);
  EXPECT_GE(s.stats().evictions, 1u);
}

TEST(MetaCacheStore, ListingsKeyedByClassAndMask)
{
  Store s(1 << 20);
  auto a = std::make_shared<Listing>();
  a->add(std::string(8, 'a'), L"A");
  s.putListing(L"\\D", 37, L"*", a);

  EXPECT_NE(nullptr, s.lookupListing(L"\\D", 37, L"*"));
  EXPECT_EQ(nullptr, s.lookupListing(L"\\D", 3, L"*"));
  EXPECT_EQ(nullptr, s.lookupListing(L"\\D", 37, L"*.ESP"));
  EXPECT_TRUE(s.hasListing(L"\\D", 37, L"*"));
  EXPECT_FALSE(s.hasListing(L"\\D", 3, L"*"));
}
