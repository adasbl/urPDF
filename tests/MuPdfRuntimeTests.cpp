#include "CppUnitTest.h"
#include "../urPDF/MuPdfRuntime.h"

#include <future>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace urPDFTests
{
    static_assert(!std::is_copy_constructible_v<urpdf::MuPdfRuntime>);
    static_assert(!std::is_copy_assignable_v<urpdf::MuPdfRuntime>);
    static_assert(!std::is_move_constructible_v<urpdf::MuPdfRuntime>);
    static_assert(!std::is_move_assignable_v<urpdf::MuPdfRuntime>);

    namespace
    {
        bool RecognizesPdf(fz_context* context)
        {
            bool recognized = false;
            fz_try(context)
            {
                recognized = fz_recognize_document(context, "document.pdf") != nullptr;
            }
            fz_catch(context)
            {
                fz_report_error(context);
                return false;
            }
            return recognized;
        }

        // Translate longjmp into a C++ exception before unwinding context owners.
        void ThrowWorkerError(fz_context* context)
        {
            fz_try(context)
            {
                fz_throw(context, FZ_ERROR_GENERIC, "expected worker error");
            }
            fz_catch(context)
            {
                int code = 0;
                throw std::runtime_error(fz_convert_error(context, &code));
            }
        }

        fz_document* OpenTestDocument(fz_context* context)
        {
            static constexpr unsigned char pdf[] =
                "%PDF-1.4\n"
                "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
                "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
                "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> >>\nendobj\n"
                "xref\n0 4\n0000000000 65535 f \n0000000009 00000 n \n"
                "0000000058 00000 n \n0000000115 00000 n \n"
                "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n203\n%%EOF\n";
            fz_stream* stream = nullptr;
            fz_document* document = nullptr;
            fz_var(stream);
            fz_try(context)
            {
                stream = fz_open_memory(context, pdf, sizeof(pdf) - 1);
                document = fz_open_document_with_stream(context, "application/pdf", stream);
            }
            fz_always(context)
            {
                fz_drop_stream(context, stream);
            }
            fz_catch(context)
            {
                int code = 0;
                throw std::runtime_error(fz_convert_error(context, &code));
            }
            return document;
        }

        struct TestDocument
        {
            explicit TestDocument(std::shared_ptr<urpdf::MuPdfRuntime> owner)
                : runtime(std::move(owner)), context(runtime->CloneContext()), document(OpenTestDocument(context.get())) {}
            ~TestDocument() { fz_drop_document(context.get(), document); }

            std::shared_ptr<urpdf::MuPdfRuntime> runtime;
            urpdf::MuPdfRuntime::Context context;
            fz_document* document;
        };

        int CountTestPages(fz_context* context, fz_document* document)
        {
            int count = 0;
            fz_try(context) { count = fz_count_pages(context, document); }
            fz_catch(context)
            {
                int code = 0;
                throw std::runtime_error(fz_convert_error(context, &code));
            }
            return count;
        }
    }

    TEST_CLASS(MuPdfRuntimeTests)
    {
    public:
        TEST_METHOD(ShutdownWaitsForDocumentCleanupWithoutRunningQueuedUiCallback)
        {
            auto processOwner = std::make_shared<urpdf::MuPdfRuntime>();
            auto windowOwner = processOwner;
            auto windowContext = windowOwner->CloneContext();
            auto document = std::make_shared<TestDocument>(windowOwner);
            auto workerContext = processOwner->CloneContext();
            std::weak_ptr<urpdf::MuPdfRuntime> runtimeLifetime = processOwner;
            std::weak_ptr<TestDocument> documentLifetime = document;
            std::promise<void> entered;
            std::promise<void> finish;
            auto gate = finish.get_future();
            std::function<void()> queuedUiCallback;
            bool cleanedBeforePublication = false;
            int publishedPages = 0;
            auto registration = processOwner->Work().TryRegister();
            auto worker = std::async(std::launch::async,
                [&, registration = std::move(registration), document = std::move(document),
                    context = std::move(workerContext)]() mutable
                {
                    auto work = std::optional([&, document = std::move(document), context = std::move(context)]
                        {
                            entered.set_value();
                            gate.wait();
                            return CountTestPages(context.get(), document->document);
                        });
                    auto publish = [&](std::optional<int> pages, std::exception_ptr error)
                        {
                            cleanedBeforePublication = documentLifetime.expired();
                            publishedPages = pages && !error ? *pages : 0;
                            queuedUiCallback = [pages] { (void)pages; };
                        };
                    urpdf::ExecuteTrackedWork(std::move(registration), work, publish);
                });
            entered.get_future().wait();
            processOwner->Work().Stop();
            windowContext.reset();
            windowOwner.reset();
            finish.set_value();
            processOwner->Work().Wait();
            worker.get();
            Assert::IsTrue(cleanedBeforePublication);
            Assert::IsTrue(documentLifetime.expired());
            Assert::AreEqual(1, publishedPages);
            Assert::IsTrue(static_cast<bool>(queuedUiCallback));
            Assert::IsFalse(runtimeLifetime.expired());
            processOwner.reset();
            Assert::IsTrue(runtimeLifetime.expired());
        }

        TEST_METHOD(TrackedMuPdfErrorReleasesDocumentAndClonesBeforeShutdownCompletes)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto reference = runtime->CloneContext();
            const int initialCount = reference->master->context_count;
            auto document = std::make_shared<TestDocument>(runtime);
            std::weak_ptr<TestDocument> documentLifetime = document;
            auto context = runtime->CloneContext();
            bool errorPublishedAfterCleanup = false;
            auto registration = runtime->Work().TryRegister();
            auto worker = std::async(std::launch::async,
                [&, registration = std::move(registration), document = std::move(document), context = std::move(context)]() mutable
                {
                    auto work = std::optional([document = std::move(document), context = std::move(context)]() -> int
                        {
                            ThrowWorkerError(context.get());
                            return 0;
                        });
                    auto publish = [&](std::optional<int> result, std::exception_ptr error)
                        {
                            errorPublishedAfterCleanup = !result && error && documentLifetime.expired();
                        };
                    urpdf::ExecuteTrackedWork(std::move(registration), work, publish);
                });
            runtime->Work().Stop();
            runtime->Work().Wait();
            worker.get();
            Assert::IsTrue(errorPublishedAfterCleanup);
            Assert::AreEqual(initialCount, reference->master->context_count);
        }

        TEST_METHOD(ClonesShareResourcesAndLocks)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto first = runtime->CloneContext();
            auto second = runtime->CloneContext();

            Assert::IsTrue(first.get() != second.get());
            // These fields are exposed by the installed MuPDF headers.
            Assert::IsTrue(first->store == second->store);
            Assert::IsTrue(first->glyph_cache == second->glyph_cache);
            Assert::IsTrue(first->locks.user == second->locks.user);
            Assert::IsTrue(first->locks.lock == second->locks.lock);
            Assert::IsTrue(first->locks.unlock == second->locks.unlock);
            Assert::IsNotNull(first->locks.user);
            Assert::IsTrue(RecognizesPdf(first.get()));
            Assert::IsTrue(RecognizesPdf(second.get()));
        }

        TEST_METHOD(RemainingOwnerKeepsRuntimeAliveAfterWindowOwnerIsReleased)
        {
            auto appOwner = std::make_shared<urpdf::MuPdfRuntime>();
            std::weak_ptr<urpdf::MuPdfRuntime> lifetime = appOwner;
            auto documentOwner = appOwner;
            auto windowContext = appOwner->CloneContext();
            auto documentContext = documentOwner->CloneContext();

            windowContext.reset();
            appOwner.reset();

            Assert::IsFalse(lifetime.expired());
            Assert::IsTrue(RecognizesPdf(documentContext.get()));
            documentContext.reset();
            documentOwner.reset();
            Assert::IsTrue(lifetime.expired());
        }

        TEST_METHOD(ConcurrentCloneCreationAndDestruction)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto reference = runtime->CloneContext();
            auto* store = reference->store;
            void* locks = reference->locks.user;
            std::vector<std::future<bool>> workers;
            for (int worker = 0; worker < 8; ++worker)
            {
                workers.push_back(std::async(std::launch::async, [runtime, store, locks]
                {
                    for (int iteration = 0; iteration < 100; ++iteration)
                    {
                        auto context = runtime->CloneContext();
                        if (context->store != store || context->locks.user != locks || !RecognizesPdf(context.get()))
                        {
                            return false;
                        }
                    }
                    return true;
                }));
            }
            for (auto& worker : workers)
            {
                Assert::IsTrue(worker.get());
            }
        }

        TEST_METHOD(PreparedContextAndOwnerSurviveTransferToWorker)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            std::weak_ptr<urpdf::MuPdfRuntime> lifetime = runtime;
            auto preparedContext = runtime->CloneContext();
            std::promise<void> start;
            bool keptAlive = false;
            bool recognized = false;
            {
                auto worker = std::async(std::launch::async,
                    [owner = runtime, context = std::move(preparedContext), gate = start.get_future()]() mutable
                    {
                        auto workerContext = std::move(context);
                        gate.get();
                        return RecognizesPdf(workerContext.get());
                    });
                runtime.reset();
                keptAlive = !lifetime.expired();
                start.set_value();
                recognized = worker.get();
            }
            Assert::IsTrue(keptAlive);
            Assert::IsTrue(recognized);
            Assert::IsTrue(lifetime.expired());
        }

        TEST_METHOD(ContextCanBeReusedAfterConvertedMuPdfError)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto context = runtime->CloneContext();
            Assert::ExpectException<std::runtime_error>([&context] { ThrowWorkerError(context.get()); });
            Assert::AreEqual(static_cast<int>(FZ_ERROR_NONE), context->error.errcode);
            Assert::IsTrue(RecognizesPdf(context.get()));
        }

        TEST_METHOD(WorkerErrorReleasesPreparedCloneAndPreservesOtherContext)
        {
            auto runtime = std::make_shared<urpdf::MuPdfRuntime>();
            auto reference = runtime->CloneContext();
            // Observe MuPDF's family count only while no worker is running.
            const int initialCount = reference->master->context_count;
            {
                auto preparedContext = runtime->CloneContext();
                auto worker = std::async(std::launch::async,
                    [owner = runtime, context = std::move(preparedContext)]() mutable
                    {
                        auto workerContext = std::move(context);
                        ThrowWorkerError(workerContext.get());
                    });
                Assert::ExpectException<std::runtime_error>([&worker] { worker.get(); });
            }
            Assert::AreEqual(initialCount, reference->master->context_count);
            Assert::IsTrue(RecognizesPdf(reference.get()));
            auto nextContext = runtime->CloneContext();
            Assert::IsTrue(RecognizesPdf(nextContext.get()));
        }
    };
}
