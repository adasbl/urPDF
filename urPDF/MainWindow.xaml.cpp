#include "pch.h"
#include <winrt/Windows.UI.Xaml.Interop.h>
#include "MainWindow.xaml.h"
#include "PdfRenderScale.h"
#include "SettingsPage.h"
#include "AboutPage.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

extern "C" {
#include <mupdf/fitz.h>
}

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <microsoft.ui.xaml.window.h>
#include <shobjidl_core.h>
#include <MemoryBuffer.h>
#include <map>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <algorithm>
#include <chrono>
#include <exception>
#include <cmath>
#include <cwctype>
#include <limits>
#include <string>
#include <memory>
#include <wil/resource.h>
#include <coroutine>
#include <winrt/Windows.Graphics.Printing.h>
#include <winrt/Microsoft.UI.Xaml.Printing.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <printmanagerinterop.h>
#include <winrt/Microsoft.UI.Windowing.h>

    
using namespace winrt;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
using namespace winrt::Microsoft::UI::Composition;
    
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

    constexpr double PdfRenderScale(double zoom, double dpiScale) noexcept
    {
        return urpdf::DipPerPdfPoint * zoom * dpiScale * urpdf::RenderSupersampling;
    }

    int ZoomBucket(double zoom) noexcept
    {
        if (!std::isfinite(zoom) || zoom <= 0.0)
        {
            return 0;
        }

        constexpr double bucketSize = 0.1;
        return static_cast<int>(std::lround(zoom / bucketSize));
    }

    int NormalizeRotation(int rotation) noexcept
    {
        rotation %= 360;
        return rotation < 0 ? rotation + 360 : rotation;
    }

    void ZoomAroundViewportCenter(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer const& scrollViewer, float targetZoom)
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

    std::size_t BitmapMemoryBytes(WriteableBitmap const& bitmap) noexcept
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

    fz_pixmap* RenderPagePixmap(fz_context* ctx, fz_document* doc, int pageIndex, fz_rect pageBounds, fz_matrix transform)
    {
        fz_page* page = nullptr;
        fz_device* dev = nullptr;
        fz_pixmap* pix = nullptr;

        fz_var(page);
        fz_var(dev);
        fz_var(pix);

        fz_try(ctx)
        {
            page = fz_load_page(ctx, doc, pageIndex);
            const fz_irect bbox = fz_round_rect(fz_transform_rect(pageBounds, transform));
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

    SoftwareBitmap RenderPageToSoftwareBitmap(fz_context* baseCtx, fz_document* doc, int pageIndex, fz_rect pageBounds, double renderScale, int rotation)
    {
        fz_context* ctx = fz_clone_context(baseCtx);
        if (!ctx)
        {
            return nullptr;
        }

        const fz_matrix transform = fz_concat(fz_scale(renderScale, renderScale), fz_rotate(rotation));

        SoftwareBitmap bitmap{ nullptr };
        fz_pixmap* pix = RenderPagePixmap(ctx, doc, pageIndex, pageBounds, transform);
        if (pix)
        {
            const int width = fz_pixmap_width(ctx, pix);
            const int height = fz_pixmap_height(ctx, pix);
            const std::size_t sourceStride = static_cast<std::size_t>(fz_pixmap_stride(ctx, pix));
            const unsigned char* samples = fz_pixmap_samples(ctx, pix);

            try
            {
                bitmap = SoftwareBitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
                BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                const BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
                winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();
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

        Closed([weakThis = get_weak()](auto const&, auto const&)
        {
            if (auto self = weakThis.get())
            {
                self->m_isClosed = true;
                self->DetachDpiObserver();
                self->CancelPendingZoomRender();
            }
        });

        AppWindow().SetIcon(L"Assets\\logo.ico");

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

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
                m_pendingFile = fileArgs.Files().GetAt(0).try_as<winrt::Windows::Storage::StorageFile>();
            }
        }
    }


    // HELPER FUNCTIONS
    auto resume_foreground(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher)
    {
        struct awaitable
        {
            winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher;
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
    std::size_t PageBitmapCacheKeyHash::operator() (PageBitmapCacheKey const& key) const noexcept
    {
        std::size_t hash = std::hash<std::uint64_t>{}(key.documentId);

        const auto combine = [&hash](std::size_t value)
        {
            hash ^= value + static_cast<std::size_t>(0x9e3779b9) + (hash << 6) + (hash >> 2);
        };

        combine(std::hash<int>{}(key.pageIndex));
        combine(std::hash<int>{}(key.rotation));
        combine(std::hash<int>{}(key.zoomBucket));
        combine(std::hash<double>{}(key.renderScale));
        return hash;
    }

    PageBitmapCache::PageBitmapCache(std::size_t budgetBytes)
        : m_budgetBytes(budgetBytes)
    {
    }

    std::optional<winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap>PageBitmapCache::TryGetBitmap(PageBitmapCacheKey const& key)
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

    void PageBitmapCache::StoreBitmap(PageBitmapCacheKey const& key, winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap const& bitmap)
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

            if (!m_hasViewContext
                || key.documentId != m_viewContext.documentId
                || key.rotation != m_viewContext.rotation
                || key.zoomBucket != m_viewContext.zoomBucket)
            {
                EvictEntry(key);
                return true;
            }

            if (key.pageIndex >= m_viewContext.firstPage && key.pageIndex <= m_viewContext.lastPage)
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
    winrt::event_token MainWindow::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
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
            m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"DisplayPageNumber" });
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
            m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"DisplayPageCount" });
        }
    }


    // SETTINGS 
    void MainWindow::SettingsButton_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        winrt::urPDF::SettingsPage settingsPage;

        settingsPage.BackRequested([this](auto const&, auto const&)
        {
            SettingsFrame().Content(nullptr);
            SettingsOverlay().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
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
        SettingsOverlay().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        MainWindowUI().IsHitTestVisible(false);
    }

    void MainWindow::CloseSettings_Click(
        winrt::Windows::Foundation::IInspectable const&,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        SettingsFrame().Content(nullptr);
        SettingsOverlay().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        MainWindowUI().IsHitTestVisible(true);
    }

    void MainWindow::LoadSettings()
    {
        auto localSettings = winrt::Windows::Storage::ApplicationData::Current().LocalSettings();

        if (localSettings.Values().HasKey(L"AppTheme"))
        {
            winrt::hstring theme = winrt::unbox_value<winrt::hstring>(localSettings.Values().Lookup(L"AppTheme"));
            ApplyTheme(theme);
        }
        else
        {
            ApplyTheme(L"Default");
        }

        if (localSettings.Values().HasKey(L"Transparency"))
        {
            winrt::hstring transparency = winrt::unbox_value<winrt::hstring>(localSettings.Values().Lookup(L"Transparency"));
            ApplyBackdrop(transparency);
        }
        else
        {
            ApplyBackdrop(L"None");
        }
    }

    void MainWindow::ApplyBackdrop(winrt::hstring const& transparencyValue)
    {
        if (transparencyValue == L"Mica")
        {
            auto mica = winrt::Microsoft::UI::Xaml::Media::MicaBackdrop();
            mica.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::Base);
            this->SystemBackdrop(mica);
        }
        else if (transparencyValue == L"MicaAlt")
        {
            winrt::Microsoft::UI::Xaml::Media::MicaBackdrop backdrop;
            backdrop.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
            SystemBackdrop(backdrop);
        }
        else if (transparencyValue == L"None")
        {
            this->SystemBackdrop(nullptr);
        }
        else
        {
            this->SystemBackdrop(nullptr);
        }
    }

    void MainWindow::ApplyTheme(winrt::hstring const& themeName)
    {
        ElementTheme theme = ElementTheme::Default;

        if (themeName == L"Light")
        {
            theme = ElementTheme::Light;
        }
        else if (themeName == L"Dark")
        {
            theme = ElementTheme::Dark;
        }
        else
        {
            theme = ElementTheme::Default;
        }

        if (auto rootElement = Content().try_as<FrameworkElement>())
        {
            rootElement.RequestedTheme(theme);
        }
    }


    // ABOUT
    void winrt::urPDF::implementation::MainWindow::AboutButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        MainWindowUI().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        RootFrame().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        RootFrame().Navigate(winrt::xaml_typename<winrt::urPDF::AboutPage>());
    }


    // TABVIEW
    void MainWindow::TabView_Loaded(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
        TabView_AddButtonClick(tabView, nullptr);

        if (m_pendingFile)
        {
            auto file = m_pendingFile;
            m_pendingFile = nullptr;
            OpenPdfFileAsync(file);
        }
    }

    void MainWindow::TabView_AddButtonClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Windows::Foundation::IInspectable const&)
    {
        auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

        winrt::Microsoft::UI::Xaml::Controls::TabViewItem newTab;

        newTab.Header(winrt::box_value(L"New document"));
        winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
        tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
        newTab.IconSource(tabIcon);

        tabView.TabItems().Append(newTab);

        tabView.SelectedItem(newTab);
    }

    void MainWindow::TabView_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args)
    {
        CancelPendingZoomRender();
        auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

        m_isRestoringTabView = true;
        auto restoreViewState = wil::scope_exit([this]
        {
            m_isRestoringTabView = false;
            if (m_dpiRefreshPending) { RequestDpiRefresh(); }
        });

        for (auto const& removedItem : args.RemovedItems())
        {
            auto removedTab = removedItem.try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
        auto currentTab = tabView.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

        if (currentTab)
        {
            PagesRepeater().ItemsSource(nullptr);
            m_pageRepeaterItems = nullptr;

            if (m_openedTabs.find(currentTab) != m_openedTabs.end())
            {
                EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

                auto pdfInfo = m_openedTabs[currentTab];

                DisplayPageNumber(pdfInfo->m_currentPageIndex);
                DisplayPageCount(pdfInfo->m_pagecount);
                RebuildPagesContainer(pdfInfo->m_renderedPages);
                PdfScrollViewer().ChangeView(nullptr, nullptr, static_cast<float>(pdfInfo->m_uiZoom), true);
                PdfScrollViewer().UpdateLayout();
                RestoreSlotsFromCache(pdfInfo);
                UpdateRenderZoom(pdfInfo, pdfInfo->m_uiZoom, false);
                RenderVisiblePagesAsync();
            }
            else
            {
                EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                PdfScrollViewer().ChangeView(nullptr, nullptr, 1.0f, true);
            }
        }
        else
        {
            EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            DisplayPageNumber(0);
            DisplayPageCount(0);
        }
    }

    void MainWindow::TabView_TabCloseRequested(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args)
    {
        auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

        auto openedTab = m_openedTabs.find(args.Tab());
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
        if (tabView.TabItems().IndexOf(args.Tab(), index))
        {
            tabView.TabItems().RemoveAt(index);
        }
    }

    void MainWindow::EmptyTab_DragOver(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
    {
        if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
        {
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            EmptyTabDropHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::None);
        }
    }

    void MainWindow::EmptyTab_DragLeave(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const&)
    {
        EmptyTabDropHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    winrt::fire_and_forget MainWindow::EmptyTab_Drop(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
    {
        auto deferral = args.GetDeferral();
        auto storageItems = co_await args.DataView().GetStorageItemsAsync();

        EmptyTabDropHint().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        for (auto const& item : storageItems)
        {
            auto file = item.try_as<winrt::Windows::Storage::StorageFile>();
            if (file)
            {
                auto fileType = file.FileType();
                std::wstring normalizedFileType(fileType.c_str());
                std::transform(normalizedFileType.begin(), normalizedFileType.end(), normalizedFileType.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });

                if (normalizedFileType == L".pdf")
                {
                    deferral.Complete();
                    OpenPdfFileAsync(file);
                    co_return;
                }
            }
        }

        deferral.Complete();
    }


    // FILE OPENING 
    winrt::fire_and_forget MainWindow::OpenPdfFileAsync(IInspectable const&, RoutedEventArgs const&)
    {
        // TODO: add check if empty tab is opened
        // TODO: check if file is already opened (+ option to turn off in settings)
        FileOpenPicker filePicker;
        filePicker.FileTypeFilter().Append(L".pdf");

        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
        filePicker.as<::IInitializeWithWindow>()->Initialize(hwnd);

        auto file = co_await filePicker.PickSingleFileAsync();
        if (!file) co_return;

        OpenPdfFileAsync(file);
    }

    winrt::fire_and_forget MainWindow::OpenPdfFileAsync(winrt::Windows::Storage::StorageFile const& file)
    {
        if (!TryAcquireRenderingLock())
        {
            co_return;
        }

        try
        {
            const auto openStartedAt = std::chrono::steady_clock::now();
            std::string filePath = winrt::to_string(file.Path());
            winrt::hstring fileName = file.Name();

            auto PdfInfo = std::make_shared<PdfTabInfo>(m_ctx, filePath);
            PdfInfo->m_renderMetrics.openStartedAt = openStartedAt;
            PdfInfo->m_documentId = m_nextDocumentId++;
            PdfInfo->m_fileName = fileName;

            winrt::Microsoft::UI::Xaml::Controls::TabViewItem targetTab;

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            bool isCurrentTabEmpty = currentTab && !m_openedTabs.contains(currentTab);

            if (isCurrentTabEmpty)
            {
                targetTab = currentTab;
                targetTab.Content(nullptr);
            }
            else
            {
                targetTab = winrt::Microsoft::UI::Xaml::Controls::TabViewItem();

                MainTabView().TabItems().Append(targetTab);
                MainTabView().SelectedItem(targetTab);
            }
            PagesRepeater().ItemsSource(nullptr);
            m_pageRepeaterItems = nullptr;
            EmptyTabDropZone().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            targetTab.Header(winrt::box_value(fileName));

            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            targetTab.IconSource(tabIcon);

            m_openedTabs[targetTab] = PdfInfo;

            co_await LoadDocumentLayoutAsync(PdfInfo);
            ReleaseRenderingLock();
            RenderVisiblePagesAsync();
        }
        catch (...)
        {
            ReleaseRenderingLock();
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::LoadDocumentLayoutAsync(std::shared_ptr<PdfTabInfo> PdfTab)
    {
        auto strongThis = get_strong();
        auto dispatcher = DispatcherQueue();
        const std::uint64_t generation = PdfTab->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

        fz_context* layoutCtx = fz_clone_context(m_ctx);
        if (!layoutCtx)
        {
            co_return;
        }

        co_await winrt::resume_background();

        const int pagecount = fz_count_pages(layoutCtx, PdfTab->m_doc);
        auto pageBounds = CollectPageBounds(layoutCtx, PdfTab->m_doc, pagecount);
        fz_drop_context(layoutCtx);

        co_await resume_foreground(dispatcher);

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
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

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (currentTab)
        {
            auto openedTab = m_openedTabs.find(currentTab);
            if (openedTab != m_openedTabs.end() && openedTab->second == PdfTab)
            {
                RebuildPagesContainer(PdfTab->m_renderedPages);
                currentTab.Header(winrt::box_value(PdfTab->m_fileName));
                DisplayPageCount(PdfTab->m_pagecount);
                DisplayPageNumber(PdfTab->m_currentPageIndex);
            }
        }
    }


    // PAGE RENDERING
    winrt::Windows::Foundation::IAsyncAction MainWindow::RenderSinglePageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
    {
        const double pageZoom = PdfTab->m_renderZoom;
        const int pageRotation = PdfTab->m_currentRotation;
        const double layoutRenderScale = GetPageRenderScale(PdfTab, 0);
        const double renderScale = layoutRenderScale > 0.0 ? layoutRenderScale : PdfRenderScale(pageZoom, GetRasterizationScale());
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

        fz_page* page = fz_load_page(renderCtx, PdfTab->m_doc, 0);

        fz_matrix transform = fz_scale(renderScale, renderScale);
        transform = fz_concat(transform, fz_rotate(pageRotation));

        fz_rect pdfBounds = pageBounds[0];
        fz_rect rect = pdfBounds;
        rect = fz_transform_rect(rect, transform);
        fz_irect bbox = fz_round_rect(rect);

        fz_pixmap* pix = fz_new_pixmap_with_bbox(renderCtx, fz_device_bgr(renderCtx), bbox, NULL, 1);
        fz_clear_pixmap_with_value(renderCtx, pix, 255);

        fz_device* dev = fz_new_draw_device(renderCtx, fz_identity, pix);
        fz_run_page(renderCtx, page, dev, transform, NULL);
        fz_close_device(renderCtx, dev);

        int width = fz_pixmap_width(renderCtx, pix);
        int height = fz_pixmap_height(renderCtx, pix);

        SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
        {
            BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
            winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

            uint8_t* data = reference.data();

            memcpy(data, fz_pixmap_samples(renderCtx, pix), width * height * 4);
        }

        fz_drop_device(renderCtx, dev);
        fz_drop_pixmap(renderCtx, pix);
        fz_drop_page(renderCtx, page);
        fz_drop_context(renderCtx);

        co_await resume_foreground(DispatcherQueue());

        if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
        {
            co_return;
        }

        winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap current_writeable_bitmap(width, height);
        bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());
        current_writeable_bitmap.Invalidate();

        m_pageBitmapCache.StoreBitmap(PageBitmapCacheKey{ PdfTab->m_documentId, 0, NormalizeRotation(pageRotation), ZoomBucket(pageZoom), renderScale }, current_writeable_bitmap);

        PageSlot singlePageSlot;
        singlePageSlot.pdfBounds = pdfBounds;
        singlePageSlot.state = PageRenderState::ready;
        singlePageSlot.bitmap = current_writeable_bitmap;
        singlePageSlot.generation = generation;
        singlePageSlot.zoomBucket = ZoomBucket(pageZoom);
        singlePageSlot.renderScale = renderScale;

        std::vector<PageSlot> singlePage{
            std::move(singlePageSlot)
        };
        PdfTab->m_pagecount = pagecount;
        PdfTab->m_pageBounds = std::move(pageBounds);
        PdfTab->m_renderedPages = singlePage;
        RebuildPagesContainer(singlePage);

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (currentTab)
        {
            currentTab.Tag(current_writeable_bitmap);
            currentTab.Header(winrt::box_value(PdfTab->m_fileName));
            DisplayPageCount(PdfTab->m_pagecount);
            DisplayPageNumber(PdfTab->m_currentPageIndex);
        }
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::RenderMultiPageAsync(std::shared_ptr<PdfTabInfo> PdfTab)
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

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
            && m_layoutRotation == NormalizeRotation(PdfTab->m_currentRotation);

        PdfTab->m_pagecount = pagecount;
        PdfTab->m_pageBounds = pageBounds;
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
                RebuildPagesContainer(PdfTab->m_renderedPages);
            }
            currentTab.Header(winrt::box_value(PdfTab->m_fileName));
            DisplayPageCount(PdfTab->m_pagecount);
            DisplayPageNumber(PdfTab->m_currentPageIndex);
        }

        const double pageZoom = PdfTab->m_renderZoom;
        const int pageRotation = PdfTab->m_currentRotation;
        std::vector<double> renderScales(pagecount, PdfRenderScale(pageZoom, GetRasterizationScale()));
        for (int i = 0; i < pagecount; ++i)
        {
            const double scale = GetPageRenderScale(PdfTab, i);
            if (scale > 0.0) { renderScales[i] = scale; }
        }

        co_await winrt::resume_background();

        for (int i = 0; i < pagecount; i++)
        {
            const double renderScale = renderScales[i];
            const fz_matrix transform = fz_concat(fz_scale(renderScale, renderScale), fz_rotate(pageRotation));
            if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
            {
                fz_drop_context(renderCtx);
                co_return;
            }

            renderedPages[i].state = PageRenderState::loading;
            fz_page* page = fz_load_page(renderCtx, PdfTab->m_doc, i);

            fz_rect rect = renderedPages[i].pdfBounds;
            rect = fz_transform_rect(rect, transform);
            fz_irect bbox = fz_round_rect(rect);

            fz_pixmap* pix = fz_new_pixmap_with_bbox(renderCtx, fz_device_bgr(renderCtx), bbox, NULL, 1);
            fz_clear_pixmap_with_value(renderCtx, pix, 255);

            fz_device* dev = fz_new_draw_device(renderCtx, fz_identity, pix);
            fz_run_page(renderCtx, page, dev, transform, NULL);
            fz_close_device(renderCtx, dev);
                    
            int width = fz_pixmap_width(renderCtx, pix);
            int height = fz_pixmap_height(renderCtx, pix);

            SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
            {
                BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

                uint8_t* data = reference.data();

                memcpy(data, fz_pixmap_samples(renderCtx, pix), width * height * 4);
            }

            fz_drop_device(renderCtx, dev);
            fz_drop_pixmap(renderCtx, pix);
            fz_drop_page(renderCtx, page);
                
            co_await resume_foreground(DispatcherQueue());

            if (PdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation)
            {
                fz_drop_context(renderCtx);
                co_return;
            }

            winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap current_writeable_bitmap(width, height);
            bitmap.CopyToBuffer(current_writeable_bitmap.PixelBuffer());
            current_writeable_bitmap.Invalidate();

            m_pageBitmapCache.StoreBitmap(PageBitmapCacheKey{ PdfTab->m_documentId, i, NormalizeRotation(pageRotation), ZoomBucket(pageZoom), renderScale }, current_writeable_bitmap);

            renderedPages[i].bitmap = std::move(current_writeable_bitmap);
            renderedPages[i].state = PageRenderState::ready;
            renderedPages[i].zoomBucket = ZoomBucket(pageZoom);
            renderedPages[i].renderScale = renderScale;
            PdfTab->m_renderedPages[i] = renderedPages[i];

            auto selectedTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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

        auto finalTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
        if (auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>())
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
            slot.zoomBucket = 0;
            slot.renderScale = 0.0;

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
        const int rotation = NormalizeRotation(PdfTab->m_currentRotation);
        const int zoomBucket = ZoomBucket(PdfTab->m_renderZoom);

        for (int32_t i = first; i <= last; ++i)
        {
            const double renderScale = GetPageRenderScale(PdfTab, i);
            if (renderScale <= 0.0) { continue; }
            auto& slot = PdfTab->m_renderedPages[i];
            if (slot.bitmap)
            {
                if (slot.state == PageRenderState::ready && slot.zoomBucket == zoomBucket && slot.renderScale == renderScale)
                {
                    ++PdfTab->m_slotBitmapReuses;
                }
                continue;
            }

            if (auto cachedBitmap = m_pageBitmapCache.TryGetBitmap(PageBitmapCacheKey{ PdfTab->m_documentId, i, rotation, zoomBucket, renderScale }))
            {
                ApplyPageBitmap(PdfTab, i, *cachedBitmap, zoomBucket, renderScale);
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
        if (m_isRendering || m_isRestoringTabView) { return; }

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

    double MainWindow::GetPageRenderScale(std::shared_ptr<PdfTabInfo> const& pdfTab, int32_t pageIndex)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        auto openedTab = m_openedTabs.find(currentTab);
        if (openedTab == m_openedTabs.end() || openedTab->second != pdfTab || m_layoutRotation != NormalizeRotation(pdfTab->m_currentRotation) || pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= pdfTab->m_renderedPages.size())
        {
            return 0.0;
        }

        auto const& rects = winrt::get_self<winrt::urPDF::implementation::PdfPageLayout>(PagesLayout())->PageRects();
        if (static_cast<std::size_t>(pageIndex) >= rects.size()) { return 0.0; }
        auto const& rect = rects[pageIndex];
        auto const& bounds = pdfTab->m_renderedPages[pageIndex].pdfBounds;
        return urpdf::PageRenderScale(std::abs(static_cast<double>(bounds.x1) - bounds.x0), std::abs(static_cast<double>(bounds.y1) - bounds.y0), pdfTab->m_currentRotation, rect.Width, rect.Height, pdfTab->m_renderZoom, GetRasterizationScale());
    }

    bool MainWindow::ApplyPageBitmap(std::shared_ptr<PdfTabInfo> const& PdfTab, int32_t pageIndex, WriteableBitmap const& bitmap, int zoomBucket, double renderScale)
    {
        if (pageIndex < 0 || static_cast<std::size_t>(pageIndex) >= PdfTab->m_renderedPages.size())
        {
            return false;
        }

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
        slot.zoomBucket = zoomBucket;
        slot.renderScale = renderScale;
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
                    if (slot.IsRequestInFlight(inFlightMetrics->generation, ZoomBucket(inFlightMetrics->zoom), inFlightMetrics->rotation, inFlightMetrics->renderScale))
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

                auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
                const int pageRotation = NormalizeRotation(pdfTab->m_currentRotation);
                const int zoomBucket = ZoomBucket(pageZoom);
                PageViewRange viewRange;
                const std::vector<PageRenderRequest> renderQueue = BuildRenderQueue(pdfTab->m_pagecount, viewRange);

                pdfTab->m_retainedPageFirst = viewRange.keepFirst;
                pdfTab->m_retainedPageLast = viewRange.keepLast;
                ReleaseSlotsOutsideRange(pdfTab, viewRange.keepFirst, viewRange.keepLast);
                m_pageBitmapCache.SetViewContext(PageBitmapCacheViewContext{ pdfTab->m_documentId, viewRange.keepFirst, viewRange.keepLast, pageRotation, zoomBucket });
                m_pageBitmapCache.TrimToBudget();

                for (const PageRenderRequest& request : renderQueue)
                {
                    const int32_t pageIndex = request.pageIndex;
                    if (m_visibleRenderRequested || pdfTab->m_renderGeneration.load(std::memory_order_acquire) != generation || static_cast<std::size_t>(pageIndex) >= pdfTab->m_renderedPages.size())
                    {
                        break;
                    }

                    const double renderScale = GetPageRenderScale(pdfTab, pageIndex);
                    if (renderScale <= 0.0) { continue; }
                    auto& slot = pdfTab->m_renderedPages[pageIndex];
                    const bool isUpToDate = slot.zoomBucket == zoomBucket && slot.renderScale == renderScale && ((slot.state == PageRenderState::ready && slot.bitmap) || slot.state == PageRenderState::error);
                    if (isUpToDate)
                    {
                        if (slot.state == PageRenderState::ready && slot.bitmap)
                        {
                            ++pdfTab->m_slotBitmapReuses;
                        }
                        continue;
                    }

                    const PageBitmapCacheKey cacheKey{ pdfTab->m_documentId, pageIndex, pageRotation, zoomBucket, renderScale };
                    if (auto cachedBitmap = m_pageBitmapCache.TryGetBitmap(cacheKey))
                    {
                        ApplyPageBitmap(pdfTab, pageIndex, *cachedBitmap, zoomBucket, renderScale);
                        continue;
                    }

                    if (slot.IsRequestInFlight(generation, zoomBucket, pageRotation, renderScale))
                    {
                        continue;
                    }

                    if (!slot.bitmap)
                    {
                        slot.state = PageRenderState::loading;
                    }
                    slot.requestInFlight = true;
                    slot.requestGeneration = generation;
                    slot.requestZoomBucket = zoomBucket;
                    slot.requestRotation = pageRotation;
                    slot.requestRenderScale = renderScale;
                    inFlightTab = pdfTab;
                    inFlightPage = pageIndex;
                    inFlightMetrics.emplace();
                    auto& renderMetrics = *inFlightMetrics;
                    renderMetrics.documentId = pdfTab->m_documentId;
                    renderMetrics.pageNumber = pageIndex + 1;
                    renderMetrics.generation = generation;
                    renderMetrics.zoom = pageZoom;
                    renderMetrics.renderScale = renderScale;
                    renderMetrics.rotation = pageRotation;
                    const fz_rect pageBounds = slot.pdfBounds;

                    co_await winrt::resume_background();
                    const auto renderStart = std::chrono::steady_clock::now();
                    SoftwareBitmap pageBitmap{ nullptr };
                    std::exception_ptr renderError;
                    try
                    {
                        pageBitmap = RenderPageToSoftwareBitmap(m_ctx, pdfTab->m_doc, pageIndex, pageBounds, renderScale, pageRotation);
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

                    std::optional<WriteableBitmap> writeableBitmap;
                    if (pageBitmap)
                    {
                        writeableBitmap.emplace(pageBitmap.PixelWidth(), pageBitmap.PixelHeight());
                        pageBitmap.CopyToBuffer(writeableBitmap->PixelBuffer());
                        writeableBitmap->Invalidate();

                        m_pageBitmapCache.StoreBitmap(cacheKey, *writeableBitmap);
                    }

                    if (NormalizeRotation(pdfTab->m_currentRotation) != pageRotation || ZoomBucket(pdfTab->m_renderZoom) != zoomBucket
                        || GetPageRenderScale(pdfTab, pageIndex) != renderScale)
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
                            failedSlot.zoomBucket = zoomBucket;
                            failedSlot.renderScale = renderScale;
                            failedSlot.state = PageRenderState::error;
                        }
                        finishRender(PageRenderOutcome::error);
                        continue;
                    }

                    const bool applied = ApplyPageBitmap(pdfTab, pageIndex, *writeableBitmap, zoomBucket, renderScale);
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

        CancelPendingZoomRender();
        m_isRendering = true;
        m_zoomWasEnabled = PdfScrollViewer().ZoomMode() == winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Enabled;
        PdfScrollViewer().ZoomMode(winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Disabled);
        ZoomInButton().IsEnabled(false);
        ZoomOutButton().IsEnabled(false);
        RotateButton().IsEnabled(false);
        return true;
    }

    void MainWindow::ReleaseRenderingLock()
    {
        if (m_zoomWasEnabled)
        {
            PdfScrollViewer().ZoomMode(winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Enabled);
        }
        m_zoomWasEnabled = false;
        m_isRendering = false;
        ZoomInButton().IsEnabled(true);
        ZoomOutButton().IsEnabled(true);
        RotateButton().IsEnabled(true);
        if (m_dpiRefreshPending) { RequestDpiRefresh(); }
    }

    void MainWindow::RebuildPagesContainer(std::vector<PageSlot> const& pages)
    {
        int currentRotation = 0;
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (currentTab)
        {
            auto openedTab = m_openedTabs.find(currentTab);
            if (openedTab != m_openedTabs.end())
            {
                currentRotation = openedTab->second->m_currentRotation;
            }
        }

        auto pageSizes = winrt::single_threaded_vector<winrt::Windows::Foundation::Size>();

        const bool swapDimensions = NormalizeRotation(currentRotation) == 90 || NormalizeRotation(currentRotation) == 270;
        constexpr double dipPerPdfPoint = 96.0 / 72.0;

        for (auto const& page : pages)
        {
            const double pdfWidth = std::abs(static_cast<double>(page.pdfBounds.x1 - page.pdfBounds.x0));
            const double pdfHeight = std::abs(static_cast<double>(page.pdfBounds.y1 - page.pdfBounds.y0));
            const double width = (swapDimensions ? pdfHeight : pdfWidth) * dipPerPdfPoint;
            const double height = (swapDimensions ? pdfWidth : pdfHeight) * dipPerPdfPoint;
            pageSizes.Append(winrt::Windows::Foundation::Size{static_cast<float>((std::max)(1.0, width)), static_cast<float>((std::max)(1.0, height))});
        }

        m_layoutRotation = NormalizeRotation(currentRotation);
        PagesLayout().ColumnCount(m_bookViewMode ? 2 : 1);
        PagesLayout().PageSizes(pageSizes.GetView());

        m_pageRepeaterItems = winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            m_pageRepeaterItems.Append(winrt::box_value(static_cast<int32_t>(i)));
        }

        PagesRepeater().ItemsSource(m_pageRepeaterItems);
        PdfScrollViewer().UpdateLayout();

        for (int32_t i = 0; i < static_cast<int32_t>(pages.size()); ++i)
        {
            if (auto element = PagesRepeater().TryGetElement(i))
            {
                ConfigurePageElement(element, static_cast<std::size_t>(i));
            }
        }
    }

    void MainWindow::ConfigurePageElement(winrt::Microsoft::UI::Xaml::UIElement const& element, std::size_t pageIndex)
    {
        auto border = element.try_as<winrt::Microsoft::UI::Xaml::Controls::Border>();
        if (!border)
        {
            return;
        }

        auto image = border.Child().try_as<winrt::Microsoft::UI::Xaml::Controls::Image>();
        if (!image)
        {
            return;
        }

        auto clearImage = [&]()
        {
            image.Source(nullptr);
            border.Opacity(0.65);
        };

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
            winrt::Microsoft::UI::Xaml::Controls::Image freshImage;
            freshImage.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
            freshImage.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            freshImage.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);
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

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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
            auto border = element ? element.try_as<winrt::Microsoft::UI::Xaml::Controls::Border>() : nullptr;
            auto image = border ? border.Child().try_as<winrt::Microsoft::UI::Xaml::Controls::Image>() : nullptr;

            if (!image || image.Source() != *slot.bitmap) { continue; }

            metrics.firstPageAttachedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *metrics.openStartedAt).count();
            metrics.firstVisiblePageIndex = static_cast<int32_t>(i);
            return;
        }
    }

    void MainWindow::PagesRepeater_ElementPrepared(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementPreparedEventArgs const& args)
    {
        ConfigurePageElement(args.Element(), static_cast<std::size_t>(args.Index()));
    }

    void MainWindow::PagesRepeater_ElementClearing(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::ItemsRepeaterElementClearingEventArgs const& args)
    {
        auto border = args.Element().try_as<winrt::Microsoft::UI::Xaml::Controls::Border>();
        if (!border)
        {
            return;
        }

        auto image = border.Child().try_as<winrt::Microsoft::UI::Xaml::Controls::Image>();
        if (image)
        {
            image.Source(nullptr);
        }

        border.Opacity(0.65);
    }


    // NAVIGATION
    void MainWindow::PageNumberBox_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const& sender, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args)
    {
        if (std::isnan(args.NewValue())) return;

        int32_t newPage = static_cast<int32_t>(std::round(args.NewValue()));

        if (newPage == m_pageNumber) return;

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int totalPages = pdfInfo->m_pagecount;

            if (totalPages > 1 && newPage >= 1 && newPage <= totalPages)
            {
                int pagesPerRow = m_bookViewMode ? 2 : 1;
                int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
                int targetRow = (newPage - 1) / pagesPerRow;
                double scrollPercentage = totalRows > 1
                    ? static_cast<double>(targetRow) / (totalRows - 1)
                    : 0.0;
                double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
            }
        }
    }
        
    void MainWindow::NextPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto button = sender.as<winrt::Microsoft::UI::Xaml::Controls::Button>();
        button.IsEnabled(false);

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int currentPage = pdfInfo->m_currentPageIndex;
            int totalPages = pdfInfo->m_pagecount;
            int pagesPerRow = m_bookViewMode ? 2 : 1;
            int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
            int currentRow = (currentPage - 1) / pagesPerRow;

            if (totalRows > 1 && currentRow + 1 < totalRows)
            {
                int targetRow = currentRow + 1;

                double scrollPercentage = static_cast<double>(targetRow) / (totalRows - 1);
                double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
            }
        }
        button.IsEnabled(true);
    }

    void MainWindow::PreviousPageButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto button = sender.as<winrt::Microsoft::UI::Xaml::Controls::Button>();
        button.IsEnabled(false);

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int currentPage = pdfInfo->m_currentPageIndex;
            int totalPages = pdfInfo->m_pagecount;
            int pagesPerRow = m_bookViewMode ? 2 : 1;
            int totalRows = (totalPages + pagesPerRow - 1) / pagesPerRow;
            int currentRow = (currentPage - 1) / pagesPerRow;

            if (totalRows > 1 && currentRow > 0)
            {
                int targetRow = currentRow - 1;

                double scrollPercentage = static_cast<double>(targetRow) / (totalRows - 1);
                double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

                PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
            }
        }
        button.IsEnabled(true);
    }

    void MainWindow::UpdateRenderZoom(std::shared_ptr<PdfTabInfo> const& pdfTab, double zoom, bool isIntermediate)
    {
        const bool zoomChanged = pdfTab->m_uiZoom != zoom;
        pdfTab->m_uiZoom = zoom;
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
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        auto openedTab = m_openedTabs.find(currentTab);
        CancelPendingZoomRender();
        if (m_isRendering || !pdfTab || openedTab == m_openedTabs.end() || openedTab->second != pdfTab) { return; }

        pdfTab->m_renderZoom = pdfTab->m_uiZoom;
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

    winrt::fire_and_forget MainWindow::PdfScrollViewer_ViewChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args)
    {
        if (m_isRestoringTabView) { co_return; }
        auto scrollViewer = sender.as<winrt::Microsoft::UI::Xaml::Controls::ScrollViewer>();

        double scrollOffset = scrollViewer.VerticalOffset();
        double totalHeight = scrollViewer.ScrollableHeight() + scrollViewer.ViewportHeight();
        const double currentZoom = scrollViewer.ZoomFactor();

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end())
        {
            auto pdfInfo = m_openedTabs[currentTab];
            int totalPages = pdfInfo->m_pagecount;

            TryRecordFirstVisiblePage(pdfInfo);

            if (!m_isRendering)
            {
                UpdateRenderZoom(pdfInfo, currentZoom, args.IsIntermediate());
                RenderVisiblePagesAsync();
            }

            if (totalPages > 0 && totalHeight > 0)
            {
                double scrollPercentage = scrollOffset / scrollViewer.ScrollableHeight();

                if (scrollViewer.ScrollableHeight() == 0) scrollPercentage = 0.0;
                if (scrollPercentage > 1.0) scrollPercentage = 1.0;
                if (scrollPercentage < 0.0) scrollPercentage = 0.0;

                int32_t currentPage = static_cast<int32_t>(std::round(scrollPercentage * (totalPages - 1))) + 1;

                pdfInfo->m_currentPageIndex = currentPage;

                DisplayPageNumber(currentPage);
            }
        }
        co_return;
    }


    // VIEW
    winrt::fire_and_forget MainWindow::RotateButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (currentTab && m_openedTabs.find(currentTab) != m_openedTabs.end() && !m_isRendering)
        {
            auto pdfInfo = m_openedTabs[currentTab];

            pdfInfo->m_currentRotation += 90;
            if (pdfInfo->m_currentRotation >= 360)
            {
                pdfInfo->m_currentRotation = 0;
            }

            const std::uint64_t generation = pdfInfo->m_renderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
            for (auto& slot : pdfInfo->m_renderedPages)
            {
                slot.bitmap.reset();
                slot.state = PageRenderState::unloaded;
                slot.generation = generation;
            }

            RebuildPagesContainer(pdfInfo->m_renderedPages);
            RenderVisiblePagesAsync();
        }
        co_return;
    }

    winrt::fire_and_forget MainWindow::ZoomInButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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

    winrt::fire_and_forget MainWindow::ZoomOutButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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

    void MainWindow::BookViewButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
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

        m_bookViewMode = !m_bookViewMode;

        if (!pdfInfo)
        {
            return;
        }

        RebuildPagesContainer(pdfInfo->m_renderedPages);

        const int pagesPerRow = m_bookViewMode ? 2 : 1;
        const int totalRows = (pdfInfo->m_pagecount + pagesPerRow - 1) / pagesPerRow;
        const int targetRow = totalRows > 0 ? (std::min)((currentPage - 1) / pagesPerRow, totalRows - 1) : 0;
        const int targetPage = (targetRow * pagesPerRow) + 1;

        pdfInfo->m_currentPageIndex = targetPage;
        DisplayPageNumber(targetPage);

        PdfScrollViewer().UpdateLayout();

        const double scrollPercentage = totalRows > 1 ? static_cast<double>(targetRow) / (totalRows - 1) : 0.0;
        const double targetOffset = scrollPercentage * PdfScrollViewer().ScrollableHeight();

        PdfScrollViewer().ChangeView(nullptr, targetOffset, nullptr);
        RenderVisiblePagesAsync();
    }

    // PRINT
    winrt::fire_and_forget MainWindow::PrintButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (!currentTab || m_openedTabs.find(currentTab) == m_openedTabs.end())
        {
            co_return;
        }
        auto pdfInfo = m_openedTabs[currentTab];

        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);

        auto printInterop = winrt::get_activation_factory<winrt::Windows::Graphics::Printing::PrintManager, IPrintManagerInterop>();
        winrt::Windows::Graphics::Printing::PrintManager printManager{ nullptr };
        winrt::check_hresult(printInterop->GetForWindow(hwnd, winrt::guid_of<decltype(printManager)>(), winrt::put_abi(printManager)));

        m_printDocument = winrt::Microsoft::UI::Xaml::Printing::PrintDocument();
        auto printDocumentSource = m_printDocument.DocumentSource();

        auto printSize = std::make_shared<winrt::Windows::Foundation::Size>(0.0f, 0.0f);

        m_printDocument.Paginate([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::PaginateEventArgs const& args)
            {
                auto senderDoc = sender.template as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();

                auto pageDesc = args.PrintTaskOptions().GetPageDescription(1);
                printSize->Width = pageDesc.ImageableRect.Width;
                printSize->Height = pageDesc.ImageableRect.Height;

                senderDoc.SetPreviewPageCount(pdfInfo->m_pagecount, winrt::Microsoft::UI::Xaml::Printing::PreviewPageCountType::Final);
            });

        m_printDocument.GetPreviewPage([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::GetPreviewPageEventArgs const& args)
            {
                auto senderDoc = sender.template as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();
                if (args.PageNumber() > 0 && args.PageNumber() <= pdfInfo->m_pagecount)
                {
                    winrt::Microsoft::UI::Xaml::Controls::Grid pageContainer;
                    pageContainer.Width(printSize->Width);
                    pageContainer.Height(printSize->Height);

                    winrt::Microsoft::UI::Xaml::Controls::Image pageImage;
                    auto const& pageSlot = pdfInfo->m_renderedPages[args.PageNumber() - 1];
                    if (pageSlot.bitmap && pageSlot.state == PageRenderState::ready)
                    {
                        pageImage.Source(*pageSlot.bitmap);
                    }
                    pageImage.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
                    pageImage.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
                    pageImage.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

                    pageContainer.Children().Append(pageImage);

                    senderDoc.SetPreviewPage(args.PageNumber(), pageContainer);
                }
            });

        m_printDocument.AddPages([pdfInfo, printSize](auto const& sender, winrt::Microsoft::UI::Xaml::Printing::AddPagesEventArgs const& args)
            {
                auto senderDoc = sender.template as<winrt::Microsoft::UI::Xaml::Printing::PrintDocument>();
                for (int i = 0; i < pdfInfo->m_pagecount; ++i)
                {
                    winrt::Microsoft::UI::Xaml::Controls::Grid pageContainer;
                    pageContainer.Width(printSize->Width);
                    pageContainer.Height(printSize->Height);

                    winrt::Microsoft::UI::Xaml::Controls::Image pageImage;
                    auto const& pageSlot = pdfInfo->m_renderedPages[i];
                    if (pageSlot.bitmap && pageSlot.state == PageRenderState::ready)
                    {
                        pageImage.Source(*pageSlot.bitmap);
                    }
                    pageImage.Stretch(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);
                    pageImage.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
                    pageImage.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

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
            winrt::Windows::Graphics::Printing::PrintManager const&,
            winrt::Windows::Graphics::Printing::PrintTaskRequestedEventArgs const& args)
            {
                auto deferral = args.Request().GetDeferral();
                args.Request().CreatePrintTask(fileName, [printDocumentSource](winrt::Windows::Graphics::Printing::PrintTaskSourceRequestedArgs const& sourceArgs)
                    {
                        sourceArgs.SetSource(printDocumentSource);
                    });
                deferral.Complete();
            });

        try
        {
            winrt::Windows::Foundation::IAsyncOperation<bool> showPrintUIOp{ nullptr };
            winrt::check_hresult(printInterop->ShowPrintUIForWindowAsync(hwnd, winrt::guid_of<decltype(showPrintUIOp)>(), winrt::put_abi(showPrintUIOp)));

            co_await showPrintUIOp;
        }
        catch (winrt::hresult_error const& ex)
        {
            // TODO: printing fails
        }
    }
}
