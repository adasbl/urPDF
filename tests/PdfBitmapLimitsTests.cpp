#include "CppUnitTest.h"
#include "../urPDF/PdfRenderScale.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    bool FitsPage(double width, double height, int bucket)
    {
        const double scale = urpdf::RenderScaleForBucket(bucket);
        const double w = (std::max)(1.0, std::ceil(width * scale));
        const double h = (std::max)(1.0, std::ceil(height * scale));
        if (!std::isfinite(w) || !std::isfinite(h)
            || w > urpdf::MaxBitmapDimension || h > urpdf::MaxBitmapDimension) { return false; }
        if (w == 0 || h == 0) { return true; }
        std::size_t bytes = 0;
        return urpdf::TryBitmapBufferSize(static_cast<std::int64_t>(w), static_cast<std::int64_t>(h), bytes);
    }

    int LimitedBucket(double width, double height, double scale)
    {
        return urpdf::SelectLimitedRenderScaleBucket(urpdf::RenderScaleBucket(scale),
            [=](int bucket) { return FitsPage(width, height, bucket); });
    }

    TEST_CLASS(PdfBitmapLimitsTests)
    {
    public:
        TEST_METHOD(AcceptsExactDimensionAndPixelLimits)
        {
            std::size_t bytes = 0;
            Assert::IsTrue(urpdf::TryBitmapBufferSize(8192, 2048, bytes));
            Assert::IsTrue(bytes == 64ull * 1024 * 1024);
            Assert::IsTrue(urpdf::TryBitmapBufferSize(4096, 4096, bytes));
            Assert::IsTrue(bytes == 64ull * 1024 * 1024);
        }

        TEST_METHOD(RejectsOversizedDimensionsAndPixelCounts)
        {
            std::size_t bytes = 1;
            Assert::IsFalse(urpdf::TryBitmapBufferSize(8193, 1, bytes));
            Assert::IsTrue(bytes == 0);
            Assert::IsFalse(urpdf::TryBitmapBufferSize(1, 8193, bytes));
            Assert::IsFalse(urpdf::TryBitmapBufferSize(4096, 4097, bytes));
            Assert::IsFalse(urpdf::TryBitmapBufferSize(8192, 8192, bytes));
        }

        TEST_METHOD(RejectsEmptyNegativeAndOverflowingSizes)
        {
            std::size_t bytes = 1;
            Assert::IsFalse(urpdf::TryBitmapBufferSize(0, 100, bytes));
            Assert::IsFalse(urpdf::TryBitmapBufferSize(100, -1, bytes));
            Assert::IsFalse(urpdf::TryBitmapBufferSize(-1, -1, bytes));
            const auto huge = (std::numeric_limits<std::int64_t>::max)();
            Assert::IsFalse(urpdf::TryBitmapBufferSize(huge, huge, bytes));
            Assert::IsTrue(bytes == 0);
        }

        TEST_METHOD(KeepsRequestedQualityForNormalPages)
        {
            Assert::AreEqual(urpdf::RenderScaleBucket(2.1), LimitedBucket(595, 842, 2.1));
        }

        TEST_METHOD(LimitsLongPagesByDimension)
        {
            const int bucket = LimitedBucket(1000, 100, 20);
            Assert::AreEqual(urpdf::RenderScaleBucket(8), bucket);
            Assert::IsTrue(FitsPage(1000, 100, bucket));
            Assert::IsFalse(FitsPage(1000, 100, bucket + 1));
        }

        TEST_METHOD(LimitsLargePagesByTotalPixels)
        {
            const int bucket = LimitedBucket(1000, 1000, 20);
            Assert::AreEqual(urpdf::RenderScaleBucket(4), bucket);
            Assert::IsTrue(FitsPage(1000, 1000, bucket));
            Assert::IsFalse(FitsPage(1000, 1000, bucket + 1));
        }

        TEST_METHOD(CanonicalRoundingCannotExceedLimit)
        {
            const int bucket = LimitedBucket(1000, 100, 8.192);
            Assert::AreEqual(urpdf::RenderScaleBucket(8), bucket);
            Assert::AreEqual(bucket, LimitedBucket(1000, 100, 40));
        }

        TEST_METHOD(AccountsForOutwardPixelRounding)
        {
            const int bucket = LimitedBucket(8192.1, 100, 1);
            Assert::IsTrue(urpdf::RenderScaleForBucket(bucket) < 1);
            Assert::IsTrue(FitsPage(8192.1, 100, bucket));
            Assert::IsFalse(FitsPage(8192.1, 100, bucket + 1));
        }

        TEST_METHOD(ReducesHugePagesWithoutLosingCanonicalScale)
        {
            const int bucket = LimitedBucket(1e30, 2e30, 100);
            Assert::IsTrue(bucket > 0);
            Assert::IsTrue(FitsPage(1e30, 2e30, bucket));
            Assert::IsFalse(FitsPage(1e30, 2e30, bucket + 1));
            Assert::AreEqual(bucket, urpdf::RenderScaleBucket(urpdf::RenderScaleForBucket(bucket)));
        }

        TEST_METHOD(RejectsInvalidOrUnavailableBuckets)
        {
            Assert::AreEqual(0, urpdf::SelectLimitedRenderScaleBucket(0, [](int) { return true; }));
            Assert::AreEqual(0, urpdf::SelectLimitedRenderScaleBucket(-1, [](int) { return true; }));
            Assert::AreEqual(0, urpdf::SelectLimitedRenderScaleBucket(urpdf::RenderScaleBucket(2), [](int) { return false; }));
        }

        TEST_METHOD(VeryThinPagesStillFitOnePixelOnTheShortEdge)
        {
            const int bucket = LimitedBucket(100, 1e30, 20);
            Assert::IsTrue(bucket > 0);
            Assert::IsTrue(FitsPage(100, 1e30, bucket));
            Assert::IsFalse(FitsPage(100, 1e30, bucket + 1));
            Assert::IsTrue(100 * urpdf::RenderScaleForBucket(bucket) < 1);
            Assert::AreEqual(bucket, LimitedBucket(1e30, 100, 20));
        }
    };
}
