#include "PageImages.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace urpdf
{
    namespace
    {
        bool IsValidBounds(PdfPageBounds bounds) noexcept
        {
            return std::isfinite(bounds.x0) && std::isfinite(bounds.y0)
                && std::isfinite(bounds.x1) && std::isfinite(bounds.y1)
                && bounds.x0 < bounds.x1 && bounds.y0 < bounds.y1;
        }

        bool IsInvalidFileNameChar(wchar_t ch) noexcept
        {
            return ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"'
                || ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*';
        }

        bool EndsWithPdfExtension(std::wstring_view name) noexcept
        {
            if (name.size() < 4) { return false; }
            const auto suffix = name.substr(name.size() - 4);
            return suffix[0] == L'.'
                && std::towlower(suffix[1]) == L'p'
                && std::towlower(suffix[2]) == L'd'
                && std::towlower(suffix[3]) == L'f';
        }
    }

    std::optional<PdfPageBounds> TryClipImageRect(PdfPageBounds rect, PdfPageBounds page) noexcept
    {
        if (!IsValidBounds(rect) || !IsValidBounds(page)) { return std::nullopt; }
        const PdfPageBounds clipped{
            (std::max)(rect.x0, page.x0), (std::max)(rect.y0, page.y0),
            (std::min)(rect.x1, page.x1), (std::min)(rect.y1, page.y1) };
        return IsValidBounds(clipped) ? std::optional(clipped) : std::nullopt;
    }

    std::optional<std::uint32_t> HitTestImage(std::span<PageImage const> images, PdfPagePoint point) noexcept
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) { return std::nullopt; }
        for (auto it = images.rbegin(); it != images.rend(); ++it)
        {
            if (!IsValidBounds(it->bounds)) { continue; }
            if (it->bounds.x0 <= point.x && point.x <= it->bounds.x1
                && it->bounds.y0 <= point.y && point.y <= it->bounds.y1)
            {
                return it->index;
            }
        }
        return std::nullopt;
    }

    PageImage const* FindPageImage(std::span<PageImage const> images, std::uint32_t imageIndex) noexcept
    {
        for (auto const& image : images)
        {
            if (image.index == imageIndex) { return &image; }
        }
        return nullptr;
    }

    std::wstring SuggestedImageFileName(std::wstring_view documentName, int pageIndex, std::uint32_t imageIndex)
    {
        if (EndsWithPdfExtension(documentName))
        {
            documentName.remove_suffix(4);
        }

        std::wstring base;
        base.reserve(documentName.size());
        for (wchar_t ch : documentName)
        {
            if (IsInvalidFileNameChar(ch)) { base.push_back(L'_'); }
            else { base.push_back(ch); }
        }

        while (!base.empty() && (base.front() == L' ' || base.front() == L'.')) { base.erase(base.begin()); }
        while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) { base.pop_back(); }

        constexpr std::size_t maxBaseLength = 80;
        if (base.size() > maxBaseLength)
        {
            base.resize(maxBaseLength);
            while (!base.empty() && (base.back() == L' ' || base.back() == L'.')) { base.pop_back(); }
        }

        if (base.empty()) { base = L"image"; }

        const auto safePage = pageIndex < 0 ? 0 : pageIndex;
        return base + L"-page-" + std::to_wstring(safePage + 1) + L"-image-" + std::to_wstring(imageIndex + 1);
    }
}
