#include "PageSearch.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <utility>

namespace urpdf
{
    namespace
    {
        bool IsPositiveFinite(double value) noexcept
        {
            return std::isfinite(value) && value > 0.0;
        }

        bool IsValidBounds(PdfPageBounds bounds) noexcept
        {
            return std::isfinite(bounds.x0) && std::isfinite(bounds.y0)
                && std::isfinite(bounds.x1) && std::isfinite(bounds.y1)
                && bounds.x0 < bounds.x1 && bounds.y0 < bounds.y1;
        }

        bool IsValidPageRect(PageRect const& page) noexcept
        {
            return std::isfinite(page.x) && std::isfinite(page.y)
                && IsPositiveFinite(page.width) && IsPositiveFinite(page.height);
        }

        bool IsValidViewport(ViewportGeometry const& viewport) noexcept
        {
            return IsPositiveFinite(viewport.zoom)
                && IsPositiveFinite(viewport.width)
                && IsPositiveFinite(viewport.height)
                && std::isfinite(viewport.offsetX)
                && std::isfinite(viewport.offsetY)
                && std::isfinite(viewport.contentX)
                && std::isfinite(viewport.contentY)
                && std::isfinite(viewport.scrollableWidth) && viewport.scrollableWidth >= 0.0
                && std::isfinite(viewport.scrollableHeight) && viewport.scrollableHeight >= 0.0;
        }

        std::optional<int> NormalizeQuarterTurnRotation(int rotation) noexcept
        {
            if (rotation % 90 != 0) { return std::nullopt; }
            int normalized = rotation % 360;
            if (normalized < 0) { normalized += 360; }
            return normalized;
        }

        bool BoundsEqual(PdfPageBounds const& a, PdfPageBounds const& b) noexcept
        {
            return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
        }

        double AxisScrollOffset(double matchStart, double matchEnd, double viewportLength, double scrollableLength) noexcept
        {
            const double matchLength = matchEnd - matchStart;
            const double desired = matchLength <= viewportLength
                ? (matchStart + matchEnd) * 0.5 - viewportLength * 0.5
                : matchStart;
            return std::clamp(desired, 0.0, scrollableLength);
        }
    }

    bool SearchMatch::operator==(SearchMatch const& other) const noexcept
    {
        if (pageIndex != other.pageIndex || rects.size() != other.rects.size()) { return false; }
        for (std::size_t i = 0; i < rects.size(); ++i)
        {
            if (!BoundsEqual(rects[i], other.rects[i])) { return false; }
        }
        return true;
    }

    bool IsBlankSearchQuery(std::wstring_view query) noexcept
    {
        return std::all_of(query.begin(), query.end(), [](wchar_t ch) noexcept
            {
                return std::iswspace(ch) != 0;
            });
    }

    std::optional<PdfPageBounds> TryClipSearchRect(PdfPageBounds rect, PdfPageBounds page) noexcept
    {
        if (!IsValidBounds(rect) || !IsValidBounds(page)) { return std::nullopt; }
        const PdfPageBounds clipped{
            (std::max)(rect.x0, page.x0), (std::max)(rect.y0, page.y0),
            (std::min)(rect.x1, page.x1), (std::min)(rect.y1, page.y1) };
        return IsValidBounds(clipped) ? std::optional(clipped) : std::nullopt;
    }

    std::optional<PdfPageBounds> TrySearchMatchBounds(std::span<PdfPageBounds const> rects, PdfPageBounds page) noexcept
    {
        if (!IsValidBounds(page) || rects.empty()) { return std::nullopt; }

        std::optional<PdfPageBounds> combined;
        for (auto const& rect : rects)
        {
            const auto clipped = TryClipSearchRect(rect, page);
            if (!clipped) { continue; }
            if (!combined)
            {
                combined = *clipped;
            }
            else
            {
                combined->x0 = (std::min)(combined->x0, clipped->x0);
                combined->y0 = (std::min)(combined->y0, clipped->y0);
                combined->x1 = (std::max)(combined->x1, clipped->x1);
                combined->y1 = (std::max)(combined->y1, clipped->y1);
            }
        }
        return combined;
    }

    PageMatchRange DocumentSearchState::PageMatchesRange(int pageIndex) const noexcept
    {
        if (pageIndex < 0) { return {}; }
        const auto index = static_cast<std::size_t>(pageIndex);
        if (index >= m_pageRanges.size()) { return {}; }
        return m_pageRanges[index];
    }

    std::span<SearchMatch const> DocumentSearchState::MatchesOnPage(int pageIndex) const noexcept
    {
        const auto range = PageMatchesRange(pageIndex);
        if (range.Empty() || range.end > m_matches.size()) { return {}; }
        return std::span<SearchMatch const>(m_matches.data() + range.begin, range.Count());
    }

    SearchMatch const* DocumentSearchState::ActiveMatch() const noexcept
    {
        if (!m_activeMatchIndex || *m_activeMatchIndex >= m_matches.size()) { return nullptr; }
        return &m_matches[*m_activeMatchIndex];
    }

