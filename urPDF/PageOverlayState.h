#pragma once

#include "PageCoordinateMapping.h"

#include <cstdint>
#include <optional>

namespace urpdf
{
    // UI-thread state. A token identifies one binding of a realized element;
    // callbacks from earlier bindings must not update its geometry.
    class PageOverlayState
    {
    public:
        struct Binding
        {
            std::uint64_t documentId{ 0 };
            int pageIndex{ -1 };
            std::uint64_t generation{ 0 };
            std::uint64_t revision{ 0 };
            bool operator==(Binding const&) const = default;
        };

        std::optional<Binding> Bind(std::uint64_t documentId, int pageIndex, std::uint64_t generation) noexcept
        {
            if (documentId == 0 || pageIndex < 0) { Clear(); return std::nullopt; }
            if (m_binding && m_binding->documentId == documentId && m_binding->pageIndex == pageIndex
                && m_binding->generation == generation) { return m_binding; }
            Clear();
            m_binding = Binding{ documentId, pageIndex, generation, m_revision };
            return m_binding;
        }

        void Clear() noexcept
        {
            ++m_revision;
            m_binding.reset();
            m_raster.reset();
            m_mapping.reset();
            m_displaySize = {};
        }

        bool IsCurrent(Binding binding) const noexcept { return m_binding && *m_binding == binding; }
        std::optional<Binding> CurrentBinding() const noexcept { return m_binding; }
        std::optional<PageCoordinateMapping> const& Mapping() const noexcept { return m_mapping; }
        ElementSize DisplaySize() const noexcept { return m_displaySize; }

        // Accept a snapshot only after its bitmap has passed the application's
        // render-generation checks and has been installed in this element.
        bool SetRaster(Binding binding, PageRasterGeometry const& raster, ElementSize size) noexcept
        {
            if (!IsCurrent(binding)) { return false; }
            m_raster = raster;
            m_displaySize = size;
            m_mapping = PageCoordinateMapping::TryCreate(raster, size);
            return true;
        }

        bool ClearRaster(Binding binding) noexcept
        {
            if (!IsCurrent(binding)) { return false; }
            m_raster.reset();
            m_mapping.reset();
            return true;
        }

        bool Resize(Binding binding, ElementSize size) noexcept
        {
            if (!IsCurrent(binding)) { return false; }
            if (size.width == m_displaySize.width && size.height == m_displaySize.height) { return true; }
            m_displaySize = size;
            m_mapping = m_raster ? PageCoordinateMapping::TryCreate(*m_raster, size) : std::nullopt;
            return true;
        }

    private:
        std::uint64_t m_revision{ 0 };
        std::optional<Binding> m_binding;
        std::optional<PageRasterGeometry> m_raster;
        std::optional<PageCoordinateMapping> m_mapping;
        ElementSize m_displaySize;
    };
}
