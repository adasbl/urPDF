#include "CppUnitTest.h"
#include "../urPDF/BackgroundWork.h"
#include "../urPDF/DocumentOperation.h"

#include <chrono>
#include <future>
#include <stdexcept>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace urPDFTests
{
    TEST_CLASS(BackgroundWorkTests)
    {
    public:
        TEST_METHOD(RejectedRegistrationDoesNotRunOrPublishWork)
        {
            urpdf::BackgroundWorkTracker tracker;
            tracker.Stop();
            auto owner = std::make_shared<int>(42);
            std::weak_ptr<int> lifetime = owner;
            bool ran = false;
            bool published = false;
            auto work = std::optional([owner = std::move(owner), &ran]
                {
                    ran = true;
                    return *owner;
                });
            auto publish = [&](std::optional<int>, std::exception_ptr) { published = true; };
            urpdf::ExecuteTrackedWork(tracker.TryRegister(), work, publish);
            tracker.Wait();
            Assert::IsFalse(ran);
            Assert::IsFalse(published);
            Assert::IsTrue(lifetime.expired());
        }

        TEST_METHOD(StopRejectsNewWorkAndWaitsForExistingRegistration)
        {
            urpdf::BackgroundWorkTracker tracker;
            auto registration = tracker.TryRegister();
            Assert::IsTrue(static_cast<bool>(registration));
            tracker.Stop();
            Assert::IsFalse(static_cast<bool>(tracker.TryRegister()));
            auto waiter = std::async(std::launch::async, [&] { tracker.Wait(); });
            const auto busy = waiter.wait_for(50ms);
            registration.Reset();
            waiter.get();
            Assert::IsTrue(busy == std::future_status::timeout);
            Assert::IsFalse(static_cast<bool>(tracker.TryRegister()));
        }

        TEST_METHOD(MovedAndAbandonedRegistrationsAreReleasedExactlyOnce)
        {
            urpdf::BackgroundWorkTracker tracker;
            {
                auto first = tracker.TryRegister();
                auto second = tracker.TryRegister();
                second = std::move(first);
                Assert::IsFalse(static_cast<bool>(first));
                Assert::IsTrue(static_cast<bool>(second));
                // Simulate an exception before the scheduler accepts the work.
                try
                {
                    auto failedSubmission = tracker.TryRegister();
                    throw std::runtime_error("submission rejected");
                }
                catch (std::runtime_error const&) {}
            }
            tracker.Stop();
            tracker.Wait();
        }

        TEST_METHOD(RejectedPublicationDoesNotRetainNativeResources)
        {
            urpdf::BackgroundWorkTracker tracker;
            auto nativeOwner = std::make_shared<int>(42);
            std::weak_ptr<int> lifetime = nativeOwner;
            auto work = std::optional([owner = std::move(nativeOwner)] { return *owner; });
            bool cleanedBeforePublication = false;
            bool receivedResult = false;
            auto publish = [&](std::optional<int> result, std::exception_ptr error)
                {
                    cleanedBeforePublication = lifetime.expired();
                    receivedResult = result == 42 && !error;
                    return false; // DispatcherQueue::TryEnqueue rejected the callback.
                };
            urpdf::ExecuteTrackedWork(tracker.TryRegister(), work, publish);
            tracker.Stop();
            tracker.Wait();
            Assert::IsFalse(work.has_value());
            Assert::IsTrue(cleanedBeforePublication);
            Assert::IsTrue(receivedResult);
        }

        TEST_METHOD(AcceptedCallbackMayRemainUnexecutedAfterShutdown)
        {
            urpdf::BackgroundWorkTracker tracker;
            auto nativeOwner = std::make_shared<int>(42);
            std::weak_ptr<int> lifetime = nativeOwner;
            auto work = std::optional([owner = std::move(nativeOwner)] { return *owner; });
            std::function<void()> queuedCallback;
            bool ran = false;
            auto publish = [&](std::optional<int> result, std::exception_ptr)
                {
                    queuedCallback = [value = *result, &ran] { ran = value == 42; };
                };
            urpdf::ExecuteTrackedWork(tracker.TryRegister(), work, publish);
            tracker.Stop();
            tracker.Wait();
            Assert::IsTrue(lifetime.expired());
            Assert::IsTrue(static_cast<bool>(queuedCallback));
            Assert::IsFalse(ran);
        }

        TEST_METHOD(WorkerExceptionCleansUpBeforeErrorPublication)
        {
            urpdf::BackgroundWorkTracker tracker;
            auto nativeOwner = std::make_shared<int>(42);
            std::weak_ptr<int> lifetime = nativeOwner;
            auto work = std::optional([owner = std::move(nativeOwner)]() -> int
                {
                    throw std::runtime_error("native operation failed");
                });
            bool cleanedBeforePublication = false;
            bool receivedError = false;
            auto publish = [&](std::optional<int> result, std::exception_ptr error)
                {
                    cleanedBeforePublication = lifetime.expired();
                    receivedError = !result && error != nullptr;
                };
            urpdf::ExecuteTrackedWork(tracker.TryRegister(), work, publish);
            tracker.Stop();
            tracker.Wait();
            Assert::IsTrue(cleanedBeforePublication);
            Assert::IsTrue(receivedError);
        }

        TEST_METHOD(PublicationExceptionStillReleasesRegistration)
        {
            urpdf::BackgroundWorkTracker tracker;
            auto nativeOwner = std::make_shared<int>(42);
            std::weak_ptr<int> lifetime = nativeOwner;
            auto work = std::optional([owner = std::move(nativeOwner)] { return *owner; });
            auto publish = [](std::optional<int>, std::exception_ptr)
                {
                    throw std::runtime_error("dispatcher unavailable");
                };
            Assert::ExpectException<std::runtime_error>([&]
                {
                    urpdf::ExecuteTrackedWork(tracker.TryRegister(), work, publish);
                });
            tracker.Stop();
            tracker.Wait();
            Assert::IsTrue(lifetime.expired());
        }

        TEST_METHOD(WaitIncludesPublicationWithoutNeedingItsQueuedCallback)
        {
            urpdf::BackgroundWorkTracker tracker;
            auto registration = tracker.TryRegister();
            std::promise<void> publishing;
            std::promise<void> releasePublisher;
            auto release = releasePublisher.get_future();
            auto worker = std::async(std::launch::async, [&, registration = std::move(registration)]() mutable
                {
                    auto work = std::optional([] { return 42; });
                    auto publish = [&](std::optional<int>, std::exception_ptr)
                        {
                            publishing.set_value();
                            release.wait();
                        };
                    urpdf::ExecuteTrackedWork(std::move(registration), work, publish);
                });
            publishing.get_future().wait();
            tracker.Stop();
            auto waiter = std::async(std::launch::async, [&] { tracker.Wait(); });
            const auto busy = waiter.wait_for(50ms);
            releasePublisher.set_value();
            worker.get();
            waiter.get();
            Assert::IsTrue(busy == std::future_status::timeout);
        }

        TEST_METHOD(ShutdownWhileWaitingForDocumentSkipsNativeWork)
        {
            urpdf::BackgroundWorkTracker tracker;
            std::mutex documentMutex;
            std::unique_lock busyDocument(documentMutex);
            std::atomic<std::uint64_t> generation{ 1 };
            std::atomic<bool> windowClosed{ false };
            std::promise<void> waiting;
            bool accessedDocument = false;
            bool skipped = false;
            auto registration = tracker.TryRegister();
            auto worker = std::async(std::launch::async, [&, registration = std::move(registration)]() mutable
                {
                    auto work = std::optional([&]
                        {
                            waiting.set_value();
                            return urpdf::RunDocumentOperation(documentMutex, generation, 1, [&]
                                {
                                    if (windowClosed.load()) { return; }
                                    accessedDocument = true;
                                });
                        });
                    auto publish = [&](std::optional<bool> ran, std::exception_ptr error)
                        {
                            skipped = ran && !*ran && !error;
                        };
                    urpdf::ExecuteTrackedWork(std::move(registration), work, publish);
                });
            waiting.get_future().wait();
            windowClosed.store(true);
            generation.fetch_add(1);
            tracker.Stop();
            auto waiter = std::async(std::launch::async, [&] { tracker.Wait(); });
            const auto busy = waiter.wait_for(50ms);
            busyDocument.unlock();
            worker.get();
            waiter.get();
            Assert::IsTrue(busy == std::future_status::timeout);
            Assert::IsFalse(accessedDocument);
            Assert::IsTrue(skipped);
        }
    };
}
