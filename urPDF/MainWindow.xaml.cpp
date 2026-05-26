#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

extern "C" {
#include <mupdf/fitz.h>
}

#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <microsoft.ui.xaml.window.h>
#include <shobjidl_core.h>
#include <MemoryBuffer.h>

using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::urPDF::implementation
{
    winrt::fire_and_forget MainWindow::OpenButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        FileOpenPicker filePicker;
        filePicker.FileTypeFilter().Append(L".pdf");

        // picker window initialization
        HWND hwnd;
        this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
        filePicker.as<::IInitializeWithWindow>()->Initialize(hwnd);

        auto file = co_await filePicker.PickSingleFileAsync();
        if (!file) co_return;

        // muPDF render
        std::string path = winrt::to_string(file.Path());

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

        SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, width, height);
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

        // memory cleanup 
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
    }
}