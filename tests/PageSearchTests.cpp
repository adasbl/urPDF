#include "CppUnitTest.h"
#include "../urPDF/PageLayoutGeometry.h"
#include "../urPDF/PageSearch.h"

#include <array>
#include <limits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        void AssertNear(double expected, double actual, wchar_t const* description = L"")
        {
            Assert::AreEqual(expected, actual, 1e-9, description);
        }

        urpdf::SearchMatch MakeMatch(int pageIndex, double x0, double y0, double x1, double y1)
        {
            return urpdf::SearchMatch{ pageIndex, { { x0, y0, x1, y1 } } };
        }

        urpdf::ViewportGeometry MakeViewport(
            double width,
            double height,
            double zoom = 1.0,
            double scrollableWidth = 10000.0,
            double scrollableHeight = 10000.0,
            double contentX = 0.0,
            double contentY = 0.0)
        {
            urpdf::ViewportGeometry viewport;
            viewport.width = width;
            viewport.height = height;
            viewport.zoom = zoom;
            viewport.scrollableWidth = scrollableWidth;
            viewport.scrollableHeight = scrollableHeight;
            viewport.contentX = contentX;
            viewport.contentY = contentY;
            return viewport;
        }
    }

    TEST_CLASS(PageSearchTests)
    {
    public:
        TEST_METHOD(ClipsSearchRectInUnnormalizedPageCoordinatesAndRejectsInvalid)
        {
            const urpdf::PdfPageBounds page{ 100.0, -20.0, 300.0, 400.0 };
            const auto inside = urpdf::TryClipSearchRect({ 110.0, 0.0, 200.0, 100.0 }, page);
            Assert::IsTrue(inside.has_value());
            AssertNear(110.0, inside->x0);
            AssertNear(0.0, inside->y0);
            AssertNear(200.0, inside->x1);
            AssertNear(100.0, inside->y1);

            const auto clipped = urpdf::TryClipSearchRect({ 50.0, -100.0, 220.0, 500.0 }, page);
            Assert::IsTrue(clipped.has_value());
            AssertNear(100.0, clipped->x0);
            AssertNear(-20.0, clipped->y0);
            AssertNear(220.0, clipped->x1);
            AssertNear(400.0, clipped->y1);

            const double nan = std::numeric_limits<double>::quiet_NaN();
            const double inf = std::numeric_limits<double>::infinity();
            for (auto rect : {
                urpdf::PdfPageBounds{ 300.0, 0.0, 350.0, 20.0 },
                urpdf::PdfPageBounds{ 10.0, -50.0, 99.0, -25.0 },
                urpdf::PdfPageBounds{ 120.0, 10.0, 120.0, 40.0 },
                urpdf::PdfPageBounds{ 200.0, 10.0, 150.0, 40.0 },
                urpdf::PdfPageBounds{ 1.0, 1.0, -1.0, -1.0 },
                urpdf::PdfPageBounds{ nan, 0.0, 150.0, 20.0 },
                urpdf::PdfPageBounds{ 110.0, 0.0, inf, 20.0 } })
            {
                Assert::IsFalse(urpdf::TryClipSearchRect(rect, page).has_value());
                Assert::IsFalse(urpdf::TryClipSearchRect(page, rect).has_value());
            }
        }

        TEST_METHOD(ComputesClippedUnionBoundsForMultiRectMatch)
        {
            const urpdf::PdfPageBounds page{ 0.0, 0.0, 200.0, 300.0 };
            const std::vector<urpdf::PdfPageBounds> rects{
                { 180.0, 40.0, 230.0, 52.0 },
                { -10.0, 56.0, 60.0, 68.0 },
                { 50.0, 50.0, 40.0, 60.0 } // invalid, ignored
            };

            const auto bounds = urpdf::TrySearchMatchBounds(rects, page);
            Assert::IsTrue(bounds.has_value());
            AssertNear(0.0, bounds->x0);
            AssertNear(40.0, bounds->y0);
            AssertNear(200.0, bounds->x1);
            AssertNear(68.0, bounds->y1);

            Assert::IsFalse(urpdf::TrySearchMatchBounds({}, page).has_value());
            const std::vector<urpdf::PdfPageBounds> outsideOnly{ { 210.0, 10.0, 250.0, 20.0 } };
            Assert::IsFalse(urpdf::TrySearchMatchBounds(outsideOnly, page).has_value());
        }

        TEST_METHOD(DetectsBlankSearchQueries)
        {
            Assert::IsTrue(urpdf::IsBlankSearchQuery(L""));
            Assert::IsTrue(urpdf::IsBlankSearchQuery(L"   \t\r\n  "));
            Assert::IsFalse(urpdf::IsBlankSearchQuery(L"a"));
            Assert::IsFalse(urpdf::IsBlankSearchQuery(L"  hello world  "));
        }

        TEST_METHOD(DocumentSearchStateLifecycleTracksRequestsAndPerPageRanges)
        {
            urpdf::DocumentSearchState search;
            Assert::IsTrue(search.State() == urpdf::SearchStatus::idle);
            Assert::AreEqual(std::wstring(L""), search.Query());
            Assert::AreEqual(std::size_t(0), search.MatchCount());
            Assert::IsFalse(search.ActiveMatchIndex().has_value());
            Assert::IsNull(search.ActiveMatch());

            const auto req1 = search.BeginSearch(L"needle");
            Assert::IsTrue(req1.has_value());
            Assert::IsTrue(search.State() == urpdf::SearchStatus::searching);
            Assert::AreEqual(std::wstring(L"needle"), search.Query());
            Assert::AreEqual(*req1, search.RequestId());

            Assert::IsTrue(search.AppendPageMatches(*req1, 0, {
                MakeMatch(0, 10, 10, 40, 22),
                MakeMatch(0, 60, 10, 90, 22)
            }));
            Assert::IsTrue(search.AppendPageMatches(*req1, 1, {}));
            Assert::IsTrue(search.AppendPageMatches(*req1, 3, {
                urpdf::SearchMatch{ 3, { { 120, 40, 180, 52 }, { 10, 54, 50, 66 } } }
            }));
            Assert::IsTrue(search.Complete(*req1, true, false));

            Assert::IsTrue(search.State() == urpdf::SearchStatus::ready);
            Assert::AreEqual(std::size_t(3), search.MatchCount());

            Assert::IsTrue(search.PageMatchesRange(0) == urpdf::PageMatchRange{ 0, 2 });
            Assert::IsTrue(search.PageMatchesRange(1).Empty());
            Assert::IsTrue(search.PageMatchesRange(2).Empty());
            Assert::IsTrue(search.PageMatchesRange(3) == urpdf::PageMatchRange{ 2, 3 });
            Assert::IsTrue(search.PageMatchesRange(99).Empty());
            Assert::IsTrue(search.PageMatchesRange(-1).Empty());

            const auto page0Matches = search.MatchesOnPage(0);
            Assert::AreEqual(std::size_t(2), page0Matches.size());
            AssertNear(10.0, page0Matches[0].rects[0].x0);
            AssertNear(60.0, page0Matches[1].rects[0].x0);

            const auto page3Matches = search.MatchesOnPage(3);
            Assert::AreEqual(std::size_t(1), page3Matches.size());
            Assert::AreEqual(std::size_t(2), page3Matches[0].rects.size());
        }

        TEST_METHOD(RejectsStaleRequestIdsAndFiltersInvalidMatchRects)
        {
            urpdf::DocumentSearchState search;
            const auto first = *search.BeginSearch(L"alpha");
            Assert::IsTrue(search.AppendPageMatches(first, 0, { MakeMatch(0, 10, 10, 20, 20) }));

            const auto second = *search.BeginSearch(L"beta");
            Assert::IsTrue(second != first);
            Assert::AreEqual(std::size_t(0), search.MatchCount());

            // Stale worker from "alpha" cannot mutate "beta".
            Assert::IsFalse(search.AppendPageMatches(first, 1, { MakeMatch(1, 10, 10, 20, 20) }));
            Assert::IsFalse(search.Complete(first, true, false));
            Assert::IsTrue(search.State() == urpdf::SearchStatus::searching);
            Assert::AreEqual(std::size_t(0), search.MatchCount());

            // Negative page index is rejected.
            Assert::IsFalse(search.AppendPageMatches(second, -1, { MakeMatch(-1, 10, 10, 20, 20) }));

            // Invalid/empty rects are stripped; matches with no valid rects are skipped.
            Assert::IsTrue(search.AppendPageMatches(second, 2, {
                urpdf::SearchMatch{ 99, { { 50, 50, 40, 60 }, { 10, 20, 30, 40 } } },
                urpdf::SearchMatch{ 2, {} },
                urpdf::SearchMatch{ 2, { { 10, 10, 10, 20 } } }
            }));
            Assert::IsTrue(search.Complete(second, true, false));
            Assert::AreEqual(std::size_t(1), search.MatchCount());
            Assert::AreEqual(2, search.Matches()[0].pageIndex);
            Assert::AreEqual(std::size_t(1), search.Matches()[0].rects.size());
            AssertNear(10.0, search.Matches()[0].rects[0].x0);

            // Duplicate or late completion after ready is rejected.
            Assert::IsFalse(search.AppendPageMatches(second, 3, { MakeMatch(3, 1, 1, 2, 2) }));
            Assert::IsFalse(search.Complete(second, true, false));
        }

        TEST_METHOD(OutOfOrderPageAppendMaintainsDocumentOrderAndActiveMatch)
        {
            urpdf::DocumentSearchState search;
            const auto req = *search.BeginSearch(L"order");
            Assert::IsTrue(search.AppendPageMatches(req, 4, { MakeMatch(4, 10, 10, 20, 20) }));
            Assert::IsTrue(search.SelectInitialMatch(4) == std::size_t(0));
            Assert::IsTrue(search.AppendPageMatches(req, 1, {
                MakeMatch(1, 10, 10, 20, 20),
                MakeMatch(1, 30, 10, 40, 20)
            }));
            Assert::IsTrue(search.Complete(req, true, false));

            Assert::AreEqual(std::size_t(3), search.MatchCount());
            Assert::AreEqual(1, search.Matches()[0].pageIndex);
            Assert::AreEqual(1, search.Matches()[1].pageIndex);
            Assert::AreEqual(4, search.Matches()[2].pageIndex);
            Assert::IsTrue(search.PageMatchesRange(1) == urpdf::PageMatchRange{ 0, 2 });
            Assert::IsTrue(search.PageMatchesRange(4) == urpdf::PageMatchRange{ 2, 3 });
            Assert::IsTrue(search.ActiveMatchIndex() == std::size_t(2));
            Assert::IsNotNull(search.ActiveMatch());
            Assert::AreEqual(4, search.ActiveMatch()->pageIndex);
        }

        TEST_METHOD(FailedCancelledBlankResetPendingAndClearTransitions)
        {
            urpdf::DocumentSearchState search;

            // Failure clears partial results and enters failed state.
            const auto reqFail = *search.BeginSearch(L"fail");
            Assert::IsTrue(search.AppendPageMatches(reqFail, 0, { MakeMatch(0, 0, 0, 10, 10) }));
            Assert::IsTrue(search.Complete(reqFail, false, true));
            Assert::IsTrue(search.State() == urpdf::SearchStatus::failed);
            Assert::AreEqual(std::size_t(0), search.MatchCount());

            // Cancelled worker completion returns to idle and clears partial results.
            const auto reqCancel = *search.BeginSearch(L"cancel");
            Assert::IsTrue(search.AppendPageMatches(reqCancel, 0, { MakeMatch(0, 0, 0, 10, 10) }));
            Assert::IsTrue(search.Complete(reqCancel, false, false));
            Assert::IsTrue(search.State() == urpdf::SearchStatus::idle);
            Assert::AreEqual(std::size_t(0), search.MatchCount());

            // ResetPending during searching invalidates in-flight request while keeping query.
            const auto reqPending = *search.BeginSearch(L"transfer");
            Assert::IsTrue(search.AppendPageMatches(reqPending, 0, { MakeMatch(0, 0, 0, 10, 10) }));
            search.ResetPending();
            Assert::IsTrue(search.State() == urpdf::SearchStatus::idle);
            Assert::AreEqual(std::wstring(L"transfer"), search.Query());
            Assert::AreEqual(std::size_t(0), search.MatchCount());
            Assert::IsFalse(search.Complete(reqPending, true, false));

            // ResetPending during ready preserves finished results across tab transfer.
            const auto reqReady = *search.BeginSearch(L"keep");
            Assert::IsTrue(search.AppendPageMatches(reqReady, 1, { MakeMatch(1, 0, 0, 10, 10) }));
            Assert::IsTrue(search.Complete(reqReady, true, false));
            Assert::IsTrue(search.SelectInitialMatch(0) == std::size_t(0));
            search.ResetPending();
            Assert::IsTrue(search.State() == urpdf::SearchStatus::ready);
            Assert::AreEqual(std::size_t(1), search.MatchCount());
            Assert::IsTrue(search.ActiveMatchIndex() == std::size_t(0));

            // Blank BeginSearch clears state and invalidates prior request.
            const auto reqBeforeBlank = *search.BeginSearch(L"before-blank");
            Assert::IsFalse(search.BeginSearch(L"   ").has_value());
            Assert::IsTrue(search.State() == urpdf::SearchStatus::idle);
            Assert::AreEqual(std::wstring(L""), search.Query());
            Assert::IsFalse(search.Complete(reqBeforeBlank, true, false));

            // Clear invalidates active search and empties everything.
            const auto reqClear = *search.BeginSearch(L"clear-me");
            search.Clear();
            Assert::IsTrue(search.State() == urpdf::SearchStatus::idle);
            Assert::AreEqual(std::wstring(L""), search.Query());
            Assert::AreEqual(std::size_t(0), search.MatchCount());
            Assert::IsFalse(search.Complete(reqClear, true, false));
        }

        TEST_METHOD(SelectsInitialMatchFromCurrentPageWithFallbackAndWrapsNavigation)
        {
            urpdf::DocumentSearchState search;
            Assert::IsFalse(search.SelectInitialMatch(0).has_value());
            Assert::IsFalse(search.NextMatch().has_value());
            Assert::IsFalse(search.PreviousMatch().has_value());

            const auto req = *search.BeginSearch(L"word");
            Assert::IsTrue(search.AppendPageMatches(req, 1, { MakeMatch(1, 10, 10, 20, 20) }));
            Assert::IsTrue(search.AppendPageMatches(req, 4, {
                MakeMatch(4, 10, 10, 20, 20),
                MakeMatch(4, 50, 10, 60, 20)
            }));
            Assert::IsTrue(search.AppendPageMatches(req, 7, { MakeMatch(7, 10, 10, 20, 20) }));
            Assert::IsTrue(search.Complete(req, true, false));

            // Before first match -> picks index 0 (page 1).
            Assert::IsTrue(search.SelectInitialMatch(0) == std::size_t(0));
            Assert::IsTrue(search.SelectInitialMatch(-2) == std::size_t(0));
            // Between page 1 and page 4 -> picks first match on page 4 (index 1).
            Assert::IsTrue(search.SelectInitialMatch(2) == std::size_t(1));
            // Exactly on page 4 -> picks first match on page 4 (index 1).
            Assert::IsTrue(search.SelectInitialMatch(4) == std::size_t(1));
            // Past last match (page 7) -> falls back to first match (index 0).
            Assert::IsTrue(search.SelectInitialMatch(8) == std::size_t(0));

            // NextMatch wraps around from last to first.
            Assert::IsTrue(search.NextMatch() == std::size_t(1));
            Assert::IsTrue(search.NextMatch() == std::size_t(2));
            Assert::IsTrue(search.NextMatch() == std::size_t(3));
            Assert::IsTrue(search.NextMatch() == std::size_t(0));

            // PreviousMatch wraps around from first to last.
            Assert::IsTrue(search.PreviousMatch() == std::size_t(3));
            Assert::IsTrue(search.PreviousMatch() == std::size_t(2));
            Assert::IsTrue(search.PreviousMatch() == std::size_t(1));
            Assert::IsTrue(search.PreviousMatch() == std::size_t(0));

            Assert::IsTrue(search.SetActiveMatch(2));
            Assert::IsTrue(search.ActiveMatchIndex() == std::size_t(2));
            Assert::IsFalse(search.SetActiveMatch(4));
            Assert::IsTrue(search.ActiveMatchIndex() == std::size_t(2));
        }

        TEST_METHOD(NavigationHandlesSingleMatchAndUnsetInitialIndex)
        {
            urpdf::DocumentSearchState search;
            const auto req = *search.BeginSearch(L"solo");
            Assert::IsTrue(search.AppendPageMatches(req, 2, { MakeMatch(2, 10, 10, 20, 20) }));
            Assert::IsTrue(search.Complete(req, true, false));

            // NextMatch / PreviousMatch when activeMatchIndex is unset.
            Assert::IsTrue(search.NextMatch() == std::size_t(0));
            Assert::IsTrue(search.NextMatch() == std::size_t(0));
            Assert::IsTrue(search.PreviousMatch() == std::size_t(0));

            // Fresh two-match search calling PreviousMatch before SelectInitialMatch starts at last.
            const auto req2 = *search.BeginSearch(L"pair");
            Assert::IsTrue(search.AppendPageMatches(req2, 0, {
                MakeMatch(0, 10, 10, 20, 20),
                MakeMatch(0, 30, 10, 40, 20)
            }));
            Assert::IsTrue(search.Complete(req2, true, false));
            Assert::IsTrue(search.PreviousMatch() == std::size_t(1));
        }

        TEST_METHOD(ScrollOffsetsCenterMatchWithZoomAndContentOffset)
        {
            const urpdf::PdfPageBounds pageBounds{ 0.0, 0.0, 600.0, 800.0 };
            const urpdf::PageRect pageRect{ 0.0, 810.0, 600.0, 800.0 };
            const urpdf::PdfPageBounds matchBounds{ 100.0, 200.0, 200.0, 220.0 };

            // Zoom 1.0, page fits horizontally (scrollableWidth == 0).
            const auto at1x = urpdf::CalculateSearchMatchScrollOffsets(
                matchBounds, pageBounds, pageRect,
                MakeViewport(600.0, 700.0, 1.0, 0.0, 2000.0, 0.0, 20.0), 0);
            Assert::IsTrue(at1x.has_value());
            AssertNear(0.0, at1x->x, L"clamped to zero horizontal range");
            // Match layout Y: [1010, 1030], + contentY 20 => [1030, 1050], center 1040, - 350 => 690.
            AssertNear(690.0, at1x->y, L"centered vertically at 1x");

            // Zoom 2.0, scrollable in both axes with contentX/contentY.
            const auto at2x = urpdf::CalculateSearchMatchScrollOffsets(
                matchBounds, pageBounds, pageRect,
                MakeViewport(500.0, 400.0, 2.0, 2000.0, 5000.0, 30.0, 20.0), 0);
            Assert::IsTrue(at2x.has_value());
            // Match layout center X: 150, + contentX 30 => 180, * 2 => 360, - 250 => 110.
            // Match layout center Y: 1020, + contentY 20 => 1040, * 2 => 2080, - 200 => 1880.
            AssertNear(110.0, at2x->x, L"centered horizontally at 2x");
            AssertNear(1880.0, at2x->y, L"centered vertically at 2x");
        }

        TEST_METHOD(ScrollOffsetsAccountForEveryQuarterTurnRotationAndNonZeroPageOrigin)
        {
            // Non-zero PDF page origin: width 100, height 200.
            // Match u in [0.1, 0.3] (center 0.2), v in [0.2, 0.4] (center 0.3) so all 4 turns are distinct.
            const urpdf::PdfPageBounds pageBounds{ 10.0, 20.0, 110.0, 220.0 };
            const urpdf::PdfPageBounds matchBounds{ 20.0, 60.0, 40.0, 100.0 };
            const auto viewport = MakeViewport(300.0, 300.0, 1.5, 5000.0, 5000.0);

            struct Case
            {
                int rotation;
                urpdf::PageRect pageRect;
                double expectedX;
                double expectedY;
            };

            const std::array<Case, 6> cases{{
                // 0°: portrait 400x800; ru=0.2 -> layoutX=180 -> scrollX=270 -> 120; rv=0.3 -> layoutY=340 -> scrollY=510 -> 360.
                { 0, { 100.0, 100.0, 400.0, 800.0 }, 120.0, 360.0 },
                // 90° CW: landscape 800x400; ru=1-v=0.7 -> layoutX=660 -> scrollX=990 -> 840; rv=u=0.2 -> layoutY=180 -> scrollY=270 -> 120.
                { 90, { 100.0, 100.0, 800.0, 400.0 }, 840.0, 120.0 },
                // 180°: portrait 400x800; ru=1-u=0.8 -> layoutX=420 -> scrollX=630 -> 480; rv=1-v=0.7 -> layoutY=660 -> scrollY=990 -> 840.
                { 180, { 100.0, 100.0, 400.0, 800.0 }, 480.0, 840.0 },
                // 270° CW: landscape 800x400; ru=v=0.3 -> layoutX=340 -> scrollX=510 -> 360; rv=1-u=0.8 -> layoutY=420 -> scrollY=630 -> 480.
                { 270, { 100.0, 100.0, 800.0, 400.0 }, 360.0, 480.0 },
                // Negative and wrapped rotations match 270° and 90°.
                { -90, { 100.0, 100.0, 800.0, 400.0 }, 360.0, 480.0 },
                { 450, { 100.0, 100.0, 800.0, 400.0 }, 840.0, 120.0 }
            }};

            for (auto const& testCase : cases)
            {
                const auto result = urpdf::CalculateSearchMatchScrollOffsets(
                    matchBounds, pageBounds, testCase.pageRect, viewport, testCase.rotation);
                Assert::IsTrue(result.has_value());
                AssertNear(testCase.expectedX, result->x);
                AssertNear(testCase.expectedY, result->y);
            }
        }

        TEST_METHOD(ScrollOffsetsHandleBookViewAndUnrealizedPagesAndMultiRectMatches)
        {
            // 6 pages in 2-column book view (3 rows of 2 pages).
            const std::vector<urpdf::PageSize> sizes(6, urpdf::PageSize{ 600.0, 800.0 });
            const auto layout = urpdf::CalculatePageLayoutGeometry(sizes, 2, 10.0, 0.0);
            const std::vector<urpdf::PdfPageBounds> pageBounds(6, urpdf::PdfPageBounds{ 0.0, 0.0, 600.0, 800.0 });

            // Page 5 (0-based) is the right-hand page on row 2: x=610, y=1620, w=600, h=800.
            const urpdf::SearchMatch match{ 5, {
                { 250.0, 380.0, 350.0, 400.0 },
                { 250.0, 400.0, 350.0, 420.0 }
            } };
            const auto viewport = MakeViewport(700.0, 500.0, 1.5, 2000.0, 5000.0, 10.0, 20.0);

            const auto result = urpdf::CalculateSearchMatchScrollOffsets(
                match, pageBounds, layout.pageRects, viewport, 0);
            Assert::IsTrue(result.has_value());
            // Match center on page 5: (300, 400) -> layout (910, 2020) -> +content (920, 2040) -> *1.5 (1380, 3060).
            // Subtract half viewport (350, 250) => (1030, 2810).
            AssertNear(1030.0, result->x, L"right-hand book page horizontal offset");
            AssertNear(2810.0, result->y, L"distant row vertical offset");

            // Left-hand page on the same row (page 4: x=0, y=1620).
            const urpdf::SearchMatch leftMatch{ 4, { { 50.0, 100.0, 150.0, 120.0 } } };
            const auto leftResult = urpdf::CalculateSearchMatchScrollOffsets(
                leftMatch, pageBounds, layout.pageRects, viewport, 0);
            Assert::IsTrue(leftResult.has_value());
            // Layout center (100, 1730) -> +content (110, 1750) -> *1.5 (165, 2625) -> - (350, 250) => (0 clamped, 2375).
            AssertNear(0.0, leftResult->x, L"left-hand book page clamped to 0");
            AssertNear(2375.0, leftResult->y, L"left-hand book page vertical offset");
        }

        TEST_METHOD(ScrollOffsetsAlignStartWhenMatchExceedsViewportAndClampToScrollRange)
        {
            const urpdf::PdfPageBounds pageBounds{ 0.0, 0.0, 600.0, 800.0 };
            const urpdf::PageRect pageRect{ 0.0, 0.0, 600.0, 800.0 };
            // Wide and tall match at 2x zoom: [200, 1000] x [200, 1000] (800x800 > 400x300 viewport).
            const urpdf::PdfPageBounds largeMatch{ 100.0, 100.0, 500.0, 500.0 };

            const auto aligned = urpdf::CalculateSearchMatchScrollOffsets(
                largeMatch, pageBounds, pageRect,
                MakeViewport(400.0, 300.0, 2.0, 800.0, 1300.0), 0);
            Assert::IsTrue(aligned.has_value());
            AssertNear(200.0, aligned->x, L"aligns match left edge when wider than viewport");
            AssertNear(200.0, aligned->y, L"aligns match top edge when taller than viewport");

            // Match near bottom-right clamps to scrollableWidth/scrollableHeight.
            const urpdf::PdfPageBounds bottomRight{ 550.0, 760.0, 620.0, 820.0 }; // clipped to page
            const auto clamped = urpdf::CalculateSearchMatchScrollOffsets(
                bottomRight, pageBounds, pageRect,
                MakeViewport(400.0, 300.0, 2.0, 800.0, 1300.0), 0);
            Assert::IsTrue(clamped.has_value());
            AssertNear(800.0, clamped->x, L"clamped to scrollableWidth");
            AssertNear(1300.0, clamped->y, L"clamped to scrollableHeight");
        }

        TEST_METHOD(ScrollOffsetsRejectInvalidGeometryAndRotations)
        {
            const urpdf::PdfPageBounds pageBounds{ 0.0, 0.0, 600.0, 800.0 };
            const urpdf::PageRect pageRect{ 0.0, 0.0, 600.0, 800.0 };
            const urpdf::PdfPageBounds matchBounds{ 10.0, 10.0, 50.0, 30.0 };
            const auto viewport = MakeViewport(600.0, 700.0);

            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(matchBounds, pageBounds, pageRect, viewport, 45).has_value());
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets({ 700.0, 10.0, 750.0, 30.0 }, pageBounds, pageRect, viewport, 0).has_value());
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(matchBounds, { 0.0, 0.0, 0.0, 800.0 }, pageRect, viewport, 0).has_value());
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(matchBounds, pageBounds, { 0.0, 0.0, 0.0, 800.0 }, viewport, 0).has_value());

            auto badViewport = viewport;
            badViewport.zoom = 0.0;
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(matchBounds, pageBounds, pageRect, badViewport, 0).has_value());
            badViewport = viewport;
            badViewport.scrollableWidth = -1.0;
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(matchBounds, pageBounds, pageRect, badViewport, 0).has_value());
            badViewport = viewport;
            badViewport.offsetY = std::numeric_limits<double>::quiet_NaN();
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(matchBounds, pageBounds, pageRect, badViewport, 0).has_value());

            const std::vector<urpdf::PdfPageBounds> boundsList{ pageBounds };
            const std::vector<urpdf::PageRect> rectsList{ pageRect };
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(
                urpdf::SearchMatch{ -1, { matchBounds } }, boundsList, rectsList, viewport, 0).has_value());
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(
                urpdf::SearchMatch{ 1, { matchBounds } }, boundsList, rectsList, viewport, 0).has_value());
            Assert::IsFalse(urpdf::CalculateSearchMatchScrollOffsets(
                urpdf::SearchMatch{ 0, {} }, boundsList, rectsList, viewport, 0).has_value());
        }
    };
}
