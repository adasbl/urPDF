#include "pch.h"

// Corresponding
#include "MainWindow.xaml.h"
#include "DocumentOperation.h"
#include "MuPdfDocument.h"
#include "ExternalUriPolicy.h"
#include "LinkHitArea.h"
#include "TextSelectionLayer.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

// Project
#include "AppSettings.h"
#include "WindowPlacement.h"
#include "App.xaml.h"
#include "PdfRenderScale.h"
#include "SettingsPage.h"
#include "AboutPage.h"

// External libraries
extern "C" {
#include <mupdf/fitz.h>
}

// Standard C++
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

// Windows SDK / WinRT
#include <shobjidl_core.h>
#include <dwmapi.h>
#include <MemoryBuffer.h>
#include <printmanagerinterop.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.DragDrop.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Graphics.Printing.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Printing.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#pragma comment(lib, "dwmapi.lib")


namespace xaml        = winrt::Microsoft::UI::Xaml;
namespace controls    = winrt::Microsoft::UI::Xaml::Controls;
namespace media       = winrt::Microsoft::UI::Xaml::Media;
namespace imaging     = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace dispatching = winrt::Microsoft::UI::Dispatching;
namespace backdrops   = winrt::Microsoft::UI::Composition::SystemBackdrops;

namespace foundation = winrt::Windows::Foundation;
namespace graphics   = winrt::Windows::Graphics::Imaging;
namespace printing   = winrt::Windows::Graphics::Printing;
namespace storage    = winrt::Windows::Storage;
namespace pickers    = winrt::Windows::Storage::Pickers;

namespace
{
    constexpr int PrefetchPageCount = 2;
    constexpr int SlotRetainMarginPages = 3;
    constexpr auto ZoomRenderDebounce = std::chrono::milliseconds(200);
    constexpr auto DpiRenderDebounce = std::chrono::milliseconds(200);
    constexpr auto SearchDebounce = std::chrono::milliseconds(200);
    constexpr std::size_t SearchPublishBatchPages = 8;
    constexpr wchar_t TabDragProperty[] = L"urPDF.Tab";

    bool NeedsPageText(urpdf::PageTextSlot const& slot) noexcept
    {
        const auto state = slot.State();
        return state == urpdf::PageTextState::notLoaded || state == urpdf::PageTextState::loading;
    }

    struct TabDragKeyboardState
    {
        HHOOK hook{};
        bool cancelled{};
        controls::TabViewItem tab{ nullptr };
        void Stop()
        {
            if (hook) { UnhookWindowsHookEx(std::exchange(hook, nullptr)); }
        }
        ~TabDragKeyboardState() { Stop(); }
    };
    thread_local TabDragKeyboardState TabDragKeyboard;

    LRESULT CALLBACK TrackTabDragKey(int code, WPARAM key, LPARAM state)
    {
        if (code >= 0 && key == VK_ESCAPE && !(state & 0x80000000)) { TabDragKeyboard.cancelled = true; }
        return CallNextHookEx(nullptr, code, key, state);
    }

    struct EmptyTabContent : winrt::implements<EmptyTabContent, foundation::IStringable>
    {
        winrt::hstring ToString() const { return {}; }
    };

    controls::TabViewItem CreateTabItem()
    {
        controls::TabViewItem tab;
        tab.Content(winrt::make<EmptyTabContent>());
        return tab;
    }

    controls::TabViewItem DraggedTab(xaml::DragEventArgs const& args)
    {
        auto properties = args.DataView().Properties();
        return properties.HasKey(TabDragProperty) ? TabDragKeyboard.tab : nullptr;
    }

    bool IsTabListDrag(xaml::DragEventArgs const& args)
    {
        auto element = args.OriginalSource().try_as<xaml::DependencyObject>();
        while (element)
        {
            if (element.try_as<controls::Primitives::TabViewListView>()) { return true; }
            element = media::VisualTreeHelper::GetParent(element);
        }
        return false;
    }

    template<typename Element>
    Element FindVisualDescendant(xaml::DependencyObject const& parent, wchar_t const* name)
    {
        for (int32_t i = 0; i < media::VisualTreeHelper::GetChildrenCount(parent); ++i)
        {
            auto child = media::VisualTreeHelper::GetChild(parent, i);
            if (auto element = child.try_as<Element>(); element && element.Name() == name) { return element; }
            if (auto element = FindVisualDescendant<Element>(child, name)) { return element; }
        }
        return nullptr;
    }

    std::filesystem::path ExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        while (true)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0) { winrt::throw_last_error(); }
            if (length < path.size())
            {
                path.resize(length);
                return std::filesystem::path{ path }.parent_path();
            }
            path.resize(path.size() * 2);
        }
    }

    bool IsPdfFile(storage::StorageFile const& file)
    {
        std::wstring fileType{ file.FileType() };
        std::transform(fileType.begin(), fileType.end(), fileType.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
        return fileType == L".pdf";
    }

    winrt::hstring CurrentOpenErrorMessage()
    {
        try { throw; }
        catch (winrt::hresult_error const& error) { return error.message(); }
        catch (std::exception const& error) { return winrt::to_hstring(error.what()); }
        catch (...) { return L"The document could not be opened."; }
    }

    template<typename Work, typename Publish>
    void QueueMuPdfWork(urpdf::BackgroundWorkTracker::Registration registration, Work work, Publish publish)
    {
        struct Job
        {
            urpdf::BackgroundWorkTracker::Registration registration;
            std::optional<Work> work;
            Publish publish;
        };

        auto job = std::make_shared<Job>(Job{ std::move(registration), std::move(work), std::move(publish) });
        winrt::Windows::System::Threading::ThreadPool::RunAsync([job](foundation::IAsyncAction const&)
            {
                urpdf::ExecuteTrackedWork(std::move(job->registration), job->work, job->publish);
            });
    }

    template<typename Callback>
    bool TryPublishResult(dispatching::DispatcherQueue const& dispatcher,
        std::shared_ptr<std::atomic<bool>> const& cancelled, Callback callback) noexcept
    {
        if (cancelled->load(std::memory_order_acquire)) { return false; }
        try
        {
            if (dispatcher.TryEnqueue(std::move(callback))) { return true; }
        }
        catch (...) {}

        cancelled->store(true, std::memory_order_release);
        return false;
    }

    struct DocumentLayoutResult
    {
        int pagecount{ 0 };
        std::vector<fz_rect> pageBounds;
    };

    struct PageRenderResult
    {
        std::optional<winrt::urPDF::implementation::SoftwarePageBitmap> bitmap;
        winrt::urPDF::implementation::PageRenderMetrics metrics;
        bool rendered{ false };
    };

    struct PageLinksResult
    {
        std::vector<urpdf::PageLink> links;
        bool loaded{ false };
    };

    struct PageTextResult
    {
        urpdf::PageText text;
        bool loaded{ false };
    };

    struct DocumentSearchResult
    {
        bool completed{ false };
    };

    winrt::fire_and_forget LaunchExternalUriAsync(std::string utf8Uri)
    {
        try
        {
            const auto checked = urpdf::TryGetLaunchableExternalUri(utf8Uri);
            if (!checked) { co_return; }
            foundation::Uri uri{ winrt::to_hstring(*checked) };
            if (!urpdf::IsAllowedExternalScheme(winrt::to_string(uri.SchemeName()))) { co_return; }
            co_await winrt::Windows::System::Launcher::LaunchUriAsync(uri);
        }
        catch (...)
        {
#ifdef _DEBUG
            OutputDebugStringW(L"urPDF: external link could not be opened.\n");
#endif
        }
    }

    struct PageElementParts
    {
        controls::Border border{ nullptr };
        controls::Grid content{ nullptr };
        controls::Image image{ nullptr };
        winrt::urPDF::TextSelectionLayer selection{ nullptr };
        controls::Canvas imageAreas{ nullptr };
        controls::Canvas searchHighlights{ nullptr };
        controls::Canvas highlights{ nullptr };
        controls::Canvas overlay{ nullptr };
    };

    std::optional<PageElementParts> TryGetPageElementParts(xaml::UIElement const& element)
    {
        auto border = element ? element.try_as<controls::Border>() : nullptr;
        auto content = border ? border.Child().try_as<controls::Grid>() : nullptr;
        if (!content || content.Children().Size() != 3) { return std::nullopt; }

        // Keep the template's child order in one place; Image is replaced in
        // this slot without replacing the Grid or the overlay above it.
        auto image = content.Children().GetAt(0).try_as<controls::Image>();
        auto selection = content.Children().GetAt(1).try_as<winrt::urPDF::TextSelectionLayer>();
        auto overlay = content.Children().GetAt(2).try_as<controls::Canvas>();
        const bool hasSelectionCanvases = selection && selection.Children().Size() == 3;
        auto imageAreas = hasSelectionCanvases ? selection.Children().GetAt(0).try_as<controls::Canvas>() : nullptr;
        auto searchHighlights = hasSelectionCanvases ? selection.Children().GetAt(1).try_as<controls::Canvas>() : nullptr;
        auto highlights = hasSelectionCanvases ? selection.Children().GetAt(2).try_as<controls::Canvas>() : nullptr;
        if (!image || !imageAreas || !searchHighlights || !highlights || !overlay) { return std::nullopt; }
        return PageElementParts{ border, content, image, selection, imageAreas, searchHighlights, highlights, overlay };
    }

    xaml::UIElement GetPageElementForTextSelectionLayer(winrt::urPDF::TextSelectionLayer const& layer)
    {
        auto content = layer.Parent().try_as<controls::Grid>();
        return content ? content.Parent().try_as<controls::Border>() : nullptr;
    }

    bool IsPointerOverPageText(winrt::urPDF::implementation::PageOverlayElement const& entry,
        winrt::urPDF::TextSelectionLayer const& layer, xaml::Input::PointerRoutedEventArgs const& args)
    {
        const auto binding = entry.state.CurrentBinding();
        auto const& mapping = entry.state.Mapping();
        auto document = entry.document.lock();
        if (!binding || !mapping || !document || binding->pageIndex < 0
            || static_cast<std::size_t>(binding->pageIndex) >= document->m_pageText.size()) { return false; }
        auto const& slot = document->m_pageText[binding->pageIndex];
        if (slot.State() != urpdf::PageTextState::ready) { return false; }

        const auto position = args.GetCurrentPoint(layer).Position();
        const urpdf::ElementPoint point{ position.X, position.Y };
        if (mapping->IsElementPointInsidePage(point) != true) { return false; }
        const auto pdfPoint = mapping->ElementToPdfPoint(point);
        return pdfPoint && urpdf::IsOverText(slot.Text(), *pdfPoint);
    }

    void UpdatePageTextCursor(winrt::urPDF::TextSelectionLayer const& layer,
        winrt::urPDF::implementation::PageOverlayElement const* entry, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (args.Pointer().PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Touch) { return; }
        winrt::get_self<winrt::urPDF::implementation::TextSelectionLayer>(layer)->SetTextCursor(
            entry && IsPointerOverPageText(*entry, layer, args));
    }

    void UpdatePageOverlayClip(PageElementParts const& parts, std::optional<urpdf::PageCoordinateMapping> const& mapping = std::nullopt)
    {
        foundation::Rect bounds{};
        if (mapping)
        {
            const auto rect = mapping->BitmapRect();
            bounds = { static_cast<float>(rect.x), static_cast<float>(rect.y),
                static_cast<float>(rect.width), static_cast<float>(rect.height) };
        }
        else
        {
            winrt::get_self<winrt::urPDF::implementation::TextSelectionLayer>(parts.selection)->SetTextCursor(false);
        }
        parts.selection.Clip().as<media::RectangleGeometry>().Rect(bounds);
        parts.overlay.Clip().as<media::RectangleGeometry>().Rect(bounds);
    }

    void ClearPageOverlay(PageElementParts const& parts, winrt::urPDF::implementation::PageOverlayElement* entry)
    {
        parts.imageAreas.Children().Clear();
        parts.searchHighlights.Children().Clear();
        parts.highlights.Children().Clear();
        parts.overlay.Children().Clear();
        if (entry)
        {
            entry->linkAreasBuilt = false;
            entry->imageAreasBuilt = false;
            entry->searchHighlightsBuilt = false;
            entry->selectionBuilt = false;
        }
    }

    void ClearPageElement(PageElementParts const& parts, winrt::urPDF::implementation::PageOverlayElement* entry = nullptr)
    {
        parts.image.Source(nullptr);
        ClearPageOverlay(parts, entry);
        UpdatePageOverlayClip(parts);
        parts.border.Opacity(0.65);
    }

    void DrawPageImageAreas(controls::Canvas const& canvas, urpdf::PageCoordinateMapping const& mapping,
        winrt::urPDF::implementation::PdfTabInfo const& document, int32_t pageIndex)
    {
        auto children = canvas.Children();
        if (pageIndex < 0
            || static_cast<std::size_t>(pageIndex) >= document.m_pageText.size()
            || document.m_pageText[pageIndex].State() != urpdf::PageTextState::ready)
        {
            children.Clear();
            return;
        }

        auto const& images = document.m_pageText[pageIndex].Text().images;
        if (images.empty())
        {
            children.Clear();
            return;
        }

        const auto borderStyle = canvas.Resources().Lookup(winrt::box_value(L"PageImageSelectionBorderStyle")).as<xaml::Style>();
        const auto fillStyle = canvas.Resources().Lookup(winrt::box_value(L"PageImageSelectionFillStyle")).as<xaml::Style>();
        std::uint32_t count = 0;
        for (auto const& image : images)
        {
            const auto rect = mapping.PdfToElementRect(image.bounds);
            if (!rect) { continue; }

            auto area = count < children.Size()
                ? children.GetAt(count).as<controls::Border>() : nullptr;
            if (!area)
            {
                area = controls::Border();
                children.Append(area);
            }
            area.Width(rect->width);
            area.Height(rect->height);
            controls::Canvas::SetLeft(area, rect->x);
            controls::Canvas::SetTop(area, rect->y);
            xaml::Automation::AutomationProperties::SetName(area, L"Image " + winrt::to_hstring(image.index + 1));

            const bool selected = document.m_selectedImage
                && document.m_selectedImage->page == pageIndex
                && document.m_selectedImage->imageIndex == image.index;
            if (selected)
            {
                area.Style(borderStyle);
                if (!area.Child())
                {
                    xaml::Shapes::Rectangle fill;
                    fill.Style(fillStyle);
                    area.Child(fill);
                }
            }
            else
            {
                area.Style(nullptr);
                area.BorderThickness({ 0.0, 0.0, 0.0, 0.0 });
                area.BorderBrush(nullptr);
                area.Child(nullptr);
            }
            ++count;
        }
        while (children.Size() > count) { children.RemoveAtEnd(); }
    }

    void DrawPageSearchMatches(controls::Canvas const& canvas, urpdf::PageCoordinateMapping const& mapping,
        winrt::urPDF::implementation::PdfTabInfo const& document, int32_t pageIndex)
    {
        auto children = canvas.Children();
        const auto range = document.m_search.PageMatchesRange(pageIndex);
        if (range.Empty())
        {
            children.Clear();
            return;
        }

        const auto matchStyle = canvas.Resources().Lookup(winrt::box_value(L"PageSearchMatchRectangleStyle")).as<xaml::Style>();
        const auto activeStyle = canvas.Resources().Lookup(winrt::box_value(L"PageActiveSearchMatchRectangleStyle")).as<xaml::Style>();
        auto const& matches = document.m_search.Matches();
        const auto activeIndex = document.m_search.ActiveMatchIndex();
        const std::size_t end = (std::min)(range.end, matches.size());

        std::uint32_t count = 0;
        for (std::size_t matchIndex = range.begin; matchIndex < end; ++matchIndex)
        {
            const bool isActive = activeIndex && *activeIndex == matchIndex;
            auto const& style = isActive ? activeStyle : matchStyle;
            for (auto const& bounds : matches[matchIndex].rects)
            {
                const auto rect = mapping.PdfToElementRect(bounds);
                if (!rect) { continue; }

                auto highlight = count < children.Size()
                    ? children.GetAt(count).as<xaml::Shapes::Rectangle>() : nullptr;
                if (!highlight)
                {
                    highlight = xaml::Shapes::Rectangle();
                    children.Append(highlight);
                }
                if (highlight.Style() != style)
                {
                    highlight.Style(style);
                }
                highlight.Width(rect->width);
                highlight.Height(rect->height);
                controls::Canvas::SetLeft(highlight, rect->x);
                controls::Canvas::SetTop(highlight, rect->y);
                ++count;
            }
        }
        while (children.Size() > count) { children.RemoveAtEnd(); }
    }

    void DrawPageSelection(controls::Canvas const& canvas, urpdf::PageCoordinateMapping const& mapping,
        winrt::urPDF::implementation::PdfTabInfo const& document, int32_t pageIndex)
    {
        auto children = canvas.Children();
        auto const& selection = document.m_textSelection;
        const auto carets = selection ? urpdf::PageSelectionCarets(*selection, pageIndex) : std::nullopt;
        if (!carets || pageIndex < 0
            || static_cast<std::size_t>(pageIndex) >= document.m_pageText.size()
            || document.m_pageText[pageIndex].State() != urpdf::PageTextState::ready)
        {
            children.Clear();
            return;
        }

        const auto style = canvas.Resources().Lookup(winrt::box_value(L"PageSelectionRectangleStyle")).as<xaml::Style>();
        auto const& text = document.m_pageText[pageIndex].Text();
        std::uint32_t count = 0;
        for (auto const& bounds : urpdf::SelectionRects(text, carets->first, carets->second))
        {
            const auto rect = mapping.PdfToElementRect(bounds);
            if (!rect) { continue; }

            auto highlight = count < children.Size()
                ? children.GetAt(count).as<xaml::Shapes::Rectangle>() : nullptr;
            if (!highlight)
            {
                highlight = xaml::Shapes::Rectangle();
                highlight.Style(style);
                children.Append(highlight);
            }
            highlight.Width(rect->width);
            highlight.Height(rect->height);
            controls::Canvas::SetLeft(highlight, rect->x);
            controls::Canvas::SetTop(highlight, rect->y);
            ++count;
        }
        while (children.Size() > count) { children.RemoveAtEnd(); }
    }

    constexpr double PdfRenderScale(double zoom, double dpiScale) noexcept
    {
        return urpdf::DipPerPdfPoint * zoom * dpiScale * urpdf::RenderSupersampling;
    }

    int NormalizeRotation(int rotation) noexcept
    {
        rotation %= 360;
        return rotation < 0 ? rotation + 360 : rotation;
    }

    void ZoomAroundViewportCenter(controls::ScrollViewer const& scrollViewer, float targetZoom)
    {
        const double currentZoom = scrollViewer.ZoomFactor();
        const double halfViewportWidth = scrollViewer.ViewportWidth() / 2.0;
        const double halfViewportHeight = scrollViewer.ViewportHeight() / 2.0;
        const double centerX = (scrollViewer.HorizontalOffset() + halfViewportWidth) / currentZoom;
        const double centerY = (scrollViewer.VerticalOffset() + halfViewportHeight) / currentZoom;

        scrollViewer.ChangeView( (std::max)(0.0, centerX * targetZoom - halfViewportWidth), (std::max)(0.0, centerY * targetZoom - halfViewportHeight), targetZoom);
    }

    std::size_t BitmapMemoryBytes(imaging::WriteableBitmap const& bitmap) noexcept
    {
        constexpr std::uint64_t bytesPerPixel = 4;
        const std::uint64_t bytes = static_cast<std::uint64_t>(bitmap.PixelWidth()) * static_cast<std::uint64_t>(bitmap.PixelHeight()) * bytesPerPixel;

        if (bytes > (std::numeric_limits<std::size_t>::max)())
        {
            return (std::numeric_limits<std::size_t>::max)();
        }

        return static_cast<std::size_t>(bytes);
    }

    bool ValidPageBounds(fz_rect bounds) noexcept
    {
        return std::isfinite(bounds.x0) && std::isfinite(bounds.y0) && std::isfinite(bounds.x1) && std::isfinite(bounds.y1) && bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0 && !fz_is_infinite_rect(bounds);
    }

    bool TryPageRasterBounds(fz_rect pageBounds, int rotation, int bucket, fz_matrix& transform, fz_irect& bbox, bool allowEmpty = false)
    {
        if (!ValidPageBounds(pageBounds)) { return false; }
        const double scale = urpdf::RenderScaleForBucket(bucket);
        if (scale <= 0.0 || scale > (std::numeric_limits<float>::max)()) { return false; }
        const float rasterScale = static_cast<float>(scale);
        if (rasterScale == 0.0f) { return allowEmpty; }
        transform = fz_concat(fz_scale(rasterScale, rasterScale), fz_rotate(NormalizeRotation(rotation)));
        const fz_rect rect = fz_transform_rect(pageBounds, transform);
        for (float coordinate : { rect.x0, rect.y0, rect.x1, rect.y1 })
        {
            if (!std::isfinite(coordinate) || coordinate <= static_cast<float>(FZ_MIN_INF_RECT) || coordinate >= static_cast<float>(FZ_MAX_INF_RECT)) { return false; }
        }
        bbox = fz_round_rect(rect);
        if (bbox.x1 == bbox.x0) { ++bbox.x1; }
        if (bbox.y1 == bbox.y0) { ++bbox.y1; }
        const std::int64_t width = static_cast<std::int64_t>(bbox.x1) - bbox.x0;
        const std::int64_t height = static_cast<std::int64_t>(bbox.y1) - bbox.y0;
        std::size_t bufferBytes = 0;
        return urpdf::TryBitmapBufferSize(width, height, bufferBytes);
    }

    int LimitedPageRenderScaleBucket(fz_rect pageBounds, int rotation, double requestedScale)
    {
        if (!ValidPageBounds(pageBounds)) { return 0; }
        const int bucket = urpdf::SelectLimitedRenderScaleBucket(urpdf::RenderScaleBucket(requestedScale), [&](int candidate)
            {
                fz_matrix transform;
                fz_irect bbox;
                return TryPageRasterBounds(pageBounds, rotation, candidate, transform, bbox, true);
            });
        fz_matrix transform;
        fz_irect bbox;
        return TryPageRasterBounds(pageBounds, rotation, bucket, transform, bbox) ? bucket : 0;
    }

    fz_pixmap* RenderPagePixmap(fz_context* ctx, fz_document* doc, int pageIndex, fz_rect pageBounds, int bucket, int rotation,
        urpdf::PageRasterGeometry& geometry)
    {
        fz_matrix transform;
        fz_irect bbox;
        if (!TryPageRasterBounds(pageBounds, rotation, bucket, transform, bbox)) { return nullptr; }
        fz_page* page = nullptr;
        fz_device* dev = nullptr;
        fz_pixmap* pix = nullptr;

        fz_var(page);
        fz_var(dev);
        fz_var(pix);

        fz_try(ctx)
        {
            page = fz_load_page(ctx, doc, pageIndex);
            pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox, nullptr, 1);
            fz_clear_pixmap_with_value(ctx, pix, 255);
            dev = fz_new_draw_device(ctx, fz_identity, pix);
            fz_run_page(ctx, page, dev, transform, nullptr);
            fz_close_device(ctx, dev);
        }
        fz_always(ctx)
        {
            fz_drop_device(ctx, dev);
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx)
        {
            fz_drop_pixmap(ctx, pix);
            fz_report_error(ctx);
            pix = nullptr;
        }

        if (pix)
        {
            geometry = { { pageBounds.x0, pageBounds.y0, pageBounds.x1, pageBounds.y1 },
                { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f },
                { bbox.x0, bbox.y0, bbox.x1, bbox.y1 },
                { fz_pixmap_width(ctx, pix), fz_pixmap_height(ctx, pix) } };
        }
        return pix;
    }

    std::optional<winrt::urPDF::implementation::SoftwarePageBitmap> RenderPageToSoftwareBitmap(
        fz_context* ctx, fz_document* doc, int pageIndex, fz_rect pageBounds, int renderScaleBucket, int rotation)
    {
        const double renderScale = urpdf::RenderScaleForBucket(renderScaleBucket);
        if (renderScale <= 0.0) { return std::nullopt; }

        graphics::SoftwareBitmap bitmap{ nullptr };
        urpdf::PageRasterGeometry geometry;
        fz_pixmap* pix = RenderPagePixmap(ctx, doc, pageIndex, pageBounds, renderScaleBucket, rotation, geometry);
        if (pix)
        {
            const int width = fz_pixmap_width(ctx, pix);
            const int height = fz_pixmap_height(ctx, pix);
            const std::size_t sourceStride = static_cast<std::size_t>(fz_pixmap_stride(ctx, pix));
            const unsigned char* samples = fz_pixmap_samples(ctx, pix);

            try
            {
                bitmap = graphics::SoftwareBitmap(graphics::BitmapPixelFormat::Bgra8, width, height, graphics::BitmapAlphaMode::Premultiplied);
                graphics::BitmapBuffer buffer = bitmap.LockBuffer(graphics::BitmapBufferAccessMode::Write);
                const graphics::BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
                foundation::IMemoryBufferReference reference = buffer.CreateReference();
                uint8_t* data = reference.data();

                const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
                for (int y = 0; y < height; ++y)
                {
                    memcpy(data + plane.StartIndex + static_cast<std::size_t>(y) * plane.Stride, samples + static_cast<std::size_t>(y) * sourceStride, rowBytes);
                }
            }
            catch (...)
            {
                bitmap = nullptr;
            }

            fz_drop_pixmap(ctx, pix);
        }

        if (!bitmap) { return std::nullopt; }
        return winrt::urPDF::implementation::SoftwarePageBitmap{ bitmap, geometry };
    }
}


namespace winrt::urPDF::implementation
{
    // MAINWINDOW
    MainWindow::~MainWindow()
    {
        ShutdownMuPdfWork();
    }

    void MainWindow::ShutdownMuPdfWork() noexcept
    {
        if (m_isClosed) { return; }
        m_isClosed = true;
        try { ClearPageOverlays(); } catch (...) { m_pageOverlays.clear(); }
        try { StopTrackingWindowState(); } catch (...) {}
        if (m_fullscreenExitHideTimer) { m_fullscreenExitHideTimer.Stop(); }
        if (m_documentViewPointerMovedHandler)
        {
            try { DocumentViewHost().RemoveHandler(xaml::UIElement::PointerMovedEvent(), winrt::box_value(m_documentViewPointerMovedHandler)); } catch (...) {}
            m_documentViewPointerMovedHandler = nullptr;
        }
        m_firstFrameRenderedRevoker.revoke();
        CloseTransferredSourceWindow();
        if (m_moveSizeInput)
        {
            if (m_enteredMoveSizeToken.value) { m_moveSizeInput.EnteredMoveSize(m_enteredMoveSizeToken); }
            if (m_exitedMoveSizeToken.value) { m_moveSizeInput.ExitedMoveSize(m_exitedMoveSizeToken); }
            m_moveSizeInput = nullptr;
        }
        m_workCancelled->store(true, std::memory_order_release);
        CancelPendingSearchDebounce();
        for (auto const& tabEntry : m_openedTabs)
        {
            CancelActiveSearchSession(tabEntry.second);
            ++tabEntry.second->m_renderGeneration;
            for (auto& slot : tabEntry.second->m_renderedPages) { slot.bitmap.reset(); }
        }
        if (m_loadingTab)
        {
            CancelActiveSearchSession(m_loadingTab);
            ++m_loadingTab->m_renderGeneration;
        }
        m_loadingTab.reset();
        m_loadingTargetTab.reset();
        m_openedTabs.clear();
        m_pendingOpens.clear();
        m_pageTextLoadQueue.clear();
        m_pendingSelectionCopy.reset();
        m_ctx.reset();
        m_runtime.reset();
        try { if (m_openErrorDialog) { m_openErrorDialog.Hide(); } } catch (...) {}
        CancelPendingViewStateSave();
        try { DetachDpiObserver(); } catch (...) {}
        try { CancelPendingZoomRender(); } catch (...) {}
        m_printDocument = nullptr;
        if (m_printManager && m_printTaskRequestedToken)
        {
            try { m_printManager.PrintTaskRequested(m_printTaskRequestedToken); } catch (...) {}
            m_printTaskRequestedToken = {};
        }
        m_printManager = nullptr;
        m_pageBitmapCache = PageBitmapCache{ 256ull * 1024ull * 1024ull };
    }

    MainWindow::MainWindow()
        : MainWindow(urpdf::WindowCreationOptions{})
    {
    }

    MainWindow::MainWindow(urpdf::WindowCreationOptions const& options)
        : m_runtime(xaml::Application::Current().as<App>()->MuPdfRuntime()),
          m_ctx(m_runtime->CloneContext()),
          m_createInitialEmptyTab(options.createInitialEmptyTab)
    {
        if (options.file || !options.filePath.empty()) { m_pendingOpens.push_back({ options }); }
        InitializeComponent();

        m_zoomRenderTimer = DispatcherQueue().CreateTimer();
        m_zoomRenderTimer.IsRepeating(false);
        m_zoomRenderTimer.Interval(ZoomRenderDebounce);
        m_zoomRenderTimer.Tick([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->m_zoomDebounceElapsed = true;
                self->ApplyPendingRenderZoom();
            }
        });

