#include "pch.h"
#include "game_thread_dispatch.h"

#include <mutex>
#include <queue>
#include <variant>

namespace GameThreadDispatch
{
    namespace
    {
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

    void Drain()
    {
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
