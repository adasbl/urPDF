#include "pch.h"

// Corresponding
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

// Project
#include "AppSettings.h"
#include "CommandLine.h"
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
#include <coroutine>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

// Windows SDK / WinRT
#include <shobjidl_core.h>
#include <MemoryBuffer.h>
#include <wil/resource.h>
#include <printmanagerinterop.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Graphics.Printing.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Printing.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>


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
    constexpr size_t PdfStoreBudgetBytes = 256ull * 1024ull * 1024ull;
    constexpr int PrefetchPageCount = 2;
    constexpr int SlotRetainMarginPages = 3;
    constexpr auto ZoomRenderDebounce = std::chrono::milliseconds(200);
    constexpr auto DpiRenderDebounce = std::chrono::milliseconds(200);

    void LockMuPdf(void* user, int lock)
    {
        static_cast<std::mutex*>(user)[lock].lock();
    }

    void UnlockMuPdf(void* user, int lock)
    {
        static_cast<std::mutex*>(user)[lock].unlock();
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

        scrollViewer.ChangeView(
            (std::max)(0.0, centerX * targetZoom - halfViewportWidth),
            (std::max)(0.0, centerY * targetZoom - halfViewportHeight),
            targetZoom);
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

    std::vector<fz_rect> CollectPageBounds(fz_context* ctx, fz_document* doc, int pagecount)
    {
        std::vector<fz_rect> pageBounds;
        pageBounds.reserve(pagecount);

        for (int i = 0; i < pagecount; ++i)
        {
            fz_page* page = fz_load_page(ctx, doc, i);
            pageBounds.push_back(fz_bound_page(ctx, page));
            fz_drop_page(ctx, page);
        }

        return pageBounds;
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

    fz_pixmap* RenderPagePixmap(fz_context* ctx, fz_document* doc, int pageIndex, fz_rect pageBounds, int bucket, int rotation)
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
            pix = nullptr;
        }

        return pix;
    }

    graphics::SoftwareBitmap RenderPageToSoftwareBitmap(fz_context* baseCtx, fz_document* doc, int pageIndex, fz_rect pageBounds, int renderScaleBucket, int rotation)
    {
        const double renderScale = urpdf::RenderScaleForBucket(renderScaleBucket);
        if (renderScale <= 0.0) { return nullptr; }
        fz_context* ctx = fz_clone_context(baseCtx);
        if (!ctx)
        {
            return nullptr;
        }

        graphics::SoftwareBitmap bitmap{ nullptr };
        fz_pixmap* pix = RenderPagePixmap(ctx, doc, pageIndex, pageBounds, renderScaleBucket, rotation);
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

        fz_drop_context(ctx);
        return bitmap;
    }
}


namespace winrt::urPDF::implementation
{
    // MAINWINDOW
    MainWindow::~MainWindow()
    {
        CancelPendingViewStateSave();
        DetachDpiObserver();
        CancelPendingZoomRender();
        for (auto const& tabEntry : m_openedTabs)
        {
            ++tabEntry.second->m_renderGeneration;
        }

        if (m_ctx) {
            fz_drop_context(m_ctx);
            m_ctx = nullptr;
        }
    }

    MainWindow::MainWindow()
    {
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
                self->m_isClosed = true;
                self->CancelPendingViewStateSave();
                self->DetachDpiObserver();
                self->CancelPendingZoomRender();
            }
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

        m_mupdfLocks.user = m_mupdfMutexes.data();
        m_mupdfLocks.lock = LockMuPdf;
        m_mupdfLocks.unlock = UnlockMuPdf;
        m_ctx = fz_new_context(nullptr, &m_mupdfLocks, PdfStoreBudgetBytes);
        fz_register_document_handlers(m_ctx);

