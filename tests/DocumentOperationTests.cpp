#include "CppUnitTest.h"
#include "../urPDF/DocumentOperation.h"

#include <chrono>
#include <future>
#include <stdexcept>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace urPDFTests
{
    TEST_CLASS(DocumentOperationTests)
    {
    public:
        TEST_METHOD(WaitingOperationSkipsObsoleteGeneration)
        {
            std::mutex mutex;
            std::atomic<std::uint64_t> generation{ 1 };
            std::unique_lock busyDocument(mutex);
            std::promise<void> started;
            bool accessedDocument = false;
            auto worker = std::async(std::launch::async, [&]
                {
                    started.set_value();
                    return urpdf::RunDocumentOperation(mutex, generation, 1, [&]
                        {
                            accessedDocument = true;
                        });
                });

            started.get_future().wait();
            const auto waiting = worker.wait_for(50ms);
            generation.fetch_add(1, std::memory_order_acq_rel);
            busyDocument.unlock();
            const bool ran = worker.get();

            Assert::IsTrue(waiting == std::future_status::timeout);
            Assert::IsFalse(ran);
            Assert::IsFalse(accessedDocument);
        }

        TEST_METHOD(NewGenerationWaitsForRunningOperation)
        {
            std::mutex mutex;
            std::atomic<std::uint64_t> generation{ 1 };
            std::promise<void> entered;
            std::promise<void> finishOldOperation;
            auto finish = finishOldOperation.get_future();
            std::vector<int> order;
            auto oldOperation = std::async(std::launch::async, [&]
                {
                    return urpdf::RunDocumentOperation(mutex, generation, 1, [&]
                        {
                            order.push_back(1);
                            entered.set_value();
                            finish.wait();
                            order.push_back(2);
                        });
                });

            entered.get_future().wait();
            generation.fetch_add(1, std::memory_order_acq_rel);
            std::promise<void> started;
            auto newOperation = std::async(std::launch::async, [&]
                {
                    started.set_value();
                    return urpdf::RunDocumentOperation(mutex, generation, 2, [&]
                        {
                            order.push_back(3);
                        });
                });
            started.get_future().wait();
            const auto waiting = newOperation.wait_for(50ms);
            finishOldOperation.set_value();
            const bool oldRan = oldOperation.get();
            const bool newRan = newOperation.get();

            Assert::IsTrue(waiting == std::future_status::timeout);
            // Invalidation does not interrupt an operation already inside MuPDF.
            Assert::IsTrue(oldRan);
            Assert::IsTrue(newRan);
            Assert::IsTrue(order == std::vector<int>{ 1, 2, 3 });
        }

        TEST_METHOD(IndependentDocumentsDoNotBlockEachOther)
        {
            std::mutex firstMutex;
            std::mutex secondMutex;
            std::atomic<std::uint64_t> firstGeneration{ 1 };
            std::atomic<std::uint64_t> secondGeneration{ 1 };
            std::promise<void> entered;
            std::promise<void> finishFirstOperation;
            auto finish = finishFirstOperation.get_future();
            auto first = std::async(std::launch::async, [&]
                {
                    return urpdf::RunDocumentOperation(firstMutex, firstGeneration, 1, [&]
                        {
                            entered.set_value();
                            finish.wait();
                        });
                });
            entered.get_future().wait();
            auto second = std::async(std::launch::async, [&]
                {
                    return urpdf::RunDocumentOperation(secondMutex, secondGeneration, 1, [] {});
                });

            const auto secondStatus = second.wait_for(5s);
            finishFirstOperation.set_value();
            const bool firstRan = first.get();
            const bool secondRan = second.get();

            Assert::IsTrue(secondStatus == std::future_status::ready);
            Assert::IsTrue(firstRan);
            Assert::IsTrue(secondRan);
        }

        TEST_METHOD(FailedOperationReleasesDocumentForNextWorker)
        {
            std::mutex mutex;
            std::atomic<std::uint64_t> generation{ 1 };
            auto failed = std::async(std::launch::async, [&]
                {
                    urpdf::RunDocumentOperation(mutex, generation, 1, []
                        {
                            throw std::runtime_error("expected document operation error");
                        });
                });
            Assert::ExpectException<std::runtime_error>([&] { failed.get(); });

            // Probe without blocking so a leaked lock reports a failure.
            std::unique_lock probe(mutex, std::try_to_lock);
            Assert::IsTrue(probe.owns_lock());
            probe.unlock();
            auto next = std::async(std::launch::async, [&]
                {
                    return urpdf::RunDocumentOperation(mutex, generation, 1, [] {});
                });
            Assert::IsTrue(next.get());
        }
    };
}
