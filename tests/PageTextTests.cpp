#include "CppUnitTest.h"
#include "../urPDF/MuPdfDocument.h"
#include "../urPDF/MuPdfRuntime.h"

#include <atomic>
#include <chrono>
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
        // Byte 0xAE (octal 256) is mapped to the "fi" ligature glyph.
        std::filesystem::path WriteTextPdf(int rotation)
        {
            const std::string content = "BT /F1 12 Tf 20 250 Td (Hello world) Tj 0 -20 Td (\\256nd) Tj ET";
            const std::vector<std::string> objects{
                "<< /Type /Catalog /Pages 2 0 R >>",
                "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Rotate " + std::to_string(rotation)
                    + " /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R /Annots [7 0 R] >>",
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica"
                    " /Encoding << /Type /Encoding /Differences [174 /fi] >> >>",
                "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Resources << >> >>",
                "<< /Type /Annot /Subtype /Link /Rect [15 240 120 270] /A << /S /URI /URI (https://example.com) >> >>"
            };
            std::ostringstream pdf;
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
            const auto name = "urpdf-text-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++serial) + ".pdf";
            auto path = std::filesystem::temp_directory_path() / name;
            std::ofstream file(path, std::ios::binary);
            file << pdf.str();
            return path;
        }

        struct TextDocument
        {
            explicit TextDocument(int rotation = 0)
                : runtime(std::make_shared<urpdf::MuPdfRuntime>()), context(runtime->CloneContext()), path(WriteTextPdf(rotation))
            {
                const auto utf8 = path.u8string();
                document = urpdf::OpenPdfDocument(context.get(), reinterpret_cast<char const*>(utf8.c_str()));
            }
            ~TextDocument()
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

        std::u32string LineText(urpdf::PageText const& text, urpdf::TextLine const& line)
        {
            std::u32string result;
            for (auto i = line.firstChar; i < line.endChar; ++i) { result.push_back(text.chars[i].codepoint); }
            return result;
        }

        void AssertCharsInside(urpdf::PageText const& text, urpdf::TextLine const& line, urpdf::PdfPageBounds area)
        {
            for (auto i = line.firstChar; i < line.endChar; ++i)
            {
                auto const& bounds = text.chars[i].bounds;
                Assert::IsTrue(bounds.x0 >= area.x0 && bounds.y0 >= area.y0 && bounds.x1 <= area.x1 && bounds.y1 <= area.y1);
            }
        }
    }

    TEST_CLASS(PageTextTests)
    {
    public:
        TEST_METHOD(LoadsLinesInReadingOrderOnClone)
        {
            TextDocument pdf;
            auto workerContext = pdf.runtime->CloneContext();
            const auto text = urpdf::LoadPageText(workerContext.get(), pdf.document, 0);
            Assert::AreEqual(std::size_t(2), text.lines.size());
            Assert::IsTrue(LineText(text, text.lines[0]) == U"Hello world");
            Assert::IsTrue(LineText(text, text.lines[1]) == U"find");
            Assert::IsTrue(text.lines[0].bounds.y1 <= text.lines[1].bounds.y0);
            Assert::AreEqual(std::size_t(15), text.chars.size());
            Assert::IsTrue(urpdf::LoadPageText(workerContext.get(), pdf.document, 1).chars.empty());
        }

        TEST_METHOD(SplitsLigatureIntoLettersWithSplitBounds)
        {
            TextDocument pdf;
            const auto text = urpdf::LoadPageText(pdf.context.get(), pdf.document, 0);
            auto const& f = text.chars[text.lines[1].firstChar];
            auto const& i = text.chars[text.lines[1].firstChar + 1];
            Assert::IsTrue(f.codepoint == U'f');
            Assert::IsTrue(i.codepoint == U'i');
            Assert::IsTrue(f.bounds.x0 < f.bounds.x1);
            Assert::IsTrue(f.bounds.x1 <= i.bounds.x0 + 0.01);
        }

        TEST_METHOD(TextBoundsShareLinkPageSpace)
        {
            for (int rotation : { 0, 90 })
            {
                TextDocument pdf(rotation);
                const auto links = urpdf::CollectPageLinks(pdf.context.get(), pdf.document, 0);
                Assert::AreEqual(std::size_t(1), links.size());
                const auto text = urpdf::LoadPageText(pdf.context.get(), pdf.document, 0);
                AssertCharsInside(text, text.lines[0], links.front().rect);
                const auto caret = urpdf::HitTest(text, { (links.front().rect.x0 + links.front().rect.x1) / 2.0,
                    (links.front().rect.y0 + links.front().rect.y1) / 2.0 });
                Assert::IsTrue(caret >= text.lines[0].firstChar && caret <= text.lines[0].endChar);
            }
        }

        TEST_METHOD(PageLoadErrorsLeaveContextUsable)
        {
            TextDocument pdf;
            Assert::ExpectException<std::runtime_error>([&] { urpdf::LoadPageText(pdf.context.get(), pdf.document, 99); });
            Assert::AreEqual(std::size_t(2), urpdf::LoadPageText(pdf.context.get(), pdf.document, 0).lines.size());
        }
    };
}
