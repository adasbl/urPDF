#include "pch.h"
#include "PdfPageLayout.h"
#include "PdfPageLayout.g.cpp"

#include <algorithm>
#include <array>
#include <cmath>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::urPDF::implementation
{
    int32_t PdfPageLayout::ColumnCount()
    {
        return m_columnCount;
    }

    void PdfPageLayout::ColumnCount(int32_t value)
    {
        value = (std::clamp)(value, 1, 2);
        if (m_columnCount != value)
        {
            m_columnCount = value;
            InvalidateMeasure();
        }
    }

    double PdfPageLayout::Spacing()
    {
        return m_spacing;
    }

    void PdfPageLayout::Spacing(double value)
    {
        if (!std::isfinite(value) || value < 0.0)
        {
            value = 0.0;
        }

        if (std::abs(m_spacing - value) > 0.001)
        {
            m_spacing = value;
            InvalidateMeasure();
        }
    }

    double PdfPageLayout::ViewportWidth()
    {
        return m_viewportWidth;
    }

    void PdfPageLayout::ViewportWidth(double value)
    {
        if (!std::isfinite(value) || value < 0.0)
        {
            value = 0.0;
        }

        if (std::abs(m_viewportWidth - value) > 0.001)
        {
            m_viewportWidth = value;
            InvalidateMeasure();
        }
    }

    double PdfPageLayout::ZoomFactor()
    {
        return m_zoomFactor;
    }

    void PdfPageLayout::ZoomFactor(double value)
    {
        if (!std::isfinite(value) || value <= 0.0)
        {
            value = 1.0;
        }

        if (std::abs(m_zoomFactor - value) > 0.001)
        {
            m_zoomFactor = value;
            InvalidateMeasure();
        }
    }

    Windows::Foundation::Collections::IVectorView<Size> PdfPageLayout::PageSizes()
    {
        return m_pageSizes;
    }

    void PdfPageLayout::PageSizes(
        Windows::Foundation::Collections::IVectorView<Size> const& value)
    {
        m_pageSizes = value;
        InvalidateMeasure();
    }

    std::vector<Rect> PdfPageLayout::CalculatePageRects(Size const& availableSize)
    {
        std::vector<Rect> pageRects;
        if (!m_pageSizes)
        {
            m_contentWidth = 0.0;
            return pageRects;
        }

        const uint32_t pageCount = m_pageSizes.Size();
        pageRects.resize(pageCount);

        if (pageCount == 0)
        {
            m_contentWidth = 0.0;
            return pageRects;
        }

        double contentWidth = availableSize.Width;
        if (!std::isfinite(contentWidth) || contentWidth <= 0.0)
        {
            contentWidth = 0.0;
            for (uint32_t i = 0; i < pageCount; ++i)
            {
                auto const size = m_pageSizes.GetAt(i);
                contentWidth = (std::max)(contentWidth, static_cast<double>(size.Width));
            }

            if (m_columnCount == 2)
            {
                for (uint32_t i = 0; i < pageCount; i += 2)
                {
                    const double firstWidth = m_pageSizes.GetAt(i).Width;
                    const double secondWidth = i + 1 < pageCount ? m_pageSizes.GetAt(i + 1).Width : 0.0;
                    contentWidth = (std::max)(contentWidth, firstWidth + secondWidth + m_spacing);
                }
            }
        }

        if (m_viewportWidth > 0.0 && m_zoomFactor > 0.0)
        {
            contentWidth = (std::max)(contentWidth, m_viewportWidth / m_zoomFactor);
        }

        m_contentWidth = contentWidth;

        const double safeSpacing = (std::max)(0.0, m_spacing);

        double y = 0.0;
        for (uint32_t rowStart = 0; rowStart < pageCount; rowStart += m_columnCount)
        {
            const uint32_t rowEnd = (std::min)(pageCount, rowStart + static_cast<uint32_t>(m_columnCount));
            double rowHeight = 0.0; 
            std::array<double, 2> widths{};
            std::array<double, 2> heights{};

            for (uint32_t column = 0; rowStart + column < rowEnd; ++column)
            {
                auto const size = m_pageSizes.GetAt(rowStart + column);
                const double rawWidth = (std::max)(1.0, static_cast<double>(size.Width));
                const double rawHeight = (std::max)(1.0, static_cast<double>(size.Height));
                widths[column] = rawWidth;
                heights[column] = rawHeight;
            }

            const bool isPagePair = rowEnd - rowStart == 2;
            const double rawPagesWidth = widths[0] + (isPagePair ? widths[1] : 0.0);
            const double availablePageWidth = isPagePair ? (std::max)(0.0, contentWidth - safeSpacing) : contentWidth;
            const double rowScale = rawPagesWidth > availablePageWidth && availablePageWidth > 0.0 ? availablePageWidth / rawPagesWidth : 1.0;

            for (uint32_t column = 0; rowStart + column < rowEnd; ++column)
            {
                widths[column] *= rowScale;
                heights[column] *= rowScale;
                rowHeight = (std::max)(rowHeight, heights[column]);
            }

            double x = isPagePair ? (contentWidth / 2.0) - (safeSpacing / 2.0) - widths[0] : (contentWidth - widths[0]) / 2.0;
            for (uint32_t column = 0; rowStart + column < rowEnd; ++column)
            {
                pageRects[rowStart + column] = Rect{
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(widths[column]),
                    static_cast<float>(heights[column])
                };

                x += widths[column] + safeSpacing;
            }

            y += rowHeight + safeSpacing;
        }

        return pageRects;
    }

    bool PdfPageLayout::IntersectsRealizationRect(Rect const& pageRect, Rect const& realizationRect)
    {
        if (!std::isfinite(realizationRect.Y) || !std::isfinite(realizationRect.Height) || realizationRect.Height <= 0.0)
        {
            return true;
        }

        const double pageBottom = pageRect.Y + pageRect.Height;
        const double realizationBottom = realizationRect.Y + realizationRect.Height;
        return pageBottom >= realizationRect.Y && pageRect.Y <= realizationBottom;
    }

    Size PdfPageLayout::MeasureOverride(VirtualizingLayoutContext const& context, Size const& availableSize)
    {
        m_pageRects = CalculatePageRects(availableSize);
        const Rect realizationRect = context.RealizationRect();
        const uint32_t itemCount = (std::min)(static_cast<uint32_t>(context.ItemCount()), static_cast<uint32_t>(m_pageRects.size()));

        for (uint32_t index = 0; index < itemCount; ++index)
        {
            const auto& pageRect = m_pageRects[index];
            if (!IntersectsRealizationRect(pageRect, realizationRect))
            {
                continue;
            }

            auto element = context.GetOrCreateElementAt(index);
            element.Measure(Size{ pageRect.Width, pageRect.Height });
        }

        const float extentHeight = m_pageRects.empty() ? 0.0f : m_pageRects.back().Y + m_pageRects.back().Height;
        const float extentWidth = static_cast<float>(m_contentWidth);

        return Size{ extentWidth, extentHeight };
    }

    Size PdfPageLayout::ArrangeOverride(VirtualizingLayoutContext const& context, Size const& finalSize)
    {
        Size layoutSize = finalSize;
        if (m_contentWidth > 0.0 && (!std::isfinite(layoutSize.Width) || layoutSize.Width < m_contentWidth))
        {
            layoutSize.Width = static_cast<float>(m_contentWidth);
        }

        m_pageRects = CalculatePageRects(layoutSize);
        const Rect realizationRect = context.RealizationRect();
        const uint32_t itemCount = (std::min)(static_cast<uint32_t>(context.ItemCount()), static_cast<uint32_t>(m_pageRects.size()));

        for (uint32_t index = 0; index < itemCount; ++index)
        {
            const auto& pageRect = m_pageRects[index];
            if (!IntersectsRealizationRect(pageRect, realizationRect))
            {
                continue;
            }

            auto element = context.GetOrCreateElementAt(index);
            element.Arrange(pageRect);
        }

        const float extentHeight = m_pageRects.empty() ? 0.0f : m_pageRects.back().Y + m_pageRects.back().Height;
        return Size{ layoutSize.Width, extentHeight };
    }
}
