#include "CppUnitTest.h"
#include "../urPDF/DocumentOperation.h"
#include "../urPDF/MuPdfDocument.h"
#include "../urPDF/MuPdfRuntime.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    namespace
    {
        struct DocumentLayout
        {
            int pageCount{ 0 };
            std::vector<fz_rect> pageBounds;
        };

        // Mirrors the document ownership of a tab: the tab's own clone opens and drops
        // the document, while the loading task reads it through a separate clone.
        struct TestTab
        {
            TestTab(std::shared_ptr<urpdf::MuPdfRuntime> owner, std::string documentPath)
                : runtime(std::move(owner)), context(runtime->CloneContext()), path(std::move(documentPath)) {}
            ~TestTab() { fz_drop_document(context.get(), document); }

            void Open() { if (!document) { document = urpdf::OpenPdfDocument(context.get(), path.c_str()); } }

            std::shared_ptr<urpdf::MuPdfRuntime> runtime;
            urpdf::MuPdfRuntime::Context context;
            std::mutex documentMutex;
            std::atomic<std::uint64_t> generation{ 1 };
            std::string path;
            fz_document* document{ nullptr };
        };

        std::string Utf8Path(std::filesystem::path const& path)
        {
            auto text = path.u8string();
            return { reinterpret_cast<char const*>(text.data()), text.size() };
        }

        std::filesystem::path WriteTestPdf()
        {
            static constexpr char pdf[] =
                "%PDF-1.4\n"
                "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
                "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
                "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> >>\nendobj\n"
                "xref\n0 4\n0000000000 65535 f \n0000000009 00000 n \n"
                "0000000058 00000 n \n0000000115 00000 n \n"
                "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n203\n%%EOF\n";
            auto directory = std::filesystem::temp_directory_path() / L"urPDF runtime tests \u0105\u0119\u0142\u00f3\u017c";
            std::filesystem::create_directories(directory);
            auto path = directory / L"Dokument \u017ad\u017ab\u0142o.pdf";
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(pdf, sizeof(pdf) - 1);
            return path;
        }

        // Runs the same steps as a tab's loading task: open, count pages and read their bounds
        // in one document operation.
        DocumentLayout LoadLayout(TestTab& tab, fz_context* context)
        {
            DocumentLayout layout;
            urpdf::RunDocumentOperation(tab.documentMutex, tab.generation, 1, [&]
                {
                    tab.Open();
                    layout.pageCount = urpdf::CountDocumentPages(context, tab.document);
                    layout.pageBounds = urpdf::CollectPageBounds(context, tab.document, layout.pageCount);
                });
            return layout;
        }

        struct LoadOutcome
        {
            std::optional<DocumentLayout> layout;
            std::exception_ptr error;
            bool tabReleasedBeforePublication{ false };
        };

        std::future<LoadOutcome> StartLoad(std::shared_ptr<urpdf::MuPdfRuntime> const& runtime,
            std::shared_ptr<TestTab> tab, std::shared_future<void> gate = {})
        {
            std::weak_ptr<TestTab> tabLifetime = tab;
            auto registration = runtime->Work().TryRegister();
            auto context = runtime->CloneContext();
            return std::async(std::launch::async,
                [registration = std::move(registration), tab = std::move(tab), context = std::move(context), gate, tabLifetime]() mutable
                {
                    LoadOutcome outcome;
                    auto work = std::optional([tab = std::move(tab), context = std::move(context), gate]
                        {
                            if (gate.valid()) { gate.wait(); }
                            return LoadLayout(*tab, context.get());
                        });
                    auto publish = [&](std::optional<DocumentLayout> layout, std::exception_ptr error)
                        {
                            outcome.layout = std::move(layout);
                            outcome.error = error;
                            outcome.tabReleasedBeforePublication = tabLifetime.expired();
                        };
                    urpdf::ExecuteTrackedWork(std::move(registration), work, publish);
                    return outcome;
                });
        }
    }

    TEST_CLASS(MuPdfDocumentTests)
    {
    public:
        TEST_METHOD(BackgroundLoadOpensDocumentWithUnicodePath)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto tab = std::make_shared<TestTab>(runtime, Utf8Path(WriteTestPdf()));
            Assert::IsNull(tab->document);

            auto outcome = StartLoad(runtime, tab).get();
            Assert::IsFalse(static_cast<bool>(outcome.error));
            Assert::IsTrue(outcome.layout.has_value());
            Assert::AreEqual(1, outcome.layout->pageCount);
            Assert::AreEqual(std::size_t{ 1 }, outcome.layout->pageBounds.size());
            Assert::AreEqual(100.0f, outcome.layout->pageBounds[0].x1 - outcome.layout->pageBounds[0].x0);
            Assert::AreEqual(100.0f, outcome.layout->pageBounds[0].y1 - outcome.layout->pageBounds[0].y0);
            Assert::IsNotNull(tab->document);
        }

        TEST_METHOD(BackgroundLoadPublishesOpenErrorAndReleasesResources)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto reference = runtime->CloneContext();
            const int initialCount = reference->master->context_count;
            auto missing = WriteTestPdf().parent_path() / L"missing.pdf";
            std::filesystem::remove(missing);
            auto tab = std::make_shared<TestTab>(runtime, Utf8Path(missing));

            auto outcome = StartLoad(runtime, std::move(tab)).get();
            Assert::IsFalse(outcome.layout.has_value());
            Assert::IsTrue(static_cast<bool>(outcome.error));
            Assert::ExpectException<std::runtime_error>([&] { std::rethrow_exception(outcome.error); });
            Assert::IsTrue(outcome.tabReleasedBeforePublication);
            Assert::AreEqual(initialCount, reference->master->context_count);
            Assert::IsTrue(static_cast<bool>(runtime->Work().TryRegister()));
        }

        TEST_METHOD(ShutdownDuringBackgroundLoadWaitsForDocumentCleanup)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            std::weak_ptr<urpdf::MuPdfRuntime> runtimeLifetime = runtime;
            auto windowOwner = runtime;
            auto tab = std::make_shared<TestTab>(windowOwner, Utf8Path(WriteTestPdf()));
            std::weak_ptr<TestTab> tabLifetime = tab;
            std::promise<void> finish;
            std::shared_future<void> gate = finish.get_future().share();

            auto load = StartLoad(runtime, std::move(tab), gate);
            runtime->Work().Stop();
            Assert::IsFalse(static_cast<bool>(runtime->Work().TryRegister()));
            windowOwner.reset();
            finish.set_value();
            runtime->Work().Wait();

            auto outcome = load.get();
            Assert::IsTrue(outcome.layout.has_value());
            Assert::IsTrue(outcome.tabReleasedBeforePublication);
            Assert::IsTrue(tabLifetime.expired());
            Assert::IsFalse(runtimeLifetime.expired());
            runtime.reset();
            Assert::IsTrue(runtimeLifetime.expired());
        }
    };
}
