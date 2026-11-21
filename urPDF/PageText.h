#pragma once

#include "PageCoordinateGeometry.h"

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace urpdf
{
    struct TextChar
    {
        char32_t codepoint{ 0 };
        PdfPageBounds bounds;
    };

    struct TextLine
    {
        // Range [firstChar, endChar) in PageText::chars.
        std::uint32_t firstChar{ 0 };
        std::uint32_t endChar{ 0 };
        std::uint32_t block{ 0 };
        PdfPageBounds bounds;
    };

    // Characters keep MuPDF's structured-text order (blocks, lines, characters), in
    // the same page space as PageLink rectangles.
    struct PageText
    {
        std::vector<TextChar> chars;
        std::vector<TextLine> lines;
    };

    // A caret between characters, or the end of a page whose text may be unloaded.
    inline constexpr auto kPageEnd = (std::numeric_limits<std::uint32_t>::max)();

    struct TextPosition
    {
        int page{ 0 };
        std::uint32_t caret{ 0 };
        auto operator<=>(TextPosition const&) const = default;
    };

    struct TextSelection
    {
        TextPosition anchor;
        TextPosition focus;
        bool operator==(TextSelection const&) const = default;
        std::pair<TextPosition, TextPosition> InDocumentOrder() const noexcept;
    };

    // Keep the PDF position even after pointer release until the caret can be resolved.
    struct PendingTextPosition
    {
        int page{ 0 };
        PdfPagePoint point;
        bool operator==(PendingTextPosition const& other) const noexcept
        {
            return page == other.page && point.x == other.point.x && point.y == other.point.y;
        }
    };

    using TextCaretRange = std::pair<std::uint32_t, std::uint32_t>;
    std::optional<TextCaretRange> PageSelectionCarets(TextSelection const& selection, int page) noexcept;

    // Picks the line under (or nearest to) the point first, so points between lines or
    // columns do not jump to an unrelated character elsewhere on the page.
    std::uint32_t HitTest(PageText const& text, PdfPagePoint point) noexcept;
    bool IsOverText(PageText const& text, PdfPagePoint point) noexcept;

    // One rectangle per line intersecting the half-open range between the carets.
    std::vector<PdfPageBounds> SelectionRects(PageText const& text, std::uint32_t firstCaret, std::uint32_t lastCaret);
    // Windows UTF-16 text for the same range, with CRLF between selected lines.
    std::wstring SelectionText(PageText const& text, std::uint32_t firstCaret, std::uint32_t lastCaret);

    enum class PageTextState { notLoaded, loading, ready, failed };

    // Same lifecycle as PageLinksSlot: independent of raster generations, request IDs
    // reject completions belonging to an earlier window owner after a tab transfer.
    class PageTextSlot
    {
    public:
        PageTextState State() const noexcept { return m_state; }
        PageText const& Text() const noexcept { return m_text; }

        std::optional<std::uint64_t> BeginLoad() noexcept
        {
            if (m_state != PageTextState::notLoaded) { return std::nullopt; }
            m_state = PageTextState::loading;
            return ++m_requestId;
        }

        bool Complete(std::uint64_t requestId, PageText text, bool loaded, bool failed) noexcept
        {
            if (m_state != PageTextState::loading || requestId != m_requestId) { return false; }
            m_text = std::move(text);
            m_state = failed ? PageTextState::failed : loaded ? PageTextState::ready : PageTextState::notLoaded;
            return true;
        }

        void ResetPending() noexcept
        {
            if (m_state == PageTextState::loading)
            {
                ++m_requestId;
                m_state = PageTextState::notLoaded;
            }
        }

    private:
        PageTextState m_state{ PageTextState::notLoaded };
        std::uint64_t m_requestId{ 0 };
        PageText m_text;
    };

    // Only complete selections can be copied. Failed and empty pages are skipped.
    std::wstring SelectionText(std::span<PageTextSlot const> pages, TextSelection const& selection);
}
