#include "CppUnitTest.h"
#include "../urPDF/PageText.h"

#include <string_view>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        // Each character is 10 points wide and the line is 12 points tall.
        void AppendLine(urpdf::PageText& text, std::u32string_view chars, double x, double y, std::uint32_t block = 0)
        {
            urpdf::TextLine line{ static_cast<std::uint32_t>(text.chars.size()), 0, block,
                { x, y, x + 10.0 * chars.size(), y + 12.0 } };
            for (std::size_t i = 0; i < chars.size(); ++i)
            {
                text.chars.push_back({ chars[i], { x + 10.0 * i, y, x + 10.0 * (i + 1), y + 12.0 } });
            }
            line.endChar = static_cast<std::uint32_t>(text.chars.size());
            text.lines.push_back(line);
        }
    }

    TEST_CLASS(PageTextStateTests)
    {
    public:
        TEST_METHOD(HitTestUsesCharacterHalves)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 100, 50);
            Assert::AreEqual(1u, urpdf::HitTest(text, { 112, 56 }));
            Assert::AreEqual(2u, urpdf::HitTest(text, { 118, 56 }));
            Assert::AreEqual(0u, urpdf::HitTest(text, { 100, 56 }));
            Assert::AreEqual(3u, urpdf::HitTest(text, { 129, 56 }));
        }

        TEST_METHOD(HitTestOutsideTextSnapsToNearestLineEnds)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 100, 50);
            AppendLine(text, U"de", 100, 70);
            Assert::AreEqual(0u, urpdf::HitTest(text, { 20, 55 }));
            Assert::AreEqual(3u, urpdf::HitTest(text, { 300, 55 }));
            Assert::AreEqual(3u, urpdf::HitTest(text, { 102, 75 }));
            Assert::AreEqual(5u, urpdf::HitTest(text, { 300, 400 }));
            Assert::AreEqual(1u, urpdf::HitTest(text, { 112, 0 }));
        }

        TEST_METHOD(HitTestBetweenLinesPicksVerticallyNearerLine)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 100, 50);
            AppendLine(text, U"def", 100, 70);
            Assert::AreEqual(1u, urpdf::HitTest(text, { 112, 64 }));
            Assert::AreEqual(4u, urpdf::HitTest(text, { 112, 67 }));
        }

        TEST_METHOD(HitTestInColumnGapStaysInNearerColumn)
        {
            urpdf::PageText text;
            AppendLine(text, U"left", 0, 50, 0);
            AppendLine(text, U"low", 0, 70, 0);
            AppendLine(text, U"right", 200, 50, 1);
            // The gap point is level with the right column's first line but horizontally
            // nearer the left column, so the caret stays at the end of the left line.
            Assert::AreEqual(4u, urpdf::HitTest(text, { 60, 56 }));
            Assert::AreEqual(7u, urpdf::HitTest(text, { 190, 56 }));
        }

        TEST_METHOD(HitTestOnEmptyPageReturnsStart)
        {
            Assert::AreEqual(0u, urpdf::HitTest({}, { 10, 10 }));
            urpdf::PageText malformed;
            malformed.lines.push_back({ 0, 3, 0, { 0, 0, 10, 10 } });
            Assert::AreEqual(0u, urpdf::HitTest(malformed, { 5, 5 }));
        }

        TEST_METHOD(EmptyAndPopulatedReadyCachesAreNotLoadedAgain)
        {
            for (bool empty : { false, true })
            {
                urpdf::PageTextSlot slot;
                const auto request = slot.BeginLoad();
                Assert::IsTrue(request.has_value());
                Assert::IsFalse(slot.BeginLoad().has_value());
                urpdf::PageText text;
                if (!empty) { AppendLine(text, U"abc", 0, 0); }
                Assert::IsTrue(slot.Complete(*request, std::move(text), true, false));
                slot.ResetPending();
                Assert::IsTrue(slot.State() == urpdf::PageTextState::ready);
                Assert::IsFalse(slot.BeginLoad().has_value());
                Assert::AreEqual(empty ? std::size_t(0) : std::size_t(3), slot.Text().chars.size());
            }
        }

        TEST_METHOD(DiscardedLoadCanRetry)
        {
            urpdf::PageTextSlot slot;
            const auto first = *slot.BeginLoad();
            Assert::IsTrue(slot.Complete(first, {}, false, false));
            Assert::IsTrue(slot.State() == urpdf::PageTextState::notLoaded);
            const auto retry = slot.BeginLoad();
            Assert::IsTrue(retry.has_value());
            Assert::IsTrue(*retry != first);
        }

        TEST_METHOD(TransferInvalidatesPendingOwnerEvenIfOldCompletionArrivesLast)
        {
            urpdf::PageTextSlot slot;
            const auto old = *slot.BeginLoad();
            slot.ResetPending();
            const auto current = *slot.BeginLoad();
            Assert::IsFalse(slot.Complete(old, {}, true, false));
            Assert::IsTrue(slot.State() == urpdf::PageTextState::loading);
            urpdf::PageText text;
            AppendLine(text, U"x", 0, 0);
            Assert::IsTrue(slot.Complete(current, std::move(text), true, false));
            Assert::IsFalse(slot.Complete(old, {}, false, true));
            Assert::IsTrue(slot.State() == urpdf::PageTextState::ready);
            Assert::AreEqual(std::size_t(1), slot.Text().chars.size());
        }

        TEST_METHOD(FailedLoadIsNotRetried)
        {
            urpdf::PageTextSlot slot;
            const auto request = *slot.BeginLoad();
            Assert::IsTrue(slot.Complete(request, {}, false, true));
            slot.ResetPending();
            Assert::IsTrue(slot.State() == urpdf::PageTextState::failed);
            Assert::IsFalse(slot.BeginLoad().has_value());
        }
    };
}
