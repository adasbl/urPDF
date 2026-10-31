#include "CppUnitTest.h"
#include "../urPDF/DocumentViewState.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    using urpdf::CaptureViewAnchor;
    using urpdf::CurrentPageNumber;
    using urpdf::DocumentViewState;
    using urpdf::PageRect;
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
}
