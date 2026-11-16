#pragma once

#include "PageCoordinateGeometry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace urpdf
{
    enum class PageLinkKind { external, internal };

    struct PageLink
    {
        PdfPageBounds rect;
        PageLinkKind kind{ PageLinkKind::external };
        std::string uri;
        int targetPageIndex{ -1 };
    };

    std::optional<PdfPageBounds> TryClipLinkRect(PdfPageBounds rect, PdfPageBounds page) noexcept;

    enum class PageLinksState { notLoaded, loading, ready, failed };

    // UI-thread cache, independent of raster generations. Request IDs invalidate
    // completions belonging to an earlier window owner after a tab transfer.
    class PageLinksSlot
    {
    public:
        PageLinksState State() const noexcept { return m_state; }
        std::vector<PageLink> const& Links() const noexcept { return m_links; }

        std::optional<std::uint64_t> BeginLoad() noexcept
        {
            if (m_state != PageLinksState::notLoaded) { return std::nullopt; }
            m_state = PageLinksState::loading;
            return ++m_requestId;
        }

        bool Complete(std::uint64_t requestId, std::vector<PageLink> links, bool loaded, bool failed) noexcept
        {
            if (m_state != PageLinksState::loading || requestId != m_requestId) { return false; }
            m_links = std::move(links);
            m_state = failed ? PageLinksState::failed : loaded ? PageLinksState::ready : PageLinksState::notLoaded;
            return true;
        }

        void ResetPending() noexcept
        {
            if (m_state == PageLinksState::loading)
            {
                ++m_requestId;
                m_state = PageLinksState::notLoaded;
            }
        }

    private:
        PageLinksState m_state{ PageLinksState::notLoaded };
        std::uint64_t m_requestId{ 0 };
        std::vector<PageLink> m_links;
    };
}
