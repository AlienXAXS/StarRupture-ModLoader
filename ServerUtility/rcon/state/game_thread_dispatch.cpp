#include "game_thread_dispatch.h"

#include <mutex>
#include <queue>

namespace GameThreadDispatch
{

namespace
{
    std::mutex                                         g_mutex;
    std::queue<std::packaged_task<std::string()>>      g_queue;
} // anonymous namespace

std::future<std::string> Post(std::function<std::string()> fn)
{
    std::packaged_task<std::string()> task(std::move(fn));
    std::future<std::string> fut = task.get_future();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push(std::move(task));
    }

    return fut;
}

void Drain()
{
    // Swap the queue under lock so we don't hold the mutex while executing tasks
    // (tasks may themselves call Post, which would deadlock if we held the lock).
    std::queue<std::packaged_task<std::string()>> local;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        local.swap(g_queue);
    }

    while (!local.empty())
    {
        local.front()();   // execute the task (sets the promise inside the packaged_task)
        local.pop();
    }
}

} // namespace GameThreadDispatch
