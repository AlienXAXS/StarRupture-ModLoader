#include "pch.h"
#include "game_thread_dispatch.h"

#include <atomic>
#include <mutex>
#include <queue>
#include <variant>

namespace GameThreadDispatch
{
    namespace
    {
        // Learned from Drain(), which the engine-tick hook calls on the game
        // thread every frame. 0 until the first tick.
        std::atomic<DWORD> g_gameThreadId{ 0 };

        // A task is either a void packaged_task or a string packaged_task.
        // We store them in a single queue using std::variant to avoid two
        // separate queues and two mutex lock/unlock cycles per Drain call.
        using VoidTask   = std::packaged_task<void()>;
        using StringTask = std::packaged_task<std::string()>;
        using Task       = std::variant<VoidTask, StringTask>;

        std::mutex       g_mutex;
        std::queue<Task> g_queue;
    } // anonymous namespace

    void PostVoid(std::function<void()> fn)
    {
        VoidTask task(std::move(fn));
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push(std::move(task));
    }

    std::future<std::string> PostString(std::function<std::string()> fn)
    {
        StringTask task(std::move(fn));
        std::future<std::string> fut = task.get_future();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push(std::move(task));
        }
        return fut;
    }

    bool IsGameThread()
    {
        const DWORD id = g_gameThreadId.load(std::memory_order_relaxed);
        return id != 0 && id == GetCurrentThreadId();
    }

    void Drain()
    {
        g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

        // Swap under lock so tasks queued during Drain do not deadlock.
        std::queue<Task> local;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            local.swap(g_queue);
        }

        while (!local.empty())
        {
            Task& t = local.front();
            if (std::holds_alternative<VoidTask>(t))
                std::get<VoidTask>(t)();
            else
                std::get<StringTask>(t)();
            local.pop();
        }
    }
} // namespace GameThreadDispatch
