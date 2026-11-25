#include "CppUnitTest.h"
#include "../urPDF/PageImages.h"

#include <limits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    TEST_CLASS(PageImagesStateTests)
    {
    public:
        TEST_METHOD(ClipsImageRectToPageAndRejectsInvalid)
        {
            const urpdf::PdfPageBounds page{ 0.0, 0.0, 200.0, 300.0 };
            const auto clipped = urpdf::TryClipImageRect({ -10.0, 20.0, 100.0, 350.0 }, page);
            Assert::IsTrue(clipped.has_value());
            Assert::AreEqual(0.0, clipped->x0);
            Assert::AreEqual(20.0, clipped->y0);
            Assert::AreEqual(100.0, clipped->x1);
            Assert::AreEqual(300.0, clipped->y1);

            Assert::IsFalse(urpdf::TryClipImageRect({ 210.0, 10.0, 250.0, 50.0 }, page).has_value());
            Assert::IsFalse(urpdf::TryClipImageRect({ 50.0, 50.0, 50.0, 100.0 }, page).has_value());
            Assert::IsFalse(urpdf::TryClipImageRect({ 80.0, 20.0, 40.0, 100.0 }, page).has_value());
            const double nan = std::numeric_limits<double>::quiet_NaN();
            Assert::IsFalse(urpdf::TryClipImageRect({ nan, 0.0, 10.0, 10.0 }, page).has_value());
            Assert::IsFalse(urpdf::TryClipImageRect({ 10.0, 10.0, 50.0, 50.0 }, { 0.0, 0.0, 0.0, 100.0 }).has_value());
        }

        TEST_METHOD(HitTestPicksTopmostOverlappingImageAndEdges)
        {
            const std::vector<urpdf::PageImage> images{
                { 0, { 10.0, 10.0, 150.0, 150.0 } },
                { 2, { 40.0, 40.0, 90.0, 90.0 } },
                { 5, { 160.0, 20.0, 190.0, 60.0 } }
            };

            const auto overlap = urpdf::HitTestImage(images, { 60.0, 60.0 });
            Assert::IsTrue(overlap.has_value());
            Assert::AreEqual(std::uint32_t(2), *overlap);

            const auto backgroundOnly = urpdf::HitTestImage(images, { 20.0, 20.0 });
            Assert::IsTrue(backgroundOnly.has_value());
            Assert::AreEqual(std::uint32_t(0), *backgroundOnly);

            const auto edge = urpdf::HitTestImage(images, { 190.0, 60.0 });
            Assert::IsTrue(edge.has_value());
            Assert::AreEqual(std::uint32_t(5), *edge);

            Assert::IsFalse(urpdf::HitTestImage(images, { 155.0, 50.0 }).has_value());
            Assert::IsFalse(urpdf::HitTestImage(images, { std::numeric_limits<double>::quiet_NaN(), 50.0 }).has_value());
            Assert::IsFalse(urpdf::HitTestImage({}, { 20.0, 20.0 }).has_value());
        }

        TEST_METHOD(FindPageImageMatchesNonContiguousIndices)
        {
            const std::vector<urpdf::PageImage> images{
                { 1, { 10.0, 10.0, 50.0, 50.0 } },
                { 4, { 60.0, 10.0, 90.0, 50.0 } }
            };

            const auto* found = urpdf::FindPageImage(images, 4);
            Assert::IsNotNull(found);
            Assert::AreEqual(60.0, found->bounds.x0);
            Assert::IsNull(urpdf::FindPageImage(images, 0));
            Assert::IsNull(urpdf::FindPageImage(images, 2));
        }

        TEST_METHOD(SuggestedFileNameStripsPdfExtensionAndSanitizes)
        {
            Assert::AreEqual(std::wstring(L"report-page-1-image-1"),
                urpdf::SuggestedImageFileName(L"report.pdf", 0, 0));
            Assert::AreEqual(std::wstring(L"Annual.Report-page-3-image-2"),
                urpdf::SuggestedImageFileName(L"Annual.Report.PDF", 2, 1));
            Assert::AreEqual(std::wstring(L"a_b_c_d_-page-1-image-1"),
                urpdf::SuggestedImageFileName(L"a:b/c*d?.pdf", 0, 0));
            Assert::AreEqual(std::wstring(L"image-page-1-image-1"),
                urpdf::SuggestedImageFileName(L"  ...  ", -5, 0));
            Assert::AreEqual(std::wstring(L"image-page-2-image-3"),
                urpdf::SuggestedImageFileName(L".pdf", 1, 2));
        }
    };
}
