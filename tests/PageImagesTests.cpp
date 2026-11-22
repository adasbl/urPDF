#include "CppUnitTest.h"
#include "../urPDF/MuPdfDocument.h"
#include "../urPDF/MuPdfRuntime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        std::string MakeRawStream(std::string const& dict, std::vector<std::uint8_t> const& bytes)
        {
            std::string raw(reinterpret_cast<char const*>(bytes.data()), bytes.size());
            return "<< " + dict + " /Length " + std::to_string(bytes.size()) + " >>\nstream\n" + raw + "\nendstream";
        }

        std::filesystem::path WriteImagesPdf(int rotation)
        {
            // 2x2 RGB: top row red (255,0,0) & green (0,255,0), bottom row blue (0,0,255) & white (255,255,255).
            const std::vector<std::uint8_t> rgbPixels{
                255, 0, 0,   0, 255, 0,
                0, 0, 255,   255, 255, 255
            };
            // 2x2 CMYK: pure cyan, magenta, yellow, black.
            const std::vector<std::uint8_t> cmykPixels{
                255, 0, 0, 0,   0, 255, 0, 0,
                0, 0, 255, 0,   0, 0, 0, 255
            };
            // 2x2 soft mask: top-left opaque (255), top-right transparent (0), bottom-left half (128), bottom-right opaque (255).
            const std::vector<std::uint8_t> smaskPixels{
                255, 0,
                128, 255
            };

            const std::string page1Content =
                "q 60 0 0 40 20 200 cm /ImRgb Do Q\n"
                "q 50 0 0 50 110 100 cm /ImCmyk Do Q\n"
                "q 40 0 0 40 20 50 cm /ImMasked Do Q";

            const std::vector<std::string> objects{
                "<< /Type /Catalog /Pages 2 0 R >>",
                "<< /Type /Pages /Kids [3 0 R 9 0 R] /Count 2 >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Rotate " + std::to_string(rotation)
                    + " /Resources << /XObject << /ImRgb 5 0 R /ImCmyk 6 0 R /ImMasked 7 0 R >> >> /Contents 4 0 R >>",
                "<< /Length " + std::to_string(page1Content.size()) + " >>\nstream\n" + page1Content + "\nendstream",
                MakeRawStream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8", rgbPixels),
                MakeRawStream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceCMYK /BitsPerComponent 8", cmykPixels),
                MakeRawStream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 8 0 R", rgbPixels),
                MakeRawStream("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8", smaskPixels),
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Resources << >> >>"
            };

            std::ostringstream pdf(std::ios::binary);
            pdf << "%PDF-1.4\n";
            std::vector<std::streamoff> offsets{ 0 };
            for (std::size_t i = 0; i < objects.size(); ++i)
            {
                offsets.push_back(pdf.tellp());
                pdf << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
            }
            const auto xref = pdf.tellp();
            pdf << "xref\n0 " << offsets.size() << "\n0000000000 65535 f \n";
            for (std::size_t i = 1; i < offsets.size(); ++i)
            {
                pdf << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
            }
            pdf << "trailer\n<< /Size " << offsets.size() << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";

            static std::atomic<unsigned int> serial{ 0 };
            const auto name = "urpdf-images-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++serial) + ".pdf";
            auto path = std::filesystem::temp_directory_path() / name;
            std::ofstream file(path, std::ios::binary);
            const auto content = pdf.str();
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            return path;
        }

        struct ImagesDocument
        {
            explicit ImagesDocument(int rotation = 0)
                : runtime(std::make_shared<urpdf::MuPdfRuntime>()), context(runtime->CloneContext()), path(WriteImagesPdf(rotation))
            {
                const auto utf8 = path.u8string();
                document = urpdf::OpenPdfDocument(context.get(), reinterpret_cast<char const*>(utf8.c_str()));
            }
            ~ImagesDocument()
            {
                fz_drop_document(context.get(), document);
                std::error_code error;
                std::filesystem::remove(path, error);
            }
            std::shared_ptr<urpdf::MuPdfRuntime> runtime;
            urpdf::MuPdfRuntime::Context context;
            std::filesystem::path path;
            fz_document* document{ nullptr };
        };

        struct DecodedPng
        {
            int width{ 0 };
            int height{ 0 };
            int components{ 0 };
            int alpha{ 0 };
            std::vector<std::uint8_t> pixels;
        };

        DecodedPng DecodePng(fz_context* ctx, std::vector<std::uint8_t> const& png)
        {
            static constexpr std::array<std::uint8_t, 8> kPngSignature{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
            Assert::IsTrue(png.size() > kPngSignature.size());
            for (std::size_t i = 0; i < kPngSignature.size(); ++i)
            {
                Assert::AreEqual(kPngSignature[i], png[i]);
            }

            fz_buffer* buffer = nullptr;
            fz_image* image = nullptr;
            fz_pixmap* pixmap = nullptr;
            DecodedPng decoded;
            fz_var(buffer);
            fz_var(image);
            fz_var(pixmap);
            fz_try(ctx)
            {
                buffer = fz_new_buffer_from_copied_data(ctx, png.data(), png.size());
                image = fz_new_image_from_buffer(ctx, buffer);
                pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
                decoded.width = fz_pixmap_width(ctx, pixmap);
                decoded.height = fz_pixmap_height(ctx, pixmap);
                decoded.components = fz_pixmap_components(ctx, pixmap);
                decoded.alpha = fz_pixmap_alpha(ctx, pixmap);
                const auto* samples = fz_pixmap_samples(ctx, pixmap);
                const auto stride = static_cast<std::size_t>(fz_pixmap_stride(ctx, pixmap));
                const auto rowBytes = static_cast<std::size_t>(decoded.width) * static_cast<std::size_t>(decoded.components);
                decoded.pixels.resize(rowBytes * static_cast<std::size_t>(decoded.height));
                for (int y = 0; y < decoded.height; ++y)
                {
                    std::copy_n(samples + static_cast<std::size_t>(y) * stride, rowBytes,
                        decoded.pixels.data() + static_cast<std::size_t>(y) * rowBytes);
                }
            }
            fz_always(ctx)
            {
                fz_drop_pixmap(ctx, pixmap);
                fz_drop_image(ctx, image);
                fz_drop_buffer(ctx, buffer);
            }
            fz_catch(ctx)
            {
                Assert::Fail(L"MuPDF failed to decode encoded PNG.");
            }
            return decoded;
        }
    }

    TEST_CLASS(PageImagesTests)
    {
    public:
        TEST_METHOD(DetectsImageBlocksWithPageSpaceBounds)
        {
            ImagesDocument pdf;
            auto workerContext = pdf.runtime->CloneContext();
            const auto text = urpdf::LoadPageText(workerContext.get(), pdf.document, 0);
            Assert::AreEqual(std::size_t(3), text.images.size());
            Assert::AreEqual(std::uint32_t(0), text.images[0].index);
            Assert::AreEqual(20.0, text.images[0].bounds.x0, 0.5);
            Assert::AreEqual(60.0, text.images[0].bounds.y0, 0.5);
            Assert::AreEqual(80.0, text.images[0].bounds.x1, 0.5);
            Assert::AreEqual(100.0, text.images[0].bounds.y1, 0.5);

            Assert::IsTrue(urpdf::LoadPageText(workerContext.get(), pdf.document, 1).images.empty());
        }

        TEST_METHOD(RotatedImageBoundsUseMuPdfPageSpace)
        {
            ImagesDocument pdf(90);
            const auto text = urpdf::LoadPageText(pdf.context.get(), pdf.document, 0);
            Assert::AreEqual(std::size_t(3), text.images.size());
            // MediaBox 200x300 rotated 90 CW: PDF rect [20,200,80,240] maps to [200,20,240,80].
            Assert::AreEqual(200.0, text.images[0].bounds.x0, 0.5);
            Assert::AreEqual(20.0, text.images[0].bounds.y0, 0.5);
            Assert::AreEqual(240.0, text.images[0].bounds.x1, 0.5);
            Assert::AreEqual(80.0, text.images[0].bounds.y1, 0.5);
        }

        TEST_METHOD(EncodesRgbCmykAndSoftMaskedImagesToPng)
        {
            ImagesDocument pdf;
            auto workerContext = pdf.runtime->CloneContext();

            // 0: RGB 2x2
            const auto rgbPng = urpdf::ExtractPageImagePng(workerContext.get(), pdf.document, 0, 0);
            const auto rgb = DecodePng(workerContext.get(), rgbPng);
            Assert::AreEqual(2, rgb.width);
            Assert::AreEqual(2, rgb.height);
            Assert::AreEqual(3, rgb.components);
            Assert::AreEqual(std::uint8_t(255), rgb.pixels[0]); // top-left R
            Assert::AreEqual(std::uint8_t(0), rgb.pixels[1]);
            Assert::AreEqual(std::uint8_t(0), rgb.pixels[2]);
            Assert::AreEqual(std::uint8_t(0), rgb.pixels[6]);   // bottom-left B
            Assert::AreEqual(std::uint8_t(0), rgb.pixels[7]);
            Assert::AreEqual(std::uint8_t(255), rgb.pixels[8]);

            // 1: CMYK 2x2 converted to RGB PNG
            const auto cmykPng = urpdf::ExtractPageImagePng(workerContext.get(), pdf.document, 0, 1);
            const auto cmyk = DecodePng(workerContext.get(), cmykPng);
            Assert::AreEqual(2, cmyk.width);
            Assert::AreEqual(2, cmyk.height);
            Assert::AreEqual(3, cmyk.components);

            // 2: RGB + SMask 2x2 preserves orientation and alpha
            const auto maskedPng = urpdf::ExtractPageImagePng(workerContext.get(), pdf.document, 0, 2);
            const auto masked = DecodePng(workerContext.get(), maskedPng);
            Assert::AreEqual(2, masked.width);
            Assert::AreEqual(2, masked.height);
            Assert::AreEqual(4, masked.components);
            Assert::AreEqual(1, masked.alpha);
            // Top-left: opaque red (255, 0, 0, 255)
            Assert::AreEqual(std::uint8_t(255), masked.pixels[0]);
            Assert::AreEqual(std::uint8_t(0), masked.pixels[1]);
            Assert::AreEqual(std::uint8_t(0), masked.pixels[2]);
            Assert::AreEqual(std::uint8_t(255), masked.pixels[3]);
            // Top-right: transparent (alpha 0)
            Assert::AreEqual(std::uint8_t(0), masked.pixels[7]);
            // Bottom-left: semi-transparent blue (alpha ~128)
            Assert::IsTrue(masked.pixels[11] >= 120 && masked.pixels[11] <= 136);
            // Bottom-right: opaque white
            Assert::AreEqual(std::uint8_t(255), masked.pixels[15]);
        }

        TEST_METHOD(InvalidImageIndexOrPageLeavesContextUsable)
        {
            ImagesDocument pdf;
            Assert::ExpectException<std::runtime_error>([&]
                {
                    urpdf::ExtractPageImagePng(pdf.context.get(), pdf.document, 0, 99);
                });
            Assert::ExpectException<std::runtime_error>([&]
                {
                    urpdf::ExtractPageImagePng(pdf.context.get(), pdf.document, 99, 0);
                });
            const auto png = urpdf::ExtractPageImagePng(pdf.context.get(), pdf.document, 0, 0);
            Assert::IsFalse(png.empty());
        }
    };
}