        auto activationArgs = winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent().GetActivatedEventArgs();
        if (activationArgs.Kind() == winrt::Microsoft::Windows::AppLifecycle::ExtendedActivationKind::File)
        {
            auto fileArgs = activationArgs.Data().try_as<winrt::Windows::ApplicationModel::Activation::IFileActivatedEventArgs>();
            if (fileArgs && fileArgs.Files().Size() > 0)
            {
                m_pendingFile = fileArgs.Files().GetAt(0).try_as<storage::StorageFile>();
            }
        }
        else if (auto path = urpdf::DocumentPathArgument(__argc, __wargv))
        {
            m_pendingFilePath = *path;
        }
    }


    // HELPER FUNCTIONS
    auto resume_foreground(dispatching::DispatcherQueue const& dispatcher)
    {
        struct awaitable
        {
            dispatching::DispatcherQueue m_dispatcher;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> handle) const
            {
                m_dispatcher.TryEnqueue([handle]() { handle(); });
            }
            void await_resume() const noexcept {}
        };
        return awaitable{ dispatcher };
    }


    // CASHE
    PageBitmapCache::PageBitmapCache(std::size_t budgetBytes)
        : m_budgetBytes(budgetBytes)
    {
    }

    std::optional<imaging::WriteableBitmap>PageBitmapCache::TryGetBitmap(PageBitmapCacheKey const& key)
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

    void PageBitmapCache::StoreBitmap(PageBitmapCacheKey const& key, imaging::WriteableBitmap const& bitmap)
    {
        const std::size_t memoryBytes = BitmapMemoryBytes(bitmap);
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
    PdfTabInfo::PdfTabInfo(fz_context* ctx, const std::string& filePath)
    {
        m_ctx = ctx;
        m_doc = fz_open_document(m_ctx, filePath.c_str());
    }

    PdfTabInfo::~PdfTabInfo()
    {
        fz_drop_document(m_ctx, m_doc);
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

        settingsPage.ThemeChanged([this](auto const&, winrt::hstring const& themeName)
        {
            ApplyTheme(themeName);
        });

        settingsPage.BackdropChanged([this](auto const&, winrt::hstring const& transparencyValue)
        {
            ApplyBackdrop(transparencyValue);
        });

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
        auto settings = urpdf::AppSettings::Instance().Get();

        ApplyTheme(winrt::hstring{ urpdf::ToString(settings.theme) });
        ApplyBackdrop(winrt::hstring{ urpdf::ToString(settings.backdrop) });
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

        using winrt::Windows::Graphics::RectInt32;
        std::vector<RectInt32> rects;

        if (MainWindowUI().Visibility() == xaml::Visibility::Visible &&
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
        TabView_AddButtonClick(tabView, nullptr);

        if (m_pendingFile)
        {
            auto file = m_pendingFile;
            m_pendingFile = nullptr;
            OpenPdfFileAsync(file);
        }
        else if (!m_pendingFilePath.empty())
        {
            OpenPdfPathAsync(std::exchange(m_pendingFilePath, {}));
        }
    }

    void MainWindow::TabView_AddButtonClick(foundation::IInspectable const&, foundation::IInspectable const&)
    {
        auto tabView = MainTabView();

        controls::TabViewItem newTab;

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
            }
        }
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
            return;
        }

        const auto operation = DeferViewStateSave(nullptr);
        m_presentedTab.reset();
        PagesRepeater().ItemsSource(nullptr);
        m_pageRepeaterItems = nullptr;

        EmptyTabDropZone().Visibility(currentTab ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
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
            ++openedTab->second->m_renderGeneration;
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
        auto deferral = args.GetDeferral();
        auto storageItems = co_await args.DataView().GetStorageItemsAsync();

        EmptyTabDropHint().Visibility(xaml::Visibility::Collapsed);

        for (auto const& item : storageItems)
        {
            auto file = item.try_as<storage::StorageFile>();
            if (file && IsPdfFile(file))
            {
                deferral.Complete();
                OpenPdfFileAsync(file);
                co_return;
            }
        }

        deferral.Complete();
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
        auto file = co_await PickPdfFileAsync();
        if (!file || m_isClosed) { co_return; }

        OpenPdfFileAsync(file);
    }

    winrt::fire_and_forget MainWindow::OpenPdfInNewWindowAsync(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto strongThis = get_strong();
        winrt::hstring errorMessage;
        try
        {
            auto file = co_await PickPdfFileAsync();
            if (!file || m_isClosed) { co_return; }

            const auto executablePath = ExecutableDirectory() / L"urPDF.exe";
            std::wstring commandLine = L"\"" + executablePath.wstring() + L"\" \"" + file.Path().c_str() + L"\"";
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            winrt::check_bool(CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &processInfo));
            wil::unique_handle processHandle{ processInfo.hProcess };
            wil::unique_handle threadHandle{ processInfo.hThread };
        }
        catch (winrt::hresult_error const& error)
        {
            errorMessage = error.message();
        }

        if (!errorMessage.empty() && !m_isClosed)
        {
            controls::ContentDialog dialog;
            dialog.XamlRoot(MainWindowUI().XamlRoot());
            dialog.Title(winrt::box_value(L"Couldn't open document in a new window"));
            dialog.Content(winrt::box_value(errorMessage));
            dialog.CloseButtonText(L"OK");
            co_await dialog.ShowAsync();
        }
    }

    winrt::fire_and_forget MainWindow::OpenPdfPathAsync(winrt::hstring path)
    {
        auto strongThis = get_strong();

        storage::StorageFile file{ nullptr };
        try
        {
            file = co_await storage::StorageFile::GetFileFromPathAsync(path);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;
        }

        if (IsPdfFile(file))
        {
            OpenPdfFileAsync(file);
        }
    }

    winrt::fire_and_forget MainWindow::OpenPdfFileAsync(storage::StorageFile const& file)
    {
        auto strongThis = get_strong();
        if (!TryAcquireRenderingLock())
        {
            co_return;
        }

        std::shared_ptr<PdfTabInfo> loadingTab;
        try
        {
            const auto openStartedAt = std::chrono::steady_clock::now();
            std::string filePath = winrt::to_string(file.Path());
            winrt::hstring fileName = file.Name();

            auto PdfInfo = std::make_shared<PdfTabInfo>(m_ctx, filePath);
            loadingTab = PdfInfo;
            PdfInfo->m_renderMetrics.openStartedAt = openStartedAt;
            PdfInfo->m_documentId = m_nextDocumentId++;
            PdfInfo->m_fileName = fileName;

            controls::TabViewItem targetTab;

            auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();

            bool isCurrentTabEmpty = currentTab && !m_openedTabs.contains(currentTab);

            if (isCurrentTabEmpty)
            {
                targetTab = currentTab;
                targetTab.Content(nullptr);
            }
            else
            {
                targetTab = controls::TabViewItem();

                MainTabView().TabItems().Append(targetTab);
                MainTabView().SelectedItem(targetTab);
            }
            SaveViewState(m_presentedTab.lock());
            targetTab.Header(winrt::box_value(fileName));

            controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(controls::Symbol::Document);
            targetTab.IconSource(tabIcon);

            m_openedTabs[targetTab] = PdfInfo;
            RestoreViewState(PdfInfo);

            co_await LoadDocumentLayoutAsync(PdfInfo);
            if (m_isClosed) { co_return; }
            ReleaseRenderingLock();
            if (!m_viewStateGuard.IsPending()) { RenderVisiblePagesAsync(); }
        }
        catch (...)
        {
            if (!m_isClosed)
            {
                // A failed open must not leave protection stuck, nor cancel a newer tab's operation.
                if (loadingTab)
                {
                    loadingTab->m_layoutLoaded = true;
                    QueuePendingViewStateSave();
                }
                ReleaseRenderingLock();
            }
        }
    }

    foundation::IAsyncAction MainWindow::LoadDocumentLayoutAsync(std::shared_ptr<PdfTabInfo> PdfTab)
    {
        auto strongThis = get_strong();
        auto dispatcher = DispatcherQueue();
        const std::uint64_t generation = PdfTab->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

        fz_context* layoutCtx = fz_clone_context(m_ctx);
        if (!layoutCtx)
        {
            winrt::throw_hresult(E_OUTOFMEMORY);
        }

        co_await winrt::resume_background();

        const int pagecount = fz_count_pages(layoutCtx, PdfTab->m_doc);
        auto pageBounds = CollectPageBounds(layoutCtx, PdfTab->m_doc, pagecount);
        fz_drop_context(layoutCtx);

        co_await resume_foreground(dispatcher);

        if (m_isClosed || PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            co_return;
        }

        std::vector<PageSlot> pageSlots(pagecount);
        for (int i = 0; i < pagecount; ++i)
        {
            pageSlots[i].pdfBounds = pageBounds[i];
            pageSlots[i].generation = generation;
        }

        PdfTab->m_pagecount = pagecount;
        PdfTab->m_pageBounds = std::move(pageBounds);
        PdfTab->m_renderedPages = std::move(pageSlots);
        PdfTab->m_layoutLoaded = true;

        // Only load the model here. The current restoration operation owns all UI work,
        // including when loading finishes after A -> B -> A.
        QueuePendingViewStateSave();
    }


    // PAGE RENDERING
    foundation::IAsyncAction MainWindow::RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
    {
        const double pageZoom = PdfTab->m_renderZoom;
        const int pageRotation = PdfTab->m_viewState.rotation;
        const int layoutBucket = GetPageRenderScaleBucket(PdfTab, 0);
        const int requestedBucket = layoutBucket > 0 ? layoutBucket : urpdf::RenderScaleBucket(PdfRenderScale(pageZoom, GetRasterizationScale()));
        if (requestedBucket == 0) { co_return; }
        const std::uint64_t generation = PdfTab->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        fz_context* renderCtx = fz_clone_context(m_ctx);
        if (!renderCtx)
        {
            co_return;
        }

        co_await winrt::resume_background();

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            fz_drop_context(renderCtx);
            co_return;
        }

        int pagecount = fz_count_pages(renderCtx, PdfTab->m_doc);
        auto pageBounds = CollectPageBounds(renderCtx, PdfTab->m_doc, pagecount);

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            fz_drop_context(renderCtx);
            co_return;
        }

        fz_drop_context(renderCtx);
        if (pageBounds.empty()) { co_return; }
        const fz_rect pdfBounds = pageBounds[0];
        const int renderScaleBucket = LimitedPageRenderScaleBucket(pdfBounds, pageRotation, urpdf::RenderScaleForBucket(requestedBucket));
        if (renderScaleBucket == 0) { co_return; }
        graphics::SoftwareBitmap bitmap = RenderPageToSoftwareBitmap(m_ctx, PdfTab->m_doc, 0, pdfBounds, renderScaleBucket, pageRotation);
        if (!bitmap) { co_return; }
        const int width = bitmap.PixelWidth();
        const int height = bitmap.PixelHeight();

        co_await resume_foreground(DispatcherQueue());

        const auto selectedTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        const auto selectedEntry = m_openedTabs.find(selectedTab);
        const int currentLayoutBucket = GetPageRenderScaleBucket(PdfTab, 0);
        const int currentBucket = currentLayoutBucket > 0 ? currentLayoutBucket: LimitedPageRenderScaleBucket(pdfBounds, PdfTab->m_viewState.rotation, PdfRenderScale(PdfTab->m_renderZoom, GetRasterizationScale()));
        if (m_isClosed || PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation || selectedEntry == m_openedTabs.end() || selectedEntry->second != PdfTab || NormalizeRotation(PdfTab->m_viewState.rotation) != NormalizeRotation(pageRotation)|| currentBucket != renderScaleBucket)
        {
            co_return;
        }

        imaging::WriteableBitmap current_writeable_bitmap(width, height);
        bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());
        current_writeable_bitmap.Invalidate();

        m_pageBitmapCache.StoreBitmap(PageBitmapCacheKey{ PdfTab->m_documentId, 0, NormalizeRotation(pageRotation), renderScaleBucket }, current_writeable_bitmap);

        PageSlot singlePageSlot;
        singlePageSlot.pdfBounds = pdfBounds;
        singlePageSlot.state = PageRenderState::ready;
        singlePageSlot.bitmap = current_writeable_bitmap;
        singlePageSlot.generation = generation;
        singlePageSlot.renderScaleBucket = renderScaleBucket;

        std::vector<PageSlot> singlePage{ std::move(singlePageSlot) };
        PdfTab->m_pagecount = pagecount;
        PdfTab->m_pageBounds = std::move(pageBounds);
        PdfTab->m_renderedPages = singlePage;
        PdfTab->m_layoutLoaded = true;
        SaveViewState(PdfTab);
        const auto operation = DeferViewStateSave(PdfTab);
        RebuildPagesContainer(PdfTab, singlePage, operation);
        FinishViewStateChange(PdfTab, operation);

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (currentTab)
        {
            currentTab.Tag(current_writeable_bitmap);
            currentTab.Header(winrt::box_value(PdfTab->m_fileName));
            DisplayPageCount(PdfTab->m_pagecount);
            DisplayPageNumber(PdfTab->m_currentPageIndex);
        }
    }

    foundation::IAsyncAction MainWindow::RenderMultiPageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
    {
        const std::uint64_t generation = PdfTab->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::vector<PageSlot> renderedPages;

        fz_context* renderCtx = fz_clone_context(m_ctx);
        if (!renderCtx)
        {
            co_return;
        }

        co_await winrt::resume_background();

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            fz_drop_context(renderCtx);
            co_return;
        }

        int pagecount = fz_count_pages(renderCtx, PdfTab->m_doc);
        auto pageBounds = CollectPageBounds(renderCtx, PdfTab->m_doc, pagecount);
        renderedPages.resize(pagecount);

        for (int i = 0; i < pagecount; ++i)
        {
            renderedPages[i].pdfBounds = pageBounds[i];
            renderedPages[i].generation = generation;
        }

        co_await resume_foreground(DispatcherQueue());

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            fz_drop_context(renderCtx);
            co_return;
        }

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        bool isCurrentTab = false;
        if (currentTab)
        {
            auto openedTab = m_openedTabs.find(currentTab);
            isCurrentTab = openedTab != m_openedTabs.end() && openedTab->second == PdfTab;
        }

        const bool keepLayout = isCurrentTab
            && m_pageRepeaterItems
            && m_pageRepeaterItems.Size() == static_cast<uint32_t>(pagecount)
            && PdfTab->m_renderedPages.size() == static_cast<std::size_t>(pagecount)
            && m_layoutRotation == NormalizeRotation(PdfTab->m_viewState.rotation);

        PdfTab->m_pagecount = pagecount;
        PdfTab->m_pageBounds = pageBounds;
        PdfTab->m_layoutLoaded = true;
        if (keepLayout)
        {
            for (auto& slot : PdfTab->m_renderedPages)
            {
                slot.generation = generation;
            }
        }
        else
        {
            PdfTab->m_renderedPages = renderedPages;
        }

        if (isCurrentTab)
        {
            if (!keepLayout)
            {
                SaveViewState(PdfTab);
                const auto operation = DeferViewStateSave(PdfTab);
                RebuildPagesContainer(PdfTab, PdfTab->m_renderedPages, operation);
                FinishViewStateChange(PdfTab, operation);
            }
            currentTab.Header(winrt::box_value(PdfTab->m_fileName));
            DisplayPageCount(PdfTab->m_pagecount);
            DisplayPageNumber(PdfTab->m_currentPageIndex);
        }

        const double pageZoom = PdfTab->m_renderZoom;
        const int pageRotation = PdfTab->m_viewState.rotation;
        std::vector<int> renderScaleBuckets(pagecount);
        for (int i = 0; i < pagecount; ++i)
        {
            const int bucket = GetPageRenderScaleBucket(PdfTab, i);
            renderScaleBuckets[i] = bucket > 0 ? bucket : LimitedPageRenderScaleBucket(pageBounds[i], pageRotation, PdfRenderScale(pageZoom, GetRasterizationScale()));
        }

        co_await winrt::resume_background();

        for (int i = 0; i < pagecount; i++)
        {
            const int renderScaleBucket = renderScaleBuckets[i];
            if (renderScaleBucket == 0) { continue; }
            if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
            {
                fz_drop_context(renderCtx);
                co_return;
            }

            renderedPages[i].state = PageRenderState::loading;
            graphics::SoftwareBitmap bitmap = RenderPageToSoftwareBitmap(m_ctx, PdfTab->m_doc, i, renderedPages[i].pdfBounds, renderScaleBucket, pageRotation);
            if (!bitmap) { continue; }
            const int width = bitmap.PixelWidth();
            const int height = bitmap.PixelHeight();
                
            co_await resume_foreground(DispatcherQueue());

            if (m_isClosed || PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation || NormalizeRotation(PdfTab->m_viewState.rotation) != NormalizeRotation(pageRotation) || GetPageRenderScaleBucket(PdfTab, i) != renderScaleBucket)
            {
                fz_drop_context(renderCtx);
                co_return;
            }

            imaging::WriteableBitmap current_writeable_bitmap(width, height);
            bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());
            current_writeable_bitmap.Invalidate();

            m_pageBitmapCache.StoreBitmap(PageBitmapCacheKey{ PdfTab->m_documentId, i, NormalizeRotation(pageRotation), renderScaleBucket }, current_writeable_bitmap);

            renderedPages[i].bitmap = std::move(current_writeable_bitmap);
            renderedPages[i].state = PageRenderState::ready;
            renderedPages[i].renderScaleBucket = renderScaleBucket;
            PdfTab->m_renderedPages[i] = renderedPages[i];

            auto selectedTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
            if (selectedTab)
            {
                auto openedTab = m_openedTabs.find(selectedTab);
                if (openedTab != m_openedTabs.end() && openedTab->second == PdfTab)
                {
                    auto element = PagesRepeater().TryGetElement(i);
                    if (element)
                    {
                        ConfigurePageElement(element, static_cast<std::size_t>(i));
                    }
                }
            }

            if (i < pagecount - 1)
            {
                co_await winrt::resume_background();
            }
        }
        fz_drop_context(renderCtx);

        co_await resume_foreground(DispatcherQueue());

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            co_return;
        }

        auto finalTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (finalTab)
        {
            auto openedTab = m_openedTabs.find(finalTab);
            if (openedTab != m_openedTabs.end() && openedTab->second == PdfTab)
            {
                finalTab.Header(winrt::box_value(PdfTab->m_fileName));
                DisplayPageCount(PdfTab->m_pagecount);
                DisplayPageNumber(PdfTab->m_currentPageIndex);
            }
        }
    }

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

    bool MainWindow::ApplyPageBitmap(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t pageIndex, imaging::WriteableBitmap const& bitmap, int renderScaleBucket)
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

    winrt::fire_and_forget MainWindow::RenderVisiblePagesAsync()
    {
        if (m_isClosed) { co_return; }
        if (m_isVisibleRenderRunning)
        {
            m_visibleRenderRequested = true;
            co_return;
        }

        auto strongThis = get_strong();
        auto dispatcher = DispatcherQueue();
        m_isVisibleRenderRunning = true;

        std::shared_ptr<PdfTabInfo> inFlightTab;
        int32_t inFlightPage = -1;
        std::optional<PageRenderMetrics> inFlightMetrics;
        auto finishRender = [&](PageRenderOutcome status)
        {
            if (inFlightTab && inFlightMetrics)
            {
                inFlightMetrics->status = status;
                if (inFlightPage >= 0 && static_cast<std::size_t>(inFlightPage) < inFlightTab->m_renderedPages.size())
                {
                    auto& slot = inFlightTab->m_renderedPages[inFlightPage];
                    if (slot.IsRequestInFlight(inFlightMetrics->generation, inFlightMetrics->renderScaleBucket, inFlightMetrics->rotation))
                    {
                        slot.requestInFlight = false;
                    }
                }
                RecordPageRenderMetrics(inFlightTab, *inFlightMetrics);
            }
            inFlightMetrics.reset();
            inFlightTab = nullptr;
            inFlightPage = -1;
        };

        try
        {
            do
            {
                if (m_isClosed) { break; }
                m_visibleRenderRequested = false;

                auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
                if (!currentTab)
                {
                    break;
                }

                auto openedTab = m_openedTabs.find(currentTab);
                if (openedTab == m_openedTabs.end())
                {
                    break;
                }

                auto pdfTab = openedTab->second;
                const std::uint64_t generation = pdfTab->m_renderGeneration.load(std::memory_order_acquire);
                const double pageZoom = pdfTab->m_renderZoom;
                const int pageRotation = NormalizeRotation(pdfTab->m_viewState.rotation);
                PageViewRange viewRange;
                const std::vector<PageRenderRequest> renderQueue = BuildRenderQueue(pdfTab->m_pagecount, viewRange);

                pdfTab->m_retainedPageFirst = viewRange.keepFirst;
                pdfTab->m_retainedPageLast = viewRange.keepLast;
                ReleaseSlotsOutsideRange(pdfTab, viewRange.keepFirst, viewRange.keepLast);
                PageBitmapCacheViewContext viewContext{ pdfTab->m_documentId, viewRange.keepFirst, viewRange.keepLast, pageRotation, {} };
                for (int32_t i = viewRange.keepFirst; i <= viewRange.keepLast; ++i)
                {
                    viewContext.renderScaleBuckets.push_back(GetPageRenderScaleBucket(pdfTab, i));
                }
                m_pageBitmapCache.SetViewContext(viewContext);
                m_pageBitmapCache.TrimToBudget();

                for (const PageRenderRequest& request : renderQueue)
                {
                    const int32_t pageIndex = request.pageIndex;
                    if (m_visibleRenderRequested || pdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation || static_cast<std::size_t>(pageIndex) >= pdfTab->m_renderedPages.size())
                    {
                        break;
                    }

                    const int renderScaleBucket = GetPageRenderScaleBucket(pdfTab, pageIndex);
                    if (renderScaleBucket == 0) { continue; }
                    if (renderScaleBucket != viewContext.renderScaleBuckets.at(pageIndex - viewRange.keepFirst))
                    {
                        m_visibleRenderRequested = true;
                        break;
                    }
                    const double renderScale = urpdf::RenderScaleForBucket(renderScaleBucket);
                    auto& slot = pdfTab->m_renderedPages[pageIndex];
                    const bool isUpToDate = slot.generation == generation && slot.renderScaleBucket == renderScaleBucket
                        && ((slot.state == PageRenderState::ready && slot.bitmap) || slot.state == PageRenderState::error);
                    if (isUpToDate)
                    {
                        if (slot.state == PageRenderState::ready && slot.bitmap)
                        {
                            ++pdfTab->m_slotBitmapReuses;
                        }
                        continue;
                    }

                    const PageBitmapCacheKey cacheKey{ pdfTab->m_documentId, pageIndex, pageRotation, renderScaleBucket };
                    if (auto cachedBitmap = m_pageBitmapCache.TryGetBitmap(cacheKey))
                    {
                        ApplyPageBitmap(pdfTab, pageIndex, *cachedBitmap, renderScaleBucket);
                        continue;
                    }

                    if (slot.IsRequestInFlight(generation, renderScaleBucket, pageRotation))
                    {
                        continue;
                    }

                    if (!slot.bitmap)
                    {
                        slot.state = PageRenderState::loading;
                    }
                    slot.requestInFlight = true;
                    slot.requestGeneration = generation;
                    slot.requestRenderScaleBucket = renderScaleBucket;
                    slot.requestRotation = pageRotation;
                    inFlightTab = pdfTab;
                    inFlightPage = pageIndex;
                    inFlightMetrics.emplace();
                    auto& renderMetrics = *inFlightMetrics;
                    renderMetrics.documentId = pdfTab->m_documentId;
                    renderMetrics.pageNumber = pageIndex + 1;
                    renderMetrics.generation = generation;
                    renderMetrics.zoom = pageZoom;
                    renderMetrics.renderScale = renderScale;
                    renderMetrics.renderScaleBucket = renderScaleBucket;
                    renderMetrics.rotation = pageRotation;
                    const fz_rect pageBounds = slot.pdfBounds;

                    co_await winrt::resume_background();
                    const auto renderStart = std::chrono::steady_clock::now();
                    graphics::SoftwareBitmap pageBitmap{ nullptr };
                    std::exception_ptr renderError;
                    try
                    {
                        pageBitmap = RenderPageToSoftwareBitmap(m_ctx, pdfTab->m_doc, pageIndex, pageBounds, renderScaleBucket, pageRotation);
                        renderMetrics.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
                        renderMetrics.bitmapCreated = static_cast<bool>(pageBitmap);
                        if (pageBitmap)
                        {
                            renderMetrics.pixelWidth = pageBitmap.PixelWidth();
                            renderMetrics.pixelHeight = pageBitmap.PixelHeight();
                            renderMetrics.pixelCount = static_cast<std::uint64_t>(renderMetrics.pixelWidth)
                                * static_cast<std::uint64_t>(renderMetrics.pixelHeight);
                            renderMetrics.estimatedBufferBytes = renderMetrics.pixelCount * 4ull;
                        }
                    }
                    catch (...)
                    {
                        renderMetrics.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
                        renderError = std::current_exception();
                    }
                    co_await resume_foreground(dispatcher);

                    if (renderError)
                    {
                        std::rethrow_exception(renderError);
                    }

                    if (m_isClosed || pdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation || static_cast<std::size_t>(pageIndex) >= pdfTab->m_renderedPages.size())
                    {
                        finishRender(PageRenderOutcome::discarded);
                        break;
                    }

                    std::optional<imaging::WriteableBitmap> writeableBitmap;
                    if (pageBitmap)
                    {
                        writeableBitmap.emplace(pageBitmap.PixelWidth(), pageBitmap.PixelHeight());
                        pageBitmap.CopyToBuffer(writeableBitmap->PixelBuffer());
                        writeableBitmap->Invalidate();

                        m_pageBitmapCache.StoreBitmap(cacheKey, *writeableBitmap);
                    }

                    if (NormalizeRotation(pdfTab->m_viewState.rotation) != pageRotation
                        || GetPageRenderScaleBucket(pdfTab, pageIndex) != renderScaleBucket)
                    {
                        m_visibleRenderRequested = true;
                        finishRender(PageRenderOutcome::discarded);
                        break;
                    }

                    if (!writeableBitmap)
                    {
                        auto& failedSlot = pdfTab->m_renderedPages[pageIndex];
                        if (!failedSlot.bitmap)
                        {
                            failedSlot.generation = generation;
                            failedSlot.renderScaleBucket = renderScaleBucket;
                            failedSlot.state = PageRenderState::error;
                        }
                        finishRender(PageRenderOutcome::error);
                        continue;
                    }

                    const bool applied = ApplyPageBitmap(pdfTab, pageIndex, *writeableBitmap, renderScaleBucket);
                    finishRender(applied ? PageRenderOutcome::applied : PageRenderOutcome::discarded);
                }
            } while (m_visibleRenderRequested);
        }
        catch (...)
        {
            finishRender(PageRenderOutcome::error);
        }

        m_isVisibleRenderRunning = false;
    }

    bool MainWindow::TryAcquireRenderingLock()
    {
        if (m_isRendering)
        {
            return false;
        }

        SaveViewState(m_presentedTab.lock());
        CancelPendingZoomRender();
        m_isRendering = true;
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
        ZoomInButton().IsEnabled(true);
        ZoomOutButton().IsEnabled(true);
        RotateButton().IsEnabled(true);
        if (m_dpiRefreshPending) { RequestDpiRefresh(); }
    }

    void MainWindow::RebuildPagesContainer(std::shared_ptr<PdfTabInfo> const& pdfTab, std::vector<PageSlot> const& pages, urpdf::ViewStateGuard::Operation operation)
    {
        if (!IsCurrentViewOperation(pdfTab, operation)) { return; }
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

    void MainWindow::ConfigurePageElement(xaml::UIElement const& element, std::size_t pageIndex)
    {
        auto border = element.try_as<controls::Border>();
        if (!border)
        {
            return;
        }

        auto image = border.Child().try_as<controls::Image>();
        if (!image)
        {
            return;
        }

        auto clearImage = [&]()
        {
            image.Source(nullptr);
            border.Opacity(0.65);
        };

        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        if (!currentTab)
        {
            clearImage();
            return;
        }

        auto openedTab = m_openedTabs.find(currentTab);
        if (openedTab == m_openedTabs.end() || pageIndex >= openedTab->second->m_renderedPages.size())
        {
            clearImage();
            return;
        }

        auto const& page = openedTab->second->m_renderedPages[pageIndex];
        if (!page.bitmap || page.state != PageRenderState::ready)
        {
            clearImage();
            return;
        }

        if (image.Source() != *page.bitmap)
        {
            controls::Image freshImage;
            freshImage.Stretch(media::Stretch::Uniform);
            freshImage.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
            freshImage.VerticalAlignment(xaml::VerticalAlignment::Stretch);
            freshImage.Source(*page.bitmap);
            image.Source(nullptr);
            border.Child(freshImage);
        }

        border.Opacity(1.0);
        TryRecordFirstVisiblePage(openedTab->second);
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
            if (slot.state != PageRenderState::ready || !slot.bitmap || !*slot.bitmap || slot.generation != PdfTab->m_renderGeneration.load(std::memory_order_acquire)) { continue; }

            auto element = PagesRepeater().TryGetElement(static_cast<int32_t>(i));
            auto border = element ? element.try_as<controls::Border>() : nullptr;
            auto image = border ? border.Child().try_as<controls::Image>() : nullptr;

            if (!image || image.Source() != *slot.bitmap) { continue; }

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
        auto border = args.Element().try_as<controls::Border>();
        if (!border)
        {
            return;
        }

        auto image = border.Child().try_as<controls::Image>();
        if (image)
        {
            image.Source(nullptr);
        }

        border.Opacity(0.65);
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
        // Geometry readiness is also needed while protected, to decide when protection may end.
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
            [weakThis = get_weak(), pdfTab, operation](auto const&, auto const&)
            {
                auto window = weakThis.get();
                if (!window || !window->IsCurrentViewOperation(pdfTab, operation)) { return; }
                window->m_viewStateRenderingRevoker.revoke();
                const bool queued = window->DispatcherQueue().TryEnqueue(dispatching::DispatcherQueuePriority::Low,
                    [weakThis, pdfTab, operation]
                    {
                        auto self = weakThis.get();

                        if (!self || !self->IsCurrentViewOperation(pdfTab, operation)) { return; }
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
        if (m_pendingRestoreState && m_pendingRestoreHasAnchor && pdfTab->m_pagecount > 0)
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

    void MainWindow::PageNumberBox_ValueChanged(controls::NumberBox const& sender, controls::NumberBoxValueChangedEventArgs const& args)
    {
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
        
    void MainWindow::NextPageButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e)
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

    void MainWindow::PreviousPageButton_Click(foundation::IInspectable const& sender, xaml::RoutedEventArgs const& e)
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

    void MainWindow::BookViewButton_Click(foundation::IInspectable const&, xaml::RoutedEventArgs const&)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<controls::TabViewItem>();
        std::shared_ptr<PdfTabInfo> pdfInfo;
        int currentPage = 1;

        if (currentTab)
        {
            auto openedTab = m_openedTabs.find(currentTab);
            if (openedTab != m_openedTabs.end())
            {
                pdfInfo = openedTab->second;
                currentPage = pdfInfo->m_currentPageIndex;
            }
        }

        if (!pdfInfo)
        {
            return;
        }

        SaveViewState(pdfInfo);
        const auto operation = DeferViewStateSave(pdfInfo);
        pdfInfo->m_viewState.bookView = !pdfInfo->m_viewState.bookView;

        RebuildPagesContainer(pdfInfo, pdfInfo->m_renderedPages, operation);

        const int pagesPerRow = pdfInfo->m_viewState.bookView ? 2 : 1;
        const int totalRows = (pdfInfo->m_pagecount + pagesPerRow - 1) / pagesPerRow;
        const int targetRow = totalRows > 0 ? (std::min)((currentPage - 1) / pagesPerRow, totalRows - 1) : 0;
        PdfScrollViewer().UpdateLayout();

        const double scrollPercentage = totalRows > 1 ? static_cast<double>(targetRow) / (totalRows - 1) : 0.0;
        const double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

        m_pendingLayoutViewOffsetY = targetOffset;
        PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr, true);
        FinishViewStateChange(pdfInfo, operation);
        RenderVisiblePagesAsync();
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

        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);

        auto printInterop = winrt::get_activation_factory<printing::PrintManager, IPrintManagerInterop>();
        printing::PrintManager printManager{ nullptr };
        winrt::check_hresult(printInterop->GetForWindow(hwnd, winrt::guid_of<decltype(printManager)>(), winrt::put_abi(printManager)));

        m_printDocument = xaml::Printing::PrintDocument();
        auto printDocumentSource = m_printDocument.DocumentSource();

        auto printSize = std::make_shared<foundation::Size>(0.0f, 0.0f);

        m_printDocument.Paginate([pdfInfo, printSize](auto const& sender, xaml::Printing::PaginateEventArgs const& args)
            {
                auto senderDoc = sender.template as<xaml::Printing::PrintDocument>();

                auto pageDesc = args.PrintTaskOptions().GetPageDescription(1);
                printSize->Width = pageDesc.ImageableRect.Width;
                printSize->Height = pageDesc.ImageableRect.Height;

                senderDoc.SetPreviewPageCount(pdfInfo->m_pagecount, xaml::Printing::PreviewPageCountType::Final);
            });

        m_printDocument.GetPreviewPage([pdfInfo, printSize](auto const& sender, xaml::Printing::GetPreviewPageEventArgs const& args)
            {
                auto senderDoc = sender.template as<xaml::Printing::PrintDocument>();
                if (args.PageNumber() > 0 && args.PageNumber() <= pdfInfo->m_pagecount)
                {
                    controls::Grid pageContainer;
                    pageContainer.Width(printSize->Width);
                    pageContainer.Height(printSize->Height);

                    controls::Image pageImage;
                    auto const& pageSlot = pdfInfo->m_renderedPages[args.PageNumber() - 1];
                    if (pageSlot.bitmap && pageSlot.state == PageRenderState::ready)
                    {
                        pageImage.Source(*pageSlot.bitmap);
                    }
                    pageImage.Stretch(media::Stretch::Uniform);
                    pageImage.HorizontalAlignment(xaml::HorizontalAlignment::Center);
                    pageImage.VerticalAlignment(xaml::VerticalAlignment::Center);

                    pageContainer.Children().Append(pageImage);

                    senderDoc.SetPreviewPage(args.PageNumber(), pageContainer);
                }
            });

        m_printDocument.AddPages([pdfInfo, printSize](auto const& sender, xaml::Printing::AddPagesEventArgs const& args)
            {
                auto senderDoc = sender.template as<xaml::Printing::PrintDocument>();
                for (int i = 0; i < pdfInfo->m_pagecount; ++i)
                {
                    controls::Grid pageContainer;
                    pageContainer.Width(printSize->Width);
                    pageContainer.Height(printSize->Height);

                    controls::Image pageImage;
                    auto const& pageSlot = pdfInfo->m_renderedPages[i];
                    if (pageSlot.bitmap && pageSlot.state == PageRenderState::ready)
                    {
                        pageImage.Source(*pageSlot.bitmap);
                    }
                    pageImage.Stretch(media::Stretch::Uniform);
                    pageImage.HorizontalAlignment(xaml::HorizontalAlignment::Center);
                    pageImage.VerticalAlignment(xaml::VerticalAlignment::Center);

                    pageContainer.Children().Append(pageImage);

                    senderDoc.AddPage(pageContainer);
                }
                senderDoc.AddPagesComplete();
            });

        if (m_printTaskRequestedToken)
        {
            printManager.PrintTaskRequested(m_printTaskRequestedToken);
        }

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

        try
        {
            foundation::IAsyncOperation<bool> showPrintUIOp{ nullptr };
            winrt::check_hresult(printInterop->ShowPrintUIForWindowAsync(hwnd, winrt::guid_of<decltype(showPrintUIOp)>(), winrt::put_abi(showPrintUIOp)));

            co_await showPrintUIOp;
        }
        catch (winrt::hresult_error const& ex)
        {
            // TODO: printing fails
        }
    }
}
