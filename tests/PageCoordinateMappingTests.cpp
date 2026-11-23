#include "CppUnitTest.h"
#include "../urPDF/PageCoordinateMapping.h"

#include <array>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        urpdf::PageRasterGeometry RotatedPage(int quarterTurns)
        {
            const std::array<urpdf::PageRasterTransform, 4> transforms{{
                { 2, 0, 0, 2, 0, 0 }, { 0, 2, -2, 0, 0, 0 },
                { -2, 0, 0, -2, 0, 0 }, { 0, -2, 2, 0, 0, 0 } }};
            const std::array<urpdf::PixmapBounds, 4> bounds{{
                { 20, 40, 220, 440 }, { -440, 20, -40, 220 },
                { -220, -440, -20, -40 }, { 40, -220, 440, -20 } }};
            return { { 10, 20, 110, 220 }, transforms[quarterTurns], bounds[quarterTurns],
                quarterTurns % 2 ? urpdf::BitmapSize{ 400, 200 } : urpdf::BitmapSize{ 200, 400 } };
        }

        void EqualPoint(urpdf::ElementPoint expected, std::optional<urpdf::ElementPoint> actual)
        {
            Assert::IsTrue(actual.has_value());
            Assert::AreEqual(expected.x, actual->x, 1e-9);
            Assert::AreEqual(expected.y, actual->y, 1e-9);
        }

        void EqualPagePoint(urpdf::PdfPagePoint expected, std::optional<urpdf::PdfPagePoint> actual)
        {
            Assert::IsTrue(actual.has_value());
            Assert::AreEqual(expected.x, actual->x, 1e-9);
            Assert::AreEqual(expected.y, actual->y, 1e-9);
        }
    }

    TEST_CLASS(PageCoordinateMappingTests)
    {
    public:
        TEST_METHOD(MapsAllCornersAndCenterForEveryQuarterTurn)
        {
            const std::array<urpdf::PdfPagePoint, 5> points{{ { 10, 20 }, { 110, 20 },
                { 10, 220 }, { 110, 220 }, { 60, 120 } }};
            const std::array<std::array<urpdf::ElementPoint, 5>, 4> expected{{
                {{ { 100, 0 }, { 300, 0 }, { 100, 400 }, { 300, 400 }, { 200, 200 } }},
                {{ { 400, 100 }, { 400, 300 }, { 0, 100 }, { 0, 300 }, { 200, 200 } }},
                {{ { 300, 400 }, { 100, 400 }, { 300, 0 }, { 100, 0 }, { 200, 200 } }},
                {{ { 0, 300 }, { 0, 100 }, { 400, 300 }, { 400, 100 }, { 200, 200 } }} }};
            for (int rotation = 0; rotation < 4; ++rotation)
            {
                const auto mapping = urpdf::PageCoordinateMapping::TryCreate(RotatedPage(rotation), { 400, 400 });
                Assert::IsTrue(mapping.has_value());
                for (std::size_t i = 0; i < points.size(); ++i)
                {
                    EqualPoint(expected[rotation][i], mapping->PdfToElementPoint(points[i]));
                    EqualPagePoint(points[i], mapping->ElementToPdfPoint(expected[rotation][i]));
                    Assert::IsTrue(*mapping->IsElementPointInsidePage(expected[rotation][i]));
                }
            }
        }

        TEST_METHOD(MapsRectanglesAndTheirInverseForEveryQuarterTurn)
        {
            const urpdf::PdfPageBounds pageRect{ 20, 40, 50, 100 };
            const std::array<urpdf::ElementRect, 4> expected{{
                { 120, 40, 60, 120 }, { 240, 120, 120, 60 },
                { 220, 240, 60, 120 }, { 40, 220, 120, 60 } }};
            for (int rotation = 0; rotation < 4; ++rotation)
            {
                const auto mapping = urpdf::PageCoordinateMapping::TryCreate(RotatedPage(rotation), { 400, 400 });
                Assert::IsTrue(mapping.has_value());
                const auto result = mapping->PdfToElementRect(pageRect);
                Assert::IsTrue(result.has_value());
                Assert::AreEqual(expected[rotation].x, result->x, 1e-9);
                Assert::AreEqual(expected[rotation].y, result->y, 1e-9);
                Assert::AreEqual(expected[rotation].width, result->width, 1e-9);
                Assert::AreEqual(expected[rotation].height, result->height, 1e-9);
                const auto inverse = mapping->ElementToPdfRect(*result);
                Assert::IsTrue(inverse.has_value());
                Assert::AreEqual(pageRect.x0, inverse->x0, 1e-9);
                Assert::AreEqual(pageRect.y0, inverse->y0, 1e-9);
                Assert::AreEqual(pageRect.x1, inverse->x1, 1e-9);
                Assert::AreEqual(pageRect.y1, inverse->y1, 1e-9);
            }
        }

        TEST_METHOD(PreservesFractionalPageBoundsAndRoundedRasterFringe)
        {
            const urpdf::PageRasterGeometry raster{ { -10.25, 20.5, 89.75, 220.5 },
                { 2, 0, 0, 2, 0, 0 }, { -21, 41, 180, 441 }, { 201, 400 } };
            const auto mapping = urpdf::PageCoordinateMapping::TryCreate(raster, { 400, 400 });
            Assert::IsTrue(mapping.has_value());
            EqualPoint({ 100, 0 }, mapping->PdfToElementPoint({ -10.25, 20.5 }));
            EqualPoint({ 300, 400 }, mapping->PdfToElementPoint({ 89.75, 220.5 }));
            EqualPagePoint({ -10.375, 120.5 }, mapping->ElementToPdfPoint({ 99.75, 200 }));
            Assert::IsTrue(*urpdf::IsElementPointInsideBitmap({ 99.75, 200 }, mapping->BitmapRect()));
            Assert::IsFalse(*mapping->IsElementPointInsidePage({ 99.75, 200 }));
            Assert::IsFalse(*mapping->IsElementPointInsidePage({ 300.25, 200 }));
            Assert::IsTrue(*mapping->IsElementPointInsidePage({ 100, 0 }));
        }

        TEST_METHOD(ComposesRasterTranslationBeforePixmapOffsetAndUniform)
        {
            auto raster = RotatedPage(0);
            raster.transform.e = 13;
            raster.transform.f = -7;
            raster.pixmapBounds = { 33, 33, 233, 433 };
            const auto mapping = urpdf::PageCoordinateMapping::TryCreate(raster, { 400, 400 });
            Assert::IsTrue(mapping.has_value());
            EqualPoint({ 100, 0 }, mapping->PdfToElementPoint({ 10, 20 }));
            EqualPoint({ 200, 200 }, mapping->PdfToElementPoint({ 60, 120 }));
            EqualPagePoint({ 60, 120 }, mapping->ElementToPdfPoint({ 200, 200 }));
        }

        TEST_METHOD(IncludesPageEdgesWithFractionalScaleAndDisplaySize)
        {
            const urpdf::PageRasterGeometry raster{ { -10.3, 20.7, 89.7, 220.7 },
                { 1.3, 0, 0, 1.3, 0, 0 }, { -14, 26, 117, 287 }, { 131, 261 } };
            const auto mapping = urpdf::PageCoordinateMapping::TryCreate(raster, { 417.25, 503.75 });
            Assert::IsTrue(mapping.has_value());
            for (auto point : { urpdf::PdfPagePoint{ -10.3, 20.7 }, urpdf::PdfPagePoint{ 89.7, 20.7 },
                urpdf::PdfPagePoint{ -10.3, 220.7 }, urpdf::PdfPagePoint{ 89.7, 220.7 } })
            {
                const auto mapped = mapping->PdfToElementPoint(point);
                Assert::IsTrue(mapped.has_value());
                EqualPagePoint(point, mapping->ElementToPdfPoint(*mapped));
                Assert::IsTrue(*mapping->IsElementPointInsidePage(*mapped));
            }
        }

        TEST_METHOD(RectangleBoundsUseEveryCornerInBothDirections)
        {
            const urpdf::PageRasterGeometry raster{ { 0, 0, 10, 10 },
                { 1, 1, -1, 1, 0, 0 }, { -10, 0, 10, 20 }, { 20, 20 } };
            const auto mapping = urpdf::PageCoordinateMapping::TryCreate(raster, { 20, 20 });
            Assert::IsTrue(mapping.has_value());
            const auto rect = mapping->PdfToElementRect({ 2, 3, 5, 7 });
            Assert::IsTrue(rect.has_value());
            Assert::AreEqual(5.0, rect->x, 1e-9);
            Assert::AreEqual(5.0, rect->y, 1e-9);
            Assert::AreEqual(7.0, rect->width, 1e-9);
            Assert::AreEqual(7.0, rect->height, 1e-9);
            const auto inverse = mapping->ElementToPdfRect(*rect);
            Assert::IsTrue(inverse.has_value());
            Assert::AreEqual(0.0, inverse->x0, 1e-9);
            Assert::AreEqual(1.5, inverse->y0, 1e-9);
            Assert::AreEqual(7.0, inverse->x1, 1e-9);
            Assert::AreEqual(8.5, inverse->y1, 1e-9);
        }

        TEST_METHOD(RasterResolutionDoesNotMultiplyLocalDipsAgain)
        {
            for (int rasterScale : { 1, 2, 4 })
            {
                const urpdf::PageRasterGeometry raster{ { 0, 0, 100, 200 },
                    { static_cast<double>(rasterScale), 0, 0, static_cast<double>(rasterScale), 0, 0 },
                    { 0, 0, 100 * rasterScale, 200 * rasterScale }, { 100 * rasterScale, 200 * rasterScale } };
                const auto mapping = urpdf::PageCoordinateMapping::TryCreate(raster, { 300, 400 });
                Assert::IsTrue(mapping.has_value());
                EqualPoint({ 100, 100 }, mapping->PdfToElementPoint({ 25, 50 }));
                EqualPagePoint({ 25, 50 }, mapping->ElementToPdfPoint({ 100, 100 }));
                const auto resized = urpdf::PageCoordinateMapping::TryCreate(raster, { 600, 400 });
                Assert::IsTrue(resized.has_value());
                EqualPoint({ 250, 100 }, resized->PdfToElementPoint({ 25, 50 }));
            }
        }

        TEST_METHOD(OutsidePointsAndRectanglesAreNotClipped)
        {
            for (int rotation = 0; rotation < 4; ++rotation)
            {
                const auto mapping = urpdf::PageCoordinateMapping::TryCreate(RotatedPage(rotation), { 400, 400 });
                Assert::IsTrue(mapping.has_value());
                const urpdf::PdfPagePoint outside{ -15.125, 250.75 };
                const auto mapped = mapping->PdfToElementPoint(outside);
                Assert::IsTrue(mapped.has_value());
                EqualPagePoint(outside, mapping->ElementToPdfPoint(*mapped));
                Assert::IsFalse(*mapping->IsElementPointInsidePage(*mapped));
                Assert::IsTrue(mapping->PdfToElementRect({ -20, -20, 150, 250 }).has_value());
                Assert::IsTrue(mapping->ElementToPdfRect({ -100, -100, 600, 600 }).has_value());
            }
            const auto mapping = urpdf::PageCoordinateMapping::TryCreate(RotatedPage(0), { 400, 400 });
            Assert::IsTrue(mapping.has_value());
            Assert::IsFalse(*mapping->IsElementPointInsidePage({ 50, 200 }));
            EqualPagePoint({ -15, 120 }, mapping->ElementToPdfPoint({ 50, 200 }));
        }

        TEST_METHOD(RejectsInvalidGeometryPointsAndRectangles)
        {
            Assert::IsFalse(urpdf::PageCoordinateMapping::TryCreate({}, { 400, 400 }).has_value());
            auto raster = RotatedPage(0);
            raster.transform = { 1, 2, 2, 4, 0, 0 };
            Assert::IsFalse(urpdf::PageCoordinateMapping::TryCreate(raster, { 400, 400 }).has_value());
            for (double invalid : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity() })
            {
                Assert::IsFalse(urpdf::PageCoordinateMapping::TryCreate(RotatedPage(0), { invalid, 400 }).has_value());
            }
            const auto mapping = urpdf::PageCoordinateMapping::TryCreate(RotatedPage(0), { 400, 400 });
            Assert::IsTrue(mapping.has_value());
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            const auto inf = std::numeric_limits<double>::infinity();
            Assert::IsFalse(mapping->PdfToElementPoint({ nan, 0 }).has_value());
            Assert::IsFalse(mapping->ElementToPdfPoint({ 0, inf }).has_value());
            Assert::IsFalse(mapping->IsElementPointInsidePage({ inf, 0 }).has_value());
            Assert::IsFalse(mapping->PdfToElementRect({ 0, 0, nan, 100 }).has_value());
            Assert::IsFalse(mapping->PdfToElementRect({ 10, 10, 0, 0 }).has_value());
            Assert::IsFalse(mapping->ElementToPdfRect({ 0, 0, 0, 100 }).has_value());
            Assert::IsFalse(mapping->ElementToPdfRect({ 0, 0, inf, 100 }).has_value());
            const auto huge = (std::numeric_limits<double>::max)();
            Assert::IsFalse(mapping->PdfToElementPoint({ huge, huge }).has_value());
            Assert::IsFalse(mapping->ElementToPdfRect({ huge, 0, huge, 10 }).has_value());
            raster = RotatedPage(0);
            raster.transform.a = 1e-200;
            raster.transform.d = 1e-200;
            Assert::IsFalse(urpdf::PageCoordinateMapping::TryCreate(raster, { 1e-200, 1e-200 }).has_value());
        }
    };
}
