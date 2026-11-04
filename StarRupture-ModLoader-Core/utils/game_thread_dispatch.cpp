#include "pch.h"
#include "game_thread_dispatch.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <variant>
#include <vector>

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
        struct Task
        {
            std::variant<VoidTask, StringTask> run;
            HMODULE                            owner = nullptr;   // see PostVoid
        };

        // g_queue takes new posts. g_batches points at the batch each Drain in
        // progress is working through (more than one only if a task ticks the
        // engine), so ForgetModule can reach those too. All under g_mutex.
        std::mutex                     g_mutex;
        std::deque<Task>               g_queue;
        std::vector<std::deque<Task>*> g_batches;
    } // anonymous namespace

    void PostVoid(std::function<void()> fn, HMODULE owner)
    {
        Task task{ VoidTask(std::move(fn)), owner };
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push_back(std::move(task));
    }

    void ForgetModule(HMODULE owner)
    {
        if (!owner) return;
        std::deque<Task> dropped;   // destroyed after the lock is released
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<std::deque<Task>*> lists = g_batches;
        lists.push_back(&g_queue);
        for (std::deque<Task>* list : lists)
            for (auto it = list->begin(); it != list->end(); )
            {
                if (it->owner == owner) { dropped.push_back(std::move(*it)); it = list->erase(it); }
                else ++it;
            }
    }

    std::future<std::string> PostString(std::function<std::string()> fn)
    {
        StringTask task(std::move(fn));
        std::future<std::string> fut = task.get_future();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push_back(Task{ std::move(task), nullptr });
        }
        return fut;
    }

    bool IsGameThread()
    {
        const DWORD id = g_gameThreadId.load(std::memory_order_relaxed);
        return id != 0 && id == GetCurrentThreadId();
    }

    bool HasTicked()
    {
        return g_gameThreadId.load(std::memory_order_relaxed) != 0;
    }

    void Drain()
    {
        g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

        // This frame's batch is what was queued when Drain began; tasks posted
        // while it runs wait for the next tick. Each task is taken out under
        // the lock and run without it, so a task can post, or ForgetModule
        // what is left of the batch.
        std::deque<Task> batch;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            batch.swap(g_queue);
            g_batches.push_back(&batch);
        }

        for (;;)
        {
            Task t;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (batch.empty())
                {
                    g_batches.pop_back();
                    break;
                }
                t = std::move(batch.front());
                batch.pop_front();
            }
            if (std::holds_alternative<VoidTask>(t.run))
                std::get<VoidTask>(t.run)();
            else
                std::get<StringTask>(t.run)();
        }
    }
} // namespace GameThreadDispatch
