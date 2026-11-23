#include "CppUnitTest.h"
#include "../urPDF/PageCoordinateGeometry.h"

#include <limits>
#include <type_traits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        urpdf::PageRasterGeometry OffsetPage()
        {
            return { { -10.25, 20.5, 89.75, 220.5 },
                { 2.0, 0.0, 0.0, 2.0, 0.0, 0.0 },
                { -21, 41, 180, 441 }, { 201, 400 } };
        }

        bool Accepts(urpdf::PageRasterGeometry const& raster)
        {
            return urpdf::TryCreatePageCoordinateInputs(raster, { 400.0, 400.0 }).has_value();
        }
    }

    static_assert(!std::is_convertible_v<urpdf::PdfPagePoint, urpdf::BitmapPoint>);
    static_assert(!std::is_convertible_v<urpdf::BitmapPoint, urpdf::ElementPoint>);

    TEST_CLASS(PageCoordinateGeometryTests)
    {
    public:
        TEST_METHOD(PreservesOffsetPageAndRoundedPixmapOrigin)
        {
            const auto inputs = urpdf::TryCreatePageCoordinateInputs(OffsetPage(), { 400.0, 400.0 });
            Assert::IsTrue(inputs.has_value());
            Assert::AreEqual(-10.25, inputs->raster.pdfBounds.x0);
            Assert::AreEqual(20.5, inputs->raster.pdfBounds.y0);
            Assert::AreEqual(-21, inputs->raster.pixmapBounds.x0);
            Assert::AreEqual(41, inputs->raster.pixmapBounds.y0);
            Assert::AreEqual(2.0, inputs->raster.transform.a);
            Assert::AreEqual(201, inputs->raster.bitmapSize.width);
            Assert::AreEqual(99.5, inputs->bitmapRect.x);
            Assert::AreEqual(0.0, inputs->bitmapRect.y);
        }

        TEST_METHOD(AcceptsRotatedRasterWithNegativeOrigin)
        {
            auto raster = OffsetPage();
            raster.transform = { 0.0, 2.0, -2.0, 0.0, 0.0, 0.0 };
            raster.pixmapBounds = { -441, -21, -41, 180 };
            raster.bitmapSize = { 400, 201 };
            Assert::IsTrue(Accepts(raster));
        }

        TEST_METHOD(SubtractsAsymmetricBorderAndPaddingOnlyOnce)
        {
            const auto content = urpdf::TryPageContentSize({ 412.0, 624.0 },
                { 1.0, 2.0, 3.0, 4.0 }, { 3.0, 7.0, 5.0, 11.0 });
            Assert::IsTrue(content.has_value());
            Assert::AreEqual(400.0, content->width);
            Assert::AreEqual(600.0, content->height);
            const auto measured = urpdf::TryPageContentSize(*content, {});
            Assert::IsTrue(measured.has_value());
            Assert::AreEqual(content->width, measured->width);
            Assert::AreEqual(content->height, measured->height);
        }

        TEST_METHOD(RecognizesUniformMarginsAndIncludesBitmapEdges)
        {
            const auto vertical = urpdf::TryUniformBitmapRect({ 200, 400 }, { 400.0, 400.0 });
            Assert::IsTrue(vertical.has_value());
            Assert::AreEqual(100.0, vertical->x);
            Assert::IsFalse(*urpdf::IsElementPointInsideBitmap({ 99.0, 200.0 }, *vertical));
            Assert::IsFalse(*urpdf::IsElementPointInsideBitmap({ 301.0, 200.0 }, *vertical));
            Assert::IsTrue(*urpdf::IsElementPointInsideBitmap({ 100.0, 0.0 }, *vertical));
            Assert::IsTrue(*urpdf::IsElementPointInsideBitmap({ 300.0, 400.0 }, *vertical));

            const auto horizontal = urpdf::TryUniformBitmapRect({ 400, 200 }, { 400.0, 400.0 });
            Assert::IsTrue(horizontal.has_value());
            Assert::AreEqual(100.0, horizontal->y);
            Assert::IsFalse(*urpdf::IsElementPointInsideBitmap({ 200.0, 99.0 }, *horizontal));
            Assert::IsFalse(*urpdf::IsElementPointInsideBitmap({ 200.0, 301.0 }, *horizontal));
            Assert::IsFalse(*urpdf::IsElementPointInsideBitmap({ -1.0, 200.0 }, *horizontal));
            Assert::IsTrue(*urpdf::IsElementPointInsideBitmap({ 200.0, 200.0 }, *horizontal));
        }

        TEST_METHOD(UniformUsesBitmapPixelsAndMeasuredDips)
        {
            const auto rect = urpdf::TryUniformBitmapRect({ 400, 200 }, { 100.0, 100.0 });
            const auto higherResolution = urpdf::TryUniformBitmapRect({ 800, 400 }, { 100.0, 100.0 });
            Assert::IsTrue(rect.has_value() && higherResolution.has_value());
            Assert::AreEqual(0.0, rect->x);
            Assert::AreEqual(25.0, rect->y);
            Assert::AreEqual(100.0, rect->width);
            Assert::AreEqual(50.0, rect->height);
            Assert::AreEqual(rect->x, higherResolution->x);
            Assert::AreEqual(rect->y, higherResolution->y);
            Assert::AreEqual(rect->width, higherResolution->width);
            Assert::AreEqual(rect->height, higherResolution->height);
        }

        TEST_METHOD(RejectsEmptyAndNonfiniteDisplaySizes)
        {
            for (double value : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity() })
            {
                Assert::IsFalse(urpdf::TryCreatePageCoordinateInputs(OffsetPage(), { value, 400.0 }).has_value());
                Assert::IsFalse(urpdf::TryCreatePageCoordinateInputs(OffsetPage(), { 400.0, value }).has_value());
            }
            Assert::IsFalse(urpdf::TryUniformBitmapRect({ 0, 100 }, { 100.0, 100.0 }).has_value());
            Assert::IsFalse(urpdf::TryUniformBitmapRect({ 100, -1 }, { 100.0, 100.0 }).has_value());
            Assert::IsFalse(urpdf::TryUniformBitmapRect({ 1, 1 },
                { std::numeric_limits<double>::denorm_min(), 1.0 }).has_value());
        }

        TEST_METHOD(RejectsInvalidInsetsAndConsumedContent)
        {
            Assert::IsFalse(urpdf::TryPageContentSize({ 2.0, 100.0 }, { 1.0, 0.0, 1.0, 0.0 }).has_value());
            Assert::IsFalse(urpdf::TryPageContentSize({ 100.0, 2.0 }, {}, { 0.0, 1.0, 0.0, 1.0 }).has_value());
            for (double value : { -1.0, std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity() })
            {
                for (auto insets : { urpdf::ElementInsets{ value, 0.0, 0.0, 0.0 },
                    urpdf::ElementInsets{ 0.0, value, 0.0, 0.0 },
                    urpdf::ElementInsets{ 0.0, 0.0, value, 0.0 },
                    urpdf::ElementInsets{ 0.0, 0.0, 0.0, value } })
                {
                    Assert::IsFalse(urpdf::TryPageContentSize({ 100.0, 100.0 }, insets).has_value());
                    Assert::IsFalse(urpdf::TryPageContentSize({ 100.0, 100.0 }, {}, insets).has_value());
                }
            }
        }

        TEST_METHOD(RejectsEmptyReversedAndNonfinitePageBounds)
        {
            for (auto bounds : { urpdf::PdfPageBounds{ 1.0, 0.0, 1.0, 10.0 },
                urpdf::PdfPageBounds{ 1.0, 0.0, 0.0, 10.0 },
                urpdf::PdfPageBounds{ 0.0, 1.0, 10.0, 1.0 },
                urpdf::PdfPageBounds{ 0.0, 1.0, 10.0, 0.0 } })
            {
                auto raster = OffsetPage();
                raster.pdfBounds = bounds;
                Assert::IsFalse(Accepts(raster));
            }
            for (double value : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
            {
                for (auto member : { &urpdf::PdfPageBounds::x0, &urpdf::PdfPageBounds::y0,
                    &urpdf::PdfPageBounds::x1, &urpdf::PdfPageBounds::y1 })
                {
                    auto raster = OffsetPage();
                    raster.pdfBounds.*member = value;
                    Assert::IsFalse(Accepts(raster));
                }
            }
        }

        TEST_METHOD(RejectsMismatchedEmptyAndOverflowingPixmapSizes)
        {
            auto raster = OffsetPage();
            raster.bitmapSize.width = 200;
            Assert::IsFalse(Accepts(raster));
            raster = OffsetPage();
            raster.pixmapBounds.y1 = raster.pixmapBounds.y0;
            Assert::IsFalse(Accepts(raster));
            raster = OffsetPage();
            raster.pixmapBounds.x0 = (std::numeric_limits<std::int32_t>::min)();
            raster.pixmapBounds.x1 = (std::numeric_limits<std::int32_t>::max)();
            Assert::IsFalse(Accepts(raster));
            raster = OffsetPage();
            raster.bitmapSize.height = 0;
            Assert::IsFalse(Accepts(raster));
        }

        TEST_METHOD(RejectsSingularNonfiniteAndUnrepresentableInverse)
        {
            auto raster = OffsetPage();
            raster.transform = { 1.0, 2.0, 2.0, 4.0, 0.0, 0.0 };
            Assert::IsFalse(Accepts(raster));
            raster.transform = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            Assert::IsFalse(Accepts(raster));
            for (auto member : { &urpdf::PageRasterTransform::a, &urpdf::PageRasterTransform::b,
                &urpdf::PageRasterTransform::c, &urpdf::PageRasterTransform::d,
                &urpdf::PageRasterTransform::e, &urpdf::PageRasterTransform::f })
            {
                for (double value : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
                {
                    raster = OffsetPage();
                    raster.transform.*member = value;
                    Assert::IsFalse(Accepts(raster));
                }
            }
            raster.transform = { std::numeric_limits<double>::denorm_min(), 0.0,
                0.0, std::numeric_limits<double>::denorm_min(), 0.0, 0.0 };
            Assert::IsFalse(Accepts(raster));
        }

        TEST_METHOD(DoesNotTreatSmallInvertibleScaleAsSingular)
        {
            auto raster = OffsetPage();
            raster.transform = { 1e-100, 0.0, 0.0, 1e-100, 0.0, 0.0 };
            raster.pixmapBounds = { 0, 0, 1, 1 };
            raster.bitmapSize = { 1, 1 };
            Assert::IsTrue(Accepts(raster));
        }

        TEST_METHOD(DistinguishesInvalidHitTestFromOutsideBitmap)
        {
            const urpdf::ElementRect rect{ 10.0, 20.0, 100.0, 200.0 };
            Assert::IsFalse(*urpdf::IsElementPointInsideBitmap({ 9.0, 20.0 }, rect));
            Assert::IsFalse(urpdf::IsElementPointInsideBitmap(
                { std::numeric_limits<double>::quiet_NaN(), 20.0 }, rect).has_value());
            Assert::IsFalse(urpdf::IsElementPointInsideBitmap(
                { 10.0, std::numeric_limits<double>::infinity() }, rect).has_value());
            Assert::IsFalse(urpdf::IsElementPointInsideBitmap({ 10.0, 20.0 }, {}).has_value());
            const auto huge = (std::numeric_limits<double>::max)();
            Assert::IsFalse(urpdf::IsElementPointInsideBitmap({ 10.0, 20.0 }, { huge, 0.0, huge, 1.0 }).has_value());
        }
    };
}
