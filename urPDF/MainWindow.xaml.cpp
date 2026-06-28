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

using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Composition::SystemBackdrops;
using namespace Microsoft::UI::Composition;

namespace winrt::urPDF::implementation
{
    MainWindow::MainWindow() 
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar(true);

        winrt::Microsoft::UI::Xaml::Media::MicaBackdrop backdrop;
        backdrop.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
        SystemBackdrop(backdrop);

        SetTitleBar(AppTitleBar());
    }

    // tabView
    void MainWindow::TabView_Loaded(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e)
    {
        auto tabView = sender.as<winrt::Microsoft::UI::Xaml::Controls::TabView>();
        TabView_AddButtonClick(tabView, nullptr);
    }

    void MainWindow::TabView_AddButtonClick(
        winrt::Microsoft::UI::Xaml::Controls::TabView const& sender,
        winrt::Windows::Foundation::IInspectable const& args)
    {
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

        sender.TabItems().Append(newTab);

        sender.SelectedItem(newTab);
    }

    void MainWindow::TabView_TabCloseRequested(
        winrt::Microsoft::UI::Xaml::Controls::TabView const& sender,
        winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const& args)
    {
        uint32_t index;
        if (sender.TabItems().IndexOf(args.Tab(), index))
        {
            sender.TabItems().RemoveAt(index);
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

            PdfImageDisplay().Source(savedSource);
        }
    }
       

    winrt::fire_and_forget MainWindow::OpenButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        FileOpenPicker filePicker;
        filePicker.FileTypeFilter().Append(L".pdf");

        // picker window initialization
        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
        filePicker.as<::IInitializeWithWindow>()->Initialize(hwnd);

        // file loading
        auto file = co_await filePicker.PickSingleFileAsync();
        if (!file) co_return;

        std::string path = winrt::to_string(file.Path());

        // muPDF render
        fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
        fz_register_document_handlers(ctx);

        fz_document* doc = fz_open_document(ctx, path.c_str());
        fz_page* page = fz_load_page(ctx, doc, 0);

        fz_rect rect = fz_bound_page(ctx, page);    // download page size
        fz_irect bbox = fz_round_rect(rect);


        fz_pixmap* pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), bbox, NULL, 1);   // pixmap in BGR format
        fz_clear_pixmap_with_value(ctx, pix, 255); // white background

        fz_device* dev = fz_new_draw_device(ctx, fz_identity, pix);
        fz_run_page(ctx, page, dev, fz_identity, NULL);

        // WinUI format conversion
        int width = fz_pixmap_width(ctx, pix);
        int height = fz_pixmap_height(ctx, pix);

        SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height, BitmapAlphaMode::Premultiplied);
        {
            BitmapBuffer buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
            winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();

            uint8_t* data = reference.data();

            memcpy(data, fz_pixmap_samples(ctx, pix), width * height * 4);  // copy pixels from MuPDF to WinUI
        }

        // display
        SoftwareBitmapSource source;
        co_await source.SetBitmapAsync(bitmap);
        PdfImageDisplay().Source(source);

        auto currentTab = MainTabView().SelectedItem().try_as<winrt::Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (currentTab)
        {
            currentTab.Tag(source);
            currentTab.Header(winrt::box_value(file.Name()));
        }

        // memory cleanup 
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
    }
}