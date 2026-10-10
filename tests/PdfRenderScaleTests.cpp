#include "CppUnitTest.h"
#include "../urPDF/PdfRenderScale.h"

#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    using urpdf::PageRenderScale;

    void ExpectNear(double actual, double expected, const wchar_t* description)
    {
        Assert::IsTrue(std::isfinite(actual), description);
        Assert::AreEqual(expected, actual, 1e-9, description);
    }

    TEST_CLASS(PdfRenderScaleTests)
    {
    public:
        TEST_METHOD(NaturalSizeMatchesPhysicalPixels)
        {
            // A 6 x 9 inch PDF occupies 576 x 864 DIPs, plus the border.
            for (double dpi : { 1.0, 1.25, 1.5, 2.0, 4.0 })
            {
                for (double zoom : { 0.5, 1.0, 1.25, 3.0 })
                {
                    const double scale = PageRenderScale(432, 648, 0, 578, 866, zoom, dpi);
                    ExpectNear(432 * scale, 576 * zoom * dpi, L"Natural page pixel width");
                    ExpectNear(648 * scale, 864 * zoom * dpi, L"Natural page pixel height");
                }
            }
        }

        TEST_METHOD(BookPagesUseLayoutReduction)
        {
            const double first = PageRenderScale(432, 648, 0, 288, 432, 1, 2, 0);
            const double second = PageRenderScale(648, 864, 0, 432, 576, 1, 2, 0);
            ExpectNear(432 * first, 576, L"First book page");
            ExpectNear(648 * second, 864, L"Second book page");
        }

        TEST_METHOD(QuarterTurnsSwapDimensions)
        {
            for (int rotation : { 90, 270, -90, 450 })
            {
                const double scale = PageRenderScale(432, 648, rotation, 866, 578, 1.25, 1.5);
                ExpectNear(648 * scale, 1620, L"Rotated pixel width");
                ExpectNear(432 * scale, 1080, L"Rotated pixel height");
            }
        }

        TEST_METHOD(HalfTurnPreservesDimensions)
        {
            ExpectNear(PageRenderScale(432, 648, 180, 578, 866, 1, 1), 4.0 / 3.0, L"Half turn");
        }

        TEST_METHOD(FitsBorderAtZoomAndDpi)
        {
            ExpectNear(432 * PageRenderScale(432, 648, 0, 602, 902, 1.25, 1.5), 1125,
                L"600 DIP image at 125% zoom and 150% DPI");
        }

        TEST_METHOD(UniformStretchFitsHeight)
        {
            ExpectNear(648 * PageRenderScale(432, 648, 0, 602, 452, 1, 1), 450, L"Height limits image");
        }

        TEST_METHOD(RejectsEmptyPdfBounds)
        {
            ExpectNear(PageRenderScale(0, 648, 0, 578, 866, 1, 1), 0, L"Empty PDF width");
        }

        TEST_METHOD(RejectsUnmeasuredOrEmptyLayout)
        {
            ExpectNear(PageRenderScale(432, 648, 0, 0, 866, 1, 1), 0, L"Layout not measured");
            ExpectNear(PageRenderScale(432, 648, 0, 2, 2, 1, 1), 0, L"No room inside border");
        }

        TEST_METHOD(RejectsInvalidZoom)
        {
            const double invalid = (std::numeric_limits<double>::quiet_NaN)();
            ExpectNear(PageRenderScale(432, 648, 0, 578, 866, invalid, 1), 0, L"Invalid zoom");
        }

        TEST_METHOD(RejectsInvalidDpi)
        {
            const double invalid = (std::numeric_limits<double>::quiet_NaN)();
            ExpectNear(PageRenderScale(432, 648, 0, 578, 866, 1, 0), 0, L"Invalid DPI");
            ExpectNear(PageRenderScale(432, 648, 0, 578, 866, 1, invalid), 0, L"Non-finite DPI");
        }
    };
}
