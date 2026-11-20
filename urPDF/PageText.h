#pragma once

#include "PageCoordinateGeometry.h"

#include <cstdint>
#include <optional>
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

    // A caret between characters, in the range 0..chars.size().
    struct TextPosition
    {
        int page{ 0 };
        std::uint32_t caret{ 0 };
        bool operator==(TextPosition const&) const = default;
    };

    struct TextSelection
    {
        TextPosition anchor;
        TextPosition focus;
        bool operator==(TextSelection const&) const = default;
    };

    // A press can arrive before the page's text is loaded. Keep the PDF points
    // until the text is ready, even if the pointer has already been released.
    struct PendingTextPress
    {
        int page{ 0 };
        PdfPagePoint pressed;
        PdfPagePoint moved;
    };

    // Picks the line under (or nearest to) the point first, so points between lines or
    // columns do not jump to an unrelated character elsewhere on the page.
    std::uint32_t HitTest(PageText const& text, PdfPagePoint point) noexcept;
    bool IsOverText(PageText const& text, PdfPagePoint point) noexcept;

    // One rectangle per line intersecting the half-open range between the carets.
    std::vector<PdfPageBounds> SelectionRects(PageText const& text, std::uint32_t firstCaret, std::uint32_t lastCaret);

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
}