    void DocumentSearchState::ClearMatches() noexcept
    {
        m_matches.clear();
        m_pageRanges.clear();
        m_activeMatchIndex.reset();
    }

    void DocumentSearchState::RebuildPageIndex()
    {
        m_pageRanges.clear();
        if (m_matches.empty()) { return; }

        const auto maxPage = static_cast<std::size_t>(m_matches.back().pageIndex);
        m_pageRanges.resize(maxPage + 1);

        std::size_t i = 0;
        while (i < m_matches.size())
        {
            const int page = m_matches[i].pageIndex;
            const std::size_t begin = i;
            while (i < m_matches.size() && m_matches[i].pageIndex == page)
            {
                ++i;
            }
            m_pageRanges[static_cast<std::size_t>(page)] = PageMatchRange{ begin, i };
        }
    }

    void DocumentSearchState::Clear() noexcept
    {
        ++m_requestId;
        m_state = SearchStatus::idle;
        m_query.clear();
        ClearMatches();
    }

    std::optional<std::uint64_t> DocumentSearchState::BeginSearch(std::wstring_view query)
    {
        ++m_requestId;
        ClearMatches();
        if (IsBlankSearchQuery(query))
        {
            m_query.clear();
            m_state = SearchStatus::idle;
            return std::nullopt;
        }

        m_query.assign(query);
        m_state = SearchStatus::searching;
        return m_requestId;
    }

    bool DocumentSearchState::AppendPageMatches(std::uint64_t requestId, int pageIndex, std::vector<SearchMatch> matches)
    {
        if (m_state != SearchStatus::searching || requestId != m_requestId || pageIndex < 0)
        {
            return false;
        }

        std::vector<SearchMatch> validMatches;
        validMatches.reserve(matches.size());
        for (auto& match : matches)
        {
            std::vector<PdfPageBounds> validRects;
            validRects.reserve(match.rects.size());
            for (auto const& rect : match.rects)
            {
                if (IsValidBounds(rect))
                {
                    validRects.push_back(rect);
                }
            }
            if (!validRects.empty())
            {
                validMatches.push_back(SearchMatch{ pageIndex, std::move(validRects) });
            }
        }

        if (validMatches.empty())
        {
            return true;
        }

        const auto pageSlot = static_cast<std::size_t>(pageIndex);
        if (m_matches.empty() || pageIndex >= m_matches.back().pageIndex)
        {
            if (m_pageRanges.size() <= pageSlot)
            {
                m_pageRanges.resize(pageSlot + 1);
            }
            auto& range = m_pageRanges[pageSlot];
            if (range.Empty())
            {
                range.begin = m_matches.size();
            }
            m_matches.reserve(m_matches.size() + validMatches.size());
            for (auto& match : validMatches)
            {
                m_matches.push_back(std::move(match));
            }
            range.end = m_matches.size();
        }
        else
        {
            const auto insertIt = std::upper_bound(m_matches.begin(), m_matches.end(), pageIndex,
                [](int targetPage, SearchMatch const& existing) noexcept
                {
                    return targetPage < existing.pageIndex;
                });
            const auto insertOffset = static_cast<std::size_t>(insertIt - m_matches.begin());
            const auto addedCount = validMatches.size();
            m_matches.insert(insertIt,
                std::make_move_iterator(validMatches.begin()),
                std::make_move_iterator(validMatches.end()));
            if (m_activeMatchIndex && *m_activeMatchIndex >= insertOffset)
            {
                *m_activeMatchIndex += addedCount;
            }
            RebuildPageIndex();
        }

        return true;
    }

    bool DocumentSearchState::Complete(std::uint64_t requestId, bool completed, bool failed) noexcept
    {
        if (m_state != SearchStatus::searching || requestId != m_requestId)
        {
            return false;
        }

        if (failed)
        {
            ClearMatches();
            m_state = SearchStatus::failed;
        }
        else if (completed)
        {
            m_state = SearchStatus::ready;
        }
        else
        {
            ClearMatches();
            m_state = SearchStatus::idle;
        }
        return true;
    }

    void DocumentSearchState::ResetPending() noexcept
    {
        if (m_state == SearchStatus::searching)
        {
            ++m_requestId;
            ClearMatches();
            m_state = SearchStatus::idle;
        }
    }

    std::optional<std::size_t> DocumentSearchState::SelectInitialMatch(int currentPageIndex) noexcept
    {
        if (m_matches.empty())
        {
            m_activeMatchIndex.reset();
            return std::nullopt;
        }

        const auto it = std::lower_bound(m_matches.begin(), m_matches.end(), currentPageIndex,
            [](SearchMatch const& match, int targetPage) noexcept
            {
                return match.pageIndex < targetPage;
            });
        m_activeMatchIndex = it != m_matches.end()
            ? static_cast<std::size_t>(it - m_matches.begin())
            : std::size_t{ 0 };
        return m_activeMatchIndex;
    }

