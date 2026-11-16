#include "CppUnitTest.h"
#include "../urPDF/PageOverlayState.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        urpdf::PageRasterGeometry OverlayRaster()
        {
            return { { 10, 20, 110, 220 }, { 2, 0, 0, 2, 0, 0 },
                { 20, 40, 220, 440 }, { 200, 400 } };
        }

        urpdf::PageOverlayState::Binding Bind(urpdf::PageOverlayState& state,
            std::uint64_t documentId = 1, int page = 0, std::uint64_t generation = 1)
        {
            const auto binding = state.Bind(documentId, page, generation);
            Assert::IsTrue(binding.has_value());
            return *binding;
        }

        void CheckPoint(urpdf::PageOverlayState const& state, double x, double y)
        {
            Assert::IsTrue(state.Mapping().has_value());
            const auto point = state.Mapping()->PdfToElementPoint({ 10, 20 });
            Assert::IsTrue(point.has_value());
            Assert::AreEqual(x, point->x, 1e-9);
            Assert::AreEqual(y, point->y, 1e-9);
        }
    }

    TEST_CLASS(PageOverlayStateTests)
    {
    public:
        TEST_METHOD(PreparedPageWaitsForBitmapAndMeasuredLayout)
        {
            urpdf::PageOverlayState state;
            const auto binding = Bind(state, 12, 7, 3);
            Assert::IsTrue(binding.documentId == 12 && binding.pageIndex == 7 && binding.generation == 3);
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsTrue(state.Resize(binding, { 400, 400 }));
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsTrue(state.SetRaster(binding, OverlayRaster(), {}));
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsTrue(state.Resize(binding, { 400, 400 }));
            CheckPoint(state, 100, 0);
        }

        TEST_METHOD(ClearingRejectsCallbacksEvenAfterRebindingSamePage)
        {
            urpdf::PageOverlayState state;
            const auto old = Bind(state);
            state.SetRaster(old, OverlayRaster(), { 400, 400 });
            state.Clear();
            Assert::IsFalse(state.CurrentBinding().has_value());
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsFalse(state.SetRaster(old, OverlayRaster(), { 400, 400 }));
            Assert::IsFalse(state.Resize(old, { 400, 400 }));
            Assert::IsFalse(state.ClearRaster(old));

            const auto current = Bind(state);
            Assert::IsFalse(current == old);
            Assert::IsFalse(state.Mapping().has_value());
            state.SetRaster(current, OverlayRaster(), { 600, 400 });
            Assert::IsFalse(state.Resize(old, { 400, 400 }));
            CheckPoint(state, 200, 0);
        }

        TEST_METHOD(RecyclingToAnotherPageOrDocumentDropsPreviousGeometry)
        {
            urpdf::PageOverlayState state;
            const auto firstPage = Bind(state, 1, 0);
            state.SetRaster(firstPage, OverlayRaster(), { 400, 400 });
            const auto secondPage = Bind(state, 1, 1);
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsFalse(state.SetRaster(firstPage, OverlayRaster(), { 400, 400 }));
            state.SetRaster(secondPage, OverlayRaster(), { 600, 400 });
            const auto otherDocument = Bind(state, 2, 1);
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsFalse(state.Resize(secondPage, { 400, 400 }));
            state.SetRaster(otherDocument, OverlayRaster(), { 400, 400 });
            CheckPoint(state, 100, 0);
        }

        TEST_METHOD(NewGenerationKeepsDisplayedSnapshotUntilReplacementIsAccepted)
        {
            urpdf::PageOverlayState state;
            const auto old = Bind(state, 1, 0, 4);
            state.SetRaster(old, OverlayRaster(), { 400, 400 });
            const auto current = Bind(state, 1, 0, 5);
            Assert::IsFalse(state.Mapping().has_value());
            // The UI republishes the still-visible bitmap into its new binding.
            state.SetRaster(current, OverlayRaster(), { 400, 400 });
            CheckPoint(state, 100, 0);
            const urpdf::PageRasterGeometry rotated{ { 10, 20, 110, 220 },
                { 0, 2, -2, 0, 0, 0 }, { -440, 20, -40, 220 }, { 400, 200 } };
            Assert::IsFalse(state.SetRaster(old, rotated, { 400, 400 }));
            CheckPoint(state, 100, 0);
            Assert::IsTrue(state.SetRaster(current, rotated, { 400, 400 }));
            CheckPoint(state, 400, 100);
            Assert::IsFalse(state.ClearRaster(old));
            CheckPoint(state, 400, 100);
        }

        TEST_METHOD(ResizeUsesStoredRasterAndRecoversAfterEmptyLayout)
        {
            urpdf::PageOverlayState state;
            const auto binding = Bind(state);
            state.SetRaster(binding, OverlayRaster(), { 400, 400 });
            CheckPoint(state, 100, 0);
            state.Resize(binding, { 600, 400 });
            CheckPoint(state, 200, 0);
            state.Resize(binding, {});
            Assert::IsFalse(state.Mapping().has_value());
            state.Resize(binding, { 400, 400 });
            CheckPoint(state, 100, 0);
        }

        TEST_METHOD(ReconfiguringSameBindingPreservesItsMapping)
        {
            urpdf::PageOverlayState state;
            const auto binding = Bind(state);
            state.SetRaster(binding, OverlayRaster(), { 400, 400 });
            const auto unchanged = Bind(state);
            Assert::IsTrue(unchanged == binding);
            CheckPoint(state, 100, 0);
        }

        TEST_METHOD(ReleasingBitmapPreventsResizeFromRestoringOldGeometry)
        {
            urpdf::PageOverlayState state;
            const auto binding = Bind(state);
            state.SetRaster(binding, OverlayRaster(), { 400, 400 });
            Assert::IsTrue(state.ClearRaster(binding));
            Assert::IsTrue(state.IsCurrent(binding));
            Assert::IsFalse(state.Mapping().has_value());
            state.Resize(binding, { 600, 400 });
            Assert::IsFalse(state.Mapping().has_value());
            state.SetRaster(binding, OverlayRaster(), { 600, 400 });
            CheckPoint(state, 200, 0);
        }

        TEST_METHOD(InvalidPageIdentityClearsBindingAndInvalidatesCallbacks)
        {
            urpdf::PageOverlayState state;
            const auto binding = Bind(state);
            state.SetRaster(binding, OverlayRaster(), { 400, 400 });
            Assert::IsFalse(state.Bind(0, 0, 1).has_value());
            Assert::IsFalse(state.CurrentBinding().has_value());
            Assert::IsFalse(state.Mapping().has_value());
            Assert::IsFalse(state.SetRaster(binding, OverlayRaster(), { 400, 400 }));
            Assert::IsFalse(state.Bind(1, -1, 1).has_value());
        }
    };
}
