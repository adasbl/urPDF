    #include "pch.h"
    #include "MainWindow.xaml.h"
    #if __has_include("MainWindow.g.cpp")
    #include "MainWindow.g.cpp"
    #endif

    extern "C" {
    #include <mupdf/fitz.h>
    }

    #include <winrt/Windows.Storage.h>
    #include <winrt/Windows.Storage.Pickers.h>
    #include <winrt/Windows.Storage.Streams.h>
    #include <winrt/Windows.Graphics.Imaging.h>
    #include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
    #include <microsoft.ui.xaml.window.h>
    #include <shobjidl_core.h>
    #include <MemoryBuffer.h>
    #include <map>
    #include <winrt/Microsoft.UI.Dispatching.h>
    #include <string>
    #include <memory>
    #include <coroutine>

    using namespace winrt;
    using namespace winrt::Windows::Storage::Pickers;
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage;
    using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Composition::SystemBackdrops;
    using namespace Microsoft::UI::Composition;
    

    namespace winrt::urPDF::implementation
    {
        MainWindow::MainWindow() 
        {
            InitializeComponent();

            ExtendsContentIntoTitleBar(true);
            SetTitleBar(AppTitleBar());

            winrt::Microsoft::UI::Xaml::Media::MicaBackdrop backdrop;
            backdrop.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
            SystemBackdrop(backdrop);

            m_ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
            fz_register_document_handlers(m_ctx);
        }

        MainWindow::~MainWindow()
        {
            if (m_ctx) {
                fz_drop_context(m_ctx);
                m_ctx = nullptr;
            }
        }

        PdfTabInfo::PdfTabInfo(fz_context* ctx, const std::string& filePath)
        {
            m_ctx = ctx;
            m_doc = fz_open_document(m_ctx, filePath.c_str());
        }

        PdfTabInfo::~PdfTabInfo()
        {
            fz_drop_document(m_ctx, m_doc);
        }

        void MainWindow::TabView_Loaded(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
            TabView_AddButtonClick(tabView, nullptr);
        }

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

        void SaveSettings()
        {
            auto localSettings = ApplicationData::Current().LocalSettings();

            localSettings.Values().Insert(L"AppTheme", winrt::box_value(L"Dark"));
            localSettings.Values().Insert(L"AutoSaveEnabled", winrt::box_value(true));
        }

        void LoadSettings()
        {
            auto localSettings = ApplicationData::Current().LocalSettings();

            if (localSettings.Values().HasKey(L"AppTheme"))
            {
                winrt::hstring theme = winrt::unbox_value<winrt::hstring>(localSettings.Values().Lookup(L"AppTheme"));
            }
        }

        winrt::fire_and_forget MainWindow::OpenPdfFileAsync(IInspectable const&, RoutedEventArgs const&)
        {
            // TODO: add check if empty tab is opened
            FileOpenPicker filePicker;
            filePicker.FileTypeFilter().Append(L".pdf");

            HWND hwnd;
            this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
            filePicker.as<::IInitializeWithWindow>()->Initialize(hwnd);

            auto file = co_await filePicker.PickSingleFileAsync();
            if (!file) co_return;

            std::string filePath = winrt::to_string(file.Path());
            winrt::hstring fileName = file.Name();

            auto PdfInfo = std::make_shared<PdfTabInfo>(m_ctx, filePath);
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
            targetTab.Header(winrt::box_value(fileName));

            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            targetTab.IconSource(tabIcon);

            m_openedTabs[targetTab] = PdfInfo;

            co_await RenderPageAsync(1.0, PdfInfo);
        }  

        winrt::Windows::Foundation::IAsyncAction MainWindow::RenderPageAsync(double zoomFactor, std::shared_ptr<PdfTabInfo> PdfTab)
        {
            co_await winrt::resume_background();

            fz_page* page = fz_load_page(m_ctx, PdfTab->m_doc, 0);

            fz_rect rect = fz_bound_page(m_ctx, page);
            fz_irect bbox = fz_round_rect(rect);


            fz_pixmap* pix = fz_new_pixmap_with_bbox(m_ctx, fz_device_bgr(m_ctx), bbox, NULL, 1);   // pixmap in BGR format
            fz_clear_pixmap_with_value(m_ctx, pix, 255); // white background

            fz_device* dev = fz_new_draw_device(m_ctx, fz_identity, pix);
            fz_run_page(m_ctx, page, dev, fz_identity, NULL);

            // WinUI format conversion
            int width = fz_pixmap_width(m_ctx, pix);
            int height = fz_pixmap_height(m_ctx, pix);

            SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
            {
                BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

                uint8_t* data = reference.data();

                memcpy(data, fz_pixmap_samples(m_ctx, pix), width * height * 4);  // copy pixels from MuPDF to WinUI
            }

            fz_drop_device(m_ctx, dev);
            fz_drop_pixmap(m_ctx, pix);
            fz_drop_page(m_ctx, page);

            co_await resume_foreground(DispatcherQueue());

            SoftwareBitmapSource bitmap_source;
            co_await bitmap_source.SetBitmapAsync(bitmap);

            winrt::Microsoft::UI::Xaml::Controls::Image img;
            img.Source(bitmap_source);
            img.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 10 });

            PagesContainer().Children().Clear();
            PagesContainer().Children().Append(img);

            auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
            if (currentTab)
            {
                currentTab.Tag(bitmap_source);
                currentTab.Header(winrt::box_value(PdfTab->m_fileName));
            }
        }

        // TABVIEW MENAGEMENT
        void MainWindow::TabView_AddButtonClick(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Windows::Foundation::IInspectable const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            winrt::Microsoft::UI::Xaml::Controls::TabViewItem newTab;

            newTab.Header(winrt::box_value(L"New document"));
            winrt::Microsoft::UI::Xaml::Controls::SymbolIconSource tabIcon;
            tabIcon.Symbol(winrt::Microsoft::UI::Xaml::Controls::Symbol::Document);
            newTab.IconSource(tabIcon);

            winrt::Microsoft::UI::Xaml::Controls::TextBlock newTabText;
            newTabText.Text(L"Here the document will be displayed");
            newTabText.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
            newTabText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);

            newTab.Content(newTabText);

            tabView.TabItems().Append(newTab);

            tabView.SelectedItem(newTab);
        }

        void MainWindow::TabView_TabCloseRequested(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            m_openedTabs.erase(args.Tab());
            
            uint32_t index;
            if (tabView.TabItems().IndexOf(args.Tab(), index))
            {
                tabView.TabItems().RemoveAt(index);
            }
        }

        void MainWindow::TabView_SelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args)
        {
            auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();

            auto currentTab = tabView.SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();

            if (currentTab)
            {
                auto savedSource = currentTab.Tag().try_as<winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource>();
                
                PagesContainer().Children().Clear();

                if (savedSource)
                {
                    winrt::Microsoft::UI::Xaml::Controls::Image img;
                    img.Source(savedSource);
                    img.Margin(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 10 });

                    PagesContainer().Children().Append(img);
                }
            }
        }

        void MainWindow::PdfScrollViewer_ViewChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const& args)
        {
            // TODO
        }

        void MainWindow::ToggleSwitch_Toggled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args)
        {
            auto toggleSwitch = sender.as<winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch>();

            if (toggleSwitch.IsOn())
            {
                // TODO
            }
            else
            {
                // TODO
            }
        }

        void MainWindow::ThemeSelection_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e)
        {

        }
    }
    