    std::optional<std::size_t> DocumentSearchState::NextMatch() noexcept
    {
        if (m_matches.empty())
        {
            m_activeMatchIndex.reset();
            return std::nullopt;
        }

        if (!m_activeMatchIndex || *m_activeMatchIndex >= m_matches.size())
        {
            m_activeMatchIndex = 0;
        }
        else
        {
            m_activeMatchIndex = (*m_activeMatchIndex + 1) % m_matches.size();
        }
        return m_activeMatchIndex;
    }

    std::optional<std::size_t> DocumentSearchState::PreviousMatch() noexcept
    {
        if (m_matches.empty())
        {
            m_activeMatchIndex.reset();
            return std::nullopt;
        }

        if (!m_activeMatchIndex || *m_activeMatchIndex >= m_matches.size())
        {
            m_activeMatchIndex = m_matches.size() - 1;
        }
        else
        {
            m_activeMatchIndex = *m_activeMatchIndex == 0
                ? m_matches.size() - 1
                : *m_activeMatchIndex - 1;
        }
        return m_activeMatchIndex;
    }

    bool DocumentSearchState::SetActiveMatch(std::size_t index) noexcept
    {
        if (index >= m_matches.size()) { return false; }
        m_activeMatchIndex = index;
        return true;
    }

    std::optional<ScrollOffsets> CalculateSearchMatchScrollOffsets(
        PdfPageBounds matchBounds,
        PdfPageBounds pageBounds,
        PageRect const& pageRect,
        ViewportGeometry const& viewport,
        int rotation) noexcept
    {
        const auto normalizedRotation = NormalizeQuarterTurnRotation(rotation);
        if (!normalizedRotation || !IsValidPageRect(pageRect) || !IsValidViewport(viewport))
        {
            return std::nullopt;
        }

        const auto clipped = TryClipSearchRect(matchBounds, pageBounds);
        if (!clipped) { return std::nullopt; }

        const double pageWidth = pageBounds.x1 - pageBounds.x0;
        const double pageHeight = pageBounds.y1 - pageBounds.y0;
        if (!IsPositiveFinite(pageWidth) || !IsPositiveFinite(pageHeight)) { return std::nullopt; }

        const double u0 = (clipped->x0 - pageBounds.x0) / pageWidth;
        const double u1 = (clipped->x1 - pageBounds.x0) / pageWidth;
        const double v0 = (clipped->y0 - pageBounds.y0) / pageHeight;
        const double v1 = (clipped->y1 - pageBounds.y0) / pageHeight;

        double ru0 = 0.0;
        double ru1 = 0.0;
        double rv0 = 0.0;
        double rv1 = 0.0;
        switch (*normalizedRotation)
        {
        case 0:
            ru0 = u0;
            ru1 = u1;
            rv0 = v0;
            rv1 = v1;
            break;
        case 90:
            ru0 = 1.0 - v1;
            ru1 = 1.0 - v0;
            rv0 = u0;
            rv1 = u1;
            break;
        case 180:
            ru0 = 1.0 - u1;
            ru1 = 1.0 - u0;
            rv0 = 1.0 - v1;
            rv1 = 1.0 - v0;
            break;
        case 270:
            ru0 = v0;
            ru1 = v1;
            rv0 = 1.0 - u1;
            rv1 = 1.0 - u0;
            break;
        default:
            return std::nullopt;
        }

        const double layoutX0 = pageRect.x + ru0 * pageRect.width;
        const double layoutX1 = pageRect.x + ru1 * pageRect.width;
        const double layoutY0 = pageRect.y + rv0 * pageRect.height;
        const double layoutY1 = pageRect.y + rv1 * pageRect.height;

        const double scrollX0 = (layoutX0 + viewport.contentX) * viewport.zoom;
        const double scrollX1 = (layoutX1 + viewport.contentX) * viewport.zoom;
        const double scrollY0 = (layoutY0 + viewport.contentY) * viewport.zoom;
        const double scrollY1 = (layoutY1 + viewport.contentY) * viewport.zoom;

        if (!std::isfinite(scrollX0) || !std::isfinite(scrollX1)
            || !std::isfinite(scrollY0) || !std::isfinite(scrollY1))
        {
            return std::nullopt;
        }

        return ScrollOffsets{
            AxisScrollOffset(scrollX0, scrollX1, viewport.width, viewport.scrollableWidth),
            AxisScrollOffset(scrollY0, scrollY1, viewport.height, viewport.scrollableHeight) };
    }

    std::optional<ScrollOffsets> CalculateSearchMatchScrollOffsets(
        SearchMatch const& match,
        std::span<PdfPageBounds const> pageBounds,
        std::span<PageRect const> pageRects,
        ViewportGeometry const& viewport,
        int rotation) noexcept
    {
        if (match.pageIndex < 0) { return std::nullopt; }
        const auto index = static_cast<std::size_t>(match.pageIndex);
        if (index >= pageBounds.size() || index >= pageRects.size()) { return std::nullopt; }

        const auto combinedBounds = TrySearchMatchBounds(match.rects, pageBounds[index]);
        if (!combinedBounds) { return std::nullopt; }

        return CalculateSearchMatchScrollOffsets(*combinedBounds, pageBounds[index], pageRects[index], viewport, rotation);
    }
}
