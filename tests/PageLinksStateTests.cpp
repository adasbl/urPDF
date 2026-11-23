#include "CppUnitTest.h"
#include "../urPDF/PageLinks.h"
#include "../urPDF/DocumentOperation.h"

#include <future>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    TEST_CLASS(PageLinksStateTests)
    {
    public:
        TEST_METHOD(ClipsLinkBoundsInUnnormalizedPageCoordinates)
        {
            const urpdf::PdfPageBounds page{ 100, -20, 300, 400 };
            const auto inside = urpdf::TryClipLinkRect({ 110, 0, 200, 100 }, page);
            Assert::IsTrue(inside.has_value());
            Assert::AreEqual(110.0, inside->x0);
            Assert::AreEqual(100.0, inside->y1);
            const auto clipped = urpdf::TryClipLinkRect({ 50, -100, 200, 500 }, page);
            Assert::IsTrue(clipped.has_value());
            Assert::AreEqual(100.0, clipped->x0);
            Assert::AreEqual(-20.0, clipped->y0);
            Assert::AreEqual(200.0, clipped->x1);
            Assert::AreEqual(400.0, clipped->y1);
        }

        TEST_METHOD(RejectsEmptyOutsideReversedAndNonfiniteBounds)
        {
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            const auto inf = std::numeric_limits<double>::infinity();
            const urpdf::PdfPageBounds page{ 0, 0, 100, 100 };
            for (auto rect : { urpdf::PdfPageBounds{ 100, 0, 200, 20 }, { -20, -20, -1, -1 },
                { 0, 0, 0, 20 }, { 20, 0, 10, 20 }, { nan, 0, 10, 10 }, { 0, 0, inf, 10 } })
            {
                Assert::IsFalse(urpdf::TryClipLinkRect(rect, page).has_value());
                Assert::IsFalse(urpdf::TryClipLinkRect(page, rect).has_value());
            }
        }

        TEST_METHOD(EmptyAndPopulatedReadyCachesAreNotLoadedAgain)
        {
            for (bool empty : { false, true })
            {
                urpdf::PageLinksSlot slot;
                const auto request = slot.BeginLoad();
                Assert::IsTrue(request.has_value());
                Assert::IsFalse(slot.BeginLoad().has_value());
                std::vector<urpdf::PageLink> links;
                if (!empty) { links.push_back({ { 1, 2, 3, 4 }, urpdf::PageLinkKind::external, "https://example.com" }); }
                Assert::IsTrue(slot.Complete(*request, std::move(links), true, false));
                // Raster changes and window transfers must preserve ready data.
                slot.ResetPending();
                Assert::IsTrue(slot.State() == urpdf::PageLinksState::ready);
                Assert::IsFalse(slot.BeginLoad().has_value());
                Assert::AreEqual(empty ? std::size_t(0) : std::size_t(1), slot.Links().size());
            }
        }

        TEST_METHOD(GenerationDiscardCanRetryAfterPageWasAlreadyConfigured)
        {
            urpdf::PageLinksSlot slot;
            std::mutex mutex;
            std::atomic<std::uint64_t> generation{ 1 };
            const auto first = *slot.BeginLoad();
            std::unique_lock lock(mutex);
            auto worker = std::async(std::launch::async, [&]
                { return urpdf::RunDocumentOperation(mutex, generation, 1, [] {}); });
            generation.store(2);
            // The new raster's configure happens while the old request is pending.
            Assert::IsFalse(slot.BeginLoad().has_value());
            lock.unlock();
            Assert::IsFalse(worker.get());
            Assert::IsTrue(slot.Complete(first, {}, false, false));
            // Completion's refresh explicitly starts another request immediately.
            const auto retry = slot.BeginLoad();
            Assert::IsTrue(retry.has_value());
            Assert::IsTrue(*retry != first);
            Assert::IsTrue(slot.Complete(*retry, {}, true, false));
            Assert::IsTrue(slot.State() == urpdf::PageLinksState::ready);
        }

        TEST_METHOD(TransferInvalidatesPendingOwnerEvenIfOldCompletionArrivesLast)
        {
            urpdf::PageLinksSlot slot;
            const auto old = *slot.BeginLoad();
            slot.ResetPending();
            const auto current = *slot.BeginLoad();
            Assert::IsFalse(slot.Complete(old, {}, true, false));
            Assert::IsTrue(slot.State() == urpdf::PageLinksState::loading);
            std::vector<urpdf::PageLink> links{ { { 1, 2, 3, 4 }, urpdf::PageLinkKind::internal, "#page=2", 1 } };
            Assert::IsTrue(slot.Complete(current, std::move(links), true, false));
            Assert::IsFalse(slot.Complete(old, {}, false, true));
            Assert::IsTrue(slot.State() == urpdf::PageLinksState::ready);
            Assert::AreEqual(1, slot.Links().front().targetPageIndex);
        }

        TEST_METHOD(FailedLoadDoesNotRetryOnEveryConfigure)
        {
            urpdf::PageLinksSlot slot;
            const auto request = *slot.BeginLoad();
            Assert::IsTrue(slot.Complete(request, {}, false, true));
            slot.ResetPending();
            Assert::IsTrue(slot.State() == urpdf::PageLinksState::failed);
            Assert::IsFalse(slot.BeginLoad().has_value());
        }
    };
}
