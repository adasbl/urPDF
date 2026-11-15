#include "CppUnitTest.h"
#include "../urPDF/PageLayoutGeometry.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        using urpdf::PageLayoutGeometry;
        using urpdf::PageSize;
        using urpdf::ViewportGeometry;

        constexpr double Spacing = 10.0;
        constexpr double IntrinsicWidth = std::numeric_limits<double>::infinity();

        void Near(double expected, double actual, wchar_t const* message)
        {
            Assert::AreEqual(expected, actual, 1e-5, message);
        }

        void Rect(urpdf::PageRect const& actual, double x, double y, double width, double height)
        {
            Near(x, actual.x, L"page x");
            Near(y, actual.y, L"page y");
            Near(width, actual.width, L"page width");
            Near(height, actual.height, L"page height");
        }

        PageLayoutGeometry Layout(std::vector<PageSize> const& sizes, int columns, double width = IntrinsicWidth)
        {
            return urpdf::CalculatePageLayoutGeometry(sizes, columns, Spacing, width);
        }

        std::vector<PageSize> MixedSizes()
        {
            return { {600, 600}, {600, 1800}, {800, 1500}, {1200, 600},
                {600, 900}, {900, 1500}, {700, 1200} };
        }

        ViewportGeometry ViewFor(PageLayoutGeometry const& layout, double zoom = 1.0,
            double width = 600.0, double height = 500.0, double contentX = 0.0)
        {
            ViewportGeometry view;
            view.zoom = zoom;
            view.width = width;
            view.height = height;
            view.contentX = contentX;
            view.contentY = 20.0;
            view.scrollableWidth = (std::max)(0.0, (layout.contentWidth + contentX) * zoom - width);
            // Same extent as PdfPageLayout and the repeater's top/bottom margins.
            auto const& last = layout.pageRects.back();
            const double extentHeight = static_cast<float>(last.y + last.height);
            view.scrollableHeight = (std::max)(0.0, (extentHeight + 40.0) * zoom - height);
            return view;
        }

        ViewportGeometry AtPagePoint(PageLayoutGeometry const& layout, ViewportGeometry view,
            int pageNumber, double x, double y)
        {
            auto const& page = layout.pageRects.at(static_cast<std::size_t>(pageNumber - 1));
            view.offsetX = (page.x + x * page.width + view.contentX) * view.zoom;
            view.offsetY = (page.y + y * page.height + view.contentY) * view.zoom;
            Assert::IsTrue(view.offsetX >= 0.0 && view.offsetX <= view.scrollableWidth, L"source horizontal position is reachable");
            Assert::IsTrue(view.offsetY >= 0.0 && view.offsetY <= view.scrollableHeight, L"source vertical position is reachable");
            return view;
        }

        urpdf::DocumentViewState Capture(PageLayoutGeometry const& layout, ViewportGeometry const& view,
            int pageNumber, double x, double y)
        {
            const auto state = urpdf::CaptureViewAnchor(layout.pageRects, view);
            Assert::IsTrue(state.has_value(), L"source geometry produces an anchor");
            Assert::AreEqual(pageNumber, state->anchorPage);
            Near(x, state->anchorX, L"captured point x");
            Near(y, state->anchorY, L"captured point y");
            Near(view.zoom, state->zoom, L"captured zoom");
            return *state;
        }

        void SameScreenPoint(PageLayoutGeometry const& source, PageLayoutGeometry const& target,
            ViewportGeometry const& before, ViewportGeometry after, int pageNumber, double x, double y)
        {
            const auto state = Capture(source, before, pageNumber, x, y);
            const auto offsets = urpdf::RestoreViewAnchor(state, target.pageRects, after);
            Assert::IsTrue(offsets.has_value(), L"target geometry restores the anchor");
            after.offsetX = offsets->x;
            after.offsetY = offsets->y;
            auto const& oldPage = source.pageRects.at(static_cast<std::size_t>(pageNumber - 1));
            auto const& newPage = target.pageRects.at(static_cast<std::size_t>(pageNumber - 1));
            Near((oldPage.x + x * oldPage.width + before.contentX) * before.zoom - before.offsetX,
                (newPage.x + x * newPage.width + after.contentX) * after.zoom - after.offsetX, L"same point stays at the same screen x");
            Near((oldPage.y + y * oldPage.height + before.contentY) * before.zoom - before.offsetY,
                (newPage.y + y * newPage.height + after.contentY) * after.zoom - after.offsetY, L"same point stays at the same screen y");
            Near(before.zoom, after.zoom, L"same zoom is used for both layouts");
        }
    }

    TEST_CLASS(PageLayoutGeometryTests)
    {
    public:
        TEST_METHOD(EmptyPagesHaveNoContentWidth)
        {
            const auto layout = Layout({}, 2, 1000.0);
            Assert::IsTrue(layout.pageRects.empty());
            Near(0.0, layout.contentWidth, L"empty layout width");
        }

        TEST_METHOD(SingleColumnCentersEachPageAndUsesIndividualHeights)
        {
            const auto layout = Layout({ {600, 800}, {300, 200}, {900, 400} }, 1, 1000.0);
            Near(1000.0, layout.contentWidth, L"available content width");
            Assert::AreEqual(std::size_t{3}, layout.pageRects.size());
            Rect(layout.pageRects[0], 200, 0, 600, 800);
            Rect(layout.pageRects[1], 350, 810, 300, 200);
            Rect(layout.pageRects[2], 50, 1020, 900, 400);
        }

        TEST_METHOD(BookRowsAlignTheirGutterAndTopEdges)
        {
            const auto layout = Layout({ {400, 800}, {600, 300}, {300, 500}, {200, 900} }, 2, 1200.0);
            Rect(layout.pageRects[0], 195, 0, 400, 800);
            Rect(layout.pageRects[1], 605, 0, 600, 300);
            Rect(layout.pageRects[2], 295, 810, 300, 500);
            Rect(layout.pageRects[3], 605, 810, 200, 900);
        }

        TEST_METHOD(OddLastPageIsCenteredAfterTallestPageOfPreviousRow)
        {
            const auto layout = Layout({ {600, 800}, {400, 1200}, {300, 500} }, 2, 1400.0);
            Assert::AreEqual(std::size_t{3}, layout.pageRects.size());
            Rect(layout.pageRects[2], 550, 1210, 300, 500);
        }

        TEST_METHOD(WidePairScalesBothPagesAndFollowingRowUsesScaledHeight)
        {
            const auto layout = Layout({ {600, 1200}, {600, 400}, {200, 1000}, {200, 200} }, 2, 610.0);
            Rect(layout.pageRects[0], 0, 0, 300, 600);
            Rect(layout.pageRects[1], 310, 0, 300, 200);
            Rect(layout.pageRects[2], 100, 610, 200, 1000);
            Rect(layout.pageRects[3], 310, 610, 200, 200);
        }

        TEST_METHOD(WideSinglePageScalesWithoutUpscalingSmallerPages)
        {
            const auto layout = Layout({ {1000, 600}, {300, 200} }, 1, 500.0);
            Rect(layout.pageRects[0], 0, 0, 500, 300);
            Rect(layout.pageRects[1], 100, 310, 300, 200);
        }

        TEST_METHOD(IntrinsicWidthKeepsExistingTrailingPageSpacing)
        {
            const std::vector<PageSize> sizes{ {600, 800}, {400, 400}, {1200, 200} };
            const auto single = Layout(sizes, 1);
            const auto book = Layout(sizes, 2);
            Near(1200.0, single.contentWidth, L"widest single page");
            Near(1210.0, book.contentWidth, L"unpaired page still contributes spacing to intrinsic width");
            Rect(book.pageRects[2], 5, 810, 1200, 200);
        }

        TEST_METHOD(NonpositiveAndNonfiniteWidthsUseIntrinsicPairWidth)
        {
            for (double width : { 0.0, -1.0, IntrinsicWidth, std::numeric_limits<double>::quiet_NaN() })
            {
                const auto layout = Layout({ {600, 800}, {400, 400} }, 2, width);
                Near(1010.0, layout.contentWidth, L"intrinsic pair width");
                Rect(layout.pageRects[0], -100, 0, 600, 800);
                Rect(layout.pageRects[1], 510, 0, 400, 400);
            }
        }

        TEST_METHOD(PageSizesColumnsAndSpacingHaveTheSameBoundsAsTheLayout)
        {
            const std::vector<PageSize> sizes{ {0, -5}, {10, 20} };
            const auto single = urpdf::CalculatePageLayoutGeometry(sizes, 0, IntrinsicWidth, 100.0);
            Rect(single.pageRects[0], 49.5, 0, 1, 1);
            Rect(single.pageRects[1], 45, 1, 10, 20);
            const auto book = urpdf::CalculatePageLayoutGeometry(sizes, 8, -10.0, 100.0);
            Rect(book.pageRects[0], 49, 0, 1, 1);
            Rect(book.pageRects[1], 50, 0, 10, 20);
        }

        TEST_METHOD(RectanglesKeepXamlFloatPrecision)
        {
            const auto layout = Layout({ {3, 1} }, 1, 1.0);
            Assert::AreEqual(static_cast<double>(1.0f / 3.0f), layout.pageRects[0].height);
        }
    };

    TEST_CLASS(ColumnLayoutViewStateTests)
    {
    public:
        TEST_METHOD(SingleToBookKeepsTheMiddleOfAMixedSizePage)
        {
            const auto single = Layout(MixedSizes(), 1);
            const auto book = Layout(MixedSizes(), 2);
            const auto before = AtPagePoint(single, ViewFor(single), 3, 0.25, 0.5);
            SameScreenPoint(single, book, before, ViewFor(book), 3, 0.25, 0.5);
        }

        TEST_METHOD(BookToSingleKeepsTheMiddleOfAMixedSizePage)
        {
            const auto book = Layout(MixedSizes(), 2);
            const auto single = Layout(MixedSizes(), 1);
            const auto before = AtPagePoint(book, ViewFor(book), 3, 0.25, 0.5);
            SameScreenPoint(book, single, before, ViewFor(single), 3, 0.25, 0.5);
        }

        TEST_METHOD(ScaledRowsPreserveAnchorWithZoomHorizontalScrollAndContentOffset)
        {
            for (int columns : {1, 2})
            {
                const auto source = Layout(MixedSizes(), columns, 1000.0);
                const auto target = Layout(MixedSizes(), 3 - columns, 900.0);
                const auto before = AtPagePoint(source, ViewFor(source, 1.75, 450, 350, 17), 3, 0.25, 0.5);
                Assert::IsTrue(before.offsetX > 0.0, L"source is horizontally scrolled");
                SameScreenPoint(source, target, before, ViewFor(target, 1.75, 450, 350, 41), 3, 0.25, 0.5);
            }
        }

        TEST_METHOD(RotatedMixedSizesPreserveAnchorInBothDirections)
        {
            auto sizes = MixedSizes();
            for (auto& size : sizes) { std::swap(size.width, size.height); }
            for (int columns : {1, 2})
            {
                const auto source = Layout(sizes, columns);
                const auto target = Layout(sizes, 3 - columns);
                const auto before = AtPagePoint(source, ViewFor(source, 1.25, 450, 350, 17), 3, 0.2, 0.5);
                SameScreenPoint(source, target, before, ViewFor(target, 1.25, 450, 350, 41), 3, 0.2, 0.5);
            }
        }

        TEST_METHOD(RightHandPagePreservesItsAnchorInBothDirections)
        {
            for (int columns : {1, 2})
            {
                const auto source = Layout(MixedSizes(), columns);
                const auto target = Layout(MixedSizes(), 3 - columns);
                const auto before = AtPagePoint(source, ViewFor(source, 1.0, 450, 350, 17), 4, 0.25, 0.5);
                SameScreenPoint(source, target, before, ViewFor(target, 1.0, 450, 350, 41), 4, 0.25, 0.5);
            }
        }

        TEST_METHOD(OddLastPagePreservesItsMiddleInBothDirections)
        {
            for (int columns : {1, 2})
            {
                const auto source = Layout(MixedSizes(), columns);
                const auto target = Layout(MixedSizes(), 3 - columns);
                const auto before = AtPagePoint(source, ViewFor(source, 1.25, 400, 250), 7, 0.25, 0.5);
                SameScreenPoint(source, target, before, ViewFor(target, 1.25, 400, 250), 7, 0.25, 0.5);
            }
        }

        TEST_METHOD(FirstPageMovesOnlyAsFarAsTheHorizontalStartAllows)
        {
            const std::vector<PageSize> sizes(4, {600, 800});
            const auto single = Layout(sizes, 1, 2000.0);
            const auto book = Layout(sizes, 2, 2000.0);
            const auto before = ViewFor(single, 1.0, 1000, 500);
            const auto state = Capture(single, before, 1, 0, 0);
            Near(0.7, state.viewportX, L"source page starts inside the viewport");
            const auto after = ViewFor(book, 1.0, 1000, 500);
            const auto offsets = urpdf::RestoreViewAnchor(state, book.pageRects, after).value();
            Near(0, offsets.x, L"cannot scroll left of document start");
            Near(0, offsets.y, L"page stays at the document top");
            Near(395, book.pageRects[0].x - offsets.x, L"gutter alignment determines the reachable screen position");
        }

        TEST_METHOD(LastPageMovesOnlyAsFarAsBothScrollEndsAllow)
        {
            const std::vector<PageSize> sizes(4, {600, 1200});
            for (int columns : {1, 2})
            {
                const auto source = Layout(sizes, columns, 2000.0);
                const auto target = Layout(sizes, 3 - columns, columns == 1 ? 610.0 : 300.0);
                const auto before = AtPagePoint(source, ViewFor(source, 1.0, 500, 500), 4, 0.5, 0.5);
                const auto state = Capture(source, before, 4, 0.5, 0.5);
                const auto after = ViewFor(target, 1.0, 500, 500);
                const auto offsets = urpdf::RestoreViewAnchor(state, target.pageRects, after).value();
                Near(columns == 1 ? 110.0 : 0.0, offsets.x, L"horizontal end");
                Near(columns == 1 ? 750.0 : 1970.0, offsets.y, L"vertical end");
                Near(after.scrollableWidth, offsets.x, L"entire horizontal range used");
                Near(after.scrollableHeight, offsets.y, L"entire vertical range used");
                auto const& page = target.pageRects.back();
                Assert::IsTrue((page.x + 0.5 * page.width) - offsets.x > 0, L"point can move no further left");
                Assert::IsTrue((page.y + 0.5 * page.height + after.contentY) - offsets.y > 0, L"point can move no further up");
            }
        }

        TEST_METHOD(SinglePageSwitchesWithZeroScrollRange)
        {
            for (int columns : {1, 2})
            {
                const auto source = Layout({ {200, 300} }, columns);
                const auto target = Layout({ {200, 300} }, 3 - columns);
                // At this zoom the unzoomed viewport is 1600 units wide;
                // the repeater centers its differently sized layout in it.
                const auto before = ViewFor(source, 0.75, 1200, 1000, (1600.0 - source.contentWidth) / 2.0);
                const auto after = ViewFor(target, 0.75, 1200, 1000, (1600.0 - target.contentWidth) / 2.0);
                Near(0, after.scrollableWidth, L"no horizontal scrolling");
                Near(0, after.scrollableHeight, L"no vertical scrolling");
                SameScreenPoint(source, target, before, after, 1, 0, 0);
            }
        }
    };
}
