#pragma once

#include "PageCoordinateGeometry.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace urpdf
{
    struct PageImage
    {
        // Zero-based ordinal among valid image blocks on this page in MuPDF structured-text order.
        std::uint32_t index{ 0 };
        PdfPageBounds bounds;

        bool operator==(PageImage const& other) const noexcept
        {
            return index == other.index
                && bounds.x0 == other.bounds.x0 && bounds.y0 == other.bounds.y0
                && bounds.x1 == other.bounds.x1 && bounds.y1 == other.bounds.y1;
        }
    };

    struct PageImageSelection
    {
        int page{ 0 };
        std::uint32_t imageIndex{ 0 };

        bool operator==(PageImageSelection const&) const = default;
    };

    std::optional<PdfPageBounds> TryClipImageRect(PdfPageBounds rect, PdfPageBounds page) noexcept;

    // Iterates in reverse paint order so the topmost overlapping image wins.
    std::optional<std::uint32_t> HitTestImage(std::span<PageImage const> images, PdfPagePoint point) noexcept;

    PageImage const* FindPageImage(std::span<PageImage const> images, std::uint32_t imageIndex) noexcept;

    // Builds a sanitized base filename (without extension) for FileSavePicker.
    std::wstring SuggestedImageFileName(std::wstring_view documentName, int pageIndex, std::uint32_t imageIndex);
}
