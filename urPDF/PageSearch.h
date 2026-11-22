#pragma once

#include "DocumentViewState.h"
#include "PageCoordinateGeometry.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace urpdf
{
    struct SearchMatch
    {
        int pageIndex{ 0 };
        std::vector<PdfPageBounds> rects;

        bool operator==(SearchMatch const& other) const noexcept;
    };

    // Half-open range [begin, end) into DocumentSearchState::Matches() for one page.
    struct PageMatchRange
    {
        std::size_t begin{ 0 };
        std::size_t end{ 0 };

        bool Empty() const noexcept { return begin >= end; }
        std::size_t Count() const noexcept { return begin < end ? end - begin : 0; }
        bool operator==(PageMatchRange const&) const = default;
    };

    enum class SearchStatus { idle, searching, ready, failed };

    bool IsBlankSearchQuery(std::wstring_view query) noexcept;

    std::optional<PdfPageBounds> TryClipSearchRect(PdfPageBounds rect, PdfPageBounds page) noexcept;

    // Clips each match rectangle to the page bounds and returns their union bounding box.
    std::optional<PdfPageBounds> TrySearchMatchBounds(std::span<PdfPageBounds const> rects, PdfPageBounds page) noexcept;

    // UI-thread search state per document. Request IDs invalidate background completions
    // from earlier queries, closed search bars, or prior window owners after a tab transfer.
    class DocumentSearchState
    {
    public:
        SearchStatus State() const noexcept { return m_state; }
        std::wstring const& Query() const noexcept { return m_query; }
        std::uint64_t RequestId() const noexcept { return m_requestId; }
        std::vector<SearchMatch> const& Matches() const noexcept { return m_matches; }
        std::size_t MatchCount() const noexcept { return m_matches.size(); }
        PageMatchRange PageMatchesRange(int pageIndex) const noexcept;
        std::span<SearchMatch const> MatchesOnPage(int pageIndex) const noexcept;
        std::optional<std::size_t> ActiveMatchIndex() const noexcept { return m_activeMatchIndex; }
        SearchMatch const* ActiveMatch() const noexcept;

        // Invalidates any in-flight search and clears query, matches, and active selection.
        void Clear() noexcept;

        // Starts a new search request, invalidating any earlier request.
        // Blank queries clear matches and return to idle without issuing a request.
        std::optional<std::uint64_t> BeginSearch(std::wstring_view query);

        // Appends matches for one page under the active request while keeping document order
        // and the per-page index up to date.
        bool AppendPageMatches(std::uint64_t requestId, int pageIndex, std::vector<SearchMatch> matches);

        // Finishes the active request. Failed or cancelled completions discard partial matches.
        bool Complete(std::uint64_t requestId, bool completed, bool failed) noexcept;

        // Cancels an in-flight search (e.g. during tab transfer) while keeping ready results intact.
        void ResetPending() noexcept;

        // Selects the first match on or after currentPageIndex, falling back to the first match.
        std::optional<std::size_t> SelectInitialMatch(int currentPageIndex) noexcept;

        // Advances to the next or previous match with wrap-around across document boundaries.
        std::optional<std::size_t> NextMatch() noexcept;
        std::optional<std::size_t> PreviousMatch() noexcept;
        bool SetActiveMatch(std::size_t index) noexcept;

    private:
        void ClearMatches() noexcept;
        void RebuildPageIndex();

        SearchStatus m_state{ SearchStatus::idle };
        std::uint64_t m_requestId{ 0 };
        std::wstring m_query;
        std::vector<SearchMatch> m_matches;
        std::vector<PageMatchRange> m_pageRanges;
        std::optional<std::size_t> m_activeMatchIndex;
    };

    // Computes ScrollViewer target offsets to center/reveal a match rectangle on a page,
    // accounting for zoom, quarter-turn rotation (0/90/180/270), and measured layout geometry
    // (including two-column book view and unrealized pages).
    std::optional<ScrollOffsets> CalculateSearchMatchScrollOffsets(
        PdfPageBounds matchBounds,
        PdfPageBounds pageBounds,
        PageRect const& pageRect,
        ViewportGeometry const& viewport,
        int rotation) noexcept;

    std::optional<ScrollOffsets> CalculateSearchMatchScrollOffsets(
        SearchMatch const& match,
        std::span<PdfPageBounds const> pageBounds,
        std::span<PageRect const> pageRects,
        ViewportGeometry const& viewport,
        int rotation) noexcept;
}
