#include "CppUnitTest.h"
#include "../urPDF/DocumentViewState.h"

#include <vector>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    using urpdf::CalculatePageFitScrollOffsets;
    using urpdf::CalculatePageFitView;
    using urpdf::CaptureViewAnchor;
    using urpdf::CurrentPageNumber;
    using urpdf::DocumentViewState;
    using urpdf::PageFitMode;
    using urpdf::PageRect;
    using urpdf::RestoreViewAnchor;
    using urpdf::ViewportGeometry;

    namespace
    {
        constexpr double PageWidth = 600.0;
        constexpr double PageHeight = 800.0;
        constexpr double Spacing = 10.0;

        std::vector<PageRect> SingleColumn(int pageCount)
        {
            std::vector<PageRect> pages;
            for (int i = 0; i < pageCount; ++i)
            {
                pages.push_back({ 0.0, i * (PageHeight + Spacing), PageWidth, PageHeight });
            }
            return pages;
        }

        std::vector<PageRect> TwoColumns(int pageCount)
        {
            std::vector<PageRect> pages;
            for (int i = 0; i < pageCount; ++i)
            {
                const int row = i / 2;
                const int column = i % 2;
                pages.push_back({ column * (PageWidth + Spacing), row * (PageHeight + Spacing), PageWidth, PageHeight });
            }
            return pages;
        }

        ViewportGeometry Viewport(double offsetX, double offsetY, double zoom = 1.0, double width = 600.0, double height = 700.0)
        {
            ViewportGeometry viewport;
            viewport.offsetX = offsetX;
            viewport.offsetY = offsetY;
            viewport.width = width;
            viewport.height = height;
            viewport.zoom = zoom;
            return viewport;
        }

        void AssertNear(double expected, double actual, const wchar_t* description)
        {
            Assert::AreEqual(expected, actual, 1e-9, description);
        }

        DocumentViewState Capture(std::vector<PageRect> const& pages, ViewportGeometry const& viewport)
        {
            auto state = CaptureViewAnchor(pages, viewport);
            Assert::IsTrue(state.has_value(), L"Expected a captured state");
            return *state;
        }
    }

    TEST_CLASS(DocumentViewStateTests)
    {
    public:
        TEST_METHOD(AnchorsTopLeftCornerOnVisiblePage)
        {
            const auto state = Capture(SingleColumn(3), Viewport(0.0, 810.0 + 400.0));
            Assert::AreEqual(2, state.anchorPage);
            AssertNear(0.0, state.anchorX, L"anchorX");
            AssertNear(0.5, state.anchorY, L"anchorY");
            AssertNear(0.0, state.viewportX, L"viewportX");
            AssertNear(0.0, state.viewportY, L"viewportY");
        }

        TEST_METHOD(ConvertsZoomedOffsetsToLayoutCoordinates)
        {
            const auto state = Capture(SingleColumn(3), Viewport(300.0, 2.0 * (810.0 + 400.0), 2.0));
            Assert::AreEqual(2, state.anchorPage);
            AssertNear(0.25, state.anchorX, L"anchorX includes horizontal scroll");
            AssertNear(0.5, state.anchorY, L"anchorY");
            AssertNear(2.0, state.zoom, L"zoom");
        }

        TEST_METHOD(CornerInGapAnchorsToNextPageEdge)
        {
            const auto state = Capture(SingleColumn(3), Viewport(0.0, 805.0));
            Assert::AreEqual(2, state.anchorPage);
            AssertNear(0.0, state.anchorY, L"anchorY at top edge");
            AssertNear(5.0 / 700.0, state.viewportY, L"anchor below viewport top");
        }

        TEST_METHOD(UsesPageGeometryForMixedPageSizes)
        {
            const std::vector<PageRect> pages{
                { 0.0, 0.0, 600.0, 800.0 },
                { 0.0, 810.0, 600.0, 300.0 },
                { 0.0, 1120.0, 600.0, 800.0 },
            };
            const auto state = Capture(pages, Viewport(0.0, 1000.0));
            Assert::AreEqual(2, state.anchorPage);
            AssertNear(190.0 / 300.0, state.anchorY, L"anchorY");
        }

        TEST_METHOD(BookViewPicksLeftPageOfRow)
        {
            const auto state = Capture(TwoColumns(4), Viewport(0.0, 900.0, 1.0, 1210.0));
            Assert::AreEqual(3, state.anchorPage);
        }

        TEST_METHOD(BookViewPicksRightPageWhenScrolledToIt)
        {
            const auto state = Capture(TwoColumns(4), Viewport(700.0, 900.0));
            Assert::AreEqual(4, state.anchorPage);
            AssertNear(90.0 / 600.0, state.anchorX, L"anchorX");
            AssertNear(90.0 / 800.0, state.anchorY, L"anchorY");
        }

        TEST_METHOD(AccountsForCenteredPagesContainer)
        {
            auto viewport = Viewport(0.0, 0.0, 1.0, 800.0);
            viewport.contentX = 100.0;
            const auto state = Capture(SingleColumn(3), viewport);
            Assert::AreEqual(1, state.anchorPage);
            AssertNear(0.0, state.anchorX, L"anchorX");
            AssertNear(100.0 / 800.0, state.viewportX, L"page starts right of viewport edge");
        }

        TEST_METHOD(DocumentSmallerThanViewport)
        {
            const auto state = Capture(SingleColumn(1), Viewport(0.0, 0.0, 1.0, 1000.0, 1000.0));
            Assert::AreEqual(1, state.anchorPage);
            AssertNear(0.0, state.anchorY, L"anchorY");
        }

        TEST_METHOD(ViewportPastLastPageFallsBackToNearestPage)
        {
            const auto state = Capture(SingleColumn(3), Viewport(0.0, 5000.0));
            Assert::AreEqual(3, state.anchorPage);
            AssertNear(1.0, state.anchorY, L"anchorY at bottom edge");
        }

        TEST_METHOD(SkipsUnmeasuredPages)
        {
            auto pages = SingleColumn(3);
            pages[1].width = 0.0;
            const auto state = Capture(pages, Viewport(0.0, 810.0 + 400.0));
            Assert::AreNotEqual(2, state.anchorPage);
        }

        TEST_METHOD(RejectsMissingPagesOrInvalidViewport)
        {
            Assert::IsFalse(CaptureViewAnchor({}, Viewport(0.0, 0.0)).has_value(), L"no pages");
            Assert::IsFalse(CaptureViewAnchor(SingleColumn(2), Viewport(0.0, 0.0, 0.0)).has_value(), L"zero zoom");
            Assert::IsFalse(CaptureViewAnchor(SingleColumn(2), Viewport(0.0, 0.0, 1.0, 0.0)).has_value(), L"zero width");
            Assert::IsFalse(CaptureViewAnchor(SingleColumn(2), Viewport(0.0, 0.0, 1.0, 600.0, 0.0)).has_value(), L"zero height");
        }

        TEST_METHOD(CurrentPageIsPageWithLargestVisibleArea)
        {
            Assert::AreEqual(1, CurrentPageNumber(SingleColumn(3), Viewport(0.0, 400.0)).value(), L"previous page still dominates");
            Assert::AreEqual(2, CurrentPageNumber(SingleColumn(3), Viewport(0.0, 500.0)).value(), L"next page fills more of the viewport");
        }

        TEST_METHOD(CurrentPageInBookViewIsLeftPageOfRow)
        {
            Assert::AreEqual(1, CurrentPageNumber(TwoColumns(4), Viewport(0.0, 0.0, 1.0, 1210.0)).value());
        }

        TEST_METHOD(AnchorPageIsIndependentOfLargestVisiblePage)
        {
            const auto pages = SingleColumn(6);
            const auto viewport = Viewport(0.0, 3.0 * (PageHeight + Spacing) + 500.0);
            const auto state = Capture(pages, viewport);

            Assert::AreEqual(4, state.anchorPage, L"anchor remains on the tail of page 4");
            AssertNear(500.0 / PageHeight, state.anchorY, L"anchor belongs to page 4");
            AssertNear(0.0, state.viewportY, L"anchor is at the viewport top");
            Assert::AreEqual(5, CurrentPageNumber(pages, viewport).value(), L"page 5 has the largest visible area");
        }

        TEST_METHOD(CurrentPageWithEqualVisibleAreasIsEarlierPage)
        {
            const auto pages = SingleColumn(3);
            const auto viewport = Viewport(0.0, 455.0);

            Assert::AreEqual(1, CurrentPageNumber(pages, viewport).value(), L"both pages have 345 visible units of height");
        }

        TEST_METHOD(CurrentPageAtEndOfDocumentIsLastVisiblePage)
        {
            const std::vector<PageRect> pages{
                { 0.0, 0.0, 600.0, 800.0 },
                { 0.0, 810.0, 600.0, 800.0 },
                { 0.0, 1620.0, 600.0, 200.0 },
            };
            auto viewport = Viewport(0.0, 1120.0);
            Assert::AreEqual(2, CurrentPageNumber(pages, viewport).value(), L"end position unknown");

            viewport.scrollableHeight = 1120.0;
            Assert::AreEqual(3, CurrentPageNumber(pages, viewport).value(), L"scrolled to end");

            const auto state = Capture(pages, viewport);
            Assert::AreEqual(2, state.anchorPage, L"end-of-document counter rule does not change the anchor page");
            AssertNear(310.0 / PageHeight, state.anchorY, L"anchor still belongs to page 2");
        }

        TEST_METHOD(CurrentPageRejectsInvisibleOrInvalidViewport)
        {
            Assert::IsFalse(CurrentPageNumber(SingleColumn(3), Viewport(0.0, 5000.0)).has_value(), L"no visible page");
            Assert::IsFalse(CurrentPageNumber(SingleColumn(3), Viewport(0.0, 0.0, 0.0)).has_value(), L"zero zoom");
        }

        TEST_METHOD(RestoresCapturedAnchorInBothAxesAtAppliedZoom)
        {
            const auto pages = SingleColumn(5);
            auto viewport = Viewport(330.0, 2300.0, 2.0, 800.0, 700.0);
            viewport.contentX = 90.0;
            viewport.contentY = 20.0;
            viewport.scrollableWidth = 1000.0;
            viewport.scrollableHeight = 10000.0;
            const auto state = Capture(pages, viewport);

            viewport.offsetX = viewport.offsetY = 0.0;
            const auto restored = RestoreViewAnchor(state, pages, viewport).value();
            AssertNear(330.0, restored.x, L"horizontal offset round trip");
            AssertNear(2300.0, restored.y, L"vertical offset round trip");
        }

        TEST_METHOD(RestoresAnchorInResizedViewportWithCenteredContent)
        {
            DocumentViewState state;
            state.anchorPage = 3;
            state.anchorX = 0.6;
            state.anchorY = 0.4;
            state.viewportX = 0.25;
            state.viewportY = 0.5;
            auto pages = SingleColumn(4);
            for (auto& page : pages) { page.x = 200.0; }
            auto viewport = Viewport(0.0, 0.0, 1.5, 1000.0, 900.0);
            viewport.contentX = 10.0;
            viewport.contentY = 20.0;
            viewport.scrollableWidth = viewport.scrollableHeight = 10000.0;

            const auto restored = RestoreViewAnchor(state, pages, viewport).value();
            AssertNear(605.0, restored.x, L"anchor retains its relative horizontal viewport position");
            AssertNear(2490.0, restored.y, L"anchor retains its relative vertical viewport position");
        }

        TEST_METHOD(RestoresRightHandBookPageUsingItsActualRectangle)
        {
            DocumentViewState state;
            state.bookView = true;
            state.anchorPage = 4;
            state.anchorX = 0.5;
            state.anchorY = 0.25;
            state.viewportX = 0.5;
            auto viewport = Viewport(0.0, 0.0, 2.0, 600.0, 700.0);
            viewport.scrollableWidth = 2000.0;
            viewport.scrollableHeight = 3000.0;

            const auto restored = RestoreViewAnchor(state, TwoColumns(4), viewport).value();
            AssertNear(1520.0, restored.x, L"right-hand page, horizontal scroll and zoom");
            AssertNear(2020.0, restored.y, L"second row, quarter-page anchor");
        }

        TEST_METHOD(RestoresMixedPageSizesAfterRotation)
        {
            DocumentViewState state;
            state.rotation = 90;
            state.anchorPage = 2;
            state.anchorX = 0.5;
            state.anchorY = 0.25;
            const std::vector<PageRect> rotatedPages{
                { 0.0, 0.0, 800.0, 600.0 }, { 20.0, 610.0, 300.0, 900.0 }
            };
            auto viewport = Viewport(0.0, 0.0, 1.25, 500.0, 400.0);
            viewport.scrollableWidth = viewport.scrollableHeight = 10000.0;

            const auto restored = RestoreViewAnchor(state, rotatedPages, viewport).value();
            AssertNear(212.5, restored.x, L"uses rotated width without a second rotation");
            AssertNear(1043.75, restored.y, L"uses the actual height and position of page 2");
        }

        TEST_METHOD(ClampsAnchorPageToExistingFirstOrLastPage)
        {
            DocumentViewState state;
            state.anchorY = 0.5;
            auto viewport = Viewport(0.0, 0.0);
            viewport.scrollableHeight = 3000.0;
            state.anchorPage = 100;
            AssertNear(2020.0, RestoreViewAnchor(state, SingleColumn(3), viewport)->y, L"past last page");
            state.anchorPage = (std::numeric_limits<int>::min)();
            AssertNear(400.0, RestoreViewAnchor(state, SingleColumn(3), viewport)->y, L"before first page");
        }

        TEST_METHOD(ClampsBothOffsetsToAvailableScrollRange)
        {
            DocumentViewState state;
            state.anchorPage = 3;
            state.anchorX = state.anchorY = 1.0;
            auto viewport = Viewport(0.0, 0.0, 2.0);
            viewport.scrollableWidth = 300.0;
            viewport.scrollableHeight = 2000.0;
            auto restored = RestoreViewAnchor(state, SingleColumn(3), viewport).value();
            AssertNear(300.0, restored.x, L"horizontal end");
            AssertNear(2000.0, restored.y, L"vertical end");

            state.anchorPage = 1;
            state.anchorX = state.anchorY = 0.0;
            state.viewportX = state.viewportY = 1.0;
            restored = RestoreViewAnchor(state, SingleColumn(3), viewport).value();
            AssertNear(0.0, restored.x, L"horizontal start");
            AssertNear(0.0, restored.y, L"vertical start");
        }

        TEST_METHOD(RestoresSinglePageWithZeroScrollRange)
        {
            DocumentViewState state;
            state.anchorPage = 50;
            state.anchorX = state.anchorY = 0.8;
            const auto viewport = Viewport(0.0, 0.0, 1.0, 1000.0, 1000.0);
            const auto restored = RestoreViewAnchor(state, SingleColumn(1), viewport).value();
            AssertNear(0.0, restored.x, L"no horizontal range");
            AssertNear(0.0, restored.y, L"no vertical range");
        }

        TEST_METHOD(RestoredAnchorDoesNotForcePageCounterToAnchorPage)
        {
            const auto pages = SingleColumn(6);
            auto viewport = Viewport(0.0, 3.0 * (PageHeight + Spacing) + 500.0);
            viewport.scrollableHeight = 5000.0;
            const auto state = Capture(pages, viewport);
            viewport.height = 900.0;
            const auto restored = RestoreViewAnchor(state, pages, viewport).value();
            viewport.offsetX = restored.x;
            viewport.offsetY = restored.y;

            Assert::AreEqual(4, state.anchorPage);
            AssertNear(2930.0, restored.y, L"the saved point on page 4 stays at the top");
            Assert::AreEqual(5, CurrentPageNumber(pages, viewport).value(), L"page 5 dominates the resized viewport");
        }

        TEST_METHOD(RejectsEmptyDocumentAndUnmeasuredAnchorPage)
        {
            DocumentViewState state;
            const auto viewport = Viewport(0.0, 0.0);
            Assert::IsFalse(RestoreViewAnchor(state, {}, viewport).has_value());
            auto pages = SingleColumn(1);
            pages[0].height = 0.0;
            Assert::IsFalse(RestoreViewAnchor(state, pages, viewport).has_value());
        }

        TEST_METHOD(RejectsInvalidAnchorAndRestoreGeometry)
        {
            DocumentViewState state;
            const auto pages = SingleColumn(1);
            auto viewport = Viewport(0.0, 0.0);
            state.anchorX = std::numeric_limits<double>::quiet_NaN();
            Assert::IsFalse(RestoreViewAnchor(state, pages, viewport).has_value());
            state.anchorX = 0.0;
            state.viewportY = 1.1;
            Assert::IsFalse(RestoreViewAnchor(state, pages, viewport).has_value());
            state.viewportY = 0.0;
            viewport.zoom = 0.0;
            Assert::IsFalse(RestoreViewAnchor(state, pages, viewport).has_value());
            viewport.zoom = 1.0;
            viewport.scrollableWidth = -1.0;
            Assert::IsFalse(RestoreViewAnchor(state, pages, viewport).has_value());
        }
    };

    TEST_CLASS(ViewStateGuardTests)
    {
    public:
        TEST_METHOD(TransitionalGeometryCannotOverwriteEitherTab)
        {
            urpdf::ViewStateGuard guard;
            const auto first = guard.Begin(1);
            Assert::IsTrue(guard.Complete(first));
            Assert::IsTrue(guard.CanSave(1));
            Assert::IsFalse(guard.CanSave(2));

            const auto second = guard.Begin(2);
            Assert::IsFalse(guard.CanSave(1), L"outgoing geometry is protected");
            Assert::IsFalse(guard.CanSave(2), L"destination geometry is not ready");
            Assert::IsTrue(guard.Complete(second));
            Assert::IsFalse(guard.CanSave(1));
            Assert::IsTrue(guard.CanSave(2));
        }

        TEST_METHOD(RapidSwitchBackRejectsOldCompletionForSameDocument)
        {
            urpdf::ViewStateGuard guard;
            const auto firstA = guard.Begin(1);
            const auto firstB = guard.Begin(2);
            const auto secondA = guard.Begin(1);

            Assert::IsFalse(guard.IsCurrent(firstA), L"document identity alone is insufficient");
            Assert::IsFalse(guard.Complete(firstA));
            Assert::IsFalse(guard.Complete(firstB));
            Assert::IsTrue(guard.IsPending(), L"stale completion must retain the newer protection");
            Assert::IsFalse(guard.CanSave(1));
            Assert::IsTrue(guard.Complete(secondA));
            Assert::IsTrue(guard.CanSave(1));
        }

        TEST_METHOD(NewLayoutOnSameTabSupersedesPreviousLayout)
        {
            urpdf::ViewStateGuard guard;
            const auto rotation = guard.Begin(1);
            const auto bookView = guard.Begin(1);
            Assert::IsFalse(guard.IsCurrent(rotation));
            Assert::IsFalse(guard.Complete(rotation));
            Assert::IsTrue(guard.IsCurrent(bookView));
            Assert::IsTrue(guard.Complete(bookView));
            Assert::IsFalse(guard.Complete(bookView), L"duplicate completions are discarded");
        }

        TEST_METHOD(EmptyTabInvalidatesOutgoingOperations)
        {
            urpdf::ViewStateGuard guard;
            const auto document = guard.Begin(1);
            const auto empty = guard.Begin(0);
            Assert::IsFalse(guard.Complete(document));
            Assert::IsTrue(guard.Complete(empty));
            Assert::IsFalse(guard.CanSave(0));
            Assert::IsFalse(guard.CanSave(1));
        }

        TEST_METHOD(CancelledCallbacksCannotUnlockLaterOperation)
        {
            urpdf::ViewStateGuard guard;
            const auto closing = guard.Begin(1);
            guard.Cancel();
            Assert::IsFalse(guard.IsCurrent(closing));
            Assert::IsFalse(guard.CanSave(1));
            const auto opening = guard.Begin(2);
            Assert::IsFalse(guard.Complete(closing));
            Assert::IsTrue(guard.IsPending());
            Assert::IsTrue(guard.Complete(opening));
        }
    };

    TEST_CLASS(PageFitViewTests)
    {
    public:
        TEST_METHOD(SingleColumnFitsVerticallyAndAlignsCurrentPage)
        {
            const auto pages = SingleColumn(3);
            const auto fit = CalculatePageFitView(pages, 2, false, PageFitMode::Vertical, 1000.0, 1200.0, 0.5, 3.0);
            Assert::IsTrue(fit.has_value());
            AssertNear(1.5, fit->zoom, L"1200 / 800");

            auto viewport = Viewport(0.0, 0.0, fit->zoom, 1000.0, 1200.0);
            viewport.contentX = 50.0;
            viewport.contentY = 20.0;
            viewport.scrollableWidth = 1000.0;
            viewport.scrollableHeight = 5000.0;

            const auto offsets = CalculatePageFitScrollOffsets(pages, viewport, 2, false);
            Assert::IsTrue(offsets.has_value());
            AssertNear(25.0, offsets->x, L"centers page horizontally");
            AssertNear((810.0 + 20.0) * 1.5, offsets->y, L"aligns fitted page top to viewport top");
        }

        TEST_METHOD(SingleColumnFitsHorizontallyAndAlignsPageTopWhenTallerThanViewport)
        {
            const auto pages = SingleColumn(3);
            const auto fit = CalculatePageFitView(pages, 2, false, PageFitMode::Horizontal, 900.0, 700.0, 0.5, 3.0);
            Assert::IsTrue(fit.has_value());
            AssertNear(1.5, fit->zoom, L"900 / 600");

            auto viewport = Viewport(0.0, 0.0, fit->zoom, 900.0, 700.0);
            viewport.contentY = 20.0;
            viewport.scrollableWidth = 0.0;
            viewport.scrollableHeight = 5000.0;

            const auto offsets = CalculatePageFitScrollOffsets(pages, viewport, 2, false);
            Assert::IsTrue(offsets.has_value());
            AssertNear(0.0, offsets->x, L"page fills viewport width");
            AssertNear((810.0 + 20.0) * 1.5, offsets->y, L"taller fitted page aligns its top edge");
        }

        TEST_METHOD(BookViewFitVerticallyUsesTallerPageInRowForEitherPage)
        {
            const std::vector<PageRect> pages{
                { 0.0, 0.0, 600.0, 500.0 },
                { 610.0, 0.0, 600.0, 1000.0 },
                { 0.0, 1010.0, 600.0, 400.0 },
            };

            const auto leftFit = CalculatePageFitView(pages, 1, true, PageFitMode::Vertical, 1200.0, 800.0, 0.5, 3.0);
            const auto rightFit = CalculatePageFitView(pages, 2, true, PageFitMode::Vertical, 1200.0, 800.0, 0.5, 3.0);
            Assert::IsTrue(leftFit.has_value() && rightFit.has_value());
            AssertNear(0.8, leftFit->zoom, L"uses taller page (1000) when left page is active");
            AssertNear(0.8, rightFit->zoom, L"uses taller page (1000) when right page is active");
        }

        TEST_METHOD(BookViewFitHorizontallyIncludesBothPagesAndColumnSpacing)
        {
            const std::vector<PageRect> pages{
                { 100.0, 0.0, 500.0, 800.0 },
                { 610.0, 0.0, 700.0, 600.0 },
                { 400.0, 810.0, 600.0, 800.0 },
            };

            const auto pairFit = CalculatePageFitView(pages, 2, true, PageFitMode::Horizontal, 1210.0, 900.0, 0.5, 3.0);
            Assert::IsTrue(pairFit.has_value());
            AssertNear(1.0, pairFit->zoom, L"500 + 10 spacing + 700 = 1210");

            auto viewport = Viewport(0.0, 0.0, pairFit->zoom, 1210.0, 900.0);
            viewport.contentY = 20.0;
            viewport.scrollableWidth = 500.0;
            viewport.scrollableHeight = 2000.0;
            const auto offsets = CalculatePageFitScrollOffsets(pages, viewport, 2, true);
            Assert::IsTrue(offsets.has_value());
            AssertNear(100.0, offsets->x, L"left edge of page pair aligns to viewport left");
            AssertNear(0.0, offsets->y, L"row shorter than viewport centers vertically and clamps to 0");

            const auto oddLastFit = CalculatePageFitView(pages, 3, true, PageFitMode::Horizontal, 1200.0, 900.0, 0.5, 3.0);
            Assert::IsTrue(oddLastFit.has_value());
            AssertNear(2.0, oddLastFit->zoom, L"unpaired last page fits its own width");
        }

        TEST_METHOD(RotatedPageDimensionsDetermineFitZoom)
        {
            const std::vector<PageRect> upright{ { 0.0, 0.0, 600.0, 800.0 } };
            const std::vector<PageRect> rotated90{ { 0.0, 0.0, 800.0, 600.0 } };

            const auto uprightV = CalculatePageFitView(upright, 1, false, PageFitMode::Vertical, 1000.0, 1200.0, 0.5, 3.0);
            const auto rotatedV = CalculatePageFitView(rotated90, 1, false, PageFitMode::Vertical, 1000.0, 1200.0, 0.5, 3.0);
            const auto uprightH = CalculatePageFitView(upright, 1, false, PageFitMode::Horizontal, 1000.0, 1200.0, 0.5, 3.0);
            const auto rotatedH = CalculatePageFitView(rotated90, 1, false, PageFitMode::Horizontal, 1000.0, 1200.0, 0.5, 3.0);

            Assert::IsTrue(uprightV && rotatedV && uprightH && rotatedH);
            AssertNear(1.5, uprightV->zoom, L"upright vertical fit");
            AssertNear(2.0, rotatedV->zoom, L"rotated vertical fit uses swapped height");
            AssertNear(1000.0 / 600.0, uprightH->zoom, L"upright horizontal fit");
            AssertNear(1.25, rotatedH->zoom, L"rotated horizontal fit uses swapped width");
        }

        TEST_METHOD(ClampsFitZoomToScrollViewerBoundsAndCentersShortRow)
        {
            const std::vector<PageRect> pages{
                { 0.0, 0.0, 100.0, 100.0 },
                { 0.0, 110.0, 100.0, 100.0 },
                { 0.0, 220.0, 4000.0, 4000.0 },
            };

            const auto clampedMax = CalculatePageFitView(pages, 2, false, PageFitMode::Vertical, 1000.0, 900.0, 0.5, 3.0);
            Assert::IsTrue(clampedMax.has_value());
            AssertNear(3.0, clampedMax->zoom, L"clamped to max zoom");

            auto maxViewport = Viewport(0.0, 0.0, clampedMax->zoom, 1000.0, 900.0);
            maxViewport.contentY = 20.0;
            maxViewport.scrollableHeight = 20000.0;
            const auto maxOffsets = CalculatePageFitScrollOffsets(pages, maxViewport, 2, false);
            Assert::IsTrue(maxOffsets.has_value());
            AssertNear((110.0 + 50.0 + 20.0) * 3.0 - 450.0, maxOffsets->y, L"centers clamped short page vertically");

            const auto clampedMin = CalculatePageFitView(pages, 3, false, PageFitMode::Horizontal, 1000.0, 900.0, 0.5, 3.0);
            Assert::IsTrue(clampedMin.has_value());
            AssertNear(0.5, clampedMin->zoom, L"clamped to min zoom");
        }

        TEST_METHOD(RejectsInvalidFitInputs)
        {
            const auto pages = SingleColumn(1);
            Assert::IsFalse(CalculatePageFitView({}, 1, false, PageFitMode::Vertical, 800.0, 600.0, 0.5, 3.0).has_value());
            Assert::IsFalse(CalculatePageFitView(pages, 1, false, PageFitMode::Vertical, 0.0, 600.0, 0.5, 3.0).has_value());
            Assert::IsFalse(CalculatePageFitView(pages, 1, false, PageFitMode::Horizontal, 800.0, 0.0, 0.5, 3.0).has_value());
            Assert::IsFalse(CalculatePageFitView(pages, 1, false, PageFitMode::Vertical, 800.0, 600.0, 3.0, 0.5).has_value());
            Assert::IsFalse(CalculatePageFitView(pages, 1, false, PageFitMode::Vertical, 800.0, 600.0, 0.0, 3.0).has_value());

            auto invalidViewport = Viewport(0.0, 0.0, 0.0, 800.0, 600.0);
            Assert::IsFalse(CalculatePageFitScrollOffsets(pages, invalidViewport, 1, false).has_value());
            auto negativeScroll = Viewport(0.0, 0.0, 1.0, 800.0, 600.0);
            negativeScroll.scrollableHeight = -1.0;
            Assert::IsFalse(CalculatePageFitScrollOffsets(pages, negativeScroll, 1, false).has_value());
        }
    };
}
