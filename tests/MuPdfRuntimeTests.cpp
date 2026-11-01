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
    }

    TEST_CLASS(MuPdfRuntimeTests)
    {
    public:
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
