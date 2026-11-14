#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace urpdf
{
    class BackgroundWorkTracker final
    {
        struct State
        {
            std::mutex mutex;
            std::condition_variable idle;
            std::size_t active{ 0 };
            bool stopping{ false };
        };

    public:
        class Registration final
        {
        public:
            Registration() = default;
            Registration(Registration const&) = delete;
            Registration& operator=(Registration const&) = delete;
            Registration(Registration&&) noexcept = default;
            Registration& operator=(Registration&& other) noexcept
            {
                if (this != &other)
                {
                    Reset();
                    m_state = std::move(other.m_state);
                }
                return *this;
            }
            ~Registration() { Reset(); }

            explicit operator bool() const noexcept { return m_state != nullptr; }

            void Reset() noexcept
            {
                auto state = std::move(m_state);
                if (!state) { return; }
                {
                    std::lock_guard lock(state->mutex);
                    --state->active;
                }
                state->idle.notify_all();
            }

        private:
            friend class BackgroundWorkTracker;
            explicit Registration(std::shared_ptr<State> state) : m_state(std::move(state)) {}
            std::shared_ptr<State> m_state;
        };

        BackgroundWorkTracker() = default;
        BackgroundWorkTracker(BackgroundWorkTracker const&) = delete;
        BackgroundWorkTracker& operator=(BackgroundWorkTracker const&) = delete;

        Registration TryRegister()
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->stopping) { return {}; }
            ++m_state->active;
            return Registration(m_state);
        }

        void Stop() noexcept
        {
            std::lock_guard lock(m_state->mutex);
            m_state->stopping = true;
        }

        // Only wait after the UI loop exits. Completion never requires the UI.
        void Wait()
        {
            std::unique_lock lock(m_state->mutex);
            m_state->idle.wait(lock, [&] { return m_state->active == 0; });
        }

    private:
        std::shared_ptr<State> m_state{ std::make_shared<State>() };
    };

    // Work exclusively owns native resources. Publication may retain only the
    // result and weak references, so even an accepted but unexecuted UI callback
    // cannot keep a document, a context or its registration alive.
    template<typename Work, typename Publish>
    void ExecuteTrackedWork(BackgroundWorkTracker::Registration registration,
        std::optional<Work>& work, Publish& publish)
    {
        if (!registration)
        {
            work.reset();
            return;
        }
        using Result = std::invoke_result_t<Work&>;
        std::optional<Result> result;
        std::exception_ptr error;
        try
        {
            result.emplace(std::invoke(*work));
        }
        catch (...)
        {
            error = std::current_exception();
        }
        work.reset();
        // The registration remains active through publication and is released
        // on this worker even when publication fails or throws.
        std::invoke(publish, std::move(result), error);
    }
}
