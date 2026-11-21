#include "PageText.h"

#include <algorithm>
#include <cmath>

namespace urpdf
{
    namespace
    {
        constexpr double LineHitTolerance = 1.0;

        double DistanceOutside(double value, double low, double high) noexcept
        {
            return value < low ? low - value : value > high ? value - high : 0.0;
        }

        bool HasCharacters(TextLine const& line, PageText const& text) noexcept
        {
            return line.firstChar < line.endChar && line.endChar <= text.chars.size();
        }

        void AppendUtf16(std::wstring& text, char32_t codepoint)
        {
            if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
            {
                codepoint = 0xFFFD;
            }
            if (codepoint <= 0xFFFF)
            {
                text.push_back(static_cast<wchar_t>(codepoint));
                return;
            }
            codepoint -= 0x10000;
            text.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
            text.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
        }

        void AppendLineBreak(std::wstring& text)
        {
            while (!text.empty() && text.back() == L' ') { text.pop_back(); }
            text.append(L"\r\n");
        }

        PdfPagePoint DistanceFromLine(PdfPagePoint point, PdfPageBounds bounds) noexcept
        {
            return { DistanceOutside(point.x, bounds.x0, bounds.x1), DistanceOutside(point.y, bounds.y0, bounds.y1) };
        }
    }

    std::pair<TextPosition, TextPosition> TextSelection::InDocumentOrder() const noexcept
    {
        return std::minmax(anchor, focus);
    }

    std::optional<TextCaretRange> PageSelectionCarets(TextSelection const& selection, int page) noexcept
    {
        const auto [begin, end] = selection.InDocumentOrder();
        if (page < begin.page || page > end.page) { return std::nullopt; }
        const auto first = page == begin.page ? begin.caret : 0;
        const auto last = page == end.page ? end.caret : kPageEnd;
        if (first == last) { return std::nullopt; }
        return TextCaretRange{ first, last };
    }

    std::uint32_t HitTest(PageText const& text, PdfPagePoint point) noexcept
    {
        TextLine const* nearest = nullptr;
        double nearestDistance = 0.0;
        double nearestVertical = 0.0;
        for (auto const& line : text.lines)
        {
            if (!HasCharacters(line, text)) { continue; }
            const auto [dx, dy] = DistanceFromLine(point, line.bounds);
            double distance = std::hypot(dx, dy);
            if (distance <= LineHitTolerance) { distance = 0.0; }
            if (!nearest || distance < nearestDistance || (distance == nearestDistance && dy < nearestVertical))
            {
                nearest = &line;
                nearestDistance = distance;
                nearestVertical = dy;
            }
        }
        if (!nearest) { return 0; }

        for (auto i = nearest->firstChar; i < nearest->endChar; ++i)
        {
            auto const& bounds = text.chars[i].bounds;
            if (point.x < (bounds.x0 + bounds.x1) / 2.0) { return i; }
        }
        return nearest->endChar;
    }

    bool IsOverText(PageText const& text, PdfPagePoint point) noexcept
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) { return false; }
        return std::any_of(text.lines.begin(), text.lines.end(), [&](TextLine const& line)
            {
                if (!HasCharacters(line, text)) { return false; }
                const auto [dx, dy] = DistanceFromLine(point, line.bounds);
                return std::hypot(dx, dy) <= LineHitTolerance;
            });
    }

    std::vector<PdfPageBounds> SelectionRects(PageText const& text, std::uint32_t firstCaret, std::uint32_t lastCaret)
    {
        std::vector<PdfPageBounds> rectangles;
        if (firstCaret == lastCaret) { return rectangles; }

        const auto [begin, end] = std::minmax(firstCaret, lastCaret);
        for (auto const& line : text.lines)
        {
            if (!HasCharacters(line, text)) { continue; }
            const auto first = (std::max)(begin, line.firstChar);
            const auto last = (std::min)(end, line.endChar);
            if (first >= last) { continue; }

            const PdfPageBounds bounds{
                text.chars[first].bounds.x0, line.bounds.y0,
                text.chars[last - 1].bounds.x1, line.bounds.y1 };
            if (bounds.x0 < bounds.x1 && bounds.y0 < bounds.y1) { rectangles.push_back(bounds); }
        }
        return rectangles;
    }

    std::wstring SelectionText(PageText const& text, std::uint32_t firstCaret, std::uint32_t lastCaret)
    {
        std::wstring result;
        if (firstCaret == lastCaret) { return result; }
        const auto [begin, end] = std::minmax(firstCaret, lastCaret);
        for (auto const& line : text.lines)
        {
            if (!HasCharacters(line, text)) { continue; }
            const auto first = (std::max)(begin, line.firstChar);
            const auto last = (std::min)(end, line.endChar);
            if (first >= last) { continue; }

            if (!result.empty()) { AppendLineBreak(result); }
            for (auto i = first; i < last; ++i) { AppendUtf16(result, text.chars[i].codepoint); }
        }
        return result;
    }

    std::wstring SelectionText(std::span<PageTextSlot const> pages, TextSelection const& selection)
    {
        const auto [begin, end] = selection.InDocumentOrder();
        if (begin.page < 0 || static_cast<std::size_t>(end.page) >= pages.size()) { return {}; }

        std::wstring result;
        for (auto page = static_cast<std::size_t>(begin.page); page <= static_cast<std::size_t>(end.page); ++page)
        {
            const auto carets = PageSelectionCarets(selection, static_cast<int>(page));
            if (!carets || pages[page].State() == PageTextState::failed) { continue; }
            if (pages[page].State() != PageTextState::ready) { return {}; }

            const auto text = SelectionText(pages[page].Text(), carets->first, carets->second);
            if (text.empty()) { continue; }
            if (!result.empty()) { AppendLineBreak(result); }
            result.append(text);
        }
        return result;
    }
}
