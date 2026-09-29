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
            return pageRects;
        }

        const uint32_t pageCount = m_pageSizes.Size();
        pageRects.resize(pageCount);

        if (pageCount == 0)
        {
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

        const double safeSpacing = (std::max)(0.0, m_spacing);
        const double columnWidth = m_columnCount == 2 ? (std::max)(0.0, (contentWidth - safeSpacing) / 2.0) : contentWidth;

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
                const double scale = columnWidth > 0.0 ? (std::min)(1.0, columnWidth / rawWidth) : 1.0;

                widths[column] = rawWidth * scale;
                heights[column] = rawHeight * scale;
                rowHeight = (std::max)(rowHeight, heights[column]);
            }

            for (uint32_t column = 0; rowStart + column < rowEnd; ++column)
            {
                const double columnX = m_columnCount == 2 ? column * (columnWidth + safeSpacing) : (contentWidth - widths[column]) / 2.0;
                const double x = m_columnCount == 2 ? columnX + (columnWidth - widths[column]) / 2.0 : columnX;

                pageRects[rowStart + column] = Rect{
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(widths[column]),
                    static_cast<float>(heights[column])
                };
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
        const float extentWidth = std::isfinite(availableSize.Width) && availableSize.Width > 0.0 ? availableSize.Width : (m_pageRects.empty() ? 0.0f : m_pageRects.front().Width);

        return Size{ extentWidth, extentHeight };
    }

    Size PdfPageLayout::ArrangeOverride(VirtualizingLayoutContext const& context, Size const& finalSize)
    {
        m_pageRects = CalculatePageRects(finalSize);
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
        return Size{ finalSize.Width, extentHeight };
    }
}
