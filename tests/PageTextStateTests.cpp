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

        void AssertBounds(urpdf::PdfPageBounds const& expected, urpdf::PdfPageBounds const& actual)
        {
            Assert::AreEqual(expected.x0, actual.x0);
            Assert::AreEqual(expected.y0, actual.y0);
            Assert::AreEqual(expected.x1, actual.x1);
            Assert::AreEqual(expected.y1, actual.y1);
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

        TEST_METHOD(SelectionRectsJoinsCharactersAtLineHeight)
        {
            urpdf::PageText text;
            AppendLine(text, U"abcd", 100, 50);
            text.chars[1].bounds.y0 = 53;
            text.chars[2].bounds.y1 = 60;

            const auto rectangles = urpdf::SelectionRects(text, 1, 3);
            Assert::AreEqual(std::size_t(1), rectangles.size());
            AssertBounds({ 110, 50, 130, 62 }, rectangles[0]);
        }

        TEST_METHOD(SelectionRectsClipsEachLineForEitherDragDirection)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 100, 50);
            AppendLine(text, U"defg", 120, 70);
            AppendLine(text, U"hi", 200, 90);
            const auto forward = urpdf::SelectionRects(text, 2, 8);
            const auto backward = urpdf::SelectionRects(text, 8, 2);

            for (auto const* rectangles : { &forward, &backward })
            {
                Assert::AreEqual(std::size_t(3), rectangles->size());
                AssertBounds({ 120, 50, 130, 62 }, (*rectangles)[0]);
                AssertBounds({ 120, 70, 160, 82 }, (*rectangles)[1]);
                AssertBounds({ 200, 90, 210, 102 }, (*rectangles)[2]);
            }
        }

        TEST_METHOD(SelectionRectsExcludesTheEndCaretAndEmptyRanges)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 100, 50);
            AppendLine(text, U"de", 100, 70);

            const auto rectangles = urpdf::SelectionRects(text, 0, 3);
            Assert::AreEqual(std::size_t(1), rectangles.size());
            AssertBounds({ 100, 50, 130, 62 }, rectangles[0]);
            Assert::IsTrue(urpdf::SelectionRects(text, 2, 2).empty());
            Assert::IsTrue(urpdf::SelectionRects(text, 5, 9).empty());
            Assert::IsTrue(urpdf::SelectionRects({}, 0, 1).empty());
        }

        TEST_METHOD(SelectionTextCopiesTheSameRangeInEitherDirection)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 0, 0);
            AppendLine(text, U"def", 0, 20);

            Assert::AreEqual(L"bc\r\nde", urpdf::SelectionText(text, 1, 5).c_str());
            Assert::AreEqual(L"bc\r\nde", urpdf::SelectionText(text, 5, 1).c_str());
            Assert::AreEqual(L"abc", urpdf::SelectionText(text, 0, 3).c_str());
            Assert::IsTrue(urpdf::SelectionText(text, 2, 2).empty());
            Assert::IsTrue(urpdf::SelectionText(text, 6, 9).empty());
        }

        TEST_METHOD(SelectionTextTrimsSpacesBeforeCrLf)
        {
            urpdf::PageText text;
            AppendLine(text, U"a b  ", 0, 0);
            AppendLine(text, U"  ", 0, 20);
            AppendLine(text, U" c ", 0, 40, 1);

            Assert::AreEqual(L"a b\r\n\r\n c ", urpdf::SelectionText(text, 0, 10).c_str());
        }

        TEST_METHOD(SelectionTextEncodesUnicodeAsUtf16)
        {
            urpdf::PageText text;
            AppendLine(text, U"\u0141\U0001F600x", 0, 0);
            text.chars[2].codepoint = 0xD800;

            const std::wstring expected{ L'\u0141', 0xD83D, 0xDE00, 0xFFFD };
            Assert::AreEqual(expected.c_str(), urpdf::SelectionText(text, 0, 3).c_str());
        }

        TEST_METHOD(IsOverTextUsesLineBoundsAndTolerance)
        {
            urpdf::PageText text;
            AppendLine(text, U"abc", 100, 50);
            AppendLine(text, U"def", 200, 50);

            Assert::IsTrue(urpdf::IsOverText(text, { 115, 56 }));
            Assert::IsTrue(urpdf::IsOverText(text, { 99, 56 }));
            Assert::IsTrue(urpdf::IsOverText(text, { 99.4, 49.4 }));
            Assert::IsFalse(urpdf::IsOverText(text, { 99.2, 49.2 }));
            Assert::IsFalse(urpdf::IsOverText(text, { 160, 56 }));
            Assert::IsFalse(urpdf::IsOverText(text, { 115, 66 }));
            Assert::IsFalse(urpdf::IsOverText({}, { 115, 56 }));
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
