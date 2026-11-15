#include "CppUnitTest.h"
#include "../urPDF/WindowPlacement.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        void AssertBounds(std::optional<RECT> const& actual, RECT const& expected)
        {
            Assert::IsTrue(actual.has_value());
            Assert::IsTrue(EqualRect(&*actual, &expected) != FALSE);
        }
    }

    TEST_CLASS(WindowPlacementTests)
    {
    public:
        TEST_METHOD(TearOutKeepsRestoreSizeAndTabUnderCursor)
        {
            // Restore coordinates can be workspace-relative; only their size is used.
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { -1800, 80, -900, 680 }, { 0, 0, 1920, 1040 }, { 500, 300 }, { 140, 20 }),
                { 360, 280, 1260, 880 });
        }

        TEST_METHOD(TearOutKeepsNormalSizeAtEveryScreenEdge)
        {
            const RECT normal{ 100, 100, 1000, 700 };
            const RECT work{ 0, 40, 1920, 1040 };
            AssertBounds(urpdf::ResolveTabTearOutPlacement(normal, work, { 0, 40 }, { 140, 20 }),
                { 0, 40, 900, 640 });
            AssertBounds(urpdf::ResolveTabTearOutPlacement(normal, work, { 1919, 40 }, { 140, 20 }),
                { 1020, 40, 1920, 640 });
            AssertBounds(urpdf::ResolveTabTearOutPlacement(normal, work, { 0, 1039 }, { 140, 20 }),
                { 0, 440, 900, 1040 });
            AssertBounds(urpdf::ResolveTabTearOutPlacement(normal, work, { 1919, 1039 }, { 140, 20 }),
                { 1020, 440, 1920, 1040 });
        }

        TEST_METHOD(TearOutFitsSmallerMonitorWithNegativeCoordinatesAndTaskbar)
        {
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { 100, 100, 1600, 1000 }, { -1280, -760, 0, 0 }, { -10, -10 }, { 140, 20 }),
                { -960, -570, 0, 0 });
        }

        TEST_METHOD(TearOutShrinksOnlyTheDimensionThatExceedsWorkArea)
        {
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { 0, 0, 1500, 500 }, { -1280, 40, 0, 1000 }, { -500, 200 }, { 100, 20 }),
                { -1280, 180, 0, 680 });
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { 0, 0, 500, 1500 }, { 1920, 0, 3200, 960 }, { 2500, 500 }, { 100, 20 }),
                { 2400, 0, 2900, 960 });
        }

        TEST_METHOD(TearOutWithoutValidRestoreSizeUsesThreeQuartersOfWorkArea)
        {
            const RECT invalidBounds[] = {
                {}, { 0, 0, 0, 600 }, { 0, 0, 900, 0 },
                { 100, 100, 0, 700 }, { 100, 100, 1000, 0 }
            };
            for (auto bounds : invalidBounds)
            {
                AssertBounds(urpdf::ResolveTabTearOutPlacement(
                    bounds, { 0, 40, 1920, 1080 }, { 100, 100 }, { 40, 20 }),
                    { 60, 80, 1500, 860 });
            }
        }

        TEST_METHOD(TearOutWithWorkAreaSizedRestoreBoundsUsesFallback)
        {
            for (auto bounds : { RECT{ 0, 0, 1920, 1040 }, RECT{ -8, -8, 1928, 1088 } })
            {
                AssertBounds(urpdf::ResolveTabTearOutPlacement(
                    bounds, { 0, 40, 1920, 1080 }, { 100, 100 }, { 40, 20 }),
                    { 60, 80, 1500, 860 });
            }
        }

        TEST_METHOD(TearOutClampsDragOffsetToResizedWindow)
        {
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { 0, 0, 1500, 500 }, { 0, 0, 1000, 1000 }, { 999, 600 }, { 1400, 700 }),
                { 0, 101, 1000, 601 });
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { 0, 0, 400, 300 }, { 0, 0, 1000, 1000 }, { 200, 200 }, { -50, -20 }),
                { 200, 200, 600, 500 });
        }

        TEST_METHOD(TearOutRejectsInvalidWorkArea)
        {
            const RECT invalidWorkAreas[] = {
                {}, { 0, 0, 0, 1000 }, { 0, 0, 1000, 0 }, { 1000, 0, 0, 1000 },
                { (std::numeric_limits<LONG>::min)(), 0, (std::numeric_limits<LONG>::max)(), 1000 }
            };
            for (auto work : invalidWorkAreas)
            {
                Assert::IsFalse(urpdf::ResolveTabTearOutPlacement(
                    { 0, 0, 900, 600 }, work, { 100, 100 }, { 40, 20 }).has_value());
            }
        }

        TEST_METHOD(TearOutFallbackHasPositiveSizeEvenForTinyWorkArea)
        {
            AssertBounds(urpdf::ResolveTabTearOutPlacement({}, { -1, -1, 0, 0 }, {}, {}),
                { -1, -1, 0, 0 });
        }

        TEST_METHOD(TearOutDoesNotOverflowWhenSubtractingCursorOffset)
        {
            const auto minimum = (std::numeric_limits<LONG>::min)();
            AssertBounds(urpdf::ResolveTabTearOutPlacement(
                { 0, 0, 900, 600 }, { minimum, minimum, minimum + 1920, minimum + 1040 },
                { minimum, minimum }, { 140, 20 }),
                { minimum, minimum, minimum + 900, minimum + 600 });
        }
    };
}
