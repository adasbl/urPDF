#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace urpdf
{
    // Call only on a worker thread. The synchronous operation must handle MuPDF's
    // longjmp exceptions in its own helper frames, before they reach this lock.
    template<typename Operation>
    bool RunDocumentOperation(std::mutex& mutex, std::atomic<std::uint64_t> const& generation,
        std::uint64_t expectedGeneration, Operation&& operation)
    {
        static_assert(std::is_void_v<std::invoke_result_t<Operation>>,
            "Document operations must be synchronous and return void.");
        std::lock_guard lock(mutex);
        // A queued operation may have become obsolete while waiting for access.
        if (generation.load(std::memory_order_acquire) != expectedGeneration)
        {
            return false;
        }
        operation();
        return true;
    }
}
