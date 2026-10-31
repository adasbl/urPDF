#include "CppUnitTest.h"
#include "../urPDF/MuPdfRuntime.h"

#include <future>
#include <type_traits>
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
                return false;
            }
            return recognized;
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
    };
}