        m_dpiRenderTimer = DispatcherQueue().CreateTimer();
        m_dpiRenderTimer.IsRepeating(false);
        m_dpiRenderTimer.Interval(DpiRenderDebounce);
        m_dpiRenderTimer.Tick([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->ApplyPendingDpiRefresh();
            }
        });

        m_pageTextScrollTimer = DispatcherQueue().CreateTimer();
        m_pageTextScrollTimer.Interval(std::chrono::milliseconds(20));
        m_pageTextScrollTimer.IsRepeating(true);
        m_pageTextScrollTimer.Tick([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get()) { self->ScrollPageTextDragAtEdge(); }
        });

        m_searchDebounceTimer = DispatcherQueue().CreateTimer();
        m_searchDebounceTimer.IsRepeating(false);
        m_searchDebounceTimer.Interval(SearchDebounce);
        m_searchDebounceTimer.Tick([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get()) { self->ExecutePendingDebouncedSearch(); }
        });

        m_fullscreenExitHideTimer = DispatcherQueue().CreateTimer();
        m_fullscreenExitHideTimer.IsRepeating(false);
        m_fullscreenExitHideTimer.Interval(std::chrono::milliseconds(600));
        m_fullscreenExitHideTimer.Tick([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get(); self && !self->m_isClosed)
            {
                if (!self->m_isPointerOverFullscreenExitButton && !self->IsFocusInsideElement(self->FullscreenExitButton()))
                {
                    self->FullscreenExitButton().Visibility(xaml::Visibility::Collapsed);
                }
            }
        });

        m_documentViewPointerMovedHandler = xaml::Input::PointerEventHandler(
            [weakThis = get_weak()](auto const&, xaml::Input::PointerRoutedEventArgs const& args)
            {
                if (auto self = weakThis.get(); self && !self->m_isClosed && self->IsFullscreen())
                {
                    self->HandleDocumentViewPointerMoved(args.GetCurrentPoint(self->DocumentViewHost()).Position());
                }
            });
        DocumentViewHost().AddHandler(xaml::UIElement::PointerMovedEvent(),
            winrt::box_value(m_documentViewPointerMovedHandler), true);
        DocumentViewHost().PointerExited([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get(); self && !self->m_isClosed && self->IsFullscreen())
            {
                self->ScheduleFullscreenExitButtonHide();
            }
        });
        FullscreenExitButton().LostFocus([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get(); self && !self->m_isClosed && self->IsFullscreen())
            {
                self->ScheduleFullscreenExitButtonHide();
            }
        });

        FitPageSplitButton().Loaded([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get(); self && !self->m_isClosed)
            {
                self->SyncFitPageSplitButtonVisuals();
            }
        });
        FitPageSplitButton().IsEnabledChanged([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get(); self && !self->m_isClosed)
            {
                self->SyncFitPageSplitButtonVisuals();
                self->DispatcherQueue().TryEnqueue([weakThis]()
                {
                    if (auto delayedSelf = weakThis.get(); delayedSelf && !delayedSelf->m_isClosed)
                    {
                        delayedSelf->SyncFitPageSplitButtonVisuals();
                    }
                });
            }
        });

        PdfScrollViewer().Loaded([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->AttachDpiObserver();
            }
        });

        PdfScrollViewer().Unloaded([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                if (self->m_pageTextDrag) { self->EndPageTextDrag(); }
                self->DetachDpiObserver();
            }
        });

        PdfScrollViewer().LayoutUpdated([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->QueuePendingViewStateSave();
            }
        });

        Closed([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->SaveWindowState();
                self->ShutdownMuPdfWork();
            }
        });

        Activated([weakThis = get_weak()](auto const&, xaml::WindowActivatedEventArgs const& args)
        {
            auto self = weakThis.get();
            if (!self || self->m_isClosed) { return; }
            if (args.WindowActivationState() == xaml::WindowActivationState::Deactivated)
            {
                self->m_lastPageTextClick.reset();
                if (self->m_pageTextDrag) { self->EndPageTextDrag(); }
                if (self->IsFullscreen()) { self->ScheduleFullscreenExitButtonHide(); }
                return;
            }
            xaml::Application::Current().as<App>()->WindowActivated(*self);
        });

        AppWindow().SetIcon((ExecutableDirectory() / L"Assets" / L"logo.ico").wstring());

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

        AppTitleBar().LayoutUpdated([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->UpdateTitleBarPassthroughRegions();
            }
        });

        LoadSettings();

        if (!options.deferPresentation) { RestoreWindowState(); }
        TrackWindowState();
    }


    void MainWindow::RestoreWindowState()
    {
        const auto settings = urpdf::AppSettings::Instance().Get();
        m_windowState = settings.windowState;

        auto nativeWindow = try_as<::IWindowNative>();
        HWND hwnd{};
        if (!nativeWindow || FAILED(nativeWindow->get_WindowHandle(&hwnd))) { return; }

        RECT defaultBounds{};
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetWindowRect(hwnd, &defaultBounds)) { return; }
        HMONITOR monitor = settings.windowState.normalPosition
            ? urpdf::FindWindowMonitor(settings.windowState.normalPosition->monitorDevice) : nullptr;
        if (!monitor) { monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST); }
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            monitorInfo.rcWork = defaultBounds;
        }

        const auto placement = urpdf::ResolveWindowStartupPlacement(settings, defaultBounds, monitorInfo.rcWork);
        auto appWindow = AppWindow();
        auto presenter = appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
        if (!presenter) { return; }
        if (presenter.State() != winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Restored)
        {
            presenter.Restore(false);
        }

        const auto& bounds = placement.normalBounds;
        appWindow.MoveAndResize({ static_cast<int32_t>(bounds.left), static_cast<int32_t>(bounds.top),
            static_cast<int32_t>(bounds.right - bounds.left), static_cast<int32_t>(bounds.bottom - bounds.top) });
        const auto size = appWindow.Size();
        m_windowState.normalSize = urpdf::WindowSize{ size.Width, size.Height };
        m_windowState.maximized = placement.maximized;
        if (placement.maximized) { presenter.Maximize(); }
    }

    void MainWindow::TrackWindowState()
    {
        StopTrackingWindowState();
        CaptureWindowState();

        m_observedAppWindow = AppWindow();
        m_windowChangedToken = m_observedAppWindow.Changed(
            [weakThis = get_weak()](auto const&, winrt::Microsoft::UI::Windowing::AppWindowChangedEventArgs const& args)
            {
                if (auto self = weakThis.get(); self && !self->m_isClosed)
                {
                    if (args.DidPresenterChange())
                    {
                        self->HandleAppWindowPresenterChanged();
                    }
                    self->CaptureWindowState();
                }
            });

        m_windowClosingToken = m_observedAppWindow.Closing(
            [weakThis = get_weak()](auto const&, auto const&)
            {
                if (auto self = weakThis.get())
                {
                    self->CaptureWindowState();
                    self->SaveWindowState();
                }
            });
    }

    void MainWindow::StopTrackingWindowState() noexcept
    {
        if (m_observedAppWindow)
        {
            if (m_windowChangedToken)
            {
                m_observedAppWindow.Changed(std::exchange(m_windowChangedToken, {}));
            }
            if (m_windowClosingToken)
            {
                m_observedAppWindow.Closing(std::exchange(m_windowClosingToken, {}));
            }
            m_observedAppWindow = nullptr;
        }
    }

    void MainWindow::CaptureWindowState()
    {
        if (m_isClosed || !m_fullscreenTracker.ShouldCaptureWindowState(IsAppWindowFullscreen())) { return; }

        auto nativeWindow = try_as<::IWindowNative>();
        HWND hwnd{};
        if (!nativeWindow || FAILED(nativeWindow->get_WindowHandle(&hwnd))) { return; }

        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(hwnd, &placement))
        {
            urpdf::UpdateWindowStateFromPlacement(m_windowState, placement);
            MONITORINFOEXW monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                const bool toolWindow = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
                urpdf::UpdateWindowPositionFromPlacement(m_windowState, placement, monitor, toolWindow);
            }
        }
    }

    void MainWindow::SaveWindowState()
    {
        auto& settings = urpdf::AppSettings::Instance();
        if (settings.Get().windowState != m_windowState)
        {
            settings.SetWindowState(m_windowState);
        }
    }

    bool MainWindow::IsAppWindowFullscreen()
    {
        if (m_isClosed) { return false; }
        auto appWindow = AppWindow();
        return appWindow && appWindow.Presenter()
            && appWindow.Presenter().Kind() == winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen;
    }

    bool MainWindow::IsFullscreen()
    {
        return m_fullscreenTracker.active || IsAppWindowFullscreen();
    }

    bool MainWindow::CanEnterFullscreen()
    {
        if (m_isClosed
            || MainWindowUI().Visibility() != xaml::Visibility::Visible
            || SettingsOverlay().Visibility() == xaml::Visibility::Visible
            || RootFrame().Visibility() == xaml::Visibility::Visible)
        {
            return false;
        }
        auto document = SelectedPdfDocument();
        return document && document->m_layoutLoaded;
    }

    void MainWindow::EnterFullscreen()
    {
        if (!CanEnterFullscreen() || IsFullscreen()) { return; }

        CaptureWindowState();
        bool wasMaximized = m_windowState.maximized;
        if (auto overlapped = AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            wasMaximized = overlapped.State() == winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Maximized;
        }

        m_fullscreenTracker.BeginEnter(wasMaximized);
        try
        {
            AppWindow().SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen);
        }
        catch (...)
        {
            m_fullscreenTracker.CompleteEnter(IsAppWindowFullscreen());
            ApplyFullscreenChrome(m_fullscreenTracker.active);
            return;
        }

        m_fullscreenTracker.CompleteEnter(IsAppWindowFullscreen());
        ApplyFullscreenChrome(m_fullscreenTracker.active);
        if (m_fullscreenTracker.active && !IsFocusInsideElement(SearchBar()))
        {
            PdfScrollViewer().Focus(xaml::FocusState::Programmatic);
        }
    }

    void MainWindow::ExitFullscreen()
    {
        if (m_isClosed || !IsFullscreen()) { return; }

        const bool restoreMaximized = m_fullscreenTracker.BeginExit();
        try
        {
            if (IsAppWindowFullscreen())
            {
                AppWindow().SetPresenter(winrt::Microsoft::UI::Windowing::AppWindowPresenterKind::Overlapped);
            }
            if (restoreMaximized)
            {
                if (auto overlapped = AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
                {
                    if (overlapped.State() != winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Maximized)
                    {
                        overlapped.Maximize();
                    }
                }
            }
        }
        catch (...)
        {
            m_fullscreenTracker.CompleteExit(IsAppWindowFullscreen());
            ApplyFullscreenChrome(m_fullscreenTracker.active);
            return;
        }

        m_fullscreenTracker.CompleteExit(IsAppWindowFullscreen());
        ApplyFullscreenChrome(m_fullscreenTracker.active);
        CaptureWindowState();
    }

    void MainWindow::ToggleFullscreen()
    {
        if (IsFullscreen())
        {
            ExitFullscreen();
        }
        else if (CanEnterFullscreen())
        {
            EnterFullscreen();
        }
    }

    void MainWindow::HandleAppWindowPresenterChanged()
    {
        if (m_isClosed) { return; }
        const bool appWindowFullscreen = IsAppWindowFullscreen();
        bool chromeChanged = false;
        const bool restoreMaximized = m_fullscreenTracker.SyncExternalPresenter(appWindowFullscreen, chromeChanged);
        if (restoreMaximized)
        {
            m_fullscreenTracker.transitioning = true;
            try
            {
                if (auto overlapped = AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
                {
                    if (overlapped.State() != winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Maximized)
                    {
                        overlapped.Maximize();
                    }
                }
            }
            catch (...) {}
            m_fullscreenTracker.transitioning = false;
        }

        if (chromeChanged)
        {
            ApplyFullscreenChrome(m_fullscreenTracker.active);
        }
        else if (!m_fullscreenTracker.active && !appWindowFullscreen)
        {
            UpdateTitleBarPassthroughRegions();
        }
    }

    void MainWindow::ApplyFullscreenChrome(bool fullscreen)
    {
        if (m_isClosed) { return; }

        HideFullscreenExitButton();
        if (fullscreen)
        {
            TitleBarRow().Height(xaml::GridLength{ 0.0, xaml::GridUnitType::Pixel });
            AppTitleBar().Visibility(xaml::Visibility::Collapsed);
            MainToolbar().Visibility(xaml::Visibility::Collapsed);
            MainContentBorder().CornerRadius(xaml::CornerRadius{ 0.0, 0.0, 0.0, 0.0 });
            MainContentBorder().BorderThickness(xaml::Thickness{ 0.0, 0.0, 0.0, 0.0 });
        }
        else
        {
            TitleBarRow().Height(xaml::GridLength{ 40.0, xaml::GridUnitType::Pixel });
            AppTitleBar().Visibility(xaml::Visibility::Visible);
            MainToolbar().Visibility(xaml::Visibility::Visible);
            MainContentBorder().CornerRadius(xaml::CornerRadius{ 8.0, 8.0, 0.0, 0.0 });
            MainContentBorder().BorderThickness(xaml::Thickness{ 1.0, 0.0, 1.0, 0.0 });
            SetTitleBar(AppTitleBar());
        }

        UpdateTitleBarPassthroughRegions();
        if (!fullscreen)
        {
            DispatcherQueue().TryEnqueue([weakThis = get_weak()]()
            {
                if (auto self = weakThis.get(); self && !self->m_isClosed)
                {
                    self->UpdateTitleBarPassthroughRegions();
                }
            });
        }
    }

    void MainWindow::ShowFullscreenExitButton()
    {
        if (m_isClosed || !IsFullscreen()) { return; }
        if (m_fullscreenExitHideTimer) { m_fullscreenExitHideTimer.Stop(); }
        FullscreenExitButton().Visibility(xaml::Visibility::Visible);
    }

    void MainWindow::ScheduleFullscreenExitButtonHide()
    {
        if (m_isClosed || m_isPointerOverFullscreenExitButton || IsFocusInsideElement(FullscreenExitButton())) { return; }
        if (FullscreenExitButton().Visibility() != xaml::Visibility::Visible) { return; }
        if (m_fullscreenExitHideTimer && !m_fullscreenExitHideTimer.IsRunning())
        {
            m_fullscreenExitHideTimer.Start();
        }
    }

    void MainWindow::HideFullscreenExitButton()
    {
        m_isPointerOverFullscreenExitButton = false;
        if (m_fullscreenExitHideTimer) { m_fullscreenExitHideTimer.Stop(); }
        if (!m_isClosed)
        {
            FullscreenExitButton().Visibility(xaml::Visibility::Collapsed);
        }
    }

    void MainWindow::HandleDocumentViewPointerMoved(foundation::Point position)
    {
        constexpr double topTriggerHeight = 32.0;
        constexpr double topKeepHeight = 72.0;

        if (m_pageTextDrag) { return; }
        if (position.Y <= topTriggerHeight)
        {
            ShowFullscreenExitButton();
            return;
        }
        if (m_isPointerOverFullscreenExitButton || position.Y <= topKeepHeight)
        {
            if (FullscreenExitButton().Visibility() == xaml::Visibility::Visible && m_fullscreenExitHideTimer)
            {
                m_fullscreenExitHideTimer.Stop();
            }
            return;
        }
        ScheduleFullscreenExitButtonHide();
    }

    void MainWindow::ToggleFullscreen_Invoked(xaml::Input::KeyboardAccelerator const&,
        xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (m_isClosed
            || SettingsOverlay().Visibility() == xaml::Visibility::Visible
            || RootFrame().Visibility() == xaml::Visibility::Visible)
        {
            return;
        }

        if (IsFullscreen())
        {
            ExitFullscreen();
            args.Handled(true);
        }
        else if (CanEnterFullscreen())
        {
            EnterFullscreen();
            args.Handled(true);
        }
    }

    void MainWindow::FullscreenButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        ToggleFullscreen();
    }

    void MainWindow::FullscreenExitButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        ExitFullscreen();
    }

    void MainWindow::FullscreenExitButton_PointerEntered(foundation::IInspectable const&, xaml::Input::PointerRoutedEventArgs const&)
    {
        m_isPointerOverFullscreenExitButton = true;
        if (m_fullscreenExitHideTimer) { m_fullscreenExitHideTimer.Stop(); }
    }

    void MainWindow::FullscreenExitButton_PointerExited(foundation::IInspectable const&, xaml::Input::PointerRoutedEventArgs const&)
    {
        m_isPointerOverFullscreenExitButton = false;
        if (IsFullscreen())
        {
            ScheduleFullscreenExitButtonHide();
        }
    }


    // CASHE
    PageBitmapCache::PageBitmapCache(std::size_t budgetBytes)
        : m_budgetBytes(budgetBytes)
    {
    }

    std::optional<PageBitmap> PageBitmapCache::TryGetBitmap(PageBitmapCacheKey const& key)
    {
        auto cacheEntry = m_entries.find(key);
        if (cacheEntry == m_entries.end())
        {
            ++m_metrics.misses;
            return std::nullopt;
        }

        ++m_metrics.hits;
        MarkAsRecentlyUsed(cacheEntry);
        return cacheEntry->second.bitmap;
    }

    void PageBitmapCache::StoreBitmap(PageBitmapCacheKey const& key, PageBitmap const& bitmap)
    {
        const std::size_t memoryBytes = BitmapMemoryBytes(bitmap.source);
        if (memoryBytes > m_budgetBytes)
        {
            return;
        }

        auto existingEntry = m_entries.find(key);
        if (existingEntry != m_entries.end())
        {
            m_usedBytes -= existingEntry->second.memoryBytes;
            m_lru.erase(existingEntry->second.lruPosition);
            m_entries.erase(existingEntry);
        }

        m_lru.push_front(key);
        CacheEntry cacheEntry{ bitmap, memoryBytes, m_lru.begin() };
        m_entries.emplace(key, std::move(cacheEntry));
        m_usedBytes += memoryBytes;
        TrimToBudget();
    }

    void PageBitmapCache::ClearDocumentEntries(std::uint64_t documentId)
    {
        for (auto cacheEntry = m_entries.begin(); cacheEntry != m_entries.end();)
        {
            if (cacheEntry->first.documentId != documentId)
            {
                ++cacheEntry;
                continue;
            }

            m_usedBytes -= cacheEntry->second.memoryBytes;
            m_lru.erase(cacheEntry->second.lruPosition);
            cacheEntry = m_entries.erase(cacheEntry);
        }
    }

    void PageBitmapCache::MarkAsRecentlyUsed(std::unordered_map<PageBitmapCacheKey, CacheEntry, PageBitmapCacheKeyHash>::iterator cacheEntry)
    {
        m_lru.splice(m_lru.begin(), m_lru, cacheEntry->second.lruPosition);
        cacheEntry->second.lruPosition = m_lru.begin();
    }

    void PageBitmapCache::SetViewContext(PageBitmapCacheViewContext const& context)
    {
        m_viewContext = context;
        m_hasViewContext = true;
    }

    void PageBitmapCache::TrimToBudget()
    {
        while (m_usedBytes > m_budgetBytes && !m_lru.empty())
        {
            if (!TryEvictOneEntry())
            {
                break;
            }
        }
    }

    void PageBitmapCache::EvictEntry(PageBitmapCacheKey const& key)
    {
        const PageBitmapCacheKey keyCopy = key;
        auto cacheEntry = m_entries.find(keyCopy);
        if (cacheEntry == m_entries.end())
        {
            return;
        }

        m_usedBytes -= cacheEntry->second.memoryBytes;
        m_lru.erase(cacheEntry->second.lruPosition);
        m_entries.erase(cacheEntry);
    }

    bool PageBitmapCache::TryEvictOneEntry()
    {
        bool hasFarthest = false;
        int farthestDistance = -1;
        PageBitmapCacheKey farthestKey;

        for (auto entry = m_lru.rbegin(); entry != m_lru.rend(); ++entry)
        {
            const PageBitmapCacheKey& key = *entry;
            const bool isRetained = key.pageIndex >= m_viewContext.firstPage && key.pageIndex <= m_viewContext.lastPage;
            const auto scaleIndex = static_cast<std::size_t>(key.pageIndex - m_viewContext.firstPage);
            const bool matchesScale = isRetained && scaleIndex < m_viewContext.renderScaleBuckets.size()
                && key.renderScaleBucket == m_viewContext.renderScaleBuckets[scaleIndex];

            if (!m_hasViewContext || key.documentId != m_viewContext.documentId || key.rotation != m_viewContext.rotation || (isRetained && !matchesScale))
            {
                EvictEntry(key);
                return true;
            }

            if (isRetained)
            {
                continue;
            }

            const int distance = key.pageIndex < m_viewContext.firstPage
                ? m_viewContext.firstPage - key.pageIndex
                : key.pageIndex - m_viewContext.lastPage;

            if (distance > farthestDistance)
            {
                farthestDistance = distance;
                farthestKey = key;
                hasFarthest = true;
            }
        }

        if (!hasFarthest)
        {
            return false;
        }

        EvictEntry(farthestKey);
        return true;
    }


    // PDFTABINFO
    PdfTabInfo::PdfTabInfo(std::shared_ptr<urpdf::MuPdfRuntime> runtime, std::string documentPath)
        : m_runtime(std::move(runtime)), m_ctx(m_runtime->CloneContext()), m_documentPath(std::move(documentPath))
    {
    }

    void PdfTabInfo::OpenDocument()
    {
        if (!m_doc) { m_doc = urpdf::OpenPdfDocument(m_ctx.get(), m_documentPath.c_str()); }
    }

    PdfTabInfo::~PdfTabInfo()
    {
        if (m_searchCancelled) { m_searchCancelled->store(true, std::memory_order_release); }
        fz_drop_document(m_ctx.get(), m_doc);
    }


    // XAML DATA
    winrt::event_token MainWindow::PropertyChanged(xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }

    void MainWindow::PropertyChanged(winrt::event_token const& token) noexcept
    {
        m_propertyChanged.remove(token);
    }

    int32_t MainWindow::DisplayPageNumber()
    {
        return m_pageNumber;
    }

    void MainWindow::DisplayPageNumber(int32_t value)
    {
        if (m_pageNumber != value)
        {
            m_pageNumber = value;
            m_propertyChanged(*this, xaml::Data::PropertyChangedEventArgs{ L"DisplayPageNumber" });
        }
    }

    int32_t MainWindow::DisplayPageCount()
    {
        return m_pageCounter;
    }

    void MainWindow::DisplayPageCount(int32_t value)
    {
        if (m_pageCounter != value)
        {
            m_pageCounter = value;
            m_propertyChanged(*this, xaml::Data::PropertyChangedEventArgs{ L"DisplayPageCount" });
        }
    }


    // SETTINGS 
    void MainWindow::SettingsButton_Click(
        foundation::IInspectable const&,
        xaml::RoutedEventArgs const&)
    {
        winrt::urPDF::SettingsPage settingsPage;

        settingsPage.BackRequested([this](auto const&, auto const&)
        {
            SettingsFrame().Content(nullptr);
            SettingsOverlay().Visibility(xaml::Visibility::Collapsed);
            MainWindowUI().IsHitTestVisible(true);
        });

        auto applySettings = [](auto const&, auto const&)
        {
            xaml::Application::Current().as<App>()->ApplySettingsToWindows();
        };
        settingsPage.ThemeChanged(applySettings);
        settingsPage.BackdropChanged(applySettings);

        SettingsFrame().Content(settingsPage);
        SettingsOverlay().Visibility(xaml::Visibility::Visible);
        MainWindowUI().IsHitTestVisible(false);
    }

    void MainWindow::CloseSettings_Click(
        foundation::IInspectable const&,
        xaml::RoutedEventArgs const&)
    {
        SettingsFrame().Content(nullptr);
        SettingsOverlay().Visibility(xaml::Visibility::Collapsed);
        MainWindowUI().IsHitTestVisible(true);
    }

    void MainWindow::LoadSettings()
    {
        if (m_isClosed) { return; }
        auto settings = urpdf::AppSettings::Instance().Get();

        ApplyTheme(winrt::hstring{ urpdf::ToString(settings.theme) });
        ApplyBackdrop(winrt::hstring{ urpdf::ToString(settings.backdrop) });
        if (auto page = SettingsFrame().Content().try_as<winrt::urPDF::SettingsPage>())
        {
            winrt::get_self<SettingsPage>(page)->LoadSavedSettings();
        }
    }

    void MainWindow::ApplyBackdrop(winrt::hstring const& transparencyValue)
    {
        if (transparencyValue == L"Mica")
        {
            auto mica = media::MicaBackdrop();
            mica.Kind(backdrops::MicaKind::Base);
            this->SystemBackdrop(mica);
            SolidBackgroundLayer().Visibility(xaml::Visibility::Collapsed);
        }
        else if (transparencyValue == L"MicaAlt")
        {
            media::MicaBackdrop backdrop;
            backdrop.Kind(backdrops::MicaKind::BaseAlt);
            SystemBackdrop(backdrop);
            SolidBackgroundLayer().Visibility(xaml::Visibility::Collapsed);
        }
        else if (transparencyValue == L"None")
        {
            this->SystemBackdrop(nullptr);
            SolidBackgroundLayer().Visibility(xaml::Visibility::Visible);
        }
        else
        {
            this->SystemBackdrop(nullptr);
            SolidBackgroundLayer().Visibility(xaml::Visibility::Visible);
        }
    }

    void MainWindow::ApplyTheme(winrt::hstring const& themeName)
    {
        xaml::ElementTheme theme = xaml::ElementTheme::Default;

        if (themeName == L"Light")
        {
            theme = xaml::ElementTheme::Light;
        }
        else if (themeName == L"Dark")
        {
            theme = xaml::ElementTheme::Dark;
        }
        else
        {
            theme = xaml::ElementTheme::Default;
        }

        if (auto rootElement = Content().try_as<FrameworkElement>())
        {
            rootElement.RequestedTheme(theme);
        }

        using winrt::Microsoft::UI::Windowing::TitleBarTheme;
        TitleBarTheme titleBarTheme = TitleBarTheme::UseDefaultAppMode;
        if (theme == xaml::ElementTheme::Light)
        {
            titleBarTheme = TitleBarTheme::Light;
        }
        else if (theme == xaml::ElementTheme::Dark)
        {
            titleBarTheme = TitleBarTheme::Dark;
        }
        AppWindow().TitleBar().PreferredTheme(titleBarTheme);
    }


    // ABOUT
    void winrt::urPDF::implementation::MainWindow::AboutButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        MainWindowUI().Visibility(xaml::Visibility::Collapsed);

        RootFrame().Visibility(xaml::Visibility::Visible);
        RootFrame().Navigate(winrt::xaml_typename<winrt::urPDF::AboutPage>());
    }


    // TABVIEW
    void MainWindow::UpdateTitleBarPassthroughRegions()
    {
        if (m_isClosed || !ExtendsContentIntoTitleBar()) { return; }
        auto button = NewTabSplitButton();
        auto root = button.XamlRoot();
        if (!root) { return; }

        const bool titleBarVisible = !IsFullscreen() && AppTitleBar().Visibility() == xaml::Visibility::Visible;
        if (titleBarVisible)
        {
            const double rightInset = AppWindow().TitleBar().RightInset() / root.RasterizationScale();
            auto titleBarMargin = AppTitleBar().Margin();
            if (titleBarMargin.Right != rightInset)
            {
                titleBarMargin.Right = rightInset;
                AppTitleBar().Margin(titleBarMargin);
            }
        }

        using winrt::Windows::Graphics::RectInt32;
        std::vector<RectInt32> rects;

        if (titleBarVisible &&
            MainWindowUI().Visibility() == xaml::Visibility::Visible &&
            SettingsOverlay().Visibility() != xaml::Visibility::Visible)
        {
            const double scale = root.RasterizationScale();

            auto toRect = [scale](xaml::FrameworkElement const& element) -> std::optional<RectInt32>
            {
                if (!element.IsLoaded() || element.ActualWidth() <= 0 || element.ActualHeight() <= 0) { return std::nullopt; }
                const auto bounds = element.TransformToVisual(nullptr).TransformBounds(
                    { 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
                RectInt32 rect{};
                rect.X = static_cast<int32_t>(std::floor(bounds.X * scale));
                rect.Y = static_cast<int32_t>(std::floor(bounds.Y * scale));
                rect.Width = static_cast<int32_t>(std::ceil((bounds.X + bounds.Width) * scale)) - rect.X;
                rect.Height = static_cast<int32_t>(std::ceil((bounds.Y + bounds.Height) * scale)) - rect.Y;
                if (rect.Width <= 0 || rect.Height <= 0) { return std::nullopt; }
                return rect;
            };

            if (auto buttonRect = toRect(button))
            {
                rects.push_back(*buttonRect);
            }

            const auto tabView = MainTabView();
            const auto stripRect = toRect(tabView);
            if (TabDragKeyboard.tab && stripRect)
            {
                rects.push_back(*stripRect);
            }
            for (auto const& item : tabView.TabItems())
            {
                auto tab = item.try_as<controls::TabViewItem>();
                if (!tab) { continue; }
                auto tabRect = toRect(tab);
                if (!tabRect) { continue; }

                if (stripRect)
                {
                    const int32_t left = (std::max)(tabRect->X, stripRect->X);
                    const int32_t right = (std::min)(tabRect->X + tabRect->Width, stripRect->X + stripRect->Width);
                    if (right <= left) { continue; }
                    tabRect->X = left;
                    tabRect->Width = right - left;
                }
                rects.push_back(*tabRect);
            }
        }

        auto sameRects = [](std::vector<RectInt32> const& a, std::vector<RectInt32> const& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                [](RectInt32 const& l, RectInt32 const& r)
                {
                    return l.X == r.X && l.Y == r.Y && l.Width == r.Width && l.Height == r.Height;
                });
        };
        if (m_passthroughRects && sameRects(*m_passthroughRects, rects)) { return; }

        const auto input = winrt::Microsoft::UI::Input::InputNonClientPointerSource::GetForWindowId(AppWindow().Id());
        const auto region = winrt::Microsoft::UI::Input::NonClientRegionKind::Passthrough;
        if (!rects.empty())
        {
            input.SetRegionRects(region, rects);
        }
        else
        {
            input.ClearRegionRects(region);
        }
        m_passthroughRects = std::move(rects);
    }

    void MainWindow::TabView_Loaded(foundation::IInspectable const& sender, xaml::RoutedEventArgs const&)
    {
        auto tabView = sender.as<controls::TabView>();
        if (m_createInitialEmptyTab && tabView.TabItems().Size() == 0)
        {
            TabView_AddButtonClick(tabView, nullptr);
        }

        if (!m_moveSizeInput)
        {
            const auto windowId = AppWindow().Id();
            auto input = winrt::Microsoft::UI::Input::InputNonClientPointerSource::GetForWindowId(windowId);
            if (input)
            {
                m_moveSizeInput = input;
                auto weakApp = xaml::Application::Current().as<App>()->get_weak();
                m_enteredMoveSizeToken = input.EnteredMoveSize(
                    [weakApp, windowId](auto const&, auto const&)
                    {
                        if (auto app = weakApp.get()) { app->WindowMoveSizeEntered(windowId); }
                    });
                m_exitedMoveSizeToken = input.ExitedMoveSize(
                    [weakApp, windowId](auto const&, auto const&)
                    {
                        if (auto app = weakApp.get()) { app->WindowMoveSizeExited(windowId); }
                    });
            }
        }

        m_isTabViewLoaded = true;
        ProcessPendingOpens();
    }

    void MainWindow::TabView_AddButtonClick(foundation::IInspectable const&, foundation::IInspectable const&)
    {
        auto tabView = MainTabView();

        auto newTab = CreateTabItem();

        newTab.Header(winrt::box_value(L"New tab"));
        controls::SymbolIconSource tabIcon;
        tabIcon.Symbol(controls::Symbol::Document);
        newTab.IconSource(tabIcon);

        tabView.TabItems().Append(newTab);

        tabView.SelectedItem(newTab);
    }

    void MainWindow::TabView_SelectionChanged(foundation::IInspectable const& sender, controls::SelectionChangedEventArgs const& args)
    {
        for (auto const& removedItem : args.RemovedItems())
        {
            auto removedTab = removedItem.try_as<controls::TabViewItem>();
            auto removedEntry = m_openedTabs.find(removedTab);
            if (removedEntry != m_openedTabs.end())
            {
                SaveViewState(removedEntry->second);
                CancelSelectionCopy(removedEntry->second);
                CancelPendingSearchDebounce(removedEntry->second);
            }
        }
        ClearPageOverlays();
        CancelPendingZoomRender();
        auto tabView = sender.as<controls::TabView>();
        auto currentTab = tabView.SelectedItem().try_as<controls::TabViewItem>();
        auto currentEntry = m_openedTabs.find(currentTab);
        auto pdfInfo = currentEntry != m_openedTabs.end() ? currentEntry->second : nullptr;

        for (auto const& removedItem : args.RemovedItems())
        {
            auto removedTab = removedItem.try_as<controls::TabViewItem>();
            if (!removedTab)
            {
                continue;
            }

            auto removedEntry = m_openedTabs.find(removedTab);
            if (removedEntry != m_openedTabs.end())
            {
                ReleaseSlotsOutsideRange(removedEntry->second, 0, -1);
            }
        }
        if (pdfInfo)
        {
            RestoreViewState(pdfInfo);
            SyncSearchBarToDocument(pdfInfo);
            return;
        }

        const auto operation = DeferViewStateSave(nullptr);
        m_presentedTab.reset();
        PagesRepeater().ItemsSource(nullptr);
        m_pageRepeaterItems = nullptr;

        EmptyTabDropZone().Visibility(currentTab ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        SyncSearchBarToDocument(nullptr);
        if (!m_isClosed && tabView.TabItems().Size() > 0 && IsFullscreen())
        {
            ExitFullscreen();
        }
        DisplayPageNumber(0);
        DisplayPageCount(0);
        PagesLayout().PageSizes(nullptr);
        m_pendingLayoutViewZoom = 1.0;
        PdfScrollViewer().ChangeView(0.0, 0.0, 1.0f, true);
        FinishViewStateChange(nullptr, operation);
    }

    void MainWindow::TabView_TabCloseRequested(foundation::IInspectable const&, controls::TabViewTabCloseRequestedEventArgs const& args)
    {
        CloseTab(args.Tab());
    }

    bool MainWindow::CanDetachTab(controls::TabViewItem const& tab)
    {
        return CanTransferTab(tab, false);
    }

    bool MainWindow::CanTransferTab(controls::TabViewItem const& tab, bool allowViewRestore)
    {
        if (!tab || m_isClosed) { return false; }

        auto tabView = MainTabView();
        uint32_t index;
        if (!tabView.TabItems().IndexOf(tab, index)) { return false; }
        if (m_isRendering || m_loadingTab) { return false; }

        auto openedTab = m_openedTabs.find(tab);
        if (openedTab == m_openedTabs.end()) { return true; }
        auto const& pdfTab = openedTab->second;
        const bool restoringSavedView = allowViewRestore && m_viewRestorePhase != ViewRestorePhase::None && m_pendingLayoutViewSaveTab.lock() == pdfTab;
        if (!pdfTab->m_layoutLoaded
            || (m_viewStateGuard.IsPending() && m_pendingLayoutViewSaveTab.lock() == pdfTab && !restoringSavedView))
        {
            return false;
        }

        if (tabView.SelectedItem() == tab)
        {
            if (restoringSavedView) { return true; }
            return m_presentedTab.lock() == pdfTab && m_viewStateGuard.CanSave(pdfTab->m_documentId)
                && (pdfTab->m_pagecount == 0 || HasViewGeometry(pdfTab));
        }
        return true;
    }

    std::shared_ptr<PdfTabInfo> MainWindow::DetachTab(controls::TabViewItem const& tab)
    {
        return DetachTabForTransfer(tab, false);
    }

    std::shared_ptr<PdfTabInfo> MainWindow::DetachTabForTransfer(controls::TabViewItem const& tab, bool allowViewRestore)
    {
        if (!CanTransferTab(tab, allowViewRestore)) { return nullptr; }

        auto openedTab = m_openedTabs.find(tab);
        if (openedTab == m_openedTabs.end()) { return nullptr; }
        auto pdfTab = openedTab->second;
        auto tabView = MainTabView();
        uint32_t index;
        if (!tabView.TabItems().IndexOf(tab, index)) { return nullptr; }

        if (tabView.SelectedItem() == tab) { SaveViewState(pdfTab); }

        ClearPageTextSelection(pdfTab);
        ClearPageImageSelection(pdfTab);
        ResetPendingDocumentSearch(pdfTab);
        const auto generation = pdfTab->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (auto& links : pdfTab->m_pageLinks) { links.ResetPending(); }
        for (auto& text : pdfTab->m_pageText) { text.ResetPending(); }
        if (m_pendingZoomTab.lock() == pdfTab) { CancelPendingZoomRender(); }
        if (m_presentedTab.lock() == pdfTab || m_pendingLayoutViewSaveTab.lock() == pdfTab)
        {
            CancelPendingViewStateSave();
            m_presentedTab.reset();
        }

        m_pageBitmapCache.ClearDocumentEntries(pdfTab->m_documentId);
        ClearPageOverlays(pdfTab->m_documentId);
        ReleaseSlotsOutsideRange(pdfTab, 0, -1);
        for (auto& slot : pdfTab->m_renderedPages)
        {
            slot = PageSlot{ slot.pdfBounds };
            slot.generation = generation;
        }
        pdfTab->m_retainedPageFirst = 0;
        pdfTab->m_retainedPageLast = -1;

        m_openedTabs.erase(openedTab);
        tabView.TabItems().RemoveAt(index);
        if (!m_isClosed && tabView.TabItems().Size() > 0 && m_openedTabs.empty() && IsFullscreen())
        {
            ExitFullscreen();
        }
        return pdfTab;
    }

    bool MainWindow::DetachEmptyTab(controls::TabViewItem const& tab)
    {
        if (!CanDetachTab(tab) || m_openedTabs.contains(tab)) { return false; }

        auto tabView = MainTabView();
        uint32_t index;
        if (!tabView.TabItems().IndexOf(tab, index)) { return false; }
        if (tabView.SelectedItem() == tab)
        {
            CancelPendingZoomRender();
            CancelPendingViewStateSave();
            m_presentedTab.reset();
        }
        tabView.TabItems().RemoveAt(index);
        return true;
    }

    controls::TabViewItem MainWindow::AttachTab(std::shared_ptr<PdfTabInfo> const& pdfTab, int32_t index,
        controls::TabViewItem const& transferredTab)
    {
        if (m_isClosed || m_isRendering || !pdfTab || !pdfTab->m_layoutLoaded || !pdfTab->m_doc
            || pdfTab->m_documentId == 0 || pdfTab->m_runtime != m_runtime)
        {
            return nullptr;
        }
        if (std::any_of(m_openedTabs.begin(), m_openedTabs.end(),
            [&](auto const& entry) { return entry.second == pdfTab; }))
        {
            return nullptr;
        }

        auto tabView = MainTabView();
        auto items = tabView.TabItems();
        const auto insertionIndex = static_cast<uint32_t>(std::clamp(index, 0, static_cast<int32_t>(items.Size())));

        auto tab = transferredTab ? transferredTab : CreateTabItem();
        uint32_t existingIndex;
        if (items.IndexOf(tab, existingIndex)) { return nullptr; }
        tab.Header(winrt::box_value(pdfTab->m_fileName));
        controls::SymbolIconSource icon;
        icon.Symbol(controls::Symbol::Document);
        tab.IconSource(icon);

        SaveViewState(m_presentedTab.lock());
        m_openedTabs.emplace(tab, pdfTab);
        try
        {
            items.InsertAt(insertionIndex, tab);
            tabView.SelectedItem(tab);
        }
        catch (...)
        {
            m_openedTabs.erase(tab);
            uint32_t insertedIndex;
            if (items.IndexOf(tab, insertedIndex)) { items.RemoveAt(insertedIndex); }
            throw;
        }
        return tab;
    }

    controls::TabViewItem MainWindow::AttachEmptyTab(controls::TabViewItem const& tab, int32_t index)
    {
        if (!tab || m_isClosed || m_isRendering || m_loadingTab || m_openedTabs.contains(tab) || (m_viewStateGuard.IsPending() && !m_pendingLayoutViewSaveTab.expired() && m_viewRestorePhase == ViewRestorePhase::None))
        {
            return nullptr;
        }
        auto tabView = MainTabView();
        auto items = tabView.TabItems();
        uint32_t existingIndex;
        if (items.IndexOf(tab, existingIndex) || xaml::Application::Current().as<App>()->FindWindowForTab(tab))
        {
            return nullptr;
        }
        const auto insertionIndex = static_cast<uint32_t>(std::clamp(index, 0, static_cast<int32_t>(items.Size())));

        SaveViewState(m_presentedTab.lock());
        try
        {
            items.InsertAt(insertionIndex, tab);
            tabView.SelectedItem(tab);
        }
        catch (...)
        {
            uint32_t insertedIndex;
            if (items.IndexOf(tab, insertedIndex)) { items.RemoveAt(insertedIndex); }
            throw;
        }
        return tab;
    }

    bool MainWindow::CanReceiveTabFrom(MainWindow& source, controls::TabViewItem const& tab)
    {
        if (&source == this || m_isClosed || m_isRendering || m_loadingTab || m_runtime != source.m_runtime || (m_viewStateGuard.IsPending() && !m_pendingLayoutViewSaveTab.expired() && m_viewRestorePhase == ViewRestorePhase::None) || !source.CanTransferTab(tab, true))
        {
            return false;
        }

        auto app = xaml::Application::Current().as<App>();
        auto sourceWindow = app->FindWindowForTab(tab);
        auto destinationWindow = app->FindWindow(AppWindow().Id());
        return sourceWindow && get_self<MainWindow>(sourceWindow) == &source && destinationWindow && get_self<MainWindow>(destinationWindow) == this;
    }

    bool MainWindow::TransferTabFrom(MainWindow& source, controls::TabViewItem const& tab, int32_t index)
    {
        if (!CanReceiveTabFrom(source, tab)) { return false; }
        auto sourceTabView = source.MainTabView();
        uint32_t originalIndex;
        if (!sourceTabView.TabItems().IndexOf(tab, originalIndex)) { return false; }
        auto originalSelection = sourceTabView.SelectedItem();
        const bool isEmptyTab = !source.m_openedTabs.contains(tab);
        std::shared_ptr<PdfTabInfo> pdfTab;
        if (isEmptyTab)
        {
            if (!source.DetachEmptyTab(tab)) { return false; }
        }
        else
        {
            pdfTab = source.DetachTabForTransfer(tab, true);
            if (!pdfTab) { return false; }
        }

        auto restoreSourceTab = [&]
        {
            if (pdfTab) { source.m_openedTabs.emplace(tab, pdfTab); }
            try
            {
                sourceTabView.TabItems().InsertAt(originalIndex, tab);
            }
            catch (...)
            {
                if (pdfTab) { source.m_openedTabs.erase(tab); }
                throw;
            }
            if (sourceTabView.SelectedItem() != originalSelection)
            {
                source.SaveViewState(source.m_presentedTab.lock());
                sourceTabView.SelectedItem(originalSelection);
            }
        };

        controls::TabViewItem attached{ nullptr };
        try
        {
            if (isEmptyTab)
            {
                auto incoming = CreateTabItem();
                incoming.Header(tab.Header());
                incoming.IconSource(tab.IconSource());
                attached = AttachEmptyTab(incoming, index);
            }
            else { attached = AttachTab(pdfTab, index); }
        }
        catch (...)
        {
            restoreSourceTab();
            throw;
        }
        if (!attached)
        {
            restoreSourceTab();
            return false;
        }
        if (source.MainTabView().TabItems().Size() == 0)
        {
            if (m_presentationPending) { m_sourceWindowToClose = source.AppWindow().Id(); }
            else { xaml::Application::Current().as<App>()->RequestCloseEmptyWindow(source.AppWindow().Id()); }
        }
        BringToFront();
        return true;
    }

    void MainWindow::TabView_TabDragStarting(controls::TabView const& sender,
        controls::TabViewTabDragStartingEventArgs const& args)
    {
        auto tab = args.Tab();
        auto tabView = MainTabView();
        uint32_t index;
        // Reordering does not transfer document ownership. View-state transfer
        // checks belong at the drop, after the selection has settled.
        if (sender != tabView || !tab || m_isClosed || m_isRendering || m_loadingTab
            || !tabView.TabItems().IndexOf(tab, index))
        {
            args.Cancel(true);
            return;
        }

        TabDragKeyboard.Stop();
        TabDragKeyboard.cancelled = false;
        TabDragKeyboard.tab = tab;
        xaml::Application::Current().as<App>()->RefreshTabDragRegions();
        TabDragKeyboard.hook = SetWindowsHookExW(WH_KEYBOARD, TrackTabDragKey, nullptr, GetCurrentThreadId());
        args.Data().SetData(TabDragProperty, winrt::box_value(true));
        args.Data().Properties().Insert(TabDragProperty, winrt::box_value(true));
        args.Data().RequestedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            const auto position = AppWindow().Position();
            const auto tabPosition = tab.TransformToVisual(sender).TransformPoint({});
            const auto scale = tab.XamlRoot().RasterizationScale();
            const auto firstTab = sender.TabItems().GetAt(0).as<controls::TabViewItem>();
            const auto firstPosition = firstTab.TransformToVisual(sender).TransformPoint({});
            m_tabDragOffset = { cursor.x - position.X - static_cast<int32_t>((tabPosition.X - firstPosition.X) * scale),
                cursor.y - position.Y };
        }
    }

    void MainWindow::TabView_TabDragCompleted(controls::TabView const&, controls::TabViewTabDragCompletedEventArgs const&)
    {
        TabDragKeyboard.Stop();
        TabDragKeyboard.tab = nullptr;
        xaml::Application::Current().as<App>()->RefreshTabDragRegions();
    }

    void MainWindow::TabView_TabStripDragOver(foundation::IInspectable const&, xaml::DragEventArgs const& args)
    {
        auto tab = DraggedTab(args);
        auto source = xaml::Application::Current().as<App>()->FindWindowForTab(tab);
        if (!source) { return; }
        auto sourceWindow = get_self<MainWindow>(source);
        if (sourceWindow == this && IsTabListDrag(args)) { return; }
        args.AcceptedOperation((sourceWindow == this || CanReceiveTabFrom(*sourceWindow, tab))
            ? winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move
            : winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::None);
        args.DragUIOverride().IsCaptionVisible(false);
        args.DragUIOverride().IsGlyphVisible(false);
        args.Handled(true);
    }

    void MainWindow::TabView_TabStripDrop(foundation::IInspectable const&, xaml::DragEventArgs const& args)
    {
        auto tab = DraggedTab(args);
        auto source = xaml::Application::Current().as<App>()->FindWindowForTab(tab);
        if (!source) { return; }
        const bool reordering = get_self<MainWindow>(source) == this;
        if (reordering && IsTabListDrag(args)) { return; }

        auto tabView = MainTabView();
        const auto pointer = args.GetPosition(tabView);
        int32_t index = 0;
        for (auto const& item : tabView.TabItems())
        {
            if (reordering && item == tab) { continue; }
            auto target = item.as<controls::TabViewItem>();
            const auto bounds = target.TransformToVisual(tabView).TransformBounds(
                { 0, 0, static_cast<float>(target.ActualWidth()), static_cast<float>(target.ActualHeight()) });
            if (pointer.X < bounds.X + bounds.Width / 2) { break; }
            ++index;
        }
        bool moved = false;
        if (reordering)
        {
            uint32_t oldIndex;
            auto items = tabView.TabItems();
            if (items.IndexOf(tab, oldIndex))
            {
                const auto newIndex = static_cast<uint32_t>(index);
                if (newIndex != oldIndex)
                {
                    items.RemoveAt(oldIndex);
                    items.InsertAt(newIndex, tab);
                }
                tabView.SelectedItem(tab);
                moved = true;
            }
        }
        else { moved = TransferTabFrom(*get_self<MainWindow>(source), tab, index); }
        args.AcceptedOperation(moved
            ? winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move
            : winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::None);
        args.Handled(true);
    }

    void MainWindow::TabView_TabDroppedOutside(controls::TabView const& sender,
        controls::TabViewTabDroppedOutsideEventArgs const& args)
    {
        if (sender != MainTabView() || TabDragKeyboard.cancelled || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { return; }
        auto tab = args.Tab();
        if (!CanTransferTab(tab, true)) { return; }

        auto app = xaml::Application::Current().as<App>();
        urpdf::WindowCreationOptions options;
        options.createInitialEmptyTab = false;
        options.deferPresentation = true;
        auto window = app->CreateWindow(options);
        try
        {
            POINT cursor{};
            if (GetCursorPos(&cursor))
            {
                auto nativeWindow = try_as<::IWindowNative>();
                HWND hwnd{};
                WINDOWPLACEMENT placement{};
                placement.length = sizeof(placement);
                RECT normalBounds{};
                if (nativeWindow && SUCCEEDED(nativeWindow->get_WindowHandle(&hwnd))
                    && GetWindowPlacement(hwnd, &placement))
                {
                    normalBounds = placement.rcNormalPosition;
                }

                MONITORINFO monitorInfo{};
                monitorInfo.cbSize = sizeof(monitorInfo);
                if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitorInfo))
                {
                    if (const auto bounds = urpdf::ResolveTabTearOutPlacement(normalBounds, monitorInfo.rcWork,
                        cursor, { m_tabDragOffset.X, m_tabDragOffset.Y }))
                    {
                        window.AppWindow().MoveAndResize({ static_cast<int32_t>(bounds->left), static_cast<int32_t>(bounds->top),
                            static_cast<int32_t>(bounds->right - bounds->left), static_cast<int32_t>(bounds->bottom - bounds->top) });
                    }
                }
            }
            auto destination = get_self<MainWindow>(window);
            if (!destination->TransferTabFrom(*this, tab, 0)) { window.Close(); }
            else { destination->ShowPreparedWindow(); }
        }
        catch (...)
        {
            window.Close();
            throw;
        }
    }

    void MainWindow::CloseIfEmpty()
    {
        if (!m_isClosed && !m_isRendering && !m_loadingTab && MainTabView().TabItems().Size() == 0)
        {
            Close();
        }
    }

    void MainWindow::CloseTab(controls::TabViewItem const& tab)
    {
        if (!tab) { return; }

        auto tabView = MainTabView();

        auto openedTab = m_openedTabs.find(tab);
        if (openedTab != m_openedTabs.end())
        {
            if (m_pendingZoomTab.lock() == openedTab->second)
            {
                CancelPendingZoomRender();
            }
            ClearPageTextSelection(openedTab->second);
            ClearPageImageSelection(openedTab->second);
            CancelPendingSearchDebounce(openedTab->second);
            CancelActiveSearchSession(openedTab->second);
            openedTab->second->m_search.Clear();
            ++openedTab->second->m_renderGeneration;
            ClearPageOverlays(openedTab->second->m_documentId);
            for (auto& slot : openedTab->second->m_renderedPages) { slot.bitmap.reset(); }
            m_pageBitmapCache.ClearDocumentEntries(openedTab->second->m_documentId);
            m_openedTabs.erase(openedTab);
        }

        uint32_t index;
        if (tabView.TabItems().IndexOf(tab, index))
        {
            tabView.TabItems().RemoveAt(index);
        }

        if (tabView.TabItems().Size() == 0 && !m_isClosed)
        {
            Close();
        }
        else if (!m_isClosed && m_openedTabs.empty() && IsFullscreen())
        {
            ExitFullscreen();
        }
    }

    void MainWindow::EmptyTab_DragOver(foundation::IInspectable const&, xaml::DragEventArgs const& args)
    {
        if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
        {
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            EmptyTabDropHint().Visibility(xaml::Visibility::Visible);
        }
        else
        {
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::None);
        }
    }

    void MainWindow::EmptyTab_DragLeave(foundation::IInspectable const&, xaml::DragEventArgs const&)
    {
        EmptyTabDropHint().Visibility(xaml::Visibility::Collapsed);
    }

    winrt::fire_and_forget MainWindow::EmptyTab_Drop(foundation::IInspectable const&, xaml::DragEventArgs const& args)
    {
        auto strongThis = get_strong();
        auto deferral = args.GetDeferral();
        winrt::weak_ref<controls::TabViewItem> targetTab;
        if (auto selectedTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>()) { targetTab = winrt::make_weak(selectedTab); }
        auto storageItems = co_await args.DataView().GetStorageItemsAsync();

        EmptyTabDropHint().Visibility(xaml::Visibility::Collapsed);
        deferral.Complete();

        for (auto const& item : storageItems)
        {
            auto file = item.try_as<storage::StorageFile>();
            if (file && IsPdfFile(file))
            {
                urpdf::WindowCreationOptions document;
                document.file = file;
                OpenDocument(document, std::exchange(targetTab, {}));
            }
        }
    }


    // FILE OPENING 
    foundation::IAsyncOperation<storage::StorageFile> MainWindow::PickPdfFileAsync()
    {
        pickers::FileOpenPicker filePicker;
        filePicker.FileTypeFilter().Append(L".pdf");

        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
        filePicker.as<::IInitializeWithWindow>()->Initialize(hwnd);

        co_return co_await filePicker.PickSingleFileAsync();
    }

    winrt::fire_and_forget MainWindow::OpenPdfFileAsync(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto strongThis = get_strong();
        auto selectedTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        winrt::weak_ref<controls::TabViewItem> targetTab;
        if (selectedTab && !m_openedTabs.contains(selectedTab)) { targetTab = winrt::make_weak(selectedTab); }
        auto file = co_await PickPdfFileAsync();
        if (!file || m_isClosed) { co_return; }

        urpdf::WindowCreationOptions document;
        document.file = file;
        OpenDocument(document, targetTab);
    }

    winrt::fire_and_forget MainWindow::OpenPdfInNewWindowAsync(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto strongThis = get_strong();
        winrt::hstring errorMessage;
        try
        {
            auto file = co_await PickPdfFileAsync();
            if (!file || m_isClosed) { co_return; }

            urpdf::WindowCreationOptions options;
            options.file = file;
            auto app = xaml::Application::Current().as<App>();
            app->CreateWindow(options);
        }
        catch (...)
        {
            errorMessage = CurrentOpenErrorMessage();
        }

        if (!errorMessage.empty() && !m_isClosed)
        {
            ShowOpenErrorAsync(errorMessage);
        }
    }

    winrt::fire_and_forget MainWindow::ShowOpenErrorAsync(winrt::hstring message)
    {
        auto strongThis = get_strong();
        if (m_isClosed) { co_return; }

        m_openErrorMessages.push_back(std::move(message));
        if (m_openErrorMessages.size() > 1) { co_return; }

        while (!m_isClosed && !m_openErrorMessages.empty())
        {
            m_openErrorDialog = controls::ContentDialog();
            m_openErrorDialog.XamlRoot(MainWindowUI().XamlRoot());
            m_openErrorDialog.Title(winrt::box_value(L"Couldn't open document"));
            m_openErrorDialog.Content(winrt::box_value(m_openErrorMessages.front()));
            m_openErrorDialog.CloseButtonText(L"OK");
            try
            {
                co_await m_openErrorDialog.ShowAsync();
            }
            catch (winrt::hresult_error const& error)
            {
                if (!m_isClosed) { OutputDebugStringW(error.message().c_str()); }
            }
            m_openErrorDialog = nullptr;
            m_openErrorMessages.pop_front();
        }
        m_openErrorMessages.clear();
    }

    winrt::fire_and_forget MainWindow::OpenPdfPathAsync(PendingDocumentOpen request)
    {
        auto strongThis = get_strong();
        m_isResolvingPendingOpen = true;

        storage::StorageFile file{ nullptr };
        winrt::hstring errorMessage;
        try
        {
            file = co_await storage::StorageFile::GetFileFromPathAsync(request.document.filePath);
            if (!IsPdfFile(file))
            {
                throw winrt::hresult_error(E_INVALIDARG, L"Please select a PDF document.");
            }
        }
        catch (...)
        {
            errorMessage = CurrentOpenErrorMessage();
        }

        m_isResolvingPendingOpen = false;
        if (m_isClosed) { co_return; }
        if (!errorMessage.empty())
        {
            ShowOpenErrorAsync(errorMessage);
        }
        else
        {
            request.document.file = file;
            m_pendingOpens.push_front(std::move(request));
        }
        ProcessPendingOpens();
    }

    void MainWindow::OpenDocument(urpdf::WindowCreationOptions const& document,
        std::optional<winrt::weak_ref<controls::TabViewItem>> targetTab)
    {
        if (m_isClosed || (!document.file && document.filePath.empty())) { return; }
        m_pendingOpens.push_back({ document, std::move(targetTab) });
        ProcessPendingOpens();
    }

    void MainWindow::ProcessPendingOpens()
    {
        while (!m_isClosed && m_isTabViewLoaded && !m_isRendering && !m_loadingTab && !m_isResolvingPendingOpen && !m_pendingOpens.empty())
        {
            auto request = std::move(m_pendingOpens.front());
            m_pendingOpens.pop_front();
            if (request.document.file)
            {
                OpenPdfFileAsync(std::move(request));
            }
            else
            {
                OpenPdfPathAsync(std::move(request));
            }
        }
    }

    void MainWindow::SchedulePendingOpens()
    {
        if (m_isClosed || m_pendingOpensQueued || m_pendingOpens.empty()) { return; }
        m_pendingOpensQueued = DispatcherQueue().TryEnqueue(dispatching::DispatcherQueuePriority::Low,
            [weakThis = get_weak()]
            {
                auto self = weakThis.get();
                if (!self) { return; }
                self->m_pendingOpensQueued = false;
                self->ProcessPendingOpens();
            });
    }

    void MainWindow::PrepareForDeferredPresentation()
    {
        HWND hwnd{};
        auto native = this->try_as<::IWindowNative>();
        if (!native) { throw winrt::hresult_no_interface(); }
        winrt::check_hresult(native->get_WindowHandle(&hwnd));
        const BOOL cloak = TRUE;
        winrt::check_hresult(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
        m_presentationPending = true;
    }

    void MainWindow::ShowPreparedWindow()
    {
        if (m_isClosed) { return; }
        if (!m_presentationPending) { BringToFront(); return; }

        // A cloaked window is still composed by DWM. Wait for its initialized
        // XAML tree to render before making the native window visible to users.
        m_firstFrameRenderedRevoker = media::CompositionTarget::Rendered(winrt::auto_revoke,
            [weakThis = get_weak()](auto const&, auto const&)
            {
                auto self = weakThis.get();
                if (!self || self->m_isClosed || !self->MainWindowUI().IsLoaded()
                    || self->MainWindowUI().ActualWidth() <= 0) { return; }
                self->m_firstFrameRenderedRevoker.revoke();
                self->DispatcherQueue().TryEnqueue([weakThis]
                    {
                        if (auto window = weakThis.get()) { window->CompleteDeferredPresentation(); }
                    });
            });
        Activate();
    }

    winrt::fire_and_forget MainWindow::CompleteDeferredPresentation()
    {
        auto lifetime = get_strong();
        if (m_isClosed || !m_presentationPending) { co_return; }
        try
        {
            co_await media::CompositionTarget::GetCompositorForCurrentThread().RequestCommitAsync();
        }
        catch (winrt::hresult_error const&)
        {
            // Closing the window can dispose its compositor during the await.
            if (m_isClosed) { co_return; }
        }
        if (m_isClosed) { co_return; }
        // Submit the rendered surface before removing the compositor's cloak.
        DwmFlush();
        HWND hwnd{};
        auto native = this->try_as<::IWindowNative>();
        if (!native) { throw winrt::hresult_no_interface(); }
        winrt::check_hresult(native->get_WindowHandle(&hwnd));
        const BOOL cloak = FALSE;
        winrt::check_hresult(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
        m_presentationPending = false;
        BringToFront();
        DwmFlush();
        CloseTransferredSourceWindow();
    }

    void MainWindow::CloseTransferredSourceWindow()
    {
        if (auto source = std::exchange(m_sourceWindowToClose, std::nullopt))
        {
            xaml::Application::Current().as<App>()->RequestCloseEmptyWindow(*source);
        }
    }

    void MainWindow::BringToFront()
    {
        if (m_isClosed || m_presentationPending) { return; }
        if (auto presenter = AppWindow().Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            if (presenter.State() == winrt::Microsoft::UI::Windowing::OverlappedPresenterState::Minimized)
            {
                presenter.Restore();
            }
        }
        Activate();

        HWND hwnd{ nullptr };
        if (auto windowNative = this->try_as<::IWindowNative>(); windowNative && SUCCEEDED(windowNative->get_WindowHandle(&hwnd)) && hwnd)
        {
            ::SetForegroundWindow(hwnd);
        }
    }

    void MainWindow::OpenPdfFileAsync(PendingDocumentOpen request)
    {
        auto const& file = request.document.file;
        if (!TryAcquireRenderingLock()) { return; }
        winrt::hstring errorMessage;
        try
        {
            auto registration = m_runtime->Work().TryRegister();
            if (!registration)
            {
                ReleaseRenderingLock();
                return;
            }
            const auto openStartedAt = std::chrono::steady_clock::now();
            auto pdfTab = std::make_shared<PdfTabInfo>(m_runtime, winrt::to_string(file.Path()));
            m_loadingTab = pdfTab;
            m_loadingTargetTab = std::move(request.targetTab);
            pdfTab->m_renderMetrics.openStartedAt = openStartedAt;
            pdfTab->m_documentId = xaml::Application::Current().as<App>()->AllocateDocumentId();
            pdfTab->m_fileName = file.Name();
            pdfTab->m_openedFilePath = file.Path();
            const auto generation = pdfTab->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            auto context = pdfTab->m_runtime->CloneContext();
            auto weakTab = std::weak_ptr<PdfTabInfo>(pdfTab);
            auto cancelled = m_workCancelled;
            auto dispatcher = DispatcherQueue();
            QueueMuPdfWork(std::move(registration),
                [pdfTab = std::move(pdfTab), context = std::move(context), cancelled, generation]() mutable
                {
                    DocumentLayoutResult result;
                    if (cancelled->load(std::memory_order_acquire)) { return result; }
                    urpdf::RunDocumentOperation(pdfTab->m_documentMutex, pdfTab->m_renderGeneration, generation, [&]
                        {
                            if (cancelled->load(std::memory_order_acquire)) { return; }
                            pdfTab->OpenDocument();
                            if (cancelled->load(std::memory_order_acquire)) { return; }
                            result.pagecount = urpdf::CountDocumentPages(context.get(), pdfTab->m_doc);
                            result.pageBounds = urpdf::CollectPageBounds(context.get(), pdfTab->m_doc, result.pagecount);
                        });
                    return result;
                },
                [weakThis = get_weak(), weakTab, cancelled, dispatcher, generation]
                (std::optional<DocumentLayoutResult> result, std::exception_ptr error)
                {
                    auto layout = result ? std::move(*result) : DocumentLayoutResult{};
                    TryPublishResult(dispatcher, cancelled,
                        [weakThis, weakTab, cancelled, generation, layout = std::move(layout), error]() mutable
                        {
                            auto self = weakThis.get();
                            if (!self || cancelled->load(std::memory_order_acquire)) { return; }
                            if (auto pdfTab = weakTab.lock())
                            {
                                self->CompleteDocumentOpen(pdfTab, generation, layout.pagecount, std::move(layout.pageBounds), error);
                            }
                        });
                });
        }
        catch (...)
        {
            errorMessage = CurrentOpenErrorMessage();
            m_loadingTab.reset();
            m_loadingTargetTab.reset();
            ReleaseRenderingLock();
        }
        if (!errorMessage.empty()) { ShowOpenErrorAsync(errorMessage); }
    }

    void MainWindow::CompleteDocumentOpen(std::shared_ptr<PdfTabInfo> const& pdfTab,
        std::uint64_t generation, int pagecount, std::vector<fz_rect> pageBounds, std::exception_ptr error)
    {
        if (m_isClosed || m_loadingTab != pdfTab) { return; }
        m_loadingTab.reset();
        auto requestedTarget = std::exchange(m_loadingTargetTab, std::nullopt);
        if (pdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            ReleaseRenderingLock();
            return;
        }
        controls::TabViewItem loadingTabItem{ nullptr };
        winrt::hstring errorMessage;
        try
        {
            if (error) { std::rethrow_exception(error); }
            std::vector<PageSlot> slots(pagecount);
            for (int i = 0; i < pagecount; ++i)
            {
                slots[i].pdfBounds = pageBounds.at(i);
                slots[i].generation = generation;
            }
            pdfTab->m_pagecount = pagecount;
            pdfTab->m_pageBounds = std::move(pageBounds);
            pdfTab->m_renderedPages = std::move(slots);
            pdfTab->m_pageLinks.resize(pagecount);
            pdfTab->m_pageText.resize(pagecount);
            pdfTab->m_layoutLoaded = true;
            const auto previous = m_openedTabs.find(MainTabView().SelectedItem().try_as<controls::TabViewItem>());
            if (previous != m_openedTabs.end())
            {
                ClearPageTextSelection(previous->second);
                ClearPageImageSelection(previous->second);
            }

            auto targetTab = requestedTarget
                ? requestedTarget->get()
                : MainTabView().SelectedItem().try_as<controls::TabViewItem>();
            uint32_t targetIndex;
            if (targetTab && (m_openedTabs.contains(targetTab) || !MainTabView().TabItems().IndexOf(targetTab, targetIndex)))
            {
                targetTab = nullptr;
            }
            if (targetTab)
            {
                targetTab.Content(nullptr);
            }
            else
            {
                targetTab = CreateTabItem();
                MainTabView().TabItems().Append(targetTab);
            }
            if (MainTabView().SelectedItem() != targetTab) { MainTabView().SelectedItem(targetTab); }
            loadingTabItem = targetTab;
            SaveViewState(m_presentedTab.lock());
            targetTab.Header(winrt::box_value(pdfTab->m_fileName));
            controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(controls::Symbol::Document);
            targetTab.IconSource(tabIcon);
            m_openedTabs[targetTab] = pdfTab;
            RestoreViewState(pdfTab);
            SyncSearchBarToDocument(pdfTab);
            ReleaseRenderingLock();
            if (!m_viewStateGuard.IsPending()) { RenderVisiblePagesAsync(); }
        }
        catch (...)
        {
            errorMessage = CurrentOpenErrorMessage();
            ++pdfTab->m_renderGeneration;
            if (loadingTabItem)
            {
                if (MainTabView().TabItems().Size() == 1) { TabView_AddButtonClick(nullptr, nullptr); }
                CloseTab(loadingTabItem);
            }
            ReleaseRenderingLock();
        }
        if (!errorMessage.empty()) { ShowOpenErrorAsync(errorMessage); }
    }


    // PAGE RENDERING
    std::vector<PageRenderRequest> MainWindow::BuildRenderQueue(int32_t pageCount, PageViewRange& viewRange)
    {
        std::vector<PageRenderRequest> renderQueue;
        viewRange = PageViewRange{};
        if (pageCount <= 0)
        {
            return renderQueue;
        }

        auto const& pageRects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        const int32_t knownPages = (std::min)(pageCount, static_cast<int32_t>(pageRects.size()));

        auto scrollViewer = PdfScrollViewer();
        const double zoom = (std::max)(0.01, static_cast<double>(scrollViewer.ZoomFactor()));
        const double repeaterTop = PagesRepeater().ActualOffset().y;
        const double viewportTop = scrollViewer.VerticalOffset() / zoom - repeaterTop;
        const double viewportBottom = (scrollViewer.VerticalOffset() + scrollViewer.ViewportHeight()) / zoom - repeaterTop;

        int32_t firstVisible = -1;
        int32_t lastVisible = -1;
        for (int32_t i = 0; i < knownPages; ++i)
        {
            const auto& rect = pageRects[i];
            if (rect.Y + rect.Height >= viewportTop && rect.Y <= viewportBottom)
            {
                if (firstVisible < 0)
                {
                    firstVisible = i;
                }
                lastVisible = i;
            }
            else if (rect.Y > viewportBottom)
            {
                break;
            }
        }

        if (firstVisible < 0)
        {
            firstVisible = 0;
            for (int32_t i = 0; i < knownPages; ++i)
            {
                if (pageRects[i].Y + pageRects[i].Height >= viewportTop)
                {
                    firstVisible = i;
                    break;
                }
            }
            lastVisible = firstVisible;
        }

        for (int32_t i = firstVisible; i <= lastVisible; ++i)
        {
            renderQueue.push_back(PageRenderRequest{ i, PageRenderPriority::visible, i - firstVisible });
        }

        const int32_t prefetchPages = PrefetchPageCount * (std::max)(1, PagesLayout().ColumnCount());
        for (int32_t distance = 1; distance <= prefetchPages; ++distance)
        {
            if (lastVisible + distance < pageCount)
            {
                renderQueue.push_back(PageRenderRequest{ lastVisible + distance, PageRenderPriority::prefetch, distance });
            }
            if (firstVisible - distance >= 0)
            {
                renderQueue.push_back(PageRenderRequest{ firstVisible - distance, PageRenderPriority::prefetch, distance });
            }
        }

        std::stable_sort(renderQueue.begin(), renderQueue.end(), [](PageRenderRequest const& left, PageRenderRequest const& right)
        {
            if (left.priority != right.priority)
            {
                return left.priority < right.priority;
            }
            return left.distance < right.distance;
        });

        const int32_t retainMargin = prefetchPages + SlotRetainMarginPages;
        viewRange.firstVisible = firstVisible;
        viewRange.lastVisible = lastVisible;
        viewRange.keepFirst = (std::max)(0, firstVisible - retainMargin);
        viewRange.keepLast = (std::min)(pageCount - 1, lastVisible + retainMargin);

        return renderQueue;
    }

    void MainWindow::ReleaseSlotsOutsideRange(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t keepFirst, int32_t keepLast)
    {
        bool isCurrentTab = false;
        if (auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>())
        {
            auto openedTab = m_openedTabs.find(currentTab);
            isCurrentTab = openedTab != m_openedTabs.end() && openedTab->second == PdfTab;
        }

        const int32_t slotCount = static_cast<int32_t>(PdfTab->m_renderedPages.size());
        for (int32_t i = 0; i < slotCount; ++i)
        {
            if (i >= keepFirst && i <= keepLast)
            {
                continue;
            }

            auto& slot = PdfTab->m_renderedPages[i];
            if (!slot.bitmap && slot.state != PageRenderState::error)
            {
                continue;
            }

            slot.bitmap.reset();
            slot.state = PageRenderState::unloaded;
            slot.renderScaleBucket = 0;

            if (isCurrentTab)
            {
                if (auto element = PagesRepeater().TryGetElement(i))
                {
                    ConfigurePageElement(element, static_cast<std::size_t>(i));
                }
            }
        }
    }

    void MainWindow::RestoreSlotsFromCache(std::shared_ptr<PdfTabInfo> const& PdfTab)
    {
        const int32_t slotCount = static_cast<int32_t>(PdfTab->m_renderedPages.size());
        const int32_t first = (std::max)(0, PdfTab->m_retainedPageFirst);
        const int32_t last = (std::min)(slotCount - 1, PdfTab->m_retainedPageLast);
        const int rotation = NormalizeRotation(PdfTab->m_viewState.rotation);

        for (int32_t i = first; i <= last; ++i)
        {
            const int renderScaleBucket = GetPageRenderScaleBucket(PdfTab, i);
            if (renderScaleBucket == 0) { continue; }
            auto& slot = PdfTab->m_renderedPages[i];
            if (slot.bitmap)
            {
                if (slot.state == PageRenderState::ready && slot.renderScaleBucket == renderScaleBucket)
                {
                    ++PdfTab->m_slotBitmapReuses;
                }
                continue;
            }

            if (auto cachedBitmap = m_pageBitmapCache.TryGetBitmap(PageBitmapCacheKey{ PdfTab->m_documentId, i, rotation, renderScaleBucket }))
            {
                ApplyPageBitmap(PdfTab, i, *cachedBitmap, renderScaleBucket);
            }
        }
    }

    void MainWindow::AttachDpiObserver()
    {
        if (m_isClosed) { return; }
        auto root = PdfScrollViewer().XamlRoot();
        if (!root || root == m_observedXamlRoot) { return; }

        DetachDpiObserver();
        m_observedXamlRoot = root;
        m_lastRasterizationScale = GetRasterizationScale();
        m_xamlRootChangedRevoker = root.Changed(winrt::auto_revoke, [weakThis = get_weak()](auto const& sender, auto const&)
            {
                if (auto self = weakThis.get())
                {
                    if (self->m_isClosed || sender != self->m_observedXamlRoot) { return; }
                    self->UpdateTitleBarPassthroughRegions();
                    const double scale = sender.RasterizationScale();
                    if (!std::isfinite(scale) || scale <= 0.0 || scale == self->m_lastRasterizationScale) { return; }
                    self->m_lastRasterizationScale = scale;
                    self->RequestDpiRefresh();
                }
            });

        RequestDpiRefresh();
    }

    void MainWindow::DetachDpiObserver()
    {
        m_xamlRootChangedRevoker.revoke();
        m_observedXamlRoot = nullptr;
        if (m_dpiRenderTimer) { m_dpiRenderTimer.Stop(); }
        m_dpiRefreshPending = false;
    }

    void MainWindow::RequestDpiRefresh()
    {
        if (m_isClosed || !m_observedXamlRoot) { return; }
        m_dpiRefreshPending = true;
        m_dpiRenderTimer.Stop();
        m_dpiRenderTimer.Start();
    }

    void MainWindow::ApplyPendingDpiRefresh()
    {
        if (m_isClosed || !m_observedXamlRoot || !m_dpiRefreshPending) { return; }
        if (m_isRendering || m_viewStateGuard.IsPending()) { return; }

        m_dpiRefreshPending = false;
        PdfScrollViewer().UpdateLayout();
        RefreshRealizedPageOverlays();
        RenderVisiblePagesAsync();
    }

    double MainWindow::GetRasterizationScale()
    {
        auto root = PdfScrollViewer().XamlRoot();
        const double scale = root ? root.RasterizationScale() : 1.0;
        return std::isfinite(scale) && scale > 0.0 ? scale : 1.0;
    }

    int MainWindow::GetPageRenderScaleBucket(std::shared_ptr<PdfTabInfo> const& pdfTab, int32_t pageIndex)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto openedTab = m_openedTabs.find(currentTab);
        if (openedTab == m_openedTabs.end() || openedTab->second != pdfTab || m_layoutRotation != NormalizeRotation(pdfTab->m_viewState.rotation) || pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= pdfTab->m_renderedPages.size())
        {
            return 0;
        }

        auto const& rects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        if (static_cast<std::size_t>(pageIndex) >= rects.size()) { return 0; }
        auto const& rect = rects[pageIndex];
        auto const& bounds = pdfTab->m_renderedPages[pageIndex].pdfBounds;
        return LimitedPageRenderScaleBucket(bounds, pdfTab->m_viewState.rotation, urpdf::PageRenderScale(std::abs(static_cast<double>(bounds.x1) - bounds.x0), std::abs(static_cast<double>(bounds.y1) - bounds.y0), pdfTab->m_viewState.rotation, rect.Width, rect.Height, pdfTab->m_renderZoom, GetRasterizationScale()));
    }

    bool MainWindow::ApplyPageBitmap(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t pageIndex, PageBitmap const& bitmap, int renderScaleBucket)
    {
        if (m_isClosed || renderScaleBucket <= 0 || GetPageRenderScaleBucket(PdfTab, pageIndex) != renderScaleBucket)
        {
            return false;
        }
        if (pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= PdfTab->m_renderedPages.size())
        {
            return false;
        }

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (!currentTab)
        {
            return false;
        }

        auto openedTab = m_openedTabs.find(currentTab);
        if (openedTab == m_openedTabs.end() || openedTab->second != PdfTab)
        {
            return false;
        }

        auto& slot = PdfTab->m_renderedPages[pageIndex];
        slot.bitmap = bitmap;
        slot.state = PageRenderState::ready;
        slot.renderScaleBucket = renderScaleBucket;
        slot.generation = PdfTab->m_renderGeneration.load(std::memory_order_acquire);

        if (auto element = PagesRepeater().TryGetElement(pageIndex))
        {
            ConfigurePageElement(element, static_cast<std::size_t>(pageIndex));
        }
        return true;
    }

    void MainWindow::RecordPageRenderMetrics(std::shared_ptr<PdfTabInfo> const& PdfTab, PageRenderMetrics const& metrics) noexcept
    {
        auto& documentMetrics = PdfTab->m_renderMetrics;
        documentMetrics.lastRenderAttempt = metrics;
        documentMetrics.totalRenderMs += metrics.elapsedMs;
        switch (metrics.status)
        {
        case PageRenderOutcome::applied:
            ++documentMetrics.appliedRenderCount;
            break;
        case PageRenderOutcome::discarded:
            ++documentMetrics.discardedRenderCount;
            break;
        case PageRenderOutcome::error:
            ++documentMetrics.failedRenderCount;
            break;
        }

        if (metrics.pageNumber > 0 && static_cast<std::size_t>(metrics.pageNumber - 1) < PdfTab->m_renderedPages.size())
        {
            PdfTab->m_renderedPages[metrics.pageNumber - 1].lastRenderMetrics = metrics;
        }
    }

    void MainWindow::RenderVisiblePagesAsync()
    {
        try
        {
            if (m_isClosed || m_isRendering || m_workCancelled->load(std::memory_order_acquire)) { return; }
            if (m_isVisibleRenderRunning)
            {
                return;
            }
            auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
            auto openedTab = m_openedTabs.find(currentTab);
            if (openedTab == m_openedTabs.end()) { return; }
            auto pdfTab = openedTab->second;
            const auto generation = pdfTab->m_renderGeneration.load(std::memory_order_acquire);
            const int rotation = NormalizeRotation(pdfTab->m_viewState.rotation);
            PageViewRange viewRange;
            const auto renderQueue = BuildRenderQueue(pdfTab->m_pagecount, viewRange);
            pdfTab->m_retainedPageFirst = viewRange.keepFirst;
            pdfTab->m_retainedPageLast = viewRange.keepLast;
            ReleaseSlotsOutsideRange(pdfTab, viewRange.keepFirst, viewRange.keepLast);
            PageBitmapCacheViewContext viewContext{ pdfTab->m_documentId, viewRange.keepFirst, viewRange.keepLast, rotation, {} };
            for (int32_t i = viewRange.keepFirst; i <= viewRange.keepLast; ++i)
            {
                viewContext.renderScaleBuckets.push_back(GetPageRenderScaleBucket(pdfTab, i));
            }
            m_pageBitmapCache.SetViewContext(viewContext);
            m_pageBitmapCache.TrimToBudget();

            for (auto const& request : renderQueue)
            {
                const int32_t pageIndex = request.pageIndex;
                if (pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= pdfTab->m_renderedPages.size()) { continue; }
                const int bucket = GetPageRenderScaleBucket(pdfTab, pageIndex);
                if (bucket == 0) { continue; }
                auto& slot = pdfTab->m_renderedPages[pageIndex];
                if (slot.generation == generation && slot.renderScaleBucket == bucket
                    && ((slot.state == PageRenderState::ready && slot.bitmap) || slot.state == PageRenderState::error))
                {
                    if (slot.state == PageRenderState::ready && slot.bitmap) { ++pdfTab->m_slotBitmapReuses; }
                    continue;
                }
                const PageBitmapCacheKey key{ pdfTab->m_documentId, pageIndex, rotation, bucket };
                if (auto cached = m_pageBitmapCache.TryGetBitmap(key))
                {
                    ApplyPageBitmap(pdfTab, pageIndex, *cached, bucket);
                    continue;
                }
                if (slot.IsRequestInFlight(generation, bucket, rotation)) { continue; }
                auto registration = m_runtime->Work().TryRegister();
                if (!registration) { return; }
                PageRenderMetrics metrics;
                metrics.documentId = pdfTab->m_documentId;
                metrics.pageNumber = pageIndex + 1;
                metrics.generation = generation;
                metrics.zoom = pdfTab->m_renderZoom;
                metrics.renderScale = urpdf::RenderScaleForBucket(bucket);
                metrics.renderScaleBucket = bucket;
                metrics.rotation = rotation;
                if (!slot.bitmap) { slot.state = PageRenderState::loading; }
                slot.requestInFlight = true;
                slot.requestGeneration = generation;
                slot.requestRenderScaleBucket = bucket;
                slot.requestRotation = rotation;
                m_isVisibleRenderRunning = true;
                auto weakTab = std::weak_ptr<PdfTabInfo>(pdfTab);
                try
                {
                    auto context = pdfTab->m_runtime->CloneContext();
                    const auto bounds = slot.pdfBounds;
                    auto cancelled = m_workCancelled;
                    auto dispatcher = DispatcherQueue();
                    QueueMuPdfWork(std::move(registration),
                        [pdfTab = std::move(pdfTab), context = std::move(context), cancelled, metrics, bounds, pageIndex]() mutable
                        {
                            PageRenderResult result;
                            result.metrics = metrics;
                            if (cancelled->load(std::memory_order_acquire)) { return result; }
                            urpdf::RunDocumentOperation(pdfTab->m_documentMutex, pdfTab->m_renderGeneration, metrics.generation, [&]
                                {
                                    if (cancelled->load(std::memory_order_acquire)) { return; }
                                    const auto start = std::chrono::steady_clock::now();
                                    result.bitmap = RenderPageToSoftwareBitmap(context.get(), pdfTab->m_doc, pageIndex,
                                        bounds, metrics.renderScaleBucket, metrics.rotation);
                                    result.metrics.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                                    result.rendered = true;
                                });
                            result.metrics.bitmapCreated = static_cast<bool>(result.bitmap);
                            if (result.bitmap)
                            {
                                result.metrics.pixelWidth = result.bitmap->source.PixelWidth();
                                result.metrics.pixelHeight = result.bitmap->source.PixelHeight();
                                result.metrics.pixelCount = static_cast<std::uint64_t>(result.metrics.pixelWidth) * result.metrics.pixelHeight;
                                result.metrics.estimatedBufferBytes = result.metrics.pixelCount * 4ull;
                            }
                            return result;
                        },
                        [weakThis = get_weak(), weakTab, cancelled, dispatcher, metrics]
                        (std::optional<PageRenderResult> result, std::exception_ptr error)
                        {
                            PageRenderResult completed;
                            completed.metrics = metrics;
                            if (result) { completed = std::move(*result); }
                            TryPublishResult(dispatcher, cancelled,
                                [weakThis, weakTab, cancelled, completed = std::move(completed), error]
                                {
                                    auto self = weakThis.get();
                                    if (!self || cancelled->load(std::memory_order_acquire)) { return; }
                                    self->CompletePageRender(weakTab, completed.metrics, completed.bitmap, completed.rendered, error);
                                });
                        });
                }
                catch (...)
                {
                    CompletePageRender(weakTab, metrics, std::nullopt, false, std::current_exception(), false);
                }
                return;
            }
        }
        catch (...)
        {
            m_isVisibleRenderRunning = false;
        }
    }

    void MainWindow::CompletePageRender(std::weak_ptr<PdfTabInfo> const& weakTab, PageRenderMetrics metrics,
        std::optional<SoftwarePageBitmap> const& bitmap, bool rendered, std::exception_ptr error, bool scheduleNext)
    {
        try
        {
            m_isVisibleRenderRunning = false;
            if (m_isClosed) { return; }
            auto pdfTab = weakTab.lock();
            const bool belongsToWindow = pdfTab && std::any_of(m_openedTabs.begin(), m_openedTabs.end(),
                [&](auto const& entry) { return entry.second == pdfTab; });
            if (belongsToWindow && metrics.pageNumber > 0
                && static_cast<std::size_t>(metrics.pageNumber - 1) < pdfTab->m_renderedPages.size())
            {
                const int pageIndex = metrics.pageNumber - 1;
                auto& slot = pdfTab->m_renderedPages[pageIndex];
                if (slot.IsRequestInFlight(metrics.generation, metrics.renderScaleBucket, metrics.rotation))
                {
                    slot.requestInFlight = false;
                }
                const bool current = pdfTab->m_renderGeneration.load(std::memory_order_acquire) == metrics.generation
                    && NormalizeRotation(pdfTab->m_viewState.rotation) == metrics.rotation
                    && GetPageRenderScaleBucket(pdfTab, pageIndex) == metrics.renderScaleBucket;
                metrics.status = PageRenderOutcome::discarded;
                if (current)
                {
                    try
                    {
                        if (error) { std::rethrow_exception(error); }
                        if (rendered && bitmap)
                        {
                            imaging::WriteableBitmap writeable(bitmap->source.PixelWidth(), bitmap->source.PixelHeight());
                            bitmap->source.CopyToBuffer(writeable.PixelBuffer());
                            writeable.Invalidate();
                            const PageBitmap pageBitmap{ writeable, bitmap->geometry };
                            m_pageBitmapCache.StoreBitmap(PageBitmapCacheKey{ pdfTab->m_documentId, pageIndex,
                                metrics.rotation, metrics.renderScaleBucket }, pageBitmap);
                            metrics.status = ApplyPageBitmap(pdfTab, pageIndex, pageBitmap, metrics.renderScaleBucket)
                                ? PageRenderOutcome::applied : PageRenderOutcome::discarded;
                        }
                        else if (rendered)
                        {
                            metrics.status = PageRenderOutcome::error;
                        }
                    }
                    catch (...)
                    {
                        metrics.status = PageRenderOutcome::error;
                    }
                    if (metrics.status == PageRenderOutcome::error)
                    {
                        slot.generation = metrics.generation;
                        slot.renderScaleBucket = metrics.renderScaleBucket;
                        slot.state = PageRenderState::error;
                    }
                }
                RecordPageRenderMetrics(pdfTab, metrics);
            }
            if (scheduleNext) { RenderVisiblePagesAsync(); }
        }
        catch (...)
        {
            m_isVisibleRenderRunning = false;
        }
    }

    bool MainWindow::TryAcquireRenderingLock()
    {
        if (m_isClosed || m_workCancelled->load(std::memory_order_acquire) || m_isRendering)
        {
            return false;
        }

        SaveViewState(m_presentedTab.lock());
        CancelPendingZoomRender();
        m_isRendering = true;
        UpdateTitleBarPassthroughRegions();
        m_zoomWasEnabled = PdfScrollViewer().ZoomMode() == controls::ZoomMode::Enabled;
        PdfScrollViewer().ZoomMode(controls::ZoomMode::Disabled);
        ZoomInButton().IsEnabled(false);
        ZoomOutButton().IsEnabled(false);
        RotateButton().IsEnabled(false);
        return true;
    }

    void MainWindow::ReleaseRenderingLock()
    {
        if (m_zoomWasEnabled)
        {
            PdfScrollViewer().ZoomMode(controls::ZoomMode::Enabled);
        }
        m_zoomWasEnabled = false;
        m_isRendering = false;
        UpdateTitleBarPassthroughRegions();
        ZoomInButton().IsEnabled(true);
        ZoomOutButton().IsEnabled(true);
        RotateButton().IsEnabled(true);
        if (m_dpiRefreshPending) { RequestDpiRefresh(); }
        SchedulePendingOpens();
    }

    void MainWindow::RebuildPagesContainer(std::shared_ptr<PdfTabInfo> const& pdfTab, std::vector<PageSlot> const& pages, urpdf::ViewStateGuard::Operation operation)
    {
        if (!IsCurrentViewOperation(pdfTab, operation)) { return; }
        ClearPageOverlays();
        m_presentedTab.reset();
        const int currentRotation = pdfTab->m_viewState.rotation;
        const bool bookView = pdfTab->m_viewState.bookView;

        auto pageSizes = winrt::single_threaded_vector<foundation::Size>();

        const bool swapDimensions = NormalizeRotation(currentRotation) == 90 || NormalizeRotation(currentRotation) == 270;
        constexpr double dipPerPdfPoint = 96.0 / 72.0;

        for (auto const& page : pages)
        {
            const double pdfWidth = std::abs(static_cast<double>(page.pdfBounds.x1 - page.pdfBounds.x0));
            const double pdfHeight = std::abs(static_cast<double>(page.pdfBounds.y1 - page.pdfBounds.y0));
            const double width = (swapDimensions ? pdfHeight : pdfWidth) * dipPerPdfPoint;
            const double height = (swapDimensions ? pdfWidth : pdfHeight) * dipPerPdfPoint;
            pageSizes.Append(foundation::Size{static_cast<float>((std::max)(1.0, width)), static_cast<float>((std::max)(1.0, height))});
        }

        m_layoutRotation = NormalizeRotation(currentRotation);
        PagesLayout().ColumnCount(bookView ? 2 : 1);
        PagesLayout().PageSizes(pageSizes.GetView());

        m_pageRepeaterItems = winrt::single_threaded_observable_vector<foundation::IInspectable>();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            m_pageRepeaterItems.Append(winrt::box_value(static_cast<int32_t>(i)));
        }

        PagesRepeater().ItemsSource(m_pageRepeaterItems);
        PdfScrollViewer().UpdateLayout();
        if (!IsCurrentViewOperation(pdfTab, operation)) { return; }
        m_presentedTab = pdfTab;

        for (int32_t i = 0; i < static_cast<int32_t>(pages.size()); ++i)
        {
            if (auto element = PagesRepeater().TryGetElement(i))
            {
                ConfigurePageElement(element, static_cast<std::size_t>(i));
            }
        }
    }

    void MainWindow::ReleasePageOverlay(xaml::UIElement const& element)
    {
        auto found = m_pageOverlays.find(element);
        if (found != m_pageOverlays.end())
        {
            auto& entry = *found->second;
            entry.sizeChangedRevoker.revoke();
            entry.state.Clear();
            entry.bitmap.reset();
            entry.document.reset();
            m_pageOverlays.erase(found);
        }
        if (auto parts = TryGetPageElementParts(element)) { ClearPageElement(*parts); }
    }

    void MainWindow::ClearPageOverlays(std::uint64_t documentId)
    {
        if (m_lastPageTextClick)
        {
            const auto document = m_lastPageTextClick->document.lock();
            if (!document || documentId == 0 || document->m_documentId == documentId) { m_lastPageTextClick.reset(); }
        }
        if (m_pageTextDrag)
        {
            auto document = m_pageTextDrag->press.document.lock();
            if (documentId == 0 || (document && document->m_documentId == documentId)) { EndPageTextDrag(); }
        }
        std::vector<xaml::UIElement> elements;
        elements.reserve(m_pageOverlays.size());
        for (auto const& [element, entry] : m_pageOverlays)
        {
            const auto binding = entry->state.CurrentBinding();
            if (documentId == 0 || (binding && binding->documentId == documentId)) { elements.push_back(element); }
        }
        for (auto const& element : elements) { ReleasePageOverlay(element); }
    }

    void MainWindow::RefreshPageOverlay(xaml::UIElement const& element, urpdf::PageOverlayState::Binding binding)
    {
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end() || !found->second->state.IsCurrent(binding)) { return; }
        auto& entry = *found->second;
        auto document = entry.document.lock();
        auto tab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto opened = m_openedTabs.find(tab);
        if (m_isClosed || !document || document->m_documentId != binding.documentId
            || document->m_renderGeneration.load(std::memory_order_acquire) != binding.generation
            || opened == m_openedTabs.end() || opened->second != document
            || m_layoutRotation != NormalizeRotation(document->m_viewState.rotation)
            || PagesRepeater().GetElementIndex(element) != binding.pageIndex)
        {
            ReleasePageOverlay(element);
            return;
        }
        const auto parts = TryGetPageElementParts(element);
        if (!parts) { ReleasePageOverlay(element); return; }
        if (!entry.bitmap || parts->image.Source() != entry.bitmap->source)
        {
            entry.bitmap.reset();
            entry.state.ClearRaster(binding);
            ClearPageOverlay(*parts, &entry);
            UpdatePageOverlayClip(*parts);
            return;
        }

        const urpdf::ElementSize size{ parts->content.ActualWidth(), parts->content.ActualHeight() };
        const auto previousSize = entry.state.DisplaySize();
        if (size.width != previousSize.width || size.height != previousSize.height)
        {
            // Page-specific features rebuild their drawings for the new mapping.
            ClearPageOverlay(*parts, &entry);
        }
        entry.state.Resize(binding, size);
        auto const& mapping = entry.state.Mapping();
        UpdatePageOverlayClip(*parts, mapping);
        if (!mapping) { return; }
        if (!entry.imageAreasBuilt)
        {
            DrawPageImageAreas(parts->imageAreas, *mapping, *document, binding.pageIndex);
            entry.imageAreasBuilt = true;
        }
        if (!entry.searchHighlightsBuilt)
        {
            DrawPageSearchMatches(parts->searchHighlights, *mapping, *document, binding.pageIndex);
            entry.searchHighlightsBuilt = true;
        }
        if (!entry.selectionBuilt)
        {
            DrawPageSelection(parts->highlights, *mapping, *document, binding.pageIndex);
            entry.selectionBuilt = true;
        }
        if (!entry.linkAreasBuilt
            && binding.pageIndex >= 0 && static_cast<std::size_t>(binding.pageIndex) < document->m_pageLinks.size()
            && document->m_pageLinks[binding.pageIndex].State() == urpdf::PageLinksState::ready)
        {
            BuildPageLinkAreas(element, entry, document, binding);
        }
    }

    void MainWindow::RefreshRealizedPageOverlays()
    {
        // Snapshot only realized elements; configuration may release entries.
        std::vector<xaml::UIElement> elements;
        elements.reserve(m_pageOverlays.size());
        for (auto const& [element, entry] : m_pageOverlays) { elements.push_back(element); }
        for (auto const& element : elements)
        {
            const auto index = PagesRepeater().GetElementIndex(element);
            if (index < 0) { ReleasePageOverlay(element); }
            else { ConfigurePageElement(element, static_cast<std::size_t>(index)); }
        }
    }

    std::optional<urpdf::PageCoordinateMapping> MainWindow::TryGetPageCoordinateMapping(int32_t pageIndex)
    {
        if (m_isClosed || pageIndex < 0) { return std::nullopt; }
        auto element = PagesRepeater().TryGetElement(pageIndex);
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end()) { return std::nullopt; }
        const auto binding = found->second->state.CurrentBinding();
        if (!binding) { return std::nullopt; }
        RefreshPageOverlay(element, *binding);
        found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end() || !found->second->state.IsCurrent(*binding)) { return std::nullopt; }
        return found->second->state.Mapping();
    }

    void MainWindow::ConfigurePageElement(xaml::UIElement const& element, std::size_t pageIndex)
    {
        auto parts = TryGetPageElementParts(element);
        if (!parts) { ReleasePageOverlay(element); return; }
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto openedTab = m_openedTabs.find(currentTab);
        if (m_isClosed || openedTab == m_openedTabs.end()
            || pageIndex >= openedTab->second->m_renderedPages.size()
            || PagesRepeater().GetElementIndex(element) != static_cast<int32_t>(pageIndex))
        {
            ReleasePageOverlay(element);
            return;
        }

        auto document = openedTab->second;
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end())
        {
            found = m_pageOverlays.emplace(element, std::make_shared<PageOverlayElement>()).first;
        }
        auto entry = found->second;
        const auto previousBinding = entry->state.CurrentBinding();
        const auto binding = entry->state.Bind(document->m_documentId, static_cast<int>(pageIndex),
            document->m_renderGeneration.load(std::memory_order_acquire));
        if (!binding) { ReleasePageOverlay(element); return; }
        if (previousBinding != binding)
        {
            entry->sizeChangedRevoker.revoke();
            ClearPageElement(*parts, entry.get());
            entry->bitmap.reset();
            entry->document = document;
            entry->sizeChangedRevoker = parts->content.SizeChanged(winrt::auto_revoke,
                [weakThis = get_weak(), weakElement = winrt::make_weak(element),
                    weakEntry = std::weak_ptr<PageOverlayElement>(entry), binding = *binding](auto const&, auto const&)
                {
                    auto self = weakThis.get();
                    auto element = weakElement.get();
                    auto entry = weakEntry.lock();
                    if (!self || !element || !entry) { return; }
                    const auto found = self->m_pageOverlays.find(element);
                    if (found == self->m_pageOverlays.end() || found->second != entry) { return; }
                    self->RefreshPageOverlay(element, binding);
                });
        }

        auto const& page = document->m_renderedPages[pageIndex];
        // A failed/pending replacement may leave a valid older bitmap visible.
        if (!page.bitmap || !page.bitmap->source)
        {
            entry->bitmap.reset();
            entry->state.ClearRaster(*binding);
            ClearPageElement(*parts, entry.get());
            return;
        }
        const bool bitmapChanged = !entry->bitmap || entry->bitmap->source != page.bitmap->source;
        if (bitmapChanged) { ClearPageOverlay(*parts, entry.get()); }
        if (parts->image.Source() != page.bitmap->source)
        {
            controls::Image freshImage;
            freshImage.Stretch(media::Stretch::Uniform);
            freshImage.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
            freshImage.VerticalAlignment(xaml::VerticalAlignment::Stretch);
            freshImage.Source(page.bitmap->source);
            parts->image.Source(nullptr);
            parts->content.Children().SetAt(0, freshImage);
            parts->image = freshImage;
        }
        if (bitmapChanged || !entry->bitmap)
        {
            entry->bitmap = page.bitmap;
            entry->state.SetRaster(*binding, entry->bitmap->geometry,
                { parts->content.ActualWidth(), parts->content.ActualHeight() });
        }
        RefreshPageOverlay(element, *binding);
        EnsurePageLinks(document, static_cast<int32_t>(pageIndex));
        if ((document->m_textSelection && urpdf::PageSelectionCarets(*document->m_textSelection, binding->pageIndex))
            || (document->m_selectedImage && document->m_selectedImage->page == binding->pageIndex))
        {
            EnsurePageText(document, binding->pageIndex);
        }
        parts->border.Opacity(1.0);
        TryRecordFirstVisiblePage(document);
    }

    void MainWindow::EnsurePageLinks(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex)
    {
        if (m_isClosed || m_isPageLinksLoadRunning || m_workCancelled->load(std::memory_order_acquire)
            || pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= document->m_pageLinks.size()) { return; }
        auto& slot = document->m_pageLinks[pageIndex];
        if (slot.State() != urpdf::PageLinksState::notLoaded) { return; }
        auto registration = m_runtime->Work().TryRegister();
        if (!registration) { return; }
        const auto requestId = *slot.BeginLoad();
        m_isPageLinksLoadRunning = true;
        const auto generation = document->m_renderGeneration.load(std::memory_order_acquire);
        const auto documentId = document->m_documentId;
        const auto weakTab = std::weak_ptr<PdfTabInfo>(document);
        try
        {
            auto context = m_runtime->CloneContext();
            auto cancelled = m_workCancelled;
            auto dispatcher = DispatcherQueue();
            QueueMuPdfWork(std::move(registration),
                [document, context = std::move(context), cancelled, generation, pageIndex]() mutable
                {
                    PageLinksResult result;
                    if (cancelled->load(std::memory_order_acquire)) { return result; }
                    urpdf::RunDocumentOperation(document->m_documentMutex, document->m_renderGeneration, generation, [&]
                        {
                            if (cancelled->load(std::memory_order_acquire)) { return; }
                            result.links = urpdf::CollectPageLinks(context.get(), document->m_doc, pageIndex);
                            result.loaded = true;
                        });
                    return result;
                },
                [weakThis = get_weak(), weakTab, cancelled, dispatcher, documentId, pageIndex, requestId]
                (std::optional<PageLinksResult> result, std::exception_ptr error)
                {
                    PageLinksResult completed;
                    if (result) { completed = std::move(*result); }
                    TryPublishResult(dispatcher, cancelled,
                        [weakThis, weakTab, cancelled, documentId, pageIndex, requestId, completed = std::move(completed), error]() mutable
                        {
                            auto self = weakThis.get();
                            if (!self || cancelled->load(std::memory_order_acquire)) { return; }
                            self->CompletePageLinksLoad(weakTab, documentId, pageIndex, requestId,
                                std::move(completed.links), completed.loaded, error);
                        });
                });
        }
        catch (...)
        {
            CompletePageLinksLoad(weakTab, documentId, pageIndex, requestId, {}, false, std::current_exception());
        }
    }

    void MainWindow::CompletePageLinksLoad(std::weak_ptr<PdfTabInfo> const& weakTab, std::uint64_t documentId,
        int32_t pageIndex, std::uint64_t requestId, std::vector<urpdf::PageLink> links, bool loaded, std::exception_ptr error)
    {
        m_isPageLinksLoadRunning = false;
        if (m_isClosed) { return; }
        try
        {
            auto document = weakTab.lock();
            const bool belongsToWindow = document && document->m_documentId == documentId
                && std::any_of(m_openedTabs.begin(), m_openedTabs.end(), [&](auto const& tab) { return tab.second == document; });
            if (belongsToWindow && pageIndex >= 0 && static_cast<std::size_t>(pageIndex) < document->m_pageLinks.size())
            {
                document->m_pageLinks[pageIndex].Complete(requestId, std::move(links), loaded, error != nullptr);
            }
            // Also schedules pending pages after this job, including an explicit
            // retry if a generation change discarded it AFTER the last configure.
            // A transferred tab resets its pending slots before the new owner
            // configures them; request IDs reject any old owner's completion.
            RefreshRealizedPageOverlays();
        }
        catch (...)
        {
#ifdef _DEBUG
            OutputDebugStringW(L"urPDF: page link completion could not be applied.\n");
#endif
        }
    }

    void MainWindow::BuildPageLinkAreas(xaml::UIElement const& element, PageOverlayElement& entry,
        std::shared_ptr<PdfTabInfo> const& document, urpdf::PageOverlayState::Binding binding)
    {
        const auto parts = TryGetPageElementParts(element);
        const auto& mapping = entry.state.Mapping();
        if (!parts || !mapping) { return; }
        for (auto const& link : document->m_pageLinks[binding.pageIndex].Links())
        {
            const auto rect = mapping->PdfToElementRect(link.rect);
            if (!rect) { continue; }
            auto area = winrt::make<LinkHitArea>();
            area.Width(rect->width);
            area.Height(rect->height);
            controls::Canvas::SetLeft(area, rect->x);
            controls::Canvas::SetTop(area, rect->y);
            xaml::Automation::AutomationProperties::SetName(area, link.kind == urpdf::PageLinkKind::external
                ? winrt::to_hstring(link.uri) : L"Page " + winrt::to_hstring(link.targetPageIndex + 1));
            area.Tapped([weakThis = get_weak(), weakElement = winrt::make_weak(element), binding, link]
                (foundation::IInspectable const&, xaml::Input::TappedRoutedEventArgs const& args)
                {
                    auto self = weakThis.get();
                    auto element = weakElement.get();
                    if (!self || self->m_isClosed || !element) { return; }
                    auto overlay = self->m_pageOverlays.find(element);
                    if (overlay == self->m_pageOverlays.end() || !overlay->second->state.IsCurrent(binding)) { return; }
                    auto tab = self->MainTabView().SelectedItem().try_as<controls::TabViewItem>();
                    auto opened = self->m_openedTabs.find(tab);
                    if (opened == self->m_openedTabs.end() || opened->second->m_documentId != binding.documentId
                        || opened->second->m_renderGeneration.load(std::memory_order_acquire) != binding.generation) { return; }
                    self->m_lastPageTextClick.reset();
                    args.Handled(true);
                    if (link.kind == urpdf::PageLinkKind::external) { LaunchExternalUriAsync(link.uri); }
                    else { self->ScrollToPage(link.targetPageIndex + 1); }
                });
            parts->overlay.Children().Append(area);
        }
        entry.linkAreasBuilt = true;
    }

    void MainWindow::EnsurePageText(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex, bool prioritize)
    {
        if (m_isClosed || m_workCancelled->load(std::memory_order_acquire)
            || pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= document->m_pageText.size()) { return; }
        auto& slot = document->m_pageText[pageIndex];
        if (slot.State() != urpdf::PageTextState::notLoaded) { return; }
        if (m_isPageTextLoadRunning)
        {
            auto queued = std::find_if(m_pageTextLoadQueue.begin(), m_pageTextLoadQueue.end(),
                [&](auto const& item) { return item.second == pageIndex && item.first.lock() == document; });
            if (queued != m_pageTextLoadQueue.end())
            {
                if (!prioritize) { return; }
                m_pageTextLoadQueue.erase(queued);
            }
            if (prioritize) { m_pageTextLoadQueue.emplace_front(document, pageIndex); }
            else { m_pageTextLoadQueue.emplace_back(document, pageIndex); }
            return;
        }
        auto registration = m_runtime->Work().TryRegister();
        if (!registration) { return; }
        const auto requestId = *slot.BeginLoad();
        m_isPageTextLoadRunning = true;
        const auto generation = document->m_renderGeneration.load(std::memory_order_acquire);
        const auto documentId = document->m_documentId;
        const auto weakTab = std::weak_ptr<PdfTabInfo>(document);
        try
        {
            auto context = m_runtime->CloneContext();
            auto cancelled = m_workCancelled;
            auto dispatcher = DispatcherQueue();
            QueueMuPdfWork(std::move(registration),
                [document, context = std::move(context), cancelled, generation, pageIndex]() mutable
                {
                    PageTextResult result;
                    if (cancelled->load(std::memory_order_acquire)) { return result; }
                    urpdf::RunDocumentOperation(document->m_documentMutex, document->m_renderGeneration, generation, [&]
                        {
                            if (cancelled->load(std::memory_order_acquire)) { return; }
                            result.text = urpdf::LoadPageText(context.get(), document->m_doc, pageIndex);
                            result.loaded = true;
                        });
                    return result;
                },
                [weakThis = get_weak(), weakTab, cancelled, dispatcher, documentId, pageIndex, requestId]
                (std::optional<PageTextResult> result, std::exception_ptr error)
                {
                    PageTextResult completed;
                    if (result) { completed = std::move(*result); }
                    TryPublishResult(dispatcher, cancelled,
                        [weakThis, weakTab, cancelled, documentId, pageIndex, requestId, completed = std::move(completed), error]() mutable
                        {
                            auto self = weakThis.get();
                            if (!self || cancelled->load(std::memory_order_acquire)) { return; }
                            self->CompletePageTextLoad(weakTab, documentId, pageIndex, requestId,
                                std::move(completed.text), completed.loaded, error);
                        });
                });
        }
        catch (...)
        {
            CompletePageTextLoad(weakTab, documentId, pageIndex, requestId, {}, false, std::current_exception());
        }
    }

    void MainWindow::StartNextPageTextLoad()
    {
        while (!m_isClosed && !m_isPageTextLoadRunning && !m_pageTextLoadQueue.empty())
        {
            auto [weakTab, pageIndex] = m_pageTextLoadQueue.front();
            m_pageTextLoadQueue.pop_front();
            auto document = weakTab.lock();
            if (document && std::any_of(m_openedTabs.begin(), m_openedTabs.end(), [&](auto const& tab) { return tab.second == document; }))
            {
                EnsurePageText(document, pageIndex);
            }
        }
    }

    void MainWindow::CompletePageTextLoad(std::weak_ptr<PdfTabInfo> const& weakTab, std::uint64_t documentId,
        int32_t pageIndex, std::uint64_t requestId, urpdf::PageText text, bool loaded, std::exception_ptr error)
    {
        m_isPageTextLoadRunning = false;
        if (m_isClosed) { return; }
        try
        {
            auto document = weakTab.lock();
            const bool belongsToWindow = document && document->m_documentId == documentId
                && std::any_of(m_openedTabs.begin(), m_openedTabs.end(), [&](auto const& tab) { return tab.second == document; });
            if (belongsToWindow && pageIndex >= 0 && static_cast<std::size_t>(pageIndex) < document->m_pageText.size()
                && document->m_pageText[pageIndex].Complete(requestId, std::move(text), loaded, error != nullptr))
            {
                const auto state = document->m_pageText[pageIndex].State();
                auto const& word = document->m_wordSelectionPoints;
                const bool wordEndpoint = word && (word->anchor.page == pageIndex || word->focus.page == pageIndex);
                if (state == urpdf::PageTextState::ready || state == urpdf::PageTextState::failed)
                {
                    if (wordEndpoint) { UpdateWordTextSelection(document); }
                    else { ResolvePendingTextPositions(document, pageIndex); }
                    ResolvePendingImageClick(document, pageIndex);
                    RefreshPageTextSelection(document, pageIndex);
                    RefreshPageImageSelection(document, pageIndex);
                }
                else if ((document->m_pendingTextAnchor && document->m_pendingTextAnchor->page == pageIndex)
                    || (document->m_pendingTextFocus && document->m_pendingTextFocus->page == pageIndex)
                    || (document->m_pendingImageClick && document->m_pendingImageClick->page == pageIndex)
                    || wordEndpoint)
                {
                    // The render generation changed before a newly requested caret load ran.
                    EnsurePageText(document, pageIndex, true);
                }
            }
        }
        catch (...)
        {
#ifdef _DEBUG
            OutputDebugStringW(L"urPDF: page text completion could not be applied.\n");
#endif
        }
        try { StartNextPageTextLoad(); } catch (...) {}
        if (m_pendingSelectionCopy)
        {
            // Starting a load can fail synchronously. Resume on the dispatcher so
            // a run of failed pages cannot recurse through load completion.
            DispatcherQueue().TryEnqueue([weakThis = get_weak()]
            {
                if (auto self = weakThis.get()) { self->ContinueSelectionCopy(); }
            });
        }
    }

    // DOCUMENT SEARCH
    bool MainWindow::OwnsDocument(std::shared_ptr<PdfTabInfo> const& document, std::uint64_t documentId) const noexcept
    {
        return document && documentId != 0 && document->m_documentId == documentId
            && std::any_of(m_openedTabs.begin(), m_openedTabs.end(),
                [&](auto const& tab) { return tab.second == document; });
    }

    void MainWindow::CancelPendingSearchDebounce(std::shared_ptr<PdfTabInfo> const& document) noexcept
    {
        if (document && m_pendingSearchTab.lock() != document) { return; }
        if (m_searchDebounceTimer)
        {
            try { m_searchDebounceTimer.Stop(); } catch (...) {}
        }
        m_pendingSearchTab.reset();
        m_pendingSearchQuery.clear();
    }

    void MainWindow::CancelActiveSearchSession(std::shared_ptr<PdfTabInfo> const& document) noexcept
    {
        if (!document || !document->m_searchCancelled) { return; }
        document->m_searchCancelled->store(true, std::memory_order_release);
        document->m_searchCancelled.reset();
    }

    void MainWindow::ScheduleDocumentSearch(std::shared_ptr<PdfTabInfo> const& document, std::wstring query)
    {
        if (m_isClosed || !document || !OwnsDocument(document, document->m_documentId)) { return; }
        document->m_searchDraftQuery = query;
        if (urpdf::IsBlankSearchQuery(query))
        {
            ClearDocumentSearch(document);
            return;
        }
        if (m_searchDebounceTimer)
        {
            m_searchDebounceTimer.Stop();
            m_pendingSearchTab = document;
            m_pendingSearchQuery = std::move(query);
            m_searchDebounceTimer.Start();
            return;
        }
        StartDocumentSearch(document, query);
    }

    void MainWindow::ExecutePendingDebouncedSearch()
    {
        auto document = m_pendingSearchTab.lock();
        auto query = std::move(m_pendingSearchQuery);
        m_pendingSearchTab.reset();
        m_pendingSearchQuery.clear();
        if (m_isClosed || !document || !OwnsDocument(document, document->m_documentId)) { return; }
        StartDocumentSearch(document, query);
    }

    void MainWindow::TriggerImmediateDocumentSearch(std::shared_ptr<PdfTabInfo> const& document, std::wstring_view query)
    {
        if (m_isClosed || !document || !OwnsDocument(document, document->m_documentId)) { return; }
        CancelPendingSearchDebounce(document);
        document->m_searchDraftQuery.assign(query);

        if (urpdf::IsBlankSearchQuery(query))
        {
            ClearDocumentSearch(document);
            return;
        }

        if (document->m_search.Query() == query)
        {
            const auto state = document->m_search.State();
            if (state == urpdf::SearchStatus::ready)
            {
                NavigateToNextSearchMatch(document);
                return;
            }
            if (state == urpdf::SearchStatus::searching)
            {
                if (document->m_search.MatchCount() > 0)
                {
                    NavigateToNextSearchMatch(document);
                }
                return;
            }
        }

        StartDocumentSearch(document, query);
    }

    void MainWindow::StartDocumentSearch(std::shared_ptr<PdfTabInfo> const& document, std::wstring_view query)
    {
        if (m_isClosed || m_workCancelled->load(std::memory_order_acquire)
            || !document || !document->m_layoutLoaded || !document->m_doc
            || !OwnsDocument(document, document->m_documentId))
        {
            return;
        }

        CancelPendingSearchDebounce(document);
        document->m_searchDraftQuery.assign(query);

        if (urpdf::IsBlankSearchQuery(query))
        {
            ClearDocumentSearch(document);
            return;
        }

        CancelActiveSearchSession(document);

        if (document->m_pagecount <= 0)
        {
            if (const auto requestId = document->m_search.BeginSearch(query))
            {
                document->m_search.Complete(*requestId, true, false);
            }
            RefreshAllSearchHighlights(document);
            UpdateSearchBarStatus(document);
            return;
        }

        auto registration = m_runtime->Work().TryRegister();
        if (!registration) { return; }

        const auto requestIdOpt = document->m_search.BeginSearch(query);
        if (!requestIdOpt)
        {
            RefreshAllSearchHighlights(document);
            UpdateSearchBarStatus(document);
            return;
        }
        const std::uint64_t requestId = *requestIdOpt;

        auto sessionCancelled = std::make_shared<std::atomic<bool>>(false);
        document->m_searchCancelled = sessionCancelled;
        RefreshAllSearchHighlights(document);
        UpdateSearchBarStatus(document);

        const std::uint64_t documentId = document->m_documentId;
        const int pageCount = document->m_pagecount;
        const auto weakTab = std::weak_ptr<PdfTabInfo>(document);

        try
        {
            std::string utf8Needle = winrt::to_string(query);
            auto context = m_runtime->CloneContext();
            auto windowCancelled = m_workCancelled;
            auto dispatcher = DispatcherQueue();
            auto weakThis = get_weak();

            QueueMuPdfWork(std::move(registration),
                [document, context = std::move(context), windowCancelled, sessionCancelled, dispatcher,
                 weakThis, weakTab, documentId, requestId, pageCount, utf8Needle = std::move(utf8Needle)]() mutable
                {
                    DocumentSearchResult result;
                    std::vector<PageSearchBatch> pendingBatch;
                    pendingBatch.reserve(SearchPublishBatchPages);
                    bool publishedAny = false;

                    auto flushBatch = [&]() noexcept -> bool
                    {
                        if (pendingBatch.empty()) { return true; }
                        if (windowCancelled->load(std::memory_order_acquire)
                            || sessionCancelled->load(std::memory_order_acquire))
                        {
                            return false;
                        }
                        auto batchToPublish = std::move(pendingBatch);
                        pendingBatch.clear();
                        publishedAny = true;
                        return TryPublishResult(dispatcher, windowCancelled,
                            [weakThis, weakTab, windowCancelled, sessionCancelled, documentId, requestId,
                             batch = std::move(batchToPublish)]() mutable
                            {
                                auto self = weakThis.get();
                                if (!self || self->m_isClosed
                                    || windowCancelled->load(std::memory_order_acquire)
                                    || sessionCancelled->load(std::memory_order_acquire))
                                {
                                    return;
                                }
                                self->ApplyDocumentSearchBatch(weakTab, documentId, requestId, std::move(batch));
                            });
                    };

                    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex)
                    {
                        if (windowCancelled->load(std::memory_order_acquire)
                            || sessionCancelled->load(std::memory_order_acquire))
                        {
                            return result;
                        }

                        // Release m_documentMutex between pages so rendering and page interaction stay responsive.
                        // Intentionally independent of m_renderGeneration (zoom/rotation/DPI do not alter text).
                        std::vector<urpdf::SearchMatch> pageMatches;
                        {
                            std::lock_guard lock(document->m_documentMutex);
                            if (windowCancelled->load(std::memory_order_acquire)
                                || sessionCancelled->load(std::memory_order_acquire)
                                || !document->m_doc)
                            {
                                return result;
                            }
                            pageMatches = urpdf::SearchPage(context.get(), document->m_doc, pageIndex, utf8Needle.c_str());
                        }

                        if (!pageMatches.empty())
                        {
                            pendingBatch.push_back(PageSearchBatch{ pageIndex, std::move(pageMatches) });
                            if (!publishedAny || pendingBatch.size() >= SearchPublishBatchPages)
                            {
                                if (!flushBatch()) { return result; }
                            }
                        }
                    }

                    if (!flushBatch()) { return result; }
                    if (windowCancelled->load(std::memory_order_acquire)
                        || sessionCancelled->load(std::memory_order_acquire))
                    {
                        return result;
                    }

                    result.completed = true;
                    return result;
                },
                [weakThis, weakTab, windowCancelled, sessionCancelled, dispatcher, documentId, requestId]
                (std::optional<DocumentSearchResult> result, std::exception_ptr error)
                {
                    if (sessionCancelled->load(std::memory_order_acquire)) { return; }
                    const bool completed = result && result->completed;
                    TryPublishResult(dispatcher, windowCancelled,
                        [weakThis, weakTab, windowCancelled, sessionCancelled, documentId, requestId, completed, error]() mutable
                        {
                            auto self = weakThis.get();
                            if (!self || self->m_isClosed
                                || windowCancelled->load(std::memory_order_acquire)
                                || sessionCancelled->load(std::memory_order_acquire))
                            {
                                return;
                            }
                            self->CompleteDocumentSearch(weakTab, documentId, requestId, completed, error);
                        });
                });
        }
        catch (...)
        {
            CompleteDocumentSearch(weakTab, documentId, requestId, false, std::current_exception());
        }
    }

    void MainWindow::ClearDocumentSearch(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (!document) { return; }
        CancelPendingSearchDebounce(document);
        CancelActiveSearchSession(document);
        document->m_searchDraftQuery.clear();
        document->m_search.Clear();
        RefreshAllSearchHighlights(document);
        UpdateSearchBarStatus(document);
    }

    void MainWindow::CloseDocumentSearch(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (!document) { return; }
        document->m_searchBarOpen = false;
        ClearDocumentSearch(document);
    }

    void MainWindow::ResetPendingDocumentSearch(std::shared_ptr<PdfTabInfo> const& document) noexcept
    {
        if (!document) { return; }
        CancelPendingSearchDebounce(document);
        CancelActiveSearchSession(document);
        document->m_search.ResetPending();
    }

    void MainWindow::ApplyDocumentSearchBatch(std::weak_ptr<PdfTabInfo> const& weakTab, std::uint64_t documentId,
        std::uint64_t requestId, std::vector<PageSearchBatch> batches)
    {
        if (m_isClosed) { return; }
        auto document = weakTab.lock();
        if (!OwnsDocument(document, documentId)) { return; }
        if (document->m_search.State() != urpdf::SearchStatus::searching
            || document->m_search.RequestId() != requestId)
        {
            return;
        }

        const int32_t currentPage = CurrentSearchPageIndex(document);
        bool reachedCurrentOrLater = false;
        std::vector<int32_t> updatedPages;
        updatedPages.reserve(batches.size());

        for (auto& batch : batches)
        {
            const bool hadMatches = !batch.matches.empty();
            if (document->m_search.AppendPageMatches(requestId, batch.pageIndex, std::move(batch.matches)) && hadMatches)
            {
                updatedPages.push_back(batch.pageIndex);
                if (batch.pageIndex >= currentPage)
                {
                    reachedCurrentOrLater = true;
                }
            }
        }

        bool selectedNow = false;
        if (!document->m_search.ActiveMatchIndex() && reachedCurrentOrLater)
        {
            selectedNow = document->m_search.SelectInitialMatch(currentPage).has_value();
        }

        for (int32_t pageIndex : updatedPages)
        {
            RefreshPageSearchHighlights(document, pageIndex);
        }

        if (selectedNow)
        {
            if (auto const* active = document->m_search.ActiveMatch())
            {
                ScrollToSearchMatch(document, *active);
            }
        }

        UpdateSearchBarStatus(document);
    }

    void MainWindow::CompleteDocumentSearch(std::weak_ptr<PdfTabInfo> const& weakTab, std::uint64_t documentId,
        std::uint64_t requestId, bool completed, std::exception_ptr error)
    {
        if (m_isClosed) { return; }
        auto document = weakTab.lock();
        if (!OwnsDocument(document, documentId)) { return; }
        if (!document->m_search.Complete(requestId, completed, error != nullptr))
        {
            return;
        }

        document->m_searchCancelled.reset();

        if (document->m_search.State() == urpdf::SearchStatus::ready)
        {
            if (!document->m_search.ActiveMatchIndex() && document->m_search.MatchCount() > 0)
            {
                const int32_t currentPage = CurrentSearchPageIndex(document);
                if (document->m_search.SelectInitialMatch(currentPage))
                {
                    if (auto const* active = document->m_search.ActiveMatch())
                    {
                        RefreshPageSearchHighlights(document, active->pageIndex);
                        ScrollToSearchMatch(document, *active);
                    }
                }
            }
        }
        else
        {
            RefreshAllSearchHighlights(document);
        }

        UpdateSearchBarStatus(document);
    }

    int32_t MainWindow::CurrentSearchPageIndex(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (!document) { return 0; }
        if (document == SelectedPdfDocument())
        {
            if (const auto visible = VisiblePageNumber(document))
            {
                return (std::max)(*visible - 1, 0);
            }
        }
        return (std::max)(document->m_currentPageIndex - 1, 0);
    }

    void MainWindow::ScrollToSearchMatch(std::shared_ptr<PdfTabInfo> const& document, urpdf::SearchMatch const& match)
    {
        if (m_isClosed || !document || document != SelectedPdfDocument()) { return; }
        if (match.pageIndex < 0
            || static_cast<std::size_t>(match.pageIndex) >= document->m_pageBounds.size())
        {
            return;
        }

        std::vector<urpdf::PageRect> pages;
        urpdf::ViewportGeometry viewport;
        ReadViewGeometry(document, pages, viewport);
        if (static_cast<std::size_t>(match.pageIndex) >= pages.size()) { return; }

        auto const& rawBounds = document->m_pageBounds[match.pageIndex];
        const urpdf::PdfPageBounds pageBounds{ rawBounds.x0, rawBounds.y0, rawBounds.x1, rawBounds.y1 };
        const auto matchBounds = urpdf::TrySearchMatchBounds(match.rects, pageBounds);
        if (!matchBounds) { return; }

        const auto offsets = urpdf::CalculateSearchMatchScrollOffsets(
            *matchBounds, pageBounds, pages[match.pageIndex], viewport, document->m_viewState.rotation);
        if (!offsets) { return; }

        PdfScrollViewer().ChangeView(offsets->x, offsets->y, nullptr);
    }

    void MainWindow::NavigateToNextSearchMatch(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (m_isClosed || !document || document->m_search.MatchCount() == 0) { return; }
        const std::optional<int32_t> previousPage = document->m_search.ActiveMatch()
            ? std::optional<int32_t>{ document->m_search.ActiveMatch()->pageIndex }
            : std::nullopt;
        if (!document->m_search.NextMatch()) { return; }
        auto const* active = document->m_search.ActiveMatch();
        if (!active) { return; }
        if (previousPage && *previousPage != active->pageIndex)
        {
            RefreshPageSearchHighlights(document, *previousPage);
        }
        RefreshPageSearchHighlights(document, active->pageIndex);
        ScrollToSearchMatch(document, *active);
        UpdateSearchBarStatus(document);
    }

    void MainWindow::NavigateToPreviousSearchMatch(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (m_isClosed || !document || document->m_search.MatchCount() == 0) { return; }
        const std::optional<int32_t> previousPage = document->m_search.ActiveMatch()
            ? std::optional<int32_t>{ document->m_search.ActiveMatch()->pageIndex }
            : std::nullopt;
        if (!document->m_search.PreviousMatch()) { return; }
        auto const* active = document->m_search.ActiveMatch();
        if (!active) { return; }
        if (previousPage && *previousPage != active->pageIndex)
        {
            RefreshPageSearchHighlights(document, *previousPage);
        }
        RefreshPageSearchHighlights(document, active->pageIndex);
        ScrollToSearchMatch(document, *active);
        UpdateSearchBarStatus(document);
    }

    void MainWindow::RefreshPageSearchHighlights(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex)
    {
        if (m_isClosed || !document || pageIndex < 0) { return; }
        auto element = PagesRepeater().TryGetElement(pageIndex);
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end() || found->second->document.lock() != document) { return; }
        if (const auto binding = found->second->state.CurrentBinding())
        {
            found->second->searchHighlightsBuilt = false;
            RefreshPageOverlay(element, *binding);
        }
    }

    void MainWindow::RefreshAllSearchHighlights(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (m_isClosed || !document) { return; }
        std::vector<std::pair<xaml::UIElement, urpdf::PageOverlayState::Binding>> targets;
        targets.reserve(m_pageOverlays.size());
        for (auto const& [element, entry] : m_pageOverlays)
        {
            if (entry->document.lock() != document) { continue; }
            if (const auto binding = entry->state.CurrentBinding())
            {
                entry->searchHighlightsBuilt = false;
                targets.emplace_back(element, *binding);
            }
        }
        for (auto const& [element, binding] : targets)
        {
            RefreshPageOverlay(element, binding);
        }
    }

    void MainWindow::UpdateSearchBarStatus(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (m_isClosed || !document || document != SelectedPdfDocument()) { return; }

        const auto state = document->m_search.State();
        const std::size_t total = document->m_search.MatchCount();
        const auto active = document->m_search.ActiveMatchIndex();
        const bool isSearching = state == urpdf::SearchStatus::searching;

        SearchProgressRing().IsActive(isSearching);
        SearchProgressRing().Visibility(isSearching ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);

        std::wstring statusText;
        if (total > 0)
        {
            if (active)
            {
                statusText = std::to_wstring(*active + 1) + L" of " + std::to_wstring(total);
            }
            else
            {
                statusText = std::to_wstring(total);
            }
        }
        else if (isSearching)
        {
            statusText = L"Searching...";
        }
        else if (state == urpdf::SearchStatus::ready)
        {
            statusText = L"No results";
        }
        else if (state == urpdf::SearchStatus::failed)
        {
            statusText = L"Search failed";
        }

        SearchStatusText().Text(statusText);
        const bool canNavigate = total > 0;
        SearchPreviousButton().IsEnabled(canNavigate);
        SearchNextButton().IsEnabled(canNavigate);
    }

    bool MainWindow::CanUseDocumentSearch()
    {
        if (m_isClosed
            || SettingsOverlay().Visibility() == xaml::Visibility::Visible
            || RootFrame().Visibility() == xaml::Visibility::Visible)
        {
            return false;
        }
        auto document = SelectedPdfDocument();
        return document && document->m_layoutLoaded;
    }

    bool MainWindow::IsFocusInsideElement(xaml::DependencyObject const& container)
    {
        if (m_isClosed || !container) { return false; }
        auto root = MainWindowUI().XamlRoot();
        if (!root) { return false; }
        auto focused = xaml::Input::FocusManager::GetFocusedElement(root).try_as<xaml::DependencyObject>();
        while (focused)
        {
            if (focused == container) { return true; }
            focused = media::VisualTreeHelper::GetParent(focused);
        }
        return false;
    }

    void MainWindow::SyncSearchBarToDocument(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (m_isClosed) { return; }

        if (!document || !document->m_layoutLoaded)
        {
            SearchButton().IsEnabled(false);
            FitPageSplitButton().IsEnabled(false);
            FullscreenButton().IsEnabled(false);
            SearchBar().Visibility(xaml::Visibility::Collapsed);
            m_isSyncingSearchText = true;
            SearchTextBox().Text(L"");
            m_isSyncingSearchText = false;
            SearchProgressRing().IsActive(false);
            SearchProgressRing().Visibility(xaml::Visibility::Collapsed);
            SearchStatusText().Text(L"");
            SearchPreviousButton().IsEnabled(false);
            SearchNextButton().IsEnabled(false);
            return;
        }

        SearchButton().IsEnabled(true);
        FitPageSplitButton().IsEnabled(true);
        FullscreenButton().IsEnabled(true);
        SearchBar().Visibility(document->m_searchBarOpen ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);

        std::wstring const& queryText = !document->m_searchDraftQuery.empty()
            ? document->m_searchDraftQuery
            : document->m_search.Query();
        if (std::wstring_view{ SearchTextBox().Text() } != queryText)
        {
            m_isSyncingSearchText = true;
            SearchTextBox().Text(queryText);
            m_isSyncingSearchText = false;
        }

        if (document->m_searchBarOpen
            && document->m_search.State() == urpdf::SearchStatus::idle
            && !urpdf::IsBlankSearchQuery(queryText))
        {
            StartDocumentSearch(document, queryText);
            return;
        }

        UpdateSearchBarStatus(document);
    }

    void MainWindow::OpenSearchBar()
    {
        if (!CanUseDocumentSearch()) { return; }
        auto document = SelectedPdfDocument();
        if (!document) { return; }

        document->m_searchBarOpen = true;
        SyncSearchBarToDocument(document);
        if (SearchTextBox().Focus(xaml::FocusState::Programmatic))
        {
            SearchTextBox().SelectAll();
        }
        else
        {
            DispatcherQueue().TryEnqueue([weakThis = get_weak()]()
            {
                if (auto self = weakThis.get(); self && !self->m_isClosed
                    && self->SearchBar().Visibility() == xaml::Visibility::Visible)
                {
                    self->SearchTextBox().Focus(xaml::FocusState::Programmatic);
                    self->SearchTextBox().SelectAll();
                }
            });
        }
    }

    void MainWindow::CloseSearchBar()
    {
        if (m_isClosed) { return; }
        if (auto document = SelectedPdfDocument())
        {
            CloseDocumentSearch(document);
        }
        SearchBar().Visibility(xaml::Visibility::Collapsed);
        m_isSyncingSearchText = true;
        SearchTextBox().Text(L"");
        m_isSyncingSearchText = false;
        SearchProgressRing().IsActive(false);
        SearchProgressRing().Visibility(xaml::Visibility::Collapsed);
        SearchStatusText().Text(L"");
        SearchPreviousButton().IsEnabled(false);
        SearchNextButton().IsEnabled(false);
        PdfScrollViewer().Focus(xaml::FocusState::Programmatic);
    }

    void MainWindow::SearchButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        OpenSearchBar();
    }

    void MainWindow::FindInDocument_Invoked(xaml::Input::KeyboardAccelerator const&,
        xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (!CanUseDocumentSearch()) { return; }
        OpenSearchBar();
        args.Handled(true);
    }

    void MainWindow::FindNext_Invoked(xaml::Input::KeyboardAccelerator const&,
        xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (!CanUseDocumentSearch()) { return; }
        auto document = SelectedPdfDocument();
        if (!document || !document->m_searchBarOpen) { return; }

        std::wstring query{ SearchTextBox().Text() };
        TriggerImmediateDocumentSearch(document, query);
        args.Handled(true);
    }

    void MainWindow::FindPrevious_Invoked(xaml::Input::KeyboardAccelerator const&,
        xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (!CanUseDocumentSearch()) { return; }
        auto document = SelectedPdfDocument();
        if (!document || !document->m_searchBarOpen) { return; }

        std::wstring query{ SearchTextBox().Text() };
        const auto state = document->m_search.State();
        if (document->m_search.Query() == query
            && (state == urpdf::SearchStatus::ready
                || (state == urpdf::SearchStatus::searching && document->m_search.MatchCount() > 0)))
        {
            NavigateToPreviousSearchMatch(document);
        }
        else
        {
            TriggerImmediateDocumentSearch(document, query);
        }
        args.Handled(true);
    }

    void MainWindow::SearchTextBox_TextChanged(foundation::IInspectable const&, controls::TextChangedEventArgs const&)
    {
        if (m_isClosed || m_isSyncingSearchText) { return; }
        auto document = SelectedPdfDocument();
        if (!document || !document->m_searchBarOpen) { return; }

        std::wstring query{ SearchTextBox().Text() };
        ScheduleDocumentSearch(document, std::move(query));
    }

    void MainWindow::SearchTextBox_KeyDown(foundation::IInspectable const&, xaml::Input::KeyRoutedEventArgs const& args)
    {
        if (m_isClosed || args.Key() != winrt::Windows::System::VirtualKey::Enter) { return; }
        auto document = SelectedPdfDocument();
        if (!document || !document->m_searchBarOpen) { return; }

        const auto shiftState = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(
            winrt::Windows::System::VirtualKey::Shift);
        const bool shiftPressed = (shiftState & winrt::Windows::UI::Core::CoreVirtualKeyStates::Down)
            == winrt::Windows::UI::Core::CoreVirtualKeyStates::Down;

        std::wstring query{ SearchTextBox().Text() };
        const auto state = document->m_search.State();
        if (shiftPressed && document->m_search.Query() == query
            && (state == urpdf::SearchStatus::ready
                || (state == urpdf::SearchStatus::searching && document->m_search.MatchCount() > 0)))
        {
            NavigateToPreviousSearchMatch(document);
        }
        else
        {
            TriggerImmediateDocumentSearch(document, query);
        }
        args.Handled(true);
    }

    void MainWindow::SearchPreviousButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        if (auto document = SelectedPdfDocument())
        {
            NavigateToPreviousSearchMatch(document);
        }
    }

    void MainWindow::SearchNextButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        if (auto document = SelectedPdfDocument())
        {
            NavigateToNextSearchMatch(document);
        }
    }

    void MainWindow::SearchCloseButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        CloseSearchBar();
    }

    void MainWindow::ResolvePendingTextPositions(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex)
    {
        if (!document->m_textSelection) { return; }
        auto selection = *document->m_textSelection;
        auto const& slot = document->m_pageText[pageIndex];
        const auto resolve = [&](std::optional<urpdf::PendingTextPosition>& pending, urpdf::TextPosition& endpoint)
        {
            if (!pending || pending->page != pageIndex) { return; }
            if (slot.State() == urpdf::PageTextState::ready) { endpoint.caret = urpdf::HitTest(slot.Text(), pending->point); }
            pending.reset();
        };
        resolve(document->m_pendingTextAnchor, selection.anchor);
        resolve(document->m_pendingTextFocus, selection.focus);
        SetPageTextSelection(document, selection);
    }

    void MainWindow::UpdateWordTextSelection(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (!document->m_wordSelectionPoints) { return; }
        const auto [anchor, focus] = *document->m_wordSelectionPoints;
        auto const& anchorSlot = document->m_pageText[anchor.page];
        if (NeedsPageText(anchorSlot))
        {
            EnsurePageText(document, anchor.page, true);
            return;
        }

        auto const& anchorText = anchorSlot.Text();
        const auto word = urpdf::WordAt(anchorText, anchor.point);
        const auto anchorCaret = word ? word->first : urpdf::HitTest(anchorText, anchor.point);
        const urpdf::TextSelection anchorWord{ { anchor.page, anchorCaret }, { anchor.page, word ? word->second : anchorCaret } };

        auto const& focusSlot = document->m_pageText[focus.page];
        const bool focusPending = NeedsPageText(focusSlot);
        urpdf::TextCaretRange focusRange;
        if (word && !focusPending) { focusRange = urpdf::WordDragCarets(focusSlot.Text(), focus.point); }
        else
        {
            const auto caret = focusPending ? (focus.page < anchor.page ? 0u : urpdf::kPageEnd)
                : urpdf::HitTest(focusSlot.Text(), focus.point);
            focusRange = { caret, caret };
        }
        const auto selection = urpdf::ExtendWordSelection(anchorWord, focus.page, focusRange);
        const bool dragging = m_pageTextDrag && m_pageTextDrag->byWord && m_pageTextDrag->press.document.lock() == document;
        if (!dragging && !focusPending) { document->m_wordSelectionPoints.reset(); }
        SetPageTextSelection(document, selection.anchor != selection.focus ? std::optional{ selection } : std::nullopt);
        if (focusPending)
        {
            EnsurePageText(document, focus.page, true);
        }
    }

    void MainWindow::SetPageTextSelection(std::shared_ptr<PdfTabInfo> const& document, std::optional<urpdf::TextSelection> selection)
    {
        if (document->m_textSelection == selection) { return; }
        const auto previous = std::exchange(document->m_textSelection, selection);
        std::vector<int32_t> changedPages;
        for (auto const& [element, entry] : m_pageOverlays)
        {
            const auto binding = entry->state.CurrentBinding();
            if (!binding || entry->document.lock() != document) { continue; }
            const auto page = binding->pageIndex;
            const auto before = previous ? urpdf::PageSelectionCarets(*previous, page) : std::nullopt;
            const auto after = selection ? urpdf::PageSelectionCarets(*selection, page) : std::nullopt;
            if (before != after) { changedPages.push_back(page); }
        }
        for (auto page : changedPages)
        {
            RefreshPageTextSelection(document, page);
            if (selection && urpdf::PageSelectionCarets(*selection, page)) { EnsurePageText(document, page); }
        }
    }

    void MainWindow::RefreshPageTextSelection(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex)
    {
        auto element = PagesRepeater().TryGetElement(pageIndex);
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end() || found->second->document.lock() != document) { return; }
        if (const auto binding = found->second->state.CurrentBinding())
        {
            found->second->selectionBuilt = false;
            RefreshPageOverlay(element, *binding);
        }
    }

    void MainWindow::CancelSelectionCopy(std::shared_ptr<PdfTabInfo> const& document) noexcept
    {
        if (m_pendingSelectionCopy && m_pendingSelectionCopy->document.lock() == document) { m_pendingSelectionCopy.reset(); }
    }

    void MainWindow::ClearPageTextSelection(std::shared_ptr<PdfTabInfo> const& document)
    {
        CancelSelectionCopy(document);
        if (m_lastPageTextClick && m_lastPageTextClick->document.lock() == document) { m_lastPageTextClick.reset(); }
        document->m_pendingTextAnchor.reset();
        document->m_pendingTextFocus.reset();
        document->m_wordSelectionPoints.reset();
        SetPageTextSelection(document, std::nullopt);
        if (m_pageTextDrag && m_pageTextDrag->press.document.lock() == document) { EndPageTextDrag(); }
    }

    void MainWindow::SetSelectedPageImage(std::shared_ptr<PdfTabInfo> const& document,
        std::optional<urpdf::PageImageSelection> selection)
    {
        if (selection) { ClearPageTextSelection(document); }
        if (document->m_selectedImage == selection) { return; }
        const auto previous = std::exchange(document->m_selectedImage, selection);
        if (previous) { RefreshPageImageSelection(document, previous->page); }
        if (selection && (!previous || previous->page != selection->page))
        {
            RefreshPageImageSelection(document, selection->page);
            EnsurePageText(document, selection->page);
        }
    }

    void MainWindow::RefreshPageImageSelection(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex)
    {
        auto element = PagesRepeater().TryGetElement(pageIndex);
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end() || found->second->document.lock() != document) { return; }
        if (const auto binding = found->second->state.CurrentBinding())
        {
            found->second->imageAreasBuilt = false;
            RefreshPageOverlay(element, *binding);
        }
    }

    void MainWindow::ClearPageImageSelection(std::shared_ptr<PdfTabInfo> const& document)
    {
        if (m_contextMenuImage && m_contextMenuImage->document.lock() == document) { m_contextMenuImage.reset(); }
        document->m_pendingImageClick.reset();
        SetSelectedPageImage(document, std::nullopt);
    }

    void MainWindow::ResolvePendingImageClick(std::shared_ptr<PdfTabInfo> const& document, int32_t pageIndex)
    {
        if (!document->m_pendingImageClick || document->m_pendingImageClick->page != pageIndex) { return; }
        const auto pending = std::exchange(document->m_pendingImageClick, std::nullopt);
        auto const& slot = document->m_pageText[pageIndex];
        if (slot.State() != urpdf::PageTextState::ready || urpdf::IsOverText(slot.Text(), pending->point))
        {
            SetSelectedPageImage(document, std::nullopt);
            return;
        }
        if (const auto imageIndex = urpdf::HitTestImage(slot.Text().images, pending->point))
        {
            SetSelectedPageImage(document, urpdf::PageImageSelection{ pageIndex, *imageIndex });
        }
        else
        {
            SetSelectedPageImage(document, std::nullopt);
        }
    }

    std::shared_ptr<PdfTabInfo> MainWindow::SelectedPdfDocument()
    {
        if (m_isClosed) { return nullptr; }
        const auto tab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        const auto found = m_openedTabs.find(tab);
        return found != m_openedTabs.end() ? found->second : nullptr;
    }

    void MainWindow::SelectAll()
    {
        const auto document = SelectedPdfDocument();
        if (!document || document->m_pagecount <= 0) { return; }

        const urpdf::TextSelection all{ { 0, 0 }, { document->m_pagecount - 1, urpdf::kPageEnd } };
        if (document->m_textSelection == all && !document->m_pendingTextAnchor && !document->m_pendingTextFocus
            && !document->m_wordSelectionPoints && !m_pageTextDrag) { return; }

        ClearPageImageSelection(document);
        ClearPageTextSelection(document);
        // The sentinel does not require loading the last page. The same overlay path
        // used for dragging loads text only for realized pages until a copy is requested.
        SetPageTextSelection(document, all);
    }

    bool MainWindow::HasSelection()
    {
        const auto document = SelectedPdfDocument();
        return document && document->m_textSelection
            && document->m_textSelection->anchor != document->m_textSelection->focus;
    }

    bool MainWindow::HasImageSelection()
    {
        const auto document = SelectedPdfDocument();
        return document && document->m_selectedImage.has_value();
    }

    bool MainWindow::CopySelectionText()
    {
        const auto document = SelectedPdfDocument();
        if (!document) { return false; }

        auto const& selection = document->m_textSelection;
        auto const& word = document->m_wordSelectionPoints;
        const bool pendingDrag = document->m_pendingTextFocus && document->m_pendingTextFocus != document->m_pendingTextAnchor;
        if (!word && (!selection || (selection->anchor == selection->focus && !pendingDrag))) { return false; }

        const auto firstPage = word ? (std::min)(word->anchor.page, word->focus.page) : selection->InDocumentOrder().first.page;
        const auto clipboardSequence = GetClipboardSequenceNumber();
        if (!DispatcherQueue().TryEnqueue([weakThis = get_weak()]
            {
                if (auto self = weakThis.get()) { self->ContinueSelectionCopy(); }
            })) { return false; }

        m_pendingSelectionCopy = PendingSelectionCopy{ document, firstPage, clipboardSequence };
        return true;
    }

    bool MainWindow::CopySelectedImage()
    {
        const auto document = SelectedPdfDocument();
        if (!document || !document->m_selectedImage) { return false; }
        return QueueImageCopy(document, *document->m_selectedImage);
    }

    bool MainWindow::QueueImageCopy(std::shared_ptr<PdfTabInfo> const& document, urpdf::PageImageSelection selection)
    {
        if (m_isClosed || !document || m_workCancelled->load(std::memory_order_acquire)
            || selection.page < 0 || selection.page >= document->m_pagecount) { return false; }
        auto registration = m_runtime->Work().TryRegister();
        if (!registration) { return false; }
        const auto clipboardSequence = GetClipboardSequenceNumber();
        try
        {
            auto context = m_runtime->CloneContext();
            auto cancelled = m_workCancelled;
            auto dispatcher = DispatcherQueue();
            QueueMuPdfWork(std::move(registration),
                [document, context = std::move(context), cancelled, selection]() mutable
                {
                    std::vector<std::uint8_t> png;
                    if (cancelled->load(std::memory_order_acquire)) { return png; }
                    std::lock_guard lock(document->m_documentMutex);
                    if (cancelled->load(std::memory_order_acquire) || !document->m_doc) { return png; }
                    png = urpdf::ExtractPageImagePng(context.get(), document->m_doc, selection.page, selection.imageIndex);
                    return png;
                },
                [weakThis = get_weak(), cancelled, dispatcher, clipboardSequence]
                (std::optional<std::vector<std::uint8_t>> result, std::exception_ptr error)
                {
                    if (error || !result || result->empty())
                    {
#ifdef _DEBUG
                        OutputDebugStringW(L"urPDF: could not extract PDF image for clipboard.\n");
#endif
                        return;
                    }
                    TryPublishResult(dispatcher, cancelled,
                        [weakThis, cancelled, clipboardSequence, png = std::move(*result)]() mutable
                        {
                            auto self = weakThis.get();
                            if (!self || self->m_isClosed || cancelled->load(std::memory_order_acquire)) { return; }
                            self->CompleteImageCopy(std::move(png), clipboardSequence);
                        });
                });
            return true;
        }
        catch (...)
        {
#ifdef _DEBUG
            OutputDebugStringW(L"urPDF: could not queue PDF image copy.\n");
#endif
            return false;
        }
    }

    winrt::fire_and_forget MainWindow::CompleteImageCopy(std::vector<std::uint8_t> pngBytes, DWORD clipboardSequence)
    {
        auto strongThis = get_strong();
        try
        {
            if (m_isClosed || pngBytes.empty() || clipboardSequence != GetClipboardSequenceNumber()) { co_return; }

            using namespace winrt::Windows::Storage::Streams;
            using namespace winrt::Windows::ApplicationModel::DataTransfer;

            InMemoryRandomAccessStream stream;
            DataWriter writer(stream.GetOutputStreamAt(0));
            writer.WriteBytes(pngBytes);
            co_await writer.StoreAsync();
            co_await writer.FlushAsync();
            writer.DetachStream();
            stream.Seek(0);

            if (m_isClosed || clipboardSequence != GetClipboardSequenceNumber()) { co_return; }

            DataPackage data;
            data.RequestedOperation(DataPackageOperation::Copy);
            data.SetBitmap(RandomAccessStreamReference::CreateFromStream(stream));
            Clipboard::SetContent(data);
            Clipboard::Flush();
        }
        catch (...)
        {
            OutputDebugStringW(L"urPDF: could not copy PDF image to the clipboard.\n");
        }
    }

    foundation::IAsyncOperation<storage::StorageFile> MainWindow::PickImageSaveFileAsync(winrt::hstring const& suggestedFileName)
    {
        pickers::FileSavePicker picker;
        picker.SuggestedStartLocation(pickers::PickerLocationId::PicturesLibrary);
        picker.SuggestedFileName(suggestedFileName);
        picker.FileTypeChoices().Insert(L"PNG image", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));

        HWND hwnd{};
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);

        co_return co_await picker.PickSaveFileAsync();
    }

    void MainWindow::QueueImageSave(std::shared_ptr<PdfTabInfo> const& document, urpdf::PageImageSelection selection,
        storage::StorageFile file)
    {
        if (m_isClosed || !document || !file || m_workCancelled->load(std::memory_order_acquire)
            || selection.page < 0 || selection.page >= document->m_pagecount) { return; }
        auto registration = m_runtime->Work().TryRegister();
        if (!registration) { return; }
        try
        {
            auto context = m_runtime->CloneContext();
            auto cancelled = m_workCancelled;
            auto dispatcher = DispatcherQueue();
            QueueMuPdfWork(std::move(registration),
                [document, context = std::move(context), cancelled, selection]() mutable
                {
                    std::vector<std::uint8_t> png;
                    if (cancelled->load(std::memory_order_acquire)) { return png; }
                    std::lock_guard lock(document->m_documentMutex);
                    if (cancelled->load(std::memory_order_acquire) || !document->m_doc) { return png; }
                    png = urpdf::ExtractPageImagePng(context.get(), document->m_doc, selection.page, selection.imageIndex);
                    return png;
                },
                [weakThis = get_weak(), cancelled, dispatcher, file = std::move(file)]
                (std::optional<std::vector<std::uint8_t>> result, std::exception_ptr error) mutable
                {
                    if (error || !result || result->empty())
                    {
#ifdef _DEBUG
                        OutputDebugStringW(L"urPDF: could not extract PDF image for saving.\n");
#endif
                        return;
                    }
                    TryPublishResult(dispatcher, cancelled,
                        [weakThis, cancelled, file = std::move(file), png = std::move(*result)]() mutable
                        {
                            auto self = weakThis.get();
                            if (!self || self->m_isClosed || cancelled->load(std::memory_order_acquire)) { return; }
                            self->CompleteImageSave(std::move(file), std::move(png));
                        });
                });
        }
        catch (...)
        {
#ifdef _DEBUG
            OutputDebugStringW(L"urPDF: could not queue PDF image save.\n");
#endif
        }
    }

    winrt::fire_and_forget MainWindow::CompleteImageSave(storage::StorageFile file, std::vector<std::uint8_t> pngBytes)
    {
        auto strongThis = get_strong();
        try
        {
            if (m_isClosed || !file || pngBytes.empty()) { co_return; }
            storage::CachedFileManager::DeferUpdates(file);
            co_await storage::FileIO::WriteBytesAsync(file, pngBytes);
            co_await storage::CachedFileManager::CompleteUpdatesAsync(file);
        }
        catch (...)
        {
            OutputDebugStringW(L"urPDF: could not save PDF image.\n");
        }
    }

    void MainWindow::PdfScrollViewer_ContextRequested(xaml::UIElement const&, xaml::Input::ContextRequestedEventArgs const& args)
    {
        if (m_isClosed) { return; }
        const auto document = SelectedPdfDocument();
        if (!document || m_presentedTab.lock() != document) { return; }

        auto source = args.OriginalSource().try_as<xaml::DependencyObject>();
        while (source && source != PdfScrollViewer())
        {
            if (source.try_as<controls::Primitives::ScrollBar>()) { return; }
            source = media::VisualTreeHelper::GetParent(source);
        }

        std::optional<urpdf::PageImageSelection> targetImage;
        foundation::Point showPosition{};
        bool hasPosition = false;
        if (args.TryGetPosition(PdfScrollViewer(), showPosition))
        {
            hasPosition = true;
            if (const auto target = PageTextPositionAtPointer(document, showPosition, true))
            {
                if (target->page >= 0 && static_cast<std::size_t>(target->page) < document->m_pageText.size())
                {
                    auto const& slot = document->m_pageText[target->page];
                    if (slot.State() == urpdf::PageTextState::ready)
                    {
                        if (const auto imageIndex = urpdf::HitTestImage(slot.Text().images, target->point))
                        {
                            targetImage = urpdf::PageImageSelection{ target->page, *imageIndex };
                            SetSelectedPageImage(document, targetImage);
                        }
                    }
                    else if (NeedsPageText(slot))
                    {
                        EnsurePageText(document, target->page, true);
                    }
                }
            }
        }
        else if (document->m_selectedImage)
        {
            targetImage = document->m_selectedImage;
            if (const auto element = PagesRepeater().TryGetElement(targetImage->page))
            {
                const auto found = m_pageOverlays.find(element);
                const auto parts = TryGetPageElementParts(element);
                if (found != m_pageOverlays.end() && parts && found->second->state.Mapping()
                    && targetImage->page >= 0
                    && static_cast<std::size_t>(targetImage->page) < document->m_pageText.size()
                    && document->m_pageText[targetImage->page].State() == urpdf::PageTextState::ready)
                {
                    if (const auto* image = urpdf::FindPageImage(
                        document->m_pageText[targetImage->page].Text().images, targetImage->imageIndex))
                    {
                        if (const auto rect = found->second->state.Mapping()->PdfToElementRect(image->bounds))
                        {
                            const foundation::Point center{
                                static_cast<float>(rect->x + rect->width / 2.0),
                                static_cast<float>(rect->y + rect->height / 2.0) };
                            showPosition = parts->selection.TransformToVisual(PdfScrollViewer()).TransformPoint(center);
                            hasPosition = std::isfinite(showPosition.X) && std::isfinite(showPosition.Y);
                        }
                    }
                }
            }
        }

        if (!targetImage) { return; }

        m_contextMenuImage = ContextMenuImageTarget{ document, *targetImage };
        controls::Primitives::FlyoutShowOptions options;
        if (hasPosition) { options.Position(showPosition); }
        options.ShowMode(controls::Primitives::FlyoutShowMode::Standard);
        DocumentContextMenu().ShowAt(PdfScrollViewer(), options);
        args.Handled(true);
    }

    void MainWindow::CopyImageMenuItem_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        const auto target = std::exchange(m_contextMenuImage, std::nullopt);
        if (target)
        {
            if (auto document = target->document.lock())
            {
                QueueImageCopy(document, target->selection);
                return;
            }
        }
        CopySelectedImage();
    }

    winrt::fire_and_forget MainWindow::SaveImageAsMenuItem_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto strongThis = get_strong();
        std::shared_ptr<PdfTabInfo> document;
        std::optional<urpdf::PageImageSelection> selection;
        if (const auto target = std::exchange(m_contextMenuImage, std::nullopt))
        {
            document = target->document.lock();
            selection = target->selection;
        }
        else if (auto current = SelectedPdfDocument(); current && current->m_selectedImage)
        {
            document = std::move(current);
            selection = document->m_selectedImage;
        }
        if (!document || !selection) { co_return; }

        try
        {
            const winrt::hstring suggestedName{
                urpdf::SuggestedImageFileName(document->m_fileName, selection->page, selection->imageIndex) };
            auto file = co_await PickImageSaveFileAsync(suggestedName);
            if (!file || m_isClosed) { co_return; }
            QueueImageSave(document, *selection, std::move(file));
        }
        catch (...)
        {
            OutputDebugStringW(L"urPDF: could not open save dialog for PDF image.\n");
        }
    }

    void MainWindow::ClearPdfSelection_Invoked(xaml::Input::KeyboardAccelerator const&, xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (m_isClosed
            || SettingsOverlay().Visibility() == xaml::Visibility::Visible
            || RootFrame().Visibility() == xaml::Visibility::Visible)
        {
            return;
        }

        auto document = SelectedPdfDocument();
        if (!document) { document = m_presentedTab.lock(); }
        if (document)
        {
            const bool hasSelection = document->m_textSelection || document->m_wordSelectionPoints
                || document->m_selectedImage || document->m_pendingImageClick;

            if (document->m_searchBarOpen && (IsFocusInsideElement(SearchBar()) || !hasSelection))
            {
                CloseSearchBar();
                args.Handled(true);
                return;
            }

            if (hasSelection)
            {
                ClearPageTextSelection(document);
                ClearPageImageSelection(document);
                args.Handled(true);
                return;
            }
        }

        if (IsFullscreen())
        {
            ExitFullscreen();
            args.Handled(true);
        }
    }

    void MainWindow::PdfScrollViewer_Tapped(foundation::IInspectable const&, xaml::Input::TappedRoutedEventArgs const& args)
    {
        // Page taps are handled by the selection layer. Taps on viewer chrome
        // such as scroll bars must not clear the current selection.
        auto source = args.OriginalSource().try_as<xaml::DependencyObject>();
        if (source == PagesRepeater()) { return; }
        while (source && source != PdfScrollViewer())
        {
            if (source.try_as<winrt::urPDF::TextSelectionLayer>() || source.try_as<controls::Primitives::ScrollBar>()) { return; }
            source = media::VisualTreeHelper::GetParent(source);
        }
        m_lastPageTextClick.reset();
        if (auto document = m_presentedTab.lock())
        {
            ClearPageTextSelection(document);
            ClearPageImageSelection(document);
        }
    }

    void MainWindow::CopyPdfText_Invoked(xaml::Input::KeyboardAccelerator const&, xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (m_isClosed) { return; }
        const auto root = PdfScrollViewer().XamlRoot();
        if (!root) { return; }
        auto focused = xaml::Input::FocusManager::GetFocusedElement(root).try_as<xaml::DependencyObject>();
        while (focused)
        {
            if (focused.try_as<controls::TextBox>() || focused.try_as<controls::RichEditBox>()
                || focused.try_as<controls::PasswordBox>() || focused.try_as<controls::NumberBox>()) { return; }
            focused = media::VisualTreeHelper::GetParent(focused);
        }

        if (CopySelectionText() || CopySelectedImage()) { args.Handled(true); }
    }

    void MainWindow::ContinueSelectionCopy()
    {
        if (m_isClosed || !m_pendingSelectionCopy) { return; }
        auto document = m_pendingSelectionCopy->document.lock();
        if (!document || m_pendingSelectionCopy->clipboardSequence != GetClipboardSequenceNumber())
        {
            m_pendingSelectionCopy.reset();
            return;
        }
        auto const& word = document->m_wordSelectionPoints;
        if (word && (NeedsPageText(document->m_pageText[word->anchor.page]) || NeedsPageText(document->m_pageText[word->focus.page]))) { return; }
        if (!document->m_textSelection)
        {
            m_pendingSelectionCopy.reset();
            return;
        }
        try
        {
            const auto selection = *document->m_textSelection;
            const auto [begin, end] = selection.InDocumentOrder();
            if (begin.page < 0 || static_cast<std::size_t>(end.page) >= document->m_pageText.size())
            {
                m_pendingSelectionCopy.reset();
                return;
            }
            auto& nextPage = m_pendingSelectionCopy->nextPage;
            for (; nextPage <= end.page; ++nextPage)
            {
                const auto state = document->m_pageText[nextPage].State();
                if (state == urpdf::PageTextState::ready || state == urpdf::PageTextState::failed) { continue; }
                if (!urpdf::PageSelectionCarets(selection, nextPage)
                    && (!document->m_pendingTextAnchor || document->m_pendingTextAnchor->page != nextPage)
                    && (!document->m_pendingTextFocus || document->m_pendingTextFocus->page != nextPage)) { continue; }
                if (!m_isPageTextLoadRunning) { EnsurePageText(document, nextPage, true); }
                return;
            }
            const auto clipboardSequence = m_pendingSelectionCopy->clipboardSequence;
            m_pendingSelectionCopy.reset();
            const auto text = urpdf::SelectionText(document->m_pageText, selection);
            // While text was loading, the user may have copied from a text box or another app.
            if (text.empty() || clipboardSequence != GetClipboardSequenceNumber()) { return; }

            using namespace winrt::Windows::ApplicationModel::DataTransfer;
            DataPackage data;
            data.SetText(text);
            Clipboard::SetContent(data);
        }
        catch (...)
        {
            m_pendingSelectionCopy.reset();
            OutputDebugStringW(L"urPDF: could not copy PDF text to the clipboard.\n");
        }
    }

    void MainWindow::TextSelectionLayer_PointerEntered(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isClosed) { return; }
        auto layer = sender.as<winrt::urPDF::TextSelectionLayer>();
        auto found = m_pageOverlays.find(GetPageElementForTextSelectionLayer(layer));
        if (found == m_pageOverlays.end()) { return; }
        const auto binding = found->second->state.CurrentBinding();
        auto document = found->second->document.lock();
        UpdatePageTextCursor(layer, found->second.get(), args);
        if (document && binding) { EnsurePageText(document, binding->pageIndex); }
    }

    void MainWindow::TextSelectionLayer_PointerPressed(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isClosed || m_pageTextDrag) { return; }
        const auto previous = std::exchange(m_lastPageTextClick, std::nullopt);
        // LinkHitArea is in the Canvas above this layer, so its presses never
        // route through the selection layer.
        auto layer = sender.as<winrt::urPDF::TextSelectionLayer>();
        auto element = GetPageElementForTextSelectionLayer(layer);
        auto found = m_pageOverlays.find(element);
        if (found == m_pageOverlays.end()) { return; }
        const auto binding = found->second->state.CurrentBinding();
        const auto& mapping = found->second->state.Mapping();
        auto document = found->second->document.lock();
        if (!mapping || !binding || !document || binding->pageIndex < 0
            || static_cast<std::size_t>(binding->pageIndex) >= document->m_pageText.size()) { return; }

        const auto point = args.GetCurrentPoint(layer);
        const auto position = point.Position();
        const urpdf::ElementPoint elementPoint{ position.X, position.Y };
        if (!point.Properties().IsLeftButtonPressed() || mapping->IsElementPointInsidePage(elementPoint) != true) { return; }
        using winrt::Microsoft::UI::Input::PointerDeviceType;
        using winrt::Microsoft::UI::Input::PointerUpdateKind;
        const auto pointer = args.Pointer();
        const auto device = pointer.PointerDeviceType();
        if (device == PointerDeviceType::Mouse && point.Properties().PointerUpdateKind() != PointerUpdateKind::LeftButtonPressed) { return; }
        const auto pdfPoint = mapping->ElementToPdfPoint(elementPoint);
        if (!pdfPoint) { return; }

        const auto pageIndex = binding->pageIndex;
        auto const& slot = document->m_pageText[pageIndex];
        const auto pressed = args.GetCurrentPoint(PdfScrollViewer()).Position();
        const PageTextClick press{ document, pressed, point.Timestamp(), pageIndex };
        bool byWord = false;
        if (device == PointerDeviceType::Mouse && previous && previous->document.lock() == document
            && previous->page == pageIndex && press.timestamp >= previous->timestamp
            && press.timestamp - previous->timestamp <= static_cast<std::uint64_t>(GetDoubleClickTime()) * 1000)
        {
            const auto scale = GetRasterizationScale();
            const auto dpi = static_cast<UINT>(std::lround(USER_DEFAULT_SCREEN_DPI * scale));
            const auto halfWidth = GetSystemMetricsForDpi(SM_CXDOUBLECLK, dpi) / (2.0 * scale);
            const auto halfHeight = GetSystemMetricsForDpi(SM_CYDOUBLECLK, dpi) / (2.0 * scale);
            byWord = std::abs(pressed.X - previous->position.X) <= halfWidth && std::abs(pressed.Y - previous->position.Y) <= halfHeight;
        }
        document->m_pendingImageClick.reset();
        if (byWord) { ClearPageImageSelection(document); }
        ClearPageTextSelection(document);
        const bool directContact = device == PointerDeviceType::Touch || device == PointerDeviceType::Pen;
        // Keep capture as the layout recycles the page where the gesture began.
        const auto capture = PagesRepeater();
        if (!capture.CapturePointer(pointer)) { return; }
        m_pageTextDrag = PageTextDrag{ capture, directContact ? xaml::UIElement(layer) : nullptr,
            pointer, press, pressed, urpdf::PendingTextPosition{ pageIndex, *pdfPoint }, byWord };
        args.Handled(true);

        if (directContact)
        {
            // WinUI can register the contact after PointerPressed returns.
            // Restore system gestures when this selection ends.
            layer.ManipulationMode(xaml::Input::ManipulationModes::None);
            layer.CancelDirectManipulations();
        }
        PdfScrollViewer().Focus(xaml::FocusState::Pointer);
        if (!OwnsPageTextPointer(capture, args)) { return; }

        if (byWord)
        {
            const urpdf::PendingTextPosition wordStart{ pageIndex, *pdfPoint };
            document->m_wordSelectionPoints = urpdf::WordSelectionPoints{ wordStart, wordStart };
            UpdateWordTextSelection(document);
            return;
        }

        const bool ready = slot.State() == urpdf::PageTextState::ready;
        const urpdf::TextPosition caret{ pageIndex, ready ? urpdf::HitTest(slot.Text(), *pdfPoint) : 0 };
        if (!ready && slot.State() != urpdf::PageTextState::failed)
        {
            document->m_pendingTextAnchor = urpdf::PendingTextPosition{ pageIndex, *pdfPoint };
            document->m_pendingTextFocus = document->m_pendingTextAnchor;
        }
        SetPageTextSelection(document, urpdf::TextSelection{ caret, caret });
        if (document->m_pendingTextAnchor) { EnsurePageText(document, pageIndex, true); }
    }

    bool MainWindow::OwnsPageTextPointer(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args) const
    {
        return m_pageTextDrag && m_pageTextDrag->capture == sender
            && m_pageTextDrag->pointer.PointerId() == args.Pointer().PointerId();
    }

    void MainWindow::TextSelectionLayer_PointerMoved(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isClosed) { return; }
        const auto layer = sender.as<winrt::urPDF::TextSelectionLayer>();
        const auto found = m_pageOverlays.find(GetPageElementForTextSelectionLayer(layer));
        UpdatePageTextCursor(layer, found != m_pageOverlays.end() ? found->second.get() : nullptr, args);
    }

    void MainWindow::PageText_PointerMoved(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isClosed || !OwnsPageTextPointer(sender, args)) { return; }
        UpdatePageTextDrag(args.GetCurrentPoint(PdfScrollViewer()).Position());
        args.Handled(true);
    }

    void MainWindow::PageText_PointerReleased(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!OwnsPageTextPointer(sender, args)) { return; }
        const auto point = args.GetCurrentPoint(PdfScrollViewer());
        if (args.Pointer().PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Mouse
            && point.Properties().IsLeftButtonPressed()) { return; }
        UpdatePageTextDrag(point.Position());
        if (m_pageTextDrag) { EndPageTextDrag(true); }
        args.Handled(true);
    }

    void MainWindow::PageText_PointerCanceled(foundation::IInspectable const& sender, xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (OwnsPageTextPointer(sender, args)) { EndPageTextDrag(); }
    }

    void MainWindow::PageText_DoubleTapped(foundation::IInspectable const&, xaml::Input::DoubleTappedRoutedEventArgs const& args)
    {
        // Mouse double-clicks are recognized on the second press, before any drag.
        if (m_isClosed || args.PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Mouse
            || (m_pageTextDrag && m_pageTextDrag->moved)) { return; }
        auto document = m_presentedTab.lock();
        if (!document) { return; }

        auto source = args.OriginalSource().try_as<xaml::DependencyObject>();
        const auto repeater = PagesRepeater();
        while (source && source != repeater)
        {
            if (source.try_as<winrt::urPDF::LinkHitArea>()) { return; }
            source = media::VisualTreeHelper::GetParent(source);
        }
        const auto target = PageTextPositionAtPointer(document, args.GetPosition(PdfScrollViewer()), true);
        if (!target) { return; }

        ClearPageImageSelection(document);
        ClearPageTextSelection(document);
        document->m_wordSelectionPoints = urpdf::WordSelectionPoints{ *target, *target };
        UpdateWordTextSelection(document);
        args.Handled(true);
    }

    std::optional<urpdf::PendingTextPosition> MainWindow::PageTextPositionAtPointer(std::shared_ptr<PdfTabInfo> const& document,
        foundation::Point viewPosition, bool requireInsidePage)
    {
        const auto repeater = PagesRepeater();
        const auto scrollViewer = PdfScrollViewer();
        const auto width = static_cast<float>(scrollViewer.ViewportWidth());
        const auto height = static_cast<float>(scrollViewer.ViewportHeight());
        if (std::isfinite(width) && width > 0.0f) { viewPosition.X = std::clamp(viewPosition.X, 0.0f, width); }
        if (std::isfinite(height) && height > 0.0f) { viewPosition.Y = std::clamp(viewPosition.Y, 0.0f, height); }
        const auto position = scrollViewer.TransformToVisual(repeater).TransformPoint(viewPosition);
        if (!std::isfinite(position.X) || !std::isfinite(position.Y)) { return std::nullopt; }
        auto const& rects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        const auto generation = document->m_renderGeneration.load(std::memory_order_acquire);
        xaml::UIElement nearestElement{ nullptr };
        PageOverlayElement const* nearestEntry = nullptr;
        std::tuple<float, float, int32_t> nearestScore;
        for (auto const& [element, entry] : m_pageOverlays)
        {
            const auto binding = entry->state.CurrentBinding();
            if (!binding || binding->generation != generation || entry->document.lock() != document
                || !entry->state.Mapping() || binding->pageIndex < 0) { continue; }
            const auto page = static_cast<std::size_t>(binding->pageIndex);
            if (page >= rects.size() || page >= document->m_pageText.size()) { continue; }
            auto const& rect = rects[page];
            if (rect.Width <= 0 || rect.Height <= 0) { continue; }
            // Prefer a containing page, then the nearest row, then the closest column.
            const float dy = (std::max)({ 0.0f, rect.Y - position.Y, position.Y - (rect.Y + rect.Height) });
            const float dx = (std::max)({ 0.0f, rect.X - position.X, position.X - (rect.X + rect.Width) });
            const auto score = std::tuple{ dy, dx, binding->pageIndex };
            if (nearestEntry && score >= nearestScore) { continue; }
            nearestScore = score;
            nearestElement = element;
            nearestEntry = entry.get();
        }
        if (!nearestEntry) { return std::nullopt; }
        const auto parts = TryGetPageElementParts(nearestElement);
        if (!parts) { return std::nullopt; }
        const auto local = repeater.TransformToVisual(parts->selection).TransformPoint(position);
        const urpdf::ElementPoint elementPoint{ local.X, local.Y };
        auto const& mapping = *nearestEntry->state.Mapping();
        if (requireInsidePage && mapping.IsElementPointInsidePage(elementPoint) != true) { return std::nullopt; }
        const auto pdfPoint = mapping.ElementToPdfPoint(elementPoint);
        if (!pdfPoint) { return std::nullopt; }
        return urpdf::PendingTextPosition{ std::get<2>(nearestScore), *pdfPoint };
    }

    void MainWindow::UpdatePageTextDrag(foundation::Point position)
    {
        auto document = m_pageTextDrag->press.document.lock();
        if (!document) { return; }
        m_pageTextDrag->position = position;
        if (!m_pageTextDrag->moved)
        {
            const auto pressed = m_pageTextDrag->press.position;
            constexpr float dragThreshold = 3.0f;
            if (std::abs(position.X - pressed.X) < dragThreshold && std::abs(position.Y - pressed.Y) < dragThreshold) { return; }
            m_pageTextDrag->moved = true;
            ClearPageImageSelection(document);
            m_pageTextScrollTimer.Start();
        }

        const auto target = PageTextPositionAtPointer(document, position);
        if (!target) { return; }
        if (m_pageTextDrag->byWord)
        {
            auto& points = document->m_wordSelectionPoints;
            if (!points || points->focus == *target) { return; }
            CancelSelectionCopy(document);
            points->focus = *target;
            UpdateWordTextSelection(document);
            return;
        }

        if (!document->m_textSelection) { return; }
        auto selection = *document->m_textSelection;
        auto const& slot = document->m_pageText[target->page];
        const bool ready = slot.State() == urpdf::PageTextState::ready;
        const auto fallback = target->page < selection.anchor.page ? 0 : urpdf::kPageEnd;
        selection.focus = { target->page, ready ? urpdf::HitTest(slot.Text(), target->point) : fallback };
        const auto pending = ready || slot.State() == urpdf::PageTextState::failed ? std::nullopt : target;
        if (selection == document->m_textSelection && pending == document->m_pendingTextFocus) { return; }

        CancelSelectionCopy(document);
        document->m_pendingTextFocus = pending;
        SetPageTextSelection(document, selection);
        if (document->m_pendingTextFocus) { EnsurePageText(document, target->page, true); }
    }

    void MainWindow::ScrollPageTextDragAtEdge()
    {
        if (m_isClosed || !m_pageTextDrag || !m_pageTextDrag->moved || m_viewStateGuard.IsPending()) { return; }
        const auto document = m_pageTextDrag->press.document.lock();
        if (!document || m_presentedTab.lock() != document) { return; }

        const auto viewer = PdfScrollViewer();
        const auto position = m_pageTextDrag->position;
        const auto offsetX = viewer.HorizontalOffset();
        const auto offsetY = viewer.VerticalOffset();
        const auto targetX = urpdf::SelectionEdgeScrollOffset(position.X, viewer.ViewportWidth(), offsetX, viewer.ScrollableWidth());
        const auto targetY = urpdf::SelectionEdgeScrollOffset(position.Y, viewer.ViewportHeight(), offsetY, viewer.ScrollableHeight());
        if (!targetX && !targetY) { return; }

        const foundation::IReference<double> scrollX = targetX && *targetX != offsetX ? foundation::IReference<double>{ *targetX } : nullptr;
        const foundation::IReference<double> scrollY = targetY && *targetY != offsetY ? foundation::IReference<double>{ *targetY } : nullptr;
        if ((scrollX || scrollY) && viewer.ChangeView(scrollX, scrollY, nullptr, true)) { return; }

        // ViewChanged tracks the caret while scrolling. At a limit a newly rendered
        // page can still move the caret after the last offset change.
        if (m_pageTextDrag) { UpdatePageTextDrag(position); }
    }

    void MainWindow::EndPageTextDrag(bool released)
    {
        auto drag = std::move(*m_pageTextDrag);
        m_pageTextDrag.reset();
        m_pageTextScrollTimer.Stop();
        if (drag.manipulationLayer)
        {
            drag.manipulationLayer.ManipulationMode(xaml::Input::ManipulationModes::System);
        }
        drag.capture.ReleasePointerCapture(drag.pointer);
        const auto document = drag.press.document.lock();
        if (!document) { return; }
        if (drag.byWord) { UpdateWordTextSelection(document); }
        else if (!drag.moved)
        {
            ClearPageTextSelection(document);
            if (released && drag.pointer.PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Mouse)
            {
                m_lastPageTextClick = drag.press;
            }
            if (released && drag.pdfPress
                && drag.pdfPress->page >= 0
                && static_cast<std::size_t>(drag.pdfPress->page) < document->m_pageText.size())
            {
                auto const& slot = document->m_pageText[drag.pdfPress->page];
                if (slot.State() == urpdf::PageTextState::ready)
                {
                    if (!urpdf::IsOverText(slot.Text(), drag.pdfPress->point))
                    {
                        if (const auto imageIndex = urpdf::HitTestImage(slot.Text().images, drag.pdfPress->point))
                        {
                            SetSelectedPageImage(document, urpdf::PageImageSelection{ drag.pdfPress->page, *imageIndex });
                        }
                        else
                        {
                            ClearPageImageSelection(document);
                        }
                    }
                    else
                    {
                        ClearPageImageSelection(document);
                    }
                }
                else if (NeedsPageText(slot))
                {
                    document->m_pendingImageClick = drag.pdfPress;
                    EnsurePageText(document, drag.pdfPress->page, true);
                }
                else
                {
                    ClearPageImageSelection(document);
                }
            }
        }
    }

    void MainWindow::TryRecordFirstVisiblePage(std::shared_ptr<PdfTabInfo> const& PdfTab)
    {
        auto& metrics = PdfTab->m_renderMetrics;
        if (!metrics.openStartedAt || metrics.firstPageAttachedMs) { return; }

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto openedTab = m_openedTabs.find(currentTab);
        if (openedTab == m_openedTabs.end() || openedTab->second != PdfTab) { return; }

        auto scrollViewer = PdfScrollViewer();
        const double zoom = scrollViewer.ZoomFactor();
        const double viewportWidth = scrollViewer.ViewportWidth();
        const double viewportHeight = scrollViewer.ViewportHeight();
        if (!std::isfinite(zoom) || zoom <= 0.0 || !std::isfinite(viewportWidth) || viewportWidth <= 0.0 || !std::isfinite(viewportHeight) || viewportHeight <= 0.0) { return; }

        auto repeaterOffset = PagesRepeater().ActualOffset();
        const double viewportLeft = scrollViewer.HorizontalOffset() / zoom - repeaterOffset.x;
        const double viewportTop = scrollViewer.VerticalOffset() / zoom - repeaterOffset.y;
        const double viewportRight = viewportLeft + viewportWidth / zoom;
        const double viewportBottom = viewportTop + viewportHeight / zoom;

        auto const& pageRects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        const std::size_t knownPages = (std::min)(pageRects.size(), PdfTab->m_renderedPages.size());
        for (std::size_t i = 0; i < knownPages; ++i)
        {
            auto const& rect = pageRects[i];
            if (rect.Y >= viewportBottom) { break; }
            if (rect.Y + rect.Height <= viewportTop || rect.X + rect.Width <= viewportLeft || rect.X >= viewportRight) { continue; }

            auto const& slot = PdfTab->m_renderedPages[i];
            if (slot.state != PageRenderState::ready || !slot.bitmap || !slot.bitmap->source || slot.generation != PdfTab->m_renderGeneration.load(std::memory_order_acquire)) { continue; }

            const auto parts = TryGetPageElementParts(PagesRepeater().TryGetElement(static_cast<int32_t>(i)));
            if (!parts || parts->image.Source() != slot.bitmap->source) { continue; }

            metrics.firstPageAttachedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *metrics.openStartedAt).count();
            metrics.firstVisiblePageIndex = static_cast<int32_t>(i);
            return;
        }
    }

    void MainWindow::PagesRepeater_ElementPrepared(foundation::IInspectable const&, controls::ItemsRepeaterElementPreparedEventArgs const& args)
    {
        ConfigurePageElement(args.Element(), static_cast<std::size_t>(args.Index()));
    }

    void MainWindow::PagesRepeater_ElementClearing(foundation::IInspectable const&, controls::ItemsRepeaterElementClearingEventArgs const& args)
    {
        ReleasePageOverlay(args.Element());
    }


    // NAVIGATION
    void MainWindow::ReadViewGeometry(std::shared_ptr<PdfTabInfo> const& pdfTab, std::vector<urpdf::PageRect>& pages, urpdf::ViewportGeometry& viewport)
    {
        auto const& layoutRects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        const std::size_t knownPages = (std::min)(layoutRects.size(), static_cast<std::size_t>((std::max)(pdfTab->m_pagecount, 0)));

        pages.clear();
        pages.reserve(knownPages);
        for (std::size_t i = 0; i < knownPages; ++i)
        {
            auto const& rect = layoutRects[i];
            pages.push_back({ rect.X, rect.Y, rect.Width, rect.Height });
        }

        auto scrollViewer = PdfScrollViewer();
        const auto repeaterOffset = PagesRepeater().ActualOffset();
        viewport = {};
        viewport.offsetX = scrollViewer.HorizontalOffset();
        viewport.offsetY = scrollViewer.VerticalOffset();
        viewport.width = scrollViewer.ViewportWidth();
        viewport.height = scrollViewer.ViewportHeight();
        viewport.zoom = scrollViewer.ZoomFactor();
        viewport.contentX = repeaterOffset.x;
        viewport.contentY = repeaterOffset.y;
        viewport.scrollableWidth = scrollViewer.ScrollableWidth();
        viewport.scrollableHeight = scrollViewer.ScrollableHeight();
    }

    std::optional<int32_t> MainWindow::VisiblePageNumber(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        std::vector<urpdf::PageRect> pages;
        urpdf::ViewportGeometry viewport;
        ReadViewGeometry(pdfTab, pages, viewport);
        return urpdf::CurrentPageNumber(pages, viewport);
    }

    std::optional<urpdf::DocumentViewState> MainWindow::CaptureViewState(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        std::vector<urpdf::PageRect> pages;
        urpdf::ViewportGeometry viewport;
        ReadViewGeometry(pdfTab, pages, viewport);

        auto state = urpdf::CaptureViewAnchor(pages, viewport);
        if (state)
        {
            state->rotation = pdfTab->m_viewState.rotation;
            state->bookView = pdfTab->m_viewState.bookView;
        }
        return state;
    }

    void MainWindow::SaveViewState(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        if (!pdfTab || m_isClosed || !m_viewStateGuard.CanSave(pdfTab->m_documentId) || m_presentedTab.lock() != pdfTab) { return; }

        const double zoom = PdfScrollViewer().ZoomFactor();
        if (std::isfinite(zoom) && zoom > 0.0)
        {
            pdfTab->m_viewState.zoom = zoom;
        }

        if (m_pendingLayoutViewSaveTab.lock() == pdfTab || !HasViewGeometry(pdfTab))
        {
            return;
        }

        if (auto state = CaptureViewState(pdfTab))
        {
            pdfTab->m_viewState.anchorPage = state->anchorPage;
            pdfTab->m_viewState.anchorX = state->anchorX;
            pdfTab->m_viewState.anchorY = state->anchorY;
            pdfTab->m_viewState.viewportX = state->viewportX;
            pdfTab->m_viewState.viewportY = state->viewportY;
            pdfTab->m_hasViewAnchor = true;
        }
    }

    bool MainWindow::HasViewGeometry(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        if (!pdfTab || m_isClosed || m_presentedTab.lock() != pdfTab) { return false; }

        auto layout = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout());
        return pdfTab->m_pagecount > 0
            && layout->PageRects().size() == static_cast<std::size_t>(pdfTab->m_pagecount)
            && m_layoutRotation == NormalizeRotation(pdfTab->m_viewState.rotation)
            && layout->ColumnCount() == (pdfTab->m_viewState.bookView ? 2 : 1)
            && PdfScrollViewer().ViewportWidth() > 0.0 && PdfScrollViewer().ViewportHeight() > 0.0;
    }

    void MainWindow::UpdatePageCounter(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        if (!pdfTab || !m_viewStateGuard.CanSave(pdfTab->m_documentId) || !HasViewGeometry(pdfTab)) { return; }

        if (auto pageNumber = VisiblePageNumber(pdfTab))
        {
            pdfTab->m_currentPageIndex = *pageNumber;
            DisplayPageNumber(*pageNumber);
        }
    }

    void MainWindow::RestoreViewState(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        auto tab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto entry = m_openedTabs.find(tab);
        if (m_isClosed || !pdfTab || entry == m_openedTabs.end() || entry->second != pdfTab) { return; }

        CancelPendingZoomRender();
        const auto operation = DeferViewStateSave(pdfTab);
        m_pendingRestoreState = pdfTab->m_viewState;
        m_pendingRestoreHasAnchor = pdfTab->m_hasViewAnchor;
        m_viewRestorePhase = ViewRestorePhase::Layout;

        ClearPageOverlays();
        m_presentedTab.reset();
        PagesRepeater().ItemsSource(nullptr);
        m_pageRepeaterItems = nullptr;
        EmptyTabDropZone().Visibility(xaml::Visibility::Collapsed);
        DisplayPageCount(pdfTab->m_pagecount);
        DisplayPageNumber(pdfTab->m_pagecount > 0 ? pdfTab->m_currentPageIndex : 0);
        FinishViewStateChange(pdfTab, operation);
    }

    urpdf::ViewStateGuard::Operation MainWindow::DeferViewStateSave(std::shared_ptr<PdfTabInfo> const& pdfTab)
    {
        CancelPendingViewStateSave();
        m_pendingLayoutViewSaveTab = pdfTab;
        return m_viewStateGuard.Begin(pdfTab ? pdfTab->m_documentId : 0);
    }

    bool MainWindow::IsCurrentViewOperation(std::shared_ptr<PdfTabInfo> const& pdfTab, urpdf::ViewStateGuard::Operation operation)
    {
        if (m_isClosed || !m_viewStateGuard.IsCurrent(operation) || operation.documentId != (pdfTab ? pdfTab->m_documentId : 0) || m_pendingLayoutViewSaveTab.lock() != pdfTab) { return false; }

        auto tab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto entry = m_openedTabs.find(tab);
        return pdfTab ? entry != m_openedTabs.end() && entry->second == pdfTab : entry == m_openedTabs.end();
    }

    void MainWindow::FinishViewStateChange(std::shared_ptr<PdfTabInfo> const& pdfTab, urpdf::ViewStateGuard::Operation operation)
    {
        if (!IsCurrentViewOperation(pdfTab, operation)) { return; }
        m_viewOperationReady = true;
        UpdateTitleBarPassthroughRegions();
        QueuePendingViewStateSave();
    }

    void MainWindow::CancelPendingViewStateSave()
    {
        m_viewStateRenderingRevoker.revoke();
        m_viewStateGuard.Cancel();
        m_pendingLayoutViewSaveTab.reset();
        m_pendingLayoutViewOffsetX.reset();
        m_pendingLayoutViewOffsetY.reset();
        m_pendingLayoutViewZoom.reset();
        m_pendingViewSnapshot.reset();
        m_pendingRestoreState.reset();
        m_pendingRestoreHasAnchor = false;
        m_pendingPageFit.reset();
        m_viewRestorePhase = ViewRestorePhase::None;
        m_viewOperationReady = false;
        m_layoutViewSaveQueued = false;
        m_isViewChanging = false;
    }

    void MainWindow::QueuePendingViewStateSave()
    {
        auto pdfTab = m_pendingLayoutViewSaveTab.lock();
        const auto operation = m_viewStateGuard.CurrentOperation();
        if (!m_viewOperationReady || m_layoutViewSaveQueued || !IsCurrentViewOperation(pdfTab, operation) || (pdfTab && !pdfTab->m_layoutLoaded)) { return; }

        m_layoutViewSaveQueued = true;
        m_viewStateRenderingRevoker = xaml::Media::CompositionTarget::Rendering(winrt::auto_revoke,
            [weakThis = get_weak(), weakTab = std::weak_ptr<PdfTabInfo>(pdfTab), operation](auto const&, auto const&)
            {
                auto window = weakThis.get();
                if (!window) { return; }
                auto pdfTab = weakTab.lock();
                if (!window->IsCurrentViewOperation(pdfTab, operation)) { return; }
                window->m_viewStateRenderingRevoker.revoke();
                const bool queued = window->DispatcherQueue().TryEnqueue(dispatching::DispatcherQueuePriority::Low,
                    [weakThis, weakTab, operation]
                    {
                        auto self = weakThis.get();
                        if (!self) { return; }
                        auto pdfTab = weakTab.lock();
                        if (!self->IsCurrentViewOperation(pdfTab, operation)) { return; }
                        self->m_layoutViewSaveQueued = false;
                        self->AdvanceViewStateChange(pdfTab, operation);
                    });
                if (!queued && window->IsCurrentViewOperation(pdfTab, operation))
                {
                    window->m_layoutViewSaveQueued = false;
                }
            });
    }

    void MainWindow::AdvanceViewStateChange(std::shared_ptr<PdfTabInfo> const& pdfTab, urpdf::ViewStateGuard::Operation operation)
    {
        if (!IsCurrentViewOperation(pdfTab, operation) || !m_viewOperationReady) { return; }
        auto scrollViewer = PdfScrollViewer();

        if (m_viewRestorePhase == ViewRestorePhase::Layout)
        {
            auto const& target = *m_pendingRestoreState;
            pdfTab->m_viewState.rotation = NormalizeRotation(target.rotation);
            pdfTab->m_viewState.bookView = target.bookView;
            RebuildPagesContainer(pdfTab, pdfTab->m_renderedPages, operation);
            if (!IsCurrentViewOperation(pdfTab, operation)) { return; }
            RestoreSlotsFromCache(pdfTab);
            DisplayPageCount(pdfTab->m_pagecount);
            if (pdfTab->m_pagecount > 0)
            {
                m_pendingRestoreState->anchorPage = std::clamp(target.anchorPage, 1, pdfTab->m_pagecount);
            }
            const double zoom = std::isfinite(target.zoom) && target.zoom > 0.0 ? target.zoom : 1.0;
            m_pendingLayoutViewZoom = static_cast<float>(std::clamp(zoom,
                static_cast<double>(scrollViewer.MinZoomFactor()), static_cast<double>(scrollViewer.MaxZoomFactor())));
            m_viewRestorePhase = ViewRestorePhase::Zoom;
            m_pendingViewSnapshot.reset();
            scrollViewer.ChangeView(nullptr, nullptr, static_cast<float>(*m_pendingLayoutViewZoom), true);
            QueuePendingViewStateSave();
            return;
        }

        if (m_isViewChanging || (pdfTab && pdfTab->m_pagecount > 0 && !HasViewGeometry(pdfTab))
            || scrollViewer.ViewportWidth() <= 0.0 || scrollViewer.ViewportHeight() <= 0.0)
        {
            m_pendingViewSnapshot.reset();
            QueuePendingViewStateSave();
            return;
        }
        if (m_pendingLayoutViewZoom && std::abs(scrollViewer.ZoomFactor() - *m_pendingLayoutViewZoom) > 0.001)
        {
            m_pendingViewSnapshot.reset();
            scrollViewer.ChangeView(nullptr, nullptr, static_cast<float>(*m_pendingLayoutViewZoom), true);
            QueuePendingViewStateSave();
            return;
        }

        urpdf::ScrollOffsets restoredOffsets;
        if (m_pendingPageFit && pdfTab && pdfTab->m_pagecount > 0)
        {
            std::vector<urpdf::PageRect> pages;
            urpdf::ViewportGeometry viewport;
            ReadViewGeometry(pdfTab, pages, viewport);
            restoredOffsets = urpdf::CalculatePageFitScrollOffsets(
                pages, viewport, m_pendingPageFit->pageNumber, m_pendingPageFit->bookView).value_or(urpdf::ScrollOffsets{});
        }
        else if (m_pendingRestoreState && m_pendingRestoreHasAnchor && pdfTab->m_pagecount > 0)
        {
            std::vector<urpdf::PageRect> pages;
            urpdf::ViewportGeometry viewport;
            ReadViewGeometry(pdfTab, pages, viewport);
            restoredOffsets = urpdf::RestoreViewAnchor(*m_pendingRestoreState, pages, viewport).value_or(urpdf::ScrollOffsets{});
        }
        if (m_viewRestorePhase == ViewRestorePhase::Offsets)
        {
            m_pendingLayoutViewOffsetX = restoredOffsets.x;
            m_pendingLayoutViewOffsetY = restoredOffsets.y;
        }

        const double targetX = m_pendingLayoutViewOffsetX ? std::clamp(*m_pendingLayoutViewOffsetX, 0.0, (std::max)(0.0, scrollViewer.ScrollableWidth())) : scrollViewer.HorizontalOffset();
        const double targetY = m_pendingLayoutViewOffsetY ? std::clamp(*m_pendingLayoutViewOffsetY, 0.0, (std::max)(0.0, scrollViewer.ScrollableHeight())) : scrollViewer.VerticalOffset();
        if (std::abs(scrollViewer.HorizontalOffset() - targetX) > 0.5 || std::abs(scrollViewer.VerticalOffset() - targetY) > 0.5)
        {
            m_pendingViewSnapshot.reset();
            scrollViewer.ChangeView(targetX, targetY, nullptr, true);
            QueuePendingViewStateSave();
            return;
        }

        const auto offset = PagesRepeater().ActualOffset();
        const std::array<double, 10> snapshot{
            scrollViewer.HorizontalOffset(), scrollViewer.VerticalOffset(), scrollViewer.ZoomFactor(),
            scrollViewer.ViewportWidth(), scrollViewer.ViewportHeight(), scrollViewer.ScrollableWidth(), scrollViewer.ScrollableHeight(),
            offset.x, offset.y, PagesRepeater().ActualHeight() };
        if (!m_pendingViewSnapshot || *m_pendingViewSnapshot != snapshot)
        {
            m_pendingViewSnapshot = snapshot;
            QueuePendingViewStateSave();
            return;
        }

        if (m_viewRestorePhase == ViewRestorePhase::Zoom)
        {
            m_pendingLayoutViewOffsetX = restoredOffsets.x;
            m_pendingLayoutViewOffsetY = restoredOffsets.y;
            m_viewRestorePhase = ViewRestorePhase::Offsets;
            m_pendingViewSnapshot.reset();
            scrollViewer.ChangeView(restoredOffsets.x, restoredOffsets.y, nullptr, true);
            QueuePendingViewStateSave();
            return;
        }

        if (!m_viewStateGuard.Complete(operation)) { return; }
        m_pendingLayoutViewSaveTab.reset();
        m_pendingLayoutViewOffsetX.reset();
        m_pendingLayoutViewOffsetY.reset();
        m_pendingLayoutViewZoom.reset();
        m_pendingViewSnapshot.reset();
        m_pendingRestoreState.reset();
        m_pendingRestoreHasAnchor = false;
        m_pendingPageFit.reset();
        m_viewRestorePhase = ViewRestorePhase::None;
        m_viewOperationReady = false;
        if (pdfTab)
        {
            if (!m_isRendering) { UpdateRenderZoom(pdfTab, scrollViewer.ZoomFactor(), false); }
            if (pdfTab->m_pagecount == 0)
            {
                pdfTab->m_currentPageIndex = 0;
                DisplayPageNumber(0);
            }
            else
            {
                UpdatePageCounter(pdfTab);
            }
            SaveViewState(pdfTab);
            if (!m_isRendering) { RenderVisiblePagesAsync(); }
        }
        UpdateTitleBarPassthroughRegions();
        if (m_dpiRefreshPending) { RequestDpiRefresh(); }
    }

    void MainWindow::ScrollToPage(int32_t page)
    {
        auto const& pageRects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        if (page < 1 || static_cast<std::size_t>(page) > pageRects.size()) { return; }

        auto scrollViewer = PdfScrollViewer();
        const double zoom = scrollViewer.ZoomFactor();
        if (!std::isfinite(zoom) || zoom <= 0.0) { return; }

        const double pageTop = (pageRects[page - 1].Y + PagesRepeater().ActualOffset().y) * zoom;
        const double targetOffset = std::clamp(pageTop, 0.0, (std::max)(0.0, scrollViewer.ScrollableHeight()));
        scrollViewer.ChangeView(nullptr, targetOffset, nullptr);
    }

    void MainWindow::PageNumberBox_ValueChanged(controls::NumberBox const&, controls::NumberBoxValueChangedEventArgs const& args)
    {
        if (m_viewStateGuard.IsPending()) return;
        if (std::isnan(args.NewValue())) return;

        int32_t newPage = static_cast<int32_t>(std::round(args.NewValue()));

        if (newPage == m_pageNumber) return;

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int totalPages = pdfInfo->m_pagecount;

            if (totalPages > 1 && newPage >= 1 && newPage <= totalPages)
            {
                ScrollToPage(newPage);
            }
        }
    }
        
    void MainWindow::NextPageButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const&)
    {
        auto button = sender.as<controls::Button>();
        button.IsEnabled(false);

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int currentPage = pdfInfo->m_currentPageIndex;
            int totalPages = pdfInfo->m_pagecount;
            int pagesPerRow = pdfInfo->m_viewState.bookView ? 2 : 1;
            int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
            int currentRow = (currentPage - 1) / pagesPerRow;

            if (totalRows > 1 && currentRow + 1 < totalRows)
            {
                ScrollToPage((currentRow + 1) * pagesPerRow + 1);
            }
        }
        button.IsEnabled(true);
    }

    void MainWindow::PreviousPageButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const&)
    {
        auto button = sender.as<controls::Button>();
        button.IsEnabled(false);

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int currentPage = pdfInfo->m_currentPageIndex;
            int totalPages = pdfInfo->m_pagecount;
            int pagesPerRow = pdfInfo->m_viewState.bookView ? 2 : 1;
            int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
            int currentRow = (currentPage - 1) / pagesPerRow;

            if (totalRows > 1 && currentRow > 0)
            {
                ScrollToPage((currentRow - 1) * pagesPerRow + 1);
            }
        }
        button.IsEnabled(true);
    }

    void MainWindow::UpdateRenderZoom(std::shared_ptr<PdfTabInfo> const& pdfTab, double zoom, bool isIntermediate)
    {
        const bool zoomChanged = pdfTab->m_viewState.zoom != zoom;
        pdfTab->m_viewState.zoom = zoom;
        RefreshRealizedPageOverlays();
        auto pendingTab = m_pendingZoomTab.lock();
        if (zoom == pdfTab->m_renderZoom)
        {
            if (pendingTab == pdfTab)
            {
                CancelPendingZoomRender();
            }
            return;
        }

        if (zoomChanged || pendingTab != pdfTab)
        {
            m_pendingZoomTab = pdfTab;
            m_zoomIsIntermediate = isIntermediate;
            m_zoomDebounceElapsed = false;
            m_zoomRenderTimer.Stop();
            m_zoomRenderTimer.Start();
        }
        else if (pendingTab == pdfTab)
        {
            m_zoomIsIntermediate = isIntermediate;
            ApplyPendingRenderZoom();
        }
    }

    void MainWindow::ApplyPendingRenderZoom()
    {
        if (!m_zoomDebounceElapsed || m_zoomIsIntermediate) { return; }

        auto pdfTab = m_pendingZoomTab.lock();
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        auto openedTab = m_openedTabs.find(currentTab);
        CancelPendingZoomRender();
        if (m_isRendering || !pdfTab || openedTab == m_openedTabs.end() || openedTab->second != pdfTab) { return; }

        pdfTab->m_renderZoom = pdfTab->m_viewState.zoom;
        RenderVisiblePagesAsync();
    }

    void MainWindow::CancelPendingZoomRender()
    {
        if (m_zoomRenderTimer)
        {
            m_zoomRenderTimer.Stop();
        }
        m_pendingZoomTab.reset();
        m_zoomIsIntermediate = false;
        m_zoomDebounceElapsed = false;
    }

    winrt::fire_and_forget MainWindow::PdfScrollViewer_ViewChanged(foundation::IInspectable const& sender, controls::ScrollViewerViewChangedEventArgs const& args)
    {
        m_lastPageTextClick.reset();
        m_isViewChanging = args.IsIntermediate();
        if (m_viewStateGuard.IsPending())
        {
            m_pendingViewSnapshot.reset();
            QueuePendingViewStateSave();
            co_return;
        }
        auto scrollViewer = sender.as<controls::ScrollViewer>();

        const double currentZoom = scrollViewer.ZoomFactor();

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            if (m_presentedTab.lock() != pdfInfo || !m_viewStateGuard.CanSave(pdfInfo->m_documentId)) { co_return; }

            TryRecordFirstVisiblePage(pdfInfo);

            if (!m_isRendering)
            {
                UpdateRenderZoom(pdfInfo, currentZoom, args.IsIntermediate());
            }

            SaveViewState(pdfInfo);
            UpdatePageCounter(pdfInfo);

            if (!m_isRendering)
            {
                RenderVisiblePagesAsync();
            }
            if (m_pageTextDrag && m_pageTextDrag->moved)
            {
                UpdatePageTextDrag(m_pageTextDrag->position);
            }
        }
        co_return;
    }


    // VIEW
    winrt::fire_and_forget MainWindow::RotateButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end() && !m_isRendering)
        {
            auto pdfInfo = m_openedTabs[currentTab];

            SaveViewState(pdfInfo);
            ClearPageTextSelection(pdfInfo);
            ClearPageImageSelection(pdfInfo);
            const auto operation = DeferViewStateSave(pdfInfo);
            pdfInfo->m_viewState.rotation += 90;
            if (pdfInfo->m_viewState.rotation >= 360)
            {
                pdfInfo->m_viewState.rotation = 0;
            }

            const std::uint64_t generation = pdfInfo->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            for (auto& slot : pdfInfo->m_renderedPages)
            {
                slot.bitmap.reset();
                slot.state = PageRenderState::unloaded;
                slot.generation = generation;
            }

            RebuildPagesContainer(pdfInfo, pdfInfo->m_renderedPages, operation);
            FinishViewStateChange(pdfInfo, operation);
            RenderVisiblePagesAsync();
        }
        co_return;
    }

    winrt::fire_and_forget MainWindow::ZoomInButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
        {
            co_return;
        }

        constexpr float maxZoom = 3.0f;
        constexpr float zoomFactor = 1.1f;
        const float currentZoom = PdfScrollViewer().ZoomFactor();
        const float targetZoom = (std::min)(currentZoom * zoomFactor, maxZoom);

        if (targetZoom <= currentZoom)
        {
            co_return;
        }

        ZoomAroundViewportCenter(PdfScrollViewer(), targetZoom);
    }

    winrt::fire_and_forget MainWindow::ZoomOutButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
        {
            co_return;
        }

        constexpr float minZoom = 0.5f;
        constexpr float zoomFactor = 1.1f;
        const float currentZoom = PdfScrollViewer().ZoomFactor();
        const float targetZoom = (std::max)(currentZoom / zoomFactor, minZoom);

        if (targetZoom >= currentZoom)
        {
            co_return;
        }

        ZoomAroundViewportCenter(PdfScrollViewer(), targetZoom);
    }

    void MainWindow::UpdateFitPageSplitButtonPresentation()
    {
        const bool fitHorizontally = m_lastPageFitMode == urpdf::PageFitMode::Horizontal;
        FitVerticallyIcon().Visibility(fitHorizontally ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
        FitHorizontallyIcon().Visibility(fitHorizontally ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
        const wchar_t* label = fitHorizontally ? L"Fit horizontally" : L"Fit vertically";
        controls::ToolTipService::SetToolTip(FitPageSplitButton(), winrt::box_value(label));
        xaml::Automation::AutomationProperties::SetName(FitPageSplitButton(), label);
    }

    void MainWindow::SyncFitPageSplitButtonVisuals()
    {
        if (m_isClosed) { return; }
        auto splitButton = FitPageSplitButton();
        if (!splitButton) { return; }

        // Work around WinUI SplitButton staying in the Disabled visual state after IsEnabled flips true.
        xaml::VisualStateManager::GoToState(splitButton, splitButton.IsEnabled() ? L"Normal" : L"Disabled", false);

        // Inset SplitButton background grids inside its 1px outer border to match Button's InnerBorderEdge sizing.
        std::vector<xaml::DependencyObject> pending;
        pending.push_back(splitButton);
        while (!pending.empty())
        {
            auto current = pending.back();
            pending.pop_back();
            if (auto grid = current.try_as<controls::Grid>())
            {
                const auto name = grid.Name();
                if (name == L"PrimaryBackgroundGrid")
                {
                    grid.Margin(xaml::Thickness{ 1.0, 1.0, 1.0, 1.0 });
                    grid.CornerRadius(xaml::CornerRadius{ 3.0, 0.0, 0.0, 3.0 });
                }
                else if (name == L"SecondaryBackgroundGrid")
                {
                    grid.Margin(xaml::Thickness{ 0.0, 1.0, 1.0, 1.0 });
                    grid.CornerRadius(xaml::CornerRadius{ 0.0, 3.0, 3.0, 0.0 });
                }
            }
            const int childCount = media::VisualTreeHelper::GetChildrenCount(current);
            for (int i = 0; i < childCount; ++i)
            {
                pending.push_back(media::VisualTreeHelper::GetChild(current, i));
            }
        }
    }

    void MainWindow::ApplyPageFit(urpdf::PageFitMode mode)
    {
        if (m_isClosed || m_isRendering) { return; }
        auto pdfTab = SelectedPdfDocument();
        if (!pdfTab || !pdfTab->m_layoutLoaded || pdfTab->m_pagecount <= 0) { return; }
        if (m_viewStateGuard.IsPending() && !m_pendingPageFit) { return; }
        if (!HasViewGeometry(pdfTab)) { return; }

        std::vector<urpdf::PageRect> pages;
        urpdf::ViewportGeometry viewport;
        ReadViewGeometry(pdfTab, pages, viewport);

        const int currentPage = std::clamp(
            urpdf::CurrentPageNumber(pages, viewport).value_or(pdfTab->m_currentPageIndex),
            1, pdfTab->m_pagecount);

        auto scrollViewer = PdfScrollViewer();
        const auto fit = urpdf::CalculatePageFitView(
            pages, currentPage, pdfTab->m_viewState.bookView, mode,
            viewport.width, viewport.height,
            static_cast<double>(scrollViewer.MinZoomFactor()),
            static_cast<double>(scrollViewer.MaxZoomFactor()));
        if (!fit) { return; }

        CancelPendingZoomRender();
        const auto operation = DeferViewStateSave(pdfTab);
        m_pendingPageFit = PendingPageFit{ currentPage, pdfTab->m_viewState.bookView };
        m_pendingLayoutViewZoom = static_cast<float>(fit->zoom);
        m_viewRestorePhase = ViewRestorePhase::Zoom;
        m_pendingViewSnapshot.reset();
        scrollViewer.ChangeView(nullptr, nullptr, static_cast<float>(*m_pendingLayoutViewZoom), true);
        FinishViewStateChange(pdfTab, operation);
    }

    void MainWindow::FitPageSplitButton_Click(controls::SplitButton const&, controls::SplitButtonClickEventArgs const&)
    {
        ApplyPageFit(m_lastPageFitMode);
    }

    void MainWindow::FitVerticallyMenuItem_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        m_lastPageFitMode = urpdf::PageFitMode::Vertical;
        UpdateFitPageSplitButtonPresentation();
        ApplyPageFit(m_lastPageFitMode);
    }

    void MainWindow::FitHorizontallyMenuItem_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        m_lastPageFitMode = urpdf::PageFitMode::Horizontal;
        UpdateFitPageSplitButtonPresentation();
        ApplyPageFit(m_lastPageFitMode);
    }

    void MainWindow::BookViewButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        std::shared_ptr<PdfTabInfo> pdfInfo;

        if (currentTab)
        {
            auto openedTab = m_openedTabs.find(currentTab);
            if (openedTab != m_openedTabs.end())
            {
                pdfInfo = openedTab->second;
            }
        }

        if (!pdfInfo)
        {
            return;
        }

        SaveViewState(pdfInfo);
        pdfInfo->m_viewState.bookView = !pdfInfo->m_viewState.bookView;
        if (!pdfInfo->m_hasViewAnchor)
        {
            pdfInfo->m_viewState.anchorPage = pdfInfo->m_currentPageIndex;
            pdfInfo->m_viewState.anchorX = 0.0;
            pdfInfo->m_viewState.anchorY = 0.0;
            pdfInfo->m_viewState.viewportX = 0.0;
            pdfInfo->m_viewState.viewportY = 0.0;
            pdfInfo->m_hasViewAnchor = true;
        }
        RestoreViewState(pdfInfo);
    }

    // PRINT
    winrt::fire_and_forget MainWindow::PrintButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
        {
            co_return;
        }
        auto pdfInfo = m_openedTabs[currentTab];

        auto printPages = std::make_shared<std::vector<imaging::WriteableBitmap>>();
        printPages->reserve(pdfInfo->m_renderedPages.size());
        for (auto const& slot : pdfInfo->m_renderedPages)
        {
            printPages->push_back(slot.bitmap && slot.state == PageRenderState::ready ? slot.bitmap->source : nullptr);
        }
        auto cancelled = m_workCancelled;
        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);

        auto printInterop = winrt::get_activation_factory<printing::PrintManager, IPrintManagerInterop>();
        printing::PrintManager printManager{ nullptr };
        winrt::check_hresult(printInterop->GetForWindow(hwnd, winrt::guid_of<decltype(printManager)>(), winrt::put_abi(printManager)));

        m_printDocument = xaml::Printing::PrintDocument();
        auto printDocumentSource = m_printDocument.DocumentSource();

        auto printSize = std::make_shared<foundation::Size>(0.0f, 0.0f);

        m_printDocument.Paginate([printPages, cancelled, printSize](auto const& sender, xaml::Printing::PaginateEventArgs const& args)
            {
                if (cancelled->load(std::memory_order_acquire)) { return; }
                auto senderDoc = sender.template as<xaml::Printing::PrintDocument>();

                auto pageDesc = args.PrintTaskOptions().GetPageDescription(1);
                printSize->Width = pageDesc.ImageableRect.Width;
                printSize->Height = pageDesc.ImageableRect.Height;

                senderDoc.SetPreviewPageCount(static_cast<int>(printPages->size()), xaml::Printing::PreviewPageCountType::Final);
            });

        m_printDocument.GetPreviewPage([printPages, cancelled, printSize](auto const& sender, xaml::Printing::GetPreviewPageEventArgs const& args)
            {
                if (cancelled->load(std::memory_order_acquire)) { return; }
                auto senderDoc = sender.template as<xaml::Printing::PrintDocument>();
                if (args.PageNumber() > 0 && static_cast<std::size_t>(args.PageNumber()) <= printPages->size())
                {
                    controls::Grid pageContainer;
                    pageContainer.Width(printSize->Width);
                    pageContainer.Height(printSize->Height);

                    controls::Image pageImage;
                    auto const& bitmap = (*printPages)[args.PageNumber() - 1];
                    if (bitmap)
                    {
                        pageImage.Source(bitmap);
                    }
                    pageImage.Stretch(media::Stretch::Uniform);
                    pageImage.HorizontalAlignment(xaml::HorizontalAlignment::Center);
                    pageImage.VerticalAlignment(xaml::VerticalAlignment::Center);

                    pageContainer.Children().Append(pageImage);

                    senderDoc.SetPreviewPage(args.PageNumber(), pageContainer);
                }
            });

        m_printDocument.AddPages([printPages, cancelled, printSize](auto const& sender, xaml::Printing::AddPagesEventArgs const&)
            {
                if (cancelled->load(std::memory_order_acquire)) { return; }
                auto senderDoc = sender.template as<xaml::Printing::PrintDocument>();
                for (std::size_t i = 0; i < printPages->size(); ++i)
                {
                    controls::Grid pageContainer;
                    pageContainer.Width(printSize->Width);
                    pageContainer.Height(printSize->Height);

                    controls::Image pageImage;
                    auto const& bitmap = (*printPages)[i];
                    if (bitmap)
                    {
                        pageImage.Source(bitmap);
                    }
                    pageImage.Stretch(media::Stretch::Uniform);
                    pageImage.HorizontalAlignment(xaml::HorizontalAlignment::Center);
                    pageImage.VerticalAlignment(xaml::VerticalAlignment::Center);

                    pageContainer.Children().Append(pageImage);

                    senderDoc.AddPage(pageContainer);
                }
                senderDoc.AddPagesComplete();
            });

        if (m_printManager && m_printTaskRequestedToken)
        {
            m_printManager.PrintTaskRequested(m_printTaskRequestedToken);
        }

        m_printManager = printManager;
        m_printTaskRequestedToken = printManager.PrintTaskRequested([printDocumentSource, fileName = pdfInfo->m_fileName](
            printing::PrintManager const&,
            printing::PrintTaskRequestedEventArgs const& args)
            {
                auto deferral = args.Request().GetDeferral();
                args.Request().CreatePrintTask(fileName, [printDocumentSource](printing::PrintTaskSourceRequestedArgs const& sourceArgs)
                    {
                        sourceArgs.SetSource(printDocumentSource);
                    });
                deferral.Complete();
            });

        pdfInfo.reset();
        try
        {
            foundation::IAsyncOperation<bool> showPrintUIOp{ nullptr };
            winrt::check_hresult(printInterop->ShowPrintUIForWindowAsync(hwnd, winrt::guid_of<decltype(showPrintUIOp)>(), winrt::put_abi(showPrintUIOp)));

            co_await showPrintUIOp;
        }
        catch (winrt::hresult_error const&)
        {
            // TODO: printing fails
        }
    }
}
